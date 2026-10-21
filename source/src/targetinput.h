#pragma once
#include <cstdint>
// The scanner key's gestures (2026-10-20, the user): a single tap selects what is aimed at, a double tap clears the route, a hold arms the
// vehicle for a ram. A single tap is reported only once the double-tap window has passed, so a double tap never briefly starts a trip or ram.
// `capture` marks the press whose aimed selection the later tap uses; a second press or a hold never replaces it.
namespace targetinput {
inline constexpr std::uint64_t kDoubleMs = 300, kHoldMs = 1000;
struct Action { bool tap = false, doubleTap = false, hold = false, capture = false; };
struct Input {
    bool down = false, consumed = false, pending = false, blocked = true;
    std::uint64_t pressedAt = 0, releasedAt = 0;
    void Cancel(bool held) { down = consumed = pending = false; blocked = held; }
    Action Update(bool held, bool enabled, std::uint64_t now) {
        if (!enabled) { Cancel(held); return {}; }
        // Focus returning or the key becoming usable while T is already down needs a fresh press.
        if (blocked) { blocked = held; return {}; }
        Action a;
        if (held) {
            if (!down) {
                down = true; pressedAt = now;
                if (pending && now - releasedAt < kDoubleMs) { pending = false; consumed = true; a.doubleTap = true; return a; }
                if (pending) { pending = false; a.tap = true; }  // a frame gap passed the window: the earlier tap still counts
                consumed = false; a.capture = true;
                return a;
            }
            if (!consumed && now - pressedAt >= kHoldMs) { consumed = true; a.hold = true; }
            return a;
        }
        if (down) {
            down = false;
            if (!consumed) {
                if (now - pressedAt >= kHoldMs) a.hold = true;  // a missed frame still counts as the hold, never as a selection
                else { pending = true; releasedAt = now; }
            }
            consumed = false;
            return a;
        }
        if (pending && now - releasedAt >= kDoubleMs) { pending = false; a.tap = true; }
        return a;
    }
};
}
