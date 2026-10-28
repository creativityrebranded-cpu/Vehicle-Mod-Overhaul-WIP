#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "ringschedule.h"

// City ring traffic: each traffic vehicle's home spot (2026-10-24, the user: "use the placements I gave as landing pads, one for
// each level"). The user's recorded New Atlantis spots (own_spots.h) lie on two levels of the city; the lower ring's vehicles
// (even k) get the lower level's spots and the upper ring's (odd k) the upper level's, one spot each, in the order of the spots'
// recorded keys. scripts/make_traffic_esm.py places the vehicles with the very same rule (it reads own_spots.h), so the
// plugin's vehicles and the DLL's schedule agree. Pure: tests/ringschedule_test.cpp.
namespace trafficspots {
struct In { double x = 0, y = 0, z = 0, heading = 0; std::uint64_t key = 0; };

// homes[k] for k < ringschedule::kVehicles; a level with too few spots leaves the rest without one (they stay on the ring).
inline void Assign(const In* spots, int n, ringschedule::Spot (&homes)[ringschedule::kVehicles]) {
    for (auto& h : homes) h = {};
    if (!spots || n <= 0) return;
    double lo = spots[0].z, hi = spots[0].z;
    for (int i = 0; i < n; ++i) { lo = std::min(lo, spots[i].z); hi = std::max(hi, spots[i].z); }
    const double split = (lo + hi) / 2;  // between the two levels (one level only: everything is "lower")
    std::vector<In> levels[2];
    for (int i = 0; i < n; ++i) levels[hi - lo > 10 && spots[i].z > split ? 1 : 0].push_back(spots[i]);
    for (auto& l : levels) std::sort(l.begin(), l.end(), [](const In& a, const In& b) { return a.key < b.key; });
    for (int k = 0; k < ringschedule::kVehicles; ++k) {
        const auto& l = levels[k % 2];
        const std::size_t i = std::size_t(k / 2);
        if (i < l.size()) homes[k] = { true, l[i].x, l[i].y, l[i].z, l[i].heading };
    }
}
}
