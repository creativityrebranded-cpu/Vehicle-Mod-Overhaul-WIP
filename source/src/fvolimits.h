#pragma once
// One size for every vehicle limit in FVO and FVO Traffic (2026-10-29, the user: "match all sizes; doesn't make sense to have them
// disjoint params"). Everything that counts vehicles derives from this: the vehicle list (world::kMaxVehicles), NPC pilot slots
// (npcvehicles kJobs, pilotlayout::kSlots, FVO_NPCPilots.esm: scripts/make_pilots_esm.py PILOTS must equal it), probe / ray / sweep
// slots (prefetch), planners (routeplan, groundnav, autodrive neighbours), traffic trip slots, the traffic pool, route vehicle counts.
// How many are actually active is decided by cost (vehicleworld's sampling budget, vehicleflight's planning tiers), not by this.
namespace fvolimits
{
	inline constexpr int kVehicles = 256;
}
