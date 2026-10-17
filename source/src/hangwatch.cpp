#include "hangwatch.h"

#include "log.h"
#include "telemetry.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <cstdio>
#include <cstring>

namespace hangwatch
{
	namespace
	{
		std::atomic<DWORD> g_loadThread{ 0 }, g_frameThread{ 0 };
		std::atomic<ULONGLONG> g_lastFrame{ 0 };
		std::atomic<ULONGLONG> g_shiftStarted{ 0 }, g_shiftStageAt{ 0 };
		std::atomic<DWORD> g_shiftThread{ 0 };
		std::atomic<const char*> g_shiftStage{ "idle" };
		std::atomic<unsigned> g_shiftCells{ 0 }, g_shiftRefs{ 0 };
		HMODULE g_self = nullptr;

		void Describe(DWORD64 address, char* out, size_t size)
		{
			HMODULE module = nullptr;
			if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCSTR>(address), &module) && module) {
				char path[MAX_PATH] = {};
				GetModuleFileNameA(module, path, MAX_PATH);
				const char* name = std::strrchr(path, 92);
				std::snprintf(out, size, "%s+0x%llX", name ? name + 1 : path, static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
			}
			else {
				std::snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
			}
		}

		// Unwinds a suspended thread into `out` (one line); true if any frame is in one of our plugins.
		bool Walk(CONTEXT ctx, char* out, size_t size)
		{
			bool ours = false;
			size_t used = 0;
			out[0] = 0;
			for (int frame = 0; frame < 28 && ctx.Rip && used + 64 < size; ++frame) {
				char fn[128];
				Describe(ctx.Rip, fn, sizeof(fn));
				if (std::strncmp(fn, "Seamless", 8) == 0) {
					ours = true;
				}
				used += std::snprintf(out + used, size - used, "%s%s", frame ? " < " : "", fn);
				DWORD64 imageBase = 0;
				auto* entry = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
				if (!entry) {
					if (IsBadReadPtr(reinterpret_cast<void*>(ctx.Rsp), 8)) {
						break;
					}
					ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
					ctx.Rsp += 8;
					continue;
				}
				void* handlerData = nullptr;
				DWORD64 establisher = 0;
				RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher, nullptr);
			}
			return ours;
		}

		// Dumps the load thread, the last frame-task thread and every thread with one of our plugins on its stack.
		void DumpThreads(const char* why)
		{
			const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
			const DWORD load = g_loadThread.load(), frameT = g_frameThread.load(), shiftT = g_shiftThread.load();
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap == INVALID_HANDLE_VALUE) {
				return;
			}
			logger::Write("Hang: %s; load thread %lu, last frame-task thread %lu (%.1f s ago); stacks follow", why, load, frameT,
				g_lastFrame.load() ? double(GetTickCount64() - g_lastFrame.load()) / 1000.0 : -1.0);
			THREADENTRY32 te{ sizeof(te) };
			unsigned ourThreads = 0;
			for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
				if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
					continue;
				}
				HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
				if (!t) {
					continue;
				}
				static char line[4096];
				bool ours = false;
				if (SuspendThread(t) != DWORD(-1)) {
					CONTEXT c{};
					c.ContextFlags = CONTEXT_FULL;
					if (GetThreadContext(t, &c)) {
						ours = Walk(c, line, sizeof(line));
					}
					else {
						line[0] = 0;
					}
					ResumeThread(t);
				}
				CloseHandle(t);
			const bool key = te.th32ThreadID == load || te.th32ThreadID == frameT || te.th32ThreadID == shiftT;
				if (key || ours) {
					ourThreads += ours;
					logger::Write("Hang:   thread %lu%s%s: %s", te.th32ThreadID, te.th32ThreadID == load ? " [load]" : "",
						te.th32ThreadID == shiftT ? " [shift]" : (te.th32ThreadID == frameT ? " [frame task]" : ""), line);
				}
			}
			CloseHandle(snap);
			logger::Write("Hang: %u thread(s) with a Seamless plugin on the stack", ourThreads);
		}
	}
	void BeginShift()
	{
		const auto now = GetTickCount64();
		g_shiftThread.store(GetCurrentThreadId());
		g_shiftCells.store(0);
		g_shiftRefs.store(0);
		g_shiftStage.store("start");
		g_shiftStageAt.store(now);
		g_shiftStarted.store(now);
		telemetry::Record("shift_stage", "\"stage\":\"start\",\"cells\":0,\"refs\":0");
	}
	void ShiftStage(const char* stage, unsigned cells, unsigned refs)
	{
		g_shiftCells.store(cells);
		g_shiftRefs.store(refs);
		g_shiftStage.store(stage);
		g_shiftStageAt.store(GetTickCount64());
		telemetry::Record("shift_stage", "\"stage\":\"%s\",\"cells\":%u,\"refs\":%u,\"elapsed_ms\":%llu", stage, cells, refs,
			static_cast<unsigned long long>(GetTickCount64() - g_shiftStarted.load()));
	}
	void ShiftProgress(unsigned cells, unsigned refs) { g_shiftCells.store(cells); g_shiftRefs.store(refs); }
	void ShiftRefs(unsigned refs) { g_shiftRefs.store(refs); }
	void EndShift()
	{
		const auto started = g_shiftStarted.exchange(0);
		if (started) {
			telemetry::Record("shift_stage", "\"stage\":\"end\",\"cells\":%u,\"refs\":%u,\"elapsed_ms\":%llu",
				g_shiftCells.load(), g_shiftRefs.load(), static_cast<unsigned long long>(GetTickCount64() - started));
		}
		g_shiftStage.store("idle");
		g_shiftThread.store(0);
	}

	void NoteLoadThread() { g_loadThread.store(GetCurrentThreadId()); }

	void NoteFrame()
	{
		g_frameThread.store(GetCurrentThreadId());
		g_lastFrame.store(GetTickCount64());
	}

	void Check(bool managerLoading)
	{
		static ULONGLONG since = 0;
		static int dumps = 0;
		static int shiftDumps = 0;
		const ULONGLONG now = GetTickCount64();
		const auto shiftStarted = g_shiftStarted.load();
		if (!shiftStarted) shiftDumps = 0;
		else {
			constexpr ULONGLONG shiftAt[3] = { 3000, 10000, 30000 };
			if (shiftDumps < 3 && now - shiftStarted >= shiftAt[shiftDumps]) {
				logger::Write("Hang: shift running %.1f s, stage %s for %.1f s, thread %lu, cells %u, refs %u",
					double(now - shiftStarted) / 1000.0, g_shiftStage.load(), double(now - g_shiftStageAt.load()) / 1000.0,
					g_shiftThread.load(), g_shiftCells.load(), g_shiftRefs.load());
				DumpThreads("world shift has not completed");
				++shiftDumps;
			}
		}
		// Only during a load (0.56.2): the main menu and space also leave the manager outside its idle state, which
		// gave a false report at the menu. A load is armed by the planet manager setup and disarmed once idle.
		if (!managerLoading || !g_loadThread.load()) {
			if (!managerLoading) {
				g_loadThread.store(0);
			}
			since = 0;
			dumps = 0;
			return;
		}
		if (!since) {
			since = now;
			return;
		}
		// 15 s, then 45 s, then 120 s into the stall: three snapshots show whether the threads are stuck in one place.
		const ULONGLONG at[3] = { 15000, 45000, 120000 };
		if (dumps < 3 && now - since >= at[dumps]) {
			char why[96];
			std::snprintf(why, sizeof(why), "planet manager still loading after %.0f s", double(now - since) / 1000.0);
			DumpThreads(why);
			++dumps;
		}
	}
}
