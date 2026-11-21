#pragma once
// TK-1 (2026-11-19, user: "propose the optimal node system and then just replace entire thing at once"):
// tracking quality -> speed cap. This adds the missing feedback from the Drive stage into the Speed
// stage. Until now nothing lowered the speed when the car was not following its plan well. Logs showed
// the car snaking: heading swung about +/-20 deg every half second, the turn rate averaged 0.37 rad/s
// side to side, and the rear slip angle (the back stepping out) peaked at 12-35 deg, while the planner
// kept asking 9-14 m/s. Update() measures how well the car follows the plan every tick and returns a
// speed cap; the Speed stage takes the minimum of its own speed and this cap.
//
// Pure logic: header-only, no game calls, no globals, no logging. The caller owns State (one per car).
//
// Cap rules (each one gives a candidate cap; the lowest wins):
//  a. weaving            - yaw-rate sign flips (only samples with |yawRate| > yawFloor) >= weaveFlips
//                          in the window -> max(minCap, onsetSpeed * 0.7)
//  b. back_stepping_out  - rearSlipDeg > slipOk -> max(minCap, speed * slipOk / rearSlipDeg)
//  c. off_the_plan       - |lateralErr| > lateralOk or |headingErr| > headingOk for >= offHold s
//                          -> max(minCap, onsetSpeed * 0.8)
//  d. turn_too_hard      - |yawRate * speed| > PlanLat(grip) -> max(minCap, speed at which the present
//                          curvature |yawRate|/speed gives PlanLat(grip) sideways). PlanLat is the one car record's
//                          planning sideways acceleration (carcap.h, D2 2026-11-19: 3.0 x the gear's grip; was a
//                          separate 3.5 here): the car turning harder than the plan may ask is off its plan.
//  e. not on wheels      - nothing changes (cap, hold timer and history are frozen)
// A lowered cap is held dropHold s, then rises at most recoverRate m/s per s. It is released to +inf
// ("tracking_fine") once no rule applies and it has risen to releaseAt, or releaseMargin above the speed
// (it no longer limits anything).
//
// Weaving and off_the_plan scale the speed at the moment the episode began (onsetSpeed), not the live
// speed: the live speed follows the cap down, so speed * 0.7 every tick would ratchet the cap to minCap
// within a few ticks. Slip and turn caps use the live speed because they are physical relationships.
#include "carcap.h"
#include <cmath>
#include <cstddef>
#include <limits>

namespace tracking {

struct Sample {
    double now = 0.0;          // s, game time
    double lateralErr = 0.0;   // m, car vs plan path
    double headingErr = 0.0;   // rad, car heading minus plan heading
    double yawRate = 0.0;      // rad/s about up
    double rearSlipDeg = 0.0;  // deg, rear axle slip angle
    double speed = 0.0;        // m/s forward
    bool onWheels = true;
    double grip = 1.0;         // the gear's tyre grip multiplier (wheelgrip::PlanGrip): scales PlanLat
};

struct Rules {
    double window = 2.0;         // s of history
    double weaveFlips = 3;       // yaw-rate sign changes over |yaw| > yawFloor within the window that count as weaving
    double yawFloor = 0.15;      // rad/s
    double lateralOk = 0.4;      // m
    double headingOk = 0.10;     // rad
    double offHold = 0.5;        // s off the plan before it counts
    double slipOk = 8.0;         // deg
    double minCap = 3.0;         // m/s
    double recoverRate = 1.0;    // m/s per s the cap may rise
    double dropHold = 1.0;       // s a lowered cap is held before rising
    double releaseAt = 25.0;     // m/s: a recovering cap at or above this is released to +inf
    double releaseMargin = 4.0;  // m/s: a recovering cap this far above the speed is released to +inf
};

struct State {
    static constexpr std::size_t kHistory = 256;  // 2 s at up to 128 ticks/s
    struct Entry { double now, lateral, heading, yaw, slip; };
    Entry ring[kHistory] = {};
    std::size_t head = 0;     // next write slot
    std::size_t count = 0;
    double cap = std::numeric_limits<double>::infinity();
    double heldUntil = -std::numeric_limits<double>::infinity();
    double lastNow = 0.0;
    bool started = false;
    double offSince = -1.0;       // <0: on the plan
    double weaveOnsetSpeed = -1.0; // <0: not weaving
    double offOnsetSpeed = -1.0;   // <0: not off the plan
    const char* lastWhy = "tracking_fine";
};

struct Out {
    double cap = std::numeric_limits<double>::infinity();  // m/s, +inf when tracking is fine
    double score = 0.0;                                    // 0 good .. 1 bad
    int weaveFlips = 0;
    double worstLateral = 0.0, worstHeading = 0.0, worstSlip = 0.0;
    const char* why = "tracking_fine";
    bool changed = false;
};

namespace detail {
inline double Clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline double Ratio(double v, double ok) { return ok > 0.0 ? v / ok : 0.0; }
}  // namespace detail

inline Out Update(State& st, const Sample& s, const Rules& r = {}) {
    constexpr double kInf = std::numeric_limits<double>::infinity();
    const double prevCap = st.cap;
    const char* prevWhy = st.lastWhy;
    const double dt = st.started ? std::fmax(0.0, s.now - st.lastNow) : 0.0;
    st.started = true;
    st.lastNow = s.now;

    Out o;
    if (!s.onWheels) {
        // e. airborne: hold everything; the hold timer does not run out in the air.
        if (std::isfinite(st.heldUntil)) st.heldUntil += dt;
        if (st.offSince >= 0.0) st.offSince += dt;
    } else {
        st.ring[st.head] = {s.now, std::fabs(s.lateralErr), std::fabs(s.headingErr), s.yawRate, s.rearSlipDeg};
        st.head = (st.head + 1) % State::kHistory;
        if (st.count < State::kHistory) ++st.count;
    }

    // Window measures (newest to oldest).
    int flips = 0;
    int lastSign = 0;
    for (std::size_t i = 0; i < st.count; ++i) {
        const auto& e = st.ring[(st.head + State::kHistory - 1 - i) % State::kHistory];
        if (s.now - e.now > r.window) break;
        if (e.lateral > o.worstLateral) o.worstLateral = e.lateral;
        if (e.heading > o.worstHeading) o.worstHeading = e.heading;
        if (e.slip > o.worstSlip) o.worstSlip = e.slip;
        if (std::fabs(e.yaw) > r.yawFloor) {
            const int sign = e.yaw > 0.0 ? 1 : -1;
            if (lastSign != 0 && sign != lastSign) ++flips;
            lastSign = sign;
        }
    }
    o.weaveFlips = flips;

    o.score = detail::Clamp01(0.5 * std::fmax(std::fmax(detail::Ratio(flips, r.weaveFlips),
                                                        detail::Ratio(o.worstLateral, r.lateralOk)),
                                              std::fmax(detail::Ratio(o.worstHeading, r.headingOk),
                                                        detail::Ratio(o.worstSlip, r.slipOk))));

    if (!s.onWheels) {
        o.cap = st.cap;
        o.why = std::isfinite(st.cap) ? "airborne_hold" : "tracking_fine";
        o.changed = false;
        st.lastWhy = o.why;
        return o;
    }

    // Candidate caps.
    double target = kInf;
    const char* why = "tracking_fine";
    auto offer = [&](double c, const char* w) {
        c = std::fmax(r.minCap, c);
        if (c < target) { target = c; why = w; }
    };

    // a. weaving
    if (flips >= r.weaveFlips) {
        if (st.weaveOnsetSpeed < 0.0) st.weaveOnsetSpeed = std::fmax(0.0, s.speed);
        offer(st.weaveOnsetSpeed * 0.7, "weaving");
    } else {
        st.weaveOnsetSpeed = -1.0;
    }
    // b. back stepping out
    if (s.rearSlipDeg > r.slipOk) offer(std::fmax(0.0, s.speed) * r.slipOk / s.rearSlipDeg, "back_stepping_out");
    // c. off the plan, sustained
    if (std::fabs(s.lateralErr) > r.lateralOk || std::fabs(s.headingErr) > r.headingOk) {
        if (st.offSince < 0.0) st.offSince = s.now;
        if (s.now - st.offSince >= r.offHold) {
            if (st.offOnsetSpeed < 0.0) st.offOnsetSpeed = std::fmax(0.0, s.speed);
            offer(st.offOnsetSpeed * 0.8, "off_the_plan");
        }
    } else {
        st.offSince = -1.0;
        st.offOnsetSpeed = -1.0;
    }
    // d. turning harder than the plan may ask (carcap PlanLat at this gear's grip)
    const double latOk = carcap::kCar.PlanLat(s.grip);
    if (std::fabs(s.yawRate * s.speed) > latOk) {
        const double curvature = std::fmax(1e-3, std::fabs(s.yawRate) / std::fmax(s.speed, 0.1));
        offer(std::sqrt(latOk / curvature), "turn_too_hard");
    }

    // Cap dynamics: drop at once, hold, then rise at the recover rate.
    if (target < st.cap) {
        st.cap = target;
        st.heldUntil = s.now + r.dropHold;
        o.why = why;
    } else if (std::isfinite(st.cap)) {
        if (s.now < st.heldUntil) {
            o.why = std::isfinite(target) ? why : "holding";
        } else {
            st.cap = std::fmin(target, st.cap + r.recoverRate * dt);
            o.why = (std::isfinite(target) && st.cap >= target) ? why : "recovering";
            if (!std::isfinite(target) &&
                (st.cap >= r.releaseAt || st.cap >= std::fmax(0.0, s.speed) + r.releaseMargin)) {
                st.cap = kInf;
                st.heldUntil = -kInf;
                o.why = "tracking_fine";
            }
        }
    } else {
        o.why = "tracking_fine";
    }

    o.cap = st.cap;
    o.changed = (o.cap != prevCap && !(std::isinf(o.cap) && std::isinf(prevCap))) || o.why != prevWhy;
    st.lastWhy = o.why;
    return o;
}

}  // namespace tracking
