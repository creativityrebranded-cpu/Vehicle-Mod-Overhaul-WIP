#pragma once
// RV-1 (2026-11-19, user: "propose the optimal node system and then just replace entire thing at once"):
// the autopilot's mode state machine plus an explicit get-unstuck routine.
//
// It replaces three older pieces as one unit:
//   - the SL-1 stuck ladder, which only loosened ten plan cost weights in steps at 6/12/18 s of no
//     progress. Loosened weights let the plan flip between forward and reverse every few ticks
//     (run logs: 104 swaps into and out of reverse in 60 s, 24 s stuck nose-on against a wall);
//   - the LD-1 layer flag (lane-centre layer on/off);
//   - the RH-1 ladder timers.
//
// Modes:
//   Follow  - drive the lane centre (default).
//   Avoid   - the full plan search is allowed (centre blocked, no lane, or a flight stretch).
//   Recover - an explicit routine: BackUp -> TurnToLane -> Retry, each step held until its own end
//             condition. AP-1 (2026-11-21): BackUp reverses steering the nose toward the lane and TurnToLane turns on full lock
//             until within 10 deg of it; a TurnToLane whose own plan is blocked backs up again (maxCycles per try), a real
//             multi-point turn. A managed car whose every forward plan is blocked while a reverse is clear starts the routine at once.
//             condition, never swapped back and forth. After maxTries tries without progress the
//             routine ends in a Hover step (when hover is allowed) or Stand.
//   Stand   - give up for now (nothing safe to do, or the routine failed maxTries times); retries
//             Avoid every standRetry s. `why` says which.
//
// Progress: Input::progress (metres gained along the route since the last tick, negative when
// reversing) is summed from an anchor. When the sum reaches progressMin the anchor moves to the
// car, the no-progress clock restarts and tries go back to 0. Backing up therefore has to be won
// back before anything counts as progress, so driving back to the same wall never resets tries.
//
// Pure logic: no game calls, no globals, no logging. The caller feeds one Input per tick and acts
// on the Command; a Recover command carries its own signed speed and steer.

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace recovery {

enum class Mode { Follow, Avoid, Recover, Stand };

struct Input {
    double now = 0.0;                       // s
    bool haveLane = true;
    bool flightStretch = false;
    bool centreFeasible = true;
    bool centreAtWantedSpeedBlocked = false;
    bool anyFeasible = true;
    bool reverseFeasible = true;
    double progress = 0.0;                  // metres gained along the route since the last tick (can be negative)
    double speed = 0.0;                     // m/s, measured
    double headingErrToLane = 0.0;          // rad, car heading minus lane heading at the car
    bool hoverAllowed = false;
    bool meantToStand = false;              // no wanted speed, or a stop line within 10 m
    bool continuous = false;               // a managed lane keeps searching instead of entering a timed Stand
    bool floorRetry = false;                // a verified point left the scene and the full centre candidate is feasible
    double roadGrade = 0.0;                 // SP-1: rise over run of the road just ahead along the travel (World's lane floor)
    bool wheels = true;
    bool legForwardClear = true;
    bool legForwardFaces = false;
    bool legReverseClear = true;
    bool forwardBlocked = false;            // AP-1: no forward plan is clear
};

struct Command {
    Mode mode = Mode::Follow;
    enum class Step { None, BackUp, TurnToLane, Retry, Hover };
    Step step = Step::None;
    double wantSpeed = std::numeric_limits<double>::quiet_NaN();  // signed m/s for Recover steps, NaN otherwise
    double steer = std::numeric_limits<double>::quiet_NaN();      // -1..1 for Recover steps, NaN otherwise
    const char* why = "start";
    bool changed = false;                   // mode or step changed this tick
};

struct Rules {
    double avoidHold = 1.5;          // s the centre must stay clear before Avoid hands back to Follow
    double noProgress = 6.0;         // s without progressMin metres before Recover
    double progressMin = 2.0;        // m that count as progress
    double explainedGrade = .15;     // SP-1: a climb at least this steep explains a slow car
    double explainedSpeed = .1;      // SP-1: m/s forward that still counts as moving on it
    double backUpMetres = 5.0;       // m reversed in BackUp (AP-1: 3 m straight gave no room to turn)
    double backUpSpeed = 1.5;        // m/s
    double backUpMax = 5.0;          // s
    double backUpSteerGain = 3.0;    // AP-1: reverse steer per rad of heading error (toward the lane; full lock from 0.33 rad)
    double turnSpeed = 1.5;          // m/s forward while turning to the lane
    double turnDone = 0.17;          // rad heading error that ends TurnToLane (AP-1: 10 deg; 20 deg left cars pointing at the kerb)
    double turnMax = 10.0;           // s
    double turnSteerGain = 4.0;      // AP-1: forward steer per rad (full lock from 0.25 rad; was 2)
    double kturnFrom = 1.04;         // rad, approximately 60 degrees
    double kturnBackMetres = 2.7;    // m before a high-angle reverse leg must switch
    double kturnRestSpeed = .15;    // m/s below which lane-centre cost cannot suppress recovery
    // Longest first. The planner still needs room to stop at the end of any prefix.
    std::array<double, 10> kturnPrefixes = {5.0, 3.6, 2.7, 2.1, 1.6, 1.2, .9, .6, .35, .15};
    double minBackForTurn = .8;      // m before switching early from reverse to a clear forward turn
    int maxCycles = 4;               // extra back-up/turn cycles in one try
    int maxTries = 3;
    double standRetry = 5.0;         // s in Stand before Avoid is tried again
    double nothingFeasibleFor = 2.0; // s with no feasible plan before Recover
};

struct State {
    Mode mode = Mode::Follow;
    Command::Step step = Command::Step::None;
    const char* why = "start";
    double lastNow = std::numeric_limits<double>::quiet_NaN();
    double modeSince = std::numeric_limits<double>::quiet_NaN();
    double stepSince = std::numeric_limits<double>::quiet_NaN();
    double clearSince = std::numeric_limits<double>::quiet_NaN();   // centre feasible and unblocked since
    double noneSince = std::numeric_limits<double>::quiet_NaN();    // nothing feasible since
    double progressSince = std::numeric_limits<double>::quiet_NaN();// no-progress clock start
    double progressSum = 0.0;  // metres since the anchor
    double backed = 0.0;       // metres reversed in the current BackUp step
    int tries = 0;             // Recover entries since the last progress
    int cycles = 0;            // back-up/turn cycles in the current try
    int turnSign = 0;
    double startErr = 0.0;
};

inline double LegError(const State& s, double err) {
    constexpr double pi = 3.14159265358979323846;
    if (std::fabs(err) > pi / 2 && s.turnSign * err < 0)
        return err + s.turnSign * 2 * pi;
    return err;
}

inline double LegSteer(const State& s, double err, Command::Step step, const Rules& r = {}) {
    if (!std::isfinite(err)) return 0;
    const double gain = step == Command::Step::BackUp ? r.backUpSteerGain : -r.turnSteerGain;
    return std::clamp(LegError(s, err) * gain, -1.0, 1.0);
}

inline bool HighAngleTurn(const State& s, double err, const Rules& r) {
    return std::fabs(err) >= r.kturnFrom ||
        (s.mode == Mode::Recover && std::fabs(s.startErr) >= r.kturnFrom);
}

inline double BackUpMetres(const State& s, double err, const Rules& r) {
    const double start = s.mode == Mode::Recover ? s.startErr : err;
    return std::fabs(start) >= r.kturnFrom ? r.kturnBackMetres : r.backUpMetres;
}

inline Command Update(State& s, const Input& in, const Rules& r = {}) {
    using Step = Command::Step;
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    const double now = in.now;

    const double dt = std::isnan(s.lastNow) ? 0.0 : std::clamp(now - s.lastNow, 0.0, 0.5);
    s.lastNow = now;
    if (std::isnan(s.modeSince)) s.modeSince = now;
    if (std::isnan(s.stepSince)) s.stepSince = now;
    if (std::isnan(s.progressSince)) s.progressSince = now;
    const Mode oldMode = s.mode;
    const Step oldStep = s.step;

    // Sensed clocks.
    const bool centreClear = in.haveLane && in.centreFeasible && !in.centreAtWantedSpeedBlocked;
    if (centreClear) { if (std::isnan(s.clearSince)) s.clearSince = now; }
    else s.clearSince = nan;
    if (!in.anyFeasible) { if (std::isnan(s.noneSince)) s.noneSince = now; }
    else s.noneSince = nan;

    // Progress anchor: progressMin metres net since the anchor resets the clock and the tries.
    if (std::isfinite(in.progress) && !(in.speed < -.05 && in.progress > 0))
        s.progressSum += in.progress;
    bool madeProgress = false;
    if (s.progressSum >= r.progressMin) {
        madeProgress = true;
        s.progressSum = 0.0;
        s.progressSince = now;
        s.tries = 0;
    }

    auto enter = [&](Mode m, Step st, const char* why) {
        if (m != s.mode) s.modeSince = now;
        if (m != s.mode || st != s.step) s.stepSince = now;
        s.mode = m;
        if (m != Mode::Recover) s.turnSign = 0;
        s.step = st;
        s.why = why;
    };
    auto followOrAvoid = [&](const char* followWhy, const char* avoidWhy) {
        if (centreClear) enter(Mode::Follow, Step::None, followWhy);
        else enter(Mode::Avoid, Step::None, avoidWhy);
    };
    const bool offHeading = in.haveLane && std::fabs(in.headingErrToLane) >= r.turnDone;
    auto startRecover = [&](const char* why) {
        s.progressSince = now;
        s.backed = 0.0;
        s.cycles = 0;
        if (!in.anyFeasible && !in.reverseFeasible && !(offHeading && in.legForwardClear)) {
            if (in.continuous) enter(Mode::Avoid, Step::None, "stalled_no_feasible_maneuver");
            else enter(Mode::Stand, Step::None, "nothing_safe");
            return;
        }
        if (s.tries >= r.maxTries) {
            if (in.hoverAllowed) enter(Mode::Recover, Step::Hover, "three_tries_hover");
            else if (in.continuous) enter(Mode::Avoid, Step::None, "search_alternate_maneuver");
            else enter(Mode::Stand, Step::None, "three_tries_stand");
            return;
        }
        ++s.tries;
        s.turnSign = (in.headingErrToLane > 0) - (in.headingErrToLane < 0);
        s.startErr = in.headingErrToLane;
        if (offHeading && in.legForwardClear && in.legForwardFaces)
            enter(Mode::Recover, Step::TurnToLane, why);
        else if (in.reverseFeasible && in.legReverseClear)
            enter(Mode::Recover, Step::BackUp, why);
        else if (offHeading && in.legForwardClear)
            enter(Mode::Recover, Step::TurnToLane, why);
        else
            enter(Mode::Recover, Step::Retry, "no_clear_leg");
    };
    // SP-1 (2026-11-19): slowness the road explains is not "no progress". On a measured climb, with the centre clear and the car still
    // moving forward, the no-progress clock holds (13:20 run: the rover climbed at 0.3 m/s and recovery backed it down the ramp).
    if (in.roadGrade >= r.explainedGrade && in.speed >= r.explainedSpeed && in.centreFeasible) s.progressSince = now;
    const bool noProgressLong = now - s.progressSince >= r.noProgress;
    const bool nothingLong = !std::isnan(s.noneSince) && now - s.noneSince >= r.nothingFeasibleFor;
    // AP-1: a managed car nose-on to something with a clear way back reverses now, as one held step (17:36 run: FF1000DE swapped
    // between forward and reverse plans every 0.3-0.7 s for 15 s and never gained room).
    const bool forwardStuck = in.continuous && in.forwardBlocked && in.reverseFeasible && s.tries < r.maxTries;

    const bool turnRound = in.wheels && in.haveLane && std::fabs(in.headingErrToLane) >= r.kturnFrom &&
        (!centreClear || std::fabs(in.speed) < r.kturnRestSpeed) && s.tries < r.maxTries;

    if (in.meantToStand) {
        // Rule 1: never recovers while it is supposed to wait.
        s.tries = 0;
        s.progressSum = 0.0;
        s.progressSince = now;
        s.backed = 0.0;
        enter(Mode::Follow, Step::None, "meant_to_stand");
    } else if (in.flightStretch) {
        // Rule 2: flight stretches plan freely; no ground routine in the air.
        s.progressSince = now;
        enter(Mode::Avoid, Step::None, "flight_stretch");
    } else {
        switch (s.mode) {
        case Mode::Follow:
            if (forwardStuck) startRecover("forward_blocked_reverse_clear");
            else if (turnRound) startRecover("off_heading_kturn");
            else if (!in.haveLane) enter(Mode::Avoid, Step::None, "no_lane");
            else if (in.centreAtWantedSpeedBlocked) enter(Mode::Avoid, Step::None, "centre_blocked");
            else if (!in.centreFeasible) enter(Mode::Avoid, Step::None, "centre_infeasible");
            else if (noProgressLong) startRecover("no_progress_6s");  // centre "clear" but the car is held
            break;
        case Mode::Avoid:
            if (forwardStuck) startRecover("forward_blocked_reverse_clear");
            else if (turnRound) startRecover("off_heading_kturn");
            else if (!std::isnan(s.clearSince) && now - s.clearSince >= r.avoidHold)
                enter(Mode::Follow, Step::None, "centre_clear_long_enough");
            else if (noProgressLong) startRecover("no_progress_6s");
            else if (nothingLong) startRecover("nothing_feasible_2s");
            break;
        case Mode::Recover:
            switch (s.step) {
            case Step::BackUp:
                s.backed += std::max(0.0, -in.speed) * dt;
                if (in.haveLane && std::fabs(s.startErr) >= r.kturnFrom && !offHeading)
                    enter(Mode::Recover, Step::Retry, "faced_while_backing");
                else if (offHeading && in.legForwardClear && in.legForwardFaces && s.backed >= r.minBackForTurn)
                    enter(Mode::Recover, Step::TurnToLane, "forward_turn_fits");
                else if (s.backed >= BackUpMetres(s, in.headingErrToLane, r))
                    enter(Mode::Recover, Step::TurnToLane, "backed_up");
                else if (now - s.stepSince >= r.backUpMax)
                    enter(Mode::Recover, Step::TurnToLane, "back_up_time_up");
                else if (!in.reverseFeasible || !in.legReverseClear) {
                    if (in.legForwardClear || !offHeading)
                        enter(Mode::Recover, Step::TurnToLane, "reverse_blocked");
                    else
                        enter(Mode::Recover, Step::Retry, "both_legs_blocked");
                }
                break;
            case Step::TurnToLane:
                if (!in.haveLane) enter(Mode::Recover, Step::Retry, "no_lane_to_face");
                else if (!offHeading) enter(Mode::Recover, Step::Retry, "facing_the_lane");
                else if (!in.legForwardClear && s.cycles < r.maxCycles && in.legReverseClear) {
                    ++s.cycles; s.backed = 0.0;
                    enter(Mode::Recover, Step::BackUp, "turn_blocked_back_up_again");
                }
                else if (!in.legForwardClear) enter(Mode::Recover, Step::Retry, "turn_blocked");
                else if (now - s.stepSince >= r.turnMax) enter(Mode::Recover, Step::Retry, "turn_time_up");
                break;
            case Step::Retry:
                // One tick; the try gets a fresh no-progress clock but keeps the anchor.
                s.progressSince = now;
                followOrAvoid("retry_follow", "retry_avoid");
                break;
            case Step::Hover:
                if (madeProgress) followOrAvoid("hover_progress", "hover_progress");
                else if (!in.hoverAllowed) enter(Mode::Stand, Step::None, "hover_not_allowed");
                else if (noProgressLong) enter(Mode::Stand, Step::None, "hover_no_progress");
                break;
            case Step::None:
                followOrAvoid("recover_without_step", "recover_without_step");
                break;
            }
            break;
        case Mode::Stand:
            if (in.continuous) followOrAvoid("managed_resume_follow", "managed_resume_avoid");
            else if (madeProgress) enter(Mode::Follow, Step::None, "progress");
            else if (in.floorRetry && centreClear) {
                s.progressSince = now;
                enter(Mode::Follow, Step::None, "verified_floor_retry");
            }
            else if (now - s.modeSince >= r.standRetry) {
                s.progressSince = now;
                enter(Mode::Avoid, Step::None, "stand_retry");
            }
            break;
        }
    }

    Command c;
    c.mode = s.mode;
    c.step = s.step;
    c.why = s.why;
    c.changed = s.mode != oldMode || s.step != oldStep;
    if (s.mode == Mode::Recover) {
        switch (s.step) {
        case Step::BackUp:  // AP-1: reversing, the wheel turns the nose the other way: +error steers it back toward the lane
            c.wantSpeed = -r.backUpSpeed;
            c.steer = LegSteer(s, in.headingErrToLane, s.step, r);
            break;
        case Step::TurnToLane:
            c.wantSpeed = r.turnSpeed;
            c.steer = LegSteer(s, in.headingErrToLane, s.step, r);
            break;
        case Step::Retry: c.wantSpeed = r.turnSpeed; c.steer = 0.0; break;  // keep rolling through the hand-over tick
        case Step::Hover: c.wantSpeed = r.turnSpeed; c.steer = 0.0; break;
        case Step::None: break;
        }
    }
    return c;
}

}  // namespace recovery
