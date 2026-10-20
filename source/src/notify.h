#pragma once

namespace notify
{
	// Shows a message in the HUD's notification area, exactly like Papyrus Debug.Notification.
	// Main thread only.
	void Show(const char* fmt, ...);
	// Log only (no HUD message): diagnostics and periodic readouts stay out of the game screen (0.56.2).
	void Log(const char* fmt, ...);
}

namespace readout
{
	// Per-frame: notifies on every cell change (grid coordinates + form id), reports speed, and
	// warns "cell not ready" when the player enters a cell that has not finished loading.
	void Tick();

	// Every few seconds, moves the player's custom map marker to the player, so the planet map shows
	// where they are. Replaces any custom marker the player placed.
	void TrackPlayerMarker();

	// Places the player's custom map marker (the game's own, FUN_141A43220) at `pos` (world coordinates) in the current planet's world. False: no planet world yet.
	bool SetPlayerMarker(const float pos[3]);

	// Called after a successful re-centre.
	void OnRecentre(int cellsX, int cellsY, unsigned objects);
}
