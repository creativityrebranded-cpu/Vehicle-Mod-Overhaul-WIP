#pragma once
// Hazard picking and placement maths for the F7 route editor (2026-11-22). Pure: no game, no editor state. The editor (routeedit.cpp, hazardedit.inl)
// projects a Hazard's eight corners with its own camera and asks here whether a click touches it; the two-click placement, the height / base
// maths and the index bookkeeping after a delete live here so they can be tested. Data model stays routehazard.h (unchanged).
#include "routehazard.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace hazardpick {
struct P2 { double x = 0, y = 0; };
constexpr double kPickSlack = 10.0;  // pixels around the outline that still count as a hit

// The eight corners of a box, bottom four then top four, in world metres (x east, y north, yaw clockwise from north).
inline void Corners3(const routehazard::Box& b, double out[8][3]) {
    const double r = b.yaw * routehazard::kPi / 180.0, s = std::sin(r), k = std::cos(r);
    static const int sg[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (int i = 0; i < 4; ++i) {
        const double u = sg[i][0] * b.hx, v = sg[i][1] * b.hy, x = b.x + u * k + v * s, y = b.y - u * s + v * k;
        out[i][0] = x; out[i][1] = y; out[i][2] = b.z - b.hz;
        out[i + 4][0] = x; out[i + 4][1] = y; out[i + 4][2] = b.z + b.hz;
    }
}
// Projects the corners with `project(x, y, z, sx, sy) -> bool`. Corners that do not project (behind the camera) are skipped, so a box
// that is partly off the view is still drawn and picked from what is visible (it used to vanish when any corner failed). Returns the count.
template <class Project> int ProjectCorners(const routehazard::Box& b, Project&& project, P2 out[8]) {
    double c[8][3]; Corners3(b, c); int n = 0;
    for (int i = 0; i < 8; ++i) { double sx = 0, sy = 0; if (project(c[i][0], c[i][1], c[i][2], sx, sy)) out[n++] = { sx, sy }; }
    return n;
}
inline double Cross(P2 o, P2 a, P2 b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }
// Convex hull (monotone chain), counter-clockwise, at most 2n points.
inline std::vector<P2> Hull(std::vector<P2> p) {
    std::sort(p.begin(), p.end(), [](P2 a, P2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    p.erase(std::unique(p.begin(), p.end(), [](P2 a, P2 b) { return a.x == b.x && a.y == b.y; }), p.end());
    if (p.size() < 3) return p;
    std::vector<P2> h(2 * p.size()); std::size_t k = 0;
    for (std::size_t i = 0; i < p.size(); ++i) { while (k >= 2 && Cross(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
    for (std::size_t i = p.size() - 1, t = k + 1; i > 0; --i) { while (k >= t && Cross(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k; h[k++] = p[i - 1]; }
    h.resize(k - 1); return h;
}
inline double SegDistance(P2 p, P2 a, P2 b) {
    const double dx = b.x - a.x, dy = b.y - a.y, l = dx * dx + dy * dy, t = l > 1e-12 ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / l, 0.0, 1.0) : 0.0;
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}
inline double Area(const std::vector<P2>& h) {
    double a = 0; for (std::size_t i = 0, j = h.size() - 1; i < h.size(); j = i++) a += h[j].x * h[i].y - h[i].x * h[j].y; return std::fabs(a) * .5;
}
// Distance from the click to the box's screen footprint (its hull): 0 inside, else to the nearest outline. -1 when nothing of it is visible.
inline double HullDistance(const std::vector<P2>& h, double mx, double my) {
    if (h.empty()) return -1;
    const P2 m{ mx, my };
    if (h.size() == 1) return std::hypot(h[0].x - mx, h[0].y - my);
    double best = 1e18; bool inside = h.size() >= 3;
    for (std::size_t i = 0, j = h.size() - 1; i < h.size(); j = i++) { best = std::min(best, SegDistance(m, h[j], h[i])); if (h.size() >= 3 && Cross(h[j], h[i], m) < 0) inside = false; }
    return inside ? 0.0 : best;
}
// Which box does a click at (mx, my) touch? Within `slack` pixels of its footprint. Several: the one whose footprint is smallest wins (a small
// box lying on a large one stays selectable), then the nearer. -1 for none.
template <class Project> int Pick(const std::vector<routehazard::Box>& boxes, Project&& project, double mx, double my, double slack = kPickSlack) {
    int best = -1; double bestArea = 1e18, bestD = 1e18;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        P2 pts[8]; const int n = ProjectCorners(boxes[i], project, pts); if (!n) continue;
        const auto h = Hull(std::vector<P2>(pts, pts + n)); const double d = HullDistance(h, mx, my);
        if (d < 0 || d > slack) continue;
        const double a = h.size() >= 3 ? Area(h) : 0.0;
        if (best < 0 || d < bestD - 1e-9 || (std::fabs(d - bestD) <= 1e-9 && a < bestArea)) { bestD = d; bestArea = a; best = int(i); }
    }
    return best;
}

// ---- height and base: a box is z (centre) and hz (half height); "base" is z - hz, "height" is 2 hz ----
inline double Base(const routehazard::Box& b) { return b.z - b.hz; }
inline double Height(const routehazard::Box& b) { return 2 * b.hz; }
inline void SetHeight(routehazard::Box& b, double height) { const double base = Base(b); b.hz = std::clamp(height * .5, routehazard::kMinHalf, routehazard::kMaxHalf); b.z = base + b.hz; }  // the base stays, the top moves
inline void SetBase(routehazard::Box& b, double base) { b.z = base + b.hz; }                                                                                                               // the height stays
inline void Raise(routehazard::Box& b, double dz) { b.z += dz; }
constexpr double kRaiseStep = 1.0, kRaiseBig = 5.0;  // PgUp / PgDn (and the wheel near it): the editor's 5 / 25 m steps are too coarse for a box, same 1 : 5 shape

// ---- the two-click placement (the user's flow): click 1 sets it down, click 2 sets length and direction, Escape cancels ----
struct Placing { bool active = false; double x = 0, y = 0, base = 0; };
constexpr double kDefaultAcross = 3.0, kDefaultHeight = 4.0, kMinLine = .5;
inline double YawOf(double dx, double dy) { double a = std::atan2(dx, dy) * 180.0 / routehazard::kPi; if (a < 0) a += 360.0; return a; }  // clockwise from north
// The box a first click at (x0, y0) and the cursor at (x1, y1) describe: it runs from the first click to the cursor, across and height as given.
inline routehazard::Box FromLine(double x0, double y0, double base, double x1, double y1, double across, double height, double fallbackYaw) {
    routehazard::Box b; const double dx = x1 - x0, dy = y1 - y0, len = std::hypot(dx, dy);
    if (len < kMinLine) { b.x = x0; b.y = y0; b.hy = routehazard::kDefaultHalfXY; b.yaw = fallbackYaw; }  // a second click on the first: the default size, last heading
    else { b.x = (x0 + x1) * .5; b.y = (y0 + y1) * .5; b.hy = len * .5; b.yaw = YawOf(dx, dy); }
    b.hx = across * .5; b.hz = height * .5; b.z = base + b.hz;
    return routehazard::Clamped(b);
}
inline void Begin(Placing& p, double x, double y, double base) { p.active = true; p.x = x; p.y = y; p.base = base; }
inline void Cancel(Placing& p) { p = Placing{}; }  // Escape, leaving the tool, a route change
// The second click: the finished box, and the state ends. Without a first click it does nothing (callers begin instead).
inline bool Commit(Placing& p, double x1, double y1, double across, double height, double fallbackYaw, routehazard::Box& out) {
    if (!p.active) return false;
    out = FromLine(p.x, p.y, p.base, x1, y1, across, height, fallbackYaw); Cancel(p); return true;
}

// ---- selection bookkeeping: picks are box indices of the shown route ----
// A box at `erased` is gone: later picks move one down, a pick of the erased one disappears.
inline void AfterErase(std::vector<int>& picks, int erased) {
    picks.erase(std::remove(picks.begin(), picks.end(), erased), picks.end());
    for (int& i : picks) if (i > erased) --i;
}
inline int AfterEraseOne(int selected, int erased) { return selected == erased ? -1 : selected > erased ? selected - 1 : selected; }
// A group move or turn of the editor (dx, dy, dz, a turn about (cx, cy) clockwise degrees): the box's position follows, its heading turns with it.
inline void GroupTurn(routehazard::Box& b, double dx, double dy, double dz, double cx, double cy, double clockwiseDeg) {
    if (clockwiseDeg != 0) {
        const double r = clockwiseDeg * routehazard::kPi / 180.0, s = std::sin(r), c = std::cos(r), ox = b.x - cx, oy = b.y - cy;
        b.x = cx + ox * c + oy * s; b.y = cy - ox * s + oy * c;   // clockwise seen from above
        b.yaw = std::fmod(b.yaw + clockwiseDeg + 720.0, 360.0);
    }
    b.x += dx; b.y += dy; b.z += dz;
}
}
