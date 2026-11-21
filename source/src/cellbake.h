#pragma once
// Each attached exterior cell contributes to an 8 m terrain grid (cellbakegrid.h) using the existing
// bake slots. Pilots compare live terrain against the grid. Solid evidence comes from the other
// World sources after the whole-cell collision query hung in the physics engine.
// Main thread (the frame task).
namespace cellbake
{
	void Tick(bool loading);
	// The baked terrain height at (x, y); false where nothing is baked yet.
	bool Ground(double x, double y, double& z);
	// Whether NPC pilots use the baked terrain instead of their own terrain probes.
	bool UseForPilots();
	// A live terrain probe answered `live` at (x, y): counted against the grid for the agreement line.
	void Compare(double x, double y, double live);
	void Reset();
}
