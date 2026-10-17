#pragma once

#include <cstdint>
#include <vector>

// Small read-only scene helpers shared by the module plugins (no hooks, no writes).
namespace modscene
{
	// Class name from the MSVC RTTI behind the vtable, or "?".
	const char* RttiName(std::uintptr_t obj);
	// The scene root above the player's 3D (the top parent), or 0.
	std::uintptr_t SceneRoot();
	// Every node under root whose class name contains `cls` (depth and count limited).
	void FindClass(std::uintptr_t root, const char* cls, std::vector<std::uintptr_t>& out, unsigned limit = 5000);
	// Every reference in the attached grid cells.
	std::vector<std::uintptr_t> LoadedRefs(unsigned limit = 4000);
	bool IsActor(std::uintptr_t ref);
}
