#pragma once

#include "gtav/rage/fiDevice.h"
#include <string>

// Additive unpack-mods overlay: Relative `zivmods:/` + stock-name remounts,
// GetDevice prefer ([overlay] ini), ResolveOpen for FileReadHooks.
namespace unpackdev
{
	bool Ready();

	// Exact overlay files only (whitelist); never bare mounts or directories.
	bool ResolveOpen(const char* path, rage::fiDevice*& device, std::string& openPath);
}
