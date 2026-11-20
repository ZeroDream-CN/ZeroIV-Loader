#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Locates the NG LUT / keys / decrypt tables in the live game module by matching
// the SHA-1 digests in nghashes, then packs them into the flat blob ngcrypt expects.
namespace ngextract
{
	// Total size of the packed blob (LUT + 101 keys + 17x16x256 uint32).
	constexpr size_t kBlobBytes = 256 + 101 * 272 + 17 * 16 * 256 * 4;

	// Cache file written next to ZeroIV.log after a successful live extract.
	constexpr const char* kCacheFile = "ZeroIV.ngdata.bin";

	// Unlocks the embedded magic.dat with the retail AES key (SHA-1 search).
	// Works on Legacy and Enhanced; preferred over a full NG table scan.
	bool from_magic(std::vector<uint8_t>& out, std::string& error);

	// Scans loaded modules for NG LUT/keys/tables by SHA-1 (Legacy-oriented fallback).
	bool from_process(std::vector<uint8_t>& out, std::string& error);

	// Reads kCacheFile from the process working directory.
	bool from_cache(std::vector<uint8_t>& out, std::string& error);

	// Writes the blob to kCacheFile (best-effort; failure is logged via error).
	bool to_cache(const std::vector<uint8_t>& blob, std::string& error);
}
