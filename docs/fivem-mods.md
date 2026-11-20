# Add-on vehicles (FiveM resources)

ZeroIV-Loader can turn **FiveM-style vehicle resources** into story-mode DLC
packs and register them automatically. This page is for players.

Verified on **GTA V Legacy 1.0.3889**. Enhanced may differ; use matching
companion tools for your build.

## What you need

| Piece | Role |
| --- | --- |
| `ZeroIV.asi` | Scans packs, builds `ziv_*` DLC, patches `dlclist.xml`, optionally raises `gameconfig.xml` pools |
| ASI loader | e.g. [Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) |
| [Heap Limit Adjuster](https://www.gta5-mods.com/tools/heapadjuster) (or Enhanced) | More streaming memory. **2048** MiB is a working starting point with a full RDE-style `gameconfig` |
| [Packfile Limit Adjuster Enhanced](https://www.gta5-mods.com/scripts/packfile-limit-adjuster-enhanced) | Raises how many RPF archives the game can mount. The `.asi` must be present; an `.ini` alone does nothing |

Without Heap + Packfile adjusters, a large `gameconfig` (many streaming stores)
can make the map disappear or packs fail to mount. That is a game limit issue,
not a missing vehicle file.

## Install a vehicle pack

1. Put each FiveM resource in its own folder under `unpack-mods/`:

   ```
   Grand Theft Auto V/
     unpack-mods/
       nisa31/          ← folder name is yours
         fxmanifest.lua   (or __resource.lua)
         data/
         stream/
       f90/
         ...
   ```

2. Launch the game once. ZeroIV:
   - builds `mods/update/x64/dlcpacks/ziv_<name>.../dlc.rpf`
   - adds `dlcpacks:/ziv_.../` lines to `mods/update/update.rpf` → `dlclist.xml`
   - raises listed pool ceilings in `gameconfig.xml` (unless you opted out)

3. Spawn the model name from the resource (trainer / console). If the game says
   the model does not exist, the pack was not mounted — check `ZeroIV.log`.

To disable a pack, move its folder out of `unpack-mods/` (for example to
`unpack-mods.off/`) and restart. Stale `ziv_*` dlclist entries are pruned on
the next start.

## `gameconfig.xml` — three modes

Priority, highest first:

### 1. Your own file (recommended if you already have one)

Place a complete `gameconfig.xml` here:

```
unpack-mods/gameconfig.xml
```

On start, ZeroIV copies it into `mods/update/update.rpf` and **does not** apply
its own pool raises. Edit that file freely; as long as it sits there, the ASI
will keep installing it when it differs from the archive.

A known-good community file for build 3889:
[pnwparksfan/gameconfig](https://github.com/pnwparksfan/gameconfig) →
`versions/3889/gameconfig.xml` (RDE-synced). Pair it with Heap **2048** and
Packfile Limit Adjuster Enhanced.

### 2. Turn auto-patching off

In `ZeroIV.ini`:

```ini
[fivem]
patchGameConfig = off
```

(`false` works the same.) The archive's `gameconfig.xml` is left untouched.
Use this if you edit the file inside `update.rpf` yourself (OpenIV / CodeWalker)
and do not want ZeroIV to rewrite it.

### 3. Automatic raises (default)

```ini
[fivem]
patchGameConfig = true
```

ZeroIV only **raises** a fixed list of model-info / streaming ceilings toward
RDE-style targets. Values that are already higher are kept. It does not replace
the whole XML and does not add every pool a full RDE file would add — for that,
use mode 1.

## `dlclist.xml`

ZeroIV always registers generated `ziv_*` packs from FiveM resources under
`unpack-mods/`.

With `[fivem] registerAddonPacks=true` (default), it also registers traditional
add-on folders that already contain `dlc.rpf`, under
`mods/update/x64/dlcpacks/<name>/`, `unpack-mods/dlcpacks/<name>/`, or
`unpack-mods/.cache/dlcpacks/<name>/`. Set `registerAddonPacks=false` to leave
those for hand-edited `dlclist.xml` entries.

Cleanup rules:

- `dlcpacks:/ziv_*` — pruned when the matching FiveM resource disappears
- Traditional packs ZeroIV auto-added — recorded in
  `unpack-mods/.cache/registered-addon-packs.txt`; only those names are removed
  from `dlclist.xml` when the folder is gone from **all** of: `mods/`,
  `unpack-mods/`, and the retail `update/x64/dlcpacks/`
- Retail game DLC under `update/x64/dlcpacks/` — never auto-added to the
  manifest and never removed from `dlclist.xml`
- Lines you added by hand — never touched

You normally do not edit `dlclist.xml` for ZeroIV packs.

## `ZeroIV.ini` (FiveM section)

Created next to `ZeroIV.asi` on first launch. Relevant keys:

```ini
[fivem]
enabled = true
; true = raise listed pools; off/false = leave gameconfig alone
; unpack-mods/gameconfig.xml always wins and skips ASI raises
patchGameConfig = true
plainifyRpf = true
patchStockUpdateRpf = false
; true = also auto-register traditional dlcpacks folders into dlclist
; FiveM resources are always scanned regardless
registerAddonPacks = true

[paths]
mods = mods
unpackMods = unpack-mods
```

| Key | Meaning |
| --- | --- |
| `enabled` | Master switch for the FiveM scan / pack pipeline |
| `patchGameConfig` | See [modes](#gameconfigxml--three-modes) above |
| `plainifyRpf` | Keep `mods/update/update.rpf` as a plaintext OPEN archive after the first conversion |
| `patchStockUpdateRpf` | Leave `false`. Only set `true` if you intentionally want the loader to edit the Steam `update/update.rpf` instead of the `mods/` copy |
| `registerAddonPacks` | Auto-add traditional `dlcpacks/<name>/dlc.rpf` folders to `dlclist.xml`. FiveM resources are always scanned |

## Check that it worked

Open `ZeroIV.log` after launch:

```
[fivem][init] N resource(s) under ...\unpack-mods
[fivem][init] nisa31 -> .cache/dlcpacks/ziv_nisa31... (cached|built)
[fivem][init] N pack(s) armed for dlclist
[fivem][gameconfig] using ...\unpack-mods\gameconfig.xml (ASI raises skipped)
```

or, without a user file:

```
[fivem][gameconfig] ...: raise (K): MaxVehicleModelInfos ...
```

If mounts fail, also check `PackfileLimitAdjusterEnhanced.log` and
`HeapAdjuster.log` / `HeapAdjuster.ini` (`HEAP_SIZE`).

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Model does not exist | Pack folder not under `unpack-mods/`, or game not restarted after adding it |
| Map missing / world empty | Large `gameconfig` without Heap 2048 + Packfile Limit Adjuster Enhanced |
| Only some packs work | Others still under `unpack-mods.off/` or another folder |
| Crash while opening `stream.rpf` | Pool ceilings too low for the number of packs — use a full community `gameconfig` or keep `patchGameConfig=true` |
| Your `gameconfig` edits vanish | Remove or rename `unpack-mods/gameconfig.xml`, or set `patchGameConfig=off` if you edit inside the RPF yourself |

## Not covered here

Engineering notes and measured pool tables live in
[fivem-findings.md](fivem-findings.md). End users do not need that file to play.
