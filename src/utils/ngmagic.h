#pragma once

#include <cstddef>

// Obfuscated NG table blob (CodeWalker magic.dat). Unlocked at runtime with the
// retail AES key found by SHA-1 in the game module — see ngextract::from_magic.
namespace ngmagic
{
	extern const unsigned char kData[];
	extern const size_t kDataSize;
}
