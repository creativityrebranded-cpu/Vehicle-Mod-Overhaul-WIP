#include "loadedarea.h"
#include "vehicle_probe.h"
#include "updateschedule.h"
#include "hookcost.h"

#include "addresses.h"
#include "game.h"
#include "crash.h"
#include "hook.h"
#include "log.h"
#include "navmap.h"
#include "prefetch.h"
#include "shipmodel.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <vector>

#pragma comment(lib, "Synchronization.lib")

namespace vehicle_probe
{
	namespace
	{
		constexpr std::uint32_t kGroundLayers = 0x22006;
		struct Probe
		{
			bool pending = false;
			bool valid = false;
			float x = 0, y = 0, z = 0, above = 0, length = 0;
			float height = -3.4e38f;
			std::uint32_t mask = 0;
			unsigned age = 0;
		};
		std::array<Probe, prefetch::kSlotCount> g_probes{};
		std::mutex g_lock;
		// ---- Forward rays ----
		// The query FUN_140583CB0 builds on its stack (1.16.244), laid out as it is there:
		//  filter (hknpBSCustomCollisionFilter, 0x30): +0 vtable, +8 0xFFFFFFFF, +0x10 1, +0x18 (u8) 4, +0x20 layer mask,
		//    +0x28 0x6001E, +0x2C (u8) 0.
		//  query (0x70, right after it): +0 the world's +0x20 object's vfunc +0x18 result, +8 &filter, +0x10 (u16) 0xFFFF,
		//    +0x14 0xFFFFFFFF, +0x18 0, +0x20 (u16) 1, +0x22 (u8) 0xFB, +0x30 from (w 0), +0x40 to - from (w from the
		//    constant at 0x14587A64C), +0x50 1 / that per lane (0 -> the constant at 0x144F065B0), +0x60 0.
		//  collector (hknpAllHitsCollector): +0 vtable, +8 (u8) 0x40, +0xC hit count, +0x10 16 bytes from 0x14587A830,
		//    +0x20 (u8) 0, +0x30 hit array, +0x38 size, +0x3C capacity | 0x80000000 (not owned); hits are 0x70 bytes,
		//    position at +0, query type at +0x68 (1 = ray).
		// CastRay = FUN_1430CBEF0(bsWorld + 0x20, &query, &collector), called without the world lock like the game's own
		// generic FUN_142362760. The world: TES +0xE8 (interior cell) or the exterior cell at the point
		// (FUN_140BBD970(TES +0x188 holder, &cell, pos)); cell +0x48 bit 0 ? FUN_1405105C0(cell +0x50) : *0x1461DF6E8;
		// bsWorld = that +0x50.
		constexpr std::uint32_t kRvaCastRay = 0x030CBEF0, kRvaCellAt = 0x00BBD970, kRvaCellWorld = 0x005105C0, kRvaDefaultWorld = 0x061DF6E8,
			kRvaRelease = 0x00384600, kRvaHitsVtbl = 0x03BC1CF8, kRvaFilterVtbl = 0x04B07680, kRvaDirW = 0x0587A64C, kRvaInvZero = 0x04F065B0,
			kRvaCollectorInit = 0x0587A830;
		struct alignas(16) RayFrame
		{
			std::uint8_t filter[0x30];
			std::uint8_t query[0x70];
			std::uint8_t collector[0x50];
			std::uint8_t hits[0x490];
		};
		struct RayProbe
		{
			bool requested = false;
			float from[3] = {}, to[3] = {};
			std::uint64_t mask = 0;
			bool haveResult = false;
			prefetch::RayHit hit;
			unsigned resultTick = 0;  // g_rayTick when the result arrived (age = g_rayTick - resultTick)
		};
		RayProbe g_rays[prefetch::kRaySlots];
		// Queued slots (2026-10-29 audit): the service used to copy the whole table onto the stack and age every slot on every game update
		// (about 470 KB a call with 256 pilots). Now a request lists its slot once, the service takes only those (in slot order, as
		// before), and a result's age is the service count since it arrived: the same number the old per-slot increment reached.
		std::vector<int> g_rayPending, g_sweepPending;
		unsigned g_rayTick = 0, g_sweepTick = 0;

		// Is `p` inside the game's own image (a plausible vtable or function address)?
		bool InImage(std::uintptr_t p)
		{
			static std::uintptr_t lo = 0, hi = 0;
			if (!hi) {
				lo = game::Base();
				const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(lo);
				const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(lo + dos->e_lfanew);
				hi = lo + nt->OptionalHeader.SizeOfImage;
			}
			return p >= lo && p < hi;
		}

		// A fault in a query is recorded (where, and what it touched) so the next one can be diagnosed from the log, and the
		// query is retried after a back-off instead of being switched off for the session: 5, 10, 20, 40 s, then off.
		struct FaultInfo
		{
			DWORD code = 0;
			std::uintptr_t address = 0, target = 0;
			int access = -1;
		} g_fault;

		int CaptureFault(EXCEPTION_POINTERS* ep)
		{
			g_fault.code = ep->ExceptionRecord->ExceptionCode;
			g_fault.address = reinterpret_cast<std::uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
			g_fault.access = -1;
			g_fault.target = 0;
			if (g_fault.code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
				g_fault.access = int(ep->ExceptionRecord->ExceptionInformation[0]);
				g_fault.target = ep->ExceptionRecord->ExceptionInformation[1];
			}
			return EXCEPTION_EXECUTE_HANDLER;
		}

		constexpr int kMaxFaults = 5;
		struct Backoff
		{
			int faults = 0;
			ULONGLONG retryAt = 0;
			bool Blocked() const { return faults >= kMaxFaults || GetTickCount64() < retryAt; }
			void Fault(const char* what)
			{
				++faults;
				const ULONGLONG wait = 5000ull << std::min(faults - 1, 3);
				retryAt = GetTickCount64() + wait;
				HMODULE m = nullptr;
				char name[MAX_PATH] = "?";
				if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(g_fault.address), &m) && m)
					GetModuleFileNameA(m, name, MAX_PATH);
				const char* leaf = std::strrchr(name, '\\');
				logger::Write("Prefetch: %s faulted (#%d): 0x%08lX at %s+0x%llX (%s 0x%llX); %s", what, faults, g_fault.code, leaf ? leaf + 1 : name,
					static_cast<unsigned long long>(g_fault.address - reinterpret_cast<std::uintptr_t>(m)),
					g_fault.access == 0 ? "read" : g_fault.access == 1 ? "write" : g_fault.access == 8 ? "execute" : "no access info",
					static_cast<unsigned long long>(g_fault.target), faults >= kMaxFaults ? "off until restart" : "trying again after a pause");
			}
		};
		Backoff g_rayBackoff, g_sweepBackoff;

		template <class T>
		T& F(std::uint8_t* p, std::size_t o) { return *reinterpret_cast<T*>(p + o); }

		std::uintptr_t RayWorld(std::uintptr_t tes, const float* at)
		{
			std::uintptr_t cell = *reinterpret_cast<std::uintptr_t*>(tes + 0xE8);
			std::uintptr_t found = 0;
			if (!cell) {
				const auto holder = *reinterpret_cast<std::uintptr_t*>(tes + 0x188);
				if (!holder) return 0;
				reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t*, const float*)>(game::Base() + kRvaCellAt)(holder, &found, at);
				cell = found;
			}
			std::uintptr_t world = 0;
			if (cell) {
				world = (*reinterpret_cast<std::uint8_t*>(cell + 0x48) & 1)
					? reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(game::Base() + kRvaCellWorld)(*reinterpret_cast<std::uintptr_t*>(cell + 0x50))
					: **reinterpret_cast<std::uintptr_t**>(game::Base() + kRvaDefaultWorld);
			}
			if (found) reinterpret_cast<void (*)(std::uintptr_t)>(game::Base() + kRvaRelease)(found);
			return world ? *reinterpret_cast<std::uintptr_t*>(world + 0x50) : 0;
		}

		// (The owner of a hit body was resolved here through FUN_140B3CF40 + FUN_140384600. Removed 2026-10-18: the first ray that ever hit something ran it, and the
		// game crashed three times out of three right after, a deferred engine task calling through a freed object: the reference count was not ours to release.
		// The hit's body id and layer are all the navigation needs.)

		// The physics world's read lock. The game's own CastRay wrapper (FUN_140583CB0) takes it around the query: FUN_1422CC350(bsWorld + 0xB20) to enter
		// (count at +0xB24, writers hold bits 0xFFC00000; the physics step is the writer) and an atomic decrement of +0xB24 to leave, waking a waiter when the
		// old count was 1 with waiters registered. Our direct calls skipped it, so a query could run while the step rewrote the world (crash 13:00:21, a game
		// task jumped through a destroyed object right after the first live forward ray). The lock is taken inside the Now functions once the world is known and
		// released by the guarded wrappers on every path (a fault included), so it can never stay held.
		constexpr std::uint32_t kRvaWorldLock = 0x022CC350;
		std::uintptr_t g_heldWorld = 0;
		void LockWorld(std::uintptr_t bs)
		{
			crash::Crumb("world read lock", 1);
			reinterpret_cast<void (*)(std::uintptr_t)>(game::Base() + kRvaWorldLock)(bs + 0xB20);
			g_heldWorld = bs;
		}
		void UnlockWorld()
		{
			if (!g_heldWorld) return;
			crash::Crumb("world read lock released", 0);
			volatile LONG* count = reinterpret_cast<volatile LONG*>(g_heldWorld + 0xB24);
			g_heldWorld = 0;
			const std::uint32_t old = static_cast<std::uint32_t>(InterlockedExchangeAdd(count, -1));
			if ((old & 0xFFC00FFFu) == 1 && (old & 0x3FF000u) != 0) WakeByAddressSingle(const_cast<LONG*>(count));
		}

		bool CastRayNow(std::uintptr_t tes, const RayProbe& r, prefetch::RayHit& out)
		{
			out = prefetch::RayHit{};
			const std::uintptr_t bs = RayWorld(tes, r.from);
			if (!bs) return false;
			LockWorld(bs);
			RayFrame fr{};
			const auto base = game::Base();
			F<std::uintptr_t>(fr.filter, 0) = base + kRvaFilterVtbl;
			F<std::uint64_t>(fr.filter, 8) = 0xFFFFFFFFull;
			F<std::uint64_t>(fr.filter, 0x10) = 1;
			F<std::uint8_t>(fr.filter, 0x18) = 4;
			F<std::uint64_t>(fr.filter, 0x20) = r.mask;
			F<std::uint32_t>(fr.filter, 0x28) = 0x6001E;
			// The hknpWorld is EMBEDDED in the bsWorld at +0x20: its address is `this` and the qword there is its VTABLE (the game's
			// own FUN_14219AE90: plVar16 = bs + 0x20; (*plVar16 + 0x18) called with plVar16; FUN_140583CB0 the same). The first
			// version loaded *(bs + 0x20) and used that vtable pointer as the object, so the first query called garbage: every
			// forward ray faulted on its first use in all three runs (20:58, 11:38, 11:55) and never returned a single hit.
			const std::uintptr_t inner = bs + 0x20;
			{
				void** vt = *reinterpret_cast<void***>(inner);
				if (!vt || !InImage(reinterpret_cast<std::uintptr_t>(vt)) || !InImage(reinterpret_cast<std::uintptr_t>(vt[3]))) return false;  // never call through a pointer that is not code
			}
			F<std::uint64_t>(fr.query, 0) = reinterpret_cast<std::uint64_t (*)(std::uintptr_t)>((*reinterpret_cast<void***>(inner))[3])(inner);
			F<std::uintptr_t>(fr.query, 8) = reinterpret_cast<std::uintptr_t>(fr.filter);
			F<std::uint16_t>(fr.query, 0x10) = 0xFFFF;
			F<std::uint32_t>(fr.query, 0x14) = 0xFFFFFFFF;
			F<std::uint16_t>(fr.query, 0x20) = 1;
			F<std::uint8_t>(fr.query, 0x22) = 0xFB;
			const float dirW = *reinterpret_cast<const float*>(base + kRvaDirW);
			const float* invZero = reinterpret_cast<const float*>(base + kRvaInvZero);
			const float dir[4] = { r.to[0] - r.from[0], r.to[1] - r.from[1], r.to[2] - r.from[2], dirW };
			for (int i = 0; i < 3; ++i) F<float>(fr.query, 0x30 + 4 * i) = r.from[i];
			for (int i = 0; i < 4; ++i) {
				F<float>(fr.query, 0x40 + 4 * i) = dir[i];
				F<float>(fr.query, 0x50 + 4 * i) = dir[i] == 0.0f ? invZero[i] : 1.0f / dir[i];
			}
			F<std::uintptr_t>(fr.collector, 0) = base + kRvaHitsVtbl;
			F<std::uint8_t>(fr.collector, 8) = 0x40;
			std::memcpy(fr.collector + 0x10, reinterpret_cast<const void*>(base + kRvaCollectorInit), 16);
			F<std::uintptr_t>(fr.collector, 0x30) = reinterpret_cast<std::uintptr_t>(fr.hits);
			F<std::uint32_t>(fr.collector, 0x3C) = 0x8000000A;
			reinterpret_cast<void (*)(std::uintptr_t, void*, void*)>(base + kRvaCastRay)(bs + 0x20, fr.query, fr.collector);
			const std::uint32_t n = std::min<std::uint32_t>(std::max(F<std::uint32_t>(fr.collector, 0xC), F<std::uint32_t>(fr.collector, 0x38)), 10);
			const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			float best = 1e30f;
			for (std::uint32_t i = 0; i < n; ++i) {
				std::uint8_t* h = fr.hits + i * 0x70;
				if (F<std::int32_t>(h, 0x68) != 1) continue;
				if (loadedarea::IsWall(F<std::uint32_t>(h, 0x48))) continue;  // the game's loaded-area wall: FVO vehicles pass it
				const float hx = F<float>(h, 0), hy = F<float>(h, 4), hz = F<float>(h, 8);
				const float d = std::sqrt((hx - r.from[0]) * (hx - r.from[0]) + (hy - r.from[1]) * (hy - r.from[1]) + (hz - r.from[2]) * (hz - r.from[2]));
				if (!(d < best) || d > len + 1.0f) continue;
				best = d;
				out.hit = true;
				out.distance = d;
				out.x = hx; out.y = hy; out.z = hz;
				out.bodyId = F<std::uint32_t>(h, 0x48);
				out.layer = F<std::uint32_t>(h, 0x5C) & 0x7F;
				{
					// Is +0x10 the surface normal? Checked on every hit: a unit vector there says yes (counted in the nav map's log line).
					const float ax = F<float>(h, 0x10), ay = F<float>(h, 0x14), az = F<float>(h, 0x18);
					const float al = std::sqrt(ax * ax + ay * ay + az * az);
					out.normalOk = std::isfinite(al) && al > 0.9f && al < 1.1f;
					out.nx = ax; out.ny = ay; out.nz = az;
				}
			}
			return true;
		}

		// ---- Hull sweeps ----
		// hknpBSWorld::CastShape = FUN_142362840(bsWorld, query, orientation, hitCollector, startCollector), the call the
		// vehicle-exit check FUN_14219AE90 makes (with a capsule). Query (0x80): +0 the world object's vfunc +0x18 result, +8
		// &filter, +0x10 u16 0xFFFF, +0x14 u32 0xFFFFFFFF, +0x20 u16 1, +0x22 u8 0xFB, +0x28 the SHAPE, +0x30 start, +0x40
		// displacement (w as the ray's), +0x50 1 / displacement per lane (0 -> the constant), +0x60 float 0.001 (DAT_144F03E40),
		// +0x6C u32 0x100, +0x78 float FLT_MAX. The shape is the vehicle's own: *(body + 0x78); orientation = the body's
		// rotation (3 rows of 16 bytes at body + 0). Collectors and filter exactly as the ray's.
		constexpr std::uint32_t kRvaCastShape = 0x02362840, kRvaAccuracy = 0x04F03E40;
		struct alignas(16) ShapeFrame
		{
			alignas(16) std::uint8_t filter[0x30];
			alignas(16) std::uint8_t query[0x80];
			alignas(16) float orient[12];
			alignas(16) std::uint8_t collector[2][0x50];  // [0] hits, [1] start-point hits (shapes already overlapping)
			alignas(16) std::uint8_t hits[2][0x490];
		};
		struct SweepReq
		{
			bool requested = false;
			std::uintptr_t instance = 0;
			float dir[2] = {};
			float range = 0;
			std::uint64_t mask = 0;
			bool haveResult = false;
			prefetch::SweepHit hit;
			unsigned resultTick = 0;  // g_sweepTick when the result arrived
		};
		SweepReq g_sweeps[prefetch::kSweepSlots];
		unsigned g_sweepLogged = 0;

		void InitCollector(std::uint8_t* collector, std::uint8_t* hits)
		{
			const auto base = game::Base();
			F<std::uintptr_t>(collector, 0) = base + kRvaHitsVtbl;
			F<std::uint8_t>(collector, 8) = 0x40;
			std::memcpy(collector + 0x10, reinterpret_cast<const void*>(base + kRvaCollectorInit), 16);
			F<std::uintptr_t>(collector, 0x30) = reinterpret_cast<std::uintptr_t>(hits);
			F<std::uint32_t>(collector, 0x3C) = 0x8000000A;
		}

		bool CastShapeNow(std::uintptr_t tes, const SweepReq& r, prefetch::SweepHit& out)
		{
			out = prefetch::SweepHit{};
			if (!r.instance || !(r.range > 1.0f)) return false;
			const auto base = game::Base();
			const std::uintptr_t read = *reinterpret_cast<std::uintptr_t*>(r.instance + 0x28);
			const std::uint32_t ownId = *reinterpret_cast<std::uint32_t*>(r.instance + 0x20);
			// The instance was asked for on another thread and its vehicle may have unloaded since: this memory can already be something else (most runs fault here, 0x1200000012 and
			// -1 in the 2026-10-30 14:00 one). Never call through what is not a pointer to code.
			if (!read || (read & 7)) return false;
			{
				void** vt = *reinterpret_cast<void***>(read);
				if (!vt || !InImage(reinterpret_cast<std::uintptr_t>(vt)) || !InImage(reinterpret_cast<std::uintptr_t>(vt[0x70 / 8]))) return false;
			}
			const auto bodyOf = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>((*reinterpret_cast<void***>(read))[0x70 / 8]);
			const std::uintptr_t unlockedBody = bodyOf(read, ownId);
			if (!unlockedBody) return false;
			const float* bt = reinterpret_cast<const float*>(unlockedBody + 0x30);
			const float start[3] = { bt[0], bt[1], bt[2] };
			const float dl = std::sqrt(r.dir[0] * r.dir[0] + r.dir[1] * r.dir[1]);
			if (!std::isfinite(start[0]) || !std::isfinite(start[1]) || !std::isfinite(start[2]) || !(dl > 1e-4f)) return false;
			const float dx = r.dir[0] / dl, dy = r.dir[1] / dl;
			const std::uintptr_t bs = RayWorld(tes, start);
			if (!bs) return false;
			LockWorld(bs);
			// Body and shape again, now that the world cannot remove them during the cast.
			const std::uintptr_t body = bodyOf(read, ownId);
			if (!body) return false;
			const std::uintptr_t shape = *reinterpret_cast<std::uintptr_t*>(body + 0x78);
			if (!shape) return false;
			// The hknpWorld is EMBEDDED in the bsWorld at +0x20: its address is `this` and the qword there is its VTABLE (the game's
			// own FUN_14219AE90: plVar16 = bs + 0x20; (*plVar16 + 0x18) called with plVar16; FUN_140583CB0 the same). The first
			// version loaded *(bs + 0x20) and used that vtable pointer as the object, so the first query called garbage: every
			// forward ray faulted on its first use in all three runs (20:58, 11:38, 11:55) and never returned a single hit.
			const std::uintptr_t inner = bs + 0x20;
			{
				void** vt = *reinterpret_cast<void***>(inner);
				if (!vt || !InImage(reinterpret_cast<std::uintptr_t>(vt)) || !InImage(reinterpret_cast<std::uintptr_t>(vt[3]))) return false;  // never call through a pointer that is not code
			}
			ShapeFrame fr{};
			F<std::uintptr_t>(fr.filter, 0) = base + kRvaFilterVtbl;
			F<std::uint64_t>(fr.filter, 8) = 0xFFFFFFFFull;
			F<std::uint64_t>(fr.filter, 0x10) = 1;
			F<std::uint8_t>(fr.filter, 0x18) = 4;
			F<std::uint64_t>(fr.filter, 0x20) = r.mask;
			F<std::uint32_t>(fr.filter, 0x28) = 0x6001E;
			F<std::uint64_t>(fr.query, 0) = reinterpret_cast<std::uint64_t (*)(std::uintptr_t)>((*reinterpret_cast<void***>(inner))[3])(inner);
			F<std::uintptr_t>(fr.query, 8) = reinterpret_cast<std::uintptr_t>(fr.filter);
			F<std::uint16_t>(fr.query, 0x10) = 0xFFFF;
			F<std::uint32_t>(fr.query, 0x14) = 0xFFFFFFFF;
			F<std::uint16_t>(fr.query, 0x20) = 1;
			F<std::uint8_t>(fr.query, 0x22) = 0xFB;
			F<std::uintptr_t>(fr.query, 0x28) = shape;
			const float dirW = *reinterpret_cast<const float*>(base + kRvaDirW);
			const float* invZero = reinterpret_cast<const float*>(base + kRvaInvZero);
			const float disp[4] = { dx * r.range, dy * r.range, 0.0f, dirW };
			for (int i = 0; i < 3; ++i) F<float>(fr.query, 0x30 + 4 * i) = start[i];
			for (int i = 0; i < 4; ++i) {
				F<float>(fr.query, 0x40 + 4 * i) = disp[i];
				F<float>(fr.query, 0x50 + 4 * i) = disp[i] == 0.0f ? invZero[i] : 1.0f / disp[i];
			}
			F<float>(fr.query, 0x60) = *reinterpret_cast<const float*>(base + kRvaAccuracy);
			F<std::uint32_t>(fr.query, 0x6C) = 0x100;
			F<float>(fr.query, 0x78) = 3.4028235e38f;
			std::memcpy(fr.orient, reinterpret_cast<const void*>(body), sizeof(fr.orient));
			InitCollector(fr.collector[0], fr.hits[0]);
			InitCollector(fr.collector[1], fr.hits[1]);
			LARGE_INTEGER t0, t1, fq;
			QueryPerformanceFrequency(&fq);
			QueryPerformanceCounter(&t0);
			reinterpret_cast<void (*)(std::uintptr_t, void*, void*, void*, void*)>(base + kRvaCastShape)(bs, fr.query, fr.orient, fr.collector[0], fr.collector[1]);
			QueryPerformanceCounter(&t1);
			out.micros = unsigned(double(t1.QuadPart - t0.QuadPart) * 1e6 / double(fq.QuadPart));
			const std::uint32_t n = std::min<std::uint32_t>(std::max(F<std::uint32_t>(fr.collector[0], 0xC), F<std::uint32_t>(fr.collector[0], 0x38)), 10);
			float best = 1e30f;
			for (std::uint32_t i = 0; i < n; ++i) {
				std::uint8_t* h = fr.hits[0] + i * 0x70;
				if (F<std::int32_t>(h, 0x68) != 2) continue;  // 2 = shape cast
				const float hx = F<float>(h, 0), hy = F<float>(h, 4), hz = F<float>(h, 8);
				const std::uint32_t bodyId = F<std::uint32_t>(h, 0x48), layer = F<std::uint32_t>(h, 0x5C) & 0x7F;
				if (!std::isfinite(hx) || !std::isfinite(hy) || !std::isfinite(hz)) continue;
				if (bodyId == ownId || loadedarea::IsWall(bodyId) || ((layer == 13 || layer == 17) && hz < start[2] + 0.5f)) {  // (terrain at or under the vehicle's origin is the height probes' job; the thin ray sits 1 m above it)
					++out.skipped;
					continue;
				}
				const float d = std::max(0.0f, (hx - start[0]) * dx + (hy - start[1]) * dy);
				if (d > r.range + 1.0f) continue;
				++out.kept;
				if (d < best) {
					best = d;
					out.hit = true;
					out.distance = d;
					out.x = hx; out.y = hy; out.z = hz;
					out.layer = layer;
					out.bodyId = bodyId;
				}
			}
			for (int i = 0; i < 3; ++i) {
				out.from[i] = start[i];
				out.to[i] = start[i] + (i == 0 ? disp[0] : i == 1 ? disp[1] : 0.0f);
			}
			return true;
		}

		int CastShapeGuarded(std::uintptr_t tes, const SweepReq* r, prefetch::SweepHit* out)
		{
			int result = -1;
			crash::Crumb("hull sweep begin", 0);
			__try {
				result = CastShapeNow(tes, *r, *out) ? 1 : 0;
			}
			__except (CaptureFault(GetExceptionInformation())) {
				result = -1;
			}
			UnlockWorld();
			return result;
		}

		void ServiceSweeps(std::uintptr_t tes)
		{
			static thread_local std::vector<std::pair<int, SweepReq>> work;
			work.clear();
			unsigned tick = 0;
			{
				std::lock_guard lk(g_lock);
				tick = ++g_sweepTick;
				std::sort(g_sweepPending.begin(), g_sweepPending.end());
				for (const int i : g_sweepPending) {
					if (!g_sweeps[i].requested) continue;
					work.push_back({ i, g_sweeps[i] });
					g_sweeps[i].requested = false;
				}
				g_sweepPending.clear();
			}
			if (g_sweepBackoff.Blocked() || g_rayBackoff.faults > 0 || !navmap::RaysEnabled()) return;  // (the sweep follows the rays: it starts only once a ray has gone through unharmed)
			for (const auto& [i, w] : work) {
				prefetch::SweepHit hit;
				const int r = CastShapeGuarded(tes, &w, &hit);
				if (r < 0) {
					g_sweepBackoff.Fault("hull sweep");
					return;
				}
				if (r == 0) continue;
				if (g_sweepLogged < 8) {
					++g_sweepLogged;
					if (logger::Verbose()) logger::Write("Prefetch: hull sweep #%u (slot %d, %.0f m): %s%s, %d hit(s) kept, %d left out (own body / ground below), %u us",
						g_sweepLogged, i, w.range, hit.hit ? "nearest " : "clear", hit.hit ? "" : "", hit.kept, hit.skipped, hit.micros);
					if (hit.hit && logger::Verbose()) logger::Write("Prefetch:   hit at %.1f m, layer %u, body %u, (%.1f, %.1f, %.1f)", hit.distance, hit.layer, hit.bodyId, hit.x, hit.y, hit.z);
				}
				std::lock_guard lk(g_lock);
				g_sweeps[i].hit = hit;
				g_sweeps[i].haveResult = true;
				g_sweeps[i].resultTick = tick;
			}
		}

		// One ray, faults caught (no C++ objects here: __try needs a plain frame).
		int CastRayGuarded(std::uintptr_t tes, const RayProbe* r, prefetch::RayHit* out)
		{
			int result = -1;
			crash::Crumb("forward ray begin", 0);
			__try {
				result = CastRayNow(tes, *r, *out) ? 1 : 0;
			}
			__except (CaptureFault(GetExceptionInformation())) {
				result = -1;
			}
			UnlockWorld();
			return result;
		}

		void ServiceRays(std::uintptr_t tes)
		{
			static thread_local std::vector<std::pair<int, RayProbe>> work;
			work.clear();
			unsigned tick = 0;
			{
				std::lock_guard lk(g_lock);
				tick = ++g_rayTick;  // (results age even while the queries are paused)
				std::sort(g_rayPending.begin(), g_rayPending.end());
				for (const int i : g_rayPending) {
					if (!g_rays[i].requested) continue;
					work.push_back({ i, g_rays[i] });
					g_rays[i].requested = false;
				}
				g_rayPending.clear();
			}
			if (g_rayBackoff.Blocked() || !navmap::RaysEnabled()) return;
			for (const auto& [i, w] : work) {
				prefetch::RayHit hit;
				const int r = CastRayGuarded(tes, &w, &hit);
				if (r < 0) {
					g_rayBackoff.Fault("forward ray");
					return;
				}
				if (r == 0) continue;
				for (int j = 0; j < 3; ++j) {
					hit.from[j] = w.from[j];
					hit.to[j] = w.to[j];
				}
				std::lock_guard lk(g_lock);
				g_rays[i].hit = hit;
				g_rays[i].haveResult = true;
				g_rays[i].resultTick = tick;
			}
		}

		// ---- Solid-object box query (cellbake, 2026-10-29) ----
		// hknpBSWorld::QueryAabb = FUN_142362680(bsWorld, &query, &collector) -> hknpWorld::queryAabb FUN_1430CC2B0(bsWorld + 0x20, ...),
		// laid out as the game's own caller FUN_141B19990 builds it: query +0 the world object's vfunc +0x18 result, +8 its vfunc +0x10
		// result, +0x10 (u16) 0xFFFF, +0x14 0, +0x18 0, +0x20 (u16) 1, +0x22 (u8) 0xFB, +0x30 box min, +0x40 box max; collector an
		// hknpAllHitsCollector as the ray's (10 inline hits; Havok grows the array on its heap past that, and the game frees it with
		// FUN_142E90220(*0x14586C920, array, 0x70, capacity & 0x3FFFFFFF) when the capacity's sign bit is clear). Hits: 0x70 bytes,
		// query type +0x68 (5 = AABB), body id +0x48. A body's reference as the game resolves it after the query (outside the lock):
		// FUN_142389B40(bsWorld, body id) -> FUN_140B3CF40(that, 1) -> the reference; the game adds 0x80000200001 to its count word
		// (+8) and releases it with FUN_140384600. (The 2026-10-18 crashes came from releasing without that add.)
		constexpr std::uint32_t kRvaQueryAabb = 0x02362680, kRvaBodyObject = 0x02389B40, kRvaObjectRef = 0x00B3CF40, kRvaHavokFree = 0x02E90220,
			kRvaHavokHeap = 0x0586C920;
		constexpr int kMaxSolids = 4096;
		struct SolidReq { bool requested = false, done = false, ok = false; float mn[3] = {}, mx[3] = {}; std::vector<std::uint32_t> ids; };
		SolidReq g_solidReq;
		Backoff g_solidBackoff;
		struct alignas(16) BoxFrame
		{
			alignas(16) std::uint8_t query[0x60];
			alignas(16) std::uint8_t collector[0x50];
			alignas(16) std::uint8_t hits[0x490];
		};
		BoxFrame g_boxFrame;
		std::uint32_t g_solidIds[kMaxSolids];
		// Under the world lock: the query only. Returns the number of hits; the hit array (maybe Havok's heap) in *array, its capacity word in *cap.
		int QueryBoxNow(std::uintptr_t bs, const float* mn, const float* mx, std::uint8_t** array, std::uint32_t* cap)
		{
			BoxFrame& fr = g_boxFrame;
			const auto base = game::Base();
			const std::uintptr_t inner = bs + 0x20;
			void** vt = *reinterpret_cast<void***>(inner);
			if (!vt || !InImage(reinterpret_cast<std::uintptr_t>(vt)) || !InImage(reinterpret_cast<std::uintptr_t>(vt[2])) || !InImage(reinterpret_cast<std::uintptr_t>(vt[3]))) return -1;
			F<std::uint64_t>(fr.query, 0) = reinterpret_cast<std::uint64_t (*)(std::uintptr_t)>(vt[3])(inner);
			F<std::uint64_t>(fr.query, 8) = reinterpret_cast<std::uint64_t (*)(std::uintptr_t)>(vt[2])(inner);
			F<std::uint16_t>(fr.query, 0x10) = 0xFFFF;
			F<std::uint16_t>(fr.query, 0x20) = 1;
			F<std::uint8_t>(fr.query, 0x22) = 0xFB;
			for (int i = 0; i < 3; ++i) { F<float>(fr.query, 0x30 + 4 * i) = mn[i]; F<float>(fr.query, 0x40 + 4 * i) = mx[i]; }
			F<std::uintptr_t>(fr.collector, 0) = base + kRvaHitsVtbl;
			F<std::uint8_t>(fr.collector, 8) = 0x40;
			std::memcpy(fr.collector + 0x10, reinterpret_cast<const void*>(base + kRvaCollectorInit), 16);
			F<std::uintptr_t>(fr.collector, 0x30) = reinterpret_cast<std::uintptr_t>(fr.hits);
			F<std::uint32_t>(fr.collector, 0x3C) = 0x8000000A;
			reinterpret_cast<void (*)(std::uintptr_t, void*, void*)>(base + kRvaQueryAabb)(bs, fr.query, fr.collector);
			*array = F<std::uint8_t*>(fr.collector, 0x30);
			*cap = F<std::uint32_t>(fr.collector, 0x3C);
			return F<std::int32_t>(fr.collector, 0xC);
		}
		// Outside the lock, as the game does: a hit body's reference form id (its count raised, then released).
		std::uint32_t BodyRefId(std::uintptr_t bs, std::uint32_t body)
		{
			const auto base = game::Base();
			const std::uintptr_t obj = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(base + kRvaBodyObject)(bs, body);
			if (!obj) return 0;
			const std::uintptr_t ref = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, int)>(base + kRvaObjectRef)(obj, 1);
			if (!ref) return 0;
			InterlockedExchangeAdd64(reinterpret_cast<volatile LONG64*>(ref + 8), 0x80000200001LL);
			const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(ref + 0x28);
			reinterpret_cast<void (*)(std::uintptr_t)>(base + kRvaRelease)(ref);
			return id;
		}
		void FreeHits(std::uint8_t* array, std::uint32_t cap)
		{
			if (!array || array == g_boxFrame.hits || static_cast<std::int32_t>(cap) < 0) return;  // the inline buffer (not owned)
			const auto base = game::Base();
			reinterpret_cast<void (*)(std::uintptr_t, void*, int, std::uint32_t)>(base + kRvaHavokFree)(*reinterpret_cast<std::uintptr_t*>(base + kRvaHavokHeap), array, 0x70, cap & 0x3FFFFFFF);
		}
		// Plain data only (a __try function). Returns -1 a fault, 0 no world, else 1 + the number of form ids written to g_solidIds.
		int SolidQueryGuarded(std::uintptr_t tes, const float* mn, const float* mx)
		{
			int n = -1;
			std::uint8_t* array = nullptr;
			std::uint32_t cap = 0;
			std::uintptr_t bs = 0;
			crash::Crumb("solid box query begin", 0);
			__try {
				const float mid[3] = { 0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), 0.5f * (mn[2] + mx[2]) };
				bs = RayWorld(tes, mid);
				if (bs) {
					std::memset(&g_boxFrame, 0, sizeof g_boxFrame);
					LockWorld(bs);
					n = QueryBoxNow(bs, mn, mx, &array, &cap);
				}
				else n = -3;
			}
			__except (CaptureFault(GetExceptionInformation())) { n = -2; }
			UnlockWorld();
			if (n == -2) return -1;
			if (n < 0) return 0;
			int count = 0;
			__try {
				for (int i = 0; i < n && count < kMaxSolids; ++i) {
					std::uint8_t* h = array + std::size_t(i) * 0x70;
					if (F<std::int32_t>(h, 0x68) != 5) continue;
					const std::uint32_t id = BodyRefId(bs, F<std::uint32_t>(h, 0x48));
					if (id) g_solidIds[count++] = id;
				}
			}
			__except (CaptureFault(GetExceptionInformation())) { FreeHits(array, cap); return -1; }
			FreeHits(array, cap);
			return 1 + count;
		}
		void ServiceSolid(std::uintptr_t tes)
		{
			float mn[3], mx[3];
			{
				std::lock_guard lk(g_lock);
				if (!g_solidReq.requested) return;
				g_solidReq.requested = false;
				for (int i = 0; i < 3; ++i) { mn[i] = g_solidReq.mn[i]; mx[i] = g_solidReq.mx[i]; }
			}
			int r = 0;
			if (!g_solidBackoff.Blocked()) {
				r = SolidQueryGuarded(tes, mn, mx);
				if (r < 0) g_solidBackoff.Fault("solid box query");
			}
			std::lock_guard lk(g_lock);
			g_solidReq.ids.clear();
			if (r > 0) g_solidReq.ids.assign(g_solidIds, g_solidIds + (r - 1));
			g_solidReq.ok = r > 0;
			g_solidReq.done = true;
		}

		using Update = void (*)(std::uintptr_t, std::uintptr_t);
		Update g_original = nullptr;
        updateschedule::Pulse g_updates;
        hookcost::Counter g_updateCost;
        // Report and publish even when TES is temporarily absent. Publishing
        // after probe work means consumers see a completed native update.
        struct FinishedUpdate {
            LARGE_INTEGER start{},nativeEnd{};
            ~FinishedUpdate(){LARGE_INTEGER end{};QueryPerformanceCounter(&end);
                g_updateCost.Add(end.QuadPart-nativeEnd.QuadPart,nativeEnd.QuadPart-start.QuadPart);
                g_updates.Post();}
        };

		void Detour(std::uintptr_t system, std::uintptr_t argument)
		{
            LARGE_INTEGER start{},nativeEnd{};QueryPerformanceCounter(&start);
			g_original(system, argument);
            QueryPerformanceCounter(&nativeEnd);FinishedUpdate finished{start,nativeEnd};
			const auto base = game::Base();
			const auto tes = *reinterpret_cast<std::uintptr_t*>(base + addr::kTESPtr);
			if (!tes) return;
			ServiceRays(tes);
			ServiceSweeps(tes);
			ServiceSolid(tes);
			const auto ray = reinterpret_cast<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(base + addr::kGroundRayCast);
			for (int i = 0; i < prefetch::kSlotCount; ++i) {
				Probe work;
				{
					std::lock_guard lk(g_lock);
					work = g_probes[i];
					g_probes[i].pending = false;
					if (g_probes[i].valid && g_probes[i].age < 1000) ++g_probes[i].age;
				}
				if (!work.pending) continue;
				const float pos[3] = { work.x, work.y, work.z };
				const float height = ray(tes, pos, work.mask ? work.mask : kGroundLayers, work.above, work.length);
				{
					std::lock_guard lk(g_lock);
					g_probes[i].height = height;
					g_probes[i].valid = true;
					g_probes[i].age = 0;
				}
			}
			const auto player = *reinterpret_cast<std::uintptr_t*>(base + addr::kPlayerCharacterPtr);
			if (player) shipmodel::OnPlayerUpdate(player);
		}
	}

    std::uint64_t UpdateSerial(){return g_updates.Serial();}
    void ReportUpdateCost()
    {
        static ULONGLONG began=0;const auto now=GetTickCount64();
        if(!began){began=now;return;}if(now-began<10000)return;
        hookcost::Samples samples;if(!g_updateCost.Take(samples))return;began=now;
        if(!samples.count)return;
        LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);const double ms=1000.0/frequency.QuadPart;
        logger::Write("Vehicle game-update cost: %llu updates; FVO probes+ship model %.3f ms avg / %.3f worst; native original %.3f / %.3f; %llu timing samples skipped (elapsed call time)",
            static_cast<unsigned long long>(samples.count),ms*double(samples.hook)/samples.count,ms*samples.maxHook,
            ms*double(samples.native)/samples.count,ms*samples.maxNative,static_cast<unsigned long long>(samples.skipped));
    }

	bool Install()
	{
		static const std::uint8_t prologue[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kShipSystemUpdate, prologue, sizeof(prologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Vehicle: standalone ground-update hook unavailable");
			return false;
		}
		g_original = reinterpret_cast<Update>(original);
		logger::Write("Vehicle: standalone ground probes installed");
		return true;
	}

	void Request(int slot, float x, float y, float z, float above, float length, std::uint32_t mask)
	{
		if (slot < 0 || slot >= prefetch::kSlotCount) return;
		std::lock_guard lk(g_lock);
		auto& p = g_probes[slot];
		p.pending = true;
		p.x = x; p.y = y; p.z = z; p.above = above; p.length = length; p.mask = mask;
	}

	bool Latest(int slot, float& height, unsigned& age)
	{
		if (slot < 0 || slot >= prefetch::kSlotCount) return false;
		std::lock_guard lk(g_lock);
		const auto& p = g_probes[slot];
		height = p.height;
		age = p.age;
		return p.valid;
	}

	void Reset()
	{
		std::lock_guard lk(g_lock);
		g_probes = {};
		for (auto& r : g_rays) r = RayProbe{};
		for (auto& s : g_sweeps) s = SweepReq{};
		g_rayPending.clear(); g_sweepPending.clear();
	}

	void RequestSweep(int slot, std::uintptr_t instance, float dirX, float dirY, float range, std::uint64_t mask)
	{
		if (slot < 0 || slot >= prefetch::kSweepSlots) return;
		std::lock_guard lk(g_lock);
		auto& s = g_sweeps[slot];
		if (!s.requested) g_sweepPending.push_back(slot);
		s.requested = true;
		s.instance = instance;
		s.dir[0] = dirX; s.dir[1] = dirY;
		s.range = range;
		s.mask = mask;
	}

	bool LatestSweep(int slot, prefetch::SweepHit& out, unsigned& age)
	{
		if (slot < 0 || slot >= prefetch::kSweepSlots) return false;
		std::lock_guard lk(g_lock);
		const auto& s = g_sweeps[slot];
		if (!s.haveResult) return false;
		out = s.hit;
		age = g_sweepTick - s.resultTick;
		return true;
	}

	bool RequestSolids(const float mn[3], const float mx[3])
	{
		std::lock_guard lk(g_lock);
		if (g_solidReq.requested) return false;  // one at a time
		g_solidReq.requested = true; g_solidReq.done = false;
		for (int i = 0; i < 3; ++i) { g_solidReq.mn[i] = mn[i]; g_solidReq.mx[i] = mx[i]; }
		return true;
	}
	bool TakeSolids(std::vector<std::uint32_t>& ids, bool& ok)
	{
		std::lock_guard lk(g_lock);
		if (!g_solidReq.done) return false;
		g_solidReq.done = false;
		ids.swap(g_solidReq.ids);
		g_solidReq.ids.clear();
		ok = g_solidReq.ok;
		return true;
	}

	// One ray now, on the calling (main) thread (2026-10-29 late: the route editor on the local map, which pauses the game, so the
	// queued rays above are never serviced and new points kept a guessed height). Same query, same physics world read lock and
	// fault guard as the queued path; refused while that path is backed off after a fault. 1 = answered, 0 = no world, -1 = fault.
	int CastNow(const float from[3], const float to[3], std::uint64_t mask, prefetch::RayHit& out)
	{
		out = prefetch::RayHit{};
		if (g_rayBackoff.Blocked() || !navmap::RaysEnabled()) return 0;
		const auto tes = *reinterpret_cast<std::uintptr_t*>(game::Base() + addr::kTESPtr);
		if (!tes) return 0;
		RayProbe r;
		for (int i = 0; i < 3; ++i) { r.from[i] = from[i]; r.to[i] = to[i]; }
		r.mask = mask;
		const int result = CastRayGuarded(tes, &r, &out);
		if (result < 0) { g_rayBackoff.Fault("editor ray"); return -1; }
		if (result == 1) for (int j = 0; j < 3; ++j) { out.from[j] = from[j]; out.to[j] = to[j]; }
		return result;
	}

	void RequestRay(int slot, const float from[3], const float to[3], std::uint64_t mask)
	{
		if (slot < 0 || slot >= prefetch::kRaySlots) return;
		std::lock_guard lk(g_lock);
		auto& r = g_rays[slot];
		if (!r.requested) g_rayPending.push_back(slot);
		r.requested = true;
		for (int i = 0; i < 3; ++i) { r.from[i] = from[i]; r.to[i] = to[i]; }
		r.mask = mask;
	}

	bool LatestRay(int slot, prefetch::RayHit& out, unsigned& age)
	{
		if (slot < 0 || slot >= prefetch::kRaySlots) return false;
		std::lock_guard lk(g_lock);
		const auto& r = g_rays[slot];
		if (!r.haveResult) return false;
		out = r.hit;
		age = g_rayTick - r.resultTick;
		return true;
	}
}
