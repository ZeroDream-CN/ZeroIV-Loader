#include "main.h"

static const int OPEN = 0x4E45504F;

uint32_t currentEncryption;
bool FindEncryptionHook(uint32_t encryption)
{
	currentEncryption = encryption;
	return (encryption & 0xFF00000) == 0xFE00000;
}

void(*DecryptHeaderOrig)(uint32_t, char*, int);
void DecryptHeaderHook(uint32_t salt, char* entryTable, int size)
{
	if (currentEncryption == OPEN)
	{
		logger::log("mods", "[%s] not encrypted RPF found", __FUNCTION__);
		return;
	}
	logger::log("mods", "[%s] called", __FUNCTION__);
	DecryptHeaderOrig(salt, entryTable, size);
}

void(*DecryptHeader2Orig)(uint32_t, uint32_t, char*, int);
void DecryptHeader2Hook(uint32_t encryption, uint32_t salt, char* header, int nameTableLen)
{
	if (encryption == OPEN)
	{
		logger::log("mods", "[%s] not encrypted RPF found", __FUNCTION__);
		return;
	}
	logger::log("mods", "[%s] called", __FUNCTION__);
	DecryptHeader2Orig(encryption, salt, header, nameTableLen);
}

bool(*ParseHeaderOrig)(rage::fiPackfile*, const char*, bool, void*);
bool ParseHeaderHook(rage::fiPackfile* a1, const char* name, bool readHeader, void* customHeader)
{
	bool ret = ParseHeaderOrig(a1, name, readHeader, customHeader);
	logger::log("mods", "[%s] parsed header /%s", __FUNCTION__, name);
	if (name)
		forensics::note_file(name);
	if (ret)
	{
		if (IsEnhanced()) {
			// fiPackfile on Enhanced is shifted by 0x08
			a1 = reinterpret_cast<rage::fiPackfile*>(reinterpret_cast<uintptr_t>(a1) + 0x8);
		}
		logger::log("mods", "[%s] fileCount /%d", __FUNCTION__, a1->filesCount);

		// The archive records its own encryption; trusting the global set by
		// FindEncryptionHook is not safe because Enhanced streams archives from
		// several threads at once. A stale OPEN from the unencrypted mod pack
		// would make an encrypted stock archive be treated as plain text, and
		// the resources read out of it are garbage.
		const bool openByArchive = a1->encryptionMagic == OPEN;
		const bool openByGlobal = currentEncryption == OPEN;

		// A bogus count would walk off the entry table and corrupt the heap.
		const int count = a1->filesCount;
		const bool sane = count >= 0 && count <= 1000000 && a1->entryTable != nullptr;
		if (!sane)
		{
			logger::write("mods", "[%s] refusing archive /%s: filesCount=%d entryTable=%p",
				__FUNCTION__, name, count, (void*)a1->entryTable);
			return ret;
		}

		const bool open = openByArchive || openByGlobal;
		int patched = 0;

		// A mod archive reaching this point has been opened and its entry table
		// parsed, which is what "the game can actually see the models" means.
		// Logged unconditionally for mod packs: if a car never appears, this
		// line decides whether the engine rejected the archive or never got to
		// it, and that is the difference between a bad model and a bad loader.
		if (name && strstr(name, "nisa31"))
			logger::write("pack", "[%s] parsed %s: %d entries, open=%d, table=%p",
				__FUNCTION__, name, count, (int)open, (void*)a1->entryTable);

		// GTA V parses an OPEN (unencrypted) RPF7 by itself, so poking the entry
		// table is only needed on builds that reject them. The switch lets that
		// be ruled out without a rebuild when a mod archive misbehaves.
		static const bool allowPatching = config::get_value<bool>("config", "allowUnencryptedRpf", true);
		if (open && allowPatching)
		{
			for (int i = 0; i < count; ++i)
			{
				Entry* v21 = (Entry*)(a1->entryTable + 16 * i);
				if (v21->IsBinary() && v21->bin.nameOffset > 0 && v21->bin.isEncrypted)
				{
					v21->bin.isEncrypted = 0xFEFFFFF;
					++patched;
				}
			}

			a1->currentFileOffset = 0xFEFFFFF;
		}

		// Unencrypted archives are rare (only the mod packs), so this stays
		// always-on: it is the line that tells whether a mod archive was
		// actually accepted. The fallback marker flags the cross-thread case.
		if (openByArchive || patched > 0)
		{
			logger::write("mods", "[%s] unencrypted archive /%s: %d entries, patched %d%s%s",
				__FUNCTION__, name, count, patched,
				openByArchive ? "" : " (stale global, no per-archive marker)",
				allowPatching ? "" : " (patching disabled by config)");
		}
	}
	return ret;
}

static memory::InitFuncs PackfileEncryptionHooks([] {
	// Allow unencrypted RPFs
	if (IsEnhanced()) {
		memory::scan("e8 ? ? ? ? 80 7c 24 2e ? 74 ? 48 8b 56").set_call(FindEncryptionHook);

		auto mem = memory::scan("e8 ? ? ? ? 8b 46 ? 85 c0 74 ? b9");
		DecryptHeaderOrig = mem.add(1).rip().as<decltype(DecryptHeaderOrig)>();
		mem.set_call(DecryptHeaderHook);

		mem = memory::scan("e8 ? ? ? ? 48 8b 46 ? 66 c7 00 00 ? eb"); // 
		DecryptHeader2Orig = mem.add(1).rip().as<decltype(DecryptHeader2Orig)>();
		mem.set_call(DecryptHeader2Hook);
		mem = memory::scan("e8 ? ? ? ? 48 8b 46 ? 8b 4c 24 ? 48 8d 14 08");
		mem.set_call(DecryptHeader2Hook);

		mem = memory::scan("c6 86 ? ? ? ? ? 48 89 f1 4c 89 fa 45 89 f0 45 31 c9").add(19);
		ParseHeaderOrig = mem.add(1).rip().as<decltype(ParseHeaderOrig)>();
		mem.set_call(ParseHeaderHook);
	}
	else {
		memory::scan("E8 ? ? ? ? 48 8B 53 20 44 8B C7 41 8B CE E8").set_call(FindEncryptionHook);

		auto mem = memory::scan("E8 ? ? ? ? 41 8B D4 44 39 63 28 76 3F 41 B9");
		DecryptHeaderOrig = mem.add(1).rip().as<decltype(DecryptHeaderOrig)>();
		mem.set_call(DecryptHeaderHook);

		mem = memory::scan("E8 ? ? ? ? 8B 55 F8 48 8B 43 10 48 03 D0 48 8B CB 48 89 53 18 66 44 89 22 33 D2 E8");
		DecryptHeader2Orig = mem.add(1).rip().as<decltype(DecryptHeader2Orig)>();
		mem.set_call(DecryptHeader2Hook);

		mem = memory::scan("44 88 BB ? ? ? ? 89 43 58 E8 ? ? ? ? 4C 8D 9C 24 ? ? ? ? 49 8B 5B 38 49 8B 73 40 49 8B 7B 48 49 8B E3 41 5F 41 5E 41 5D 41 5C 5D C3").add(10);
		ParseHeaderOrig = mem.add(1).rip().as<decltype(ParseHeaderOrig)>();
		mem.set_call(ParseHeaderHook);
	}
});