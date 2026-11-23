#pragma once
// Speed (notes/VEHICLE_DECISION_DESIGN_20261106.md section 7): a tiny MPC on a toy model. State: distance along a branch and speed.
// Control: acceleration over a short horizon. It picks the trial closest to the wanted speed that breaks no hard limit, and gives speed
// over time, arrival times and a cost. It replaces the list of speed limits and the separate collision predictor (G12, G19-G23).
// Fixed blocks and speed limits fold into one curve first: the fastest the car may be at each distance and still stop or slow in time.
// Only moving blocks are checked per trial. Reasons are kept, so the ledger can name what set the speed.
// Pure; tests: tests/planspeed_test.cpp.
#include <algorithm>
#include <cmath>
#include "carcap.h"
#include "wheelguard.h"

namespace planspeed {
constexpr int kSteps = 16;
constexpr double kDt = .25;                 // 4 s ahead
constexpr int kMaxLimits = 64, kMaxBlocks = 16;

// The toy model's brake (m/s^2): the same 6 as bicycle::Spec. Used where the car's own brake is not known (the traffic add-on); callers
// that know it pass it.
constexpr double kToyBrake = 6.0;
// Stage E (2026-11-22, GENERATED_COSTS_DESIGN): following values come from the car, not from hand numbers (were 3.0 m, 1.4 s, 3.0 m/s^2).
//   standing gap = 2 x the car's half length (a body length of room);
//   headway h    = t_r + v_lead / brake (the controller's reaction, then the time the leader's speed takes to brake away);
//   comfort brake = brake / 2 (half the hard brake).
// Keep the bumper gap plus headway. Closing on the leader also uses the distance needed to brake down to its speed.
inline double FollowingCap(double bumperGap, double leaderSpeed, double halfLength, double brake) {
    const double b = std::max(.1, brake), comfort = .5 * b;
    const double room = std::max(0.0, bumperGap - 2 * halfLength);
    const double lead = std::max(0.0, leaderSpeed);
    const double headway = wheelguard::reaction + lead / b;
    const double steady = room / headway;
    if (steady <= lead) return steady;
    const double bt = comfort * wheelguard::reaction;  // closing: the follower brakes at the comfort rate after one reaction (the leader's own braking is already in `lead`)
    return std::sqrt(bt * bt + lead * lead + 2 * comfort * room) - bt;
}
// A speed limit from `from` metres ahead onward to `to`.
struct Limit { double from = 0, to = 1e9, speed = 1e9; const char* why = ""; };
// A part of the branch the car's nose must not be in during a time window. Fixed things: tFrom 0, tTo 1e9.
struct Block { double from = 0, to = 0, tFrom = 0, tTo = 1e9, gap = 1.5; const char* why = ""; int shape = -1; };  // HN-1 shape: the World shape behind it (-1: none)

struct Input {
    double speed = 0, want = 10;            // now; wanted
    double accel = 6, brake = kToyBrake;   // the toy model's limits (m/s^2)
    double length = 1e9;                    // where the line ends: stop there
    double endGap = .75;
    const Limit* limits = nullptr; int limitCount = 0;
    const Block* blocks = nullptr; int blockCount = 0;
    double lastAccel = 0;                   // what the previous solution asked now (the warm start)
    double effort = .02, change = .05;      // weights
    const char* endWhy = "end of the line";  // PD-1c: what the line's end is (a route hold names its reason)
    const char* wantWhy = "";              // PD-1c: why the wanted speed is lower than the car's own top (a lane speed limit); empty: it is not
};
struct Output {
    bool feasible = false;                  // no hard limit broken
    double speed[kSteps + 1] = {}, along[kSteps + 1] = {}, accel = 0;
    double cost = 1e18, lost = 0;           // lost: seconds behind the wanted speed over the horizon
    const char* why = "";                   // what held the speed down most
    double blockedAt = -1;                  // a fixed block the car cannot stop for
    int heldShape = -1;                     // HN-1: the shape of the block that held it (a moving window, or the first fixed block)
};

// The fastest allowed at distance d: every limit and fixed stop ahead, brought back to d by braking.
inline double CapAt(const Input& in, double d, const char** why = nullptr) {
    double cap = 1e9; const char* w = "";
    auto take = [&](double v, const char* name) { if (v < cap) { cap = v; w = name; } };
    for (int i = 0; i < in.limitCount; ++i) {
        const auto& l = in.limits[i];
        if (d > l.to) continue;
        const double room = std::max(0.0, l.from - d);
        take(std::sqrt(l.speed * l.speed + 2 * in.brake * room), l.why);
    }
    for (int i = 0; i < in.blockCount; ++i) {
        const auto& b = in.blocks[i];
        if (b.tFrom > 0 || b.tTo < 1e8 || b.to < d) continue;
        take(std::sqrt(2 * in.brake * std::max(0.0, b.from - b.gap - d)), b.why);
    }
    if (in.length < 1e8) take(std::sqrt(2 * in.brake * std::max(0.0, in.length - in.endGap - d)), in.endWhy && *in.endWhy ? in.endWhy : "end of the line");
    if (why) *why = w;
    return cap;
}

// The cap curve, worked out once per branch: braking curves are straight lines in speed squared, so a table every 2 m is exact between
// its points. Reasons stay with each point.
constexpr int kTable = 96; constexpr double kTableStep = 2.0;
struct Curve {
    double v2[kTable] = {}; const char* why[kTable] = {};
    int count = kTable;
    // Only as far as the car can get inside the horizon.
    void Build(const Input& in) {
        const double T = kSteps * kDt, reach = std::max(in.speed, 0.0) * T + .5 * std::max(in.accel, .1) * T * T;
        count = std::clamp(int(reach / kTableStep) + 3, 2, kTable);
        for (int i = 0; i < count; ++i) { const double c = CapAt(in, i * kTableStep, &why[i]); v2[i] = c > 1e4 ? 1e8 : c * c; }
    }
    double At(double d, const char** w = nullptr) const {
        const double f = std::max(0.0, d) / kTableStep; const int i = std::min(int(f), count - 2); const double u = std::min(1.0, f - i);
        if (w) *w = why[v2[i] <= v2[i + 1] ? i : i + 1];
        return std::sqrt(std::max(0.0, v2[i] * (1 - u) + v2[i + 1] * u));
    }
};

inline Output Solve(const Input& in) {
    Output best;
    const double a = std::max(.1, in.accel), b = std::max(.1, in.brake);
    const double options[5] = { -b, -.4 * b, 0, .4 * a, a };
    Curve curve; curve.Build(in);
    int moving[kMaxBlocks], movingCount = 0; double firstFixed = 1e18; int firstFixedShape = -1;
    for (int i = 0; i < in.blockCount && i < kMaxBlocks; ++i) {
        if (in.blocks[i].tFrom <= 0 && in.blocks[i].tTo >= 1e8) { if (in.blocks[i].from < firstFixed) firstFixedShape = in.blocks[i].shape; firstFixed = std::min(firstFixed, in.blocks[i].from); }
        else moving[movingCount++] = i;
    }
    bool any = false;
    // Already over the curve (a block just appeared): braking as hard as the car can is all that is left. Otherwise the curve is a hard limit.
    const bool overNow = in.speed > curve.At(0) + .05;
    // mode 0: a1 for 1 s then a2. mode 1: follow the cap curve exactly (what keeps a stop precise). mode 2: full braking.
    auto trial = [&](double a1, double a2, int mode, Output* into) {
        Output o; o.speed[0] = std::max(0.0, in.speed); o.along[0] = 0;
        double cost = 0, lost = 0, prev = in.lastAccel; bool ok = true; const char* held = ""; int heldShape = -1;
        for (int k = 0; k < kSteps; ++k) {
            const double v = o.speed[k], s = o.along[k];
            double act = k < 4 ? a1 : a2;
            const char* w = ""; const double capNow = curve.At(s, &w), target = std::min(in.want, capNow);
            if (target < in.want - 1e-6) held = w;
            if (mode == 1) { const double ahead = curve.At(s + v * kDt); act = std::clamp((std::min(in.want, ahead) - v) / kDt, -b, a); }
            if (mode == 2) act = -b;
            double next = v + act * kDt;
            if (next < 0) { next = 0; act = -v / kDt; }
            const double moved = (v + next) * .5 * kDt, nose = s + moved;
            if (k == 0) o.accel = act;
            o.speed[k + 1] = next; o.along[k + 1] = nose;
            if (next > curve.At(nose) + 1e-3 && !(overNow && act <= -b + 1e-6)) ok = false;
            if (nose >= firstFixed) { ok = false; if (heldShape < 0) heldShape = firstFixedShape; }
            const double t0 = k * kDt, t1 = t0 + kDt;
            for (int m = 0; m < movingCount; ++m) {
                const auto& bl = in.blocks[moving[m]];  // the whole step against the whole window: nothing slips between samples
                if (t1 >= bl.tFrom && t0 <= bl.tTo && nose >= bl.from - bl.gap && s <= bl.to + bl.gap) { ok = false; held = bl.why; heldShape = bl.shape; }
            }
            const double e = next - target;
            cost += e * e * kDt + in.effort * act * act * kDt + in.change * (act - prev) * (act - prev);
            lost += std::max(0.0, in.want - next) / std::max(in.want, .1) * kDt;
            prev = act;
            if (!ok && !into) return;  // this trial is out
        }
        if (!*held && in.wantWhy && *in.wantWhy) held = in.wantWhy;  // PD-1c: nothing else held it below the wanted speed: the lowered wanted speed is the reason
        o.cost = cost; o.lost = lost; o.why = held; o.feasible = ok; o.heldShape = heldShape;
        if (into) { *into = o; return; }
        if (ok && (!any || cost < best.cost)) { best = o; any = true; }
    };
    for (double a1 : options) for (double a2 : options) trial(a1, a2, 0, nullptr);
    trial(0, 0, 1, nullptr);
    { const double w = std::clamp(in.lastAccel, -b, a); trial(w, w, 0, nullptr); }  // the warm start
    trial(0, 0, 2, nullptr);
    if (any) return best;
    // Nothing keeps every limit: full braking, marked so the branch drops out.
    Output stop; trial(0, 0, 2, &stop);
    stop.feasible = false; stop.cost = 1e12; stop.blockedAt = firstFixed < 1e17 ? firstFixed : -1;
    return stop;
}

// Knowledge kept as limits. Stage E (2026-11-22): each reads the car's own record (carcap.h) instead of a hand number.
// Corner: aLat is the sideways acceleration the car turns with: ground carcap::kCar.PlanLat(grip of the gear) (3.0 x grip; was a separate
// 3.5), flight the hull's sustained turn (hulllogic Flight lateral x the gear's lateralScale).
inline double CornerLimit(double curvature, double aLat) { const double c = std::fabs(curvature); return c > 1e-5 ? std::sqrt(std::max(0.0, aLat) / c) : 1e9; }
constexpr double kGravity = 9.81;  // physical
// kMinShare: CHOSEN RESOLUTION, no physical derivation. A grade the wheels cannot climb at all would be a speed of 0; the planner treats
// the grade as a constraint (carcap MaxGrade), so the speed cap only has to say "slow", never "stop": it bottoms at half the top speed.
constexpr double kMinShare = .5;
// Slope: the share of the tyres' grip that gravity along the grade uses is |grade| / MaxGrade(grip) (MaxGrade = friction limit or engine
// grade, carcap.h); what is left may go to speed. Was 1 - 2 |grade|: the same rule with MaxGrade .5.
inline double SlopeLimit(double top, double grade, double maxGrade) {
    const double used = std::fabs(grade) / std::max(1e-3, maxGrade);
    return top * std::clamp(1 - used, kMinShare, 1.0);
}// SG-1 (2026-11-19): SlopeLimit for a slope `distance` m ahead, brought back to the car by braking at `brake` (as the crest and bend caps
// are: planrules crestBrake). grade: rise over run along travel (+ up). Down a slope gravity takes g*sin from the brakes, so the braking
// that brings the cap back is that much weaker (never under 0.5 m/s^2): a steep descent in sight is met at its speed.
inline double GradeCap(double top, double grade, double distance, double brake, double maxGrade) {
    const double v = SlopeLimit(top, grade, maxGrade);
    const double down = grade < 0 ? kGravity * -grade / std::sqrt(1 + grade * grade) : 0.0;
    const double b = std::max(.5, brake - down);
    return std::sqrt(v * v + 2 * b * std::max(0.0, distance));
}
// Crest: the wheels keep their grip while the road's downward bend takes less than the share of their load the planner's safety margin
// can spare: friction goes with wheel load and the planner counts on `safety` of it (carcap Car::safety), so the share is 1 - safety
// (was a hand .5). The record has no suspension travel, so travel is NOT part of this share (a longer-travel car would be allowed more).
inline double CrestShare(const carcap::Car& car) { return 1.0 - car.safety; }
inline double CrestLimit(double curvature, double share) { return curvature > 1e-3 ? std::sqrt(std::max(0.0, share) * kGravity / curvature) : 1e9; }
// Sideways: D3. 1.2 m/s at speed; slowly, half a metre across per metre along (pathgrid.h, lanedriver.h kPassRate). (kept, see the report: no physical derivation was found for these three; they are chosen resolutions).
constexpr double kSideRate = 1.2, kSlowAcross = .5, kSlowSpeed = 6;
inline double SideLimit(double acrossPerMetre) {
    const double s = std::fabs(acrossPerMetre);
    if (s < 1e-4) return 1e9;
    if (s <= kSlowAcross) return std::max(kSlowSpeed, kSideRate / s);
    return 3.0;
}
}
