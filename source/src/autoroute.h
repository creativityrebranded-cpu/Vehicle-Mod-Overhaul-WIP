#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <queue>
#include <vector>

// 3D auto-routing for auto-built geometry (2026-11-03, F7 spec 7d; the user: "avoid obstacles and find the best place
// so like A*", "in 3d"). Pure: no engine calls. The editor fills the voxel grid from what the mod already remembers
// (nav map samples, learned ground) on the main thread and runs Search on a worker, as groundnav does.
//
// Voxels are 4 m across and 2 m high. A drive cell must stand on something solid directly below it (ground or a deck),
// so a bridge over a road and a tunnel under a roof both fall out of the same rule. Solids are inflated by the chosen
// cross-section (half-width sideways, height upwards), so the route's field fits by construction. Unknown cells are
// allowed at a higher cost and reported, never treated as known-clear. Crossing another route's field costs a lot and
// is reported, so a junction can be proposed. The grid path is string-pulled in 3D (never costlier than the grid path).
namespace autoroute {

constexpr double kXY = 4.0, kZ = 2.0;
enum class State : std::uint8_t { Unknown, Free, Solid };

struct Voxel { State state = State::Unknown; bool field = false; };  // field: inside another route's field

struct Grid {
    double ox = 0, oy = 0, oz = 0;  // world position of voxel (0,0,0)'s corner
    int nx = 0, ny = 0, nz = 0;
    std::vector<Voxel> v;
    void Resize(double x, double y, double z, int sx, int sy, int sz) { ox = x; oy = y; oz = z; nx = sx; ny = sy; nz = sz; v.assign(std::size_t(sx) * sy * sz, {}); }
    bool Inside(int i, int j, int k) const { return i >= 0 && j >= 0 && k >= 0 && i < nx && j < ny && k < nz; }
    Voxel& At(int i, int j, int k) { return v[(std::size_t(k) * ny + j) * nx + i]; }
    const Voxel& At(int i, int j, int k) const { return v[(std::size_t(k) * ny + j) * nx + i]; }
    double X(int i) const { return ox + (i + .5) * kXY; }
    double Y(int j) const { return oy + (j + .5) * kXY; }
    double Z(int k) const { return oz + (k + .5) * kZ; }
    int I(double x) const { return int(std::floor((x - ox) / kXY)); }
    int J(double y) const { return int(std::floor((y - oy) / kXY)); }
    int K(double z) const { return int(std::floor((z - oz) / kZ)); }
};

struct Params {
    bool drive = true;          // every cell needs support below (flat tile / drive wires); false: may leave the ground
    double halfWidth = 3.5;     // the chosen cross-section's half width plus margin
    double height = 4.0;        // headroom the cross-section needs above the floor
    double maxStep = 1.2;       // m of rise per 4 m the wheels take
    double unknownExtra = 1.5;  // extra per metre through unknown cells
    double nearWallExtra = .6;  // extra per metre next to a solid
    double climbExtra = 1.0;    // extra per metre climbed or descended
    double airFactor = 3.0;     // cost of an unsupported metre (hover only)
    double fieldExtra = 25.0;   // extra per metre inside another route's field
    std::size_t budget = 400000;  // expanded nodes before giving up
};

struct Point { double x = 0, y = 0, z = 0; bool unknown = false; };
struct Result {
    bool ok = false;
    std::vector<Point> points;    // simplified route points, start and goal included
    double cost = 0;
    std::size_t expanded = 0, unknownCells = 0, fieldCells = 0;
    const char* why = "";         // ok, no_path, budget, start_blocked, goal_blocked, outside
};

namespace detail {
// The cross-section fits here: no solid within the half width sideways or the headroom upwards.
inline bool Clear(const Grid& g, int i, int j, int k, const Params& p) {
    const int r = int(std::ceil(std::max(0.0, p.halfWidth - kXY * .5) / kXY)), up = std::max(1, int(std::ceil(p.height / kZ)));
    for (int dk = 0; dk < up; ++dk) for (int dj = -r; dj <= r; ++dj) for (int di = -r; di <= r; ++di) {
        const int a = i + di, b = j + dj, c = k + dk;
        if (!g.Inside(a, b, c)) { if (dk == 0 && di == 0 && dj == 0) return false; continue; }
        if (g.At(a, b, c).state == State::Solid) return false;
    }
    return true;
}
inline bool Supported(const Grid& g, int i, int j, int k) { return k > 0 && g.At(i, j, k - 1).state == State::Solid; }
inline bool Passable(const Grid& g, int i, int j, int k, const Params& p) {
    if (!g.Inside(i, j, k) || g.At(i, j, k).state == State::Solid) return false;
    if (p.drive && !Supported(g, i, j, k)) return false;
    return Clear(g, i, j, k, p);
}
inline bool NearWall(const Grid& g, int i, int j, int k) {
    for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) if ((di || dj) && g.Inside(i + di, j + dj, k) && g.At(i + di, j + dj, k).state == State::Solid) return true;
    return false;
}
inline double CellCost(const Grid& g, int i, int j, int k, const Params& p, double metres, double climb) {
    const auto& c = g.At(i, j, k);
    double factor = 1;
    if (c.state == State::Unknown) factor += p.unknownExtra;
    if (c.field) factor += p.fieldExtra;
    if (NearWall(g, i, j, k)) factor += p.nearWallExtra;
    if (!p.drive && !Supported(g, i, j, k)) factor *= p.airFactor;
    return metres * factor + std::fabs(climb) * p.climbExtra;
}
// A straight 3D segment is usable when every sample along it is passable, keeps drive steps legal, and costs no more.
inline bool Line(const Grid& g, Point a, Point b, const Params& p, double& cost) {
    const double length = std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
    const int steps = std::max(1, int(std::ceil(length / 1.0)));
    cost = 0; int pi = -1, pj = -1, pk = -1;
    for (int s = 0; s <= steps; ++s) {
        const double f = double(s) / steps;
        const int i = g.I(a.x + (b.x - a.x) * f), j = g.J(a.y + (b.y - a.y) * f), k = g.K(a.z + (b.z - a.z) * f);
        if (i == pi && j == pj && k == pk) continue;
        if (!Passable(g, i, j, k, p)) return false;
        if (p.drive && pi >= 0 && std::abs(k - pk) * kZ > p.maxStep * std::max(1, std::max(std::abs(i - pi), std::abs(j - pj))) + 1e-9) return false;
        cost += CellCost(g, i, j, k, p, length / steps * (pi < 0 ? 0 : 1), pi < 0 ? 0 : (k - pk) * kZ);
        pi = i; pj = j; pk = k;
    }
    return true;
}
}

// A* over 26 neighbours from the cell holding `from` to the cell holding `to`.
inline Result Search(const Grid& g, Point from, Point to, const Params& p) {
    Result out;
    const int si = g.I(from.x), sj = g.J(from.y), sk = g.K(from.z), gi = g.I(to.x), gj = g.J(to.y), gk = g.K(to.z);
    if (!g.Inside(si, sj, sk) || !g.Inside(gi, gj, gk)) { out.why = "outside"; return out; }
    if (!detail::Passable(g, si, sj, sk, p)) { out.why = "start_blocked"; return out; }
    if (!detail::Passable(g, gi, gj, gk, p)) { out.why = "goal_blocked"; return out; }
    const std::size_t total = std::size_t(g.nx) * g.ny * g.nz;
    auto index = [&](int i, int j, int k) { return (std::size_t(k) * g.ny + j) * g.nx + i; };
    std::vector<double> best(total, std::numeric_limits<double>::infinity());
    std::vector<std::int64_t> parent(total, -1);
    using Node = std::pair<double, std::size_t>;
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    auto h = [&](int i, int j, int k) { return std::hypot((i - gi) * kXY, (j - gj) * kXY, (k - gk) * kZ); };
    const auto start = index(si, sj, sk), goal = index(gi, gj, gk);
    best[start] = 0; open.push({ h(si, sj, sk), start });
    while (!open.empty()) {
        const auto [f, at] = open.top(); open.pop();
        const int k = int(at / (std::size_t(g.nx) * g.ny)), j = int(at / g.nx % g.ny), i = int(at % g.nx);
        if (f - h(i, j, k) > best[at] + 1e-9) continue;
        if (at == goal) break;
        if (++out.expanded > p.budget) { out.why = "budget"; return out; }
        for (int dk = -1; dk <= 1; ++dk) for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) {
            if (!di && !dj && !dk) continue;
            const int a = i + di, b = j + dj, c = k + dk;
            if (!detail::Passable(g, a, b, c, p)) continue;
            if (p.drive && std::abs(dk) * kZ > p.maxStep * std::max(std::abs(di), std::abs(dj)) + 1e-9) continue;  // no straight climbs on wheels
            const double metres = std::hypot(di * kXY, dj * kXY, dk * kZ);
            const double next = best[at] + detail::CellCost(g, a, b, c, p, metres, dk * kZ);
            const auto to2 = index(a, b, c);
            if (next < best[to2] - 1e-9) { best[to2] = next; parent[to2] = std::int64_t(at); open.push({ next + h(a, b, c), to2 }); }
        }
    }
    if (!std::isfinite(best[goal])) { out.why = "no_path"; return out; }
    std::vector<Point> path;
    for (std::int64_t at = std::int64_t(goal); at >= 0; at = parent[std::size_t(at)]) {
        const auto u = std::size_t(at);
        const int k = int(u / (std::size_t(g.nx) * g.ny)), j = int(u / g.nx % g.ny), i = int(u % g.nx);
        const auto& c = g.At(i, j, k);
        path.push_back({ g.X(i), g.Y(j), p.drive ? g.oz + k * kZ : g.Z(k), c.state == State::Unknown });  // drive points sit on their surface
        if (c.state == State::Unknown) ++out.unknownCells;
        if (c.field) ++out.fieldCells;
    }
    std::reverse(path.begin(), path.end());
    path.front() = { from.x, from.y, from.z, path.front().unknown };
    path.back() = { to.x, to.y, to.z, path.back().unknown };
    // String pulling: from each kept point, jump to the farthest later point a straight usable segment reaches at
    // no more cost than the grid path between them.
    std::vector<double> along(path.size(), 0);
    for (std::size_t n = 1; n < path.size(); ++n) along[n] = along[n - 1] + std::hypot(path[n].x - path[n - 1].x, path[n].y - path[n - 1].y, path[n].z - path[n - 1].z);
    out.points.push_back(path.front());
    for (std::size_t at = 0; at + 1 < path.size();) {
        std::size_t reach = at + 1;
        for (std::size_t n = path.size() - 1; n > at + 1; --n) {
            double cost = 0;
            if (detail::Line(g, path[at], path[n], p, cost) && cost <= (best[index(g.I(path[n].x), g.J(path[n].y), g.K(path[n].z))] - best[index(g.I(path[at].x), g.J(path[at].y), g.K(path[at].z))]) + 1e-6 + (along[n] - along[at]) * 1e-3) { reach = n; break; }
        }
        bool unknown = false; for (std::size_t n = at + 1; n <= reach; ++n) unknown = unknown || path[n].unknown;
        auto point = path[reach]; point.unknown = unknown; out.points.push_back(point); at = reach;
    }
    out.cost = best[goal]; out.ok = true; out.why = "ok";
    return out;
}
}
