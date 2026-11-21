#pragma once
// Redesign step 1 (2026-11-12, notes/redesign-20261112/AUDIT_AND_REDESIGN.md section 4.1): what the car can physically do, for the energy
// planner's rollouts. Built on carmodel.h (the steering Havok really applies, measured in game 2026-11-04): the steering input in [-1, 1] is
// shaped and scaled by the vehicle's lock; the bicycle then moves. Position, heading, signed speed (negative = reverse). Every candidate the
// planner considers is rolled out through this, so no plan asks for a turn the car cannot make. Pure; tests: tests/energyplan_test.cpp.
#include "carcap.h"
#include "carmodel.h"
#include <algorithm>
#include <cmath>

namespace bicycle {
struct Spec {
    carmodel::Car car = carmodel::Fallback(2.3);  // lock, wheelbase, input shaping (measured models, else the fallback)
    double inputRate = 2.0;                        // steering input units per second (the game's PID moves the input, not the wheels)
    double accel = 3.0, brake = 6.0;               // m/s^2
    double maxSpeed = 20.0, maxReverse = 4.0;      // m/s
    double halfWidth = 1.0, halfLength = 2.3, height = 2.0;
    double climb = carcap::kCar.climb;             // m: the highest edge a wheel rolls over (half a tyre; the one car record, carcap.h)
    // Step 3 (D6): the hover, as the game's hover follows a bearing, a speed and a height (autodrive Limits; planhover.h).
    double hoverTop = 26.0, hoverAccel = 4.0;      // m/s, m/s^2
    double climbRate = 5.0, descendRate = 3.0;     // m/s up / down toward the height it holds
    double hoverYaw = 1.6;                         // rad/s the nose turns at full input (the rail's cap, the same hover turn)
    // FC-1 (2026-11-19): the sustained sideways acceleration the hovering craft holds (m/s^2). The craft turns at min(yaw, lateral / v)
    // (hulllogic::Limits::YawAt), so the plan does too: the flight tick fills hoverYaw / hoverLat / hoverTop from the car's flight gear
    // row (carcap::Flight, hulllogic::Flight). 12: the standard hover's reference hull at gear 3.
    double hoverLat = 12.0;
    double HoverYawAt(double speed) const { return carcap::TurnAt(hoverYaw, hoverLat, speed); }
    double hoverClear = 2.5;                       // m kept over the surface when hovering (autodrive Limits::hoverClear)
    double MinRadius() const { return car.wheelbase / std::tan(carmodel::WheelAngle(car, 1.0, 0.0)); }
};
// z: the car's base height (on wheels: the floor; hovering: where it holds). mode: 0 on its wheels, 1 hovering (step 3).
struct State { double x = 0, y = 0, heading = 0, speed = 0, input = 0, z = 0; int mode = 0; };  // heading: atan2(dx, dy) (0 = +y), as the rest of FVO
struct Control { double input = 0, speed = 0; };  // asked steering input [-1, 1] and asked signed speed (m/s)

// grade (SG-1, 2026-11-13): the floor's rise per metre along the nose (+ = uphill ahead). Gravity along the slope takes from the push that
// climbs and from the brake that holds on the way down, never below a quarter of either (this car does climb 0.33 grades in the logs; its
// engine reserve is not measured, so the model does not let it roll back). 0: flat, as before.
inline State Step(const State& s, const Control& c, const Spec& k, double dt, double grade = 0) {
    State n = s;
    n.input += std::clamp(std::clamp(c.input, -1.0, 1.0) - s.input, -k.inputRate * dt, k.inputRate * dt);
    const double target = std::clamp(c.speed, -k.maxReverse, k.maxSpeed);
    const bool slowing = std::fabs(target) < std::fabs(s.speed) || target * s.speed < 0;
    double rate = slowing ? k.brake : k.accel;
    if (grade != 0) {
        const double along = 9.81 * grade / std::sqrt(1 + grade * grade);  // + = gravity pulls backward along the nose
        const double moveDir = (slowing ? (s.speed >= 0 ? -1.0 : 1.0) : (target >= s.speed ? 1.0 : -1.0));  // the way this change pushes
        rate = std::max(.25 * rate, rate - std::max(0.0, moveDir * along));  // pushing against gravity: less; with it: as before
    }
    n.speed += std::clamp(target - s.speed, -rate * dt, rate * dt);
    const double v = .5 * (s.speed + n.speed);
    n.heading = s.heading + v / std::max(1.0, k.car.wheelbase) * std::tan(carmodel::WheelAngle(k.car, n.input, std::fabs(v))) * dt;
    n.x += v * std::sin(n.heading) * dt;
    n.y += v * std::cos(n.heading) * dt;
    return n;
}
// Step 3 (D6): one hover step. The nose turns at input x the hover turn at this speed (FC-1: min(hoverYaw, hoverLat / v)); travel is along the nose turned by `strafe` (a combat line keeps its nose
// on the target and moves sideways); speed moves toward the asked speed within hoverAccel / brake; height toward `targetZ` at the climb or
// descent rate. No wheels: no grip limit, no slope limit.
inline State HoverStep(const State& s, double input, double speed, double strafe, double targetZ, const Spec& k, double dt) {
    State n = s; n.mode = 1;
    n.input = std::clamp(input, -1.0, 1.0);
    n.heading = s.heading + n.input * k.HoverYawAt(s.speed) * dt;  // FC-1: the turn the craft holds at this speed
    const double target = std::clamp(speed, -k.maxReverse, k.hoverTop);
    const bool slowing = std::fabs(target) < std::fabs(s.speed) || target * s.speed < 0;
    n.speed += std::clamp(target - s.speed, -(slowing ? k.brake : k.hoverAccel) * dt, (slowing ? k.brake : k.hoverAccel) * dt);
    const double v = .5 * (s.speed + n.speed), dir = n.heading + strafe;
    n.x += v * std::sin(dir) * dt; n.y += v * std::cos(dir) * dt;
    n.z += std::clamp(targetZ - s.z, -k.descendRate * dt, k.climbRate * dt);
    return n;
}
}
