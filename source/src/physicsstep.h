#pragma once
#include <cmath>
#include <cstdint>

namespace physicsstep {
// FUN_1423CB260 consumes its third argument in XMM2. Preserve the native float exactly when forwarding.
using Callback = std::uint64_t (*)(std::uintptr_t vehicle, std::uintptr_t context, float seconds);

// Custom control does not advance on paused, invalid or unusually long steps. The native callback still receives
// the unmodified value. Valid simulation intervals have no wall-clock-dependent floor, clamp or fallback.
inline double ModelSeconds(float seconds) {
    return std::isfinite(seconds) && seconds > 0.0f && seconds <= 0.25f ? double(seconds) : 0.0;
}
}
