#include "navmap.h"

#include "log.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
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

		// Collision layers whose hits are remembered as obstacles: static things that stay where they are. Characters
		// (8 biped, 30 character controller, 33) and debris are not.
		constexpr std::uint64_t kPersistentLayers = (1ull << 1) | (1ull << 2) | (1ull << 9) | (1ull << 10) | (1ull << 13) | (1ull << 17) | (1ull << 35);

		struct Cell
		{
			float ground = kNaN, floor = kNaN, blockedZ = kNaN, freeLo = kNaN, freeHi = kNaN;
			ULONGLONG seen = 0, groundAt = 0, floorAt = 0, blockedAt = 0, freeAt = 0, contactAt = 0;
			std::uint8_t contacts = 0;
		};

		std::unordered_map<std::uint64_t, Cell> g_cells;
		ULONGLONG g_lastTick = 0, g_lastLog = 0;

		struct Counters
		{
			unsigned ground = 0, rays = 0, contacts = 0, hits = 0, normals = 0, sampUsed = 0, sampFallback = 0,
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

	void NoteGround(double x, double y, double h)
	{
		if (!Finite(x) || !Finite(y) || !Finite(h)) return;
		const ULONGLONG now = GetTickCount64();
		Cell& c = Make(x, y);
		c.ground = float(h);
		c.groundAt = c.seen = now;
		++g_n.ground;
	}

	void NoteRay(const float from[3], const float to[3], bool hit, float hx, float hy, float hz, std::uint32_t layer, bool normalOk)
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
			MarkFree(Make(from[0] + dx * t, from[1] + dy * t), float(from[2] + dz * t), now);
		}
		if (usable) {
			++g_n.hits;
			if (normalOk) ++g_n.normals;
		}
		if (usable && layer < 64 && ((kPersistentLayers >> layer) & 1ull)) {
			Cell& c = Make(hx, hy);
			c.blockedZ = hz;
			c.blockedAt = c.seen = now;
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

	Sample At(double x, double y, double z)
	{
		Sample s;
		const Cell* c = Find(x, y);
		if (!c) return s;
		const ULONGLONG now = GetTickCount64();
		if (Valid(c->ground) && now - c->groundAt < kCellLifeMs) {
			s.haveGround = true;
			s.ground = c->ground;
		}
		if (Valid(c->floor) && now - c->floorAt < kFloorLifeMs) {
			s.ground = s.haveGround ? std::max(s.ground, c->floor) : c->floor;
			s.haveGround = true;
		}
		s.blocked = Valid(c->blockedZ) && now - c->blockedAt < kBlockedLifeMs && std::fabs(z - c->blockedZ) <= kBlockedBand;
		s.free = Valid(c->freeLo) && now - c->freeAt < kCellLifeMs && z >= c->freeLo - 2.0 && z <= c->freeHi + 2.0;
		if (now - c->contactAt < kContactLifeMs) s.contacts = c->contacts;
		s.known = s.haveGround || s.blocked || s.free || s.contacts > 0;
		return s;
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

	HeadingChoice ChooseHeading(const HeadingQuery& q)
	{
		HeadingChoice best;
		best.score = -1e9;
		const double kPi = 3.14159265358979;
		const double len = std::clamp(q.speed * 3.0, 40.0, 80.0);  // at least the corridors' own reach (40 m)
		const double minFree = std::max(8.0, q.speed);
		// The candidates are the sweep ring's own 16 compass directions (22.5 degrees apart) within 120 degrees of the
		// bearing: the sweep rays run exactly along them, so the map has data on those lines (a thin ray knows nothing
		// between its lines; the hull-sized shape sweep will, later).
		for (int i = 0; i < 16; ++i) {
			const double hd = i * (2.0 * kPi / 16.0);
			double off = hd - q.bearing;
			while (off > kPi) off -= 2.0 * kPi;
			while (off < -kPi) off += 2.0 * kPi;
			if (std::fabs(off) > 2.0944) continue;
			if (q.haveAvoid && std::fabs(off - q.avoidOffset) < 0.2) continue;
			double at = len;
			const Verdict v = PathClear(q.pos, std::sin(hd) * len, std::cos(hd) * len, q.halfWidth, q.clearance, q.climbRate, q.speed, &at, nullptr);
			if (v == Verdict::Unknown) { ++best.unknown; continue; }
			double freeDist = len;
			if (v == Verdict::Blocked) {
				if (at < minFree) { ++best.blocked; continue; }
				freeDist = at;
			}
			++best.usable;
			const double score = std::cos(off) + 0.8 * (freeDist / len) - (v == Verdict::Blocked ? 0.5 : 0.0) - 0.5 * std::fabs(off - q.currentOffset) / kPi;
			if (score > best.score) {
				best.score = score;
				best.offset = off;
				best.freeDist = freeDist;
				best.found = true;
			}
		}
		return best;
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
					g_iniPath = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\nav.ini";
					CoTaskMemFree(docs);
				}
				else {
					g_iniPath = L"-";
				}
			}
			if (g_iniPath == L"-") return;
			const bool on = GetPrivateProfileIntW(L"nav", L"sampler", 1, g_iniPath.c_str()) != 0;
			if (on != g_samplerOn) logger::Write("Nav map: heading sampler %s (nav.ini)", on ? "ON" : "OFF");
			g_samplerOn = on;
			const bool hull = GetPrivateProfileIntW(L"nav", L"shape", 1, g_iniPath.c_str()) != 0;
			if (hull != g_hullOn) logger::Write("Nav map: hull sweeps %s (nav.ini)", hull ? "ON" : "OFF");
			g_hullOn = hull;
			const bool rays = GetPrivateProfileIntW(L"nav", L"rays", 1, g_iniPath.c_str()) != 0;
			if (rays != g_raysOn) logger::Write("Nav map: engine ray and sweep queries %s (nav.ini)", rays ? "ON" : "OFF");
			g_raysOn = rays;
		}
	}

	bool SamplerEnabled()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_samplerRead >= 5000) {
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

	void NoteSampler(bool used)
	{
		if (used) ++g_n.sampUsed;
		else ++g_n.sampFallback;
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_lastTick >= 5000) {
			g_lastTick = now;
			for (auto it = g_cells.begin(); it != g_cells.end();) {
				const bool floorKept = Valid(it->second.floor) && now - it->second.floorAt < kFloorLifeMs;
				if (now - it->second.seen > (floorKept ? kFloorLifeMs : kCellLifeMs)) it = g_cells.erase(it);
				else ++it;
			}
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
			unsigned ground = 0, floors = 0, blocked = 0, freeCells = 0;
			for (const auto& [k, c] : g_cells) {
				if (Valid(c.ground)) ++ground;
				if (Valid(c.floor) && now - c.floorAt < kFloorLifeMs) ++floors;
				if (Valid(c.blockedZ) && now - c.blockedAt < kBlockedLifeMs) ++blocked;
				if (Valid(c.freeLo)) ++freeCells;
			}
			logger::Write("Nav map: %zu cells (ground %u, floors %u, solid %u, free %u); 10 s: %u ground, %u rays, %u contacts noted; "
				"ray hits with a unit vector at +0x10: %u of %u; sampler: used %u, fell back %u; straight way, hull sweep vs thin ray: %u sweeps, both blocked %u, hull only %u, ray only %u",
				g_cells.size(), ground, floors, blocked, freeCells, g_n.ground, g_n.rays, g_n.contacts,
				g_n.normals, g_n.hits, g_n.sampUsed, g_n.sampFallback, g_n.sweeps, g_n.hullBoth, g_n.hullOnly, g_n.rayOnly);
			g_n = Counters{};
		}
	}

	void Reset()
	{
		g_cells.clear();
		g_n = Counters{};
		g_lastTick = g_lastLog = 0;
	}
}
