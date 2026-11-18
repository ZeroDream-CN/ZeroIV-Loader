#pragma once
#include <cstdarg>

namespace logger
{
	void init();
	// Rare, important events. Always recorded (subject to flood control).
	void write(const char* type, const char* msg, ...);
	// Per-file-operation traces. Compiled out unless _LOG is defined; otherwise
	// enabled through the [log] switches in ZeroIV.ini.
	void log(const char* type, const char* msg, ...);
	void vwrite(const char* type, const char* msg, va_list args);
	// Records one line, bypassing the per-second allowance. For rare events.
	void vwrite_impl(const char* type, const char* msg, va_list args, bool essential);
	// Reserves one line of the category's per-second allowance. Cheap enough to
	// call on every hot-path log so over-budget lines are dropped before they
	// are formatted.
	bool allow_line(const char* type);
	// Flushes the buffered log file (called on detach).
	void flush();
	// Re-probes the console handle. Call after AllocConsole().
	void console_attached();
}
