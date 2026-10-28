#pragma once
#include <cstdint>

// Route markers (2026-10-28, the user: "I would like the probes appearing for whatever route is there but for the routes let
// them be defined via the files directly"). The traffic addon hands core the points of every route, exit and place of the
// player's world (FVO_SetRouteMarkers, fvo_addon_api.h); while "Show traffic routes" is on, an outpost beacon (the probe
// the old ring editor used) stands at each point. The beacons are made once per world and reused: their ids are kept in
// Logs\FlyingVehicleOverhaul\routeprobes.ini, so loading never adds more to the save. Off, they are all disabled.
namespace routeprobes
{
	struct Marker { float x, y, z, scale; };
	// The markers for `world` (the worldspace editor name). Any thread; core applies them on the main thread.
	void Set(const char* world, const Marker* markers, int count);
	void Tick();   // main thread, every frame
	void Reset();  // a save load
}
