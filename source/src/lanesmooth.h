#pragma once
// LF-1 (2026-11-12, notes/logic-fix-20261112/HAIRPIN.md): the line a car follows must never fold or kink. The 13:56 run's lane at
// lc_ring_road's hairpin (an outer-lane offset of the road's centre line) turned from -92 to +13 degrees within 1 m and rose at 41 degrees
// there. That one kink tripped "steep ahead" (Mode flapped rail/flight every 1.5 s), set the corner speed near 0, and left the car 2.5 m off
// its line at the rock. Route and Speed read a line, so the line is repaired once, where every lane command enters the core: fold points
// are removed and corners sharper than kRoundTurn get a short fillet. A line with no fold and no sharp corner is returned unchanged.
// Pure; tests: tests/lanesmooth_test.cpp.
#include <algorithm>
#include <cmath>

namespace lanesmooth {
inline constexpr double kFoldTurn = 100.0 * 3.14159265358979 / 180.0;  // a turn this sharp next to a short segment is a fold: the point goes
inline constexpr double kFoldShort = 4.0;                               // m: "short" for a fold
inline constexpr double kRoundTurn = 35.0 * 3.14159265358979 / 180.0;  // corners sharper than this get a fillet
inline constexpr double kCutMax = 2.5, kCutShare = .4;                 // the fillet cuts at most this far, and this share of either segment
struct Result { int removed = 0, rounded = 0; double worstTurnDeg = 0; };

template <class P> double Turn(const P& a, const P& b, const P& c, double* l1 = nullptr, double* l2 = nullptr) {
    const double ax = b.x - a.x, ay = b.y - a.y, bx = c.x - b.x, by = c.y - b.y, la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (l1) *l1 = la; if (l2) *l2 = lb;
    if (la < 1e-6 || lb < 1e-6) return 0;
    return std::acos(std::clamp((ax * bx + ay * by) / (la * lb), -1.0, 1.0));
}

// P has x, y, z. `n` is updated; `cap` is the array's size.
template <class P> Result Repair(P* p, int& n, int cap) {
    Result r;
    // 1. Duplicates and folds: a point where the line doubles back next to a short segment.
    for (int pass = 0; pass < 64 && n >= 3; ++pass) {
        int drop = -1;
        for (int i = 1; i + 1 < n; ++i) {
            double l1 = 0, l2 = 0; const double t = Turn(p[i - 1], p[i], p[i + 1], &l1, &l2);
            r.worstTurnDeg = std::max(r.worstTurnDeg, t * 57.29578);
            if (l1 < .05 || (t > kFoldTurn && std::min(l1, l2) < kFoldShort)) { drop = i; break; }
        }
        if (drop < 0) break;
        for (int k = drop; k + 1 < n; ++k) p[k] = p[k + 1];
        --n; ++r.removed;
    }
    // 2. Sharp corners: replace the corner point by two points cut back along each segment (a short fillet), heights interpolated.
    for (int i = 1; i + 1 < n && n + 1 <= cap; ++i) {
        double l1 = 0, l2 = 0; const double t = Turn(p[i - 1], p[i], p[i + 1], &l1, &l2);
        if (t <= kRoundTurn) continue;
        const double c1 = std::min(kCutMax, kCutShare * l1), c2 = std::min(kCutMax, kCutShare * l2);
        if (c1 < .05 || c2 < .05) continue;
        const P a = p[i - 1], b = p[i], c = p[i + 1];
        P in = b, out = b;
        in.x = b.x - (b.x - a.x) * c1 / l1; in.y = b.y - (b.y - a.y) * c1 / l1; in.z = b.z - (b.z - a.z) * c1 / l1;
        out.x = b.x + (c.x - b.x) * c2 / l2; out.y = b.y + (c.y - b.y) * c2 / l2; out.z = b.z + (c.z - b.z) * c2 / l2;
        for (int k = n; k > i + 1; --k) p[k] = p[k - 1];
        p[i] = in; p[i + 1] = out; ++n; ++r.rounded; ++i;  // the two new points are not rounded again
    }
    return r;
}
}
