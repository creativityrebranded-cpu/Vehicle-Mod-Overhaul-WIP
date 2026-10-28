#pragma once
#include "ringlogic.h"

// City ring road beacons: REMOVED 2026-10-25 (the user: "the probe part of the mod for the ring thing, remove it").
// The ring editor used to show the ring as outpost beacons in the sky and reshape it with keys. Now this only cleans up:
// after each load settles, every beacon listed in Logs\FlyingVehicleOverhaul\ringroad.ini ("beacons=") that still
// exists as an outpost beacon is disabled, in every world, so older saves lose them too. The ring's shape ("ring=")
// stays in ringroad.ini: scheduled traffic flies it. Old editor source: notes/ring-beacons-removed-20261025/before/.
namespace ringroad
{
	void Tick();   // main thread, every frame
	void Reset();  // a save load: clean up again once the world settles
}
