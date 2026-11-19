#include <windows.h>

// Proves that the in-place decryption of update.rpf produced the same bytes the
// encrypted original held.
//
// stock: NG-encrypted table of contents and payloads.
// mods : table of contents already OPEN, payloads already decrypted.
//
// For every entry the two archives share, the stock payload is decrypted with
// the key the loader uses (entry name + uncompressed size) and compared byte
// for byte with what now sits in the mods archive. Any difference means the
// decryption wrote something wrong, and the entry name says which file.

#include "utils/ngcrypt.h"
#include "utils/rpftoc.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
	bool load(const char* path, rpftoc::Table& table, bool decrypt, std::string& error)
	{
		rpftoc::Raw raw;
		if (!rpftoc::readRaw(path, raw, error))
			return false;

		const uint64_t size = rpftoc::fileSize(path);
		const uint32_t keyLength = static_cast<uint32_t>(size & 0xFFFFFFFFull);

		if (decrypt && raw.encryption != rpftoc::kEncryptionOpen && raw.encryption != 0)
		{
			const size_t entryBytes = static_cast<size_t>(raw.count) * 16;
			std::string entries = raw.blob.substr(16, entryBytes);
			std::string names = raw.blob.substr(16 + entryBytes);
			if (!ngcrypt::decrypt(entries, "update.rpf", keyLength, error) ||
				!ngcrypt::decrypt(names, "update.rpf", keyLength, error))
				return false;
			raw.blob = raw.blob.substr(0, 16) + entries + names;
		}

		return rpftoc::parse(raw.blob, table, error);
	}

	std::string join(const std::string& prefix, const std::string& name)
	{
		if (prefix.empty())
			return name;
		return prefix + "/" + name;
	}

	// A name in the blob is only the leaf, and "vehicles.meta" exists under a
	// dozen directories, so entries are addressed by full path: the tree is
	// walked exactly the way the engine walks it.
	void collect(const rpftoc::Table& table, uint32_t index, uint32_t count,
		const std::string& prefix, std::map<std::string, uint32_t>& out)
	{
		for (uint32_t k = 0; k < count && index + k < table.count; ++k)
		{
			const uint32_t i = index + k;
			const std::string name = join(prefix, table.entryName(i));
			if (table.entryIsDirectory(i))
				collect(table, table.entryRealSize(i), table.entryFlags(i), name, out);
			else
				out[name] = i;
		}
	}

	std::map<std::string, uint32_t> paths(const rpftoc::Table& table)
	{
		std::map<std::string, uint32_t> out;
		if (table.count == 0)
			return out;
		collect(table, table.entryRealSize(0), table.entryFlags(0), "", out);
		return out;
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: verify <stock.rpf> <mods.rpf> [maxReport]\n");
		return 2;
	}
	const size_t maxReport = argc > 3 ? static_cast<size_t>(atoi(argv[3])) : 12;

	std::string error;
	if (!ngcrypt::init(error))
	{
		std::printf("ngcrypt: %s\n", error.c_str());
		return 1;
	}

	rpftoc::Table stock;
	rpftoc::Table mods;
	if (!load(argv[1], stock, true, error))
	{
		std::printf("stock: %s\n", error.c_str());
		return 1;
	}
	if (!load(argv[2], mods, false, error))
	{
		std::printf("mods: %s\n", error.c_str());
		return 1;
	}
	std::printf("stock %u entries (0x%08X), mods %u entries (0x%08X)\n", stock.count,
		stock.encryption, mods.count, mods.encryption);

	const auto stockPaths = paths(stock);
	const auto modsPaths = paths(mods);
	std::printf("stock %zu files, mods %zu files\n", stockPaths.size(), modsPaths.size());

	size_t same = 0, differ = 0, missing = 0, unreadable = 0;
	for (const auto& kv : stockPaths)
	{
		const std::string& name = kv.first;
		const uint32_t i = kv.second;
		const auto it = modsPaths.find(name);
		if (it == modsPaths.end())
		{
			++missing;
			continue;
		}
		const uint32_t j = it->second;

		std::string a;
		if (!rpftoc::readPayloadRaw(argv[1], stock, i, a, error))
		{
			++unreadable;
			continue;
		}
		const bool encrypted = stock.entryIsEncrypted(i);
		if (encrypted &&
			!ngcrypt::decrypt(a, stock.entryName(i), stock.entryPayloadSize(i), error))
		{
			std::printf("decrypt failed: %s: %s\n", name.c_str(), error.c_str());
			++unreadable;
			continue;
		}

		std::string b;
		if (!rpftoc::readPayloadRaw(argv[2], mods, j, b, error))
		{
			++unreadable;
			continue;
		}

		if (a == b)
		{
			++same;
			continue;
		}

		++differ;
		if (differ <= maxReport)
		{
			size_t first = 0;
			const size_t limit = a.size() < b.size() ? a.size() : b.size();
			while (first < limit && a[first] == b[first])
				++first;
			std::printf("DIFF %-46s stock=%zu mods=%zu first-diff@%zu%s\n", name.c_str(), a.size(),
				b.size(), first, encrypted ? "" : " (entry was not encrypted)");
		}
	}

	std::printf("\nidentical=%zu  differ=%zu  missing-in-mods=%zu  unreadable=%zu\n", same, differ,
		missing, unreadable);
	return differ == 0 ? 0 : 1;
}
