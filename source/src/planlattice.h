#pragma once
// Redesign (2026-11-12): the cost to go. The energy planner's rollouts see 6 s; a turn that needs reversing first looks worse inside 6 s
// than pushing forward, so without a view past the horizon it rocks (energyplan_test hairpin, first draft: 24 gear changes, no progress).
// This field gives, for every position, heading and gear near the car, the least cost to reach the goal under THIS car's turning
// (bicycle.h primitives forward and reverse, a cost per gear change), around every solid in the scene. The planner adds it at each
// rollout's end, so a reverse is chosen exactly when it shortens the real way round, and rocking can never lower the energy.
// Backward Dijkstra over a grid (cells x heading bins x gear); collision from the same scene test. Pure; tests: tests/energyplan_test.cpp.
#include "bicycle.h"
#include "planscene.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>
#include <vector>

namespace planlattice {
constexpr int kHeadings = 48;
constexpr double kPi = 3.14159265358979323846;
struct Params {
    double cell = .5;          // m
    double size = 56.0;        // m, square window
    double step = 1.35;         // m per primitive
    double reverseFactor = 1.6, gearCost = 4.0, steerCost = .05;  // reverse metres cost more; changing gear costs metres
    double clearance = .08;     // m a pose must keep from solids to count as free
    double beyondCar = 60.0;    // step 2: stop the search this far (metre-equivalents) past the car's own cost to go (a 6 s rollout ends within about 50 of it)
    double fieldOut = 2.0;      // FC-1: extra metre-equivalents per metre travelled per m^2 outside the field boxes (the energy's field term)
    double laneOut = 2.0;       // LK-1: extra metre-equivalents per metre travelled per metre of the body past its own lane's edge (lane scenes)
};
struct Field {
    bool ready = false;
    double x0 = 0, y0 = 0; int n = 0; Params p; double goalStation = 0;
    std::vector<float> cost;   // [((ix * n + iy) * kHeadings + h) * 2 + gear], gear 0 forward 1 reverse
    std::vector<std::uint8_t> free;  // [(ix * n + iy) * kHeadings + h]
    static int Bin(double heading) { const double t = std::remainder(heading, 2 * kPi); int b = int(std::lround(t / (2 * kPi) * kHeadings)); return ((b % kHeadings) + kHeadings) % kHeadings; }
    static double Angle(int b) { return b * 2 * kPi / kHeadings; }
    bool Cell(double x, double y, int& ix, int& iy) const { ix = int(std::floor((x - x0) / p.cell)); iy = int(std::floor((y - y0) / p.cell)); return ix >= 0 && iy >= 0 && ix < n && iy < n; }
    std::size_t Index(int ix, int iy, int h, int g) const { return ((std::size_t(ix) * n + iy) * kHeadings + h) * 2 + g; }
    // Cost to go from a pose; +inf outside the window or where no way was found.
    double At(double x, double y, double heading, double speed) const {
        int ix, iy; if (!ready || !Cell(x, y, ix, iy)) return std::numeric_limits<double>::infinity();
        const int h = Bin(heading), g = speed < -.05 ? 1 : 0;
        const float c = std::min(cost[Index(ix, iy, h, g)], cost[Index(ix, iy, h, 1 - g)] + float(p.gearCost));
        return c;
    }
    // LK-1: the least cost to go from (x, y) at any heading, for a car that turns on the spot (hovering): the wheels' turning is not its way.
    double AtAnyHeading(double x, double y) const {
        int ix, iy; if (!ready || !Cell(x, y, ix, iy)) return std::numeric_limits<double>::infinity();
        float best = std::numeric_limits<float>::infinity();
        for (int h = 0; h < kHeadings; ++h) for (int g = 0; g < 2; ++g) best = std::min(best, cost[Index(ix, iy, h, g)]);
        return best;
    }
};

// One primitive from a pose: steering input `in`, direction `dir`, length p.step (4 sub-steps through the car's own geometry).
inline void Move(double& x, double& y, double& hd, double in, int dir, const bicycle::Spec& k, double len) {
    const double angle = carmodel::WheelAngle(k.car, in, 0.0), curv = std::tan(angle) / std::max(1.0, k.car.wheelbase);
    for (int i = 0; i < 4; ++i) { const double ds = dir * len / 4; hd += ds * curv * .5; x += ds * std::sin(hd); y += ds * std::cos(hd); hd += ds * curv * .5; }
}

// Build the field for a window centred between `cx, cy` and the goal. Goal: route station >= goalStation, within the lane, heading within
// 50 deg of the route's direction there.
// carX/carY (step 2): the car's position. When given, the search stops once the costs it settles are `p.beyondCar` past the car's own: a
// state that much worse than standing still never wins a rollout, so no decision changes; such states read as unknown (+inf).
inline Field Build(const planscene::Scene& sc, const bicycle::Spec& k, double cx, double cy, double goalStation, double hintStation, const Params& p = {},
                   double carX = std::numeric_limits<double>::quiet_NaN(), double carY = std::numeric_limits<double>::quiet_NaN()) {
    Field f; f.p = p; f.n = int(p.size / p.cell); f.x0 = cx - p.size * .5; f.y0 = cy - p.size * .5; f.goalStation = goalStation;
    const int n = f.n;
    f.cost.assign(std::size_t(n) * n * kHeadings * 2, std::numeric_limits<float>::infinity());
    f.free.assign(std::size_t(n) * n * kHeadings, 0);
    planscene::Scene probe = sc; probe.reach = p.clearance + .05;  // only "free or not" is needed here
    std::vector<std::uint8_t> goal(std::size_t(n) * n * kHeadings, 0);
    std::vector<float> outside(std::size_t(n) * n, 0.f);
    std::vector<float> fieldCost(std::size_t(n) * n, 0.f);  // FC-1: (m outside the field boxes)^2 at each cell  // LK-1: m of the body past the own lane's edge at each cell (lane scenes)
    std::vector<float> seed(std::size_t(n) * n, 0.f);  // D13: a far goal's mark cells start at their remaining way to the goal (pull past the mark)
    const double wayTotal = sc.goal.have && !sc.route.empty() ? sc.route.back().station : 0;
    for (int ix = 0; ix < n; ++ix) for (int iy = 0; iy < n; ++iy) {
        const double x = f.x0 + (ix + .5) * p.cell, y = f.y0 + (iy + .5) * p.cell;
        double z = 0; sc.Floor(x, y, z);
        double st, lat, half, routeYaw = 0, distance;
        sc.Route(x, y, st, lat, half, hintStation, &routeYaw, &distance);
        // D13 (step 4): a goal scene's goal is the goal region (within its radius; the arrive heading if one is given) when the goal is
        // within reach, else every cell past the sub-goal mark heading on (within 90 deg of the way): never a point that may lie in a solid.
        const bool goalScene = sc.goal.have;
        if (!goalScene && sc.lanes && !sc.LaneEdgeFree(st)) outside[std::size_t(ix) * n + iy] = float(std::max(0.0, distance + k.halfWidth - half));  // D1: not beside a static block in the lane
        // FC-1 (13:56 crest, plan_sim: the field led out of the authored field boxes, the energy's field term rose to 12, and the car crawled and
        // reversed 12 times): the field's moves pay what the energy pays outside the boxes, so the two agree on the way.
        { double fo = sc.FieldOutside(x, y); if (!goalScene && sc.lanes && fo > 0) fo = std::min(fo, std::max(0.0, distance - half));  // in a box or in the lane
          fieldCost[std::size_t(ix) * n + iy] = float(fo * fo); }
        // LK-1: a lane scene's goal is where the whole body is inside the lane (the centre within the half width less the body's).
        const bool goalCell = !goalScene ? st >= goalStation && distance <= std::max(.5, half - k.halfWidth)
                            : sc.goalFinal ? std::hypot(x - sc.goal.x, y - sc.goal.y) <= sc.goal.radius : st >= goalStation;
        const double goalYaw = goalScene && sc.goalFinal ? sc.goal.arriveYaw : routeYaw;
        // D13: a far goal's mark cells start at their remaining way. (LF-1 tried this for lane scenes too: it bent the field across a hairpin's
        // inner barrier, 12 gear changes; lane scenes get the same pull at the rollout's end instead, energyplan Score.)
        if (goalScene && !sc.goalFinal && goalCell) seed[std::size_t(ix) * n + iy] = float(std::max(0.0, wayTotal - st));
        const double goalCone = !goalScene ? 50 * kPi / 180 : sc.goalFinal ? (std::isfinite(goalYaw) ? 50 * kPi / 180 : 4.0) : 90 * kPi / 180;
        // Step 2: a cell with no measured solid within the body's reach is free at every heading (exact: Clearance could only find one in
        // that radius); only cells near a solid check their 48 headings.
        bool anyNear = false;
        sc.Near(x, y, sc.SearchRadius(k, p.clearance + .05), [&](const planscene::Solid& so) { if (!so.soft) anyNear = true; });
        for (int h = 0; h < kHeadings; ++h) {
            const std::size_t i = (std::size_t(ix) * n + iy) * kHeadings + h;
            f.free[i] = !anyNear || probe.Clearance(x, y, Field::Angle(h), z, k) >= p.clearance ? 1 : 0;
            if (goalCell && f.free[i] && (goalCone > kPi || std::fabs(std::remainder(Field::Angle(h) - goalYaw, 2 * kPi)) < goalCone)) goal[i] = 1;
        }
    }
    // Step 2: a bucket queue (Dial) instead of a binary heap. Every move costs at least p.step (1.35), so buckets 1.0 wide are exact: a state
    // popped from the lowest bucket can only push states into later buckets, so each state is final when popped, as with the heap.
    const double bucketWidth = 1.0;
    std::vector<std::vector<std::uint32_t>> buckets(1);
    auto push = [&](float c, std::size_t id) { const std::size_t b = std::size_t(c / bucketWidth); if (b >= buckets.size()) buckets.resize(b + 1); buckets[b].push_back(std::uint32_t(id)); };
    for (std::size_t i = 0; i < goal.size(); ++i) if (goal[i]) for (int g = 0; g < 2; ++g) { const float c0 = seed[i / kHeadings]; f.cost[i * 2 + g] = c0; push(c0, i * 2 + g); }
    int carIx = -1, carIy = -1; const bool haveCar = std::isfinite(carX) && std::isfinite(carY) && f.Cell(carX, carY, carIx, carIy);
    double stopAt = std::numeric_limits<double>::infinity();
    static constexpr double inputs[] = { -1, -.5, 0, .5, 1 };
    // Step 2: every primitive, run backwards from a cell centre at each heading, precomputed once: the cell offsets and heading bins of its
    // half-way and end points. A primitive always starts from a cell centre, so these are exactly what Move gave per expansion.
    struct Prim { int mdx, mdy, mh, edx, edy, eh; };
    static thread_local std::vector<Prim> table; static thread_local double tableKey[4] = { -1, -1, -1, -1 };
    const double key[4] = { k.car.lock, k.car.wheelbase, p.step, p.cell };
    if (std::memcmp(key, tableKey, sizeof key) != 0) {
        table.assign(std::size_t(kHeadings) * 5 * 2, {});
        for (int h = 0; h < kHeadings; ++h) for (int ii = 0; ii < 5; ++ii) for (int g = 0; g < 2; ++g) {
            const int d = g ? -1 : 1; double x = 0, y = 0, hd = Field::Angle(h);
            Move(x, y, hd, inputs[ii], -d, k, p.step * .5); Prim pr;
            pr.mdx = int(std::floor(x / p.cell + .5)); pr.mdy = int(std::floor(y / p.cell + .5)); pr.mh = Field::Bin(hd);
            Move(x, y, hd, inputs[ii], -d, k, p.step * .5);
            pr.edx = int(std::floor(x / p.cell + .5)); pr.edy = int(std::floor(y / p.cell + .5)); pr.eh = Field::Bin(hd);
            table[(std::size_t(h) * 5 + ii) * 2 + g] = pr;
        }
        std::memcpy(tableKey, key, sizeof key);
    }
    for (std::size_t bi = 0; bi < buckets.size(); ++bi) {
      if (bi * bucketWidth > stopAt) break;
      for (std::size_t q = 0; q < buckets[bi].size(); ++q) {
        const std::size_t id = buckets[bi][q]; const float c = f.cost[id];
        if (std::size_t(c / bucketWidth) != bi) continue;  // a later, cheaper push of this state is in an earlier bucket (already handled)
        if (haveCar && !std::isfinite(stopAt)) { const std::size_t ci = id / 2 / kHeadings; if (int(ci / n) == carIx && int(ci % n) == carIy) stopAt = c + p.beyondCar; }
        const int g = int(id % 2); const std::size_t cellH = id / 2; const int h = int(cellH % kHeadings); const std::size_t cellI = cellH / kHeadings;
        const int ix = int(cellI / n), iy = int(cellI % n);
        // State (cell, h, gear g) was reached by a primitive driven in direction d = g ? reverse : forward. Its predecessors are found by
        // running that primitive backwards in time from here.
        const int d = g ? -1 : 1;
        for (int ii = 0; ii < 5; ++ii) {
            const double in = inputs[ii]; const Prim& pr = table[(std::size_t(h) * 5 + ii) * 2 + g];
            const int mx = ix + pr.mdx, my = iy + pr.mdy;
            if (mx < 0 || my < 0 || mx >= n || my >= n || !f.free[(std::size_t(mx) * n + my) * kHeadings + pr.mh]) continue;
            const int px = ix + pr.edx, py = iy + pr.edy;
            if (px < 0 || py < 0 || px >= n || py >= n) continue;
            const int ph = pr.eh;
            if (!f.free[(std::size_t(px) * n + py) * kHeadings + ph]) continue;
            const double base = p.step * (d < 0 ? p.reverseFactor : 1.0) + p.steerCost * std::fabs(in)
                              + p.laneOut * p.step * .5 * (outside[std::size_t(mx) * n + my] + outside[std::size_t(px) * n + py])  // LK-1
                              + p.fieldOut * p.step * .5 * (fieldCost[std::size_t(mx) * n + my] + fieldCost[std::size_t(px) * n + py]);  // FC-1
            for (int pg = 0; pg < 2; ++pg) {  // the predecessor's gear; a change costs gearCost
                const float nc = c + float(base + (pg != g ? p.gearCost : 0));
                const std::size_t pid = f.Index(px, py, ph, pg);
                if (nc < f.cost[pid]) { f.cost[pid] = nc; push(nc, pid); }
            }
        }
      }
      std::vector<std::uint32_t>().swap(buckets[bi]);  // done with this bucket
    }
    f.ready = true;
    return f;
}
}
