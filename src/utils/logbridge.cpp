#include "logbridge.h"

// winsock2.h must precede windows.h, so this translation unit deliberately does
// not include main.h.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "config.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#ifndef ZEROIV_LOADER_VERSION
#define ZEROIV_LOADER_VERSION "1.0.0"
#endif

namespace
{
	// Must match loaderLogBridgePort in rpf-browser-go/logbridge.go.
	constexpr int kDefaultPort = 27183;
	// Bounds memory when the app is not running: the oldest lines are dropped.
	constexpr size_t kMaxQueuedLines = 1024;
	constexpr int kReconnectDelayMs = 2000;
	constexpr int kIdleWaitMs = 3000;

	std::mutex g_queueMutex;
	std::condition_variable g_queueCv;
	std::deque<std::string> g_queue;

	std::atomic<bool> g_enabled{ false };
	std::atomic<bool> g_started{ false };
	std::atomic<bool> g_insideDllMain{ false };

	std::mutex g_configMutex;
	std::string g_host = "127.0.0.1";
	int g_port = kDefaultPort;

	std::string sanitize(const char* text)
	{
		std::string out = text ? text : "";
		for (char& c : out)
		{
			if (c == '\r' || c == '\n')
				c = ' ';
		}
		if (out.size() > 2000)
			out.resize(2000);
		return out;
	}

	std::string make_line(const char* level, const std::string& module, int64_t millis, const std::string& message)
	{
		std::string line;
		line.reserve(message.size() + module.size() + 40);
		line += level;
		line += '|';
		line += module;
		line += '|';
		line += std::to_string(millis);
		line += '|';
		line += message;
		return line;
	}

	bool send_all(SOCKET sock, const std::string& data)
	{
		size_t sent = 0;
		while (sent < data.size())
		{
			const int chunk = static_cast<int>(std::min<size_t>(data.size() - sent, 4096));
			const int written = send(sock, data.data() + sent, chunk, 0);
			if (written == SOCKET_ERROR || written <= 0)
				return false;
			sent += static_cast<size_t>(written);
		}
		return true;
	}

	SOCKET connect_bridge()
	{
		std::string host;
		int port = kDefaultPort;
		{
			std::lock_guard<std::mutex> lock(g_configMutex);
			host = g_host;
			port = g_port;
		}

		SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (sock == INVALID_SOCKET)
			return INVALID_SOCKET;

		sockaddr_in addr{};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(static_cast<u_short>(port));
		if (InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1)
		{
			closesocket(sock);
			return INVALID_SOCKET;
		}

		// Loopback connects fail fast (WSAECONNREFUSED) when ZeroIV is closed,
		// so a blocking connect here cannot stall for the TCP timeout.
		if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			closesocket(sock);
			return INVALID_SOCKET;
		}
		return sock;
	}

	void sender_loop()
	{
		WSADATA wsaData{};
		if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
			return;

		const std::string hello =
			std::string("ZEROIV-LOADER/1 ZeroIV.asi ") + ZEROIV_LOADER_VERSION + "\n";

		SOCKET sock = INVALID_SOCKET;
		auto ensure_connected = [&]() -> bool
		{
			if (sock != INVALID_SOCKET)
				return true;
			sock = connect_bridge();
			if (sock == INVALID_SOCKET)
				return false;
			if (!send_all(sock, hello))
			{
				closesocket(sock);
				sock = INVALID_SOCKET;
				return false;
			}
			return true;
		};

		for (;;)
		{
			std::string line;
			{
				std::unique_lock<std::mutex> lock(g_queueMutex);
				g_queueCv.wait_for(lock, std::chrono::milliseconds(kIdleWaitMs),
					[] { return !g_queue.empty(); });
				if (!g_queue.empty())
				{
					line = std::move(g_queue.front());
					g_queue.pop_front();
				}
			}

			if (!ensure_connected())
			{
				// Give the line back so a late-starting ZeroIV still gets it;
				// the queue bound keeps this from growing without limit.
				if (!line.empty())
				{
					std::lock_guard<std::mutex> lock(g_queueMutex);
					if (g_queue.size() < kMaxQueuedLines)
						g_queue.push_front(std::move(line));
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
				continue;
			}

			if (line.empty())
				continue;

			if (!send_all(sock, line + "\n"))
			{
				closesocket(sock);
				sock = INVALID_SOCKET;
				std::lock_guard<std::mutex> lock(g_queueMutex);
				if (g_queue.size() < kMaxQueuedLines)
					g_queue.push_front(std::move(line));
				std::this_thread::sleep_for(std::chrono::milliseconds(kReconnectDelayMs));
			}
		}
	}
}

void logbridge::init()
{
	const bool enabled = config::get_value<bool>("debugConsole", "enabled", true);
	std::string host = config::get_value<std::string>("debugConsole", "host", "127.0.0.1");
	int port = config::get_value<int>("debugConsole", "port", kDefaultPort);

	if (host.empty())
		host = "127.0.0.1";
	if (port <= 0 || port > 65535)
		port = kDefaultPort;

	{
		std::lock_guard<std::mutex> lock(g_configMutex);
		g_host = host;
		g_port = port;
	}
	g_enabled.store(enabled);
}

bool logbridge::enabled()
{
	return g_enabled.load();
}

std::string logbridge::target()
{
	std::lock_guard<std::mutex> lock(g_configMutex);
	return g_host + ":" + std::to_string(g_port);
}

void logbridge::set_inside_dll_main(bool inside)
{
	g_insideDllMain.store(inside);
}

void logbridge::ensure_started()
{
	if (!g_enabled.load() || g_insideDllMain.load())
		return;
	if (g_started.exchange(true))
		return;

	try
	{
		std::thread(sender_loop).detach();
	}
	catch (...)
	{
		// Out of thread handles / memory: keep the game alive and let a later
		// log line retry.
		g_started.store(false);
	}
}

void logbridge::enqueue(const char* type, const char* message)
{
	if (!g_enabled.load() || type == nullptr || message == nullptr)
		return;

	try
	{
		std::string module = type;
		if (module.empty() || module == "info")
			module = "loader";

		const char* level = "info";
		if (std::strstr(type, "error") != nullptr || std::strstr(type, "fatal") != nullptr)
			level = "error";
		else if (std::strstr(type, "warn") != nullptr)
			level = "warn";

		const int64_t millis = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();

		std::string line = make_line(level, module, millis, sanitize(message));
		{
			std::lock_guard<std::mutex> lock(g_queueMutex);
			if (g_queue.size() >= kMaxQueuedLines)
				g_queue.pop_front();
			g_queue.push_back(std::move(line));
		}
		g_queueCv.notify_one();
	}
	catch (...)
	{
		// Logging must never take the game down.
		return;
	}

	ensure_started();
}
