#pragma once

// Keep vehicles in sight loaded (2026-10-24, the user: "just make the game render it, if it's far away it'll be a dot";
// approved keeping vehicle references loaded past the normal cell limit, "esp if you can tie it to sight").
// Vehicles normally unload with their cell (100 m cells, uGridsToLoad wide). When a vehicle near or past that edge is
// inside the camera's view cone, this marks it with the game's own Full LOD setter (FUN_140b36640: ref flag 0x10000,
// then the native persistent setter FUN_140b36c40), the same state a Creation Kit "Is Full LOD" reference has.
// Released again (the flags it set are cleared, nothing else) after 10 s out of view, or when it comes back inside
// the loaded area so the mod's cell scans find it again. Only the real vehicle models are drawn: no dots, no proxies.
// Logs\FlyingVehicleOverhaul\settings.ini [view]: keepInSight=1 (0 = off), keepCone=60 (half-angle, degrees), keepMax=48.
// kept-vehicles.txt in the same folder lists what is held, so a save made while held is cleaned up after the next load.
namespace vehiclekeep
{
	void Tick();   // main thread
	void Reset();  // a save load: forget held refs (the journal releases leftovers once the world settles)
}
