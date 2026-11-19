#pragma once

#include <filesystem>
#include <string>

// RPF7 synthesis for unpacked ("loose") packs.
//
// GTA V Enhanced cannot mount a directory as an archive: fiPackfile::Open
// rejects every (flags, type) pair the Legacy loader used, so the ClosedIV
// `type = 2` directory support has no counterpart here. A loose pack therefore
// has to be presented to the engine as a real .rpf, which this builds from the
// directory tree on demand.
namespace loosepack
{
	// Builds (or reuses) an archive for `dir` and returns the path to pass to
	// the engine, relative to the working directory so the game's root device
	// resolves it. Returns an empty string on failure and fills `error`.
	std::string Prepare(const std::filesystem::path& dir, std::string& error);

	// Builds `dir` into `outFile` (created parent directories included) and
	// leaves no temporary file behind on success. Used by the FiveM generator,
	// which writes straight into unpack-mods/dlcpacks/<pack>/dlc.rpf instead of
	// the ZeroIVLoose cache.
	bool BuildTo(const std::filesystem::path& dir, const std::filesystem::path& outFile,
		std::string& error);
}
