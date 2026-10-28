#pragma once

#include <cstdint>

// Planet ship flight, Phase 2 test (notes/plan-ship-flight-on-planets.md). Sitting in the pilot seat already takes
// control of the ship, landed or not (ChairEnterHandler -> FUN_1412aa9b0). On a planet the game opens "TakeoffMenu"
// instead of the flight HUD "SpaceshipHudMenu" (which carries flight input), and the ship counts as landed (actor
// value set by FUN_142117d00). The test swaps the menus and clears "landed", then logs what the ship does. Only the
// game's own menu and landed switches are used; the movement controller handle is never touched.
// The test showed mouse look arrives but the ship does not move on a planet, so 0.22.1 bridges the motion (hybrid):
// every frame our flight laws move the ship's position/heading and its 3D (scene-graph update that ignores physics);
// every second the game's full move routine brings cell, collision and physics in line (timed in the log). F7 or
// leaving the seat sets the ship down on the ground under it and restores the landed state.
namespace shipflight
{
	bool Active();
	void Start(std::uintptr_t ship);  // seated in the pilot seat of a landed ship
	void Stop(const char* why);
	void Tick();                      // per frame, main thread
	void Reset();                     // loading a save
}
