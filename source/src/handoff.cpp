#include "handoff.h"

#include "addresses.h"
#include "anchor.h"
#include "content.h"
#include "game.h"
#include "hexgrid.h"
#include "log.h"
#include "notify.h"
#include "recentre.h"
#include "sphere.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace handoff
{
	namespace
	{
		// Zones: the user's cube + star design on a geodesic (subdivided icosahedron) lattice covering the whole
		// planet sphere. Target rhombus side 3.6 km: a cube zone (~7.2 km across) is about Bethesda's walled
		// landing area (~7.9 km square). The real side is the icosahedron edge divided evenly, so it varies a
		// little per planet.
		// 0.57.0: a zone is one engine landing area (the generation unit): its side is the playable width the game
		// generates per landing ((uBlockSize + uBlockMergeZoneSize) x 8 cells, capped by uMaximumPlayableAreaWorldCoord;
		// 8 km on Mars), so each zone gets vanilla content density and a hand-built site (Cydonia spans ~3.1 km) is not
		// split between zones. Was 3.6 km. The move (shift) distances stay in metres: they follow the engine's fixed
		// landing-centred radii, not the zone size.
		constexpr double kFallbackSideMetres = 3600.0;
		double TargetSideMetres()
		{
			const auto l = game::ReadLimits();
			const double w = 2.0 * (double(l.borderMetres) + 50.0);
			return w >= 2000.0 ? w : kFallbackSideMetres;
		}
		constexpr double kCommitSeconds = 3.0;      // time deeper in another zone before it takes over
		// One system (user): the zone is the physics region and the spawning region. The primary zone's centre is the
		// world origin, the physics box (half-extent > zone + overlap band) is centred on it, and the zone handoff (cube +
		// star overlap, deepest cell, commit timer / edge rule) is the moment the world re-centres onto the new zone.
		// Prediction from speed: the edge rule looks kLookAheadSeconds ahead along the velocity, and the time to the
		// physics edge at the current velocity sets a deadline: under kEdgeDeadlineSeconds the handoff is forced onto
		// the zone ahead and the re-centre is urgent.
		constexpr double kLookAheadSeconds = 6.0;    // edge rule: this far ahead at this velocity, so the overlap band grows with speed (time for the settle pause)
		constexpr double kMinOverlapMetres = 1000.0;  // the overlap band: 1 km at walking speed (user: overlap in km); 0.57.0: at least 0.28 x side
		constexpr double kMaxOverlapMetres = 3000.0;  // ... growing with the hysteresis speed up to this
		constexpr double kAheadCommitSeconds = 2.0;   // the zone ahead must stay the same this long before the handoff
		// Hysteresis between zones: a zone takes over only where it is clearly deeper than the current one. At a corner
		// where a star and a cube zone meet (Akila) the deepest zone alternated every 2-3 s and each flip asked for a
		// re-centre (0.35.5).
		constexpr double kSwitchMarginMetres = 400.0;  // 0.57.0: at least 0.11 x side (MarginFor)
		// After a handoff the zone holds this long (only the physics deadline overrides): the "zone ahead" rule and the
		// "time deeper inside" rule disagreed near a corner and alternated every 3 s (0.36.0).
		constexpr double kHoldSeconds = 20.0;
		constexpr double kEdgeDeadlineSeconds = 45.0;  // was 25 s: at 100 m/s the urgent re-centre came too late (0.36.2)
		constexpr ULONGLONG kSettleMs = 5000;       // world must be settled this long first

		// Physics box half-extent in 50 m units. 2048 (102.4 km) is the eventual size; the re-centring is being made to
		// work at the game's own 128 (6.4 km) first (user).
		// 256 = +-12.8 km (stock 128 = +-6.4 km): twice the stock box while testing, re-centre past ~6.4 km
		// (kUrgentReachMetres), reachable in about a minute at speedmult 1500. Target later: 2048.
		constexpr std::int32_t kPhysicsExtents = 256;
		constexpr float kEdgeWarnMetres = kPhysicsExtents * 50.0f - 500.0f;
		// Beyond this distance from the origin a re-centre is urgent whatever the velocity (half the physics half-extent).
		constexpr float kUrgentReachMetres = kPhysicsExtents * 50.0f * 0.5f;

		// 0.55.0 move schedule (ratios of the rhombus side S = 3.6 km): planned at 0.67 S predicted 6 s ahead, or at a
		// committed zone handoff past 0.44 S; urgent at 0.89 S predicted 3 s ahead (under content 3.2 km / play area
		// 3.8 km; 3 s covers the shift and the near collision rebuild). The physics deadline stays as the backstop.
		constexpr double kPlanMetres = 2400.0, kPlanLeadSeconds = 6.0;
		constexpr double kHandoffMoveMetres = 1600.0;
		constexpr double kMoveUrgentMetres = 3200.0, kMoveUrgentLeadSeconds = 3.0;

		const hexgrid::Lattice kUnitLattice{ 1.0 };  // lattice coordinates from sphere::Locate (side = 1)

		struct ZoneId
		{
			int face = -1;
			hexgrid::CellId cell{};
			bool operator==(const ZoneId& o) const { return face == o.face && cell == o.cell; }
			bool operator!=(const ZoneId& o) const { return !(*this == o); }
		};

		std::uint64_t ZoneKey(const ZoneId& z)
		{
			return (std::uint64_t(std::uint8_t(z.face)) << 56) | (std::uint64_t(std::uint32_t(z.cell.i) & 0xFFFFFFF) << 28) |
				std::uint64_t(std::uint32_t(z.cell.j) & 0xFFFFFFF);
		}

		struct Planet
		{
			std::uint32_t id = 0;
			double radius = 0;     // metres
			int frequency = 0;     // lattice subdivisions per icosahedron edge
			double side = 0;       // real rhombus side, metres
		};

		struct State
		{
			bool       active = false;
			ZoneId     zone{};
			double     dwell = 0.0;
			ULONGLONG  settledSince = 0;
			game::Vec3 lastPos{};
			ULONGLONG  lastTime = 0;
			Planet     planet{};
			// Speed with hysteresis (user): the velocity is smoothed (~1 s); the speed used for the overlap rises over
			// ~1.5 s and falls over ~10 s, so it stays near constant and only grows while the player keeps speeding up.
			double     svx = 0.0, svy = 0.0;
			double     hspeed = 0.0;
			ZoneId     ahead{};             // zone at the overlap look-ahead point
			double     aheadDwell = 0.0;
			// Zone centre cache: one search per zone and world frame.
			std::uint64_t cachedZone = ~0ull;
			std::int32_t cachedShiftX = 0, cachedShiftY = 0, cachedCx = 0, cachedCy = 0;
			bool       cachedOk = false;
		} g;

		// The overlap band's floor scales with the zone (0.28 x side, the 1 km / 3.6 km ratio), within the 3 km cap.
		double MinOverlap() { return std::min(kMaxOverlapMetres, std::max(kMinOverlapMetres, 0.28 * g.planet.side)); }

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		// The landing-area wall is off everywhere, hand-built worldspaces included (the user's decision).
		void KeepWallOff()
		{
			auto& border = At<std::uint8_t>(game::Base(), addr::kBorderRegionsEnabled);
			if (border != 0) {
				border = 0;
				logger::Write("Zones: landing wall disabled");
				telemetry::Record("wall", "\"on\":0");
			}
		}

		// The engine's world position -> planet latitude/longitude (radians), as the planet globe uses it.
		void LatLong(std::uintptr_t mgr, float x, float y, float z, double& lat, double& lon)
		{
			double out[2] = {};
			const float p[3] = { x, y, z };
			EngineFn<void* (*)(std::uintptr_t, double*, const float*)>(addr::kWorldToLatLong)(mgr, out, p);
			lon = out[0];
			lat = out[1];
		}

		// Planet size from the engine's own conversion: the angle covered by a 1 km step.
		bool MeasurePlanet(std::uintptr_t mgr, const game::Vec3& pos)
		{
			const auto id = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			if (g.planet.id == id && g.planet.radius > 0) {
				return true;
			}
			double lat0, lon0, lat1, lon1, lat2, lon2;
			LatLong(mgr, pos.x, pos.y, pos.z, lat0, lon0);
			LatLong(mgr, pos.x + 1000.0f, pos.y, pos.z, lat1, lon1);
			LatLong(mgr, pos.x, pos.y + 1000.0f, pos.z, lat2, lon2);
			const double a = 0.5 * (sphere::Angle(lat0, lon0, lat1, lon1) + sphere::Angle(lat0, lon0, lat2, lon2));
			if (!(a > 1e-9)) {
				return false;
			}
			Planet p;
			p.id = id;
			p.radius = 1000.0 / a;
			p.frequency = std::max(1, static_cast<int>(std::lround(sphere::EdgeAngle() * p.radius / TargetSideMetres())));
			p.side = sphere::EdgeAngle() * p.radius / p.frequency;
			g.planet = p;
			const double zones = 10.0 * p.frequency * p.frequency + 2.0;  // geodesic lattice points
			logger::Write("Zones: planet %08X radius %.0f km, circumference %.0f km, %d subdivisions, zone side %.2f km, ~%.0f zones",
				id, p.radius / 1000.0, 2.0 * 3.14159265 * p.radius / 1000.0, p.frequency, p.side / 1000.0, zones);
			telemetry::Record("planet_geometry", "\"planet\":\"%08X\",\"radius_m\":%.0f,\"frequency\":%d,\"side_m\":%.0f,\"zones\":%.0f",
				id, p.radius, p.frequency, p.side, zones);
			notify::Log("Planet: radius %.0f km, %.0f km around", p.radius / 1000.0, 2.0 * 3.14159265 * p.radius / 1000.0);
			return true;
		}

		struct Located
		{
			ZoneId zone;
			sphere::FacePoint point;
		};

		Located Locate(std::uintptr_t mgr, float x, float y, float z)
		{
			double lat, lon;
			LatLong(mgr, x, y, z, lat, lon);
			const auto fp = sphere::Locate(lat, lon, g.planet.frequency);
			return { { fp.face, kUnitLattice.Deepest(fp.x, fp.y, nullptr) }, fp };
		}

		const char* Kind(const ZoneId& z) { return z.cell.IsStar() ? "star" : "cube"; }

		// The point is clearly inside `to` compared with `from`: deeper by kSwitchMarginMetres (another face: yes).
		bool ClearlyIn(const ZoneId& to, const ZoneId& from, const sphere::FacePoint& p, double side)
		{
			if (p.face != from.face || p.face != to.face || side <= 0) {
				return true;
			}
			const double gain = (kUnitLattice.Depth(to.cell, p.x, p.y) - kUnitLattice.Depth(from.cell, p.x, p.y)) * side;
			return gain >= std::max(kSwitchMarginMetres, 0.11 * side);
		}

		// The zone centre as a cell offset from the current world origin. Found with the engine's world -> latitude/
		// longitude conversion only (the one the zones use all the time): Newton steps from the player's position until
		// the point's latitude/longitude is the zone centre's. (0.31.0 used the planet grid coordinate functions with a
		// computed latitude/longitude; memory corruption followed at zone start, so they are not used.)
		bool CentreCells(std::uintptr_t mgr, const ZoneId& z, const game::Vec3& from, std::int32_t& cx, std::int32_t& cy)
		{
			double x = 0, y = 0;
			kUnitLattice.CentreOf(z.cell, x, y);
			double tlat = 0, tlon = 0;
			sphere::ToLatLong({ z.face, x, y }, g.planet.frequency, tlat, tlon);
			double px = from.x, py = from.y;
			const auto wrap = [](double a) {
				while (a > 3.14159265358979) a -= 6.28318530717959;
				while (a < -3.14159265358979) a += 6.28318530717959;
				return a;
			};
			// The engine's conversion is not smooth at short range (the zones measure the planet with 1 km steps), so the
			// slopes use 1 km steps and the search stops within half a cell (the re-centre snaps to whole cells).
			constexpr double kStep = 1000.0, kTolerance = 100.0;
			double la0, lo0;
			LatLong(mgr, float(px), float(py), from.z, la0, lo0);
			const double startKm = sphere::Angle(la0, lo0, tlat, tlon) * g.planet.radius / 1000.0;  // should be a few km
			bool converged = false;
			double lastMove = 0.0;
			for (int it = 0; it < 12; ++it) {
				double la, lo, lax, lox, lay, loy;
				LatLong(mgr, float(px), float(py), from.z, la, lo);
				LatLong(mgr, float(px + kStep), float(py), from.z, lax, lox);
				LatLong(mgr, float(px), float(py + kStep), from.z, lay, loy);
				const double a = (lax - la) / kStep, b = (lay - la) / kStep;              // dlat/dx, dlat/dy
				const double c = wrap(lox - lo) / kStep, d = wrap(loy - lo) / kStep;      // dlon/dx, dlon/dy
				const double det = a * d - b * c;
				if (!(std::fabs(det) > 1e-30)) {
					return false;
				}
				const double rlat = tlat - la, rlon = wrap(tlon - lo);
				const double dx = (d * rlat - b * rlon) / det, dy = (-c * rlat + a * rlon) / det;
				px += std::clamp(dx, -5000.0, 5000.0);
				py += std::clamp(dy, -5000.0, 5000.0);
				lastMove = std::hypot(dx, dy);
				if (lastMove < kTolerance) {
					converged = true;
					break;
				}
			}
			static ULONGLONG lastFailLog = 0;
			if (!converged) {
				if (GetTickCount64() - lastFailLog > 5000) {
					lastFailLog = GetTickCount64();
					logger::Write("Zones: zone centre search did not settle (start %.1f km away, last step %.0f m, at %.0f, %.0f); no re-centre", startKm,
						lastMove, px, py);
				}
				return false;
			}
			static ULONGLONG lastOkLog = 0;
			if (GetTickCount64() - lastOkLog > 5000) {
				lastOkLog = GetTickCount64();
				logger::Write("Zones: zone centre %d:(%d, %d) at (%.0f, %.0f) m in the current frame (start %.1f km away, last step %.1f m)", z.face,
					z.cell.i, z.cell.j, px, py, startKm, lastMove);
			}
			cx = std::int32_t(std::lround(px / off::kCellSize));
			cy = std::int32_t(std::lround(py / off::kCellSize));
			if (std::abs(cx) > 400 || std::abs(cy) > 400) {
				logger::Write("Zones: zone centre offset (%d, %d) cells is implausible; no re-centre", cx, cy);
				return false;
			}
			return true;
		}

		void CentreOnZone(std::uintptr_t mgr, const ZoneId& z, const game::Vec3& from, bool urgent)
		{
			std::int32_t sx = 0, sy = 0;
			recentre::TotalShift(sx, sy);
			const auto key = ZoneKey(z);
			if (g.cachedZone != key || g.cachedShiftX != sx || g.cachedShiftY != sy) {
				g.cachedZone = key;
				g.cachedShiftX = sx;
				g.cachedShiftY = sy;
				g.cachedOk = CentreCells(mgr, z, from, g.cachedCx, g.cachedCy);
			}
			if (g.cachedOk) {
				recentre::RequestCentre(g.cachedCx, g.cachedCy, urgent);
			}
		}

		// Seconds until the player crosses the physics box edge at the current velocity (1e9 if not heading out).
		double TimeToPhysicsEdge(const game::Vec3& pos, float vx, float vy)
		{
			const double half = kPhysicsExtents * 50.0;
			double t = 1e9;
			if (vx > 0.5f) t = std::min(t, (half - pos.x) / vx);
			if (vx < -0.5f) t = std::min(t, (-half - pos.x) / vx);
			if (vy > 0.5f) t = std::min(t, (half - pos.y) / vy);
			if (vy < -0.5f) t = std::min(t, (-half - pos.y) / vy);
			return std::max(0.0, t);
		}
	}

	float PhysicsHalfExtent() { return kPhysicsExtents * 50.0f; }

	void EnlargePhysicsWorld()
	{
		// iNumCellWorldExtents:Havok is read once, when a physics world is created (FUN_140b06d70: half-extent =
		// value * 50 m on every axis). Default 128 = 6.4 km, the real reason for the landing wall.
		auto& extents = At<std::int32_t>(game::Base(), addr::kNumCellWorldExtents);
		if (extents != kPhysicsExtents) {
			logger::Write("Zones: physics world extents %d -> %d (half-extent %.1f km)", extents, kPhysicsExtents, kPhysicsExtents * 0.05);
			telemetry::Record("physics_extents", "\"from\":%d,\"to\":%d", extents, kPhysicsExtents);
			extents = kPhysicsExtents;
		}
	}

	namespace
	{
		// Phase 1 of the zone-overlap handoff: the switch itself, on a key so it can be tested anywhere.
		// F8 (game window focused): translate the world so the current terrain centre becomes the origin.
		void ManualSwitch(std::uintptr_t mgr)
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
			static bool wasDown = false;
			const bool pressed = down && !wasDown;
			wasDown = down;
			if (!pressed) {
				return;
			}
			if (!anchor::WorldSettled()) {
				notify::Show("Zone switch: cells still loading, try again in a moment");
				telemetry::Record("switch_manual", "\"result\":\"not settled\"");
				return;
			}
			const auto cx = At<std::int32_t>(mgr, off::kMgrGridCentre);
			const auto cy = At<std::int32_t>(mgr, off::kMgrGridCentre + 4);
			if (cx == 0 && cy == 0) {
				notify::Show("Zone switch: already centred (walk a few hundred metres first)");
				return;
			}
			const bool engineTerrain = (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0;
			recentre::SetEngineTerrainShift(engineTerrain);
			const bool ok = recentre::Run(cx, cy);
			notify::Show(ok ? "Zone switch: done (%d, %d cells, %s)" : "Zone switch: refused (%d, %d, %s)", cx, cy, engineTerrain ? "engine terrain" : "bookkeeping only");
			telemetry::Record("switch_manual", "\"result\":\"%s\",\"cx\":%d,\"cy\":%d,\"engine_terrain\":%d", ok ? "ok" : "refused", cx, cy, engineTerrain ? 1 : 0);
		}
	}

	bool ZoneKeyAt(float x, float y, float z, std::uint64_t& key)
	{
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		if (!mgr || g.planet.radius <= 0 || At<std::uint32_t>(mgr, off::kMgrPlanetId) != g.planet.id) {
			return false;
		}
		key = ZoneKey(Locate(mgr, x, y, z).zone);
		return true;
	}

	double ZoneSide() { return g.planet.side; }

	double PlanetRadius() { return g.planet.radius; }

	bool ZoneKeyAtLatLong(double lat, double lon, std::uint64_t& key)
	{
		if (g.planet.radius <= 0) {
			return false;
		}
		const auto fp = sphere::Locate(lat, lon, g.planet.frequency);
		key = ZoneKey({ fp.face, kUnitLattice.Deepest(fp.x, fp.y, nullptr) });
		return true;
	}

	void Reset()
	{
		g.active = false;
		g.settledSince = 0;
		g.dwell = 0.0;
		g.lastTime = 0;
	}

	void Tick()
	{
		KeepWallOff();
		EnlargePhysicsWorld();

		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const auto a = anchor::Current();
		const ULONGLONG now = GetTickCount64();
		if (!mgr || !a.ref || At<std::uint8_t>(mgr, off::kMgrState) != 2) {
			g.settledSince = 0;
			g.lastTime = 0;
			return;
		}
		// ManualSwitch(mgr);  // F8 disabled: the switch collided with in-flight cell loads (crash); needs a
		//                      loader-idle check first. Zone handoffs move to seam re-basing.
		const auto pos = At<game::Vec3>(a.ref, off::kRefPosition);
		const double dt = g.lastTime ? (now - g.lastTime) / 1000.0 : 0.0;
		const bool jumped = std::hypot(pos.x - g.lastPos.x, pos.y - g.lastPos.y) > 200.0f;  // teleport
		if (dt > 0 && !jumped) {
			const double rvx = (pos.x - g.lastPos.x) / dt, rvy = (pos.y - g.lastPos.y) / dt;
			const double kv = std::min(1.0, dt / 1.0);
			g.svx += (rvx - g.svx) * kv;
			g.svy += (rvy - g.svy) * kv;
			const double sp = std::hypot(g.svx, g.svy);
			const double tau = sp > g.hspeed ? 1.5 : 10.0;  // hysteresis: quick to rise, slow to fall
			g.hspeed += (sp - g.hspeed) * std::min(1.0, dt / tau);
		}
		else if (jumped) {
			g.svx = g.svy = 0.0;  // teleport / re-centre: keep the speed level, drop the direction
		}
		// Velocity used for prediction: the smoothed direction at the hysteresis speed.
		const double sl = std::hypot(g.svx, g.svy);
		const float vx = sl > 0.5 ? float(g.svx / sl * g.hspeed) : 0.0f;
		const float vy = sl > 0.5 ? float(g.svy / sl * g.hspeed) : 0.0f;
		g.lastPos = pos;
		g.lastTime = now;

		// Physics-box edge warning.
		const float reach = std::max(std::fabs(pos.x), std::fabs(pos.y));
		if (reach > kEdgeWarnMetres) {
			static ULONGLONG lastWarn = 0;
			if (now - lastWarn > 10000) {
				lastWarn = now;
				notify::Log("Zone edge: %.1f km from the landing origin (physics ends at %.1f km)", reach / 1000.0f, kPhysicsExtents * 0.05f);
				telemetry::Record("physics_edge_warning", "\"reach\":%.0f", reach);
			}
		}

		if (!g.active) {
			if (!anchor::WorldSettled() || jumped) {
				g.settledSince = 0;
				return;
			}
			if (g.settledSince == 0) {
				g.settledSince = now;
			}
			if (now - g.settledSince < kSettleMs) {
				return;
			}
		}
		if (!MeasurePlanet(mgr, pos)) {
			return;
		}

		auto here = Locate(mgr, pos.x, pos.y, pos.z);
		if (!g.active || At<std::uint32_t>(mgr, off::kMgrPlanetId) != g.planet.id) {
			g.active = true;
			g.zone = here.zone;
			g.dwell = 0.0;
			logger::Write("Zones: in %s zone %d:(%d, %d)", Kind(here.zone), here.zone.face, here.zone.cell.i, here.zone.cell.j);
			notify::Log("Zone: %s %d:(%d, %d)", Kind(here.zone), here.zone.face, here.zone.cell.i, here.zone.cell.j);
			telemetry::Record("zone_enter", "\"face\":%d,\"zone\":[%d,%d],\"star\":%d,\"why\":\"start\"",
				here.zone.face, here.zone.cell.i, here.zone.cell.j, here.zone.cell.IsStar() ? 1 : 0);
			content::OnZoneEnter(ZoneKey(here.zone), pos.x, pos.y, pos.z, vx, vy, true);
			// No re-centre at the zone start (after a load): the save anchoring already centred the world on the player's
			// area, and a shift right after a load collided with the load's own setup and the content post-steps (0.32.1:
			// the next cell never became ready, the game rebuilt the planet in the old frame). Handoffs re-centre.
			return;
		}

		// Just after a re-centre the world -> lat/long answer is briefly wrong (0.32.3: a zone 7.6 km away for 0.5 s):
		// no handoff for 3 s.
		if (recentre::LastRunMs() && now - recentre::LastRunMs() < 3000) {
			g.dwell = 0.0;
			return;
		}
		// Zone ownership: commit timer (time spent deeper inside another zone) or edge rule (the current zone will
		// not contain the path ahead, measured on the same lattice face).
		g.dwell = (here.zone != g.zone) ? g.dwell + dt : std::fmax(0.0, g.dwell - dt);
		bool edge = false;
		if (here.zone != g.zone) {
			// The overlap band: kLookAheadSeconds of travel at the hysteresis speed, at least kMinOverlapMetres.
			const double sp = std::hypot(vx, vy);
			const double reachAhead = std::clamp(sp * kLookAheadSeconds, MinOverlap(), kMaxOverlapMetres);
			const float ux = sp > 0.5 ? float(vx / sp) : 0.0f, uy = sp > 0.5 ? float(vy / sp) : 0.0f;
			const auto ahead = Locate(mgr, pos.x + ux * float(reachAhead), pos.y + uy * float(reachAhead), pos.z);
			edge = ahead.point.face != g.zone.face || kUnitLattice.Depth(g.zone.cell, ahead.point.x, ahead.point.y) <= 0.0;
		}
		// Overlap in km: the zone 1-3 km ahead along the smoothed direction (at the hysteresis speed) takes over once it
		// has stayed the same for kAheadCommitSeconds, before the player reaches it.
		bool aheadHandoff = false;
		if (std::hypot(vx, vy) > 2.0f) {
			const double sp2 = std::hypot(vx, vy);
			const double reach2 = std::clamp(sp2 * kLookAheadSeconds, MinOverlap(), kMaxOverlapMetres);
			const auto al = Locate(mgr, pos.x + float(vx / sp2 * reach2), pos.y + float(vy / sp2 * reach2), pos.z);
			const auto az = al.zone;
			if (az == g.ahead) {
				g.aheadDwell += dt;
			}
			else {
				g.ahead = az;
				g.aheadDwell = 0.0;
			}
			aheadHandoff = g.ahead != g.zone && g.aheadDwell >= kAheadCommitSeconds && ClearlyIn(g.ahead, g.zone, al.point, g.planet.side);
		}
		else {
			g.aheadDwell = 0.0;
		}
		// Deadline from speed: if the physics edge would be reached soon, hand off now onto the zone the player is
		// heading into (predicted point), and make the re-centre urgent.
		const double tEdge = TimeToPhysicsEdge(pos, vx, vy);
		bool deadline = false;
		ZoneId target = here.zone;
		if (tEdge < kEdgeDeadlineSeconds) {
			const double look = std::min(tEdge, kLookAheadSeconds);
			target = Locate(mgr, pos.x + vx * float(look), pos.y + vy * float(look), pos.z).zone;
			deadline = target != g.zone || recentre::Pending();
			if (target == g.zone) {
				// Still the same zone but the physics edge is near (the origin is not on this zone yet): centre on it now.
				CentreOnZone(mgr, g.zone, pos, true);
			}
		}
		// 0.55.0 predictive move schedule (rhombus side S = 3.6 km; notes/decomp/TRAVERSAL_STAGE_SCHEDULE.md). Every move
		// now also carries the landing lat/long records (recentre FollowLandingRecords), so the landing frame stays within
		// the engine's landing-centred radii (content 3.2 km, play area 3.8 km). Predicted reach = reach + speed x lead.
		{
			const double sp = std::hypot(vx, vy);
			const double planReach = reach + sp * kPlanLeadSeconds, urgentReach = reach + sp * kMoveUrgentLeadSeconds;
			if (!deadline && (urgentReach >= kMoveUrgentMetres || reach > kUrgentReachMetres)) {
				CentreOnZone(mgr, g.zone, pos, true);   // urgent (also upgrades a waiting planned request)
			}
			else if (!deadline && !recentre::Pending()) {
				if (planReach >= kPlanMetres) {
					CentreOnZone(mgr, g.zone, pos, false);  // planned: at the first quiet window
				}
			}
		}
		const bool clearlyHere = here.zone != g.zone && ClearlyIn(here.zone, g.zone, here.point, g.planet.side);
		static ULONGLONG lastHandoff = 0;
		const bool holding = lastHandoff && double(now - lastHandoff) / 1000.0 < kHoldSeconds;
		// Heading into the current zone (it is the zone ahead): standing deeper in the old one does not take it back.
		const bool headingIn = std::hypot(vx, vy) > 2.0f && g.ahead == g.zone;
		const bool timerSwitch = clearlyHere && (g.dwell >= kCommitSeconds || edge) && !headingIn;
		if ((!holding && (timerSwitch || aheadHandoff)) || (deadline && target != g.zone)) {
			lastHandoff = now;
			const bool urgent = deadline && target != g.zone;
			const char* why = urgent ? "physics deadline" : (aheadHandoff ? "overlap ahead" : (edge ? "edge" : "timer"));
			if (urgent) {
				here.zone = target;
			}
			else if (aheadHandoff) {
				here.zone = g.ahead;
			}
			g.aheadDwell = 0.0;
			g.zone = here.zone;
			g.dwell = 0.0;
			logger::Write("Zones: entered %s zone %d:(%d, %d) [%s], %.1f km from the landing origin", Kind(here.zone), here.zone.face,
				here.zone.cell.i, here.zone.cell.j, why, reach / 1000.0f);
			notify::Log("Zone: %s %d:(%d, %d)", Kind(here.zone), here.zone.face, here.zone.cell.i, here.zone.cell.j);
			telemetry::Record("zone_enter", "\"face\":%d,\"zone\":[%d,%d],\"star\":%d,\"why\":\"%s\",\"reach\":%.0f",
				here.zone.face, here.zone.cell.i, here.zone.cell.j, here.zone.cell.IsStar() ? 1 : 0, why, reach);
			content::OnZoneEnter(ZoneKey(here.zone), pos.x, pos.y, pos.z, vx, vy, false);
			// Zone changes and physics re-centres are separate (middle path): entering a zone switches content only. The
			// world re-centres only when the physics box needs it (the deadline above, or past kUrgentReachMetres).
			if (urgent) {
				CentreOnZone(mgr, here.zone, pos, true);
			}
			else if (aheadHandoff && reach >= kHandoffMoveMetres && !recentre::Pending()) {
				CentreOnZone(mgr, here.zone, pos, false);  // 0.55.0: a committed zone ahead plans the move early
			}
		}
	}
}
