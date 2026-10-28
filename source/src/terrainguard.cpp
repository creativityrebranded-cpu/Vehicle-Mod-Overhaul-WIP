#include "terrainguard.h"

#include "addresses.h"
#include "crash.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "telemetry.h"

#include <Windows.h>

#include <intrin.h>

#include <atomic>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace terrainguard
{
	namespace
	{
		// FUN_14098e8d0(page, pageCoords, neighbour, neighbourCoords): copies the shared edge rows of terrain height /
		// data buffers between a terrain page and its neighbour (BGSTerrain PageStitchNode jobs, worker threads).
		// When a world shift lands while stitch jobs are queued, a neighbour's buffers are already gone and the copy
		// reads null + 0x20 (crashes at +0x98EB3D in 0.34.2, 0.35.0, 0.35.1). The copy is plain data with no locks, so
		// a fault inside it is caught here and that one stitch is skipped (at worst a seam until the page rebuilds).
		using Stitch_t = void (*)(char*, std::intptr_t*, std::intptr_t, std::intptr_t*);
		Stitch_t g_original = nullptr;
		std::atomic<unsigned> g_skipped{ 0 };
		std::atomic<unsigned long long> g_lastActivity{ 0 };

		// FUN_14098a330 builds terrain blocks from three pages (page, coords) and calls the stitch. It reads each page's
		// buffer list (mode 1: +0x08, mode 2: +0x10, mode 3: +0x18, mode = first byte, as the stitch does) and crashed
		// on an emptied list after a shift (+0x98ACDB, 0.35.2). It uses the thread's scratch allocator, so a fault cannot
		// be caught safely: the pages are checked before the call instead and the block is skipped if one is empty.
		using Generate_t = void (*)(char*, std::intptr_t*, char*, std::intptr_t*, char*, std::intptr_t*, int*, char, char);
		Generate_t g_generate = nullptr;
		std::atomic<unsigned> g_blocksSkipped{ 0 };
		std::atomic<std::uint64_t> g_blockCalls{ 0 }, g_blockSampleUs{ 0 };
		std::atomic<unsigned> g_blockSamples{ 0 };

		bool PageReady(const char* page)
		{
			if (!page) {
				return true;  // not every call uses all three
			}
			__try {
				const auto mode = static_cast<unsigned char>(page[0]);
				if (mode < 1 || mode > 3) {
					return true;
				}
				const auto list = *reinterpret_cast<std::uintptr_t* const*>(page + 8 * mode);
				return list && *list;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Set when GenerateDetour skips a block; read by EnsureDetour around the build of one block.
		thread_local bool t_blockSkipped = false;

		void GenerateDetour(char* p1, std::intptr_t* c1, char* p2, std::intptr_t* c2, char* p3, std::intptr_t* c3, int* out, char a, char b)
		{
			const auto call = g_blockCalls.fetch_add(1) + 1;
			g_lastActivity.store(GetTickCount64());
			crash::SetExpectedFaults(true);
			const bool ready = PageReady(p1) && PageReady(p2) && PageReady(p3);
			crash::SetExpectedFaults(false);
			if (!ready) {
				const auto n = g_blocksSkipped.fetch_add(1) + 1;
				telemetry::Record("terrain_block_skip", "\"count\":%u,\"x\":%lld,\"y\":%lld", n,
					c1 ? static_cast<long long>(c1[0]) : 0ll, c1 ? static_cast<long long>(c1[1]) : 0ll);
				if (n <= 5 || n % 100 == 0) {
					logger::Write("Terrain guard: skipped a terrain block whose page buffers were gone (%u so far)", n);
				}
				t_blockSkipped = true;
				return;
			}
			const bool sample = (call & 63u) == 0;
			const auto startUs = sample ? telemetry::NowUs() : 0;
			g_generate(p1, c1, p2, c2, p3, c3, out, a, b);
			if (sample) {
				const auto elapsedUs = telemetry::NowUs() - startUs;
				g_blockSampleUs.fetch_add(elapsedUs);
				g_blockSamples.fetch_add(1);
				if (elapsedUs >= 2000) {
					telemetry::Record("terrain_block_slow", "\"us\":%llu,\"x\":%lld,\"y\":%lld",
						static_cast<unsigned long long>(elapsedUs), c1 ? static_cast<long long>(c1[0]) : 0ll,
						c1 ? static_cast<long long>(c1[1]) : 0ll);
				}
			}
			if ((call & 1023u) == 0) {
				telemetry::Record("terrain_block_batch", "\"calls\":1024,\"sample_count\":%u,\"sample_us\":%llu,\"skipped_total\":%u",
					g_blockSamples.exchange(0), static_cast<unsigned long long>(g_blockSampleUs.exchange(0)), g_blocksSkipped.load());
			}
		}

		// FUN_140978280(grid, request, coords): the on-demand block build of BGSTerrainBlock::Grid*File (vtable +0x78
		// -> here -> vtable +0x90 builder -> FUN_14098A330). After the builder returns it sets the block's built bit
		// (grid+0x240 [y * width + x], bit 1 << request+0x20) whether or not anything was generated, and the block is
		// never built again. A block we skipped (pages emptied by a shift) stayed empty for good: no heights, no
		// collision under the player (0.41.0: falling pose over visible ground after a shift that skipped 600 blocks),
		// stretched terrain. After a skip the bit is cleared again, so the next request rebuilds the block.
		using Ensure_t = std::uint64_t (*)(std::uintptr_t, std::uintptr_t, const int*);
		Ensure_t g_ensure = nullptr;
		std::atomic<unsigned> g_blocksRearmed{ 0 };

		std::uint64_t EnsureDetour(std::uintptr_t grid, std::uintptr_t request, const int* coords)
		{
			const bool outer = t_blockSkipped;  // nested builds (neighbour grids) keep their own flag
			t_blockSkipped = false;
			const auto result = g_ensure(grid, request, coords);
			if (t_blockSkipped && grid && request && coords) {
				const auto bits = *reinterpret_cast<std::uint8_t**>(grid + 0x240);
				const auto width = *reinterpret_cast<const std::int32_t*>(grid + 0x28);
				const auto level = *reinterpret_cast<const std::uint32_t*>(request + 0x20);
				if (bits && level < 7) {
					auto* cell = reinterpret_cast<volatile char*>(bits + static_cast<std::intptr_t>(coords[1]) * width + coords[0]);
					_InterlockedAnd8(cell, static_cast<char>(~(1u << level)));
					const auto n = g_blocksRearmed.fetch_add(1) + 1;
					if (n <= 5 || n % 100 == 0) {
						logger::Write("Terrain guard: skipped block (%d, %d) level %u marked unbuilt; the next request rebuilds it (%u so far)", coords[0], coords[1], level, n);
					}
				}
			}
			t_blockSkipped = outer;
			return result;
		}

		// FUN_14041efa0(worldspace, cell, coords, arg, flag): the end step of a LoadedCellDB::LoadCell job (from
		// FUN_14041f8f0 -> FUN_14041f120, worldspace = job +0xD0). Twice a job arrived with a null worldspace after world
		// shifts (+0x41EFD6, 0.35.4 and 0.36.1, 1.6 s and 9 s after a shift) and the read of worldspace +0x1F0 crashed.
		// It reads only +0x1F0, +0xF2 and +0xE0 of the worldspace; with a zeroed stand-in it skips the worldspace step
		// (FUN_140990850) and finishes the cell as usual. Each case is logged with the cell, to find the cause.
		using Finish_t = std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, const int*, std::uintptr_t, std::uint64_t);
		Finish_t g_finish = nullptr;
		alignas(16) std::uint8_t g_blankWorldspace[0x400] = {};
		std::atomic<unsigned> g_nullWorldspace{ 0 };

		// ---- Terrain collision rebuild -------------------------------------------------------------------------
		struct Pending
		{
			std::uintptr_t cell;
			terrainguard::Why why;
			unsigned long long since;
			unsigned long long notBefore;
			unsigned tries = 0;
		};
		std::mutex g_pendingLock;
		std::vector<Pending> g_pending;
		std::atomic<unsigned> g_rebuilt{ 0 };

		template <class T>
		T& F(std::uintptr_t base, std::uint32_t off) { return *reinterpret_cast<T*>(base + off); }

		template <class Fn>
		Fn E(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		// The terrain manager the LoadCell finish step uses (FUN_14041EFA0).
		std::uintptr_t TerrainManager(std::uintptr_t ws)
		{
			if (!ws) {
				return 0;
			}
			const auto handle = F<std::uintptr_t>(ws, 0x1F0);
			if (handle && (F<std::uint8_t>(ws, 0xF2) >> 1 & 1)) {
				return E<std::uintptr_t (*)(std::uintptr_t, int)>(addr::kWsTerrainFromHandle)(handle, 0);
			}
			return F<std::uintptr_t>(ws, 0xE0);
		}

		bool SourceAccepts(std::uintptr_t mgr, const int* coords)
		{
			return mgr && F<std::uintptr_t>(mgr, 0xE8) && E<bool (*)(std::uintptr_t, const int*)>(addr::kTerrainSourceAccepts)(mgr, coords);
		}

		void SourceRect(std::uintptr_t mgr, int out[4])
		{
			out[0] = out[1] = out[2] = out[3] = 0;
			const auto src = mgr ? F<std::uintptr_t>(mgr, 0xE8) : 0;
			const auto obj = src ? F<std::uintptr_t>(src, 8) : 0;
			if (!obj) {
				return;
			}
			auto** vtbl = *reinterpret_cast<void***>(obj);
			reinterpret_cast<void (*)(std::uintptr_t, int*)>(vtbl[0x88 / 8])(obj, out);       // origin
			reinterpret_cast<void (*)(std::uintptr_t, int*)>(vtbl[0x80 / 8])(obj, out + 2);   // dimensions
		}

		std::uintptr_t FinishDetour(std::uintptr_t ws, std::uintptr_t cell, const int* coords, std::uintptr_t arg, std::uint64_t flag)
		{
			const auto startedUs = telemetry::NowUs();
			const bool hadWorldspace = ws != 0;
			const auto formId = cell ? F<std::uint32_t>(cell, off::kFormId) : 0u;
			const int cellX = coords ? coords[0] : 0, cellY = coords ? coords[1] : 0;
			bool outsideSource = false;
			if (ws && cell && coords) {
				const auto mgr = TerrainManager(ws);
				if (mgr && !SourceAccepts(mgr, coords)) {
					outsideSource = true;
					int r[4];
					SourceRect(mgr, r);
					static std::atomic<unsigned> n{ 0 };
					if (n.fetch_add(1) < 40) {
						logger::Write("Terrain collision: cell (%d, %d) loaded outside the terrain source (origin %d, %d, size %d x %d): no ground collision; queued for a rebuild",
							coords[0], coords[1], r[0], r[1], r[2], r[3]);
					}
					terrainguard::MarkPending(cell, terrainguard::Why::Load);
				}
			}
			if (ws == 0) {
				const auto n = g_nullWorldspace.fetch_add(1) + 1;
				if (n <= 20) {
					logger::Write("Cell guard: a cell load finished with no worldspace (cell %08X, coords %d, %d); finished without the worldspace step (%u so far)",
						cell ? *reinterpret_cast<const std::uint32_t*>(cell + 0x28) : 0u, coords ? coords[0] : 0, coords ? coords[1] : 0, n);
				}
				ws = reinterpret_cast<std::uintptr_t>(g_blankWorldspace);
			}
			const auto result = g_finish(ws, cell, coords, arg, flag);
			telemetry::Record("cell_finish", "\"form\":\"%08X\",\"cx\":%d,\"cy\":%d,\"worldspace\":%d,\"outside_source\":%d,\"us\":%llu",
				formId, cellX, cellY,
				hadWorldspace ? 1 : 0, outsideSource ? 1 : 0, static_cast<unsigned long long>(telemetry::NowUs() - startedUs));
			return result;
		}

		bool Guarded(char* page, std::intptr_t* coords, std::intptr_t neighbour, std::intptr_t* neighbourCoords)
		{
			__try {
				g_original(page, coords, neighbour, neighbourCoords);
				return true;
			}
			__except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
				return false;
			}
		}

		void Detour(char* page, std::intptr_t* coords, std::intptr_t neighbour, std::intptr_t* neighbourCoords)
		{
			g_lastActivity.store(GetTickCount64());
			crash::SetExpectedFaults(true);
			const bool ok = Guarded(page, coords, neighbour, neighbourCoords);
			crash::SetExpectedFaults(false);
			if (!ok) {
				const auto n = g_skipped.fetch_add(1) + 1;
				if (n <= 5 || n % 100 == 0) {
					logger::Write("Terrain guard: skipped a terrain page stitch whose neighbour was gone (%u so far)", n);
				}
			}
		}
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kTerrainPageStitch, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Terrain guard: hook failed");
			return false;
		}
		g_original = reinterpret_cast<Stitch_t>(original);
		static const std::uint8_t kGenPrologue[15] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x4C, 0x89, 0x48, 0x20, 0x48, 0x89, 0x50, 0x10 };
		void* gen = nullptr;
		if (!hook::Install(game::Base() + addr::kTerrainBlockGenerate, kGenPrologue, sizeof(kGenPrologue), reinterpret_cast<void*>(&GenerateDetour), &gen)) {
			logger::Write("Terrain guard: block generator hook failed");
		}
		else {
			g_generate = reinterpret_cast<Generate_t>(gen);
		}
		static const std::uint8_t kEnsurePrologue[15] = { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57 };
		void* ens = nullptr;
		if (!hook::Install(game::Base() + addr::kTerrainBlockEnsure, kEnsurePrologue, sizeof(kEnsurePrologue), reinterpret_cast<void*>(&EnsureDetour), &ens)) {
			logger::Write("Terrain guard: block ensure hook failed (skipped blocks stay empty)");
		}
		else {
			g_ensure = reinterpret_cast<Ensure_t>(ens);
		}
		static const std::uint8_t kFinishPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20 };
		void* fin = nullptr;
		if (!hook::Install(game::Base() + addr::kCellLoadFinish, kFinishPrologue, sizeof(kFinishPrologue), reinterpret_cast<void*>(&FinishDetour), &fin)) {
			logger::Write("Cell guard: cell load finish hook failed");
		}
		else {
			g_finish = reinterpret_cast<Finish_t>(fin);
		}
		logger::Write("Terrain guard: page stitch and block generator guarded; cell guard: load finish without a worldspace");
		return true;
	}

	unsigned Skipped() { return g_skipped.load(); }

	void MarkPending(std::uintptr_t cell, Why why)
	{
		const auto now = GetTickCount64();
		std::lock_guard lk(g_pendingLock);
		for (auto& p : g_pending) {
			if (p.cell == cell) {
				p.why = why;
				return;
			}
		}
		g_pending.push_back({ cell, why, now, now, 0 });
		telemetry::Record("collision_queued", "\"form\":\"%08X\",\"why\":%u,\"pending\":%zu", F<std::uint32_t>(cell, off::kFormId),
			static_cast<unsigned>(why), g_pending.size());
	}

	void MarkGridPending(std::uintptr_t tes, unsigned delayMs)
	{
		const auto entries = F<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = std::min<std::uint32_t>(F<std::uint32_t>(tes, off::kTESGridCount), 64);
		const auto now = GetTickCount64();
		std::lock_guard lk(g_pendingLock);
		for (std::uint32_t i = 0; entries && i < count; ++i) {
			const auto cell = entries[i] ? F<std::uintptr_t>(entries[i], off::kGridEntryCell) : 0;
			if (!cell) {
				continue;
			}
			bool found = false;
			for (auto& p : g_pending) {
				if (p.cell == cell) {
					p.notBefore = now + delayMs;
					found = true;
				}
			}
			if (!found) {
				g_pending.push_back({ cell, Why::Shift, now, now + delayMs, 0 });
			}
		}
	}

	bool HoldPlayer(std::uintptr_t cell)
	{
		if (!cell) {
			return false;
		}
		const auto now = GetTickCount64();
		std::lock_guard lk(g_pendingLock);
		for (const auto& p : g_pending) {
			if (p.cell == cell && p.why == Why::Load && now - p.since < 60000) {
				return true;
			}
		}
		return false;
	}

	unsigned PendingCount()
	{
		std::lock_guard lk(g_pendingLock);
		return unsigned(g_pending.size());
	}

	void ServicePending(std::uintptr_t tes)
	{
		static unsigned long long last = 0;
		const auto now = GetTickCount64();
		if (!tes || now - last < 250) {
			return;
		}
		last = now;
		std::vector<Pending> work;
		{
			std::lock_guard lk(g_pendingLock);
			if (g_pending.empty()) {
				return;
			}
			work = g_pending;
		}
		// Only cells in the loaded grid, attached (state 10); others are dropped after a minute (unloaded or never
		// attached: their pointer may be gone).
		const auto entries = F<std::uintptr_t*>(tes, off::kTESGridCells);
		const auto count = std::min<std::uint32_t>(F<std::uint32_t>(tes, off::kTESGridCount), 64);
		auto inGrid = [&](std::uintptr_t cell) {
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				if (entries[i] && F<std::uintptr_t>(entries[i], off::kGridEntryCell) == cell) {
					return true;
				}
			}
			return false;
		};
		struct Ready { std::uintptr_t cell, mgr; int coords[2]; Why why; unsigned long long since; unsigned tries; };
		std::vector<Ready> ready;
		std::vector<std::uintptr_t> drop;
		unsigned waitGrid = 0, waitDelay = 0, waitAttach = 0, waitSource = 0, waitTerrain = 0, verifiedGround = 0;
		// Detaching the rover's cell while it is being driven severs PlayerVehicle. In the 04:30 Cydonia-area
		// run, a 25-cell rebuild finished 44 ms before hover stopped with "left the vehicle". Leave that cell
		// pending until the rover leaves it; the other grid cells can rebuild now.
		const auto player = *reinterpret_cast<const std::uintptr_t*>(game::Base() + addr::kPlayerCharacterPtr);
		const auto vehicle = player ? E<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player) : 0;
		const auto playerCell = vehicle ? F<std::uintptr_t>(player, off::kRefParentCell) : 0;
		const auto vehicleCell = vehicle ? F<std::uintptr_t>(vehicle, off::kRefParentCell) : 0;
		const auto vehiclePos = vehicle ? F<game::Vec3>(vehicle, off::kRefPosition) : game::Vec3{};
		const int vehicleX = vehicle ? static_cast<int>(std::floor(vehiclePos.x / off::kCellSize)) : 0;
		const int vehicleY = vehicle ? static_cast<int>(std::floor(vehiclePos.y / off::kCellSize)) : 0;
		unsigned deferredOccupied = 0;
		for (const auto& p : work) {
			if (!inGrid(p.cell)) {
				++waitGrid;
				if (now - p.since > 60000) {
					drop.push_back(p.cell);
				}
				continue;
			}
			if (now < p.notBefore) {
				++waitDelay;
				continue;
			}
			if (F<std::uint8_t>(p.cell, off::kCellState) != 10) {
				++waitAttach;
				continue;
			}
			const auto ext = F<std::uintptr_t>(p.cell, off::kCellExterior);
			const auto ws = F<std::uintptr_t>(p.cell, off::kCellWorldspace);
			const auto mgr = TerrainManager(ws);
			if (!ext || !mgr) {
				drop.push_back(p.cell);
				continue;
			}
			Ready r{ p.cell, mgr, { F<std::int32_t>(ext, 0), F<std::int32_t>(ext, 4) }, p.why, p.since, p.tries };
			if (!SourceAccepts(mgr, r.coords)) {
				++waitSource;
				continue;  // the terrain source does not cover it yet
			}
			// A world shift already moves Havok ground bodies on the engine's shiftWorld path.
			// Rebuilding every cell unconditionally detached working collision and cost 95-143 ms
			// per batch in the 01:45 run, even though the post-shift ground ring passed.
			// Verify the shifted cell at three separated points before scheduling a rebuild.
			if (p.why == Why::Shift) {
				constexpr float samples[3][2] = { { 0.2f, 0.2f }, { 0.5f, 0.5f }, { 0.8f, 0.8f } };
				unsigned hits = 0;
				for (const auto& sample : samples) {
					const float point[3] = { (r.coords[0] + sample[0]) * off::kCellSize,
						(r.coords[1] + sample[1]) * off::kCellSize, 0.0f };
					const float ground = E<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(addr::kGroundRayCast)
						(tes, point, 0x22006, 3000.0f, 6000.0f);
					if (std::isfinite(ground) && ground > -1e30f && ground != -500.0f) ++hits;
				}
				if (hits == 3) {
					++verifiedGround;
					drop.push_back(p.cell);
					continue;
				}
			}
			if (vehicle && (p.cell == playerCell || p.cell == vehicleCell ||
				(r.coords[0] == vehicleX && r.coords[1] == vehicleY))) {
				++deferredOccupied;
				continue;
			}
			// After a load the source has only just moved here: its blocks are still being generated, and a build now
			// samples nothing (0.45.0: rebuilt 0.7 s after the source moved, the player then fell 400 m). Wait until
			// the terrain jobs have been quiet for a moment.
			if (p.why == Why::Load && now - g_lastActivity.load() < 1200) {
				++waitTerrain;
				continue;
			}
			ready.push_back(r);
		}
		{
			static ULONGLONG lastWaitSample = 0;
			if (now - lastWaitSample >= 1000) {
				lastWaitSample = now;
				unsigned long long oldest = 0;
				for (const auto& p : work) oldest = std::max(oldest, now - p.since);
				telemetry::Record("collision_backlog", "\"pending\":%zu,\"ready\":%zu,\"verified_ground\":%u,\"grid\":%u,\"delay\":%u,\"attach\":%u,\"occupied\":%u,\"source\":%u,\"terrain_busy\":%u,\"oldest_ms\":%llu",
					work.size(), ready.size(), verifiedGround, waitGrid, waitDelay, waitAttach, deferredOccupied, waitSource, waitTerrain, oldest);
			}
		}
		if (deferredOccupied) {
			static ULONGLONG lastDeferralLog = 0;
			if (now - lastDeferralLog >= 2000) {
				lastDeferralLog = now;
				logger::Write("Terrain collision: deferred %u occupied rover cells; rebuilding the other cells", deferredOccupied);
			}
		}
		// Staggered: the cells under and next to the player are rebuilt at once; the outer ring is ordered by
		// predicted travel position, because a cell ahead can be reached before a nearer one behind the rover.
		// 25 cells in one go took 166-583 ms. Load rebuilds keep the old all-at-once rule:
		// the player is held until the ground exists anyway.
		if (!ready.empty() && player) {
			const auto pp = F<game::Vec3>(vehicle ? vehicle : player, off::kRefPosition);
			const int px = static_cast<int>(std::floor(pp.x / off::kCellSize)), py = static_cast<int>(std::floor(pp.y / off::kCellSize));
			static game::Vec3 previous{};
			static ULONGLONG previousAt = 0;
			float aheadX = pp.x, aheadY = pp.y;
			if (previousAt && now > previousAt) {
				const float dt = static_cast<float>(now - previousAt) * 0.001f;
				const float dx = pp.x - previous.x, dy = pp.y - previous.y;
				if (dt < 1.0f && std::hypot(dx, dy) < 200.0f) {
					const float lead = std::min(1.5f, 200.0f / std::max(1.0f, std::hypot(dx, dy) / dt));
					aheadX += dx / dt * lead;
					aheadY += dy / dt * lead;
				}
			}
			previous = pp;
			previousAt = now;
			auto dist = [&](const Ready& r) { return std::max(std::abs(r.coords[0] - px), std::abs(r.coords[1] - py)); };
			auto score = [&](const Ready& r) {
				const float cx = (r.coords[0] + 0.5f) * off::kCellSize;
				const float cy = (r.coords[1] + 0.5f) * off::kCellSize;
				return std::hypot(cx - aheadX, cy - aheadY);
			};
			std::stable_sort(ready.begin(), ready.end(), [&](const Ready& a, const Ready& b) {
				if ((dist(a) <= 1) != (dist(b) <= 1)) return dist(a) < dist(b);
				return score(a) < score(b);
			});
			constexpr unsigned kFarPerTick = 3;
			std::vector<Ready> now1;
			unsigned farCount = 0, later = 0;
			for (const auto& r : ready) {
				if (r.why == Why::Load || dist(r) <= 1 || farCount < kFarPerTick) {
					if (r.why != Why::Load && dist(r) > 1) ++farCount;
					now1.push_back(r);
				}
				else {
					++later;
				}
			}
			if (later) {
				static ULONGLONG lastStaggerLog = 0;
				if (now - lastStaggerLog >= 1000) {
					lastStaggerLog = now;
					logger::Write("Terrain collision: staggered: %zu cells now (nearest first), %u later", now1.size(), later);
				}
			}
			ready.swap(now1);
		}
		if (!ready.empty()) {
			const auto arg = F<std::uintptr_t>(tes, 0x88);
			LARGE_INTEGER f, t0, t1;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&t0);
			const auto startedUs = telemetry::NowUs();
			std::vector<std::uint64_t> detachUs(ready.size()), buildUs(ready.size()), attachUs(ready.size());
			for (size_t i = 0; i < ready.size(); ++i) {
				const auto s = telemetry::NowUs();
				E<void (*)(std::uintptr_t, char, char)>(addr::kCellDetach)(ready[i].cell, 1, 0);
				detachUs[i] = telemetry::NowUs() - s;
			}
			const auto detachedUs = telemetry::NowUs();
			for (size_t i = 0; i < ready.size(); ++i) {
				const auto& r = ready[i];
				const auto s = telemetry::NowUs();
				const auto systems = E<std::uintptr_t (*)(std::uintptr_t)>(addr::kCellSystems)(r.cell);
				if (systems) {
					E<void (*)(std::uintptr_t)>(addr::kCellSystemsClear)(systems);
				}
				E<void (*)(std::uintptr_t, const int*, std::uintptr_t)>(addr::kTerrainCollisionBuild)(r.mgr, r.coords, r.cell);
				buildUs[i] = telemetry::NowUs() - s;
			}
			const auto builtUs = telemetry::NowUs();
			unsigned attached = 0;
			for (size_t i = 0; i < ready.size(); ++i) {
				const auto& r = ready[i];
				const auto s = telemetry::NowUs();
				const auto st = F<std::uint8_t>(r.cell, off::kCellState);
				if (st == 6 || st == 7) {
					E<void (*)(std::uintptr_t, std::uintptr_t)>(addr::kCellAttach)(r.cell, arg);
					E<void (*)(std::uintptr_t, char)>(addr::kCellAttachFinish)(r.cell, 1);
					++attached;
				}
				attachUs[i] = telemetry::NowUs() - s;
			}
			const auto attachedUs = telemetry::NowUs();
			QueryPerformanceCounter(&t1);
			int rect[4];
			SourceRect(ready.front().mgr, rect);
			unsigned loads = 0;
			for (const auto& r : ready) {
				loads += r.why == Why::Load;
			}
			g_rebuilt.fetch_add(unsigned(ready.size()));
			logger::Write("Terrain collision: rebuilt %u cells (%u after a load, %u after a shift; %u re-attached) in %.1f ms; first (%d, %d); source origin (%d, %d) size %d x %d",
				unsigned(ready.size()), loads, unsigned(ready.size()) - loads, attached, double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart),
				ready.front().coords[0], ready.front().coords[1], rect[0], rect[1], rect[2], rect[3]);
			telemetry::Record("terrain_rebuild", "\"cells\":%u,\"load\":%u,\"ms\":%.1f,\"detach_us\":%llu,\"build_us\":%llu,\"attach_us\":%llu", unsigned(ready.size()), loads,
				double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart),
				static_cast<unsigned long long>(detachedUs - startedUs), static_cast<unsigned long long>(builtUs - detachedUs),
				static_cast<unsigned long long>(attachedUs - builtUs));
			for (size_t i = 0; i < ready.size(); ++i) {
				const auto& r = ready[i];
				telemetry::Record("collision_cell", "\"form\":\"%08X\",\"cx\":%d,\"cy\":%d,\"why\":%u,\"queue_ms\":%llu,\"tries\":%u,\"detach_us\":%llu,\"build_us\":%llu,\"attach_us\":%llu",
					F<std::uint32_t>(r.cell, off::kFormId), r.coords[0], r.coords[1], static_cast<unsigned>(r.why),
					static_cast<unsigned long long>(now - r.since), r.tries, static_cast<unsigned long long>(detachUs[i]),
					static_cast<unsigned long long>(buildUs[i]), static_cast<unsigned long long>(attachUs[i]));
			}
			// Cells rebuilt after a load are checked with the ground ray at their centre; no ground yet = queued again
			// (the player stays held), up to 20 tries.
			std::vector<std::uintptr_t> retry;
			for (const auto& r : ready) {
				if (r.why != Why::Load) {
					continue;
				}
				const float p[3] = { (float(r.coords[0]) + 0.5f) * 100.0f, (float(r.coords[1]) + 0.5f) * 100.0f, 0.0f };
				const float g = E<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(addr::kGroundRayCast)(tes, p, 0x22006, 3000.0f, 6000.0f);
				if (!(g > -1e30f) || g == -500.0f) {
					retry.push_back(r.cell);
				}
			}
			if (!retry.empty()) {
				logger::Write("Terrain collision: %zu of the rebuilt cells still have no ground under a ray; trying again", retry.size());
			}
			for (const auto& r : ready) {
				if (std::find(retry.begin(), retry.end(), r.cell) == retry.end()) {
					drop.push_back(r.cell);
				}
			}
			if (!retry.empty()) {
				std::lock_guard lk(g_pendingLock);
				for (auto& p : g_pending) {
					if (std::find(retry.begin(), retry.end(), p.cell) != retry.end()) {
						p.notBefore = now + 1000;
						if (++p.tries >= 20) {
							drop.push_back(p.cell);
							logger::Write("Terrain collision: giving up on a cell after 20 rebuilds without ground");
						}
					}
				}
			}
		}
		if (!drop.empty()) {
			std::lock_guard lk(g_pendingLock);
			std::erase_if(g_pending, [&](const Pending& p) { return std::find(drop.begin(), drop.end(), p.cell) != drop.end(); });
		}
	}

	unsigned long long LastActivityMs() { return g_lastActivity.load(); }
}
