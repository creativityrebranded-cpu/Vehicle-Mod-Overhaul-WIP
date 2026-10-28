#pragma once

#include <cstdint>

// Planet navmesh generation (BGSPlanet NavMeshGen module) following the player (0.54.0).
// The body->actor sync places actors (the driver included) on the navmesh; with no triangle it falls back to a
// ground search, which is the drop / fall far from the landing. The module generates navmesh per attached grid cell,
// but only while its enable flag (+0x98) is set: the landing post-step (slot 7) sets it, FUN_1407911f0(module, 0)
// clears it and wipes its queues. This watches the flag and names whoever clears it, counts the per-cell gate's
// answers (with its status and density inputs for rejected cells), reports the player cell's "generated" flag, and
// re-enables generation through the engine's own calls when it is found off while the player is on the surface.
namespace navgen
{
	bool Install();
	// Game-update thread, every frame at the sync point.
	void Service();
	// One state line now (called by the drop trace and around shifts).
	void LogState(const char* why);

	// Origin shift support (NAVMESH_SNAPSHOT_AUDIT_20261017). The module's pending entries (+0x80) and in-flight entries
	// (+0x70) are keyed by grid-frame exterior coordinates; completion re-resolves them by coordinates, so they must not
	// straddle a shift.
	// Number of navmesh entries still in flight: queued/running update task (DAT_145e610d4) plus module +0x70 entries.
	// Zero when the module is absent or disabled. Game-update thread.
	unsigned InFlight();
	// Raw counters for the flight recorder: module +0x70 entries, +0x80 entries, the update-task counter; false when no module.
	bool Counts(unsigned& inFlightList, unsigned& pendingList, int& updateTask);
	// Translate the not-yet-started entries (module +0x80) into the frame after a shift of (cx, cy) cells. Entries in
	// flight are never edited (their copies live in child jobs). Returns how many entries were translated.
	unsigned TranslatePending(std::int32_t cx, std::int32_t cy);
}
