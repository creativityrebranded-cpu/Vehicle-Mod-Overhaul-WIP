#pragma once

#include <cstdint>

// Prefetch (streaming source): the engine centres the loaded cell grid on the player every frame
// (FUN_140576530(TES, &player->position, 1, 0, 0) from the player update). While the player moves fast, this
// leads that centre ahead along their velocity, up to (grid radius - 1) cells so the player's own cell always
// stays loaded. Cells, objects and collision ahead therefore load earlier. Bethesda's own fly-mount path
// preloader (FUN_141889b10) builds a list of path cells but never requests them in the release build.
namespace prefetch
{
	bool Install();
	// Settle pause (recentre.cpp): used only while a normal re-centre waits at low speed, so forward loading remains
	// active during fast travel and the grid can finish attaching when the player slows.
	void SuppressLead(bool on);

	// Ground probes (the engine's vertical ray cast) must run while the game itself is updating the player,
	// not from the plugin's own task (Havok may be mid-step then: that crashed). Callers queue a probe in a
	// slot; it runs inside the player's per-frame update and the latest result is read back next frame.
	// kSlotModel0..: the flight model's hull and look-ahead samples (flightmodel.h), kSlotModelCount of them.
	enum Slot { kSlotFlight = 0, kSlotGuard = 1, kSlotShip = 2, kSlotModel0 = 3, kSlotModelCount = 12, kSlotTrace = kSlotModel0 + kSlotModelCount,
		kSlotDiag0 = kSlotTrace + 1, kSlotDiagCount = 4,
		// SeamlessCheck: ground ring around the player after each shift (9 near + 16 far).
		kSlotCheck0 = kSlotDiag0 + kSlotDiagCount, kSlotCheckCount = 25,
		// Core: buried-player rescue (floor below / surface above).
		kSlotRescue0 = kSlotCheck0 + kSlotCheckCount,
		// SeamlessVehicle: NPC pilots 1..3, 16 each (7 model samples, then 9 avoidance corridors).
		kSlotPilot0 = kSlotRescue0 + 2, kSlotPilotEach = 16, kSlotPilotCount = 3,
		kSlotCount = kSlotPilot0 + kSlotPilotEach * kSlotPilotCount };

	// Also service probes when the engine updates the loaded grid around this reference (the flown ship).
	void SetProbeAnchor(std::uintptr_t ref);

	// Run queued probes now (called from the per-frame game tick, a safe gameplay context).
	void ServiceProbesNow(std::uintptr_t tes);
	void RequestGround(int slot, float x, float y, float z, float above, float length);
	// Same with an explicit collision layer mask (diagnostics).
	void RequestGroundMask(int slot, float x, float y, float z, float above, float length, std::uint32_t mask);
	// Latest result for the slot: ground z, or -FLT_MAX if nothing was hit. `age` = frames since computed.
	bool LatestGround(int slot, float& ground, unsigned& age);
	// A world shift or save load changes the local coordinate frame. No cached
	// ray result may be consumed in the next frame under its old coordinates.
	void ResetProbes();
	// Load-ahead lead time in seconds (the grid is centred this far ahead along the velocity, capped by the grid size).
	void SetLeadSeconds(float seconds);
}
