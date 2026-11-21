#include "spottest.h"
#include <cstdio>
#include <cstring>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace spottest;

// Updates every 1/60 s with one physics step each; `moves`: the car answers an active ask by 3 mm a step (refused asks do not move it).
struct Sim {
    State s; Input in; double t = 100; std::uint64_t steps = 0; bool physics = true, moves = true; int pulses = 0; Event last;
    bool refuseForward = false, refuseAllButBack = false; Ask lastAsk;
    Ask Tick() {
        t += 1.0 / 60; if (physics) ++steps;
        in.now = t; in.steps = steps; in.eligible = true;
        in.refused = lastAsk.active && ((refuseForward && lastAsk.dir == Dir::Forward) || (refuseAllButBack && lastAsk.dir != Dir::Back));
        Event ev; const Ask a = Step(s, in, ev); lastAsk = a;
        const bool runs = a.active && !((refuseForward && a.dir == Dir::Forward) || (refuseAllButBack && a.dir != Dir::Back));
        if (runs && moves && physics) in.x += a.reverse ? -.003 : .003;
        if (*ev.what) { last = ev; if (!std::strcmp(ev.what, "pulse")) ++pulses; }
        return a;
    }
    bool Settle() { bool any = false; for (int i = 0; i < rules::settleSteps; ++i) any = any || Tick().active; return any; }
};

int main() {
    {   Sim m; Check(!m.Settle(), "spawn check: waits settleSteps physics steps after the car's first hidden steps");
        const Ask a = m.Tick(); Check(a.active && !std::strcmp(m.s.trigger, "spawn_check"), "spawn check: then starts, while hidden");
        for (int i = 0; i < 10; ++i) m.Tick();
        Check(m.s.result == Result::Passed && std::fabs(m.in.x) < .02, "healthy car answers, moved under 2 cm (imperceptible)");
        bool again = false; for (int i = 0; i < 400; ++i) again = again || m.Tick().active;
        Check(!again, "one spawn check per car: never again in play"); }
    {   Sim m; m.in.covered = false; bool any = false; for (int i = 0; i < 200; ++i) any = any || m.Tick().active;
        Check(!any, "not hidden: no spawn check (nothing may be seen)"); }
    {   Sim m; m.in.speed = 2.0; bool any = false; for (int i = 0; i < 100; ++i) any = any || m.Tick().active;
        Check(!any, "still moving after release: no check until at rest"); }
    {   Sim m; m.physics = false; bool any = false; for (int i = 0; i < 200; ++i) any = any || m.Tick().active;
        Check(!any && m.s.result == Result::None, "no physics steps (behind the loading screen): never started, never a fail"); }
    {   Sim m; m.moves = false; m.Settle(); for (int i = 0; i < 40; ++i) m.Tick();
        Check(m.s.result == Result::Suspect && m.pulses == 3, "dead car: four directions, one failed test is only suspect (a body may be asleep)");
        for (int i = 0; i < 60; ++i) m.Tick(); Check(m.s.result == Result::Suspect, "no retest before the wake gap");
        for (int i = 0; i < 200; ++i) m.Tick(); Check(m.s.result == Result::Failed && Exported(m.s) == 3, "second failure after the wake gap (same check): failed");
        m.in.speed = 1.0; m.Tick(); Check(m.s.result == Result::Passed, "a failed car seen moving is healthy again"); }
    {   Sim m; m.refuseAllButBack = true; m.Settle(); for (int i = 0; i < 20; ++i) m.Tick();
        Check(m.s.result == Result::Passed && m.in.x < 0, "held car: forward and sides refused by the hold, the reverse pulse runs and answers"); }
    {   Sim m; m.refuseAllButBack = true; m.moves = false; m.Settle(); for (int i = 0; i < 30; ++i) m.Tick();
        Check(m.s.result == Result::Suspect, "held faulty car: its reverse pulse alone gives no answer: suspect (cannot hide behind the hold)"); }
    {   Sim m; m.refuseForward = true; m.Settle(); for (int i = 0; i < 20; ++i) m.Tick();
        Check(m.s.result == Result::Passed && !std::strcmp(m.last.why, "answered"), "a refused direction is skipped; the next one answers"); }
    {   Sim m; m.moves = false; m.Settle(); m.Tick(); Input in = m.in; in.refused = true; Event ev;
        for (int k = 0; k < 4; ++k) { in.now += .02; in.steps += 1; Step(m.s, in, ev); }
        Check(!m.s.running && !std::strcmp(ev.why, "every_direction_refused") && m.s.fails == 0, "every direction refused (boxed in): not tested, never a fail"); }
    {   Sim m; m.moves = false; m.Settle(); m.Tick(); Check(m.s.running, "flicker case running");
        Input in = m.in; in.eligible = false; in.now = m.t + .02; in.steps = m.steps + 1; Event ev;
        Step(m.s, in, ev); Check(m.s.running && !*ev.what, "a pause flicker pauses the test, it does not end it");
        in.now += rules::ineligibleGrace + .1; in.steps += 1; Step(m.s, in, ev);
        Check(!m.s.running && !std::strcmp(ev.why, "lost_eligibility") && m.s.result == Result::None, "a long loss of eligibility ends it, not a fail"); }
    {   Sim m; m.moves = false; m.Settle(); m.Tick(); Input in = m.in; in.eligible = false; in.x += .05; in.now = m.t + .02; in.steps = m.steps + 1; Event ev;
        Step(m.s, in, ev); Check(m.s.result == Result::Passed, "it moved on the update it lost eligibility: it answered (moved is checked first)"); }
    {   Sim m; m.in.laneOffset = -8.4; m.in.laneHalfWidth = 2.75; m.Settle(); const Ask a = m.Tick();
        Check(!a.active && m.s.result == Result::Misplaced && Exported(m.s) == 5 && !std::strcmp(m.last.why, "off_its_lane_at_check"), "8.4 m off its lane at the check: misplaced, no pulses (13:58 FF100001)"); }
    {   Sim m; m.in.upZ = -.87; m.Settle(); m.Tick(); Check(m.s.result == Result::Misplaced && !std::strcmp(m.last.why, "tipped_at_check"), "on its roof at the check: misplaced"); }
    {   Sim m; m.in.covered = false; m.in.unseen = true; m.in.builtAt = 5000; m.Settle(); const Ask a = m.Tick();
        Check(a.active && !std::strcmp(m.s.trigger, "zone_check"), "Z-1: a car that got its physics body out of view: zone check");
        for (int i = 0; i < 10; ++i) m.Tick(); Check(m.s.result == Result::Passed, "Z-1: it answered: verified");
        bool again = false; for (int i = 0; i < 100; ++i) again = again || m.Tick().active; Check(!again, "Z-1: verified: no more pulses (self-reports from here)");
        m.in.builtAt = 9000; m.Tick(); Check(!m.s.checked && !std::strcmp(m.last.why, "physics_body_rebuilt"), "Z-1: body rebuilt (cell reload): the verified mark expires");
        m.Settle(); const Ask b = m.Tick(); Check(b.active && !std::strcmp(m.s.trigger, "zone_check"), "Z-1: and the car is checked again"); }
    {   Sim m; m.in.covered = false; m.in.unseen = false; m.in.builtAt = 5000; bool any = false; for (int i = 0; i < 200; ++i) any = any || m.Tick().active;
        Check(!any, "Z-1: in the camera's view: no check until it is out of view");
        m.in.unseen = true; m.Settle(); Check(m.Tick().active, "Z-1: out of view: checked"); }
    {   const Ask l = AskFor(Dir::Left), r = AskFor(Dir::Right), b = AskFor(Dir::Back);
        Check(l.lateral < 0 && r.lateral > 0 && b.reverse, "directions: back reverses, left and right ask a side"); }
    {
        MotionEvidence evidence;
        Input in;
        in.eligible = true;
        in.builtAt = 300;
        in.steps = 20;
        auto ask = AskFor(Dir::Forward);
        evidence.Observe(in, 42, 1000, 0, 1.5707963267948966, ask);
        const auto serial = ask.serial;
        Check(serial != 0, "factory evidence tags a concealed pulse");
        in.x = .02;
        evidence.Observe(in, 42, 1010, serial + 1, 1.5707963267948966, ask);
        Check(!evidence.proof.completed, "a different plan pulse cannot authorize this sample");
        evidence.Observe(in, 42, 1020, serial, 1.5707963267948966, ask);
        in.x = .03;
        evidence.Observe(in, 42, 1030, serial, 1.5707963267948966, ask);
        Check(!evidence.proof.completed, "motion without a new physics step is not evidence");
        ++in.steps;
        evidence.Observe(in, 42, 1040, serial, 1.5707963267948966, ask);
        Check(evidence.proof.completed == 1040 && evidence.proof.steps == 1 &&
              evidence.proof.body == 42 && evidence.proof.started == 1000,
              "same-body motion after an accepted pulse and new step is evidence");
        in.builtAt = 400;
        evidence.Observe(in, 42, 1050, serial, 1.5707963267948966, ask);
        Check(!evidence.proof.completed && ask.serial != serial, "body rebuild discards the old proof and pulse");
        const auto rebuiltSerial = ask.serial;
        evidence.Observe(in, 42, 1060, rebuiltSerial, 1.5707963267948966, ask);
        ++in.steps;
        in.x += .02;
        in.refused = true;
        evidence.Observe(in, 42, 1070, rebuiltSerial, 1.5707963267948966, ask);
        Check(!evidence.proof.completed, "a refused pulse cannot produce factory evidence");
        in.refused = false;
        in.eligible = false;
        ask = AskFor(Dir::Forward);
        evidence.Observe(in, 42, 1080, rebuiltSerial, 1.5707963267948966, ask);
        Check(!evidence.proof.completed && !ask.serial, "loss of eligibility ends the observation interval");
        in.eligible = true;
        in.covered = false;
        evidence.Observe(in, 42, 1090, rebuiltSerial, 1.5707963267948966, ask);
        Check(!ask.serial, "visible proof cannot start");
        in.unseen = true;
        ask = AskFor(Dir::Back);
        evidence.Observe(in, 42, 1100, 0, 1.5707963267948966, ask);
        evidence.Observe(in, 42, 1110, ask.serial, 1.5707963267948966, ask);
        ++in.steps;
        in.x += .01;
        evidence.Observe(in, 42, 1120, ask.serial, 1.5707963267948966, ask);
        Check(!evidence.proof.completed, "motion opposite the pulse direction does not qualify");
        ++in.steps;
        in.x -= .03;
        evidence.Observe(in, 42, 1130, ask.serial, 1.5707963267948966, ask);
        Check(evidence.proof.completed == 1130 && evidence.proof.direction == Dir::Back,
              "a reverse pulse can prove reverse motion");
    }
    {
        FactoryTest test;
        MotionEvidence evidence;
        Input in;
        in.eligible = true;
        in.builtAt = 10;
        in.steps = 10;
        const auto step = [&](std::uint64_t at, std::uint64_t plan = 0,
                              bool ok = false, bool refused = false) {
            return test.Step(9, in, 14, at, plan, ok, refused, 0.0, evidence);
        };
        const auto ask = step(1000);
        Check(ask.factory && ask.reverse && ask.serial != 0, "factory asks Plan for a held-lane reverse pulse");
        in.steps += 20;
        Check(step(1300).serial == ask.serial && !evidence.proof.completed,
              "pre-accept physics cannot exhaust the bounded pulse");
        Check(step(1320, ask.serial + 1, false, true).serial == ask.serial,
              "another snapshot's refusal does not cancel a factory pulse");
        Check(step(1330, ask.serial, true).active, "factory continues a current accepted pulse");
        in.y -= .02;
        Check(step(1340, ask.serial, true).active && !evidence.proof.completed,
              "factory still waits for a new step despite displacement");
        ++in.steps;
        Check(!step(1350, ask.serial, true).active && evidence.proof.completed == 1350,
              "fresh commanded motion completes and retires the pulse");
        Check(!step(1500).active, "proved factory stock remains held");
        ++in.builtAt;
        const auto rebuilt = step(1600);
        Check(rebuilt.active && rebuilt.serial != ask.serial && !evidence.proof.completed,
              "body rebuild requires a new factory pulse");
        Check(!step(1700, rebuilt.serial, false, true).active && !step(1800).active,
              "a current refusal ends a factory attempt and paces its retry");
        in.unseen = false;
        in.covered = false;
        Check(!step(5000).active, "an exposed bay asks for no motion");
        in.unseen = true;
        Check(step(5100).active, "a concealed bay can retry");
        Check(!step(5100 + rules::factoryPlanWaitMs).active && !evidence.proof.completed,
              "a worker that never accepts cannot hold an attempt indefinitely");
    }
    {   // Lab stock in flight (stockfloat.h): the same FactoryTest, answered by the hover's own command instead of the wheeled planner. The test
        // never needs wheels or a floor: eligibility is the caller's (float held, seated, concealed); the pulse is the reverse ask; "accepted" is
        // "the hover applied last frame's pulse", and the answer is displacement against the nose.
        FactoryTest test;
        MotionEvidence evidence;
        Input in;
        in.eligible = true; in.covered = true;
        in.builtAt = 10; in.steps = 100; in.speed = 0;
        const double heading = 1.5707963267948966;   // facing +x
        std::uint64_t issuedSerial = 0; bool applied = false;
        const auto frame = [&](std::uint64_t at) {
            const auto ask = test.Step(21, in, 33, at, applied ? issuedSerial : 0, applied, false, heading, evidence);
            applied = ask.active; if (ask.active) issuedSerial = ask.serial;   // the hover applies what is asked this frame; the test sees it next frame
            return ask;
        };
        const auto first = frame(1000);
        Check(first.active && first.reverse && first.factory && first.serial, "flight: the first frame asks for the reverse pulse");
        ++in.steps;
        Check(frame(1016).active && !evidence.proof.completed, "flight: the pulse is applied; no proof before the car moves");
        ++in.steps; in.x -= .002;
        Check(frame(1032).active && !evidence.proof.completed, "flight: 2 mm backwards is below the answer threshold");
        ++in.steps; in.x -= .004;
        Check(!frame(1048).active && evidence.proof.completed == 1048 && evidence.proof.direction == Dir::Back && evidence.proof.moved >= rules::answer,
              "flight: backwards motion along the nose after a completed step proves it and retires the pulse");
        Check(!frame(1500).active && evidence.proof.completed == 1048, "flight: the proof stays after the pulse");
    }
    {   // A drifting hover (forward, or sideways) is not the commanded answer.
        FactoryTest test; MotionEvidence evidence; Input in;
        in.eligible = true; in.covered = true; in.builtAt = 10; in.steps = 100;
        const double heading = 0.0;   // facing +y
        std::uint64_t serial = 0; bool applied = false;
        const auto frame = [&](std::uint64_t at) {
            const auto ask = test.Step(22, in, 34, at, applied ? serial : 0, applied, false, heading, evidence);
            applied = ask.active; if (ask.active) serial = ask.serial; return ask; };
        frame(1000); ++in.steps; frame(1016);
        ++in.steps; in.y += .05;
        frame(1032);
        Check(!evidence.proof.completed, "flight: drifting forward is not a reverse answer");
        ++in.steps; in.x += .05;
        frame(1048);
        Check(!evidence.proof.completed, "flight: sideways drift is not a reverse answer");
        // The pulse is bounded: after factoryPulseMs without an answer it ends and paces its retry.
        const auto expired = frame(1048 + rules::factoryPulseMs + 20);
        Check(!expired.active && !evidence.proof.completed, "flight: an unanswered pulse is bounded in time");
        Check(!frame(1048 + rules::factoryPulseMs + 100).active, "flight: the retry is paced (no pulse train)");
    }
    {   // No float, no eligibility: nothing is asked and no proof can start.
        FactoryTest test; MotionEvidence evidence; Input in;
        in.eligible = false; in.covered = true; in.builtAt = 10; in.steps = 100;
        Check(!test.Step(23, in, 35, 1000, 0, false, false, 0.0, evidence).active && !evidence.serial,
              "flight: a float that is not held asks for nothing");
    }
    std::printf("spottest_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
