#include <windows.h>

// Finds where GTA5.exe sets its heap budget, without relying on a signature
// copied from an older build.
//
// The heap budget is an immediate in a `mov r32, imm32` (or push/mov-to-memory)
// that is always a whole number of MiB in the hundreds -- 478 MiB on v3586, and
// the community adjusters write heapSize*1024*1024 the same way. Requiring
// "multiple of 1 MiB, in a plausible range" plus a look at the neighbouring
// instruction cuts a 47 MB executable down to a handful of candidates that can
// be checked by eye.

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

	const Section* find(const std::vector<Section>& all, size_t offset)
	{
		for (const auto& s : all)
		{
			if (offset >= s.rawPointer && offset < s.rawPointer + s.rawSize)
				return &s;
		}
		return nullptr;
	}

	void dump(const std::vector<uint8_t>& b, size_t from, size_t to)
	{
		for (size_t i = from; i < to && i < b.size(); ++i)
			std::printf("%02X ", b[i]);
	}
}

int main(int argc, char** argv)
{
	const uint32_t loMiB = argc > 2 ? static_cast<uint32_t>(atoi(argv[2])) : 256;
	const uint32_t hiMiB = argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 8192;
	if (argc < 2)
	{
		std::printf("usage: scanheap2 <exe> [loMiB] [hiMiB]\n");
		return 2;
	}

	const std::vector<uint8_t> image = read_file(argv[1]);
	if (image.empty())
	{
		std::printf("cannot read %s\n", argv[1]);
		return 1;
	}
	const auto all = sections(image);
	std::printf("image %zu bytes, %zu sections\n", image.size(), all.size());
	for (const auto& s : all)
		std::printf("  %-8s va=0x%08X raw=0x%08X size=0x%X\n", s.name.c_str(), s.virtualAddress,
			s.rawPointer, s.rawSize);

	std::printf("\ncandidates: imm32 multiple of 1 MiB in [%u, %u] MiB\n\n", loMiB, hiMiB);

	int shown = 0;
	for (size_t i = 1; i + 5 < image.size(); ++i)
	{
		const uint8_t op = image[i];
		const bool movR32 = op >= 0xB8 && op <= 0xBF;
		const bool push32 = op == 0x68;
		const bool rexMov = op == 0x41 && i + 1 < image.size() && image[i + 1] >= 0xB8 &&
			image[i + 1] <= 0xBF;
		if (!movR32 && !push32 && !rexMov)
			continue;

		const size_t immAt = i + (rexMov ? 2 : 1);
		const uint32_t imm = rd32(image, immAt);
		if (imm == 0 || imm % kMiB != 0)
			continue;
		const uint32_t mib = imm / kMiB;
		if (mib < loMiB || mib > hiMiB)
			continue;

		const Section* sec = find(all, i);
		const char* name = sec ? sec->name.c_str() : "?";
		const bool code = sec && (sec->characteristics & 0x20000000);
		if (strcmp(name, ".text") != 0)
			continue;

		std::printf("[%s%s] file 0x%zX  %s  imm=%u (%u MiB)\n", name, code ? "" : "(nodata)",
			i, rexMov ? "41 mov r32" : (push32 ? "push" : "mov r32"), imm, mib);
		std::printf("   ctx: ");
		dump(image, i - 12, i);
		std::printf("| ");
		dump(image, i, immAt + 4);
		std::printf("| ");
		dump(image, immAt + 4, immAt + 16);
		std::printf("\n");
		++shown;
	}
	std::printf("\ntotal %d\n", shown);
	return 0;
}
