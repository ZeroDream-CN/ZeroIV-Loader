# ZeroIV-Loader

[![Build](https://github.com/ZeroDream-CN/ZeroIV-Loader/actions/workflows/build.yaml/badge.svg)](https://github.com/ZeroDream-CN/ZeroIV-Loader/actions/workflows/build.yaml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE.txt)
[![Latest release](https://img.shields.io/github/v/release/ZeroDream-CN/ZeroIV-Loader)](https://github.com/ZeroDream-CN/ZeroIV-Loader/releases/latest)

ZeroIV-Loader is the open source RPF loader for GTA V, built as `ZeroIV.asi`.
It is an alternative to `OpenIV.asi` for GTA V Legacy and `OpenRPF.asi` for
GTA V Enhanced, and it feeds its log into ZeroIV's debug console in real time.

- Originally developed by martonp96 as ClosedIV
- Updated to support GTA V Legacy and Enhanced with the same binary by Chiheb-Bacha
- Renamed and extended with the ZeroIV debug-console bridge

## Download

Grab `ZeroIV.asi` from the [latest release](https://github.com/ZeroDream-CN/ZeroIV-Loader/releases/latest)
and follow [How to install](#how-to-install). Every release also ships the
complete corresponding source archive and a `SHA256SUMS.txt`, so the binary can
be verified against a build you make yourself.

Requires an ASI loader (recommended:
[Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)) and a
legitimately owned copy of GTA V. Single-player only.

## Benefits over OpenIV.asi

- open source
- supports all features of OpenIV.asi
- Uses a hashmap as a cache for rpf path lookups, which makes it faster
- supports multiple different versions of ASI Loaders (Recommended: https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- adds a new way to load mods (see [Mods layouts](#mods-layouts))

## Files

| File | Purpose |
| --- | --- |
| `ZeroIV.asi` | The loader itself (build output). |
| `ZeroIV.ini` | Runtime configuration. Created next to `ZeroIV.asi` on first launch. An existing `RageOpenV.ini` is migrated automatically. |
| `ZeroIV.log` | Session log, rewritten at every launch. |
| `LICENSE.txt` | GPL-3.0-or-later. Must stay next to the binary. |
| `THIRD-PARTY-NOTICES.txt` | Licences of the bundled libraries. Must stay next to the binary. |

## Mods layouts

Two layouts are supported and they can be combined:

- `mods/` — OpenIV-style per-file redirection. A file that exists under `mods/`
  replaces the stock asset with the same path (handled by `FileReadHooks`).
- `unpack-mods/` — the unpacked layer. It only carries the subtrees it
  overrides, so `unpack-mods/dlcpacks/<pack>/dlc.rpf` serves
  `update/x64/dlcpacks/<pack>/dlc.rpf`, and the same applies to `platform/` and
  `common/`. The directory is mounted at a private `unpack-mods:/` prefix and a
  path is redirected to it only when the file exists there, so the retail packs
  keep loading from their stock devices. A missing `unpack-mods/` changes
  nothing. Set `[config] modsOverride = false` to skip this layer entirely.

A `dlc.rpf` that is a directory rather than an archive is supported in both
layouts: GTA V Enhanced cannot mount a directory as a packfile, so the folder is
compiled into a real RPF7 under `ZeroIVLoose/` on first launch and remounted
from there. The compiled archive is rebuilt automatically whenever a file in the
folder changes.

### Storage locations

Both folders live next to the executable by default. Point them anywhere with
the `[paths]` section of `ZeroIV.ini`:

```ini
[paths]
; game directory by default
mods = mods
unpackMods = unpack-mods
; examples
; mods = D:/GTAV-Mods/mods
; unpackMods = ../shared/unpack-mods
```

- An absolute path is used as given. Both `D:\GTAV-Mods\mods` and
  `D:/GTAV-Mods/mods` work.
- A relative path resolves against the game directory. It cannot climb above
  the drive root, so under Proton `..\..\..\..\..\..\` stops at the drive the
  Steam library is on; use an absolute path to reach another disk.
- Running under Proton/Wine, a Unix path such as `/home/you/gta-mods/mods` is
  accepted too and is translated through the `Z:` drive, which Wine maps to the
  filesystem root.
- `;` and `#` both start a comment. If the file cannot be parsed, the loader
  keeps a copy as `ZeroIV.ini.broken`, logs the parser message and runs with
  defaults instead of silently ignoring the settings.

The resolved roots are printed at every launch:

```
attach: mods root   Z:\home\you\gta-mods\mods
attach: unpack root Z:\home\you\gta-mods\unpack-mods
```


## Debug console integration

ZeroIV-Loader streams its log to the ZeroIV app, where every
line appears in the **Debug Console** window under the `host` source. Lines are
also written to `ZeroIV.log`, so a session can be inspected offline.

The transport is a loopback TCP connection to `127.0.0.1:27183`; no network
traffic leaves the machine. Lines logged before ZeroIV is open are queued and
delivered as soon as it starts.

`ZeroIV.ini`:

```ini
[config]
console = false
modsOverride = true

[log]
; Per-file-operation traces. Each category is opt-in because these hooks fire
; hundreds of times per second; the loader also throttles and collapses them.
info = false
mods = false
rpf = false
device = false

[debugConsole]
enabled = true
host = 127.0.0.1
port = 27183
```

`[debugConsole]` controls the forwarder. The port must match the listener built
into ZeroIV (`logbridge.go`, default `27183`). The `[log]` switches enable the
per-file-operation traces (`info`, `mods`, `rpf`, `device`); important events —
startup, mounted devices, failed hooks, and every `mods/` redirect hit — are
always recorded, so a default session stays quiet.

`[log]` tracing is diagnostic only: enabling categories costs loading time
because every file operation is formatted and written. Leave them `false` for
normal play.

### Flood control

The device/file hooks fire hundreds of times per second while GTA streams
assets, so logging them naively stalls the game thread and floods the console.
Three independent gates keep a traced session usable:

- **Opt-in categories** — `[log] device = false` means those lines are not even
  formatted; only a cached boolean is read per call.
- **Throttling and repeat collapsing** in the loader — each category gets a
  per-second allowance (120 lines/s for `device`, `mods`, `rpf`; 500 otherwise).
  Lines over the allowance are dropped and reported once as
  `[rate-limit] dropped N line(s) from '<category>'`, and an identical message
  repeated within one second is collapsed into a single `[repeat] ... xN`
  line.
- **Burst clamping** in ZeroIV — the bridge batches loader lines for 120 ms
  before emitting one console event and caps a connection at 400 lines/s,
  reporting `suppressed N loader line(s): flood limit reached` when it clamps.

Console output (`[config] console = true`) is written with a single
`WriteFile` on the console handle, and the console's **QuickEdit** mode is turned
off at startup. QuickEdit suspends console output while text is selected, which
made a game thread block inside the logger as soon as the window had a
selection — the cause of the "window never appears / everything freezes" reports.
No helper thread is used: creating one inside `DllMain` (loader lock) deadlocks,
and creating one from a hook is no safer. Lines are only sent to a real console;
when stdout is a pipe (launched from a shell, or captured by Steam) they go to
`ZeroIV.log` only, because a synchronous write to a slow pipe can block too.
`ZeroIV.log` is flushed on every line outside the chatty categories, so a
session that dies still leaves a usable log.

## How to build

CMake 3.17.x and Visual Studio are needed to build on Windows. Run
`release.bat` to build it quickly or use `generate_vs2022.bat` to create
project files for a specific VS version. The binary is produced as
`BUILD/BIN/Release/ZeroIV.asi`.

No submodule checkout or network access is required: MinHook is vendored under
`src/vendor/minhook`. The version number comes from the `VERSION` file, which
is what release tags must match; building inside a git checkout additionally
records the commit.


### Building on Windows with PowerShell

`build.ps1` drives the same CMake build and needs only Visual Studio (with the
"Desktop development with C++" workload): CMake is taken from `PATH` or from
the copy bundled with Visual Studio, and the newest installed VS generator is
picked automatically.

```powershell
.\build.ps1                     # release -> BUILD\BIN\Release\ZeroIV.asi
.\build.ps1 -Logs               # ReleaseWithLogs (_LOG)
.\build.ps1 -DebugBuild         # BUILD\BIN\Debug\ZeroIV.asi
.\build.ps1 -Clean -Jobs 8
.\build.ps1 -Generator ninja    # imports the MSVC environment from vcvars64.bat
```

Options: `-Configuration` (`Release`, `RelWithDebInfo`, `Debug`), `-Logs`,
`-Clean`, `-Jobs N`, `-Generator` (`auto`, `ninja`, `vs2026`, `vs2022`,
`vs2019`), `-Proxy URL` (for the MinHook download; `none` forces direct
access), `-MinHook DIR` (use a local checkout), `-NoIni`. A default
`ZeroIV.ini` is written next to the binary and never overwrites an existing
one.

MinHook is vendored under `src/vendor/minhook`, so a fresh clone builds without
any network access. The pinned upstream revision is still downloaded as a
fallback if that directory is missing. Behind a proxy, pass
`-Proxy http://127.0.0.1:7897` or set `ZEROIV_PROXY`; without a flag the usual
local proxy ports are probed once the network is actually needed. If the execution policy blocks the script, run
`powershell -ExecutionPolicy Bypass -File .\build.ps1`.

### Cross-compiling on Linux

`scripts/build-cross-linux.sh` builds the same `ZeroIV.asi` with mingw-w64 — no
Windows machine, no CMake and no MSVC needed.

```bash
sudo dnf install mingw64-gcc-c++ mingw64-binutils      # Fedora
# sudo apt install g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64   # Debian/Ubuntu
# sudo pacman -S mingw-w64-gcc                                     # Arch

./scripts/build-cross-linux.sh                     # release
./scripts/build-cross-linux.sh --logs              # ReleaseWithLogs (_LOG)
./scripts/build-cross-linux.sh -d -j4              # debug, 4 jobs
```

Options: `-d/--debug`, `-l/--logs`, `--opt LEVEL` (default `-O2`),
`--no-strip`, `-c/--clean`, `-j/--jobs N`, `--proxy URL`, `--minhook DIR`,
`-h/--help`.

Output matches the CMake layout: `BUILD/BIN/Release/ZeroIV.asi` for release
builds and `BUILD/BIN/Debug/ZeroIV.asi` for `--debug`, each with a default
`ZeroIV.ini` next to it (an existing one is never overwritten).

`-Os` / `-Oz` are accepted but warned about: some mingw-w64 libstdc++ builds
omit the out-of-line `std::string` move constructor at those levels and the
link fails. The stripped `-O2` binary is only ~1.3 MB, so there is little to
gain.

Notes:

- MinHook is vendored in `src/vendor/minhook` (upstream commit `4a45552`), so
  a fresh clone or a source archive builds without network access.
- MinGW links the GCC runtimes statically (`libstdc++`, `libgcc`,
  `libwinpthread`), so the ASI only needs `KERNEL32.dll`, `msvcrt.dll` and
  `WS2_32.dll` — nothing has to be shipped beside it.
- Behind a proxy (common on networks where `github.com` is slow or blocked),
  pass it explicitly or let the script detect a local one:

  ```bash
  ./scripts/build-cross-linux.sh --proxy http://127.0.0.1:7897
  ZEROIV_PROXY=socks5://127.0.0.1:7897 ./scripts/build-cross-linux.sh
  ```

  Without a flag the script honours `https_proxy`/`all_proxy`, and otherwise
  probes the usual local proxy ports (7897, 7890, 10809, 1080, 8080) the first
  time it actually needs the network. `--proxy none` forces direct access.
- Objects are tracked with `-MMD` dependency files, so a rebuild only
  recompiles what changed; changing flags (`--logs`) discards the objects
  automatically.
- `scripts/compat/Windows.h` exists because the sources include `<Windows.h>`
  while mingw-w64 only ships `windows.h`.

## How to install

- Install an ASI loader.
- Put `ZeroIV.asi` in your GTA V folder, together with `LICENSE.txt` and
  `THIRD-PARTY-NOTICES.txt` (required when you redistribute it).
- Delete `OpenIV.asi` or `OpenRPF.asi` if you have it.
- Put your mods in `mods/` and `unpack-mods/`, or point `[paths]` in
  `ZeroIV.ini` at wherever they already live.
- A `dlc.rpf` may be a real archive or an unpacked folder; folders are compiled
  into an archive under `ZeroIVLoose/` on first launch.

## Add-on vehicles (FiveM resources)

Drop each FiveM vehicle resource into `unpack-mods/<name>/` (with
`fxmanifest.lua` or `__resource.lua`). On launch, ZeroIV builds a `ziv_*` DLC
pack, registers it in `dlclist.xml`, and can raise `gameconfig.xml` pool
ceilings so story mode can load the cars.

On **Enhanced**, convert Gen8 stream assets to Gen9 before placing them under
`unpack-mods/`. ZeroIV warns at startup when RSC versions do not match the
running game.

Traditional `dlcpacks/<name>/dlc.rpf` folders can also be auto-registered
(`[fivem] registerAddonPacks=true`).

For many packs you also need:

- [Heap Limit Adjuster](https://www.gta5-mods.com/tools/heapadjuster) — try **2048** MiB
- [Packfile Limit Adjuster Enhanced](https://www.gta5-mods.com/scripts/packfile-limit-adjuster-enhanced) — install the **`.asi`**, not only the `.ini`

Optional: put your own `unpack-mods/gameconfig.xml` to use that file instead of
the ASI's automatic raises, or set `[fivem] patchGameConfig = off` to leave the
archive alone.

**Player guide:** [docs/fivem-mods.md](docs/fivem-mods.md)

## Info

- Upstream projects: [ClosedIV](https://github.com/martonp96/ClosedIV)
  (original, no longer reachable) and
  [RageOpenV](https://github.com/Chiheb-Bacha/RageOpenV) (GPL-3.0 fork this
  project descends from).

## Contact

Have any questions, suggestions, or would like to keep up to date with the
latest projects? Join the [Discord](https://discord.gg/VMvvuFxArY)!

## LICENSE

ZeroIV-Loader is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE.txt](LICENSE.txt) for the full text.

Because the GPL covers the program as a whole, there are no restrictions beyond
it: you may redistribute the binary, bundle it with an installer, or ship it in
a launcher (the ZeroIV app does the latter) as long as the corresponding source
travels with it. Redistribution is expressly permitted — that clause that used
to forbid it no longer applies.

- **Source**: the repository is the corresponding source for every release.
  Each tagged release also ships `ZeroIV-Loader-<version>-src.tar.gz`, which
  builds offline: MinHook is vendored under `src/vendor/minhook`, so no
  submodule or download step is needed.
- **Version traceability**: `VERSION` is the single source of truth for the
  release number, and the build records the commit. The startup log line
  (`ZeroIV-Loader 1.2.0+abcdef12 inited`) ties a binary back to its tree.
- **Binaries**: when distributing ZeroIV.asi, ship `LICENSE.txt` and
  `THIRD-PARTY-NOTICES.txt` alongside it. MinHook (BSD-2-Clause) and inipp
  (MIT) both require their notice to accompany binary distributions.

### Third-party components

| Component | Version | Licence |
| --- | --- | --- |
| [MinHook](https://github.com/TsudaKageyu/minhook) | `4a45552` | BSD-2-Clause |
| [inipp](https://github.com/mcmtroffaes/inipp) | vendored header | MIT |

Full texts are in [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

### Not affiliated with Rockstar Games

ZeroIV-Loader is an unofficial tool. It contains no game code, assets or keys,
does not bypass any anti-cheat, and is intended for single-player use with a
legitimately owned copy of GTA V. Use at your own risk.
