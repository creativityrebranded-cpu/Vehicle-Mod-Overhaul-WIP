#pragma once

#include <cstdint>

// Flight: the player becomes the ship (the user's design). Outdoors, F7 switches no-clip on (like the console's
// tcl: no gravity, no player collision) and the flight laws move the player each frame: W/S throttle, fly where
// you look, Space/Ctrl climb/descend, Shift boosts. The ground stays solid through the engine's own ray cast
// (elevation readout, hard floor at 2 m). Below 30 m and slow: F7 lands and brings the real ship next to you.
// Shift+F7 drops out of flight anywhere. Low and slow for 3 s lands by itself (with a countdown).
// F7 in the pilot seat of a landed ship launches: leave the seat, step out above the ship, fly. (The cloned ship
// model was retired, see notes/retired/shipmodel.cpp; the ship's look in flight waits on the space-flight recon.)
namespace flight
{
	// Per frame, main thread.
	// Hooks the mouse-look handler (ship mouse control). Call once at startup.
	bool Install();
	void Tick();
	// Flying, launching, landing or summoning the ship (no autosave then).
	bool Busy();
	// Sync point (game-update thread, every frame): the aimed landing point while F6 is held.
	void SyncTick(std::uintptr_t tes);
	// Around SaveGame: the ship beacon is never saved.
	void BeforeSave();
	void AfterSave();
	// Mouse for the rover hover (vehicleflight.cpp): while captured, the mouse-look counts are taken (zeroed for the
	// camera) and summed; TakeMouse returns and clears the sum.
	void CaptureMouse(bool on);
	void TakeMouse(double& x, double& y);
}
