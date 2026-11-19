#include <windows.h>

// Looks for the heap budget where it is most likely to live: a global, not an
// immediate in code. scanheap2 only searched code operands and found nothing
// but data tables, so this scans the initialized data sections for a 4-byte
// value that is a whole number of MiB in the hundreds -- the shape every
// community adjuster reads and writes.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{
	constexpr uint32_t kMiB = 1024u * 1024u;

	struct Section
	{
		std::string name;
		uint32_t virtualAddress;
		uint32_t virtualSize;
		uint32_t rawPointer;
		uint32_t rawSize;
		uint32_t characteristics;
	};

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

	uint32_t rd32(const std::vector<uint8_t>& b, size_t at)
	{
		if (at + 4 > b.size())
			return 0;
		uint32_t v;
		memcpy(&v, &b[at], 4);
		return v;
	}
	uint16_t rd16(const std::vector<uint8_t>& b, size_t at)
	{
		if (at + 2 > b.size())
			return 0;
		uint16_t v;
		memcpy(&v, &b[at], 2);
		return v;
	}

	std::vector<Section> sections(const std::vector<uint8_t>& image)
	{
		std::vector<Section> out;
		const uint32_t pe = rd32(image, 0x3C);
		if (rd32(image, pe) != 0x00004550)
			return out;
		const uint16_t count = rd16(image, pe + 6);
		const uint16_t optSize = rd16(image, pe + 20);
		const size_t table = pe + 24 + optSize;
		for (uint16_t i = 0; i < count; ++i)
		{
			const size_t at = table + static_cast<size_t>(i) * 40;
			Section s;
			char name[9] = {};
			memcpy(name, &image[at], 8);
			s.name = name;
			s.virtualSize = rd32(image, at + 8);
			s.virtualAddress = rd32(image, at + 12);
			s.rawSize = rd32(image, at + 16);
			s.rawPointer = rd32(image, at + 20);
			s.characteristics = rd32(image, at + 36);
			out.push_back(s);
		}
		return out;
	}
}

int main(int argc, char** argv)
{
	const uint32_t loMiB = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 256;
	const uint32_t hiMiB = argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 4096;
	if (argc < 2)
	{
		std::printf("usage: scanheap3 <exe> [loMiB] [hiMiB]\n");
		return 2;
	}

	const std::vector<uint8_t> image = read_file(argv[1]);
	if (image.empty())
	{
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}

	std::printf("value scan, 1 MiB multiples in [%u, %u] MiB, aligned to 4\n\n", loMiB, hiMiB);
	int shown = 0;
	for (const auto& s : sections(image))
	{
		if (s.rawSize == 0)
			continue;
		// Writable, initialized data: .data and anything like it.
		const bool code = (s.characteristics & 0x20000000) != 0;
		if (code)
			continue;

		for (uint32_t off = 0; off + 4 <= s.rawSize; off += 4)
		{
			const uint32_t v = rd32(image, static_cast<size_t>(s.rawPointer) + off);
			if (v == 0 || v % kMiB != 0)
				continue;
			const uint32_t mib = v / kMiB;
			if (mib < loMiB || mib > hiMiB)
				continue;

			std::printf("  %-8s rva=0x%08X (file 0x%zX) = %u MiB\n", s.name.c_str(),
				s.virtualAddress + off, static_cast<size_t>(s.rawPointer) + off, mib);
			++shown;
		}
	}
	std::printf("\ntotal %d\n", shown);
	return 0;
}
