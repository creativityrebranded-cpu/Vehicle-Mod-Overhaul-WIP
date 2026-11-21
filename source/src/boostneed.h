#pragma once
// BO-1 (2026-11-19): Speed's "possible speed" with the game's forward boost, for a car on its wheels. Pure: no game calls, no globals.
// Place in the design (notes/VEHICLE_DECISION_DESIGN_20261106.md): Route says the wanted speed, the energy plan chose its speed, Speed works out what
// the car can hold. When the wheels cannot reach that speed (thrust-limited: a grade, a heavy car, a weak start) the possible speed is raised by
// the boost the car has. The decision value is a request (drive::Output::boost) beside the plan's speed; the numbers are in planbranch::rules
// (planrules.h, BO-1); the actuator in Fly() only executes it (Input block +0x1C). The plan stays the one decider; nothing here reads or changes
// a line, a hold or a brake. A boost is never asked into a halt: a hold, a stop line, a brake request, a stop, a reverse or the wheel-clearance
// vote below the plan's speed ends it at once. The meter is the game's own (component +0x238, read as a fact); nothing here pins it.
#include "planrules.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace boostneed {
namespace R = planbranch::rules;

struct State {
    bool on = false, exhausted = false;
    double since = 0, offSince = -1e9;   // when the boost started / last ended (s)
    double need = 0;                     // s the start condition has held
    double accel = 0, lastFwd = 0, lastNow = -1;  // the car's smoothed gain (m/s^2), for the closing check
};

struct Sample {
    double now = 0;                      // s
    double wanted = 0;                   // m/s the plan asks along the nose (after the speed follower)
    double fwd = 0;                      // m/s measured along the nose (signed)
    bool onWheels = true, reverse = false, brake = false, held = false;  // brake: the plan's brake request; held: a Traffic hold
    double stopDistance = -1;            // m to a route stop line (< 0: none)
    double wheelVote = std::numeric_limits<double>::infinity();  // the wheel-clearance vote's speed for the plan's first command
    double steerAbs = 0, slipDeg = 0;    // last steering sent (0..1), rear slip angle (deg)
    double pedal = 0; bool pedalFresh = false;  // the tyre pedal's demand before its clamp (1 = full), measured by the actuator
    double charge = 0; bool meterFresh = false, nativeOn = false;  // the game's boost charge (0..1), the game's boosting flag
    // BO-2 (hover cars): hover = a flying car (no wheels, no tyre pedal: `pedal` is the hover throttle before its clamp, `charge` the pilot's hover
    // meter, boostlogic::Meter); capSpeed = the speed the hover may reach now (the ground-loaded cap, the flight gear's top, the plan's speed + 1);
    // hoverHalt = a case where the flight is precise and the thrust is not the question (take-off, landing, a held spot, a strafe, near the floor,
    // the arrival or an obstacle, a gear held low for a tight turn): its name, or null.
    bool hover = false; double capSpeed = std::numeric_limits<double>::infinity(); const char* hoverHalt = nullptr;
};

struct Out { bool boost = false, changed = false; const char* why = ""; };

// Reasons are words for the OBS trace: what started it, what ended it, why it did not start.
inline Out Decide(State& s, const Sample& in) {
    Out o; const bool was = s.on;
    const double dt = s.lastNow >= 0 && in.now > s.lastNow ? std::min(.5, in.now - s.lastNow) : 0.0;
    // The car's gain, smoothed: a car on its way up to speed is not thrust-limited; one that plateaus under the ask is.
    if (dt > 0 && std::isfinite(in.fwd)) { const double a = (in.fwd - s.lastFwd) / dt, k = std::min(1.0, dt / R::boostAccelTau); s.accel += (a - s.accel) * k; }
    s.lastNow = in.now; s.lastFwd = std::isfinite(in.fwd) ? in.fwd : 0.0;
    const double charge = std::isfinite(in.charge) ? std::clamp(in.charge, 0.0, 1.0) : 0.0;
    if (s.exhausted && in.meterFresh && charge >= R::boostMeterOn) s.exhausted = false;  // recharged enough to start again

    const char* halt = nullptr;  // a halt (or a case the boost does not belong to) cuts the boost at once
    if (!std::isfinite(in.wanted) || !std::isfinite(in.fwd)) halt = "no_numbers";
    else if (!in.onWheels && !in.hover) halt = "not_on_wheels";
    else if (in.hover && in.hoverHalt) halt = in.hoverHalt;
    else if (in.reverse) halt = "reverse";
    else if (in.held) halt = "traffic_hold";
    else if (in.stopDistance >= 0 && in.stopDistance < R::boostStopLook) halt = "stop_line_ahead";
    else if (in.brake) halt = "brake_requested";
    else if (in.wanted < R::boostMinWanted) halt = "slow_or_stopped_ask";
    else if (std::isfinite(in.wheelVote) && in.wheelVote < in.wanted - .05) halt = "wheel_clearance_below_ask";
    else if (in.hover && std::isfinite(in.capSpeed) && in.capSpeed < in.wanted - R::boostCapSlack) halt = "speed_cap_below_ask";
    else if (in.slipDeg > R::boostSlipDeg) halt = "sliding";
    else if (in.steerAbs > R::boostSteer) halt = "steering_hard";
    if (halt) {
        if (s.on) { s.on = false; s.offSince = in.now; }
        s.need = 0; o.boost = false; o.why = halt; o.changed = was != s.on; return o;
    }

    const double shortfall = in.wanted - in.fwd;
    if (s.on) {
        const bool minRun = in.now - s.since < R::boostMinOn;
        const bool meterOut = in.meterFresh && (charge <= R::boostMeterOff || (!in.nativeOn && in.now - s.since > .5));
        if (!in.meterFresh) { s.on = false; o.why = "meter_unknown"; }
        else if (meterOut) { s.on = false; s.exhausted = true; o.why = "meter_exhausted"; }
        else if (!minRun && shortfall < R::boostMetGap) { s.on = false; o.why = "speed_met"; }
        else o.why = shortfall < R::boostMetGap ? "holding_minimum_run" : "thrust_limited_boosting";
        if (!s.on) { s.offSince = in.now; s.need = 0; }
        o.boost = s.on; o.changed = was != s.on; return o;
    }

    // Off: is the car thrust-limited? Short of the ask, pedal already full, and not closing the gap.
    const bool limited = shortfall >= R::boostGap && in.pedalFresh && in.pedal >= R::boostPedal && s.accel < R::boostMinAccel;
    s.need = limited ? s.need + dt : std::max(0.0, s.need - 2.0 * dt);  // a short gap in the condition does not reset it
    if (!limited) { o.why = shortfall < R::boostGap ? "speed_met" : !in.pedalFresh ? "pedal_unknown" : in.pedal < R::boostPedal ? "pedal_not_full" : "still_gaining"; return o; }
    if (!in.meterFresh) { o.why = "meter_unknown"; return o; }
    if (s.exhausted || charge < R::boostMeterOn) { o.why = "meter_low_recharging"; return o; }
    if (in.now - s.offSince < R::boostMinOff) { o.why = "cooling_off"; return o; }
    if (s.need < R::boostOnHold) { o.why = "thrust_limited_waiting"; return o; }
    s.on = true; s.since = in.now; s.need = 0; o.boost = true; o.changed = true; o.why = "thrust_limited_start";
    return o;
}
}
