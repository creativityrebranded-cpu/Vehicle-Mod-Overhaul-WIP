#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <vector>

// Ground-first general route planner (2026-10-25, the user: "build a ground planner ... that accounts for everything;
// it should be generalizable but prefer ground"). Pure: no engine calls; the runtime (groundnav.cpp) fills the grid
// from the nav map, the game's collision objects and downward terrain rays, and runs this on a worker thread.
//
// One search over (cell, mode) with two modes, Ground and Air:
//  - Ground moves need dry, mapped-or-unknown ground, a step a wheel can take, a slope under the vehicle's limit and room
//    for the hull (solids inflated by the half-width). Cost per metre grows with slope, unmapped ground, nearby walls and
//    remembered bumps; roads are cheaper.
//  - Air moves (only for vehicles that can fly) cost airFactor times a flat ground metre, plus a take-off and a landing.
//    So the route flies only where driving is impossible or much longer: water, cliffs, walls, gaps, a goal up high.
// The result is a list of waypoints with a mode each, simplified (string pulling that never makes the route costlier),
// with a speed profile from corner radius, slope, unmapped ground, mode changes and the stop at the end.
// Follow() is the pure-pursuit guidance a controller steers by.
namespace groundplan {

constexpr double kCell = 4.0;
constexpr double kInf = std::numeric_limits<double>::infinity();
enum class Mode : std::uint8_t { Ground, Air };
inline const char* Name(Mode m) { return m == Mode::Air ? "air" : "ground"; }

struct Cell {
    float ground = std::numeric_limits<float>::quiet_NaN();  // surface height, NaN = unmapped
    float nz = std::numeric_limits<float>::quiet_NaN();      // surface normal z (1 = flat), NaN = unknown
    bool water = false;      // a water surface (driving refused)
    bool solidLow = false;   // something solid at hull height above the ground (a wall, a rock, a building)
    bool solidHigh = false;  // something solid at the air layer (flying refused here)
    bool road = false;       // a road or paved lane
    std::uint8_t contacts = 0;  // bumps remembered here
    bool Known() const { return std::isfinite(ground); }
};

struct Limits {
    double halfWidth = 1.4, halfLength = 2.6, height = 2.0;
    double maxStep = 1.2;       // m of height change across one 4 m cell edge the wheels take
    double minNz = 0.8;         // steepest drivable ground (normal z; 0.8 = 37 degrees)
    double topSpeed = 30, cruise = 22;  // m/s
    double latAccel = 4.0, accel = 3.0, decel = 6.0;  // m/s^2
    double minTurnRadius = 6;   // m
    bool canFly = true;
    double airFactor = 3.0;     // cost of an air metre against a flat ground metre (> 1 prefers the ground)
    double takeoff = 40, landing = 30;  // metres of ground the mode changes are worth
    double unknownExtra = 0.6;  // extra per metre over unmapped ground (optimistic: the sensing maps it as we go)
    double nearWallExtra = 0.8; // extra per metre right beside a solid
    double slopeExtra = 1.5;    // extra per metre at the steepest drivable slope
    double contactExtra = 0.3;  // extra per metre per remembered bump (up to 5)
    double roadFactor = 0.7;    // road metres cost this share
    double airClear = 6;        // m the air layer flies over the local ground
    double unknownSpeed = 14;   // m/s cap over unmapped ground
    double modeChangeSpeed = 4; // m/s at a take-off or landing
};

struct Grid {
    int ox = 0, oy = 0, n = 0;
    std::vector<Cell> cells;
    static int Index(double v) { return int(std::floor(v / kCell)); }
    double X(int i) const { return (ox + i + 0.5) * kCell; }
    double Y(int j) const { return (oy + j + 0.5) * kCell; }
    bool Inside(int i, int j) const { return i >= 0 && j >= 0 && i < n && j < n; }
    const Cell& At(int i, int j) const { return cells[std::size_t(j) * n + i]; }
    Cell& At(int i, int j) { return cells[std::size_t(j) * n + i]; }
    void Resize(int originX, int originY, int size) { ox = originX; oy = originY; n = size; cells.assign(std::size_t(size) * size, Cell{}); }
};

struct Waypoint { double x = 0, y = 0, z = 0; Mode mode = Mode::Ground; double speed = 0; bool unknown = false; };
struct Plan {
    bool ok = false, reachesGoal = false;
    std::vector<Waypoint> pts;
    double cost = 0, groundMetres = 0, airMetres = 0, unknownMetres = 0;
    unsigned modeChanges = 0, expanded = 0;
    const char* why = "";
};

namespace detail {
// Per-cell traversal facts the search reads, precomputed once per grid.
struct Facts {
    std::vector<unsigned char> drive, fly;  // passable on the ground / in the air
    std::vector<float> perMetre;            // ground cost per metre in this cell (before road)
    std::vector<float> height;              // ground height (unknown filled from neighbours or the hint)
};
inline double Lerp(double a, double b, double t) { return a + (b - a) * t; }

inline Facts Prepare(const Grid& g, const Limits& lim, double hintZ) {
    Facts f;
    const std::size_t total = std::size_t(g.n) * g.n;
    f.drive.assign(total, 0); f.fly.assign(total, 0); f.perMetre.assign(total, float(kInf)); f.height.assign(total, float(hintZ));
    // Distance (cells) to the nearest solid at hull height: a chamfer transform, two passes.
    std::vector<float> wall(total, 1e6f);
    for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i) if (g.At(i, j).solidLow) wall[std::size_t(j) * g.n + i] = 0;
    auto relax = [&](int i, int j, int a, int b, float w) {
        if (!g.Inside(a, b)) return;
        auto& here = wall[std::size_t(j) * g.n + i];
        here = std::min(here, wall[std::size_t(b) * g.n + a] + w);
    };
    for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i) { relax(i, j, i - 1, j, 1); relax(i, j, i, j - 1, 1); relax(i, j, i - 1, j - 1, 1.4142f); relax(i, j, i + 1, j - 1, 1.4142f); }
    for (int j = g.n - 1; j >= 0; --j) for (int i = g.n - 1; i >= 0; --i) { relax(i, j, i + 1, j, 1); relax(i, j, i, j + 1, 1); relax(i, j, i + 1, j + 1, 1.4142f); relax(i, j, i - 1, j + 1, 1.4142f); }
    // Heights: known cells as measured; unknown cells take the mean of known neighbours (two passes) or the hint.
    for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i) if (g.At(i, j).Known()) f.height[std::size_t(j) * g.n + i] = g.At(i, j).ground;
    for (int pass = 0; pass < 2; ++pass)
        for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i) {
            if (g.At(i, j).Known()) continue;
            double sum = 0; int count = 0;
            for (int b = j - 1; b <= j + 1; ++b) for (int a = i - 1; a <= i + 1; ++a) if (g.Inside(a, b) && (g.At(a, b).Known() || pass)) { sum += f.height[std::size_t(b) * g.n + a]; ++count; }
            if (count) f.height[std::size_t(j) * g.n + i] = float(sum / count);
        }
    const double clear = lim.halfWidth + kCell * 0.5;  // a solid cell centre closer than this touches the hull
    for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i) {
        const std::size_t k = std::size_t(j) * g.n + i;
        const Cell& c = g.At(i, j);
        f.fly[k] = lim.canFly && !c.solidHigh;
        const double wallM = wall[k] * kCell;
        if (c.water || c.solidLow || wallM < clear) continue;
        double nz = c.nz;
        if (!std::isfinite(nz) && c.Known()) {  // slope from the neighbours' heights
            double gx = 0, gy = 0; int nx = 0, ny = 0;
            if (g.Inside(i - 1, j) && g.Inside(i + 1, j) && g.At(i - 1, j).Known() && g.At(i + 1, j).Known()) { gx = (g.At(i + 1, j).ground - g.At(i - 1, j).ground) / (2 * kCell); nx = 1; }
            if (g.Inside(i, j - 1) && g.Inside(i, j + 1) && g.At(i, j - 1).Known() && g.At(i, j + 1).Known()) { gy = (g.At(i, j + 1).ground - g.At(i, j - 1).ground) / (2 * kCell); ny = 1; }
            if (nx || ny) nz = 1.0 / std::sqrt(1 + gx * gx + gy * gy);
        }
        if (std::isfinite(nz) && nz < lim.minNz) continue;
        double extra = 0;
        if (std::isfinite(nz)) extra += lim.slopeExtra * std::clamp((1 - nz) / std::max(1e-3, 1 - lim.minNz), 0.0, 1.0);
        if (!c.Known()) extra += lim.unknownExtra;
        if (wallM < clear + 6) extra += lim.nearWallExtra * (1 - (wallM - clear) / 6);
        extra += lim.contactExtra * std::min<int>(c.contacts, 5);
        f.drive[k] = 1;
        f.perMetre[k] = float(1 + extra);
    }
    return f;
}
}  // namespace detail

// The cost of driving straight from a to b across the grid (sampled every metre), kInf when any part is undrivable.
inline double LineCost(const Grid& g, const detail::Facts& f, const Limits& lim, double ax, double ay, double bx, double by, Mode mode) {
    const double len = std::hypot(bx - ax, by - ay);
    const int steps = std::max(1, int(std::ceil(len)));
    double cost = 0, lastH = std::numeric_limits<double>::quiet_NaN();
    for (int s = 0; s <= steps; ++s) {
        const double t = double(s) / steps, x = ax + (bx - ax) * t, y = ay + (by - ay) * t;
        const int i = Grid::Index(x) - g.ox, j = Grid::Index(y) - g.oy;
        if (!g.Inside(i, j)) return kInf;
        const std::size_t k = std::size_t(j) * g.n + i;
        if (mode == Mode::Air) { if (!f.fly[k]) return kInf; if (s) cost += len / steps * lim.airFactor; continue; }
        if (!f.drive[k]) return kInf;
        const double h = f.height[k];
        if (std::isfinite(lastH) && std::fabs(h - lastH) > lim.maxStep * (len / steps) / kCell + 0.05 && g.At(i, j).Known()) return kInf;
        lastH = h;
        if (s) cost += len / steps * f.perMetre[k] * (g.At(i, j).road ? lim.roadFactor : 1.0);
    }
    return cost;
}

// Searches from (sx, sy) to (gx, gy). goalZ: the goal's height (NaN: any). A goal outside the grid or unreachable gives the
// cheapest partial route toward it (reachesGoal false). hintZ: the height used for unmapped cells.
inline Plan Search(const Grid& g, const Limits& lim, double sx, double sy, double sz, Mode startMode, double gx, double gy, double goalZ, double goalStop = 3) {
    Plan plan;
    if (g.n < 3 || g.cells.size() != std::size_t(g.n) * g.n) { plan.why = "no grid"; return plan; }
    const double hint = std::isfinite(sz) ? sz : 0;
    const auto f = detail::Prepare(g, lim, hint);
    const int si = Grid::Index(sx) - g.ox, sj = Grid::Index(sy) - g.oy;
    if (!g.Inside(si, sj)) { plan.why = "start outside the grid"; return plan; }
    const int gi = Grid::Index(gx) - g.ox, gj = Grid::Index(gy) - g.oy;
    const bool goalInside = g.Inside(gi, gj);
    const std::size_t plane = std::size_t(g.n) * g.n;
    std::vector<double> cost(plane * 2, kInf);
    std::vector<int> parent(plane * 2, -1);
    using Entry = std::pair<double, int>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    const double hScale = std::min(1.0, lim.roadFactor);  // never above the cheapest metre: admissible
    auto heuristic = [&](int i, int j) { return std::hypot(g.X(i) - gx, g.Y(j) - gy) * hScale; };
    const int start = int((std::size_t(sj) * g.n + si) * 2 + (startMode == Mode::Air ? 1 : 0));
    if (startMode == Mode::Air && !lim.canFly) { plan.why = "starts in the air but cannot fly"; return plan; }
    cost[start] = 0; open.emplace(heuristic(si, sj), start);
    // A goal is met on the ground when that cell is drivable and (no goal height, or the ground is near it); in the air otherwise.
    auto goalMet = [&](int node) {
        const int cell = node / 2, i = cell % g.n, j = cell / g.n;
        if (std::hypot(g.X(i) - gx, g.Y(j) - gy) > std::max(goalStop, kCell * 0.75)) return false;
        const bool air = node & 1;
        const double ground = f.height[cell];
        const bool high = std::isfinite(goalZ) && goalZ - ground > lim.airClear + 4;
        return air ? (high || !f.drive[cell]) : !high;
    };
    // A partial route ends where it gets closest to the goal; its cost only breaks near-ties (a quarter metre per unit),
    // so a long detour to gain a few metres is not taken.
    constexpr double kPartialCostWeight = 0.25;
    int best = start, goal = -1;
    double bestScore = std::hypot(sx - gx, sy - gy);
    while (!open.empty()) {
        const auto [fScore, node] = open.top(); open.pop();
        const double here = cost[node];
        if (fScore > here + heuristic((node / 2) % g.n, (node / 2) / g.n) + 1e-6) continue;
        ++plan.expanded;
        const int cell = node / 2, i = cell % g.n, j = cell / g.n;
        const bool air = node & 1;
        if (goalInside && goalMet(node)) { goal = node; break; }
        const double score = std::hypot(g.X(i) - gx, g.Y(j) - gy) + kPartialCostWeight * here;
        if (score < bestScore) { bestScore = score; best = node; }
        auto push = [&](int to, double add) {
            const double c = here + add;
            if (c < cost[to]) { cost[to] = c; parent[to] = node; open.emplace(c + heuristic((to / 2) % g.n, (to / 2) / g.n), to); }
        };
        // Mode change in place.
        if (!air && lim.canFly && f.fly[cell]) push(cell * 2 + 1, lim.takeoff);
        if (air && f.drive[cell]) push(cell * 2, lim.landing);
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            if (!dx && !dy) continue;
            const int a = i + dx, b = j + dy;
            if (!g.Inside(a, b)) continue;
            const int to = b * g.n + a;
            const double len = kCell * (dx && dy ? 1.4142135623730951 : 1.0);
            if (air) {
                if (!f.fly[to]) continue;
                if (dx && dy && (!f.fly[j * g.n + a] || !f.fly[b * g.n + i])) continue;
                push(to * 2 + 1, len * lim.airFactor);
                continue;
            }
            const bool leavingStart = node == start;  // the start cell may read as blocked (its own hull, a kerb): always leave it
            if (!f.drive[to] || (!f.drive[cell] && !leavingStart)) continue;
            if (dx && dy && (!f.drive[j * g.n + a] || !f.drive[b * g.n + i])) continue;
            const Cell& A = g.At(i, j); const Cell& B = g.At(a, b);
            if (A.Known() && B.Known() && std::fabs(B.ground - A.ground) > lim.maxStep * (len / kCell)) continue;
            const double per = 0.5 * (double(f.perMetre[cell] < kInf ? f.perMetre[cell] : f.perMetre[to]) + f.perMetre[to]);
            push(to * 2, len * per * (A.road && B.road ? lim.roadFactor : 1.0));
        }
    }
    const int end = goal >= 0 ? goal : best;
    if (end == start && goal < 0) { plan.why = std::hypot(sx - gx, sy - gy) <= goalStop ? "already there" : "no way out of the start"; plan.ok = std::hypot(sx - gx, sy - gy) <= goalStop; return plan; }
    plan.reachesGoal = goal >= 0;
    plan.cost = cost[end];
    // Cells back to the start.
    std::vector<int> chain;
    for (int n = end; n >= 0; n = parent[n]) chain.push_back(n);
    std::reverse(chain.begin(), chain.end());
    std::vector<Waypoint> raw;
    for (std::size_t k = 0; k < chain.size(); ++k) {
        const int cell = chain[k] / 2, i = cell % g.n, j = cell / g.n;
        Waypoint w; w.mode = (chain[k] & 1) ? Mode::Air : Mode::Ground;
        w.x = k == 0 ? sx : g.X(i); w.y = k == 0 ? sy : g.Y(j);
        w.unknown = !g.At(i, j).Known();
        w.z = f.height[cell] + (w.mode == Mode::Air ? lim.airClear : 0);
        if (!raw.empty() && raw.back().mode != w.mode && std::hypot(raw.back().x - w.x, raw.back().y - w.y) < 1e-6) {
            // A mode change in place: keep both points (the change happens here).
        }
        raw.push_back(w);
    }
    if (plan.reachesGoal) { Waypoint w = raw.back(); w.x = gx; w.y = gy; if (std::isfinite(goalZ) && w.mode == Mode::Air) w.z = goalZ; raw.push_back(w); }
    // String pulling inside each run of one mode: skip to the farthest point whose straight line costs no more than the chain.
    std::vector<Waypoint> out;
    std::size_t a = 0;
    out.push_back(raw[0]);
    while (a + 1 < raw.size()) {
        std::size_t b = a + 1;
        double chainCost = 0;
        std::size_t bestB = a + 1;
        for (std::size_t c = a + 1; c < raw.size() && raw[c].mode == raw[a].mode && (c == a + 1 || raw[c - 1].mode == raw[a].mode); ++c) {
            chainCost += LineCost(g, f, lim, raw[c - 1].x, raw[c - 1].y, raw[c].x, raw[c].y, raw[a].mode);
            if (!std::isfinite(chainCost)) break;
            if (c - a > 60) break;  // keep corners within ~240 m
            const double direct = LineCost(g, f, lim, raw[a].x, raw[a].y, raw[c].x, raw[c].y, raw[a].mode);
            if (direct <= chainCost * 1.02 + 0.5) bestB = c;
            b = c;
        }
        (void)b;
        if (raw[a + 1].mode != raw[a].mode) bestB = a + 1;  // the mode change point itself
        out.push_back(raw[bestB]);
        a = bestB;
    }
    // Totals.
    for (std::size_t k = 1; k < out.size(); ++k) {
        const double len = std::hypot(out[k].x - out[k - 1].x, out[k].y - out[k - 1].y);
        if (out[k].mode != out[k - 1].mode) ++plan.modeChanges;
        else if (out[k].mode == Mode::Air) plan.airMetres += len;
        else { plan.groundMetres += len; if (out[k].unknown) plan.unknownMetres += len; }
    }
    plan.pts = std::move(out);
    plan.ok = plan.pts.size() >= 2;
    plan.why = plan.reachesGoal ? "reaches the goal" : goalInside ? "goal unreachable: partial route" : "goal beyond the map: partial route";
    return plan;
}

// The speed each waypoint may be passed at: corner radius (lateral grip), unmapped ground, mode changes and the end,
// then braking and acceleration limits along the route.
inline void SpeedProfile(Plan& plan, const Limits& lim, double startSpeed, bool stopAtEnd) {
    auto& p = plan.pts;
    if (p.size() < 2) return;
    for (std::size_t k = 0; k < p.size(); ++k) {
        double v = p[k].mode == Mode::Air ? lim.topSpeed : lim.cruise;
        if (k > 0 && k + 1 < p.size() && p[k].mode == p[k - 1].mode && p[k].mode == p[k + 1].mode) {
            const double ax = p[k].x - p[k - 1].x, ay = p[k].y - p[k - 1].y, bx = p[k + 1].x - p[k].x, by = p[k + 1].y - p[k].y;
            const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
            if (la > 1e-3 && lb > 1e-3) {
                const double turn = std::acos(std::clamp((ax * bx + ay * by) / (la * lb), -1.0, 1.0));
                if (turn > 0.05) {
                    const double radius = std::max(lim.minTurnRadius * 0.5, 0.5 * std::min(la, lb) / std::tan(turn * 0.5));
                    v = std::min(v, std::sqrt(lim.latAccel * radius));
                }
            }
        }
        if (p[k].mode == Mode::Ground && p[k].unknown) v = std::min(v, lim.unknownSpeed);
        if ((k > 0 && p[k].mode != p[k - 1].mode) || (k + 1 < p.size() && p[k + 1].mode != p[k].mode)) v = std::min(v, lim.modeChangeSpeed);
        p[k].speed = v;
    }
    if (stopAtEnd) p.back().speed = 0;
    p.front().speed = std::min(p.front().speed, std::max(startSpeed, p.front().speed));
    for (std::size_t k = p.size() - 1; k-- > 0;) {  // braking ahead of slow points
        const double d = std::hypot(p[k + 1].x - p[k].x, p[k + 1].y - p[k].y);
        p[k].speed = std::min(p[k].speed, std::sqrt(p[k + 1].speed * p[k + 1].speed + 2 * lim.decel * d));
    }
    p.front().speed = std::min(p.front().speed, std::max(0.0, startSpeed) + 2.0);
    for (std::size_t k = 1; k < p.size(); ++k) {  // acceleration from the slow points
        const double d = std::hypot(p[k].x - p[k - 1].x, p[k].y - p[k - 1].y);
        p[k].speed = std::min(p[k].speed, std::sqrt(p[k - 1].speed * p[k - 1].speed + 2 * lim.accel * d));
    }
}

// Pure-pursuit guidance along a plan.
struct Guidance {
    bool ok = false, done = false, offPath = false;
    Mode mode = Mode::Ground, nextMode = Mode::Ground;
    double tx = 0, ty = 0, tz = 0;       // the point to steer at
    double bearing = 0;                  // world bearing to it, atan2(dx, dy)
    double speed = 0;                    // m/s the route allows here (corner, braking for what is ahead)
    double cross = 0;                    // m off the route
    double toModeChange = kInf;          // m along the route to the next mode change
    double remaining = 0;                // m along the route to its end
    std::size_t segment = 0, segments = 0;
};
inline Guidance Follow(const Plan& plan, double x, double y, double speed, std::size_t& cursor, const Limits& lim) {
    Guidance out;
    const auto& p = plan.pts;
    if (!plan.ok || p.size() < 2) return out;
    cursor = std::min(cursor, p.size() - 2);
    // The closest point within the next few segments (never back up the route).
    double bestD = kInf, bestT = 0; std::size_t bestS = cursor;
    for (std::size_t s = cursor; s + 1 < p.size() && s < cursor + 8; ++s) {
        const double ax = p[s].x, ay = p[s].y, bx = p[s + 1].x, by = p[s + 1].y;
        const double lx = bx - ax, ly = by - ay, l2 = lx * lx + ly * ly;
        const double t = l2 > 1e-9 ? std::clamp(((x - ax) * lx + (y - ay) * ly) / l2, 0.0, 1.0) : 1.0;
        const double d = std::hypot(ax + lx * t - x, ay + ly * t - y);
        if (d < bestD - 1e-6) { bestD = d; bestT = t; bestS = s; }
    }
    cursor = bestS;
    out.cross = bestD;
    out.segment = bestS; out.segments = p.size() - 1;
    out.mode = p[bestS + 1].mode == p[bestS].mode ? p[bestS].mode : p[bestS].mode;
    out.offPath = bestD > std::max(8.0, 2 * lim.halfWidth + 4);
    // Distance along the route to its end and to the next mode change.
    const double segLen = std::hypot(p[bestS + 1].x - p[bestS].x, p[bestS + 1].y - p[bestS].y);
    double along = segLen * (1 - bestT);
    out.nextMode = out.mode;
    if (p[bestS + 1].mode != out.mode) { out.toModeChange = 0; out.nextMode = p[bestS + 1].mode; }
    for (std::size_t s = bestS + 1; s + 1 < p.size(); ++s) {
        if (!std::isfinite(out.toModeChange) && p[s + 1].mode != p[s].mode) { out.toModeChange = along; out.nextMode = p[s + 1].mode; }
        along += std::hypot(p[s + 1].x - p[s].x, p[s + 1].y - p[s].y);
    }
    if (!std::isfinite(out.toModeChange) && p.back().mode != out.mode) out.toModeChange = along;
    out.remaining = along;
    // The look-ahead point, stopping at a mode change (the change happens there, not cut across).
    double look = std::clamp(0.8 * std::fabs(speed) + 6.0, 6.0, 30.0);
    double px = p[bestS].x + (p[bestS + 1].x - p[bestS].x) * bestT, py = p[bestS].y + (p[bestS + 1].y - p[bestS].y) * bestT;
    double pz = p[bestS].z + (p[bestS + 1].z - p[bestS].z) * bestT;
    std::size_t s = bestS;
    double left = look;
    while (left > 0) {
        const double ex = p[s + 1].x - px, ey = p[s + 1].y - py, el = std::hypot(ex, ey);
        if (el >= left) { px += ex / el * left; py += ey / el * left; pz += (p[s + 1].z - pz) * (left / el); break; }
        left -= el; px = p[s + 1].x; py = p[s + 1].y; pz = p[s + 1].z;
        if (s + 2 >= p.size() || p[s + 2].mode != p[s + 1].mode || p[s + 1].mode != out.mode) break;
        ++s;
    }
    out.tx = px; out.ty = py; out.tz = pz;
    out.bearing = std::atan2(px - x, py - y);
    // Speed: the profile at this point, and braking for every slower point within reach ahead.
    const double v0 = p[bestS].speed + (p[bestS + 1].speed - p[bestS].speed) * bestT;
    double v = v0;
    double d = segLen * (1 - bestT);
    for (std::size_t k = bestS + 1; k < p.size() && d < 300; ++k) {
        v = std::min(v, std::sqrt(p[k].speed * p[k].speed + 2 * lim.decel * d));
        if (k + 1 < p.size()) d += std::hypot(p[k + 1].x - p[k].x, p[k + 1].y - p[k].y);
    }
    out.speed = std::max(0.0, v);
    out.done = out.remaining < 2.0 && bestS + 2 >= p.size();
    out.ok = true;
    return out;
}

}  // namespace groundplan
