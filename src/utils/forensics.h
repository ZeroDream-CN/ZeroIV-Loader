#pragma once

namespace forensics
{
	// Installs a vectored exception handler that records fatal exceptions (with
	// the stack that produced them) and the game's debug output into the log.
	// The game raises a breakpoint of its own when it decides to stop, and it
	// prints the reason through OutputDebugString immediately beforehand; that
	// text is the only explanation the game ever gives.
	void install();

	// Records the file the engine is touching right now. Kept tiny and
	// allocation-free because it runs on the streaming hot path.
	void note_file(const char* path);

	// Dumps the decrypted in-process image (all sections) to outDir, so an
	// offline disassembler can resolve an assert address the packed executable
	// on disk cannot. Writes ZeroIV.image.bin plus a section manifest.
	void dump_image(const char* outDir);
}
