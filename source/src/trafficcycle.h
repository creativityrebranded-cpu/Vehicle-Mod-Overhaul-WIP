#pragma once
#include <cstdint>

namespace trafficcycle {
// A continuous landed dwell, not time spent descending or looking for ground.
struct Dwell {
    std::uint64_t since = 0;
    bool waiting = false;
    bool Step(std::uint64_t now, bool landed) {
        if (!landed) { waiting = false; return false; }
        if (!waiting || now < since) { since = now; waiting = true; }
        return now - since >= 10000;
    }
};
inline bool Repeat(int result) { return result == 3 || result == 4; }
inline bool Destination(int result) { return result == 2 || result == 4; }
}
