#pragma once
// FC-1 / FG-1 (2026-11-19, work/flight-gears/DESIGN.md): the flight gear in use and what it does to the standard hover.
//   Plan (FG-1): the autopilot's flight gear. A deterministic rule, as AG-1 (autogear.h) is on the ground: low gear where the flight must
//     be precise (take-off, landing, holding a spot, a strafe, near the floor, near the arrival, near an obstacle, a turn tighter than the
//     gear holds), otherwise the lowest gear whose top reaches the route's wanted speed, one up on a long straight leg. Down after
//     2 reactions (2 x wheelguard::reaction), up one gear after kUpHold.
//   Stage F (GENERATED_COSTS_DESIGN_20261122.md): the distances and speeds are the car's own, not fixed metres (see Gear thresholds below). The player's gear key sets the player's gear (no automatic shift for the player).
//   Actuation (standard hover): the spin easing and the sideways leak of carcap::FlightRow, in the one place that writes them.
// The row itself is carcap::Flight (the one car record). Pure: no game calls, no globals, no logging. Tests: tests/flightgear_test.cpp.
#include "bicycle.h"
#include "carcap.h"
#include "energyplan.h"
#include "hoverfollow.h"
#include "wheelguard.h"
#include <algorithm>
#include <cmath>

namespace flightgear {
// ---- Standard hover actuation (vehicleflight.cpp's standard model) ----
// The written spin eases toward the wanted rate at the row's response (1/s). Gear 3: 4 /s yaw, 5 /s pitch and roll, as always.
inline double Ease(double spin, double want, double response, double dt) { return spin + (want - spin) * (1.0 - std::exp(-dt * response)); }
// The sideways speed across the nose bleeds off at the row's side gain past its deadband (gear 3: all of it at 1.2 /s, as always).
inline double SideLeak(const carcap::FlightRow& r, double side, double hSpeed) {
    const double dead = r.SideDeadband(hSpeed);
    const double past = std::fabs(side) > dead ? side - std::copysign(dead, side) : 0.0;
    return past * r.sideGain;
}

// ---- The committed plan's sharpest turn (rad/s): the largest heading change per second between its points ----
inline double PeakTurn(const hoverfollow::Path& p) {
    double peak = 0;
    for (int i = 1; i < p.count; ++i) {
        const double dt = p.p[i].t - p.p[i - 1].t;
        if (dt > 1e-6) peak = std::max(peak, std::fabs(hoverfollow::Wrap(p.p[i].heading - p.p[i - 1].heading)) / dt);
    }
    return peak;
}

// ---- FG-1: the autopilot's flight gear ----
// Gear thresholds, generated from the car (bicycle::Spec: brake, hoverClear, climbRate, hoverAccel) and the controller reaction t_r
// (wheelguard::reaction). Replaces kNearFloor 3 m, kNearArrival 15 m, kNearObstacle 20 m, kHoldBelow 1 m/s, kLongLeg 200 m, kDownHold .4 s.
constexpr double kReaction = wheelguard::reaction;
// Distance the car needs to stop from speed v: braking v^2 / (2 b) plus the distance covered while the controller replaces its command.
inline double StopDistance(double v, double brake) { v = std::fabs(v); return v * v / (2.0 * std::max(.1, brake)) + v * kReaction; }
// A plan this slow holds its spot: the speed the brakes stop within one plan step (the same bottom rung as energyplan::SpeedRungs).
inline double HoldBelow(const bicycle::Spec& k) { return k.brake * energyplan::kDt; }
// Near the floor: the hover height plus the height the climb still carries it while the lift answers (climbRate^2 / (2 accel)).
inline double NearFloor(const bicycle::Spec& k) { return k.hoverClear + k.climbRate * k.climbRate / (2.0 * std::max(.1, k.hoverAccel)); }
// Straight share: CHOSEN RESOLUTION, no physical derivation. A plan whose sharpest turn asks under this share of the gear's lateral is
// called straight (the margin that stops a plan's small wobble from counting as a bend). Kept at today's value; reported, not derived.
constexpr double kStraightShare = .25;
// Down hold = two reactions (the cause must outlast one command replacement and its check). Up hold: CHOSEN RESOLUTION kept at 2.5 s.
// The gear row's own thrust response (carcap::FlightRow yawResponse / tiltResponse, 2.4-10 /s = 0.1-0.4 s per time constant) is
// only the lower bound; an upshift also must not follow a downshift inside the time a plan changes its mind, which no car number gives.
constexpr double kDownHold = 2.0 * kReaction;
constexpr double kUpHold = 2.5;  // s

struct Ask {
    bool landing = false, takeOff = false, strafe = false;
    double planSpeed = 0;        // m/s the plan asks now
    double floorGap = 1e9;       // m over the floor
    double toArrival = 1e9;      // m to the stop (none: 1e9)
    double obstacle = 1e9;       // m to the nearest wall / vehicle ahead (none: 1e9)
    double want = 0;             // m/s: the route's wanted speed (not the plan's output, which the gear's top already caps)
    double speed = 0;            // m/s now
    double peakTurn = 0;         // rad/s: the committed plan's sharpest turn
    double lateral[5] = { 0, 0, 0, 0, 0 };  // m/s^2 the hull holds in gears 1-4 (hulllogic::Flight per gear)
    bicycle::Spec car;           // the car's own numbers (the spec the energy plan plans with): brake, hoverClear, climbRate, hoverAccel
};
struct Pick { int gear = 1; const char* reason = ""; double need = 0; };
inline Pick Target(const Ask& a) {
    Pick p; p.need = std::max(4.0, std::fabs(a.speed)) * std::max(0.0, a.peakTurn);  // m/s^2 the plan's sharpest turn asks of the hull
    if (a.landing) { p.reason = "landing"; return p; }
    if (a.takeOff) { p.reason = "take_off"; return p; }
    if (a.strafe) { p.reason = "strafe"; return p; }
    const double vNow = std::max(std::fabs(a.speed), std::fabs(a.planSpeed));  // the speed to be shed
    if (a.planSpeed < HoldBelow(a.car)) { p.reason = "hover_in_place"; return p; }
    if (a.floorGap < NearFloor(a.car)) { p.reason = "near_floor"; return p; }
    if (a.toArrival < StopDistance(vNow, a.car.brake)) { p.reason = "near_arrival"; return p; }
    if (a.obstacle < StopDistance(vNow, a.car.brake)) { p.reason = "near_obstacle"; return p; }
    int g = 4; for (int i = 1; i <= 4; ++i) if (carcap::kFlightTop[i] >= a.want) { g = i; break; }
    p.gear = g; p.reason = "wanted_speed";
    if (a.lateral[g] < p.need) {  // the plan turns tighter than this gear holds: the highest lower gear that holds it (else gear 1)
        int lo = 1; for (int i = g - 1; i >= 1; --i) if (a.lateral[i] >= p.need) { lo = i; break; }
        p.gear = lo; p.reason = "tight_turn"; return p;
    }
    if (g < 4 && a.toArrival > StopDistance(carcap::kFlightTop[g + 1], a.car.brake) &&  // a leg long enough to stop from the higher gear's top
         p.need < kStraightShare * a.lateral[g + 1]) { p.gear = g + 1; p.reason = "long_straight_leg"; }
    return p;
}
struct Shift { int gear = 0; int dir = 0; double since = 0; const char* reason = ""; };
// The gear held: the first pick at once; a lower pick after kDownHold (straight to it), a higher one gear up after kUpHold.
// Returns true when the gear changed.
inline bool Update(Shift& s, const Pick& p, double now) {
    if (s.gear < 1) { s.gear = p.gear; s.dir = 0; s.reason = p.reason; return true; }
    const int dir = p.gear < s.gear ? -1 : p.gear > s.gear ? 1 : 0;
    if (!dir) { s.dir = 0; return false; }
    if (dir != s.dir) { s.dir = dir; s.since = now; }
    if (now - s.since < (dir < 0 ? kDownHold : kUpHold)) return false;
    s.gear = dir < 0 ? p.gear : s.gear + 1; s.reason = p.reason; s.since = now;
    if (s.gear == p.gear) s.dir = 0;
    return true;
}
}
