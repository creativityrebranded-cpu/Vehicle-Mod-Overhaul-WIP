#pragma once

#include <cstdint>

// Zones as landing zones: planet content (POIs, creatures, NPCs: the BGSPlanetContentManager's "overlays") is
// generated per zone the way a landing does it, and deleted again when the player has left the zone behind.
//
// Generate: a landing (planet manager MoveTo FUN_14078bc30) runs every planet module's slot 7, then slot 8. The
// content step (ContentPlacement slot 7, FUN_14076df30) places a temporary landing marker, runs the virtual
// landing block generator on it (FUN_1418abd60), then tears the marker down; Overlays (apply / refresh),
// OrderedLandTextures (texture order) and NavMeshGen (regenerate flag) follow. Per zone we spread landing points
// one terrain block apart over the whole zone (count follows zone size), generate them in time-to-arrival order
// (the zone at a random 20-40 s TTA ahead is queued early), and run the module post-steps every few points.
// Terrain relocation (Landscape slot 8) is never run.
//
// Delete: the manager keeps its overlay records per planet and block (PCM+0x358). Records that appear while a
// zone is current are tagged with that zone; once the player is far from a zone, its records go through the
// game's own removal (FUN_1418b40b0, with the same quest guard as the game's "clear planet" FUN_1418b1c60). The
// removal refuses content that is still in use. A deleted zone generates afresh when it is entered again.
namespace content
{
	// GetTickCount64 of the last landing post-steps run (module hooks + overlay refresh); 0 = never.
	unsigned long long LastPostSteps();
	// A zone was committed (start = first zone after a load or arrival). key identifies the zone.
	void OnZoneEnter(std::uint64_t key, float x, float y, float z, float vx, float vy, bool start);

	// Per frame, main thread.
	void Tick();

	// Forget everything (loading a save).
	void Reset();
}
