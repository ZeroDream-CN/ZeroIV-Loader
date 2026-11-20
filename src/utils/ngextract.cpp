#include "ngextract.h"
#include "nghashes.h"
#include "ngmagic.h"

#include "utils/inflate.h"
#include "utils/log.h"

#include <Windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

/*
	Two ways to obtain the NG tables without shipping plaintext keys:

	1. from_magic — unlock CodeWalker's obfuscated magic.dat with the retail
	   AES key (SHA-1 search). Works on Legacy and Enhanced.
	2. from_process — SHA-1 scan for the LUT/keys/tables themselves (Legacy
	   dumps; Enhanced usually has no matching plaintext blobs).
*/

namespace
{
	constexpr size_t kAlign = 8;
	constexpr size_t kBlock = 1u << 20;

	// Compact SHA-1 (FIPS 180-1). Enough for digest matching; not a general crypto API.
	struct Sha1
	{
		uint32_t state[5]{};
		uint64_t bitCount = 0;
		uint8_t buffer[64]{};
		size_t bufferLen = 0;

		Sha1() { reset(); }

		void reset()
		{
			state[0] = 0x67452301u;
			state[1] = 0xEFCDAB89u;
			state[2] = 0x98BADCFEu;
			state[3] = 0x10325476u;
			state[4] = 0xC3D2E1F0u;
			bitCount = 0;
			bufferLen = 0;
		}

		static uint32_t rol(uint32_t v, uint32_t n)
		{
			return (v << n) | (v >> (32u - n));
		}

		void transform(const uint8_t block[64])
		{
			uint32_t w[80];
			for (int i = 0; i < 16; ++i)
			{
				w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
					(static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
					(static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
					static_cast<uint32_t>(block[i * 4 + 3]);
			}
			for (int i = 16; i < 80; ++i)
				w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

			uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
			for (int i = 0; i < 80; ++i)
			{
				uint32_t f, k;
				if (i < 20)
				{
					f = (b & c) | ((~b) & d);
					k = 0x5A827999u;
				}
				else if (i < 40)
				{
					f = b ^ c ^ d;
					k = 0x6ED9EBA1u;
				}
				else if (i < 60)
				{
					f = (b & c) | (b & d) | (c & d);
					k = 0x8F1BBCDCu;
				}
				else
				{
					f = b ^ c ^ d;
					k = 0xCA62C1D6u;
				}
				const uint32_t temp = rol(a, 5) + f + e + k + w[i];
				e = d;
				d = c;
				c = rol(b, 30);
				b = a;
				a = temp;
			}
			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
			state[4] += e;
		}

		void update(const uint8_t* data, size_t len)
		{
			bitCount += static_cast<uint64_t>(len) * 8ull;
			if (bufferLen)
			{
				const size_t need = 64 - bufferLen;
				const size_t take = len < need ? len : need;
				std::memcpy(buffer + bufferLen, data, take);
				bufferLen += take;
				data += take;
				len -= take;
				if (bufferLen == 64)
				{
					transform(buffer);
					bufferLen = 0;
				}
			}
			while (len >= 64)
			{
				transform(data);
				data += 64;
				len -= 64;
			}
			if (len)
			{
				std::memcpy(buffer, data, len);
				bufferLen = len;
			}
		}

		void final(uint8_t out[20])
		{
			const uint64_t bits = bitCount;
			const uint8_t one = 0x80;
			update(&one, 1);
			const uint8_t zero = 0;
			while (bufferLen != 56)
				update(&zero, 1);
			uint8_t len[8];
			for (int i = 7; i >= 0; --i)
				len[7 - i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
			update(len, 8);
			for (int i = 0; i < 5; ++i)
			{
				out[i * 4] = static_cast<uint8_t>(state[i] >> 24);
				out[i * 4 + 1] = static_cast<uint8_t>(state[i] >> 16);
				out[i * 4 + 2] = static_cast<uint8_t>(state[i] >> 8);
				out[i * 4 + 3] = static_cast<uint8_t>(state[i]);
			}
		}
	};

	struct DigestKey
	{
		uint8_t b[20]{};
		bool operator==(const DigestKey& o) const
		{
			return std::memcmp(b, o.b, 20) == 0;
		}
	};

	struct DigestHash
	{
		size_t operator()(const DigestKey& k) const
		{
			size_t h = 1469598103934665603ull;
			for (int i = 0; i < 20; ++i)
			{
				h ^= k.b[i];
				h *= 1099511628211ull;
			}
			return h;
		}
	};

	bool module_image(const uint8_t*& base, size_t& size, std::string& error)
	{
		HMODULE mod = ::GetModuleHandleW(nullptr);
		if (!mod)
		{
			error = "cannot locate the game module";
			return false;
		}
		base = reinterpret_cast<const uint8_t*>(mod);
		if (base[0] != 'M' || base[1] != 'Z')
		{
			error = "game module has a bad DOS header";
			return false;
		}
		const uint32_t lfanew = *reinterpret_cast<const uint32_t*>(base + 0x3C);
		const uint8_t* nt = base + lfanew;
		if (nt[0] != 'P' || nt[1] != 'E')
		{
			error = "game module has a bad NT header";
			return false;
		}
		const uint16_t optSize = *reinterpret_cast<const uint16_t*>(nt + 20);
		const uint8_t* opt = nt + 24;
		if (optSize < 60)
		{
			error = "game module optional header is too short";
			return false;
		}
		size = *reinterpret_cast<const uint32_t*>(opt + 56);
		if (size < 0x1000)
		{
			error = "game module SizeOfImage is too small";
			return false;
		}
		return true;
	}

	// Scans [base, base+size) for windows of `length` whose SHA-1 matches any
	// digest in `wanted`. Fills result[i] for every index that shares that digest.
	void scan_length(const uint8_t* base, size_t size, size_t length,
		const std::unordered_map<DigestKey, std::vector<size_t>, DigestHash>& wanted,
		std::vector<std::vector<uint8_t>>& result, std::atomic<size_t>& remaining)
	{
		if (wanted.empty() || size < length)
			return;

		const size_t last = size - length;
		const size_t numBlocks = (last / kBlock) + 1;
		const unsigned workers = std::max(1u, std::thread::hardware_concurrency());
		std::mutex mu;

		auto worker = [&](size_t blockIndex) {
			Sha1 sha;
			uint8_t digest[20];
			DigestKey key{};
			for (size_t off = 0; off < kBlock && remaining.load(std::memory_order_relaxed) > 0; off += kAlign)
			{
				const size_t pos = blockIndex * kBlock + off;
				if (pos > last)
					break;
				sha.reset();
				sha.update(base + pos, length);
				sha.final(digest);
				std::memcpy(key.b, digest, 20);
				const auto it = wanted.find(key);
				if (it == wanted.end())
					continue;
				std::lock_guard<std::mutex> lock(mu);
				for (size_t idx : it->second)
				{
					if (!result[idx].empty())
						continue;
					result[idx].assign(base + pos, base + pos + length);
					remaining.fetch_sub(1, std::memory_order_relaxed);
				}
			}
		};

		std::vector<std::thread> threads;
		threads.reserve(workers);
		std::atomic<size_t> nextBlock{0};
		for (unsigned t = 0; t < workers; ++t)
		{
			threads.emplace_back([&]() {
				for (;;)
				{
					if (remaining.load(std::memory_order_relaxed) == 0)
						return;
					const size_t b = nextBlock.fetch_add(1, std::memory_order_relaxed);
					if (b >= numBlocks)
						return;
					worker(b);
				}
			});
		}
		for (auto& th : threads)
			th.join();
	}

	bool pack_blob(const std::vector<uint8_t>& lut,
		const std::vector<std::vector<uint8_t>>& keys,
		const std::vector<std::vector<uint8_t>>& tables,
		std::vector<uint8_t>& out, std::string& error)
	{
		if (lut.size() != nghashes::kLutBytes)
		{
			error = "NG LUT was not found in the game module";
			return false;
		}
		for (size_t i = 0; i < nghashes::kKeyCount; ++i)
		{
			if (keys[i].size() != nghashes::kKeyBytes)
			{
				error = "NG key " + std::to_string(i) + " was not found in the game module";
				return false;
			}
		}
		for (size_t i = 0; i < nghashes::kTableCount; ++i)
		{
			if (tables[i].size() != nghashes::kTableRowBytes)
			{
				error = "NG decrypt table row " + std::to_string(i) +
					" was not found in the game module";
				return false;
			}
		}

		out.assign(ngextract::kBlobBytes, 0);
		std::memcpy(out.data(), lut.data(), nghashes::kLutBytes);
		uint8_t* keyDst = out.data() + nghashes::kLutBytes;
		for (size_t i = 0; i < nghashes::kKeyCount; ++i)
			std::memcpy(keyDst + i * nghashes::kKeyBytes, keys[i].data(), nghashes::kKeyBytes);
		uint8_t* tabDst = keyDst + nghashes::kKeyCount * nghashes::kKeyBytes;
		for (size_t i = 0; i < nghashes::kTableCount; ++i)
			std::memcpy(tabDst + i * nghashes::kTableRowBytes, tables[i].data(),
				nghashes::kTableRowBytes);
		return true;
	}
}

namespace ngextract
{
	bool from_cache(std::vector<uint8_t>& out, std::string& error)
	{
		std::ifstream in(kCacheFile, std::ios::binary);
		if (!in)
		{
			error = std::string(kCacheFile) + " is not present";
			return false;
		}
		out.assign(kBlobBytes, 0);
		in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(kBlobBytes));
		if (!in || in.gcount() != static_cast<std::streamsize>(kBlobBytes))
		{
			error = std::string(kCacheFile) + " has the wrong size";
			out.clear();
			return false;
		}
		return true;
	}

	bool to_cache(const std::vector<uint8_t>& blob, std::string& error)
	{
		if (blob.size() != kBlobBytes)
		{
			error = "refusing to cache an incomplete NG blob";
			return false;
		}
		std::ofstream out(kCacheFile, std::ios::binary | std::ios::trunc);
		if (!out)
		{
			error = std::string("cannot write ") + kCacheFile;
			return false;
		}
		out.write(reinterpret_cast<const char*>(blob.data()),
			static_cast<std::streamsize>(blob.size()));
		if (!out)
		{
			error = std::string("failed while writing ") + kCacheFile;
			return false;
		}
		return true;
	}

	uint32_t jenk_bytes(const uint8_t* data, size_t len)
	{
		uint32_t h = 0;
		for (size_t i = 0; i < len; ++i)
		{
			h += data[i];
			h += h << 10;
			h ^= h >> 6;
		}
		h += h << 3;
		h ^= h >> 11;
		h += h << 15;
		return h;
	}

	// .NET CompatPrng / System.Random used by CodeWalker UseMagicData.
	struct CsharpRandom
	{
		int32_t inext = 0;
		int32_t inextp = 0;
		int32_t seedArray[56]{};

		explicit CsharpRandom(int32_t seed)
		{
			constexpr int32_t mbig = 2147483647;
			constexpr int32_t mseed = 161803398;
			int32_t subtraction = seed;
			if (seed == static_cast<int32_t>(0x80000000u))
				subtraction = mbig;
			else if (seed < 0)
				subtraction = -seed;

			int32_t mj = mseed - subtraction;
			seedArray[55] = mj;
			int32_t mk = 1;
			int32_t ii = 0;
			for (int32_t i = 1; i < 55; ++i)
			{
				ii += 21;
				if (ii >= 55)
					ii -= 55;
				seedArray[ii] = mk;
				mk = mj - mk;
				if (mk < 0)
					mk += mbig;
				mj = seedArray[ii];
			}
			for (int k = 1; k < 5; ++k)
			{
				for (int32_t i = 1; i < 56; ++i)
				{
					int32_t n = i + 30;
					if (n >= 55)
						n -= 55;
					seedArray[i] = seedArray[i] - seedArray[1 + n];
					if (seedArray[i] < 0)
						seedArray[i] += mbig;
				}
			}
			inext = 0;
			inextp = 21;
		}

		int32_t internalSample()
		{
			constexpr int32_t mbig = 2147483647;
			int32_t locINext = inext + 1;
			if (locINext >= 56)
				locINext = 1;
			int32_t locINextp = inextp + 1;
			if (locINextp >= 56)
				locINextp = 1;
			int32_t retVal = seedArray[locINext] - seedArray[locINextp];
			if (retVal == mbig)
				retVal--;
			if (retVal < 0)
				retVal += mbig;
			seedArray[locINext] = retVal;
			inext = locINext;
			inextp = locINextp;
			return retVal;
		}

		void nextBytes(uint8_t* buf, size_t len)
		{
			for (size_t i = 0; i < len; ++i)
				buf[i] = static_cast<uint8_t>(internalSample());
		}
	};

	bool aes256_ecb_decrypt(std::vector<uint8_t>& data, const uint8_t key[32], std::string& error)
	{
		BCRYPT_ALG_HANDLE alg = nullptr;
		BCRYPT_KEY_HANDLE keyHandle = nullptr;
		NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0);
		if (st < 0)
		{
			error = "BCryptOpenAlgorithmProvider failed";
			return false;
		}
		st = ::BCryptSetProperty(alg, BCRYPT_CHAINING_MODE,
			reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
			static_cast<ULONG>((std::wcslen(BCRYPT_CHAIN_MODE_ECB) + 1) * sizeof(wchar_t)), 0);
		if (st < 0)
		{
			::BCryptCloseAlgorithmProvider(alg, 0);
			error = "BCryptSetProperty(ECB) failed";
			return false;
		}
		st = ::BCryptGenerateSymmetricKey(alg, &keyHandle, nullptr, 0,
			const_cast<PUCHAR>(key), 32, 0);
		if (st < 0)
		{
			::BCryptCloseAlgorithmProvider(alg, 0);
			error = "BCryptGenerateSymmetricKey failed";
			return false;
		}

		const ULONG length = static_cast<ULONG>(data.size() - (data.size() % 16));
		ULONG written = 0;
		st = ::BCryptDecrypt(keyHandle, data.data(), length, nullptr, nullptr, 0,
			data.data(), length, &written, 0);
		::BCryptDestroyKey(keyHandle);
		::BCryptCloseAlgorithmProvider(alg, 0);
		if (st < 0)
		{
			error = "BCryptDecrypt failed";
			return false;
		}
		return true;
	}

	bool find_aes_key(std::vector<uint8_t>& key, std::string& error)
	{
		std::unordered_map<DigestKey, std::vector<size_t>, DigestHash> wanted;
		DigestKey dig{};
		std::memcpy(dig.b, nghashes::kAesKeyHash, 20);
		wanted[dig].push_back(0);
		std::vector<std::vector<uint8_t>> slot(1);
		std::atomic<size_t> left{1};

		const uint8_t* base = nullptr;
		size_t size = 0;
		if (!module_image(base, size, error))
			return false;
		logger::write("info", "ng: searching main module for AES key (%zu bytes)", size);
		scan_length(base, size, nghashes::kAesKeyBytes, wanted, slot, left);

		if (slot[0].empty())
		{
			// Enhanced may keep the AES key outside the main image.
			HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
				::GetCurrentProcessId());
			if (snap != INVALID_HANDLE_VALUE)
			{
				MODULEENTRY32W entry{};
				entry.dwSize = sizeof(entry);
				HMODULE mainMod = ::GetModuleHandleW(nullptr);
				if (::Module32FirstW(snap, &entry))
				{
					do
					{
						if (left.load(std::memory_order_relaxed) == 0)
							break;
						if (reinterpret_cast<HMODULE>(entry.modBaseAddr) == mainMod)
							continue;
						auto* dos = reinterpret_cast<const uint8_t*>(entry.modBaseAddr);
						scan_length(dos, static_cast<size_t>(entry.modBaseSize),
							nghashes::kAesKeyBytes, wanted, slot, left);
					} while (::Module32NextW(snap, &entry));
				}
				::CloseHandle(snap);
			}
		}

		if (slot[0].empty())
		{
			// CodeWalker GenerateV2 scans the on-disk exe; Enhanced often exposes
			// the AES key there even when the live image does not.
			const char* names[] = {
				"GTA5_Enhanced.exe", "gta5_enhanced.exe",
				"GTA5.exe", "gta5.exe", nullptr
			};
			for (const char** name = names; *name; ++name)
			{
				std::ifstream file(*name, std::ios::binary);
				if (!file)
					continue;
				file.seekg(0, std::ios::end);
				const std::streamoff fileSize = file.tellg();
				if (fileSize <= 0)
					continue;
				file.seekg(0, std::ios::beg);
				std::vector<uint8_t> image(static_cast<size_t>(fileSize));
				file.read(reinterpret_cast<char*>(image.data()), fileSize);
				if (!file)
					continue;
				logger::write("info", "ng: searching on-disk %s for AES key (%zu bytes)",
					*name, image.size());
				left.store(1, std::memory_order_relaxed);
				slot[0].clear();
				scan_length(image.data(), image.size(), nghashes::kAesKeyBytes, wanted, slot, left);
				if (!slot[0].empty())
					break;
			}
		}

		if (slot[0].size() != nghashes::kAesKeyBytes)
		{
			error = "AES key was not found in the game modules or on-disk exe";
			return false;
		}
		key = std::move(slot[0]);
		return true;
	}

	bool from_magic(std::vector<uint8_t>& out, std::string& error)
	{
		if (ngmagic::kDataSize == 0)
		{
			error = "magic.dat is not embedded in this build";
			return false;
		}

		std::vector<uint8_t> aesKey;
		if (!find_aes_key(aesKey, error))
			return false;

		const int32_t seed = static_cast<int32_t>(jenk_bytes(aesKey.data(), aesKey.size()));
		CsharpRandom rnd(seed);
		std::vector<uint8_t> rb1(ngmagic::kDataSize);
		std::vector<uint8_t> rb2(ngmagic::kDataSize);
		std::vector<uint8_t> rb3(ngmagic::kDataSize);
		std::vector<uint8_t> rb4(ngmagic::kDataSize);
		rnd.nextBytes(rb1.data(), rb1.size());
		rnd.nextBytes(rb2.data(), rb2.size());
		rnd.nextBytes(rb3.data(), rb3.size());
		rnd.nextBytes(rb4.data(), rb4.size());

		std::vector<uint8_t> scrambled(ngmagic::kDataSize);
		for (size_t i = 0; i < ngmagic::kDataSize; ++i)
		{
			scrambled[i] = static_cast<uint8_t>(
				(static_cast<int>(ngmagic::kData[i]) - static_cast<int>(rb1[i]) -
					static_cast<int>(rb2[i]) - static_cast<int>(rb3[i]) -
					static_cast<int>(rb4[i])) & 0xFF);
		}

		if (!aes256_ecb_decrypt(scrambled, aesKey.data(), error))
			return false;

		std::string inflated;
		std::string inflateError;
		if (!inflate::raw(inflated,
				std::string(reinterpret_cast<char*>(scrambled.data()), scrambled.size()),
				inflateError))
		{
			// DeflateStream may leave a PKCS-style remainder; try truncating to
			// the AES block length that CodeWalker decrypts in place.
			error = std::string("magic.dat inflate failed: ") + inflateError;
			return false;
		}

		constexpr size_t kKeysBytes = 101 * 272;       // 27472
		constexpr size_t kTablesBytes = 17 * 16 * 256 * 4; // 278528
		constexpr size_t kNeed = kKeysBytes + kTablesBytes + 256 + 16;
		if (inflated.size() < kNeed)
		{
			error = "inflated magic.dat is too short (" + std::to_string(inflated.size()) +
				" < " + std::to_string(kNeed) + ")";
			return false;
		}

		const uint8_t* keys = reinterpret_cast<const uint8_t*>(inflated.data());
		const uint8_t* tables = keys + kKeysBytes;
		const uint8_t* lut = tables + kTablesBytes;

		out.assign(kBlobBytes, 0);
		std::memcpy(out.data(), lut, 256);
		std::memcpy(out.data() + 256, keys, kKeysBytes);
		std::memcpy(out.data() + 256 + kKeysBytes, tables, kTablesBytes);
		logger::write("info", "ng: unlocked magic.dat with live AES key");
		return true;
	}

	bool from_process(std::vector<uint8_t>& out, std::string& error)
	{
		std::unordered_map<DigestKey, std::vector<size_t>, DigestHash> keyWanted;
		std::unordered_map<DigestKey, std::vector<size_t>, DigestHash> tableWanted;
		DigestKey lutWanted{};
		std::memcpy(lutWanted.b, nghashes::kLutHash, 20);

		for (size_t i = 0; i < nghashes::kKeyCount; ++i)
		{
			DigestKey k{};
			std::memcpy(k.b, nghashes::kKeyHashes[i], 20);
			keyWanted[k].push_back(i);
		}
		for (size_t i = 0; i < nghashes::kTableCount; ++i)
		{
			DigestKey k{};
			std::memcpy(k.b, nghashes::kTableHashes[i], 20);
			tableWanted[k].push_back(i);
		}

		std::vector<uint8_t> lut;
		std::vector<std::vector<uint8_t>> keys(nghashes::kKeyCount);
		std::vector<std::vector<uint8_t>> tables(nghashes::kTableCount);
		std::vector<std::vector<uint8_t>> lutSlot(1);

		std::unordered_map<DigestKey, std::vector<size_t>, DigestHash> lutWantedMap;
		lutWantedMap[lutWanted].push_back(0);

		std::atomic<size_t> lutLeft{1};
		std::atomic<size_t> keysLeft{nghashes::kKeyCount};
		std::atomic<size_t> tablesLeft{nghashes::kTableCount};

		auto scan_module = [&](const uint8_t* dos, size_t size) {
			if (!dos || size < 0x1000 || dos[0] != 'M' || dos[1] != 'Z')
				return;
			scan_length(dos, size, nghashes::kLutBytes, lutWantedMap, lutSlot, lutLeft);
			scan_length(dos, size, nghashes::kKeyBytes, keyWanted, keys, keysLeft);
			scan_length(dos, size, nghashes::kTableRowBytes, tableWanted, tables, tablesLeft);
		};

		auto complete = [&]() {
			return lutLeft.load(std::memory_order_relaxed) == 0 &&
				keysLeft.load(std::memory_order_relaxed) == 0 &&
				tablesLeft.load(std::memory_order_relaxed) == 0;
		};

		// Main module first — usually enough on Legacy; cheap path on Enhanced too.
		{
			const uint8_t* base = nullptr;
			size_t size = 0;
			if (!module_image(base, size, error))
				return false;
			logger::write("info", "ng: scanning main module (%zu bytes) for NG tables", size);
			scan_module(base, size);
		}

		if (!complete())
		{
			HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
				::GetCurrentProcessId());
			if (snap != INVALID_HANDLE_VALUE)
			{
				MODULEENTRY32W entry{};
				entry.dwSize = sizeof(entry);
				size_t count = 0;
				HMODULE mainMod = ::GetModuleHandleW(nullptr);
				if (::Module32FirstW(snap, &entry))
				{
					do
					{
						++count;
						if (complete())
							break;
						if (reinterpret_cast<HMODULE>(entry.modBaseAddr) == mainMod)
							continue;
						scan_module(reinterpret_cast<const uint8_t*>(entry.modBaseAddr),
							static_cast<size_t>(entry.modBaseSize));
					} while (::Module32NextW(snap, &entry));
				}
				::CloseHandle(snap);
				logger::write("info", "ng: extended scan across %zu module(s)", count);
			}
		}

		lut = std::move(lutSlot[0]);
		return pack_blob(lut, keys, tables, out, error);
	}
}
