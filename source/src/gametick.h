#pragma once

// A per-frame moment inside the game's own update (main thread, gameplay phase), where gameplay code may query
// physics and touch the player's 3D: the ship system update FUN_142169100, which the main loop runs every frame
// (0x1417c808f). The grid-update hook (prefetch) only sees the player on cell changes and loads, so per-frame work
// that needs a safe context (ground probes, the flight ship model) runs here.
namespace gametick
{
	bool Install();
}
