// Structural architecture tests (2026-11-23):
// Verifies semantic collider radius clamp, SimTier hysteresis, FrameContext O(1) lookup,
// FrameBudget governor with starvation prevention, and pre-baked uniform-arclength lane tables.

#include "game.h"
#include "nodeflow.h"
#include "simtier.h"
#include "trafficlook.h"
#include "trafficroute.h"

#include <cmath>
#include <cstdio>

namespace {
int g_fail = 0;
void Check(bool ok, const char* name) {
	if (ok) std::printf("PASS %s\n", name);
	else { std::printf("FAIL %s\n", name); ++g_fail; }
}
}

int main() {
	// 1. Semantic unit clamp: 6400 m Havok broadphase can NEVER produce > 160 m loaded collider radius.
	Check(std::fabs(game::ClampLoadedColliderMetres(6400.0f) - 160.0f) < 1e-4f,
		"semantic clamp caps 6400 m broadphase to 160 m collider radius");
	Check(std::fabs(game::ClampLoadedColliderMetres(0.0f) - 160.0f) < 1e-4f,
		"semantic clamp defaults unknown 0 m to 160 m");
	Check(std::fabs(game::ClampLoadedColliderMetres(120.0f) - 120.0f) < 1e-4f,
		"semantic clamp preserves valid 120 m radius");
	Check(std::fabs(game::ClampLoadedColliderMetres(30.0f) - 80.0f) < 1e-4f,
		"semantic clamp floors tiny radius to 80 m");

	// 2. SimTier hysteresis: < 140 m NearPhysical, > 160 m FarRail, 140..160 m holds previous, !hasModel Unloaded.
	Check(simtier::Decide(simtier::Tier::Unloaded, 50.0, false) == simtier::Tier::Unloaded,
		"SimTier: no model is always Unloaded");
	Check(simtier::Decide(simtier::Tier::FarRail, 130.0, true) == simtier::Tier::NearPhysical,
		"SimTier: inside 140 m enters NearPhysical");
	Check(simtier::Decide(simtier::Tier::NearPhysical, 170.0, true) == simtier::Tier::FarRail,
		"SimTier: beyond 160 m enters FarRail");
	Check(simtier::Decide(simtier::Tier::NearPhysical, 150.0, true) == simtier::Tier::NearPhysical,
		"SimTier: hysteresis band holds NearPhysical");
	Check(simtier::Decide(simtier::Tier::FarRail, 150.0, true) == simtier::Tier::FarRail,
		"SimTier: hysteresis band holds FarRail");

	// 3. FrameContext once-per-frame O(1) lookup + proof counters.
	simtier::FrameContext ctx;
	ctx.Begin(1, true, 0.0f, 0.0f, 0.0f, 6400.0, 6400.0);
	const float nearP[3] = { 50.0f, 0.0f, 0.0f }, farP[3] = { 300.0f, 0.0f, 0.0f }, zeroV[3] = {};
	ctx.Add(0x1001, 0xAAAA, nearP, zeroV, true, false);
	ctx.Add(0x1002, 0xBBBB, farP, zeroV, true, false);
	ctx.Add(0x1003, 0xCCCC, farP, zeroV, false, false);
	ctx.Add(0x1004, 0xDDDD, farP, zeroV, true, true);  // player-owned stays NearPhysical
	ctx.Finish();
	Check(ctx.tierNear.load() == 2 && ctx.tierRail.load() == 1 && ctx.tierUnloaded.load() == 1,
		"FrameContext classifies near=2, rail=1, unloaded=1 with 6400 m input clamped");
	const auto* eNear = ctx.Find(0x1001);
	const auto* eRail = ctx.Find(0x1002);
	const auto* eMiss = ctx.Find(0x9999);
	Check(eNear && eNear->tier == simtier::Tier::NearPhysical && eRail && eRail->tier == simtier::Tier::FarRail && !eMiss,
		"FrameContext Find returns exact tier and nullptr on miss");
	Check(ctx.lookups.load() == 3 && ctx.hits.load() == 2,
		"FrameContext proof counters track lookups=3, hits=2");

	// 4. FrameBudget governor defers over-budget background work and prevents starvation.
	framebudget::Governor gov;
	gov.BeginFrame(3.5);
	Check(gov.Allow(framebudget::RouteSurvey, 1.0), "FrameBudget allows background work under budget");
	int defers = 0;
	for (int i = 0; i < 9; ++i) {
		if (!gov.Allow(framebudget::RouteSurvey, 4.2)) ++defers;
	}
	Check(defers == 9, "FrameBudget defers 9 consecutive over-budget calls");
	Check(gov.Allow(framebudget::RouteSurvey, 4.2) && gov.starveGrants == 1,
		"FrameBudget grants starvation-prevention slice on 10th consecutive defer");
	gov.ResetAll();
	gov.BeginFrame(3.5);
	gov.NoteGameplayMs(3.0);
	Check(!gov.Allow(framebudget::CellBake, 0.8),
		"FrameBudget reserves gameplay cost so pre-gameplay bg work defers when 0.8 + 3.0 >= 3.5 ms");

	// 5. Pre-baked uniform-arclength lane table in trafficroute::Build.
	trafficdata::Route r;
	r.name = "test_straight";
	r.world = "TestWorld";
	r.ring = false;
	r.closed = false;
	r.lanes = 2;
	r.width = 8.0;
	r.points = { {0, 0, 0}, {50, 0, 0}, {100, 0, 0}, {150, 0, 0} };
	const auto line = trafficroute::Build(r);
	Check(!line.error && line.baked && line.baked->size() >= 2 && (*line.baked)[0].samples.size() >= 70,
		"trafficroute::Build pre-bakes uniform ~2 m lane samples");
	trafficdata::Vec pts[64]{};
	double us[64]{};
	const int sliced = trafficroute::SliceBaked(line, 0, 1.0, -3, 64, pts, us);
	Check(sliced == 64 && pts[4].x > pts[3].x && trafficroute::BakedSpawnSafe(line, 0, 1.0),
		"SliceBaked returns 64 monotonic forward samples and marks smooth straight spawnSafe");

	// Sharp hairpin kink + steep cliff must be marked !spawnSafe at the vertex.
	trafficdata::Route kink;
	kink.name = "test_kink";
	kink.world = "TestWorld";
	kink.ring = false;
	kink.closed = false;
	kink.lanes = 1;
	kink.width = 6.0;
	kink.points = { {0, 0, 0}, {20, 0, 0}, {20, 20, 20} };
	const auto kinkLine = trafficroute::Build(kink);
	bool anyUnsafe = false;
	if (kinkLine.baked && !kinkLine.baked->empty()) {
		for (const auto& s : (*kinkLine.baked)[0].samples) if (!s.spawnSafe) { anyUnsafe = true; break; }
	}
	Check(anyUnsafe, "Pre-baked lane table flags sharp kink / steep grade stations as !spawnSafe");

	// 6. Corridor envelope baking (authored yellow box minus hazard box) + BakedSpawnSafe / BakedCorridor.
	{
		trafficdata::Route bridge;
		bridge.name = "test_bridge";
		bridge.world = "TestWorld";
		bridge.ring = false;
		bridge.closed = false;
		bridge.lanes = 2;
		bridge.width = 5.0;     // lane 0 at -2.5 (right of +X travel -> y = -2.5), lane 1 at +2.5
		bridge.corridor = 4.0;
		bridge.points = { {0, 0, 0}, {100, 0, 0} };
		roadplan::FieldBox fb;
		fb.width = 10.0;  // authored yellow box fitted to bridge guardrails (+-5 m from centreline -> right wall +2.5 m from lane 0)
		bridge.road.boxes.push_back(fb);
		routehazard::Box hb;
		hb.x = 50.0; hb.y = -2.5; hb.z = 0.0; hb.hx = 1.5; hb.hy = 2.0; hb.hz = 2.0; hb.yaw = 0.0;  // hazard directly on lane 0 at x=50
		bridge.hazards.boxes.push_back(hb);
		const auto bl = trafficroute::Build(bridge);
		double minLat = 0.0, maxLat = 0.0;
		const bool gotClean = trafficroute::BakedCorridor(bl, 0, 0.2, minLat, maxLat);
		Check(gotClean && std::fabs(maxLat - 2.5) < 0.25 && minLat < -7.0,
			"BakedCorridor respects authored yellow box walls without widening past guardrails");
		Check(trafficroute::BakedSpawnSafe(bl, 0, 0.2),
			"BakedSpawnSafe allows clean bridge station clear of hazards");
		Check(!trafficroute::BakedSpawnSafe(bl, 0, 0.5),
			"BakedSpawnSafe rejects station overlapping route hazard box");

		// Legacy FW-1 regression test: wider road (width=15.1, corridor=5.5) with older narrower stored road_boxes (width=10) must NOT self-block spawns.
		trafficdata::Route legacy;
		legacy.name = "test_legacy_fw1";
		legacy.world = "TestWorld";
		legacy.ring = false;
		legacy.closed = false;
		legacy.lanes = 2;
		legacy.width = 20.0;
		legacy.corridor = 5.5;
		legacy.points = { {0, 0, 0}, {100, 0, 0} };
		roadplan::FieldBox lfb;
		lfb.width = 10.0;
		legacy.road.boxes.push_back(lfb);
		const auto ll = trafficroute::Build(legacy);
		Check(trafficroute::BakedSpawnSafe(ll, 0, 0.5),
			"BakedSpawnSafe preserves legacy FW-1 routes whose stored road_boxes are narrower than lane span");
	}

	// 7. 60 Hz virtual guardrail barrier (corridorwall::Enforce60Hz).
	{
		corridorwall::Barrier bar{};
		bar.refX = 0.0; bar.refY = 0.0;
		bar.tx = 0.0; bar.ty = 1.0;  // heading north (+Y), right normal is (+X)
		bar.minLat = -4.0; bar.maxLat = 1.15;
		bar.valid = true;
		// At right guardrail (x = 1.3 >= 1.15) moving outward (+X) and forward (+Y): outward vx must be zeroed, forward vy preserved.
		const auto rHit = corridorwall::Enforce60Hz(bar, 1.3, 10.0, 3.0, 12.0);
		Check(rHit.arrested && std::fabs(rHit.vx) < 1e-6 && std::fabs(rHit.vy - 12.0) < 1e-6 && rHit.yawNudge > 0.4,
			"Enforce60Hz zeroes outward lateral velocity at right wall and nudges inward (left)");
		// At right guardrail moving inward (-X): untouched.
		const auto rIn = corridorwall::Enforce60Hz(bar, 1.3, 10.0, -2.0, 12.0);
		Check(!rIn.arrested && std::fabs(rIn.vx + 2.0) < 1e-6,
			"Enforce60Hz leaves inward recovery velocity untouched");
		// Inside corridor: untouched.
		const auto rMid = corridorwall::Enforce60Hz(bar, 0.2, 10.0, 3.0, 12.0);
		Check(!rMid.arrested && std::fabs(rMid.vx - 3.0) < 1e-6,
			"Enforce60Hz leaves interior lateral motion untouched");
	}

	// 8. 60 Hz closed-loop tyre-mode heading overshoot damping (corridorwall::DampTyreYaw60Hz).
	{
		double damped = 0.0;
		// Diverging: spinning left (+0.9 rad/s) while aim is right (+0.3 rad).
		Check(corridorwall::DampTyreYaw60Hz(0.3, 0.9, 1.6, damped) && damped < 0.0,
			"DampTyreYaw60Hz brakes diverging left spin when aim is to the right");
		// Converging: spinning right (-0.5 rad/s) while aim is right (+0.3 rad).
		Check(!corridorwall::DampTyreYaw60Hz(0.3, -0.5, 1.6, damped),
			"DampTyreYaw60Hz leaves converging yaw spin untouched");
	}

	// 9. Node Flow Landing Bay Registry (bay occupancy, reservation, dwell, release).
	{
		nodeflow::Registry reg;
		reg.AddBay("spaceport_bay", "taxi", 0, 2, 100.0, 200.0, 10.0, 35.0);
		reg.AddBay("spaceport_bay", "taxi", 1, 2, 114.0, 200.0, 10.0, 35.0);
		const int b1 = reg.Reserve("spaceport_bay", 0xFF100001, 20.0);
		const int b2 = reg.Reserve("spaceport_bay", 0xFF100002, 15.0);
		const int b3 = reg.Reserve("spaceport_bay", 0xFF100003, 15.0);  // full -> must refuse (-1)
		const int b1Again = reg.Reserve("spaceport_bay", 0xFF100001, 25.0);  // idempotent
		Check(b1 == 0 && b2 == 1 && b3 == -1 && b1Again == 0 && reg.FreeCount("spaceport_bay") == 0,
			"nodeflow::Registry enforces 1-vehicle-per-bay reservation and refuses overflow");
		Check(reg.MarkParked("spaceport_bay", 0xFF100001, 18.0),
			"nodeflow::Registry marks reserved vehicle as Parked with dwell remaining");
		FvoLandingStateV1 snap{};
		Check(reg.Read(0, snap) && snap.occupantVehicle == 0xFF100001 && snap.state == FVO_BAY_PARKED && std::fabs(snap.dwellRemainingS - 18.0) < 1e-6,
			"nodeflow::Registry Read populates FvoLandingStateV1 accurately for Quest/Encounter queries");
		Check(reg.ReleaseVehicle(0xFF100001) == 1 && reg.FreeCount("spaceport_bay") == 1 && reg.Reserve("spaceport_bay", 0xFF100003, 10.0) == 0,
			"nodeflow::Registry frees bay cleanly on release so next vehicle can claim it");
	}

	// 10. Design-by-Look cinematic control conversions (trafficlook.h).
	{
		routeschedule::Schedule sched;
		// Worked example from design doc: Living Capital ring (4.84 km, 2 lanes, 16 m/s, Calm 18 s) -> Steady 6..8/km -> 34 cars.
		trafficlook::ApplySpacing(sched, trafficlook::Calm, 16.0, 2, 4840.0);
		const auto lk = trafficlook::EvaluateRoadLook(sched, 16.0, 2, 4840.0);
		Check(sched.trafficLevel == routeschedule::Steady && sched.trafficBands[routeschedule::Steady].low == 6 &&
		      sched.trafficBands[routeschedule::Steady].high == 8 && lk.cars == 34,
			"ApplySpacing(Calm, 16 m/s, 2 lanes, 4.84 km) writes Steady 6..8/km and yields 34 cars");
		Check(trafficlook::DetectSpacing(sched, 16.0, 2, 4840.0) == trafficlook::Calm,
			"DetectSpacing round-trips Calm preset");
		Check(trafficlook::DetectPace(16.0, false) == trafficlook::City && trafficlook::DetectPace(22.0, true) == trafficlook::City &&
		      !trafficlook::SpeedJumpTooLarge(16.0, 22.0) && trafficlook::SpeedJumpTooLarge(12.0, 24.0),
			"Pace presets and 8 m/s link speed-jump warning match design spec");
		Check(!trafficlook::QueueAllowed(false, 5) && !trafficlook::QueueAllowed(true, 2) && trafficlook::QueueAllowed(true, 3),
			"QueueAllowed permits wait-above only on hover roads with >= 3 spaces");
		trafficdata::Zone cz;
		trafficlook::ApplyCalmZone(cz, 10.0, -165.0, -125.0);
		Check(cz.slow && std::fabs(cz.speed - 10.0) < 1e-6 && !trafficconstraints::Empty(cz.restrictions) &&
		      std::fabs(cz.restrictions.speed - 10.0) < 1e-6 &&
		      std::fabs(cz.restrictionFloor + 175.0) < 1e-6 && std::fabs(cz.restrictionTop + 115.0) < 1e-6,
			"ApplyCalmZone writes both slow speed and height-banded restrictions for all driver types");
	}

	// 11. Scripted Node Event Bus, bounded pad hold timeout, link director & optional tag/event_id I/O.
	{
		Check(std::fabs(nodeflow::ClampHoldSeconds(0.0) - nodeflow::kDefaultMaxHoldS) < 1e-6 &&
		      std::fabs(nodeflow::ClampHoldSeconds(9999.0) - nodeflow::kHardMaxHoldS) < 1e-6 &&
		      std::fabs(nodeflow::ClampHoldSeconds(25.0) - 25.0) < 1e-6,
			"ClampHoldSeconds bounds pad holds so stalled scripts never wedge a landing spot");
		nodeflow::EventBus bus;
		bus.Emit(1000, FVO_NODE_LINK, FVO_EVT_ENTER, FVO_TIER_PHYS, 0xFF100001, "l_ring_ns", "mast_jct", "ev_convoy", "lc_ring_road", "lap_cross", 0, 1, 16.0, 12.5, 0.0);
		bus.Emit(2500, FVO_NODE_LINK, FVO_EVT_ENTER, FVO_TIER_RAIL, 0xFF100002, "l_ring_ns", "mast_jct", "ev_convoy", "lc_ring_road", "rail_lap_cross", 1, -1, 22.0, 12.5, 0.0);
		FvoNodeEventV1 e1{}, e2{};
		Check(bus.ReadAfter(0, e1) && e1.seq == 1 && e1.timestampMs == 1000 && e1.simTier == FVO_TIER_PHYS &&
		      std::fabs(e1.speed - 16.0) < 1e-6 && e1.lane == 0 && e1.dir == 1 && std::fabs(e1.u - 12.5) < 1e-6,
			"EventBus records time and motion fields (t_ms, speed, lane, dir, u) for phys tier");
		Check(bus.ReadAfter(e1.seq, e2) && e2.seq == 2 && e2.simTier == FVO_TIER_RAIL && e2.dir == -1 &&
		      std::string(e2.tag) == "mast_jct" && std::string(e2.eventId) == "ev_convoy",
			"EventBus records far FleetRail tier events with optional tag and event_id");
		nodeflow::LinkDirector dir;
		dir.Set("l_ring_ns", 0.05, true, false, 1000, 10.0, "convoy_divert");
		const auto* ov = dir.Find("l_ring_ns", 5000);
		Check(ov != nullptr && ov->divertAll && !ov->closed && std::fabs(ov->shareOverride - 0.05) < 1e-6,
			"LinkDirector stores active divertAll and shareOverride before expiry");
		Check(dir.Find("l_ring_ns", 12000) == nullptr,
			"LinkDirector auto-expires temporary link rules after durationSeconds");
		trafficdata::Data td;
		trafficdata::Read(
			"[world.w]\nid = Jemison\n"
			"[place.p16]\nworld = w\npoint = 10 20 30\ntag = hero_pad\nevent_id = vip_arrival\n"
			"[link.lk1]\nfrom = r1\nto = r2\nat = 1\ntag = mast\nevent_id = convoy\n"
			"[zone.z1]\nworld = w\npoint = 1 2 3\nkind = slow\nspeed = 10\ntag = plaza_calm\nevent_id = hero_calm\n",
			"test_tags.ini", td);
		Check(td.problems.empty() && td.places.size() == 1 && td.places[0].tag == "hero_pad" && td.places[0].eventId == "vip_arrival" &&
		      td.links.size() == 1 && td.links[0].tag == "mast" && td.links[0].eventId == "convoy" &&
		      td.zones.size() == 1 && td.zones[0].tag == "plaza_calm" && td.zones[0].eventId == "hero_calm",
			"trafficdata::Read parses optional tag and event_id on places, links, and zones without problems");
	}

	if (!g_fail) std::printf("ALL PASS\n");
	return g_fail ? 1 : 0;
}
