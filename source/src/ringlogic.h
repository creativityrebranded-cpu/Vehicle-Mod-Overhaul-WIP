#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// City ring road (2026-10-24, the user: a circular road around New Atlantis that traffic flies round, with 4 exits).
// Stage 1: the ring's shape and its editing. An ellipse around (cx, cy) at height z: `radius` along its north axis,
// `radius * aspect` across, turned by `rotation` (clockwise, radians). Markers sit at kPoints even angles; the exits at
// the 4 quarter points (north, east, south, west before the rotation). Traffic flies clockwise. Pure math:
// tests/ringlogic_test.cpp.
namespace ringlogic {
constexpr int kPoints = 24, kExits = 4, kLayers = 2;  // two parallel rings: the lower flown clockwise, the upper counter-clockwise
constexpr double kPi = 3.14159265358979323846;
constexpr double kMinRadius = 100, kMaxRadius = 3000, kMinAspect = 0.3, kMaxAspect = 3;
constexpr double kDefaultRadius = 600, kDefaultHeight = 150;  // m; height above the given top (the highest street level)
constexpr double kDefaultGap = 40, kMinGap = 15, kMaxGap = 200;  // m between the two rings

struct Ring { double cx = 0, cy = 0, z = 0, radius = kDefaultRadius, aspect = 1, rotation = 0, gap = kDefaultGap; };  // z: the lower ring
struct Point { double x = 0, y = 0, z = 0, heading = 0; };  // heading: the clockwise travel direction, atan2(dx, dy)

inline double Wrap(double a) { return std::isfinite(a) ? std::remainder(a, 2 * kPi) : 0.0; }
inline bool Valid(const Ring& r) {
    return std::isfinite(r.cx) && std::isfinite(r.cy) && std::isfinite(r.z) && std::isfinite(r.rotation) &&
        r.radius >= kMinRadius && r.radius <= kMaxRadius && r.aspect >= kMinAspect && r.aspect <= kMaxAspect && r.gap >= kMinGap && r.gap <= kMaxGap;
}
inline Ring Clean(Ring r) {
    r.radius = std::clamp(std::isfinite(r.radius) ? r.radius : kDefaultRadius, kMinRadius, kMaxRadius);
    r.aspect = std::clamp(std::isfinite(r.aspect) ? r.aspect : 1.0, kMinAspect, kMaxAspect);
    r.rotation = Wrap(r.rotation);
    r.gap = std::clamp(std::isfinite(r.gap) ? r.gap : kDefaultGap, kMinGap, kMaxGap);
    return r;
}
// A ring around (x, y) whose height clears `top` (the highest street or roof the ring must pass over).
inline Ring Default(double x, double y, double top) { return Clean({ x, y, top + kDefaultHeight, kDefaultRadius, 1, 0, kDefaultGap }); }

// The point at angle `a` round the ring (0 = its north end, increasing clockwise).
inline Point At(const Ring& r, double a) {
    const double lx = std::sin(a) * r.radius * r.aspect, ly = std::cos(a) * r.radius;           // ring frame
    const double tx = std::cos(a) * r.radius * r.aspect, ty = -std::sin(a) * r.radius;          // d/da: clockwise travel
    const double c = std::cos(r.rotation), s = std::sin(r.rotation);                            // clockwise turn
    auto turn = [&](double x, double y, double& ox, double& oy) { ox = x * c + y * s; oy = -x * s + y * c; };
    Point p; double dx, dy;
    turn(lx, ly, p.x, p.y); turn(tx, ty, dx, dy);
    p.x += r.cx; p.y += r.cy; p.z = r.z; p.heading = std::atan2(dx, dy);
    return p;
}
// Layer 1 is the upper ring, `gap` above, flown the other way (counter-clockwise).
inline Point OnLayer(Point p, const Ring& r, int layer) {
    if (layer == 1) { p.z += r.gap; p.heading = Wrap(p.heading + kPi); }
    return p;
}
inline Point Marker(const Ring& r, int i, int layer = 0) { return OnLayer(At(r, 2 * kPi * i / kPoints), r, layer); }
inline Point Exit(const Ring& r, int k, int layer = 0) { return OnLayer(At(r, 2 * kPi * k / kExits), r, layer); }

enum class Edit { Wider, Narrower, Stretch, Squeeze, TurnLeft, TurnRight, Raise, Lower, Forward, Back, Left, Right, GapUp, GapDown };
// One edit. `big`: the Ctrl step. `facing`: the player's heading, so the move keys go where you look.
inline Ring Apply(Ring r, Edit e, bool big, double facing) {
    const double move = big ? 50 : 10, height = big ? 25 : 5, turn = (big ? 30.0 : 5.0) * kPi / 180, size = big ? 1.25 : 1.05;
    const double fx = std::sin(facing), fy = std::cos(facing), rx = std::cos(facing), ry = -std::sin(facing);
    switch (e) {
    case Edit::Wider: r.radius *= size; break;
    case Edit::Narrower: r.radius /= size; break;
    case Edit::Stretch: r.aspect *= size; break;
    case Edit::Squeeze: r.aspect /= size; break;
    case Edit::TurnLeft: r.rotation -= turn; break;
    case Edit::TurnRight: r.rotation += turn; break;
    case Edit::Raise: r.z += height; break;
    case Edit::Lower: r.z -= height; break;
    case Edit::Forward: r.cx += fx * move; r.cy += fy * move; break;
    case Edit::Back: r.cx -= fx * move; r.cy -= fy * move; break;
    case Edit::Left: r.cx -= rx * move; r.cy -= ry * move; break;
    case Edit::Right: r.cx += rx * move; r.cy += ry * move; break;
    case Edit::GapUp: r.gap += height; break;
    case Edit::GapDown: r.gap -= height; break;
    }
    return Clean(r);
}

// "cx cy z radius aspect rotationDegrees gap" (ringroad.ini; a line without the gap, from the one-ring version, gets the default).
inline std::string Format(const Ring& r) {
    char b[160];
    std::snprintf(b, sizeof(b), "%.1f %.1f %.1f %.1f %.3f %.1f %.1f", r.cx, r.cy, r.z, r.radius, r.aspect, r.rotation * 180 / kPi, r.gap);
    return b;
}
inline bool Parse(const std::string& text, Ring& out) {
    Ring r; double deg = 0;
    const int n = std::sscanf(text.c_str(), "%lf %lf %lf %lf %lf %lf %lf", &r.cx, &r.cy, &r.z, &r.radius, &r.aspect, &deg, &r.gap);
    if (n < 6) return false;
    if (n == 6) r.gap = kDefaultGap;
    r.rotation = deg * kPi / 180;
    if (!Valid(Clean(r)) || !std::isfinite(r.cx) || !std::isfinite(r.cy) || !std::isfinite(r.z)) return false;
    out = Clean(r);
    return true;
}
}
