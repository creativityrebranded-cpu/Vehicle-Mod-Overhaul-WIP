#include "cellbake.h"
#include "cellbakegrid.h"

#include "addresses.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "navseed.h"
#include "prefetch.h"
#include "runtimecells.h"
#include "vehicle_probe.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

namespace cellbake
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T> T At(uptr p, std::uint32_t o) { return *reinterpret_cast<const T*>(p + o); }

		constexpr std::uint32_t kTerrainLayers = (1u << 13) | (1u << 17);  // vehicleflight's terrain probes
		constexpr double kCellMin = 100.0, kCellMax = 220.0;              // m: a cell's box from its references, clamped
		constexpr ULONGLONG kPollMs = 1000, kReportMs = 30000, kSolidWaitMs = 3000;
		constexpr int kBank = prefetch::kSlotBakeCount / 2;

		struct Job { std::uint32_t cell = 0; float mn[3] = {}, mx[3] = {}; };
		cellbakegrid::Grid g_grid;
		cellbakegrid::Agreement g_agree;
		std::unordered_set<std::uint32_t> g_known;   // cells baked or queued in this world
		std::deque<Job> g_jobs;
		uptr g_world = 0;
		bool g_use = false;
		ULONGLONG g_pollAt = 0, g_reportAt = 0, g_iniAt = 0;
		unsigned g_cellsBaked = 0, g_solids = 0, g_points = 0, g_solidFails = 0;

		// The job in hand: its solids, then its terrain points, a bank of probes a frame.
		bool g_have = false;
		Job g_job;
		int g_phase = 0;                  // 0 solids asked?, 1 waiting solids, 2 terrain
		ULONGLONG g_askedAt = 0;
		std::vector<std::pair<std::int32_t, std::int32_t>> g_todo;
		std::size_t g_next = 0;
		struct Pending { std::int32_t ix = 0, iy = 0; bool on = false; };
		Pending g_pending[prefetch::kSlotBakeCount];
		int g_bank = 0, g_waitTicks = 0;

		uptr Player() { __try { return *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
		uptr WorldOf(uptr player)
		{
			__try {
				const uptr cell = At<uptr>(player, off::kRefParentCell);
				return cell && !(At<std::uint8_t>(cell, 0x48) & 1) ? At<uptr>(cell, off::kCellWorldspace) : 0;  // exteriors only
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		// A cell's box: around its references (clamped to a cell's size), from below the lowest to well over the highest.
		bool CellBox(uptr cell, Job& j)
		{
			uptr* refs = nullptr;
			std::uint32_t n = 0;
			if (!runtimecells::References(cell, refs, n) || !n) return false;
			double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
			unsigned used = 0;
			__try {
				for (std::uint32_t i = 0; i < n && i < 4000; ++i) {
					const uptr r = refs[i];
					if (!r) continue;
					const auto p = At<game::Vec3>(r, off::kRefPosition);
					if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
					lo[0] = std::min(lo[0], double(p.x)); lo[1] = std::min(lo[1], double(p.y)); lo[2] = std::min(lo[2], double(p.z));
					hi[0] = std::max(hi[0], double(p.x)); hi[1] = std::max(hi[1], double(p.y)); hi[2] = std::max(hi[2], double(p.z));
					++used;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			if (!used) return false;
			for (int k = 0; k < 2; ++k) {
				const double mid = 0.5 * (lo[k] + hi[k]);
				const double half = std::clamp(0.5 * (hi[k] - lo[k]), kCellMin * 0.5, kCellMax * 0.5);
				j.mn[k] = float(mid - half); j.mx[k] = float(mid + half);
			}
			j.mn[2] = float(lo[2] - 30.0); j.mx[2] = float(hi[2] + 250.0);
			return true;
		}
		void Poll(uptr player)
		{
			const uptr world = WorldOf(player);
			if (world != g_world) {  // another world: nothing of the old one applies
				if (g_world) logger::Write("Cell bake: world changed; %u cell(s), %zu terrain point(s) dropped", g_cellsBaked, g_grid.h.size());
				g_world = world; g_grid.Clear(); g_known.clear(); g_jobs.clear(); g_have = false; g_cellsBaked = 0;
			}
			if (!world) return;
			const unsigned grid = runtimecells::GridSize(64);
			for (unsigned i = 0; i < grid; ++i) {
				const uptr cell = runtimecells::Attached(i);
				if (!cell) continue;
				std::uint32_t id = 0;
				__try { id = At<std::uint32_t>(cell, off::kFormId); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
				if (!id || g_known.count(id)) continue;
				Job j; j.cell = id;
				if (!CellBox(cell, j)) continue;
				g_known.insert(id);
				g_jobs.push_back(j);
			}
		}
		void StartTerrain()
		{
			g_todo.clear(); g_next = 0;
			const std::int32_t x0 = cellbakegrid::Index(g_job.mn[0]), x1 = cellbakegrid::Index(g_job.mx[0]) + 1;
			const std::int32_t y0 = cellbakegrid::Index(g_job.mn[1]), y1 = cellbakegrid::Index(g_job.mx[1]) + 1;
			for (std::int32_t ix = x0; ix <= x1; ++ix)
				for (std::int32_t iy = y0; iy <= y1; ++iy)
					if (!g_grid.Has(ix, iy)) g_todo.push_back({ ix, iy });
			for (auto& p : g_pending) p.on = false;
			g_waitTicks = 0;
			g_phase = 2;
		}
		// One bank: read what it was asked last round (two rounds ago for its slots), then ask the next points.
		void TerrainStep()
		{
			const int first = g_bank * kBank;
			for (int k = 0; k < kBank; ++k) {
				auto& p = g_pending[first + k];
				if (!p.on) continue;
				float h = 0; unsigned age = 0;
				if (prefetch::LatestGround(prefetch::kSlotBake0 + first + k, h, age) && age <= 3 && h > -1e30f && h != -500.0f) {
					g_grid.Put(p.ix, p.iy, h);
					++g_points;
				}
				p.on = false;
			}
			const float top = g_job.mx[2] + 300.0f;
			for (int k = 0; k < kBank && g_next < g_todo.size(); ++k, ++g_next) {
				const auto [ix, iy] = g_todo[g_next];
				auto& p = g_pending[first + k];
				p.ix = ix; p.iy = iy; p.on = true;
				prefetch::RequestGroundMask(prefetch::kSlotBake0 + first + k, float(ix * cellbakegrid::kStep), float(iy * cellbakegrid::kStep), top, 0.0f, 8000.0f, kTerrainLayers);
			}
			g_bank ^= 1;
		}
		void Work(ULONGLONG now)
		{
			if (!g_have) {
				if (g_jobs.empty()) return;
				g_job = g_jobs.front(); g_jobs.pop_front();
				g_have = true; g_phase = 0;
			}
			if (g_phase == 0) {
				if (vehicle_probe::RequestSolids(g_job.mn, g_job.mx)) { g_phase = 1; g_askedAt = now; }
				return;
			}
			if (g_phase == 1) {
				std::vector<std::uint32_t> ids;
				bool ok = false;
				if (vehicle_probe::TakeSolids(ids, ok)) {
					std::sort(ids.begin(), ids.end());
					ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
					if (ok) { navseed::MarkSolid(ids.data(), ids.size()); g_solids += unsigned(ids.size()); }
					else ++g_solidFails;
					StartTerrain();
				}
				else if (now - g_askedAt > kSolidWaitMs) { ++g_solidFails; StartTerrain(); }  // no answer: the terrain anyway
				return;
			}
			TerrainStep();
			// Done when every point was asked and both banks have been read once more.
			bool waiting = false;
			for (const auto& p : g_pending) waiting = waiting || p.on;
			if (g_next >= g_todo.size() && !waiting) { g_have = false; ++g_cellsBaked; }
		}
	}

	void Tick(bool loading)
	{
		const ULONGLONG now = GetTickCount64();
		static fvopaths::Watch watch;  // re-read only when settings.ini changed (2026-10-29 audit)
		if (watch.Changed(fvopaths::Settings())) {
			g_iniAt = now;
			const bool use = GetPrivateProfileIntW(L"nav", L"bakedGround", 0, fvopaths::Settings().c_str()) != 0;
			if (use != g_use) logger::Write("Cell bake: NPC pilots %s the baked terrain (settings.ini [nav] bakedGround)", use ? "now use" : "do not use");
			g_use = use;
		}
		if (loading) { g_have = false; g_jobs.clear(); g_known.clear(); return; }  // cells are rebuilt; bake them once loaded
		const uptr player = Player();
		if (!player) return;
		if (now - g_pollAt >= kPollMs) { g_pollAt = now; Poll(player); }
		Work(now);
		if (now - g_reportAt >= kReportMs) {
			g_reportAt = now;
			logger::Write("Cell bake: %u cell(s) baked, %zu queued; %u solid object(s) named by the physics world (%u queries without an answer); %zu terrain point(s); "
				"agreement with live terrain probes: %u sample(s), mean %.2f m apart, %u within 0.5 m, %u more than 2 m off",
				g_cellsBaked, g_jobs.size() + (g_have ? 1 : 0), g_solids, g_solidFails, g_grid.h.size(), g_agree.samples, g_agree.Mean(), g_agree.close, g_agree.off2);
		}
	}

	bool Ground(double x, double y, double& z) { return g_grid.Get(x, y, z); }
	bool UseForPilots() { return g_use; }
	void Compare(double x, double y, double live)
	{
		double z = 0;
		if (g_grid.Get(x, y, z)) g_agree.Add(z, live);
	}
	void Reset()
	{
		g_world = 0; g_grid.Clear(); g_known.clear(); g_jobs.clear(); g_have = false;
		g_cellsBaked = g_solids = g_points = g_solidFails = 0;
		g_agree = {};
	}
}
