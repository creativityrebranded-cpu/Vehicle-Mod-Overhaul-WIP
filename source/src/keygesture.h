#pragma once
#include <cstdint>
// Tap / hold on one key (2026-10-21, the user's control scheme: N tap your vehicle, N hold every escort too; U tap your vehicle covers,
// U hold a follower drives it). A tap is reported on release; a hold once, at kHoldMs (the same 1 s as T). Releasing a hold never also taps.
namespace keygesture {
inline constexpr std::uint64_t kHoldMs = 1000;
struct Action { bool tap = false, hold = false; };
struct Input {
    bool down = false, fired = false, blocked = true;
    std::uint64_t pressedAt = 0;
    Action Update(bool held, bool enabled, std::uint64_t now) {
        if (!enabled) { down = fired = false; blocked = held; return {}; }
        // Focus returning or a key already held when it becomes usable needs a fresh press.
        if (blocked) { blocked = held; return {}; }
        Action a;
        if (held) {
            if (!down) { down = true; fired = false; pressedAt = now; }
            if (!fired && now - pressedAt >= kHoldMs) { fired = true; a.hold = true; }
            return a;
        }
        if (down) {
            down = false;
            if (!fired) { if (now - pressedAt >= kHoldMs) a.hold = true; else a.tap = true; }  // a missed frame still counts as the hold
            fired = false;
        }
        return a;
    }
};
}
