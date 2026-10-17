#pragma once

namespace logger
{
	// Opens Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets.log ; the previous one is kept as SeamlessPlanets.prev.log.
	void Init();
	void Write(const char* fmt, ...);

	// No lock, no timestamp: for the crash handler (the lock may be held by the crashing thread).
	void WriteUnlocked(const char* fmt, ...);
}
