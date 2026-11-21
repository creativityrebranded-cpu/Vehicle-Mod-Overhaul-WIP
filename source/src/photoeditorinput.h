#pragma once
#include <cstdint>

namespace photoeditorinput {
// Pure policy: the hold gesture belongs to the editor, never to PhotoMode's
// MoveUp / ToggleHelmet actions. Ctrl + right mouse deliberately permits lift
// (Shift is the 5x camera speed key, so it no longer lifts). Pad LB is the
// pad's speed key while editing, never PhotoMode's LShoulder tab switch.
inline bool Block(bool editing, bool navigating, bool lift,
                  unsigned device, unsigned kind, unsigned code) {
    if (!editing) return false;
    if (device == 0 && kind == 0 && code == 0x77) return true; // F8 (Traffic Manager key; F7 belongs to the UI)
    if (device == 2 && kind == 0 && (code == 0x40 || code == 0x80 || code == 0x100)) return true;
    if (device == 1 && kind == 0 && code == 1) return !navigating || !lift;
    if (!navigating) return true;
    // Camera hold (user, 2026-11-04: "F8 should completely cut off anything photomode regardless of what I press"):
    // only camera movement reaches photo mode. Keyboard W A S D; mouse look, LMB (down), wheel; pad sticks and
    // triggers. Every other photo action (Space photo, T menu, Z helmet, X head track, V reset, R mode, Q/E tabs,
    // Esc, arrows, pad face/shoulder/D-pad/View buttons) stays blocked.
    if (device == 0) return !(kind == 0 && (code == 'W' || code == 'A' || code == 'S' || code == 'D'));
    if (device == 1) return !(kind == 1 || (kind == 0 && (code == 0 || code == 8 || code == 9)));
    if (device == 2) return !(kind == 4 || (kind == 0 && (code == 9 || code == 10)));
    return true;
}
}
