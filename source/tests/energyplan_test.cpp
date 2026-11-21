// Redesign (2026-11-12): the energy planner, closed loop on synthetic scenes built from the week's failures. Each scene is driven:
// plan, move the car 0.1 s through the same car model, plan again. Pass = the car does what a driver would, by construction.
#include "energyplan.h"
#include "planlattice.h"
#include "planscene.h"
#include "planinput.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <thread>
#include <string>
#include <vector>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kPi = 3.14159265358979323846;

struct Run { int steerFlips = 0, speedJumps = 0, switches = 0; double hoverSeconds = 0, maxAbove = -1e9, minAboveWhenHover = 1e9; int modeSwitches = 0; double minMover = 1e9, maxFront = -1e9, minSpeedAfter = 1e9, seconds = 0, minClear = 1e9, maxY = -1e9, station = 0; int gearFlips = 0, notOk = 0; bicycle::State end; bool reached = false; double maxLat = 0; int rel[6] = {}; };

static Run Drive(planscene::Scene& sc, bicycle::State s, const bicycle::Spec& k, double want, double limit, double goalStation, bool trace = false,
                 const std::function<void(planscene::Scene&, double, const bicycle::State&)>& world = {}) {
    sc.Index();
    energyplan::Memory mem; Run r; double lastDir = 0;
    double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half);
    const double routeEnd = sc.route.empty() ? 0 : sc.route.back().station;
    planlattice::Field togo; double builtAt = -1e9, builtSt = -1e9;
    for (double t = 0; t < limit; t += .1) {
        // The cost-to-go field: rebuilt every 2 s or after 8 m of route, goal 30 m on (as the game adapter will).
        if (t - builtAt >= 2.0 || hint - builtSt >= 8.0) {
            const double goal = std::min(hint + 30.0, routeEnd - 2.0); double gx = s.x, gy = s.y;
            for (const auto& q : sc.route) if (q.station >= goal) { gx = q.x; gy = q.y; break; }
            togo = planlattice::Build(sc, k, .5 * (s.x + gx), .5 * (s.y + gy), goal, hint); builtAt = t; builtSt = hint; }
        if (world) world(sc, t, s);  // a moving world: the test moves its users and stop line each update
        const auto p = energyplan::Plan(s, k, want, sc, mem, t, {}, hint, &togo);
        if (!p.ok) ++r.notOk;
        for (int q = 0; q < 6; ++q) r.rel[q] = p.rel[q];  // HC-3: switches per rule so far
        {   // O1 / O3 (route oscillation): steering that swaps sides, the speed ask jumping, the committed plan replaced (after the first 2 s)
            static thread_local double lastIn = 0, lastAsk = -1; if (t == 0) { lastIn = 0; lastAsk = -1; }
            if (t >= 2.0) { if (std::fabs(p.first.input) > .05 && std::fabs(lastIn) > .05 && (p.first.input > 0) != (lastIn > 0)) ++r.steerFlips;
                if (lastAsk >= 0 && std::fabs(p.speedAhead - lastAsk) > 2.0) ++r.speedJumps; if (!p.kept) ++r.switches; }
            if (std::fabs(p.first.input) > .05) lastIn = p.first.input; lastAsk = p.speedAhead; }
        for (int i = 0; i < 2; ++i) {  // the same models the plan rolls out (step 3: hover, landing)
            const int before = s.mode; double fz = 0; const bool hf = sc.Floor(s.x, s.y, fz);
            if (p.firstMode == 1) { int seg = 0; double st, la, ha; sc.Route(s.x, s.y, st, la, ha, hint, nullptr, nullptr, &seg); s = bicycle::HoverStep(s, p.first.input, p.first.speed, p.firstStrafe, sc.HoverTarget(s.x, s.y, fz, hf, k, seg), k, .05); }
            else if (s.mode == 1 && s.z > (hf ? fz : s.z) + .3) s = bicycle::HoverStep(s, 0, p.first.speed, 0, hf ? fz : s.z, k, .05);
            else { s.mode = 0; if (hf) s.z = fz; s = bicycle::Step(s, p.first, k, .05); }
            if (s.mode != before) ++r.modeSwitches;
        }
        {   double fz = 0; if (sc.Floor(s.x, s.y, fz)) { if (s.mode == 0) s.z = fz; r.maxAbove = std::max(r.maxAbove, s.z - fz); } if (s.mode == 1) r.hoverSeconds += .1; }
        double z = 0; sc.Floor(s.x, s.y, z);
        r.minClear = std::min(r.minClear, sc.Clearance(s.x, s.y, s.heading, s.mode == 1 ? s.z - k.climb + .2 : z, k, s.mode == 0));  // step 3: a hovering body's band (RP-1: flat)
        if (world) world(sc, t + .1, s);
        { bool c; r.minMover = std::min(r.minMover, sc.MoverClearance(s.x, s.y, s.heading, z, k, 0, 0, 0, c)); }
        r.maxFront = std::max(r.maxFront, s.y + k.halfLength);
        const double dir = s.speed > .2 ? 1 : s.speed < -.2 ? -1 : 0;
        if (dir != 0 && lastDir != 0 && dir != lastDir) ++r.gearFlips;
        if (dir != 0) lastDir = dir;
        sc.Route(s.x, s.y, hint, lat, half, hint); r.maxLat = std::max(r.maxLat, std::fabs(lat));
        r.maxY = std::max(r.maxY, s.y); r.station = hint; r.seconds = t;
        if (trace && int(t * 10) % 10 == 0) std::printf("   t=%4.1f pos=(%6.2f,%6.2f) hdg=%6.1f v=%5.2f in=%5.2f st=%6.2f lat=%5.2f %s E=%.1f\n", t, s.x, s.y, s.heading * 180 / kPi, s.speed, s.input, hint, lat, p.why, p.energy);
        if (hint >= goalStation) { r.reached = true; break; }
    }
    r.end = s; return r;
}
// O1 (2026-11-14): the game's loop as it really runs. The plan is made from the car as it was `lag` s ago (the worker's age), only every
// `every` s, and the car steers with `gain` of what the model assumes (the real car is not the model). Counts steering side swaps and speed
// ask jumps after the first 2 s, and how far the car gets off the line.
struct Wobble { int steerFlips = 0, speedJumps = 0; double maxLat = 0, station = 0; };
static Wobble DriveLagged(planscene::Scene& sc, bicycle::State s, const bicycle::Spec& k, double want, double limit, double lag, double every, double gain) {
    sc.Index(); energyplan::Memory mem; Wobble w; double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half);
    const double routeEnd = sc.route.empty() ? 0 : sc.route.back().station;
    planlattice::Field togo; double builtAt = -1e9; std::vector<std::pair<double, bicycle::State>> past; energyplan::Result p; double nextPlan = 0, lastIn = 0, lastAsk = -1;
    for (double t = 0; t < limit; t += .05) {
        past.push_back({ t, s }); while (past.size() > 2 && past[1].first <= t - lag) past.erase(past.begin());
        if (t >= nextPlan) {
            const bicycle::State seen = past.front().second; double sh, sl, sf; sc.Route(seen.x, seen.y, sh, sl, sf, hint);
            if (t - builtAt >= 2.0) { const double goal = std::min(sh + 30.0, routeEnd - 2.0); double gx = seen.x, gy = seen.y;
                for (const auto& q : sc.route) if (q.station >= goal) { gx = q.x; gy = q.y; break; }
                togo = planlattice::Build(sc, k, .5 * (seen.x + gx), .5 * (seen.y + gy), goal, sh); builtAt = t; }
            p = energyplan::Plan(seen, k, want, sc, mem, t, {}, sh, &togo); nextPlan = t + every;
            if (t >= 2.0) { if (std::fabs(p.first.input) > .05 && std::fabs(lastIn) > .05 && (p.first.input > 0) != (lastIn > 0)) ++w.steerFlips;
                if (lastAsk >= 0 && std::fabs(p.speedAhead - lastAsk) > 2.0) ++w.speedJumps; }
            if (std::fabs(p.first.input) > .05) lastIn = p.first.input; lastAsk = p.speedAhead;
        }
        bicycle::Control c = p.first; c.input *= gain; s.mode = 0; s = bicycle::Step(s, c, k, .05);
        sc.Route(s.x, s.y, hint, lat, half, hint); if (t >= 2.0) w.maxLat = std::max(w.maxLat, std::fabs(lat)); w.station = hint;
    }
    return w;
}
static void Wall(planscene::Scene& sc, double x0, double y0, double x1, double y1, std::initializer_list<double> heights, double step = .5) {
    const double len = std::hypot(x1 - x0, y1 - y0); const int n = std::max(1, int(len / step));
    for (int i = 0; i <= n; ++i) for (double h : heights) sc.solids.push_back({ x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, h, h, .15 });
}
static void Straight(planscene::Scene& sc, double length) { for (double y = -20; y <= length; y += 1) sc.route.push_back({ 0, y }); sc.floor = [](double, double, double& z) { z = 0; return true; }; }


// Step 4 (D13): drive to a goal point with no lane. The goal scene and its field are rebuilt every second or 5 m (as the adapter will).
struct GoalRun { double seconds = 0, minClear = 1e9, distance = 1e9, speed = 0, heading = 0; int gearFlips = 0, notOk = 0; bool arrived = false; };
static GoalRun GoalDrive(planscene::Scene& sc, bicycle::State s, const bicycle::Spec& k, double want, double limit, planscene::Scene::Goal g, bool trace = false) {
    sc.Index(); energyplan::Memory mem; GoalRun r; double lastDir = 0, builtAt = -1e9, bx = 1e9, by = 1e9; planlattice::Field togo;
    for (double t = 0; t < limit; t += .1) {
        if (t - builtAt >= 1.0 || std::hypot(s.x - bx, s.y - by) >= 5.0) {
            sc.SetGoal(s.x, s.y, g, k.halfLength, energyplan::Weights{}.stopGap);
            double gx = g.x, gy = g.y;  // the window: between the car and the field's goal (the goal, or the sub-goal mark)
            if (!sc.goalFinal) { const double f = sc.goalStation / sc.route.back().station; gx = s.x + (g.x - s.x) * f; gy = s.y + (g.y - s.y) * f; }
            togo = planlattice::Build(sc, k, .5 * (s.x + gx), .5 * (s.y + gy), sc.goalStation, std::nan(""), {}, s.x, s.y); builtAt = t; bx = s.x; by = s.y;
        }
        const double d = std::hypot(s.x - g.x, s.y - g.y);
        const auto p = energyplan::Plan(s, k, d < g.radius ? 0.0 : want, sc, mem, t, {}, std::nan(""), &togo);
        if (!p.ok) ++r.notOk;
        for (int i = 0; i < 2; ++i) s = bicycle::Step(s, p.first, k, .05);
        r.minClear = std::min(r.minClear, sc.Clearance(s.x, s.y, s.heading, 0, k));
        const double dir = s.speed > .2 ? 1 : s.speed < -.2 ? -1 : 0;
        if (dir != 0 && lastDir != 0 && dir != lastDir) ++r.gearFlips;
        if (dir != 0) lastDir = dir;
        r.seconds = t; r.distance = std::hypot(s.x - g.x, s.y - g.y); r.speed = s.speed; r.heading = s.heading;
        if (trace && int(t * 10) % 10 == 0) std::printf("   t=%4.1f pos=(%6.2f,%6.2f) hdg=%6.1f v=%5.2f d=%6.2f %s E=%.1f\n", t, s.x, s.y, s.heading * 180 / kPi, s.speed, r.distance, p.why, p.energy);
        if (r.distance <= g.radius + .5 && std::fabs(s.speed) < .3) { r.arrived = true; break; }
    }
    return r;
}

int main() {
#ifndef FVO_RELEASE
    {   // The planned collision must report the same sensor and object that supplied its hard shape.
        planscene::Scene sc;
        Straight(sc, 40);
        planrun::Frame frame{};
        planworld::Shape hit;
        hit.source = planworld::Source::Volume;
        hit.centre = {0, 5, 1};
        hit.halfHeight = .6;
        hit.owner = 0x102;
        hit.sensorBody = 0x203;
        hit.layer = 17;
        planinput::AddShapes(sc, frame, &hit, 1);
        sc.Index();
        bicycle::State car{};
        car.speed = 3;
        energyplan::Candidate lane;
        lane.count = 1;
        lane.seg[0] = {0, 3, energyplan::kSteps * energyplan::kDt};
        lane.seg[0].follow = true;
        energyplan::Terms terms;
        energyplan::BlockTrace trace;
        double clearance;
        const auto score = energyplan::Score(lane, car, bicycle::Spec{}, 3, sc, {}, nullptr, 0,
                                             terms, nullptr, clearance, std::nan(""), nullptr, &trace);
        Check(!std::isfinite(score) && trace.valid && trace.clearance < 0 && trace.floorKnown &&
              trace.shape.owner == 0x102 && trace.shape.body == 0x203 && trace.shape.layer == 17 &&
              std::strcmp(trace.shape.sensor, "volume") == 0 && trace.y > 0 && trace.seconds > 0,
              "sensor trace: a rejected rollout preserves the exact blocker's World provenance and predicted pose");
    }
#endif

    bicycle::Spec k; k.car = carmodel::Fallback(2.3); k.halfWidth = 1.0; k.halfLength = 2.3;
    std::printf("car: lock %.2f rad, wheelbase %.2f m, min radius %.2f m\n", k.car.lock, k.car.wheelbase, k.MinRadius());

    {   // 1. Open road: drives at the wanted speed, never reverses.
        planscene::Scene sc; Straight(sc, 120); Wall(sc, -5, -20, -5, 120, { .6 }); Wall(sc, 5, -20, 5, 120, { .6 });
        bicycle::State s; s.y = 0; s.speed = 0;
        const auto r = Drive(sc, s, k, 10, 20, 80);
        Check(r.reached && r.gearFlips == 0 && r.minClear > 0, "open road: reaches 80 m forward, no reversing, no contact");
    }
    {   // LD-1 (2026-11-19, the 21:26 run): a 4 m walled lane (walls 2 m either side of the centre, the car 2.4 m wide): the follow layer
        // keeps the centre; no swinging between lane offsets.
        planscene::Scene sc; Straight(sc, 120); Wall(sc, -2.0, -20, -2.0, 120, { .6 }); Wall(sc, 2.0, -20, 2.0, 120, { .6 });
        bicycle::State s; s.y = 0; s.speed = 4;
        const auto r = Drive(sc, s, k, 9, 25, 80);
        std::printf("   narrow walled lane: reached=%d max lateral %.2f m, steering side swaps %d, least clearance %.2f m\n", int(r.reached), r.maxLat, r.steerFlips, r.minClear);
        Check(r.reached && r.maxLat < .35 && r.minClear > 0 && r.gearFlips == 0, "LD-1: a 4 m walled lane: stays on the centre (under 0.35 m off), no contact, no reversing");
    }
    {   // 2. A rock in the lane: goes round it forward.
        planscene::Scene sc; Straight(sc, 80); Wall(sc, -6, -20, -6, 80, { .6 }); Wall(sc, 6, -20, 6, 80, { .6 });
        for (double x = -1.2; x <= 1.2; x += .4) for (double y = 28.8; y <= 31.2; y += .4) sc.solids.push_back({ x, y, .8, .8, .2 });
        bicycle::State s; s.speed = 6;
        const auto r = Drive(sc, s, k, 8, 20, 50);
        Check(r.reached && r.gearFlips == 0 && r.minClear > 0, "rock in the lane: drives round it, no reversing, no contact");
    }
    {   // 3. The 17:41 barrier: hits at 0.30, 0.64 and 0.98 m right across the road. It is a wall, never climbed.
        planscene::Scene sc; Straight(sc, 60); Wall(sc, -6, -20, -6, 60, { .6 }); Wall(sc, 6, -20, 6, 60, { .6 });
        Wall(sc, -6, 20, 6, 20, { .30, .64, .98 });
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 15, 40);
        Check(!r.reached && r.maxY < 20 - k.halfLength + .05 && r.minClear >= 0, "barrier (0.30/0.64/0.98 m hits): stops short, never climbs it");
    }
    {   // 4. A kerb (hits only at 0.20 m) across the road: rolled over.
        planscene::Scene sc; Straight(sc, 60); Wall(sc, -6, -20, -6, 60, { .6 }); Wall(sc, 6, -20, 6, 60, { .6 });
        Wall(sc, -6, 20, 6, 20, { .20 });
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 15, 40);
        Check(r.reached && r.gearFlips == 0, "kerb (0.20 m): rolled over");
    }
    {   // 5. The 17:40 ramp: a 23 deg ramp measured as 2 m stairs with uneven steps. Driven, never refused for slope.
        planscene::Scene sc; Straight(sc, 80); Wall(sc, -6, -20, -6, 80, { 30 }, 1.0); Wall(sc, 6, -20, 6, 80, { 30 }, 1.0);
        sc.solids.clear();
        sc.floor = [](double, double y, double& z) { const double u = std::clamp(y - 10, 0.0, 40.0); const int step = int(u / 2); z = step * .84 + (step % 3 == 1 ? -.21 : step % 3 == 2 ? .6 : 0); return true; };
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 20, 60);
        Check(r.reached && r.notOk == 0, "stair-stepped 23 deg ramp: driven up, never refused");
    }
    {   // 6. The hairpin: a U-turn tighter than the car turns (lane radius 4 m, car min radius above 5 m) between an inner barrier and
        //    outer walls. Forward alone cannot make it; the planner must find a turn with reversing, and never touch a wall.
        planscene::Scene sc;
        for (double y = -30; y <= 0; y += 1) sc.route.push_back({ 0, y });
        for (int i = 1; i < 16; ++i) { const double a = kPi - kPi * i / 16; sc.route.push_back({ 4 + 4 * std::cos(a), 4 * std::sin(a) }); }
        for (double y = 0; y >= -30; y -= 1) sc.route.push_back({ 8, y });
        sc.floor = [](double, double, double& z) { z = 0; return true; };
        Wall(sc, 4, -30, 4, 0, { .30, .64, .98 });                        // inner barrier, as the log saw it
        Wall(sc, -2.8, -30, -2.8, 0, { .6 }); Wall(sc, 10.8, -30, 10.8, 0, { .6 });
        for (int i = 0; i <= 40; ++i) { const double a = kPi * i / 40; sc.solids.push_back({ 4 + 6.8 * std::cos(a), 6.8 * std::sin(a), .6, .6, .15 }); }
        bicycle::State s; s.x = 0; s.y = -22; s.speed = 4;
        const double goal = 30 + kPi * 4 + 10;
        const auto r = Drive(sc, s, k, 6, 60, goal, true);
        std::printf("   hairpin: reached=%d in %.1f s, gear changes %d, closest %.2f m, end (%.1f,%.1f)\n", r.reached, r.seconds, r.gearFlips, r.minClear, r.end.x, r.end.y);
        Check(r.reached, "hairpin tighter than the car: gets round");
        Check(r.minClear >= 0, "hairpin: never touches a wall");
        Check(r.gearFlips <= 6, "hairpin: a few deliberate gear changes, not rocking");
    }
    // ---- One-authority step 1: D5 holds / line end, D4 moving users (the world moves while the car drives) ----
    auto mover = [](double x0, double y0, double vx, double vy, double t, double heading) {
        planscene::Mover m; m.heading = heading; m.halfWidth = 1.0; m.halfLength = 2.3; m.base = 0; m.top = 2; m.margin = .3;
        for (int i = 0; i < planscene::kMoverSamples; ++i) { const double tt = t + i * planscene::kMoverStep; m.x[i] = x0 + vx * tt; m.y[i] = y0 + vy * tt; }
        return m; };
    {   // D5: a stop line 40 m on (a route hold): the car stops with its front short of it, and does not creep over.
        planscene::Scene sc; Straight(sc, 120); Wall(sc, -5, -20, -5, 120, { .6 }); Wall(sc, 5, -20, 5, 120, { .6 });
        bicycle::State s; s.speed = 8;
        const auto r = Drive(sc, s, k, 12, 15, 1e9, false, [](planscene::Scene& w, double, const bicycle::State&) { w.stopStation = 20 + 40; });  // car starts at station 20
        std::printf("   stop line at y=40: front reached y=%.2f, end speed %.2f\n", r.maxFront, r.end.speed);
        Check(r.maxFront <= 40.0 + 1e-6 && std::fabs(r.end.speed) < .3, "D5 stop line: stops with the front short of it");
    }
    {   // D4 queue: a car ahead in the lane at 3 m/s. We follow behind it without touching, at its speed, and never stop for good.
        planscene::Scene sc; Straight(sc, 200); Wall(sc, -2.2, -20, -2.2, 200, { .6 }); Wall(sc, 2.2, -20, 2.2, 200, { .6 });  // one lane, no passing
        bicycle::State s; s.speed = 8;
        const auto r = Drive(sc, s, k, 12, 20, 1e9, false, [&](planscene::Scene& w, double t, const bicycle::State&) { w.movers = { mover(0, 25, 0, 3, t, 0) }; });
        std::printf("   queue: closest %.2f m, end speed %.2f, travelled to y=%.1f\n", r.minMover, r.end.speed, r.end.y);
        Check(r.minMover > -.25, "D4 queue: never touches the car ahead");
        Check(std::fabs(r.end.speed - 3.0) < 1.2 && r.end.y > 30, "D4 queue: follows at about its speed, keeps moving");
    }
    {   // D4 crossing: a car crosses the road 25 m ahead at 4 m/s. We let it pass (slow or wait), no contact, then carry on.
        planscene::Scene sc; Straight(sc, 120); Wall(sc, -6, -20, -6, 120, { .6 }); Wall(sc, 6, -20, 6, 120, { .6 });
        bicycle::State s; s.speed = 8;
        const auto r = Drive(sc, s, k, 10, 20, 60, false, [&](planscene::Scene& w, double t, const bicycle::State&) { w.movers = { mover(-14, 25, 4, 0, t, 1.5708) }; });
        std::printf("   crossing: closest %.2f m, reached=%d in %.1f s\n", r.minMover, int(r.reached), r.seconds);
        Check(r.minMover > -.25 && r.reached, "D4 crossing: lets it pass without contact, then reaches 60 m");
    }
    {   // D4 follower (the B1.2 case: a car 1 m behind froze every branch): a faster car comes up from behind and closes to a short gap. Its
        // prediction (9 m/s straight on) says it will run into us; really it stays behind. It is not ours to avoid by standing: we keep driving.
        planscene::Scene sc; Straight(sc, 200); Wall(sc, -6, -20, -6, 200, { .6 }); Wall(sc, 6, -20, 6, 200, { .6 });
        bicycle::State s; s.speed = 6;
        double followerY = -15, lastT = 0;
        const auto r = Drive(sc, s, k, 8, 15, 80, false, [&](planscene::Scene& w, double t, const bicycle::State& car) {
            // It closes at 9 m/s but keeps a 1 m gap behind our rear bumper (as a real follower does); its prediction still says 9 m/s on.
            followerY = std::min(followerY + 9 * std::max(0.0, t - lastT), car.y - (2 * 2.3 + 1.0)); lastT = t;
            w.movers = { mover(0, followerY, 0, 9, 0, 0) }; });
        std::printf("   follower: reached=%d not_ok=%d end y=%.1f speed %.2f in %.1f s\n", int(r.reached), r.notOk, r.end.y, r.end.speed, r.seconds);
        // Step 1 checks "never frozen" (no update with every plan ruled out, the car keeps moving). Its speed (3 m/s here) is the known
        // window-edge crawl, fixed in design step 2.
        Check(r.notOk == 0 && r.end.y > 30 && r.end.speed > 1, "D4 follower behind: never frozen, keeps moving");
    }
    // ---- One-authority step 3: rail or flight (D6) and facing (D7), in the same energy ----
    auto road = [&](planscene::Scene& sc, double length, int travel, bool hover) {
        Straight(sc, length); Wall(sc, -6, -20, -6, length, { .6 }); Wall(sc, 6, -20, 6, length, { .6 }); sc.travel = travel; sc.hoverPossible = hover; };
    {   // D6 both: a 1 m barrier right across the road (the 17:41 heights). The car lifts off, crosses over it, lands, and goes on. No contact.
        planscene::Scene sc; road(sc, 140, 0, true); Wall(sc, -6, 20, 6, 20, { .30, .64, .98 });
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 30, 100);  // 80 m of road after the barrier to come down on
        std::printf("   barrier, both: reached=%d hover %.1f s, mode switches %d, closest %.2f, end mode %d\n", int(r.reached), r.hoverSeconds, r.modeSwitches, r.minClear, r.end.mode);
        Check(r.reached && r.minClear >= 0 && r.hoverSeconds > 0, "D6 both: flies over a barrier wheels cannot pass, no contact");
        Check(r.end.mode == 0, "D6 both: lands again after it");
    }
    {   // NH-1 drive-only (user 2026-11-15: "the route said there should be no hover"): the same barrier. Wheels cannot go on, so it stops
        // before it on its wheels. It never lifts off, and it never touches the barrier.
        planscene::Scene sc; road(sc, 80, 1, true); Wall(sc, -6, 20, 6, 20, { .30, .64, .98 });
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 25, 50);
        std::printf("   barrier, drive-only: reached=%d hover %.1f s, closest %.2f, max y %.1f\n", int(r.reached), r.hoverSeconds, r.minClear, r.maxY);
        Check(r.hoverSeconds == 0 && r.minClear >= 0 && r.maxY < 20, "NH-1 drive-only: never hovers, stops before a barrier wheels cannot pass");
    }
    {   // NH-1 drive-only: a car that starts 3 m up (bumped off its wheels, or the hover just switched on) only comes down, then drives on.
        planscene::Scene sc; road(sc, 120, 1, true);
        bicycle::State s; s.speed = 4; s.mode = 1; s.z = 3.0;
        const auto r = Drive(sc, s, k, 8, 20, 60);
        std::printf("   in the air, drive-only: reached=%d highest %.2f m, end mode %d, hover %.1f s\n", int(r.reached), r.maxAbove, r.end.mode, r.hoverSeconds);
        Check(r.reached && r.maxAbove <= 3.0 + 1e-6 && r.end.mode == 0 && r.hoverSeconds < 3, "NH-1 drive-only: a car in the air only comes down, never climbs");
    }
    {   // D6 flight-only: never on its wheels; holds hoverClear over the floor.
        planscene::Scene sc; road(sc, 120, 2, true);
        bicycle::State s; s.speed = 0;
        const auto r = Drive(sc, s, k, 10, 20, 60);
        std::printf("   flight-only: reached=%d hover %.1f s of %.1f, highest %.2f m\n", int(r.reached), r.hoverSeconds, r.seconds, r.maxAbove);
        Check(r.reached && r.hoverSeconds >= r.seconds - .3 && r.maxAbove > 1.5, "D6 flight-only: flies the whole way");
    }
    {   // D6 both: the stair-stepped 23 deg ramp. Wheels can take it, so it stays on its wheels (the 17:40 stall came from a flight decision).
        planscene::Scene sc; road(sc, 80, 0, true); sc.solids.clear();
        sc.floor = [](double, double y, double& z) { const double u = std::clamp(y - 10, 0.0, 40.0); const int step = int(u / 2); z = step * .84 + (step % 3 == 1 ? -.21 : step % 3 == 2 ? .6 : 0); return true; };
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 20, 60);
        Check(r.reached && r.hoverSeconds == 0, "D6 both: a ramp wheels can take stays on wheels");
    }
    {   // D6 both: a 52 deg ramp, too steep for wheels: it flies it.
        planscene::Scene sc; road(sc, 80, 0, true); sc.solids.clear();
        sc.floor = [](double, double y, double& z) { z = std::clamp(y - 15, 0.0, 12.0) * 1.3; return true; };
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 25, 50);
        std::printf("   steep ramp: reached=%d hover %.1f s\n", int(r.reached), r.hoverSeconds);
        Check(r.reached && r.hoverSeconds > 0, "D6 both: a slope too steep for wheels is flown");
    }
    {   // D7 facing: on a flight-only stretch north with the nose to be kept east (a combat line): it moves north while its nose turns east.
        planscene::Scene sc; road(sc, 120, 2, true); sc.facing = 1.5708;
        bicycle::State s; s.speed = 0;
        const auto r = Drive(sc, s, k, 6, 20, 40, std::getenv("D7_TRACE") != nullptr);
        std::printf("   facing east, travelling north: reached=%d end heading %.0f deg\n", int(r.reached), r.end.heading * 180 / kPi);
        Check(r.reached && std::cos(r.end.heading - 1.5708) > .85, "D7: keeps the facing while travelling (strafe)");
    }
    {   // Step 2: candidates scored on several threads pick exactly what one thread picks (same candidate, energy and first command).
        planscene::Scene sc;
        for (double y = -30; y <= 0; y += 1) sc.route.push_back({ 0, y });
        for (int i = 1; i < 16; ++i) { const double a = kPi - kPi * i / 16; sc.route.push_back({ 4 + 4 * std::cos(a), 4 * std::sin(a) }); }
        for (double y = 0; y >= -30; y -= 1) sc.route.push_back({ 8, y });
        sc.floor = [](double, double, double& z) { z = 0; return true; };
        Wall(sc, 4, -30, 4, 0, { .30, .64, .98 }); Wall(sc, -2.8, -30, -2.8, 0, { .6 }); Wall(sc, 10.8, -30, 10.8, 0, { .6 });
        for (int i = 0; i <= 40; ++i) { const double a = kPi * i / 40; sc.solids.push_back({ 4 + 6.8 * std::cos(a), 6.8 * std::sin(a), .6, .6, .15 }); }
        sc.Index();
        const auto field = planlattice::Build(sc, k, 2, -8, 40, 8, {}, 0, -10);
        const energyplan::ForEach parallel = [](int n, const std::function<void(int)>& fn) {
            std::vector<std::thread> th; for (int t = 0; t < 4; ++t) th.emplace_back([&, t] { for (int i = t; i < n; i += 4) fn(i); }); for (auto& x : th) x.join(); };
        bool same = true; int states = 0;
        for (double y : { -20.0, -10.0, -4.0, -1.0 }) for (double h : { 0.0, .4, .9 }) for (double v : { 0.0, 2.0, -1.0 }) {
            bicycle::State st; st.x = .3; st.y = y; st.heading = h; st.speed = v;
            energyplan::Memory m1, m2; double hint, lat, half; sc.Route(st.x, st.y, hint, lat, half);
            const auto a = energyplan::Plan(st, k, 6, sc, m1, 0, {}, hint, &field);
            const auto b = energyplan::Plan(st, k, 6, sc, m2, 0, {}, hint, &field, parallel);
            same = same && a.ok == b.ok && a.energy == b.energy && a.first.input == b.first.input && a.first.speed == b.first.speed && a.feasible == b.feasible; ++states;
        }
        // The cost-to-go search stopped 60 past each car's own cost (planlattice beyondCar) picks what the full search picks.
        bool sameStop = true;
        for (double y : { -20.0, -10.0, -4.0, -1.0 }) for (double h : { 0.0, .9 }) for (double v : { 0.0, 2.0 }) {
            bicycle::State st; st.x = .3; st.y = y; st.heading = h; st.speed = v;
            const auto stopped = planlattice::Build(sc, k, 2, -8, 40, 8, {}, st.x, st.y);
            energyplan::Memory m1, m2; double hint, lat, half; sc.Route(st.x, st.y, hint, lat, half);
            const auto a = energyplan::Plan(st, k, 6, sc, m1, 0, {}, hint, &field), b = energyplan::Plan(st, k, 6, sc, m2, 0, {}, hint, &stopped);
            sameStop = sameStop && a.ok == b.ok && a.first.input == b.first.input && a.first.speed == b.first.speed && std::string(a.why) == b.why;
        }
        Check(sameStop, "step 2: the early-stopped cost to go picks what the full one picks");
        std::printf("   parallel vs serial: %d states compared\n", states);
        Check(same, "step 2: parallel scoring picks exactly what serial scoring picks");
    }


    {   // LK-1 (2026-11-13 09:56 run: the car cut bends 5-9 m off a 3.78 m half-width lane at 9-11 m/s with nothing in the way): a 90 deg bend,
        // radius 35 m, in wide open ground; the whole body stays inside its own lane.
        planscene::Scene sc; sc.laneHalf = 3.78; sc.floor = [](double, double, double& z) { z = 0; return true; };
        for (double y = -20; y < 0; y += 1) sc.route.push_back({ 0, y });
        for (double a = 0; a <= 1.5708 + 1e-9; a += .02) sc.route.push_back({ 35 - 35 * std::cos(a), 35 * std::sin(a) });
        for (double x = 36; x <= 140; x += 1) sc.route.push_back({ x, 35 });
        bicycle::State s; s.speed = 10;
        const auto r = Drive(sc, s, k, 11, 25, 110, std::getenv("BEND_TRACE") != nullptr);
        std::printf("   bend in open ground: reached=%d in %.1f s, furthest off the line %.2f m (body inside the lane up to %.2f m)\n", int(r.reached), r.seconds, r.maxLat, sc.laneHalf - k.halfWidth);
        Check(r.reached && r.maxLat + k.halfWidth <= sc.laneHalf + .3, "LK-1: keeps its own lane round a bend in open ground (no corner cutting)");
        Check(r.reached && r.seconds < 12.0, "LF-1: and keeps its speed doing it (90 m at a wanted 11 m/s in under 12 s; the fixed-steering shapes alone took 19.6 s)");
    }

    {   // D1 in the energy plan (removal step 1; 19:55 and 20:14 runs: the car stood 9 m short of a fallen tree in its lane with the other lane
        // open, 227 of 408 moves feasible, "stop" cheapest). A static block over the right half of the car's lane, the road open to the left
        // (the oncoming lane), walls at both road edges: the car goes round and back. The same block as an occupied car in traffic: it waits.
        auto road = [&](bool waits) {
            planscene::Scene sc; sc.laneHalf = 1.9; Straight(sc, 140);
            Wall(sc, -6.0, -20, -6.0, 140, { .5, 1.2 }); Wall(sc, 2.4, -20, 2.4, 140, { .5, 1.2 });
            for (double x = -.3; x <= 2.3; x += .4) for (double y = 38; y <= 43; y += .4) { planscene::Solid b{ x, y, .3, 2.5, .2 }; b.waits = waits; sc.solids.push_back(b); }
            return sc; };
        bicycle::State s; s.speed = 8;
        auto open = road(false); const auto r = Drive(open, s, k, 11, 30, 90, std::getenv("D1_TRACE") != nullptr);
        std::printf("   static block in the lane: reached=%d in %.1f s, furthest off the line %.2f m, least clearance %.2f m\n", int(r.reached), r.seconds, r.maxLat, r.minClear);
        Check(r.reached && r.minClear >= 0, "D1: a static block in the car's own lane is driven round through the open lane beside it (no stop in front of it)");
        auto queue = road(true); const auto q = Drive(queue, s, k, 11, 20, 90, std::getenv("D1Q_TRACE") != nullptr);
        std::printf("   occupied car in the lane: reached=%d, front got to y=%.1f (car starts at 38)\n", int(q.reached), q.maxFront);
        Check(!q.reached && q.maxFront < 38.0 && q.minClear >= 0, "D1: an occupied car in traffic in the lane is waited behind (the lane edge stays charged)");
    }

    {   // SG-1: the car model accelerates less up a 0.33 grade and brakes less down it; flat is unchanged.
        bicycle::State a; bicycle::State flat = a, up = a, down = a; down.speed = 8; bicycle::State flatDown = down;
        for (int i = 0; i < 10; ++i) { flat = bicycle::Step(flat, { 0, 10 }, k, .2); up = bicycle::Step(up, { 0, 10 }, k, .2, .33); flatDown = bicycle::Step(flatDown, { 0, 0 }, k, .2); down = bicycle::Step(down, { 0, 0 }, k, .2, -.33); }
        std::printf("   slope model: 2 s from rest flat %.2f m/s, up 0.33 %.2f m/s; braking from 8 m/s flat %.2f, down 0.33 %.2f\n", flat.speed, up.speed, flatDown.speed, down.speed);
        Check(up.speed < flat.speed * .5 && up.speed > 0 && down.speed > flatDown.speed && bicycle::Step(a, { 0, 10 }, k, .2, 0).speed == bicycle::Step(a, { 0, 10 }, k, .2).speed,
              "SG-1: slower to accelerate uphill, slower to stop downhill, flat unchanged");
    }
    {   // O1 / O3 (2026-11-14, 11:03 run: on a clear road each new plan bent the other way and the speed ask swung 16.5 <-> 9 m/s): replanning every
        // 0.1 s while the car follows, on a straight lane and round the LK-1 bend, the steering does not swap sides and the speed ask does not jump.
        planscene::Scene sc; sc.laneHalf = 1.9; Straight(sc, 300);
        bicycle::State s; s.speed = 14;
        const auto r = Drive(sc, s, k, 16.5, 15, 260);
        std::printf("   straight: steering side swaps %d, speed ask jumps over 2 m/s %d, plan switches %d, off the line up to %.2f m\n", r.steerFlips, r.speedJumps, r.switches, r.maxLat);
        Check(r.steerFlips <= 2 && r.speedJumps <= 1 && r.maxLat < .5, "O1/O3: on a straight lane the plan does not weave and the speed ask does not jump");
        planscene::Scene sb; sb.laneHalf = 3.78; sb.floor = [](double, double, double& z) { z = 0; return true; };
        for (double y = -20; y < 0; y += 1) sb.route.push_back({ 0, y });
        for (double a = 0; a <= 1.5708 + 1e-9; a += .02) sb.route.push_back({ 35 - 35 * std::cos(a), 35 * std::sin(a) });
        for (double x = 36; x <= 140; x += 1) sb.route.push_back({ x, 35 });
        bicycle::State sbs; sbs.speed = 10;
        const auto rb = Drive(sb, sbs, k, 11, 25, 110);
        std::printf("   bend: steering side swaps %d, speed ask jumps %d, plan switches %d\n", rb.steerFlips, rb.speedJumps, rb.switches);
        Check(rb.reached && rb.steerFlips <= 2 && rb.speedJumps <= 2, "O1/O3: round the bend the same");
        // The game's loop: plans 0.1 s apart from a car 0.05 s old, the real steering 0.8 of the model's (11:06:58 run: each plan bent the other way).
        planscene::Scene sl; sl.laneHalf = 1.9; Straight(sl, 400); bicycle::State sls; sls.speed = 14;
        const auto lw = DriveLagged(sl, sls, k, 16.5, 15, .05, .1, .8);
        std::printf("   straight, lagged and mismatched: steering side swaps %d, speed ask jumps %d, off the line up to %.2f m, got to %.0f m\n", lw.steerFlips, lw.speedJumps, lw.maxLat, lw.station);
        Check(lw.steerFlips <= 3 && lw.maxLat < .6, "O1: with the game's delay and a car unlike the model, the plan still does not weave");
    }
    {   // O2 (12:28 run: a forward plan 35 cheaper waited 2.5 s behind a fixed direction hold): a direction change is a switch like any other.
        // A clearly better forward plan replaces a committed back-up once it has won for challengeFor s (after holdAfter s); a near tie does not.
        planscene::Scene sc; sc.laneHalf = 1.9; Straight(sc, 60); sc.Index();
        bicycle::State s; double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half);
        energyplan::Memory mem; mem.have = true; mem.at = 0; mem.best.count = 1; mem.best.seg[0] = { 0, -1.8, 6 }; mem.best.kind = "reverse"; mem.switchedAt = 0;
        for (int i = 0; i <= energyplan::kSteps; ++i) { mem.traj[i] = s; mem.traj[i].speed = -1.8; mem.traj[i].y = -1.8 * i * energyplan::kDt; }
        const auto a = energyplan::Plan(s, k, 10, sc, mem, .3, {}, hint, nullptr);
        std::printf("   at 0.3 s: kept=%d why=%s rival=%s gap=%.1f\n", int(a.kept), a.switchWhy, a.rival, a.rivalGap);
        Check(a.kept && a.first.speed <= 0, "O2: within holdAfter of the last switch the back-up is kept");
        energyplan::Result b; double switchedAt = -1; for (double t = .4; t <= 2.0 + 1e-9; t += .1) { b = energyplan::Plan(s, k, 10, sc, mem, t, {}, hint, nullptr); if (switchedAt < 0 && b.first.speed > 0) switchedAt = t; }
        std::printf("   forward took over at %.1f s (why %s)\n", switchedAt, b.switchWhy);
        Check(switchedAt > 0 && switchedAt <= 1.1, "O2: a clearly better forward plan takes over within about a second, no fixed 1.5 s hold");
    }
    // Step 4 (D13): goal driving, no lane.
    const bool goalTrace = std::getenv("GOAL_TRACE") != nullptr;
    auto open = [](planscene::Scene& sc) { sc.floor = [](double, double, double& z) { z = 0; return true; }; };
    {   planscene::Scene sc; open(sc); Wall(sc, -4, 12, 4, 12, { 1.0 });  // a wall between the car and the goal
        bicycle::State s; planscene::Scene::Goal g; g.x = 0; g.y = 25;
        const auto r = GoalDrive(sc, s, k, 8, 25, g, goalTrace);
        std::printf("   goal round a wall: arrived=%d in %.1f s, d=%.2f v=%.2f clear=%.2f flips=%d notOk=%d\n", r.arrived, r.seconds, r.distance, r.speed, r.minClear, r.gearFlips, r.notOk);
        Check(r.arrived && r.minClear >= 0 && r.notOk == 0, "D13 goal: round a wall to the goal and stop there");
    }
    {   planscene::Scene sc; open(sc);
        bicycle::State s; planscene::Scene::Goal g; g.x = 0; g.y = -15;  // behind the car
        const auto r = GoalDrive(sc, s, k, 6, 30, g, goalTrace);
        std::printf("   goal behind: arrived=%d in %.1f s, d=%.2f flips=%d\n", r.arrived, r.seconds, r.distance, r.gearFlips);
        Check(r.arrived && r.gearFlips <= 4, "D13 goal: a goal behind the car is reached (turn round, no rocking)");
    }
    {   planscene::Scene sc; open(sc);
        bicycle::State s; planscene::Scene::Goal g; g.x = 12; g.y = 20; g.arriveYaw = kPi / 2;  // arrive facing east
        const auto r = GoalDrive(sc, s, k, 6, 30, g, goalTrace);
        const double err = std::fabs(std::remainder(r.heading - g.arriveYaw, 2 * kPi)) * 180 / kPi;
        std::printf("   arrive heading: arrived=%d in %.1f s, d=%.2f heading error %.0f deg\n", r.arrived, r.seconds, r.distance, err);
        Check(r.arrived && err < 30, "D13 goal: arrives facing the arrive heading");
    }
    {   planscene::Scene sc; open(sc); Wall(sc, -5, 40, 3, 40, { 1.0 }); Wall(sc, -3, 80, 5, 80, { 1.0 });
        bicycle::State s; planscene::Scene::Goal g; g.x = 0; g.y = 120;  // far: through sub-goals
        const auto r = GoalDrive(sc, s, k, 10, 40, g, goalTrace);
        std::printf("   far goal: arrived=%d in %.1f s, d=%.2f clear=%.2f notOk=%d\n", r.arrived, r.seconds, r.distance, r.minClear, r.notOk);
        Check(r.arrived && r.minClear >= 0 && r.seconds < 30, "D13 goal: a far goal through sub-goals, round two walls");
    }
    // Step 4 (D12): the spot test pulse is a requested candidate.
    {   planscene::Scene sc; Straight(sc, 60); sc.Index();
        energyplan::Memory mem; bicycle::State s; double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half);
        energyplan::Plan(s, k, 0, sc, mem, 0, {}, hint, nullptr);  // a committed plan (standing)
        const double at = mem.at; const std::string kept = mem.best.kind ? mem.best.kind : "";
        energyplan::TestPulse pulse; pulse.active = true;
        const auto p = energyplan::Plan(s, k, 0, sc, mem, .5, {}, hint, nullptr, energyplan::Serial, &pulse);
        Check(p.pulse && !p.testRefused && p.first.speed > 0 && mem.at == at && kept == (mem.best.kind ? mem.best.kind : ""), "D12 pulse: run when safe, the committed plan kept");
        Wall(sc, -1.5, 2.45, 1.5, 2.45, { 1.0 }); sc.Index();  // a wall at the bumper: no room to push forward
        const auto q = energyplan::Plan(s, k, 0, sc, mem, 1.0, {}, hint, nullptr, energyplan::Serial, &pulse);
        std::printf("   pulse at a wall: pulse=%d refused=%d first speed %.2f (%s)\n", q.pulse, q.testRefused, q.first.speed, q.why);
        Check(!q.pulse && q.testRefused && q.first.speed <= .05, "D12 pulse: refused when it would touch a solid");
    }
    {   // CT-1 (2026-11-14 20:37 run: 0 of 405 feasible, no rule named): every rejected candidate is counted under the rule that rejected it,
        // and a lane scene lists the cheapest follow-lane plan per speed.
        planscene::Scene sc; sc.laneHalf = 1.9; Straight(sc, 200);
        Wall(sc, -1.5, 6.0, 1.5, 6.0, { 1.0 }); sc.Index();  // a wall across the lane 6 m ahead
        energyplan::Memory mem; bicycle::State s; s.speed = 8; double hint, lat, half; sc.Route(s.x, s.y, hint, lat, half);
        const auto p = energyplan::Plan(s, k, 11, sc, mem, 0, {}, hint, nullptr);
        int counted = 0; bool solid = false; for (int q = 0; q < p.rejectKinds; ++q) { counted += p.rejectCount[q]; solid |= std::strcmp(p.rejectName[q], "enters_a_solid") == 0; }
        std::printf("   wall 6 m ahead at 8 m/s: feasible %d of %d, rejections counted %d in %d rules, lane speeds %d (first %.1f m/s: %s)\n", p.feasible, p.candidates, counted, p.rejectKinds,
                    p.laneSpeedCount, p.laneSpeedCount ? p.laneSpeeds[0].speed : 0.0, p.laneSpeedCount ? (std::isfinite(p.laneSpeeds[0].energy) ? "feasible" : p.laneSpeeds[0].reject) : "-");
        Check(counted == p.candidates - p.feasible && solid && p.laneSpeedCount == 5, "CT-1: every rejection named and counted; follow-lane plans listed per speed");
    }
    {   // MP-1 (2026-11-14 19:44 run: the full-speed plan scored -135.0 against a slower kept plan's -135.1 and the hold kept the slow one, its
        // margin 1.6 from 1% of a total of -133): from 3 m/s on an open straight lane the car reaches the wanted 16.5 m/s and keeps it.
        planscene::Scene sc; sc.laneHalf = 1.9; Straight(sc, 400);
        bicycle::State s; s.speed = 3;
        double vEnd = 0; const auto r = Drive(sc, s, k, 16.5, 15, 380, false, [&](planscene::Scene&, double, const bicycle::State& c) { vEnd = c.speed; });
        std::printf("   open straight from 3 m/s: speed after %.1f s %.2f m/s (wanted 16.5), travelled %.0f m\n", r.seconds, vEnd, r.station);
        Check(vEnd >= 15.5, "MP-1: on an open lane the car speeds up to the wanted speed (a slower plan is not kept as a near tie)");
    }
    {   // HC-1 / HC-2 (2026-11-14 19:14 run: a winding road lined by rocks; 3 m/s and 475 of 477 switches past the hold). An S of two 12 m radius
        // bends, lane half 2.0 m, rock walls 0.6 m outside each lane edge all the way. The goal moves 30 m on every 2 s / 8 m (as in the game).
        planscene::Scene sc; sc.laneHalf = 2.0; sc.floor = [](double, double, double& z) { z = 0; return true; };
        for (double y = -20; y < 0; y += 1) sc.route.push_back({ 0, y });
        for (double a = 0; a <= 1.5708 + 1e-9; a += .02) sc.route.push_back({ 12 - 12 * std::cos(a), 12 * std::sin(a) });   // right
        for (double a = 0; a <= 1.5708 + 1e-9; a += .02) sc.route.push_back({ 12 + 12 * std::sin(a), 24 - 12 * std::cos(a) });  // left
        for (double y = 25; y <= 140; y += 1) sc.route.push_back({ 24, y });
        for (std::size_t i = 1; i < sc.route.size(); ++i) {  // walls along both sides
            const double dx = sc.route[i].x - sc.route[i - 1].x, dy = sc.route[i].y - sc.route[i - 1].y, l = std::hypot(dx, dy); if (l < 1e-6) continue;
            const double nx = dy / l, ny = -dx / l, off = sc.laneHalf + .6;
            for (double side : { -1.0, 1.0 }) { planscene::Solid b; b.x = sc.route[i].x + side * off * nx; b.y = sc.route[i].y + side * off * ny; b.base = 0; b.top = 1.0; b.half = .3; sc.solids.push_back(b); } }
        bicycle::State s; s.speed = 6;
        bicycle::Spec kb = k; kb.brake = 2.5;  // the game's softest hull brake (limits.brake is clamped to 2.5-8)
        const auto r = Drive(sc, s, kb, 11, 30, 110, std::getenv("HC_TRACE") != nullptr);
        std::printf("   walled S-bend: reached=%d in %.1f s (avg %.1f m/s), least clearance %.2f m; switches: got worse %d, too close %d, enters solid %d, yields %d, timer %d; kept %d\n",
                    int(r.reached), r.seconds, r.station / std::max(.1, r.seconds), r.minClear, r.rel[0], r.rel[1], r.rel[2], r.rel[3], r.rel[4], r.rel[5]);
        Check(r.reached && r.minClear >= 0, "HC: drives the walled S-bend without touching a wall");
        Check(r.reached && r.station / std::max(.1, r.seconds) >= 6.0, "HC-2: keeps above 6 m/s on average round a walled S (the straight-on rule passes this too: the game crawl is not reproduced, HC-4 traces it)");
        std::printf("   walled S-bend: steering side swaps %d, speed ask jumps %d, instant releases %.1f a second\n", r.steerFlips, r.speedJumps, (r.rel[0] + r.rel[1]) / std::max(.1, r.seconds));
        Check(r.steerFlips <= 3 && (r.rel[0] + r.rel[1]) / std::max(.1, r.seconds) <= 3.0, "HC-1: no weave, and instant releases stay rare (the 19:14 run had 26 a second)");
    }
    {   // JL-1 (2026-11-18): joining from beside the lane: the lane is priced at offLaneEase until the car is back inside it.
        planscene::Scene sc; Straight(sc, 120); Wall(sc, -14, -20, -14, 120, { .6 }); Wall(sc, 14, -20, 14, 120, { .6 });
        bicycle::State s; s.x = 8; s.y = 0; s.speed = 0;
        const auto r = Drive(sc, s, k, 10, 25, 80);
        double lat = 0, st = 0, half = 0; sc.Route(r.end.x, r.end.y, st, lat, half);
        Check(r.reached && std::fabs(lat) <= half && r.minClear > 0, "JL-1: joins from 8 m beside the lane, reaches 80 m inside it, no contact");
    }
    {   // RV-1 (2026-11-19): the mode state machine in the plan. A barrier across the whole road (no way round): after 6 s without
        // progress the car backs up straight, never touching the barrier (it was: the ladder loosened weights and rocked, 104 swaps in 60 s).
        planscene::Scene sc; Straight(sc, 60); Wall(sc, -6, -20, -6, 60, { .6 }); Wall(sc, 6, -20, 6, 60, { .6 });
        Wall(sc, -6, 20, 6, 20, { .30, .64, .98 });
        bicycle::State s; s.speed = 5;
        const auto r = Drive(sc, s, k, 8, 20, 40);
        std::printf("   barrier, recover: reached=%d gear changes %d, closest %.2f, furthest y %.1f\n", int(r.reached), r.gearFlips, r.minClear, r.maxY);
        Check(!r.reached && r.gearFlips >= 1 && r.minClear >= 0, "RV-1: blocked by a barrier: the routine backs up, no contact");
        Check(r.gearFlips <= 8, "RV-1: and does not rock (at most the routine's own direction changes)");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
