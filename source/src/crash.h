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
	// The engine commands FVO ran last, for the crash and hang reports. Crumbs are drowned out by ray lines, and a heap fault shows up after its cause.
	void Action(const char* what, unsigned id);
	// Each update: a 6 s stop logs its stack; a 45 s stop saves all threads to last-hang.dmp.
	// [diagnostics] hangDumpSeconds (6..600) changes the dump threshold at next launch.
	void Beat();
	void SetGameplay(bool on);  // foreground gameplay gaps >=3 s can capture three successful small dumps; failures retry, gate/recovery diagnostics explain skips
}
