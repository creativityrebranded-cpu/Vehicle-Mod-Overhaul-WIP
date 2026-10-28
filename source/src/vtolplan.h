#pragma once
#include <algorithm>
#include <cmath>
#include "vtolai.h"

// The route planner's motion model for a physical VTOL chassis (2026-10-24, the user: AI tuned for physical flight).
// The standard model's velocity follows the nose at once (its grip); the physical chassis keeps its momentum: when the nose
// turns, the velocity lags and the chassis slides until the side thrusters and the banked collective pull it round. The bank
// builds at the hull's roll rate toward the coordinated-turn bank vtolai flies. The planner rolls this forward so it sees the
// wide line and the slide before it commits to a turn. Pure math, tests/vtolplan_test.cpp.
namespace vtolplan {
struct State {
    double vx=0, vy=0;   // world velocity, m/s (x east, y north, heading 0 = +y)
    double bank=0;       // rad, right side down +
};
struct Limits {
    double accel=6, decel=10;   // m/s^2 along the nose
    double rollRate=1;          // rad/s, the hull's roll rate
};
// One step: the nose is at `heading` turning at `yawRate`, aiming for `speed` along it.
inline State Step(State s, double heading, double yawRate, double speed, const Limits& l, double dt)
{
    if (!(dt > 0) || !std::isfinite(heading) || !std::isfinite(yawRate) || !std::isfinite(speed)) return s;
    const double fx = std::sin(heading), fy = std::cos(heading), rx = std::cos(heading), ry = -std::sin(heading);
    double along = s.vx*fx + s.vy*fy, side = s.vx*rx + s.vy*ry;
    along += std::clamp(speed - along, -l.decel*dt, l.accel*dt);
    const double want = vtolai::TurnBank(along, yawRate);
    s.bank += std::clamp(vtolai::kBankGain*(want - s.bank), -l.rollRate, l.rollRate)*dt;
    s.bank = std::clamp(s.bank, -vtolai::kMaxBank, vtolai::kMaxBank);
    // The slide is pulled back toward the nose by the side thrusters (proportional, bounded) and by a bank toward it.
    const double thrusters = std::min(vtolai::kSideAcceleration, 1.2*std::fabs(side));
    const double tilt = (s.bank > 0) == (side < 0) ? vtolai::kGravity*std::tan(std::fabs(s.bank)) : 0.0;
    side -= std::copysign(std::min(std::fabs(side), (thrusters + tilt)*dt), side);
    s.vx = along*fx + side*rx; s.vy = along*fy + side*ry;
    return s;
}
inline double Slip(const State& s, double heading) { return s.vx*std::cos(heading) - s.vy*std::sin(heading); }
}
