// Relocation consumers (src/relocationrebase.h, planworld.h Progress/Still clocks, spottest.h). Offline logic only: never proof of behaviour in game.
#include "relocationrebase.h"
#include "planworld.h"
#include "spottest.h"
#include <cstdio>
#include <cmath>
#include <string>
static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace relocrebase;

static FvoRelocationStatusV1 g_status; static int g_has = 0;
static int Source(unsigned v, FvoRelocationStatusV1* out) { (void)v; if (!g_has) return 0; *out = g_status; return 1; }
static Event Applied(std::uint32_t v, std::uint64_t serial, std::uint64_t epoch = 1) { Event e; e.vehicle = v; e.serial = serial; e.epoch = epoch; return e; }

int main() {
    // Take: once per serial
    { Seen s; const Event e = Applied(7, 3);
      Check(Take(s, e, 7), "Take: the first applied serial re-bases");
      Check(!Take(s, e, 7), "Take: the same serial again does nothing (once per serial)");
      Check(Take(s, Applied(7, 4), 7), "Take: a newer serial re-bases again"); }
    // stale serial and stale epoch
    { Seen s; Take(s, Applied(7, 5), 7);
      Check(!Take(s, Applied(7, 4), 7) && Judge(s, Applied(7, 4), 7) == Verdict::Duplicate, "Take: an older serial is ignored (stale)");
      Check(Judge(s, Applied(7, 9, 0), 7) == Verdict::StaleEpoch, "Judge: a receipt of an earlier epoch is stale, whatever its serial");
      Check(Take(s, Applied(7, 1, 2), 7), "Take: a newer epoch re-bases even when its serial restarted lower"); }
    // other vehicle
    { Seen s;
      Check(!Take(s, Applied(8, 3), 7) && Judge(s, Applied(8, 3), 7) == Verdict::OtherVehicle, "Take: another vehicle's receipt is ignored");
      Check(s.serial == 0, "Take: an ignored receipt leaves the record untouched");
      Check(Judge(s, Event{}, 7) == Verdict::None && Judge(s, Applied(7, 1), 0) == Verdict::OtherVehicle, "Judge: no receipt = none; vehicle 0 never matches"); }
    // a record following a different vehicle starts clean
    { Seen s; Take(s, Applied(7, 9), 7);
      Check(Take(s, Applied(8, 2), 8) && s.vehicle == 8, "Take: the record moves to another vehicle and applies its first serial");
      Check(Take(s, Applied(7, 9), 7), "Take: back on the first vehicle its serial is new to this record"); }
    // FromStatus: only applied counts
    { FvoRelocationStatusV1 st; st.result = 1; st.serial = 4; st.epoch = 2; st.to[0] = 1; Event e;
      Check(FromStatus(7, st, e) && e.serial == 4 && e.epoch == 2 && e.to[0] == 1, "FromStatus: an applied status becomes an event");
      st.result = 0; Check(!FromStatus(7, st, e), "FromStatus: no result is not a move");
      st.result = 2; Check(!FromStatus(7, st, e), "FromStatus: refused is not a move");
      st.result = 3; Check(!FromStatus(7, st, e), "FromStatus: failed is not a move");
      st.result = 4; Check(!FromStatus(7, st, e), "FromStatus: restored is not a move to re-base on");
      st.result = 1; st.serial = 0; Check(!FromStatus(7, st, e), "FromStatus: serial 0 is not a receipt"); }
    // Board
    { Board b; Event e;
      g_has = 0; Check(!b.Refresh(7, &Source) && !b.Latest(7, e), "Board: a vehicle with no status stores nothing");
      g_has = 1; g_status = {}; g_status.result = 2; g_status.serial = 1; Check(!b.Refresh(7, &Source), "Board: a refused receipt stores nothing");
      g_status.result = 1; g_status.serial = 2; g_status.epoch = 1; Check(b.Refresh(7, &Source) && b.Latest(7, e) && e.serial == 2, "Board: an applied receipt is stored");
      Check(!b.Refresh(7, &Source), "Board: the same receipt is not stored twice");
      g_status.serial = 1; Check(!b.Refresh(7, &Source) && b.Latest(7, e) && e.serial == 2, "Board: an older serial never replaces a newer one");
      g_status.serial = 3; Check(b.Refresh(7, &Source) && b.Latest(7, e) && e.serial == 3, "Board: a newer serial replaces it");
      g_status.epoch = 0; g_status.serial = 9; Check(!b.Refresh(7, &Source), "Board: an older epoch never replaces a newer one");
      Check(!b.Latest(8, e), "Board: another vehicle has none");
      for (std::uint32_t v = 100; v < 100 + 70; ++v) { g_status.epoch = 1; g_status.serial = v; b.Refresh(v, &Source); }
      Check(b.Latest(169, e) && e.serial == 169, "Board: when full the oldest slot is reused, the newest kept");
      Check(!b.Refresh(0, &Source) && !b.Refresh(7, nullptr), "Board: no vehicle or no source is refused");
      b.Clear(); Check(!b.Latest(169, e), "Board: Clear forgets"); }
    // One receipt reaches several consumers, each applies it once
    { Board b; g_has = 1; g_status = {}; g_status.result = 1; g_status.serial = 5; g_status.epoch = 1; b.Refresh(7, &Source);
      Event e; b.Latest(7, e); Seen plan, world, keep;
      Check(Take(plan, e, 7) && Take(world, e, 7) && Take(keep, e, 7), "consumers: Plan, World and Keep-in-sight each apply the one receipt");
      Check(!Take(plan, e, 7) && !Take(world, e, 7) && !Take(keep, e, 7), "consumers: none applies it twice"); }
    // ProgressClock / StillClock (Route and stuck detector)
    { planworld::ProgressClock pc; for (int k = 0; k < 12; ++k) pc.Step(k * 1.1, 0, 0);
      Check(pc.Net(12.0, 10.0) >= 0 && pc.Net(12.0, 10.0) < 0.01, "ProgressClock: a car standing still has no net progress (stuck sense)");
      Check(!pc.Rebase(13.0, 6, 0, Event{}, 7), "ProgressClock: no receipt does not re-base");
      Check(pc.Rebase(13.0, 6, 0, Applied(7, 1), 7), "ProgressClock: an applied receipt re-bases");
      Check(pc.Net(14.0, 10.0) < 0, "ProgressClock: history restarts, so no verdict yet (not read as progress, not as stuck)");
      Check(!pc.Rebase(13.5, 12, 0, Applied(7, 1), 7), "ProgressClock: the same serial does not re-base again");
      Check(!pc.Rebase(13.5, 12, 0, Applied(7, 0), 7) && !pc.Rebase(13.5, 12, 0, Applied(8, 9), 7), "ProgressClock: no serial or another vehicle does not re-base");
      for (int k = 0; k < 12; ++k) pc.Step(14.2 + k * 1.1, 6, 0);
      Check(pc.Net(27.0, 10.0) >= 0 && pc.Net(27.0, 10.0) < 0.01, "ProgressClock: afterwards stillness is measured from the new station only");
      planworld::StillClock sc; sc.Step(1, 0); sc.Step(5, 0); sc.Rebase(); Check(sc.Step(6, 0) == 0, "StillClock: Rebase forgets the old standing time"); }
    // Spot test: the jump is not motion
    { spottest::State s; spottest::Input in; in.eligible = true; in.covered = true; in.builtAt = 5; in.steps = 100; in.x = 0; in.y = 0; in.speed = 0; spottest::Event ev;
      s.checked = true; s.haveFirst = true; s.builtSeen = 5; s.result = spottest::Result::Suspect; s.lastTestAt = -100; s.lastFailAt = -100; in.now = 10; in.requested = true;
      auto ask = spottest::Step(s, in, ev); Check(s.running && ask.active, "spot test: a requested retest starts a pulse");
      in.now = 10.05; in.steps = 101; in.x = 6.0; in.relocation = 1;   // the car was moved 6 m across the receipt
      ask = spottest::Step(s, in, ev);
      Check(s.running && s.result == spottest::Result::Suspect && std::string(ev.what) != "passed", "spot test: a 6 m jump across an applied receipt does not pass the pulse");
      Check(s.relocSeen == 1 && s.x0 == 6.0, "spot test: the baseline moved to the destination");
      in.now = 10.1; in.steps = 102; in.x = 6.01; ask = spottest::Step(s, in, ev);
      Check(std::string(ev.what) == "passed", "spot test: real motion after the receipt still passes it"); }
    { spottest::State s; spottest::Input in; in.eligible = true; in.covered = true; in.builtAt = 5; in.steps = 100; in.now = 10; spottest::Event ev;
      s.checked = true; s.haveFirst = true; s.builtSeen = 5; s.result = spottest::Result::Suspect; s.lastTestAt = -100; s.lastFailAt = -100; in.requested = true;
      in.relocation = 3; spottest::Step(s, in, ev); in.now = 10.05; in.steps = 101; in.x = 6.0; in.relocation = 2;   // stale serial
      spottest::Step(s, in, ev);
      Check(std::string(ev.what) == "passed", "spot test: a stale (older) serial does not hide motion"); }
    // Factory motion evidence
    { spottest::MotionEvidence me; spottest::Input in; in.eligible = true; in.covered = true; in.builtAt = 9; in.steps = 10; in.x = 0; in.y = 0;
      spottest::Ask ask = spottest::AskFor(spottest::Dir::Back); ask.factory = true;
      me.Observe(in, 77, 1000, 0, 0.0, ask);
      const std::uint64_t serial = me.serial; ask = spottest::AskFor(spottest::Dir::Back);
      in.steps = 11; me.Observe(in, 77, 1010, serial, 0.0, ask);   // accepted here, baseline (0,0)
      Check(me.accepted, "motion evidence: the accepted pulse sets its baseline");
      in.steps = 12; in.y = -6.0; in.relocation = 1;   // a 6 m jump backward across the receipt
      me.Observe(in, 77, 1020, serial, 0.0, ask);
      Check(!me.proof.completed && me.relocSeen == 1 && me.y == -6.0, "motion evidence: a 6 m jump across an applied receipt is not proof of motion");
      in.steps = 13; in.y = -6.01; me.Observe(in, 77, 1030, serial, 0.0, ask);
      Check(me.proof.completed, "motion evidence: motion after the receipt is still proved"); }
    std::printf("relocationrebase_test: %d checks, %d failed\n%s\n", checks, failed, failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
