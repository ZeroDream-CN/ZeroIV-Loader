#pragma once

#include <string>
#include <vector>

// Dynamic dlclist.xml registration and optional gameconfig.xml pool raises.
//
// gameconfig.xml handling (highest priority first):
//   1. unpack-mods/gameconfig.xml — installed verbatim; ASI raises skipped
//   2. [fivem] patchGameConfig=off|false — leave the archive entry alone
//   3. patchGameConfig=true — raise listed pools (never lower a larger value)
//
// Dynamic dlclist.xml registration.
//
// A generated DLC is useless until the engine is told to mount it, and that list
// lives inside update.rpf. The archive's table of contents is NG-encrypted, and
// its key index mixes the file name with the *total file size*, so appending a
// single byte to update.rpf turns it into a file nothing can read. That is why
// every earlier in-place attempt crashed the game.
//
// So the loader never appends to an NG archive. It reads the encrypted table of
// contents, decrypts it with the NG cipher (utils/ngcrypt), and writes the result
// back as a plaintext (OPEN) archive -- the format OpenIV has always produced and
// the game loads natively. Only then can the list be extended, as an ordinary
// file edit, before the engine opens the archive:
//
//   1. add the missing <Item> lines to dlclist.xml;
//   2. store the result uncompressed in a reserved tail region;
//   3. point the dlclist.xml entry at it and write the table back.
//
// Everything happens in plain file I/O at start-up, so there is no memory
// patching, no restart and no participation from the engine. A payload that is
// left NG-encrypted keeps working: its key index depends on the entry's own name
// and uncompressed size, neither of which changes here.
//
// Only the mods copy is written. The stock archive is the file Steam verifies and
// is read once, to seed that copy.
namespace dlclist
{
	// Pack names (dlcpacks/<pack>/) that must be present in dlclist.xml.
	// Includes generated ziv_* packs and traditional add-on folders discovered
	// under mods/ or unpack-mods/dlcpacks/.
	//
	// `pruneAddons` — traditional packs this loader previously auto-registered
	// that are gone from disk; only those lines are removed (never hand-edited
	// entries). `claimableAddons` — traditional packs discovered this run; any
	// line actually inserted for them is reported by TakeClaimedAddons().
	void Reserve(const std::vector<std::string>& packs,
		const std::vector<std::string>& pruneAddons = {},
		const std::vector<std::string>& claimableAddons = {});

	// Converts mods/update/update.rpf to a plaintext table of contents and
	// registers every reserved pack. Called before the engine opens the archive.
	void Prepare();

	// Traditional pack names whose <Item> lines were inserted by the last
	// Prepare(). Empty when nothing new was claimed.
	std::vector<std::string> TakeClaimedAddons();
}
