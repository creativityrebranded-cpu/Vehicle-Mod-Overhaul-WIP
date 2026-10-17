#include "trace.h"
#include "controlmap.h"
#include "notify.h"

#include <Windows.h>
#include <ShlObj.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace trace
{
	namespace
	{
		std::FILE* g_file = nullptr;
		std::mutex g_lock;
		std::condition_variable g_wake;
		std::string g_pending;
		std::atomic<unsigned long long> g_frame{ 0 };
		std::atomic<unsigned long long> g_burstUntil{ 0 };
		std::atomic<bool> g_verbose{ false };
		ULONGLONG g_start = 0;
		std::mutex g_ringLock;
		std::deque<std::string> g_ring;
		constexpr std::size_t kRingMax = 2400;

		std::string Head(const char* channel)
		{
			char head[96];
			SYSTEMTIME t;
			GetLocalTime(&t);
			std::snprintf(head, sizeof(head), "%02u:%02u:%02u.%03u f%llu t%lu %-8s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
				static_cast<unsigned long long>(g_frame.load()), GetCurrentThreadId(), channel);
			return head;
		}

		void Writer()
		{
			std::string chunk;
			for (;;) {
				{
					std::unique_lock lock(g_lock);
					g_wake.wait_for(lock, std::chrono::milliseconds(100), [] { return g_pending.size() > (1u << 20); });
					chunk.swap(g_pending);
				}
				if (!chunk.empty() && g_file) {
					std::fwrite(chunk.data(), 1, chunk.size(), g_file);
					std::fflush(g_file);
					chunk.clear();
				}
			}
		}

		void Append(const char* channel, const char* text)
		{
			if (!g_file) {
				return;
			}
			char head[96];
			SYSTEMTIME t;
			GetLocalTime(&t);
			std::snprintf(head, sizeof(head), "%02u:%02u:%02u.%03u f%llu t%lu %-8s ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
				static_cast<unsigned long long>(g_frame.load()), GetCurrentThreadId(), channel);
			std::lock_guard lock(g_lock);
			g_pending += head;
			g_pending += text;
			g_pending += '\n';
		}
	}

	void Init()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return;
		}
		std::wstring dir = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessPlanets";
		CoTaskMemFree(docs);
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
		SYSTEMTIME t;
		GetLocalTime(&t);
		wchar_t name[64];
		swprintf_s(name, L"\\trace-%04u%02u%02u-%02u%02u%02u.log", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
		g_file = _wfopen((dir + name).c_str(), L"w");
		g_start = GetTickCount64();
		std::thread(Writer).detach();
		Raw("log", "trace started");
	}

	void Line(const char* channel, const char* fmt, ...)
	{
		if (!g_file) {
			return;
		}
		char text[1536];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		Append(channel, text);
	}

	void Raw(const char* channel, const char* text) { Append(channel, text); }

	void NextFrame() { g_frame.fetch_add(1); }

	unsigned long long Frame() { return g_frame.load(); }

	void Burst(unsigned ms, const char* why)
	{
		const auto until = GetTickCount64() + ms;
		if (until > g_burstUntil.load()) {
			g_burstUntil.store(until);
		}
		Line("mark", "burst %u ms: %s", ms, why);
	}

	bool Detailed() { return g_verbose.load() || GetTickCount64() < g_burstUntil.load(); }

	void Ring(const char* channel, const std::string& text)
	{
		if (!g_file) {
			return;
		}
		std::string line = Head(channel);
		line += text;
		line += '\n';
		if (Detailed()) {
			std::lock_guard lock(g_lock);
			g_pending += line;
			return;
		}
		std::lock_guard lock(g_ringLock);
		g_ring.push_back(std::move(line));
		if (g_ring.size() > kRingMax) {
			g_ring.pop_front();
		}
	}

	void Marker(const char* why)
	{
		if (!g_file) {
			return;
		}
		std::deque<std::string> ring;
		{
			std::lock_guard lock(g_ringLock);
			ring.swap(g_ring);
		}
		{
			std::lock_guard lock(g_lock);
			g_pending += Head("mark");
			g_pending += "---- flight recorder: " + std::string(why) + " (ring below: " + std::to_string(ring.size()) + " lines before this point) ----\n";
			for (const auto& l : ring) {
				g_pending += l;
			}
			g_pending += Head("mark");
			g_pending += "---- end of ring ----\n";
		}
		Burst(20000, why);
	}

	void HotkeyTick()
	{
		// Flight-recorder key: the first of these the game does not bind (read once the control map is available).
		static int markKey = 0;
		static ULONGLONG nextTry = 0;
		static bool markDown = false;
		const ULONGLONG nowMs = GetTickCount64();
		if (!markKey && nowMs >= nextTry && nowMs - g_start > 20000) {
			nextTry = nowMs + 5000;
			markKey = controlmap::FirstFree({ VK_F4, VK_F3, VK_F2, VK_F1 });
			if (markKey) {
				Line("mark", "flight recorder key: %s", controlmap::KeyName(markKey).c_str());
			}
		}
		if (markKey) {
			DWORD fg = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &fg);
			const bool k = fg == GetCurrentProcessId() && (GetAsyncKeyState(markKey) & 0x8000) != 0;
			if (k && !markDown) {
				Marker("hotkey");
				notify::Show("Flight recorder saved");
			}
			markDown = k;
		}
		static bool down = false;
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		const bool now = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
		if (now && !down) {
			g_verbose.store(!g_verbose.load());
			Line("mark", "F9: verbose per-frame tracing %s", g_verbose.load() ? "ON" : "OFF");
		}
		down = now;
	}
}
