#include <windows.h>

// Offline check for the heap-size patch location. Not part of the loader.
//
// The heap adjuster works by overwriting a 4-byte constant in GTA5.exe that
// holds the heap budget in bytes. It finds it with a byte pattern and a fixed
// offset; this tool proves both still hold for the build installed on disk, and
// prints the vanilla value so the log can be checked against it.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	struct Sig
	{
		const char* name;
		const char* bytes; // space separated hex, "?" = wildcard
		size_t offset;     // where the 4-byte heap size starts, from the match
	};

	const Sig kSigs[] = {
		{ "legacy", "83 C8 01 48 8D 0D ? ? ? ? 41 B1 01 45 33 C0", 17 },
		{ "enhanced", "BA ? ? ? ? 45 31 C0 41 B1 01 E8 ? ? ? ? 48 8D 0D", 1 },
	};

	std::vector<int> parse(const char* pattern)
	{
		std::vector<int> out;
		const char* p = pattern;
		while (*p)
		{
			while (*p == ' ')
				++p;
			if (!*p)
				break;
			if (*p == '?')
			{
				out.push_back(-1);
				++p;
				continue;
			}
			out.push_back(static_cast<int>(strtoul(p, const_cast<char**>(&p), 16)));
		}
		return out;
	}

	std::vector<uint8_t> read_file(const char* path)
	{
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in)
			return {};
		const std::streamoff size = in.tellg();
		in.seekg(0);
		std::vector<uint8_t> data(static_cast<size_t>(size));
		in.read(reinterpret_cast<char*>(data.data()), size);
		return data;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: scanheap <exe>\n");
		return 2;
	}

	const std::vector<uint8_t> image = read_file(argv[1]);
	if (image.empty())
	{
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}
	std::printf("image: %zu bytes\n", image.size());

	for (const auto& sig : kSigs)
	{
		const std::vector<int> bytes = parse(sig.bytes);
		const size_t n = bytes.size();
		size_t hits = 0;
		std::printf("\n[%s] pattern %zu bytes, value at +%zu\n", sig.name, n, sig.offset);

		for (size_t i = 0; i + n + sig.offset + 4 <= image.size(); ++i)
		{
			bool match = true;
			for (size_t j = 0; j < n; ++j)
			{
				if (bytes[j] != -1 && image[i + j] != static_cast<uint8_t>(bytes[j]))
				{
					match = false;
					break;
				}
			}
			if (!match)
				continue;

			++hits;
			const size_t at = i + sig.offset;
			int32_t value = 0;
			memcpy(&value, &image[at], 4);
			std::printf("  match at file offset 0x%zX -> value at 0x%zX = %d (%.1f MiB)\n", i, at,
				value, static_cast<double>(value) / (1024.0 * 1024.0));

			// A heap budget is a whole number of MiB in the low hundreds; show the
			// surrounding bytes so a wrong offset is obvious rather than plausible.
			std::printf("    bytes from match:");
			for (size_t k = 0; k < 34 && i + k < image.size(); ++k)
				std::printf(" %02X", image[i + k]);
			std::printf("\n");
		}

		if (hits == 0)
			std::printf("  NO MATCH\n");
	}
	return 0;
}
