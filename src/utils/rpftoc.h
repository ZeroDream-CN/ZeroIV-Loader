#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Minimal RPF7 table-of-contents reader and writer.
//
// Only what the dlclist patch needs is implemented, but the one rule that
// matters is kept: the entry array and the name table are carried around as the
// exact bytes that came off disk. A payload edit then rewrites 16 bytes inside
// that buffer instead of re-serialising 1644 entries, so directory entries,
// resource flags and anything else this code does not model survive untouched.
//
// Layout of an RPF7 archive (all little endian):
//
//   header   16 bytes   "RPF7", entryCount, namesLength, encryption
//   entries  count * 16
//   names    namesLength   NUL-terminated strings addressed by nameOffset
//   payloads at 512-byte block offsets stored in the entries
//
// Directory entries point at a contiguous run of the entry array
// (entriesIndex / entriesCount), so the tree is walked by index range.
namespace rpftoc
{
	constexpr uint32_t kEncryptionOpen = 0x4E45504F; // "OPEN", plaintext TOC
	constexpr uint32_t kVersionRpf7 = 0x52504637;    // "RPF7", stored little endian
	constexpr uint32_t kDirectoryIdent = 0x7FFFFF00;
	constexpr uint64_t kBlockSize = 512;
	constexpr uint64_t kAlign = 2048; // every retail archive is a multiple of this

	struct Table
	{
		uint32_t encryption = 0;
		uint32_t count = 0;
		std::string entries; // count * 16 raw bytes
		std::string names;   // raw name blob
		bool isOpen() const { return encryption == kEncryptionOpen; }
		bool empty() const { return count == 0 || entries.size() != count * 16ull; }

		bool entryIsDirectory(uint32_t index) const;
		bool entryIsResource(uint32_t index) const;
		bool entryIsEncrypted(uint32_t index) const;
		uint32_t entryNameOffset(uint32_t index) const;
		uint32_t entryStoredSize(uint32_t index) const;
		uint32_t entryBlock(uint32_t index) const;
		uint32_t entryRealSize(uint32_t index) const;   // raw field
		uint32_t entryPayloadSize(uint32_t index) const; // decoded payload length
		uint32_t entryFlags(uint32_t index) const;
		std::string entryName(uint32_t index) const;

		// Points one binary entry at a stored-or-raw payload. Leaves the name
		// offset alone, so the entry keeps its place in the tree.
		void setPayload(uint32_t index, uint32_t block, uint32_t stored, uint32_t realSize);

		// Marks one binary entry as plaintext. Used after its payload has actually
		// been decrypted: the flag and the bytes must agree, or the engine reads
		// ciphertext as if it were XML.
		void clearEncryption(uint32_t index);

		// Resolves "common/data/dlclist.xml" to an entry index, or -1.
		int find(const std::string& path) const;

		// Checks that every entry's payload range lies inside an archive of
		// `archiveSize` bytes. A table captured from another process is only
		// trusted after this passes: a mis-decrypted or mis-located entry array
		// still parses as plausible bytes, but its offsets land outside the file.
		bool checkPayloads(uint64_t archiveSize, std::string& error) const;

		// Serialises header + entries + names, with `encryption` written to the
		// header. Used both for the on-disk form and for the cached dump.
		std::string serialize(uint32_t encryptionOverride) const;
	};

	// Reads and parses a plaintext TOC. Fails on an encrypted archive, which is
	// the signal that the table has to be decrypted first.
	bool read(const std::string& path, Table& out, std::string& error);

	// Reads the table of contents exactly as stored, encryption included, so the
	// caller can decrypt it. `blob` is header, entries and names, i.e. the same
	// layout serialize() writes, which lets a decrypted copy go straight into
	// parse(). `headerOnly` reads just the 16-byte header, enough to learn the
	// entry count and the cipher.
	struct Raw
	{
		uint32_t encryption = 0;
		uint32_t count = 0;
		uint32_t namesLength = 0;
		std::string blob;
	};

	bool readRaw(const std::string& path, Raw& out, std::string& error);
	bool readHeader(const std::string& path, Raw& out, std::string& error);

	// Parses a serialised table (the cached dump), without touching the disk.
	bool parse(const std::string& blob, Table& out, std::string& error);

	// Rewrites the table in place as plaintext. The file size is unchanged, so
	// every payload offset stays valid.
	bool writeOpen(const std::string& path, const Table& table, std::string& error);

	// Reads a payload stored uncompressed. Returns false for a compressed one,
	// which the caller has to obtain from the engine instead.
	bool readPayload(const std::string& path, const Table& table, uint32_t index,
		std::string& out, std::string& error);

	// Reads a payload exactly as stored: no decryption, no inflation.
	bool readPayloadRaw(const std::string& path, const Table& table, uint32_t index,
		std::string& out, std::string& error);

	// Writes `data` at `offset`, growing the file when the region runs past the
	// end. The offset is rounded up to kAlign.
	bool writePayload(const std::string& path, uint64_t& offset, const std::string& data,
		std::string& error);

	// Writes `data` at exactly `offset`, with no alignment rounding and no
	// growth. Only for rewriting a payload in place at the same length it
	// already occupies, which is what decrypting one amounts to.
	bool writePayloadAt(const std::string& path, uint64_t offset, const std::string& data,
		std::string& error);

	// Extends (or trims) the archive to exactly `size` bytes, zero filling the
	// new region.
	bool ensureFileSize(const std::string& path, uint64_t size, std::string& error);

	uint64_t fileSize(const std::string& path);
}
