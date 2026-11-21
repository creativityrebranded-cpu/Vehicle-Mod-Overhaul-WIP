#pragma once
// Rail drive turns (2026-11-05). With the rail moving the car directly, turns no longer need the wheel-era limits (1.5 m/s at a
// 90 degree corner, the waypoint landing on the corner point). Two things replace them:
//   bearing: aim at a point `look` metres ahead along the lane centre (moved sideways by any passing offset), so the car drives a
//            smooth arc through a corner instead of to the corner and then round it;
//   corner speed: the lane's turning over a window ahead gives the arc radius that fits inside the lane; the speed is what a car
//            holds round that radius at `lateral` m/s^2, reached by braking at `brake` m/s^2 before it.
// Pure; tests: tests/railturn_test.cpp.
#include <algorithm>
#include <cmath>

namespace railturn {
constexpr double kPi = 3.141592653589793;
inline double Wrap(double a) { return std::remainder(a, 2 * kPi); }
struct Plan { bool ok = false; double bearing = 0, cornerCap = 1e9, look = 0, cornerAt = -1, radius = 0; };

// Path: count + points[i].x/.y. along: the car's distance along the path (its projection).
template<class Path, class AtFn>
Plan Pursue(const Path& path, AtFn&& at, double along, double x, double y, double speed, double offset,
            double laneHalf, double carHalfW, double lateral = 3.5, double brake = 4.0, double horizon = 90)
{
    Plan p;
    if (path.count < 2) return p;
    const double room = std::max(0.8, laneHalf - carHalfW - 0.3);   // sideways room either side of the centre line
    const double window = 8 + 2 * room;                                // a corner is judged over this much lane
    p.look = std::clamp(3 + 0.6 * std::fabs(speed), std::max(5.0, window * 0.6), 14.0);
    auto point = [&](double s) { const auto q = at(path, s); return std::pair<double, double>{ q.x, q.y }; };
    auto yawAt = [&](double s) { const auto a = point(s), b = point(s + 1.0); return std::atan2(b.first - a.first, b.second - a.second); };
    // Target ahead on the centre line, shifted by the passing offset (+ = right of travel: (cos, -sin) of the lane yaw, the lane driver's
    // convention; 2026-11-05: this was the other way round, so the rail aimed every pass at the side the lane driver had found blocked).
    const auto t = point(along + p.look); const double ty = yawAt(along + p.look);
    const double tx = t.first + std::cos(ty) * offset, tyy = t.second - std::sin(ty) * offset;
    p.bearing = std::atan2(tx - x, tyy - y);
    // Corner speed: turning over the window at each 2 m ahead.
    for (double s = 0; s <= horizon; s += 2) {
        const double turn = std::fabs(Wrap(yawAt(along + s + window) - yawAt(along + s)));
        if (turn < 0.05) continue;
        const double radius = std::max(2.5, window / turn);
        const double v = std::sqrt(lateral * radius);
        const double reach = std::max(0.0, s + window * 0.5 - p.look * 0.5);  // the arc starts about half a window before the bend
        const double cap = std::sqrt(v * v + 2 * brake * reach);
        if (cap < p.cornerCap) { p.cornerCap = cap; p.cornerAt = s + window * 0.5; p.radius = radius; }
    }
    p.ok = true;
    return p;
}
}
