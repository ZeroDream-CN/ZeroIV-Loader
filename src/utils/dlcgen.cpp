#include "main.h"
#include "utils/dlcgen.h"

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
	//   7: also register the change set under GROUP_UPDATE_STREAMING so Enhanced
	//      still applies high-order COMPAT packs after common:/ remount.
	//   8: AUDIO_* configs land under x64/audio/ and AUDIO_WAVEPACK is a plain
	//      folder of .awc files (matching retail mptuner), not stream.rpf.
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

	constexpr size_t npos = std::string::npos;

	// Staging lives under ZeroIVLoose/fivem/<pack>/stage. After the archive is
	// written (or the build aborts) the stage tree is deleted, but the empty
	// pack / fivem / ZeroIVLoose parents used to remain. Walk up and drop every
	// empty ancestor that still sits under the cache root.
	void remove_stage_tree(const fs::path& stage)
	{
		std::error_code ec;
		fs::remove_all(stage, ec);

		const fs::path cacheRoot = config::cache_root();
		fs::path cur = stage.parent_path();
		while (!cur.empty())
		{
			if (!fs::is_directory(cur, ec))
				break;
			if (!fs::is_empty(cur, ec))
				break;

			const bool atCacheRoot = cur == cacheRoot;
			const fs::path parent = cur.parent_path();
			fs::remove(cur, ec);
			if (atCacheRoot)
				break;
			cur = parent;
		}
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

	bool ends_with_ci(const std::string& value, const std::string& tail)
	{
		if (value.size() < tail.size())
			return false;
		for (size_t i = 0; i < tail.size(); ++i)
		{
			const unsigned char a = static_cast<unsigned char>(value[value.size() - tail.size() + i]);
			const unsigned char b = static_cast<unsigned char>(tail[i]);
			if (::tolower(a) != ::tolower(b))
				return false;
		}
		return true;
	}

	bool is_audio_wavepack(const std::string& type)
	{
		return _stricmp(type.c_str(), "AUDIO_WAVEPACK") == 0;
	}

	const char* audio_rel_suffix(const std::string& type)
	{
		if (_stricmp(type.c_str(), "AUDIO_GAMEDATA") == 0) return ".dat151.rel";
		if (_stricmp(type.c_str(), "AUDIO_SOUNDDATA") == 0) return ".dat54.rel";
		if (_stricmp(type.c_str(), "AUDIO_SYNTHDATA") == 0) return ".dat10.rel";
		if (_stricmp(type.c_str(), "AUDIO_DYNAMIXDATA") == 0) return ".dat15.rel";
		if (_stricmp(type.c_str(), "AUDIO_SPEECHDATA") == 0) return ".dat4.rel";
		if (_stricmp(type.c_str(), "AUDIO_CURVEDATA") == 0) return ".dat13.rel";
		return nullptr;
	}

	bool is_audio_data_type(const std::string& type)
	{
		return audio_rel_suffix(type) != nullptr;
	}

	// Logical stem used in content.xml (`foo_game` from `foo_game.dat` /
	// `foo_game.dat151.rel`).
	std::string audio_logical_stem(std::string path)
	{
		const size_t slash = path.find_last_of("/\\");
		if (slash != npos)
			path = path.substr(slash + 1);

		const std::string lower = lower_copy(path);
		static const char* kStrip[] = {
			".dat151.rel", ".dat54.rel", ".dat10.rel", ".dat15.rel",
			".dat4.rel", ".dat13.rel",
			".dat151", ".dat54", ".dat10", ".dat15", ".dat4", ".dat13",
			".dat",
		};
		for (const char* suffix : kStrip)
		{
			const size_t len = std::strlen(suffix);
			if (lower.size() >= len &&
				lower.compare(lower.size() - len, len, suffix) == 0)
			{
				path.resize(path.size() - len);
				break;
			}
		}
		return path;
	}

	// FiveM declares `common/data/foo_game.dat`; the bytes are usually
	// `foo_game.dat151.rel` beside it.
	fs::path resolve_audio_source(const fs::path& root, const std::string& declared,
		const std::string& type)
	{
		std::error_code ec;
		const fs::path exact = root / fs::path(declared);
		if (fs::is_regular_file(exact, ec))
			return exact;

		const char* suffix = audio_rel_suffix(type);
		if (!suffix)
			return {};

		std::string stemPath = declared;
		if (ends_with_ci(stemPath, ".dat"))
			stemPath.resize(stemPath.size() - 4);

		const fs::path candidate = root / fs::path(stemPath + suffix);
		if (fs::is_regular_file(candidate, ec))
			return candidate;

		return {};
	}

	// Reserves a unique pack-relative path under prefix/ (e.g. data/ or
	// x64/audio/). content.xml for audio uses a separate logical name.
	std::string unique_under(const std::string& prefix, const std::string& basename,
		std::set<std::string>& used)
	{
		const std::string folder = prefix.empty() ? std::string() : prefix + "/";
		std::string candidate = folder + basename;
		if (used.insert(lower_copy(candidate)).second)
			return candidate;

		const size_t dot = basename.find_last_of('.');
		const std::string stem = dot == npos ? basename : basename.substr(0, dot);
		const std::string ext = dot == npos ? std::string() : basename.substr(dot);

		for (int index = 2; index < 1000; ++index)
		{
			candidate = folder + stem + "_" + std::to_string(index) + ext;
			if (used.insert(lower_copy(candidate)).second)
				return candidate;
		}
		return {};
	}

	// One pack-internal name per staged file. Two metadata files sharing a
	// basename is common enough (several `vehicles.meta`) that collisions are
	// resolved instead of overwriting each other.
	std::string unique_name(const std::string& basename, std::set<std::string>& used)
	{
		return unique_under("data", basename, used);
	}

	// Copies every regular file under sourceDir into targetDir (flat: wavepacks
	// are a single folder of .awc files in retail, not a nested tree).
	bool stage_wavepack_dir(const fs::path& sourceDir, const fs::path& targetDir,
		std::string& error)
	{
		std::error_code ec;
		fs::create_directories(targetDir, ec);

		bool any = false;
		for (fs::directory_iterator it(sourceDir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code fileEc;
			if (!it->is_regular_file(fileEc))
				continue;
			if (!stage_file(it->path(), targetDir / it->path().filename()))
			{
				error = "cannot stage " + it->path().string();
				return false;
			}
			any = true;
		}
		if (!any)
		{
			error = "wavepack folder is empty: " + sourceDir.string();
			return false;
		}
		return true;
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
		xml << "    <Item>\n";
		xml << "      <NameHash>GROUP_UPDATE_STREAMING</NameHash>\n";
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
		remove_stage_tree(stage);
		fs::create_directories(stage, ec);

		std::vector<StagedDataFile> stagedFiles;
		std::set<std::string> usedNames;

		for (const auto& file : resource.dataFiles)
		{
			const std::string type = map_file_type(file.type);
			const std::string relative = file.path;

			// AUDIO_WAVEPACK points at a folder of .awc files. Retail stores it
			// as a plain directory under x64/audio/sfx/<name>/ (not a nested
			// RPF); content.xml names that folder without a .rpf suffix.
			if (is_audio_wavepack(type))
			{
				const fs::path sourceDir = resource.dir / fs::path(relative);
				if (!fs::is_directory(sourceDir, ec))
				{
					logger::write("fivem", "[gen] %s: missing wavepack folder %s",
						resource.name.c_str(), relative.c_str());
					continue;
				}

				const std::string folderName = fs::path(relative).filename().string();
				if (folderName.empty() || folderName == "." || folderName == "..")
				{
					logger::write("fivem", "[gen] %s: bad wavepack path %s",
						resource.name.c_str(), relative.c_str());
					continue;
				}

				const std::string stageRel = "x64/audio/sfx/" + folderName;
				if (!usedNames.insert(lower_copy(stageRel)).second)
				{
					logger::write("fivem", "[gen] %s: duplicate wavepack %s; keeping the first",
						resource.name.c_str(), folderName.c_str());
					continue;
				}

				std::string stageError;
				if (!stage_wavepack_dir(sourceDir, stage / fs::path(stageRel), stageError))
				{
					logger::write("fivem", "[gen] %s: %s", resource.name.c_str(),
						stageError.c_str());
					continue;
				}

				stagedFiles.push_back({ type, std::string("%PLATFORM%/audio/sfx/") + folderName });
				continue;
			}

			// AUDIO_* configs: resolve FiveM's logical `.dat` to the on-disk
			// `.datNN.rel`, stage under x64/audio/, and declare the logical
			// `%PLATFORM%/audio/<stem>.dat` name the audio mounter expects.
			// The staged basename must match that stem -- the engine looks up
			// stem.dat151.rel from the logical name, so collisions are skipped
			// rather than renamed.
			if (is_audio_data_type(type))
			{
				const fs::path source =
					resolve_audio_source(resource.dir, relative, type);
				if (source.empty())
				{
					logger::write("fivem", "[gen] %s: missing audio data %s",
						resource.name.c_str(), relative.c_str());
					continue;
				}

				const char* suffix = audio_rel_suffix(type);
				const std::string stem = audio_logical_stem(relative);
				if (stem.empty() || !suffix)
					continue;

				const std::string stageRel = std::string("x64/audio/") + stem + suffix;
				if (!usedNames.insert(lower_copy(stageRel)).second)
				{
					logger::write("fivem", "[gen] %s: duplicate audio data %s; keeping the first",
						resource.name.c_str(), stem.c_str());
					continue;
				}

				if (!stage_file(source, stage / fs::path(stageRel)))
					continue;

				// Optional nametable next to the .rel (retail ships both).
				if (ends_with_ci(source.filename().string(), ".rel"))
				{
					std::string tableFile = source.filename().string();
					tableFile.resize(tableFile.size() - 4);
					tableFile += ".nametable";
					const fs::path tableSrc = source.parent_path() / tableFile;

					std::string typed = stem + suffix; // e.g. foo_game.dat151.rel
					typed.resize(typed.size() - 4);
					typed += ".nametable";
					const std::string tableRel = std::string("x64/audio/") + typed;
					if (fs::is_regular_file(tableSrc, ec) &&
						usedNames.insert(lower_copy(tableRel)).second)
					{
						stage_file(tableSrc, stage / fs::path(tableRel));
					}
				}

				stagedFiles.push_back({ type,
					std::string("%PLATFORM%/audio/") + stem + ".dat" });
				continue;
			}

			const fs::path source = resource.dir / fs::path(relative);
			if (!fs::is_regular_file(source, ec))
			{
				logger::write("fivem", "[gen] %s: missing %s", resource.name.c_str(),
					relative.c_str());
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

		// Streamed assets are flattened into the archive root; see kStreamArchive
		// for why the prefix has to go and why the name is generic.
		const fs::path streamRoot = stage / "x64" / kStreamArchive;
		std::set<std::string> usedAssets;
		bool hasStream = false;
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
					"keeping the first", resource.name.c_str(), asset.c_str(), basename.c_str());
				continue;
			}

			if (!stage_file(source, streamRoot / fs::path(basename)))
				continue;
			hasStream = true;
		}

		if (stagedFiles.empty() && !hasStream)
		{
			error = "nothing to package (no data files and no streamed assets)";
			remove_stage_tree(stage);
			return BuildResult::Failed;
		}

		// A layout reference that resolves to nothing is invisible in game, so it
		// is worth a line here even though it does not stop the build.
		check_layout_refs(resource.name, stage, stagedFiles);

		if (!write_text(stage / "setup2.xml", build_setup2(resource.pack)) ||
			!write_text(stage / "content.xml",
				build_content(resource.pack, stagedFiles, hasStream)))
		{
			error = "cannot write setup2.xml/content.xml under " + stage.string();
			remove_stage_tree(stage);
			return BuildResult::Failed;
		}

		std::string buildError;
		if (!loosepack::BuildTo(stage, outputRpf, buildError))
		{
			error = "archive: " + buildError;
			remove_stage_tree(stage);
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
		remove_stage_tree(stage);

		return BuildResult::Built;
	}

	void Prune(const std::vector<std::string>& packs)
	{
		unpublish_missing(std::set<std::string>(packs.begin(), packs.end()));
	}
}
