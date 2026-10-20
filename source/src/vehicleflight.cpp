#include "vehicleflight.h"

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
#include "npcvehicles.h"
#include "notify.h"
#include "prefetch.h"
#include "routeplan.h"
#include "telemetry.h"
#include "vehicleworld.h"
#include "vehiclehits.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>

// Rover flight (0.57.0): a connected flight model that pushes the rover's own physics body instead of overwriting it.
//  - On / off: double-tap the game's vertical boost (Space). Also off: settle onto the ground, hit something, or leave
//    the vehicle.
//  - Controls (mouse-aim layout): the mouse aims, the nose follows the camera in yaw and pitch. A / D: roll.
//    W: boost (hold) at the selected gear; S: brake (hold); Shift + W / Shift + S: gear up / down (4 gears, 1-4 pick
//    one directly); neither: coast. Up / down keys: lift.
//  - Lift-off: switching on near the ground climbs to a short hover height first.
//  - Avoidance (terrain assist): three corridors (ahead, left, right) are sampled with ground rays at three distances
//    scaled by speed. Ahead clear: nothing. Ahead blocked but climbable in time: nose up and climb. Otherwise a clear
//    side: turn that way (the A / D bank turn). Nothing clear: brake to a stop.
//  - Yaw: the gap between where the camera looks and where the nose points, used like a variable turn input
//    (bigger gap = faster turn, sin(gap / 2) shaped). The camera is never written: it stays the game's own camera.
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
		constexpr ULONGLONG kAvoidHoldMs = 600;   // an avoidance action is kept at least this long (no flicker)
		// Call
		constexpr double kCallFar = 150.0;        // m: farther than this (or not loaded) the rover is moved, not flown (2026-10-17: was 300)
		constexpr double kCruiseAhead = 300.0;    // m: Auto (player aboard) flies at a point this far along the camera's heading
		constexpr double kNearBoard = 6.0;        // m: the HUD's "Get in" shows this close to the player's own vehicle
		constexpr double kCallStop = 8.0;         // m: the autopilot stops this far from the player
		constexpr double kCallBrakeSlack = 1.6;   // brakes once the distance left is under stop + speed * this (s)
		constexpr ULONGLONG kCallTimeoutMs = 90000;
		constexpr ULONGLONG kGuardMs = 6000;
		// Autopilot attitude: its own yaw, pitch and roll (the player only has the camera aim and the A / D bank).
		constexpr double kApYawGain = 1.6;        // 1/s: turn rate per radian of heading error
		constexpr double kApYawMax = 1.2;         // rad/s
		constexpr double kApBankPerYaw = 0.35;    // rad of bank per rad/s of turn (coordinated turn)
		constexpr double kApBankMax = 0.45;       // rad
		constexpr double kApRollGain = 3.0;       // roll input per radian of bank error
		constexpr double kApPitchMax = 0.35;      // rad: nose up / down at most this far
		constexpr double kApClimbPitch = 0.35;    // rad: nose up over an obstacle
		// Follow
		constexpr ULONGLONG kFollowHoldMs = 1000;  // hold the call key this long: follow on / off
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
		constexpr ULONGLONG kDetourHoldMs = 900;     // a detour step is held at least this long
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
		constexpr double kSelfHitBand = 3.0;      // m: a front sample this close to the rover's own height is the rover
		constexpr double kLandHeight = 1.0;       // m: settling under this (not climbing) for kLandMs ends the hover
		constexpr ULONGLONG kLandMs = 1500;
		constexpr float kMatchMetres = 10.0f;     // the physics body must be this close to the rover reference
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
			bool haveAim = false;
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
			// Pitch / roll direction checks (as the yaw one in 0.54.3): the body's change against the spin we asked for.
			double lastPitch = 0, lastRoll = 0, lastWP = 0, lastWR = 0, respP = 0, respR = 0;
			double pitchSign = 1.0, rollSign = 1.0;
			bool havePR = false;
			int fixedVel = 0;              // steps where an absurd body velocity was replaced
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
			float collisionDelta = 0;
			bool collisionUp = false;      // the contact pushed the vehicle up (something under it)
			LARGE_INTEGER lastStep{};
			// NPC pilots: the vehicle this pilot flies (its physics steps use this state). Pilot 0 (the player's) keeps
			// matching its body by position, as before.
			std::atomic<std::uintptr_t> vehicle{ 0 };
			// The physics step's `this` is the Havok vehicle (hknpBSVehicleInstance, vtable 0x144D66C98, step = slot 5
			// FUN_1423CB260), NOT the vehicle reference: comparing it with the reference never matched (2026-10-17 16:52:
			// an NPC pilot got 0 steps; the release guard and the ground gear cap never ran either). Matched by the body's
			// position once, then remembered.
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

		// Called every frame in a vehicle with the body heading: the camera reading whose turns follow the rover's.
		void LearnCamera(double heading)
		{
			double c, r;
			if (!CameraHeadings(c, r)) {
				return;
			}
			if (g_camPick.have) {
				const double dh = Wrap(heading - g_camPick.lastH);
				if (std::fabs(dh) > 0.002 && std::fabs(dh) < 0.3) {
					g_camPick.sumC = g_camPick.sumC * 0.998 + Wrap(c - g_camPick.lastC) * dh;
					g_camPick.sumR = g_camPick.sumR * 0.998 + Wrap(r - g_camPick.lastR) * dh;
					const int want = g_camPick.sumR >= g_camPick.sumC ? 1 : 0;
					if (std::fabs(g_camPick.sumR - g_camPick.sumC) > 0.05 && (want != g_camPick.pick || !g_camPick.told)) {
						g_camPick.pick = want;
						g_camPick.told = true;
						logger::Write("Rover flight: camera heading from the %s of its rotation (follows the rover: column %.3f, row %.3f)",
							want ? "row" : "column", g_camPick.sumC, g_camPick.sumR);
					}
				}
			}
			g_camPick.lastC = c;
			g_camPick.lastR = r;
			g_camPick.lastH = heading;
			g_camPick.have = true;
		}

		bool CameraHeading(double& yaw)
		{
			double c, r;
			if (!CameraHeadings(c, r)) {
				return false;
			}
			yaw = g_camPick.pick ? r : c;
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
			const float z = At<float>(root, g_camPick.pick ? 0x98 : 0xA4);
			if (!std::isfinite(z)) {
				return false;
			}
			pitch = std::asin(std::clamp(double(z), -1.0, 1.0));
			return true;
		}

		// The camera's resting pitch (it looks down at the rover from behind): learned while driving, so the aim is
		// "camera pitch above its rest" and a resting camera means level flight.
		struct CamRest
		{
			double pitch = 0;
			bool have = false;
			ULONGLONG lastTold = 0;
		} g_camRest;

		// Drive mode output per pilot (frame thread writes, the input hooks read): the controls for the vehicle's input block.
		struct DriveOut
		{
			std::atomic<bool> on{ false };
			std::atomic<std::uint32_t> vehicleId{ 0 };
			std::atomic<float> steer{ 0 }, accel{ 0 }, brake{ 0 };
			std::atomic<bool> boost{ false }, vert{ false };
		} g_driveOut[kPilots];

		using Step_t = std::uint64_t (*)(std::uintptr_t, std::uintptr_t);
		Step_t g_original = nullptr;

		// Booster visuals held while hovering (FUN_1413211F0 sets VehicleIsBoosting / VehicleIsVertBoosting each update;
		// right after it they are set again while hovering). Infinite boost: the fuel (+0x238) refilled.
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
		std::atomic<unsigned> g_occupiedSteps{ 0 }, g_unoccupiedSteps{ 0 };

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
			// Shift only changes gears (hovering and driving): the game's own boost gets no charge (+0x238), so its
			// Shift boost does nothing for the player's rover; other vehicles keep a full charge.
			const bool own = ctxVehicle && ctxVehicle == g_ownRover.load();
			// Unlimited boost (the user, 2026-10-17: "all vehicles should now have unlimited boost"): the game itself pins the
			// charge at 1.0 when its unlimited flag is set (FUN_1413211F0 start: component +0x238 = 1.0); done here for every
			// vehicle except those whose boost our flight model replaces: hovering ones, the vehicle the player drives (Shift is
			// the gear key there) and the player's own unattended rover -- unless drive mode is using the game's boost.
			const bool noBoost = !drive && (hovering || (driven && ctxVehicle == driven) || (own && ctxVehicle != driven));
			// Only vehicles in use get the pinned charge (2026-10-18: small parked vehicles kept hopping about; a permanently full charge on an idle
			// vehicle is one suspect, so parked ones keep the game's own charge).
			const bool inUse = hovering || drive || own || (driven && ctxVehicle == driven) ||
				(ctxVehicle && npcvehicles::UsesVehicle(*reinterpret_cast<std::uint32_t*>(ctxVehicle + 0x28)));
			if (component && noBoost) {
				*reinterpret_cast<float*>(component + 0x238) = 0.0f;
			}
			else if (component && inUse) {
				*reinterpret_cast<float*>(component + 0x238) = 1.0f;
			}
			g_boostOriginal(component, context);
			if (component && noBoost) {
				*reinterpret_cast<float*>(component + 0x238) = 0.0f;
			}
			else if (component && inUse) {
				*reinterpret_cast<float*>(component + 0x238) = 1.0f;
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
			std::uintptr_t body = 0, motion = 0, write = 0;
			std::uint32_t id = 0;
		};

		Body FindBody(std::uintptr_t vehicle, float px, float py, float pz, bool anyPosition = false)
		{
			Body b;
			const auto read = At<std::uintptr_t>(vehicle, 0x28);
			b.write = At<std::uintptr_t>(vehicle, 0x30);
			if (!read || !b.write) {
				return {};
			}
			b.id = At<std::uint32_t>(vehicle, 0x20);
			b.body = VFn<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(read, 0x70)(read, b.id);
			if (!b.body) {
				return {};
			}
			const float* t = reinterpret_cast<const float*>(b.body + 0x30);
			if (!anyPosition && (std::fabs(t[0] - px) > kMatchMetres || std::fabs(t[1] - py) > kMatchMetres || std::fabs(t[2] - pz) > kMatchMetres)) {
				return {};
			}
			b.motion = VFn<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(read, 0xD0)(read, At<std::uint32_t>(b.body, 0x80));
			return b.motion ? b : Body{};
		}

		// Before the game's step: a sharp change from what we wrote = the solver resolved a contact (hover ends).
		void CheckCollision(Shared& g_sh, std::uintptr_t vehicle)
		{
			float l[3], px, py, pz;
			{
				std::lock_guard lk(g_sh.lock);
				if (!g_sh.active || !g_sh.haveLast) {
					return;
				}
				l[0] = g_sh.lastV[0]; l[1] = g_sh.lastV[1]; l[2] = g_sh.lastV[2];
				px = g_sh.px; py = g_sh.py; pz = g_sh.pz;
			}
			const auto b = FindBody(vehicle, px, py, pz);
			if (!b.body) {
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
				g_sh.collided = true;
				g_sh.collisionDelta = d;
				if (v[2] - l[2] > 0.6f * d) g_sh.collisionUp = true;  // mostly up: something under it
			}
		}

		// ---- The flight model, once per physics step ---------------------------------------------------------------
		std::uint64_t StepModel(Shared& g_sh, std::uintptr_t vehicle, std::uint64_t result);

		// FindBody on any stepping vehicle (modded ones too) from the physics thread: guarded.
		bool BodyNear(std::uintptr_t vehicle, float px, float py, float pz)
		{
			__try {
				return FindBody(vehicle, px, py, pz).body != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::uint64_t StepDetour(std::uintptr_t vehicle, std::uintptr_t world)
		{
			// An NPC pilot's vehicle uses that pilot's state; everything else (and anything the player drives) pilot 0's.
			Shared* s = &g_sh;
			if (vehicle) {
				for (int i = 1; i < kPilots; ++i) {
					if (!g_shs[i].vehicle.load(std::memory_order_relaxed)) continue;
					if (g_shs[i].instance.load(std::memory_order_relaxed) == vehicle) {
						s = &g_shs[i];
						break;
					}
					if (!g_shs[i].instance.load(std::memory_order_relaxed)) {
						if (BodyNear(vehicle, g_shs[i].px, g_shs[i].py, g_shs[i].pz)) {  // within kMatchMetres of the pilot's vehicle
							g_shs[i].instance.store(vehicle, std::memory_order_relaxed);
							logger::Write("%s: physics vehicle %p matched", Who(i), reinterpret_cast<void*>(vehicle));
							s = &g_shs[i];
							break;
						}
					}
				}
			}
			CheckCollision(*s, vehicle);
			const auto result = g_original(vehicle, world);
			if (vehicle && vehicle == g_lastDrivenVehicle.load(std::memory_order_relaxed)) {
				(vehicle == g_driverVehicle.load(std::memory_order_relaxed) ? g_occupiedSteps : g_unoccupiedSteps).fetch_add(1, std::memory_order_relaxed);
			}
			return StepModel(*s, vehicle, result);
		}

		std::uint64_t StepModel(Shared& g_sh, std::uintptr_t vehicle, std::uint64_t result)
		{
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
				if (gb.body && !g_sh.active) {
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
				if (!gb.body || g_sh.active) {
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
				const double cap = g_sh.groundFrac * g_sh.groundTop;
				if (g_sh.groundFrac < 1.0 && hs > cap && hs > 0.5) {
					const double k = std::max(cap / hs, 0.97);  // eased: about 3 % per physics step
					gv[0] = float(gv[0] * k);
					gv[1] = float(gv[1] * k);
				}
				return result;
			}
			// Never hold our lock across a call into the game (0.57.0-0.58.0 did, and the game froze 0.5 s after a
			// re-centre while hovering: the physics step waited on the game while the frame task waited on our lock).
			const float qx = g_sh.px, qy = g_sh.py, qz = g_sh.pz;
			lk.unlock();
			const auto b = FindBody(vehicle, qx, qy, qz);
			lk.lock();
			if (!g_sh.active) {
				return result;
			}
			if (b.body) g_sh.instance.store(vehicle, std::memory_order_relaxed);
			if (!b.body) {
				return result;
			}
			LARGE_INTEGER now, freq;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			const double dt = g_sh.lastStep.QuadPart ? std::clamp(double(now.QuadPart - g_sh.lastStep.QuadPart) / double(freq.QuadPart), 1.0 / 240.0, 1.0 / 20.0) : 1.0 / 60.0;
			g_sh.lastStep = now;

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
			const double pitch = std::asin(std::clamp(double(F[2]), -1.0, 1.0));   // nose up +
			const double roll = std::asin(std::clamp(double(-R[2]), -1.0, 1.0));   // right side down +

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
			const double yawWant = g_sh.yawWant + g_sh.camYaw + kBankYaw * std::sin(roll) * speedK;  // banked: the nose follows the curve
			if (g_sh.havePR) {
				const double dp = pitch - g_sh.lastPitch, dr = roll - g_sh.lastRoll;
				if (std::fabs(g_sh.lastWP) > 0.2 && std::fabs(dp) < 0.2) {
					g_sh.respP = g_sh.respP * 0.99 + dp * g_sh.lastWP;
					if (g_sh.respP < -0.02) {
						g_sh.pitchSign = -g_sh.pitchSign;
						g_sh.respP = 0;
						logger::Write("Rover flight: pitch moved against the command; pitch spin reversed (sign %.0f)", g_sh.pitchSign);
					}
				}
				if (std::fabs(g_sh.lastWR) > 0.2 && std::fabs(dr) < 0.2) {
					g_sh.respR = g_sh.respR * 0.99 + dr * g_sh.lastWR;
					if (g_sh.respR < -0.02) {
						g_sh.rollSign = -g_sh.rollSign;
						g_sh.respR = 0;
						logger::Write("Rover flight: roll moved against the command; roll spin reversed (sign %.0f)", g_sh.rollSign);
					}
				}
			}
			// Pitch: the nose follows the camera aim (deadzone, then a rate toward it); without a camera it levels.
			double pitchWant = -pitch * kLevelRate;
			if (g_sh.haveAim) {
				double e = g_sh.pitchAim - pitch;
				e = e > 0 ? std::max(0.0, e - kCamPitchDeadzone) : std::min(0.0, e + kCamPitchDeadzone);
				pitchWant = std::clamp(kCamPitchGain * e, -kPitchRate, kPitchRate);
			}
			if ((pitch > kPitchMax && pitchWant > 0) || (pitch < -kPitchMax && pitchWant < 0)) pitchWant = 0;
			double rollWant = g_sh.rollIn * kRollRate;
			if (g_sh.rollAimOn) {
				rollWant = std::clamp(kApRollGain * (g_sh.rollAim - roll), -kRollRate, kRollRate);
			}
			else if (g_sh.rollIn == 0.0) {
				const double leanTo = std::clamp(kLean * g_sh.wYaw * speedK, -0.35, 0.35);  // lean into the turn
				rollWant = (leanTo - roll) * kLevelRate;
			}
			if ((roll > kRollMax && rollWant > 0) || (roll < -kRollMax && rollWant < 0)) rollWant = 0;
			g_sh.wYaw += (yawWant - g_sh.wYaw) * (1.0 - std::exp(-dt * kYawResp));
			g_sh.wPitch += (pitchWant - g_sh.wPitch) * (1.0 - std::exp(-dt * kTiltResp));
			g_sh.wRoll += (rollWant - g_sh.wRoll) * (1.0 - std::exp(-dt * kTiltResp));
			// Body angular velocity: pitch about the right axis (nose up +), roll about the nose (right down +), yaw about
			// world up (turn right = negative z).
			const double wp = g_sh.pitchSign * g_sh.wPitch, wr = g_sh.rollSign * g_sh.wRoll;
			alignas(16) float w[4] = { float(R[0] * wp + F[0] * wr), float(R[1] * wp + F[1] * wr), float(R[2] * wp + F[2] * wr - g_sh.wYaw), 0.0f };
			g_sh.lastPitch = pitch;
			g_sh.lastRoll = roll;
			g_sh.lastWP = g_sh.wPitch;
			g_sh.lastWR = g_sh.wRoll;
			g_sh.havePR = true;
			lk.unlock();
			VFn<void (*)(std::uintptr_t, std::uint32_t, const float*, int)>(b.write, 0xB0)(b.write, b.id, w, 0);
			lk.lock();

			// ---- Linear: pushes added to the body's own velocity ----
			const double fh = std::hypot(F[0], F[1]);
			const double hx = fh > 1e-4 ? F[0] / fh : 0.0, hy = fh > 1e-4 ? F[1] / fh : 1.0;  // level nose direction
			const double sx = hy, sy = -hx;                                                     // level right
			double ax = 0, ay = 0, az = 0;
			// Thrust along the 3D nose, or (avoidance stop) braking against the horizontal velocity.
			if (g_sh.braking) {
				if (hSpeed > 0.5) {
					ax -= kBrakeAcc * v[0] / hSpeed;
					ay -= kBrakeAcc * v[1] / hSpeed;
				}
			}
			else {
				ax += g_sh.thrustAcc * F[0];
				ay += g_sh.thrustAcc * F[1];
				az += g_sh.thrustAcc * F[2];
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
			if (g_sh.groundOk && z - g_sh.groundUnder < kMinClearance) {
				vzT = std::max(vzT, (kMinClearance - (z - g_sh.groundUnder)) * 3.0 + 1.0);
			}
			// Hover lift: carries gravity and drives the vertical speed to its target; it acts along the rover's up axis,
			// so a tilt pushes sideways (bank: the path curves) or forward (nose down).
			// Integral of the vertical speed error: rising faster than wanted lowers it, sinking raises it, so a wrong
			// gravity can never hold the vehicle up (or down). gEst is that integral (kept as the hover's "gravity").
			g_sh.gEst = std::clamp(g_sh.gEst + kLiftIntegral * (vzT - v[2]) * dt, 0.0, 30.0);
			double lift = std::clamp(kVzResponse * (vzT - v[2]), -kMaxDown, kMaxUp) + g_sh.gEst;
			const double upZ = std::max(0.3, double(U[2]));
			az += lift;
			const double tilt = std::max(0.0, lift) / upZ * kTiltPush;
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
			bool wDown = false, sDown = false;
			bool wHold = false;            // double-tapped W: accelerate stays on until W or S is pressed
			ULONGLONG wLastPress = 0;
			bool liftoff = false;          // climbing to kLiftoffHeight after switching on near the ground
			ULONGLONG startedAt = 0;       // when the flight switched on (contacts are ignored for kContactGraceMs)
			ULONGLONG contactGraceUntil = 0;  // after a push up, contacts until then are its aftermath, not a crash
			unsigned contactsIgnored = 0;
			bool autopilot = false;        // called: flying to the player with nobody aboard
			bool playerAuto = false;       // Auto: the player is aboard and the autopilot flies (any W/A/S/D takes over)
			std::uint32_t holdId = 0;      // the player got out with Auto on: this vehicle holds its spot
			double holdX = 0, holdY = 0;
			// Taxi test (Auto, 2026-10-17): no map marker, it hovers where Auto was switched on (or where the marker went
			// away); a marker, it flies there and holds over it.
			bool taxiWait = false;
			double taxiWaitX = 0, taxiWaitY = 0;
			std::uint32_t taxiMarker = 0;
			double taxiMx = 0, taxiMy = 0, taxiLeft = 0;
			bool taxiArrived = false;
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
			double cramped = 0;            // score that switches drive mode on
			double drvX = 0, drvY = 0, drvSp = 0, drvHead = 0, steerSign = 1.0, steerResp = 0, drvSteer = 0;
			ULONGLONG drvT = 0;
			ULONGLONG airSince = 0;
			double steerSecs = 0, stuckGX = 0, stuckGY = 0;
			int aiGear = 1;
			ULONGLONG shiftAt = 0;
			bool taxiOrbit = false;        // Auto's orbit around the marker is on (AutoMode::Orbit)
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
		};
		State g_gs[kPilots];
		world::Snapshot g_vsnap;       // this frame's vehicle list (main thread)
		bool g_vsnapOk = false;

		float VehRadius(const world::VehicleInfo& v) { return std::max(v.half[0], v.half[1]) > 0.4f ? std::max(v.half[0], v.half[1]) : 2.5f; }
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
				if (!r || !npcvehicles::LooksLikeActor(r) || npcvehicles::IsDead(r)) continue;
				if (EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(r) == vehicle) continue;  // aboard
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
		int MoveSlot(int idx) { return idx ? CorridorSlot(idx, 0) : prefetch::kSlotRescue0; }

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
			int call = 0;                  // summon: free in every gameplay context (used on foot)
			int follow = 0;                // follow on / off (on foot; U is the hover toggle only in the rover)
			int autoKey = 0;               // Auto on / off (in the vehicle)
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
			hoverui::SetKeyNames(controlmap::KeyName(g_mk.accel).c_str(), controlmap::KeyName(g_mk.brake).c_str());
		}

		void ChooseKeys()
		{
			if (g_keys.chosen) {
				return;
			}
			auto freeHere = [](int vk) { return controlmap::KeyFreeIn(vk, "Vehicle"); };
			for (const int vk : std::initializer_list<int>{ 'U', 'J', 'K', 'Y', 'O', 'P', 'B', 'N' }) {
				if (freeHere(vk) && (vk == 'U' || controlmap::KeyFree(vk))) {
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
			for (const int vk : std::initializer_list<int>{ 'J', 'K', 'Y', 'O', 'P', 'B', 'N' }) {
				if (vk != g_keys.toggle && controlmap::KeyFree(vk)) {
					g_keys.call = vk;
					break;
				}
			}
			for (const int vk : std::initializer_list<int>{ 'U', VK_OEM_1, VK_OEM_7, VK_OEM_5 }) {
				if (vk != g_keys.call && controlmap::KeyFree(vk)) {
					g_keys.follow = vk;
					break;
				}
			}
			for (const int vk : std::initializer_list<int>{ 'T', 'J', 'K', 'Y', 'O', 'P', 'B' }) {
				if (vk != g_keys.toggle && vk != g_keys.up && vk != g_keys.down && freeHere(vk)) {
					g_keys.autoKey = vk;
					break;
				}
			}
			logger::Write("Rover auto key: %s", g_keys.autoKey ? controlmap::KeyName(g_keys.autoKey).c_str() : "NONE FREE");
			logger::Write("Rover call key: %s, follow key: %s", g_keys.call ? controlmap::KeyName(g_keys.call).c_str() : "NONE FREE",
				g_keys.follow ? controlmap::KeyName(g_keys.follow).c_str() : "NONE FREE");
			g_keys.chosen = true;
			controlmap::Note("flight", "hover toggle", g_keys.toggle);
			controlmap::Note("flight", "roll left", g_keys.rollLeft);
			controlmap::Note("flight", "roll right", g_keys.rollRight);
			controlmap::Note("flight", "up", g_keys.up);
			controlmap::Note("flight", "down", g_keys.down);
			controlmap::Note("flight", "call", g_keys.call);
			controlmap::Note("flight", "follow", g_keys.follow);
			controlmap::Note("flight", "auto", g_keys.autoKey);
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
		bool g_follow = false;            // follow mode (outlives hover starts / stops)
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

		// NPC pilots' moves (stuck escape): the vehicle by form id, to a point; grounded by a ray in its own slot.
		struct PilotMove
		{
			bool pending = false;
			std::uint32_t id = 0;
			double x = 0, y = 0, z = 0, yaw = 0;
			ULONGLONG asked = 0;
		} g_pmoves[kPilots];
		int MoveSlot(int idx);
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

		bool TaxiMarker(std::uintptr_t player, std::uintptr_t vehicle, std::uint32_t& id, double& x, double& y)
		{
			id = At<std::uint32_t>(player, off::kPlayerCustomMarker);
			// 21:03-21:05: Auto waited with no "marker" line at all. Every 3 s while waiting: what the read sees.
			static ULONGLONG said = 0;
			const ULONGLONG now = GetTickCount64();
			const bool say = now - said >= 3000;
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
			x = mp.x;
			y = mp.y;
			const double d = std::hypot(x - vp.x, y - vp.y);
			if (d >= kTaxiMaxRange) {
				if (say) logger::Write("Rover taxi: waiting; marker %08X at (%.0f, %.0f) is %.0f m away (over %.0f m)", id, mp.x, mp.y, d, kTaxiMaxRange);
				return false;
			}
			return true;
		}

		void AskMove(std::uintptr_t player)
		{
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
			float h = -3.4e38f;
			unsigned age = 0;
			const bool have = prefetch::LatestGround(prefetch::kSlotRescue0, h, age) && age <= 2 && h > -1e30f && h != -500.0f;
			if (!have && GetTickCount64() - g_move.asked < 1000) return;  // wait for the ray (up to 1 s)
			g_move.pending = false;
			const auto rover = Rover();
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			if (!rover || !cell || !ws) {
				logger::Write("Rover call: move cancelled (%s)", rover ? "player not outdoors" : "rover not found");
				return;
			}
			float pos[3] = { float(g_move.x), float(g_move.y), float((have ? h : g_move.z) + 1.0) };
			float rot[3] = { 0.0f, 0.0f, float(g_move.yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(rover, &handle, cell, ws, pos, rot);
				logger::Write("Rover call: moved to (%.1f, %.1f, %.1f) next to the player (ground %s)", pos[0], pos[1], pos[2], have ? "from the ray" : "not found: player height");
				g_movedAt = GetTickCount64();
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::Write("Rover call: move raised an exception");
			}
		}

		void AskPilotMove(int idx, std::uint32_t id, double x, double y, double z, double yaw)
		{
			g_pmoves[idx] = { true, id, x, y, z, yaw, GetTickCount64() };
			prefetch::RequestGround(MoveSlot(idx), float(x), float(y), float(z) + 60.0f, 0.0f, 400.0f);
		}

		void FinishPilotMoves(std::uintptr_t player)
		{
			for (int idx = 1; idx < kPilots; ++idx) {
				auto& m = g_pmoves[idx];
				if (!m.pending) continue;
				float h = -3.4e38f;
				unsigned age = 0;
				const bool have = prefetch::LatestGround(MoveSlot(idx), h, age) && age <= 2 && h > -1e30f && h != -500.0f;
				if (!have && GetTickCount64() - m.asked < 1000) continue;
				m.pending = false;
				const auto vehicle = RefById(m.id);
				const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
				const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
				if (!vehicle || !cell || !ws) {
					logger::Write("%s: move cancelled (%s)", Who(idx), vehicle ? "player not outdoors" : "vehicle not found");
					continue;
				}
				float pos[3] = { float(m.x), float(m.y), float((have ? h : m.z) + 1.0) };
				float rot[3] = { 0.0f, 0.0f, float(m.yaw) };
				std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
				__try {
					EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(vehicle, &handle, cell, ws, pos, rot);
					logger::Write("%s: moved to (%.1f, %.1f, %.1f) (ground %s)", Who(idx), pos[0], pos[1], pos[2], have ? "from the ray" : "not found: player height");
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					logger::Write("%s: move raised an exception", Who(idx));
				}
			}
		}

		void Stop(int idx, const char* why)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const auto releasedVehicle = g_hoverVehicles[idx].load();
			if (releasedVehicle) g_clearBoosts[idx] = releasedVehicle;  // its boost flags off at its next boost update
			g.active = false;
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.active = false;
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
				if (g.autopilot || g_sh.autopilot) {
					g_sh.guardVehicle = releasedVehicle;  // an NPC pilot keeps its vehicle routed to it until the guard ends
					g_sh.guardInstance = g_sh.instance.load(std::memory_order_relaxed);
					g_sh.guardUntil = GetTickCount64() + kGuardMs;
					g_sh.guardMax = 0;
				}
				g_sh.autopilot = false;
			}
			std::snprintf(g_endWhy[idx], sizeof(g_endWhy[idx]), "%s", why);
			if (g.autopilot) logger::Write("%s: autopilot ended (%s)", idx ? Who(idx) : "Rover call", why);
			g.autopilot = false;
			g.playerAuto = false;
			g.holdId = 0;
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
			if (!idx && !g_camRest.have) {
				double cp;
				if (CameraPitch(cp)) {
					g_camRest.pitch = cp;  // never driven yet: the camera now is the rest
					g_camRest.have = true;
				}
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
				g_sh.gEst = std::clamp(g_sh.gEst, 3.0, 20.0);  // a drained estimate would start a flight with no lift
				g_sh.holding = false;
				g_sh.haveLast = false;
				g_sh.collided = false;
				g_sh.guardVehicle = 0;
				g_sh.active = true;
			}
			g_sh.instance.store(0);  // matched again from the new vehicle's body
			if (idx) g_sh.vehicle = vehicle;
			g_hoverVehicles[idx] = vehicle;
			g_endWhy[idx][0] = 0;
			if (idx) {
				logger::Write("%s: hover on at (%.0f, %.0f, %.0f)", Who(idx), pos.x, pos.y, pos.z);
				return;
			}
			logger::Write("Rover flight: on at (%.0f, %.0f, %.0f)", pos.x, pos.y, pos.z);
			notify::Log("Hover on - [W] boost [S] brake [Shift+W/S] gear  [%s]/[%s] up/down  [Shift+Space] down  [%s]/[%s] turn  [Space x2] off", controlmap::KeyName(g_keys.up).c_str(),
				controlmap::KeyName(g_keys.down).c_str(), controlmap::KeyName(g_keys.rollLeft).c_str(), controlmap::KeyName(g_keys.rollRight).c_str());
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
				prefetch::RequestGround(ModelSlot(idx, i), float(g.asked[i].x), float(g.asked[i].y), i == kCentre ? pos.z - 1.5f : pos.z + float(kCastAbove),
					0.0f, 8000.0f);
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
					prefetch::RequestGround(CorridorSlot(idx, c * 3 + k), float(q.x), float(q.y), pos.z + float(kCastAbove), 0.0f, 8000.0f);
				}
				const double range = std::max(kRayMin, speed * kRayLead);
				const float from[3] = { float(pos.x + std::sin(hd) * kRayStart), float(pos.y + std::cos(hd) * kRayStart), float(pos.z + kRayLift) };
				const float to[3] = { float(from[0] + std::sin(hd) * range), float(from[1] + std::cos(hd) * range), from[2] };
				prefetch::RequestRay(idx * 3 + c, from, to, kWallLayers);
				g.rayRange[c] = range;
			}
			g.haveCorr = true;
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
						logger::Write("%s: hull sweep blocked %.1f m ahead (layer %u, body %u) where the thin ray saw nothing", Who(idx), sw.distance, sw.layer, sw.bodyId);
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
				g_sh.holding = false;
			}
			g.liftoff = true;
			g.holding = false;
			QueryPerformanceCounter(&g.last);
			logger::Write("%s: drive mode off (%s): hover on", idx ? Who(idx) : "Rover call", why);
		}

		// Stuck or lost on autopilot: stop and move the vehicle next to the target (pilot 0: next to the player, as the
		// far summon; NPC pilots: to their goal point). Following picks up again from there.
		void EscapeMove(int idx, std::uintptr_t vehicle, const Goal& goal)
		{
			if (!idx && (goal.cruise || goal.hold)) {
				State& g0 = g_gs[0];
				if (goal.cruise) {
					g0.autopilot = false;
					g0.playerAuto = false;
					notify::Log("Auto off: stuck");
					logger::Write("Rover auto: stuck; control back to the player");
				}
				else {
					// Taxi / hold: keep flying; climb and come at it again.
					g0.cruiseExtra = std::min(kCruiseExtraMax, g0.cruiseExtra + 12.0);
					g0.apClimb = true;
					g0.autopilotSince = GetTickCount64();
					logger::Write("Rover taxi: stuck; climbing out (+%.0f m), still flying", g0.cruiseExtra);
				}
				return;
			}
			Stop(idx, "stuck; moved to the target");
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			if (!player) return;
			if (!idx) {
				AskMove(player);
				return;
			}
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			AskPilotMove(idx, At<std::uint32_t>(vehicle, 0x28), goal.x, goal.y, pp.z, At<game::Vec3>(player, off::kRefRotation).z);
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

		// One frame of a hovering pilot: the assist, avoidance, height hold and (autopilot) the flight to the goal.
		void Fly(int idx, std::uintptr_t vehicle, const Goal& goal, bool focused)
		{
			State& g = g_gs[idx];
			Shared& g_sh = g_shs[idx];
			const char* wf = idx ? Who(idx) : "Rover flight";
			const char* wc = idx ? Who(idx) : "Rover call";
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

			// ---- Input ----
			// Shift + W / Shift + S: one gear up / down per press (gears 1-4 set the boost strength and top speed; 1-4 keys
			// pick a gear directly). Plain W held: boost at the selected gear. Plain S held: brake. Neither: coast.
			const bool keysOn = focused && !g.autopilot;  // on autopilot the player's keys do not fly the rover
			const bool shiftHeld = keysOn && Held(g_mk.boost);
			const bool wHeld = keysOn && Held(g_mk.accel), sHeld = keysOn && Held(g_mk.brake);
			const bool wWasDown = g.wDown, sWasDown = g.sDown;
			{
				const int before = g.gear;
				if (shiftHeld && wHeld && !g.wDown) g.gear = std::min(4, g.gear + 1);
				if (shiftHeld && sHeld && !g.sDown) g.gear = std::max(1, g.gear - 1);
				g.wDown = wHeld;
				g.sDown = sHeld;
				for (int i = 1; i <= 4; ++i) {
					if (keysOn && Held('0' + i)) g.gear = i;
				}
				if (g.gear != before) {
					if (!idx) notify::Log("Gear %d of 4  (%.0f m/s)", g.gear, kGearTop[g.gear]);
					logger::Write("%s: gear %d -> %d", wf, before, g.gear);
				}
			}
			// W: accelerate while held; double-tap W: accelerate stays on; the next W or S press releases it.
			{
				const bool wPress = wHeld && !shiftHeld && !wWasDown;
				const bool sPress = sHeld && !shiftHeld && !sWasDown;
				if (wPress) {
					const ULONGLONG nowMs = GetTickCount64();
					if (g.wHold) {
						g.wHold = false;
						g.wLastPress = 0;
					}
					else if (g.wLastPress && nowMs - g.wLastPress <= kDoubleTapMs) {
						g.wHold = true;
						g.wLastPress = 0;
						logger::Write("%s: accelerate held (double-tapped W)", wf);
					}
					else {
						g.wLastPress = nowMs;
					}
				}
				if (sPress && g.wHold) {
					g.wHold = false;
				}
			}
			bool boosting = (wHeld && !shiftHeld) || g.wHold;
			bool sBrake = sHeld && !shiftHeld;
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
				const double brakeAt = stopR + sp * kCallBrakeSlack;
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
							logger::Write("%s: gear %d -> %d (want %.0f m/s, at %.0f)", wc, before, ag, desired, sp);
						}
					}
					if (ag > g.gearCap) {
						logger::Write("%s: gear %d -> %d (path cap)", wc, ag, g.gearCap);
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
						logger::Write("%s: climbed to %.1f m; flying to the target", wc, above);
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
				if (!goal.keepGoing && dist <= goal.stop + 1.0 && sp < 1.5) {
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
			double thrustAcc = boosting ? kThrust * (g.aiGears ? kAiGearThrust[apGear] : kGearThrust[apGear]) * (g.autopilot ? g.throttle : 1.0) : 0.0;
			bool thrust = thrustAcc > 0.0;
			const double pitchIn = 0.0;
			// The camera aim: its pitch above its resting pitch is the nose pitch wanted.
			double camPitch = 0, pitchAim = 0;  // modified by the avoidance climb
			bool haveAim = !g.autopilot && g_camRest.have && CameraPitch(camPitch);  // autopilot: its own pitch below
			if (haveAim) {
				pitchAim = std::clamp(camPitch - g_camRest.pitch, -kPitchMax, kPitchMax);
			}
			double rollIn = (keysOn && Held(g_mk.right) ? 1.0 : 0.0) -
				(keysOn && Held(g_mk.left) ? 1.0 : 0.0);
			double lift = (keysOn && Held(g_keys.up) ? 1.0 : 0.0) -
				(keysOn && Held(g_keys.down) ? 1.0 : 0.0);
			if (keysOn && Held(g_mk.boost) && Held(g_mk.vert)) lift = -1.0;  // Shift + Space: descend (the user, 2026-10-17)
			g_forwardBoosts[idx] = boost;

			// ---- Yaw follows the camera gap in the vehicle step. The camera is only read, never written. ----
			double view = 0;
			bool haveCam = CameraHeading(view);
			if (g.autopilot) {
				view = apBearing;  // the player's bearing stands in for the camera: the rover turns toward the player
				haveCam = apSteer;
			}
			double yawWant = 0.0;

			double top = g.aiGears ? kAiGearTop[apGear] : boosting ? kGearTop[apGear] : std::max(kGearTop[apGear], kSpeedCap);  // AI: the gear caps it
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
					if (f == 2 && !hold && goal.keepGoing && !goal.cruise && !goal.hold && !g.trailOn && g_trailCount > 3 && std::hypot(goal.x - pos.x, goal.y - pos.y) > kTrailOff) {
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
							rq.priority = PilotNavOf(idx).priority > 0 ? PilotNavOf(idx).priority : (idx == 0 ? 120 : 60);
							rq.pos[0] = pos.x; rq.pos[1] = pos.y; rq.pos[2] = pos.z;
							rq.dir[0] = float(std::sin(apBearing));
							rq.dir[1] = float(std::cos(apBearing));
							rq.speed = float(std::max(g.drive ? g.drvSp : sp, 6.0));
							rq.ground = g.drive;
							rq.distToGoal = float(std::hypot(goal.x - pos.x, goal.y - pos.y));
							const routeplan::Plan pl = routeplan::Update(idx, rq);
							apBearing += pl.yawOffset;
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
					if (g.detourAng != 0 || f == 2) top = std::min(top, f == 2 ? 12.0 : kDetourSlowTo);
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
					logger::Write("%s: gear cap %d (%s)", wc, capNow, why);
				}
				else if (capNow > g.gearCap) {
					if (!g.capClearSince) g.capClearSince = nowMs;
					else if (nowMs - g.capClearSince >= kCapRaiseMs) {
						++g.gearCap;
						g.capClearSince = nowMs;
						logger::Write("%s: gear cap %d (clear)", wc, g.gearCap);
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
				if (!g.drive && occupied && g.cramped >= kCrampedOn && !g.apClimb && std::fabs(v[2]) < 4.0) {  // not mid-fall or mid-climb (it may stand on a structure the ground rays miss)
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
					logger::Write("%s: drive mode ON (cramped %.1f): hover off, driving with boost for forward and up", wc, g.cramped);
				}
				else if (g.drive && tNow - g.driveSince > kDriveMinMs && g.cramped < 0.5 && g.lastF == 0 && g.lastL < 2 && g.lastR < 2 && !g.apClimb) {
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
					const bool driveBoost = fwdBoost || (vert && accel > 0.4);  // (named so it no longer hides the booster-visuals `boost` above)
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
					double want = kCallSkim + (g.autopilot ? EnvClimb() : 0.0);  // night / bad weather: a little higher
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
					g.hold = g.cruiseHold + g.cruiseExtra + g.laneZ;
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
				rollAim = std::clamp(kApBankPerYaw * yawWant * std::clamp(sp / 15.0, 0.0, 1.0), -kApBankMax, kApBankMax);
				rollAimOn = true;
				const auto& hc = g_cmds[idx];
				if (hc.c.set && nowMs < hc.until) {
					const auto& c = hc.c;
					const int cg = (c.set & Command::kGear) ? std::clamp(c.gear, 1, 4) : apGear;
					if (c.set & Command::kThrottle) {
						thrustAcc = std::clamp(c.throttle, -1.0, 1.0) * kThrust * kGearThrust[cg];
						thrust = thrustAcc != 0.0;
						if (c.throttle != 0.0) braking = false;
					}
					if (c.set & Command::kBrake) {
						braking = c.brake;
						if (c.brake) { thrustAcc = 0; thrust = false; }
					}
					if (c.set & Command::kStrafe) strafe = std::clamp(c.strafe, -1.0, 1.0);
					if (c.set & Command::kVertical) { vzOn = true; vzCmd = c.vertical; }
					if (c.set & Command::kYaw) { yawWant = c.yawRate; haveCam = false; }
					if (c.set & Command::kPitch) { pitchAim = std::clamp(c.pitch, -kPitchMax, kPitchMax); haveAim = true; }
					if (c.set & Command::kRoll) { rollAim = std::clamp(c.roll, -kRollMax, kRollMax); rollAimOn = true; }
					if (c.set & Command::kGear) g.gear = cg;
				}
			}
			AskCorridors(idx, pos, (g.autopilot && apSteer && !g.apClimb) ? apBearing : (sp > 2.0 ? std::atan2(v[0], v[1]) : bodyHeading), std::max(sp, g.autopilot ? 10.0 : 0.0));
			{
				std::lock_guard lk(g_sh.lock);
				g_sh.px = pos.x; g_sh.py = pos.y; g_sh.pz = pos.z;
				g_sh.thrustAcc = thrustAcc;
				g_sh.pitchAim = pitchAim;
				g_sh.haveAim = haveAim;
				g_sh.thrusting = thrust;
				g_sh.braking = braking;
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
				g_sh.holding = g.holding;
				g_sh.hold = g.hold;
				g_sh.refGround = refGround;
				g_sh.groundUnder = floorOn ? std::max(g.ground, g.floorZ - 0.5) : g.ground;
				g_sh.groundOk = groundOk;
				g_sh.airborne = groundOk && above > 1.5;
				g_sh.cap = g.cap;
			}

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
					"pitch %.2f (aim %.2f: camera %.2f, rest %.2f) roll %.2f (in %.0f, signs %.0f / %.0f), yaw spin %.2f; gear %d boost %d; heading %.2f, camera %s (%s) %.2f gap %.2f -> turn %.2f; lift %.0f; thrust %d assist %d; gravity %.2f; %d bad velocities replaced; %u ground samples rejected",
					wf, pos.x, pos.y, pos.z, matched ? "matched" : "NOT matched", steps, sp, g.cap, aheadLoaded ? "loaded" : "NOT loaded",
					near0 ? 1 : 0, near1 ? 1 : 0, far2 ? 1 : 0,
					ray0 ? 1 : 0, ray1 ? 1 : 0, ray2 ? 1 : 0, cell0 ? 1 : 0, cell1 ? 1 : 0, cell2 ? 1 : 0,
					v[2], above, groundOk ? "ground" : "(no ground)",
					g.holding ? "holding" : "free", g.hold, refGround, frontOk ? "used" : "ignored", bodyPitch, pitchAim, camPitch, g_camRest.pitch, bodyRoll, rollIn, g_sh.pitchSign, g_sh.rollSign, wYaw, g.gear, boost ? 1 : 0,
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

	void PlayerTick()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !g_original) {
			hoverui::SetState(false, false, g.gear, false);
			g_driverVehicle.store(0, std::memory_order_relaxed);
			return;
		}
		// The player died (aboard or not): the vehicle can no longer go on. Hover, Auto, follow and summon end and stay
		// off until a load brings the player back (the dead check is the actor's vtable +0x868, as for NPCs).
		static bool deadSaid = false;
		if (PlayerDead(player)) {
			if (!deadSaid) {
				deadSaid = true;
				logger::Write("Rover: you died%s; vehicle control off", g.active ? " (hover on: ended)" : "");
			}
			if (g.active) Stop("you died");
			g.playerAuto = false;
			g.holdId = 0;
			g_follow = false;
			hoverui::SetState(false, false, g.gear, false);
			hoverui::SetAuto("", false, "");
			hoverui::SetFoot(false, false, "", "", "", "");
			g_driverVehicle.store(0, std::memory_order_relaxed);
			return;
		}
		deadSaid = false;
		const bool focused = Focused();
		ChooseKeys();
		ReadMoveKeys();
		hoverui::EditTick(focused);
		const bool h = focused && Held(g_keys.toggle);
		const bool pressed = h && !g.keyDown;
		g.keyDown = h;
		const auto driven = PlayerVehicle(player);
		hoverui::SetState(driven != 0, g.active && (!g.autopilot || g.playerAuto), g.gear, g.wHold);
		g_driverVehicle.store(driven, std::memory_order_relaxed);
		// Got out with Auto on: the vehicle holds where it is (it neither follows nor lands).
		if (!driven && g.playerAuto) {
			const auto last = g_lastDrivenVehicle.load(std::memory_order_relaxed);
			g.playerAuto = false;
			if (last && g.active) {
				const auto hp = At<game::Vec3>(last, off::kRefPosition);
				g.holdId = At<std::uint32_t>(last, 0x28);
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
			g.holdId = 0;
			g_lastDrivenVehicle.store(driven, std::memory_order_relaxed);
			if (g.autopilot && !g.playerAuto) {
				g.autopilot = false;  // the player got in: hover stays, under the player's control
				logger::Write("Rover call: player boarded; autopilot off");
			}
			// Auto key: the autopilot flies with the player aboard; any movement key takes control back.
			static bool autoDown = false;
			const bool ak = focused && g_keys.autoKey && Held(g_keys.autoKey);
			const bool autoPressed = ak && !autoDown;
			autoDown = ak;
			const bool goBoard = g_goOnBoard.load() && !g.playerAuto && GetTickCount64() - g_goAt.load() < 900000 && At<std::uint32_t>(driven, 0x28) == g_roverId;
			if (goBoard) {
				g_goOnBoard.store(false);
				logger::Write("Rover scanner trip: you boarded; Auto starts toward your marker");
			}
			if (autoPressed || goBoard) {
				if (g.playerAuto && !goBoard) {
					g.playerAuto = false;
					g.autopilot = false;
					notify::Log("Auto off");
					logger::Write("Rover auto: off (key)");
				}
				else {
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
					notify::Log("Auto on - place a map marker to go; W/A/S/D takes over");
					logger::Write("Rover auto: on (%s)", wasHovering ? "already hovering" : "lifting off");
				}
			}
			else if (g.playerAuto && focused && (Held(g_mk.accel) || Held(g_mk.brake) || Held(g_mk.left) || Held(g_mk.right))) {
				g.playerAuto = false;
				g.autopilot = false;
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
			if (g_follow) {
				g_follow = false;
				logger::Write("Rover follow: off (a follower drives the vehicle)");
			}
		}
		// The call key (on foot): far -> move next to the player; close -> autopilot; again -> cancel.
		{
			static bool callDown = false;
			const bool c = focused && Held(g_keys.call);
			const bool scanCall = g_scanCall.exchange(false);  // the scanner marker button counts as the call key
			bool callPressed = (c && !callDown) || scanCall;
			callDown = c;
			// The follow key (on foot): follow on / off.
			static bool followDown = false;
			const bool fk = focused && Held(g_keys.follow);
			bool followPressed = fk && !followDown;
			// Hold Follow (kFollowHoldMs): every escort drops its fight and comes back into formation for 15 s.
			static ULONGLONG followAt = 0;
			static bool recalled = false;
			if (followPressed) { followAt = GetTickCount64(); recalled = false; }
			if (fk && !recalled && followAt && GetTickCount64() - followAt >= kFollowHoldMs) {
				recalled = true;
				npcvehicles::Recall(15000);
			}
			followDown = fk;
			// Crew: with a follower driving or free, the buttons go to the follower.
			if (!driven && (callPressed || followPressed) && g_roverId) {
				const auto order = npcvehicles::CrewOrderOf(g_roverId);
				const auto ppos = At<game::Vec3>(player, off::kRefPosition);
				if (order != npcvehicles::CrewOrder::None) {
					if (followPressed && order == npcvehicles::CrewOrder::Follow) npcvehicles::ReleaseCrew(g_roverId);  // follow off: the follower gets out
					else npcvehicles::SetCrewOrder(g_roverId, scanCall ? npcvehicles::CrewOrder::Marker : npcvehicles::CrewOrder::Follow);
					g_goOnBoard.store(false);
					callPressed = followPressed = false;
				}
				else if (g_crewPending != npcvehicles::CrewOrder::None && followPressed) {
					g_crewPending = npcvehicles::CrewOrder::None;
					logger::Write("Rover crew: cancelled");
					notify::Show("Follower order cancelled");
					followPressed = false;
				}
				else if (Rover() && !g_follow && npcvehicles::FreeFollower(player, ppos.x, ppos.y, 60.0)) {
					g_crewPending = scanCall ? npcvehicles::CrewOrder::Marker : npcvehicles::CrewOrder::Follow;
					g_crewAt = GetTickCount64();
					g_goOnBoard.store(false);
					logger::Write("Rover crew: %s; bringing the vehicle for a follower", scanCall ? "marker" : "follow");
					notify::Show("A follower will drive your vehicle");
					callPressed = true;    // the vehicle is brought as a summon brings it
					followPressed = false; // the follower follows, not the empty vehicle
				}
			}
			if (followPressed && !driven) {
				if (g_follow) {
					g_follow = false;
					logger::Write("Rover follow: off");
					if (g.autopilot) Stop("follow off");
				}
				else if (Rover()) {
					g_follow = true;
					g_followStill = 0;
					logger::Write("Rover follow: on");
				}
				else {
					logger::Write("Rover follow: you own no vehicle");
				}
			}
			if ((callPressed || followPressed) && g_roverId && vcontract::AssignedTo(g_roverId)) {
				vcontract::Unassign(g_roverId);  // the player's commands always win over a profile
				logger::Write("Rover profile: unassigned (summon / follow pressed)");
				if (g_remote) {
					g_remote = false;
					if (g.autopilot) Stop("profile replaced by summon / follow");
				}
			}
			if ((callPressed || followPressed) && !driven && g.holdId) Stop("hold ended (summon / follow)");
			if (callPressed && !driven) {
				bool takeOver = false;
				if (g_follow) {
					g_follow = false;  // a summon replaces following: a flying rover keeps flying, now to the player
					takeOver = g.autopilot;
					logger::Write("Rover follow: off (summoned)");
				}
				const auto rover = Rover();
				if (takeOver) {
					g.autopilotSince = GetTickCount64();
				}
				else if (g.autopilot) {
					Stop("call cancelled");
				}
				else if (!rover) {
					logger::Write("Rover call: you own no vehicle (no vehicle link from the player or the home ship)");
				}
				else {
					const auto rp = At<game::Vec3>(rover, off::kRefPosition), pp = At<game::Vec3>(player, off::kRefPosition);
					const double dist = std::hypot(double(rp.x) - pp.x, double(rp.y) - pp.y);
					const auto loaded = At<std::uintptr_t>(rover, off::kRefLoadedData);
					const bool has3D = loaded && At<std::uintptr_t>(loaded, 0x08);
					const bool sameWorld = Worldspace(rover) && Worldspace(rover) == Worldspace(player);
					if (!has3D || !sameWorld || !std::isfinite(dist) || dist > kCallFar) {
						logger::Write("Rover call: %08X %s; moving it next to the player", g_roverId,
							!sameWorld ? "in another worldspace" : !has3D ? "not loaded" : "far");
						AskMove(player);
					}
					else if (dist <= kCallStop + 2.0) {
						logger::Write("Rover call: already here (%.0f m)", dist);
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
						logger::Write("Rover crew: no follower could board; the vehicle %s", order == npcvehicles::CrewOrder::Follow ? "follows by itself" : "waits for you");
						if (order == npcvehicles::CrewOrder::Follow) {
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
				else if (playerStill && distP < kFollowFar && !g_air.air) {
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
					vcontract::Unassign(g_roverId);  // remote profiles are for an empty vehicle; Cruise is handled by hook 2a
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
			if (!driven) {
				if (g.holdId) std::snprintf(status, sizeof(status), "HOLDING");
				else if (g_follow) std::snprintf(status, sizeof(status), "FOLLOWING  %.0f m", std::min(toRover, 9999.0));
				else if (g.autopilot) std::snprintf(status, sizeof(status), "SUMMONING  %.0f m", std::min(toRover, 9999.0));
				else if (g_move.pending || (g_movedAt && nowMs - g_movedAt < 4000)) std::snprintf(status, sizeof(status), "MOVED TO YOU");
			}
			const std::string callKey = g_keys.call ? controlmap::KeyName(g_keys.call) : std::string();
			const std::string followKey = g_keys.follow ? controlmap::KeyName(g_keys.follow) : std::string();
			const std::string useKey = controlmap::KeyName(controlmap::KeyFor("MainGameplay", "Activate", 'E'));
			hoverui::SetFoot(!driven && g_roverId != 0, rover && !driven && !g.autopilot && toRover <= kNearBoard, callKey.c_str(), followKey.c_str(), useKey.c_str(), status);
			char line[64] = "";
			if (driven && g.playerAuto) {
				if (g.taxiMarker && g.taxiArrived) std::snprintf(line, sizeof(line), "ARRIVED");
				else if (g.taxiMarker) std::snprintf(line, sizeof(line), "TO MARKER  %.0f m", std::min(g.taxiLeft, 99999.0));
				else std::snprintf(line, sizeof(line), "SET A MARKER");
			}
			const std::string autoKey = g_keys.autoKey ? controlmap::KeyName(g_keys.autoKey) : std::string();
			hoverui::SetAuto(autoKey.c_str(), driven && g.playerAuto, line);
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
			const unsigned occupied = g_occupiedSteps.exchange(0, std::memory_order_relaxed);
			const unsigned unoccupied = g_unoccupiedSteps.exchange(0, std::memory_order_relaxed);
			if (occupied || unoccupied) {
				logger::Write("Ship vehicle physics probe: last rover stepped %u times while driven, %u times after leaving it (10 s)", occupied, unoccupied);
			}
		}
		const bool boostTap = g_tapBoost.Update(focused && Held(g_mk.vert) && !Held(g_mk.boost));  // Shift + Space is descend, not a tap
		static std::uintptr_t announced = 0;
		if (vehicle && vehicle != announced && !g.active) {
			announced = vehicle;
			notify::Log("Rover: double-tap vertical boost (Space) to hover");
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
			LearnCamera(At<game::Vec3>(vehicle, off::kRefRotation).z);
			double cp;
			if (CameraPitch(cp)) {
				g_camRest.pitch = g_camRest.have ? g_camRest.pitch + (cp - g_camRest.pitch) * 0.01 : cp;
				g_camRest.have = true;
			}
		}
		if (boostTap && driven) {  // on foot (autopilot) a double jump must not cancel the call
			if (g.active) {
				Stop("double-tapped vertical boost");
				return;
			}
			Start(vehicle);
			return;
		}
		if (pressed && driven) {
			if (g.active) {
				Stop(controlmap::KeyName(g_keys.toggle).c_str());
				return;
			}
			Start(vehicle);
		}
		if (!g.active) {
			// Driving: Shift + Accelerate / Shift + Brake change gear here too; the gear caps the ground speed.
			const bool shiftHeld = focused && Held(g_mk.boost);
			const bool wHeld = focused && Held(g_mk.accel), sHeld = focused && Held(g_mk.brake);
			const int before = g.gear;
			if (shiftHeld && wHeld && !g.wDown) g.gear = std::min(4, g.gear + 1);
			if (shiftHeld && sHeld && !g.sDown) g.gear = std::max(1, g.gear - 1);
			g.wDown = wHeld;
			g.sDown = sHeld;
			if (g.gear != before) logger::Write("Rover flight: gear %d -> %d (driving)", before, g.gear);
			const auto gp = At<game::Vec3>(vehicle, off::kRefPosition);
			constexpr double kGroundShare[5] = { 1.0, 0.25, 0.5, 0.75, 1.0 };
			std::lock_guard lk(g_sh.lock);
			g_sh.px = gp.x; g_sh.py = gp.y; g_sh.pz = gp.z;
			g_sh.groundOn = true;
			g_sh.groundFrac = kGroundShare[std::clamp(g.gear, 0, 4)];
			g_sh.groundLearn = g.gear == 4 && wHeld && !shiftHeld;
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
				goal.stop = 1.5;
				goal.hold = true;
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
				if (TaxiMarker(player, vehicle, mid, mx, my)) {
					if (mid != g.taxiMarker || std::hypot(mx - g.taxiMx, my - g.taxiMy) > 2.0) {
						g.taxiMarker = mid;
						g.taxiMx = mx;
						g.taxiMy = my;
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
					if (!g.taxiArrived && g.taxiLeft <= kTaxiStop + 4.0) {
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
				// Return home / hold: once at the spot and nearly still, settle (this stands in for the navigation service's own settle-on-arrival).
				static double lx = 0, ly = 0;
				static ULONGLONG lt = 0;
				const ULONGLONG tNow = GetTickCount64();
				double speed = 99.0;
				if (lt && tNow > lt && tNow - lt < 1000) speed = std::hypot(rpos.x - lx, rpos.y - ly) / ((tNow - lt) / 1000.0);
				lx = rpos.x; ly = rpos.y; lt = tNow;
				if (pg.hold && pg.nav.landOnArrival && std::hypot(pg.x - rpos.x, pg.y - rpos.y) <= pg.stop + 2.0 && speed < 1.0) {
					g_remoteDone = vcontract::AssignedTo(g_roverId);
					g_remote = false;
					Stop("arrived; parked");
					return;
				}
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
		return PilotFly(pilot, vehicle, goal.x, goal.y, goal.stop, timeoutMs, keepGoing, goal.z);
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

	void RequestScannerTrip()
	{
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

	bool MarkerPoint(std::uintptr_t vehicle, double& x, double& y)
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !vehicle) return false;
		std::uint32_t id = 0;
		return TaxiMarker(player, vehicle, id, x, y);
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
		if (const auto player = Ptr(addr::kPlayerCharacterPtr)) {
			FinishPilotMoves(player);
			if (!PlayerVehicle(player)) WatchFlip(Rover(), "Rover");  // never while the player drives it
		}
	}

	void LoadReset()
	{
		TrailClear();
		// An in-game save load can keep the same player and rover addresses while
		// replacing their native bodies. Never carry thrust, contact or body
		// identity from the previous save into the new one.
		for (int i = 0; i < kPilots; ++i) {
			g_gs[i] = State{};
			g_driveOut[i].on.store(false);
			{
				std::lock_guard lk(g_shs[i].lock);
				g_shs[i].active = false;
				g_shs[i].haveLast = false;
				g_shs[i].collided = false;
				g_shs[i].havePR = false;
				g_shs[i].steps = 0;
				g_shs[i].guardVehicle = 0;
				g_shs[i].autopilot = false;
			}
			g_shs[i].vehicle.store(0);
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
		g_camRest = CamRest{};
		g_camPick = CamPick{};
		logger::Write("Rover flight: load reset discarded prior rover motion and body identity");
	}
}
