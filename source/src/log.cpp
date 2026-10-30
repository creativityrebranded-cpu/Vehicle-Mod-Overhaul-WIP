#include "log.h"
#include "plugininfo.h"
#include "trace.h"

#include "fvopaths.h"
#include "logbuffer.h"

#include <Windows.h>
#include <ShlObj.h>

#include <cstdarg>
#include <atomic>
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
        char g_buffer[65536]{};
        logbuffer::Policy g_flush;
#ifdef SP_LOG_TEST
        ULONGLONG g_testTime=1000;
#endif
        ULONGLONG Now() {
#ifdef SP_LOG_TEST
            return g_testTime;
#else
            return GetTickCount64();
#endif
        }
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
        if(g_file)std::setvbuf(g_file,g_buffer,_IOFBF,sizeof(g_buffer));
	}

	bool Verbose()
	{
		static std::atomic<bool> value{false};
        static fvopaths::Watch watch;  // re-read only when settings.ini changed (2026-10-29 audit)
        const auto& ini=fvopaths::Settings();
        if(watch.Changed(ini)&&!ini.empty())value.store(GetPrivateProfileIntW(L"log",L"verbose",0,ini.c_str())!=0,std::memory_order_relaxed);
		return value.load(std::memory_order_relaxed);
	}

    void Service() {
        if(!g_file)return;
        std::lock_guard lock(g_lock);
        const auto now=Now();g_flush.Begin(now);
        if(g_flush.Due(now)){std::fflush(g_file);g_flush.Flushed(now);}
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
			const int bytes=std::fprintf(g_file, "[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, text);
            if(bytes>0)g_flush.Added(std::size_t(bytes));
            const auto now=Now();
            if(g_flush.Due(now)){std::fflush(g_file);g_flush.Flushed(now);}
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
#ifdef SP_LOG_TEST
    namespace testing {
        void Open(std::FILE* file) {g_file=file;g_flush={};if(file)std::setvbuf(file,g_buffer,_IOFBF,sizeof(g_buffer));}
        void Clock(ULONGLONG now) {g_testTime=now;}
    }
#endif
}
