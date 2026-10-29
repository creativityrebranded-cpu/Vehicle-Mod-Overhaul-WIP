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
}
