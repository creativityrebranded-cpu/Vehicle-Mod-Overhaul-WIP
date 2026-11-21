// BO-1 boost need (src/boostneed.h): Speed's possible speed with the game's forward boost, against a toy car with a thrust limit, the game's
// boost meter (drain while boosting, recharge after a delay), and the real tyre pedal (railtyres.h). Offline logic only: never proof in game.
#include "boostneed.h"
#include "railtyres.h"
#include "boostlogic.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstring>
static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

struct Car {
    double v = 0, charge = 1.0, delay = 0; bool boosting = false;
    double thrust = 3.5, boostThrust = 6.0;  // m/s^2 at full pedal; the boost adds boostThrust while it burns
};
struct Run {
    double t = 0; int starts = 0, stops = 0; double boostSeconds = 0, vMax = 0; const char* lastWhy = ""; bool boostedWhileHalt = false;
    boostneed::State bs; railtyres::State ts; Car c;
    double wanted = 12, slope = 0, stopDistance = -1, wheelVote = INFINITY; bool held = false, brake = false, reverse = false, meterKnown = true;
};
// One 1/30 s step: the plan's ask, the tyre pedal, the decision, the car (native meter: burns over 3 s, recharge after 1 s over 4 s).
static void Step(Run& r, double dt = 1.0 / 30) {
    auto& c = r.c;
    const auto po = railtyres::StepSigned(r.ts, r.held || r.brake ? 0.0 : r.wanted, c.v, 0, dt, 0);
    boostneed::Sample s; s.now = r.t; s.wanted = r.held ? 0.0 : r.wanted; s.fwd = c.v; s.reverse = r.reverse; s.brake = r.brake; s.held = r.held;
    s.stopDistance = r.stopDistance; s.wheelVote = r.wheelVote; s.pedal = po.demand; s.pedalFresh = true; s.charge = c.charge; s.meterFresh = r.meterKnown; s.nativeOn = c.boosting;
    const auto d = boostneed::Decide(r.bs, s);
    if (d.changed) { if (d.boost) ++r.starts; else ++r.stops; }
    r.lastWhy = d.why;
    if (d.boost && (r.held || r.brake || r.reverse || r.stopDistance >= 0 || r.wanted < 3)) r.boostedWhileHalt = true;
    // the game's native boost update
    const bool want = d.boost && c.charge > 0.05;
    c.boosting = want;
    if (want) { c.charge = std::max(0.0, c.charge - dt / 3.0); c.delay = 0; r.boostSeconds += dt; }
    else { c.delay += dt; if (c.delay >= 1.0) c.charge = std::min(1.0, c.charge + dt / 4.0); }
    double a = c.thrust * std::max(0.0, po.accel) - 0.15 * c.v - 9.8 * r.slope + (want ? c.boostThrust : 0.0);
    if (po.brake > 0 && c.v > 0.2) a -= 6.0 * po.brake;
    c.v = std::max(-5.0, c.v + a * dt);
    r.vMax = std::max(r.vMax, c.v); r.t += dt;
}
static void Go(Run& r, double secs) { for (double t = 0; t < secs; t += 1.0 / 30) Step(r); }
static bool Until(Run& r, double secs) { for (double t = 0; t < secs && !r.bs.on; t += 1.0 / 30) Step(r); return r.bs.on; }  // run until a boost is on

int main() {
    {   // Thrust-limited: 0.2 grade at 3.5 m/s^2 full pedal plateaus near 10 m/s, under the 14 m/s asked.
        Run r; r.wanted = 14; r.slope = 0.2; Go(r, 12);
        Check(r.starts >= 1, "thrust-limited climb: the boost starts");
        Check(r.vMax > 11.0, "thrust-limited climb: boost lifts the car past what the wheels hold alone");
    }
    {   // The same ask on the flat: the wheels reach it, no boost.
        Run r; r.wanted = 14; Go(r, 20);
        Check(r.starts == 0 && r.boostSeconds == 0, "speed reachable on its own (flat road): no boost");
        Check(r.c.v > 12.5, "flat road: reaches the ask on wheels");
    }
    {   // Pulling away from rest in a normal car gains over the closing rate: no boost.
        Run r; r.wanted = 20; Go(r, 6);
        Check(r.starts == 0, "starting from rest at a normal rate of gain: no boost");
    }
    {   // A weak or heavy car (0.8 m/s^2 at full pedal) from rest: thrust-limited, boosts.
        Run r; r.wanted = 12; r.c.thrust = 0.8; r.c.boostThrust = 4.0; Go(r, 10);
        Check(r.starts >= 1, "heavy car from rest: thrust-limited, the boost starts");
    }
    {   // Stops when the speed is met (not when the meter is empty).
        Run r; r.wanted = 12; r.c.thrust = 0.8; r.c.boostThrust = 4.0; Go(r, 14);
        Check(r.stops >= 1 && r.c.charge > 0.0, "boost ends when the speed is met (the meter is not run empty)");
    }
    {   // Halts: never a boost; and a running boost is cut at once.
        Run r; r.wanted = 14; r.slope = 0.2; Check(Until(r, 20), "setup: boosting on the climb");
        r.held = true; Step(r); Check(!r.bs.on && !std::strcmp(r.lastWhy, "traffic_hold"), "Traffic hold cuts a running boost at once");
        r.held = false; Check(Until(r, 30), "setup: boosting again after the hold");
        r.stopDistance = 30; Step(r); Check(!r.bs.on && !std::strcmp(r.lastWhy, "stop_line_ahead"), "stop line ahead cuts a running boost at once");
        r.stopDistance = -1; Check(Until(r, 30), "setup: boosting again after the stop line");
        r.brake = true; Step(r); Check(!r.bs.on && !std::strcmp(r.lastWhy, "brake_requested"), "brake request cuts a running boost at once");
        r.brake = false; Check(Until(r, 30), "setup: boosting again after the brake");
        r.reverse = true; Step(r); Check(!r.bs.on && !std::strcmp(r.lastWhy, "reverse"), "reverse never boosts");
        r.reverse = false; Check(Until(r, 30), "setup: boosting again after the reverse");
        r.wheelVote = 5.0; Step(r); Check(!r.bs.on && !std::strcmp(r.lastWhy, "wheel_clearance_below_ask"), "wheel-clearance vote under the ask cuts the boost (a clearance zero is never overridden)");
        Check(!r.boostedWhileHalt, "never a boost request while any halt was set");
    }
    {   // Halts from the start: no boost even when thrust-limited.
        const char* names[4] = { "hold from the start: no boost", "brake request from the start: no boost", "stop line from the start: no boost", "zero wheel clearance from the start: no boost" };
        for (int k = 0; k < 4; ++k) {
            Run r; r.wanted = 14; r.slope = 0.2;
            if (k == 0) r.held = true;
            if (k == 1) r.brake = true;
            if (k == 2) r.stopDistance = 20;
            if (k == 3) r.wheelVote = 0.0;
            Go(r, 10); Check(r.starts == 0 && r.boostSeconds == 0, names[k]);
        }
        Run r; r.wanted = 2.0; r.c.thrust = 0.3; Go(r, 10); Check(r.starts == 0, "a slow ask (under the minimum): no boost");
    }
    {   // Meter exhaustion: a long steep climb outlasts the charge; the boost ends with "meter_exhausted" and does not restart until recharged.
        Run r; r.wanted = 25; r.slope = 0.35; r.c.boostThrust = 3.0; bool sawExhausted = false, restartedEarly = false, was = false;
        for (double t = 0; t < 40; t += 1.0 / 30) { const double before = r.c.charge; Step(r);
            if (!std::strcmp(r.lastWhy, "meter_exhausted")) sawExhausted = true;
            if (r.bs.on && !was && before < planbranch::rules::boostMeterOn - 0.01) restartedEarly = true;  // a start below the start level
            was = r.bs.on; }
        Check(sawExhausted, "meter runs out on a long climb: the boost ends, reason meter_exhausted");
        Check(!restartedEarly, "no restart until the meter is back above the start level");
    }
    {   // Hysteresis: a speed at the edge of the gap does not flicker the boost.
        Run r; r.wanted = 14; r.slope = 0.2; int flips = 0; bool last = false;
        for (double t = 0; t < 20; t += 1.0 / 30) { Step(r); if (r.bs.on != last) { ++flips; last = r.bs.on; } }
        Check(flips <= 6, "no flicker: a handful of changes over 20 s, not one per step");
    }
    {   // Minimum run and minimum off time.
        Run r; r.wanted = 14; r.slope = 0.2; double on = -1, off = -1, minRun = 1e9, minOff = 1e9; bool last = false;
        for (double t = 0; t < 30; t += 1.0 / 30) { Step(r);
            if (r.bs.on && !last) { on = r.t; if (off >= 0) minOff = std::min(minOff, on - off); }
            if (!r.bs.on && last) { off = r.t; minRun = std::min(minRun, off - on); }
            last = r.bs.on; }
        Check(minRun >= planbranch::rules::boostMinOn - 0.05, "a boost runs at least the minimum time");
        Check(minOff >= planbranch::rules::boostMinOff - 0.05, "off at least the minimum time before the next boost");
    }
    {   // Unknown evidence: no meter, or no fresh pedal, no boost.
        Run r; r.wanted = 14; r.slope = 0.2; r.meterKnown = false; Go(r, 10);
        Check(r.starts == 0 && r.boostSeconds == 0, "meter unknown: no boost (the game's meter is respected, never guessed)");
        boostneed::State bs; boostneed::Sample s; s.wanted = 14; s.fwd = 8; s.pedal = 1.2; s.pedalFresh = false; s.meterFresh = true; s.charge = 1;
        bool any = false; for (int k = 0; k < 120; ++k) { s.now = k / 30.0; any |= boostneed::Decide(bs, s).boost; }
        Check(!any, "no fresh tyre pedal measurement (velocity-write rail, hover): no boost");
        s.pedalFresh = true; s.pedal = 0.6; any = false; for (int k = 0; k < 120; ++k) { s.now = 5 + k / 30.0; any |= boostneed::Decide(bs, s).boost; }
        Check(!any, "pedal not yet full: no boost");
        s.pedal = 1.2; s.slipDeg = 20; any = false; for (int k = 0; k < 120; ++k) { s.now = 10 + k / 30.0; any |= boostneed::Decide(bs, s).boost; }
        Check(!any, "sliding: no boost");
        s.slipDeg = 0; s.onWheels = false; any = false; for (int k = 0; k < 120; ++k) { s.now = 15 + k / 30.0; any |= boostneed::Decide(bs, s).boost; }
        Check(!any, "not on wheels (hover): no boost");
        s.onWheels = true; s.pedal = NAN; s.wanted = NAN; Check(!boostneed::Decide(bs, s).boost, "no numbers: no boost");
    }
    // ---- BO-2: hover cars (a flying car: hover throttle before its clamp, the pilot's boostlogic::Meter, the speed caps, precise-flight halts) ----
    {   // Toy hover car: thrust = hullAccel x throttle (x boostlogic::kMultiplier while boosting and the meter is active), drag, speed capped at
        // min(cap, plan + 1) like the actuator. Meter: boostlogic::Meter on the native timings (drain 3 s, delay 1 s, recharge 4 s).
        struct Hov {
            double t = 0, v = 0, hullAccel = 1.0, cap = 1e9, wanted = 14; boostneed::State bs; boostlogic::Meter m{ 1.0, 0.0, false, false };
            bool held = false, brake = false; double stopDistance = -1, steer = 0; const char* halt = nullptr; bool allow = true;
            int starts = 0, stops = 0; double boostSeconds = 0, vMax = 0, minCharge = 1; bool boostedWhileHalt = false; const char* why = "";
            bool meterFresh = true;
            void Step(double dt = 1.0 / 30) {
                const double throttleRaw = ((held ? 0.0 : wanted) - v) / 4.0;
                boostneed::Sample s; s.now = t; s.wanted = held ? 0.0 : wanted; s.fwd = v; s.hover = true; s.onWheels = false; s.brake = brake; s.held = held;
                s.stopDistance = stopDistance; s.pedal = throttleRaw; s.pedalFresh = true; s.charge = m.charge; s.meterFresh = meterFresh; s.nativeOn = m.active;
                s.capSpeed = std::min(cap, wanted + 1.0); s.steerAbs = steer; s.hoverHalt = halt;
                const auto d = boostneed::Decide(bs, s); why = d.why;
                if (d.changed) { if (d.boost) ++starts; else ++stops; }
                const bool want = allow && d.boost;
                if (want && (held || brake || stopDistance >= 0 || halt || wanted < 3)) boostedWhileHalt = true;
                m = boostlogic::Step(m.charge, m.delay, want, m.exhausted, dt, 3.0, 1.0, 4.0);
                minCharge = std::min(minCharge, m.charge);
                const double mult = want && m.active ? boostlogic::kMultiplier : 1.0; if (want && m.active) boostSeconds += dt;
                const double throttle = std::clamp(throttleRaw, 0.0, 1.0);
                double a = mult * hullAccel * throttle - 0.01 * v; if (held || brake) a = -8.0 * (v > 0.1);
                v = std::max(0.0, v + a * dt); const double top = std::min(cap, wanted + 1.0); if (v > top) v = top;
                vMax = std::max(vMax, v); t += dt;
            }
            void Go(double secs) { for (double q = 0; q < secs; q += 1.0 / 30) Step(); }
            bool Until(double secs) { for (double q = 0; q < secs && !bs.on; q += 1.0 / 30) Step(); return bs.on; }
        };
        {   Hov h; h.hullAccel = 1.0; h.wanted = 14; h.Go(12);
            Hov n; n.hullAccel = 1.0; n.wanted = 14; n.allow = false; n.Go(12);
            Check(h.starts >= 1, "hover, weak thrust (heavy hull) short of the ask: the boost starts");
            Check(h.v > n.v + 1.0, "hover boost gets a thrust-limited car to the plan's speed sooner than without it");
            Check(h.vMax <= 15.0 + 1e-6, "hover boost adds thrust only: never past the plan's speed + 1");
        }
        {   Hov h; h.hullAccel = 20.0; h.wanted = 30; h.Go(15);
            Check(h.starts == 0 && h.boostSeconds == 0, "hover with ample thrust: reaches the ask, no boost");
        }
        {   Hov h; h.hullAccel = 1.0; h.wanted = 20; h.cap = 8.0; h.Go(12);
            Check(h.starts == 0 && !std::strcmp(h.why, "speed_cap_below_ask"), "hover held by a cap (ground not loaded, a gear top) under the ask: no boost, reason speed_cap_below_ask");
        }
        {   const char* reasons[5] = { "landing", "take_off", "strafe_plan", "holding_spot", "tight_turn" };
            bool any = false; for (const char* r : reasons) { Hov h; h.hullAccel = 1.0; h.wanted = 14; h.halt = r; h.Go(8); any |= h.starts > 0; }
            Check(!any, "hover: precise flight (landing, take-off, strafe, held spot, gear held low for a tight turn) never boosts");
            Hov h; h.hullAccel = 1.0; h.wanted = 14; h.steer = 0.9; h.Go(8); Check(h.starts == 0, "hover: hard yaw demand never boosts");
        }
        {   Hov h; h.hullAccel = 1.0; h.wanted = 14; Check(h.Until(20), "setup: hover boosting");
            h.held = true; h.Step(); Check(!h.bs.on && !std::strcmp(h.why, "traffic_hold"), "hover: Traffic hold cuts a running boost at once");
            h.held = false; Check(h.Until(30), "setup: hover boosting again");
            h.stopDistance = 30; h.Step(); Check(!h.bs.on && !std::strcmp(h.why, "stop_line_ahead"), "hover: stop line ahead cuts a running boost at once");
            h.stopDistance = -1; Check(h.Until(30), "setup: hover boosting again after the stop line");
            h.brake = true; h.Step(); Check(!h.bs.on && !std::strcmp(h.why, "brake_requested"), "hover: brake request cuts a running boost at once");
            h.brake = false; Check(h.Until(30), "setup: hover boosting again after the brake");
            h.halt = "near_obstacle"; h.Step(); Check(!h.bs.on && !std::strcmp(h.why, "near_obstacle"), "hover: precise flight cuts a running boost at once");
            Check(!h.boostedWhileHalt, "hover: never a boost request while any halt was set");
        }
        {   Hov h; h.hullAccel = 0.8; h.wanted = 60; h.Go(60);   // a long thrust-limited run outlasts the meter
            Check(h.minCharge >= 0.0 && h.minCharge < 0.35, "hover meter drains under boost and is bounded at 0 (never pinned)");
            Check(h.boostSeconds < 60 * 0.6, "hover boost is bounded by the meter: it does not run the whole time");
            Check(h.stops >= 1, "hover boost ends when the meter runs out and restarts only after recharge");
            Hov r; r.hullAccel = 0.8; r.wanted = 60; bool early = false, was = false;
            for (double q = 0; q < 60; q += 1.0 / 30) { const double before = r.m.charge; r.Step(); if (r.bs.on && !was && before < planbranch::rules::boostMeterOn - 0.01) early = true; was = r.bs.on; }
            Check(!early, "hover: no restart below the start charge");
        }
        {   Hov h; h.hullAccel = 1.0; h.wanted = 14; h.meterFresh = false; h.Go(10);
            Check(h.starts == 0, "hover: meter unknown: no boost");
            boostneed::State bs; boostneed::Sample s; s.hover = true; s.onWheels = false; s.wanted = 14; s.fwd = 5; s.pedal = 2.0; s.pedalFresh = false; s.meterFresh = true; s.charge = 1;
            bool any = false; for (int k = 0; k < 120; ++k) { s.now = k / 30.0; any |= boostneed::Decide(bs, s).boost; }
            Check(!any, "hover: no fresh throttle measurement: no boost");
            s.pedalFresh = true; s.reverse = true; any = false; for (int k = 0; k < 120; ++k) { s.now = 9 + k / 30.0; any |= boostneed::Decide(bs, s).boost; }
            Check(!any, "hover: reverse never boosts");
        }
        {   Hov h; h.hullAccel = 1.0; h.wanted = 14; int flips = 0; bool last = false;
            for (double q = 0; q < 30; q += 1.0 / 30) { h.Step(); if (h.bs.on != last) { ++flips; last = h.bs.on; } }
            Check(flips <= 8, "hover: no flicker over 30 s");
        }
    }
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
