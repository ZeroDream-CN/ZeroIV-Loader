#include "rpftoc.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
	constexpr uint32_t kMaxEntries = 1000000;
	constexpr uint32_t kMaxNames = 32u << 20;
	constexpr uint32_t kMaxToc = 64u << 20;

	uint32_t le16(const char* p)
	{
		return static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8);
	}

	uint32_t le24(const char* p)
	{
		return static_cast<uint8_t>(p[0]) | (static_cast<uint8_t>(p[1]) << 8) |
			(static_cast<uint8_t>(p[2]) << 16);
	}

	uint32_t le32(const char* p)
	{
		return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
			(static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
			(static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
			(static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
	}

	void put24(std::string& buffer, size_t at, uint32_t value)
	{
		buffer[at + 0] = static_cast<char>(value & 0xFF);
		buffer[at + 1] = static_cast<char>((value >> 8) & 0xFF);
		buffer[at + 2] = static_cast<char>((value >> 16) & 0xFF);
	}

	void put32(std::string& buffer, size_t at, uint32_t value)
	{
		buffer[at + 0] = static_cast<char>(value & 0xFF);
		buffer[at + 1] = static_cast<char>((value >> 8) & 0xFF);
		buffer[at + 2] = static_cast<char>((value >> 16) & 0xFF);
		buffer[at + 3] = static_cast<char>((value >> 24) & 0xFF);
	}

	bool read_at(const std::string& path, uint64_t offset, uint64_t length, std::string& out,
		std::string& error)
	{
		HANDLE file = ::CreateFileA(path.c_str(), GENERIC_READ,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = "cannot open " + path + " (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}

		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		if (!::SetFilePointerEx(file, position, nullptr, FILE_BEGIN))
		{
			error = "seek failed in " + path + " (win32 " + std::to_string(::GetLastError()) + ")";
			::CloseHandle(file);
			return false;
		}

		out.assign(static_cast<size_t>(length), '\0');
		size_t total = 0;
		while (total < out.size())
		{
			const DWORD want = static_cast<DWORD>(out.size() - total);
			DWORD got = 0;
			if (!::ReadFile(file, out.data() + total, want, &got, nullptr) || got == 0)
				break;
			total += got;
		}
		::CloseHandle(file);

		if (total != out.size())
		{
			error = path + " is shorter than its header claims";
			return false;
		}
		return true;
	}
}

namespace rpftoc
{
	bool Table::entryIsDirectory(uint32_t index) const
	{
		if (index >= count)
			return false;
		return le32(entries.data() + static_cast<size_t>(index) * 16 + 4) == kDirectoryIdent;
	}

	bool Table::entryIsResource(uint32_t index) const
	{
		if (index >= count || entryIsDirectory(index))
			return false;
		// Bit 23 of the 24-bit offset field. The previous test compared the
		// directory sentinel at +4 with a mask, which matched directories and
		// almost never matched a real resource. plainify then treated .ymt
		// resources as binary, zeroed their graphicsFlags at +12, and the
		// engine exited with ERR_SYS_INVALIDRESOURCE_5.
		return (le24(entries.data() + static_cast<size_t>(index) * 16 + 5) & 0x800000) != 0;
	}

	uint32_t Table::entryNameOffset(uint32_t index) const
	{
		const char* p = entries.data() + static_cast<size_t>(index) * 16;
		return entryIsDirectory(index) ? le32(p) : le16(p);
	}

	uint32_t Table::entryStoredSize(uint32_t index) const
	{
		return le24(entries.data() + static_cast<size_t>(index) * 16 + 2);
	}

	uint32_t Table::entryBlock(uint32_t index) const
	{
		// Bit 23 of the stored offset marks a resource entry; the remaining 23 bits
		// are the block number. Leaving the bit in would put every resource at an
		// offset 4 GB past the archive.
		return le24(entries.data() + static_cast<size_t>(index) * 16 + 5) & 0x7FFFFF;
	}

	namespace
	{
		// A resource stores its size packed into two flag words instead of a plain
		// byte count. Each flag word is a 9-bit mantissa times a 512-byte page
		// size, so the payload length is the sum of the two.
		uint32_t resource_bytes(uint32_t flags)
		{
			const uint32_t sum = ((flags >> 27) & 0x1) | (((flags >> 26) & 0x1) << 1) |
				(((flags >> 25) & 0x1) << 2) | (((flags >> 24) & 0x1) << 3) |
				(((flags >> 17) & 0x7F) << 4) | (((flags >> 11) & 0x3F) << 5) |
				(((flags >> 7) & 0xF) << 6) | (((flags >> 5) & 0x3) << 7) |
				(((flags >> 4) & 0x1) << 8);
			return (0x200u << (flags & 0xF)) * sum;
		}
	}

	uint32_t Table::entryRealSize(uint32_t index) const
	{
		// The raw 32-bit field. For a directory this is entriesIndex and for a
		// resource it is a packed flag word, so payloads use entryPayloadSize().
		return le32(entries.data() + static_cast<size_t>(index) * 16 + 8);
	}

	uint32_t Table::entryPayloadSize(uint32_t index) const
	{
		if (index >= count || entryIsDirectory(index))
			return 0;
		const char* p = entries.data() + static_cast<size_t>(index) * 16;
		if (!entryIsResource(index))
			return le32(p + 8); // binary: uncompressed size

		// Only the flags are authoritative for a resource; the loader never reads
		// one, but bounds checking still needs a truthful length.
		return resource_bytes(le32(p + 8)) + resource_bytes(le32(p + 12));
	}

	bool Table::entryIsEncrypted(uint32_t index) const
	{
		if (index >= count || entryIsDirectory(index) || entryIsResource(index))
			return false;
		// CodeWalker only accepts 0 (plain) or 1 (encrypted). Other values at
		// this offset on binary-looking entries (e.g. 0x20000000 on map .ymt)
		// are not NG encryption types — treating them as ciphertext and
		// decrypting or zeroing them is what previously destroyed the map.
		return le32(entries.data() + static_cast<size_t>(index) * 16 + 12) == 1;
	}

	uint32_t Table::entryFlags(uint32_t index) const
	{
		return le32(entries.data() + static_cast<size_t>(index) * 16 + 12);
	}

	std::string Table::entryName(uint32_t index) const
	{
		if (index >= count)
			return {};
		const uint32_t offset = entryNameOffset(index);
		if (offset >= names.size())
			return {};
		const char* start = names.data() + offset;
		size_t length = 0;
		while (offset + length < names.size() && length < 260 && start[length])
			++length;
		return std::string(start, length);
	}

	void Table::setPayload(uint32_t index, uint32_t block, uint32_t stored, uint32_t realSize)
	{
		if (index >= count)
			return;
		const size_t at = static_cast<size_t>(index) * 16;
		put24(entries, at + 2, stored);
		put24(entries, at + 5, block);
		put32(entries, at + 8, realSize);
		put32(entries, at + 12, 0); // binary, unencrypted
	}

	void Table::clearEncryption(uint32_t index)
	{
		if (index >= count || entryIsDirectory(index) || entryIsResource(index))
			return;
		put32(entries, static_cast<size_t>(index) * 16 + 12, 0);
	}

	int Table::find(const std::string& path) const
	{
		if (empty() || names.empty())
			return -1;

		std::vector<std::string> parts;
		std::string current;
		for (char c : path)
		{
			if (c == '/')
			{
				if (!current.empty())
					parts.push_back(current);
				current.clear();
			}
			else
			{
				current.push_back(c);
			}
		}
		if (!current.empty())
			parts.push_back(current);
		if (parts.empty())
			return -1;

		// The root of an RPF7 is entry 0 and holds the top-level names.
		uint32_t index = 0;
		for (size_t depth = 0; depth < parts.size(); ++depth)
		{
			if (index >= count || !entryIsDirectory(index))
				return -1;
			const uint32_t first = entryRealSize(index); // entriesIndex
			const uint32_t total = entryFlags(index);    // entriesCount
			if (first >= count || static_cast<uint64_t>(first) + total > count)
				return -1;

			int next = -1;
			for (uint32_t child = first; child < first + total; ++child)
			{
				const std::string name = entryName(child);
				if (name.empty())
					continue;
				if (_stricmp(name.c_str(), parts[depth].c_str()) != 0)
					continue;
				next = static_cast<int>(child);
				break;
			}
			if (next < 0)
				return -1;

			index = static_cast<uint32_t>(next);
			if (depth + 1 == parts.size())
				return static_cast<int>(index);
		}
		return -1;
	}

	bool Table::checkPayloads(uint64_t archiveSize, std::string& error) const
	{
		if (empty() || archiveSize == 0)
		{
			error = "empty table of contents or archive";
			return false;
		}

		// The root must be a directory covering everything below it.
		if (!entryIsDirectory(0) || entryNameOffset(0) != 0)
		{
			error = "entry 0 is not the root directory";
			return false;
		}
		if (entryRealSize(0) != 1 || static_cast<uint64_t>(entryRealSize(0)) + entryFlags(0) > count)
		{
			error = "the root directory does not cover the entry array";
			return false;
		}

		uint32_t checked = 0;
		for (uint32_t i = 0; i < count; ++i)
		{
			if (entryIsDirectory(i))
				continue;

			const uint32_t stored = entryStoredSize(i);
			const uint32_t block = entryBlock(i);
			const uint32_t realSize = entryPayloadSize(i);
			if (realSize == 0)
				continue; // an empty file has no payload to place

			const uint64_t begin = static_cast<uint64_t>(block) * kBlockSize;
			const uint64_t length = stored ? stored : realSize;
			if (begin > archiveSize || begin + length > archiveSize)
			{
				error = "entry " + std::to_string(i) + " (" + entryName(i) + ") points at " +
					std::to_string(begin) + "+" + std::to_string(length) + ", past the " +
					std::to_string(archiveSize) + "-byte archive";
				return false;
			}
			++checked;
		}

		if (checked == 0)
		{
			error = "no binary entries in the table of contents";
			return false;
		}
		return true;
	}

	std::string Table::serialize(uint32_t encryptionOverride) const
	{
		std::string out;
		out.resize(16, '\0');
		put32(out, 0, kVersionRpf7);
		put32(out, 4, count);
		put32(out, 8, static_cast<uint32_t>(names.size()));
		put32(out, 12, encryptionOverride);
		out += entries;
		out += names;
		return out;
	}

	bool parse(const std::string& blob, Table& out, std::string& error)
	{
		if (blob.size() < 16)
		{
			error = "table of contents is truncated";
			return false;
		}
		if (le32(blob.data()) != kVersionRpf7)
		{
			error = "not an RPF7 archive";
			return false;
		}

		Table table;
		table.encryption = le32(blob.data() + 12);
		table.count = le32(blob.data() + 4);
		const uint32_t namesLength = le32(blob.data() + 8);

		if (table.count == 0 || table.count > kMaxEntries)
		{
			error = "implausible entry count " + std::to_string(table.count);
			return false;
		}
		if (namesLength == 0 || namesLength > kMaxNames)
		{
			error = "implausible name table length " + std::to_string(namesLength);
			return false;
		}

		const uint64_t tocLength = 16 + static_cast<uint64_t>(table.count) * 16 + namesLength;
		if (tocLength > kMaxToc || tocLength > blob.size())
		{
			error = "table of contents does not fit the archive";
			return false;
		}

		table.entries.assign(blob.data() + 16, static_cast<size_t>(table.count) * 16);
		table.names.assign(blob.data() + 16 + static_cast<size_t>(table.count) * 16, namesLength);
		out = std::move(table);
		return true;
	}

	bool read(const std::string& path, Table& out, std::string& error)
	{
		Raw raw;
		if (!readRaw(path, raw, error))
			return false;
		if (raw.encryption != kEncryptionOpen)
		{
			char magic[16];
			std::snprintf(magic, sizeof(magic), "0x%08X", raw.encryption);
			error = std::string("the table of contents is encrypted (") + magic + ")";
			return false;
		}
		return parse(raw.blob, out, error);
	}

	bool readHeader(const std::string& path, Raw& out, std::string& error)
	{
		std::string header;
		if (!read_at(path, 0, 16, header, error))
			return false;

		Raw raw;
		if (le32(header.data()) != kVersionRpf7)
		{
			error = "not an RPF7 archive";
			return false;
		}
		raw.count = le32(header.data() + 4);
		raw.namesLength = le32(header.data() + 8);
		raw.encryption = le32(header.data() + 12);
		if (raw.count == 0 || raw.count > kMaxEntries || raw.namesLength == 0 ||
			raw.namesLength > kMaxNames)
		{
			error = "implausible table of contents";
			return false;
		}
		raw.blob = header;
		out = std::move(raw);
		return true;
	}

	bool readRaw(const std::string& path, Raw& out, std::string& error)
	{
		Raw raw;
		if (!readHeader(path, raw, error))
			return false;

		std::string body;
		const uint64_t length = static_cast<uint64_t>(raw.count) * 16 + raw.namesLength;
		if (length > kMaxToc || !read_at(path, 16, length, body, error))
			return false;

		raw.blob += body;
		out = std::move(raw);
		return true;
	}

	bool writeOpen(const std::string& path, const Table& table, std::string& error)
	{
		const std::string blob = table.serialize(kEncryptionOpen);

		HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = "cannot open for writing (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}

		LARGE_INTEGER position;
		position.QuadPart = 0;
		DWORD written = 0;
		const bool ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
			::WriteFile(file, blob.data(), static_cast<DWORD>(blob.size()), &written, nullptr) &&
			written == blob.size();
		::FlushFileBuffers(file);
		::CloseHandle(file);

		if (!ok)
		{
			error = "write failed (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}
		return true;
	}

	bool readPayloadRaw(const std::string& path, const Table& table, uint32_t index,
		std::string& out, std::string& error)
	{
		if (index >= table.count)
		{
			error = "entry index out of range";
			return false;
		}
		if (table.entryIsDirectory(index))
		{
			error = "entry is a directory";
			return false;
		}

		// A binary entry stores `storedSize` bytes on disk, `realSize` bytes once
		// decrypted and inflated; a resource entry stores only its decoded size.
		const uint32_t stored = table.entryStoredSize(index);
		const uint32_t requested = stored ? stored : table.entryPayloadSize(index);
		if (requested == 0)
		{
			error = "payload is empty";
			return false;
		}

		const uint64_t offset = static_cast<uint64_t>(table.entryBlock(index)) * kBlockSize;
		return read_at(path, offset, requested, out, error);
	}

	bool readPayload(const std::string& path, const Table& table, uint32_t index,
		std::string& out, std::string& error)
	{
		if (index >= table.count)
		{
			error = "entry index out of range";
			return false;
		}
		if (table.entryIsDirectory(index))
		{
			error = "entry is a directory";
			return false;
		}
		if (table.entryStoredSize(index) != 0)
		{
			error = "payload is compressed";
			return false;
		}
		if (table.entryIsEncrypted(index))
		{
			error = "payload is encrypted";
			return false;
		}

		const uint32_t size = table.entryPayloadSize(index);
		if (size == 0)
		{
			error = "payload is empty";
			return false;
		}

		const uint64_t offset = static_cast<uint64_t>(table.entryBlock(index)) * kBlockSize;
		return read_at(path, offset, size, out, error);
	}

	bool writePayload(const std::string& path, uint64_t& offset, const std::string& data,
		std::string& error)
	{
		if (offset % kAlign != 0)
			offset = (offset + (kAlign - 1)) & ~(kAlign - 1);

		HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = "cannot open for writing (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}

		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		bool ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0;

		size_t total = 0;
		while (ok && total < data.size())
		{
			const DWORD want = static_cast<DWORD>(data.size() - total);
			DWORD written = 0;
			if (!::WriteFile(file, data.data() + total, want, &written, nullptr) || written == 0)
			{
				ok = false;
				break;
			}
			total += written;
		}
		::FlushFileBuffers(file);
		::CloseHandle(file);

		if (!ok || total != data.size())
		{
			error = "write failed (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}
		return true;
	}

	bool writePayloadAt(const std::string& path, uint64_t offset, const std::string& data,
		std::string& error)
	{
		HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = "cannot open for writing (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}

		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		bool ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != 0;

		size_t total = 0;
		while (ok && total < data.size())
		{
			const DWORD want = static_cast<DWORD>(data.size() - total);
			DWORD written = 0;
			if (!::WriteFile(file, data.data() + total, want, &written, nullptr) || written == 0)
			{
				ok = false;
				break;
			}
			total += written;
		}
		::FlushFileBuffers(file);
		::CloseHandle(file);

		if (!ok || total != data.size())
		{
			error = "write failed (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}
		return true;
	}

	uint64_t fileSize(const std::string& path)
	{
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!::GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
			return 0;
		return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
	}

	bool ensureFileSize(const std::string& path, uint64_t size, std::string& error)
	{
		HANDLE file = ::CreateFileA(path.c_str(), GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			error = "cannot open for writing (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}

		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(size);
		const bool ok = ::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
			::SetEndOfFile(file);
		::CloseHandle(file);

		if (!ok)
		{
			error = "cannot resize (win32 " + std::to_string(::GetLastError()) + ")";
			return false;
		}
		return true;
	}
}
