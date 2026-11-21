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
// T-1 (2026-11-07): the one owner of cover facts. fade: the game's fade-in (FaderMenu) still covers the screen after the loading
// screen (read-only cover.measure, main_vehicle.cpp); closedAt: when the loading screen last closed (GetTickCount64). The
// presentation API (Window's cover) is unchanged: the fade is not cover for removals (not fully opaque, no visible glitch).
inline std::atomic<bool> fade{false};
inline std::atomic<unsigned long long> closedAt{0};
// T-3b: the Start/Swap panel covers a running world, published by secretunpause at its transitions.
inline std::atomic<bool> panel{false};
// Hidden for the spawn health check: the loading screen, its fade-in, or the Start/Swap panel.
inline bool Hidden(){return Covered()||fade.load()||panel.load();}
inline void Progress(int total,int live){expected=total;ready=live;}
}
