#pragma once

#include <cstdint>

namespace recentre
{
	// Moves the world origin by (cellsX, cellsY) whole cells on the current planet, so a position that
	// was at (cellsX * 100, cellsY * 100) becomes (0, 0). With followLanding, the requested target must be the
	// player's block-aligned landing origin and the stored landing follows it. Must run on the main thread. Returns false
	// (and changes nothing) if the game is not in a state where this is safe.
	bool Run(std::int32_t cellsX, std::int32_t cellsY, bool followLanding = false);

	// Terrain during a switch: Bethesda's origin-delta path (true) or only the planet manager bookkeeping (false).
	void SetEngineTerrainShift(bool on);

	// What the world is centred around: the player outdoors, or the ship the player is inside (landed or
	// flying over a planet). The engine streams the planet around the ship in that case (FUN_1417df210).
	struct Anchor
	{
		std::uintptr_t ref = 0;   // null when there is none (indoors)
		std::uintptr_t cell = 0;  // the anchor's exterior cell
		bool inShip = false;
	};
	Anchor CurrentAnchor();

	// The player is standing/sitting in a ship's interior cell.
	bool InShipInterior(std::uintptr_t ref);

	// The ship a reference is inside (engine FUN_142116840), or 0.
	std::uintptr_t ShipOf(std::uintptr_t ref);

	// Distance (cells, Chebyshev) from the anchor to the nearest hand-built cell of its worldspace, or INT32_MAX
	// in procedural worldspaces.
	std::int32_t NearestAuthoredCell();

	// Per frame: a loaded-grid cell whose stored coordinates are far outside the grid window still carries
	// coordinates from before a shift (it was loaded from the data files or buffered meanwhile). Move it and
	// its objects into the current frame (its cell-map key). This is what makes re-centring safe everywhere,
	// including hand-built worldspaces.
	void FixStaleCells();

	// Per frame: if a shifted worldspace was left or its planet manager restarted (save load, death reload,
	// fast travel, doors), put its bookkeeping back in the original frame. Returns true if anything was reset.
	bool WatchFrame();
	// Incremented only when the planet setup is entered from the native save-load caller.
	// A surface landing also passes through planet setup, but must not clear save-local state.
	std::uint64_t NativeSaveLoadSerial();

	// Puts every shifted worldspace's bookkeeping back in its original frame (cell map keys, persistent
	// references, shift record) without moving loaded cells. For "a reload is about to happen".
	void ResetAllFrames(const char* why, bool movePersistent);

	// Fully shifts the current worldspace back to its original frame (everything loaded moves), e.g. before a
	// save so saves always hold original-frame positions. False if the world was not settled enough.
	bool ShiftBackToOriginal(const char* why);

	// Total shift (cells) applied to the anchor's worldspace so far; (0, 0) = its original frame.
	void TotalShift(std::int32_t& x, std::int32_t& y);
	// The landing lat/long of this load as the engine set it (mgr+0x98 before the landing records began following
	// shifts, 0.55.0); while the world is unshifted, the live mgr+0x98.
	bool OriginalLandingLatLong(double out[2]);

	// Outdoors (or in a ship that is outdoors), planet manager idle, the anchor's cell and every loaded grid cell fully attached.
	bool WorldSettled();

	// The current worldspace is hand-built (city etc.): its content sits at fixed world positions.
	bool AuthoredWorldspace();

	// The zone handoff asks for the world to be centred on a zone (handoff.cpp: the zone is the physics region): the
	// cell offset (cx, cy) of the zone centre from the current origin. Urgent = the player would reach the physics edge
	// within the handoff deadline: run as soon as the loaded cells are attached, at any speed.
	void RequestCentre(std::int32_t cx, std::int32_t cy, bool urgent);
	// Per frame: runs a requested re-centre at the first safe moment (every loaded cell attached for a moment, below
	// 20 m/s so the prefetch is not leading the grid; urgent: cells attached, any speed).
	void Tick();
	// True while a requested re-centre is waiting.
	bool Pending();
	// Drops a waiting re-centre request (load or transition: its cells belong to the old frame).
	void ClearRequest();
	// Measured re-centre test (F10): snapshot planet positions, shift, re-measure, roll back on any mismatch.
	void FrameCheckTick();
	// Data collection after a shift (60 s) and whenever the player's cell is not attached: the loaded grid's cells
	// (id, stored coordinates, state, cell-map key) and the map lookup for the player's coordinates.
	void CellWatchTick();
	// GetTickCount64 of the last successful re-centre (0 = none).
	unsigned long long LastRunMs();
	// Cell-loader queues with pending work (BSResource2::DB<LoadedCellDB>); a re-centre waits for 0.
	int PendingCellLoads();
	// Hooks the LoadCell run so a load requested before a shift uses the new frame's coordinates (terrain generation).
	bool InstallLoadCellFix();
	bool InstallLoadCellRequestTrace();
	bool InstallLoadCellBuildFix();
	// Hooks the engine's per-frame loaded-cells update: every shift runs there, inside the game update.
	bool InstallSyncPoint();
	// Hooks the reference position setter: old-frame player positions written just after a shift are moved into the new frame.
	bool InstallStaleWriteFix();
	// Legacy save frame conversion, retained for fallback paths.
	void BeforeSave();
	void AfterSave();
	// Called by the save record writer (rebase): the save is being written now.
	void OnSaveWrite();
	// Hooks BGSSaveLoadGame::SaveGame: matching live landings are saved unchanged; mismatches use a conversion fallback.
	bool InstallSaveHook();
	// Diagnostics for the load hang: planet manager setup and packed-origin decoding during a load.
	bool InstallLoadDiagnostics();
	// Packed planet cell id of grid (x, y) in the worldspace's current frame (~0 when not a planet worldspace).
	std::uint64_t PlanetCellIdAt(std::uintptr_t worldspace, std::int32_t x, std::int32_t y);
	// LoadedCellDB::LoadCell task nodes still alive (queued or running on a worker).
	int LiveCellLoadJobs();
}
