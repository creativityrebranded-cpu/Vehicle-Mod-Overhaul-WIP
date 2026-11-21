// D15 (one-authority step 5): the hover follower. Two kinds of test:
// (1) the follower alone on committed paths (line with an offset, arc, climb, strafe with a held nose, landing, holding position);
// (2) closed loop with the energy Plan (barrier both, flight-only, facing), the car moved by a hover that is NOT the Plan's own model
//     (slower yaw, weaker climb and acceleration), as the game's actuators will not match the model either.
#include "energyplan.h"
#include "hoverfollow.h"
#include "planscene.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kPi = 3.14159265358979323846;
using hoverfollow::Wrap;

// The "game" hover: yaws its nose toward the asked nose (the travel bearing unless the nose is held), moves along the nose, or along the
// travel when split (strafe channel); its height hold moves z toward the asked height. Deliberately weaker than the Plan's model.
static bicycle::Spec GameSpec(bicycle::Spec k) { k.hoverYaw = 1.2; k.hoverAccel = 3.0; k.climbRate = 4.0; k.descendRate = 2.5; return k; }
static bicycle::State Actuate(bicycle::State a, const hoverfollow::Out& o, const bicycle::Spec& game, double floorZ, double dt) {
    if (a.mode == 0 && o.land) { a = bicycle::Step(a, { 0, o.brake ? 0.0 : o.speed }, game, dt); return a; }
    const double noseTarget = o.split || o.facingUsed || o.hold ? o.nose : o.bearing;
    const double input = std::clamp(Wrap(noseTarget - a.heading) * 2.0, -1.0, 1.0);
    const double strafe = o.split ? Wrap(o.bearing - a.heading) : 0.0;
    if (o.land && a.z <= floorZ + .3) { a.mode = 0; a.z = floorZ; return a; }
    return bicycle::HoverStep(a, input, o.brake && o.speed < .1 ? 0.0 : o.speed, strafe, o.land ? floorZ : o.height, game, dt);
}
static double DistanceTo(const hoverfollow::Path& p, double x, double y) {
    double best = 1e18;
    for (int i = 0; i + 1 < p.count; ++i) {
        const auto& a = p.p[i]; const auto& b = p.p[i + 1]; const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy;
        const double u = l2 < 1e-9 ? 0 : std::clamp(((x - a.x) * dx + (y - a.y) * dy) / l2, 0.0, 1.0);
        best = std::min(best, std::hypot(a.x + u * dx - x, a.y + u * dy - y));
    }
    return best;
}
// A plan rollout through the Plan's own hover model: segments of (input, speed, seconds, strafe), height target `zAt(x, y)`.
template <class Z> static hoverfollow::Path Rollout(bicycle::State s, const bicycle::Spec& k, std::initializer_list<std::array<double, 4>> segs, Z zAt) {
    bicycle::State traj[energyplan::kSteps + 1]; traj[0] = s; int n = 1;
    for (const auto& g : segs) for (double t = 0; t < g[2] - 1e-9 && n <= energyplan::kSteps; t += energyplan::kDt) { s = bicycle::HoverStep(s, g[0], g[1], g[3], zAt(s.x, s.y), k, energyplan::kDt); traj[n++] = s; }
    return hoverfollow::FromTraj(traj, n, energyplan::kDt);
}

int main() {
    bicycle::Spec k; k.car = carmodel::Fallback(2.3); k.halfWidth = 1.0; k.halfLength = 2.3;
    const bicycle::Spec game = GameSpec(k);
    auto flat = [](double, double) { return 2.5; };
    auto follow = [&](const hoverfollow::Path& p, bicycle::State a, double seconds, auto&& each) {
        for (double t = 0; t < seconds; t += .05) {
            hoverfollow::Car c{ a.x, a.y, a.z, a.heading, std::fabs(a.speed), 0 };
            const auto o = hoverfollow::Follow(p, c, t);  // the plan's age: this test follows one plan from its start
            a = Actuate(a, o, game, 0.0, .05);
            if (std::getenv("FOLLOW_TRACE") && int(std::lround(t * 20)) % 5 == 0) std::printf("     t=%4.2f pos=(%6.2f,%6.2f) hdg=%5.0f v=%5.2f d=%.2f | %s bearing=%.0f nose=%.0f v=%.2f split=%d\n", t, a.x, a.y, a.heading * 180 / kPi, a.speed, DistanceTo(p, a.x, a.y), o.why, o.bearing * 180 / kPi, o.nose * 180 / kPi, o.speed, int(o.split));
            each(t + .05, a, o);
        }
        return a;
    };
    {   // 1. Straight at 10 m/s, the car 1.5 m right of the plan: it comes onto the line without overshooting.
        bicycle::State s; s.mode = 1; s.z = 2.5; s.speed = 10;
        const auto p = Rollout(s, k, { { 0, 10, 6, 0 } }, flat);
        bicycle::State a = s; a.x = 1.5; double late = 0, worst = 0;
        follow(p, a, 5.0, [&](double t, const bicycle::State& b, const hoverfollow::Out&) { const double d = DistanceTo(p, b.x, b.y); worst = std::max(worst, d); if (t > 3) late = std::max(late, d); });
        std::printf("   offset 1.5 m: worst %.2f m, after 3 s %.2f m\n", worst, late);
        Check(late < .3 && worst <= 1.55, "D15: a sideways offset is brought back onto the plan, no overshoot");
    }
    {   // 2. An arc (the plan turns at 0.6 input, 8 m/s): the slower-yawing hover stays on it.
        bicycle::State s; s.mode = 1; s.z = 2.5; s.speed = 8;
        const auto p = Rollout(s, k, { { .6, 8, 6, 0 } }, flat);
        double worst = 0; follow(p, s, 5.5, [&](double, const bicycle::State& b, const hoverfollow::Out&) { worst = std::max(worst, DistanceTo(p, b.x, b.y)); });
        std::printf("   arc: worst %.2f m off the plan\n", worst);
        Check(worst < .6, "D15: follows a hover arc within 0.6 m");
    }
    {   // 3. A climb: the plan rises 4 m over a block from y = 20 to 30; the weaker height hold is up before it gets there.
        bicycle::State s; s.mode = 1; s.z = 2.5; s.speed = 8;
        auto block = [](double, double y) { return y > 13 && y < 33 ? 6.5 : 2.5; };
        const auto p = Rollout(s, k, { { 0, 8, 6, 0 } }, block);
        double lowOver = 1e9; follow(p, s, 5.5, [&](double, const bicycle::State& b, const hoverfollow::Out&) { if (b.y > 20 && b.y < 30) lowOver = std::min(lowOver, b.z); });
        std::printf("   climb: lowest over the block %.2f m (plan 6.5)\n", lowOver);
        Check(lowOver > 6.2, "D15: climbs before what the plan climbs over");
    }
    {   // 4. Strafe: the plan travels north at 6 m/s while its nose turns east and holds (D7's turn-then-hold shape).
        bicycle::State s; s.mode = 1; s.z = 2.5; s.speed = 6;
        const auto p = Rollout(s, k, { { 1, 6, 1, -.8 }, { 0, 6, 5, -1.6 } }, flat);
        double worst = 0, noseErr = 0; bool split = false;
        const auto a = follow(p, s, 5.0, [&](double t, const bicycle::State& b, const hoverfollow::Out& o) { worst = std::max(worst, DistanceTo(p, b.x, b.y)); if (t > 2) { noseErr = std::max(noseErr, std::fabs(Wrap(b.heading - p.p[p.count - 1].heading))); split |= o.split; } });
        std::printf("   strafe: worst %.2f m, nose error after 2 s %.0f deg, end heading %.0f\n", worst, noseErr * 180 / kPi, a.heading * 180 / kPi);
        Check(split && worst < .8 && noseErr < 15 * kPi / 180, "D15: travels the plan with the nose held (strafe channel)");
    }
    {   // 5. Landing: the plan hovers 2 s at 5 m/s, then comes down and drives on. The follower brings it down near where the plan lands.
        bicycle::State s; s.mode = 1; s.z = 2.5; s.speed = 5;
        bicycle::State traj[energyplan::kSteps + 1]; traj[0] = s; int n = 1; double landY = 1e9;
        for (; n <= energyplan::kSteps; ++n) {
            if (n * energyplan::kDt <= 2.0 + 1e-9) s = bicycle::HoverStep(s, 0, 5, 0, 2.5, k, energyplan::kDt);
            else if (s.mode == 1 && s.z > .3) s = bicycle::HoverStep(s, 0, 5, 0, 0, k, energyplan::kDt);
            else { if (s.mode == 1) landY = s.y; s.mode = 0; s.z = 0; s = bicycle::Step(s, { 0, 5 }, k, energyplan::kDt); }
            traj[n] = s;
        }
        const auto p = hoverfollow::FromTraj(traj, n, energyplan::kDt);
        double firstLand = 1e9, carLandY = 1e9;
        follow(p, traj[0], 5.5, [&](double t, const bicycle::State& b, const hoverfollow::Out& o) { if (o.land && firstLand > 1e8) firstLand = t; if (b.mode == 0 && carLandY > 1e8) carLandY = b.y; });
        std::printf("   landing: plan lands at y=%.1f, car at y=%.1f, land asked from %.2f s\n", landY, carLandY, firstLand);
        Check(firstLand > 1.4 && carLandY < 1e8 && std::fabs(carLandY - landY) < 3.0, "D15: comes down where the plan lands (within 3 m), not before it asks");
    }
    {   // 6. Holding position while the nose turns a quarter: it stays put and turns.
        bicycle::State s; s.mode = 1; s.z = 2.5;
        const auto p = Rollout(s, k, { { 1, 0, 1, 0 }, { 0, 0, 5, 0 } }, flat);
        bicycle::State a = s; a.x = .4; a.y = .2;
        const auto e = follow(p, a, 4.0, [&](double, const bicycle::State&, const hoverfollow::Out&) {});
        const double off = std::hypot(e.x - p.p[p.count - 1].x, e.y - p.p[p.count - 1].y), ne = std::fabs(Wrap(e.heading - p.p[p.count - 1].heading));
        std::printf("   hold: %.2f m from the plan's point, nose error %.0f deg\n", off, ne * 180 / kPi);
        Check(off < .5 && ne < 15 * kPi / 180, "D15: holds position and turns the nose in place");
    }

    // (2) Closed loop: the Plan every 0.1 s, the follower and the game hover in the air; on wheels the Plan's first command (AF-1 stands in).
    auto Straight = [](planscene::Scene& sc, double length) { for (double y = -20; y <= length; y += 1) sc.route.push_back({ 0, y }); sc.floor = [](double, double, double& z) { z = 0; return true; }; };
    auto Wall = [](planscene::Scene& sc, double x0, double y0, double x1, double y1, std::initializer_list<double> heights) {
        const double len = std::hypot(x1 - x0, y1 - y0); const int n = std::max(1, int(len / .5));
        for (int i = 0; i <= n; ++i) for (double h : heights) sc.solids.push_back({ x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, h, h, .15 }); };
    auto road = [&](planscene::Scene& sc, double length, int travel) { Straight(sc, length); Wall(sc, -6, -20, -6, length, { .6 }); Wall(sc, 6, -20, 6, length, { .6 }); sc.travel = travel; sc.hoverPossible = true; };
    struct Loop { bool reached = false; double minClear = 1e9, hover = 0, seconds = 0, maxTrack = 0; bicycle::State end; };
    const bool trace = std::getenv("HOVER_TRACE") != nullptr;
    auto loop = [&](planscene::Scene& sc, bicycle::State s, double want, double limit, double goalY) {
        sc.Index(); energyplan::Memory mem; Loop r;
        for (double t = 0; t < limit; t += .1) {
            double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half);
            const auto p = energyplan::Plan(s, k, want, sc, mem, t, {}, hint, nullptr);
            double fz = 0; sc.Floor(s.x, s.y, fz);
            if (p.firstMode == 1 || s.mode == 1) {  // the hover follower drives
                const auto path = hoverfollow::FromTraj(p.traj, energyplan::kSteps + 1, energyplan::kDt);
                for (int i = 0; i < 2; ++i) {
                    hoverfollow::Car c{ s.x, s.y, s.z, s.heading, std::fabs(s.speed), 0 };
                    const auto o = hoverfollow::Follow(path, c, i * .05);
                    sc.Floor(s.x, s.y, fz); s = Actuate(s, o, game, fz, .05);
                    r.maxTrack = std::max(r.maxTrack, DistanceTo(path, s.x, s.y));
                    if (trace && i == 0 && int(t * 10) % 5 == 0) std::printf("   t=%4.1f pos=(%5.2f,%6.2f) z=%.2f hdg=%5.0f v=%5.2f mode=%d %s bearing=%.0f nose=%.0f h=%.2f land=%d split=%d\n", t, s.x, s.y, s.z, s.heading * 180 / kPi, s.speed, s.mode, o.why, o.bearing * 180 / kPi, o.nose * 180 / kPi, o.height, int(o.land), int(o.split));
                }
            } else for (int i = 0; i < 2; ++i) { s = bicycle::Step(s, p.first, game, .05); s.z = fz; }
            if (s.mode == 1) r.hover += .1;
            sc.Floor(s.x, s.y, fz);
            r.minClear = std::min(r.minClear, sc.Clearance(s.x, s.y, s.heading, s.mode == 1 ? s.z - k.climb + .2 : fz, k, s.mode == 0));
            r.seconds = t; if (s.y >= goalY) { r.reached = true; break; }
        }
        r.end = s; return r;
    };
    {   planscene::Scene sc; road(sc, 140, 0); Wall(sc, -6, 20, 6, 20, { .30, .64, .98 });
        bicycle::State s; s.speed = 5;
        const auto r = loop(sc, s, 8, 30, 100);
        std::printf("   loop barrier, both: reached=%d in %.1f s, hover %.1f s, closest %.2f, worst tracking %.2f m, end mode %d\n", r.reached, r.seconds, r.hover, r.minClear, r.maxTrack, r.end.mode);
        Check(r.reached && r.minClear >= 0 && r.hover > 0 && r.end.mode == 0, "D15 loop: over a barrier and down again through the follower, no contact");
    }
    {   planscene::Scene sc; road(sc, 120, 2);
        bicycle::State s;
        const auto r = loop(sc, s, 10, 20, 60);
        std::printf("   loop flight-only: reached=%d in %.1f s, hover %.1f s, worst tracking %.2f m, closest %.2f\n", r.reached, r.seconds, r.hover, r.maxTrack, r.minClear);
        Check(r.reached && r.minClear >= 0 && r.hover >= r.seconds - .3, "D15 loop: a flight-only stretch flown through the follower");
    }
    {   planscene::Scene sc; road(sc, 120, 2); sc.facing = 1.5708;
        bicycle::State s;
        const auto r = loop(sc, s, 6, 20, 40);
        std::printf("   loop facing east, travelling north: reached=%d in %.1f s, end heading %.0f, worst tracking %.2f m\n", r.reached, r.seconds, r.end.heading * 180 / kPi, r.maxTrack);
        Check(r.reached && std::cos(r.end.heading - 1.5708) > .85 && r.minClear >= 0, "D15 loop: keeps the facing while travelling, through the follower");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
