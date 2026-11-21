#pragma once
// Road lanes at a point (2026-11-05, the user: "why not turn into another lane" -> "oncoming too, if clear"). The traffic addon knows
// every lane's edges (the road's dividers) and direction; the core's lane guidance carries only the car's own lane. This optional
// query gives the car the whole road at its position, so the path planner can pass through a neighbouring lane.
// Laterals are metres from the car's own lane centre line, + = right of its travel. A median gap between lanes is left out of both.
// Optional both ways: an addon without it leaves the core planning inside its own lane (as before); a core without the export is never
// offered it by the addon.
#include <cmath>
#include <cstdint>

struct FvoRoadLanesV1 {
    std::uint32_t size = sizeof(FvoRoadLanesV1), version = 1;
    int count = 0, own = -1;          // lanes across the road (left to right as listed by the addon); the car's own lane index
    double left[8] = {}, right[8] = {};  // each lane's edges, lateral from the own lane's centre (+ = right)
    int sameWay[8] = {};              // 1: runs the same way as the car's lane; 0: oncoming
    // Added with the route edge no-go bands (2026-11-19, additive: an older addon never writes them, they stay 0 = no band known).
    // The width in metres of the no-go band beyond the lowest / highest lateral lane edge above (the lane edges already exclude it).
    double bandLow = 0, bandHigh = 0;
};
inline constexpr std::uint32_t kFvoRoadLanesV1BaseSize = 8 + 8 + 8 * 8 * 2 + 4 * 8;  // size of the struct before bandLow/bandHigh (older callers' size)
static_assert(sizeof(FvoRoadLanesV1) == kFvoRoadLanesV1BaseSize + 16, "FvoRoadLanesV1 layout");
// 1 = answered, 0 = no road lanes here (not a road route, a junction or join, an open-air shaft).
using FvoRoadLanesProviderV1 = int (*)(void* owner, double x, double y, FvoRoadLanesV1* out);
using FvoRegisterRoadLanesV1 = int (*)(FvoRoadLanesProviderV1);

// Plausibility of a road-lane answer (the gate in vehicleflight.cpp). The lane edges already exclude the route edge no-go bands, so the outermost
// lane on a side with a band is trimmed by it: its width is judged with the band added back (never narrower than 1.0 m untrimmed), so a
// band-trimmed outer lane is not rejected as implausible. The upper bound and the lateral range judge the lane as given. Older callers' smaller
// struct (no band fields, `size` below the full struct) adds nothing. `badLane`/`widthM`/`untrimmedM`: the first failing lane (traced).
inline bool RoadLanesPlausible(const FvoRoadLanesV1& rl, int* badLane = nullptr, double* widthM = nullptr, double* untrimmedM = nullptr, bool* trimmedHelped = nullptr) {
    if (trimmedHelped) *trimmedHelped = false;
    if (rl.count <= 0 || rl.count > 8 || rl.own < 0 || rl.own >= rl.count) return false;
    double lo = rl.left[0], hi = rl.right[0];
    for (int k = 1; k < rl.count; ++k) { lo = std::fmin(lo, rl.left[k]); hi = std::fmax(hi, rl.right[k]); }
    const bool haveBands = rl.size >= sizeof(FvoRoadLanesV1);
    const double bl = haveBands && std::isfinite(rl.bandLow) && rl.bandLow > 0 && rl.bandLow <= 50 ? rl.bandLow : 0;
    const double bh = haveBands && std::isfinite(rl.bandHigh) && rl.bandHigh > 0 && rl.bandHigh <= 50 ? rl.bandHigh : 0;
    if (!(rl.left[rl.own] <= 0.5 && rl.right[rl.own] >= -0.5)) return false;
    for (int k = 0; k < rl.count; ++k) {
        const double w = rl.right[k] - rl.left[k];
        const double full = w + (rl.left[k] == lo ? bl : 0.0) + (rl.right[k] == hi ? bh : 0.0);  // untrimmed
        const bool bad = !std::isfinite(w) || full < 1.0 || w > 20.0 || std::fabs(rl.left[k]) > 40 || std::fabs(rl.right[k]) > 40;
        if (bad) { if (badLane) *badLane = k; if (widthM) *widthM = w; if (untrimmedM) *untrimmedM = full; return false; }
        if (w < 1.0 && trimmedHelped) *trimmedHelped = true;
    }
    return true;
}
