#pragma once
// World (notes/VEHICLE_DECISION_DESIGN_20261106.md section 2). Everything is a shape: ground, decks, walls, vehicles, people. The only
// question is whether a shape is a surface this car wants to touch. One test sweeps the car's box along a branch against every shape,
// leaving out the wanted surface; it replaces the separate "is this a wall?" checks (register G1-G3, G7, G12, G32).
// Missing data is not a shape: unknown space can add cost but can never block. World decides nothing: it answers.
// Pure; tests: tests/planworld_test.cpp.
#include "fieldbox.h"
#include "relocationrebase.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include <unordered_map>

namespace planworld {
using fieldbox::V3;
enum class Source : std::uint8_t { Ray, Box, Ground, Vehicle, Person, Contact, Survey, Volume, Radar };  // Radar (2026-11-13, appended): a body's real box from the vehicle's radar lookup (measured)  // Survey: a solid the clear-path sweep found; Volume: this car's own ray volume (live measurement)
inline const char* Name(Source s) { constexpr const char* n[]{ "ray", "box", "ground", "vehicle", "person", "contact", "survey", "volume", "radar" }; return n[int(s)]; }

constexpr int kTrackPoints = 8;
struct Shape {
    V3 centre, velocity;
    double halfWidth = .2, halfLength = .2, halfHeight = 0, heading = 0;  // a ray hit is a point in height: its z is the surface
    Source source = Source::Ray;
    double seenAt = 0, expires = 1e18;
    std::uint32_t owner = 0;
    double normalZ = 0; bool haveNormal = false;  // signed: + faces up, - faces down
    bool steerRound = true;                       // false: wait behind it (an occupied vehicle in traffic, D1)
    int support = 1;                              // fix B: ray hits that agree on this spot (World counts them; 1 = this hit alone)
    int layer = -1;                               // world truth A: the game's collision layer of a ray hit (-1 unknown)
    bool tireClimb = false;                       // World: measured loose-object height is within half a tire; Speed owns its limit
    bool mayStop = true;                          // world truth B1: false = an unexplained road or terrain hit: it eases the speed, never stops or steers
    bool hazardMark = false;                      // a route's hazard box (routehazard.h, hazardfield.h): the user's own mark; a field box's "solids allowed" never lets the car through it
    bool unmeasured = false;                      // one flow: space this car's volume did not measure. Never a solid and never clear road: Speed slows through it, Avoid does not steer for it
    // A published plan: where this vehicle intends to be and when. Used instead of guessing from its velocity.
    int trackCount = 0; double trackT[kTrackPoints] = {}; V3 track[kTrackPoints] = {};
    std::uint32_t floorProbeBody = 0;  // stable static radar point eligible for a floor proof
    // RF-1 (World): the road surface under this shape at the car's scale (NaN: none trusted) and its pattern spread (m).
    double road = std::numeric_limits<double>::quiet_NaN(), roadSpread = 0;
#ifndef FVO_RELEASE
    const char* sensor = nullptr;  // static diagnostic label, never used by planning policy
    std::uint32_t sensorBody = 0;
#endif
};
inline double Top(const Shape& s) { return s.centre.z + s.halfHeight; }
inline double Base(const Shape& s) { return s.centre.z - s.halfHeight; }
// Source priority (WORKING_MANDATE.md): a prediction shapes the line and may slow the car, but only contact and confirmed live rays stop it.
inline bool Predicted(const Shape& s) { return s.source == Source::Survey || s.source == Source::Box; }
// RD-1 / RD-2 (World): is point (x, y, z) inside a vehicle body box (centre, heading = rotation z: sin = x, cos = y, half sizes; base =
// centre z - halfHeight, up to height + 1.5 m above it, 0.5 m slack across and along)? A person there is that vehicle's rider.
inline bool InsideBody(double x, double y, double z, double cx, double cy, double baseZ, double heading, double halfWidth, double halfLength, double height) {
    const double dx = x - cx, dy = y - cy, c = std::cos(heading), s = std::sin(heading);
    const double lat = dx * c - dy * s, lon = dx * s + dy * c, up = z - baseZ;
    return std::fabs(lat) <= halfWidth + .5 && std::fabs(lon) <= halfLength + .5 && up >= -.5 && up <= height + 1.5;
}
// ST-1 (World, D1 / C3): a vehicle is moving only when its body moves now or its published trajectory actually advances along the road;
// a stopped or passable (steerRound) vehicle is a static shape so Avoid and C3 can steer round it.
inline bool Moving(const Shape& s) {
    if (std::hypot(s.velocity.x, s.velocity.y) > .3) return true;
    if (s.steerRound || s.trackCount <= 1) return false;
    return std::hypot(s.track[s.trackCount - 1].x - s.track[0].x, s.track[s.trackCount - 1].y - s.track[0].y) > .5;
}

constexpr double kGrowPerSecond = .25, kGrowMax = 2.0, kPersonMargin = 1.0;
// How unsure a prediction is t seconds ahead. A published plan is firm; a guess from velocity grows.
inline double Margin(const Shape& s, double t) {
    const double base = s.source == Source::Person ? kPersonMargin : 0;
    return s.trackCount > 1 ? base : base + std::min(kGrowMax, kGrowPerSecond * std::max(0.0, t));
}
inline V3 At(const Shape& s, double t) {
    if (s.trackCount > 1) {
        if (t <= s.trackT[0]) return s.track[0];
        for (int i = 0; i + 1 < s.trackCount; ++i) if (t <= s.trackT[i + 1]) {
            const double span = std::max(1e-6, s.trackT[i + 1] - s.trackT[i]), u = (t - s.trackT[i]) / span;
            return fieldbox::Add(s.track[i], fieldbox::Mul(fieldbox::Sub(s.track[i + 1], s.track[i]), u));
        }
        return s.track[s.trackCount - 1];
    }
    return fieldbox::Add(s.centre, fieldbox::Mul(s.velocity, t));
}

// The surface this car wants at a place: on a route the field says; off a route it is what the wheels are on (D10).
// measured: z comes from a real floor measurement, not the authored line (which sits up to 1.4 m off the deck).
struct Wanted { bool have = false; double z = 0, step = 1.1, faceStep = .35; bool measured = true; };

enum class Verdict : std::uint8_t { Free, Low, Tall };
inline const char* Name(Verdict v) { constexpr const char* n[]{ "free", "low", "tall" }; return n[int(v)]; }
constexpr double kLowTop = .9;  // a face this high over the wanted surface can be hopped

// One shape against a car standing on `carBase` and `carHeight` tall.
inline Verdict JudgeAtHeight(const Shape& s, double centreZ, const Wanted& w, double carBase, double carHeight) {
    const double top = centreZ + s.halfHeight, base = centreZ - s.halfHeight;
    if (base >= carBase + carHeight) return Verdict::Free;           // over the car's roof: not touched
    if (top <= carBase - .3) return Verdict::Free;                   // under the car
    // A floor or ramp, facing up. With no measured floor here the authored height says nothing about it: an up-facing surface is the road.
    if (s.haveNormal && s.normalZ > .7 && (!w.have || !w.measured || top <= w.z + w.step)) return Verdict::Free;
    if (w.have) {
        const bool face = s.haveNormal && std::fabs(s.normalZ) < .5;
        const double rise = top - w.z;
        if (rise <= (face ? w.faceStep : w.step)) return Verdict::Free;  // the wanted surface itself, or within a step of it
        if (face && rise < kLowTop) return Verdict::Low;
    }
    return Verdict::Tall;
}

inline Verdict Judge(const Shape& s, const Wanted& w, double carBase, double carHeight) {
    return JudgeAtHeight(s, s.centre.z, w, carBase, carHeight);
}

struct HeadingAxes {
    double sine, cosine;
    static HeadingAxes Of(double heading) { return { std::sin(heading), std::cos(heading) }; }
};
// Two boxes on the ground plane overlap (separating axes). Half sizes grown by `margin`.
inline bool Overlap2D(double ax, double ay, HeadingAxes a, double ahw, double ahl, double bx, double by,
                      HeadingAxes b, double bhw, double bhl, double margin) {
    const double dx = bx - ax, dy = by - ay;
    const double as = a.sine, ac = a.cosine, bs = b.sine, bc = b.cosine;
    const double axes[4][2] = { { as, ac }, { ac, -as }, { bs, bc }, { bc, -bs } };
    for (const auto& n : axes) {
        const double dist = std::fabs(dx * n[0] + dy * n[1]);
        const double ra = ahl * std::fabs(as * n[0] + ac * n[1]) + ahw * std::fabs(ac * n[0] - as * n[1]);
        const double rb = bhl * std::fabs(bs * n[0] + bc * n[1]) + bhw * std::fabs(bc * n[0] - bs * n[1]);
        if (dist > ra + rb + margin) return false;
    }
    return true;
}

inline bool Overlap2D(double ax, double ay, double ah, double ahw, double ahl, double bx, double by, double bh,
                      double bhw, double bhl, double margin) {
    return Overlap2D(ax, ay, HeadingAxes::Of(ah), ahw, ahl, bx, by, HeadingAxes::Of(bh), bhw, bhl, margin);
}

// The car's pose at one point of a branch.
struct Pose { double x = 0, y = 0, base = 0, heading = 0, station = 0; Wanted wanted; };
struct Car { double halfWidth = 1.2, halfLength = 2.3, height = 2.0, gap = .3; };

struct Projection { V3 at; double margin; };
inline Projection Project(const Shape& s, double time) { return { At(s, time), Margin(s, time) }; }

inline Verdict Hit(const Pose& p, HeadingAxes carAxes, const Car& c, const Shape& s, HeadingAxes shapeAxes, const Projection& q) {
    if (!Overlap2D(p.x, p.y, carAxes, c.halfWidth, c.halfLength, q.at.x, q.at.y, shapeAxes,
                   s.halfWidth, s.halfLength, c.gap + q.margin)) return Verdict::Free;
    return JudgeAtHeight(s, q.at.z, p.wanted, p.base, c.height);
}
// Does the car at `p` hit shape `s` at time t?
inline Verdict Hit(const Pose& p, const Car& c, const Shape& s, double t) {
    return Hit(p, HeadingAxes::Of(p.heading), c, s, HeadingAxes::Of(s.heading), Project(s, t));
}

// Trust (G4): a source contradicted by where the car physically stands loses for this sample. A static shape the car's own body is
// inside, while nothing is touching it, is a poor fit, not a wall.
inline bool Contradicted(const Shape& s, const Pose& here, const Car& c, bool touching) {
    if (touching || s.source == Source::Vehicle || s.source == Source::Person || s.source == Source::Contact) return false;
    return Overlap2D(here.x, here.y, here.heading, c.halfWidth * .5, c.halfLength * .5, s.centre.x, s.centre.y, s.heading, s.halfWidth, s.halfLength, 0) &&
        Judge(s, here.wanted, here.base, c.height) == Verdict::Tall;
}

// The per-frame store (design section 2, work order step 1): moving shapes (vehicles, people) made once a frame for every reader (the
// plan, combat, context), instead of each car copying the vehicle snapshot. Static data stays in the nav map (hits, whole boxes, levels).
// `frame` says which frame it was made for; a reader on a later frame sees it is stale and asks for a rebuild.
struct Store {
    std::uint64_t frame = ~0ull; double at = -1;
    void Begin(std::uint64_t f, double now) {
        frame = f; at = now; shapes.clear(); links.clear();
        // Keep bucket storage around across neighbouring frames. Bound keys retained over long journeys.
        if (++generation == 0 || cells.size() > kRetainedCells) cells.clear();
    }
    void Add(const Shape& s) {
        if (!std::isfinite(s.centre.x) || !std::isfinite(s.centre.y)) return;
        auto& bucket = cells[Key(Cell(s.centre.x), Cell(s.centre.y))];
        if (bucket.generation != generation) bucket = { generation, -1 };
        links.push_back(bucket.head);
        bucket.head = int(shapes.size());
        shapes.push_back(s);
    }
    // Preserve publication order, including the cap when several nearby cells have valid shapes.
    int Near(double x, double y, double radius, std::uint32_t self, Shape* out, int max) const {
        if (max <= 0) return 0;
        static thread_local std::vector<std::size_t> hits;
        hits.clear();
        Visit(x, y, radius, [&](std::size_t i) {
            const auto& s = shapes[i];
            if ((!self || s.owner != self) && std::hypot(s.centre.x - x, s.centre.y - y) <= radius)
                hits.push_back(i);
            return false;
        });
        const auto n = std::min(hits.size(), std::size_t(max));
        if (n < hits.size()) std::nth_element(hits.begin(), hits.begin() + n, hits.end());
        std::sort(hits.begin(), hits.begin() + n);
        for (std::size_t i = 0; i < n; ++i) out[i] = shapes[hits[i]];
        return int(n);
    }
    template<class Predicate>
    bool AnyNearby(double x, double y, double radius, Predicate&& accept) const {
        return Visit(x, y, radius, [&](std::size_t i) {
            const auto& s = shapes[i];
            return std::hypot(s.centre.x - x, s.centre.y - y) <= radius && accept(s);
        });
    }

private:
    static constexpr double kCell = 32;
    static constexpr std::size_t kRetainedCells = 4096;
    static std::int32_t Cell(double x) {
        return std::int32_t(std::clamp(std::floor(x / kCell), -2147483647.0, 2147483646.0));
    }
    static std::uint64_t Key(std::int32_t x, std::int32_t y) {
        return (std::uint64_t(std::uint32_t(x)) << 32) | std::uint32_t(y);
    }
    template<class Predicate>
    bool Visit(double x, double y, double radius, Predicate&& accept) const {
        if (!std::isfinite(x) || !std::isfinite(y) || !(radius >= 0)) return false;
        if (radius <= 256 && shapes.size() > 16) {
            const auto x0 = Cell(x - radius), x1 = Cell(x + radius), y0 = Cell(y - radius), y1 = Cell(y + radius);
            for (auto ix = x0; ix <= x1; ++ix) for (auto iy = y0; iy <= y1; ++iy) {
                const auto it = cells.find(Key(ix, iy));
                if (it == cells.end() || it->second.generation != generation) continue;
                for (int i = it->second.head; i >= 0; i = links[i]) if (accept(std::size_t(i))) return true;
            }
        } else {
            for (std::size_t i = 0; i < shapes.size(); ++i) if (accept(i)) return true;
        }
        return false;
    }
    std::vector<Shape> shapes;
    std::vector<int> links;
    struct Bucket { std::uint64_t generation = 0; int head = -1; };
    std::uint64_t generation = 0;
    std::unordered_map<std::uint64_t, Bucket> cells;
};
// D1's clock for a vehicle: how long it has been still. 0 while moving.
struct StillClock {
    double since = 0;
    double Step(double now, double speed) { if (speed >= .6) since = 0; else if (since <= 0) since = now; return since > 0 ? now - since : 0; }
    void Rebase() { since = 0; }   // a relocation receipt: the old standing-still time says nothing about the new place
};
// PS-1 (2026-11-12): how far a vehicle really got over a window, sampled once a second. Rocking in place is no progress.
struct ProgressClock {
    static constexpr int kN = 16;
    double t[kN] = {}, x[kN] = {}, y[kN] = {}; int n = 0, head = 0; double lastAt = -1;
    void Step(double now, double px, double py) {
        if (lastAt >= 0 && now < lastAt) n = 0;            // the clock went back (a load): start again
        else if (lastAt >= 0 && now - lastAt < 1.0) return;
        lastAt = now; t[head] = now; x[head] = px; y[head] = py; head = (head + 1) % kN; if (n < kN) ++n;
    }
    // Straight-line distance between now and the newest sample at least `window` s old; -1: not enough history yet.
    // A relocation receipt (applied, once per serial, this vehicle only): the history is the old place, so net progress restarts from the new station.
    // The jump is never counted as progress or as a stuck. Returns true when it re-based.
    relocrebase::Seen relocSeen;
    bool Rebase(double now, double px, double py, const relocrebase::Event& ev, std::uint32_t vehicle) {
        if (!relocrebase::Take(relocSeen, ev, vehicle)) return false;
        n = 0; head = 0; lastAt = -1; Step(now, px, py); return true;
    }
    double Net(double now, double window) const {
        if (n == 0) return -1;
        const int cur = (head - 1 + kN) % kN;
        for (int k = 1; k < n; ++k) { const int i = (head - 1 - k + 2 * kN) % kN; if (now - t[i] >= window) return std::hypot(x[cur] - x[i], y[cur] - y[i]); }
        return -1;
    }
};
// A person's velocity from frame to frame (people only give a position).
struct Track2 {
    double x = 0, y = 0, at = -1, vx = 0, vy = 0;
    void Step(double now, double nx, double ny) {
        if (at >= 0 && now - at > .05) { const double dt = now - at; vx = (nx - x) / dt; vy = (ny - y) / dt; x = nx; y = ny; at = now; }
        else if (at < 0) { x = nx; y = ny; at = now; }
    }
};
}
