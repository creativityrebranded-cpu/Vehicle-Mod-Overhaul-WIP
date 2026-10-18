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
	// Every reference in the attached grid cells. Between two NewFrame() calls the scan is done once and shared: a later call with the same or a smaller
	// limit gets the first `limit` references of it (the order is the cells' own, so a prefix is what a smaller scan would have returned). A plugin that never
	// calls NewFrame (or stopped calling it) gets a fresh scan every time.
	std::vector<std::uintptr_t> LoadedRefs(unsigned limit = 4000);
	// Main thread, once per frame, before anything scans: forget the shared scan.
	void NewFrame();
	bool IsActor(std::uintptr_t ref);
}
