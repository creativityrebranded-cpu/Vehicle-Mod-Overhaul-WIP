#pragma once
// The car's control channels and the per-update reading of the car (removal 2026-11-14). Types only, no decision: the energy plan
// (planenergy.h, step 7 in vehicleflight.cpp) is the one decider and writes Output; the actuators in Fly() read it.
// Moved out of the old autopilot (autodrive.h, removed 2026-11-14, kept outside FVO in fvo-removed-20261114/deciders).
#include <cmath>
#include <cstdint>
#include <limits>

namespace drive
{
	constexpr double kPi = 3.14159265358979323846;

	enum class Mode : std::uint8_t { Drive, Hover };
	enum class Permit : std::uint8_t { Both, DriveOnly, HoverOnly };
	enum class Phase : std::uint8_t { Travel, Arrived, Yield, Reverse, Turn, Climb, Boxed };

	inline const char* Name(Mode m) { return m == Mode::Drive ? "drive" : "hover"; }
	inline const char* Name(Phase p)
	{
		switch (p) {
		case Phase::Travel: return "travel";
		case Phase::Arrived: return "arrived";
		case Phase::Yield: return "yield";
		case Phase::Reverse: return "reverse";
		case Phase::Turn: return "turning out";
		case Phase::Climb: return "climb";
		default: return "boxed";
		}
	}

	struct Shape
	{
		double halfWidth = 1.25, halfLength = 2.3, height = 2.0;
		double originAbove = 0.8;  // the reference's height over the ground it stands on
		double steerLock = 0, wheelbase = 0, slopePoint = .8, initialSlope = .7, fullSteerSpeed = 45;  // measured car (carmodel.h); 0 = unknown
	};

	struct Limits
	{
		double driveTop = 20.0, hoverTop = 26.0;  // m/s
		double brake = 6.0;                       // m/s^2 the vehicle can count on
		double climbRate = 5.0;                   // m/s the hover can climb
		double hoverClear = 2.5;                  // m kept over the surface ahead when hovering
		double maxSlope = 0.62;                   // rise/run the wheels take (about 32 degrees)
		double maxStep = 1.1;                     // m a single 2 m step may rise when driving
		double maxDrop = 1.5;                     // m a step may fall when driving
		double unknownDrive = 9.0, unknownHover = 12.0;  // m/s over ground nobody has seen
	};

	// Another vehicle near this one (World's vehicles, for the plan's movers and the logs).
	struct Other
	{
		std::uint32_t id = 0;
		double x = 0, y = 0, z = 0, vx = 0, vy = 0, radius = 2.5;
		int priority = 0;
		double halfWidth = 0;
		double heading = 0, halfLength = 0; bool parked = false;
	};

	// This update's reading of the car and its goal (built in AutodriveDecide from the game and the shared map).
	struct Input
	{
		double now = 0;                 // s (monotonic)
		double x = 0, y = 0, z = 0, heading = 0, vx = 0, vy = 0;
		double groundUnder = 0;         // world height of the ground under the vehicle
		bool groundOk = false;
		double above = 0;               // m over that ground
		double gx = 0, gy = 0, gz = std::numeric_limits<double>::quiet_NaN(), stop = 3.0;
		bool keepGoing = false, hold = false, cruise = false;
		double cruiseSpeed = 0;
		double targetVx = 0, targetVy = 0;  // a followed target's velocity
		Permit permit = Permit::Both;
		double speedLimit = 0;          // 0: none
		bool waterAhead = false;
		bool hoverPossible = true;      // false under 30 % health (the hover fails)
		double ceiling = 1e9;           // m free above the vehicle's top; 1e9: open sky
		bool preferAir = false;         // the moment prefers the air (a fight, fleeing, a long summon); the energy plan prices it
		const char* preferWhy = "";
		bool onWheels = true;           // on the game's own wheels
		bool contact = false;           // a side contact this frame
		bool strictTraffic = false;
		bool driverSeated = true;
		bool paused = false;
		std::uint32_t self = 0;
		int priority = 60;
		const Other* others = nullptr;
		int otherCount = 0;
		Shape shape;
		Limits limits;
	};

	// The control channels the actuators read (the energy plan's decision this update).
	struct Output
	{
		Mode mode = Mode::Drive;
		Phase phase = Phase::Travel;
		double bearing = 0;     // world heading to steer to
		double speed = 0;       // m/s wanted along it
		bool brake = false;
		bool reverse = false;   // back up (the nose turns toward `bearing` + pi)
		double climb = 0;       // m added to the hover height
		bool modeChanged = false;
		bool boost = false;     // BO-1: Speed's request for the game's forward boost (thrust-limited on wheels); the actuator only executes it
		const char* why = "";   // the reason behind this update's decision
	};
}
