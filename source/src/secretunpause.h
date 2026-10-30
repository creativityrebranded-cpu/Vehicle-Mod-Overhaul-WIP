#pragma once
// Secret unpause (2026-10-29, the user: "a pass when clicking that button to act as a secret unpause even though it's still
// hidden and then back to pause under the covers"). While the route editor's Start/Swap panel covers the local map, the
// world runs behind it so placed traffic is checked, seated and armed, then the map pauses the game again.
// Never in combat (a notice says so). Main thread only.
namespace secretunpause
{
    enum class Result { Started, InCombat, NotPaused, Unavailable };
    Result Begin();          // lift the pause of every open pausing menu (the game's own SetMenuPausesGame)
    void End(const char* why); // give every lifted pause back; safe to call any time, idempotent
    bool Active();
    bool PlayerInCombat();   // Start/Swap is refused in combat ("Exit combat first")
    void RefusedForCombat(); // after combat, prompt the player to go back and press Start/Swap
    void Reset();            // a load or world reset: pause given back, no stale prompt
    void Tick();             // ends it on combat, a closed map or the time cap
}
