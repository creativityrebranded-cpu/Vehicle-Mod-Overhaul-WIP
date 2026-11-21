#pragma once
// World, one flow (notes/one-flow-plan-20261110/PLAN.md cards V2-V4; design section 2: one collision test for everything).
// This car's own ray volume (fieldslice::Slice) becomes ordinary World shapes, so the planner's one collision test, Avoid and Speed read
// it exactly as they read every other shape. Nothing here decides: it only says what the rays measured.
//
// The cell vocabulary, as shapes:
//   measured      hits that agree (beams, or the same object again)    -> Source::Volume at the hits' own measured heights, a live
//                                                                         measurement: may stop, Avoid steers round
//   doubt         one hit nothing agrees with                          -> unmeasured flag: slow near it and look, never a stop
//   no information legal space nothing measured                        -> cut out: no shape (user, 2026-11-11); never clear road, never a solid
//   clear         measured free                                        -> no shape
// Space outside the authored field gives no shape at all: leaving the field is the field's own containment rule.
// E (2026-11-19): the volume no longer says "low edge" itself. Its own straight-grade floor and pass height made that verdict final before
// World's road fit (RF-1, given to every volume shape by the coordinator's AssignRoad) and Plan's road-referenced judgement saw the hit; a
// ramp rising ahead read as a wall, a lip under the volume's 0.5 m pass height as climbable while the plan climbs 0.35. Now every measured
// hit is a shape at the height it was hit, and Plan's contact rule (the one car record's climb, carcap.h) says what is climbed and what is met.
// Hits of one column at one face (depths within rules::face) are one shape: a wall's beams stack into one; a slope's beams, each at its own
// depth, stay apart at their own heights (one merged shape would stand the far beams' heights over the near road: a ramp read as a wall).
// Pure; tests: tests/fieldvolume_test.cpp.
#include "fieldslice.h"
#include "planworld.h"
#include <algorithm>
#include <cmath>

namespace fieldvolume {
namespace rules {
inline constexpr double face = .25;          // half length of a doubt cell (the hit is its near face); hits of one column this close in depth are one face
inline constexpr double point = .1;          // VT-1: half size of a solid: the measured hit, not the column's 0.5 m width
inline constexpr double pad = .15;           // a measured face reaches this far below its lowest hit (it stands on something); its top is the
                                             // highest hit itself (E: padded above, a 0.30 m tyre-beam hit read 0.45, over the 0.35 climb)
inline constexpr double walk = 2.0;          // not-measured space is cut to the field in steps this long
inline constexpr double beyond = 6.0;        // not measured past the far end of the volume, this deep
inline constexpr double curtain = .25;       // half width of the not-measured strip beside the volume
inline constexpr double minGap = .2;         // shorter not-measured remainders are not worth a shape
}
struct Counts { int solid = 0, doubt = 0, unmeasured = 0, dropped = 0, byObject = 0, remembered = 0; };

enum class Cell { None, Solid, Doubt };
// What one hit ray is, by the volume's own agreement rule: a hit is measured (Solid: a Volume shape) when another hit in the same or a
// neighbouring column agrees on the depth, or the same object was hit again close by (OA-1); alone it is doubt. Its height is its own:
// the recorded hit height, else the beam's height over the volume's floor. support: how many hits agree, itself included.
inline Cell Classify(const fieldslice::Slice& m, int i, int h, double& height, int& support, bool* byObject = nullptr, double now = -1) {
    using fieldslice::Answer;
    const auto& ray = m.rays[i][h];
    if (ray.answer != Answer::Hit) return Cell::None;
    height = std::isfinite(ray.z) ? ray.z : m.base + m.grade * ray.depth + m.heightAbove[h];
    support = 1;
    bool confirmed = false;
    const int reach = fieldslice::Neighbours(m.spacing);  // agreement by lateral distance, not by column index
    for (int j = std::max(0, i - reach); j <= std::min(fieldslice::columns - 1, i + reach); ++j)
        for (int level = 0; level < fieldslice::levels; ++level) {
            if (j == i && level == h) continue;
            const auto& other = m.rays[j][level];
            if (other.answer != Answer::Hit || std::abs(other.depth - ray.depth) > fieldslice::agreement) continue;
            confirmed = true; ++support;
        }
    // OA-1: the same object hit again close by (another beam, or this or another refresh: the remembered points) agrees with this hit.
    if (!confirmed && ray.owner != 0 && std::isfinite(ray.z) && std::isfinite(ray.hx) && std::isfinite(ray.hy)) {
        const int same = m.SameObject(ray.owner, ray.hx, ray.hy, ray.z, ray.at, now < 0 ? ray.at : now);
        if (same > 0) { confirmed = true; if (byObject) *byObject = true; support += same; }
    }
    return confirmed ? Cell::Solid : Cell::Doubt;
}

// floorAt(depth, side) -> the floor's height there (the wanted surface, measured where World has it).
// inside(x, y, z) -> is this point in the authored field? With no field known it answers true.
template <class FloorFn, class InsideFn>
// withUnmeasured false (F1, the volume as one more source beside the older ones): only what the rays hit becomes shapes; space they did
// not measure says nothing here, because the older sources still answer for it.
inline int Shapes(const fieldslice::Slice& m, double carHeight, FloorFn&& floorAt, InsideFn&& inside, planworld::Shape* out, int max, Counts* counts = nullptr, bool withUnmeasured = true, double now = -1) {
    using fieldslice::Answer; using planworld::Shape;
    Counts c; int n = 0;
    if (!m.active || m.spacing <= 0 || m.end <= m.start || max <= 0) { if (counts) *counts = c; return 0; }
    if (now < 0) { for (const auto& column : m.rays) for (const auto& r : column) if (r.answer != Answer::Unknown) now = std::max(now, r.at); }  // the newest measurement
    const double s = std::sin(m.yaw), co = std::cos(m.yaw);
    const auto place = [&](Shape& sh, double depth, double side) { sh.centre.x = m.x + s * depth + co * side; sh.centre.y = m.y + co * depth - s * side; sh.heading = m.yaw; };
    const auto push = [&](const Shape& sh) { if (n < max) { out[n++] = sh; return true; } ++c.dropped; return false; };
    const double tall = std::max(1.0, carHeight);  // tall enough that the one test never reads it as a hop
    // Not-measured space between two depths at one side offset, cut to the field: one shape per unbroken run inside it.
    const auto unmeasured = [&](double from, double to, double side, double halfWidth) {
        if (!withUnmeasured || to - from < rules::minGap) return;
        double runFrom = -1, d = from;
        const auto flush = [&](double runTo) {
            if (runFrom < 0) return;
            if (runTo - runFrom >= rules::minGap) {
                Shape sh; sh.source = planworld::Source::Volume; sh.unmeasured = true; sh.mayStop = false; sh.steerRound = true;
                const double mid = (runFrom + runTo) * .5; place(sh, mid, side);
                sh.halfWidth = halfWidth; sh.halfLength = (runTo - runFrom) * .5; sh.halfHeight = tall * .5;
                sh.centre.z = floorAt(mid, side) + tall * .5; sh.haveNormal = true; sh.normalZ = 0; sh.support = 0;
                if (push(sh)) ++c.unmeasured;
            }
            runFrom = -1;
        };
        while (d < to - 1e-9) {
            const double next = std::min(to, d + rules::walk), mid = (d + next) * .5;
            const double x = m.x + s * mid + co * side, y = m.y + co * mid - s * side;
            if (inside(x, y, floorAt(mid, side) + .5)) { if (runFrom < 0) runFrom = d; }
            else flush(d);
            d = next;
        }
        flush(to);
    };
    double bandLeft = 1e18, bandRight = -1e18;
    for (int i = 0; i < fieldslice::columns; ++i) {
        const double side = m.left + i * m.spacing, half = m.spacing * .5;
        // How far this column is measured: the nearest end of any of its rays. A level that never started in the field says nothing.
        double known = m.end; bool legal = false;
        struct Measured { double depth, z, seen; int support; };
        Measured hits[fieldslice::levels]; int nh = 0;
        double doubtAt = 1e18;
        for (int h = 0; h < fieldslice::levels; ++h) {
            const auto& ray = m.rays[i][h];
            if (ray.answer == Answer::Clear && ray.field && ray.depth <= m.start + 1e-6) continue;  // no legal space at this level
            legal = true;
            known = std::min(known, ray.answer == Answer::Unknown ? m.start : std::max(m.start, ray.depth));
            double height = 0; int support = 0; bool byObject = false;
            const Cell cell = Classify(m, i, h, height, support, &byObject, now);
            if (byObject) ++c.byObject;
            if (cell == Cell::Solid) hits[nh++] = { ray.depth, height, ray.at, support };
            else if (cell == Cell::Doubt) doubtAt = std::min(doubtAt, ray.depth);
        }
        if (!legal) continue;
        bandLeft = std::min(bandLeft, side - half); bandRight = std::max(bandRight, side + half);
        std::sort(hits, hits + nh, [](const Measured& p, const Measured& q) { return p.depth < q.depth; });
        const double solidAt = nh > 0 ? hits[0].depth : 1e18;
        for (int k = 0; k < nh;) {
            // One face: this hit and the column's next hits within rules::face of it in depth; its height is the measured span of those hits.
            // VT-1 (2026-11-19 13:01 run): a face is where its rays hit (the column's own side, the nearest hit depth as its near face), 0.2 m
            // across. Spread over the column's whole width it put a barrier face beside the car into the car's front corner (25 of 27
            // blockers 1.0-1.5 m right of the car on the lc_ring_road ramp, while the car was not touching). Neighbouring columns
            // still give a wall its run of faces.
            const double nearFace = hits[k].depth;
            double bottom = hits[k].z, top = hits[k].z, seen = hits[k].seen; int support = hits[k].support, e = k + 1;
            while (e < nh && hits[e].depth - nearFace <= rules::face) {
                bottom = std::min(bottom, hits[e].z); top = std::max(top, hits[e].z); seen = std::max(seen, hits[e].seen);
                support = std::max(support, hits[e].support); ++e; }
            Shape sh; sh.source = planworld::Source::Volume; place(sh, nearFace + rules::point, side);
            sh.halfWidth = std::min(half, rules::point); sh.halfLength = rules::point;
            bottom -= rules::pad;
            sh.centre.z = (bottom + top) * .5; sh.halfHeight = (top - bottom) * .5;
            sh.haveNormal = true; sh.normalZ = 0; sh.support = support; sh.seenAt = seen; sh.mayStop = true; sh.steerRound = true;
            if (push(sh)) ++c.solid;
            k = e;
        }
        if (doubtAt < solidAt - fieldslice::agreement) {  // one hit alone: doubt, slow and look
            Shape sh; sh.source = planworld::Source::Volume; sh.unmeasured = true; sh.mayStop = false; place(sh, doubtAt + rules::face, side);
            sh.halfWidth = half; sh.halfLength = rules::face; sh.halfHeight = tall * .5;
            sh.centre.z = floorAt(doubtAt, side) + tall * .5;
            sh.haveNormal = true; sh.normalZ = 0; sh.support = 1;
            if (push(sh)) ++c.doubt;
        }
        // Space nothing measured (past where a column's beams ended, beside or beyond the volume) is cut out of the volume (user, 2026-11-11):
        // no information, so no shape. It is never a solid and never clear road; how far ahead was measured is Speed's own limit (planrun).
        (void)known;
    }
    (void)bandLeft; (void)bandRight; (void)unmeasured;
    // OA-1: remembered points of an object hit more than once stay in the volume until a beam passes through them, also on refreshes where
    // no beam met that object (thin twigs, beams re-aimed by a turn). Points the live beams still hold are already shapes above.
    if (m.memory) for (const auto& p : m.memory->points) {
        if (!m.Live(p, now)) continue;
        const double depth = (p.x - m.x) * s + (p.y - m.y) * co;
        if (depth < m.start - .5 || depth > m.end + 1.0) continue;
        bool live = false;
        for (const auto& column : m.rays) for (const auto& r : column)
            if (r.answer == Answer::Hit && r.owner == p.owner && std::abs(r.at - p.at) < 1e-9 && std::abs(r.hx - p.x) < 1e-3 && std::abs(r.hy - p.y) < 1e-3) live = true;
        if (live) continue;
        if (m.SameObject(p.owner, p.x, p.y, p.z, p.at, now) == 0) continue;  // one point alone is not agreement
        double bottom = p.z, top = p.z;
        for (const auto& q : m.memory->points)
            if (q.owner == p.owner && m.Live(q, now) && std::hypot(q.x - p.x, q.y - p.y) <= fieldslice::sameObjectReach) { bottom = std::min(bottom, q.z); top = std::max(top, q.z); }
        Shape sh; sh.source = planworld::Source::Volume; sh.centre.x = p.x; sh.centre.y = p.y; sh.heading = m.yaw;
        sh.halfWidth = std::max(.25, m.spacing * .5); sh.halfLength = rules::face;
        bottom -= rules::pad;
        sh.centre.z = (bottom + top) * .5; sh.halfHeight = (top - bottom) * .5;
        sh.haveNormal = true; sh.normalZ = 0; sh.support = 2; sh.seenAt = p.at;
        sh.mayStop = true; sh.steerRound = true;  // measured: what is climbed is Plan's, against World's road (E)
        if (push(sh)) ++c.remembered;
    }
    if (counts) *counts = c;
    return n;
}
}
