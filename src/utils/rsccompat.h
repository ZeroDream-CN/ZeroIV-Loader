#pragma once

#include <filesystem>
#include <string>
#include <vector>

// Detects Gen8 (Legacy) vs Gen9 (Enhanced) RSC resources by reading the
// version nibbles out of a plain RSC7 header. Used to warn before the game
// crashes on an unconverted add-on.
namespace rsccompat
{
	enum class Status
	{
		Ok,            // matches the running game edition, or not a known type
		NeedsGen9,     // Legacy asset on Enhanced
		NeedsLegacy,   // Gen9 asset on Legacy
		Unreadable,    // looks like RSC7 but the header could not be parsed
	};

	struct Hit
	{
		std::string pack;   // resource / pack folder name
		std::string file;   // pack-relative path
		int version = -1;
		Status status = Status::Ok;
	};

	// Combined RSC version from system/graphics page flags (CodeWalker).
	int VersionFromFlags(uint32_t systemFlags, uint32_t graphicsFlags);

	// Reads the first 16 bytes of `path`. Returns -1 when the file is not a
	// plain RSC7 resource (or cannot be read).
	int ReadVersion(const std::filesystem::path& path);

	// Classifies one file for the running game. Non-resource files and
	// extensions outside the convertible set report Ok.
	Status CheckFile(const std::filesystem::path& path, bool gameIsEnhanced, int* versionOut = nullptr);

	// Walks FiveM stream/data assets listed on the resource and appends hits.
	void ScanFiveMAssets(const std::string& packName,
		const std::filesystem::path& resourceDir,
		const std::vector<std::string>& relativePaths,
		bool gameIsEnhanced, std::vector<Hit>& out);

	// Walks a loose directory tree (e.g. unpack-mods/dlcpacks/<name>/dlc.rpf/)
	// for convertible resources.
	void ScanTree(const std::string& packName, const std::filesystem::path& root,
		bool gameIsEnhanced, std::vector<Hit>& out);
}
