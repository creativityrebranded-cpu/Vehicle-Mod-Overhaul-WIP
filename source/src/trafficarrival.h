#pragma once
#include <atomic>

// Arrival traffic is placed behind the game's own loading screen (2026-10-29, the user: "operation within the proper cover
// already there", not an extra cover and not a longer loading screen). The core opens the window while the loading screen is up,
// the save has been read and the world is ready; it closes when the game closes the screen. Nothing here delays the game.
namespace trafficarrival {
inline std::atomic<bool> window{false};
// The nav map (collision boxes of the loaded cells) finished a full pass after the loading screen opened: until then nothing is
// placed behind it, so the spawn check sees the city's buildings instead of an empty map (2026-10-30: vehicles placed blind
// behind the screen landed inside buildings).
inline std::atomic<bool> seeded{false};
inline std::atomic<int> expected{0}, ready{0};
inline void Reset(){window=false;seeded=false;expected=ready=0;}
inline void Window(bool open){window=open;}
inline bool Covered(){return window.load();}
inline void Progress(int total,int live){expected=total;ready=live;}
}
