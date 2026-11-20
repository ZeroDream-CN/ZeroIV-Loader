#include "main.h"
#include "utils/rsccompat.h"

#include <cstring>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	constexpr uint32_t kRsc7Magic = 0x37435352; // "RSC7"

	struct VersionPair
	{
		const char* ext; // including leading '.'
		int legacy;
		int gen9;
	};

	// Mirrors gtalib-go/convert KnownVersions (CodeWalker GetVersion).
	constexpr VersionPair kVersions[] = {
		{ ".ytd", 13, 5 },
		{ ".ydr", 165, 159 },
		{ ".ydd", 165, 159 },
		{ ".yft", 162, 171 },
		{ ".ypt", 68, 71 },
	};

	std::string lower_ext(const fs::path& path)
	{
		std::string ext = path.extension().string();
		for (auto& c : ext)
			c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		return ext;
	}

	const VersionPair* find_pair(const std::string& ext)
	{
		for (const auto& pair : kVersions)
			if (ext == pair.ext)
				return &pair;
		return nullptr;
	}

	bool read_header(const fs::path& path, uint8_t out[16])
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		in.read(reinterpret_cast<char*>(out), 16);
		return in.gcount() == 16;
	}

	void append_hit(std::vector<rsccompat::Hit>& out, const std::string& pack,
		const std::string& file, int version, rsccompat::Status status)
	{
		if (status == rsccompat::Status::Ok)
			return;
		out.push_back({ pack, file, version, status });
	}

	void scan_one(const std::string& pack, const std::string& relative,
		const fs::path& full, bool gameIsEnhanced, std::vector<rsccompat::Hit>& out)
	{
		int version = -1;
		const rsccompat::Status status = rsccompat::CheckFile(full, gameIsEnhanced, &version);
		if (status == rsccompat::Status::Ok)
			return;

		append_hit(out, pack, relative, version, status);

		const char* need = status == rsccompat::Status::NeedsGen9
			? "Gen9 (Enhanced)" : status == rsccompat::Status::NeedsLegacy
			? "Gen8 (Legacy)" : "a readable RSC7 header";
		logger::write("fivem", "[compat] %s: %s is incompatible (version %d); needs %s",
			pack.c_str(), relative.c_str(), version, need);
	}
}

namespace rsccompat
{
	int VersionFromFlags(uint32_t systemFlags, uint32_t graphicsFlags)
	{
		const uint32_t sv = (systemFlags >> 28) & 0xF;
		const uint32_t gv = (graphicsFlags >> 28) & 0xF;
		return static_cast<int>((sv << 4) + gv);
	}

	int ReadVersion(const fs::path& path)
	{
		uint8_t header[16] = {};
		if (!read_header(path, header))
			return -1;
		uint32_t magic = 0;
		uint32_t systemFlags = 0;
		uint32_t graphicsFlags = 0;
		std::memcpy(&magic, header, 4);
		std::memcpy(&systemFlags, header + 8, 4);
		std::memcpy(&graphicsFlags, header + 12, 4);
		if (magic != kRsc7Magic)
			return -1;
		return VersionFromFlags(systemFlags, graphicsFlags);
	}

	Status CheckFile(const fs::path& path, bool gameIsEnhanced, int* versionOut)
	{
		if (versionOut)
			*versionOut = -1;

		const VersionPair* pair = find_pair(lower_ext(path));
		if (!pair)
			return Status::Ok;

		std::error_code ec;
		if (!fs::is_regular_file(path, ec))
			return Status::Ok;

		const int version = ReadVersion(path);
		if (versionOut)
			*versionOut = version;
		if (version < 0)
		{
			// Missing or non-RSC7 payload: not something this check can judge.
			return Status::Ok;
		}

		if (gameIsEnhanced)
		{
			if (version == pair->legacy)
				return Status::NeedsGen9;
			return Status::Ok; // Gen9 or an unknown version we cannot classify
		}

		if (version == pair->gen9)
			return Status::NeedsLegacy;
		return Status::Ok;
	}

	void ScanFiveMAssets(const std::string& packName, const fs::path& resourceDir,
		const std::vector<std::string>& relativePaths, bool gameIsEnhanced,
		std::vector<Hit>& out)
	{
		for (const auto& relative : relativePaths)
		{
			if (relative.empty())
				continue;
			scan_one(packName, relative, resourceDir / fs::path(relative), gameIsEnhanced, out);
		}
	}

	void ScanTree(const std::string& packName, const fs::path& root, bool gameIsEnhanced,
		std::vector<Hit>& out)
	{
		std::error_code ec;
		if (!fs::exists(root, ec))
			return;

		for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		{
			if (!it->is_regular_file(ec))
				continue;
			const fs::path& full = it->path();
			if (!find_pair(lower_ext(full)))
				continue;

			std::error_code relEc;
			fs::path rel = fs::relative(full, root, relEc);
			std::string relative = relEc ? full.filename().string() : rel.generic_string();
			scan_one(packName, relative, full, gameIsEnhanced, out);
		}
	}
}
