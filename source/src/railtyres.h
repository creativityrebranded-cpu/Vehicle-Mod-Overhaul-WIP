#pragma once
// TY-1 tyre mode (2026-11-07, user: "yes both" to notes/vehicle-native-input-20261107/FINDINGS.md candidate A). Owner: the actuator
// (locked controller). It still only tracks the committed plan: the rail's wanted speed and turn become the game's own throttle and
// brake pedals (sent through the existing input path, SubmitFire / ApplyInputDetour) instead of velocity writes after each step.
// Steering stays the rail's bicycle-model wheel angle. No engine hook, address or layout changes. Pure: tests/railtyres_test.cpp.
//
// Game behaviour designed around [game, ENGINE_HANDBOOK section 4]: the brake pedal backs a stopped car up; so the brake is pressed only
// while moving forward faster than wanted (or to stop), never at rest; a car told to stop and rolling back gets a little throttle, not
// brake; no full brake at a crawl with the wheel near full lock (2026-11-05 00:33: side-on for 70 s).
#include <algorithm>
#include <cmath>

namespace railtyres
{
    namespace rules {
        inline constexpr double kP = 0.25;            // throttle per m/s short of the wanted speed
        inline constexpr double kI = 0.08;            // throttle per m/s per s (holds speed against drag and slopes)
        inline constexpr double iMax = 0.6;           // the held part of the throttle at most
        // SP-1 (2026-11-19): the push a climb needs, from the road's grade along the travel (World's lane floor), at once: not waited
        // for from the held part. 0.42 (23 deg) asks 0.8; a hand-driven car climbs that ramp on full throttle. Downhill adds nothing.
        inline constexpr double gradeFeed = 2.0, gradeFeedMax = 0.8;
        inline constexpr double feedPerMs = 0.02;     // throttle per m/s wanted (cruise feed)
        inline constexpr double feedMax = 0.4;
        inline constexpr double stopSpeed = 0.1;      // wanted under this: stop
        inline constexpr double brakeOver = 1.0;      // m/s over the wanted speed before the brake is pressed
        inline constexpr double brakeGain = 0.2;      // brake per m/s over
        inline constexpr double stopBrakeGain = 1.0 / 3.0, stopBrakeMin = 0.3;
        inline constexpr double movingFwd = 0.3;      // m/s forward: moving (the brake slows; at or under this it would reverse)
        inline constexpr double rollBack = -0.3;      // m/s: rolling back while it should not
        inline constexpr double rollBackGain = 0.4, rollBackMax = 0.4;
        inline constexpr double lockSteer = 0.7, crawl = 4.0, lockBrakeMax = 0.6;  // near full lock under crawl m/s: brake capped
        inline constexpr double heldFade = 6.0;       // TM-1: s for a remembered held throttle to fade to a third (a different spot needs a different push)
    }
    struct State { double i = 0; double fwdHeld = 0, revHeld = 0; };  // TM-1: the held throttle remembered per direction across stops and reversals
    struct Out { double accel = 0, brake = 0; const char* why = ""; };

    // want: the rail's wanted speed along the nose (m/s, >= 0); fwd: measured signed speed along the nose; reverse: the plan backs up;
    // steerAbs: |steer| sent this frame (0..1); dt: s since the last call.
    inline Out Step(State& s, double want, double fwd, bool reverse, double steerAbs, double dt)
    {
        Out o;
        dt = std::clamp(dt, 0.0, 0.25);
        const double cap = steerAbs > rules::lockSteer && std::fabs(fwd) < rules::crawl ? rules::lockBrakeMax : 1.0;
        if (reverse) { s.i = 0; o.brake = 1.0; o.why = "reverse"; return o; }  // the brake pedal backs up once stopped
        if (!std::isfinite(want) || !std::isfinite(fwd)) { s.i = 0; o.why = "no_numbers"; return o; }
        if (want < rules::stopSpeed) {
            s.i = 0;
            if (fwd > rules::movingFwd) { o.brake = std::min(cap, std::clamp(fwd * rules::stopBrakeGain, rules::stopBrakeMin, 1.0)); o.why = "stopping"; }
            else if (fwd < rules::rollBack) { o.accel = std::min(rules::rollBackMax, -fwd * rules::rollBackGain); o.why = "rolling_back"; }
            else o.why = "held";  // no pedal at rest: a held brake would back it up
            return o;
        }
        const double err = want - fwd;
        if (fwd > want + rules::brakeOver && fwd > rules::movingFwd) {
            s.i = std::max(0.0, s.i + rules::kI * err * dt);  // err < 0: the held throttle bleeds off
            o.brake = std::min(cap, std::clamp((fwd - want) * rules::brakeGain, 0.0, 1.0)); o.why = "slowing";
            return o;
        }
        s.i = std::clamp(s.i + rules::kI * err * dt, 0.0, rules::iMax);
        const double feed = std::min(rules::feedMax, want * rules::feedPerMs);
        o.accel = std::clamp(rules::kP * err + s.i + feed, 0.0, 1.0);
        o.why = err > 0.5 ? "accelerating" : "holding";
        return o;
    }

    // TY-3 (2026-11-07, user: "combined them together to make a continuous multiplier ... for both directions"): one signed pedal,
    // the way the game reads it (FUN_141324650: pedal = brake - accelerate). want and fwd are signed along the nose (+ forward).
    // pedal > 0 = throttle; pedal < 0 = the brake pedal, which brakes a car moving forward and reverses one at rest or rolling back.
    // So one controller on (want - fwd) covers forward, slowing, stopping, reversing and slowing a reverse; zero gap at rest = no pedal.
    struct Signed { double pedal = 0; double accel = 0, brake = 0; const char* why = ""; double demand = 0; };  // BO-1: demand = the pedal before its clamp (>= 1: full throttle already; Speed reads it as a measured fact)
    inline Signed StepSigned(State& s, double want, double fwd, double steerAbs, double dt, double grade = 0.0)
    {
        Signed o;
        dt = std::clamp(dt, 0.0, 0.25);
        if (!std::isfinite(want) || !std::isfinite(fwd)) { s.i = 0; o.why = "no_numbers"; return o; }
        if (!std::isfinite(s.i)) s.i = 0;  // a bad held value never reaches the pedal
        if (!std::isfinite(s.fwdHeld)) s.fwdHeld = 0;
        if (!std::isfinite(s.revHeld)) s.revHeld = 0;
        const int dir = want > rules::stopSpeed ? 1 : want < -rules::stopSpeed ? -1 : 0;
        // TM-1 (2026-11-13 09:56 run: on a 0.33 grade the plan stopped and reversed every 0.3-0.5 s and each time the held throttle went back
        // to 0, so the push never built up): the held part is kept per direction when the car stops or changes direction, restored when it
        // drives that way again, and fades with time (heldFade) while not in use.
        const double fade = std::exp(-dt / rules::heldFade);
        if (s.i > 0) s.fwdHeld = s.i; else if (s.i < 0) s.revHeld = s.i;
        if (dir <= 0) s.fwdHeld *= fade;
        if (dir >= 0) s.revHeld *= fade;
        if (dir == 0) {  // stop: brake whichever way it rolls (the brake pedal slows forward motion, throttle slows backward)
            s.i = 0;
            if (fwd > rules::movingFwd) { o.pedal = -std::clamp(fwd * rules::stopBrakeGain, rules::stopBrakeMin, 1.0); o.why = "stopping"; }
            else if (fwd < -rules::movingFwd) { o.pedal = std::clamp(-fwd * rules::stopBrakeGain, rules::stopBrakeMin, 1.0); o.why = "stopping_reverse"; }
            else o.why = "held";
        } else {
            if ((s.i >= 0 && dir < 0) || (s.i <= 0 && dir > 0)) s.i = dir > 0 ? s.fwdHeld : s.revHeld;  // TM-1: this direction's remembered push (never the other's)
            const double err = want - fwd;
            const double feed = dir * std::min(rules::feedMax, std::fabs(want) * rules::feedPerMs);
            s.i = std::clamp(s.i + rules::kI * err * dt, dir > 0 ? 0.0 : -rules::iMax, dir > 0 ? rules::iMax : 0.0);
            const double climb = std::clamp(dir * grade * rules::gradeFeed, 0.0, rules::gradeFeedMax);  // SP-1: uphill along the travel
            double u = rules::kP * err + s.i + feed + dir * climb;
            // Dead band against the travel: a pedal against the motion only once it is over the wanted speed by brakeOver.
            const bool over = dir > 0 ? fwd > want + rules::brakeOver : fwd < want - rules::brakeOver;
            if (dir > 0 && u < 0 && !over) u = 0;
            if (dir < 0 && u > 0 && !over) u = 0;
            o.demand = u;
            o.pedal = std::clamp(u, -1.0, 1.0);
            o.why = over ? (dir > 0 ? "slowing" : "slowing_reverse") : std::fabs(err) > 0.5 ? (dir > 0 ? "accelerating" : "reversing") : (dir > 0 ? "holding" : "holding_reverse");
        }
        // No hard brake at a crawl near full lock (2026-11-05 side-on flip-flop): a brake against forward motion is capped.
        if (o.pedal < 0 && fwd > 0 && steerAbs > rules::lockSteer && std::fabs(fwd) < rules::crawl) o.pedal = std::max(o.pedal, -rules::lockBrakeMax);
        o.accel = std::max(0.0, o.pedal); o.brake = std::max(0.0, -o.pedal);
        return o;
    }
}
