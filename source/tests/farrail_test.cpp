#include "farrail.h"
#include "relocation.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace farrail;
static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }
static bool Is(const char* got, const char* want) { return std::string(got) == want; }
static bool Near(double a, double b, double e = 1e-6) { return std::fabs(a - b) <= e; }

int main() {
    // The hop size is the proven doer hop, and the distances are beyond the loaded-area wall.
    Check(Near(kHopM, relocation::kMaxHopM), "the hop is the relocation doer's hop");
    Check(kStartDistanceM > relocation::kMaxMoveM, "the first test distance is past the 250 m move limit and the wall");
    Check(Near(TargetDistance(0), 300) && Near(TargetDistance(1), 350) && Near(TargetDistance(6), 600) && Near(TargetDistance(50), 600), "target distance 300 +50 per run, capped at 600");
    Check(kProfileCap * kProfileSpacingM >= kPathCapM, "the profile covers the path cap");

    // Probe points and source choice
    Check(ProbeCount(300) == 7 && Near(ProbeDistance(6), 300), "probes every 50 m through the target");
    {
        Coverage c[kSources]{};
        c[0].all = 7; c[0].beyond = 3; c[1].all = 6; c[1].beyond = 0;
        Check(PickSource(c, 7, 3) == Source::Cached, "the source that answered everywhere wins");
        c[0].all = 5; c[0].beyond = 1; c[1].all = 4; c[1].beyond = 3; c[2].all = 2; c[2].beyond = 2;
        Check(PickSource(c, 7, 3) == Source::Cellbake, "no full source: most answers beyond the wall wins");
        c[1].beyond = 1; c[0].beyond = 1; c[2].beyond = 1;
        Check(PickSource(c, 7, 3) == Source::Cached, "ties go to the earlier source in the fixed order");
        Coverage none[kSources]{};
        Check(PickSource(none, 7, 3) == Source::None, "nothing answered: no source");
        Check(Is(SourceName(Source::Cached), "cached_heightfield") && Is(SourceName(Source::None), "none"), "source names");
    }

    // Ride height
    Check(Near(RideFromStart(105.2, 104.0, true), 1.2), "ride = chassis height minus ground");
    Check(Near(RideFromStart(105.2, 104.0, false), 1.5), "unknown ground: fallback ride");
    Check(Near(RideFromStart(90.0, 104.0, true), 1.5), "ground above the chassis (a bad reading): fallback");
    Check(Near(RideFromStart(140.0, 104.0, true), 1.5), "implausible ride: fallback");
    Check(Near(RideFromStart(std::nan(""), 104.0, true), 1.5), "non finite: fallback");

    // Profile
    {
        Profile p;
        double z = 0;
        Check(!p.Get(10, z) && p.KnownCount() == 0, "an empty profile knows nothing");
        p.Set(0, 100); p.Set(1, 108); p.Set(2, 100);
        Check(p.Get(4, z) && Near(z, 104), "linear between samples");
        Check(p.Get(8, z) && Near(z, 108), "on a sample");
        Check(p.Get(20, z) && Near(z, 100), "the last known sample is used as the nearest one");
        Check(!p.Get(40, z), "past the known samples is unknown");
        Check(!p.Get(-1, z) && !p.Get(1e9, z), "out of range is unknown");
        p.Set(5, std::nan("")); Check(!p.Known(5), "a non finite height is not stored");
        Check(p.KnownCount() == 3, "known count");
        p.Clear(); Check(p.KnownCount() == 0, "clear forgets");
        Check(Profile::IndexOf(0) == 0 && Profile::IndexOf(7.99) == 0 && Profile::IndexOf(8) == 1 && Profile::IndexOf(-0.1) == -1, "index by 8 m");
    }

    // Rail step: forward at the set speed, height from terrain + ride, never falls, never clips under
    {
        RailParams p; p.x0 = 10; p.y0 = 20; p.dirX = 0; p.dirY = 1; p.speed = 15; p.ride = 1.5;
        RailState st; st.s = 300; st.z = 106.5;
        auto o = Step(st, p, 1.0 / 60, true, 105.0);
        Check(Near(o.x, 10) && Near(o.y, 20 + st.s) && Near(st.s, 300.25), "moves along the heading at the speed");
        Check(Near(o.vy, 15) && Near(o.vx, 0), "velocity matches the move");
        Check(o.terrainKnown && Near(o.target, 106.5) && Near(o.z, 106.5) && Near(o.vz, 0), "on the target height it does not move vertically");
        // hill: the terrain rises 3 m at once: limited climb
        auto o2 = Step(st, p, 1.0 / 60, true, 108.0);
        Check(o2.z > 106.5 && o2.z <= 106.5 + 8.0 / 60 + 1e-9 && Near(o2.vz, 8.0, 1e-6), "a step in the terrain is climbed at the vertical limit");
        // unknown terrain: height held, counted
        RailState hold; hold.s = 400; hold.z = 120;
        auto o3 = Step(hold, p, 1.0 / 60, false, 0);
        Check(!o3.terrainKnown && Near(o3.z, 120) && hold.holds == 1 && Near(o3.target, 120), "no terrain answer: the height is held and counted");
        // a non finite terrain answer is unknown
        auto o4 = Step(hold, p, 1.0 / 60, true, std::nan(""));
        Check(!o4.terrainKnown && hold.holds == 2, "a NaN terrain answer is treated as unknown");
        // bad dt: nothing moves
        RailState same; same.s = 5; same.z = 7;
        Step(same, p, 0.0, true, 1); Step(same, p, -1.0, true, 1); Step(same, p, std::nan(""), true, 1);
        Check(Near(same.s, 5) && Near(same.z, 7) && same.steps == 0, "a bad step interval changes nothing");
        // 10 s run: it reaches 150 m further and stays at ride height over a slope
        RailState run; run.s = 300; run.z = 106.5; double maxErr = 0;
        for (int i = 0; i < 600; ++i) {
            const double nextS = run.s + 15.0 / 60;
            const double ground = 105.0 + 0.1 * (nextS - 300.0);    // 10 % slope
            auto r = Step(run, p, 1.0 / 60, true, ground);
            maxErr = std::max(maxErr, std::fabs(r.z - (ground + p.ride)));
        }
        Check(Near(run.s, 450, 1e-6), "10 s at 15 m/s is 150 m");
        Check(maxErr < 0.5, "over a 10 percent slope the height error stays small");
        // cap
        RailState cap; cap.s = kPathCapM - 0.1; cap.z = 0;
        auto oc = Step(cap, p, 1.0, true, 0);
        Check(oc.capped && Near(cap.s, kPathCapM) && Near(oc.vy, 0.1), "the rail never runs past the path cap");
    }

    // Hops
    {
        P3 a{0, 0, 0}, b{0, 300, 0.3}; bool last = false;
        auto h = NextHop(a, b, last);
        Check(!last && Near(Dist(a, h), kHopM, 1e-9), "a hop is at most 20 m");
        P3 near{0, 295, 0.3}; h = NextHop(near, b, last);
        Check(last && Near(h.y, 300), "the last hop reaches the target");
        Check(HopsFor(300) == 15 && HopsFor(0) == 0 && HopsFor(20) == 1 && HopsFor(20.01) == 2 && HopsFor(std::nan("")) == 0, "hop counts");
        Check(kMaxHopsOut(600) > HopsFor(600), "the hop budget exceeds the hops needed");
        // a car that drifted sideways: the next hop is recomputed from where it is
        P3 drifted{3, 100, 0}; h = NextHop(drifted, b, last);
        Check(std::fabs(h.x) < 3 && Near(Dist(drifted, h), kHopM, 1e-9), "hops correct a sideways drift instead of accumulating it");
    }
    {
        const char* why = "";
        Check(JudgeHop(false, true, 5, 0, 3, true, true, false, why) == Hop::Failed && Is(why, "body_changed"), "a changed body fails");
        Check(JudgeHop(true, false, 5, 0, 3, true, true, false, why) == Hop::Failed && Is(why, "physics_not_live"), "forward: physics not live fails at once");
        Check(JudgeHop(true, false, 5, 0, 3, false, false, false, why) == Hop::Wait && Is(why, "physics_not_live_waiting"), "return: physics not live waits");
        Check(JudgeHop(true, false, 5, 0, 3, false, false, true, why) == Hop::Failed, "return: physics not live fails when the window ran out");
        Check(JudgeHop(true, true, 0, 0, 3, true, true, false, why) == Hop::Wait && Is(why, "no_fresh_step"), "no completed step: wait");
        Check(JudgeHop(true, true, 2, 9, 3, true, true, false, why) == Hop::Failed && Is(why, "hop_not_at_target"), "left behind fails");
        Check(JudgeHop(true, true, 2, 1, 3, false, true, false, why) == Hop::Failed && Is(why, "seat_link_lost"), "forward: lost seat link fails");
        Check(JudgeHop(true, true, 2, 1, 3, false, false, false, why) == Hop::Arrived, "return: an unseated chassis still arrives");
        Check(JudgeHop(true, true, 2, 1, 3, true, true, false, why) == Hop::Arrived && Is(why, "hop_arrived"), "a good hop arrives");
    }

    // Tally and verdicts
    {
        Tally t; Sample s; s.z = 100; s.terrainKnown = true;
        for (int i = 0; i < 60; ++i) { s.errM = 0.1 * (i % 3); s.speed = 15; t.Add(s); }
        Check(t.samples == 60 && t.errSamples == 60 && Near(t.maxAbsErr, 0.2) && Near(t.MeanAbsErr(), 0.1, 1e-9), "error tally");
        Check(Is(Stage2Verdict(t), "rail_held") && t.moving == 60, "a held rail with no faults");
        Check(Is(Stage1Verdict(t), "stayed_loaded_and_still"), "stage 1: nothing wrong");
        Tally f; Sample g; g.z = 100; for (int i = 0; i < 10; ++i) { g.z = 100 - 10.0 * i; f.Add(g); }
        Check(Near(f.Fall(), 90) && Is(Stage1Verdict(f), "fell"), "a fall is reported");
        Tally m; Sample lost; lost.modelLoaded = false; m.Add(lost); Check(Is(Stage1Verdict(m), "model_lost") && Is(Stage2Verdict(m), "model_lost"), "a lost model is reported first");
        Tally d; Sample ns; ns.seated = false; d.Add(ns); Check(Is(Stage1Verdict(d), "driver_not_seated"), "driver not seated");
        Tally j; Sample jp; jp.jumpM = 80; j.Add(jp); Check(j.jumps == 1 && Near(j.maxJump, 80) && Is(Stage1Verdict(j), "moved_by_something_else"), "a jump is reported");
        Tally u; Sample un; un.bodyReadable = false; u.Add(un); Check(Is(Stage1Verdict(u), "body_unreadable"), "unreadable body");
        Tally nt; Sample nn; nn.terrainKnown = false; nt.Add(nn); Check(Is(Stage2Verdict(nt), "no_terrain_answer") && nt.terrainUnknown == 1, "no terrain answer");
        Tally big; Sample bg; bg.terrainKnown = true; bg.errM = 3.0; big.Add(bg); Check(Is(Stage2Verdict(big), "height_error_over_2m"), "a big height error");
        Tally e; Check(Is(Stage1Verdict(e), "no_samples") && Is(Stage2Verdict(e), "no_samples"), "no samples");
        Check(Near(UncommandedJump({0, 0, 0}, {30, 40, 0}, 10), 40) && Near(UncommandedJump({0, 0, 0}, {3, 4, 0}, 10), 0), "uncommanded jump is the move beyond what was commanded");
    }
    {
        Check(Is(ReturnVerdict(20, 20, 20, 5, 0, 0.5), "clean_and_driving"), "return verdict: clean and driving");
        Check(Is(ReturnVerdict(20, 20, 20, 0, 0, 0.5), "clean_but_not_driving"), "return verdict: clean, autopilot not resumed");
        Check(Is(ReturnVerdict(20, 19, 20, 5, 0, 0), "physics_not_clean"), "return verdict: physics");
        Check(Is(ReturnVerdict(20, 20, 18, 5, 0, 0), "driver_not_seated"), "return verdict: seat");
        Check(Is(ReturnVerdict(20, 20, 20, 5, 3, 0), "census_says_broken"), "return verdict: census");
        Check(Is(ReturnVerdict(20, 20, 20, 5, 0, 9), "fell"), "return verdict: fall");
        Check(Is(ReturnVerdict(0, 0, 0, 0, 0, 0), "no_samples"), "return verdict: none");
    }

    std::printf("%s: %d checks, %d failed\n", g_fail ? "FAIL" : "ALL PASS", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
