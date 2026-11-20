#pragma once

#include <cstdint>
#include <cstddef>

// SHA-1 digests of the retail NG key material (CodeWalker GTA5KeyHashes).
// Digests only — the key bytes themselves are never shipped.
namespace nghashes
{
	constexpr size_t kHashBytes = 20;
	constexpr size_t kAesKeyBytes = 32;
	constexpr size_t kKeyCount = 101;
	constexpr size_t kKeyBytes = 272;
	constexpr size_t kTableCount = 17 * 16; // 272 rows
	constexpr size_t kTableRowBytes = 1024; // 256 uint32
	constexpr size_t kLutBytes = 256;

	extern const uint8_t kAesKeyHash[kHashBytes];
	extern const uint8_t kLutHash[kHashBytes];
	extern const uint8_t kKeyHashes[kKeyCount][kHashBytes];
	extern const uint8_t kTableHashes[kTableCount][kHashBytes];
}
