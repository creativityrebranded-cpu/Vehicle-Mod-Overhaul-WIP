#include "crash.h"

#include "fvopaths.h"
#include "log.h"
#include "stallcapture.h"

#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")

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
		struct ActionEntry { char what[28]; unsigned id; DWORD thread; ULONGLONG at; };
		constexpr unsigned kActions = 48;
		ActionEntry g_actions[kActions];
		std::atomic<unsigned> g_actionNext{ 0 };
		void LogActions()
		{
			const ULONGLONG now = GetTickCount64();
			const unsigned next = g_actionNext.load();
			logger::WriteUnlocked("  last FVO engine commands (oldest first):");
			for (unsigned k = 0; k < kActions; ++k) {
				const ActionEntry& e = g_actions[(next + k) % kActions];
				if (!e.at) continue;
				logger::WriteUnlocked("    -%llu ms  thread %lu  %.27s %08X", now - e.at, e.thread, e.what, e.id);
			}
		}

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

		// The crash dump (2026-10-30 14:00 crash: a bad object in the game's own cache, nothing of ours on the stack, and the log cannot show what the other threads were doing).
		// Written by its own thread, ready before any crash: the faulting thread may hold the heap's lock, and the writer allocates. One file, overwritten, so the last fault is what stays.
		decltype(&MiniDumpWriteDump) g_writeDump = nullptr;
		HANDLE g_dumpAsk = nullptr, g_dumpDone = nullptr;
		MINIDUMP_EXCEPTION_INFORMATION g_dumpInfo{};
		BOOL g_dumpWritten = FALSE;
		std::wstring g_dumpPath;
		std::wstring g_allocatorDumpPath;
		std::uintptr_t g_gameBase = 0;
		std::atomic<bool> g_allocatorCaptured{false};
		bool g_dumpAllocator = false;
		EXCEPTION_RECORD g_savedException{};
		CONTEXT g_savedContext{};
		EXCEPTION_POINTERS g_savedPointers{&g_savedException,&g_savedContext};
		std::wstring g_hangPath;
		std::atomic<bool> g_gameplay{ false };
		// How the run ended, kept in a small file because a kill or a hard crash writes nothing at the time. The next launch reports it (system=run).
		std::wstring g_runPath;
		std::atomic<const char*> g_runState{ "running" };
		std::atomic<std::uintptr_t> g_runFault{ 0 };
		char g_runStarted[16] = "?";
		void WriteRunState();
		std::atomic_flag g_dumpBusy = ATOMIC_FLAG_INIT; // DbgHelp calls must never overlap.
		ULONGLONG g_hangDelay = 45000;

		bool WriteHangDump(const std::wstring& path, DWORD& error)
		{
			if (!g_writeDump) { error = ERROR_PROC_NOT_FOUND; return false; }
			if (g_dumpBusy.test_and_set()) { error = ERROR_BUSY; return false; }
			const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			BOOL ok = FALSE;
			error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
			if (file != INVALID_HANDLE_VALUE) {
				const auto kind = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithProcessThreadData | MiniDumpWithUnloadedModules | MiniDumpWithDataSegs);
				ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, kind, nullptr, nullptr, nullptr);
				if (!ok) error = GetLastError();
				CloseHandle(file);
			}
			g_dumpBusy.clear();
			return ok != FALSE;
		}

		// Thread stacks and modules only, a few MB: fast enough to take during a hitch, where a full dump would make it longer.
		bool WriteStallDump(const std::wstring& path, DWORD& error)
		{
			if (!g_writeDump) { error = ERROR_PROC_NOT_FOUND; return false; }
			if (g_dumpBusy.test_and_set()) { error = ERROR_BUSY; return false; }
			const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			BOOL ok = FALSE;
			error = file == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
			if (file != INVALID_HANDLE_VALUE) {
				ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo), nullptr, nullptr, nullptr);
				if (!ok) error = GetLastError();
				CloseHandle(file);
			}
			g_dumpBusy.clear();
			return ok != FALSE;
		}

		DWORD WINAPI DumpThread(void*)
		{
			for (;;) {
				if (WaitForSingleObject(g_dumpAsk, INFINITE) != WAIT_OBJECT_0) return 0;
				g_dumpWritten = FALSE;
				const auto& path = g_dumpAllocator ? g_allocatorDumpPath : g_dumpPath;
				const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (file != INVALID_HANDLE_VALUE) {
					const auto kind = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
					g_dumpWritten = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, kind, &g_dumpInfo, nullptr, nullptr);
					CloseHandle(file);
				}
				g_dumpBusy.clear();
				SetEvent(g_dumpDone);
			}
		}

		bool WriteDump(EXCEPTION_POINTERS* info, bool allocator = false)
		{
			if (!g_writeDump || !g_dumpAsk || !g_dumpDone) return false;
			if (g_dumpBusy.test_and_set()) {
				logger::WriteUnlocked("  crash dump: skipped (another dump is being written)");
				return false;
			}
			// Keep the context alive even if a writer outlasts the exception handler.
			g_savedException = *info->ExceptionRecord;
			g_savedException.ExceptionRecord = nullptr;
			g_savedContext = *info->ContextRecord;
			g_dumpAllocator = allocator;
			g_dumpInfo.ThreadId = GetCurrentThreadId();
			g_dumpInfo.ExceptionPointers = &g_savedPointers;
			g_dumpInfo.ClientPointers = FALSE;
			ResetEvent(g_dumpDone);
			SetEvent(g_dumpAsk);
			const bool done = WaitForSingleObject(g_dumpDone, 120000) == WAIT_OBJECT_0;
			logger::WriteUnlocked("  %s dump: %s", allocator ? "allocator first-fault" : "crash", done && g_dumpWritten ? allocator ? "written (last-allocator.dmp next to settings.ini)" : "written (last-crash.dmp next to settings.ini)" : "could not be written");
			return done && g_dumpWritten;
		}
		bool AllocatorFault(const EXCEPTION_RECORD& record) {
			const auto address = reinterpret_cast<std::uintptr_t>(record.ExceptionAddress);
			return g_gameBase && record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
				address >= g_gameBase + 0x022D2000 && address < g_gameBase + 0x022D3800;
		}
		bool CaptureAllocatorFault(EXCEPTION_POINTERS* info) {
			if(t_expected || t_unhandled || !AllocatorFault(*info->ExceptionRecord))return false;
			bool expected=false;
			if(!g_allocatorCaptured.compare_exchange_strong(expected,true))return false;
			const bool ok=WriteDump(info,true);
			if(!ok)g_allocatorCaptured=false;
			return ok;
		}

		LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
		{
			const auto* rec = info->ExceptionRecord;
			if (!IsFatal(rec->ExceptionCode) || t_expected) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			// These allocator faults have repeatedly terminated the game without
			// reaching its unhandled filter. Capture exactly one before continuing
			// exception dispatch; ordinary handled reads still make no dump.
			CaptureAllocatorFault(info);
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
			g_runFault.store(reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) - g_gameBase);
			if (t_unhandled) g_runState.store("crashed");
			WriteRunState();
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

			LogActions();
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
			// The dump only for a real crash (nothing handled it). Written at a first-chance fault it froze the game for the length of a
			// 278 MB write, even when FVO's own guard around the call caught the fault (2026-10-30 18:43).
			if (t_unhandled) WriteDump(info);
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
			g_runState.store("crashed");
			WriteRunState();
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
	void Action(const char* what, unsigned id)
	{
		ActionEntry& e = g_actions[g_actionNext.fetch_add(1) % kActions];
		std::strncpy(e.what, what ? what : "?", sizeof e.what - 1);
		e.what[sizeof e.what - 1] = 0;
		e.id = id; e.thread = GetCurrentThreadId(); e.at = GetTickCount64();
	}

	namespace
	{
		std::atomic<ULONGLONG> g_beatAt{ 0 };
		std::atomic<ULONGLONG> g_recoveredBeat{0},g_recoveredGap{0};
		std::atomic<DWORD> g_beatThread{ 0 };
		void WriteRunState()
		{
			if (g_runPath.empty()) return;
			SYSTEMTIME st{}; GetLocalTime(&st);
			const ULONGLONG beat = g_beatAt.load(std::memory_order_relaxed);
			char text[192];
			const int n = std::snprintf(text, sizeof text, "state=%s pid=%lu started=%s last_alive=%02u:%02u:%02u last_update_age_s=%llu last_fault_rva=%llX\n", g_runState.load(), GetCurrentProcessId(), g_runStarted,
				st.wHour, st.wMinute, st.wSecond, static_cast<unsigned long long>(beat ? (GetTickCount64() - beat) / 1000 : 0), static_cast<unsigned long long>(g_runFault.load()));
			const HANDLE f = CreateFileW(g_runPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) return;
			DWORD wrote = 0;
			if (n > 0) WriteFile(f, text, DWORD(n), &wrote, nullptr);
			CloseHandle(f);
		}
		// state=running left behind means nothing got to say otherwise: killed, frozen and closed, or a crash too hard for the handlers.
		void ReportPreviousRun()
		{
			char text[192] = {};
			const HANDLE f = CreateFileW(g_runPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) { logger::Write("OBS system=run event=previous_run result=no_record"); return; }
			DWORD got = 0;
			ReadFile(f, text, sizeof text - 1, &got, nullptr);
			CloseHandle(f);
			if (char* end = std::strchr(text, '\n')) *end = 0;
			const char* result = std::strstr(text, "state=clean") ? "clean_exit" : std::strstr(text, "state=crashed") ? "crashed" : "killed_or_frozen";
			logger::Write("OBS system=run event=previous_run result=%s %s", result, text);
		}
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
		// Who holds what, read live when a hang is seen: the dump does not include the heap the locks live in (2026-10-31: 3 dumps, no holder).
		// Spinners sit in FUN_142404230 with the lock in rdi (+0x208 state, +0x230 owner); waiters in FUN_1422C8D10 have the lock's address on their stack, its first dword the owner.
		constexpr std::uintptr_t kRvaLockSpin = 0x02404230, kLockSpinSize = 0xF8, kRvaLockWaitReturn = 0x022C8DBA;
		bool ReadU32(std::uintptr_t at, unsigned& out)
		{
			__try { out = *reinterpret_cast<const volatile unsigned*>(at); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void LogLocks(std::uintptr_t gameBase)
		{
			static DWORD ids[1024];
			unsigned idCount = 0;
			const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
			if (const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); snap != INVALID_HANDLE_VALUE) {
				THREADENTRY32 e{}; e.dwSize = sizeof e;
				if (Thread32First(snap, &e)) do {
					if (e.th32OwnerProcessID == pid && e.th32ThreadID != self && idCount < 1024) ids[idCount++] = e.th32ThreadID;
				} while (Thread32Next(snap, &e));
				CloseHandle(snap);
			}
			struct Found { DWORD tid; std::uintptr_t lock; unsigned state, readers, waiting, owner; bool spin; };
			static Found found[48];
			static std::uintptr_t words[0x100];
			unsigned count = 0;
			for (int pass = 0; pass < 3; ++pass) {
				for (unsigned t = 0; t < idCount && count < 48; ++t) {
					const HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, ids[t]);
					if (!thread) continue;
					std::size_t n = 0;
					alignas(16) CONTEXT c{};
					c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
					if (SuspendThread(thread) != DWORD(-1)) {
						if (GetThreadContext(thread, &c)) {
							__try {
								const auto* p = reinterpret_cast<const std::uintptr_t*>(c.Rsp);
								for (; n < 0x100; ++n) words[n] = p[n];
							}
							__except (EXCEPTION_EXECUTE_HANDLER) {}
						}
						ResumeThread(thread);
					}
					CloseHandle(thread);
					if (c.Rip >= gameBase + kRvaLockSpin && c.Rip < gameBase + kRvaLockSpin + kLockSpinSize) {
						Found f{ ids[t], c.Rdi, 0, 0, 0, 0, true };
						ReadU32(c.Rdi + 0x208, f.state); ReadU32(c.Rdi + 0x210, f.readers); ReadU32(c.Rdi + 0x214, f.waiting); ReadU32(c.Rdi + 0x230, f.owner);
						found[count++] = f;
						continue;
					}
					for (std::size_t i = 0; i < n && count < 48; ++i) {
						if (words[i] != gameBase + kRvaLockWaitReturn) continue;
						// The candidates sit in this frame and the one below it. Only a value whose first dword is a live thread id is reported.
						for (std::size_t k = i > 16 ? i - 16 : 0; k < n && k < i + 16 && count < 48; ++k) {
							unsigned owner = 0;
							if (words[k] < 0x10000 || !ReadU32(words[k], owner) || !owner) continue;
							if (std::find(ids, ids + idCount, DWORD(owner)) == ids + idCount) continue;
							unsigned state = 0; ReadU32(words[k] + 4, state);
							found[count++] = Found{ ids[t], words[k], state, 0, 0, owner, false };
							break;
						}
						break;
					}
				}
				Sleep(100);
			}
			logger::WriteUnlocked("OBS system=freeze.locks event=scan threads=%u found=%u", idCount, count);
			for (unsigned i = 0; i < count; ++i)
				logger::WriteUnlocked("OBS system=freeze.locks event=%s thread=%lu lock=%llX state=%X readers=%X waiting=%X owner=%u", found[i].spin ? "spinning" : "waiting", found[i].tid, static_cast<unsigned long long>(found[i].lock), found[i].state, found[i].readers, found[i].waiting, found[i].owner);
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
			bool hangDumped = false;
			unsigned dumpAttempts = 0;
			stallcapture::Policy stall;
			ULONGLONG watchAt=0,healthAt=0,recoveryReported=0;
			stallcapture::Gate previousGate=stallcapture::Gate::NoBeat;
			ULONGLONG dumpTriedAt = 0, runWrittenAt = 0;
			for (;;) {
				Sleep(1000);
				const ULONGLONG beat = g_beatAt.load(std::memory_order_relaxed);
				const DWORD id = g_beatThread.load(std::memory_order_relaxed);
				// Capture once per run after a sustained stall; long loads may also trigger it.
				const ULONGLONG now = GetTickCount64();
				DWORD foregroundOwner=0;if(const HWND front=GetForegroundWindow())GetWindowThreadProcessId(front,&foregroundOwner);
				const bool gameplay=g_gameplay.load(std::memory_order_relaxed),foreground=foregroundOwner==GetCurrentProcessId();
				const auto gate=stall.Check(now,beat,gameplay,foreground);
				// A hitch in play ends before the hang dump's delay (2026-11-01: 6.1 s, twice, with nothing in any log). Up to 3 a run, one per hitch; not when another window is in front, which also stops updates.
				if (gate==stallcapture::Gate::Capture) {
						const unsigned index=stall.successes+1;
						const auto path = fvopaths::File(index == 1 ? L"last-stall-1.dmp" : index == 2 ? L"last-stall-2.dmp" : L"last-stall-3.dmp");
						DWORD error = 0; const ULONGLONG at = GetTickCount64();
						const bool written = WriteStallDump(path, error);
						stall.Result(now,beat,written);
						logger::WriteUnlocked("Freeze recorder: stall %llu ms into a hitch in play: stack dump %u of 3 %s in %llu ms (last-stall-%u.dmp); error %lu; attempt %u successes %u", now - beat, index, written ? "written" : "FAILED", GetTickCount64() - at, index, error,stall.attempts,stall.successes);
						if(written)LogLocks(gameBase);
				}
				const auto recovered=g_recoveredBeat.load(std::memory_order_acquire);
				if(recovered&&recovered!=recoveryReported){recoveryReported=recovered;
					logger::WriteUnlocked("OBS system=freeze.stall event=recovered beat=%llu gap_ms=%llu captured=%d previous_gate=%s gameplay_now=%d foreground_now=%d",recovered,g_recoveredGap.load(),stall.capturedBeat==recovered,stallcapture::Name(previousGate),gameplay,foreground);}
				if(now-healthAt>=5000||gate!=previousGate){healthAt=now;
					logger::WriteUnlocked("OBS system=freeze.stall event=watch gate=%s heartbeat=%llu heartbeat_age_ms=%llu heartbeat_thread=%lu gameplay=%d foreground=%d foreground_pid=%lu writer=%d successes=%u attempts=%u monitor_gap_ms=%llu",stallcapture::Name(gate),beat,beat&&now>=beat?now-beat:0,id,gameplay,foreground,foregroundOwner,g_writeDump!=nullptr,stall.successes,stall.attempts,watchAt?now-watchAt:0);}
				previousGate=gate;watchAt=now;
				// Cloud-folder state writes cannot postpone the three-second capture check.
				if (now - runWrittenAt >= 5000) { runWrittenAt = now; WriteRunState(); }
				if (beat && !hangDumped && dumpAttempts < 3 && now - beat >= g_hangDelay && (!dumpTriedAt || now - dumpTriedAt >= 10000)) {
					dumpTriedAt = now; ++dumpAttempts;
					if (dumpAttempts == 1) { LogLocks(gameBase); LogActions(); }
					DWORD error = 0;
					hangDumped = WriteHangDump(g_hangPath, error);
					logger::WriteUnlocked("Freeze recorder: all-thread hang dump %s (last-hang.dmp next to settings.ini); error %lu, attempt %u/3", hangDumped ? "written" : "FAILED", error, dumpAttempts);
				}
				if (!beat || !id || beat == toldFor || GetTickCount64() - beat < 6000 || lines >= 12) continue;
				toldFor = beat; ++lines;
				HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
				if (!thread) continue;
				std::uintptr_t rip = 0;
				const std::size_t n = CopyStack(thread, rip, stack, 0x1000);
				CloseHandle(thread);
				char text[1024];
				char location[256]; Describe(rip, location, sizeof location);
				int used = std::snprintf(text, sizeof text, "Freeze recorder: the game thread has not updated for %.0f s; the last update ran on thread %lu, now at %s (a worker, not necessarily the stuck thread; the window in the background also stops updates); stack candidates:", (GetTickCount64() - beat) / 1000.0, id, location);
				int shown = 0;
				for (std::size_t k = 0; k < n && shown < 34 && used > 0 && used < int(sizeof text) - 24; ++k) {
					const auto v = stack[k];
					if (v >= gameBase && v < gameEnd) { used += std::snprintf(text + used, sizeof text - used, " %llX", static_cast<unsigned long long>(v - gameBase + 0x140000000ull)); ++shown; }
					else if (v >= ownBase && v < ownEnd) { used += std::snprintf(text + used, sizeof text - used, " FVO+%llX", static_cast<unsigned long long>(v - ownBase)); ++shown; }
				}
				// Never wait for the logger's gameplay mutex before reaching the dump threshold.
				logger::WriteUnlocked("%s", text);
			}
		}
	}
	void SetGameplay(bool on) { g_gameplay.store(on, std::memory_order_relaxed); }

	void Beat()
	{
		g_beatThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
		const auto now=GetTickCount64(),previous=g_beatAt.exchange(now,std::memory_order_relaxed);
		if(previous&&now>=previous&&now-previous>=3000){g_recoveredGap=now-previous;g_recoveredBeat.store(previous,std::memory_order_release);}
	}

	void Install()
	{
		g_gameBase = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
		GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCSTR>(&Install), &g_self);
#ifndef FVO_RELEASE
		g_hangPath = fvopaths::File(L"last-hang.dmp");
		g_runPath = fvopaths::File(L"run-state.txt");
		ReportPreviousRun();
#endif
		{ SYSTEMTIME st{}; GetLocalTime(&st); std::snprintf(g_runStarted, sizeof g_runStarted, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond); }
		WriteRunState();
		const unsigned seconds = GetPrivateProfileIntW(L"diagnostics", L"hangDumpSeconds", 45, fvopaths::Settings().c_str());
		g_hangDelay = ULONGLONG(seconds < 6 ? 6 : seconds > 600 ? 600 : seconds) * 1000;
		if (const HMODULE help = LoadLibraryW(L"dbghelp.dll")) {
			g_dumpPath = fvopaths::File(L"last-crash.dmp");
			g_allocatorDumpPath = fvopaths::File(L"last-allocator.dmp");
			g_writeDump = reinterpret_cast<decltype(&MiniDumpWriteDump)>(GetProcAddress(help, "MiniDumpWriteDump"));
			g_dumpAsk = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			g_dumpDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
			if (g_writeDump && g_dumpAsk && g_dumpDone) { if (HANDLE t = CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr)) CloseHandle(t); else g_writeDump = nullptr; }
		}
#ifndef FVO_RELEASE  // release DLLs: only a real crash is reported (the unhandled filter below)
		// Publish paths, configuration and DbgHelp before starting the watcher.
		if (HANDLE t = CreateThread(nullptr, 0, FreezeWatch, nullptr, 0, nullptr)) CloseHandle(t);
		logger::Write("Freeze recorder: all-thread dump %s after %llu s without an update (last-hang.dmp)", g_writeDump ? "armed" : "UNAVAILABLE", g_hangDelay / 1000);
		AddVectoredExceptionHandler(1, Handler);
#endif
		g_previous = SetUnhandledExceptionFilter(Unhandled);
		// A normal exit (ExitProcess) runs this; a crash or a killed process does not: the log then tells the two apart.
		std::atexit([] { logger::WriteUnlocked("Process exiting normally (ExitProcess)"); g_runState.store("clean"); WriteRunState(); });
		logger::Write("Crash logger installed");
		logger::Write("Allocator recorder: first AV in SF+22D2000..22D37FF writes last-allocator.dmp; once per process");
	}
}
