#pragma once

#include <limits>

#include <cstdint>

// Planetary flight on the REV-8 rover (user, 2026-10-14; design: notes/plan-vehicle-flight.md). While driving, H
// toggles hover: the rover holds a height above the ground with an infinite boost and flies with the flight model's
// controls (W/S speed lever, mouse yaw with hysteresis, A/D slide, Space/Ctrl move the hold height), with the
// predictive terrain assist. Applied as velocity in the rover's own physics step (FUN_1423cb260): Havok still resolves
// every collision. The rover is kept upright (yaw only).
namespace vehicleflight
{
	bool Install();  // hooks the vehicle physics step
	void Tick();     // per frame, main thread (frame task)
	void LoadReset(); // discard hover/body state before a different save replaces the rover
	bool Active();

	// NPC pilots 1..3 (the "pursuit" side; pilot 0 is the player's escort autopilot): the same flight model, terrain
	// assist and avoidance, flying `vehicle` to (x, y) and settling `stop` metres short. Called again while flying it
	// only changes the target. keepGoing: arriving does not end the flight (it holds `stop` metres off; no timeout).
	// z: the world height to fly at (NaN: skim the terrain); never under the terrain clearance.
	// False: not allowed (the player's rover, or another pilot has this vehicle).
	bool PilotFly(int pilot, std::uintptr_t vehicle, double x, double y, double stop, unsigned timeoutMs, bool keepGoing = false,
		double z = std::numeric_limits<double>::quiet_NaN());
	bool PilotActive(int pilot);
	const char* PilotEndReason(int pilot);  // why the pilot last stopped ("arrived", "stuck", ...)
	void PilotStop(int pilot, const char* why);
	std::uint32_t OwnRoverId();  // the player's own vehicle (form id), never flown by an NPC
	// The player's view heading (world yaw, atan2(x, y)); false when unreadable. For a passenger steering an NPC pilot.
	bool ViewHeading(double& yaw);
	// The player's custom map marker as a point in `vehicle`'s world (false: none, not loaded, another worldspace, too far).
	bool MarkerPoint(std::uintptr_t vehicle, double& x, double& y);

	// Direct control of an autopilot (pilot 1..3, or 0 = the player's rover while it flies itself): every channel in
	// `set` overrides the autopilot's own for holdMs, then hands back. Channels left out stay with the autopilot.
	struct Command
	{
		enum : unsigned { kThrottle = 1, kStrafe = 2, kVertical = 4, kYaw = 8, kPitch = 16, kRoll = 32, kBrake = 64, kGear = 128 };
		unsigned set = 0;
		double throttle = 0;   // -1 full reverse .. 1 full forward (the gear's thrust)
		double strafe = 0;     // -1 slide left .. 1 slide right
		double vertical = 0;   // m/s up (+) / down (-); replaces the height hold
		double yawRate = 0;    // rad/s, + turns right
		double pitch = 0;      // rad, nose up + (attitude wanted)
		double roll = 0;       // rad, right side down + (attitude wanted)
		bool brake = false;
		int gear = 0;          // 1-4
		unsigned holdMs = 300;
	};
	bool PilotCommand(int pilot, const Command& c);  // false: that pilot is not on autopilot

	// Flipped-vehicle protocol: call every frame for a vehicle we look after (the player's own rover, NPC job
	// vehicles). Tipped past ~65 degrees and slow for 2 s: its hover (if any) ends and it is set upright 1.5 m higher
	// in place, heading kept.
	void WatchFlip(std::uintptr_t vehicle, const char* who);
}
