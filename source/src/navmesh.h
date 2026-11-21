#pragma once
// Car navmesh service (2026-11-05, the user: "Build all three" — route bake, own lane first, A* when lanes cannot pass).
// One Detour tile per game cell (100 m), baked from the game's OWN navmesh input for that cell (navprobe::Collect, the calls proven
// in game 21:46 / 22:04) with car settings (navbake.h), on a worker thread, one cell at a time. Tiles are cached per world in
// Logs\FlyingVehicleOverhaul\navcache\<world>\<gx>_<gy>.tile (header: magic, version, settings hash, cell) and loaded from there the
// next time, so a cell is baked once per world and settings. Only cells the chosen route needs are asked for (Want, from the lane
// window of a car on a route), and only cells within one cell of the player are baked (the loaded 3x3, which stays loaded while the
// worker runs). Nothing here changes driving by itself: lane driving asks Find when its lane planner has no line (vehicleflight.cpp).
// Cell edges (2026-11-06): each tile is baked with its neighbours' edge bands (their triangles within Recast's border of this cell),
// so a road the game files under the next cell still reaches the edge and the tiles join; tile files (version 3) keep one fingerprint
// per neighbour, and a tile baked before a neighbour was loaded is checked again (rebaked if that neighbour adds anything) once it is.
// Traces: OBS system=navmesh event=want/bake/cache/tile/verify/verified/evict/world/refused/capture.
namespace navmesh
{
	// Main thread, each frame: world changes, finished tiles into the mesh, the next cell to the worker.
	void Tick(bool loading);
	// Main thread: the car navmesh is wanted under (x, y), Starfield metres (the route's cells). Cheap when already there.
	void Want(double x, double y, const char* reason);
	// Main thread: a tile for the cell under (x, y) is in the mesh.
	bool Has(double x, double y);
	// Main thread: drop every cached tile of this world from disk and memory, so the cells are baked again (route refresh).
	void Rebake(const char* reason);
	struct Route
	{
		bool found = false, complete = false; int polys = 0, corners = 0; double length = 0, startSnap = -1, endSnap = -1;
		float pts[32][3] = {};   // straight-path corners, Starfield frame (first = start on the mesh)
		const char* why = "";
	};
	// Main thread: A* on the car navmesh from `from` to `to` (Starfield frame); each end snaps to the mesh within `snap` m sideways.
	Route Find(const float from[3], const float to[3], float snap = 3.0f);
	int Tiles();
	// Main thread: horizontal metres from (x, y) at height z to the nearest point of the car navmesh within 1.5 m up or down; 9 when
	// there is none within 2 m; -1 when no tile is loaded there. The lane planner treats >= 1.1 m as a wall (lanedriver kNavWall),
	// but only while the car itself is on the car navmesh at its own height (lanedriver navBlind).
	double Clearance(double x, double y, double z);
	// Main thread: the car navmesh surface nearest height z at (x, y), read straight from the cell's saved tile, so it works behind the
	// loading screen, where no ray answers and Tick does not run. 1 found; 0 no saved tile for that cell; -1 a tile but no surface within
	// 1 m sideways and 5 m up or down. Used to place new traffic (notes/SPAWN_DESIGN_20261106.md).
	int Deck(double x, double y, double z, double& deck);
	// Main thread, test builds only (FVO_RELEASE off): the car stands on its wheels at (x, y, z = under the hull) where the car navmesh
	// has no surface at its height (2026-11-06 10:37 run: a ramp and a deck missing). Once per cell per world, at most 4: the worker
	// collects that cell's input again (own + neighbours' edge bands), bakes it, writes navcache\<world>\capture_<gx>_<gy>_<run>_input.obj
	// and _car.obj, and OBS system=navmesh event=capture lists the input's surfaces over the car (height, u/d facing, slope, flags) and
	// the car mesh's there, so the log alone says whether the game's input or the car settings lose the surface.
	void Capture(double x, double y, double z, const char* reason);
}
