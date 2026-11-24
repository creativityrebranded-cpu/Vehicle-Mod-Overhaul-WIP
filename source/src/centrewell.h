#pragma once
// Centre-line well (CW-1, 2026-11-22; notes/centreline-well-20261122/PLAN.md, user: "if it's convex where the center line is the well and
// then attractors are the type that push it out", "plan on how to get it to work for all cases", "Yes, start step 1").
//
// Place in notes/VEHICLE_DECISION_DESIGN_20261106.md: Plan, between Route (section 4) and Speed (section 7). STEP 1 OF THE PLAN: pure solver
// and side choice with offline tests (tests/centrewell_test.cpp). Nothing here is wired into the game yet.
//
// One lateral problem per car. Stations s = 0..n-1, `step` metres apart along the route line; y[s] = metres to the RIGHT of the line.
//
//     minimise   sum a*y_s^2                          the well (the centre line is the lowest point)
//              + sum b*(y_s+1 - y_s)^2                 damping (no ripple: b = 2*sqrt(a*c), critical damping, by default)
//              + sum c*(y_s-1 - 2*y_s + y_s+1)^2      smoothness (no kinks the steering cannot follow)
//              + sum w*(y_s - e_s)^2 where one-sided  soft pushes (doubtful points), only on the side they push from
//     subject to lo_s <= y_s <= hi_s                  hard bounds (lane edges, trusted solids, hazard boxes, the chosen sides)
//                y_0 = the car's offset now, y_1 = y_0 + its heading error * step
//
// Every term is convex and the bounds are intervals, so the answer is unique. Solved exactly by an active-set method over a banded
// (pentadiagonal) system: a handful of factorisations of a 60 x 60 band, microseconds.
//
// Hard obstacles become bounds through a side choice (Choose): each obstacle is passed on one side, chosen once and kept until passed.
// Where the bounds cross (lo > hi) nothing fits: that station is the block point, the plan ends there and the speed layer stops short.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace centrewell {

inline constexpr int kMaxStations = 96;
inline constexpr double kOpen = 1e9;  // an unbounded side

namespace rules {
inline constexpr int kActiveSetRounds = 60;    // active-set passes at most (each is one banded solve)
inline constexpr double kBoundTol = 1e-9;      // m: on a bound
inline constexpr double kCrossCost = 2.0;      // side choice: cost of crossing the centre line between two obstacles (in well units * m)
inline constexpr int kPreferredSide = +1;      // D1 default (user to decide): a free choice passes on the right
}

// ---- the lateral problem ----
struct Problem {
    int n = 0;                 // stations used (<= kMaxStations)
    double step = 1.0;         // m between stations
    double a = 1.0;            // well weight per m^2 per station
    double c = 10.0;           // smoothness weight per station (second difference, m^2)
    double b = -1.0;           // slope weight (first difference); < 0: critical damping 2*sqrt(a*c), the least that leaves no ripple
    double y0 = 0.0;           // the car's offset now (m, + right)
    double slope0 = 0.0;       // the car's heading error relative to the line (dy/ds, + turning right of the line)
    bool fixStart = true;      // y_0 (and y_1 from slope0) are the car's real state
    double lo[kMaxStations]{}, hi[kMaxStations]{};  // hard bounds (set by Reset to the open road)
    double softW[kMaxStations]{}, softE[kMaxStations]{};  // one soft push per station: weight and edge
    int softSide[kMaxStations]{};                          // +1 pushes right (cost when y < e), -1 pushes left (cost when y > e), 0 none
    void Reset(int stations, double laneLo = -kOpen, double laneHi = kOpen) {
        n = std::clamp(stations, 0, kMaxStations);
        for (int i = 0; i < kMaxStations; ++i) { lo[i] = laneLo; hi[i] = laneHi; softW[i] = 0; softE[i] = 0; softSide[i] = 0; }
    }
    void Bound(int i, double l, double h) { if (i >= 0 && i < n) { lo[i] = std::max(lo[i], l); hi[i] = std::min(hi[i], h); } }
    void Soft(int i, int side, double edge, double w) {  // keeps the stronger push at a station
        if (i < 0 || i >= n || side == 0 || !(w > 0)) return;
        if (softSide[i] == 0 || w > softW[i]) { softSide[i] = side; softE[i] = edge; softW[i] = w; }
    }
};

struct Result {
    bool ok = false;
    int n = 0;                 // stations solved (stops before the block point)
    int blockAt = -1;          // first station where nothing fits (lo > hi), -1 none
    int rounds = 0;            // active-set passes used
    bool converged = false;
    double y[kMaxStations]{};
    double maxOffset = 0.0;    // largest |y|
    double wellCost = 0, smoothCost = 0, softCost = 0;
    const char* why = "";
};

namespace detail {
// Solves Q y = b for the free variables of a symmetric pentadiagonal Q (bands q0 main, q1, q2), with the fixed ones moved to the right.
// Returns false if the band is not positive definite (cannot happen with a > 0).
inline bool SolveBand(int n, const double* q0, const double* q1, const double* q2, const double* b, const bool* fixed, double* y) {
    // Reduced ordering of the free variables; fixed values move to the right-hand side.
    int idx[kMaxStations]; int m = 0;
    for (int i = 0; i < n; ++i) if (!fixed[i]) idx[m++] = i;
    if (m == 0) return true;
    auto Q = [&](int i, int j) -> double {  // full symmetric entry
        const int d = std::abs(i - j); const int k = std::min(i, j);
        return d == 0 ? q0[k] : d == 1 ? q1[k] : d == 2 ? q2[k] : 0.0;
    };
    double r[kMaxStations];
    for (int t = 0; t < m; ++t) {
        const int i = idx[t]; double v = b[i];
        for (int j = std::max(0, i - 2); j <= std::min(n - 1, i + 2); ++j) if (fixed[j]) v -= Q(i, j) * y[j];
        r[t] = v;
    }
    // Banded Cholesky of the reduced matrix (bandwidth 2 is kept: removing rows/cols of a band matrix never widens the band in the
    // reduced order beyond the original index gap, and free neighbours more than 2 indices apart have Q = 0).
    double L0[kMaxStations]{}, L1[kMaxStations]{}, L2[kMaxStations]{};  // L(t,t), L(t,t-1), L(t,t-2)
    for (int t = 0; t < m; ++t) {
        const double a2 = t >= 2 ? Q(idx[t], idx[t - 2]) : 0.0;
        const double a1 = t >= 1 ? Q(idx[t], idx[t - 1]) : 0.0;
        const double l2 = t >= 2 ? a2 / L0[t - 2] : 0.0;
        const double l1 = t >= 1 ? (a1 - (t >= 2 ? l2 * L1[t - 1] : 0.0)) / L0[t - 1] : 0.0;
        const double d = Q(idx[t], idx[t]) - l1 * l1 - l2 * l2;
        if (!(d > 1e-14)) return false;
        L0[t] = std::sqrt(d); L1[t] = l1; L2[t] = l2;
    }
    double z[kMaxStations];
    for (int t = 0; t < m; ++t) z[t] = (r[t] - (t >= 1 ? L1[t] * z[t - 1] : 0.0) - (t >= 2 ? L2[t] * z[t - 2] : 0.0)) / L0[t];
    for (int t = m - 1; t >= 0; --t) {
        double v = z[t];
        if (t + 1 < m) v -= L1[t + 1] * y[idx[t + 1]];
        if (t + 2 < m) v -= L2[t + 2] * y[idx[t + 2]];
        y[idx[t]] = v / L0[t];
    }
    return true;
}
}  // namespace detail

// Solves the problem. The plan stops before the first station where the bounds cross.
inline Result Solve(const Problem& p) {
    Result r;
    int n = std::clamp(p.n, 0, kMaxStations);
    const Problem& pp = p;
    for (int i = 0; i < n; ++i) if (pp.lo[i] > pp.hi[i] + rules::kBoundTol) { r.blockAt = i; n = i; break; }
    r.n = n;
    if (n == 0) { r.why = r.blockAt == 0 ? "blocked_at_the_car" : "no_stations"; return r; }
    const double a = std::max(1e-6, p.a), c = std::max(0.0, p.c);
    // Critical damping: with only a and c the path rings (the solution of c y'''' + a y = 0 oscillates: a 6 cm swing the wrong way before
    // and after a dodge, the first test run). b^2 >= 4ac makes every root real, so the path eases out and back without crossing the line.
    const double bw = p.b < 0 ? 2.0 * std::sqrt(a * c) : p.b;
    // Q = a I + c D^T D (D: second differences on rows 1..n-2), bands.
    double q0[kMaxStations]{}, q1[kMaxStations]{}, q2[kMaxStations]{};
    for (int i = 0; i < n; ++i) q0[i] = a;
    for (int k = 0; k + 1 < n; ++k) { q0[k] += bw; q0[k + 1] += bw; q1[k] += -bw; }  // row (k, k+1) with (-1, 1)
    for (int k = 1; k + 1 < n; ++k) {  // row (k-1, k, k+1) with (1, -2, 1)
        const int i0 = k - 1, i1 = k, i2 = k + 1;
        q0[i0] += c; q0[i1] += 4 * c; q0[i2] += c;
        q1[i0] += -2 * c; q1[i1] += -2 * c;  // (i0,i1) and (i1,i2)
        q2[i0] += c;                         // (i0,i2)
    }
    bool fixed[kMaxStations]{}; int atBound[kMaxStations]{};  // -1 lo, +1 hi, 0 free; start fixes are separate
    bool start[kMaxStations]{};
    double y[kMaxStations]{};
    if (p.fixStart) {
        start[0] = true; y[0] = std::clamp(p.y0, pp.lo[0], pp.hi[0]);
        if (n > 1) { start[1] = true; y[1] = std::clamp(p.y0 + p.slope0 * p.step, pp.lo[1], pp.hi[1]); }
    }
    bool softOn[kMaxStations]{};
    for (int round = 0; round < rules::kActiveSetRounds; ++round) {
        r.rounds = round + 1;
        double d0[kMaxStations], b[kMaxStations];
        for (int i = 0; i < n; ++i) {
            d0[i] = q0[i] + (softOn[i] ? pp.softW[i] : 0.0);
            b[i] = softOn[i] ? pp.softW[i] * pp.softE[i] : 0.0;
            fixed[i] = start[i] || atBound[i] != 0;
            if (atBound[i] < 0) y[i] = pp.lo[i]; else if (atBound[i] > 0) y[i] = pp.hi[i];
        }
        if (!detail::SolveBand(n, d0, q1, q2, b, fixed, y)) { r.why = "band_not_positive"; return r; }
        bool changed = false;
        // Soft pushes: on where the solution is on their costly side.
        for (int i = 0; i < n; ++i) {
            if (!pp.softSide[i]) continue;
            const bool want = pp.softSide[i] > 0 ? y[i] < pp.softE[i] : y[i] > pp.softE[i];
            if (want != softOn[i]) { softOn[i] = want; changed = true; }
        }
        // Bounds: fix the most violated free variables; release fixed ones the gradient pulls inward.
        for (int i = 0; i < n; ++i) {
            if (start[i]) continue;
            if (!atBound[i]) {
                if (y[i] < pp.lo[i] - rules::kBoundTol) { atBound[i] = -1; changed = true; }
                else if (y[i] > pp.hi[i] + rules::kBoundTol) { atBound[i] = +1; changed = true; }
            } else {
                double g = d0[i] * y[i] - b[i];  // gradient / 2 of y^T Q y - 2 b^T y at i
                for (int j = std::max(0, i - 2); j <= std::min(n - 1, i + 2); ++j) {
                    if (j == i) continue;
                    const int dd = std::abs(i - j), k = std::min(i, j);
                    g += (dd == 1 ? q1[k] : q2[k]) * y[j];
                }
                if ((atBound[i] < 0 && g < -1e-12) || (atBound[i] > 0 && g > 1e-12)) { atBound[i] = 0; changed = true; }
            }
        }
        if (!changed) { r.converged = true; break; }
    }
    for (int i = 0; i < n; ++i) {
        r.y[i] = y[i]; r.maxOffset = std::max(r.maxOffset, std::fabs(y[i]));
        r.wellCost += a * y[i] * y[i];
        if (pp.softSide[i]) { const double gap = pp.softSide[i] > 0 ? std::max(0.0, pp.softE[i] - y[i]) : std::max(0.0, y[i] - pp.softE[i]); r.softCost += pp.softW[i] * gap * gap; }
    }
    for (int k = 1; k + 1 < n; ++k) { const double d2 = y[k - 1] - 2 * y[k] + y[k + 1]; r.smoothCost += c * d2 * d2; }
    for (int k = 0; k + 1 < n; ++k) { const double d1 = y[k + 1] - y[k]; r.smoothCost += bw * d1 * d1; }
    r.ok = r.converged;
    r.why = !r.converged ? "active_set_did_not_settle" : r.blockAt >= 0 ? "plan_ends_at_a_block" : r.maxOffset < 1e-6 ? "centre_line" : "pushed_off_centre";
    return r;
}

// ---- hard obstacles and the side choice ----
// One obstacle in the road frame: stations [s0, s1] (m along the line from the car) and the lateral span [left, right] (m, + right) it
// occupies, ALREADY widened by the car's half width and the margin for this source (person, vehicle, box).
struct Obstacle {
    unsigned id = 0;
    double s0 = 0, s1 = 0, left = 0, right = 0;
    int kept = 0;  // the side chosen earlier and not yet passed: +1 pass on the right (y >= right), -1 on the left (y <= left), 0 none
};
struct Choice { int side = 0; double need = 0; bool fits = false; bool touching = false; const char* why = ""; };

// Already inside its margin (step 2 finding, 2026-11-22: all 21 recorded blocks the well found were the car already inside an obstacle's
// widened zone, a wall or a run of sensor points beside it, so no station had room from the start). The car cannot cross through it and
// it may not come closer: it is passed on the side the car is on, with its edge eased to where the car is (the car keeps its present
// distance or moves away, never further in).
inline bool Touching(const Problem& p, const Obstacle& o) { return p.fixStart && o.s0 <= 0.0 && o.s1 >= 0.0 && p.y0 > o.left && p.y0 < o.right; }
// Level with the car (its stations include the car's): passing it on the far side would mean driving through it, so only the car's side is
// offered, whether or not the car is inside its margin.
inline bool Beside(const Problem& p, const Obstacle& o) { return p.fixStart && o.s0 <= 0.0 && o.s1 >= 0.0; }

inline int StationOf(double s, double step) { return int(std::floor(s / std::max(1e-6, step) + 1e-9)); }

// The tightest lane bounds over an obstacle's stations (the road it must be passed within).
inline void LaneOver(const Problem& p, const Obstacle& o, double& laneLo, double& laneHi) {
    laneLo = -kOpen; laneHi = kOpen;
    const int i0 = std::max(0, StationOf(o.s0, p.step)), i1 = std::min(p.n - 1, StationOf(o.s1, p.step) + 1);
    for (int i = i0; i <= i1; ++i) { laneLo = std::max(laneLo, p.lo[i]); laneHi = std::min(laneHi, p.hi[i]); }
}

// Picks a side for every obstacle (sorted along the road): the side that fits with the smallest well cost, a cost for crossing the
// centre line between consecutive obstacles, the kept side while it still fits, rules::kPreferredSide on a tie. A small dynamic programme
// over the obstacles, two states each.
inline std::vector<Choice> Choose(const Problem& p, const std::vector<Obstacle>& obs) {
    const std::size_t m = obs.size();
    std::vector<Choice> out(m);
    if (!m) return out;
    const double a = std::max(1e-6, p.a);
    auto option = [&](const Obstacle& o, int side, double& need, bool& fits) {
        double laneLo, laneHi; LaneOver(p, o, laneLo, laneHi);
        if (Beside(p, o)) {  // only the side the car is on; inside the margin, its edge where the car is
            const int mine = p.y0 >= .5 * (o.left + o.right) ? +1 : -1;
            const bool inside = Touching(p, o);
            const double edge = side > 0 ? (inside ? std::min(o.right, p.y0) : o.right) : (inside ? std::max(o.left, p.y0) : o.left);
            need = side > 0 ? std::max(0.0, edge) : std::max(0.0, -edge);
            fits = side == mine && (side > 0 ? edge <= laneHi : edge >= laneLo);
            const double length = std::max(p.step, o.s1 - o.s0);
            return fits ? a * need * need * length : std::numeric_limits<double>::infinity();
        }
        if (side > 0) { need = std::max(0.0, o.right); fits = o.right <= laneHi; }   // y >= right inside the lane
        else { need = std::max(0.0, -o.left); fits = o.left >= laneLo; }              // y <= left inside the lane
        const double length = std::max(p.step, o.s1 - o.s0);
        return fits ? a * need * need * length : std::numeric_limits<double>::infinity();
    };
    constexpr double kInf = std::numeric_limits<double>::infinity();
    std::vector<std::array<double, 2>> cost(m), need(m); std::vector<std::array<bool, 2>> fit(m); std::vector<std::array<int, 2>> from(m);
    for (std::size_t k = 0; k < m; ++k) {
        double hereOf[2];
        for (int t = 0; t < 2; ++t) { double nd = 0; bool f = false; hereOf[t] = option(obs[k], t ? +1 : -1, nd, f); need[k][t] = nd; fit[k][t] = f; }
        for (int t = 0; t < 2; ++t) {
            const int side = t ? +1 : -1;
            double here = hereOf[t];
            const bool f = fit[k][t];
            if (f && obs[k].kept == -side && fit[k][obs[k].kept > 0 ? 1 : 0]) here = kInf;     // kept side still fits: the other is not offered
            if (f && side != rules::kPreferredSide && obs[k].kept == 0) here += 1e-9;           // tie: the preferred side
            if (k == 0) { cost[k][t] = here; from[k][t] = -1; continue; }
            double best = kInf; int arg = -1;
            for (int u = 0; u < 2; ++u) {
                const bool crosses = u != t && (need[k - 1][u] > 0 || need[k][t] > 0);
                const double v = cost[k - 1][u] + (crosses ? rules::kCrossCost : 0.0);
                if (v < best) { best = v; arg = u; }
            }
            cost[k][t] = best + here; from[k][t] = arg;
        }
    }
    int t = cost[m - 1][1] <= cost[m - 1][0] ? 1 : 0;
    if (!(cost[m - 1][t] < kInf)) t = fit[m - 1][1] ? 1 : 0;
    for (std::size_t k = m; k-- > 0;) {
        auto& c = out[k];
        c.side = t ? +1 : -1; c.need = need[k][t]; c.fits = fit[k][t]; c.touching = Touching(p, obs[k]);
        c.why = !c.fits ? "neither_side_fits" : c.touching ? "already_touching_keeps_its_side" : obs[k].kept == c.side ? "kept_until_passed" : need[k][t] == 0 ? "clear_of_the_line" : "more_room_this_side";
        const int prev = from[k][t]; if (k > 0) t = prev >= 0 ? prev : t;
    }
    return out;
}

// Applies the chosen sides as bounds: pass right = y >= right over the obstacle's stations, pass left = y <= left.
inline void ApplySides(Problem& p, const std::vector<Obstacle>& obs, const std::vector<Choice>& sides) {
    for (std::size_t k = 0; k < obs.size() && k < sides.size(); ++k) {
        const auto& o = obs[k];
        const int i0 = std::max(0, StationOf(o.s0, p.step)), i1 = std::min(p.n - 1, StationOf(o.s1, p.step) + 1);
        const bool touching = sides[k].touching;
        const double right = touching ? std::min(o.right, p.y0) : o.right, left = touching ? std::max(o.left, p.y0) : o.left;  // eased to the car
        for (int i = i0; i <= i1; ++i) {
            if (sides[k].side > 0) p.lo[i] = std::max(p.lo[i], right);
            else p.hi[i] = std::min(p.hi[i], left);
        }
    }
}

// Kept sides: an obstacle keeps its side until the car is passMargin past its end (s1 < -passMargin in the car's frame).
struct Memory {
    struct Entry { unsigned id = 0; int side = 0; };
    std::vector<Entry> entries;
    int Side(unsigned id) const { for (const auto& e : entries) if (e.id == id) return e.side; return 0; }
    void Update(const std::vector<Obstacle>& obs, const std::vector<Choice>& sides, double passMargin = 2.0) {
        std::vector<Entry> next;
        for (std::size_t k = 0; k < obs.size() && k < sides.size(); ++k)
            if (obs[k].s1 > -passMargin && sides[k].fits) next.push_back({ obs[k].id, sides[k].side });
        entries.swap(next);
    }
};

// ---- step 3: what each source is worth ----
// The margin beyond the car's half width that each source keeps, and why (step 2: 7 recorded blocks were lanes barely wider than the car
// plus a flat 0.3 m, so one margin for everything was too wide where the data is exact and too narrow where people are).
namespace margin {
inline constexpr double kHazardBox = .3;   // the user's own mark, drawn by hand: a little room for the drawing
inline constexpr double kGameBox = .3;     // a game collision box: fitted, not exact
inline constexpr double kSensor = .15;     // a measured point (ray, volume, radar): it is where the surface is; only the car's tracking error
inline constexpr double kVehicle = .5;     // a standing vehicle: its doors and mirrors are outside its box
inline constexpr double kPerson = .8;      // a person: never brushed past, and they move
}

// Floor bounds (step 2 finding: a lateral plan that never reads the floor drives off a drop as happily as round a box). For every station
// the drivable interval round the centre line: `drivable(station, y)` says whether the floor at (station, y) is the road's (unknown counts
// as drivable: missing data never closes the road; only a measured drop or step does). Sampled every `sampleStep` m out from the centre;
// the bound is the last drivable sample. A measured hole on the centre line itself closes the station (the plan ends there).
// Returns how many stations it narrowed.
template <class Drivable>
inline int FloorBounds(Problem& p, Drivable&& drivable, double sampleStep = .5, int stationStride = 2) {
    int narrowed = 0; const double ds = std::max(.1, sampleStep);
    for (int i = 0; i < p.n; i += std::max(1, stationStride)) {
        const double s = i * p.step; double lo = p.lo[i], hi = p.hi[i];
        if (lo > hi) continue;
        const double c = std::clamp(0.0, lo, hi);
        if (!drivable(s, c)) {
            for (int k = i; k < std::min(p.n, i + std::max(1, stationStride)); ++k) { p.lo[k] = 1; p.hi[k] = -1; }
            ++narrowed; continue;
        }
        double newHi = hi, newLo = lo;
        for (double y = c + ds; y <= hi + 1e-9; y += ds) if (!drivable(s, y)) { newHi = y - ds; break; }
        for (double y = c - ds; y >= lo - 1e-9; y -= ds) if (!drivable(s, y)) { newLo = y + ds; break; }
        if (newHi < hi || newLo > lo) {
            ++narrowed;
            for (int k = i; k < std::min(p.n, i + std::max(1, stationStride)); ++k) { p.lo[k] = std::max(p.lo[k], newLo); p.hi[k] = std::min(p.hi[k], newHi); }
        }
    }
    return narrowed;
}

// ---- the road frame (step 2): world <-> (station along the line, metres right of it) ----
// A route line as world points in travel order (x east, y north; heading = atan2(dx, dy), clockwise from north, as the rest of FVO).
struct Line {
    std::vector<double> x, y, s;  // s: cumulative length
    void Add(double px, double py) {
        if (!x.empty() && std::hypot(px - x.back(), py - y.back()) < 1e-6) return;
        s.push_back(x.empty() ? 0.0 : s.back() + std::hypot(px - x.back(), py - y.back())); x.push_back(px); y.push_back(py);
    }
    double Length() const { return s.empty() ? 0.0 : s.back(); }
    int Segment(double at) const {
        if (x.size() < 2) return -1;
        auto it = std::upper_bound(s.begin(), s.end(), at); int i = int(it - s.begin()) - 1;
        return std::clamp(i, 0, int(x.size()) - 2);
    }
    double HeadingAt(double at) const { const int i = Segment(at); return i < 0 ? 0.0 : std::atan2(x[i + 1] - x[i], y[i + 1] - y[i]); }
    // The world point `right` metres right of the line at station `at` (clamped to the line's ends, extended along the end segments).
    bool At(double at, double right, double& ox, double& oy) const {
        const int i = Segment(at); if (i < 0) return false;
        const double len = s[i + 1] - s[i]; if (!(len > 1e-9)) return false;
        const double t = (at - s[i]) / len, dx = (x[i + 1] - x[i]) / len, dy = (y[i + 1] - y[i]) / len;
        ox = x[i] + t * (x[i + 1] - x[i]) + dy * right; oy = y[i] + t * (y[i + 1] - y[i]) - dx * right;  // right of travel = (dy, -dx)
        return true;
    }
    // Nearest point: station, metres right of the line, the line's heading there.
    bool Project(double px, double py, double& at, double& right, double& heading) const {
        if (x.size() < 2) return false;
        double best = 1e300;
        for (std::size_t i = 0; i + 1 < x.size(); ++i) {
            const double dx = x[i + 1] - x[i], dy = y[i + 1] - y[i], l2 = dx * dx + dy * dy; if (l2 < 1e-12) continue;
            const double u = std::clamp(((px - x[i]) * dx + (py - y[i]) * dy) / l2, 0.0, 1.0);
            const double qx = x[i] + u * dx, qy = y[i] + u * dy, d2 = (px - qx) * (px - qx) + (py - qy) * (py - qy);
            if (d2 < best) { best = d2; const double l = std::sqrt(l2); at = s[i] + u * l; right = ((px - qx) * dy - (py - qy) * dx) / l; heading = std::atan2(dx, dy); }
        }
        return best < 1e300;
    }
};

// Weights from the car, not by hand (rule: general scheme). The well's unit is one: a = 1 per m^2 per station. The smoothness sets how
// long the car takes to move sideways: a sideways move of one metre over a characteristic length L asks about v^2 / L^2 of sideways
// acceleration, so L = v / sqrt(latAccel) keeps it within the grip the car holds, and c = a L^4 (the length at which the well and the
// smoothness balance). Never shorter than two stations.
struct Weights { double a = 1, c = 1, length = 2; };
inline Weights WeightsFor(double speed, double latAccel, double step = 1.0) {
    Weights w; w.length = std::max(2.0 * step, std::fabs(speed) / std::sqrt(std::max(.5, latAccel)));
    const double L = w.length / std::max(1e-6, step);  // in stations
    w.c = w.a * L * L * L * L; return w;
}

// The sharpest sideways bend the path asks (1/m, relative to the line): max |y''|.
inline double MaxCurvature(const Result& r, double step) {
    double k = 0; const double s2 = std::max(1e-6, step * step);
    for (int i = 1; i + 1 < r.n; ++i) k = std::max(k, std::fabs(r.y[i - 1] - 2 * r.y[i] + r.y[i + 1]) / s2);
    return k;
}
// Fit to grip (step 2 finding, 2026-11-22: starting 2 m off the line with 20 deg of heading error asked 10.6 m/s^2 sideways at 8 m/s, and
// a box at 15 m/s 6.8, against 3.5 the car holds). Sideways acceleration is v^2 |y''| and |y''| falls with the square of the characteristic
// length, so the length grows by sqrt(peak / grip) until the path fits. Where the bounds still force a tighter move (an obstacle close
// ahead), no length can fix it: `speedCap` = sqrt(grip / |y''|max) goes to the speed layer instead.
struct GripFit { Result r; Weights w; double peak = 0, speedCap = 1e9; int solves = 0; };
inline GripFit SolveWithinGrip(Problem p, double speed, double latAccel) {
    GripFit g; const double v = std::max(1.0, std::fabs(speed)), grip = std::max(.5, latAccel);
    g.w = WeightsFor(v, grip, p.step); p.a = g.w.a; p.c = g.w.c; p.b = -1;
    for (int i = 0; i < 5; ++i) {
        g.r = Solve(p); ++g.solves;
        g.peak = v * v * MaxCurvature(g.r, p.step);
        if (g.peak <= grip * 1.02 || !g.r.ok) break;
        g.w.length *= std::sqrt(g.peak / grip) * 1.05;
        const double L = g.w.length / std::max(1e-6, p.step); p.c = g.w.a * L * L * L * L;
    }
    const double k = MaxCurvature(g.r, p.step);
    g.speedCap = k > 1e-9 ? std::sqrt(grip / k) : 1e9;
    return g;
}

// A world box (centre, heading, half length along it, half width across) in the road frame relative to the car's station, widened by
// `widen` (the car's half width plus the source's margin) on both sides and `along` metres front and back (the car's half length).
inline Obstacle FromBox(const Line& line, double carStation, unsigned id, double cx, double cy, double heading, double halfLength, double halfWidth,
                        double widen, double along) {
    Obstacle o; o.id = id;
    const double sx = std::sin(heading), cyh = std::cos(heading);
    double s0 = 1e300, s1 = -1e300, l = 1e300, r = -1e300;
    for (int i = 0; i < 4; ++i) {
        const double u = (i & 1 ? 1 : -1) * halfLength, v = (i & 2 ? 1 : -1) * halfWidth;  // along, across (right)
        const double px = cx + sx * u + cyh * v, py = cy + cyh * u - sx * v;
        double at = 0, right = 0, h = 0; if (!line.Project(px, py, at, right, h)) continue;
        s0 = std::min(s0, at); s1 = std::max(s1, at); l = std::min(l, right); r = std::max(r, right);
    }
    o.s0 = s0 - carStation - along; o.s1 = s1 - carStation + along; o.left = l - widen; o.right = r + widen;
    return o;
}

// Fills a Problem for a car on the line: n stations from its station, the lane's half width less the car's half width as the hard
// bounds, the car's offset and heading error as the start.
inline void Frame(Problem& p, const Line& line, double carX, double carY, double carHeading, double laneHalf, double carHalfWidth, int n,
                  double step, double& carStation) {
    double at = 0, right = 0, h = 0; line.Project(carX, carY, at, right, h);
    carStation = at;
    const double room = std::max(0.0, laneHalf - carHalfWidth);
    p.Reset(n, -room, room); p.step = step; p.y0 = right;
    p.slope0 = std::tan(std::clamp(std::remainder(carHeading - h, 2 * 3.14159265358979323846), -1.2, 1.2));
    const int last = int(std::floor((line.Length() - at) / step));
    if (last + 1 < p.n) p.n = std::max(2, last + 1);  // the line ends
}

}  // namespace centrewell
