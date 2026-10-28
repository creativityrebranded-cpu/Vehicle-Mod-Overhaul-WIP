#pragma once
#include "groundplan.h"
#include <cstdint>

// The ground-first route service (2026-10-25; groundplan.h is the pure planner). Main thread only, except the search,
// which runs on one owned worker that is never waited for.
//  - Senses: downward terrain rays (environment::GroundAt) along the route ahead, the straight line to the goal and
//    around the vehicle: ground height, slope (surface normal) and water, kept in a terrain memory (and fed to the nav map).
//  - Maps: a 141 x 141 grid of 4 m cells (564 m) leaning toward the goal, from that memory, the nav map's remembered
//    ground and hits, and the game's collision object boxes (navseed). A hillside hit at the ground is not a wall.
//  - Plans: groundplan::Search per pilot, ground preferred, air only where driving cannot go (and only for vehicles that fly).
//  - Replans: new goal, old plan, off the route, blocked ahead (the map changed), a partial route nearly used up, stuck.
// Switched by the setting "Ground planner and native driving" (features::Id::NativeGround, [nav] groundPlanner, off).
// Log prefix "Ground plan:" / "Ground sense:".
namespace groundnav
{
	struct Request
	{
		std::uint32_t vehicle = 0;
		double x = 0, y = 0, z = 0, speed = 0;
		bool airborne = false;       // flying well above the ground now
		double gx = 0, gy = 0, gz = 0;  // gz NaN: any height
		double stop = 3;
		bool stopAtEnd = true;       // arrive and stop (false: pass through, keep going)
		bool canFly = true;
		double halfWidth = 1.4, halfLength = 2.6, height = 2.0;
		double topSpeed = 30, speedLimit = 0;  // speedLimit 0: none
		double latAccel = 4.0;
	};
	struct Status
	{
		bool ok = false;             // a current plan guides this vehicle
		groundplan::Guidance guide;
		bool reachesGoal = false, endsOnGround = true;
		double groundMetres = 0, airMetres = 0;
		unsigned modeChanges = 0, route = 0;
	};
	bool Enabled();
	// Main thread, each frame a pilot drives or flies to a goal by the planner. Never blocks: the first frames of a new
	// goal return ok = false until the first search finishes.
	Status Guide(int pilot, const Request& rq);
	void Invalidate(int pilot, const char* why);  // plan again soon (stuck, recovery)
	void Forget(int pilot);                       // the pilot stopped
	// Roads (optional): a query telling whether (x, y) lies on a road or paved lane (the road traffic's baked network).
	void SetRoadQuery(bool (*onRoad)(double x, double y));
	void Service();  // main thread, every frame
	void Reset();    // a save load
}
