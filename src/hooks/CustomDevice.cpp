#include "main.h"
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

/*
	unpack-mods/ is an additive overlay. Historically a device was mounted
	straight over `dlcpacks:/`, `common:/` and `platform:/`, which on GTA V
	Enhanced hides the stock device for that mount point: every retail DLC then
	resolves against the overlay and fails, and the game dies on the loading
	screen without a message.

	Instead, one device is mounted at a private prefix (`unpack-mods:/`) and
	OpenArchiveHook rewrites a path to it only when the modded file actually
	exists. Everything that is not overridden keeps using its stock device, so
	the retail packs still load from update/x64/dlcpacks.

	An unpacked pack (a directory where a .rpf is expected) is accepted at either

		unpack-mods/dlcpacks/<pack>/dlc.rpf/
		mods/update/x64/dlcpacks/<pack>/dlc.rpf/

	but Enhanced cannot mount a directory at all: fiPackfile::Open rejects every
	(flags, type) pair and reports the directory as a zero-length archive. Such a
	pack is therefore turned into a real RPF7 in ZeroIVLoose/ at load time and
	that archive is what gets mounted.

	Both roots are configurable ([paths] mods / unpackMods) and default to
	folders next to the executable.
*/

static std::string g_overlayMount = "unpack-mods:/";
static bool g_overlayMounted = false;

// The ini is read once; OpenArchiveHook runs for every pack the engine opens.
static bool overlay_enabled()
{
	static const bool enabled = config::get_value<bool>("config", "modsOverride", true);
	return enabled;
}

struct overlayRoot
{
	const char* mount;   // stock mount point, e.g. "dlcpacks:/"
	const char* sub;     // directory under the unpack root
};

// Only the mount points that carry whole archives are worth overlaying: a .meta
// inside common.rpf is read through the packfile, not through a device path.
static const overlayRoot g_overlayRoots[] = {
	{ "dlcpacks:/", "dlcpacks" },
	{ "platform:/", "platform" },
	{ "common:/", "common" },
};

// Finds the overlay counterpart of a stock path. Returns true and writes the
// rewritten path (unpack-mods:<sub>/<rest>) plus the physical path only when
// file exists on disk, so a missing override never turns into a lookup that
// fails where stock would have answered.
static bool resolve_overlay(const char* path, std::string& out, std::filesystem::path& physical)
{
	if (!path || !g_overlayMounted)
		return false;

	for (const auto& root : g_overlayRoots)
	{
		const size_t prefixLen = std::strlen(root.mount);
		if (_strnicmp(path, root.mount, prefixLen) != 0)
			continue;

		const char* rest = path + prefixLen;
		physical = config::unpack_root() / root.sub / rest;

		std::error_code ec;
		if (!std::filesystem::exists(physical, ec))
			return false;

		out = g_overlayMount + root.sub + "/" + rest;
		return true;
	}
	return false;
}

static memory::func<rage::fiDevice*, const char*, bool> GetDeviceHook(IsEnhanced() ? "41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 89 d6 49 89 ce" :
	"48 89 5C 24 ? 88 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8D 15 ? ? ? ? 41 B8");

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

	// One device for the whole tree, mounted at a prefix no stock code uses. The
	// stock mount points are deliberately left alone: shadowing `dlcpacks:/` is
	// what made Enhanced crash before the first loading screen.
	auto* overlay = new rage::fiDeviceLocal();
	overlay->SetRootPath(root.string());
	overlay->SetMountPrefix(g_overlayMount);
	overlay->SetPath(root.string().c_str(), true, nullptr);

	if (!overlay->Mount(g_overlayMount.c_str()))
	{
		logger::write("device", "[%s] %s could not be mounted, using stock devices",
			__FUNCTION__, g_overlayMount.c_str());
		return;
	}
	g_overlayMounted = true;

	logger::write("device", "[%s] %s mounted from %s (additive overlay)",
		__FUNCTION__, g_overlayMount.c_str(), root.string().c_str());
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

		if (!unpacked)
		{
			if (auto device = GetDeviceHook(usePath, true))
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
		logger::write("pack", "[%s] Opening add-on archive %s %d %d -> %d%s", __FUNCTION__,
			usePath, (int)smth, (int)type, (int)result,
			result == 0 ? " (REJECTED)" : "");
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
	}
	else {

		auto mem = memory::scan("0F B7 05 ? ? ? ? 48 03 C3 44 88 34 38").add(21);
		InitialMountOrig = mem.add(1).rip().as<decltype(InitialMountOrig)>();
		mem.set_call(InitialMountHook);

		memory::scan("48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 98 48 81 EC ? ? ? ? 41 8B F9 4C 8B E2 48 8B D9 4C 8B CA 48 8D 05")
			.hook(OpenArchiveHook, &OpenArchiveOrig);
	}
});
