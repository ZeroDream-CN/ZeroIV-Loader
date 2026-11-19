#include "main.h"

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

/*
	RPF7 synthesis for unpacked packs.

	GTA V Enhanced refuses to mount a directory as an archive. The Legacy loader
	switched the packfile type to 2 when the path was a directory; on Enhanced
	every (flags, type) combination returns 0 after Open/GetFileSize(0)/IsRpf, so
	the engine never enumerates the folder. The only way to serve a loose pack is
	to hand the engine a real archive, which is what this file builds.

	Format notes (matching CodeWalker / gtalib-go, which produced archives the
	Enhanced engine loads):

	  header     u32 version "RPF7", u32 entryCount, u32 namesLength, u32 encTag
	  entries    entryCount * 16 bytes, root first, each directory naming a
	             contiguous range of its children
	  names      NUL-terminated, de-duplicated, padded to 16 bytes
	  payloads   raw file bodies, each starting on its own 512-byte block

	The archive is written with encTag "OPEN" (0x4E45504F): ZeroIV's packfile
	encryption hook already accepts unencrypted archives, so no key material is
	needed here and the pack stays readable by any RPF tool.
*/

namespace
{
	namespace fs = std::filesystem;

	constexpr uint32_t kVersion = 0x52504637;   // "RPF7"
	constexpr uint32_t kDirIdent = 0x7FFFFF00;
	constexpr uint32_t kRsc7Magic = 0x37435352; // "RSC7"
	constexpr uint32_t kOpenTag = 0x4E45504F;   // "OPEN"
	constexpr uint32_t kSector = 512;
	constexpr uint32_t kDirEntrySize = 16;
	constexpr uint32_t kMaxNameTable = 0x10000; // name offsets are 16-bit
	constexpr uint32_t kMaxField24 = 0xFFFFFF;

	struct Node
	{
		std::string name;
		bool dir = false;
		std::vector<uint32_t> children;

		// Files only.
		fs::path src;
		std::vector<uint8_t> embedded; // nested archive built in memory
		bool resource = false;
		uint32_t systemFlags = 0;
		uint32_t graphicsFlags = 0;
		// A resource is stored as its 16-byte RSC7 header followed by the deflate
		// compressed heap, and the TOC size covers both. GTA V obfuscates those
		// 16 bytes in retail archives, but a plain RSC7 header is accepted (and
		// is what CodeWalker writes), so the loose file is copied verbatim.
		uint64_t totalSize = 0; // bytes on disk, RSC header included
		uint64_t storedSize = 0; // bytes written to the archive
		uint32_t offsetBlocks = 0;

		// Filled while serialising.
		uint32_t nameOffset = 0;
		uint32_t entriesIndex = 0;
		uint32_t entriesCount = 0;
	};

	uint32_t block_count(uint64_t bytes)
	{
		return static_cast<uint32_t>((bytes + kSector - 1) / kSector);
	}

	bool read_prefix(const fs::path& path, uint8_t* out, size_t len)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(len));
		return in.gcount() == static_cast<std::streamsize>(len);
	}

	bool ends_with_rpf(const std::string& name)
	{
		if (name.size() < 4)
			return false;
		std::string tail = name.substr(name.size() - 4);
		for (auto& c : tail)
			c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		return tail == ".rpf";
	}

	bool build_archive(const fs::path& dir, std::vector<uint8_t>& out, std::string& error);

	// Appends one file node. A directory named *.rpf is a nested archive: it is
	// built first and stored as a binary entry, because the content.xml of a
	// pack refers to those names as RPF_FILE entries and the engine opens them
	// with fiPackfile against the entry, not against a directory.
	bool add_child(const fs::path& path, bool isDir, std::vector<Node>& nodes, uint32_t& index,
		std::string& error, int depth);

	bool add_directory(const fs::path& dir, uint32_t index, std::vector<Node>& nodes, std::string& error,
		int depth)
	{
		if (depth > 8)
		{
			error = "directory nesting is too deep under " + dir.string();
			return false;
		}

		std::vector<std::string> names;
		std::error_code ec;
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			names.push_back(it->path().filename().string());
		if (ec)
		{
			error = "cannot list " + dir.string() + ": " + ec.message();
			return false;
		}

		// CodeWalker keeps directories and files in one list sorted by name, so
		// the entry table reads back as a sorted tree.
		std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
			return a < b;
		});

		std::vector<uint32_t> children;
		for (const auto& name : names)
		{
			const fs::path child = dir / name;
			std::error_code sec;
			const bool isDir = fs::is_directory(child, sec);
			uint32_t childIndex = 0;
			if (!add_child(child, isDir, nodes, childIndex, error, depth))
				return false;
			children.push_back(childIndex);
		}

		nodes[index].children = std::move(children);
		return true;
	}

	bool add_child(const fs::path& path, bool isDir, std::vector<Node>& nodes, uint32_t& index,
		std::string& error, int depth)
	{
		const std::string name = path.filename().string();

		if (isDir && ends_with_rpf(name))
		{
			std::vector<uint8_t> bytes;
			if (!build_archive(path, bytes, error))
				return false;

			nodes.emplace_back();
			index = static_cast<uint32_t>(nodes.size() - 1);
			Node& node = nodes[index];
			node.name = name;
			node.dir = false;
			node.embedded = std::move(bytes);
			node.totalSize = node.embedded.size();
			node.storedSize = node.embedded.size();
			return true;
		}

		if (isDir)
		{
			nodes.emplace_back();
			index = static_cast<uint32_t>(nodes.size() - 1);
			nodes[index].name = name;
			nodes[index].dir = true;
			return add_directory(path, index, nodes, error, depth + 1);
		}

		std::error_code sec;
		const uintmax_t size = fs::file_size(path, sec);
		if (sec)
		{
			error = "cannot stat " + path.string() + ": " + sec.message();
			return false;
		}

		uint8_t header[16] = {};
		const bool haveHeader = size >= 16 && read_prefix(path, header, sizeof(header));
		const bool resource = haveHeader &&
			*reinterpret_cast<uint32_t*>(header) == kRsc7Magic;

		nodes.emplace_back();
		index = static_cast<uint32_t>(nodes.size() - 1);
		Node& node = nodes[index];
		node.name = name;
		node.dir = false;
		node.src = path;
		node.resource = resource;
		node.totalSize = size;
		node.storedSize = size;
		if (resource)
		{
			std::memcpy(&node.systemFlags, header + 8, 4);
			std::memcpy(&node.graphicsFlags, header + 12, 4);
		}
		return true;
	}

	void put_u32(std::vector<uint8_t>& out, uint32_t v)
	{
		out.push_back(static_cast<uint8_t>(v & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
	}

	void put_u16(std::vector<uint8_t>& out, uint16_t v)
	{
		out.push_back(static_cast<uint8_t>(v & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
	}

	void put_u24(std::vector<uint8_t>& out, uint32_t v)
	{
		out.push_back(static_cast<uint8_t>(v & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
		out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
	}

	// Grows the archive to `bytes` (multiple of the sector size), zero filled.
	void resize_zero(std::vector<uint8_t>& out, size_t bytes)
	{
		if (out.size() < bytes)
			out.resize(bytes, 0);
	}

	bool write_payload(std::vector<uint8_t>& out, const Node& node, std::string& error)
	{
		const size_t at = static_cast<size_t>(node.offsetBlocks) * kSector;
		resize_zero(out, at + block_count(node.storedSize) * kSector);
		if (node.storedSize == 0)
			return true;

		if (!node.embedded.empty())
		{
			std::memcpy(out.data() + at, node.embedded.data(), node.embedded.size());
			return true;
		}

		std::ifstream in(node.src, std::ios::binary);
		if (!in)
		{
			error = "cannot read " + node.src.string();
			return false;
		}

		// A resource whose total size does not fit the 24-bit TOC field stores
		// 0xFFFFFF there and repeats the real length inside the RSC header, in
		// the byte positions CodeWalker uses.
		if (node.resource && node.totalSize >= kMaxField24)
		{
			uint8_t header[16] = {};
			in.read(reinterpret_cast<char*>(header), sizeof(header));
			header[7] = static_cast<uint8_t>((node.totalSize >> 0) & 0xFF);
			header[14] = static_cast<uint8_t>((node.totalSize >> 8) & 0xFF);
			header[5] = static_cast<uint8_t>((node.totalSize >> 16) & 0xFF);
			header[2] = static_cast<uint8_t>((node.totalSize >> 24) & 0xFF);
			std::memcpy(out.data() + at, header, sizeof(header));
			in.seekg(16, std::ios::beg);
			in.read(reinterpret_cast<char*>(out.data() + at + 16),
				static_cast<std::streamsize>(node.storedSize - 16));
		}
		else
		{
			in.read(reinterpret_cast<char*>(out.data() + at), static_cast<std::streamsize>(node.storedSize));
		}

		if (!in && !in.eof())
		{
			error = "short read on " + node.src.string();
			return false;
		}
		return true;
	}

	bool build_archive(const fs::path& dir, std::vector<uint8_t>& out, std::string& error)
	{
		std::vector<Node> nodes;
		nodes.reserve(256);
		nodes.emplace_back(); // root
		nodes[0].dir = true;

		if (!add_directory(dir, 0, nodes, error, 0))
			return false;

		// Breadth-first so every directory's children form a contiguous range.
		std::vector<uint32_t> order;
		order.reserve(nodes.size());
		order.push_back(0);
		for (size_t i = 0; i < order.size(); ++i)
		{
			Node& node = nodes[order[i]];
			if (!node.dir)
				continue;
			node.entriesIndex = static_cast<uint32_t>(order.size());
			node.entriesCount = static_cast<uint32_t>(node.children.size());
			order.insert(order.end(), node.children.begin(), node.children.end());
		}

		// Name table: identical names share one offset.
		std::vector<uint8_t> names;
		std::unordered_map<std::string, uint32_t> seen;
		for (uint32_t idx : order)
		{
			Node& node = nodes[idx];
			auto it = seen.find(node.name);
			if (it != seen.end())
			{
				node.nameOffset = it->second;
				continue;
			}
			if (names.size() + node.name.size() + 1 > kMaxNameTable)
			{
				error = "name table exceeded 64 KiB for " + dir.string();
				return false;
			}
			node.nameOffset = static_cast<uint32_t>(names.size());
			seen.emplace(node.name, node.nameOffset);
			names.insert(names.end(), node.name.begin(), node.name.end());
			names.push_back(0);
		}
		resize_zero(names, (names.size() + 15) / 16 * 16);

		const uint32_t entryCount = static_cast<uint32_t>(order.size());
		const uint32_t headerBytes = block_count(16 + entryCount * kDirEntrySize + names.size()) * kSector;

		// Payload blocks, in entry order.
		uint64_t next = headerBytes / kSector;
		for (uint32_t idx : order)
		{
			Node& node = nodes[idx];
			if (node.dir)
				continue;
			if (next > kMaxField24)
			{
				error = "archive would exceed the 8 GiB TOC limit: " + dir.string();
				return false;
			}
			node.offsetBlocks = static_cast<uint32_t>(next);
			next += block_count(node.storedSize);
		}

		// Header + entries + names.
		out.clear();
		out.reserve(headerBytes);
		put_u32(out, kVersion);
		put_u32(out, entryCount);
		put_u32(out, static_cast<uint32_t>(names.size()));
		put_u32(out, kOpenTag);

		for (uint32_t idx : order)
		{
			const Node& node = nodes[idx];
			if (node.dir)
			{
				put_u32(out, node.nameOffset);
				put_u32(out, kDirIdent);
				put_u32(out, node.entriesIndex);
				put_u32(out, node.entriesCount);
				continue;
			}

			put_u16(out, static_cast<uint16_t>(node.nameOffset));
			if (node.resource)
			{
				put_u24(out, static_cast<uint32_t>(std::min<uint64_t>(node.totalSize, kMaxField24)));
			}
			else
			{
				// Zero means "stored uncompressed"; the reader then uses the
				// uncompressed size field.
				put_u24(out, 0);
			}
			put_u24(out, node.offsetBlocks | (node.resource ? 0x800000 : 0));
			if (node.resource)
			{
				put_u32(out, node.systemFlags);
				put_u32(out, node.graphicsFlags);
			}
			else
			{
				put_u32(out, static_cast<uint32_t>(node.storedSize));
				put_u32(out, 0); // encryptionType: none
			}
		}

		out.insert(out.end(), names.begin(), names.end());
		resize_zero(out, headerBytes);

		for (uint32_t idx : order)
		{
			const Node& node = nodes[idx];
			if (node.dir)
				continue;
			if (!write_payload(out, node, error))
				return false;
		}

		return true;
	}
}

namespace loosepack
{
	std::string Prepare(const fs::path& dir, std::string& error)
	{
		std::error_code ec;
		if (!fs::is_directory(dir, ec))
		{
			error = dir.string() + " is not a directory";
			return {};
		}

		// Signature of the tree: a rebuild is only needed when the file count,
		// the total size or the newest write time changed.
		uint64_t count = 0, total = 0, newest = 0;
		for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code sec;
			if (it->is_directory(sec))
				continue;
			++count;
			total += it->file_size(sec);
			const auto stamp = static_cast<uint64_t>(it->last_write_time(sec).time_since_epoch().count());
			if (stamp > newest)
				newest = stamp;
		}
		if (ec)
		{
			error = "cannot scan " + dir.string() + ": " + ec.message();
			return {};
		}
		if (count == 0)
		{
			error = dir.string() + " has no files";
			return {};
		}

		const std::string signature = std::to_string(count) + " " + std::to_string(total) + " " +
			std::to_string(newest);

		// A stable name per source directory: the basename plus a path hash, so
		// several loose packs can coexist in the cache.
		uint64_t hash = 1469598103934665603ull;
		const std::string key = dir.lexically_normal().string();
		for (unsigned char c : key)
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}
		char suffix[17];
		std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<uint32_t>(hash >> 32));

		const fs::path cacheDir = config::cache_root();
		fs::create_directories(cacheDir, ec);

		std::string base = dir.filename().string();
		if (base.empty())
			base = "pack";
		for (auto& c : base)
		{
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-'))
				c = '_';
		}
		const std::string fileName = base + "_" + suffix + ".rpf";
		const fs::path archive = cacheDir / fileName;
		const fs::path stampPath = cacheDir / (fileName + ".stamp");

		// Relative to the working directory, which is what the engine resolves
		// through the game root device.
		std::error_code relEc;
		std::string relative = fs::relative(archive, fs::current_path(), relEc).generic_string();
		if (relEc || relative.empty())
			relative = archive.generic_string();

		std::ifstream stampIn(stampPath, std::ios::binary);
		std::string previous;
		if (stampIn && fs::exists(archive, ec))
		{
			std::getline(stampIn, previous);
			if (previous == signature)
				return relative;
		}

		std::vector<uint8_t> bytes;
		std::string buildError;
		if (!build_archive(dir, bytes, buildError))
		{
			error = buildError;
			return {};
		}

		const fs::path tmp = cacheDir / (fileName + ".tmp");
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			if (!out)
			{
				error = "cannot write " + tmp.string();
				return {};
			}
			out.write(reinterpret_cast<const char*>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
			if (!out)
			{
				error = "short write on " + tmp.string();
				return {};
			}
		}

		fs::remove(archive, ec);
		fs::rename(tmp, archive, ec);
		if (ec)
		{
			error = "cannot publish " + archive.string() + ": " + ec.message();
			return {};
		}

		{
			std::ofstream stampOut(stampPath, std::ios::binary | std::ios::trunc);
			stampOut << signature << "\n";
		}

		return relative;
	}
}
