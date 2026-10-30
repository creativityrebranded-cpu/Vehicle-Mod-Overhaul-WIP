#include "groundnav.h"
#include "fvolimits.h"
#include "environment.h"
#include "features.h"
#include "log.h"
#include "navmap.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <unordered_map>
#include <vector>

namespace groundnav
{
	namespace
	{
		using groundplan::kCell;
		using groundplan::Mode;
		constexpr int kPilots = 1 + fvolimits::kVehicles;  // vehicleflight pilots 0 (your vehicle) .. kVehicles
		constexpr int kN = 141;                  // grid cells per side (564 m)
		constexpr int kGatherPerFrame = 3000;    // cells filled per frame on the main thread
		constexpr ULONGLONG kMinReplanMs = 1500, kMaxPlanAgeMs = 8000, kSeenMs = 600;
		constexpr ULONGLONG kTerrainLifeMs = 10 * 60 * 1000;
		constexpr std::size_t kTerrainMax = 60000;
		constexpr unsigned kAskMax = 24;         // outstanding ground rays of ours at once
		constexpr unsigned kQueueShare = 48;     // the shared ray queue's backlog above which we ask nothing new

		bool (*g_road)(double, double) = nullptr;

		// ---- Terrain memory (downward rays) ----
		struct Terrain { float z = 0, nz = 1; bool hit = false, water = false, normal = false; ULONGLONG at = 0; };
		std::unordered_map<std::int64_t, Terrain> g_terrain;
		std::int64_t Key(double x, double y) { return (std::int64_t(groundplan::Grid::Index(x)) << 32) ^ std::uint32_t(groundplan::Grid::Index(y)); }
		struct Ask { double x = 0, y = 0, top = 0, bottom = 0; std::uint32_t self = 0; ULONGLONG since = 0; std::int64_t key = 0; };
		std::vector<Ask> g_asks;
		struct SenseStats { unsigned answered = 0, asked = 0, dropped = 0; ULONGLONG at = 0; } g_sense;

		const Terrain* TerrainAt(double x, double y, ULONGLONG now)
		{
			const auto it = g_terrain.find(Key(x, y));
			return it != g_terrain.end() && now - it->second.at < kTerrainLifeMs ? &it->second : nullptr;
		}
		void WantGround(std::uint32_t self, double x, double y, double hintZ, ULONGLONG now)
		{
			const auto key = Key(x, y);
			if (TerrainAt(x, y, now)) return;
			for (const auto& a : g_asks) if (a.key == key) return;
			if (g_asks.size() >= kAskMax || environment::Pending() > kQueueShare) return;
			// The cell centre, from well above (decks, roofs) to far below (valleys).
			Ask a; a.key = key; a.self = self; a.since = now;
			a.x = (groundplan::Grid::Index(x) + 0.5) * kCell; a.y = (groundplan::Grid::Index(y) + 0.5) * kCell;
			a.top = hintZ + 80; a.bottom = hintZ - 250;
			g_asks.push_back(a); ++g_sense.asked;
		}
		void SenseTick(ULONGLONG now)
		{
			for (auto it = g_asks.begin(); it != g_asks.end();) {
				environment::Ground hit;
				if (environment::GroundAt(it->self, it->x, it->y, it->top, it->bottom, hit)) {
					Terrain t; t.hit = hit.hit; t.z = float(hit.z); t.nz = float(hit.nz); t.water = hit.water; t.normal = hit.normalOk; t.at = now;
					g_terrain[it->key] = t; ++g_sense.answered;
					it = g_asks.erase(it);
				}
				else if (now - it->since > 3000) { ++g_sense.dropped; it = g_asks.erase(it); }
				else ++it;
			}
			if (g_terrain.size() > kTerrainMax) std::erase_if(g_terrain, [&](const auto& e) { return now - e.second.at > kTerrainLifeMs / 2; });
			if (now - g_sense.at >= 10000) {
				if (g_sense.asked || g_sense.answered) {
					unsigned water = 0, steep = 0;
					for (const auto& [k, t] : g_terrain) { if (t.water) ++water; if (t.hit && t.normal && t.nz < 0.8f) ++steep; }
					logger::Write("Ground sense: %zu terrain cells mapped (%u water, %u steep); rays in 10 s: %u asked, %u answered, %u dropped",
						g_terrain.size(), water, steep, g_sense.asked, g_sense.answered, g_sense.dropped);
				}
				g_sense = {}; g_sense.at = now;
			}
		}

		// ---- One cell of the planning grid from everything known ----
		groundplan::Cell CellAt(double x, double y, double hintZ, const Request& rq, double airClear, ULONGLONG now)
		{
			groundplan::Cell c;
			const Terrain* t = TerrainAt(x, y, now);
			const auto any = navmap::At(x, y, hintZ);
			if (t && t->hit) {
				c.ground = t->z; c.water = t->water;
				if (t->normal) c.nz = t->nz;
			}
			else if (any.haveGround) c.ground = any.ground;
			const double base = c.Known() ? double(c.ground) : hintZ;
			// Hull height: a remembered hit clearly above the ground (a hillside hit sits at it), or a collision object's box.
			const auto low = navmap::At(x, y, base + 1.2);
			const double lowFloor = c.Known() ? base + 0.7 : base + 1.0;
			c.solidLow = (low.blocked && low.blockedZ > lowFloor && low.blockedZ < base + rq.height + 1.5) || low.objBlocked;
			const auto high = navmap::At(x, y, base + airClear);
			c.solidHigh = (high.blocked && std::fabs(high.blockedZ - (base + airClear)) < 3.0) || high.objBlocked;
			c.contacts = std::uint8_t(std::min(low.contacts, 255));
			if (g_road && c.Known() && !c.water) c.road = g_road(x, y);
			return c;
		}

		groundplan::Limits LimitsFor(const Request& rq)
		{
			groundplan::Limits lim;
			lim.halfWidth = std::clamp(rq.halfWidth, 0.6, 6.0);
			lim.halfLength = std::clamp(rq.halfLength, 1.0, 10.0);
			lim.height = std::clamp(rq.height, 1.0, 6.0);
			lim.canFly = rq.canFly;
			lim.topSpeed = std::max(5.0, rq.topSpeed);
			lim.cruise = std::min(lim.topSpeed, rq.speedLimit > 0 ? rq.speedLimit : 22.0);
			lim.latAccel = std::clamp(rq.latAccel, 1.5, 12.0);
			lim.minTurnRadius = std::max(4.0, lim.halfLength * 2.2);
			return lim;
		}

		// ---- Per pilot ----
		struct Pilot
		{
			Request rq;
			groundplan::Plan plan;
			std::size_t cursor = 0;
			bool have = false, want = false;
			const char* why = "first route";
			ULONGLONG planAt = 0, askAt = 0, seen = 0, logAt = 0, checkAt = 0, offSince = 0;
			double planGX = 0, planGY = 0, planGZ = 0;
			unsigned route = 0, generation = 0;
		};
		std::array<Pilot, kPilots> g_p;

		// ---- The one search in progress: the grid is filled over a few frames, searched on the worker ----
		struct Job
		{
			int pilot = -1; unsigned generation = 0;
			Request rq; groundplan::Limits lim; groundplan::Grid grid; double hintZ = 0;
			int filled = 0; ULONGLONG started = 0;
			Mode startMode = Mode::Ground;
			const char* why = "";
		} g_job;
		struct Result { groundplan::Plan plan; double ms = 0; };
		std::future<Result> g_search;
		bool g_searching = false;
		int g_searchPilot = -1; unsigned g_searchGeneration = 0; const char* g_searchWhy = ""; Request g_searchRq;

		void StartJob(int pilot, ULONGLONG now)
		{
			auto& p = g_p[pilot];
			g_job = Job{};
			g_job.pilot = pilot; g_job.generation = p.generation; g_job.rq = p.rq; g_job.lim = LimitsFor(p.rq); g_job.started = now; g_job.why = p.why;
			g_job.hintZ = p.rq.z;
			g_job.startMode = p.rq.airborne && p.rq.canFly ? Mode::Air : Mode::Ground;
			// Lean the grid toward the goal: up to 180 m ahead of the vehicle, never leaving the vehicle near an edge.
			const double dx = p.rq.gx - p.rq.x, dy = p.rq.gy - p.rq.y, d = std::hypot(dx, dy);
			const double lean = std::min(d * 0.5, kN * kCell * 0.5 - 60.0);
			const double cx = p.rq.x + (d > 1 ? dx / d * lean : 0), cy = p.rq.y + (d > 1 ? dy / d * lean : 0);
			g_job.grid.Resize(groundplan::Grid::Index(cx) - kN / 2, groundplan::Grid::Index(cy) - kN / 2, kN);
			p.want = false; p.askAt = now;
		}
		void GatherTick(ULONGLONG now)
		{
			if (g_job.pilot < 0 || g_searching) return;
			auto& G = g_job.grid;
			const int total = G.n * G.n;
			const int end = std::min(total, g_job.filled + kGatherPerFrame);
			for (int k = g_job.filled; k < end; ++k) {
				const int i = k % G.n, j = k / G.n;
				G.cells[k] = CellAt(G.X(i), G.Y(j), g_job.hintZ, g_job.rq, g_job.lim.airClear, now);
			}
			g_job.filled = end;
			if (g_job.filled < total) return;
			// Copy for the worker; the main thread keeps nothing it shares.
			g_searchPilot = g_job.pilot; g_searchGeneration = g_job.generation; g_searchWhy = g_job.why; g_searchRq = g_job.rq;
			const auto rq = g_job.rq; const auto lim = g_job.lim; const auto start = g_job.startMode;
			g_search = std::async(std::launch::async, [grid = std::move(g_job.grid), rq, lim, start]() {
				const auto t0 = std::chrono::steady_clock::now();
				Result r;
				r.plan = groundplan::Search(grid, lim, rq.x, rq.y, rq.z, start, rq.gx, rq.gy, rq.gz, std::max(2.0, rq.stop));
				groundplan::SpeedProfile(r.plan, lim, rq.speed, rq.stopAtEnd);
				r.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				return r;
			});
			g_searching = true;
			g_job.pilot = -1;
		}
		void CollectTick(ULONGLONG now)
		{
			if (!g_searching || g_search.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
			Result r = g_search.get();
			g_searching = false;
			if (g_searchPilot < 0 || g_searchPilot >= kPilots) return;
			auto& p = g_p[g_searchPilot];
			if (p.generation != g_searchGeneration) return;  // forgotten or a new vehicle meanwhile
			const auto& q = g_searchRq;
			if (!r.plan.ok) {
				logger::Write("Ground plan: [%d] %08X no route (%s; %s); %.0f ms, %u cells searched", g_searchPilot, q.vehicle, r.plan.why, g_searchWhy, r.ms, r.plan.expanded);
				p.have = false; p.planAt = now;
				return;
			}
			p.plan = std::move(r.plan); p.cursor = 0; p.have = true; p.planAt = now; ++p.route;
			p.planGX = q.gx; p.planGY = q.gy; p.planGZ = q.gz; p.offSince = 0;
			double top = 0; for (const auto& w : p.plan.pts) top = std::max(top, w.speed);
			logger::Write("Ground plan: [%d] %08X route %u (%s): %.0f m ground (%.0f unmapped), %.0f m air, %u mode changes, %zu points, up to %.0f m/s; %s; %.0f ms, %u cells searched",
				g_searchPilot, q.vehicle, p.route, g_searchWhy, p.plan.groundMetres, p.plan.unknownMetres, p.plan.airMetres, p.plan.modeChanges,
				p.plan.pts.size(), top, p.plan.why, r.ms, p.plan.expanded);
		}

		// Is the route ahead still open on the current map? Checks the next ~100 m every 4 m.
		bool BlockedAhead(Pilot& p, ULONGLONG now, const char*& why)
		{
			const auto& pts = p.plan.pts;
			double run = 0;
			for (std::size_t s = p.cursor; s + 1 < pts.size() && run < 100; ++s) {
				const double len = std::hypot(pts[s + 1].x - pts[s].x, pts[s + 1].y - pts[s].y);
				for (double d = 0; d < len && run + d < 100; d += kCell) {
					const double t = len > 1e-6 ? d / len : 0, x = pts[s].x + (pts[s + 1].x - pts[s].x) * t, y = pts[s].y + (pts[s + 1].y - pts[s].y) * t;
					const double z = pts[s].z + (pts[s + 1].z - pts[s].z) * t;
					const auto c = CellAt(x, y, z, p.rq, 6.0, now);
					if (pts[s].mode == Mode::Ground && pts[s + 1].mode == Mode::Ground) {
						if (c.water) { why = "water found on the route"; return true; }
						if (c.solidLow && run + d > 6) { why = "something solid on the route"; return true; }
						if (c.Known() && std::isfinite(c.nz) && c.nz < 0.8f) { why = "too steep on the route"; return true; }
					}
					else if (pts[s].mode == Mode::Air && pts[s + 1].mode == Mode::Air && c.solidHigh && run + d > 6) { why = "something solid on the air leg"; return true; }
				}
				run += len;
			}
			return false;
		}

		// The sensing wanted for this pilot: the route ahead, the straight way to the goal and the ground around it.
		void SenseFor(const Pilot& p, ULONGLONG now)
		{
			const auto& rq = p.rq;
			for (int oy = -1; oy <= 1; ++oy) for (int ox = -1; ox <= 1; ++ox) WantGround(rq.vehicle, rq.x + ox * 8.0, rq.y + oy * 8.0, rq.z, now);
			if (p.have) {
				const auto& pts = p.plan.pts; double run = 0;
				for (std::size_t s = p.cursor; s + 1 < pts.size() && run < 320; ++s) {
					const double len = std::hypot(pts[s + 1].x - pts[s].x, pts[s + 1].y - pts[s].y);
					for (double d = 0; d < len; d += 8) {
						const double t = d / std::max(len, 1e-6);
						WantGround(rq.vehicle, pts[s].x + (pts[s + 1].x - pts[s].x) * t, pts[s].y + (pts[s + 1].y - pts[s].y) * t, pts[s].z + (pts[s + 1].z - pts[s].z) * t, now);
						// Either side of the lane too (slope and edges).
						const double ux = (pts[s + 1].y - pts[s].y) / std::max(len, 1e-6), uy = -(pts[s + 1].x - pts[s].x) / std::max(len, 1e-6);
						if (int(d / 8) % 2 == 0) for (int side : { -1, 1 })
							WantGround(rq.vehicle, pts[s].x + (pts[s + 1].x - pts[s].x) * t + ux * 8 * side, pts[s].y + (pts[s + 1].y - pts[s].y) * t + uy * 8 * side, rq.z, now);
					}
					run += len;
				}
			}
			const double dx = rq.gx - rq.x, dy = rq.gy - rq.y, d = std::hypot(dx, dy);
			for (double s = 12; s < std::min(d, 300.0); s += 12) WantGround(rq.vehicle, rq.x + dx / d * s, rq.y + dy / d * s, rq.z, now);
		}
	}

	bool Enabled() { return features::On(features::Id::NativeGround); }
	void SetRoadQuery(bool (*onRoad)(double, double)) { g_road = onRoad; }

	Status Guide(int pilot, const Request& rq)
	{
		Status out;
		if (pilot < 0 || pilot >= kPilots || !Enabled()) return out;
		const ULONGLONG now = GetTickCount64();
		auto& p = g_p[pilot];
		if (p.rq.vehicle != rq.vehicle) { const unsigned gen = p.generation + 1; p = Pilot{}; p.generation = gen; p.why = "new vehicle"; }
		p.rq = rq; p.seen = now;
		const double dist = std::hypot(rq.gx - rq.x, rq.gy - rq.y);
		// Replan reasons, most urgent first.
		const char* why = nullptr;
		if (!p.have) why = p.planAt ? "no route yet" : p.why;
		else if (std::hypot(rq.gx - p.planGX, rq.gy - p.planGY) > std::max(10.0, 0.1 * dist)) why = "the goal moved";
		else if (now - p.planAt > kMaxPlanAgeMs) why = "route refresh";
		else if (!p.plan.reachesGoal && !p.plan.pts.empty() && std::hypot(p.plan.pts.back().x - rq.x, p.plan.pts.back().y - rq.y) < 150) why = "partial route nearly used: extending";
		groundplan::Guidance gd;
		if (p.have) {
			gd = groundplan::Follow(p.plan, rq.x, rq.y, rq.speed, p.cursor, LimitsFor(rq));
			if (gd.offPath) { if (!p.offSince) p.offSince = now; else if (now - p.offSince > 1000 && !why) why = "off the route"; }
			else p.offSince = 0;
			if (!why && now - p.checkAt >= 500) { p.checkAt = now; const char* b = nullptr; if (BlockedAhead(p, now, b)) why = b; }
		}
		if (why && !p.want && now - p.askAt >= (p.have ? kMinReplanMs : 300)) {
			p.want = true; p.why = why;
			if (p.have && p.route) logger::Write("Ground plan: [%d] %08X replan: %s", pilot, rq.vehicle, why);
		}
		if (!p.have || !gd.ok) return out;
		out.ok = true; out.guide = gd; out.reachesGoal = p.plan.reachesGoal; out.endsOnGround = p.plan.pts.back().mode == Mode::Ground;
		out.groundMetres = p.plan.groundMetres; out.airMetres = p.plan.airMetres; out.modeChanges = p.plan.modeChanges; out.route = p.route;
		if (now - p.logAt >= 3000) {
			p.logAt = now;
			char next[96] = "";
			if (std::isfinite(gd.toModeChange)) std::snprintf(next, sizeof next, "; %s in %.0f m", gd.nextMode == Mode::Air ? "take-off" : "landing", gd.toModeChange);
			logger::Write("Ground plan: [%d] %08X following route %u, %s leg %zu/%zu: %.1f m off, route speed %.1f, speed %.1f, %.0f m to go%s",
				pilot, rq.vehicle, p.route, groundplan::Name(gd.mode), gd.segment + 1, gd.segments, gd.cross, gd.speed, rq.speed, gd.remaining, next);
		}
		return out;
	}

	void Invalidate(int pilot, const char* why)
	{
		if (pilot < 0 || pilot >= kPilots) return;
		auto& p = g_p[pilot];
		if (!p.rq.vehicle) return;
		p.want = true; p.why = why; p.askAt = 0;
		logger::Write("Ground plan: [%d] %08X replan: %s", pilot, p.rq.vehicle, why);
	}
	void Forget(int pilot)
	{
		if (pilot < 0 || pilot >= kPilots) return;
		const unsigned gen = g_p[pilot].generation + 1;
		g_p[pilot] = Pilot{}; g_p[pilot].generation = gen;
	}

	void Service()
	{
		const ULONGLONG now = GetTickCount64();
		CollectTick(now);
		if (!Enabled()) { if (!g_asks.empty()) g_asks.clear(); return; }
		for (int i = 0; i < kPilots; ++i) {
			auto& p = g_p[i];
			if (!p.rq.vehicle) continue;
			if (now - p.seen > 5000) { Forget(i); continue; }  // no longer guided (stopped, hovering by itself)
			if (now - p.seen < kSeenMs) SenseFor(p, now);
		}
		SenseTick(now);
		if (g_job.pilot < 0 && !g_searching) {
			// The pilot that asked longest ago goes next.
			int next = -1;
			for (int i = 0; i < kPilots; ++i) if (g_p[i].want && g_p[i].rq.vehicle && now - g_p[i].seen < kSeenMs && (next < 0 || g_p[i].askAt < g_p[next].askAt)) next = i;
			if (next >= 0) StartJob(next, now);
		}
		GatherTick(now);
	}

	void Reset()
	{
		if (g_searching) { g_search.wait(); g_searching = false; }
		g_job = Job{};
		for (auto& p : g_p) { const unsigned gen = p.generation + 1; p = Pilot{}; p.generation = gen; }
		g_terrain.clear(); g_asks.clear(); g_sense = {};
	}
}
