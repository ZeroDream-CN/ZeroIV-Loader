#include "main.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
	constexpr const char* kLogFile = "ZeroIV.log";

	// The device/file hooks log once per file operation, so a GTA streaming
	// session can produce tens of thousands of lines per second. Unthrottled
	// that stalls the game thread (file I/O) and floods ZeroIV's debug console,
	// which is exactly the runaway this gate prevents.
	constexpr int kHotLinesPerSecond = 120;
	constexpr int kLinesPerSecond = 500;
	// One identical message repeated inside this window is collapsed: the
	// first line is written, the rest count up, and a summary is emitted when
	// the window rolls over.
	constexpr auto kRepeatWindow = std::chrono::milliseconds(1000);
	constexpr size_t kMaxTrackedRepeats = 256;
	// A sweep that reports every expired repeat can emit hundreds of lines at
	// once; cap it and summarise the rest.
	constexpr int kMaxSweepNotes = 6;
	constexpr int kMaxBudgetTypes = 64;
	// Hot categories (hundreds of lines per second) are flushed in batches;
	// everything else is flushed on the spot so a crash still leaves a log.
	constexpr unsigned kFlushEveryLines = 64;
	// A single over-long path must not overflow the stack buffer.
	constexpr size_t kBufferSize = 1024;

	using Clock = std::chrono::steady_clock;

	struct Budget
	{
		Clock::time_point windowStart{};
		int used = 0;
		int dropped = 0;
	};

	struct Repeat
	{
		Clock::time_point since{};
		int count = 0;
		std::string message;
	};

	std::mutex g_mutex;
	std::ofstream g_file;
	unsigned g_pendingFlush = 0;
	Clock::time_point g_lastRepeatSweep{};
	std::unordered_map<std::string, Budget> g_budgets;
	std::unordered_map<std::string, Repeat> g_repeats;

	// Console sink. Deliberately synchronous and thread-free: a helper thread
	// cannot be created from DllMain (loader lock) nor safely from a hook that
	// may run before the game finished initialising, so the blocking cause is
	// removed instead. A console write only stalls while the window has text
	// selected, which QuickEdit is responsible for.
	bool g_consoleWant = false;
	bool g_consoleProbed = false;
	HANDLE g_console = nullptr;

	bool is_hot_category(const char* type)
	{
		return type != nullptr
			&& (std::strcmp(type, "device") == 0
				|| std::strcmp(type, "mods") == 0
				|| std::strcmp(type, "rpf") == 0);
	}

	int budget_for(const char* type)
	{
		return is_hot_category(type) ? kHotLinesPerSecond : kLinesPerSecond;
	}

	// Resolves the console handle on first use. It cannot be cached at init
	// time because AllocConsole + freopen run after logger::init() in DllMain.
	bool console_ready()
	{
		if (g_consoleProbed)
			return g_console != nullptr;
		g_consoleProbed = true;
		if (!g_consoleWant)
			return false;

		HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
		if (out == nullptr || out == INVALID_HANDLE_VALUE)
		{
			g_consoleWant = false;
			return false;
		}

		// Only write when stdout really is a console. When the game is started
		// from a shell (or Steam pipes stdout) AllocConsole fails and this
		// handle is a pipe: a synchronous write there can block once the reader
		// falls behind, which is the same stall we are trying to avoid.
		DWORD outMode = 0;
		if (!GetConsoleMode(out, &outMode))
		{
			g_consoleWant = false;
			return false;
		}

		// QuickEdit is what makes a console write wait: while text is selected
		// the console suspends output. Clearing it keeps the game threads from
		// stalling when someone drags over the window.
		HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
		DWORD mode = 0;
		if (in != nullptr && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
			SetConsoleMode(in, (mode | ENABLE_EXTENDED_FLAGS) & ~ENABLE_QUICK_EDIT_MODE);

		g_console = out;
		return true;
	}

	// One WriteFile per line on the handle already opened by the console. No
	// FILE* buffering, no thread, no allocation.
	void write_console(const char* data, size_t length)
	{
		if (!console_ready())
			return;
		DWORD written = 0;
		WriteFile(g_console, data, static_cast<DWORD>(length), &written, nullptr);
	}

	// Caller holds g_mutex. Appends one already-formatted line to the file and
	// forwards it to ZeroIV's debug console.
	void emit(const char* type, const std::string& body, const std::string& line)
	{
		if (!g_file.is_open())
			g_file.open(kLogFile, std::ofstream::out | std::ofstream::app);
		if (g_file.is_open())
		{
			g_file << line << '\n';

			// Every line outside the chatty categories is flushed immediately:
			// a session that dies before the buffer fills must still leave a
			// usable log. Hot categories are batched, where the flush cost per
			// line would otherwise dominate.
			if (!is_hot_category(type) || ++g_pendingFlush >= kFlushEveryLines)
			{
				g_file.flush();
				g_pendingFlush = 0;
			}
		}
		logbridge::enqueue(type, body.c_str());
	}

	// Caller holds g_mutex. Writes a synthetic note that bypasses the limiter
	// so the console can show that lines were dropped rather than silently
	// losing them.
	void emit_note(const char* type, const std::string& body)
	{
		SYSTEMTIME st;
		GetLocalTime(&st);
		char line[kBufferSize]{ 0 };
		snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d][%s]%s",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, type, body.c_str());
		emit(type, body, line);
		write_console(line, std::strlen(line));
		write_console("\n", 1);
	}

	// Caller holds g_mutex. Reports (and forgets) collapsed repeats whose
	// window closed. Without this a burst that just stops would never report
	// its count because the same line never arrives again.
	void flush_stale_repeats(const Clock::time_point now)
	{
		if (now - g_lastRepeatSweep < kRepeatWindow)
			return;
		g_lastRepeatSweep = now;

		int reported = 0;
		int suppressed = 0;
		for (auto it = g_repeats.begin(); it != g_repeats.end();)
		{
			if (now - it->second.since >= kRepeatWindow)
			{
				if (it->second.count > 1)
				{
					if (reported < kMaxSweepNotes)
					{
						++reported;
						emit_note("repeat",
							" previous line repeated " + std::to_string(it->second.count) +
							"x: " + it->second.message);
					}
					else
					{
						++suppressed;
					}
				}
				it = g_repeats.erase(it);
				continue;
			}
			++it;
		}

		// Hundreds of expired repeats in one sweep would otherwise become
		// hundreds of lines in one millisecond.
		if (suppressed > 0)
		{
			emit_note("repeat", " " + std::to_string(suppressed) +
				" more repeated line(s) not shown this window");
		}
	}

	// Caller holds g_mutex. Returns true when the line is a repeat of the one
	// currently inside its collapse window (so the caller should skip it).
	bool collapse_repeat(const char* type, const std::string& body)
	{
		const Clock::time_point now = Clock::now();
		std::string key = type != nullptr ? type : "info";
		key += '\x1f';
		key += body;

		auto it = g_repeats.find(key);
		if (it == g_repeats.end())
		{
			// Bound the map; an endless stream of unique paths must not turn
			// into unbounded memory.
			if (g_repeats.size() >= kMaxTrackedRepeats)
				g_repeats.erase(g_repeats.begin());
			Repeat repeat;
			repeat.since = now;
			repeat.count = 1;
			repeat.message = body;
			g_repeats.emplace(std::move(key), std::move(repeat));
			return false;
		}

		if (now - it->second.since < kRepeatWindow)
		{
			++it->second.count;
			return true;
		}

		const int repeats = it->second.count;
		it->second.since = now;
		it->second.count = 1;
		if (repeats > 1)
		{
			emit_note("repeat",
				" previous line repeated " + std::to_string(repeats) + "x: " + it->second.message);
		}
		return false;
	}
}

void logger::init()
{
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		if (g_file.is_open())
			g_file.close();
		std::error_code ec;
		std::filesystem::remove(kLogFile, ec);
		g_file.open(kLogFile, std::ofstream::out | std::ofstream::app);
		g_pendingFlush = 0;
		g_budgets.clear();
		g_repeats.clear();
	}

	{
		g_consoleWant = config::get_config<bool>("console");
		// Probe after AllocConsole + freopen, which happen later in DllMain.
		g_consoleProbed = false;
		g_console = nullptr;
	}

	// Configuration is already loaded by the time init runs, so the bridge can
	// resolve host/port and whether forwarding is enabled.
	logbridge::init();
}

bool logger::allow_line(const char* type)
{
	std::lock_guard<std::mutex> lock(g_mutex);

	const Clock::time_point now = Clock::now();
	const std::string key = type != nullptr ? type : "info";

	auto it = g_budgets.find(key);
	if (it == g_budgets.end())
	{
		if (g_budgets.size() >= kMaxBudgetTypes)
			g_budgets.erase(g_budgets.begin());
		Budget fresh;
		fresh.windowStart = now;
		fresh.used = 1;
		g_budgets.emplace(key, fresh);
		return true;
	}

	Budget& budget = it->second;
	if (now - budget.windowStart >= std::chrono::seconds(1))
	{
		budget.windowStart = now;
		budget.used = 0;
		if (budget.dropped > 0)
		{
			emit_note("rate-limit",
				" dropped " + std::to_string(budget.dropped) + " line(s) from '" + key + "'");
			budget.dropped = 0;
		}
	}

	if (budget.used >= budget_for(type))
	{
		++budget.dropped;
		return false;
	}
	++budget.used;
	return true;
}

void logger::vwrite(const char* type, const char* msg, va_list args)
{
    vwrite_impl(type, msg, args, false);
}

void logger::vwrite_impl(const char* type, const char* msg, va_list args, bool essential)
{
    // Reject over-budget categories before formatting: with every category
    // enabled the hooks can call this thousands of times per second and most of
    // those calls only exist to be thrown away. Essential lines (logger::write)
    // are never throttled; they still collapse repeats.
    if (!essential && !logger::allow_line(type))
        return;

    char buffer[kBufferSize]{ 0 };
    char timeBuffer[32]{ 0 };

    SYSTEMTIME st;
    GetLocalTime(&st);

    snprintf(timeBuffer, sizeof(timeBuffer),
        "%02d:%02d:%02d.%03d",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    int offset = snprintf(buffer, sizeof(buffer), "[%s][%s]", timeBuffer, type);

    if (offset < 0 || offset >= (int)sizeof(buffer))
        offset = sizeof(buffer) - 1;

    vsnprintf(buffer + offset, sizeof(buffer) - offset, msg, args);

    const std::string body(buffer + offset);
    const std::string line(buffer);

	{
		std::lock_guard<std::mutex> lock(g_mutex);

		// Order matters: collapse first (cheap for a repeated line) and let the
		// repeat summary stand in for the dropped copies.
		flush_stale_repeats(Clock::now());
		if (collapse_repeat(type, body))
			return;

		emit(type, body, line);
	}

	// Outside the lock: a console write can be slow, and no other thread should
	// wait on the console because of it.
	write_console(line.c_str(), line.size());
	write_console("\n", 1);
}

// This function will log things that happen rarely
void logger::write(const char* type, const char* msg, ...)
{
    va_list args;
    va_start(args, msg);
    vwrite_impl(type, msg, args, true);
    va_end(args);
}

// Per-file-operation traces from the hooks. With _LOG they are always
// recorded; in a regular release build each category is opt-in so an idle
// session stays quiet and a traced one stays usable.
void logger::log(const char* type, const char* msg, ...)
{
#ifndef _LOG
    if (!config::get_log(type))
        return;
#endif
    va_list args;
    va_start(args, msg);
    vwrite(type, msg, args);
    va_end(args);
}

void logger::flush()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file.is_open())
        g_file.flush();
}

void logger::flush_nolock()
{
	if (g_file.is_open())
		g_file.flush();
}

void logger::console_attached()
{
	// Called after AllocConsole(). Any probe made before that saw the inherited
	// (usually absent) stdout handle and would keep console output disabled
	// forever, so drop the cached result.
	g_consoleProbed = false;
	g_console = nullptr;
	g_consoleWant = true;
}
