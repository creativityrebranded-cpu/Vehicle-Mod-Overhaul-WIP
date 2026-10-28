#pragma once

#include <cstdint>

// Zones as landings, across the whole planet (0.51.0).
//  - The planet's own landing-site registry (*(mgr+0x140): entries {lat, lon, worldspace form id}, each with a cell
//    footprint) tells whether a point lies inside a hand-built site (Akila City, other cities and fixed sites).
//  - Open terrain on one cube face: the in-place re-centre with the landing following the player (recentre.cpp).
//  - Entering or leaving a hand-built site, or crossing onto another cube face: the game's own surface landing at the
//    player's latitude/longitude (FUN_1417DEC70, as fast travel and the console landing do); the planet setup picks
//    the site's worldspace itself. The first versions show the game's loading screen.
namespace landing
{
	// Main thread, every frame (frame task): watches the player's position against the site footprints and runs
	// pending landings (never inside the game update's sync point).
	void Tick();
	// The re-centre found the player on another cube face: land at the player's position instead of shifting.
	void RequestLandHere(const char* why);
	// The authored site (worldspace form id) whose footprint contains this lat/long on the current planet, or 0.
	std::uint32_t SiteAt(const double latlong[2]);
}
