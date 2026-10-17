#pragma once

// Planet globe (star map body inspect) markers: adds "Player Location" (streams with the player, or their
// ship while aboard) and "My Ship" (the player's home ship) to the POI list the globe publishes to its UI.
// See the engine reference, "Planet globe".
namespace globe
{
	// Installs the hook on the globe's marker publish. Call once at plugin load, after signature checks.
	bool Install();

	// Per frame, main thread: refreshes the cached marker positions (the hook may run on another thread).
	void Tick();
}
