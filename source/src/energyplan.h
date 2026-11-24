#pragma once
// Redesign (2026-11-12, user approved; notes/redesign-20261112/AUDIT_AND_REDESIGN.md section 4): ONE planner, ONE energy.
// World (volume + floor) -> Route (where, how fast) -> Plan (this) -> Controller -> Ledger.
//
// Candidates are control sequences (up to three segments of steering input + signed speed) rolled out through the car's own steering
// model (bicycle.h on carmodel.h), so every candidate is drivable. A three-point turn is a candidate with gear changes, not a rule.
// Each candidate gets one energy, a sum of continuous terms:
//   collision  - the body footprint against solid World points inside the body's height band (above what a wheel rolls over).
//                Entering a solid is infinite. Only shape points decide: nothing is labelled "low edge", "doubt" or "wall" first.
//   low edges  - points a wheel rolls over (at or below Spec::climb above the floor): a small cost each.
//   slope      - the floor's grade along the car, fitted over its length (never sample to sample); past the one car record's MaxGrade at
//                the gear's grip (carcap.h) wheels cannot go.
//   stopping   - v^2 <= 2 * brake * free distance at the end of the rollout (the measured-energy rule, applied to every candidate).
//   field      - a soft potential outside the field boxes: never a veto, so standing still is always a candidate.
//   route      - distance from the lane centre, progress along the route, the wanted speed.
//   commitment - distance from the last update's best trajectory, and a cost per gear change.
//   comfort    - steering input changes.
// The lowest energy wins. Nothing withdraws, latches or overrides it. Pure; tests: tests/energyplan_test.cpp; closed loop: tools/plan_sim.cpp.
#include "bicycle.h"
#include "carcap.h"
#include "planlattice.h"
#include "planspeed.h"
#include "recovery.h"
#include "wheelguard.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <type_traits>

namespace energyplan {
constexpr int kSteps = 30;            // rollout steps
constexpr double kDt = .2;            // s per step (6 s ahead)
constexpr int kMaxSegments = 3;
constexpr int kMaxCandidates = 800;  // step 3: the full library + drive-then-stop + hover families (546 + 56 + 7 at most)
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr double kNoGoRise = .05;       // m: leaving a band: the body's centre may be this much deeper in it than it was before the step is refused
constexpr double kNoGoTolerance = .03;  // m: a body this far into known no-go space (route edge band / median) still counts as clear
constexpr double kBandTierStep = .25;   // m: a clearance tier is this deep when the only thing the body is in is a known band (a solid: 5 cm)
constexpr double kBandSoft = .12;       // m: a body this far into a known band alone is a cost, not a clearance tier (beyond it: a tier)

// ST-B: the numbers the margins come from, all from the car or the controller (nothing chosen here).
inline double CreepSpeed(const bicycle::Spec& k) { return k.brake * kDt; }               // m/s: the lowest rung; time to creep out of an intrusion = excess / this
// m: the distance covered in one controller reaction, at the speed (never below the creep speed: a speed under it is "a stop", so a stand
// cannot be placed better than creep x that time).
inline double Margin(double speed, const bicycle::Spec& k) { return std::max(std::fabs(speed), CreepSpeed(k)) * wheelguard::reaction; }
// s: the time a take-off plus a landing takes; hovering is preferred over wheels by one such switch per horizon (the plan sees H ahead).
inline double ModeSwitchTime(const bicycle::Spec& k) { return k.hoverClear / k.climbRate + k.hoverClear / k.descendRate; }
inline double StopGap(const bicycle::Spec& k) { return .5 * k.halfLength; }              // m: a half body length kept short of a stop station
inline double EdgeSpeed(const bicycle::Spec& k) { return std::sqrt(2 * 9.81 * k.climb); } // m/s: the speed that lifts a wheel over the highest edge it rolls over
// ST-A (2026-11-22, notes/GENERATED_COSTS_DESIGN_20261122.md): every cost is in SECONDS. Speed shortfall dt*(1 - v/v_w); route / cost-to-go metres divided
// by the wanted speed; a mode switch costs the take-off + landing time; a gear change the time to stop and pull away; the facing the time to turn the nose.
struct Weights {
    // ST-B (2026-11-22): margins and constraints in seconds. No weight is left but the gear's grip: a body inside the margin m(v) = v * t_r (the
    // distance covered in one controller reaction) pays the time to creep out of it, excess / v_creep (v_creep = brake * kDt, the lowest rung);
    // contact stays infinite (a clearance tier). The lane, the field, a road user's box, a predicted solid, a low edge and a steep grade are priced
    // the same way (see Score). Replaces collision / margin / mover / moverMargin / moverHit / soft / lateral / laneEdge / field / slope /
    // comfortGrade / lowEdge / stopGap / endCurveMax / offLaneEase.
    double grip = 1.0;          // the gear's tyre grip multiplier: speed in a turn is capped by carcap PlanLat(grip) and the grade by carcap
                                // MaxGrade(grip), inside the rollout (D2 2026-11-19, the one car record: was latAccel 3.0 x grip here)
    // Step 2 finding: a trimmed library changed decisions scene by scene; the full library passes every scene, so it stays the default.
    bool fullSet = true;        // the full library (default) / the trimmed coarse set (diagnostic)
    int refineRounds = 0;       // refinement after the trimmed set (diagnostic; the full library needs none)
    bool windowEdgeRule = true; // step 2 (ED-2): progress past the field window counts against the last inside cost to go (false: the horizon time, as before)
};

// mode (step 3): 0 on the wheels (input = steering), 1 hovering (input = turn rate share, strafe = travel angle off the nose).
struct Segment { double input = 0, speed = 0, seconds = 0; int mode = 0; double strafe = 0;
                 double travel = std::numeric_limits<double>::quiet_NaN();
                 bool follow = false; double offset = 0; bool retrace = false; };  // LF-1: wheels; steer each step along the route `offset` m right of it (lane following)  // LK-1: hover only; finite = hold this world travel direction (the strafe follows the nose)
struct Candidate { std::array<Segment, kMaxSegments> seg{}; int count = 0; const char* kind = ""; };

struct GradeFit { double grade = 0, raw = 0; int points = 0, setAside = 0; bool ok = false; };  // GR-2
struct Terms { double hover = 0, modes = 0, facing = 0, movers = 0, soft = 0, togo = 0, progress = 0, lateral = 0, speed = 0, collision = 0, lowEdge = 0, slope = 0, field = 0, gear = 0;
               int clearanceTier = 0;
               double wheelSpeed = kInf;
               const char* wheelReason = "clear";
               double endFree = 0; const char* reject = ""; };  // HC-4: the rule that made a rollout infeasible (traced)  // HC-2: the free distance the stopping rule found at the end (m; traced, not an energy)

inline int ClearanceTier(int firstTier, int lastTier, double depth) {
    return firstTier + int(std::ceil(std::clamp(depth / .05, 0., double(lastTier - firstTier))));
}

inline double WeightedEnergy(int tier, double preference) {
    // The entire preference range is narrower than a single clearance tier.
    return 100.0 * tier + 30.0 * std::atan(preference / 30.0);
}
struct BlockTrace {
    bool valid = false, floorKnown = false;
    double wantedSpeed = 0, seconds = 0, x = 0, y = 0, z = 0, heading = 0, speed = 0;
    double floor = 0, clearance = 0, initialClearance = 0;
    planscene::Solid shape;
};
struct Result {
    BlockTrace centreBlock;
    double laneErr = 0;
    bool legF = false, legFaces = false, legR = false;
    int recTries = 0, recCycles = 0, turnSign = 0;
    bool ok = false;                       // some candidate has finite energy
    // O1 / O4 (Ledger): kept = the committed plan stayed; the challenger it held off (or the plan it replaced) and the energy gap; why it switched.
    bool kept = false; const char* rival = ""; double rivalGap = 0; const char* switchWhy = ""; double offset = 0, speedAhead = 0;
    // HC-4: the plan at the wanted speed (follow lane / straight arc at w): its energy (inf: rejected), why, and its terms; traced beside the best.
    double fullSpeed = 0, fullEnergy = kInf; Terms fullTerms;
    // CT-1 (2026-11-14 20:29 run: stuck 100 s at a corner; 20:37: 0 of 405 feasible, no rule named): how many candidates each rule rejected,
    // and the cheapest lane-following plan at each speed (energy, or the rule that rejected it, and its terms). Traced, not used.
    static constexpr int kRejectKinds = 10; const char* rejectName[kRejectKinds] = {}; int rejectCount[kRejectKinds] = {}; int rejectKinds = 0;
    GradeFit startGrade; double maxGrade = 0; int floorSetAside = 0;  // GR-2 (traced): grade under the car, limit used, floor samples set aside
    struct LaneSpeed { double speed = 0, energy = kInf; const char* reject = ""; Terms terms; };
    static constexpr int kLaneSpeeds = 5; LaneSpeed laneSpeeds[kLaneSpeeds]; int laneSpeedCount = 0;
    int rel[6] = {};  // HC-3: running counts for this car: switches by got worse, too close, enters a solid, yields, the challenge timer; plans kept
    Candidate best; double energy = kInf; Terms terms;
    bicycle::State traj[kSteps + 1] = {};  // the best rollout
    bicycle::Control first;                // what to do now
    int candidates = 0, feasible = 0, gearChanges = 0;
    double minClearance = kInf;            // along the best rollout
    const char* why = "";                  // the best candidate's kind (Ledger)
    int firstMode = 0; double firstStrafe = 0, firstHeight = 0;  // step 3: wheels or hover now, its travel angle, the height it holds
    bool pulse = false, testRefused = false;  // D12 (step 4): this command is the requested spot-test pulse / the pulse was refused (not safe)
    // RV-1: the mode that chose (follow / avoid / recover / stand), the routine's step, whether either changed this tick, and why
    const char* layer = ""; const char* layerWhy = ""; const char* step = ""; bool layerChanged = false;
};
struct Trail {
    struct Point { double x = 0, y = 0, z = 0; unsigned options = 0; bool swept = false; };
    static constexpr int capacity = 64;
    std::array<Point, capacity> points{};
    int count = 0, target = -1;
    double seen = -1;

    void Note(const bicycle::State& car, unsigned options, bool swept, double now) {
        if (seen >= 0 && (now < seen || now - seen > 2 ||
            (count && (std::fabs(car.z - points[count - 1].z) > 3 ||
             std::hypot(car.x - points[count - 1].x, car.y - points[count - 1].y) > 6)))) {
            count = 0; target = -1;
        }
        seen = now;
        if (car.mode) { count = 0; target = -1; return; }
        if (target >= 0) {
            const auto& goal = points[target];
            if (std::hypot(car.x - goal.x, car.y - goal.y) <= .55) { count = target + 1; target = -1; }
            return;
        }
        if (count && std::hypot(car.x - points[count - 1].x, car.y - points[count - 1].y) < .45) {
            auto& last = points[count - 1];
            if (std::popcount(options) > std::popcount(last.options)) last.options = options;
            last.swept |= swept;
            return;
        }
        if (count == capacity) {
            std::move(points.begin() + 1, points.end(), points.begin());
            --count;
        }
        points[count++] = {car.x, car.y, car.z, options, swept};
    }

    int FindTarget(const bicycle::State& car) const {
        int alternative = -1;
        double distance = 0, x = car.x, y = car.y;
        for (int i = count - 1; i >= 0; --i) {
            const auto& p = points[i];
            distance += std::hypot(x - p.x, y - p.y); x = p.x; y = p.y;
            if (distance < 2) continue;
            if (distance > 18) break;
            if (std::popcount(p.options) < 3) continue;
            if (p.swept) return i;
            if (alternative < 0) alternative = i;
        }
        return alternative;
    }

    const Point& LookBack(const bicycle::State& car) const {
        int nearest = target;
        double best = kInf;
        for (int i = target; i < count; ++i) {
            const auto& p = points[i];
            const double d = std::hypot(car.x - p.x, car.y - p.y);
            if (d < best) { best = d; nearest = i; }
        }
        while (nearest > target && std::hypot(car.x - points[nearest].x, car.y - points[nearest].y) < 1.2) --nearest;
        return points[nearest];
    }
};

struct Memory { bool have = false; Candidate best; bicycle::State traj[kSteps + 1] = {}; double at = 0;
                double challengeSince = -1, switchedAt = -1e9;  // O1: since when a challenger has been winning by the margin; the last switch
                double keptGap = std::numeric_limits<double>::quiet_NaN();  // HC-1: the smallest gap (kept minus best) since the plan was chosen
                int relWorse = 0, relClose = 0, relSolid = 0, relYield = 0, relTimer = 0, keptN = 0; double relSince = -1;
                // RV-1: the mode state machine, when the plan last ran (2 s apart: a fresh start), where the car stood then (progress)
                Trail trail;
                recovery::State rec; double recSeen = -1, progX = 0, progY = 0;
                double queuedSince = -1; };   // AP-1: since when the lane centre is blocked by a road user (queued in traffic)  // HC-3: switches per rule, per second


inline bicycle::Control Command(const Segment& g, const bicycle::Spec& k, const Weights& wt) {
    const double curv = std::fabs(std::tan(carmodel::WheelAngle(k.car, g.input, std::fabs(g.speed)))) / std::max(1.0, k.car.wheelbase);
    const double grip = curv > 1e-6 ? std::sqrt(carcap::kCar.PlanLat(wt.grip) / curv) : kInf;
    return { g.input, std::clamp(g.speed, -grip, grip) };
}

// LF-1 (2026-11-13 10:48 run: with the lane priced, every fixed-steering shape at 9-18 m/s left the lane somewhere in 6 s, so only slow ones
// were chosen and the car crawled 40% slower): the steering a driver uses, worked out each step. Pure pursuit to the route point
// `look` m ahead at the segment's offset, through this car's own steering (the speed-dependent wheel angle inverted by bisection).
template <class W>
inline double FollowInput(const W& w, const bicycle::State& s, const Segment& g, const bicycle::Spec& k, double station) {
    const double v = std::fabs(s.speed > .1 ? s.speed : g.speed), look = std::clamp(2.0 + .9 * v, 4.0, 18.0);
    double tx, ty; if (!w.RouteAt(station + look, g.offset, tx, ty)) return 0;
    const double alpha = std::remainder(std::atan2(tx - s.x, ty - s.y) - s.heading, 2 * 3.14159265358979323846);
    const double want = std::atan(2 * std::sin(alpha) / look * std::max(1.0, k.car.wheelbase)), speed = std::max(std::fabs(g.speed), v);
    double lo = -1, hi = 1;
    if (want >= carmodel::WheelAngle(k.car, 1, speed)) return 1;
    if (want <= carmodel::WheelAngle(k.car, -1, speed)) return -1;
    for (int i = 0; i < 14; ++i) { const double mid = .5 * (lo + hi); (carmodel::WheelAngle(k.car, mid, speed) < want ? lo : hi) = mid; }
    return .5 * (lo + hi);
}

// The World the planner reads. W must provide:
//   double Clearance(double x, double y, double heading, double floorZ, const bicycle::Spec&) const  // m to the nearest solid in the body band; < 0 inside
//   int    LowEdges (double x, double y, double heading, double floorZ, const bicycle::Spec&) const  // wheel-climbable points under the body
//   Probe Contact(x, y, heading, floorZ, spec) const  // step 2: {clear, low, soft} in one pass
//   int RouteLocal(x, y, seg, station, lateral, laneHalf) const  // step 2: projection near the last segment
//   bool lanes; double facingScale  // D13 (step 4): a goal scene has no lane; the facing's weight now
//   bool RouteAt(station, offset, x&, y&) const  // LF-1: a point along the route
//   double stopStation; double MoverClearance(x, y, heading, floorZ, spec, t, motionX, motionY, bool& closing, int* which) const  // D5, D4
//   bool MoverReaches(int which, x, y, heading, floorZ, spec) const  // step 6: the follower excuse holds only if it reaches where the car stands
//   int    SoftNear (double x, double y, double heading, double floorZ, const bicycle::Spec&) const  // predicted solids near the body (ED-1)
//   bool   Floor(double x, double y, double& z) const
//   double FieldOutside(double x, double y) const                                                   // m outside the field (0 inside or no field)
//   void   Route(double x, double y, double& station, double& lateral, double& laneHalf, double hint) const  // hint: a nearby station (NaN: anywhere)
template <class W>
inline GradeFit FitGrade(const W& w, const bicycle::State& s, const bicycle::Spec& k, bool useGradeProfile = true) {
    // GR-2 (2026-11-19 17:44 run): the floor's grade along the car is its pattern, not one sample. Nine points over the car's half length
    // plus a metre each side; a first least-squares fit; points further than 3 robust spreads (at least 0.08 m) from it are set aside;
    // the grade is the second fit. On the lc_ring_road ramp one sample 0.45 m under the car (a deck seam or a lower surface) read 0.88 on
    // a 0.42 road and every path was "too steep for wheels".
    constexpr int kN = 9; double a[kN], z[kN]; bool keep[kN]; int n = 0;
    const double reach = k.halfLength + 1.0;
    for (int i = 0; i < kN; ++i) {
        const double d = -reach + 2 * reach * i / (kN - 1); double zz;
        const double x = s.x + d * std::sin(s.heading), y = s.y + d * std::cos(s.heading);
        bool found;
        if constexpr (requires { w.GradeFloor(x, y, zz); }) {
            found = useGradeProfile ? w.GradeFloor(x, y, zz) : w.Floor(x, y, zz);
        } else found = w.Floor(x, y, zz);
        if (!found || !std::isfinite(zz)) continue;
        a[n] = d; z[n] = zz; keep[n] = true; ++n;
    }
    GradeFit g; g.points = n;
    if (n < 3) return g;
    auto fit = [&](double& slope, double& at0) {
        double sx = 0, sz = 0, sxx = 0, sxz = 0; int m = 0;
        for (int i = 0; i < n; ++i) if (keep[i]) { sx += a[i]; sz += z[i]; sxx += a[i] * a[i]; sxz += a[i] * z[i]; ++m; }
        const double den = m * sxx - sx * sx;
        if (m < 3 || std::fabs(den) < 1e-9) return false;
        slope = (m * sxz - sx * sz) / den; at0 = (sz - slope * sx) / m; return true;
    };
    double slope = 0, at0 = 0;
    if (!fit(slope, at0)) return g;
    g.raw = slope;
    double dev[kN]; for (int i = 0; i < n; ++i) dev[i] = std::fabs(z[i] - (at0 + slope * a[i]));
    double sorted[kN]; std::copy(dev, dev + n, sorted); std::nth_element(sorted, sorted + n / 2, sorted + n);
    const double cut = std::max(.08, 3 * 1.4826 * sorted[n / 2]);
    for (int i = 0; i < n; ++i) { keep[i] = dev[i] <= cut; g.setAside += !keep[i]; }
    double slope2 = slope, at02 = at0;
    g.grade = fit(slope2, at02) ? slope2 : slope;
    g.ok = true;
    return g;
}
template <class W>
inline double Grade(const W& w, const bicycle::State& s, const bicycle::Spec& k) { return FitGrade(w, s, k).grade; }

inline bicycle::Control RetraceControl(const Trail& trail, const bicycle::State& car,
                                      const bicycle::Spec& spec, const Weights& weights, Segment segment) {
    const auto& waypoint = trail.LookBack(car);
    const auto& end = trail.points[trail.target];
    const double distance = std::hypot(car.x - end.x, car.y - end.y);
    const double dx = waypoint.x - car.x, dy = waypoint.y - car.y;
    const double error = std::remainder(std::atan2(dx, dy) - car.heading - 3.14159265358979323846,
                                       2 * 3.14159265358979323846);
    // Reverse pure pursuit: the wheel angle has the opposite sign from yaw.
    const double wheel = -std::atan2(2 * spec.car.wheelbase * std::sin(error),
                                     std::max(.5, std::hypot(dx, dy)));
    double lo = -1, hi = 1;
    for (int i = 0; i < 14; ++i) {
        const double mid = (lo + hi) / 2;
        if (carmodel::WheelAngle(spec.car, mid, std::fabs(car.speed)) < wheel) lo = mid;
        else hi = mid;
    }
    segment.input = (lo + hi) / 2;
    segment.speed = distance < .35 ? 0.0 : -std::min(1.2, std::sqrt(std::max(0.0, distance - .30)));
    return Command(segment, spec, weights);
}

template <class W>
inline bicycle::Control FirstControl(const Candidate& candidate, const bicycle::State& car, const bicycle::Spec& spec,
                                     const W& world, const Weights& weights, const Memory* memory, double hint) {
    auto segment = candidate.seg[0];
    if (segment.mode == 1) return {segment.input, segment.speed};
    if (segment.retrace && memory && memory->trail.target >= 0)
        return RetraceControl(memory->trail, car, spec, weights, segment);
    if (segment.follow && world.lanes) {
        double station, lateral, half;
        world.Route(car.x, car.y, station, lateral, half, hint);
        segment.input = FollowInput(world, car, segment, spec, station);
    }
    return Command(segment, spec, weights);
}

template <class W>
inline double Score(const Candidate& c, const bicycle::State& start, const bicycle::Spec& k, double want, const W& w, const Weights& wt,
                    const Memory* mem, double now, Terms& terms, bicycle::State* out, double& minClear, double hint = std::nan(""), const planlattice::Field* field = nullptr
                    , BlockTrace* trace = nullptr, bool weightedClearance = false, bool wheelVote = false
                    ) {
    terms = {}; minClear = kInf;
    bicycle::State s = start;
    double z0 = 0; const bool floor0 = w.Floor(s.x, s.y, z0);
    if (s.mode == 0 && floor0) s.z = z0;  // on its wheels the car stands on the floor
    // A stationary candidate may hold its starting position. A rover already above
    // its grade limit may move toward a gentler grade without approving a sustained climb.
    const double startGrade = s.mode == 0 && floor0 ? std::fabs(Grade(w, s, k)) : 0.0;
    if (out) out[0] = s;
    // The body's height band: on wheels from the floor (what the wheels roll over is under the band); hovering, anything taller than 0.2 m
    // under the car's base blocks it (step 3).
    auto bandFloor = [&](const bicycle::State& st, double floorZ) { return st.mode == 0 ? floorZ : st.z - k.climb + .2; };
    double clear0 = w.Clearance(s.x, s.y, s.heading, bandFloor(s, z0), k, s.mode == 0);
    // kNoGoTolerance, meaningfully (2026-11-20, 08:00-08:03 S-bend stall: 643 plan switches over bodies 1-10 cm into a band): a body within the
    // tolerance of a KNOWN band alone is clear. Every sensed solid still counts in full (the check leaves the band walls out and asks again).
    auto bandSlack = [&](double c, const bicycle::State& st, double floorZ) {
        return c < 0 && c >= -kNoGoTolerance && st.mode == 0 && !w.legal.empty() && w.Clearance(st.x, st.y, st.heading, floorZ, k, true, true) >= 0;
    };
    if (bandSlack(clear0, s, bandFloor(s, z0))) clear0 = 0;
    double st0, lat0, half0; int rseg = -1; w.Route(s.x, s.y, st0, lat0, half0, hint, nullptr, nullptr, &rseg);
    // Step 2: route projections follow the rollout from the last segment (RouteLocal) instead of scanning the station window each step.
    auto routeAt = [&](double x, double y, double& st, double& lat, double& half) { if (rseg >= 0) rseg = w.RouteLocal(x, y, rseg, st, lat, half); else w.Route(x, y, st, lat, half, st0); };
    // Known no-go (route edge bands, medians): walls in the scene (planscene BuildKnownBandSolids), refused by the same collision rule below as any
    // sensed wall, including "a car already touching may move without going deeper" (its way out). Only the reason differs: enters_a_known_band.
    // The way out of a band: a car whose contact is with a known band ALONE (the sensed solids leave it clear) may take a path that does not carry its
    // centre deeper into the band (turning out swings the rear across for a moment, so the centre is judged, as the corridor's tolerance did).
    // Every other wall rule stays: a sensed wall is never excused, and a path that goes deeper or a car outside is refused as for any wall.
    bool inBand = !w.legal.empty() && w.lanes && start.mode == 0 && clear0 < 0 && w.Clearance(s.x, s.y, s.heading, bandFloor(s, z0), k, true, true) >= 0;
    double bandPrevC = 0;
    if (inBand) { double yaw0 = s.heading, a0, b0, h0; w.Route(s.x, s.y, a0, b0, h0, st0, &yaw0); bandPrevC = w.NoGoDepth(lat0, 0, k); }
    int seg = 0; double segLeft = c.count ? c.seg[0].seconds : 0;
    double lastDir = start.speed > .3 ? 1 : start.speed < -.3 ? -1 : 0; int gears = 0;
    const double vw = std::max(1.0, want);  // ST-A: the wanted speed turns metres into seconds
    const double vCreep = CreepSpeed(k);
    // ST-B: each constraint pays once per rollout the time to creep out of its worst intrusion (the deepest step), never a rate per second.
    double inCollision = 0, inMover = 0, inSoft = 0, inLane = 0, inField = 0, inEdge = 0, inGrade = 0;
    const double laneOut0 = w.lanes && std::isfinite(lat0) && std::isfinite(half0) ? std::max(0.0, std::fabs(lat0) + k.halfWidth - half0) : 0.0;  // joining: priced from where the plan starts
    double lastIn = kInf, lastInStation = 0;  // ED-2 (step 2): the cost to go at the last rollout state inside the field window, and its station
    bool contactExcused = false;  // D4: this rollout's predicted contact began as a follower coming from behind; the rest of it stays theirs
    double endTurn = start.heading; struct { double x, y; } endFrom = { start.x, start.y };  // HC-2: heading and position one step before the end
    double prevSpeed = start.speed, prevClear = clear0;  // prevClear: the clearance at the step before (the midpoint bound below)
    bool outsideLane = false;  // this step's body is past its lane's edge by more than the plan started with
    double prevStation = st0;  // ST-A: the speed that counts is the way gained along the route (closing on the goal), not speed in any direction
    const bool onRoute = (w.lanes && std::isfinite(st0)) || w.goal.have;
    double lastGrade = 0;  // SG-1: the floor's signed grade along the nose at the last step (0 until measured, or off the floor)
    for (int step = 1; step <= kSteps; ++step) {
        while (seg < c.count - 1 && segLeft <= 1e-9) { ++seg; segLeft = c.seg[seg].seconds; }
        const Segment& g = c.seg[std::min(seg, c.count - 1)]; segLeft -= kDt;
        const double dir = g.speed > .05 ? 1 : g.speed < -.05 ? -1 : 0;
        if (dir != 0 && lastDir != 0 && dir != lastDir) {  // ST-A: replace the command (stop), brake, replace it again (go), pull away the other way at the gear's acceleration
            ++gears; terms.gear += 2 * wheelguard::reaction + std::fabs(s.speed) / k.brake + std::min(std::fabs(g.speed), g.speed < 0 ? k.maxReverse : g.mode == 1 ? k.hoverTop : k.maxSpeed) / (g.mode == 1 ? k.hoverAccel : k.accel); }
        if (dir != 0) lastDir = dir;
        // Step 3: a hover segment flies (HoverStep toward the height it holds); a wheels segment drives (grip: the speed in a turn is what
        // the tyres hold), and a car still in the air first comes down at the descent rate, landing inside the plan.
        const bicycle::State pre = s;
        const int modeBefore = s.mode; endTurn = s.heading; endFrom = { s.x, s.y };  // HC-2: the last step's start, for the end curvature
        if (g.mode == 1) {
            if (!w.hoverPossible) { terms.reject = "hover_not_possible"; return kInf; }
            if (w.travel == 1) { terms.reject = "hover_on_a_drive_only_stretch"; return kInf; }  // NH-1 (user 2026-11-15): drive only = never hover; a car in the air only comes down
            double fc; const bool hf = w.Floor(s.x, s.y, fc);
            const double strafe = std::isfinite(g.travel) ? std::remainder(g.travel - s.heading, 2 * 3.14159265358979323846) : g.strafe;  // LK-1
            s = bicycle::HoverStep(s, g.input, g.speed, strafe, w.HoverTarget(s.x, s.y, fc, hf, k, rseg), k, kDt);
        } else if (s.mode == 1) {
            double fc; const bool hf = w.Floor(s.x, s.y, fc); const double fl = hf ? fc : s.z;
            if (s.z > fl + .3) s = bicycle::HoverStep(s, 0, g.speed, 0, fl, k, kDt);  // still coming down
            else { s.mode = 0; s.z = fl; s = bicycle::Step(s, Command(g, k, wt), k, kDt, lastGrade); }
        } else if (g.retrace) {
            if (!mem || mem->trail.target < 0) { terms.reject = "retrace_checkpoint_unavailable"; return kInf; }
            s = bicycle::Step(s, RetraceControl(mem->trail, s, k, wt, g), k, kDt, lastGrade);
        } else if (g.follow && w.lanes) {  // LF-1: lane following, steered each step
            double st, la, ha; routeAt(s.x, s.y, st, la, ha);
            Segment f = g; f.input = FollowInput(w, s, g, k, st); s = bicycle::Step(s, Command(f, k, wt), k, kDt, lastGrade);
        } else s = bicycle::Step(s, Command(g, k, wt), k, kDt, lastGrade);  // SG-1: the slope found last step
        double z = z0; const bool haveFloor = w.Floor(s.x, s.y, z);
        if (s.mode == 0 && haveFloor) s.z = z;
        if (out) out[step] = s;
        if (s.mode != modeBefore) terms.modes += s.mode == 1 ? k.hoverClear / k.climbRate : k.hoverClear / k.descendRate;  // the time the change takes: climbing to the height it holds / coming down from it  // ST-A: take-off + landing time
        if (s.mode == 0 && w.travel == 2) { terms.reject = "wheels_on_a_flight_only_stretch"; return kInf; }               // a flight-only stretch: no wheels
        if (s.mode == 1) {
            if (haveFloor && z > s.z + .05) {
                if (!weightedClearance) { terms.reject = "ground_rises_into_the_hover"; return kInf; }
                terms.clearanceTier = std::max(terms.clearanceTier, ClearanceTier(10, 29, z - s.z));
            }
        }
        if (s.mode == 1) terms.hover += ModeSwitchTime(k) / (kSteps * kDt) * kDt;  // ST-A: wheels preferred where both do the job: one switch's time per horizon of flight
        outsideLane = false;
        const double band = bandFloor(s, z);
        // Collision: entering a solid is infinite. A car already touching may move without going deeper (it can always get out).
        const auto probe = w.Contact(s.x, s.y, s.heading, band, k, Margin(s.speed, k), s.mode == 0);  // step 2: clearance, low edges and predictions in one pass (RP-1: tilted on wheels)
        double clear = probe.clear;
        if (probe.nearest && probe.nearest->band && bandSlack(clear, s, band)) clear = 0;  // within the tolerance of a known band alone: clear
        bool leavingBand = false;
        if (inBand && clear < 0 && probe.nearest && probe.nearest->band && s.mode == 0 && w.Clearance(s.x, s.y, s.heading, band, k, true, true) >= 0) {
            double st2, la2, ha2; routeAt(s.x, s.y, st2, la2, ha2);
            const double cdep = w.NoGoDepth(la2, 0, k);
            if (cdep <= bandPrevC + kNoGoRise) { leavingBand = true; bandPrevC = std::min(bandPrevC, cdep); }
        }
        if (clear >= 0) inBand = false;  // out: from here the whole body is judged as for any wall
        if (!leavingBand) minClear = std::min(minClear, clear);
        // ST-B: the rollout sees the body once per step, so between two samples the body has moved up to half a step's travel from the nearest one
        // (a turning body's far corner moves faster than its centre by 1 + halfLength / radius). A sampled clearance under that may hide a contact:
        // it counts as one. (The hairpin grazed a wall by 10 cm at 4 m/s once the old 0.5 m cost was gone: its samples were 0.8 m apart.)
        // The pose half a step back is looked at too (the body at the midpoint of the step), so what is left unseen is a quarter step.
        const double sweep = .25 * kDt * std::max(std::fabs(s.speed), std::fabs(prevSpeed)) * (s.mode == 0 ? 1 + k.halfLength / k.MinRadius() : 1.0);
        prevSpeed = s.speed;
        double clearC = clear;  // the clearance the contact test uses: the least of the sample and the midpoint
        if (sweep > 0 && clear >= sweep && std::min(prevClear, clear) < 3 * sweep && std::hypot(s.x - pre.x, s.y - pre.y) > .05) {  // HV-3: exact early-out. Clearance moves at most as fast as the body, and the midpoint is half a step (2 sweeps of edge travel) from either sample, so when both samples clear 3 sweeps the midpoint clears one and is not looked at
            const double mh = pre.heading + .5 * std::remainder(s.heading - pre.heading, 2 * 3.14159265358979323846);
            clearC = std::min(clear, w.Clearance(.5 * (s.x + pre.x), .5 * (s.y + pre.y), mh, band, k, s.mode == 0));
        }
        prevClear = clear;
        if (clearC < sweep && !leavingBand && !(clear0 < sweep && clear >= clear0 - .02)) {
            if constexpr (requires { probe.nearest; }) {
                if (trace && !trace->valid && probe.nearest)
                    *trace = {true, haveFloor, c.seg[0].speed, step * kDt, s.x, s.y, s.z, s.heading, s.speed,
                              band, clearC - sweep, clear0, *probe.nearest};  // the trace reports the clearance of the swept body: below 0 is the rejection
            }
            if (!weightedClearance) { terms.reject = probe.nearest && probe.nearest->band ? "enters_a_known_band" : "enters_a_solid"; return kInf; }
            double deeper = sweep - clearC + std::min(0.0, clear0);
            // A known band alone is a line the road's edge draws, not a wall the body hits: how deep into it is graded in kBandTierStep metres, not
            // 5 cm, so a body wavering a few centimetres at its edge does not change tier (and plan) every tick (08:00-08:03 stall: 465 tier switches).
            const bool bandOnly = probe.nearest && probe.nearest->band && s.mode == 0 && w.Clearance(s.x, s.y, s.heading, band, k, true, true) >= 0;
            if (bandOnly) deeper *= .05 / kBandTierStep;
            // Up to kBandSoft into a known band alone is a cost (the collision term above, continuous in the depth), never a tier: the tier is a
            // step of 100 in the energy, so a body hugging the band edge flipped between plans at the tolerance line (the 4.4 switches a second).
            if (!(bandOnly && -clear <= kBandSoft)) terms.clearanceTier = std::max(terms.clearanceTier, ClearanceTier(10, 29, deeper));
        }
        { const double m = Margin(s.speed, k); if (clear < m) inCollision = std::max(inCollision, m - clear);
          if (s.mode == 0 && probe.low > 0) inEdge = std::max(inEdge, (std::fabs(s.speed) - EdgeSpeed(k)) / k.brake);  // time to slow to the speed that lifts a wheel over the edge
          if (probe.soft > 0) inSoft = std::max(inSoft, m); }  // a predicted solid within the margin: the whole margin is the intrusion (its distance is not known)
        if (haveFloor && s.mode == 0) {
            lastGrade = Grade(w, s, k); const double grade = std::fabs(lastGrade);  // SG-1: signed, for the next step's acceleration
            const double maxGrade = carcap::kCar.MaxGrade(wt.grip);
            const bool holdingStart = std::fabs(s.speed) < .1 && std::hypot(s.x - start.x, s.y - start.y) < .1;
            const bool retreatingToGentlerGrade = startGrade > maxGrade && grade < startGrade - .02;
            if (grade > maxGrade && !holdingStart && !retreatingToGentlerGrade) {
                if (!weightedClearance) { terms.reject = "too_steep_for_wheels"; return kInf; }
                terms.clearanceTier = std::max(terms.clearanceTier, ClearanceTier(0, 9, grade - maxGrade));
            }
            inGrade = std::max(inGrade, (std::fabs(s.speed) - planspeed::SlopeLimit(s.speed > 0 ? k.maxSpeed : k.maxReverse, grade, maxGrade)) / k.brake);  // the speed the grade forces: time to slow to it
        }
        double station, lateral, half; routeAt(s.x, s.y, station, lateral, half);
        // FC-1: on the road = inside a field box OR inside the lane. The frame carries only the boxes near the car (three, ending 17 m on at
        // the 13:56 crest), and past them the route line itself read as 1-7 m outside the field, so the road ahead looked forbidden.
        {   double outside = w.FieldOutside(s.x, s.y);
            if (w.lanes && outside > 0) outside = std::min(outside, std::max(0.0, std::fabs(lateral) - half));
            inField = std::max(inField, outside); }
        double fcNow = kInf;
        if (field && field->ready) { const double fc = s.mode == 1 ? field->AtAnyHeading(s.x, s.y) : field->At(s.x, s.y, s.heading, s.speed); fcNow = fc; if (std::isfinite(fc)) { lastIn = fc; lastInStation = station; } }  // LK-1: hovering turns in place
        // D5: a route hold, stop line or the line's end is a wall across the road for forward motion: the front stays stopGap short of it,
        // and the speed always lets the car stop there (stopping energy).
        if (std::isfinite(w.stopStation) && s.speed > .05) {
            const double room = w.stopStation - (station + k.halfLength) - StopGap(k);
            if (room < 0 || s.speed * s.speed > 2 * k.brake * room + 1e-6) { terms.reject = "cannot_stop_before_the_line_end"; return kInf; }
        }
        // D4: moving road users where they will be at this step's time.
        {   bool closing = false; int which = -1;
            const double mc = w.MoverClearance(s.x, s.y, s.heading, band, k, step * kDt, std::sin(s.heading) * s.speed, std::cos(s.heading) * s.speed, closing, &which, s.mode == 0);
            minClear = std::min(minClear, mc);
            // The encounter is judged where it starts: a follower's straight-on prediction carries it through the car, and once its predicted
            // centre passes ours it would look like a car ahead (energyplan_test "D4 follower", first try: every plan ruled out 3 times).
            // A follower's contact costs once per rollout, not per second: per second, matching its speed stretched the predicted overlap and the
            // car slowed to 3 m/s to let a car behind "pass through" (energyplan_test follower, step 2).
            // Step 6: and only a follower whose path also reaches the car where it stands now (standing still would not avoid it); one the
            // rollout drives into is ours (shadowcompare_test goal scene: a crossing car met the behind test after the car turned its way).
            if (mc < 0) {
                const bool ours = !contactExcused && (closing ||
                    !w.MoverReaches(which, start.x, start.y, start.heading, bandFloor(start, z0), k, start.mode == 0));
                if (ours && !weightedClearance) { terms.reject = closing ? "closing_on_a_road_user" : "meets_a_road_user"; return kInf; }
                if (ours) terms.clearanceTier = std::max(terms.clearanceTier, ClearanceTier(30, 49, -mc));
                if (!ours) contactExcused = true;
            }
            { const double m = Margin(s.speed, k); if (mc < m) inMover = std::max(inMover, m - mc); }
        }
        if (w.lanes) { const double out = std::fabs(lateral) + k.halfWidth - half;  // D13: no lane, no lateral. The body inside its lane is free; only what is past the edge is priced
            if (out > 0 && !w.LaneEdgeFree(station)) {
                inLane = std::max(inLane, out);  // what is outside is paid as the time to creep back in
                if (out > laneOut0 + kNoGoTolerance) outsideLane = true; } }  // and the way gained outside it is not credited (below): time outside the lane is lost time  // JL-1: a car starting off its lane pays only for going further out  // LK-1: past the lane's edge (D1: free beside a static block in the lane)
        const double gained = onRoute && std::isfinite(station) ? (outsideLane ? 0.0 : station - prevStation) / kDt : (outsideLane ? 0.0 : s.speed); prevStation = std::isfinite(station) ? station : prevStation;
        if (!(fcNow < 1e-6)) terms.speed += kDt * std::max(0.0, 1.0 - gained / vw);  // only while there is way left to go: standing in the goal loses no time  // ST-A: the time lost to going slower than wanted (planspeed's `lost`)
    }
    double st1, lat1, half1; routeAt(s.x, s.y, st1, lat1, half1);
    terms.collision = inCollision / vCreep; terms.movers = inMover / vCreep; terms.soft = inSoft / vCreep; terms.lateral = inLane / vCreep;
    terms.field = inField / vCreep;
    if (w.lanes && std::isfinite(lat1)) terms.lateral += std::fabs(lat1) / vCreep;  // the route is the plan: the time to creep back onto it from where the rollout ends
 terms.lowEdge = std::max(0.0, inEdge); terms.slope = std::max(0.0, inGrade);
    // The way on past the horizon: the cost to go under this car's turning, where the field covers it; else plain route progress.
    // ED-2 (step 2; 20:15 run: the car crawled at 2.5-3 m/s everywhere): a fast rollout runs past the field's window, and an end outside it
    // paid togoCap, so only slow rollouts could win. Past the window, the road covered beyond the last state inside counts one for one
    // against that state's cost to go; togoCap only where no state of the rollout was inside.
    if (field && field->ready) {
        const double fc = s.mode == 1 ? field->AtAnyHeading(s.x, s.y) : field->At(s.x, s.y, s.heading, s.speed);  // LK-1: a hovering end turns in place
        terms.togo = (std::isfinite(fc) ? fc : wt.windowEdgeRule && std::isfinite(lastIn) ? std::max(0.0, lastIn - std::max(0.0, st1 - lastInStation)) : vw * kSteps * kDt) / vw;  // ST-A: seconds; no way on known pays the horizon time
        // LF-1: in a lane scene, way past the goal band's start always counts (every speed reached the band, so following the lane held 5.5-6
        // m/s where 11 was wanted). Ending in the band (cost 0): minus the way past its start. Ending past the window: the window-edge rule
        // without its floor at 0, from the last state inside (in the band: from the band's start). The field itself is unchanged (seeding it
        // bent it across a hairpin's barrier).
        if (w.lanes && std::isfinite(field->goalStation)) {
            const double past = std::max(0.0, st1 - field->goalStation);
            if (std::isfinite(fc)) { if (fc < 1e-6) terms.togo = -past / vw; }
            else if (wt.windowEdgeRule && std::isfinite(lastIn)) terms.togo = (lastIn < 1e-6 ? -past : lastIn - std::max(0.0, st1 - lastInStation)) / vw;
        }
    }
    else terms.progress = -(st1 - st0) / vw;
    // Stopping energy at the end: the car can stop inside the free distance from where the rollout ends.
    // HC-2 (2026-11-14 19:14 run: 3 m/s on a winding road lined by rocks): the free distance follows the curve the car is on at the end (braking
    // with the wheel held keeps that curvature), not straight on; straight on meets the verge of any bend within a few metres.
    if (std::fabs(s.speed) > .3) {
        const double dir = s.speed > 0 ? 1 : -1; double free = 0;
        const double moved = std::hypot(s.x - endFrom.x, s.y - endFrom.y);
        const double curve = moved > .05 ? std::clamp(std::remainder(s.heading - endTurn, 2 * 3.14159265358979323846) / moved, -1.0 / k.MinRadius(), 1.0 / k.MinRadius()) : 0.0;
        double x = s.x, y = s.y, h = s.heading, along = 0;
        // Step 2: stepped by the clearance found (a body this far from every solid can move that far without touching one), at least 0.5 m;
        // the arc is walked in 0.5 m pieces so a long step still bends with the road.
        // HV-2 (2026-11-22): the look-ahead is what the stopping rule needs, derived from this rollout's own end speed: the braking distance v^2 / 2b
        // plus the distance covered in the controller's reaction. It was a fixed 25 m (endReach), which capped every plan at sqrt(2 b 25) = 17 m/s on
        // an empty road, air or land (offline probe: 22 and 18 m/s rejected "cannot stop in the free distance" with the road clear to 700 m).
        const double reach = s.speed * s.speed / (2 * k.brake) + std::fabs(s.speed) * wheelguard::reaction + 1.0;
        for (double d = .5; d <= reach;) {
            while (along + 1e-9 < d) { const double piece = std::min(.5, d - along); h += curve * piece * .5; x += dir * piece * std::sin(h); y += dir * piece * std::cos(h); h += curve * piece * .5; along += piece; }
            double z; if (!w.Floor(x, y, z)) z = z0;
            const double cl = w.Clearance(x, y, h, s.mode == 0 ? z : bandFloor(s, z), k, s.mode == 0, inBand);  // a car still leaving a band is not stopped by it
            if (cl < 0) break;
            free = std::max(d, std::min(reach, d + cl) - StopGap(k)); d += std::max(.5, cl);  // HV-2: the clearance at d proves the body can move that far on, less the gap D5 keeps (the walk ends at reach, not before it)
        }
        terms.endFree = free;
        if (s.speed * s.speed > 2 * k.brake * free + 1e-6) {
            if (!weightedClearance) { terms.reject = "cannot_stop_in_the_free_distance_at_the_end"; return kInf; }
            terms.clearanceTier = std::max(terms.clearanceTier, ClearanceTier(0, 9, s.speed * s.speed / (2 * k.brake) - free));
        }
    }
    if (std::isfinite(w.facing)) {  // ST-A: the time to turn the nose: angle / yaw rate (hover: the gear row's turn at this speed; wheels: speed / smallest turn radius, at least a creep)
        const double yaw = s.mode == 1 ? k.HoverYawAt(std::fabs(s.speed)) : std::max(std::fabs(s.speed), k.brake * kDt) / k.MinRadius();
        terms.facing = w.facingScale * std::fabs(std::remainder(s.heading - w.facing, 2 * 3.14159265358979323846)) / yaw; }  // D7: the nose against the wanted facing (D13: scaled near the goal)
    if constexpr (std::is_same_v<W, planscene::Scene>) {
        if (wheelVote && start.mode == 0 && c.seg[0].mode == 0 && std::fabs(c.seg[0].speed) > .1) {
            const auto first = FirstControl(c, start, k, w, wt, mem, hint);
            wheelguard::Command command;
            command.steering = first.input;
            command.want = first.speed;
            command.vx = std::sin(start.heading) * start.speed;
            command.vy = std::cos(start.heading) * start.speed;
            command.constrained = minClear < .65 || !c.seg[0].follow || c.seg[0].offset != 0;
            const auto vote = wheelguard::Check(&w, start, k, command);
            terms.wheelSpeed = vote.speed;
            terms.wheelReason = vote.reason;
            const double demand = std::fabs(first.speed);
            const double deficit = demand - vote.speed;
            if (deficit > .05) {
                if (weightedClearance) {
                    const int tier = vote.speed < .1 ? 10 : ClearanceTier(0, 9, deficit);
                    terms.clearanceTier = std::max(terms.clearanceTier, tier);
                }
                terms.collision += deficit / k.brake;  // the time to slow by what the guard would not allow
            }
        }
    }
    return terms.hover + terms.modes + terms.facing + terms.movers + terms.soft + terms.togo + terms.progress + terms.lateral + terms.speed + terms.collision + terms.lowEdge + terms.slope + terms.field + terms.gear;
}

// HV-2 (2026-11-22, user: values come from a general scheme, not hand-tuning): the forward speeds every family offers, generated from the car's own
// physics instead of fixed fractions and constants. The top rung is the wanted speed capped by what the car can do in this mode. The rungs step
// down by what the car comfortably gains in one settle second (accel x kSettle), so neighbours are as far apart as the controller can tell apart
// before the next plan, whatever the car, mode or wanted speed. The bottom rung is the speed the brakes stop inside one plan step (brake x kDt):
// below it a speed is a stop, which is rung 0. notes/GENERATED_LIMITS_DESIGN_20261122.md.
// ST-C: derived from the commitment (it was a chosen 0.5). A challenger must keep winning for 2 x the switch time (2 reactions) before it takes
// over, so two speeds are different plans when the car gets from one to the other in that time: the step is accel x 2 x reaction. With the single
// switch time in place the result no longer depends on it (0.2 to 1.0 s gave the same hairpin and D1 outcomes; before, 0.36 rocked and 1.0 broke D1).
constexpr double kSettle = 2 * wheelguard::reaction;  // s
struct Rungs { double v[24]; int n = 0; };
inline Rungs SpeedRungs(double want, double top, double accel, double brake) {
    Rungs r; const double floorV = brake * kDt, step = std::max(accel * kSettle, floorV);  // no rung finer than the lowest one
    double v = std::min(std::max(want, floorV), std::max(top, floorV));
    while (r.n < 20 && v > floorV + .5 * step) { r.v[r.n++] = v; v -= step; }
    r.v[r.n++] = floorV; r.v[r.n++] = 0.0;
    return r;
}
inline Rungs WheelRungs(double want, const bicycle::Spec& k) { return SpeedRungs(want, k.maxSpeed, k.accel, k.brake); }
inline Rungs HoverRungs(double want, const bicycle::Spec& k) { return SpeedRungs(want, k.hoverTop, k.hoverAccel, k.brake); }
// Step 3 (D6/D7): the hover family. Hover arcs at several speeds, hover then land, and (with a facing to keep) strafing: travel off the nose
// while the nose turns toward the facing. Only offered where the car can hover.
inline void AddHover(Candidate* out, int& n, const bicycle::Spec& k, double want, bool facing, double heading = std::numeric_limits<double>::quiet_NaN()) {
    auto add = [&](std::initializer_list<Segment> segs, const char* kind) {
        if (n >= kMaxCandidates) return; Candidate c; c.kind = kind; for (const auto& g : segs) if (c.count < kMaxSegments) c.seg[c.count++] = g; out[n++] = c; };
    const double H = kSteps * kDt, w = std::max(1.0, want);
    static constexpr double in5[] = { -1, -.5, 0, .5, 1 };
    static constexpr double in3[] = { -1, 0, 1 };
    const Rungs hv = HoverRungs(want, k);  // HV-2: generated rungs (replaces HV-1's hand list)
    for (double a : in5) for (int q = 0; q < hv.n; ++q) add({ { a, hv.v[q], H, 1 } }, hv.v[q] > 0 ? "hover" : "hover in place");
    // Landing after 1-4 s (2 s steps let a car stay up 2 s longer than it needed: energyplan_test drive-only barrier, step 3).
    for (double a : in3) for (double v : { w * .5, 3.0 }) for (double t : { 1.0, 2.0, 3.0, 4.0 }) add({ { a, v, t, 1 }, { a, v, H - t, 0 } }, "hover then land");
    // Strafe: turn the nose toward the facing for t s (travel held about straight by half the angle), then hold the nose and slide at the
    // full angle. (Constant-input strafes either spun the nose or never turned it: energyplan_test D7, step 3.)
    if (facing) for (double turn : { -1.0, 1.0 }) for (double t : { .5, 1.0 }) for (double v : { w, w * .5 }) {  // full speed too (half speed only lost to a straight hover)
        const double angle = turn * k.HoverYawAt(v) * t;  // the nose turns the hover turn at this speed x t (FC-1: the gear row's, as HoverStep)
        add({ { turn, v, t, 1, -angle * .5 }, { 0, v, H - t, 1, -angle } }, "hover strafe");
    }
    if (facing) for (double sf : { -1.5708, 1.5708 }) for (double v : { w, w * .5 }) add({ { 0, v, H, 1, sf } }, "hover strafe");
    // LK-1: turn the nose while the travel stays exactly on its present direction (the strafe follows the nose), then hold. The shapes
    // above hold the travel only roughly and wander 3-7 m sideways; with the lane edge priced, the facing needs these.
    if (facing && std::isfinite(heading)) for (double turn : { -1.0, 1.0 }) for (double t : { .5, 1.0, 1.5 }) for (double v : { w, w * .5 }) {
        Segment a{ turn, v, t, 1, 0 }; a.travel = heading; Segment b{ 0, v, H - t, 1, 0 }; b.travel = heading;
        add({ a, b }, "hover strafe, travel held");
    }
}
// LF-1: lane following at the lane speed and below, on the lane line and 1 m either side; with a stop ahead, follow then stop.
inline void AddFollow(Candidate* out, int& n, double want, bool stopAhead) {
    const double H = kSteps * kDt, w = std::max(1.0, want);
    auto add = [&](std::initializer_list<Segment> segs, const char* kind) {
        if (n >= kMaxCandidates) return; Candidate c; c.kind = kind; for (const auto& g : segs) if (c.count < kMaxSegments) c.seg[c.count++] = g; out[n++] = c; };
    auto follow = [](double v, double t, double off) { Segment g{ 0, v, t }; g.follow = true; g.offset = off; return g; };
    for (double v : { w, w * .75, w * .5, 6.0, 3.0 }) for (double off : { 0.0, -1.0, 1.0 }) add({ follow(v, H, off) }, "follow lane");
    if (stopAhead) for (double v : { w, w * .5, 3.0 }) for (double t : { 2.0, 4.0 }) add({ follow(v, t, 0), follow(0, H - t, 0) }, "follow then stop");
}
// The candidate set (step 2): coarse spread over every manoeuvre family, then Plan refines around the best few (Refine).
// Predict the held command only to the leg's own end, then score its stopping path.
inline Candidate KTurnLeg(const bicycle::State& start, const bicycle::Spec& spec, const Weights& weights,
                         double laneHeading, recovery::Command::Step step, double steer, double metres,
                         const recovery::Rules& rules, bool& faces) {
    const bool reverse = step == recovery::Command::Step::BackUp;
    const double speed = reverse ? -rules.backUpSpeed : rules.turnSpeed;
    constexpr double horizon = kSteps * kDt;
    constexpr double tau = 6.28318530717958647692;
    Candidate leg;
    leg.kind = reverse ? "recover: back up" : "recover: turn to the lane";
    leg.seg[0] = {steer, speed, 0};
    const auto control = Command(leg.seg[0], spec, weights);
    auto pose = start;
    double distance = 0;
    int steps = 0;
    const bool offHeading = std::fabs(std::remainder(start.heading - laneHeading, tau)) >= rules.turnDone;
    faces = false;
    while (steps < kSteps && distance < metres && !faces) {
        const auto next = bicycle::Step(pose, control, spec, kDt);
        distance += std::hypot(next.x - pose.x, next.y - pose.y);
        pose = next;
        ++steps;
        faces = offHeading && std::fabs(std::remainder(pose.heading - laneHeading, tau)) < rules.turnDone;
    }
    leg.seg[0].seconds = std::max(1, steps) * kDt;
    leg.count = steps < kSteps ? 2 : 1;
    leg.seg[1] = {0, 0, horizon - leg.seg[0].seconds};
    return leg;
}

// ST-D (2026-11-22, notes/GENERATED_COSTS_DESIGN_20261122.md): the manoeuvre library's speeds and lengths come from the car, not from constants.
//   R      the tightest turn the car makes (Spec::MinRadius); a three-point turn is two of them, so every leg is a fraction or multiple of it.
//   vBend  the speed full lock holds: sqrt(a_lat R) with a_lat the sideways acceleration the plan counts on (carcap PlanLat), never above the wanted speed.
//   vRev   the reverse speed: what the brakes stop inside one turn radius, sqrt(2 b R), never above the car's reverse top.
//   vCreep the lowest rung (the speed the brakes stop inside one plan step): the slow leg speed of a turn on the spot.
struct Manoeuvre {
    double R, vBend, vRev, vCreep;
    double Time(double radii, double v) const { return std::clamp(radii * R / std::max(v, .1), kDt, kSteps * kDt - kDt); }  // s to cover `radii` turn radii at v
};
inline Manoeuvre ManoeuvreFor(const bicycle::Spec& k, double want) {
    Manoeuvre m; m.R = k.MinRadius();
    m.vBend = std::min(std::max(1.0, want), std::sqrt(carcap::kCar.PlanLat(1.0) * m.R));
    m.vRev = std::min(k.maxReverse, std::sqrt(2 * k.brake * m.R));
    m.vCreep = CreepSpeed(k);
    return m;
}

inline int Candidates(const bicycle::State& s, const bicycle::Spec& k, double want, const Memory* mem, double now, Candidate* out, bool stopAhead = false, bool hoverOK = false, bool facing = false, bool lane = false) {
    // Every manoeuvre family the full set had, at its speeds and steering, with fewer duration steps (about 230 instead of 450). A first
    // coarse set of 110 left out the slow half-lock forward-reverse-forward and reverse-into-turn shapes, and the 19:51 recorded hairpin,
    // which needs exactly those, stood still (0.55 m); the full set threads it (35 m).
    int n = 0;
    auto add = [&](std::initializer_list<Segment> segs, const char* kind) {
        if (n >= kMaxCandidates) return; Candidate c; c.kind = kind; for (const auto& g : segs) if (c.count < kMaxSegments) c.seg[c.count++] = g; out[n++] = c; };
    const double H = kSteps * kDt, w = std::max(1.0, want);
    static constexpr double in5[] = { -1, -.5, 0, .5, 1 };
    static constexpr double in3[] = { -1, 0, 1 };
    { const Rungs wr = WheelRungs(want, k); for (double a : in5) for (int q = 0; q < wr.n; ++q) add({ { a, wr.v[q], H } }, wr.v[q] > 0 ? "forward arc" : "stop"); }  // HV-2: generated rungs
    const Manoeuvre mv = ManoeuvreFor(k, want);
    // ST-D finding: the plain bends keep w * .6 and 2.5 m/s. Derived values (sqrt(a_lat R) = 3.97 m/s, geometric mid-speed, legs of 1 and 3 R) fail
    // energyplan_test D1 (static block: no dodge) and 'rock in the lane': 2.5 m/s is a sideways acceleration of about 1.2 m/s^2 on this car (.4 of PlanLat), a
    // gentler manoeuvring turn than the grip limit, and no rule for it has been found (reported, not hidden).
    for (double a : in5) for (double b : in5) for (double tt : { 1.0, 2.5 }) for (double v : { w * .6, 2.5 }) add({ { a, v, tt }, { b, v, H - tt } }, "forward bend");
    for (double r : in5) for (double m : { .5, 1.0 }) for (double f : in3) add({ { r, -mv.vRev, mv.Time(m, mv.vRev) }, { f, mv.vBend, H - mv.Time(m, mv.vRev) } }, "reverse then forward");
    for (double r : in5) add({ { r, -mv.vRev, H } }, "reverse");
    // D5: drive, then stop, only where a stop is ahead (elsewhere a standing plan is kept by commitment: 19:51 hairpin fell to 4.75 m).
    if (stopAhead) for (double a : in5) for (double v : { w, w * .5, 3.0, 1.5 }) for (double t : { 1.0, 2.5, 4.0 }) add({ { a, v, t }, { a, 0.0, H - t } }, "drive then stop");
    for (double turn : { -1.0, 1.0 }) for (double f : { .5, 1.0 }) for (double a : { .25, .5 }) for (double b : { .25, .5, 1.0 }) for (double v : { mv.vCreep, mv.vRev })
        add({ { turn * f, v, mv.Time(a, v) }, { -turn, -v, mv.Time(b, v) }, { turn, v, H - mv.Time(a, v) - mv.Time(b, v) } }, "forward reverse forward");
    for (double turn : { -1.0, 1.0 }) for (double m : { .25, .5, 1.0, 1.5 }) for (double v : { mv.vCreep, mv.vRev }) add({ { -turn, -v, mv.Time(m, v) }, { turn, v, H - mv.Time(m, v) } }, "reverse into turn");
    if (hoverOK) AddHover(out, n, k, want, facing, s.heading);  // step 3 (LK-1: travel-held strafes from the present heading)
    if (lane) AddFollow(out, n, want, stopAhead);  // LF-1
    if (mem && mem->have && mem->best.count) {
        Candidate c = mem->best; double gone = std::max(0.0, now - mem->at);
        for (int i = 0; i < c.count && gone > 0; ++i) { const double d = std::min(gone, c.seg[i].seconds - kDt * .5); c.seg[i].seconds -= std::max(0.0, d); gone -= std::max(0.0, d); }
        // O1 (2026-11-14): a segment whose time is used up is dropped, not kept at half a step (kept again and again, a "turn the nose, then
        // hold it" plan kept turning: energyplan_test D7 spun once the committed plan was held).
        { int f = 0; while (f < c.count - 1 && c.seg[f].seconds <= kDt * .5 + 1e-9) ++f; if (f) { for (int i = f; i < c.count; ++i) c.seg[i - f] = c.seg[i]; c.count -= f; } }
        c.seg[c.count - 1].seconds += H;  // the last segment carries on to the horizon
        c.kind = "kept"; if (n < kMaxCandidates) out[n++] = c;
        for (double da : { -.2, .2 }) for (int i = 0; i < c.count && n < kMaxCandidates; ++i) { Candidate d = c; d.seg[i].input = std::clamp(d.seg[i].input + da, -1.0, 1.0); d.kind = "kept, steered"; out[n++] = d; }
    }
    (void)s;
    return n;
}
// The full set (before step 2; about 450 rollouts), kept as the diagnostic reference (Weights::fullSet) for plan_sim comparisons.
inline int CandidatesFull(const bicycle::State& s, const bicycle::Spec& k, double want, const Memory* mem, double now, Candidate* out, bool stopAhead = false, bool hoverOK = false, bool facing = false, bool lane = false) {
    int n = 0;
    auto add = [&](std::initializer_list<Segment> segs, const char* kind) {
        if (n >= kMaxCandidates) return; Candidate c; c.kind = kind; for (const auto& g : segs) if (c.count < kMaxSegments) c.seg[c.count++] = g; out[n++] = c; };
    const double H = kSteps * kDt, w = std::max(1.0, want);
    static constexpr double in7[] = { -1, -.66, -.33, 0, .33, .66, 1 };
    static constexpr double in5[] = { -1, -.5, 0, .5, 1 };
    { const Rungs wr = WheelRungs(want, k); for (double a : in7) for (int q = 0; q < wr.n; ++q) add({ { a, wr.v[q], H } }, wr.v[q] > 0 ? "forward arc" : "stop"); }  // HV-2
    const Manoeuvre mv = ManoeuvreFor(k, want);
    // ST-D: the plain bends keep w * .6 and 2.5 m/s (see Candidates: derived values fail D1 and 'rock in the lane').
    for (double a : in7) for (double b : in5) for (double tt : { 1.0, 2.5 }) for (double v : { w * .6, 2.5 }) add({ { a, v, tt }, { b, v, H - tt } }, "forward bend");
    for (double r : in5) for (double m : { .5, 1.0 }) for (double f : in5) add({ { r, -mv.vRev, mv.Time(m, mv.vRev) }, { f, mv.vBend, H - mv.Time(m, mv.vRev) } }, "reverse then forward");
    for (double r : in5) add({ { r, -mv.vRev, H } }, "reverse");
    // D5 (one-authority step 1): drive, then stop. Without these, every candidate holds one speed for the whole horizon, so near a stop line
    // only a crawl fits the room left and the car never arrives (energyplan_test "D5 stop line", first try: 9.5 m short after 15 s).
    // Only where there is a stop to arrive at: elsewhere a plan that ends standing is kept by the commitment term and the car settles in front of
    // what it should get round (19:51 recorded hairpin: 35 m reached before, 4.75 m with these offered everywhere).
    if (stopAhead) for (double a : in5) for (double v : { w, w * .5, 6.0, 3.0, 1.5 }) for (double t : { 1.0, 2.0, 3.0, 4.0 }) add({ { a, v, t }, { a, 0.0, H - t } }, "drive then stop");
    for (double turn : { -1.0, 1.0 }) for (double f : { .5, 1.0 }) for (double a : { .25, .5, 1.0 })
        for (double b : { .25, .5, 1.0 }) for (double v : { mv.vCreep, mv.vRev })
            add({ { turn * f, v, mv.Time(a, v) }, { -turn, -v, mv.Time(b, v) }, { turn, v, H - mv.Time(a, v) - mv.Time(b, v) } }, "forward reverse forward");
    for (double turn : { -1.0, 1.0 }) for (double m : { .25, .5, 1.0, 1.5, 2.0 })
        for (double v : { mv.vCreep, mv.vRev, mv.vBend })
            add({ { -turn, -v, mv.Time(m, v) }, { turn, v, H - mv.Time(m, v) } }, "reverse into turn");
    if (hoverOK) AddHover(out, n, k, want, facing, s.heading);  // step 3 (LK-1: travel-held strafes from the present heading)
    if (lane) AddFollow(out, n, want, stopAhead);  // LF-1
    if (mem && mem->have && mem->best.count) {
        Candidate c = mem->best; double gone = std::max(0.0, now - mem->at);
        for (int i = 0; i < c.count && gone > 0; ++i) { const double d = std::min(gone, c.seg[i].seconds - kDt * .5); c.seg[i].seconds -= std::max(0.0, d); gone -= std::max(0.0, d); }
        // O1 (2026-11-14): a segment whose time is used up is dropped, not kept at half a step (kept again and again, a "turn the nose, then
        // hold it" plan kept turning: energyplan_test D7 spun once the committed plan was held).
        { int f = 0; while (f < c.count - 1 && c.seg[f].seconds <= kDt * .5 + 1e-9) ++f; if (f) { for (int i = f; i < c.count; ++i) c.seg[i - f] = c.seg[i]; c.count -= f; } }
        c.seg[c.count - 1].seconds += H;  // the last segment carries on to the horizon
        c.kind = "kept"; if (n < kMaxCandidates) out[n++] = c;
        for (double da : { -.2, .2 }) for (int i = 0; i < c.count && n < kMaxCandidates; ++i) { Candidate d = c; d.seg[i].input = std::clamp(d.seg[i].input + da, -1.0, 1.0); d.kind = "kept, steered"; out[n++] = d; }
    }
    (void)s;
    return n;
}

// Step 2: variations of a good candidate: each segment's steering +-0.25, its speed x0.75 / x1.25, and the first segment's length +-0.6 s.
inline int Refine(const Candidate& c, Candidate* out, int room) {
    int n = 0;
    auto put = [&](const Candidate& d) { if (n < room) { out[n] = d; out[n].kind = c.kind; ++n; } };
    for (int i = 0; i < c.count; ++i) {
        for (double da : { -.25, .25 }) { Candidate d = c; d.seg[i].input = std::clamp(d.seg[i].input + da, -1.0, 1.0); put(d); }
        if (std::fabs(c.seg[i].speed) > .05) for (double f : { .75, 1.25 }) { Candidate d = c; d.seg[i].speed *= f; put(d); }
    }
    if (c.count > 1) for (double dt : { -.6, .6 }) { Candidate d = c; const double t = std::clamp(d.seg[0].seconds + dt, kDt, kSteps * kDt - kDt); d.seg[1].seconds += d.seg[0].seconds - t; d.seg[0].seconds = t; put(d); }
    return n;
}

// Step 2: how the candidates of one plan are scored: run fn(i) for every i in [0, n), in any order or in parallel. Each candidate is scored
// into its own slot and the winner is picked afterwards in index order, so a parallel run chooses exactly what a serial one does.
using ForEach = std::function<void(int n, const std::function<void(int)>& fn)>;
// D12 (one-authority step 4): the health check's spot test is a request to the Plan, not a writer of its own. The pulse (a short push at
// `speed` m/s, steering `input`, for `seconds`, then standing) is scored by the same energy as every candidate. Finite: it is this update's
// command and the committed plan (Memory) is left as it was, so driving resumes it after the pulse. Infinite (it would enter a solid,
// cross the stop wall or meet a road user): refused, and the plan drives as if nothing had been asked.
struct TestPulse { bool active = false; double input = 0, speed = 1.0, seconds = .25; };
inline void Serial(int n, const std::function<void(int)>& fn) { for (int i = 0; i < n; ++i) fn(i); }

template <class W>
inline Result Plan(const bicycle::State& s, const bicycle::Spec& k, double want, const W& w, Memory& mem, double now, const Weights& wtIn = {}, double hint = std::nan(""),
                   const planlattice::Field* field = nullptr, const ForEach& forEach = Serial, const TestPulse* pulse = nullptr,
                   bool floorPointCleared = false, bool traceCentreBlock =
#ifdef FVO_RELEASE
                       false
#else
                       true
#endif
                   ) {
    Result r;
    Weights wt = wtIn;
    if (pulse && pulse->active) {  // D12
        Candidate c; c.kind = "spot test pulse"; c.count = 2;
        c.seg[0] = { pulse->input, pulse->speed, std::clamp(pulse->seconds, kDt, kSteps * kDt - kDt) };
        c.seg[1] = { pulse->input, 0.0, kSteps * kDt - c.seg[0].seconds };
        Terms t; double cl;
        if (std::isfinite(Score(c, s, k, want, w, wt, nullptr, now, t, r.traj, cl, hint, field))) {
            r.ok = true; r.pulse = true; r.best = c; r.why = c.kind; r.terms = t; r.minClearance = cl; r.candidates = r.feasible = 1;
            r.energy = t.hover + t.modes + t.facing + t.movers + t.soft + t.togo + t.progress + t.lateral + t.speed + t.collision + t.lowEdge + t.slope + t.field + t.gear;
            r.first = Command(c.seg[0], k, wt); r.firstMode = 0; r.firstHeight = r.traj[std::min(kSteps, 5)].z;
            return r;  // memory untouched: the committed plan resumes after the pulse
        }
        r.testRefused = true;
    }
    static thread_local Candidate cands[kMaxCandidates];
    const bool hoverOK = w.hoverPossible && w.travel != 1, facing = std::isfinite(w.facing), lane = w.lanes && w.route.size() >= 2;  // LF-1
    bool weighted = false;
    if constexpr (requires { w.continuousRecovery; w.managerHeld; }) {
        weighted = w.continuousRecovery && !w.managerHeld;
    }
    int n = wt.fullSet ? CandidatesFull(s, k, want, &mem, now, cands, std::isfinite(w.stopStation), hoverOK, facing, lane) : Candidates(s, k, want, &mem, now, cands, std::isfinite(w.stopStation), hoverOK, facing, lane); r.candidates = n;
    auto addPrimitive = [&](const char* name, double steering, double speed, bool retrace, int mode = 0) {
        if (n >= kMaxCandidates) return;
        Candidate c; c.count = 1; c.kind = name;
        c.seg[0] = {steering, speed, kSteps * kDt, mode};
        c.seg[0].retrace = retrace;
        cands[n++] = c;
    };
    if (weighted) {
        if (w.travel != 2) for (double steer : {-1., 0., 1.}) {
            addPrimitive("primitive: crawl forward", steer, .6, false);
            addPrimitive("primitive: crawl reverse", steer, -.6, false);
        }
        if (hoverOK) for (double steer : {-1., 0., 1.})
            addPrimitive("primitive: hover crawl", steer, .6, false, 1);
        const auto& last = mem.trail.points[std::max(0, mem.trail.count - 1)];
        if (s.mode == 0 && w.travel != 2 && mem.trail.count && mem.trail.seen >= 0 &&
            now >= mem.trail.seen && now - mem.trail.seen <= 2.0 &&
            std::fabs(s.z - last.z) <= 3.0 && std::hypot(s.x - last.x, s.y - last.y) <= 6.0) {
            if (mem.trail.target >= 0) {
                const auto& target = mem.trail.points[mem.trail.target];
                if (std::hypot(s.x - target.x, s.y - target.y) <= .55)
                    mem.trail.Note(s, 0, false, now);
            }
            if (mem.trail.target < 0) mem.trail.target = mem.trail.FindTarget(s);
            // AP-1 (2026-11-21): retrace is off. It backed straight down the way the car came, so the car pointed at the same
            // obstacle again (17:36 run: ~430 retrace picks, cars rocking a metre back and forth). The held routine reverses instead.
            constexpr bool kRetrace = false;
            if (kRetrace && mem.trail.target >= 0)
                addPrimitive("recover: retrace travelled path", 0., -1.2, true);
        } else {
            mem.trail.target = -1;
        }
        r.candidates = n;
    }
    int best = -1; Terms terms; double clear;
    static thread_local double energies[kMaxCandidates];
    static thread_local int clearanceTiers[kMaxCandidates];
    static thread_local const char* rejects[kMaxCandidates];  // CT-1: the rule that rejected each candidate (nullptr: feasible)
    auto centre = [](const Candidate& c) {
        return c.count && c.seg[0].follow && std::fabs(c.seg[0].offset) <= 1e-6;
    };
    bool preferRecovery = false;
    bool trailSwept = false;
    unsigned trailOptions = 0;
    auto rank = [&](const Candidate& c, int tier, double raw) {
        if (!weighted || !std::isfinite(raw)) return raw;
        const auto& first = c.seg[0];
        if (preferRecovery && first.retrace) raw -= 120.;
        if (!preferRecovery && first.retrace) raw += 50.;
        if (!preferRecovery && centre(c)) raw -= 4.;
        return WeightedEnergy(tier, raw);
    };
    {   Candidate* cs = cands; double* es = energies; int* tiers = clearanceTiers; const char** rj = rejects;
        forEach(n, [&](int i) {
            Terms t; double cl;
            es[i] = Score(cs[i], s, k, want, w, wt, &mem, now, t, nullptr, cl, hint, field, nullptr, weighted, true);
            tiers[i] = t.clearanceTier;
            rj[i] = std::isfinite(es[i]) ? nullptr : t.reject;
        });
    }
    // RV-1 (recovery.h; 2026-11-19, user: "propose the optimal node system and then just replace entire thing at once"): the mode for this
    // tick from what the scoring found. Follow: only lane-centre plans may challenge (the committed plan stays; the Arbiter's hold decides).
    // Avoid: every candidate. Recover: the routine's held step (back up straight, turn to the lane, retry) or hover where allowed. Stand:
    // nothing. REPLACES the SL-1 ladder weights, RH-1 ladder timers and the LD-1 layer flag.
    recovery::Command cmd;
    bool heldStepChosen = false;  // PF-2: the routine's held step is this tick's plan
    {
        bool anyCentre = false, anyFeasible = false, anyMoving = false, reverseFeasible = false, anyForwardClear = false; int full = -1; unsigned options = 0;
        for (int i = 0; i < n; ++i) {
            if (!weighted && cands[i].count && cands[i].seg[0].retrace) {
                energies[i] = kInf; rejects[i] = "retrace_rebuild";
                continue;
            }
            const bool ok = std::isfinite(energies[i]) && (!weighted || clearanceTiers[i] == 0);
            if (ok) anyFeasible = true; if (ok && cands[i].count && cands[i].seg[0].speed < -.05) reverseFeasible = true;
            if (ok && cands[i].count && cands[i].seg[0].speed > .1) anyForwardClear = true;
            if (ok && cands[i].count && std::fabs(cands[i].seg[0].speed) > .1) {
                anyMoving = true;
                const auto& g = cands[i].seg[0];
                const double lateral = g.follow ? g.offset : g.input;
                const int side = lateral < -.08 ? 0 : lateral > .08 ? 2 : 1;
                options |= 1u << (g.mode == 1 ? 6 : (g.speed < 0 ? 3 : 0) + side);
            }
            if (!centre(cands[i]) || std::strcmp(cands[i].kind, "follow lane") != 0) continue;
            if (ok) anyCentre = true;
            if (full < 0 || cands[i].seg[0].speed > cands[full].seg[0].speed) full = i; }
        const bool fullSolid = full >= 0 && (weighted ? clearanceTiers[full] >= 10 :
            !std::isfinite(energies[full]) && rejects[full] && std::strcmp(rejects[full], "enters_a_solid") == 0);
        if (fullSolid && traceCentreBlock) {
            Terms traceTerms; double traceClearance;
            Score(cands[full], s, k, want, w, wt, &mem, now, traceTerms, nullptr, traceClearance, hint, field, &r.centreBlock, weighted);
        }
        // AP-1: in weighted mode a road user does not reject the plan, it raises its tier to 30..49 (only movers use those tiers).
        const bool waitingForUser = full >= 0 && ((!std::isfinite(energies[full]) && rejects[full] && std::strstr(rejects[full], "road_user")) ||
                                                  (weighted && std::isfinite(energies[full]) && clearanceTiers[full] >= 30));  // queued behind traffic: waiting, not stuck
        double st = std::numeric_limits<double>::quiet_NaN(), lat = 0, half = 0; if (lane) w.Route(s.x, s.y, st, lat, half, hint);
        // Progress since the last plan, both positions in this snapshot (route stations restart with every snapshot: LR-1); a plan not run
        // for 2 s (Cruise off and on) starts the mode afresh.
        double progress = 0;
        if (mem.recSeen >= 0 && now >= mem.recSeen && now - mem.recSeen <= 2.0) {
            if (lane && std::isfinite(st)) { double sa = std::numeric_limits<double>::quiet_NaN(), la = 0, ha = 0; w.Route(mem.progX, mem.progY, sa, la, ha, hint); if (std::isfinite(sa)) progress = st - sa; }
            else if (w.goal.have) progress = std::hypot(mem.progX - w.goal.x, mem.progY - w.goal.y) - std::hypot(s.x - w.goal.x, s.y - w.goal.y);  // goal driving: how much closer to the goal (turning round is progress)
            else progress = (s.x - mem.progX) * std::sin(s.heading) + (s.y - mem.progY) * std::cos(s.heading);
        } else mem.rec = recovery::State{};
        mem.recSeen = now; mem.progX = s.x; mem.progY = s.y;
        double laneHeading = s.heading;
        if (lane && std::isfinite(st)) { std::size_t q = 1; while (q + 1 < w.route.size() && w.route[q].station < st) ++q;
            laneHeading = std::atan2(w.route[q].x - w.route[q - 1].x, w.route[q].y - w.route[q - 1].y); }
        recovery::Input in; in.now = now; in.haveLane = lane; in.flightStretch = w.travel == 2; in.centreFeasible = anyCentre;
        in.floorRetry = floorPointCleared && full >= 0 && std::isfinite(energies[full]);
        in.centreAtWantedSpeedBlocked = fullSolid; in.anyFeasible = anyFeasible; in.reverseFeasible = reverseFeasible; in.progress = progress;
        {   double gz0 = 0, gz1 = 0; constexpr double ahead = 4.0;  // SP-1: the road's grade just ahead along the nose
            if (w.Floor(s.x, s.y, gz0) && w.Floor(s.x + std::sin(s.heading) * ahead, s.y + std::cos(s.heading) * ahead, gz1)) in.roadGrade = (gz1 - gz0) / ahead; }
        in.speed = s.speed; in.headingErrToLane = std::remainder(s.heading - laneHeading, 2 * 3.14159265358979323846); in.hoverAllowed = hoverOK;
        in.meantToStand = want < .5 || (std::isfinite(w.stopStation) && std::isfinite(st) && w.stopStation - st < 10.0) || waitingForUser;
        bool managed = false, trafficHold = false, swept = false, queuedWait = false;
        if constexpr (requires { w.continuousRecovery; w.managerHeld; w.checkedPath; }) {
            managed = w.continuousRecovery;
            trafficHold = w.managerHeld || (std::isfinite(w.stopStation) && std::isfinite(st) &&
                w.stopStation - st <= std::max(0.0, k.halfLength + .8));
            for (const auto& span : w.checkedPath) {
                if (now < span.at || now - span.at > 12 || std::fabs(s.z - span.z1) > 2) continue;
                const double dx = span.x1 - span.x0, dy = span.y1 - span.y0;
                const double t = std::clamp(((s.x-span.x0)*dx + (s.y-span.y0)*dy) /
                                           std::max(.001, dx*dx + dy*dy), 0.0, 1.0);
                if (std::hypot(s.x-span.x0-t*dx, s.y-span.y0-t*dy) <= .75) { swept = true; break; }
            }
        }
        if (managed) {
            if (weighted) { trailOptions = options; trailSwept = swept; }
            else mem.trail.Note(s, options, swept, now);
            if (trafficHold) mem.trail.target = -1;
            in.continuous = true;
            // AP-1: queued behind a road user is waiting, not stuck (17:36 run: followers of a stuck car started recovering and
            // made the jam worse). After kQueuePatience s the queue counts as stuck, so a car behind a dead one can still get round.
            constexpr double kQueuePatience = 20.0;
            if (waitingForUser && !trafficHold) { if (mem.queuedSince < 0 || now < mem.queuedSince) mem.queuedSince = now; } else mem.queuedSince = -1;
            queuedWait = waitingForUser && !trafficHold && now - mem.queuedSince <= kQueuePatience;
            in.meantToStand = trafficHold || queuedWait;
            in.anyFeasible = anyMoving;
        }
        using Step = recovery::Command::Step;
        const recovery::Rules rules;
        const bool checkLegs = lane && s.mode == 0 && w.travel != 2;
        const bool highAngle = recovery::HighAngleTurn(mem.rec, in.headingErrToLane, rules);
        struct Leg { Candidate candidate; Terms terms; double energy = kInf; bool clear = false, faces = false; };
        // A short recovery leg must clear the whole body between ordinary rollout samples,
        // including its stop. Ordinary lane scores keep their existing band tolerance.
        const bool checkBody = checkLegs && highAngle && w.Clearance(s.x, s.y, s.heading, s.z, k, true) >= 0;
        auto clearsRecoveryBody = [&](const Candidate& candidate) {
            constexpr double sampleDt = .05;
            auto pose = s;
            for (int i = 0; i < candidate.count; ++i) {
                const auto& segment = candidate.seg[i];
                const auto control = Command(segment, k, wt);
                const int samples = static_cast<int>(std::lround(segment.seconds / sampleDt));
                for (int tick = 0; tick < samples; ++tick) {
                    if (std::fabs(pose.speed) < 1e-9 && control.speed == 0) break;
                    pose = bicycle::Step(pose, control, k, sampleDt);
                    double floor = pose.z;
                    if (w.Floor(pose.x, pose.y, floor)) pose.z = floor;
                    if (w.Clearance(pose.x, pose.y, pose.heading, pose.z, k, true) < 0) return false;
                }
            }
            return true;
        };
        auto predictLeg = [&](Step step, double steer, double remaining) {
            Leg leg;
            auto assess = [&](double length) {
                leg.candidate = KTurnLeg(s, k, wt, laneHeading, step, steer, length, rules, leg.faces);
                double clearance;
                leg.energy = Score(leg.candidate, s, k, want, w, wt, &mem, now, leg.terms, nullptr, clearance, hint, field, nullptr, weighted, true);
                leg.clear = std::isfinite(leg.energy) && (!weighted || leg.terms.clearanceTier < 10) &&
                    (!checkBody || clearsRecoveryBody(leg.candidate));
            };
            assess(remaining);
            if (highAngle && !leg.clear) {
                for (const double length : rules.kturnPrefixes) {
                    if (length >= remaining) continue;
                    assess(length);
                    if (leg.clear) break;
                }
            }
            return leg;
        };
        const double backMetres = recovery::BackUpMetres(mem.rec, in.headingErrToLane, rules);
        const double backRemaining = mem.rec.step == Step::BackUp
            ? std::max(0.0, backMetres - mem.rec.backed) : backMetres;
        const double forwardMetres = rules.turnSpeed * rules.turnMax;
        Leg forward, reverse;
        in.wheels = s.mode == 0;
        in.forwardBlocked = !anyForwardClear;
        if (checkLegs) {
            forward = predictLeg(Step::TurnToLane, recovery::LegSteer(mem.rec, in.headingErrToLane, Step::TurnToLane, rules), forwardMetres);
            reverse = predictLeg(Step::BackUp, recovery::LegSteer(mem.rec, in.headingErrToLane, Step::BackUp, rules), backRemaining);
            in.legForwardClear = forward.clear;
            in.legForwardFaces = forward.faces;
            in.legReverseClear = reverse.clear;
            if (highAngle) in.reverseFeasible = reverse.clear;
        }
        cmd = recovery::Update(mem.rec, in, rules);
        r.laneErr = in.headingErrToLane; r.legF = forward.clear; r.legFaces = forward.faces; r.legR = reverse.clear;
        r.recTries = mem.rec.tries; r.recCycles = mem.rec.cycles; r.turnSign = mem.rec.turnSign;
        const bool heldLeg = cmd.mode == recovery::Mode::Recover &&
            (cmd.step == Step::BackUp || cmd.step == Step::TurnToLane);
        auto commandedLeg = [&] {
            Leg leg;
            if (heldLeg && checkLegs) {
                leg = cmd.step == Step::BackUp ? reverse : forward;
                const double remaining = cmd.step == Step::BackUp ? std::max(0.0, recovery::BackUpMetres(mem.rec, in.headingErrToLane, rules) - mem.rec.backed) : forwardMetres;
                if (leg.candidate.seg[0].input != cmd.steer || (cmd.step == Step::BackUp && remaining > backRemaining))
                    leg = predictLeg(cmd.step, cmd.steer, remaining);
            } else {
                leg.candidate.count = 1;
                leg.candidate.seg[0] = {cmd.steer, cmd.wantSpeed, kSteps * kDt};
                leg.candidate.kind = cmd.step == Step::BackUp ? "recover: back up" :
                    cmd.step == Step::TurnToLane ? "recover: turn to the lane" : "recover: retry";
                double clearance;
                leg.energy = Score(leg.candidate, s, k, want, w, wt, &mem, now, leg.terms, nullptr, clearance, hint, field, nullptr, weighted, true);
                leg.clear = std::isfinite(leg.energy) && (!weighted || leg.terms.clearanceTier < 10);
            }
            return leg;
        };
        r.layer = cmd.mode == recovery::Mode::Follow ? "follow" : cmd.mode == recovery::Mode::Avoid ? "avoid" : cmd.mode == recovery::Mode::Recover ? "recover" : "stand";
        r.step = cmd.step == recovery::Command::Step::BackUp ? "back_up" : cmd.step == recovery::Command::Step::TurnToLane ? "turn_to_lane" : cmd.step == recovery::Command::Step::Retry ? "retry" : cmd.step == recovery::Command::Step::Hover ? "hover" : "none";
        r.layerWhy = cmd.why ? cmd.why : ""; r.layerChanged = cmd.changed;
        auto only = [&](auto keep, const char* why) { for (int i = 0; i < n; ++i) if (!keep(i) && std::isfinite(energies[i])) { energies[i] = kInf; rejects[i] = why; } };
        if (weighted && !trafficHold) {
            preferRecovery = cmd.mode == recovery::Mode::Recover || !anyMoving;
            // AP-1: the routine's held step (back up / turn to the lane) is the one plan while it is clear; a blocked step is told to
            // the routine (next tick it moves on) and this tick the candidates compete as before.
            bool heldStep = false;
            if (cmd.mode == recovery::Mode::Recover && (cmd.step == recovery::Command::Step::BackUp || cmd.step == recovery::Command::Step::TurnToLane) &&
                std::isfinite(cmd.wantSpeed) && n < kMaxCandidates) {
                const auto leg = commandedLeg();
                if (leg.clear) {
                    cands[n] = leg.candidate; energies[n] = leg.energy; clearanceTiers[n] = leg.terms.clearanceTier; rejects[n] = nullptr; ++n; r.candidates = n;
                    only([&](int i) { return i == n - 1; }, "recover_step_only");
                    heldStep = true; heldStepChosen = true;
                }
            }
            for (int i = 0; i < n && !heldStep; ++i) {
                if (!std::isfinite(energies[i])) continue;
                if (cands[i].count && cands[i].seg[0].speed < -.05 && queuedWait) {
                    // AP-1: no reversing in a queue (a car behind a stopped car waits)
                    energies[i] = kInf;
                    rejects[i] = "queued_behind_traffic";
                } else if (cands[i].count && std::fabs(cands[i].seg[0].speed) <= .1 && !std::isfinite(w.stopStation) && !queuedWait) {
                    energies[i] = kInf;
                    rejects[i] = "stationary_without_stop_order";
                } else {
                    energies[i] = rank(cands[i], clearanceTiers[i], energies[i]);
                }
            }
        } else if (cmd.mode == recovery::Mode::Follow) only([&](int i) {
            return (centre(cands[i]) && std::strcmp(cands[i].kind, "follow lane") == 0) ||
                std::strcmp(cands[i].kind, "kept") == 0 ||
                (std::isfinite(w.stopStation) && std::strcmp(cands[i].kind, "stop") == 0);
        }, "follow_mode_lane_centre_only");
        else if (cmd.mode == recovery::Mode::Stand) only([](int) { return false; }, "stand_mode");
        else if (cmd.mode == recovery::Mode::Recover && cmd.step == recovery::Command::Step::Hover) only([&](int i) { return cands[i].count && cands[i].seg[0].mode == 1; }, "recover_hover_only");
        else if (cmd.mode == recovery::Mode::Recover && std::isfinite(cmd.wantSpeed) && n < kMaxCandidates) {  // the routine's step as the one plan
            const auto leg = commandedLeg();
            cands[n] = leg.candidate; energies[n] = leg.clear ? leg.energy : kInf; rejects[n] = leg.clear ? nullptr : leg.terms.reject; ++n; r.candidates = n;
            only([&](int i) { return i == n - 1; }, "recover_step_only");
        }
    }
    for (int i = 0; i < n; ++i) {  // the winner, in index order (identical to the serial loop it replaces)
        const double e = energies[i];
        if (!std::isfinite(e)) continue;
        ++r.feasible;
        if (e < r.energy) { r.energy = e; best = i; }
    }
    // Step 2: refine around the four lowest-energy candidates (coarse to fine), appended after the coarse set; then once more around the best
    // two of those refinements (the tight hairpin took 33 s with one round, against 19.5 s with the old full set).
    {   int from = 0, to = n, m = n;
        for (int round = 0; round < (cmd.mode == recovery::Mode::Avoid ? wt.refineRounds : 0); ++round) {  // RV-1: only Avoid refines (refinements leave the centre)
            const int want4 = round == 0 ? 4 : 2; int top[4]; int nt = 0;
            for (int pass = 0; pass < want4; ++pass) { int b = -1; for (int i = from; i < to; ++i) { if (!std::isfinite(energies[i])) continue; bool used = false; for (int t = 0; t < nt; ++t) used |= top[t] == i; if (!used && (b < 0 || energies[i] < energies[b])) b = i; } if (b >= 0) top[nt++] = b; }
            const int start = m;
            for (int t = 0; t < nt && m < kMaxCandidates; ++t) m += Refine(cands[top[t]], cands + m, kMaxCandidates - m);
            for (int i = start; i < m; ++i) {
                const double raw = Score(cands[i], s, k, want, w, wt, &mem, now, terms, nullptr, clear, hint, field, nullptr, weighted, true);
                clearanceTiers[i] = terms.clearanceTier;
                const double e = rank(cands[i], terms.clearanceTier, raw);
                energies[i] = e; rejects[i] = std::isfinite(e) ? nullptr : terms.reject;
                if (!std::isfinite(e)) continue;
                ++r.feasible;
                if (e < r.energy) { r.energy = e; best = i; }
            }
            from = start; to = m;
        }
        r.candidates = m;
    }
    if (s.mode == 0) { r.startGrade = FitGrade(w, s, k); r.maxGrade = carcap::kCar.MaxGrade(wt.grip); }  // GR-2 trace
    if constexpr (requires { w.floorSetAside; }) {
        r.floorSetAside = w.floorSetAside;
        if (r.startGrade.ok && r.floorSetAside) r.startGrade.raw = FitGrade(w, s, k, false).raw;
    }
    {   // CT-1: rejections per rule over every scored candidate, and the cheapest lane-following plan per speed (coarse set, any offset)
        for (int i = 0; i < r.candidates; ++i) { if (!rejects[i]) continue; const char* nm = *rejects[i] ? rejects[i] : "unnamed"; int q = 0;
            while (q < r.rejectKinds && std::strcmp(r.rejectName[q], nm) != 0) ++q;
            if (q == r.rejectKinds) { if (q >= Result::kRejectKinds) continue; r.rejectName[q] = nm; ++r.rejectKinds; }
            ++r.rejectCount[q]; }
        if (lane) {
            int pick[Result::kLaneSpeeds]; for (int& p : pick) p = -1;
            for (int i = 0; i < n; ++i) { if (std::strcmp(cands[i].kind, "follow lane") != 0 || !cands[i].count) continue;
                const double v = cands[i].seg[0].speed; int q = 0;
                while (q < r.laneSpeedCount && std::fabs(r.laneSpeeds[q].speed - v) > 1e-6) ++q;
                if (q == r.laneSpeedCount) { if (q >= Result::kLaneSpeeds) continue; r.laneSpeeds[q].speed = v; ++r.laneSpeedCount; }
                const int p = pick[q];
                if (p < 0 || (std::isfinite(energies[i]) && (!std::isfinite(energies[p]) || energies[i] < energies[p])) || (!std::isfinite(energies[p]) && !std::isfinite(energies[i]) && cands[i].seg[0].offset == 0)) pick[q] = i; }
            for (int q = 0; q < r.laneSpeedCount; ++q) { const int p = pick[q]; if (p < 0) continue; auto& L = r.laneSpeeds[q]; double cl;
                const double raw = Score(cands[p], s, k, want, w, wt, &mem, now, L.terms, nullptr, cl, hint, field, nullptr, weighted, true);
                L.energy = rank(cands[p], L.terms.clearanceTier, raw);
                L.reject = std::isfinite(L.energy) ? "" : L.terms.reject; } }
    }
    if (best < 0) {  // nothing finite: stand (always physically possible) and say so
        r.best.count = 1; r.best.seg[0] = { 0, 0, kSteps * kDt }; r.best.kind = cmd.mode == recovery::Mode::Stand ? "stand: the mode stands (see plan.mode)" : "stand: every candidate enters a solid";
        r.first = { s.input, 0 }; r.why = r.best.kind; for (auto& t : r.traj) t = s; mem.have = false; return r;
    }
    {   // ST-C (2026-11-22): the committed plan ("kept", time-shifted) against the cheapest challenger, with ONE margin: the time a switch itself costs.
        // Every energy is in seconds, so replacing a published command costs one controller reaction (wheelguard::reaction), and a challenger must be
        // cheaper than the kept plan by that long for two reactions before it takes over. Replaces switchBase / challengeFor / holdAfter / jumpWorse /
        // steepWorse / roomBetter and the commit term: a consistent cost-to-go makes consistent choices, so no extra stickiness is added.
        constexpr double kSwitchTime = wheelguard::reaction;
        // A candidate's commanded speed (unsigned) t seconds in (segment speeds; the rollout follows them within the car's limits).
        auto speedAt = [](const Candidate& c, double t) { for (int i = 0; i < c.count; ++i) { if (t < c.seg[i].seconds || i + 1 == c.count) return std::fabs(c.seg[i].speed); t -= c.seg[i].seconds; } return 0.0; };
        auto dirOf = [](const Candidate& c) { for (int i = 0; i < c.count; ++i) { if (c.seg[i].speed > .05) return 1; if (c.seg[i].speed < -.05) return -1; } return 0; };
        int keptIdx = -1; for (int i = 0; i < r.candidates; ++i) if (std::strcmp(cands[i].kind, "kept") == 0) { keptIdx = i; break; }
        const bool dirChange = keptIdx >= 0 && dirOf(cands[best]) != 0 && dirOf(cands[keptIdx]) != 0 && dirOf(cands[best]) != dirOf(cands[keptIdx]);
        if (keptIdx >= 0 && best != keptIdx && mem.have) {
            const double ek = energies[keptIdx], eb = energies[best];
            const double gap = ek - eb;
            if (!std::isfinite(ek)) { r.switchWhy = "committed_plan_enters_a_solid"; mem.challengeSince = -1; ++mem.relSolid; }
            else if (weighted && clearanceTiers[best] < clearanceTiers[keptIdx]) {
                r.switchWhy = "challenger_has_better_clearance_tier"; ++mem.relClose;
            }
            // Yielding is never held (as drivers brake at once and speed up with hesitation): a challenger that is slower within the next 2 s by
            // more than 0.5 m/s, or stops, in the same direction, takes over at once. The hold only settles gaining: a faster plan or another line.
            else if (dirOf(cands[best]) * dirOf(cands[keptIdx]) >= 0 && energies[best] < ek
                     && (speedAt(cands[best], 1.0) < speedAt(cands[keptIdx], 1.0) - .5 || speedAt(cands[best], 2.0) < speedAt(cands[keptIdx], 2.0) - .5)) {
                r.switchWhy = "challenger_yields"; r.rival = cands[keptIdx].kind; r.rivalGap = ek - energies[best]; mem.challengeSince = -1; ++mem.relYield; }
            else {
                const bool wins = eb < ek - kSwitchTime;
                if (wins) { if (mem.challengeSince < 0 || now < mem.challengeSince) mem.challengeSince = now; } else mem.challengeSince = -1;
                const bool ready = wins && now - mem.challengeSince >= 2 * kSwitchTime;
                r.rival = cands[best].kind; r.rivalGap = gap;
                if (ready) { r.switchWhy = "challenger_cheaper_by_the_switch_time_long_enough"; ++mem.relTimer; }
                else { best = keptIdx; r.energy = ek; r.kept = true; r.switchWhy = !wins ? (dirChange ? "direction_near_tie_kept" : "near_tie_committed_plan_kept") : "challenger_not_yet_long_enough"; }
            }
        } else if (keptIdx >= 0 && best == keptIdx) { r.kept = true; mem.challengeSince = -1; }
        // PF-2: the same recovery step carrying on is the committed plan, not a switch (its steering follows the heading error each tick;
        // counting it as a switch logged a plan.switch line per tick and reset the hold timers).
        if (heldStepChosen && !r.kept && mem.have && mem.best.count && mem.best.kind && std::strcmp(mem.best.kind, cands[best].kind) == 0) {
            r.kept = true; r.switchWhy = "recovery_step_continues"; }
        if (!r.kept) { mem.switchedAt = now; mem.challengeSince = -1; }
        else ++mem.keptN;
    }
    r.ok = true; r.best = cands[best]; r.why = r.best.kind;
    Score(r.best, s, k, want, w, wt, &mem, now, r.terms, r.traj, r.minClearance, hint, field, nullptr, weighted, true);
    r.first = FirstControl(r.best, s, k, w, wt, &mem, hint);
    if (weighted && r.best.seg[0].retrace) {
        r.layer = "recover"; r.step = "retrace"; r.layerWhy = "weighted_travelled_path";
    } else if (weighted) {
        mem.trail.target = -1;
    }
    if (weighted) mem.trail.Note(s, trailOptions, trailSwept, now);
    // firstMode is the SEGMENT's mode: a wheels segment while still in the air means "come down" (reporting the state's mode made a landing
    // look like "hover" and the follower climbed back up: step 3 probe).
    r.firstMode = r.best.seg[0].mode; r.firstStrafe = r.best.seg[0].mode == 1 ? (std::isfinite(r.best.seg[0].travel) ? std::remainder(r.best.seg[0].travel - s.heading, 2 * 3.14159265358979323846) : r.best.seg[0].strafe) : 0.0; r.firstHeight = r.traj[std::min(kSteps, 5)].z;  // step 3
    { double dirs = 0; for (int i = 0; i < r.best.count; ++i) { const double d = r.best.seg[i].speed > .05 ? 1 : r.best.seg[i].speed < -.05 ? -1 : 0; if (d != 0 && dirs != 0 && d != dirs) ++r.gearChanges; if (d != 0) dirs = d; } }
    r.speedAhead = r.traj[std::min(kSteps, int(std::lround(.5 / kDt)))].speed;  // O3: the controller follows the plan's speed 0.5 s ahead
    r.offset = r.best.count && r.best.seg[0].follow ? r.best.seg[0].offset : 0.0;
    {   // HC-4: the first candidate at the full wanted speed straight on the lane (follow lane, offset 0) or a straight arc where there is no lane
        for (int i = 0; i < r.candidates; ++i) { const Candidate& c = cands[i]; if (!c.count || c.seg[0].speed < std::max(1.0, want) - 1e-6) continue;
            const bool hit = lane ? std::strcmp(c.kind, "follow lane") == 0 && c.seg[0].offset == 0 : std::strcmp(c.kind, "forward arc") == 0 && c.seg[0].input == 0;
            if (!hit) continue; double cl; r.fullSpeed = c.seg[0].speed;
            const double raw = Score(c, s, k, want, w, wt, &mem, now, r.fullTerms, nullptr, cl, hint, field, nullptr, weighted, true);
            r.fullEnergy = rank(c, r.fullTerms.clearanceTier, raw); break; } }
    r.rel[0] = mem.relWorse; r.rel[1] = mem.relClose; r.rel[2] = mem.relSolid; r.rel[3] = mem.relYield; r.rel[4] = mem.relTimer; r.rel[5] = mem.keptN;
    mem.have = true; mem.best = r.best; mem.at = now; for (int i = 0; i <= kSteps; ++i) mem.traj[i] = r.traj[i];
    return r;
}
}
