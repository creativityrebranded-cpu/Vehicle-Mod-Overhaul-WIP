#pragma once

#include <cstdint>

#include "prefetch.h"

// Services the rover's ground samples in the game's update context when the
// traversal plugin is absent. Never installed alongside the core hook.
namespace vehicle_probe
{
	bool Install();
	void Request(int slot, float x, float y, float z, float above, float length, std::uint32_t mask = 0);
	bool Latest(int slot, float& height, unsigned& age);
	void Reset();
	// Forward rays (prefetch::RequestRay / LatestRay in this DLL).
	void RequestRay(int slot, const float from[3], const float to[3], std::uint64_t mask);
	bool LatestRay(int slot, prefetch::RayHit& out, unsigned& age);
}
