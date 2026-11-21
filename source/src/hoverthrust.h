#pragma once
// The hover actuator's thrust split (removal 2026-11-14): the plan's travel (bearing + speed) as forward and sideways thrust for a body
// whose nose points elsewhere (combat facing, the hover follower's held nose). Pure math, no decision. Moved out of the old hover plan
// follower (planhover.h, removed 2026-11-14, kept outside FVO in fvo-removed-20261114/deciders); name kept for the callers.
#include <algorithm>
#include <cmath>

namespace planhover {
struct Thrust { double throttle = 0, strafe = 0; bool brake = false; };
// travelBearing/speed: the plan's travel; heading: the body's nose now; vx, vy: the body's velocity; maxSpeed: the hover's top speed.
inline Thrust SplitTravel(double travelBearing, double speed, double heading, double vx, double vy, double maxSpeed) {
    Thrust t;
    const double wx = std::sin(travelBearing) * speed, wy = std::cos(travelBearing) * speed;
    const double fx = std::sin(heading), fy = std::cos(heading), rx = std::cos(heading), ry = -std::sin(heading);
    const double wantF = wx * fx + wy * fy, wantR = wx * rx + wy * ry, haveF = vx * fx + vy * fy, haveR = vx * rx + vy * ry;
    t.throttle = std::clamp((wantF - haveF) / 6.0 + wantF / std::max(1.0, maxSpeed) * 0.3, -1.0, 1.0);
    t.strafe = std::clamp((wantR - haveR) / 5.0, -1.0, 1.0);
    t.brake = wantF < haveF - 8.0 && haveF > 3.0;
    return t;
}
}
