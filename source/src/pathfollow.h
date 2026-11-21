#pragma once
// AF-1 (2026-11-12, user: "Yes, unlock it"): the Controller follows the committed plan's PATH, forward and in reverse.
// Was (locked handoff, vehicleflight.cpp): forward aimed at one plan point 5-14 m ahead with a turn rate proportional to the heading
// error (2.5 x error), so holding a curve needed the nose already off it (about 5 deg at a 10 m radius, 2 m/s: the car ran 0.8-1.6 m
// wide at the hairpin); reverse set the target heading to the car's own heading, so every back-up went straight (steer 0 in every
// reverse sample of the 17:36 and 18:27 runs) and could never change the car's angle to a corner.
// Now: one law for both directions. The travel direction is the nose (forward) or the tail (reverse); the path is the plan's samples
// in the order the car will reach them. Turn rate = speed x the path's curvature where the car is (feedforward), plus the heading error
// and the lateral error brought back over a short distance (feedback). The output is the nose's turn rate and the nose bearing the
// existing rail and tyre stages already take; nothing below them changes (the engine write path stays as it is).
// Pure; tests: tests/pathfollow_test.cpp.
#include <algorithm>
#include <cmath>

namespace pathfollow {
constexpr int kMaxPoints = 32;
constexpr double kPi = 3.14159265358979323846;
inline double Wrap(double a) { return std::remainder(a, 2 * kPi); }

struct Gains {
    double heading = 1.4;     // 1/s per rad of heading error, scaled by speed (see Follow)
    double lateral = .9;      // rad of extra heading per (lateral error / correction length)
    double length = 3.0;      // m over which a lateral error is brought back (shorter = sharper)
    double maxRate = 1.6;     // rad/s, the nose turn rate never asked above this
};
struct Path { double x[kMaxPoints] = {}, y[kMaxPoints] = {}; int count = 0; };  // in the order the car reaches them
struct Out {
    bool ok = false;
    double yawRate = 0;       // rad/s, + = heading increasing (turning right), for the NOSE
    double bearing = 0;       // the nose heading to aim at (rail/tyre stages): forward the travel target, reverse its opposite
    double lateral = 0;       // m, + = path to the right of the travel direction
    double headingError = 0;  // rad, path direction minus travel direction
    double curvature = 0;     // 1/m of the path at the car, + = turning right along travel
    int segment = -1;
};

// heading: the nose (atan2(dx, dy), 0 = +y). speed: signed along the nose (- = backing). reverse: the plan backs up.
inline Out Follow(const Path& p, double x, double y, double heading, double speed, bool reverse, const Gains& g = {}) {
    Out o; if (p.count < 2) return o;
    const double travel = reverse ? heading + kPi : heading;
    // Nearest point on the path.
    double best = 1e18; int seg = 0; double t = 0;
    for (int i = 0; i + 1 < p.count; ++i) {
        const double dx = p.x[i + 1] - p.x[i], dy = p.y[i + 1] - p.y[i], l2 = dx * dx + dy * dy; if (l2 < 1e-9) continue;
        const double u = std::clamp(((x - p.x[i]) * dx + (y - p.y[i]) * dy) / l2, 0.0, 1.0);
        const double qx = p.x[i] + u * dx - x, qy = p.y[i] + u * dy - y, d2 = qx * qx + qy * qy;
        if (d2 < best) { best = d2; seg = i; t = u; }
    }
    const double dx = p.x[seg + 1] - p.x[seg], dy = p.y[seg + 1] - p.y[seg], len = std::hypot(dx, dy);
    const double psi = std::atan2(dx, dy);
    const double px = p.x[seg] + t * dx, py = p.y[seg] + t * dy;
    // + when the path lies to the right of the travel direction (right of travel = (cos, -sin) of the travel heading).
    o.lateral = (px - x) * std::cos(travel) - (py - y) * std::sin(travel);
    o.headingError = Wrap(psi - travel);
    // Curvature: the heading change between the neighbouring segments over their spacing (+ = turning right along the path).
    {   const int a = std::max(0, seg - 1), b = std::min(p.count - 2, seg + 1);
        if (b > a) {
            const double ha = std::atan2(p.x[a + 1] - p.x[a], p.y[a + 1] - p.y[a]), hb = std::atan2(p.x[b + 1] - p.x[b], p.y[b + 1] - p.y[b]);
            const double run = .5 * (std::hypot(p.x[a + 1] - p.x[a], p.y[a + 1] - p.y[a]) + std::hypot(p.x[b + 1] - p.x[b], p.y[b + 1] - p.y[b])) * (b - a);
            o.curvature = run > 1e-6 ? Wrap(hb - ha) / run : 0;
        } }
    (void)len;
    const double v = std::fabs(speed), vv = std::max(v, .5);
    // Travel-direction turn rate: follow the curve, close the heading error, bring a lateral offset back over g.length.
    const double correction = Wrap(o.headingError + std::atan(g.lateral * o.lateral / g.length));
    const double travelRate = vv * o.curvature + g.heading * std::min(vv, 4.0) * correction / 2.0;
    // The nose turns at the same rate as the travel direction (the tail's direction is the nose's, plus pi).
    o.yawRate = std::clamp(travelRate, -g.maxRate, g.maxRate);
    const double target = travel + correction;
    o.bearing = reverse ? target - kPi : target;
    o.ok = true; o.segment = seg;
    return o;
}

// Heading and yaw are positive to the right, unlike the physics body's world spin.
// Advance the heading error by the expected time to unwind the last wheel command.
// Use yaw relative to the lane's required turn, so damping does not fight steady cornering.
inline double DampedLaneYaw(const Out& follow, double speed, double rightYaw,
                             double sentSteer, double steerRate) {
    if (!follow.ok || !std::isfinite(rightYaw)) return follow.yawRate;
    const double v = std::fabs(speed);
    const double curveYaw = v * follow.curvature;
    const double unwind = std::min(.45, std::fabs(sentSteer) / steerRate);
    const double residualYaw = std::clamp(rightYaw - curveYaw, -1.6, 1.6);
    const double laneHeading = std::clamp(std::atan(.9 * follow.lateral / 4.5), -.32, .32);
    const double projectedError = Wrap(follow.headingError + laneHeading - (.20 + unwind) * residualYaw);
    const double correction = std::clamp(1.6 * std::min(v / 2.0, 1.0) * projectedError, -.75, .75);
    return std::clamp(curveYaw + correction, -1.6, 1.6);
}
}
