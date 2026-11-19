# FiveM resource loading: findings, and the one bug that actually broke the game

Status: the loader's `[fivem]` path is **off** and `mods/update/update.rpf` is a
pristine retail copy. The game boots in that state. Everything below is what was
measured on `GTA V Legacy 1.0.3889.0`, not inferred.

## TL;DR

The world going low-poly, then vanishing, then add-on vehicles reporting "model
not found" was caused by **this loader writing smaller pool values into
`gameconfig.xml`**, not by anything about RPF encryption. That bug is fixed:
limits are now raised only, never lowered.

The archive surgery (converting the table of contents to `OPEN`) is a separate,
older decision that works as long as the engine is told the payloads are
readable. It was not the cause of the map problem.

## What was measured

| Claim | Measurement |
| --- | --- |
| The pool code downgraded an already-tuned install | `gameconfig.xml` said `DrawableStore 96000`, `TxdStore 96000`, `FragmentStore 52000`, `DwdStore 26000`, `fwArchetypePooledMap 80000`. One loader run rewrote all five down to stock (`81000/80200/42000/20500/64000`) and reported it: `adjusted 13 pool setting(s)` |
| Decrypting payloads in place is correct | After decrypting 794 payloads, every file in the archive was compared with the retail original by full path: **1044 of 1046 byte-identical**, the two exceptions being the `dlclist.xml` and `gameconfig.xml` the loader rewrites on purpose |
| A freshly restored (NG) archive converted to `OPEN` cannot be read | Converted a pristine `update.rpf` to `OPEN` with payloads still NG-encrypted: `ERR_GEN_INVALID` on every start |
| A fully plaintext archive is rejected too | `OPEN` header, all payloads decrypted, all entry flags zero: `ERR_SYS_INVALIDRESOURCE_5` |
| Per-entry encryption flags are not the mechanism | The retail archive has **zero** flagged entries (`entryFlags == 0` on all 1644), yet its payloads are ciphertext. NG encryption is applied from the archive header, not per entry |
| `HeapHook` never did anything | Every run logged `heap budget site is ambiguous, not patching: 27 candidate(s)`, i.e. it declined and wrote nothing |

## The bug that was fixed

`raise_number()` in `src/hooks/DlclistHook.cpp` overwrote any value that differed
from its target, including values that were already larger. A comment rationalised
it as "two-way on purpose, so a table that only ever grew could never be
retuned". In practice it retuned the install downwards.

It now takes `raiseOnly`: when the loader applies its own settings it never
shrinks an entry, and only the revert path (`patchGameConfig=false`) writes
unconditionally. The pool targets were also restored to the figures this install
ran with while every add-on vehicle still loaded.

## A false lead worth recording

Roughly an hour went into "the marker patch turns map `.ymt` files into garbage".
The reasoning was: `ParseHeaderHook` stamped `0xFEFFFFF` onto entry flags, the
engine then read ciphertext as plaintext. It was wrong, and the time-table shows
why — the map was already going low-poly **before** that code was ever enabled,
and the disappearance coincided exactly with the pool downgrade.

Two corrections that came out of it, both worth keeping:

1. `0x0FEFFFFF` is not a value that belongs on disk. Every archive this loader
   generates writes `encryptionType = 0` for a payload stored as-is
   (`loosepack.cpp`), and those archives mount fine. Writing `0x0FEFFFFF` to the
   file produced `ERR_GEN_INVALID` with the loader disabled, which is exactly an
   archive the engine cannot make sense of.
2. Fully plaintext archives still fail. So the working design is the one that was
   already there: `OPEN` table of contents, payloads left as they are, entries
   marked in memory at parse time.

## Removed

`src/hooks/HeapHook.cpp` and the `heapSize` config key. It scanned for a
`mov r32, imm32` before a call and would have patched the immediate if exactly
one candidate turned up; with 27 candidates it always declined, so it had no
effect. Deleting it is not a fix — it is removing a component that could only
ever cause a crash, never prevent one. A heap limit adjuster is not needed at the
pool sizes used here anyway.

## Open question

`ERR_SYS_INVALIDRESOURCE_5` appears once the archive is genuinely plaintext. Its
documented meaning is a `.y*` resource stored as a binary entry, losing its RSC7
header. `update.rpf` contains no resource entries at all (1644 entries, 0
resources), but the 18 generated `ziv_*` packs contain nothing but `.yft` and
`.ytd`.

So the next thing to test is the generated pack structure, not the archive. The
cheapest bisect: generate zero packs (empty `unpack-mods`) **and** clear the
`ziv_*` items out of `dlclist.xml`, then boot. If that succeeds, the fault is in
`dlcgen`/`loosepack` and the 18 packs are reproducible test cases.

## Recovery state at time of writing

- `mods/update/update.rpf`: 2,014,193,664 bytes, byte-identical to retail
- `[fivem] enabled=false`, `plainifyRpf=false`
- `[config] allowUnencryptedRpf=true`
- `mods/update/x64/dlcpacks/`: 18 `ziv_*` pack directories from earlier runs
- game boots, no crash
