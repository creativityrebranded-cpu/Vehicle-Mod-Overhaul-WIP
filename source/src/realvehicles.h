#pragma once

#include <cstdint>

// Vehicle registry (2026-10-17, the user: "we don't generate, we just store vehicles at location"). Nothing is created
// or placed: every real vehicle in the loaded cells is recorded with its place, owner and who uses it (parked / player /
// npc / own), and changes (a new place after a move, a new user) are logged and written to
// Logs\SeamlessVehicle\vehicles.tsv. Parked Vehicles' props are left alone here (they are to become real vehicles in Vehicles.esm
// itself, 2026-10-17). Only bases with the vehicle keyword count as vehicles. Duplicate vehicles an older
// build created at a parked spot are cleared once (one kept).
namespace realvehicles
{
	// Main thread, every frame (works every 2 s).
	void Tick();
	// Main thread: calls fn for every enabled vehicle (keyword-gated) in the attached grid cells.
	void ForEachLoaded(void (*fn)(std::uintptr_t ref, void* ctx), void* ctx);
	// A save load: forget the processed list (the save restores the swaps it already holds).
	void Reset();
}
