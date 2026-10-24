#pragma once
#include <cstdint>

// Switches the GAME's own action off on the controller buttons the mod uses in the situation now (2026-10-24, the user:
// a mod button must never also do the game's thing). Every input event is resolved to the game's user event as it is
// queued (FUN_1422DABC0 -> FUN_1422F1840); right after that, a gamepad button event on a blocked button gets the queue's
// block reason (+0x34 = 4). Every handler asks the event for its user event through vtable slot 2 (FUN_1422D6D80,
// ButtonEvent vtable 0x144D59F50): with +0x34 set that returns the empty name, so no game handler matches the press.
// The mod itself still reads the controller (bindings.cpp).
// notes/CONTROLLER_INPUT_STUDY_20261024.md, sections 5 and 5b.
namespace inputgate
{
	bool Install();                          // startup only (an E8 call redirect)
	void SetBlocked(std::uint32_t padBits);  // bindlogic controller bits; any thread
	unsigned TakeBlockedCount();             // events blocked since the last call (for the log)
}
