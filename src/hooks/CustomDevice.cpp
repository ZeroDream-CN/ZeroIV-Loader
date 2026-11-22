#include "main.h"
#include "CustomDevice.h"
#include "DlclistHook.h"
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/*
	Disk folder remains `unpack-mods/`. Private mount prefix is `zivmods:/`
	(exactly 9 characters).

	Critical: the game must never call into our C++ fiDeviceLocal VMT for
	fiPackfile::Open (AV / wrong `this`). Locals are only for ResolveOpen /
	FileReadHooks where *we* call OpenBulk. GetDevice must return game
	fiDeviceRelative VMTs — returning Local caused ERR_STR_MISSING.

	Mount layout (Relative SetPath + Mount):
	  zivmods:/                  -> abs(unpack-mods)
	  platform:/, platformcrc:/  -> zivmods:/platform/
	  common:/, commoncrc:/      -> zivmods:/common/
	  dlcpacks:/                 -> abs(mods/update/x64/dlcpacks)

	OpenArchive must NOT rewrite platform:/ or common:/ onto zivmods:/ (hangs).
	GetDevice prefer forces remount Relatives for:
	  - [overlay] whitelist files that exist under unpack-mods (visuals)
	  - dlcpacks:/ paths that exist under the mods tree (including ziv_*)
	Preferring content metas (gta5.meta, carcols.ymt, …) breaks addon cars;
	those stay on the deny list. setup2 for generated packs also lists
	GROUP_UPDATE_STREAMING (generator v7) so high-order COMPAT packs still apply.

	Unpacked pack directories are synthesized to RPF7 in ZeroIVLoose/ before
	mount. Roots: [paths] mods / unpackMods (default next to the executable).
*/

// Folder on disk remains unpack-mods/.
static std::string g_overlayMount = "zivmods:/";
static_assert(sizeof("zivmods:/") - 1 == 9, "zivmods:/ mount prefix must be 9 chars");
static bool g_overlayMounted = false;

// Game-facing root: stock Relative VMT. Not our Local.
static rage::fiDeviceRelative* g_overlayRelative = nullptr;

// Remounted Relatives for stock names + GetDevice prefer (game VMT only —
// returning our C++ Local from GetDevice caused ERR_STR_MISSING).
static rage::fiDeviceRelative* g_commonRelative = nullptr;
static rage::fiDeviceRelative* g_commonCrcRelative = nullptr;
static rage::fiDeviceRelative* g_platformRelative = nullptr;
static rage::fiDeviceRelative* g_platformCrcRelative = nullptr;
static rage::fiDeviceRelative* g_dlcpacksRelative = nullptr;

// Unmounted Locals for FileReadHooks / ResolveOpen only (we invoke them).
static rage::fiDeviceLocal* g_unpackOverlay = nullptr;
static rage::fiDeviceLocal* g_commonOverlay = nullptr;
static rage::fiDeviceLocal* g_platformOverlay = nullptr;

// The ini is read once; OpenArchiveHook runs for every pack the engine opens.
static bool overlay_enabled()
{
	static const bool enabled = config::get_value<bool>("config", "modsOverride", true);
	return enabled;
}

// [overlay] in ZeroIV.ini — prefer unpack-mods loose files over stock.
// mode=whitelist|blacklist|off; allow/deny are comma/semicolon lists of paths
// relative to common:/ or platform:/. Trailing `/` or `*` = prefix match.
namespace {

enum class OverlayMode { Whitelist, Blacklist, Off };

struct OverlayRules
{
	OverlayMode mode = OverlayMode::Whitelist;
	std::vector<std::string> allow;
	std::vector<std::string> deny;
};

static std::string trim_copy(std::string s)
{
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
		s.erase(s.begin());
	while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
		s.pop_back();
	for (char& c : s)
	{
		if (c == '\\')
			c = '/';
	}
	return s;
}

static void split_patterns(const std::string& raw, std::vector<std::string>& out)
{
	std::string cur;
	auto flush = [&]() {
		std::string t = trim_copy(std::move(cur));
		cur.clear();
		if (!t.empty())
			out.push_back(std::move(t));
	};
	for (char c : raw)
	{
		if (c == ',' || c == ';' || c == '\n' || c == '\r')
			flush();
		else
			cur.push_back(c);
	}
	flush();
}

static bool pattern_match(const char* rest, const std::string& pat)
{
	if (!rest || pat.empty())
		return false;
	if (pat.back() == '/' || pat.back() == '*')
	{
		const size_t n = pat.size() - (pat.back() == '*' ? 1 : 0);
		return n > 0 && _strnicmp(rest, pat.c_str(), n) == 0;
	}
	return _stricmp(rest, pat.c_str()) == 0;
}

static bool any_match(const char* rest, const std::vector<std::string>& pats)
{
	for (const auto& p : pats)
	{
		if (pattern_match(rest, p))
			return true;
	}
	return false;
}

static const OverlayRules& overlay_rules()
{
	static const OverlayRules rules = []() {
		OverlayRules r;
		const std::string mode = config::get_value<std::string>("overlay", "mode", "whitelist");
		if (_stricmp(mode.c_str(), "off") == 0 || _stricmp(mode.c_str(), "disabled") == 0)
			r.mode = OverlayMode::Off;
		else if (_stricmp(mode.c_str(), "blacklist") == 0 || _stricmp(mode.c_str(), "deny") == 0)
			r.mode = OverlayMode::Blacklist;
		else
			r.mode = OverlayMode::Whitelist;

		split_patterns(config::get_value<std::string>("overlay", "allow", ""), r.allow);
		split_patterns(config::get_value<std::string>("overlay", "deny", ""), r.deny);

		const char* modeName =
			r.mode == OverlayMode::Off ? "off" :
			r.mode == OverlayMode::Blacklist ? "blacklist" : "whitelist";
		logger::write("device", "[overlay] mode=%s allow=%zu deny=%zu",
			modeName, r.allow.size(), r.deny.size());
		return r;
	}();
	return rules;
}

} // namespace

static bool overlay_rest_allowed(const char* rest)
{
	if (!rest || !*rest)
		return false;

	const OverlayRules& r = overlay_rules();
	if (r.mode == OverlayMode::Off)
		return false;
	if (any_match(rest, r.deny))
		return false;
	if (r.mode == OverlayMode::Blacklist)
		return true;
	return any_match(rest, r.allow);
}

struct overlayRoot
{
	const char* mount;   // stock mount point, e.g. "dlcpacks:/"
	const char* sub;     // directory under the unpack root
};

// Hand-placed packs in unpack-mods/dlcpacks still need an OpenArchive rewrite.
// platform:/ and common:/ are NOT rewritten — Relative remounts own those names.
static const overlayRoot g_packRoot = { "dlcpacks:/", "dlcpacks" };

static bool resolve_root(const overlayRoot& root, const char* rest, bool generated,
	std::string& out, std::filesystem::path& physical)
{
	std::error_code ec;
	if (generated)
	{
		const std::filesystem::path cached = config::generated_root() / root.sub / rest;
		if (std::filesystem::exists(cached, ec))
		{
			physical = cached;
			out = g_overlayMount + ".cache/" + root.sub + "/" + rest;
			return true;
		}
	}

	physical = config::unpack_root() / root.sub / rest;
	if (!std::filesystem::exists(physical, ec))
		return false;

	out = g_overlayMount + root.sub + "/" + rest;
	return true;
}

static bool resolve_overlay(const char* path, std::string& out, std::filesystem::path& physical)
{
	if (!path || !g_overlayMounted)
		return false;

	const size_t packLen = std::strlen(g_packRoot.mount);
	if (_strnicmp(path, g_packRoot.mount, packLen) == 0)
		return resolve_root(g_packRoot, path + packLen, false, out, physical);

	return false;
}

static const char* kGetDeviceSig = IsEnhanced()
	? "41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 89 d6 49 89 ce"
	: "48 89 5C 24 ? 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8D 15 ? ? ? ? 41 B8";

void(*InitialMountOrig)();
void InitialMountHook()
{
	InitialMountOrig();

	if (!overlay_enabled())
	{
		logger::write("device", "[%s] unpack overlay disabled by config", __FUNCTION__);
		return;
	}

	const std::filesystem::path root = config::unpack_root();

	std::error_code ec;
	if (!std::filesystem::is_directory(root, ec))
	{
		logger::write("device", "[%s] %s is missing, using stock devices",
			__FUNCTION__, root.string().c_str());
		return;
	}

	// Root: Relative + absolute unpack path + Mount("zivmods:/").
	// Game packfile code only tolerates the stock Relative VMT here.
	const std::string absRoot = std::filesystem::absolute(root, ec).lexically_normal().string();
	if (ec)
	{
		logger::write("device", "[%s] cannot absolutize %s: %s",
			__FUNCTION__, root.string().c_str(), ec.message().c_str());
		return;
	}

	g_overlayRelative = new rage::fiDeviceRelative();
	g_overlayRelative->SetPath(absRoot.c_str(), true, nullptr);
	if (!g_overlayRelative->Mount(g_overlayMount.c_str()))
	{
		logger::write("device", "[%s] %s Relative mount failed, using stock devices",
			__FUNCTION__, g_overlayMount.c_str());
		g_overlayRelative = nullptr;
		return;
	}
	g_overlayMounted = true;

	// Unmounted Locals for ResolveOpen / FileReadHooks (direct calls only).
	g_unpackOverlay = new rage::fiDeviceLocal();
	g_unpackOverlay->SetRootPath(absRoot);
	g_unpackOverlay->SetMountPrefix(g_overlayMount);
	g_unpackOverlay->SetPath(absRoot.c_str(), true, nullptr);

	const std::filesystem::path commonRoot = root / "common";
	if (std::filesystem::is_directory(commonRoot, ec))
	{
		g_commonOverlay = new rage::fiDeviceLocal();
		g_commonOverlay->SetRootPath(commonRoot.string());
		g_commonOverlay->SetMountPrefix("common:/");
		g_commonOverlay->SetPath(commonRoot.string().c_str(), true, nullptr);
	}

	const std::filesystem::path platformRoot = root / "platform";
	if (std::filesystem::is_directory(platformRoot, ec))
	{
		g_platformOverlay = new rage::fiDeviceLocal();
		g_platformOverlay->SetRootPath(platformRoot.string());
		g_platformOverlay->SetMountPrefix("platform:/");
		g_platformOverlay->SetPath(platformRoot.string().c_str(), true, nullptr);
	}

	// Relative SetPath(virt) Mount(stock name). Game VMT only.
	// GetDevice prefer (whitelist / existing files) forces overlay when Mount
	// left stock ahead. Never return Local from GetDevice — that ERR_STR_MISSING.
	auto remount = [](const std::string& virt, const char* mountPoint,
		rage::fiDeviceRelative*& slot) {
		auto* rel = new rage::fiDeviceRelative();
		rel->SetPath(virt.c_str(), true, nullptr);
		const bool ok = rel->Mount(mountPoint);
		logger::write("device", "[%s] Relative %s -> %s = %d",
			__FUNCTION__, virt.c_str(), mountPoint, ok ? 1 : 0);
		if (!ok)
		{
			delete rel;
			slot = nullptr;
			return;
		}
		slot = rel;
	};

	if (g_platformOverlay)
	{
		remount(g_overlayMount + "platform/", "platform:/", g_platformRelative);
		remount(g_overlayMount + "platform/", "platformcrc:/", g_platformCrcRelative);
	}
	if (g_commonOverlay)
	{
		remount(g_overlayMount + "common/", "common:/", g_commonRelative);
		remount(g_overlayMount + "common/", "commoncrc:/", g_commonCrcRelative);
	}

	// Point dlcpacks:/ at the published mods tree (addon packs + ziv_*). After
	// common remount, COMPAT packs with setup2 order=100 stopped being probed
	// through the stock update/x64 path; a Relative onto the mods dlcpacks
	// folder makes dlcpacks:/<pack>/dlc.rpf resolve from mods/ directly.
	{
		const std::filesystem::path dlcRoot =
			config::mods_root() / "update" / "x64" / "dlcpacks";
		if (std::filesystem::is_directory(dlcRoot, ec))
		{
			const std::string absDlc = std::filesystem::absolute(dlcRoot, ec).lexically_normal().string();
			if (!ec)
			{
				auto* rel = new rage::fiDeviceRelative();
				rel->SetPath(absDlc.c_str(), true, nullptr);
				const bool ok = rel->Mount("dlcpacks:/");
				logger::write("device", "[%s] Relative %s -> dlcpacks:/ = %d",
					__FUNCTION__, absDlc.c_str(), ok ? 1 : 0);
				if (ok)
					g_dlcpacksRelative = rel;
				else
					delete rel;
			}
		}
	}

	logger::write("device", "[%s] all devices mounted~ %s <- %s (common=%d platform=%d dlcpacks=%d)",
		__FUNCTION__, g_overlayMount.c_str(), absRoot.c_str(),
		g_commonRelative ? 1 : 0, g_platformRelative ? 1 : 0,
		g_dlcpacksRelative ? 1 : 0);
}

namespace unpackdev
{
	bool Ready()
	{
		return g_overlayMounted;
	}

	bool ResolveOpen(const char* path, rage::fiDevice*& device, std::string& openPath)
	{
		device = nullptr;
		openPath.clear();
		if (!path || !*path || !g_overlayMounted)
			return false;

		auto take = [&](rage::fiDeviceLocal* dev, const std::filesystem::path& root,
			const char* mount, const char* rest) -> bool
		{
			// Never claim the bare mount (`common:/`) or a directory. Doing so
			// makes the engine treat a sparse unpack dump as the whole common
			// tree — stock GXT/text disappear and the game dies with
			// ERR_STR_MISSING. Only exact files that exist on disk are ours.
			if (!dev || !rest || !*rest)
				return false;
			if (!overlay_rest_allowed(rest))
				return false;
			std::error_code ec;
			const std::filesystem::path physical = root / rest;
			if (!std::filesystem::is_regular_file(physical, ec))
				return false;
			device = dev;
			openPath = std::string(mount) + rest;
			return true;
		};

		const std::filesystem::path unpack = config::unpack_root();
		if (_strnicmp(path, "common:/", 8) == 0)
			return take(g_commonOverlay, unpack / "common", "common:/", path + 8);
		if (_strnicmp(path, "platform:/", 10) == 0)
			return take(g_platformOverlay, unpack / "platform", "platform:/", path + 10);
		if (_strnicmp(path, "common/", 7) == 0)
			return take(g_commonOverlay, unpack / "common", "common:/", path + 7);
		if (_strnicmp(path, "platform/", 9) == 0)
			return take(g_platformOverlay, unpack / "platform", "platform:/", path + 9);

		// update/x64/... → unpack-mods tree (generated cache first). Same
		// rule: only regular files, never a directory prefix.
		if (_strnicmp(path, "update/x64/", 11) == 0 && g_unpackOverlay)
		{
			const char* rest = path + 11;
			if (!rest || !*rest)
				return false;
			std::error_code ec;
			const std::filesystem::path cached = config::generated_root() / rest;
			const std::filesystem::path plain = unpack / rest;
			if (std::filesystem::is_regular_file(cached, ec) ||
				std::filesystem::is_regular_file(plain, ec))
			{
				device = g_unpackOverlay;
				openPath = g_overlayMount + rest;
				return true;
			}
		}

		return false;
	}
}

static rage::fiDevice* (*GetDeviceOrig)(const char* path, bool allowRoot);

// Prefer remount Relatives when the loose/mod file exists. Game Relative VMT
// only — never our Local (ERR_STR_MISSING). Whitelist limits common/platform.
static rage::fiDevice* GetDeviceDetour(const char* path, bool allowRoot)
{
	auto preferVisual = [&](rage::fiDeviceRelative* rel, const char* prefix, size_t prefixLen,
		const std::filesystem::path& root) -> rage::fiDevice*
	{
		if (!rel || !path || _strnicmp(path, prefix, prefixLen) != 0)
			return nullptr;
		const char* rest = path + prefixLen;
		if (!rest || !*rest)
			return nullptr;
		if (!overlay_rest_allowed(rest))
			return nullptr;
		std::error_code ec;
		if (!std::filesystem::is_regular_file(root / rest, ec))
			return nullptr;
		logger::write("device", "[%s] prefer overlay %s", __FUNCTION__, path);
		return reinterpret_cast<rage::fiDevice*>(rel);
	};

	auto preferDlc = [&]() -> rage::fiDevice*
	{
		if (!g_dlcpacksRelative || !path || _strnicmp(path, "dlcpacks:/", 10) != 0)
			return nullptr;
		const char* rest = path + 10;
		if (!rest || !*rest)
			return nullptr;
		std::error_code ec;
		const std::filesystem::path physical =
			config::mods_root() / "update" / "x64" / "dlcpacks" / rest;
		if (!std::filesystem::exists(physical, ec))
			return nullptr;
		logger::write("device", "[%s] prefer dlcpacks %s", __FUNCTION__, path);
		return reinterpret_cast<rage::fiDevice*>(g_dlcpacksRelative);
	};

	const std::filesystem::path unpack = config::unpack_root();
	if (auto* d = preferVisual(g_commonRelative, "common:/", 8, unpack / "common"))
		return d;
	if (auto* d = preferVisual(g_commonCrcRelative, "commoncrc:/", 11, unpack / "common"))
		return d;
	if (auto* d = preferVisual(g_platformRelative, "platform:/", 10, unpack / "platform"))
		return d;
	if (auto* d = preferVisual(g_platformCrcRelative, "platformcrc:/", 13, unpack / "platform"))
		return d;
	if (auto* d = preferDlc())
		return d;

	return GetDeviceOrig(path, allowRoot);
}

char (*OpenArchiveOrig)(rage::fiPackfile* a1, const char* path, char smth, int32_t type, __int64 a5);

// Loose packs are built into an archive once per change and reused after that.
// The engine asks for the same pack more than once per session, and a rebuild
// copies the whole tree, so the result is remembered here as well.
static std::mutex g_looseMutex;
static std::unordered_map<std::string, std::string> g_looseBuilt;

// Returns the archive path that stands in for a loose directory, or an empty
// string when it cannot be built.
static std::string synthesize_loose(const std::filesystem::path& dir)
{
	const std::string key = dir.lexically_normal().string();

	{
		std::lock_guard<std::mutex> lock(g_looseMutex);
		auto it = g_looseBuilt.find(key);
		if (it != g_looseBuilt.end())
			return it->second;
	}

	std::string error;
	std::string built = loosepack::Prepare(dir, error);
	if (built.empty())
	{
		logger::write("pack", "[%s] could not build %s: %s", __FUNCTION__, key.c_str(), error.c_str());
		return {};
	}

	{
		std::lock_guard<std::mutex> lock(g_looseMutex);
		g_looseBuilt[key] = built;
	}
	return built;
}

char OpenArchiveHook(rage::fiPackfile* a1, const char* path, char smth, int32_t type, __int64 a5)
{
	std::string overlaid;
	std::filesystem::path overlayFile;
	std::string built;
	const char* usePath = path;

	if (path && overlay_enabled() && resolve_overlay(path, overlaid, overlayFile))
	{
		usePath = overlaid.c_str();

		std::error_code ec;
		if (std::filesystem::is_directory(overlayFile, ec))
		{
			// Enhanced cannot mount a directory, so the unpacked tree is turned
			// into a real archive first and that archive is what gets mounted.
			built = synthesize_loose(overlayFile);
			if (built.empty())
			{
				logger::write("pack", "[%s] %s is unpacked but no archive could be built",
					__FUNCTION__, path);
				return OpenArchiveOrig(a1, path, smth, type, a5);
			}
			usePath = built.c_str();
			logger::write("pack", "[%s] %s is unpacked: mounting %s", __FUNCTION__,
				path, usePath);
		}
		else
		{
			// Virtual prefix on the Relative root (game VMT). Absolute paths
			// hang; our C++ Local VMT AVs inside fiPackfile. `overlaid` is
			// g_overlayMount + sub + rest from resolve_overlay.
			usePath = overlaid.c_str();
			logger::write("device", "[%s] Overlay %s -> %s", __FUNCTION__, path, usePath);
		}
	}
	else if (usePath)
	{
		// A directory reached without the overlay is an unpacked pack sitting
		// either in the game tree itself or in mods/ under its stock path. Both
		// are turned into an archive the same way.
		std::error_code modsEc;
		const std::filesystem::path modsPack = config::mods_root() / usePath;
		bool unpacked = std::filesystem::is_directory(modsPack, modsEc);
		std::filesystem::path unpackedPath = modsPack;

		if (!unpacked && GetDeviceOrig)
		{
			if (auto device = GetDeviceOrig(usePath, true))
			{
				const uint32_t attributes = device->GetAttributes(usePath);
				unpacked = attributes != INVALID_FILE_ATTRIBUTES &&
					(attributes & FILE_ATTRIBUTE_DIRECTORY);
				unpackedPath = std::filesystem::current_path() / usePath;
			}
		}

		if (unpacked)
		{
			built = synthesize_loose(unpackedPath);
			if (!built.empty())
			{
				usePath = built.c_str();
				logger::write("pack", "[%s] %s is unpacked: mounting %s", __FUNCTION__,
					path, usePath);
			}
		}
	}

	// A mod archive is only useful once the game mounts it. Records are cheap
	// and rare, and this is the one line that tells a mounted pack apart from a
	// pack that was redirected to disk but never parsed: without it there is no
	// way to tell whether a car failed to appear because the engine rejected it
	// or because it never read it at all.
	const char result = OpenArchiveOrig(a1, usePath, smth, type, a5);

	// An add-on pack is always worth a line: when a car never appears, this is
	// what tells "the engine mounted the archive" apart from "the loader never
	// served it". The per-file device traces are off by default, so this cannot
	// ride on them.
	const bool addonPack = usePath && strstr(usePath, "dlcpacks/");
	if (!built.empty())
		logger::write("pack", "[%s] mounted %s -> %d", __FUNCTION__, usePath, (int)result);
	else if (addonPack)
	{
		// The attempt count for this exact path is what separates a genuine
		// failure from the engine re-opening a pack it already holds: a second
		// attempt that fails is only meaningful if the first one did not mount.
		static std::mutex attemptMutex;
		static std::unordered_map<std::string, int> attempts;
		int attempt = 0;
		{
			std::lock_guard<std::mutex> lock(attemptMutex);
			attempt = ++attempts[std::string(usePath)];
		}
		logger::write("pack", "[%s] Opening add-on archive %s %d %d -> %d%s (try %d, tid %lu)",
			__FUNCTION__, usePath, (int)smth, (int)type, (int)result,
			result == 0 ? " (REJECTED)" : "", attempt,
			static_cast<unsigned long>(::GetCurrentThreadId()));
	}
	else
		logger::log("device", "[%s] Opening archive %s %d %d -> %d", __FUNCTION__, usePath,
			(int)smth, (int)type, (int)result);
	return result;
}

static memory::InitFuncs CustomDevice([] {
	if (IsEnhanced()) {
		auto mem = memory::scan("e8 ? ? ? ? 48 8d 05 ? ? ? ? 66 c7 44 58 02 00 00 66 89 3d").add(26);
		InitialMountOrig = mem.add(1).rip().as<decltype(InitialMountOrig)>();
		mem.set_call(InitialMountHook);

		memory::scan("41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 45 89 cd 45 89 c6 49 89 d7 48 89 ce 8b 05")
			.hook(OpenArchiveHook, &OpenArchiveOrig);

		// Per-file overlay preference (existing loose files only).
		memory::scan(kGetDeviceSig).hook(GetDeviceDetour, &GetDeviceOrig);
	}
	else {

		auto mem = memory::scan("0F B7 05 ? ? ? ? 48 03 C3 44 88 34 38").add(21);
		InitialMountOrig = mem.add(1).rip().as<decltype(InitialMountOrig)>();
		mem.set_call(InitialMountHook);

		memory::scan("48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC ? ? ? ? 41 8B F9 4C 8B E2 48 8B D9 4C 8B CA 48 8D 05")
			.hook(OpenArchiveHook, &OpenArchiveOrig);

		memory::scan(kGetDeviceSig).hook(GetDeviceDetour, &GetDeviceOrig);
	}
});
