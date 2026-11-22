#pragma once
// PF-1 (2026-11-21, user: "systems work but they're not performance grade"): the update's cost per part, measured in player
// builds too. main_vehicle.cpp writes one "Performance:" line a minute through logger::Important (release builds keep those).
// Parts are timed on the main thread (Scope); the energy plan's world gathering adds its time through the atomics.
#include <windows.h>
#include <atomic>

namespace costparts {

enum Part : int { FlightSnapshot, FlightEnv, FlightPlayer, FlightRails, FlightPilots, FlightBind,
                  PoolLook, PoolAddon, PoolWatch, PoolVisibility, kParts };

inline double g_frame[kParts] = {};  // this update, ms (main thread only)

// World gathering for the energy plan, ms since the last read (gather total; surface and its three pieces).
inline std::atomic<double> g_gatherMs{0}, g_surfaceMs{0}, g_surfProvMs{0}, g_surfBoxMs{0}, g_surfReachMs{0};
inline std::atomic<unsigned> g_gathers{0}, g_surfaceReused{0};
// Planning passes this minute (main thread): by interval (<=50 ms, <=100 ms, slower), near cars out of view slowed to 10 Hz,
// far cars out of view slowed to 2-4 Hz.
inline unsigned g_pass[3] = {}, g_passNearOut = 0, g_passFarOut = 0;

inline double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
}
struct Scope {
    Part p; LARGE_INTEGER a{};
    explicit Scope(Part part) : p(part) { QueryPerformanceCounter(&a); }
    ~Scope() { LARGE_INTEGER b; QueryPerformanceCounter(&b); g_frame[p] += Ms(a, b); }
    Scope(const Scope&) = delete; Scope& operator=(const Scope&) = delete;
};
inline void BeginFrame() { for (auto& v : g_frame) v = 0; }
inline void Add(std::atomic<double>& a, double v) {
    double o = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(o, o + v, std::memory_order_relaxed)) {}
}
inline void ResetMinute() {
    for (auto& v : g_pass) v = 0;
    g_passNearOut = g_passFarOut = 0;
    g_gathers = 0; g_surfaceReused = 0;
    g_surfaceMs = 0; g_surfProvMs = 0; g_surfBoxMs = 0; g_surfReachMs = 0;
}

}  // namespace costparts
