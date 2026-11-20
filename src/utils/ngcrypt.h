#pragma once

#include <cstdint>
#include <string>

// The RPF7 NG cipher, in the direction the loader needs: reading.
//
// NG encryption replaces AES in modern GTA V archives. Everything about it is
// driven by three constant tables that the game derives from the executable and
// keeps in memory:
//
//   LUT            256 bytes              name hashing
//   keys           101 x 272 bytes        one per key index
//   decrypt tables 17 x 16 x 256 uint32   the cipher rounds
//
// They do not depend on the installation. Resolution order (see docs/ng-keys.md):
// embedded NGDATA resource, ZeroIV.ngdata.bin cache, then a live SHA-1 scan of
// the running game modules. Digests live in nghases; key bytes never ship.
//
// The key index is what makes this cipher awkward: it mixes the file's name and
// its total size, so an archive is only decryptable while its length is
// unchanged. That is precisely why appending to update.rpf used to destroy it,
// and why this code only ever decrypts the original file and writes a plaintext
// (OPEN) copy from the result.
namespace ngcrypt
{
	// Loads and sanity checks the embedded tables. Safe to call repeatedly.
	bool init(std::string& error);

	// True once init() has succeeded.
	bool ready();

	// The name hash used to pick a key. `length` is the archive's total size for
	// a table of contents, or the entry's uncompressed size for a payload.
	uint32_t calculateHash(const std::string& name);

	// Decrypts `data` in place. A trailing partial block is left untouched, which
	// is what the game does as well.
	bool decrypt(std::string& data, const std::string& name, uint32_t length,
		std::string& error);
}
