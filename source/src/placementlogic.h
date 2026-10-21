#pragma once
#include <cmath>
#include <cstdint>
#include <string>

// The placement patch's choice of a vehicle type for a parked spot (realvehicles.cpp). Pure: seed and stable keys in,
// an index out. Each location weights every type at random (a skewed spread, so each place has its own mix); each spot
// draws from it by weighted reservoir sampling (the max of ln(u) / w). Only stable keys go in (the spot from the plugin
// file, the location's worldspace, each type's model and file-local id), so the same spot always gets the same type, the
// load order does not matter, and a newly installed vehicle mod only changes the spots where its vehicles win.
namespace placementlogic {
inline std::uint64_t Mix(std::uint64_t x)  // splitmix64
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
inline double Unit(std::uint64_t x) { return (double(Mix(x) >> 11) + 0.5) / 9007199254740992.0; }  // (0, 1)
inline std::uint64_t Fnv(const std::string& s)
{
    std::uint64_t h = 1469598103934665603ull;
    for (const char c : s) h = (h ^ std::uint8_t(c)) * 1099511628211ull;
    return h;
}
// The location's weight for a type (> 0).
inline double Weight(std::uint64_t seed, std::uint64_t location, std::uint64_t type)
{
    return std::pow(-std::log(Unit(seed ^ Mix(location) ^ Mix(type * 3 + 1))), 2.0) + 1e-6;
}
// The index into types[0..count) the spot gets; -1 for an empty pool.
inline int Pick(std::uint64_t seed, std::uint64_t location, std::uint64_t spot, const std::uint64_t* types, int count)
{
    int best = -1;
    double bestScore = -1e300;
    for (int i = 0; i < count; ++i) {
        const double score = std::log(Unit(seed ^ Mix(spot) ^ Mix(types[i] * 5 + 2))) / Weight(seed, location, types[i]);
        if (score > bestScore) { bestScore = score; best = i; }
    }
    return best;
}
}
