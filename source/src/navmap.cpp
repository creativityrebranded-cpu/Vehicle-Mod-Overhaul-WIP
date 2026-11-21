#include "navmap.h"

#include "log.h"
#include "fvopaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace navmap
{
	namespace
	{
		constexpr double kCell = 4.0;                 // m: grid cell size
		constexpr ULONGLONG kCellLifeMs = 180000;     // a cell not touched this long is forgotten
		constexpr ULONGLONG kBlockedLifeMs = 120000;  // a solid hit is trusted this long
		constexpr ULONGLONG kFloorLifeMs = 600000;    // a learned floor (platform) is kept longer
		constexpr ULONGLONG kContactLifeMs = 120000;
		constexpr std::size_t kMaxCells = 60000;
		constexpr double kBlockedBand = 5.0;          // m above / below a hit's height that count as blocked
		constexpr double kFreeBand = 3.0;             // m: a free band is extended by rays this close to it
		constexpr double kFloorBelowOrigin = 1.5;     // m: a push-up's floor sits about this far under the vehicle's origin
		constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
		constexpr ULONGLONG kObjLifeMs = 8000;        // an object band not refreshed by the scan this long is gone (the scan runs every ~2 s)
		constexpr double kObjVehTop = 2.0;            // m: the vehicle's height over its origin (a band meeting [z - 0.5, z + this] blocks it)
		constexpr int kObjMaxCells = 1600;            // cells one object may mark (a 160 m square)
		constexpr ULONGLONG kObjFreeTrustMs = 60000;  // a ray's free pass overrules an object box for this long
		constexpr ULONGLONG kBlockedStaleMs = 2000;   // a ray hit older than this is forgotten when a later ray flies through it
		double g_seedX = 0, g_seedY = 0, g_seedR = 0;
		ULONGLONG g_seedAt = 0;

		// Collision layers whose hits are remembered as obstacles: static things that stay where they are. Characters
		// (8 biped, 30 character controller, 33) and debris are not.
		constexpr std::uint64_t kPersistentLayers = (1ull << 1) | (1ull << 2) | (1ull << 9) | (1ull << 10) | (1ull << 13) | (1ull << 17) | (1ull << 35);

		struct Cell
		{
			float ground = kNaN, floor = kNaN, blockedZ = kNaN, freeLo = kNaN, freeHi = kNaN;
			ULONGLONG seen = 0, groundAt = 0, floorAt = 0, blockedAt = 0, freeAt = 0, contactAt = 0;
			std::uint8_t contacts = 0;
			float objLo[2] = { kNaN, kNaN }, objHi[2] = { kNaN, kNaN };  // solid height bands from the game's collision objects (navseed)
			// Every level seen here, for World (2026-11-06): `ground` keeps only the highest, so stacked decks collapse. Beside it, not instead of it.
			static constexpr int kLevels = 3;
			float levelZ[kLevels] = { kNaN, kNaN, kNaN };
			ULONGLONG levelAt[kLevels] = {};
			ULONGLONG objAt[2] = {};
			// The actual ray hit points in this cell (2026-11-04: a 0.3 m post blocked its whole 4 m cell; waypoint driving asks
			// HitNear for the real points instead). Ring of the last kHits, each with its time.
			static constexpr int kHits = 4;
			float hitX[kHits] = { kNaN, kNaN, kNaN, kNaN }, hitY[kHits] = { kNaN, kNaN, kNaN, kNaN }, hitZ[kHits] = { kNaN, kNaN, kNaN, kNaN };
			ULONGLONG hitAt[kHits] = {};
			float hitUp[kHits] = { kNaN, kNaN, kNaN, kNaN };  // the hit surface's normal z (NaN unknown): up-facing = floor
			// Kept for World (2026-11-06, planworld.h), beside hitUp, which stays as it was until the sign below is proven in a run:
			float hitNz[kHits] = { kNaN, kNaN, kNaN, kNaN };  // the normal z as the game gave it, with its sign
			std::uint32_t hitOwner[kHits] = {};               // the reference that owns the hit body, 0 unknown
			std::uint8_t hitLayer[kHits] = { 255, 255, 255, 255 };  // the game's collision layer of the hit (255 unknown; world truth A)
			std::uint8_t hitNext = 0;
		};

		std::unordered_map<std::uint64_t, Cell> g_cells;
		ULONGLONG g_lastTick = 0, g_lastLog = 0;

		struct Counters
		{
			unsigned ground = 0, rays = 0, contacts = 0, hits = 0, normals = 0, sampUsed = 0, sampFallback = 0, cleared = 0,
				sweeps = 0, hullBoth = 0, hullOnly = 0, rayOnly = 0;
		} g_n;

		bool Finite(double v) { return std::isfinite(v) && std::fabs(v) < 1e7; }

		std::uint64_t Key(int ix, int iy) { return (std::uint64_t(std::uint32_t(ix)) << 32) | std::uint32_t(iy); }
		int CellIndex(double v) { return int(std::floor(v / kCell)); }

		Cell* Find(double x, double y)
		{
			const auto it = g_cells.find(Key(CellIndex(x), CellIndex(y)));
			return it == g_cells.end() ? nullptr : &it->second;
		}

		Cell& Make(double x, double y) { return g_cells[Key(CellIndex(x), CellIndex(y))]; }

		bool Valid(float v) { return std::isfinite(v); }

		void MarkFree(Cell& c, float z, ULONGLONG now)
		{
			if (Valid(c.freeLo) && now - c.freeAt < kCellLifeMs && z >= c.freeLo - kFreeBand && z <= c.freeHi + kFreeBand) {
				c.freeLo = std::min(c.freeLo, z);
				c.freeHi = std::max(c.freeHi, z);
			}
			else {
				c.freeLo = c.freeHi = z;
			}
			c.freeAt = c.seen = now;
		}
	}

	// Boxes kept whole for World (2026-11-06). Buckets of 64 m; a box is listed in every bucket it touches. Same box again refreshes it. The
	// bands above stay in charge: road boxes are whole deck pieces with railings, so nothing blocks on these until a run proves the wanted-surface
	// test reads them as floor (OBS system=plan.world).
	namespace {
		constexpr double kBoxBucket = 64.0;
		struct StoredBox { KeptBox box; ULONGLONG at = 0; };
		std::unordered_map<std::uint64_t, std::vector<StoredBox>> g_boxes;
		std::uint64_t BoxBucket(int ix, int iy) { return (std::uint64_t(std::uint32_t(ix)) << 32) | std::uint32_t(iy); }
		int BucketIndex(double v) { return int(std::floor(v / kBoxBucket)); }
		void KeepBox(const KeptBox& b, ULONGLONG now) {
			const double c = std::fabs(std::cos(b.yaw)), s = std::fabs(std::sin(b.yaw)), ex = c * b.hx + s * b.hy, ey = s * b.hx + c * b.hy;
			for (int ix = BucketIndex(b.x - ex); ix <= BucketIndex(b.x + ex); ++ix)
				for (int iy = BucketIndex(b.y - ey); iy <= BucketIndex(b.y + ey); ++iy) {
					auto& list = g_boxes[BoxBucket(ix, iy)]; bool have = false;
					for (auto& o : list) if (std::fabs(o.box.x - b.x) < .05 && std::fabs(o.box.y - b.y) < .05 && std::fabs(o.box.zLo - b.zLo) < .05) { o.box = b; o.at = now; have = true; break; }
					if (!have) { list.erase(std::remove_if(list.begin(), list.end(), [&](const StoredBox& o) { return now - o.at >= kObjLifeMs; }), list.end()); list.push_back({ b, now }); }
				}
		}
	}
	int BoxesNear(double x, double y, double radius, KeptBox* out, int max)
	{
		if (!Finite(x) || !Finite(y) || !(radius > 0) || !out || max <= 0) return 0;
		const ULONGLONG now = GetTickCount64(); int n = 0;
		for (int ix = BucketIndex(x - radius); ix <= BucketIndex(x + radius); ++ix)
			for (int iy = BucketIndex(y - radius); iy <= BucketIndex(y + radius); ++iy) {
				const auto it = g_boxes.find(BoxBucket(ix, iy));
				if (it == g_boxes.end()) continue;
				for (const auto& o : it->second) {
					if (n >= max) return n;
					if (now - o.at >= kObjLifeMs || std::hypot(o.box.x - x, o.box.y - y) > radius + std::hypot(o.box.hx, o.box.hy)) continue;
					bool dup = false; for (int k = 0; k < n; ++k) if (std::fabs(out[k].x - o.box.x) < .05 && std::fabs(out[k].y - o.box.y) < .05 && std::fabs(out[k].zLo - o.box.zLo) < .05) { dup = true; break; }
					if (!dup) out[n++] = o.box;
				}
			}
		return n;
	}
	int GroundLevels(double x, double y, float* out, int max)
	{
		if (!Finite(x) || !Finite(y) || !out || max <= 0) return 0;
		const auto it = g_cells.find(Key(CellIndex(x), CellIndex(y)));
		if (it == g_cells.end()) return 0;
		int n = 0; for (int k = 0; k < Cell::kLevels && n < max; ++k) if (Valid(it->second.levelZ[k])) out[n++] = it->second.levelZ[k];
		return n;
	}
	void NoteObject(double cx, double cy, double yaw, double hx, double hy, double zlo, double zhi)
	{
		if (!Finite(cx) || !Finite(cy) || !Finite(zlo) || !Finite(zhi) || !(hx > 0) || !(hy > 0)) return;
		const ULONGLONG now = GetTickCount64();
		KeepBox({ cx, cy, yaw, hx, hy, zlo, zhi }, now);
		const double c = std::cos(yaw), s = std::sin(yaw);
		// The box's world footprint (heading `yaw`: forward = (sin, cos), right = (cos, -sin)), inflated by half a cell.
		const double ex = std::fabs(c) * hx + std::fabs(s) * hy + kCell * 0.5, ey = std::fabs(s) * hx + std::fabs(c) * hy + kCell * 0.5;
		const int x0 = CellIndex(cx - ex), x1 = CellIndex(cx + ex), y0 = CellIndex(cy - ey), y1 = CellIndex(cy + ey);
		if ((x1 - x0 + 1) * (y1 - y0 + 1) > kObjMaxCells) return;
		for (int ix = x0; ix <= x1; ++ix) {
			for (int iy = y0; iy <= y1; ++iy) {
				const double px = (ix + 0.5) * kCell - cx, py = (iy + 0.5) * kCell - cy;
				const double lr = px * c - py * s, lf = px * s + py * c;  // into the box's own right / forward
				if (std::fabs(lr) > hx + kCell * 0.5 || std::fabs(lf) > hy + kCell * 0.5) continue;
				Cell& cell = g_cells[Key(ix, iy)];
				cell.seen = now;
				// Merge into an overlapping band, else a free / stale slot, else the nearer band.
				int k = -1;
				for (int b = 0; b < 2; ++b) {
					if (Valid(cell.objLo[b]) && now - cell.objAt[b] < kObjLifeMs && zlo <= cell.objHi[b] + 1.0 && zhi >= cell.objLo[b] - 1.0) { k = b; break; }
				}
				if (k < 0) {
					for (int b = 0; b < 2; ++b) {
						if (!Valid(cell.objLo[b]) || now - cell.objAt[b] >= kObjLifeMs) { k = b; cell.objLo[b] = float(zlo); cell.objHi[b] = float(zhi); break; }
					}
				}
				if (k < 0) {
					const double mid = 0.5 * (zlo + zhi);
					k = std::fabs(mid - 0.5 * (cell.objLo[0] + cell.objHi[0])) <= std::fabs(mid - 0.5 * (cell.objLo[1] + cell.objHi[1])) ? 0 : 1;
				}
				if (now - cell.objAt[k] < kObjLifeMs && Valid(cell.objLo[k])) {
					cell.objLo[k] = std::min(cell.objLo[k], float(zlo));
					cell.objHi[k] = std::max(cell.objHi[k], float(zhi));
				}
				else {
					cell.objLo[k] = float(zlo);
					cell.objHi[k] = float(zhi);
				}
				cell.objAt[k] = now;
			}
		}
	}


	void NoteSeeded(double cx, double cy, double radius)
	{
		g_seedX = cx;
		g_seedY = cy;
		g_seedR = radius;
		g_seedAt = GetTickCount64();
	}

	void NoteGround(double x, double y, double h)
	{
		if (!Finite(x) || !Finite(y) || !Finite(h)) return;
		const ULONGLONG now = GetTickCount64();
		Cell& c = Make(x, y);
		c.ground = float(h);
		c.groundAt = c.seen = now;
		{	int k = -1, oldest = 0;  // the same level again refreshes it; a new one takes the oldest slot
			for (int i = 0; i < Cell::kLevels; ++i) {
				if (Valid(c.levelZ[i]) && std::fabs(c.levelZ[i] - float(h)) < 1.0f) { k = i; break; }
				if (c.levelAt[i] < c.levelAt[oldest]) oldest = i;
			}
			if (k < 0) k = oldest;
			c.levelZ[k] = float(h); c.levelAt[k] = now;
		}
		++g_n.ground;
	}

	std::atomic<unsigned> g_signs[4]{};  // rays falling onto a flat surface: normal up, normal down; rays rising into one: normal up, normal down
	void NoteRay(const float from[3], const float to[3], bool hit, float hx, float hy, float hz, std::uint32_t layer, bool normalOk, float normalZ, bool keepAnyLayer, std::uint32_t owner)
	{
		for (int i = 0; i < 3; ++i) {
			if (!Finite(from[i]) || !Finite(to[i])) return;
		}
		const ULONGLONG now = GetTickCount64();
		const bool usable = hit && Finite(hx) && Finite(hy) && Finite(hz);
		const double ex = usable ? hx : to[0], ey = usable ? hy : to[1], ez = usable ? hz : to[2];
		const double dx = ex - from[0], dy = ey - from[1], dz = ez - from[2];
		const double len = std::hypot(dx, dy);
		const double freeLen = std::max(0.0, len - (usable ? 1.0 : 0.0));
		for (double s = 0.0; s <= freeLen; s += kCell * 0.75) {
			const double t = len > 1e-6 ? s / len : 0.0;
			Cell& c = Make(from[0] + dx * t, from[1] + dy * t);
			const float z = float(from[2] + dz * t);
			MarkFree(c, z, now);
			// A remembered hit this ray now flies through (at its height, more than a cell short of the ray's own end, the hit over 2 s old) was
			// a door since opened or an object since moved: forgotten (2026-10-20). A fresh hit is kept, so a thin pole in the cell is not lost.
			if (s <= freeLen - kCell && Valid(c.blockedZ) && now - c.blockedAt > kBlockedStaleMs && std::fabs(z - c.blockedZ) <= kBlockedBand * 0.6) {
				c.blockedZ = kNaN;
				++g_n.cleared;
			}
			// The same rule for the kept hit points: an old one at this height that a ray now flies through is forgotten. Only one the ray
			// really passes (within 0.5 m of its line, 2026-11-05): a ray 1.5 m beside a post crossed the post's 4 m cell and erased it,
			// so obstacles flickered in and out of the plan.
			if (s <= freeLen - kCell)
				for (int k = 0; k < Cell::kHits; ++k)
					if (Valid(c.hitZ[k]) && now - c.hitAt[k] > kBlockedStaleMs && std::fabs(z - c.hitZ[k]) <= kBlockedBand * 0.6 &&
						(len < 1e-6 || std::fabs((c.hitX[k] - from[0]) * dy - (c.hitY[k] - from[1]) * dx) / len < 0.5)) c.hitZ[k] = kNaN;
		}
		if (usable) {
			++g_n.hits;
			if (normalOk) ++g_n.normals;
		}
		if (usable && (keepAnyLayer || (layer < 64 && ((kPersistentLayers >> layer) & 1ull)))) {
			Cell& c = Make(hx, hy);
			c.blockedZ = hz;
			c.blockedAt = c.seen = now;
			const int k = c.hitNext++ % Cell::kHits;
			c.hitX[k] = hx; c.hitY[k] = hy; c.hitZ[k] = hz; c.hitAt[k] = now;
			c.hitUp[k] = normalOk && Finite(normalZ) ? std::fabs(normalZ) : kNaN;
			c.hitNz[k] = normalOk && Finite(normalZ) ? normalZ : kNaN; c.hitOwner[k] = owner; c.hitLayer[k] = layer < 255 ? std::uint8_t(layer) : std::uint8_t(255);
			// Proof of the sign: a ray falling onto a flat surface should read normal up. Until a run shows that, nothing relies on the sign.
			if (normalOk && Finite(normalZ) && std::fabs(normalZ) > 0.7f) g_signs[(hz < from[2] ? 0 : 2) + (normalZ > 0 ? 0 : 1)].fetch_add(1, std::memory_order_relaxed);
		}
		++g_n.rays;
	}

	void NoteContact(double x, double y, double z, bool up, double dx, double dy)
	{
		if (!Finite(x) || !Finite(y) || !Finite(z)) return;
		const ULONGLONG now = GetTickCount64();
		++g_n.contacts;
		if (up) {
			// Something under the vehicle that the ground probes missed: a floor, about a footprint wide.
			const float floor = float(z - kFloorBelowOrigin);
			for (int ox = -1; ox <= 1; ++ox) {
				for (int oy = -1; oy <= 1; ++oy) {
					Cell& c = Make(x + ox * kCell, y + oy * kCell);
					c.floor = Valid(c.floor) && now - c.floorAt < kFloorLifeMs ? std::max(c.floor, floor) : floor;
					c.floorAt = c.seen = now;
				}
			}
			return;
		}
		const double len = std::hypot(dx, dy);
		const double ux = len > 1e-6 ? dx / len : 0.0, uy = len > 1e-6 ? dy / len : 0.0;
		for (int k = 0; k <= 1; ++k) {  // here and one cell ahead
			Cell& c = Make(x + ux * kCell * k, y + uy * kCell * k);
			c.blockedZ = float(z);
			c.blockedAt = c.contactAt = c.seen = now;
			if (c.contacts < 255) ++c.contacts;
		}
	}

	namespace
	{
		Sample AtCell(const Cell* c, bool seeded, double z, ULONGLONG now)
		{
		Sample s;
		if (!c) {
			s.known = seeded;
			return s;
		}
		if (Valid(c->ground) && now - c->groundAt < kCellLifeMs) {
			s.haveGround = true;
			s.ground = c->ground;
		}
		if (Valid(c->floor) && now - c->floorAt < kFloorLifeMs) {
			s.ground = s.haveGround ? std::max(s.ground, c->floor) : c->floor;
			s.haveGround = true;
		}
		s.blocked = Valid(c->blockedZ) && now - c->blockedAt < kBlockedLifeMs && std::fabs(z - c->blockedZ) <= kBlockedBand;
		if (s.blocked) s.blockedZ = c->blockedZ;
		s.free = Valid(c->freeLo) && now - c->freeAt < kCellLifeMs && z >= c->freeLo - 2.0 && z <= c->freeHi + 2.0;
		if (now - c->contactAt < kContactLifeMs) s.contacts = c->contacts;
		for (int b = 0; b < 2; ++b) {  // a solid object's band where the vehicle would be
			if (Valid(c->objLo[b]) && now - c->objAt[b] < kObjLifeMs && z + kObjVehTop >= c->objLo[b] - 0.5 && z - 0.5 <= c->objHi[b]) s.objBlocked = true;
		}
		// The two sources combined (2026-10-20, the user: "a combination of the data"): an object's box is the prior, the rays are the evidence.
		// A ray that recently flew through this cell at this height proves the box wrong here (an arch's opening, an L-shape's gap): open. A ray
		// hit stays solid (s.blocked) whatever the boxes say; a box nobody has probed stays solid (the early warning).
		if (s.objBlocked && Valid(c->freeLo) && now - c->freeAt < kObjFreeTrustMs && z >= c->freeLo - 1.0 && z <= c->freeHi + 1.0) {
			s.objBlocked = false;
			s.objOverruled = true;
		}
		s.known = s.haveGround || s.blocked || s.objBlocked || s.free || s.contacts > 0 || seeded;
		return s;
		}
	}

	Sample At(double x, double y, double z)
	{
		const ULONGLONG now = GetTickCount64();
		// Inside the area the object scan covered: known (no object here = open), whatever the probes saw.
		const bool seeded = g_seedAt && now - g_seedAt < kObjLifeMs && std::hypot(x - g_seedX, y - g_seedY) < g_seedR;
		return AtCell(Find(x, y), seeded, z, now);
	}

	std::atomic<unsigned> g_floorSkips{ 0 };
	unsigned TakeFloorSkips() { return g_floorSkips.exchange(0); }
	bool WallNear(double x, double y, double zLo, double zHi, double radius, double* hitZ, bool* face)
	{
		if (face) *face = false;
		if (!Finite(x) || !Finite(y) || !(radius > 0)) return false;
		const ULONGLONG now = GetTickCount64();
		const int x0 = CellIndex(x - radius), x1 = CellIndex(x + radius), y0 = CellIndex(y - radius), y1 = CellIndex(y + radius);
		bool found = false; double top = -1e30;
		for (int ix = x0; ix <= x1; ++ix)
			for (int iy = y0; iy <= y1; ++iy) {
				const auto it = g_cells.find(Key(ix, iy));
				if (it == g_cells.end()) continue;
				const Cell& c = it->second;
				for (int k = 0; k < Cell::kHits; ++k) {
					if (!Valid(c.hitZ[k]) || now - c.hitAt[k] >= kBlockedLifeMs) continue;
					if (c.hitZ[k] < zLo || c.hitZ[k] > zHi) continue;
					const double d = std::hypot(c.hitX[k] - x, c.hitY[k] - y);
					if (d > radius) continue;
					if (Valid(c.hitUp[k]) && c.hitUp[k] > 0.7f) { g_floorSkips.fetch_add(1, std::memory_order_relaxed); continue; }  // a floor, not a wall
					found = true;
					if (face && Valid(c.hitUp[k]) && std::fabs(c.hitUp[k]) < 0.5f) *face = true;
					if (c.hitZ[k] > top) { top = c.hitZ[k]; if (hitZ) *hitZ = c.hitZ[k]; }
				}
			}
		return found;
	}
	void TakeNormalSigns(unsigned out[4]) { for (int i = 0; i < 4; ++i) out[i] = g_signs[i].exchange(0); }
	int HitsNear(double x, double y, double zLo, double zHi, double radius, KeptHit* out, int max)
	{
		if (!Finite(x) || !Finite(y) || !(radius > 0) || !out || max <= 0) return 0;
		const ULONGLONG now = GetTickCount64();
		const int x0 = CellIndex(x - radius), x1 = CellIndex(x + radius), y0 = CellIndex(y - radius), y1 = CellIndex(y + radius);
		int n = 0;
		for (int ix = x0; ix <= x1; ++ix)
			for (int iy = y0; iy <= y1; ++iy) {
				if (n == max) return n;
				const auto it = g_cells.find(Key(ix, iy));
				if (it == g_cells.end()) continue;
				const Cell& c = it->second;
				for (int k = 0; k < Cell::kHits && n < max; ++k) {
					if (!Valid(c.hitZ[k]) || now - c.hitAt[k] >= kBlockedLifeMs || c.hitZ[k] < zLo || c.hitZ[k] > zHi) continue;
					if (std::hypot(c.hitX[k] - x, c.hitY[k] - y) > radius) continue;
					out[n++] = { c.hitX[k], c.hitY[k], c.hitZ[k], Valid(c.hitUp[k]) ? c.hitUp[k] : 0.0f, Valid(c.hitNz[k]) ? c.hitNz[k] : 0.0f, Valid(c.hitUp[k]), c.hitOwner[k], double(now - c.hitAt[k]) / 1000.0, c.hitLayer[k] };
				}
			}
		return n;
	}
	bool HitNear(double x, double y, double zLo, double zHi, double radius, double* hitZ)
	{
		if (!Finite(x) || !Finite(y) || !(radius > 0)) return false;
		const ULONGLONG now = GetTickCount64();
		const int x0 = CellIndex(x - radius), x1 = CellIndex(x + radius), y0 = CellIndex(y - radius), y1 = CellIndex(y + radius);
		bool found = false; double best = 1e30;
		for (int ix = x0; ix <= x1; ++ix)
			for (int iy = y0; iy <= y1; ++iy) {
				const auto it = g_cells.find(Key(ix, iy));
				if (it == g_cells.end()) continue;
				const Cell& c = it->second;
				for (int k = 0; k < Cell::kHits; ++k) {
					if (!Valid(c.hitZ[k]) || now - c.hitAt[k] >= kBlockedLifeMs) continue;
					if (c.hitZ[k] < zLo || c.hitZ[k] > zHi) continue;
					const double d = std::hypot(c.hitX[k] - x, c.hitY[k] - y);
					if (d <= radius && d < best) { best = d; found = true; if (hitZ) *hitZ = c.hitZ[k]; }
				}
			}
		return found;
	}

	void Column(double x, double y, const double* zs, int n, Sample* out)
	{
		const ULONGLONG now = GetTickCount64();
		const bool seeded = g_seedAt && now - g_seedAt < kObjLifeMs && std::hypot(x - g_seedX, y - g_seedY) < g_seedR;
		const Cell* c = Find(x, y);
		for (int k = 0; k < n; ++k) out[k] = AtCell(c, seeded, zs[k], now);
	}

	Verdict PathClear(const float p[3], double dx, double dy, double halfWidth, double clearance, double climbRate, double speed,
		double* at, const char** why)
	{
		const double len = std::hypot(dx, dy);
		if (!Finite(len) || len < 0.5) return Verdict::Clear;
		const double ux = dx / len, uy = dy / len, px = -uy, py = ux;  // forward, and its left
		const double lateral[3] = { 0.0, halfWidth, -halfWidth };
		int known = 0, total = 0;
		for (double s = 2.0; s <= len; s += 2.0) {
			for (const double off : lateral) {
				const Sample smp = At(p[0] + ux * s + px * off, p[1] + uy * s + py * off, p[2]);
				++total;
				if (smp.known) ++known;
				const double need = smp.haveGround ? smp.ground + clearance - p[2] : -1e9;  // metres to climb to pass it
				if (smp.blocked || need > climbRate * (s / std::max(speed, 5.0))) {
					if (at) *at = s;
					if (why) *why = smp.blocked ? "solid" : "surface";
					return Verdict::Blocked;
				}
			}
		}
		return total > 0 && known * 10 < total * 3 ? Verdict::Unknown : Verdict::Clear;
	}


	namespace
	{
		bool g_samplerOn = true, g_hullOn = true, g_raysOn = true;
		ULONGLONG g_samplerRead = 0;
		std::wstring g_iniPath;

		void ReadIni()
		{
			if (g_iniPath.empty()) {
				PWSTR docs = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
					g_iniPath = fvopaths::File(L"nav.ini");
					CoTaskMemFree(docs);
				}
				else {
					g_iniPath = L"-";
				}
			}
			if (g_iniPath == L"-") return;
			const bool on = true;  // [nav] sampler: fixed on (2026-11-05 settings strip)
			if (on != g_samplerOn) logger::Write("Nav map: heading sampler %s (nav.ini)", on ? "ON" : "OFF");
			g_samplerOn = on;
			const bool hull = true;  // [nav] shape: fixed on (2026-11-05 settings strip)
			if (hull != g_hullOn) logger::Write("Nav map: hull sweeps %s (nav.ini)", hull ? "ON" : "OFF");
			g_hullOn = hull;
			const bool rays = true;  // [nav] rays: fixed on (2026-11-05 settings strip)
			if (rays != g_raysOn) logger::Write("Nav map: engine ray and sweep queries %s (nav.ini)", rays ? "ON" : "OFF");
			g_raysOn = rays;
		}
	}

	bool SamplerEnabled()
	{
		const ULONGLONG now = GetTickCount64();
		static const std::wstring nav = fvopaths::File(L"nav.ini");
		static fvopaths::Watch watch;  // re-read only when nav.ini changed (2026-10-29 audit)
		if (watch.Changed(nav)) {
			g_samplerRead = now;
			ReadIni();
		}
		return g_samplerOn;
	}

	bool RaysEnabled()
	{
		SamplerEnabled();
		return g_raysOn;
	}


	bool HullEnabled()
	{
		SamplerEnabled();  // (re-reads nav.ini every 5 s)
		return g_hullOn;
	}

	void NoteHull(bool rayBlocked, bool hullBlocked)
	{
		++g_n.sweeps;
		if (rayBlocked && hullBlocked) ++g_n.hullBoth;
		else if (hullBlocked) ++g_n.hullOnly;
		else if (rayBlocked) ++g_n.rayOnly;
	}


	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		// Expiry, spread over the frames (2026-10-29 audit): the whole table (up to 60 000 cells) was walked in one frame every 5 s.
		// Now a slice of its buckets each frame, a full round about every 5 s: each cell is still checked once per ~5 s, as before.
		{
			static std::size_t bucket = 0;
			static std::vector<std::uint64_t> gone;
			const std::size_t n = g_cells.bucket_count();
			const std::size_t step = std::max<std::size_t>(1, n / 300);
			gone.clear();
			for (std::size_t k = 0; k < step; ++k, ++bucket) {
				if (bucket >= n) bucket = 0;
				for (auto it = g_cells.begin(bucket); it != g_cells.end(bucket); ++it) {
					const bool floorKept = Valid(it->second.floor) && now - it->second.floorAt < kFloorLifeMs;
					if (now - it->second.seen > (floorKept ? kFloorLifeMs : kCellLifeMs)) gone.push_back(it->first);
				}
			}
			for (const auto key : gone) g_cells.erase(key);
		}
		if (now - g_lastTick >= 5000) {
			g_lastTick = now;
			if (g_cells.size() > kMaxCells) {  // the oldest quarter goes
				std::vector<ULONGLONG> ages;
				ages.reserve(g_cells.size());
				for (const auto& [k, c] : g_cells) ages.push_back(c.seen);
				std::nth_element(ages.begin(), ages.begin() + ages.size() / 4, ages.end());
				const ULONGLONG cut = ages[ages.size() / 4];
				for (auto it = g_cells.begin(); it != g_cells.end();) {
					if (it->second.seen <= cut) it = g_cells.erase(it);
					else ++it;
				}
			}
		}
		if (now - g_lastLog >= 10000) {
			g_lastLog = now;
			const unsigned notes = g_n.ground + g_n.rays + g_n.contacts + g_n.sampUsed + g_n.sampFallback + g_n.sweeps;
			if (notes == 0) return;
			if (!logger::Verbose()) { g_n = Counters{}; return; }  // the counts below are for the detailed log only (2026-10-29 audit)
			unsigned ground = 0, floors = 0, blocked = 0, freeCells = 0;
			for (const auto& [k, c] : g_cells) {
				if (Valid(c.ground)) ++ground;
				if (Valid(c.floor) && now - c.floorAt < kFloorLifeMs) ++floors;
				if (Valid(c.blockedZ) && now - c.blockedAt < kBlockedLifeMs) ++blocked;
				if (Valid(c.freeLo)) ++freeCells;
			}
			if (logger::Verbose()) logger::Write("Nav map: %zu cells (ground %u, floors %u, solid %u, free %u); 10 s: %u ground, %u rays, %u contacts noted; "
				"ray hits with a unit vector at +0x10: %u of %u; sampler: used %u, fell back %u; straight way, hull sweep vs thin ray: %u sweeps, both blocked %u, hull only %u, ray only %u",
				g_cells.size(), ground, floors, blocked, freeCells, g_n.ground, g_n.rays, g_n.contacts,
				g_n.normals, g_n.hits, g_n.sampUsed, g_n.sampFallback, g_n.sweeps, g_n.hullBoth, g_n.hullOnly, g_n.rayOnly);
			g_n = Counters{};
		}
	}

	void Reset()
	{
		g_cells.clear();
		g_boxes.clear();
		g_seedAt = 0;
		g_n = Counters{};
		g_lastTick = g_lastLog = 0;
	}
}
