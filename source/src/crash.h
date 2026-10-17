#pragma once

// Crash logger: records the first fatal exceptions (access violations etc.) with the faulting module + offset,
// registers and an unwound stack, straight to SeamlessPlanets.log. Starfield writes no crash report of its own.
namespace crash
{
	void Install();
	// Faults on this thread are expected (a guarded engine call): not logged as crashes.
	void SetExpectedFaults(bool on);
}
