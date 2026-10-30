#pragma once

#include <cstdint>
#include "vehiclelinks.h"
#include "fvolimits.h"

// The world layer's vehicle list (notes/VEHICLE_ARCHITECTURE_PROPOSAL_20261017.md; header agreed with the flight
// session 2026-10-17): every keyword-gated vehicle in the attached cells, published once per frame on the main thread
// and read from any thread (the physics step included) as a synchronized copy.
// bodyIds enumerate the reference's owned collision bodies; native callbacks identify the chassis. rot[] = the 3D
// root's world rotation (heading-only when not loaded); half[] from the base's bounds (TESBoundObject +0x48 / +0x54),
// sanity-checked, scaled by the live transform and capped by the 3D's bound sphere radius; vel from smoothed position differences;
// driverId is the actual native seated driver for player and NPC vehicles (0 when empty/unknown).
namespace world
{
    // Main thread before frame registration. Replacing providers clears the old
    // snapshot and in-progress scan; ordinary save resets retain the connection.
    void SetServices(vehiclelinks::Services services);
	struct VehicleInfo
	{
		std::uint32_t formId = 0, baseId = 0;
		std::uintptr_t ref = 0;  // main thread only, in the frame it was published
		float pos[3] = {}, vel[3] = {};
        float center[3]={}; // actual base-bounds centre transformed into the reference frame
        float mass=1500; bool nativeMass=false; // recent native chassis observation, otherwise bounds estimate
		float rot[9] = {};       // rows: right, forward, up (heading only in v1)
		float heading = 0;       // radians, ref +0x88; forward = (sin, cos)
		float half[3] = {};      // half-extents x (width), y (length), z (height)
		bool baseBoundsValid = false; // false means half[] is the legacy fallback, not measured bounds
		std::uint32_t driverId = 0;
		enum class Who : std::uint8_t { None, Player, Npc, Parked } who = Who::None;
		std::uint32_t bodyId = 0;
		std::uint32_t bodyCount=0,bodyIds[16]={}; // owned scene bodies; native vehicle callback identifies its chassis
	};
	// Capacity, not a policy (2026-10-29, the user: "raise the limit"; "base it on the resource req of each"): every loaded vehicle up
	// to this many is listed; how often each one's 3D and bodies are re-read is decided per vehicle by its measured cost against a
	// frame budget (vehicleworld.cpp Publish, settings.ini [world] sampleBudgetMs).
	constexpr std::uint32_t kMaxVehicles = fvolimits::kVehicles;
	struct Snapshot
	{
		std::uint64_t frame = 0;
		std::uint32_t count = 0;
		VehicleInfo v[kMaxVehicles];
	};

	// Main thread, once per frame (before vehicleflight::Tick).
	void Publish();
    // Bounded scene ownership for a known reference, independent of the
    // general snapshot capacity. Main-thread only.
    unsigned SceneBodies(std::uintptr_t ref,std::uint32_t (&ids)[16]);
	// A save load: forget everything.
	void Reset();
	// Any thread: a synchronized copy of the latest snapshot; false before the first publication.
	bool Vehicles(Snapshot& out);
	// Any vehicle base form's size from its bounds (half-extents width, length, height; centre offset from the origin).
	// False when unreadable (half/centre then hold a REV-8-sized default).
	bool BaseSize(std::uintptr_t base, float half[3], float centre[3]);
	// Any thread: the first vehicle (other than ignoreFormId / ignoreBodyId; 0 = none) a body of half-extents `half`
	// moving from p along d (metres) would touch, as vertical cylinders; distance along d in *distanceOut. False: clear.
	bool FirstInPath(const float p[3], const float d[3], const float half[3], std::uint32_t ignoreFormId, std::uint32_t ignoreBodyId, VehicleInfo* hit,
		float* distanceOut);
}
