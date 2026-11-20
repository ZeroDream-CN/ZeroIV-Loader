# FiveM resource loading: findings and current state

Measured on `GTA V Legacy 1.0.3889.0`. Status below is what is true **now**,
not earlier guesses that were walked back.

## Current status (2026-11-20)

| Item | State |
| --- | --- |
| Boot | Yes — story mode loads, no `ERR_GEN` / `ERR_SYS` |
| Map | **Still fully missing** |
| Add-on packs | 18 `ziv_*` armed in `dlclist.xml` (spawn not re-verified today) |
| `mods/update/update.rpf` | Fully OPEN TOC; **728** `flag==1` payloads decrypted in place; **68** entries keep non-`0`/`1` flags (mostly map `.ymt` with `0x20000000`) |
| Config | `[fivem] enabled=true`, `plainifyRpf=true` (default on); `[config] allowUnencryptedRpf=true` |
| `ParseHeaderHook` | Logs OPEN archives only; **does not** stamp `0xFEFFFFF` onto entries or `currentFileOffset` |

Working recovery if you need a visible map tomorrow: restore
`update/update.rpf` → `mods/update/update.rpf` and set `[fivem] enabled=false`.
Stock NG archive + fivem off → map OK (confirmed earlier today).

## What actually broke the map (ordered by evidence)

1. **Pool downgrade** (fixed) — `raise_number()` used to write stock ceilings
   over larger community values (`DrawableStore 96000→81000`, etc.). World went
   low-poly, then vanished. Now raise-only.
2. **Hybrid OPEN TOC + still-NG payloads + in-memory stamp** — converting only
   the TOC to OPEN, leaving ciphertext, then stamping `0xFEFFFFF` on entry flags
   and/or `currentFileOffset` either crashed (`ERR_GEN_INVALID` with no stamp)
   or booted with a **missing map** (with stamp). Abandoned as the supported path.
3. **Plainify that treated every non-zero flag as NG** — CodeWalker only allows
   encryption type `0` or `1`. About **68** binary-looking entries carry other
   values (e.g. `0x20000000` on district `.ymt`). Decrypting or zeroing those
   destroyed map data / produced `ERR_SYS_INVALIDRESOURCE_5`. Fixed:
   `entryIsEncrypted` is now `flag == 1` only; plainify skips the rest.
4. **Broken `entryIsResource`** (fixed) — old test almost never matched real
   resources; plainify could zero `graphicsFlags`. Now uses bit 23 of the
   24-bit offset.

## What was tried today

| Approach | Result |
| --- | --- |
| Stock NG `update.rpf`, fivem off, packs off | Map **visible** |
| OPEN TOC only, payloads NG, no stamp | `ERR_GEN_INVALID` |
| OPEN TOC + stamp entry flags / `currentFileOffset` to `0xFEFFFFF` | Boots, map **gone** |
| Full plainify (flag==1 only), OPEN TOC, no stamp, 18 packs | Boots (~1.7 GB), **no ERR_**, map **still gone** |

So a genuine plaintext `update.rpf` is no longer fatal, but it is also **not
sufficient** for the world map. Next bisects should separate:

- plainify alone (no pack list / empty `dlclist` ziv entries)
- the 68 non-`0`/`1` flag entries (leave vs rewrite vs treat as resources)
- `gameconfig` raise-only values vs stock
- whether map `.ymt` payloads with flag `0x20000000` need a different open path

## Code / config that matches this doc

- `plainifyRpf` default **true** — primary path rebuilds a plaintext OPEN archive
- `entryIsEncrypted` → `flag == 1` only (`rpftoc.cpp`)
- `ParseHeaderHook` — no per-entry / archive NG stamp for OPEN files
- `HeapHook` removed earlier (never patched; 27 ambiguous sites)

## Earlier measurements still useful

| Claim | Measurement |
| --- | --- |
| Pool code could downgrade a tuned install | Logged `adjusted 13 pool setting(s)` shrinking store pools |
| Flag-1 decrypt matches stock | Full-path compare after decrypt: intentional XML edits differ; flag-1 payloads match |
| Retail TOC is not “all flags zero” | After decrypting the NG TOC: hundreds of `flag==1`, plus ~68 other non-zero values on map-related names |
| `HeapHook` never patched | Always `heap budget site is ambiguous` |
