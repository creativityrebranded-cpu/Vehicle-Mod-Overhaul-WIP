#pragma once
// One car (D2, 2026-11-19, design rule "one car"): every number about what THIS car can do comes from this one record. Before it, the
// sensors and the plan each kept their own: the step/climb height was 0.25 (World surface Judge), 0.35 (Plan bicycle), 0.4 (radar floor
// rule) and 0.5 (lane volume pass height), so a 0.25-0.5 m lip blocked or passed by which sensor saw it; the steepest grade was 0.7 (surface)
// and 1.0 (energy plan); the sideways grip was 3.0 x grip (energy plan), 3.5 (bend cap BC-1, tracking TK-1) and 6 (steering cap ST-1, corner
// forces CF-1, the latter without the gear's grip).
//
//   climb       - the highest edge a wheel rolls over: half a tyre (0.35 m, the planner's own number). Over it the car meets a face.
//   MaxGrade    - the steepest grade (rise / run) the wheels climb: the lower of the friction limit (mu x the gear's grip x the planner's
//                 safety share) and the grade the engine pulls (SR-1). Gravity's strength cancels out of the friction limit.
//   PlanLat     - the sideways acceleration the PLAN counts on in a turn (comfort, with margin under the tyres): 3.0 m/s^2 x the gear's grip
//                 (clamped 0.5-3). 3.0 is the energy plan's own number (the sole decider since 2026-11-14): its rollouts already turn within
//                 it, so a speed cap above it (BC-1's 3.5) never limited anything the plan asked, and a tracking cap (TK-1) at the same number
//                 flags exactly the turns the plan would not have asked for.
//   TyreLat     - the physical tyre limit: 6 m/s^2 x the gear's grip (at least a quarter): the steering cap (ST-1) and the corner forces'
//                 turning target (CF-1) never ask for more.
// grip: the gear's tyre grip multiplier (wheelgrip::PlanGrip; 1 = the game's own tyres, gear 3).
// Pure: no game calls, no globals, no logging. Tests: tests/surface_test.cpp, tests/energyplan_test.cpp.
#include <algorithm>

namespace carcap {
struct Car {
    double climb = .35;                       // m: the highest edge a wheel rolls over (half a tyre)
    double mu = 1.15;                         // tyre friction
    double safety = .85;                      // share of the friction limit the planner counts on
    double engineGrade = .7;                  // steepest grade (rise / run) the engine pulls
    double planLat = 3.0;                     // m/s^2 at grip 1: what the plan turns with
    double tyreLat = 6.0;                     // m/s^2 at grip 1: what the tyres hold
    constexpr double MaxGrade(double grip) const { return std::max(0.0, std::min(mu * grip * safety, engineGrade)); }
    constexpr double PlanLat(double grip) const { return planLat * std::clamp(grip, .5, 3.0); }
    constexpr double TyreLat(double grip) const { return tyreLat * std::max(.25, grip); }
};
inline constexpr Car kCar{};

// ---- Flight (FC-1, 2026-11-19, work/flight-gears/DESIGN.md; the user's decisions: body-axis thrusters, both hover models, build) ----
// The craft's per-gear flight handling: the ONE row the physical VTOL (vtol::Solve), the standard hover (its spin and velocity write),
// the hull's turn limit (hulllogic::Flight) and the flight planner (bicycle::Spec hoverYaw / hoverLat / hoverTop) all read. CF-1's
// principle in the air: track the turn the controls ask for, oppose the sideways slide, within a per-gear budget.
//   Physical VTOL: rateGain (1/s: angular acceleration per rad/s of rate error), angularLimit (rad/s^2, also the torque budget's share),
//                  sideGain (1/s: side-thruster push per m/s of drift past the deadband) within sideBudget (m/s^2; player / AI).
//                  The side push acts along the craft's own right axis, like a real thruster (the user, 2026-11-19): banked, its small
//                  vertical share stays, as it always did; the lift controller holds the height. Never vertical by design otherwise.
//   Standard hover: yawResponse / tiltResponse (1/s: how fast the written spin eases to the wanted rate), and the same sideGain as its
//                  sideways leak (1/s) past the same deadband.
//   Both:          lateralScale (the share of the hull's own sustained turn, hulllogic::Flight; gear 3 = 1) and top (m/s, the player's
//                  flight gear table, kFlightTop).
// Gear 3 is today's flight exactly: Solve 5 /s, 6 rad/s^2, 1.2 /s within 3 (player) / 6 (AI) m/s^2, no deadband; the standard hover's
// 4 /s, 5 /s and 1.2 /s; the hull's own turn; 100 m/s. Gear 1 locks hard (CF-1's latGain 3), gear 4 carries its momentum (CF-1's
// latDead0 0.5 + latDeadPerSpeed 0.05 deadband). Lift is not in the row: the hover lift owns the vertical (never learn gravity per step).
inline constexpr double kFlightTop[5] = { 60.0, 30.0, 60.0, 100.0, 150.0 };  // m/s per gear (0: coasting, the old kGearTop)
struct FlightRow {
    int gear = 3;
    bool ai = false;
    double rateGain = 5, angularLimit = 6, sideGain = 1.2, sideBudget = 3, sideDead0 = 0, sideDeadPerSpeed = 0;
    double yawResponse = 4, tiltResponse = 5;
    double lateralScale = 1, top = 100;
    double SideDeadband(double hSpeed) const { return sideDead0 + sideDeadPerSpeed * hSpeed; }
};
inline FlightRow Flight(int gear, bool ai) {
    static constexpr double rate[5] = { 5, 10, 7, 5, 3 }, ang[5] = { 6, 12, 9, 6, 4 }, side[5] = { 1.2, 3.0, 2.0, 1.2, .5 };
    static constexpr double budgetPlayer[5] = { 3, 12, 8, 3, 2 }, budgetAi[5] = { 6, 12, 12, 6, 4 };
    static constexpr double dead0[5] = { 0, 0, 0, 0, .5 }, deadV[5] = { 0, 0, 0, 0, .05 };
    static constexpr double yawResp[5] = { 4, 8, 5.6, 4, 2.4 }, tiltResp[5] = { 5, 10, 7, 5, 3 };
    static constexpr double lateral[5] = { 1, 12.0 / 9.0, 10.0 / 9.0, 1, 6.0 / 9.0 };  // the physical chassis' 12 / 10 / 9 / 6 m/s^2 over its 9
    const int g = std::clamp(gear, 1, 4);
    FlightRow r; r.gear = g; r.ai = ai;
    r.rateGain = rate[g]; r.angularLimit = ang[g]; r.sideGain = side[g]; r.sideBudget = ai ? budgetAi[g] : budgetPlayer[g];
    r.sideDead0 = dead0[g]; r.sideDeadPerSpeed = deadV[g]; r.yawResponse = yawResp[g]; r.tiltResponse = tiltResp[g];
    r.lateralScale = lateral[g]; r.top = kFlightTop[g];
    return r;
}
// The sustained turn rate at a speed: the lower of the yaw limit and lateral / speed (speed under 4 m/s counts as 4). The hull
// (hulllogic::Limits::YawAt) and the flight planner (bicycle::Spec::HoverYawAt) both turn with this one formula.
inline double TurnAt(double yawMax, double lateral, double speed) { return std::min(yawMax, lateral / std::max(4.0, speed < 0 ? -speed : speed)); }
}
