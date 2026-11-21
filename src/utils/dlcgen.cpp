#include "main.h"
#include "utils/dlcgen.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <unordered_map>

/*
	content.xml / setup2.xml synthesis.

	The two files are copied from the shape of a working add-on pack, because
	the engine validates more of them than the obvious fields suggest: every
	data file is declared in <dataFiles> *and* enabled from the change set, and
	the streamed archive is registered as an RPF_FILE for %PLATFORM% rather than
	for a hard-coded x64.
*/

namespace
{
	namespace fs = std::filesystem;

	// Bumped whenever the generated layout changes, so an existing cache is
	// rebuilt instead of being reused with stale content.
	// 除非有重大改动，否则不要变更版本号，测试的时候优先清空缓存
	//   4: streamed assets are flattened to the archive root, because a
	//      `stream/` prefix in the entry path stops models from streaming.
	//   5: the streamed assets live in stream.rpf, not vehicles.rpf.
	//   6: setup2.xml is written with an order above every stock pack, so a
	//      layout or handling definition owned by a retail DLC exists before
	//      this pack's vehicles are registered.
	//   7: this_is_a_map resources become EXTRACONTENT_LEVEL_PACK packs with
	//      metadata.rpf / mapdata.rpf split out; dumping _manifest.ymf into a
	//      plain stream.rpf crashed Enhanced while parsing map metadata.
	//   8: map setup2/content match a working OpenIV level pack (change-set
	//      groups, MPHEIST mp-stream name, models/metadata flags); maps also
	//      mount earlier via dlclist ordering.
	//      pdoccl/strmfile overlays also start enabled with CONTENTS so
	//      stock-named replacements do not UAF on leave/re-enter.
	constexpr int kGeneratorVersion = 8;

	// The sequence number written to setup2.xml's <order>.
	//
	// This is not cosmetic. Content registration follows this value, and a
	// vehicle resolves its <layout> when *it* is registered, not when the player
	// gets in. The stock packs run from -1 to 57, and a layout is often owned by
	// a late DLC -- LAYOUT_VAN_YOUGA2 only exists in mpbiker, at order 23. A
	// generated pack left at the old hard-coded 9 therefore registered its
	// vehicles before that layout existed: the car spawned and drove, but the
	// player could not get in, with no error anywhere.
	//
	// Everything this loader generates goes above the stock range so it sees the
	// complete set of retail definitions.
	constexpr int kDlcOrder = 100;

	// The archive that carries the streamed assets, under %PLATFORM%.
	//
	// The name is arbitrary -- the engine mounts whatever content.xml declares --
	// so it is deliberately generic. resources here are not always vehicles: a
	// map or a prop pack streams exactly the same way, and calling its archive
	// vehicles.rpf only misleads whoever opens the cache folder.
	//
	// What does matter is that the assets sit at the *root* of this archive. A
	// streaming object is identified by its path relative to the packfile, so
	// `stream/nisa31.yft` registers under that whole name and never answers a
	// lookup for `nisa31`.
	const char* kStreamArchive = "stream.rpf";

	// Map packs mirror a working retail/OpenIV level DLC (see ron_247shop):
	// models, metadata (CONTENTS_DLC_MAP_DATA), occlusion overlay, story strm
	// overlay, and heist/mp strm — all under %PLATFORM%/levels/gta5/.
	const char* kMapLevelsDir = "levels/gta5";
	const char* kMapModelsArchive = "models.rpf";
	const char* kMapMetadataArchive = "metadata.rpf";
	const char* kMapPdocclArchive = "pdoccl.rpf";
	const char* kMapStrmfileArchive = "strmfile.rpf";
	const char* kMapMpstrmArchive = "mpstrm.rpf";
	constexpr int kMapDlcOrder = 36;

	constexpr size_t npos = std::string::npos;

	bool ends_with_ci(const std::string& value, const char* suffix)
	{
		const size_t suffixLen = std::strlen(suffix);
		if (value.size() < suffixLen)
			return false;
		for (size_t i = 0; i < suffixLen; ++i)
		{
			const unsigned char a = static_cast<unsigned char>(value[value.size() - suffixLen + i]);
			const unsigned char b = static_cast<unsigned char>(suffix[i]);
			if (::tolower(a) != ::tolower(b))
				return false;
		}
		return true;
	}

	std::string lower_copy(std::string value)
	{
		for (auto& c : value)
			c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		return value;
	}

	std::string trim_copy(const std::string& value)
	{
		const size_t begin = value.find_first_not_of(" \t\r\n");
		if (begin == std::string::npos)
			return {};
		const size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(begin, end - begin + 1);
	}

	std::string xml_escape(const std::string& value)
	{
		std::string out;
		out.reserve(value.size());
		for (char c : value)
		{
			switch (c)
			{
			case '&': out += "&amp;"; break;
			case '<': out += "&lt;"; break;
			case '>': out += "&gt;"; break;
			case '"': out += "&quot;"; break;
			case '\'': out += "&apos;"; break;
			default: out.push_back(c); break;
			}
		}
		return out;
	}

	// FiveM and retail disagree about a few type names; anything not listed is
	// passed through unchanged, which is correct for most of them.
	std::string map_file_type(const std::string& type)
	{
		if (_stricmp(type.c_str(), "DLCTEXT_FILE") == 0)
			return "TEXTFILE_METAFILE";
		if (_stricmp(type.c_str(), "DLCTEXT") == 0)
			return "TEXTFILE_METAFILE";
		if (_stricmp(type.c_str(), "VEHICLE_SHOP_FILE") == 0)
			return "VEHICLE_SHOP_FILE";
		return type;
	}

	// Hard links keep the staging tree from being a second full copy of the
	// resource. The unpack tree and the cache normally share a volume; a copy
	// is the fallback when they do not.
	bool stage_file(const fs::path& source, const fs::path& target)
	{
		std::error_code ec;
		fs::create_directories(target.parent_path(), ec);
		fs::remove(target, ec);

		if (::CreateHardLinkW(target.c_str(), source.c_str(), nullptr))
			return true;

		ec.clear();
		fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
		return !ec;
	}

	bool write_text(const fs::path& path, const std::string& content)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);

		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(content.data(), static_cast<std::streamsize>(content.size()));
		return static_cast<bool>(out);
	}

	// A leading dot is a Unix convention; Windows only hides a directory that
	// carries FILE_ATTRIBUTE_HIDDEN, so the attribute is set on the generated
	// root too. Failure is cosmetic and never worth reporting.
	void mark_hidden(const fs::path& dir)
	{
		const DWORD attributes = ::GetFileAttributesW(dir.c_str());
		if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_HIDDEN))
			return;
		::SetFileAttributesW(dir.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
	}

	// Where the engine expects an add-on pack: under the mods layer, at the same
	// path a hand-installed DLC would use.
	//
	// Publishing here rather than serving the pack through the device overlay is
	// deliberate. An archive reached through a rewritten device path
	// (`unpack-mods:/...`) opens once and is then rejected on the engine's second
	// mount pass, so its vehicles register in dlclist but never become spawnable.
	// The stock path is opened through the normal reader, which is what makes the
	// models stream.
	fs::path published_path(const std::string& pack)
	{
		return config::mods_root() / "update" / "x64" / "dlcpacks" / pack / "dlc.rpf";
	}

	// Mirrors the archive into the mods tree. A hard link is used so the bytes
	// live once, in the hidden cache layer, and the pack costs no disk space.
	bool publish(const std::string& pack, const fs::path& archive, std::string& error)
	{
		std::error_code ec;
		const fs::path target = published_path(pack);
		fs::create_directories(target.parent_path(), ec);

		// A rebuild replaces the archive, so the link is refreshed every start
		// instead of being trusted: a link to a deleted inode would serve the
		// previous build forever.
		fs::remove(target, ec);

		if (::CreateHardLinkW(target.c_str(), archive.c_str(), nullptr))
			return true;

		// Different volume, or a filesystem without hard links. A copy is still
		// better than a pack the game cannot mount.
		ec.clear();
		fs::copy_file(archive, target, fs::copy_options::overwrite_existing, ec);
		if (!ec)
			return true;

		error = "cannot publish " + archive.string() + " to " + target.string() +
			": " + ec.message();
		return false;
	}

	// Removes packs this loader published earlier that no longer have a resource.
	// A leftover link is worse than untidy: the engine mounts an archive whose
	// dlclist entry was pruned, and a failed mount poisons the whole pass.
	void unpublish_missing(const std::set<std::string>& packs)
	{
		std::error_code ec;
		const fs::path root = config::mods_root() / "update" / "x64" / "dlcpacks";
		if (!fs::is_directory(root, ec))
			return;

		for (const auto& entry : fs::directory_iterator(root, ec))
		{
			if (!entry.is_directory(ec))
				continue;
			const std::string name = entry.path().filename().string();
			if (name.rfind("ziv_", 0) != 0 || packs.count(name))
				continue;
			fs::remove_all(entry.path(), ec);
			logger::write("fivem", "[gen] %s: no resource left, removed the published pack",
				name.c_str());
		}
	}

	std::string timestamp()
	{
		std::time_t now = std::time(nullptr);
		std::tm local{};
#ifdef _WIN32
		localtime_s(&local, &now);
#else
		localtime_r(&now, &local);
#endif
		char text[32];
		std::snprintf(text, sizeof(text), "%02d/%02d/%04d %02d:%02d:%02d",
			local.tm_mday, local.tm_mon + 1, local.tm_year + 1900,
			local.tm_hour, local.tm_min, local.tm_sec);
		return text;
	}

	struct StagedDataFile
	{
		std::string type;
		std::string name;   // pack-relative path used in content.xml
	};

	// One pack-internal name per staged file. Two metadata files sharing a
	// basename is common enough (several `vehicles.meta`) that collisions are
	// resolved instead of overwriting each other.
	std::string unique_name(const std::string& basename, std::set<std::string>& used)
	{
		std::string candidate = "data/" + basename;
		if (used.insert(lower_copy(candidate)).second)
			return candidate;

		const size_t dot = basename.find_last_of('.');
		const std::string stem = dot == npos ? basename : basename.substr(0, dot);
		const std::string ext = dot == npos ? std::string() : basename.substr(dot);

		for (int index = 2; index < 1000; ++index)
		{
			candidate = "data/" + stem + "_" + std::to_string(index) + ext;
			if (used.insert(lower_copy(candidate)).second)
				return candidate;
		}
		return {}; // unreachable in practice
	}

	// Names of every layout the retail game defines. Loaded from the embedded
	// resource once; see resources/stocklayouts.txt for why the list has to cover
	// the DLCs and not just the base game.
	const std::set<std::string>& stock_layouts()
	{
		static const std::set<std::string> names = [] {
			std::set<std::string> out;

			HMODULE module = nullptr;
			if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
					GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(&stock_layouts), &module))
				return out;

			HRSRC found = ::FindResourceA(module, "STOCKLAYOUTS", RT_RCDATA);
			if (!found)
			{
				logger::write("fivem", "[gen] the stock layout list is missing from the build");
				return out;
			}

			HGLOBAL loaded = ::LoadResource(module, found);
			const char* data = loaded
				? static_cast<const char*>(::LockResource(loaded)) : nullptr;
			const DWORD size = ::SizeofResource(module, found);
			if (!data || size == 0)
				return out;

			std::string text(data, size);
			std::istringstream lines(text);
			std::string line;
			while (std::getline(lines, line))
			{
				if (line.empty() || line[0] == '#')
					continue;
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				if (!line.empty())
					out.insert(line);
			}
			return out;
		}();
		return names;
	}

	// Collects every layout the pack defines itself, so the check below can tell
	// "defined by this pack" apart from "defined nowhere".
	void collect_defined_layouts(const fs::path& stage,
		const std::vector<StagedDataFile>& staged, std::set<std::string>& out)
	{
		for (const auto& file : staged)
		{
			if (_stricmp(file.type.c_str(), "VEHICLE_LAYOUTS_FILE") != 0)
				continue;

			std::ifstream in(stage / fs::path(file.name), std::ios::binary);
			if (!in)
				continue;
			std::string text((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());

			size_t cursor = 0;
			while ((cursor = text.find("<Name>", cursor)) != npos)
			{
				const size_t start = cursor + 6;
				const size_t end = text.find("</Name>", start);
				if (end == npos)
					break;
				out.insert(trim_copy(text.substr(start, end - start)));
				cursor = end;
			}
		}
	}

	// Warns about every <layout> a staged vehicles.meta references but nothing
	// defines. Such a vehicle spawns and drives yet cannot be entered, and the
	// game reports nothing, so this is the only place the reason can surface.
	void check_layout_refs(const std::string& name, const fs::path& stage,
		const std::vector<StagedDataFile>& staged)
	{
		const std::set<std::string>& stock = stock_layouts();
		if (stock.empty())
			return;

		std::set<std::string> definedHere;
		collect_defined_layouts(stage, staged, definedHere);

		std::set<std::string> reported;
		for (const auto& file : staged)
		{
			if (_stricmp(file.type.c_str(), "VEHICLE_METADATA_FILE") != 0)
				continue;

			std::ifstream in(stage / fs::path(file.name), std::ios::binary);
			if (!in)
				continue;
			std::string text((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());

			size_t cursor = 0;
			const std::string open = "<layout>";
			while ((cursor = text.find(open, cursor)) != npos)
			{
				const size_t start = cursor + open.size();
				const size_t end = text.find("</layout>", start);
				if (end == npos)
					break;
				const std::string layout = trim_copy(text.substr(start, end - start));
				cursor = end;

				if (layout.empty() || stock.count(layout) || definedHere.count(layout))
					continue;
				if (!reported.insert(layout).second)
					continue;

				logger::write("fivem", "[gen] %s: layout %s is defined neither by the game "
					"nor by this resource; the vehicle will spawn but cannot be entered",
					name.c_str(), layout.c_str());
			}
		}
	}

	std::string build_setup2(const std::string& pack)
	{
		std::ostringstream xml;
		xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		xml << "<SSetupData>\n";
		xml << "  <deviceName>dlc_" << xml_escape(pack) << "</deviceName>\n";
		xml << "  <datFile>content.xml</datFile>\n";
		xml << "  <timeStamp>" << timestamp() << "</timeStamp>\n";
		xml << "  <nameHash>" << xml_escape(pack) << "</nameHash>\n";
		xml << "  <contentChangeSetGroups>\n";
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_STARTUP</NameHash>\n";
		xml << "      <ContentChangeSets>\n";
		xml << "        <Item>" << xml_escape(pack) << "_AUTOGEN</Item>\n";
		xml << "      </ContentChangeSets>\n";
		xml << "    </Item>\n";
		xml << "  </contentChangeSetGroups>\n";
		xml << "  <type>EXTRACONTENT_COMPAT_PACK</type>\n";
		xml << "  <order value=\"" << kDlcOrder << "\" />\n";
		xml << "</SSetupData>\n";
		return xml.str();
	}

	std::string build_content(const std::string& pack, const std::vector<StagedDataFile>& dataFiles,
		bool hasStream)
	{
		const std::string device = "dlc_" + pack;

		std::ostringstream xml;
		xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		xml << "<CDataFileMgr__ContentsOfDataFileXml>\n";
		xml << "  <disabledFiles />\n";
		xml << "  <includedXmlFiles />\n";
		xml << "  <includedDataFiles />\n";
		xml << "  <dataFiles>\n";

		for (const auto& file : dataFiles)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(device) << ":/" << xml_escape(file.name)
				<< "</filename>\n";
			xml << "      <fileType>" << xml_escape(file.type) << "</fileType>\n";
			xml << "      <overlay value=\"false\" />\n";
			xml << "      <disabled value=\"true\" />\n";
			xml << "      <persistent value=\"false\" />\n";
			xml << "    </Item>\n";
		}

		if (hasStream)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(device) << ":/%PLATFORM%/"
				<< kStreamArchive << "</filename>\n";
			xml << "      <fileType>RPF_FILE</fileType>\n";
			xml << "      <overlay value=\"false\" />\n";
			xml << "      <disabled value=\"true\" />\n";
			xml << "      <persistent value=\"true\" />\n";
			xml << "    </Item>\n";
		}

		xml << "  </dataFiles>\n";
		xml << "  <contentChangeSets>\n";
		xml << "    <Item>\n";
		xml << "      <changeSetName>" << xml_escape(pack) << "_AUTOGEN</changeSetName>\n";
		xml << "      <filesToDisable />\n";
		xml << "      <filesToEnable>\n";

		for (const auto& file : dataFiles)
			xml << "        <Item>" << xml_escape(device) << ":/" << xml_escape(file.name)
				<< "</Item>\n";
		if (hasStream)
			xml << "        <Item>" << xml_escape(device) << ":/%PLATFORM%/"
				<< kStreamArchive << "</Item>\n";

		xml << "      </filesToEnable>\n";
		xml << "      <txdToLoad />\n";
		xml << "      <txdToUnload />\n";
		xml << "      <residentResources />\n";
		xml << "      <unregisterResources />\n";
		xml << "    </Item>\n";
		xml << "  </contentChangeSets>\n";
		xml << "  <patchFiles />\n";
		xml << "</CDataFileMgr__ContentsOfDataFileXml>\n";
		return xml.str();
	}

	// Buckets match the working ron_247shop OpenIV layout.
	enum class MapBucket
	{
		Models,    // *.rpf models/collisions/textures
		Metadata,  // _manifest.ymf, .ytyp, milo/mlo, optional meta-only ymap
		Pdoccl,    // occlusion ymap overlay
		Strmfile,  // story-mode *_strm_*.ymap overlay
		Mpstrm,    // hei_* / MP strm ymap
	};

	bool is_interior_ymap(const std::string& lowerBasename)
	{
		return lowerBasename.find("milo") != npos || lowerBasename.find("mlo") != npos;
	}

	// Files named `_metadata_<name>` are staged into metadata.rpf as `<name>` —
	// used when the same ymap exists in both metadata and pdoccl with different
	// bytes (ron_247shop's ch1_occl_03).
	std::string map_stage_name(const std::string& basename)
	{
		const std::string lower = lower_copy(basename);
		const char* prefix = "_metadata_";
		const size_t prefixLen = std::strlen(prefix);
		if (lower.size() > prefixLen && lower.compare(0, prefixLen, prefix) == 0)
			return basename.substr(prefixLen);
		return basename;
	}

	MapBucket classify_map_asset(const std::string& basename)
	{
		const std::string lower = lower_copy(basename);
		const std::string staged = lower_copy(map_stage_name(basename));

		if (ends_with_ci(lower, ".ymf") || ends_with_ci(lower, ".ytyp"))
			return MapBucket::Metadata;
		if (lower.rfind("_metadata_", 0) == 0)
			return MapBucket::Metadata;
		if (ends_with_ci(lower, ".ymap"))
		{
			if (is_interior_ymap(staged))
				return MapBucket::Metadata;
			if (staged.find("occl") != npos)
				return MapBucket::Pdoccl;
			if (staged.rfind("hei_", 0) == 0)
				return MapBucket::Mpstrm;
			return MapBucket::Strmfile;
		}
		return MapBucket::Models;
	}

	// Stable interior proxy base so two generated map packs do not start from
	// the same index. The absolute value only has to clear the stock range.
	uint32_t interior_start_from(const std::string& pack)
	{
		uint32_t hash = 2166136261u;
		for (unsigned char c : pack)
		{
			hash ^= c;
			hash *= 16777619u;
		}
		return 4000u + (hash % 50000u);
	}

	std::string interior_proxy_name(const std::string& basename)
	{
		std::string name = basename;
		if (ends_with_ci(name, ".ymap"))
			name.resize(name.size() - 5);
		return name;
	}

	std::string build_interior_proxies(const std::string& pack,
		const std::vector<std::string>& interiorNames)
	{
		std::ostringstream xml;
		xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		xml << "<SInteriorOrderData>\n";
		xml << "  <startFrom value=\"" << interior_start_from(pack) << "\" />\n";
		xml << "  <proxies>\n";
		for (const auto& name : interiorNames)
			xml << "    <Item>" << xml_escape(name) << "</Item>\n";
		xml << "  </proxies>\n";
		xml << "</SInteriorOrderData>\n";
		return xml.str();
	}

	std::string map_archive_path(const char* archive)
	{
		return std::string("%PLATFORM%/") + kMapLevelsDir + "/" + archive;
	}

	// Stock heist change-set name cityhills_01 map DLCs piggyback on. Referenced
	// from GROUP_MAP so mpstrm enables with the retail heist streaming pass.
	const char* kMapMpStreamChangeSet = "MPHEIST_GTA5_HILLS_CITYHILLS_01";

	// Copied from a known-good OpenIV map DLC (ron_247shop): LEVEL_PACK at the
	// stock map-pack order, with the same change-set groups the game expects.
	std::string build_map_setup2(const std::string& pack)
	{
		const std::string ccsInit = "CCS_" + pack + "_NG_INIT";
		const std::string ccsStreaming = "CCS_" + pack + "_NG_STREAMING";
		const std::string ccsStreamingMap = "CCS_" + pack + "_NG_STREAMING_MAP";
		const std::string ccsText = "CCS_" + pack + "_NG_TEXT";

		std::ostringstream xml;
		xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		xml << "<SSetupData>\n";
		xml << "  <deviceName>dlc_" << xml_escape(pack) << "</deviceName>\n";
		xml << "  <datFile>content.xml</datFile>\n";
		xml << "  <timeStamp>" << timestamp() << "</timeStamp>\n";
		xml << "  <nameHash>" << xml_escape(pack) << "</nameHash>\n";
		xml << "  <contentChangeSets />\n";
		xml << "  <contentChangeSetGroups>\n";
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_STARTUP</NameHash>\n";
		xml << "      <ContentChangeSets>\n";
		xml << "        <Item>" << xml_escape(pack) << "_AUTOGEN</Item>\n";
		xml << "      </ContentChangeSets>\n";
		xml << "    </Item>\n";
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_EARLY_ON</NameHash>\n";
		xml << "      <ContentChangeSets>\n";
		xml << "        <Item>" << xml_escape(ccsInit) << "</Item>\n";
		xml << "      </ContentChangeSets>\n";
		xml << "    </Item>\n";
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_MAP</NameHash>\n";
		xml << "      <ContentChangeSets>\n";
		xml << "        <Item>" << xml_escape(kMapMpStreamChangeSet) << "</Item>\n";
		xml << "      </ContentChangeSets>\n";
		xml << "    </Item>\n";
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_UPDATE_STREAMING</NameHash>\n";
		xml << "      <ContentChangeSets>\n";
		xml << "        <Item>" << xml_escape(ccsStreaming) << "</Item>\n";
		xml << "        <Item>" << xml_escape(ccsStreamingMap) << "</Item>\n";
		xml << "      </ContentChangeSets>\n";
		xml << "    </Item>\n";
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_UPDATE_TEXT</NameHash>\n";
		xml << "      <ContentChangeSets>\n";
		xml << "        <Item>" << xml_escape(ccsText) << "</Item>\n";
		xml << "      </ContentChangeSets>\n";
		xml << "    </Item>\n";
		xml << "  </contentChangeSetGroups>\n";
		xml << "  <type>EXTRACONTENT_LEVEL_PACK</type>\n";
		xml << "  <order value=\"" << kMapDlcOrder << "\" />\n";
		xml << "  <minorOrder value=\"0\" />\n";
		xml << "  <isLevelPack value=\"true\" />\n";
		xml << "</SSetupData>\n";
		return xml.str();
	}

	struct MapArchives
	{
		bool models = false;
		bool metadata = false;
		bool pdoccl = false;
		bool strmfile = false;
		bool mpstrm = false;
	};

	void write_map_rpf_item(std::ostringstream& xml, const std::string& device,
		const char* archive, bool disabled, bool overlay, bool locked,
		bool persistent, bool contentsMapData)
	{
		xml << "    <Item>\n";
		xml << "      <filename>" << xml_escape(device) << ":/"
			<< map_archive_path(archive) << "</filename>\n";
		xml << "      <fileType>RPF_FILE</fileType>\n";
		if (locked)
			xml << "      <locked value=\"true\" />\n";
		xml << "      <overlay value=\"" << (overlay ? "true" : "false") << "\" />\n";
		xml << "      <disabled value=\"" << (disabled ? "true" : "false") << "\" />\n";
		xml << "      <persistent value=\"" << (persistent ? "true" : "false") << "\" />\n";
		if (contentsMapData)
			xml << "      <contents>CONTENTS_DLC_MAP_DATA</contents>\n";
		xml << "    </Item>\n";
	}

	std::string build_map_content(const std::string& pack,
		const std::vector<StagedDataFile>& dataFiles, const MapArchives& archives)
	{
		const std::string device = "dlc_" + pack;

		std::ostringstream xml;
		xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		xml << "<CDataFileMgr__ContentsOfDataFileXml>\n";
		xml << "  <disabledFiles />\n";
		xml << "  <includedXmlFiles />\n";
		xml << "  <includedDataFiles />\n";
		xml << "  <dataFiles>\n";

		// Models/metadata and stock-name overlays (pdoccl/strmfile) start enabled.
		// Overlays that replace retail ymaps must stay resident: toggling them from
		// STREAMING_MAP alone lets the engine tear the replacement down with the
		// sector and crash on leave/re-enter. mpstrm still waits for GROUP_MAP /
		// MPHEIST like retail heist map DLCs.
		if (archives.models)
			write_map_rpf_item(xml, device, kMapModelsArchive, false, false, false, false, false);
		if (archives.metadata)
			write_map_rpf_item(xml, device, kMapMetadataArchive, false, false, false, false, true);
		if (archives.pdoccl)
			write_map_rpf_item(xml, device, kMapPdocclArchive, false, true, false, true, true);
		if (archives.strmfile)
			write_map_rpf_item(xml, device, kMapStrmfileArchive, false, true, true, true, true);
		if (archives.mpstrm)
			write_map_rpf_item(xml, device, kMapMpstrmArchive, true, false, false, true, true);

		for (const auto& file : dataFiles)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(device) << ":/" << xml_escape(file.name)
				<< "</filename>\n";
			xml << "      <fileType>" << xml_escape(file.type) << "</fileType>\n";
			xml << "      <overlay value=\"false\" />\n";
			xml << "      <disabled value=\"true\" />\n";
			const bool persistentScenario =
				_stricmp(file.type.c_str(), "SCENARIO_POINTS_FILE") == 0;
			xml << "      <persistent value=\"" << (persistentScenario ? "true" : "false")
				<< "\" />\n";
			xml << "    </Item>\n";
		}

		xml << "  </dataFiles>\n";
		xml << "  <contentChangeSets>\n";

		xml << "    <Item>\n";
		xml << "      <changeSetName>" << xml_escape(pack) << "_AUTOGEN</changeSetName>\n";
		xml << "      <filesToDisable />\n";
		xml << "      <filesToEnable>\n";
		for (const auto& file : dataFiles)
			xml << "        <Item>" << xml_escape(device) << ":/" << xml_escape(file.name)
				<< "</Item>\n";
		xml << "      </filesToEnable>\n";
		xml << "    </Item>\n";

		xml << "    <Item>\n";
		xml << "      <changeSetName>CCS_" << xml_escape(pack)
			<< "_NG_STREAMING_MAP</changeSetName>\n";
		xml << "      <filesToEnable>\n";
		if (archives.models)
			xml << "        <Item>" << xml_escape(device) << ":/"
				<< map_archive_path(kMapModelsArchive) << "</Item>\n";
		if (archives.metadata)
			xml << "        <Item>" << xml_escape(device) << ":/"
				<< map_archive_path(kMapMetadataArchive) << "</Item>\n";
		// pdoccl/strmfile are already enabled above; do not re-bind them here.
		xml << "      </filesToEnable>\n";
		xml << "      <executionConditions>\n";
		xml << "        <activeChangesetConditions>\n";
		xml << "        </activeChangesetConditions>\n";
		xml << "        <genericConditions>$level=MO_JIM_L11</genericConditions>\n";
		xml << "      </executionConditions>\n";
		xml << "    </Item>\n";

		if (archives.mpstrm)
		{
			xml << "    <Item>\n";
			xml << "      <changeSetName>" << xml_escape(kMapMpStreamChangeSet)
				<< "</changeSetName>\n";
			xml << "      <mapChangeSetData>\n";
			xml << "        <Item>\n";
			xml << "          <associatedMap>MO_JIM_L11</associatedMap>\n";
			xml << "          <filesToEnable>\n";
			xml << "            <Item>" << xml_escape(device) << ":/"
				<< map_archive_path(kMapMpstrmArchive) << "</Item>\n";
			xml << "          </filesToEnable>\n";
			xml << "        </Item>\n";
			xml << "      </mapChangeSetData>\n";
			xml << "      <requiresLoadingScreen value=\"true\" />\n";
			xml << "      <loadingScreenContext>LOADINGSCREEN_CONTEXT_LAST_FRAME"
				"</loadingScreenContext>\n";
			xml << "      <useCacheLoader value=\"true\" />\n";
			xml << "    </Item>\n";
		}

		xml << "  </contentChangeSets>\n";
		xml << "  <patchFiles />\n";
		xml << "</CDataFileMgr__ContentsOfDataFileXml>\n";
		return xml.str();
	}

	std::string read_stamp(const fs::path& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return {};
		std::string value;
		std::getline(in, value);
		return value;
	}

	void write_stamp(const fs::path& path, const std::string& signature)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (out)
			out << signature << "\n";
	}

	// The exact inputs the archive was built from, one line per file:
	// `<size> <mtime> <path>`. Path goes last because it may contain spaces.
	void write_sources(const fs::path& path, const fivem::Resource& resource)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return;
		for (const auto& source : resource.sources)
			out << source.size << ' ' << source.mtime << ' ' << source.path << '\n';
	}

	struct PreviousSource
	{
		uint64_t size = 0;
		uint64_t mtime = 0;
	};

	std::unordered_map<std::string, PreviousSource> read_sources(const fs::path& path)
	{
		std::unordered_map<std::string, PreviousSource> previous;

		std::ifstream in(path, std::ios::binary);
		std::string line;
		while (std::getline(in, line))
		{
			std::istringstream parser(line);
			PreviousSource entry;
			if (!(parser >> entry.size >> entry.mtime))
				continue;

			std::string rest;
			std::getline(parser, rest);
			const size_t start = rest.find_first_not_of(' ');
			if (start == std::string::npos)
				continue;

			previous[lower_copy(rest.substr(start))] = entry;
		}
		return previous;
	}

	// Explains a rebuild in the log. Without it a rebuild looks like the cache
	// simply did not work, and a rebuild triggered by a file the user did not
	// touch is impossible to diagnose.
	void log_source_changes(const fivem::Resource& resource, const fs::path& sourcesPath)
	{
		const auto previous = read_sources(sourcesPath);
		if (previous.empty())
		{
			logger::write("fivem", "[gen] %s: no source record yet, rebuilding",
				resource.name.c_str());
			return;
		}

		int reported = 0;
		for (const auto& source : resource.sources)
		{
			const auto it = previous.find(lower_copy(source.path));
			if (it == previous.end())
			{
				if (reported < 5)
				{
					logger::write("fivem", "[gen] %s: %s is new, rebuilding",
						resource.name.c_str(), source.path.c_str());
					++reported;
				}
				continue;
			}

			const PreviousSource& before = it->second;
			if (before.size == source.size && before.mtime == source.mtime)
				continue;

			if (reported < 5)
			{
				const char* reason = before.size != source.size ? "size" : "date";
				logger::write("fivem", "[gen] %s: %s changed (%s: %llu -> %llu), rebuilding",
					resource.name.c_str(), source.path.c_str(), reason,
					static_cast<unsigned long long>(
						before.size != source.size ? before.size : before.mtime),
					static_cast<unsigned long long>(
						before.size != source.size ? source.size : source.mtime));
				++reported;
			}
		}

		// A file that disappeared cannot be seen above: iterate the previous
		// record and look for paths the current scan no longer carries.
		std::set<std::string> current;
		for (const auto& source : resource.sources)
			current.insert(lower_copy(source.path));

		for (const auto& entry : previous)
		{
			if (current.count(entry.first))
				continue;
			if (reported < 5)
			{
				logger::write("fivem", "[gen] %s: %s is gone, rebuilding",
					resource.name.c_str(), entry.first.c_str());
				++reported;
			}
		}

		if (reported == 0)
			logger::write("fivem", "[gen] %s: generator layout changed, rebuilding",
				resource.name.c_str());
	}
}

namespace dlcgen
{
	BuildResult Build(const fivem::Resource& resource, std::string& error)
	{
		std::error_code ec;

		const fs::path outputDir = config::generated_root() / "dlcpacks" / resource.pack;
		const fs::path outputRpf = outputDir / "dlc.rpf";
		const fs::path stampPath = outputDir / "dlc.stamp";
		const fs::path sourcesPath = outputDir / "dlc.sources";

		// The cache layer is created and hidden before the stamp check, so a
		// reused pack also gets its folder hidden again if someone unhid it.
		fs::create_directories(outputDir, ec);
		mark_hidden(config::generated_root());

		// The signature covers every input file's size and write time, so an
		// edited, added or deleted source forces a rebuild on the next start.
		const std::string signature = resource.signature + "-gen" + std::to_string(kGeneratorVersion);
		if (fs::is_regular_file(outputRpf, ec) && read_stamp(stampPath) == signature)
		{
			// The link in the mods tree is what the engine mounts, and it can be
			// missing even when the cache is fine: someone deleted it, or the
			// cache was moved between volumes.
			std::string publishError;
			if (!publish(resource.pack, outputRpf, publishError))
				logger::write("fivem", "[gen] %s: %s", resource.name.c_str(),
					publishError.c_str());
			return BuildResult::Reused;
		}

		if (fs::is_regular_file(outputRpf, ec))
			log_source_changes(resource, sourcesPath);
		else if (resource.isMap)
			logger::write("fivem", "[gen] %s: no archive yet, building map level pack",
				resource.name.c_str());
		else
			logger::write("fivem", "[gen] %s: no archive yet, building", resource.name.c_str());

		const fs::path stage = config::cache_root() / "fivem" / resource.pack / "stage";
		fs::remove_all(stage, ec);
		fs::create_directories(stage, ec);

		std::vector<StagedDataFile> stagedFiles;
		std::set<std::string> usedNames;
		bool hasStream = false;
		MapArchives mapArchives;

		for (const auto& file : resource.dataFiles)
		{
			const fs::path source = resource.dir / fs::path(file.path);
			if (!fs::is_regular_file(source, ec))
			{
				logger::write("fivem", "[gen] %s: missing %s", resource.name.c_str(),
					file.path.c_str());
				continue;
			}

			const std::string type = map_file_type(file.type);
			const std::string relative = file.path;

			// These types are addressed by pack-relative path in content.xml
			// (retail map DLCs keep common/data/..., ityp keeps stream/...).
			if (_stricmp(type.c_str(), "DLC_ITYP_REQUEST") == 0 ||
				_stricmp(type.c_str(), "INTERIOR_PROXY_ORDER_FILE") == 0 ||
				_stricmp(type.c_str(), "SCENARIO_POINTS_FILE") == 0)
			{
				if (stage_file(source, stage / fs::path(relative)))
					stagedFiles.push_back({ type, relative });
				continue;
			}

			const std::string basename = fs::path(relative).filename().string();
			const std::string target = unique_name(basename, usedNames);
			if (target.empty())
				continue;
			if (stage_file(source, stage / fs::path(target)))
				stagedFiles.push_back({ type, target });
		}

		std::set<std::string> usedAssets;
		std::vector<std::string> miloProxies;
		bool hasInteriorProxyFile = false;
		for (const auto& file : stagedFiles)
		{
			if (_stricmp(file.type.c_str(), "INTERIOR_PROXY_ORDER_FILE") == 0)
				hasInteriorProxyFile = true;
		}

		if (resource.isMap)
		{
			// Layout copied from a working OpenIV map DLC (ron_247shop).
			const fs::path levelsRoot = stage / "x64" / kMapLevelsDir;
			const fs::path modelsRoot = levelsRoot / kMapModelsArchive;
			const fs::path metadataRoot = levelsRoot / kMapMetadataArchive;
			const fs::path pdocclRoot = levelsRoot / kMapPdocclArchive;
			const fs::path strmfileRoot = levelsRoot / kMapStrmfileArchive;
			const fs::path mpstrmRoot = levelsRoot / kMapMpstrmArchive;
			std::set<std::string> usedStageNames;

			for (const auto& asset : resource.assets)
			{
				const fs::path source = resource.dir / fs::path(asset);
				if (!fs::is_regular_file(source, ec))
					continue;

				const std::string basename = fs::path(asset).filename().string();
				if (basename.empty())
					continue;
				if (!usedAssets.insert(lower_copy(basename)).second)
				{
					logger::write("fivem", "[gen] %s: %s and an earlier file share the name %s; "
						"keeping the first", resource.name.c_str(), asset.c_str(),
						basename.c_str());
					continue;
				}

				const std::string stagedName = map_stage_name(basename);
				const MapBucket bucket = classify_map_asset(basename);
				fs::path targetRoot = modelsRoot;
				switch (bucket)
				{
				case MapBucket::Metadata: targetRoot = metadataRoot; break;
				case MapBucket::Pdoccl: targetRoot = pdocclRoot; break;
				case MapBucket::Strmfile: targetRoot = strmfileRoot; break;
				case MapBucket::Mpstrm: targetRoot = mpstrmRoot; break;
				case MapBucket::Models: default: break;
				}

				// Same output name in one archive would collide (e.g. two occl
				// sources both wanting ch1_occl_03.ymap in different RPFs is fine;
				// two into the same RPF is not).
				const std::string stageKey = lower_copy(
					std::to_string(static_cast<int>(bucket)) + "|" + stagedName);
				if (!usedStageNames.insert(stageKey).second)
				{
					logger::write("fivem", "[gen] %s: skip %s — %s already staged in that archive",
						resource.name.c_str(), asset.c_str(), stagedName.c_str());
					continue;
				}

				if (!stage_file(source, targetRoot / fs::path(stagedName)))
					continue;

				switch (bucket)
				{
				case MapBucket::Metadata:
					mapArchives.metadata = true;
					if (ends_with_ci(stagedName, ".ymap") &&
						is_interior_ymap(lower_copy(stagedName)))
						miloProxies.push_back(interior_proxy_name(stagedName));
					break;
				case MapBucket::Pdoccl: mapArchives.pdoccl = true; break;
				case MapBucket::Strmfile: mapArchives.strmfile = true; break;
				case MapBucket::Mpstrm: mapArchives.mpstrm = true; break;
				case MapBucket::Models: mapArchives.models = true; break;
				}
			}

			if (!hasInteriorProxyFile && !miloProxies.empty())
			{
				const std::string relative = "common/data/interiorproxies.meta";
				if (write_text(stage / fs::path(relative),
						build_interior_proxies(resource.pack, miloProxies)))
				{
					stagedFiles.push_back({ "INTERIOR_PROXY_ORDER_FILE", relative });
					hasInteriorProxyFile = true;
					logger::write("fivem", "[gen] %s: wrote interiorproxies.meta for %zu interior(s)",
						resource.name.c_str(), miloProxies.size());
				}
			}

			logger::write("fivem",
				"[gen] %s: map split models=%d metadata=%d pdoccl=%d strm=%d mpstrm=%d data=%zu",
				resource.name.c_str(),
				mapArchives.models ? 1 : 0, mapArchives.metadata ? 1 : 0,
				mapArchives.pdoccl ? 1 : 0, mapArchives.strmfile ? 1 : 0,
				mapArchives.mpstrm ? 1 : 0, stagedFiles.size());
		}
		else
		{
			// Streamed assets are flattened into the archive root; see
			// kStreamArchive for why the prefix has to go and why the name is
			// generic. This is the vehicle / prop path and must stay unchanged.
			const fs::path streamRoot = stage / "x64" / kStreamArchive;
			for (const auto& asset : resource.assets)
			{
				const fs::path source = resource.dir / fs::path(asset);
				if (!fs::is_regular_file(source, ec))
					continue;

				const std::string basename = fs::path(asset).filename().string();
				if (basename.empty())
					continue;
				if (!usedAssets.insert(lower_copy(basename)).second)
				{
					logger::write("fivem", "[gen] %s: %s and an earlier file share the name %s; "
						"keeping the first", resource.name.c_str(), asset.c_str(),
						basename.c_str());
					continue;
				}

				if (!stage_file(source, streamRoot / fs::path(basename)))
					continue;
				hasStream = true;
			}
		}

		if (stagedFiles.empty() && !hasStream && !mapArchives.models &&
			!mapArchives.metadata && !mapArchives.pdoccl && !mapArchives.strmfile &&
			!mapArchives.mpstrm)
		{
			error = "nothing to package (no data files and no streamed assets)";
			return BuildResult::Failed;
		}

		if (!resource.isMap)
		{
			// A layout reference that resolves to nothing is invisible in game,
			// so it is worth a line here even though it does not stop the build.
			check_layout_refs(resource.name, stage, stagedFiles);
		}

		const std::string setupXml = resource.isMap
			? build_map_setup2(resource.pack)
			: build_setup2(resource.pack);
		const std::string contentXml = resource.isMap
			? build_map_content(resource.pack, stagedFiles, mapArchives)
			: build_content(resource.pack, stagedFiles, hasStream);

		if (!write_text(stage / "setup2.xml", setupXml) ||
			!write_text(stage / "content.xml", contentXml))
		{
			error = "cannot write setup2.xml/content.xml under " + stage.string();
			return BuildResult::Failed;
		}

		std::string buildError;
		if (!loosepack::BuildTo(stage, outputRpf, buildError))
		{
			error = "archive: " + buildError;
			return BuildResult::Failed;
		}

		write_stamp(stampPath, signature);
		write_sources(sourcesPath, resource);

		std::string publishError;
		if (!publish(resource.pack, outputRpf, publishError))
			logger::write("fivem", "[gen] %s: %s", resource.name.c_str(), publishError.c_str());

		// Marks the folder as generated so it is not mistaken for a hand-made
		// pack when someone looks into the cache layer.
		write_text(outputDir / "generated.txt",
			std::string("Generated by ZeroIV-Loader from a FiveM resource.\n") +
			(resource.isMap ? "Kind: map level pack (this_is_a_map).\n"
				: "Kind: add-on / vehicle pack.\n") +
			"Source: " + resource.dir.string() + "\n"
			"Delete the whole .cache folder to force a rebuild.\n");

		// Nothing else reads the staging tree once the archive exists.
		fs::remove_all(stage, ec);

		return BuildResult::Built;
	}

	void Prune(const std::vector<std::string>& packs)
	{
		unpublish_missing(std::set<std::string>(packs.begin(), packs.end()));
	}
}
