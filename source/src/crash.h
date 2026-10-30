#pragma once

// Crash logger: records the first fatal exceptions (access violations etc.) with the faulting module + offset,
// registers and an unwound stack, straight to SeamlessPlanets.log. Starfield writes no crash report of its own.
namespace crash
{
	void Install();
	// Faults on this thread are expected (a guarded engine call): not logged as crashes.
	void SetExpectedFaults(bool on);
	// Breadcrumbs: the last 24 things the plugin did that touch the engine (what is a string literal, arg any number). Any thread, lock free, cheap. A crash prints
	// them with their age and thread, so a crash in a game thread can be matched to what the plugin was doing a moment before.
	void Crumb(const char* what, int arg = 0);
	// The freeze recorder: the game thread calls this each update. Stopped for 6 s: its stack goes to the log (a freeze writes no crash report).
	void Beat();
}
