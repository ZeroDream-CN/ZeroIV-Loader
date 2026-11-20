# FiveM resource loading: findings and current state

Measured on `GTA V Legacy 1.0.3889.0`.

## Current status (2026-11-20) — working

| Item | State |
| --- | --- |
| Boot | Story mode loads |
| Map | **Visible** |
| Mod vehicles | **Spawnable** |
| `mods/update/update.rpf` | Fully OPEN; flag==1 payloads plainified in place |
| Packs | `ziv_*` armed via dlclist (this session verified with 10 packs) |
| HeapAdjuster | Optional; installed at 1024 MiB. Does **not** make RDE-sized store raises safe |
| Config | `[fivem] enabled=true`, `plainifyRpf=true`, `patchGameConfig=true` |

### Safe `gameconfig` raises

- Vehicle ceilings: `MaxVehicleModelInfos`, `MaxExtraVehicleModelInfos`, `ArchiveCount`, `VehicleStreamRequest` / `Render`, `VehicleStruct`, `HandlingData`
- Modest only: `FragmentStore` 42000→48000, `MetaDataStore` 3200→4000

### Unsafe (erases the map on this install)

Raising `DrawableStore`, `TxdStore`, `DwdStore`, or `fwArchetypePooledMap` to the old RDE-sized targets (e.g. 96000). HeapAdjuster does not fix that. Those rows stay in `kPoolLimits` with `target == stock` so `patchGameConfig=false` can still revert a previously raised archive.

### Crash when packs mount with stock Fragment/Meta pools

With 18 packs and stock `FragmentStore`/`MetaDataStore`, the game AV'd while opening `dlc_ziv_*/x64/stream.rpf`. Crashcontext showed both pools at capacity. The modest bumps above cleared that path.

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
- `kPoolLimits` — vehicle raises + modest Fragment/Meta only
