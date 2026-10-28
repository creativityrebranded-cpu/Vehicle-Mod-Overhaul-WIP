#pragma once

#include <cstdint>

// Hardware write watch (debug register DR0) on one float in game memory, for finding which engine code writes it.
// Armed on every thread of the process by a helper thread; logs writes that move the value more than `jump` away
// from the value being tracked (with the writing code address and an unwound stack). Tracks ordinary changes.
namespace watchpoint
{
	void Install();
	// Watch the float at `address` for `ms` milliseconds. `what` names it in the log.
	void Arm(const float* address, float jump, unsigned ms, const char* what);
	// Call every frame: disarms when the time is up.
	void Tick();
	// Watching this address already (armed and not expired)?
	bool Watching(const float* address);
	// Allow a fresh set of reports (e.g. at each shift).
	void ResetReports();
	// Which thread is running this (for logs).
	unsigned long ThreadId();
}
