#pragma once

#include <cstdint>
#include <string>

// Detailed trace: a separate, high-volume file (Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets\trace-<time>.log)
// written by a background thread. Every line: time, game frame number, thread id, channel, text.
// Channels: log (mirror of SeamlessPlanets.log), snap (state snapshot), grid / planet / tes (state changes), write
// (player position writes), cellload (planet cell load jobs), mark (F9). Snapshots run 10 times a second, every frame
// during a burst (after shifts, saves, loads, F10) and while verbose mode is on (F9 toggles).
namespace trace
{
	void Init();
	void Line(const char* channel, const char* fmt, ...);
	// Mirror of a finished logger line (no formatting).
	void Raw(const char* channel, const char* text);
	// Called once per game frame from the sync point.
	void NextFrame();
	unsigned long long Frame();
	// Per-frame detail for `ms` from now.
	void Burst(unsigned ms, const char* why);
	bool Detailed();
	// F9: toggles verbose mode and writes a marker line (call every frame).
	void HotkeyTick();

	// Flight recorder (2026-10-17). Lines of any length (raw object dumps) go to a ring buffer that always keeps the last
	// ~2400 lines; they reach the file only through Marker, or directly while Detailed() is on.
	void Ring(const char* channel, const std::string& text);
	// Writes the ring (the seconds before now) to the file, then starts a 20 s per-frame burst. Hotkey and automatic triggers.
	void Marker(const char* why);
}
