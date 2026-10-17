#pragma once

#include <cstdint>

// Guards the terrain page stitch (FUN_14098e8d0) against world shifts that remove a neighbour page mid-job.
namespace terrainguard
{
	bool Install();
	unsigned Skipped();
	// GetTickCount64 of the last terrain block build or page stitch (worker jobs); 0 before any.
	unsigned long long LastActivityMs();

	// Terrain collision rebuild (0.44.0). A cell's heightfield collision is built once, by the LoadCell finish step
	// (FUN_14041EFA0 -> FUN_140990850), and only when the terrain source rectangle covers the cell. Cells loaded before
	// the source moved to them (after a load far from the landing origin) never get it, and after a shift the existing
	// collision is not found by any ray. Pending cells are rebuilt from the game-update thread with the engine's own
	// steps: detach, clear the cell's physics systems, build, attach.
	enum class Why { Load, Shift };
	void MarkPending(std::uintptr_t cell, Why why);
	// Every grid cell (after a shift), rebuilt no earlier than `delayMs` from now.
	void MarkGridPending(std::uintptr_t tes, unsigned delayMs);
	// Called from the sync point every frame (game-update thread).
	void ServicePending(std::uintptr_t tes);
	// The player's parent cell is waiting for its collision after a load: hold the player.
	bool HoldPlayer(std::uintptr_t cell);
	// Cells waiting for a terrain collision rebuild.
	unsigned PendingCount();
}
