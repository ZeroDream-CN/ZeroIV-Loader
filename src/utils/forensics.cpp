#include "main.h"
#include "utils/forensics.h"
#include "hooks/BindDescriptorHooks.h"

#include <cstdio>
#include <cstring>

/*
	Why this exists
	---------------
	When the game decides it cannot continue (a failed asset load, a pool it
	could not satisfy, an assert) it prints the reason with OutputDebugString
	and then executes a breakpoint. With no debugger attached the process just
	disappears, so a log that stops mid-file says nothing about whether the
	loader served a bad file or the game rejected a good one.

	A vectored handler runs before SEH and before the crash reporter, so it is
	the only place where the faulting address, the stack and the pending debug
	output are all still reachable.
*/

namespace forensics
{
	namespace
	{
		constexpr size_t kMaxModules = 512;
		constexpr size_t kMaxExecSections = 8;
		constexpr size_t kMaxFrames = 40;
		constexpr size_t kMaxStackBytes = 2048;
		constexpr size_t kNameLen = 64;
		constexpr size_t kNoteCount = 8;
		constexpr size_t kNoteLen = 160;

		struct ExecSection
		{
			uintptr_t start;
			uintptr_t end;
		};

		struct ModuleEntry
		{
			uintptr_t base;
			uintptr_t imageEnd;
			char name[kNameLen];
			ExecSection exec[kMaxExecSections];
			size_t execCount;
		};

		ModuleEntry g_modules[kMaxModules];
		size_t g_moduleCount = 0;
		bool g_modulesResolved = false;

		// One thread reports at a time; a fault inside the handler must not
		// recurse.
		volatile LONG g_reporting = 0;

		// Fatal sites are reported once each: the game's error path can raise
		// the same breakpoint several times on its way out.
		struct Site { uint32_t code; uintptr_t addr; };
		Site g_seen[8];
		size_t g_seenCount = 0;

		// Ring of recently touched files, newest last.
		char g_notes[kNoteCount][kNoteLen];
		volatile LONG g_noteIndex = 0;

		// The game prints the reason it is stopping with OutputDebugString,
		// which does nothing at all when no debugger is attached. Hooking it is
		// the only way to read that text: without a debugger the call never
		// raises 0x40010006, so a vectored handler never sees it.
		using OutputDebugStringAFn = void(WINAPI*)(LPCSTR);
		using OutputDebugStringWFn = void(WINAPI*)(LPCWSTR);
		OutputDebugStringAFn g_origOdsA = nullptr;
		OutputDebugStringWFn g_origOdsW = nullptr;
		volatile LONG g_odsLines = 0;
		constexpr LONG kMaxOdsLines = 400;

		void log_game_text(const char* text, bool wideSource, const wchar_t* wtext)
		{
			if (!text && !wtext)
				return;
			if (InterlockedIncrement(&g_odsLines) > kMaxOdsLines)
				return;

			char buffer[1024] = { 0 };
			if (wideSource)
				WideCharToMultiByte(CP_UTF8, 0, wtext, -1, buffer, (int)sizeof(buffer) - 1, nullptr, nullptr);
			else
				snprintf(buffer, sizeof(buffer), "%s", text);

			for (char* p = buffer; *p; ++p)
			{
				if (*p == '\r' || *p == '\n' || *p == '\t')
					*p = ' ';
			}
			if (buffer[0] == 0)
				return;
			logger::write("game", "%s", buffer);
			logger::flush();
		}

		void WINAPI hookOutputDebugStringA(LPCSTR text)
		{
			log_game_text(text, false, nullptr);
			if (g_origOdsA)
				g_origOdsA(text);
		}

		void WINAPI hookOutputDebugStringW(LPCWSTR text)
		{
			log_game_text(nullptr, true, text);
			if (g_origOdsW)
				g_origOdsW(text);
		}

		// Reads a NUL-terminated string at addr and logs it if it looks like
		// text. Used on the registers and stack at fault time, where the
		// error handler still holds its message.
		void try_log_string(const char* tag, uintptr_t addr, int& budget)
		{
			if (addr == 0 || budget <= 0)
				return;

			char raw[256] = { 0 };
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, raw, sizeof(raw) - 1, &got) || got < 6)
				return;

			char out[256] = { 0 };
			size_t used = 0;

			// UTF-16 text shows up as printable, NUL, printable, NUL.
			if (raw[1] == 0 && raw[0] >= 0x20 && raw[0] < 0x7F)
			{
				wchar_t wbuf[128] = { 0 };
				memcpy(wbuf, raw, sizeof(wbuf) - sizeof(wchar_t));
				wbuf[(sizeof(wbuf) / sizeof(wchar_t)) - 1] = 0;
				WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)sizeof(out) - 1, nullptr, nullptr);
				used = strlen(out);
			}
			else
			{
				for (size_t i = 0; i < sizeof(raw) && raw[i]; ++i)
				{
					const unsigned char c = (unsigned char)raw[i];
					if (c < 0x20 || c >= 0x7F)
						break;
					out[used++] = (char)c;
					if (used + 1 >= sizeof(out))
						break;
				}
				out[used] = 0;
			}

			if (used < 6)
				return;
			--budget;
			logger::write("fatal", "  %s: %s", tag, out);
		}

		using EnumProcessModulesFn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
		using GetModuleFileNameExWFn = DWORD(WINAPI*)(HANDLE, HMODULE, LPWSTR, DWORD);

		void basename_into(const wchar_t* src, char* dst, size_t dstSize)
		{
			if (dstSize < 2)
				return;
			// The conversion gets its own oversized buffer: converting straight
			// into dst failed whenever the full path was longer than dstSize,
			// which left module names truncated or completely empty.
			char wide[MAX_PATH * 2] = { 0 };
			if (WideCharToMultiByte(CP_ACP, 0, src, -1, wide, (int)sizeof(wide) - 1,
					nullptr, nullptr) <= 0)
			{
				dst[0] = 0;
				return;
			}

			const char* from = wide;
			for (const char* p = wide; *p; ++p)
			{
				if (*p == '\\' || *p == '/')
					from = p + 1;
			}
			const size_t len = strlen(from);
			const size_t copy = len < dstSize - 1 ? len : dstSize - 1;
			memcpy(dst, from, copy);
			dst[copy] = 0;
		}

		// Reads the PE headers of a mapped image and records its executable
		// sections. Only those can hold a return address, so restricting the
		// lookup to them keeps data pointers out of the stack trace.
		void collect_sections(ModuleEntry& mod)
		{
			auto* base = reinterpret_cast<const uint8_t*>(mod.base);
			const uint32_t lfanew = *reinterpret_cast<const uint32_t*>(base + 0x3C);
			if (lfanew == 0 || lfanew > 0x1000)
			{
				mod.imageEnd = mod.base + 0x100000;
				return;
			}
			const uint8_t* nt = base + lfanew;
			if (nt[0] != 'P' || nt[1] != 'E')
			{
				mod.imageEnd = mod.base + 0x100000;
				return;
			}

			const uint16_t sectionCount = *reinterpret_cast<const uint16_t*>(nt + 6);
			const uint16_t optSize = *reinterpret_cast<const uint16_t*>(nt + 20);
			const uint8_t* opt = nt + 24;
			if (optSize >= 60)
			{
				const uint32_t sizeOfImage = *reinterpret_cast<const uint32_t*>(opt + 56);
				mod.imageEnd = mod.base + (sizeOfImage ? sizeOfImage : 0x100000);
			}
			else
			{
				mod.imageEnd = mod.base + 0x100000;
			}

			const uint8_t* sec = opt + optSize;
			for (uint16_t i = 0; i < sectionCount && mod.execCount < kMaxExecSections; ++i, sec += 40)
			{
				const uint32_t vSize = *reinterpret_cast<const uint32_t*>(sec + 8);
				const uint32_t vAddr = *reinterpret_cast<const uint32_t*>(sec + 12);
				const uint32_t chars = *reinterpret_cast<const uint32_t*>(sec + 36);
				if ((chars & 0x20000000) == 0) // IMAGE_SCN_MEM_EXECUTE
					continue;
				if (vSize == 0)
					continue;
				mod.exec[mod.execCount].start = mod.base + vAddr;
				mod.exec[mod.execCount].end = mod.base + vAddr + vSize;
				++mod.execCount;
			}
		}

		// force=true re-enumerates. Modules loaded after the first call (D3D12,
		// the shader compiler, overlay DLLs) were previously missing, which is
		// why a fault inside one of them resolved to "unknown module".
		void resolve_modules(bool force = false)
		{
			if (g_modulesResolved && !force)
				return;
			g_modulesResolved = true;
			g_moduleCount = 0;

			HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
			auto enumModules = kernel ? (EnumProcessModulesFn)GetProcAddress(kernel, "K32EnumProcessModules") : nullptr;
			auto moduleFileName = kernel ? (GetModuleFileNameExWFn)GetProcAddress(kernel, "K32GetModuleFileNameExW") : nullptr;
			if (!enumModules || !moduleFileName)
			{
				if (HMODULE psapi = LoadLibraryW(L"psapi.dll"))
				{
					if (!enumModules)
						enumModules = (EnumProcessModulesFn)GetProcAddress(psapi, "EnumProcessModules");
					if (!moduleFileName)
						moduleFileName = (GetModuleFileNameExWFn)GetProcAddress(psapi, "GetModuleFileNameExW");
				}
			}
			if (!enumModules || !moduleFileName)
				return;

			HMODULE handles[kMaxModules];
			DWORD needed = 0;
			if (!enumModules(GetCurrentProcess(), handles, (DWORD)sizeof(handles), &needed))
				return;

			const size_t count = needed / sizeof(HMODULE);
			for (size_t i = 0; i < count && i < kMaxModules; ++i)
			{
				wchar_t full[MAX_PATH * 2] = { 0 };
				if (!moduleFileName(GetCurrentProcess(), handles[i], full, (DWORD)(sizeof(full) / sizeof(wchar_t))))
					continue;
				ModuleEntry& mod = g_modules[g_moduleCount];
				mod.base = (uintptr_t)handles[i];
				mod.execCount = 0;
				basename_into(full, mod.name, sizeof(mod.name));
				collect_sections(mod);
				++g_moduleCount;
			}

			std::sort(g_modules, g_modules + g_moduleCount,
				[](const ModuleEntry& a, const ModuleEntry& b) { return a.base < b.base; });
		}

		// True only when the address lies inside an executable section, so a
		// stray data pointer is never mistaken for a return address.
		bool describe(uintptr_t addr, char* out, size_t outSize)
		{
			if (addr == 0 || g_moduleCount == 0)
				return false;
			size_t lo = 0, hi = g_moduleCount;
			while (lo < hi)
			{
				const size_t mid = (lo + hi) / 2;
				if (addr < g_modules[mid].base)
					hi = mid;
				else if (addr >= g_modules[mid].imageEnd)
					lo = mid + 1;
				else
				{
					const ModuleEntry& m = g_modules[mid];
					for (size_t s = 0; s < m.execCount; ++s)
					{
						if (addr >= m.exec[s].start && addr < m.exec[s].end)
						{
							snprintf(out, outSize, "%s+0x%llX", m.name,
								(unsigned long long)(addr - m.base));
							return true;
						}
					}
					return false;
				}
			}
			return false;
		}

		bool read_word(uintptr_t addr, uintptr_t& value)
		{
			SIZE_T got = 0;
			return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, &value, sizeof(value), &got)
				&& got == sizeof(value);
		}

		// A module image range check that also accepts non-executable pages, so
		// a pc landing in data or a JIT page still names its owner.
		bool nearest_module(uintptr_t addr, char* out, size_t outSize)
		{
			if (g_moduleCount == 0)
				return false;
			const ModuleEntry* best = nullptr;
			for (size_t i = 0; i < g_moduleCount; ++i)
			{
				const ModuleEntry& m = g_modules[i];
				if (addr >= m.base && addr < m.imageEnd)
					best = &m;
			}
			if (!best)
				return false;
			snprintf(out, outSize, "%s+0x%llX", best->name, (unsigned long long)(addr - best->base));
			return true;
		}

		// Writes every loaded module and its range. The crash pc could not be
		// attributed to a module last time because the module table was stale,
		// so the table itself is now part of the evidence.
		void dump_modules()
		{
			logger::write("fatal", "  modules (%llu):", (unsigned long long)g_moduleCount);
			for (size_t i = 0; i < g_moduleCount; ++i)
			{
				const ModuleEntry& m = g_modules[i];
				logger::write("fatal", "    %s base=0x%llX end=0x%llX",
					m.name, (unsigned long long)m.base, (unsigned long long)m.imageEnd);
			}
		}

		// dbghelp is resolved once, at install time. Loading it from inside the
		// handler would take the loader lock while the process is already in a
		// fault, which can deadlock instead of writing a dump.
		using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, uint32_t, void*, void*, void*);
		MiniDumpWriteDumpFn g_miniDumpWrite = nullptr;
		bool g_dumped = false;

		// MINIDUMP_EXCEPTION_INFORMATION, declared here so the dump path does
		// not need the dbghelp headers.
		struct MiniDumpExceptionInfo
		{
			DWORD ThreadId;
			EXCEPTION_POINTERS* ExceptionPointers;
			BOOL ClientPointers;
		};

		// Logs raw bytes as hex. The in-memory image is authoritative: the
		// executable on disk is packed, and a driver patch could differ too.
		void dump_bytes(const char* tag, uintptr_t addr, size_t len)
		{
			if (addr == 0 || len == 0 || len > 128)
				return;
			unsigned char raw[128] = { 0 };
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, raw, len, &got) || got == 0)
			{
				logger::write("fatal", "  %s @0x%llX unreadable", tag, (unsigned long long)addr);
				return;
			}
			char line[128 * 2 + 16] = { 0 };
			size_t used = 0;
			for (SIZE_T i = 0; i < got && used + 3 < sizeof(line); ++i)
				used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", raw[i]);
			logger::write("fatal", "  %s @0x%llX = %s", tag, (unsigned long long)addr, line);
		}

		// The game binds a shader's resources with one ID3D12Device::CopyDescriptors
		// call: it builds an array of D3D12_CPU_DESCRIPTOR_HANDLE (8 bytes per
		// entry) on the stack from a resource table, a matching array of sizes,
		// and hands both to the driver.
		//
		// The live values are no longer recovered from the faulting context:
		// by then the registers belong to the driver, not to the game call, and
		// guessing from r9/r13 produced misleading output. BindDescriptorHooks
		// records the real arguments into a lock-free ring as the game calls
		// CopyDescriptors, so the newest records are printed here instead.
		void dump_descriptor_arrays(const CONTEXT*)
		{
			bindhooks::dump_recent(8);
		}

		// Writes one minidump for the first access violation of the session.
		// Every thread's stack is included, plus the info needed to resolve the
		// addresses offline against the dumped image; full memory is left out
		// because this process maps many gigabytes.
		void write_minidump(EXCEPTION_POINTERS* info, const char* path)
		{
			if (!g_miniDumpWrite || g_dumped)
				return;
			g_dumped = true;

			HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				FILE_ATTRIBUTE_NORMAL, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				logger::write("fatal", "  [dump] cannot create %s (err %lu)", path, GetLastError());
				return;
			}

			MiniDumpExceptionInfo dumpInfo{ GetCurrentThreadId(), info, FALSE };
			const uint32_t flags =
				0x00000000 | // MiniDumpNormal: module list + thread stacks
				0x00000040 | // MiniDumpWithIndirectlyReferencedMemory
				0x00000100 | // MiniDumpWithProcessThreadData
				0x00000800 | // MiniDumpWithFullMemoryInfo
				0x00001000;  // MiniDumpWithThreadInfo
			const BOOL ok = g_miniDumpWrite(GetCurrentProcess(), GetCurrentProcessId(),
				file, flags, &dumpInfo, nullptr, nullptr);
			CloseHandle(file);
			logger::write("fatal", "  [dump] %s (ok=%d, err %lu)", path, ok ? 1 : 0, ok ? 0UL : GetLastError());
		}

		// Prints what a register points at when it looks like an object with a
		// vtable. A call through a null method pointer lands at pc=0 and leaves
		// the object that should have supplied it in an argument register, so
		// the vtable slot is what identifies the failing class.
		void probe_object(const char* name, uintptr_t obj)
		{
			if (obj < 0x10000)
				return;
			uintptr_t vt = 0;
			if (!read_word(obj, vt) || vt == 0)
			{
				logger::write("fatal", "  %s=0x%llX vt=0", name, (unsigned long long)obj);
				return;
			}
			char where[160] = { 0 };
			if (!describe(vt, where, sizeof(where)))
			{
				logger::write("fatal", "  %s=0x%llX vt=0x%llX", name, (unsigned long long)obj,
					(unsigned long long)vt);
				return;
			}
			logger::write("fatal", "  %s=0x%llX vt=%s", name, (unsigned long long)obj, where);
			for (uintptr_t slot = 0; slot < 4 * sizeof(uintptr_t); slot += sizeof(uintptr_t))
			{
				uintptr_t fn = 0;
				if (!read_word(vt + slot, fn) || fn == 0)
				{
					logger::write("fatal", "    vt+0x%llX = 0", (unsigned long long)slot);
					continue;
				}
				char fnWhere[160] = { 0 };
				if (describe(fn, fnWhere, sizeof(fnWhere)))
					logger::write("fatal", "    vt+0x%llX = %s", (unsigned long long)slot, fnWhere);
				else
					logger::write("fatal", "    vt+0x%llX = 0x%llX", (unsigned long long)slot,
						(unsigned long long)fn);
			}
		}

		bool already_seen(uint32_t code, uintptr_t addr)
		{
			for (size_t i = 0; i < g_seenCount; ++i)
			{
				if (g_seen[i].code == code && g_seen[i].addr == addr)
					return true;
			}
			if (g_seenCount < sizeof(g_seen) / sizeof(g_seen[0]))
			{
				g_seen[g_seenCount].code = code;
				g_seen[g_seenCount].addr = addr;
				++g_seenCount;
			}
			return false;
		}

		const char* code_name(uint32_t code)
		{
			switch (code)
			{
			case 0xC0000005: return "access violation";
			case 0x80000003: return "breakpoint (game asked to stop)";
			case 0xC000001D: return "illegal instruction";
			case 0xC0000094: return "integer divide by zero";
			case 0xC00000FD: return "stack overflow";
			case 0xC0000409: return "stack buffer overrun / fastfail";
			case 0xE06D7363: return "C++ exception";
			}
			return "exception";
		}

		void dump_notes()
		{
			const LONG latest = g_noteIndex;
			if (latest <= 0)
				return;
			logger::write("fatal", "  last files touched:");
			for (size_t i = 0; i < kNoteCount; ++i)
			{
				const LONG idx = latest - 1 - (LONG)i;
				if (idx < 0)
					break;
				const char* note = g_notes[idx % kNoteCount];
				if (note[0] == 0)
					continue;
				logger::write("fatal", "    %s", note);
			}
		}

		void report(EXCEPTION_POINTERS* info)
		{
			const uint32_t code = info->ExceptionRecord->ExceptionCode;
			const uintptr_t addr = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
			// Re-enumerated every time: a module loaded after the first call
			// (D3D12, the shader compiler) would otherwise be missing and its
			// fault would read back as "unknown module".
			resolve_modules(true);

			char where[160] = { 0 };
			if (!describe(addr, where, sizeof(where)))
				nearest_module(addr, where, sizeof(where));

			if (already_seen(code, addr))
				return;
			if (InterlockedCompareExchange(&g_reporting, 1, 0) != 0)
				return;

			logger::write("fatal", "game stopped: %s 0x%08X at %s",
				code_name(code), code, where[0] ? where : "unknown module");

			if (info->ExceptionRecord->NumberParameters >= 2)
			{
				const ULONG_PTR op = info->ExceptionRecord->ExceptionInformation[0];
				const ULONG_PTR target = info->ExceptionRecord->ExceptionInformation[1];
				if (op <= 1)
					logger::write("fatal", "  %s of address 0x%llX",
						op == 1 ? "write" : "read", (unsigned long long)target);
			}

			dump_notes();

			// Diagnostic for this build only: the assert at +0x1E8373C fires
			// when a resource allocation returns a non-zero status, and the
			// allocator is reached through this global. Logging the live
			// pointer chain lets the failing implementation be read out of the
			// image dump instead of guessed at.
			const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
			if (base)
			{
				auto qword = [&](uintptr_t addr) -> uintptr_t {
					uintptr_t v = 0;
					ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, &v, sizeof(v), nullptr);
					return v;
				};
				const uintptr_t allocGlobal = (uintptr_t)base + 0x4E87AF0;
				const uintptr_t obj = qword(allocGlobal);
				if (obj > (uintptr_t)base && obj < (uintptr_t)base + 0x10000000)
				{
					const uintptr_t vt = qword(obj);
					logger::write("fatal", "  [alloc] global=0x%llX obj=0x%llX vtable=0x%llX",
						(unsigned long long)allocGlobal, (unsigned long long)obj, (unsigned long long)vt);
					if (vt > (uintptr_t)base && vt < (uintptr_t)base + 0x10000000)
					{
						for (uintptr_t slot : {(uintptr_t)0xC8, (uintptr_t)0xE8})
						{
							const uintptr_t fn = qword(vt + slot);
							logger::write("fatal", "  [alloc] vtable+0x%llX = 0x%llX (exe+0x%llX)",
								(unsigned long long)slot, (unsigned long long)fn,
								(unsigned long long)(fn - (uintptr_t)base));
						}
					}
				}
				else
				{
					logger::write("fatal", "  [alloc] global 0x%llX -> %llX (unset)",
						(unsigned long long)allocGlobal, (unsigned long long)obj);
				}
			}

			if (info->ContextRecord)
			{
				const uintptr_t sp = (uintptr_t)info->ContextRecord->Rsp;
				const uintptr_t pc = (uintptr_t)info->ContextRecord->Rip;

				// Numeric register state. The string probe below only fires when a
				// register happens to hold ASCII; for asserts that validate a
				// pointer the value itself is what identifies the offending
				// resource, so log the whole argument set unconditionally.
				logger::write("fatal",
					"  regs rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX",
					(unsigned long long)info->ContextRecord->Rax,
					(unsigned long long)info->ContextRecord->Rbx,
					(unsigned long long)info->ContextRecord->Rcx,
					(unsigned long long)info->ContextRecord->Rdx,
					(unsigned long long)info->ContextRecord->Rsi,
					(unsigned long long)info->ContextRecord->Rdi);
				logger::write("fatal",
					"  regs r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX",
					(unsigned long long)info->ContextRecord->R8,
					(unsigned long long)info->ContextRecord->R9,
					(unsigned long long)info->ContextRecord->R10,
					(unsigned long long)info->ContextRecord->R11,
					(unsigned long long)info->ContextRecord->R12,
					(unsigned long long)info->ContextRecord->R13,
					(unsigned long long)info->ContextRecord->R14,
					(unsigned long long)info->ContextRecord->R15);

				// Raw fault coordinates. describe() only prints these when the
				// address resolves, and a null indirect call is exactly the case
				// where nothing resolves: pc sits at 0 and there is no module.
				logger::write("fatal", "  pc=0x%llX sp=0x%llX thread=%lu",
					(unsigned long long)info->ContextRecord->Rip,
					(unsigned long long)info->ContextRecord->Rsp,
					(unsigned long)GetCurrentThreadId());

				// A read of address 0 at a pc of 0 is a call through a null
				// function pointer. The object that owns the missing method is
				// normally still in an argument register.
				if (info->ContextRecord->Rip == 0)
				{
					probe_object("rcx", (uintptr_t)info->ContextRecord->Rcx);
					probe_object("rdx", (uintptr_t)info->ContextRecord->Rdx);
					probe_object("r8 ", (uintptr_t)info->ContextRecord->R8);
					probe_object("r9 ", (uintptr_t)info->ContextRecord->R9);
				}

				// Surrounding instructions plus the structs the faulting call
				// was handed. A driver fault at a fixed offset is decided by
				// what the game passed in, not by the driver itself.
				{
					char owner[160] = { 0 };
					if (pc >= 0x20 && !describe(pc, owner, sizeof(owner)))
						nearest_module(pc, owner, sizeof(owner));
					if (owner[0])
						logger::write("fatal", "  pc owner: %s", owner);
					dump_bytes("code", pc - 0x20, 0x40);
					dump_bytes("rcx[0..0x60]", (uintptr_t)info->ContextRecord->Rcx, 0x60);
					dump_bytes("r8 [0..0x60]", (uintptr_t)info->ContextRecord->R8, 0x60);
					dump_bytes("r11", (uintptr_t)info->ContextRecord->R11, 0x20);

					// Only meaningful when the fault is outside the game image:
					// that is the game handing bad data across an API boundary.
					const auto* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
					const bool inExe = exe && pc >= (uintptr_t)exe && pc < (uintptr_t)exe + 0x10000000;
					if (!inExe)
						dump_descriptor_arrays(info->ContextRecord);
				}

				// Only access violations get a dump: the breakpoint path is the
				// game's own orderly stop and repeats several times on the way
				// out.
				if (code == 0xC0000005)
					write_minidump(info, "ZeroIV.crash.dmp");

				dump_modules();

				// The error handler is still holding its message here. Try the
				// argument registers first, then anything on the stack that
				// points at text.
				int budget = 12;
				try_log_string("rcx", (uintptr_t)info->ContextRecord->Rcx, budget);
				try_log_string("rdx", (uintptr_t)info->ContextRecord->Rdx, budget);
				try_log_string("r8 ", (uintptr_t)info->ContextRecord->R8, budget);
				try_log_string("r9 ", (uintptr_t)info->ContextRecord->R9, budget);
				try_log_string("rax", (uintptr_t)info->ContextRecord->Rax, budget);
				try_log_string("rbx", (uintptr_t)info->ContextRecord->Rbx, budget);
				try_log_string("rsi", (uintptr_t)info->ContextRecord->Rsi, budget);
				try_log_string("rdi", (uintptr_t)info->ContextRecord->Rdi, budget);
				try_log_string("r10", (uintptr_t)info->ContextRecord->R10, budget);
				try_log_string("r11", (uintptr_t)info->ContextRecord->R11, budget);
				try_log_string("r12", (uintptr_t)info->ContextRecord->R12, budget);
				try_log_string("r13", (uintptr_t)info->ContextRecord->R13, budget);
				try_log_string("r14", (uintptr_t)info->ContextRecord->R14, budget);
				try_log_string("r15", (uintptr_t)info->ContextRecord->R15, budget);
				for (size_t i = 0; i < 64 && budget > 0; ++i)
				{
					uintptr_t word = 0;
					if (!read_word(sp + i * sizeof(uintptr_t), word))
						break;
					try_log_string("stk", word, budget);
				}
				if (info->ContextRecord->Rbp > sp && info->ContextRecord->Rbp - sp < 0x4000)
				{
					for (size_t i = 0; i < 32 && budget > 0; ++i)
					{
						uintptr_t word = 0;
						if (!read_word((uintptr_t)info->ContextRecord->Rbp + i * sizeof(uintptr_t), word))
							break;
						try_log_string("rbp", word, budget);
					}
				}

				char frame[160] = { 0 };
				if (describe(pc, frame, sizeof(frame)))
					logger::write("fatal", "  pc   %s", frame);

				size_t printed = 0;
				char previous[160] = { 0 };
				for (size_t offset = 0; offset + sizeof(uintptr_t) <= kMaxStackBytes && printed < kMaxFrames; offset += sizeof(uintptr_t))
				{
					uintptr_t word = 0;
					if (!read_word(sp + offset, word))
						break;
					char desc[160] = { 0 };
					if (!describe(word, desc, sizeof(desc)))
						continue;
					if (strcmp(desc, previous) == 0)
						continue;
					snprintf(previous, sizeof(previous), "%s", desc);
					logger::write("fatal", "  #%-2d %s", (int)printed, desc);
					++printed;
				}
			}

			logger::flush();
			InterlockedExchange(&g_reporting, 0);
		}

		// The game prints the reason for stopping here, one exception before the
		// breakpoint. The string is read with ReadProcessMemory so a bogus
		// pointer cannot fault inside the handler.
		void report_debug_output(EXCEPTION_POINTERS* info)
		{
			const uint32_t code = info->ExceptionRecord->ExceptionCode;
			const bool wide = (code == 0x4001000A);
			const ULONG_PTR ptr = info->ExceptionRecord->ExceptionInformation[0];
			if (ptr == 0)
				return;

			char text[512] = { 0 };
			SIZE_T got = 0;
			if (wide)
			{
				wchar_t wtext[256] = { 0 };
				if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ptr, wtext, sizeof(wtext) - sizeof(wchar_t), &got))
					return;
				wtext[(sizeof(wtext) / sizeof(wchar_t)) - 1] = 0;
				WideCharToMultiByte(CP_ACP, 0, wtext, -1, text, (int)sizeof(text) - 1, nullptr, nullptr);
			}
			else
			{
				if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ptr, text, sizeof(text) - 1, &got))
					return;
				text[sizeof(text) - 1] = 0;
			}

			// Output snippets are the game's state dumps; keep them one line.
			for (char* p = text; *p; ++p)
			{
				if (*p == '\r' || *p == '\n')
					*p = ' ';
			}
			if (text[0] == 0)
				return;

			logger::write("game", "%s", text);
			logger::flush();
		}

		LONG CALLBACK handler(EXCEPTION_POINTERS* info)
		{
			if (!info || !info->ExceptionRecord)
				return EXCEPTION_CONTINUE_SEARCH;

			const uint32_t code = info->ExceptionRecord->ExceptionCode;

			// Debug output is not a fault: the game and Wine use it for
			// ordinary messages, including the error text emitted just before a
			// deliberate stop.
			if (code == 0x40010006 || code == 0x4001000A)
			{
				report_debug_output(info);
				return EXCEPTION_CONTINUE_SEARCH;
			}

			// C++ exceptions are thrown by normal code, and informational or
			// warning codes are not fatal. Only errors (0xC......) and the
			// deliberate breakpoint are worth recording.
			const bool fatal = (code & 0xC0000000) == 0xC0000000 || code == 0x80000003;
			if (fatal)
				report(info);

			// Never swallow the exception: the game's own handler must still
			// run, otherwise a fatal error would become a silent hang.
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void install()
	{
		if (AddVectoredExceptionHandler(1, handler) == nullptr)
			logger::write("info", "forensics: vectored handler not installed");
		else
			logger::write("info", "forensics: handler installed");

		// Resolved here, once, while the process is healthy. Writing a dump
		// from inside the handler cannot take the loader lock safely.
		if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll"))
		{
			g_miniDumpWrite = (MiniDumpWriteDumpFn)GetProcAddress(dbghelp, "MiniDumpWriteDump");
			if (g_miniDumpWrite)
				logger::write("info", "forensics: minidump writer ready");
			else
				logger::write("info", "forensics: MiniDumpWriteDump not found");
		}
		else
		{
			logger::write("info", "forensics: dbghelp.dll unavailable");
		}

		// Hooking the debug-output API itself, because the exception form of it
		// only exists while a debugger is attached.
		if (memory::HookApi(L"kernel32.dll", "OutputDebugStringA",
			(PVOID)hookOutputDebugStringA, (PVOID*)&g_origOdsA) &&
			memory::HookApi(L"kernel32.dll", "OutputDebugStringW",
				(PVOID)hookOutputDebugStringW, (PVOID*)&g_origOdsW))
			logger::write("info", "forensics: debug output captured");
		else
			logger::write("info", "forensics: debug output not hooked");
	}

	void note_file(const char* path)
	{
		if (!path || !*path)
			return;
		const LONG idx = g_noteIndex;
		// Cheap repeat suppression: the same file is opened over and over.
		if (idx > 0)
		{
			const char* last = g_notes[(idx - 1) % kNoteCount];
			if (strcmp(last, path) == 0)
				return;
		}

		char* slot = g_notes[idx % kNoteCount];
		const size_t len = strlen(path);
		const size_t copy = len < kNoteLen - 1 ? len : kNoteLen - 1;
		memcpy(slot, path, copy);
		slot[copy] = 0;
		InterlockedIncrement(&g_noteIndex);
	}

	// Writes the game's mapped image to disk. The executable on disk is packed,
	// so its .text and .rdata cannot be disassembled or string-searched; the
	// in-process image is already relocated and decrypted, which makes it the
	// only source for resolving an assert address to a function or a message.
	// The manifest records where each section landed so an offline disassembler
	// can be pointed at the right file offset.
	void dump_image(const char* outDir)
	{
		auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
		if (!base)
		{
			logger::write("dump", "no module base");
			return;
		}

		const uint32_t lfanew = *reinterpret_cast<const uint32_t*>(base + 0x3C);
		if (lfanew == 0 || lfanew > 0x1000)
		{
			logger::write("dump", "bad DOS header");
			return;
		}
		const uint8_t* nt = base + lfanew;
		if (nt[0] != 'P' || nt[1] != 'E')
		{
			logger::write("dump", "bad NT header");
			return;
		}
		const uint16_t sectionCount = *reinterpret_cast<const uint16_t*>(nt + 6);
		const uint16_t optSize = *reinterpret_cast<const uint16_t*>(nt + 20);
		const uint8_t* opt = nt + 24;
		const uint32_t sizeOfImage = optSize >= 60 ? *reinterpret_cast<const uint32_t*>(opt + 56) : 0;

		char binPath[512];
		char txtPath[512];
		snprintf(binPath, sizeof(binPath), "%s\\ZeroIV.image.bin", outDir);
		snprintf(txtPath, sizeof(txtPath), "%s\\ZeroIV.image.txt", outDir);

		FILE* bin = nullptr;
		if (fopen_s(&bin, binPath, "wb") != 0 || !bin)
		{
			logger::write("dump", "cannot open %s", binPath);
			return;
		}
		FILE* txt = nullptr;
		if (fopen_s(&txt, txtPath, "w") != 0 || !txt)
		{
			fclose(bin);
			logger::write("dump", "cannot open %s", txtPath);
			return;
		}

		fprintf(txt, "base=0x%llX size=0x%X\n", (unsigned long long)(uintptr_t)base, sizeOfImage);
		const uint8_t* sec = opt + optSize;
		uint64_t written = 0;
		for (uint16_t i = 0; i < sectionCount; ++i, sec += 40)
		{
			char name[9] = { 0 };
			memcpy(name, sec, 8);
			const uint32_t vSize = *reinterpret_cast<const uint32_t*>(sec + 8);
			const uint32_t vAddr = *reinterpret_cast<const uint32_t*>(sec + 12);
			if (vSize == 0)
				continue;
			const size_t n = fwrite(base + vAddr, 1, vSize, bin);
			fprintf(txt, "section=%s rva=0x%X vsize=0x%X offset=0x%llX\n",
				name, vAddr, vSize, (unsigned long long)written);
			written += n;
		}
		fclose(bin);
		fclose(txt);
		logger::write("dump", "wrote %s (%llu bytes)", binPath, (unsigned long long)written);
	}
}
