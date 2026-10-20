#pragma once

#include <vector>

// The game's own map points (outposts, landmarks, markers) of the planet the player is on. The star map / globe publishes its POI list (FUN_141684a10, BodyInspectState);
// this module watches that publish (a pass-through detour, nothing is changed) and keeps the lat/long of every shown + visible record, then converts them to world x/y
// with the engine's own world -> lat/long function. Used by the scanner marker's magnet pull (scantarget.cpp). Standalone (SeamlessVehicle without the core);
// with the core present the core owns the hook and this stays off.
namespace mappoints
{
	struct Point
	{
		float x = 0, y = 0;
		bool outpost = false;
	};

	// Installs the pass-through detour. False: not installed (the core owns the hook, or the bytes differ); the magnet then has nothing to pull to.
	bool Install();

	// The captured points of the planet the player is on, as world x/y, using the player's world position `playerPos` (float[3]) as the starting point of the conversion.
	// False: none known (the star map has not published the list for this planet yet). Main thread.
	bool WorldPoints(const float playerPos[3], std::vector<Point>& out);
}
