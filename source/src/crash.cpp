#include "crash.h"

#include "log.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>

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
			if (g_reported.fetch_add(1) >= 3) {
				return EXCEPTION_CONTINUE_SEARCH;  // only the first few (the game may catch some itself)
			}

			char where[160];
			Describe(reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof(where));
			const auto* c = info->ContextRecord;
			logger::WriteUnlocked("CRASH? exception 0x%08X at %s (thread %lu)", rec->ExceptionCode, where, GetCurrentThreadId());
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
			Handler(info);
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

	void Install()
	{
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&Install), &g_self);
		AddVectoredExceptionHandler(1, Handler);
		g_previous = SetUnhandledExceptionFilter(Unhandled);
		logger::Write("Crash logger installed");
	}
}
