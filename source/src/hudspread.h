#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// Keeps HUD text from sitting on top of other HUD text (2026-10-27, the user: "the words don't like to overlap over
// anything and would move each other if they got closer"). Boxes are on-screen rectangles in one coordinate space;
// overlapping ones are pushed apart along the axis that needs the smaller push (text lines are wide and short, so
// that is usually up/down), split between two movable boxes, all of it on the movable one when the other is fixed.
// The result is an offset per box from where the layout put it; the caller eases toward it each frame.
namespace hudspread {
struct Box { double x = 0, y = 0, w = 0, h = 0; bool movable = true; };
// The screen in the boxes' coordinates (2026-10-27, the user: "let it know the bounds of the screen too"): a push never
// moves a box past an edge (less `margin`); a box already partly off screen is brought back in. valid = false: no limit.
struct Screen { double x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool valid = false; double margin = 8.0; };

// Moves a movable box's offset so the box lies inside the screen (a box larger than the screen keeps its top-left in).
inline void KeepInside(const Box& b, std::array<double, 2>& off, const Screen& s)
{
    if (!s.valid || !b.movable) return;
    const double lo[2] = { s.x0 + s.margin, s.y0 + s.margin }, hi[2] = { s.x1 - s.margin, s.y1 - s.margin };
    const double at[2] = { b.x, b.y }, size[2] = { b.w, b.h };
    for (int a = 0; a < 2; ++a) {
        const double start = at[a] + off[a];
        if (start + size[a] > hi[a]) off[a] -= start + size[a] - hi[a];
        if (at[a] + off[a] < lo[a]) off[a] += lo[a] - (at[a] + off[a]);
    }
}

inline std::vector<std::array<double, 2>> Solve(const std::vector<Box>& in, double gap = 4.0, int iterations = 16, double maxShift = 160.0,
    const Screen& screen = {})
{
    std::vector<std::array<double, 2>> off(in.size(), { 0.0, 0.0 });
    for (int it = 0; it < iterations; ++it) {
        bool moved = false;
        for (std::size_t i = 0; i < in.size(); ++i) {
            for (std::size_t j = i + 1; j < in.size(); ++j) {
                const Box& a = in[i];
                const Box& b = in[j];
                if ((!a.movable && !b.movable) || a.w <= 0 || a.h <= 0 || b.w <= 0 || b.h <= 0) continue;
                const double ax = a.x + off[i][0], ay = a.y + off[i][1], bx = b.x + off[j][0], by = b.y + off[j][1];
                const double ox = std::min(ax + a.w, bx + b.w) - std::max(ax, bx) + gap;
                const double oy = std::min(ay + a.h, by + b.h) - std::max(ay, by) + gap;
                if (ox <= 0 || oy <= 0) continue;
                const bool alongY = oy <= ox;
                const double push = alongY ? oy : ox;
                // Direction: away from each other's centre (ties: the later box goes down / right).
                const double ca = alongY ? ay + a.h * 0.5 : ax + a.w * 0.5;
                const double cb = alongY ? by + b.h * 0.5 : bx + b.w * 0.5;
                const double dir = cb >= ca ? 1.0 : -1.0;
                const double shareA = a.movable && b.movable ? 0.5 : a.movable ? 1.0 : 0.0;
                const int axis = alongY ? 1 : 0;
                off[i][axis] -= dir * push * shareA;
                off[j][axis] += dir * push * (1.0 - shareA);
                moved = true;
            }
        }
        for (std::size_t i = 0; i < in.size(); ++i) {
            if (!in[i].movable) { off[i] = { 0.0, 0.0 }; continue; }
            for (double& v : off[i]) v = std::clamp(v, -maxShift, maxShift);
            const auto before = off[i];
            KeepInside(in[i], off[i], screen);
            if (off[i] != before) moved = true;
        }
        if (!moved) break;
    }
    // The screen edge has the last word (an unresolved overlap stays on screen rather than pushed off it).
    for (std::size_t i = 0; i < in.size(); ++i) KeepInside(in[i], off[i], screen);
    return off;
}

// One frame of easing toward the solved offset (fast enough to clear an overlap in a few frames, slow enough not to
// jitter when two lines graze each other).
inline double Ease(double current, double target, double rate = 0.3)
{
    const double next = current + (target - current) * rate;
    return std::fabs(next - target) < 0.5 ? target : next;
}
}
