# NG key tables

`src/resources/ngdata.bin` is **not in this repository**. It is Rockstar's key
material, read out of the retail game, and a public GPL tree is the wrong place
for it.

## What the build does without it

Nothing breaks. `CMakeLists.txt` writes `src/resources/ngdata.rcinc` at configure
time, and emits the `NGDATA` resource only when the file is present. Verified
both ways:

| `ngdata.bin` | `ZeroIV.asi` | CIPHER |
| --- | --- | --- |
| present | 1.04 MB | available |
| absent | 0.75 MB | `ngcrypt::init()` fails, every caller reports it and stops |

Without the tables the loader still starts, reads archives and patches
`gameconfig.xml`; only the paths that need to decrypt an NG payload refuse to
run. That is the intended degradation.

## Layout

`ngcrypt.cpp` expects one flat blob, exactly `kTotalBytes` = 306,256 bytes:

| offset | size | contents |
| --- | --- | --- |
| `0` | 256 | byte lookup table used to build the round tables |
| `256` | 101 x 272 | the NG round keys, one per key index |
| `27,728` | 17 x 16 x 256 x 4 | the round tables, 17 rounds of 16 rows of 256 dwords |

The cipher is a straight port of CodeWalker's `GTA5Crypto`, which is where the
layout and the byte/word order come from. Any generator that reproduces those
three regions byte for byte is compatible.

## How to get one

The key material lives inside `GTA5.exe`, which is packed on disk: an offline
scan of the file finds nothing, because the code that holds the tables is not in
the on-disk image. It has to be read out of the process after the game has
loaded, which is something this loader can do and a standalone tool cannot.

That extraction is **not implemented yet**. Until it is, a build needs a blob
produced by hand and placed at `src/resources/ngdata.bin`. The intended end state
is that the first run reads the tables from the running process, caches them
next to the logs, and the embedded resource disappears entirely -- which also
retires this document's first paragraph.

## Why not just ship it

The repository is public and GPL-3.0. Redistributing key material extracted from
a commercial game is the kind of thing that attracts a takedown, and it is not
needed: the tables are reachable at runtime on any machine that can run the
loader in the first place.
