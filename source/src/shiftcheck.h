#pragma once

#include <cstdint>

// SeamlessCheck (test plugin): after every world shift (zone re-centre or the core's F10 measured shift), checks each
// system against what it was just before and logs PASS / WARN / FAIL per system, so a test session gives a clear list.
//  - objects: every loaded reference must have moved by exactly the shift (statics 0.5 m, actors 3 m; baseline taken in the shift frame);
//  - player: same, plus ground under them;
//  - ground ring: 9 near (within 100 m) and 16 far (200 m) points have ground at +0.1 s, +1 s and +3 s;
//  - actors: none dropped more than 10 m in the 3 s after the shift;
// It reads the engine and the core API only; it changes nothing in the game.
namespace shiftcheck
{
	void Sync(std::uintptr_t tes);  // the core's sync point (game-update thread), every frame
	void LoadReset();             // a load or transition: forget the baseline
	void BeforeShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);
}
