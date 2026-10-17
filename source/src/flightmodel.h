#pragma once
// Flight model for SeamlessPlanets: pure math, no engine calls, builds and runs on any OS.
//
// Frame: x east, y north, z up (metres). yaw 0 = +y, positive yaw turns right (towards +x).
// pitch: positive = nose up. roll: positive = right wing down.
// forward = (sin(yaw) cos(pitch), cos(yaw) cos(pitch), sin(pitch)).
// (The game's player rotation has x = pitch with POSITIVE = looking DOWN and z = yaw; convert at the boundary.)
//
// Controls (user's design, all three axes usable at the same time):
//   mouse Y  -> pitch (nose up/down), mouse X -> roll (bank). A held bank keeps turning the ship (no auto-level).
//   A / D    -> yaw (rudder) left / right, with momentum.
//   W / S    -> forward speed up / slow down (throttle lever that stays where you leave it).
//   Climb and descent come from the nose direction; there are no climb buttons.
// The mouse feeds a "virtual stick" (integrates mouse counts, springs back to centre) so every axis has momentum.
// A hidden controller (terrain assist) adds pitch commands through the same path when the ship is on course to hit
// terrain, based on time to impact and how long a pull-up takes.

#include <vector>

namespace fm
{
	struct Params
	{
		// Speed
		double vMax = 100.0;           // m/s absolute top speed
		double accel = 30.0;           // m/s^2 limit on the thrust acceleration
		double thrustGain = 0.5;       // 1/s: thrust acceleration per m/s of speed error (so climbing visibly costs speed)
		double throttleRate = 0.5;     // throttle per second while W / S held
		// Simple speed (user, 2026-10-14: "I liked the original method of speeding and slowing"): the speed eases to
		// throttle x vMax at `accel`, and nothing else changes it (no climb cost, no turn loss, no reach cap).
		bool simpleSpeed = true;
		double gravityAlong = 8.0;     // m/s^2 * sin(pitch): climbing costs speed, diving gains it
		double turnBleed = 8.0;        // m/s^2 * (n^2 - 1), n = 1 / cos(roll): hard banked turns cost speed
		// Stall
		double vStall = 25.0;          // below this (and not near the ground) the ship stalls
		double stallSink = 25.0;       // m/s^2 downward at full stall
		double terminalSink = 45.0;    // m/s
		double stallNoseDown = 0.6;    // rad/s nose drop at full stall
		double stallRecoveryRate = 0.4; // rad/s: after a stall, with no pitch input, the nose eases back to level
		double stallRecoveryTime = 4.0; // s the recovery lasts
		double stallNoseDownLimit = 0.6; // rad: a stall drops the nose to at most this far below level
		double landingSpeed = 30.0;    // below this speed and landingHeight the terrain assist stands down (the hard floor still holds)
		double landingHeight = 15.0;   // m
		double nearGround = 12.0;      // m: below this stall sink is suppressed and sink is capped (so landing is possible)
		double nearGroundSink = 3.0;   // m/s
		// Attitude rates (rad/s) and momentum time constants (s)
		double pitchRateMax = 0.9;
		double rollRateMax = 1.8;
		double yawRateMax = 0.8;
		double turnGain = 1.0;         // rad/s of yaw per sin(roll)
		double tauPitch = 0.25, tauRoll = 0.25, tauYaw = 0.45;
		double tauAlign = 0.5;         // the flight path chases the nose with this lag
		double maxPitch = 1.40;        // rad (80 deg)
		double maxRoll = 1.48;         // rad (85 deg)
		double vAuth = 40.0;           // speed at which control authority is full
		double authLow = 0.35;         // authority at zero speed
		double autoLevelRate = 0.0;    // rad/s: roll returns to level with no input (0 = roll is held, as requested)
		// Virtual stick
		double mouseGain = 0.0025;     // stick units per mouse count
		double stickTau = 0.35;        // s: spring back to centre
		bool invertPitch = false;      // false: mouse up = nose up
		// Easy scheme (user, 2026-10-13: "only rotations is mouse"): mouse X turns the nose directly (yaw) and the
		// hull banks into the turn for looks only; A / D slide sideways, Space / Ctrl move straight up / down.
		bool mouseYaw = true;          // false: the handoff's scheme (mouse X = roll, the bank turns the ship)
		double bankPerYawRate = 0.6;   // rad of cosmetic bank per rad/s of yaw rate (easy scheme)
		double easyYawRateMax = 1.4;   // rad/s at full mouse (easy scheme: the mouse steers, so it must be quick)
		double easyTauYaw = 0.18;      // s: yaw momentum in the easy scheme (short, so the nose stops where you stop)
		// Mouse rate control (user, 2026-10-14): moving the mouse is the turn and its speed the strength; small jitters are
		// ignored (hysteresis: turning starts above mouseOn and stops below mouseOff, counts per second); with the mouse
		// still the ship stabilises (the turn stops, the nose eases back to level).
		bool mouseRateControl = true;
		double yawPerCount = 0.0025;   // rad/s of yaw per count/s of mouse speed
		double pitchPerCount = 0.0018; // rad/s of pitch per count/s
		double mouseOn = 60.0;         // counts/s to start turning
		double mouseOff = 25.0;        // counts/s below which the turn stops
		double mouseFilterTau = 0.06;  // s: mouse speed smoothing
		double levelRate = 1.2;        // 1/s: the nose eases back to level while the mouse is still
		double strafeSpeed = 30.0;     // m/s sideways at full A / D
		double liftSpeed = 30.0;       // m/s up / down at full Space / Ctrl
		double tauThrust = 0.4;        // s: strafe and lift build up and coast with this lag
		bool stallEnabled = false;     // vertical thrust holds the ship up, so no stall in the easy scheme
		// Height keeping (user: the assist should move the ship away from terrain and keep a far minimum distance):
		// the ship holds minClearance above the highest ground under the hull and ahead, using lift (not only the
		// nose), slowing down when even full lift cannot clear what is ahead. Off while landing. 0 = off.
		double minClearance = 25.0;    // m above the ground (the hull floor `clearance` still applies below this)
		double floorGain = 1.2;        // 1/s: climb rate per metre below the minimum
		double assistLiftMax = 45.0;   // m/s: the most the height keeping lifts
		double tauAssistLift = 0.2;    // s
		double landingSink = 8.0;      // m/s: descent while landing (Input::landing)
		// Predictive braking (user: the assist must never let it collide, and may stop harshly): along the actual
		// direction of travel, the nearest point that the height keeping cannot clear in time (or where collision is not
		// loaded yet) is an obstacle; the speed is held to what can stop before it at brakeDecel. Diving at the ground
		// is braked the same way.
		double brakeDecel = 60.0;      // m/s^2: the hardest stop
		double brakeMargin = 25.0;     // m: kept between the stopped ship (nose) and the obstacle
		double brakeHeightMargin = 10.0; // m above ground + clearance the ship must pass over (else: brake / climb)
		// Vehicle hover (vehicleflight.cpp): the rover holds a height above the ground with its (infinite) boost.
		double hoverGain = 1.5;        // 1/s: vertical speed per metre away from the hold height
		double hoverMaxUp = 15.0;      // m/s
		double hoverMaxDown = 8.0;     // m/s
		double boostMultiplier = 1.0;  // scales the hover's strength (gain and limits)
		double levelGain = 4.0;        // 1/s: the rover is turned back upright (pitch and roll) this fast
		double holdRate = 6.0;         // m/s: Space / Ctrl move the hold height
		// Hull and terrain
		double hullHalfLen = 20.0;     // m: nose / tail sample distance
		double hullHalfSpan = 15.0;    // m: wingtip sample distance
		double clearance = 6.0;        // m: hull half-height plus margin
		double lookTimes[5] = { 0.5, 1.0, 2.0, 3.0, 4.0 };  // s: look-ahead samples along the path
		double warnTTC = 4.0;          // s: warning (notification) when a collision is predicted within this
		double assistGain = 3.0;       // 1/s: pitch rate per radian of missing nose angle
		double assistMargin = 0.12;    // rad added to the required nose angle
		double reachTime = 2.0;        // s of known ground wanted ahead; unknown ground beyond limits speed
		double minSpeedCap = 30.0;     // m/s: the reach cap never goes below this
	};

	struct Input
	{
		double mouseDx = 0, mouseDy = 0;  // counts this frame (right / up are positive for dx / dy)
		double rudder = 0;                // -1 .. +1: yaw left / right (handoff scheme only)
		double strafe = 0;                // -1 (A) .. +1 (D): slide left / right, nose unchanged
		double lift = 0;                  // -1 (Ctrl) .. +1 (Space): straight down / up, nose unchanged
		bool faster = false;              // W
		bool slower = false;              // S
		bool landing = false;             // landing: height keeping off, the ship settles straight down
	};

	// A ground height request (x, y). Answered one frame late by the engine's ray cast.
	struct Request
	{
		double x = 0, y = 0;
		int kind = 0;       // 0 hull point, 2 hull centre, 1 look-ahead along the path
		double dist = 0;    // planned horizontal distance for look-ahead samples
	};
	struct Result
	{
		bool valid = false;  // false: no collision loaded there (no data)
		double h = 0;        // ground height, top surface
	};

	enum class Mode { Cruise, Warning, Assist, Scraping, Stall, Grounded };

	struct State
	{
		double x = 0, y = 0, z = 100.0;
		double vx = 0, vy = 0, vz = 0;
		double yaw = 0, pitch = 0, roll = 0;
		double yawRate = 0, pitchRate = 0, rollRate = 0;
		double stickX = 0, stickY = 0;
		double throttle = 0.5, speed = 50.0;
		double fx = 0, fy = 1, fz = 0;   // flight path direction (unit)
		double sink = 0;                 // stall sink, m/s downward
		double vStrafe = 0, vLift = 0;   // sideways (right +) and vertical (up +) thrust velocity, m/s
		double assistLift = 0;           // m/s of lift added by the height keeping
		double obstacle = -1;            // m to the nearest obstacle the ship must stop before (-1 none)
		double mouseSX = 0, mouseSY = 0; // filtered mouse speed, counts/s
		bool mouseActive = false;        // hysteresis state
		bool braking = false;            // predictive braking is holding the speed down
		bool stalled = false;
		double recovering = 0;           // s left of the post-stall nose recovery
		Mode mode = Mode::Cruise;
		double ttc = 1e9;                // predicted time to impact, s (1e9 = none)
		double elevation = -1;           // m above the ground under the hull centre, -1 unknown
		double speedCap = 1e9;           // m/s, from unknown ground ahead
		int clampEvents = 0;             // hard-floor corrections so far
		double maxClampLift = 0;         // largest single correction, m
	};

	class Model
	{
	public:
		Params p;
		State s;

		Model();
		void Reset(double x, double y, double z, double yaw, double speed);

		// Ground samples the engine should take for the next frame (order matters for Feed).
		const std::vector<Request>& Requests() const { return m_req; }
		// Results for an earlier request vector (they are one or two frames old when they arrive).
		void Feed(const std::vector<Request>& reqs, const std::vector<Result>& res);

		void Step(double dt, const Input& in);

	private:
		std::vector<Request> m_req;
		std::vector<Request> m_lastReq;
		std::vector<Result> m_lastRes;
		void PlanRequests(double dt);
	};

	const char* ModeName(Mode m);
}
