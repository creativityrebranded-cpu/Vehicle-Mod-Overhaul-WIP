#pragma once

namespace logger
{
	// Opens Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets.log ; the previous one is kept as SeamlessPlanets.prev.log.
	void Init();
	void Write(const char* fmt, ...);

	// Chatty lines (per-second NPC state, the 2 s pilot position line, HUD notices, traffic per-NPC lines) only when
	// Logs\FlyingVehicleOverhaul\settings.ini has [log] verbose=1 (re-read every 5 s). Decisions and faults are always logged.
	bool Verbose();

	// No lock, no timestamp: for the crash handler (the lock may be held by the crashing thread).
	void WriteUnlocked(const char* fmt, ...);
}
