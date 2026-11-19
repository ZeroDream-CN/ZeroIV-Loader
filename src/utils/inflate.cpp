#include "inflate.h"

#include <cstdint>
#include <cstring>
#include <string>

/*
	A canonical-Huffman inflate. The structure follows the classic "puff"
	reference decoder, which is small enough to audit:

	  - bits are consumed LSB-first from the input;
	  - a Huffman code is decoded one bit at a time, comparing the running code
	    against the first code of each length, so no lookup tables are needed;
	  - a fixed and a dynamic block type share the same symbol loop, and stored
	    blocks are copied after aligning to a byte boundary.

	Nothing here allocates from the stream except the output itself, which is
	bounded by a caller-supplied limit: a corrupt payload must not be able to
	grow memory without bound.
*/

namespace
{
	constexpr int kMaxBits = 15;
	constexpr int kMaxCodes = 288;      // literal/length alphabet
	constexpr int kMaxDistCodes = 30;   // distance alphabet
	constexpr int kMaxCodeLengths = 19; // code-length alphabet

	// Hard ceiling on a decoded payload. dlclist.xml is ~4 KB; anything past this
	// is a corrupt stream, not a file the loader wants.
	constexpr size_t kMaxOutput = 64u << 20;

	// Length codes 257..285 and distance codes 0..29, with their extra bits.
	const uint16_t kLengthBase[29] = {
		3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
		131, 163, 195, 227, 258,
	};
	const uint8_t kLengthExtra[29] = {
		0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
	};
	const uint16_t kDistBase[30] = {
		1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
		2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577,
	};
	const uint8_t kDistExtra[30] = {
		0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12,
		13, 13,
	};
	// Order the code-length code lengths are stored in.
	const uint8_t kCodeLengthOrder[kMaxCodeLengths] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
	};

	struct Huffman
	{
		// count[len] = number of codes of that bit length; symbol[] = the symbols
		// ordered so a code of length len starting at first[len] indexes it.
		uint16_t count[kMaxBits + 1];
		uint16_t symbol[kMaxCodes];
	};

	struct Reader
	{
		const uint8_t* data = nullptr;
		size_t size = 0;
		size_t pos = 0;
		uint32_t bitbuf = 0;
		int bitcnt = 0;
		bool overflow = false;
	};

	int bits(Reader& r, int need)
	{
		uint32_t value = r.bitbuf;
		while (r.bitcnt < need)
		{
			if (r.pos >= r.size)
			{
				r.overflow = true;
				return 0;
			}
			value |= static_cast<uint32_t>(r.data[r.pos++]) << r.bitcnt;
			r.bitcnt += 8;
		}
		r.bitbuf = value >> need;
		r.bitcnt -= need;
		return static_cast<int>(value & ((1u << need) - 1));
	}

	// Builds the canonical code tables. Returns false when the lengths do not
	// describe a usable code (over-subscribed), which marks a corrupt block.
	bool construct(Huffman& h, const uint8_t* lengths, int n)
	{
		std::memset(h.count, 0, sizeof(h.count));
		for (int i = 0; i < n; ++i)
			++h.count[lengths[i]];
		if (h.count[0] == n)
			return true; // no codes at all: legal for the distance alphabet

		int left = 1;
		for (int len = 1; len <= kMaxBits; ++len)
		{
			left <<= 1;
			left -= h.count[len];
			if (left < 0)
				return false;
		}

		uint16_t offsets[kMaxBits + 1];
		offsets[1] = 0;
		for (int len = 1; len < kMaxBits; ++len)
			offsets[len + 1] = static_cast<uint16_t>(offsets[len] + h.count[len]);
		for (int i = 0; i < n; ++i)
			if (lengths[i])
				h.symbol[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
		return true;
	}

	// Reads one symbol: walk the canonical codes length by length.
	int decode(Reader& r, const Huffman& h)
	{
		int code = 0;
		int first = 0;
		int index = 0;
		for (int len = 1; len <= kMaxBits; ++len)
		{
			code |= bits(r, 1);
			if (r.overflow)
				return -1;
			const int count = h.count[len];
			if (code - first < count)
				return h.symbol[index + (code - first)];
			index += count;
			first += count;
			first <<= 1;
			code <<= 1;
		}
		return -1;
	}

	bool stored(Reader& r, std::string& out, std::string& error)
	{
		// Byte alignment: drop the bits left in the current byte.
		r.bitbuf = 0;
		r.bitcnt = 0;
		if (r.pos + 4 > r.size)
		{
			error = "stored block header is truncated";
			return false;
		}

		const unsigned len = r.data[r.pos] | (r.data[r.pos + 1] << 8);
		const unsigned nlen = r.data[r.pos + 2] | (r.data[r.pos + 3] << 8);
		r.pos += 4;
		if (len != (~nlen & 0xFFFF))
		{
			error = "stored block length is inconsistent";
			return false;
		}
		if (r.pos + len > r.size)
		{
			error = "stored block runs past the input";
			return false;
		}
		if (out.size() + len > kMaxOutput)
		{
			error = "decompressed data exceeds the size limit";
			return false;
		}

		out.append(reinterpret_cast<const char*>(r.data + r.pos), len);
		r.pos += len;
		return true;
	}

	bool codes(Reader& r, std::string& out, const Huffman& lengths, const Huffman& distances,
		std::string& error)
	{
		for (;;)
		{
			const int symbol = decode(r, lengths);
			if (symbol < 0)
			{
				error = "malformed literal/length code";
				return false;
			}
			if (symbol < 256)
			{
				if (out.size() + 1 > kMaxOutput)
				{
					error = "decompressed data exceeds the size limit";
					return false;
				}
				out.push_back(static_cast<char>(symbol));
				continue;
			}
			if (symbol == 256)
				return true;

			const int lengthIndex = symbol - 257;
			if (lengthIndex >= 29)
			{
				error = "invalid length symbol";
				return false;
			}
			const size_t length = kLengthBase[lengthIndex] +
				static_cast<size_t>(bits(r, kLengthExtra[lengthIndex]));

			const int distSymbol = decode(r, distances);
			if (distSymbol < 0 || distSymbol >= kMaxDistCodes)
			{
				error = "invalid distance code";
				return false;
			}
			const size_t distance = kDistBase[distSymbol] +
				static_cast<size_t>(bits(r, kDistExtra[distSymbol]));
			if (distance == 0 || distance > out.size())
			{
				error = "distance points before the start of the output";
				return false;
			}
			if (r.overflow)
			{
				error = "input ended inside a code";
				return false;
			}

			// Overlapping copies are legal and common: distance may be smaller
			// than length, so the bytes are copied one at a time.
			const size_t from = out.size() - distance;
			for (size_t i = 0; i < length; ++i)
				out.push_back(out[from + i]);
			if (out.size() > kMaxOutput)
			{
				error = "decompressed data exceeds the size limit";
				return false;
			}
		}
	}

	bool dynamic(Reader& r, std::string& out, std::string& error)
	{
		const int literals = bits(r, 5) + 257;
		const int distancesCount = bits(r, 5) + 1;
		const int codeLengths = bits(r, 4) + 4;
		if (r.overflow)
		{
			error = "dynamic block header is truncated";
			return false;
		}
		if (literals > 286 || distancesCount > kMaxDistCodes)
		{
			error = "dynamic block declares too many codes";
			return false;
		}

		uint8_t lengths[320] = {};
		for (int i = 0; i < codeLengths; ++i)
			lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(bits(r, 3));

		Huffman lengthCodes = {};
		if (!construct(lengthCodes, lengths, kMaxCodeLengths) || r.overflow)
		{
			error = "code-length code is invalid";
			return false;
		}

		// The symbol lengths themselves are run-length encoded.
		const int total = literals + distancesCount;
		int index = 0;
		while (index < total)
		{
			const int symbol = decode(r, lengthCodes);
			if (symbol < 0)
			{
				error = "malformed code-length symbol";
				return false;
			}
			if (symbol < 16)
			{
				lengths[index++] = static_cast<uint8_t>(symbol);
				continue;
			}

			uint8_t value = 0;
			int repeat = 0;
			if (symbol == 16)
			{
				if (index == 0)
				{
					error = "repeat code with nothing to repeat";
					return false;
				}
				value = lengths[index - 1];
				repeat = 3 + bits(r, 2);
			}
			else if (symbol == 17)
			{
				repeat = 3 + bits(r, 3);
			}
			else
			{
				repeat = 11 + bits(r, 7);
			}
			if (r.overflow || index + repeat > total)
			{
				error = "code-length repeat runs past the alphabet";
				return false;
			}
			while (repeat--)
				lengths[index++] = value;
		}

		Huffman literalsTable = {};
		Huffman distancesTable = {};
		if (!construct(literalsTable, lengths, literals) ||
			!construct(distancesTable, lengths + literals, distancesCount))
		{
			error = "dynamic block declares an unusable code";
			return false;
		}
		return codes(r, out, literalsTable, distancesTable, error);
	}

	bool fixed(Reader& r, std::string& out, std::string& error)
	{
		uint8_t lengths[288];
		for (int i = 0; i < 144; ++i)
			lengths[i] = 8;
		for (int i = 144; i < 256; ++i)
			lengths[i] = 9;
		for (int i = 256; i < 280; ++i)
			lengths[i] = 7;
		for (int i = 280; i < 288; ++i)
			lengths[i] = 8;

		uint8_t distLengths[kMaxDistCodes];
		for (int i = 0; i < kMaxDistCodes; ++i)
			distLengths[i] = 5;

		Huffman literalsTable = {};
		Huffman distancesTable = {};
		if (!construct(literalsTable, lengths, 288) ||
			!construct(distancesTable, distLengths, kMaxDistCodes))
		{
			error = "the fixed code table is unusable";
			return false;
		}
		return codes(r, out, literalsTable, distancesTable, error);
	}
}

namespace inflate
{
	bool raw(std::string& out, const std::string& input, std::string& error)
	{
		out.clear();
		error.clear();

		Reader r;
		r.data = reinterpret_cast<const uint8_t*>(input.data());
		r.size = input.size();

		int last = 0;
		while (!last)
		{
			last = bits(r, 1);
			const int type = bits(r, 2);
			if (r.overflow)
			{
				error = "input ended inside the block header";
				return false;
			}

			bool ok = false;
			switch (type)
			{
			case 0: ok = stored(r, out, error); break;
			case 1: ok = fixed(r, out, error); break;
			case 2: ok = dynamic(r, out, error); break;
			default:
				error = "reserved block type";
				return false;
			}
			if (!ok)
				return false;
			if (r.overflow)
			{
				error = "input ended before the block was complete";
				return false;
			}
		}
		return true;
	}
}
