#include "trafficrank.h"
#include "vehicleflight.h"
#include "handoff.h"
#include "trafficpool.h"
#include "laneline.h"
#include "railturn.h"
#include "planframe.h"
#include "fieldvolume.h"
#include "fieldboxapi.h"
#include "planowner.h"
#include "spottest.h"
#include "stockfloat.h"
#include "relocationconsumers.h"
#include "selfreport.h"
#include "worldtrust.h"
#include "trafficcensusapi.h"
#include "runtimeforms.h"
#include "loadedarea.h"
#include "routesurvey_run.h"
#include "hazardfield.h"
#include "wheelgrip.h"
#include "autogear.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include "surface.h"
#include "tracking.h"
#include "cornerforce.h"
#include "planprobe.h"
#include "vehicle_probe.h"
#include <string>
#include <string_view>
#include "planrays.h"
#include "planview.h"
#include "hoverthrust.h"
#include "goaldoor.h"
#include "jobline.h"
#include "cameraahead.h"
#include "contactevent.h"
#include <unordered_map>
#include "railforce.h"
#include "routeguidance.h"
#include "fvolimits.h"
#include "cellbake.h"
#include "bodymass.h"
#include "hookcost.h"
#include "vehiclediag.h"
#include "vehiclekeep.h"
#include "simtier.h"
#include "vehiclelifecycle.h"
#include "bindings.h"
#include "passenger.h"
#include "features.h"
#include "scriptdrive.h"
#include "centrewell.h"
#include "overrides.h"
#include "profileclass.h"
#include "railtyres.h"
#include "pathfollow.h"
#include "planenergy.h"
#include "planspeed.h"
#include "coverfight.h"
#include "keygesture.h"

#include "addresses.h"
#include "controlmap.h"
#include "vcontract.h"
#include "flight.h"
#include "game.h"
#include "hook.h"
#include "hoverui.h"
#include "log.h"
#include "modscene.h"
#include "navmap.h"
#include "navmesh.h"
#include "drivecontrols.h"
#include "radar.h"
#include "inputgate.h"
#include "trafficarrival.h"
#include "navfield.h"
#include "environment.h"
#include "bodybinding.h"
#include "hoverlogic.h"
#include "bodyattitude.h"
#include "freeroll.h"
#include "physicsstep.h"
#include "farrail.h"
#include "fleetrail_policy.h"
#include "trafficprofile.h"
#include "vtolphysics.h"
#include "vtolai.h"
#include "flightgear.h"
#include "vtolassist.h"
#include "vtolhooks.h"
#include "vehiclecamera.h"
#include "hulllogic.h"
#include "integrationlogic.h"
#include "boostlogic.h"
#include "boostneed.h"
#include "altcontrol.h"
#include "targetsession.h"
#include "scantarget.h"
#include "npcvehicles.h"
#include "notify.h"
#include "prefetch.h"
#include "telemetry.h"
#include "vehicleworld.h"
#include "vehiclehits.h"
#include "realvehicles.h"
#include "travelmode.h"
#include "flightsave.h"

#include "fvopaths.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <unordered_set>
#include <type_traits>
#include <unordered_map>

// Rover flight (0.57.0): a connected flight model that pushes the rover's own physics body instead of overwriting it.
//  - On / off: the hover toggle (V). Also off: settle onto the ground, hit something, or leave
//    the vehicle.
//  - Controls (mouse-aim layout): the mouse aims, the nose follows the camera in yaw and pitch. A / D: roll.
//    W: boost (hold) at the selected gear; S: brake (hold); tap Alt: cycle gears 1-4.
//    Neither W nor S: coast. Hold Space: climb; hold C: descend (2026-10-27, the user: Alt no longer descends).
//  - Lift-off: switching on near the ground climbs to a short hover height first.
//  - Avoidance (terrain assist): three corridors (ahead, left, right) are sampled with ground rays at three distances
//    scaled by speed. Ahead clear: nothing. Ahead blocked but climbable in time: nose up and climb. Otherwise a clear
//    side: turn that way (the A / D bank turn). Nothing clear: brake to a stop.
//  - Yaw: the gap between where the camera looks and where the nose points, used like a variable turn input
//    (bigger gap = faster turn, sin(gap / 2) shaped). Physical VTOL adds camera bank about its unchanged forward.
//  - Rotation has momentum: inputs set wanted turn rates and the spin eases toward them; released axes level back.
//    Couplings: banking curves the path (the hover lift tilts) and turns the nose into the curve; turning leans the
//    rover into the turn; pitch trades speed and height; sideways speed leaks away and partly turns along the nose.
//  - Every physics step (FUN_1423CB260, after the game's own vehicle step) reads the body's real velocity and
//    attitude and adds accelerations; contacts, slopes and the game's gravity stay in the motion. Gravity is
//    measured from the body (what one step cycle does to the vertical speed) and carried by the hover lift.
//  - Call (stage 1 of NPC vehicles): a free key on foot calls the player's own vehicle (the one the game links to the
//    player or the home ship; never another vehicle). Far (over kCallFar), not loaded,
//    or in another worldspace: it is moved (MoveReference) to a clear spot next to the player (ground ray). Close: it
//    switches to hover on autopilot and flies to the player with the same model and terrain assist, the player's
//    bearing standing in for the camera and accelerate / brake chosen from the distance; it stops kCallStop away.
//  - Elevation assist: a height hold above the terrain (rising early for higher ground ahead) and a 2.5 m floor.
#include "fvo_addon_api.h"
#include "trafficinspect.h"
#include "routeedit.h"
#include "playclock.h"
#include "traffichealthapi.h"
#include "costparts.h"

namespace vehicleflight
{
	// Internal: the navigation settings of the profile driving this pilot (pilot 0 = the player's vehicle), stored with each goal and read by the navigation service.
    void RouteNav(int pilot, void* trip, const FvoTripGoal& goal, vcontract::NavParams& nav) {
        if (routeguidance::Capture(pilot,trip,GetTickCount64())) {
            const auto* lane=routeguidance::Get(pilot,GetTickCount64());
            nav.trafficLane=true;
            nav.travel=lane->travel==1?travelmode::Default::Drive:lane->travel==2?travelmode::Default::Hover:travelmode::Default::Auto;
            nav.priority=lane->priority;
        } else if(const auto* policy=routeguidance::Policy(pilot);policy&&trafficcoordinator::Strict(*policy)) {
            const auto& lane=routeguidance::frames[pilot].lane;nav.trafficLane=true;
            nav.travel=lane.count<2?(goal.fly?travelmode::Default::Hover:travelmode::Default::Drive):lane.travel==1?travelmode::Default::Drive:lane.travel==2?travelmode::Default::Hover:travelmode::Default::Auto;
        } else if(goal.fly) {nav.trafficLane=true;nav.travel=travelmode::Default::Hover;}
    }

	void PilotNav(int pilot, const vcontract::NavParams& nav);
	const vcontract::NavParams& PilotNavOf(int pilot);

	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }
		template <class Fn>
		Fn VFn(std::uintptr_t obj, std::uint32_t off) { return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[off / 8]); }

		// A ground ray can hit terrain while its TES cell is still attaching. Physics/content is not ready in that
		// state (01:06 session: grid readiness lagged far behind the millisecond-scale cell load jobs).
		// Check the actual cell under each look-ahead sample before allowing the rover to gain speed.
		bool CellAttachedAt(double x, double y)
		{
			const auto tes = Ptr(addr::kTESPtr);
			if (!tes || At<std::uintptr_t>(tes, off::kTESInteriorCell)) return false;
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = std::min(At<std::uint32_t>(tes, off::kTESGridCount), 64u);
			if (!entries || !count) return false;
			const auto cx = static_cast<std::int32_t>(std::floor(x / off::kCellSize));
			const auto cy = static_cast<std::int32_t>(std::floor(y / off::kCellSize));
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto entry = entries[i];
				if (!entry) continue;
				const auto state = At<std::uint32_t>(entry, off::kGridEntryState);
				if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) continue;
				const auto cell = At<std::uintptr_t>(entry, off::kGridEntryCell);
				if (!cell) continue;
				const auto ext = At<std::uintptr_t>(cell, off::kCellExterior);
				if (ext && At<std::int32_t>(ext, 0) == cx && At<std::int32_t>(ext, 4) == cy)
					return At<std::uint8_t>(cell, off::kCellState) == 10;
			}
			return false;
		}

		// ---- Tuning ---------------------------------------------------------------------------------------------
		// Linear
		constexpr double kThrust = 18.0;          // m/s^2 along the nose at 100 %
		constexpr double kDragCoast = 0.35;       // 1/s, horizontal speed decay without thrust
		constexpr double kDragThrust = 0.12;      // 1/s, with thrust (sets the top speed with kThrust)
		// (FC-1: the sideways leak is the flight gear row's side gain, carcap::Flight; gear 3 = 1.2 /s as before)
		constexpr double kRedirect = 0.7;         // the share of the sideways leak that turns along the nose (follow the path)
		constexpr double kTiltPush = 0.6;         // share of the tilted hover lift that pushes sideways / forward
		constexpr double kTrade = 0.5;            // share of the speed <-> height energy trade for pitch climbs / dives
		constexpr double kClimbExtra = 8.0;       // m/s of climb from full pitch at zero speed
		constexpr double kLiftSpeed = 8.0;        // m/s straight up / down from the lift keys
		constexpr double kVzResponse = 3.0;       // 1/s, vertical speed toward its target
		constexpr double kMaxUp = 25.0;           // m/s^2 the hover lift may add over gravity
		constexpr double kMaxDown = 6.0;          // m/s^2 the down thrusters may pull (below zero lift)
		constexpr double kLiftIntegral = 1.2;     // 1/s^2: how fast the hover lift's integral corrects a vertical speed error
		constexpr double kHoldGain = 1.5;         // 1/s, height hold
		constexpr double kHoldMaxVz = 6.0;        // m/s
		constexpr double kMinClearance = 2.5;     // m above the ground, hard floor
		constexpr double kSpeedCap = 60.0;        // m/s with the ground ahead loaded
		constexpr double kSpeedCapBlind = 25.0;   // m/s when the ground ahead is not loaded yet
		// Rotation (wanted rates; the spin eases toward them = rotational momentum)
		constexpr double kCamYawMax = 1.6;        // rad/s at a 180 degree camera gap (sin(gap / 2) shape)
		constexpr double kCamDeadzone = 0.035;    // rad (2 degrees): no turn for a gap this small
		constexpr double kPitchRate = 0.9;        // rad/s, the fastest pitch the camera aim asks for
		constexpr double kCamPitchGain = 2.0;     // 1/s: pitch rate per radian between the camera aim and the nose
		constexpr double kCamPitchDeadzone = 0.05;  // rad (3 degrees)
		constexpr double kThrottleRate = 0.5;     // per second while W / S are held
		constexpr double kBoost = 2.5;            // boost thrust, times kThrust (45 m/s^2: reaches the boost cap in a few seconds)
		constexpr double kBoostCap = 150.0;       // m/s: boost top speed = the ship's planned cruise speed (plan-ship-flight)
		constexpr double kRollRate = 1.0;         // rad/s while a roll button is held
		constexpr double kPitchMax = 0.7;         // rad
		constexpr double kRollMax = 0.8;          // rad
		constexpr double kLevelRate = 1.2;        // 1/s, released pitch / roll level back
		constexpr double kBankYaw = 0.8;          // rad/s of turn per unit sin(roll) (scaled up with speed)
		constexpr double kLean = 0.25;            // rad of roll per rad/s of yaw at speed: leaning into a turn
		// (FC-1: how fast the spin follows its wanted rate is the flight gear row's yaw / tilt response, carcap::Flight; gear 3 = 4 / 5 /s)
		// Gears (W / S): thrust (times kThrust) and top speed per gear; gear 0 coasts. The top is the flight row's (carcap::kFlightTop).
		constexpr double kGearThrust[5] = { 0.0, 0.6, 1.0, 1.6, 2.5 };
		constexpr const double (&kGearTop)[5] = carcap::kFlightTop;
		// Lift-off: switching on below kLiftoffBelow climbs to kLiftoffHeight before the normal hold takes over.
		constexpr double kLiftoffHeight = 5.0;    // m
		constexpr double kLiftoffBelow = 3.0;     // m
		// Avoidance controller
		constexpr double kAvoidClear = 4.0;       // m kept above anything in the path
		constexpr double kAvoidClimbRate = 10.0;  // m/s the climb may use to get over something
		constexpr double kAvoidMaxClimb = 40.0;   // m: more than this above the rover is not "a way up"
		constexpr double kAvoidSide = 0.6;        // rad: left / right corridors (35 degrees)
		constexpr double kAvoidTurnRate = 0.9;    // rad/s of turn while avoiding
		constexpr double kAvoidSlowTo = 40.0;     // m/s: turning away is done at most this fast
		constexpr double kBrakeAcc = 30.0;        // m/s^2 when stopping
		constexpr ULONGLONG kAvoidHoldMs = 250;   // an avoidance action is kept at least this long (no flicker; 2026-10-20: was 600, snappier)
		// Call
		constexpr double kCallFar = 150.0;        // m: farther than this (or not loaded) the rover is moved, not flown (2026-10-17: was 300)
		constexpr double kCruiseAhead = 300.0;    // m: Auto (player aboard) flies at a point this far along the camera's heading
		constexpr double kNearBoard = 6.0;        // m: the HUD's "Get in" shows this close to the player's own vehicle
		constexpr double kCallStop = 8.0;         // m: the autopilot stops this far from the player
		constexpr double kCallBrakeSlack = 1.6;   // brakes once the distance left is under stop + speed * this (s)
		constexpr ULONGLONG kCallTimeoutMs = 90000;
		constexpr ULONGLONG kGuardMs = 6000;
		// Autopilot attitude: its own yaw, pitch and roll (the player only has the camera aim and the A / D bank).
		constexpr double kApYawGain = 2.2;        // 1/s: turn rate per radian of heading error (2026-10-20: was 1.6, snappier)
		constexpr double kApYawMax = 1.2;         // rad/s
		constexpr double kApBankPerYaw = 0.35;    // rad of bank per rad/s of turn (coordinated turn)
		constexpr double kApBankMax = 0.45;       // rad
		constexpr double kApRollGain = 3.0;       // roll input per radian of bank error
		constexpr double kApPitchMax = 0.35;      // rad: nose up / down at most this far
		constexpr double kApClimbPitch = 0.35;    // rad: nose up over an obstacle
		// Follow
		constexpr double kFollowBack = 12.0;      // m behind the player
		constexpr double kFollowSide = 4.0;       // m to the player's right
		constexpr double kFollowNear = 6.0;       // m: closer than this the rover backs off
		constexpr double kFollowFar = 20.0;       // m: beyond this it catches up (gear from the distance)
		constexpr double kFollowRelaunch = 25.0;  // m: a parked rover lifts off again when the player is this far
		constexpr ULONGLONG kFollowStillMs = 5000;  // the player still this long: the rover lands and parks
		constexpr double kCallClimb = 6.0;        // m: called, the rover first rises straight up this high (never snags)
		constexpr ULONGLONG kCallClimbMs = 4000;  // ... or for at most this long
		// Autopilot navigation (2026-10-17): the corridors are cast along the way to the target; blocked ahead, the
		// heading turns away in kAvoidSide steps (up to kDetourMax) and keeps that side until the corridor back toward
		// the target is clear again (wall following); nothing open on either side: it climbs.
		constexpr double kDetourMax = 1.8;           // rad (3 steps, about 100 degrees)
		constexpr ULONGLONG kDetourHoldMs = 400;     // a detour step is held at least this long (2026-10-20: was 900, snappier)
		constexpr double kDetourSlowTo = 25.0;       // m/s while detouring
		constexpr double kCruiseExtraMax = 30.0;     // m: blocked everywhere, the skim height rises at most this much
		// Stuck escape: first climb out higher; stuck again within kEscapeWindowMs, or not closing on the target:
		// the vehicle is moved next to the target.
		constexpr ULONGLONG kEscapeWindowMs = 25000;
		// Trail (2026-10-17): the player's own path, a point every kTrailStep; a follower blocked on the way to the
		// player flies it (anywhere the player got through, it can) until it is close again.
		constexpr int kTrailMax = 512;
		constexpr double kTrailStep = 4.0;           // m between points
		constexpr double kTrailReach = 8.0;          // m: a point this close is passed
		constexpr double kTrailOff = 25.0;           // m from the player: back to flying straight at them
		constexpr double kTrailAwayMax = 40.0;       // m further from the player than when the trail started: it leads away, drop it
		constexpr ULONGLONG kTrailBanMs = 10000;     // then no trail for this long
		// Cruise height: just over the highest ground in the next corridor samples (rises fast, sinks slowly).
		constexpr double kTerrainClear = 2.5;        // m over the highest ground ahead
		constexpr double kCruiseSink = 1.5;          // m/s the cruise height comes down at most
		// Goal height (2026-10-17): a pilot given the target's height flies at it (never under the terrain clearance),
		// up to kGoalMaxAbove over the ground, and comes down at kGoalSink (a diving target is followed down).
		constexpr double kGoalMaxAbove = 400.0;      // m (was 200: the user's ring lanes sit ~305 / 345 m over the parked vehicles, 2026-10-28)
		constexpr double kGoalSink = 8.0;            // m/s
		// Speed matching (following): the player's speed along the way plus a share of the gap.
		constexpr double kMatchGain = 0.5;           // (m/s) per m of gap
		// AI gears (following; the player's own gears unchanged): far apart, so each gear is its own speed band and
		// the top speed of the gear in use caps the speed (2026-10-17: thrust alone forced speed with no real control).
		constexpr double kAiGearTop[5] = { 0.0, 5.0, 18.0, 50.0, 150.0 };     // m/s (each gear about 3x the last)
		constexpr double kAiGearThrust[5] = { 0.0, 0.15, 0.5, 1.4, 3.2 };     // share of kThrust
		// Gear cap from the path (obstacles decide the top gear, the gap to the target picks within it):
		// backing out 1, path blocked / detouring 2, a climb or rough ground ahead 3, ground ahead loading 3.
		constexpr double kRoughSpread = 6.0;         // m of height difference across the corridor samples ahead
		constexpr ULONGLONG kCapRaiseMs = 2000;      // the cap rises one gear per this long clear
		constexpr double kShiftUp = 0.9;             // up when the speed wanted passes this share of the gear's top
		constexpr double kShiftDown = 0.75;          // down when it fits under this share of the lower gear's top
		constexpr ULONGLONG kShiftMs = 700;          // at least this long between shifts
		// Contacts on autopilot (2026-10-17 17:52: an NPC vehicle on a platform the ground rays do not hit -- layers
		// 0x22006 -- took the terrain 5.6 m below for its ground, dived into the platform and the solver's push-up
		// ended four flights in a row as "collision 125-268 m/s"). A contact no longer ends an autopilot flight:
		// a push up = something under it the rays miss (held above it for kFloorMs); sideways = a bump (back off).
		// kBumpLimit bumps in kBumpWindowMs: moved to the target.
		// Drive mode (2026-10-17, the user: "hover struggles with terrain assist and close quarters ... hover off and boost for
		// up and forward respectively and normal driving"): repeated contacts, push-ups, stuck and boxed-in decisions raise a
		// score; at kCrampedOn the autopilot turns hover off and drives with the game's controls (steer, accelerate, brake,
		// forward boost, vertical boost). Back to hover after kDriveMinMs once the score is gone and the way ahead is clear.
		constexpr double kCrampedOn = 3.0, kCrampedDecay = 0.25;  // score; per second
		constexpr ULONGLONG kDriveMinMs = 8000;
		constexpr double kDriveTop = 30.0;           // m/s: faster than this on the ground needs the forward boost
		constexpr ULONGLONG kFloorMs = 8000;
		constexpr double kFloorClear = 2.0;          // m kept over that floor
		constexpr int kBumpLimit = 3;
		constexpr ULONGLONG kBumpWindowMs = 10000;
		// One contact episode is one bump: a grinding or bouncing vehicle reports a contact on every physics step (22:22:02: about
		// 150 "bumps" in 6 s, each flipping the detour and raising the drive-mode score to 148).
		constexpr ULONGLONG kBumpDebounceMs = 700;
		constexpr double kCrampedMax = 8.0;
		constexpr double kFlipUp = 0.42;             // cos(pitch) * cos(roll) under this: flipped (~65 degrees)
		constexpr ULONGLONG kFlipMs = 2000;
		constexpr double kStrafeAcc = 9.0;           // m/s^2 sideways at full strafe
		constexpr ULONGLONG kReverseMs = 1800;       // backing away from something close ahead
		constexpr double kReverseThrottle = 0.7;
		constexpr double kLostFar = 60.0;            // m: farther than this and not kLostGain closer in kLostMs: moved
		constexpr double kLostGain = 5.0;
		constexpr ULONGLONG kLostMs = 12000;
		constexpr double kLostMove = 15.0;           // m: a vehicle that moved this far in the window is chasing, not lost
		constexpr double kFollowLead = 1.2;          // s: the follow point leads the player's movement
		constexpr double kCallSkim = 2.5;         // m: then it flies this low, rising only when terrain or obstacles need it
		constexpr std::uint32_t kLookupForm = 0x005DE3B0;  // (form id) -> TESForm* (global form map), or null
		// Misc
		constexpr ULONGLONG kDoubleTapMs = 350;   // two presses within this = a double tap
		constexpr double kCastAbove = 80.0;       // m: ahead / side rays start this far above the rover (0.54.2)
		// Forward rays (2026-10-17 20:37: the summoned rover sat 10 s at 0 m/s against something the ground rays never
		// saw, all corridors clear). Every solid Havok layer: 1 static, 2 anim static, 3 transparent, 4 clutter,
		// 8 biped, 9 trees, 10 props, 13 terrain, 14 trap, 17 ground, 20 debris large, 26-28 transparent small /
		// invisible wall / small anim, 30 char controller, 33 biped no CC, 35 collision box. Not triggers (12), water
		// (11) or other non-solid volumes, so trigger boxes in cities are not walls.
		constexpr std::uint64_t kSolidLayers = 0xA5C12671Eull;
		// Walls only for every navigation query (rays, sweep ring, hull sweep): no characters (8 biped, 30 character controller,
		// 33 biped no CC). The player and the people around are not walls; the forward rays had never once run before 2026-10-18,
		// so no behaviour depends on them hitting characters.
		constexpr std::uint64_t kWallLayers = kSolidLayers & ~((1ull << 8) | (1ull << 30) | (1ull << 33));
		constexpr double kRayStart = 3.5;         // m ahead of the vehicle's origin (past its own hull)
		constexpr double kRayLift = 1.0;          // m above its origin
		constexpr double kRayMin = 25.0;          // m: the shortest look ahead (2026-10-20: was 15; walls are flagged earlier)
		constexpr double kRayLead = 2.4;          // s of travel the forward rays and the hull sweep look ahead (was 1.6)
		// Walls (2026-10-20, the user: "terrain assist tends to act like a magnet to walls ... turn it off but use it as warning flag for the
		// control system ... increase the distance for an earlier flag"). A wall = a forward ray / hull sweep hit, or a ground sample that
		// rises faster than kWallSlope from the ground under the vehicle. Walls never lift the vehicle (no hold, no floor, no "climb over");
		// they set the wall warning, and the autopilot slows to stop kWallStandoff short and goes around (the detour steering). Gentle
		// terrain still raises the hold as before.
		constexpr double kWallSlope = 0.6;        // rise / distance (about 30 degrees)
		constexpr double kWallMinRise = 4.0;      // m: lower steps are terrain
		constexpr double kWallStandoff = 6.0;     // m kept from a wall
		constexpr double kWallDecel = 8.0;        // m/s^2 the speed cap plans with
		// Sweep rays (2026-10-18, the shared nav map): three more rays a frame per autopilot vehicle, walking round a fixed
		// ring of 16 compass directions (a full turn in about 6 frames), only to fill the map the heading sampler reads.
		constexpr int kSweepRays = 3, kSweepRing = 16;  // slots: prefetch::SweepRay
		// What is ground (2026-10-20, the user: "terrain assist should only be for the ground and for everything else just a warning ... should
		// still be able to go through"; the design picked: terrain plus surfaces below the flight level). Every probe ahead (front, the three
		// look-ahead samples, the nine corridors) is two probes: the terrain and planet ground layers (13, 17) from kCastAbove (hills, cliffs) and
		// every surface from kFlightLevel over the vehicle down (a street, platform or roof it can reach without climbing into anything). The
		// higher of the two is the ground. A building face, overhang or bridge above the flight level lifts nothing: it is the forward rays' wall
		// warning. The probe straight down and the two side probes see every surface, as before.
		constexpr std::uint32_t kTerrainLayers = (1u << 13) | (1u << 17);
		constexpr double kFlightLevel = 3.0;      // m over the vehicle the flight-level probes start
		int LowSlot(int idx, int i) { return prefetch::kSlotLow0 + idx * prefetch::kSlotLowEach + i; }
		// Ceilings: two rays a frame per pilot (straight up kCeilUp, and up and ahead), the lowest hit above the vehicle caps the height hold
		// kCeilClear under it, so it never presses itself into an underside; under an overhang it flies through at that height.
		constexpr double kCeilUp = 30.0, kCeilAhead = 15.0, kCeilAheadUp = 12.0, kCeilClear = 5.0, kVehicleTop = 2.0;  // (kCeilClear 2026-10-20: was 2.5; the user still felt a pull toward ceilings)
		constexpr double kSelfHitBand = 3.0;      // m: a front sample this close to the rover's own height is the rover
		constexpr double kLandHeight = 1.0;       // m: settling under this (not climbing) for kLandMs ends the hover
		constexpr ULONGLONG kLandMs = 1500;
		constexpr unsigned kProbeMaxAge = 2;
		// Planning tiers (2026-10-29, 64 NPC vehicles): a pilot planned every 100 / 200 ms (far, see the planning loop) reads results that
		// many frames older; its probe age limit grows with its interval (x2 / x4), so a slow pilot never takes its own data for missing.
		unsigned g_ageScale[1 + prefetch::kSlotPilotCount] = {};
		unsigned MaxAge(int idx) { const unsigned k = idx > 0 && idx <= prefetch::kSlotPilotCount ? std::max(1u, g_ageScale[idx]) : 1u; return kProbeMaxAge * k; }
		// Far pilots (2026-10-29 performance pass, approved by the user): a traffic pilot kHoldM or more away stops and hovers where it is
		// (its job and trip untouched) until the player comes within kResumeM. Straight flight (autodrive's `straight`: no route search,
		// no heading fan, the one heading still judged) and slower planning follow the view, see PilotsTick.
		constexpr double kHoldM = 1000.0, kResumeM = 900.0;
		// Vision tiers (2026-10-29, the user: "should it not be tied to my vision", "poll the mouse as well for predictive", "with
		// safeguards"): a traffic pilot out of view (kViewHalf either side of the camera's heading, widened toward where the camera is
		// turning, kViewLeadS ahead; seen within kViewKeepMs counts as in view) and safe (below) plans at 4 Hz past kNearM, 2 Hz and
		// straight past kMidM. It never stops for it: physics keeps applying its last speed, heading and height hold between passes.
		// Safe = none of: within kNearM of the player, on its wheels, taking off / landing / settling / climbing over, fighting, a vehicle
		// within kCrowdM, giving way, its path blocked or uncertain, a bump within kBumpRecentMs; and only on traffic lanes.
		// A vehicle counts as near within kCrowdM plus kCrowdLeadS of both speeds (two at 30 m/s: 130 m). The check runs every tick; the
		// moment it fails the pilot plans at once, and it stays at its normal rate kDangerHoldMs after the last failure.
		constexpr double kViewHalf = 1.0472, kViewLeadS = 0.5, kViewLeadMax = 1.2, kCrowdM = 40.0, kCrowdLeadS = 1.5, kNearKeepM = 60.0;
		constexpr ULONGLONG kViewKeepMs = 3000, kBumpRecentMs = 5000, kDangerHoldMs = 2000, kOutMidMs = 250, kOutFarMs = 500, kNearOutMs = 100;
		bool g_straight[1 + prefetch::kSlotPilotCount] = {};
        // What World keeps per lane car (removal 2026-11-14; the old lane driver's state is gone): the road's lanes at the car (asked every
        // gather, for the energy plan's frame and the corridor rays) and the hover follower's wanted height (the height hold reads it).
        struct LaneCar { FvoRoadLanesV1 roadLanes{}; bool roadLanesOk = false; double height = 0; };
        LaneCar g_laneDriver[1 + prefetch::kSlotPilotCount];
        constexpr bool g_driveRail = true;
        const char* g_autoOffWhy = "";  // who ended Auto (and so Cruise) last: your key, or the mod's reason (2026-11-05 source tags)  // rail drive: always (2026-11-05 settings strip)
        ULONGLONG g_railLaneAt[1 + prefetch::kSlotPilotCount] = {};
        double g_railBearing[1 + prefetch::kSlotPilotCount] = {};  // the rail's look-ahead bearing (railturn::Pursue), smoothed
        double g_railBearingAt[1 + prefetch::kSlotPilotCount] = {};  // when it was last set (smoothing)
        double g_railAimJump[1 + prefetch::kSlotPilotCount] = {};    // how far the raw aim moved since the last frame (probe)
        double g_railYawCmd[1 + prefetch::kSlotPilotCount] = {};     // the damped turn command
        // AF-1 (2026-11-12, user: "Yes, unlock it"): the committed plan's path as world points in the order the car reaches them, written
        // where the plan is handed to the actuator and read by the rail stage, which follows it (pathfollow.h) instead of one look-ahead point
        // forward and a straight back-up in reverse.
        struct FollowPath { pathfollow::Path path; bool reverse = false, laneCentre = false; ULONGLONG at = 0; ULONGLONG logAt = 0; };
        FollowPath g_planPath[1 + prefetch::kSlotPilotCount];
        // CW-1 step 3 (centre-line well, notes/centreline-well-20261122/PLAN.md): each lane car's lateral plan, made at its World gather and
        // driven every update. Kept sides live here; `recovering` hands the car to the energy plan's backing-up families while no forward fit
        // exists (decision D3 default, design section 8). Never in Script mode.
        struct WellState {
            std::uint32_t vehicle = 0; centrewell::Memory mem; pathfollow::Path path; bool centre = true;
            double speed = 0, at = -1, maxOffset = 0; int blockAt = -1; const char* why = "";
            double blockedStandSince = -1, offRoadSince = -1, clearSince = -1; bool recovering = false; double recoverSince = -1;
            double logAt = -1; const char* loggedWhy = ""; int loggedBlock = -2; bool loggedRecover = false;
        };
        WellState g_well[1 + prefetch::kSlotPilotCount];
        std::mutex g_followLock;
        // ED-1: the energy plan's stretch to follow (world points already).
        void NoteFollowPath(int idx, const pathfollow::Path& path, bool reverse, bool laneCentre) {
            if (idx < 0 || idx >= int(sizeof g_planPath / sizeof g_planPath[0])) return;
            std::lock_guard lk(g_followLock); FollowPath f; f.path = path; f.reverse = reverse; f.laneCentre = laneCentre; f.at = GetTickCount64(); f.logAt = g_planPath[idx].logAt; g_planPath[idx] = f;
        }
        struct EnergyLog { ULONGLONG at = 0; bool used = false; const char* why = ""; } g_energyLog;
        // Path rays (2026-11-05 avoidance pass): horizontal rays cast along the lane path array the car will drive, at the car's edges
        // and centre and the lane's edges, low (kerb height) and mid (body height); filled in by the lane branch, cast in AskCorridors.
        double g_railHalfW[1 + prefetch::kSlotPilotCount] = {}, g_railLaneHalf[1 + prefetch::kSlotPilotCount] = {};
        float g_railRoadN[1 + prefetch::kSlotPilotCount][3] = {};
        ULONGLONG g_railRoadNAt[1 + prefetch::kSlotPilotCount] = {};
        double g_railFloorZ[1 + prefetch::kSlotPilotCount] = {};
        ULONGLONG g_railFloorZAt[1 + prefetch::kSlotPilotCount] = {};
        corridorwall::Barrier g_railWall[1 + prefetch::kSlotPilotCount] = {};
        ULONGLONG g_railWallAt[1 + prefetch::kSlotPilotCount] = {};
        unsigned g_pathPhase[1 + prefetch::kSlotPilotCount] = {}, g_pathRaysAsked[1 + prefetch::kSlotPilotCount] = {}, g_pathRayHits[1 + prefetch::kSlotPilotCount] = {},
            g_pathRayFaces[1 + prefetch::kSlotPilotCount] = {};
        bool g_pathRayMode[1 + prefetch::kSlotPilotCount] = {};
        double g_bodyAt[1 + prefetch::kSlotPilotCount] = {};  // world truth A: last body check per pilot
        // World truth B1: a body-check touch stands as a stopping shape for bodyHold s; the nearest eased hit ahead triggers the check.
        struct BodyTouch{double x=0,y=0,z=0,until=0;std::uint32_t owner=0;int layer=-1;};BodyTouch g_bodyTouch[1 + prefetch::kSlotPilotCount];
        double g_easedNear[1 + prefetch::kSlotPilotCount] = {};
        unsigned g_truthStat[1 + prefetch::kSlotPilotCount][4] = {};  // B1 since the last plan.context: dropped explained, eased, kept, touch shapes
        unsigned g_aimHow[1 + prefetch::kSlotPilotCount][3] = {};  // A1: ray ends aimed by 0 authored, 1 survey floor, 2 car offset
        ULONGLONG g_aimLogAt[1 + prefetch::kSlotPilotCount] = {};
        ULONGLONG g_railHighLog[1 + prefetch::kSlotPilotCount] = {};
        unsigned g_pathLayers[1 + prefetch::kSlotPilotCount][64] = {}, g_hullHits[1 + prefetch::kSlotPilotCount] = {}, g_hullAnswers[1 + prefetch::kSlotPilotCount] = {};
        // Smoothed aim (2026-11-05 jitter pass): the look-ahead bearing may swing at most 2 rad/s, so a corner or a segment change does
        // not jump the steering target.
        void RailAim(int idx, double raw, double now)
        {
            const double dt = std::clamp(now - g_railBearingAt[idx], 0.0, 0.1);
            const double gap = std::remainder(raw - g_railBearing[idx], 6.283185307179586);
            g_railAimJump[idx] = std::fabs(gap);
            if (g_railBearingAt[idx] <= 0 || now - g_railBearingAt[idx] > 0.5) g_railBearing[idx] = raw;
            else g_railBearing[idx] += std::clamp(gap, -2.0 * dt, 2.0 * dt);
            g_railBearingAt[idx] = now;
        }
        // Far rail (2026-11-05, the user: "vehicles also not moving till we're close"): a traffic car whose model is not loaded has no
        // physics body, so nothing moves it. It is moved along its lane by writing its position and heading directly, the way the
        // mod already keeps an unloaded driver on its vehicle (the game's position setter would take a seated driver out). Lane
        // speed, the lane's own holds and stop lines, and a gap behind the car ahead; nothing while the game is paused. The normal
        // rail takes over from the same spot when the model loads.
        double g_farAt[1 + prefetch::kSlotPilotCount] = {};
        ULONGLONG g_farLogAt[1 + prefetch::kSlotPilotCount] = {};
        bool FarWrite(std::uintptr_t vehicle, double x, double y, double z, double yaw)
        {
            __try {
                float* p = reinterpret_cast<float*>(vehicle + off::kRefPosition); p[0] = float(x); p[1] = float(y); p[2] = float(z);
                float* r = reinterpret_cast<float*>(vehicle + off::kRefRotation); r[2] = float(yaw);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        bool FarHas3D(std::uintptr_t vehicle)
        {
            __try { const auto loaded = At<std::uintptr_t>(vehicle, off::kRefLoadedData); return loaded && At<std::uintptr_t>(loaded, off::kLoadedData3D); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return true; }  // unknown: leave it to the game
        }
        // Cruise boost: the rail's target speed may rise when only the car's top speed holds it back (corners still win).
        int g_scriptLogged[1 + prefetch::kSlotPilotCount] = {};  // Script mode: last state logged per pilot (0 off, 1 on)
        bool g_railFree[1 + prefetch::kSlotPilotCount] = {};       // this frame only top speed limits the car
        double g_railCorner[1 + prefetch::kSlotPilotCount] = {};   // this frame's corner speed limit (1e9 none)  // when this pilot last drove a Drive lane on lane waypoints
        ULONGLONG g_navWantAt[1 + prefetch::kSlotPilotCount] = {};
        planrun::Odometer g_planOdo[1 + prefetch::kSlotPilotCount];  // real distance along the road (the lane window's station stays near 6 m)
        const char* g_travelWhy[1 + prefetch::kSlotPilotCount] = {}; bool g_travelAir[1 + prefetch::kSlotPilotCount] = {}, g_travelBlock[1 + prefetch::kSlotPilotCount] = {}; double g_travelAt[1 + prefetch::kSlotPilotCount] = {};  // G37: the last travelmode reason
        planrays::Budget g_rayBudget;  // step 6: path sweeps shared across plan cars, nearest first
        ULONGLONG g_rayBudgetLogAt = 0;
        contactevent::Bus<64> g_contacts;  // step 6, G6: one event per contact; World (a shape that expires) and Progress listen, health counts
        struct JobLineState { FvoLaneGuidanceV1 lane{}; double at = -1, gx = 0, gy = 0; bool ok = false; };  // GD-1: each pilot's job line
JobLineState g_jobLine[1 + prefetch::kSlotPilotCount];
const char* g_jobLineWhy[1 + prefetch::kSlotPilotCount] = {};
// MB-4: who drove each pilot slot on its last update (plan.ownermap). Read only by the trace; decides nothing.
const char* g_driverOwner[1 + prefetch::kSlotPilotCount] = {};
double g_driverOwnerAt[1 + prefetch::kSlotPilotCount] = {};
double g_ownerSummaryAt = -1;
// GD-1 switch: settings.ini [traffic] planJobs (default 1 in test builds, 0 in FVO_RELEASE). Re-read when the file changes; traced.
bool JobLinesOn()
{
#ifdef FVO_RELEASE
	constexpr int kDefault = 0;
#else
	constexpr int kDefault = 1;
#endif
	static fvopaths::Watch watch; static std::atomic<bool> on{ kDefault != 0 };
	if (watch.Changed(fvopaths::Settings())) {
		const bool now = GetPrivateProfileIntW(L"traffic", L"planJobs", kDefault, fvopaths::Settings().c_str()) != 0;
		on.store(now); logger::Write("OBS system=plan.door vehicle=00000000 event=job_lines_switch on=%d reason=settings_traffic_planJobs default=%d", int(now), kDefault);
	}
	return on.load();
}
// CB-2 (combat on the plan): combat's aim point and whether it may go on the plan this frame (PilotCombatAim; stale after 300 ms).
struct CombatAim { double x = 0, y = 0, z = 0; ULONGLONG at = 0; bool onPlan = false, aim = false; };
CombatAim g_combatAim[1 + prefetch::kSlotPilotCount];
ULONGLONG g_combatLineAt[1 + prefetch::kSlotPilotCount] = {};  // CB-4: a combat line drove this pilot at (ms): the actuator follows the plan facing
// F1 switch (variant, plan card F1): settings.ini [traffic] planVolumeEverywhere (default 1 in test builds, 0 in FVO_RELEASE). On: a planned
// vehicle with no road surface under its line (a flight line, a combat or job line, an open-air shaft) also casts its own ray volume, as one
// more World source beside the older ones. Off: only a car on an authored road surface casts it, as before. Traced.
bool VolumeEverywhereOn()
{
#ifdef FVO_RELEASE
	constexpr int kDefault = 0;
#else
	constexpr int kDefault = 1;
#endif
	static fvopaths::Watch watch; static std::atomic<bool> on{ kDefault != 0 };
	if (watch.Changed(fvopaths::Settings())) {
		const bool now = GetPrivateProfileIntW(L"traffic", L"planVolumeEverywhere", kDefault, fvopaths::Settings().c_str()) != 0;
		on.store(now); logger::Write("OBS system=world.volume vehicle=00000000 event=volume_everywhere_switch on=%d reason=settings_traffic_planVolumeEverywhere default=%d", int(now), kDefault);
	}
	return on.load();
}
// CB-7 switch: settings.ini [traffic] planCombat (default 1 in test builds, 0 in FVO_RELEASE). Off: today's stick path. Traced.
bool CombatLinesOn()
{
#ifdef FVO_RELEASE
	constexpr int kDefault = 0;
#else
	constexpr int kDefault = 1;
#endif
	static fvopaths::Watch watch; static std::atomic<bool> on{ kDefault != 0 };
	if (watch.Changed(fvopaths::Settings())) {
		const bool now = GetPrivateProfileIntW(L"traffic", L"planCombat", kDefault, fvopaths::Settings().c_str()) != 0;
		on.store(now); logger::Write("OBS system=plan.combat vehicle=00000000 event=combat_lines_switch on=%d reason=settings_traffic_planCombat default=%d", int(now), kDefault);
	}
	return on.load();
}
// TY-1 switch: settings.ini [traffic] railTyres (default 1 in test builds, 0 in FVO_RELEASE). On: a rail car is driven by the game's
// own throttle/brake pedals and steering (railtyres.h); the rail step only measures. Off: today's velocity and spin writes. Traced.
bool RailTyresOn()
{
#ifdef FVO_RELEASE
	constexpr int kDefault = 0;
#else
	constexpr int kDefault = 1;
#endif
	static fvopaths::Watch watch; static std::atomic<bool> on{ kDefault != 0 };
	if (watch.Changed(fvopaths::Settings())) {
		const bool now = GetPrivateProfileIntW(L"traffic", L"railTyres", kDefault, fvopaths::Settings().c_str()) != 0;
		on.store(now); logger::Write("OBS system=traffic.tyres vehicle=00000000 event=switch on=%d reason=settings_traffic_railTyres default=%d", int(now), kDefault);
	}
	return on.load();
}
// TY-3 switch: settings.ini [traffic] railTyresReverse (default 1 in test builds, 0 in FVO_RELEASE). On: tyre mode backs up on the tyres
// too, with one signed pedal (railtyres::StepSigned). Off: backing up rides the rail (TY-2). Only read while railTyres is on. Traced.
bool RailTyresReverseOn()
{
#ifdef FVO_RELEASE
	constexpr int kDefault = 0;
#else
	constexpr int kDefault = 1;
#endif
	static fvopaths::Watch watch; static std::atomic<bool> on{ kDefault != 0 };
	if (watch.Changed(fvopaths::Settings())) {
		const bool now = GetPrivateProfileIntW(L"traffic", L"railTyresReverse", kDefault, fvopaths::Settings().c_str()) != 0;
		on.store(now); logger::Write("OBS system=traffic.tyres vehicle=00000000 event=reverse_switch on=%d reason=settings_traffic_railTyresReverse default=%d", int(now), kDefault);
	}
	return on.load();
}
bool CombatAimFresh(int idx, ULONGLONG now) { return idx >= 0 && idx < int(sizeof g_combatAim / sizeof g_combatAim[0]) && g_combatAim[idx].at && now >= g_combatAim[idx].at && now - g_combatAim[idx].at < 300; }
bool CombatLineNow(int idx, ULONGLONG now) { return idx >= 0 && idx < int(sizeof g_combatLineAt / sizeof g_combatLineAt[0]) && g_combatLineAt[idx] && now >= g_combatLineAt[idx] && now - g_combatLineAt[idx] < 300; }
// CB-4: the hover actuator's split for a combat line with a facing while moving (planhover::FacingWhileMoving): the plan's travel
// bearing and speed, read by the hover's thrust and strafe channels in the same frame (stale after 300 ms).
struct PlanSplit { ULONGLONG at = 0; double travel = 0, speed = 0; };
PlanSplit g_planSplit[1 + prefetch::kSlotPilotCount];
// One-authority step 7 (notes/redesign-20261112/ONE_AUTHORITY_DESIGN.md section 16): with [traffic] planEnergy on (test builds), the energy
// Plan is the one decision for every car; the legacy pipeline stays in the source behind planEnergy=0 for one release.
bool g_testActive[1 + prefetch::kSlotPilotCount] = {};          // a spot test owns this car now (it keeps its own pulse; D12 exception, below)
ULONGLONG g_energyOwnsAt[1 + prefetch::kSlotPilotCount] = {};   // when the energy plan last decided this car (GetTickCount64)
double g_energyAuthLogAt[1 + prefetch::kSlotPilotCount] = {};
inline bool EnergyOwns(int idx) { return idx >= 0 && idx < int(sizeof g_energyOwnsAt / sizeof g_energyOwnsAt[0]) && g_energyOwnsAt[idx] && GetTickCount64() - g_energyOwnsAt[idx] < 300; }
goaldoor::Door<1 + prefetch::kSlotPilotCount> g_door;  // step 4: one way in for every goal; Route reads ending and facing here
        // planHover (step 4): the plan's Mode decides; the old selector's answer is kept to log disagreements, and the plan's last mode is
        // what the lane driver is given on the next update (the plan is committed with its own latch, so it cannot flicker).
        bool g_planFlies[1 + prefetch::kSlotPilotCount] = {};
        bool g_driveOnlyLane[1 + prefetch::kSlotPilotCount] = {};  // NH-1 / 2026-11-23: drive-only lane never hovers or climbs
        double g_planStepAt[1 + prefetch::kSlotPilotCount] = {};  // when planrun::Step last ran for this pilot (a plan older than 1 s is not fresh)
        double g_hoverLogAt[1 + prefetch::kSlotPilotCount] = {};
        ULONGLONG g_planHudAt = 0;
        double g_planHitsAt[1 + prefetch::kSlotPilotCount] = {};  // plan.hits trace clock per pilot
        double g_surfaceLogAt[1 + prefetch::kSlotPilotCount] = {}; double g_wholeBoxLogAt[1 + prefetch::kSlotPilotCount] = {};  // WB-1  // plan.surface trace clock per pilot
        // Look-ahead doubt probe: spots each car probed lately, its counters for the 1 s trace, and the shared per-frame ray budget (main thread).
        planprobe::Recent g_probeRecent[1 + prefetch::kSlotPilotCount];
        struct ProbeStats { int targets = 0, floorTargets = 0, rays = 0, floor = 0, face = 0, clear = 0, noAnswer = 0, budgetSkipped = 0, wrongThread = 0, share = 0; double logAt = 0; };
        ProbeStats g_probeStats[1 + prefetch::kSlotPilotCount];
        unsigned long long g_probeFrame = 0; int g_probeRays = 0;
        int g_probeAskers = 0, g_probeAskersLast = 1;  // W1: cars that asked for probe rays this frame and last frame (fair share)
        worldtrust::CleanStore g_clean; std::mutex g_cleanLock;  // T1: clean probe face rays (main thread adds, World reads)
        struct TrustStat { int clean = 0, measured = 0, kept = 0; double logAt = 0; } g_trustStat[1 + prefetch::kSlotPilotCount];
        spottest::State g_spot[1 + prefetch::kSlotPilotCount]; std::uint32_t g_spotVehicle[1 + prefetch::kSlotPilotCount] = {};  // ST-A: per pilot, reset when the pilot's vehicle changes
#ifndef FVO_RELEASE
        spottest::MotionEvidence g_spotMotion[1 + prefetch::kSlotPilotCount];
        spottest::FactoryTest g_factoryTest[1 + prefetch::kSlotPilotCount];
        std::unordered_map<std::uint32_t, spottest::MotionProof> g_spotProof;  // under g_spotLock
        std::uint64_t g_stockPulseOwner = 0, g_stockPulseAt = 0;  // one live private goal-test request
        // Lab stock in flight (stockfloat.h): the commanded ABSOLUTE float height per pilot (main thread; EnergyStep writes, Fly reads), and the
        // spot test's last pulse command (serial, ms) so the next frame can tell the test that its pulse was applied.
        stockfloat::Hold g_stockFloat[1 + prefetch::kSlotPilotCount];
        struct StockPulseIssued { std::uint64_t serial = 0, at = 0; } g_stockPulseIssued[1 + prefetch::kSlotPilotCount];
#endif
        std::mutex g_spotLock; std::unordered_map<std::uint32_t, int> g_spotResult; std::unordered_map<std::uint32_t, selfreport::Record> g_selfReport; std::unordered_map<std::uint32_t, FvoPairHealthSampleV1> g_healthSample;  // CH-1 under g_spotLock
        std::unordered_map<std::uint32_t, ULONGLONG> g_spotRequest;  // CH-4: Health asked for a spot test at (ms), under g_spotLock  // PF-1 under g_spotLock     // ST-B: the exported fact by vehicle; cleared on load
        planowner::Table g_owners;  // step 5: read-only owner and rank copy in the core, rebuilt each frame, never saved
        std::mutex g_ownersLock;    // the addon's Census (FVO_TrafficCensusPublishV1) may call from its own tick; mod-owned data only
        // Who a ray hit belongs to (trace only, decides nothing; user 2026-11-07: "check the id of anything hit by rays and find out what
        // it belongs to"). The hit's owner is the reference that owns the hit body (prefetch RayHit.formId). Resolved once per id:
        // reference -> base form (+0x98), base form type (TESForm +0x2E), base bounds (TESBoundObject +0x50/+0x5C), reference position
        // (+0x8C). Plugin = the id's load-order prefix (FE/FD = light/medium; resolve against Plugins.txt). Reads only, under __try.
        struct HitWho { bool ok = false; std::uint32_t base = 0; std::uint8_t refType = 0, baseType = 0; float lo[3] = {}, hi[3] = {}, pos[3] = {}; };
        HitWho ReadHitWho(std::uint32_t id){
            HitWho w;if(!id)return w;const auto ref=runtimeforms::LookupChecked(id);if(!ref)return w;
            __try{
                w.refType=*reinterpret_cast<const std::uint8_t*>(ref+0x2E);
                const auto base=*reinterpret_cast<const std::uintptr_t*>(ref+0x98);
                for(int i=0;i<3;++i)w.pos[i]=reinterpret_cast<const float*>(ref+off::kRefPosition)[i];
                if(base){w.base=*reinterpret_cast<const std::uint32_t*>(base+off::kFormId);w.baseType=*reinterpret_cast<const std::uint8_t*>(base+0x2E);
                    for(int i=0;i<3;++i){w.lo[i]=*reinterpret_cast<const float*>(base+0x50+i*4);w.hi[i]=*reinterpret_cast<const float*>(base+0x5C+i*4);}}
                w.ok=true;
            }__except(EXCEPTION_EXECUTE_HANDLER){w=HitWho{};}
            return w;
        }
        std::mutex g_hitWhoLock;std::unordered_map<std::uint32_t,HitWho> g_hitWho;
        HitWho HitOwner(std::uint32_t id){
            {std::lock_guard<std::mutex> lk(g_hitWhoLock);const auto it=g_hitWho.find(id);if(it!=g_hitWho.end())return it->second;}
            const HitWho w=ReadHitWho(id);
            std::lock_guard<std::mutex> lk(g_hitWhoLock);if(g_hitWho.size()>4096)g_hitWho.clear();g_hitWho[id]=w;
            if(id)logger::Write("OBS system=plan.hitwho ref=%08X ok=%d plugin_prefix=%02X ref_type=%u base=%08X base_type=%u base_plugin_prefix=%02X ref_pos=%.2f,%.2f,%.2f bounds_min=%.2f,%.2f,%.2f bounds_max=%.2f,%.2f,%.2f size=%.1fx%.1fx%.1f reason=first_hit_by_a_ray",
                id,int(w.ok),id>>24,w.refType,w.base,w.baseType,w.base>>24,w.pos[0],w.pos[1],w.pos[2],w.lo[0],w.lo[1],w.lo[2],w.hi[0],w.hi[1],w.hi[2],w.hi[0]-w.lo[0],w.hi[1]-w.lo[1],w.hi[2]-w.lo[2]);
            return w;
        }
        fieldbox::Containment g_fieldHold[1 + prefetch::kSlotPilotCount];  // step 3: one-way containment per pilot
        int g_fieldWas[1 + prefetch::kSlotPilotCount] = {}; double g_fieldLogAt[1 + prefetch::kSlotPilotCount] = {};
        std::unordered_map<std::uint32_t, int> g_planPilot;
        std::uint32_t g_planLogged[1 + prefetch::kSlotPilotCount] = {};
        double g_planReportAt[1 + prefetch::kSlotPilotCount] = {}, g_planSignsAt = 0;
        // PC-1 (2026-11-08): per-car planning cost aggregator, logged every 5 s as OBS system=plan.cost.
        struct PlanCostAgg {
            std::mutex lock;
            double logAt = 0, summaryAt = 0;
            unsigned long long lastFrame = 0;
            unsigned frames = 0, calls = 0, replans = 0, kept = 0, nearN = 0, midN = 0, farN = 0, urgentN = 0;
            unsigned staggered = 0, workerSubmitted = 0, workerMissed = 0, workerTrials = 0;
            double frameCurUs = 0, frameMaxUs = 0, carMaxUs = 0, totalUs = 0;
            double surfaceUs = 0, worldUs = 0, probeUs = 0, buildUs = 0, collideUs = 0, speedUs = 0, scoreWallUs = 0, arbiterUs = 0, bodyUs = 0;
            std::uint64_t activeMask[4] = {};
        } g_planCost;
        trafficcoordinator::Ownership g_laneOwnership[1 + prefetch::kSlotPilotCount];

		bool g_farHold[1 + prefetch::kSlotPilotCount] = {};
		// A collision: the velocity at the start of a step differs sharply from what we wrote at the end of the last.
		constexpr float kCollisionDelta = 4.0f;   // m/s change at least ...
		constexpr float kCollisionShare = 0.35f;  // ... and this share of the speed
		constexpr float kCollisionMinSpeed = 2.0f;
		// Player flights (2026-10-17 20:03-20:04: hover ended 0.03-0.9 s after switching on, "13-89 m/s knocked off", while
		// lifting off ground it touched or scraping it low): contacts in the first kContactGraceMs or during lift-off are
		// ignored, a push up is ground under it (held above it, like autopilot), and only a hard side hit ends the flight.
		constexpr ULONGLONG kContactGraceMs = 2000;
		constexpr ULONGLONG kPushGraceMs = 1000;  // after a push up: contacts ignored this long
		constexpr ULONGLONG kHitGraceMs = 500;    // after a hard side hit: contacts ignored this long
		constexpr float kCrashDelta = 20.0f;      // m/s: a sideways change this big is a hard hit (logged; the flight goes on)

		double Wrap(double a)
		{
			if (!std::isfinite(a) || std::fabs(a) > 1000.0) {
				return 0.0;
			}
			while (a > 3.14159265358979) a -= 6.28318530717959;
			while (a < -3.14159265358979) a += 6.28318530717959;
			return a;
		}

		// ---- Shared between the frame (inputs, ground, assist) and the physics step (the model) ------------------
		struct Shared
		{
			std::mutex lock;
			hulllogic::Limits hull;        // the limits in use: hulllogic::Flight(hullBase, physical, row's turn share) (FC-1)
			carcap::FlightRow row;         // FC-1: the flight gear's handling row (the player's gear; the autopilot's FG-1 gear)
			// FC-1 handling trace (written by the step, read and reset by the flight tick's 1 s line): worst rate error, worst drift,
			// bounds that held, and (physical) the lift's collective / trim / vertical-speed error to show the lift is untouched.
			double trkRateErr = 0, trkDrift = 0, trkSidePush = 0, trkCollective = 0, trkTrim = 0, trkVzErr = 0;
			unsigned trkSteps = 0, trkAngCapped = 0, trkTorqueCapped = 0, trkSideCapped = 0; bool trkPhysical = false;
			hulllogic::Limits hullBase;    // this vehicle's own size-based limits (BindBody)
			bool active = false;
			float px = 0, py = 0, pz = 0;  // the rover reference's position (to find its body)
			// commands (frame -> step)
			double thrustAcc = 0;          // m/s^2 along the nose
			bool thrusting = false;
			bool braking = false;          // avoidance stop: decelerate along the velocity
			// Driving (not hovering): the gear caps the rover's ground speed at a share of its normal top speed, learned
			// from flat-out driving in gear 4 (W held).
			// Autopilot (nobody aboard): the game's own vehicle step wipes the horizontal speed every step (a parked
			// brake: 2026-10-17 13:28, speed stuck at one step's push, 0.3 m/s), so the model carries its own.
			bool autopilot = false;
			// After a summon ends, the empty rover shot off at high speed (2026-10-17 13:42: 186 m in 3 s with hover
			// off). For kGuardMs its sideways motion is cancelled every step; the fastest speed caught is logged.
			std::uintptr_t guardVehicle = 0;
			ULONGLONG guardUntil = 0;
			float guardMax = 0;
			bool groundOn = false;
			bool groundLearn = false;
			double groundFrac = 1.0;
			double groundTop = 28.0;       // m/s, learned
			double yawWant = 0;            // direct yaw input (camera gap and bank turn added in the step)
			double view = 0;               // the camera's world heading (atan2(x, y) of its forward)
			bool haveView = false;
			double pitchIn = 0, rollIn = 0, lift = 0;  // -1 .. 1 (pitchIn unused since 0.58: the camera aims the pitch)
			double pitchAim = 0;           // wanted nose pitch from the camera (rad)
			// Autopilot channels (2026-10-17, "full hooks"): sideways push, a vertical speed in place of the hold, and a
			// roll attitude in place of the roll rate.
			double strafe = 0;             // -1 .. 1
			bool vzOn = false;
            double fieldClearance = -1; // active road field uses the measured hull bottom
			double vz = 0;                 // m/s
			bool rollAimOn = false;
			double rollAim = 0;            // rad
			// Free roll (freeroll.h, the settings menu): the player's own hover turns about its body axes with no roll limit.
			// The bank test (the , key with the detailed log on) uses the same body-axis turning for its held bank.
			bool freeRoll = false, assist = true, bankTest = false;
			bool physical = false;
			bool physicalFault = false;
			// Rail drive (2026-11-05, the user: "just move the vehicle directly"): a traffic car on a Drive lane is moved by setting its
			// body's horizontal velocity and yaw spin after each physics step (the same velocity write as the ground speed cap and
			// the same spin call as the hover). Wheels, steering, brake and reverse play no part; gravity, the road and collisions do.
			bool railOn = false;
			bool railTyres = false;  // TY-1: the tyres move it (pedals + steer); the rail step only measures, writes no velocity or spin
			float railVx = 0, railVy = 0, railYaw = 0;  // m/s world, rad/s about world up (+ = turn left)
			float railAim = 0, railMaxYaw = 1.6f; bool railAimValid = false;  // 60 Hz closed-loop heading target (prevents open-loop fishtail)
			corridorwall::Barrier railWall{};  // 60 Hz virtual guardrail from authored road/yellow-box edges
			ULONGLONG railUntil = 0;
			unsigned railSteps = 0, railFaults = 0;
			bool railAir = false;  // the rail's jump is on (the game's vertical boost): the vertical motion stays the game's
			double railCommand = -1;
			float railRoad[3] = { 0, 0, 1 }; ULONGLONG railRoadAt = 0;  // the road's up vector from the lane's grade (railforce road plane)
			double railFloorZ = 0; ULONGLONG railFloorAt = 0;  // authored road floor Z for unloaded-collision support beyond ~240 m
			// Rail probes (2026-11-05, "the old print everywhere trick"): what each part did to this car's speed. Written by the physics
			// threads without the lock (speeds in mm/s); read and reset by the rail step's 4 Hz probe line.
			std::atomic<std::uintptr_t> railMotion{0};       // the body's motion record (velocity at +0x60) the rail last wrote
			std::atomic<long long> probeNativeDrop{0}, probeDampDrop{0}, probeClampDrop{0};
			std::atomic<unsigned> probeNativeSteps{0}, probeDampCalls{0}, probeClampCalls{0}, probeRailSteps{0}, probeGuardClears{0};
			double probeWrote = 0, probeMeasured = 0, probeTarget = 0, probeNz = 1, probeSlip = 0; bool probeGrounded = false, probeTipped = false;
			ULONGLONG probeAt = 0;
			// Wheel contacts (read-only probe, 2026-11-05; notes/WHEEL_CONTACT_RESEARCH_20261105.md): read after the native step.
			int probeWheels = 0, probeWheelsTouching = 0; double probeWheelZ = 0, probeWheelUp = 0, probeRoadZ = -1;
			vtol::State physicalState{};
			// Physical ownership of this pilot's chassis, read lock-free by the native assist hooks (PhysicalOwns).
			// needsDriver: the player's manual hover is owned only while the player drives it; AI flights have no driver check.
			std::atomic<bool> physOn{false}, physNeedsDriver{false};
			std::atomic<std::uintptr_t> physInstance{0}, physChassis{0};
			std::atomic<std::uint32_t> physBody{0};
			std::atomic<unsigned> physSteps{0};
			unsigned physWiped = 0;        // AI: consecutive steps where the game's own step wiped the speed we built
			bool haveAim = false;
			bool landing=false;
            bool holding = false;          // level, no lift keys: hold the height
			double hold = 3.0;             // m above the reference ground
			double refGround = 0;          // elevation assist reference (absolute z)
			double groundUnder = 0;        // ground under the rover (absolute z)
			bool groundOk = false;
			double cap = kSpeedCap;
			// model state (step)
			double wYaw = 0, wPitch = 0, wRoll = 0;  // spin, rad/s
			double gEst = 9.8;             // gravity measured from the body, m/s^2
			// report (step -> frame)
			float v[3] = {};               // the body's velocity after our push
			float bodyPitch = 0, bodyRoll = 0;
			double bodyHeading = 0;        // atan2(forward.x, forward.y) of the body: the same frame as the camera
			double gap = 0, camYaw = 0;
			int fixedVel = 0;              // steps where an absurd body velocity was replaced
			// Attitude measurement (stage 0, notes/FLIGHT_MODEL_STUDY_20261023.md, pilot 0 only): what the game did to the spin we wrote
			// one step earlier (its upright actions, contacts, air control). "Pull" = the part about the roll / pitch axis toward level.
			float lastW[3] = {};
			bool haveLastW = false;
			unsigned measN = 0;
			double measRollPull = 0, measPitchPull = 0, measMax = 0;
			double measW[3] = {};          // the last spin written (world)
			bool matched = false;
			unsigned steps = 0;
			std::uint64_t stepSerial = 0;
            std::uint64_t handoffSteps = 0;
            ULONGLONG handoffStepAt = 0;
            std::uintptr_t handoffStepInstance = 0;
            std::uint32_t handoffStepBody = 0;
			std::atomic<std::uint64_t> testSteps{0};  // ST-A: physics steps of any kind, never reset (the spot test counts its pulses in these)
			ULONGLONG lastStepAt = 0;
			std::uintptr_t lastStepInstance = 0;
			// collision detection
			float lastV[3] = {};
			bool haveLast = false;
			bool collided = false;
			bool airborne = false;         // frame: clear of the ground (gravity is measured only then)
			unsigned launches = 0;         // steps where an upward kick from a contact was capped
			// Carried speed (autopilot): a run of steps where the body's speed read ~0 and ours was restored; if the body did not move
			// as that speed says, it is blocked and the carry stops for a while.
			unsigned carryRun = 0;
			ULONGLONG carryT0 = 0, carryBlockedUntil = 0;
			float carryX = 0, carryY = 0;
			std::atomic<ULONGLONG> impactAt{0};
            float collisionDelta = 0;
			bool collisionUp = false;      // the contact pushed the vehicle up (something under it)
			// Every pilot, including the player and ground driving, owns one reference and its published body ID.
			std::atomic<std::uintptr_t> vehicle{ 0 };
			std::atomic<std::uint32_t> bodyId{ 0 };
			std::uint32_t sceneBodies[16]={},sceneCount=0;
			// The physics step's `this` is the Havok vehicle (hknpBSVehicleInstance, vtable 0x144D66C98, step = slot 5
			// FUN_1423CB260), NOT the vehicle reference: comparing it with the reference never matched (2026-10-17 16:52:
			// an NPC pilot got 0 steps; the release guard and the ground gear cap never ran either). The instance is
			// cached only after its full body ID matches the reference's collision body in the shared world snapshot.
			std::atomic<std::uintptr_t> instance{ 0 };
			std::uintptr_t guardInstance = 0;
			// The real car read from the Havok vehicle (carmodel.h): steering lock, wheelbase, input shaping. 0 = not read.
			std::atomic<float> carInput{ 0 };
			std::atomic<float> carLock{ 0 }, carWheelbase{ 0 }, carSlope{ .8f }, carInitial{ .7f }, carFull{ 45.f };
		};
		// Per-pilot functions bind local `g` / `g_sh` to their pilot's state, hiding pilot 0's on purpose.
#pragma warning(disable : 4459)
		// Pilots: 0 is the player's rover (escort: summon / follow / the player's own hover); 1..3 fly NPC-driven
		// vehicles with the same model, terrain assist and avoidance.
		constexpr int kPilots = 1 + prefetch::kSlotPilotCount;
		Shared g_shs[kPilots];
		Shared& g_sh = g_shs[0];
		const char* Who(int idx)
		{
			static char names[kPilots][16] = {};
			static bool made = false;
			if (!made) {
				made = true;
				std::snprintf(names[0], sizeof names[0], "Rover");
				for (int i = 1; i < kPilots; ++i) std::snprintf(names[i], sizeof names[i], "NPC rover %d", i);
			}
			return names[std::clamp(idx, 0, kPilots - 1)];
		}

		// The camera's world heading. Two readings of the camera root's world rotation (+0x80, 3 rows of 16 bytes):
		// forward as column 1 (+0x84, +0x94) or as row 1 (+0x90, +0x94). For a turn they are mirror images. 0.57.0 used
		// column 1 and the rover turned the opposite way to the view and settled at an offset (user: "inverted" and
		// "an angular offset"), so row 1 is the default; while driving (the game camera trails the rover) the one that
		// follows the rover's turns is confirmed (logged).
		struct CamPick
		{
			int pick = 1;            // 0 column, 1 row
			double lastC = 0, lastR = 0, lastH = 0;
			bool have = false;
			double sumC = 0, sumR = 0;
			bool told = false;
		} g_camPick;

		bool CameraHeadings(double& col, double& row)
		{
			const auto cam = Ptr(addr::kPlayerCameraPtr);
			const auto root = cam ? At<std::uintptr_t>(cam, 0x08) : 0;
			if (!root) {
				return false;
			}
			const float c0 = At<float>(root, 0x84), c1 = At<float>(root, 0x94);
			const float r0 = At<float>(root, 0x90), r1 = At<float>(root, 0x94);
			if (!std::isfinite(c0) || !std::isfinite(c1) || !std::isfinite(r0) || !std::isfinite(r1) || (std::fabs(c0) + std::fabs(c1) < 1e-4f) ||
				(std::fabs(r0) + std::fabs(r1) < 1e-4f)) {
				return false;
			}
			col = std::atan2(c0, c1);
			row = std::atan2(r0, r1);
			return true;
		}

		bool CameraHeading(double& yaw)
		{
			double c, r;
			if (!CameraHeadings(c, r)) {
				return false;
			}
			yaw = r; // camera world forward is row 1 in both rover perspectives
			return true;
		}

		// The camera's pitch (its forward's z, in the same reading as the heading: row 1 +0x98, column 1 +0xA4).
		bool CameraPitch(double& pitch)
		{
			const auto cam = Ptr(addr::kPlayerCameraPtr);
			const auto root = cam ? At<std::uintptr_t>(cam, 0x08) : 0;
			if (!root) {
				return false;
			}
			const float z = At<float>(root, 0x98);
			if (!std::isfinite(z)) {
				return false;
			}
			pitch = std::asin(std::clamp(double(z), -1.0, 1.0));
			return true;
		}

		bool CameraAimReading(std::uintptr_t vehicle, double& pitch, bool& first)
		{
			const auto cam=Ptr(addr::kPlayerCameraPtr);
			const auto state=cam?At<std::uintptr_t>(cam,0x10):0;
			first=state && state==At<std::uintptr_t>(cam,0x188);
			if(state && At<std::uintptr_t>(state,0)==game::Base()+addr::kVehicleCameraStateVtbl)
				first=At<std::uint8_t>(state,0x74)==1; // FUN_140FB4D80: first-person pitch/yaw limits
			if(!first) return CameraPitch(pitch);
			const auto root=cam?At<std::uintptr_t>(cam,0x08):0;
			if(!root)return false;
			static thread_local world::Snapshot snap; if(!world::Vehicles(snap))return false;
			for(std::uint32_t i=0;i<snap.count;++i)if(snap.v[i].ref==vehicle) {
				const auto&r=snap.v[i].rot;
				const double f[]={At<float>(root,0x90),At<float>(root,0x94),At<float>(root,0x98)};
				pitch=hoverlogic::RelativePitch(r,f);
				return std::isfinite(pitch);
			}
			return false;
		}

		struct CamRest {
			double pitch=0; bool have=false;
			double rest[2]={}; bool learned[2]={}; int mode=-1;
			void Observe(double reading,bool first,bool learn) {
				const int next=first?1:0;
				if(!learned[next]) {rest[next]=first?0:reading;learned[next]=true;}
				else if(learn&&!first)rest[next]+=(reading-rest[next])*0.01;
				mode=next;pitch=rest[next];have=true;
			}
		} g_camRest;

		struct { bool active=false; float min=0, max=0; } g_firstPersonLimits;
		void FirstPersonLookLimits(bool driving)
		{
			// 140153F20/F50 register these degree settings; 140FB4D80 clamps
			// vehicle first-person pitch to them (stock -45 .. +15 degrees).
			auto& low = At<float>(game::Base(), addr::kVehicleFirstPersonMinPitch);
			auto& high = At<float>(game::Base(), addr::kVehicleFirstPersonMaxPitch);
			if (driving && !g_firstPersonLimits.active) {
				if (!std::isfinite(low) || !std::isfinite(high) || low > high) return;
				g_firstPersonLimits = {true,low,high};
				low = std::min(low, -80.0f); high = std::max(high, 80.0f);
				logger::Write("Vehicle camera: first-person pitch %.0f .. %.0f -> %.0f .. %.0f degrees", g_firstPersonLimits.min, g_firstPersonLimits.max, low, high);
			} else if (!driving && g_firstPersonLimits.active) {
				low = g_firstPersonLimits.min; high = g_firstPersonLimits.max;
				g_firstPersonLimits = {};
			}
		}

		// Drive mode output per pilot (frame thread writes, the input hooks read): the controls for the vehicle's input block.
		handoff::WheelMailbox<DriveInput> g_driveOut[kPilots];

		using Step_t = physicsstep::Callback;
		Step_t g_original = nullptr;
		std::atomic<unsigned> g_nativeStepSamples{0}, g_nativeStepSkipped{0};
		std::atomic<std::uint64_t> g_nativeStepTotal{0};  // load trace; never reset
		std::atomic<float> g_nativeStepSeconds{0};

		// Booster visuals held while hovering. Paid forward boost uses the native fuel fraction (+0x238),
		// recharge delay (+0x23C), and this vehicle configuration's burn/recharge timings.
		using Boost_t = void (*)(std::uintptr_t, std::uintptr_t);
		Boost_t g_boostOriginal = nullptr;
		std::atomic<std::uintptr_t> g_hoverVehicles[kPilots]{};
		std::atomic<std::uintptr_t>& g_hoverVehicle = g_hoverVehicles[0];
		std::atomic<std::uintptr_t> g_clearBoosts[kPilots]{};
		std::atomic<bool> g_forwardBoosts[kPilots]{};
		// The player's own rover (for the boost charge) and a released rover whose boost flags must be cleared: after a
		// summon the empty rover rose 400 m in 7 s (2026-10-17 13:57): its VehicleIsVertBoosting flag stayed set and the
		// charge was refilled for every vehicle not driven.
		std::atomic<std::uintptr_t> g_ownRover{ 0 };
		std::atomic<std::uintptr_t>& g_clearBoost = g_clearBoosts[0];
		std::atomic<bool>& g_forwardBoost = g_forwardBoosts[0];
		// A seated ship pilot has no PlayerVehicle link. Count whether the last driven rover's physics
		// callback keeps running after dismount; comparison only, never dereference a stale reference.
		std::atomic<std::uint32_t> g_manualDriverId{0};
        std::mutex g_saveControlLock;
        fleetownershipstore::PlayerControl g_saveControl;
        std::atomic<std::uintptr_t> g_driverVehicle{ 0 }, g_lastDrivenVehicle{ 0 };
		std::atomic<unsigned> g_physicalSteps{0}, g_physicalFailures{0};
		std::atomic<unsigned> g_physicalReject{0};
		std::atomic<float> g_physicalCollective{0}, g_physicalMass{0};
		std::atomic<float> g_roadGrade[1 + prefetch::kSlotPilotCount]{};
		std::atomic<float> g_wheelHalf[1 + prefetch::kSlotPilotCount]{};  // MS-1: the widest wheel hardpoint off the centre line (m; 0 unknown)
		constexpr double kTyreHalf = .25;  // MS-1: a wheel's hardpoint to the tyre's outer face, with a little to spare  // SP-1: the lane floor's grade just ahead (lane branch -> tyre controller)
		std::atomic<float> g_physicalLinearError{0},g_physicalAngularError{0};
		bool g_physicalHooksReady = false;
		std::atomic<unsigned> g_occupiedSteps{ 0 }, g_unoccupiedSteps{ 0 };

		std::atomic<bool> g_boostRequest{false},g_meterBoost{false};
		std::atomic<std::uintptr_t> g_meterVehicle{0};
		// BO-1: the game's own boost meter of each pilot's vehicle, read after the native boost update as a fact for Speed (never written), and
		// the tyre pedal's demand the actuator measured (Speed's thrust-limited evidence). Times in ms (GetTickCount64); 0 = never seen.
		std::atomic<float> g_boostCharge[kPilots]{};
		std::atomic<bool> g_boostNative[kPilots]{};
		std::atomic<ULONGLONG> g_boostSeenAt[kPilots]{};
		std::atomic<float> g_pedalDemand[kPilots]{};
		std::atomic<ULONGLONG> g_pedalAt[kPilots]{};
		std::atomic<bool> g_meterExhausted{false};
		// BO-2 (2026-11-19, hover cars, Speed): the flying car's forward boost. Speed's request (Output::boost, BoostSpeed) is executed by the hover
		// actuator as the thrust multiplier boostlogic::kMultiplier (the same factor the player's paid boost uses; the caps are not raised). The
		// actuator publishes the request it executed (g_hoverBoostWant) and the evidence Speed reads (the hover throttle before its clamp in
		// g_pedalDemand/g_pedalAt, the speed the hover may reach now, the steering share, and a precise-flight reason). The pilot's hover meter is
		// boostlogic::Meter, stepped in BoostDetour on the vehicle's own native timings: the player's vehicle writes its native meter (as for the
		// player's Shift boost); any other flying car keeps a mod-side meter per pilot and never writes the game's component. Charge 0..1, drains
		// while boosting, recharges after the native delay: never pinned.
		std::atomic<bool> g_hoverBoostWant[kPilots]{}, g_hoverBoostActive[kPilots]{};
		std::atomic<float> g_hoverCap[kPilots]{}, g_hoverSteer[kPilots]{};
		std::atomic<const char*> g_hoverHalt[kPilots]{};
		struct HoverMeter { boostlogic::Meter m; std::uintptr_t vehicle = 0; bool seeded = false; };
		HoverMeter g_hoverMeter[kPilots];  // the game's physics thread only
		void BoostDetour(std::uintptr_t component, std::uintptr_t context)
		{
			const std::uintptr_t ctxVehicle = context ? *reinterpret_cast<std::uintptr_t*>(context + 0x100) : 0;
			int hp = -1;  // the pilot hovering this vehicle
			for (int i = 0; ctxVehicle && i < kPilots && hp < 0; ++i) {
				if (g_hoverVehicles[i].load() == ctxVehicle) hp = i;
			}
			const bool drive = hp >= 0 && g_driveOut[hp].Active();  // driving, not hovering
			const bool hovering = hp >= 0 && !drive;
			const auto vehicle = ctxVehicle;
			const auto driven = g_driverVehicle.load(std::memory_order_relaxed);
			const bool manual=component&&driven&&ctxVehicle==driven;
			const bool hoverAsk=hp>=0&&hovering&&g_hoverBoostWant[hp].load(std::memory_order_relaxed);  // BO-2: Speed's request for a flying car
			const bool request=manual&&(g_boostRequest.load()||hoverAsk);
			float charge=0,delay=0;
			if(manual&&hovering) {
				charge=At<float>(component,0x238);delay=At<float>(component,0x23C);
				// Hover replaces native boost force. The meter still uses this vehicle's native timings.
				At<std::uint8_t>(component,0x240)=0;At<std::uint8_t>(component,0x241)=0;
			} else if(manual) At<std::uint8_t>(component,0x240)=request?1:0;
			g_boostOriginal(component,context);
			if(hp>=0&&component) {  // BO-1: the meter as the game left it (charge, boosting flag), for Speed; read only
				g_boostCharge[hp].store(At<float>(component,0x238),std::memory_order_relaxed);
				g_boostNative[hp].store(At<std::uint8_t>(component,0x240)!=0,std::memory_order_relaxed);
				g_boostSeenAt[hp].store(GetTickCount64(),std::memory_order_relaxed);
			}
			if(manual) {
				if(g_meterVehicle.exchange(vehicle)!=vehicle)g_meterExhausted=false;
				if(hovering) {
					const auto config=context?At<std::uintptr_t>(context,0x110):0;
					const double dt=context?std::clamp(double(At<float>(context,0x16C)),0.0,0.1):0;
					const auto meter=boostlogic::Step(charge,delay,request,g_meterExhausted,dt,
						config?At<float>(config,0x418):3.0,config?At<float>(config,0x420):1.0,config?At<float>(config,0x424):4.0);
					At<float>(component,0x238)=float(meter.charge);At<float>(component,0x23C)=float(meter.delay);
					g_meterBoost.store(meter.active);g_meterExhausted=meter.exhausted;
					if(hp>=0){g_boostCharge[hp].store(float(meter.charge),std::memory_order_relaxed);g_boostNative[hp].store(meter.active,std::memory_order_relaxed);g_hoverBoostActive[hp].store(meter.active,std::memory_order_relaxed);}  // BO-2
				}else g_meterBoost.store(request&&At<float>(component,0x238)>0);
			}
			if(hp>=0&&!hovering)g_hoverBoostActive[hp].store(false,std::memory_order_relaxed);  // BO-2: on wheels the game's own meter is read, not ours
			if(hp>=0&&hp<kPilots&&hovering&&!manual&&component) {  // BO-2: a flying car other than the player's: its own meter, native timings, never written to the game
				auto& hm=g_hoverMeter[hp];
				if(hm.vehicle!=vehicle||!hm.seeded) {  // first step as a flyer (or another vehicle): start from the game's own charge, bounded
					hm=HoverMeter{};hm.vehicle=vehicle;hm.seeded=true;
					const float c0=At<float>(component,0x238);hm.m.charge=std::isfinite(c0)?std::clamp(double(c0),0.0,1.0):1.0;
				}
				const auto config=context?At<std::uintptr_t>(context,0x110):0;
				const double dt=context?std::clamp(double(At<float>(context,0x16C)),0.0,0.1):0;
				hm.m=boostlogic::Step(hm.m.charge,hm.m.delay,g_hoverBoostWant[hp].load(std::memory_order_relaxed),hm.m.exhausted,dt,
					config?At<float>(config,0x418):3.0,config?At<float>(config,0x420):1.0,config?At<float>(config,0x424):4.0);
				g_boostCharge[hp].store(float(hm.m.charge),std::memory_order_relaxed);g_boostNative[hp].store(hm.m.active,std::memory_order_relaxed);
				g_hoverBoostActive[hp].store(hm.m.active,std::memory_order_relaxed);
			} else if(hp>=0&&hp<kPilots&&!hovering)g_hoverMeter[hp].seeded=false;
			const auto set = EngineFn<void (*)(std::uintptr_t, std::uintptr_t, char, int)>(addr::kSetGraphBool);
			for (int i = 0; ctxVehicle && i < kPilots; ++i) {
				if (ctxVehicle == g_clearBoosts[i].load()) {
					set(ctxVehicle + 0x60, game::Base() + addr::kVarVehicleIsVertBoosting, 0, 0);
					set(ctxVehicle + 0x60, game::Base() + addr::kVarVehicleIsBoosting, 0, 0);
					g_clearBoosts[i] = 0;
				}
			}
			if (!hovering) {
				return;
			}
			set(vehicle + 0x60, game::Base() + addr::kVarVehicleIsVertBoosting, 1, 0);
			set(vehicle + 0x60, game::Base() + addr::kVarVehicleIsBoosting, g_forwardBoosts[hp].load() ? 1 : 0, 0);
		}

		// The player's rover body: hknpBody (rows right +0x00, forward +0x10, up +0x20, position +0x30) and its motion
		// (linear velocity +0x60), or null for another vehicle.
		struct Body
		{
			std::uintptr_t body = 0, motion = 0, write = 0, read = 0;
			std::uint32_t id = 0;
		};

		Body FindBody(std::uintptr_t vehicle, float px, float py, float pz, bool anyPosition = false)
		{
			Body b;
			const auto read = At<std::uintptr_t>(vehicle, 0x28);
			b.write = At<std::uintptr_t>(vehicle, 0x30);
			b.read = read;
			if (!read || !b.write) {
				return {};
			}
			b.id = At<std::uint32_t>(vehicle, 0x20);
			b.body = VFn<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(read, 0x70)(read, b.id);
			if (!b.body) {
				return {};
			}
			(void)px;(void)py;(void)pz;(void)anyPosition; // full scene identity, independent of body/reference origin offsets
			b.motion = VFn<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(read, 0xD0)(read, At<std::uint32_t>(b.body, 0x80));
			return b.motion ? b : Body{};
		}

        // WK-1 (2026-11-13 09:56 run: your car stood 24 s with full throttle sent and no physics step; the light-throttle wake never woke it):
        // the game's own wake, as its vehicle component update FUN_14131D5F0 does it every frame: on the instance's read interface
        // (instance+0x28) vt+0x58 says the body is valid, vt+0xB8 its activation state; when that is not 1 (active), the instance's world
        // (instance+0x30) vt+0x140 (body, 1) activates it. Main thread only (where the game calls it). Returns the state before (-1: not done).
        std::atomic<std::uintptr_t> g_wakeQueue[kPilots]{};  // 2026-11-23: off-thread Fly() queues WakeBody for main-thread Tick()
        int WakeBody(std::uintptr_t instance)
        {
            if (!instance || !routesurvey::OnMainThread()) return -1;
            __try {
                const auto b = FindBody(instance, 0, 0, 0, true);
                if (!b.body || !b.read || !b.write || At<std::uint32_t>(b.body, 0x50) != b.id) return -1;
                if (VFn<int (*)(std::uintptr_t, std::uint32_t)>(b.read, 0x58)(b.read, b.id) == 0) return -1;
                const int state = VFn<int (*)(std::uintptr_t, std::uint32_t)>(b.read, 0xB8)(b.read, b.id);
                if (state != 1) VFn<void (*)(std::uintptr_t, std::uint32_t, int)>(b.write, 0x140)(b.write, b.id, 1);
                return state;
            } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
        }
        void ObserveChassisMass(std::uintptr_t vehicle,ULONGLONG now,std::uint64_t epoch)
        {
            __try {
                const auto b=FindBody(vehicle,0,0,0,true);
                if(!b.body||!b.motion||At<std::uint32_t>(b.body,0x50)!=b.id||
                    (At<std::uint32_t>(b.body,0x54)&7)!=2)return;
                const double inverse=vtol::Expand(std::uint16_t(At<std::uint64_t>(b.motion,0x40)>>48));
                if(std::isfinite(inverse)&&inverse>1e-9&&inverse<1000)
                    bodymass::observations.Observe(b.id,float(1/inverse),now,epoch);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        hookcost::Counter g_stepCost;
        struct StepTiming {
            LARGE_INTEGER start{},nativeStart{};std::uint64_t native=0;
            StepTiming(){QueryPerformanceCounter(&start);}
            void BeginNative(){QueryPerformanceCounter(&nativeStart);}
            void EndNative(){LARGE_INTEGER at{};QueryPerformanceCounter(&at);native=at.QuadPart-nativeStart.QuadPart;}
            ~StepTiming(){LARGE_INTEGER at{};QueryPerformanceCounter(&at);g_stepCost.Add(std::uint64_t(at.QuadPart-start.QuadPart)-native,native);}
        };
        void ReportStepCost()
        {
            static ULONGLONG began=0;const auto now=GetTickCount64();
            if(!began){began=now;return;}if(now-began<10000)return;
            hookcost::Samples samples;if(!g_stepCost.Take(samples))return;began=now;
            if(!samples.count)return;
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);const double ms=1000.0/frequency.QuadPart;
            logger::Write("Vehicle physics cost: %llu calls; mod hook %.3f ms avg / %.3f worst; native step %.3f / %.3f; %llu telemetry samples skipped (elapsed call time, threads may overlap)",
                static_cast<unsigned long long>(samples.count),ms*double(samples.hook)/samples.count,ms*samples.maxHook,
                ms*double(samples.native)/samples.count,ms*samples.maxNative,static_cast<unsigned long long>(samples.skipped));
        }

		// Native impulse entry points are confirmed in both writer vtables in the pinned executable. Reject a
		// different interface/body instead of invoking an unknown slot or falling back to a velocity overwrite.
		bool ReadPhysicalMotion(const Body& b, vtol::Motion& motion)
		{
			__try {
				if (!b.body || !b.motion || !b.read || !b.write) { g_physicalReject=1; return false; }
				if (b.read-0x20!=b.write-0x18) { g_physicalReject=2; return false; }
				if (At<std::uint32_t>(b.body,0x50)!=b.id || (At<std::uint32_t>(b.body,0x54)&7)!=2) { g_physicalReject=3; return false; }
				if (reinterpret_cast<std::uintptr_t>(VFn<void*>(b.write,0xC0))!=game::Base()+0x03082320) { g_physicalReject=4; return false; }
				if (reinterpret_cast<std::uintptr_t>(VFn<void*>(b.write,0xC8))!=game::Base()+0x03081D70) { g_physicalReject=5; return false; }
				motion=vtol::Decode(At<std::uint64_t>(b.motion,0x40),reinterpret_cast<const float*>(b.motion));
				const bool valid=vtol::Valid(motion); g_physicalReject=valid?0:6; return valid;
			} __except(EXCEPTION_EXECUTE_HANDLER) { g_physicalReject=7; return false; }
		}

		bool PhysicalOwnsSlot(const Shared& s,std::uintptr_t instance)
		{
			if (!s.physOn.load(std::memory_order_acquire) || !instance ||
				s.physInstance.load(std::memory_order_relaxed)!=instance) return false;
			if (s.physNeedsDriver.load(std::memory_order_relaxed) &&
				(!g_hoverVehicle.load() || g_hoverVehicle.load()!=g_driverVehicle.load())) return false;
			__try {
				const auto chassis=s.physChassis.load(std::memory_order_relaxed);
				const auto id=s.physBody.load(std::memory_order_relaxed);
				return chassis&&At<std::uint32_t>(instance,0x20)==id&&At<std::uint32_t>(chassis,0x50)==id&&(At<std::uint32_t>(chassis,0x54)&7)==2;
			}
			__except(EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// Any pilot's physical chassis (the native assist hooks see every vehicle).
		// The game's own wheel contacts for a vehicle instance (research 2026-11-05, decompile only): records at *(inst+0x90), 0xC0 bytes
		// each, count *(int8*)(*(inst+0x38)+0x30); +0x00 contact point, +0x10 contact normal, +0x24 contact body id (low 24 bits all set =
		// not touching). Written on the step threads: read only in the step hook, after the original ran. Guarded; nothing is written.
		struct WheelRead { int count = 0, touching = 0; double z = 0, up = 0; };
		bool ReadWheels(std::uintptr_t inst, WheelRead& out)
		{
			__try {
				if (!inst) return false;
				const std::uintptr_t arr = *reinterpret_cast<const std::uintptr_t*>(inst + 0x90);
				const std::uintptr_t cfg = *reinterpret_cast<const std::uintptr_t*>(inst + 0x38);
				if (!arr || !cfg) return false;
				const int n = *reinterpret_cast<const std::int8_t*>(cfg + 0x30);
				if (n < 1 || n > 16) return false;
				out.count = n;
				for (int i = 0; i < n; ++i) {
					const std::uintptr_t w = arr + std::uintptr_t(i) * 0xC0;
					const std::uint32_t body = *reinterpret_cast<const std::uint32_t*>(w + 0x24);
					if ((body & 0xFFFFFFu) == 0xFFFFFFu) continue;
					const float* cp = reinterpret_cast<const float*>(w + 0x00);
					const float* nrm = reinterpret_cast<const float*>(w + 0x10);
					if (!std::isfinite(cp[2]) || !std::isfinite(nrm[2])) continue;
					++out.touching; out.z += cp[2]; out.up += nrm[2];
				}
				if (out.touching) { out.z /= out.touching; out.up /= out.touching; }
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		double ProbeSpeed(std::uintptr_t motion)
		{
			__try { const float* v = reinterpret_cast<const float*>(motion + 0x60); return std::sqrt(double(v[0]) * v[0] + double(v[1]) * v[1]); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		Shared* RailSlotOf(std::uintptr_t instance);
		bool PhysicalOwns(std::uintptr_t instance)
		{
			if (!instance) return false;
			for (const auto& s : g_shs) if (PhysicalOwnsSlot(s,instance)) return true;
			return false;
		}
		void PhysicalRelease(Shared& s) { s.physInstance.store(0); }

		using Level_t=void(*)(std::uintptr_t,std::uintptr_t);
		using Damp_t=void(*)(std::uintptr_t,float);
		using Clamp_t=void(*)(std::uintptr_t,std::uintptr_t,float);
		Level_t g_physicalLevelOriginal=nullptr;
		Damp_t g_physicalDampOriginal=nullptr;
		Clamp_t g_physicalClampOriginal=nullptr;
		struct UprightLease {
			std::uintptr_t instance=0,action[2]={};
			std::uint32_t body=0,form=0;
			float strength[2]={},damping[2]={};
		};
		std::mutex g_uprightLock;
		std::unordered_map<std::uintptr_t,UprightLease> g_uprightLeases;
		bool CaptureUpright(std::uintptr_t component,UprightLease& lease)
		{
			__try {
				lease.instance=At<std::uintptr_t>(component,0x10);
				if (!PhysicalOwns(lease.instance)) return false;
				lease.body=At<std::uint32_t>(lease.instance,0x20); lease.form=At<std::uint32_t>(component,0x0C);
				for (int i=0;i<2;++i) {
					lease.action[i]=At<std::uintptr_t>(component,i?0x540:0x538);
					if (!lease.action[i]||At<std::uint32_t>(lease.action[i],0x20)!=lease.body) return false;
					lease.strength[i]=At<float>(lease.action[i],0x50); lease.damping[i]=At<float>(lease.action[i],0x54);
					if (!std::isfinite(lease.strength[i])||!std::isfinite(lease.damping[i])) return false;
				}
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) { g_physicalReject=7; return false; }
		}
		bool SetUpright(std::uintptr_t component,const UprightLease& lease,bool mute)
		{
			__try {
				if (At<std::uintptr_t>(component,0x10)!=lease.instance||At<std::uint32_t>(component,0x0C)!=lease.form||
					At<std::uint32_t>(lease.instance,0x20)!=lease.body) return false;
				for (int i=0;i<2;++i)
					if (At<std::uintptr_t>(component,i?0x540:0x538)!=lease.action[i]||At<std::uint32_t>(lease.action[i],0x20)!=lease.body) return false;
				for (int i=0;i<2;++i) {
					At<float>(lease.action[i],0x50)=mute?0.0f:lease.strength[i];
					At<float>(lease.action[i],0x54)=mute?0.0f:lease.damping[i];
				}
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void PhysicalLevelDetour(std::uintptr_t component,std::uintptr_t config)
		{
			UprightLease previous; bool restore=false;
			{ std::lock_guard lk(g_uprightLock); const auto it=g_uprightLeases.find(component);
				if (it!=g_uprightLeases.end()) { previous=it->second; g_uprightLeases.erase(it); restore=true; } }
			if (restore) SetUpright(component,previous,false);
			g_physicalLevelOriginal(component,config);
			UprightLease current;
			if (CaptureUpright(component,current)) {
				{ std::lock_guard lk(g_uprightLock); g_uprightLeases[component]=current; }
				SetUpright(component,current,true);
			}
		}
		Shared* RailSlotOf(std::uintptr_t instance)
		{
			if (!instance) return nullptr;
			for (auto& s : g_shs) if (s.railOn && s.instance.load(std::memory_order_relaxed) == instance && s.railMotion.load(std::memory_order_relaxed)) return &s;
			return nullptr;
		}
		void PhysicalDampDetour(std::uintptr_t instance,float seconds)
		{
			if (PhysicalOwns(instance)) return;
			// Probe: how much speed the game's own damping removes from a rail car each call.
			Shared* rs = RailSlotOf(instance);
			const std::uintptr_t m = rs ? rs->railMotion.load(std::memory_order_relaxed) : 0;
			const double before = m ? ProbeSpeed(m) : -1;
			g_physicalDampOriginal(instance,seconds);
			if (m && before >= 0) { const double after = ProbeSpeed(m); if (after >= 0) { rs->probeDampDrop.fetch_add((long long)std::llround((before - after) * 1000.0)); rs->probeDampCalls.fetch_add(1); } }
		}
		void PhysicalClampDetour(std::uintptr_t instance,std::uintptr_t world,float seconds)
		{
			if (PhysicalOwns(instance)) return;
			// Rail cars skip the game's speed clamp (2026-11-05 19:08 probes: it removed 3-4 m/s every step, pinning Cruise at ~14 m/s
			// while the rail asked for 22; the damping removed nothing). The same skip physical flight already has (PhysicalOwns above).
			// Counted as clamp calls with no drop in traffic.railprobe.
			if (Shared* railCar = RailSlotOf(instance)) { railCar->probeClampCalls.fetch_add(1); return; }
			// Probe: how much speed the game's own speed clamp removes from a rail car each call.
			Shared* rs = RailSlotOf(instance);
			const std::uintptr_t m = rs ? rs->railMotion.load(std::memory_order_relaxed) : 0;
			const double before = m ? ProbeSpeed(m) : -1;
			g_physicalClampOriginal(instance,world,seconds);
			if (m && before >= 0) { const double after = ProbeSpeed(m); if (after >= 0) { rs->probeClampDrop.fetch_add((long long)std::llround((before - after) * 1000.0)); rs->probeClampCalls.fetch_add(1); } }
		}

		void PreparePhysical(Shared& s,std::uintptr_t instance)
		{
			bool requested;
			{ std::lock_guard lk(s.lock); requested=s.active&&s.physical&&!s.physicalFault; }
			if (!requested) { PhysicalRelease(s); return; }
			const auto b=FindBody(instance,0,0,0,true);
			vtol::Motion motion;
			const bool ready=ReadPhysicalMotion(b,motion);
			std::lock_guard lk(s.lock);
			if (ready&&s.active&&s.physical&&bodybinding::Matches(b.id,s.bodyId.load())) {
				s.physChassis.store(b.body,std::memory_order_relaxed);
				s.physBody.store(b.id,std::memory_order_relaxed);
				s.physInstance.store(instance,std::memory_order_release);
			} else { PhysicalRelease(s); g_physicalFailures.fetch_add(1,std::memory_order_relaxed); }
		}

		bool ApplyPhysical(const Body& b,const vtol::Output& output)
		{
			alignas(16) const float linear[4]={float(output.linear.x),float(output.linear.y),float(output.linear.z),0};
			alignas(16) const float angular[4]={float(output.angular.x),float(output.angular.y),float(output.angular.z),0};
			__try {
				using Impulse=void(*)(std::uintptr_t,std::uint32_t,const float*,int);
				VFn<Impulse>(b.write,0xC0)(b.write,b.id,linear,0);
				VFn<Impulse>(b.write,0xC8)(b.write,b.id,angular,0);
				return true;
			} __except(EXCEPTION_EXECUTE_HANDLER) { g_physicalReject=9; return false; }
		}

		std::uint64_t StepPhysical(Shared& s,const Body& b,const float spin[4],double dt,std::uint64_t result,std::unique_lock<std::mutex>& lk)
		{
			if (s.physicalFault) return result;
			vtol::Motion motion;
			const bool player=&s==&g_shs[0]&&!s.autopilot;  // the global diagnostics stay the player's manual flight
			if (!PhysicalOwnsSlot(s,s.instance.load())||!ReadPhysicalMotion(b,motion)) {
				if (player) g_physicalFailures.fetch_add(1,std::memory_order_relaxed);
				return result;
			}
			auto vec=[](const float* p) { return vtol::Vec{p[0],p[1],p[2]}; };
			vtol::Body body{motion,vec(reinterpret_cast<const float*>(b.body)),vec(reinterpret_cast<const float*>(b.body+0x10)),
				vec(reinterpret_cast<const float*>(b.body+0x20)),vec(reinterpret_cast<const float*>(b.motion+0x60)),vec(spin)};
			const auto* R=reinterpret_cast<const float*>(b.body);
			const auto* F=reinterpret_cast<const float*>(b.body+0x10);
			const auto* U=reinterpret_cast<const float*>(b.body+0x20);
			const float z=At<float>(b.body,0x38);
			const double roll=hoverlogic::Roll(R[2],U[2]);
			const double pitch=hoverlogic::Pitch(F[0],F[1],F[2]);
			const double hSpeed=std::hypot(body.velocity.x,body.velocity.y);
			if (s.autopilot&&s.haveLast) {
				// The game's step brakes an unoccupied vehicle to rest (the "parked wipe" the stock model carries speed through).
				// The speed clamp is bypassed for this chassis; if the speed still vanishes with nothing hit, this vehicle goes
				// back to the stock model instead of fighting the game.
				const double ourH=std::hypot(double(s.lastV[0]),double(s.lastV[1]));
				const bool hit=GetTickCount64()-s.impactAt.load()<500;
				s.physWiped=ourH>3&&hSpeed<0.15*ourH&&!hit?s.physWiped+1:0;
				if (s.physWiped>=5) {
					s.physicalFault=true; PhysicalRelease(s);
					logger::Write("Physical VTOL AI: the game's step wiped the built speed %.1f -> %.1f m/s for 5 steps; this vehicle uses the standard model",ourH,hSpeed);
					return result;
				}
			}
			vtol::Vec rates;
			if (s.autopilot) {
				// AI pilots: the planner's yaw rate, a coordinated bank for it (or its roll aim), the nose level or aimed.
				vtolai::Input in{vec(R),vec(F),vec(U),pitch,roll,hSpeed,s.yawWant,s.hull.YawAt(hSpeed),s.hull.pitch,s.hull.roll,
					s.rollAimOn,s.rollAim,s.haveAim,s.pitchAim};
				rates=vtolai::Rates(in);
			} else if (s.assist&&!s.rollAimOn&&vtolassist::Applies(roll)) {
				// Flight assist: coordinated turns (vtolassist.h, 2026-10-24). A camera turn banks as far as a level turn needs;
				// the player's own roll wins and the nose follows its curve; near the ground and landing it levels.
				vtolassist::Input in;
				in.right=vec(R); in.forward=vec(F); in.up=vec(U); in.pitch=pitch; in.roll=roll; in.heading=std::atan2(double(F[0]),double(F[1]));
				in.speed=hSpeed; in.haveView=s.haveView; in.view=s.view; in.haveAim=s.haveAim; in.pitchAim=s.pitchAim; in.rollIn=s.rollIn;
				in.rollRate=freeroll::Rate(s.hull.roll); in.yawLimit=s.hull.YawAt(hSpeed); in.pitchLimit=s.hull.pitch;
				in.camGain=kCamPitchGain; in.levelRate=kLevelRate; in.nearGround=s.groundOk&&z-s.groundUnder<freeroll::kGroundLevel; in.landing=s.landing;
				rates=vtolassist::Solve(in).world;
			} else {
				const auto aim=s.haveView ? freeroll::AimDir(s.view,s.haveAim?s.pitchAim:0.0) : freeroll::Vec{F[0],F[1],F[2]};
				const auto error=freeroll::AimError(R,F,U,aim);
				const double pitchRate=std::clamp(kCamPitchGain*error.pitch,-s.hull.pitch,s.hull.pitch);
				const double yawRate=std::clamp(kCamPitchGain*error.yaw,-s.hull.YawAt(hSpeed),s.hull.YawAt(hSpeed));
				const bool nearGround=s.groundOk&&z-s.groundUnder<freeroll::kGroundLevel;
				const double rate=freeroll::Rate(s.hull.roll);
				const double rollRate=s.rollAimOn?freeroll::RollToward(roll,s.rollAim,kApRollGain,rate):
					freeroll::RollWant(roll,s.rollIn,rate,kLevelRate,0,s.assist,nearGround,s.landing);
				alignas(16) float composed[4]; freeroll::Compose(R,F,U,pitchRate,rollRate,yawRate,composed);
				rates=vec(composed);
			}
			double vertical;
			if (s.autopilot) vertical=s.vzOn?std::clamp(s.vz,-kLiftSpeed,kLiftSpeed):s.holding?std::clamp(kHoldGain*(s.refGround+s.hold-z),-kHoldMaxVz,kHoldMaxVz):0.0;
			else vertical=s.landing?-1.0:s.holding?std::clamp(kHoldGain*(s.refGround+s.hold-z),-kHoldMaxVz,kHoldMaxVz):s.lift*kLiftSpeed;
			if (s.groundOk&&!s.landing) {
				const double floor=fieldgate::FloorClearance(s.autopilot,s.fieldClearance,std::max(kMinClearance,std::fabs(R[2])*s.hull.width+std::fabs(F[2])*s.hull.length+std::fabs(U[2])*s.hull.height+1.5));
				if (z-s.groundUnder<floor) vertical=std::max(vertical,std::min(kLiftSpeed,(floor-(z-s.groundUnder))*3+1));
			}
			// AI pilots always fly assisted (the planners expect a stable chassis), with stronger slip thrusters and strafe.
			// FC-1: the side thrusters' bound and the rotation / drift gains are the flight gear row's (gear 3: 3 player / 6 AI, 5 /s,
			// 6 rad/s^2, 1.2 /s, as before); the speed cap already carries the gear's top (the flight tick's `top`).
			vtol::Command command{rates,s.thrustAcc*s.hull.acceleration,s.autopilot?0.0:s.lift,vertical,s.hull.brake,s.cap,
				std::max({s.hull.width,s.hull.length,s.hull.height}),s.autopilot||s.assist,s.braking,
				s.autopilot?s.strafe*kStrafeAcc:0.0,s.row.sideBudget};
			command.rateGain=s.row.rateGain; command.angularLimit=s.row.angularLimit; command.sideGain=s.row.sideGain;
			command.sideDead0=s.row.sideDead0; command.sideDeadPerSpeed=s.row.sideDeadPerSpeed;
			const auto output=vtol::Solve(body,command,s.physicalState,dt);
			if (!output.valid) { g_physicalReject=8; if (player) g_physicalFailures.fetch_add(1,std::memory_order_relaxed); return result; }
			s.trkPhysical=true; ++s.trkSteps; s.trkRateErr=std::max(s.trkRateErr,output.rateError); s.trkDrift=std::max(s.trkDrift,std::fabs(output.drift));
			s.trkSidePush=std::max(s.trkSidePush,std::fabs(output.sidePush)); s.trkAngCapped+=output.angularCapped; s.trkTorqueCapped+=output.torqueCapped;
			s.trkSideCapped+=output.sideCapped; s.trkCollective=output.collective; s.trkTrim=s.physicalState.trim; s.trkVzErr=vertical-body.velocity.z;
			lk.unlock();
			const bool applied=ApplyPhysical(b,output);
			alignas(16) float actualSpin[4]={};
			if (applied) VFn<void(*)(std::uintptr_t,std::uint32_t,float*)>(b.read,0xA8)(b.read,b.id,actualSpin);
			lk.lock();
			if (!applied) { s.physicalFault=true; PhysicalRelease(s); g_physicalFailures.fetch_add(1,std::memory_order_relaxed); return result; }
			if (!s.active||!s.physical||!bodybinding::Matches(b.id,s.bodyId.load())) return result;
			const auto* velocity=reinterpret_cast<const float*>(b.motion+0x60);
			for (int i=0;i<3;++i) { s.lastV[i]=s.v[i]=velocity[i]; s.lastW[i]=actualSpin[i]; s.measW[i]=actualSpin[i]; }
			s.haveLast=true; s.haveLastW=false; s.bodyPitch=float(pitch); s.bodyRoll=float(roll); s.matched=true; ++s.steps;
			++s.stepSerial; s.lastStepAt=GetTickCount64(); s.lastStepInstance=s.instance.load(); s.testSteps.fetch_add(1,std::memory_order_relaxed);
			s.physSteps.fetch_add(1,std::memory_order_relaxed);
			if (!player) return result;
			g_physicalLinearError.store(float(vtol::Length(vec(velocity)-(body.velocity+output.linear*motion.inverseMass))),std::memory_order_relaxed);
			g_physicalAngularError.store(float(vtol::Length(vec(actualSpin)-(body.spin+vtol::AngularDelta(motion,output.angular)))),std::memory_order_relaxed);
			g_physicalSteps.fetch_add(1,std::memory_order_relaxed);
			g_physicalCollective.store(float(output.collective),std::memory_order_relaxed);
			g_physicalMass.store(float(1/motion.inverseMass),std::memory_order_relaxed);
			return result;
		}

		// Before the game's step: a sharp change from what we wrote = the solver resolved a contact (hover ends).
		void CheckCollision(Shared& g_sh, std::uintptr_t vehicle)
		{
			float l[3], px, py, pz;
			{
				std::lock_guard lk(g_sh.lock);
				if (!g_sh.haveLast) {
					return;
				}
				l[0] = g_sh.lastV[0]; l[1] = g_sh.lastV[1]; l[2] = g_sh.lastV[2];
				px = g_sh.px; py = g_sh.py; pz = g_sh.pz;
			}
			const auto b = FindBody(vehicle, px, py, pz);
			if (!b.body || !bodybinding::Matches(b.id,g_sh.bodyId.load())) {
				return;
			}
			const float* v = reinterpret_cast<const float*>(b.motion + 0x60);
			bool ap;
			{
				std::lock_guard lk(g_sh.lock);
				ap = g_sh.autopilot;
			}
			(void)ap;  // the parked wipe happens inside the game's step, after this check: contacts count on autopilot too
			const float d = std::sqrt((v[0] - l[0]) * (v[0] - l[0]) + (v[1] - l[1]) * (v[1] - l[1]) + (v[2] - l[2]) * (v[2] - l[2]));
			const float speed = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
			if (d > std::max(kCollisionDelta, kCollisionShare * speed) && speed > kCollisionMinSpeed) {
				std::lock_guard lk(g_sh.lock);
				if(!bodybinding::Matches(b.id,g_sh.bodyId.load()))return;
				g_sh.collided = true;
                g_sh.impactAt.store(GetTickCount64());
				g_sh.collisionDelta = d;
				if (v[2] - l[2] > 0.6f * d) g_sh.collisionUp = true;  // mostly up: something under it
			}
		}

		// ---- The flight model, once per physics step ---------------------------------------------------------------
		std::uint64_t StepModel(Shared& g_sh, std::uintptr_t vehicle, std::uint64_t result, float seconds);

		std::uint32_t PhysicsBodyId(std::uintptr_t vehicle)
		{
			__try {
				return vehicle?At<std::uint32_t>(vehicle,0x20):0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// Diagnostics (2026-10-21: "0 steps, body NOT BOUND" with the body listed): the body ids the hook was called with lately,
		// and bodies more than one flight slot claimed (Owner() then gives nobody).
		std::atomic<std::uint32_t> g_seenIds[16]{};
		std::atomic<unsigned> g_seenNext{0};
		std::atomic<unsigned> g_ambiguous{0};
		std::atomic<std::uint32_t> g_ambiguousBody{0};
		void NoteSeen(std::uint32_t body)
		{
			for (const auto& x : g_seenIds) if (x.load(std::memory_order_relaxed) == body) return;
			g_seenIds[g_seenNext.fetch_add(1, std::memory_order_relaxed) % 16].store(body, std::memory_order_relaxed);
		}
		std::string StepDiag(const Shared& me)
		{
			char b[640];
			int n = std::snprintf(b, sizeof(b), "listed");
			for (std::uint32_t i = 0; i < me.sceneCount && i < 4; ++i) n += std::snprintf(b + n, sizeof(b) - n, " %08X", me.sceneBodies[i]);
			n += std::snprintf(b + n, sizeof(b) - n, "; hook saw");
			for (const auto& x : g_seenIds) if (const auto v = x.load()) n += std::snprintf(b + n, sizeof(b) - n, " %08X", v);
			n += std::snprintf(b + n, sizeof(b) - n, "; claimed twice %u (last %08X)", g_ambiguous.load(), g_ambiguousBody.load());
			for (int i = 0; i < kPilots; ++i) {
				const Shared& o = g_shs[i];
				if (&o == &me || !o.vehicle.load()) continue;
				for (std::uint32_t k = 0; k < o.sceneCount; ++k)
					for (std::uint32_t m = 0; m < me.sceneCount; ++m)
						if (o.sceneBodies[k] == me.sceneBodies[m]) n += std::snprintf(b + n, sizeof(b) - n, "; slot %d lists it too (active %d, ground %d)", i, o.active ? 1 : 0, o.groundOn ? 1 : 0);
			}
			return b;
		}

		// ---- Car model (2026-11-04, read-only; in release builds too since the user approved it for steering, 2026-11-04) ----
		// Offsets from Starfield.exe's own Havok reflection records (member name -> byte offset), cross-checked
		// against FUN_141324650 (steering PID writes deviceStatus+0x18 / +0x1C):
		//   hknpVehicleInstance: driverInput +0x40, steering +0x48, suspension +0x68, deviceStatus +0xE8,
		//                        rpm +0x10C, mainSteeringAngle +0x110, mainSteeringAngleAssumingNoReduction +0x114
		//   hknpVehicleDriverInputAnalogStatus: positionX +0x18 (steering), positionY +0x1C (accelerator)
		//   hknpVehicleDefaultAnalogDriverInput: slopeChangePointX +0x18, initialSlope +0x1C, deadZone +0x20
		//   hknpVehicleDefaultSteering: maxSteeringAngle +0x18, maxSpeedFullSteeringAngle +0x1C, doesWheelSteer +0x20 (hkArray<hkBool>)
		//   hknpVehicleSuspension: wheelParams +0x18 (hkArray, stride 0x30): hardpointChassisSpace +0x0, directionChassisSpace +0x10
		// Read after the game's own step, on the physics thread, under SEH; nothing is written.
		struct CarModel {
			float maxAngle=0,fullSpeed=0,slopePoint=0,initialSlope=0,deadZone=0,posX=0,main=0,noReduction=0;
			int wheels=0,steerCount=0;float hp[4][3]{},dir[4][3]{};unsigned char steers[4]{};
		};
		static bool Plausible(std::uintptr_t p){return p>0x10000&&p<0x7FFFFFFFFFFFull&&(p&7)==0;}
		static bool ReadCarModel(std::uintptr_t inst,CarModel& m)
		{
			__try {
				const auto steer=*reinterpret_cast<std::uintptr_t*>(inst+0x48),drive=*reinterpret_cast<std::uintptr_t*>(inst+0x40);
				const auto dev=*reinterpret_cast<std::uintptr_t*>(inst+0xE8),susp=*reinterpret_cast<std::uintptr_t*>(inst+0x68);
				if(!Plausible(steer)||!Plausible(drive)||!Plausible(dev)||!Plausible(susp))return false;
				m.maxAngle=*reinterpret_cast<float*>(steer+0x18);m.fullSpeed=*reinterpret_cast<float*>(steer+0x1C);
				const auto ws=*reinterpret_cast<std::uintptr_t*>(steer+0x20);m.steerCount=*reinterpret_cast<int*>(steer+0x28);
				m.slopePoint=*reinterpret_cast<float*>(drive+0x18);m.initialSlope=*reinterpret_cast<float*>(drive+0x1C);m.deadZone=*reinterpret_cast<float*>(drive+0x20);
				m.posX=*reinterpret_cast<float*>(dev+0x18);
				m.main=*reinterpret_cast<float*>(inst+0x110);m.noReduction=*reinterpret_cast<float*>(inst+0x114);
				const auto arr=*reinterpret_cast<std::uintptr_t*>(susp+0x18);m.wheels=*reinterpret_cast<int*>(susp+0x20);
				if(m.wheels<0||m.wheels>16)return false;
				if(Plausible(arr))for(int i=0;i<m.wheels&&i<4;++i)for(int k=0;k<3;++k){m.hp[i][k]=*reinterpret_cast<float*>(arr+i*0x30+k*4);m.dir[i][k]=*reinterpret_cast<float*>(arr+i*0x30+0x10+k*4);}
				if(ws&&m.steerCount>0&&m.steerCount<=16)for(int i=0;i<m.steerCount&&i<4;++i)m.steers[i]=*reinterpret_cast<unsigned char*>(ws+i);
				return std::isfinite(m.maxAngle)&&std::isfinite(m.main);
			} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void ObserveCarModel(Shared& s,std::uintptr_t inst)
		{
			static std::mutex lock;static std::unordered_set<std::uint32_t> logged;static ULONGLONG liveAt[kPilots]{};
			const auto ref=s.vehicle.load();if(!ref)return;
			const int idx=int(&s-g_shs);const auto now=GetTickCount64();
			const auto base=At<std::uintptr_t>(ref,0x98);const std::uint32_t baseId=base?At<std::uint32_t>(base,off::kFormId):0;
			bool once=false;{std::lock_guard lk(lock);once=logged.insert(baseId).second;if(!once&&now-liveAt[idx]<500)return;liveAt[idx]=now;}
			CarModel m;if(!ReadCarModel(inst,m)){if(once)logger::Write("OBS system=vehicle.model base=%08X event=unreadable instance=%p",baseId,reinterpret_cast<void*>(inst));return;}
			{double front=-1e9,rear=1e9;bool f=false,r=false;
			 for(int i=0;i<m.wheels&&i<4;++i){if(m.steers[i]){front=std::max(front,double(m.hp[i][1]));f=true;}else{rear=std::min(rear,double(m.hp[i][1]));r=true;}}
			 const double wheelbase=f&&r?front-rear:0;
			 {double across=0;for(int i=0;i<m.wheels&&i<4;++i)across=std::max(across,std::fabs(double(m.hp[i][0])));  // MS-1: the wheels set the least width
			  if(idx>=0&&idx<=prefetch::kSlotPilotCount&&across>.2&&across<3.0)g_wheelHalf[idx].store(float(across),std::memory_order_relaxed);}
			 if(m.maxAngle>.05f&&m.maxAngle<1.6f&&wheelbase>.5&&wheelbase<20){if(std::isfinite(m.posX))s.carInput=std::clamp(m.posX,-1.f,1.f);s.carLock=m.maxAngle;s.carWheelbase=float(wheelbase);s.carSlope=m.slopePoint;s.carInitial=m.initialSlope;s.carFull=m.fullSpeed;}}
			if(once){
				logger::Write("OBS system=vehicle.model base=%08X event=params max_steer_rad=%.4f full_steer_speed=%.3f slope_point=%.3f initial_slope=%.3f dead_zone=%.3f wheels=%d steer_flags=%d%d%d%d",
					baseId,m.maxAngle,m.fullSpeed,m.slopePoint,m.initialSlope,m.deadZone,m.wheels,m.steers[0],m.steers[1],m.steers[2],m.steers[3]);
				for(int i=0;i<m.wheels&&i<4;++i)logger::Write("OBS system=vehicle.model base=%08X event=wheel index=%d hardpoint=%.3f,%.3f,%.3f direction=%.3f,%.3f,%.3f steers=%d",
					baseId,i,m.hp[i][0],m.hp[i][1],m.hp[i][2],m.dir[i][0],m.dir[i][1],m.dir[i][2],m.steers[i]);
			}
			double speed=0;{std::lock_guard lk(s.lock);if(s.haveLast)speed=std::hypot(s.lastV[0],s.lastV[1]);}
			logger::Observe("vehicle.steer",At<std::uint32_t>(ref,off::kFormId),std::uint64_t(std::llround(m.posX*20))^(std::uint64_t(std::llround(m.main*200))<<16),
				"pilot=%d base=%08X input_x=%.3f wheel_angle=%.4f no_reduction=%.4f speed=%.2f autopilot=%d",idx,baseId,m.posX,m.main,m.noReduction,speed,int(s.autopilot));
		}
		// WO-1 (wheelgrip.h): the gear's turning weight. One record per vehicle data block (shared by a model's cars when the game shares it).
		std::mutex g_gripLock; std::unordered_map<std::uintptr_t, wheelgrip::Block> g_grip; ULONGLONG g_gripReadAt = 0;
		wheelgrip::Settings g_gripSet;  // turning weight / assist / slip target per gear
		void GripSettings(ULONGLONG now) {
			if (now - g_gripReadAt < 2000 && g_gripReadAt) return; g_gripReadAt = now;
			const auto ini = fvopaths::Settings();
			for (int gr = 1; gr <= 4; ++gr) { wchar_t key[12];
				swprintf(key, 12, L"assist%d", gr); g_gripSet.assist[gr] = int(GetPrivateProfileIntW(L"grip", key, wheelgrip::kDefaultAssistTenths[gr], ini.c_str()));
				swprintf(key, 12, L"slip%d", gr); g_gripSet.slip[gr] = int(GetPrivateProfileIntW(L"grip", key, wheelgrip::kDefaultSlipTenths[gr], ini.c_str()));
				swprintf(key, 12, L"gear%d", gr); g_gripSet.friction[gr] = int(GetPrivateProfileIntW(L"grip", key, wheelgrip::kDefaultPercent[gr], ini.c_str()));  // GT-1
				swprintf(key, 12, L"rear%d", gr); g_gripSet.rear[gr] = int(GetPrivateProfileIntW(L"grip", key, wheelgrip::kDefaultRearPercent[gr], ini.c_str()));
				swprintf(key, 12, L"weight%d", gr); g_gripSet.weight[gr] = int(GetPrivateProfileIntW(L"grip", key, wheelgrip::kDefaultWeightPercent[gr], ini.c_str())); }
		}
		int GearOfBody(std::uint32_t body);
		int SlotOfBody(std::uint32_t body);
		int GearOfSlot(int slot);
		// GK-1 guarded access (no C++ objects: SEH).
		bool EngineRead(std::uintptr_t vehicle, std::uintptr_t& engine, float* f4) {
			__try { engine = *reinterpret_cast<std::uintptr_t*>(vehicle + wheelgrip::kEngine); if (!engine) return false;
				f4[0] = *reinterpret_cast<float*>(engine + wheelgrip::kMinRpm); f4[1] = *reinterpret_cast<float*>(engine + wheelgrip::kOptRpm);
				f4[2] = *reinterpret_cast<float*>(engine + wheelgrip::kMaxRpm); f4[3] = *reinterpret_cast<float*>(engine + wheelgrip::kMaxTorque); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool EngineWriteTorque(std::uintptr_t engine, float torque) {
			__try { *reinterpret_cast<float*>(engine + wheelgrip::kMaxTorque) = torque; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool AxesAndVelocity(std::uintptr_t body, std::uintptr_t motion, float* f, float* u, float* v) {
			__try { const float* F = reinterpret_cast<const float*>(body + 0x10); const float* U = reinterpret_cast<const float*>(body + 0x20); const float* W = reinterpret_cast<const float*>(motion + 0x60);
				for (int i = 0; i < 3; ++i) { f[i] = F[i]; u[i] = U[i]; v[i] = W[i]; if (!std::isfinite(f[i]) || !std::isfinite(u[i]) || !std::isfinite(v[i])) return false; } return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool VelocityWrite(std::uintptr_t motion, const float* v) {
			__try { float* W = reinterpret_cast<float*>(motion + 0x60); W[0] = v[0]; W[1] = v[1]; W[2] = v[2]; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// Traction control (GK-1): the engine's max torque = the game's x a factor that falls while the wheels slip over the gear's target.
		struct Traction { float original = 0; double factor = 1.0; bool ok = false, checked = false; double logAt = -1e9; };
		std::mutex g_tcLock; std::unordered_map<std::uintptr_t, Traction> g_tc;
		std::atomic<float> g_wheelSlipAny[1 + prefetch::kSlotPilotCount];
		void ApplyTraction(std::uintptr_t vehicle, int slot, int gear, double dt, double nowS) {
			std::uintptr_t engine = 0; float e[4] = {}; if (!EngineRead(vehicle, engine, e)) return;
			std::lock_guard<std::mutex> lk(g_tcLock); auto& t = g_tc[engine];
			if (!t.checked) { t.checked = true; t.ok = wheelgrip::EnginePlausible(e[0], e[1], e[2], e[3]); t.original = e[3];
				logger::Write("OBS system=vehicle.grip event=engine instance=%p engine=%p min_rpm=%g opt_rpm=%g max_rpm=%g max_torque=%g plausible=%d reason=traction_control_reads_the_game_engine_once",
					reinterpret_cast<void*>(vehicle), reinterpret_cast<void*>(engine), e[0], e[1], e[2], e[3], int(t.ok)); }
			if (!t.ok) return;
			const auto prof = wheelgrip::ProfileOf(gear > 0 ? gear : 3, g_gripSet);
			if (gear <= 0 || slot < 0 || prof.slipTarget <= 0) { if (t.factor < 1.0) { t.factor = 1.0; EngineWriteTorque(engine, t.original); } return; }  // RB-1: slip 0 = off
			const double slip = g_wheelSlipAny[slot].load(std::memory_order_relaxed);
			const double before = t.factor; t.factor = wheelgrip::Traction(t.factor, slip, prof.slipTarget, dt);
			if (std::fabs(t.factor - before) > 1e-4) EngineWriteTorque(engine, float(t.original * t.factor));
			if (t.factor < .99 && (nowS - t.logAt >= 1.0 || nowS < t.logAt)) { t.logAt = nowS;
				logger::Write("OBS system=vehicle.grip event=traction pilot=%d gear=%d wheel_slip=%.2f slip_target=%.2f torque_factor=%.2f max_torque=%.1f reason=wheels_spin_engine_eased",
					slot, gear, slip, prof.slipTarget, t.factor, t.original * t.factor); }
		}
		// Sideways grip assist (GK-1): after the game's wheel step, the sideways slide over the cap is taken back within the gear's budget.
		bool RailReadSpin(const Body& b, float* spin); bool RailSetSpin(const Body& b, const float* spin);  // GK-2 (defined with the rail)
		// RB-1: the bicycle model's rear slip angle per FVO car on its wheels, every physics step (the back stepping out), and how often its
		// rotation flips (the oscillation): auto gear reads the angle; vehicle.grip event=rear traces both each second.
		std::atomic<float> g_rearSlipDeg[1 + prefetch::kSlotPilotCount];
		std::atomic<std::uintptr_t> g_yawInstance[1 + prefetch::kSlotPilotCount]{};
        std::atomic<std::uint64_t> g_yawAt[1 + prefetch::kSlotPilotCount]{};
        std::atomic<float> g_yawMeasured[1 + prefetch::kSlotPilotCount];  // TK-1: the body's turn rate about up (rad/s), each physics step
		// TK-1 (tracking.h): the tracking cap per car, set by the Drive stage, read by the Speed stage next tick (+inf: tracking fine).
		std::atomic<double> g_trackCap[1 + prefetch::kSlotPilotCount]; std::atomic<const char*> g_trackWhy[1 + prefetch::kSlotPilotCount];
		tracking::State g_track[1 + prefetch::kSlotPilotCount];
		struct RearLog { double logAt = -1e9, worst = 0, lastYaw = 0; int flips = 0, steps = 0; };
		std::mutex g_rearLock; RearLog g_rearLog[1 + prefetch::kSlotPilotCount];
		void MeasureRear(std::uintptr_t vehicle, int slot, int gear, double nowS) {
			if (slot < 0 || slot > prefetch::kSlotPilotCount) return;
			const auto bb = FindBody(vehicle, 0, 0, 0, true); if (!bb.body || !bb.motion) return;
			float f[3], u[3], v[3]; if (!AxesAndVelocity(bb.body, bb.motion, f, u, v)) return;
			alignas(16) float spin[4] = {}; if (!RailReadSpin(bb, spin)) return;
			const wheelgrip::V n = wheelgrip::Unit({ u[0], u[1], u[2] }); if (n.z < .5) return;
			const wheelgrip::V fv{ f[0], f[1], f[2] }; const wheelgrip::V fp = wheelgrip::Unit(wheelgrip::Sub(fv, wheelgrip::Sc(n, wheelgrip::Dot(fv, n))));
			const wheelgrip::V side = wheelgrip::Unit(wheelgrip::Cross(n, fp)), vel{ v[0], v[1], v[2] };
			const double yaw = spin[0] * n.x + spin[1] * n.y + spin[2] * n.z;
			const double wb = double(g_shs[slot].carWheelbase.load()); const double rearArm = (wb > .5 ? wb : 3.2) * .5;
			const double deg = wheelgrip::RearSlipDeg(wheelgrip::Dot(vel, fp), wheelgrip::Dot(vel, side), yaw, rearArm);
			g_rearSlipDeg[slot].store(float(deg), std::memory_order_relaxed); g_yawMeasured[slot].store(float(yaw), std::memory_order_relaxed);
            g_yawInstance[slot].store(vehicle, std::memory_order_relaxed);
            g_yawAt[slot].store(static_cast<std::uint64_t>(std::llround(nowS * 1000.0)), std::memory_order_release);
			std::lock_guard<std::mutex> lk(g_rearLock); auto& L = g_rearLog[slot];
			L.worst = std::max(L.worst, deg); ++L.steps;
			if (std::fabs(yaw) > .3 && std::fabs(L.lastYaw) > .3 && (yaw > 0) != (L.lastYaw > 0)) ++L.flips;
			if (std::fabs(yaw) > .3) L.lastYaw = yaw;
			if (nowS - L.logAt >= 1.0 || nowS < L.logAt) {
				if (L.worst >= 2.0 || L.flips > 0) logger::Write("OBS system=vehicle.grip event=rear pilot=%d gear=%d rear_slip_deg_max=%.1f rotation_flips_per_s=%d speed=%.1f steps=%d reason=bicycle_rear_slip_angle_and_oscillation",
					slot, gear, L.worst, L.flips, wheelgrip::Dot(vel, fp), L.steps);
				const double keepYaw = L.lastYaw; L = RearLog{}; L.logAt = nowS; L.lastYaw = keepYaw; }
		}
		// CF-1 (cornerforce.h): four corner forces after the game's wheel step hold the turn rate to the front wheels' angle and the rear axle
		// from sliding, within the gear's budget (settings.ini [grip] assistN, tenths of m/s^2: gear 1 strongest, gear 3 negligible). Every
		// FVO car on its wheels, manual and autopilot alike. REPLACES GK-1 (sideways cap) and GK-2 (fishtail turn-rate cap).
		cornerforce::State g_corner[1 + prefetch::kSlotPilotCount];
		struct CornerLog { double logAt = -1e9, worstErr = 0, worstSlide = 0, sumAy = 0, sumYaw = 0; int steps = 0, active = 0, capped = 0; const char* why = ""; };
		std::mutex g_cornerLock; CornerLog g_cornerLog[1 + prefetch::kSlotPilotCount];
		bool WheelAngle(std::uintptr_t inst, float& a) {  // the front wheels' steering angle (rad): instance +0x110 (ReadCarModel's `main`)
			__try { a = *reinterpret_cast<float*>(inst + 0x110); return std::isfinite(a) && std::fabs(a) < 1.6f; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		void ApplyCorners(std::uintptr_t vehicle, int slot, int gear, double dt, double nowS) {
			if (gear <= 0 || slot < 0 || slot > prefetch::kSlotPilotCount || dt <= 0) return;
			const auto prof = wheelgrip::ProfileOf(gear, g_gripSet);
			bool onWheels = true; { WheelRead wr; if (!ReadWheels(vehicle, wr) || wr.touching < 3) onWheels = false; }
			const auto bb = FindBody(vehicle, 0, 0, 0, true); if (!bb.body || !bb.motion) return;
			float f[3], u[3], v[3]; if (!AxesAndVelocity(bb.body, bb.motion, f, u, v)) return;
			alignas(16) float spin[4] = {}; if (!RailReadSpin(bb, spin)) return;
			const wheelgrip::V n = wheelgrip::Unit({ u[0], u[1], u[2] }); if (n.z < .75) onWheels = false;
			const wheelgrip::V fv{ f[0], f[1], f[2] }; const wheelgrip::V fp = wheelgrip::Unit(wheelgrip::Sub(fv, wheelgrip::Sc(n, wheelgrip::Dot(fv, n))));
			const wheelgrip::V side = wheelgrip::Unit(wheelgrip::Cross(n, fp)), vel{ v[0], v[1], v[2] };  // side: left of the nose
			float delta = 0; WheelAngle(vehicle, delta);
			cornerforce::Car car; { const double wb = double(g_shs[slot].carWheelbase.load()); if (wb > .5) car.wheelbase = wb; }
			cornerforce::Input in; in.dt = dt; in.speed = wheelgrip::Dot(vel, fp); in.vSide = wheelgrip::Dot(vel, side);
			in.yawRate = spin[0] * n.x + spin[1] * n.y + spin[2] * n.z; in.wheelAngle = delta; in.budget = prof.assist; in.grip = prof.friction; in.onWheels = onWheels;
			const auto o = cornerforce::Update(g_corner[slot], in, car);
			if (o.applied) {
				const float nv[3] = { float(v[0] + side.x * o.ay * dt), float(v[1] + side.y * o.ay * dt), float(v[2] + side.z * o.ay * dt) };
				if (!VelocityWrite(bb.motion, nv)) return;
				const double dw = o.yawAccel * dt; alignas(16) float ns[4] = { float(spin[0] + dw * n.x), float(spin[1] + dw * n.y), float(spin[2] + dw * n.z), 0.f };
				RailSetSpin(bb, ns);
			}
			std::lock_guard<std::mutex> lk(g_cornerLock); auto& L = g_cornerLog[slot];
			L.worstErr = std::max(L.worstErr, std::fabs(o.yawErr)); L.worstSlide = std::max(L.worstSlide, std::fabs(o.rearSlide));
			L.sumAy += std::fabs(o.ay) * dt; L.sumYaw += std::fabs(o.yawAccel) * dt; ++L.steps; if (o.applied) { ++L.active; L.why = o.why; } if (o.capped) ++L.capped;
			if (nowS - L.logAt >= 1.0 || nowS < L.logAt) {
				if (L.active > 0) logger::Write("OBS system=vehicle.corners pilot=%d gear=%d budget=%.1f steer_sign=%.0f worst_turn_err=%.2f worst_rear_slide=%.2f sideways_dv=%.2f turn_dw=%.2f active_steps=%d of %d capped=%d why=%s reason=four_corner_forces_matrix",
					slot, gear, prof.assist, g_corner[slot].sign, L.worstErr, L.worstSlide, L.sumAy, L.sumYaw, L.active, L.steps, L.capped, L.why);
				L = CornerLog{}; L.logAt = nowS; }
		}
		// AG-1: the game's own wheel slip (hknpVehicleInstance +0x90 WheelInfo array, count +0x98, 0xC0 a wheel, +0xB4 forward slip
		// velocity; FUN_1413206F0). The largest |slip| of the wheels after each native step, per pilot slot (any thread reads).
		std::atomic<float> g_wheelSlip[1 + prefetch::kSlotPilotCount];
		bool WheelSlip(std::uintptr_t vehicle, float& maxSlip) {
			__try { const auto arr = *reinterpret_cast<std::uintptr_t*>(vehicle + 0x90); const int n = *reinterpret_cast<int*>(vehicle + 0x98);
				if (!arr || n < 1 || n > 8) return false; float m = 0;
				for (int i = 0; i < n; ++i) { const float v = *reinterpret_cast<float*>(arr + i * 0xC0 + 0xB4); if (!std::isfinite(v) || std::fabs(v) > 500) return false; m = std::max(m, std::fabs(v)); }
				maxSlip = m; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// Guarded memory access (no C++ objects here: SEH).
		bool DataOf(std::uintptr_t vehicle, std::uintptr_t& data) {
			__try { data = *reinterpret_cast<std::uintptr_t*>(vehicle + wheelgrip::kData); return data != 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// GT-1 tyres (data +0x30 wheel count, +0xA0 wheel params; FUN_1423C3270 writes axle = index > 1: wheels 0-1 front, 2+ rear).
		bool TyresRead(std::uintptr_t data, wheelgrip::Tyres& t) {
			__try { const int n = *reinterpret_cast<signed char*>(data + wheelgrip::kCount); const auto arr = *reinterpret_cast<std::uintptr_t*>(data + wheelgrip::kWheels);
				if (!arr || n < 1 || n > wheelgrip::kMaxWheelCount) return false; t.wheels = n;
				for (int i = 0; i < n; ++i) { t.friction[i] = *reinterpret_cast<float*>(arr + i * wheelgrip::kStride + wheelgrip::kFriction); t.maxFriction[i] = *reinterpret_cast<float*>(arr + i * wheelgrip::kStride + wheelgrip::kMaxFriction); }
				return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool TyresWrite(std::uintptr_t data, const wheelgrip::Tyres& t, double front, double rear) {
			__try { const auto arr = *reinterpret_cast<std::uintptr_t*>(data + wheelgrip::kWheels); if (!arr) return false;
				for (int i = 0; i < t.wheels; ++i) { const double m = i >= 2 ? rear : front;
					*reinterpret_cast<float*>(arr + i * wheelgrip::kStride + wheelgrip::kFriction) = float(t.friction[i] * m);
					*reinterpret_cast<float*>(arr + i * wheelgrip::kStride + wheelgrip::kMaxFriction) = float(t.maxFriction[i] * m); }
				return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// YW-1 guarded access to the baked chassis friction inertia (wheelgrip.h kChassisInvInertia; decomp FUN_1430A7670 / FUN_1430AA430).
		bool ChassisRead(std::uintptr_t data, wheelgrip::Chassis& c) {
			__try { for (int i = 0; i < 4; ++i) c.inv[i] = *reinterpret_cast<float*>(data + wheelgrip::kChassisInvInertia + i * 4);
				for (int k = 0; k < 3; ++k) for (int i = 0; i < 4; ++i) c.rows[k][i] = *reinterpret_cast<float*>(data + wheelgrip::kOrientRows + k * 0x10 + i * 4);
				c.torque[0] = *reinterpret_cast<float*>(data + wheelgrip::kTorqueFactors + 8); c.torque[1] = *reinterpret_cast<float*>(data + wheelgrip::kTorqueFactors);
				c.torque[2] = *reinterpret_cast<float*>(data + wheelgrip::kTorqueFactors + 4);
				for (int k = 0; k < 3; ++k) c.inertia[k] = *reinterpret_cast<float*>(data + wheelgrip::kUnitInertias + k * 4);
				return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool ChassisWrite(std::uintptr_t data, const float* inv3) {
			__try { for (int i = 0; i < 3; ++i) *reinterpret_cast<float*>(data + wheelgrip::kChassisInvInertia + i * 4) = inv3[i]; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// GT-1, before the native step: this car gets its gear's tyre grip (front, rear) and turning weight (per car; a shared block: the
		// player's car owns it, else the highest ask; GO-1). Every write is guarded; numbers that are not plausible are never touched. Traced on a new block and a change.
		void ApplyGrip(std::uintptr_t vehicle, std::uint32_t body, double nowS) {
			std::uintptr_t data = 0; if (!DataOf(vehicle, data)) return;
			const int pilot = SlotOfBody(body); const int gear = GearOfSlot(pilot); const auto prof = wheelgrip::ProfileOf(gear > 0 ? gear : 3, g_gripSet);
			wheelgrip::Ask ask; if (gear > 0) { ask.front = prof.friction; ask.rear = prof.friction * prof.rear; ask.weight = prof.weight; }
			std::lock_guard<std::mutex> lk(g_gripLock);
			auto& blk = g_grip[data];
			{   // GO-1: a block address reused by another car (the old one unloaded) is read again, never given the old car's game values.
				static std::unordered_map<std::uintptr_t, std::uintptr_t> lastInstance;
				auto& last = lastInstance[data];
				if (last && last != vehicle && wheelgrip::FreshUsers(blk, nowS) == 0) {
					logger::Write("OBS system=vehicle.grip event=block_reused data=%p old_instance=%p instance=%p reason=read_again", reinterpret_cast<void*>(data), reinterpret_cast<void*>(last), reinterpret_cast<void*>(vehicle));
					blk = {};
				}
				last = vehicle;
			}
			if (!blk.read) {
				blk.read = true;
				if (ChassisRead(data, blk.chassis)) { float probe[4]; blk.chassis.ok = wheelgrip::YawWeighted(blk.chassis, 1.0, probe) && blk.chassis.inv[3] > 0; }
				bool tyresOk = TyresRead(data, blk.tyres);
				for (int i = 0; tyresOk && i < blk.tyres.wheels; ++i) if (!std::isfinite(blk.tyres.friction[i]) || blk.tyres.friction[i] <= 0 || blk.tyres.friction[i] > 100 || !std::isfinite(blk.tyres.maxFriction[i]) || blk.tyres.maxFriction[i] < 0 || blk.tyres.maxFriction[i] > 1000) tyresOk = false;
				if (!tyresOk) blk.tyres.wheels = 0;
				const auto& c = blk.chassis;
				logger::Write("OBS system=vehicle.grip event=block data=%p instance=%p body=%08X inv_inertia=%g,%g,%g inv_mass=%g torque_yaw=%g inertia_yaw=%g chassis_ok=%d wheels=%d friction=%.3f max_friction=%.3f tyres_ok=%d reason=game_handling_read_once",
					reinterpret_cast<void*>(data), reinterpret_cast<void*>(vehicle), body, c.inv[0], c.inv[1], c.inv[2], c.inv[3], c.torque[0], c.inertia[0], int(c.ok),
					blk.tyres.wheels, blk.tyres.friction[0], blk.tyres.maxFriction[0], int(tyresOk));
			}
			const int before = wheelgrip::FreshUsers(blk, nowS);
			const auto dec = wheelgrip::Want(blk, vehicle, pilot, ask, nowS); auto want = dec.want;
			if (!blk.chassis.ok) want.weight = 1.0;
			if (!blk.tyres.wheels) { want.front = 1.0; want.rear = 1.0; }
			const int users = wheelgrip::FreshUsers(blk, nowS);
			const bool tyresChanged = std::fabs(want.front - blk.appliedFront) >= 1e-6 || std::fabs(want.rear - blk.appliedRear) >= 1e-6;
			const bool weightChanged = std::fabs(want.weight - blk.appliedWeight) >= 1e-6;
			if (!tyresChanged && !weightChanged && users == before && dec.owner == blk.appliedOwner) return;
			if (tyresChanged && !TyresWrite(data, blk.tyres, want.front, want.rear)) { blk.tyres.wheels = 0;
				logger::Write("OBS system=vehicle.grip event=write_failed data=%p part=tyres reason=not_writable_never_touched_again", reinterpret_cast<void*>(data)); return; }
			float inv[4] = { blk.chassis.inv[0], blk.chassis.inv[1], blk.chassis.inv[2], blk.chassis.inv[3] };
			if (blk.chassis.ok && !wheelgrip::YawWeighted(blk.chassis, weightChanged ? want.weight : blk.appliedWeight, inv)) return;
			if (weightChanged && !ChassisWrite(data, inv)) { blk.chassis.ok = false;
				logger::Write("OBS system=vehicle.grip event=write_failed data=%p part=chassis reason=not_writable_never_touched_again", reinterpret_cast<void*>(data)); return; }
			logger::Write("OBS system=vehicle.grip event=applied data=%p instance=%p gear=%d front=%.2f rear=%.2f turning_weight=%.2f was=%.2f/%.2f/%.2f cars_sharing_block=%d owner=%d rule=%s tyres_per_instance=%d weight_per_instance=%d held=%d ever_shared=%d friction_front=%.3f friction_rear=%.3f inv_inertia=%g,%g,%g reason=game_handling_times_gear_one_owner",
				reinterpret_cast<void*>(data), reinterpret_cast<void*>(vehicle), gear, want.front, want.rear, want.weight, blk.appliedFront, blk.appliedRear, blk.appliedWeight, users,
				dec.owner, wheelgrip::RuleName(dec.rule), int(dec.perInstance), int(dec.perInstance), int(dec.held), int(blk.everShared),
				blk.tyres.friction[0] * want.front, blk.tyres.friction[std::min(2, std::max(0, blk.tyres.wheels - 1))] * want.rear, inv[0], inv[1], inv[2]);
			blk.appliedFront = want.front; blk.appliedRear = want.rear; blk.appliedWeight = want.weight; blk.appliedOwner = dec.owner;
		}
        // Applies a rail target inside the same native chassis callback for both traffic and the test.
        // POD frame: SEH protects the body handles, which can disappear when cells change.
        const char* WriteRailPose(std::uintptr_t vehicle, std::uint32_t bodyId,
                                  const FvoFleetRailPoint& position, const FvoFleetRailPoint& velocity,
                                  double tangentX = 0.0, double tangentY = 0.0, double feedYawRate = 0.0)
        {
            __try {
                const Body body = FindBody(vehicle, 0, 0, 0, true);
                if (!body.body || !body.motion || body.id != bodyId ||
                    At<std::uint32_t>(body.body, 0x50) != body.id) return "body_changed";
                if (At<std::uint32_t>(body.motion, 0x48) != body.id) return "motion_owner_changed";
                auto* centre = reinterpret_cast<double*>(body.motion + 0x10);
                auto* pose = reinterpret_cast<float*>(body.body + 0x30);
                auto* speed = reinterpret_cast<float*>(body.motion + 0x60);
                if (!std::isfinite(centre[0]) || !std::isfinite(centre[1]) || !std::isfinite(centre[2]) ||
                    !std::isfinite(pose[0]) || !std::isfinite(pose[1]) || !std::isfinite(pose[2])) return "non_finite_pose";
                if (!(std::hypot(centre[0] - pose[0], centre[1] - pose[1], centre[2] - pose[2]) <= 20.0))
                    return "motion_body_separated";
                const double dx = position.x - pose[0], dy = position.y - pose[1], dz = position.z - pose[2];
                if (!(std::hypot(dx, dy, dz) <= 25.0)) return "step_over_25m";
                centre[0] += dx; centre[1] += dy; centre[2] += dz;
                pose[0] += float(dx); pose[1] += float(dy); pose[2] += float(dz);
                speed[0] = float(velocity.x); speed[1] = float(velocity.y); speed[2] = float(velocity.z);
                // Align the Havok rigid body upright (including inverted-car righting when U[2] < 0.2)
                // and along the energy-optimal velocity/curvature heading with feedforward yaw rate.
                alignas(16) float spin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                const float* F = reinterpret_cast<const float*>(body.body + 0x10);
                const float* U = reinterpret_cast<const float*>(body.body + 0x20);
                if (std::isfinite(U[0]) && std::isfinite(U[1]) && std::isfinite(U[2])) {
                    if (U[2] < 0.2f && std::isfinite(F[0]) && std::isfinite(F[1])) {
                        // Inverted or on its side: roll around local forward axis so U[0]=U[1]=0 never deadlocks upside-down.
                        const float fLen = std::hypot(F[0], F[1]);
                        if (fLen > 0.1f) {
                            spin[0] = (F[0] / fLen) * 2.5f;
                            spin[1] = (F[1] / fLen) * 2.5f;
                        } else {
                            spin[0] = 2.5f;
                        }
                    } else {
                        spin[0] = std::clamp(U[1] * 4.0f, -2.0f, 2.0f);
                        spin[1] = std::clamp(-U[0] * 4.0f, -2.0f, 2.0f);
                    }
                }
                const double tLen = std::hypot(tangentX, tangentY);
                if (tLen > 0.05 && std::isfinite(F[0]) && std::isfinite(F[1])) {
                    const double tx = tangentX / tLen, ty = tangentY / tLen;
                    const double yawErr = std::atan2(double(F[0]) * ty - double(F[1]) * tx,
                                                     double(F[0]) * tx + double(F[1]) * ty);
                    if (std::isfinite(yawErr)) spin[2] = float(std::clamp(yawErr * 5.0 + feedYawRate, -3.5, 3.5));
                }
                RailSetSpin(body, spin);
                return nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return "fault_while_writing"; }
        }

        // AC-2: the traffic manager's advice per fleet car (which actuator the autopilot should drive it through).
        enum class Actuator : int { Physical = 0, Virtual = 1, Waiting = 2 };
        inline const char* ActuatorName(Actuator a) { return a == Actuator::Physical ? "physical" : a == Actuator::Virtual ? "virtual" : "waiting"; }
        struct FleetRail {
            unsigned driver = 0, body = 0;
            std::uintptr_t instance = 0;
            fleetrail::Path path;
            fleetrail::Mode mode = fleetrail::Mode::Awaiting;
            bool railOwner = false, seeded = false;
            double station = 0.0, speed = 0.0;
            FvoFleetRailPoint position{};
            std::uint64_t lastStep = 0;
            const char* fault = "";
            // Soft switching (fleetrail::rules): the rail's own speed, the entry blend and when the mode last changed.
            double railSpeed = 0.0, blendT = 0.0;
            bool blendReady = false;
            FvoFleetRailPoint offset{};
            std::uint64_t modeSince = 0;
            // Reference mode: last reference move and how many were queued.
            std::uint64_t refStepAt = 0;
            unsigned refSteps = 0;
            Actuator advice = Actuator::Waiting;  // AC-2: the manager's advice
            std::uint64_t adviceAt = 0;
            double toPlayer = 0.0;
            double hazardOffset = 0.0;  // routehazard.h: how far the carried car stands to the right of its path to go round a hazard box
        };
        std::mutex g_fleetRailLock;
        std::unordered_map<unsigned, FleetRail> g_fleetRails;
        std::atomic<unsigned> g_fleetRailCount{0};

        // Exit ramp: after the rail hands a car to autopilot, autopilot may only speed up gradually from the rail's speed.
        struct FleetHandoff { std::uint64_t at = 0; double speed = 0.0; };
        std::mutex g_fleetHandoffLock;
        std::unordered_map<unsigned, FleetHandoff> g_fleetHandoffs;
        void NoteFleetHandoff(unsigned vehicle, double speed, std::uint64_t now)
        {
            std::lock_guard lk(g_fleetHandoffLock);
            g_fleetHandoffs[vehicle] = {now, std::isfinite(speed) ? std::max(0.0, speed) : 0.0};
        }
        double FleetHandoffCap(unsigned vehicle, std::uint64_t now)
        {
            std::lock_guard lk(g_fleetHandoffLock);
            if (g_fleetHandoffs.empty()) return std::numeric_limits<double>::infinity();
            const auto it = g_fleetHandoffs.find(vehicle);
            if (it == g_fleetHandoffs.end()) return std::numeric_limits<double>::infinity();
            const double cap = fleetrail::RampCap(it->second.speed, now >= it->second.at ? now - it->second.at : 0);
            if (!std::isfinite(cap)) g_fleetHandoffs.erase(it);
            return cap;
        }
        // Reads the body the rail is about to drive: where it is and how fast it moves (for the entry blend).
        bool ReadRailBody(std::uintptr_t vehicle, std::uint32_t bodyId, FvoFleetRailPoint& pose, FvoFleetRailPoint& velocity)
        {
            __try {
                const Body body = FindBody(vehicle, 0, 0, 0, true);
                if (!body.body || !body.motion || body.id != bodyId || At<std::uint32_t>(body.body, 0x50) != body.id ||
                    At<std::uint32_t>(body.motion, 0x48) != body.id) return false;
                const auto* p = reinterpret_cast<const float*>(body.body + 0x30);
                const auto* v = reinterpret_cast<const float*>(body.motion + 0x60);
                pose = {p[0], p[1], p[2]};
                velocity = {v[0], v[1], v[2]};
                return std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(pose.z) &&
                    std::isfinite(velocity.x) && std::isfinite(velocity.y) && std::isfinite(velocity.z);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        }

        // Distance-gated FleetRail (2026-11-23): beyond the player's loaded Havok physics radius (~220 m, where road colliders
        // are unloaded), FleetRailStep glides live Full-LOD bodies kinematically along their authored lane spline inside StepDetour
        // (with corner speed, leader headway, smooth entry blend, and 1.5 s exit ramp), freeing PilotsTick CPU for near cars.
        // Inside (farRadius - 20 m), cars ALWAYS run full dynamic energyplan driving (never gliding beside the player as in AC-1).
        constexpr bool kFleetRailDrives = true;
        std::unordered_map<unsigned, ULONGLONG> g_virtualAt;  // vehicle -> when the autopilot last moved it virtually (under g_fleetRailLock)
        // The game's physics radius (havok cell extents x 50 m, game::ReadLimits), read every 5 s; 0 = unknown.
        double PhysicsRadius() {
            static double radius = 0; static ULONGLONG at = 0; const auto now = GetTickCount64();
            if (!at || now - at >= 5000) { at = now; const double r = double(game::ReadLimits().physicsMetres); radius = std::isfinite(r) && r > 50.0 ? r : 0.0; }
            return radius;
        }
        unsigned FleetDriverOf(unsigned vehicle) {
            std::lock_guard lk(g_fleetRailLock); const auto it = g_fleetRails.find(vehicle); return it == g_fleetRails.end() ? 0u : it->second.driver;
        }
        void NoteVirtual(unsigned vehicle, ULONGLONG now) { std::lock_guard lk(g_fleetRailLock); g_virtualAt[vehicle] = now; }
        // AC-2: the manager's advice for this car (false: not a fleet car, or no fresh advice).
        bool AdviceOf(unsigned vehicle, Actuator& out, double& toPlayer) {
            std::lock_guard lk(g_fleetRailLock); const auto it = g_fleetRails.find(vehicle);
            if (it == g_fleetRails.end() || !it->second.adviceAt || GetTickCount64() - it->second.adviceAt > 1000) return false;
            out = it->second.advice; toPlayer = it->second.toPlayer; return true;
        }
        // AC-2 virtual actuator: the fleet rail's path carries the car at the autopilot's wanted speed (capped by the path's corner speed);
        // returns the next pose (the path gives the height and heading). False: no rail path for this car.
        bool RailVirtualStep(unsigned vehicle, double x, double y, double z, double want, double dt, double& ox, double& oy, double& oz, double& yaw, double& used,
                             double halfWidth = 1.4, double halfLength = 3.0, double height = 2.5) {
            std::lock_guard lk(g_fleetRailLock); const auto it = g_fleetRails.find(vehicle);
            if (it == g_fleetRails.end() || !(it->second.path.End() > 1.0)) return false;
            auto& car = it->second;
            // HJ-2 (2026-11-22 18:54 run: FF10A329 jumped 140 m when it went virtual again): the station kept from an earlier virtual
            // stretch is not where the car is now. Re-seed from the car's real position whenever they differ by more than 3 m.
            if (car.seeded) { const auto onPath = car.path.At(car.station); const double off = std::hypot(onPath.x - x, onPath.y - y);
                if (!(off <= 3.0)) { car.seeded = false; logger::Write("OBS system=traffic.actuator vehicle=%08X event=virtual_reseed stale_m=%.1f reason=station_from_an_earlier_virtual_stretch", vehicle, off); } }
            if (!car.seeded) { car.station = car.path.Project(x, y, z); car.seeded = true; car.railSpeed = 0; }
            // Hazard boxes (routehazard.h): the same boxes the physical planner sees. A valid shift round them if one exists, else stop short.
            routehazard::Plan hazards; { routehazard::Box found[16]; const int n = hazardfield::NearNow(x, y, z - 20.0, z + 20.0, 150.0, found, 16); hazards.boxes.assign(found, found + n); }
            const auto advice = routehazard::Advise(hazards, [&](double s) { return car.path.At(s); }, car.station, car.path.End(), car.railSpeed, halfWidth, halfLength, height, car.hazardOffset, dt);
            if (advice.blocked || advice.detour) { static ULONGLONG told = 0; const auto t = GetTickCount64(); if (t - told > 2000) { told = t; logger::Write("OBS system=world.hazard event=rail_%s vehicle=%08X offset_m=%.2f stop_in_m=%.1f reason=hazard_box_on_the_rail_path", advice.blocked ? "stop" : "detour", vehicle, advice.offset, advice.stopIn); } }
            car.hazardOffset = advice.offset;
            const double v = fleetrail::Approach(car.railSpeed, std::min({ want, car.path.CornerSpeed(car.station), advice.speedCap }), dt);
            const double st = car.path.Advance(car.station, std::max(0.0, v) * dt);
            auto pt = car.path.At(st); const auto q = car.path.At(car.path.Advance(st, 1.0));
            if (!std::isfinite(pt.x + pt.y + pt.z)) return false;
            car.station = st; car.railSpeed = v; car.position = pt;
            if (advice.offset != 0.0) { const double dx = q.x - pt.x, dy = q.y - pt.y, len = std::hypot(dx, dy); if (len > 1e-6) { pt.x += dy / len * advice.offset; pt.y -= dx / len * advice.offset; } }
            ox = pt.x; oy = pt.y; oz = pt.z; yaw = std::atan2(q.x - pt.x, q.y - pt.y); used = v; return true;
        }
        bool FarYaw(std::uintptr_t vehicle, double yaw) { __try { reinterpret_cast<float*>(vehicle + off::kRefRotation)[2] = float(yaw); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        bool FarLevelYaw(std::uintptr_t vehicle, double yaw) { __try { float* r = reinterpret_cast<float*>(vehicle + off::kRefRotation); r[0] = 0.0f; r[1] = 0.0f; r[2] = float(yaw); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
        bool FleetRailOwns(unsigned vehicle)
        {
            if (!g_fleetRailCount.load(std::memory_order_acquire) || !trafficprofile::FleetRailAllowed()) return false;
            std::lock_guard lk(g_fleetRailLock);
            const auto it = g_fleetRails.find(vehicle);
            return it != g_fleetRails.end() && it->second.railOwner;
        }

        bool FleetRailStep(std::uintptr_t vehicle, std::uint32_t body, float seconds)
        {
            if (!g_fleetRailCount.load(std::memory_order_acquire) || !trafficprofile::FleetRailAllowed()) return false;
            std::lock_guard lk(g_fleetRailLock);
            if (!trafficprofile::FleetRailWritesAllowed()) {
                for (const auto& [id, car] : g_fleetRails)
                    if (car.railOwner && car.instance == vehicle && car.body == body) return true;
                return false;
            }
            for (auto& [id, car] : g_fleetRails) {
                if (!car.railOwner || car.instance != vehicle || car.body != body) continue;
                if (g_manualDriverId.load(std::memory_order_relaxed) == id) return false;
                const double dt = physicsstep::ModelSeconds(seconds);
                if (dt <= 0.0) return true;
                if (!car.blendReady) {
                    // Entry blend: start from where the car actually is and how fast it actually moves.
                    FvoFleetRailPoint pose{}, moving{};
                    const auto onPath = car.path.At(car.station);
                    if (ReadRailBody(vehicle, body, pose, moving)) {
                        car.offset = {pose.x - onPath.x, pose.y - onPath.y, pose.z - onPath.z};
                        if (!(std::hypot(car.offset.x, car.offset.y, car.offset.z) <= fleetrail::rules::kMaxBlendOffset)) car.offset = {};
                        car.railSpeed = std::clamp(std::hypot(moving.x, moving.y, moving.z), 0.0, 150.0);
                    } else {
                        car.offset = {};
                        car.railSpeed = car.speed;
                    }
                    car.blendT = 0.0;
                    car.blendReady = true;
                    logger::Write("OBS system=traffic.fleet_rail event=entry_blend vehicle=%08X offset_m=%.2f speed=%.1f target_speed=%.1f",
                        id, std::hypot(car.offset.x, car.offset.y, car.offset.z), car.railSpeed, car.speed);
                }
                double want = std::min(car.speed, car.path.CornerSpeed(car.station));
                // Structural car-to-car headway spacing on FleetRail so faster cars never stack nose-to-tail.
                // Only yield to cars moving in the SAME direction (dot > 0.5) and within the same lane (lat < 2.8 m)
                // so opposing traffic on narrow 2-way roads never head-on deadlocks.
                {
                    const auto curPt = car.path.At(car.station);
                    const auto fwdPt = car.path.At(car.path.Advance(car.station, 3.0));
                    const double fLen = std::hypot(fwdPt.x - curPt.x, fwdPt.y - curPt.y);
                    if (fLen > 0.1) {
                        const double ftx = (fwdPt.x - curPt.x) / fLen, fty = (fwdPt.y - curPt.y) / fLen;
                        for (const auto& [otherId, other] : g_fleetRails) {
                            if (otherId == id) continue;
                            if (other.mode != fleetrail::Mode::Rail || other.railSpeed < 0.5) continue;
                            const double odx = other.position.x - curPt.x;
                            const double ody = other.position.y - curPt.y;
                            const double odz = other.position.z - curPt.z;
                            if (std::fabs(odz) > 4.5) continue;
                            const double fwd = odx * ftx + ody * fty;
                            if (fwd <= 0.5 || fwd >= 24.0) continue;
                            const double lat = std::fabs(-odx * fty + ody * ftx);
                            if (lat >= 2.8) continue;
                            // Direction check: ignore opposing-lane traffic.
                            if (other.path.End() > 1.0) {
                                const auto oCur = other.path.At(other.station);
                                const auto oFwd = other.path.At(other.path.Advance(other.station, 3.0));
                                const double oLen = std::hypot(oFwd.x - oCur.x, oFwd.y - oCur.y);
                                if (oLen > 0.1) {
                                    const double dot = ((oFwd.x - oCur.x) / oLen) * ftx + ((oFwd.y - oCur.y) / oLen) * fty;
                                    if (dot < 0.5) continue;
                                }
                            }
                            const double gapCap = fwd <= 9.0 ? 0.0 : car.speed * ((fwd - 9.0) / 15.0);
                            want = std::min(want, std::max(0.0, gapCap));
                        }
                    }
                }
                const double railSpeed = fleetrail::Approach(car.railSpeed, want, dt);
                const double station = car.path.Advance(car.station, railSpeed * dt);
                const double w0 = fleetrail::BlendWeight(car.blendT), w1 = fleetrail::BlendWeight(car.blendT + dt);
                const auto herePt = car.path.At(station);
                const auto aheadPt = car.path.At(car.path.Advance(station, 4.0));
                const auto farPt = car.path.At(car.path.Advance(station, 8.0));
                // Clamp far-rail lateral rejoin offset inside the corridor envelope so cars never drift into guardrails.
                if (w1 > 0.01) {
                    const double segLen = std::hypot(aheadPt.x - herePt.x, aheadPt.y - herePt.y);
                    if (segLen > 0.1) {
                        const double ux = (aheadPt.x - herePt.x) / segLen, uy = (aheadPt.y - herePt.y) / segLen;
                        const double rx = uy, ry = -ux;
                        const double latOff = car.offset.x * rx + car.offset.y * ry;
                        constexpr double kMaxRailLat = 2.5;
                        if (std::fabs(latOff) > kMaxRailLat) {
                            const double excess = latOff - std::clamp(latOff, -kMaxRailLat, kMaxRailLat);
                            car.offset.x -= excess * rx;
                            car.offset.y -= excess * ry;
                            archproof::State().corridorRailClamped.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                }
                const auto Shifted = [&](FvoFleetRailPoint p, double w) {
                    return FvoFleetRailPoint{p.x + car.offset.x * w, p.y + car.offset.y * w, p.z + car.offset.z * w};
                };
                const auto target = Shifted(herePt, w1);
                const auto previous = Shifted(car.path.At(car.station), w0);
                const FvoFleetRailPoint velocity{(target.x - previous.x) / dt,
                    (target.y - previous.y) / dt, (target.z - previous.z) / dt};
                // Energy-optimal alignment: match velocity direction (including rejoin blend) + curvature feedforward yaw rate.
                double alignX = aheadPt.x - herePt.x, alignY = aheadPt.y - herePt.y;
                const double vHoriz = std::hypot(velocity.x, velocity.y);
                if (vHoriz > 1.5) {
                    const double aLen = std::hypot(alignX, alignY);
                    if (aLen > 0.1) {
                        alignX = 0.65 * (velocity.x / vHoriz) + 0.35 * (alignX / aLen);
                        alignY = 0.65 * (velocity.y / vHoriz) + 0.35 * (alignY / aLen);
                    } else {
                        alignX = velocity.x; alignY = velocity.y;
                    }
                }
                double feedYawRate = 0.0;
                {
                    const double d1 = std::hypot(aheadPt.x - herePt.x, aheadPt.y - herePt.y);
                    const double d2 = std::hypot(farPt.x - aheadPt.x, farPt.y - aheadPt.y);
                    if (d1 > 0.5 && d2 > 0.5) {
                        const double a1 = std::atan2(aheadPt.y - herePt.y, aheadPt.x - herePt.x);
                        const double a2 = std::atan2(farPt.y - aheadPt.y, farPt.x - aheadPt.x);
                        const double dAng = std::remainder(a2 - a1, 6.283185307179586);
                        feedYawRate = std::clamp(railSpeed * (dAng / 4.0), -1.8, 1.8);
                    }
                }
                if (const char* fault = WriteRailPose(vehicle, body, target, velocity, alignX, alignY, feedYawRate)) {
                    if (std::strcmp(fault, "step_over_25m") == 0) {
                        // Auto-reseed station from the body's actual position only if the nearest path point is within
                        // 24 m (prevents an infinite re-seed loop if the car genuinely fell > 25 m off the road).
                        FvoFleetRailPoint curPose{}, curVel{};
                        if (ReadRailBody(vehicle, body, curPose, curVel)) {
                            const double projS = car.path.Project(curPose.x, curPose.y, curPose.z);
                            const auto projPt = car.path.At(projS);
                            if (std::hypot(projPt.x - curPose.x, projPt.y - curPose.y, projPt.z - curPose.z) <= 24.0) {
                                car.station = projS;
                                car.position = curPose;
                                car.blendReady = false;
                                car.fault = "";
                                car.lastStep = GetTickCount64();
                                return true;
                            }
                        }
                    }
                    if (car.fault != fault)
                        logger::Write("OBS system=traffic.fleet_rail event=engine_result vehicle=%08X mode=awaiting reason=%s", id, fault);
                    car.fault = fault;
                    if (car.mode != fleetrail::Mode::Awaiting) car.modeSince = GetTickCount64();
                    car.mode = fleetrail::Mode::Awaiting;
                    return true;
                }
                car.fault = "";
                if (car.mode != fleetrail::Mode::Rail) car.modeSince = GetTickCount64();
                car.mode = fleetrail::Mode::Rail;
                car.station = station;
                car.railSpeed = railSpeed;
                car.blendT += dt;
                car.position = target;
                car.lastStep = GetTickCount64();
                return true;
            }
            return false;
        }

#ifndef FVO_RELEASE
		// Relocation doer, physics-thread half (diagnostic builds). One armed request; issued at the armed chassis's next action call, which
		// runs inside the world step before it integrates. The substitution adds the displacement to the motion's centre of mass (double X/Y at
		// +0x10/+0x18, Z at +0x20: the integrator's own store site) and to the body position (+0x30), so the step integrates from the new pose
		// and the engine's own publisher carries it to the body, bounds and listeners. Nothing here decides; it only writes once and records facts.
		namespace reloc {
			bool RailOnStep(std::uintptr_t vehicle, float seconds);
            bool RailOwns(std::uint32_t vehicle);
			struct State {
				std::mutex lock;
				std::atomic<bool> armed{ false }, ready{ false };
				std::uint64_t serial = 0;
				std::uint64_t authority = 0;
				std::uint32_t vehicleId = 0, bodyId = 0;
				std::uintptr_t instance = 0;
				double dx = 0, dy = 0, dz = 0;
				vehicleflight::relocate::Facts facts;
				ULONGLONG lastActionAt = 0;
			};
			State g_state;
            bool HopOwns(std::uintptr_t vehicle)
            {
                if (!g_state.armed.load(std::memory_order_acquire) && !g_state.ready.load(std::memory_order_acquire)) return false;
                std::lock_guard lk(g_state.lock);
                return g_state.instance == vehicle && (g_state.armed.load() || g_state.ready.load());
            }
			// POD only: __try cannot share a frame with objects that have destructors.
			bool Write(State& st, std::uintptr_t vehicle)
			{
				auto& f = st.facts;
				__try {
					const Body b = FindBody(vehicle, 0, 0, 0, true);
					if (!b.body || !b.motion || b.id != st.bodyId || At<std::uint32_t>(b.body, 0x50) != b.id) { f.refusal = "body_changed"; return false; }
					f.bodyOk = true;
					f.ringFirstOk = At<std::uint32_t>(b.motion, 0x48) == b.id;   // the motion's first body is this chassis (the engine's own group move checks the same)
					double* com = reinterpret_cast<double*>(b.motion + 0x10);
					float* bp = reinterpret_cast<float*>(b.body + 0x30);
					const float* v = reinterpret_cast<const float*>(b.motion + 0x60);
					f.speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
					const bool finite = std::isfinite(com[0]) && std::isfinite(com[1]) && std::isfinite(com[2]) && std::isfinite(bp[0]) && std::isfinite(bp[1]) && std::isfinite(bp[2]);
					f.comNearBody = finite && std::sqrt((bp[0] - com[0]) * (bp[0] - com[0]) + (bp[1] - com[1]) * (bp[1] - com[1]) + (bp[2] - com[2]) * (bp[2] - com[2])) < 20.0;
					for (int i = 0; i < 3; ++i) { f.comBefore[i] = com[i]; f.bodyBefore[i] = bp[i]; }
					if (!f.ringFirstOk) { f.refusal = "motion_ring_not_this_body"; return false; }
					if (!f.comNearBody) { f.refusal = "motion_position_not_at_body"; return false; }
					if (!handoff::WheelGrantCurrent(st.authority)) { f.refusal = "relocation_authority_revoked"; return false; }
					com[0] += st.dx; com[1] += st.dy; com[2] += st.dz;
					bp[0] += float(st.dx); bp[1] += float(st.dy); bp[2] += float(st.dz);
					for (int i = 0; i < 3; ++i) { f.comAfter[i] = com[i]; f.bodyAfter[i] = bp[i]; }
					f.issued = true;
					return true;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { f.refusal = "fault_while_writing"; return false; }
			}
			void OnStep(std::uintptr_t vehicle)
			{
				if (!g_state.armed.load(std::memory_order_acquire)) return;
				std::unique_lock lock(g_state.lock, std::try_to_lock);
				if (!lock.owns_lock() || !g_state.armed.load() || vehicle != g_state.instance) return;
				g_state.armed.store(false, std::memory_order_release);
				Write(g_state, vehicle);
				g_state.ready.store(true, std::memory_order_release);
			}
			void AfterStep(std::uintptr_t vehicle)
			{
				if (!g_state.ready.load(std::memory_order_acquire) || vehicle != g_state.instance) return;
				std::unique_lock lock(g_state.lock, std::try_to_lock);
				if (!lock.owns_lock()) return;
				if (g_state.facts.issued) ++g_state.facts.actionCalls;
				g_state.lastActionAt = GetTickCount64();
			}
			bool ReadBody(std::uintptr_t vehicle, std::uint32_t bodyId, double* out)
			{
				__try {
					const Body b = FindBody(vehicle, 0, 0, 0, true);
					if (!b.body || !b.motion || b.id != bodyId || At<std::uint32_t>(b.body, 0x50) != b.id) return false;
					const float* bp = reinterpret_cast<const float*>(b.body + 0x30);
					out[0] = bp[0]; out[1] = bp[1]; out[2] = bp[2];
					return true;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			}
			// Position and linear velocity (far tier test read-back).
			bool ReadBodyV(std::uintptr_t vehicle, std::uint32_t bodyId, double* pos, double* vel)
			{
				__try {
					const Body b = FindBody(vehicle, 0, 0, 0, true);
					if (!b.body || !b.motion || b.id != bodyId || At<std::uint32_t>(b.body, 0x50) != b.id) return false;
					const float* bp = reinterpret_cast<const float*>(b.body + 0x30);
					const float* mv = reinterpret_cast<const float*>(b.motion + 0x60);
					for (int i = 0; i < 3; ++i) { pos[i] = bp[i]; vel[i] = mv[i]; }
					return true;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			}

			// ---- Far tier rail (diagnostic): one kinematic pose substitution per physics step for one chassis, before the world integrates ----
			// The step adds the move (target pose minus the body's pose) to the motion's centre of mass and to the body position, exactly like the hop
			// write above, and sets the linear velocity to the path's own so the integrator adds nothing. Height: farrail::Profile (terrain + ride).
			struct Rail {
				std::mutex lock;
				std::atomic<bool> active{ false };
				std::uint32_t vehicleId = 0, bodyId = 0;
				std::uintptr_t instance = 0;
				farrail::RailParams p;
				farrail::RailState st;
				farrail::Profile profile;
				vehicleflight::relocate::RailFacts facts;
			};
			Rail g_rail;
			bool RailWrite(Rail& r, std::uintptr_t vehicle, double dt)
            {
                auto& facts = r.facts;
                double ground = 0;
                const bool known = r.profile.Get(r.st.s + r.p.speed * dt, ground);
                const auto step = farrail::Step(r.st, r.p, dt, known, ground);
                if (const char* fault = WriteRailPose(vehicle, r.bodyId, {step.x, step.y, step.z},
                                                     {step.vx, step.vy, step.vz})) {
                    facts.fault = fault;
                    ++facts.faults;
                    if (fault != std::string_view{"body_changed"}) r.active.store(false, std::memory_order_release);
                    return false;
                }
                facts.steps = r.st.steps; facts.holds = r.st.holds; facts.s = r.st.s;
                facts.x = step.x; facts.y = step.y; facts.z = step.z;
                facts.target = step.target; facts.terrainKnown = step.terrainKnown;
                return true;
            }
            bool RailOwns(std::uint32_t vehicle)
            {
                auto& r = g_rail;
                std::lock_guard lk(r.lock);
                return r.active.load(std::memory_order_acquire) && vehicle == r.vehicleId;
            }
            bool RailOnStep(std::uintptr_t vehicle, float seconds)
            {
                auto& r = g_rail;
                if (!r.active.load(std::memory_order_acquire)) return false;
                std::lock_guard lk(r.lock);
                if (!r.active.load(std::memory_order_acquire) || vehicle != r.instance) return false;
                const double dt = physicsstep::ModelSeconds(seconds);
                if (dt > 0.0) RailWrite(r, vehicle, dt);
                return true;
            }
		}
#endif
		// Multi-vehicle far-dispatch pose substitution inside StepDetour (2026-11-23):
		// Shifts both hknpMotion + 0x10 (COM) and hknpBody + 0x30 (pos) and zeroes linear velocity on the physics thread
		// right before Havok integrates the vehicle's first step, so Havok commits the far route position instead of
		// overwriting a pre-step service-thread write back to the near-stage spawn point.
		namespace fardisp {
			constexpr int kMaxSlots = 128;
			struct Slot {
				std::uint32_t vehicleId = 0, driverId = 0, bodyId = 0;
				std::uintptr_t instance = 0;
				double x = 0, y = 0, z = 0;
				bool active = false;
			};
			std::mutex g_lock;
			Slot g_slots[kMaxSlots];
			std::atomic<int> g_count{ 0 };

			void ReadBinding(std::uint32_t vehicleId, std::uintptr_t& outInst, std::uint32_t& outBody)
			{
				outInst = 0; outBody = 0;
				const auto comp = vehiclediag::ComponentOf(vehicleId);
				if (!comp) return;
				__try {
					outInst = At<std::uintptr_t>(comp, 0x10);
					outBody = outInst ? At<std::uint32_t>(outInst, 0x20) : 0;
				} __except (EXCEPTION_EXECUTE_HANDLER) { outInst = 0; outBody = 0; }
			}

			bool ApplyPose(std::uintptr_t vehicle, std::uint32_t bodyId, double x, double y, double z)
			{
				__try {
					const Body b = FindBody(vehicle, 0, 0, 0, true);
					if (!b.body || !b.motion || b.id != bodyId || At<std::uint32_t>(b.body, 0x50) != b.id) return false;
					if (At<std::uint32_t>(b.motion, 0x48) != b.id) return false;
					double* com = reinterpret_cast<double*>(b.motion + 0x10);
					float* bp = reinterpret_cast<float*>(b.body + 0x30);
					float* vel = reinterpret_cast<float*>(b.motion + 0x60);
					if (!std::isfinite(com[0]) || !std::isfinite(com[1]) || !std::isfinite(com[2]) ||
						!std::isfinite(bp[0]) || !std::isfinite(bp[1]) || !std::isfinite(bp[2])) return false;
					const double dx = x - double(bp[0]), dy = y - double(bp[1]), dz = z - double(bp[2]);
					com[0] += dx; com[1] += dy; com[2] += dz;
					bp[0] = float(x); bp[1] = float(y); bp[2] = float(z);
					vel[0] = 0.0f; vel[1] = 0.0f; vel[2] = 0.0f;
					alignas(16) float zeroSpin[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
					RailSetSpin(b, zeroSpin);
					return true;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			}

			void Arm(std::uint32_t vehicleId, std::uint32_t driverId, double x, double y, double z)
			{
				if (!vehicleId || !std::isfinite(x + y + z)) return;
				std::uintptr_t inst = 0; std::uint32_t bid = 0;
				ReadBinding(vehicleId, inst, bid);
				std::lock_guard lk(g_lock);
				int freeIdx = -1;
				for (int i = 0; i < kMaxSlots; ++i) {
					if (g_slots[i].active && g_slots[i].vehicleId == vehicleId) {
						g_slots[i] = { vehicleId, driverId, bid, inst, x, y, z, true };
						return;
					}
					if (!g_slots[i].active && freeIdx < 0) freeIdx = i;
				}
				if (freeIdx >= 0) {
					g_slots[freeIdx] = { vehicleId, driverId, bid, inst, x, y, z, true };
					g_count.fetch_add(1, std::memory_order_release);
				}
			}

			bool HasPending(std::uint32_t vehicleId)
			{
				if (!vehicleId || g_count.load(std::memory_order_acquire) <= 0) return false;
				std::lock_guard lk(g_lock);
				for (int i = 0; i < kMaxSlots; ++i)
					if (g_slots[i].active && g_slots[i].vehicleId == vehicleId) return true;
				return false;
			}

			void Clear()
			{
				std::lock_guard lk(g_lock);
				for (auto& s : g_slots) s = {};
				g_count.store(0, std::memory_order_release);
			}

			void OnStep(std::uintptr_t vehicle, std::uint32_t bodyId)
			{
				if (g_count.load(std::memory_order_acquire) <= 0 || (!vehicle && !bodyId)) return;
				double tx = 0, ty = 0, tz = 0;
				std::uint32_t vid = 0, did = 0;
				bool found = false;
				{
					std::unique_lock lk(g_lock, std::try_to_lock);
					if (!lk.owns_lock()) return;
					for (int i = 0; i < kMaxSlots; ++i) {
						if (!g_slots[i].active) continue;
						if (!g_slots[i].instance && !g_slots[i].bodyId)
							ReadBinding(g_slots[i].vehicleId, g_slots[i].instance, g_slots[i].bodyId);
						if ((g_slots[i].instance && g_slots[i].instance == vehicle) ||
							(g_slots[i].bodyId && g_slots[i].bodyId == bodyId)) {
							vid = g_slots[i].vehicleId;
							did = g_slots[i].driverId;
							tx = g_slots[i].x; ty = g_slots[i].y; tz = g_slots[i].z;
							g_slots[i].active = false;
							g_count.fetch_sub(1, std::memory_order_release);
							found = true;
							break;
						}
					}
				}
				if (!found) return;
				const bool ok = ApplyPose(vehicle, bodyId, tx, ty, tz);
				logger::Observe("traffic.dispatch-step", vid, ok ? 1u : 0u,
					"driver=%08X body=%08X moved=%d to=%.1f,%.1f,%.1f", did, bodyId, ok ? 1 : 0, tx, ty, tz);
			}
		}
		std::uint64_t StepDetour(std::uintptr_t vehicle, std::uintptr_t world, float seconds)
		{
			StepTiming timing;const auto massEpoch=bodymass::observations.Epoch();
			g_nativeStepSamples.fetch_add(1, std::memory_order_relaxed);
			g_nativeStepTotal.fetch_add(1, std::memory_order_relaxed);
			g_nativeStepSeconds.store(seconds, std::memory_order_relaxed);
			if (physicsstep::ModelSeconds(seconds) == 0.0) g_nativeStepSkipped.fetch_add(1, std::memory_order_relaxed);
			// This hook sees every vehicle. Unowned steps must never default to the player's controller.
			vehiclediag::NoteStep(vehicle);
			const auto body=PhysicsBodyId(vehicle);const auto now=GetTickCount64();
			fardisp::OnStep(vehicle, body);
            ObserveChassisMass(vehicle,now,massEpoch);
#ifdef FVO_RELEASE
            const bool railOwnsStep = FleetRailStep(vehicle, body, seconds);
#else
            const bool railOwnsStep = reloc::RailOnStep(vehicle, seconds) ||
                (!reloc::HopOwns(vehicle) && FleetRailStep(vehicle, body, seconds));
#endif
            if (railOwnsStep) {
                // The native step publishes the body. Stale FVO force/wheel controllers do not run in rail mode.
                for (auto& binding : g_shs)
                    if (binding.physInstance.load() == vehicle) PhysicalRelease(binding);
                timing.BeginNative();
                const auto result = g_original(vehicle, world, seconds);
                timing.EndNative();
                return result;
            }
			bodybinding::Binding bindings[kPilots];
            int wheelObserver=-1;
            bool ambiguousWheelObserver=false;
            std::uintptr_t wheelRef=0;
			// An empty slot owns nothing (Owner only counts enabled ones): skipped before its lock (2026-10-29 audit: this runs for every
			// vehicle on every physics step, and locked all 257 slots each time).
			for(int i=0;i<kPilots;++i){auto&s=g_shs[i];if(!s.vehicle.load()){bindings[i]={};continue;}std::lock_guard lk(s.lock);
				const bool owns=bodybinding::Contains({s.sceneBodies,s.sceneCount},body) && (!s.instance.load()||s.bodyId.load()==body);
                if(i>0&&owns&&g_driveOut[i].Active()){
                    ambiguousWheelObserver|=wheelObserver>=0;
                    wheelObserver=i;wheelRef=s.vehicle.load();
                }
				bindings[i]={owns?body:0,s.vehicle.load()&&(s.active||s.groundOn||(s.railOn&&GetTickCount64()<s.railUntil)||(s.guardInstance&&now<s.guardUntil))};}
			const int owner=bodybinding::Owner(body,bindings);
			NoteSeen(body);
			if(owner<0){int claims=0;for(const auto&bb:bindings)if(bb.enabled&&bodybinding::Matches(body,bb.body))++claims;
				if(claims>1){ambiguousWheelObserver=true;g_ambiguous.fetch_add(1,std::memory_order_relaxed);g_ambiguousBody.store(body,std::memory_order_relaxed);}}
			Shared*s=owner>=0?&g_shs[owner]:nullptr;
			if(s){
				{std::lock_guard lk(s->lock);if(!bodybinding::Contains({s->sceneBodies,s->sceneCount},body)||
					(s->instance.load()&&s->bodyId.load()!=body))s=nullptr;else{s->bodyId.store(body);s->instance.store(vehicle);}}
				if(s)CheckCollision(*s,vehicle);
			}
			if (s) PreparePhysical(*s,vehicle);
			for (auto& o : g_shs) if (&o!=s && o.physInstance.load()==vehicle) PhysicalRelease(o);  // a chassis nobody owns now
			// Probe: the whole native step's effect on a rail car's speed (wheels, drive model, contacts, damping, clamp together).
			const std::uintptr_t probeMotion = s && s->railOn ? s->railMotion.load(std::memory_order_relaxed) : 0;
			const double probeBefore = probeMotion ? ProbeSpeed(probeMotion) : -1;
			GripSettings(now); ApplyGrip(vehicle, body, double(now) / 1000.0);  // GT-1: the game's grip and turning weight x the gear, before the wheels run
			const int gripSlot = SlotOfBody(body); const int gripGear = gripSlot >= 0 ? GearOfSlot(gripSlot) : 0;
			bool gripHands = gripSlot >= 0;  // GK-1: not while the hover or the rail writes this car's velocity
			if (gripHands) { auto& gs = g_shs[gripSlot]; std::lock_guard lk(gs.lock); if (gs.active || (gs.railOn && !gs.railTyres)) gripHands = false; }
			ApplyTraction(vehicle, gripHands ? gripSlot : -1, gripHands ? gripGear : 0, double(seconds), double(now) / 1000.0);
#ifndef FVO_RELEASE
			reloc::OnStep(vehicle);   // the armed relocation, if this is its chassis: one substitution before the world integrates
#endif
			timing.BeginNative();
			const auto result = g_original(vehicle, world, seconds);
			timing.EndNative();
#ifndef FVO_RELEASE
			reloc::AfterStep(vehicle);
#endif
			if (gripSlot >= 0) { float ws = 0; if (WheelSlip(vehicle, ws)) { g_wheelSlip[gripSlot].store(ws, std::memory_order_relaxed); g_wheelSlipAny[gripSlot].store(ws, std::memory_order_relaxed); } }  // AG-1 / GK-1: hand or autopilot
			if (gripHands) { MeasureRear(vehicle, gripSlot, gripGear, double(now) / 1000.0);  // RB-1: the back stepping out, measured
				ApplyCorners(vehicle, gripSlot, gripGear, double(seconds), double(now) / 1000.0); }  // CF-1: four corner forces within the gear's budget
			if (probeMotion && probeBefore >= 0) { const double after = ProbeSpeed(probeMotion); if (after >= 0) { s->probeNativeDrop.fetch_add((long long)std::llround((probeBefore - after) * 1000.0)); s->probeNativeSteps.fetch_add(1); } }
            if(!s){
                // Native wheel mode can leave the mod's physics controller idle.
                // Observe the existing native step without enabling another writer.
                if(wheelObserver>0&&!ambiguousWheelObserver&&physicsstep::ModelSeconds(seconds)>0.0){
                    auto& wheel=g_shs[wheelObserver];
                    std::unique_lock lock(wheel.lock,std::try_to_lock);
                    if(lock.owns_lock()&&wheelRef==wheel.vehicle.load()&&g_driveOut[wheelObserver].Active()&&
                        bodybinding::Contains({wheel.sceneBodies,wheel.sceneCount},body)){
                        ++wheel.handoffSteps;wheel.handoffStepAt=now;
                        wheel.handoffStepInstance=vehicle;wheel.handoffStepBody=body;
                    }
                }
                return result;
            }
			ObserveCarModel(*s,vehicle);  // logs only in diagnostic builds (the logger is compiled out in release)
			const auto ref=s->vehicle.load();
			if (ref && ref == g_lastDrivenVehicle.load(std::memory_order_relaxed)) {
				(ref == g_driverVehicle.load(std::memory_order_relaxed) ? g_occupiedSteps : g_unoccupiedSteps).fetch_add(1, std::memory_order_relaxed);
			}
            {std::lock_guard lock(s->lock);
             if(owner>0&&s->instance.load()==vehicle&&bodybinding::Matches(body,s->bodyId.load())&&
                physicsstep::ModelSeconds(seconds)>0.0){
                ++s->handoffSteps;s->handoffStepAt=now;s->handoffStepInstance=vehicle;s->handoffStepBody=body;
             }
             if(!s->active) {
                const auto passiveBody=FindBody(vehicle,0,0,0,true);
                if(passiveBody.motion&&bodybinding::Matches(passiveBody.id,s->bodyId.load())) {
                    const auto v=reinterpret_cast<const float*>(passiveBody.motion+0x60);
                    for(int k=0;k<3;++k)s->lastV[k]=v[k];
                    s->haveLast=std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);
                }
            }}
            return StepModel(*s, vehicle, result, seconds);
		}
		static_assert(std::is_same_v<decltype(&StepDetour), Step_t>);

		bool RailReadSpin(const Body& b, float* spin)
		{
			__try { VFn<void(*)(std::uintptr_t, std::uint32_t, float*)>(b.read, 0xA8)(b.read, b.id, spin); return std::isfinite(spin[0]) && std::isfinite(spin[1]) && std::isfinite(spin[2]); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool RailSetSpin(const Body& b, const float* spin)
		{
			__try { VFn<void(*)(std::uintptr_t, std::uint32_t, const float*, int)>(b.write, 0xB0)(b.write, b.id, spin, 0); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		std::uint64_t StepModel(Shared& g_sh, std::uintptr_t vehicle, std::uint64_t result, float seconds)
		{
			const double dt = physicsstep::ModelSeconds(seconds);
			if (dt == 0.0) return result;
			std::unique_lock lk(g_sh.lock);
			// The rail takes over from the release guard (2026-11-05 09:50:58: Cruise switched hover off into drive mode, the guard then
			// zeroed the car's sideways speed every step while the rail asked for 22 m/s: the car stopped). The rail's own acceleration
			// limit already prevents a launch.
			if (g_sh.railOn && g_sh.guardInstance && GetTickCount64() <= g_sh.railUntil) { g_sh.guardInstance = 0; g_sh.guardVehicle = 0; g_sh.guardMax = 0; g_sh.probeGuardClears.fetch_add(1); }
			if (!g_sh.active && g_sh.guardInstance && vehicle == g_sh.guardInstance) {
				if (GetTickCount64() > g_sh.guardUntil || g_sh.guardVehicle == g_driverVehicle.load(std::memory_order_relaxed)) {
					if (g_sh.guardMax > 0.5f) logger::Write("Rover call: release guard ended (fastest sideways speed caught %.1f m/s)", g_sh.guardMax);
					g_sh.guardVehicle = 0;
					g_sh.guardInstance = 0;
					g_sh.guardMax = 0;
					return result;
				}
				const float gx = g_sh.px, gy = g_sh.py, gz = g_sh.pz;
				lk.unlock();
				const auto gb = FindBody(vehicle, gx, gy, gz, true);  // its own body wherever it went (13:50: it flew 330 m)
				lk.lock();
				if (gb.body && !g_sh.active && bodybinding::Matches(gb.id,g_sh.bodyId.load())) {
					float* gv = reinterpret_cast<float*>(gb.motion + 0x60);
					const float hs = std::sqrt(gv[0] * gv[0] + gv[1] * gv[1]);
					if (std::isfinite(hs)) {
						g_sh.guardMax = std::max(g_sh.guardMax, hs);
						gv[0] = 0.0f;
						gv[1] = 0.0f;
						if (gv[2] > 0.0f) gv[2] = 0.0f;  // no launch upward either
					}
					const float* bp = reinterpret_cast<const float*>(gb.body + 0x30);
					g_sh.px = bp[0]; g_sh.py = bp[1]; g_sh.pz = bp[2];  // keep finding the body
				}
				return result;
			}
			if (!g_sh.active && g_sh.railOn) {
				if (GetTickCount64() > g_sh.railUntil || !vehicle) { g_sh.railOn = false; return result; }
				const float gx = g_sh.px, gy = g_sh.py, gz = g_sh.pz, tvx = g_sh.railVx, tvy = g_sh.railVy;
				float tyaw = g_sh.railYaw;
				lk.unlock();
				const auto gb = FindBody(vehicle, gx, gy, gz);
				alignas(16) float spin[4] = {};
				const bool read = gb.body && gb.read && gb.write && RailReadSpin(gb, spin);
				lk.lock();
				if (!read || g_sh.active || !bodybinding::Matches(gb.id, g_sh.bodyId.load())) { ++g_sh.railFaults; return result; }
				float* gv = reinterpret_cast<float*>(gb.motion + 0x60);
				if (!std::isfinite(gv[0]) || !std::isfinite(gv[1])) { ++g_sh.railFaults; return result; }
				// Rail force rules (railforce.h): along the road plane, little sideways slip, real acceleration limits, hands off the
				// vertical in the air. (Was: 35 % of the gap per step, always flat: loaded the suspension on ramps.)
				{
					const float* F = reinterpret_cast<const float*>(gb.body + 0x10);
					const float* U = reinterpret_cast<const float*>(gb.body + 0x20);
					g_sh.railMotion.store(gb.motion, std::memory_order_relaxed);
					g_sh.probeMeasured = std::hypot(double(gv[0]), double(gv[1])); g_sh.probeTarget = std::hypot(double(tvx), double(tvy));
					const bool roadFresh = GetTickCount64() - g_sh.railRoadAt < 1000;
					const railforce::V3 road{ g_sh.railRoad[0], g_sh.railRoad[1], g_sh.railRoad[2] };
					const auto o = railforce::Step({ gv[0], gv[1], gv[2] }, { tvx, tvy, 0 }, { F[0], F[1], F[2] }, { U[0], U[1], U[2] }, dt, g_sh.railAir, g_sh.railCommand, roadFresh ? &road : nullptr);
					g_sh.probeRoadZ = roadFresh ? road.z : -1;
					g_sh.probeWrote = o.tipped ? g_sh.probeMeasured : std::hypot(o.v.x, o.v.y); g_sh.probeNz = U[2]; g_sh.probeSlip = o.slip; g_sh.probeGrounded = o.grounded; g_sh.probeTipped = o.tipped;
					g_sh.probeRailSteps.fetch_add(1);
					{ WheelRead wr; if (ReadWheels(g_sh.instance.load(), wr)) { g_sh.probeWheels = wr.count; g_sh.probeWheelsTouching = wr.touching; g_sh.probeWheelZ = wr.z; g_sh.probeWheelUp = wr.up; } else g_sh.probeWheels = -1; }
					if (GetTickCount64() - g_sh.probeAt >= 5000) {  // PF-2: every 5 s (cuts physics-thread log I/O 5x)
						g_sh.probeAt = GetTickCount64();
						const unsigned ns = g_sh.probeNativeSteps.exchange(0), dc = g_sh.probeDampCalls.exchange(0), cc = g_sh.probeClampCalls.exchange(0), rsN = g_sh.probeRailSteps.exchange(0), gcl = g_sh.probeGuardClears.exchange(0);
						const double nd = g_sh.probeNativeDrop.exchange(0) / 1000.0, dd = g_sh.probeDampDrop.exchange(0) / 1000.0, cd = g_sh.probeClampDrop.exchange(0) / 1000.0;
						logger::Write("OBS system=traffic.railprobe slot=%d rail_steps=%u native_steps=%u measured=%.2f command=%.2f target=%.2f wrote=%.2f grounded=%d tipped=%d up_z=%.2f slip=%.2f native_drop=%.2f damp_drop=%.2f damp_calls=%u clamp_drop=%.2f clamp_calls=%u guard_clears=%u wheels=%d wheels_touching=%d wheel_ground_z=%.2f wheel_up_z=%.2f body_z=%.2f road_up_z=%.3f",
							int(&g_sh - g_shs), rsN, ns, g_sh.probeMeasured, o.command, g_sh.probeTarget, g_sh.probeWrote, int(o.grounded), int(o.tipped), double(U[2]), o.slip, nd, dd, dc, cd, cc, gcl,
							g_sh.probeWheels, g_sh.probeWheelsTouching, g_sh.probeWheelZ, g_sh.probeWheelUp, double(reinterpret_cast<const float*>(gb.body + 0x30)[2]), g_sh.probeRoadZ);
					}
					if (o.tipped) {  // on its side or roof: no push, no turn; the game's physics settles it
						const float* bp = reinterpret_cast<const float*>(gb.body + 0x30);
						g_sh.px = bp[0]; g_sh.py = bp[1]; g_sh.pz = bp[2];
						g_sh.lastStepAt = GetTickCount64(); g_sh.lastStepInstance = g_sh.instance.load(); ++g_sh.railFaults;
						return result;
					}
					const bool unloadedRoadHold = g_sh.probeWheels >= 0 && g_sh.probeWheelsTouching == 0 && !g_sh.railAir &&
						g_sh.railFloorAt != 0 && std::isfinite(g_sh.railFloorZ);
					const bool unloadedVoidArrest = g_sh.probeWheels >= 0 && g_sh.probeWheelsTouching == 0 && !g_sh.railAir &&
						!unloadedRoadHold && (gv[2] < -10.0f || (g_sh.pz < -220.0 && gv[2] < -4.0f));
					if (g_sh.railTyres && !unloadedRoadHold && !unloadedVoidArrest) {
						if (o.grounded && g_sh.probeWheels > 0 && g_sh.probeWheelsTouching > 0) {
							const auto dv = railforce::TyreAssist({gv[0],gv[1],gv[2]}, {tvx,tvy,0},
								roadFresh ? road : railforce::V3{U[0],U[1],U[2]}, dt);
							gv[0] += float(dv.x); gv[1] += float(dv.y); gv[2] += float(dv.z);
						}
						const float* tp = reinterpret_cast<const float*>(gb.body + 0x30);
						bool needSpinWrite = false;
						// 60 Hz virtual guardrail barrier: zero outward lateral velocity if the car reaches the corridor wall.
						if (g_sh.railWall.valid) {
							const auto wall = corridorwall::Enforce60Hz(g_sh.railWall, double(tp[0]), double(tp[1]), double(gv[0]), double(gv[1]));
							if (wall.arrested) {
								gv[0] = float(wall.vx); gv[1] = float(wall.vy);
								spin[2] = float(wall.yawNudge);
								needSpinWrite = true;
								archproof::State().corridorPhysWallArrest.fetch_add(1, std::memory_order_relaxed);
							}
						}
						// 60 Hz closed-loop heading overshoot brake for TY-1 tyre mode (prevents fishtail between slow plan ticks).
						if (!needSpinWrite && g_sh.railAimValid && std::isfinite(g_sh.railAim)) {
							const float* liveF = reinterpret_cast<const float*>(gb.body + 0x10);
							if (std::isfinite(liveF[0]) && std::isfinite(liveF[1]) && std::hypot(double(liveF[0]), double(liveF[1])) > 0.1) {
								const double liveH = std::atan2(double(liveF[0]), double(liveF[1]));
								const double eLive = Wrap(double(g_sh.railAim) - liveH);
								double dampedZ = double(spin[2]);
								if (corridorwall::DampTyreYaw60Hz(eLive, double(spin[2]), double(g_sh.railMaxYaw), dampedZ)) {
									spin[2] = float(dampedZ);
									needSpinWrite = true;
									archproof::State().corridorTyreYawDamped.fetch_add(1, std::memory_order_relaxed);
								}
							}
						}
						for (int k = 0; k < 3; ++k) g_sh.v[k] = gv[k];
						g_sh.px = tp[0]; g_sh.py = tp[1]; g_sh.pz = tp[2];
						if (needSpinWrite) {
							lk.unlock();
							RailSetSpin(gb, spin);
							lk.lock();
						}
						++g_sh.railSteps;
						g_sh.lastStepAt = GetTickCount64(); g_sh.lastStepInstance = g_sh.instance.load(); ++g_sh.steps; g_sh.testSteps.fetch_add(1, std::memory_order_relaxed);
						return result;
					}
					if (std::isfinite(o.v.x) && std::isfinite(o.v.y) && std::isfinite(o.v.z)) { gv[0] = float(o.v.x); gv[1] = float(o.v.y); gv[2] = float(o.v.z); }
					// Unloaded-road support for far dynamic drive cars (beyond Starfield's ~240 m Havok collision grid):
					// when no wheel touches a collider and we have the authored road floor Z, hold the chassis on the
					// road plane so gravity does not drop it through the unloaded cell (e.g. z = -5000).
					if (unloadedRoadHold) {
						const float* curPos = reinterpret_cast<const float*>(gb.body + 0x30);
						const double targetZ = g_sh.railFloorZ + 0.55;
						if (double(curPos[2]) <= targetZ + 1.5) {
							gv[2] = float(std::clamp((targetZ - double(curPos[2])) * 4.0, -4.0, 8.0));
							spin[0] = 0.0f; spin[1] = 0.0f;
						}
					} else if (unloadedVoidArrest) {
						// Even if railFloorAt was not yet set or cleared, never let a wheel-less drive car plunge into the void.
						gv[2] = 0.0f;
						spin[0] = 0.0f; spin[1] = 0.0f;
					}
					if (g_sh.railWall.valid) {
						const float* wPos = reinterpret_cast<const float*>(gb.body + 0x30);
						const auto wall = corridorwall::Enforce60Hz(g_sh.railWall, double(wPos[0]), double(wPos[1]), double(gv[0]), double(gv[1]));
						if (wall.arrested) {
							gv[0] = float(wall.vx); gv[1] = float(wall.vy);
							tyaw = float(wall.yawNudge);
							archproof::State().corridorPhysWallArrest.fetch_add(1, std::memory_order_relaxed);
						}
					}
					for (int k = 0; k < 3; ++k) g_sh.v[k] = gv[k];  // the wheel stage reads this (was stale on the rail: -8 m/s read while still)
				}
				// Close the heading loop at 60 Hz using live body forward F so slow 2-5 Hz planning cadences
				// can never hold a stale open-loop yaw rate past railAim (eliminates Nyquist fishtail oscillation).
				float appliedSpinZ = tyaw;
				if (g_sh.railAimValid && std::isfinite(g_sh.railAim)) {
					const float* liveF = reinterpret_cast<const float*>(gb.body + 0x10);
					if (std::isfinite(liveF[0]) && std::isfinite(liveF[1]) && std::hypot(double(liveF[0]), double(liveF[1])) > 0.1) {
						const double liveH = std::atan2(double(liveF[0]), double(liveF[1]));
						const double eLive = Wrap(double(g_sh.railAim) - liveH);  // + = aim is right of nose; spin[2] + = turn left
						const double mYaw = std::clamp(double(g_sh.railMaxYaw), 0.25, 1.6);
						if ((tyaw > 0.0f && eLive > 0.02) || (tyaw < 0.0f && eLive < -0.02)) {
							appliedSpinZ = float(std::clamp(-eLive * 2.5, -mYaw, mYaw));
						} else {
							const double liveCap = std::min(mYaw, std::max(0.25, std::fabs(eLive) * 3.0 + 0.15));
							appliedSpinZ = float(std::clamp(double(tyaw), -liveCap, liveCap));
						}
					}
				}
				spin[2] = appliedSpinZ;  // pitch and roll spin stay the game's (suspension, slopes)
				const float* bp = reinterpret_cast<const float*>(gb.body + 0x30);
				g_sh.px = bp[0]; g_sh.py = bp[1]; g_sh.pz = bp[2];
				lk.unlock();
				const bool spun = RailSetSpin(gb, spin);
				lk.lock();
				if (spun) ++g_sh.railSteps; else ++g_sh.railFaults;
				// A rail step is a physics step: without this the pilot read "no steps" and stopped the car as paused.
				g_sh.lastStepAt = GetTickCount64(); g_sh.lastStepInstance = g_sh.instance.load(); ++g_sh.steps; g_sh.testSteps.fetch_add(1, std::memory_order_relaxed);
				return result;
			}
			if (!g_sh.active) {
				if (!g_sh.groundOn || !vehicle || !g_driverVehicle.load(std::memory_order_relaxed)) {
					return result;
				}
				const float gx = g_sh.px, gy = g_sh.py, gz = g_sh.pz;
				lk.unlock();
				const auto gb = FindBody(vehicle, gx, gy, gz);
				lk.lock();
				if (!gb.body || g_sh.active || !bodybinding::Matches(gb.id,g_sh.bodyId.load())) {
					return result;
				}
				float* gv = reinterpret_cast<float*>(gb.motion + 0x60);
				const double hs = std::hypot(double(gv[0]), double(gv[1]));
				if (!std::isfinite(hs)) {
					return result;
				}
				if (g_sh.groundLearn && hs < 80.0 && hs > g_sh.groundTop) {
					g_sh.groundTop = std::min(60.0, hs);
				}
				const bool paid=g_meterBoost.load() && g_driverVehicle.load()==g_sh.vehicle.load();
				const double cap = g_sh.groundFrac * g_sh.groundTop * (paid?boostlogic::kMultiplier:1.0);
				if ((g_sh.groundFrac < 1.0 || paid) && hs > cap && hs > 0.5) {
					const double k = std::max(cap / hs, 0.97);  // eased: about 3 % per physics step
					gv[0] = float(gv[0] * k);
					gv[1] = float(gv[1] * k);
				}
				return result;
			}
			// Never hold our lock across a call into the game (0.57.0-0.58.0 did, and the game froze 0.5 s after a
			// re-centre while hovering: the physics step waited on the game while the frame task waited on our lock).
			const float qx = g_sh.px, qy = g_sh.py, qz = g_sh.pz;
			const bool physicalSpin = g_sh.physical;
			lk.unlock();
			const auto b = FindBody(vehicle, qx, qy, qz);
			// Stage 0 (pilot 0): the body's spin now, after the game's step (world, read interface vt +0xA8, as FUN_1423CE0F0 reads it).
			const bool measure = &g_sh == &g_shs[0] && b.body && b.read;
			alignas(16) float wNow[4] = {};
			if (measure || (physicalSpin && b.body && b.read)) VFn<void (*)(std::uintptr_t, std::uint32_t, float*)>(b.read, 0xA8)(b.read, b.id, wNow);
			lk.lock();
			if (!g_sh.active || !bodybinding::Matches(b.id,g_sh.bodyId.load())) {
				return result;
			}
			if (b.body) g_sh.instance.store(vehicle, std::memory_order_relaxed);
			if (!b.body) {
				return result;
			}
			// Both models publish live orientation. StepPhysical returns before the stock
			// rotation code; leaving heading there froze the AI's feedback during flight.
			bodyattitude::Publish(g_sh, reinterpret_cast<const float*>(b.body),
				reinterpret_cast<const float*>(b.body + 0x10), reinterpret_cast<const float*>(b.body + 0x20));
			// Physical flight (player manual or, with its own setting, AI pilots); a faulted chassis flies the stock model.
			if (g_sh.physical&&!g_sh.physicalFault)
				return StepPhysical(g_sh,b,wNow,dt,result,lk);
			const float* R = reinterpret_cast<const float*>(b.body + 0x00);  // right
			const float* F = reinterpret_cast<const float*>(b.body + 0x10);  // forward (nose)
			const float* U = reinterpret_cast<const float*>(b.body + 0x20);  // up
			const float* P = reinterpret_cast<const float*>(b.body + 0x30);  // position
			float* lv = reinterpret_cast<float*>(b.motion + 0x60);
			double v[3] = { lv[0], lv[1], lv[2] };
			if (g_sh.autopilot) {
				if (!g_sh.haveLast) {
					v[0] = v[1] = 0.0;  // the first step starts from rest (never the motion the game left on the body)
				}
				else {
					// The parked wipe leaves the sideways speed at (almost exactly) zero: carry ours. Anything else is the
					// physics (a contact): keep it, or the rover pushes into the obstacle and is launched (13:50:34, 56 m/s).
					const double bodyH = std::hypot(v[0], v[1]), ourH = std::hypot(double(g_sh.lastV[0]), double(g_sh.lastV[1]));
					const ULONGLONG tick = GetTickCount64();
					if (ourH > 0.3 && bodyH < 0.1 * ourH && tick >= g_sh.carryBlockedUntil) {
						if (!g_sh.carryRun) {
							g_sh.carryX = P[0];
							g_sh.carryY = P[1];
							g_sh.carryT0 = tick;
						}
						++g_sh.carryRun;
						v[0] = g_sh.lastV[0];
						v[1] = g_sh.lastV[1];
						if (tick - g_sh.carryT0 >= 250) {  // judged per 250 ms: did it travel what the carried speed says?
							const double moved = std::hypot(double(P[0]) - g_sh.carryX, double(P[1]) - g_sh.carryY), expected = ourH * double(tick - g_sh.carryT0) / 1000.0;
							g_sh.carryRun = 0;
							if (moved < 0.25 * expected) {
								// Blocked (a wall, an overlap): restoring the speed every step pressed it into the obstacle forever (the "magnet").
								g_sh.carryBlockedUntil = tick + 1000;
								v[0] = bodyH > 0.0 ? v[0] : 0.0;
								v[1] = bodyH > 0.0 ? v[1] : 0.0;
								logger::Write("Rover flight: carried speed %.0f m/s is blocked (moved %.1f of %.1f m); not pushing into it", ourH, moved, expected);
							}
						}
					}
					else if (bodyH >= 0.1 * ourH) {
						g_sh.carryRun = 0;
					}
				}
			}
			// An absurd velocity from the game (0.57.0: a ground strike left 22,901 m/s on the body): keep the last good one.
			if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || std::fabs(v[0]) > 300.0 || std::fabs(v[1]) > 300.0 || std::fabs(v[2]) > 300.0) {
				for (int i = 0; i < 3; ++i) v[i] = g_sh.haveLast ? g_sh.lastV[i] : 0.0;
				++g_sh.fixedVel;
			}
			// An upward kick we did not make: the solver pushing the rover out of a floor it was driven into (2026-10-17
			// 15:08:57: 108 m/s, then 282 m/s up after a descent into a city platform). Keep our own vertical speed and end
			// the flight as a collision instead of riding the launch.
			if (g_sh.haveLast && v[2] - g_sh.lastV[2] > 12.0) {
				g_sh.collisionUp = true;
				g_sh.collided = true;
				g_sh.collisionDelta = float(v[2] - g_sh.lastV[2]);
				// Cap, never refuse: refusing the push-out kept the overlap alive, so the solver kicked again every step (22:25:56: 127-287 m/s
				// six times in four seconds) and the vehicle stayed stuck in the object ("magnetized"). 14 m/s clears an overlap in a few steps.
				v[2] = std::min(v[2], 14.0);
				++g_sh.launches;
			}
			v[2] = std::clamp(v[2], -60.0, 40.0);  // the model never climbs faster than this
			const double pitch = hoverlogic::Pitch(F[0], F[1], F[2]);   // nose up +
			const double roll = hoverlogic::Roll(R[2], U[2]);   // right side down +

			// Gravity, measured: what one cycle (solver + game step) did to the vertical speed since our last write,
			// when nothing hit us. The hover lift carries it.
			if (g_sh.haveLast) {
				const double dz = v[2] - g_sh.lastV[2];
				const double dx = v[0] - g_sh.lastV[0], dy = v[1] - g_sh.lastV[1];
				// Only clear of the ground: resting on a floor the contact cancels gravity and the estimate drained to 0.05
				// over a relaunch loop (15:09:53-57), leaving the hover with almost no lift on the next flight.
				// No longer learned here (2026-10-17 16:2x: the estimate sat at 13-15 m/s^2; the game's own vehicle step damps
				// vertical speed, which while rising reads as extra gravity -> more lift -> more climb; the rover rose to 348 m
				// with no lift input and the down key could not win). The lift integrator below carries gravity instead.
				(void)dz;
				(void)dx;
				(void)dy;
			}

			// ---- Rotation: wanted rates, spin eases toward them (momentum) ----
			const double hSpeed = std::hypot(v[0], v[1]);
			const double speedK = std::clamp(hSpeed / 15.0, 0.3, 1.0);
			// The body's heading in the camera's frame; the camera gap is a variable A / D (sin(gap / 2) shaped).
			const double bodyHeading = g_sh.bodyHeading;
			g_sh.camYaw = 0;
			if (g_sh.haveView) {
				g_sh.gap = Wrap(g_sh.view - bodyHeading);
				const double g2 = std::max(0.0, std::fabs(g_sh.gap) - kCamDeadzone);
				g_sh.camYaw = (g_sh.gap >= 0 ? 1.0 : -1.0) * kCamYawMax * std::sin(std::min(g2, 3.14159265358979) / 2.0);
			}
			const double yawLimit=g_sh.hull.YawAt(hSpeed);
			// Stage 0: what happened to the spin we wrote last step (game upright actions, air control, contacts).
			if (measure && g_sh.haveLastW && std::isfinite(wNow[0]) && std::isfinite(wNow[1]) && std::isfinite(wNow[2])) {
				const float d[3] = { wNow[0] - g_sh.lastW[0], wNow[1] - g_sh.lastW[1], wNow[2] - g_sh.lastW[2] };
				const double dF = d[0] * F[0] + d[1] * F[1] + d[2] * F[2], dR = d[0] * R[0] + d[1] * R[1] + d[2] * R[2];
				g_sh.measRollPull += -dF * (roll >= 0.0 ? 1.0 : -1.0);     // + : toward level
				g_sh.measPitchPull += -dR * (pitch >= 0.0 ? 1.0 : -1.0);
				g_sh.measMax = std::max(g_sh.measMax, std::sqrt(double(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])));
				++g_sh.measN;
			}
			// Free roll and the bank test turn about the body's own axes (freeroll.h); otherwise the stock model below.
			const bool bodyAxes = g_sh.freeRoll || g_sh.bankTest;
			double yawWant, pitchWant, rollWant;
			if (bodyAxes) {
				// The nose follows the camera aim measured in the body frame: elevation -> pitch, bearing -> yaw (no bank turn: a
				// banked pull is the turn). No camera: the aim is level ahead.
				const auto e = freeroll::AimError(R, F, U, freeroll::AimDir(g_sh.haveView ? g_sh.view : bodyHeading, g_sh.haveAim ? g_sh.pitchAim : 0.0));
				g_sh.gap = e.yaw;
				const double y2 = std::max(0.0, std::fabs(e.yaw) - kCamDeadzone);
				g_sh.camYaw = g_sh.haveView ? (e.yaw >= 0 ? 1.0 : -1.0) * kCamYawMax * std::sin(std::min(y2, 3.14159265358979) / 2.0) : 0.0;
				yawWant = std::clamp(g_sh.yawWant + g_sh.camYaw, -yawLimit, yawLimit);
				const double ep = e.pitch > 0 ? std::max(0.0, e.pitch - kCamPitchDeadzone) : std::min(0.0, e.pitch + kCamPitchDeadzone);
				pitchWant = std::clamp(kCamPitchGain * ep, -kPitchRate, kPitchRate);
				const double rate = freeroll::Rate(g_sh.hull.roll);
				const bool nearGround = g_sh.groundOk && P[2] - g_sh.groundUnder < freeroll::kGroundLevel;
				const double leanTo = std::clamp(kLean * g_sh.wYaw * speedK, -0.35, 0.35);
				rollWant = g_sh.rollAimOn ? freeroll::RollToward(roll, g_sh.rollAim, kApRollGain, rate)
				                          : freeroll::RollWant(roll, g_sh.rollIn, rate, kLevelRate, leanTo, g_sh.assist, nearGround, g_sh.landing);
				pitchWant = std::clamp(pitchWant, -g_sh.hull.pitch, g_sh.hull.pitch);
			}
			else {
				yawWant = std::clamp(g_sh.yawWant + g_sh.camYaw + kBankYaw * std::sin(roll) * speedK,-yawLimit,yawLimit);  // banked: the nose follows the curve
				// Axis signs come from the body basis; contacts must never recalibrate the controls.
				// Pitch: the nose follows the camera aim (deadzone, then a rate toward it); without a camera it levels.
				pitchWant = -pitch * kLevelRate;
				if (g_sh.haveAim) {
					double e = g_sh.pitchAim - pitch;
					e = e > 0 ? std::max(0.0, e - kCamPitchDeadzone) : std::min(0.0, e + kCamPitchDeadzone);
					pitchWant = std::clamp(kCamPitchGain * e, -kPitchRate, kPitchRate);
				}
				if ((pitch > kPitchMax && pitchWant > 0) || (pitch < -kPitchMax && pitchWant < 0)) pitchWant = 0;
				rollWant = g_sh.rollIn * kRollRate;
				if (g_sh.rollAimOn) {
					rollWant = std::clamp(kApRollGain * Wrap(g_sh.rollAim - roll), -kRollRate, kRollRate);
				}
				else if (g_sh.rollIn == 0.0) {
					const double leanTo = std::clamp(kLean * g_sh.wYaw * speedK, -0.35, 0.35);  // lean into the turn
					rollWant = std::clamp(Wrap(leanTo - roll) * kLevelRate, -kRollRate, kRollRate);
				}
				if ((roll > kRollMax && rollWant > 0) || (roll < -kRollMax && rollWant < 0)) rollWant = 0;
				pitchWant=std::clamp(pitchWant,-g_sh.hull.pitch,g_sh.hull.pitch);
				rollWant=std::clamp(rollWant,-g_sh.hull.roll,g_sh.hull.roll);
			}
			// FC-1: the spin eases toward the wanted rates at the flight gear's response (gear 3: 4 /s yaw, 5 /s pitch and roll, as before).
			g_sh.wYaw = flightgear::Ease(g_sh.wYaw, yawWant, g_sh.row.yawResponse, dt);
			g_sh.wPitch = flightgear::Ease(g_sh.wPitch, pitchWant, g_sh.row.tiltResponse, dt);
			g_sh.wRoll = flightgear::Ease(g_sh.wRoll, rollWant, g_sh.row.tiltResponse, dt);
			g_sh.trkPhysical = false; ++g_sh.trkSteps;
			g_sh.trkRateErr = std::max({ g_sh.trkRateErr, std::fabs(yawWant - g_sh.wYaw), std::fabs(pitchWant - g_sh.wPitch), std::fabs(rollWant - g_sh.wRoll) });
			// Body angular velocity: pitch about the right axis (nose up +), roll about the nose (right down +), yaw about
			// world up (stock) or the body's up (body axes); turn right = a negative turn about it.
			const double wp = g_sh.wPitch, wr = g_sh.wRoll;
			alignas(16) float w[4] = { float(R[0] * wp + F[0] * wr), float(R[1] * wp + F[1] * wr), float(R[2] * wp + F[2] * wr - g_sh.wYaw), 0.0f };
			if (bodyAxes) freeroll::Compose(R, F, U, wp, wr, g_sh.wYaw, w);
			lk.unlock();
			VFn<void (*)(std::uintptr_t, std::uint32_t, const float*, int)>(b.write, 0xB0)(b.write, b.id, w, 0);
			lk.lock();
			if (!g_sh.active || !bodybinding::Matches(b.id,g_sh.bodyId.load())) return result;
			if (measure) {
				for (int i = 0; i < 3; ++i) { g_sh.lastW[i] = w[i]; g_sh.measW[i] = w[i]; }
				g_sh.haveLastW = true;
			}

			// ---- Linear: pushes added to the body's own velocity ----
			const double fh = std::hypot(F[0], F[1]);
			const double hx = fh > 1e-4 ? F[0] / fh : 0.0, hy = fh > 1e-4 ? F[1] / fh : 1.0;  // level nose direction
			const double sx = hy, sy = -hx;                                                     // level right
			double ax = 0, ay = 0, az = 0;
			// Thrust along the 3D nose, or (avoidance stop) braking against the horizontal velocity.
			if (g_sh.braking) {
				if (hSpeed > 0.5) {
					ax -= g_sh.hull.brake * v[0] / hSpeed;
					ay -= g_sh.hull.brake * v[1] / hSpeed;
				}
			}
			else {
				ax += g_sh.thrustAcc * g_sh.hull.acceleration * F[0];
				ay += g_sh.thrustAcc * g_sh.hull.acceleration * F[1];
				az += g_sh.thrustAcc * g_sh.hull.acceleration * F[2];
			}
			// Strafe (autopilot): a push along the level right.
			if (g_sh.strafe != 0.0) {
				ax += g_sh.strafe * kStrafeAcc * sx;
				ay += g_sh.strafe * kStrafeAcc * sy;
			}
			// Path: speed across the nose bleeds off, part of it turns along the nose.
			const double along = v[0] * hx + v[1] * hy, side = v[0] * sx + v[1] * sy;
			const double leak = flightgear::SideLeak(g_sh.row, side, hSpeed);  // FC-1: the gear's side gain past its deadband (gear 3: side x 1.2)
			g_sh.trkDrift = std::max(g_sh.trkDrift, std::fabs(side)); g_sh.trkSidePush = std::max(g_sh.trkSidePush, std::fabs(leak));
			ax += -leak * sx + (along >= 0 ? 1.0 : -1.0) * std::fabs(leak) * kRedirect * hx;
			ay += -leak * sy + (along >= 0 ? 1.0 : -1.0) * std::fabs(leak) * kRedirect * hy;
			// Drag (horizontal).
			const double drag = g_sh.thrusting ? kDragThrust : kDragCoast;
			ax -= v[0] * drag;
			ay -= v[1] * drag;
			// Vertical target: hold, or pitch + lift keys.
			const double z = P[2];
			double vzT;
			bool pitchClimb = false;
			if (g_sh.vzOn) {
				vzT = std::clamp(g_sh.vz, -kLiftSpeed, kLiftSpeed);
			}
			else if (g_sh.holding) {
				vzT = std::clamp(kHoldGain * (g_sh.refGround + g_sh.hold - z), -kHoldMaxVz, kHoldMaxVz);
			}
			else {
				vzT = g_sh.lift * kLiftSpeed;
				if (std::fabs(pitch) > 0.05) {
					vzT += std::sin(pitch) * (std::max(along, 0.0) + kClimbExtra);
					pitchClimb = true;
				}
			}
			// Floor.
			const double clearance=g_sh.landing?0.15:fieldgate::FloorClearance(g_sh.autopilot,g_sh.fieldClearance,std::max(kMinClearance,std::fabs(R[2])*g_sh.hull.width+std::fabs(F[2])*g_sh.hull.length+std::fabs(U[2])*g_sh.hull.height+1.5));
			if (g_sh.groundOk && z - g_sh.groundUnder < clearance) {
				vzT = std::max(vzT, (clearance - (z - g_sh.groundUnder)) * 3.0 + 1.0);
			}
			// Hover lift: carries gravity and drives the vertical speed to its target; it acts along the rover's up axis,
			// so a tilt pushes sideways (bank: the path curves) or forward (nose down).
			// Integral of the vertical speed error: rising faster than wanted lowers it, sinking raises it, so a wrong
			// gravity can never hold the vehicle up (or down). gEst is that integral (kept as the hover's "gravity").
			g_sh.gEst = std::clamp(g_sh.gEst + kLiftIntegral * (vzT - v[2]) * dt, 0.0, 30.0);
			double lift = std::clamp(kVzResponse * (vzT - v[2]), -kMaxDown, kMaxUp) + g_sh.gEst;
			az += lift;
			const double tilt = hoverlogic::TiltLift(lift, U[2], kTiltPush);
			ax += U[0] * tilt;
			ay += U[1] * tilt;
			// Energy trade: a pitch climb costs speed along the nose, a dive gives it.
			if (pitchClimb && hSpeed > 1.0) {
				const double trade = -kTrade * g_sh.gEst * v[2] / std::max(hSpeed, 5.0);
				ax += trade * v[0] / hSpeed;
				ay += trade * v[1] / hSpeed;
			}
			v[0] += ax * dt;
			v[1] += ay * dt;
			v[2] += az * dt;
			// Speed cap (horizontal), eased.
			const double hs = std::hypot(v[0], v[1]);
			if (hs > g_sh.cap) {
				const double k = std::max(g_sh.cap / hs, 1.0 - 2.0 * dt);
				v[0] *= k;
				v[1] *= k;
			}
			lv[0] = float(v[0]);
			lv[1] = float(v[1]);
			lv[2] = float(v[2]);
			for (int i = 0; i < 3; ++i) {
				g_sh.lastV[i] = float(v[i]);
				g_sh.v[i] = float(v[i]);
			}
			g_sh.bodyPitch = float(pitch);
			g_sh.bodyRoll = float(roll);
			g_sh.haveLast = true;
			g_sh.matched = true;
			++g_sh.steps;
			++g_sh.stepSerial; g_sh.lastStepAt=GetTickCount64(); g_sh.lastStepInstance=vehicle; g_sh.testSteps.fetch_add(1,std::memory_order_relaxed);
			return result;
		}

		// ---- Ground samples -------------------------------------------------------------------------------------
		// Slots (prefetch kSlotModel0 + i): 0 centre (queried just under the rover), 1 front, 2 right, 3 left, 4..6
		// ahead along the travel direction.
		enum { kCentre = 0, kFront, kRight, kLeft, kAhead0, kSamples = kAhead0 + 3 };

		struct Sample
		{
			double x = 0, y = 0, dist = 0;
			bool valid = false;
			double h = 0;
		};

		struct State
		{
			bool active = false;
            bool landing=false;double landGround=0,landHeight=0;ULONGLONG landProofAt=0;
            // Descend before the drop (2026-10-28, the user: "all vehicles descend to a certain point before they can drop off ...
            // except for manual flying for me"): a stop asked while an autopilot-flown vehicle is high is held until it is down.
            bool settle=false, settleForce=false; double settleX=0, settleY=0, settleFrom=0; ULONGLONG settleAt=0; char settleWhy[64]={};
            bool groundHug=false;  // your empty vehicle "drives": the hover stays just over the ground and hops what a jump clears (travelmode)
			bool keyDown = false;
			double hold = 3.0;
			bool holding = false;
			double cap = kSpeedCap;
			Sample asked[kSamples];
			bool haveAsked = false;
			double ground = 0;
			bool haveGround = false;
			double candidate = 0;
			int agree = 0;
			ULONGLONG groundAt = 0;
			unsigned rejected = 0;
			int gear = 2;                  // 1-4 (shown on the HUD before hovering too)
			flightgear::Shift flightShift; // FG-1: the autopilot's flight gear (the player's flight gear is `gear`)
			int rowGear = 0, rowWriter = -1; bool rowAi = false;  // FC-1: the row last traced
			ULONGLONG trackAt = 0;         // FC-1: the last handling track line
			bool wDown = false, sDown = false, boostDown = false;
			bool wHold = false;            // double-tapped boost (2026-10-24, was W): boost stays on until boost or brake is pressed
			ULONGLONG wLastPress = 0;      // the last boost press (the double-tap window)
			bool liftoff = false;          // climbing to kLiftoffHeight after switching on near the ground
			bool autoOnWheels = false;     // HO-1: this autopilot started on the car's wheels (CW-1): when it ends the car goes back to the wheels
			ULONGLONG startedAt = 0;       // when the flight switched on (contacts are ignored for kContactGraceMs)
			ULONGLONG contactGraceUntil = 0;  // after a push up, contacts until then are its aftermath, not a crash
			unsigned contactsIgnored = 0;
			bool autopilot = false;        // called: flying to the player with nobody aboard
			bool playerAuto = false;       // Auto: the player is aboard and the autopilot flies (any W/A/S/D takes over)
			std::uint32_t holdId = 0;      // the player got out with Auto on: this vehicle holds its spot
			double holdX = 0, holdY = 0;
			double holdZ=std::numeric_limits<double>::quiet_NaN();
			bool routeHold=false;
			// Taxi test (Auto, 2026-10-17): no map marker, it hovers where Auto was switched on (or where the marker went
			// away); a marker, it flies there and holds over it.
			bool taxiWait = false;
			double taxiWaitX = 0, taxiWaitY = 0;
			std::uint32_t taxiMarker = 0;
			double taxiMx = 0, taxiMy = 0, taxiLeft = 0;
			bool taxiArrived = false;
			double taxiMz = std::numeric_limits<double>::quiet_NaN();
			FvoTripClaim taxiRoute{};      // Auto on a traffic route to the marker (the addon's trip); step null = straight
			bool cruise = false;           // Cruise: Auto with no destination, lap after lap of random traffic routes
			FvoTripClaim cruiseRoute{};    // the lap under way; step null = asking for one
			ULONGLONG cruiseAskAt = 0;
			bool cruiseToldNone = false;
			double cruiseWaitX = 0, cruiseWaitY = 0;
			bool taxiRouteAsked = false;   // the planner was asked for this marker
			TripProgress trip;
			ULONGLONG autopilotSince = 0;
			int avoid = 0;                 // 0 none, 1 climb, 2 left, 3 right, 4 stop
			ULONGLONG avoidSince = 0;
			bool haveCorr = false;
			struct { double x = 0, y = 0, d = 0; bool valid = false; double h = 0; } corr[9];  // [corridor * 3 + distance]
			// Forward rays (one per corridor, at the vehicle's height, every solid collision layer): what blocks it.
			prefetch::RayHit ray[3];
			bool rayValid[3] = {};
			double rayRange[3] = {};
			unsigned sweepPhase = 0;                     // which part of the 16-direction ring the next sweep rays look at
			bool hullValid = false, hullHit = false;     // the hull sweep along the planned heading (prefetch::LatestSweep)
			double hullDist = 0, hullRange = 0;
			ULONGLONG hullLogAt = 0;
			std::uintptr_t lastBlocker = 0;
			ULONGLONG lastBlockerAt = 0;
			ULONGLONG landedSince = 0;
			double baseZ = 0;
			LARGE_INTEGER last{};
			ULONGLONG lastLog = 0, lastHud = 0;
			// Probe results are read back a few frames later; right after a start the slots still hold the previous
			// flight's answers (2026-10-17 15:07: a relaunch read "climbed to 245 m" at once). Nothing is read until this.
			ULONGLONG freshAt = 0;
			double askZ = 0;               // the rover's height when the samples were asked
			// navigation
			double cruiseExtra = 0;        // m above the skim height
			ULONGLONG clearSince = 0;
			double castHeading = 0;
			bool haveCast = false;
			ULONGLONG reverseUntil = 0;    // backing away (the nose still turns toward the detour)
			double floorZ = -1e9;          // a floor the ground rays miss (learned from a push up)
			ULONGLONG floorUntil = 0;
			double cruiseHold = kCallSkim; // m: the terrain-ahead hold height
			double throttle = 1.0;         // 0..1 share of the gear's thrust
			bool aiGears = false;          // this frame: the AI gear tables apply
			int lastF = 0;                 // last frame's straight-ahead corridor verdict
			int lastL = 0, lastR = 0;      // the left / right corridors
			double lastNeed = 0;           // metres to climb to pass the straight-ahead corridor
			bool drive = false;            // drive mode: hover off, the game's controls
			ULONGLONG driveSince = 0, hopUntil = 0, drvLog = 0;
			double cramped = 0;            // score that switches drive mode on
			double drvX = 0, drvY = 0, drvSp = 0, drvHead = 0, steerSign = 1.0, steerResp = 0, drvSteer = 0;
			ULONGLONG drvT = 0;
			ULONGLONG airSince = 0;
			double steerSecs = 0;
			bool taxiOrbit = false;        // Auto's orbit around the marker is on (AutoMode::Orbit)
			bool ceilKnown = false;        // both ceiling probes answered freshly, including open sky
			bool ceilValid = false;        // a ceiling over the vehicle (the ceiling rays); the height hold stays kCeilClear under it
			double ceilZ = 0;
			ULONGLONG ceilLogAt = 0;
			bool wallAhead = false;        // the wall warning: a wall along the way (rays, hull sweep, steep ground samples)
			double wallDist = 1e9;         // m to it
			double wallL = 1e9, wallR = 1e9;  // m to a wall left / right (the side rays)
			ULONGLONG wallLogAt = 0;
			bool personAhead = false;      // someone on foot in the way (checked every kPersonEveryMs)
			double personDist = 1e9;
			ULONGLONG personAt = 0;
			// Another vehicle close by (2026-10-20, the user: hovering vehicles "magnetized" into each other). Vehicles are no terrain to the assist:
			// probe hits inside one are dropped and a contact with one is no floor or bump; instead this silent warning steers the autopilot.
			bool vehWarn = false;          // a vehicle within the warning range
			bool vehTouch = false;         // close enough that a contact is most likely with it
			double vehGap = 1e9;           // m between the hulls (centres minus footprint radii)
			double vehBearing = 0;         // from this vehicle to the other
			double vehClosing = 0;         // m/s the gap shrinks
			std::uint32_t vehId = 0;
			bool vehYield = true;          // this vehicle gives way to that one (right of way by rank)
			ULONGLONG vehLogAt = 0;
			unsigned vehDropped = 0;       // probe samples dropped as "a vehicle, not ground" (logged)
			drive::Output adOut;
			bool adContact = false;        // a contact this frame (side bump or another vehicle), for its stuck/bump memory
			ULONGLONG adLogAt = 0;
			drive::Phase adLogPhase = drive::Phase::Travel;
			bool adCombat = false;         // the pilot fights now: the hover keeps a low combat band (about 8 m more)
		};
		// m over the ground under which an autopilot-flown vehicle may switch its flight off (the hover floor kMinClearance is 2.5 m):
		// any higher, it descends first (Settle). Followers park only under it too.
		constexpr double kDropMaxAbove = 3.5;
		// A physics step within 1.5 s (the 500 ms of PilotPhysics flips on any frame hitch). A physical flight does not count steps this way, and a busy lock says nothing.
		bool StepsRecent(int idx, ULONGLONG now)
		{
			auto& s = g_shs[idx];
			std::unique_lock lock(s.lock, std::try_to_lock);
			if (!lock.owns_lock() || s.physical) return true;
			const auto instance = s.instance.load();
			return instance && s.lastStepInstance == instance && s.lastStepAt && now - s.lastStepAt < 1500;
		}
		// HJ-3 (2026-11-22 22:35 run: FF1014A1 carried 162 m while still Proving, then snapped 159 m back to its birth spot at the
		// hand-off): a traffic car whose census life is Born or Proving is not the autopilot's yet, so no actuator moves it. A busy
		// census lock counts as unfinished for this tick (never move on a guess).
		bool BirthUnfinished(unsigned vehicle)
		{
			std::unique_lock lk(g_ownersLock, std::try_to_lock);
			if (!lk.owns_lock()) return true;
			const auto* r = g_owners.Find(vehicle);
			return r && r->owner == planowner::Owner::Traffic && (r->life == planowner::Life::Born || r->life == planowner::Life::Proving);
		}
		// HJ-1: this pilot's physics body is in the live scene (the read interface says it is valid). Read-only, the same reads WK-1 uses.
		bool BodyInScene(int idx)
		{
			if (idx < 0 || idx >= kPilots) return false;
			const auto instance = g_shs[idx].instance.load();
			if (!instance) return false;
			__try {
				const auto b = FindBody(instance, 0, 0, 0, true);
				if (!b.body || !b.read || At<std::uint32_t>(b.body, 0x50) != b.id) return false;
				return VFn<int (*)(std::uintptr_t, std::uint32_t)>(b.read, 0x58)(b.read, b.id) != 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// A descent that does not get down in time ends anyway: 20 s plus 1 s per 5 m it started above the ground (2026-10-28 18:43
		// run: 306 m and 351 m up after the drivers were lost; a flat 25 s dropped them from about 150 m).
		constexpr ULONGLONG kSettleBaseMs = 20000;
		State g_gs[kPilots];
		// GR-1: the gear of the FVO car (any pilot slot) whose scene holds this physics body; 0: no FVO car (the game's grip stays).
		int SlotOfBody(std::uint32_t body) {
			for (int i = 0; i < kPilots; ++i) { auto& s = g_shs[i]; if (!s.vehicle.load()) continue; std::lock_guard lk(s.lock);
				if (bodybinding::Contains({ s.sceneBodies, s.sceneCount }, body)) return i; }
			return -1;
		}
		int GearOfBody(std::uint32_t body) { const int i = SlotOfBody(body); return i >= 0 ? std::clamp(g_gs[i].gear, 1, 4) : 0; }
		int GearOfSlot(int slot) { return slot >= 0 && slot < kPilots ? std::clamp(g_gs[slot].gear, 1, 4) : 0; }
		// FC-1: what the flight planner needs from the route and its own plan for FG-1 (the route's wanted speed; the committed hover
		// plan's sharpest turn). Written where the plan is submitted / followed, read by the flight tick.
		std::atomic<float> g_flightWant[kPilots] = {}, g_flightPeak[kPilots] = {};
		void NoteFlightPlan(int idx, double want, const hoverfollow::Path* path) {
			if (idx < 0 || idx >= kPilots) return;
			if (want >= 0) g_flightWant[idx].store(float(want), std::memory_order_relaxed);
			if (path) g_flightPeak[idx].store(float(flightgear::PeakTurn(*path)), std::memory_order_relaxed);
		}
		// FC-1 (plan and craft agree): the flight planner's hover turns and tops out exactly as the craft does: the hull in use
		// (hulllogic::Flight with the gear row's turn share) and the row's top. One record, no second copy of numbers.
		void FlightSpec(bicycle::Spec& spec, int idx, std::uint32_t self) {
			if (idx < 0 || idx >= kPilots) return;
			Shared& s = g_shs[idx];
			double yaw, lateral, top; int gear;
			{ std::lock_guard lk(s.lock); yaw = s.hull.yaw; lateral = s.hull.lateral; top = s.row.top; gear = s.row.gear; }
			const double frameTop = spec.hoverTop;
			spec.hoverYaw = yaw; spec.hoverLat = lateral; spec.hoverTop = std::min(spec.hoverTop, top);
			static int loggedGear[kPilots] = {}; static float loggedLat[kPilots] = {}, loggedYaw[kPilots] = {};
			if (loggedGear[idx] != gear || std::fabs(loggedLat[idx] - float(lateral)) > .05f || std::fabs(loggedYaw[idx] - float(yaw)) > .01f) {
				loggedGear[idx] = gear; loggedLat[idx] = float(lateral); loggedYaw[idx] = float(yaw);
				logger::Write("OBS system=plan.flightspec vehicle=%08X pilot=%d gear=%d hover_yaw=%.3f hover_lat=%.2f hover_top=%.1f frame_top=%.1f yaw_at_10=%.3f yaw_at_20=%.3f reason=planner_reads_the_crafts_flight_gear_row",
					self, idx, gear, yaw, lateral, spec.hoverTop, frameTop, spec.HoverYawAt(10), spec.HoverYawAt(20));
			}
		}
		world::Snapshot g_vsnap;       // this frame's vehicle list (main thread)
		bool g_vsnapOk = false;

		// Main thread, caller holds Shared::lock. Missing identity disables writes until the scene proves ownership.

		void BindBody(Shared&s,std::uintptr_t ref)
		{
			std::uint32_t ids[16]={},count=0;
			for(unsigned i=0;g_vsnapOk&&i<g_vsnap.count&&i<world::kMaxVehicles;++i)if(g_vsnap.v[i].ref==ref){
				const auto&v=g_vsnap.v[i];count=std::min(v.bodyCount,16u);std::copy(v.bodyIds,v.bodyIds+count,ids);
				s.hullBase=hulllogic::For(v.half[0],v.half[1],v.half[2]);s.hull=hulllogic::Flight(s.hullBase,s.physical,s.row.lateralScale);break;}
			// 2026-10-28 (the user: "vehicles fell from the sky after a distance"): past the loaded-area edge (~190 m) a flying
			// vehicle's bodies drop out of its 3D for a moment and come back (01:53 run: 1 -> 0 -> 1 -> 0 in 8 s). Each 0 reset the
			// binding and physics state, and the vehicle fell in every gap. A momentary 0 for the same vehicle keeps the binding.
			if(!count&&s.vehicle.load()==ref&&s.sceneCount){s.vehicle.store(ref);return;}
			if(s.vehicle.load()!=ref||s.sceneCount!=count||!std::equal(ids,ids+count,s.sceneBodies)){
				s.instance.store(0);s.bodyId.store(0);s.haveLast=false;s.matched=false;s.carryRun=0;s.physicalState={};s.physicalFault=false;
				s.guardVehicle=0;s.guardInstance=0;s.sceneCount=count;std::copy(ids,ids+count,s.sceneBodies);
				logger::Write("Hover body binding: ref %p -> %u owned bodies%s",reinterpret_cast<void*>(ref),count,count?"":" (waiting for scene identity)");
			}
			s.vehicle.store(ref);
		}

		float VehRadius(const world::VehicleInfo& v) { return std::max(v.half[0], v.half[1]) > 0.4f ? std::hypot(v.half[0], v.half[1]) : 2.5f; }
		float VehHalfZ(const world::VehicleInfo& v) { return v.half[2] > 0.3f ? v.half[2] : 1.5f; }

		// A probe point inside another vehicle's space (a vertical cylinder around it, 1 m of slack): that vehicle, not terrain or a wall.
		// ground = a vertical ground probe: only a hit above the vehicle's origin is its body (the terrain right under a parked vehicle is ground).
		bool InVehicleSpace(std::uint32_t self, double x, double y, double z, bool ground = false)
		{
			if (!g_vsnapOk) return false;
			for (std::uint32_t i = 0; i < g_vsnap.count && i < world::kMaxVehicles; ++i) {
				const auto& v = g_vsnap.v[i];
				if (!v.formId || v.formId == self) continue;
				const double r = VehRadius(v) + 1.0, hz = VehHalfZ(v);
				if (std::hypot(x - v.pos[0], y - v.pos[1]) > r) continue;
				if (z < (ground ? v.pos[2] + 0.5 : v.pos[2] - hz - 1.0) || z > v.pos[2] + 2.0 * hz + 1.0) continue;
				return true;
			}
			return false;
		}

		// ---- Follow from below (2026-10-20, the user): the player in the air (over the ground under them, the terrain assist's ground, by more than
		// kAirOn; back down under kAirOff for kAirOffMs: hysteresis) -> the following rover flies kUnderBy under the player's feet at the point where
		// their fall (their 3D velocity and the vehicle's measured gravity) reaches that height, so it can catch them; the goal moves with a
		// deadband and a low-pass so it does not wiggle. On the ground: the normal follow point.
		constexpr double kAirOn = 4.0, kAirOff = 2.0;
		constexpr ULONGLONG kAirOffMs = 400;
		constexpr double kUnderBy = 7.0;           // m under the player's feet
		constexpr double kUnderLeadMax = 2.5;      // s of fall predicted at most
		constexpr double kUnderLeadDist = 40.0;    // m: the prediction never leads farther
		constexpr double kUnderDeadband = 1.5;     // m: a smaller change of the prediction does not move the goal
		struct PlayerAir
		{
			bool air = false, groundOk = false;
			double ground = 0;
			ULONGLONG groundAt = 0, offSince = 0;
			double px = 0, py = 0, pz = 0, vx = 0, vy = 0, vz = 0;
			ULONGLONG at = 0;
			double gx = 0, gy = 0, wx = 0, wy = 0;  // the goal (smoothed) and the last prediction it moved for
			bool haveGoal = false;
			ULONGLONG goalAt = 0;
		} g_air;

		bool InVehicleSpace(std::uint32_t self, double x, double y, double z, bool ground);

		// Autopilots switched off by the hit counter (vehiclehits): vehicle form id -> refused until.
		struct DisabledAp { std::uint32_t id = 0; ULONGLONG until = 0; } g_disabled[16];
		AutoMode g_autoMode = AutoMode::Stop;
		constexpr double kOrbitRadius = 25.0;      // m: Auto's orbit around the marker

		bool IsDisabled(std::uint32_t id)
		{
			if (!id) return false;
			const ULONGLONG now = GetTickCount64();
			for (const auto& d : g_disabled) {
				if (d.id == id && now < d.until) return true;
			}
			return false;
		}

		void PlayerAirTick(std::uintptr_t player)
		{
			const ULONGLONG now = GetTickCount64();
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (g_air.at && now > g_air.at && now - g_air.at < 1000) {
				const double dt = (now - g_air.at) / 1000.0;
				const double ix = (pp.x - g_air.px) / dt, iy = (pp.y - g_air.py) / dt, iz = (pp.z - g_air.pz) / dt;
				if (std::sqrt(ix * ix + iy * iy + iz * iz) < 150.0) {
					const double a = std::min(1.0, dt * 6.0);
					g_air.vx += (ix - g_air.vx) * a;
					g_air.vy += (iy - g_air.vy) * a;
					g_air.vz += (iz - g_air.vz) * a;
				}
			}
			else {
				g_air.vx = g_air.vy = g_air.vz = 0;
			}
			g_air.px = pp.x; g_air.py = pp.y; g_air.pz = pp.z; g_air.at = now;
			float h = 0;
			unsigned age = 0;
			if (prefetch::LatestGround(prefetch::kSlotFollow, h, age) && age <= 4 && h > -1e30f && h != -500.0f && h <= pp.z + 0.5f && !InVehicleSpace(0, pp.x, pp.y, h, true)) {
				g_air.ground = h;
				g_air.groundOk = true;
				g_air.groundAt = now;
			}
			if (now - g_air.groundAt > 1500) g_air.groundOk = false;
			prefetch::RequestGround(prefetch::kSlotFollow, pp.x, pp.y, pp.z + 0.5f, 0.0f, 3000.0f);
			const bool onVehicle = InVehicleSpace(0, pp.x, pp.y, pp.z, false);  // standing on (or in) a vehicle: not in the air
			const double above = pp.z - g_air.ground;
			if (!g_air.air) {
				if (g_air.groundOk && !onVehicle && above > kAirOn) {
					g_air.air = true;
					g_air.offSince = 0;
					g_air.haveGoal = false;
					logger::Write("Rover follow: you are in the air (%.1f m over the ground, falling %.1f m/s); the rover goes under you", above, -g_air.vz);
				}
			}
			else if (onVehicle || (g_air.groundOk && above < kAirOff)) {
				if (!g_air.offSince) g_air.offSince = now;
				if (now - g_air.offSince >= kAirOffMs) {
					g_air.air = false;
					logger::Write("Rover follow: you are down (%.1f m over the ground%s); normal follow", above, onVehicle ? ", on a vehicle" : "");
				}
			}
			else {
				g_air.offSince = 0;
			}
		}

		bool g_waitDoor = false;   // the player is indoors: the following rover waits where it is (following stays on)

		// ---- Night and weather (2026-10-20, notes/VEHICLE_BEHAVIOUR_RESEARCH_20261020.md): the autopilot flies slower and higher at night (the game's
		// IsTimeSpanNightAny condition, FUN_140c03480: planet-local hour against the night time span) and in rain, snow or dust (the current weather's
		// keywords WeatherTypeRainy 5B844 / Snowy 5B843 / Dust 38C5EF through the game's own check FUN_140cfc3f0 on FUN_140cf7de0()). Read every 3 s.
		struct EnvState { bool night = false, rain = false, snow = false, dust = false; ULONGLONG at = 0; bool told = false; } g_env;
		constexpr double kNightSpeed = 0.75, kWeatherSpeed = 0.7, kEnvClimb = 2.0;

		float ConditionFloat(std::uint32_t rva)
		{
			float r = 0.0f;
			__try {
				EngineFn<char (*)(void*, void*, void*, float*)>(rva)(nullptr, nullptr, nullptr, &r);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0.0f;
			}
			return r;
		}

		// The current weather has this keyword: what the game's own GetCurrentAndLastWeatherHaveKeyword condition does (FUN_140c4b4b0): the sky
		// (DAT_1461DF6F0) +0x118 = the current weather, whose keywords FUN_1408ff500 searches (count +0x2E0, array +0x2E8). Plain reads, no engine
		// call (2026-10-20 bug hunt: the first version called FUN_140CF7DE0 with no arguments; it takes an object and an index).
		bool WeatherHas(std::uint32_t keywordId)
		{
			__try {
				const std::uintptr_t sky = Ptr(0x061DF6F0);
				const std::uintptr_t w = sky ? At<std::uintptr_t>(sky, 0x118) : 0;
				if (!w) return false;
				const std::uintptr_t kw = EngineFn<std::uintptr_t (*)(std::uint32_t)>(kLookupForm)(keywordId);
				if (!kw) return false;
				const std::uint32_t n = At<std::uint32_t>(w, 0x2E0);
				const std::uintptr_t* list = At<std::uintptr_t*>(w, 0x2E8);
				if (!list || n > 256) return false;
				for (std::uint32_t i = 0; i < n; ++i) {
					if (list[i] == kw) return true;
				}
				return false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void EnvTick()
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_env.at < 3000) return;
			g_env.at = now;
			const EnvState was = g_env;
			g_env.night = ConditionFloat(0x00C03480) > 0.5f;
			g_env.rain = WeatherHas(0x0005B844);
			g_env.snow = WeatherHas(0x0005B843);
			g_env.dust = WeatherHas(0x0038C5EF);
			if (!g_env.told || was.night != g_env.night || was.rain != g_env.rain || was.snow != g_env.snow || was.dust != g_env.dust) {
				g_env.told = true;
				logger::Write("Vehicles: conditions: %s%s%s%s", g_env.night ? "night" : "day", g_env.rain ? ", rain" : "", g_env.snow ? ", snow" : "", g_env.dust ? ", dust" : "");
			}
		}

		double EnvSpeed() { return (g_env.night ? kNightSpeed : 1.0) * ((g_env.rain || g_env.snow || g_env.dust) ? kWeatherSpeed : 1.0); }
		double EnvClimb() { return (g_env.night ? kEnvClimb : 0.0) + ((g_env.rain || g_env.snow || g_env.dust) ? kEnvClimb : 0.0); }
		// Crew (2026-10-20, the user: "if I have any followers and I press those buttons ... they should enter the vehicle and do the behaviour I
		// asked for"): Summon / Follow / the scanner marker with a follower free -> the vehicle is brought as usual, then the follower boards it
		// as its driver (npcvehicles::CrewBoard) with the order; while a follower drives it, this autopilot leaves it alone.
		npcvehicles::CrewOrder g_crewPending = npcvehicles::CrewOrder::None;
		ULONGLONG g_crewAt = 0;
		constexpr ULONGLONG kCrewWaitMs = 60000;   // the vehicle must be near within this long
		constexpr double kCrewNear = 30.0;         // m: the vehicle this close to the player: the follower boards

		// The catch point under the falling player (smoothed, with a deadband).
		void UnderGoal(double gravity, double& gx, double& gy, double& gz)
		{
			const ULONGLONG now = GetTickCount64();
			const double g = std::clamp(gravity, 1.0, 30.0);
			const double catchZ = std::max(g_air.groundOk ? g_air.ground + kTerrainClear : -1e9, g_air.pz - kUnderBy);
			const double drop = std::max(0.0, g_air.pz - catchZ);
			double t = (g_air.vz + std::sqrt(std::max(0.0, g_air.vz * g_air.vz + 2.0 * g * drop))) / g;  // when the fall reaches catchZ
			t = std::clamp(t, 0.0, kUnderLeadMax);
			double lx = g_air.vx * t, ly = g_air.vy * t;
			const double ll = std::hypot(lx, ly);
			if (ll > kUnderLeadDist) { lx *= kUnderLeadDist / ll; ly *= kUnderLeadDist / ll; }
			const double px = g_air.px + lx, py = g_air.py + ly;
			if (!g_air.haveGoal) {
				g_air.gx = g_air.wx = px;
				g_air.gy = g_air.wy = py;
				g_air.haveGoal = true;
			}
			else if (std::hypot(px - g_air.wx, py - g_air.wy) > kUnderDeadband) {
				g_air.wx = px;
				g_air.wy = py;
			}
			const double dt = g_air.goalAt && now > g_air.goalAt ? std::min(0.5, (now - g_air.goalAt) / 1000.0) : 0.0;
			const double a = std::min(1.0, dt * 4.0);
			g_air.gx += (g_air.wx - g_air.gx) * a;
			g_air.gy += (g_air.wy - g_air.gy) * a;
			g_air.goalAt = now;
			gx = g_air.gx;
			gy = g_air.gy;
			gz = g_air.pz - kUnderBy;
		}

		// Someone on foot in the way of an autopilot (2026-10-20, avoid people): within kPersonRange ahead along the travel direction, kPersonCone
		// either side, near the vehicle's height. Checked every kPersonEveryMs (the shared reference scan); the player counts too.
		constexpr double kPersonRange = 14.0, kPersonCone = 0.6;
		constexpr ULONGLONG kPersonEveryMs = 200;
		// The living actors of this frame (2026-10-29 audit): every moving pilot copied the shared reference list (up to 4000) every
		// 200 ms and, for each actor in it, asked the game whether it was dead and which vehicle it sat in: with 91 traffic pilots,
		// ~450 whole-list walks a second. Now the list of living actors (position, vehicle) is made once a frame from the same
		// references, with the same checks, and each pilot only looks through it.
		std::uint64_t g_flightFrame = 0;  // BeginFrame counts frames
		struct PersonRow { std::uintptr_t ref = 0, aboard = 0; float x = 0, y = 0, z = 0; };
		std::vector<PersonRow> g_people;
		std::uint64_t g_peopleFrame = ~0ull;
		const std::vector<PersonRow>& PeopleThisFrame()
		{
			if (g_peopleFrame == g_flightFrame) return g_people;
			g_peopleFrame = g_flightFrame;
			g_people.clear();
			const auto pl = Ptr(addr::kPlayerCharacterPtr);
			const auto pp = pl ? At<game::Vec3>(pl, off::kRefPosition) : game::Vec3{};
			for (const auto r : modscene::LoadedRefs()) {
				if (!r || !npcvehicles::LooksLikeActor(r) || npcvehicles::IsDead(r)) continue;
				const auto p = At<game::Vec3>(r, off::kRefPosition);
				// Beyond ~260 m, far cars run on FleetRailStep (not energyplan), so distant actors need no per-frame seat query.
				if (pl && std::hypot(double(p.x) - pp.x, double(p.y) - pp.y) > 260.0) continue;
				const auto aboard = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(r);
				g_people.push_back({ r, aboard, p.x, p.y, p.z });
			}
			return g_people;
		}
		// World's per-frame store (design section 2, work order step 1): every vehicle and person as a shape, made once a frame from the
		// same vehicle snapshot and person list, so the plan, combat and context read one list. Cars we fly carry their published plan and
		// real body velocity; people get a velocity from frame to frame; D1's still clock runs here once, not per asking car.
		planworld::Store g_world;
		std::unordered_map<std::uint32_t, planworld::StillClock> g_worldStill;
		std::unordered_map<std::uint32_t, planworld::ProgressClock> g_worldProgress;  // PS-1: net movement per vehicle
		std::unordered_map<std::uintptr_t, planworld::Track2> g_worldPeople;
		ULONGLONG g_worldLogAt = 0;
		const planworld::Store& WorldThisFrame(double now)
		{
			if (g_world.frame == g_flightFrame) return g_world;
			g_world.Begin(g_flightFrame, now);
			int vehicles = 0, flown = 0, people = 0;
			for (unsigned k = 0; g_vsnapOk && k < g_vsnap.count && k < world::kMaxVehicles; ++k) {
				const auto& w = g_vsnap.v[k];
				planworld::Shape s; s.source = planworld::Source::Vehicle; s.centre = { w.pos[0], w.pos[1], w.pos[2] }; s.velocity = { w.vel[0], w.vel[1], 0 };
				s.heading = w.heading; s.halfWidth = std::max(.5, double(w.half[0])); s.halfLength = std::max(1.0, double(w.half[1])); s.halfHeight = 1.2; s.owner = w.formId; s.seenAt = now;
				const double still = g_worldStill[w.formId].Step(now, std::hypot(w.vel[0], w.vel[1]));
				s.steerRound = w.who == world::VehicleInfo::Who::Parked || still >= planbranch::rules::stoppedWait || w.outOfTraffic;  // D1; C4: out of traffic per the coordinator
				{   // PS-1 (2026-11-12, ramp queue 13:10:44: FF101109 rocked for 40 s, its still clock kept resetting, the car behind waited): passable
					// means not getting anywhere. A car the core drives is judged by its own plan: under a route hold it is a queue; in recovery or
					// flagged not moving it is stuck. Any other vehicle: net movement over progressWindow under progressNet.
					auto& pc = g_worldProgress[w.formId];
					if (pc.Rebase(now, w.pos[0], w.pos[1], relocconsumers::Poll(w.formId), w.formId)) g_worldStill[w.formId].Rebase();  // a relocation jump is not progress or a stall
					else pc.Step(now, w.pos[0], w.pos[1]);
					const char* why = nullptr;
					const auto it = g_planPilot.find(w.formId);
					const bool queued = it != g_planPilot.end() && it->second >= 0 && it->second < 257 && routeguidance::frames[it->second].lane.held;
					if (queued) why = nullptr;  // under a route hold: a queue, waited behind
					else if (const double net = pc.Net(now, planbranch::rules::progressWindow); net >= 0 && net < planbranch::rules::progressNet) why = "no_net_progress";
					if (why && !s.steerRound) {
						s.steerRound = true;
						logger::Observe("world.passable", w.formId, 0x5053ull, "event=passable vehicle=%08X reason=%s note=PS-1_not_getting_anywhere_may_be_gone_round", w.formId, why);
					}
				}
				g_world.Add(s); ++vehicles;
			}
			for (const auto& r : PeopleThisFrame()) {
				if (r.aboard) continue;
				auto& t = g_worldPeople[r.ref]; t.Step(now, r.x, r.y);
				planworld::Shape s; s.source = planworld::Source::Person; s.centre = { r.x, r.y, r.z + .9 }; s.velocity = { t.vx, t.vy, 0 };
				s.halfWidth = s.halfLength = .4; s.halfHeight = .9; s.steerRound = false; s.seenAt = now;  // owner 0: a ref is not a form id, and must never be mistaken for the asking car
				g_world.Add(s); ++people;
			}
			if (g_worldPeople.size() > 4096) g_worldPeople.clear();  // ids of people gone long ago; velocity restarts from the next frame
			if (g_worldStill.size() > 4096) g_worldStill.clear();
			if (GetTickCount64() - g_worldLogAt >= 10000) { g_worldLogAt = GetTickCount64();
				logger::Write("OBS system=plan.world event=store frame=%llu vehicles=%d flown_with_plan=%d people=%d reason=one_indexed_view_a_frame_for_every_reader", g_flightFrame, vehicles, flown, people); }
			return g_world;
		}
		void PersonAhead(int idx, std::uintptr_t vehicle)
		{
			State& g = g_gs[idx];
			const ULONGLONG now = GetTickCount64();
			if (now - g.personAt < kPersonEveryMs) return;
			g.personAt = now;
			g.personAhead = false;
			g.personDist = 1e9;
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const Shared& s = g_shs[idx];
			const double vx = s.v[0], vy = s.v[1], sp = std::hypot(vx, vy);
			if (sp < 3.0) return;
			const double travel = std::atan2(vx, vy);
			for (const auto& person : PeopleThisFrame()) {
				if (person.aboard == vehicle) continue;
				const game::Vec3 p{ person.x, person.y, person.z };
				if (std::fabs(double(p.z) - vp.z) > 5.0) continue;
				const double dx = double(p.x) - vp.x, dy = double(p.y) - vp.y, d = std::hypot(dx, dy);
				if (d > kPersonRange || d < 0.5) continue;
				if (std::fabs(Wrap(std::atan2(dx, dy) - travel)) > kPersonCone) continue;
				if (d < g.personDist) { g.personDist = d; g.personAhead = true; }
			}
		}

		std::uint32_t SelfId(int idx)
		{
			const std::uintptr_t v = g_hoverVehicles[idx].load();
			return v ? At<std::uint32_t>(v, off::kFormId) : 0;
		}

		// Right of way (2026-10-20, the user): the lower rank gives way, equal ranks both do. 4 parked (cannot move), 3 the player's vehicle or the one
		// they drive, 2 a follower's (escort), 1 any other NPC's, 0 a hostile's (pursuit).
		// Right of way (trafficrank.h), separate from RankOf's targeting order. Reads only the mod's own job list.
		trafficrank::Facts RightOfWay(std::uint32_t formId,bool player,bool parked,int navPriority)
		{
			trafficrank::Facts f;f.player=player||formId==OwnRoverId();f.parked=parked;f.navPriority=navPriority;
			static npcvehicles::JobInfo jobs[fvolimits::kVehicles];
			static int n = 0;
			static std::uint64_t jobsFrame = ~0ull;
			if (jobsFrame != g_flightFrame) { jobsFrame = g_flightFrame; n = npcvehicles::Jobs(jobs, fvolimits::kVehicles); }
			for (int i = 0; i < n; ++i) if (jobs[i].vehicle == formId) { f.pursuit = jobs[i].pursuit; f.following = jobs[i].following && !jobs[i].pursuit; break; }
			return f;
		}
		int RankOf(const world::VehicleInfo& v)
		{
			if (v.who == world::VehicleInfo::Who::Player || v.formId == OwnRoverId()) return 3;
			// The job list once a frame (2026-10-29 audit: it was rebuilt for every vehicle every pilot compared, and capped at 80 jobs).
			static npcvehicles::JobInfo jobs[fvolimits::kVehicles];
			static int n = 0;
			static std::uint64_t jobsFrame = ~0ull;
			if (jobsFrame != g_flightFrame) { jobsFrame = g_flightFrame; n = npcvehicles::Jobs(jobs, fvolimits::kVehicles); }
			for (int i = 0; i < n; ++i) {
				if (jobs[i].vehicle == v.formId) return jobs[i].pursuit ? 0 : 2;
			}
			if (v.who == world::VehicleInfo::Who::Npc) return 1;
			return 4;
		}

		// The nearest other vehicle: the warning the autopilot steers by (never a notification: the log only, verbose).
		void VehicleWarning(int idx, std::uintptr_t vehicle, const char* who)
		{
			State& g = g_gs[idx];
			g.vehWarn = g.vehTouch = false;
			g.vehGap = 1e9;
			g.vehClosing = 0;
			g.vehId = 0;
			if (!g_vsnapOk || !vehicle) return;
			const std::uint32_t self = At<std::uint32_t>(vehicle, off::kFormId);
			const world::VehicleInfo* me = nullptr;
			for (std::uint32_t i = 0; i < g_vsnap.count && i < world::kMaxVehicles; ++i) {
				if (g_vsnap.v[i].formId == self) me = &g_vsnap.v[i];
			}
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const double myR = me ? VehRadius(*me) : 2.5, myVx = me ? me->vel[0] : 0.0, myVy = me ? me->vel[1] : 0.0;
			const double sp = std::hypot(myVx, myVy);
			for (std::uint32_t i = 0; i < g_vsnap.count && i < world::kMaxVehicles; ++i) {
				const auto& v = g_vsnap.v[i];
				if (!v.formId || v.formId == self) continue;
				const double dx = v.pos[0] - pos.x, dy = v.pos[1] - pos.y, dz = v.pos[2] - pos.z;
				if (std::fabs(dz) > 2.0 * (VehHalfZ(v) + 2.0)) continue;  // well above or below: no conflict
				const double d = std::hypot(dx, dy);
				const double gap = d - myR - VehRadius(v);
				if (gap >= g.vehGap) continue;
				g.vehGap = gap;
				g.vehBearing = std::atan2(dx, dy);
				g.vehClosing = d > 0.1 ? -((v.vel[0] - myVx) * dx + (v.vel[1] - myVy) * dy) / d : 0.0;
				g.vehId = v.formId;
			}
			if (!g.vehId) return;
			{
				int myRank = idx == 0 ? 3 : 1;
				if (me) myRank = std::min(3, RankOf(*me));  // flying: never "parked"
				int other = 4;
				for (std::uint32_t i = 0; i < g_vsnap.count && i < world::kMaxVehicles; ++i) {
					if (g_vsnap.v[i].formId == g.vehId) other = RankOf(g_vsnap.v[i]);
				}
				// 2026-10-24: equal ranks both gave way, so two NPC vehicles side by side in bays waited for each other forever.
				// Equal ranks: only the higher form id gives way.
				g.vehYield = myRank < other || (myRank == other && self > g.vehId);
			}
			g.vehWarn = g.vehGap < 6.0 + sp * 1.2;
			g.vehTouch = g.vehGap < 2.5;
			const ULONGLONG now = GetTickCount64();
			if (g.vehWarn && logger::Verbose() && now - g.vehLogAt >= 2000) {
				g.vehLogAt = now;
				logger::Write("%s: vehicle warning: %08X %.1f m (hull gap), closing %.1f m/s%s, %s; %u probe samples on vehicles dropped", who, g.vehId, g.vehGap, g.vehClosing,
					g.vehTouch ? ", touching range" : "", g.vehYield ? "giving way" : "it gives way", g.vehDropped);
			}
		}
		// 2026-10-24: a near vehicle only stops this one when it is in the way: within 60 degrees of the direction of travel
		// (the nose when slow), or closing in. A parked neighbour 2 m to the side used to stop a departing vehicle forever.
		bool TrafficInWay(const State& g, double sp, const double v[3], double heading)
		{
			const double travel = sp > 1.0 ? std::atan2(v[0], v[1]) : heading;
			return std::cos(Wrap(g.vehBearing - travel)) > 0.5 || g.vehClosing > 0.5;
		}
		// Kept across relaunches: a pilot stuck again soon after climbing out is moved instead.
		struct Escape { int count = 0; ULONGLONG at = 0; } g_escape[kPilots];
		struct Bumps { int count = 0; ULONGLONG at = 0; ULONGLONG ev = 0; } g_bumps[kPilots];
		struct HeldCommand { Command c; ULONGLONG until = 0; } g_cmds[kPilots];
		// Passenger control while hovering (PlayerHands): the pilot the player flies by hand, its gear, until when.
		std::atomic<int> g_hands{-1}, g_handsGear{2};
		std::atomic<ULONGLONG> g_handsUntil{0};
		bool HandsOn(int idx) { return idx > 0 && g_hands.load() == idx && GetTickCount64() < g_handsUntil.load(); }

		// The player's trail (ring buffer, by sequence number) and smoothed velocity, updated every frame.
		struct TrailPoint { double x = 0, y = 0; };
		TrailPoint g_trail[kTrailMax];
		unsigned long long g_trailNext = 0;  // sequence number of the next point
		unsigned g_trailCount = 0;
		double g_pvx = 0, g_pvy = 0, g_plx = 0, g_ply = 0;
		ULONGLONG g_plt = 0;

		void TrailClear()
		{
			g_trailCount = 0;
			g_pvx = g_pvy = 0;
			g_plt = 0;
		}

		bool TrailHas(unsigned long long seq) { return g_trailCount && seq < g_trailNext && seq + g_trailCount >= g_trailNext; }
		const TrailPoint& TrailAt(unsigned long long seq) { return g_trail[seq % kTrailMax]; }

		void TrailTick(std::uintptr_t player)
		{
			const auto pcell = At<std::uintptr_t>(player, off::kRefParentCell);
			if (!pcell || !At<std::uintptr_t>(pcell, off::kCellWorldspace)) {
				TrailClear();  // indoors: a new trail outside
				return;
			}
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const ULONGLONG now = GetTickCount64();
			if (g_plt && now > g_plt) {
				const double dt = (now - g_plt) / 1000.0;
				const double ix = (pp.x - g_plx) / dt, iy = (pp.y - g_ply) / dt;
				if (dt < 1.0 && std::hypot(ix, iy) < 250.0) {
					const double a = std::min(1.0, dt * 3.0);
					g_pvx += (ix - g_pvx) * a;
					g_pvy += (iy - g_pvy) * a;
				}
				else {
					TrailClear();  // a jump (fast travel, a move): the old trail leads nowhere
				}
			}
			g_plx = pp.x; g_ply = pp.y; g_plt = now;
			if (g_trailCount) {
				const auto& last = TrailAt(g_trailNext - 1);
				if (std::hypot(pp.x - last.x, pp.y - last.y) < kTrailStep) return;
			}
			g_trail[g_trailNext % kTrailMax] = { pp.x, pp.y };
			++g_trailNext;
			g_trailCount = std::min<unsigned>(g_trailCount + 1, kTrailMax);
		}

		// The trail point nearest (x, y), searched from the oldest kept one.
		unsigned long long TrailNearest(double x, double y)
		{
			unsigned long long best = g_trailNext - 1;
			double bd = 1e18;
			for (unsigned long long q = g_trailNext - g_trailCount; q < g_trailNext; ++q) {
				const auto& t = TrailAt(q);
				const double d = (t.x - x) * (t.x - x) + (t.y - y) * (t.y - y);
				if (d < bd) { bd = d; best = q; }
			}
			return best;
		}
		State& g = g_gs[0];
		char g_endWhy[kPilots][96] = {};  // why each pilot last stopped

		// Probe slots: pilot 0 keeps its original ones; NPC pilots have their own block.
		int ModelSlot(int idx, int i) { return idx ? prefetch::kSlotPilot0 + (idx - 1) * prefetch::kSlotPilotEach + i : prefetch::kSlotModel0 + i; }
		int CorridorSlot(int idx, int i) { return idx ? prefetch::kSlotPilot0 + (idx - 1) * prefetch::kSlotPilotEach + 7 + i : prefetch::kSlotCheck0 + i; }

		// Where an autopilot flies: the shared goal type (vcontract.h): a point, the stop radius, hold / keep-going / cruise, nav settings.
		using Goal = vcontract::Goal;

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}
		bool Held(int vk) { return vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }

		// Keys: free in the game's "Vehicle" input context (checked against its ControlMap).
		struct Keys
		{
			bool chosen = false;
			int toggle = 0, rollLeft = 0, rollRight = 0;
			int up = 0, down = 0;
			int call = 0;                  // N: summon / recall (on foot)
			int follow = 0;                // U: cover on / off (on foot)
			int stance = 0;                // Defend / Attack / Hold fire for your vehicle and your escorts (on foot and driving)
			int bankTest = 0;              // , : the stage 0 bank test (only with the detailed log on)
		} g_keys;

		struct Tap
		{
			bool down = false;
			ULONGLONG lastPress = 0;
			bool Update(bool held)
			{
				const bool press = held && !down;
				down = held;
				if (!press) {
					return false;
				}
				const ULONGLONG now = GetTickCount64();
				if (lastPress && now - lastPress <= kDoubleTapMs) {
					lastPress = 0;
					return true;
				}
				lastPress = now;
				return false;
			}
		};
		Tap g_tapBoost, g_tapThrust;
		altcontrol::Input g_altInput;
		bool g_altDrop = false;

		// The keys bound to the game's own Vehicle events (followed when the player rebinds them; WASD by default).
		struct MoveKeys
		{
			int accel = 'W', brake = 'S', left = 'A', right = 'D', boost = VK_LSHIFT, vert = VK_SPACE;
			ULONGLONG at = 0;
		} g_mk;

		void ReadMoveKeys()
		{
			const ULONGLONG now = GetTickCount64();
			if (g_mk.at && now - g_mk.at < 1000) return;
			g_mk.at = now;
			g_mk.accel = controlmap::KeyFor("Vehicle", "VehicleAccel", 'W');
			g_mk.brake = controlmap::KeyFor("Vehicle", "VehicleBrake", 'S');
			g_mk.left = controlmap::KeyFor("Vehicle", "VehicleTurnLeft", 'A');
			g_mk.right = controlmap::KeyFor("Vehicle", "VehicleTurnRight", 'D');
			g_mk.boost = controlmap::KeyFor("Vehicle", "VehicleBoost", VK_LSHIFT);
			g_mk.vert = controlmap::KeyFor("Vehicle", "VehicleVertBoost", VK_SPACE);
			hoverui::SetKeyNames(bindings::Label(bindings::Action::Accelerate, g_mk.accel).c_str(), bindings::Label(bindings::Action::Brake, g_mk.brake).c_str());
		}

		void ChooseKeys()
		{
			if (g_keys.chosen) {
				return;
			}
            g_keys.toggle='V';g_keys.rollLeft='A';g_keys.rollRight='D';g_keys.up=VK_SPACE;g_keys.down='C';
			g_keys.call = 'N';
			g_keys.follow = 'U';
			for (const int vk : { g_keys.call, g_keys.follow })
				if (!controlmap::KeyFree(vk)) logger::Write("Rover keys: %s is also bound by the game (%s); kept as the user chose", controlmap::KeyName(vk).c_str(), controlmap::WhoUses(vk).c_str());
			logger::Write("Rover summon / recall key: %s, cover key: %s", controlmap::KeyName(g_keys.call).c_str(), controlmap::KeyName(g_keys.follow).c_str());

			g_keys.chosen = true;
			controlmap::Note("flight", "hover toggle", g_keys.toggle);
			controlmap::Note("flight", "roll left", g_keys.rollLeft);
			controlmap::Note("flight", "roll right", g_keys.rollRight);
			controlmap::Note("flight", "up", g_keys.up);
			controlmap::Note("flight", "down", g_keys.down);
			controlmap::Note("flight", "call", g_keys.call);
			controlmap::Note("flight", "follow", g_keys.follow);
			// Stance: one free key besides the ones above, never a number key nor the test keys , . / ' (the user's rule).
            g_keys.stance='K';
			logger::Write("Rover stance key: %s", g_keys.stance ? controlmap::KeyName(g_keys.stance).c_str() : "NONE FREE");
			// The bank test is a test key (the user's rule: , . / ' only).
			g_keys.bankTest = controlmap::Claim("flight", "bank test (detailed log)", VK_OEM_COMMA) ? VK_OEM_COMMA : 0;
			logger::Write("Rover flight keys: toggle %s, roll %s / %s, up %s / down %s; mouse aims (yaw + pitch), W/S throttle, Shift boost; extra mouse buttons unused",
				g_keys.toggle ? controlmap::KeyName(g_keys.toggle).c_str() : "NONE FREE", g_keys.rollLeft ? controlmap::KeyName(g_keys.rollLeft).c_str() : "none",
				g_keys.rollRight ? controlmap::KeyName(g_keys.rollRight).c_str() : "none", g_keys.up ? controlmap::KeyName(g_keys.up).c_str() : "none",
				g_keys.down ? controlmap::KeyName(g_keys.down).c_str() : "none");
		}

		std::uintptr_t PlayerVehicle(std::uintptr_t player)
		{
			return EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player);
		}
		// The vehicle the player DRIVES: the one the game says they are in, unless they ride it as a passenger (2026-10-28, the
		// user: "I still had control" in the passenger seat: these sites took the passenger for the driver, stopped the NPC pilot
		// with "the player took the vehicle" and sent the player's keys to it).
		std::uintptr_t DrivenBy(std::uintptr_t player)
		{
			const auto v = player ? PlayerVehicle(player) : 0;
			return v && passenger::RidingVehicle() == At<std::uint32_t>(v, off::kFormId) ? 0 : v;
		}
		bool ReadWheels(std::uintptr_t inst, struct WheelRead& out);
		bool WheelSlip(std::uintptr_t vehicle, float& maxSlip);
// TD-1 test drive REMOVED 2026-11-19 (user: "this feels like a waste"; its key took "." during Cruise tests): backup notes/planfix-20261119/before.
				bool AxesAndVelocity(std::uintptr_t body, std::uintptr_t motion, float* f, float* u, float* v);
		// ---- The player's rover, remembered by form id (resolved through the form map, never a stale pointer) ----
		FvoRoutePlanner g_routePlanner = nullptr;  // the traffic addon (FVO_RegisterRoutePlanner)
		FvoCruisePlanner g_cruisePlanner = nullptr;  // the traffic addon (FVO_RegisterCruisePlanner)
		std::uint32_t g_roverId = 0;
		std::atomic<bool> g_scanCall{ false }, g_goOnBoard{ false };  // the scanner marker button: summon, then Auto on boarding
		std::atomic<ULONGLONG> g_goAt{ 0 };
		bool g_remote = false;                                // HOOK 2b: the autopilot is running a vcontract profile for the player's EMPTY vehicle
		const vcontract::Profile* g_remoteDone = nullptr;     // the profile that finished / parked; not restarted until the assignment changes
		bool g_follow = false;            // Cover (U) of the empty vehicle: it trails and defends the player (outlives hover starts / stops)
		bool g_guard = false;             // Cover's guard point (T while covering): the empty vehicle holds g.holdX/Y
		ULONGLONG g_followStill = 0;      // since when the player has not moved
		ULONGLONG g_relaunchAt = 0;       // follow waits until this after a collision ended the flight (15:07: ten relaunches a second)

		std::uintptr_t Rover()
		{
			if (!g_roverId) return 0;
			std::uintptr_t r = 0;
			__try {
				r = EngineFn<std::uintptr_t (*)(std::uint32_t)>(kLookupForm)(g_roverId);
				if (r && At<std::uint32_t>(r, 0x28) != g_roverId) r = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				r = 0;
			}
			return r;
		}

		// The owned rover without having driven it: the game's vehicle links (FUN_140517D60(extra data at ref+0xC8,
		// keyword, 1) -> linked ref, as the NPC vehicle code FUN_141956A70 uses). Keywords are default objects (+0x48):
		// VehicleLinkedRefKeyword_DO (DAT_145E89218) and SpaceshipPartLinkedVehicleKeyword_DO (DAT_145E9E700).
		constexpr std::uint32_t kGetLinkedRef = 0x00517D60, kKwVehicleLinked = 0x05E89260, kKwShipPartVehicle = 0x05E9E748;
		// The player's own vehicle: the game keeps it as the player's linked ref with the keyword at DAT_145F042E8
		// (FUN_14219ED60 reads it before spawning the ship's vehicle; FUN_14219F800 clears it with SetLinkedRef(player,
		// 0, keyword) when it despawns it).
		constexpr std::uint32_t kKwPlayerVehicle = 0x05F042E8;

		std::uintptr_t LinkedRef(std::uintptr_t ref, std::uint32_t kwRva)
		{
			if (!ref) return 0;
			const auto kw = *reinterpret_cast<std::uintptr_t*>(game::Base() + kwRva);
			if (!kw || At<std::uint8_t>(kw, 0x2E) != 4) return 0;  // a keyword form
			const auto extra = At<std::uintptr_t>(ref, 0xC8);
			if (!extra) return 0;
			__try {
				return EngineFn<std::uintptr_t (*)(std::uintptr_t, std::uintptr_t, char)>(kGetLinkedRef)(extra, kw, 1);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		std::uint32_t FindOwnedRover(std::uintptr_t player, bool quiet)
		{
			struct Try { const char* what; std::uintptr_t from; std::uint32_t kw; };
			std::uintptr_t ship = 0;
			__try {
				EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				ship = 0;
			}
			const Try tries[4] = { { "player's own vehicle link", player, kKwPlayerVehicle }, { "player vehicle link", player, kKwVehicleLinked },
				{ "home ship vehicle link", ship, kKwVehicleLinked }, { "home ship part vehicle link", ship, kKwShipPartVehicle } };
			std::uint32_t found = 0;
			for (const auto& t : tries) {
				const auto v = LinkedRef(t.from, t.kw);
				if (v) {
					found = At<std::uint32_t>(v, 0x28);
					if (!quiet) logger::Write("Rover call: owned rover %08X found through the %s", found, t.what);
					break;
				}
			}
			if (!found && !quiet) logger::Write("Rover call: no owned rover found through the vehicle links (home ship %p)", reinterpret_cast<void*>(ship));
			if (ship) EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
			return found;
		}

		std::uintptr_t Worldspace(std::uintptr_t ref)
		{
			const auto cell = At<std::uintptr_t>(ref, off::kRefParentCell);
			return cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
		}

		// A move in progress: a ground ray was asked at the target; the move happens when it answers (a few frames).
		struct CallMove
		{
			bool pending = false;
			double x = 0, y = 0, z = 0, yaw = 0;
			ULONGLONG asked = 0;
			bool summonPoint = false;  // to the area's summon point, not next to the player
			int tries = 0;             // spots already found unsafe (FinishMove tries others round the player)
			bool air = false;          // AS-1: no safe ground spot: placing it in the air, out of the camera's view, then follow
			int airTry = 0;            // AS-1: air spots tried
		} g_move;
		// AS-1 (2026-11-07, user: "spawn it in the air away from where my camera is so I dont see it and then activate follow"): rules.
		constexpr double kAirSpawnDist = 45.0;   // m from the player
		constexpr double kAirSpawnUp = 25.0;     // m over the measured ground there (or over the player, whichever is higher)
		constexpr double kAirSpawnMinFromPlayer = 35.0;
		constexpr int kAirSpawnTries = 8;        // spots round the player, the first straight behind the camera
		constexpr double kAirSpawnAheadS = 0.5;  // must stay out of the camera's view this long (camera turn extrapolated)
		ULONGLONG g_movedAt = 0;  // the last summon move (the HUD says "MOVED TO YOU" for a few seconds)
		bool g_movedToPoint = false;  // ... or "AT SUMMON POINT"

		std::uintptr_t RefById(std::uint32_t id);

		// The player's custom map marker (form id at player +0xE40, placed by SetCustomMarker) as a point in the
		// vehicle's world: none, not loaded, another worldspace or absurdly far counts as no marker.
		constexpr double kTaxiStop = 6.0;          // m: the taxi holds this close over the marker
		constexpr double kTaxiMaxRange = 30000.0;  // m
		// player +0xE40 is a reference HANDLE, not a form id (2026-10-17 21:21: "marker id 00107C37 does not resolve to a
		// loaded reference" with the form-id lookup; the game reads it with FUN_1428CCC40(handle table, handle, functor)
		// and clears it to the invalid handle, FUN_141A43760). Returns the reference with one reference held (release it).
		std::uintptr_t MarkerFromHandle(std::uint32_t handle)
		{
			std::uintptr_t ref = 0;
			std::uintptr_t* out = &ref;
			struct
			{
				std::uintptr_t vtbl;
				std::uintptr_t** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			__try {
				EngineFn<void (*)(std::uintptr_t, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				ref = 0;
			}
			return ref;
		}

		targetsession::Session g_targetSession;
		bool TaxiMarker(std::uintptr_t player, std::uintptr_t vehicle, std::uint32_t& id, double& x, double& y, double* z = nullptr)
		{
			if (z) *z = std::numeric_limits<double>::quiet_NaN();
			id = At<std::uint32_t>(player, off::kPlayerCustomMarker);
			// 21:03-21:05: Auto waited with no "marker" line at all. Every 3 s while waiting: what the read sees.
			static ULONGLONG said = 0;
			const ULONGLONG now = GetTickCount64();
			const bool say = now - said >= 3000 && logger::Verbose();  // every 3 s while waiting: verbose logging only
			if (say) said = now;
			if (!id) {
				if (say) logger::Write("Rover taxi: waiting; no custom marker (player +0xE40 is 0)");
				return false;
			}
			const std::uintptr_t m = MarkerFromHandle(id);
			if (!m) {
				if (say) logger::Write("Rover taxi: waiting; marker handle %08X does not resolve to a reference", id);
				return false;
			}
			const auto mc = At<std::uintptr_t>(m, off::kRefParentCell), vc = At<std::uintptr_t>(vehicle, off::kRefParentCell);
			const auto mws = mc ? At<std::uintptr_t>(mc, off::kCellWorldspace) : 0, vws = vc ? At<std::uintptr_t>(vc, off::kCellWorldspace) : 0;
			const auto mp = At<game::Vec3>(m, off::kRefPosition), vp = At<game::Vec3>(vehicle, off::kRefPosition);
			EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(m);
			static std::uint32_t loggedHandle = 0;
			if (loggedHandle != id) {
				loggedHandle = id;
				logger::Write("Rover taxi: marker handle %08X -> reference at (%.0f, %.0f, %.0f), %.0f m from the vehicle", id, mp.x, mp.y, mp.z,
					std::hypot(double(mp.x) - vp.x, double(mp.y) - vp.y));
			}
			if (mws && vws && mws != vws) {
				if (say) logger::Write("Rover taxi: waiting; marker %08X at (%.0f, %.0f, %.0f) is in another worldspace (%p vs %p)", id, mp.x, mp.y, mp.z,
					reinterpret_cast<void*>(mws), reinterpret_cast<void*>(vws));
				return false;
			}
			if(!g_targetSession.Accept({id,mp.x,mp.y,mp.z}))return false;
			x = mp.x;
			y = mp.y;
			if (z && std::isfinite(mp.z)) {
				const auto at = navmap::At(x, y, mp.z + 4.0);
				if (at.known) {
					double surface = mp.z;
					if (at.haveGround) surface = std::max(surface, double(at.ground));
					// Keep embedded markers above the first mapped open height.
					for (int n=0; n<50; ++n) {
						const auto open=navmap::At(x,y,surface+4.0);
						if (!open.blocked && !open.objBlocked) { *z=surface+4.0; break; }
						surface+=4.0;
					}
				}
			}
			const double d = std::hypot(x - vp.x, y - vp.y);
			if (d >= kTaxiMaxRange) {
				if (say) logger::Write("Rover taxi: waiting; marker %08X at (%.0f, %.0f) is %.0f m away (over %.0f m)", id, mp.x, mp.y, d, kTaxiMaxRange);
				return false;
			}
			return true;
		}

		void AskMove(std::uintptr_t player, bool summonPoint = false)
		{
			if(vehiclehits::Wrecked(g_roverId)||IsDisabled(g_roverId))return;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			// The area's summon point (placement mode, 2026-10-24, the user): a far or unloaded vehicle appears there instead.
			// Only a summon uses it; follow keeps bringing the vehicle to the player.
			double sx, sy, sz, syaw;
			if (summonPoint && realvehicles::SummonPointNear(pp.x, pp.y, sx, sy, sz, syaw)) {
				g_move = { true, sx, sy, sz, syaw, GetTickCount64(), true };
				prefetch::RequestGround(prefetch::kSlotRescue0, float(sx), float(sy), float(sz) + 60.0f, 0.0f, 3000.0f);
				logger::Write("Rover call: moving it to this area's summon point (%.1f, %.1f, %.1f), %.0f m from the player", sx, sy, sz,
					std::hypot(sx - pp.x, sy - pp.y));
				return;
			}
			const double yaw = At<game::Vec3>(player, off::kRefRotation).z;
			// 12 m behind the player and 3 m to the right (out of view), facing the way the player faces.
			const double fx = std::sin(yaw), fy = std::cos(yaw), rx = std::cos(yaw), ry = -std::sin(yaw);
			g_move = { true, pp.x + rx * 3.0 - fx * 12.0, pp.y + ry * 3.0 - fy * 12.0, pp.z, yaw, GetTickCount64() };
			prefetch::RequestGround(prefetch::kSlotRescue0, float(g_move.x), float(g_move.y), pp.z + 60.0f, 0.0f, 3000.0f);  // 400 m missed the ground under a player high in the air
		}

		// AS-1: the next air spot round the player (the first straight behind the camera, then alternating 45 degrees either side),
		// with a long ground ray asked there so the height is over the real ground. false: every spot tried.
		bool AirSpot(std::uintptr_t player)
		{
			if (g_move.airTry >= kAirSpawnTries) return false;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			double cam = At<game::Vec3>(player, off::kRefRotation).z; CameraHeading(cam);
			const int k = g_move.airTry++;
			const double a = cam + 3.14159265358979 + (k % 2 ? 1.0 : -1.0) * ((k + 1) / 2) * 0.785398163;
			g_move.x = pp.x + std::sin(a) * kAirSpawnDist; g_move.y = pp.y + std::cos(a) * kAirSpawnDist; g_move.z = pp.z; g_move.yaw = a + 3.14159265358979;  // facing the player
			g_move.asked = GetTickCount64();
			prefetch::RequestGround(prefetch::kSlotRescue0, float(g_move.x), float(g_move.y), pp.z + 80.0f, 0.0f, 3000.0f);
			return true;
		}
		// AS-1: place the vehicle at the air spot once its ground ray answered, only where the camera does not see it now or in the next
		// kAirSpawnAheadS (vehiclekeep::CameraAhead, code 0) and the map knows no solid there; then follow (cover) brings it in by air.
		void FinishAirMove(std::uintptr_t player)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (GetTickCount64() - g_move.asked < 150) return;  // the ray just asked: an older answer for the last spot may still be the latest
			float ground = 0; unsigned age = 0; double base = pp.z; const char* groundFrom = "player_height";
			if (prefetch::LatestGround(prefetch::kSlotRescue0, ground, age) && age < 120 && ground > -1.0e30f && std::isfinite(ground)) { base = std::max(base, double(ground)); groundFrom = "ground_ray"; }
			else if (GetTickCount64() - g_move.asked < 6000) return;
			else { const auto s = navmap::At(g_move.x, g_move.y, pp.z); if (s.haveGround) { base = std::max(base, double(s.ground)); groundFrom = "nav_map"; } }
			const double z = base + kAirSpawnUp;
			const double fromPlayer = std::hypot(g_move.x - pp.x, g_move.y - pp.y);
			double turn = 0; const int seen = vehiclekeep::CameraAhead(g_move.x, g_move.y, z, kAirSpawnAheadS, &turn);
			bool solid = false;
			for (const double dz : { -2.0, 0.0, 2.0 }) for (const double off2 : { 0.0, 3.0 }) for (const double side : { -1.0, 1.0 }) {
				const auto c = navmap::At(g_move.x + side * off2, g_move.y + side * off2 * 0.5, z + dz); if (c.blocked || c.objBlocked) solid = true; }
			const char* why = seen != 0 ? cameraahead::Name(seen) : solid ? "solid_at_the_spot" : fromPlayer < kAirSpawnMinFromPlayer ? "too_close_to_the_player" : nullptr;
			if (why) {
				logger::Write("OBS system=summon.air vehicle=%08X event=spot_refused try=%d at=%.1f,%.1f,%.1f reason=%s camera_turn_deg_s=%.0f ground_from=%s",g_roverId,g_move.airTry,g_move.x,g_move.y,z,why,turn,groundFrom);
				if (!AirSpot(player)) { g_move.pending = false; notify::Show("Summon: no safe pickup site"); logger::Write("OBS system=summon.air vehicle=%08X event=refused reason=every_air_spot_seen_or_solid tries=%d",g_roverId,g_move.airTry); }
				return;
			}
			g_move.pending = false;
			const auto rover = Rover();
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			if (!rover || !cell || !ws) { logger::Write("OBS system=summon.air vehicle=%08X event=cancelled reason=%s",g_roverId,rover?"player_not_outdoors":"rover_not_found"); return; }
			float pos[3] = { float(g_move.x), float(g_move.y), float(z) };
			float rot[3] = { 0.0f, 0.0f, float(g_move.yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try { EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(rover, &handle, cell, ws, pos, rot); }
			__except (EXCEPTION_EXECUTE_HANDLER) { logger::Write("OBS system=summon.air vehicle=%08X event=failed reason=move_raised_an_exception",g_roverId); return; }
			// Follow (the same as pressing it): the follow loop starts the autopilot at once (the vehicle is over kFollowRelaunch away).
			g_follow = true; g_guard = false; g_followStill = 0; g_relaunchAt = 0;
			g_movedToPoint = false; g_movedAt = GetTickCount64();
			logger::Write("OBS system=summon.air vehicle=%08X event=placed try=%d at=%.1f,%.1f,%.1f over=%s %.1f from_player_m=%.0f camera=out_now_and_ahead follow=on reason=no_safe_ground_spot",
				g_roverId,g_move.airTry,pos[0],pos[1],pos[2],groundFrom,base,fromPlayer);
		}

		void FinishMove(std::uintptr_t player)
		{
			if (!g_move.pending) return;
			if(vehiclehits::Wrecked(g_roverId)||IsDisabled(g_roverId)){g_move.pending=false;return;}
			if (g_move.air) { FinishAirMove(player); return; }  // AS-1
            const auto rover=Rover();
            // 2026-10-28 (the user: a summon 400 m away while high in the air said "moved to you" and the vehicle never came): the
            // site check casts from 6 m over to 120 m under the height it is given, which was the player's. The ground under the
            // spot (the long ray asked for in AskMove) is the height to check at; until that ray answers, wait.
            double checkZ=g_move.z;
            {
                float ground=0;unsigned age=0;
                if(prefetch::LatestGround(prefetch::kSlotRescue0,ground,age)&&age<120&&ground>-1.0e30f&&std::isfinite(ground))checkZ=double(ground)+2.0;
                else if(GetTickCount64()-g_move.asked<6000)return;
            }
            const auto site=environment::CheckSite(g_roverId,g_move.x,g_move.y,checkZ,g_move.yaw);
            if(site.verdict!=environment::Verdict::Safe) {
                const bool decided=site.verdict==environment::Verdict::Unsafe;
                if(!decided&&GetTickCount64()-g_move.asked<6000)return;
                // An unsafe spot (slope, roof edge, water) is not the end of the summon: two more spots round the player,
                // 15 m out and 120 degrees apart, each with its own long ground ray.
                if(!g_move.summonPoint&&g_move.tries<2){
                    ++g_move.tries;
                    const auto pp=At<game::Vec3>(player,off::kRefPosition);
                    const double a=g_move.yaw+3.14159265358979+g_move.tries*2.0943951;
                    g_move.x=pp.x+std::sin(a)*15.0;g_move.y=pp.y+std::cos(a)*15.0;g_move.asked=GetTickCount64();
                    prefetch::RequestGround(prefetch::kSlotRescue0,float(g_move.x),float(g_move.y),pp.z+60.0f,0.0f,3000.0f);
                    logger::Write("Summon: spot %d unsafe (%s); trying another at (%.0f, %.0f)",g_move.tries,site.reason,g_move.x,g_move.y);
                    return;
                }
                if(!g_move.air){  // AS-1: no safe ground spot: try the air, out of the camera's view, instead of refusing
                    logger::Write("OBS system=summon.air vehicle=%08X event=start reason=no_safe_ground_spot ground_tries=%d last_reason=\"%s\"",g_roverId,g_move.tries+1,site.reason);
                    g_move.air=true;g_move.airTry=0;AirSpot(player);return;}
                g_move.pending=false;notify::Show("Summon: no safe pickup site");logger::Write("Summon refused after %d spots: %s",g_move.tries+1,site.reason);return;
            }
            g_move.pending=false;
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			if (!rover || !cell || !ws) {
				logger::Write("Rover call: move cancelled (%s)", rover ? "player not outdoors" : "rover not found");
				return;
			}
			float pos[3] = { float(g_move.x), float(g_move.y), float(site.ground+1.0) };
			float rot[3] = { 0.0f, 0.0f, float(g_move.yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(rover, &handle, cell, ws, pos, rot);
				logger::Write("Rover call: moved to (%.1f, %.1f, %.1f) %s (ground %s)", pos[0], pos[1], pos[2],
					g_move.summonPoint ? "at the area's summon point" : "next to the player", "validated footprint and boarding side");
				g_movedToPoint = g_move.summonPoint;
				if (g_move.summonPoint) notify::Show("Your vehicle is at the summon point");
				g_movedAt = GetTickCount64();
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::Write("Rover call: move raised an exception");
			}
		}

		bool Settle(int idx, const char* why);
		void SettleTick(int idx, std::uintptr_t vehicle);
		void Stop(int idx, const char* why)
		{
			if (Settle(idx, why)) return;
			g_shs[idx].physOn.store(false); PhysicalRelease(g_shs[idx]);
			if (!idx) vehiclecamera::Reset();
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const auto releasedVehicle = g_hoverVehicles[idx].load();
			const bool wreck=releasedVehicle&&vehiclehits::Wrecked(At<std::uint32_t>(releasedVehicle,off::kFormId));
            g_cmds[idx]={};
            g.groundHug = false;
			if (releasedVehicle) g_clearBoosts[idx] = releasedVehicle;  // its boost flags off at its next boost update
			g.active = false;
			g.landing=false;g.landProofAt=0;
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.active = false;
				g_sh.physical = false; g_sh.physicalState={};g_sh.physicalFault=false;
				g_sh.groundOn = false;
				g_sh.landing = false;
				g_sh.haveLast = false;
				g_sh.collided = false;
			}
			g_hoverVehicles[idx] = 0;
			g_driveOut[idx].Clear();
			g_forwardBoosts[idx] = false;
			g_hoverBoostWant[idx].store(false); g_hoverBoostActive[idx].store(false);  // BO-2 (the meter itself is reseeded by the physics thread on a vehicle change)
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.braking = false;
				g_sh.strafe = 0;
				g_sh.vzOn = false;
				g_sh.rollAimOn = false;
				if (!wreck && (g.autopilot || g_sh.autopilot)) {
					g_sh.guardVehicle = releasedVehicle;  // an NPC pilot keeps its vehicle routed to it until the guard ends
					g_sh.guardInstance = g_sh.instance.load(std::memory_order_relaxed);
					g_sh.guardUntil = GetTickCount64() + kGuardMs;
					g_sh.guardMax = 0;
				}
				g_sh.autopilot = false;
				if(wreck){g_sh.guardVehicle=0;g_sh.guardInstance=0;g_sh.guardUntil=0;}
			}
			std::snprintf(g_endWhy[idx], sizeof(g_endWhy[idx]), "%s", why);
			if (g.autopilot) logger::Write("%s: autopilot ended (%s)", idx ? Who(idx) : "Rover call", why);
			g.autopilot = false;
			g_autoOffWhy = g_endWhy[idx]; g.playerAuto = false;
			g.holdId = 0;
			g.routeHold=false;
			if (idx) logger::Write("%s: hover off (%s)", Who(idx), why);
			else logger::Write("Rover flight: off (%s)", why);
			if (!idx) notify::Log("Hover off");
			if (!idx && std::strncmp(why, "collision", 9) == 0) g_relaunchAt = GetTickCount64() + 4000;
		}
		void Stop(const char* why) { Stop(0, why); }

		bool PlayerDead(std::uintptr_t player)
		{
			__try {
				auto** vt = *reinterpret_cast<void***>(player);
				return reinterpret_cast<bool (*)(std::uintptr_t, int)>(vt[0x868 / 8])(player, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// CW-1 (2026-11-18, user: "it wants to fly and then drop rather than cruising at whatever state and then switching depending on
		// route"): Auto / Cruise / the scanner trip on a car that is on its wheels start in drive mode at once (the game's own wheels, the
		// hover model not writing), exactly as the drive branch enters it; Mode (the energy plan on the route's rule) may switch to the
		// hover later where the route allows it. REPLACED: every start was a hover lift-off, then the plan brought it down to drive.
		void Start(int idx, std::uintptr_t vehicle, bool onWheels);
		void Start(int idx, std::uintptr_t vehicle) { Start(idx, vehicle, false); }
		void Start(int idx, std::uintptr_t vehicle, bool onWheels)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const bool keyDown = g.keyDown;
			const int gear = g.gear;  // the gear carries over from driving
			g = State{};
			g.keyDown = keyDown;
			g.gear = gear;
			g.liftoff = true;  // decided on the first ground sample: near the ground, climb to the lift-off height
			g.startedAt = GetTickCount64();
			if (!idx) {double cp;bool first;
				if(CameraAimReading(vehicle,cp,first))g_camRest.Observe(cp,first,false);
			}
			g.active = true;
			g.baseZ = pos.z;
			g.freshAt = GetTickCount64() + 150;
			QueryPerformanceCounter(&g.last);
			{
				// Nothing is overwritten: the body keeps the speed and attitude the driving left it; the spin starts at rest
				// and the gravity estimate carries over from the last flight.
				std::lock_guard lk(g_sh.lock);
				g_sh.px = pos.x; g_sh.py = pos.y; g_sh.pz = pos.z;
				g_sh.wYaw = g_sh.wPitch = g_sh.wRoll = 0;
				g_sh.physicalState={};
				g_sh.physicalFault=false;
				g_sh.haveLastW = false;
				g_sh.instance.store(0);g_sh.matched=false;
				g_sh.stepSerial=0;g_sh.lastStepAt=0;g_sh.lastStepInstance=0;
                g_sh.handoffStepAt=0;g_sh.handoffStepInstance=0;
				g_sh.gEst = std::clamp(g_sh.gEst, 3.0, 20.0);  // a drained estimate would start a flight with no lift
				g_sh.holding = false;
				g_sh.haveLast = false;
				g_sh.collided = false;
				g_sh.guardVehicle = 0;
				g_sh.guardInstance = 0;
				g_sh.groundOn = false;
				BindBody(g_sh,vehicle);
				g_sh.active = true;
			}
			g_hoverVehicles[idx] = vehicle;
			g_endWhy[idx][0] = 0;
			if (onWheels) {  // CW-1: drive mode at once, no lift-off
				g.liftoff = false;
				g.drive = true; g.driveSince = GetTickCount64(); g.drvT = 0; g.drvSteer = 0; g.autoOnWheels = true;
				g.adOut.mode = drive::Mode::Drive;
				{ std::lock_guard lk(g_sh.lock); g_sh.active = false; g_sh.haveLast = false; }
				g_clearBoosts[idx] = vehicle; g_forwardBoosts[idx] = false;
				logger::Write("%s: drive mode ON (autopilot started on its wheels at %.0f, %.0f, %.0f): no lift-off; the route and the plan pick hover only where allowed",
					idx ? Who(idx) : "Rover call", pos.x, pos.y, pos.z);
				return;
			}
			if (idx) {
				logger::Write("%s: hover on at (%.0f, %.0f, %.0f)", Who(idx), pos.x, pos.y, pos.z);
				return;
			}
			logger::Write("Rover flight: on at (%.0f, %.0f, %.0f)", pos.x, pos.y, pos.z);
			notify::Log("Hover on - [W] accelerate [S] brake [Alt tap] gear [%s] boost (double tap: stays on)  [%s]/[%s] up/down (hold)  [%s]/[%s] turn  [%s] off",
				bindings::Label(bindings::Action::Boost, VK_LSHIFT).c_str(), bindings::Label(bindings::Action::LiftUp, g_keys.up).c_str(),
				bindings::Label(bindings::Action::LiftDown, g_keys.down).c_str(), bindings::Label(bindings::Action::TurnLeft, g_mk.left).c_str(), bindings::Label(bindings::Action::TurnRight, g_mk.right).c_str(),
				bindings::Label(bindings::Action::HoverToggle, g_keys.toggle).c_str());
			telemetry::Record("rover_flight_on", "\"pos\":[%.0f,%.0f,%.0f]", pos.x, pos.y, pos.z);
		}
		void Start(std::uintptr_t vehicle) { Start(0, vehicle, false); }

		// Baked terrain (cellbake, 2026-10-29): an NPC pilot takes the cell's baked terrain height instead of its own terrain probe there
		// (settings.ini [nav] bakedGround=1). The player's own vehicle (idx 0) always probes live.
		bool BakedFor(int idx, double x, double y, double& h) { return idx > 0 && cellbake::UseForPilots() && cellbake::Ground(x, y, h); }
		void ReadSamples(int idx)
		{
			State& g = g_gs[idx];
			if (!g.haveAsked || GetTickCount64() < g.freshAt) {
				return;
			}
			for (int i = 0; i < kSamples; ++i) {
				float h = -3.4e38f;
				unsigned age = 0;
				const bool ahead = i != kCentre && i != kLeft && i != kRight;
				double baked = 0;
				if (ahead && BakedFor(idx, g.asked[i].x, g.asked[i].y, baked)) { h = float(baked); g.asked[i].valid = true; }
				else {
					g.asked[i].valid = prefetch::LatestGround(ModelSlot(idx, i), h, age) && age <= MaxAge(idx) && h > -1e30f && h != -500.0f;
					if (ahead && g.asked[i].valid) cellbake::Compare(g.asked[i].x, g.asked[i].y, h);  // the terrain probe the bake copies
				}
				g.asked[i].h = h;
				if (ahead) {  // ahead: the terrain or the surface under the flight level, whichever is higher
					float lh = -3.4e38f;
					unsigned la = 0;
					if (prefetch::LatestGround(LowSlot(idx, i), lh, la) && la <= MaxAge(idx) && lh > -1e30f && lh != -500.0f && (!g.asked[i].valid || lh > g.asked[i].h)) {
						g.asked[i].h = lh;
						g.asked[i].valid = true;
					}
				}
				if (g.asked[i].valid && InVehicleSpace(SelfId(idx), g.asked[i].x, g.asked[i].y, h, true)) {  // another vehicle's roof is no ground
					g.asked[i].valid = false;
					++g.vehDropped;
				}
				if (g.asked[i].valid && i != kCentre) navmap::NoteGround(g.asked[i].x, g.asked[i].y, g.asked[i].h);
			}
			// The centre ray starts 1.5 m under the rover (never its own body). On a raised floor (a city platform) that start
			// is already under the floor and the ray finds the terrain far below (2026-10-17 15:08: "climbed to 245 m" at once,
			// then a dive into the platform and a collision stop, over and over). The side rays start high above and land on
			// the floor: a side surface at or just under the rover is the ground when it is higher than the centre answer.
			Sample c = g.asked[kCentre];
			for (const int side : { kLeft, kRight }) {
				const auto& q = g.asked[side];
				if (q.valid && q.h <= g.askZ + 1.0 && (!c.valid || q.h > c.h + 2.0)) {
					c.h = q.h;
					c.valid = true;
				}
			}
			if (!c.valid) {
				return;
			}
			{   // GD-1 (2026-11-19, 19:58 run: on the corner deck the car read 2.2 m above ground while the addon's floor was under its wheels):
				// trace what the centre and side probes said when the chosen ground is more than 1 m under where the car asked from.
				static ULONGLONG gdLog[1 + prefetch::kSlotPilotCount] = {};
				if (g.drive && idx >= 0 && idx <= prefetch::kSlotPilotCount && g.askZ - c.h > 1.0 && GetTickCount64() - gdLog[idx] >= 1000) { gdLog[idx] = GetTickCount64();
					const auto& L = g.asked[kLeft]; const auto& R = g.asked[kRight]; const auto& C = g.asked[kCentre];
					logger::Write("OBS system=vehicle.ground event=low_reading pilot=%d ask_z=%.2f chosen=%.2f centre=%d:%.2f left=%d:%.2f right=%d:%.2f reason=ground_more_than_1m_under_a_car_on_its_wheels",
						idx, g.askZ, c.h, int(C.valid), C.h, int(L.valid), L.h, int(R.valid), R.h); }
			}
			if (!g.haveGround || std::fabs(c.h - g.ground) <= 5.0) {
				g.ground = c.h;
				g.haveGround = true;
				g.agree = 0;
				g.groundAt = GetTickCount64();
			}
			else if (g.agree > 0 && std::fabs(c.h - g.candidate) <= 1.0) {
				if (++g.agree >= 3) {
					g.ground = c.h;
					g.agree = 0;
					g.groundAt = GetTickCount64();
				}
			}
			else {
				g.candidate = c.h;
				g.agree = 1;
				++g.rejected;
			}
		}

		void AskSamples(int idx, const game::Vec3& pos, double heading, double vx, double vy)
		{
			if (idx >= 1) {
				if (const auto pl = Ptr(addr::kPlayerCharacterPtr)) {
					const auto pp = At<game::Vec3>(pl, off::kRefPosition);
					if (std::hypot(double(pos.x) - double(pp.x), double(pos.y) - double(pp.y)) > 240.0) return;
				}
			}
			State& g = g_gs[idx];
			const double speed = std::hypot(vx, vy);
			double dx = std::sin(heading), dy = std::cos(heading);
			if (speed > 2.0) {
				dx = vx / speed;
				dy = vy / speed;
			}
			const double rx = std::cos(heading), ry = -std::sin(heading);
			g.askZ = pos.z;
			g.asked[kCentre] = { pos.x, pos.y, 0 };
			g.asked[kFront] = { pos.x + dx * 6.0, pos.y + dy * 6.0, 6.0 };
			g.asked[kRight] = { pos.x + rx * 4.0, pos.y + ry * 4.0, 0 };
			g.asked[kLeft] = { pos.x - rx * 4.0, pos.y - ry * 4.0, 0 };
			const double ahead[3] = { std::max(15.0, speed * 0.75), std::max(30.0, speed * 1.5), std::max(45.0, speed * 2.5) };
			for (int i = 0; i < 3; ++i) {
				g.asked[kAhead0 + i] = { pos.x + dx * ahead[i], pos.y + dy * ahead[i], ahead[i] };
			}
			for (int i = 0; i < kSamples; ++i) {
				const float z = i == kCentre ? pos.z - 1.5f : pos.z + float(kCastAbove);
				if (i == kCentre || i == kLeft || i == kRight) {
					prefetch::RequestGround(ModelSlot(idx, i), float(g.asked[i].x), float(g.asked[i].y), z, 0.0f, 8000.0f);
				}
				else {  // ahead: the terrain from high above, and every surface under the flight level
					double baked = 0;
					if (!BakedFor(idx, g.asked[i].x, g.asked[i].y, baked))
						prefetch::RequestGroundMask(ModelSlot(idx, i), float(g.asked[i].x), float(g.asked[i].y), z, 0.0f, 8000.0f, kTerrainLayers);
					prefetch::RequestGround(LowSlot(idx, i), float(g.asked[i].x), float(g.asked[i].y), pos.z + float(kFlightLevel), 0.0f, 8000.0f);
				}
			}
			g.haveAsked = true;
		}

		// Avoidance corridors: ahead, left, right of the travel heading, three distances each, rays from high above.
		// A reference's display name (vtable +0x268, what the console's PickLastRef prints), or "".
		const char* DisplayName(std::uintptr_t ref)
		{
			__try {
				const char* n = reinterpret_cast<const char* (*)(std::uintptr_t)>((*reinterpret_cast<void***>(ref))[0x268 / 8])(ref);
				return n ? n : "";
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return "";
			}
		}

		// The object the straight ray hit: the nearest loaded reference to the hit point (not this vehicle, not the player),
		// logged once per object and at most once a second (only with the verbose log: it scans every loaded reference).
		void NameBlocker(int idx)
		{
			State& g = g_gs[idx];
			const ULONGLONG now = GetTickCount64();
			if (now - g.lastBlockerAt < 1000) return;
			g.lastBlockerAt = now;
			const auto& h = g.ray[0];
			const std::uintptr_t self = g_hoverVehicles[idx].load(), player = Ptr(addr::kPlayerCharacterPtr);
			std::uintptr_t best = 0;
			double bestD = 12.0;
			for (const auto r : modscene::LoadedRefs()) {
				if (!r || r == self || r == player) continue;
				const auto p = At<game::Vec3>(r, off::kRefPosition);
				const double d = std::sqrt(std::pow(p.x - h.x, 2) + std::pow(p.y - h.y, 2) + std::pow(p.z - h.z, 2));
				if (d < bestD) { bestD = d; best = r; }
			}
			// The exact owner of the hit body, when the body -> ref path gave one.
			if (h.formId) {
				const std::uintptr_t exact = RefById(h.formId);
				if (exact && exact != self) best = exact, bestD = 0.0;
			}
			if (best == g.lastBlocker) return;
			g.lastBlocker = best;
			if (!best) {
				logger::Write("%s: blocked %.1f m ahead at (%.1f, %.1f, %.1f), layer %u, body %u: no loaded object within 12 m of the hit (terrain or a large mesh)", Who(idx),
					h.distance, h.x, h.y, h.z, h.layer, h.bodyId);
				return;
			}
			const char* name = DisplayName(best);
			const auto base = At<std::uintptr_t>(best, 0x98);
			logger::Write("%s: blocked %.1f m ahead by %08X '%s' (base %08X, %s, %s, layer %u, body %u)", Who(idx), h.distance, At<std::uint32_t>(best, off::kFormId),
				name ? name : "", base ? At<std::uint32_t>(base, off::kFormId) : 0, modscene::RttiName(best), bestD == 0.0 ? "exact" : "nearest to the hit", h.layer,
				h.bodyId);
		}

		void AskCorridors(int idx, const game::Vec3& pos, double heading, double speed)
		{
			if (idx >= 1) {
				if (const auto pl = Ptr(addr::kPlayerCharacterPtr)) {
					const auto pp = At<game::Vec3>(pl, off::kRefPosition);
					if (std::hypot(double(pos.x) - double(pp.x), double(pos.y) - double(pp.y)) > 240.0) return;
				}
			}
			State& g = g_gs[idx];
			const double offs[3] = { 0.0, -kAvoidSide, kAvoidSide };
			const double dist[3] = { std::max(12.0, speed * 0.8), std::max(25.0, speed * 1.6), std::max(40.0, speed * 2.6) };
			for (int c = 0; c < 3; ++c) {
				const double hd = heading + offs[c];
				for (int k = 0; k < 3; ++k) {
					auto& q = g.corr[c * 3 + k];
					q.x = pos.x + std::sin(hd) * dist[k];
					q.y = pos.y + std::cos(hd) * dist[k];
					q.d = dist[k];
					double baked = 0;
					if (!BakedFor(idx, q.x, q.y, baked))
						prefetch::RequestGroundMask(CorridorSlot(idx, c * 3 + k), float(q.x), float(q.y), pos.z + float(kCastAbove), 0.0f, 8000.0f, kTerrainLayers);  // terrain
					prefetch::RequestGround(LowSlot(idx, 7 + c * 3 + k), float(q.x), float(q.y), pos.z + float(kFlightLevel), 0.0f, 8000.0f);              // under the flight level
				}
				const double range = std::max(kRayMin, speed * kRayLead);
				const float from[3] = { float(pos.x + std::sin(hd) * kRayStart), float(pos.y + std::cos(hd) * kRayStart), float(pos.z + kRayLift) };
				const float to[3] = { float(from[0] + std::sin(hd) * range), float(from[1] + std::cos(hd) * range), from[2] };
				prefetch::RequestRay(prefetch::CorridorRay(idx, c), from, to, kWallLayers);
				g.rayRange[c] = range;
			}
			g.haveCorr = true;
			{
				const float from[3] = { float(pos.x), float(pos.y), float(pos.z + 1.5) };
				const float up[3] = { from[0], from[1], float(from[2] + kCeilUp) };
				const float ahead[3] = { float(pos.x + std::sin(heading) * kCeilAhead), float(pos.y + std::cos(heading) * kCeilAhead), float(from[2] + kCeilAheadUp) };
				prefetch::RequestRay(prefetch::CeilingRay(idx, 0), from, up, kWallLayers);
				prefetch::RequestRay(prefetch::CeilingRay(idx, 1), from, ahead, kWallLayers);
			}
			if (g.autopilot && navmap::HullEnabled()) {
				// The vehicle's own shape swept along the planned heading (hull-sized: what a thin ray cannot know).
				const std::uintptr_t inst = g_shs[idx].instance.load(std::memory_order_relaxed);
				if (inst && (idx == 0 || StepsRecent(idx, GetTickCount64()))) {
					g.hullRange = std::max(kRayMin, speed * kRayLead);
					prefetch::RequestSweep(idx, inst, float(std::sin(heading)), float(std::cos(heading)), float(g.hullRange), kWallLayers);
				} else if (inst) {
					logger::Observe("world.hull", SelfId(idx), 0, "event=deferred reason=no_recent_steps pilot=%d", idx);
				}
			}
			// The sweep ring (the lane path rays read the old lane driver's path array and are gone with it; the radar's path sweep casts
			// the car's own hull along the energy plan's path instead).
			if (g.autopilot) {
				const double sweepRange = std::clamp(speed * 2.5, 30.0, 80.0);
				for (int k = 0; k < kSweepRays; ++k) {
					const double hd = double((g.sweepPhase * kSweepRays + k) % kSweepRing) * (6.283185307179586 / kSweepRing);
					const float from[3] = { float(pos.x + std::sin(hd) * kRayStart), float(pos.y + std::cos(hd) * kRayStart), float(pos.z + kRayLift) };
					const float to[3] = { float(from[0] + std::sin(hd) * sweepRange), float(from[1] + std::cos(hd) * sweepRange), from[2] };
					prefetch::RequestRay(prefetch::SweepRay(idx, k), from, to, kWallLayers);
				}
				++g.sweepPhase;
			}
		}

		void ReadCorridors(int idx)
		{
			State& g = g_gs[idx];
			if (!g.haveCorr || GetTickCount64() < g.freshAt) return;
			for (int i = 0; i < 9; ++i) {
				float h = -3.4e38f;
				unsigned age = 0;
				double baked = 0;
				if (BakedFor(idx, g.corr[i].x, g.corr[i].y, baked)) { h = float(baked); g.corr[i].valid = true; }
				else {
					g.corr[i].valid = prefetch::LatestGround(CorridorSlot(idx, i), h, age) && age <= MaxAge(idx) && h > -1e30f && h != -500.0f;
					if (g.corr[i].valid) cellbake::Compare(g.corr[i].x, g.corr[i].y, h);
				}
				g.corr[i].h = h;
				{
					float lh = -3.4e38f;
					unsigned la = 0;
					if (prefetch::LatestGround(LowSlot(idx, 7 + i), lh, la) && la <= MaxAge(idx) && lh > -1e30f && lh != -500.0f && (!g.corr[i].valid || lh > g.corr[i].h)) {
						g.corr[i].h = lh;
						g.corr[i].valid = true;
					}
				}
				if (g.corr[i].valid && InVehicleSpace(SelfId(idx), g.corr[i].x, g.corr[i].y, h, true)) {
					g.corr[i].valid = false;
					++g.vehDropped;
				}
				if (g.corr[i].valid) navmap::NoteGround(g.corr[i].x, g.corr[i].y, h);
			}
			// A hit on the player (their character controller) is where the vehicle is going, not a wall.
			const std::uintptr_t player = Ptr(addr::kPlayerCharacterPtr);
			const auto pp = player ? At<game::Vec3>(player, off::kRefPosition) : game::Vec3{};
			for (int c = 0; c < 3; ++c) {
				unsigned age = 0;
				g.rayValid[c] = prefetch::LatestRay(prefetch::CorridorRay(idx, c), g.ray[c], age) && age <= MaxAge(idx);
				auto& h = g.ray[c];
				const bool rawHit = h.hit;
				if (g.rayValid[c] && h.hit && player && std::hypot(h.x - pp.x, h.y - pp.y) < 2.5 && h.z > pp.z - 1.0 && h.z < pp.z + 3.0) h.hit = false;
				bool onVehicle = false;
				if (g.rayValid[c] && h.hit && InVehicleSpace(SelfId(idx), h.x, h.y, h.z)) {  // a vehicle: the warning's job, not a wall
					h.hit = false;
					onVehicle = true;
					++g.vehDropped;
				}
				if (g.rayValid[c] && rawHit && !h.formId) h.formId = loadedarea::HitBodyOwner(h.bodyId);  // trace: who owns the hit body
				if (g.rayValid[c]) navmap::NoteRay(h.from, h.to, rawHit && !onVehicle, h.x, h.y, h.z, h.layer, h.normalOk, h.nz, false, h.formId);  // characters and vehicles are free space to the map, not walls
			}
			// Ceiling: the lowest hit above the vehicle of the two ceiling rays (another vehicle's underside does not count).
			g.ceilValid = false;
			unsigned ceilingAnswers = 0;
			for (int k = 0; k < 2; ++k) {
				prefetch::RayHit ch;
				unsigned age = 0;
				if (!prefetch::LatestRay(prefetch::CeilingRay(idx, k), ch, age) || age > MaxAge(idx)) continue;
				++ceilingAnswers;
				if (!ch.hit) continue;
				if (ch.z < g.askZ + 1.0 || InVehicleSpace(SelfId(idx), ch.x, ch.y, ch.z)) continue;
				if (!g.ceilValid || ch.z < g.ceilZ) { g.ceilZ = ch.z; g.ceilValid = true; }
			}
			g.ceilKnown = ceilingAnswers == 2;
			g.hullValid = g.hullHit = false;
			if (g.autopilot && navmap::HullEnabled()) {
				prefetch::SweepHit sw;
				unsigned age = 0;
				g.hullValid = prefetch::LatestSweep(idx, sw, age) && age <= MaxAge(idx);
				if (g.hullValid) {
					g.hullDist = sw.distance;
					g.hullHit = sw.hit && sw.distance >= 1.0f;  // (a hit inside the first metre is the vehicle's own surroundings)
					if (sw.hit && InVehicleSpace(SelfId(idx), sw.x, sw.y, sw.z)) {
						g.hullHit = false;
						sw.hit = false;
						++g.vehDropped;
					}
					// A sweep hit is where the car's own body would strike: a face whatever its height (2026-11-05: noted with no normal, a
					// bumper-height post fell under the 1.1 m step rule and was never a wall). Kept on any layer.
					if (!sw.hit || g.hullHit) navmap::NoteRay(sw.from, sw.to, sw.hit, sw.x, sw.y, sw.z, sw.layer, sw.hit, 0.0f, true);
					if (g.hullHit) ++g_hullHits[idx];
					++g_hullAnswers[idx];
					const bool rayBlocked = g.rayValid[0] && g.ray[0].hit && g.ray[0].distance < g.rayRange[0];
					navmap::NoteHull(rayBlocked, g.hullHit && g.hullDist < g.hullRange);
					const ULONGLONG nowMs = GetTickCount64();
					if (g.hullHit && g.hullDist < g.hullRange && !rayBlocked && nowMs - g.hullLogAt >= 2000) {
						g.hullLogAt = nowMs;
						if (logger::Verbose()) logger::Write("%s: hull sweep blocked %.1f m ahead (layer %u, body %u) where the thin ray saw nothing", Who(idx), sw.distance, sw.layer, sw.bodyId);
					}
				}
			}
			if (g.autopilot) {
				for (int k = 0; k < kSweepRays; ++k) {
					prefetch::RayHit sh;
					unsigned age = 0;
					if (prefetch::LatestRay(prefetch::SweepRay(idx, k), sh, age) && age <= MaxAge(idx)) {
						if (sh.hit && InVehicleSpace(SelfId(idx), sh.x, sh.y, sh.z)) sh.hit = false;  // vehicles move: never walls in the map
						if (sh.hit && idx >= 0 && idx < int(sizeof g_pathRayMode) && g_pathRayMode[idx]) { ++g_pathRayHits[idx]; if (sh.normalOk && std::fabs(sh.nz) < 0.5f) ++g_pathRayFaces[idx]; }
						if (sh.hit && !sh.formId) sh.formId = loadedarea::HitBodyOwner(sh.bodyId);  // trace: who owns the hit body (0 until the main thread resolves it)
						navmap::NoteRay(sh.from, sh.to, sh.hit, sh.x, sh.y, sh.z, sh.layer, sh.normalOk, sh.nz, idx >= 0 && idx < int(sizeof g_pathRayMode) && g_pathRayMode[idx], sh.formId);
						if (sh.hit && idx >= 0 && idx < int(sizeof g_pathRayMode) && g_pathRayMode[idx] && sh.layer < 64) ++g_pathLayers[idx][sh.layer];
					}
				}
			}
			if (g.autopilot && g.rayValid[0] && g.ray[0].hit && logger::Verbose()) NameBlocker(idx);
		}

		// The wall warning (once a frame, after ReadCorridors): how far the nearest wall is along the way and to each side.
		bool IsWallSample(const State& g, double h, double d)
		{
			if (!g.haveGround) return false;
			const double rise = h - g.ground;
			return rise > kWallMinRise && rise / std::max(d, 1.0) > kWallSlope && h > g.askZ - 1.0;  // (a wall whose top is under us is no wall)
		}

		void WallScan(int idx, const char* who)
		{
			State& g = g_gs[idx];
			g.wallAhead = false;
			g.wallDist = g.wallL = g.wallR = 1e9;
			if (g.rayValid[0] && g.ray[0].hit) { g.wallAhead = true; g.wallDist = std::min(g.wallDist, double(g.ray[0].distance) + kRayStart); }
			if (g.hullValid && g.hullHit) { g.wallAhead = true; g.wallDist = std::min(g.wallDist, double(g.hullDist)); }
			for (int k = 0; k < 3; ++k) {
				const auto& q = g.corr[k];
				if (q.valid && IsWallSample(g, q.h, q.d)) { g.wallAhead = true; g.wallDist = std::min(g.wallDist, q.d); }
			}
			if (g.rayValid[1] && g.ray[1].hit) g.wallL = double(g.ray[1].distance) + kRayStart;
			if (g.rayValid[2] && g.ray[2].hit) g.wallR = double(g.ray[2].distance) + kRayStart;
			const ULONGLONG now = GetTickCount64();
			static ULONGLONG toldAt[kPilots] = {};
			if (g.wallAhead && !g.autopilot && g.active && g.wallDist < 25.0 && now - toldAt[idx] >= 3000) {  // by hand: the warning only
				toldAt[idx] = now;
				notify::Log("Assist: wall ahead %.0f m", g.wallDist);
			}
			if (g.wallAhead && logger::Verbose() && now - g.wallLogAt >= 2000) {
				g.wallLogAt = now;
				logger::Write("%s: wall warning: %.1f m ahead (left %.0f, right %.0f)", who, g.wallDist, g.wallL > 1e8 ? -1.0 : g.wallL, g.wallR > 1e8 ? -1.0 : g.wallR);
			}
		}

		// A corridor: 0 clear at the current height, 1 clear with a climb that fits in time, 2 blocked (or unknown).
		int Corridor(int idx, int c, double z, double speed, double& needOut)
		{
			const State& g = g_gs[idx];
			int verdict = 0;
			double need = -1e9;
			// Something solid straight along this corridor at the vehicle's own height: blocked, whatever the ground says.
			if (g.autopilot && g.rayValid[c] && g.ray[c].hit && g.ray[c].distance < g.rayRange[c]) {
				needOut = kAvoidMaxClimb + 1.0;
				return 2;
			}
			// The vehicle's own shape swept along the planned heading hits something: blocked, however the thin ray read.
			if (g.autopilot && c == 0 && g.hullHit && g.hullDist < g.hullRange) {
				needOut = kAvoidMaxClimb + 1.0;
				return 2;
			}
			for (int k = 0; k < 3; ++k) {
				const auto& q = g.corr[c * 3 + k];
				if (!q.valid) continue;  // not loaded yet: unknown, not a wall (the blind speed cap covers it)
				const double n = q.h + kAvoidClear - z;  // metres to climb to pass this sample
				need = std::max(need, n);
				if (n <= 0) continue;
				if (IsWallSample(g, q.h, q.d)) {
					if (!g.autopilot) continue;   // flying by hand: a wall is only the warning (no turn, no stop, no climb)
					return needOut = need, 2;     // autopilot: go round it (climbing only as the last resort, cruiseExtra)
				}
				const double t = q.d / std::max(speed, 5.0);
				if (n <= kHoldMaxVz * t) continue;  // the normal height hold rises over this in time
				if (n > kAvoidMaxClimb || n > kAvoidClimbRate * t + 2.0) return needOut = need, 2;
				verdict = 1;
			}
			needOut = need;
			return verdict;
		}
	}

    bool PlayerDriving(std::uint32_t id){return id&&g_manualDriverId.load()==id;}

	bool Active() { return g.active; }
	std::uint64_t NativeStepCount() { return g_nativeStepTotal.load(std::memory_order_relaxed); }
	bool PhysicalCameraActive(std::uintptr_t vehicle) {
		return vehicle && g_sh.physOn.load(std::memory_order_acquire) && g_sh.physInstance.load() &&
			g_hoverVehicle.load()==vehicle && g_driverVehicle.load()==vehicle;
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kVehicleStep, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&StepDetour), &original)) {
			logger::Write("Rover flight: vehicle step hook failed");
			return false;
		}
		g_original = reinterpret_cast<Step_t>(original);
		logger::Write("Rover flight: vehicle physics step hooked");
		void* level=nullptr; void* damp=nullptr; void* clamp=nullptr;
		const bool levelOk=hook::Install(game::Base()+vtolhooks::kLevel,vtolhooks::kLevelBytes,sizeof(vtolhooks::kLevelBytes),reinterpret_cast<void*>(&PhysicalLevelDetour),&level);
		if (levelOk) g_physicalLevelOriginal=reinterpret_cast<Level_t>(level);
		const bool dampOk=hook::Install(game::Base()+vtolhooks::kDamp,vtolhooks::kDampBytes,sizeof(vtolhooks::kDampBytes),reinterpret_cast<void*>(&PhysicalDampDetour),&damp);
		if (dampOk) g_physicalDampOriginal=reinterpret_cast<Damp_t>(damp);
		const bool clampOk=hook::Install(game::Base()+vtolhooks::kClamp,vtolhooks::kClampBytes,sizeof(vtolhooks::kClampBytes),reinterpret_cast<void*>(&PhysicalClampDetour),&clamp);
		if (clampOk) g_physicalClampOriginal=reinterpret_cast<Clamp_t>(clamp);
		g_physicalHooksReady=levelOk&&dampOk&&clampOk;
		logger::Write("Physical VTOL: native assist hooks %s (opt-in, player only; native impulse writer checked per body)",g_physicalHooksReady?"ready":"unavailable; existing flight retained");
		vehiclecamera::Install();
		static const std::uint8_t kBoostPrologue[14] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
		void* boost = nullptr;
		if (hook::Install(game::Base() + addr::kVehicleBoostUpdate, kBoostPrologue, sizeof(kBoostPrologue), reinterpret_cast<void*>(&BoostDetour), &boost)) {
			g_boostOriginal = reinterpret_cast<Boost_t>(boost);
			logger::Write("Rover flight: boost update hooked (booster visuals held while hovering)");
		}
		else {
			logger::Write("Rover flight: boost update hook failed (no booster visuals while hovering)");
		}
		return true;
	}

	namespace
	{
		// The height a vertical boost clears on the ground ([nav] jump= in settings.ini, default 4 m): an obstacle taller than this
		// makes an NPC vehicle hover (travelmode).
		double JumpHeight()
		{
			static double value = 0;
			if (value <= 0) value = 4.0;  // [nav] jump: fixed (2026-11-05 settings strip)
			return value;
		}

		// Drive mode off: hover on again (a lift-off from where it is), the game's controls are no longer overridden.
		void LeaveDrive(int idx, std::uintptr_t vehicle, const char* why)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			(void)vehicle;
			g.drive = false;
			g_driveOut[idx].Clear();
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.active = true;
				g_sh.haveLast = false;
				g_sh.collided = false;
				g_sh.wYaw = g_sh.wPitch = g_sh.wRoll = 0;
				g_sh.haveLastW = false;
				g_sh.holding = false;
			}
			g.liftoff = true;
			g.holding = false;
			QueryPerformanceCounter(&g.last);
			logger::Write("%s: drive mode off (%s): hover on", idx ? Who(idx) : "Rover call", why);
		}

		// A contact at the vehicle's place, for the shared nav map (up = pushed up by a floor, else a side bump ahead).
		void NoteContactAt(std::uintptr_t vehicle, bool up)
		{
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const double h = At<game::Vec3>(vehicle, off::kRefRotation).z;
			navmap::NoteContact(pos.x, pos.y, pos.z, up, std::sin(h), std::cos(h));
		}


		// ---- Stage 0 attitude measurement (notes/FLIGHT_MODEL_STUDY_20261023.md, VEHICLE_CAMERA_ROLL_STUDY_20261023.md) ----
		// Bank test: with the detailed log on, the , key steps the player's hover through a held bank of 0 -> 60 -> 120 -> 0 degrees
		// (body-axis turning, freeroll.h). While a bank is held, or free roll is on with the detailed log, a line every 0.5 s
		// logs the attitude, what the game did to our spin, the game's two upright actions and the camera's rows.
		constexpr double kBankTestDeg[3] = { 0.0, 60.0, 120.0 };
		constexpr ULONGLONG kAttitudeLogMs = 500;
		struct BankTest { int step = 0; bool down = false; ULONGLONG logAt = 0, flight = 0; bool settingsLogged = false; } g_bankTest;

		// A VehicleRuntimeComponent's two hknpReorientActions (+0x540 pitch axis, +0x538 roll axis; FUN_141317C90): strength +0x50,
		// damping +0x54 (FUN_14131E290 sets them per frame, 0.1 / 0 when tipped past fVehicleLevelingMinUpAngle).
		struct Reorient { bool ok = false; float pitchStrength = 0, pitchDamping = 0, rollStrength = 0, rollDamping = 0; };
		Reorient ReadReorient(std::uintptr_t component)
		{
			Reorient r;
			if (!component) return r;
			__try {
				const auto p = At<std::uintptr_t>(component, 0x540), q = At<std::uintptr_t>(component, 0x538);
				if (p) { r.pitchStrength = At<float>(p, 0x50); r.pitchDamping = At<float>(p, 0x54); }
				if (q) { r.rollStrength = At<float>(q, 0x50); r.rollDamping = At<float>(q, 0x54); }
				r.ok = p || q;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				r.ok = false;
			}
			return r;
		}

		// The camera root's world rows (+0x80 right, +0x90 forward, +0xA0 up; the readings CameraHeadings uses).
		bool CameraRows(float rows[9])
		{
			const auto cam = Ptr(addr::kPlayerCameraPtr);
			const auto root = cam ? At<std::uintptr_t>(cam, 0x08) : 0;
			if (!root) return false;
			for (int r = 0; r < 3; ++r)
				for (int k = 0; k < 3; ++k) rows[r * 3 + k] = At<float>(root, 0x80 + 0x10 * r + 4 * k);
			return std::isfinite(rows[0]) && std::isfinite(rows[4]) && std::isfinite(rows[8]);
		}

		// The game settings that limit the attitude and the camera (values at setting object +8, degrees): FUN_14131E290 and
		// FUN_140FB4D80 read them. Logged once.
		void LogAttitudeSettings()
		{
			if (g_bankTest.settingsLogged) return;
			g_bankTest.settingsLogged = true;
			const float minUp = At<float>(game::Base(), 0x05F045A0);       // fVehicleLevelingMinUpAngle
			const float camMin = At<float>(game::Base(), 0x05F074C0);      // fVehicleCameraPitchMin
			const float camMax = At<float>(game::Base(), 0x05F074E0);      // fVehicleCameraPitchMax
			logger::Write("Flight attitude: game settings fVehicleLevelingMinUpAngle %.1f, fVehicleCameraPitchMin %.1f, fVehicleCameraPitchMax %.1f (degrees)", minUp, camMin, camMax);
		}

		// The bank test key and the attitude line, pilot 0 hovering. Returns the bank target (rad) while one is held.
		bool BankTestFrame(std::uintptr_t vehicle, ULONGLONG flightStart, bool keysOn, bool autopilot, double above, bool freeRoll, bool firstPerson, double& target)
		{
			auto& t = g_bankTest;
			if (t.flight != flightStart) { t.flight = flightStart; t.step = 0; }  // every flight starts level
			const bool held = keysOn && g_keys.bankTest && (GetAsyncKeyState(g_keys.bankTest) & 0x8000) != 0;
			if (held && !t.down && logger::Verbose() && !autopilot) {
				t.step = (t.step + 1) % 3;
				notify::Show("Bank test: %.0f degrees", kBankTestDeg[t.step]);
				logger::Write("Flight attitude: bank test -> %.0f degrees", kBankTestDeg[t.step]);
			}
			t.down = held;
			if (t.step && (autopilot || above < freeroll::kGroundLevel || !logger::Verbose())) {
				t.step = 0;
				notify::Show("Bank test off");
				logger::Write("Flight attitude: bank test off (%s)", autopilot ? "autopilot" : above < freeroll::kGroundLevel ? "too low" : "detailed log off");
			}
			const ULONGLONG now = GetTickCount64();
			if ((t.step || (freeRoll && logger::Verbose())) && now - t.logAt >= kAttitudeLogMs) {
				t.logAt = now;
				LogAttitudeSettings();
				double pitch, roll, rollPull, pitchPull, maxD, w[3];
				unsigned n; bool physicalLog;
				{
					std::lock_guard lk(g_sh.lock);
					pitch = g_sh.bodyPitch; roll = g_sh.bodyRoll;
					physicalLog=g_sh.physical;
					n = g_sh.measN; rollPull = g_sh.measRollPull; pitchPull = g_sh.measPitchPull; maxD = g_sh.measMax;
					for (int i = 0; i < 3; ++i) w[i] = g_sh.measW[i];
					g_sh.measN = 0; g_sh.measRollPull = g_sh.measPitchPull = g_sh.measMax = 0;
				}
				const auto ro = ReadReorient(vehiclediag::ComponentOf(At<std::uint32_t>(vehicle, off::kFormId)));
				float cr[9] = {};
				const bool camOk = CameraRows(cr);
				const double camRoll = camOk ? std::atan2(-double(cr[2]), double(cr[8])) : 0.0;  // the camera's own roll (right side down +)
				logger::Write("Flight attitude: bank %.0f, roll %.1f pitch %.1f deg; spin %s (%.2f, %.2f, %.2f) rad/s; game change per step over %u steps: "
					"toward level roll %+.3f pitch %+.3f, largest %.3f rad/s; upright actions %s pitch %.2f / %.2f roll %.2f / %.2f (strength / damping); "
					"camera %s %s roll %.1f deg rows R(%.2f %.2f %.2f) F(%.2f %.2f %.2f) U(%.2f %.2f %.2f)",
					kBankTestDeg[t.step], roll * 57.2958, pitch * 57.2958, physicalLog?"after impulses":"written",w[0], w[1], w[2], n, n ? rollPull / n : 0.0, n ? pitchPull / n : 0.0, maxD,
					ro.ok ? "read" : "NOT read", ro.pitchStrength, ro.pitchDamping, ro.rollStrength, ro.rollDamping,
					firstPerson ? "1st person" : "3rd person", camOk ? "ok" : "MISSING", camRoll * 57.2958, cr[0], cr[1], cr[2], cr[3], cr[4], cr[5], cr[6], cr[7], cr[8]);
			}
			target = kBankTestDeg[t.step] / 57.2958;
			return t.step != 0;
		}

		bool CoverFighting();

		// World radar (radar.h; notes/radar-20261113/DECOMP_FINDINGS.md). Engine side: the body lookup and the game's own collision test.
		// Every address is from the 1.16.244 decomp; each call is behind an exception guard and traced (world.radar).
		constexpr std::uint32_t kRvaCollisionFilter = 0x05FDCE60;   // bhkCollisionFilter* (layer table at +0x1B8)
		constexpr std::uint32_t kRvaCollides = 0x02398EF0;          // FUN_142398EF0(filter, infoA, infoB): the game's pair test
		constexpr std::uint32_t kRvaShapeFunctions = 0x05D7AB40;    // hknpShapeFunctionsSingleton*: table at +0x18, 0x100 bytes per type, calcAabb entry 0
		constexpr std::uint32_t kRvaShapeNullAabb = 0x030B7890;     // hknpShapeNullFunctions::calcAabb: "not implemented" (asserts): never call it
		constexpr std::uint32_t kRvaIdentity = 0x0587A7E0;          // hkTransform identity (verified in the executable)
		// The body behind `bodyId` in the world of `instance` (a vehicle instance's read interface, as FindBody): its collision info,
		// motion, transform and its shape's own box. -1 fault, 0 not usable, 1 ok.
		int RadarLookup(std::uintptr_t instance, std::uint32_t bodyId, radar::Lookup& out)
		{
			out = radar::Lookup{};
			if (!instance || !bodyId) return 0;
			__try {
				const auto read = At<std::uintptr_t>(instance, 0x28);
				if (!read) return 0;
				const auto body = VFn<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(read, 0x70)(read, bodyId);
				if (!body || At<std::uint32_t>(body, 0x50) != bodyId) return 0;
				out.info = At<std::uint32_t>(body, 0x8C);
				out.dynamic = (At<std::uint32_t>(body, 0x54) & 7) == 2;
				for (int i = 0; i < 12; ++i) out.rot[i] = reinterpret_cast<const float*>(body)[i];
				for (int i = 0; i < 4; ++i) out.pos[i] = reinterpret_cast<const float*>(body + 0x30)[i];
				const auto shape = At<std::uintptr_t>(body, 0x78);
				if (!shape) return 0;
				const unsigned type = At<std::uint8_t>(shape, 0x1A);
				out.shapeType = int(type);
				if (type >= 32) return 0;
				const auto table = *reinterpret_cast<std::uintptr_t*>(game::Base() + kRvaShapeFunctions);
				if (!table) return 0;
				const auto fn = *reinterpret_cast<std::uintptr_t*>(table + 0x18 + std::uintptr_t(type) * 0x100);
				if (!fn || fn == game::Base() + kRvaShapeNullAabb) return 0;
				alignas(16) float aabb[8] = {};
				reinterpret_cast<void (*)(std::uintptr_t, const void*, float*)>(fn)(shape, reinterpret_cast<const void*>(game::Base() + kRvaIdentity), aabb);
				for (int i = 0; i < 4; ++i) { out.lo[i] = aabb[i]; out.hi[i] = aabb[4 + i]; }
				out.ok = true;
				return 1;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		// The game's own answer to "do these two collide?" (-1 fault / no filter, 0 no, 1 yes).
		int RadarCollides(std::uint32_t infoA, std::uint32_t infoB)
		{
			__try {
				const auto filter = *reinterpret_cast<std::uintptr_t*>(game::Base() + kRvaCollisionFilter);
				if (!filter) return -1;
				const auto r = reinterpret_cast<std::uint64_t (*)(std::uintptr_t, std::uint32_t, std::uint32_t)>(game::Base() + kRvaCollides)(filter, infoA, infoB);
				return (r & 0xFF) ? 1 : 0;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		radar::State g_radar[1 + prefetch::kSlotPilotCount];
		std::uint32_t g_radarOwnInfo[1 + prefetch::kSlotPilotCount] = {};
		// "One car" (2026-11-19): the own car's size measured from its collision box (radar::OwnSizeFrom), per pilot slot. Written by RadarStep
		// (main thread), read where the planner's own shape is filled; the model bounds stand in until it is ok for that vehicle.
		struct OwnMeasured { std::uint32_t vehicle = 0, body = 0; double halfWidth = 0, halfLength = 0, height = 0; bool ok = false; double triedAt = -1e9; };
		OwnMeasured g_ownSize[1 + prefetch::kSlotPilotCount];
		std::mutex g_ownSizeLock;
		// SR-1 (2026-11-19, user: "propose the optimal node system and then just replace entire thing at once"; surface.h): the one rule for
		// every sensed hit: can the wheels drive on it? The car's climb limit comes from the gear's grip (GT-1) and the engine; the floor is
		// the measured map's where it has one (WM-1), else `fallbackFloor`. REPLACED in this build: TW-1 (terrain face at a fixed 45 deg),
		// TP-1/2/3 (the down-ray wall check and its cache), RadarRoadLearn (RT-1), the radar judge's floor-map vote and the B1 / RT-1
		// eases-only / may-stop world truth. Drivable hits give the plan nothing; solid and unknown hits keep their real shape.
		surface::Car SurfaceCar(int idx) {
			surface::Car c; const int gear = idx >= 0 && idx <= prefetch::kSlotPilotCount ? GearOfSlot(idx) : 0;
			c.gripMult = wheelgrip::PlanGrip(gear, g_gripSet); return c;
		}
		surface::Verdict SurfaceOf(int idx, double x, double y, double z, bool haveNormal, double nz, bool trusted,
                                          double sweepFloor = std::numeric_limits<double>::quiet_NaN(), bool floorProofLane = false) {
            surface::Hit h; h.haveNormal = haveNormal; h.upDot = nz; h.normalTrusted = trusted;
            const auto car = SurfaceCar(idx);
            double fz = 0;
            const int mv = routesurvey::MapVerdict(idx, x, y, z, radar::rules::floorTol, planbranch::rules::wantWindow, fz);
            h.haveFloor = mv == 1 || mv == 2;
            if (!h.haveFloor && std::isfinite(sweepFloor)) {
                if (!floorProofLane) { fz = sweepFloor; h.haveFloor = true; }
                // Keep near-floor continuity when the cell has no survey evidence.
                // Elsewhere, let the true hit stay unresolved for exact-point checks.
                else if (mv == 0 && surface::SweepFloorSupports(z - sweepFloor, car))
                    return surface::Verdict::Drivable;
            }
            if (mv == 3 && trusted) {
                double above = 0, under = 0;
                const double lip = carcap::kCar.climb, carH = planbranch::rules::wantWindow;
                if (routesurvey::FloorReachAt(idx, x, y, z + (lip + 4.0) * .5, (4.0 - lip) * .5, above) && above - z > lip &&
                    !routesurvey::FloorReachAt(idx, x, y, z + (lip - carH) * .5, (carH + lip) * .5, under)) {
                    fz = above;
                    h.haveFloor = true;
                }
            }
            h.heightOverFloor = z - fz;
            return surface::Judge(h, car);
        }
		struct SurfaceStat { long drivable = 0, solid = 0, unknown = 0, below = 0; double logAt = -1; };
		SurfaceStat g_surfaceStat[1 + prefetch::kSlotPilotCount];
		void SurfaceCount(int idx, surface::Verdict v, std::uint32_t self, double now) {
			if (idx < 0 || idx > prefetch::kSlotPilotCount) return; auto& s = g_surfaceStat[idx];
			(v == surface::Verdict::Drivable ? s.drivable : v == surface::Verdict::Solid ? s.solid : v == surface::Verdict::Below ? s.below : s.unknown)++;
			if (now - s.logAt >= 1.0 || now < s.logAt) { const auto car = SurfaceCar(idx);
				logger::Write("OBS system=world.surface vehicle=%08X pilot=%d drivable=%ld solid=%ld unknown=%ld below=%ld max_grade=%.2f climb_deg=%.1f step_m=%.2f reason=one_surface_rule_every_hit",
					self, idx, s.drivable, s.solid, s.unknown, s.below, surface::MaxGrade(car), std::atan(surface::MaxGrade(car)) * 57.29578, car.climb);
				s = SurfaceStat{}; s.logAt = now; }
		}
		unsigned long long g_radarFrame = 0; int g_radarFrameRays = 0, g_radarFrameSweeps = 0;
		constexpr int kRadarFrameRays = 32;    // all vehicles together, per frame
		constexpr int kRadarFrameSweeps = 48;  // path-sweep pieces, all vehicles together, per frame (a vehicle's pass is all its pieces or none)
		// One hit on a body, from a ray or the path sweep: a known static body is refreshed; a new (or moving) one is looked up (the game's
		// collision test + the body's own box). A big body, one the car stands in, or one with no box gives its hit point instead of a box.
		void RadarHit(int idx, std::uint32_t self, radar::State& st, std::uintptr_t instance, std::uint32_t body, std::uint32_t owner, std::uint32_t layer,
			double hx, double hy, double hz, bool normalOk, double nz, double carX, double carY, double base, double now, const char* by)
		{
			for (int i = 0; i < st.count; ++i) if (st.boxes[i].body == body && !st.boxes[i].dynamic) {
				auto& b = st.boxes[i]; b.lastHit = now; ++b.hits; b.hitX = hx; b.hitY = hy; b.hitZ = hz;
#ifndef FVO_RELEASE
                b.sensor = by;
#endif
				if (b.collides) radar::NotePoint(st, body, owner, int(layer), hx, hy, hz, normalOk, nz, now, by);  // every hit kept (SR-1 judged it before)
				return;
			}
			radar::Lookup L; const int lr = RadarLookup(instance, body, L); ++st.stats.lookups;
			radar::Box b; b.body = body; b.owner = owner; b.layer = int(layer);
#ifndef FVO_RELEASE
            b.sensor = by;
#endif
			if (lr != 1 || !radar::BoxFrom(L, hx, hy, hz, b)) {  // no box: keep the hit point so nothing is lost
				++st.stats.lookupFailed; b.big = true; b.hitX = hx; b.hitY = hy; b.hitZ = hz; b.cx = hx; b.cy = hy; b.dynamic = false;  // 2026-11-23: do not re-lookup a failing static body every ray hit
				static ULONGLONG failLog = 0; if (GetTickCount64() - failLog > 2000) { failLog = GetTickCount64();
					logger::Write("OBS system=world.radar vehicle=%08X pilot=%d event=lookup_failed by=%s body=%08X layer=%u shape_type=%d result=%d owner=%08X pos=%.1f,%.1f,%.1f reason=%s",
						self, idx, by, body, layer, L.shapeType, lr, owner, hx, hy, hz, lr < 0 ? "fault_guarded" : "no_body_or_no_box_function_kept_as_point"); }
			} else {
				b.info = L.info; b.dynamic = L.dynamic; (b.convention ? st.stats.conventionB : st.stats.conventionA)++;
				b.shapeType = L.shapeType; if (!radar::BoxIsSolid(L.shapeType)) b.big = true;  // a mesh / compound / LOD shape: its real hit points, not its bounding box
				const int c = g_radarOwnInfo[idx] ? RadarCollides(g_radarOwnInfo[idx], L.info) : -1;
				b.collides = c != 0;  // unknown (no own info or fault): assume it collides (never drop a solid on doubt)
				(b.collides ? st.stats.collides : st.stats.passes)++; if (b.big) ++st.stats.big;
				logger::Write("OBS system=world.radar vehicle=%08X event=box by=%s pilot=%d body=%08X owner=%08X layer=%u collides=%d test=%d dynamic=%d shape_type=%d box=%.2fx%.2fx%.2f heading=%.0f centre=%.1f,%.1f z=%.2f..%.2f big=%d solid_box=%d hit_to_box=%.2f normal_z=%.2f normal_ok=%d layout=%s reason=hit_looked_up_game_collision_test_and_body_box",
					self, by, idx, body, owner, layer, int(b.collides), c, int(b.dynamic), L.shapeType, 2 * b.halfL, 2 * b.halfW, b.zhi - b.zlo, b.heading * 57.29578, b.cx, b.cy, b.zlo, b.zhi, int(b.big), int(radar::BoxIsSolid(L.shapeType)), b.gap, nz, int(normalOk), b.convention ? "rows" : "columns");
			}
			radar::Box* nb = radar::Note(st, b, now);
			if (b.collides) radar::NotePoint(st, body, owner, int(layer), hx, hy, hz, normalOk, nz, now, by); (void)nb;
		}
		// Air evidence (radar.h NoteAir): a line that touched none of `touched` crosses listed boxes' cores. A box proven air enough times
		// becomes its hit points (traced: event=airy).
		void RadarAir(int idx, std::uint32_t self, radar::State& st, double x0, double y0, double z0, double x1, double y1, double z1, const std::uint32_t* touched, int nt, const char* by)
		{
			int turned[8]; const int n = radar::NoteAir(st, x0, y0, z0, x1, y1, z1, touched, nt, turned, 8);
			for (int k = 0; k < n && k < 8; ++k) { const auto& b = st.boxes[turned[k]]; int pts = 0; for (int i = 0; i < st.pointCount; ++i) pts += st.points[i].body == b.body;
				logger::Write("OBS system=world.radar vehicle=%08X event=airy by=%s pilot=%d body=%08X box=%.2fx%.2fx%.2f centre=%.1f,%.1f passes=%d points=%d line=%.1f,%.1f,%.1f->%.1f,%.1f,%.1f reason=lines_crossed_the_box_core_without_touching_the_body_hit_points_used",
					self, by, idx, b.body, 2 * b.halfL, 2 * b.halfW, b.zhi - b.zlo, b.cx, b.cy, b.air, pts, x0, y0, z0, x1, y1, z1); }
		}
		// The floor level nearest `ref` at (x, y) from the nav map's measured levels (within 2.5 m), else `ref`.
		double RadarFloor(double x, double y, double ref)
		{
			float lv[4]; const int nl = navmap::GroundLevels(x, y, lv, 4); double best = 2.5, z = ref;
			for (int k = 0; k < nl; ++k) if (std::fabs(lv[k] - ref) < best) { best = std::fabs(lv[k] - ref); z = lv[k]; }
			return z;
		}
		// The path sweep (2026-11-13, after the 19:14 run: the car drove into a fallen tree no ray hit on the way in). The vehicle's OWN hull
		// is swept along the path it drives now (the energy plan's published path, else straight along the nose; backwards when reversing) out
		// to its TTC reach, piece by piece, and every body it touches is a radar hit. A swept volume cannot pass something it overlaps: anything
		// in the way of the hull along that path is found. Every other pass also sweeps half a metre to one side (alternating), for the
		// follower's tracking error.
		void RadarSweep(int idx, std::uint32_t self, radar::State& st, std::uintptr_t instance, std::uint32_t ownBody, double x, double y, double base, double heading, double speed, double now, unsigned long long frame, bool floorProofLane)
		{
			if (!instance || !ownBody) return;
			double distToPlayer = 0.0;
			if (idx != 0) {
				if (const auto* ce = simtier::Context().Find(self)) distToPlayer = ce->distM;
			}
			const auto rpol = simtier::RadarBudget(idx == 0, floorProofLane, st.count > 0, distToPlayer);
			if (idx != 0 && frame - st.sweptFrame < rpol.frameInterval) return;
			const double reach = std::clamp(std::fabs(speed) * radar::rules::ttc, radar::rules::aheadMin, radar::rules::aheadMax);
			radar::Segment seg[radar::rules::sweepSegments]; int ns = 0; const char* along = "nose";
			bool backing = speed < -.3;
			if (idx < planenergy::kSlots) {
				const auto r = planenergy::Latest(idx);
				if (r.ok && r.vehicle == self && r.at && GetTickCount64() - r.at < 600 &&
					r.path.count >= 2 && (r.reverse == backing || std::fabs(speed) <= .3)) {
					backing = r.reverse;
					ns = radar::SweepPlan(r.path.x, r.path.y, r.path.count, x, y,
						heading + (backing ? radar::kPi : 0.0), reach, seg, radar::rules::sweepSegments);
					along = backing ? "reverse_plan_path" : "plan_path";
				}
			}
			if (!ns) { ns = radar::SweepPlan(nullptr, nullptr, 0, x, y, backing ? heading + radar::kPi : heading, reach, seg, radar::rules::sweepSegments); if (backing) along = "reverse"; }
			// Every pass: the path itself, and (when radar budget permits) the path shifted to one side so the Plan learns which
			// shifted lines are clear before it picks one.
			constexpr int kSides = int(sizeof radar::rules::sideOffsets / sizeof radar::rules::sideOffsets[0]);
			const bool side = rpol.sideSweep; const double sideOff = radar::rules::sideOffsets[st.sweepPass % kSides];
			const int cost = ns * (side ? 2 : 1);
			if (g_radarFrameSweeps + cost > kRadarFrameSweeps) { ++st.stats.sweepBudget; return; }
			g_radarFrameSweeps += cost; st.sweptFrame = frame; ++st.sweepPass;
			// The hull is cast from the body's origin, where it stands now over the floor `base`; each piece is shifted by the measured floor's
			// change along the way (a ramp ahead is swept along its rise) and lifted by sweepLift.
			static prefetch::SweepContact contacts[96];
			int found = 0, floorN = 0, bodies = 0, floorNoted = 0; unsigned worst = 0; double zPrev = base;
			for (int k = 0; k < ns; ++k) {
				const auto& s = seg[k]; const double z0 = k == 0 ? base : zPrev, z1 = RadarFloor(s.x1, s.y1, z0); zPrev = z1;
				for (int pass = 0; pass < (side ? 2 : 1); ++pass) {
					const double off = pass == 0 ? 0.0 : sideOff, px = std::cos(s.heading) * off, py = -std::sin(s.heading) * off;  // to the right of the piece for +off
					const float offset[3] = { float(s.x0 + px - x), float(s.y0 + py - y), float(z0 - base + radar::rules::sweepLift) };
					const float disp[3] = { float(s.x1 - s.x0), float(s.y1 - s.y0), float(z1 - z0) };
					int count = 0; unsigned us = 0;
					const int r = vehicle_probe::SweepPathNow(instance, offset, float(std::remainder(s.heading - heading + (backing ? radar::kPi : 0.0), 2 * radar::kPi)), disp, kWallLayers, contacts, 96, count, us);
					if (r < 0) { ++st.stats.sweepFault; return; }
					if (r == 0) { ++st.stats.sweepRefused; continue; }
					++st.stats.sweeps; st.stats.sweepMicros += us; worst = std::max(worst, us); st.stats.sweepMicrosMax = std::max(st.stats.sweepMicrosMax, us);
					const double len3 = std::max(.05, std::sqrt(s.length * s.length + (z1 - z0) * (z1 - z0)));
					if (pass == 1) ++st.stats.sideSweeps;
					std::uint32_t touched[96]; int nt = 0;
					for (int c = 0; c < count; ++c) if (contacts[c].bodyId != ownBody) touched[nt++] = contacts[c].bodyId;
					// The hull touched nothing else: every listed box whose core this piece's line crosses (at body height) is air there.
					RadarAir(idx, self, st, s.x0 + px, s.y0 + py, z0 + radar::rules::sweepLift + .6, s.x1 + px, s.y1 + py, z1 + radar::rules::sweepLift + .6, touched, nt, pass == 0 ? "path_sweep" : "side_sweep");
					bool clean = pass == 0 && count < 96;
					for (int c = 0; c < count; ++c) {
						const auto& h = contacts[c]; ++found; ++st.stats.contacts;
						const double floorZ = z0 + (z1 - z0) * std::clamp(double(h.distance) / len3, 0.0, 1.0);
						if (h.bodyId == ownBody) continue;
						const auto sv = SurfaceOf(idx, h.x, h.y, h.z, h.normalOk, h.nz, false, floorZ, floorProofLane);  // SR-1: unknown sweep hits retain their obstacle evidence
						SurfaceCount(idx, sv, self, now);
						if (sv == surface::Verdict::Below) continue;  // SB-1: under the swept floor: not in the way, not ground
						if (sv == surface::Verdict::Drivable) { ++floorN; ++st.stats.floor; navmap::NoteGround(h.x, h.y, h.z);  /* WM-2: hull contacts are not floor measurements: not into the map */ ++floorNoted; continue; }
						clean = false; ++bodies;
						RadarHit(idx, self, st, instance, h.bodyId, loadedarea::HitBodyOwner(h.bodyId), h.layer, h.x, h.y, h.z, h.normalOk, h.nz, x, y, base, now, "path_sweep");  // RT-1: the owner when the walk knows it
					}
					if (clean) planenergy::NoteCheckedPath(idx, self, {s.x0,s.y0,z0,s.x1,s.y1,z1,GetTickCount64()/1000.0});
				}
			}
			static double sweepLog[1 + prefetch::kSlotPilotCount]{};
			if (idx < int(sizeof sweepLog / sizeof sweepLog[0]) && (now - sweepLog[idx] >= 5.0 || now < sweepLog[idx] || (bodies > 0 && now - sweepLog[idx] >= 1.0))) { sweepLog[idx] = now;
				logger::Write("OBS system=world.radar vehicle=%08X pilot=%d event=path_sweep along=%s pieces=%d side=%.1f reach=%.1f speed=%.1f contacts=%d floor=%d floor_noted=%d bodies=%d worst_us=%u first=%.1f,%.1f->%.1f,%.1f last_end=%.1f,%.1f reason=own_hull_swept_along_the_path_every_contact_looked_up",
					self, idx, along, ns, sideOff, reach, speed, found, floorN, floorNoted, bodies, worst, seg[0].x0, seg[0].y0, seg[0].x1, seg[0].y1, seg[ns - 1].x1, seg[ns - 1].y1); }
		}
		// One static box a tick checked against the game: its body gone (or its id reused for another body) or moved, it leaves the list.
		// (Static boxes stay while in range: this is what keeps a stale one from standing in the road.)
		void RadarVerify(int idx, std::uint32_t self, radar::State& st, std::uintptr_t instance, double x, double y, double base)
		{
			const int i = radar::NextToVerify(st); if (i < 0 || !instance) return;
			radar::Box& b = st.boxes[i];
			radar::Lookup L; const int lr = RadarLookup(instance, b.body, L); ++st.stats.verified;
			if (lr < 0) return;  // a fault proves nothing: kept
			radar::Box fresh; fresh.body = b.body;
			const bool same = lr == 1 && radar::BoxFrom(L, b.hitX, b.hitY, b.hitZ, fresh, b.convention) && std::hypot(fresh.cx - b.cx, fresh.cy - b.cy) < .5 && std::fabs(fresh.zlo - b.zlo) < .5
				&& std::fabs(fresh.halfL - b.halfL) < .5 && std::fabs(fresh.halfW - b.halfW) < .5;
			if (same) return;
			++st.stats.verifyDropped;
			logger::Write("OBS system=world.radar vehicle=%08X event=dropped pilot=%d body=%08X lookup=%d box=%.2fx%.2f centre=%.1f,%.1f reason=%s",
				self, idx, b.body, lr, 2 * b.halfL, 2 * b.halfW, b.cx, b.cy, lr == 1 ? "body_moved_or_changed" : "body_gone");
			if (lr == 1) logger::Write("OBS system=world.radar vehicle=%08X event=moved pilot=%d body=%08X was=%.2fx%.2f@%.1f,%.1f,%.2f now=%.2fx%.2f@%.1f,%.1f,%.2f convention=%d",
				self, idx, b.body, 2 * b.halfL, 2 * b.halfW, b.cx, b.cy, b.zlo, 2 * fresh.halfL, 2 * fresh.halfW, fresh.cx, fresh.cy, fresh.zlo, b.convention);
			radar::Remove(st, i);
		}
		// One tick of a vehicle's radar: the rays, the path sweep, one check against the game, and the list's range rule. Main thread only.
		void RadarStep(int idx, std::uint32_t self, double x, double y, double base, double heading, double speed, double halfWidth, double halfLength, double now, bool floorProofLane)
		{
			if (!features::On(features::Id::Radar) || features::On(features::Id::ScriptMode) || idx < 0 || idx >= int(sizeof g_radar / sizeof g_radar[0]) || !routesurvey::OnMainThread()) return;
			auto& st = g_radar[idx]; auto& sh = g_shs[idx];
			const std::uintptr_t instance = sh.instance.load(std::memory_order_relaxed); const std::uint32_t ownBody = sh.bodyId.load(std::memory_order_relaxed);
			const unsigned long long frame = routesurvey::Frame(); if (frame != g_radarFrame) { g_radarFrame = frame; g_radarFrameRays = 0; g_radarFrameSweeps = 0; }
			if (instance && ownBody && !g_radarOwnInfo[idx]) { radar::Lookup own; if (RadarLookup(instance, ownBody, own) == 1) g_radarOwnInfo[idx] = own.info; }
			if (instance && ownBody && self) {  // "one car": measure the own body's box once per vehicle/body; retry every 2 s while it fails
				OwnMeasured m; { std::lock_guard<std::mutex> g(g_ownSizeLock); m = g_ownSize[idx]; }
				const bool fresh = m.vehicle != self || m.body != ownBody;
				if (fresh || (!m.ok && (now - m.triedAt >= 2.0 || now < m.triedAt))) {
					radar::Lookup L; const int lr = RadarLookup(instance, ownBody, L); radar::OwnSize os;
					const bool ok = lr == 1 && radar::OwnSizeFrom(L, heading, os);
					OwnMeasured n; n.vehicle = self; n.body = ownBody; n.triedAt = now; n.ok = ok;
					if (ok) { n.halfWidth = os.halfWidth; n.halfLength = os.halfLength; n.height = os.height; }
					{ std::lock_guard<std::mutex> g(g_ownSizeLock); g_ownSize[idx] = n; }
					if (!ok) logger::Write("OBS system=vehicle.size vehicle=%08X pilot=%d event=measure_failed body=%08X lookup=%d shape_type=%d box=%.2fx%.2fx%.2f half_width=%.2f half_length=%.2f reason=%s",
						self, idx, ownBody, lr, L.shapeType, double(L.hi[0] - L.lo[0]), double(L.hi[1] - L.lo[1]), double(L.hi[2] - L.lo[2]), os.halfWidth, os.halfLength,
						lr < 0 ? "fault_guarded" : lr == 0 ? "no_body_or_no_box_function" : "implausible_car_size_bounds_kept");
					else logger::Write("OBS system=vehicle.size vehicle=%08X pilot=%d event=measured body=%08X shape_type=%d box=%.2fx%.2fx%.2f layout=%s half_width=%.2f half_length=%.2f height=%.2f reason=own_collision_box_along_and_across_heading",
						self, idx, ownBody, L.shapeType, double(L.hi[0] - L.lo[0]), double(L.hi[1] - L.lo[1]), double(L.hi[2] - L.lo[2]), os.convention ? "rows" : "columns", os.halfWidth, os.halfLength, os.height);
				}
			}
			radar::Ray rays[radar::rules::rotateRays + radar::rules::forwardRays];
			const int nr = radar::Plan(st, x, y, base, heading, speed, halfWidth, halfLength, rays, int(sizeof rays / sizeof rays[0]));
			for (int i = 0; i < nr && g_radarFrameRays < kRadarFrameRays; ++i) {
				prefetch::RayHit hit; ++g_radarFrameRays; ++st.stats.rays;
				if (vehicle_probe::CastNow(rays[i].from, rays[i].to, kWallLayers, hit) != 1) continue;
				{   // the ray's free stretch (to its hit, or all of it) is air in every box core it crosses, except the body it hit
					const float* e = hit.hit ? nullptr : rays[i].to; const double ex = e ? e[0] : hit.x, ey = e ? e[1] : hit.y, ez = e ? e[2] : hit.z;
					const std::uint32_t t = hit.hit ? hit.bodyId : 0;
					RadarAir(idx, self, st, rays[i].from[0], rays[i].from[1], rays[i].from[2], ex, ey, ez, &t, hit.hit ? 1 : 0, rays[i].kind ? "forward_ray" : "rotating_ray"); }
				if (!hit.hit) continue;
				++st.stats.hits;
				if (hit.bodyId == ownBody) { ++st.stats.ownBody; continue; }
				{   const auto sv = SurfaceOf(idx, hit.x, hit.y, hit.z, hit.normalOk, hit.nz, true);  // SR-1: the ray's normal and the map's floor
					SurfaceCount(idx, sv, self, now);
					if (sv == surface::Verdict::Drivable) { ++st.stats.floor; continue; }
					if (sv == surface::Verdict::Below) continue; }  // SB-1: under the car's floor: not a radar box
				RadarHit(idx, self, st, instance, hit.bodyId, hit.formId, hit.layer, hit.x, hit.y, hit.z, hit.normalOk, hit.nz, x, y, base, now, rays[i].kind ? "forward_ray" : "rotating_ray");
			}
			RadarSweep(idx, self, st, instance, ownBody, x, y, base, heading, speed, now, frame, floorProofLane);
			RadarVerify(idx, self, st, instance, x, y, base);
			radar::Expire(st, now, x, y);
			if (now - st.logAt >= 2.0 || now < st.logAt) { st.logAt = now; const auto& S = st.stats;
				logger::Write("OBS system=world.radar vehicle=%08X pilot=%d event=summary rays=%ld hits=%ld lookups=%ld lookup_failed=%ld collides=%ld passes_through=%ld ground=%ld floor=%ld big=%ld floor_bodies=%ld own_body=%ld layout_columns=%ld layout_rows=%ld boxes=%d points=%d own_info=%08X sweeps=%ld sweep_refused=%ld sweep_fault=%ld sweep_budget_skips=%ld contacts=%ld side_sweeps=%ld air_passes=%ld airy=%ld sweep_us_avg=%.0f sweep_us_max=%u verified=%ld verify_dropped=%ld points_dropped=%ld drop_far_m=%.1f drop_near_m=%.1f refusal=%s reason=rays_and_path_sweep_per_vehicle",
					self, idx, S.rays, S.hits, S.lookups, S.lookupFailed, S.collides, S.passes, S.ground, S.floor, S.big, S.floorBodies, S.ownBody, S.conventionA, S.conventionB, st.count, st.pointCount, g_radarOwnInfo[idx],
					S.sweeps, S.sweepRefused, S.sweepFault, S.sweepBudget, S.contacts, S.sideSweeps, S.airPasses, S.airy, S.sweeps ? S.sweepMicros / double(S.sweeps) : 0.0, S.sweepMicrosMax, S.verified, S.verifyDropped, S.pointsDropped, S.dropFar, S.pointsDropped ? S.dropNear : 0.0, vehicle_probe::RefusalName(vehicle_probe::LastRefusal())); }
		}
		// This vehicle's radar boxes appended to a World shape list (the energy plan's input).
        std::mutex g_floorProofLock;
        std::array<surface::VerifiedFloorPoint, 8> g_floorProofs{};
        std::size_t g_nextFloorProof = 0;

        void ProbeBlockerFloor(std::uint32_t vehicle, double x, double y, double base) {
            if (!routesurvey::OnMainThread()) return;
            int gateWhy = 0;
            if (!vehicle_probe::GateOpen(gateWhy)) return;
            const auto request = planenergy::TakeFloorProbe();
            if (!request) return;
            const auto& hit = *request;
            const double now = GetTickCount64() / 1000.0;
            if (hit.vehicle != vehicle || now < hit.snapshot || now - hit.snapshot > .75 ||
                std::hypot(x - hit.carX, y - hit.carY) > 1.0 || std::fabs(base - hit.carZ) > .5) return;
            constexpr float d = float(surface::kFloorProbeOffset);
            const float offsets[5][2] = {{0, 0}, {-d, 0}, {d, 0}, {0, -d}, {0, d}};
            surface::FloorSample samples[5];
            int measured = 0, connected = 0, sameBody = 0, anchor = -1;
            for (int i = 0; i < 5; ++i) {
                const float px = float(hit.x) + offsets[i][0], py = float(hit.y) + offsets[i][1];
                const float from[3] = {px, py, float(hit.z + 1.0)}, to[3] = {px, py, float(hit.z - 2.5)};
                prefetch::RayHit ray{};
                const int result = vehicle_probe::CastNow(from, to, 0x22806, ray);
                if (result == 1 && ray.hit && ray.normalOk && ray.nz > .7) {
                    double mapped = 0;
                    const bool sameLevel = routesurvey::FloorReachAt(0, px, py, ray.z, .45, mapped) && std::fabs(mapped - ray.z) <= .25;
                    samples[i] = {true, sameLevel, ray.z, ray.bodyId};
                }
                measured += samples[i].measured;
                connected += samples[i].connected;
                if (samples[i].connected && samples[i].body == hit.body) {
                    ++sameBody;
                    if (anchor < 0) anchor = i;
                }
                logger::Write("OBS system=world.floor_confidence vehicle=%08X snapshot_s=%.6f event=sample sample=%d xy=%.3f,%.3f answered=%d hit=%d floor=%d z=%.3f normal_z=%.3f body=%08X connected=%d",
                    vehicle, hit.snapshot, i, px, py, result, int(ray.hit), int(samples[i].measured), ray.z, ray.nz, ray.bodyId, int(samples[i].connected));
            }
            const double lower = hit.z - hit.carHeight;
            const double upper = std::min(hit.z + .25, samples[0].z - .35);
            double alternate = 0;
            const bool competing = samples[0].measured && upper > lower &&
                routesurvey::FloorAt(hit.x, hit.y, (upper + lower) / 2, (upper - lower) / 2, alternate) &&
                alternate >= lower && alternate <= upper;
            const auto fit = surface::FitProbedFloor(samples, hit.z, hit.body, competing, SurfaceCar(0));
            logger::Write("OBS system=world.floor_confidence vehicle=%08X snapshot_s=%.6f event=verdict body=%08X at=%.3f,%.3f,%.3f measured=%d connected=%d same_body=%d fitted=%d local_floor=%.3f error=%.3f competing_floor=%d clear=%d",
                vehicle, hit.snapshot, hit.body, hit.x, hit.y, hit.z, measured, connected,
                sameBody, int(fit.fitted), fit.height, fit.error, int(competing), int(fit.clear));
            if (!fit.clear) return;
            if (anchor < 0) for (int i = 0; i < 5 && anchor < 0; ++i) if (samples[i].connected) anchor = i;  // FC-2: any connected sample
            if (anchor < 0) return;
            std::lock_guard lock(g_floorProofLock);
            g_floorProofs[g_nextFloorProof++ % g_floorProofs.size()] = {vehicle, hit.body, hit.x, hit.y, hit.z, now,
                hit.x + offsets[anchor][0], hit.y + offsets[anchor][1], samples[anchor].z};
        }


        // RF-1 (2026-11-19): World gives every static shape near the car the road surface under it (routesurvey::RoadFit: the car's
        // reachable floor 2 m round it, outliers set aside). A surface whose own pattern spreads more than half a wheel's climb is rough,
        // not a road pattern, and is not given. Movers, and boxes wider than 2 m (their tiles take the lane floor in Index), are left.
        struct RoadFitStat { long fitted = 0, rough = 0, weak = 0, none = 0, overClimb = 0; double logAt = -1; };
        RoadFitStat g_roadFitStat[1 + prefetch::kSlotPilotCount];
        void AssignRoad(int idx, std::uint32_t self, std::vector<planworld::Shape>& shapes, double x, double y, double carHeight, double now) {
            const double climb = bicycle::Spec{}.climb;
            auto& st = g_roadFitStat[std::clamp(idx, 0, int(prefetch::kSlotPilotCount))];
            for (auto& s : shapes) {
                if (s.source == planworld::Source::Vehicle || s.source == planworld::Source::Person) continue;
                if (std::max(s.halfWidth, s.halfLength) > 1.0 || std::hypot(s.centre.x - x, s.centre.y - y) > 45.0) continue;
                routesurvey::RoadSurface r;
                if (!routesurvey::RoadFit(idx, s.centre.x, s.centre.y, planworld::Base(s), carHeight + .5, r)) { ++st.none; continue; }
                if (!r.ok) { ++st.weak; continue; }
                if (r.spread > climb * .5) { ++st.rough; continue; }
                s.road = r.z; s.roadSpread = r.spread; ++st.fitted;
                if (planworld::Top(s) - r.z > climb) ++st.overClimb;
            }
            if (now - st.logAt >= 2.0 || now < st.logAt) {
                st.logAt = now;
                logger::Write("OBS system=world.road_fit vehicle=%08X pilot=%d fitted=%ld rough=%ld weak=%ld none=%ld over_climb=%ld",
                    self, idx, st.fitted, st.rough, st.weak, st.none, st.overClimb);
                st = RoadFitStat{}; st.logAt = now;
            }
        }

        bool ApplyFloorProofs(std::uint32_t vehicle, std::vector<planworld::Shape>& shapes) {
            const double now = GetTickCount64() / 1000.0;
            std::array<surface::VerifiedFloorPoint, 8> proofs;
            { std::lock_guard lock(g_floorProofLock); proofs = g_floorProofs; }
            const auto removed = std::erase_if(shapes, [&](const auto& s) {
                if (s.source != planworld::Source::Ray || !s.floorProbeBody) return false;
                return std::any_of(proofs.begin(), proofs.end(), [&](const auto& p) {
                    double reachable = 0;
                    return p.Matches(vehicle, s.floorProbeBody, s.centre.x, s.centre.y, s.centre.z, now) &&
                        routesurvey::FloorReachAt(0, p.anchorX, p.anchorY, p.anchorZ, .45, reachable) &&
                        std::fabs(reachable - p.anchorZ) <= .25;
                });
            });
            logger::Observe("plan.floor_confidence", vehicle, removed, "event=snapshot_filter removed=%zu", removed);
            return removed != 0;
        }

		void RadarAppend(int idx, std::vector<planworld::Shape>& shapes, double now, double connectedFloorCarHeight = 0)
		{
			if (!features::On(features::Id::Radar) || features::On(features::Id::ScriptMode) || idx < 0 || idx >= int(sizeof g_radar / sizeof g_radar[0])) return;
			const auto& st = g_radar[idx];
			planworld::Shape tmp[radar::kMaxBoxes];
			const int n = radar::ToShapes(st, now, tmp, radar::kMaxBoxes);
			bool below[radar::kMaxBoxes]{};
			double floorZ[radar::kMaxBoxes]{};
			if (n && connectedFloorCarHeight > 0) {
				routesurvey::CheckRadarPoints(idx, tmp, n, SurfaceCar(idx).climb, connectedFloorCarHeight, below, floorZ);
			}

			// A live static body can outlast an individual obstructing point. Reclassify the
			// exported sample without erasing it, so another floor can make it relevant again.
			int dropped = 0, example = -1;
			for (int i = 0; i < n; ++i) {
				if (below[i]) { ++dropped; example = i; }
				else shapes.push_back(tmp[i]);
			}
			static double logAt[1 + prefetch::kSlotPilotCount] = {};
			if (now - logAt[idx] >= 5.0 || now < logAt[idx]) {  // PF-1: every 5 s (was 1 s)
				logAt[idx] = now;
				logger::Write("OBS system=world.radar pilot=%d event=shapes solids=%d dropped_below=%d reason=point_local_floor", idx, n - dropped, dropped);
#ifndef FVO_RELEASE
				if (example >= 0) {
					const auto& s = tmp[example];
					logger::Write("OBS system=world.radar pilot=%d event=below_point sensor=%s owner=%08X body=%08X at=%.3f,%.3f,%.3f floor=%.3f age_s=%.3f",
						idx, s.sensor, s.owner, s.sensorBody, s.centre.x, s.centre.y, s.centre.z, floorZ[example], now - s.seenAt);
				}
#endif
			}
		}

		// When a lane car last handed the energy plan a lane snapshot (s): such a car is not a goal car this update (EnergyStep).
		double g_shadowLaneAt[planenergy::kSlots]{};
		// A car with no lane: World shapes (kept ray hits, kept collision boxes, vehicles and people) and measured floor levels along the way
		// from the car to its goal (at most the sub-goal reach), the same sources the lane gathers along its line.
		void GoalWorld(std::uint32_t self, double x, double y, double base, double gx, double gy, double now,
			std::vector<planworld::Shape>& shapes, std::vector<planinput::FloorPoint>& floor)
		{
			shapes.clear(); floor.clear(); constexpr std::size_t kMax = 160; constexpr int kLevels = 4;
			const double dist = std::hypot(gx - x, gy - y), reach = std::min(dist, planscene::Scene::kSubGoal + 4.0);
			const double ux = dist > 1e-3 ? (gx - x) / dist : 0.0, uy = dist > 1e-3 ? (gy - y) / dist : 1.0;
			floor.push_back({ x, y, base });
			for (double a = 0; a <= reach + 1e-6; a += 2.0) for (double l = -6; l <= 6; l += 2.0) {  // floor: the level nearest the car's base
				const double px = x + ux * a + uy * l, py = y + uy * a - ux * l; float lv[kLevels]; const int nl = navmap::GroundLevels(px, py, lv, kLevels);
				double best = 2.5, z = 0; bool got = false; for (int k = 0; k < nl; ++k) if (std::fabs(lv[k] - base) < best) { best = std::fabs(lv[k] - base); z = lv[k]; got = true; }
				if (got) floor.push_back({ px, py, z });
			}
			for (double a = 0; a <= reach + 1e-6 && shapes.size() < kMax; a += 4.0) {
				const double px = x + ux * a, py = y + uy * a;
				navmap::KeptHit kh[32]; const int n = navmap::HitsNear(px, py, base - 1.0, base + 3.5, 6.0, kh, 32);
				for (int k = 0; k < n && shapes.size() < kMax; ++k) {
					bool dup = false; for (const auto& s : shapes) if (std::fabs(s.centre.x - kh[k].x) < .05 && std::fabs(s.centre.y - kh[k].y) < .05 && std::fabs(s.centre.z - kh[k].z) < .05) { dup = true; break; }
					if (dup) continue;
					planworld::Shape s; s.source = planworld::Source::Ray; s.centre = { kh[k].x, kh[k].y, kh[k].z }; s.owner = kh[k].owner; s.seenAt = now - kh[k].age;
					s.layer = kh[k].layer == 255 ? -1 : int(kh[k].layer); s.haveNormal = kh[k].haveNormal; s.normalZ = kh[k].up; shapes.push_back(s);
				}
				navmap::KeptBox kb[8]; const int nb = navmap::BoxesNear(px, py, 6.0, kb, 8);
				for (int k = 0; k < nb && shapes.size() < kMax; ++k) {
					const auto& b = kb[k]; if (b.zHi <= base + .35 || b.hx > planbranch::rules::wholeBoxMaxHalf || b.hy > planbranch::rules::wholeBoxMaxHalf) continue;
					bool dup = false; for (const auto& s : shapes) if (s.source == planworld::Source::Box && std::fabs(s.centre.x - b.x) < .05 && std::fabs(s.centre.y - b.y) < .05) { dup = true; break; }
					if (dup) continue;
					planworld::Shape s; s.source = planworld::Source::Box; s.centre = { b.x, b.y, .5 * (b.zLo + b.zHi) }; s.halfWidth = b.hx; s.halfLength = b.hy; s.halfHeight = .5 * (b.zHi - b.zLo);
					s.heading = b.yaw; s.seenAt = now; shapes.push_back(s);
				}
			}
			hazardfield::AppendNow(.5 * (x + gx), .5 * (y + gy), base - 1.0, base + 3.5, .5 * reach + 6.0, now, shapes, kMax + 16);  // a route's hazard boxes
			const auto& world = WorldThisFrame(now);  // vehicles and people (D4: moving ones become movers in the scene); World excludes the asking car
			planworld::Shape nearby[48]; const int nn = world.Near(x, y, reach + 10.0, self, nearby, 48);
			for (int k = 0; k < nn && shapes.size() < kMax; ++k) shapes.push_back(nearby[k]);
		}
		// O3 (2026-11-14, replaces the brake whenever 0.15 m/s over the ask): the speed the actuators follow is the plan's own speed 0.5 s ahead,
		// eased by the car's acceleration (6 m/s^2) and braking limits; the brake only when the car is 1.5 m/s over that or the plan stops.
		double g_askSpeed[1 + prefetch::kSlotPilotCount] = {}, g_askAt[1 + prefetch::kSlotPilotCount] = {};
		void FollowSpeed(int idx, double target, double now, double brakeLimit, double actual, double& speed, bool& brake)
		{
			if (idx < 0 || idx >= int(sizeof g_askSpeed / sizeof g_askSpeed[0])) { speed = target; brake = target < .3 || actual > target + 1.5; return; }
			double& a = g_askSpeed[idx]; double& at = g_askAt[idx]; const double dt = at > 0 && now > at ? std::min(.5, now - at) : 0.0; at = now;
			if (target < .3) a = 0;  // a stop is followed at once
			else if (dt <= 0) a = std::min(target, std::max(a, actual));
			else a += std::clamp(target - a, -std::max(2.5, brakeLimit) * dt, 6.0 * dt);
			speed = a; brake = a < .3 || actual > a + 1.5;
		}
		// BO-1 (2026-11-19, Speed, user: "in cases where the control has an issue moving forward fast enough or with enough thrust it can just use
		// boost"): the possible speed with the game's forward boost. Called where FollowSpeed gives the plan's speed (lane and goal scenes) and writes
		// only Output::boost; the decision is boostneed.h with the numbers in planbranch::rules; the actuator in Fly() executes it. The evidence is
		// measured: the speed along the nose, the tyre pedal's demand (published by the actuator), the game's own boost charge (read after the
		// native boost update), the rear slip. A hold, a stop line, a brake request, a stop or a reverse never boosts (boostneed.h Decide).
		boostneed::State g_boostState[1 + prefetch::kSlotPilotCount];
		struct BoostLog { double at = 0; const char* why = ""; };
		BoostLog g_boostLog[1 + prefetch::kSlotPilotCount];
		void BoostSpeed(int idx, std::uint32_t self, drive::Output& out, bool onWheels, bool held, double stopDistance, double wheelVote,
			double forward, double steerAbs, double now, bool hover = false, const char* hoverHalt = nullptr)
		{
			if (idx < 0 || idx >= int(sizeof g_boostState / sizeof g_boostState[0])) { out.boost = false; return; }
			const ULONGLONG tick = GetTickCount64();
			boostneed::Sample s; s.now = now; s.wanted = out.speed; s.fwd = forward; s.onWheels = onWheels && out.mode == drive::Mode::Drive;
			s.reverse = out.reverse; s.brake = out.brake; s.held = held; s.stopDistance = stopDistance; s.wheelVote = wheelVote; s.steerAbs = steerAbs;
			s.slipDeg = !hover && std::fabs(forward) > 4.0 ? std::fabs(double(g_rearSlipDeg[idx].load(std::memory_order_relaxed))) : 0.0;
			const ULONGLONG pedalAt = g_pedalAt[idx].load(std::memory_order_relaxed), meterAt = g_boostSeenAt[idx].load(std::memory_order_relaxed);
			s.pedalFresh = pedalAt && tick >= pedalAt && tick - pedalAt <= 300; s.pedal = g_pedalDemand[idx].load(std::memory_order_relaxed);
			if (hover) {  // BO-2: a flying car: no wheels; the hover throttle, the speed the hover may reach and the precise-flight reason are the actuator's measured facts
				s.hover = true; s.onWheels = false; s.slipDeg = 0.0; s.wheelVote = std::numeric_limits<double>::infinity();
				s.hoverHalt = hoverHalt ? hoverHalt : s.pedalFresh ? g_hoverHalt[idx].load(std::memory_order_relaxed) : nullptr;
				s.capSpeed = s.pedalFresh ? double(g_hoverCap[idx].load(std::memory_order_relaxed)) : std::numeric_limits<double>::infinity();
				s.steerAbs = s.pedalFresh ? double(g_hoverSteer[idx].load(std::memory_order_relaxed)) : 0.0;
			}
			s.meterFresh = meterAt && tick >= meterAt && double(tick - meterAt) <= planbranch::rules::boostSampleAge * 1000.0;
			s.charge = g_boostCharge[idx].load(std::memory_order_relaxed); s.nativeOn = g_boostNative[idx].load(std::memory_order_relaxed);
			auto& st = g_boostState[idx];
			const auto d = boostneed::Decide(st, s);
			out.boost = d.boost;
			auto& lg = g_boostLog[idx]; const bool waiting = !d.boost && (!std::strncmp(d.why, "meter_", 6) || !std::strcmp(d.why, "cooling_off") || !std::strcmp(d.why, "thrust_limited_waiting"));
			if (d.changed || (waiting && (d.why != lg.why || now - lg.at >= 2.0 || now < lg.at))) {
				lg.at = now; lg.why = d.why;
				logger::Write("OBS system=drive.boost vehicle=%08X pilot=%d mode=%s event=%s boost=%d reason=%s wanted=%.2f forward=%.2f shortfall=%.2f pedal=%.2f pedal_fresh=%d gain=%.2f charge=%.2f native_on=%d meter_fresh=%d held=%d stop_line=%.0f wheel_vote=%.2f steer=%.2f slip=%.1f cap=%.1f",
					self, idx, hover ? "hover" : "wheels", d.changed ? (d.boost ? "start" : "stop") : "waiting", int(d.boost), d.why, s.wanted, s.fwd, s.wanted - s.fwd, s.pedal, int(s.pedalFresh), st.accel,
					s.charge, int(s.nativeOn), int(s.meterFresh), int(held), stopDistance, std::isfinite(s.wheelVote) ? s.wheelVote : -1.0, s.steerAbs, s.slipDeg, std::isfinite(s.capSpeed) ? s.capSpeed : -1.0);
			}
		}
		// The energy by term (2026-11-14: the 15:12 crawl at energy 110 with 406 of 408 moves possible did not say which cost held it).
		const char* TermsText(const energyplan::Terms& t, char* out, std::size_t n)
		{
			std::snprintf(out, n, "togo=%.1f progress=%.1f lateral=%.1f speed=%.1f collision=%.1f field=%.1f slope=%.1f gear=%.1f soft=%.1f movers=%.1f low_edge=%.1f hover=%.1f modes=%.1f facing=%.1f end_free=%.1f",
				t.togo, t.progress, t.lateral, t.speed, t.collision, t.field, t.slope, t.gear, t.soft, t.movers, t.lowEdge, t.hover, t.modes, t.facing, t.endFree);
			return out;
		}
		// CT-1: what the plan weighed: candidates rejected per rule, and the cheapest lane-following plan at each speed (its energy and main
		// terms, or the rule that rejected it). Beside your car's plan.energy line and every plan.blocked line.
		void TraceChoices(std::uint32_t self, int idx, const planenergy::Result& r, const char* when)
		{
			char rj[400]; std::size_t o = 0; rj[0] = 0;
			for (int q = 0; q < r.rejectKinds && o < sizeof rj - 64; ++q) o += std::snprintf(rj + o, sizeof rj - o, "%s%s:%d", q ? "," : "", r.rejectName[q] ? r.rejectName[q] : "?", r.rejectCount[q]);
			char ls[900]; o = 0; ls[0] = 0;
			for (int q = 0; q < r.laneSpeedCount && o < sizeof ls - 160; ++q) { const auto& L = r.laneSpeeds[q];
				if (std::isfinite(L.energy)) o += std::snprintf(ls + o, sizeof ls - o, "%s%.1f:E=%.1f(lateral=%.1f,field=%.1f,togo=%.1f,collision=%.1f,speed=%.1f,hover=%.1f,end_free=%.1f)", q ? "|" : "", L.speed, L.energy,
					L.terms.lateral, L.terms.field, L.terms.togo, L.terms.collision, L.terms.speed, L.terms.hover, L.terms.endFree);
				else o += std::snprintf(ls + o, sizeof ls - o, "%s%.1f:rejected_%s", q ? "|" : "", L.speed, L.reject && *L.reject ? L.reject : "unnamed"); }
			logger::Write("OBS system=plan.choices vehicle=%08X pilot=%d when=%s candidate=\"%s\" energy=%.1f clearance_tier=%d wheel_vote_speed=%.2f wheel_vote_reason=%s feasible=%d/%d rejected={%s} lane_speeds={%s} raw_start_grade=%.2f start_grade=%.2f grade_points=%d grade_set_aside=%d max_grade=%.2f floor_set_aside=%d reason=what_the_plan_weighed",
				self, idx, when, r.why ? r.why : "", r.energy, r.terms.clearanceTier, r.terms.wheelSpeed, r.terms.wheelReason, r.feasible, r.candidates, rj, ls, r.startGrade.raw, r.startGrade.grade, r.startGrade.points, r.startGrade.setAside, r.maxGrade, r.floorSetAside);
		}
		// O1 / O4: a car's committed plan replaced (or a challenger held off by the direction hold), traced per car.
		void TraceSwitch(int idx, std::uint32_t self, const planenergy::Result& r, const char* scene)
		{
			static std::uint64_t seen[1 + prefetch::kSlotPilotCount] = {}; if (idx < 0 || idx >= int(sizeof seen / sizeof seen[0]) || seen[idx] == r.at) return; seen[idx] = r.at;
			{   // HC-3: once a second, how many plans were kept and how many switches went through each rule (the hold working: mostly kept, few instant releases)
				static int last[1 + prefetch::kSlotPilotCount][6] = {}; static double lastAt[1 + prefetch::kSlotPilotCount] = {};
				const double t = GetTickCount64() / 1000.0;
				if (t - lastAt[idx] >= 1.0) { int dlt[6]; bool any = false; for (int q = 0; q < 6; ++q) { dlt[q] = std::max(0, r.rel[q] - last[idx][q]); any |= dlt[q] && q < 5; last[idx][q] = r.rel[q]; }
					if (any && lastAt[idx] > 0) logger::Write("OBS system=plan.switch vehicle=%08X pilot=%d scene=%s event=summary seconds=%.1f kept=%d got_worse=%d too_close=%d enters_solid=%d yields=%d timer=%d reason=switches_per_rule_since_last_summary",
						self, idx, scene, t - lastAt[idx], dlt[5], dlt[0], dlt[1], dlt[2], dlt[3], dlt[4]);
					lastAt[idx] = t; } }
			if (r.kept && std::strcmp(r.switchWhy, "direction_near_tie_kept") != 0) return;
			logger::Write("OBS system=plan.switch vehicle=%08X pilot=%d scene=%s event=%s candidate=\"%s\" rival=\"%s\" gap=%.2f offset=%.1f speed_ahead=%.2f why=%s",
				self, idx, scene, r.kept ? "held" : "switched", r.why ? r.why : "", r.rival ? r.rival : "", r.rivalGap, r.offset, r.speedAhead, r.switchWhy ? r.switchWhy : "");
		}
		// Every car on autopilot, every update (removal 2026-11-14: in every build): one tick of its World radar, and for a car with no lane its
		// goal plan (D13) and step 7 (the plan's newest result writes the controls; none fresh: stand). Lane cars are planned and driven in
		// AutodriveDecide's lane branch.
#ifndef FVO_RELEASE
        // Lab stock in FLIGHT (user decision 2026-11-20: the lab spot check must not need driving, wheels or a floor under the road; the below-road
        // floor may not exist). Mode for a private lab pair is flight at one commanded absolute height (stockfloat.h, held by the hover in Fly with
        // no ground reference). The spot test is the same FactoryTest / MotionEvidence as before (Health and Handoff read the same g_spotProof): a
        // bounded reverse pulse measured along the heading. Only who answers the ask changed: this frame's hover command, not the wheeled planner.
        // Eligibility keeps: released lease, driver seated, concealment (covered or unseen), steps advancing, one pulse at a time, no player
        // handoff. It no longer needs onWheels or any floor; it needs the float to be held instead.
        spottest::Ask StockFlightTest(int idx, unsigned self, std::uint64_t lease, const drive::Input& in)
        {
            auto& motion = g_spotMotion[idx];
            auto& test = g_factoryTest[idx];
            auto& hold = g_stockFloat[idx];
            if (g_spotVehicle[idx] != self) {
                std::lock_guard lock(g_spotLock);
                g_spotProof.erase(g_spotVehicle[idx]);
                g_spotResult.erase(g_spotVehicle[idx]);
                g_spot[idx] = {};
                g_spotVehicle[idx] = self;
                motion.Reset();
                test = {};
                g_stockPulseIssued[idx] = {};
            }
            const auto now = GetTickCount64();
            const auto receipt = relocconsumers::Poll(self);
            // The commanded height: captured from the car's own, followed through a seated move, re-based on its receipt.
            stockfloat::Sample fs;
            fs.vehicle = self; fs.lease = lease; fs.now = now; fs.z = in.z; fs.onWheels = in.onWheels;
            fs.relocating = (npcvehicles::RelocationMark(self) & 1u) != 0;
            fs.relocEpoch = receipt.epoch; fs.relocSerial = receipt.serial;
            if (receipt.valid()) fs.relocToZ = receipt.to[2];
            const auto floatEvent = stockfloat::Update(hold, fs);
            if (floatEvent != stockfloat::Event::None)
                logger::Write("OBS system=stock.float vehicle=%08X pilot=%d event=%s lease=%llu z=%.2f commanded_z=%.2f on_wheels=%d ground_ok=%d ground_z=%.2f reloc_serial=%llu reloc_to_z=%.2f reason=%s",
                    self, idx, stockfloat::Name(floatEvent), static_cast<unsigned long long>(lease), in.z, hold.z, int(in.onWheels), int(in.groundOk),
                    in.groundOk ? in.groundUnder : 0.0, static_cast<unsigned long long>(receipt.serial), receipt.valid() ? receipt.to[2] : 0.0,
                    floatEvent == stockfloat::Event::Captured ? "lab_lease_float_at_own_height_no_ground_reference" :
                    floatEvent == stockfloat::Event::Following ? "seated_move_in_progress_height_follows_the_car" : "applied_receipt_destination_height");
            const bool held = stockfloat::Held(hold, now, in.z, in.vz, in.onWheels);
            static bool wasHeld[1 + prefetch::kSlotPilotCount] = {};
            if (held != wasHeld[idx]) {
                wasHeld[idx] = held;
                logger::Write("OBS system=stock.float vehicle=%08X pilot=%d event=%s lease=%llu z=%.2f commanded_z=%.2f vz=%.2f engaged_ms=%llu reason=%s",
                    self, idx, held ? "held" : "released_hold", static_cast<unsigned long long>(lease), in.z, hold.z, in.vz,
                    static_cast<unsigned long long>(hold.engagedAt && now >= hold.engagedAt ? now - hold.engagedAt : 0),
                    held ? "flight_at_commanded_height_spot_pulse_allowed" : "flight_height_or_mode_not_held");
            }

            const bool released = npcvehicles::FactoryStockLeaseOf(idx, self, true) == lease;
            const bool anotherPulse = g_stockPulseOwner && g_stockPulseOwner != lease &&
                now >= g_stockPulseAt && now - g_stockPulseAt < spottest::rules::factoryPulseMs;
            spottest::Input sample;
            sample.x = in.x; sample.y = in.y; sample.now = in.now;
            sample.relocation = receipt.serial;  // the jump of a relocation is not motion
            sample.speed = std::hypot(in.vx, in.vy);
            sample.builtAt = vehiclediag::BuiltAt(self);
            sample.covered = trafficarrival::Hidden();
            sample.unseen = !vehiclekeep::InCameraView(in.x, in.y, in.z);
            const bool concealed = sample.covered || sample.unseen;
            sample.eligible = released && held && in.driverSeated && !in.paused && concealed && !anotherPulse &&
                !trafficpool::PlayerOwnsHandoff(self);
            auto& binding = g_shs[idx];
            unsigned body = 0;
            {
                std::lock_guard lock(binding.lock);
                const auto instance = binding.instance.load();
                body = instance ? binding.bodyId.load() : binding.handoffStepBody;
                if (!bodybinding::Contains({binding.sceneBodies, binding.sceneCount}, body)) body = 0;
                sample.steps = binding.handoffSteps;
            }
            // The pulse the hover applied last frame is the test's "accepted" answer (the planner no longer answers for stock).
            const auto& issued = g_stockPulseIssued[idx];
            const bool applied = issued.serial && now >= issued.at && now - issued.at <= 150 && held;
            const auto before = motion.proof.completed;
            const auto ask = test.Step(lease, sample, body, now, applied ? issued.serial : 0, applied, false, in.heading, motion);
            if (ask.active) {
                g_stockPulseOwner = lease;
                g_stockPulseAt = now;
                // One short backwards thrust in flight (float travel, gentlest gear), renewed every frame the test asks; it lapses by itself.
                Command c;
                c.set = Command::kScripted | Command::kTravel | Command::kThrottle | Command::kGear;
                c.travel = 1;
                c.throttle = ask.reverse ? -stockfloat::rules::pulseThrottle : stockfloat::rules::pulseThrottle;
                c.gear = stockfloat::rules::pulseGear;
                c.holdMs = stockfloat::rules::pulseHoldMs;
                g_cmds[idx].c = c;
                g_cmds[idx].until = now + c.holdMs;
                if (issued.serial != ask.serial)
                    logger::Write("OBS system=stock.pulse vehicle=%08X pilot=%d event=issued serial=%llu dir=%s throttle=%.2f gear=%d hold_ms=%u steps=%llu reason=flight_pulse_on_the_hover_actuator_not_the_wheeled_planner",
                        self, idx, static_cast<unsigned long long>(ask.serial), spottest::Name(ask.dir), c.throttle, c.gear, c.holdMs,
                        static_cast<unsigned long long>(sample.steps));
                g_stockPulseIssued[idx] = {ask.serial, now};
            } else if (g_stockPulseOwner == lease) g_stockPulseOwner = 0;
            if (motion.proof.completed != before) {
                const auto& proof = motion.proof;
                {
                    std::lock_guard lock(g_spotLock);
                    if (proof.completed) g_spotProof[self] = proof;
                    else g_spotProof.erase(self);
                }
                if (proof.completed) logger::Write(
                    "OBS system=spot.motion vehicle=%08X body=%08X event=pulse_response mode=flight_pulse built_at=%llu start=%llu completed=%llu dir=%s moved_m=%.4f steps=%llu commanded_z=%.2f z=%.2f",
                    self, proof.body, proof.builtAt, proof.started, proof.completed, spottest::Name(proof.direction),
                    proof.moved, proof.steps, hold.z, in.z);
            }
            const bool proved = motion.proof.completed != 0;
            const char* why = !released ? "await_handoff" : proved ? "proved" :
                !stockfloat::Active(hold, now) ? "await_float_capture" : in.onWheels ? "await_flight_mode" :
                !held ? "await_float_height" : !concealed ? "await_concealment" :
                !sample.eligible ? "await_eligible_owner" : ask.active ? "pending_flight_pulse" : "await_retry";
            logger::Observe("factory.stock-test", self, unsigned(released) | (unsigned(held) << 1) |
                (unsigned(sample.eligible) << 2) | (unsigned(ask.active) << 3) | (unsigned(proved) << 4) |
                (unsigned(concealed) << 5),
                "pilot=%d token=%llu mode=flight reason=%s body=%08X on_wheels=%d float_held=%d commanded_z=%.2f z=%.2f vz=%.2f steps=%llu",
                idx, static_cast<unsigned long long>(lease), why, body, int(in.onWheels), int(held), hold.z, in.z, in.vz,
                static_cast<unsigned long long>(sample.steps));
            return ask;
        }
#endif

		void EnergyStep(int idx, std::uint32_t self, const drive::Input& ain, const char* owner, bool floorProofLane)
		{
			if (idx < 0 || idx >= planenergy::kSlots || !self) return;
			auto& d = planenergy::For(idx);
			const double now = ain.now;
			const bool lane = now - g_shadowLaneAt[idx] < .5;
			const bool ordinaryGoal = !lane && owner && std::strcmp(owner, "energy_plan_goal") == 0;
            bool stockGoal = false;
#ifndef FVO_RELEASE
            if (const auto stockLease = npcvehicles::FactoryStockLeaseOf(idx, self, false)) {
                stockGoal = true;
                (void)StockFlightTest(idx, self, stockLease, ain);  // the float and its spot pulse (a hover command), no planner, no floor
            }
#endif
            const double estimatedBase = ain.z - ain.shape.originAbove;
            const double normalBase = ain.groundOk ? ain.groundUnder + (ain.onWheels ? 0.0 : std::max(0.0, ain.above)) : estimatedBase;
            const double base = stockGoal ? estimatedBase : normalBase;
            // World radar and the Goal scene use the same local base.
            RadarStep(idx, self, ain.x, ain.y, base, ain.heading, ain.vx * std::sin(ain.heading) + ain.vy * std::cos(ain.heading),
                ain.shape.halfWidth, ain.shape.halfLength, ain.now, floorProofLane);
            const bool goalCar = ordinaryGoal;
			if (goalCar && planenergy::budget::MaySubmit(now, d.submitAt, planenergy::g_plansInFlight.load(std::memory_order_relaxed))) {
				static thread_local std::vector<planworld::Shape> shapes; static thread_local std::vector<planinput::FloorPoint> floor;
				planinput::GoalAsk ask; ask.now = now; ask.carX = ain.x; ask.carY = ain.y; ask.carBase = base; ask.halfLength = ain.shape.halfLength;
				ask.goal.x = ain.hold ? ain.x : ain.gx; ask.goal.y = ain.hold ? ain.y : ain.gy; ask.goal.radius = std::max(1.5, ain.stop); ask.goal.stop = !ain.keepGoing;
				ask.travel = ain.permit == drive::Permit::DriveOnly ? 1 : ain.permit == drive::Permit::HoverOnly ? 2 : ain.preferAir ? 0 : 1; ask.hoverPossible = ain.hoverPossible;  // Both with the ground preferred: priced as drive-only (hover only where wheels cannot go), the autopilot's own "preference, not a limit"
                GoalWorld(self, ain.x, ain.y, base, ask.goal.x, ask.goal.y, now, shapes, floor);
				RadarAppend(idx, shapes, now);  // radar boxes: measured solids with their real size
				bicycle::Spec spec; spec.halfWidth = ain.shape.halfWidth; spec.halfLength = ain.shape.halfLength; spec.height = std::max(1.0, ain.shape.height);
				if (ain.shape.steerLock > 0 && ain.shape.wheelbase > 0) spec.car = { ain.shape.steerLock, ain.shape.wheelbase, ain.shape.slopePoint, ain.shape.initialSlope, ain.shape.fullSteerSpeed };
				else spec.car = carmodel::Fallback(ain.shape.halfLength);  // not measured yet
				const double want = ain.cruise && ain.cruiseSpeed > 0 ? ain.cruiseSpeed : ain.speedLimit > 0 ? ain.speedLimit : 12.0;
				FlightSpec(spec, idx, self); NoteFlightPlan(idx, want, nullptr);  // FC-1: the gear row's hover turn and top; FG-1's wanted speed
				bicycle::State st; st.x = ain.x; st.y = ain.y; st.heading = ain.heading; st.speed = ain.vx * std::sin(ain.heading) + ain.vy * std::cos(ain.heading);
				st.input = g_shs[idx].carInput.load(std::memory_order_relaxed); st.z = base; st.mode = ain.onWheels ? 0 : 1;
				planenergy::SubmitGoal(idx, self, ask, shapes, floor, st, spec, want, now);
			}
			// Lab stock (user decision 2026-11-20): held in FLIGHT at its commanded absolute height, standing (no speed, brake on, nose kept). The hover
			// in Fly holds that height with no ground reference (stockfloat.h); the spot pulse is StockFlightTest's command. When the lease ends (the
			// pair is promoted to a route) this branch stops: the ordinary goal plan / lane decides hover or wheels for the route as for any traffic car.
#ifndef FVO_RELEASE
			if (stockGoal) {
				auto& out = g_gs[idx].adOut; g_energyOwnsAt[idx] = GetTickCount64();
				out.mode = drive::Mode::Hover; out.phase = drive::Phase::Arrived; out.bearing = ain.heading;
				out.speed = 0; out.brake = true; out.reverse = false; out.boost = false; out.why = "factory float hold";
				if (now - g_energyAuthLogAt[idx] >= 2.0 || now < g_energyAuthLogAt[idx]) { g_energyAuthLogAt[idx] = now;
					logger::Write("OBS system=plan.authority vehicle=%08X pilot=%d event=decides scene=stock_float mode=%s speed=0.00 reverse=0 brake=1 commanded_z=%.2f z=%.2f pos=%.1f,%.1f reason=lab_stock_flight_hold_no_wheels_no_floor",
						self, idx, drive::Name(out.mode), g_stockFloat[idx].z, ain.z, ain.x, ain.y); }
				return;
			}
#endif
			// Step 7 (one authority): a car with no lane is driven by its goal plan (D13). Wheels: the path follower's bearing and the plan's speed into
			// the actuators' channels; hover: the hover follower. Not ready: stand.
			if (goalCar) {
				const auto ge = planenergy::Latest(idx); const ULONGLONG gage = ge.at ? GetTickCount64() - ge.at : ~0ull;
				auto& out = g_gs[idx].adOut; g_energyOwnsAt[idx] = GetTickCount64(); const char* gwhy = "";
				const double gspd = std::hypot(ain.vx, ain.vy);
				if (ge.at && ge.vehicle == self && ge.goal && gage <= 500 && ge.ok) {
					if (ge.hover) {
						hoverfollow::Car hc; hc.x = ain.x; hc.y = ain.y; hc.z = ain.z - ain.shape.originAbove; hc.heading = ain.heading; hc.speed = gspd; hc.originAbove = ain.shape.originAbove;
						const auto ho = hoverfollow::Follow(ge.hoverPath, hc, gage / 1000.0);
						NoteFlightPlan(idx, -1, &ge.hoverPath);  // FG-1: the committed hover plan's sharpest turn
						out.mode = ho.land ? drive::Mode::Drive : drive::Mode::Hover; out.speed = ho.speed; out.brake = ho.brake; out.reverse = false; out.boost = false;
						out.bearing = (ho.split || ho.facingUsed || ho.hold) ? ho.nose : ho.bearing; out.phase = drive::Phase::Travel;
						if (!ho.land) BoostSpeed(idx, self, out, false, false, -1.0, std::numeric_limits<double>::infinity(), ain.vx * std::sin(ain.heading) + ain.vy * std::cos(ain.heading), 0.0, now, true, ho.split ? "strafe_plan" : ho.hold ? "holding_spot" : ho.facingUsed ? "facing_turn" : nullptr);  // BO-2: Speed's boost request for a flying car
						if (idx < int(sizeof g_planSplit / sizeof g_planSplit[0])) { if (ho.split) g_planSplit[idx] = { GetTickCount64(), ho.bearing, ho.speed }; else g_planSplit[idx].at = 0; }
						gwhy = ho.land ? "hover_landing" : "hover";
					} else {
						const auto pf = pathfollow::Follow(ge.path, ain.x, ain.y, ain.heading, ain.vx * std::sin(ain.heading) + ain.vy * std::cos(ain.heading), ge.reverse);
						const double gtarget = std::fabs(ge.speed) < .1 ? 0.0 : ge.reverse ? std::max(0.0, -ge.speedAhead) : std::max(0.0, ge.speedAhead);
						out.mode = drive::Mode::Drive; out.reverse = ge.reverse; FollowSpeed(idx, gtarget, now, ain.limits.brake, gspd, out.speed, out.brake); TraceSwitch(idx, self, ge, "goal");
						BoostSpeed(idx, self, out, ain.onWheels, false, -1.0, ge.terms.wheelSpeed, ain.vx * std::sin(ain.heading) + ain.vy * std::cos(ain.heading), std::fabs(g_gs[idx].drvSteer), now);  // BO-1: Speed's boost request
						if (pf.ok) out.bearing = pf.bearing; out.phase = ge.reverse ? drive::Phase::Reverse : drive::Phase::Travel; gwhy = "wheels";
					}
					out.why = ge.why;
				} else { out.speed = 0; out.brake = true; out.reverse = false; out.boost = false; out.why = "energy plan not ready"; gwhy = !ge.at ? "no_result_yet" : ge.vehicle != self ? "result_for_another_car" : !ge.goal ? "lane_scene_result" : gage > 500 ? "result_stale" : "no_finite_candidate"; }
				if (now - g_energyAuthLogAt[idx] >= 2.0 || now < g_energyAuthLogAt[idx]) { g_energyAuthLogAt[idx] = now;
					logger::Write("OBS system=plan.authority vehicle=%08X pilot=%d event=decides scene=goal mode=%s speed=%.2f reverse=%d brake=%d age_ms=%llu candidate=\"%s\" state=%s pos=%.1f,%.1f reason=one_authority_energy_plan",
						self, idx, drive::Name(out.mode), out.speed, int(out.reverse), int(out.brake), static_cast<unsigned long long>(ge.at ? gage : 0), ge.why ? ge.why : "", gwhy, ain.x, ain.y); }
			}
		}

		// The autopilot's decision (autodrive.h) for this frame: inputs from what the vehicle and the shared map know now.
		void AutodriveDecide(int idx, std::uintptr_t vehicle, const Goal& goal, const game::Vec3& pos, double heading, const double v[3],
			bool groundOk, double groundZ, double above, const char* who)
		{
			State& g = g_gs[idx];
			const Shared& sh = g_shs[idx];
			drive::Input in;
			const ULONGLONG nowMs = GetTickCount64();
			in.now = double(nowMs) / 1000.0;
			in.x = pos.x; in.y = pos.y; in.z = pos.z; in.heading = heading;
			if (g.drive) { in.vx = std::sin(heading) * g.drvSp; in.vy = std::cos(heading) * g.drvSp; }
			else { in.vx = v[0]; in.vy = v[1]; in.vz = v[2]; }
			if (const auto selfId = vehicle ? At<std::uint32_t>(vehicle, off::kFormId) : 0u) world::NoteFlownVelocity(selfId, v);  // VL-1: the body's velocity for the Vehicle List
			in.groundUnder = groundZ; in.groundOk = groundOk; in.above = above;
			in.gx = goal.x; in.gy = goal.y; in.gz = std::isfinite(goal.z) ? goal.z : pos.z; in.stop = std::max(goal.stop, 1.0);
			if (idx >= 0 && idx < int(sizeof g_driveOnlyLane / sizeof g_driveOnlyLane[0])) g_driveOnlyLane[idx] = false;
			in.keepGoing = goal.keepGoing; in.hold = goal.hold; in.cruise = goal.cruise; in.cruiseSpeed = goal.cruiseSpeed;
			if (goal.keepGoing && !goal.cruise && !goal.hold) { in.targetVx = g_pvx; in.targetVy = g_pvy; }
			const auto& nv = PilotNavOf(idx);
			const std::uint32_t self = At<std::uint32_t>(vehicle, off::kFormId);
			const double dist = std::hypot(goal.x - pos.x, goal.y - pos.y);
			const bool occupied = idx >= 1 || g_driverVehicle.load(std::memory_order_relaxed) == vehicle;
			// ---- Hard limits (2026-10-27, the user: contexts set preferences; limits always win) ----
			// Your own vehicle's travel profile is your choice: Drive only / Hover only hold. An NPC profile's travel is
			// only its default preference (0.8 meant it so: "a fight makes it hover"); the moment decides below.
			in.permit = drive::Permit::Both;
			if (idx == 0 && nv.travel == travelmode::Default::Drive) in.permit = drive::Permit::DriveOnly;
			if (idx == 0 && nv.travel == travelmode::Default::Hover) in.permit = drive::Permit::HoverOnly;
			// "Hover or drive" off: vehicles hover, as before.
			if (nv.travel == travelmode::Default::Auto && !features::On(features::Id::HoverOrDrive)) in.permit = drive::Permit::HoverOnly;
			// Under 30 % health the hover fails: wheels only.
			if (vehiclehits::HealthShare(self) < travelmode::kDamaged) { in.permit = drive::Permit::DriveOnly; in.hoverPossible = false; }
			// A low ceiling (indoors, under a structure): no room to fly.
			const double ceilingRoom = g.ceilValid ? g.ceilZ - pos.z - kVehicleTop : 1e9;
			if (ceilingRoom < 6.0 && in.permit == drive::Permit::Both) in.permit = drive::Permit::DriveOnly;
			// ---- What the moment prefers (strongest first) ----
			const bool fighting = idx ? npcvehicles::PilotFighting(idx) : CoverFighting();
			const bool fleeing = idx ? npcvehicles::PilotFleeing(idx) : false;
			const bool catching = idx == 0 && g_follow && g_air.air;
			const bool finalApproach = dist < 30.0 && (!goal.keepGoing || goal.hold);
			bool air = false;
			const char* why = "";
			if (fighting) { air = true; why = "combat: low flight for a firing line"; }
			else if (fleeing) { air = true; why = "fleeing: the straight way out"; }
			else if (catching) { air = true; why = "catching you in the air"; }
			else if (idx >= 1 && nv.trafficLane) {
				// A lane on authored road tiles is a road: drive it (2026-11-04, the user prefers driving). Open-air tiles,
				// routes without road tiles and lanes in transition keep flying.
				// Alignment (2026-11-04): the lane's own travel kind decides, the same one the lane driver locks to
				// (addon NaturalTravel: road tile drive, lift hover; explicit Drive/Hover as set). "Both" keeps flying.
				const auto* lane = routeguidance::Get(idx, GetTickCount64());
				const int kind = lane ? lane->travel : 0;
				air = kind != 1; why = kind == 1 ? "lane travel: drive (road)" : kind == 2 ? "lane travel: hover (lift)" : "traffic lane: flying";
			}
			else if (finalApproach) { air = false; why = "final approach: on the ground"; }
			else if (realvehicles::PlaceOwner(vehicle) != 0) { air = false; why = "settlement: on the ground"; }
			else if (idx == 0 && !occupied && !goal.keepGoing && dist > 150.0) { air = true; why = "long summon: flying"; }
			else {
				switch (nv.travel) {
				case travelmode::Default::Hover: air = true; why = "profile: flying"; break;
				case travelmode::Default::CopyPlayer: air = g_gs[0].active && !g_gs[0].drive && g_driverVehicle.load(std::memory_order_relaxed) != 0;
					why = air ? "escort: you fly, it flies" : "escort: you drive, it drives"; break;
				default: air = false; why = idx ? "traffic / profile: on the ground" : "on the ground"; break;
				}
			}
			in.preferAir = air;
			in.preferWhy = why;
			g.adCombat = fighting;
			in.speedLimit = nv.speedLimit > 0 ? double(nv.speedLimit) : 0.0;
            in.strictTraffic=idx>=1&&nv.trafficLane;
			// NPC pilots: the stuck logic acts only with someone in the driver seat (checked every 0.5 s). The player's own vehicle may
			// be summoned empty on purpose, so it stays on there.
			if (idx >= 1) {
				static ULONGLONG seatAt[kPilots] = {};
				static bool seated[kPilots] = {};
				if (nowMs - seatAt[idx] >= 500) { seatAt[idx] = nowMs; seated[idx] = npcvehicles::HasDriver(self); }
				in.driverSeated = seated[idx];
			}
			// Paused or covered (the same screens the traffic addon calls 2 and 3): the loading screen, or the local map / a menu
			// holding gameplay (also the game window not in front). Read once a frame for all pilots.
			{
				static ULONGLONG pausedAt = 0;
				static bool paused = false;
				if (nowMs != pausedAt) { pausedAt = nowMs; paused = !playclock::Running(); }  // W1: the one play clock (cover, menus, photo mode)
				// Held on purpose (seat brake, waiting for a model) is not stuck: the watch would start escapes and backtracks on a vehicle told to stay.
				const bool heldOnPurpose = (g_cmds[idx].c.set & Command::kBrake) && g_cmds[idx].c.brake && nowMs < g_cmds[idx].until;
				// No physics steps: the vehicle cannot move whatever it is told, so it is not stuck and an escape cannot help (2026-11-01: 60 of 61 stuck reports, 44 from one pilot). generator: PairVerdict.
				static ULONGLONG noStepSince[kPilots] = {};
				const bool scriptedNow = (g_cmds[idx].c.set & Command::kScripted) && nowMs < g_cmds[idx].until;
				if (paused || scriptedNow || StepsRecent(idx, nowMs)) noStepSince[idx] = 0;
				else if (!noStepSince[idx]) noStepSince[idx] = nowMs;
				// A moving vehicle IS being simulated, whatever our step counter says (2026-11-05 00:21: "no steps" at 18.8 m/s braked
				// moving traffic for 1.5-168 s at a time, 132 times in one run: the stop-go wobble). No steps counts only while still.
				const bool movingNow = std::hypot(in.vx, in.vy) > 0.5;
				if (movingNow && noStepSince[idx] && nowMs - noStepSince[idx] >= 1500) logger::Observe("autopilot.steps", self, 2u, "pilot=%d event=moving_without_counted_steps speed=%.1f reason=not_paused", idx, std::hypot(in.vx, in.vy));
				const bool noSteps = noStepSince[idx] && nowMs - noStepSince[idx] >= 1500 && !movingNow;
				logger::Observe("autopilot.steps", self, noSteps ? 1u : 0u, "pilot=%d no_steps=%d for_ms=%llu", idx, noSteps, noStepSince[idx] ? nowMs - noStepSince[idx] : 0ull);
				in.paused = paused || heldOnPurpose || noSteps;
				logger::Observe("autopilot.hold", self, heldOnPurpose ? 1u : 0u, "pilot=%d held=%d screen_paused=%d", idx, heldOnPurpose, paused);
			}
			if (in.permit == drive::Permit::Both && dist > 20.0)
				in.waterAhead = environment::SurfaceAhead(self, pos.x, pos.y, pos.z, std::atan2(goal.x - pos.x, goal.y - pos.y), std::min(250.0, dist)).water;
			in.contact = g.adContact;
			g.adContact = false;
			if (g.ceilValid) in.ceiling = std::max(0.0, g.ceilZ - pos.z - kVehicleTop);
            else if(in.strictTraffic&&!g.ceilKnown)in.ceiling=0;
			in.onWheels = g.drive;  // the game's own wheels; otherwise the hover (held low for "drive")
			in.self = self;
			const auto selfRight=RightOfWay(self,idx==0,false,nv.priority);
			const auto selfRank=trafficrank::Of(selfRight);
			{	// Owner and rank record (planowner.h): who drives this vehicle, from what the core already knows. Traced on every change or refusal.
				const auto owner = idx == 0 ? planowner::Owner::Cruise : fighting ? planowner::Owner::Combat : nv.trafficLane ? planowner::Owner::Traffic : planowner::Owner::Job;
				std::lock_guard<std::mutex> ownersGuard(g_ownersLock);
				const auto oc = g_owners.Claim(self, owner, selfRank, idx, double(GetTickCount64()) / 1000.0, idx == 0 ? "your vehicle on autopilot" : fighting ? "fighting" : nv.trafficLane ? "traffic lane" : "job or trip");
				if (auto* rec = g_owners.Find(self); rec && rec->owner == owner) { rec->x = in.x; rec->y = in.y; rec->z = in.z; rec->speed = std::hypot(in.vx, in.vy); rec->placedAt = double(GetTickCount64()) / 1000.0; }  // C1
				if (oc.changed || oc.refused) logger::Write("OBS system=plan.owner vehicle=%08X pilot=%d event=%s from=%s to=%s rank=%s reason=%s", self, idx, oc.refused ? "refused" : "claim", planowner::Name(oc.from), planowner::Name(oc.to), trafficrank::Name(selfRank), oc.refused ? "a_stronger_owner_holds_it" : "owner_changed");
			}
			in.priority = trafficrank::PriorityOf(selfRight);
			static std::array<drive::Other, fvolimits::kVehicles> others;  // (off the stack; filled up to `count` below)
			others.fill(drive::Other{});
			int count = 0;
			for (unsigned k = 0; g_vsnapOk && k < g_vsnap.count && k < world::kMaxVehicles; ++k) {
				const auto& w = g_vsnap.v[k];
				if (w.formId == self) {
					{   // "one car": the measured collision box when it is ok for this vehicle, else the model bounds
						OwnMeasured m; if (idx >= 0 && idx <= prefetch::kSlotPilotCount) { std::lock_guard<std::mutex> sizeGuard(g_ownSizeLock); m = g_ownSize[idx]; }
						const bool measured = m.ok && m.vehicle == self;
						// MS-1: never narrower than the wheels plus the tyre; with no measured box and no valid model bounds the wheels decide
						// (not the 2.5 m default box: model 0500250F has wheels at +-0.54 m and the default made a rail 0.4 m clear read as touching).
						const double wheelHalf = idx >= 0 && idx <= prefetch::kSlotPilotCount ? double(g_wheelHalf[idx].load(std::memory_order_relaxed)) : 0.0;
						const double fromWheels = wheelHalf > .2 ? wheelHalf + kTyreHalf : 0.0;
						const bool byWheels = !measured && !w.baseBoundsValid && fromWheels > 0;
						const double widthSeen = measured ? m.halfWidth : byWheels ? fromWheels : double(w.half[0]);
						in.shape.halfWidth = std::max({0.6, widthSeen, fromWheels});
						in.shape.halfLength = std::max(1.0, measured ? m.halfLength : double(w.half[1]));
						static double sizeLogged[1 + prefetch::kSlotPilotCount][3] = {}; static std::uint32_t sizeLoggedFor[1 + prefetch::kSlotPilotCount] = {};
						if (idx >= 0 && idx <= prefetch::kSlotPilotCount) {
							const double now3[3] = { measured ? 1.0 : byWheels ? 2.0 : 0.0, in.shape.halfWidth, in.shape.halfLength };
							if (sizeLoggedFor[idx] != self || now3[0] != sizeLogged[idx][0] || std::fabs(now3[1] - sizeLogged[idx][1]) > .05 || std::fabs(now3[2] - sizeLogged[idx][2]) > .05) {
								sizeLoggedFor[idx] = self; for (int q = 0; q < 3; ++q) sizeLogged[idx][q] = now3[q];
								logger::Write("OBS system=vehicle.size vehicle=%08X pilot=%d source=%s half_width=%.2f half_length=%.2f height=%.2f bounds_half_width=%.2f bounds_half_length=%.2f bounds_valid=%d",
									self, idx, measured ? "measured" : byWheels ? "wheels" : "bounds", in.shape.halfWidth, in.shape.halfLength, measured ? m.height : 2.0 * double(w.half[2]),
									double(w.half[0]), double(w.half[1]), int(w.baseBoundsValid));
							}
						}
					}
					in.shape.originAbove = fieldgate::kOriginAbove;  // measured rest offset, not model bounds (see fieldgate.h)
					in.shape.steerLock = sh.carLock.load(); in.shape.wheelbase = sh.carWheelbase.load(); in.shape.slopePoint = sh.carSlope.load();
					in.shape.initialSlope = sh.carInitial.load(); in.shape.fullSteerSpeed = sh.carFull.load();  // the measured car (0 until read)
					in.shape.height = fieldgate::RestHeight(double(w.center[2]) + double(w.half[2]) - double(w.pos[2]));
					continue;
				}
				auto& o = others[count++];
				o.id = w.formId;
				o.x = w.pos[0]; o.y = w.pos[1]; o.z = w.pos[2];
				o.vx = w.vel[0]; o.vy = w.vel[1];
				o.radius = VehRadius(w);o.halfWidth=std::max(0.5,double(w.half[0]));o.halfLength=std::max(1.0,double(w.half[1]));o.heading=w.heading;o.parked=w.who==world::VehicleInfo::Who::Parked;
				o.priority = trafficrank::PriorityOf(RightOfWay(w.formId,w.who == world::VehicleInfo::Who::Player,w.who == world::VehicleInfo::Who::Parked,0));
			}
			in.others = others.data();
			in.otherCount = count;
			in.limits.driveTop = 22.0 * EnvSpeed();
			in.limits.hoverTop = 35.0 * EnvSpeed();
			in.limits.brake = std::clamp(sh.hull.brake, 2.5, 8.0);
			in.limits.hoverClear = kCallSkim + EnvClimb();
			// The game's own measurement, kept before the addon's floor replaces it: fieldgate::GroundLift needs the REAL
			// road under an authored floor (2026-11-04 22:16: Roadrunner floor -177.06 over the deck at -178.49; with the
			// addon floor passed in, the lift never fired and cars on the deck sat "outside, settling" for the whole run).
			const bool measuredGroundOk=in.groundOk;const double measuredGround=in.groundUnder;
			// The traffic addon's road answers first on its own lane (see FvoRoadSurfaceV1); the game's map refines it.
			void* const surfaceOwner = fieldgate::surfaceProvider && routeguidance::Get(idx, GetTickCount64()) ? routeguidance::frames[idx].owner : nullptr;
			if (surfaceOwner) {
				double floorZ = 0;
				if (fieldgate::surfaceProvider(surfaceOwner, in.x, in.y, &floorZ) == 1) {
					if (fieldgate::Disagrees(in.groundOk, in.groundUnder, floorZ))
						logger::Observe("traffic.surface", self, 2, "event=game_disagrees pilot=%d addon_floor=%.2f game_ground=%.2f z=%.2f applied=addon", idx, floorZ, in.groundUnder, in.z);
					else if (!in.groundOk)
						logger::Observe("traffic.surface", self, 1, "event=addon_ground pilot=%d floor=%.2f z=%.2f game=unmeasured", idx, floorZ, in.z);
					in.groundOk = true; in.groundUnder = floorZ; in.above = in.z - floorZ;
					if (idx >= 0 && idx < int(sizeof g_railFloorZ / sizeof g_railFloorZ[0])) { g_railFloorZ[idx] = floorZ; g_railFloorZAt[idx] = GetTickCount64(); }
				}
			}
			const bool preciseDrive=in.onWheels;  // a car on its wheels (the lane grace below)
			// A game collision box at (x, y, z) from the shared map, except on the addon's own road or lift (its road is the authority there).
			auto objectAt=[&](double x,double y,double z){const auto m=navmap::At(x,y,z);if(!m.objBlocked)return false;
				double fz=0;if(surfaceOwner){const int a=fieldgate::surfaceProvider(surfaceOwner,x,y,&fz);if(a==1||a==2)return false;}return true;};
			const auto* authoredLane=routeguidance::Get(idx,GetTickCount64());
			if(authoredLane&&idx>=0&&idx<257){const auto& rp=routeguidance::frames[idx].repaired;  // LF-1: the line repair, traced
				if(rp.removed||rp.rounded)logger::Observe("lane.repair",self,authoredLane->identity^(std::uint64_t(rp.removed)<<40)^(std::uint64_t(rp.rounded)<<48),
					"event=repaired pilot=%d removed=%d rounded=%d worst_turn_deg=%.0f points=%d reason=no_fold_no_kink_in_the_line_a_car_follows",idx,rp.removed,rp.rounded,rp.worstTurnDeg,authoredLane->count);}
            // One owner for traffic lanes (2026-11-23): a lane command a little late keeps the last lane for up to 4 s
            // across all travel modes (drive, both, hover) when there is no explicit refusal, so post-load bursts never
            // slam cars into coordinator_hold (expired_lane_command).
            if(!authoredLane){std::uint64_t age=0;authoredLane=routeguidance::Grace(idx,GetTickCount64(),4000,&age);
                if(authoredLane)logger::Observe("traffic.lane",self,authoredLane->identity^0x200,"event=grace pilot=%d age_ms=%llu travel=%d reason=late_lane_command_last_lane_kept",idx,(unsigned long long)age,int(authoredLane->travel));}
            (void)preciseDrive;
            // GD-1 (Route, design section 4): an NPC job goal reduced to one line the plan follows (jobline.h). Only on a slot with no addon
            // lane frame, on wheels, not in combat or a scripted command, farther than jobline::rules::handBack from the goal; the old
            // autopilot keeps the final approach (arrive, land, hold, park). The line is the core's own: no addon provider is asked for it.
            bool jobLine=false,jobLineCombat=false;
            if(!authoredLane&&idx>=1&&idx<int(sizeof g_jobLine/sizeof g_jobLine[0])){
                const ULONGLONG nowJ=GetTickCount64();const auto& fr=routeguidance::frames[idx];const auto* dg=g_door.Of(idx);
                jobline::Ask ja;ja.combat=g.adCombat;ja.combatOnPlan=ja.combat&&CombatLinesOn()&&CombatAimFresh(idx,nowJ)&&g_combatAim[idx].onPlan;  // CB-2/7
                ja.on=ja.combat?ja.combatOnPlan:JobLinesOn();
                ja.jobGoal=(dg&&dg->source==goaldoor::Source::Job&&!dg->direct)||ja.combatOnPlan;  // combat aims through PilotCommand, which re-admits a direct goal each frame
                ja.onWheels=in.onWheels;ja.scripted=(g_cmds[idx].c.set&Command::kScripted)&&nowJ<g_cmds[idx].until;
                ja.laneFrame=(fr.at&&nowJ>=fr.at&&nowJ-fr.at<10000)||fr.coordinated||fr.rules.mode!=travelrules::Legacy||fr.corridor.active;
                ja.distance=std::hypot(in.gx-in.x,in.gy-in.y);
                const char* why=jobline::Refuse(ja);auto& J=g_jobLine[idx];
                if(!why){
                    if(jobline::Rebuild(in.now,J.at,in.gx,in.gy,J.gx,J.gy,ja.combat?jobline::rules::combatRebuildEvery:jobline::rules::rebuildEvery)){
                        const double want=in.cruise?double(in.cruiseSpeed):in.speedLimit>0?in.speedLimit:15.0;  // the job speed (AutodriveDecide input)
                        J.ok=jobline::Build(J.lane,self,in.x,in.y,in.z,in.gx,in.gy,in.gz,want,in.shape.originAbove,
                            [&](double x,double y,double& gz){if(ja.combat&&!in.onWheels)return false;const auto s=navmap::At(x,y,in.z);if(s.haveGround){gz=s.ground;return true;}return false;},
                            ja.combat&&!in.onWheels?2:1,ja.combat?jobline::rules::combatLength:jobline::rules::maxLength,ja.combat?jobline::rules::combatMaxSpeed:jobline::rules::maxSpeed);  // CB-3: in the air the line keeps the fight height
                        J.at=in.now;J.gx=in.gx;J.gy=in.gy;}
                    if(J.ok){authoredLane=&J.lane;jobLine=true;jobLineCombat=ja.combat;if(ja.combat)g_combatLineAt[idx]=nowJ;}else why="line_not_built";}
                if(why!=g_jobLineWhy[idx]&&(why==nullptr||g_jobLineWhy[idx]==nullptr||std::strcmp(why,g_jobLineWhy[idx])!=0)){
                    if(!why||!g_jobLineWhy[idx]||ja.jobGoal||ja.combat)  // a refusal is traced only for job and combat goals (every other pilot is "no_job_goal")
                        logger::Write("OBS system=plan.door vehicle=%08X pilot=%d event=%s reason=%s to_goal_m=%.1f points=%d speed=%.1f source=%s",self,idx,why?(ja.combat?"combat_line_off":"job_line_off"):(ja.combat?"combat_line_on":"job_line_on"),
                            why?why:"job_goal_on_the_plan",ja.distance,J.ok?J.lane.count:0,J.ok?J.lane.speed:0.0,why?"old_autopilot":"plan");
                    g_jobLineWhy[idx]=why;}
            }
            const char* laneWhy=nullptr;double laneSpeed=-1;const char* capBy="";const char* bearingBy="";
            bool floorProofLane = false;
            const char* driverOwner="energy_plan_goal";  // MB-4: which owner drives this car this update (energy plan lane / goal, or the coordinator's hold)
            bool energyHover=false;  // step 7: the energy plan flies this lane car this update (the hover actuator tracks it)
            if (const auto* lane = authoredLane) {
                const laneline::LaneWindow laneWindow(*lane);
                in.strictTraffic=true;
                auto& ld=g_laneDriver[idx];
				const auto* policy=routeguidance::Policy(idx);
                const FvoTrafficPolicyV1 legacyPolicy;
                const auto& effective=policy?*policy:legacyPolicy;
                const auto command=lane->identity ^ (std::uint64_t(effective.movement)<<48) ^
                    (std::uint64_t(effective.obstacles)<<44) ^ (std::uint64_t(effective.moving)<<40) ^
                    (std::uint64_t(effective.people)<<36) ^ reinterpret_cast<std::uintptr_t>(routeguidance::frames[idx].owner);
                const bool railCar=in.onWheels&&lane->travel!=2;  // on its wheels on a stretch that allows driving (the energy plan decides wheels or hover)
                {   // The whole road at the car (roadlanes.h): lets the planner pass through other lanes. Logged when it changes.
                    const bool had=ld.roadLanesOk;const int hadCount=ld.roadLanes.count;ld.roadLanesOk=false;
                    // Removal 2026-11-14: the road's lanes are asked every gather (World), so the energy plan's frame knows the road it may use.
                    const bool asked=true;
                    if(asked&&!jobLine&&fieldgate::roadLanesProvider&&routeguidance::frames[idx].owner){FvoRoadLanesV1 rl;  // GD-1: never a stale owner
                        if(fieldgate::roadLanesProvider(routeguidance::frames[idx].owner,in.x,in.y,&rl)==1&&rl.count>0&&rl.count<=8&&rl.own>=0&&rl.own<rl.count){
                            bool trimmedHelped=false;const bool sane=RoadLanesPlausible(rl,nullptr,nullptr,nullptr,&trimmedHelped);  // 2026-11-19: a lane trimmed by a route edge band is judged untrimmed (roadlanes.h)
                            if(sane&&trimmedHelped)logger::Observe("traffic.lanes",self,0x5100ull+std::uint64_t(rl.own),"event=trimmed_outer_lane_accepted pilot=%d band_low=%.2f band_high=%.2f reason=outer_lane_narrower_than_1m_only_because_of_known_band",idx,rl.size>=sizeof(FvoRoadLanesV1)?rl.bandLow:0.0,rl.size>=sizeof(FvoRoadLanesV1)?rl.bandHigh:0.0);
                            if(sane){ld.roadLanes=rl;ld.roadLanesOk=true;}
                            else{char t[200]="";int at=0;for(int k=0;k<rl.count&&at<190;++k)at+=std::snprintf(t+at,sizeof t-at,"%s%.1f..%.1f%s",k?",":"",rl.left[k],rl.right[k],k==rl.own?"*":"");
                                logger::Observe("traffic.lanes",self,0x5000ull+std::uint64_t(rl.own),"event=rejected pilot=%d reason=%s lanes=%s",idx,
                                    rl.left[rl.own]>0.5||rl.right[rl.own]<-0.5?"own_lane_not_at_car":"implausible_lane",t);}}}
                    if(had!=ld.roadLanesOk||hadCount!=(ld.roadLanesOk?ld.roadLanes.count:hadCount)){
                        char lanesText[200]="-";int at=0;if(ld.roadLanesOk)for(int k=0;k<ld.roadLanes.count&&at<190;++k)at+=std::snprintf(lanesText+at,sizeof lanesText-at,"%s%.1f..%.1f%s",k?",":"",ld.roadLanes.left[k],ld.roadLanes.right[k],k==ld.roadLanes.own?"*":ld.roadLanes.sameWay[k]?"":"(oncoming)");
                        logger::Write("OBS system=traffic.lanes vehicle=%08X pilot=%d event=road known=%d count=%d own=%d lanes=%s provider=%d",self,idx,int(ld.roadLanesOk),ld.roadLanesOk?ld.roadLanes.count:0,ld.roadLanesOk?ld.roadLanes.own:-1,lanesText,int(fieldgate::roadLanesProvider!=nullptr));}
                }
                if(idx>=0&&idx<int(sizeof g_railHalfW/sizeof g_railHalfW[0])){g_railHalfW[idx]=in.shape.halfWidth;g_railLaneHalf[idx]=lane->halfWidth;
                    // The road's up vector here from the lane's grade (railforce road plane), along the lane's direction.
                    const double al=laneline::Project(*lane,in.x,in.y,in.z).along;const auto pa=laneWindow.At(al-3.0),pb=laneWindow.At(al+3.0);
                    const double run=std::hypot(pb.x-pa.x,pb.y-pa.y);
                    if(run>1.0){const double gr=std::clamp((pb.z-pa.z)/run,-1.5,1.5),fx=(pb.x-pa.x)/run,fy=(pb.y-pa.y)/run,nl=std::sqrt(1+gr*gr);
                        g_railRoadN[idx][0]=float(-gr*fx/nl);g_railRoadN[idx][1]=float(-gr*fy/nl);g_railRoadN[idx][2]=float(1/nl);g_railRoadNAt[idx]=GetTickCount64();
                        // Populate 60 Hz virtual guardrail barrier from authored road edges (excluding no-go bands) minus car half-width.
                        const auto pCen = laneWindow.At(al);
                        double wallLo = -std::max(lane->halfWidth, in.shape.halfWidth + 0.5);
                        double wallHi =  std::max(lane->halfWidth, in.shape.halfWidth + 0.5);
                        if (ld.roadLanesOk && ld.roadLanes.count > 0) {
                            wallLo = ld.roadLanes.left[0]; wallHi = ld.roadLanes.right[0];
                            for (int rk = 1; rk < ld.roadLanes.count; ++rk) {
                                wallLo = std::min(wallLo, ld.roadLanes.left[rk]);
                                wallHi = std::max(wallHi, ld.roadLanes.right[rk]);
                            }
                        }
                        const double carHW = std::max(0.8, in.shape.halfWidth);
                        corridorwall::Barrier wb{};
                        wb.refX = pCen.x; wb.refY = pCen.y;
                        wb.tx = fx; wb.ty = fy;
                        wb.minLat = std::min(-0.15, wallLo + carHW);
                        wb.maxLat = std::max( 0.15, wallHi - carHW);
                        wb.valid = std::isfinite(wb.refX) && std::isfinite(wb.refY) && wb.maxLat > wb.minLat;
                        g_railWall[idx] = wb;
                        g_railWallAt[idx] = GetTickCount64();}}
                // planHover: a lane car not on the rail (in the air, or still landing) keeps its plan too; the hover follows it (planhover.h).
                const bool planFlies=!railCar&&idx>=0&&idx<planenergy::kSlots;  // removal step 1: World reads the lane as a 3D line for a car off its wheels
                // NH-1 (user 2026-11-15: "the route said there should be no hover"): on a drive-only lane the lane stays a ground line and the
                // car is a wheeled car even off its wheels (it only comes down); only Both / Hover lanes are 3D lines a car flies.
                const bool driveOnly=lane->travel==1;const bool flightLine=planFlies&&!driveOnly;
                if(idx>=0&&idx<int(sizeof g_driveOnlyLane/sizeof g_driveOnlyLane[0]))g_driveOnlyLane[idx]=driveOnly;
                if(idx>=0&&idx<int(sizeof g_planFlies/sizeof g_planFlies[0])){
                    if(flightLine!=g_planFlies[idx])logger::Write("OBS system=plan.hover vehicle=%08X pilot=%d event=%s on_wheels=%d travel=%d reason=%s",self,idx,flightLine?"start":"end",int(in.onWheels),int(lane->travel),
                        !planFlies?"on_its_wheels":driveOnly?"off_its_wheels_drive_only_comes_down_no_hover":"off_its_wheels_the_lane_is_a_3d_line");
                    g_planFlies[idx]=flightLine;}
                driverOwner=(railCar||driveOnly)?"plan_rail":"plan_flies";
                g.adOut={};g.adOut.bearing=in.heading;g.adOut.mode=in.onWheels||driveOnly?drive::Mode::Drive:drive::Mode::Hover;  // NH-1: never a hover start on drive onlyg.adOut.phase=drive::Phase::Travel;  // removal step 1: the lane driver no longer decides; the energy plan writes the controls below
                laneWhy=nullptr;laneSpeed=0;
                if(idx>=0&&railCar)g_railLaneAt[idx]=GetTickCount64();  // traffic and the player's Cruise, wherever the selector gives Rail
                // AC-1 actuator (2026-11-21): physical while the engine steps this car; virtual (moved along the lane with its driver) when it
                // has no model, or the engine does not step it and it is beyond the game's physics radius (less kSwapBandM); a car inside the
                // radius whose physics has not started waits (never moved without physics where it can be seen). Same autopilot either way:
                // lane speed, the lane's holds and stop lines, the gap behind the car ahead.
                static const char* actuatorWas[1 + prefetch::kSlotPilotCount] = {};
                const bool hasModel = FarHas3D(vehicle), stepping = hasModel && StepsRecent(idx, GetTickCount64());
                const double physicsR = PhysicsRadius();
                double toPlayerM = 0; if (const auto pl = Ptr(addr::kPlayerCharacterPtr)) { const auto pp = At<game::Vec3>(pl, off::kRefPosition); toPlayerM = std::hypot(double(pp.x) - in.x, double(pp.y) - in.y); }
                // AC-2 gate (the one arbiter): the manager advises; the gate never drives without collision (advised physical, physics not
                // stepping: wait) and never writes positions onto a live body (advised virtual, physics stepping: stay physical).
                Actuator advised = Actuator::Waiting; double advisedDistance = 0;
                const bool haveAdvice = idx >= 1 && AdviceOf(self, advised, advisedDistance);
                // AC-3/AC-4: no car waits, fleet or not: physical the moment the engine steps it, carried virtually otherwise (the rail path
                // for a fleet car, its lane for any other). A car under its birth hold is not the autopilot's yet: no actuator moves it.
                const bool birthHeld = vehiclediag::BirthHeld(self);
                // HJ-1 (2026-11-22 18:45 run: FF100010 snapped 158 m, 45 m from the player): a car stopped long enough has a sleeping
                // body, so "not stepping"; moving its reference left the body behind and the car snapped back when physics woke it.
                // A car whose physics body exists (model loaded) stays physical: the rail's WK-1 wake starts it from where it really is.
                // Only a car with no body is carried virtually (the engine builds its body where the car is).
                const bool bodyHere = hasModel && !stepping && idx >= 1 && BodyInScene(idx);
                const bool provingBirth = idx >= 1 && !stepping && BirthUnfinished(self);  // HJ-3
                const bool moveVirtual = idx >= 1 && !birthHeld && !provingBirth && (!hasModel || (!stepping && !bodyHere));
                const char* gateWhy = birthHeld ? "birth_hold_not_released" : provingBirth ? "birth_not_handed_off_no_carry" : bodyHere ? "body_in_scene_stays_physical_wake" : !haveAdvice ? (moveVirtual ? "not_a_fleet_car_lane_carries_it" : "not_a_fleet_car_physical") :
                    advised == Actuator::Virtual && stepping ? "physics_live_takes_over_at_once" :
                    advised == Actuator::Physical && moveVirtual ? "physics_not_ready_keeps_going_on_rail" : "as_advised";
                // AC-2/AC-3: the arbiter's owner for this car this update (MB-4 one decider): the rail path carries it, or the physical actuator.
                if (idx >= 1 && moveVirtual) driverOwner = "plan_virtual";
                if (idx >= 1 && idx < int(sizeof actuatorWas / sizeof actuatorWas[0])) {
                    const char* now = moveVirtual ? "virtual" : birthHeld ? "birth_hold" : provingBirth ? "birth_proving" : stepping ? "physical" : bodyHere ? "physical_body_waking" : "physical_not_stepping";
                    if (actuatorWas[idx] != now) { actuatorWas[idx] = now;
                        logger::Write("OBS system=traffic.actuator vehicle=%08X pilot=%d event=swap to=%s advice=%s gate=%s distance=%.0f physics_radius=%.0f physics_live=%d model=%d",
                            self, idx, now, haveAdvice ? ActuatorName(advised) : "none", gateWhy, toPlayerM, physicsR, int(stepping), int(hasModel)); }
                }
                if(idx>=1&&g_driveRail&&lane->count>=2&&moveVirtual){
                    const bool live=!trafficarrival::Covered()&&inputgate::GameplayAllowed();
                    const double dt=g_farAt[idx]>0?std::clamp(in.now-g_farAt[idx],0.0,1.0):0.0;g_farAt[idx]=in.now;  // AC-1: far cars plan at 2-5 Hz
                    const auto here=laneline::Project(*lane,in.x,in.y,in.z);
                    double want=lane->held?0.0:std::min(double(lane->speed),in.limits.driveTop);
                    if(lane->stopDistance>=0)want=std::min(want,std::sqrt(2*4.0*std::max(0.0,lane->stopDistance-3.0)));
                    const double fx=std::sin(here.bearing),fy=std::cos(here.bearing);
                    for(int k=0;k<in.otherCount&&in.others;++k){const auto& o=in.others[k];const double dx=o.x-in.x,dy=o.y-in.y,ahead=dx*fx+dy*fy,side=std::fabs(-dx*fy+dy*fx);
                        if(ahead>0&&ahead<14&&side<3)want=0;}
                    if(live&&dt>0&&want>0){
                        // Hazard boxes (routehazard.h): a lane car carried without physics goes round them or stops short, like the physical planner does.
                        routehazard::Advice hz;{static std::unordered_map<unsigned,double> hzOffset;routehazard::Plan hp;routehazard::Box found[16];const int hn=hazardfield::NearNow(in.x,in.y,in.z-20.0,in.z+20.0,150.0,found,16);hp.boxes.assign(found,found+hn);
                            hz=routehazard::Advise(hp,[&](double st){return laneWindow.At(st);},here.along,here.along+130.0,want,in.shape.halfWidth,in.shape.halfLength,in.shape.height,hzOffset[self],dt);hzOffset[self]=hz.offset;}
                        const double s=here.along+std::min(want,hz.speedCap)*dt;auto p=laneWindow.At(s);const auto q=laneWindow.At(s+1.0);
                        if(hz.offset!=0.0){const double dx=q.x-p.x,dy=q.y-p.y,len=std::hypot(dx,dy);if(len>1e-6){p.x+=dy/len*hz.offset;p.y-=dx/len*hz.offset;}}
                        double z=p.z+in.shape.originAbove;double fz=0;
                        if(surfaceOwner&&fieldgate::surfaceProvider(surfaceOwner,p.x,p.y,&fz)==1)z=fz+in.shape.originAbove;
                        double railYaw=0,railUsed=0,rx=0,ry=0,rz=0;
                        const bool onRail=RailVirtualStep(self,in.x,in.y,in.z,want,dt,rx,ry,rz,railYaw,railUsed,in.shape.halfWidth,in.shape.halfLength,in.shape.height);  // AC-2: the rail path is the virtual actuator
                        if(onRail){p.x=rx;p.y=ry;z=rz;}
                        {const auto m=navmap::At(p.x,p.y,z+2.0);if(m.haveGround&&std::isfinite(m.ground)&&double(m.ground)+in.shape.originAbove>z&&double(m.ground)-z<4.0)z=double(m.ground)+in.shape.originAbove;}  // AC-1: never under the real ground at the swap
                        const double yaw=onRail?railYaw:std::atan2(q.x-p.x,q.y-p.y);
                        // AC-1: the car and its seated driver move together (StepReference, as the rail's reference mode did); the heading is written here.
                        const unsigned farDriver=FleetDriverOf(self);
                        bool ok=false;
                        if(farDriver&&hasModel){ok=loadedarea::StepReference(self,farDriver,float(p.x),float(p.y),float(z));
                            if(ok)FarYaw(vehicle,yaw);}
                        else ok=FarWrite(vehicle,p.x,p.y,z,yaw);
                        if(ok)NoteVirtual(self,GetTickCount64());
                        if(GetTickCount64()-g_farLogAt[idx]>=5000){g_farLogAt[idx]=GetTickCount64();
                            logger::Write("OBS system=traffic.far vehicle=%08X pilot=%d event=move speed=%.1f along=%.1f pos=%.1f,%.1f,%.1f ok=%d reason=no_model_loaded",self,idx,want,s,p.x,p.y,z,int(ok));}
                    } else if(GetTickCount64()-g_farLogAt[idx]>=5000){g_farLogAt[idx]=GetTickCount64();
                        logger::Write("OBS system=traffic.far vehicle=%08X pilot=%d event=wait speed=%.1f live=%d reason=%s",self,idx,want,int(live),!live?"paused":lane->held?"lane_hold":lane->stopDistance>=0?"stop_line":"car_ahead");}
                } else if(idx>=1&&idx<int(sizeof g_farAt/sizeof g_farAt[0]))g_farAt[idx]=0;
                if(idx>=0){  // removal step 1: every lane car is the energy plan's (the rail / flight gate and the arbiter switch are gone)
                    // Rail turns: aim ahead along the lane and hold the corner speed that fits inside the lane (railturn.h).
                    // ---- Plan arbiter (planarbiter.h; notes/SESSIONS_BOARD.md standing rule): every steering source is a speaker offering a
                    // candidate line along the lane; one arbiter keeps the committed plan with hysteresis; below, the rail only tracks it.
                    // Speakers: the route itself (rejoin from where the car is), the grid planner's line, the navmesh detour (only when the
                    // grid has no line: a real block, since missing navmesh no longer counts). Speed caps from the lane driver stay speakers too.
                    const bool arb=idx>=0&&idx<planenergy::kSlots;
#ifndef FVO_RELEASE
                    const auto factoryLease = npcvehicles::FactoryLeaseOf(idx, self, false);
                    std::uint64_t privatePulseSerial = 0;
#endif
                    const double planLook=0;  // the rail aims along the lane itself (the energy plan's path is followed when fresh)
                    // CW-1 step 3 (2026-11-22, the user: the centre line is a convex well and obstacles push the car out of it, "plan on how to get it to
                    // work for all cases"; notes/centreline-well-20261122/PLAN.md). A lane car on its wheels (or on a drive-only stretch) is planned by the
                    // centre-line well: one convex lateral problem, its path to the AF-1 follower, its speed to FollowSpeed. The energy plan keeps flight,
                    // goal scenes and, outside Script mode, the backing-up families while no forward fit exists (wellRecovering). Script mode (SD-2) is the
                    // same well with hazard boxes as the only pushes and no floor reads; vehicles and people only stop it.
                    const bool scriptMode_=features::On(features::Id::ScriptMode);
                    const bool wellLane=arb&&lane->count>=2&&lane->travel!=2&&(in.onWheels||lane->travel==1)&&
                        !(idx<int(sizeof g_testActive/sizeof g_testActive[0])&&g_testActive[idx]);
                    if(wellLane&&g_well[idx].vehicle!=self){g_well[idx]=WellState{};g_well[idx].vehicle=self;}
                    const bool wellRecovering=wellLane&&!scriptMode_&&g_well[idx].recovering;
                    const bool wellDrive=wellLane&&!wellRecovering;
                    {static int wellWas[1+prefetch::kSlotPilotCount]={};const int now_=wellDrive?(scriptMode_?3:2):wellRecovering?4:1;
                        if(idx>=0&&idx<int(sizeof wellWas/sizeof wellWas[0])&&wellWas[idx]!=now_){wellWas[idx]=now_;
                        logger::Write("OBS system=plan.well vehicle=%08X pilot=%d event=%s on_wheels=%d travel=%d reason=%s",self,idx,
                            now_==3?"script_drive":now_==2?"well_drive":now_==4?"recover_handoff":"energy_plan",int(in.onWheels),int(lane->travel),
                            now_==3?"script_mode_well_hazard_boxes_only":now_==2?"centre_line_well_plans_this_lane_car":now_==4?"no_forward_fit_energy_plan_backs_up":
                            lane->travel==2?"flight_only_lane_keeps_the_energy_plan":(idx<int(sizeof g_testActive/sizeof g_testActive[0])&&g_testActive[idx])?"spot_test_active":lane->count<2?"lane_too_short":"off_its_wheels_on_a_lane_that_allows_flight");}}
                    if(arb){
                        // World -> Route -> Avoid -> Mode -> Speed -> Arbiter: notes/VEHICLE_DECISION_DESIGN_20261106.md. This block only gathers
                        // what already exists; every decision is in planrun.h and runs offline. Arbiter rule change plus new speakers, actuator locked.
                        using planrun::kSamples;using planrun::kStep;using planworld::Shape;
                        if(g_spotVehicle[idx]!=self){  // another car in this slot: its records restart
                            const auto previous=g_planPilot.find(g_spotVehicle[idx]);
                            if(previous!=g_planPilot.end()&&previous->second==idx)g_planPilot.erase(previous);
                            g_planOdo[idx]={};g_planStepAt[idx]=0;
                        }
                        const auto hereP=laneline::Project(*lane,in.x,in.y,in.z);
                        const auto onA=laneWindow.At(hereP.along),onB=laneWindow.At(hereP.along+1.0);
                        const double lyaw=std::atan2(onB.x-onA.x,onB.y-onA.y);
                        const double c0=(in.x-onA.x)*std::cos(lyaw)-(in.y-onA.y)*std::sin(lyaw);  // + right of travel
                        const double spd=std::hypot(in.vx,in.vy),carBase=in.z-in.shape.originAbove;
                        auto lineAt=[&](double d){const auto p=laneWindow.At(hereP.along+d),q=laneWindow.At(hereP.along+d+1.0);return planrun::LinePoint{p.x,p.y,p.z,std::atan2(q.x-p.x,q.y-p.y)};};
                        const auto tPlanCar0=std::chrono::steady_clock::now();
                        double pcProvUs=0,pcBoxUs=0,pcReachUs=0;double pcSurfaceUs=0,pcWorldUs=0,pcProbeUs=0,pcBuildUs=0,pcCollideUs=0,pcSpeedUs=0,pcScoreWallUs=0,pcArbUs=0,pcBodyUs=0;
                        const double stNow=g_planOdo[idx].Step(in.x,in.y,lineAt(0).yaw);
                        const auto plRef=Ptr(addr::kPlayerCharacterPtr);const auto plPos=plRef?At<game::Vec3>(plRef,off::kRefPosition):game::Vec3{};
                        const double toPlayer=plRef?std::hypot(double(plPos.x)-in.x,double(plPos.y)-in.y):0.0;
                        spottest::Ask spotAsk{};
                        {   // ST-A: step spot test before ShouldReplan
                            if(g_spotVehicle[idx]!=self){
                                std::lock_guard sl(g_spotLock);
                                if(g_spotVehicle[idx])g_spotResult.erase(g_spotVehicle[idx]);
#ifndef FVO_RELEASE
                                g_spotProof.erase(g_spotVehicle[idx]);
                                g_spotMotion[idx].Reset();
                                g_factoryTest[idx] = {};
#endif
                                g_spot[idx]={};g_spotVehicle[idx]=self;
                            }
                            spottest::Input si;si.now=in.now;si.x=in.x;si.y=in.y;si.speed=spd;si.relocation=relocconsumers::Poll(self).serial;si.steps=g_shs[idx].testSteps.load(std::memory_order_relaxed);
                            si.eligible=idx>=1&&in.driverSeated&&!in.paused&&in.onWheels&&!planFlies;
                            si.covered=trafficarrival::Hidden();
                            const bool inCameraView=vehiclekeep::InCameraView(in.x,in.y,in.z);
                            si.unseen=!inCameraView||toPlayer>=spottest::rules::checkInViewFrom;
                            si.builtAt=vehiclediag::BuiltAt(self);
                            const auto er=planenergy::Latest(idx);
                            si.refused=er.at&&er.vehicle==self&&er.testRefused;  // T-2: the energy plan refused the last pulse
                            {std::lock_guard sl(g_spotLock);const auto rq=g_spotRequest.find(self);si.requested=rq!=g_spotRequest.end()&&GetTickCount64()-rq->second<3000;}
                            si.laneOffset=c0;si.laneHalfWidth=lane->halfWidth;si.upZ=g_shs[idx].probeTipped?-1.0:std::cos(double(g_shs[idx].bodyPitch))*std::cos(double(g_shs[idx].bodyRoll));
                            spottest::Event sev;
#ifndef FVO_RELEASE
                            auto& motion=g_spotMotion[idx];
                            const auto oldProof=motion.proof.completed;
                            const bool freshResult=er.at && nowMs >= er.at && nowMs-er.at <= 500 && er.vehicle==self&&!er.goal;
                            const bool published=freshResult&&er.ok&&!er.hover&&er.speed!=0;
                            const auto body=g_shs[idx].instance.load()?g_shs[idx].bodyId.load():0;
                            auto proofInput=si;
                            proofInput.unseen=!inCameraView;
                            if(factoryLease) {
                                proofInput.eligible &= lane->travel==1 && lane->held &&
                                    npcvehicles::FactoryLeaseOf(idx,self,true)==factoryLease &&
                                    !trafficpool::PlayerOwnsHandoff(self);
                                auto& test = g_factoryTest[idx];
                                spotAsk=test.Step(factoryLease,proofInput,body,nowMs,freshResult?er.spotPulseSerial:0,
                                    published&&er.factorySpotPulse,freshResult&&er.testRefused,in.heading,motion);
                                if (spotAsk.active) privatePulseSerial = spotAsk.serial;
                            } else {
                                auto& test = g_factoryTest[idx];
                                if(test.lease){test={};motion.Reset();}
                                spotAsk=spottest::Step(g_spot[idx],si,sev);
                                motion.Observe(proofInput,body,nowMs,published&&!er.factorySpotPulse?er.spotPulseSerial:0,in.heading,spotAsk);
                            }
                            if(motion.proof.completed!=oldProof){
                                const auto& proof=motion.proof;
                                {
                                    std::lock_guard sl(g_spotLock);
                                    if(proof.completed)g_spotProof[self]=proof;
                                    else g_spotProof.erase(self);
                                }
                                if(proof.completed)logger::Write(
                                    "OBS system=spot.motion vehicle=%08X body=%08X event=pulse_response built_at=%llu start=%llu completed=%llu dir=%s moved_m=%.4f steps=%llu",
                                    self,proof.body,proof.builtAt,proof.started,proof.completed,spottest::Name(proof.direction),proof.moved,proof.steps);
                            }
#else
                            spotAsk=spottest::Step(g_spot[idx],si,sev);
#endif
                            if(*sev.what)logger::Write("OBS system=spot.test vehicle=%08X pilot=%d event=%s reason=%s trigger=%s dir=%s moved_m=%.4f steps=%d fails=%d result=%s since_cover_ms=%lld pos=%.2f,%.2f",
                                self,idx,sev.what,sev.why,g_spot[idx].trigger,spottest::Name(sev.dir),sev.moved,sev.steps,g_spot[idx].fails,spottest::Name(g_spot[idx].result),
                                trafficarrival::closedAt.load()?(long long)(GetTickCount64()-trafficarrival::closedAt.load()):-1LL,in.x,in.y);
                            std::lock_guard sl(g_spotLock);g_spotResult[self]=spottest::Exported(g_spot[idx]);}
                        // World is gathered for the energy plan on its own clock: your car every update, others every 0.25 s.
                        const bool gather=idx==0||in.now-g_planStepAt[idx]>=.25||in.now<g_planStepAt[idx];
                        if(gather){
                        const auto tSurf0=std::chrono::steady_clock::now();
                        // The wanted surface along the line: the addon's road where it answers near the car's level, else the car's own level
                        // carried along the lane's rise. An open-air tile wants nothing touched. (Derived intent: older addons work the same.)
                        double wantZ[kSamples];bool wantHave[kSamples];double fieldEnd=-1,arriveHeading=std::numeric_limits<double>::quiet_NaN();const double roadZ0=carBase-onA.z;
                        // PF-1 (2026-11-21): the add-on's road answers and field boxes are reused while the car has hardly moved (under 0.3 m and
                        // 0.05 rad, 0.2 m in height, within 1 s): queued and slow cars asked the same questions every pass. On wheels only.
                        struct SurfCache{bool ok=false;std::uint32_t self=0;void* owner=nullptr;double x=0,y=0,heading=0,base=0,at=-1;int answer[kSamples]{};double fz[kSamples]{};int got=-1;FvoFieldBoxV1 raw[12];};
                        static SurfCache s_surf[planenergy::kSlots];auto& sc=s_surf[idx];
                        const bool reuseSurf=!flightLine&&sc.ok&&sc.self==self&&sc.owner==surfaceOwner&&in.now>=sc.at&&in.now-sc.at<1.0&&
                            std::hypot(in.x-sc.x,in.y-sc.y)<0.3&&std::fabs(std::remainder(in.heading-sc.heading,6.283185307179586))<0.05&&std::fabs(carBase-sc.base)<0.2;
                        if(reuseSurf)++costparts::g_surfaceReused;
                        else{sc.ok=!flightLine;sc.self=self;sc.owner=surfaceOwner;sc.x=in.x;sc.y=in.y;sc.heading=in.heading;sc.base=carBase;sc.at=in.now;sc.got=-1;}
                        for(int i=0;i<kSamples;++i){const auto p=lineAt(i*kStep);const double expected=flightLine?p.z:p.z+roadZ0;double fz=0;
                            int answer=0;
                            if(reuseSurf){answer=sc.answer[i];fz=sc.fz[i];}
                            else{answer=surfaceOwner?fieldgate::surfaceProvider(surfaceOwner,p.x,p.y,&fz):0;sc.answer[i]=answer;sc.fz[i]=fz;}
                            wantHave[i]=answer!=2;wantZ[i]=answer==1&&std::fabs(fz-expected)<4.0?fz:expected;
                            // planHover: the line is 3D. A surface is wanted only where one really is near the line (the addon's road, or measured
                            // ground); a line in the air over a road far below wants nothing touched there.
                            if(flightLine&&!(answer==1&&std::fabs(fz-expected)<4.0)){const auto m=navmap::At(p.x,p.y,p.z+2.0);
                                wantHave[i]=answer!=2&&m.haveGround&&std::fabs(m.ground-p.z)<4.0;wantZ[i]=wantHave[i]?double(m.ground):p.z;}}
                        const auto tProv1=std::chrono::steady_clock::now();
                        // Field boxes (step 3, fieldboxapi.h): the addon's boxes round the car. A Touch bottom picks the level among stacked decks;
                        // the measured or authored road near it gives the exact height (authored floors sit up to 1.4 m off the deck). No Touch
                        // face: nothing to touch (open air). No answer: the road reads exactly as before (older addons, D11 shapes).
                        fieldbox::Box fbox[12];int nbox=0;  // FB-1 (2026-11-12): up to 12 tiles ahead (short hairpin tiles); older addons send 3
                        if(fieldboxapi::provider&&surfaceOwner){FvoFieldBoxV1 raw[12];int got=0;if(reuseSurf&&sc.got>=0){got=sc.got;std::copy(sc.raw,sc.raw+got,raw);}
                            else{got=std::clamp(fieldboxapi::provider(surfaceOwner,in.x,in.y,carBase,raw,12),0,12);if(sc.ok){sc.got=got;std::copy(raw,raw+got,sc.raw);}}
                            int lineEnd=0;double endX=0,endY=0,endZ=0;arriveHeading=std::numeric_limits<double>::quiet_NaN();
                            for(int k=0;k<got;++k)if(fieldboxapi::ToBox(raw[k],fbox[nbox])){++nbox;if(raw[k].lineEnd>lineEnd){lineEnd=raw[k].lineEnd;endX=raw[k].endX;endY=raw[k].endY;endZ=raw[k].endZ;if(lineEnd==2&&std::isfinite(raw[k].arriveHeading))arriveHeading=raw[k].arriveHeading;}}
                            int bottomUsed=0,airUsed=0;
                            for(int i=0;i<kSamples&&nbox>0;++i){const auto p=lineAt(i*kStep);
                                for(int k=0;k<nbox;++k){const auto sf=fieldbox::SurfaceAt(fbox[k],p.x,p.y,wantZ[i]);if(!sf.inside)continue;
                                    if(!sf.touch){wantHave[i]=false;++airUsed;}
                                    else if(!sf.wall){if(std::fabs(wantZ[i]-sf.z)>1.5)wantZ[i]=sf.z;wantHave[i]=true;++bottomUsed;}  // the box picks the level; a nearby road keeps its exact height
                                    break;}}
                            if(lineEnd>=1&&std::isfinite(endX)&&std::isfinite(endY)){const auto pe=laneline::Project(*lane,endX,endY,endZ);const double to=pe.along-hereP.along;
                                if(to>=0&&to<=laneWindow.Length()-hereP.along+1.0&&std::hypot(endX-pe.x,endY-pe.y)<6.0)fieldEnd=to;}
                            fieldboxapi::lastLineEnd=lineEnd;
                            if(nbox!=g_fieldWas[idx]||in.now-g_fieldLogAt[idx]>=5||in.now<g_fieldLogAt[idx]){g_fieldWas[idx]=nbox;g_fieldLogAt[idx]=in.now;
                                logger::Write("OBS system=plan.field vehicle=%08X pilot=%d answered=%d boxes=%d bottom_samples=%d open_air_samples=%d line_end=%d end_in=%.1f contained=%d reason=%s",
                                    self,idx,got,nbox,bottomUsed,airUsed,lineEnd,fieldEnd,int(g_fieldHold[idx].contained),got==0?"no_boxes_here_road_reads_as_before":nbox<got?"some_boxes_malformed_dropped":"addon_boxes_used");}}
                        // Fix A (design section 2, wanted surface; user 2026-11-07): the field or road above only picked the LEVEL. The exact height is the
                        // measured floor in that level: the route survey's down rays first, then live floor hits (facing up) near the line. Authored
                        // heights are a straight line between route points and sit up to 1.4 m off the deck, so at a ramp's foot or crest the real
                        // floor bent away from them and its edge read as a 0.4 m face (23:40 / 23:59 runs). Unmeasured samples keep the old value.
                        const auto tBox1=std::chrono::steady_clock::now();
                        bool wantMeasured[kSamples]={};
                        // C1 (2026-11-07, source priority: contact, then rays, then the sweep, then the coarse map): on its wheels the car's own
                        // floor is the newest, closest measurement. 11:38 run: the addon floor sat 1.1 m under the wheels with 6 samples
                        // unmeasured, so the real road 2 m ahead read as a 1.23 m wall and Cruise stood 25 s until the user drove past by hand.
                        // The correction leads the samples ahead, fading over contactCarry m; Fix A below still replaces any with a measured floor.
                        double contactOffset=0;
                        // Touching: drive mode, the game measures ground under the car, and the car's base rests on it (not airborne, not falling).
                        const bool touchingGround=!planFlies&&in.onWheels&&in.groundOk&&std::isfinite(in.groundUnder)&&std::fabs(carBase-in.groundUnder)<.5;
                        if(touchingGround&&wantHave[0]){contactOffset=double(in.groundUnder)-wantZ[0];
                            if(std::fabs(contactOffset)>planbranch::rules::contactTrust){
                                for(int i=0;i<kSamples;++i){if(!wantHave[i])continue;const double fade=std::max(0.0,1.0-i*kStep/planbranch::rules::contactCarry);if(fade<=0)break;wantZ[i]+=contactOffset*fade;}
                                wantMeasured[0]=true;}
                            else contactOffset=0;}
                        // WM-1 (2026-11-18, one world map; design section 2): the car's own floor goes into the map (contact, the newest and closest
                        // measurement), the levels it can reach are flooded from where it stands, and every sample's height is the reachable map
                        // floor near the picked height (window wantWindow: reachability, not the authored line, picks the level). REMOVED: the
                        // survey's nearest-level-in-the-square answer, the live-hit and nav-map-level fallbacks, and the radar's wanted points.
                        {int fromMap=0,unmeasured=0;double most=0,reachMs=0;
                            if(touchingGround)routesurvey::NoteFloor(in.x,in.y,in.groundUnder,1.0);
                            const int reachLevels=routesurvey::UpdateReach(idx,in.x,in.y,touchingGround?double(in.groundUnder):carBase,in.now,reachMs);
                            double prevZ=0,prevPicked=0;bool havePrev=false;
                            for(int i=0;i<kSamples;++i){if(!wantHave[i])continue;
                                const double picked=wantZ[i];
                                if(i==0&&contactOffset!=0){prevZ=wantZ[0];prevPicked=picked;havePrev=true;continue;}  // C1: under the car, the wheels win
                                const auto p=lineAt(i*kStep);double mz=0;
                                const double ref=havePrev?prevZ+(picked-prevPicked):picked;  // carried along the measured floor behind it
                                if(routesurvey::FloorReachAt(idx,p.x,p.y,ref,planbranch::rules::wantWindow,mz)){++fromMap;wantMeasured[i]=true;most=std::max(most,std::fabs(mz-picked));wantZ[i]=mz;prevZ=mz;prevPicked=picked;havePrev=true;}
                                else ++unmeasured;}
                            if(in.now-g_surfaceLogAt[idx]>=5||in.now<g_surfaceLogAt[idx]){g_surfaceLogAt[idx]=in.now;
                                logger::Write("OBS system=plan.surface vehicle=%08X pilot=%d from_map=%d unmeasured=%d reachable_levels=%d reach_ms=%.2f largest_correction_m=%.2f contact_offset_m=%.2f reason=reachable_map_floor_gives_height",
                                    self,idx,fromMap,unmeasured,reachLevels,reachMs,most,contactOffset);}}
                        const auto tReach1=std::chrono::steady_clock::now();
                        fieldbox::Body body;body.centre={in.x,in.y,carBase+in.shape.height*.5};body.heading=in.heading;body.halfWidth=in.shape.halfWidth;body.halfLength=in.shape.halfLength;body.halfHeight=in.shape.height*.5;
                        fieldbox::JoinAtBends(fbox,nbox);  // D13: for containment; the surfaces above read the boxes as drawn
                        const double fieldShareNow=nbox>0?fieldbox::ShareFlat(fbox,nbox,body):-1.0;  // CH-1: also the health sample
                        const bool contained=nbox>0?g_fieldHold[idx].Update(in.now,fieldShareNow):(g_fieldHold[idx]=fieldbox::Containment{},false);
                        auto wanted=[&](double d,double){const int i=std::clamp(int(std::lround(d/kStep)),0,kSamples-1);planworld::Wanted w;w.have=wantHave[i];w.z=wantZ[i];w.measured=wantMeasured[i];w.step=in.limits.maxStep;w.faceStep=laneline::kFaceStep;return w;};
                        // World: real hit points, object boxes, vehicles, people. Each policy flag applies once, here.
                        // Script mode (2026-11-22, the user): no object detection. World keeps hazard boxes (planned separately), the known positions
                        // of other vehicles (each one's real box is its avoid distance) and people; every sensed source below is skipped.
                        const bool scriptMode=features::On(features::Id::ScriptMode);
                        if(idx>=0&&idx<int(sizeof g_scriptLogged/sizeof g_scriptLogged[0])&&g_scriptLogged[idx]!=int(scriptMode)){g_scriptLogged[idx]=int(scriptMode);
                            logger::Write("OBS system=world.script vehicle=%08X pilot=%d event=%s sources=%s reason=traffic_manager_script_mode",self,idx,scriptMode?"on":"off",
                                scriptMode?"hazard_boxes,vehicle_positions,people":"all_sensors");}
                        const bool useFieldSlice=nbox>0&&wantHave[0]&&effective.obstacles!=2&&!scriptMode;
                        // F1: with no road surface under the line the volume is cast too, beside the older sources (nothing is bypassed for it).
                        const bool volumeExtra=!scriptMode&&!useFieldSlice&&effective.obstacles!=2&&idx>=0&&(nbox==0||!wantHave[0])&&VolumeEverywhereOn();
                        const bool volumeAir=volumeExtra&&!wantHave[0];  // nothing is wanted under the car: any agreeing hit in its way is a solid
                        auto survey=routesurvey::ReadSweep();
                        const auto tWorld0=std::chrono::steady_clock::now();
                        pcSurfaceUs=std::chrono::duration<double,std::micro>(tWorld0-tSurf0).count();
                        pcProvUs=std::chrono::duration<double,std::micro>(tProv1-tSurf0).count();pcBoxUs=std::chrono::duration<double,std::micro>(tBox1-tProv1).count();
                        pcReachUs=std::chrono::duration<double,std::micro>(tReach1-tBox1).count();
                        static thread_local std::vector<Shape> shapes;shapes.clear();constexpr std::size_t kMaxShapes=128;int sweptOverBand=0,wholeBoxes=0,wholeTooBig=0,bandCells=0,sweepKnown=0,sweepShapes=0;  // WB-1 counts
                        const double span=std::max(double(lane->halfWidth),6.0);
                        auto tProbe0=tWorld0;
                        if(!useFieldSlice&&!scriptMode){
                        if(effective.obstacles!=2)for(int i=0;i<kSamples&&shapes.size()<kMaxShapes;i+=2){const auto p=lineAt(i*kStep);navmap::KeptHit kh[32];
                            const int n=navmap::HitsNear(p.x,p.y,wantZ[i]-1.0,wantZ[i]+3.5,span,kh,32);
                            for(int k=0;k<n&&shapes.size()<kMaxShapes;++k){bool dup=false;for(const auto& s:shapes)if(std::fabs(s.centre.x-kh[k].x)<.05&&std::fabs(s.centre.y-kh[k].y)<.05&&std::fabs(s.centre.z-kh[k].z)<.05){dup=true;break;}
                                if(dup)continue;Shape s;s.source=planworld::Source::Ray;s.centre={kh[k].x,kh[k].y,kh[k].z};s.owner=kh[k].owner;s.seenAt=in.now-kh[k].age;s.layer=kh[k].layer==255?-1:int(kh[k].layer);
                                s.haveNormal=kh[k].haveNormal;s.normalZ=kh[k].up;  // unsigned until the sign is proven (navmap::TakeNormalSigns)
                                shapes.push_back(s);}
                            double rowUx=0,rowUy=0;unsigned rowMask=0;
                            const bool surveyedRow=routesurvey::RowAt(*survey,p.x,p.y,wantZ[i],rowUx,rowUy,rowMask);
                            for(double lat=-span;lat<=span&&shapes.size()<kMaxShapes;lat+=2.0){const double x=p.x+std::cos(p.yaw)*lat,y=p.y-std::sin(p.yaw)*lat;
                                if(!objectAt(x,y,wantZ[i]+in.limits.maxStep+0.5))continue;
                                // The coarse map flattens objects into 4 m cells (design 2.1), so it calls open ground an object. Where the sweep has measured
                                // this spot and found no solid, the measurement wins. Unswept ground still counts as before.
                                {routesurvey::SweptSolid close[1];if(surveyedRow&&std::fabs(lat)<=planbranch::rules::scanHalf&&routesurvey::SolidsNear(*survey,x,y,wantZ[i],planbranch::rules::bandClear,close,1)==0){++sweptOverBand;continue;}}
                                {   // WB-1 (World, collision boxes kept whole): the game's own box behind this band cell, with its real size and heading, when
                                    // it reads tall to the wanted-surface test and is object-sized (deck pieces with railings stay band cells). Still a prediction.
                                    navmap::KeptBox kb[8];const int nb=navmap::BoxesNear(x,y,2.0,kb,8);bool whole=false;
                                    planworld::Wanted wl;wl.have=wantHave[i];wl.z=wantZ[i];wl.measured=wantMeasured[i];wl.step=in.limits.maxStep;wl.faceStep=laneline::kFaceStep;
                                    for(int k=0;k<nb&&shapes.size()<kMaxShapes;++k){const auto& b=kb[k];
                                        if(b.hx>planbranch::rules::wholeBoxMaxHalf||b.hy>planbranch::rules::wholeBoxMaxHalf){++wholeTooBig;continue;}
                                        const double dx=x-b.x,dy=y-b.y,c=std::cos(b.yaw),sn=std::sin(b.yaw),lx=dx*c-dy*sn,ly=dx*sn+dy*c;
                                        if(std::fabs(lx)>b.hx+1.0||std::fabs(ly)>b.hy+1.0)continue;
                                        Shape s;s.source=planworld::Source::Box;s.centre={b.x,b.y,.5*(b.zLo+b.zHi)};s.halfWidth=b.hx;s.halfLength=b.hy;s.halfHeight=.5*(b.zHi-b.zLo);s.heading=b.yaw;s.seenAt=in.now;
                                        if(planworld::Judge(s,wl,carBase,in.shape.height)!=planworld::Verdict::Tall)continue;
                                        whole=true;bool dup=false;for(const auto& o:shapes)if(o.source==planworld::Source::Box&&std::fabs(o.centre.x-s.centre.x)<.05&&std::fabs(o.centre.y-s.centre.y)<.05&&std::fabs(o.centre.z-s.centre.z)<.05){dup=true;break;}
                                        if(!dup){shapes.push_back(s);++wholeBoxes;}}
                                    if(whole)continue;}
                                ++bandCells;
                                Shape s;s.source=planworld::Source::Box;s.centre={x,y,wantZ[i]+1.5};s.halfWidth=s.halfLength=1.0;s.halfHeight=1.5;s.seenAt=in.now;shapes.push_back(s);}}
                        // Clear-path sweep (World; source priority in WORKING_MANDATE.md). Swept solids along the line are predictions: in World from far
                        // ahead, so the car plans round them long before it arrives. Live rays outrank them: a hit stands, and clean rays clear them.
                        if(effective.obstacles!=2){
                            for(const auto& s:shapes)if(s.source==planworld::Source::Ray&&routesurvey::KnownSolid(*survey,s.centre.x,s.centre.y,s.centre.z))++sweepKnown;  // live hits that confirm the sweep; they stay, rays outrank it
                            for(int i=0;i<kSamples&&sweepShapes<planbranch::rules::sweepShapes;i+=2){const auto p=lineAt(i*kStep);routesurvey::SweptSolid swept[24];
                                const int ns=routesurvey::SolidsNear(*survey,p.x,p.y,wantZ[i],planbranch::rules::sweepReach,swept,24);
                                for(int k=0;k<ns&&sweepShapes<planbranch::rules::sweepShapes;++k){bool dup=false;for(const auto& s:shapes)if(s.source==planworld::Source::Survey&&std::fabs(s.centre.x-swept[k].x)<.05&&std::fabs(s.centre.y-swept[k].y)<.05){dup=true;break;}if(dup)continue;
                                    Shape s;s.source=planworld::Source::Survey;s.centre={swept[k].x,swept[k].y,(swept[k].lo+swept[k].hi)*.5};s.halfWidth=s.halfLength=.5;s.halfHeight=(swept[k].hi-swept[k].lo)*.5;
                                    s.haveNormal=true;s.normalZ=0;s.seenAt=in.now;shapes.push_back(s);++sweepShapes;}}}
                        // Fix B (World): how many ray hits agree on each spot. One stray hit is doubt; several are a real surface.
                        {const double r2=planbranch::rules::confirmRadius*planbranch::rules::confirmRadius;
                            for(auto& a:shapes){if(a.source!=planworld::Source::Ray)continue;int n=0;
                                for(const auto& b:shapes)if(b.source==planworld::Source::Ray){const double dx=a.centre.x-b.centre.x,dy=a.centre.y-b.centre.y,dz=a.centre.z-b.centre.z;if(dx*dx+dy*dy+dz*dz<=r2)++n;}
                                a.support=n;}}
                        // T1 (World, trust by source; worldtrust.h): a prediction (sweep or coarse box) a clean ray passed through, or that confirmed
                        // live hits measured, no longer stands; the newest, closest measurement wins. With the rays down nothing is dropped.
                        if(idx>=0&&idx<int(sizeof g_trustStat/sizeof g_trustStat[0])){auto& ts=g_trustStat[idx];const bool live=vehicle_probe::RaysLive();
                            std::lock_guard cl(g_cleanLock);
                            for(std::size_t k=0;k<shapes.size();){const auto w=worldtrust::Judge(shapes[k],shapes.data(),int(shapes.size()),g_clean,in.now,live);
                                if(w==worldtrust::Why::Kept){if(planworld::Predicted(shapes[k]))++ts.kept;++k;continue;}
                                if(w==worldtrust::Why::Clean)++ts.clean;else ++ts.measured;
                                if(shapes[k].source==planworld::Source::Survey&&sweepShapes>0)--sweepShapes;
                                shapes.erase(shapes.begin()+std::ptrdiff_t(k));}
                            if(in.now-ts.logAt>=1.0||in.now<ts.logAt){if(ts.clean||ts.measured)logger::Write("OBS system=world.trust vehicle=%08X pilot=%d dropped_clean=%d dropped_measured=%d predictions_kept=%d rays_live=%d reason=newest_closest_measurement_wins",self,idx,ts.clean,ts.measured,ts.kept,int(live));
                                ts=TrustStat{};ts.logAt=in.now;}}
                        // SR-1 (2026-11-19) replaces World truth B1 here: one surface rule decides drivable (dropped) or solid (stops);
                        // a touch stands below as a stopping shape. With the rays down nothing changes (fail-safe: predictions may stop a car).
                        {double easedNear=1e9;
                            if(vehicle_probe::RaysLive()&&idx>=0&&idx<int(sizeof g_easedNear/sizeof g_easedNear[0])){
                                const double hx=std::sin(in.heading),hy=std::cos(in.heading);
                                for(std::size_t k=0;k<shapes.size();){auto& s=shapes[k];if(s.source!=planworld::Source::Ray){++k;continue;}
                                    // SR-1 (2026-11-19): the one surface rule (ray normal, the map's floor); drivable hits are dropped, the rest stop.
                                    const auto sv=SurfaceOf(idx,s.centre.x,s.centre.y,planworld::Top(s),s.haveNormal,s.normalZ,true);
                                    if(sv==surface::Verdict::Drivable||sv==surface::Verdict::Below){shapes.erase(shapes.begin()+std::ptrdiff_t(k));++g_truthStat[idx][0];continue;}  // SB-1: under the floor is dropped too
                                    ++g_truthStat[idx][2];(void)hx;(void)hy;
                                    ++k;}
                                const auto& bt=g_bodyTouch[idx];
                                if(bt.until>in.now){Shape s;s.source=planworld::Source::Ray;
#ifndef FVO_RELEASE
                                    s.sensor="body_check";
#endif
                                    s.centre={bt.x,bt.y,bt.z};s.halfWidth=s.halfLength=.4;s.haveNormal=true;s.normalZ=0;s.support=3;
                                    s.seenAt=in.now;s.owner=bt.owner;s.layer=bt.layer;shapes.push_back(s);++g_truthStat[idx][3];}
                                g_easedNear[idx]=easedNear;}
                            else if(idx>=0&&idx<int(sizeof g_easedNear/sizeof g_easedNear[0]))g_easedNear[idx]=1e9;}
                        // Look-ahead doubt probe (World, planprobe.h; user 2026-11-07): doubtful spots between the stopping distance and 40 m are
                        // measured now with direct rays, so the car knows before it arrives. Answers go into World's stores (navmap hits, the
                        // survey's floor); the next frame's World judges them (fixes A, B, F). Measures only; main thread only (CastNow).
                        survey.reset();  // direct clean rays can invalidate the shared sweep
                        tProbe0=std::chrono::steady_clock::now();
                        pcWorldUs+=std::chrono::duration<double,std::micro>(tProbe0-tWorld0).count();
                        if(idx>=0&&idx<int(sizeof g_probeRecent/sizeof g_probeRecent[0])){
                            auto& pr=g_probeStats[idx];
                            planprobe::Sample smp[kSamples];int ns=0;
                            for(int i=0;i<kSamples;++i){const auto p=lineAt(i*kStep);double mz=0;smp[ns++]={p.x,p.y,wantZ[i],i*kStep,p.yaw,wantHave[i],
                                wantHave[i]&&routesurvey::FloorAt(p.x,p.y,wantZ[i],planbranch::rules::levelWindow,mz)};}
                            planprobe::Target tg[planbranch::rules::probeTargets];
                            const int nt=planprobe::Pick(shapes.data(),int(shapes.size()),smp,ns,in.x,in.y,in.heading,spd,std::max(1.0,double(in.limits.brake)),span,in.now,g_probeRecent[idx],tg,planbranch::rules::probeTargets);
                            pr.targets+=nt;
                            // W1 (2026-11-07): each car gets its share of the frame budget (03:39 run: FE007807 used all 12 rays every frame, FF100039 got none all run).
                            int carRays=0,share=planbranch::rules::probeFrameRays;
                            if(nt>0&&routesurvey::OnMainThread()){
                                const unsigned long long frame=routesurvey::Frame();if(frame!=g_probeFrame){g_probeFrame=frame;g_probeRays=0;g_probeAskersLast=std::max(1,g_probeAskers);g_probeAskers=0;}
                                ++g_probeAskers;share=std::max(planbranch::rules::probeCarMin,planbranch::rules::probeFrameRays/g_probeAskersLast/3*3);pr.share=share;}
                            if(nt>0&&!routesurvey::OnMainThread())++pr.wrongThread;
                            else for(int k=0;k<nt;++k){
                                if(g_probeRays+3>planbranch::rules::probeFrameRays||carRays+3>share){++pr.budgetSkipped;continue;}
                                g_probeRays+=3;carRays+=3;g_probeRecent[idx].Note(tg[k].x,tg[k].y,in.now);
                                const double hx=std::sin(tg[k].heading),hy=std::cos(tg[k].heading);
                                const double bx1=tg[k].x,by1=tg[k].y;
                                auto cast=[&](float ax,float ay,float az,float bx,float by,float bz,bool down){
                                    const float a[3]={ax,ay,az},b[3]={bx,by,bz};prefetch::RayHit h;const int r=vehicle_probe::CastNow(a,b,0x22806,h);++pr.rays;
                                    if(r!=1){++pr.noAnswer;return;}
                                    if(!h.hit){++pr.clear;navmap::NoteRay(a,b,false,0,0,0,0,false,0.0f,true,0);if(!down){{std::lock_guard cl(g_cleanLock);g_clean.Add(a,b,in.now);}  // T1: clean evidence for every prediction it passed
                                        const int gone=routesurvey::ForgetSolid(bx1,by1);if(gone)logger::Observe("plan.probe",self,0x52ull,"event=prediction_cleared pilot=%d solids=%d pos=%.1f,%.1f reason=rays_passed_clean",idx,gone,bx1,by1);}return;}
                                    if(!h.formId)h.formId=loadedarea::HitBodyOwner(h.bodyId);
                                    navmap::NoteRay(a,b,true,h.x,h.y,h.z,h.layer,h.normalOk,h.nz,true,h.formId);
                                    const bool up=h.normalOk&&h.nz>.7;  // MB-1: signed (plan.normals: falling rays 3881 up, 3 down); a down-facing underside is never floor
                                    if(down&&up){routesurvey::NoteFloor(h.x,h.y,h.z,std::fabs(h.nz));++pr.floor;}else if(!up)++pr.face;else ++pr.floor;};
                                const float tx=float(tg[k].x),ty=float(tg[k].y),tz=float(tg[k].z);
                                cast(tx,ty,tz+2.0f,tx,ty,tz-2.0f,true);  // down onto the spot: a floor here?
                                const float bx=float(tg[k].x-hx*3.0),by=float(tg[k].y-hy*3.0),ex=float(tg[k].x+hx*1.0),ey=float(tg[k].y+hy*1.0);
                                cast(bx,by,tz,ex,ey,tz,false);            // through it at its height
                                cast(bx,by,tz-.2f,ex,ey,tz-.2f,false);    // and 0.2 m lower
                                if(tg[k].kind==planprobe::Kind::UnmeasuredFloor)++pr.floorTargets;}
                            if(in.now-pr.logAt>=1||in.now<pr.logAt){
                                if(pr.targets||pr.wrongThread||pr.budgetSkipped)
                                    logger::Write("OBS system=plan.probe vehicle=%08X pilot=%d targets=%d floor_targets=%d rays=%d floor=%d face=%d clear=%d no_answer=%d budget_skipped=%d wrong_thread=%d share=%d reason=resolve_doubt_before_arrival",
                                        self,idx,pr.targets,pr.floorTargets,pr.rays,pr.floor,pr.face,pr.clear,pr.noAnswer,pr.budgetSkipped,pr.wrongThread,pr.share);
                                pr=ProbeStats{};pr.logAt=in.now;}}
                        }
                        const auto tWorld1=std::chrono::steady_clock::now();
                        if(!useFieldSlice&&!scriptMode)pcProbeUs=std::chrono::duration<double,std::micro>(tWorld1-tProbe0).count();
                        {   // Vehicles and people from World's per-frame store; each policy flag applies once, here.
                            const auto& W=WorldThisFrame(in.now);static thread_local planworld::Shape around[96];const int nn=W.Near(in.x,in.y,90.0,self,around,96);
                            for(int k=0;k<nn&&shapes.size()<kMaxShapes+96;++k){const auto& s=around[k];
                                if(scriptMode&&s.source!=planworld::Source::Vehicle&&s.source!=planworld::Source::Person)continue;  // Script mode: known positions only
                                if(s.source==planworld::Source::Vehicle){
                                    if(effective.moving==2||(useFieldSlice&&s.steerRound&&!planworld::Moving(s)))continue;
                                }
                                else if(s.source==planworld::Source::Person){if(effective.people==2||std::hypot(s.centre.x-in.x,s.centre.y-in.y)>40.0)continue;
                                    // RD-1 (World, G7 "ignore what is under the own hull"): a person inside this car's own body is its rider (driver, passenger,
                                    // the player on Cruise), never a person in its way. 16:09 run: FE007807 held at 0 m/s "by person" 2 m ahead, which was the
                                    // player in the driver's seat (1.4 m from the car's centre, 2.3 m up); the game's aboard answer missed them.
                                    const double dx=s.centre.x-in.x,dy=s.centre.y-in.y,hr=std::cos(in.heading),hs=std::sin(in.heading);
                                    const double lat=dx*hr-dy*hs,lon=dx*hs+dy*hr,up=s.centre.z-carBase;
                                    if(planworld::InsideBody(s.centre.x,s.centre.y,s.centre.z,in.x,in.y,carBase,in.heading,in.shape.halfWidth,in.shape.halfLength,in.shape.height)){  // same box, pure (planworld.h)
                                        logger::Observe("plan.people",self,0x52ull,"event=rider_skipped pilot=%d at=%.1f,%.1f,%.1f lat=%.2f lon=%.2f up=%.2f reason=person_inside_own_body_is_a_rider",idx,s.centre.x,s.centre.y,s.centre.z,lat,lon,up);
                                        continue;}
                                    // RD-2 (World, 2026-11-07): a person inside another vehicle's body is that vehicle's rider; the vehicle shape already stands
                                    // for it (22:53 run: FE007807 "held by person" 81 times by the driver seated in FF108D7C at -749.1,1239.1). Same box as RD-1.
                                    {std::uint32_t inside=0;
                                     for(int j=0;j<nn&&!inside;++j){const auto& o=around[j];if(o.source!=planworld::Source::Vehicle)continue;
                                        if(planworld::InsideBody(s.centre.x,s.centre.y,s.centre.z,o.centre.x,o.centre.y,o.centre.z-o.halfHeight,o.heading,o.halfWidth,o.halfLength,2*o.halfHeight))inside=o.owner?o.owner:1u;}
                                     if(inside){logger::Observe("plan.people",self,0x53ull,"event=rider_skipped pilot=%d at=%.1f,%.1f,%.1f owner=%08X reason=person_inside_another_vehicle",idx,s.centre.x,s.centre.y,s.centre.z,inside);continue;}}}
                                shapes.push_back(s);}}
                        {   // Contact is a sensor (G6): pushing without moving puts a shape at the nose for 8 s, so the next branches go round it.
                            // One event on the contact bus (contactevent.h); World reads it back as a shape, health counts it.
                            contactevent::Event live[8];const int nl=g_contacts.For(self,in.now,live,8);
                            for(int k=0;k<nl&&!scriptMode;++k){  // Script mode: contact inferences are detection too
                                if(useFieldSlice&&live[k].kind==contactevent::Kind::Push)continue;
                                Shape s;s.source=planworld::Source::Contact;s.centre={live[k].x,live[k].y,live[k].z};s.halfWidth=live[k].halfWidth;s.halfLength=.4;s.halfHeight=1.0;s.heading=live[k].heading;
                                s.seenAt=live[k].at;s.expires=live[k].expires;
#ifndef FVO_RELEASE
                                s.sensor=live[k].kind==contactevent::Kind::Push?"inferred_push":"contact";
#endif
                                shapes.push_back(s);}}
                        pcWorldUs+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-tWorld1).count();
                        planrun::Frame f;f.now=in.now;f.speed=spd;f.lateral=c0;f.station=stNow;f.x=in.x;f.y=in.y;f.carBase=carBase;
                        f.topSpeed=f.want=planbranch::WantedSpeed(lane->speed,flightLine?in.limits.hoverTop:in.limits.driveTop,idx>=1);
                        {   // PD-1c: say why the line holds or slows the car (route data), so a stop is never "held by nothing"
                            const double carTop=planbranch::WantedSpeed(1e9,flightLine?in.limits.hoverTop:in.limits.driveTop,idx>=1);
                            f.heldWhy=routeguidance::HeldWhy(int(lane->held));f.wantWhy=f.want<carTop-.05?"lane speed limit":"";}
                        // SC-1 (Speed, design section 7: the possible speed; user 2026-11-18): only plan speeds the car can hold. The 14:25 run:
                        // 16.5 m/s over a crest, the car went light (1-2 wheels touching, slip 0.8), slid 60 degrees and hit the wall it had planned
                        // round. Crest: from the map's measured floor ahead. Slide: sideways speed (or off its wheels) holds the speed down.
                        if(!flightLine&&!planFlies){double crestCap=1e9;int crestAt=-1;
                            for(int i=2;i+2<kSamples;++i){if(!(wantMeasured[i-2]&&wantMeasured[i]&&wantMeasured[i+2]))continue;
                                const double bend=(wantZ[i-2]-2*wantZ[i]+wantZ[i+2])/(4*kStep*kStep);if(bend>=-.01)continue;  // only a crest (the road bends down)
                                const double vc=std::sqrt(planbranch::rules::crestLift*planbranch::rules::gravity/-bend),reach=std::sqrt(vc*vc+2*planbranch::rules::crestBrake*i*kStep);
                                if(reach<crestCap){crestCap=reach;crestAt=i;}}
                            double bendCap=1e9;int bendAt=-1;  // BC-1: the lane line's bends ahead
                            for(int i=1;i+1<kSamples;++i){const auto a=lineAt((i-1)*kStep),c=lineAt((i+1)*kStep);
                                const double k=std::fabs(std::remainder(c.yaw-a.yaw,2*3.14159265358979))/(2*kStep);
                                const double r=planbranch::rules::BendCap(k,i*kStep,wheelgrip::PlanGrip(g_gs[idx>=0&&idx<kPilots?idx:0].gear,g_gripSet));if(r<bendCap){bendCap=r;bendAt=i;}}
                            if(bendCap<crestCap){crestCap=bendCap;crestAt=bendAt;}
                            double gradeCap=1e9;int gradeAt=-1;  // SG-1: the measured floor's grade ahead over ~4 m (planspeed::GradeCap); downhill only:
                            for(int i=0;i+2<kSamples;++i){if(!(wantMeasured[i]&&wantMeasured[i+2]))continue;  // uphill is the tyre push's (SP-1), never a cap
                                const double g=std::min(0.0,(wantZ[i+2]-wantZ[i])/(2*kStep));
                                const double r=planspeed::GradeCap(f.topSpeed,g,i*kStep,planbranch::rules::crestBrake,carcap::kCar.MaxGrade(wheelgrip::PlanGrip(g_gs[idx>=0&&idx<kPilots?idx:0].gear,g_gripSet)));  // SE: the grade the car climbs, from its record
                                if(r<gradeCap){gradeCap=r;gradeAt=i;}}
                            if(gradeCap<crestCap){crestCap=gradeCap;crestAt=gradeAt;}
                            static double slideUntil[1+prefetch::kSlotPilotCount]{},slideCap[1+prefetch::kSlotPilotCount]{},capLogAt[1+prefetch::kSlotPilotCount]{};
                            const double sideways=std::fabs(in.vx*std::cos(in.heading)-in.vy*std::sin(in.heading));
                            if(idx>=0&&idx<=prefetch::kSlotPilotCount){
                                if(sideways>planbranch::rules::slideLimit||!in.onWheels){slideUntil[idx]=in.now+planbranch::rules::slideHold;slideCap[idx]=std::max(planbranch::rules::slideFloor,spd*planbranch::rules::slideKeep);}
                                const bool sliding=in.now<slideUntil[idx]&&in.now>=slideUntil[idx]-planbranch::rules::slideHold-1e-6;
                                const double before=f.want;
                                if(crestCap<f.want)f.want=crestCap;
                                if(sliding&&slideCap[idx]<f.want)f.want=slideCap[idx];
                                const double trackCap=g_trackCap[idx].load(std::memory_order_relaxed);  // TK-1: the Drive stage's tracking cap (last tick)
                                const bool trackBinds=std::isfinite(trackCap)&&trackCap>0&&trackCap<f.want;
                                if(trackBinds)f.want=trackCap;
                                if(f.want<before-.05){f.wantWhy=trackBinds?"tracking: the car is not following its plan well":sliding&&slideCap[idx]<=crestCap?"sliding: holding the speed down":gradeCap<1e8&&gradeCap==crestCap?"steep road down ahead: holding the speed the slope allows":bendAt==crestAt&&bendCap<1e8?"bend ahead: the tyres must hold the turn":"crest ahead: wheels must stay on the road";
                                    if(in.now-capLogAt[idx]>=1.0||in.now<capLogAt[idx]){capLogAt[idx]=in.now;
                                        logger::Write("OBS system=plan.speedcap vehicle=%08X pilot=%d wanted=%.1f capped=%.1f crest_cap=%.1f crest_at_m=%.0f bend_cap=%.1f grade_cap=%.1f sliding=%d sideways=%.2f on_wheels=%d speed=%.1f reason=%s",
                                            self,idx,before,f.want,crestCap<1e8?crestCap:-1.0,crestAt*kStep,bendCap<1e8?bendCap:-1.0,gradeCap<1e8?gradeCap:-1.0,int(sliding),sideways,int(in.onWheels),spd,f.wantWhy);}}}}
                        // AG-1 (autogear.h): a car the autopilot drives on its wheels (Cruise, AI) picks its own gear from the game's wheel slip, the
                        // steepest climb of the map's floor in the next 16 m, and skids. The player's own driving keeps the gear key.
                        if(!flightLine&&!planFlies&&idx>=0&&idx<kPilots&&(idx>=1||g_gs[idx].autopilot||g_gs[idx].playerAuto)){
                            static autogear::State ag[kPilots];static double agLogAt[kPilots]{};
                            double grade=0;if(wantMeasured[0]&&wantMeasured[2])grade=(wantZ[2]-wantZ[0])/(2*kStep);  // SP-1: the road just ahead, for the tyre push and the trace (not a gear cause)
                            g_roadGrade[idx].store(float(grade),std::memory_order_relaxed);
                            const double side=std::fabs(in.vx*std::cos(in.heading)-in.vy*std::sin(in.heading));
                            const double slipNow=g_rearSlipDeg[idx].load(std::memory_order_relaxed);  // RB-1: the rear slip angle (deg), not WheelInfo +0xB4
                            if(ag[idx].at<0)ag[idx].gear=std::clamp(g_gs[idx].gear,1,3);
                            const int was=g_gs[idx].gear,gear=autogear::Update(ag[idx],in.now,slipNow,side);
                            if(gear!=was){g_gs[idx].gear=gear;
                                logger::Write("OBS system=vehicle.gear vehicle=%08X pilot=%d event=auto_shift from=%d to=%d rear_slip_deg=%.2f grade=%.2f sideways=%.2f speed=%.1f turning_weight=%.2f reason=%s",
                                    self,idx,was,gear,ag[idx].slip,grade,side,spd,wheelgrip::ProfileOf(gear,g_gripSet).weight,
                                    gear<was?(ag[idx].slip>=autogear::rules::slip2?"back_stepping_out":"skidding"):"cause_gone_shift_up");
                                if(idx==0){const auto pr=wheelgrip::ProfileOf(gear,g_gripSet);notify::Log("Gear %d (auto)  grip x%.2f, rear x%.2f, turning weight x%.2f",gear,pr.friction,pr.friction*pr.rear,pr.weight);}}
                            else if(in.now-agLogAt[idx]>=5||in.now<agLogAt[idx]){agLogAt[idx]=in.now;
                                logger::Write("OBS system=vehicle.gear vehicle=%08X pilot=%d event=hold gear=%d rear_slip_deg=%.2f raw_rear_slip_deg=%.2f grade=%.2f sideways=%.2f reason=auto_gear_inputs",self,idx,gear,ag[idx].slip,slipNow,grade,side);}}
                        f.grip=wheelgrip::PlanGrip(g_gs[idx>=0&&idx<kPilots?idx:0].gear,g_gripSet);  // GT-1: the plan turns within the gear's grip
                        f.flightLine=flightLine;f.flying=planFlies&&!in.onWheels;  // NH-1: flying = in the air now (on drive only it comes down)  // planHover: a 3D line; Mode starts from flight for a car in the air
                        f.accel=railforce::kAccel;f.brake=std::max(1.0,in.limits.brake);f.car.halfWidth=in.shape.halfWidth;f.car.halfLength=in.shape.halfLength;f.car.height=in.shape.height;
                        f.travel=lane->travel;f.hoverPossible=in.hoverPossible;f.paused=in.paused;f.driverSeated=in.driverSeated;f.touching=in.contact||g_contacts.Count(self,in.now-1.0)>0;f.held=lane->held;  // Progress listens to the contact bus too
                        f.lineEnd=lane->held?0.0:lane->stopDistance>=0?double(lane->stopDistance):1e9;f.aboveSurface=wantHave[0]?carBase-wantZ[0]:99;
                        if(fieldEnd>=0)f.lineEnd=std::min(f.lineEnd,fieldEnd);  // line_end stop or arrive: the line ends there
                        {   // The goal door (goaldoor.h): this lane's goal goes in through the one door; Route reads its ending and facing back.
                            goaldoor::Goal dg;dg.source=idx==0?goaldoor::Source::Cruise:goaldoor::Source::Lane;dg.identity=lane->identity;dg.at=in.now;
                            dg.ending=fieldboxapi::provider&&surfaceOwner&&fieldEnd>=0?goaldoor::LaneEnding(fieldboxapi::lastLineEnd,false):goaldoor::Ending::Pass;
                            if(fieldEnd>=0){const auto pe=lineAt(fieldEnd);dg.x=pe.x;dg.y=pe.y;dg.z=pe.z;}else{const auto pe=lineAt((kSamples-1)*kStep);dg.x=pe.x;dg.y=pe.y;}
                            dg.heading=dg.ending>=goaldoor::Ending::Arrive?arriveHeading:std::numeric_limits<double>::quiet_NaN();dg.keepGoing=dg.ending==goaldoor::Ending::Pass;
                            if(g_door.Admit(idx,dg))logger::Observe("plan.door.lane",self,dg.identity^std::uint64_t(dg.ending),"event=admit pilot=%d source=%s ending=%s at=%.1f,%.1f heading=%.0f identity=%016llX reason=lane_goal note=PH-2_one_line_per_goal",
                                idx,goaldoor::Name(dg.source),goaldoor::Name(dg.ending),dg.x,dg.y,std::isfinite(dg.heading)?dg.heading*57.29578:-1.0,(unsigned long long)dg.identity);
                            const auto* door=g_door.Of(idx);
                            if(door&&door->ending>=goaldoor::Ending::Arrive&&fieldEnd>=0&&fieldEnd<20.0&&std::isfinite(door->heading))f.facing=door->heading;  // arrive: the final heading (Route's facing)
                            if(door&&std::isfinite(door->facing))f.facing=door->facing;
                            if(jobLineCombat&&g_combatAim[idx].aim)f.facing=std::atan2(g_combatAim[idx].x-in.x,g_combatAim[idx].y-in.y);}  // CB-4: combat line: the nose on the aim point (Route's facing)
                        f.field=nbox>0?fbox:nullptr;f.fieldCount=nbox;f.contained=contained;
                        if(g_travelWhy[idx]&&double(GetTickCount64())/1000.0-g_travelAt[idx]<1.0){if(g_travelAir[idx])f.airWhy=g_travelWhy[idx];else if(g_travelBlock[idx])f.groundWhy=g_travelWhy[idx];}  // only a blocking drive reason (cramped) forbids flight  // G37: none of travelmode's reasons dropped
                        f.goalDistance=laneWindow.Length()-hereP.along;f.headingError=std::fabs(std::remainder(lyaw-in.heading,6.283185307179586));
                        if(jobLineCombat&&planFlies&&!in.onWheels&&std::isfinite(f.facing))f.headingError=0;  // CB-4: a hover aiming on a combat line strafes; its nose is not "facing away from the line"
                        f.lanes.ownLeft=-lane->halfWidth;f.lanes.ownRight=lane->halfWidth;
                        if(ld.roadLanesOk){f.lanes.count=std::min(ld.roadLanes.count,planbranch::kMaxLanes);f.lanes.own=ld.roadLanes.own;
                            for(int k=0;k<f.lanes.count;++k){f.lanes.left[k]=ld.roadLanes.left[k];f.lanes.right[k]=ld.roadLanes.right[k];f.lanes.sameWay[k]=ld.roadLanes.sameWay[k];f.lanes.open[k]=true;}
                            if(f.lanes.own>=0){f.lanes.ownLeft=f.lanes.left[f.lanes.own];f.lanes.ownRight=f.lanes.right[f.lanes.own];}
                            // Route edge no-go bands (additive: an older addon leaves them 0 = no band known). The planner reads them as known edges.
                            if(ld.roadLanes.size>=sizeof(FvoRoadLanesV1)){const double bl=ld.roadLanes.bandLow,bh=ld.roadLanes.bandHigh;
                                f.lanes.bandLow=std::isfinite(bl)&&bl>0&&bl<=50?bl:0;f.lanes.bandHigh=std::isfinite(bh)&&bh>0&&bh<=50?bh:0;}
                            // C3 / C4 (lanecoord.h): the coordinator's lane permissions are route data. A lane it closes is not a lane for this car this
                            // update; the car's own lane is never closed. No provider, no answer or another lane's answer: every listed lane is open, as before.
                            if(fieldgate::lanePermitProvider&&idx>=0&&routeguidance::frames[idx].owner){lanecoord::PermitV1 pm;pm.identity=lane->identity;
                                static unsigned permitWas[1+prefetch::kSlotPilotCount];unsigned closed=0;int why=0;
                                if(fieldgate::lanePermitProvider(routeguidance::frames[idx].owner,&pm)==1&&pm.size==sizeof(pm)&&pm.version==1&&pm.identity==lane->identity&&pm.count==f.lanes.count)
                                    for(int k=0;k<f.lanes.count;++k)if(k!=f.lanes.own&&!pm.open[k]){f.lanes.open[k]=false;closed|=1u<<k;if(!why)why=pm.reason[k];}
                                if(closed!=permitWas[idx]){permitWas[idx]=closed;logger::Observe("plan.permit",self,closed,"event=lane_permissions pilot=%d lanes=%d own=%d closed_mask=%02X first_reason=%s reason=coordinator_route_data_closes_lanes_for_this_car",idx,f.lanes.count,f.lanes.own,closed,lanecoord::Name(why));}}}
                        if(useFieldSlice||volumeExtra){
                            struct CachedSlice { fieldslice::Slice slice; unsigned vehicle=0; double at=-1, base=0; };
                            static CachedSlice slices[1+prefetch::kSlotPilotCount];
                            auto& cached=slices[idx];
                            auto& slice=f.slice;
                            slice.active=true;slice.x=in.x;slice.y=in.y;slice.yaw=in.heading;
                            slice.start=in.shape.halfLength+.15;
                            slice.end=std::min({fieldslice::Depth(spd,f.brake,in.shape.halfLength),f.goalDistance,f.lineEnd});
                            double left=f.lanes.ownLeft,right=f.lanes.ownRight;
                            for(int k=0;k<f.lanes.count;++k)if(f.lanes.open[k]){
                                left=std::min(left,f.lanes.left[k]);right=std::max(right,f.lanes.right[k]);
                            }
                            slice.vehicleHalf=fieldslice::VehicleHalf(in.shape.halfWidth,spd);  // vehicle layer: never narrower than car + margin
                            // The field gives the volume its extent (user, 2026-11-11): the authored boxes say where legal space is across the road just
                            // ahead of the car; beams are laid across that and none where there is no field. Without a field there, the open lanes.
                            double fieldLeft=left,fieldRight=right;bool fieldWidthFound=false;
                            if(nbox>0){
                                const double probeD=in.shape.halfLength+1.0,hx=std::sin(in.heading),hy=std::cos(in.heading),pz=carBase+.8;
                                const auto inFieldAt=[&](double lineLateral){const double side=lineLateral-c0,px=in.x+hx*probeD+hy*side,py=in.y+hy*probeD-hx*side;
                                    for(int k=0;k<nbox;++k)if(fieldbox::Contains(fbox[k],{float(px),float(py),float(pz)},.05))return true;return false;};
                                if(inFieldAt(c0)){double lo=c0,hi=c0;while(lo>c0-12.0&&inFieldAt(lo-.25))lo-=.25;while(hi<c0+12.0&&inFieldAt(hi+.25))hi+=.25;
                                    fieldLeft=lo;fieldRight=hi;fieldWidthFound=true;}
                            }
                            slice.roadLeft=fieldLeft-c0;slice.roadRight=fieldRight-c0;  // legal space across, from the car's own line
                            {double placeLeft=0,placeSpacing=0;
                             if(!fieldslice::Place(fieldLeft,fieldRight,c0,slice.vehicleHalf,placeLeft,placeSpacing)){placeLeft=-slice.vehicleHalf;placeSpacing=2*slice.vehicleHalf/(fieldslice::columns-1);}
                             slice.left=placeLeft;slice.spacing=placeSpacing;}
                            const bool main=routesurvey::OnMainThread();
                            const bool fresh=cached.vehicle==self&&in.now>=cached.at&&in.now-cached.at<.25&&
                                std::hypot(in.x-cached.slice.x,in.y-cached.slice.y)<1.0&&std::abs(carBase-cached.base)<.4&&
                                std::abs(std::remainder(in.heading-cached.slice.yaw,6.283185307179586))<.10;
                            static unsigned long long maskFrame=~0ull;static int maskRays=0;  // Field Mask's own frame budget, apart from the doubt probe's
                            if(main){
                                const auto frame=routesurvey::Frame();
                                if(frame!=maskFrame){maskFrame=frame;maskRays=0;}
                            }
                            // The volume lives in the field's own frame: as the car moves forward the old depths shorten by the distance travelled, so a
                            // measurement stays good at speed and between refreshes (redundancy in time).
                            const double cdx=in.x-cached.slice.x,cdy=in.y-cached.slice.y,csn=std::sin(cached.slice.yaw),ccs=std::cos(cached.slice.yaw);
                            const double moved=cdx*csn+cdy*ccs,drift=cdx*ccs-cdy*csn;  // forward travel and sideways drift since that volume, in its frame
                            const bool canReuse=cached.vehicle==self&&in.now>=cached.at&&moved>=-0.3&&moved<=8.0&&std::abs(drift)<.6&&
                                std::abs(carBase-cached.base)<.6&&std::abs(std::remainder(in.heading-cached.slice.yaw,6.283185307179586))<.10&&
                                std::abs(drift+slice.left-cached.slice.left)<.4&&std::abs(cached.slice.spacing-slice.spacing)<.3;
                            // Gates are synchronization, not holes (user, 2026-11-11): while a probe gate is closed nothing is cast and nothing counts as
                            // refused; the volume is carried and casting resumes when the gate opens. The gates themselves are unchanged.
                            int gateWhy=0;const bool gateOpen=main&&vehicle_probe::GateOpen(gateWhy);
                            bool fullRefresh=!canReuse;  // nothing to carry (first refresh, turned, jumped): every column is cast once
                            if(fullRefresh&&maskRays+fieldslice::CastsPerRefresh(slice.start,slice.end,true)>planbranch::rules::maskFrameRays)fullRefresh=false;  // no room this frame: one phase now, the rest on the next refreshes
                            const int castNeed=fieldslice::CastsPerRefresh(slice.start,slice.end,fullRefresh);
                            {static fieldslice::Memory memories[1+prefetch::kSlotPilotCount];static unsigned memoryFor[1+prefetch::kSlotPilotCount]={};  // OA-1: object memory, world space, kept through turns and full refreshes
                             if(memoryFor[idx]!=self){memories[idx]={};memoryFor[idx]=self;}  // another car in this slot: its memory is not this car's
                             {static std::uint64_t memoryMark[1+prefetch::kSlotPilotCount]={};const auto mark=npcvehicles::RelocationMark(self);
                              if(mark!=memoryMark[idx]){memories[idx]={};memoryMark[idx]=mark;logger::Observe("plan.memory",self,0x30u+(mark&1u),"event=flushed reason=pose_relocated mark=%llu",(unsigned long long)mark);}}  // the old surroundings are not where the car is now
                             slice.memory=&memories[idx];}
                            if(fresh)slice=cached.slice;
                            else if(main&&!gateOpen&&canReuse&&slice.end>slice.start+1){
                                int carried=0;
                                for(int col=0;col<fieldslice::columns;++col)for(int h=0;h<fieldslice::levels;++h)
                                    if(fieldslice::Carry(cached.slice.rays[col][h],in.now,moved,slice.start,cached.slice.start,slice.rays[col][h]))++carried;
                                slice.phase=cached.slice.phase;slice.base=carBase;slice.grade=cached.slice.grade;slice.heightAbove=cached.slice.heightAbove;
                                cached={slice,self,in.now,carBase};
                                logger::Observe("plan.slice",self,0x47ull|(std::uint64_t(gateWhy)<<8),"event=gate_closed pilot=%d gate=%s carried=%d of=%d depth=%.1f reason=gates_are_synchronization_nothing_cast_volume_carried",
                                    idx,vehicle_probe::RefusalName(gateWhy),carried,fieldslice::rayCount,slice.end);
                            }
                            else if(main&&gateOpen&&slice.end>slice.start+1&&slice.spacing>0&&maskRays+castNeed<=planbranch::rules::maskFrameRays){
                                maskRays+=castNeed;  // one cast per segment per beam of this refresh's phase (all phases on a full refresh)
                                const auto probeStart=std::chrono::steady_clock::now();
                                const double sx=std::sin(slice.yaw),sy=std::cos(slice.yaw);
                                const auto surfaceAt=[&](double distance){
                                    const auto i=std::clamp(int(distance/kStep),0,kSamples-2);
                                    const double u=std::clamp(distance/kStep-i,0.0,1.0);
                                    return (1-u)*wantZ[i]+u*wantZ[i+1];
                                };
                                slice.base=carBase;
                                slice.grade=(surfaceAt(slice.end)-wantZ[0])/slice.end;
                                for(int h=0;h<fieldslice::levels;++h)slice.heightAbove[h]=fieldslice::Height(h,in.shape.height);  // this car's own vertical profile
                                const auto inside=[&](const std::array<float,3>& point){
                                    if(nbox==0)return true;  // no authored field (a job, combat or summon line): all space is legal, as planrun::PoseInField says
                                    for(int k=0;k<nbox;++k)if(fieldbox::Contains(fbox[k],{point[0],point[1],point[2]},.05))return true;
                                    return false;
                                };
                                static_assert(fieldslice::terrainLayers==kTerrainLayers,"the mask's terrain layers are the game's terrain layers");
                                int reused=0,rjOutside=0,rjZero=0,rjNoNormal=0,rjSelf=0,rjRange=0,rjFieldEnd=0,clearFloor=0,clearRoadUser=0,clearAllowed=0,clearTerrain=0,clearGround=0,hitUnderside=0;  // the cell vocabulary and its reasons (trace everything)
                                int refused[vehicle_probe::RefusedKinds]={};
                                // Redundancy in time: this refresh casts one phase of the columns; the others are carried forward from the car's own volume.
                                const int phase=fullRefresh?0:(cached.vehicle==self?(cached.slice.phase+1)%fieldslice::phases:0);slice.phase=phase;
                                int hitsSeen=0,casts=0,floorMeasured=0,floorLinear=0,carried=0;
                                int downRays=0,groundFromDown=0,clearOverhead=0;  // WT-1 / WT-2 (trace everything)
                                for(int col=0;col<fieldslice::columns;++col)for(int h=0;h<fieldslice::levels;++h){
                                    const double lateral=slice.left+col*slice.spacing;
                                    const double height=slice.heightAbove[h];  // fitted to this car's own height
                                    const double lx=sy*lateral,ly=-sx*lateral;  // this column's sideways offset
                                    // The field walk uses the straight grade (the boxes are generous in height); the casts follow the measured floor.
                                    const auto point=[&](double d){
                                        return std::array<float,3>{float(in.x+sx*d+lx),float(in.y+sy*d+ly),float(slice.base+slice.grade*d+height)};
                                    };
                                    const auto pointAt=[&](double d){   // the floor under this column at distance d: measured when the survey has it
                                        const double px=in.x+sx*d+lx,py=in.y+sy*d+ly;double mz=0,fz;
                                        if(routesurvey::FloorAt(px,py,surfaceAt(d),planbranch::rules::levelWindow,mz)){fz=mz;++floorMeasured;}
                                        else{fz=slice.base+slice.grade*d;++floorLinear;}
                                        return std::array<float,3>{float(px),float(py),float(fz+height)};
                                    };
                                    auto& ray=slice.rays[col][h];
                                    if(!inside(point(slice.start))){ray={fieldslice::Answer::Clear,slice.start,true,in.now};++rjOutside;continue;}  // no legal space here: not measured is the wrong word
                                    double rayEnd=slice.start;
                                    while(rayEnd<slice.end){
                                        const double next=std::min(slice.end,rayEnd+kStep);
                                        if(!inside(point(next)))break;
                                        rayEnd=next;
                                    }
                                    if(rayEnd<=slice.start){ray={fieldslice::Answer::Clear,slice.start,true,in.now};++rjZero;continue;}
                                    if(!fullRefresh&&!fieldslice::InPhase(col,phase)){  // another phase's column: carried forward, cast on its own refresh
                                        if(canReuse&&fieldslice::Carry(cached.slice.rays[col][h],in.now,moved,slice.start,cached.slice.start,ray))++carried;
                                        continue;}
                                    const bool fieldEnds=rayEnd<slice.end-.01;
                                    prefetch::RayHit hit{};bool gotHit=false,refusedHere=false;double from=slice.start;
                                    while(from<rayEnd-1e-6){   // one cast per segment, each end on the measured floor
                                        const double to=std::min(rayEnd,from+fieldslice::segmentLength);
                                        const auto a=pointAt(from),b=pointAt(to);++casts;
                                        if(vehicle_probe::CastNow(a.data(),b.data(),kWallLayers,hit)!=1){refusedHere=true;break;}
                                        if(hit.hit){gotHit=true;break;}
                                        from=to;
                                    }
                                    if(refusedHere){
                                        ++refused[std::clamp(vehicle_probe::LastRefusal(),0,vehicle_probe::RefusedKinds-1)];
                                        if(from>slice.start+1e-6)ray={fieldslice::Answer::Clear,from,false,in.now};  // measured clear this far before the refusal
                                        else if(canReuse&&fieldslice::Carry(cached.slice.rays[col][h],in.now,moved,slice.start,cached.slice.start,ray))++reused;  // the depth left after the car's own travel
                                        continue;}
                                    if(!gotHit){ray={fieldslice::Answer::Clear,rayEnd,fieldEnds,in.now};if(fieldEnds)++rjFieldEnd;continue;}
                                    const unsigned owner=hit.formId?hit.formId:loadedarea::HitBodyOwner(hit.bodyId);
                                    const double d=(hit.x-in.x)*sx+(hit.y-in.y)*sy;
                                    if(!std::isfinite(d)||d<slice.start-.1||d>rayEnd+.1){++rjRange;continue;}
                                    if(!hit.normalOk){++rjNoNormal;continue;}  // cannot tell what it is: not measured
                                    if(owner==self){++rjSelf;continue;}  // the car's own hull is segmented out of its volume
                                    // F1: a surface is only "clear up to it" where the line wants a surface there (a road, a landing). Where the line is in the air, terrain or a floor in its way is a solid.
                                    const bool surfaceWantedThere=!volumeExtra||wantHave[std::clamp(int(std::lround(d/kStep)),0,kSamples-1)];  // on a road surface: exactly the installed rule
                                    // Ground truth (2026-11-11 run, 06:00:44: hits 0.6-1.0 m below the measured ground of a 2.4 m ramp read as solids and stopped
                                    // the car): where the ground under a hit is measured, the hit is judged by its own height above it. At the ground: the floor
                                    // itself, clear up to it. Above it: that height is the volume's vertical profile for this ray (low edge or solid by the pass height).
                                    double groundZ=0;bool haveGround=surfaceWantedThere&&routesurvey::FloorAt(hit.x,hit.y,hit.z,planbranch::rules::levelWindow,groundZ);
                                    const bool underside=hit.nz< -.65;  // WT-2: the bottom of something above (normal facing down)
                                    // WT-1 (2026-11-12, ramp wall at 13:11:41: unmeasured ground made a lip 1 m up read as the beam's 0.30 m): no survey floor under
                                    // the hit, so measure it with one short down-ray just in front of it (toward the car). Only an up-facing answer is ground.
                                    if(surfaceWantedThere&&!haveGround&&downRays<fieldslice::downRaysPerRefresh&&maskRays<planbranch::rules::maskFrameRays){
                                        const double bx=hit.x-sx*.35,by=hit.y-sy*.35,topZ=underside?hit.z-.05:hit.z+.2;
                                        const float da[3]={float(bx),float(by),float(topZ)},db[3]={float(bx),float(by),float(topZ-3.2)};
                                        prefetch::RayHit gh{};++downRays;++maskRays;++casts;
                                        if(vehicle_probe::CastNow(da,db,kWallLayers,gh)==1&&gh.hit&&gh.normalOk&&gh.nz>.7&&gh.z<=hit.z+.05){groundZ=gh.z;haveGround=true;++groundFromDown;}}
                                    if(haveGround&&hit.z-groundZ<=fieldslice::groundTolerance){ray={fieldslice::Answer::Clear,d,false,in.now};++clearGround;continue;}
                                    if(surfaceWantedThere&&fieldslice::TerrainLayer(unsigned(hit.layer))){ray={fieldslice::Answer::Clear,d,false,in.now};++clearTerrain;continue;}  // terrain shapes the floor and slows, never a solid (world truth)
                                    if(surfaceWantedThere&&hit.nz>.65){ray={fieldslice::Answer::Clear,d,false,in.now};++clearFloor;continue;}
                                    if(underside){++hitUnderside;  // MB-1 trace: was read as floor before the sign was used
                                        // WT-2: an overhang. Hanging higher than the car's roof (plus overheadClear), the car passes under: clear up to it.
                                        if(haveGround&&hit.z-groundZ>=in.shape.height+fieldslice::overheadClear){ray={fieldslice::Answer::Clear,d,false,in.now};++clearOverhead;continue;}}
                                    if(std::any_of(shapes.begin(),shapes.end(),[&](const Shape& roadUser){
                                            return owner&&roadUser.owner==owner&&(roadUser.source==planworld::Source::Vehicle||
                                                roadUser.source==planworld::Source::Person);
                                        })){ray={fieldslice::Answer::Clear,d,false,in.now};++clearRoadUser;continue;}  // a road user: World's vehicle and people shapes already hold it
                                    Shape evidence;evidence.source=planworld::Source::Ray;evidence.centre={hit.x,hit.y,hit.z};
                                    if(planrun::SolidAllowed(evidence,f)){ray={fieldslice::Answer::Clear,d,false,in.now};++clearAllowed;continue;}  // the field allows solids here
                                    ray={fieldslice::Answer::Hit,d,false,in.now};ray.z=hit.z;  // the hit's own height: the volume's shapes take their vertical extent from it
                                    if(haveGround)ray.aboveGround=hit.z-groundZ;  // measured height above the ground: low edge or solid by the pass height
                                    ray.owner=owner;ray.hx=float(hit.x);ray.hy=float(hit.y);  // OA-1: which object, and where: the same object hit again agrees
                                    slice.Remember(owner,ray.hx,ray.hy,ray.z,std::isfinite(ray.aboveGround)?ray.aboveGround:slice.heightAbove[h],in.now);
                                    if(++hitsSeen<=3){double gz=0;const bool haveGround=routesurvey::FloorAt(hit.x,hit.y,hit.z,planbranch::rules::levelWindow,gz);
                                        logger::Observe("plan.maskhit",self,std::uint64_t(owner)^(std::uint64_t(hit.layer)<<32),
                                        "event=solid_ray pilot=%d col=%d level=%d depth=%.1f at=%.1f,%.1f,%.1f up_z=%.2f layer=%u body=%08X owner=%08X ray_height_above_floor=%.2f ground_z=%s%.1f hit_above_ground=%s%.2f reason=which_surface_made_this_hit",
                                        idx,col,h,d,hit.x,hit.y,hit.z,hit.nz,unsigned(hit.layer),unsigned(hit.bodyId),owner,slice.heightAbove[h],haveGround?"":"unmeasured:",haveGround?gz:slice.base+slice.grade*d,haveGround?"":"unmeasured:",haveGround?hit.z-gz:hit.z-(slice.base+slice.grade*d));}
                                }
                                const int memoryCleared=slice.ClearPassed(in.now);  // OA-1: beams measured passing through remembered points: those are gone
                                // WT-3 (2026-11-12, user: look again where it flickers): a belief seen only once and not met again this refresh gets one aimed
                                // ray from the car's front at the point's height, through it and lookBeyond past it. The same object near the point agrees (it
                                // becomes confirmed); a clean pass means the point is gone. Something nearer in between settles nothing.
                                int lookAgain=0,lookConfirmed=0,lookCleared=0;
                                if(slice.memory)for(auto& p:slice.memory->points){
                                    if(lookAgain>=fieldslice::lookAgainPerRefresh||maskRays>=planbranch::rules::maskFrameRays)break;
                                    if(!slice.Live(p,in.now)||p.hits>=2||std::abs(p.at-in.now)<1e-9)continue;
                                    const double dx=p.x-in.x,dy=p.y-in.y,dist=std::hypot(dx,dy);if(dist<slice.start||dist>slice.end+2.0)continue;
                                    const double ux=dx/dist,uy=dy/dist,from=in.shape.halfLength+.2;
                                    const float la[3]={float(in.x+ux*from),float(in.y+uy*from),float(p.z)},lb[3]={float(p.x+ux*fieldslice::lookBeyond),float(p.y+uy*fieldslice::lookBeyond),float(p.z)};
                                    prefetch::RayHit lh{};++lookAgain;++maskRays;++casts;
                                    if(vehicle_probe::CastNow(la,lb,kWallLayers,lh)!=1)continue;  // not answered: measured nothing
                                    if(lh.hit){const unsigned o=lh.formId?lh.formId:loadedarea::HitBodyOwner(lh.bodyId);
                                        if(o&&o==p.owner&&std::hypot(lh.x-p.x,lh.y-p.y)<=fieldslice::sameObjectReach*.5){slice.Remember(p.owner,float(lh.x),float(lh.y),lh.z,p.above,in.now);++lookConfirmed;}}
                                    else{p.owner=0;p.at=-1;++lookCleared;}}
                                const int memoryLive=slice.MemoryLive(in.now);
                                logger::Observe("plan.memory",self,unsigned(memoryLive)|(unsigned(memoryCleared)<<8)|(std::uint64_t(lookAgain)<<16)|(std::uint64_t(lookConfirmed)<<24)|(std::uint64_t(lookCleared)<<32),"pilot=%d points=%d cleared_by_pass=%d hits_this_refresh=%d look_again=%d look_confirmed=%d look_cleared=%d reason=same_object_hit_again_agrees_until_a_beam_passes_through",
                                    idx,memoryLive,memoryCleared,hitsSeen,lookAgain,lookConfirmed,lookCleared);
                                int nClear=0,nHit=0,nUnknown=0;
                                for(const auto& column:slice.rays)for(const auto& r:column){if(r.answer==fieldslice::Answer::Clear)++nClear;else if(r.answer==fieldslice::Answer::Hit)++nHit;else ++nUnknown;}
                                int refusedTotal=0;for(int k=1;k<vehicle_probe::RefusedKinds;++k)refusedTotal+=refused[k];
                                cached={slice,self,in.now,carBase};
                                pcProbeUs+=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-probeStart).count();
                                logger::Observe("plan.slice",self,unsigned(nClear)|(unsigned(nHit)<<8)|(std::uint64_t(refusedTotal)<<16)|(std::uint64_t(rjNoNormal+rjRange+rjSelf)<<24),"pilot=%d depth=%.1f clear=%d hit=%d not_measured=%d rays=%d vehicle_half=%.2f road=%.1f..%.1f spacing=%.2f speed=%.1f started_outside_field=%d zero_length=%d field_end=%d clear_to_floor=%d clear_to_terrain=%d clear_to_road_user=%d clear_to_allowed_solid=%d reused=%d refused_backoff=%d refused_rays_off=%d refused_no_world=%d refused_settling=%d refused_map_procgen=%d refused_caster_busy=%d refused_fault=%d hit_no_normal=%d hit_own_hull=%d hit_out_of_range=%d casts=%d floor_measured=%d floor_linear=%d phase=%d full_refresh=%d carried=%d field_width=%.1f field_width_from=%s clear_to_ground=%d hit_underside=%d down_rays=%d ground_from_down=%d clear_overhead=%d reason=volume_beams_across_the_field_interleaved_in_time",
                                    idx,slice.end,nClear,nHit,nUnknown,fieldslice::rayCount,slice.vehicleHalf,slice.roadLeft,slice.roadRight,slice.spacing,spd,rjOutside,rjZero,rjFieldEnd,clearFloor,clearTerrain,clearRoadUser,clearAllowed,reused,
                                    refused[vehicle_probe::RefusedBackoff],refused[vehicle_probe::RefusedRaysOff],refused[vehicle_probe::RefusedNoWorld],refused[vehicle_probe::RefusedSettling],refused[vehicle_probe::RefusedHeld],refused[vehicle_probe::RefusedCasterBusy],refused[vehicle_probe::RefusedFault],rjNoNormal,rjSelf,rjRange,casts,floorMeasured,floorLinear,slice.phase,int(fullRefresh),carried,slice.roadRight-slice.roadLeft,fieldWidthFound?"field_boxes":"open_lanes",clearGround,hitUnderside,downRays,groundFromDown,clearOverhead);
                            }
                            else if(!fresh){   // the mask was not cast this refresh: say why (every not-measured ray must have a reason)
                                const char* why=!main?"not_main_thread":!(slice.end>slice.start+1)?"mask_too_short":!gateOpen?"gate_closed_nothing_to_carry":!(slice.spacing>0)?"no_field_width":"frame_ray_budget_used";
                                logger::Observe("plan.slice",self,0x4E43ull,"event=not_cast pilot=%d depth=%.1f spacing=%.2f used_rays=%d need=%d budget=%d gate=%s reason=%s",
                                    idx,slice.end,slice.spacing,maskRays,castNeed,planbranch::rules::maskFrameRays,vehicle_probe::RefusalName(gateWhy),why);
                            }
                        }
                        if(useFieldSlice||volumeExtra){   // One flow (V2/V3, fieldvolume.h): the volume reaches the planner as World shapes, through the one collision test. Nothing else reads it to decide.
                            f.volumeWorld=useFieldSlice;f.volumeEnd=f.slice.end;  // volumeWorld: the volume is the fixed-obstacle source (on a road surface); F1's extra volume only adds shapes
                            f.volumeRange=useFieldSlice?fieldslice::Depth(spd,f.brake,in.shape.halfLength):0.0;  // Speed's measured-range limit reads it
                            const auto floorAt=[&](double depth,double){const auto i=std::clamp(int(depth/kStep),0,kSamples-2);const double u=std::clamp(depth/kStep-i,0.0,1.0);return (1-u)*wantZ[i]+u*wantZ[i+1];};
                            const auto insideField=[&](double x,double y,double z){if(nbox==0)return true;for(int k=0;k<nbox;++k)if(fieldbox::Contains(fbox[k],{x,y,z},.05))return true;return false;};
                            Shape vol[48];fieldvolume::Counts vc;
                            const int nv=fieldvolume::Shapes(f.slice,in.shape.height,floorAt,insideField,vol,48,&vc,useFieldSlice,in.now);
                            for(int k=0;k<nv;++k){if(vol[k].seenAt<=0)vol[k].seenAt=in.now;shapes.push_back(vol[k]);}
                            logger::Observe("world.volume",self,unsigned(vc.solid)|(unsigned(vc.doubt)<<16)|(std::uint64_t(vc.unmeasured)<<24)|(std::uint64_t(vc.remembered)<<32)|(std::uint64_t(vc.byObject)<<40),"pilot=%d shapes=%d solid=%d doubt=%d not_measured=%d dropped=%d confirmed_by_same_object=%d remembered=%d depth=%.1f speed=%.1f mode=%s reason=volume_cells_given_to_world_as_shapes_for_the_one_collision_test",
                                idx,nv,vc.solid,vc.doubt,vc.unmeasured,vc.dropped,vc.byObject,vc.remembered,f.slice.end,spd,useFieldSlice?"road_surface_sole_fixed_source":volumeAir?"air_extra_source":"no_field_extra_source");
                        }
                        {   // Hazard boxes (routehazard.h): the route files' marks as World volumes, through the same shapes the walls use. A box on another deck (its height range clear of this road) is not here.
                            double zLo=carBase,zHi=carBase;for(int i=0;i<kSamples;++i)if(wantHave[i]){zLo=std::min(zLo,wantZ[i]);zHi=std::max(zHi,wantZ[i]);}
                            hazardfield::AppendNow(in.x,in.y,zLo-.5,zHi+in.shape.height,110.0,in.now,shapes,kMaxShapes+16);}
                        if(hazardfield::IsTest(self)){   // Test route: fixed things that intrude into this car's path become suggested boxes (hazardfield.h Suggestions; the editor offers them, nothing is saved until accepted)
                            std::vector<routehazard::P3> lp;for(double d=0;d<=std::min(55.0,(kSamples-1)*double(kStep));d+=2.0){const auto q=lineAt(d);lp.push_back({q.x,q.y,q.z});}
                            const double laneYaw=lineAt(0).yaw*180.0/routehazard::kPi;
                            for(const auto& s:shapes){
                                if(s.hazardMark||s.source==planworld::Source::Vehicle||s.source==planworld::Source::Person||s.source==planworld::Source::Contact||s.source==planworld::Source::Ground||s.source==planworld::Source::Survey)continue;
                                if(planworld::Top(s)<=carBase+.5||(s.haveNormal&&s.normalZ>.7)||s.unmeasured||!s.mayStop)continue;
                                const bool coarseCell=s.source==planworld::Source::Box&&s.halfWidth==1.0&&s.halfLength==1.0;  // the 4 m map's band cells are not objects
                                if(coarseCell||(s.source==planworld::Source::Ray&&s.support<2))continue;
                                routehazard::Box b;b.x=s.centre.x;b.y=s.centre.y;b.z=s.centre.z;
                                if(s.source==planworld::Source::Box||s.source==planworld::Source::Volume||s.source==planworld::Source::Radar){b.hx=std::max(.3,s.halfWidth);b.hy=std::max(.3,s.halfLength);b.yaw=s.heading*180.0/routehazard::kPi;}
                                else{b.hx=b.hy=.35;b.yaw=laneYaw;}
                                b.hz=std::max(.3,s.halfHeight);
                                if(routehazard::Intrudes(b,lp,in.shape.halfWidth,in.shape.height))hazardfield::NoteSighting(self,b,in.now);}}
                        f.rearOpen=true;for(double side:{-1.0,0.0,1.0}){const double d=in.shape.halfLength+1.5,bx=in.x-std::sin(in.heading)*d+std::cos(in.heading)*side*in.shape.halfWidth,by=in.y-std::cos(in.heading)*d-std::sin(in.heading)*side*in.shape.halfWidth;
                            const auto cell=navmap::At(bx,by,in.z);if(cell.objBlocked||(cell.blocked&&cell.blockedZ>=carBase+in.limits.maxStep))f.rearOpen=false;}
                        // Navmesh: asked only when no progress has opened it, at most once a second.
                        f.sweepKnown=sweepKnown;f.sweepShapes=sweepShapes;f.sweepBand=sweptOverBand;f.raysLive=vehicle_probe::RaysLive();
                        if(in.now-g_wholeBoxLogAt[idx]>=5||in.now<g_wholeBoxLogAt[idx]){g_wholeBoxLogAt[idx]=in.now;logger::Write("OBS system=world.boxes vehicle=%08X pilot=%d event=coarse_shapes whole_boxes=%d band_cells=%d too_big_for_whole=%d swept_over_band=%d reason=whole_game_box_replaces_band_cell_when_tall_and_object_sized",self,idx,wholeBoxes,bandCells,wholeTooBig,sweptOverBand);}
                        g_planPilot[self]=idx;
                        f.test=spotAsk;if(idx>=0&&idx<int(sizeof g_testActive/sizeof g_testActive[0]))g_testActive[idx]=f.test.active;  // step 7
                        g_planStepAt[idx]=in.now;
                        {   // PF-1 / CH-1 (removal step 1): the pair's self-reports and the raw health sample, from the energy plan's newest result for this car.
                            const auto er=planenergy::Latest(idx);const bool mine=er.at&&er.vehicle==self&&!er.goal;
                            const double asked=mine?std::fabs(er.speed):0.0;const bool notMoving=mine&&asked>.5&&spd<.1;
                            const double upZ=std::cos(double(g_shs[idx].bodyPitch))*std::cos(double(g_shs[idx].bodyRoll));
                            const bool srOff=selfreport::OffLane(f.lateral,lane->halfWidth),srTip=selfreport::Tipped(g_shs[idx].probeTipped,upZ);
                            std::lock_guard sl(g_spotLock);auto& r=g_selfReport[self];const unsigned was=selfreport::Facts(r,GetTickCount64());
                            selfreport::Update(r,GetTickCount64(),notMoving,srOff,srTip);const unsigned now_=selfreport::Facts(r,GetTickCount64());
                            if(now_!=was)logger::Write("OBS system=pair.facts vehicle=%08X pilot=%d event=self_report facts=%u was=%u fact=%s lateral=%.2f lane_half=%.2f up_z=%.2f not_moving=%d reason=energy_plan_result",
                                self,idx,now_,was,selfreport::Name(now_),f.lateral,double(lane->halfWidth),upZ,int(notMoving));
                            auto& hs=g_healthSample[self];hs=FvoPairHealthSampleV1{};hs.at=GetTickCount64();hs.asked=float(asked);hs.actual=float(spd);
                            hs.lateral=float(f.lateral);hs.halfWidth=float(lane->halfWidth);hs.fieldShare=float(fieldShareNow);hs.upZ=float(upZ);
                            hs.steps=StepsRecent(idx,GetTickCount64())?1:0;hs.onRail=mine&&!er.hover?1:0;
                            hs.floorKnown=wantHave[0]&&wantMeasured[0]?1:0;hs.floorGap=hs.floorKnown?float(carBase-wantZ[0]):0.f;}
                        floorProofLane = idx == 0 && in.onWheels && in.shape.steerLock > 0 && in.shape.wheelbase > 0;
                        if (floorProofLane) ProbeBlockerFloor(self, in.x, in.y, carBase);
                        RadarAppend(idx, shapes, in.now, std::max(planbranch::rules::wantWindow, in.shape.height));  // lane branch has updated the reachable floor
                        const bool floorPointCleared = floorProofLane && ApplyFloorProofs(self, shapes);
                        AssignRoad(idx, self, shapes, in.x, in.y, std::max(planbranch::rules::wantWindow, in.shape.height), in.now);  // RF-1
                        {   // R4 trace (12:28 run): the plan standing 2 s on an open route: the first solid along the lane ahead that the car's body would meet
                            // (World's shapes against the wanted floor there, the plan's own inputs), every 2 s while it stands.
                            static double standSince[1+prefetch::kSlotPilotCount]{},standLog[1+prefetch::kSlotPilotCount]{};
                            const auto er=planenergy::Latest(idx);const bool standing=er.at&&er.vehicle==self&&!er.goal&&std::fabs(er.speed)<.1&&spd<.3&&!lane->held;
                            // MP-3 (19:44 run: 10 s stuck hovering in the ramp channel, never standing): also while the plan at the wanted speed has been
                            // rejected by a solid for 2 s, moving or not. Replaces the standing-only trigger.
                            const bool fullBlocked=er.at&&er.vehicle==self&&!er.goal&&!lane->held&&!std::isfinite(er.fullEnergy)&&er.fullTerms.reject&&std::strcmp(er.fullTerms.reject,"enters_a_solid")==0;
                            const bool blocked=standing||fullBlocked;const char* blockedWhy=standing?"standing":"full_speed_plan_enters_a_solid";
                            if(!blocked)standSince[idx]=0;else if(!standSince[idx])standSince[idx]=in.now;
                            if(blocked&&in.now-standSince[idx]>=2.0&&in.now-standLog[idx]>=2.0){standLog[idx]=in.now;char termsBuf[320];
                                const planworld::Shape* first=nullptr;double firstAt=1e9,firstOver=0;
                                for(int i=0;i<kSamples&&!first;++i){const auto p=lineAt(i*kStep);const double floor=wantHave[i]?wantZ[i]:carBase;
                                    for(const auto& sh:shapes){if(sh.source==planworld::Source::Vehicle||sh.source==planworld::Source::Person||!sh.mayStop)continue;  /* RT-1: eases only, never a block */ 
                                        const double dx=sh.centre.x-p.x,dy=sh.centre.y-p.y,along=dx*std::sin(p.yaw)+dy*std::cos(p.yaw),across=dx*std::cos(p.yaw)-dy*std::sin(p.yaw);
                                        const double reachL=std::max(sh.halfLength,sh.halfWidth)+in.shape.halfLength,reachW=std::max(sh.halfLength,sh.halfWidth)+in.shape.halfWidth;
                                        const double top=sh.centre.z+sh.halfHeight,bottom=sh.centre.z-sh.halfHeight;
                                        if(std::fabs(along)>reachL*.5+kStep*.5||std::fabs(across)>reachW||top<=floor+laneline::kFaceStep||bottom>=floor+in.shape.height)continue;
                                        if(i*kStep<firstAt){first=&sh;firstAt=i*kStep;firstOver=top-floor;}}}
                                int unmeasured=0;for(int i=0;i<kSamples;++i)unmeasured+=wantHave[i]&&!wantMeasured[i]?1:0;
                                if(first)logger::Write("OBS system=plan.blocked vehicle=%08X pilot=%d event=%s speed=%.1f seconds=%.1f first_solid=%s owner=%08X layer=%d ahead_m=%.0f centre=%.1f,%.1f,%.2f size=%.2fx%.2fx%.2f top_over_wanted_floor=%.2f unmeasured_floor=%d of=%d candidate=\"%s\" feasible=%d/%d %s reason=first_world_solid_on_the_lane_ahead",
                                    self,idx,blockedWhy,spd,in.now-standSince[idx],planworld::Name(first->source),first->owner,first->layer,firstAt,first->centre.x,first->centre.y,first->centre.z,2*first->halfLength,2*first->halfWidth,2*first->halfHeight,firstOver,unmeasured,kSamples,er.why?er.why:"",er.feasible,er.candidates,TermsText(er.terms,termsBuf,sizeof termsBuf));
                                else logger::Write("OBS system=plan.blocked vehicle=%08X pilot=%d event=%s speed=%.1f seconds=%.1f first_solid=none unmeasured_floor=%d of=%d candidate=\"%s\" feasible=%d/%d %s reason=no_world_solid_on_the_lane_ahead_the_cost_holds_it",
                                    self,idx,blockedWhy,spd,in.now-standSince[idx],unmeasured,kSamples,er.why?er.why:"",er.feasible,er.candidates,TermsText(er.terms,termsBuf,sizeof termsBuf));
                                TraceChoices(self,idx,er,blockedWhy);  // CT-1: what the plan weighed while blocked
                                // SD-1 (2026-11-18, user: "Yes, add it"): the whole scene the planner was given, once per blocked report: the car, the lane
                                // line, the 24 wanted-floor samples and every World shape within 40 m with what the planner makes of it (role). Trace only;
                                // tools/scene_view.py draws a dump. Roles follow planinput::AddShapes: skip (unmeasured, expired, the field allows it),
                                // mover, soft (prediction or world truth says never stops), hard (blocks the body).
                                {static unsigned dumpSeq=0;const unsigned dump=++dumpSeq;int nearShapes=0;
                                    for(const auto& sh:shapes)if(std::hypot(sh.centre.x-in.x,sh.centre.y-in.y)<=40.0)++nearShapes;
                                    logger::Write("OBS system=plan.scene vehicle=%08X pilot=%d dump=%u event=begin why=%s seconds=%.1f car=%.2f,%.2f,%.2f heading=%.4f half_width=%.2f half_length=%.2f height=%.2f speed=%.2f shapes=%d of=%zu rays_live=%d",
                                        self,idx,dump,blockedWhy,in.now-standSince[idx],in.x,in.y,carBase,in.heading,in.shape.halfWidth,in.shape.halfLength,in.shape.height,spd,nearShapes,shapes.size(),int(vehicle_probe::RaysLive()));
                                    for(int i=-4;i<=24;i+=2){const auto p=lineAt(i*2.0);
                                        logger::Write("OBS system=plan.scene dump=%u event=line d=%.0f at=%.2f,%.2f,%.2f yaw=%.4f half=%.2f",dump,i*2.0,p.x,p.y,p.z,p.yaw,double(lane->halfWidth));}
                                    for(int i=0;i<kSamples;++i){const auto p=lineAt(i*kStep);
                                        logger::Write("OBS system=plan.scene dump=%u event=floor i=%d at=%.2f,%.2f z=%.2f have=%d measured=%d",dump,i,p.x,p.y,wantZ[i],int(wantHave[i]),int(wantMeasured[i]));}
                                    for(const auto& sh:shapes){if(std::hypot(sh.centre.x-in.x,sh.centre.y-in.y)>40.0)continue;
                                        const bool live=sh.source==planworld::Source::Vehicle||sh.source==planworld::Source::Person;
                                        const char* role=sh.unmeasured?"skip_unmeasured":sh.expires<in.now?"skip_expired":planrun::SolidAllowed(sh,f)?"skip_field_allows":
                                            (live&&planworld::Moving(sh))?"mover":(!live&&(planworld::Predicted(sh)||!sh.mayStop))?"soft":"hard";
                                        logger::Write("OBS system=plan.scene dump=%u event=shape src=%s role=%s owner=%08X layer=%d centre=%.2f,%.2f,%.2f half=%.2f,%.2f,%.2f heading=%.4f top=%.2f base=%.2f may_stop=%d support=%d normal=%d,%.2f age=%.2f",
                                            dump,planworld::Name(sh.source),role,sh.owner,sh.layer,sh.centre.x,sh.centre.y,sh.centre.z,sh.halfLength,sh.halfWidth,sh.halfHeight,sh.heading,
                                            sh.centre.z+sh.halfHeight,sh.centre.z-sh.halfHeight,int(sh.mayStop),sh.support,int(sh.haveNormal),sh.normalZ,in.now-sh.seenAt);}
                                    logger::Write("OBS system=plan.scene dump=%u event=end plan=\"%s\" feasible=%d/%d reason=whole_scene_the_planner_was_given",dump,er.why?er.why:"",er.feasible,er.candidates);}}
                        }
                        bool ed1Submitted=false;
                        // ED-1 (user: "Yes, move to it"): your car on wheels is driven by the energy planner when its newest result is fresh and nothing in
                        // planenergy::Use rules it out; otherwise the legacy plan above drives, as before. The planner runs on a worker (planenergy.h).
                        if(wellLane){  // CW-1 step 3: the centre-line well (made even while recovering: it says when a forward fit is back)
                            const auto tW0=std::chrono::steady_clock::now();
                            auto& W=g_well[idx];
                            const double wFwd=in.vx*std::sin(in.heading)+in.vy*std::cos(in.heading),wV=std::max(3.0,std::fabs(wFwd));
                            constexpr double kGrip=3.5;  // m/s^2 sideways the car holds (railturn's limit, design section 7)
                            const double kClimbTol=std::max(.5,carcap::kCar.climb);  // under the wheels' climb
                            const double kDropTol=std::max(1.5,carcap::kCar.climb);  // 2026-11-23: coarse 4 m navmap only closes a real drop below the road (higher hits are railings/solids, not floor holes)
                            centrewell::Line wl;for(int k=0;k<lane->count;++k)wl.Add(lane->points[k].x,lane->points[k].y);
                            centrewell::Problem wp;double carS=0;
                            centrewell::Frame(wp,wl,in.x,in.y,in.heading,lane->halfWidth,in.shape.halfWidth,std::clamp(int(wV*3.0+20.0),20,centrewell::kMaxStations),1.0,carS);
                            // Floor: only a measured drop closes road (unknown never does). Not in Script mode (no detection but hazard boxes).
                            int floorNarrowed=0;
                            if(!scriptMode_)floorNarrowed=centrewell::FloorBounds(wp,[&](double st,double y){if(st<1.5&&in.onWheels)return true;double x=0,yy=0;if(!wl.At(carS+st,y,x,yy))return true;
                                const int wi=std::clamp(int(std::lround(st/kStep)),0,kSamples-1);if(!wantHave[wi]||!wantMeasured[wi])return true;
                                const auto m=navmap::At(x,yy,wantZ[wi]+1.0);if(!m.haveGround)return true;return double(m.ground)>=wantZ[wi]-kDropTol;});
                            std::vector<centrewell::Obstacle> wobs;wobs.reserve(64);int nHaz=0,nHard=0,nSoft=0;
                            const auto inRange=[&](const centrewell::Obstacle& o){return o.s1>=-2.0&&o.s0<=wp.n*wp.step;};
                            {routehazard::Box hb[16];const int hn=hazardfield::NearNow(in.x,in.y,in.z-20.0,in.z+20.0,150.0,hb,16);  // the user's marks: always hard
                                for(int k=0;k<hn;++k){const unsigned id=0x48000000u^(unsigned(std::lround(hb[k].x*2))*73856093u)^(unsigned(std::lround(hb[k].y*2))*19349663u);
                                    auto o=centrewell::FromBox(wl,carS,id,hb[k].x,hb[k].y,hb[k].yaw*3.14159265358979323846/180.0,hb[k].hy,hb[k].hx,in.shape.halfWidth+centrewell::margin::kHazardBox,in.shape.halfLength);
                                    if(!inRange(o))continue;o.kept=W.mem.Side(o.id);wobs.push_back(o);++nHaz;}}
                            // World's shapes (not in Script mode). Moving vehicles and people, and occupied traffic cars (wait behind, D1), are the speed
                            // layer's; a standing vehicle or a person is hard; game boxes, radar bodies, contacts, sweep solids and agreeing sensor hits are hard;
                            // a lone sensor hit or an unexplained road hit is a soft push (step 2: 11 of 22 recorded blocks were lone volume and ray points).
                            scriptdrive::Blocker wbl[48];int wbn=0;
                            for(const auto& sh:shapes){
                                const bool live=sh.source==planworld::Source::Vehicle||sh.source==planworld::Source::Person;
                                if(live&&(scriptMode_||planworld::Moving(sh)||(sh.source==planworld::Source::Vehicle&&!sh.steerRound))){
                                    if(wbn<48&&sh.expires>=in.now)wbl[wbn++]={sh.centre.x,sh.centre.y,sh.heading,sh.halfLength,sh.halfWidth,sh.source==planworld::Source::Person};continue;}
                                if(scriptMode_||sh.hazardMark||sh.unmeasured||sh.expires<in.now||sh.source==planworld::Source::Ground||planrun::SolidAllowed(sh,f))continue;
                                double m=centrewell::margin::kSensor;bool hard=true;
                                if(sh.source==planworld::Source::Person)m=centrewell::margin::kPerson;
                                else if(sh.source==planworld::Source::Vehicle)m=centrewell::margin::kVehicle;
                                else{const double top=sh.centre.z+sh.halfHeight,bot=sh.centre.z-sh.halfHeight;
                                    if(top<=carBase+kClimbTol||bot>=carBase+in.shape.height)continue;  // under the wheels' climb or over the roof
                                    if(sh.source==planworld::Source::Box)m=centrewell::margin::kGameBox;
                                    const bool trusted=sh.mayStop&&(sh.source==planworld::Source::Box||sh.source==planworld::Source::Radar||sh.source==planworld::Source::Contact||
                                        sh.source==planworld::Source::Survey||sh.support>=2);
                                    hard=trusted;}
                                const unsigned id=sh.owner?(sh.owner^(unsigned(sh.layer+1)<<27)):(0x52000000u^(unsigned(std::lround(sh.centre.x*2))*73856093u)^(unsigned(std::lround(sh.centre.y*2))*19349663u));
                                auto o=centrewell::FromBox(wl,carS,id,sh.centre.x,sh.centre.y,sh.heading,std::max(.05,sh.halfLength),std::max(.05,sh.halfWidth),in.shape.halfWidth+m,in.shape.halfLength);
                                if(!inRange(o))continue;
                                if(hard){o.kept=W.mem.Side(o.id);wobs.push_back(o);++nHard;}
                                else{const double mid=.5*(o.left+o.right);const int side=mid>0?-1:+1;
                                    const int i0=std::max(0,centrewell::StationOf(o.s0,1.0)),i1=std::min(wp.n-1,centrewell::StationOf(o.s1,1.0)+1);
                                    for(int k=i0;k<=i1;++k)wp.Soft(k,side,side>0?o.right:o.left,.3*std::min(1.0,double(sh.support)/2.0));++nSoft;}
                            }
                            const auto wsides=centrewell::Choose(wp,wobs);centrewell::ApplySides(wp,wobs,wsides);W.mem.Update(wobs,wsides);
                            const double wFitV=std::clamp(std::max(wV,f.want),3.0,25.0);  // 2026-11-23: fit smoothness to target speed so standing cars do not build a 2 m kink capped at 4.2 m/s
                            const auto wg=centrewell::SolveWithinGrip(wp,wFitV,kGrip);const auto& wr=wg.r;
                            // The path for the follower (AF-1 takes at most kMaxPoints, 1 m apart from where the car is).
                            // Reject singular SolveWithinGrip rollouts (e.g. grip_peak=173 / length=212km) -> clean lane centreline.
                            const bool singularWell=!std::isfinite(wg.peak)||wg.peak>30.0||!std::isfinite(wg.w.length)||wg.w.length>5000.0;
                            pathfollow::Path wpath;
                            for(int i=0;i<wr.n&&wpath.count<pathfollow::kMaxPoints;++i){double x=0,yy=0;if(!wl.At(carS+i,singularWell?0.0:wr.y[i],x,yy))break;wpath.x[wpath.count]=x;wpath.y[wpath.count]=yy;++wpath.count;}
                            // Speed along that path: the lane's (crest, slide and tracking caps already in f.want), hold, stop line, bend, grip, the block point,
                            // and the first vehicle or person on the path ahead (scriptdrive.h's stop rule on the well's own path).
                            struct WP{double x,y;};
                            const auto pathAt=[&](double st){const double c=std::clamp(st,0.0,double(std::max(0,wr.n-1)));const int i=std::min(int(c),std::max(0,wr.n-2));const double t=c-i;
                                const double y=wr.n>=2?wr.y[i]+(wr.y[i+1]-wr.y[i])*t:0.0;double x=0,yy=0;wl.At(carS+st,y,x,yy);return WP{x,yy};};
                            scriptdrive::Car wc;wc.along=0;wc.speed=wFwd;wc.halfWidth=in.shape.halfWidth;wc.halfLength=in.shape.halfLength;wc.brake=f.brake;wc.want=f.want;
                            wc.held=lane->held!=0;wc.stopDistance=lane->stopDistance;wc.lookEnd=std::max(0,wr.n-1);
                            wc.bendCap=idx<int(sizeof g_railCorner/sizeof g_railCorner[0])&&g_railCorner[idx]>0&&g_railCorner[idx]<1e8?g_railCorner[idx]:0.0;
                            const double blockCap=wr.blockAt>=0?std::sqrt(2.0*std::max(.5,f.brake)*std::max(0.0,wr.blockAt*wp.step-in.shape.halfLength-2.0)):1e9;
                            const double cap=std::min(wg.speedCap,blockCap);
                            const auto wso=scriptdrive::Speed(wc,pathAt,0.0,cap,wbl,wbn);
                            const char* wwhy=std::strcmp(wso.why,"script_hazard_box")==0?(wr.blockAt>=0&&blockCap<=wg.speedCap?"well_block_point":"well_grip_cap"):wso.why;
                            W.path=wpath;W.centre=wr.maxOffset<.05;W.speed=wso.speed;W.why=wwhy;W.at=in.now;W.maxOffset=wr.maxOffset;W.blockAt=wr.blockAt;
                            // D3 (default: keep the backing-up families): no forward fit and standing, or off the road's surface, hands the car to the energy plan;
                            // a forward fit on the road for 1 s hands it back. Never in Script mode (it waits).
                            const bool standing=std::fabs(wFwd)<.3;
                            const bool offRoad=wantHave[0]&&wantMeasured[0]&&std::fabs(carBase-wantZ[0])>1.0;
                            if(wr.blockAt>=0&&standing){if(W.blockedStandSince<0)W.blockedStandSince=in.now;}else W.blockedStandSince=-1;
                            if(offRoad){if(W.offRoadSince<0)W.offRoadSince=in.now;}else W.offRoadSince=-1;
                            const bool fit=wr.blockAt<0&&!offRoad;if(fit){if(W.clearSince<0)W.clearSince=in.now;}else W.clearSince=-1;
                            if(!scriptMode_&&!W.recovering&&((W.blockedStandSince>=0&&in.now-W.blockedStandSince>=planbranch::rules::notMovingAfter)||(W.offRoadSince>=0&&in.now-W.offRoadSince>=1.0))){
                                W.recovering=true;W.recoverSince=in.now;
                                logger::Write("OBS system=plan.well vehicle=%08X pilot=%d event=recover_start block_at=%d off_road=%d car_base=%.2f road=%.2f reason=%s",self,idx,wr.blockAt,int(offRoad),carBase,wantZ[0],
                                    offRoad?"car_off_the_road_surface_energy_plan_takes_it":"no_forward_fit_standing_energy_plan_backs_up");}
                            else if(W.recovering&&(scriptMode_||(W.clearSince>=0&&in.now-W.clearSince>=1.0)||in.now-W.recoverSince>=planbranch::rules::passGiveUp)){
                                logger::Write("OBS system=plan.well vehicle=%08X pilot=%d event=recover_end seconds=%.1f reason=%s",self,idx,in.now-W.recoverSince,
                                    scriptMode_?"script_mode_never_recovers":W.clearSince>=0?"forward_fit_on_the_road_for_1s":"recovery_gave_up_after_pass_give_up");
                                W.recovering=false;}
                            const double wUs=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-tW0).count();
                            int nL=0,nR=0,nT=0;for(const auto& c:wsides){if(c.touching)++nT;if(c.need>.05)(c.side>0?nR:nL)++;}
                            if(in.now-W.logAt>=2.0||in.now<W.logAt||W.loggedWhy!=wwhy||(W.loggedBlock>=0)!=(wr.blockAt>=0)||W.loggedRecover!=W.recovering){
                                W.logAt=in.now;W.loggedWhy=wwhy;W.loggedBlock=wr.blockAt;W.loggedRecover=W.recovering;
                                logger::Write("OBS system=plan.well vehicle=%08X pilot=%d event=plan mode=%s why=%s speed=%.1f want=%.1f actual=%.1f block_at=%d max_offset=%.2f y0=%.2f stations=%d pass_left=%d pass_right=%d touching=%d hazards=%d hard=%d soft=%d movers=%d floor_narrowed=%d grip_peak=%.2f speed_cap=%.1f length=%.1f solves=%d rounds=%d stop_in=%.1f recovering=%d us=%.0f pos=%.1f,%.1f reason=centre_line_well",
                                    self,idx,scriptMode_?"script":"normal",wwhy,wso.speed,f.want,wFwd,wr.blockAt,wr.maxOffset,wp.y0,wr.n,nL,nR,nT,nHaz,nHard,nSoft,wbn,floorNarrowed,wg.peak,
                                    wg.speedCap>1e8?-1.0:wg.speedCap,wg.w.length,wg.solves,wr.rounds,wso.stopIn,int(W.recovering),wUs,in.x,in.y);}
                            if(wellDrive)ed1Submitted=true;  // the well drives this car: nothing goes to the energy plan
                        }
                        if(!wellDrive&&idx==0&&in.shape.steerLock>0&&in.shape.wheelbase>0){  // removal step 1: your car's energy plan, every gather (the old ED-1 check and writer are gone)
                            auto espec=planinput::Spec(f);espec.car={in.shape.steerLock,in.shape.wheelbase,in.shape.slopePoint,in.shape.initialSlope,in.shape.fullSteerSpeed};
                            FlightSpec(espec,idx,self);NoteFlightPlan(idx,f.want,nullptr);  // FC-1: the gear row; FG-1 reads the lane's wanted speed
                            const double eforward=in.vx*std::sin(in.heading)+in.vy*std::cos(in.heading);
                            bicycle::State estate{f.x,f.y,in.heading,eforward,sh.carInput.load(std::memory_order_relaxed)};estate.z=f.carBase;estate.mode=in.onWheels?0:1;
                            planenergy::Submit(self,f,lineAt,wanted,shapes,estate,espec,floorPointCleared);ed1Submitted=true;
                            const auto er=planenergy::Latest();const ULONGLONG enow=GetTickCount64();
                            if(enow-g_energyLog.at>=500){g_energyLog={enow,er.ok,er.why?er.why:""};char termsBufR[320];char termsBufF[320];
                                logger::Write("OBS system=plan.energy vehicle=%08X event=result candidate=\"%s\" speed=%.2f reverse=%d hover=%d clearance=%.2f energy=%.1f gears=%d feasible=%d/%d worker_ms=%.1f field=%d age_ms=%llu path_points=%d pulse=%d pos=%.2f,%.2f heading=%.1f kept=%d rival=\"%s\" gap=%.2f offset=%.1f speed_ahead=%.2f ask=%.2f why=%s %s full_speed=%.1f full_energy=%.1f full_reject=%s full_terms={%s} reason=one_decider",
                                    self,er.why?er.why:"",er.speed,int(er.reverse),int(er.hover),er.clearance,er.energy,er.gears,er.feasible,er.candidates,er.workerMs,int(er.field),er.at?static_cast<unsigned long long>(enow-er.at):0ull,er.path.count,int(er.pulse),f.x,f.y,in.heading*57.29578,int(er.kept),er.rival?er.rival:"",er.rivalGap,er.offset,er.speedAhead,g_askSpeed[0],er.switchWhy?er.switchWhy:"",TermsText(er.terms,termsBufR,sizeof termsBufR),er.fullSpeed,er.fullEnergy,er.fullTerms.reject&&*er.fullTerms.reject?er.fullTerms.reject:"none",TermsText(er.fullTerms,termsBufF,sizeof termsBufF));TraceChoices(self,0,er,"energy_result");}  // HC-4: the plan at the wanted speed beside the best; CT-1
                        }
                        // Every other lane car (and your car when it was not submitted above) hands its snapshot to its own driver, within the budget.
                        if(!ed1Submitted&&idx>=0&&idx<planenergy::kSlots){  // step 7: driving plans too
                            auto sspec=planinput::Spec(f);if(in.shape.steerLock>0&&in.shape.wheelbase>0)sspec.car={in.shape.steerLock,in.shape.wheelbase,in.shape.slopePoint,in.shape.initialSlope,in.shape.fullSteerSpeed};
                            FlightSpec(sspec,idx,self);NoteFlightPlan(idx,f.want,nullptr);  // FC-1: the gear row; FG-1 reads the lane's wanted speed
                            const double sforward=in.vx*std::sin(in.heading)+in.vy*std::cos(in.heading);
                            bicycle::State sstate{f.x,f.y,in.heading,sforward,sh.carInput.load(std::memory_order_relaxed)};sstate.z=f.carBase;sstate.mode=in.onWheels?0:1;
                            planenergy::SubmitLane(idx,self,f,lineAt,wanted,shapes,sstate,sspec,in.now);}
                        }
                        {   // PC-1 (2026-11-08): record per-car planning cost and log 5 s summary.
                            const double carUs=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-tPlanCar0).count();
                            const unsigned long long frameNow=routesurvey::Frame();
                            std::lock_guard<std::mutex> lk(g_planCost.lock);
                            if(frameNow!=g_planCost.lastFrame){
                                if(g_planCost.lastFrame!=0)g_planCost.frameMaxUs=std::max(g_planCost.frameMaxUs,g_planCost.frameCurUs);
                                g_planCost.lastFrame=frameNow;g_planCost.frameCurUs=0;++g_planCost.frames;}
                            ++g_planCost.calls;
                            ++g_planCost.replans;  // a World gather for the energy plan
                            if(idx>=0&&idx<256)g_planCost.activeMask[idx>>6]|=(1ull<<(idx&63));
                            g_planCost.frameCurUs+=carUs;g_planCost.totalUs+=carUs;g_planCost.carMaxUs=std::max(g_planCost.carMaxUs,carUs);
                            g_planCost.surfaceUs+=pcSurfaceUs;g_planCost.worldUs+=pcWorldUs;g_planCost.probeUs+=pcProbeUs;
                            costparts::Add(costparts::g_gatherMs,carUs/1000.0);costparts::Add(costparts::g_surfaceMs,pcSurfaceUs/1000.0);++costparts::g_gathers;
                            costparts::Add(costparts::g_surfProvMs,pcProvUs/1000.0);costparts::Add(costparts::g_surfBoxMs,pcBoxUs/1000.0);costparts::Add(costparts::g_surfReachMs,pcReachUs/1000.0);
                            g_planCost.buildUs+=pcBuildUs;g_planCost.collideUs+=pcCollideUs;g_planCost.speedUs+=pcSpeedUs;g_planCost.scoreWallUs+=pcScoreWallUs;
                            g_planCost.arbiterUs+=pcArbUs;g_planCost.bodyUs+=pcBodyUs;
                            if(in.now-g_planCost.logAt>=5.0||in.now<g_planCost.logAt){
                                g_planCost.frameMaxUs=std::max(g_planCost.frameMaxUs,g_planCost.frameCurUs);
                                int activeCars=0;for(int m=0;m<4;++m){std::uint64_t v=g_planCost.activeMask[m];while(v){v&=v-1;++activeCars;}}
                                const unsigned fr=std::max(1u,g_planCost.frames),cl=std::max(1u,g_planCost.calls);
                                logger::Write("OBS system=plan.cost frames=%u gathers=%u active_cars=%d total_ms=%.2f frame_avg_ms=%.3f frame_max_ms=%.2f car_avg_us=%.1f car_max_us=%.1f surface_ms=%.2f world_ms=%.2f probe_ms=%.2f reason=world_gathering_for_the_energy_plan",
                                    g_planCost.frames,g_planCost.calls,activeCars,g_planCost.totalUs/1000.0,(g_planCost.totalUs/1000.0)/fr,g_planCost.frameMaxUs/1000.0,
                                    g_planCost.totalUs/cl,g_planCost.carMaxUs,g_planCost.surfaceUs/1000.0,g_planCost.worldUs/1000.0,g_planCost.probeUs/1000.0);
                                const unsigned long long lf=g_planCost.lastFrame;
                                g_planCost.frames=0;g_planCost.calls=0;g_planCost.replans=0;g_planCost.kept=0;
                                g_planCost.nearN=0;g_planCost.midN=0;g_planCost.farN=0;g_planCost.urgentN=0;
                                g_planCost.staggered=0;g_planCost.workerSubmitted=0;g_planCost.workerMissed=0;g_planCost.workerTrials=0;
                                g_planCost.frameCurUs=0;g_planCost.frameMaxUs=0;g_planCost.carMaxUs=0;g_planCost.totalUs=0;
                                g_planCost.surfaceUs=0;g_planCost.worldUs=0;g_planCost.probeUs=0;g_planCost.buildUs=0;
                                g_planCost.collideUs=0;g_planCost.speedUs=0;g_planCost.scoreWallUs=0;g_planCost.arbiterUs=0;g_planCost.bodyUs=0;
                                for(auto& m:g_planCost.activeMask)m=0;
                                g_planCost.lastFrame=lf;g_planCost.logAt=in.now;}
                        }
                    }
                    // Step 7 (one authority): the energy Plan's newest result for THIS car is the decision, every update (replan or not). Wheels:
                    // the AF-1 path follower; hover: the step-5 hover follower into the hover actuator's channels; mode: the Plan's. Not ready
                    // (no result, another car's, older than 500 ms, or none finite): stand, and say so. A spot test keeps its own pulse (D12).
                    if(wellDrive){  // CW-1 step 3: the well's newest path and speed (made at the World gather, 4 Hz) drive this car
                        g_energyOwnsAt[idx]=GetTickCount64();
                        const auto& W=g_well[idx];const double wspd=std::hypot(in.vx,in.vy);
                        if(W.vehicle==self&&W.at>=0&&in.now-W.at<.6&&in.now>=W.at&&W.path.count>=2){
                            NoteFollowPath(idx,W.path,false,W.centre);
                            g.adOut.mode=drive::Mode::Drive;g.adOut.reverse=false;g.adOut.phase=drive::Phase::Travel;g.adOut.boost=false;
                            FollowSpeed(idx,W.speed<.1?0.0:W.speed,in.now,in.limits.brake,wspd,g.adOut.speed,g.adOut.brake);
                            g.adOut.why=W.why;capBy="centre-line well";laneSpeed=g.adOut.speed;bearingBy="centre-line well (path follower)";
                        } else {
                            g.adOut.speed=0;g.adOut.brake=true;g.adOut.reverse=false;g.adOut.boost=false;g.adOut.why="well not ready";capBy="well not ready";laneSpeed=0;
                        }
                        if(idx<int(sizeof g_planSplit/sizeof g_planSplit[0]))g_planSplit[idx].at=0;
                    } else
                    if(idx>=0&&idx<planenergy::kSlots){  // removal step 1: always (the spot test's pulse is the energy plan's own, D12)
                        g_energyOwnsAt[idx]=GetTickCount64();
                        const auto er=planenergy::Latest(idx);const ULONGLONG eage=er.at?GetTickCount64()-er.at:~0ull;
                        const double espd=std::hypot(in.vx,in.vy);const char* ewhy="";
                        bool currentPulse = !er.factorySpotPulse;
#ifndef FVO_RELEASE
                        if (er.factorySpotPulse) currentPulse = privatePulseSerial && privatePulseSerial == er.spotPulseSerial;
#endif
                        if(er.at&&er.vehicle==self&&!er.goal&&eage<=500&&er.ok&&currentPulse){
                            if(er.hover){
                                hoverfollow::Car hc;hc.x=in.x;hc.y=in.y;hc.z=in.z-in.shape.originAbove;hc.heading=in.heading;hc.speed=espd;hc.originAbove=in.shape.originAbove;
                                const auto ho=hoverfollow::Follow(er.hoverPath,hc,eage/1000.0);
                                NoteFlightPlan(idx,-1,&er.hoverPath);  // FG-1: the committed hover plan's sharpest turn
                                energyHover=!ho.land;
                                g.adOut.mode=ho.land?drive::Mode::Drive:drive::Mode::Hover;g.adOut.speed=ho.speed;g.adOut.brake=ho.brake;g.adOut.reverse=false;
                                g.adOut.phase=drive::Phase::Travel;g.adOut.climb=0;ld.height=ho.height;
                                g.adOut.bearing=(ho.split||ho.facingUsed||ho.hold)?ho.nose:ho.bearing;
                                if(ho.land)g.adOut.boost=false;  // BO-2: Speed's boost request for a flying car (hover thrust); landing is wheels' business
                                else BoostSpeed(idx,self,g.adOut,false,lane->held!=0,lane->held?0.0:double(lane->stopDistance),std::numeric_limits<double>::infinity(),in.vx*std::sin(in.heading)+in.vy*std::cos(in.heading),0.0,in.now,true,ho.split?"strafe_plan":ho.hold?"holding_spot":ho.facingUsed?"facing_turn":nullptr);
                                if(idx<int(sizeof g_planSplit/sizeof g_planSplit[0])){if(ho.split)g_planSplit[idx]={GetTickCount64(),ho.bearing,ho.speed};else g_planSplit[idx].at=0;}
                                bearingBy=ho.split?"energy plan nose held (hover strafe)":"energy plan (hover follower)";ewhy=ho.land?"hover_landing":"hover";
                            } else {
                                NoteFollowPath(idx,er.SteeringPath(),er.reverse,er.UsesLaneCentre());
                                const double etarget=std::fabs(er.speed)<.1?0.0:er.reverse?std::max(0.0,-er.speedAhead):std::max(0.0,er.speedAhead);
                                g.adOut.mode=drive::Mode::Drive;g.adOut.reverse=er.reverse;FollowSpeed(idx,etarget,in.now,in.limits.brake,espd,g.adOut.speed,g.adOut.brake);TraceSwitch(idx,self,er,"lane");
                                BoostSpeed(idx,self,g.adOut,in.onWheels,lane->held!=0,lane->held?0.0:double(lane->stopDistance),er.terms.wheelSpeed,in.vx*std::sin(in.heading)+in.vy*std::cos(in.heading),std::fabs(g.drvSteer),in.now);  // BO-1: Speed's boost request
                                g.adOut.phase=er.reverse?drive::Phase::Reverse:drive::Phase::Travel;if(er.reverse)g.adOut.bearing=in.heading;
                                if(idx<int(sizeof g_planSplit/sizeof g_planSplit[0]))g_planSplit[idx].at=0;
                                bearingBy="energy plan (path follower)";ewhy="wheels";
                            }
                            g.adOut.why=er.why;capBy="energy plan";laneSpeed=g.adOut.speed;
                            // Exit ramp after a rail handoff: speeding up is gradual, braking is never limited.
                            if(const double rampCap=FleetHandoffCap(self,GetTickCount64());rampCap<g.adOut.speed){g.adOut.speed=rampCap;laneSpeed=rampCap;capBy="rail handoff ramp";}
                        } else {
                            g.adOut.speed=0;g.adOut.brake=true;g.adOut.reverse=false;g.adOut.boost=false;g.adOut.why="energy plan not ready";capBy="energy plan not ready";laneSpeed=0;
                            ewhy=!currentPulse?"factory_pulse_ended":!er.at?"no_result_yet":er.vehicle!=self?"result_for_another_car":er.goal?"goal_scene_result":eage>500?"result_stale":"no_finite_candidate";
                        }
                        if(in.now-g_energyAuthLogAt[idx]>=2.0||in.now<g_energyAuthLogAt[idx]){g_energyAuthLogAt[idx]=in.now;
                            logger::Write("OBS system=plan.authority vehicle=%08X pilot=%d event=decides scene=lane mode=%s speed=%.2f reverse=%d brake=%d age_ms=%llu candidate=\"%s\" state=%s pos=%.1f,%.1f reason=one_authority_energy_plan",
                                self,idx,drive::Name(g.adOut.mode),g.adOut.speed,int(g.adOut.reverse),int(g.adOut.brake),static_cast<unsigned long long>(er.at?eage:0),er.why?er.why:"",ewhy,in.x,in.y);}
                    }
                    if(!energyHover){  // the drive actuator (rail); a flying car is tracked by the hover actuator (step 7: the energy plan's mode)
                    const double along=laneline::Project(*lane,in.x,in.y,in.z).along;const double sp=std::hypot(in.vx,in.vy);
                    // The actuator: the rail aims at the plan's line (with the arbiter on) or the lane driver's pass offset (off).
                    const auto rt=railturn::Pursue(*lane,[&](const FvoLaneGuidanceV1&,double s){return laneWindow.At(s);},along,in.x,in.y,sp,planLook,lane->halfWidth,in.shape.halfWidth);
                    if(rt.ok){RailAim(idx,rt.bearing,in.now);
                        const bool onlyTop=std::strcmp(capBy,"vehicle top speed")==0||std::strcmp(capBy,"lane speed")==0;
                        g_railCorner[idx]=rt.cornerCap;
                        g_railFree[idx]=onlyTop&&rt.cornerCap>g.adOut.speed;
                        if(rt.cornerAt>=0&&rt.cornerCap<12)logger::Observe("traffic.rail",self,lane->identity^0x800ull,"event=corner pilot=%d ahead=%.0f radius=%.1f cap=%.1f look=%.1f speed=%.1f",idx,rt.cornerAt,rt.radius,rt.cornerCap,rt.look,sp);}
                    else RailAim(idx,g.adOut.bearing,in.now);
                    }  // !planFlies
                    // Car navmesh (2026-11-05, navmesh.h): the cells under the car and under its lane window ahead are wanted, so the chosen
                    // route's cells are baked (once per world, cached on disk) as it is driven.
                    if(idx>=0&&idx<int(sizeof g_navWantAt/sizeof g_navWantAt[0])&&GetTickCount64()>=g_navWantAt[idx]){g_navWantAt[idx]=GetTickCount64()+500;
                        navmesh::Want(in.x,in.y,"route_car");for(int k=0;k<lane->count;k+=4)navmesh::Want(lane->points[k].x,lane->points[k].y,"route_ahead");}
                } else if(idx>=0&&idx<int(sizeof g_railBearing/sizeof g_railBearing[0])){g_railBearing[idx]=g.adOut.bearing;g_railBearingAt[idx]=0;}
                g_laneOwnership[idx].Change(trafficcoordinator::Owner::Lane,command);  // the ownership record (trafficinspect reads it)
            } else if(const auto* policy=routeguidance::Policy(idx);policy&&trafficcoordinator::Strict(*policy)) {
                driverOwner="coordinator_hold";
                g_laneOwnership[idx].Change(trafficcoordinator::Owner::Hold,policy->identity);
                g.adOut={};g.adOut.bearing=in.heading;g.adOut.mode=in.onWheels?drive::Mode::Drive:drive::Mode::Hover;
                g.adOut.brake=true;g.adOut.phase=drive::Phase::Boxed;g.adOut.why=routeguidance::Refusal(idx);
                if (g.adOut.why && std::strcmp(g.adOut.why, "expired_lane_command") == 0)
                    archproof::State().violExpiredLane.fetch_add(1, std::memory_order_relaxed);
                logger::Observe("traffic.boundary",self,0x10000ull,"event=refused operation=lane_command caller=coordinator reason=%s pilot=%d",g.adOut.why,idx);
            } else {
                // Removal 2026-11-14: a car with no lane (a job, a summon, an escort, combat, your car's trip) is the energy plan's goal scene. The
                // controls start neutral here; EnergyStep below writes them from the plan's newest result for this car (stand when there is none).
                g_laneOwnership[idx].Change(trafficcoordinator::Owner::General,0);
                g.adOut={};g.adOut.bearing=in.heading;g.adOut.mode=in.onWheels?drive::Mode::Drive:drive::Mode::Hover;g.adOut.phase=drive::Phase::Travel;
            }
            if(idx>=0&&idx<int(sizeof g_driverOwner/sizeof g_driverOwner[0])){  // MB-4: who drives, traced on change, census every 30 s
                if(g_driverOwner[idx]!=driverOwner)logger::Write("OBS system=plan.ownermap vehicle=%08X pilot=%d event=changed owner=%s was=%s job_line=%d job_line_refused=%s on_wheels=%d reason=who_drives_this_car",
                    self,idx,driverOwner,g_driverOwner[idx]?g_driverOwner[idx]:"-",int(jobLine),g_jobLineWhy[idx]?g_jobLineWhy[idx]:"-",int(in.onWheels));
                g_driverOwner[idx]=driverOwner;g_driverOwnerAt[idx]=in.now;
                if(g_ownerSummaryAt<0||in.now-g_ownerSummaryAt>=30||in.now<g_ownerSummaryAt){g_ownerSummaryAt=in.now;
                    static const char* const kOwners[]={"plan_rail","plan_flies","energy_plan_goal","coordinator_hold","plan_virtual"};
                    int n[sizeof kOwners/sizeof kOwners[0]]={};
                    for(int k=0;k<int(sizeof g_driverOwner/sizeof g_driverOwner[0]);++k){if(!g_driverOwner[k]||in.now-g_driverOwnerAt[k]>2.0||g_driverOwnerAt[k]>in.now)continue;
                        for(int o=0;o<int(sizeof kOwners/sizeof kOwners[0]);++o)if(!std::strcmp(g_driverOwner[k],kOwners[o])){++n[o];break;}}
                    logger::Write("OBS system=plan.ownermap event=census plan_rail=%d plan_flies=%d energy_plan_goal=%d coordinator_hold=%d plan_virtual=%d reason=removal_20261114_one_decider",n[0],n[1],n[2],n[3],n[4]);}
            }
            EnergyStep(idx,self,in,driverOwner,floorProofLane);  // radar tick; a car with no lane: its goal plan and step 7
            const auto& o = g.adOut;
            trafficinspect::Control observation;observation.at=nowMs;observation.askedSpeed=o.speed;observation.actualSpeed=std::hypot(in.vx,in.vy);
            observation.laneError=authoredLane?laneline::Project(*authoredLane,in.x,in.y,in.z).distance:0.0;observation.height=g_laneDriver[idx].height;observation.mode=int(o.mode);observation.owner=int(g_laneOwnership[idx].owner);
            std::snprintf(observation.reason,sizeof observation.reason,"%s",o.why?o.why:"");trafficinspect::Publish(self,observation);
			logger::Observe("autopilot",self,unsigned(o.phase)|(std::uint64_t(o.mode)<<8)|(std::uint64_t(in.driverSeated)<<17)|(std::uint64_t(in.paused)<<18),
				"pilot=%d phase=%s mode=%s driver_seated=%d paused=%d pos=%.2f,%.2f,%.2f goal=%.2f,%.2f,%.2f speed_asked=%.2f speed_actual=%.2f distance=%.2f owner=%s reason=%s",
				idx,drive::Name(o.phase),drive::Name(o.mode),in.driverSeated,in.paused,pos.x,pos.y,pos.z,goal.x,goal.y,std::isfinite(goal.z)?goal.z:pos.z,o.speed,std::hypot(in.vx,in.vy),dist,driverOwner,o.why?o.why:"");
			logger::Observe("avoidance",self,std::uint64_t(groundOk)<<32,
				"pilot=%d ground_known=%d ground_z=%.2f ceiling_room=%.2f contacts=%d neighbours=%d bearing=%.3f health_share=%.3f",
				idx,groundOk,groundZ,ceilingRoom,in.contact,in.otherCount,o.bearing,vehiclehits::HealthShare(self));
			if (o.phase != g.adLogPhase || (logger::Verbose() && nowMs - g.adLogAt >= 2000)) {
				g.adLogAt = nowMs;
				g.adLogPhase = o.phase;
				static const char* permits[3] = { "both", "drive only", "hover only" };
				logger::Write("%s: energy plan %s (%s; %s), %s: want %.1f m/s (at %.1f), bearing %+.0f deg (heading %+.0f), %s; %.0f m to the goal, %.1f m above ground%s",
					who, drive::Name(o.mode), permits[int(in.permit)], in.preferWhy, drive::Name(o.phase), o.speed, std::hypot(in.vx, in.vy), o.bearing * 57.2958, heading * 57.2958,
					o.why && *o.why ? o.why : "-", dist, pos.z - groundZ, groundOk ? "" : " (no ground reading)");
			}
		}
		// One frame of a hovering pilot: the assist, avoidance, height hold and (autopilot) the flight to the goal.
		inline bool in_railLane(int idx) { return idx >= 0 && idx < int(sizeof g_railLaneAt / sizeof g_railLaneAt[0]) && GetTickCount64() - g_railLaneAt[idx] < 300; }
		void Fly(int idx, std::uintptr_t vehicle, Goal goal, bool focused)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const char* wf = idx ? Who(idx) : "Rover flight";
			const char* wc = idx ? Who(idx) : "Rover call";
			// Scripted flight (flightscript.h, 2026-10-24): the caller's command is the plan this frame.
			const bool scripted = (g_cmds[idx].c.set & Command::kScripted) && GetTickCount64() < g_cmds[idx].until;
			// Passenger control (kManual, 2026-11-17, ported 2026-11-18): the player's own hands on this NPC-driven vehicle; see vehicleflight.h Command.
			const bool manual = (g_cmds[idx].c.set & Command::kManual) && GetTickCount64() < g_cmds[idx].until;
#ifndef FVO_RELEASE
			// Lab stock float (stockfloat.h): the hover holds one commanded absolute height, with no ground reference, while a lab lease renews it.
			const bool stockFloat = idx >= 1 && idx < int(sizeof g_stockFloat / sizeof g_stockFloat[0]) && stockfloat::Active(g_stockFloat[idx], GetTickCount64());
#else
			constexpr bool stockFloat = false;
#endif
            const auto* trafficPolicy=routeguidance::Policy(idx);
            const bool respondStatic=!g.autopilot||!trafficPolicy||trafficPolicy->obstacles!=2;
            const bool respondPeople=!g.autopilot||!trafficPolicy||trafficPolicy->people!=2;
            const bool respondVehicles=!g.autopilot||!trafficPolicy||trafficPolicy->moving!=2;
			{
				std::unique_lock lock(g_sh.lock,std::try_to_lock);
				if(lock.owns_lock()){
				const auto now=GetTickCount64(),last=g_sh.lastStepAt;const auto instance=g_sh.instance.load(),owner=g_sh.vehicle.load();
				const auto body=g_sh.bodyId.load(),count=g_sh.sceneCount,first=count?g_sh.sceneBodies[0]:0,steps=g_sh.steps;
				const bool physical=g_sh.physical;lock.unlock();
				logger::Observe("flight.physics",At<std::uint32_t>(vehicle,off::kFormId),body^(std::uint64_t(count)<<32)^(std::uint64_t(physical)<<48)^(std::uint64_t(instance!=0)<<49),
					"pilot=%d active=%d autopilot=%d physical=%d scripted=%d instance=%p body=%08X scene_count=%u first_scene_body=%08X steps=%u step_age_ms=%llu controller_vehicle=%p",
					idx,g.active,g.autopilot,physical,scripted,reinterpret_cast<void*>(instance),body,count,first,steps,
					static_cast<unsigned long long>(last?now-last:0),reinterpret_cast<void*>(owner));
				}else logger::Observe("flight.snapshot",At<std::uint32_t>(vehicle,off::kFormId),0,"pilot=%d unavailable=physics_lock_busy",idx);
			}
			bool scriptTraffic = false;  // a vehicle within 5 m in the scripted direction of travel
            {
                const auto& nav=PilotNavOf(idx);const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);const auto now=GetTickCount64();
                const bool requested=g.autopilot&&goal.hold&&nav.landOnArrival&&std::hypot(goal.x-pos.x,goal.y-pos.y)<=goal.stop+2;
                if(requested) {
                    const auto site=environment::CheckSite(At<std::uint32_t>(vehicle,off::kFormId),goal.x,goal.y,pos.z,At<game::Vec3>(vehicle,off::kRefRotation).z);
                    if(site.verdict==environment::Verdict::Safe){g.landGround=site.ground;g.landProofAt=now;g.landHeight=g_sh.hull.height+0.1;
                        for(unsigned k=0;g_vsnapOk&&k<g_vsnap.count&&k<world::kMaxVehicles;++k)if(g_vsnap.v[k].ref==vehicle)g.landHeight=std::max(0.2,double(g_vsnap.v[k].half[2])-(g_vsnap.v[k].center[2]-g_vsnap.v[k].pos[2])+0.1);
                    }
                    if(site.verdict==environment::Verdict::Unsafe)g.landProofAt=0;
                }
                g.landing=requested&&g.landProofAt&&now>=g.landProofAt&&now-g.landProofAt<500;
                if(g.landing){goal.z=g.landGround+g.landHeight;}
                else if(nav.height!=vcontract::NavParams::Height::Auto || nav.minClearance>0) {
                    const auto map=navmap::At(pos.x,pos.y,pos.z);
                    if(map.haveGround){const double floor=map.ground+integrationlogic::Clearance(int(nav.height),nav.minClearance,g_sh.hull.height,kCallSkim)+EnvClimb();goal.z=std::isfinite(goal.z)?std::max(goal.z,floor):floor;}
                }
                std::lock_guard lock(g_sh.lock);g_sh.landing=g.landing||g.groundHug;
            }
			VehicleWarning(idx, vehicle, wc);
			if (g.autopilot) PersonAhead(idx, vehicle);
			if (g.autopilot && IsDisabled(At<std::uint32_t>(vehicle, off::kFormId))) {
				Stop(idx, "autopilot off: too many hits");
				return;
			}
			{
				bool hit, up;
				float delta;
				{
					std::lock_guard lk(g_sh.lock);
					hit = g_sh.collided; delta = g_sh.collisionDelta; up = g_sh.collisionUp;
					if (hit && (g.autopilot || g.vehTouch)) {
						g_sh.collided = false;
						g_sh.collisionUp = false;
					}
				}
				if (hit && g.autopilot && (g.vehTouch || !up)) g.adContact = true;
				if (hit && g.vehTouch) {
					// A contact with another vehicle: a plain collision (Havok resolves it); no floor, bump, detour or drive-mode score.
					if (logger::Verbose() && GetTickCount64() - g_bumps[idx].ev >= kBumpDebounceMs) {
						g_bumps[idx].ev = GetTickCount64();
						logger::Write("%s: contact %.0f m/s %s with vehicle %08X: a vehicle collision, not ground (no hold, no bump)", wc, delta, up ? "up" : "side", g.vehId);
					}
				}
				else if (hit && g.autopilot && GetTickCount64() - g_bumps[idx].ev < kBumpDebounceMs) {
					// the same contact episode: counted once
				}
				else if (hit && g.autopilot) {
					const ULONGLONG nowMs = GetTickCount64();
					auto& b = g_bumps[idx];
					b.ev = nowMs;
					if (nowMs - b.at > kBumpWindowMs) b.count = 0;
					++b.count;
					b.at = nowMs;
					g.cramped = std::min(kCrampedMax, g.cramped + (up ? 2.0 : 1.0));  // contacts raise the drive-mode score
					NoteContactAt(vehicle, up);
					const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
					if (b.count >= kBumpLimit) {  // logged only: the energy plan's World answers contacts (the old climb-out is gone)
						b.count = 0;
						logger::Write("%s: %d contacts in %.0f s (cramped)", wc, kBumpLimit, kBumpWindowMs / 1000.0);
					}
					if (up && !(g.wallAhead && g.wallDist < kWallStandoff + 4.0) && g.wallL > 4.0 && g.wallR > 4.0) {
						g.floorZ = pos.z;
						g.floorUntil = nowMs + kFloorMs;
						logger::Write("%s: pushed up %.0f m/s at z %.1f: something under it the ground rays miss; holding above it", wc, delta, pos.z);
					}
					else {
						// What it hit, as far as the vehicles go (2026-10-28, the user: "what collision was there?"): the nearest other vehicle.
						std::uint32_t nearId = 0; double nearD = 1e9;
						for (unsigned k = 0; g_vsnapOk && k < g_vsnap.count && k < world::kMaxVehicles; ++k) {
							const auto& o = g_vsnap.v[k];
							if (o.ref == vehicle) continue;
							const double dd = std::sqrt(std::pow(o.pos[0] - pos.x, 2) + std::pow(o.pos[1] - pos.y, 2) + std::pow(o.pos[2] - pos.z, 2));
							if (dd < nearD) { nearD = dd; nearId = o.formId; }
						}
						logger::Write("%s: bumped (%.0f m/s) at z %.1f; nearest vehicle %08X %.1f m away%s; the energy plan answers it", wc, delta, pos.z,
							nearId, nearD < 1e8 ? nearD : -1.0, nearD < 8.0 ? " (likely what it hit)" : " (not a vehicle: a building or object)");
					}
				}
				else if (hit) {
					{
						std::lock_guard lk(g_sh.lock);
						g_sh.collided = false;
						g_sh.collisionUp = false;
					}
					const ULONGLONG nowMs = GetTickCount64();
					const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
					if (nowMs - g.startedAt < kContactGraceMs || (g.liftoff && nowMs - g.startedAt < 3 * kContactGraceMs)) {
						if (g.contactsIgnored++ < 3) logger::Write("%s: contact %.0f m/s %s ignored (lift-off)", wf, delta, up ? "up" : "side");
					}
					else if (nowMs < g.contactGraceUntil) {
						// 20:41:52: a 120 m/s push up, then 18 ms later the step after it read as "83 m/s knocked off".
						if (g.contactsIgnored++ < 6) logger::Write("%s: contact %.0f m/s %s ignored (just pushed up)", wf, delta, up ? "up" : "side");
					}
					else if (up && !(g.wallAhead && g.wallDist < kWallStandoff + 4.0) && g.wallL > 4.0 && g.wallR > 4.0) {
						g.floorZ = pos.z;
						g.floorUntil = nowMs + kFloorMs;
						g.contactGraceUntil = nowMs + kPushGraceMs;
						logger::Write("%s: contact %.0f m/s up at z %.1f: ground under it; holding above it", wf, delta, pos.z);
						NoteContactAt(vehicle, true);
					}
					else if (delta < kCrashDelta) {
						logger::Write("%s: bump %.0f m/s (side); still flying", wf, delta);
						NoteContactAt(vehicle, false);
					}
					else {
						// A hard hit no longer ends the flight (the user, 2026-10-17: the recovery handles it): the hover keeps
						// flying; the next half second of contacts is the same hit settling, not new crashes.
						g.contactGraceUntil = nowMs + kHitGraceMs;
						logger::Write("%s: hard hit %.0f m/s (side); still flying", wf, delta);
						npcvehicles::WitnessCrash(pos.x, pos.y, pos.z, "a hard crash");
						NoteContactAt(vehicle, false);
					}
				}
			}

			// HO-1 (2026-11-19, 19:58:49 run: the drive keys ended Cruise, the car switched to hover and tipped up; the player turned hover off
			// 2 s later): an autopilot that started on the wheels hands back the car on its wheels (hover off), as it was before Cruise.
			if (g.drive && !g.autopilot && g.autoOnWheels) {
				logger::Write("OBS system=vehicle.handover pilot=%d event=back_to_wheels reason=autopilot_started_on_its_wheels_ended_no_hover", idx);
				Stop(idx, "autopilot ended: back on its wheels"); return; }
			if (g.drive && !g.autopilot) LeaveDrive(idx, vehicle, "autopilot ended");  // never keep overriding the controls

			LARGE_INTEGER now, freq;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			const double dt = std::clamp(double(now.QuadPart - g.last.QuadPart) / double(freq.QuadPart), 0.0, 0.1);
			g.last = now;

			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const double heading = At<game::Vec3>(vehicle, off::kRefRotation).z;
			double v[3], bodyPitch, bodyRoll, wYaw, gEst, gap, camYaw, bodyHeading;
			bool matched;
			int fixedVel;
			{
				std::lock_guard lk(g_sh.lock);
				v[0] = g_sh.v[0]; v[1] = g_sh.v[1]; v[2] = g_sh.v[2];
				bodyPitch = g_sh.bodyPitch; bodyRoll = g_sh.bodyRoll; wYaw = g_sh.wYaw; gEst = g_sh.gEst;
				matched = g_sh.matched;
				gap = g_sh.gap; camYaw = g_sh.camYaw; bodyHeading = matched ? g_sh.bodyHeading : heading;
				fixedVel = g_sh.fixedVel;
			}
			const double sp = std::hypot(v[0], v[1]);

			ReadSamples(idx);
			ReadCorridors(idx);
			WallScan(idx, wc);
			const bool groundOk = g.haveGround && GetTickCount64() - g.groundAt <= 500;
			if (groundOk) {
				g.baseZ = g.ground;
			}
			const double above = groundOk ? pos.z - g.ground : pos.z - g.baseZ;

            // Handoff admission supplies transport permission before Route and Plan choose motion.
            const auto commandVehicle = At<std::uint32_t>(vehicle, off::kFormId);
            const auto wheelAdmission = npcvehicles::AdmitWheelOutput(idx, commandVehicle);

			// Removal 2026-11-14: the energy plan decides heading, speed, wheels or hover for every car on autopilot (AutodriveDecide, step 7);
			// a command (combat, a job's hold) overrides only the channels it sets. The 0.8 rules and the old autopilot are gone.
			if (g.autopilot) {
				LARGE_INTEGER adStart{}, adEnd{}, adFreq{};
				QueryPerformanceCounter(&adStart);
				AutodriveDecide(idx, vehicle, goal, pos, g.drive ? heading : bodyHeading, v, groundOk, groundOk ? g.ground : g.baseZ, above, wc);
				QueryPerformanceCounter(&adEnd); QueryPerformanceFrequency(&adFreq);
				{
					// The decision's own cost (the FPS pass): average and worst per pilot, every 10 s (detailed log).
					const double ms = 1000.0 * double(adEnd.QuadPart - adStart.QuadPart) / double(adFreq.QuadPart);
					static double sum[kPilots] = {}, worst[kPilots] = {};
					static unsigned calls[kPilots] = {};
					static ULONGLONG reportAt = 0;
					sum[idx] += ms; worst[idx] = std::max(worst[idx], ms); ++calls[idx];
					const ULONGLONG tNow = GetTickCount64();
					if (!reportAt) reportAt = tNow;
					if (tNow - reportAt >= 10000) {
						reportAt = tNow;
						if (logger::Verbose()) for (int k = 0; k < kPilots; ++k) if (calls[k])
							logger::Write("Autopilot cost: %s %.3f ms avg / %.2f worst over %u decisions", k ? Who(k) : "Rover", sum[k] / calls[k], worst[k], calls[k]);
						for (int k = 0; k < kPilots; ++k) { sum[k] = worst[k] = 0; calls[k] = 0; }
					}
				}
				const auto& o = g.adOut;
				// The energy plan's World holds walls, people and vehicles: the wall scan's warning is not a second writer for a car it drives.
				g.wallAhead = false; g.wallDist = 1e9; g.wallL = g.wallR = 1e9;
				if (o.reverse) g.reverseUntil = GetTickCount64() + 150;
				else if (g.reverseUntil > GetTickCount64()) g.reverseUntil = 0;
				// In a fight the hover keeps a low band (about 8 m over the usual skim height): a firing line over cover, still
				// under ceilings (the hold stays under any ceiling the rays see).
				g.cruiseExtra = o.mode == drive::Mode::Hover ? o.climb + (g.adCombat ? 8.0 : 0.0) : 0.0;
			}

			// ---- Input ----
			// Alt tap/hold is resolved once in PlayerTick, shared by driving and hover.
			// W held: boost at the selected gear. S held: brake. Neither: coast.
			const bool keysOn = focused && !g.autopilot;  // on autopilot the player's keys do not fly the rover
			const bool boostHeld = keysOn && bindings::Held(bindings::Action::Boost, VK_LSHIFT);
			const bool wHeld = keysOn && bindings::Held(bindings::Action::Accelerate, g_mk.accel), sHeld = keysOn && bindings::Held(bindings::Action::Brake, g_mk.brake);
			const bool sWasDown = g.sDown, boostWasDown = g.boostDown;
			{
				g.wDown = wHeld;
				g.sDown = sHeld;
				g.boostDown = boostHeld;
			}
            if(keysOn&&bindings::Triggered(bindings::Action::ThrottleLock))g.wHold=!g.wHold;
            else if((boostHeld&&!boostWasDown)||(sHeld&&!sWasDown))g.wHold=false;

			const bool paidBoost = (boostHeld || g.wHold) && g_meterBoost.load() && g_meterVehicle.load()==vehicle;
			// The player's throttle as an amount (2026-10-24: analog sticks; a key or the boost lock is all of it).
			const double throttleIn = g.wHold || paidBoost ? 1.0 : keysOn ? bindings::Amount(bindings::Action::Accelerate, g_mk.accel) : 0.0;
			bool boosting = wHeld || throttleIn > 0.0;
			bool sBrake = sHeld;
			// Autopilot: accelerate while the distance left exceeds the braking distance, then brake; arrive and stop.
			double apBearing = 0;
			bool apSteer = false;
			int apGear = g.gear;
			if (g.autopilot) {
				const double dist = std::hypot(goal.x - pos.x, goal.y - pos.y);
				// The energy plan's heading and speed (apGear 4: the AI gear table's top stays out of the way; `top` below caps it).
				const auto& o = g.adOut;
				apBearing = o.bearing;
				apSteer = o.phase != drive::Phase::Arrived;
				apGear = 4;
				g.aiGears = true;
				const double cur = g.drive ? g.drvSp : sp;
				g.throttle = o.reverse ? 0.0 : std::clamp((o.speed - cur) / 4.0, 0.0, 1.0);
				boosting = g.throttle > 0.05;
				sBrake = o.brake || (!o.reverse && cur > o.speed + 1.5);
				if (!goal.keepGoing && AtDestination(vehicle, goal.x, goal.y, goal.z, goal.stop)) {
					logger::Write("%s: arrived %.1f m from the target", wc, dist);
					Stop(idx, "arrived");
					return;
				}
				if (!goal.keepGoing && GetTickCount64() - g.autopilotSince > goal.timeoutMs) {
					Stop(idx, "call timed out");
					return;
				}
			}
			const bool boost = boosting;  // booster visuals while accelerating
			// BO-2: a flying car on autopilot executes Speed's request (Output::boost) as the same thrust multiplier, while its hover meter has charge
			// (g_hoverBoostActive); the plan's speed + 1 and the flight gear's top still cap it, so it adds thrust, not speed past the plan.
			const bool hoverAsk = g.autopilot && !g.drive && g.adOut.boost && !g.adOut.reverse;
			g_hoverBoostWant[idx].store(hoverAsk, std::memory_order_relaxed);
			const bool hoverBoost = hoverAsk && g_hoverBoostActive[idx].load(std::memory_order_relaxed);
			const double boostMultiplier=(paidBoost||hoverBoost)?boostlogic::kMultiplier:1.0;
			double thrustAcc = boosting ? boostMultiplier*kThrust * (g.aiGears ? kAiGearThrust[apGear] : kGearThrust[apGear]) * (g.autopilot ? g.throttle : std::max(throttleIn, wHeld ? 1.0 : 0.0)) : 0.0;
			bool thrust = thrustAcc > 0.0;
			const double pitchIn = 0.0;
			// The camera aim: its pitch above its resting pitch is the nose pitch wanted.
			double camPitch = 0, pitchAim = 0;  // modified by the avoidance climb
			bool firstPerson=false;
			bool haveAim = !g.autopilot && CameraAimReading(vehicle,camPitch,firstPerson);  // autopilot: its own pitch below
			if (haveAim) {
				g_camRest.Observe(camPitch,firstPerson,false);
				pitchAim = std::clamp(camPitch - g_camRest.pitch, -kPitchMax, kPitchMax);
			}
			// Analog (2026-10-24): half a stick rolls at half rate; A / D are all of it.
			double rollIn = (keysOn ? bindings::Amount(bindings::Action::TurnRight, g_mk.right) : 0.0) -
				(keysOn ? bindings::Amount(bindings::Action::TurnLeft, g_mk.left) : 0.0);
			double lift = (keysOn && bindings::Held(bindings::Action::LiftUp, g_keys.up) ? 1.0 : 0.0) -
				(keysOn && bindings::Held(bindings::Action::LiftDown, g_keys.down) ? 1.0 : 0.0);
			if (!idx && keysOn && g_altDrop) lift = -1.0;
			g_forwardBoosts[idx] = boost;

			// ---- Yaw follows the camera gap in the vehicle step. The camera is only read, never written. ----
			double view = 0;
			bool haveCam = CameraHeading(view);
			if (g.autopilot) {
				view = apBearing;  // the player's bearing stands in for the camera: the rover turns toward the player
				haveCam = apSteer;
			}
			double yawWant = 0.0;

			double top = (g.aiGears ? kAiGearTop[apGear] : boosting ? kGearTop[apGear] : std::max(kGearTop[apGear], kSpeedCap))*boostMultiplier;  // AI: the gear caps it
			if (g.autopilot) {
				const auto& nv = PilotNavOf(idx);  // the profile's speed limit (0 = the gears' own)
				if (nv.speedLimit > 0) top = std::min(top, double(nv.speedLimit));
				top = std::min(top, g.adOut.speed + 1.0);  // the energy plan's speed
			}
			// ---- FC-1 / FG-1: the flight gear row (the one row both hover writers and the flight planner read) ----
			// The player: the gear key's gear (no automatic shift, AG-1's rule; its top is already kGearTop = the row's). The autopilot:
			// FG-1 (flightgear.h), and the gear's top caps the speed (the cap eases down: no snap on a downshift). On its wheels the
			// autopilot's flight gear restarts (the next take-off picks at once); AG-1 stays the ground rule.
			int flightGear = std::clamp(g.gear, 1, 4);
			const char* gearReason = "";  // BO-2: why FG-1 picked this gear (a precise flight is not a thrust question)
			if (g.autopilot) {
				if (g.drive) g.flightShift = {};
				flightgear::Ask fa;
				fa.landing = g.landing; fa.takeOff = g.liftoff || g.drive; fa.planSpeed = g.adOut.speed;
				fa.car = bicycle::Spec{};  // Stage F: the spec the energy plan plans with (brake, hoverClear, climbRate, hoverAccel) sets the gear zones
				fa.strafe = idx < int(sizeof g_planSplit / sizeof g_planSplit[0]) && g_planSplit[idx].at && GetTickCount64() - g_planSplit[idx].at < 300;
				fa.floorGap = groundOk ? above : 1e9;
				fa.toArrival = goal.keepGoing ? 1e9 : std::hypot(goal.x - pos.x, goal.y - pos.y);
				fa.obstacle = std::min(g.wallAhead ? g.wallDist : 1e9, g.vehWarn ? g.vehGap : 1e9);
				fa.want = g_flightWant[idx].load(std::memory_order_relaxed); fa.speed = sp; fa.peakTurn = g_flightPeak[idx].load(std::memory_order_relaxed);
				{ std::lock_guard lk(g_sh.lock);
				  for (int i = 1; i <= 4; ++i) fa.lateral[i] = hulllogic::Flight(g_sh.hullBase, g_sh.physical && !g_sh.physicalFault, carcap::Flight(i, true).lateralScale).lateral; }
				const auto pick = flightgear::Target(fa);
				gearReason = pick.reason;
				const int was = g.flightShift.gear;
				if (flightgear::Update(g.flightShift, pick, double(GetTickCount64()) / 1000.0) && !g.drive)  // PF-1: on wheels the gear restarts every pass (64k lines a run)
					logger::Write("OBS system=vehicle.gear vehicle=%08X pilot=%d event=flight_shift from=%d to=%d target=%d want=%.1f plan_speed=%.1f speed=%.1f peak_turn=%.3f need_lateral=%.2f gear_lateral=%.2f floor_gap=%.1f to_arrival=%.0f obstacle=%.0f on_wheels=%d reason=%s",
						At<std::uint32_t>(vehicle, off::kFormId), idx, was, g.flightShift.gear, pick.gear, fa.want, fa.planSpeed, sp, fa.peakTurn, pick.need,
						fa.lateral[g.flightShift.gear], fa.floorGap, fa.toArrival, fa.obstacle, int(g.drive), g.flightShift.reason);
				flightGear = g.flightShift.gear;
				top = std::min(top, carcap::kFlightTop[flightGear]);
			}
			const carcap::FlightRow flightRow = carcap::Flight(flightGear, g.autopilot);
			// ---- Avoidance controller (10 Hz decisions, held at least kAvoidHoldMs) ----
			bool braking = sBrake;
			{
				double needF = 0, needL = 0, needR = 0;
				const int f = Corridor(idx, 0, pos.z, sp, needF), l = Corridor(idx, 1, pos.z, sp, needL), r = Corridor(idx, 2, pos.z, sp, needR);
				g.lastF = f;
				g.lastL = l;
				g.lastR = r;
				g.lastNeed = needF;
				int want = 0;
				if (g.autopilot) {
					want = 0;  // the energy plan chose heading and speed; no assist turn, climb or stop on top of it
				}
				else if (sp < 3.0 && !boosting) want = 0;        // parked in the air: nothing to avoid
				else if (f == 0) want = 0;
				else if (f == 1) want = 1;                        // a way over: nose up and climb
				else if (l < 2 && (r == 2 || needL <= needR)) want = 2;
				else if (r < 2) want = 3;
				else want = 4;                                    // no way forward: stop
				const ULONGLONG nowMs = GetTickCount64();
				if (want != g.avoid && (want > g.avoid || nowMs - g.avoidSince >= kAvoidHoldMs)) {
					static const char* names[5] = { "clear", "climbing over", "turning left", "turning right", "stopped - no way forward" };
					logger::Write("%s: assist %s -> %s (ahead %d need %.1f m, left %d %.1f, right %d %.1f; speed %.1f, z %.1f)", wf, names[g.avoid], names[want], f, needF, l,
						needL, r, needR, sp, pos.z);
					if (want && !idx) notify::Log("Assist: %s", names[want]);
					g.avoid = want;
					g.avoidSince = nowMs;
				}
				switch (g.avoid) {
				case 1:
					pitchAim = std::max(pitchAim, 0.35);
					lift = std::max(lift, 1.0);
					break;
				case 2:
				case 3:
					if (rollIn == 0.0) rollIn = g.avoid == 2 ? -1.0 : 1.0;  // the player's own A / D wins
					yawWant = (g.avoid == 2 ? -1.0 : 1.0) * kAvoidTurnRate;
					top = std::min(top, kAvoidSlowTo);
					break;
				case 4:
					braking = true;
					thrustAcc = 0.0;
					thrust = false;
					top = 0.0;
					break;
				default:
					break;
				}
			}

			// ---- Autopilot attitude: yaw, pitch and roll each controlled (not the camera gap or the player's keys) ----
			if (g.autopilot) {
				// Yaw: a turn rate toward the target heading (the avoidance turn, when active, sets its own rate).
				double yawRate = yawWant;
				if (g.avoid != 2 && g.avoid != 3) {
					yawRate = apSteer ? std::clamp(kApYawGain * Wrap(apBearing - bodyHeading), -kApYawMax, kApYawMax) : 0.0;
					yawWant = yawRate;
				}
				haveCam = false;  // no camera gap: the yaw rate above is the whole turn
				// Pitch: nose along the climb or descent wanted; nose up over an obstacle; level while braking or climbing up.
				const double vzWanted = g.avoid == 1 ? kHoldMaxVz : std::clamp(kHoldGain * (g.hold - above), -kHoldMaxVz, kHoldMaxVz);
				double pitchT = g.avoid == 1 ? kApClimbPitch : std::atan2(vzWanted, std::max(sp, 8.0));
				if (braking) pitchT = 0.0;
				pitchAim = std::clamp(pitchT, -kApPitchMax, kApPitchMax);
				haveAim = true;
				// Roll: bank into the turn in proportion to the turn rate (level when straight).
				const double bankT = std::clamp(kApBankPerYaw * yawRate * std::clamp(sp / 15.0, 0.0, 1.0), -kApBankMax, kApBankMax);
				rollIn = std::clamp(kApRollGain * (bankT - bodyRoll), -1.0, 1.0);
			}

			// ---- Speed cap from the environment: the ground ahead must be loaded ----
			const bool ray0 = g.asked[kAhead0].valid, ray1 = g.asked[kAhead0 + 1].valid, ray2 = g.asked[kAhead0 + 2].valid;
			const bool cell0 = CellAttachedAt(g.asked[kAhead0].x, g.asked[kAhead0].y);
			const bool cell1 = CellAttachedAt(g.asked[kAhead0 + 1].x, g.asked[kAhead0 + 1].y);
			const bool cell2 = CellAttachedAt(g.asked[kAhead0 + 2].x, g.asked[kAhead0 + 2].y);
			const bool near0 = ray0 && cell0, near1 = ray1 && cell1, far2 = ray2 && cell2;
			const bool aheadLoaded = near0 && near1 && far2;
			// Ground must be both ray-visible and in a fully attached cell. Keep the graded 0.75/1.5/2.5 s response,
			// but do not accelerate into a cell whose terrain exists while its engine attachment is still pending.
			double capWanted;
			if (!groundOk) capWanted = kSpeedCapBlind;
			else if (aheadLoaded) capWanted = top;
			else if (near0 && near1) capWanted = std::max(kSpeedCapBlind, std::min(top, sp + 2.0));  // hold the speed
			else if (near0) capWanted = std::max(kSpeedCapBlind, std::min(top, sp * 0.85));
			else capWanted = kSpeedCapBlind;
			const double capResponse = capWanted >= g.cap ? 0.5 : near0 ? 1.5 : 3.0;
			g.cap += (capWanted - g.cap) * (1.0 - std::exp(-dt * capResponse));
			if (g.autopilot && !g.drive && idx >= 0 && idx < kPilots) {  // BO-2: the flying car's measured facts for Speed (boostneed.h), published each update
				g_pedalDemand[idx].store(float((g.adOut.speed - sp) / 4.0), std::memory_order_relaxed);  // the hover throttle before its clamp
				g_hoverCap[idx].store(float(std::min(g.cap, top)), std::memory_order_relaxed);
				g_hoverSteer[idx].store(float(std::fabs(yawWant) / kApYawMax), std::memory_order_relaxed);
				g_hoverHalt[idx].store(g.landing ? "landing" : g.liftoff ? "take_off" : (!std::strcmp(gearReason, "wanted_speed") || !std::strcmp(gearReason, "long_straight_leg")) ? nullptr : gearReason, std::memory_order_relaxed);
				g_pedalAt[idx].store(GetTickCount64(), std::memory_order_relaxed);
			}

			// ---- Drive mode ----
			if (g.autopilot) {
				const ULONGLONG tNow = GetTickCount64();
				g.cramped = std::max(0.0, g.cramped - kCrampedDecay * dt);
				const bool occupied = idx >= 1 || g_driverVehicle.load(std::memory_order_relaxed) == vehicle;  // never an empty vehicle
				// NPC pilots choose hover or drive from their profile and the reasons around them (2026-10-21, travelmode.h); your own
				// vehicle keeps the old rule (drive only while cramped).
				bool decided = false, wantDrive = false, nativeWanted = false;
				const char* travelWhy = "cramped";
				// NPC pilots, and your own vehicle on autopilot (2026-10-21, the user: summon, follow, cover, guard, Auto), except a ram.
				const auto& scmd = g_cmds[idx].c;
				const int scriptTravel = (scripted || manual) && (scmd.set & Command::kTravel) ? scmd.travel : 0;
				if (scriptTravel == 1 || scriptTravel == 2) {
					// The script decides (2026-10-25, the user: "drive and float when needed ... swapping based on obstacle based on script").
					decided = true; wantDrive = scriptTravel == 2; nativeWanted = manual && wantDrive && occupied;  // yours: the game's own wheels
					travelWhy = manual ? (wantDrive ? "you: drive" : "you: float") : wantDrive ? "script: drive" : "script: float";
					const bool hug = wantDrive && !g.drive;
					if (hug != g.groundHug) {
						g.groundHug = hug;
						logger::Write("%s: travel: %s (%s)", wc, hug ? "on the ground (hover held low)" : "flying", travelWhy);
					}
				}
				else {
					// The energy plan picks wheels or hover; the game's own wheels when someone is aboard, else the hover held low.
					decided = true;
					wantDrive = g.adOut.mode == drive::Mode::Drive;
					nativeWanted = wantDrive && occupied;
					travelWhy = g.adOut.why && *g.adOut.why ? g.adOut.why : "energy plan";
					const bool hug = wantDrive && !g.drive;
					if (hug != g.groundHug) {
						g.groundHug = hug;
						logger::Write("%s: travel: %s (energy plan: %s)", wc, hug ? "on the ground (hover held low)" : "flying", travelWhy);
					}
				}
				if (!decided && g.groundHug) {  // hover or drive switched off: back to the hover
					g.groundHug = false;
					logger::Write("%s: travel: flying (hover or drive off)", wc);
				}
				// Only on known ground (2026-10-29 23:02 run: traffic placed on its lane with the ground below not yet known read
				// "0.0 m above ground" (height from where its hover started), switched hover off and dropped ~150 m into the city).
				const bool forceDriveOnly = idx >= 0 && idx < int(sizeof g_driveOnlyLane / sizeof g_driveOnlyLane[0]) && g_driveOnlyLane[idx];
				const bool enterDrive = decided ? nativeWanted && (forceDriveOnly || (groundOk && above < 2.5)) : groundOk && g.cramped >= kCrampedOn;  // a hovering vehicle comes down first; drive-only never hovers
				if (!g.drive && occupied && enterDrive && (forceDriveOnly || std::fabs(v[2]) < 4.0)) {  // not mid-fall or mid-climb; drive-only disengages hover at once
					g.drive = true;
					g.driveSince = tNow;
					g.drvT = 0;
					g.drvSteer = 0;
					{
						std::lock_guard lk(g_sh.lock);
						g_sh.active = false;  // the hover model stops writing; the game's own step runs
						g_sh.haveLast = false;
					}
					g_clearBoosts[idx] = vehicle;
					g_forwardBoosts[idx] = false;
					logger::Write("%s: drive mode ON (%s): hover off, driving with boost for forward and up", wc, travelWhy);
				}
				else if (g.drive && decided && (!wantDrive ||
					(!nativeWanted && tNow - g.driveSince > kDriveMinMs && g.lastF == 0 && g.lastL < 2 && g.lastR < 2))) {
					LeaveDrive(idx, vehicle, wantDrive ? "clear again: on the ground, hover held low" : travelWhy);
				}
				else if (g.drive && !decided && tNow - g.driveSince > kDriveMinMs && g.cramped < 0.5 && g.lastF == 0 && g.lastL < 2 && g.lastR < 2) {
					LeaveDrive(idx, vehicle, "clear again");
				}
				if (g.drive) {
					// Speed from position differences (no physics step in this mode); the steer sign is checked against the
					// heading change it causes.
					// A relocation moved this car's pose on purpose (relocation.h): the jump is not speed. 2026-11-20 19:40 run: a 6 m move read as
					// 24 m/s here (position difference over a short sample), which the plan then answered (moving_without_counted_steps, phase=reverse).
					// While the move is in progress and once at its end the baseline is re-taken and the speed already known is kept.
					bool relocationJump = false;
					{
						static std::uint64_t relocMark[kPilots] = {};
						const std::uint64_t mark = npcvehicles::RelocationMark(At<std::uint32_t>(vehicle, off::kFormId));
						if (mark != relocMark[idx] || (mark & 1u)) {
							relocationJump = true;
							logger::Observe("autopilot.speed", At<std::uint32_t>(vehicle, off::kFormId), 0x20u + (mark & 1u), "pilot=%d event=baseline_retaken mark=%llu in_progress=%d speed_kept=%.2f reason=relocation_jump_is_not_speed", idx, (unsigned long long)mark, int(mark & 1u), g.drvSp);
							relocMark[idx] = mark;
						}
					}
					if (relocationJump && g.drvT) { g.drvX = pos.x; g.drvY = pos.y; g.drvT = tNow; g.drvHead = heading; }
					else if (g.drvT && tNow > g.drvT) {
						const double dd = (tNow - g.drvT) / 1000.0;
						if (dd >= 0.02) {
							const double inst = std::hypot(pos.x - g.drvX, pos.y - g.drvY) / dd;
							g.drvSp += (inst - g.drvSp) * std::min(1.0, dd * 4.0);
							if (g.drvSp > 4.0 && std::fabs(g.drvSteer) > 0.5 && tNow >= g.hopUntil) {
								g.steerResp += g.drvSteer * g.steerSign * Wrap(heading - g.drvHead);
								g.steerSecs += dd;
							}
							g.drvX = pos.x; g.drvY = pos.y; g.drvT = tNow; g.drvHead = heading;
						}
					}
					else if (!g.drvT) {
						g.drvX = pos.x; g.drvY = pos.y; g.drvT = tNow; g.drvHead = heading; g.drvSp = 0;
					}
					if (g.steerSecs >= 2.0) {  // judged over 2 s of steering on the ground, never on noise
						if (g.steerResp < -0.4 && tNow - g.driveSince > 3000) {
							g.steerSign = -g.steerSign;
							logger::Write("%s: drive: steering ran the other way (%.2f over %.1f s); sign reversed", wc, g.steerResp, g.steerSecs);
						}
						g.steerResp = 0;
						g.steerSecs = 0;
					}
					const double spD = g.drvSp;
					double steer = 0, accel = 0, brake = 0, pedalDemand = 0; bool fwdBoost = false, vert = false;  // pedalDemand: BO-1
					double railVx = 0, railVy = 0, railYaw = 0, railAimOut = 0;
					bool railActive = false, railTyres = false;
					const double wheelFwd = v[0] * std::sin(heading) + v[1] * std::cos(heading);
					const double tgt = std::min(top, kDriveTop);
					const bool boxed = g.lastF == 2 && g.lastL == 2 && g.lastR == 2 && spD < 2.0;  // the trip status reads it
                    const double dist=std::hypot(goal.x-pos.x,goal.y-pos.y);
                    g.trip={true,triplogic::Status(groundOk,
                        triplogic::Reached(dist,goal.stop,std::isfinite(goal.z)?pos.z-goal.z:0,spD,groundOk),dist<=goal.stop+2,
                        std::isfinite(goal.z)?pos.z-goal.z:0,GetTickCount64()<g.reverseUntil,boxed),goal.x,goal.y,dist,spD,0,GetTickCount64()};
					{
						// The energy plan's heading and speed on the game's own wheels.
						const auto& o = g.adOut;
						const double eAd = Wrap(o.bearing - heading);
						// Small corrections ease off at a crawl (full lock stays for big errors), so a slow car's nose does not swing
						// back and forth across the waypoint (2026-11-05 00:10:01-03: -62, -38, -8, +2 deg at 0.3-0.7 m/s).
						const double crawl = std::clamp(std::hypot(v[0], v[1]) / 6.0, 0.0, 1.0);
						const double gain = std::fabs(eAd) < 0.35 ? 1.8 * (0.6 + 0.4 * crawl) : 1.8;
						steer = std::clamp(gain * eAd, -1.0, 1.0);
						// Signed speed along the nose. The game's brake pedal reverses a stopped car, so braking on |speed| made a car
						// told to stop back up, and its backward speed then kept the brake (= reverse) on (2026-11-04 23:38 video:
						// "a lot of reverse"). Brake only while moving forward; rolling back when it should not, a little throttle.
						const double fwd = v[0] * std::sin(heading) + v[1] * std::cos(heading);
						accel = std::clamp((o.speed - fwd) / 4.0, 0.0, 1.0);
						brake = fwd > o.speed + 1.5 ? std::clamp((fwd - o.speed) / 6.0, 0.0, 1.0) : 0.0;
						// BO-1: the forward boost is Speed's request (boostneed.h), executed here. REPLACED: o.speed > kDriveTop with the nose near the bearing,
						// a decision hiding in the actuator that boosted whatever the thrust, a hold, or the meter.
						fwdBoost = o.boost && !o.reverse;
						pedalDemand = (o.speed - fwd) / 4.0;  // BO-1: the pedal before its clamp, published below as Speed's thrust-limited evidence
						vert = false;
						if (o.reverse) { accel = 0.0; brake = 1.0; steer = -steer; }  // the brake pedal backs up once stopped; the nose swings toward the bearing
						// Full brake only when the car should stop. The lane driver's brake flag with a speed still to reach means "slow down":
						// treated as full brake, a car turning at 1.5 m/s braked at 1.65, the game's brake then backed it up, the throttle sent
						// it forward, and forward + reverse at full lock cancelled the turn (2026-11-05 00:33: side-on for 70 s at -608,1476).
						else if (o.speed < 0.3) { accel = fwd < -0.3 ? std::min(0.4, -fwd * 0.4) : 0.0; brake = fwd > 0.5 ? 1.0 : 0.0; }  // at rest no pedal: held, it would back up
						else if (o.brake) { accel = 0.0; brake = fwd > o.speed + 0.5 ? std::clamp((fwd - o.speed) / 6.0, 0.0, 0.6) : 0.0; }
						if (!o.reverse && fwd < -0.5) logger::Observe("traffic.drive", 0, 0x300ull + idx, "event=rolling_back pilot=%d fwd=%.2f wanted=%.2f action=throttle_not_brake", idx, fwd, o.speed);
						// Rail drive: a traffic car on a Drive lane is moved directly (see Shared::railOn). The velocity points along the nose
						// turned at most 25 deg toward the lane's bearing, and the nose turns toward it at a rate that grows with speed, so a
						// car side-on to its lane drives a short arc instead of sliding sideways. Stop means stop (no brake = reverse).
						// Cruise too (pilot 0 on autopilot): the player's own car on a Drive lane rides the rail; any control input ends Cruise first.
						const bool rail = g_driveRail && in_railLane(idx) && (idx >= 1 || g.autopilot) && o.mode == drive::Mode::Drive;
						{
							std::lock_guard rl(g_sh.lock);
							const bool was = g_sh.railOn;
							if (rail) {
								const bool stopNow = o.speed < 0.1;
								double want = stopNow ? 0.0 : std::min(o.speed, kDriveTop);
								// Cruise boost: your boost key (with the game's meter) raises the speed while only top speed limits the car.
								if (idx == 0 && paidBoost && g_railFree[idx] && !stopNow) want = std::min(o.speed * boostlogic::kMultiplier, g_railCorner[idx]);
								// Forward: the rail's look-ahead bearing; travel at most 35 deg off the nose while the nose catches up, and the nose
								// turns at a rate that grows with speed (a moving car turns, a still one barely). Reverse keeps the lane's bearing.
								double aim = o.reverse ? o.bearing : g_railBearing[idx];
								// AF-1: with a fresh plan path, the nose aims where the path follower says and turns at its rate (curvature feedforward +
								// heading and lateral feedback), forward and backing up alike. Without one: exactly as before.
								pathfollow::Out follow; bool following = false;
                                const bool tyreMode = RailTyresOn();
                                double laneYaw = 0, measuredRightYaw = 0;
                                bool yawDamped = false;
								if (idx >= 0 && idx < int(sizeof g_planPath / sizeof g_planPath[0])) {
									FollowPath fp; { std::lock_guard lk(g_followLock); fp = g_planPath[idx]; }
									if (fp.at && tNow - fp.at < 400 && fp.reverse == o.reverse) {
										follow = pathfollow::Follow(fp.path, pos.x, pos.y, heading, fwd, o.reverse);
										if (follow.ok && idx <= prefetch::kSlotPilotCount) {  // TK-1: how well the car follows its plan, fed to Speed next tick
											tracking::Rules tr; tr.slipOk = 15.0;  // the rear slip angle reads high at a crawl and in tight turns: 15 deg before it slows the car
											tracking::Sample ts; ts.now = double(tNow) / 1000.0; ts.lateralErr = follow.lateral; ts.headingErr = follow.headingError;
											ts.yawRate = g_yawMeasured[idx].load(std::memory_order_relaxed); ts.speed = std::fabs(fwd); ts.onWheels = o.mode == drive::Mode::Drive && !o.reverse;
											ts.rearSlipDeg = std::fabs(fwd) > 4.0 ? g_rearSlipDeg[idx].load(std::memory_order_relaxed) : 0.0;
											ts.grip = wheelgrip::PlanGrip(GearOfSlot(idx), g_gripSet);  // one car: tracking's planning grip
											const auto to = tracking::Update(g_track[idx], ts, tr);
											g_trackCap[idx].store(to.cap, std::memory_order_relaxed); g_trackWhy[idx].store(to.why, std::memory_order_relaxed);
											static double trackLogAt[1 + prefetch::kSlotPilotCount] = {}; static const char* trackLogWhy[1 + prefetch::kSlotPilotCount] = {};
											const double tnow = double(tNow) / 1000.0;
											const bool logIt = to.changed && (to.why != trackLogWhy[idx] || tnow - trackLogAt[idx] >= 1.0 || tnow < trackLogAt[idx]);  // a new reason, or once a second
											if (logIt) { trackLogAt[idx] = tnow; trackLogWhy[idx] = to.why; }
											if (logIt) logger::Write("OBS system=drive.tracking pilot=%d cap=%.1f why=%s score=%.2f weave_flips=%d worst_lateral=%.2f worst_heading_deg=%.1f worst_rear_slip=%.1f speed=%.1f reason=plan_vs_car",
												idx, std::isfinite(to.cap) ? to.cap : -1.0, to.why ? to.why : "", to.score, to.weaveFlips, to.worstLateral, to.worstHeading * 57.29578, to.worstSlip, std::fabs(fwd)); }
										if (follow.ok) {
                                        following = true; aim = follow.bearing;
                                        laneYaw = follow.yawRate;
                                        const auto yawAt = g_yawAt[idx].load(std::memory_order_acquire);
                                        if (tyreMode && !o.reverse && fwd > .1 &&
                                            yawAt && yawAt <= tNow && tNow - yawAt <= 250 &&
                                            g_yawInstance[idx].load(std::memory_order_relaxed) == g_sh.instance.load(std::memory_order_relaxed)) {
                                            // The physics spin convention is positive left; the follower is positive right.
                                            measuredRightYaw = -g_yawMeasured[idx].load(std::memory_order_relaxed);
                                            yawDamped = std::isfinite(measuredRightYaw);
                                            laneYaw = pathfollow::DampedLaneYaw(follow, fwd, measuredRightYaw,
                                                                                g.drvSteer, wheelgrip::kSteerRate);
                                        }
											if (tNow - fp.logAt >= 500) { { std::lock_guard lk(g_followLock); g_planPath[idx].logAt = tNow; }
												logger::Write("OBS system=plan.follow pilot=%d reverse=%d lateral=%.2f heading_err=%.1f curvature=%.3f yaw_rate=%.2f bearing=%.1f heading=%.1f fwd=%.2f reference=%s yaw_damped=%d measured_right_yaw=%.2f applied_yaw_target=%.2f reason=rail_follows_the_plan_path",
													idx, int(o.reverse), follow.lateral, follow.headingError * 57.29578, follow.curvature, follow.yawRate, follow.bearing * 57.29578, heading * 57.29578,  fwd, fp.laneCentre ? "lane_centre" : "rollout", int(yawDamped), measuredRightYaw, laneYaw); } } }
								}
								const double eRail = Wrap(aim - heading);
								const double dir = (o.reverse ? heading + 3.14159265358979 : heading) + std::clamp(eRail, -0.6, 0.6) * (o.reverse ? -1.0 : 1.0);
								const double maxYaw = std::min(1.6, 0.4 + std::fabs(fwd) / 4.0);
								railVx = std::sin(dir) * want; railVy = std::cos(dir) * want;
								// Damped turn (jitter pass): lower gain, and the command eases toward its target instead of jumping, so the nose
								// stops overshooting and swinging back (the +-4 m/s sideways flips at corners).
								const double yawTarget = -std::clamp(following ? laneYaw : 2.5 * eRail, -maxYaw, maxYaw);  // heading + = right; world spin + = left (AF-1: the follower's rate)
								g_railYawCmd[idx] += (yawTarget - g_railYawCmd[idx]) * 0.3;
								railYaw = g_railYawCmd[idx]; railAimOut = aim; railActive = true;
								// TY-2 (user: "use rail for backward it's fine"): backing up stays on the rail's velocity writes (the plan's reverse speed).
								// TY-3: with railTyresReverse on, backing up is on the tyres too (one signed pedal); off, it stays on the rail (TY-2).
								{ const bool tyOn = tyreMode, ty = tyOn && (!o.reverse || RailTyresReverseOn());
								  if (ty != g_sh.railTyres) logger::Observe("traffic.tyres", 0, 0x500ull + idx, "event=%s pilot=%d speed=%.1f reason=%s", ty ? "tyres_on" : "tyres_off", idx, fwd, ty ? (o.reverse ? "reverse_on_tyres" : "forward") : (tyOn ? "reverse_on_rail" : "switch_off"));
								  railTyres = ty; }
								if (tNow - g_railRoadNAt[idx] < 1000) { for (int k = 0; k < 3; ++k) g_sh.railRoad[k] = g_railRoadN[idx][k]; g_sh.railRoadAt = g_railRoadNAt[idx]; }
								if (tNow - g_railFloorZAt[idx] < 1000) { g_sh.railFloorZ = g_railFloorZ[idx]; g_sh.railFloorAt = g_railFloorZAt[idx]; }
								g_sh.railAir = idx == 0 && keysOn && bindings::Held(bindings::Action::VerticalBoost, VK_SPACE);
								g_sh.px = float(pos.x); g_sh.py = float(pos.y); g_sh.pz = float(pos.z);
							}
							else if (was) { g_sh.railOn = false; g_sh.railTyres = false; g_sh.railWall.valid = false; g_sh.railCommand = -1; g_sh.railMotion.store(0); }
							if (rail != was) logger::Observe("traffic.rail", 0, 0x400ull + idx, "event=%s pilot=%d speed=%.1f steps=%u faults=%u", rail ? "on" : "off", idx, fwd, g_sh.railSteps, g_sh.railFaults);
							// Probes: what the wheel stage decided, and whether the rail's physics step is running at all.
							static ULONGLONG wheelProbeAt[kPilots] = {};
							if (rail && tNow - wheelProbeAt[idx] >= 3000) {
								wheelProbeAt[idx] = tNow;
								logger::Write("OBS system=drive.wheel pilot=%d selected_speed=%.2f rail_target=%.2f fwd=%.2f reverse=%d jump=%d accel_before_rail=%.2f brake_before_rail=%.2f aim_err=%.1f aim_jump=%.2f yaw_cmd=%.2f",
									idx, o.speed, std::hypot(railVx, railVy), fwd, int(o.reverse), int(g_sh.railAir), accel, brake,
									Wrap(g_railBearing[idx] - heading) * 57.29578, g_railAimJump[idx], g_railYawCmd[idx]);
								const ULONGLONG age = g_sh.lastStepAt ? tNow - g_sh.lastStepAt : 99999;
								if (age > 300) logger::Write("OBS system=drive.wheel pilot=%d event=no_rail_step step_age_ms=%llu active=%d rail_on=%d body=%08X instance=%p motion=%p",
									idx, (unsigned long long)age, int(g_sh.active), int(g_sh.railOn), g_sh.bodyId.load(), reinterpret_cast<void*>(g_sh.instance.load()), reinterpret_cast<void*>(g_sh.railMotion.load()));
							}
						}
						if (rail) {
							accel = 0.0; brake = 0.0; pedalDemand = 0.0;  // the wheels roll freely; the rail moves the car (BO-1: a boost stays only on tyres, below)
							// The wheels point where the rail turns, so the tyres help the turn instead of dragging against it every step.
							// The wheel angle a car of this wheelbase needs for the rail's turn rate at this speed (bicycle model), so the tyres
							// agree with the turn: at 22 m/s a turn needs a few degrees, not a full lock (19:37:02: 0.87 lock at 21.8 m/s rolled it).
							{
								const double L = g_sh.carWheelbase.load() > 0.5f ? double(g_sh.carWheelbase.load()) : 3.0;
								const double lock = g_sh.carLock.load() > 0.05f ? double(g_sh.carLock.load()) : 0.6;
								const double wheel = std::atan(L * -g_railYawCmd[idx] / std::max(2.0, std::fabs(fwd)));  // spin + = left, steer + = right
								steer = std::clamp(wheel / lock, -1.0, 1.0);
							}
							// TY-1 tyre mode: the rail's wanted speed becomes the game's own pedals (railtyres.h); the wheel angle above stays.
							// Backing up, the same yaw rate needs the opposite wheel. The rail step only measures (Shared::railTyres).
							const bool tyres = railTyres;
							if (!tyres) fwdBoost = false;  // BO-1: with velocity writes there is no thrust shortfall and the native boost would fight them
							if (tyres) {
								static railtyres::State tyreState[kPilots]; static ULONGLONG tyreAt[kPilots] = {}, tyreLog[kPilots] = {};
								// TY-3: one signed pedal for both directions (railtyres::StepSigned); want is signed along the nose (- = back up).
								const double wantT = (o.reverse ? -1.0 : 1.0) * std::hypot(railVx, railVy);
								// Backing up (or about to), the same yaw rate needs the opposite wheel.
								if (fwd < -0.3 || (o.reverse && fwd < 0.3)) steer = -steer;
								if (tyreAt[idx] && tNow - tyreAt[idx] > 300) {  // SP-1: a gap keeps each direction's remembered push (faded), not a wipe
									const auto kept = tyreState[idx];
									const double gapS = double(tNow - tyreAt[idx]) / 1000.0;  // as a double first: negating the unsigned gap wrapped and made exp() inf (NaN pedal)
									const double fade = std::exp(-gapS / railtyres::rules::heldFade);
									tyreState[idx] = {};
									tyreState[idx].fwdHeld = std::max(kept.fwdHeld, std::max(0.0, kept.i)) * fade;
									tyreState[idx].revHeld = std::min(kept.revHeld, std::min(0.0, kept.i)) * fade;
								}
								const double dtT = tyreAt[idx] ? (tNow - tyreAt[idx]) / 1000.0 : 0.0; tyreAt[idx] = tNow;
								const double roadGrade = idx >= 0 && idx <= prefetch::kSlotPilotCount ? double(g_roadGrade[idx].load(std::memory_order_relaxed)) : 0.0;
								const auto po = railtyres::StepSigned(tyreState[idx], wantT, fwd, std::fabs(steer), dtT, roadGrade);
								accel = po.accel; brake = po.brake; pedalDemand = po.demand;
								if (tNow - tyreLog[idx] >= 500) { tyreLog[idx] = tNow;
									logger::Write("OBS system=traffic.tyres pilot=%d event=pedals want=%.2f fwd=%.2f pedal=%+.2f accel=%.2f brake=%.2f held=%+.2f steer=%+.2f why=%s reverse=%d",
										idx, wantT, fwd, po.pedal, accel, brake, tyreState[idx].i, steer, po.why, int(o.reverse)); }
							}
							// Wake the body: stopped long enough, the game puts it to sleep and no physics step (so no rail step) runs again
							// (2026-11-05 19:07: no rail step for 27 s). A light throttle wakes it when the rail wants to move.
							if (std::hypot(railVx, railVy) > 0.5 && g_sh.lastStepAt && tNow - g_sh.lastStepAt > 300) {
								if (tyres && o.reverse) brake = std::max(brake, 0.3);  // TY-3: backing up on the tyres wakes it with the reverse pedal
								else accel = std::max(accel, 0.3);  // TY-1: tyre mode may already send more
								static ULONGLONG wakeLog[kPilots] = {};
								if (tNow - wakeLog[idx] > 2000) { wakeLog[idx] = tNow; logger::Write("OBS system=traffic.rail pilot=%d event=wake step_age_ms=%llu reason=body_asleep_rail_wants_to_move", idx, (unsigned long long)(tNow - g_sh.lastStepAt)); }
								// WK-1: and the game's own body wake (the throttle alone left the body asleep 24 s), at most every 250 ms.
								static ULONGLONG bodyWakeAt[kPilots] = {}, bodyWakeLog[kPilots] = {};
								if (tNow - bodyWakeAt[idx] >= 250) { bodyWakeAt[idx] = tNow;
									const auto inst = g_sh.instance.load(std::memory_order_relaxed);
									int before = -1; bool queued = false;
									if (routesurvey::OnMainThread()) before = WakeBody(inst);
									else if (inst) { g_wakeQueue[idx].store(inst, std::memory_order_relaxed); queued = true; }
									if (tNow - bodyWakeLog[idx] >= 1000) { bodyWakeLog[idx] = tNow;
										logger::Write("OBS system=traffic.rail pilot=%d event=wake_body state_before=%d step_age_ms=%llu reason=%s", idx, before, (unsigned long long)(tNow - g_sh.lastStepAt),
											queued ? "queued_for_main_thread" : before < 0 ? "not_done_no_valid_body_or_not_main_thread" : before == 1 ? "already_active_so_not_asleep" : "activated_as_the_component_update_does"); } }
							}
							// Jump: the game's own vertical boost, your jump key during Cruise (Cruise keeps going).
							const bool jumpKey = idx == 0 && keysOn && bindings::Held(bindings::Action::VerticalBoost, VK_SPACE);
							vert = jumpKey;
							static bool jumpLogged[kPilots] = {};
							if (vert != jumpLogged[idx]) { jumpLogged[idx] = vert; if (vert) logger::Observe("traffic.rail", 0, 0x4000ull + idx, "event=jump pilot=%d source=jump_key speed=%.1f", idx, fwd); }
						}
					}
					if (manual) {  // passenger control: your keys on the game's own wheels, after the plan's step 7 (a held command, as combat's)
						const auto& mc = g_cmds[idx].c;
						if (mc.set & Command::kSteer) steer = std::clamp(mc.steer, -1.0, 1.0);
						if (mc.set & Command::kThrottle) { accel = std::max(0.0, std::clamp(mc.throttle, -1.0, 1.0)); brake = mc.throttle < 0 ? std::min(1.0, -mc.throttle) : 0.0; }
						if ((mc.set & Command::kBrake) && mc.brake) { accel = 0.0; brake = 1.0; }
						vert = (mc.set & Command::kVertical) && mc.vertical > 0.5;
						fwdBoost = (mc.set & Command::kBoost) && mc.boost;
					}
					if (!manual) {  // ST-1 (wheelgrip.h SmoothSteer): the autopilot's steering capped to what the tyres carry at this speed, and rate limited
						static wheelgrip::Steer steerState[kPilots]; static double steerLogAt[kPilots] = {};
						const auto so = wheelgrip::SmoothSteer(steerState[idx], steer, spD, double(g_sh.carWheelbase.load()), double(g_sh.carLock.load()),
							wheelgrip::PlanGrip(g.gear, g_gripSet), double(tNow) / 1000.0);  // GT-1: the gear's grip
						if ((so.capped || so.rateLimited) && (double(tNow) / 1000.0 - steerLogAt[idx] >= 1.0)) { steerLogAt[idx] = double(tNow) / 1000.0;
							logger::Write("OBS system=vehicle.steer pilot=%d event=smoothed wanted=%+.2f sent=%+.2f cap=%.2f capped=%d rate_limited=%d speed=%.1f gear=%d reason=steering_within_what_the_tyres_carry_and_turned_gradually",
								idx, so.wanted, so.steer, so.cap, int(so.capped), int(so.rateLimited), spD, g.gear); }
						steer = so.steer;
					}
					if (railActive) {
						std::lock_guard lk(g_sh.lock);
						g_sh.railVx = float(railVx); g_sh.railVy = float(railVy); g_sh.railYaw = float(railYaw);
						g_sh.railAim = float(railAimOut); g_sh.railMaxYaw = float(std::min(1.6, 0.4 + std::fabs(wheelFwd) / 4.0));
						g_sh.railAimValid = std::isfinite(railAimOut);
						if (idx >= 0 && idx < int(sizeof g_railWall / sizeof g_railWall[0]) && tNow - g_railWallAt[idx] < 1500) {
							g_sh.railWall = g_railWall[idx];
						} else {
							g_sh.railWall.valid = false;
						}
						g_sh.railTyres = railTyres;
						g_sh.railUntil = tNow + 800; g_sh.railOn = true;
					}
					const bool driveBoost = fwdBoost || (vert && accel > 0.4);
					if (!manual && idx >= 0 && idx < kPilots) { g_pedalDemand[idx].store(float(pedalDemand), std::memory_order_relaxed); g_pedalAt[idx].store(tNow, std::memory_order_relaxed); }  // BO-1
					g.drvSteer = steer;
                    g_driveOut[idx].PublishAdmitted(commandVehicle,
                        {float(steer*g.steerSign),float(accel),float(brake),driveBoost,vert},wheelAdmission);
					if (tNow - g.drvLog >= 2000) {
						g.drvLog = tNow;
						logger::Write("%s: drive: speed %.1f (want %.0f), steer %+.2f accel %.2f brake %.2f boost %d up %d; ahead %d need %.1f m, left %d right %d; %.1f m above ground",
							wc, spD, tgt, steer * g.steerSign, accel, brake, driveBoost ? 1 : 0, vert ? 1 : 0, g.lastF, g.lastNeed, g.lastL, g.lastR, above);
					}
					AskSamples(idx, pos, heading, std::sin(heading) * spD, std::cos(heading) * spD);
					AskCorridors(idx, pos, apBearing, std::max(spD, 10.0));
					return;
				}
			}

			// ---- Lift-off: switched on near the ground, climb to a short hover height first ----
			if (g.liftoff && groundOk) {
				if (above >= kLiftoffHeight - 0.5 || above > kLiftoffBelow + 5.0) {
					g.liftoff = false;
					if (above < kLiftoffBelow + 5.0) logger::Write("%s: lifted off (%.1f m)", wf, above);
				}
			}

			// ---- Elevation assist reference and the height hold ----
			double refGround = groundOk ? g.ground : g.baseZ;
			const bool floorOn = GetTickCount64() < g.floorUntil;
			if (floorOn) refGround = std::max(refGround, g.floorZ - kFloorClear + 0.5);  // ride over the floor the rays miss
			const bool frontOk = g.asked[kFront].valid && std::fabs(g.asked[kFront].h - pos.z) > kSelfHitBand && g.asked[kFront].h < pos.z + kCastAbove &&
				!IsWallSample(g, g.asked[kFront].h, g.asked[kFront].dist);  // a wall 6 m ahead is no ground to hold above (the wall "magnet")
			if (frontOk) refGround = std::max(refGround, g.asked[kFront].h);
			if (g.asked[kAhead0].valid && sp > 2.0 && g.asked[kAhead0].h < pos.z - 0.5) refGround = std::max(refGround, g.asked[kAhead0].h);
			const double aboveRef = pos.z - refGround;
			const bool level = std::fabs(pitchAim) < 0.08 && std::fabs(bodyPitch) < 0.08 && lift == 0.0;
			if (g.autopilot && g.avoid != 1) {
				{
					// Just over the highest ground in the corridor ahead (cast along the way it is going).
					const auto& nv=PilotNavOf(idx);
                    double want=integrationlogic::Clearance(int(nv.height),nv.minClearance,g_sh.hull.height,kCallSkim)+(g.autopilot?EnvClimb():0.0);  // night / bad weather: a little higher
                    if(g.groundHug) {  // "driving": just over the ground; the corridor below still lifts it over what a jump clears
                        want=g_sh.hull.height*0.5+0.4;
                        for(unsigned k=0;g_vsnapOk&&k<g_vsnap.count&&k<world::kMaxVehicles;++k)if(g_vsnap.v[k].ref==vehicle)
                            want=std::max(0.3,double(g_vsnap.v[k].half[2])-(g_vsnap.v[k].center[2]-g_vsnap.v[k].pos[2])+0.4);
                    }
					for (int k = 0; k < 3; ++k) {
						const auto& q = g.corr[k];
						if (q.valid && !IsWallSample(g, q.h, q.d)) want = std::max(want, q.h + kTerrainClear - refGround);  // walls: the warning, not a climb
					}
					want = std::min(want, kAvoidMaxClimb);
                    // Lane height rules while the lane driver steers. 2026-11-01 fix: a hovering lane
                    // vehicle keeps the terrain lift above (walls excluded, so routes through
                    // structures stay allowed); during an avoidance fallback the lane steps aside.
                    const bool laneHeight=routeguidance::Get(idx,GetTickCount64())!=nullptr;
                    if (laneHeight) {
                        const double laneWant=std::max(0.15,g_laneDriver[idx].height-refGround);
                        want=routeguidance::Rules(idx).mode!=travelrules::Legacy?laneWant:g.adOut.mode==drive::Mode::Hover?std::max(want,laneWant):laneWant;
                        g.cruiseExtra=0;
                    }
					const double goalZ = laneHeight ? g_laneDriver[idx].height : goal.z;
					const bool haveGoalZ = !std::isnan(goalZ);
					if (haveGoalZ) want = std::max(want, std::min(goalZ - refGround, kGoalMaxAbove));
					const double sink = haveGoalZ ? kGoalSink : kCruiseSink;
					g.cruiseHold = want >= g.cruiseHold ? want : std::max(want, g.cruiseHold - sink * dt);
					g.hold = g.landing?std::max(0.15,g.landGround+g.landHeight-refGround):g.cruiseHold+g.cruiseExtra;
				}
				g.holding = true;
				g.liftoff = false;
			}
			else if (g.liftoff && groundOk) {
				g.hold = kLiftoffHeight;
				g.holding = true;
				refGround = g.ground;
			}
			else if (level && !g.holding) {
				g.hold = std::max(kMinClearance, aboveRef + v[2] / kVzResponse);  // where the current climb or fall eases out
				g.holding = true;
			}
			else if (!level) {
				g.holding = false;
			}
#ifndef FVO_RELEASE
			if (stockFloat) {  // the commanded absolute height replaces every ground-relative rule above
				g.hold = g_stockFloat[idx].z - refGround; g.holding = true; g.liftoff = false;
			}
#endif

			// Settling onto the ground ends the hover (never a lab float: its height is commanded, not the ground's).
			if (groundOk && !g.liftoff && !stockFloat && above < kLandHeight && v[2] <= 0.1 && !thrust) {  // never while climbing out
				if (!g.landedSince) g.landedSince = GetTickCount64();
				if (GetTickCount64() - g.landedSince >= kLandMs) {
					Stop(idx, "landed");
					return;
				}
			}
			else {
				g.landedSince = 0;
			}

			AskSamples(idx, pos, bodyHeading, v[0], v[1]);
			// ---- Autopilot channels: its own reverse / strafe, then any held command (PilotCommand) on top ----
			double strafe = 0, vzCmd = 0, rollAim = 0;
			bool vzOn = false, rollAimOn = false;
			if (g.autopilot) {
				const ULONGLONG nowMs = GetTickCount64();
				if (nowMs < g.reverseUntil) {  // the energy plan backs up
					thrustAcc = -kReverseThrottle * kThrust * kGearThrust[2];
					thrust = true;
					braking = false;
					pitchAim = 0.0;
				}
				rollAim = std::clamp(kApBankPerYaw * yawWant * std::clamp(sp / 15.0, 0.0, 1.0), -kApBankMax, kApBankMax);
				rollAimOn = true;
				if (idx >= 0 && idx < int(sizeof g_planSplit / sizeof g_planSplit[0]) && g_planSplit[idx].at) {
					// CB-4: a combat line with a facing while moving. The nose already turns to the facing (apBearing); the plan's travel goes to
					// thrust along the nose and the strafe channel. The plan decided where and how fast; this only follows it.
					if (nowMs >= g_planSplit[idx].at && nowMs - g_planSplit[idx].at < 300) {
						const auto t = planhover::SplitTravel(g_planSplit[idx].travel, g_planSplit[idx].speed, bodyHeading, v[0], v[1], std::max(1.0, top));
						thrustAcc = t.brake ? 0.0 : t.throttle * kThrust * kGearThrust[std::clamp(apGear, 1, 4)];
						thrust = thrustAcc != 0.0; braking = t.brake; strafe = t.strafe;
					}
					else if (!(nowMs >= g_planSplit[idx].at && nowMs - g_planSplit[idx].at < 300)) {
						logger::Write("OBS system=plan.combat vehicle=00000000 pilot=%d event=split_off reason=no_fresh_combat_plan", idx);
						g_planSplit[idx].at = 0;
					}
				}
				const auto& hc = g_cmds[idx];
				if (hc.c.set && nowMs < hc.until) {
					const auto& c = hc.c;
                    constexpr unsigned movement=Command::kThrottle|Command::kYaw|Command::kStrafe|Command::kVertical|Command::kGear;
                    const bool requestedMotion=(c.set&movement)!=0;
                    const integrationlogic::MotionFence fence{false,(respondStatic&&g.wallAhead),(respondPeople&&g.personAhead),(respondVehicles&&g.vehWarn)&&g.vehYield&&g.vehGap<5.0&&TrafficInWay(g,sp,v,bodyHeading),top,sp};
                    // Scripted: no rollout check against the planner's field (it vetoed every move out of roofed bays);
                    // only a vehicle in the direction the script drives stops the thrust (walls/people: the final envelope).
                    if (scripted) {
                        const double dir = c.throttle < 0 ? bodyHeading + 3.14159265358979 : bodyHeading;
                        scriptTraffic = (c.set & Command::kThrottle) && std::fabs(c.throttle) > 0.01 && (respondVehicles&&g.vehWarn) && g.vehGap < 5.0 &&
                            std::cos(Wrap(g.vehBearing - dir)) > 0.5;
                        braking = false; strafe = 0;  // the planner's avoidance braking and side-steps do not apply
                    }
                    if (manual) { braking = false; strafe = 0; }  // passenger control: your hands, as at your own wheel
                    const bool allowMovement=scripted||manual||!requestedMotion||(!(trafficPolicy&&trafficcoordinator::Strict(*trafficPolicy))&&integrationlogic::MovementAllowed(fence,true));
                    const unsigned allowed=c.set&((allowMovement?movement:0)|Command::kPitch|Command::kRoll|Command::kBrake);
                    const int cg = (allowed & Command::kGear) ? std::clamp(c.gear, 1, 4) : apGear;
                    if (scripted) {  // the script picks the gear: the planner's path caps do not limit it
                        top = kAiGearTop[cg];
                        const auto& nv = PilotNavOf(idx); if (nv.speedLimit > 0) top = std::min(top, double(nv.speedLimit));
                        g.cap = groundOk ? top : std::min(top, kSpeedCapBlind);
                    }
					if (allowed & Command::kThrottle) {
						thrustAcc = braking?0.0:std::clamp(c.throttle,-1.0,1.0)*kThrust*kGearThrust[cg];
						thrust = thrustAcc != 0.0;
						// Existing planner braking remains authoritative.
					}
					if (allowed & Command::kBrake) {
						braking = braking || c.brake;
						if (c.brake) { thrustAcc = 0; thrust = false; }
					}
					if (allowed & Command::kStrafe) strafe = std::clamp(c.strafe, -1.0, 1.0);
					if (allowed & Command::kVertical) { vzOn = true; vzCmd = c.vertical; }
					if (allowed & Command::kYaw) { yawWant = c.yawRate; haveCam = false; }
					if (allowed & Command::kPitch) { pitchAim = std::clamp(c.pitch, -kPitchMax, kPitchMax); haveAim = true; }
					if (allowed & Command::kRoll) { rollAim = std::clamp(c.roll, -kRollMax, kRollMax); rollAimOn = true; }
					if (allowed & Command::kGear) g.gear = cg;
				}
			}
            if(g.landing) {
                const double error=pos.z-(g.landGround+g.landHeight);
                vzOn=true;vzCmd=std::clamp(-error,-1.0,0.5);thrustAcc=0;thrust=false;braking=true;strafe=0;
                const double speed=std::hypot(v[0],v[1]);
                if(std::fabs(error)<0.2&&speed<0.3&&std::fabs(v[2])<0.25&&environment::BoardingSite(vehicle).verdict==environment::Verdict::Safe) {
                    if(idx==0&&g_remote){g_remoteDone=vcontract::AssignedTo(g_roverId);g_remote=false;}
                    Stop(idx,"arrived; supported touchdown");return;
                }
            }
            // The final envelope for a script's direct commands (the energy plan does not vet those): a wall, a person or a vehicle in the
            // direction the script drives stops the thrust. A car the energy plan drives follows the plan (its World holds them).
            if(g.autopilot&&scripted&&((respondStatic&&g.wallAhead)||(respondPeople&&g.personAhead)||scriptTraffic)) {
                thrustAcc=std::min(thrustAcc,0.0);thrust=thrustAcc!=0;braking=true;
            }
            if(g.autopilot&&sp>top+0.5){thrustAcc=std::min(thrustAcc,0.0);thrust=thrustAcc!=0;braking=true;}
            if(g.ceilValid&&pos.z>=g.ceilZ-kCeilClear-g_sh.hull.height&&vzOn&&vzCmd>0)vzCmd=0;
			// Free roll (the settings menu) flies only your own hover, never the autopilot or NPC pilots; the bank test likewise.
			const bool freeRoll = idx == 0 && !g.autopilot && features::On(features::Id::FreeRoll);
			const bool flightAssist = features::On(features::Id::FlightAssist);
			// Physical flight: the player's own hover (Physical VTOL), or any autopilot / NPC pilot (Physical VTOL for AI pilots).
			const bool manualPhysical=idx==0&&!g.autopilot&&features::On(features::Id::PhysicalVtol);
			const bool aiPhysical=g.autopilot&&features::On(features::Id::PhysicalAi);
			const bool physical=g_physicalHooksReady&&(manualPhysical||aiPhysical);
			g_sh.physNeedsDriver.store(manualPhysical,std::memory_order_relaxed);
			g_sh.physOn.store(physical,std::memory_order_release);
			double bankTarget = 0;
			const bool bankTest = idx == 0 && BankTestFrame(vehicle, g.startedAt, keysOn, g.autopilot, above, freeRoll, firstPerson, bankTarget);
			if (bankTest) { rollAimOn = true; rollAim = bankTarget; }
			AskCorridors(idx, pos, (g.autopilot && apSteer) ? apBearing : (sp > 2.0 ? std::atan2(v[0], v[1]) : bodyHeading), std::max(sp, g.autopilot ? 10.0 : 0.0));
            double fieldClearance=-1;  // (the field's final gate is gone: field boxes are in the energy plan's scene, D12)
			bool attitudeActive=false;
			double cameraPitch=0,cameraRoll=0;
			// FC-1 traces (taken under the lock, written after it)
			struct Track { bool active, physical; unsigned steps; double rateErr, drift, sidePush, collective, trim, vzErr; unsigned angCapped, torqueCapped, sideCapped; double cap; } track{};
			bool haveTrack = false, rowPhysical = false; double rowHullLateral = 0;
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.px = pos.x; g_sh.py = pos.y; g_sh.pz = pos.z;
				g_sh.thrustAcc = thrustAcc;
				g_sh.pitchAim = pitchAim;
				g_sh.haveAim = haveAim;
				g_sh.thrusting = thrust;
				g_sh.braking = braking;
				const bool pilotChanged = g_sh.autopilot != g.autopilot;
				g_sh.autopilot = g.autopilot;
				g_sh.yawWant = yawWant;
				g_sh.view = view;
				g_sh.haveView = haveCam;
				g_sh.pitchIn = pitchIn;
				g_sh.rollIn = rollIn;
				g_sh.lift = lift;
				g_sh.strafe = strafe;
				g_sh.vzOn = vzOn;
				g_sh.vz = vzCmd;
                g_sh.fieldClearance=fieldClearance;
				g_sh.rollAimOn = rollAimOn;
				g_sh.rollAim = rollAim;
				g_sh.freeRoll = freeRoll;
				if (g_sh.physical!=physical||(physical&&pilotChanged)) {
					g_sh.physicalState={}; g_sh.physicalFault=false; g_sh.haveLastW=false; g_sh.physWiped=0;
					if (physical||g_sh.physical) logger::Write("%s: physical flight %s (%s)",Who(idx),physical?"on":"off",g.autopilot?"AI pilot":"manual");
				}
				g_sh.physical = physical;
				// Physical flight plans with the chassis' real grip and brake, both models with the flight gear's turn share (FC-1,
				// hulllogic::Flight): every planner reads g_sh.hull, the step reads g_sh.row.
				g_sh.row = flightRow;
				g_sh.hull = hulllogic::Flight(g_sh.hullBase, physical && !g_sh.physicalFault, flightRow.lateralScale);
				rowHullLateral = g_sh.hull.lateral; rowPhysical = physical && !g_sh.physicalFault;
				if (GetTickCount64() - g.trackAt >= 1000) {
					g.trackAt = GetTickCount64();
					track = { g_sh.active, g_sh.trkPhysical, g_sh.trkSteps, g_sh.trkRateErr, g_sh.trkDrift, g_sh.trkSidePush, g_sh.trkCollective, g_sh.trkTrim, g_sh.trkVzErr,
						g_sh.trkAngCapped, g_sh.trkTorqueCapped, g_sh.trkSideCapped, g_sh.cap };
					g_sh.trkSteps = g_sh.trkAngCapped = g_sh.trkTorqueCapped = g_sh.trkSideCapped = 0;
					g_sh.trkRateErr = g_sh.trkDrift = g_sh.trkSidePush = 0;
					haveTrack = true;
				}
				g_sh.assist = flightAssist;
				g_sh.bankTest = bankTest;
				g_sh.holding = g.holding;
				if (g.ceilValid) {
					// Never up into a ceiling: the hold (metres over refGround) stays under it; the climb-outs give way to it too.
					const double maxHold = g.ceilZ - kCeilClear - kVehicleTop - refGround;
					if (g.hold > maxHold && !stockFloat) {
						g.hold = std::max(0.8, maxHold);
						const ULONGLONG cNow = GetTickCount64();
						if (logger::Verbose() && cNow - g.ceilLogAt >= 2000) {
							g.ceilLogAt = cNow;
							logger::Write("%s: ceiling %.1f m over the vehicle: height held %.1f m under it", wf, g.ceilZ - pos.z, kCeilClear);
						}
					}
				}
				g_sh.hold = g.hold;
				g_sh.refGround = refGround;
				g_sh.groundUnder = floorOn ? std::max(g.ground, g.floorZ - 0.5) : g.ground;
				g_sh.groundOk = groundOk && !stockFloat;  // a lab float has no ground reference: no floor-clearance push under the commanded height
				g_sh.airborne = groundOk && above > 1.5;
				g_sh.cap = g.cap;
				attitudeActive=physical&&!g_sh.physicalFault;  // the autopilot too (2026-10-30, the user: everything of manual VTOL, the camera included)
				cameraPitch=g_sh.bodyPitch; cameraRoll=g_sh.bodyRoll;
			}
			if (!idx) {
				vehiclecamera::Publish(attitudeActive?vehicle:0,cameraPitch,cameraRoll);
				vehiclecamera::LogStatus();
			}
			{   // FC-1: the row in use (on a gear, role or writer change) and the handling it gave (1 s, while the hover writes)
				const int writer = rowPhysical ? 1 : 0;
				if (g.rowGear != flightRow.gear || g.rowAi != flightRow.ai || g.rowWriter != writer) {
					g.rowGear = flightRow.gear; g.rowAi = flightRow.ai; g.rowWriter = writer;
					logger::Write("OBS system=vehicle.flightgear vehicle=%08X pilot=%d event=row gear=%d role=%s writer=%s rate_gain=%.2f angular_limit=%.2f side_gain=%.2f side_budget=%.2f side_dead=%.2f side_dead_per_speed=%.3f yaw_response=%.2f tilt_response=%.2f lateral_scale=%.3f hull_lateral=%.2f hull_yaw=%.3f top=%.0f source=%s reason=per_gear_flight_row",
						At<std::uint32_t>(vehicle, off::kFormId), idx, flightRow.gear, flightRow.ai ? "ai" : "player", writer ? "physical" : "standard", flightRow.rateGain, flightRow.angularLimit,
						flightRow.sideGain, flightRow.sideBudget, flightRow.sideDead0, flightRow.sideDeadPerSpeed, flightRow.yawResponse, flightRow.tiltResponse, flightRow.lateralScale,
						rowHullLateral, g_sh.hull.yaw, flightRow.top, g.autopilot ? "autopilot_fg1" : "player_gear_key");
				}
				if (haveTrack && track.active && track.steps)
					logger::Write("OBS system=vehicle.flightgear vehicle=%08X pilot=%d event=track gear=%d writer=%s steps=%u rate_err=%.3f drift=%.2f side_push=%.2f top_cap=%.1f speed=%.1f angular_capped=%u torque_capped=%u side_capped=%u collective=%.2f trim=%.2f vz_err=%.2f reason=flight_handling_per_gear",
						At<std::uint32_t>(vehicle, off::kFormId), idx, flightRow.gear, track.physical ? "physical" : "standard", track.steps, track.rateErr, track.drift, track.sidePush,
						track.cap, sp, track.angCapped, track.torqueCapped, track.sideCapped, track.physical ? track.collective : 0.0, track.physical ? track.trim : 0.0, track.physical ? track.vzErr : 0.0);
			}

			if (g.autopilot) {
				const double dist = std::hypot(goal.x-pos.x, goal.y-pos.y);
				const double targetZ = std::isfinite(goal.z) ? std::max(goal.z, refGround + kCallSkim + EnvClimb()) : pos.z;
				const double zError = pos.z - targetZ;
				const bool ready = groundOk && matched && (aheadLoaded || sp>3.0 || dist<=goal.stop+2.0);
				const bool arrived = !goal.cruise && AtDestination(vehicle, goal.x, goal.y, goal.z, goal.stop);
				const bool rerouting = GetTickCount64() < g.reverseUntil;  // the energy plan backs up
				const bool blocked = sp < 1.0 && g.adOut.why && std::strcmp(g.adOut.why, "energy plan not ready") == 0;
				g.trip = {true, triplogic::Status(ready, arrived, dist <= goal.stop+2.0, zError, rerouting, blocked),
					goal.x, goal.y, dist, std::hypot(sp,v[2]), zError, GetTickCount64()};
			} else g.trip = {};

			const ULONGLONG ms = GetTickCount64();
			if (ms - g.lastLog >= 2000) {
				g.lastLog = ms;
				unsigned steps;
				{
					std::lock_guard lk(g_sh.lock);
					steps = g_sh.steps;
					g_sh.matched = false;
					g_sh.steps = 0;
				}
				if (logger::Verbose()) logger::Write("%s: pos (%.1f, %.1f, %.1f) body %s, %u steps/2 s; speed %.1f (cap %.0f, ahead %s [%d%d%d], rays [%d%d%d], cells [%d%d%d]) vz %.1f; %.1f m above %s, %s %.1f, ref ground %.1f (front %s); "
					"pitch %.2f (aim %.2f: camera %.2f, rest %.2f) roll %.2f (in %.0f), yaw spin %.2f; gear %d boost %d; heading %.2f, camera %s (%s) %.2f gap %.2f -> turn %.2f; lift %.0f; thrust %d assist %d; gravity %.2f; %d bad velocities replaced; %u ground samples rejected",
					wf, pos.x, pos.y, pos.z, matched ? "matched" : "NOT matched", steps, sp, g.cap, aheadLoaded ? "loaded" : "NOT loaded",
					near0 ? 1 : 0, near1 ? 1 : 0, far2 ? 1 : 0,
					ray0 ? 1 : 0, ray1 ? 1 : 0, ray2 ? 1 : 0, cell0 ? 1 : 0, cell1 ? 1 : 0, cell2 ? 1 : 0,
					v[2], above, groundOk ? "ground" : "(no ground)",
					g.holding ? "holding" : "free", g.hold, refGround, frontOk ? "used" : "ignored", bodyPitch, pitchAim, camPitch, g_camRest.pitch, bodyRoll, rollIn, wYaw, g.gear, boost ? 1 : 0,
					bodyHeading, haveCam ? "ok" : "MISSING", g_camPick.pick ? "row" : "column", view, gap, camYaw, lift, thrust ? 1 : 0, g.avoid, gEst, fixedVel, g.rejected);
				if (!idx) telemetry::Record("rover_flight", "\"pos\":[%.1f,%.1f,%.1f],\"speed\":%.1f,\"cap\":%.0f,\"ahead_ray\":[%d,%d,%d],\"ahead_cell\":[%d,%d,%d],\"vz\":%.1f,\"above\":%.1f,\"pitch\":%.2f,\"roll\":%.2f,\"gap\":%.3f,\"g\":%.2f",
					pos.x, pos.y, pos.z, sp, g.cap, ray0 ? 1 : 0, ray1 ? 1 : 0, ray2 ? 1 : 0,
					cell0 ? 1 : 0, cell1 ? 1 : 0, cell2 ? 1 : 0, v[2], above, bodyPitch, bodyRoll, gap, gEst);
			}
			if (ms - g.lastHud >= 1000) {
				g.lastHud = ms;
				// (not in the first 1.5 s: every start read "rays [000]" before the first samples came back)
				const bool loadingShown = !aheadLoaded && ms - g.startedAt > 1500;
				if (!idx) notify::Log("HOVER %.0f m/s%s | %.1f m | gear %d | assist %d | pitch %+.0f deg", sp, loadingShown ? " (slowed: ground ahead loading)" : "", above,
					g.gear, g.avoid, bodyPitch * 57.2958);
			}
		}
	}

	namespace
	{
		// ---- Cover / guard combat of the player's own EMPTY vehicle (coverfight.h) ----
		struct CoverFight
		{
			std::uint32_t foe = 0;               // the enemy (an actor); its vehicle is aimed at when it drives one
			ULONGLONG lastSeen = 0, pickAt = 0, posAt = 0, startedAt = 0;
			ULONGLONG targetAt = 0, blockedSince = 0;
			double anchorX=0,anchorY=0;
			double spotX = 0, spotY = 0, spotZ = std::numeric_limits<double>::quiet_NaN();
			bool haveSpot = false, fireSent = false;
			game::Vec3 lastFoe{};
			ULONGLONG lastFoeAt = 0;
			coverfight::Burst burst;
			unsigned bursts = 0;
		} g_cf;

		bool CoverFighting() { return coverfight::Fighting(g_cf.foe != 0, g_cf.lastSeen, GetTickCount64()); }

		void EndCoverFight(const char* why)
		{
			if (g_cf.fireSent && g_roverId) npcvehicles::FireVehicleWeapon(g_roverId, false, 0, 0, 0);
			if (g_cf.foe) logger::Write("Rover cover: fight over (%s)", why);
			g_cf = CoverFight{};
		}

		// Main thread, every PlayerTick on foot: enemy awareness, the defend spot and the weapon. The goal itself is set in the goal block.
		void CoverFightTick(std::uintptr_t player, bool driven, bool crewDrives)
		{
			const ULONGLONG now = GetTickCount64();
			const bool guard = g_guard && g.holdId == g_roverId && g_roverId;
			const auto rover = Rover();
			const auto pcell = At<std::uintptr_t>(player, off::kRefParentCell);
			const bool outdoors = pcell && At<std::uintptr_t>(pcell, off::kCellWorldspace);
			if (!rover || !outdoors || !coverfight::Allowed(driven, crewDrives, g_follow, guard) || IsDisabled(g_roverId) ||
				Worldspace(rover) != Worldspace(player)) {
				if (g_cf.foe || g_cf.fireSent) EndCoverFight(driven ? "you got in" : crewDrives ? "a follower drives" : !rover ? "no vehicle" : "cover ended");
				return;
			}
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const double cx = guard ? g.holdX : pp.x, cy = guard ? g.holdY : pp.y, cz = pp.z;
			if (now - g_cf.pickAt >= coverfight::kPickMs) {
				g_cf.pickAt = now;
				bool inVeh = false;
				int count = 0;
				std::uintptr_t foe = npcvehicles::EnemyNear(player, cx, cy, cz, coverfight::kRange, inVeh, count);
				const auto current=RefById(g_cf.foe);
				const auto currentPos=current?At<game::Vec3>(current,off::kRefPosition):game::Vec3{};
				const bool currentValid=current&&!npcvehicles::IsDead(current)&&npcvehicles::HostileToPlayer(current)&&
					Worldspace(current)==Worldspace(rover)&&std::hypot(currentPos.x-cx,currentPos.y-cy,currentPos.z-cz)<coverfight::kRange*coverfight::kKeep;
				const bool higher=foe&&EngineFn<std::uintptr_t(*)(std::uintptr_t)>(addr::kPlayerVehicle)(foe)&&
					current&&!EngineFn<std::uintptr_t(*)(std::uintptr_t)>(addr::kPlayerVehicle)(current);
				if(coverfight::KeepTarget(currentValid,higher,now-g_cf.targetAt))foe=current;
				if(const auto focus=RefById(npcvehicles::CombatFocus()))foe=focus;
				const std::uint32_t id = foe ? At<std::uint32_t>(foe, off::kFormId) : 0;
				if (id) {
					if (id != g_cf.foe) {
						logger::Write("Rover cover: enemy %08X (%s, %.0f m from the %s, %d around)", id, inVeh ? "in a vehicle" : "on foot",
							std::hypot(At<game::Vec3>(foe, off::kRefPosition).x - cx, At<game::Vec3>(foe, off::kRefPosition).y - cy), guard ? "guard point" : "player", count);
						if (!g_cf.foe) g_cf.startedAt = now;
						g_cf.foe = id;
						g_cf.targetAt = now;
						g_cf.burst = {};
						g_cf.lastFoeAt = 0;
					}
					g_cf.lastSeen = now;
				}
			}
			if (!CoverFighting()) {
				if (g_cf.foe) EndCoverFight("no enemy left");
				return;
			}
			const auto foeRef = RefById(g_cf.foe);
			if (!foeRef) { EndCoverFight("enemy unloaded"); return; }
			const auto foeVeh = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(foeRef);
			const auto aimRef = foeVeh ? foeVeh : foeRef;
			const auto ep = At<game::Vec3>(aimRef, off::kRefPosition);
			// The defend spot: Cover shields the player (between them and the threat), Guard stays at its point; CombatPosition adds cover and a firing lane.
			const bool movedAnchor=g_cf.haveSpot&&std::hypot(cx-g_cf.anchorX,cy-g_cf.anchorY)>12.0;
			const auto lastHit=vehiclehits::LastHitMs(g_roverId);
			const bool danger=lastHit&&now>=lastHit&&now-lastHit<coverfight::kJinkHitMs&&vehiclehits::HealthShare(g_roverId)<0.4f;
			const bool blocked=g_cf.blockedSince&&now-g_cf.blockedSince>=coverfight::kBlockedMs;
			if (now - g_cf.posAt >= coverfight::kPickMs && coverfight::Reposition(g_cf.haveSpot,g_cf.fireSent,movedAnchor,danger,
				now-g_cf.posAt,blocked?now-g_cf.blockedSince:0)) {
				g_cf.posAt = now;
				double x = 0, y = 0, z = std::numeric_limits<double>::quiet_NaN();
				const auto* cmb = vcontract::AssignedTo(g_roverId, vcontract::Class::Combat);
				if (guard) { x = g.holdX; y = g.holdY; }
				else if (cmb && profileclass::Is(cmb->name, "attack")) coverfight::AttackPoint(cx, cy, ep.x, ep.y, At<game::Vec3>(rover, off::kRefPosition).x, At<game::Vec3>(rover, off::kRefPosition).y, x, y);
				else coverfight::ShieldPoint(pp.x, pp.y, ep.x, ep.y, x, y);
				const float threat[3] = { ep.x, ep.y, ep.z + 1.2f };
				environment::CombatPosition(g_roverId, threat, x, y, z, true);  // false: the centre itself (no better mapped spot)
				g_cf.spotX = x; g_cf.spotY = y; g_cf.spotZ = std::numeric_limits<double>::quiet_NaN(); g_cf.haveSpot = true;  // skim the terrain there
				g_cf.anchorX=cx;g_cf.anchorY=cy;
			}
			// A parked vehicle lifts off to fight (follow parks it when the player stands still).
			if (!g.autopilot && !g_move.pending) {
				Start(rover);
				g.autopilot = true;
				g.autopilotSince = now;
				g.gear = std::max(g.gear, 2);
				logger::Write("Rover cover: lifting off to defend");
			}
			if (!g.active || g_hoverVehicle.load() != rover || !g.autopilot) return;
			// Facing and the weapon. Holding its spot the vehicle turns its nose on the threat (part of the defensive position); it fires only with
			// the nose on target, in range and with a clear lane (FireVehicleWeapon checks the lane; npcvehicles' fire gate refuses a driven vehicle).
			const auto vp = At<game::Vec3>(rover, off::kRefPosition);
			const auto vr = At<game::Vec3>(rover, off::kRefRotation);
			double vx = 0, vy = 0;
			if (g_cf.lastFoeAt && now > g_cf.lastFoeAt && now - g_cf.lastFoeAt < 1000) {
				const double dt = (now - g_cf.lastFoeAt) / 1000.0;
				vx = (ep.x - g_cf.lastFoe.x) / dt; vy = (ep.y - g_cf.lastFoe.y) / dt;
				if (std::hypot(vx, vy) > 150.0) vx = vy = 0;
			}
			g_cf.lastFoe = ep; g_cf.lastFoeAt = now;
			const double range = std::sqrt(std::pow(ep.x - vp.x, 2) + std::pow(ep.y - vp.y, 2) + std::pow(ep.z - vp.z, 2));
			const double t = vehiclehits::ShotTime(g_roverId,range); // native projectile travel
			const double ax = ep.x + vx * t, ay = ep.y + vy * t, az = ep.z + (foeVeh ? 1.0 : 1.2);
			const bool atSpot = g_cf.haveSpot && std::hypot(g_cf.spotX - vp.x, g_cf.spotY - vp.y) < coverfight::kAtSpot;
			const vcontract::Profile* wp = vcontract::AssignedTo(g_roverId, vcontract::Class::Weapons);
			const auto weapons = profileclass::Effective(profileclass::WeaponsFor(wp ? wp->name : nullptr), false, false);
			if (atSpot && weapons != profileclass::Weapons::Never) {
				Command c;
				c.set = Command::kYaw | Command::kPitch;
				c.yawRate = std::clamp(1.6 * Wrap(std::atan2(ax - vp.x, ay - vp.y) - vr.z), -1.2, 1.2);
				c.pitch = std::clamp(std::atan2(az - vp.z, std::max(5.0, std::hypot(ax - vp.x, ay - vp.y))), -0.6, 0.6);
				// Break / jink while it is being hit (2026-10-21, the manoeuvre set: Cover keeps only the gun run and the break).
				if (danger) {
					c.set |= Command::kStrafe;
					c.strafe = ((now / coverfight::kJinkMs) % 2) ? 0.8 : -0.8;
				}
				c.holdMs = 300;
				PilotCommand(0, c);
			}
			const double yawErr = Wrap(std::atan2(ax - vp.x, ay - vp.y) - vr.z);
			const double pitchErr = std::atan2(az - vp.z, std::max(1.0, std::hypot(ax - vp.x, ay - vp.y))) + vr.x;  // rotation x: nose down +
			const float muzzle[]={vp.x+float(std::sin(vr.z)*4),vp.y+float(std::cos(vr.z)*4),vp.z+1.5f};
			const float target[]={float(ax),float(ay),float(az)};
			const bool clear=environment::ClearShot(g_roverId,g_cf.foe,foeVeh?At<std::uint32_t>(foeVeh,off::kFormId):0,muzzle,target);
			if(clear)g_cf.blockedSince=0;else if(!g_cf.blockedSince)g_cf.blockedSince=now;
			const bool can = clear && profileclass::MayFire(weapons, driven) && coverfight::OnTarget(yawErr, pitchErr, range,vehiclehits::BallisticsOf(g_roverId).range);
			const bool fire = g_cf.burst.Update(can, now);
			if (fire || g_cf.fireSent) {
				const bool sent = npcvehicles::FireVehicleWeapon(g_roverId, fire, ax, ay, az,g_cf.foe,foeVeh?At<std::uint32_t>(foeVeh,off::kFormId):0);
				if (fire && !g_cf.fireSent && g_cf.bursts++ % 10 == 0)
					logger::Write("Rover cover: weapon burst %u at %08X (%.0f m) -> %s", g_cf.bursts, g_cf.foe, range, sent ? "input sent" : "input FAILED");
				g_cf.fireSent = fire;
			}
		}
	}

	void PlayerTick()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !g_original) {
			FirstPersonLookLimits(false);
			g_altInput = {}; g_altDrop = false;
			hoverui::SetState(false, false, g.gear, false);
			g_driverVehicle.store(0, std::memory_order_relaxed);g_manualDriverId.store(0);
			return;
		}
		// The player died (aboard or not): the vehicle can no longer go on. Hover, Auto, follow and summon end and stay
		// off until a load brings the player back (the dead check is the actor's vtable +0x868, as for NPCs).
		static bool deadSaid = false;
		if (PlayerDead(player)) {
			FirstPersonLookLimits(false);
			g_altInput = {}; g_altDrop = false;
			if (!deadSaid) {
				deadSaid = true;
				logger::Write("Rover: you died%s; vehicle control off", g.active ? " (hover on: ended)" : "");
			}
			if (g.active) Stop("you died");
			g_autoOffWhy = "you died"; g.playerAuto = false;
			g.holdId = 0;
			g.routeHold=false;
			g_follow = false; g_guard = false;
			hoverui::SetState(false, false, g.gear, false);
			hoverui::SetTripStatus(false, "");
			hoverui::SetFoot(false, false, "", "", "", "", "", "");
			g_driverVehicle.store(0, std::memory_order_relaxed);g_manualDriverId.store(0);
			return;
		}
		deadSaid = false;
		const bool focused = Focused();
		ChooseKeys();
		ReadMoveKeys();
		hoverui::EditTick(focused);
		const bool h = focused && bindings::Held(bindings::Action::HoverToggle, g_keys.toggle);
		const bool pressed = focused && bindings::Triggered(bindings::Action::HoverToggle);
		g.keyDown = h;
		const auto driven = DrivenBy(player);
		FirstPersonLookLimits(driven != 0);
		g_boostRequest.store(driven && focused && !g.playerAuto && (bindings::Held(bindings::Action::Boost, VK_LSHIFT) || (g.active && g.wHold)));
		if(!driven)g_meterBoost.store(false);
		hoverui::SetState(driven != 0, g.active && (!g.autopilot || g.playerAuto), g.gear, g.wHold);
		const auto priorDriven=g_driverVehicle.exchange(driven,std::memory_order_relaxed);
        g_manualDriverId.store(driven&&!passenger::RidingVehicle()?At<std::uint32_t>(driven,off::kFormId):0);
		// Saved hover (flightsave.h): what a save made now records; only your own manual hover at the wheel.
		{
			const bool manual = driven && g.active && !g.autopilot && !passenger::RidingVehicle() && g_hoverVehicle.load() == driven;
			flightsave::Publish(manual ? At<std::uint32_t>(driven, off::kFormId) : 0, manual ? At<game::Vec3>(driven, off::kRefPosition).z : 0.f, g.gear);
		}
		if (driven && priorDriven != driven) {
			const auto id = At<std::uint32_t>(driven, off::kFormId);
			logger::Write("Vehicle health: player boarded %08X with %.3f / %.0f health; existing chassis record retained", id,
				vehiclehits::HealthShare(id) * vehiclehits::MaxHealth(), vehiclehits::MaxHealth());
		}
		if(priorDriven && priorDriven!=driven)EndTargetSession(priorDriven);
		if (priorDriven != driven) g_altInput = {};
        const altcontrol::Action alt{bindings::Triggered(bindings::Action::Gear),false};
        g_altDrop=false;
        if(bindings::Triggered(bindings::Action::FlightAssist)){
            const bool on=!features::On(features::Id::FlightAssist);features::Set(features::Id::FlightAssist,on);
            notify::Show(on?"Flight assist on":"Flight assist off");
        }
		if (alt.cycle) {
			const int before = g.gear;
			g.gear = altcontrol::NextGear(g.gear);
			if (g.active && !g.drive) { const auto fr = carcap::Flight(g.gear, false);  // FC-1: hovering, the gear names its flight row
			  notify::Log("Gear %d of 4  (%.0f m/s, flight: turn response x%.2f, drift damping %.1f/s, turn grip x%.2f)", g.gear, fr.top, fr.yawResponse / carcap::Flight(3, false).yawResponse, fr.sideGain, fr.lateralScale); }
			else { const auto pr = wheelgrip::ProfileOf(g.gear, g_gripSet);
			  if (pr.friction == 1.0 && pr.rear == 1.0 && pr.assist == 0.0 && pr.slipTarget == 0.0 && pr.weight == 1.0) notify::Log("Gear %d of 4  (%.0f m/s, the game's own handling)", g.gear, kGearTop[g.gear]);
			  else notify::Log("Gear %d of 4  (%.0f m/s, grip x%.2f, rear x%.2f, turning weight x%.2f)", g.gear, kGearTop[g.gear], pr.friction, pr.friction * pr.rear, pr.weight); }
			logger::Write("Rover: gear %d -> %d (Alt tap)", before, g.gear);
			if (g.active && !g.drive) logger::Write("OBS system=vehicle.gear vehicle=00000000 pilot=0 event=flight_shift from=%d to=%d top=%.0f reason=player_gear_key", before, g.gear, kGearTop[std::clamp(g.gear, 0, 4)]);
		}
		// E (VehicleExit) while the hover holds the vehicle up (2026-10-21 11:38 run: "Vehicle exit is obstructed" in a hovering
		// V-47; the REV-8 got out fine once its hover was off): the game's exit test finds no place for the player, so the
		// hover goes off, the vehicle settles, and then the game's own exit runs (its obstruction checks still decide).
		{
			static bool exitDown = false;
			static ULONGLONG settleSince = 0;
			// Wrecked under you (2026-10-21, the user: "when wrecked it should force eject player"): out at once, without setting
			// down first; the game's exit is tried, then the seat interaction is ended. Retried every 500 ms until you are out.
			{
				static ULONGLONG ejectAt = 0, gentleAt = 0;
				static unsigned ejectTries = 0;
				static bool gentle = false;
				if (driven && vehiclehits::Wrecked(At<std::uint32_t>(driven, off::kFormId))) {
					const ULONGLONG t = GetTickCount64();
					// 2026-10-28: the game's own way out first (it stands you up), the forced exit only if you are still
					// seated 2.5 s later (the forced one left you in the sitting pose).
					if (!gentleAt) {
						gentleAt = t; ejectAt = t;
						if (g.active) {
							g_autoOffWhy = "vehicle wrecked: getting you out"; g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
							Stop("wrecked: getting you out");
						}
						gentle = npcvehicles::BeginPlayerExit();
						logger::Write("Rover: your vehicle %08X is wrecked -> %s", At<std::uint32_t>(driven, off::kFormId),
							gentle ? "getting you out with the game's own exit" : "the game's exit refused; forcing you out");
						if (gentle) notify::Show("Your vehicle is wrecked: getting out");
					}
					if (t - ejectAt >= 500 && (!gentle || t - gentleAt >= 2500)) {
						ejectAt = t;
						settleSince = 0;
						if (g.active) {
							g_autoOffWhy = "vehicle wrecked: getting you out"; g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
							Stop("wrecked: you are thrown out");
						}
						const bool out = npcvehicles::ExitPlayer(true);
						if (ejectTries++ < 5) logger::Write("Rover: your vehicle %08X is wrecked -> ejecting you: %s", At<std::uint32_t>(driven, off::kFormId), out ? "out" : "not yet (retrying)");
						if (out) notify::Show("Your vehicle is wrecked: you were thrown out");
					}
				}
				else { ejectTries = 0; gentleAt = 0; gentle = false; }
			}
			static std::uintptr_t settleVehicle = 0;
			static int exitKey = 0;
			if (!exitKey) exitKey = controlmap::KeyFor("Vehicle", "VehicleExit", 'E');
			const bool eDown = focused && driven && bindings::Held(bindings::Action::DriverExit,exitKey);
			if (eDown && !exitDown && g.active && !settleSince) {
				settleSince = GetTickCount64();
				settleVehicle = driven;
				g_autoOffWhy = "your E key (getting out)"; g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
				Stop("exit: setting down first");
				notify::Log("Setting down to get out");
			}
			exitDown = eDown;
			if (settleSince) {
				const ULONGLONG t = GetTickCount64();
				double speed = 99.0;
				static thread_local world::Snapshot snap;
				if (world::Vehicles(snap)) for (unsigned k = 0; k < snap.count && k < world::kMaxVehicles; ++k) if (snap.v[k].ref == settleVehicle)
					speed = std::sqrt(snap.v[k].vel[0] * snap.v[k].vel[0] + snap.v[k].vel[1] * snap.v[k].vel[1] + snap.v[k].vel[2] * snap.v[k].vel[2]);
				if (driven != settleVehicle) { settleSince = 0; settleVehicle = 0; }  // already out (or another vehicle)
				else if (t - settleSince > 400 && (speed < 0.8 || t - settleSince > 4000)) {
					const bool out = npcvehicles::ExitPlayer();
					logger::Write("Rover exit: set down (%.1f m/s after %.1f s) -> the game's exit %s", speed, (t - settleSince) / 1000.0, out ? "let you out" : "refused (really obstructed here)");
					settleSince = 0; settleVehicle = 0;
				}
			}
		}
		// Got out with Auto on: the vehicle holds where it is (it neither follows nor lands).
		if (!driven && g.playerAuto) {
			const auto last = g_lastDrivenVehicle.load(std::memory_order_relaxed);
			g_autoOffWhy = "you left the vehicle"; g.playerAuto = false;
			if (last && g.active) {
				const auto hp = At<game::Vec3>(last, off::kRefPosition);
				g.holdId = At<std::uint32_t>(last, 0x28);
				g.holdZ=hp.z;
				g.holdX = hp.x;
				g.holdY = hp.y;
				g.autopilot = true;
				g.autopilotSince = GetTickCount64();
				notify::Log("Vehicle holding here");
				logger::Write("Rover auto: you got out; %08X holds at (%.0f, %.0f)", g.holdId, hp.x, hp.y);
			}
		}
		if (driven) {
			if (g.holdId && At<std::uint32_t>(driven, 0x28) != g.holdId) Stop("you took another vehicle");
			if (!g.routeHold) g.holdId = 0;
			g_lastDrivenVehicle.store(driven, std::memory_order_relaxed);
			if (g.autopilot && !g.playerAuto) {
				g.autopilot = false;  // the player got in: hover stays, under the player's control
				logger::Write("Rover call: player boarded; autopilot off");
			}
			// Selecting a destination arms navigation; movement takes control back.
			const bool goBoard = g_goOnBoard.load() && !g.playerAuto && GetTickCount64() - g_goAt.load() < 900000 && At<std::uint32_t>(driven, 0x28) == g_roverId;
			if (goBoard) {
				g_goOnBoard.store(false);
				logger::Write("Rover scanner trip: you boarded; Auto starts toward your marker");
			}
			if (goBoard) {
				g.routeHold=false; g.holdId=0;
					const bool wasHovering = g.active;
					if (!wasHovering) Start(0, driven, true);  // CW-1: on its wheels
					g.autopilot = true;
					g.playerAuto = true;
					g.taxiWait = false;
					g.taxiMarker = 0;
					g.cramped = 0;  // a new Auto flight starts with a clean slate (no leftover contact score)
					g_bumps[0] = Bumps{};
					g.autopilotSince = GetTickCount64();
					notify::Log("Destination selected - navigating; W/A/S/D takes over");
					logger::Write("Rover auto: on (%s)", wasHovering ? "already hovering" : "on its wheels");
			}
			else if (g.playerAuto && focused && (alt.cycle || alt.drop || bindings::Amount(bindings::Action::Accelerate, g_mk.accel) > 0.0 || bindings::Amount(bindings::Action::Brake, g_mk.brake) > 0.0 || bindings::Amount(bindings::Action::TurnLeft, g_mk.left) > 0.0 || bindings::Amount(bindings::Action::TurnRight, g_mk.right) > 0.0)) {
				g_autoOffWhy = "your drive keys (W/A/S/D, gear or brake)"; g.playerAuto = false;
				g.autopilot = false;
				g.routeHold=false; g.holdId=0;
				notify::Log("Auto off - you have control");
				logger::Write("Rover auto: off (movement key)");
			}
		}
		// Only the player's own vehicle can be summoned: the one the game links to the player or the home ship, looked
		// up every 5 s (never "the last vehicle driven", which may be someone else's).
		{
			static ULONGLONG lastLook = 0;
			const ULONGLONG nowMs = GetTickCount64();
			if (!g.autopilot && (!lastLook || nowMs - lastLook >= 5000)) {
				lastLook = nowMs;
				static bool quiet = false;
				const auto id = FindOwnedRover(player, quiet);
				quiet = true;
				if (id != g_roverId) {
					logger::Write("Rover call: own vehicle %08X", id);
					g_roverId = id;
				}
			}
		}
		// A follower drives the vehicle (a crew job): this autopilot stands down.
		const bool crewDrives = g_roverId && npcvehicles::UsesVehicle(g_roverId) && !driven;
		if (crewDrives) {
			if (g.autopilot) Stop("a follower drives it");
			if (g_follow || g_guard) {
				g_follow = g_guard = false;
				logger::Write("Rover cover: off (a follower drives the vehicle)");
			}
		}
		if (driven) g_guard = false;
		CoverFightTick(player, driven != 0, crewDrives);
		// N (on foot): Summon / Recall. The vehicle always comes to the player (far: moved next to them; nearer: flown over), over Cover, a guard point,
		// a hold, a ram and any fight; pressing it again re-issues it and never cancels it. U: Cover on / off (follow and defend the player, overrides.h).
		{
			// N tap: your vehicle only; N hold (1 s): every escort too, which also works while you drive. U tap: your vehicle covers you by itself;
			// U hold: a free follower drives it to cover you. The stance key cycles Defend / Attack / Hold fire anywhere.

            keygesture::Action nAct{bindings::Triggered(bindings::Action::Summon),bindings::Triggered(bindings::Action::RecallEscorts)};
            keygesture::Action uAct{bindings::Triggered(bindings::Action::Cover),bindings::Triggered(bindings::Action::CrewDrive)};
            const bool stanceHeld=bindings::Triggered(bindings::Action::Stance);
			if (stanceHeld) {
				const auto s = npcvehicles::CycleStance();
				notify::Show("Stance: %s", s == npcvehicles::Stance::Attack ? "Attack" : s == npcvehicles::Stance::HoldFire ? "Hold fire" : "Defend");
			}
			const bool scanCall = g_scanCall.exchange(false);  // the scanner marker button counts as the call key
			bool callPressed = (!driven && (nAct.tap || nAct.hold)) || scanCall;
			const bool coverHand = uAct.hold;  // hand your vehicle to a follower
			bool followPressed = uAct.tap || uAct.hold;
			if (nAct.hold) {
				npcvehicles::Recall(15000);  // escorts drop their fights and come back into formation
				logger::Write("Rover recall: N held; escorts recalled%s", driven ? " (you are driving)" : " with your vehicle");
				notify::Show("Escorts summoned");
			}
			if (callPressed && !scanCall) {
				
				logger::Write("Rover recall: N %s", nAct.hold ? "held" : "tapped");
			}
			// Crew: with a follower driving or free, the buttons go to the follower.
			if (!driven && (callPressed || followPressed) && g_roverId) {
				const auto order = npcvehicles::CrewOrderOf(g_roverId);
				const auto ppos = At<game::Vec3>(player, off::kRefPosition);
				if (order != npcvehicles::CrewOrder::None) {
					if (scanCall) npcvehicles::SetCrewOrder(g_roverId, npcvehicles::CrewOrder::Marker);
					else if (callPressed) npcvehicles::SetCrewOrder(g_roverId, npcvehicles::CrewOrder::Recall);
					else {
						const auto mode = order == npcvehicles::CrewOrder::Cover ? overrides::Mode::Cover : order == npcvehicles::CrewOrder::Guard ? overrides::Mode::Guard : overrides::Mode::None;
						if (overrides::OnCover(mode) == overrides::Mode::None) npcvehicles::ReleaseCrew(g_roverId);  // cover off: the follower gets out
						else npcvehicles::SetCrewOrder(g_roverId, npcvehicles::CrewOrder::Cover);
					}
					g_goOnBoard.store(false);
					callPressed = followPressed = false;
				}
				else if (g_crewPending != npcvehicles::CrewOrder::None && followPressed) {
					g_crewPending = npcvehicles::CrewOrder::None;
					logger::Write("Rover crew: cancelled");
					notify::Show("Follower order cancelled");
					followPressed = false;
				}
				else if ((coverHand || (scanCall && !g_follow)) && Rover() && npcvehicles::FreeFollower(player, ppos.x, ppos.y, 60.0)) {
					// N is for the player to drive, so only a U hold and the scanner trip hand the vehicle to a follower.
					if (g_follow || g_guard) { g_follow = g_guard = false; logger::Write("Rover cover: handed to a follower"); }
					g_crewPending = scanCall ? npcvehicles::CrewOrder::Marker : npcvehicles::CrewOrder::Cover;
					g_crewAt = GetTickCount64();
					g_goOnBoard.store(false);
					logger::Write("Rover crew: %s; bringing the vehicle for a follower", scanCall ? "marker" : "cover");
					notify::Show("A follower will drive your vehicle");
					callPressed = true;    // the vehicle is brought as a summon brings it
					followPressed = false; // the follower covers, not the empty vehicle
				}
			}
			if (followPressed && coverHand) {
				logger::Write("Rover crew: no follower free; your vehicle covers you by itself");
				notify::Show(npcvehicles::IsInCombat(player) ? "No follower free | your vehicle covers you" : "No follower free | your vehicle follows you");
			}
			if (followPressed && !driven) {
				if (g_follow && !g_guard && !coverHand) {
					g_follow = false;
					logger::Write("Rover cover: off");
					if (g.autopilot) Stop("cover off");
				}
				else if (Rover()) {
					if (g_guard) logger::Write("Rover cover: guard point dropped; covering you again");
					g_follow = true;
					g_guard = false;
					g_followStill = 0;
					logger::Write("Rover cover: on");
				}
				else {
					logger::Write("Rover cover: you own no vehicle");
				}
			}
			if ((callPressed || followPressed) && g_roverId && vcontract::AssignedTo(g_roverId)) {
				vcontract::Unassign(g_roverId, vcontract::Class::Navigation);  // the player's commands always win over where a profile drives
				logger::Write("Rover profile: unassigned (summon / cover pressed)");
				if (g_remote) {
					g_remote = false;
					if (g.autopilot) Stop("profile replaced by summon / cover");
				}
			}
			if ((callPressed || followPressed) && !driven && g.holdId) Stop("hold ended (summon / cover)");
			if (callPressed && !driven) {
				g_guard = false;
				if (g_follow) {
					g_follow = false;  // a recall replaces covering: a flying rover keeps flying, now to the player
					logger::Write("Rover cover: off (recalled)");
				}
				const auto rover = Rover();
				if (!rover) {
					logger::Write("Rover call: you own no vehicle (no vehicle link from the player or the home ship)");
				}
				else {
					const auto rp = At<game::Vec3>(rover, off::kRefPosition), pp = At<game::Vec3>(player, off::kRefPosition);
					const double dist = std::hypot(double(rp.x) - pp.x, double(rp.y) - pp.y);
					const auto loaded = At<std::uintptr_t>(rover, off::kRefLoadedData);
					const bool has3D = loaded && At<std::uintptr_t>(loaded, 0x08);
					const bool sameWorld = Worldspace(rover) && Worldspace(rover) == Worldspace(player);
					if (!has3D || !sameWorld || !std::isfinite(dist) || dist > kCallFar) {
						if (g.autopilot) Stop("recall: moving it next to you");
						logger::Write("Rover call: %08X %s; moving it next to the player (or the area's summon point)", g_roverId,
							!sameWorld ? "in another worldspace" : !has3D ? "not loaded" : "far");
						AskMove(player, true);
					}
					else if (dist <= kCallStop + 2.0) {
						if (g.autopilot) Stop("recall: already beside you");
						logger::Write("Rover call: already here (%.0f m)", dist);
					}
					else if (g.autopilot) {
						g.autopilotSince = GetTickCount64();  // already flying: it now comes straight to the player
						logger::Write("Rover call: recalled in flight from %.0f m", dist);
					}
					else {
						Start(rover);
						g.autopilot = true;
						g.autopilotSince = GetTickCount64();
						g.gear = std::max(g.gear, 2);
						logger::Write("Rover call: autopilot from %.0f m", dist);
					}
				}
			}
			FinishMove(player);
			// Crew: the vehicle is near and parked -> the follower boards it with the order.
			if (g_crewPending != npcvehicles::CrewOrder::None) {
				const auto rover = Rover();
				const ULONGLONG cNow = GetTickCount64();
				const double d = rover ? std::hypot(double(At<game::Vec3>(rover, off::kRefPosition).x) - At<game::Vec3>(player, off::kRefPosition).x,
					double(At<game::Vec3>(rover, off::kRefPosition).y) - At<game::Vec3>(player, off::kRefPosition).y) : 1e9;
				if (!rover || driven || cNow - g_crewAt > kCrewWaitMs) {
					logger::Write("Rover crew: given up (%s)", !rover ? "no vehicle" : driven ? "you got in" : "the vehicle did not come");
					g_crewPending = npcvehicles::CrewOrder::None;
				}
				else if (!g.autopilot && !g_move.pending && d < kCrewNear) {
					const auto order = g_crewPending;
					g_crewPending = npcvehicles::CrewOrder::None;
					if (!npcvehicles::CrewBoard(player, rover, order)) {
						const bool cover = order == npcvehicles::CrewOrder::Follow || order == npcvehicles::CrewOrder::Cover;
						logger::Write("Rover crew: no follower could board; the vehicle %s", cover ? "covers you by itself" : "waits for you");
						if (cover) {
							g_follow = true;
							g_followStill = 0;
						}
						else {
							g_goOnBoard.store(true);
							g_goAt.store(cNow);
						}
					}
				}
			}
		}
		// ---- Follow: the rover trails the player (a point kFollowBack behind, kFollowSide to the side) ----
		if (g_follow) {
			const auto rover = Rover();
			const auto pcell = At<std::uintptr_t>(player, off::kRefParentCell);
			const bool outdoors = pcell && At<std::uintptr_t>(pcell, off::kCellWorldspace);
			const ULONGLONG nowMs = GetTickCount64();
			if (driven) {
				g_follow = false;
				logger::Write("Rover follow: off (player boarded)");
			}
			else if (!rover) {
				g_follow = false;
				if (g.autopilot) Stop("rover lost");
				logger::Write("Rover follow: off (no rover)");
			}
			else if (vehiclehits::Wrecked(g_roverId)||IsDisabled(g_roverId)) {
				g_move.pending=false;
				if(g.active)Stop("wrecked: native falling physics");
			}
			else if (!outdoors) {
				// Wait at the door: following stays on; the rover parks where it is (it trailed the player to the entrance) until they come out.
				if (!g_waitDoor) {
					g_waitDoor = true;
					if (g.autopilot) Stop("player went indoors; waiting at the door");
					logger::Write("Rover follow: you went indoors; the rover waits at the door");
				}
			}
			else {
				if (g_waitDoor) {
					g_waitDoor = false;
					logger::Write("Rover follow: you are outside again; following");
				}
				const auto rp = At<game::Vec3>(rover, off::kRefPosition), pp = At<game::Vec3>(player, off::kRefPosition);
				const double distP = std::hypot(double(rp.x) - pp.x, double(rp.y) - pp.y);
				const auto loaded = At<std::uintptr_t>(rover, off::kRefLoadedData);
				const bool has3D = loaded && At<std::uintptr_t>(loaded, 0x08);
				const bool sameWorld = Worldspace(rover) && Worldspace(rover) == Worldspace(player);
				// The player standing still (moved under 1 m in kFollowStillMs).
				static double sx = 0, sy = 0;
				if (!g_followStill || std::hypot(pp.x - sx, pp.y - sy) > 1.0) {
					sx = pp.x; sy = pp.y;
					g_followStill = nowMs;
				}
				const bool playerStill = nowMs - g_followStill >= kFollowStillMs;
				static ULONGLONG lastMove = 0;
				if (!has3D || !sameWorld || !std::isfinite(distP) || distP > kCallFar) {
					if (g.autopilot) Stop("follow: too far");
					if (!g_move.pending && nowMs - lastMove > 10000) {
						lastMove = nowMs;
						logger::Write("Rover follow: %.0f m behind or not loaded; moving it next to the player", distP);
						AskMove(player);
					}
				}
				else if (!g.autopilot) {
					if ((distP > kFollowRelaunch || g_air.air) && nowMs >= g_relaunchAt && !IsDisabled(g_roverId)) {
						Start(rover);
						g.autopilot = true;
						g.autopilotSince = nowMs;
						logger::Write("Rover follow: launching (%.0f m from the player%s)", distP, g_air.air ? ", you are in the air" : "");
					}
				}
				else if (playerStill && distP < kFollowFar && !g_air.air && !CoverFighting()) {
					// 2026-10-28 18:12: parked 7.2 m up and dropped. Park only once down (ground unknown: as before); higher, it keeps flying down.
					const double above = PilotAboveGround(0, rover);
					if (!std::isfinite(above) || above < kDropMaxAbove) Stop("follow: player stopped; parked");
				}
			}
		}
		// ---- HOOK 2b (vehicle contract, Profile Fork): a profile on the player's own EMPTY vehicle (scout / return home / hold) ----
		{
			const vcontract::Profile* rp = g_roverId ? vcontract::AssignedTo(g_roverId) : nullptr;
			if (!rp) g_remoteDone = nullptr;
			if (driven) {
				if (rp) {
					vcontract::Unassign(g_roverId, vcontract::Class::Navigation);  // remote profiles are for an empty vehicle; Cruise is handled by hook 2a
					logger::Write("Rover profile: unassigned (you got in)");
				}
				g_remote = false;
			}
			else if (!rp) {
				if (g_remote) {
					g_remote = false;
					if (g.autopilot) Stop("profile unassigned");
				}
			}
			else if (!g_follow && rp != g_remoteDone) {
				if (g_remote && !g.autopilot) g_remote = false;  // something else stopped the autopilot: do not pretend it still runs
				if (!g_remote && !g.autopilot) {
					const auto rover = Rover();
					const auto loaded = rover ? At<std::uintptr_t>(rover, off::kRefLoadedData) : 0;
					if (rover && loaded && At<std::uintptr_t>(loaded, 0x08)) {
						Start(rover);
						g.autopilot = true;
						g.autopilotSince = GetTickCount64();
						g.gear = std::max(g.gear, 2);
						g_remote = true;
						logger::Write("Rover profile: %s runs on your empty vehicle %08X", rp->name, g_roverId);
					}
				}
			}
		}
		g_ownRover = Rover();
		{
			// The contextual HUD: what is there to do on foot and in the vehicle.
			const auto rover = g_ownRover.load();
			double toRover = 1e9;
			if (rover) {
				const auto rp = At<game::Vec3>(rover, off::kRefPosition), pp = At<game::Vec3>(player, off::kRefPosition);
				toRover = std::hypot(double(rp.x) - pp.x, double(rp.y) - pp.y);
				if (std::fabs(double(rp.z) - pp.z) > 4.0) toRover = std::max(toRover, kNearBoard + 1.0);
			}
			char status[64] = "";
			const ULONGLONG nowMs = GetTickCount64();
			const auto crew = g_roverId ? npcvehicles::CrewOrderOf(g_roverId) : npcvehicles::CrewOrder::None;
			const bool covering = g_follow || crew == npcvehicles::CrewOrder::Cover || crew == npcvehicles::CrewOrder::Follow;
			const bool guarding = (g_guard && g.holdId) || crew == npcvehicles::CrewOrder::Guard;
			if (!driven) {
				const auto stance = npcvehicles::CurrentStance();
				const char* defend = stance == npcvehicles::Stance::HoldFire ? " \xC2\xB7 HOLD FIRE" : !CoverFighting() ? (stance == npcvehicles::Stance::Attack ? " \xC2\xB7 ATTACK" : "") :
					stance == npcvehicles::Stance::Attack ? " \xC2\xB7 ATTACK" : " \xC2\xB7 DEFEND";
				if (guarding) std::snprintf(status, sizeof(status), "GUARD POINT%s", defend);
				else if (crew == npcvehicles::CrewOrder::Recall) std::snprintf(status, sizeof(status), "SUMMONING  %.0f m", std::min(toRover, 9999.0));
				else if (g.holdId || crew == npcvehicles::CrewOrder::Hold) std::snprintf(status, sizeof(status), "HOLDING");
				else if (covering) std::snprintf(status, sizeof(status), "%s%s  %.0f m", overrides::CoverStatus(npcvehicles::IsInCombat(player)), defend, std::min(toRover, 9999.0));
				else if (g_remote && vcontract::AssignedTo(g_roverId))  // the empty vehicle runs its navigation profile: SCOUT, RETURN HOME
					std::snprintf(status, sizeof(status), "%s", profileclass::Upper(vcontract::AssignedTo(g_roverId)->name).c_str());
				else if (g.autopilot) std::snprintf(status, sizeof(status), "SUMMONING  %.0f m", std::min(toRover, 9999.0));
				else if (g_move.pending) std::snprintf(status, sizeof(status), "CALLING");  // not "moved" until it has (2026-10-28)
				else if (g_movedAt && nowMs - g_movedAt < 4000) std::snprintf(status, sizeof(status), g_movedToPoint ? "AT SUMMON POINT" : "MOVED TO YOU");
			}
			const std::string callKey = bindings::Label(bindings::Action::Summon, g_keys.call);
			const std::string followKey = bindings::Label(bindings::Action::Cover, g_keys.follow);
			const std::string useKey = controlmap::KeyName(controlmap::KeyFor("MainGameplay", "Activate", 'E'));
			// The names: N is always Summon; U is Follow, or Cover while you are in combat.
			hoverui::SetFoot(!driven && g_roverId != 0, rover && !driven && !g.autopilot && toRover <= kNearBoard, callKey.c_str(), followKey.c_str(), useKey.c_str(), status,
				overrides::SummonLabel(), overrides::CoverLabel(npcvehicles::IsInCombat(player)));
			if (g.cruise && !(driven && g.playerAuto)) {  // anything that ended Auto (the movement keys, leaving the vehicle) ends Cruise
				g.cruise = false; g.cruiseRoute = {};
				PilotNav(0, vcontract::NavParams{});
				logger::Write("Rover cruise: off (source=%s)", g_autoOffWhy && *g_autoOffWhy ? g_autoOffWhy : "unknown");
			}
			char line[64] = "";
			if (driven && g.playerAuto && g.cruise) std::snprintf(line, sizeof(line), g.cruiseRoute.step ? "CRUISING" : "FINDING TRAFFIC");
			else if (driven && g.playerAuto) {
				if (g.taxiMarker && ProgressFor(g_roverId).phase==triplogic::Phase::Arrived) std::snprintf(line, sizeof(line), "ARRIVED");
				else if (g.taxiMarker) std::snprintf(line, sizeof(line), "%s  %.0f m", triplogic::Text(ProgressFor(g_roverId).phase), std::min(g.taxiLeft, 99999.0));
				else std::snprintf(line, sizeof(line), "AIM + T");
			}
			hoverui::SetTripStatus(driven && g.playerAuto, line);
			hoverui::SetAutoKey(driven ? bindings::Label(bindings::Action::AutoToggle).c_str() : "");
			hoverui::SetFollow(g_follow);
		}
		std::uintptr_t heldVehicle = 0;
		if (!driven && g.holdId) {
			heldVehicle = RefById(g.holdId);
			if (!heldVehicle) Stop("held vehicle lost");
		}
		const auto vehicle = driven ? driven : (g.autopilot ? (heldVehicle ? heldVehicle : Rover()) : 0);
		// Saved hover (flightsave.h): a loaded save that was made while you hovered asks for the hover once, through this
		// same on path as the hover key. Asked every frame while hover is off until it is answered or expires (logged).
		if (!g.active) {
			const bool seat = driven && !passenger::RidingVehicle();
			const std::uint32_t seatId = seat ? At<std::uint32_t>(driven, off::kFormId) : 0;
			const double seatZ = seat ? double(At<game::Vec3>(driven, off::kRefPosition).z) : std::numeric_limits<double>::quiet_NaN();
			int savedGear = g.gear;
			if (flightsave::TakeRestore(seatId, seatZ, savedGear)) {
				g.gear = savedGear;  // Start carries the gear over
				Start(driven);
			}
		}
		if (!driven && g.autopilot && !vehicle) {
			Stop("rover lost");
			return;
		}
		static ULONGLONG lastStepReport = 0;
		const ULONGLONG stepReportNow = GetTickCount64();
		if (stepReportNow - lastStepReport >= 10000) {
			lastStepReport = stepReportNow;
			const unsigned samples = g_nativeStepSamples.exchange(0, std::memory_order_relaxed);
			const unsigned skipped = g_nativeStepSkipped.exchange(0, std::memory_order_relaxed);
			if (samples) logger::Write("Vehicle physics time: %u callbacks, last native dt %.6f s; %u invalid/long intervals skipped by custom control (native dt forwarded unchanged)",
				samples, double(g_nativeStepSeconds.load(std::memory_order_relaxed)), skipped);
			const unsigned physicalSteps=g_physicalSteps.exchange(0),physicalFailures=g_physicalFailures.exchange(0);
			if (physicalSteps||physicalFailures) {
				static const char* reasons[]={"none","missing body","world interfaces differ","body generation/type differs","linear writer differs","angular writer differs","mass/inertia/frame invalid","unreadable native data","invalid model input","native impulse fault"};
				logger::Write("Physical VTOL: %u impulse steps, %u rejected; native mass %.1f, collective %.2f m/s^2; response error linear %.5f m/s angular %.5f rad/s; last reject %s; driver-only, no velocity overwrite",
					physicalSteps,physicalFailures,double(g_physicalMass.load()),double(g_physicalCollective.load()),double(g_physicalLinearError.load()),double(g_physicalAngularError.load()),reasons[std::min(g_physicalReject.load(),9u)]);
			}
			{
				char aiLine[256]; int n=0;
				for (int i=0;i<kPilots;++i) {
					const unsigned steps=g_shs[i].physSteps.exchange(0);
					bool ai,fault; { std::lock_guard lk(g_shs[i].lock); ai=g_shs[i].autopilot; fault=g_shs[i].physicalFault&&g_shs[i].physical; }
					if ((steps&&ai)||fault) n+=std::snprintf(aiLine+n,sizeof(aiLine)-n,"%s%s %u%s",n?", ":"",Who(i),ai?steps:0,fault?" (fell back)":"");
					if (n>=int(sizeof(aiLine))-48) break;
				}
				if (n) logger::Write("Physical VTOL AI: impulse steps in 10 s: %s",aiLine);
			}
			const unsigned occupied = g_occupiedSteps.exchange(0, std::memory_order_relaxed);
			const unsigned unoccupied = g_unoccupiedSteps.exchange(0, std::memory_order_relaxed);
			if (occupied || unoccupied) {
				if (logger::Verbose()) logger::Write("Ship vehicle physics probe: last rover stepped %u times while driven, %u times after leaving it (10 s)", occupied, unoccupied);
			}
		}
		// Hover on / off is the hover key only (2026-10-24, the user approved the no-chord map): double-tapping the vertical
		// boost made the vehicle hop twice first, since the game boosts on each press.
		static std::uintptr_t announced = 0;
		if (vehicle && vehicle != announced && !g.active) {
			announced = vehicle;
			notify::Log("Rover: %s to hover", bindings::Label(bindings::Action::HoverToggle, g_keys.toggle).c_str());
		}
		if (!vehicle) {
			announced = 0;
			if (g.active) {
				Stop("left the vehicle");
			}
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.groundOn = false;
			}
			return;
		}
		// Only while driving: in hover the rover turns toward the reading in use, which would confirm a wrong one.
		if (!g.active) {
			double cp;bool first;
			if(CameraAimReading(vehicle,cp,first))g_camRest.Observe(cp,first,true);
		}
		// Hover toggle evidence and guard (2026-10-24 12:42-12:43 run: 5 of 7 L3 hover-offs came back on 0.17-0.23 s later, a
		// second toggle in one press's time). Every raw L3 edge while driving is logged with how long it was held, and a toggle
		// within kToggleGuardMs of the last one is ignored, so one click can never switch hover off and straight back on.
		constexpr ULONGLONG kToggleGuardMs = 500;
		static bool l3Was = false;
		static ULONGLONG l3DownAt = 0, lastToggleAt = 0;
		const ULONGLONG toggleNow = GetTickCount64();
		const bool l3 = (bindings::PadState() & 0x0040u) != 0;  // bindlogic::kLS
		if (l3 != l3Was) {
			if (l3) l3DownAt = toggleNow;
			if (driven) logger::Write("Hover toggle: L3 %s%s (hover %s)", l3 ? "down" : "up",
				l3 ? "" : (" after " + std::to_string(toggleNow - l3DownAt) + " ms").c_str(), g.active ? "on" : "off");
			l3Was = l3;
		}
		if (pressed && driven) {
			if (lastToggleAt && toggleNow - lastToggleAt < kToggleGuardMs) {
				logger::Write("Hover toggle: ignored a second toggle %llu ms after the last one (L3 %s); hover stays %s",
					toggleNow - lastToggleAt, l3 ? "down" : "up", g.active ? "on" : "off");
			}
			else {
				lastToggleAt = toggleNow;
				logger::Write("Hover toggle: %s (L3 %s)", g.active ? "off" : "on", l3 ? "down" : "up");
				if (g.active) {
					Stop(bindings::Label(bindings::Action::HoverToggle, g_keys.toggle).c_str());
					return;
				}
				Start(vehicle);
			}
		}
		if (!g.active) {
			// Driving uses the same Alt-tap gear as hover; it caps the ground speed.
			const bool wHeld = focused && bindings::Held(bindings::Action::Accelerate, g_mk.accel), sHeld = focused && bindings::Held(bindings::Action::Brake, g_mk.brake);
			g.wDown = wHeld;
			g.sDown = sHeld;
			const auto gp = At<game::Vec3>(vehicle, off::kRefPosition);
			constexpr double kGroundShare[5] = { 1.0, 0.25, 0.5, 0.75, 1.0 };
			std::lock_guard lk(g_sh.lock);
			BindBody(g_sh,vehicle);
			g_sh.px = gp.x; g_sh.py = gp.y; g_sh.pz = gp.z;
			g_sh.groundOn = true;
			g_sh.groundFrac = kGroundShare[std::clamp(g.gear, 0, 4)];
			g_sh.groundLearn = g.gear == 4 && wHeld && !bindings::Held(bindings::Action::Boost, VK_LSHIFT);
			return;
		}
		{
			std::lock_guard lk(g_sh.lock);
			g_sh.groundOn = false;
		}
		Goal goal;
		if (g.autopilot && (g.holdId || (driven && g.playerAuto))) {
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			goal.keepGoing = true;
			if (g.holdId) {
				goal.x = g.holdX;
				goal.y = g.holdY;
				goal.z = g.holdZ;
				goal.stop = 1.5;
				goal.hold = true;
				if (g_guard && g.holdId == g_roverId && CoverFighting() && g_cf.haveSpot) {
					goal.x = g_cf.spotX; goal.y = g_cf.spotY; goal.z = g_cf.spotZ; goal.stop = 6.0;  // defending the guard point from its best spot
				}
			}
			else if (g.cruise) {
				const ULONGLONG cruiseNow = GetTickCount64();
				if (!g.cruiseRoute.step && g_cruisePlanner && cruiseNow >= g.cruiseAskAt) {
					FvoTripClaim c{};
					if (g_cruisePlanner(At<std::uint32_t>(vehicle, off::kFormId), pos.x, pos.y, pos.z, &c) && c.step) {
						g.cruiseRoute = c; g.cruiseToldNone = false;
					}
					else {
						g.cruiseAskAt = cruiseNow + 5000;
						if (!g.cruiseToldNone) { g.cruiseToldNone = true; notify::Log("Cruise: no traffic to join here"); logger::Write("Rover cruise: no route to join; asking again every 5 s"); }
					}
				}
				bool cruising = false;
				if (g.cruiseRoute.step) {
					FvoTripGoal tg{};
					if (g.cruiseRoute.step(g.cruiseRoute.user, pos.x, pos.y, pos.z, &tg) == 0) {
						cruising = true;
						goal.x = tg.x; goal.y = tg.y; goal.z = tg.z;
						goal.stop = tg.stop > 0 ? tg.stop : 6.0;
						vcontract::NavParams nav;
						nav.passThrough = tg.passThrough != 0;
						nav.speedLimit = tg.speed;
						nav.priority = 100;
                        RouteNav(0,g.cruiseRoute.user,tg,nav);
						PilotNav(0, nav);
						g.cruiseWaitX = pos.x; g.cruiseWaitY = pos.y;
					}
					else { g.cruiseRoute = {}; PilotNav(0,vcontract::NavParams{}); }  // the lap is over: the next one is asked for next frame
				}
				if (!cruising) {  // holds here until there is a lap to fly
					goal.x = g.cruiseWaitX; goal.y = g.cruiseWaitY; goal.stop = 1.5;
				}
				goal.hold = !cruising;
			}
			else if (vcontract::Goal pg; vcontract::GoalFor(vcontract::User::Player, 0, vehicle, player, pg)) {
				// HOOK 2a of the vehicle contract (Profile Fork): the player drives with Auto on and a profile is assigned to this vehicle.
				PilotNav(0, pg.nav);
				goal.x = pg.x;
				goal.y = pg.y;
				goal.stop = pg.stop;
				goal.z = pg.z;
				goal.cruise = !pg.hold;
				goal.cruiseSpeed = kAiGearTop[std::clamp(g.gear, 1, 4)];
				goal.hold = pg.hold;
			}
			else {
				// Taxi test: Auto never moves on its own; only the player's custom map marker gives it somewhere to go.
				double mx = 0, my = 0;
				std::uint32_t mid = 0;
				if (TaxiMarker(player, vehicle, mid, mx, my, &goal.z)) {
					if (mid != g.taxiMarker || std::hypot(mx - g.taxiMx, my - g.taxiMy) > 2.0 ||
						(std::isfinite(goal.z) && (!std::isfinite(g.taxiMz) || std::fabs(goal.z - g.taxiMz) > 2.5))) {
						if (g.taxiRoute.step) { g.taxiRoute = {}; PilotNav(0, vcontract::NavParams{}); }
						g.taxiRouteAsked = false;
						g.taxiMarker = mid;
						g.taxiMx = mx;
						g.taxiMy = my;
						g.taxiMz = goal.z;
						g.taxiArrived = false;
						notify::Log("Auto: flying to your marker (%.0f m)", std::hypot(mx - pos.x, my - pos.y));
						logger::Write("Rover taxi: marker %08X at (%.0f, %.0f), %.0f m away", mid, mx, my, std::hypot(mx - pos.x, my - pos.y));
					}
					g.taxiWait = false;
					g.taxiLeft = std::hypot(mx - pos.x, my - pos.y);
					goal.x = mx;
					goal.y = my;
					goal.stop = kTaxiStop;
					// A traffic route on the way (the addon's planner, asked once per marker): fly its trip, then on to the marker.
					if (g_routePlanner && !g.taxiRouteAsked && !g.taxiArrived) {
						g.taxiRouteAsked = true;
						FvoTripClaim c{};
						const double tz = std::isfinite(goal.z) ? goal.z : pos.z;
						if (g_routePlanner(At<std::uint32_t>(vehicle, off::kFormId), pos.x, pos.y, pos.z, mx, my, tz, &c) && c.step) {
							g.taxiRoute = c;
							notify::Log("Auto: taking the traffic route");
							logger::Write("Rover taxi: %s to marker %08X", c.name ? c.name : "route trip", mid);
						}
					}
					bool onRoute = false;
					if (g.taxiRoute.step) {
						FvoTripGoal tg{};
						if (g.taxiRoute.step(g.taxiRoute.user, pos.x, pos.y, pos.z, &tg) == 0) {
							onRoute = true;
							goal.x = tg.x; goal.y = tg.y; goal.z = tg.z;
							goal.stop = tg.stop > 0 ? tg.stop : 6.0;
							vcontract::NavParams nav;
							nav.passThrough = tg.passThrough != 0;
							nav.speedLimit = tg.speed;
							nav.priority = 100;
                            RouteNav(0,g.taxiRoute.user,tg,nav);
							PilotNav(0, nav);
						}
						else {
							g.taxiRoute = {};
							PilotNav(0, vcontract::NavParams{});
							logger::Write("Rover taxi: off the route; on to marker %08X (%.0f m)", mid, g.taxiLeft);
							notify::Log("Auto: off the route, on to your marker");
						}
					}
					if (onRoute) {
						g.taxiOrbit = false;
					}
					else if (g_autoMode == AutoMode::Orbit && g.taxiLeft < kOrbitRadius + 20.0) {
						// Orbit: a point a little ahead round the circle (clockwise), kept going instead of holding over the marker.
						const double ang = std::atan2(pos.x - mx, pos.y - my) + 0.45;
						goal.x = mx + std::sin(ang) * kOrbitRadius;
						goal.y = my + std::cos(ang) * kOrbitRadius;
						goal.stop = 1.0;
						g.taxiOrbit = true;
					}
					else {
						g.taxiOrbit = false;
					}
					if (!g.taxiArrived && !g.taxiOrbit && AtDestination(vehicle, mx, my, goal.z, kTaxiStop)) {
						g.taxiArrived = true;
						notify::Log("Auto: arrived");
						logger::Write("Rover taxi: arrived at marker %08X", mid);
					}
				}
				else {
					if (!g.taxiWait) {
						g.taxiWait = true;
						g.taxiWaitX = pos.x;
						g.taxiWaitY = pos.y;
						if (g.taxiMarker) logger::Write("Rover taxi: marker gone; holding at (%.0f, %.0f)", pos.x, pos.y);
						notify::Log("Auto: place a map marker to go");
					}
					g.taxiMarker = 0;
					g.taxiArrived = false;
					g.taxiLeft = 0;
					goal.x = g.taxiWaitX;
					goal.y = g.taxiWaitY;
					goal.stop = 1.5;
				}
				goal.hold = (!g.taxiOrbit || g.taxiWait) && !g.taxiRoute.step;
			}
		}
		else if (g.autopilot && g_remote) {
			// HOOK 2b: the goal comes from the assigned profile; false = it finished (or was unassigned): stop and keep the vehicle where it is.
			vcontract::Goal pg;
			const auto rpos = At<game::Vec3>(vehicle, off::kRefPosition);
			if (vcontract::GoalFor(vcontract::User::PlayerRemote, 0, vehicle, player, pg)) {
				PilotNav(0, pg.nav);  // the profile's navigation settings (speed limit, clearance ...) for the shared navigation service
				goal.x = pg.x;
				goal.y = pg.y;
				goal.stop = pg.stop;
				goal.z = pg.z;
				goal.keepGoing = true;
				goal.cruise = false;
				goal.hold = pg.hold;
				goal.timeoutMs = 3600000;
                // Shared Fly landing handles support, descent and native touchdown.

			}
			else {
				g_remoteDone = vcontract::AssignedTo(g_roverId);
				g_remote = false;
				Stop("profile done");
				return;
			}
		}
		else if (g.autopilot) {
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			goal.x = pp.x;
			goal.y = pp.y;
			goal.stop = kCallStop;
			goal.timeoutMs = kCallTimeoutMs;
			const double toPlayer = std::hypot(double(pp.x) - pos.x, double(pp.y) - pos.y);
			if (g_follow) {
				// The player's velocity (smoothed); moving, the follow point trails their track and leads it a little.
				static double lx = 0, ly = 0, vx = 0, vy = 0;
				static ULONGLONG lt = 0;
				const ULONGLONG tNow = GetTickCount64();
				if (lt && tNow > lt) {
					const double tdt = (tNow - lt) / 1000.0;
					const double ix = (pp.x - lx) / tdt, iy = (pp.y - ly) / tdt;
					if (tdt < 1.0 && std::hypot(ix, iy) < 100.0) {
						const double a = std::min(1.0, tdt * 3.0);
						vx += (ix - vx) * a;
						vy += (iy - vy) * a;
					}
					else {
						vx = vy = 0;
					}
				}
				lx = pp.x; ly = pp.y; lt = tNow;
				const bool moving = std::hypot(vx, vy) > 2.0;
				const double yaw = moving ? std::atan2(vx, vy) : At<game::Vec3>(player, off::kRefRotation).z;
				const double ax = pp.x + (moving ? vx * kFollowLead : 0.0), ay = pp.y + (moving ? vy * kFollowLead : 0.0);
				goal.x = ax - std::sin(yaw) * kFollowBack + std::cos(yaw) * kFollowSide;
				goal.y = ay - std::cos(yaw) * kFollowBack - std::sin(yaw) * kFollowSide;
				if (g_air.air) {
					// In the air: under the player, where their fall will be.
					UnderGoal(g_shs[0].gEst, goal.x, goal.y, goal.z);
					goal.stop = 1.0;
					goal.keepGoing = true;
				}
				else if (toPlayer < kFollowNear && toPlayer > 0.1) {
					// too close: back off along the line from the player
					goal.x = pp.x + (pos.x - pp.x) / toPlayer * kFollowBack;
					goal.y = pp.y + (pos.y - pp.y) / toPlayer * kFollowBack;
				}
				if (!g_air.air) {
					g_air.haveGoal = false;
					goal.stop = 3.0;
					if (CoverFighting() && g_cf.haveSpot) {
						goal.x = g_cf.spotX; goal.y = g_cf.spotY; goal.z = g_cf.spotZ; goal.stop = 6.0; goal.hold = true;  // shielding the player
					}
				}
				goal.keepGoing = true;
			}
		}
		if (g_gs[0].settle) { SettleTick(0, vehicle); return; }
		Fly(0, vehicle, goal, focused);
	}

	namespace
	{
		struct PilotJob
		{
			std::uint32_t vehicleId = 0;
			Goal goal;
		} g_jobs[kPilots];

		std::uintptr_t RefById(std::uint32_t id)
		{
			if (!id) return 0;
			std::uintptr_t r = 0;
			__try {
				r = EngineFn<std::uintptr_t (*)(std::uint32_t)>(kLookupForm)(id);
				if (r && At<std::uint32_t>(r, 0x28) != id) r = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				r = 0;
			}
			return r;
		}

        void RefreshFleetRails()
        {
            if (!g_fleetRailCount.load(std::memory_order_acquire) || !trafficprofile::FleetRailWritesAllowed()) return;
            static std::uint64_t lastRefresh = 0;
            const auto now = GetTickCount64();
            if (now - lastRefresh < 100) return;
            lastRefresh = now;
            // AC-2 traffic manager: the player and the physics radius once per refresh.
            game::Vec3 managerPlayer{}; bool managerHavePlayer = false;
            if (const auto pl = Ptr(addr::kPlayerCharacterPtr)) { managerPlayer = At<game::Vec3>(pl, off::kRefPosition); managerHavePlayer = true; }
            const double managerRadius = PhysicsRadius();
            constexpr double kManagerBandM = 20.0;  // inside radius - band: physical; beyond the radius: virtual; between: as it was
            std::array<unsigned, fvolimits::kVehicles> vehicles{};
            std::size_t count = 0;
            {
                std::lock_guard lk(g_fleetRailLock);
                for (const auto& entry : g_fleetRails) vehicles[count++] = entry.first;
            }
            for (std::size_t i = 0; i < count; ++i) {
                const auto id = vehicles[i];
                const auto ref = RefById(id);
                unsigned driver = 0;
                bool seeded = false;
                FvoFleetRailPoint position{}, ahead{}, enter{};
                {
                    std::lock_guard lk(g_fleetRailLock);
                    const auto it = g_fleetRails.find(id);
                    if (it == g_fleetRails.end()) continue;
                    const auto& car = it->second;
                    driver = car.driver;
                    seeded = car.seeded;
                    position = car.position;
                }
                if (ref && (!seeded || !FleetRailOwns(id))) {
                    const auto p = At<game::Vec3>(ref, off::kRefPosition);
                    position = {p.x, p.y, p.z};
                }
                if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z)) continue;
                int pilot = 0;
                for (int slot = 1; slot < kPilots; ++slot)
                    if (g_jobs[slot].vehicleId == id && g_gs[slot].active && g_gs[slot].autopilot) { pilot = slot; break; }
                std::uintptr_t instance = 0;
                unsigned body = 0;
                std::uint64_t driveStep = 0;
                if (pilot) {
                    auto& binding = g_shs[pilot];
                    std::lock_guard lk(binding.lock);
                    instance = binding.instance.load();
                    body = binding.bodyId.load();
                    if (!instance && binding.handoffStepAt && now - binding.handoffStepAt < 1500) {
                        instance = binding.handoffStepInstance;
                        body = binding.handoffStepBody;
                    }
                    if (binding.handoffStepInstance == instance && binding.handoffStepBody == body)
                        driveStep = binding.handoffStepAt;
                    if (binding.lastStepInstance == instance && binding.lastStepAt > driveStep)
                        driveStep = binding.lastStepAt;
                }
                const bool playerTook = PlayerDriving(id);
                const auto currentDriver = ref && !playerTook ? npcvehicles::VehicleDriverId(id) : 0;
                double station = 0.0;
                {
                    std::lock_guard lk(g_fleetRailLock);
                    const auto it = g_fleetRails.find(id);
                    if (it == g_fleetRails.end()) continue;
                    const auto& car = it->second;
                    station = car.railOwner && car.seeded ? car.station : car.path.Project(position.x, position.y, position.z);
                    const double look = fleetrail::ExitAhead(std::max(car.speed, car.railSpeed));
                    ahead = car.path.At(car.path.Advance(station, look));
                    enter = car.path.At(car.path.Advance(station, look + fleetrail::rules::kEnterAheadExtra));
                }
                loadedarea::RelocationCell here{}, next{}, further{};
                const bool hereKnown = ref && loadedarea::RelocationDestination(id, float(position.x), float(position.y), here);
                const bool nextKnown = ref && loadedarea::RelocationDestination(id, float(ahead.x), float(ahead.y), next);
                const bool furtherKnown = ref && loadedarea::RelocationDestination(id, float(enter.x), float(enter.y), further);
                std::lock_guard lk(g_fleetRailLock);
                const auto it = g_fleetRails.find(id);
                if (it == g_fleetRails.end() || !trafficprofile::FleetRailWritesAllowed()) continue;
                auto& car = it->second;
                const auto oldMode = car.mode;
                const bool sameBody = instance && body && instance == car.instance && body == car.body;
                const bool railFresh = sameBody && car.lastStep && now - car.lastStep < 1500;
                const bool driveFresh = driveStep && now - driveStep < 1500;
                const bool chassisReady = ref && pilot && instance && body && currentDriver == driver &&
                    (railFresh || driveFresh) && !*car.fault;
                const bool autopilotReady = pilot && (railFresh || driveFresh);
                if (!kFleetRailDrives) {  // AC-1: report only; the autopilot drives (physical or virtual)
                    const auto v = g_virtualAt.find(id);
                    const bool movedVirtually = v != g_virtualAt.end() && now - v->second < 1500;
                    if (car.railOwner) NoteFleetHandoff(id, car.railSpeed, now);
                    car.railOwner = false;
                    // AC-2 advice: no model -> virtual; within the physics radius (less the band) -> physical; beyond it -> virtual;
                    // in the band it stays as it was (no flapping at the edge). Unknown radius: physical with a model.
                    {   const bool modelLoaded = ref && FarHas3D(ref);
                        const double d = managerHavePlayer && ref ? std::hypot(double(position.x) - managerPlayer.x, double(position.y) - managerPlayer.y) : 0.0;
                        const auto was = car.advice;
                        Actuator next = was;
                        if (!modelLoaded) next = Actuator::Virtual;
                        else if (!(managerRadius > 0)) next = Actuator::Physical;
                        else if (d < managerRadius - kManagerBandM) next = Actuator::Physical;
                        else if (d > managerRadius) next = Actuator::Virtual;
                        else if (!car.adviceAt) next = Actuator::Physical;
                        if (next != Actuator::Virtual) car.seeded = false;  // the rail path restarts from wherever the car is next time
                        car.advice = next; car.adviceAt = now; car.toPlayer = d;
                        if (next != was || !car.modeSince)
                            logger::Write("OBS system=traffic.manager vehicle=%08X driver=%08X event=advice to=%s from=%s distance=%.0f physics_radius=%.0f model=%d",
                                id, driver, ActuatorName(next), ActuatorName(was), d, managerRadius, int(modelLoaded));
                    }
                    car.mode = playerTook || (currentDriver && currentDriver != driver) ? fleetrail::Mode::Awaiting :
                        chassisReady ? fleetrail::Mode::Auto : movedVirtually ? fleetrail::Mode::Rail : fleetrail::Mode::Awaiting;
                } else if (playerTook || (currentDriver && currentDriver != driver) || vehiclediag::BirthHeld(id) || HasPendingFarDispatch(id)) {
                    if (vehiclediag::BirthHeld(id)) archproof::State().railBlockedBirthHold.fetch_add(1, std::memory_order_relaxed);
                    car.railOwner = false;
                    car.mode = fleetrail::Mode::Awaiting;
                } else {
                    // Authoritative SimTier from once-per-frame FrameContext (semantic loadedColliderMetres <= 160 m).
                    const auto* ctxEntry = simtier::Context().Find(id);
                    const double farRadius = double(game::ClampLoadedColliderMetres(float(managerRadius)));
                    const double d = ctxEntry ? ctxEntry->distM
                        : (managerHavePlayer && ref ? std::hypot(double(position.x) - managerPlayer.x, double(position.y) - managerPlayer.y) : 0.0);
                    const simtier::Tier tier = ctxEntry ? ctxEntry->tier
                        : simtier::Decide(car.railOwner ? simtier::Tier::FarRail : simtier::Tier::NearPhysical, d, ref != 0, farRadius, kManagerBandM);
                    const bool beyondPhysics = tier == simtier::Tier::FarRail;
                    const bool insidePhysics = tier == simtier::Tier::NearPhysical && (!car.railOwner || d < farRadius - kManagerBandM);
                    if (insidePhysics) {
                        if (car.railOwner) {
                            NoteFleetHandoff(id, car.railSpeed, now);
                            logger::Write("OBS system=traffic.fleet_rail event=exit_ramp vehicle=%08X rail_speed=%.1f ramp_ms=%llu distance=%.0f",
                                id, car.railSpeed, fleetrail::rules::kRampMs, d);
                        }
                        car.railOwner = false;
                        car.seeded = false;
                        car.position = position;
                        car.station = station;
                        car.mode = chassisReady ? fleetrail::Mode::Auto : fleetrail::Mode::Awaiting;
                    } else if (beyondPhysics || car.railOwner) {
                        if (chassisReady && car.path.End() > 1.0) {
                            if (!car.railOwner || !car.seeded) {
                                car.station = station;
                                car.position = position;
                                car.seeded = true;
                                car.blendReady = false;
                                logger::Write("OBS system=traffic.fleet_rail event=rail_enter vehicle=%08X driver=%08X distance=%.0f far_radius=%.0f station=%.1f speed=%.1f",
                                    id, driver, d, farRadius, car.station, car.speed);
                            }
                            car.railOwner = true;
                            car.mode = fleetrail::Mode::Rail;
                            // Keep far-gliding chassis level and aligned with its lane tangent on the main thread.
                            if (ref) {
                                const auto pNow = car.path.At(car.station);
                                const auto pNext = car.path.At(car.path.Advance(car.station, 2.0));
                                const double tdx = pNext.x - pNow.x, tdy = pNext.y - pNow.y;
                                if (std::hypot(tdx, tdy) > 0.1) FarLevelYaw(ref, std::atan2(tdx, tdy));
                            }
                        } else if (ref && !FarHas3D(ref) && !railFresh && !driveFresh && car.path.End() > 1.0 &&
                                   hereKnown && !(here.cellAttached && !here.crossCell) &&
                                   (car.mode == fleetrail::Mode::Reference || (car.mode == fleetrail::Mode::Awaiting && car.modeSince && now - car.modeSince >= 5000))) {
                            if (car.mode != fleetrail::Mode::Reference) {
                                if (!car.railOwner || !car.seeded) { car.station = station; car.position = position; car.seeded = true; }
                                if (!(car.railSpeed > 0.0)) car.railSpeed = car.speed;
                                car.refStepAt = now; car.blendReady = false;
                            }
                            car.railOwner = true;
                            car.mode = fleetrail::Mode::Reference;
                            if (now - car.refStepAt >= fleetrail::rules::kReferenceStepMs) {
                                const double dt = std::min(1.0, double(now - car.refStepAt) / 1000.0);
                                const double want = std::min(car.speed, car.path.CornerSpeed(car.station));
                                const double v = fleetrail::Approach(car.railSpeed, want, dt);
                                const double s = car.path.Advance(car.station, v * dt);
                                const auto target = car.path.At(s);
                                if (loadedarea::StepReference(id, driver, float(target.x), float(target.y), float(target.z))) {
                                    car.station = s; car.railSpeed = v; car.position = target; car.refStepAt = now;
                                }
                            }
                        } else {
                            if (!(car.path.End() > 1.0)) archproof::State().railBlockedNoPath.fetch_add(1, std::memory_order_relaxed);
                            else if (!chassisReady) archproof::State().railBlockedNoChassis.fetch_add(1, std::memory_order_relaxed);
                            car.railOwner = false;
                            car.mode = fleetrail::Mode::Awaiting;
                        }
                    } else {
                        // In the hysteresis band (farRadius - 20 .. farRadius) while in Auto: stay Auto.
                        car.railOwner = false;
                        car.mode = chassisReady ? fleetrail::Mode::Auto : fleetrail::Mode::Awaiting;
                    }
                }
                if (car.railOwner && currentDriver == driver && instance && body) {
                    if (!sameBody) { car.lastStep = 0; car.fault = ""; }
                    car.instance = instance;
                    car.body = body;
                } else if (car.railOwner && (!ref || currentDriver != driver || !instance || !body)) {
                    car.instance = 0;
                    car.body = 0;
                }
                if (oldMode != car.mode) car.modeSince = now;
                if (oldMode != car.mode)
                    logger::Write("OBS system=traffic.fleet_rail event=mode vehicle=%08X driver=%08X mode=%d current_attached=%d ahead_attached=%d chassis_ready=%d",
                        id, driver, int(car.mode), hereKnown && here.cellAttached, nextKnown && next.cellAttached, chassisReady);
            }
#ifndef FVO_RELEASE
            static std::uint64_t lastSummary = 0;
            if (now - lastSummary >= 10000) {
                lastSummary = now;
                int nAuto = 0, nRail = 0, nAwait = 0, nRef = 0;
                {
                    std::lock_guard lk(g_fleetRailLock);
                    for (const auto& [vid, c] : g_fleetRails) {
                        if (c.mode == fleetrail::Mode::Auto) ++nAuto;
                        else if (c.mode == fleetrail::Mode::Rail) ++nRail;
                        else if (c.mode == fleetrail::Mode::Reference) ++nRef;
                        else ++nAwait;
                    }
                }
                int activePilots = 0, moving = 0, askingStalled = 0, holding = 0;
                for (int slot = 1; slot < kPilots; ++slot) {
                    if (!g_gs[slot].active) continue;
                    const auto vid = g_jobs[slot].vehicleId;
                    if (FleetRailOwns(vid)) continue;
                    ++activePilots;
                    const auto& rv = g_shs[slot].v;
                    const double spd = std::hypot(double(rv[0]), double(rv[1]));
                    const auto& ao = g_gs[slot].adOut;
                    if (spd >= 0.5) ++moving;
                    else if (ao.speed >= 0.5 && !ao.brake) ++askingStalled;
                    else ++holding;
                }
                logger::Write("OBS system=traffic.fleet_summary rail=%d auto=%d awaiting=%d ref=%d near_pilots=%d moving=%d asking_stalled=%d holding=%d",
                    nRail, nAuto, nAwait, nRef, activePilots, moving, askingStalled, holding);
                archproof::State().railActive.store(std::uint32_t(nRail + nRef), std::memory_order_relaxed);
            }
#endif
        }

		// Descend before the drop. A stop for an autopilot-flown vehicle (every NPC pilot; the player's own vehicle while Auto, follow
		// or a call flies it, never the player's manual flight) that is more than kDropMaxAbove over known ground is held: the
		// vehicle hovers straight down over where it is, and the stop happens once it is down (or after kSettleMaxMs). Reasons that
		// must end the flight at once are not held (the vehicle gone or unloaded, a wreck, the player taking it, a flip, landed).
		bool Settle(int idx, const char* why)
		{
			State& g = g_gs[idx];
			if (g.settleForce) { g.settleForce = false; g.settle = false; return false; }
			static const char* const kNow[] = { "landed", "vehicle lost", "rover lost", "unloaded", "the player took", "flipped", "autopilot off", "wreck", "died",
				"world", "load", "reset", "board", "player got in", "you drive", "manual", "recall", "too far", "follower drives" };
			for (const char* k : kNow) if (why && std::strstr(why, k)) { g.settle = false; return false; }
			if (g.settle) return true;  // already descending: the stop it waits for is the first one
			if (!g.active || (!idx && !g.autopilot)) return false;  // not flying, or the player flies it by hand
			const auto vehicle = idx ? RefById(g_jobs[idx].vehicleId) : RefById(g_roverId);
			if (!vehicle || vehiclehits::Wrecked(At<std::uint32_t>(vehicle, off::kFormId))) return false;
			const double above = PilotAboveGround(idx, vehicle);
			if (std::isfinite(above) && above <= kDropMaxAbove) return false;
			// Unknown height: set down anyway (SettleTick sinks at 3 m/s until a reading says it is down; 30 m assumed for its time limit).
			// (It returned false here: the stop dropped the vehicle wherever it was, 2026-10-29 14:40 run.)
			const auto p = At<game::Vec3>(vehicle, off::kRefPosition);
			g.settle = true; g.settleX = p.x; g.settleY = p.y; g.settleFrom = std::isfinite(above) ? above : 30.0; g.settleAt = GetTickCount64();
			std::snprintf(g.settleWhy, sizeof(g.settleWhy), "%s", why ? why : "stop");
			if (std::isfinite(above)) logger::Write("%s: %.1f m up: descending before it stops (%s)", idx ? Who(idx) : "Rover call", above, g.settleWhy);
			else logger::Write("%s: height over the ground unknown: descending before it stops (%s)", idx ? Who(idx) : "Rover call", g.settleWhy);
			return true;
		}
		void SettleTick(int idx, std::uintptr_t vehicle)
		{
			State& g = g_gs[idx];
			const double above = PilotAboveGround(idx, vehicle);
			const bool down = std::isfinite(above) && above <= kDropMaxAbove;
			if (down || GetTickCount64() - g.settleAt > kSettleBaseMs + ULONGLONG(g.settleFrom * 200.0)) {
				char why[64];
				std::snprintf(why, sizeof(why), "%s", g.settleWhy);
				logger::Write("%s: %s; stopping (%s)", idx ? Who(idx) : "Rover call", down ? "down" : "descent took too long", why);
				g.settleForce = true;
				Stop(idx, why);
				return;
			}
			// Straight down: a commanded sink rate with the brakes on (a hold goal right over the vehicle counted as "arrived"
			// and the height never changed, 2026-10-28 18:28 run: 36 m up for 25 s).
			Command c;
			c.set = Command::kVertical | Command::kBrake | Command::kThrottle | Command::kScripted;  // scripted: no rollout veto on the sink
			c.vertical = -std::clamp(std::isfinite(above) ? (above - kDropMaxAbove) * 0.25 : 3.0, 2.0, 12.0);  // faster high up, gentle near the ground
			c.brake = true; c.throttle = 0; c.holdMs = 300;
			g_cmds[idx].c = c;
			g_cmds[idx].until = GetTickCount64() + c.holdMs;
			Goal goal;
			goal.x = g.settleX; goal.y = g.settleY;
			goal.z = std::numeric_limits<double>::quiet_NaN();
			goal.stop = 1.5; goal.hold = true; goal.keepGoing = true;
			Fly(idx, vehicle, goal, false);
		}

		// NPC pilots: every frame while hovering, the same frame logic as the player's autopilot.
		void PilotsTick()
		{
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			const auto driven = DrivenBy(player);
			// 2026-10-24 CPU fix (the user: "make flight and navigation cheaper"): NPC planning ran every update (40-50/s).
			// Fly measures its own dt (clamped to 0.1 s) and the physics hook keeps applying the last targets, so 20 Hz is
			// enough. A vehicle with no physics instance cannot move at all: plan it at 3 Hz until its body binds.
			static ULONGLONG flownAt[kPilots] = {}, unboundSince[kPilots] = {};
			static unsigned slowed = 0, flown = 0, tierCount[3] = {}; static ULONGLONG reportAt = 0;
			const ULONGLONG now = GetTickCount64();
			// Planning tiers (2026-10-29, the user: 64 vehicles, "find a way for it to work and still be performant"): the kNearPilots
			// nearest pilots within kNearM plan at 20 Hz as before; others within kMidM at 10 Hz, the rest at 5 Hz. Planning and its probes
			// both follow (Fly asks them); the physics hook keeps applying the last targets in between.
			constexpr int kNearPilots = 10;
			constexpr double kNearM = 150.0, kMidM = 300.0;
			static double dist[kPilots];
			static ULONGLONG tierEvery[kPilots];
			static Goal holdGoal[kPilots];
			static ULONGLONG holdSince[kPilots], seenAt[kPilots], dangerAt[kPilots];
			static bool wasRelaxed[kPilots];
			static unsigned held = 0, straightCount = 0, outOfView = 0;
			// The camera's heading and its turn rate (smoothed over ~0.15 s), read each tick: the view leads where it is turning.
			static double camYawLast = 0, yawRate = 0;
			static ULONGLONG camAt = 0;
			double camYaw = 0;
			const bool haveCam = player && CameraHeading(camYaw);
			if (haveCam) {
				if (camAt && now > camAt) {
					const double dtc = std::min(0.25, double(now - camAt) / 1000.0);
					yawRate += (Wrap(camYaw - camYawLast) / dtc - yawRate) * std::min(1.0, dtc / 0.15);
				}
				camYawLast = camYaw; camAt = now;
			}
			else { yawRate = 0; camAt = 0; }
			const double lead = std::clamp(yawRate * kViewLeadS, -kViewLeadMax, kViewLeadMax);
			const auto ppos = player ? At<game::Vec3>(player, off::kRefPosition) : game::Vec3{};
			int order[kPilots], nActive = 0;
			{
				const auto pp = player ? At<game::Vec3>(player, off::kRefPosition) : game::Vec3{};
				for (int idx = 1; idx < kPilots; ++idx) {
					dist[idx] = 1e18;
					if (!g_gs[idx].active) { flownAt[idx] = unboundSince[idx] = seenAt[idx] = dangerAt[idx] = 0; g_straight[idx] = g_farHold[idx] = wasRelaxed[idx] = false; continue; }
					if (const auto* ce = simtier::Context().Find(g_jobs[idx].vehicleId)) {
						dist[idx] = ce->distM;
					} else if (const auto v = g_shs[idx].vehicle.load()) {
						const auto vp = At<game::Vec3>(v, off::kRefPosition);
						dist[idx] = std::hypot(double(vp.x) - pp.x, double(vp.y) - pp.y, double(vp.z) - pp.z);
					}
					order[nActive++] = idx;
				}
				std::sort(order, order + nActive, [&](int a, int b) { return dist[a] < dist[b]; });
				for (int k = 0; k < nActive; ++k) {
					const int idx = order[k];
					const double d = dist[idx];
					const bool hasObs = dangerAt[idx] != 0 && now - dangerAt[idx] < 2000;
					tierEvery[idx] = simtier::PlanIntervalMs(k, d, true, hasObs, kNearPilots);
					g_ageScale[idx] = std::max(1u, unsigned(tierEvery[idx] / 50));
				}
			}
			// Process the nearest kNearPilots first so cars right beside the player never wait behind distant ones,
			// then round-robin the remaining active pilots within the 1.8 ms frame budget.
			static int rrPilot = 0;
			const int nearCount = std::min(nActive, kNearPilots);
			const int farCount = nActive - nearCount;
			LARGE_INTEGER ptFreq{}, ptStart{}, ptNow{};
			QueryPerformanceFrequency(&ptFreq);
			QueryPerformanceCounter(&ptStart);
			constexpr double kPilotsBudgetMs = 1.8;
			int pilotsRunThisFrame = 0;
			for (int step = 0; step < nActive; ++step) {
				const int idx = step < nearCount ? order[step] : order[nearCount + ((rrPilot + (step - nearCount)) % farCount)];
				if (!g_gs[idx].active) continue;
				const auto id = g_jobs[idx].vehicleId;
                if (FleetRailOwns(id)) { flownAt[idx] = 0; continue; }
#ifndef FVO_RELEASE
                if (reloc::RailOwns(id)) { flownAt[idx] = 0; continue; }
#endif
				const auto vehicle = RefById(id);
				const auto loaded = vehicle ? At<std::uintptr_t>(vehicle, off::kRefLoadedData) : 0;
				if (!vehicle || vehicle != g_shs[idx].vehicle.load()) Stop(idx, "vehicle lost");
				else if (vehicle == driven) Stop(idx, "the player took the vehicle");
				else if (!loaded || !At<std::uintptr_t>(loaded, 0x08)) Stop(idx, "vehicle unloaded");
				else {
					if (g_shs[idx].instance.load()) unboundSince[idx] = 0;
					else if (!unboundSince[idx]) unboundSince[idx] = now;
					const bool unbound = unboundSince[idx] && now - unboundSince[idx] > 3000;
					// Far pilots (see kHoldM): the hold aims a little ahead, where the vehicle can stop, at its own height.
					State& g = g_gs[idx];
					const double d = player ? dist[idx] : 0.0;  // no player (a load): nobody is far
					const bool lane = PilotNavOf(idx).trafficLane && !g.settle && !g.drive && !npcvehicles::PilotFighting(idx);
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					const auto& rv = g_shs[idx].v;
					const double sp = std::hypot(double(rv[0]), double(rv[1]));
                    // A population must not stop at a player-centred radius.
                    // Keep distance/view planning tiers, but continue the trip.
                    g_farHold[idx]=false;

					// Vision tiers (see kViewHalf): in view, or no camera reading, plans as before.
					bool viewed = true;
					if (haveCam) {
						const double a = Wrap(std::atan2(double(vp.x) - ppos.x, double(vp.y) - ppos.y) - camYaw);
						const bool in = lead >= 0 ? (a >= -kViewHalf && a <= lead + kViewHalf) : (a >= lead - kViewHalf && a <= kViewHalf);
						if (in) seenAt[idx] = now;
						viewed = seenAt[idx] && now - seenAt[idx] < kViewKeepMs;
					}
					bool relaxed = false, nearOut = false;
					// PF-1 (2026-11-21): a near car (kNearKeepM..kNearM) out of view and safe plans at 10 Hz, not 20 (17:36 run: 78% of the
					// passes were near cars at 20 Hz); in view, within kNearKeepM, or crowded it stays at 20 Hz.
					if (!viewed && !unbound && !g_farHold[idx] && d >= kNearKeepM && lane) {
						const auto& o = g.adOut;
						bool safe = !g.liftoff && !g.landing && o.phase == drive::Phase::Travel && now - g_bumps[idx].ev >= kBumpRecentMs;
						for (unsigned k = 0; safe && g_vsnapOk && k < g_vsnap.count && k < world::kMaxVehicles; ++k) {
							const auto& w = g_vsnap.v[k];
							if (w.ref == vehicle) continue;
							const double reach = kCrowdM + kCrowdLeadS * (sp + std::hypot(double(w.vel[0]), double(w.vel[1])));
							if (std::hypot(double(w.pos[0]) - vp.x, double(w.pos[1]) - vp.y) < reach && std::fabs(double(w.pos[2]) - vp.z) < 15.0) safe = false;
						}
						if (!safe) dangerAt[idx] = now;
						const bool calm = safe && now - dangerAt[idx] >= kDangerHoldMs;
						relaxed = calm && d >= kNearM;
						nearOut = calm && d < kNearM && tierEvery[idx] <= 50;
					}
					if (wasRelaxed[idx] && !(relaxed || nearOut)) flownAt[idx] = 0;  // a failed check or coming into view: plan this tick, not at the end of the slow interval
					wasRelaxed[idx] = relaxed || nearOut;
					g_straight[idx] = relaxed && d >= kMidM;  // only safe, unseen traffic flies straight (past kHoldM a traffic pilot is held)
					ULONGLONG every = unbound ? 333 : g_farHold[idx] ? (sp > 2.0 ? 200 : 1000) : tierEvery[idx] ? tierEvery[idx] : 50;
					if (relaxed) every = std::max(every, d < kMidM ? kOutMidMs : kOutFarMs);
					else if (nearOut) every = std::max(every, kNearOutMs);
					g_ageScale[idx] = unsigned(std::max<ULONGLONG>(every, 50) / 50);  // probe age limit follows the interval actually used
					if (HandsOn(idx) && !g.settle) {  // your hands: every frame, through the player's own hover model
						flownAt[idx] = now;
						g.gear = std::clamp(g_handsGear.load(), 1, 4);
						g.autopilot = false;
						Fly(idx, vehicle, g_jobs[idx].goal, Focused());
						g.autopilot = true;  // the job, its trip and the command channel stay the autopilot's
						continue;
					}
					if (flownAt[idx] && now - flownAt[idx] < every) continue;
					if (pilotsRunThisFrame >= 2) {
						QueryPerformanceCounter(&ptNow);
						if (1000.0 * double(ptNow.QuadPart - ptStart.QuadPart) / double(ptFreq.QuadPart) >= kPilotsBudgetMs) {
							++framebudget::Main().pilotsCapped;
							if (step >= nearCount && farCount > 0) rrPilot = (rrPilot + (step - nearCount)) % farCount;
							break;
						}
					}
					flownAt[idx] = now;
					++pilotsRunThisFrame;
					if (unbound) ++slowed; else { ++flown; ++tierCount[every <= 50 ? 0 : every <= 100 ? 1 : 2];
						++costparts::g_pass[every <= 50 ? 0 : every <= 100 ? 1 : 2]; if (nearOut) ++costparts::g_passNearOut; if (relaxed) ++costparts::g_passFarOut; }
					if (g_farHold[idx]) ++held; else if (relaxed) ++outOfView;
					if (g_straight[idx]) ++straightCount;
					if (g.settle) { SettleTick(idx, vehicle); continue; }
					Fly(idx, vehicle, g_farHold[idx] ? holdGoal[idx] : g_jobs[idx].goal, false);
				}
			}
			if (now - reportAt >= 60000) {
				if (slowed || flown) logger::Write("NPC flight planning: %u passes (%u near at 20 Hz, %u mid at 10 Hz, %u far at 5 Hz or slower; %u out of view at 2-4 Hz, %u flying straight, %u held far from you), %u slowed passes (no physics instance) in the last minute",
					flown, tierCount[0], tierCount[1], tierCount[2], outOfView, straightCount, held, slowed);
				reportAt = now; slowed = flown = held = straightCount = outOfView = 0; tierCount[0] = tierCount[1] = tierCount[2] = 0;
			}
		}
	}

	bool PilotFly(int pilot, std::uintptr_t vehicle, double x, double y, double stop, unsigned timeoutMs, bool keepGoing, double z)
	{
		if (pilot < 1 || pilot >= kPilots || !vehicle) return false;
		if (g_gs[pilot].settle) {
			if (g_jobs[pilot].vehicleId == At<std::uint32_t>(vehicle, off::kFormId)) {
				g_gs[pilot].settle = false;  // flying on: the descent before the stop is cancelled
				logger::Write("%s: descent cancelled (a new flight)", Who(pilot));
			}
			else {  // this pilot is wanted for another vehicle: the descending one stops now
				char why[64];
				std::snprintf(why, sizeof(why), "%s", g_gs[pilot].settleWhy);
				g_gs[pilot].settleForce = true;
				Stop(pilot, why);
			}
		}
		const std::uint32_t id = At<std::uint32_t>(vehicle, 0x28);
		if (vehicle == g_hoverVehicle.load() ||
            (id == g_roverId && npcvehicles::CrewOrderOf(id) == npcvehicles::CrewOrder::None &&
             !trafficpool::AdoptionPilotAllowed(id, npcvehicles::VehicleDriverId(id)))) return false;
		if (IsDisabled(id)) return false;  // the hit counter switched its autopilot off
		// 2026-10-28 (01:29 run): the player took an NPC job's vehicle and the job restarted the flight 1764 times ("the player
		// took the vehicle" each frame). The vehicle the player sits in is refused.
		if (const auto player = Ptr(addr::kPlayerCharacterPtr); player && PlayerVehicle(player) == vehicle && passenger::RidingVehicle() != id) return false;  // a passenger player rides with the NPC driver
		// 2026-10-27 crash (21:18 run): after a world change an NPC job restarted the flight on its unloaded vehicle every
		// 20 ms (hover on, "vehicle unloaded", hover off, again) until the game died. An unloaded vehicle is refused.
		{
			const auto loaded = At<std::uintptr_t>(vehicle, off::kRefLoadedData);
			if (!loaded || !At<std::uintptr_t>(loaded, 0x08)) return false;
		}
		for (int i = 1; i < kPilots; ++i) {
			if (i != pilot && g_gs[i].active && g_jobs[i].vehicleId == id) return false;  // one pilot per vehicle
		}
		auto& job = g_jobs[pilot];
		job.goal.x = x;
		job.goal.y = y;
		job.goal.stop = stop;
		job.goal.keepGoing = keepGoing;
		job.goal.timeoutMs = timeoutMs;
		job.goal.z = z;
        job.goal.hold=false;job.goal.cruise=false;
        {   goaldoor::Goal dg;dg.source=goaldoor::Source::Job;dg.x=x;dg.y=y;dg.z=z;dg.stop=stop;dg.keepGoing=keepGoing;dg.identity=id;dg.at=double(GetTickCount64())/1000.0;
            dg.ending=goaldoor::JobEnding(false,false,false,keepGoing);
            if(g_door.Admit(pilot,dg))logger::Observe("plan.door",id,std::uint64_t(pilot),"event=admit pilot=%d source=job ending=%s at=%.1f,%.1f reason=pilotfly driven_by=old_autopilot_no_route_line_yet",pilot,goaldoor::Name(dg.ending),x,y);}
		State& g = g_gs[pilot];
		if (g.active && job.vehicleId == id) return true;  // a new target for the same flight
		if (g.active) Stop(pilot, "another vehicle");
		job.vehicleId = id;
		Start(pilot, vehicle);
		g.autopilot = true;
		g.autopilotSince = GetTickCount64();
		g.gear = 2;
		const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
		logger::Write("%s: %08X autopilot to (%.0f, %.0f), %.0f m away, stop %.0f m", Who(pilot), id, x, y, std::hypot(x - pos.x, y - pos.y), stop);
		return true;
	}

	bool PilotFly(int pilot, std::uintptr_t vehicle, const vcontract::Goal& goal, unsigned timeoutMs, bool keepGoing)
	{
		PilotNav(pilot, goal.nav);
        if(goal.hold&&goal.nav.landOnArrival&&!PilotActive(pilot)&&AtDestination(vehicle,goal.x,goal.y,goal.z,goal.stop))return true;
        if(!PilotFly(pilot,vehicle,goal.x,goal.y,goal.stop,timeoutMs,keepGoing,goal.z))return false;
        {   // The goal door (goaldoor.h): the same goal, with the ending its own flags ask for (adapter; the old autopilot still flies it).
            goaldoor::Goal dg;dg.source=goaldoor::Source::Job;dg.x=goal.x;dg.y=goal.y;dg.z=goal.z;dg.stop=goal.stop;dg.keepGoing=keepGoing;
            dg.ending=goaldoor::JobEnding(goal.hold,goal.nav.landOnArrival,goal.nav.passThrough,keepGoing);dg.identity=g_jobs[pilot].vehicleId;dg.at=double(GetTickCount64())/1000.0;
            if(g_door.Admit(pilot,dg))logger::Write("OBS system=plan.door vehicle=%08X pilot=%d event=admit source=job ending=%s at=%.1f,%.1f reason=pilotfly_contract driven_by=old_autopilot_no_route_line_yet",
                unsigned(dg.identity),pilot,goaldoor::Name(dg.ending),dg.x,dg.y);}
        // Preserve the complete shared goal contract, including braking and pass-through semantics.
        g_jobs[pilot].goal.hold=goal.hold;
        g_jobs[pilot].goal.cruise=goal.cruise||goal.nav.passThrough;
        g_jobs[pilot].goal.cruiseSpeed=goal.cruise ? goal.cruiseSpeed : goal.nav.speedLimit>0 ? goal.nav.speedLimit : kAiGearTop[4];
        return true;
	}

	bool PilotActive(int pilot) { return pilot >= 1 && pilot < kPilots && g_gs[pilot].active; }
	bool PilotHeldFar(int pilot) { return pilot >= 1 && pilot < kPilots && g_gs[pilot].active && g_farHold[pilot]; }
	double PilotAboveGround(int pilot, std::uintptr_t vehicle)
	{
		if (pilot < 0 || pilot >= kPilots || !vehicle) return std::numeric_limits<double>::quiet_NaN();
		const State& s = g_gs[pilot];
		const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
		if (!s.haveGround || GetTickCount64() - s.groundAt > 1500) {
			// No fresh ground reading (a slow tier, a paused game, unmapped ground; 2026-10-29: such stops dropped vehicles from up to
			// 86 m): the shared nav map's ground there, then the cell's baked terrain.
			const auto m = navmap::At(vp.x, vp.y, vp.z);
			if (m.haveGround && std::isfinite(m.ground) && m.ground <= vp.z + 1.0f) return double(vp.z) - m.ground;
			double baked = 0;
			if (cellbake::Ground(vp.x, vp.y, baked) && baked <= vp.z + 1.0) return double(vp.z) - baked;
			return std::numeric_limits<double>::quiet_NaN();
		}
		return double(vp.z) - s.ground;
	}
	PhysicsStatus PilotPhysics(int pilot)
	{
		if(pilot<1||pilot>=kPilots||!g_gs[pilot].active)return {};
		auto& s=g_shs[pilot];std::lock_guard lk(s.lock);
		PhysicsStatus out;const auto instance=s.instance.load();
		out.bound=instance!=0;out.serial=s.stepSerial;
		const auto now=GetTickCount64();
		out.live=s.active&&s.autopilot&&s.matched&&instance&&s.lastStepInstance==instance&&s.lastStepAt&&now-s.lastStepAt<500;
		return out;
	}

	PhysicsStatus HandoffPhysics(std::uint32_t id)
    {
        for(int i=1;i<kPilots;++i)if(g_jobs[i].vehicleId==id&&g_gs[i].autopilot&&
            (g_gs[i].active||g_driveOut[i].Active())){
            auto& s=g_shs[i];std::unique_lock lock(s.lock,std::try_to_lock);
            if(!lock.owns_lock())return {};
            const auto instance=s.instance.load();
            const auto now=GetTickCount64();
            const bool bound=s.vehicle.load()&&(!s.active||s.sceneCount>0);
            const bool sameBody=bodybinding::Contains({s.sceneBodies,s.sceneCount},s.handoffStepBody)&&
                (!instance||s.handoffStepInstance==instance);
            return {bound,sameBody&&s.handoffStepAt&&now-s.handoffStepAt<1500,s.handoffSteps};
        }
        return {};
    }

#ifndef FVO_RELEASE
    namespace relocate {
        const char* Arm(std::uint32_t vehicleId,double dx,double dy,double dz,std::uint64_t serial,std::uint64_t authority)
        {
            // One write is one bounded hop (relocation.h kMaxHopM = 20 m; 25 m allows the measured-position correction). Longer moves are several
            // hops, each read back before the next: the Coordinator in npcvehicles.cpp plans them.
            if (!vehicleId || !serial || !std::isfinite(dx + dy + dz) || std::sqrt(dx * dx + dy * dy + dz * dz) > 25.0) return "bad_request";
            if (FleetRailOwns(vehicleId) || reloc::RailOwns(vehicleId)) return "rail_owns_chassis";
            for (int i = 1; i < kPilots; ++i) if (g_jobs[i].vehicleId == vehicleId) {
                auto& s = g_shs[i];
                const auto instance = s.instance.load(); const auto body = s.bodyId.load();
                if (!instance || !body) return "no_physics_instance";
                auto& st = reloc::g_state; std::lock_guard lock(st.lock);
                if (st.armed.load() || st.ready.load()) return "busy";
                st.serial = serial; st.authority=authority; st.vehicleId = vehicleId; st.bodyId = body; st.instance = instance; st.dx = dx; st.dy = dy; st.dz = dz;
                st.facts = Facts{}; st.lastActionAt = 0;
                st.armed.store(true, std::memory_order_release);
                return nullptr;
            }
            return "no_pilot_slot";
        }
        bool Take(std::uint64_t serial, Facts& out)
        {
            auto& st = reloc::g_state; std::lock_guard lock(st.lock);
            if (serial != st.serial || !st.ready.load(std::memory_order_acquire)) return false;
            out = st.facts; return true;
        }
        bool Read(std::uint64_t serial, Chassis& out)
        {
            auto& st = reloc::g_state; std::lock_guard lock(st.lock);
            if (serial != st.serial || !st.ready.load(std::memory_order_acquire) || !st.facts.issued) return false;
            out = Chassis{};
            out.bodyValid = reloc::ReadBody(st.instance, st.bodyId, out.pos);
            out.live = st.lastActionAt && GetTickCount64() - st.lastActionAt < 500;
            out.completedSteps = st.facts.actionCalls > 0 ? st.facts.actionCalls - 1 : 0;   // an action call only starts after the previous world step finished
            return true;
        }
        void Disarm(std::uint64_t serial)
        {
            auto& st = reloc::g_state; std::lock_guard lock(st.lock);
            if (serial != st.serial) return;
            st.armed.store(false); st.ready.store(false); st.serial = 0; st.instance = 0; st.bodyId = 0; st.vehicleId = 0;
        }

        // ---- Far tier rail (diagnostic) ----
        const char* RailStart(std::uint32_t vehicleId, double x0, double y0, double dirX, double dirY, double s0, double z0, double speed, double ride)
        {
            if (!vehicleId || !std::isfinite(x0 + y0 + dirX + dirY + s0 + z0 + speed + ride) || !(speed >= 0.0) || speed > 60.0) return "bad_request";
            if (FleetRailOwns(vehicleId)) return "fleet_rail_owns_chassis";
            const double len = std::hypot(dirX, dirY);
            if (len < 0.5) return "bad_direction";
            for (int i = 1; i < kPilots; ++i) if (g_jobs[i].vehicleId == vehicleId) {
                auto& s = g_shs[i];
                const auto instance = s.instance.load(); const auto body = s.bodyId.load();
                if (!instance || !body) return "no_physics_instance";
                auto& hop = reloc::g_state;
                if (hop.armed.load() || hop.ready.load()) return "busy_hop_doer";
                auto& r = reloc::g_rail; std::lock_guard lock(r.lock);
                if (r.active.load()) return "rail_already_active";
                r.vehicleId = vehicleId; r.bodyId = body; r.instance = instance;
                r.p = farrail::RailParams{}; r.p.x0 = x0; r.p.y0 = y0; r.p.dirX = dirX / len; r.p.dirY = dirY / len; r.p.speed = speed; r.p.ride = ride;
                r.st = farrail::RailState{}; r.st.s = s0; r.st.z = z0;
                r.facts = RailFacts{}; r.facts.active = true; r.facts.s = s0; r.facts.z = z0;
                r.active.store(true, std::memory_order_release);
                return nullptr;
            }
            return "no_pilot_slot";
        }
        void RailStop()
        {
            auto& r = reloc::g_rail; std::lock_guard lock(r.lock);
            r.active.store(false, std::memory_order_release); r.instance = 0; r.bodyId = 0; r.vehicleId = 0; r.facts.active = false;
        }
        bool RailRead(RailFacts& out)
        {
            auto& r = reloc::g_rail; std::lock_guard lock(r.lock);
            out = r.facts; out.active = r.active.load(std::memory_order_acquire);
            return r.vehicleId != 0 || out.steps != 0 || out.faults != 0;
        }
        void RailSetTerrain(int index, double z) { reloc::g_rail.profile.Set(index, z); }
        void RailClearTerrain() { reloc::g_rail.profile.Clear(); }
        int RailTerrainKnown() { return reloc::g_rail.profile.KnownCount(); }
        bool ReadChassisNow(std::uint32_t vehicleId, ChassisNow& out)
        {
            out = ChassisNow{};
            for (int i = 1; i < kPilots; ++i) if (g_jobs[i].vehicleId == vehicleId) {
                auto& s = g_shs[i];
                const auto instance = s.instance.load(); const auto body = s.bodyId.load();
                if (!instance || !body) return false;
                out.bound = true;
                out.bodyValid = reloc::ReadBodyV(instance, body, out.pos, out.vel);
                return out.bodyValid;
            }
            return false;
        }
    }

    bool FactoryMotionProved(int pilot, unsigned vehicle, std::uint64_t lease)
    {
        if (pilot < 1 || pilot >= kPilots || !lease || g_spotVehicle[pilot] != vehicle ||
            g_factoryTest[pilot].lease != lease) return false;
        const auto& proof = g_spotMotion[pilot].proof;
        if (!proof.completed || vehiclediag::BuiltAt(vehicle) != proof.builtAt) return false;
        const bool stock = npcvehicles::FactoryStockLeaseOf(pilot, vehicle, false) == lease;
        auto& binding = g_shs[pilot];
        std::lock_guard lock(binding.lock);
        if (binding.instance.load()) return binding.bodyId.load() == proof.body;
        return stock && binding.handoffStepBody == proof.body &&
            bodybinding::Contains({binding.sceneBodies, binding.sceneCount}, proof.body);
    }

    void RebaseFactoryMotionAfterTransfer(int pilot,unsigned vehicle,std::uint64_t lease)
    {
        if(pilot<1||pilot>=kPilots||g_jobs[pilot].vehicleId!=vehicle||!lease||
           npcvehicles::FactoryStockLeaseOf(pilot,vehicle,false)!=lease)return;
        if(g_spotVehicle[pilot]==vehicle){
            g_spot[pilot]={};
            g_spotMotion[pilot].Reset();
            g_factoryTest[pilot]={};
            g_testActive[pilot]=false;
            if(g_stockPulseOwner==lease)g_stockPulseOwner=g_stockPulseAt=0;
        }
        {
            std::lock_guard lock(g_spotLock);
            g_spotProof.erase(vehicle);g_spotResult.erase(vehicle);g_spotRequest.erase(vehicle);
            g_healthSample.erase(vehicle);g_selfReport.erase(vehicle);
        }
        g_probeRecent[pilot]={};
        // The car leaves the lab float here: from now on the ordinary hover-or-drive rules of its route choose hover or wheels (no second decider).
        if(pilot<int(sizeof g_stockFloat/sizeof g_stockFloat[0])){
            logger::Write("OBS system=stock.float vehicle=%08X pilot=%d event=released_at_transfer lease=%llu commanded_z=%.2f reason=route_mode_by_the_ordinary_hover_or_drive_rules",
                vehicle,pilot,static_cast<unsigned long long>(lease),g_stockFloat[pilot].z);
            g_stockFloat[pilot]={};g_stockPulseIssued[pilot]={};
            g_gs[pilot].liftoff=true;  // an ordinary take-off at the road if its route flies (never the ground-settle stop of a car that was floating)
        }
        logger::Write("OBS system=factory.transfer vehicle=%08X event=motion_rebased pilot=%d lease=%llu",
            vehicle,pilot,static_cast<unsigned long long>(lease));
    }
#endif

    bool DriveFor(std::uint32_t vehicleFormId, DriveInput& out)
	{
		if (!vehicleFormId) return false;
        // The native wheel-input provider still receives a neutral command while the rail owns the body.
#ifndef FVO_RELEASE
        if (reloc::RailOwns(vehicleFormId)) { out = {}; return true; }
#endif
        if (FleetRailOwns(vehicleFormId)) { out = {}; return true; }
		for (int i = 0; i < kPilots; ++i) {
			DriveInput frame;
            std::uint64_t authority=0;
            if(!g_driveOut[i].Read(vehicleFormId,frame,authority))continue;
            if(!handoff::WheelGrantCurrent(authority))continue;
            out=frame;return true;
		}
		return false;
	}

	bool ViewHeading(double& yaw) { return CameraHeading(yaw); }
    // A pilot is flying this vehicle right now (the traffic fall guard freezes one that no pilot holds).
    int PilotLive(std::uint32_t id){
        if(!id)return -1;
        const auto now=GetTickCount64();
        for(int i=1;i<kPilots;++i)if(g_jobs[i].vehicleId==id&&g_gs[i].active&&g_gs[i].autopilot){
            if((g_cmds[i].c.set&Command::kScripted)&&now<g_cmds[i].until)return 1;
            return StepsRecent(i,now)?1:0;
        }
        return -1;
    }
    bool PilotHolds(std::uint32_t id){
        if(!id)return false;
        for(int i=1;i<kPilots;++i)if(g_jobs[i].vehicleId==id&&g_gs[i].active&&g_gs[i].autopilot)return true;
        return false;
    }
    bool BirthControlReady(std::uint32_t id){
        for(int i=1;i<kPilots;++i)if(g_jobs[i].vehicleId==id&&g_gs[i].autopilot&&(g_gs[i].active||g_driveOut[i].Active())){
            auto& shared=g_shs[i];std::lock_guard lock(shared.lock);
            // Scene ownership is established before the keyframed chassis can
            // take its first dynamic step; waiting for that step would deadlock.
            // Driving on wheels (a Drive lane birth, 2026-11-05): the pilot's drive output is its control; a hover-only test kept such
            // births frozen for good (FF100040 at -355,1684: controller_ready=0 for the whole run, 5,837 release attempts).
            if(shared.active)return shared.sceneCount>0&&shared.vehicle.load();
            return shared.vehicle.load()!=0;
        }return false;
    }
    bool RouteTripActive(std::uint32_t id){
        if(id==g_roverId&&g_gs[0].active&&g_gs[0].autopilot)return true;
        for(int i=1;i<kPilots;++i)if(g_jobs[i].vehicleId==id&&g_gs[i].active&&g_gs[i].autopilot)return true;
        return false;
    }

	TripProgress ProgressFor(std::uint32_t vehicleId)
	{
		for (int i=0; i<kPilots; ++i) {
			const auto vehicle=g_hoverVehicles[i].load();
			if (vehicle && g_gs[i].active && At<std::uint32_t>(vehicle,off::kFormId)==vehicleId &&
				GetTickCount64()-g_gs[i].trip.at < 500) return g_gs[i].trip;
		}
		return {};
	}

	bool AtDestination(std::uintptr_t vehicle, double x, double y, double z, double stop, bool requireStopped)
	{
		if (!vehicle) return false;
		for (int i=0; i<kPilots; ++i) {
			if (g_hoverVehicles[i].load()!=vehicle || !g_gs[i].active) continue;
			const auto& state=g_gs[i];
			auto& shared=g_shs[i];
			std::lock_guard lk(shared.lock);
			const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
			const auto& nav=PilotNavOf(i);
            if(requireStopped&&std::isfinite(nav.arriveYaw)&&std::fabs(integrationlogic::Wrap(nav.arriveYaw-At<game::Vec3>(vehicle,off::kRefRotation).z))>0.12)return false;
            const double clearance=integrationlogic::Clearance(int(nav.height),nav.minClearance,shared.hull.height,kCallSkim)+EnvClimb();
            const double expected=state.landing?state.landGround+state.landHeight:std::isfinite(z)?std::max(z,shared.refGround+clearance):pos.z;
			const double speed=std::sqrt(shared.v[0]*shared.v[0]+shared.v[1]*shared.v[1]+shared.v[2]*shared.v[2]);
			if(state.drive)return triplogic::Reached(std::hypot(x-pos.x,y-pos.y),stop,
				std::isfinite(z)?pos.z-z:0,requireStopped?state.drvSp:0,
				state.haveGround && GetTickCount64()-state.groundAt<=500);
			return triplogic::Reached(std::hypot(x-pos.x,y-pos.y),stop,pos.z-expected,requireStopped?speed:0,
				shared.matched && state.haveGround && GetTickCount64()-state.groundAt<=500);
		}
        static thread_local world::Snapshot snap;if(world::VehiclesAndHeld(snap))for(unsigned k=0;k<snap.count&&k<world::kMaxVehicles;++k)if(snap.v[k].ref==vehicle) {
            const auto& v=snap.v[k];const double speed=std::hypot(std::hypot(v.vel[0],v.vel[1]),v.vel[2]);
            const auto site=environment::BoardingSite(vehicle);
            bool landing=false;
            for(int i=0;i<kPilots;++i)if(g_jobs[i].vehicleId==v.formId || (i==0&&v.formId==g_roverId)) {
                const auto&nav=PilotNavOf(i);landing=nav.landOnArrival;
                if(requireStopped&&std::isfinite(nav.arriveYaw)&&std::fabs(integrationlogic::Wrap(nav.arriveYaw-v.heading))>0.12)return false;
            }
            return std::hypot(x-v.pos[0],y-v.pos[1])<=stop+1.5&&(!std::isfinite(z)||landing||std::fabs(v.pos[2]-z)<2.0)&&(!requireStopped||speed<0.75)&&site.verdict==environment::Verdict::Safe;
        }
        return false;
    }

	// A scanner order ends Cruise: the lap's traffic speed and pass-through settings go with it.
	static void EndCruise()
	{
		if (!g.cruise) return;
		g.cruise = false; g.cruiseRoute = {};
		PilotNav(0, vcontract::NavParams{});
		logger::Write("Rover cruise: off (scanner order)");
	}

	void CancelScannerTrip()
	{
		g_scanCall.store(false); g_goOnBoard.store(false); g_goAt.store(0);
		EndCruise();  // scanner double T: hold here, Cruise does not carry on under the hold
		if (g_crewPending==npcvehicles::CrewOrder::Marker) g_crewPending=npcvehicles::CrewOrder::None;
		if (npcvehicles::CrewOrderOf(g_roverId)==npcvehicles::CrewOrder::Marker)
			npcvehicles::SetCrewOrder(g_roverId,npcvehicles::CrewOrder::Hold);
		const auto vehicle=g_hoverVehicles[0].load();
		if (vehicle && g.active && g.autopilot) {
			const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
			g.holdId=At<std::uint32_t>(vehicle,off::kFormId); g.holdX=pos.x; g.holdY=pos.y; g.holdZ=pos.z; g.routeHold=true;
			g.taxiMarker=0; g.taxiArrived=false; g.taxiOrbit=false; g.wHold=false;
		}
	}

    void RetryScannerTrip() {
        for(int i=0;i<kPilots;++i){const auto vehicle=g_hoverVehicles[i].load();if(i==0||(vehicle&&At<std::uint32_t>(vehicle,off::kFormId)==g_roverId)){
            g_gs[i].reverseUntil=0;}}
        navfield::Reset();
        RequestScannerTrip();
    }

	void ToggleAuto()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto driven = DrivenBy(player);
		if (!driven) { logger::Write("Rover auto: refused (no vehicle you drive)"); return; }
		if (g.playerAuto) {
			g_autoOffWhy = "your Auto/Cruise key"; g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
			notify::Log("Auto off - you have control");
			logger::Write("Rover auto: off (T tap)");
			return;
		}
		const bool wasHovering = g.active && g_hoverVehicle.load() == driven;
		if (!wasHovering) Start(0, driven, true);  // CW-1: on its wheels, no lift-off
		g.autopilot = g.playerAuto = true;
		g.routeHold = false; g.holdId = 0;
		g.taxiWait = false; g.taxiMarker = 0; g.taxiArrived = false; g.taxiOrbit = false;
		g.wHold = false; g.cramped = 0; g_bumps[0] = Bumps{};
		g.autopilotSince = GetTickCount64();
		g_goOnBoard = false;
		g.cruise = false; g.cruiseRoute = {};
		notify::Log("Auto on - W/A/S/D or T takes over");
		logger::Write("Rover auto: on (T tap, %s)", wasHovering ? "already hovering" : "on its wheels");
	}

	// Cruise (2026-10-31, the user: "like double tapping T"): Auto with no destination. Double tap again, a tap, or the movement keys end it.
	bool Cruising() { return g.cruise; }
	void ToggleCruise()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto driven = DrivenBy(player);
		if (!driven) { logger::Write("Rover cruise: refused (no vehicle you drive)"); return; }
		if (g.playerAuto && g.cruise) { ToggleAuto(); notify::Log("Cruise off - you have control"); return; }
		if (!g_cruisePlanner) { notify::Log("Cruise needs the traffic addon"); logger::Write("Rover cruise: refused (the traffic addon gave no route planner)"); return; }
		if (!g.playerAuto) ToggleAuto();
		if (!g.playerAuto) { logger::Write("Rover cruise: refused (Auto did not start)"); return; }
		if (g.taxiRoute.step) { g.taxiRoute = {}; PilotNav(0, vcontract::NavParams{}); }
		g.taxiRouteAsked = false;
		const auto pos = At<game::Vec3>(driven, off::kRefPosition);
		g.cruise = true; g.cruiseRoute = {}; g.cruiseAskAt = 0; g.cruiseToldNone = false;
		g.cruiseWaitX = pos.x; g.cruiseWaitY = pos.y;
		notify::Log("Cruise on - joining traffic. W/A/S/D or T takes over");
		logger::Write("Rover cruise: on (source=your Auto/Cruise key)");
	}

	void RequestScannerTrip()
	{
		g.holdId=0; g.routeHold=false;
        for(int i=0;i<kPilots;++i){const auto vehicle=g_hoverVehicles[i].load();if(i==0||(vehicle&&At<std::uint32_t>(vehicle,off::kFormId)==g_roverId))g_gs[i].reverseUntil=0;}
		if (npcvehicles::CrewOrderOf(g_roverId)==npcvehicles::CrewOrder::Hold)
			npcvehicles::SetCrewOrder(g_roverId,npcvehicles::CrewOrder::Marker);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto driven = DrivenBy(player);
		if (driven) {
			const bool wasHovering = g.active && g_hoverVehicle.load() == driven;
			if (!wasHovering) Start(0, driven, true);  // CW-1: on its wheels
			g.autopilot = g.playerAuto = true;
			EndCruise();  // scanner T: go there
			g.taxiWait = false; g.taxiMarker = 0; g.taxiArrived = false; g.taxiOrbit = false;
			g.wHold = false; g.cramped = 0; g_bumps[0] = Bumps{};
			g.autopilotSince = GetTickCount64();
			g_scanCall = false; g_goOnBoard = false;
			logger::Write("Rover scanner trip: selected target starts navigation in driven vehicle %08X", At<std::uint32_t>(driven,off::kFormId));
			return;
		}
		g_scanCall.store(true);
		g_goOnBoard.store(true);
		g_goAt.store(GetTickCount64());
		logger::Write("Rover scanner trip: asked (summon now, Auto to the marker when you board)");
	}

	void DisableAutopilot(std::uint32_t vehicleFormId, unsigned ms, const char* why)
	{
		if (!vehicleFormId) return;
		const ULONGLONG until = GetTickCount64() + ms;
		DisabledAp* slot = nullptr;
		for (auto& d : g_disabled) {
			if (d.id == vehicleFormId) { slot = &d; break; }
		}
		if (!slot) {
			slot = &g_disabled[0];
			for (auto& d : g_disabled) if (d.until < slot->until) slot = &d;
		}
		slot->id = vehicleFormId;
		slot->until = until;
		logger::Write("Vehicle %08X: autopilot off for %.0f s (%s)", vehicleFormId, ms / 1000.0, why ? why : "");
		// A flight in progress ends on its next frame (Fly); NPC pilots now.
		for (int i = 1; i < kPilots; ++i) {
			if (g_gs[i].active && g_jobs[i].vehicleId == vehicleFormId) Stop(i, "autopilot off: too many hits");
		}
	}

	bool AutopilotDisabled(std::uint32_t vehicleFormId) { return IsDisabled(vehicleFormId); }

	void SetAutoMode(AutoMode m)
	{
		g_autoMode = m;
		logger::Write("Rover taxi: Auto behaviour %s", m == AutoMode::Orbit ? "orbit the marker" : m == AutoMode::Patrol ? "patrol the marker chain" : "stop at the marker");
	}

	AutoMode GetAutoMode() { return g_autoMode; }

	bool SetGuardPoint(double x,double y) {
		const double nan=std::numeric_limits<double>::quiet_NaN();  // skim the terrain there: the aimed height can be an estimate
		if(npcvehicles::SetCrewGuard(g_roverId,x,y,nan))return true;
		const auto rover=Rover();
		if(!rover||IsDisabled(g_roverId)){logger::Write("Rover guard: refused (%s)",rover?"autopilot disabled after hits":"no vehicle");return false;}
		g_follow=false;g_guard=true;
		if(!(g.active&&g_hoverVehicle.load()==rover))Start(rover);
		g.autopilot=true;g.playerAuto=false;g.holdId=g_roverId;g.holdX=x;g.holdY=y;g.holdZ=nan;g.routeHold=true;g.autopilotSince=GetTickCount64();
		logger::Write("Rover guard: %08X guards (%.0f, %.0f)",g_roverId,x,y);
		return true;
	}

	void FreshScannerTarget() {g_targetSession.Selected();}
	void EndTargetSession(std::uintptr_t vehicle)
	{
		const auto player=Ptr(addr::kPlayerCharacterPtr);
		if(!player||!vehicle)return;
		const auto handle=At<std::uint32_t>(player,off::kPlayerCustomMarker);
		if(const auto marker=MarkerFromHandle(handle)) {
			const auto p=At<game::Vec3>(marker,off::kRefPosition);
			g_targetSession.Exit({handle,p.x,p.y,p.z});
			EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(marker);
		}
		CancelScannerTrip();scantarget::EndRide();
		g.taxiMarker=0;g.taxiArrived=false;g.taxiOrbit=false;g.trip={};
		logger::Write("Vehicle target session ended; prior destination and route discarded");
	}

	bool MarkerPoint(std::uintptr_t vehicle, double& x, double& y, double* z)
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !vehicle) return false;
		std::uint32_t id = 0;
		return TaxiMarker(player, vehicle, id, x, y, z);
	}

	namespace
	{
		constexpr double kFlipMoved = 3.0;  // m since the tip began that still counts as slow
		struct FlipWatchEntry { std::uint32_t id = 0; ULONGLONG since = 0, fixedAt = 0, lastFixAt = 0; int repeats = 0; game::Vec3 at{}; } g_flips[8];

		bool Upright(std::uintptr_t vehicle, const game::Vec3& p, double yaw)
		{
			const auto cell = At<std::uintptr_t>(vehicle, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			if (!cell || !ws) return false;
			float pos[3] = { p.x, p.y, p.z + 1.5f };
			float rot[3] = { 0.0f, 0.0f, float(yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(vehicle, &handle, cell, ws, pos, rot);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	void WatchFlip(std::uintptr_t vehicle, const char* who)
	{
		if (!vehicle) return;
		const std::uint32_t id = At<std::uint32_t>(vehicle, 0x28);
		if(vehiclehits::Wrecked(id))return;
		const auto loaded = At<std::uintptr_t>(vehicle, off::kRefLoadedData);
		if (!loaded || !At<std::uintptr_t>(loaded, 0x08)) return;
		FlipWatchEntry* e = nullptr;
		for (auto& f : g_flips) if (f.id == id) { e = &f; break; }
		if (!e) {
			for (auto& f : g_flips) if (!f.id || GetTickCount64() - std::max(f.since, f.fixedAt) > 60000) { f = FlipWatchEntry{}; f.id = id; e = &f; break; }
			if (!e) return;
		}
		const auto rot = At<game::Vec3>(vehicle, off::kRefRotation);
		const bool flipped = std::cos(double(rot.x)) * std::cos(double(rot.y)) < kFlipUp;
		const ULONGLONG now = GetTickCount64();
		if (!flipped) { e->since = 0; return; }
		// Tipped and SLOW, as this function's contract says. A vehicle banking or tumbling through the air is not one to set upright: the old
		// check had no speed, and the 18:59 run's 1,700-2,200 m body jumps came within 0.5 s of resets of vehicles in flight (FF016A59 at z 431).
		const auto at = At<game::Vec3>(vehicle, off::kRefPosition);
		if (!e->since || std::hypot(std::hypot(at.x - e->at.x, at.y - e->at.y), at.z - e->at.z) > kFlipMoved) {
			if (e->since) logger::Observe("flight.flip", id, 1, "event=moving pitch=%.0f roll=%.0f pos=%.2f,%.2f,%.2f", rot.x * 57.2958, rot.y * 57.2958, at.x, at.y, at.z);
			e->since = now; e->at = at;
		}
		if (now - e->since < kFlipMs || now - e->fixedAt < 5000) return;
		e->fixedAt = now;
		e->since = 0;
		for (int i = 0; i < kPilots; ++i) {
			if (g_gs[i].active && g_hoverVehicles[i].load() == vehicle) Stop(i, "flipped");
		}
		// Flipped again soon after the last fix (2026-10-29 14:37: one parked REV-8 every 5 s for 3 minutes, set upright inside the ground it
		// was placed in): each repeat within 30 s sets it 1 m higher (4 m at most), so it clears what keeps tipping it.
		e->repeats = now - e->lastFixAt < 30000 ? std::min(e->repeats + 1, 4) : 0;
		e->lastFixAt = now;
		auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
		pos.z += float(e->repeats);
		const auto before = At<game::Vec3>(vehicle, off::kRefPosition);
		const bool ok = Upright(vehicle, pos, rot.z);
		const auto after = At<game::Vec3>(vehicle, off::kRefPosition);
		logger::Write("OBS system=flight.flip vehicle=%08X event=upright ok=%d repeats=%d before=%.2f,%.2f,%.2f after=%.2f,%.2f,%.2f", id, ok, e->repeats, before.x, before.y, before.z, after.x, after.y, after.z);
		logger::Write("%s: %08X flipped (pitch %.0f, roll %.0f deg) -> set upright%s %s", who, id, rot.x * 57.2958, rot.y * 57.2958,
			e->repeats ? (" " + std::to_string(e->repeats) + " m higher (flipped again)").c_str() : "", ok ? "" : "FAILED");
	}

	namespace
	{
		vcontract::NavParams g_nav[kPilots];
		ULONGLONG g_navAt[kPilots] = {};
	}

	void SetCruisePlanner(FvoCruisePlanner planner) { g_cruisePlanner = planner; }
	void SetRoutePlanner(FvoRoutePlanner planner)
	{
		g_routePlanner = planner;
		logger::Write("Rover taxi: route planner %s (Auto takes a traffic route when one helps)", planner ? "registered" : "removed");
	}

	void PilotNav(int pilot, const vcontract::NavParams& nav)
	{
		if (pilot < 0 || pilot >= kPilots) return;
		if (!nav.trafficLane) routeguidance::Clear(pilot);
        const auto& old = g_nav[pilot];
		// Logged when a value changes (2026-10-29 quieter log: a re-send after a 1 s gap also logged, ~1 800 identical lines a run).
		const bool same = old.speedLimit == nav.speedLimit && old.minClearance == nav.minClearance && old.height == nav.height &&
			old.margin == nav.margin && old.squeeze == nav.squeeze && old.priority == nav.priority && old.passThrough == nav.passThrough &&
			old.landOnArrival == nav.landOnArrival && (old.arriveYaw == nav.arriveYaw || (std::isnan(old.arriveYaw) && std::isnan(nav.arriveYaw)));
		if (!same) {
			static const char* const heights[3] = { "auto", "hug", "high" };
			logger::Write("%s: nav settings: speed limit %.0f, clearance %.1f, height %s, margin %.1f, squeeze %d, priority %d, pass through %d, land on arrival %d",
				Who(pilot), nav.speedLimit, nav.minClearance, heights[std::clamp(int(nav.height), 0, 2)], nav.margin, nav.squeeze ? 1 : 0, nav.priority,
				nav.passThrough ? 1 : 0, nav.landOnArrival ? 1 : 0);
		}
		g_nav[pilot] = nav;
		g_navAt[pilot] = GetTickCount64();
	}

	const vcontract::NavParams& PilotNavOf(int pilot)
	{
		static const vcontract::NavParams none;
		return pilot >= 0 && pilot < kPilots && GetTickCount64() - g_navAt[pilot] <= 1000 ? g_nav[pilot] : none;
	}

	void ClearPilotCommand(int pilot) {if(pilot>=0&&pilot<kPilots)g_cmds[pilot]={};}
    void BeginFrame(){++g_flightFrame;for(int i=0;i<kPilots;++i)ClearPilotCommand(i);}
    bool ImpactSince(std::uint32_t id,unsigned long long since) {
        const auto now=GetTickCount64();
        for(auto& sh:g_shs){const auto ref=sh.vehicle.load();if(!ref)continue;
            if(At<std::uint32_t>(ref,off::kFormId)!=id)continue;
            const auto at=sh.impactAt.load();return at>=since&&now>=at&&now-at<=250;}
        return false;
    }
    double CruiseLimit(std::uint32_t id) {
        static thread_local world::Snapshot snap; if(world::VehiclesAndHeld(snap))for(unsigned k=0;k<snap.count&&k<world::kMaxVehicles;++k)if(snap.v[k].formId==id) {
            const auto&v=snap.v[k];return kAiGearTop[4]*hulllogic::For(v.half[0],v.half[1],v.half[2]).acceleration;
        }
        return kAiGearTop[2];
    }
    double TurnLimit(std::uint32_t id) {
        static thread_local world::Snapshot snap;if(world::VehiclesAndHeld(snap))for(unsigned k=0;k<snap.count&&k<world::kMaxVehicles;++k)if(snap.v[k].formId==id) {
            const auto&v=snap.v[k];const double speed=std::hypot(v.vel[0],v.vel[1]);
            // A pilot flying it physically turns within the chassis' real grip (hulllogic::Physical), so the combat AI's turn
            // radius plans for its momentum too.
            for(auto&sh:g_shs){const auto ref=sh.vehicle.load();
                if(ref&&At<std::uint32_t>(ref,off::kFormId)==id){std::lock_guard lk(sh.lock);if(sh.active&&sh.physical)return sh.hull.YawAt(speed);}}
            return hulllogic::For(v.half[0],v.half[1],v.half[2]).YawAt(speed);
        }
        return 0.8;
    }
    void PlayerHands(int pilot, int gear)
    {
        if (pilot < 1 || pilot >= kPilots) return;
        if (g_hands.exchange(pilot) != pilot || GetTickCount64() >= g_handsUntil.load()) logger::Write("%s: flown by your hands from the passenger seat (your hover model)", Who(pilot));
        g_handsGear = gear; g_handsUntil = GetTickCount64() + 250;
    }
    bool PilotCommand(int pilot, const Command& c)
	{
		if (pilot < 0 || pilot >= kPilots || !g_gs[pilot].active || !g_gs[pilot].autopilot) return false;
		g_cmds[pilot].c = c;
		g_cmds[pilot].until = GetTickCount64() + c.holdMs;
        {   // The goal door: a command is direct actuation (throttle, yaw, strafe), recorded as such so the ledger can name it (adapter).
            goaldoor::Goal dg;dg.source=goaldoor::Source::Command;dg.direct=true;dg.identity=g_jobs[pilot].vehicleId;dg.at=double(GetTickCount64())/1000.0;
            if(g_door.Admit(pilot,dg))logger::Observe("plan.door",std::uint32_t(dg.identity),std::uint64_t(pilot)^0x100,"event=admit pilot=%d source=command direct=1 scripted=%d reason=pilotcommand_direct_actuation_not_on_the_plan",pilot,int((c.set&Command::kScripted)!=0));}
		return true;
	}

	bool PilotCombatAim(int pilot, double x, double y, double z, bool aim, bool onPlan)
	{
		// CB-2: combat's aim this frame (Route's facing) and whether combat may go on the plan (not stunned). Returns whether a combat
		// line drives this pilot now; only then does combat drop its movement sticks (a refused line leaves today's stick path).
		if (pilot < 1 || pilot >= kPilots || pilot >= int(sizeof g_combatAim / sizeof g_combatAim[0])) return false;
		const ULONGLONG now = GetTickCount64();
		auto& a = g_combatAim[pilot]; a.x = x; a.y = y; a.z = z; a.aim = aim; a.onPlan = onPlan; a.at = now;
		return onPlan && CombatLinesOn() && CombatLineNow(pilot, now);
	}

	bool CensusBroken(std::uint32_t vehicle)
	{
		std::lock_guard<std::mutex> ownersGuard(g_ownersLock);
		const auto* r = g_owners.Find(vehicle);
		return r && r->owner == planowner::Owner::Traffic && (r->life == planowner::Life::Broken || r->life == planowner::Life::Retiring);
	}

	const char* PilotEndReason(int pilot) { return pilot >= 1 && pilot < kPilots ? g_endWhy[pilot] : ""; }

	void PilotStop(int pilot, const char* why)
	{
        routeguidance::Clear(pilot);
        if(const auto* dg=g_door.Of(pilot)){logger::Observe("plan.door",std::uint32_t(dg->identity),std::uint64_t(pilot)^0x200,"event=clear pilot=%d source=%s reason=%s",pilot,goaldoor::Name(dg->source),why?why:"stop");g_door.Clear(pilot);}
		if (PilotActive(pilot)) Stop(pilot, why);
	}

	std::uint32_t OwnRoverId() { return g_roverId; }

    fleetownershipstore::PlayerControl PlayerControlForSave() {
        std::lock_guard lock(g_saveControlLock);
        return g_saveControl;
    }

    // RS-1 (2026-11-22, the user: "Yes, resume it"): a save made on the player's own Auto or Cruise resumes it once on load,
    // through the same ToggleAuto / ToggleCruise the T key uses, when the player sits at the wheel of that same vehicle
    // (reference and base unchanged). Manual, unknown or older saves change nothing. Called by the traffic profile restore
    // (any thread); consumed by Tick.
    namespace {
        std::mutex g_resumeLock;
        fleetownershipstore::PlayerControl g_resume;
        ULONGLONG g_resumeFirst = 0, g_resumeMatched = 0;
        void ResumeTick() {
            fleetownershipstore::PlayerControl want;
            {
                std::lock_guard lock(g_resumeLock);
                if (!fleetownershipstore::ResumesOnLoad(g_resume.control)) return;
                want = g_resume;
            }
            using fleetownershipstore::Control;
            const ULONGLONG now = GetTickCount64();
            auto finish = [&](const char* event, const char* why) {
                std::lock_guard lock(g_resumeLock);
                if (g_resume.vehicle == want.vehicle && g_resume.control == want.control) g_resume = {};
                g_resumeFirst = g_resumeMatched = 0;
                logger::Write("OBS system=player.resume vehicle=%08X event=%s mode=%s reason=%s", want.vehicle, event,
                    want.control == Control::Cruise ? "cruise" : "auto", why);
            };
            if (!g_resumeFirst) g_resumeFirst = now;
            if (now - g_resumeFirst > fleetownershipstore::kResumeWindowMs) { finish("expired", "not_at_the_wheel_of_the_saved_vehicle"); return; }
            const auto driven = DrivenBy(Ptr(addr::kPlayerCharacterPtr));
            const auto base = driven ? At<std::uintptr_t>(driven, 0x98) : 0;
            const bool same = driven && base && At<std::uint32_t>(driven, off::kFormId) == want.vehicle &&
                At<std::uint32_t>(base, off::kFormId) == want.base;
            if (!same) { g_resumeMatched = 0; return; }
            if (!g_resumeMatched) g_resumeMatched = now;
            if (now - g_resumeMatched < fleetownershipstore::kResumeSettleMs) return;  // seated and steady first
            if (g_gs[0].playerAuto) { finish("skipped", "already_on_auto"); return; }
            if (IsDisabled(want.vehicle)) { finish("skipped", "autopilot_disabled_after_hits"); return; }
            if (want.control == Control::Cruise) {
                if (!g_cruisePlanner) return;  // the add-on registers its planner after load; wait inside the window
                ToggleCruise();
            } else ToggleAuto();
            if (g_gs[0].playerAuto) finish("resumed", "saved_on_autopilot");
            else finish("refused", "auto_did_not_start");
        }
    }
    void ResumeSavedControl(fleetownershipstore::PlayerControl control) {
        std::lock_guard lock(g_resumeLock);
        g_resume = fleetownershipstore::ResumesOnLoad(control.control) ? control : fleetownershipstore::PlayerControl{};
        g_resumeFirst = g_resumeMatched = 0;
        if (g_resume.vehicle) logger::Write("OBS system=player.resume vehicle=%08X event=queued base=%08X mode=%s", control.vehicle, control.base,
            control.control == fleetownershipstore::Control::Cruise ? "cruise" : "auto");
    }

	void Tick()
	{
        if (routesurvey::OnMainThread()) {
            for (int i = 0; i < kPilots; ++i) {
                if (const auto inst = g_wakeQueue[i].exchange(0, std::memory_order_relaxed)) WakeBody(inst);
            }
        }
        ReportStepCost();
		{costparts::Scope c(costparts::FlightSnapshot);g_vsnapOk = world::VehiclesAndHeld(g_vsnap);}
		{costparts::Scope c(costparts::FlightEnv);EnvTick();}
		{costparts::Scope c(costparts::FlightPlayer);
			if (const auto player = Ptr(addr::kPlayerCharacterPtr)) {
				TrailTick(player);
				PlayerAirTick(player);
			}
			PlayerTick();
            using fleetownershipstore::Control;
            const auto driven = g_manualDriverId.load() ? g_driverVehicle.load() : 0;
            if (driven) {
                const auto drivenId = At<std::uint32_t>(driven, off::kFormId);
                if (drivenId) {
                    if (vehiclediag::BirthHeld(drivenId)) {
                        vehiclelifecycle::OnPlayerEnteredVehicle(drivenId, false);
                    } else {
                        static ULONGLONG lastDynAt = 0;
                        const ULONGLONG nowDyn = GetTickCount64();
                        if (nowDyn - lastDynAt >= 250) {
                            lastDynAt = nowDyn;
                            vehiclelifecycle::OnPlayerEnteredVehicle(drivenId, true);
                        }
                    }
                }
            }
            // The player's own Auto or Cruise only (not a guard or summon flight without the player).
            const auto flown = g_gs[0].active && g_gs[0].autopilot && g_gs[0].playerAuto ? g_hoverVehicle.load() : 0;
            const auto rover = RefById(g_roverId);
            const auto vehicle = driven ? driven : rover;
            fleetownershipstore::PlayerControl current;
            if (vehicle) {
                const auto base = At<std::uintptr_t>(vehicle, 0x98);
                if (base) current = {At<std::uint32_t>(vehicle, off::kFormId),
                    At<std::uint32_t>(base, off::kFormId),
                    flown != vehicle ? Control::Manual : g_gs[0].cruise ? Control::Cruise : Control::Autopilot};
            }
            {
                std::lock_guard lock(g_saveControlLock);
                g_saveControl = current;
            }
            ResumeTick();
        }
        {costparts::Scope c(costparts::FlightRails);RefreshFleetRails();}
		{costparts::Scope c(costparts::FlightPilots);PilotsTick();}
		// Unified vehicle management + Core Fall Guard (2026-11-23):
		// Every loaded vehicle in the world is registered in g_owners (no unmanaged vehicles fall through the cracks).
		// Any vehicle with no active pilot and no player in the seat that enters an uncontrolled fall is frozen in place.
		if (g_vsnapOk) {
			const double nowSec = double(GetTickCount64()) / 1000.0;
			const std::uint32_t roverId = g_roverId;
			const bool playerAutoActive = g_gs[0].active && g_gs[0].autopilot;
			for (std::uint32_t vi = 0; vi < g_vsnap.count && vi < world::kMaxVehicles; ++vi) {
				const auto& v = g_vsnap.v[vi];
				if (!v.formId || !std::isfinite(v.pos[0]) || !std::isfinite(v.pos[1]) || !std::isfinite(v.pos[2])) continue;
				const bool playerManualSeat = (v.who == world::VehicleInfo::Who::Player) && !playerAutoActive;
				bool hasActivePilot = false;
				planowner::Owner curOwner = planowner::Owner::None;
				{
					std::lock_guard<std::mutex> ownersGuard(g_ownersLock);
					if (playerManualSeat) {
						g_owners.Claim(v.formId, planowner::Owner::Player, trafficrank::Rank::Player, -1, nowSec, "player_manual");
					} else if (!g_owners.Of(v.formId, nowSec)) {
						g_owners.Claim(v.formId, planowner::Owner::Parked, trafficrank::Rank::Parked, -1, nowSec,
							v.formId == roverId ? "player_rover_parked" : "world_managed_parked");
					}
					if (auto* rec = g_owners.Find(v.formId)) {
						curOwner = rec->owner;
						if (rec->owner == planowner::Owner::Parked || rec->owner == planowner::Owner::Player) {
							rec->x = v.pos[0]; rec->y = v.pos[1]; rec->z = v.pos[2];
							rec->speed = std::hypot(v.vel[0], v.vel[1]); rec->placedAt = nowSec;
						}
						if (rec->pilot >= 0 && rec->pilot < kPilots && PilotActive(rec->pilot)) hasActivePilot = true;
					}
				}
				const float vz = v.vel[2];
				const float hSpeed = std::hypot(v.vel[0], v.vel[1]);
				// Only freeze true below-world void falls — never an above-ground drop (which would leave the car
				// permanently frozen in mid-air via StageBirth) and never a car driving downhill on a steep ramp.
				const bool deepVoidFall = std::isfinite(vz) && v.pos[2] < -220.0f && vz < -12.0f;
				if (playerManualSeat || (hasActivePilot && !deepVoidFall) || vehiclediag::BirthHeld(v.formId)) continue;
				bool falling = std::isfinite(vz) && (deepVoidFall || (v.pos[2] < -220.0f && vz < -6.0f));
				if (!falling && std::isfinite(vz) && std::isfinite(hSpeed) && vz < -6.0f && hSpeed < 5.0f) {
					double floorZ = 0.0;
					const auto nav = navmap::At(v.pos[0], v.pos[1], double(v.pos[2]) + 2.0);
					const bool haveFloor = (nav.haveGround && std::isfinite(nav.ground) && (floorZ = nav.ground, true)) ||
						(cellbake::Ground(v.pos[0], v.pos[1], floorZ) && std::isfinite(floorZ));
					if (haveFloor && double(v.pos[2]) < floorZ - 12.0) falling = true;
				}
				if (falling) {
					vehiclediag::StageBirth(v.formId);
					archproof::State().violFallguard.fetch_add(1, std::memory_order_relaxed);
					logger::Write("OBS system=vehicle.fallguard vehicle=%08X event=frozen owner=%s pos=%.2f,%.2f,%.2f vz=%.2f",
						v.formId, planowner::Name(curOwner), v.pos[0], v.pos[1], v.pos[2], vz);
				}
			}
		}
		{costparts::Scope c(costparts::FlightBind);
			for(auto&s:g_shs){std::lock_guard lk(s.lock);if(const auto ref=s.vehicle.load())BindBody(s,ref);}
			if (const auto player = Ptr(addr::kPlayerCharacterPtr)) {
				if (!DrivenBy(player)) WatchFlip(Rover(), "Rover");  // never while the player drives it
			}}
	}

	std::uint64_t TotalPhysicsSteps(int* boundPilots)
	{
		std::uint64_t total = 0; int bound = 0;
		for (int i = 0; i < kPilots; ++i) { total += g_shs[i].testSteps.load(std::memory_order_relaxed); if (g_shs[i].instance.load()) ++bound; }
		if (boundPilots) *boundPilots = bound;
		return total;
	}
    int FleetRailAssign(const FvoFleetRailRequest* request)
    {
        if (!trafficprofile::FleetRailWritesAllowed() || !request || !request->vehicle || !request->driver ||
            !std::isfinite(request->speed) || request->speed < 0.0 || request->speed > 150.0 ||
            PlayerDriving(request->vehicle)) return 0;
        fleetrail::Path path;
        if (!path.Set(request->points, request->count)) return 0;
        std::lock_guard lk(g_fleetRailLock);
        if (!trafficprofile::FleetRailWritesAllowed()) return 0;
        const auto it = g_fleetRails.find(request->vehicle);
        if (it == g_fleetRails.end() && g_fleetRails.size() >= fvolimits::kVehicles) return 0;
        auto& car = g_fleetRails[request->vehicle];
        if (car.driver && car.driver != request->driver) car = {};
        car.driver = request->driver;
        car.speed = request->speed;
        if (!car.path.Same(path)) {
            if (car.seeded) car.station = path.Project(car.position.x, car.position.y, car.position.z);
            car.path = std::move(path);
        }
        g_fleetRailCount.store(unsigned(g_fleetRails.size()), std::memory_order_release);
        return int(car.mode);
    }

    void FleetRailRelease(unsigned vehicle)
    {
        std::lock_guard lk(g_fleetRailLock);
        if (vehicle) g_fleetRails.erase(vehicle);
        else g_fleetRails.clear();
        {
            std::lock_guard handoffLock(g_fleetHandoffLock);
            if (vehicle) g_fleetHandoffs.erase(vehicle);
            else g_fleetHandoffs.clear();
        }
        g_fleetRailCount.store(unsigned(g_fleetRails.size()), std::memory_order_release);
    }

    int FleetRailState(unsigned vehicle)
    {
        std::lock_guard lk(g_fleetRailLock);
        const auto it = g_fleetRails.find(vehicle);
        if (it == g_fleetRails.end()) return 0;
        return it->second.mode == fleetrail::Mode::Reference ? int(fleetrail::Mode::Rail) : int(it->second.mode);
    }

	void ArmFarDispatch(std::uint32_t vehicleId, std::uint32_t driverId, double x, double y, double z)
	{
		fardisp::Arm(vehicleId, driverId, x, y, z);
	}
	bool HasPendingFarDispatch(std::uint32_t vehicleId)
	{
		return fardisp::HasPending(vehicleId);
	}
	bool FleetRailActive(std::uint32_t vehicleId)
	{
		return FleetRailOwns(vehicleId);
	}

	void LoadReset()
	{
        fardisp::Clear();
        FleetRailRelease(0);
        { std::lock_guard lock(g_floorProofLock); g_floorProofs = {}; g_nextFloorProof = 0; }
        (void)planenergy::TakeFloorProbe();
        for(int i=0;i<257;++i)routeguidance::Clear(i);
        {std::lock_guard cl(g_cleanLock);g_clean.Clear();}  // T1: no clean rays from another save
        {std::lock_guard sl(g_spotLock);g_spotResult.clear();g_selfReport.clear();g_healthSample.clear();g_spotRequest.clear();
#ifndef FVO_RELEASE
        g_spotProof.clear();
        for(auto& motion:g_spotMotion)motion.Reset();
        for(auto& test:g_factoryTest)test={};
        g_stockPulseOwner=0;g_stockPulseAt=0;
        for(auto& f:g_stockFloat)f={};for(auto& i:g_stockPulseIssued)i={};
#endif
        }for(auto& sp:g_spot)sp=spottest::State{};for(auto& v:g_spotVehicle)v=0;  // ST-B: ids are reused across saves
        for(int i=0;i<kPilots;++i){g_door.Clear(i);g_wakeQueue[i].store(0,std::memory_order_relaxed);}g_contacts.Reset();for(auto& f:g_planFlies)f=false;for(auto& f:g_driveOnlyLane)f=false;  // nothing carried into the next save
        for(auto& t:g_navWantAt)t=0;
		for (auto& s : g_shs) { s.physOn.store(false); PhysicalRelease(s); }
		vehiclecamera::Reset();
		FirstPersonLookLimits(false);
		TrailClear();
		// An in-game save load can keep the same player and rover addresses while
		// replacing their native bodies. Never carry thrust, contact or body
		// identity from the previous save into the new one.
		for (int i = 0; i < kPilots; ++i) {
			g_gs[i] = State{}; g_cmds[i]={}; g_navAt[i]=0; g_shs[i].impactAt.store(0);
			g_driveOut[i].Clear();
			{
				std::lock_guard lk(g_shs[i].lock);
				g_shs[i].active = false;g_shs[i].landing=false;
				g_shs[i].groundOn = false;
				g_shs[i].matched = false;
				g_shs[i].sceneCount=0;
				g_shs[i].haveLast = false;
				g_shs[i].collided = false;
				g_shs[i].steps = 0;
				g_shs[i].stepSerial=0;g_shs[i].lastStepAt=0;g_shs[i].lastStepInstance=0;
                g_shs[i].handoffStepAt=0;g_shs[i].handoffStepInstance=0;
				g_shs[i].guardVehicle = 0;
				g_shs[i].autopilot = false;
			}
			g_shs[i].vehicle.store(0);
			g_shs[i].bodyId.store(0);
			g_shs[i].instance.store(0);
			g_shs[i].guardInstance = 0;
			g_hoverVehicles[i].store(0);
			g_forwardBoosts[i].store(false);
			g_clearBoosts[i].store(0);
			g_boostState[i] = boostneed::State{}; g_boostSeenAt[i].store(0); g_pedalAt[i].store(0);  // BO-1
			g_hoverBoostWant[i].store(false); g_hoverBoostActive[i].store(false);  // BO-2
			g_jobs[i] = PilotJob{};
		}
		g_driverVehicle.store(0);g_manualDriverId.store(0);
        { std::lock_guard lock(g_saveControlLock); g_saveControl = {}; }
		g_lastDrivenVehicle.store(0);
		g_tapBoost = Tap{};
		g_tapThrust = Tap{};
		g_altInput = {}; g_altDrop = false;
		g_camRest = CamRest{};
		g_camPick = CamPick{};
		g_boostRequest=false;g_meterBoost=false;g_meterVehicle=0;g_meterExhausted=false;
		g_targetSession={};g_guard=false;g_cf=CoverFight{};
		logger::Write("Rover flight: load reset discarded prior rover motion and body identity");
	}
}

void FvoFleetRailFence()
{
    std::lock_guard lk(vehicleflight::g_fleetRailLock);
}

void FvoFleetRailReset()
{
    vehicleflight::FleetRailRelease(0);
}

extern "C" __declspec(dllexport) int FVO_FleetRailAssignV1(const FvoFleetRailRequest* request)
{
    return vehicleflight::FleetRailAssign(request);
}
extern "C" __declspec(dllexport) void FVO_FleetRailReleaseV1(unsigned vehicle)
{
    vehicleflight::FleetRailRelease(vehicle);
}
extern "C" __declspec(dllexport) int FVO_FleetRailStateV1(unsigned vehicle)
{
    return vehicleflight::FleetRailState(vehicle);
}

extern "C" __declspec(dllexport) int FVO_RegisterCruisePlanner(FvoCruisePlanner planner)
{
	vehicleflight::SetCruisePlanner(planner);
	return 1;
}
extern "C" __declspec(dllexport) int FVO_RegisterRoutePlanner(FvoRoutePlanner planner)
{
	vehicleflight::SetRoutePlanner(planner);
	return 1;
}

extern "C" __declspec(dllexport) int FVO_RegisterCorridorProfileV1(FvoCorridorProviderV1 provider){routeguidance::corridorProvider=provider;return 1;}
extern "C" __declspec(dllexport) int FVO_RegisterLaneProviderV1(FvoLaneProviderV1 provider) {
    if(routeguidance::provider&&routeguidance::provider!=provider)return 0;routeguidance::provider=provider;
    for(int i=0;i<257;++i)routeguidance::Clear(i);
    return 1;
}

extern "C" __declspec(dllexport) int FVO_RegisterJourneySaveV1(int (*save)()){routeguidance::beforeSave=save;return 1;}
extern "C" __declspec(dllexport) int FVO_RegisterTrafficPolicyV1(FvoTrafficPolicyProviderV1 provider)
{
    if(routeguidance::policyProvider&&routeguidance::policyProvider!=provider)return 0;routeguidance::policyProvider=provider;
    return 1;
}
extern "C" __declspec(dllexport) int FVO_RegisterTravelRulesV1(FvoTravelRulesProviderV1 provider) {
    if(routeguidance::rulesProvider&&routeguidance::rulesProvider!=provider)return 0;routeguidance::rulesProvider=provider;return 1;
}

extern "C" __declspec(dllexport) int FVO_RegisterFieldGateV1(FvoFieldProviderV1 provider) {
    if(routeguidance::fieldProvider&&routeguidance::fieldProvider!=provider)return 0;routeguidance::fieldProvider=provider;return 1;
}

extern "C" __declspec(dllexport) int FVO_RegisterRoadSurfaceV1(FvoRoadSurfaceV1 provider){
    if(fieldgate::surfaceProvider&&fieldgate::surfaceProvider!=provider)return 0;
    fieldgate::surfaceProvider=provider;return 1;
}
// The whole road's lanes at a point (roadlanes.h, 2026-11-05): optional; without it the planner keeps to the car's own lane.
extern "C" __declspec(dllexport) int FVO_RegisterRoadLanesV1(FvoRoadLanesProviderV1 provider){
    if(fieldgate::roadLanesProvider&&fieldgate::roadLanesProvider!=provider)return 0;
    fieldgate::roadLanesProvider=provider;return 1;
}
namespace vehicleflight {
// HV-2 (removal 2026-11-14): the energy plan's chosen path of its newest result, as one picked branch (planview.h). Read-only.
int PlanBranchesView(std::uint32_t v, PlanBranchView* out, int max){
    if(!out||max<=0)return 0;
    const auto it=g_planPilot.find(v);if(it==g_planPilot.end()||it->second<0||it->second>=planenergy::kSlots)return 0;
    const auto r=planenergy::Latest(it->second);if(!r.at||r.vehicle!=v||GetTickCount64()-r.at>2000||!r.ok)return 0;
    auto& b=out[0];b=PlanBranchView{};b.family=r.hover?"energy plan (hover)":"energy plan";b.picked=true;b.feasible=true;b.reverse=r.reverse;b.cost=float(r.energy);
    std::snprintf(b.speedWhy,sizeof b.speedWhy,"%s",r.why?r.why:"");
    const auto c=routeguidance::frames[it->second].lane.count>0?routeguidance::frames[it->second].lane.points[0].z:0.0;
    const int step=std::max(1,r.path.count/kPlanBranchPoints);
    for(int k=0;k<r.path.count&&b.points<kPlanBranchPoints;k+=step){b.x[b.points]=float(r.path.x[k]);b.y[b.points]=float(r.path.y[k]);b.z[b.points]=float(c);++b.points;}
    return 1;
}
bool PlanLedgerLine(std::uint32_t v, char* out, int n){
    if(!out||n<=0)return false;out[0]=0;
    const auto it=g_planPilot.find(v);if(it==g_planPilot.end()||it->second<0||it->second>=planenergy::kSlots)return false;
    const auto r=planenergy::Latest(it->second);if(!r.at||r.vehicle!=v)return false;
    const double now=double(GetTickCount64())/1000.0;
    std::lock_guard<std::mutex> ownersGuard(g_ownersLock);
    const auto* o=g_owners.Of(v,now);const auto* door=g_door.Of(it->second);
    std::snprintf(out,size_t(n),"energy plan %s: %s %.1f m/s%s, clearance %.2f m, %d/%d feasible, %llu ms old; owner %s/%s; goal %s",
        r.ok?"ok":"no way",r.why&&*r.why?r.why:"-",r.speed,r.reverse?" back":"",r.clearance,r.feasible,r.candidates,
        static_cast<unsigned long long>(GetTickCount64()-r.at),o?planowner::Name(o->owner):"none",o?trafficrank::Name(o->rank):"-",door?goaldoor::Name(door->source):"none");
    return true;
}
}
// Spawn Census -> Owner Record (trafficcensusapi.h). The Census is the traffic part of this one record, never a second registry:
// it claims Owner::Traffic (a stronger owner keeps the car: answer 2) and writes the traffic metadata. Optional both ways.
namespace { std::unordered_map<std::uint32_t,std::string> g_censusWhy; }  // reasons must outlive the record (node-based: c_str stays put); under g_ownersLock
// ST-B (Pair Facts): the spot test result of a vehicle: 0 none, 1 passed, 2 suspect, 3 failed, 4 running. Optional export (an older core
// has none: the addon then has no such fact and behaves as before).
// C1 (coordinator data): a read-only copy of the one owner and rank record, rows with a position only. Optional export.
extern "C" __declspec(dllexport) int FVO_OwnerSnapshotV1(planowner::RowV1* out,int max){
    if(!out||max<=0||out[0].size!=sizeof(planowner::RowV1)||out[0].version!=1)return 0;
    const double now=double(GetTickCount64())/1000.0;int n=0;
    std::lock_guard ownersGuard(vehicleflight::g_ownersLock);
    for(int i=0;i<vehicleflight::g_owners.count&&n<max;++i){const auto& r=vehicleflight::g_owners.rows[i];
        if(r.owner==planowner::Owner::None||r.placedAt<0||now-r.seen>planowner::kStale)continue;
        auto& o=out[n++];o=planowner::RowV1{};o.vehicle=r.vehicle;o.owner=int(r.owner);o.rank=int(r.rank);o.x=r.x;o.y=r.y;o.z=r.z;o.speed=r.speed;o.age=now-r.placedAt;}
    return n;}
// Field Mask: version 2 of the read-only owner record adds the lane the committed plan is heading for (route identity + lane index). Optional; version 1 is unchanged.
extern "C" __declspec(dllexport) int FVO_OwnerSnapshotV2(planowner::RowV2* out,int max){
    if(!out||max<=0||out[0].size!=sizeof(planowner::RowV2)||out[0].version!=2)return 0;
    const double now=double(GetTickCount64())/1000.0;int n=0;
    std::lock_guard ownersGuard(vehicleflight::g_ownersLock);
    for(int i=0;i<vehicleflight::g_owners.count&&n<max;++i){const auto& r=vehicleflight::g_owners.rows[i];
        if(r.owner==planowner::Owner::None||r.placedAt<0||now-r.seen>planowner::kStale)continue;
        auto& o=out[n++];o=planowner::RowV2{};o.vehicle=r.vehicle;o.owner=int(r.owner);o.rank=int(r.rank);o.x=r.x;o.y=r.y;o.z=r.z;o.speed=r.speed;o.age=now-r.placedAt;
        o.laneIdentity=r.laneIdentity;o.lane=r.lane;o.laneAge10ms=r.laneAt<0?-1:int(std::min(2e7,(now-r.laneAt)*100.0));}
    return n;}
// C2 (lanecoord.h): World's corridor summary per planned vehicle, read-only, for the addon's coordinator. Optional export.
extern "C" __declspec(dllexport) int FVO_CorridorSnapshotV1(lanecoord::RowV1* out,int max){
    if(!out||max<=0||out[0].size!=sizeof(lanecoord::RowV1)||out[0].version!=1)return 0;
    const double now=double(GetTickCount64())/1000.0;int n=0;
    std::lock_guard ownersGuard(vehicleflight::g_ownersLock);
    for(int i=0;i<vehicleflight::g_owners.count&&n<max;++i){const auto& r=vehicleflight::g_owners.rows[i];
        if(r.owner==planowner::Owner::None||r.corridorAt<0||now-r.seen>planowner::kStale||now-r.corridorAt>planowner::kStale)continue;
        auto& o=out[n++];o=lanecoord::RowV1{};o.vehicle=r.vehicle;o.blockOwner=r.blockOwner;o.laneIdentity=r.laneIdentity;o.ownLane=r.ownLane;o.plannedLane=r.lane;o.laneCount=r.corridorLanes;
        for(int k=0;k<lanecoord::kLanes;++k)o.freeLength[k]=r.corridorFree[k];
        o.measured=r.corridorMeasured;o.blockedAt=r.blockedAt;o.stoppedFor=r.stoppedFor;o.family=r.family;o.reverse=r.reverse?1:0;o.age=float(now-r.corridorAt);}
    return n;}
// C3 / C4 (lanecoord.h): the coordinator's lane permissions provider. Optional; one provider, as the road lanes.
extern "C" __declspec(dllexport) int FVO_RegisterLanePermitV1(lanecoord::PermitProviderV1 provider){
    if(fieldgate::lanePermitProvider&&fieldgate::lanePermitProvider!=provider)return 0;
    fieldgate::lanePermitProvider=provider;return 1;
}
extern "C" __declspec(dllexport) int FVO_SpotTestV1(std::uint32_t vehicle){
    std::lock_guard sl(vehicleflight::g_spotLock);const auto it=vehicleflight::g_spotResult.find(vehicle);return it==vehicleflight::g_spotResult.end()?0:it->second;}
extern "C" __declspec(dllexport) int FVO_TrafficCensusPublishV1(const FvoTrafficCensusV1* c){
    using vehicleflight::g_owners;using vehicleflight::g_ownersLock;
    if(!c||c->size<sizeof(FvoTrafficCensusV1)||c->version!=1||!c->vehicle)return 0;
    const double now=double(GetTickCount64())/1000.0;
    std::lock_guard<std::mutex> ownersGuard(g_ownersLock);
    const auto oc=g_owners.Claim(c->vehicle,planowner::Owner::Traffic,trafficrank::Rank::Parked,-1,now,"spawn census");
    if(oc.refused){logger::Write("OBS system=plan.owner vehicle=%08X event=census_refused holder=%s reason=a_stronger_owner_holds_it",c->vehicle,planowner::Name(oc.from));return 2;}
    auto* r=g_owners.Find(c->vehicle);if(!r)return 0;
    char reason[sizeof c->reason+1]{};std::memcpy(reason,c->reason,sizeof c->reason);
    auto& slotWhy=g_censusWhy[c->vehicle];slotWhy=reason;const char* why=slotWhy.c_str();
    const int life=c->life>=0&&c->life<=int(planowner::Life::Retiring)?c->life:0;
    g_owners.Traffic(c->vehicle,c->driver,c->slot,c->model,planowner::Life(life),why);
    logger::Write("OBS system=plan.owner vehicle=%08X event=census life=%d control=%d slot=%d reason=%s",c->vehicle,life,c->control,c->slot,why);
    return 1;
}
extern "C" __declspec(dllexport) void FVO_TrafficCensusForgetV1(std::uint32_t vehicle){
    using vehicleflight::g_owners;using vehicleflight::g_ownersLock;
    std::lock_guard<std::mutex> ownersGuard(g_ownersLock);
    auto* r=g_owners.Find(vehicle);if(!r||r->owner!=planowner::Owner::Traffic)return;  // never end a stronger owner's job
    g_owners.Release(vehicle);g_censusWhy.erase(vehicle);logger::Write("OBS system=plan.owner vehicle=%08X event=census_forget reason=pair_gone",vehicle);
}
// V2 preserves the V1 array stride and identifies the formerly unused default marker.
#ifndef FVO_RELEASE  // diagnostic builds only, as before (the spot-motion proof exists only there)
extern "C" __declspec(dllexport) int FVO_SpotMotionProofV1(
    std::uint32_t vehicle, unsigned long long after, struct FvoSpotMotionProofV1* out)
{
    if (!vehicle || !out || out->size != sizeof(*out) || out->version != 1) return 0;
    std::lock_guard lock(vehicleflight::g_spotLock);
    const auto it = vehicleflight::g_spotProof.find(vehicle);
    if (it == vehicleflight::g_spotProof.end() || it->second.started <= after) return 0;
    const auto& proof = it->second;
    out->body = proof.body;
    out->direction = int(proof.direction);
    out->started = proof.started;
    out->completed = proof.completed;
    out->builtAt = proof.builtAt;
    out->steps = proof.steps;
    out->moved = proof.moved;
    return 1;
}
#endif
extern "C" __declspec(dllexport) int FVO_RegisterFieldBoxV2(FvoFieldBoxProviderV1 provider){
    if(fieldboxapi::provider&&fieldboxapi::provider!=provider)return 0;
    fieldboxapi::provider=provider;logger::Write("OBS system=plan.field event=provider version=2 registered=%d reason=addon_offers_field_boxes",int(provider!=nullptr));return 1;
}
extern "C" __declspec(dllexport) int FVO_RegisterFieldRecoveryV1(FvoFieldRecoveryV1 provider){
    if(fieldgate::recoveryProvider&&fieldgate::recoveryProvider!=provider)return 0;
    fieldgate::recoveryProvider=provider;return 1;
}
// CH-1: the raw per-update health sample for the addon's Health. Optional export: an older addon never calls it.
extern "C" __declspec(dllexport) int FVO_PairHealthSampleV1(std::uint32_t vehicle,FvoPairHealthSampleV1* out){
    if(!out||out->size!=sizeof(FvoPairHealthSampleV1)||out->version!=1)return 0;
    std::lock_guard sl(vehicleflight::g_spotLock);const auto it=vehicleflight::g_healthSample.find(vehicle);
    if(it==vehicleflight::g_healthSample.end()||GetTickCount64()-it->second.at>2000)return 0;
    *out=it->second;return 1;}
// CH-4: Health asks for a spot test of a doubtful pair (the core runs it when the car is still and hidden). Optional export.
extern "C" __declspec(dllexport) void FVO_SpotTestRequestV1(std::uint32_t vehicle){
    if(!vehicle)return;std::lock_guard sl(vehicleflight::g_spotLock);vehicleflight::g_spotRequest[vehicle]=GetTickCount64();}
