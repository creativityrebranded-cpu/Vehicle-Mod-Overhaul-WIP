#pragma once
// Hazard boxes (2026-11-21, user: custom bounding boxes "the same system walls use"). A route owns a list of oriented boxes in its own
// coordinates. They carry no game object: the planner reads them as World shapes (planworld::Source::Box, the shape kept collision boxes
// already use) and the far-rail path checks them with the same box. Pure; tests: tests/routehazard_test.cpp.
//
// File key, additive: `hazard_boxes = x y z hx hy hz yaw ; x y z hx hy hz yaw ; ...` (metres; x y z the centre, hx hy hz half sizes across,
// along and up; yaw degrees clockwise from north, the heading convention of every other route key). A route without the key reads exactly as
// before. Text this build cannot read is kept whole and written back unchanged, so a newer file is never damaged by an older build.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace routehazard {
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kMaxBoxes = 64;
constexpr double kMinHalf = .25, kMaxHalf = 200.0;       // half sizes, metres
constexpr double kDefaultHalfXY = 1.5, kDefaultHalfZ = 2.0;  // a small box placed with one click
constexpr double kPlannerMargin = .3;                     // clearance added on every side on top of the vehicle's own half width

struct Box { double x = 0, y = 0, z = 0, hx = kDefaultHalfXY, hy = kDefaultHalfXY, hz = kDefaultHalfZ, yaw = 0; };
struct Plan {
    std::vector<Box> boxes;
    std::string unsupported;  // text this build cannot read, written back as it was
};

inline bool Finite(double v) { return std::isfinite(v) && std::fabs(v) < 1e8; }
inline bool Valid(const Box& b) {
    return Finite(b.x) && Finite(b.y) && Finite(b.z) && Finite(b.yaw) && std::fabs(b.yaw) <= 720 &&
           b.hx >= kMinHalf && b.hy >= kMinHalf && b.hz >= kMinHalf && b.hx <= kMaxHalf && b.hy <= kMaxHalf && b.hz <= kMaxHalf;
}
inline Box Clamped(Box b) {
    b.hx = std::clamp(b.hx, kMinHalf, kMaxHalf); b.hy = std::clamp(b.hy, kMinHalf, kMaxHalf); b.hz = std::clamp(b.hz, kMinHalf, kMaxHalf);
    b.yaw = std::fmod(b.yaw, 360.0); if (b.yaw < 0) b.yaw += 360.0;
    return b;
}
inline std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
inline std::string Num(double v) { std::ostringstream o; o << std::setprecision(17) << v; return o.str(); }

// The key as read from the file (empty = missing). Any entry this build cannot read keeps the whole text, so nothing is dropped.
inline Plan Read(const std::string& text) {
    Plan p; const std::string t = Trim(text); if (t.empty()) return p;
    std::vector<Box> out; std::istringstream all(t); std::string part; bool ok = true;
    while (ok && std::getline(all, part, ';')) {
        if (Trim(part).empty()) continue;
        std::istringstream in(part); Box b; std::string extra;
        if (!(in >> b.x >> b.y >> b.z >> b.hx >> b.hy >> b.hz >> b.yaw) || (in >> extra) || !Valid(b) || out.size() >= kMaxBoxes) ok = false; else out.push_back(b);
    }
    if (ok) p.boxes = std::move(out); else p.unsupported = text;
    return p;
}
inline std::string Lines(const Plan& p) {
    if (!p.unsupported.empty()) return "hazard_boxes = " + Trim(p.unsupported) + "\n";
    if (p.boxes.empty()) return {};
    std::string out = "hazard_boxes = ";
    for (std::size_t i = 0; i < p.boxes.size(); ++i) {
        const auto& b = p.boxes[i];
        if (i) out += " ; ";
        out += Num(b.x) + " " + Num(b.y) + " " + Num(b.z) + " " + Num(b.hx) + " " + Num(b.hy) + " " + Num(b.hz) + " " + Num(b.yaw);
    }
    return out + "\n";
}
// An anchored city laid down elsewhere: the boxes move with the route's points.
inline void Shift(Plan& p, double dx, double dy, double dz) { for (auto& b : p.boxes) { b.x += dx; b.y += dy; b.z += dz; } }

// ---- geometry (x east, y north, yaw clockwise from north: a box's "along" axis points (sin yaw, cos yaw)) ----
struct Local { double across, along; };
inline Local ToLocal(const Box& b, double x, double y) {
    const double r = b.yaw * kPi / 180.0, s = std::sin(r), c = std::cos(r), dx = x - b.x, dy = y - b.y;
    return { dx * c - dy * s, dx * s + dy * c };
}
inline double Lo(const Box& b) { return b.z - b.hz; }
inline double Hi(const Box& b) { return b.z + b.hz; }
// Does a body of `halfWidth` standing at (x, y) over the heights zLo..zHi touch the box? (Circle-vs-box in the plane, height ranges overlap.)
inline bool Touches(const Box& b, double x, double y, double zLo, double zHi, double halfWidth) {
    if (zHi < Lo(b) || zLo > Hi(b)) return false;
    const auto l = ToLocal(b, x, y);
    const double ex = std::max(0.0, std::fabs(l.across) - b.hx), ey = std::max(0.0, std::fabs(l.along) - b.hy);
    return std::hypot(ex, ey) <= halfWidth;
}
inline bool TouchesAny(const Plan& p, double x, double y, double zLo, double zHi, double halfWidth) {
    for (const auto& b : p.boxes) if (Touches(b, x, y, zLo, zHi, halfWidth)) return true;
    return false;
}
// Walks a polyline of (x, y, z) points (with a lateral offset in metres, + to the right of travel) and returns the distance along it at which a
// body of `halfWidth` and `height` first touches a box, or a negative number when it never does. `step` is the sampling spacing.
struct P3 { double x, y, z; };
inline double FirstHit(const Plan& plan, const std::vector<P3>& path, double offset, double halfWidth, double height, double step = .5) {
    if (plan.boxes.empty() || path.size() < 2) return -1;
    double travelled = 0;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const double dx = path[i + 1].x - path[i].x, dy = path[i + 1].y - path[i].y, len = std::hypot(dx, dy);
        if (len < 1e-6) continue;
        const double ux = dx / len, uy = dy / len;  // right of travel is (uy, -ux)
        for (double a = 0; a <= len + 1e-9; a += step) {
            const double t = len > 0 ? std::min(1.0, a / len) : 0, z = path[i].z + (path[i + 1].z - path[i].z) * t;
            if (TouchesAny(plan, path[i].x + ux * a + uy * offset, path[i].y + uy * a - ux * offset, z - .2, z + height, halfWidth)) return travelled + a;
        }
        travelled += len;
    }
    return -1;
}
// The smallest sideways shift (searching right then left in `step` metres up to `maxOffset`) at which the body clears every box along the whole
// path, or `none` when no shift does. Used by the far rail: "a valid path around it if one is available, otherwise stop before the box".
constexpr double kNoDetour = 1e9;
inline double DetourOffset(const Plan& plan, const std::vector<P3>& path, double halfWidth, double height, double maxOffset, double step = .5) {
    if (FirstHit(plan, path, 0, halfWidth, height) < 0) return 0;
    for (double o = step; o <= maxOffset + 1e-9; o += step) {
        if (FirstHit(plan, path, o, halfWidth, height) < 0) return o;
        if (FirstHit(plan, path, -o, halfWidth, height) < 0) return -o;
    }
    return kNoDetour;
}
// Where a rail must stop: `distance` is where the body first touches a box; it stops `halfLength + gap` short of that.
inline double StopBefore(double distance, double halfLength, double gap = 2.0) { return std::max(0.0, distance - halfLength - gap); }

// Suggestions (Test route): fixed obstacles that intruded into the test vehicle's path. A new sighting merges into an existing suggestion that
// already covers its centre, so one object seen many times is one suggestion.
inline bool Covers(const Box& b, double x, double y, double z) { return Touches(b, x, y, z, z, 0.0); }
inline bool Merge(std::vector<Box>& list, const Box& seen, std::size_t cap = 16) {
    for (const auto& b : list) if (Covers(b, seen.x, seen.y, seen.z)) return false;
    if (list.size() >= cap) return false;
    list.push_back(seen); return true;
}
// ---- the virtual driver (far rail, lane carried without physics) ----
// A car carried along a fixed path cannot steer by physics, so it asks here each step: shift sideways round the boxes if a valid shift exists,
// otherwise slow to a stop short of them. `pointAt(station)` gives the path point (any callable returning something with x, y, z).
struct Advice { double offset = 0; double speedCap = 1e9; bool detour = false, blocked = false; double stopIn = -1; };
constexpr double kRailMaxOffset = 4.0, kRailOffsetRate = 2.5, kRailStopGap = 2.0, kRailBrake = 4.0;
template <class PointAt>
inline Advice Advise(const Plan& plan, PointAt&& pointAt, double station, double end, double speed, double halfWidth, double halfLength, double height,
                     double currentOffset, double dt) {
    Advice a; a.offset = currentOffset;
    const auto approach = [&](double target) {
        const double step = kRailOffsetRate * std::max(0.0, dt), d = target - a.offset;
        a.offset = std::fabs(d) <= step ? target : a.offset + (d > 0 ? step : -step);
    };
    if (plan.boxes.empty() || !(end > station)) { approach(0); return a; }
    const double behind = std::min(station, halfLength + 1.0), look = std::clamp(speed * 5.0 + 25.0, 30.0, 120.0);
    std::vector<P3> path;
    for (double s = station - behind; s <= std::min(end, station + look) + 1e-9; s += 1.0) { const auto p = pointAt(s); path.push_back({ p.x, p.y, p.z }); }
    if (path.size() < 2) { approach(0); return a; }
    const double clearW = halfWidth + kPlannerMargin;
    const double detour = DetourOffset(plan, path, clearW, height, kRailMaxOffset);
    if (detour != kNoDetour) { approach(detour); a.detour = detour != 0; return a; }
    // No valid way round: hold the line and stop short of the first contact.
    approach(0);
    const double hit = FirstHit(plan, path, 0, clearW, height);
    if (hit < 0) return a;
    const double room = std::max(0.0, hit - behind - halfLength - kRailStopGap);
    a.blocked = true; a.stopIn = room; a.speedCap = std::sqrt(2.0 * kRailBrake * room);
    return a;
}
// ---- suggestions (Test route) ----
// Grows box `a` to hold everything of box `b` too (in a's own frame; a keeps its yaw).
inline void Grow(Box& a, const Box& b) {
    double lo[2] = { -a.hx, -a.hy }, hi[2] = { a.hx, a.hy };
    const double r = (b.yaw - a.yaw) * kPi / 180.0, c = std::cos(r), s = std::sin(r);
    for (int i = 0; i < 4; ++i) {
        const double u = (i & 1 ? 1 : -1) * b.hx, v = (i & 2 ? 1 : -1) * b.hy;
        const Local l = ToLocal(a, b.x + (u * std::cos(b.yaw * kPi / 180.0) + v * std::sin(b.yaw * kPi / 180.0)), b.y + (-u * std::sin(b.yaw * kPi / 180.0) + v * std::cos(b.yaw * kPi / 180.0)));
        lo[0] = std::min(lo[0], l.across); hi[0] = std::max(hi[0], l.across); lo[1] = std::min(lo[1], l.along); hi[1] = std::max(hi[1], l.along);
    }
    (void)c; (void)s;
    const double zlo = std::min(Lo(a), Lo(b)), zhi = std::max(Hi(a), Hi(b)), ca = (lo[0] + hi[0]) * .5, al = (lo[1] + hi[1]) * .5, ay = a.yaw * kPi / 180.0;
    a.x += ca * std::cos(ay) + al * std::sin(ay); a.y += -ca * std::sin(ay) + al * std::cos(ay);
    a.hx = (hi[0] - lo[0]) * .5; a.hy = (hi[1] - lo[1]) * .5; a.z = (zlo + zhi) * .5; a.hz = (zhi - zlo) * .5;
}
// Does a body of `halfWidth` and `height` driving `path` touch this one box?
inline bool Intrudes(const Box& b, const std::vector<P3>& path, double halfWidth, double height) {
    Plan one; one.boxes.push_back(b); return FirstHit(one, path, 0, halfWidth, height, 1.0) >= 0;
}
}
