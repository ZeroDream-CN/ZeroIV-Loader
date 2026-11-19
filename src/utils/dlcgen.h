#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "fivem.h"

// Rebuilds a FiveM resource as a retail add-on DLC package.
//
// Layout:
//
//   unpack-mods/.cache/dlcpacks/<pack>/dlc.rpf    real archive, hidden cache
//   mods/update/x64/dlcpacks/<pack>/dlc.rpf       hard link, what the engine mounts
//     setup2.xml
//     content.xml
//     data/<data_file basenames>
//     x64/stream.rpf/<streamed assets at the root>
//
// The streaming objects must sit at the archive root: an object is identified by
// its path relative to the packfile, so a `stream/` or `vehicles/` prefix would
// register the model under that name and the game would never find it.
//
// The archive is produced by loosepack, which already knows how to turn a
// directory tree into an RPF7 and how to nest a directory named *.rpf.
namespace dlcgen
{
	enum class BuildResult
	{
		Built,
		Reused,
		Failed,
	};

	// Generates (or reuses) the DLC for one resource and publishes it into the
	// mods tree. `error` is filled only on failure, which is logged by the caller
	// and never fatal.
	BuildResult Build(const fivem::Resource& resource, std::string& error);

	// Removes published packs that no longer correspond to a resource. Called
	// once after every resource has been built.
	void Prune(const std::vector<std::string>& packs);
}
