#pragma once

#include <cstdint>

// Structured session telemetry: one JSON object per line in
// Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets\telemetry-<date>-<time>.jsonl
// Records are formatted on the calling thread and written by a background thread, so the game thread
// never waits on disk. Analyse with seemless/scripts/analyze_telemetry.py.
namespace telemetry
{
	void Init();

	// Writes one record: {"t":<ms since start>,"type":"<type>",<fields>}. `fieldsFmt` is printf-style and must
	// produce valid JSON members without braces, e.g. "\"x\":%.1f,\"y\":%.1f". Pass "" for no fields.
	void Record(const char* type, const char* fieldsFmt, ...);

	// Microseconds since the session started (high resolution).
	std::uint64_t NowUs();

	// Called once per frame from the main thread: samples state, measures frame time, tracks cell load latency.
	void Frame();
}
