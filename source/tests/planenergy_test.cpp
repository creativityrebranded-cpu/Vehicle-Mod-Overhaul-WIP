// ED-1: the energy driver's pure parts (which part of the plan is driven now, when the energy plan may drive, when the field is rebuilt)
// and the scene adapter rule (every measured shape counts; predictions are soft).
#define PLANENERGY_PURE
#include "planenergy.h"
#include "railforce.h"
#include "planinput.h"
#include "wheelguard.h"
#include "corridor.h"
#include "planspeed.h"
#include <cstring>
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    {
        planscene::Scene scene;
        scene.floor = [](double, double, double& z) { z = 0; return true; };
        scene.laneHalf = 3.8;
        scene.managerHeld = true;
        scene.travel = 1;
        for (int y = -20; y <= 20; ++y) scene.route.push_back({0, double(y)});
        scene.stopStation = 20;
        scene.Index();
        energyplan::TestPulse pulse;
        pulse.active = true;
        pulse.speed = -1;
        energyplan::Memory memory;
        const auto clear = energyplan::Plan({}, {}, 0, scene, memory, 1, {}, 20, nullptr, energyplan::Serial, &pulse);
        Check(clear.ok && clear.pulse && clear.first.speed < 0, "the held factory lane can admit a collision-scored reverse pulse");
        scene.solids.push_back({0, -2.0, 0, .6, .22});
        scene.Index();
        const auto blocked = energyplan::Plan({}, {}, 0, scene, memory, 2, {}, 20, nullptr, energyplan::Serial, &pulse);
        Check(!blocked.pulse && blocked.testRefused, "a rear obstacle can refuse the factory pulse");
    }
    {
        planscene::Scene scene;
        scene.floor = [](double, double, double& z) { z = 0; return true; };
        bicycle::Spec spec;
        bicycle::State pose;
        wheelguard::Command command;
        command.want = 5.25; command.vy = 5.25;
        scene.solids.push_back({0, 5, 0, .6, .22});
        scene.Index();
        Check(wheelguard::Check(&scene, pose, spec, command).speed < 4,
              "final wheel corridor caps forward speed before a measured obstacle");
        scene.solids.front().soft = true;
        scene.Index();
        Check(!std::isfinite(wheelguard::Check(&scene, pose, spec, command).speed),
              "prediction alone does not hard-stop the wheel command");
        scene.solids.front().soft = false;
        scene.Index();
        command.want = -2;
        Check(wheelguard::Check(&scene, pose, spec, command).speed < 4,
              "a reverse request still checks residual forward movement");
        scene.solids.front().y = 2;
        scene.Index();
        command.vy = 0;
        Check(wheelguard::Check(&scene, pose, spec, command).speed == wheelguard::escape,
              "a reverse move that immediately increases clearance can leave overlap");
        command.want = 2;
        Check(wheelguard::Check(&scene, pose, spec, command).speed == 0,
              "a forward move that worsens overlap may not power into the barrier");
        command.constrained = true;
        Check(wheelguard::Check(nullptr, pose, spec, command).speed == wheelguard::crawl,
              "unverified recovery can creep rather than treating missing data as a wall");
        command.constrained = false;
        Check(!std::isfinite(wheelguard::Check(nullptr, pose, spec, command).speed),
              "missing evidence alone does not globally throttle ordinary driving");
    }
    {   // DrivenPath: a forward-then-reverse trajectory drives only its forward part now.
        bicycle::State t[31]; for (int i = 0; i <= 30; ++i) { t[i].x = 0; t[i].y = i < 12 ? i * .5 : 11 * .5 - (i - 11) * .4; t[i].speed = i < 12 ? 2.5 : -2.0; }
        bool rev; const auto p = planenergy::DrivenPath(t, 31, rev);
        double maxY = -1; for (int i = 0; i < p.count; ++i) maxY = std::max(maxY, p.y[i]);
        Check(!rev && p.count >= 2 && maxY <= 5.6 && p.y[p.count - 1] >= 5.0, "forward then reverse: the forward stretch only, up to the gear change");
        for (int i = 0; i <= 30; ++i) { t[i].y = -i * .36; t[i].speed = -1.8; }
        const auto q = planenergy::DrivenPath(t, 31, rev);
        Check(rev && q.count >= 2 && q.y[1] < q.y[0], "reverse plan: a reverse path behind the car");
        for (int i = 0; i <= 30; ++i) { t[i] = {}; t[i].heading = 0; }
        const auto s = planenergy::DrivenPath(t, 31, rev);
        Check(s.count == 2, "standing plan: a short path along the nose (the follower always has one)");
    }
    {   // Open straight lane: the rollout changes with the car's yaw, the steering reference does not.
        planscene::Scene scene;
        scene.laneHalf = 3.78;
        scene.floor = [](double, double, double& z) { z = 0; return true; };
        for (int i = -20; i <= 50; ++i) scene.route.push_back({0, double(i)});
        scene.Index();
        bicycle::Spec spec;
        auto run = [&](double heading) {
            bicycle::State car;
            car.heading = heading; car.speed = 4;
            energyplan::Memory memory;
            const auto plan = energyplan::Plan(car, spec, 4, scene, memory, 1);
            planenergy::Result out;
            out.path = planenergy::DrivenPath(plan.traj, energyplan::kSteps + 1, out.reverse);
            out.laneCentre = planenergy::LaneCentrePath(plan, scene, car, spec, 20, 0, 3.78, out.reverse, false, .5);
            return out;
        };
        const auto left = run(-.65), right = run(.65);
        const auto a = pathfollow::Follow(left.SteeringPath(), 0, 0, -.65, 4, false);
        const auto b = pathfollow::Follow(right.SteeringPath(), 0, 0, .65, 4, false);
        Check(left.UsesLaneCentre() && right.UsesLaneCentre() && a.ok && b.ok &&
              a.curvature == 0 && b.curvature == 0 && a.yawRate > 0 && b.yawRate < 0 &&
              left.path.x[1] < 0 && right.path.x[1] > 0,
              "centre Follow uses the straight lane with opposite corrective yaw; the scored rollouts still diverge");

        energyplan::Result plan;
        plan.ok = true; plan.layer = "follow"; plan.first.speed = 4; plan.minClearance = 1.5;
        plan.best.count = 1;
        plan.best.seg[0].follow = true; plan.best.seg[0].speed = 4;
        bicycle::State car;
        auto canUse = [&](bool reverse = false, bool hover = false) {
            return planenergy::LaneCentrePath(plan, scene, car, spec, 20, 0, 3.78, reverse, hover, .5).count >= 2;
        };
        Check(canUse() && !canUse(true) && !canUse(false, true), "reverse or hover keeps the scored rollout");
        plan.layer = "avoid";
        Check(!canUse(), "avoidance keeps the scored rollout");
        plan.layer = "recover";
        Check(!canUse(), "recovery keeps the scored rollout");
        plan.layer = "follow"; plan.best.seg[0].offset = 1;
        Check(!canUse(), "a committed offset path keeps the scored rollout even while mode is Follow");
        plan.best.seg[0].offset = 0; plan.minClearance = .3;
        Check(!canUse(), "tight predicted clearance keeps the scored rollout");
        plan.minClearance = 1.5;
        scene.solids.push_back({0, 6, 0, 1, .2});
        scene.Index();
        Check(!canUse(), "a blocked centre reference keeps the scored rollout even when that rollout is clear");
    }
    {   // The budget for cars other than yours (removal 2026-11-14: was the shadow budget): plans in flight and each car's spacing.
        using namespace planenergy::budget;
        Check(MaySubmit(10.0, 9.7, 0), "a car last planned 0.3 s ago, nothing in flight: planned");
        Check(!MaySubmit(10.0, 9.9, 0), "a car last planned 0.1 s ago: waits for its turn");
        Check(!MaySubmit(10.0, 9.0, plansInFlight), "the pool is full: waits");
        Check(MaySubmit(5.0, 9.0, 0), "the clock went back (a new session): planned");
    }
    {   // FieldStale
        Check(planenergy::FieldStale(false, 0, 0, 0, 0, 56, 0, 30), "no field: rebuild");
        Check(!planenergy::FieldStale(true, 28, 28, 0, 0, 56, 1, 25), "car in the middle, young, goal far: keep");
        Check(planenergy::FieldStale(true, 50, 28, 0, 0, 56, 1, 25), "car near the window edge: rebuild");
        Check(planenergy::FieldStale(true, 28, 28, 0, 0, 56, 5, 25), "old: rebuild");
        Check(planenergy::FieldStale(true, 28, 28, 0, 0, 56, 1, 5), "goal nearly reached: rebuild");
    }
    {   // The adapter: a wall seen by ONE ray (support 1, no normal) is solid; a survey prediction is soft (a cost, never a block).
        planrun::Frame f; f.now = 10; f.car.halfWidth = 1; f.car.halfLength = 2.3; f.car.height = 2; f.carBase = 0; f.raysLive = true;
        auto line = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = 0; p.yaw = 0; return p; };
        auto wanted = [](double, double) { planworld::Wanted w; w.have = true; w.measured = true; w.z = 0; return w; };
        std::vector<planworld::Shape> shapes(2);
        shapes[0].source = planworld::Source::Ray; shapes[0].centre = { 0, 10, .8 }; shapes[0].support = 1; shapes[0].haveNormal = false; shapes[0].seenAt = 10; shapes[0].halfWidth = .2; shapes[0].halfLength = .2;
        shapes[1].source = planworld::Source::Survey; shapes[1].centre = { 3, 10, .8 }; shapes[1].halfWidth = .2; shapes[1].halfLength = .2; shapes[1].seenAt = 10;
        const auto sc = planinput::Scene(f, line, wanted, shapes.data(), int(shapes.size()));
        int hard = 0, soft = 0; for (const auto& s : sc.solids) (s.soft ? soft : hard)++;
        Check(hard >= 1, "one-ray wall (support 1, no normal): kept as solid (was dropped as not Sure)");
        Check(soft >= 1, "survey prediction: kept as soft (was dropped while rays are live)");
        bicycle::Spec k; k.halfWidth = 1; k.halfLength = 2.3;
        Check(sc.Clearance(0, 8, 0, 0, k) < 0 && sc.Clearance(3, 8, 0, 0, k) > 0, "the one-ray wall blocks the body; the prediction does not");
        Check(sc.SoftNear(3, 8, 0, 0, k) >= 1, "the prediction costs when the body is on it");
    }
    {   // RT-1 (2026-11-18): a ray hit world truth says never stops (mayStop false: the car's own road deck) is soft, never a block; the
        // same point with mayStop (a wall, unknown, an owner not yet answered) stays a hard solid.
        planrun::Frame f; f.now = 10; f.car.halfWidth = 1; f.car.halfLength = 2.3; f.car.height = 2; f.carBase = 0; f.raysLive = true;
        auto line = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = 0; p.yaw = 0; return p; };
        auto wanted = [](double, double) { planworld::Wanted w; w.have = true; w.measured = true; w.z = 0; return w; };
        std::vector<planworld::Shape> shapes(1);
        shapes[0].source = planworld::Source::Ray; shapes[0].centre = { 0, 6, 1.03 }; shapes[0].seenAt = 10; shapes[0].owner = 0x06002BA9; shapes[0].layer = 1;
        shapes[0].mayStop = false;
        const auto road = planinput::Scene(f, line, wanted, shapes.data(), 1);
        shapes[0].mayStop = true;
        const auto wall = planinput::Scene(f, line, wanted, shapes.data(), 1);
        bicycle::Spec k; k.halfWidth = 1; k.halfLength = 2.3;
        Check(road.Clearance(0, 6, 0, 0, k) > 0 && road.SoftNear(0, 6, 0, 0, k) >= 1, "RT-1: a road-deck point 1.03 m up (eases only): a cost, the body passes");
        Check(wall.Clearance(0, 6, 0, 0, k) < 0, "RT-1: the same point that may stop (a wall): blocks the body");
    }
    {   // RP-1 (2026-11-19): a 23 deg ramp (0.42 m per m, the lc_ring_road switchback). The band follows the road under the body: a deck
        // seam just under the road ahead of the nose is not a wall; a rail 1 m over the same road still is; a hovering body stays flat.
        planrun::Frame f; f.now = 10; f.car.halfWidth = 1; f.car.halfLength = 2.3; f.car.height = 2; f.carBase = 0; f.raysLive = true;
        auto line = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = .42 * d; p.yaw = 0; return p; };
        auto wanted = [](double d, double) { planworld::Wanted w; w.have = true; w.measured = true; w.z = .42 * d; return w; };
        std::vector<planworld::Shape> shapes(1);
        shapes[0].source = planworld::Source::Ray; shapes[0].seenAt = 10; shapes[0].haveNormal = true; shapes[0].normalZ = -.92;  // an underside, as logged
        shapes[0].centre = { 0, 2, .42 * 2 - .10 };  // 0.74 m over the car's floor, 0.10 m under the road there
        const auto seam = planinput::Scene(f, line, wanted, shapes.data(), 1);
        shapes[0].centre = { 0, 2, .42 * 2 + 1.0 };   // a rail top 1 m over the road
        const auto rail = planinput::Scene(f, line, wanted, shapes.data(), 1);
        bicycle::Spec k; k.halfWidth = 1; k.halfLength = 2.3; k.height = 2;
        double z = -1;
        Check(seam.Floor(0, 1.0, z) && std::fabs(z - .42) < .02, "RP-1: the lane floor between two samples is the line between them, not a step");
        Check(seam.Clearance(0, 0, 0, 0, k) > 0 && seam.Contact(0, 0, 0, 0, k).clear > 0,
              "RP-1: a seam 0.10 m under the ramp road ahead of the nose (0.74 m over the centre floor) does not block");
        Check(rail.Clearance(0, 0, 0, 0, k) < 0 && rail.Contact(0, 0, 0, 0, k).clear < 0, "RP-1: a rail 1 m over the same ramp road blocks");
        Check(seam.Clearance(0, 0, 0, 0, k, false) < 0, "RP-1: a hovering body (tilt off) keeps the flat band");
        Check(!seam.solids.empty() && std::fabs(seam.solids.front().road - .84) < .02, "RP-1: each solid carries the road height under it");
        {   // The foot of the ramp: the car stands on the flat, the 23 deg ramp starts 1 m ahead. A seam on the ramp road 2.2 m ahead
            // (0.40 m over the car's floor) is road; a 1 m rail there is not.
            auto footLine = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = d > 1 ? .42 * (d - 1) : 0; p.yaw = 0; return p; };
            auto footWanted = [](double d, double) { planworld::Wanted w; w.have = true; w.measured = true; w.z = d > 1 ? .42 * (d - 1) : 0; return w; };
            std::vector<planworld::Shape> foot(1);
            foot[0].source = planworld::Source::Ray; foot[0].seenAt = 10; foot[0].haveNormal = true; foot[0].normalZ = -.92;
            foot[0].centre = { 0, 2.2, .42 * 1.2 - .1 + .5 };  // 0.4 over the road there (a lip), 0.9 over the car's floor
            const auto lip = planinput::Scene(f, footLine, footWanted, foot.data(), 1);
            foot[0].centre = { 0, 2.2, .42 * 1.2 + .05 };
            const auto onRoad = planinput::Scene(f, footLine, footWanted, foot.data(), 1);
            foot[0].centre = { 0, 2.2, .42 * 1.2 + 1.0 };
            const auto footRail = planinput::Scene(f, footLine, footWanted, foot.data(), 1);
            Check(onRoad.Contact(0, 0, 0, 0, k).clear > 0, "RP-1: at the foot of a ramp, a point on the ramp road ahead is not a wall");
            Check(footRail.Contact(0, 0, 0, 0, k).clear < 0 && lip.Contact(0, 0, 0, 0, k).clear < 0,
                  "RP-1: at the foot of a ramp, a 1 m rail and a 0.4 m lip on the ramp still block");
        }
        {   // Another level of a switchback: a floor 4 m above the car's within 2 m is not this road; the flat floor judges it.
            planscene::Scene stack; stack.floor = [](double, double y, double& zz) { zz = y > 1.5 ? 4.0 : 0.0; return true; };
            stack.solids.push_back({0, 2, 3.9, 3.9, .2}); stack.Index();
            Check(!std::isfinite(stack.RoadRef(stack.solids.front().road, 0, 2, 0, true)) || stack.RoadRef(stack.solids.front().road, 0, 2, 0, true) == 0,
                  "RP-1: a floor of another level is not used as this road (the flat floor stands)");
        }
        planscene::Scene level; level.floor = [](double, double, double& zz) { zz = 0; return true; };
        level.solids.push_back({0, 2, .6, .6, .2}); level.Index();
        Check(level.Clearance(0, 0, 0, 0, k) < 0, "RP-1: on flat ground a 0.6 m point in the body still blocks (unchanged)");
    }

    {   // RF-1 (2026-11-19): World's fitted road under a hit decides, with its pattern. A banked road (lane centre flat at 0): a hit
        // 0.7 m over the centre floor but 0.1 m over World's road there is road; the same hit without World's road blocks; a tooth
        // 0.5 m over a fitted road blocks; a 0.15 m edge inside a 0.1 m pattern is road, not even a low edge.
        planscene::Scene sc; sc.floor = [](double, double, double& zz) { zz = 0; return true; };
        bicycle::Spec k; k.halfWidth = 1; k.halfLength = 2.3; k.height = 2;
        planscene::Solid bank{0, 2, .7, .7, .2}; bank.road = .6; bank.roadSpread = .03; sc.solids.push_back(bank); sc.Index();
        Check(sc.Contact(0, 0, 0, 0, k).clear > 0, "RF-1: on a bank, a hit 0.1 m over World's road is road");
        planscene::Scene bare; bare.floor = sc.floor; bare.solids.push_back({0, 2, .7, .7, .2}); bare.Index();
        Check(bare.Contact(0, 0, 0, 0, k).clear < 0, "RF-1: the same hit with no World road (lane floor 0) still blocks");
        planscene::Scene tooth; tooth.floor = sc.floor; planscene::Solid t{0, 2, .5, .5, .2}; t.road = 0; t.roadSpread = .1; tooth.solids.push_back(t); tooth.Index();
        Check(tooth.Contact(0, 0, 0, 0, k).clear < 0, "RF-1: a tooth 0.5 m over the fitted road blocks (outside the pattern, over a wheel's climb)");
        planscene::Scene seam; seam.floor = sc.floor; planscene::Solid e{0, 1, .15, .15, .2}; e.road = 0; e.roadSpread = .1; seam.solids.push_back(e); seam.Index();
        const auto pe = seam.Contact(0, 0, 0, 0, k);
        Check(pe.clear > 0 && pe.low == 0, "RF-1: a 0.15 m edge inside a 0.1 m pattern is the road's own pattern, not a low edge");
    }

    {   // PS-1 + CC-1 (2026-11-19): a measured point is a point; the followed line keeps 0.5 m from a measured edge when the road has room.
        planrun::Frame f; f.now = 10; f.car.halfWidth = 1.24; f.car.halfLength = 2.3; f.car.height = 1.5; f.carBase = 0; f.raysLive = true;
        auto line = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = 0; p.yaw = 0; return p; };
        auto wanted = [](double, double) { planworld::Wanted w; w.have = true; w.measured = true; w.z = 0; return w; };
        bicycle::Spec k; k.halfWidth = 1.24; k.halfLength = 2.3; k.height = 1.5;
        auto StationOf = [](const planscene::Scene& sc) { double st, lat, half; sc.Route(0, 0, st, lat, half); return st; };
        auto barrier = [](std::vector<planworld::Shape>& v, double x) {
            for (int i = 0; i < 14; ++i) { planworld::Shape s; s.source = planworld::Source::Ray; s.seenAt = 10; s.centre = { x, 1.0 + i, .9 }; v.push_back(s); } };
        {   std::vector<planworld::Shape> one; barrier(one, 1.40);  // a barrier 0.16 m from the body's right side
            auto sc = planinput::Scene(f, line, wanted, one.data(), int(one.size()));
            Check(sc.Clearance(0, 5, 0, 0, k) > 0, "PS-1: a barrier 0.16 m beside the body is clear (a 0.4 m square round each point overlapped it)");
            const auto e = corridor::Measure(sc, StationOf(sc), 0, 0, 0, k);
            Check(e.target < -.25 && e.target > -.45 && std::strcmp(e.why, "edge_right") == 0, "CC-1: a barrier close on the right moves the followed line left until 0.5 m clear");
            corridor::ShiftRoute(sc, e.target);
            double st, lat, half; sc.Route(0, 5, st, lat, half);
            Check(std::fabs(lat + e.target) < .02 && std::fabs(st - StationOf(sc) - 5) < .05, "CC-1: the moved line puts the car on its right side by the shift; stations keep");
        }
        {   std::vector<planworld::Shape> two; barrier(two, 1.6); barrier(two, -1.6);
            auto sc = planinput::Scene(f, line, wanted, two.data(), int(two.size()));
            const auto e = corridor::Measure(sc, StationOf(sc), 0, 0, 0, k);
            Check(std::fabs(e.target) < .01 && std::strcmp(e.why, "narrow_midway") == 0, "CC-1: too narrow for the margin on both sides: midway between the edges");
        }
        {   std::vector<planworld::Shape> wide; barrier(wide, 3.0); barrier(wide, -3.0);
            auto sc = planinput::Scene(f, line, wanted, wide.data(), int(wide.size()));
            Check(corridor::Measure(sc, StationOf(sc), 0, 0, 0, k).target == 0, "CC-1: room on both sides: the line stays");
        }
        {   std::vector<planworld::Shape> low; barrier(low, 1.40); for (auto& s : low) s.centre.z = .2;  // a kerb a wheel rolls over
            auto sc = planinput::Scene(f, line, wanted, low.data(), int(low.size()));
            Check(corridor::Measure(sc, StationOf(sc), 0, 0, 0, k).target == 0, "CC-1: an edge under a wheel's climb is not a corridor edge");
        }
        {   std::vector<planworld::Shape> mid(1); mid[0].source = planworld::Source::Ray; mid[0].seenAt = 10; mid[0].centre = { .3, 6, .9 };
            auto sc = planinput::Scene(f, line, wanted, mid.data(), 1);
            Check(corridor::Measure(sc, StationOf(sc), 0, 0, 0, k).target == 0, "CC-1: an obstacle in the lane's middle is Avoid's, not an edge");
        }
        double eased = 0; for (int i = 0; i < 4; ++i) eased = corridor::Ease(eased, -.34, .1);
        Check(std::fabs(eased + .24) < .01 && corridor::Ease(-.34, -.30, .1) == -.34, "CC-1: the shift eases at 0.6 m/s and holds against small changes");
    }
    {   // CC-2 (2026-11-19, 13:57:20-13:57:50): remembered edges. A straight lane along +y; a right wall whose face is 0.84 m from the line.
        bicycle::Spec k; k.halfWidth = 1.0; k.halfLength = 2.3; k.height = 1.5;
        auto make = [](bool wall, bool mid = false) {
            planscene::Scene sc; sc.floor = [](double, double, double& z) { z = 0; return true; };
            for (int i = -20; i <= 120; ++i) sc.route.push_back({0, double(i), double(i + 20)});
            if (wall) for (int i = 0; i < 120; ++i) sc.solids.push_back({.89, i * .5, .9, .9, .05});
            if (mid) sc.solids.push_back({.3, 6, .9, .9, .05});
            sc.Index(); return sc;
        };
        const auto withWall = make(true), empty = make(false);
        auto at = [](const planscene::Scene& sc, double x, double y) { double st, lat, half; sc.Route(x, y, st, lat, half); return st; };
        const double want = corridor::Measure(withWall, at(withWall, 0, 0), 0, 0, 0, k).target;  // 0.84 - (1.0 + 0.5)
        Check(std::fabs(want + .66) < .01, "CC-2: the wall 0.84 m from the line asks the line 0.66 m left");
        {   corridor::Memory m;
            corridor::Measure(withWall, at(withWall, 0, 0), 0, 0, 0, k, &m, 100.0);
            double held = 0; int remembered = 0;
            for (int i = 1; i <= 50; ++i) { const auto e = corridor::Measure(empty, at(empty, 0, 0), 0, 0, 0, k, &m, 100.0 + i * .1); held = e.target; remembered = e.remembered; }
            Check(std::fabs(held - want) < 1e-9 && remembered > 0, "CC-2: an edge seen once, then not seen for 5 s, still holds the target");
            Check(corridor::Measure(empty, at(empty, 0, 0), 0, 0, 0, k).target == 0, "CC-2: (the same empty scene with no memory reads clear)");
            const auto passed = corridor::Measure(empty, at(empty, 0, 70), 0, 70, 0, k, &m, 106.0);
            Check(passed.target == 0 && passed.memory == 0, "CC-2: edges the car has passed are forgotten");
        }
        {   corridor::Memory m;
            corridor::Measure(withWall, at(withWall, 0, 0), 0, 0, 0, k, &m, 100.0);
            const int full = int(m.points.size());
            const auto old = corridor::Measure(empty, at(empty, 0, 0), 0, 0, 0, k, &m, 131.0);
            Check(full > 0 && full <= corridor::rules::kept && old.memory == 0 && old.target == 0, "CC-2: edges not seen again for 30 s are forgotten");
            planscene::Scene stub = make(false);  // a wall stub beside the car (within RoadRef's grade reach of a 4 m step: 3 m)
            for (int i = 0; i <= 6; ++i) stub.solids.push_back({.89, i * .5, .9, .9, .05});
            stub.Index();
            corridor::Measure(stub, at(stub, 0, 0), 0, 0, 0, k, &m, 200.0);
            const int before = int(m.points.size());
            const auto level = corridor::Measure(empty, at(empty, 0, 0), 0, 0, 4.0, k, &m, 201.0);
            Check(before > 0 && level.memory == 0 && level.target == 0, "CC-2: a car 4 m higher (another level of a switchback) forgets the edges below");
        }
        {   // The 13:57 sequence: the car drives at 1 m/s on the followed line; the wall is seen for 2 s, unseen for 2 s, for 20 s.
            auto run = [&](bool remember, bool& monotone, double& last) {
                corridor::Memory m; double shift = 0; monotone = true;
                for (int i = 0; i <= 200; ++i) {
                    const double t = i * .1, y = t;
                    const bool seen = int(t / 2.0) % 2 == 0;
                    const auto& sc = seen ? withWall : empty;
                    const auto e = corridor::Measure(sc, at(sc, shift, y), shift, y, 0, k, remember ? &m : nullptr, 50.0 + t);
                    const double next = corridor::Ease(shift, e.target, .1);
                    if (next > shift + 1e-9) monotone = false;
                    shift = next;
                }
                last = shift;
            };
            bool mono = false, monoOld = true; double last = 0, lastOld = 0;
            run(true, mono, last); run(false, monoOld, lastOld);
            Check(!monoOld, "CC-2: (without memory the flickering wall swings the shift, as logged)");
            Check(mono && std::fabs(last - want) < corridor::rules::hold, "CC-2: with memory the shift moves one way to the margin target and stays");
        }
        {   corridor::Memory m;
            const auto mid = make(false, true);
            const auto e = corridor::Measure(mid, at(mid, 0, 0), 0, 0, 0, k, &m, 10.0);
            Check(e.target == 0 && e.solids == 0 && m.points.empty(), "CC-2: an obstacle in the lane's middle is not an edge and is not remembered");
        }
        {   corridor::Memory m; planscene::Scene dense = make(false);
            for (int i = 0; i < 80; ++i) for (int j = 0; j < 40; ++j) dense.solids.push_back({1.2 + j * .25, i * .25, .9, .9, .05});
            dense.Index();
            corridor::Measure(dense, at(dense, 0, 0), 0, 0, 0, k, &m, 10.0);
            bool unique = true;
            for (std::size_t a = 0; a < m.points.size(); ++a) for (std::size_t b = a + 1; b < m.points.size(); ++b)
                if (std::floor(m.points[a].x / .5) == std::floor(m.points[b].x / .5) && std::floor(m.points[a].y / .5) == std::floor(m.points[b].y / .5)) unique = false;
            Check(int(m.points.size()) == corridor::rules::kept && unique, "CC-2: the memory is bounded (256) and keeps one point per 0.5 m cell");
        }
    }
    {   // F (2026-11-19): the up-facing road filter judges against World's fitted road where it has one, else the lane-line floor (0 here).
        planrun::Frame f; f.now = 10; f.car.halfWidth = 1; f.car.halfLength = 2.3; f.car.height = 2; f.carBase = 0; f.raysLive = true;
        auto line = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = 0; p.yaw = 0; return p; };
        auto wanted = [](double, double) { planworld::Wanted w; w.have = true; w.measured = true; w.z = 0; return w; };
        std::vector<planworld::Shape> shapes(1);
        shapes[0].source = planworld::Source::Ray; shapes[0].seenAt = 10; shapes[0].haveNormal = true; shapes[0].normalZ = .95;
        shapes[0].centre = { 2, 6, .8 };  // 0.8 m over the lane floor, 0.2 m over World's banked road
        shapes[0].road = .6;
        Check(planinput::Scene(f, line, wanted, shapes.data(), 1).solids.empty(), "F: an up-facing hit 0.2 m over World's fitted road is road");
        shapes[0].road = std::numeric_limits<double>::quiet_NaN();
        Check(!planinput::Scene(f, line, wanted, shapes.data(), 1).solids.empty(), "F: the same hit with no fitted road is 0.8 m over the lane floor: kept");
        shapes[0].centre.z = .3; shapes[0].road = -.5;  // a dip: the lane floor is 0, World's road 0.5 m lower
        Check(!planinput::Scene(f, line, wanted, shapes.data(), 1).solids.empty(), "F: an up-facing hit 0.8 m over World's road in a dip is kept");
        shapes[0].road = std::numeric_limits<double>::quiet_NaN();
        Check(planinput::Scene(f, line, wanted, shapes.data(), 1).solids.empty(), "F: with no fitted road the lane floor stands (0.3 m over it: road)");
    }
    {   // SG-1 (2026-11-19): the grade cap ahead, SlopeLimit brought back by braking (downhill brakes weaker by g*sin).
        using planspeed::GradeCap; using planspeed::SlopeLimit;
        Check(std::fabs(GradeCap(16, .42, 0, 3) - SlopeLimit(16, .42)) < 1e-9 && std::fabs(SlopeLimit(16, .42) - 8) < 1e-9,
              "SG-1: on the 23 deg ramp itself the cap is SlopeLimit (half of 16 m/s)");
        Check(std::fabs(GradeCap(16, .42, 20, 3) - std::sqrt(64 + 120)) < 1e-9, "SG-1: an uphill 20 m ahead: reachable by braking at 3 m/s^2");
        Check(GradeCap(16, -.42, 20, 3) < GradeCap(16, .42, 20, 3) && std::fabs(GradeCap(16, -.42, 20, 3) - std::sqrt(64 + 20)) < 1e-9,
              "SG-1: the same slope downhill: weaker braking (floor 0.5 m/s^2), lower cap");
        Check(GradeCap(16, 0, 0, 3) == 16 && GradeCap(16, .02, 0, 3) > 15, "SG-1: flat or gentle road: no cap below the top speed");
    }

    {   // GR-2 (2026-11-19 17:44 run): the lc_ring_road ramp's lane floor at the stop, one sample 0.45 m under the car (seam or lower surface).
        struct Ramp { double ys[16], zs[16]; int n = 0;
            bool Floor(double, double y, double& z) const { if (y <= ys[0]) { z = zs[0] + (y - ys[0]) * .42; return true; }
                for (int i = 0; i + 1 < n; ++i) if (y <= ys[i + 1]) { const double t = (y - ys[i]) / (ys[i + 1] - ys[i]); z = zs[i] + t * (zs[i + 1] - zs[i]); return true; }
                z = zs[n - 1]; return true; } } ramp;
        const double logged[] = { -106.20, -104.57, -103.88, -103.09, -102.25, -101.52, -100.73, -99.93, -99.14, -98.35 };
        for (int i = 0; i < 10; ++i) { ramp.ys[i] = i * 1.86; ramp.zs[i] = logged[i]; } ramp.n = 10;
        // behind the car the ramp continues (not logged): 0.42 down from the second sample's line
        bicycle::Spec k; k.halfLength = 1.89; k.halfWidth = .79;
        bicycle::State car; car.x = 0; car.y = 0; car.heading = 0; car.mode = 0;
        const auto g = energyplan::FitGrade(ramp, car, k);
        Check(g.ok, "GR-2: the grade fit reads the raw logged floor");
        {   // the same samples through the lane scene: the sample under the car is set aside, the ramp reads 0.42 under and behind the car
            planrun::Frame f; f.now = 10; f.car.halfWidth = .79; f.car.halfLength = 1.89; f.car.height = 1.3; f.carBase = -105.75; f.raysLive = true;
            auto line = [](double d) { planrun::LinePoint p; p.x = 0; p.y = d; p.z = 0; p.yaw = 0; return p; };
            auto wanted = [&](double d, double) { planworld::Wanted w; const int i = int(std::lround(d / planrun::kStep));
                w.have = w.measured = i >= 0 && i < 10; w.z = w.have ? logged[i] : 0; return w; };
            const auto sc = planinput::Scene(f, line, wanted, nullptr, 0);
            bicycle::State at; at.z = f.carBase;
            const auto gs = energyplan::FitGrade(sc, at, k);
            const auto raw = energyplan::FitGrade(sc, at, k, false);
            std::printf("   17:44 replay grade raw=%.3f corrected=%.3f limit=%.3f\n", raw.grade, gs.grade, carcap::kCar.MaxGrade(1.0));
            Check(raw.grade > carcap::kCar.MaxGrade(1.0), "GR-2: reproduces above-limit grade with actual lane-scene extension");
            double physical = 0;
            Check(sc.Floor(0, 0, physical) && std::fabs(physical - logged[0]) < 1e-6,
                  "GR-2: repaired grade does not alter collision floor");
            Check(sc.floorSetAside == 1 && std::fabs(gs.grade - .42) < .08,
                  "GR-2: the 17:44 lane floor sets the sample 0.45 m under the car aside and reads the 0.42 ramp (was 0.88: too steep)");
            Check(std::fabs(gs.grade) < carcap::kCar.MaxGrade(1.0), "GR-2: so the ramp is not too steep for wheels");
            auto gradePlan = [&](const planscene::Scene& scene) {
                energyplan::Memory memory;
                return energyplan::Plan(at, k, 3, scene, memory, 10);
            };
            const auto accepted = gradePlan(sc);
            Check(accepted.ok && accepted.first.speed > .1, "GR-2: the logged ramp yields a forward plan");
            f.carBase = logged[0];
            const auto lowDeck = planinput::Scene(f, line, wanted, nullptr, 0);
            Check(lowDeck.floorSetAside == 0, "GR-2: preserve the starting sample when the physical car base agrees with it");
            auto trueSteep = sc;
            trueSteep.floor = [](double, double y, double& z) { z = .90 * y; return true; };
            trueSteep.gradeFloor = {};
            const auto blocked = gradePlan(trueSteep);
            Check(blocked.ok && (blocked.terms.clearanceTier > 0 || blocked.first.speed < 0),
                  "weighted grade penalizes an above-model climb while selecting motion");
        }
        struct Steep { bool Floor(double, double y, double& z) const { z = .9 * y; return true; } } steep;
        Check(energyplan::FitGrade(steep, car, k).grade > carcap::kCar.MaxGrade(1.0), "GR-2: a real 0.9 slope over the car's length is still too steep");
        struct Seams { bool Floor(double, double y, double& z) const { const double piece = std::floor(y / 2.0); z = .42 * y - .1 * (piece - std::floor(piece / 2) * 2); return true; } } seams;
        Check(std::fabs(energyplan::FitGrade(seams, car, k).grade - .42) < .08, "GR-2: a sawtooth deck (0.1 m steps every 2 m) fits as its 0.42 ramp");
    }
    {
        planscene::Scene scene;
        scene.continuousRecovery = true;
        scene.floor = [](double, double, double& z) { z = 0; return true; };
        for (int i = -20; i <= 30; ++i) scene.route.push_back({0, double(i)});
        scene.solids.push_back({0, 3.0, 0, 1.5, .25});
        scene.Index();
        bicycle::Spec spec;
        bicycle::State current;
        energyplan::Memory memory;
        bicycle::State prior;
        for (int i = -7; i <= -1; ++i) {
            prior.y = i;
            memory.trail.Note(prior, i == -5 ? 7u : 2u, i == -5, 10 + (i + 7)*.15);
        }
        memory.recSeen = 11;
        memory.rec.mode = recovery::Mode::Recover;
        memory.rec.step = recovery::Command::Step::BackUp;
        Check(memory.trail.FindTarget(current) == 2, "route recovery finds the earlier three-option swept checkpoint");
        auto blockedBack = memory;
        auto held = memory;
        auto r = energyplan::Plan(current, spec, 6, scene, memory, 11.2);
        Check(r.ok && r.first.speed < -.1 && std::strcmp(r.step, "back_up") == 0 && std::strcmp(r.why, "recover: back up") == 0 &&
              r.traj[energyplan::kSteps].y < -2, "AP-1: in traffic the routine's back-up step is the one plan (retrace is off)");
        scene.solids.push_back({0, -3.0, 0, 1.5, .25});
        scene.Index();
        r = energyplan::Plan(current, spec, 6, scene, blockedBack, 11.2);
        Check(r.ok && blockedBack.recStepBlocked && std::strcmp(r.why, "recover: back up") != 0,
              "AP-1: a blocked back-up step is told to the routine; the candidates choose this tick");
        scene.managerHeld = true;
        scene.stopStation = 0;
        r = energyplan::Plan(current, spec, 0, scene, held, 11.2);
        Check(std::strcmp(r.step, "back_up") != 0 && std::strcmp(r.step, "retrace") != 0, "Traffic hold takes priority over the routine");
    }
    {
        Check(energyplan::WeightedEnergy(2, 1.e100) < energyplan::WeightedEnergy(3, -1.e100) &&
              energyplan::WeightedEnergy(3, 10.) < energyplan::WeightedEnergy(3, 11.),
              "a clearance tier dominates all other weights and equal tiers use preference");
        planscene::Scene scene;
        scene.continuousRecovery = true;
        scene.floor = [](double, double, double& z) { z = 0; return true; };
        for (int i = -20; i <= 30; ++i) scene.route.push_back({0, double(i)});
        const bicycle::Spec spec;
        const bicycle::State car;
        const double edge = spec.halfLength + .29;
        scene.solids.push_back({0, edge, 0, 1.5, .25});
        scene.solids.push_back({0, -edge, 0, 1.5, .25});
        scene.Index();
        energyplan::Memory memory;
        const auto boxed = energyplan::Plan(car, spec, 4, scene, memory, 2);
        Check(boxed.ok && std::fabs(boxed.first.speed) > .1 && boxed.terms.clearanceTier >= 10,
              "when every available movement intersects a solid, Plan ranks and chooses a movement");
        Check(boxed.terms.wheelSpeed == 0 && std::strcmp(boxed.terms.wheelReason, "wheel_path_blocked") == 0 &&
              std::fabs(boxed.first.speed) > .1, "a zero wheel vote remains a score and cannot cancel the chosen command");
        scene.solids.pop_back();
        scene.Index();
        const auto retreat = energyplan::Plan(car, spec, 4, scene, memory, 2.1);
        Check(retreat.ok && retreat.first.speed < -.1 && retreat.terms.clearanceTier == 0 &&
              retreat.terms.wheelSpeed >= std::fabs(retreat.first.speed),
              "a clear escape beats the committed obstructed move immediately");
        scene.managerHeld = true;
        scene.stopStation = 0;
        memory = {};
        const auto hold = energyplan::Plan(car, spec, 0, scene, memory, 2.2);
        Check(std::fabs(hold.first.speed) <= .1, "a Traffic hold still authorizes a stop");
    }
    {
        constexpr railforce::V3 ground{0, -.4, 1};
        const auto push = railforce::TyreAssist({}, {0, 3, 0}, ground, .1);
        Check(push.y > 0 && push.z > 0 && std::fabs(railforce::Dot(push, ground)) < 1e-9 &&
              railforce::Len(push) <= .06501, "rail assist adds bounded traction along a ramp");
        Check(railforce::TyreAssist({}, {0, -1, 0}, ground, .1).y < 0 &&
              railforce::Len(railforce::TyreAssist({0, 2, 0}, {0, 3, 0}, ground, .1)) == 0 &&
              railforce::Len(railforce::TyreAssist({}, {}, ground, .1)) == 0,
              "rail assist supports reverse, yields above its speed band, and stays idle with zero intent");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
