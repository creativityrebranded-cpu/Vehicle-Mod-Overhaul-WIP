#pragma once
// Engine-independent route-editing model. This module does not install hooks,
// read map positions, write files, or claim that a route is safe to fly.
#include "trafficdata.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace routeeditor {
using Route = trafficdata::Route;
using Point = trafficdata::Vec;
enum class Context { Gameplay, SurfaceMap, OtherMap, OtherMenu };
enum class F7Action { HudEditor, RouteEditor, None };
// A placed point keeps its height; only one under the ground (plus the lift) is raised. Never lowered.
inline double PlacedHeight(double z, double ground, double lift) { return z < ground + lift ? ground + lift : z; }
// The next height level from z (a controller's steps): a point between levels goes to the nearest one in that direction.
inline double NextLevel(double z, int direction, double step) {
    const double at = z / step, levels = direction > 0 ? std::floor(at + 1e-9) + 1 : std::ceil(at - 1e-9) - 1;
    return levels * step;
}
inline F7Action F7(Context context) {
    if (context == Context::SurfaceMap) return F7Action::RouteEditor;
    return context == Context::Gameplay ? F7Action::HudEditor : F7Action::None;
}
inline bool Finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline double Distance(Point a, Point b) { return std::hypot(a.x-b.x,a.y-b.y,a.z-b.z); }
enum class Issue { None, WrongType, MissingName, MissingWorld, InvalidSettings, TooFewPoints, InvalidPoint, DuplicatePoints };
inline Issue Validate(const Route& route) {
    if (route.name.empty()) return Issue::MissingName;
    if (route.world.empty()) return Issue::MissingWorld;
    if (!std::isfinite(route.speed) || route.speed <= 0 || !std::isfinite(route.width) || route.width < 0 || route.lanes < 1 || route.lanes > 2) return Issue::InvalidSettings;
    if (route.ring)  // an edited original ring: its shape, not points
        return std::isfinite(route.shape.cx) && std::isfinite(route.shape.cy) && std::isfinite(route.shape.z) && std::isfinite(route.shape.radius) &&
               route.shape.radius >= 20 ? Issue::None : Issue::InvalidPoint;
    if (route.points.size() < (route.closed ? 3u : 2u)) return Issue::TooFewPoints;
    if(!route.rounding.empty()&&(route.rounding.size()!=route.points.size()||std::any_of(route.rounding.begin(),route.rounding.end(),[](double r){return !std::isfinite(r)||r<0||r>200;})))return Issue::InvalidSettings;
    for (const auto& p : route.points) if (!Finite(p)) return Issue::InvalidPoint;
    for (std::size_t i=1; i<route.points.size(); ++i) if (Distance(route.points[i-1],route.points[i]) < .01) return Issue::DuplicatePoints;
    if (route.closed && Distance(route.points.front(),route.points.back()) < .01) return Issue::DuplicatePoints;
    return Issue::None; // obstacle and landing-site safety require live evidence
}

// ---- The whole route at once (2026-10-30, Traffic Manager stage 2: the Route tool moves, turns and raises a route) ----
// The middle a route turns about: a ring's centre, else the average of its points.
inline Point Centre(const Route& r) {
    if (r.ring) return { r.shape.cx, r.shape.cy, r.shape.z };
    Point c{0,0,0};
    if (r.points.empty()) return c;
    for (const auto& p : r.points) { c.x+=p.x; c.y+=p.y; c.z+=p.z; }
    const double n=double(r.points.size());
    return { c.x/n, c.y/n, c.z/n };
}
// Every point (a ring: its centre) moved by the same amount; heights move by dz.
inline Route Translated(Route r, double dx, double dy, double dz) {
    if (r.ring) { r.shape.cx+=dx; r.shape.cy+=dy; r.shape.z+=dz; return r; }
    for (auto& p : r.points) { p.x+=dx; p.y+=dy; p.z+=dz; }
    return r;
}
// Turned about its centre by `radians` (counter-clockwise seen from above); heights kept. A ring looks the same turned.
inline Route Turned(Route r, double radians) {
    if (r.ring || r.points.empty() || !std::isfinite(radians)) return r;
    const Point c=Centre(r);
    const double cs=std::cos(radians), sn=std::sin(radians);
    for (auto& p : r.points) { const double x=p.x-c.x, y=p.y-c.y; p.x=c.x+x*cs-y*sn; p.y=c.y+x*sn+y*cs; }
    return r;
}
// Flipped across the line through its centre along (ax, ay): left and right swap, heights and point order kept.
inline Route Mirrored(Route r, double ax, double ay) {
    const double l=std::hypot(ax,ay);
    if (r.ring || r.points.empty() || !(l>1e-9)) return r;
    ax/=l; ay/=l;
    const Point c=Centre(r);
    for (auto& p : r.points) {
        const double x=p.x-c.x, y=p.y-c.y, along=x*ax+y*ay;
        p.x=c.x+2*along*ax-x; p.y=c.y+2*along*ay-y;
    }
    return r;
}
// The lowest and highest point (a ring: its height twice).
inline std::pair<double,double> HeightRange(const Route& r) {
    if (r.ring || r.points.empty()) return { r.shape.z, r.shape.z };
    double lo=r.points.front().z, hi=lo;
    for (const auto& p : r.points) { lo=std::min(lo,p.z); hi=std::max(hi,p.z); }
    return { lo, hi };
}

// ---- The Shape tool (2026-10-30, Traffic Manager V1) ----
enum class Shape { Circle, Ellipse, Rectangle, Helix, RingBuilding };
inline const char* ShapeName(Shape s) { switch (s) { case Shape::Ellipse: return "Ellipse"; case Shape::Rectangle: return "Rectangle"; case Shape::Helix: return "Helix"; case Shape::RingBuilding: return "Ring"; default: return "Circle"; } }
// The points of a shape around `c`, radius `radius` (a rectangle: half its long side), turned by `radians`. A helix rises 30 m a
// turn for two turns and stays open; the others are loops. `rounding` gets the rectangle's corners.
inline std::vector<Point> ShapePoints(Shape s, Point c, double radius, double radians, std::vector<double>* rounding = nullptr) {
    constexpr double kTau = 6.283185307179586;
    std::vector<Point> out;
    const double cs = std::cos(radians), sn = std::sin(radians);
    auto put = [&](double x, double y, double z) { out.push_back({ c.x + x * cs - y * sn, c.y + x * sn + y * cs, z }); };
    if (rounding) rounding->clear();
    if (s == Shape::Rectangle) {
        const double h = radius * 0.6;
        put(radius, h, c.z); put(-radius, h, c.z); put(-radius, -h, c.z); put(radius, -h, c.z);
        if (rounding) rounding->assign(4, std::clamp(h * 0.4, 5.0, 200.0));
        return out;
    }
    if (s == Shape::Helix) {
        for (int i = 0; i <= 24; ++i) { const double a = i * kTau / 12; put(radius * std::cos(a), radius * std::sin(a), c.z + 30.0 * i / 12); }
        return out;
    }
    const double ry = s == Shape::Ellipse ? radius * 0.6 : radius;
    for (int i = 0; i < 12; ++i) { const double a = i * kTau / 12; put(radius * std::cos(a), ry * std::sin(a), c.z); }
    return out;
}
// ---- Resizing (2026-10-30, the user: a route can be scaled; a shape keeps "their values for how they're built") ----
// The width of a route, ring or points, seen from above.
inline double Across(const Route& r) {
    if (r.ring) return 2 * r.shape.radius;
    const Point c = Centre(r);
    double far = 0;
    for (const auto& p : r.points) far = std::max(far, std::hypot(p.x - c.x, p.y - c.y));
    return 2 * far;
}
// Bigger or smaller about its centre, seen from above; heights and point order kept. Corner rounding goes with it.
inline Route Scaled(Route r, double factor) {
    if (!std::isfinite(factor) || factor <= 0) return r;
    if (r.ring) { r.shape.radius *= factor; return r; }
    const Point c = Centre(r);
    for (auto& p : r.points) { p.x = c.x + (p.x - c.x) * factor; p.y = c.y + (p.y - c.y) * factor; }
    for (auto& q : r.rounding) if (q > 0) q = std::clamp(q * factor, 5.0, 200.0);
    return r;
}
// A shape's values are read back from its points, so nothing new is saved: routes built before this work, and moving, turning or raising
// one changes nothing here. Points moved by hand no longer fit (known = false): it is then just a route, with Size only.
struct ShapeValues {
    bool known = false;
    Shape kind = Shape::Circle;
    Point centre{ 0, 0, 0 };
    double a = 0, b = 0;         // circle, helix: a is the radius. Ellipse: long and short radius. Rectangle: half its two sides.
    double ux = 1, uy = 0;       // the direction of a
    double turns = 0, climb = 0; // helix: climb is metres a turn, below 0 when it runs downhill
    double angle = 0, dir = 1;   // helix: where it starts, and which way round
};
inline ShapeValues ReadShape(const Route& r) {
    constexpr double kStep = 0.5235987755982988, kFit = 1.0, kFlat = 0.5;  // 30 degrees a point, as ShapePoints lays them
    ShapeValues v;
    if (r.ring) return v;
    const auto& p = r.points;
    const int n = int(p.size());
    if (!r.closed) {
        if (n < 7 || (n - 1) % 6 != 0) return v;
        const double cx = (p[0].x + p[6].x) / 2, cy = (p[0].y + p[6].y) / 2;
        const double radius = std::hypot(p[0].x - cx, p[0].y - cy);
        if (radius < 5) return v;
        const double angle = std::atan2(p[0].y - cy, p[0].x - cx);
        const double cross = (p[0].x - cx) * (p[1].y - cy) - (p[0].y - cy) * (p[1].x - cx);
        const double dir = cross < 0 ? -1.0 : 1.0, climb = (p[std::size_t(n - 1)].z - p[0].z) * 12.0 / (n - 1);
        for (int i = 0; i < n; ++i) {
            const double t = angle + dir * i * kStep;
            const auto& q = p[std::size_t(i)];
            if (std::hypot(q.x - (cx + radius * std::cos(t)), q.y - (cy + radius * std::sin(t))) > kFit || std::fabs(q.z - (p[0].z + climb * i / 12)) > kFlat) return v;
        }
        v.known = true; v.kind = Shape::Helix; v.centre = { cx, cy, p[0].z }; v.a = v.b = radius;
        v.turns = (n - 1) / 12.0; v.climb = climb; v.angle = angle; v.dir = dir;
        return v;
    }
    if (n != 4 && n != 12) return v;
    for (const auto& q : p) if (std::fabs(q.z - p[0].z) > kFlat) return v;
    const Point c = Centre(r);
    if (n == 4) {
        const double ex = p[0].x - p[1].x, ey = p[0].y - p[1].y, fx = p[1].x - p[2].x, fy = p[1].y - p[2].y;
        const double e = std::hypot(ex, ey), f = std::hypot(fx, fy);
        if (e < 5 || f < 5 || std::fabs(ex * fx + ey * fy) > 0.02 * e * f) return v;
        if (std::hypot(p[3].x - p[2].x - ex, p[3].y - p[2].y - ey) > kFit) return v;
        v.known = true; v.kind = Shape::Rectangle; v.centre = c; v.a = e / 2; v.b = f / 2; v.ux = ex / e; v.uy = ey / e;
        return v;
    }
    int longest = 0;
    double a = 0;
    for (int i = 0; i < n; ++i) { const double d = std::hypot(p[std::size_t(i)].x - c.x, p[std::size_t(i)].y - c.y); if (d > a + 1e-6) { a = d; longest = i; } }
    if (a < 5) return v;
    const double ux = (p[std::size_t(longest)].x - c.x) / a, uy = (p[std::size_t(longest)].y - c.y) / a;
    double b = 0;
    for (const auto& q : p) b = std::max(b, std::fabs(-(q.x - c.x) * uy + (q.y - c.y) * ux));
    if (b < 5) return v;
    for (const double dir : { 1.0, -1.0 }) {
        bool fits = true;
        for (int k = 0; k < n && fits; ++k) {
            const double t = dir * k * kStep;
            const auto& q = p[std::size_t((longest + k) % n)];
            const double wantX = a * std::cos(t), wantY = b * std::sin(t);
            fits = std::hypot(q.x - (c.x + wantX * ux - wantY * uy), q.y - (c.y + wantX * uy + wantY * ux)) <= kFit;
        }
        if (fits) {
            v.known = true; v.kind = a - b <= 0.01 * a ? Shape::Circle : Shape::Ellipse; v.centre = c; v.a = a; v.b = b; v.ux = ux; v.uy = uy;
            return v;
        }
    }
    return v;
}
// The closed shapes: the same points stretched along the shape's own two sides. A circle has one value, so give it the same a and b.
inline Route WithSides(Route r, const ShapeValues& v, double a, double b) {
    if (!v.known || v.kind == Shape::Helix || !(v.a > 0) || !(v.b > 0)) return r;
    const double fa = a / v.a, fb = b / v.b;
    for (auto& p : r.points) {
        const double x = p.x - v.centre.x, y = p.y - v.centre.y;
        const double along = (x * v.ux + y * v.uy) * fa, across = (-x * v.uy + y * v.ux) * fb;
        p.x = v.centre.x + along * v.ux - across * v.uy;
        p.y = v.centre.y + along * v.uy + across * v.ux;
    }
    for (auto& q : r.rounding) if (q > 0) q = std::clamp(q * std::min(fa, fb), 5.0, 200.0);
    return r;
}
// A helix laid again with these values, from the same start, going the same way round.
inline Route WithHelix(Route r, const ShapeValues& v, double radius, double turns, double climb) {
    constexpr double kStep = 0.5235987755982988;
    if (!v.known || v.kind != Shape::Helix) return r;
    const int count = int(std::lround(turns * 12)) + 1;
    r.points.clear();
    for (int i = 0; i < count; ++i) {
        const double t = v.angle + v.dir * i * kStep;
        r.points.push_back({ v.centre.x + radius * std::cos(t), v.centre.y + radius * std::sin(t), v.centre.z + climb * i / 12 });
    }
    r.rounding.clear();
    return r;
}
// ---- Several things at once (2026-10-31, the user: Ctrl+click, "universal"): one change to everything selected. ----
// Turned and sized about (cx, cy), then moved. Heights only move by dz.
struct GroupMove { double dx = 0, dy = 0, dz = 0, cx = 0, cy = 0, factor = 1, radians = 0; };
inline void Apply(const GroupMove& m, double& x, double& y, double& z) {
    const double rx = (x - m.cx) * m.factor, ry = (y - m.cy) * m.factor;
    const double cs = std::cos(m.radians), sn = std::sin(m.radians);
    x = m.cx + rx * cs - ry * sn + m.dx;
    y = m.cy + rx * sn + ry * cs + m.dy;
    z += m.dz;
}
// ---- Disconnect and merge (2026-10-31). Each says where every old point went, so junctions can follow. ----
struct CutResult { std::vector<Route> pieces; std::vector<std::pair<int, int>> map; };  // map: old point -> piece and point there; -1 piece: it went
// Broken at each of `cuts` (sorted). A point cut alone ends one piece and starts the next; points cut next to each other leave a gap between them. A loop opens at the first cut.
inline CutResult CutRoute(const Route& r, const std::vector<int>& cuts) {
    CutResult out;
    const int n = int(r.points.size());
    out.map.assign(std::size_t(n), { -1, -1 });
    if (r.ring || n < 2 || cuts.empty()) return out;
    std::vector<int> seq;
    for (int i = 0; i < n; ++i) seq.push_back(r.closed ? (cuts.front() + i) % n : i);
    if (r.closed) seq.push_back(cuts.front());
    const auto isCut = [&](int p) { return std::binary_search(cuts.begin(), cuts.end(), p); };
    const auto finish = [&](std::vector<int>& part) {
        if (part.size() >= 2) {
            Route piece = r;
            piece.closed = false; piece.points.clear(); piece.rounding.clear();
            const bool rounded = !r.rounding.empty();
            for (std::size_t k = 0; k < part.size(); ++k) {
                const int old = part[k];
                piece.points.push_back(r.points[std::size_t(old)]);
                if (rounded) piece.rounding.push_back(k == 0 || k + 1 == part.size() || std::size_t(old) >= r.rounding.size() ? 0 : r.rounding[std::size_t(old)]);
                auto& m = out.map[std::size_t(old)];
                if (m.first < 0) m = { int(out.pieces.size()), int(k) };
            }
            out.pieces.push_back(std::move(piece));
        }
        part.clear();
    };
    std::vector<int> part;
    if (!(isCut(seq[0]) && isCut(seq[1]))) part.push_back(seq[0]);
    for (std::size_t j = 1; j < seq.size(); ++j) {
        part.push_back(seq[j]);
        if (!isCut(seq[j])) continue;
        finish(part);
        if (j + 1 < seq.size() && !isCut(seq[j + 1])) part.push_back(seq[j]);
    }
    finish(part);
    return out;
}
struct MergeResult { Route route; std::vector<std::vector<int>> map; };  // map: for each route given, old point -> point in the result
// One open route from several, keeping the first one's settings: each time the nearest free end (of a loop: its nearest point, where it opens) goes onto the nearest end of the chain.
inline MergeResult MergeRoutes(const std::vector<Route>& in) {
    MergeResult out;
    out.map.resize(in.size());
    if (in.empty()) return out;
    out.route = in[0];
    using Item = std::pair<int, int>;  // route, point
    std::vector<Item> chain;
    const auto flat = [](const Point& a, const Point& b) { return std::hypot(a.x - b.x, a.y - b.y); };
    const auto at = [&](const Item& i) -> const Point& { return in[std::size_t(i.first)].points[std::size_t(i.second)]; };
    const auto run = [&](int route, int first, int step) {  // every point once, from `first`
        std::vector<Item> seq;
        const int n = int(in[std::size_t(route)].points.size());
        for (int i = 0; i < n; ++i) seq.push_back({ route, ((first + step * i) % n + n) % n });
        return seq;
    };
    std::vector<bool> used(in.size(), false);
    used[0] = true;
    chain = run(0, 0, 1);
    if (in[0].closed && in.size() > 1 && !chain.empty()) {  // the first loop opens where the nearest other route comes closest
        double best = 1e300; int open = 0;
        for (std::size_t r = 1; r < in.size(); ++r) for (const auto& q : in[r].points) for (int i = 0; i < int(in[0].points.size()); ++i) {
            const double d = flat(in[0].points[std::size_t(i)], q);
            if (d < best) { best = d; open = i; }
        }
        chain = run(0, open + 1, 1);
    }
    for (std::size_t round = 1; round < in.size(); ++round) {
        double best = 1e300; int who = -1, first = 0, step = 1; bool front = false;
        for (std::size_t r = 1; r < in.size(); ++r) {
            if (used[r] || in[r].points.empty()) continue;
            const int n = int(in[r].points.size());
            const auto consider = [&](int i, int s, bool head, const Point& onto) {
                const double d = flat(in[r].points[std::size_t(i)], onto);
                if (d < best) { best = d; who = int(r); first = i; step = s; front = head; }
            };
            const Point& head = at(chain.front()), & tail = at(chain.back());
            if (in[r].closed) for (int i = 0; i < n; ++i) { consider(i, 1, false, tail); consider(i, 1, true, head); }
            else {
                consider(0, 1, false, tail); consider(n - 1, -1, false, tail);    // begins at the tail
                consider(n - 1, 1, true, head); consider(0, -1, true, head);      // ends at the head
            }
        }
        if (who < 0) break;
        used[std::size_t(who)] = true;
        auto seq = run(who, front ? (first - step * (int(in[std::size_t(who)].points.size()) - 1)) : first, step);
        if (front) chain.insert(chain.begin(), seq.begin(), seq.end());
        else chain.insert(chain.end(), seq.begin(), seq.end());
    }
    for (std::size_t i = 0; i < in.size(); ++i) out.map[i].assign(in[i].points.size(), -1);
    out.route.points.clear(); out.route.rounding.clear(); out.route.closed = false;
    bool anyRound = false;
    for (std::size_t c = 0; c < chain.size(); ++c) {
        const auto& p = at(chain[c]);
        const auto& from = in[std::size_t(chain[c].first)];
        if (!out.route.points.empty() && Distance(out.route.points.back(), p) < 1.0) {  // two ends in the same place are one point
            out.map[std::size_t(chain[c].first)][std::size_t(chain[c].second)] = int(out.route.points.size()) - 1;
            continue;
        }
        out.map[std::size_t(chain[c].first)][std::size_t(chain[c].second)] = int(out.route.points.size());
        out.route.points.push_back(p);
        const bool seam = c == 0 || c + 1 == chain.size() || chain[c - 1].first != chain[c].first || chain[c + 1].first != chain[c].first;
        const double q = !seam && std::size_t(chain[c].second) < from.rounding.size() ? from.rounding[std::size_t(chain[c].second)] : 0;
        if (q > 0) anyRound = true;
        out.route.rounding.push_back(q);
    }
    if (!anyRound) out.route.rounding.clear();
    return out;
}
// `picks` sorted: those points gone, with their corner rounding.
inline Route WithoutPoints(Route r, const std::vector<int>& picks) {
    for (auto i = picks.rbegin(); i != picks.rend(); ++i) {
        if (*i < 0 || *i >= int(r.points.size())) continue;
        if (*i < int(r.rounding.size())) r.rounding.erase(r.rounding.begin() + *i);
        r.points.erase(r.points.begin() + *i);
    }
    return r;
}
// A parallel copy, `side` metres to the right of the direction of travel (each point along its two neighbours' average normal).
inline std::vector<Point> Parallel(const std::vector<Point>& p, bool closed, double side) {
    const int n = int(p.size());
    std::vector<Point> out = p;
    for (int i = 0; i < n; ++i) {
        const Point& a = i > 0 ? p[std::size_t(i - 1)] : closed ? p[std::size_t(n - 1)] : p[std::size_t(i)];
        const Point& b = i + 1 < n ? p[std::size_t(i + 1)] : closed ? p[0] : p[std::size_t(i)];
        const double dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy);
        if (l < 1e-9) continue;
        out[std::size_t(i)].x += side * dy / l; out[std::size_t(i)].y -= side * dx / l;
    }
    return out;
}

// The Alt key hub (2026-10-30, the user: WASD as a movement controller): a tap moves this far, in metres.
inline double HubTap(bool fine) { return fine ? 0.25 : 1.0; }
// Held past a quarter second: metres a second, rising from 8 to 40 over two seconds so a short hold stays exact. Ctrl: a slow 1.5.
inline double HubSpeed(double heldSeconds, bool fine) {
    if (heldSeconds < 0.25) return 0;
    if (fine) return 1.5;
    return 8.0 + 32.0 * std::min(1.0, (heldSeconds - 0.25) / 2.0);
}

// Snapshot-based local undo. The owner supplies a draft copy; no methods mutate
// the original route or write to disk. Pointer drags should call Move on release.
class Draft {
    Route value_;
    std::vector<Route> undo_, redo_;
    static constexpr std::size_t kHistory = 64;
    void Checkpoint() {
        if (undo_.size() == kHistory) undo_.erase(undo_.begin());
        undo_.push_back(value_); redo_.clear();
    }
public:
    explicit Draft(Route source): value_(std::move(source)) {}
    const Route& Value() const { return value_; }
    Issue Check() const { return Validate(value_); }
    bool Add(Point p) {
        if (!Finite(p)) return false;
        Checkpoint(); if(!value_.rounding.empty())value_.rounding.push_back(0);value_.points.push_back(p); return true;
    }
    bool Move(std::size_t i, Point p) {
        if (i>=value_.points.size() || !Finite(p)) return false;
        const auto old=value_.points[i];
        if (old.x==p.x && old.y==p.y && old.z==p.z) return true;
        Checkpoint(); value_.points[i]=p; return true;
    }
    bool SetZ(std::size_t i, double z) {
        if (i>=value_.points.size() || !std::isfinite(z)) return false;
        auto p=value_.points[i]; p.z=z; return Move(i,p);
    }
    bool Remove(std::size_t i) {
        if (i>=value_.points.size()) return false;
        Checkpoint();if(i<value_.rounding.size())value_.rounding.erase(value_.rounding.begin()+i);value_.points.erase(value_.points.begin()+i); return true;
    }
    bool Clear() {
        if (value_.points.empty()) return false;
        Checkpoint(); value_.rounding.clear();value_.points.clear(); return true;
    }
    bool Reverse() {
        if (value_.points.size()<2) return false;
        Checkpoint();std::reverse(value_.rounding.begin(),value_.rounding.end()); std::reverse(value_.points.begin(),value_.points.end()); return true;
    }
    bool Round(std::size_t i,double radius){
        if(i>=value_.points.size()||!std::isfinite(radius)||radius<0||radius>200||value_.ring)return false;
        if(!value_.closed&&(i==0||i+1==value_.points.size())&&radius>0)return false;
        Checkpoint();value_.rounding.resize(value_.points.size(),0);value_.rounding[i]=radius;return true;
    }
    void Close(bool closed) { if (value_.closed!=closed) { Checkpoint(); value_.closed=closed; } }
    // How many undo steps there are, and every step since `depth` made into one.
    std::size_t Depth() const { return undo_.size(); }
    void CollapseTo(std::size_t depth) { if (undo_.size() > depth + 1) undo_.erase(undo_.begin() + std::ptrdiff_t(depth + 1), undo_.end()); }
    // Any other change (speed, lanes, schedule, label): one undo step.
    void Edit(const Route& next) { Checkpoint(); value_=next; }
    // The next frame of a change already checkpointed by Edit (a held key): no new undo step.
    void Amend(const Route& next) { value_=next; }
    bool Undo() {
        if (undo_.empty()) return false;
        redo_.push_back(value_); value_=std::move(undo_.back()); undo_.pop_back(); return true;
    }
    bool Redo() {
        if (redo_.empty()) return false;
        undo_.push_back(value_); value_=std::move(redo_.back()); redo_.pop_back(); return true;
    }
};
}
