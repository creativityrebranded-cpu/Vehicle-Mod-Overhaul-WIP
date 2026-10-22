#pragma once
#include <algorithm>
#include <cstdint>

namespace healthlogic {
// A quiet interval starts after combat, not merely after the last successful hit.
// Clamp the healing interval to time actually spent beyond the quiet deadline.
inline double RepairSeconds(std::uint64_t now, std::uint64_t previous, std::uint64_t quietSince,
                            std::uint64_t delay, bool busy) {
    if (busy || !previous || now <= previous || now < quietSince || now - quietSince <= delay) return 0;
    const auto start = std::max(previous, quietSince + delay);
    return double(now - start) / 1000.0;
}
}
