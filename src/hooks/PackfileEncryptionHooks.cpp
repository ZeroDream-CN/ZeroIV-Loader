#include "main.h"

static const int OPEN = 0x4E45504F;

// The encryption type of the archive currently being parsed on this thread.
// FindEncryptionHook runs just before ParseHeader for the same archive, so a
// thread-local captures that value without the cross-thread stale-global
// problem that a plain static has on Enhanced (several archives parse at once).
thread_local uint32_t g_parseEncryption = 0;

uint32_t currentEncryption;
bool FindEncryptionHook(uint32_t encryption)
{
	currentEncryption = encryption;
	g_parseEncryption = encryption;
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

		// After ParseHeaderOrig the packfile's encryptionMagic field is not
		// reliably still "OPEN": the engine may rewrite it to its internal
		// sentinel. The value FindEncryptionHook saw for *this* parse is the
		// one that decides whether the archive on disk is plaintext-TOC.
		const bool openByParse = g_parseEncryption == OPEN;
		const bool openByArchive = a1->encryptionMagic == OPEN;
		const bool isOpen = openByParse || openByArchive;
		// A bogus count would walk off the entry table and corrupt the heap.
		const int count = a1->filesCount;
		const bool sane = count >= 0 && count <= 1000000 && a1->entryTable != nullptr;
		if (!sane)
		{
			logger::write("mods", "[%s] refusing archive /%s: filesCount=%d entryTable=%p",
				__FUNCTION__, name, count, (void*)a1->entryTable);
			return ret;
		}

		// Fully plaintext OPEN archives (plainifyRpf, and every ziv_* pack)
		// need no marker. A hybrid OPEN TOC with still-encrypted payloads is
		// no longer the supported path: stamping NG here made the map vanish
		// or crashed when the stamp was skipped. Leave OPEN archives alone.
		if (isOpen)
		{
			logger::write("mods", "[%s] unencrypted archive /%s: %d entries"
				" (parse=0x%08X magic=0x%08X)",
				__FUNCTION__, name, count,
				g_parseEncryption, a1->encryptionMagic);
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
