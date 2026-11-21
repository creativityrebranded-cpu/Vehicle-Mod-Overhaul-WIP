#pragma once
// Navmesh input probe (2026-11-05, the user approved "a read-only probe build that runs these calls on a test key").
// Diagnostic only: on the test key / (fallback . or '), the cell the player stands in is run through the game's OWN navmesh input
// step (the one its runtime generator runs before Recast) into objects the probe owns, and the collected triangles are written to
// Logs\FlyingVehicleOverhaul\navprobe\cell_<cell>_<time>.obj with a .txt summary. Nothing is hooked; the game's generator, the
// planet module and NPC navmesh never see it. Engine calls (Starfield 1.16.244), each checked by its first 16 bytes before use:
//   FUN_140698480 / FUN_1406986A0 settings from the game's own values, FUN_140698630 their free;
//   FUN_1406ACB50 the cell entry as the game makes it; FUN_1426EDE70 the per-cell context (base class: the cell is found from the entry);
//   FUN_1406AD0F0 the input step (objects' collision + worldspace terrain into a buffer of 12-byte vertices and 24-byte triangles);
//   FUN_14039E730 the data holder's destructor (frees that buffer with the game's allocator).
// Then (2026-11-05, the user: "Download Recast") the same triangles are baked into a car navmesh with our own Recast copy (navbake.h,
// car settings, no engine calls) and written as <name>_car.obj, with four 30 m A* tests from the player (event=car_mesh).
// Runs on its own thread, as the game runs this step on its generator threads; one probe at a time; every step and fault is logged
// as OBS system=navprobe. Design and evidence: notes/navmesh-format-20261105/FORMAT.md sections 5b-5c.
#include <cstdint>
#include <vector>

namespace navprobe
{
	// Main thread, each frame: the key, starting the worker, reporting its result.
	void Tick(bool loading);
	// Main thread: the engine functions' signatures (checked once). Collect may only be called when this is true.
	bool EngineReady();
	// The game's navmesh input step for one loaded cell (any thread; navmesh.cpp's worker). v: x y z per vertex; t: 6 dwords per
	// triangle (3 indices + the game's 3 extra). sys/run tag the log lines. False when nothing usable came back (logged with the reason).
	bool Collect(const char* sys, unsigned run, std::uintptr_t cell, std::uint32_t cellId, std::vector<float>& v, std::vector<std::uint32_t>& t, double& msInput);
}
