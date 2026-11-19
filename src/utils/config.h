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
			try
			{
				get_ini().load(kIniPath);
			}
			catch (...)
			{
				// A hand-edited file with a syntax error must not take the game
				// down; fall back to defaults and rewrite it below.
				get_ini().clear();
			}
		}

		// Re-add keys missing from an older or hand-edited file so upgrades
		// never leave the loader querying an empty field.
		bool added = false;
		add_default<bool>("config", "console", false, added);
		add_default<bool>("config", "modsOverride", true, added);
		add_default<bool>("config", "allowUnencryptedRpf", true, added);
		add_default<bool>("config", "dumpImage", false, added);
		add_default<bool>("log", "info", false, added);
		add_default<bool>("log", "mods", false, added);
		add_default<bool>("log", "rpf", false, added);
		add_default<bool>("log", "device", false, added);
		add_default<bool>("debugConsole", "enabled", true, added);
		add_default<std::string>("debugConsole", "host", "127.0.0.1", added);
		add_default<int>("debugConsole", "port", 27183, added);
		add_default<std::string>("paths", "mods", "", added);
		add_default<std::string>("paths", "unpackMods", "", added);
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
	// Resolves one `[paths]` entry. The configured value may be absolute, or
	// relative to the working directory; empty falls back to `fallback` in the
	// working directory.
	static std::filesystem::path layer_root(const char* key, const char* fallback)
	{
		std::string configured = get_value<std::string>("paths", key, "");
		if (configured.empty())
			configured = fallback;

		std::filesystem::path root(configured);
		if (root.is_relative())
			root = std::filesystem::current_path() / root;

		std::error_code ec;
		const std::filesystem::path canon = std::filesystem::weakly_canonical(root, ec);
		if (!ec)
			return canon;
		return std::filesystem::absolute(root, ec);
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
