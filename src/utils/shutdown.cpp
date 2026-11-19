#include "main.h"
#include "shutdown.h"

#include <atomic>
#include <chrono>
#include <thread>

/*
	Two separate concerns live here, both about the process being able to die.

	1. Tracing. When the game window closes but the process lingers, it matters a
	   great deal whether the loader ever got its detach notification. The stack of
	   a hung process shows GTA5's own frames, which cannot be attributed without
	   its symbols, so the only way to place the hang relative to this module is to
	   leave a marker at each step. That is what trace() is for, and it is
	   deliberately primitive: raw Win32, no locks, no C++ streams, no allocation,
	   because the whole point is to work when normal machinery is already torn
	   down. It writes its own file so it cannot interfere with ZeroIV.log.

	2. The watchdog. Whatever blocks, the process must not survive the window. A
	   healthy teardown finishes in milliseconds, so a grace period is a reliable
	   signal that something is stuck, and terminating there is exactly what the
	   player would otherwise do by hand.
*/

namespace
{
	// Generous on purpose: anything still running after this is hung, not slow.
	constexpr int kDefaultGraceSeconds = 10;
	const char* kTraceFile = "ZeroIV.exit.log";

	std::atomic<bool> g_stopping{ false };
	std::atomic<bool> g_armed{ false };
	void(WINAPI* g_origExitProcess)(UINT) = nullptr;

	void watchdog(HANDLE process, UINT code, int graceSeconds)
	{
		const auto deadline =
			std::chrono::steady_clock::now() + std::chrono::seconds(graceSeconds);
		while (std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));

		// Still here: the teardown never returned. The window is gone and this
		// module holds no unsaved state, so cutting the process loses nothing.
		::TerminateProcess(process, code);
	}

	void WINAPI hookExitProcess(UINT code)
	{
		// ExitProcess can be reached more than once in a shutdown cascade, and the
		// watchdog must only be armed once.
		const bool first = !g_armed.exchange(true);
		g_stopping.store(true);

		exitguard::trace("ExitProcess called");

		int grace = config::get_value<int>("config", "exitTimeoutSeconds", kDefaultGraceSeconds);
		if (grace < 0)
			grace = 0;

		if (first && grace > 0)
		{
			// Created here, not at start-up: an idle session then carries no extra
			// thread, and this is still before the loader lock is taken for detach.
			try
			{
				std::thread(watchdog, ::GetCurrentProcess(), code, grace).detach();
			}
			catch (...)
			{
				// Out of thread handles: the exit proceeds unwatched.
			}
		}

		if (g_origExitProcess != nullptr)
			g_origExitProcess(code);

		// The original does not return; if it ever does, do not hand control back.
		::TerminateProcess(::GetCurrentProcess(), code);
	}
}

namespace exitguard
{
	void trace(const char* text)
	{
		// FILE_APPEND_DATA makes every write land at the end, so this can run
		// concurrently with itself or with the log writer without seeking.
		HANDLE file = ::CreateFileA(kTraceFile, FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
			return;

		SYSTEMTIME st{};
		::GetLocalTime(&st);

		char line[256]{};
		const int length = ::wsprintfA(line, "[%02d:%02d:%02d.%03d][pid %lu] %s\r\n",
			st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, ::GetCurrentProcessId(), text);
		if (length > 0)
		{
			DWORD written = 0;
			::WriteFile(file, line, static_cast<DWORD>(length), &written, nullptr);
		}
		::CloseHandle(file);
	}

	void install()
	{
		if (!memory::HookApi(L"kernel32.dll", "ExitProcess", (PVOID)hookExitProcess,
			(PVOID*)&g_origExitProcess))
			return;

		const int grace = config::get_value<int>("config", "exitTimeoutSeconds",
			kDefaultGraceSeconds);
		if (grace > 0)
			logger::write("info", "shutdown: exit watchdog armed (%ds grace)", grace);
		else
			logger::write("info", "shutdown: exit watchdog disabled by config");
	}

	bool stopping()
	{
		return g_stopping.load();
	}
}

