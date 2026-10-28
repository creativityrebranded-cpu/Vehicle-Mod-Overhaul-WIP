#pragma once
// Interactive Cover, test stage (2026-10-23, notes/INTERACTIVE_COVER_PLAN.md): one test key ' (VK_OEM_7) that steps
// through the first in-game checks, each logged to InteractiveCover.log:
//   1 crouch      force-sneak bit on actor+0xF8 for 3 s, then back (what console SetForceSneak writes)
//   2 walk        forward input (PlayerControls data +4 = 1.0) written every frame for 1 s; logs speed
//   3 sprint+slide forward + sprint request (player+0x112C bit 4) 1.2 s, then crouch while sprinting; logs speed,
//                 movement state (+0xF8 bits 17-20) and whether the game's SlideStart handler fires
//   4 cover action console "player.pa ActionEnterCover" (the result shows in the console)
// Every test restores what it changed.
namespace covertest
{
	bool Install();   // the SlideStart handler watch (vtable slot); false: not hooked, the rest still works
	void Tick();      // per frame (main thread)
	void Reset();     // on load: end a running test, restore
}
