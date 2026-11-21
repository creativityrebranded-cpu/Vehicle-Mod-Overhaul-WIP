#pragma once
// CC-1 (2026-11-19, Plan, before the rollouts): corridor centring. The authored lane line is not always where the road is: on the
// lc_ring_road switchback it runs ~1.4 m from the right barrier while the road is wide on the left, so a car holding the lane centre
// drives 5-20 cm from that barrier and every collision rule sees the barrier at its corner (13:01 and 13:10 runs). One rule, any road:
// measure the hard edges beside the lane ahead (solids standing in the body's band, judged against the road under them), and shift
// the line the car follows sideways by the least amount that keeps `margin` between the body and the nearest edge. With room on both
// sides the line stays where it is; with too little room for the margin on both, the line goes midway between the edges. A solid in the
// middle of the lane is not an edge (Avoid steers round it). The shift eases (Ease) so new points do not swing the line.
// CC-2 (2026-11-19, 13:57:20-13:57:50 hairpin exit): radar points expire and are re-sensed, so the right wall 0.84 m from the line was
// seen only now and then; "no edges" read as "clear", the target fell to 0 and the eased shift swung -0.52 -> -0.10 -> -0.56 -> -0.10
// until the car stood at the wall. Unknown is not clear: each car keeps the edge points it has measured (Memory) and Measure counts
// them with the scene's. A remembered point goes only when the car has passed it, it is far, it is old, or the current pose shows it
// was another level (the same RoadRef / height-band test). Not seeing it right now never clears it.
// Pure; tests in tests/planenergy_test.cpp.
#include "planscene.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace corridor {
namespace rules {
inline constexpr double margin = .5;     // m kept between the body's side and a measured edge when the road has room
inline constexpr double ahead = 15.0;    // m of lane ahead whose edges count
inline constexpr double behind = 2.0;    // m behind the car (its own length is still beside them)
inline constexpr double centreZone = .3; // a solid closer to the line than half the body minus this is an obstacle, not an edge
inline constexpr double rate = .6;       // m/s the followed line may move sideways
inline constexpr double hold = .1;       // m: a new target this close to the current shift does not move it
// CC-2: the remembered edges.
inline constexpr double cell = .5;       // m: one remembered point per cell of this size (a re-sensed point refreshes it)
inline constexpr int kept = 256;         // points remembered per car at most (the oldest gives way)
inline constexpr double passed = 5.0;    // m: a point this far behind the car along the route is passed
inline constexpr double far = 40.0;      // m: a point this far from the car is forgotten
inline constexpr double age = 30.0;      // s: a point not seen again for this long is forgotten
}

// CC-2: a measured edge point, in world coordinates, as the scene had it when it was last seen.
struct EdgePoint {
    double x = 0, y = 0, top = 0, base = 0, road = std::numeric_limits<double>::quiet_NaN(), half = .05, seenAt = 0;
};
struct Memory {
    std::vector<EdgePoint> points;  // at most rules::kept
};

struct Edges {
    double left = -std::numeric_limits<double>::infinity(), right = std::numeric_limits<double>::infinity();  // lane frame, + = right
    double target = 0;
    int solids = 0;      // scene solids that are edges now
    int remembered = 0;  // CC-2: remembered points that are edges now (not seen in this scene)
    int memory = 0;      // CC-2: points the memory holds after this plan
    int known = 0;       // route edge bands / medians (band solids) that are edges of the road under the line: 0 none, 1 left, 2 right, 3 both
    const char* why = "clear";
};

// What a solid at (x, y) is to the corridor: out of the window, outside the car's height band (another level, rolled over, above the
// roof), in the lane's middle (Avoid's), or an edge (lat: its lateral offset, + = right).
enum class Kind { Outside, Band, Middle, Edge };
inline Kind Judge(const planscene::Scene& sc, double x, double y, double top, double base, double road, double station,
                  double carX, double carY, double carZ, const bicycle::Spec& k, double& st, double& lat) {
    double half = 0;
    sc.Route(x, y, st, lat, half, station);
    const double ref = planscene::Scene::RoadRef(road, x - carX, y - carY, carZ, true);
    if (top <= ref + k.climb || base >= ref + k.height) return Kind::Band;  // rolled over or above the roof (or another level's)
    if (st < station - rules::behind || st > station + rules::ahead) return Kind::Outside;
    if (std::fabs(lat) > half + 2.0) return Kind::Outside;            // beyond the lane's field: not this road's edge
    if (std::fabs(lat) < k.halfWidth - rules::centreZone) return Kind::Middle;  // in the lane's middle: an obstacle for Avoid
    return Kind::Edge;
}

// CC-2: drops the remembered points the car has passed, that are far or old, or that the current pose shows were another level.
inline void Forget(Memory& m, const planscene::Scene& sc, double station, double carX, double carY, double carZ, const bicycle::Spec& k, double now) {
    auto gone = [&](const EdgePoint& p) {
        if (now - p.seenAt > rules::age || p.seenAt > now + 1.0) return true;  // old (or from a clock that went back)
        if (std::hypot(p.x - carX, p.y - carY) > rules::far) return true;
        double st, lat;
        if (Judge(sc, p.x, p.y, p.top, p.base, p.road, station, carX, carY, carZ, k, st, lat) == Kind::Band) return true;  // another level
        return st < station - rules::passed;
    };
    m.points.erase(std::remove_if(m.points.begin(), m.points.end(), gone), m.points.end());
}

// CC-2: an edge point seen now: refreshes the point in its cell, else is added (the oldest gives way when the memory is full).
inline void Remember(Memory& m, const planscene::Solid& s, double now) {
    const EdgePoint p{ s.x, s.y, s.top, s.base, s.road, s.half, now };
    const double cx = std::floor(s.x / rules::cell), cy = std::floor(s.y / rules::cell);
    for (auto& q : m.points)
        if (std::floor(q.x / rules::cell) == cx && std::floor(q.y / rules::cell) == cy) { q = p; return; }
    if (int(m.points.size()) < rules::kept) { m.points.push_back(p); return; }
    auto oldest = std::min_element(m.points.begin(), m.points.end(), [](const EdgePoint& a, const EdgePoint& b) { return a.seenAt < b.seenAt; });
    *oldest = p;
}

// The edges beside the lane ahead of `station` and the shift they ask for. carZ: the car's floor (judges solids with no road of their own).
// memory (CC-2): this car's remembered edges; the scene's edges are added to it and the remembered ones count with them. now: seconds.
inline Edges Measure(const planscene::Scene& sc, double station, double carX, double carY, double carZ, const bicycle::Spec& k,
                     Memory* memory = nullptr, double now = 0) {
    Edges e;
    if (sc.route.size() < 2) { e.why = "no_route"; if (memory) e.memory = int(memory->points.size()); return e; }
    auto take = [&](double lat, double half) {
        if (lat >= 0) e.right = std::min(e.right, lat - half);
        else e.left = std::max(e.left, lat + half);
    };
    if (memory) Forget(*memory, sc, station, carX, carY, carZ, k, now);
    for (const auto& s : sc.solids) {
        if (s.soft || s.waits) continue;
        double st = 0, lat = 0;
        if (Judge(sc, s.x, s.y, s.top, s.base, s.road, station, carX, carY, carZ, k, st, lat) != Kind::Edge) continue;
        if (s.band) {  // a known edge (route edge band / median, planscene BuildKnownBandSolids): the same edge rule as a measured wall, only certain and never remembered
            e.known |= lat >= 0 ? 2 : 1; take(lat, s.half); continue;
        }
        ++e.solids;
        take(lat, s.half);
        if (memory) Remember(*memory, s, now);
    }
    if (memory) {
        for (const auto& p : memory->points) {
            if (p.seenAt >= now) continue;  // seen in this scene: counted above
            double st = 0, lat = 0;
            if (Judge(sc, p.x, p.y, p.top, p.base, p.road, station, carX, carY, carZ, k, st, lat) != Kind::Edge) continue;
            ++e.remembered;
            take(lat, p.half);
        }
        e.memory = int(memory->points.size());
    }
    const double need = k.halfWidth + rules::margin;
    const double lo = e.left + need, hi = e.right - need;  // the band of lane offsets that keep the margin on both sides
    if (lo <= 0 && 0 <= hi) { e.target = 0; e.why = e.solids || e.remembered ? "room" : "clear"; }
    else if (lo <= hi) { e.target = std::clamp(0.0, lo, hi); e.why = e.target < 0 ? "edge_right" : "edge_left"; }
    else if (std::isfinite(e.left) && std::isfinite(e.right)) { e.target = .5 * (e.left + e.right); e.why = "narrow_midway"; }
    else { e.target = std::isfinite(e.right) ? hi : lo; e.why = "one_edge"; }
    e.target = std::clamp(e.target, -sc.laneHalf, sc.laneHalf);
    return e;
}

// The followed line's shift eases toward the target: at most `rate` m/s, and a target within `hold` of it does not move it.
inline double Ease(double current, double target, double dt) {
    if (std::fabs(target - current) < rules::hold) return current;
    const double step = rules::rate * std::clamp(dt, 0.0, .5);
    return current + std::clamp(target - current, -step, step);
}

// Moves every route point `shift` m to the right of travel (negative: left) and restarts the stations along the moved line.
inline void ShiftRoute(planscene::Scene& sc, double shift) {
    const int n = int(sc.route.size());
    if (n < 2 || std::fabs(shift) < 1e-6) return;
    for (auto& r : sc.legal) { r.first -= shift; r.second -= shift; }  // known no-go space is fixed in the world: its offsets from the moved line change
    std::vector<planscene::RoutePoint> moved = sc.route;
    for (int i = 0; i < n; ++i) {
        const auto& a = sc.route[std::max(0, i - 1)];
        const auto& b = sc.route[std::min(n - 1, i + 1)];
        const double dx = b.x - a.x, dy = b.y - a.y, len = std::hypot(dx, dy);
        if (len < 1e-9) continue;
        moved[i].x += shift * dy / len;   // right of travel: (dy, -dx)
        moved[i].y -= shift * dx / len;
    }
    for (int i = 1; i < n; ++i) moved[i].station = moved[i - 1].station + std::hypot(moved[i].x - moved[i - 1].x, moved[i].y - moved[i - 1].y);
    sc.route = std::move(moved);
}
}  // namespace corridor
