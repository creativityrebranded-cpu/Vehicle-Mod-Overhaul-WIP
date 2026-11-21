#pragma once
// World and Route inputs shared by the recorded simulator and the diagnostic observer.
#include "planframe.h"
#include "planscene.h"

namespace planinput {
struct FloorPoint { double x, y, z; };

#ifndef FVO_RELEASE
inline const char* Sensor(const planworld::Shape& s) {
    return s.sensor ? s.sensor : s.source == planworld::Source::Ray ? "retained_ray" : planworld::Name(s.source);
}
#endif

// Every measured shape into a scene by the one physical rule (shared by lane scenes and step-6 goal scenes): moving vehicles and people
// become movers (D4), predictions are soft, the rest are solids sampled over their real footprint. The floor must already be set.
// GR-2: replace a suspect starting measurement only in the grade profile. Three contiguous
// forward measurements must agree on slope and the extrapolated height must agree with
// the rover's actual base. Collision and edge checks continue to use the original floor.
inline bool RepairGradeStart(std::vector<FloorPoint>& floor, double carX, double carY, double carBase) {
    if (floor.size() < 4 || !std::isfinite(carBase)) return false;
    const auto& a = floor[0]; const auto& b = floor[1]; const auto& c = floor[2]; const auto& d = floor[3];
    if (std::hypot(a.x - carX, a.y - carY) > .75 || std::fabs(a.z - carBase) < .25) return false;
    const double dx = d.x - a.x, dy = d.y - a.y, length = std::hypot(dx, dy);
    if (length < 1) return false;
    auto along = [&](const FloorPoint& p) { return ((p.x - a.x) * dx + (p.y - a.y) * dy) / length; };
    auto across = [&](const FloorPoint& p) { return std::fabs(((p.y - a.y) * dx - (p.x - a.x) * dy) / length); };
    const double ab = along(b), bc = along(c) - ab, cd = length - along(c);
    constexpr double minGap = .5 * planrun::kStep, maxGap = 1.3 * planrun::kStep;
    if (ab < minGap || bc < minGap || cd < minGap || ab > maxGap || bc > maxGap || cd > maxGap ||
        across(b) > .25 || across(c) > .25) return false;
    const double first = (c.z - b.z) / bc, second = (d.z - c.z) / cd;
    if (std::fabs(first - second) > .10) return false;
    const double estimate = b.z - ab * (d.z - b.z) / (bc + cd);
    if (std::fabs(estimate - a.z) <= carcap::kCar.climb || std::fabs(estimate - carBase) > .50) return false;
    floor[0].z = estimate;
    return true;
}
inline constexpr double kPointHalf = .05;  // PS-1: half size of a measured ray point in the scene
inline void AddShapes(planscene::Scene& scene, const planrun::Frame& frame, const planworld::Shape* shapes, int count) {
    for (int i = 0; i < count; ++i) {
        const auto& s = shapes[i];
        if (s.unmeasured || s.expires < frame.now || planrun::SolidAllowed(s, frame)) continue;
        const bool liveUser = s.source == planworld::Source::Vehicle || s.source == planworld::Source::Person;
        // ED-1 (2026-11-12 review: shapes were dropped here by the old planner's labels, "not mayStop", "Predicted", "not Sure", so a wall seen
        // by one ray was empty space to this planner): every measured shape is kept and the one physical rule (planscene: height against
        // the body and the tyres) decides what it is. Predictions (survey, game boxes) are kept as soft: a cost, never a block.
        // RT-1 (2026-11-18, WORKING_MANDATE "Stop rule from world truth", approved 2026-11-07): a hit world truth says never stops the car
        // (an unexplained road or terrain hit, one small enough to drive over) eases it as a soft cost; it is never a block. Walls,
        // structures, unknown, loose objects, vehicles and people keep mayStop and stay solids, so ED-1's one-ray wall still blocks.
        const bool soft = !liveUser && (planworld::Predicted(s) || !s.mayStop);
        // Preserve the measured footprint of larger shapes, including their orientation.
        const double base = planworld::Base(s), top = planworld::Top(s);
        // D4 (one-authority step 1): a moving vehicle or person is a mover with World's own prediction of where it will be (its published plan
        // where it has one, else its velocity), not a frozen solid. A standing one stays a solid below.
        if (liveUser && planworld::Moving(s)) {
            planscene::Mover m; m.heading = s.heading; m.halfWidth = std::max(.3, s.halfWidth); m.halfLength = std::max(.3, s.halfLength);
            m.base = base; m.top = std::max(top, base + .5); m.person = s.source == planworld::Source::Person;
            m.margin = std::min(1.5, planworld::Margin(s, 2.0));
            for (int k = 0; k < planscene::kMoverSamples; ++k) { const auto p = planworld::At(s, k * planscene::kMoverStep); m.x[k] = p.x; m.y[k] = p.y; }
            scene.movers.push_back(m);
            continue;
        }
        // F (2026-11-19): an up-facing surface within 0.35 m of the road under it is road. The road is World's fitted road for this shape
        // (RF-1) where it has one, else the lane-line floor (a banked or crowned road is not the lane line's height beside it).
        double floorZ = frame.carBase;
        scene.Floor(s.centre.x, s.centre.y, floorZ);
        const double roadZ = std::isfinite(s.road) ? s.road : floorZ;
        if (!liveUser && s.haveNormal && s.normalZ > .7 && top <= roadZ + .35) continue;
        const int across = std::clamp(int(std::ceil(s.halfWidth * 4)), 1, 32);
        const int along = std::clamp(int(std::ceil(s.halfLength * 4)), 1, 32);
        const double c = std::cos(s.heading), n = std::sin(s.heading);
        // PS-1 (2026-11-19): a ray hit is a measured point on a face; it stands for 0.05 m round it (ray and pose error), not the
        // shape's default 0.2 m, which put 0.2 m of solid in front of every measured face (a barrier 5-20 cm from the body read as overlap).
        const bool point = s.source == planworld::Source::Ray && s.halfHeight == 0 && s.halfWidth <= .2 + 1e-9 && s.halfLength <= .2 + 1e-9;
        const double half = point ? kPointHalf : std::max(s.halfWidth / across, s.halfLength / along);
        for (int a = 0; a < across; ++a) for (int b = 0; b < along; ++b) {
            const double u = ((2.0 * a + 1) / across - 1) * s.halfWidth;
            const double v = ((2.0 * b + 1) / along - 1) * s.halfLength;
            scene.solids.push_back({ s.centre.x + c * u + n * v, s.centre.y - n * u + c * v, base, top, half, soft, liveUser && !s.steerRound });  // D1: an occupied road user waits
            auto& evidence = scene.solids.back();
            evidence.floorProbeBody = s.floorProbeBody;
            if (std::isfinite(s.road)) { evidence.road = s.road; evidence.roadSpread = s.roadSpread; }  // RF-1: World's road under it
#ifndef FVO_RELEASE
            evidence.sensor = Sensor(s);
            evidence.source = int(s.source);
            evidence.owner = s.owner;
            evidence.body = s.sensorBody;
            evidence.layer = s.layer;
            evidence.support = s.support;
            evidence.seenAt = s.seenAt;
            evidence.haveNormal = s.haveNormal;
            evidence.normalZ = s.normalZ;
#endif
        }
    }
}

// The lateral intervals (route line frame, + = right) a body may stand in, from the road's lanes: merged where lanes touch, gaps kept
// (a median). The outermost edge is finite only when that side has a band; no band at all and no gap: nothing known (empty).
inline std::vector<std::pair<double, double>> KnownLegal(const planbranch::Lanes& lanes) {
    std::vector<std::pair<double, double>> out;
    const int n = std::min(lanes.count, planbranch::kMaxLanes);
    if (n <= 0) return out;
    std::vector<std::pair<double, double>> all;
    for (int k = 0; k < n; ++k) if (std::isfinite(lanes.left[k]) && std::isfinite(lanes.right[k]) && lanes.right[k] > lanes.left[k]) all.push_back({ lanes.left[k], lanes.right[k] });
    if (all.empty()) return out;
    std::sort(all.begin(), all.end());
    for (const auto& r : all) { if (!out.empty() && r.first <= out.back().second + .02) out.back().second = std::max(out.back().second, r.second); else out.push_back(r); }
    const double inf = std::numeric_limits<double>::infinity();
    const bool lowBand = std::isfinite(lanes.bandLow) && lanes.bandLow > 0, highBand = std::isfinite(lanes.bandHigh) && lanes.bandHigh > 0;
    if (!lowBand) out.front().first = -inf;
    if (!highBand) out.back().second = inf;
    if (out.size() == 1 && !lowBand && !highBand) out.clear();
    return out;
}

template <class Line, class Wanted>
planscene::Scene Scene(const planrun::Frame& frame, const Line& line, const Wanted& wanted,
                       const planworld::Shape* shapes, int count) {
    planscene::Scene scene;
    if (frame.field && frame.fieldCount > 0)
        scene.field.assign(frame.field, frame.field + frame.fieldCount);
    scene.laneHalf = std::max(.5, (frame.lanes.ownRight - frame.lanes.ownLeft) * .5);
    // Known no-go (route edge bands and medians): the lane edges the road gave already exclude them, so what is left is where a body may
    // stand. Every listed lane counts, open or not (a closed lane is the coordinator's rule, not a band). A gap between lanes is a median;
    // an outer end is a hard edge only on a side that has a band. Never sensed: it does not flicker.
    scene.legal = KnownLegal(frame.lanes);
    scene.continuousRecovery = true;
    scene.managerHeld = frame.held;
    // D6/D7 (one-authority step 3): the stretch's rule, whether the car can hover, a 3D flight line, and the facing it should keep.
    scene.travel = frame.travel; scene.hoverPossible = frame.hoverPossible; scene.flightLine = frame.flightLine; scene.facing = frame.facing;
    for (int i = -20; i <= 80; ++i) {
        const auto p = line(i);
        if (scene.route.empty() || std::hypot(p.x - scene.route.back().x, p.y - scene.route.back().y) > .001)
            scene.route.push_back({ p.x, p.y, 0.0, p.z });  // z (step 3): a flight line's height
    }

    std::vector<FloorPoint> floor;
    for (int i = 0; i < planrun::kSamples; ++i) {
        const double d = i * planrun::kStep;
        const auto w = wanted(d, 0);
        if (!w.have || !w.measured) continue;
        const auto p = line(d);
        floor.push_back({ p.x, p.y, w.z });
    }
    // RP-1 (2026-11-19): the floor between two neighbouring measured samples is the line between them. The nearest sample's height
    // alone made a ramp a staircase (lc_ring_road: 0.78 m per 1.85 m step, so the car's own floor jumped +-0.39 m as it moved).
    // A gap of more than two missing samples is not bridged: past it, the nearest sample stands as before.
    auto makeFloor = [](std::vector<FloorPoint> samples) {
        return [floor = std::move(samples)](double x, double y, double& z) {
        constexpr double kSpan = 3.5 * planrun::kStep;
        double best = 36;
        bool found = false;
        for (std::size_t i = 0; i < floor.size(); ++i) {
            const auto& p = floor[i];
            const double d = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y);
            if (d < best) { best = d; z = p.z; found = true; }
            if (i + 1 >= floor.size()) continue;
            const auto& q = floor[i + 1];
            const double sx = q.x - p.x, sy = q.y - p.y, len2 = sx * sx + sy * sy;
            if (len2 < 1e-6 || len2 > kSpan * kSpan) continue;
            const double t = ((x - p.x) * sx + (y - p.y) * sy) / len2;
            // The samples start at the car: behind the first (and past the last) the road keeps that end segment's grade for 1.5
            // segments (past a half body length), else the floor behind the car reads flat and the band's slope is half the road's.
            const double lo = i == 0 ? -1.5 : 0.0, hi = i + 2 == floor.size() ? 2.5 : 1.0;
            if (t <= lo || t >= hi) continue;
            const double ox = x - (p.x + t * sx), oy = y - (p.y + t * sy), dd = ox * ox + oy * oy;
            if (dd < best) { best = dd; z = p.z + t * (q.z - p.z); found = true; }
        }
        return found;
        };
    };
    auto gradeProfile = floor;
    if (RepairGradeStart(gradeProfile, frame.x, frame.y, frame.carBase)) {
        scene.floorSetAside = 1;
        scene.gradeFloor = makeFloor(std::move(gradeProfile));
    }
    scene.floor = makeFloor(std::move(floor));

    AddShapes(scene, frame, shapes, count);
    scene.BuildKnownBandSolids();  // known no-go (bands, medians) as walls: the same rules as a sensed wall judge them (planscene.h)
    scene.Index();
    // D5 (one-authority step 1): the line ends `lineEnd` m ahead of the car (a route hold or stop line puts its end there; so does the route's
    // own end). That point is the scene's stop station.
    if (std::isfinite(frame.lineEnd) && frame.lineEnd < 1e6 && scene.route.size() >= 2) {
        double station, lateral, half; scene.Route(frame.x, frame.y, station, lateral, half);
        scene.stopStation = station + std::max(0.0, frame.lineEnd);
    }
    return scene;
}

// Step 6 (D13 in the shadow): a car with no lane (summon, follow, combat, a job). The same World shapes and the same physical rule as a
// lane scene; the route is the way to the goal (planscene::SetGoal). Floor: measured points (nearest within 6 m), as for lanes.
struct GoalAsk {
    double now = 0, carX = 0, carY = 0, carBase = 0, halfLength = 2.3, stopGap = 1.0;
    planscene::Scene::Goal goal;
    int travel = 0; bool hoverPossible = true; double facing = std::numeric_limits<double>::quiet_NaN();
    bool factoryHold = false;
};
inline planscene::Scene GoalScene(const GoalAsk& a, const planworld::Shape* shapes, int count, const FloorPoint* floorPts, int floorCount) {
    planscene::Scene scene;
    scene.travel = a.travel; scene.hoverPossible = a.hoverPossible; scene.facing = a.facing;
    scene.managerHeld = a.factoryHold;
    std::vector<FloorPoint> floor(floorPts, floorPts + std::max(0, floorCount));
    scene.floor = [floor = std::move(floor)](double x, double y, double& z) {
        double best = 36; bool found = false;
        for (const auto& p : floor) { const double d = (x - p.x) * (x - p.x) + (y - p.y) * (y - p.y); if (d < best) { best = d; z = p.z; found = true; } }
        return found;
    };
    planrun::Frame frame; frame.now = a.now; frame.carBase = a.carBase; frame.x = a.carX; frame.y = a.carY;  // no field boxes: nothing allowed through
    AddShapes(scene, frame, shapes, count);
    scene.SetGoal(a.carX, a.carY, a.goal, a.halfLength, a.stopGap);
    if (std::isfinite(a.facing)) { scene.facing = a.facing; scene.facingScale = 1.0; }  // a combat facing holds everywhere; SetGoal's arrive heading only near the goal
    scene.Index();
    return scene;
}

inline bicycle::Spec Spec(const planrun::Frame& f) {
    bicycle::Spec spec;
    spec.car = carmodel::Fallback(f.car.halfLength);
    spec.halfWidth = f.car.halfWidth;
    spec.halfLength = f.car.halfLength;
    spec.height = f.car.height;
    spec.accel = f.accel;
    spec.brake = f.brake;
    spec.maxSpeed = f.topSpeed;
    spec.hoverTop = std::max(f.topSpeed, 1.0);  // step 3: the frame's top (lane-limited) for the hover too
    return spec;
}
}
