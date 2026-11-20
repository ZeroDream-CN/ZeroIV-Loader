# FiveM resource loading: findings and current state

Player-facing setup is in [fivem-mods.md](fivem-mods.md). This file is the
engineering record (what was tried, what wiped the map, pool tables).

Measured on `GTA V Legacy 1.0.3889.0`.

## Current status (2026-11-20) — working (full RDE recipe)

| Item | State |
| --- | --- |
| Boot | Story mode loads |
| Map | **Visible** |
| Mod vehicles | **Spawnable** |
| `mods/update/update.rpf` | OPEN; `gameconfig.xml` = byte-identical pnw `versions/3889` |
| HeapAdjuster | **2048** MiB |
| PackfileLimitAdjusterEnhanced | Required (`PACKFILEARRAY_SIZE=13000`) |
| Config | `[fivem] enabled=true`, `plainifyRpf=true`, `patchGameConfig=true` |

### Working recipe

1. Full `gameconfig.xml` from [pnwparksfan/gameconfig](https://github.com/pnwparksfan/gameconfig) `versions/3889` (not per-tag only — adds ~41 pools).
2. HeapAdjuster **2048** + PackfileLimitAdjusterEnhanced (`.asi` must be present; ini alone is not enough).
3. Loader `kPoolLimits` / `kAttributeLimits` target RDE; `raiseOnly` preserves an already-replaced XML.

Earlier map wipe: RDE-sized streaming stores at Heap **1024** without Packfile ASI. That combo failed; the full upstream stack succeeds.

### Player-owned `gameconfig`

| Priority | Behavior |
| --- | --- |
| `unpack-mods/gameconfig.xml` present | Copied into `update.rpf` as-is; ASI pool raises skipped |
| `[fivem] patchGameConfig=off` (or `false`) | Leave archive `gameconfig.xml` untouched |
| `patchGameConfig=true` (default) | Raise listed pools only (never lower) |

`dlclist.xml` is unchanged: only `dlcpacks:/ziv_*` entries are managed; other items stay.

## Reference: [pnwparksfan/gameconfig](https://github.com/pnwparksfan/gameconfig) `versions/3889`

Full machine diff vs retail `update/update.rpf` `common/data/gameconfig.xml` (extracted 94726 bytes) is in `scratch/gameconfig-compare/FULL-DIFF.md`.

| | Stock | RDE / pnw 3889 |
| --- | ---: | ---: |
| Named pools | 143 | 183 |
| Existing pools raised | — | **71** |
| Pools added (new names) | — | **41** |
| Pools removed | — | 1 (`Object` 2300) |
| Key model-info / density attrs raised | — | **24** (+ minor `PhysicalStreamingBuffer` / 3× `SizeOfStack`) |

Also requires companion ASIs: Heap Limit Adjuster, Packfile Limit Adjuster, optional WeaponInfo / PedProp / Siren. XML alone is not their full recipe.

### What they raise hardest (pools)

| Name | Stock | RDE | Ours |
| --- | ---: | ---: | ---: |
| FragmentStore | 42000 | 299000 | 48000 |
| DwdStore | 20500 | 153800 | 20500 |
| TxdStore | 80200 | 213500 | 80200 |
| DrawableStore | 81000 | 161965 | 81000 |
| fwDynamicArchetypeComponent | 16384 | 65521 | — |
| MapDataStore | 10020 | 50000 | — |
| StaticBounds | 14700 | 46700 | — |
| fwDynamicEntityComponent | 8192 | 40000 | — |
| ActiveLoadedInfo / MaxLoadRequestedInfo | 3000 | 24000 | — |
| HandlingData | 1800 | 5450 | 5450 |
| MetaDataStore | 3200 | 6200 | 4000 |
| … + 60 more raised pools | | | see FULL-DIFF |

### Attribute raises we do / don’t copy

| Name | Stock | RDE | Ours |
| --- | ---: | ---: | ---: |
| MaxVehicleModelInfos | 320 | 5000 | 5000 |
| MaxExtraVehicleModelInfos | 640 | 5225 | 5225 |
| ArchiveCount | 4440 | 6024 | 4800 |
| MaxPed / Weapon / Mlo model infos | low | ~5000 each | not patched |
| MaxMloInstances | 18000 | 28000 | — |
| Vehicle parked density / upper limits | 100/40 | 130/50 | — |

Vehicle stream pools (`VehicleStreamRequest`/`Render` 240, `VehicleStruct` 600) match RDE. Do **not** copy their streaming-store raises here — measured map wipe even with HeapAdjuster 1024.

## Paths that failed (do not revive)

| Approach | Result |
| --- | --- |
| Hybrid OPEN TOC + NG payloads + stamp `0xFEFFFFF` | Boot or map gone |
| Plainify treating every non-zero flag as NG | Corrupts map `.ymt` / `ERR_SYS_INVALIDRESOURCE_5` |
| RDE-sized streaming store raises | Map disappears |
| Pool **downgrade** (fixed earlier) | Low-poly then vanish |

## Code that matches this doc

- `plainifyRpf` default **true**
- `entryIsEncrypted` → `flag == 1` only; `entryIsResource` uses offset bit 23
- `ParseHeaderHook` — no NG stamp on OPEN archives
- `kPoolLimits` / `kAttributeLimits` — RDE targets; skipped if user `gameconfig.xml` or `patchGameConfig=off`
