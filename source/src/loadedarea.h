#pragma once

#include <cstdint>

// FVO's pass through the game's loaded-area wall (2026-10-28, the user chose "FVO-only pass").
// Starfield.exe 1.16.244 keeps a LoadedAreaBound (Havok, setting bLoadedAreaBoundEnabled:HAVOK): six wall bodies round
// the attached cells' bounds, 30 m below them to 1500 m above, rebuilt whenever the player's centre cell changes
// (FUN_140D50650, called each frame from FUN_140578350 at 0x140578544). The walls are SOLID (20:11 run: two traffic
// vehicles stopped dead on the box side, y = 1300, with a 50 m/s depenetration push). On top of that, its Havok
// handlers record bodies touching a wall (+0x68 count / +0x70 pairs: moving body, wall body) and bodies leaving the
// broad-phase (+0x80 set); next frame a side-wall touch gets speed x -0.3 and spin zeroed, and FUN_140D500B0 "moves
// back" a body (a character via Actor::SetPosition: what pulled drivers out of their seats). The same wall is the
// game's fell-through-the-world rescue and its projectile edge cull, so it stays.
// With [physics] fvoWallPass=1 (default), for vehicles FVO is flying and their drivers only:
//  - collision: bhkCollisionFilter's pair filter (vtable 0x144D64948 slot 3, FUN_142399100(filter, world, pairs, n))
//    also drops pairs of a SIDE wall (index 2..5) and one of their bodies, so they fly through. Floor and ceiling
//    still collide. Runs on physics threads: it only compares body ids published by the main thread.
//  - the game's touch list loses those side-wall touches, and the three FUN_140D500B0 calls skip those bodies.
//  - FVO's own sensor rays ignore all six wall bodies (IsWall).
// The bodies: every 250 ms the main thread takes each FVO vehicle's chassis body (its VehicleRuntimeComponent's
// instance +0x20) and every body within 12 m of it whose owner (body +0x98 -> vtable +0x138 -> +0x10 (3D) ->
// FUN_140B3CF40, no reference count taken) is that vehicle or its driver.
// Also traces why traffic drivers vanished (20:12:57 run): logs any SetDelete(true) on a watched driver with its
// callers, and the first frame a watched driver's form id no longer resolves.
namespace loadedarea
{
	bool Install();  // startup
	void Tick();     // main thread, every frame
	bool IsWall(std::uint32_t bodyId);  // one of the six wall bodies of the current box (any thread)
	// Trace only: the reference that owns a ray-hit body, resolved on the main thread and cached; 0 until resolved (any thread).
	std::uint32_t HitBodyOwner(std::uint32_t bodyId);
	// RT-1 (2026-11-18): the same cache, saying whether the walk has answered yet (owner 0 after an answer: no reference owns the body).
	bool HitBodyOwnerKnown(std::uint32_t bodyId, std::uint32_t& owner);
	// Main thread: something moved an FVO-flown vehicle by (dx, dy, dz) without its driver (the game's vehicle build drops a
	// vehicle onto ground within ~45 m): its job's driver, if seated in it, is moved by the same amount. True when moved.
	bool CarryDriverOf(std::uint32_t vehicleId, float dx, float dy, float dz);

	// Cross-cell relocation (2026-11-20, notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md). Main thread only. These add no engine routine: the move
	// of the chassis reference into the destination cell is the existing service path (Refile: FUN_140B48CA0 after the position setter, exactly what
	// SyncOne queues for any flown vehicle that crosses a cell edge). The seated driver follows by the same direct write as every flown driver
	// (CarryDriver); a seated actor is never refiled (the game's actor mover unseats it: 2026-10-30, 16:55 run).
	struct RelocationCell {
		bool known = false;        // the vehicle reference resolved and is listed in an exterior grid cell
		bool crossCell = false;    // (x, y) is in another grid cell than the one it is listed in
		bool cellLoaded = false;   // the cell at (x, y) exists now (FUN_140595DF0 without create)
        bool cellAttached = false; // the cell actually reports the same attachment state used by the world inventory
		int fromX = 0, fromY = 0, toX = 0, toY = 0;
	};
	bool RelocationDestination(std::uint32_t vehicleId, float x, float y, RelocationCell& out);
	// Queue the existing cell move of the chassis reference to (x, y, z) and carry its driver. False: nothing queued (pose owner still holds the
	// vehicle, no reference, or already queued). Call after the relocation pose owner released the vehicle.
	bool RefileRelocated(std::uint32_t vehicleId, std::uint32_t driverId, float x, float y, float z);
	// Fleet rail outside the attached cells (2026-11-21): queue a move of the chassis reference to (x, y, z) even inside its listed
	// cell, refiling it when the point is in another existing cell, and carry its seated driver. Same service path as RefileRelocated.
	bool StepReference(std::uint32_t vehicleId, std::uint32_t driverId, float x, float y, float z);
	struct RelocationSettle {
		bool refKnown = false, listedGrid = false, inDestinationCell = false, atTarget = false;
		float gap = 0;             // m between the chassis reference and (x, y, z)
	};
	// Where the chassis reference stands now against the destination: settled when it is at the target and listed in the destination's cell.
	bool RelocationSettled(std::uint32_t vehicleId, float x, float y, float z, RelocationSettle& out);
}
