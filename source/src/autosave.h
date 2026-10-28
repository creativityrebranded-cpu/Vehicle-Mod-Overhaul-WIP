#pragma once

// Autosave while playing (user, 2026-10-14). The game's own timed autosave only fires when the game is paused or a
// menu opens (bSaveOnPause:GamePlay checks fAutosaveEveryXMins there), never while moving. Every kIntervalMinutes since
// the last save, at a safe moment (not flying or launching, no re-centre waiting, the world settled), this makes the
// same request the pause path makes: FUN_14184ff90(saveLoadManager, 1, 0), which only sets the manager's
// autosave flags if saving is allowed now; the manager saves in its own update.
namespace autosave
{
	void Tick();  // per frame, main thread
}
