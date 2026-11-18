#pragma once

// Real-time log forwarding from the ASI loaded inside GTA V to ZeroIV's debug
// console (rpf-browser-go). The transport is a loopback TCP stream: the game
// process dials 127.0.0.1:<port> and streams newline-delimited records.
//
// Nothing here may block the game thread: lines are queued in memory and a
// detached sender thread owns the socket.
namespace logbridge
{
	// Reads the [debugConsole] section of ZeroIV.ini. Called once from
	// logger::init() after config::load().
	void init();

	// DllMain runs under the loader lock, where creating a thread is unsafe.
	// While this flag is set the sender is only queued, never started.
	void set_inside_dll_main(bool inside);

	// Queues one formatted line. `type` is the logging category used by
	// logger::write (info/device/mods/rpf/...) and becomes the console module.
	void enqueue(const char* type, const char* message);

	// Starts the sender thread when allowed. enqueue() calls this itself; it is
	// exported so a game-thread call site can flush lines queued from DllMain.
	void ensure_started();
}
