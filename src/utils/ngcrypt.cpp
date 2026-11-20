#include "ngcrypt.h"
#include "ngextract.h"

#include "utils/log.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

/*
	See ngcrypt.h for why this exists.

	The block cipher is 17 rounds over four 32-bit words, alternating two round
	functions that are pure table lookups plus a subkey XOR:

	  rounds 0, 1 and 16  round A: each output word is the XOR of four table
	                              entries selected by one byte each
	  rounds 2..15        round B: the same, but the table rows are permuted

	Each round uses its own 16-byte subkey taken from the 272-byte NG key, so a
	block is decrypted with 16 rounds of table lookups. This is a straight port of
	the reference implementation (CodeWalker's GTA5Crypto), which is also what
	gtalib-go's crypto package does; the layout and the byte/word order below are
	verified against it.

	Tables are resolved in order: embedded NGDATA resource, ZeroIV.ngdata.bin
	cache next to the log, then a live SHA-1 scan of the game module.
*/

namespace
{
	constexpr size_t kLutBytes = 256;
	constexpr size_t kKeyCount = 101;
	constexpr size_t kKeyBytes = 272;
	constexpr size_t kKeysOffset = kLutBytes;
	constexpr size_t kTablesOffset = kKeysOffset + kKeyCount * kKeyBytes;
	constexpr size_t kTablesWords = 17 * 16 * 256;
	constexpr size_t kTotalBytes = kTablesOffset + kTablesWords * 4;

	static_assert(kTotalBytes == ngextract::kBlobBytes, "ngcrypt blob size drift");

	const uint8_t* g_lut = nullptr;
	const uint8_t* g_keys = nullptr;
	const uint32_t* g_tables = nullptr;
	// Owns cache / extract blobs; the embedded resource is owned by the PE.
	std::vector<uint8_t> g_owned;
	std::once_flag g_once;
	bool g_ok = false;
	std::string g_error = "NG tables not loaded";

	uint32_t readU32(const uint8_t* p)
	{
		return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
	}

	// Rounds 0, 1 and 16: output words follow the input bytes in order.
	void roundA(const uint8_t in[16], const uint32_t key[4], const uint32_t* table, uint8_t out[16])
	{
		const uint32_t x1 = table[0 * 256 + in[0]] ^ table[1 * 256 + in[1]] ^
			table[2 * 256 + in[2]] ^ table[3 * 256 + in[3]] ^ key[0];
		const uint32_t x2 = table[4 * 256 + in[4]] ^ table[5 * 256 + in[5]] ^
			table[6 * 256 + in[6]] ^ table[7 * 256 + in[7]] ^ key[1];
		const uint32_t x3 = table[8 * 256 + in[8]] ^ table[9 * 256 + in[9]] ^
			table[10 * 256 + in[10]] ^ table[11 * 256 + in[11]] ^ key[2];
		const uint32_t x4 = table[12 * 256 + in[12]] ^ table[13 * 256 + in[13]] ^
			table[14 * 256 + in[14]] ^ table[15 * 256 + in[15]] ^ key[3];

		out[0] = static_cast<uint8_t>(x1);
		out[1] = static_cast<uint8_t>(x1 >> 8);
		out[2] = static_cast<uint8_t>(x1 >> 16);
		out[3] = static_cast<uint8_t>(x1 >> 24);
		out[4] = static_cast<uint8_t>(x2);
		out[5] = static_cast<uint8_t>(x2 >> 8);
		out[6] = static_cast<uint8_t>(x2 >> 16);
		out[7] = static_cast<uint8_t>(x2 >> 24);
		out[8] = static_cast<uint8_t>(x3);
		out[9] = static_cast<uint8_t>(x3 >> 8);
		out[10] = static_cast<uint8_t>(x3 >> 16);
		out[11] = static_cast<uint8_t>(x3 >> 24);
		out[12] = static_cast<uint8_t>(x4);
		out[13] = static_cast<uint8_t>(x4 >> 8);
		out[14] = static_cast<uint8_t>(x4 >> 16);
		out[15] = static_cast<uint8_t>(x4 >> 24);
	}

	// Rounds 2..15: the table rows are interleaved so each output word mixes
	// bytes from all four input words.
	void roundB(const uint8_t in[16], const uint32_t key[4], const uint32_t* table, uint8_t out[16])
	{
		const uint32_t x1 = table[0 * 256 + in[0]] ^ table[7 * 256 + in[7]] ^
			table[10 * 256 + in[10]] ^ table[13 * 256 + in[13]] ^ key[0];
		const uint32_t x2 = table[1 * 256 + in[1]] ^ table[4 * 256 + in[4]] ^
			table[11 * 256 + in[11]] ^ table[14 * 256 + in[14]] ^ key[1];
		const uint32_t x3 = table[2 * 256 + in[2]] ^ table[5 * 256 + in[5]] ^
			table[8 * 256 + in[8]] ^ table[15 * 256 + in[15]] ^ key[2];
		const uint32_t x4 = table[3 * 256 + in[3]] ^ table[6 * 256 + in[6]] ^
			table[9 * 256 + in[9]] ^ table[12 * 256 + in[12]] ^ key[3];

		out[0] = static_cast<uint8_t>(x1);
		out[1] = static_cast<uint8_t>(x1 >> 8);
		out[2] = static_cast<uint8_t>(x1 >> 16);
		out[3] = static_cast<uint8_t>(x1 >> 24);
		out[4] = static_cast<uint8_t>(x2);
		out[5] = static_cast<uint8_t>(x2 >> 8);
		out[6] = static_cast<uint8_t>(x2 >> 16);
		out[7] = static_cast<uint8_t>(x2 >> 24);
		out[8] = static_cast<uint8_t>(x3);
		out[9] = static_cast<uint8_t>(x3 >> 8);
		out[10] = static_cast<uint8_t>(x3 >> 16);
		out[11] = static_cast<uint8_t>(x3 >> 24);
		out[12] = static_cast<uint8_t>(x4);
		out[13] = static_cast<uint8_t>(x4 >> 8);
		out[14] = static_cast<uint8_t>(x4 >> 16);
		out[15] = static_cast<uint8_t>(x4 >> 24);
	}

	void block(const uint8_t in[16], const uint8_t* key, uint8_t out[16])
	{
		uint32_t sub[4];
		uint8_t current[16];
		uint8_t next[16];
		std::memcpy(current, in, 16);

		for (int i = 0; i < 4; ++i)
			sub[i] = readU32(key + i * 4);

		roundA(current, sub, g_tables + 0 * 16 * 256, next);
		std::memcpy(current, next, 16);

		for (int round = 1; round <= 16; ++round)
		{
			for (int i = 0; i < 4; ++i)
				sub[i] = readU32(key + round * 16 + i * 4);

			if (round == 1 || round == 16)
				roundA(current, sub, g_tables + static_cast<size_t>(round) * 16 * 256, next);
			else
				roundB(current, sub, g_tables + static_cast<size_t>(round) * 16 * 256, next);
			std::memcpy(current, next, 16);
		}

		std::memcpy(out, current, 16);
	}

	void bind(const uint8_t* data)
	{
		g_lut = data;
		g_keys = data + kKeysOffset;
		g_tables = reinterpret_cast<const uint32_t*>(data + kTablesOffset);
		g_ok = true;
		g_error.clear();
	}

	bool try_resource(std::string& error)
	{
		HMODULE module = nullptr;
		if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(&try_resource), &module))
		{
			error = "cannot locate the loader module";
			return false;
		}

		HRSRC found = ::FindResourceA(module, "NGDATA", RT_RCDATA);
		if (!found)
		{
			error = "the NG tables resource is missing from the build";
			return false;
		}

		HGLOBAL loaded = ::LoadResource(module, found);
		const uint8_t* data = loaded ? static_cast<const uint8_t*>(::LockResource(loaded)) : nullptr;
		const DWORD size = ::SizeofResource(module, found);
		if (!data || size != kTotalBytes)
		{
			error = "the NG tables resource has the wrong size (" + std::to_string(size) +
				" instead of " + std::to_string(kTotalBytes) + ")";
			return false;
		}

		bind(data);
		logger::write("info", "ng: tables loaded from embedded NGDATA resource");
		return true;
	}

	bool try_cache(std::string& error)
	{
		std::vector<uint8_t> blob;
		if (!ngextract::from_cache(blob, error))
			return false;
		g_owned = std::move(blob);
		bind(g_owned.data());
		logger::write("info", "ng: tables loaded from %s", ngextract::kCacheFile);
		return true;
	}

	bool cache_and_bind(std::vector<uint8_t> blob, const char* source)
	{
		std::string cacheError;
		if (ngextract::to_cache(blob, cacheError))
			logger::write("info", "ng: cached tables as %s", ngextract::kCacheFile);
		else
			logger::write("info", "ng: %s ok but cache failed (%s)", source, cacheError.c_str());

		g_owned = std::move(blob);
		bind(g_owned.data());
		logger::write("info", "ng: tables loaded via %s", source);
		return true;
	}

	bool try_magic(std::string& error)
	{
		std::vector<uint8_t> blob;
		if (!ngextract::from_magic(blob, error))
			return false;
		return cache_and_bind(std::move(blob), "magic.dat");
	}

	bool try_scan(std::string& error)
	{
		std::vector<uint8_t> blob;
		if (!ngextract::from_process(blob, error))
			return false;
		return cache_and_bind(std::move(blob), "module scan");
	}

	void load()
	{
		std::string error;
		if (try_resource(error))
			return;
		const std::string resourceError = error;

		if (try_cache(error))
			return;

		// Enhanced has no plaintext NG blobs for HashSearch; magic.dat + AES is
		// the path that actually works there (and on Legacy too).
		if (try_magic(error))
			return;
		const std::string magicError = error;

		if (try_scan(error))
			return;

		g_error = "NG tables unavailable (resource: " + resourceError +
			"; magic: " + magicError + "; scan: " + error + ")";
	}
}

namespace ngcrypt
{
	bool init(std::string& error)
	{
		std::call_once(g_once, load);
		if (!g_ok)
		{
			error = g_error;
			return false;
		}
		return true;
	}

	bool ready()
	{
		return g_ok;
	}

	uint32_t calculateHash(const std::string& name)
	{
		if (!g_ok)
			return 0;

		uint32_t result = 0;
		for (unsigned char c : name)
		{
			const uint32_t temp = 1025 * (static_cast<uint32_t>(g_lut[c]) + result);
			result = (temp >> 6) ^ temp;
		}
		return 32769 * (((9 * result) >> 11) ^ (9 * result));
	}

	bool decrypt(std::string& data, const std::string& name, uint32_t length, std::string& error)
	{
		if (!g_ok)
		{
			error = g_error;
			return false;
		}

		// The archive's name and total size together select one of the 101 keys.
		// This is why a size change makes an archive undecryptable: the index
		// moves and a different key is picked.
		const uint32_t hash = calculateHash(name);
		const uint32_t index = (hash + length + 61) % kKeyCount;
		const uint8_t* key = g_keys + static_cast<size_t>(index) * kKeyBytes;

		const size_t blocks = data.size() / 16;
		uint8_t in[16];
		uint8_t out[16];
		for (size_t i = 0; i < blocks; ++i)
		{
			std::memcpy(in, data.data() + i * 16, 16);
			block(in, key, out);
			std::memcpy(data.data() + i * 16, out, 16);
		}
		return true;
	}
}
