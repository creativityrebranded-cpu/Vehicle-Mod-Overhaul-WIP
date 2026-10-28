#pragma once

#include <cstdint>
#include <vector>
#include "framework.h"

// Vehicle registry (2026-10-17, the user: "we don't generate, we just store vehicles at location"). Nothing is created
// or placed: every real vehicle in the loaded cells is recorded with its place, owner and who uses it (parked / player /
// npc / own), and changes (a new place after a move, a new user) are logged and written to
// Logs\FlyingVehicleOverhaul\vehicles.tsv. Parked Vehicles' props are left alone here (they are to become real vehicles in Vehicles.esm
// itself, 2026-10-17). Only bases with the vehicle keyword count as vehicles. Duplicate vehicles an older
// build created at a parked spot are cleared once (one kept).
namespace realvehicles
{
	// Main thread: jobs every frame; registry passes start every 2 s and visit one cell/frame.
	void Tick();
	bool CanManagePlacement(std::uint32_t id); // main thread: known idle vehicle, not owned/occupied/being swapped
	// Bounded discovery: up to 256 references / about 0.5 ms per call, including dense
	// cells. Cursor starts empty; true when a complete sweep finishes and resets it.
	bool ForEachLoadedStep(void (*fn)(std::uintptr_t ref, void* ctx), void* ctx, framework::ScanCursor& cursor);
	// A save load: forget the processed list (the save restores the swaps it already holds).
	void Reset();
	// Placement mode: the settings toggle (features::Id::PlacementMode) or Data/SFSE/Plugins/FlyingVehicleOverhaul.placement-recording.
	bool RecordingPlacements();
	bool RecordPlacement(double x,double y,double z,double yaw,bool estimated,const char* label);
	// Main thread, placement mode: deletes the saved spot nearest (x, y, z) in the player's world (a recorded one at once;
	// a baked one is listed in placement-removed.tsv for the next bake), and makes (x, y, z) the area's summon point.
	bool DeletePlacement(double x,double y,double z);
	bool HasPlacementNear(double x,double y,double z); // read-only, main thread; cached authoring spots
	bool SetSummonPoint(double x,double y,double z,double yaw);
	// Main thread: the summon point of the area around (px, py) in the player's world (placementedit::kAreaRadius).
	bool SummonPointNear(double px,double py,double& x,double& y,double& z,double& yaw);
	// Main thread: the owner of the place `ref` stands in (its cell's owner faction, else the faction owning most things
	// in that cell), 0 when none or unknown (a town square with no owner, open ground).
	std::uintptr_t PlaceOwner(std::uintptr_t ref);
	// Main thread: every vehicle base form carrying the vehicle keyword (empty until the pool is found).
	const std::vector<std::uintptr_t>& VehicleTypes();
}
