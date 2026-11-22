#include "main.h"
#include "FileReadHooks.h"
#include "CustomDevice.h"
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <shared_mutex>

using OpenBulkFn = HANDLE(*)(void*, const char*, __int64*);
using GetFileTimeFn = FILETIME(*)(void*, const char*);
using GetFileSizeFn = uint64_t(*)(void*, const char*);
using GetAttrsFn = uint64_t(*)(void*, const char*);

inline OpenBulkFn g_origOpenBulk1 = nullptr, g_origOpenBulk2 = nullptr;
inline GetFileTimeFn g_origGetFileTime1 = nullptr, g_origGetFileTime2 = nullptr;
inline GetFileSizeFn g_origGetFileSize1 = nullptr, g_origGetFileSize2 = nullptr;
inline GetAttrsFn g_origGetAttribs1 = nullptr, g_origGetAttribs2 = nullptr;

static const uint8_t EXISTS = 1;
static const uint8_t NOT_EXISTS = 2;
static std::unordered_map<std::string, uint8_t> g_modsCache;
static std::shared_mutex g_modsCacheMutex;

// True for a path the engine already gave in absolute form. Those belong to a
// stock tree, never to a mod layer, and joining them would silently discard the
// configured root.
static bool is_absolute(const char* path)
{
    if (!path || !*path)
        return false;
    if (path[0] == '\\' || path[0] == '/')
        return true;
    return std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' &&
        (path[2] == '\\' || path[2] == '/');
}

static bool TryModsPath(const char* path, std::string& outModsPath)
{
    if (is_absolute(path))
        return false;

    outModsPath = config::device_path(config::mods_root(), path);

    // Used a hashmap for lookups so that we don't call GetFileAttributeA every time
    {
        std::shared_lock lock(g_modsCacheMutex);
        auto it = g_modsCache.find(outModsPath);
        if (it != g_modsCache.end())
            return it->second == EXISTS;
    }

    bool exists = GetFileAttributesA(outModsPath.c_str()) != INVALID_FILE_ATTRIBUTES;
    {
        std::unique_lock lock(g_modsCacheMutex);
        g_modsCache[outModsPath] = exists ? EXISTS : NOT_EXISTS;
    }
    return exists;
}

// ---------------------------------------------------------------------------
// unpack-mods/ layer
//
// `mods/` mirrors the game directory, so prefixing the name is enough. The
// unpack layer deliberately does not: it only carries the subtrees it
// overrides, and the engine reaches most of them through a relative device path
// rather than a `common:/` style name. The dlc packs live at
// `update/x64/dlcpacks/<pack>`, so a pack placed at
// `unpack-mods/dlcpacks/<pack>/dlc.rpf` has to be found when the engine probes
// `update/x64/dlcpacks/<pack>/dlc.rpf`.
//
// Without this the pack never gets probed at all: dlclist walking asks whether
// each entry exists and silently skips the ones that do not, so OpenArchiveHook
// is never called for it.
static std::unordered_map<std::string, std::string> g_overlayCache; // name -> overlay path, empty = none
static std::shared_mutex g_overlayCacheMutex;

static bool TryOverlayPath(const char* path, std::string& outPath)
{
    if (!path || !*path)
        return false;

    {
        std::shared_lock lock(g_overlayCacheMutex);
        auto it = g_overlayCache.find(path);
        if (it != g_overlayCache.end())
        {
            if (it->second.empty())
                return false;
            outPath = it->second;
            return true;
        }
    }

    std::string result;

    // Compare case-insensitively with both separators folded to '/'; the engine
    // is not consistent about which one it uses.
    std::string lowered;
    lowered.reserve(path ? std::strlen(path) : 0);
    for (const char* p = path; p && *p; ++p)
        lowered.push_back(static_cast<char>(::tolower(static_cast<unsigned char>(*p == '\\' ? '/' : *p))));

    // The unpack layer holds each overridden subtree at its root, so the
    // `update/x64/` prefix the engine probes has to be dropped before joining.
    //
    // Do NOT auto-map the whole `common/` or `platform/` trees from
    // unpack-mods here: a partial common dump redirected wholesale over stock
    // paths soft-locks story mode. Visual overrides go through [overlay]
    // whitelist / GetDevice prefer instead.
    static const std::string kUpdateX64 = "update/x64/";
    if (lowered.compare(0, kUpdateX64.size(), kUpdateX64) == 0)
    {
        const std::string rest = path + kUpdateX64.size();

        // Two layers are checked: the hidden .cache one holds what the loader
        // generates, the plain one holds packs placed by hand. Generated wins
        // because it is rebuilt from source, so it cannot be the stale copy.
        const std::filesystem::path layers[] = {
            config::generated_root(),
            config::unpack_root(),
        };
        for (const auto& layer : layers)
        {
            const std::string candidate = config::device_path(layer, rest);
            if (GetFileAttributesA(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                result = candidate;
                break;
            }
        }
    }

    {
        std::unique_lock lock(g_overlayCacheMutex);
        g_overlayCache[path] = result;
    }

    if (result.empty())
        return false;

    outPath = result;
    return true;
}

// Resolves a redirect in priority order: `mods/` first (it mirrors the game
// directory, so it is the exact-path override), then the unpack layer.
static bool ResolveRedirect(const char* path, std::string& out, const char*& source)
{
    if (!path)
        return false;
    if (TryModsPath(path, out))
    {
        source = "mods";
        return true;
    }
    if (TryOverlayPath(path, out))
    {
        source = "unpack";
        return true;
    }
    return false;
}

bool fileread::ResolvePhysical(const char* path, std::string& out)
{
    const char* source = nullptr;
    return ResolveRedirect(path, out, source);
}

// Which spelling of a mods/ name this game's file device actually opens.
//
// The relative form is the one the stock device has always been fed and the one
// GetFileAttributesA accepts, but a device resolves against its own root, not
// the process working directory. When the first form fails every candidate is
// tried once and the winner is remembered per file, so the hot path only pays
// for this while the answer is unknown.
static std::mutex g_modsFormMutex;
static std::unordered_map<std::string, std::string> g_modsForm; // relative -> spelling that opened

static std::string mods_open_name(const std::string& path)
{
    std::lock_guard<std::mutex> lock(g_modsFormMutex);
    auto it = g_modsForm.find(path);
    if (it != g_modsForm.end() && !it->second.empty())
        return it->second;
    return path;
}

static void remember_form(const std::string& path, const char* form)
{
    std::lock_guard<std::mutex> lock(g_modsFormMutex);
    g_modsForm[path] = form ? form : "";
}

static HANDLE OpenBulkImpl(void* device, const char* path, __int64* a3, OpenBulkFn orig)
{
    logger::log("info", "[%s] Called with path /%s", __FUNCTION__, path);

    // The engine opens everything through here, so this is the best record of
    // what it was loading when it decided to stop.
    if (path)
        forensics::note_file(path);

    // unpack-mods overlay must be opened on the overlay fiDeviceLocal. Feeding
    // a filesystem path back into the stock/packfile device makes common/
    // platform overrides look "installed but not loading" (and can soft-lock
    // when partially redirected through the wrong device).
    {
        rage::fiDevice* overlay = nullptr;
        std::string overlayPath;
        if (unpackdev::ResolveOpen(path, overlay, overlayPath) && overlay)
        {
            logger::write("mods", "[%s] Overlay device %s", __FUNCTION__, overlayPath.c_str());
            return overlay->OpenBulk(overlayPath.c_str(), reinterpret_cast<uint64_t*>(a3));
        }
    }

    std::string modsPath;
    const char* source = nullptr;
    if (ResolveRedirect(path, modsPath, source))
    {
        // write(), not log(): redirects only happen for files that exist under
        // mods/ or unpack-mods/, so they are rare, and they are exactly the lines
        // needed to tell whether a mod archive is being served.
        logger::write("mods", "[%s] Redirecting to /%s (%s)", __FUNCTION__, modsPath.c_str(), source);

        const std::string known = mods_open_name(modsPath);
        HANDLE result = orig(device, known.c_str(), a3);
        if (result && result != INVALID_HANDLE_VALUE)
            return result;

        // The file is visibly there (GetFileAttributesA in TryModsPath proved it)
        // and this device still refused to open it, so the name has to be spelled
        // the way this particular device resolves it. A name is resolved against
        // the device's own root, which is not necessarily the process working
        // directory, so the relative form is only a guess. If it turns out to be
        // wrong, the mod layer silently serves the stock file instead - which is
        // invisible except for a mod pack that is "installed but not loading".
        const DWORD firstError = ::GetLastError();

        if (known == modsPath)
        {
            std::error_code ec;
            const std::string absolute = std::filesystem::absolute(modsPath, ec).string();
            const std::string dotted = "./" + modsPath;
            std::string native = modsPath;
            for (auto& c : native)
                if (c == '/')
                    c = '\\';

            const char* candidates[] = { absolute.c_str(), dotted.c_str(), native.c_str() };
            const char* labels[] = { "absolute", "dot-relative", "backslash" };
            for (int i = 0; i < 3; ++i)
            {
                if (!candidates[i] || !*candidates[i])
                    continue;
                HANDLE alt = orig(device, candidates[i], a3);
                const DWORD altError = ::GetLastError();
                const bool opened = alt && alt != INVALID_HANDLE_VALUE;
                logger::write("mods", "[%s] %s failed (%lu); %s form %s (%lu): %s",
                    __FUNCTION__, modsPath.c_str(), firstError, labels[i],
                    opened ? "works" : "fails", altError, candidates[i]);
                if (opened)
                {
                    remember_form(modsPath, candidates[i]);
                    return alt;
                }
            }
        }

        // The file exists under mods/ but the game could not open it: say so,
        // because silently falling back to the stock path is what makes a mod
        // pack look "installed but not loading".
        logger::write("mods", "[%s] Mod file could not be opened: /%s (win32 %lu)",
            __FUNCTION__, known.c_str(), firstError);
    }
    return orig(device, path, a3);
}

static FILETIME GetFileTimeImpl(void* device, const char* path, GetFileTimeFn orig)
{
    logger::log("info", "[%s] Called with path /%s", __FUNCTION__, path);

    {
        rage::fiDevice* overlay = nullptr;
        std::string overlayPath;
        if (unpackdev::ResolveOpen(path, overlay, overlayPath) && overlay)
        {
            FILETIME ft{};
            const uint64_t t = overlay->GetFileTime(overlayPath.c_str());
            *reinterpret_cast<uint64_t*>(&ft) = t;
            return ft;
        }
    }

    std::string modsPath;
    const char* source = nullptr;
    if (ResolveRedirect(path, modsPath, source))
    {
        logger::write("mods", "[%s] Redirecting to /%s (%s)", __FUNCTION__, modsPath.c_str(), source);
        return orig(device, mods_open_name(modsPath).c_str());
    }
    return orig(device, path);
}

static uint64_t GetFileSizeImpl(void* device, const char* path, GetFileSizeFn orig)
{
    logger::log("info", "[%s] Called with path /%s", __FUNCTION__, path);

    {
        rage::fiDevice* overlay = nullptr;
        std::string overlayPath;
        if (unpackdev::ResolveOpen(path, overlay, overlayPath) && overlay)
            return overlay->GetFileSize(overlayPath.c_str());
    }

    std::string modsPath;
    const char* source = nullptr;
    if (ResolveRedirect(path, modsPath, source))
    {
        logger::write("mods", "[%s] Redirecting to /%s (%s)", __FUNCTION__, modsPath.c_str(), source);
        return orig(device, mods_open_name(modsPath).c_str());
    }
    return orig(device, path);
}

static uint64_t GetAttribsImpl(void* device, const char* path, GetAttrsFn orig)
{
    logger::log("info", "[%s] Called with path /%s", __FUNCTION__, path);

    {
        rage::fiDevice* overlay = nullptr;
        std::string overlayPath;
        if (unpackdev::ResolveOpen(path, overlay, overlayPath) && overlay)
            return overlay->GetAttributes(overlayPath.c_str());
    }

    std::string modsPath;
    const char* source = nullptr;
    if (ResolveRedirect(path, modsPath, source))
    {
        logger::write("mods", "[%s] Redirecting to /%s (%s)", __FUNCTION__, modsPath.c_str(), source);
        return orig(device, mods_open_name(modsPath).c_str());
    }
    return orig(device, path);
}

// Vtable 1 (non-DS)
HANDLE OpenBulkHook1(void* d, const char* p, __int64* a3) { 
    return OpenBulkImpl(d, p, a3, g_origOpenBulk1); 
}

FILETIME GetFileTimeHook1(void* d, const char* p) { 
    return GetFileTimeImpl(d, p, g_origGetFileTime1); 
}

uint64_t GetFileSizeHook1(void* d, const char* p) { 
    return GetFileSizeImpl(d, p, g_origGetFileSize1); 
}

uint64_t GetAttribsHook1(void* d, const char* p) { 
    return GetAttribsImpl(d, p, g_origGetAttribs1); 
}

// Vtable 2 (DS)
HANDLE OpenBulkHook2(void* d, const char* p, __int64* a3) { 
    return OpenBulkImpl(d, p, a3, g_origOpenBulk2); 
}

FILETIME GetFileTimeHook2(void* d, const char* p) { 
    return GetFileTimeImpl(d, p, g_origGetFileTime2); 
}

uint64_t GetFileSizeHook2(void* d, const char* p) { 
    return GetFileSizeImpl(d, p, g_origGetFileSize2); 
}

uint64_t GetAttribsHook2(void* d, const char* p) { 
    return GetAttribsImpl(d, p, g_origGetAttribs2); 
}

static memory::InitFuncs FileReadHooks([] {
    if (IsEnhanced()) {
        memory::scan("41 56 56 57 53 48 81 ec ? ? ? ? 4c 89 c7 66 c7 ? ? ? ? ? 4c 8d 74 24")
            .hook(OpenBulkHook1, &g_origOpenBulk1);

        memory::scan("66 c7 01 ? ? 29 d9 d1 e9 66 89 4c 24 ? 31 ff 4c 8d 44 ? ? 48 89 d9 31 d2 ff 15 ? ? ? ? 85 c0").add(-0x10d)
            .hook(GetFileTimeHook1, &g_origGetFileTime1);

        memory::scan("66 c7 01 ? ? 29 d9 d1 e9 66 89 4c 24 ? 31 ff 4c 8d 44 24 ? 48 89 d9 31 d2 ff 15 ? ? ? ? 8b 4c 24").add(-0x10d)
            .hook(GetFileSizeHook1, &g_origGetFileSize1);

        memory::scan("41 56 56 57 53 48 81 ec ? ? ? ? 66 c7 ? ? ? ? ? 4c 8d 74 24")
            .hook(GetAttribsHook1, &g_origGetAttribs1);

        memory::scan("41 57 41 56 56 57 53 48 81 ec 60 02 00 00 48 89 d3 48 89 cf")
            .hook(OpenBulkHook2, &g_origOpenBulk2);

        memory::scan("66 c7 01 00 00 29 d9 d1 e9 66 89 4c 24 70 4c 8d 44 24 48 48 89 d9 31 d2 ff 15 ? ? ? ? 85 c0 74 14 8b 44 24 64 8b 4c 24 68 48 c1 e0 20 48 09 c1 48 89 4c 24 38").add(-0x15d)
            .hook(GetFileTimeHook2, &g_origGetFileTime2);

        memory::scan("66 c7 01 00 00 29 d9 d1 e9 66 89 4c 24 70 4c 8d 44 24 48 48 89 d9 31 d2 ff 15 ? ? ? ? 85 c0 74 0a 48 8b 44 24 5c 48 89 44 24 38").add(-0x15d)
            .hook(GetFileSizeHook2, &g_origGetFileSize2);

        memory::scan("56 57 53 48 81 ec 60 02 00 00 48 89 d7 48 89 ce c7 44 24 3c ff ff ff ff 48 89 d1 ba 3a 00 00 00")
            .hook(GetAttribsHook2, &g_origGetAttribs2);
    }
    else {
        memory::scan("40 53 48 81 EC ? ? ? ? 49 8B D8 4C 8B C2 48 8D 4C 24 ? BA ? ? ? ? E8 ? ? ? ? 48 83 64 24")
            .hook(OpenBulkHook1, &g_origOpenBulk1);

        memory::scan("48 81 EC ? ? ? ? 4C 8B C2 48 8D 4C 24 ? BA ? ? ? ? E8 ? ? ? ? 4C 8D 44 24 ? 33 D2 48 8B C8 FF 15 ? ? ? ? 85 C0 75 04 33 C0 EB 0F 8B 44 24 38 8B 4C 24 34 48 C1 E0 20 48 0B C1 48 81 C4")
            .hook(GetFileTimeHook1, &g_origGetFileTime1);

        memory::scan("48 81 EC ? ? ? ? 4C 8B C2 48 8D 4C 24 ? BA ? ? ? ? E8 ? ? ? ? 4C 8D 44 24 ? 33 D2 48 8B C8 FF 15 ? ? ? ? 85 C0 75 04 33 C0 EB 0F 8B 44 24 3C 8B 4C 24 40 48 C1 E0 20 48 0B C1 48 81 C4")
            .hook(GetFileSizeHook1, &g_origGetFileSize1);

        memory::scan("48 89 5C 24 ? 57 48 81 EC ? ? ? ? 4C 8B C2 48 8D 4C 24 ? BA ? ? ? ? E8 ? ? ? ? 48 8B C8 FF 15 ? ? ? ? 83 CF FF 8B D8 3B C7 74 0F 48 8D 4C 24")
            .hook(GetAttribsHook1, &g_origGetAttribs1);
    }
    });