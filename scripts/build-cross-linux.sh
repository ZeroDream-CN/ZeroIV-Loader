#!/usr/bin/env bash
#
# Cross-compiles ZeroIV.asi on Linux with mingw-w64. No Windows machine and no
# CMake/MSVC needed: the MinGW-w64 toolchain packages are enough.
#
#   Fedora:  sudo dnf install mingw64-gcc-c++ mingw64-binutils
#   Debian:  sudo apt install g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64
#   Arch:    sudo pacman -S mingw-w64-gcc
#
# Usage: scripts/build-cross-linux.sh [options]
#
#   -d, --debug       Debug build (-O0 -g, no strip)
#   -l, --logs        Enable the verbose hot-path logging (_LOG, like the
#                     ReleaseWithLogs configuration in CMakeSettings.json)
#   -c, --clean       Remove BUILD/cross before building
#       --opt LEVEL   Optimisation level: -O0..-O3 (default -O2)
#       --no-strip    Keep symbols in the .asi
#   -j, --jobs N      Parallel compile jobs (default: nproc)
#       --proxy URL   HTTP/SOCKS proxy for fetching MinHook, e.g.
#                     --proxy http://127.0.0.1:7897 ("none" disables it)
#       --minhook DIR Use local MinHook sources instead of downloading
#   -h, --help        Show this help
#
# Output: BUILD/BIN/Release/ZeroIV.asi (debug: BUILD/BIN/Debug/) + a default
#         ZeroIV.ini
#
# Environment overrides:
#   CROSS_PREFIX    Toolchain prefix     (default x86_64-w64-mingw32)
#   CC / CXX / AR   Compiler binaries    (default ${CROSS_PREFIX}-gcc/-g++/-ar)
#   BUILD_DIR       Build directory      (default BUILD/cross)
#   ZEROIV_PROXY    Proxy URL (also reads https_proxy / HTTPS_PROXY / all_proxy)
#   MINHOOK_DIR     Local MinHook checkout
#   MINHOOK_COMMIT  MinHook revision fetched when the vendored copy is missing
#   FETCH_TIMEOUT   Seconds allowed per download attempt (default 120)

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/BUILD/cross}"
MINHOOK_BUILD="$BUILD_DIR/minhook"
DIST_DIR="$ROOT/BUILD/BIN/Release"
CROSS_PREFIX="${CROSS_PREFIX:-x86_64-w64-mingw32}"
CROSS_PREFIX="${CROSS_PREFIX:-x86_64-w64-mingw32}"
CC="${CC:-${CROSS_PREFIX}-gcc}"
CXX="${CXX:-${CROSS_PREFIX}-g++}"
AR="${AR:-${CROSS_PREFIX}-ar}"

MINHOOK_COMMIT="${MINHOOK_COMMIT:-4a455528f61b5a375b1f9d44e7d296d47f18bb18}"
MINHOOK_URL="https://codeload.github.com/TsudaKageyu/minhook/tar.gz/${MINHOOK_COMMIT}"
FETCH_TIMEOUT="${FETCH_TIMEOUT:-120}"

# Ports common desktop proxies listen on, probed only when nothing is
# configured but a download is actually needed.
PROXY_CANDIDATES=(http://127.0.0.1:7897 http://127.0.0.1:7890 http://127.0.0.1:10809
    http://127.0.0.1:1080 http://127.0.0.1:8080)

OPT_PROXY="${ZEROIV_PROXY:-${https_proxy:-${HTTPS_PROXY:-${all_proxy:-${ALL_PROXY:-}}}}}"
MINHOOK_LOCAL="${MINHOOK_DIR:-}"

OPT_LEVEL="${OPT_LEVEL:--O2}"
MODE="release"
ENABLE_LOGS=0
CLEAN=0
STRIP=1
JOBS="$(nproc 2>/dev/null || echo 4)"

info() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "${BASH_SOURCE[0]}"
    exit 0
}

while (($#)); do
    case "$1" in
        -d|--debug)     MODE="debug"; OPT_LEVEL="-O0"; STRIP=0 ;;
        -l|--logs)      ENABLE_LOGS=1 ;;
        -c|--clean)     CLEAN=1 ;;
        --opt)          [[ $# -ge 2 ]] || die "$1 needs a value"; OPT_LEVEL="$2"; shift ;;
        --no-strip)     STRIP=0 ;;
        -j|--jobs)      [[ $# -ge 2 ]] || die "$1 needs a value"; JOBS="$2"; shift ;;
        --proxy)        [[ $# -ge 2 ]] || die "$1 needs a value"; OPT_PROXY="$2"; shift ;;
        --minhook)      [[ $# -ge 2 ]] || die "$1 needs a value"; MINHOOK_LOCAL="$2"; shift ;;
        -h|--help)      usage ;;
        *)              die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

if ! [[ "$JOBS" =~ ^[0-9]+$ ]] || ((JOBS < 1)); then
    die "invalid job count: $JOBS"
fi

case "$OPT_LEVEL" in
    -O0|-O1|-O2|-O3) ;;
    -Os|-Oz)
        # Some mingw-w64 libstdc++ builds omit the out-of-line std::string move
        # constructor at this level and fail at link time; warn instead.
        warn "$OPT_LEVEL may fail to link with mingw-w64's libstdc++ (std::string move constructor); trying anyway"
        ;;
    *)
        die "unsupported optimisation level: $OPT_LEVEL (use -O0, -O1, -O2 or -O3)"
        ;;
esac

# Debug builds land beside the release one instead of overwriting it, matching
# the BIN/Debug and BIN/Release layout the CMake build produces. Objects are
# also kept per mode so switching back and forth does not rebuild everything.
if [[ "$MODE" == "debug" ]]; then
    DIST_DIR="$ROOT/BUILD/BIN/Debug"
fi
OBJ_DIR="$BUILD_DIR/obj/$MODE"

# ---------------------------------------------------------------------------
# Toolchain check

for tool in "$CC" "$CXX" "$AR"; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        die "$tool not found.
Install a mingw-w64 toolchain, e.g.:
  Fedora: sudo dnf install mingw64-gcc-c++ mingw64-binutils
  Debian: sudo apt install g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64
  Arch:   sudo pacman -S mingw-w64-gcc
Or point CROSS_PREFIX at your prefix (e.g. CROSS_PREFIX=i686-w64-mingw32)."
    fi
done

info "toolchain: $("$CXX" --version | head -n1)"

if ((CLEAN)); then
    info "cleaning $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi
mkdir -p "$OBJ_DIR" "$MINHOOK_BUILD" "$DIST_DIR"

# ---------------------------------------------------------------------------
# Proxy handling
#
# Only the optional MinHook fetch touches the network, so the proxy is resolved
# lazily: nothing is probed when the sources are already on disk.

proxy_url() {
    # Prints the configured proxy, or nothing when networking should go direct.
    [[ "$OPT_PROXY" == "none" || "$OPT_PROXY" == "off" ]] && return 0
    printf '%s' "$OPT_PROXY"
}

probe_proxy() {
    local url="$1"
    curl -sS --max-time 8 --retry 1 --retry-delay 1 -x "$url" -o /dev/null https://github.com >/dev/null 2>&1
}

autodetect_proxy() {
    local candidate
    for candidate in "${PROXY_CANDIDATES[@]}"; do
        if probe_proxy "$candidate"; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}

resolve_proxy() {
    # An explicitly configured proxy is used verbatim: if it is wrong, curl/git
    # report the real error instead of a probe warning.
    local configured
    configured="$(proxy_url)"
    if [[ -n "$configured" ]]; then
        printf '%s' "$configured"
        return 0
    fi
    # Nothing configured: probe the usual local ports once the network is
    # actually needed, so a restricted network works without extra flags.
    if detected="$(autodetect_proxy)"; then
        warn "no proxy configured, using detected $detected"
        OPT_PROXY="$detected"
        printf '%s' "$detected"
        return 0
    fi
    printf ''
}

# Cached proxy for the current run: probing once keeps curl and git from each
# scanning the candidate ports.
RESOLVED_PROXY=""
PROXY_RESOLVED=0
current_proxy() {
    if ((PROXY_RESOLVED == 0)); then
        RESOLVED_PROXY="$(resolve_proxy)"
        PROXY_RESOLVED=1
    fi
    printf '%s' "$RESOLVED_PROXY"
}

# curl wrapper: honours the resolved proxy and never hangs forever.
fetch() {
    local url="$1" dest="$2" proxy
    proxy="$(current_proxy)"
    local args=(--fail --silent --show-error --location --retry 2 --retry-delay 1
        --connect-timeout 15 --max-time "$FETCH_TIMEOUT")
    if [[ -n "$proxy" ]]; then
        args+=(--proxy "$proxy")
    fi
    curl "${args[@]}" -o "$dest" "$url"
}

# git wrapper: git has no built-in timeout, so bound every remote operation.
git_fetch() {
    local proxy
    proxy="$(current_proxy)"
    local args=(-C "$ROOT")
    if [[ -n "$proxy" ]]; then
        args+=(-c "http.proxy=$proxy" -c "https.proxy=$proxy")
    fi
    GIT_TERMINAL_PROMPT=0 timeout "$FETCH_TIMEOUT" git "${args[@]}" "$@"
}

# ---------------------------------------------------------------------------
# MinHook: vendored copy > explicit checkout > pinned download
#
# The sources are committed under src/vendor/minhook so a release source
# archive builds without network access or submodules.

find_minhook() {
    local dir
    for dir in "$MINHOOK_LOCAL" "$ROOT/src/vendor/minhook"; do
        if [[ -n "$dir" && -f "$dir/include/MinHook.h" ]]; then
            printf '%s' "$dir"
            return 0
        fi
    done
    return 1
}

MINHOOK_DIR=""
if MINHOOK_DIR="$(find_minhook)"; then
    info "MinHook: $MINHOOK_DIR"
else
    if [[ -n "$MINHOOK_LOCAL" ]]; then
        die "--minhook $MINHOOK_LOCAL does not contain include/MinHook.h"
    fi
    if [[ -z "$MINHOOK_DIR" ]]; then
        MINHOOK_DIR="$BUILD_DIR/minhook-src"
        if [[ ! -f "$MINHOOK_DIR/include/MinHook.h" ]]; then
            command -v curl >/dev/null 2>&1 || die "MinHook sources missing and curl is unavailable.
Pass a checkout: $(basename "${BASH_SOURCE[0]}") --minhook /path/to/minhook"

            info "MinHook: downloading $MINHOOK_COMMIT"
            STAGE="$BUILD_DIR/minhook-stage"
            rm -rf "$STAGE" "$MINHOOK_DIR"
            mkdir -p "$STAGE"
            if ! fetch "$MINHOOK_URL" "$BUILD_DIR/minhook.tar.gz"; then
                rm -rf "$STAGE" "$BUILD_DIR/minhook.tar.gz"
                die "could not download MinHook.
Behind a proxy? pass --proxy http://127.0.0.1:7897 (or set ZEROIV_PROXY).
Already have the sources? pass --minhook /path/to/minhook"
            fi
            tar -xzf "$BUILD_DIR/minhook.tar.gz" -C "$STAGE" || die "could not extract MinHook"
            EXTRACTED="$(find "$STAGE" -mindepth 1 -maxdepth 1 -type d | head -n1)"
            [[ -n "$EXTRACTED" ]] || die "unexpected MinHook archive layout"
            mv "$EXTRACTED" "$MINHOOK_DIR"
            rm -rf "$STAGE" "$BUILD_DIR/minhook.tar.gz"
        fi
        info "MinHook: pinned source tree"
    fi
fi

[[ -f "$MINHOOK_DIR/include/MinHook.h" ]] || die "MinHook headers not found in $MINHOOK_DIR"

# ---------------------------------------------------------------------------
# Flags

# VERSION is the single source of truth for the release number. The commit is
# recorded too, so a binary can be traced back to the exact tree it was built
# from even when the version has not been bumped yet.
VERSION="$(tr -d '\r\n[:space:]' < "$ROOT/VERSION" 2>/dev/null)"
[[ -n "$VERSION" ]] || die "cannot read $ROOT/VERSION"
COMMIT="$(git -C "$ROOT" rev-parse --short=8 HEAD 2>/dev/null || true)"

DEFINES=(-DZEROIV_LOADER_VERSION="\"$VERSION\"" -DZEROIV_LOADER_COMMIT="\"$COMMIT\"")
((ENABLE_LOGS)) && DEFINES+=(-D_LOG)
# Keep asserts in debug builds; release matches the MSVC Release configuration.
[[ "$MODE" == "release" ]] && DEFINES+=(-DNDEBUG)

COMMON_FLAGS=(
    "$OPT_LEVEL"
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-deprecated-copy
    -Wno-attributes
    -Wno-cast-function-type
    -ffunction-sections -fdata-sections
    -MMD -MP
)

CXXFLAGS=(
    "${COMMON_FLAGS[@]}"
    "${DEFINES[@]}"
    -std=c++20
    -I"$ROOT/scripts/compat"
    -I"$ROOT/src"
    -I"$MINHOOK_DIR/include"
)
[[ "$MODE" == "debug" ]] && CXXFLAGS+=(-g)

MINHOOK_CFLAGS=(
    "$OPT_LEVEL"
    -DNDEBUG
    -ffunction-sections -fdata-sections
    -MMD -MP
    -I"$MINHOOK_DIR/include"
    -I"$MINHOOK_DIR/src"
    -I"$MINHOOK_DIR/src/hde"
)

# The ASI is injected into GTA V, so the GCC runtimes are linked statically:
# the game folder must not need libstdc++-6.dll or libwinpthread-1.dll.
LDFLAGS=(
    -shared
    -static -static-libgcc -static-libstdc++
    -Wl,--gc-sections
)
((STRIP)) && LDFLAGS+=(-s)

# Object files record no flag information, so a change (e.g. --logs) would
# silently reuse incompatible objects; drop them instead.
FLAGS_STAMP="$BUILD_DIR/flags-$MODE.stamp"
CURRENT_STAMP="$(printf '%s\n' "${CXXFLAGS[@]}" | tr -d '\n')"
if [[ -f "$FLAGS_STAMP" && "$(cat "$FLAGS_STAMP")" != "$CURRENT_STAMP" ]]; then
    info "compiler flags changed, rebuilding objects"
    rm -rf "$OBJ_DIR"
    mkdir -p "$OBJ_DIR"
fi
printf '%s' "$CURRENT_STAMP" > "$FLAGS_STAMP"

# ---------------------------------------------------------------------------
# Parallel compile helpers

FAILED=0
SKIPPED=0
PIDS=()
LOGS=()
SRCS=()

# needs_build OBJ SRC — true when the object is missing or older than any
# prerequisite recorded by -MMD (source, project header, system header).
needs_build() {
    local obj="$1" src="$2" dep prereq
    [[ -f "$obj" ]] || return 0
    [[ "$src" -nt "$obj" ]] && return 0
    # -MMD names the dependency file after the output with ".d" as suffix.
    dep="${obj%.o}.d"
    [[ -f "$dep" ]] || return 0
    while IFS= read -r prereq; do
        [[ -n "$prereq" && "$prereq" -nt "$obj" ]] && return 0
    done < <(sed -e 's/\\$//' -e '1s/^[^:]*://' "$dep" | tr ' ' '\n' | sed '/^$/d')
    return 1
}

flush_jobs() {
    local i
    for i in "${!PIDS[@]}"; do
        if ! wait "${PIDS[$i]}"; then
            printf '\033[1;31mfailed:\033[0m %s\n' "${SRCS[$i]}" >&2
            sed -n '1,60p' "${LOGS[$i]}" >&2 || true
            FAILED=1
        fi
    done
    PIDS=()
    LOGS=()
    SRCS=()
}

queue_job() {
    local log="$1" src="$2"
    shift 2
    "$@" >"$log" 2>&1 &
    PIDS+=("$!")
    LOGS+=("$log")
    SRCS+=("$src")
    if ((${#PIDS[@]} >= JOBS)); then
        flush_jobs
        if ((FAILED)); then
            die "compilation failed"
        fi
    fi
}

# ---------------------------------------------------------------------------
# MinHook static library

info "building MinHook"
MINHOOK_OBJECTS=()
for name in buffer hook trampoline hde/hde64; do
    src="$MINHOOK_DIR/src/$name.c"
    obj="$MINHOOK_BUILD/$(basename "$name").o"
    if needs_build "$obj" "$src"; then
        queue_job "$obj.log" "$src" "$CC" "${MINHOOK_CFLAGS[@]}" -c "$src" -o "$obj"
    fi
    MINHOOK_OBJECTS+=("$obj")
done
flush_jobs
if ((FAILED)); then
    die "MinHook compilation failed"
fi
rm -f "$MINHOOK_BUILD/libminhook.a"
"$AR" rcs "$MINHOOK_BUILD/libminhook.a" "${MINHOOK_OBJECTS[@]}"

# ---------------------------------------------------------------------------
# Loader objects

info "compiling loader sources"
mapfile -t SOURCES < <(find "$ROOT/src" -name '*.cpp' -not -path '*/vendor/*' | sort)
((${#SOURCES[@]} > 0)) || die "no C++ sources found under $ROOT/src"

OBJECTS=()
for src in "${SOURCES[@]}"; do
    rel="${src#"$ROOT/src/"}"
    obj="$OBJ_DIR/${rel%.cpp}.o"
    mkdir -p "$(dirname "$obj")"
    if needs_build "$obj" "$src"; then
        queue_job "$obj.log" "$src" "$CXX" "${CXXFLAGS[@]}" -c "$src" -o "$obj"
    else
        SKIPPED=$((SKIPPED + 1))
    fi
    OBJECTS+=("$obj")
done
flush_jobs
if ((FAILED)); then
    die "compilation failed"
fi
if ((SKIPPED > 0)); then
    info "reusing $SKIPPED up-to-date object(s)"
fi

# ---------------------------------------------------------------------------
# Link

info "linking ZeroIV.asi"
OUTPUT="$DIST_DIR/ZeroIV.asi"
"$CXX" "${LDFLAGS[@]}" -o "$OUTPUT" \
    "${OBJECTS[@]}" \
    -L"$MINHOOK_BUILD" -lminhook \
    -lversion -lws2_32

# A default configuration ships next to the binary. The loader also creates one
# on first launch and never overwrites an existing file.
INI="$DIST_DIR/ZeroIV.ini"
if [[ ! -f "$INI" ]]; then
    cat > "$INI" <<'EOF'
; ZeroIV-Loader configuration.
; Missing keys are re-added on launch; delete the file to restore defaults.

[config]
; Opens a debug console window next to the game.
console = false
; Enables the unpack-mods/ layer. Set to false to use only mods/ redirection.
modsOverride = true
; Accepts RPF7 archives written without encryption (as ZeroIV builds them).
allowUnencryptedRpf = true
; Writes every image the engine mounts to an .img file for offline inspection.
dumpImage = false

[paths]
; Where the two mod layers live. Empty or relative values resolve against the
; game directory; absolute paths are used as given.
;   mods/       per-file redirection, mirrors the game directory
;   unpack-mods/ loose packs, carries only the subtrees it overrides
mods = mods
unpackMods = unpack-mods

[log]
; Per-file-operation traces from the device/file hooks. They fire hundreds of
; times per second while GTA streams assets, so each category is opt-in; the
; loader additionally throttles every category and collapses repeated lines.
; info = every fiDevice / file-read call
; mods = mods/ path redirections and RPF overrides
; rpf  = RPF encryption traces
; device = fiDevice virtual method calls (very chatty)
info = false
mods = false
rpf = false
device = false

[debugConsole]
; Streams the log into ZeroIV's debug console over loopback.
; The port must match the listener in ZeroIV (logbridge.go).
enabled = true
host = 127.0.0.1
port = 27183
EOF
fi

# The licences have to travel with the binary: MinHook and inipp both require
# their notice to accompany binary distributions, and the GPL needs its text.
cp -f "$ROOT/LICENSE.txt" "$DIST_DIR/LICENSE.txt"
cp -f "$ROOT/THIRD-PARTY-NOTICES.txt" "$DIST_DIR/THIRD-PARTY-NOTICES.txt"

if command -v sha256sum >/dev/null 2>&1; then
    (cd "$DIST_DIR" && sha256sum -b ZeroIV.asi LICENSE.txt THIRD-PARTY-NOTICES.txt \
        > SHA256SUMS.txt) || rm -f "$DIST_DIR/SHA256SUMS.txt"
fi

info "built $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"

if command -v "${CROSS_PREFIX}-objdump" >/dev/null 2>&1; then
    IMPORTS="$("${CROSS_PREFIX}-objdump" -p "$OUTPUT" 2>/dev/null | awk '/DLL Name/ {print $3}' | tr '\n' ' ')"
    info "imports: $IMPORTS"
fi

cat <<'EOF'

Next steps:
  - Copy ZeroIV.asi (and ZeroIV.ini, optional) into the GTA V folder.
  - Start ZeroIV and open its Debug Console to watch the
    loader's log live; the same lines go to ZeroIV.log next to the ASI.
EOF
