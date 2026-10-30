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
	int CastNow(const float from[3], const float to[3], std::uint64_t mask, prefetch::RayHit& out);  // main thread; paused game OK
	bool LatestRay(int slot, prefetch::RayHit& out, unsigned& age);
	// Hull sweeps (prefetch::RequestSweep / LatestSweep in this DLL).
	void RequestSweep(int slot, std::uintptr_t instance, float dirX, float dirY, float range, std::uint64_t mask);
	bool LatestSweep(int slot, prefetch::SweepHit& out, unsigned& age);
	// Solid objects in a box (cellbake, 2026-10-29): the physics world's own box query, run in the game's update context like the rays.
	// One at a time: RequestSolids is false while one waits; TakeSolids gives the form ids of the references owning the hit bodies
	// (ok = the query ran) once it is done.
	bool RequestSolids(const float mn[3], const float mx[3]);
	bool TakeSolids(std::vector<std::uint32_t>& ids, bool& ok);
}
