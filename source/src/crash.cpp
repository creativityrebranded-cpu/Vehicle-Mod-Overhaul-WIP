#include "crash.h"

#include "log.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace crash
{
	namespace
	{
		std::atomic<int> g_reported{ 0 };
		thread_local bool t_expected = false;
		HMODULE g_self = nullptr;

		struct CrumbEntry { const char* what; int arg; DWORD thread; ULONGLONG at; };
		constexpr unsigned kCrumbs = 24;
		CrumbEntry g_crumbs[kCrumbs];
		std::atomic<unsigned> g_crumbNext{ 0 };

		// "Starfield.exe+0x1234567" style description of a code address.
		void Describe(DWORD64 address, char* out, size_t size)
		{
			HMODULE module = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(address), &module) && module) {
				char path[MAX_PATH] = {};
				GetModuleFileNameA(module, path, MAX_PATH);
				const char* name = path;
				for (const char* p = path; *p; ++p) {
					if (*p == '\\' || *p == '/') {
						name = p + 1;
					}
				}
				std::snprintf(out, size, "%s+0x%llX", name, static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
			}
			else {
				std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
			}
		}

		bool IsFatal(DWORD code)
		{
			switch (code) {
			case EXCEPTION_ACCESS_VIOLATION:
			case EXCEPTION_ILLEGAL_INSTRUCTION:
			case EXCEPTION_PRIV_INSTRUCTION:
			case EXCEPTION_STACK_OVERFLOW:
			case EXCEPTION_INT_DIVIDE_BY_ZERO:
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			case STATUS_HEAP_CORRUPTION:
				return true;
			default:
				return false;
			}
		}

		thread_local bool t_unhandled = false;  // called from the unhandled filter: a real crash, reported wherever it is

		LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
		{
			const auto* rec = info->ExceptionRecord;
			if (!IsFatal(rec->ExceptionCode) || t_expected) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			// Our own guarded reads (game::SafeRead) fault on purpose inside this DLL: not a crash.
			HMODULE faulting = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &faulting);
			if (faulting == g_self && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			// The same goes for the other plugins of this mod set (each guards its own reads and ignores its own faults above): two of them in one game must not report each
			// other's guarded faults (the 2026-10-18 launch logged two at load and used up the three-report cap).
			{
				char path[MAX_PATH] = {};
				if (faulting) GetModuleFileNameA(faulting, path, MAX_PATH);
				const char* name = path;
				for (const char* p = path; *p; ++p) {
					if (*p == '\\' || *p == '/') name = p + 1;
				}
				if (faulting && rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !t_unhandled &&
					(_strnicmp(name, "Seamless", 8) == 0 || _strnicmp(name, "FlyingVehicleOverhaul", 21) == 0 || _strnicmp(name, "Gravspace", 9) == 0)) return EXCEPTION_CONTINUE_SEARCH;
				// Other mods' DLLs raise and catch their own exceptions as normal work (2026-10-21: CassiopeiaPapyrusExtender's
				// first-chance faults were logged as "CRASH?"). Before anything handles it only the game's own code is watched;
				// a real crash anywhere still reaches the unhandled filter and is reported in full.
				// 2026-10-29 13:06 crash: nothing was logged and Windows recorded nothing, so the fault was outside the game's own
				// code or skipped the unhandled filter. One line each for the first few faults elsewhere (their own cap), so the next
				// such crash names its module.
				if (!t_unhandled && faulting && _stricmp(name, "Starfield.exe") != 0) {
					// Each address once (13:10 run: CassiopeiaPapyrusExtender's handled fault at one address took all five lines).
					static std::atomic<std::uintptr_t> seen[8]{};
					static std::atomic<int> others{ 0 };
					const auto addr = reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress);
					bool known = false;
					for (auto& s : seen) if (s.load() == addr) known = true;
					if (!known && others.load() < 8) { const int k = others.fetch_add(1); if (k < 8) seen[k].store(addr); }
					if (!known && others.load() <= 8) {
						char where[160];
						Describe(reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof(where));
						logger::WriteUnlocked("First-chance fault 0x%08X in another module at %s (thread %lu); often handled there", rec->ExceptionCode, where, GetCurrentThreadId());
					}
					return EXCEPTION_CONTINUE_SEARCH;
				}
			}
			if (g_reported.fetch_add(1) >= 3) {
				return EXCEPTION_CONTINUE_SEARCH;  // only the first few (the game may catch some itself)
			}

			char where[160];
			Describe(reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof(where));
			const auto* c = info->ContextRecord;
			logger::WriteUnlocked(t_unhandled ? "CRASH: exception 0x%08X at %s (thread %lu)"
				: "First-chance fault 0x%08X at %s (thread %lu); the game may handle it, a crash follows only if it does not",
				rec->ExceptionCode, where, GetCurrentThreadId());
			if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
				logger::WriteUnlocked("  %s address 0x%llX", rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write" : "execute",
					static_cast<unsigned long long>(rec->ExceptionInformation[1]));
			}
			logger::WriteUnlocked("  rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX", c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi);
			logger::WriteUnlocked("  r8=%llX r9=%llX r10=%llX r11=%llX r12=%llX r13=%llX r14=%llX r15=%llX rsp=%llX",
				c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15, c->Rsp);

			// What the plugin did lately (newest last), with its age and thread.
			{
				const ULONGLONG now = GetTickCount64();
				const unsigned next = g_crumbNext.load();
				logger::WriteUnlocked("  plugin breadcrumbs (oldest first; now = thread %lu):", GetCurrentThreadId());
				for (unsigned k = 0; k < kCrumbs; ++k) {
					const CrumbEntry& e = g_crumbs[(next + k) % kCrumbs];
					if (!e.what) continue;
					logger::WriteUnlocked("    -%llu ms  thread %lu  %s %d", now - e.at, e.thread, e.what, e.arg);
				}
			}

			// Unwind the faulting thread's stack.
			CONTEXT ctx = *c;
			for (int frame = 0; frame < 24 && ctx.Rip; ++frame) {
				char fn[160];
				Describe(ctx.Rip, fn, sizeof(fn));
				logger::WriteUnlocked("  #%02d %s", frame, fn);
				DWORD64 imageBase = 0;
				auto* entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
				if (!entry) {
					// Leaf function: return address is at rsp.
					ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
					ctx.Rsp += 8;
					continue;
				}
				void* handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher, nullptr);
			}
			return EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void SetExpectedFaults(bool on) { t_expected = on; }

	namespace
	{
		LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

		// The crash itself: nothing handled it (a fault inside this plugin's guarded calls never gets here).
		LONG WINAPI Unhandled(EXCEPTION_POINTERS* info)
		{
			logger::WriteUnlocked("CRASH: unhandled exception (details follow)");
			const bool expected = t_expected;
			t_expected = false;
			g_reported.store(0);
			HMODULE self = g_self;
			g_self = nullptr;  // report it even when it is inside this plugin
			t_unhandled = true;
			Handler(info);
			t_unhandled = false;
			g_self = self;
			t_expected = expected;
			return g_previous ? g_previous(info) : EXCEPTION_CONTINUE_SEARCH;
		}
	}

	void Crumb(const char* what, int arg)
	{
		const unsigned i = g_crumbNext.fetch_add(1) % kCrumbs;
		g_crumbs[i] = { what, arg, GetCurrentThreadId(), GetTickCount64() };
	}

	namespace
	{
		std::atomic<ULONGLONG> g_beatAt{ 0 };
		std::atomic<DWORD> g_beatThread{ 0 };
		bool ModuleRange(HMODULE m, std::uintptr_t& base, std::uintptr_t& end)
		{
			if (!m) return false;
			__try {
				const auto b = reinterpret_cast<std::uintptr_t>(m);
				const auto nt = b + *reinterpret_cast<const std::int32_t*>(b + 0x3C);
				base = b; end = b + *reinterpret_cast<const std::uint32_t*>(nt + 0x50);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// Copied while the thread is stopped: nothing here may allocate or take a lock, and the log is only written after it runs again.
		std::size_t CopyStack(HANDLE thread, std::uintptr_t& rip, std::uintptr_t* out, std::size_t max)
		{
			std::size_t n = 0;
			if (SuspendThread(thread) == DWORD(-1)) return 0;
			alignas(16) CONTEXT c{};
			c.ContextFlags = CONTEXT_CONTROL;
			if (GetThreadContext(thread, &c)) {
				rip = c.Rip;
				__try {
					const auto* p = reinterpret_cast<const std::uintptr_t*>(c.Rsp);
					for (; n < max; ++n) out[n] = p[n];
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			ResumeThread(thread);
			return n;
		}
		DWORD WINAPI FreezeWatch(void*)
		{
			static std::uintptr_t stack[0x1000];
			std::uintptr_t gameBase = 0, gameEnd = 0, ownBase = 0, ownEnd = 0;
			HMODULE own = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&FreezeWatch), &own);
			ModuleRange(GetModuleHandleW(nullptr), gameBase, gameEnd);
			ModuleRange(own, ownBase, ownEnd);
			ULONGLONG toldFor = 0;
			int lines = 0;
			for (;;) {
				Sleep(1000);
				const ULONGLONG beat = g_beatAt.load(std::memory_order_relaxed);
				const DWORD id = g_beatThread.load(std::memory_order_relaxed);
				if (!beat || !id || beat == toldFor || GetTickCount64() - beat < 6000 || lines >= 12) continue;
				toldFor = beat; ++lines;
				HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
				if (!thread) continue;
				std::uintptr_t rip = 0;
				const std::size_t n = CopyStack(thread, rip, stack, 0x1000);
				CloseHandle(thread);
				char text[1024];
				int used = std::snprintf(text, sizeof text, "Freeze recorder: the game thread has not updated for %.0f s; at %s+%llX; stack:", (GetTickCount64() - beat) / 1000.0, rip >= ownBase && rip < ownEnd ? "FVO" : "SF", static_cast<unsigned long long>(rip - (rip >= ownBase && rip < ownEnd ? ownBase : gameBase)));
				int shown = 0;
				for (std::size_t k = 0; k < n && shown < 34 && used > 0 && used < int(sizeof text) - 24; ++k) {
					const auto v = stack[k];
					if (v >= gameBase && v < gameEnd) { used += std::snprintf(text + used, sizeof text - used, " %llX", static_cast<unsigned long long>(v - gameBase + 0x140000000ull)); ++shown; }
					else if (v >= ownBase && v < ownEnd) { used += std::snprintf(text + used, sizeof text - used, " FVO+%llX", static_cast<unsigned long long>(v - ownBase)); ++shown; }
				}
				logger::Write("%s", text);
			}
		}
	}
	void Beat()
	{
		g_beatThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		g_beatAt.store(GetTickCount64(), std::memory_order_relaxed);
	}

	void Install()
	{
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&Install), &g_self);
		if (HANDLE t = CreateThread(nullptr, 0, FreezeWatch, nullptr, 0, nullptr)) CloseHandle(t);
		AddVectoredExceptionHandler(1, Handler);
		g_previous = SetUnhandledExceptionFilter(Unhandled);
		// A normal exit (ExitProcess) runs this; a crash or a killed process does not: the log then tells the two apart.
		std::atexit([] { logger::WriteUnlocked("Process exiting normally (ExitProcess)"); });
		logger::Write("Crash logger installed");
	}
}
