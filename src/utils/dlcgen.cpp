#include "main.h"
#include "utils/dlcgen.h"

#include <algorithm>
#include <cctype>
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
	//   4: streamed assets are flattened to the archive root, because a
	//      `stream/` prefix in the entry path stops models from streaming.
	//   5: the streamed assets live in stream.rpf, not vehicles.rpf.
	//   6: setup2.xml is written with an order above every stock pack, so a
	//      layout or handling definition owned by a retail DLC exists before
	//      this pack's vehicles are registered.
	//   7: each stream/ directory that holds assets becomes its own nested
	//      RPF (sibling under %PLATFORM%), so multiple `_manifest.ymf` files
	//      survive; map packs get CONTENT_FLAG_MAP_RESOURCE.
	//   8: skip packaging `_manifest.ymf`. Story-mode DLC mount of a stream
	//      RPF that carries CPackFileMetaData null-calls inside the engine
	//      (GTA5+0x941701); FiveM loads those manifests itself, SP does not.
	//   9: skip stream/lodlights (and *lodlights* / *distlodlights* ymaps).
	//      Incomplete stock LOD-light replacements are a common story-mode
	//      crash during world metadata load (GTA5+0x654B44).
	//  10: for this_is_a_map packs, also skip vanilla-named map overrides
	//      (apa_/bt1_/ss1_/hei_/v_carmod/…); keep ajaxon_* and gtaprops only.
	//  11: map packs no longer keep gtaprops/ or vw_/xs_ DLC prop dumps —
	//      re-registering those ytyps under CONTENT_FLAG_MAP_RESOURCE AVs
	//      during city metadata load (GTA5+0x653C8F). Custom ajaxon_* only.
	//  12: map packs skip *.ymap, INTERIOR_PROXY_ORDER_FILE, and
	//      CONTENT_FLAG_MAP_RESOURCE. Custom milo ymaps still AV at the same
	//      city-metadata site without matching exterior replacements; ship
	//      drawables/cols/ytyp only until placement is safe.
	//  13: (reverted) temporary unmount of map packs for isolation.
	//  14: map packs use the verified SP layout: LEVEL_PACK + isLevelPack,
	//      map_props.rpf (models/ytyp) + map_meta.rpf (ymap/ybn/ymf with
	//      CONTENTS_DLC_MAP_DATA), GROUP_UPDATE_STREAMING + $level=MO_JIM_L11.
	//      Vehicle-style flat stream RPFs were the city-metadata AV cause.
	constexpr int kGeneratorVersion = 14;

	// Relative paths inside the pack device for story-mode map archives.
	const char* kMapPropsArchive = "levels/gta5/props/map_props.rpf";
	const char* kMapMetaArchive = "levels/gta5/props/map_meta.rpf";

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

	// Bucket for assets that sit directly under stream/ (or outside stream/).
	// Nested folders get reversible escaped names instead; see bucket_name().
	const char* kRootStreamBucket = "stream";

	// Soft cap on the escaped bucket stem before we fall back to a hash name.
	// Keeps the RPF TOC name table small on deep FiveM trees.
	constexpr size_t kMaxBucketStem = 48;

	constexpr size_t npos = std::string::npos;

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

	// FNV-1a 32-bit over the UTF-8 path; used when an escaped bucket name is too
	// long or collides after sanitising illegal filename characters.
	uint32_t path_hash32(const std::string& text)
	{
		uint32_t hash = 2166136261u;
		for (unsigned char c : text)
		{
			hash ^= c;
			hash *= 16777619u;
		}
		return hash;
	}

	// One path segment: '_' doubles so a later join on '_' stays reversible
	// (`ymap_part1` vs `ymap/part1` -> `ymap__part1` vs `ymap_part1`).
	std::string escape_segment(const std::string& segment)
	{
		std::string out;
		out.reserve(segment.size() * 2);
		for (unsigned char c : segment)
		{
			if (c == '_')
			{
				out += "__";
				continue;
			}
			if (std::isalnum(c) || c == '-')
			{
				out.push_back(static_cast<char>(c));
				continue;
			}
			out.push_back('_');
		}
		return out;
	}

	// Relative directory under stream/ ("" for the stream root) -> bucket stem
	// without the `.rpf` suffix. Empty / root always maps to kRootStreamBucket.
	std::string bucket_stem_from_rel_dir(const std::string& relDir)
	{
		if (relDir.empty() || relDir == ".")
			return kRootStreamBucket;

		std::string stem;
		size_t begin = 0;
		while (begin <= relDir.size())
		{
			size_t end = relDir.find('/', begin);
			if (end == npos)
				end = relDir.size();
			const std::string part = relDir.substr(begin, end - begin);
			if (!part.empty() && part != ".")
			{
				const std::string escaped = escape_segment(part);
				if (!escaped.empty())
				{
					if (!stem.empty())
						stem.push_back('_');
					stem += escaped;
				}
			}
			if (end == relDir.size())
				break;
			begin = end + 1;
		}

		if (stem.empty())
			return kRootStreamBucket;
		if (stem.size() > kMaxBucketStem)
		{
			char hex[16];
			std::snprintf(hex, sizeof(hex), "s_%08x", path_hash32(relDir));
			return hex;
		}
		return stem;
	}

	// Parent directory of a resource-relative asset path, expressed relative to
	// stream/. Assets outside stream/ join the root bucket (same as today).
	std::string stream_rel_dir_for_asset(const std::string& asset)
	{
		const std::string lower = lower_copy(asset);
		const std::string prefix = "stream/";
		std::string under;
		if (lower.rfind(prefix, 0) == 0)
			under = asset.substr(prefix.size());
		else if (lower == "stream")
			return {};
		else
			return {}; // not under stream/ -> root bucket

		const size_t slash = under.find_last_of('/');
		if (slash == npos)
			return {}; // file directly under stream/
		return under.substr(0, slash);
	}

	struct StreamBucket
	{
		std::string stem;       // without .rpf
		std::string archive;    // stem + ".rpf"
		std::string sourceDir;  // stream-relative dir for logs ("" = stream root)
		size_t fileCount = 0;
	};

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

	std::string build_setup2(const std::string& pack, bool isMap)
	{
		std::ostringstream xml;
		xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
		xml << "<SSetupData>\n";
		xml << "  <deviceName>dlc_" << xml_escape(pack) << "</deviceName>\n";
		xml << "  <datFile>content.xml</datFile>\n";
		xml << "  <timeStamp>" << timestamp() << "</timeStamp>\n";
		xml << "  <nameHash>" << xml_escape(pack) << "</nameHash>\n";
		xml << "  <contentChangeSetGroups>\n";
		if (isMap)
		{
			// Data metas (interiorproxies) at startup; map RPFs when the SP
			// level streams — matches verified LEVEL_PACK map add-ons.
			xml << "    <Item>\n";
			xml << "      <NameHash>GROUP_STARTUP</NameHash>\n";
			xml << "      <ContentChangeSets>\n";
			xml << "        <Item>" << xml_escape(pack) << "_AUTOGEN</Item>\n";
			xml << "      </ContentChangeSets>\n";
			xml << "    </Item>\n";
			xml << "    <Item>\n";
			xml << "      <NameHash>GROUP_UPDATE_STREAMING</NameHash>\n";
			xml << "      <ContentChangeSets>\n";
			xml << "        <Item>CCS_" << xml_escape(pack) << "_NG_STREAMING_MAP</Item>\n";
			xml << "      </ContentChangeSets>\n";
			xml << "    </Item>\n";
		}
		else
		{
			xml << "    <Item>\n";
			xml << "      <NameHash>GROUP_STARTUP</NameHash>\n";
			xml << "      <ContentChangeSets>\n";
			xml << "        <Item>" << xml_escape(pack) << "_AUTOGEN</Item>\n";
			xml << "      </ContentChangeSets>\n";
			xml << "    </Item>\n";
		}
		xml << "  </contentChangeSetGroups>\n";
		if (isMap)
		{
			xml << "  <type>EXTRACONTENT_LEVEL_PACK</type>\n";
			xml << "  <order value=\"" << kDlcOrder << "\" />\n";
			xml << "  <isLevelPack value=\"true\" />\n";
		}
		else
		{
			xml << "  <type>EXTRACONTENT_COMPAT_PACK</type>\n";
			xml << "  <order value=\"" << kDlcOrder << "\" />\n";
		}
		xml << "</SSetupData>\n";
		return xml.str();
	}

	std::string build_content(const std::string& pack, const std::vector<StagedDataFile>& dataFiles,
		const std::vector<std::string>& streamArchives)
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

		for (const auto& archive : streamArchives)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(device) << ":/%PLATFORM%/"
				<< xml_escape(archive) << "</filename>\n";
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
		for (const auto& archive : streamArchives)
			xml << "        <Item>" << xml_escape(device) << ":/%PLATFORM%/"
				<< xml_escape(archive) << "</Item>\n";

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

	// Story-mode map DLC: props RPF (models+ytyp) + meta RPF (ymap/ybn/ymf)
	// tagged CONTENTS_DLC_MAP_DATA, enabled under $level=MO_JIM_L11.
	std::string build_content_map(const std::string& pack,
		const std::vector<StagedDataFile>& dataFiles,
		const std::vector<std::string>& itypRequests,
		bool haveProps, bool haveMeta)
	{
		const std::string device = "dlc_" + pack;
		const std::string propsPath = std::string(device) + ":/%PLATFORM%/" + kMapPropsArchive;
		const std::string metaPath = std::string(device) + ":/%PLATFORM%/" + kMapMetaArchive;

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

		if (haveProps)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(propsPath) << "</filename>\n";
			xml << "      <fileType>RPF_FILE</fileType>\n";
			xml << "      <overlay value=\"false\" />\n";
			xml << "      <disabled value=\"false\" />\n";
			xml << "      <persistent value=\"false\" />\n";
			xml << "    </Item>\n";
		}
		if (haveMeta)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(metaPath) << "</filename>\n";
			xml << "      <fileType>RPF_FILE</fileType>\n";
			xml << "      <overlay value=\"false\" />\n";
			xml << "      <disabled value=\"false\" />\n";
			xml << "      <persistent value=\"false\" />\n";
			xml << "      <contents>CONTENTS_DLC_MAP_DATA</contents>\n";
			xml << "    </Item>\n";
		}
		for (const auto& ityp : itypRequests)
		{
			xml << "    <Item>\n";
			xml << "      <filename>" << xml_escape(propsPath) << "/" << xml_escape(ityp)
				<< "</filename>\n";
			xml << "      <fileType>DLC_ITYP_REQUEST</fileType>\n";
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
		xml << "      </filesToEnable>\n";
		xml << "      <txdToLoad />\n";
		xml << "      <txdToUnload />\n";
		xml << "      <residentResources />\n";
		xml << "      <unregisterResources />\n";
		xml << "    </Item>\n";
		xml << "    <Item>\n";
		xml << "      <changeSetName>CCS_" << xml_escape(pack)
			<< "_NG_STREAMING_MAP</changeSetName>\n";
		xml << "      <filesToDisable />\n";
		xml << "      <filesToEnable>\n";
		if (haveProps)
			xml << "        <Item>" << xml_escape(propsPath) << "</Item>\n";
		if (haveMeta)
			xml << "        <Item>" << xml_escape(metaPath) << "</Item>\n";
		for (const auto& ityp : itypRequests)
			xml << "        <Item>" << xml_escape(propsPath) << "/" << xml_escape(ityp)
				<< "</Item>\n";
		xml << "      </filesToEnable>\n";
		xml << "      <txdToLoad />\n";
		xml << "      <txdToUnload />\n";
		xml << "      <residentResources />\n";
		xml << "      <unregisterResources />\n";
		xml << "      <executionConditions>\n";
		xml << "        <activeChangesetConditions>\n";
		xml << "        </activeChangesetConditions>\n";
		xml << "        <genericConditions>$level=MO_JIM_L11</genericConditions>\n";
		xml << "      </executionConditions>\n";
		xml << "    </Item>\n";
		xml << "  </contentChangeSets>\n";
		xml << "  <patchFiles />\n";
		xml << "</CDataFileMgr__ContentsOfDataFileXml>\n";
		return xml.str();
	}

	// .ymap/.ybn/_manifest.ymf → map_meta; models+ytyp → map_props; lodlights skip.
	enum class MapAssetBucket { Props, Meta, Skip };

	MapAssetBucket classify_map_asset(const std::string& relDir, const std::string& baseLower)
	{
		if (relDir == "lodlights" ||
			baseLower.find("lodlights") != npos ||
			baseLower.find("distlodlights") != npos)
			return MapAssetBucket::Skip;

		const size_t dot = baseLower.rfind('.');
		const std::string ext = (dot == npos) ? std::string() : baseLower.substr(dot);
		if (ext == ".ymap" || ext == ".ybn" || ext == ".ymf")
			return MapAssetBucket::Meta;
		if (ext == ".ydr" || ext == ".ydd" || ext == ".yft" || ext == ".ytd" ||
			ext == ".ytyp")
			return MapAssetBucket::Props;
		return MapAssetBucket::Props;
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
		else
			logger::write("fivem", "[gen] %s: no archive yet, building", resource.name.c_str());

		const fs::path stage = config::cache_root() / "fivem" / resource.pack / "stage";
		fs::remove_all(stage, ec);
		fs::create_directories(stage, ec);

		std::vector<StagedDataFile> stagedFiles;
		std::set<std::string> usedNames;
		std::vector<std::string> itypRequests;
		std::vector<std::string> streamArchives;
		bool haveMapProps = false;
		bool haveMapMeta = false;

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

			// Map packs: ytyps live inside map_props.rpf and are requested from
			// content.xml — do not also stage a loose DLC_ITYP_REQUEST copy.
			if (resource.isMap && _stricmp(type.c_str(), "DLC_ITYP_REQUEST") == 0)
			{
				logger::write("fivem", "[gen] %s: deferring %s to map_props DLC_ITYP_REQUEST",
					resource.name.c_str(), file.path.c_str());
				continue;
			}

			// DLC_ITYP_REQUEST is resolved through content.xml against the pack
			// device, so the typ has to keep a real path inside the archive.
			// Everything else is data and may be flattened into data/.
			if (_stricmp(type.c_str(), "DLC_ITYP_REQUEST") == 0)
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

		if (resource.isMap)
		{
			// Verified SP map layout: models+ytyp in map_props.rpf; ymap/ybn/
			// _manifest.ymf in map_meta.rpf with CONTENTS_DLC_MAP_DATA. Lodlights
			// still skipped — incomplete night-light replacements AV on SP.
			std::set<std::string> usedProps;
			std::set<std::string> usedMeta;
			size_t propsCount = 0;
			size_t metaCount = 0;
			const fs::path propsRoot = stage / "x64" / fs::path(kMapPropsArchive);
			const fs::path metaRoot = stage / "x64" / fs::path(kMapMetaArchive);

			for (const auto& asset : resource.assets)
			{
				const fs::path source = resource.dir / fs::path(asset);
				if (!fs::is_regular_file(source, ec))
					continue;

				const std::string basename = fs::path(asset).filename().string();
				if (basename.empty())
					continue;

				const std::string relDir = stream_rel_dir_for_asset(asset);
				const std::string baseLower = lower_copy(basename);
				const MapAssetBucket bucket = classify_map_asset(relDir, baseLower);
				if (bucket == MapAssetBucket::Skip)
				{
					logger::write("fivem", "[gen] %s: skipping %s (lodlights unsafe in story mode)",
						resource.name.c_str(), asset.c_str());
					continue;
				}

				std::set<std::string>& used =
					(bucket == MapAssetBucket::Meta) ? usedMeta : usedProps;
				if (!used.insert(baseLower).second)
				{
					logger::write("fivem", "[gen] %s: %s duplicates %s; keeping the first",
						resource.name.c_str(), asset.c_str(), basename.c_str());
					continue;
				}

				const fs::path target = (bucket == MapAssetBucket::Meta)
					? (metaRoot / basename)
					: (propsRoot / basename);
				if (!stage_file(source, target))
					continue;

				if (bucket == MapAssetBucket::Meta)
					++metaCount;
				else
				{
					++propsCount;
					if (baseLower.size() >= 5 &&
						baseLower.compare(baseLower.size() - 5, 5, ".ytyp") == 0)
						itypRequests.push_back(basename);
				}
			}

			haveMapProps = propsCount > 0;
			haveMapMeta = metaCount > 0;
			if (haveMapProps)
			{
				streamArchives.push_back(kMapPropsArchive);
				logger::write("fivem", "[gen] %s: %s <- props (%zu file(s), %zu ytyp)",
					resource.name.c_str(), kMapPropsArchive, propsCount, itypRequests.size());
			}
			if (haveMapMeta)
			{
				streamArchives.push_back(kMapMetaArchive);
				logger::write("fivem", "[gen] %s: %s <- meta (%zu file(s))",
					resource.name.c_str(), kMapMetaArchive, metaCount);
			}
		}
		else
		{
			// One nested RPF per stream/ directory that directly holds assets.
			// Files are flattened to that RPF's root so streaming names stay
			// basenames. Vehicles never ship `_manifest.ymf`.
			std::unordered_map<std::string, StreamBucket> buckets;
			std::unordered_map<std::string, std::string> stemByRelDir;
			std::set<std::string> usedStems;
			std::unordered_map<std::string, std::set<std::string>> usedInBucket;
			std::unordered_map<std::string, std::string> basenameOwner;

			auto ensure_bucket = [&](const std::string& relDir) -> StreamBucket&
			{
				auto found = stemByRelDir.find(relDir);
				if (found != stemByRelDir.end())
					return buckets[found->second];

				std::string stem = bucket_stem_from_rel_dir(relDir);
				if (!usedStems.insert(lower_copy(stem)).second)
				{
					char hex[16];
					std::snprintf(hex, sizeof(hex), "%s_%06x", stem.c_str(),
						path_hash32(relDir) & 0xFFFFFFu);
					stem = hex;
					if (stem.size() > kMaxBucketStem + 16)
					{
						std::snprintf(hex, sizeof(hex), "s_%08x", path_hash32(relDir));
						stem = hex;
					}
					usedStems.insert(lower_copy(stem));
				}

				StreamBucket bucket;
				bucket.stem = stem;
				bucket.archive = stem + ".rpf";
				bucket.sourceDir = relDir;
				buckets.emplace(stem, bucket);
				stemByRelDir.emplace(relDir, stem);
				return buckets[stem];
			};

			for (const auto& asset : resource.assets)
			{
				const fs::path source = resource.dir / fs::path(asset);
				if (!fs::is_regular_file(source, ec))
					continue;

				const std::string basename = fs::path(asset).filename().string();
				if (basename.empty())
					continue;

				if (_stricmp(basename.c_str(), "_manifest.ymf") == 0)
				{
					logger::write("fivem", "[gen] %s: skipping %s (not used for vehicles)",
						resource.name.c_str(), asset.c_str());
					continue;
				}

				const std::string relDir = stream_rel_dir_for_asset(asset);
				StreamBucket& bucket = ensure_bucket(relDir);
				std::set<std::string>& usedHere = usedInBucket[bucket.stem];
				const std::string baseKey = lower_copy(basename);
				if (!usedHere.insert(baseKey).second)
				{
					logger::write("fivem", "[gen] %s: %s and an earlier file in %s share the "
						"name %s; keeping the first", resource.name.c_str(), asset.c_str(),
						bucket.archive.c_str(), basename.c_str());
					continue;
				}

				auto owner = basenameOwner.find(baseKey);
				if (owner != basenameOwner.end() && owner->second != bucket.stem)
				{
					logger::write("fivem", "[gen] %s: %s also appears in %s.rpf; later mount wins",
						resource.name.c_str(), basename.c_str(), owner->second.c_str());
				}
				else
					basenameOwner.emplace(baseKey, bucket.stem);

				const fs::path target = stage / "x64" / bucket.archive / basename;
				if (!stage_file(source, target))
					continue;

				++bucket.fileCount;
			}

			std::vector<std::string> stems;
			stems.reserve(buckets.size());
			for (const auto& entry : buckets)
				stems.push_back(entry.first);
			std::sort(stems.begin(), stems.end(), [&](const std::string& a, const std::string& b) {
				if (a == kRootStreamBucket)
					return b != kRootStreamBucket;
				if (b == kRootStreamBucket)
					return false;
				return lower_copy(a) < lower_copy(b);
			});
			for (const auto& stem : stems)
			{
				const StreamBucket& bucket = buckets[stem];
				if (bucket.fileCount == 0)
					continue;
				streamArchives.push_back(bucket.archive);
				const char* dirLabel = bucket.sourceDir.empty() ? "stream/" : bucket.sourceDir.c_str();
				logger::write("fivem", "[gen] %s: %s <- %s (%zu file(s))",
					resource.name.c_str(), bucket.archive.c_str(), dirLabel, bucket.fileCount);
			}
		}

		if (stagedFiles.empty() && streamArchives.empty())
		{
			error = "nothing to package (no data files and no streamed assets)";
			return BuildResult::Failed;
		}

		// A layout reference that resolves to nothing is invisible in game, so it
		// is worth a line here even though it does not stop the build.
		check_layout_refs(resource.name, stage, stagedFiles);

		const std::string contentXml = resource.isMap
			? build_content_map(resource.pack, stagedFiles, itypRequests, haveMapProps, haveMapMeta)
			: build_content(resource.pack, stagedFiles, streamArchives);
		if (!write_text(stage / "setup2.xml", build_setup2(resource.pack, resource.isMap)) ||
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
			"Generated by ZeroIV-Loader from a FiveM resource.\n"
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
