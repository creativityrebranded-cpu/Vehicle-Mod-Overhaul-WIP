// AP-2 (2026-11-22, user: "when autopilot is at 90 deg to path they really struggle to recover"): closed-loop proof of the get-unstuck
// routine (recovery.h) through the real planner (energyplan::Plan) for a traffic car standing 60-120 deg off its lane, both sides, on a road
// with known edge walls (route edge bands), with and without a parked car behind it. Plan every 0.1 s, move the car through the same car
// model, plan again. Pass = the car faces the lane within 10 deg and drives on along it, never touching a wall.
// Offline check only, not in-game proof.
#include "energyplan.h"
#include "planscene.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kPi = 3.14159265358979323846;

struct Case { const char* car; double lock, wheelbase; double errDeg; double roadHalf; bool parkedBehind; double lateral; };
struct Out { bool faced = false, drove = false; double facedAt = -1, minClear = 1e9, station0 = 0, station = 0; int gearFlips = 0, steps = 0, legs = 0; std::string trail, lastWhy; };

static char Letter(const char* step) {
    if (!std::strcmp(step, "back_up")) return 'B';
    if (!std::strcmp(step, "turn_to_lane")) return 'T';
    if (!std::strcmp(step, "retry")) return 'R';
    if (!std::strcmp(step, "hover")) return 'H';
    return '.';
}

static Out Run(const Case& c, bool trace = false) {
    planscene::Scene sc;
    sc.continuousRecovery = true;  // a traffic (managed) car
    sc.floor = [](double, double, double& z) { z = 0; return true; };
    for (double y = -40; y <= 80; y += 1) sc.route.push_back({ 0, y });
    sc.legal = { { -c.roadHalf, c.roadHalf } };  // known edge bands: walls the planner must not hit
    sc.BuildKnownBandSolids();
    if (c.parkedBehind) {  // a parked car across the lane 4.5 m behind the car's tail side (where a back-up would go)
        for (double y = -7.0; y <= -3.0; y += .5) for (double x = -1.0; x <= 1.0; x += .5) sc.solids.push_back({ x + c.lateral - 3.5 * (c.errDeg > 0 ? 1 : -1), y, 0, 1.6, .25 });
    }
    sc.Index();
    bicycle::Spec k; k.car.lock = c.lock; k.car.wheelbase = c.wheelbase;
    bicycle::State s; s.x = c.lateral; s.y = 0; s.heading = c.errDeg * kPi / 180.0;
    energyplan::Memory mem; Out o;
    o.minClear = sc.Clearance(s.x, s.y, s.heading, 0, k, true);
    double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half); o.station0 = hint;
    double lastDir = 0; char lastLetter = 0;
    for (double t = 0; t < 40.0; t += .1) {
        const auto p = energyplan::Plan(s, k, 8.0, sc, mem, t, {}, hint);
        const char L = Letter(p.step);
        if (L != '.' && L != lastLetter) { o.trail += L; ++o.steps; }
        lastLetter = L;
        if (p.layerWhy && *p.layerWhy) o.lastWhy = p.layerWhy;
        if (trace && int(std::lround(t * 10)) % 5 == 0)
            std::printf("     t=%4.1f pos=(%5.2f,%6.2f) hdg=%6.1f v=%5.2f in=%5.2f %s/%s why=%s plan=%s legs=%d/%d/%d tries=%d cycles=%d side=%d\n",
                t, s.x, s.y, s.heading * 180 / kPi, s.speed, s.input, p.layer, p.step, p.layerWhy, p.why,
                p.legF, p.legFaces, p.legR, p.recTries, p.recCycles, p.turnSign);
        for (int i = 0; i < 2; ++i) {
            s.mode = 0;
            s = bicycle::Step(s, p.first, k, .05);
            o.minClear = std::min(o.minClear, sc.Clearance(s.x, s.y, s.heading, 0, k, true));
        }
        const double dir = s.speed > .2 ? 1 : s.speed < -.2 ? -1 : 0;
        if (dir != 0 && lastDir != 0 && dir != lastDir) ++o.gearFlips;
        if (dir != 0) lastDir = dir;
        sc.Route(s.x, s.y, hint, lat, half, hint); o.station = hint;
        const double err = std::remainder(s.heading, 2 * kPi);
        if (!o.faced && std::fabs(err) < 10 * kPi / 180) { o.faced = true; o.facedAt = t; }
        if (o.faced && o.station - o.station0 >= 10.0 && s.speed > 1.0) { o.drove = true; break; }
    }
    o.legs = o.gearFlips;
    return o;
}

int main(int argc, char** argv) {
    // kturn_test <car 0-2> <road half> <parked 0/1> <err deg>: trace that one case
    const bool trace = argc > 4;
    struct CarRow { const char* name; double lock, wb; };
    const CarRow cars[] = { { "FE00900A lock 0.79", .7854, 3.344 }, { "FE005005 lock 0.52", .5236, 3.633 }, { "fallback lock 0.55", .55, 3.22 } };
    const double errs[] = { 60, 75, 90, 105, 120, -60, -75, -90, -105, -120 };
    int total = 0, ok = 0; double worst = 0;
    if (trace) {
        const auto& car = cars[std::atoi(argv[1])];
        const Out o = Run(Case{ car.name, car.lock, car.wb, std::atof(argv[4]), std::atof(argv[2]), std::atoi(argv[3]) != 0, 0.0 }, true);
        std::printf("   faced %.1f s, drove %d, min clear %.2f, trail %s\n", o.facedAt, int(o.drove), o.minClear, o.trail.c_str());
        return 0;
    }
    for (const auto& car : cars) for (double road : { 5.5, 4.5 }) for (bool parked : { false, true }) for (double e : errs) {
        const Case c{ car.name, car.lock, car.wb, e, road, parked, 0.0 };
        const Out o = Run(c);
        ++total; const bool pass = o.drove && o.minClear >= 0;
        if (pass) { ++ok; worst = std::max(worst, o.facedAt); }
        std::printf("   %-19s road +-%.1f %s err %5.0f: %s faced %5.1f s, legs %2d, min clear %5.2f, gained %5.1f m, trail %s, last %s\n", car.name, road,
                    parked ? "parked behind" : "open behind  ", e, pass ? "ok  " : "FAIL", o.facedAt, o.legs, o.minClear, o.station - o.station0,
                    o.trail.c_str(), o.lastWhy.c_str());
    }
    std::printf("   %d of %d recovered, slowest facing %.1f s\n", ok, total, worst);
    Check(ok == total, "AP-2: every car 60-120 deg off its lane (both sides, two road widths, open or parked car behind) faces the lane and drives on");
    Check(worst <= 25.0, "AP-2: each faces the lane within 25 s");
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
