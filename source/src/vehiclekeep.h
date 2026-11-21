#pragma once
namespace cameravisibility { enum class Evidence : int; }
#include <cstdint>

// Keep vehicles in sight loaded (2026-10-24, the user: "just make the game render it, if it's far away it'll be a dot";
// approved keeping vehicle references loaded past the normal cell limit, "esp if you can tie it to sight").
// Vehicles normally unload with their cell (100 m cells, uGridsToLoad wide). When a vehicle near or past that edge is
// inside the camera's view cone, this marks it with the game's own Full LOD setter (FUN_140b36640: ref flag 0x10000,
// then the native persistent setter FUN_140b36c40), the same state a Creation Kit "Is Full LOD" reference has.
// Released again (the flags it set are cleared, nothing else) after 10 s out of view, or when it comes back inside
// the loaded area so the mod's cell scans find it again. Only the real vehicle models are drawn: no dots, no proxies.
// Logs\FlyingVehicleOverhaul\settings.ini [view]: keepInSight=1 (0 = off), keepCone=60 (half-angle, degrees), keepMax=16 (was 48 before the 2026-10-29 perf redesign).
// kept-vehicles.txt in the same folder lists what is held, so a save made while held is cleaned up after the next load.
namespace vehiclekeep
{
	void UpdateTrafficLook();
	bool HoldBirthForLook(double x,double y,double z,unsigned milliseconds);
	cameravisibility::Evidence TrafficCameraEvidence(unsigned id,unsigned base,double x,double y,double z,bool driver);
	void Tick();   // main thread
	void WantFromBirth(std::uint32_t id);  // a traffic vehicle to hold loaded from now on, in view or not, up to [view] holdFromBirth
	bool InCameraView(double x, double y, double z);  // inside the camera's view cone now; true when the camera cannot be read
#ifndef FVO_RELEASE
	bool ReadCameraPose(float origin[3], float forward[3]);  // existing read-only view used by diagnostics
#endif
	// Card CP-1: 0 out of view now and for `seconds` ahead (camera turn extrapolated), 1 in view now, 2 comes into view ahead,
	// 3 turn or camera unknown (callers treat 1-3 as seen). Core export FVO_CameraAheadV1.
	int CameraAhead(double x, double y, double z, double seconds, double* turnDegPerS = nullptr);
	void Reset();  // a save load: forget held refs (the journal releases leftovers once the world settles)
	void ForEachHeld(void (*visit)(std::uint32_t));  // main thread: the vehicles held now
}
