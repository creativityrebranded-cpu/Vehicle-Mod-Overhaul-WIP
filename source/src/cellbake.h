#pragma once
// Per-cell world bake (2026-10-29, the user: "at the load of each worldcell we sweep for the large onestep cast and store that for a
// world cell and then only depend on that and cheaper sensing"). Each grid cell that becomes attached is baked once:
//   1. solid objects: the physics world's own box query over the cell (vehicle_probe::RequestSolids) names every reference with a
//      collision body there; navseed stamps their boxes into the shared nav map (its own body test found none in 1.16);
//   2. terrain: a grid of terrain probes every 8 m (cellbakegrid.h), the same probe the pilots ask for (terrain layers, from high
//      above), at most kSlotBakeCount a frame from prefetch's bake slots, whatever the number of vehicles.
// Pilots compare their live terrain probes with the grid (logged every 30 s); with settings.ini [nav] bakedGround=1 the NPC pilots
// take the baked terrain where it is known and skip those probes (the player's own vehicle always probes live).
// Main thread (the frame task).
namespace cellbake
{
	void Tick(bool loading);
	// The baked terrain height at (x, y); false where nothing is baked yet.
	bool Ground(double x, double y, double& z);
	// NPC pilots may use the baked terrain instead of their own terrain probes (settings.ini [nav] bakedGround).
	bool UseForPilots();
	// A live terrain probe answered `live` at (x, y): counted against the grid for the agreement line.
	void Compare(double x, double y, double live);
	void Reset();
}
