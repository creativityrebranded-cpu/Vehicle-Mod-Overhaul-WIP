#pragma once

#include <cstdint>

// Predictive 3D lane planner (2026-10-17; the user: "I want the systems to not collide ... predict what might cause that
// to pick lanes and paths without conflicts, specifically to pick their 3D routes").
// Every frame each autopilot vehicle asks for a plan: its path over the next kHorizon seconds (at its intended speed
// toward the goal) is checked against the predicted path of every other vehicle in world::Vehicles (parked ones are
// fixed obstacles, the player's manual vehicle and other autopilots move). On a predicted conflict the vehicle with the
// lower right of way yields, trying in order:
//   1. an altitude lane (+8 / +16 / +24 m over its cruise height), 2. a side offset (keep right first, then left),
//   3. both together, 4. a speed cap (stop short of the conflict).
// Right of way: the player's manual vehicle > autopilots by priority (the player's own rover 120, NPC jobs 60) > ties
// by form id (lower passes); parked vehicles never move, so everything yields to them. A lane or side offset is held
// at least kHoldMs and released after the plain path has been clear for kHoldMs. Head-on traffic both keeps right.
namespace routeplan
{
	struct Query
	{
		std::uint32_t ignoreId=0; // only an explicitly ordered ram target
		std::uint32_t selfId = 0;  // the vehicle's form id
		int priority = 60;         // higher passes first
		float pos[3] = {};         // current position
		float dir[2] = {};         // unit horizontal direction toward the goal
		float speed = 0;           // intended horizontal speed (m/s)
		float half[3] = { 1.25f, 2.3f, 1.0f };
		bool ground = false;       // ground driving: no altitude lanes
		float distToGoal = 1e9f;
	};
	struct Plan
	{
		float laneZ = 0;       // metres added to the cruise height
		float yawOffset = 0;   // radians added to the bearing (+ right)
		float speedCap = -1;   // m/s; < 0 none
		float conflictT = -1;  // seconds to the first predicted conflict of the CURRENT plan (< 0 none)
		std::uint32_t other = 0;
	};
	void Frame();                          // main thread, once per frame after world::Publish: refreshes the snapshot
	Plan Update(int pilot, const Query& q);  // pilot 0..6
	void Forget(int pilot);                // the pilot stopped
	void Reset();                          // a save load
}
