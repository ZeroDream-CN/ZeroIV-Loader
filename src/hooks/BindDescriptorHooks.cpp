#include "main.h"
#include "hooks/BindDescriptorHooks.h"

#include <cstdint>
#include <cstdio>

/*
	Why this exists
	---------------
	An add-on vehicle converted Gen8 -> Gen9 kills the game inside the NVIDIA
	D3D12 user-mode driver at a fixed instruction:

		nvwgf2umx.dll+0x258D2E   vmovdqu ymm0, ymmword ptr [rax]
		rax = 0

	The call path is the game's resource binder. It walks a shader's resource
	declarations, resolves each one against a per-class table, and then binds
	the whole batch with a single ID3D12Device::CopyDescriptors:

		NumDestRanges          = 1
		pDestRangeStarts       = &stack_handle
		pDestRangeSizes        = &stack_size
		NumSrcRanges           = count          <-- [rsp+0x20]
		pSrcRangeStarts        = handles array  <-- [rsp+0x28]
		pSrcRangeSizes         = sizes array    <-- [rsp+0x30]
		DescriptorHeapsType    = 0

	Both arrays are carved out of the frame with `sub rsp` and are never
	zero-initialised, so any slot the binder fails to fill keeps whatever was
	on the stack. When that garbage is zero the driver is handed a null
	descriptor and faults on the first read.

	Registers cannot be used for this at fault time: by then the faulting frame
	is the driver's, and r9/r13 are its loop counter and accumulator. The only
	place where the arrays are still the game's is the instruction immediately
	before the call, so that instruction is replaced with a jump to a stub that
	records the arguments and then executes the original call.

	The stub is entered with rax = the device vtable and the stack arguments
	already in place, so it must preserve every register, call the checker
	without disturbing the pending call, then perform
	`call qword ptr [rax+0xb8]` itself and jump back to the following
	instruction.
*/

// Referenced directly from the stub's assembly, so it needs external linkage
// and an unmangled name.
extern "C" uintptr_t g_afterCall = 0;

namespace
{
	// The instruction that is replaced. The bytes are verified before the
	// patch, so a game build whose layout moved is left untouched instead of
	// being corrupted by a blind write.
	constexpr const char* kCallSitePattern =
		"48 8B 55 F0 89 54 24 20 C7 44 24 38 00 00 00 00 "
		"4C 8D 45 E8 4C 8D 4D FC BA 01 00 00 00 FF 90 B8 00 00 00";

	// The six bytes of `call qword ptr [rax+0xB8]` at the end of that pattern.
	constexpr size_t kCallLength = 6;

	bool g_installed = false;

	// Called from the stub with the game's own arguments (verified against the
	// live instruction bytes: [rsp+0x20] count, [rsp+0x28] handles,
	// [rsp+0x30] sizes). It must never allocate, log or take a lock, because it
	// runs on a render thread for every descriptor bind of every draw; an
	// earlier version logged here and corrupted the heap.
	extern "C" void CheckBind(uintptr_t handles, uint64_t count, uintptr_t sizes)
	{
		const long seq = InterlockedIncrement((volatile long*)&bindhooks::g_bindSequence);
		bindhooks::BindRecord& r = bindhooks::g_bindRing[(seq - 1) & (bindhooks::kBindRingSize - 1)];
		r.sequence = (uint64_t)seq;
		r.count = count;
		r.emptySlots = 0;
		r.zeroSizes = 0;
		for (int i = 0; i < 16; ++i)
		{
			r.handles[i] = 0;
			r.sizes[i] = 0;
		}

		if (handles == 0 || count == 0)
			return;

		// Both arrays live on the calling thread's stack, so they are directly
		// readable; the clamp keeps a corrupt count from walking off the end.
		uint64_t limit = count;
		if (limit > 2048)
			limit = 2048;

		const uintptr_t* h = reinterpret_cast<const uintptr_t*>(handles);
		const uint32_t* s = reinterpret_cast<const uint32_t*>(sizes);

		for (uint64_t i = 0; i < limit; ++i)
		{
			const uintptr_t v = h[i];
			const uint32_t sz = s ? s[i] : 0;
			if (v == 0)
				++r.emptySlots;
			if (s && sz == 0)
				++r.zeroSizes;
			if (i < 16)
			{
				r.handles[i] = v;
				r.sizes[i] = sz;
			}
		}
	}

	// The stub is emitted as raw machine code rather than written in inline
	// assembly: the tail has to jump to an absolute address held in a variable,
	// and a RIP-relative reference to a global from a naked function is not
	// relocated reliably by MinGW. Every operand here is absolute or
	// stack-relative, so the block is position independent and can live in any
	// executable allocation.
	//
	// `pushfq` plus the seven register pushes is EIGHT 8-byte pushes, i.e. the
	// stack moves by 0x40, not 0x48. The outgoing arguments that were at
	// [rsp+0x20..0x38] are therefore at [rsp+0x60..0x78]:
	//
	//   pushfq / push rax rcx rdx r8 r9 r10 r11      (8 x 8 = 0x40 bytes)
	//   mov rcx,[rsp+0x68]    pSrcRangeStarts   (was [rsp+0x28])
	//   mov edx,[rsp+0x60]    NumSrcRanges      (was [rsp+0x20], 32-bit store)
	//   mov r8, [rsp+0x70]    pSrcRangeSizes    (was [rsp+0x30])
	//   sub rsp,0x20 / call CheckBind / add rsp,0x20   (shadow space + align)
	//   pop r11 r10 r9 r8 rdx rcx rax / popfq
	//   call qword ptr [rax+0xB8]      the replaced CopyDescriptors call
	//   movabs r11, <resume> / jmp r11
	// Offsets are located by scanning for the two `movabs r11` (49 BB) opcodes
	// instead of being written out by hand, so a layout change cannot silently
	// patch the wrong immediate.
	size_t g_stubBindImm = 0;
	size_t g_stubResumeImm = 0;
	size_t g_stubSize = 0;

	uint8_t* g_stub = nullptr;
	void* g_stubMemory = nullptr;

	// `jmp rel32` can only reach +/-2 GB, so the stub must be allocated next to
	// the patched code. Handing VirtualAlloc a hint address and walking
	// outwards finds a free page inside that window; allocating anywhere and
	// hoping is what produced a truncated jump to a garbage address.
	void* allocate_near(uintptr_t nearAddr, size_t size)
	{
		const uintptr_t step = 0x10000;
		const uintptr_t maxDelta = 0x70000000; // comfortably inside +/-2 GB
		for (uintptr_t d = step; d < maxDelta; d += step)
		{
			void* p = VirtualAlloc(reinterpret_cast<void*>(nearAddr + d), size,
				MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p)
				return p;
			if (nearAddr > d)
			{
				p = VirtualAlloc(reinterpret_cast<void*>(nearAddr - d), size,
					MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
				if (p)
					return p;
			}
		}
		return nullptr;
	}

	bool build_stub(uintptr_t nearAddr)
	{
		if (g_stub)
			return true;

		g_stubMemory = allocate_near(nearAddr, 0x1000);
		if (!g_stubMemory)
		{
			logger::write("info", "bind: no executable page near 0x%llX (%lu)",
				(unsigned long long)nearAddr, GetLastError());
			return false;
		}
		logger::write("info", "bind: stub page 0x%llX, distance %lld bytes",
			(unsigned long long)(uintptr_t)g_stubMemory,
			(long long)((intptr_t)(uintptr_t)g_stubMemory - (intptr_t)nearAddr));

		static const uint8_t preamble[] = {
			0x9C,                                     // pushfq
			0x50,                                     // push rax
			0x51,                                     // push rcx
			0x52,                                     // push rdx
			0x41, 0x50,                               // push r8
			0x41, 0x51,                               // push r9
			0x41, 0x52,                               // push r10
			0x41, 0x53,                               // push r11
			0x48, 0x8B, 0x4C, 0x24, 0x68,             // mov rcx,[rsp+0x68] pSrcRangeStarts
			0x8B, 0x54, 0x24, 0x60,                   // mov edx,[rsp+0x60] NumSrcRanges (32-bit!)
			0x4C, 0x8B, 0x44, 0x24, 0x70,             // mov r8,[rsp+0x70]  pSrcRangeSizes
			0x48, 0x83, 0xEC, 0x20,                   // sub rsp,0x20
			0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs r11, <CheckBind>
			0x41, 0xFF, 0xD3,                         // call r11
			0x48, 0x83, 0xC4, 0x20,                   // add rsp,0x20
			0x41, 0x5B,                               // pop r11
			0x41, 0x5A,                               // pop r10
			0x41, 0x59,                               // pop r9
			0x41, 0x58,                               // pop r8
			0x5A,                                     // pop rdx
			0x59,                                     // pop rcx
			0x58,                                     // pop rax
			0x9D,                                     // popfq
			0xFF, 0x90, 0xB8, 0x00, 0x00, 0x00,       // call [rax+0xB8]
			0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0,       // movabs r11, <resume>
			0x41, 0xFF, 0xE3,                         // jmp r11
		};

		memcpy(g_stubMemory, preamble, sizeof(preamble));
		g_stub = reinterpret_cast<uint8_t*>(g_stubMemory);
		g_stubSize = sizeof(preamble);

		// The two immediates are the only `movabs r11, imm64` in the block: the
		// first loads CheckBind, the second the address to resume at.
		size_t found = 0;
		for (size_t i = 0; i + 10 <= sizeof(preamble); ++i)
		{
			if (preamble[i] != 0x49 || preamble[i + 1] != 0xBB)
				continue;
			++found;
			if (found == 1)
				g_stubBindImm = i + 2;
			else if (found == 2)
				g_stubResumeImm = i + 2;
		}
		if (found != 2)
		{
			logger::write("info", "bind: stub has %llu movabs opcodes, expected 2",
				(unsigned long long)found);
			return false;
		}

		const uint64_t bind = (uint64_t)(uintptr_t)&CheckBind;
		memcpy(g_stub + g_stubBindImm, &bind, sizeof(bind));

		// Recorded once so a patch that landed on the wrong offset is visible
		// in the log instead of only in a crash dump.
		{
			char line[256] = { 0 };
			size_t used = 0;
			for (size_t i = 0; i < g_stubSize && used + 4 < sizeof(line); ++i)
				used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", g_stub[i]);
			logger::write("info", "bind: stub %llu bytes bindImm=%llu resumeImm=%llu",
				(unsigned long long)g_stubSize, (unsigned long long)g_stubBindImm,
				(unsigned long long)g_stubResumeImm);
			logger::write("info", "bind: stub code %s", line);
		}
		return true;
	}

	void stub_set_resume(uintptr_t address)
	{
		const uint64_t value = (uint64_t)address;
		memcpy(g_stub + g_stubResumeImm, &value, sizeof(value));
		FlushInstructionCache(GetCurrentProcess(), g_stub, g_stubSize);
	}
}

namespace bindhooks
{
	BindRecord g_bindRing[kBindRingSize] = {};
	volatile long g_bindSequence = 0;

	// Drains the newest records. Called from the fatal path, where allocating
	// or taking locks is unsafe, so every record is formatted into a stack
	// buffer and written straight through.
	void dump_recent(int howMany)
	{
		const long total = g_bindSequence;
		if (total <= 0)
		{
			logger::write("fatal", "  bind: no CopyDescriptors call was recorded");
			return;
		}
		if (howMany > total)
			howMany = (int)total;

		const long first = total - howMany;
		for (long i = first; i < total; ++i)
		{
			const BindRecord& r = g_bindRing[i & (kBindRingSize - 1)];
			if (r.sequence == 0)
				continue;
			logger::write("fatal", "  bind #%llu: count=%llu empty=%llu zeroSize=%llu",
				(unsigned long long)r.sequence, (unsigned long long)r.count,
				(unsigned long long)r.emptySlots, (unsigned long long)r.zeroSizes);

			const uint64_t limit = r.count < 16 ? r.count : 16;
			char line[512] = { 0 };
			size_t used = 0;
			for (uint64_t k = 0; k < limit && used + 26 < sizeof(line); ++k)
				used += (size_t)snprintf(line + used, sizeof(line) - used, "#%llu=0x%llX/%u ",
					(unsigned long long)k, (unsigned long long)r.handles[k], r.sizes[k]);
			logger::write("fatal", "    %s", line);
		}
	}

	void install()
	{
		if (g_installed)
			return;

		// Presence of this file disables the hook, so the unmodified code path
		// can be compared against the hooked one without a rebuild.
		{
			wchar_t path[MAX_PATH] = { 0 };
			HMODULE self = GetModuleHandleW(L"ZeroIV.asi");
			if (!self)
				self = GetModuleHandleW(nullptr);
			DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
			if (n > 0 && n < MAX_PATH)
			{
				wchar_t* slash = wcsrchr(path, L'\\');
				if (slash)
				{
					wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"ZeroIV.nobind");
					if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
					{
						logger::write("info", "bind: disabled by ZeroIV.nobind");
						return;
					}
				}
			}
		}

		auto site = memory::scan(kCallSitePattern, true);
		if (site.address == 0)
		{
			logger::write("info", "bind: CopyDescriptors call site not found");
			return;
		}

		const uintptr_t callAddr = site.address + 29; // the 6-byte call is the pattern's tail
		auto* p = reinterpret_cast<uint8_t*>(callAddr);
		if (p[0] != 0xFF || p[1] != 0x90 || p[2] != 0xB8)
		{
			logger::write("info", "bind: unexpected bytes at call site 0x%llX",
				(unsigned long long)callAddr);
			return;
		}

		// The pattern is checked against the live process image (the PE copy on
		// disk has different offsets), so the bytes around the match are logged
		// to prove the hit is the call we expect.
		{
			char line[256] = { 0 };
			size_t used = 0;
			const uint8_t* around = reinterpret_cast<const uint8_t*>(site.address) - 16;
			for (int i = 0; i < 56 && used + 4 < sizeof(line); ++i)
				used += (size_t)snprintf(line + used, sizeof(line) - used, "%02X", around[i]);
			logger::write("info", "bind: site 0x%llX-16..+40: %s",
				(unsigned long long)site.address, line);
		}

		if (!build_stub(callAddr))
			return;
		stub_set_resume(callAddr + kCallLength);
		g_afterCall = callAddr + kCallLength;

		// Verify the jump can actually reach the stub before writing anything.
		const int64_t reach = (int64_t)(uintptr_t)g_stub - (int64_t)(callAddr + 5);
		if (reach > 0x7FFFFFFFLL || reach < -0x80000000LL)
		{
			logger::write("info", "bind: stub 0x%llX is out of jmp rel32 range from 0x%llX",
				(unsigned long long)(uintptr_t)g_stub, (unsigned long long)callAddr);
			return;
		}

		DWORD oldProtect = 0;
		if (!VirtualProtect(p, kCallLength, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			logger::write("info", "bind: VirtualProtect failed (%lu)", GetLastError());
			return;
		}

		// jmp rel32 (5 bytes) + nop, over the original 6-byte call.
		const int64_t delta = (int64_t)(uintptr_t)g_stub - (int64_t)(callAddr + 5);
		p[0] = 0xE9;
		*reinterpret_cast<int32_t*>(p + 1) = (int32_t)delta;
		p[5] = 0x90;

		VirtualProtect(p, kCallLength, oldProtect, &oldProtect);
		FlushInstructionCache(GetCurrentProcess(), p, kCallLength);

		g_installed = true;
		logger::write("info", "bind: hooked CopyDescriptors at +0x%llX (stub 0x%llX)",
			(unsigned long long)(callAddr - (uintptr_t)GetModuleHandleW(nullptr)),
			(unsigned long long)(uintptr_t)g_stub);
	}
}
