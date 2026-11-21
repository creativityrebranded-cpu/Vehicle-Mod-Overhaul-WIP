#pragma once

#include <cstdint>
#include <vector>

#include "prefetch.h"

// Services the rover's ground samples in the game's update context when the
// traversal plugin is absent. Never installed alongside the core hook.
namespace vehicle_probe
{
	bool Install();
    // Completed native ship-system updates. Monotonic across save resets;
    // the composition root uses this pulse instead of command-pump frequency.
    std::uint64_t UpdateSerial();
    void ReportUpdateCost(); // main thread, bounded ten-second telemetry

	void Request(int slot, float x, float y, float z, float above, float length, std::uint32_t mask = 0);
	bool Latest(int slot, float& height, unsigned& age);
	void Reset();
	// Forward rays (prefetch::RequestRay / LatestRay in this DLL).
	void RequestRay(int slot, const float from[3], const float to[3], std::uint64_t mask);
	bool RaysLive();  // the queued rays are running: World has live evidence (source priority falls back without it)
	int CastNow(const float from[3], const float to[3], std::uint64_t mask, prefetch::RayHit& out);  // main thread; paused game OK
	// Field Mask: why the last CastNow on this thread did not answer (0 answered). Additive; CastNow itself is unchanged.
	enum RefusalWhy { RefusedNone = 0, RefusedBackoff, RefusedRaysOff, RefusedNoWorld, RefusedSettling, RefusedHeld, RefusedCasterBusy, RefusedFault, RefusedKinds };
	int LastRefusal();
	const char* RefusalName(int why);
	// World's volume (2026-11-11): may a direct cast go out now? The same gates CastNow applies (backoff, rays off, no world, map procgen
	// hold, physics world settling), asked before casting, so a closed gate is a schedule, not a refused cast. Main thread. `why` as above.
	bool GateOpen(int& why);
	bool LatestRay(int slot, prefetch::RayHit& out, unsigned& age);
	bool RayRefused(int slot, const float from[3], const float to[3]);
	// Hull sweeps (prefetch::RequestSweep / LatestSweep in this DLL).
	void RequestSweep(int slot, std::uintptr_t instance, float dirX, float dirY, float range, std::uint64_t mask);
	bool LatestSweep(int slot, prefetch::SweepHit& out, unsigned& age);
	// World radar path sweep (2026-11-13): the vehicle's own hull from its body position + offset, turned by yaw (FVO heading sense), along disp,
	// every contact kept. Main thread. 1 cast, 0 not cast (LastRefusal says why), -1 fault.
	int SweepPathNow(std::uintptr_t instance, const float offset[3], float yaw, const float disp[3], std::uint64_t mask, prefetch::SweepContact* out, int max, int& count, unsigned& micros);
	// Solid objects in a box (cellbake, 2026-10-29): the physics world's own box query, run in the game's update context like the rays.
	// One at a time: RequestSolids is false while one waits; TakeSolids gives the form ids of the references owning the hit bodies
	// (ok = the query ran) once it is done.
	bool RequestSolids(const float mn[3], const float mx[3]);
	bool TakeSolids(std::vector<std::uint32_t>& ids, bool& ok);
}
