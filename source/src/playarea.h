#pragma once

// The planet's "playable area" follows the player. All planet content (the per-cell spawner FUN_1418aa1c0 that
// adds creatures/NPCs as cells load, the per-block placement FUN_1418ad2b0 and the landing block FUN_1418abd60)
// is gated by FUN_1403a5c20(latlong, planet, origin, buffer): a square of min(8 blocks, playable area) cells
// around the planet manager's origin (mgr+0xC8, the landing site), shrunk by a buffer. With the enlarged physics
// world the origin never moves, so content only ever existed within ~4 km of the landing.
//
// The hook keeps the engine's answer, and only when it says "outside" for a content-manager caller testing against
// the manager origin, repeats the same square test centred on the player's cell instead. Nothing is moved.
namespace playarea
{
	bool Install();

	// Per frame, main thread: refreshes the player's cell offset from the origin (used by the hook).
	void Tick();
}
