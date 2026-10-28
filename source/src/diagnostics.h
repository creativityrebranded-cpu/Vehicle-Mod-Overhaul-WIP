#pragma once

namespace diagnostics
{
	// Starts a background thread that logs the player's position, the planet manager's terrain
	// grid and shift events twice a second. Read-only: it never writes game memory.
	void Start();
}
