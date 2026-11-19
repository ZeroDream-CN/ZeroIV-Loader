#pragma once

// Keeps the process from lingering after the game window is gone, and records
// how far a shutdown got when it does.
//
// A normal exit runs ExitProcess, which terminates every other thread and then
// calls each module's DllMain(DLL_PROCESS_DETACH) while holding the loader lock.
// If anything on that path blocks -- a mutex a terminated thread still owned, a
// file lock, a driver -- the last thread never returns and the process sits in
// memory with no window: the player has quit, yet the task cannot be removed.
//
// This module cannot fix other modules, so it does two things: it leaves a
// marker at each step so a hang can be attributed, and it arms a watchdog that
// terminates the process if the teardown does not finish in time.
namespace exitguard
{
	// Hooks ExitProcess. Call once from the loader's init path.
	void install();

	// True once a process exit has begun. Helper threads poll this to stop.
	bool stopping();

	// Appends one line to ZeroIV.exit.log.
	//
	// Usable from DllMain(DLL_PROCESS_DETACH): raw Win32 only, no locks, no
	// streams, no allocation, and never a wait. Writes its own file so it cannot
	// interfere with the log that other code is still writing.
	void trace(const char* text);
}

