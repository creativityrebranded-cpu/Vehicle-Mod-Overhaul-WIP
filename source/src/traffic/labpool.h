#pragma once
// The factory lab as a pool keyed to the player's current cell (GTA-style local density).
// Pure rules only: the generator supplies the counts and distances, this header answers.
//   - a stock pair is "local" when the core can bind it in the player's cell, "stranded" when the player has left its cell;
//   - only local pairs (and births in flight) count against the pool, so stock left behind never starves the local lab;
//   - delivery fills route slots inside a density ring around the player, nearest first;
//   - a pair that never proves motion yields its lease and waits out a growing backoff so other pairs can qualify.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace labpool {
// Stock pairs kept under the player's cell. Raise to 24 for a denser lab; slots follow (2x, headroom for stranded pairs).
inline constexpr int kPoolSize = 8;
// Runtime stock slots (saved registry rows factory_v2_runtime_0..N-1). 16 with the default pool, the number older saves already use.
inline constexpr int kSlotCount = kPoolSize * 2 > 16 ? kPoolSize * 2 : 16;
static_assert(kPoolSize >= 1 && kSlotCount >= kPoolSize, "pool must fit in its slots");

// Density ring around the player: route slots closer than the inner radius are almost always in sight; beyond the outer
// radius the lab is too far to reach and the player will not meet that traffic soon. Measured on the ground plane.
inline constexpr double kRingInnerM = 25.0;
inline constexpr double kRingOuterM = 110.0;

inline constexpr std::uint64_t kYieldBackoffBaseMs = 15000;
inline constexpr std::uint64_t kYieldBackoffMaxMs = 60000;
inline constexpr std::uint64_t kRecycleEveryMs = 5000;   // one stranded pair retired per interval, at most
inline constexpr std::uint64_t kSeatEveryMs = 250;       // stock driver snap retry (fast seating took 220-250 ms in game)

struct Counts {
    int local = 0;     // stock pairs (or births in flight) the lab can use now
    int stranded = 0;  // stock pairs left in another cell
    int empty = 0;     // slots with nothing in them
};

inline bool InRing(double playerDistance) {
    return std::isfinite(playerDistance) && playerDistance >= kRingInnerM && playerDistance <= kRingOuterM;
}
// Delivery order: the slot nearest the player first.
inline bool NearestFirst(double a, double b) { return a < b; }

// A new stock birth needs room in the pool and an empty slot.
inline bool MayBirth(const Counts& c) { return c.local < kPoolSize && c.empty > 0; }
// The lab needs room for (pool - local) more births. When the empty slots cannot give it, a stranded pair is recycled.
inline bool NeedsRecycle(const Counts& c) { return c.stranded > 0 && c.local < kPoolSize && c.empty < kPoolSize - c.local; }

// How long a pair that missed its motion deadline stays out of the lease queue: 15 s, 30 s, 60 s, 60 s...
inline std::uint64_t YieldBackoffMs(int yields) {
    if (yields <= 0) return 0;
    const int shift = std::min(yields - 1, 8);
    return std::min<std::uint64_t>(kYieldBackoffBaseMs << shift, kYieldBackoffMaxMs);
}
}  // namespace labpool
