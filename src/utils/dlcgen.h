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
//     x64/<bucket>.rpf/<streamed assets at each archive root>
//
// Each directory under stream/ that directly holds assets becomes its own nested
// RPF (sibling under %PLATFORM%), so multiple `_manifest.ymf` files survive.
// Assets are still flattened to that RPF's root: streaming objects are identified
// by path relative to the packfile, so a `ybn/` prefix would never match `bt1_02_0`.
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
		Skipped, // intentionally not published (e.g. map pack isolation)
	};

	// Generates (or reuses) the DLC for one resource and publishes it into the
	// mods tree. `error` is filled only on failure, which is logged by the caller
	// and never fatal.
	BuildResult Build(const fivem::Resource& resource, std::string& error);

	// Removes published packs that no longer correspond to a resource. Called
	// once after every resource has been built.
	void Prune(const std::vector<std::string>& packs);
}
