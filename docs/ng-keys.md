# NG key tables

Plaintext `ngdata.bin` is **not in this repository**. It is Rockstar's key
material. The loader obtains working tables at runtime instead.

## How the loader gets the tables

`ngcrypt::init()` tries sources in this order:

1. **Embedded `NGDATA` resource** — only when a local `src/resources/ngdata.bin`
   existed at configure time (optional offline/dev builds).
2. **`ZeroIV.ngdata.bin`** next to `ZeroIV.log` — written after a successful
   extract so later boots skip the work.
3. **`magic.dat` unlock** — the obfuscated blob embedded as
   `src/utils/ngmagic_data.cpp` (same material CodeWalker / gtalib-go ship).
   The retail AES key is found by SHA-1 in the live modules; JenkHash + the
   .NET PRNG unscramble the blob; AES-256-ECB + raw deflate yield the tables.
   This is what works on **GTA V Enhanced** (and Legacy).
4. **Module SHA-1 scan** — digests in `src/utils/nghashes.cpp` (CodeWalker
   `GTA5KeyHashes`). Useful on some Legacy dumps; Enhanced usually has no
   matching plaintext blobs, so this is a fallback only.

## Layout

`ngcrypt.cpp` expects one flat blob, exactly `kTotalBytes` = 306,256 bytes:

| offset | size | contents |
| --- | --- | --- |
| `0` | 256 | byte lookup table used to build the round tables |
| `256` | 101 x 272 | the NG round keys, one per key index |
| `27,728` | 17 x 16 x 256 x 4 | the round tables, 17 rounds of 16 rows of 256 dwords |

## Optional local embed

For machines that cannot run a first extract, place a 306,256-byte blob at
`src/resources/ngdata.bin` and reconfigure. The resource path stays first.

## Regenerating the magic embed

```bash
# After updating src/resources/magic.dat from CodeWalker / gtalib-go:
python3 - <<'PY'
from pathlib import Path
data = Path('src/resources/magic.dat').read_bytes()
# ... rewrite src/utils/ngmagic_data.cpp (see git history / prior embed script)
PY
```

## Why not ship plaintext keys

The repository is public and GPL-3.0. Redistributing raw key tables invites
takedowns. The obfuscated `magic.dat` form is what existing open tools already
distribute; plaintext `ngdata.bin` stays optional and untracked.
