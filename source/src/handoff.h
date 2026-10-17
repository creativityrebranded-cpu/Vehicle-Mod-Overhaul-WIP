#pragma once

#include <cstdint>

// Zones (the user's hex design, "zones" so as not to clash with the engine's 100 m cells): a planet-fixed
// rhombille lattice of cube + star zones. The player is always owned by one zone; ownership changes by a commit
// timer or the edge rule. The physics world is enlarged so the engine handles everything inside a large area
// natively (no in-place world shifting); the zone handoff (an engine relocation) for very long distances is
// still to come.
namespace handoff
{
	// Per frame, main thread.
	void Tick();

	// Sets iNumCellWorldExtents:Havok to the enlarged value before physics worlds are created. Idempotent.
	void EnlargePhysicsWorld();
	// Half-extent of the physics box in metres (iNumCellWorldExtents x 50 m, as set by EnlargePhysicsWorld).
	float PhysicsHalfExtent();

	// Forget the current zone (e.g. after a reload); tracking restarts fresh.
	void Reset();

	// The zone containing a world position (on the current planet, once measured), as a key; false if unknown.
	bool ZoneKeyAt(float x, float y, float z, std::uint64_t& key);
	// The zone containing a planet lat/long (radians).
	bool ZoneKeyAtLatLong(double lat, double lon, std::uint64_t& key);
	// Rhombus side of the current planet's zones in metres (0 if not measured).
	double ZoneSide();
	// Planet radius in metres (0 if not measured yet).
	double PlanetRadius();
}
