#include <windows.h>

// Offline check for ngcrypt + rpftoc + inflate. Not part of the loader.
//
// It performs exactly what the loader will do to update.rpf, with no game
// running: read the raw table of contents, decrypt it with the NG tables, find
// common/data/dlclist.xml, decrypt and inflate that payload, and print the
// result. Comparing the output with gtalib-go's extract proves the port.

#include "utils/ngcrypt.h"
#include "utils/rpftoc.h"
#include "utils/inflate.h"

#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <string>

namespace
{
	bool write_file(const std::string& path, const std::string& data)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		return static_cast<bool>(out);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: ngtest <archive.rpf> <out-file> [archive base name] [inner path]\n");
		return 2;
	}

	const std::string archive = argv[1];
	const std::string output = argv[2];

	// The key index mixes the archive's own file name with its total size, so the
	// name has to be the one on disk, not the full path.
	std::string name = "update.rpf";
	if (argc > 3)
		name = argv[3];

	// Which entry to pull out. dlclist.xml was the original use; gameconfig.xml
	// is what the pool table needs to be compared against.
	const std::string innerPath = argc > 4 ? argv[4] : "common/data/dlclist.xml";

	std::string error;
	if (!ngcrypt::init(error))
	{
		std::printf("ngcrypt init: %s\n", error.c_str());
		return 1;
	}
	std::printf("name hash(%s) = 0x%08X\n", name.c_str(), ngcrypt::calculateHash(name));

	rpftoc::Raw raw;
	if (!rpftoc::readRaw(archive, raw, error))
	{
		std::printf("readRaw: %s\n", error.c_str());
		return 1;
	}
	std::printf("header: entries=%u namesLen=%u encryption=0x%08X\n", raw.count, raw.namesLength,
		raw.encryption);

	const uint64_t size = rpftoc::fileSize(archive);
	const uint32_t keyLength = static_cast<uint32_t>(size & 0xFFFFFFFFull);
	std::printf("archive size=%llu keyLength=%u\n", static_cast<unsigned long long>(size),
		keyLength);

	// Entries and names are separate cipher streams, both keyed by the archive.
	if (raw.encryption != rpftoc::kEncryptionOpen && raw.encryption != 0)
	{
		const size_t entryBytes = static_cast<size_t>(raw.count) * 16;
		std::string entries = raw.blob.substr(16, entryBytes);
		std::string names = raw.blob.substr(16 + entryBytes);
		if (!ngcrypt::decrypt(entries, name, keyLength, error) ||
			!ngcrypt::decrypt(names, name, keyLength, error))
		{
			std::printf("decrypt toc: %s\n", error.c_str());
			return 1;
		}
		raw.blob = raw.blob.substr(0, 16) + entries + names;
		std::printf("toc decrypted, first names: %.33s\n", names.c_str());
	}

	rpftoc::Table table;
	if (!rpftoc::parse(raw.blob, table, error))
	{
		std::printf("parse: %s\n", error.c_str());
		return 1;
	}
	std::printf("root: dir=%d index=%u count=%u\n", table.entryIsDirectory(0) ? 1 : 0,
		table.entryRealSize(0), table.entryFlags(0));

	const int index = table.find(innerPath);
	std::printf("find(%s) = %d\n", innerPath.c_str(), index);
	if (innerPath == "--list")
	{
		// Every path in the archive with the two facts that decide whether the
		// engine accepts it: the entry kind, and the magic its payload starts
		// with. A .y* stored as a binary entry is a hard load error.
		std::function<void(uint32_t, uint32_t, const std::string&)> walk =
			[&](uint32_t start, uint32_t n, const std::string& prefix) {
			for (uint32_t k = 0; k < n && start + k < table.count; ++k)
			{
				const uint32_t i = start + k;
				const std::string leaf = table.entryName(i);
				const std::string path = prefix.empty() ? leaf : prefix + "/" + leaf;
				if (table.entryIsDirectory(i))
				{
					walk(table.entryRealSize(i), table.entryFlags(i), path);
					continue;
				}

				std::string payload;
				std::string readError;
				rpftoc::readPayloadRaw(archive, table, i, payload, readError);
				std::printf("  %-3s %-5s %9u %-52s ", table.entryIsResource(i) ? "RES" : "BIN",
					table.entryIsEncrypted(i) ? "enc" : "plain", table.entryPayloadSize(i),
					path.c_str());
				if (payload.size() >= 4)
				{
					const unsigned char* m = reinterpret_cast<const unsigned char*>(payload.data());
					if (m[0] >= 32 && m[0] <= 126 && m[3] >= 32 && m[3] <= 126)
						std::printf("magic=%.4s", payload.c_str());
					else
						std::printf("magic=%02X%02X%02X%02X", m[0], m[1], m[2], m[3]);
				}
				std::printf("\n");
			}
		};
		walk(table.entryRealSize(0), table.entryFlags(0), "");
		return 0;
	}
	if (innerPath == "--stats")
	{
		// How many entries still carry an encryption flag, and what are they?
		// A payload flagged encrypted is ciphertext, so anything that clears the
		// flag without also decrypting turns that file into garbage.
		uint32_t flagged = 0, resources = 0, binary = 0;
		std::map<std::string, int> byTop;
		for (uint32_t i = 0; i < table.count; ++i)
		{
			if (table.entryIsDirectory(i))
				continue;
			if (table.entryIsResource(i))
				++resources;
			if (table.entryIsEncrypted(i))
			{
				++flagged;
				++binary;
				const std::string n = table.entryName(i);
				const size_t slash = n.find('/');
				byTop[slash == std::string::npos ? n : n.substr(0, slash)]++;
			}
		}
		std::printf("entries=%u resources=%u flaggedEncrypted=%u\n", table.count, resources,
			flagged);
		for (const auto& kv : byTop)
			std::printf("  %-24s %d\n", kv.first.c_str(), kv.second);
		return 0;
	}
	if (index < 0)
		return 1;

	const uint32_t stored = table.entryStoredSize(static_cast<uint32_t>(index));
	const uint32_t realSize = table.entryPayloadSize(static_cast<uint32_t>(index));
	const uint32_t block = table.entryBlock(static_cast<uint32_t>(index));
	const uint32_t flags = table.entryIsEncrypted(static_cast<uint32_t>(index)) ? 1 : 0;
	std::printf("dlclist.xml: stored=%u real=%u block=%u flags=0x%08X\n", stored, realSize, block,
		flags);
	std::string payload;
	if (!rpftoc::readPayloadRaw(archive, table, static_cast<uint32_t>(index), payload, error))
	{
		std::printf("readPayloadRaw: %s\n", error.c_str());
		return 1;
	}

	if (flags != 0)
	{
		// A payload is keyed by its own name and its uncompressed size.
		if (!ngcrypt::decrypt(payload, table.entryName(static_cast<uint32_t>(index)), realSize,
				error))
		{
			std::printf("decrypt payload: %s\n", error.c_str());
			return 1;
		}
		std::printf("payload decrypted, first bytes: %.16s\n", payload.c_str());
	}

	if (stored != 0)
	{
		std::string inflated;
		if (!inflate::raw(inflated, payload, error))
		{
			std::printf("inflate: %s\n", error.c_str());
			return 1;
		}
		std::printf("inflated %zu -> %zu bytes\n", payload.size(), inflated.size());
		payload = inflated;
	}

	std::printf("result: %zu bytes\n", payload.size());
	std::printf("head: %.60s\n", payload.c_str());
	if (!write_file(output, payload))
	{
		std::printf("cannot write %s\n", output.c_str());
		return 1;
	}
	std::printf("wrote %s\n", output.c_str());
	return 0;
}

