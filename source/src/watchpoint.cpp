#include "watchpoint.h"

#include "log.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <cmath>
#include <cstdio>

namespace watchpoint
{
	namespace
	{
		std::atomic<const float*> g_address{ nullptr };
		std::atomic<float> g_tracked{ 0.0f };
		float g_jump = 300.0f;
		std::atomic<int> g_reports{ 0 };
		ULONGLONG g_until = 0;
		bool g_armed = false;
		const char* g_what = "";
		HMODULE g_self = nullptr;

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

		LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
		{
			if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 1) == 0) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			info->ContextRecord->Dr6 = 0;
			const float* a = g_address.load();
			if (!a) {
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			const float now = *a;
			const float before = g_tracked.load();
			g_tracked.store(now);
			if (std::fabs(now - before) <= g_jump) {
				return EXCEPTION_CONTINUE_EXECUTION;  // ordinary movement
			}
			// Our own shift writes it on purpose.
			HMODULE writer = nullptr;
			GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCSTR>(info->ContextRecord->Rip), &writer);
			if (writer == g_self) {
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			if (g_reports.fetch_add(1) >= 4) {
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			char where[160];
			Describe(info->ContextRecord->Rip, where, sizeof(where));
			logger::WriteUnlocked("Watch: %s jumped %.1f -> %.1f, written by %s (thread %lu); rcx=%llX rdx=%llX rbx=%llX rdi=%llX rsi=%llX", g_what, before, now, where,
				GetCurrentThreadId(), info->ContextRecord->Rcx, info->ContextRecord->Rdx, info->ContextRecord->Rbx, info->ContextRecord->Rdi,
				info->ContextRecord->Rsi);
			CONTEXT ctx = *info->ContextRecord;
			for (int frame = 0; frame < 20 && ctx.Rip; ++frame) {
				char fn[160];
				Describe(ctx.Rip, fn, sizeof(fn));
				logger::WriteUnlocked("Watch:   #%02d %s", frame, fn);
				DWORD64 imageBase = 0;
				auto* entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
				if (!entry) {
					ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
					ctx.Rsp += 8;
					continue;
				}
				void* handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher, nullptr);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		struct SetJob
		{
			DWORD64 address;
			bool on;
		};

		// Runs on its own thread: a thread cannot set its own debug registers while running.
		DWORD WINAPI SetAll(LPVOID param)
		{
			const auto job = *static_cast<SetJob*>(param);
			delete static_cast<SetJob*>(param);
			const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) {
				return 0;
			}
			THREADENTRY32 te{ sizeof(te) };
			int set = 0;
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
					continue;
				}
				HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (!t) {
					continue;
				}
				if (SuspendThread(t) != DWORD(-1)) {
					CONTEXT c{};
					c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
					if (GetThreadContext(t, &c)) {
						if (job.on) {
							c.Dr0 = job.address;
							c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18);  // L0, write, 4 bytes
						}
						else {
							c.Dr0 = 0;
							c.Dr7 &= ~0xF0003ull;
						}
						set += SetThreadContext(t, &c) ? 1 : 0;
					}
					ResumeThread(t);
				}
				CloseHandle(t);
			}
			CloseHandle(snap);
			logger::Write("Watch: %s on %d threads", job.on ? "armed" : "disarmed", set);
			return 0;
		}

		void Apply(DWORD64 address, bool on)
		{
			if (HANDLE t = CreateThread(nullptr, 0, SetAll, new SetJob{ address, on }, 0, nullptr)) {
				CloseHandle(t);
			}
		}
	}

	void Install()
	{
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&Install), &g_self);
		AddVectoredExceptionHandler(1, Handler);
	}

	void Arm(const float* address, float jump, unsigned ms, const char* what)
	{
		g_jump = jump;
		g_what = what;
		g_tracked.store(*address);
		g_reports.store(0);
		g_address.store(address);
		g_until = GetTickCount64() + ms;
		g_armed = true;
		Apply(reinterpret_cast<DWORD64>(address), true);
	}

	bool Watching(const float* address) { return g_armed && g_address.load() == address; }

	void ResetReports() { g_reports.store(0); }

	unsigned long ThreadId() { return GetCurrentThreadId(); }

	void Tick()
	{
		if (g_armed && GetTickCount64() >= g_until) {
			g_armed = false;
			Apply(0, false);
			g_address.store(nullptr);
		}
	}
}
