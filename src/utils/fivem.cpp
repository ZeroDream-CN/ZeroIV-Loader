#include "main.h"
#include "utils/fivem.h"
#include "utils/dlcgen.h"
#include "utils/rsccompat.h"
#include "utils/i18n.h"
#include "hooks/DlclistHook.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

/*
	FiveM resources carry a Lua manifest instead of a retail DLC layout, so the
	loader builds the DLC from it:

	  fxmanifest.lua / __resource.lua
	    data_file 'TYPE' 'path'   -> a dataFiles entry in content.xml
	    stream/**                 -> the payload of x64/stream.rpf

	Only what the manifest actually declares is trusted. Anything the scanner
	cannot make sense of drops that one resource and leaves the rest alone: a
	malformed manifest must never take the game down.
*/

namespace
{
	namespace fs = std::filesystem;

	constexpr size_t npos = std::string::npos;

	bool is_ident_char(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
	}

	std::string lower_copy(std::string value)
	{
		for (auto& c : value)
			c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		return value;
	}

	static bool ends_with(const std::string& value, const std::string& tail)
	{
		return value.size() >= tail.size() &&
			value.compare(value.size() - tail.size(), tail.size(), tail) == 0;
	}

	// Removes Lua comments while keeping long strings intact. A manifest uses
	// both [[ ]] forms, so the delimiter level is tracked instead of assuming
	// the plain one.
	std::string strip_comments(const std::string& src)
	{
		std::string out;
		out.reserve(src.size());

		size_t i = 0;
		while (i < src.size())
		{
			if (src[i] == '-' && i + 1 < src.size() && src[i + 1] == '-')
			{
				size_t j = i + 2;
				int level = 0;
				if (j < src.size() && src[j] == '[')
				{
					size_t k = j + 1;
					while (k < src.size() && src[k] == '=')
					{
						++level;
						++k;
					}
					if (k < src.size() && src[k] == '[')
					{
						const std::string close = "]" + std::string(level, '=') + "]";
						const size_t end = src.find(close, k + 1);
						i = end == npos ? src.size() : end + close.size();
						continue;
					}
				}

				while (i < src.size() && src[i] != '\n')
					++i;
				continue;
			}

			if (src[i] == '[')
			{
				size_t j = i + 1;
				int level = 0;
				while (j < src.size() && src[j] == '=')
				{
					++level;
					++j;
				}
				if (j < src.size() && src[j] == '[')
				{
					const std::string close = "]" + std::string(level, '=') + "]";
					const size_t end = src.find(close, j + 1);
					const size_t stop = end == npos ? src.size() : end + close.size();
					out.append(src, i, stop - i);
					i = stop;
					continue;
				}
			}

			out.push_back(src[i]);
			++i;
		}

		return out;
	}

	// Skips whitespace and the punctuation Lua allows between arguments, so
	// both `data_file 'A' 'b'` and `data_file('A', 'b')` parse the same way.
	void skip_separators(const std::string& s, size_t& i)
	{
		while (i < s.size())
		{
			const char c = s[i];
			if (std::isspace(static_cast<unsigned char>(c)) || c == ',' || c == '(' || c == ')')
			{
				++i;
				continue;
			}
			break;
		}
	}

	bool read_string(const std::string& s, size_t& i, std::string& out)
	{
		skip_separators(s, i);
		if (i >= s.size())
			return false;

		const char quote = s[i];
		if (quote == '\'' || quote == '"')
		{
			const size_t start = ++i;
			while (i < s.size() && s[i] != quote)
				++i;
			if (i >= s.size())
				return false;
			out.assign(s, start, i - start);
			++i;
			return true;
		}

		if (quote == '[')
		{
			size_t j = i + 1;
			int level = 0;
			while (j < s.size() && s[j] == '=')
			{
				++level;
				++j;
			}
			if (j < s.size() && s[j] == '[')
			{
				const std::string close = "]" + std::string(level, '=') + "]";
				const size_t start = j + 1;
				const size_t end = s.find(close, start);
				if (end == npos)
					return false;
				out.assign(s, start, end - start);
				i = end + close.size();
				return true;
			}
		}

		return false;
	}

	size_t find_word(const std::string& s, const char* word, size_t from)
	{
		const size_t len = std::strlen(word);
		for (size_t i = from; i + len <= s.size(); ++i)
		{
			if (s.compare(i, len, word) != 0)
				continue;
			if (i > 0 && is_ident_char(s[i - 1]))
				continue;
			if (i + len < s.size() && is_ident_char(s[i + len]))
				continue;
			return i;
		}
		return npos;
	}

	// True when only whitespace precedes `pos` on its line; keeps the legacy
	// singular `file 'x'` form from matching inside another string.
	bool at_line_start(const std::string& s, size_t pos)
	{
		for (size_t i = pos; i > 0; --i)
		{
			const char c = s[i - 1];
			if (c == '\n' || c == '\r')
				return true;
			if (!std::isspace(static_cast<unsigned char>(c)))
				return false;
		}
		return true;
	}

	std::string normalize_relative(std::string path)
	{
		for (auto& c : path)
			if (c == '\\')
				c = '/';

		while (path.size() > 2 && path[0] == '.' && path[1] == '/')
			path.erase(0, 2);
		while (!path.empty() && path[0] == '/')
			path.erase(0, 1);
		return path;
	}

	bool wildcard_match(const char* mask, const char* text)
	{
		while (*mask)
		{
			if (*mask == '*')
			{
				++mask;
				if (!*mask)
					return true;
				for (const char* t = text; ; ++t)
				{
					if (wildcard_match(mask, t))
						return true;
					if (!*t)
						return false;
				}
			}
			if (!*text)
				return false;
			if (*mask != '?' && ::tolower(static_cast<unsigned char>(*mask)) !=
				::tolower(static_cast<unsigned char>(*text)))
				return false;
			++mask;
			++text;
		}
		return *text == '\0';
	}

	bool is_asset_ext(const std::string& ext)
	{
		static const char* kExts[] = {
			".yft", ".ytd", ".ydr", ".ydd", ".ybn", ".ytyp", ".ymap", ".yed", ".yfd",
			".yld", ".ycd", ".ypt", ".ynv", ".yvr", ".ymf", ".ygr", ".ycs",
			".ybd", ".awc",
		};
		for (const char* candidate : kExts)
			if (ext == candidate)
				return true;
		return false;
	}

	// Filename-to-data_file-type fallback for resources that only declare
	// `files {}` (common with older __resource.lua packs).
	std::string infer_type(const std::string& path)
	{
		const size_t slash = path.find_last_of('/');
		const std::string name = lower_copy(slash == npos ? path : path.substr(slash + 1));

		if (name == "handling.meta") return "HANDLING_FILE";
		if (name == "vehicles.meta") return "VEHICLE_METADATA_FILE";
		if (name == "carcols.meta") return "CARCOLS_FILE";
		if (name == "carvariations.meta") return "VEHICLE_VARIATION_FILE";
		if (name == "vehiclelayouts.meta") return "VEHICLE_LAYOUTS_FILE";
		if (name == "dlctext.meta") return "TEXTFILE_METAFILE";
		if (name == "peds.meta") return "PED_METADATA_FILE";
		if (name == "pedpersonality.meta") return "PED_PERSONALITY_FILE";
		if (name == "weapons.meta") return "WEAPONINFO_FILE";
		if (name == "weaponcomponents.meta") return "WEAPONCOMPONENTSINFO_FILE";
		if (name.find("gtxd") != npos && ends_with(name, ".meta")) return "GTXD_PARENTING_DATA";
		if (ends_with(name, ".ytyp")) return "DLC_ITYP_REQUEST";
		return {};
	}

	bool expand_file_list(const fs::path& root, const std::vector<std::string>& patterns,
		std::vector<std::string>& out)
	{
		std::set<std::string> seen;
		std::error_code ec;

		for (const auto& pattern : patterns)
		{
			const std::string clean = normalize_relative(pattern);
			if (clean.empty())
				continue;

			if (clean.find('*') == npos && clean.find('?') == npos)
			{
				if (fs::is_regular_file(root / clean, ec) && seen.insert(lower_copy(clean)).second)
					out.push_back(clean);
				continue;
			}

			const size_t slash = clean.find_last_of('/');
			const std::string dirPart = slash == npos ? std::string() : clean.substr(0, slash);
			const std::string mask = slash == npos ? clean : clean.substr(slash + 1);

			const fs::path base = dirPart.empty() ? root : (root / dirPart);
			std::error_code dirEc;
			for (fs::directory_iterator it(base, dirEc), end; !dirEc && it != end; it.increment(dirEc))
			{
				std::error_code fileEc;
				if (!it->is_regular_file(fileEc))
					continue;
				const std::string filename = it->path().filename().string();
				if (!wildcard_match(mask.c_str(), filename.c_str()))
					continue;
				const std::string rel = dirPart.empty() ? filename : dirPart + "/" + filename;
				if (seen.insert(lower_copy(rel)).second)
					out.push_back(rel);
			}
		}

		return true;
	}

	// Streamed assets: everything under stream/ plus any resource file that
	// carries an asset extension. Keeping the resource-relative directory means
	// a pack with two same-named tuning parts cannot silently overwrite one.
	void collect_assets(fivem::Resource& res)
	{
		std::set<std::string> dataPaths;
		for (const auto& file : res.dataFiles)
			dataPaths.insert(lower_copy(normalize_relative(file.path)));

		std::set<std::string> seen;
		std::error_code ec;
		for (fs::recursive_directory_iterator it(res.dir, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code fileEc;
			if (it->is_directory(fileEc))
				continue;

			const fs::path& path = it->path();
			const std::string ext = lower_copy(path.extension().string());
			if (!is_asset_ext(ext))
				continue;

			std::error_code relEc;
			const fs::path rel = fs::relative(path, res.dir, relEc);
			if (relEc)
				continue;

			const std::string text = normalize_relative(rel.generic_string());
			if (text.empty())
				continue;
			const std::string key = lower_copy(text);
			if (dataPaths.count(key))
				continue;
			if (!seen.insert(key).second)
				continue;
			res.assets.push_back(text);
		}
	}

	// Records size and write time for every file that the archive is built
	// from: the manifest, the declared data files and the streamed assets. A
	// file declared but absent is listed with zeroes, so creating it later is
	// still detected as a change.
	void collect_sources(fivem::Resource& res)
	{
		std::set<std::string> seen;
		auto add = [&](const std::string& raw)
		{
			const std::string rel = normalize_relative(raw);
			if (rel.empty())
				return;
			if (!seen.insert(lower_copy(rel)).second)
				return;

			fivem::Resource::SourceState state;
			state.path = rel;

			std::error_code ec;
			const fs::path full = res.dir / fs::path(rel);
			if (fs::is_regular_file(full, ec))
			{
				state.size = static_cast<uint64_t>(fs::file_size(full, ec));
				if (ec)
					state.size = 0;
				std::error_code timeEc;
				state.mtime = static_cast<uint64_t>(
					fs::last_write_time(full, timeEc).time_since_epoch().count());
				if (timeEc)
					state.mtime = 0;
			}

			res.sources.push_back(std::move(state));
		};

		std::error_code relEc;
		const fs::path manifestRel = fs::relative(res.manifest, res.dir, relEc);
		if (!relEc)
			add(manifestRel.generic_string());

		for (const auto& file : res.dataFiles)
			add(file.path);
		for (const auto& asset : res.assets)
			add(asset);

		std::sort(res.sources.begin(), res.sources.end(),
			[](const auto& left, const auto& right)
			{
				return lower_copy(left.path) < lower_copy(right.path);
			});
	}

	void compute_signature(fivem::Resource& res)
	{
		// Each entry contributes its own size and write time, so a change to one
		// file cannot be cancelled out by an equal-sized change to another (the
		// aggregate total used to allow exactly that).
		uint64_t hash = 1469598103934665603ull;
		auto mix = [&hash](const void* data, size_t size)
		{
			const auto* bytes = static_cast<const unsigned char*>(data);
			for (size_t i = 0; i < size; ++i)
			{
				hash ^= bytes[i];
				hash *= 1099511628211ull;
			}
		};

		for (const auto& source : res.sources)
		{
			const std::string key = lower_copy(source.path);
			mix(key.data(), key.size());
			mix(&source.size, sizeof(source.size));
			mix(&source.mtime, sizeof(source.mtime));
		}

		char text[64];
		std::snprintf(text, sizeof(text), "%zu-%016llx",
			res.sources.size(), static_cast<unsigned long long>(hash));
		res.signature = text;
	}

	// Case-insensitive check used by the dedupe below.
	std::string dedupe_key(const fivem::DataFile& file)
	{
		return lower_copy(file.type) + "|" + lower_copy(normalize_relative(file.path));
	}
}

namespace fivem
{
	bool ParseManifest(const fs::path& resourceDir, const fs::path& manifestPath, Resource& out,
		std::string& error)
	{
		std::ifstream in(manifestPath, std::ios::binary);
		if (!in)
		{
			error = "cannot read " + manifestPath.string();
			return false;
		}

		std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		const std::string text = strip_comments(raw);

		out.dir = resourceDir;
		out.manifest = manifestPath;
		out.name = resourceDir.filename().string();
		out.pack = MakePackName(resourceDir);
		out.dataFiles.clear();
		out.assets.clear();

		std::set<std::string> seenDataFiles;

		// data_file 'TYPE' 'path'
		size_t cursor = 0;
		while ((cursor = find_word(text, "data_file", cursor)) != npos)
		{
			size_t i = cursor + std::strlen("data_file");
			DataFile file;
			if (!read_string(text, i, file.type) || !read_string(text, i, file.path))
			{
				++cursor;
				continue;
			}

			file.type = file.type.substr(0, file.type.find_first_of(" \t\r\n"));
			file.path = normalize_relative(file.path);
			// A file type without a path or the other way round is unusable.
			if (!file.type.empty() && !file.path.empty() && seenDataFiles.insert(dedupe_key(file)).second)
				out.dataFiles.push_back(file);

			cursor = i > cursor ? i : cursor + 1;
		}

		// this_is_a_map 'yes'
		size_t mapPos = find_word(text, "this_is_a_map", 0);
		if (mapPos != npos)
		{
			size_t i = mapPos + std::strlen("this_is_a_map");
			std::string value;
			if (read_string(text, i, value))
			{
				const std::string lowered = lower_copy(value);
				out.isMap = lowered == "yes" || lowered == "true" || lowered == "1";
			}
		}

		std::vector<std::string> declared;
		std::set<std::string> seenDeclared;

		// files { 'a', 'b' }
		cursor = 0;
		while ((cursor = find_word(text, "files", cursor)) != npos)
		{
			size_t i = cursor + std::strlen("files");
			while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
				++i;
			if (i >= text.size() || text[i] != '{')
			{
				++cursor;
				continue;
			}

			++i;
			while (i < text.size())
			{
				while (i < text.size() &&
					(std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ','))
					++i;
				if (i >= text.size())
					break;
				if (text[i] == '}')
				{
					++i;
					break;
				}

				std::string value;
				if (!read_string(text, i, value))
				{
					++i;
					continue;
				}
				const std::string clean = normalize_relative(value);
				if (!clean.empty() && seenDeclared.insert(lower_copy(clean)).second)
					declared.push_back(clean);
			}
			cursor = i > cursor ? i : cursor + 1;
		}

		// file 'path' (legacy single-file form, one per line)
		cursor = 0;
		while ((cursor = find_word(text, "file", cursor)) != npos)
		{
			if (!at_line_start(text, cursor))
			{
				++cursor;
				continue;
			}
			size_t i = cursor + std::strlen("file");
			std::string value;
			if (read_string(text, i, value))
			{
				const std::string clean = normalize_relative(value);
				if (!clean.empty() && seenDeclared.insert(lower_copy(clean)).second)
					declared.push_back(clean);
			}
			cursor = i > cursor ? i : cursor + 1;
		}

		std::vector<std::string> expanded;
		expand_file_list(resourceDir, declared, expanded);

		// A manifest that only lists files still needs content.xml entries, so
		// the type is inferred from the filename when nothing else says it.
		if (out.dataFiles.empty())
		{
			for (const auto& file : expanded)
			{
				const std::string type = infer_type(file);
				if (type.empty())
					continue;
				DataFile entry{ type, file };
				if (seenDataFiles.insert(dedupe_key(entry)).second)
					out.dataFiles.push_back(std::move(entry));
			}
		}

		collect_assets(out);

		if (out.dataFiles.empty() && out.assets.empty())
		{
			error = "manifest declares neither data_file nor streamed assets";
			return false;
		}

		return true;
	}

	std::string MakePackName(const fs::path& resourceDir)
	{
		std::string base = resourceDir.filename().string();
		std::string sanitized;
		sanitized.reserve(base.size());
		for (char c : base)
		{
			const unsigned char value = static_cast<unsigned char>(c);
			if (std::isalnum(value))
				sanitized.push_back(static_cast<char>(::tolower(value)));
			else if (c == '_' || c == '-')
				sanitized.push_back(c);
			else
				sanitized.push_back('_');
		}

		if (sanitized.empty())
			sanitized = "res";
		if (sanitized.size() > 20)
			sanitized.resize(20);

		uint64_t hash = 1469598103934665603ull;
		std::error_code ec;
		const std::string key = fs::absolute(resourceDir, ec).lexically_normal().generic_string();
		for (unsigned char c : key)
		{
			hash ^= c;
			hash *= 1099511628211ull;
		}

		char suffix[16];
		std::snprintf(suffix, sizeof(suffix), "%06x", static_cast<unsigned>(hash >> 32) & 0xFFFFFFu);
		return "ziv_" + sanitized + suffix;
	}

	std::vector<Resource> Scan(std::string& error)
	{
		std::vector<Resource> resources;
		std::error_code ec;

		const fs::path root = config::unpack_root();
		if (!fs::is_directory(root, ec))
		{
			error = "unpack root " + root.string() + " is missing";
			return resources;
		}

		for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		{
			std::error_code dirEc;
			if (!it->is_directory(dirEc))
				continue;

			// Dot-directories are the loader's own cache (.cache) and anything
			// else that is bookkeeping rather than a resource.
			const std::string dirName = it->path().filename().string();
			if (!dirName.empty() && dirName[0] == '.')
				continue;

			const fs::path fx = it->path() / "fxmanifest.lua";
			const fs::path legacy = it->path() / "__resource.lua";

			fs::path manifest;
			if (fs::is_regular_file(fx, dirEc))
				manifest = fx;
			else if (fs::is_regular_file(legacy, dirEc))
				manifest = legacy;
			if (manifest.empty())
				continue;

			Resource resource;
			std::string parseError;
			if (!ParseManifest(it->path(), manifest, resource, parseError))
			{
				logger::write("fivem", "[scan] %s: %s", it->path().filename().string().c_str(),
					parseError.c_str());
				continue;
			}

			// Sources are collected before the signature so both describe the
			// same set of files.
			collect_sources(resource);
			compute_signature(resource);
			resources.push_back(std::move(resource));
		}

		if (ec)
			error = "cannot list " + root.string() + ": " + ec.message();

		return resources;
	}

	// Traditional add-on DLC folders (OpenIV / CodeWalker layout) that already
	// have a dlc.rpf — as a real archive or as a loose directory. These are not
	// FiveM resources, so the generator never sees them; they still need a
	// dlclist.xml line or the engine never mounts them.
	//
	// Auto-registered names are recorded under generated_root so a later start
	// can remove only those lines when the folder disappears — never a line the
	// player typed into dlclist by hand.
	std::string trim_line(const std::string& value)
	{
		const size_t begin = value.find_first_not_of(" \t\r\n");
		if (begin == npos)
			return {};
		const size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(begin, end - begin + 1);
	}

	fs::path addon_manifest_path()
	{
		return config::generated_root() / "registered-addon-packs.txt";
	}

	// Retail game tree (cwd while the ASI runs). Stock DLC lives here and must
	// never be pulled out of dlclist.xml.
	fs::path stock_dlcpacks_root()
	{
		std::error_code ec;
		return fs::current_path(ec) / "update" / "x64" / "dlcpacks";
	}

	bool has_dlc_rpf(const fs::path& packDir)
	{
		std::error_code ec;
		const fs::path dlc = packDir / "dlc.rpf";
		return fs::exists(dlc, ec);
	}

	bool is_ziv_pack(const std::string& name)
	{
		return name.size() >= 4 &&
			_strnicmp(name.c_str(), "ziv_", 4) == 0;
	}

	// Pack folder names that ship with the game install. Built once per Init.
	std::set<std::string> list_stock_packs()
	{
		std::set<std::string> names;
		std::error_code ec;
		const fs::path root = stock_dlcpacks_root();
		if (!fs::is_directory(root, ec))
			return names;
		for (const auto& entry : fs::directory_iterator(root, ec))
		{
			if (!entry.is_directory(ec))
				continue;
			const std::string name = entry.path().filename().string();
			if (name.empty() || name[0] == '.' || is_ziv_pack(name))
				continue;
			// Stock packs are always a real dlc.rpf file, not a loose directory.
			if (!fs::is_regular_file(entry.path() / "dlc.rpf", ec))
				continue;
			names.insert(name);
		}
		return names;
	}

	bool name_in_set_ci(const std::set<std::string>& names, const std::string& name)
	{
		if (names.count(name) > 0)
			return true;
		for (const auto& item : names)
			if (item.size() == name.size() &&
				_strnicmp(item.c_str(), name.c_str(), name.size()) == 0)
				return true;
		return false;
	}

	// True when a dlc.rpf for `name` exists under mods, unpack-mods, the
	// generated cache, or the retail game dlcpacks folder.
	bool pack_present(const std::string& name, const std::set<std::string>& stockPacks)
	{
		if (name_in_set_ci(stockPacks, name))
			return true;

		const fs::path roots[] = {
			config::mods_root() / "update" / "x64" / "dlcpacks" / name,
			config::unpack_root() / "dlcpacks" / name,
			config::generated_root() / "dlcpacks" / name,
			stock_dlcpacks_root() / name,
		};
		for (const auto& dir : roots)
			if (has_dlc_rpf(dir))
				return true;
		return false;
	}

	std::set<std::string> load_addon_manifest()
	{
		std::set<std::string> names;
		std::ifstream in(addon_manifest_path());
		if (!in)
			return names;
		std::string line;
		while (std::getline(in, line))
		{
			const std::string name = trim_line(line);
			if (!name.empty() && name[0] != '#')
				names.insert(name);
		}
		return names;
	}

	void save_addon_manifest(const std::set<std::string>& names)
	{
		std::error_code ec;
		fs::create_directories(addon_manifest_path().parent_path(), ec);
		std::ofstream out(addon_manifest_path(), std::ios::trunc);
		if (!out)
		{
			logger::write("fivem", "[init] cannot write %s",
				addon_manifest_path().string().c_str());
			return;
		}
		out << "# Traditional dlcpacks auto-registered into dlclist.xml by ZeroIV.\n";
		out << "# Only these names are removed when the folder disappears.\n";
		out << "# Retail update/x64/dlcpacks names are never listed or pruned.\n";
		for (const auto& name : names)
			out << name << '\n';
	}

	// Discovers traditional (non-ziv_, non-stock) packs under mods / unpack layers.
	// The retail game folder is not scanned for auto-add — those entries already
	// live in stock dlclist.xml and must stay there.
	std::vector<std::string> discover_addon_packs(const std::set<std::string>& stockPacks)
	{
		std::vector<std::string> packs;
		std::set<std::string> seen;
		const fs::path roots[] = {
			config::mods_root() / "update" / "x64" / "dlcpacks",
			config::unpack_root() / "dlcpacks",
			config::generated_root() / "dlcpacks",
		};

		for (const auto& root : roots)
		{
			std::error_code ec;
			if (!fs::is_directory(root, ec))
				continue;
			for (const auto& entry : fs::directory_iterator(root, ec))
			{
				if (!entry.is_directory(ec))
					continue;
				const std::string name = entry.path().filename().string();
				if (name.empty() || name[0] == '.' || is_ziv_pack(name) ||
					name_in_set_ci(stockPacks, name) || !has_dlc_rpf(entry.path()))
					continue;
				if (!seen.insert(name).second)
					continue;
				packs.push_back(name);
				logger::write("fivem", "[init] addon pack %s (%s)", name.c_str(),
					root.string().c_str());
			}
		}
		return packs;
	}

	void Init()
	{
		static const bool enabled = config::get_value<bool>("fivem", "enabled", true);
		if (!enabled)
		{
			logger::write("fivem", "[init] disabled by config");
			return;
		}

		const uint64_t started = GetTickCount64();

		std::string scanError;
		const std::vector<Resource> resources = Scan(scanError);
		if (!scanError.empty())
		{
			logger::write("fivem", "[init] %s", scanError.c_str());
			return;
		}
		if (resources.empty())
			logger::write("fivem", "[init] no FiveM resources under %s",
				config::unpack_root().string().c_str());
		else
			logger::write("fivem", "[init] %d resource(s) under %s",
				static_cast<int>(resources.size()),
				config::unpack_root().string().c_str());

		std::vector<std::string> packs;
		std::vector<rsccompat::Hit> compatHits;
		const bool warnIncompatible =
			config::get_value<bool>("fivem", "warnIncompatibleResources", true);
		const bool enhanced = IsEnhanced();

		for (const auto& resource : resources)
		{
			if (warnIncompatible)
			{
				std::vector<std::string> paths = resource.assets;
				for (const auto& file : resource.dataFiles)
					paths.push_back(file.path);
				rsccompat::ScanFiveMAssets(resource.name, resource.dir, paths, enhanced,
					compatHits);
			}

			std::string error;
			const dlcgen::BuildResult result = dlcgen::Build(resource, error);
			if (result == dlcgen::BuildResult::Failed)
			{
				logger::write("fivem", "[init] %s: %s", resource.name.c_str(), error.c_str());
				continue;
			}
			if (result == dlcgen::BuildResult::Skipped)
			{
				logger::write("fivem", "[init] %s: skipped (not published)", resource.name.c_str());
				continue;
			}

			logger::write("fivem", "[init] %s -> .cache/dlcpacks/%s (%s)", resource.name.c_str(),
				resource.pack.c_str(),
				result == dlcgen::BuildResult::Reused ? "cached" : "built");
			packs.push_back(resource.pack);
		}

		// A pack whose resource was deleted must lose its published link as well as
		// its dlclist entry; see dlcgen::Prune. This runs even when nothing is left,
		// so an emptied unpack-mods folder also empties the generated setup.
		dlcgen::Prune(packs);

		// Traditional packs: always reconcile the tracked manifest so deleted
		// auto-registered folders leave dlclist. registerAddonPacks only gates
		// whether newly discovered folders are added (and claimed) this run.
		// Presence checks also look under the retail game dlcpacks folder; stock
		// pack names are never claimed and never pruned from dlclist.
		static const bool registerAddons =
			config::get_value<bool>("fivem", "registerAddonPacks", true);
		const std::set<std::string> stockPacks = list_stock_packs();
		const std::set<std::string> previousAddons = load_addon_manifest();
		const std::vector<std::string> discovered = discover_addon_packs(stockPacks);

		// Loose OpenIV-style packs (directory shaped like dlc.rpf/) can also
		// carry unconverted stream assets; scan those the same way.
		if (warnIncompatible)
		{
			const fs::path looseRoot = config::unpack_root() / "dlcpacks";
			std::error_code ec;
			if (fs::is_directory(looseRoot, ec))
			{
				for (const auto& entry : fs::directory_iterator(looseRoot, ec))
				{
					if (!entry.is_directory(ec))
						continue;
					const std::string name = entry.path().filename().string();
					if (name.empty() || name[0] == '.' || is_ziv_pack(name) ||
						name_in_set_ci(stockPacks, name))
						continue;
					const fs::path loose = entry.path() / "dlc.rpf";
					if (fs::is_directory(loose, ec))
						rsccompat::ScanTree(name, loose, enhanced, compatHits);
				}
			}
		}

		std::vector<std::string> pruneAddons;
		for (const auto& name : previousAddons)
		{
			if (name_in_set_ci(stockPacks, name))
			{
				// Stale manifest entry for a retail pack — drop from the list
				// file only; never touch dlclist.xml.
				logger::write("fivem", "[init] addon pack %s is retail; leaving "
					"dlclist alone", name.c_str());
				continue;
			}
			if (pack_present(name, stockPacks))
				continue;
			pruneAddons.push_back(name);
			logger::write("fivem", "[init] addon pack %s gone from mods, unpack-mods "
				"and retail dlcpacks; will drop tracked dlclist entry", name.c_str());
		}

		std::vector<std::string> claimableAddons;
		if (registerAddons)
		{
			std::set<std::string> seen(packs.begin(), packs.end());
			for (const auto& name : discovered)
			{
				if (seen.insert(name).second)
					packs.push_back(name);
				claimableAddons.push_back(name);
			}
		}
		else
			logger::write("fivem", "[init] registerAddonPacks=off; traditional "
				"dlcpacks not auto-added (tracked removals still apply)");

		dlclist::Reserve(packs, pruneAddons, claimableAddons);

		// The archive is still closed here, so the list can be rewritten as plain
		// file I/O. When it is still NG-encrypted, this call only decides that one
		// read-only pass is needed and the work is deferred to OnArchiveOpened.
		dlclist::Prepare();

		// Manifest tracks traditional packs this loader is responsible for:
		// still-present previous claims, lines inserted this run, and (when
		// registerAddonPacks is on) every pack discovered this start — including
		// ones already in dlclist, so a later delete can still clean them up.
		// Retail packs and hand-edited lines with no matching folder stay out.
		std::set<std::string> nextManifest;
		for (const auto& name : previousAddons)
		{
			if (name_in_set_ci(stockPacks, name))
				continue;
			if (pack_present(name, stockPacks))
				nextManifest.insert(name);
		}
		for (const auto& name : dlclist::TakeClaimedAddons())
			if (!name_in_set_ci(stockPacks, name))
				nextManifest.insert(name);
		if (registerAddons)
			for (const auto& name : discovered)
				nextManifest.insert(name);
		if (nextManifest != previousAddons)
			save_addon_manifest(nextManifest);

		if (warnIncompatible && !compatHits.empty())
		{
			std::vector<std::string> badPacks;
			std::set<std::string> seenPacks;
			for (const auto& hit : compatHits)
			{
				if (seenPacks.insert(hit.pack).second)
					badPacks.push_back(hit.pack);
			}
			logger::write("fivem", "[compat] %d incompatible file(s) in %d pack(s); "
				"showing warning", static_cast<int>(compatHits.size()),
				static_cast<int>(badPacks.size()));
			i18n::showIncompatibleResources(badPacks, static_cast<int>(compatHits.size()));
		}

		logger::write("fivem", "[init] %d pack(s) armed for dlclist in %llums",
			static_cast<int>(packs.size()),
			static_cast<unsigned long long>(GetTickCount64() - started));
	}
}
