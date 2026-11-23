// RV-1 (2026-11-19): offline proof of the autopilot mode machine and get-unstuck routine (recovery.h).
// A tiny fake world drives one car along a straight route at 0.1 s ticks: the fake planner holds
// cruise speed in Follow/Avoid, a Recover command is applied as given, and an optional wall stops the
// car (hovering lifts it over). Offline check only, not in-game proof.
#include "recovery.h"
#include <cstdio>
#include <cmath>
#include <string>
#include <algorithm>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

namespace {
using namespace recovery;
using Step = Command::Step;
constexpr double kDt = 0.1;

struct Sim {
    State st;
    Rules rules;
    int tick = 0;
    double t = 0.0, s = 0.0, h = 0.0, lastDs = 0.0, lastSpeed = 0.0, cruise = 5.0;
    // world
    double wall = 1e9; bool wallOn = false;
    bool anyFeasible = true, reverseFeasible = true, hoverAllowed = true, haveLane = true, flight = false;
    bool forceBlocked = false, meantToStand = false;
    // record
    Command last{};
    std::string trail;          // one letter per mode/step change: F A B T R H S
    int recoverTicks = 0, reverseSwaps = 0, changes = 0;
    double prevWant = 0.0;
    double firstRecoverAt = -1.0; const char* firstRecoverWhy = "";
    double backedPath = 0.0, maxBackedPath = 0.0, minBackedPath = 1e9; bool backUpSteerZero = true;
    double backUpHeadingDrift = 0.0, hAtBackUp = 0.0;
    double wallHitAt = -1.0;

    static char Letter(const Command& c) {
        switch (c.mode) {
        case Mode::Follow: return 'F';
        case Mode::Avoid: return 'A';
        case Mode::Stand: return 'S';
        case Mode::Recover:
            switch (c.step) {
            case Step::BackUp: return 'B';
            case Step::TurnToLane: return 'T';
            case Step::Retry: return 'R';
            case Step::Hover: return 'H';
            default: return '?';
            }
        }
        return '?';
    }

    Command Tick() {
        t = tick * kDt;
        Input in{};
        in.now = t;
        in.haveLane = haveLane;
        in.flightStretch = flight;
        const bool ahead = wallOn && s <= wall;  // pinned at the wall counts as ahead
        in.centreAtWantedSpeedBlocked = forceBlocked || (ahead && wall - s < 8.0);
        in.centreFeasible = anyFeasible && !(ahead && wall - s < 3.0);
        in.anyFeasible = anyFeasible;
        in.reverseFeasible = reverseFeasible;
        in.progress = lastDs;
        in.speed = lastSpeed;
        in.headingErrToLane = h;
        in.hoverAllowed = hoverAllowed;
        in.meantToStand = meantToStand;
        const Command c = Update(st, in, rules);

        if (c.changed) {
            ++changes;
            trail += Letter(c);
            if (c.mode == Mode::Recover && c.step == Step::BackUp) { backedPath = 0.0; hAtBackUp = h; }
            if (c.mode == Mode::Recover && firstRecoverAt < 0.0) { firstRecoverAt = t; firstRecoverWhy = c.why; }
            if (last.mode == Mode::Recover && last.step == Step::BackUp) {
                maxBackedPath = std::max(maxBackedPath, backedPath);
                minBackedPath = std::min(minBackedPath, backedPath);
                backUpHeadingDrift = std::max(backUpHeadingDrift, std::fabs(h - hAtBackUp));
            }
        }
        if (c.mode == Mode::Recover) ++recoverTicks;

        // fake car
        double v = 0.0, steer = 0.0; bool lift = false;
        switch (c.mode) {
        case Mode::Follow: case Mode::Avoid: v = (meantToStand || !anyFeasible) ? 0.0 : cruise; break;
        case Mode::Stand: v = 0.0; break;
        case Mode::Recover: v = c.wantSpeed; steer = c.steer; lift = c.step == Step::Hover; break;
        }
        if (c.mode == Mode::Recover && c.step == Step::BackUp && c.steer != 0.0) backUpSteerZero = false;
        if (v < 0.0 && prevWant >= 0.0) ++reverseSwaps;
        prevWant = v;
        h += steer * 0.8 * kDt * (v < 0.0 ? -1.0 : 1.0);  // AP-1: reversing, the same wheel turns the nose the other way
        double ds = v * std::cos(h) * kDt;
        if (wallOn && !lift && s <= wall && s + ds > wall) {
            ds = std::max(0.0, wall - s);
            if (wallHitAt < 0.0) wallHitAt = t;
        }
        s += ds;
        lastDs = ds;
        lastSpeed = ds / (std::cos(h) * kDt);
        if (c.mode == Mode::Recover && c.step == Step::BackUp) backedPath += std::fabs(lastSpeed) * kDt;
        last = c;
        ++tick;
        return c;
    }
    void RunFor(double seconds) { const int n = (int)std::lround(seconds / kDt); for (int i = 0; i < n; ++i) Tick(); }
    template <class F> bool RunUntil(double maxSeconds, F done) {
        const int n = (int)std::lround(maxSeconds / kDt);
        for (int i = 0; i < n; ++i) { Tick(); if (done(last)) return true; }
        return false;
    }
};

// Every BackUp is followed by exactly one TurnToLane then one Retry before anything else.
bool RoutineHeld(const std::string& trail) {
    for (size_t i = 0; i < trail.size(); ++i) {
        if (trail[i] != 'B') continue;
        if (i + 2 >= trail.size() || trail[i + 1] != 'T' || trail[i + 2] != 'R') return false;
    }
    return true;
}
int CountOf(const std::string& s, char c) { return (int)std::count(s.begin(), s.end(), c); }
}  // namespace

int main() {
    {   // SP-1 (2026-11-19): a slow climb the road explains is not "no progress"; the same crawl on the flat is.
        recovery::State climb, flat; recovery::Command c1, c2;
        for (double t = 0; t <= 10.0; t += .1) {
            recovery::Input in; in.now = t; in.haveLane = true; in.centreFeasible = true; in.speed = .3; in.progress = .03;
            in.roadGrade = .42; c1 = recovery::Update(climb, in);
            in.roadGrade = 0; c2 = recovery::Update(flat, in);
        }
        Check(climb.mode == recovery::Mode::Follow, "SP-1: 10 s crawling up a 23 deg climb with the centre clear: still Follow");
        Check(flat.mode == recovery::Mode::Recover, "SP-1: the same crawl on the flat: Recover (no progress)");
    }
    {
        recovery::State state;
        state.mode = recovery::Mode::Stand;
        state.modeSince = 10.0;
        recovery::Input sensed;
        sensed.now = 10.2;
        sensed.floorRetry = true;
        sensed.centreAtWantedSpeedBlocked = true;
        Check(recovery::Update(state, sensed).mode == recovery::Mode::Stand,
              "a cleared floor point does not override a remaining centre blocker");
        sensed.centreAtWantedSpeedBlocked = false;
        sensed.now = 10.4;
        sensed.meantToStand = true;
        const auto held = recovery::Update(state, sensed);
        Check(std::string(held.why) == "meant_to_stand", "floor verification cannot override an intentional hold");
        state.mode = recovery::Mode::Stand;
        sensed.now = 10.6;
        sensed.meantToStand = false;
        sensed.floorRetry = false;
        Check(recovery::Update(state, sensed).mode == recovery::Mode::Stand,
              "without floor proof the normal Stand retry delay remains");
        sensed.now = 10.8;
        sensed.floorRetry = true;
        const auto released = recovery::Update(state, sensed);
        Check(released.mode == recovery::Mode::Follow && std::string(released.why) == "verified_floor_retry",
              "verified floor and clear centre permit an early retry");
    }
    using namespace recovery;

    {   // open lane stays Follow
        Sim w;
        w.RunFor(30.0);
        Check(w.changes == 0 && w.last.mode == Mode::Follow, "open lane: 30 s in Follow, no mode change");
        Check(std::isnan(w.last.wantSpeed) && std::isnan(w.last.steer), "open lane: Follow carries no speed/steer (NaN)");
    }

    {   // blocked centre -> Avoid; back to Follow only after 1.5 s continuously clear
        Sim w;
        w.RunFor(1.0);
        w.forceBlocked = true;
        w.Tick();
        Check(w.last.mode == Mode::Avoid && std::string(w.last.why) == "centre_blocked" && w.last.changed,
              "blocked centre: Avoid at once, why centre_blocked");
        w.RunFor(2.0);   // blocked until t=3.1
        w.forceBlocked = false;
        w.RunFor(0.8);   // clear 0.8 s
        w.forceBlocked = true;
        w.RunFor(0.2);   // blip
        w.forceBlocked = false;
        const double clearAt = w.tick * kDt;
        bool early = false;
        w.RunUntil(5.0, [&](const Command& c) {
            if (c.mode == Mode::Follow && w.t < clearAt + 1.45) early = true;
            return c.mode == Mode::Follow;
        });
        Check(CountOf(w.trail, 'F') == 1 && !early, "blocked centre: the 0.8 s clear gap did not return to Follow");
        Check(w.last.mode == Mode::Follow && std::fabs(w.t - (clearAt + 1.5)) <= 0.15 &&
              std::string(w.last.why) == "centre_clear_long_enough",
              "blocked centre: Follow 1.5 s after the centre cleared");
        Check(w.firstRecoverAt < 0.0, "blocked centre: still moving, never Recover");
    }

    {   // nose against a wall, angled 0.6 rad off the lane; the way clears after the first try
        Sim w;
        w.h = 0.6; w.wallOn = true; w.wall = 20.0;
        w.RunUntil(30.0, [](const Command& c) { return c.mode == Mode::Recover; });
        Check(w.wallHitAt > 0.0 && w.firstRecoverAt > 0.0, "wall: car hit the wall and entered Recover");
        const double wait = w.firstRecoverAt - w.wallHitAt;
        std::printf("     wall hit %.1f s, Recover %.1f s (%s), %.1f s later\n", w.wallHitAt, w.firstRecoverAt, w.firstRecoverWhy, wait);
        Check(wait >= 5.0 && wait <= 6.3 && std::string(w.firstRecoverWhy) == "no_progress_6s",
              "wall: Recover after ~6 s without progress, why no_progress_6s");
        Check(w.last.step == Step::BackUp && w.last.wantSpeed < 0.0 && w.last.steer > 0.0 && w.st.tries == 1,
              "wall: first step BackUp, reverse at 1.5 m/s steering the nose toward the lane, try 1");
        w.RunUntil(10.0, [](const Command& c) { return c.step == Step::TurnToLane; });
        std::printf("     backed %.2f m, heading drift %.3f rad, why %s\n", w.maxBackedPath, w.backUpHeadingDrift, w.last.why);
        Check(w.maxBackedPath >= 4.9 && w.maxBackedPath <= 5.3 && !w.backUpSteerZero && w.backUpHeadingDrift > 0.3 &&
              std::string(w.last.why) == "backed_up",
              "wall: backed up ~5 m turning toward the lane (AP-1), then TurnToLane (backed_up)");
        Check(w.last.steer < 0.0 && w.last.wantSpeed > 0.0, "wall: TurnToLane steers toward the lane, rolling forward");
        w.RunUntil(10.0, [](const Command& c) { return c.step == Step::Retry; });
        Check(std::fabs(w.h) < w.rules.turnDone && std::string(w.last.why) == "facing_the_lane",
              "wall: turned until facing the lane, then Retry");
        w.wallOn = false;   // the way clears
        w.RunFor(15.0);
        Check(w.last.mode == Mode::Follow && w.st.tries == 0 && w.s > 40.0,
              "wall: way cleared -> back in Follow, tries reset, car moving on");
        Check(CountOf(w.trail, 'B') == 1 && RoutineHeld(w.trail), "wall: one try, steps B->T->R held in order");
        std::printf("     trail %s\n", w.trail.c_str());
    }

    {   // permanent wall, hover allowed: three tries then Hover over it
        Sim w;
        w.h = 0.6; w.wallOn = true; w.wall = 20.0; w.hoverAllowed = true;
        w.RunFor(60.0);
        std::printf("     trail %s, reverse swaps %d in 60 s, last why %s\n", w.trail.c_str(), w.reverseSwaps, w.last.why);
        Check(CountOf(w.trail, 'B') == 3 && CountOf(w.trail, 'T') == 3 && RoutineHeld(w.trail),
              "three tries: exactly 3 BackUp and 3 TurnToLane, never alternated within a try");
        Check(w.reverseSwaps == 3, "three tries: 3 swaps into reverse in 60 s (log had 104)");
        Check(CountOf(w.trail, 'H') == 1 && w.trail.find('H') > w.trail.rfind('B'), "three tries: Hover after the third try");
        Check(w.s > w.wall && w.last.mode == Mode::Follow && w.st.tries == 0, "three tries: hovered past the wall, back in Follow, tries 0");
    }

    {   // permanent wall, hover not allowed: Stand, retried every 5 s, released by progress
        Sim w;
        w.h = 0.6; w.wallOn = true; w.wall = 20.0; w.hoverAllowed = false;
        w.RunUntil(60.0, [](const Command& c) { return c.mode == Mode::Stand; });
        Check(w.last.mode == Mode::Stand && std::string(w.last.why) == "three_tries_stand" && CountOf(w.trail, 'B') == 3,
              "no hover: Stand after three tries (three_tries_stand)");
        const double standAt = w.t;
        w.RunUntil(10.0, [](const Command& c) { return c.mode != Mode::Stand; });
        Check(w.last.mode == Mode::Avoid && std::string(w.last.why) == "stand_retry" && std::fabs(w.t - standAt - 5.0) <= 0.15,
              "no hover: Stand retries Avoid after 5 s");
        w.RunFor(20.0);
        Check(CountOf(w.trail, 'B') == 3, "no hover: no fourth BackUp after the tries ran out");
        w.wallOn = false;
        w.RunFor(10.0);
        Check(w.last.mode == Mode::Follow && w.st.tries == 0, "no hover: way cleared -> progress resets to Follow, tries 0");
        std::printf("     trail %s\n", w.trail.c_str());
    }

    {   // meant to stand at a stop line with the centre blocked: never Recover
        Sim w;
        w.RunFor(2.0);
        w.meantToStand = true;
        w.forceBlocked = true;
        w.RunFor(40.0);
        Check(w.firstRecoverAt < 0.0 && w.last.mode == Mode::Follow && std::string(w.last.why) == "meant_to_stand",
              "stop line: 40 s waiting, never Recover, stays Follow");
        w.meantToStand = false;
        w.forceBlocked = false;
        w.RunFor(10.0);
        Check(w.firstRecoverAt < 0.0 && w.last.mode == Mode::Follow, "stop line: drives on after release");
    }

    {   // AP-1: a managed car nose-on with a clear way back reverses at once; a blocked turn backs up again (multi-point turn)
        State st; Input in; in.now = 1.0; in.continuous = true; in.forwardBlocked = true; in.reverseFeasible = true; in.headingErrToLane = 0.6;
        auto c = Update(st, in);
        Check(c.mode == Mode::Recover && c.step == Step::BackUp && std::string(c.why) == "forward_blocked_reverse_clear" && c.steer > 0.0,
              "AP-1: forward blocked, reverse clear: BackUp at once, steering toward the lane");
        in.forwardBlocked = false; in.legReverseClear = false; in.now = 1.1; c = Update(st, in);
        Check(c.step == Step::TurnToLane && std::string(c.why) == "reverse_blocked", "AP-1: a blocked back-up step ends: TurnToLane");
        Check(c.steer <= -1.0 + 1e-9, "AP-1: TurnToLane at 0.6 rad is full lock");
        std::string seq;
        for (int i = 0; i < recovery::Rules{}.maxCycles; ++i) {
            in.legForwardClear = false; in.legReverseClear = true; in.now += .1;
            c = Update(st, in); seq += c.step == Step::BackUp ? 'B' : '?';
            in.legForwardClear = true; in.legReverseClear = false; in.now += .1;
            c = Update(st, in); seq += c.step == Step::TurnToLane ? 'T' : '?';
        }
        in.legForwardClear = false; in.legReverseClear = true; in.now += .1;
        c = Update(st, in);
        Check(seq == "BTBTBTBT" && c.step == Step::Retry && std::string(c.why) == "turn_blocked",
              "AP-2: blocked turn: back up again four times, then Retry");
        std::printf("     cycles %s\n", seq.c_str());
        State q; Input j; j.now = 1.0; j.continuous = false; j.forwardBlocked = true; j.reverseFeasible = true;
        Check(Update(q, j).mode == Mode::Follow, "AP-1: an unmanaged car (player autopilot) keeps the 6 s no-progress rule");
    }

    {
        State state;
        Input in;
        in.centreAtWantedSpeedBlocked = true;
        in.headingErrToLane = 2.9;
        in.legForwardFaces = true;
        auto command = Update(state, in);
        Check(command.step == Step::TurnToLane && std::string(command.why) == "off_heading_kturn" && command.steer < 0,
              "AP-2: a blocked high-angle start uses a clear forward leg that can face the lane");
        in.now = .1;
        in.headingErrToLane = -3.1;
        in.speed = -1.5;
        in.progress = 3.0;
        command = Update(state, in);
        Check(command.steer < 0 && state.turnSign == 1, "AP-2: a heading wrap does not reverse the chosen turn");
        Check(state.tries == 1 && state.progressSum == 0, "AP-2: reversing toward the goal does not reset recovery tries");
    }

    {
        for (const double side : {-1.0, 1.0}) {
            Input in;
            in.headingErrToLane = side * 1.0471975511965976;  // 60 degrees
            State stationary;
            auto command = Update(stationary, in);
            Check(command.step == Step::BackUp && command.steer * side > 0,
                  "AP-2: stationary high-angle car recovers even when the lane-centre score is feasible");
            State moving;
            in.speed = 3.0;
            command = Update(moving, in);
            Check(command.mode == Mode::Follow,
                  "AP-2: moving high-angle car does not leave a feasible lane-centre plan");
        }
    }

    {   // nothing feasible for 2 s -> Recover
        Sim w;
        w.anyFeasible = false;
        w.RunUntil(5.0, [](const Command& c) { return c.mode == Mode::Recover; });
        std::printf("     Recover at %.1f s (%s)\n", w.firstRecoverAt, w.firstRecoverWhy);
        Check(w.firstRecoverAt >= 1.9 && w.firstRecoverAt <= 2.2 && std::string(w.firstRecoverWhy) == "nothing_feasible_2s",
              "nothing feasible: Recover after 2 s (nothing_feasible_2s)");
        Sim x;
        x.anyFeasible = false; x.reverseFeasible = false;
        x.RunUntil(5.0, [](const Command& c) { return c.mode == Mode::Stand; });
        Check(x.last.mode == Mode::Stand && std::string(x.last.why) == "nothing_safe" && x.firstRecoverAt < 0.0,
              "nothing feasible and no reverse: Stand (nothing_safe), no routine");
    }

    {   // flight stretch: Avoid, never the ground routine
        Sim w;
        w.flight = true; w.wallOn = true; w.wall = 5.0;
        w.RunFor(20.0);
        Check(w.last.mode == Mode::Avoid && std::string(w.last.why) == "flight_stretch" && w.firstRecoverAt < 0.0,
              "flight stretch: Avoid, no Recover while held");
    }

    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
