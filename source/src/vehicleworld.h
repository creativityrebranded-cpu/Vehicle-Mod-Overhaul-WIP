#pragma once

#include <cstdint>

// The world layer's vehicle list (notes/VEHICLE_ARCHITECTURE_PROPOSAL_20261017.md; header agreed with the flight
// session 2026-10-17): every keyword-gated vehicle in the attached cells, published once per frame on the main thread
// and read lock-free from any thread (the physics step included) as a copy.
// bodyIds enumerate the reference's owned collision bodies; native callbacks identify the chassis. rot[] = the 3D
// root's world rotation (heading-only when not loaded); half[] from the base's bounds (TESBoundObject +0x48 / +0x54),
// sanity-checked, scaled by the live transform and capped by the 3D's bound sphere radius; vel from smoothed position differences;
// driverId is the actual native seated driver for player and NPC vehicles (0 when empty/unknown).
namespace world
{
	struct VehicleInfo
	{
		std::uint32_t formId = 0, baseId = 0;
		std::uintptr_t ref = 0;  // main thread only, in the frame it was published
		float pos[3] = {}, vel[3] = {};
        float center[3]={}; // actual base-bounds centre transformed into the reference frame
        float mass=1500; bool nativeMass=false;
		float rot[9] = {};       // rows: right, forward, up (heading only in v1)
		float heading = 0;       // radians, ref +0x88; forward = (sin, cos)
		float half[3] = {};      // half-extents x (width), y (length), z (height)
		std::uint32_t driverId = 0;
		enum class Who : std::uint8_t { None, Player, Npc, Parked } who = Who::None;
		std::uint32_t bodyId = 0;
		std::uint32_t bodyCount=0,bodyIds[16]={}; // owned scene bodies; native vehicle callback identifies its chassis
	};
	struct Snapshot
	{
		std::uint64_t frame = 0;
		std::uint32_t count = 0;
		VehicleInfo v[64];
	};

	// Main thread, once per frame (before vehicleflight::Tick).
	void Publish();
	// A save load: forget everything.
	void Reset();
	// Any thread: a synchronized copy of the latest snapshot; false before the first publication.
	bool Vehicles(Snapshot& out);
	// Any thread: the first vehicle (other than ignoreFormId / ignoreBodyId; 0 = none) a body of half-extents `half`
	// moving from p along d (metres) would touch, as vertical cylinders; distance along d in *distanceOut. False: clear.
	bool FirstInPath(const float p[3], const float d[3], const float half[3], std::uint32_t ignoreFormId, std::uint32_t ignoreBodyId, VehicleInfo* hit,
		float* distanceOut);
}
