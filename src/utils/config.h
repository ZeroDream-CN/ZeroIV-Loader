#pragma once
#include <cstring>
#include <filesystem>
#include <string>

#include "inicpp.h"

class config
{
public:
	static ini::IniFile& get_ini()
	{
		static ini::IniFile iniFile;
		return iniFile;
	}

	static void load()
	{
		constexpr const char* kIniPath = "ZeroIV.ini";
		constexpr const char* kLegacyIniPath = "RageOpenV.ini";

		// Keep settings from installs that predate the ZeroIV-Loader rename.
		if (!std::filesystem::exists(kIniPath) && std::filesystem::exists(kLegacyIniPath))
		{
			std::error_code ec;
			std::filesystem::rename(kLegacyIniPath, kIniPath, ec);
			if (ec)
			{
				ec.clear();
				std::filesystem::copy_file(kLegacyIniPath, kIniPath, ec);
			}
		}

		if (std::filesystem::exists(kIniPath))
		{
			// The parser only knows '#' by default, while every ini written for
			// this project (and every hand-edited one) uses ';'. Accepting both
			// keeps a commented file readable instead of rejecting it whole.
			get_ini().setCommentPrefixes({ "#", ";" });

			try
			{
				get_ini().load(kIniPath);
			}
			catch (const std::exception& e)
			{
				// A hand-edited file with a syntax error must not take the game
				// down. Keep the original next to it so nothing typed by hand is
				// lost when the defaults are written below.
				load_error() = e.what();

				std::error_code ec;
				std::filesystem::copy_file(kIniPath, "ZeroIV.ini.broken",
					std::filesystem::copy_options::overwrite_existing, ec);

				get_ini().clear();
			}
			catch (...)
			{
				load_error() = "unknown parser error";
				get_ini().clear();
			}
		}

		// Re-add keys missing from an older or hand-edited file so upgrades
		// never leave the loader querying an empty field.
		bool added = false;
		add_default<bool>("config", "console", false, added);
		add_default<bool>("config", "modsOverride", true, added);
		// Kept for compatibility. OPEN archives are accepted without an
		// in-memory NG stamp once plainifyRpf has rewritten payloads.
		add_default<bool>("config", "allowUnencryptedRpf", true, added);
		add_default<bool>("config", "dumpImage", false, added);
		// Patches the Enhanced D3D12 CopyDescriptors call site so a null
		// descriptor from a Gen8->Gen9 binder can be diagnosed. On some native
		// Windows + NVIDIA setups the stub itself has been seen to take the
		// process down during Streamline init; set false to rule that out.
		add_default<bool>("config", "bindDescriptors", true, added);
		add_default<bool>("fivem", "enabled", true, added);
		add_default<bool>("fivem", "patchStockUpdateRpf", false, added);
		// On: decrypt every flag-1 payload and write an OPEN TOC so update.rpf
		// is genuinely plaintext. First start that converts a stock archive is
		// slow; later starts are a no-op.
		add_default<bool>("fivem", "plainifyRpf", true, added);
		// Raise pool ceilings in update.rpf's gameconfig.xml. "off" / "false"
		// leaves the archive alone. A file at unpack-mods/gameconfig.xml always
		// wins: it is copied in and the automatic raises are skipped.
		add_default<std::string>("fivem", "patchGameConfig", "true", added);
		// Register traditional (non-FiveM) dlcpacks folders into dlclist.xml.
		// FiveM resources under unpack-mods/ are always scanned regardless.
		add_default<bool>("fivem", "registerAddonPacks", true, added);
		// Warn (MessageBox + log) when stream assets are the wrong generation
		// for this build (Gen8 on Enhanced / Gen9 on Legacy).
		add_default<bool>("fivem", "warnIncompatibleResources", true, added);
		add_default<bool>("log", "info", false, added);
		add_default<bool>("log", "mods", false, added);
		add_default<bool>("log", "rpf", false, added);
		add_default<bool>("log", "device", false, added);
		add_default<bool>("debugConsole", "enabled", true, added);
		add_default<std::string>("debugConsole", "host", "127.0.0.1", added);
		add_default<int>("debugConsole", "port", 27183, added);
		add_default<std::string>("paths", "mods", "", added);
		add_default<std::string>("paths", "unpackMods", "", added);
		add_default<std::string>("ui", "language", "auto", added);
		// Prefer unpack-mods common/platform files over stock. whitelist = only
		// `allow` paths; blacklist = everything except `deny`; off = never.
		// Patterns are relative to common:/ or platform:/ (no mount prefix).
		// Trailing `/` or `*` = prefix match; otherwise exact. deny always wins.
		add_default<std::string>("overlay", "mode", "whitelist", added);
		add_default<std::string>("overlay", "allow",
			"data/visualsettings.dat,"
			"data/clouds.xml,"
			"data/cloudkeyframes.xml,"
			"data/CloudKeyframes.xml,"
			"data/watertune.xml,"
			"data/lensflare_f.xml,"
			"data/lensflare_m.xml,"
			"data/lensflare_t.xml,"
			"data/levels/gta5/weather.xml,"
			"data/levels/gta5/time.xml,"
			"data/levels/gta5/water.xml,"
			"levels/gta5/zonebind.ymt,"
			"data/metadata/explosion.ymt,"
			"data/timecycle/,"
			"data/levels/gta5/weather/,"
			"data/effects/,"
			"textures/,"
			"levels/gta5/cloudhats/",
			added);
		add_default<std::string>("overlay", "deny",
			"data/gameconfig.xml,"
			"data/dlclist.xml,"
			"data/levels/gta5/gta5.meta,"
			"data/gtxd.meta,"
			"data/ai/weapons.meta,"
			"data/materials/procedural.meta,"
			"data/carcols.ymt",
			added);
		if (added)
			save();

		cache_switches();
	}

	static void save()
	{
		get_ini().save("ZeroIV.ini");
	}

	template<typename T>
	static T get_config(const char* key)
	{
		return get_value<T>("config", key, T{});
	}

	static bool get_log(const char* key)
	{
		return log_enabled(key);
	}

	// Reason the ini could not be parsed, empty when it loaded. The logger is
	// not initialised yet while load() runs, so main.cpp reports this once the
	// log is open.
	static const std::string& error()
	{
		return load_error();
	}

	// Storage root of one mod layer.
	//
	// An empty setting keeps the historical layout: the folder sits next to the
	// executable, which is the working directory the game was started from. A
	// relative value is resolved against that same directory, so a config can
	// stay portable across machines. The result is canonical, which keeps the
	// per-file redirection caches stable and lets a configured path live on
	// another drive entirely.
	static const std::filesystem::path& mods_root()
	{
		static const std::filesystem::path root = layer_root("mods", "mods");
		return root;
	}

	static const std::filesystem::path& unpack_root()
	{
		static const std::filesystem::path root = layer_root("unpackMods", "unpack-mods");
		return root;
	}

	// Where generated artifacts (archives built from unpacked packs) go. Not
	// configurable on purpose: the engine reaches them through the game root
	// device, so they have to stay beside the executable.
	static const std::filesystem::path& cache_root()
	{
		static const std::filesystem::path root = std::filesystem::current_path() / "ZeroIVLoose";
		return root;
	}

	// Hidden layer for archives the loader generates itself (FiveM resources).
	// It mirrors the unpack tree one level down, so `dlcpacks:/<pack>/dlc.rpf`
	// resolves to `.cache/dlcpacks/<pack>/dlc.rpf`. Keeping it dot-named and
	// hidden leaves unpack-mods/ showing only content the user placed there,
	// which matters because that folder doubles as the FiveM resource list.
	static const std::filesystem::path& generated_root()
	{
		static const std::filesystem::path root = unpack_root() / ".cache";
		return root;
	}

	// Name to hand to a game device for `<root>/<rest>`. A path under the
	// working directory is passed relative, which is the form the stock
	// fiDeviceLocal has always been fed; anything else has to be absolute.
	static std::string device_path(const std::filesystem::path& root, const std::string& rest)
	{
		const std::filesystem::path target = root / rest;

		std::error_code ec;
		const std::filesystem::path rel = std::filesystem::relative(target, std::filesystem::current_path(), ec);
		if (!ec && !rel.empty())
		{
			const std::string text = rel.generic_string();
			if (text != ".." && text.rfind("../", 0) != 0)
				return text;
		}
		return target.generic_string();
	}

	// Hot-path switches, cached at load time. logger::log runs per file
	// operation, so it must not build a std::string key and walk the ini map on
	// every call.
	static bool log_enabled(const char* key)
	{
		if (key == nullptr)
			return false;
		const Switches& s = switches();
		if (std::strcmp(key, "info") == 0)
			return s.info;
		if (std::strcmp(key, "mods") == 0)
			return s.mods;
		if (std::strcmp(key, "rpf") == 0)
			return s.rpf;
		if (std::strcmp(key, "device") == 0)
			return s.device;
		return false;
	}

	// Reads one value with a fallback, so a missing section/key or a malformed
	// value never throws inside the game process.
	template<typename T>
	static T get_value(const char* section, const char* key, T fallback)
	{
		auto& iniFile = get_ini();
		auto sec = iniFile.find(section);
		if (sec == iniFile.end())
			return fallback;
		auto field = sec->second.find(key);
		if (field == sec->second.end())
			return fallback;
		try
		{
			return field->second.template as<T>();
		}
		catch (...)
		{
			return fallback;
		}
	}

private:
	static std::string& load_error()
	{
		static std::string message;
		return message;
	}

	// Resolves one `[paths]` entry. The configured value may be absolute, or
	// relative to the working directory; empty falls back to `fallback` in the
	// working directory.
	static std::filesystem::path layer_root(const char* key, const char* fallback)
	{
		std::string configured = get_value<std::string>("paths", key, "");
		if (configured.empty())
			configured = fallback;

		std::filesystem::path root(configured);

		// A path written the way the user sees it in their file manager starts
		// with a slash. Wine resolves such a path against the Unix filesystem,
		// but the drive-letter form is what the rest of the loader (and the
		// engine's own file devices) handle consistently, and Proton always maps
		// Z: to /. The literal form is kept when that mapping does not exist.
		if (configured[0] == '/' || configured[0] == '\\')
		{
			const std::filesystem::path viaRoot("Z:" + configured);
			if (is_directory(viaRoot))
				return normalize(viaRoot);
		}

		if (root.is_relative())
			return normalize(std::filesystem::current_path() / root);

		return normalize(root);
	}

	static bool is_directory(const std::filesystem::path& path)
	{
		std::error_code ec;
		return std::filesystem::is_directory(path, ec);
	}

	static std::filesystem::path normalize(const std::filesystem::path& path)
	{
		std::error_code ec;
		const std::filesystem::path canon = std::filesystem::weakly_canonical(path, ec);
		if (!ec)
			return canon;
		return std::filesystem::absolute(path, ec);
	}

	struct Switches
	{
		bool info = false;
		bool mods = false;
		bool rpf = false;
		bool device = false;
	};

	static Switches& switches()
	{
		static Switches s;
		return s;
	}

	// Called once at the end of load(), after the ini is fully read.
	static void cache_switches()
	{
		Switches& s = switches();
		s.info = get_value<bool>("log", "info", false);
		s.mods = get_value<bool>("log", "mods", false);
		s.rpf = get_value<bool>("log", "rpf", false);
		s.device = get_value<bool>("log", "device", false);
	}

	template<typename T>
	static void add_default(const char* section, const char* key, const T& value, bool& added)
	{
		auto& iniFile = get_ini();
		auto sec = iniFile.find(section);
		if (sec == iniFile.end() || sec->second.find(key) == sec->second.end())
		{
			iniFile[section][key] = value;
			added = true;
		}
	}
};
