#pragma once

#include <cstdint>

// What the player's surroundings are anchored to: the player outdoors, or the ship they are inside (the engine
// streams the planet around the ship then, FUN_1417df210). Plus "is the world around it fully loaded".
namespace anchor
{
	struct Anchor
	{
		std::uintptr_t ref = 0;   // null when there is none (indoors, in space)
		std::uintptr_t cell = 0;  // the anchor's exterior cell
		bool inShip = false;
	};
	Anchor Current();

	// The reference is standing/sitting in a ship's interior cell.
	bool InShipInterior(std::uintptr_t ref);

	// The ship a reference is inside (engine FUN_142116840), or 0.
	std::uintptr_t ShipOf(std::uintptr_t ref);

	// Outdoors (or in a ship that is outdoors), planet manager idle, the anchor's cell and every loaded grid cell
	// fully attached.
	bool WorldSettled();
	// Native uGridsToDemand inner ring is attached around the anchor. The outer
	// uGridsToLoad ring may remain in the lightweight state 6 by design.
	bool DemandReady();
}
