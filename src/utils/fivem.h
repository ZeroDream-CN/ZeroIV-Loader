#pragma once

#include <filesystem>
#include <string>
#include <vector>

// FiveM resource discovery for the unpack-mods layer.
//
// A directory under unpack-mods/ that holds fxmanifest.lua or __resource.lua is
// treated as a FiveM resource. Its manifest names the metadata files
// (data_file) and the assets that live in stream/, which is all the generator
// needs to rebuild the resource as a retail add-on DLC.
namespace fivem
{
	struct DataFile
	{
		std::string type;   // FiveM data_file type, e.g. "HANDLING_FILE"
		std::string path;   // resource-relative, '/' separated
	};

	struct Resource
	{
		std::filesystem::path dir;
		std::filesystem::path manifest;
		std::string name;                 // directory name as it appears on disk
		std::string pack;                 // dlcpacks/<pack>/ and setup2 name
		std::vector<DataFile> dataFiles;
		std::vector<std::string> assets;  // resource-relative streamed files
		bool isMap = false;

		// One entry per input file that ends up in the generated archive, kept
		// so a later run can tell whether the archive is still current. Size and
		// write time are recorded rather than a content hash: hashing a 150 MB
		// pack on every start would cost more than rebuilding it.
		struct SourceState
		{
			std::string path;             // resource-relative, forward slashes
			uint64_t size = 0;
			uint64_t mtime = 0;           // 0 when the file is missing
		};
		std::vector<SourceState> sources; // sorted by path

		std::string signature;            // change detector for the DLC cache
	};

	// Scans the unpack root for resources. Never throws; a broken manifest is
	// logged and that resource alone is dropped.
	std::vector<Resource> Scan(std::string& error);

	// Parses one manifest; exported so the generator and diagnostics agree on
	// what the manifest said.
	bool ParseManifest(const std::filesystem::path& resourceDir,
		const std::filesystem::path& manifest, Resource& out, std::string& error);

	// Stable, filesystem-safe pack name derived from the directory name.
	std::string MakePackName(const std::filesystem::path& resourceDir);

	// Scans, generates the DLC for every resource and arms the dlclist hook.
	// Called once during loader init, before the engine opens update.rpf.
	void Init();
}
