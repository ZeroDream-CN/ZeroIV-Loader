# ZeroIV-Loader

ZeroIV-Loader is the open source RPF loader for GTA V, built as `ZeroIV.asi`.
It is an alternative to `OpenIV.asi` for GTA V Legacy and `OpenRPF.asi` for
GTA V Enhanced, and it feeds its log into ZeroIV's debug console in real time.

- Originally developed by martonp96 as ClosedIV
- Updated to support GTA V Legacy and Enhanced with the same binary by Chiheb-Bacha
- Renamed and extended with the ZeroIV debug-console bridge

## Benefits over OpenIV.asi

- open source
- supports all features of OpenIV.asi
- Uses a hashmap as a cache for rpf path lookups, which makes it faster
- supports multiple different versions of ASI Loaders (Recommended: https://github.com/ThirteenAG/Ultimate-ASI-Loader)
- adds a new way to load mods (see Info section)

## Files

| File | Purpose |
| --- | --- |
| `ZeroIV.asi` | The loader itself (build output). |
| `ZeroIV.ini` | Runtime configuration. Created next to `ZeroIV.asi` on first launch. An existing `RageOpenV.ini` is migrated automatically. |
| `ZeroIV.log` | Session log, rewritten at every launch. |

## Mods layouts

Two layouts are supported and they can be combined:

- `mods/` — OpenIV-style per-file redirection. A file that exists under `mods/`
  replaces the stock asset with the same path (handled by `FileReadHooks`).
- `newmods/` — directory override devices mounted over `platform:/`,
  `common:/`, `dlcpacks:` and `mods:/`. Only overrides whose directory exists are
  mounted; a missing `newmods/` leaves every stock device untouched. Set
  `[config] modsOverride = false` to skip this entirely. An override rooted at a
  directory that does not exist makes that mount point resolve to nothing, which
  GTA V Enhanced answers with an endless retry loop instead of booting.

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

- MinHook comes from the `src/vendor/minhook` submodule. When it is not
  initialised the script fetches the pinned revision itself, so a fresh clone
  builds without `git submodule update`.
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
- Put `ZeroIV.asi` in your GTA V folder.
- Delete `OpenIV.asi` or `OpenRPF.asi` if you have it.
- Place your mods in your mods folder, or keep the ones you already had.

## Info

Check the [wiki](https://github.com/martonp96/ClosedIV/wiki) for more information.

## Contact

Have any questions, suggestions, or would like to keep up to date with the
latest projects? Join the [Discord](https://discord.gg/VMvvuFxArY)!

## LICENSE

Please **DO NOT REDISTRIBUTE** built binaries, and instead link to the
[official release](https://www.gta5-mods.com/scripts/rageopenv).
