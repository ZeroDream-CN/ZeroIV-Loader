#include <windows.h>

// Signature bisection helper for the heap-size call site.
//
// The signature known from older builds did not match this executable, so this
// takes any pattern on the command line and prints every hit with the 4-byte
// value at a chosen offset. Shrinking the pattern from the right shows exactly
// which byte fell out of date instead of guessing at a replacement.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
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
			char* end = nullptr;
			const long v = strtol(p, &end, 16);
			if (end == p)
				break;
			out.push_back(static_cast<int>(v));
			p = end;
		}
		return out;
	}

	uint32_t rd32(const std::vector<uint8_t>& b, size_t at)
	{
		if (at + 4 > b.size())
			return 0;
		uint32_t v;
		memcpy(&v, &b[at], 4);
		return v;
	}

	void dump(const std::vector<uint8_t>& b, size_t from, size_t to)
	{
		for (size_t i = from; i < to && i < b.size(); ++i)
			std::printf("%02X ", b[i]);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::printf("usage: findpat <exe> \"<pattern>\" [valueOffset] [maxHits]\n");
		return 2;
	}

	const std::vector<uint8_t> image = read_file(argv[1]);
	if (image.empty())
	{
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}

	const std::vector<int> bytes = parse(argv[2]);
	const size_t offset = argc > 3 ? static_cast<size_t>(atoi(argv[3])) : 0;
	const size_t maxHits = argc > 4 ? static_cast<size_t>(atoi(argv[4])) : 8;
	if (bytes.empty())
	{
		std::printf("empty pattern\n");
		return 2;
	}

	std::printf("pattern %zu bytes, value at +%zu\n", bytes.size(), offset);
	const bool countOnly = maxHits == 0;
	size_t hits = 0;
	for (size_t i = 0; i + bytes.size() <= image.size(); ++i)
	{
		bool match = true;
		for (size_t j = 0; j < bytes.size(); ++j)
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
		if (countOnly || hits > maxHits)
			continue;

		const uint32_t value = rd32(image, i + offset);
		std::printf("  hit %zu at 0x%zX", hits, i);
		if (offset)
			std::printf("  (+%zu = 0x%08X = %u)", offset, value, value);
		std::printf("\n    before: ");
		dump(image, i >= 10 ? i - 10 : 0, i);
		std::printf("| match | ");
		dump(image, i + bytes.size(), i + bytes.size() + 16);
		std::printf("\n");

		if (hits >= maxHits)
			break;
	}

	std::printf("hits: %zu%s\n", hits, hits > maxHits ? " (truncated)" : "");
	return hits ? 0 : 1;}
