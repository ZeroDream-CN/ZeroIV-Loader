#pragma once

#include <string>

// Shared view of the mod layers.
//
// The redirection itself lives in FileReadHooks.cpp, but the dlclist patcher
// needs the same answer: which physical file does the engine end up reading for
// a name it asked the local device for (`update/update.rpf` may live under
// mods/). Keeping one implementation means the two cannot disagree.
namespace fileread
{
	// Resolves `path` through mods/ then unpack-mods/. Returns false when
	// neither layer overrides the name, in which case the caller should use the
	// path as given.
	bool ResolvePhysical(const char* path, std::string& out);
}
