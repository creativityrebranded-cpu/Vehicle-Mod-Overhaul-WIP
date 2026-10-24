#include "vehicleflight.h"
#include "vehiclediag.h"
#include "bindings.h"
#include "features.h"
#include "overrides.h"
#include "profileclass.h"
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
#include "navfield.h"
#include "recovery.h"
#include "environment.h"
#include "bodybinding.h"
#include "hoverlogic.h"
#include "freeroll.h"
#include "physicsstep.h"
#include "vtolphysics.h"
#include "vtolai.h"
#include "vtolassist.h"
#include "vtolhooks.h"
#include "vehiclecamera.h"
#include "hulllogic.h"
#include "integrationlogic.h"
#include "boostlogic.h"
#include "altcontrol.h"
#include "ramlogic.h"
#include "targetsession.h"
#include "scantarget.h"
#include "npcvehicles.h"
#include "notify.h"
#include "prefetch.h"
#include "routeplan.h"
#include "telemetry.h"
#include "vehicleworld.h"
#include "vehiclehits.h"
#include "realvehicles.h"
#include "travelmode.h"

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
#include <type_traits>
#include <unordered_map>

// Rover flight (0.57.0): a connected flight model that pushes the rover's own physics body instead of overwriting it.
//  - On / off: double-tap the game's vertical boost (Space). Also off: settle onto the ground, hit something, or leave
//    the vehicle.
//  - Controls (mouse-aim layout): the mouse aims, the nose follows the camera in yaw and pitch. A / D: roll.
//    W: boost (hold) at the selected gear; S: brake (hold); tap Alt: cycle gears 1-4; hold Alt: descend.
//    Neither W nor S: coast. Up / down keys: lift.
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
namespace vehicleflight
{
	// Internal: the navigation settings of the profile driving this pilot (pilot 0 = the player's vehicle), stored with each goal and read by the navigation service.
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
		constexpr double kSideLeak = 1.2;         // 1/s, sideways speed (across the nose) bleeds off ...
		constexpr double kRedirect = 0.7;         // ... and this share of it turns along the nose (follow the path)
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
		constexpr double kYawResp = 4.0;          // 1/s, how fast the yaw spin follows its wanted rate
		constexpr double kTiltResp = 5.0;         // 1/s, the same for pitch and roll
		// Gears (W / S): thrust (times kThrust) and top speed per gear; gear 0 coasts.
		constexpr double kGearThrust[5] = { 0.0, 0.6, 1.0, 1.6, 2.5 };
		constexpr double kGearTop[5] = { 60.0, 30.0, 60.0, 100.0, 150.0 };
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
		// Cruise height: just over the highest ground in the next corridor samples (rises fast, sinks slowly).
		constexpr double kTerrainClear = 2.5;        // m over the highest ground ahead
		constexpr double kCruiseSink = 1.5;          // m/s the cruise height comes down at most
		// Goal height (2026-10-17): a pilot given the target's height flies at it (never under the terrain clearance),
		// up to kGoalMaxAbove over the ground, and comes down at kGoalSink (a diving target is followed down).
		constexpr double kGoalMaxAbove = 200.0;      // m
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
		constexpr int kSweepSlot0 = 24, kSweepRays = 3, kSweepRing = 16;
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
		constexpr int kCeilSlot0 = 48;
		constexpr double kCeilUp = 30.0, kCeilAhead = 15.0, kCeilAheadUp = 12.0, kCeilClear = 5.0, kVehicleTop = 2.0;  // (kCeilClear 2026-10-20: was 2.5; the user still felt a pull toward ceilings)
		constexpr double kSelfHitBand = 3.0;      // m: a front sample this close to the rover's own height is the rover
		constexpr double kLandHeight = 1.0;       // m: settling under this (not climbing) for kLandMs ends the hover
		constexpr ULONGLONG kLandMs = 1500;
		constexpr unsigned kProbeMaxAge = 2;
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
			hulllogic::Limits hull;
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
			double vz = 0;                 // m/s
			bool rollAimOn = false;
			double rollAim = 0;            // rad
			// Free roll (freeroll.h, the settings menu): the player's own hover turns about its body axes with no roll limit.
			// The bank test (the , key with the detailed log on) uses the same body-axis turning for its held bank.
			bool freeRoll = false, assist = true, bankTest = false;
			bool physical = false;
			bool physicalFault = false;
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
			static const char* names[kPilots] = { "Rover", "NPC rover 1", "NPC rover 2", "NPC rover 3", "NPC rover 4", "NPC rover 5", "NPC rover 6" };
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
			world::Snapshot snap; if(!world::Vehicles(snap))return false;
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
		struct DriveOut
		{
			std::atomic<bool> on{ false };
			std::atomic<std::uint32_t> vehicleId{ 0 };
			std::atomic<float> steer{ 0 }, accel{ 0 }, brake{ 0 };
			std::atomic<bool> boost{ false }, vert{ false };
		} g_driveOut[kPilots];

		using Step_t = physicsstep::Callback;
		Step_t g_original = nullptr;
		std::atomic<unsigned> g_nativeStepSamples{0}, g_nativeStepSkipped{0};
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
		std::atomic<std::uintptr_t> g_driverVehicle{ 0 }, g_lastDrivenVehicle{ 0 };
		std::atomic<unsigned> g_physicalSteps{0}, g_physicalFailures{0};
		std::atomic<unsigned> g_physicalReject{0};
		std::atomic<float> g_physicalCollective{0}, g_physicalMass{0};
		std::atomic<float> g_physicalLinearError{0},g_physicalAngularError{0};
		bool g_physicalHooksReady = false;
		std::atomic<unsigned> g_occupiedSteps{ 0 }, g_unoccupiedSteps{ 0 };

		std::atomic<bool> g_boostRequest{false},g_meterBoost{false};
		std::atomic<std::uintptr_t> g_meterVehicle{0};
		std::atomic<bool> g_meterExhausted{false};
		void BoostDetour(std::uintptr_t component, std::uintptr_t context)
		{
			const std::uintptr_t ctxVehicle = context ? *reinterpret_cast<std::uintptr_t*>(context + 0x100) : 0;
			int hp = -1;  // the pilot hovering this vehicle
			for (int i = 0; ctxVehicle && i < kPilots && hp < 0; ++i) {
				if (g_hoverVehicles[i].load() == ctxVehicle) hp = i;
			}
			const bool drive = hp >= 0 && g_driveOut[hp].on.load(std::memory_order_relaxed);  // driving, not hovering
			const bool hovering = hp >= 0 && !drive;
			const auto vehicle = ctxVehicle;
			const auto driven = g_driverVehicle.load(std::memory_order_relaxed);
			const bool manual=component&&driven&&ctxVehicle==driven;
			const bool request=manual&&g_boostRequest.load();
			float charge=0,delay=0;
			if(manual&&hovering) {
				charge=At<float>(component,0x238);delay=At<float>(component,0x23C);
				// Hover replaces native boost force. The meter still uses this vehicle's native timings.
				At<std::uint8_t>(component,0x240)=0;At<std::uint8_t>(component,0x241)=0;
			} else if(manual) At<std::uint8_t>(component,0x240)=request?1:0;
			g_boostOriginal(component,context);
			if(manual) {
				if(g_meterVehicle.exchange(vehicle)!=vehicle)g_meterExhausted=false;
				if(hovering) {
					const auto config=context?At<std::uintptr_t>(context,0x110):0;
					const double dt=context?std::clamp(double(At<float>(context,0x16C)),0.0,0.1):0;
					const auto meter=boostlogic::Step(charge,delay,request,g_meterExhausted,dt,
						config?At<float>(config,0x418):3.0,config?At<float>(config,0x420):1.0,config?At<float>(config,0x424):4.0);
					At<float>(component,0x238)=float(meter.charge);At<float>(component,0x23C)=float(meter.delay);
					g_meterBoost.store(meter.active);g_meterExhausted=meter.exhausted;
				}else g_meterBoost.store(request&&At<float>(component,0x238)>0);
			}
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
		void PhysicalDampDetour(std::uintptr_t instance,float seconds)
		{
			if (!PhysicalOwns(instance)) g_physicalDampOriginal(instance,seconds);
		}
		void PhysicalClampDetour(std::uintptr_t instance,std::uintptr_t world,float seconds)
		{
			if (!PhysicalOwns(instance)) g_physicalClampOriginal(instance,world,seconds);
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
				const double floor=std::max(kMinClearance,std::fabs(R[2])*s.hull.width+std::fabs(F[2])*s.hull.length+std::fabs(U[2])*s.hull.height+1.5);
				if (z-s.groundUnder<floor) vertical=std::max(vertical,std::min(kLiftSpeed,(floor-(z-s.groundUnder))*3+1));
			}
			// AI pilots always fly assisted (the planners expect a stable chassis), with stronger slip thrusters and strafe.
			vtol::Command command{rates,s.thrustAcc*s.hull.acceleration,s.autopilot?0.0:s.lift,vertical,s.hull.brake,s.cap,
				std::max({s.hull.width,s.hull.length,s.hull.height}),s.autopilot||s.assist,s.braking,
				s.autopilot?s.strafe*kStrafeAcc:0.0,s.autopilot?vtolai::kSideAcceleration:3.0};
			const auto output=vtol::Solve(body,command,s.physicalState,dt);
			if (!output.valid) { g_physicalReject=8; if (player) g_physicalFailures.fetch_add(1,std::memory_order_relaxed); return result; }
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

		std::uint64_t StepDetour(std::uintptr_t vehicle, std::uintptr_t world, float seconds)
		{
			g_nativeStepSamples.fetch_add(1, std::memory_order_relaxed);
			g_nativeStepSeconds.store(seconds, std::memory_order_relaxed);
			if (physicsstep::ModelSeconds(seconds) == 0.0) g_nativeStepSkipped.fetch_add(1, std::memory_order_relaxed);
			// This hook sees every vehicle. Unowned steps must never default to the player's controller.
			vehiclediag::NoteStep(vehicle);
			const auto body=PhysicsBodyId(vehicle);const auto now=GetTickCount64();
			bodybinding::Binding bindings[kPilots];
			for(int i=0;i<kPilots;++i){auto&s=g_shs[i];std::lock_guard lk(s.lock);
				const bool owns=bodybinding::Contains({s.sceneBodies,s.sceneCount},body) && (!s.instance.load()||s.bodyId.load()==body);
				bindings[i]={owns?body:0,s.vehicle.load()&&(s.active||s.groundOn||(s.guardInstance&&now<s.guardUntil))};}
			const int owner=bodybinding::Owner(body,bindings);
			NoteSeen(body);
			if(owner<0){int claims=0;for(const auto&bb:bindings)if(bb.enabled&&bodybinding::Matches(body,bb.body))++claims;
				if(claims>1){g_ambiguous.fetch_add(1,std::memory_order_relaxed);g_ambiguousBody.store(body,std::memory_order_relaxed);}}
			Shared*s=owner>=0?&g_shs[owner]:nullptr;
			if(s){
				{std::lock_guard lk(s->lock);if(!bodybinding::Contains({s->sceneBodies,s->sceneCount},body)||
					(s->instance.load()&&s->bodyId.load()!=body))s=nullptr;else{s->bodyId.store(body);s->instance.store(vehicle);}}
				if(s)CheckCollision(*s,vehicle);
			}
			if (s) PreparePhysical(*s,vehicle);
			for (auto& o : g_shs) if (&o!=s && o.physInstance.load()==vehicle) PhysicalRelease(o);  // a chassis nobody owns now
			const auto result = g_original(vehicle, world, seconds);
			if(!s)return result;
			const auto ref=s->vehicle.load();
			if (ref && ref == g_lastDrivenVehicle.load(std::memory_order_relaxed)) {
				(ref == g_driverVehicle.load(std::memory_order_relaxed) ? g_occupiedSteps : g_unoccupiedSteps).fetch_add(1, std::memory_order_relaxed);
			}
            {std::lock_guard lock(s->lock);
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

		std::uint64_t StepModel(Shared& g_sh, std::uintptr_t vehicle, std::uint64_t result, float seconds)
		{
			const double dt = physicsstep::ModelSeconds(seconds);
			if (dt == 0.0) return result;
			std::unique_lock lk(g_sh.lock);
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
			const double bodyHeading = std::atan2(double(F[0]), double(F[1]));
			g_sh.bodyHeading = bodyHeading;
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
			g_sh.wYaw += (yawWant - g_sh.wYaw) * (1.0 - std::exp(-dt * kYawResp));
			g_sh.wPitch += (pitchWant - g_sh.wPitch) * (1.0 - std::exp(-dt * kTiltResp));
			g_sh.wRoll += (rollWant - g_sh.wRoll) * (1.0 - std::exp(-dt * kTiltResp));
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
			const double leak = side * kSideLeak;
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
			const double clearance=g_sh.landing?0.15:std::max(kMinClearance,std::fabs(R[2])*g_sh.hull.width+std::fabs(F[2])*g_sh.hull.length+std::fabs(U[2])*g_sh.hull.height+1.5);
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
			bool wDown = false, sDown = false, boostDown = false;
			bool wHold = false;            // double-tapped boost (2026-10-24, was W): boost stays on until boost or brake is pressed
			ULONGLONG wLastPress = 0;      // the last boost press (the double-tap window)
			bool liftoff = false;          // climbing to kLiftoffHeight after switching on near the ground
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
			TripProgress trip;
            recovery::Budget recovery;
			bool apClimb = false;          // the first climb straight up, before moving
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
			// autopilot stuck check
			double stuckX = 0, stuckY = 0;
			ULONGLONG stuckAt = 0;
			int stuckCount = 0;
			// navigation
			double detourAng = 0;          // rad added to the bearing to the target (negative: left)
			ULONGLONG detourAt = 0;
			double cruiseExtra = 0;        // m above the skim height
			ULONGLONG clearSince = 0;
			double castHeading = 0;
			bool haveCast = false;
			double bestDist = 1e9;         // the lost check: the distance to the target, and the vehicle's place, when the
			ULONGLONG bestAt = 0;          // current kLostMs window started
			double lostX = 0, lostY = 0;
			ULONGLONG reverseUntil = 0;    // backing away (the nose still turns toward the detour)
			double floorZ = -1e9;          // a floor the ground rays miss (learned from a push up)
			ULONGLONG floorUntil = 0;
			bool trailOn = false;          // following the player's trail
			unsigned long long trailSeq = 0;  // the trail point flown to
			double cruiseHold = kCallSkim; // m: the terrain-ahead hold height
			double throttle = 1.0;         // 0..1 share of the gear's thrust
			bool aiGears = false;          // this frame: the AI gear tables apply
			int gearCap = 4;               // the highest gear the path allows
			ULONGLONG capClearSince = 0;
			int lastF = 0;                 // last frame's straight-ahead corridor verdict
			int lastL = 0, lastR = 0;      // the left / right corridors
			double lastNeed = 0;           // metres to climb to pass the straight-ahead corridor
			double laneZ = 0;              // metres added to the cruise height by the lane planner
			bool drive = false;            // drive mode: hover off, the game's controls
			ULONGLONG driveSince = 0, hopUntil = 0, drvLog = 0;
			travelmode::Hold travel;       // NPC pilots: hover or drive, held against flicker (travelmode.h)
			double cramped = 0;            // score that switches drive mode on
			double drvX = 0, drvY = 0, drvSp = 0, drvHead = 0, steerSign = 1.0, steerResp = 0, drvSteer = 0;
			ULONGLONG drvT = 0;
			ULONGLONG airSince = 0;
			double steerSecs = 0, stuckGX = 0, stuckGY = 0;
			int aiGear = 1;
			ULONGLONG shiftAt = 0;
			bool taxiOrbit = false;        // Auto's orbit around the marker is on (AutoMode::Orbit)
			// The short-horizon planner (MPC): last plan, progress watch, escape.
			double mpcVz = 0;
            double mpcOmega = 0, mpcFactor = 1.0, mpcSpeed = 0;
			bool mpcOk = false;
			ULONGLONG mpcAt = 0, mpcLogAt = 0;
			double mpcBestDist = 1e9;      // the closest it got to the target in this progress window
			ULONGLONG mpcProgAt = 0;       // when that window started
			double mpcStuckX = 0, mpcStuckY = 0;
			ULONGLONG mpcEscapeUntil = 0;
			unsigned mpcInvalid = 0;       // candidates that collided in the last plan
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
			std::uint32_t ramActor=0,ramVehicle=0;
			std::uint32_t vehId = 0;
			bool vehYield = true;          // this vehicle gives way to that one (right of way by rank)
			ULONGLONG vehLogAt = 0;
			unsigned vehDropped = 0;       // probe samples dropped as "a vehicle, not ground" (logged)
		};
		State g_gs[kPilots];
		world::Snapshot g_vsnap;       // this frame's vehicle list (main thread)
		bool g_vsnapOk = false;

		// Main thread, caller holds Shared::lock. Missing identity disables writes until the scene proves ownership.
		struct RamOrder{std::uint32_t vehicle=0,actor=0;int pilot=0;ULONGLONG started=0,lastAt=0;game::Vec3 last{};bool keep=false;}g_ram;  // keep: an NPC manoeuvre's ram, the flight goes on after it

		void BindBody(Shared&s,std::uintptr_t ref)
		{
			std::uint32_t ids[16]={},count=0;
			for(unsigned i=0;g_vsnapOk&&i<g_vsnap.count&&i<64;++i)if(g_vsnap.v[i].ref==ref){
				const auto&v=g_vsnap.v[i];count=std::min(v.bodyCount,16u);std::copy(v.bodyIds,v.bodyIds+count,ids);
				s.hull=hulllogic::For(v.half[0],v.half[1],v.half[2]);break;}
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
			for (std::uint32_t i = 0; i < g_vsnap.count && i < 64; ++i) {
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
			for (const auto r : modscene::LoadedRefs()) {
				if (!r || !npcvehicles::LooksLikeActor(r) || npcvehicles::IsDead(r) || At<std::uint32_t>(r,off::kFormId)==g.ramActor) continue;
				const auto aboard=EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(r);
				if (aboard == vehicle || (aboard && g.ramVehicle && At<std::uint32_t>(aboard,off::kFormId)==g.ramVehicle)) continue;
				const auto p = At<game::Vec3>(r, off::kRefPosition);
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
		int RankOf(const world::VehicleInfo& v)
		{
			if (v.who == world::VehicleInfo::Who::Player || v.formId == OwnRoverId()) return 3;
			npcvehicles::JobInfo jobs[8];
			const int n = npcvehicles::Jobs(jobs, 8);
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
			for (std::uint32_t i = 0; i < g_vsnap.count && i < 64; ++i) {
				if (g_vsnap.v[i].formId == self) me = &g_vsnap.v[i];
			}
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const double myR = me ? VehRadius(*me) : 2.5, myVx = me ? me->vel[0] : 0.0, myVy = me ? me->vel[1] : 0.0;
			const double sp = std::hypot(myVx, myVy);
			for (std::uint32_t i = 0; i < g_vsnap.count && i < 64; ++i) {
				const auto& v = g_vsnap.v[i];
				if (!v.formId || v.formId == self || v.formId==g.ramVehicle) continue;
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
				for (std::uint32_t i = 0; i < g_vsnap.count && i < 64; ++i) {
					if (g_vsnap.v[i].formId == g.vehId) other = RankOf(g_vsnap.v[i]);
				}
				g.vehYield = myRank <= other;
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
		// Kept across relaunches: a pilot stuck again soon after climbing out is moved instead.
		struct Escape { int count = 0; ULONGLONG at = 0; } g_escape[kPilots];
		struct Bumps { int count = 0; ULONGLONG at = 0; ULONGLONG ev = 0; } g_bumps[kPilots];
		struct HeldCommand { Command c; ULONGLONG until = 0; } g_cmds[kPilots];

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
			auto freeHere = [](int vk) { return controlmap::KeyFreeIn(vk, "Vehicle"); };
			// N and U are the user's summon / cover keys (2026-10-20): never the hover toggle.
			for (const int vk : std::initializer_list<int>{ 'J', 'K', 'Y', 'O', 'P', 'B' }) {
				if (freeHere(vk) && controlmap::KeyFree(vk)) {
					g_keys.toggle = vk;
					break;
				}
			}
			g_keys.rollLeft = 'A';
			g_keys.rollRight = 'D';
			const int lifts[][2] = { { 'Q', 'Z' }, { 'F', 'C' }, { 'X', 'C' }, { VK_OEM_4, VK_OEM_6 } };
			for (const auto& pr : lifts) {
				const auto used = [&](int vk) { return vk == g_keys.toggle || vk == g_keys.rollLeft || vk == g_keys.rollRight; };
				if (!used(pr[0]) && !used(pr[1]) && freeHere(pr[0]) && freeHere(pr[1])) {
					g_keys.up = pr[0];
					g_keys.down = pr[1];
					break;
				}
			}
			// The user chose these exact keys: no fallback moves them elsewhere; a game binding on them is only logged.
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
			g_keys.stance = 0;
			for (const int vk : { 'Y', 'O', 'P', 'B', 'K', 'J', 'G' }) {
				if (vk != g_keys.toggle && vk != g_keys.up && vk != g_keys.down && controlmap::Claim("flight", "stance", vk)) { g_keys.stance = vk; break; }
			}
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

		// ---- The player's rover, remembered by form id (resolved through the form map, never a stale pointer) ----
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
		} g_move;
		ULONGLONG g_movedAt = 0;  // the last summon move (the HUD says "MOVED TO YOU" for a few seconds)

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

		void AskMove(std::uintptr_t player)
		{
			if(vehiclehits::Wrecked(g_roverId)||IsDisabled(g_roverId))return;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const double yaw = At<game::Vec3>(player, off::kRefRotation).z;
			// 12 m behind the player and 3 m to the right (out of view), facing the way the player faces.
			const double fx = std::sin(yaw), fy = std::cos(yaw), rx = std::cos(yaw), ry = -std::sin(yaw);
			g_move = { true, pp.x + rx * 3.0 - fx * 12.0, pp.y + ry * 3.0 - fy * 12.0, pp.z, yaw, GetTickCount64() };
			prefetch::RequestGround(prefetch::kSlotRescue0, float(g_move.x), float(g_move.y), pp.z + 60.0f, 0.0f, 400.0f);
		}

		void FinishMove(std::uintptr_t player)
		{
			if (!g_move.pending) return;
			if(vehiclehits::Wrecked(g_roverId)||IsDisabled(g_roverId)){g_move.pending=false;return;}
            const auto rover=Rover();
            const auto site=environment::CheckSite(g_roverId,g_move.x,g_move.y,g_move.z,g_move.yaw);
            if(site.verdict!=environment::Verdict::Safe) {
                if(GetTickCount64()-g_move.asked<4000)return;
                g_move.pending=false;notify::Show("Summon: no safe pickup site");logger::Write("Summon refused: %s",site.reason);return;
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
				logger::Write("Rover call: moved to (%.1f, %.1f, %.1f) next to the player (ground %s)", pos[0], pos[1], pos[2], "validated footprint and boarding side");
				g_movedAt = GetTickCount64();
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::Write("Rover call: move raised an exception");
			}
		}

		void Stop(int idx, const char* why)
		{
			g_shs[idx].physOn.store(false); PhysicalRelease(g_shs[idx]);
			if (!idx) vehiclecamera::Reset();
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const auto releasedVehicle = g_hoverVehicles[idx].load();
			const bool wreck=releasedVehicle&&vehiclehits::Wrecked(At<std::uint32_t>(releasedVehicle,off::kFormId));
            g_cmds[idx]={};
            g.travel = {};  // a new flight decides hover or drive afresh
            g.groundHug = false;
			if(g_ram.pilot==idx)g_ram={};
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
			g_driveOut[idx].on.store(false);
			routeplan::Forget(idx);
			g_forwardBoosts[idx] = false;
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
			g.playerAuto = false;
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

		void Start(int idx, std::uintptr_t vehicle)
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
			if (idx) {
				logger::Write("%s: hover on at (%.0f, %.0f, %.0f)", Who(idx), pos.x, pos.y, pos.z);
				return;
			}
			logger::Write("Rover flight: on at (%.0f, %.0f, %.0f)", pos.x, pos.y, pos.z);
			notify::Log("Hover on - [W] accelerate [S] brake [Alt tap] gear [%s] boost (double tap: stays on)  [%s]/[%s] up/down  [Alt hold] down  [%s]/[%s] turn  [%s] off",
				bindings::Label(bindings::Action::Boost, VK_LSHIFT).c_str(), bindings::Label(bindings::Action::LiftUp, g_keys.up).c_str(),
				bindings::Label(bindings::Action::LiftDown, g_keys.down).c_str(), bindings::Label(bindings::Action::TurnLeft, g_mk.left).c_str(), bindings::Label(bindings::Action::TurnRight, g_mk.right).c_str(),
				bindings::Label(bindings::Action::HoverToggle, g_keys.toggle).c_str());
			telemetry::Record("rover_flight_on", "\"pos\":[%.0f,%.0f,%.0f]", pos.x, pos.y, pos.z);
		}
		void Start(std::uintptr_t vehicle) { Start(0, vehicle); }

		void ReadSamples(int idx)
		{
			State& g = g_gs[idx];
			if (!g.haveAsked || GetTickCount64() < g.freshAt) {
				return;
			}
			for (int i = 0; i < kSamples; ++i) {
				float h = -3.4e38f;
				unsigned age = 0;
				g.asked[i].valid = prefetch::LatestGround(ModelSlot(idx, i), h, age) && age <= kProbeMaxAge && h > -1e30f && h != -500.0f;
				g.asked[i].h = h;
				if (i != kCentre && i != kLeft && i != kRight) {  // ahead: the terrain or the surface under the flight level, whichever is higher
					float lh = -3.4e38f;
					unsigned la = 0;
					if (prefetch::LatestGround(LowSlot(idx, i), lh, la) && la <= kProbeMaxAge && lh > -1e30f && lh != -500.0f && (!g.asked[i].valid || lh > g.asked[i].h)) {
						g.asked[i].h = lh;
						g.asked[i].valid = true;
					}
				}
				if (g.asked[i].valid && InVehicleSpace(SelfId(idx), g.asked[i].x, g.asked[i].y, h, true)) {  // another vehicle's roof is no ground
					g.asked[i].valid = false;
					++g.vehDropped;
				}
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
					prefetch::RequestGroundMask(CorridorSlot(idx, c * 3 + k), float(q.x), float(q.y), pos.z + float(kCastAbove), 0.0f, 8000.0f, kTerrainLayers);  // terrain
					prefetch::RequestGround(LowSlot(idx, 7 + c * 3 + k), float(q.x), float(q.y), pos.z + float(kFlightLevel), 0.0f, 8000.0f);              // under the flight level
				}
				const double range = std::max(kRayMin, speed * kRayLead);
				const float from[3] = { float(pos.x + std::sin(hd) * kRayStart), float(pos.y + std::cos(hd) * kRayStart), float(pos.z + kRayLift) };
				const float to[3] = { float(from[0] + std::sin(hd) * range), float(from[1] + std::cos(hd) * range), from[2] };
				prefetch::RequestRay(idx * 3 + c, from, to, kWallLayers);
				g.rayRange[c] = range;
			}
			g.haveCorr = true;
			{
				const float from[3] = { float(pos.x), float(pos.y), float(pos.z + 1.5) };
				const float up[3] = { from[0], from[1], float(from[2] + kCeilUp) };
				const float ahead[3] = { float(pos.x + std::sin(heading) * kCeilAhead), float(pos.y + std::cos(heading) * kCeilAhead), float(from[2] + kCeilAheadUp) };
				prefetch::RequestRay(kCeilSlot0 + idx * 2, from, up, kWallLayers);
				prefetch::RequestRay(kCeilSlot0 + idx * 2 + 1, from, ahead, kWallLayers);
			}
			if (g.autopilot && navmap::HullEnabled()) {
				// The vehicle's own shape swept along the planned heading (hull-sized: what a thin ray cannot know).
				const std::uintptr_t inst = g_shs[idx].instance.load(std::memory_order_relaxed);
				if (inst) {
					g.hullRange = std::max(kRayMin, speed * kRayLead);
					prefetch::RequestSweep(idx, inst, float(std::sin(heading)), float(std::cos(heading)), float(g.hullRange), kWallLayers);
				}
			}
			if (g.autopilot) {
				const double sweepRange = std::clamp(speed * 2.5, 30.0, 80.0);
				for (int k = 0; k < kSweepRays; ++k) {
					const double hd = double((g.sweepPhase * kSweepRays + k) % kSweepRing) * (6.283185307179586 / kSweepRing);
					const float from[3] = { float(pos.x + std::sin(hd) * kRayStart), float(pos.y + std::cos(hd) * kRayStart), float(pos.z + kRayLift) };
					const float to[3] = { float(from[0] + std::sin(hd) * sweepRange), float(from[1] + std::cos(hd) * sweepRange), from[2] };
					prefetch::RequestRay(kSweepSlot0 + idx * kSweepRays + k, from, to, kWallLayers);
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
				g.corr[i].valid = prefetch::LatestGround(CorridorSlot(idx, i), h, age) && age <= kProbeMaxAge && h > -1e30f && h != -500.0f;
				g.corr[i].h = h;
				{
					float lh = -3.4e38f;
					unsigned la = 0;
					if (prefetch::LatestGround(LowSlot(idx, 7 + i), lh, la) && la <= kProbeMaxAge && lh > -1e30f && lh != -500.0f && (!g.corr[i].valid || lh > g.corr[i].h)) {
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
				g.rayValid[c] = prefetch::LatestRay(idx * 3 + c, g.ray[c], age) && age <= kProbeMaxAge;
				auto& h = g.ray[c];
				const bool rawHit = h.hit;
				if (g.rayValid[c] && h.hit && player && std::hypot(h.x - pp.x, h.y - pp.y) < 2.5 && h.z > pp.z - 1.0 && h.z < pp.z + 3.0) h.hit = false;
				bool onVehicle = false;
				if (g.rayValid[c] && h.hit && InVehicleSpace(SelfId(idx), h.x, h.y, h.z)) {  // a vehicle: the warning's job, not a wall
					h.hit = false;
					onVehicle = true;
					++g.vehDropped;
				}
				if (g.rayValid[c]) navmap::NoteRay(h.from, h.to, rawHit && !onVehicle, h.x, h.y, h.z, h.layer, h.normalOk);  // characters and vehicles are free space to the map, not walls
			}
			// Ceiling: the lowest hit above the vehicle of the two ceiling rays (another vehicle's underside does not count).
			g.ceilValid = false;
			for (int k = 0; k < 2; ++k) {
				prefetch::RayHit ch;
				unsigned age = 0;
				if (!prefetch::LatestRay(kCeilSlot0 + idx * 2 + k, ch, age) || age > kProbeMaxAge || !ch.hit) continue;
				if (ch.z < g.askZ + 1.0 || InVehicleSpace(SelfId(idx), ch.x, ch.y, ch.z)) continue;
				if (!g.ceilValid || ch.z < g.ceilZ) { g.ceilZ = ch.z; g.ceilValid = true; }
			}
			g.hullValid = g.hullHit = false;
			if (g.autopilot && navmap::HullEnabled()) {
				prefetch::SweepHit sw;
				unsigned age = 0;
				g.hullValid = prefetch::LatestSweep(idx, sw, age) && age <= kProbeMaxAge;
				if (g.hullValid) {
					g.hullDist = sw.distance;
					g.hullHit = sw.hit && sw.distance >= 1.0f;  // (a hit inside the first metre is the vehicle's own surroundings)
					if (sw.hit && InVehicleSpace(SelfId(idx), sw.x, sw.y, sw.z)) {
						g.hullHit = false;
						sw.hit = false;
						++g.vehDropped;
					}
					if (!sw.hit || g.hullHit) navmap::NoteRay(sw.from, sw.to, sw.hit, sw.x, sw.y, sw.z, sw.layer, false);
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
					if (prefetch::LatestRay(kSweepSlot0 + idx * kSweepRays + k, sh, age) && age <= kProbeMaxAge) {
						if (sh.hit && InVehicleSpace(SelfId(idx), sh.x, sh.y, sh.z)) sh.hit = false;  // vehicles move: never walls in the map
						navmap::NoteRay(sh.from, sh.to, sh.hit, sh.x, sh.y, sh.z, sh.layer, sh.normalOk);
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

		// ---- Short-horizon planner (MPC, 2026-10-20, the user: "something closer to MPC for short path planning ... with the things added for
		// cheap escapes ... make decision making snappier"). Every kMpcEveryMs: kMpcOmegas turn rates x kMpcFactors speed shares (held for the
		// horizon), the same turn rates turning only for the first second, and the last plan; each rolled kMpcSteps x kMpcDt ahead with a simple
		// hover model (turn rate, speed easing toward its target), scored on every step against the nav map (a remembered solid, ground above
		// the flight level = a collision; bump spots and unknown space cost), every other vehicle's straight-line prediction, and at the end the
		// distance left to the target, the heading to it, the change from the last plan and lost speed. The first step of the best is flown.
		// All candidates colliding = no plan: the old detour / trail / climb rules steer (the first cheap escape). No progress for kMpcStuckMs =
		// escape for kMpcEscapeMs: the player's trail when there is one, else the target term is swapped for "away from where it got stuck".
		constexpr ULONGLONG kMpcEveryMs = 50, kMpcStuckMs = 6000, kMpcEscapeMs = 3000;
		constexpr int kMpcSteps = 10;
		constexpr bool kMpcEnabled = true;
		constexpr double kMpcDt = 0.25;
		constexpr double kMpcOmegas[9] = { -1.2, -0.8, -0.45, -0.2, 0.0, 0.2, 0.45, 0.8, 1.2 };
		constexpr double kMpcFactors[3] = { 1.0, 0.6, 0.3 };
		constexpr double kMpcAccel = 6.0, kMpcDecel = 10.0, kMpcHalfWidth = 2.5;
		constexpr double kMpcClimb = 5.0;   // m/s of climb the height hold makes while it flies toward rising ground
		constexpr double kKnownDecel = 15.0;  // m/s^2: the speed cap plans to stop within the mapped area ahead at this
		struct MpcOut { bool ok = false, escape = false; double omega = 0, speed = 0, cost = 0, vz = 0; unsigned invalid = 0, total = 0; };

		// One rollout: its cost, or a negative value when it collides.
		double MpcRollout(int idx, const game::Vec3& pos, double heading, double v0, double vmax, double omega, double factor, bool turnFirstOnly,
			double tx, double ty, bool escape, double sx, double sy, std::uint32_t self, bool useObjects, double halfWidth, double halfLength, double halfHeight, double selfRadius, const navfield::Field& field, double vz, double lateral=0)
		{
			const State& g = g_gs[idx];
			double x = pos.x, y = pos.y, h = heading, v = v0, cost = 0;
			const double vt = vmax * factor;
			for (int k = 1; k <= kMpcSteps; ++k) {
				const double t = k * kMpcDt;
				const double w = (turnFirstOnly && t > 1.0) ? 0.0 : omega;
				h += w * kMpcDt;
				v += std::clamp(vt - v, -kMpcDecel * g_shs[idx].hull.acceleration * kMpcDt, kMpcAccel * g_shs[idx].hull.acceleration * kMpcDt);
				x += (std::sin(h)*v+std::cos(h)*lateral)*kMpcDt;
                y += (std::cos(h)*v-std::sin(h)*lateral)*kMpcDt;
                // Sweep each rollout segment, at the candidate altitude, with both hull sides.
                const double rx=std::cos(h)*halfWidth,ry=-std::sin(h)*halfWidth;
                const int samples=std::max(1,int(std::ceil(std::max(std::hypot(v,lateral)*kMpcDt/2.0,std::fabs(vz)*kMpcDt))));
                for(int n=1;n<=samples;++n) {
                    const double age=t-kMpcDt+kMpcDt*n/samples;
                    const double zz=pos.z+vz*age;
                    const double px=x-(std::sin(h)*v+std::cos(h)*lateral)*kMpcDt*(1.0-double(n)/samples);
                    const double py=y-(std::cos(h)*v-std::sin(h)*lateral)*kMpcDt*(1.0-double(n)/samples);
                    for(int nose=-1;nose<=1;++nose)for(int side=-1;side<=1;++side)for(int height=-1;height<=1;++height) {
                        const double sampleZ=zz+height*halfHeight;
                        const auto m=navmap::At(px+rx*side+nose*std::sin(h)*halfLength,py+ry*side+nose*std::cos(h)*halfLength,sampleZ);
                        if(m.blocked || (useObjects&&m.objBlocked) || (m.haveGround&&m.ground+(height==0?2.5:0.1)>sampleZ)) return -1;
                        if(std::fabs(vz)>0.1 && !m.known) return -1;
                        if(n==samples) {cost+=25.0*m.contacts;if(!m.known)cost+=1.5;}
                    }
                }
				// Other vehicles, predicted straight on.
				if (g_vsnapOk) {
					for (std::uint32_t i = 0; i < g_vsnap.count && i < 64; ++i) {
						const auto& o = g_vsnap.v[i];
						if (!o.formId || o.formId == self || o.formId==g.ramVehicle) continue;
						if (std::fabs(double(o.pos[2]) - (pos.z+vz*t)) > 2.0 * (VehHalfZ(o) + 2.0)) continue;
						const double ox = o.pos[0] + o.vel[0] * t, oy = o.pos[1] + o.vel[1] * t;
						const double gap = std::hypot(x - ox, y - oy) - VehRadius(o) - selfRadius;
						if (gap < 0.5 && k > 1) return -1.0;
						if (gap < 8.0) cost += (8.0 - gap) * 15.0;
					}
				}
			}
			if (escape) cost -= 2.0 * std::hypot(x - sx, y - sy);  // escape: get away from the stuck spot
			else {
				const double remaining = field.Value(x, y, pos.z+vz*kMpcSteps*kMpcDt);
				if (remaining >= 1e12) return -1.0;
				cost += remaining;                                          // distance left
				cost += 6.0 * std::fabs(Wrap(std::atan2(tx - x, ty - y) - h));              // facing the target at the end
			}
			cost += 12.0 * std::fabs(omega - g.mpcOmega) + 10.0 * (1.0 - factor);       // smooth, and keep the speed
			cost += 0.4*std::fabs(vz-g.mpcVz);
            return cost + 1000.0;  // (offset: always positive, even with the escape's reward; a collision is -1)
		}

		MpcOut MpcPlan(int idx, std::uintptr_t vehicle, const game::Vec3& pos, double heading, double sp, double vmax, double tx, double ty, double tz, const char* who)
		{
			State& g = g_gs[idx];
			MpcOut out;
			const ULONGLONG now = GetTickCount64();
			if (now - g.mpcAt < kMpcEveryMs) {  // between plans: the last one
				out.ok = g.mpcOk;
				out.omega = g.mpcOmega;
				out.speed = g.mpcSpeed;
                out.vz = g.mpcVz;
				out.escape = now < g.mpcEscapeUntil;
				return out;
			}
			g.mpcAt = now;
			// Stuck watch: the vehicle itself moved under 4 m within kMpcStuckMs while still far from the target = escape. (Not "no closer to the
			// target": an escort keeping a steady gap to a moving player never gets closer and is not stuck.)
			const double dist = std::hypot(tx - pos.x, ty - pos.y);
			if (!g.mpcProgAt || std::hypot(pos.x - g.mpcStuckX, pos.y - g.mpcStuckY) > 4.0 || dist < 15.0) {
				g.mpcProgAt = now;
				g.mpcStuckX = pos.x;
				g.mpcStuckY = pos.y;
			}
			if (now >= g.mpcEscapeUntil && dist > 15.0 && now - g.mpcProgAt > kMpcStuckMs) {
				g.mpcEscapeUntil = now + kMpcEscapeMs;
				g.mpcStuckX = pos.x;
				g.mpcStuckY = pos.y;
				g.mpcProgAt = now;
				g.mpcBestDist = dist;
				logger::Write("%s: planner: no progress for %.0f s (%.0f m from the target): escaping", who, kMpcStuckMs / 1000.0, dist);
			}
			const bool escape = now < g.mpcEscapeUntil;
			out.escape = escape;
			const std::uint32_t self = At<std::uint32_t>(vehicle, off::kFormId);
			// The game's object boxes (navseed) count, unless the vehicle itself sits inside one: then that box is a poor fit (a big composite
			// object around open space) and only the probes' map is trusted for this plan.
			const bool useObjects = !navmap::At(pos.x, pos.y, pos.z).objBlocked;
			// The flown vehicle's own size (2026-10-20): its half-width (plus a margin) for the map, its footprint radius against other
			// vehicles, from the world list's bounds; the old fixed 2.5 m when it is not listed.
			double halfWidth = kMpcHalfWidth, halfLength=2.3, halfHeight=kVehicleTop, selfRadius = kMpcHalfWidth;
			for (std::uint32_t i = 0; g_vsnapOk && i < g_vsnap.count && i < 64; ++i) {
				if (g_vsnap.v[i].formId != self) continue;
				const auto& nv=PilotNavOf(idx);
                const double margin=nv.margin>0?nv.margin:(nv.squeeze?0.2:0.5);
                halfWidth = std::max(0.25,double(g_vsnap.v[i].half[0])) + margin;
				halfLength=std::max(0.25,double(g_vsnap.v[i].half[1]))+margin;
				halfHeight=std::max(0.25,double(g_vsnap.v[i].half[2]));
				selfRadius = VehRadius(g_vsnap.v[i]);
                const auto& v=g_vsnap.v[i];
                const double ox=v.center[0]-v.pos[0],oy=v.center[1]-v.pos[1];
                halfWidth+=std::fabs(ox*std::cos(heading)-oy*std::sin(heading));
                halfLength+=std::fabs(ox*std::sin(heading)+oy*std::cos(heading));
			}
			const auto& field = navfield::Get(pos.x, pos.y, pos.z, tx, ty, halfWidth, useObjects, tz);
			double best = 1e18, bestW = 0, bestF = 1.0, bestVz = 0;
			unsigned invalid = 0, total = 0;
			auto consider = [&](double w, double f, bool firstOnly, double vz) {
				++total;
				w=std::clamp(w,-g_shs[idx].hull.YawAt(sp),g_shs[idx].hull.YawAt(sp));
				const double c = MpcRollout(idx, pos, heading, sp, vmax, w, f, firstOnly, tx, ty, escape, g.mpcStuckX, g.mpcStuckY, self, useObjects, halfWidth, halfLength, halfHeight, selfRadius, field, vz);
				if (c <= 0.0) { ++invalid; return; }
				if (c < best) { best = c; bestW = w; bestF = f; bestVz = vz; }
			};
            for(const double vz : {-5.0,-2.5,0.0,2.5,5.0}) {
                for(const double w:kMpcOmegas) {
                    for(const double f:kMpcFactors) consider(w,f,false,vz);
                    consider(w,1.0,true,vz);
                }
                consider(0.0,0.0,false,vz); // stop and climb/descend in a clear column
            }
            consider(g.mpcOmega,g.mpcFactor,false,g.mpcVz);
			out.invalid = invalid;
			out.total = total;
			g.mpcInvalid = invalid;
			g.mpcOk = best < 1e17;
			if (g.mpcOk) {
				g.mpcOmega = bestW;
				g.mpcFactor = bestF;
				g.mpcSpeed = std::max(0.0, vmax * bestF);
                g.mpcVz = bestVz;
			}
			out.ok = g.mpcOk;
			out.omega = g.mpcOmega;
			out.speed = g.mpcSpeed;
			out.cost = best;
            out.vz = g.mpcVz;
			if (logger::Verbose() && now - g.mpcLogAt >= 2000) {
				g.mpcLogAt = now;
				logger::Write("%s: planner: %s turn %+.2f rad/s at %.0f m/s (%u of %u plans collide)%s", who, g.mpcOk ? "flying" : "no plan,", bestW, g.mpcSpeed, invalid, total,
					escape ? ", escaping" : "");
			}
			return out;
		}

        bool ValidateCommand(int idx,std::uintptr_t vehicle,const game::Vec3& pos,double heading,double speed,double cap,const Goal& goal,const Command& c) {
            if(!std::isfinite(c.throttle)||!std::isfinite(c.strafe)||!std::isfinite(c.vertical)||!std::isfinite(c.yawRate))return false;
            const auto& g=g_gs[idx]; const auto& hull=g_shs[idx].hull;
            const double w=(c.set&Command::kYaw)?std::clamp(c.yawRate,-hull.YawAt(speed),hull.YawAt(speed)):g.mpcOmega;
            double forward=speed,slip=0;
            for(unsigned k=0;g_vsnapOk&&k<g_vsnap.count&&k<64;++k)if(g_vsnap.v[k].ref==vehicle){const auto&v=g_vsnap.v[k];forward=v.vel[0]*std::sin(heading)+v.vel[1]*std::cos(heading);slip=v.vel[0]*std::cos(heading)-v.vel[1]*std::sin(heading);}
            const double lever=(c.set&Command::kThrottle)?std::clamp(c.throttle,-1.0,1.0):0;
            const double targetSpeed=(c.set&Command::kBrake)&&c.brake?0:std::clamp(forward+lever*6.0*hull.acceleration,-cap,cap);
            const double vz=(c.set&Command::kVertical)?std::clamp(c.vertical,-8.0,8.0):g.mpcVz;
            const double side=slip+((c.set&Command::kStrafe)?std::clamp(c.strafe,-1.0,1.0)*8.0*hull.acceleration:0);
            const auto id=At<std::uint32_t>(vehicle,off::kFormId);
            const auto& nv=PilotNavOf(idx); const double margin=nv.margin>0?nv.margin:(nv.squeeze?0.2:0.5);
            double width=hull.width+margin,length=hull.length+margin,height=hull.height;
            for(unsigned k=0;g_vsnapOk&&k<g_vsnap.count&&k<64;++k)if(g_vsnap.v[k].formId==id) {
                const auto& v=g_vsnap.v[k];const double x=v.center[0]-v.pos[0],y=v.center[1]-v.pos[1];
                width+=std::fabs(x*std::cos(heading)-y*std::sin(heading));length+=std::fabs(x*std::sin(heading)+y*std::cos(heading));
            }
            const bool objects=!navmap::At(pos.x,pos.y,pos.z).objBlocked;
            const auto& field=navfield::Get(pos.x,pos.y,pos.z,goal.x,goal.y,width,objects,goal.z);
            return MpcRollout(idx,pos,heading,forward,targetSpeed,w,1.0,false,goal.x,goal.y,false,0,0,id,objects,width,length,height,std::hypot(width,length),field,vz,side)>0;
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

	bool Active() { return g.active; }
	bool PhysicalCameraActive(std::uintptr_t vehicle) {
		return vehicle && g_sh.physOn.load(std::memory_order_acquire) && g_sh.physNeedsDriver.load() && g_sh.physInstance.load() &&
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
		// The height a vertical boost clears on the ground ([nav] jump= in seamless.ini, default 4 m): an obstacle taller than this
		// makes an NPC vehicle hover (travelmode).
		double JumpHeight()
		{
			static double value = 0;
			if (value <= 0) {
				value = 4.0;
				PWSTR docs = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
					const std::wstring ini = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\seamless.ini";
					CoTaskMemFree(docs);
					value = std::clamp(double(GetPrivateProfileIntW(L"nav", L"jump", 4, ini.c_str())), 1.0, 30.0);
				}
			}
			return value;
		}

		// Drive mode off: hover on again (a lift-off from where it is), the game's controls are no longer overridden.
		void LeaveDrive(int idx, std::uintptr_t vehicle, const char* why)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			(void)vehicle;
			g.drive = false;
			g_driveOut[idx].on.store(false);
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

		// Stuck/lost recovery never teleports a vehicle. A failed trip holds until retried or given a new goal.
		void EscapeMove(int idx, std::uintptr_t vehicle, const Goal& goal)
		{
            auto& state=g_gs[idx];
            state.recovery.failed=true;
            logger::Write("%s: recovery exhausted; holding for Retry, Skip, or Cancel",Who(idx));
            (void)vehicle; (void)goal;
        }

		// A contact at the vehicle's place, for the shared nav map (up = pushed up by a floor, else a side bump ahead).
		void NoteContactAt(std::uintptr_t vehicle, bool up)
		{
			const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
			const double h = At<game::Vec3>(vehicle, off::kRefRotation).z;
			navmap::NoteContact(pos.x, pos.y, pos.z, up, std::sin(h), std::cos(h));
		}

		// The heading sampler for a blocked straight way (navmap::ChooseHeading): sets g.detourAng to the best open heading
		// the shared map knows of; false = nothing usable (the old +-35 degree stepping runs). The profile's nav settings
		// shape the hull: margin (default 1.2 m), squeeze (0.4 m), minClearance.
		bool SamplerDetour(int idx, State& g, const game::Vec3& pos, double bearing, double sp, const char* who, int l, int r, double needL, double needR)
		{
			const auto& nv = PilotNavOf(idx);
			navmap::HeadingQuery q;
			q.pos[0] = pos.x; q.pos[1] = pos.y; q.pos[2] = pos.z;
			q.bearing = bearing;
			q.haveAvoid = true;
			q.avoidOffset = g.detourAng;
			q.currentOffset = g.detourAng;
			q.speed = sp;
			q.halfWidth = 1.3 + (nv.margin > 0 ? double(nv.margin) : (nv.squeeze ? 0.4 : 1.2));
			q.clearance = nv.minClearance > 0 ? double(nv.minClearance) : kAvoidClear;
			q.climbRate = kHoldMaxVz;
			const auto c = navmap::ChooseHeading(q);
			navmap::NoteSampler(c.found);
			if (!c.found) return false;
			const double oldSide = g.detourAng != 0 ? (g.detourAng > 0 ? 1.0 : -1.0)
				: (l < 2 && (r == 2 || needL <= needR)) ? -1.0 : (r < 2 ? 1.0 : (needL <= needR ? -1.0 : 1.0));
			const double before = g.detourAng;
			g.detourAng = std::clamp(c.offset, -kDetourMax, kDetourMax);
			if (std::fabs(g.detourAng - before) > 0.05) {
				logger::Write("%s: path blocked; sampler: heading %+.0f deg off the target (open %.0f m, score %.2f; %d usable, %d blocked, %d unknown); the old rule would turn %s",
					who, g.detourAng * 57.2958, c.freeDist, c.score, c.usable, c.blocked, c.unknown, oldSide < 0 ? "left" : "right");
			}
			return true;
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
		// One frame of a hovering pilot: the assist, avoidance, height hold and (autopilot) the flight to the goal.
		void Fly(int idx, std::uintptr_t vehicle, Goal goal, bool focused)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const char* wf = idx ? Who(idx) : "Rover flight";
			const char* wc = idx ? Who(idx) : "Rover call";
			g.ramActor=g.ramVehicle=0;
            {
                const auto& nav=PilotNavOf(idx);const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);const auto now=GetTickCount64();
                const bool requested=g.autopilot&&goal.hold&&nav.landOnArrival&&std::hypot(goal.x-pos.x,goal.y-pos.y)<=goal.stop+2;
                if(requested) {
                    const auto site=environment::CheckSite(At<std::uint32_t>(vehicle,off::kFormId),goal.x,goal.y,pos.z,At<game::Vec3>(vehicle,off::kRefRotation).z);
                    if(site.verdict==environment::Verdict::Safe){g.landGround=site.ground;g.landProofAt=now;g.landHeight=g_sh.hull.height+0.1;
                        for(unsigned k=0;g_vsnapOk&&k<g_vsnap.count&&k<64;++k)if(g_vsnap.v[k].ref==vehicle)g.landHeight=std::max(0.2,double(g_vsnap.v[k].half[2])-(g_vsnap.v[k].center[2]-g_vsnap.v[k].pos[2])+0.1);
                    }
                    if(site.verdict==environment::Verdict::Unsafe)g.landProofAt=0;
                }
                g.landing=requested&&g.landProofAt&&now>=g.landProofAt&&now-g.landProofAt<500;
                if(g.landing){goal.z=g.landGround+g.landHeight;g.apClimb=false;}
                else if(nav.height!=vcontract::NavParams::Height::Auto || nav.minClearance>0) {
                    const auto map=navmap::At(pos.x,pos.y,pos.z);
                    if(map.haveGround){const double floor=map.ground+integrationlogic::Clearance(int(nav.height),nav.minClearance,g_sh.hull.height,kCallSkim)+EnvClimb();goal.z=std::isfinite(goal.z)?std::max(goal.z,floor):floor;}
                }
                std::lock_guard lock(g_sh.lock);g_sh.landing=g.landing||g.groundHug;
            }
			if(!g.autopilot&&g_ram.pilot==idx)g_ram={};
			if(g_ram.vehicle==At<std::uint32_t>(vehicle,off::kFormId)&&g_ram.pilot==idx) {
				const auto now=GetTickCount64();
				const auto actor=RefById(g_ram.actor);
				const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
				const bool valid=actor&&!npcvehicles::IsDead(actor)&&Worldspace(actor)==Worldspace(vehicle);
				const auto target=valid?At<game::Vec3>(actor,off::kRefPosition):pos;
				bool contact;{std::lock_guard lk(g_sh.lock);contact=g_sh.collided;}
				const double distance=std::hypot(target.x-pos.x,target.y-pos.y);
				if(!g.autopilot||ramlogic::Finished(valid,now-g_ram.started,contact,distance,g_sh.hull.length+3)) {
					logger::Write("Ram order: vehicle %08X -> actor %08X ended (%s)",g_ram.vehicle,g_ram.actor,
						!g.autopilot?"autopilot off":!valid?"target lost":now-g_ram.started>=15000?"timeout":"impact");
					const bool keep=g_ram.keep;g_ram={};
					if(keep)return;  // a manoeuvre's ram: the NPC's fight takes the pilot back next frame
					if(idx&&At<std::uint32_t>(vehicle,off::kFormId)==OwnRoverId())npcvehicles::SetCrewOrder(OwnRoverId(),npcvehicles::CrewOrder::Hold);
					Stop(idx,"ram ended");return;
				}
				g.ramActor=g_ram.actor;const auto other=EngineFn<std::uintptr_t(*)(std::uintptr_t)>(addr::kPlayerVehicle)(actor);
				g.ramVehicle=other?At<std::uint32_t>(other,off::kFormId):0;
				const double dt=(now-g_ram.lastAt)/1000.0;double vx=0,vy=0;
				if(g_ram.lastAt&&dt>0&&dt<1){vx=(target.x-g_ram.last.x)/dt;vy=(target.y-g_ram.last.y)/dt;if(std::hypot(vx,vy)>150)vx=vy=0;}
				g_ram.last=target;g_ram.lastAt=now;
				const double lead=ramlogic::Lead(distance,std::hypot(g_sh.v[0],g_sh.v[1]));
				const double pass=g_sh.hull.length+4;
				goal.x=target.x+vx*lead+(distance>0.1?(target.x-pos.x)/distance*pass:0);
				goal.y=target.y+vy*lead+(distance>0.1?(target.y-pos.y)/distance*pass:0);
				goal.z=target.z+1;goal.stop=0;goal.hold=false;goal.cruise=true;goal.cruiseSpeed=26*g_sh.hull.acceleration;goal.keepGoing=true;
				// Straight line, flat out (2026-10-21, the user: "fly in a straight line as quickly as possible"): the ram steers the nose at the led
				// point itself, full throttle in top gear; no planned path, detours or slowing for traffic. The autopilot keeps only the height
				// (the target's, never under the terrain clearance).
				g.drive=false;
				const double speed=std::hypot(g_sh.v[0],g_sh.v[1]);
				const double err=Wrap(std::atan2(goal.x-pos.x,goal.y-pos.y)-At<game::Vec3>(vehicle,off::kRefRotation).z);
				Command c;
				c.set=Command::kThrottle|Command::kYaw|Command::kStrafe|Command::kGear|Command::kBrake;
				c.throttle=ramlogic::Throttle(err);c.yawRate=ramlogic::SteerRate(err,g_sh.hull.YawAt(speed));
				c.strafe=0;c.gear=4;c.brake=false;c.holdMs=200;
				g_cmds[idx]={c,now+c.holdMs};
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
					if (b.count >= kBumpLimit) {
						// Cramped: climb out over it and keep flying (contacts never end a flight; the stuck check, which
						// watches actual progress, is what moves a vehicle that truly cannot get through).
						b.count = 0;
						g.cruiseExtra = std::min(kCruiseExtraMax, g.cruiseExtra + 6.0);
						g.apClimb = true;
						g.autopilotSince = nowMs;
						logger::Write("%s: %d contacts in %.0f s (cramped); climbing out (+%.0f m), still flying", wc, kBumpLimit, kBumpWindowMs / 1000.0, g.cruiseExtra);
					}
					if (up && !(g.wallAhead && g.wallDist < kWallStandoff + 4.0) && g.wallL > 4.0 && g.wallR > 4.0) {
						g.floorZ = pos.z;
						g.floorUntil = nowMs + kFloorMs;
						g.apClimb = true;
						g.autopilotSince = nowMs;
						logger::Write("%s: pushed up %.0f m/s at z %.1f: something under it the ground rays miss; holding above it", wc, delta, pos.z);
					}
					else {
						g.reverseUntil = nowMs + kReverseMs;
						g.detourAng = g.detourAng != 0 ? -g.detourAng : kAvoidSide;
						logger::Write("%s: bumped (%.0f m/s); backing off, detour %+.0f deg", wc, delta, g.detourAng * 57.2958);
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

            if(g.autopilot && !g.routeHold && !g.ramActor) {  // a ram is a bounded straight run: no stuck recovery
                const double goalHeight=std::isfinite(goal.z) ? goal.z : 0;
                if(g.recovery.Update(GetTickCount64(),pos.x,pos.y,pos.z,goal.x,goal.y,goalHeight,
                    AtDestination(vehicle,goal.x,goal.y,goal.z,goal.stop),!groundOk || (!matched && !g.drive))) {
                    navfield::Reset();
                    g.mpcEscapeUntil=GetTickCount64()+kMpcEscapeMs;
                    logger::Write("%s: recovery attempt %u of 3",wc,g.recovery.attempts);
                }
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
			// Boost (Shift / RB): boost while held; double-tap it: boost stays on; the next boost or brake press releases it
			// (2026-10-24, the user: the forward boost's own button, instead of double-tapping W).
			{
				const bool boostPress = boostHeld && !boostWasDown;
				const bool sPress = sHeld && !sWasDown;
				if (boostPress) {
					const ULONGLONG nowMs = GetTickCount64();
					if (g.wHold) {
						g.wHold = false;
						g.wLastPress = 0;
					}
					else if (g.wLastPress && nowMs - g.wLastPress <= kDoubleTapMs) {
						g.wHold = true;
						g.wLastPress = 0;
						logger::Write("%s: boost held (double-tapped boost)", wf);
					}
					else {
						g.wLastPress = nowMs;
					}
				}
				if (sPress && g.wHold) {
					g.wHold = false;
				}
			}
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
				const double tx = goal.x, ty = goal.y, stopR = goal.stop;
				const double dx = tx - pos.x, dy = ty - pos.y;
				const double dist = std::hypot(dx, dy);
				apBearing = std::atan2(dx, dy);
				// Trail: steer at the next point of the player's path instead of straight at the goal.
				if (g.trailOn) {
					if (!goal.keepGoing || goal.cruise || goal.hold || dist < kTrailOff || !g_trailCount) {
						g.trailOn = false;
						logger::Write("%s: trail off (%.0f m from the goal)", wc, dist);
					}
					else {
						if (!TrailHas(g.trailSeq)) g.trailSeq = TrailNearest(pos.x, pos.y);
						// skip ahead to any of the next few points that is closer (cut corners the player looped)
						for (int k = 1; k <= 6 && TrailHas(g.trailSeq + k); ++k) {
							const auto& a = TrailAt(g.trailSeq + k);
							const auto& c = TrailAt(g.trailSeq);
							if (std::hypot(a.x - pos.x, a.y - pos.y) < std::hypot(c.x - pos.x, c.y - pos.y)) g.trailSeq += k;
						}
						while (TrailHas(g.trailSeq + 1) && std::hypot(TrailAt(g.trailSeq).x - pos.x, TrailAt(g.trailSeq).y - pos.y) < kTrailReach) ++g.trailSeq;
						const auto& w = TrailAt(g.trailSeq);
						apBearing = std::atan2(w.x - pos.x, w.y - pos.y);
					}
				}
				apSteer = dist > 1.0;
				apGear = dist > 80.0 ? 4 : dist > 40.0 ? 3 : 2;  // the gear from the distance left
				const double brakeAt = stopR + std::max(sp*kCallBrakeSlack,g_sh.hull.StopDistance(sp));
				boosting = dist > brakeAt;
				sBrake = dist <= brakeAt && sp > 1.0;
				g.throttle = 1.0;
				g.aiGears = false;
				if (goal.keepGoing) {
					// Following: match the player's speed along the way, plus a share of the gap (no surge and brake).
					const double vt = (goal.cruise || goal.hold) ? 0.0 : std::max(0.0, g_pvx * std::sin(apBearing) + g_pvy * std::cos(apBearing));
					const double desired = goal.cruise ? goal.cruiseSpeed : std::clamp(vt + kMatchGain * (dist - stopR), 0.0, kGearTop[4]);
					// Gear manager: one gear per speed band, shifted with hysteresis; the gear's top caps the speed.
					int& ag = g.aiGear;
					ag = std::clamp(ag, 1, 4);
					const ULONGLONG nowMs = GetTickCount64();
					if (nowMs - g.shiftAt >= kShiftMs) {
						const int before = ag;
						if (ag < g.gearCap && desired > kAiGearTop[ag] * kShiftUp) ++ag;
						else if (ag > 1 && desired < kAiGearTop[ag - 1] * kShiftDown && sp < kAiGearTop[ag - 1]) --ag;
						if (ag != before) {
							g.shiftAt = nowMs;
							if (logger::Verbose()) logger::Write("%s: gear %d -> %d (want %.0f m/s, at %.0f)", wc, before, ag, desired, sp);
						}
					}
					if (ag > g.gearCap) {
						if (logger::Verbose()) logger::Write("%s: gear %d -> %d (path cap)", wc, ag, g.gearCap);
						ag = g.gearCap;
						g.shiftAt = nowMs;
					}
					const double want = std::min(desired, kAiGearTop[ag]) * EnvSpeed();  // night / bad weather: slower
					apGear = ag;
					g.aiGears = true;
					g.throttle = std::clamp((want - sp) / 4.0, 0.0, 1.0);  // proportional: eases off as it nears the speed
					boosting = g.throttle > 0.05;
					sBrake = sp > want + 2.0;
				}
				if (g.apClimb) {
					const bool freshGround = groundOk && g.groundAt >= g.autopilotSince;  // never the previous spot's ground
					const bool overFloor = GetTickCount64() >= g.floorUntil || pos.z >= g.floorZ + kFloorClear;
					if ((freshGround && overFloor && above >= kCallClimb + g.cruiseExtra - 0.5) || GetTickCount64() - g.autopilotSince > kCallClimbMs + ULONGLONG(g.cruiseExtra * 150.0)) {
						g.apClimb = false;
						// What the physics step saw (2026-10-21: every climb ended at -0.6 m; the line alone could not tell a missing body from
						// a lift that does nothing): steps run on the body, its binding, the lift asked, gravity measured, vertical speed.
						logger::Write("%s: climbed to %.1f m; flying to the target (physics: %u steps, body %s, lift %.2f, gravity %.1f, vz %.2f, launches %u, bad velocities %d)",
							wc, above, g_sh.steps, g_sh.instance.load() ? "bound" : "NOT BOUND", g_sh.lift, g_sh.gEst, double(g_sh.lastV[2]), g_sh.launches, g_sh.fixedVel);
						if (!g_sh.steps) logger::Write("%s: no physics steps: %s", wc, StepDiag(g_sh).c_str());
					}
					else {
						boosting = false;  // straight up first
						sBrake = sp > 0.5;
						apSteer = false;
					}
				}
				// Lost (behind terrain, circling in a pocket): moved next to the target. Judged per kLostMs window, against
				// where the window started, never against the best distance ever: a moving target (the player flying off,
				// a pursuit orbit) made "60 m now, best 5 m" read as lost while the vehicle was flying fine and chasing
				// (20:59-21:02: the rover and NPC rover 1 teleported 4 times in 3 minutes). A vehicle that covered
				// kLostMove in the window is chasing, not lost, however far behind it is.
				{
					const ULONGLONG tNow = GetTickCount64();
					if (!g.bestAt) {
						g.bestAt = tNow;
						g.bestDist = dist;
						g.lostX = pos.x;
						g.lostY = pos.y;
					}
					else if (tNow - g.bestAt >= kLostMs) {
						const double gained = g.bestDist - dist;
						const double moved = std::hypot(pos.x - g.lostX, pos.y - g.lostY);
						const bool lost = !goal.cruise && !goal.hold && dist > kLostFar && gained < kLostGain && moved < kLostMove;
						if (lost) {
							logger::Write("%s: lost (%.0f m from the target, %.0f m closer and %.0f m moved in %.0f s); moving it to the target", wc, dist, gained, moved,
								kLostMs / 1000.0);
						}
						g.bestAt = tNow;
						g.bestDist = dist;
						g.lostX = pos.x;
						g.lostY = pos.y;
						if (lost) {
							EscapeMove(idx, vehicle, goal);
							return;
						}
					}
				}
				if (!goal.keepGoing && AtDestination(vehicle, goal.x, goal.y, goal.z, goal.stop)) {
					logger::Write("%s: arrived %.1f m from the target", wc, dist);
					Stop(idx, "arrived");
					return;
				}
				if (!goal.keepGoing && GetTickCount64() - g.autopilotSince > goal.timeoutMs) {
					Stop(idx, "call timed out");
					return;
				}
				// Stuck: accelerating but moved under 1 m in 3 s (twice in a row).
				{
					double& lx = g.stuckX;
					double& ly = g.stuckY;
					ULONGLONG& lt = g.stuckAt;
					int& stuck = g.stuckCount;
					const ULONGLONG nowMs = GetTickCount64();
					if (std::hypot(goal.x - g.stuckGX, goal.y - g.stuckGY) > 5.0) {  // a new target: the window starts over
						g.stuckGX = goal.x;
						g.stuckGY = goal.y;
						lt = 0;
					}
					if (!lt || nowMs - g.autopilotSince < 100) { lx = pos.x; ly = pos.y; lt = nowMs; stuck = 0; }
					else if (nowMs - lt >= 3000) {
						const double moved = std::hypot(pos.x - lx, pos.y - ly);
						// Holding a spot (Taxi waiting for a marker, or holding over it) is meant to stand still: never "stuck"
						// (21:05:01: Auto waiting with no marker backed out and climbed).
						const bool holdingStill = goal.hold && std::hypot(goal.x - pos.x, goal.y - pos.y) < 8.0;
						stuck = (!g.apClimb && boosting && moved < 1.0 && !holdingStill) ? stuck + 1 : 0;
						lx = pos.x; ly = pos.y; lt = nowMs;
						if (stuck >= 2) {
							auto& e = g_escape[idx];
							if (e.count && nowMs - e.at > kEscapeWindowMs) e.count = 0;
							if (e.count == 0) {
								e.count = 1;
								e.at = nowMs;
								g.reverseUntil = nowMs + kReverseMs;
								g.detourAng = g.detourAng != 0 ? -g.detourAng : kAvoidSide;  // the other way out
								if (goal.keepGoing && !goal.cruise && !goal.hold && !g.trailOn && g_trailCount > 3) {
									g.trailOn = true;
									g.trailSeq = TrailNearest(pos.x, pos.y);
								}
								stuck = 0;
								logger::Write("%s: stuck (moved %.1f m in 3 s); backing out, detour %+.0f deg", wc, moved, g.detourAng * 57.2958);
								g.cramped = std::min(kCrampedMax, g.cramped + 2.0);
							}
							else if (e.count == 1) {
								e.count = 2;
								e.at = nowMs;
								g.cruiseExtra = std::min(kCruiseExtraMax, g.cruiseExtra + 12.0);
								g.detourAng = -g.detourAng;  // try the other side
								g.apClimb = true;
								g.autopilotSince = nowMs;
								stuck = 0;
								logger::Write("%s: stuck (moved %.1f m in 3 s); climbing out to %.0f m", wc, moved, kCallClimb + g.cruiseExtra);
							}
							else {
								e.count = 0;
								logger::Write("%s: stuck again (moved %.1f m in 3 s); moving it to the target", wc, moved);
								EscapeMove(idx, vehicle, goal);
								return;
							}
						}
					}
				}
			}
			const bool boost = boosting;  // booster visuals while accelerating
			const double boostMultiplier=paidBoost?boostlogic::kMultiplier:1.0;
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
			}
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
				if (g.autopilot && apSteer && !g.apClimb) {
					// Corridors here are cast along the target bearing plus the detour (castHeading), not the velocity.
					const ULONGLONG nowMs = GetTickCount64();
					const bool hold = nowMs - g.detourAt < kDetourHoldMs;
					// The planner first (drive mode keeps its own steering); no plan = the old rules below.
					MpcOut mo;
					if (!g.drive && kMpcEnabled) {
						const double tdx = std::sin(apBearing), tdy = std::cos(apBearing);
						const double tdist = g.trailOn && TrailHas(g.trailSeq) ? std::hypot(TrailAt(g.trailSeq).x - pos.x, TrailAt(g.trailSeq).y - pos.y)
							: std::hypot(goal.x - pos.x, goal.y - pos.y);
						const double reach = std::max(tdist, 20.0);
						// Plans stay inside what is mapped (2026-10-20): a speed that can stop within the known area ahead (the object scan's circle,
						// at least the forward ray's reach). At gear 4 this caps about 85 m/s with 250 m mapped ahead.
						const double known = std::max(navmap::KnownAhead(pos.x, pos.y, bodyHeading), g.rayRange[0]);
						const double knownCap = std::sqrt(2.0 * std::min(kKnownDecel,g_sh.hull.brake) * std::max(0.0,known));
						if (top > knownCap) top = knownCap;
						mo = MpcPlan(idx, vehicle, pos, bodyHeading, sp, std::max(top, 5.0), pos.x + tdx * reach, pos.y + tdy * reach, goal.z, wc);
						// Escape with the player's trail available: follow it (the second cheap escape).
						if (mo.escape && std::hypot(goal.x-pos.x,goal.y-pos.y)>kTrailOff && !g.trailOn && goal.keepGoing && !goal.cruise && !goal.hold && g_trailCount > 3) {
							g.trailOn = true;
							g.trailSeq = TrailNearest(pos.x, pos.y);
							logger::Write("%s: planner escape: following your trail (%u points)", wc, g_trailCount);
						}
					}
					if (mo.ok) {
						// The planner steers: its first step's turn rate as the heading to hold, its speed as the cap; no detour steps.
						g.detourAng = 0;
						apBearing = bodyHeading + mo.omega / kApYawGain;
						top = std::min(top, mo.speed);
					}
					else if (f == 2 && !hold && goal.keepGoing && !goal.cruise && !goal.hold && !g.trailOn && g_trailCount > 3 && std::hypot(goal.x - pos.x, goal.y - pos.y) > kTrailOff) {
						g.trailOn = true;
						g.trailSeq = TrailNearest(pos.x, pos.y);
						g.detourAng = 0;
						g.detourAt = nowMs;
						logger::Write("%s: path blocked; following your trail (%u points)", wc, g_trailCount);
					}
					else if (f == 2 && !hold && navmap::SamplerEnabled() && SamplerDetour(idx, g, pos, apBearing, sp, wc, l, r, needL, needR)) {
						g.detourAt = nowMs;
						g.clearSince = 0;
					}
					else if (f == 2 && !hold) {
						const double side = g.detourAng != 0 ? (g.detourAng > 0 ? 1.0 : -1.0)
							: (l < 2 && (r == 2 || needL <= needR)) ? -1.0 : (r < 2 ? 1.0 : (needL <= needR ? -1.0 : 1.0));
						if (std::fabs(g.detourAng) < kDetourMax - 1e-3) {
							g.detourAng += side * kAvoidSide;
							logger::Write("%s: path blocked; detour %+.0f deg", wc, g.detourAng * 57.2958);
						}
						else {
							g.cruiseExtra = std::min(kCruiseExtraMax, g.cruiseExtra + 6.0);
							logger::Write("%s: blocked on that side too; flying higher (+%.0f m)", wc, g.cruiseExtra);
						}
						g.detourAt = nowMs;
						g.clearSince = 0;
					}
					else if (f < 2 && g.detourAng != 0 && !hold) {
						const int back = g.detourAng > 0 ? l : r;  // the corridor one step back toward the target
						if (back == 0) {
							g.detourAng = std::fabs(g.detourAng) <= kAvoidSide + 1e-3 ? 0.0 : g.detourAng - (g.detourAng > 0 ? 1.0 : -1.0) * kAvoidSide;  // (the sampler's headings are not multiples of a step)
							g.detourAt = nowMs;
							logger::Write("%s: way back toward the target clear; detour %+.0f deg", wc, g.detourAng * 57.2958);
						}
					}
					if (f == 0 && g.detourAng == 0) {
						if (!g.clearSince) g.clearSince = nowMs;
						if (g.cruiseExtra > 0 && nowMs - g.clearSince > 3000) g.cruiseExtra = std::max(0.0, g.cruiseExtra - 2.0 * dt);  // back down slowly
					}
					else {
						g.clearSince = 0;
					}
					// Close in front and slow: back away while the nose turns to the detour (as a three-point turn).
					const auto& c0 = g.corr[0];
					if (f == 2 && sp < 6.0 && c0.valid && c0.h + kAvoidClear - pos.z > 3.0 && nowMs >= g.reverseUntil) {
						g.reverseUntil = nowMs + kReverseMs;
						logger::Write("%s: obstacle %.0f m ahead; backing away", wc, c0.d);
					}
					apBearing += g.detourAng;
						{
							// The lane planner: predict this and every other vehicle ~10 s ahead in 3D and pick a lane / side / speed
							// that does not meet (routeplan.h).
							routeplan::Query rq;
							rq.selfId = At<std::uint32_t>(vehicle, 0x28);
							rq.ignoreId=g.ramVehicle;
							for(unsigned i=0;g_vsnapOk&&i<g_vsnap.count&&i<64;++i)if(g_vsnap.v[i].formId==rq.selfId){
								for(int axis=0;axis<3;++axis)rq.half[axis]=g_vsnap.v[i].half[axis];break;}
							rq.priority = PilotNavOf(idx).priority > 0 ? PilotNavOf(idx).priority : (idx == 0 ? 120 : 60);
							rq.pos[0] = pos.x; rq.pos[1] = pos.y; rq.pos[2] = pos.z;
							rq.dir[0] = float(std::sin(apBearing));
							rq.dir[1] = float(std::cos(apBearing));
							rq.speed = float(std::max(g.drive ? g.drvSp : sp, 6.0));
							rq.ground = g.drive;
							rq.distToGoal = float(std::hypot(goal.x - pos.x, goal.y - pos.y));
							const routeplan::Plan pl = routeplan::Update(idx, rq);
							if (!mo.ok) apBearing += pl.yawOffset;  // (the planner already weighs the other vehicles)
							g.laneZ = g.drive ? 0.0 : pl.laneZ;
							if (pl.speedCap >= 0.0f) {
								top = std::min(top, double(pl.speedCap));
								if (pl.speedCap < 1.0f) {
									boosting = false;
									sBrake = (g.drive ? g.drvSp : sp) > 1.0;
								}
							}
						}
					want = f == 1 ? 1 : 0;
					if (!mo.ok && (g.detourAng != 0 || f == 2)) top = std::min(top, f == 2 ? 12.0 : kDetourSlowTo);
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
				if (g.apClimb || braking) pitchT = 0.0;
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
			if (g.autopilot) {
				// The path's gear cap (applied by the gear manager next frame).
				double lo = 1e9, hi = -1e9;
				for (int k = 0; k < 3; ++k) {
					if (!g.corr[k].valid) continue;
					lo = std::min(lo, g.corr[k].h);
					hi = std::max(hi, g.corr[k].h);
				}
				const bool rough = hi > lo && hi - lo > kRoughSpread;
				const ULONGLONG nowMs = GetTickCount64();
				int capNow = 4;
				const char* why = "clear";
				if (nowMs < g.reverseUntil) { capNow = 1; why = "backing out"; }
				else if (g.lastF == 2 || g.detourAng != 0) { capNow = 2; why = "path blocked"; }
				else if (g.lastF == 1 || rough) { capNow = 3; why = "rough ground"; }
				else if (!aheadLoaded) { capNow = 3; why = "ground ahead loading"; }
				if (capNow < g.gearCap) {
					g.gearCap = capNow;
					g.capClearSince = 0;
					if (logger::Verbose()) logger::Write("%s: gear cap %d (%s)", wc, capNow, why);
				}
				else if (capNow > g.gearCap) {
					if (!g.capClearSince) g.capClearSince = nowMs;
					else if (nowMs - g.capClearSince >= kCapRaiseMs) {
						++g.gearCap;
						g.capClearSince = nowMs;
						if (logger::Verbose()) logger::Write("%s: gear cap %d (clear)", wc, g.gearCap);
					}
				}
				else {
					g.capClearSince = 0;
				}
			}
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
				if ((idx >= 1 || !Ramming(g_roverId)) && features::On(features::Id::HoverOrDrive)) {
					const auto& nv = PilotNavOf(idx);
					travelmode::Inputs in;
					in.profile = nv.travel;
					in.playerHovering = g_gs[0].active && !g_gs[0].drive && g_driverVehicle.load(std::memory_order_relaxed) != 0;
					in.tripDist = std::hypot(goal.x - pos.x, goal.y - pos.y);
					in.goalAbove = std::isfinite(goal.z) ? goal.z - pos.z : 0.0;
					const double toGoal = std::atan2(goal.x - pos.x, goal.y - pos.y);
					const auto surface = environment::SurfaceAhead(At<std::uint32_t>(vehicle, off::kFormId), pos.x, pos.y, pos.z, toGoal, std::min(250.0, in.tripDist));
					in.waterAhead = surface.water;
					in.roughAhead = surface.rough;
					in.clearNeed = (g.drive || g.groundHug) && g.lastF >= 1 ? g.lastNeed : 0.0;  // the corridor rays: what the way ahead needs to clear
					in.jumpHeight = JumpHeight();
					in.fighting = idx ? npcvehicles::PilotFighting(idx) : CoverFighting();
					in.fleeing = idx ? npcvehicles::PilotFleeing(idx) : false;
					in.catching = idx == 0 && g_follow && g_air.air;  // you jumped or fell: it goes under you
					in.damaged = vehiclehits::HealthShare(At<std::uint32_t>(vehicle, off::kFormId)) < travelmode::kDamaged;
					in.cramped = g.cramped >= kCrampedOn;
					in.inSettlement = realvehicles::PlaceOwner(vehicle) != 0;
					const auto reason = travelmode::Decide(in);
					if (travelmode::Step(g.travel, reason, tNow))
						logger::Write("%s: travel: %s (%s)", wc, g.travel.mode == travelmode::Mode::Drive ? "drive" : "hover", reason.why);
					decided = true;
					wantDrive = g.travel.mode == travelmode::Mode::Drive;
					travelWhy = reason.why;
					// "Drive" is the hover held just over the ground, hopping what a jump clears (2026-10-21 run: the game's own drive
					// left NPC vehicles stuck at speed 0 in towns, and it freezes an empty vehicle, bFreezeVehicleOnExit). The game's
					// own drive is kept for you aboard, a cramped spot and a failing hover (damaged), entered only once down low.
					nativeWanted = wantDrive && occupied && (idx == 0 || in.damaged || in.cramped);
					const bool hug = wantDrive && !g.drive;
					if (hug != g.groundHug) {
						g.groundHug = hug;
						logger::Write("%s: travel: %s (%s)", wc, hug ? "on the ground (hover held low)" : "flying", reason.why);
					}
				}
				if (!decided && g.groundHug) {  // hover or drive switched off: back to the hover
					g.groundHug = false;
					logger::Write("%s: travel: flying (hover or drive off)", wc);
				}
				const bool enterDrive = decided ? nativeWanted && above < 2.5 : g.cramped >= kCrampedOn;  // a hovering vehicle comes down first
				if (!g.drive && occupied && enterDrive && !g.apClimb && std::fabs(v[2]) < 4.0) {  // not mid-fall or mid-climb (it may stand on a structure the ground rays miss)
					g.drive = true;
					g.driveSince = tNow;
					g.drvT = 0;
					g.drvSteer = 0;
					{
						std::lock_guard lk(g_sh.lock);
						g_sh.active = false;  // the hover model stops writing; the game's own step runs
						g_sh.haveLast = false;
					}
					g_driveOut[idx].vehicleId.store(At<std::uint32_t>(vehicle, 0x28));
					g_driveOut[idx].on.store(true);
					g_clearBoosts[idx] = vehicle;
					g_forwardBoosts[idx] = false;
					logger::Write("%s: drive mode ON (%s): hover off, driving with boost for forward and up", wc, travelWhy);
				}
				else if (g.drive && decided && !g.apClimb && (!wantDrive ||
					(!nativeWanted && tNow - g.driveSince > kDriveMinMs && g.lastF == 0 && g.lastL < 2 && g.lastR < 2))) {
					LeaveDrive(idx, vehicle, wantDrive ? "clear again: on the ground, hover held low" : travelWhy);
				}
				else if (g.drive && !decided && tNow - g.driveSince > kDriveMinMs && g.cramped < 0.5 && g.lastF == 0 && g.lastL < 2 && g.lastR < 2 && !g.apClimb) {
					LeaveDrive(idx, vehicle, "clear again");
				}
				if (g.drive) {
					// Speed from position differences (no physics step in this mode); the steer sign is checked against the
					// heading change it causes.
					if (g.drvT && tNow > g.drvT) {
						const double dd = (tNow - g.drvT) / 1000.0;
						if (dd >= 0.02) {
							const double inst = std::hypot(pos.x - g.drvX, pos.y - g.drvY) / dd;
							g.drvSp += (inst - g.drvSp) * std::min(1.0, dd * 4.0);
							if (g.drvSp > 4.0 && std::fabs(g.drvSteer) > 0.5 && tNow >= g.hopUntil && !g.apClimb) {
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
                    const double level=std::max(double(pos.z),double(g.ground)+2.5);
                    const auto& field=navfield::Get(pos.x,pos.y,level,goal.x,goal.y,kMpcHalfWidth,true,goal.z);
                    double best=1e12,bearing=apBearing;
                    for(int n=0;n<16;++n){const double h=n*0.39269908169872414,reach=std::max(8.0,spD*1.5);
                        const double value=field.Value(pos.x+std::sin(h)*reach,pos.y+std::cos(h)*reach,level);
                        const double score=value+3*std::fabs(Wrap(h-bodyHeading));if(score<best){best=score;bearing=h;}}
                    if(best<1e12)apBearing=bearing;

					const double e = Wrap(apBearing - heading);  // + : the target is to the right
					double steer = std::clamp(1.8 * e, -1.0, 1.0);
					// Boxed in by something climbable (the hover assist stops dead there): go over it with the boost instead; only a tall
					// wall (more than 40 m) means backing away.
					const bool wall = g.lastNeed > 40.0;
					double topD = top;
					if (g.avoid == 4 && !wall) topD = std::max(topD, 12.0);
					double tgt = std::min(topD, kDriveTop);
					bool fwdBoost = false;
					if (topD > kDriveTop && std::fabs(e) < 0.35 && g.lastF == 0) {  // open and aligned: the forward boost
						tgt = topD;
						fwdBoost = true;
					}
					double accel = std::clamp((tgt - spD) / 5.0, 0.0, 1.0);
					double brake = spD > tgt + 3.0 ? std::clamp((spD - tgt) / 8.0, 0.0, 1.0) : 0.0;
					if (sBrake || (braking && wall) || tgt < 0.5) { accel = 0.0; brake = 1.0; }
					if (std::fabs(e) > 1.3) accel = std::min(accel, 0.5);
					// Up: over what is ahead (the corridor's climb need), a climb phase, or a hop after being stuck.
					bool vert = g.apClimb || tNow < g.hopUntil || (g.lastNeed > 0.3 && g.lastNeed <= 40.0 && g.lastF >= 1);
					// Back away: boxed in, or told to (stuck / close ahead).
					const bool boxed = g.lastF == 2 && g.lastL == 2 && g.lastR == 2 && spD < 2.0;
					if (tNow < g.reverseUntil || (boxed && wall)) {
						accel = 0.0;
						brake = 1.0;
						steer = -steer;
					}
					else if (boxed) {  // climbable: up and forward
						vert = true;
						accel = 1.0;
						brake = 0.0;
					}
                    if(sBrake||g.personAhead||g.wallAhead||(g.vehWarn&&g.vehYield&&g.vehGap<5.0)||spD>top+0.5){accel=0;brake=1;fwdBoost=false;vert=false;}
                    const double arrive=integrationlogic::ArrivalYaw(heading,PilotNavOf(idx).arriveYaw,std::hypot(goal.x-pos.x,goal.y-pos.y),goal.stop,spD,g_sh.hull.YawAt(spD));
                    if(std::isfinite(arrive)){steer=std::clamp(arrive,-1.0,1.0);accel=std::fabs(arrive)>0.12?0.15:0;brake=accel?0:1;fwdBoost=false;vert=false;
                        if(g.personAhead||g.wallAhead){accel=0;brake=1;}
                    }
                    if(g.recovery.failed){accel=0;brake=1;vert=false;steer=0;}
                    const double dist=std::hypot(goal.x-pos.x,goal.y-pos.y);
                    g.trip={true,g.recovery.failed?triplogic::Phase::Unreachable:triplogic::Status(groundOk,
                        triplogic::Reached(dist,goal.stop,std::isfinite(goal.z)?pos.z-goal.z:0,spD,groundOk),dist<=goal.stop+2,
                        std::isfinite(goal.z)?pos.z-goal.z:0,GetTickCount64()<g.reverseUntil,boxed),goal.x,goal.y,dist,spD,0,GetTickCount64()};
					const bool driveBoost = !g.recovery.failed && (fwdBoost || (vert && accel > 0.4));  // (named so it no longer hides the booster-visuals `boost` above)
					g.drvSteer = steer;
					g_driveOut[idx].steer.store(float(steer * g.steerSign));
					g_driveOut[idx].accel.store(float(accel));
					g_driveOut[idx].brake.store(float(brake));
					g_driveOut[idx].boost.store(driveBoost);
					g_driveOut[idx].vert.store(vert);
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
			if (g.autopilot && (g.apClimb || g.avoid != 1)) {
				if (g.apClimb) {
					g.hold = kCallClimb + g.cruiseExtra;
				}
				else {
					// Just over the highest ground in the corridor ahead (cast along the way it is going).
					const auto& nv=PilotNavOf(idx);
                    double want=integrationlogic::Clearance(int(nv.height),nv.minClearance,g_sh.hull.height,kCallSkim)+(g.autopilot?EnvClimb():0.0);  // night / bad weather: a little higher
                    if(g.groundHug) {  // "driving": just over the ground; the corridor below still lifts it over what a jump clears
                        want=g_sh.hull.height*0.5+0.4;
                        for(unsigned k=0;g_vsnapOk&&k<g_vsnap.count&&k<64;++k)if(g_vsnap.v[k].ref==vehicle)
                            want=std::max(0.3,double(g_vsnap.v[k].half[2])-(g_vsnap.v[k].center[2]-g_vsnap.v[k].pos[2])+0.4);
                    }
					for (int k = 0; k < 3; ++k) {
						const auto& q = g.corr[k];
						if (q.valid && !IsWallSample(g, q.h, q.d)) want = std::max(want, q.h + kTerrainClear - refGround);  // walls: the warning, not a climb
					}
					want = std::min(want, kAvoidMaxClimb);
					const double goalZ = goal.z;
					const bool haveGoalZ = !std::isnan(goalZ);
					if (haveGoalZ) want = std::max(want, std::min(goalZ - refGround, kGoalMaxAbove));
					const double sink = haveGoalZ ? kGoalSink : kCruiseSink;
					g.cruiseHold = want >= g.cruiseHold ? want : std::max(want, g.cruiseHold - sink * dt);
					g.hold = g.landing?std::max(0.15,g.landGround+g.landHeight-refGround):g.cruiseHold+g.cruiseExtra+g.laneZ;
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

			// Settling onto the ground ends the hover.
			if (groundOk && !g.liftoff && !(g.autopilot && g.apClimb) && above < kLandHeight && v[2] <= 0.1 && !thrust) {  // never while climbing out
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
				if (nowMs < g.reverseUntil && !g.apClimb) {
					thrustAcc = -kReverseThrottle * kThrust * kGearThrust[2];
					thrust = true;
					braking = false;
					pitchAim = 0.0;
				}
				if (g.detourAng != 0 && !g.apClimb) strafe = g.detourAng > 0 ? 0.4 : -0.4;  // slide toward the open side
				if (g.wallAhead && !g.apClimb) {
					// The wall warning: a speed that stops kWallStandoff short of it; inside the standoff: brake (the detour steering turns away).
					const double room = g.wallDist - kWallStandoff;
					const double safe = std::max(2.0, std::sqrt(2.0 * kWallDecel * std::max(0.0, room)));
					if (sp > safe || room < 0.0) {
						thrustAcc = std::min(thrustAcc, 0.0);
						thrust = thrustAcc != 0.0;
						braking = room < 0.0 || sp > safe + 2.0;
					}
				}
				// A wall close beside: slide away from it (never into it).
				if (!g.apClimb && (g.wallL < 4.0 || g.wallR < 4.0)) strafe = std::clamp(strafe + (g.wallL < g.wallR ? 0.4 : -0.4), -1.0, 1.0);
				if (g.personAhead && !g.apClimb) {
					// Someone on foot ahead (2026-10-20): slow to a speed that stops short of them.
					const double safe = std::max(2.0, std::sqrt(2.0 * 6.0 * std::max(0.0, g.personDist - 4.0)));
					if (sp > safe) {
						thrustAcc = std::min(thrustAcc, 0.0);
						thrust = thrustAcc != 0.0;
						braking = sp > safe + 2.0;
					}
				}
				if (g.vehWarn && g.vehYield && !g.apClimb) {
					// Another vehicle close: ease off when it is ahead and closing, slide away from it (never a climb over it).
					const double travel = sp > 2.0 ? std::atan2(v[0], v[1]) : bodyHeading;
					if (std::fabs(Wrap(g.vehBearing - travel)) < 1.1 && g.vehClosing > 0.5) {
						const double room = std::max(0.0, g.vehGap - 1.5);
						const double safe = std::max(2.0, std::sqrt(2.0 * 6.0 * room));  // a speed that can stop in the gap at 6 m/s^2
						if (sp > safe) {
							thrustAcc = std::min(thrustAcc, 0.0);
							thrust = thrustAcc != 0.0;
							braking = sp > safe + 3.0;
						}
					}
					if (g.vehGap < 4.0) {
						const double side = Wrap(g.vehBearing - bodyHeading);  // + : it is to the right
						strafe = std::clamp(strafe + (side > 0 ? -0.5 : 0.5), -1.0, 1.0);
					}
				}
                // Zero vertical route speed leaves precise terrain/goal-height following in control.
                // Holding zero here would strand the vehicle between the field's eight-metre layers.
                if(!g.drive && !g.apClimb && g.mpcOk && std::fabs(g.mpcVz)>0.1 && nowMs-g.mpcAt<150 && !g.recovery.failed) {
                    vzOn=true; vzCmd=g.mpcVz;
                }
				rollAim = std::clamp(kApBankPerYaw * yawWant * std::clamp(sp / 15.0, 0.0, 1.0), -kApBankMax, kApBankMax);
				rollAimOn = true;
				const auto& hc = g_cmds[idx];
				if (hc.c.set && nowMs < hc.until) {
					const auto& c = hc.c;
                    constexpr unsigned movement=Command::kThrottle|Command::kYaw|Command::kStrafe|Command::kVertical|Command::kGear;
                    const bool requestedMotion=(c.set&movement)!=0;
                    const integrationlogic::MotionFence fence{g.recovery.failed,g.wallAhead,g.personAhead,g.vehWarn&&g.vehYield&&g.vehGap<5.0,top,sp};
                    const bool allowMovement=!requestedMotion||integrationlogic::MovementAllowed(fence,ValidateCommand(idx,vehicle,pos,bodyHeading,sp,top,goal,c));
                    const unsigned allowed=c.set&((allowMovement?movement:0)|Command::kPitch|Command::kRoll|Command::kBrake);
                    const int cg = (allowed & Command::kGear) ? std::clamp(c.gear, 1, 4) : apGear;
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
            if(g.autopilot&&!g.wallAhead&&!g.personAhead) {
                const double yaw=integrationlogic::ArrivalYaw(bodyHeading,PilotNavOf(idx).arriveYaw,std::hypot(goal.x-pos.x,goal.y-pos.y),goal.stop,sp,g_sh.hull.YawAt(sp));
                if(std::isfinite(yaw)){yawWant=yaw;thrustAcc=0;thrust=false;braking=true;}
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
            // The final envelope wins over all decision sources.
            if(g.autopilot&&(g.wallAhead||g.personAhead||(g.vehWarn&&g.vehYield&&g.vehGap<5.0))) {
                thrustAcc=std::min(thrustAcc,0.0);thrust=thrustAcc!=0;braking=true;
            }
            if(g.autopilot&&sp>top+0.5){thrustAcc=std::min(thrustAcc,0.0);thrust=thrustAcc!=0;braking=true;}
            if(g.ceilValid&&pos.z>=g.ceilZ-kCeilClear-g_sh.hull.height&&vzOn&&vzCmd>0)vzCmd=0;
            if(g.autopilot && g.recovery.failed) {
                thrustAcc=0; thrust=false; braking=true; strafe=0; yawWant=0;
                vzOn=true; vzCmd=0;
                g.holding=true; g.hold=std::max(kMinClearance,pos.z-refGround);
            }
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
			AskCorridors(idx, pos, (g.autopilot && apSteer && !g.apClimb) ? apBearing : (sp > 2.0 ? std::atan2(v[0], v[1]) : bodyHeading), std::max(sp, g.autopilot ? 10.0 : 0.0));
			bool cameraActive=false;
			double cameraPitch=0,cameraRoll=0;
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
				g_sh.rollAimOn = rollAimOn;
				g_sh.rollAim = rollAim;
				g_sh.freeRoll = freeRoll;
				if (g_sh.physical!=physical||(physical&&pilotChanged)) {
					g_sh.physicalState={}; g_sh.physicalFault=false; g_sh.haveLastW=false; g_sh.physWiped=0;
					if (physical||g_sh.physical) logger::Write("%s: physical flight %s (%s)",Who(idx),physical?"on":"off",g.autopilot?"AI pilot":"manual");
				}
				g_sh.physical = physical;
				g_sh.assist = flightAssist;
				g_sh.bankTest = bankTest;
				g_sh.holding = g.holding;
				if (g.ceilValid) {
					// Never up into a ceiling: the hold (metres over refGround) stays under it; the climb-outs give way to it too.
					const double maxHold = g.ceilZ - kCeilClear - kVehicleTop - refGround;
					if (g.hold > maxHold) {
						g.hold = std::max(0.8, maxHold);
						if (g.apClimb && pos.z - refGround >= g.hold - 0.5) g.apClimb = false;
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
				g_sh.groundOk = groundOk;
				g_sh.airborne = groundOk && above > 1.5;
				g_sh.cap = g.cap;
				cameraActive=manualPhysical&&physical&&!g_sh.physicalFault;
				cameraPitch=g_sh.bodyPitch; cameraRoll=g_sh.bodyRoll;
			}
			if (!idx) {
				vehiclecamera::Publish(cameraActive?vehicle:0,cameraPitch,cameraRoll);
				vehiclecamera::LogStatus();
			}

			if (g.autopilot) {
				const double dist = std::hypot(goal.x-pos.x, goal.y-pos.y);
				const double targetZ = std::isfinite(goal.z) ? std::max(goal.z, refGround + kCallSkim + EnvClimb()) : pos.z;
				const double zError = pos.z - targetZ;
				const bool ready = groundOk && matched && (aheadLoaded || sp>3.0 || dist<=goal.stop+2.0);
				const bool arrived = !goal.cruise && AtDestination(vehicle, goal.x, goal.y, goal.z, goal.stop);
				const bool rerouting = GetTickCount64() < g.mpcEscapeUntil || GetTickCount64() < g.reverseUntil ||
					g.trailOn || std::fabs(g.detourAng) > 0.1;
				const bool blocked = g.avoid == 4 || (sp < 1.0 && g.lastF == 2 && g.mpcAt && !g.mpcOk);
				g.trip = {true, ready && g.apClimb ? triplogic::Phase::AdjustingHeight : triplogic::Status(ready, arrived, dist <= goal.stop+2.0, zError, rerouting, blocked),
					goal.x, goal.y, dist, std::hypot(sp,v[2]), zError, GetTickCount64()};
                if(g.recovery.failed) g.trip.phase=triplogic::Phase::Unreachable;
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
				g.apClimb = true;
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
			g_driverVehicle.store(0, std::memory_order_relaxed);
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
			g.playerAuto = false;
			g.holdId = 0;
			g.routeHold=false;
			g_follow = false; g_guard = false;
			hoverui::SetState(false, false, g.gear, false);
			hoverui::SetTripStatus(false, "");
			hoverui::SetFoot(false, false, "", "", "", "", "", "");
			g_driverVehicle.store(0, std::memory_order_relaxed);
			return;
		}
		deadSaid = false;
		const bool focused = Focused();
		ChooseKeys();
		ReadMoveKeys();
		hoverui::EditTick(focused);
		const bool h = focused && bindings::Held(bindings::Action::HoverToggle, g_keys.toggle);
		const bool pressed = h && !g.keyDown;
		g.keyDown = h;
		const auto driven = PlayerVehicle(player);
		FirstPersonLookLimits(driven != 0);
		g_boostRequest.store(driven && focused && !g.playerAuto && (bindings::Held(bindings::Action::Boost, VK_LSHIFT) || (g.active && g.wHold)));
		if(!driven)g_meterBoost.store(false);
		hoverui::SetState(driven != 0, g.active && (!g.autopilot || g.playerAuto), g.gear, g.wHold);
		const auto priorDriven=g_driverVehicle.exchange(driven,std::memory_order_relaxed);
		if (driven && priorDriven != driven) {
			const auto id = At<std::uint32_t>(driven, off::kFormId);
			logger::Write("Vehicle health: player boarded %08X with %.3f / %.0f health; existing chassis record retained", id,
				vehiclehits::HealthShare(id) * vehiclehits::MaxHealth(), vehiclehits::MaxHealth());
		}
		if(priorDriven && priorDriven!=driven)EndTargetSession(priorDriven);
		if (priorDriven != driven) g_altInput = {};
		const auto alt = g_altInput.Update(bindings::Held(bindings::Action::Gear, VK_MENU), driven && focused && !hoverui::IsScannerOpen(), GetTickCount64());
		g_altDrop = alt.drop;
		if (alt.cycle) {
			const int before = g.gear;
			g.gear = altcontrol::NextGear(g.gear);
			notify::Log("Gear %d of 4  (%.0f m/s)", g.gear, kGearTop[g.gear]);
			logger::Write("Rover: gear %d -> %d (Alt tap)", before, g.gear);
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
				static ULONGLONG ejectAt = 0;
				static unsigned ejectTries = 0;
				if (driven && vehiclehits::Wrecked(At<std::uint32_t>(driven, off::kFormId))) {
					const ULONGLONG t = GetTickCount64();
					if (t - ejectAt >= 500) {
						ejectAt = t;
						settleSince = 0;
						if (g.active) {
							g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
							Stop("wrecked: you are thrown out");
						}
						const bool out = npcvehicles::ExitPlayer(true);
						if (ejectTries++ < 5) logger::Write("Rover: your vehicle %08X is wrecked -> ejecting you: %s", At<std::uint32_t>(driven, off::kFormId), out ? "out" : "not yet (retrying)");
						if (out) notify::Show("Your vehicle is wrecked: you were thrown out");
					}
				}
				else ejectTries = 0;
			}
			static std::uintptr_t settleVehicle = 0;
			static int exitKey = 0;
			if (!exitKey) exitKey = controlmap::KeyFor("Vehicle", "VehicleExit", 'E');
			const bool eDown = focused && driven && Held(exitKey);
			if (eDown && !exitDown && g.active && !settleSince) {
				settleSince = GetTickCount64();
				settleVehicle = driven;
				g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
				Stop("exit: setting down first");
				notify::Log("Setting down to get out");
			}
			exitDown = eDown;
			if (settleSince) {
				const ULONGLONG t = GetTickCount64();
				double speed = 99.0;
				world::Snapshot snap;
				if (world::Vehicles(snap)) for (unsigned k = 0; k < snap.count && k < 64; ++k) if (snap.v[k].ref == settleVehicle)
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
			g.playerAuto = false;
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
					if (!wasHovering) Start(driven);
					g.autopilot = true;
					g.playerAuto = true;
					g.taxiWait = false;
					g.taxiMarker = 0;
					g.cramped = 0;  // a new Auto flight starts with a clean slate (no leftover contact score)
					g_bumps[0] = Bumps{};
					g.apClimb = !wasHovering;
					g.autopilotSince = GetTickCount64();
					notify::Log("Destination selected - navigating; W/A/S/D takes over");
					logger::Write("Rover auto: on (%s)", wasHovering ? "already hovering" : "lifting off");
			}
			else if (g.playerAuto && focused && (alt.cycle || alt.drop || bindings::Held(bindings::Action::Accelerate, g_mk.accel) || bindings::Held(bindings::Action::Brake, g_mk.brake) || bindings::Held(bindings::Action::TurnLeft, g_mk.left) || bindings::Held(bindings::Action::TurnRight, g_mk.right))) {
				g.playerAuto = false;
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
			static keygesture::Input nKey, uKey;
			static bool stanceDown = false;
			const ULONGLONG keyNow = GetTickCount64();
			const auto nAct = nKey.Update(bindings::Held(bindings::Action::Summon, g_keys.call), focused, keyNow);
			const auto uAct = uKey.Update(bindings::Held(bindings::Action::Cover, g_keys.follow), focused && !driven, keyNow);
			const bool stanceHeld = focused && bindings::Held(bindings::Action::Stance, g_keys.stance);
			if (stanceHeld && !stanceDown) {
				const auto s = npcvehicles::CycleStance();
				notify::Show("Stance: %s", s == npcvehicles::Stance::Attack ? "Attack" : s == npcvehicles::Stance::HoldFire ? "Hold fire" : "Defend");
			}
			stanceDown = stanceHeld;
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
				g_ram = {};
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
						logger::Write("Rover call: %08X %s; moving it next to the player", g_roverId,
							!sameWorld ? "in another worldspace" : !has3D ? "not loaded" : "far");
						AskMove(player);
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
						g.apClimb = true;
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
						g.apClimb = !g_air.air;  // in the air: straight to the catch point
						g.autopilotSince = nowMs;
						logger::Write("Rover follow: launching (%.0f m from the player%s)", distP, g_air.air ? ", you are in the air" : "");
					}
				}
				else if (playerStill && distP < kFollowFar && !g_air.air && !CoverFighting()) {
					Stop("follow: player stopped; parked");
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
						g.apClimb = true;
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
				else if (g_move.pending || (g_movedAt && nowMs - g_movedAt < 4000)) std::snprintf(status, sizeof(status), "MOVED TO YOU");
			}
			const std::string callKey = bindings::Label(bindings::Action::Summon, g_keys.call);
			const std::string followKey = bindings::Label(bindings::Action::Cover, g_keys.follow);
			const std::string useKey = controlmap::KeyName(controlmap::KeyFor("MainGameplay", "Activate", 'E'));
			// The names: N is always Summon; U is Follow, or Cover while you are in combat.
			hoverui::SetFoot(!driven && g_roverId != 0, rover && !driven && !g.autopilot && toRover <= kNearBoard, callKey.c_str(), followKey.c_str(), useKey.c_str(), status,
				overrides::SummonLabel(), overrides::CoverLabel(npcvehicles::IsInCombat(player)));
			char line[64] = "";
			const std::uint32_t drivenId = driven ? At<std::uint32_t>(driven, off::kFormId) : 0;
			const bool ramLine = driven && Ramming(drivenId);
			if (ramLine) std::snprintf(line, sizeof(line), "RAMMING");
			else if (driven && g.playerAuto) {
				if (g.taxiMarker && ProgressFor(g_roverId).phase==triplogic::Phase::Arrived) std::snprintf(line, sizeof(line), "ARRIVED");
				else if (g.taxiMarker) std::snprintf(line, sizeof(line), "%s  %.0f m", triplogic::Text(ProgressFor(g_roverId).phase), std::min(g.taxiLeft, 99999.0));
				else std::snprintf(line, sizeof(line), "AIM + T");
			}
			hoverui::SetTripStatus((driven && g.playerAuto) || ramLine, line);
			hoverui::SetAutoKey(driven ? scantarget::KeyName().c_str() : "");
			hoverui::SetFollow(g_follow);
		}
		std::uintptr_t heldVehicle = 0;
		if (!driven && g.holdId) {
			heldVehicle = RefById(g.holdId);
			if (!heldVehicle) Stop("held vehicle lost");
		}
		const auto vehicle = driven ? driven : (g.autopilot ? (heldVehicle ? heldVehicle : Rover()) : 0);
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
		if (pressed && driven) {
			if (g.active) {
				Stop(bindings::Label(bindings::Action::HoverToggle, g_keys.toggle).c_str());
				return;
			}
			Start(vehicle);
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
					if (g_autoMode == AutoMode::Orbit && g.taxiLeft < kOrbitRadius + 20.0) {
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
				goal.hold = !g.taxiOrbit || g.taxiWait;
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

		// NPC pilots: every frame while hovering, the same frame logic as the player's autopilot.
		void PilotsTick()
		{
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			const auto driven = player ? PlayerVehicle(player) : 0;
			for (int idx = 1; idx < kPilots; ++idx) {
				if (!g_gs[idx].active) continue;
				const auto vehicle = RefById(g_jobs[idx].vehicleId);
				const auto loaded = vehicle ? At<std::uintptr_t>(vehicle, off::kRefLoadedData) : 0;
				if (!vehicle || vehicle != g_shs[idx].vehicle.load()) Stop(idx, "vehicle lost");
				else if (vehicle == driven) Stop(idx, "the player took the vehicle");
				else if (!loaded || !At<std::uintptr_t>(loaded, 0x08)) Stop(idx, "vehicle unloaded");
				else Fly(idx, vehicle, g_jobs[idx].goal, false);
			}
		}
	}

	bool PilotFly(int pilot, std::uintptr_t vehicle, double x, double y, double stop, unsigned timeoutMs, bool keepGoing, double z)
	{
		if (pilot < 1 || pilot >= kPilots || !vehicle) return false;
		const std::uint32_t id = At<std::uint32_t>(vehicle, 0x28);
		if (vehicle == g_hoverVehicle.load() || (id == g_roverId && npcvehicles::CrewOrderOf(id) == npcvehicles::CrewOrder::None)) return false;  // the player's rover only for a crew job
		if (IsDisabled(id)) return false;  // the hit counter switched its autopilot off
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
		State& g = g_gs[pilot];
		if (g.active && job.vehicleId == id) return true;  // a new target for the same flight
		if (g.active) Stop(pilot, "another vehicle");
		job.vehicleId = id;
		Start(pilot, vehicle);
		g.autopilot = true;
		g.apClimb = true;
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
        // Preserve the complete shared goal contract, including braking and pass-through semantics.
        g_jobs[pilot].goal.hold=goal.hold;
        g_jobs[pilot].goal.cruise=goal.cruise||goal.nav.passThrough;
        g_jobs[pilot].goal.cruiseSpeed=goal.cruise ? goal.cruiseSpeed : goal.nav.speedLimit>0 ? goal.nav.speedLimit : kAiGearTop[4];
        return true;
	}

	bool PilotActive(int pilot) { return pilot >= 1 && pilot < kPilots && g_gs[pilot].active; }

	bool DriveFor(std::uint32_t vehicleFormId, DriveInput& out)
	{
		if (!vehicleFormId) return false;
		for (int i = 0; i < kPilots; ++i) {
			const auto& d = g_driveOut[i];
			if (!d.on.load(std::memory_order_relaxed) || d.vehicleId.load(std::memory_order_relaxed) != vehicleFormId) continue;
			out.steer = d.steer.load(std::memory_order_relaxed);
			out.accel = d.accel.load(std::memory_order_relaxed);
			out.brake = d.brake.load(std::memory_order_relaxed);
			out.boost = d.boost.load(std::memory_order_relaxed);
			out.vert = d.vert.load(std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	bool ViewHeading(double& yaw) { return CameraHeading(yaw); }

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
				state.haveGround && GetTickCount64()-state.groundAt<=500 && !state.recovery.failed);
			return triplogic::Reached(std::hypot(x-pos.x,y-pos.y),stop,pos.z-expected,requireStopped?speed:0,
				shared.matched && state.haveGround && GetTickCount64()-state.groundAt<=500 && !state.apClimb && !state.recovery.failed);
		}
        world::Snapshot snap;if(world::Vehicles(snap))for(unsigned k=0;k<snap.count&&k<64;++k)if(snap.v[k].ref==vehicle) {
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

	void CancelScannerTrip()
	{
		g_scanCall.store(false); g_goOnBoard.store(false); g_goAt.store(0);g_ram={};
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
            g_gs[i].recovery.Reset();g_gs[i].mpcEscapeUntil=0;g_gs[i].reverseUntil=0;}}
        navfield::Reset();
        RequestScannerTrip();
    }

	void ToggleAuto()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto driven = player ? PlayerVehicle(player) : 0;
		if (!driven) return;
		if (g.playerAuto) {
			g.playerAuto = false; g.autopilot = false; g.routeHold = false; g.holdId = 0;
			notify::Log("Auto off - you have control");
			logger::Write("Rover auto: off (T tap)");
			return;
		}
		const bool wasHovering = g.active && g_hoverVehicle.load() == driven;
		if (!wasHovering) Start(driven);
		g.autopilot = g.playerAuto = true;
		g.routeHold = false; g.holdId = 0;
		g.taxiWait = false; g.taxiMarker = 0; g.taxiArrived = false; g.taxiOrbit = false;
		g.wHold = false; g.cramped = 0; g_bumps[0] = Bumps{};
		g.apClimb = !wasHovering; g.autopilotSince = GetTickCount64();
		g_goOnBoard = false;
		notify::Log("Auto on - W/A/S/D or T takes over");
		logger::Write("Rover auto: on (T tap, %s)", wasHovering ? "already hovering" : "lifting off");
	}

	void RequestScannerTrip()
	{
		g.holdId=0; g.routeHold=false;
        for(int i=0;i<kPilots;++i){const auto vehicle=g_hoverVehicles[i].load();if(i==0||(vehicle&&At<std::uint32_t>(vehicle,off::kFormId)==g_roverId))g_gs[i].recovery.Reset();}
		if (npcvehicles::CrewOrderOf(g_roverId)==npcvehicles::CrewOrder::Hold)
			npcvehicles::SetCrewOrder(g_roverId,npcvehicles::CrewOrder::Marker);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto driven = player ? PlayerVehicle(player) : 0;
		if (driven) {
			const bool wasHovering = g.active && g_hoverVehicle.load() == driven;
			if (!wasHovering) Start(driven);
			g.autopilot = g.playerAuto = true;
			g.taxiWait = false; g.taxiMarker = 0; g.taxiArrived = false; g.taxiOrbit = false;
			g.wHold = false; g.cramped = 0; g_bumps[0] = Bumps{};
			g.apClimb = !wasHovering; g.autopilotSince = GetTickCount64();
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

	bool Ramming(std::uint32_t vehicleId){return ramlogic::Owns(g_ram.vehicle,vehicleId,GetTickCount64()-g_ram.started);}
	bool RequestRam(std::uintptr_t actor) {
		const auto player=Ptr(addr::kPlayerCharacterPtr);if(!player||!actor)return false;
		const auto vehicle=PlayerVehicle(player)?PlayerVehicle(player):Rover();
		if(!vehicle){logger::Write("Ram order: refused (no driven or own vehicle)");return false;}
		const auto id=At<std::uint32_t>(vehicle,off::kFormId),actorId=At<std::uint32_t>(actor,off::kFormId);
		const char* why=ramlogic::Reject(npcvehicles::LooksLikeActor(actor),!npcvehicles::IsDead(actor),
			EngineFn<std::uintptr_t(*)(std::uintptr_t)>(addr::kPlayerVehicle)(actor)==vehicle);
		if (!why && npcvehicles::AreAllies(player, actor)) why = "friendly fire disabled";
		if(!why&&(!Worldspace(vehicle)||Worldspace(actor)!=Worldspace(vehicle)))why="actor in another world";
		if(!why&&IsDisabled(id))why="autopilot disabled after hits";
		if(why){logger::Write("Ram order: vehicle %08X -> actor %08X refused (%s)",id,actorId,why);return false;}
		const int pilot=npcvehicles::PilotFor(id);
		EndTargetSession(vehicle);
		if(!pilot){if(!g.active)Start(vehicle);g.autopilot=true;g.playerAuto=PlayerVehicle(player)==vehicle;
			const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);g.holdId=id;g.holdX=pos.x;g.holdY=pos.y;g.holdZ=pos.z;g.routeHold=true;g.autopilotSince=GetTickCount64();}
		else {vcontract::Goal goal;const auto target=At<game::Vec3>(actor,off::kRefPosition);
			goal.x=target.x;goal.y=target.y;goal.z=target.z+1;goal.cruise=true;goal.cruiseSpeed=26;goal.stop=0;
			if(!PilotFly(pilot,vehicle,goal)){logger::Write("Ram order: vehicle %08X -> actor %08X refused (crew pilot would not fly)",id,actorId);return false;}
		}
		g_ram={id,actorId,pilot,GetTickCount64()};
		logger::Write("Ram order: vehicle %08X -> highlighted actor %08X",id,g_ram.actor);return true;
	}

	bool OrderRam(int pilot,std::uintptr_t vehicle,std::uintptr_t actor) {
		if(pilot<1||pilot>=kPilots||!vehicle||!actor||g_ram.vehicle||!g_gs[pilot].active)return false;
		const auto id=At<std::uint32_t>(vehicle,off::kFormId),actorId=At<std::uint32_t>(actor,off::kFormId);
		if(IsDisabled(id)||npcvehicles::IsDead(actor)||Worldspace(actor)!=Worldspace(vehicle))return false;
		const auto driverId = npcvehicles::VehicleDriverId(id);
		const auto driver = driverId ? RefById(driverId) : 0;
		if (driver && npcvehicles::AreAllies(driver, actor)) return false;
		vcontract::Goal goal;const auto target=At<game::Vec3>(actor,off::kRefPosition);
		goal.x=target.x;goal.y=target.y;goal.z=target.z+1;goal.cruise=true;goal.cruiseSpeed=26;goal.stop=0;
		if(!PilotFly(pilot,vehicle,goal))return false;
		g_ram={id,actorId,pilot,GetTickCount64()};g_ram.keep=true;
		logger::Write("Ram order: NPC vehicle %08X -> %08X (manoeuvre)",id,actorId);return true;
	}

	bool SetGuardPoint(double x,double y) {
		const double nan=std::numeric_limits<double>::quiet_NaN();  // skim the terrain there: the aimed height can be an estimate
		if(npcvehicles::SetCrewGuard(g_roverId,x,y,nan))return true;
		const auto rover=Rover();
		if(!rover||IsDisabled(g_roverId)){logger::Write("Rover guard: refused (%s)",rover?"autopilot disabled after hits":"no vehicle");return false;}
		g_follow=false;g_guard=true;g_ram={};
		if(!(g.active&&g_hoverVehicle.load()==rover))Start(rover);
		g.autopilot=true;g.playerAuto=false;g.holdId=g_roverId;g.holdX=x;g.holdY=y;g.holdZ=nan;g.routeHold=true;g.autopilotSince=GetTickCount64();
		logger::Write("Rover guard: %08X guards (%.0f, %.0f)",g_roverId,x,y);
		return true;
	}

	void FreshScannerTarget() {g_targetSession.Selected();g_ram={};}
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
		struct FlipWatchEntry { std::uint32_t id = 0; ULONGLONG since = 0, fixedAt = 0; } g_flips[8];

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
		if (!e->since) e->since = now;
		if (now - e->since < kFlipMs || now - e->fixedAt < 5000) return;
		e->fixedAt = now;
		e->since = 0;
		for (int i = 0; i < kPilots; ++i) {
			if (g_gs[i].active && g_hoverVehicles[i].load() == vehicle) Stop(i, "flipped");
		}
		const auto pos = At<game::Vec3>(vehicle, off::kRefPosition);
		const bool ok = Upright(vehicle, pos, rot.z);
		logger::Write("%s: %08X flipped (pitch %.0f, roll %.0f deg) -> set upright %s", who, id, rot.x * 57.2958, rot.y * 57.2958, ok ? "" : "FAILED");
	}

	namespace
	{
		vcontract::NavParams g_nav[kPilots];
		ULONGLONG g_navAt[kPilots] = {};
	}

	void PilotNav(int pilot, const vcontract::NavParams& nav)
	{
		if (pilot < 0 || pilot >= kPilots) return;
		const auto& old = g_nav[pilot];
		const bool fresh = GetTickCount64() - g_navAt[pilot] <= 1000;
		const bool same = fresh && old.speedLimit == nav.speedLimit && old.minClearance == nav.minClearance && old.height == nav.height &&
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
    void BeginFrame(){for(int i=0;i<kPilots;++i)ClearPilotCommand(i);}
    bool ImpactSince(std::uint32_t id,unsigned long long since) {
        const auto now=GetTickCount64();
        for(auto& sh:g_shs){const auto ref=sh.vehicle.load();if(!ref)continue;
            if(At<std::uint32_t>(ref,off::kFormId)!=id)continue;
            const auto at=sh.impactAt.load();return at>=since&&now>=at&&now-at<=250;}
        return false;
    }
    double CruiseLimit(std::uint32_t id) {
        world::Snapshot snap; if(world::Vehicles(snap))for(unsigned k=0;k<snap.count&&k<64;++k)if(snap.v[k].formId==id) {
            const auto&v=snap.v[k];return kAiGearTop[4]*hulllogic::For(v.half[0],v.half[1],v.half[2]).acceleration;
        }
        return kAiGearTop[2];
    }
    double TurnLimit(std::uint32_t id) {
        world::Snapshot snap;if(world::Vehicles(snap))for(unsigned k=0;k<snap.count&&k<64;++k)if(snap.v[k].formId==id) {
            const auto&v=snap.v[k];return hulllogic::For(v.half[0],v.half[1],v.half[2]).YawAt(std::hypot(v.vel[0],v.vel[1]));
        }
        return 0.8;
    }
    bool PilotCommand(int pilot, const Command& c)
	{
		if (pilot < 0 || pilot >= kPilots || !g_gs[pilot].active || !g_gs[pilot].autopilot) return false;
		g_cmds[pilot].c = c;
		g_cmds[pilot].until = GetTickCount64() + c.holdMs;
		return true;
	}

	const char* PilotEndReason(int pilot) { return pilot >= 1 && pilot < kPilots ? g_endWhy[pilot] : ""; }

	void PilotStop(int pilot, const char* why)
	{
		if (PilotActive(pilot)) Stop(pilot, why);
	}

	std::uint32_t OwnRoverId() { return g_roverId; }

	void Tick()
	{
		g_vsnapOk = world::Vehicles(g_vsnap);
		EnvTick();
		if (const auto player = Ptr(addr::kPlayerCharacterPtr)) {
			TrailTick(player);
			PlayerAirTick(player);
		}
		PlayerTick();
		PilotsTick();
		for(auto&s:g_shs){std::lock_guard lk(s.lock);if(const auto ref=s.vehicle.load())BindBody(s,ref);}
		if (const auto player = Ptr(addr::kPlayerCharacterPtr)) {
			if (!PlayerVehicle(player)) WatchFlip(Rover(), "Rover");  // never while the player drives it
		}
	}

	void LoadReset()
	{
		for (auto& s : g_shs) { s.physOn.store(false); PhysicalRelease(s); }
		vehiclecamera::Reset();
		FirstPersonLookLimits(false);
		TrailClear();
		// An in-game save load can keep the same player and rover addresses while
		// replacing their native bodies. Never carry thrust, contact or body
		// identity from the previous save into the new one.
		for (int i = 0; i < kPilots; ++i) {
			g_gs[i] = State{}; g_cmds[i]={}; g_navAt[i]=0; g_shs[i].impactAt.store(0);
			g_driveOut[i].on.store(false);
			{
				std::lock_guard lk(g_shs[i].lock);
				g_shs[i].active = false;g_shs[i].landing=false;
				g_shs[i].groundOn = false;
				g_shs[i].matched = false;
				g_shs[i].sceneCount=0;
				g_shs[i].haveLast = false;
				g_shs[i].collided = false;
				g_shs[i].steps = 0;
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
			g_jobs[i] = PilotJob{};
		}
		g_driverVehicle.store(0);
		g_lastDrivenVehicle.store(0);
		g_tapBoost = Tap{};
		g_tapThrust = Tap{};
		g_altInput = {}; g_altDrop = false;
		g_camRest = CamRest{};
		g_camPick = CamPick{};
		g_boostRequest=false;g_meterBoost=false;g_meterVehicle=0;g_meterExhausted=false;
		g_targetSession={};g_ram={};g_guard=false;g_cf=CoverFight{};
		logger::Write("Rover flight: load reset discarded prior rover motion and body identity");
	}
}

