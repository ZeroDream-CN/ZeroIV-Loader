#include "main.h"

#ifndef ZEROIV_LOADER_VERSION
#define ZEROIV_LOADER_VERSION "dev"
#endif

// Empty when the build came from a source archive, which is exactly the case
// where the tag already identifies the tree.
#ifndef ZEROIV_LOADER_COMMIT
#define ZEROIV_LOADER_COMMIT ""
#endif

#include "utils/forensics.h"
#include "hooks/BindDescriptorHooks.h"

static bool bInited = false;
void (*origGetSystemTimeAsFileTime)(LPFILETIME lpSystemTimeAsFileTime);
void HookGetSystemTimeAsFileTime(LPFILETIME lpSystemTimeAsFileTime)
{
	if (!bInited)
	{
		bInited = true;

		// Don't hide the console
		if (IsEnhanced()) {
			auto addr = memory::scan("ff 15 ? ? ? ? 48 85 c0 74 ? 48 89 c1 31 d2", true);
			if (addr.address != 0) {
				addr.add(16).nop(6);
			}
		}
		else {
			// This is kept for backwards compatibility, as I don't know when this pattern stopped working.
			auto addr = memory::scan("FF 15 ? ? ? ? E8 ? ? ? ? 65 48 8B 0C 25 ? ? ? ? 8B 05 ? ? ? ? 48 8B 04 C1 BA ? ? ? ? 83 24 02 00 E8", true);
			if (addr.address == 0) {
				addr = memory::scan("ff 15 ? ? ? ? 65 48 8b 0c 25 ? ? ? ? 8b 05 ? ? ? ? ba");
				if (addr.address != 0) {
					addr.nop(6);
				}
			}
		}

		// rpf cache check skip
		if (IsEnhanced()) {
			auto addr = memory::scan("e8 ? ? ? ? 48 8d 0d ? ? ? ? 48 8d 15 ? ? ? ? 48 8d 35");
			if (addr.address != 0) {
				addr.add(1).rip().put<uint16_t>(0x90c3);
			}
		}		

		logger::write("info", "hooks: installing");

		bindhooks::install();

		memory::InitFuncs::run();

		// Version plus commit: the line that ties a crashing binary back to the
		// exact sources it was built from.
		const char* commit = ZEROIV_LOADER_COMMIT;
		if (commit[0] != '\0')
			logger::write("info", " ZeroIV-Loader %s+%s inited (%s)", ZEROIV_LOADER_VERSION,
				commit, IsEnhanced() ? "GTA V Enhanced" : "GTA V Legacy");
		else
			logger::write("info", " ZeroIV-Loader %s inited (%s)", ZEROIV_LOADER_VERSION,
				IsEnhanced() ? "GTA V Enhanced" : "GTA V Legacy");
	}
	origGetSystemTimeAsFileTime(lpSystemTimeAsFileTime);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD  dwReason, LPVOID lpReserved)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(hModule);

		// Lines logged before the game first calls a hooked function are
		// queued; the sender thread is created once we are out of DllMain.
		logbridge::set_inside_dll_main(true);

		config::load();

		logger::init();

		// Startup breadcrumbs: if the game dies during bootstrap, the last line
		// here says exactly how far the loader got.
		logger::write("info", "attach: console=%d modsOverride=%d",
			(int)config::get_config<bool>("console"),
			(int)config::get_value<bool>("config", "modsOverride", true));

		// Resolved once here so a mistyped [paths] entry shows up as one clear
		// line instead of a pack that silently never loads.
		if (!config::error().empty())
			logger::write("info", "attach: ZeroIV.ini could not be read (%s); "
				"defaults were used and the original was kept as ZeroIV.ini.broken",
				config::error().c_str());

		logger::write("info", "attach: mods root %s", config::mods_root().string().c_str());
		logger::write("info", "attach: unpack root %s", config::unpack_root().string().c_str());

		memory::init();

		logger::write("info", "attach: memory initialised");

		// Before any hook runs: the game can stop before the first frame, and
		// this handler is the only thing that records why.
		forensics::install();

		// Debug aid: the on-disk executable is packed, so the only way to
		// disassemble an assert address later is to save the live image now.
		if (config::get_value<bool>("config", "dumpImage", false))
		{
			std::error_code dumpEc;
			auto dir = std::filesystem::current_path(dumpEc);
			if (!dumpEc)
			{
				forensics::dump_image(dir.string().c_str());
			}
		}

		if (config::get_config<bool>("console"))
		{
			const BOOL allocated = AllocConsole();
			logger::write("info", "attach: AllocConsole=%d", (int)allocated);

			FILE* unused = nullptr;
			freopen_s(&unused, "CONIN$", "r", stdin);
			freopen_s(&unused, "CONOUT$", "w", stdout);
			freopen_s(&unused, "CONOUT$", "w", stderr);

			// The console handle only exists from here on.
			logger::console_attached();
		}

		// NOTE: using a thread on legacy doesn't work consistently. Sometimes some hooks are executed before init is finished.
		// Calling HookGetSystemTimeAsFileTime directly inside DllMain doesn't work (loader lock shenanigans)
		// memory::IAT doesn't work on Enhanced since imports are obfuscated
		if (!memory::HookApi(L"kernel32.dll", "GetSystemTimeAsFileTime", (PVOID)HookGetSystemTimeAsFileTime, (PVOID*)&origGetSystemTimeAsFileTime)) {
			logger::write("info", "Hooking failed error (%ld)", GetLastError());
		}

		logger::write("info", "attach: system time hook requested");

		logbridge::set_inside_dll_main(false);
	}
	else if (dwReason == DLL_PROCESS_DETACH)
	{
		// Detach can run under the loader lock; only flush buffers here.
		logger::flush();
	}
	return TRUE;
}