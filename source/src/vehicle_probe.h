#pragma once

#include <cstdint>

// Services the rover's ground samples in the game's update context when the
// traversal plugin is absent. Never installed alongside the core hook.
namespace vehicle_probe
{
	bool Install();
	void Request(int slot, float x, float y, float z, float above, float length, std::uint32_t mask = 0);
	bool Latest(int slot, float& height, unsigned& age);
	void Reset();
}
