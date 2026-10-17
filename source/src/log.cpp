#include "log.h"
#include "plugininfo.h"
#include "trace.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace logger
{
	namespace
	{
		std::FILE* g_file = nullptr;
		std::mutex g_lock;
	}

	void Init()
	{
		PWSTR docs = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
			return;
		}
		std::wstring dir = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs";
		CoTaskMemFree(docs);
		SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
		// Keep the previous session (it may hold a crash report) before starting a new log.
		const std::wstring path = dir + L"\\" + plugininfo::Name() + L".log";
		MoveFileExW(path.c_str(), (dir + L"\\" + plugininfo::Name() + L".prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
		g_file = _wfopen(path.c_str(), L"w");
	}

	void Write(const char* fmt, ...)
	{
		if (!g_file) {
			return;
		}
		SYSTEMTIME t;
		GetLocalTime(&t);
		char text[2048];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(text, sizeof(text), fmt, args);
		va_end(args);
		{
			std::lock_guard lock(g_lock);
			std::fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
			std::fflush(g_file);
		}
		trace::Raw("log", text);
	}

	void WriteUnlocked(const char* fmt, ...)
	{
		if (!g_file) {
			return;
		}
		va_list args;
		va_start(args, fmt);
		std::vfprintf(g_file, fmt, args);
		va_end(args);
		std::fputc('\n', g_file);
		std::fflush(g_file);
	}
}
