#include "main.h"
#include "DlclistHook.h"

#include "utils/i18n.h"
#include "utils/inflate.h"
#include "utils/ngcrypt.h"
#include "utils/rpftoc.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

/*
	See DlclistHook.h for the design.

	The safe conversion is: decrypt every flag-1 payload in place (length
	unchanged, so the NG key still matches), then rewrite the TOC as OPEN.
	Payloads whose flag is not 0 or 1 are left untouched. If the process dies
	after some payloads are decrypted but before the TOC is OPEN, the next
	start resumes — flag-1 entries still encrypt, already-cleared ones skip.
*/

namespace
{
	namespace fs = std::filesystem;

	constexpr size_t npos = std::string::npos;
	constexpr uint64_t kReservation = 64ull << 10; // room for every future entry
	const char* kDlclistPath = "common/data/dlclist.xml";
	const char* kGameConfigPath = "common/data/gameconfig.xml";

	std::mutex g_mutex;
	std::vector<std::string> g_packs;

	fs::path stock_path()
	{
		std::error_code ec;
		return fs::current_path(ec) / "update/update.rpf";
	}

	fs::path mods_path()
	{
		return config::mods_root() / "update/update.rpf";
	}

	// Where the patched list is kept for inspection after a run.
	fs::path patched_xml_path()
	{
		return config::generated_root() / "update.dlclist.patched.xml";
	}

	fs::path patched_gameconfig_path()
	{
		return config::generated_root() / "update.gameconfig.patched.xml";
	}

	struct Stamp
	{
		uint64_t size = 0;
		uint64_t time = 0;

		bool valid() const { return size != 0; }
	};

	Stamp stat_file(const fs::path& path)
	{
		Stamp stamp;
		WIN32_FILE_ATTRIBUTE_DATA data;
		if (!::GetFileAttributesExA(path.string().c_str(), GetFileExInfoStandard, &data))
			return stamp;
		stamp.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
		stamp.time = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
			data.ftLastWriteTime.dwLowDateTime;
		return stamp;
	}

	bool contains_ci(const std::string& haystack, const std::string& needle)	{
		if (needle.empty() || needle.size() > haystack.size())
			return false;
		for (size_t i = 0; i + needle.size() <= haystack.size(); ++i)
			if (_strnicmp(haystack.c_str() + i, needle.c_str(), needle.size()) == 0)
				return true;
		return false;
	}

	size_t rfind_ci(const std::string& haystack, const std::string& needle)
	{
		if (needle.empty() || haystack.size() < needle.size())
			return npos;
		for (size_t i = haystack.size() - needle.size() + 1; i-- > 0;)
			if (_strnicmp(haystack.c_str() + i, needle.c_str(), needle.size()) == 0)
				return i;
		return npos;
	}

	std::string trim(const std::string& value)
	{
		const size_t begin = value.find_first_not_of(" \t\r\n");
		if (begin == npos)
			return {};
		const size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(begin, end - begin + 1);
	}

	std::string to_lower_slashes(const char* path)
	{
		std::string value(path ? path : "");
		for (auto& c : value)
		{
			if (c == '\\')
				c = '/';
			c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
		}
		return value;
	}

	bool path_starts_with(const std::string& path, const std::string& root)
	{
		const std::string loweredPath = to_lower_slashes(path.c_str());
		std::string loweredRoot = to_lower_slashes(root.c_str());
		while (!loweredRoot.empty() && loweredRoot.back() == '/')
			loweredRoot.pop_back();
		if (loweredRoot.empty() || loweredPath.size() < loweredRoot.size())
			return false;
		if (loweredPath.compare(0, loweredRoot.size(), loweredRoot) != 0)
			return false;
		return loweredPath.size() == loweredRoot.size() || loweredPath[loweredRoot.size()] == '/';
	}

	// One side comes from the resolver as `mods/update/update.rpf`, the other from
	// config as an absolute path; comparing raw strings treats them as unrelated,
	// which once made a modded archive look like the stock one.
	bool is_inside(const std::string& path, const fs::path& root)
	{
		std::error_code ec;
		fs::path candidate = fs::weakly_canonical(fs::path(path), ec);
		if (ec)
		{
			ec.clear();
			candidate = fs::absolute(fs::path(path), ec);
		}
		if (ec)
			return false;

		ec.clear();
		fs::path base = fs::weakly_canonical(root, ec);
		if (ec)
		{
			ec.clear();
			base = fs::absolute(root, ec);
		}
		if (ec)
			return false;

		return path_starts_with(candidate.string(), base.string());
	}

	bool write_text(const fs::path& path, const std::string& data)
	{
		std::error_code ec;
		fs::create_directories(path.parent_path(), ec);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		out.write(data.data(), static_cast<std::streamsize>(data.size()));
		return static_cast<bool>(out);
	}

	// True when `entry` is one of the pack names this loader generates. Only its
	// own namespace is ever removed from dlclist: a pack the player added by hand
	// keeps its entry even if this run does not know about it.
	bool is_generated_pack(const std::string& entry)
	{
		static const std::string prefix = "dlcpacks:/ziv_";
		return _strnicmp(entry.c_str(), prefix.c_str(), prefix.size()) == 0;
	}

	// Brings dlclist.xml in line with the generated packs: entries for packs that
	// no longer exist are dropped, missing ones are appended.
	//
	// Pruning is not cosmetic. A stale entry makes the engine try to mount an
	// archive that is gone on every start, and a failed mount of a pack listed in
	// dlclist can abort the whole mount pass, which makes *every* pack unavailable
	// rather than just the missing one.
	std::string build_patched(const std::string& original, const std::vector<std::string>& packs,
		int& added, int& removed, std::string& error)
	{
		size_t close = rfind_ci(original, "</Paths>");
		if (close == npos)
			close = rfind_ci(original, "</SMandatoryPacksData>");
		if (close == npos)
		{
			error = "no </Paths> marker in dlclist.xml";
			return {};
		}

		std::set<std::string> wanted;
		for (const auto& pack : packs)
			if (!pack.empty())
				wanted.insert("dlcpacks:/" + pack + "/");

		// Walk the <Item> lines, keeping every one that is not a generated pack, or
		// that is a generated pack still on disk.
		added = 0;
		removed = 0;
		const std::string openTag = "<Item>";
		const std::string closeTag = "</Item>";
		std::string kept;
		kept.reserve(original.size());
		size_t cursor = 0;
		while (true)
		{
			const size_t open = original.find(openTag, cursor);
			if (open == npos)
			{
				kept.append(original, cursor, npos);
				break;
			}
			const size_t valueStart = open + openTag.size();
			const size_t end = original.find(closeTag, valueStart);
			if (end == npos)
			{
				kept.append(original, cursor, npos);
				break;
			}

			const std::string value = trim(original.substr(valueStart, end - valueStart));
			const bool generated = is_generated_pack(value);
			const bool keep = !generated || wanted.count(value) > 0;
			if (generated && !keep)
			{
				// Drop the whole line, including its indentation and line break.
				size_t lineStart = kept.size();
				while (lineStart > cursor &&
					(original[lineStart - 1] == ' ' || original[lineStart - 1] == '\t'))
					--lineStart;
				kept.erase(lineStart);
				cursor = end + closeTag.size();
				while (cursor < original.size() &&
					(original[cursor] == '\r' || original[cursor] == '\n'))
					++cursor;
				++removed;
				continue;
			}

			kept.append(original, cursor, end + closeTag.size() - cursor);
			cursor = end + closeTag.size();
		}

		std::string insert;
		for (const auto& item : wanted)
		{
			if (contains_ci(kept, "<Item>" + item + "</Item>"))
				continue;
			insert += "\t\t<Item>" + item + "</Item>\r\n";
			++added;
		}

		if (added == 0 && removed == 0)
			return original;

		// The closing tag moved if lines were dropped, so locate it again.
		close = rfind_ci(kept, "</Paths>");
		if (close == npos)
			close = rfind_ci(kept, "</SMandatoryPacksData>");
		if (close == npos)
		{
			error = "no </Paths> marker in dlclist.xml";
			return {};
		}

		std::string patched = kept;
		if (added > 0 && close > 0 && kept[close - 1] != '\n' && kept[close - 1] != '\r')
		{
			// Another tool can leave the last entry without a line break; without
			// this the first new item would be glued onto it.
			insert.insert(0, "\r\n");
		}
		patched.insert(close, insert);
		return patched;
	}

	// Reads the table of contents and returns it in plaintext, whatever cipher the
	// archive was written with.
	//
	// The key index mixes the archive's own file name with its total size, so the
	// name passed here has to be the one on disk ("update.rpf"), not a full path.
	bool read_table(const std::string& path, rpftoc::Table& out, std::string& error)
	{
		rpftoc::Raw raw;
		if (!rpftoc::readRaw(path, raw, error))
			return false;

		if (raw.encryption != rpftoc::kEncryptionOpen)
		{
			if (!ngcrypt::init(error))
				return false;

			const std::string name = fs::path(path).filename().string();
			const uint32_t length = static_cast<uint32_t>(rpftoc::fileSize(path) & 0xFFFFFFFFull);

			// Entries and names are separate cipher streams, both keyed by the
			// archive, but they sit in one buffer here.
			const size_t entryBytes = static_cast<size_t>(raw.count) * 16;
			std::string entries = raw.blob.substr(16, entryBytes);
			std::string names = raw.blob.substr(16 + entryBytes);
			if (!ngcrypt::decrypt(entries, name, length, error) ||
				!ngcrypt::decrypt(names, name, length, error))
				return false;

			raw.blob = raw.blob.substr(0, 16) + entries + names;
		}

		if (!rpftoc::parse(raw.blob, out, error))
			return false;

		// A table captured from a wrong guess still looks like entries; offsets
		// that fall outside the file are what give it away.
		return out.checkPayloads(rpftoc::fileSize(path), error);
	}

	// Decrypts and inflates an XML payload stored inside update.rpf.
	bool read_xml(const std::string& path, const rpftoc::Table& table, uint32_t index,
		const char* label, std::string& out, std::string& error)
	{
		if (!rpftoc::readPayloadRaw(path, table, index, out, error))
			return false;

		// A payload is keyed by its own name and uncompressed size, so this stays
		// valid no matter how the archive itself is stored.
		const uint32_t realSize = table.entryPayloadSize(index);
		if (table.entryIsEncrypted(index))
		{
			if (!ngcrypt::init(error))
				return false;
			if (!ngcrypt::decrypt(out, table.entryName(index), realSize, error))
				return false;
		}

		if (table.entryStoredSize(index) != 0)
		{
			std::string inflated;
			if (!inflate::raw(inflated, out, error))
				return false;
			out = std::move(inflated);
		}

		if (out.find("<?xml") > 8)
		{
			error = std::string(label) + " did not decode to XML";
			return false;
		}
		return true;
	}

	bool read_list(const std::string& path, const rpftoc::Table& table, uint32_t index,
		std::string& out, std::string& error)
	{
		return read_xml(path, table, index, kDlclistPath, out, error);
	}

	bool write_allowed(const fs::path& target)
	{
		// The stock archive lives in update/ and is the file Steam verifies; the
		// mods layer shadows it, so there is no reason to ever write there.
		if (!is_inside(target.string(), stock_path().parent_path()))
			return true;
		return config::get_value<bool>("fivem", "patchStockUpdateRpf", false);
	}

	// ---- gameconfig.xml -----------------------------------------------------

	// The ceilings an add-on pack allocates from while the engine registers its
	// vehicles during a mount. The shipped values are already close to full once
	// every retail DLC is mounted, so a handful of extra packs runs out
	// mid-registration: the engine either rolls the mount back -- which looks
	// exactly like "the pack was rejected" -- or writes through a null slot and
	// crashes while the next DLC loads.
	//
	// These are written as targets, not floors: a table that only ever grew could
	// never be retuned, and a value an earlier build inflated would stay in the
	// archive forever. Each entry also carries the stock value so the whole patch
	// can be reverted exactly by flipping the config switch.
	//
	// Every byte a pool reserves is memory the streaming engine cannot use, so
	// values stay close to stock. ArchiveCount is the exception and must not be
	// lowered: it sizes the streaming archive pool, retail content uses virtually
	// all 4440 slots, and an add-on pack costs two more (dlc.rpf and the nested
	// stream.rpf). Cutting it back to stock overflows the pool, which does not
	// fail one pack -- archive registration collapses, the map stops streaming
	// entirely and no DLC vehicle can be spawned.
	struct Limit
	{
		const char* tag;
		uint64_t target;
		uint64_t stock;
	};

	// Values below follow the community reference for this build rather than a
	// guess: RDE 4.1.5 gameconfig.xml for GTA V 3889, maintained by Jax765 and
	// PNWParksFan (github.com/pnwparksfan/gameconfig). The stock column is the
	// value shipped in update/update.rpf for the same build, read back from the
	// retail archive.

	// <Tag value="N"/> form, used by ConfigModelInfo and ConfigStreamingEngine.
	//
	// The model info ceilings are the ones that actually run out when a pack
	// registers cars: every added car is a model info before it is an entity.
	// ArchiveCount is sized for the add-on packs with room to spare but stays
	// under the tier that needs Packfile Limit Adjuster; 4440 slots is already
	// close to what retail fills.
	const Limit kAttributeLimits[] = {
		{ "MaxVehicleModelInfos", 5000, 320 },
		{ "MaxExtraVehicleModelInfos", 5225, 640 },
		{ "ArchiveCount", 4800, 4440 },
	};

	// <PoolName>X</PoolName><PoolSize value="N"/> form, inside PoolSizes.
	//
	// DrawableStore / TxdStore / DwdStore / fwArchetypePooledMap stay at stock:
	// raising them to RDE-sized values erased the map even with HeapAdjuster.
	// FragmentStore and MetaDataStore get a modest bump only — crash dumps with
	// 18 add-on packs showed both at capacity (42000/42000 and 3200/3200) while
	// mounting stream.rpf.
	const Limit kPoolLimits[] = {
		{ "Vehicles", 300, 300 },
		{ "VehicleStreamRequest", 240, 60 },
		{ "VehicleStreamRender", 240, 100 },
		{ "VehicleStruct", 600, 200 },
		{ "HandlingData", 5450, 1800 },
		{ "carrec", 4000, 4000 },
		{ "MetaDataStore", 4000, 3200 },
		{ "FragmentStore", 48000, 42000 },
		{ "fwArchetypePooledMap", 64000, 64000 },
		{ "DrawableStore", 81000, 81000 },
		{ "TxdStore", 80200, 80200 },
		{ "DwdStore", 20500, 20500 },
	};

	// Rewrites one numeric or attribute value to `target`. `valueAt` is the
	// offset of the digits and `end` the first byte after them. Returns true
	// when it changed.
	//
	// `raiseOnly` decides what happens to a value that is already larger than
	// `target`. When the loader is applying its own settings it must never
	// shrink an existing one: an install that already carries a tuned
	// gameconfig -- bigger streaming stores, more vehicle slots -- is better off
	// than whatever this code would write, and overwriting it downwards is
	// exactly what turns the world low-poly and then makes it disappear. Only
	// the revert path, which wants stock values back, writes unconditionally.
	bool raise_number(std::string& xml, size_t valueAt, size_t end, const char* name,
		uint64_t target, bool raiseOnly, std::string& report, int& changed)
	{
		const std::string current = trim(xml.substr(valueAt, end - valueAt));
		const uint64_t value = current.empty()
			? 0 : std::strtoull(current.c_str(), nullptr, 10);
		if (value == target)
			return false;
		if (raiseOnly && value > target)
			return false;

		const std::string raised = std::to_string(target);
		xml.replace(valueAt, end - valueAt, raised);
		if (changed < 6)
		{
			if (!report.empty())
				report += ", ";
			report += std::string(name) + " " + current + " -> " + raised;
		}
		++changed;
		return true;
	}

	// Sets every `<tag ... value="N"/>` to `target`.
	int raise_values(std::string& xml, const char* tag, uint64_t target, bool raiseOnly, std::string& report)
	{
		int changed = 0;
		const std::string open = std::string("<") + tag;
		size_t cursor = 0;
		while ((cursor = xml.find(open, cursor)) != npos)
		{
			const size_t tagEnd = xml.find('>', cursor);
			if (tagEnd == npos)
				break;

			const size_t valueKey = xml.find("value=\"", cursor);
			if (valueKey == npos || valueKey > tagEnd)
			{
				cursor = tagEnd + 1;
				continue;
			}

			const size_t digits = valueKey + 7; // past `value="`
			const size_t quote = xml.find('"', digits);
			if (quote == npos || quote > tagEnd)
			{
				cursor = tagEnd + 1;
				continue;
			}

			raise_number(xml, digits, quote, tag, target, raiseOnly, report, changed);
			cursor = static_cast<size_t>(xml.find('>', digits)) + 1;
			if (cursor == 0)
				break;
		}
		return changed;
	}

	// Sets `<PoolName>X</PoolName><PoolSize value="N"/>` to `target`. The two tags
	// are matched as a pair because a bare `Vehicles` would also hit
	// `VehicleStruct` and every other vehicle-prefixed pool.
	int raise_pool(std::string& xml, const char* pool, uint64_t target, bool raiseOnly, std::string& report)
	{
		int changed = 0;
		const std::string key = std::string("<PoolName>") + pool + "</PoolName>";
		size_t cursor = 0;
		while ((cursor = xml.find(key, cursor)) != npos)
		{
			const size_t sizeKey = xml.find("<PoolSize", cursor);
			if (sizeKey == npos)
				break;

			const size_t tagEnd = xml.find('>', sizeKey);
			const size_t valueKey = xml.find("value=\"", sizeKey);
			if (tagEnd == npos || valueKey == npos || valueKey > tagEnd)
			{
				cursor = (tagEnd == npos) ? npos : tagEnd + 1;
				if (cursor == npos)
					break;
				continue;
			}

			const size_t digits = valueKey + 7;
			const size_t quote = xml.find('"', digits);
			if (quote == npos || quote > tagEnd)
			{
				cursor = tagEnd + 1;
				continue;
			}

			raise_number(xml, digits, quote, pool, target, raiseOnly, report, changed);
			cursor = static_cast<size_t>(xml.find('>', digits)) + 1;
			if (cursor == 0)
				break;
		}
		return changed;
	}

	// Returns the original text when every value already matches, so the caller
	// can skip the write and keep the archive untouched on every later start.
	// With `apply` false the stock values are written instead, which makes the
	// config switch a real revert rather than a no-op.
	std::string build_gameconfig(const std::string& original, bool apply, int& changed,
		std::string& report)
	{
		std::string patched = original;
		changed = 0;
		for (const auto& limit : kAttributeLimits)
			changed += raise_values(patched, limit.tag, apply ? limit.target : limit.stock, apply, report);
		for (const auto& limit : kPoolLimits)
			changed += raise_pool(patched, limit.tag, apply ? limit.target : limit.stock, apply, report);
		return changed == 0 ? original : patched;
	}

	// One XML payload inside update.rpf, ready to be stored.
	struct PayloadEdit
	{
		uint32_t index = 0;
		const char* path = "";
		std::string payload;
		fs::path dump;
	};

	// A region appended by an earlier run is reused instead of growing the
	// archive again. The table of contents is the only record of what lives
	// where, so a region is this loader's to overwrite exactly when no other
	// entry's payload covers it.
	bool region_is_free(const rpftoc::Table& table, uint32_t exclude, uint64_t offset,
		uint64_t length)
	{
		for (uint32_t i = 0; i < table.count; ++i)
		{
			if (i == exclude || table.entryIsDirectory(i))
				continue;

			const uint64_t start =
				static_cast<uint64_t>(table.entryBlock(i)) * rpftoc::kBlockSize;
			const uint64_t size = table.entryPayloadSize(i);
			if (size == 0)
				continue;

			if (offset < start + size && start < offset + length)
				return false;
		}
		return true;
	}

	// Counts binary entries whose flag is still 1 (NG-encrypted).
	int encrypted_count(const rpftoc::Table& table)
	{
		int count = 0;
		for (uint32_t i = 0; i < table.count; ++i)
		{
			if (!table.entryIsDirectory(i) && !table.entryIsResource(i) && table.entryIsEncrypted(i))
				++count;
		}
		return count;
	}

	// True when the archive still needs work: an encrypted payload left to
	// decrypt, or a binary entry whose flag does not yet say "stored as-is".
	bool plainify_enabled()
	{
		// Primary path: rebuild update.rpf as a fully OPEN, unencrypted archive
		// so dlclist/gameconfig edits need no hybrid NG stamp in memory.
		return config::get_value<bool>("fivem", "plainifyRpf", true);
	}

	bool needs_plainify(const rpftoc::Table& table)
	{
		// Only flag == 1 means "NG-encrypted binary". Other non-zero values are
		// left alone (see entryIsEncrypted).
		return encrypted_count(table) > 0;
	}

	// Decrypts every payload still marked encrypted (flag == 1), rewrites it at
	// the same offset (NG is a block cipher; length is unchanged) and clears
	// that flag. Combined with writeOpen this yields a genuine plaintext
	// archive the engine can read with no key material.
	//
	// First start that converts a stock archive rewrites hundreds of MB and
	// can take about a minute. Later starts are a no-op once every flag is 0.
	//
	// Verified against pristine update.rpf: flag-1 payloads decrypt
	// byte-identical to stock. Entries whose flag is not 0 or 1 are skipped.
	bool plainify(const std::string& path, rpftoc::Table& table, int& done, std::string& error)
	{
		done = 0;
		if (encrypted_count(table) == 0)
			return true;

		if (!ngcrypt::ready())
		{
			if (!ngcrypt::init(error))
				return false;
		}

		for (uint32_t i = 0; i < table.count; ++i)
		{
			if (table.entryIsDirectory(i) || table.entryIsResource(i) ||
				!table.entryIsEncrypted(i))
				continue;

			std::string payload;
			if (!rpftoc::readPayloadRaw(path, table, i, payload, error))
				return false;

			const std::string name = table.entryName(i);
			const uint32_t realSize = table.entryPayloadSize(i);
			if (!ngcrypt::decrypt(payload, name, realSize, error))
				return false;

			const uint64_t offset =
				static_cast<uint64_t>(table.entryBlock(i)) * rpftoc::kBlockSize;
			if (!rpftoc::writePayloadAt(path, offset, payload, error))
				return false;

			table.clearEncryption(i);
			++done;
		}
		return true;
	}

	// Stores every edit and repoints the entries.
	//
	// The table is written as plaintext before anything is appended: appending to
	// an NG archive and then dying before the header is rewritten would leave a
	// file whose keystream no longer matches its size, i.e. an archive nothing can
	// read. After the header is OPEN the size no longer matters.
	bool write_edits(const std::string& path, rpftoc::Table& table,
		const std::vector<PayloadEdit>& edits, std::string& error)
	{
		// The payloads go plaintext first, while the archive still has its
		// original length: the key that opens them is derived from the file as it
		// was, and appending anything would invalidate it.
		int plain = 0;
		if (plainify_enabled() && needs_plainify(table))
		{
			if (!plainify(path, table, plain, error))
				return false;
			if (plain > 0)
				logger::write("fivem", "[plain] %s: decrypted %d payload(s) in place, "
					"the archive is plaintext throughout now", path.c_str(), plain);
		}

		if (!table.isOpen() && !rpftoc::writeOpen(path, table, error))
			return false;

		for (const auto& edit : edits)
		{
			// The slack keeps the next, slightly larger revision of the same
			// payload from having to be appended a second time.
			const uint64_t need = edit.payload.size() + 4096ull;
			uint64_t offset =
				static_cast<uint64_t>(table.entryBlock(edit.index)) * rpftoc::kBlockSize;
			if (!region_is_free(table, edit.index, offset, need))
				offset = rpftoc::fileSize(path);

			if (!rpftoc::writePayload(path, offset, edit.payload, error))
				return false;

			table.setPayload(edit.index, static_cast<uint32_t>(offset / rpftoc::kBlockSize), 0,
				static_cast<uint32_t>(edit.payload.size()));

			// Round the end up so the archive keeps the sector alignment retail
			// files have.
			const uint64_t reserveEnd =
				(offset + need + rpftoc::kAlign - 1) & ~(rpftoc::kAlign - 1);
			if (rpftoc::fileSize(path) < reserveEnd &&
				!rpftoc::ensureFileSize(path, reserveEnd, error))
				return false;
		}

		if (!rpftoc::writeOpen(path, table, error))
			return false;

		// Read back through a fresh table: this is what proves the entries, the
		// payloads and the plaintext header agree. Without it a bad byte offset
		// would only show up as a crash inside the game.
		rpftoc::Table verify;
		if (!rpftoc::read(path, verify, error))
			return false;
		for (const auto& edit : edits)
		{
			const int index = verify.find(edit.path);
			std::string stored;
			if (index < 0 ||
				!rpftoc::readPayload(path, verify, static_cast<uint32_t>(index), stored, error) ||
				stored != edit.payload)
			{
				if (error.empty())
					error = std::string("the written ") + edit.path + " did not read back";
				return false;
			}
		}
		return true;
	}

	// Decides what the archive needs and writes it in one pass, so dlclist.xml and
	// gameconfig.xml are never two separate rewrites of a 1.9 GB file.
	bool install(const std::string& path, rpftoc::Table& table,
		const std::string& xml, const std::vector<std::string>& packs,
		const std::string& gameConfig, bool gameConfigEnabled, std::string& error)
	{
		const int listIndex = table.find(kDlclistPath);
		if (listIndex < 0)
		{
			error = std::string("no ") + kDlclistPath + " entry in the table of contents";
			return false;
		}

		int added = 0;
		int removed = 0;
		const std::string listPatched = build_patched(xml, packs, added, removed, error);
		if (listPatched.empty())
			return false;

		std::vector<PayloadEdit> edits;

		// The comparison is on content, not on size. A retuned pool table can be
		// exactly as long as the last one while carrying different numbers, and a
		// size check would then skip a write it should have made.
		const uint32_t listEntry = static_cast<uint32_t>(listIndex);
		const bool listCurrent = listPatched == xml;
		if (!listCurrent)
			edits.push_back({ listEntry, kDlclistPath, listPatched, patched_xml_path() });

		int limits = 0;
		std::string limitReport;
		int gcAdded = 0;
		std::string gcReport;
		const std::string gcPatched =
			build_gameconfig(gameConfig, gameConfigEnabled, gcAdded, gcReport);

		const int gcIndex = table.find(kGameConfigPath);
		bool gcCurrent = true;
		if (gcIndex >= 0 && !gameConfig.empty())
		{
			// build_gameconfig reports no change exactly when every value already
			// equals its target, so that is the whole condition.
			gcCurrent = gcAdded == 0;
			limits = gcAdded;
			limitReport = gcReport;
			if (!gcCurrent)
				edits.push_back({ static_cast<uint32_t>(gcIndex), kGameConfigPath, gcPatched,
					patched_gameconfig_path() });
		}
		else if (gcIndex < 0)
		{
			logger::write("fivem", "[gameconfig] %s has no %s entry, pool settings left alone",
				path.c_str(), kGameConfigPath);
		}

		// An archive whose table of contents is OPEN but whose payloads are still
		// ciphertext has to be repaired even when nothing else needs writing.
		const bool needsPlainify = plainify_enabled() && needs_plainify(table);

		if (edits.empty() && !needsPlainify)
		{
			logger::write("fivem", "[dlclist] %s: %zu bytes, every pack already registered",
				path.c_str(), listPatched.size());
			if (gcIndex >= 0 && !gameConfig.empty() && gcAdded == 0)
				logger::write("fivem", "[gameconfig] %s: pool settings already match",
					path.c_str());
			return true;
		}

		if (!write_edits(path, table, edits, error))
			return false;

		for (const auto& edit : edits)
			write_text(edit.dump, edit.payload);

		if (!listCurrent)
			logger::write("fivem", "[dlclist] %s: +%d -%d pack(s), %zu bytes (%s)",
				path.c_str(), added, removed, listPatched.size(),
				table.isOpen() ? "table was already plaintext" : "converted to plaintext");
		if (limits > 0 && !gcCurrent)
			logger::write("fivem", "[gameconfig] %s: adjusted %d pool setting(s): %s",
				path.c_str(), limits, limitReport.c_str());
		return true;
	}
}

namespace dlclist
{
	void Reserve(const std::vector<std::string>& packs)
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		g_packs = packs;
	}

	void Prepare()
	{
		std::vector<std::string> packs;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			packs = g_packs;
		}

		// An empty reservation is not the same as "nothing to do": it means every
		// resource is gone, and the entries left behind by earlier runs have to be
		// removed. Prepare is only reached after a scan, so this is safe.
		const fs::path mods = mods_path();
		const fs::path stock = stock_path();
		std::string error;

		if (!write_allowed(mods))
		{
			logger::write("fivem", "[dlclist] %s is the stock archive; set "
				"[fivem] patchStockUpdateRpf=true to let the loader edit it",
				mods.string().c_str());
			return;
		}

		std::error_code ec;
		if (packs.empty() && !fs::is_regular_file(mods, ec))
		{
			// Nothing to register and no mods archive to clean: seeding a 1.9 GB
			// copy just to prune an empty list would be pure cost.
			return;
		}

		if (!fs::is_regular_file(mods, ec))
		{
			if (!fs::is_regular_file(stock, ec))
			{
				logger::write("fivem", "[dlclist] neither %s nor %s exists",
					mods.string().c_str(), stock.string().c_str());
				return;
			}

			// One 1.9 GB copy, once. After this the stock archive is never touched.
			const Stamp before = stat_file(stock);
			fs::create_directories(mods.parent_path(), ec);
			ec.clear();
			fs::copy_file(stock, mods, fs::copy_options::overwrite_existing, ec);
			if (ec)
			{
				logger::write("fivem", "[dlclist] cannot copy %s to %s: %s",
					stock.string().c_str(), mods.string().c_str(), ec.message().c_str());
				return;
			}
			logger::write("fivem", "[dlclist] seeded %s from the stock archive (%llu bytes, %llums)",
				mods.string().c_str(),
				static_cast<unsigned long long>(rpftoc::fileSize(mods.string())),
				static_cast<unsigned long long>(GetTickCount64()));
			(void)before;
		}

		rpftoc::Table table;
		if (!read_table(mods.string(), table, error))
		{
			logger::write("fivem", "[dlclist] %s: %s", mods.string().c_str(), error.c_str());
			i18n::show(i18n::Message::ConversionFailed);
			return;
		}

		const int index = table.find(kDlclistPath);
		if (index < 0)
		{
			logger::write("fivem", "[dlclist] %s has no %s entry", mods.string().c_str(),
				kDlclistPath);
			i18n::show(i18n::Message::ConversionFailed);
			return;
		}

		std::string xml;
		if (!read_list(mods.string(), table, static_cast<uint32_t>(index), xml, error))
		{
			logger::write("fivem", "[dlclist] %s: %s", mods.string().c_str(), error.c_str());
			i18n::show(i18n::Message::ConversionFailed);
			return;
		}

		// gameconfig.xml is read in the same pass and written together with the
		// list: both live in this archive, and rewriting a 1.9 GB file twice per
		// start is a needless risk.
		//
		// The switch means "apply or revert", not "on or off": turning it off
		// writes the stock values back, so a table this loader inflated can always
		// be undone without unpacking the archive by hand.
		const bool gameConfigEnabled = config::get_value<bool>("fivem", "patchGameConfig", true);
		if (!gameConfigEnabled)
			logger::write("fivem", "[gameconfig] pool patch disabled: restoring stock values");

		std::string gameConfig;
		{
			const int gcIndex = table.find(kGameConfigPath);
			if (gcIndex < 0)
			{
				logger::write("fivem", "[gameconfig] %s has no %s entry", mods.string().c_str(),
					kGameConfigPath);
			}
			else if (!read_xml(mods.string(), table, static_cast<uint32_t>(gcIndex),
					kGameConfigPath, gameConfig, error))
			{
				// A readable archive matters more than the pool patch: log it and
				// register the packs anyway.
				logger::write("fivem", "[gameconfig] %s: %s", mods.string().c_str(),
					error.c_str());
				error.clear();
				gameConfig.clear();
			}
		}

		if (!install(mods.string(), table, xml, packs, gameConfig, gameConfigEnabled, error))
		{
			logger::write("fivem", "[dlclist] %s: %s", mods.string().c_str(), error.c_str());
			i18n::show(i18n::Message::ConversionFailed);
		}
	}
}
