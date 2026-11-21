#pragma once
// Imperceptible spot test: part of the spawn health check (change cards ST-A..D, ST-A2, ST-A3, T-1..T-3b; user 2026-11-07: "spot tests
// only happen when there's a health check that is vehicle spawn", "health check is for spawn, it takes priority", "it can even be
// reversed"). Pure; tests: tests/spottest_test.cpp.
// When: once per newly spawned or arrived car, at its first settled moment while it is hidden (the loading screen's fade-in, or the
// Start/Swap panel's hidden running world), after settleSteps physics steps. Never as an in-play monitor.
// Z-1 verified zone (user 2026-11-07: "ok do it"): the zone around the player is checked under the loading screen; beyond it a car is
// checked when the game builds its physics body (its cell attaches at the loaded-area edge, ahead of a moving player), at its first
// settled moment out of the camera's view. A rebuilt body (cell reload, new body) expires the verified mark: checked again.
// Between checks Health relies on the cars' self-reports. Held cars are checked too:
// the check takes priority over the hold, while the hold still limits the directions (Speed refuses forward and side pulses of a held
// car; the reverse pulse runs). A suspect car's second test (after the wake gap) belongs to the same check.
// Behind the loading screen the game steps no vehicle physics, so the check is counted in physics steps; no steps = not started.
// pulseSteps at the rail's 6 m/s^2 move a car about 1.3 cm: nobody sees it.
#include <cmath>
#include <cstdint>

namespace spottest {
namespace rules {
constexpr int pulseSteps = 4;            // physics steps per direction
constexpr double pulseSpeed = 1.0;       // m/s asked during a pulse (the actuator ramps up to it: ~1.3 cm in 4 steps)
constexpr double pulseLateral = .4;      // m: the side pulses' lateral ask
constexpr double pulseMax = .5;          // s: a pulse that saw some steps but not all within this is measured anyway
constexpr double answer = .004;          // m moved in one pulse: the car answered
constexpr double retestSuspect = 3.0;    // s before a suspect car is tested again (same check)
constexpr double wakeGap = 3.0;          // s between the two failures that make a fail (a sleeping body wakes within 2.3 s)
constexpr int failsToFail = 2;
constexpr double stepWait = 1.0;         // s without a physics step during a test: not tested
constexpr double movedOnItsOwn = .5;     // m/s: a failed car seen moving this fast is healthy again
// ST-A2 (14:00 run): start only once settled after release (the drop and wake are over), while hidden.
constexpr int settleSteps = 20;          // physics steps from the car's first hidden steps before its check
constexpr double settledSpeed = .3;      // m/s: at rest
constexpr double ineligibleGrace = 1.0;  // s: a shorter loss of eligibility (a pause flicker) pauses the test, it does not end it
// ST-A3 (13:58 run, FF100001 thrown and flipped 8.4 m off its lane): at the check a car off its lane or on its side is misplaced.
constexpr double offLaneMargin = 2.0;    // m beyond the lane's half width
constexpr double tippedUp = .5;          // body up-vector z below this (about 60 degrees) is on its side or roof
// Z-2 (user 2026-11-07: the arc of the edge band the player sees must be verified too): a 1.3 cm pulse at this distance is about
// 0.007 degrees, far below anything visible, so a car in view this far away may be checked. Removal keeps its own rule (far and out
// of sight, Retirement).
constexpr double checkInViewFrom = 100.0;  // m from the player
constexpr std::uint64_t factoryPlanWaitMs = 2000;  // includes the shared worker's gather interval
constexpr std::uint64_t factoryPulseMs = 500;      // maximum after Plan first accepts this attempt
constexpr std::uint64_t factoryRetryMs = 3000;    // give a resting chassis the ordinary wake gap
constexpr std::uint64_t factoryPulseSteps = 12;   // bounded movement after the accepted command
constexpr double factoryFloorTolerance = .5;    // measured support must be at the body base
constexpr double factorySupportNormal = .7;
constexpr int factorySupportWheels = 3;          // same native traction minimum as corner forces
constexpr std::uint64_t factorySupportMs = 250;
constexpr double factorySupportRayShift = .25;
}
// Cover facts are trafficarrival's (T-1): the caller passes `covered`.

enum class Dir : int { Forward, Back, Left, Right, Count };
inline const char* Name(Dir d) { constexpr const char* n[]{ "forward", "back", "left", "right", "-" }; return n[int(d)]; }
enum class Result : int { None, Passed, Suspect, Failed, NotTested, Misplaced };
inline const char* Name(Result r) { constexpr const char* n[]{ "none", "passed", "suspect", "failed", "not_tested", "misplaced" }; return n[int(r)]; }

struct Ask { bool active = false; Dir dir = Dir::Forward; double speed = 0; bool reverse = false; double lateral = 0; std::uint64_t serial = 0; bool factory = false; };
struct Input {
    double now = 0, x = 0, y = 0, speed = 0; std::uint64_t steps = 0;
    bool eligible = false;        // traffic on wheels, driver seated, physics stepping
    bool covered = true;          // hidden: the loading screen's fade-in or the Start/Swap panel (trafficarrival)
    bool unseen = false;          // Z-1: outside the camera's view, or (Z-2) at least checkInViewFrom away: a pulse there cannot be seen
    unsigned long long builtAt = 0;  // Z-1: when the game last built this car's physics body (vehiclediag::BuiltAt), 0 = unknown
    double laneOffset = 0, laneHalfWidth = 1e9, upZ = 1;  // ST-A3: across its lane line (m), the lane's half width, body up-vector z
    bool refused = false;         // T-2: the last pulse was not run (its branch was infeasible): skip that direction
    bool requested = false;       // CH-4: Health asked for a test (the pair is doubtful); any number of times, never more often than retestSuspect
};
struct Event { const char* what = ""; const char* why = ""; Dir dir = Dir::Count; double moved = 0; int steps = 0; };
struct State {
    Result result = Result::None; bool running = false; int dir = 0; int fails = 0;
    std::uint64_t stepsAtPulse = 0, lastSteps = 0; double pulseAt = 0, x0 = 0, y0 = 0, lastStepAt = -1;
    double lastTestAt = -1e9, lastFailAt = -1e9; const char* trigger = "";
    bool checked = false, haveFirst = false; std::uint64_t firstSteps = 0;  // the spawn check ran for this car; its first hidden steps
    unsigned long long builtSeen = 0;  // Z-1: the body build this car was last checked for
    double ineligibleSince = -1; int refusedDirs = 0;
};

// Separate evidence for factory Health. The legacy verdict also accepts
// incidental motion, which cannot establish a fresh commanded response.
struct MotionProof {
    std::uint64_t started = 0, completed = 0, builtAt = 0, steps = 0;
    std::uint32_t body = 0;
    Dir direction = Dir::Count;
    double moved = 0;
};

struct MotionEvidence {
    std::uint64_t serial = 0, nextSerial = 0;
    MotionProof proof{};
    std::uint64_t started = 0, builtAt = 0, acceptedStep = 0;
    std::uint32_t body = 0;
    Dir direction = Dir::Count;
    bool accepted = false;
    double x = 0, y = 0, heading = 0;

    void Observe(const Input& in, std::uint32_t boundBody, std::uint64_t now,
                 std::uint64_t acceptedPulse, double headingNow, Ask& ask) {
        if (proof.body != boundBody || proof.builtAt != in.builtAt) proof = {};
        const bool eligible = in.eligible && (in.covered || in.unseen) && boundBody && in.builtAt;
        if (eligible && !in.refused && serial && body == boundBody && builtAt == in.builtAt &&
            acceptedPulse == serial) {
            if (!accepted) {
                accepted = true;
                acceptedStep = in.steps;
                x = in.x;
                y = in.y;
                heading = headingNow;
            } else if (!proof.completed || proof.started != started) {
                const double along = (in.x - x) * std::sin(heading) + (in.y - y) * std::cos(heading);
                const double moved = direction == Dir::Back ? -along : along;
                if (in.steps > acceptedStep && std::isfinite(moved) && moved >= rules::answer)
                    proof = {started, now, builtAt, in.steps - acceptedStep, body, direction, moved};
            }
        } else accepted = false;
        if (!eligible || !ask.active || (ask.dir != Dir::Forward && ask.dir != Dir::Back)) {
            serial = 0;
            return;
        }
        if (!serial || body != boundBody || builtAt != in.builtAt || direction != ask.dir) {
            serial = ++nextSerial;
            started = now;
            builtAt = in.builtAt;
            body = boundBody;
            direction = ask.dir;
            accepted = false;
        }
        ask.serial = serial;
    }

    void Reset() {
        serial = 0;
        proof = {};
    }
};

inline Ask AskFor(Dir d) {
    Ask a; a.active = true; a.dir = d; a.speed = rules::pulseSpeed; a.reverse = d == Dir::Back;
    a.lateral = d == Dir::Left ? -rules::pulseLateral : d == Dir::Right ? rules::pulseLateral : 0; return a;
}

inline bool FactorySupported(bool groundCurrent, double referenceZ, double originAbove, double groundZ) {
    return groundCurrent && std::isfinite(referenceZ) && std::isfinite(originAbove) &&
        std::isfinite(groundZ) && originAbove >= 0 &&
        std::fabs(referenceZ - originAbove - groundZ) <= rules::factoryFloorTolerance;
}

struct FactoryWheelContact {
    std::uint64_t at = 0, step = 0;
    std::uint32_t body = 0;
    int touching = 0;
    double z = 0, up = 0;
};

inline bool FactoryWheelSupported(const FactoryWheelContact& contact, std::uint32_t body, std::uint64_t now) {
    return body && contact.body == body && contact.step && contact.at &&
        now >= contact.at && now - contact.at <= rules::factorySupportMs &&
        contact.touching >= rules::factorySupportWheels && std::isfinite(contact.z) &&
        std::isfinite(contact.up) && contact.up >= rules::factorySupportNormal;
}

struct FactoryTest {
    std::uint64_t lease = 0, acceptedAt = 0, retryAt = 0;

    Ask Step(std::uint64_t token, const Input& in, std::uint32_t body, std::uint64_t now,
             std::uint64_t planSerial, bool planAccepted, bool planRefused, double heading,
             MotionEvidence& evidence) {
        if (lease != token) {
            *this = FactoryTest{token};
            evidence.Reset();
        }
        Input sample = in;
        const bool same = evidence.serial && planSerial == evidence.serial;
        sample.refused = same && planRefused;
        if (evidence.proof.body != body || evidence.proof.builtAt != in.builtAt) evidence.proof = {};
        const bool eligible = token && in.eligible && (in.covered || in.unseen) && body && in.builtAt;
        const bool bodyChanged = evidence.serial && (evidence.body != body || evidence.builtAt != in.builtAt);
        if (!eligible || bodyChanged) {
            evidence.serial = 0;
            acceptedAt = 0;
            return {};
        }
        if (same && planAccepted && !acceptedAt) acceptedAt = now;
        const bool expired = evidence.serial && (acceptedAt
            ? now - acceptedAt >= rules::factoryPulseMs ||
                (evidence.accepted && in.steps >= evidence.acceptedStep + rules::factoryPulseSteps)
            : now - evidence.started >= rules::factoryPlanWaitMs);
        if (sample.refused || expired) {
            evidence.serial = 0;
            acceptedAt = 0;
            retryAt = now + rules::factoryRetryMs;
            return {};
        }
        if (evidence.proof.completed || (!evidence.serial &&
            (now < retryAt || in.speed >= rules::settledSpeed))) return {};

        // A private lane is held. The same Plan scorer can admit an allowed
        // reverse pulse without crossing the forward stop line.
        auto ask = AskFor(Dir::Back);
        ask.factory = true;
        evidence.Observe(sample, body, now, same && planAccepted ? planSerial : 0, heading, ask);
        if (evidence.proof.completed) {
            evidence.serial = 0;
            return {};
        }
        return ask;
    }
};
// For the Pair Facts export: 0 none, 1 passed, 2 suspect, 3 failed, 4 running, 5 misplaced at the spawn check (off its lane or tipped).
inline int Exported(const State& s) { return s.running ? 4 : s.result == Result::Passed ? 1 : s.result == Result::Suspect ? 2 : s.result == Result::Failed ? 3 : s.result == Result::Misplaced ? 5 : 0; }

inline void Begin(State& s, const Input& in, int dir) { s.dir = dir; s.stepsAtPulse = in.steps; s.pulseAt = in.now; s.x0 = in.x; s.y0 = in.y; }
inline void Fail(State& s, const Input& in, Event& ev) {
    s.running = false; s.lastTestAt = in.now; ++s.fails; s.lastFailAt = in.now;
    s.result = s.fails >= rules::failsToFail ? Result::Failed : Result::Suspect;
    ev.what = s.result == Result::Failed ? "failed" : "suspect"; ev.why = "no_direction_answered";
}

// One update. The ask is active only while a pulse runs; ev says what happened (empty: nothing to log).
inline Ask Step(State& s, const Input& in, Event& ev) {
    ev = Event{};
    if (s.lastStepAt < 0 || in.steps != s.lastSteps) { s.lastSteps = in.steps; s.lastStepAt = in.now; }
    if (!s.running) {
        if (s.result == Result::Failed && in.speed >= rules::movedOnItsOwn) { s.result = Result::Passed; s.fails = 0; s.lastTestAt = in.now; ev.what = "passed"; ev.why = "moved_on_its_own"; return {}; }
        if (in.builtAt && in.builtAt != s.builtSeen) {     // Z-1: a (re)built physics body: this car is checked again
            const bool rebuilt = s.builtSeen != 0; s.builtSeen = in.builtAt;
            if (rebuilt && s.checked) { s.checked = false; s.haveFirst = false; ev.what = "rearmed"; ev.why = "physics_body_rebuilt"; return {}; } }
        if (!in.eligible) return {};
        const char* why = nullptr;
        if (!s.checked) {
            if (!in.covered && !in.unseen) return {};          // only while hidden: under cover, or out of the camera's view (Z-1)
            if (!s.haveFirst) { s.haveFirst = true; s.firstSteps = in.steps; }
            if (in.steps - s.firstSteps < std::uint64_t(rules::settleSteps) || in.speed >= rules::settledSpeed) return {};
            s.checked = true; why = in.covered ? "spawn_check" : "zone_check";
            const bool off = std::fabs(in.laneOffset) > in.laneHalfWidth + rules::offLaneMargin, tipped = in.upZ < rules::tippedUp;
            if (off || tipped) { s.result = Result::Misplaced; s.lastTestAt = in.now; s.trigger = why; ev.what = "misplaced"; ev.why = tipped ? "tipped_at_check" : "off_its_lane_at_check"; ev.moved = in.laneOffset; return {}; }
        }
        else if (s.result == Result::Suspect && in.now - s.lastTestAt >= rules::retestSuspect && in.now - s.lastFailAt >= rules::wakeGap) why = "retest_after_wake";
        else if (in.requested && (in.covered || in.unseen) && in.speed < rules::settledSpeed && in.now - s.lastTestAt >= rules::retestSuspect &&
                 in.now - s.lastFailAt >= rules::wakeGap) why = "health_request";  // CH-4
        if (!why) return {};
        s.running = true; s.trigger = why; s.refusedDirs = 0; s.ineligibleSince = -1; Begin(s, in, 0);
        ev.what = "start"; ev.why = why; ev.dir = Dir::Forward;
        return AskFor(Dir::Forward);
    }
    const Dir d = Dir(s.dir);
    const double moved = std::hypot(in.x - s.x0, in.y - s.y0);
    const int steps = int(in.steps - s.stepsAtPulse);
    ev.dir = d; ev.moved = moved; ev.steps = steps;
    if (moved >= rules::answer) { s.running = false; s.result = Result::Passed; s.fails = 0; s.lastTestAt = in.now; s.ineligibleSince = -1; ev.what = "passed"; ev.why = "answered"; return {}; }  // it moved: it answered
    if (!in.eligible) {  // a flicker pauses the test (no ask); only a longer loss ends it, never a fail
        if (s.ineligibleSince < 0) s.ineligibleSince = in.now;
        if (in.now - s.ineligibleSince > rules::ineligibleGrace) { s.running = false; s.lastTestAt = in.now; s.ineligibleSince = -1; ev.what = "not_tested"; ev.why = "lost_eligibility"; return {}; }
        ev = Event{}; return {}; }
    if (s.ineligibleSince >= 0) { s.ineligibleSince = -1; Begin(s, in, s.dir); }  // eligible again: this direction starts over from here
    const bool last = s.dir + 1 >= int(Dir::Count);
    if (in.refused) {  // the hold or the collision test refused this direction: skip it, never count it
        ++s.refusedDirs;
        if (!last) { Begin(s, in, s.dir + 1); ev.what = "pulse"; ev.why = "direction_refused"; return AskFor(Dir(s.dir)); }
        if (s.refusedDirs >= int(Dir::Count)) { s.running = false; s.lastTestAt = in.now; ev.what = "not_tested"; ev.why = "every_direction_refused"; return {}; }
        Fail(s, in, ev); return {};  // the directions that ran gave no answer (one is enough to decide, e.g. a held car's reverse)
    }
    if (in.now - s.lastStepAt > rules::stepWait) { s.running = false; s.lastTestAt = in.now; ev.what = "not_tested"; ev.why = "no_physics_steps"; return {}; }
    if (steps >= rules::pulseSteps || (steps > 0 && in.now - s.pulseAt >= rules::pulseMax)) {
        if (!last) { Begin(s, in, s.dir + 1); ev.what = "pulse"; ev.why = "no_answer"; return AskFor(Dir(s.dir)); }
        Fail(s, in, ev); return {};
    }
    ev = Event{};
    return AskFor(d);
}
}
