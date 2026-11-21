// Route edge no-go bands and medians as known no-go space (2026-11-19). Offline checks on synthetic scenes; not in-game proof.
//  - planinput::KnownLegal: lanes -> lateral intervals (median = gap, outer end hard only with a band)
//  - Scene::NoGoDepth: how far a body is into known no-go space
//  - corridor::Measure: known edges keep the line's margin, certain and without a sensor
//  - Scene::BuildKnownBandSolids: known no-go becomes wall solids; the corridor, the planner's collision rule and the cost-to-go field then treat them as any sensed wall
//  - energyplan::Score: a rollout into a band or median is refused (reason enters_a_known_band) unless the car already touches it and the path does not go deeper
//  - roadfield::LanesAt keeps an older caller's smaller struct intact (see roadfield_test)
#include "energyplan.h"
#include "planlattice.h"
#include "planscene.h"
#include "planinput.h"
#include "corridor.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kInf = std::numeric_limits<double>::infinity();

// Known no-go -> wall solids, as planinput::Scene does before Index().
static void Walls(planscene::Scene& sc) { sc.solids.erase(std::remove_if(sc.solids.begin(), sc.solids.end(), [](const planscene::Solid& x) { return x.band; }), sc.solids.end()); sc.BuildKnownBandSolids(); sc.Index(); }
static void Straight(planscene::Scene& sc, double length) { for (double y = -20; y <= length; y += 1) sc.route.push_back({ 0, y }); sc.floor = [](double, double, double& z) { z = 0; return true; }; }

// A swerve: steer `input` for `seconds`, back the other way for as long, then straight on (a lane change's shape).
static double ScoreOf(const planscene::Scene& sc, const bicycle::Spec& k, double startX, double input, energyplan::Terms& t, double seconds = 1.0) {
    bicycle::State car; car.x = startX; car.speed = 4;
    energyplan::Candidate c; c.count = 3; c.seg[0] = { input, 4, seconds }; c.seg[1] = { -input, 4, seconds }; c.seg[2] = { 0, 4, energyplan::kSteps * energyplan::kDt };
    double clear = 0;
    const double v = energyplan::Score(c, car, k, 4, sc, {}, nullptr, 0, t, nullptr, clear);
    std::printf("   score(start %.1f, input %.2f, %.1f s) = %g  %s\n", startX, input, seconds, v, t.reject);
    return v;
}

int main() {
    bicycle::Spec k; k.car = carmodel::Fallback(2.3); k.halfWidth = 1.0; k.halfLength = 2.3;

    {   // KnownLegal
        planbranch::Lanes l; l.count = 2; l.left[0] = -3.0; l.right[0] = -.5; l.left[1] = .5; l.right[1] = 3.0;  // a 1 m median between two lanes
        auto none = planinput::KnownLegal(l);
        Check(none.size() == 2 && !std::isfinite(none[0].first) && none[0].second == -.5 && none[1].first == .5 && !std::isfinite(none[1].second),
              "known: a median is a gap; with no band the outer ends are open");
        l.bandLow = .8; l.bandHigh = 1.2; auto both = planinput::KnownLegal(l);
        Check(both.size() == 2 && both[0].first == -3.0 && both[1].second == 3.0, "known: a band makes the outer edge on that side a hard end");
        planbranch::Lanes touching; touching.count = 2; touching.left[0] = -3; touching.right[0] = 0; touching.left[1] = 0; touching.right[1] = 3;
        Check(planinput::KnownLegal(touching).empty(), "known: touching lanes and no band: nothing known (older roads unchanged)");
        touching.bandHigh = .5; auto one = planinput::KnownLegal(touching);
        Check(one.size() == 1 && !std::isfinite(one[0].first) && one[0].second == 3, "known: one band, one hard end");
        planbranch::Lanes empty; Check(planinput::KnownLegal(empty).empty(), "known: no lanes, nothing known");
    }
    {   // NoGoDepth
        planscene::Scene sc; sc.legal = { { -1.75, 1.2 } };  // road of 2.95 m: a 2 m wide body has 0.475 m of room each way about its middle
        Check(sc.NoGoDepth(-.27, 0, k) < 1e-9, "depth: centred in the road: clear");
        Check(sc.NoGoDepth(.6, 0, k) > .2, "depth: past the room: into no-go");
        Check(sc.NoGoDepth(-.27 - .2, 0, k) < 1e-9, "depth: within the room: clear");
        planscene::Scene wide; wide.legal = { { -.5, .5 } };  // a road narrower than the body
        Check(wide.NoGoDepth(0, 0, k) < 1e-9 && wide.NoGoDepth(.4, 0, k) > .3, "depth: a road narrower than the body is judged from its middle");
        planscene::Scene one; one.legal = { { -kInf, 1.5 } };
        Check(one.NoGoDepth(.4, 0, k) < 1e-9 && one.NoGoDepth(.8, 0, k) > .25 && one.NoGoDepth(-9, 0, k) < 1e-9, "depth: one hard end: only that side counts");
        Check(one.NoGoDepth(.4, 1.0, k) > .5, "depth: a turned body reaches further across (its length)");
        planscene::Scene med; med.legal = { { -kInf, -.5 }, { .5, kInf } };
        Check(med.NoGoDepth(0, 0, k) > 1.0 && med.NoGoDepth(-2.0, 0, k) < 1e-9 && med.NoGoDepth(-1.0, 0, k) > .4, "depth: standing on a median / straddling it is no-go");
        planscene::Scene none; Check(none.NoGoDepth(5, 0, k) == 0, "depth: nothing known: 0");
    }
    {   // corridor: known edges keep the margin; nothing known = nothing changes
        planscene::Scene sc; Straight(sc, 60);
        auto base = corridor::Measure(sc, 0, 0, 0, 0, k);
        Check(base.target == 0 && base.known == 0, "corridor: no known edges: line stays (as before)");
        sc.legal = { { -kInf, 2.2 } }; Walls(sc);  // right edge 2.2 m from the line: needs 1.5, has room
        auto room = corridor::Measure(sc, 0, 0, 0, 0, k);
        Check(room.target == 0 && room.known == 2, "corridor: a known edge with room: no shift");
        sc.legal = { { -kInf, 1.2 } }; Walls(sc);  // 1.2 m: the body (1.0) plus margin (.5) does not fit: the line moves away
        auto tight = corridor::Measure(sc, 0, 0, 0, 0, k);
        Check(tight.target < -.25 && tight.target > -.35 && tight.known == 2, "corridor: a known band edge too near: the line moves away by the least amount");
        sc.legal = { { -1.75, 1.2 } }; Walls(sc);
        auto both = corridor::Measure(sc, 0, 0, 0, 0, k);
        Check(std::fabs(both.target - .5 * (-1.75 + 1.2)) < 1e-9 && both.known == 3, "corridor: edges on both sides too near: midway");
        sc.legal = { { 2.0, 5.0 } }; Walls(sc);
        Check(corridor::Measure(sc, 0, 0, 0, 0, k).known == 2, "corridor: a line outside the legal road meets the road's near wall as any wall (no special case)");
        sc.legal = { { -kInf, 1.2 } }; Walls(sc);
        corridor::ShiftRoute(sc, -.3);
        Check(std::fabs(sc.legal[0].second - 1.5) < 1e-9 && std::fabs(sc.route[0].x + .3) < 1e-9, "corridor: shifting the line re-measures the fixed known edge from it (the wall solids stay put in the world)");
        auto again = corridor::Measure(sc, 0, -.3, 0, 0, k);
        Check(std::fabs(again.target) < 1e-9, "corridor: after the shift the margin is kept: no further shift (no oscillation)");
        planscene::Scene s2; Straight(s2, 60); s2.legal = { { -kInf, 2.2 } }; Walls(s2);
        for (double y = -2; y <= 6; y += .5) s2.solids.push_back({ -1.6, y, 0, 1.2, .1 }); s2.Index();  // a measured wall on the left
        auto mix = corridor::Measure(s2, 20, 0, 0, 0, k);  // station 20 = y 0 (the route starts at y -20)
        std::printf("   mix: solids %d known %d right %.3f left %.3f\n", mix.solids, mix.known, mix.right, mix.left);
        Check(mix.solids > 0 && mix.known == 2 && mix.right <= 2.2 + 1e-9 && mix.left > -2.0, "corridor: known and measured edges count together");
    }
    {   // Score: a rollout into a band is refused; leaving one is not
        planscene::Scene sc; Straight(sc, 120); sc.legal = { { -3.5, 1.4 } }; Walls(sc);  // road 4.9 m wide, edge 1.4 m right of the line
        energyplan::Terms t;
        const double straight = ScoreOf(sc, k, 0, 0, t);
        Check(std::isfinite(straight) && t.reject[0] == 0, "refusal: straight along the line (body to 1.0 < 1.4): allowed");
        const double right = ScoreOf(sc, k, 0, .5, t);
        Check(!std::isfinite(right) && std::strcmp(t.reject, "enters_a_known_band") == 0, "refusal: swerving right into the band: refused, reason enters_a_known_band");
        const double left = ScoreOf(sc, k, 0, -.3, t);
        Check(std::isfinite(left), "refusal: swerving left: allowed");
        planscene::Scene open; Straight(open, 120); open.Index();
        const double control = ScoreOf(open, k, 0, .5, t);
        Check(std::isfinite(control), "refusal: control, no known no-go: the same swerve is not refused (older roads unchanged)");
        // already in: lateral .8 puts the body to 1.8 (depth .4)
        const double stay = ScoreOf(sc, k, .8, 0, t);
        Check(std::isfinite(stay), "inside: holding the line (not deeper): allowed");
        const double deeper = ScoreOf(sc, k, .8, .5, t);
        Check(!std::isfinite(deeper) && std::strcmp(t.reject, "enters_a_known_band") == 0, "inside: a path that goes deeper: refused");
        const double out = ScoreOf(sc, k, .8, -.3, t);
        Check(std::isfinite(out), "inside: a path that moves out: allowed");
        planscene::Scene lane; Straight(lane, 120); lane.legal = { { -kInf, 1.0 }, { 3.0, kInf } }; Walls(lane);  // the line's lane to 1.0, a median 1..3
        const double intoMedian = ScoreOf(lane, k, -.2, .5, t);
        Check(!std::isfinite(intoMedian) && std::strcmp(t.reject, "enters_a_known_band") == 0, "median: a path that puts the body into the median: refused");
    }
    {   // closed loop: a car standing in a band drives out of it and never deeper
        planscene::Scene sc; Straight(sc, 160); sc.legal = { { -1.5, 1.5 } }; Walls(sc);  // road 3 m, body 2 m: it must hold within 0.5 m of the middle
        bicycle::State s; s.x = .9; s.speed = 3;
        energyplan::Memory mem; planlattice::Field togo; double builtAt = -1e9, builtSt = -1e9, hint = 0, lat = 0, half = 0, worst = 0;
        const double startDepth = sc.NoGoDepth(.9, 0, k);
        sc.Route(s.x, s.y, hint, lat, half);
        double maxDepth = 0; int notOk = 0;
        for (double t = 0; t < 12; t += .1) {
            if (t - builtAt >= 2.0 || hint - builtSt >= 8.0) {
                const double goal = hint + 30; double gx = s.x, gy = s.y;
                for (const auto& q : sc.route) if (q.station >= goal) { gx = q.x; gy = q.y; break; }
                togo = planlattice::Build(sc, k, .5 * (s.x + gx), .5 * (s.y + gy), goal, hint); builtAt = t; builtSt = hint;
            }
            const auto p = energyplan::Plan(s, k, 6, sc, mem, t, {}, hint, &togo);
            if (!p.ok) ++notOk;
            for (int i = 0; i < 2; ++i) { s.mode = 0; s = bicycle::Step(s, p.first, k, .05); }
            sc.Route(s.x, s.y, hint, lat, half, hint);
            const double d = sc.NoGoDepth(lat, s.heading, k); if (t > 3) maxDepth = std::max(maxDepth, d); worst = std::max(worst, sc.NoGoDepth(lat, 0, k));  // the centre's depth: turning out swings the rear for a moment
        }
        std::printf("   in a band: start depth %.2f m, worst %.2f m, worst after 3 s %.2f m, end lateral %.2f m, not ok %d\n", startDepth, worst, maxDepth, lat, notOk);
        Check(worst <= startDepth + .06 && maxDepth < .1, "closed loop: a car standing in a band never goes deeper and is out of it within 3 s");
    }
    std::printf("%s\n", failed ? "SOME FAILED" : "ALL PASSED");
    return failed ? 1 : 0;
}
