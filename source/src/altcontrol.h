#pragma once
#include <cstdint>

namespace altcontrol {
inline constexpr std::uint64_t kHoldMs = 300;
struct Action { bool cycle = false, drop = false; };
struct Input {
    bool down = false, longPress = false, blocked = true;
    std::uint64_t pressedAt = 0;

    Action Update(bool held, bool enabled, std::uint64_t now) {
        if (!enabled) {
            down = longPress = false;
            blocked = held;
            return {};
        }
        // Entering a vehicle or returning focus while Alt is held needs a fresh press.
        if (blocked) { blocked = held; return {}; }
        if (held) {
            if (!down) { down = true; pressedAt = now; }
            if (now - pressedAt >= kHoldMs) longPress = true;
            return {false, longPress};
        }
        const bool cycle = down && !longPress && now - pressedAt < kHoldMs;
        down = longPress = false;
        return {cycle, false};
    }
};
inline int NextGear(int gear) { return gear >= 1 && gear < 4 ? gear + 1 : 1; }
}
