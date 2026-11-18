#pragma once

#include <cstdint>

namespace bindhooks
{
	// Instruments the game's resource binder so the descriptor arrays handed to
	// ID3D12Device::CopyDescriptors are on record. The recorder runs inside a
	// render thread for every bind, so it writes into a fixed ring buffer with
	// no allocation, no logging and no locks; the crash handler drains it.
	struct BindRecord
	{
		uint64_t sequence;
		uint64_t count;
		uint64_t emptySlots;
		uint64_t zeroSizes;
		uint64_t handles[16];
		uint32_t sizes[16];
	};

	constexpr int kBindRingSize = 64;
	extern BindRecord g_bindRing[kBindRingSize];
	extern volatile long g_bindSequence;

	// Call once from the game thread after hooks are installed.
	void install();

	// Logs the most recent records; used by the fatal exception path.
	void dump_recent(int howMany);
}
