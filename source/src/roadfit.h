#pragma once
// Road fitting tools for the F8 / map route editor (2026-11-05, plan step 4). They fit the route data to the real road instead of
// asking the drivers to make up for it:
//   Survey      scans across every road station for solids (railings, walls, supports) and the real deck under the floor;
//   SnapPoints  the per-point deck heights ("Snap road to deck");
//   FitWidth    road width, red divider width and edge no-go bands that clear the railings ("Fit lanes to the road");
//   Corners     Drive corners tighter than a car can follow;
//   Overlaps    places where a lane overlaps something solid (red warnings).
// Pure: the ground and solid answers are passed in (the editor uses the nav map it already has; no new engine calls).
// Tests: tests/roadfit_test.cpp.
#include "trafficroute.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace roadfit {
constexpr double kStep = .25;          // m between samples across the road
constexpr double kBandLo = .3, kBandHi = 2.5;  // m over the floor where a solid blocks a car (kerbs under, signs over are ignored)
constexpr double kDeckReach = 3.0;     // a deck further than this from the floor is another level, never snapped to
constexpr double kMargin = .3;         // m kept clear of a railing each side
constexpr double kMinRadius = 12.0;    // m: tighter Drive corners get a warning
constexpr double kTurnFrom = 25.0;     // deg: smaller direction changes are not corners

struct Station {
    double x = 0, y = 0, z = 0, ux = 0, uy = 0, half = 0;  // centre, unit vector toward the left edge, road half-width
    int point = 0;                                            // nearest authored point
    bool deckKnown = false; double deck = 0;                  // the real surface under the floor
    std::vector<double> solids;                               // across offsets (m, + = left) of solid samples in the car band
    double scanned = 0;                                       // half-width that was scanned
};
struct Survey { std::vector<Station> stations; int deckKnown = 0, solidSamples = 0; };

// ground(x, y, z, &g): the surface under z (false: not measured). solid(x, y, zLo, zHi): something solid in that band there.
template<class Ground, class Solid>
Survey Measure(const trafficdata::Route& r, const trafficroute::Line& line, Ground&& ground, Solid&& solid, double extra = 6.0, int stride = 1) {
    Survey out;
    const int n = int(line.road.size());
    for (int i = 0; i < n; i += std::max(1, stride)) {
        const auto& s = line.road[std::size_t(i)];
        Station st;
        st.x = (s.left.x + s.right.x) * .5; st.y = (s.left.y + s.right.y) * .5; st.z = (s.left.z + s.right.z) * .5;
        const double dx = s.left.x - s.right.x, dy = s.left.y - s.right.y, w = std::hypot(dx, dy);
        if (!(w > 1e-6)) continue;
        st.ux = dx / w; st.uy = dy / w; st.half = w * .5;
        st.point = std::clamp(int(std::lround(line.ToSource(i))), 0, std::max(0, int(r.points.size()) - 1));
        double g = 0;
        if (ground(st.x, st.y, st.z + 2.0, &g) && std::isfinite(g) && std::fabs(g - st.z) <= kDeckReach) { st.deckKnown = true; st.deck = g; ++out.deckKnown; }
        const double floorZ = st.deckKnown ? std::max(st.deck, st.z - 1.0) : st.z;
        st.scanned = st.half + extra;
        for (double a = -st.scanned; a <= st.scanned + 1e-9; a += kStep)
            if (solid(st.x + st.ux * a, st.y + st.uy * a, floorZ + kBandLo, floorZ + kBandHi)) { st.solids.push_back(a); ++out.solidSamples; }
        out.stations.push_back(std::move(st));
    }
    return out;
}

// Snap road to deck: per authored point, the median deck height of its stations. Points with no measured deck keep theirs.
struct Snap { std::vector<double> z; std::vector<bool> changed; int moved = 0, unmeasured = 0; double largest = 0; };
inline Snap SnapPoints(const trafficdata::Route& r, const Survey& s, double minChange = .1) {
    Snap out; out.z.resize(r.points.size()); out.changed.assign(r.points.size(), false);
    for (std::size_t i = 0; i < r.points.size(); ++i) {
        out.z[i] = r.points[i].z;
        std::vector<double> d;
        for (const auto& st : s.stations) if (st.point == int(i) && st.deckKnown) d.push_back(st.deck - st.z);
        if (d.empty()) { ++out.unmeasured; continue; }
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        const double delta = d[d.size() / 2];
        if (std::fabs(delta) < minChange) continue;
        out.z[i] = r.points[i].z + delta; out.changed[i] = true; ++out.moved; out.largest = std::max(out.largest, std::fabs(delta));
    }
    return out;
}

// Fit lanes to the road: at each station the free span either side of the centre (a solid run within 1.5 m of the centre is a
// central railing: its far edges, not the centre, start the span). The road width is the narrow end of the stations (20th
// percentile, so one stray hit does not shrink a whole road); a central railing seen at 40 % of the stations sets the divider width.
struct Fit { bool ok = false; double width = 0, median = 0; int measured = 0, railingAt = 0, stations = 0; double centreShift = 0;
             double edgeLeft = 0, edgeRight = 0; int edgeSeenLeft = 0, edgeSeenRight = 0; };  // edge*: suggested no-go bands (m), 0 = none
// What one station sees across the road: the central railing run (lo..hi, within 1.5 m of the centre), and the free span to the
// nearest solid on each side beyond it (left/right, from the run's far edge; seenL/seenR false = open ground that side).
struct Span { bool centre = false, seenL = false, seenR = false; double lo = 0, hi = 0, inL = 0, inR = 0, left = 0, right = 0; };
inline Span StationSpan(const Station& st) {
    Span o; double lo = 0, hi = 0;
    for (double a : st.solids) if (std::fabs(a) <= 1.5) { if (!o.centre) { lo = hi = a; o.centre = true; } lo = std::min(lo, a); hi = std::max(hi, a); }
    if (o.centre) {  // grow the run while samples stay contiguous
        bool grew = true;
        while (grew) { grew = false; for (double a : st.solids) { if (a < lo && lo - a <= kStep * 1.5) { lo = a; grew = true; } if (a > hi && a - hi <= kStep * 1.5) { hi = a; grew = true; } } }
    }
    o.lo = lo; o.hi = hi; o.inL = o.centre ? hi : 0; o.inR = o.centre ? lo : 0;
    o.left = st.scanned; o.right = st.scanned;
    for (double a : st.solids) {
        if (a > o.inL + kStep * .5 && a - o.inL < o.left) { o.left = a - o.inL; o.seenL = true; }
        if (a < o.inR - kStep * .5 && o.inR - a < o.right) { o.right = o.inR - a; o.seenR = true; }
    }
    return o;
}
// Edge no-go bands (2026-11-19): for a road of this width, how far the outermost railing on each side stands inside the road edge
// plus the same margin the width keeps (so a railing the road already clears by the margin needs no band). Per side the 20th
// percentile of the stations that measured a railing on that side (one stray hit does not band a whole road). A side with no
// railing measured within kEdgeOut/kEdgeIn of its edge stays 0 = none. Positive left = the markers' left.
constexpr double kEdgeOut = 1.0, kEdgeIn = 3.0;  // m: a railing further outside / inside the road edge than this is not "near" the edge
struct Edges { double left = 0, right = 0; int seenLeft = 0, seenRight = 0; };
inline Edges EdgeBands(const Survey& s, double width) {
    Edges out; const double half = width * .5;
    std::vector<double> l, r;  // railing distance from the centre line (inner face)
    for (const auto& st : s.stations) {
        const Span sp = StationSpan(st);
        if (sp.seenL) l.push_back(sp.inL + sp.left - kStep * .5);
        if (sp.seenR) r.push_back(sp.right - sp.inR - kStep * .5);
    }
    out.seenLeft = int(l.size()); out.seenRight = int(r.size());
    auto band = [&](std::vector<double>& d) {
        if (d.empty()) return 0.0;
        std::sort(d.begin(), d.end());
        const double rail = d[std::size_t(double(d.size() - 1) * .2)];
        if (rail > half + kEdgeOut || rail < half - kEdgeIn) return 0.0;  // not measured near this edge
        const double b = half + kMargin - rail;
        return b > .05 ? std::clamp(b, 0.0, 50.0) : 0.0;         // already clear by the margin: none
    };
    out.left = band(l); out.right = band(r);
    return out;
}
inline Fit FitWidth(const Survey& s) {
    Fit out; out.stations = int(s.stations.size());
    std::vector<double> widths, railings, shifts;
    for (const auto& st : s.stations) {
        const Span sp = StationSpan(st);
        const bool centre = sp.centre, seenL = sp.seenL, seenR = sp.seenR;
        const double inL = sp.inL, inR = sp.inR, left = sp.left, right = sp.right;
        if (!seenL && !seenR && !centre) continue;  // open ground both sides: says nothing about width
        ++out.measured;
        const double railing = centre ? sp.hi - sp.lo + kStep : 0;
        // Symmetric about the centre line (the lanes stay where they were authored): the narrower side decides.
        // Edge positions from the centre line (a central railing sits inside the road, under the red divider).
        const double side = std::min(seenL ? inL + left : 1e9, seenR ? right - inR : 1e9) - kStep * .5 - kMargin;
        if (seenL || seenR) widths.push_back(2 * std::max(0.0, side));
        if (centre) { railings.push_back(railing); ++out.railingAt; }
        if (seenL && seenR) shifts.push_back((left - right) * .5);
    }
    if (widths.empty()) return out;
    std::sort(widths.begin(), widths.end());
    out.width = std::clamp(widths[std::size_t(double(widths.size() - 1) * .2)], 1.0, 400.0);
    if (!railings.empty() && out.railingAt * 10 >= out.measured * 4) {
        std::sort(railings.begin(), railings.end());
        out.median = std::clamp(railings[std::size_t(double(railings.size() - 1) * .8)] + 2 * kMargin, 0.0, 50.0);
    }
    if (!shifts.empty()) { std::sort(shifts.begin(), shifts.end()); out.centreShift = shifts[shifts.size() / 2]; }
    out.ok = out.width >= 1.0;
    if (out.ok) { const Edges e = EdgeBands(s, out.width); out.edgeLeft = e.left; out.edgeRight = e.right; out.edgeSeenLeft = e.seenLeft; out.edgeSeenRight = e.seenRight; }
    return out;
}

// Drive corners: the turn at each authored point and the radius the route gives it (the rounding radius; a sharp corner has 0).
struct Corner { int point = -1; double turnDeg = 0, radius = 0; };
inline std::vector<Corner> Corners(const trafficdata::Route& r) {
    std::vector<Corner> out; const auto& p = r.points; const int n = int(p.size());
    for (int i = 0; i < n && n > 2; ++i) {
        if (!r.closed && (i == 0 || i + 1 == n)) continue;
        const auto& a = p[std::size_t((i + n - 1) % n)]; const auto& c = p[std::size_t((i + 1) % n)];
        const double ux = p[std::size_t(i)].x - a.x, uy = p[std::size_t(i)].y - a.y, vx = c.x - p[std::size_t(i)].x, vy = c.y - p[std::size_t(i)].y;
        const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
        if (lu < 1e-6 || lv < 1e-6) continue;
        const double turn = std::acos(std::clamp((ux * vx + uy * vy) / (lu * lv), -1.0, 1.0)) * 180 / 3.141592653589793;
        const double radius = std::size_t(i) < r.rounding.size() ? r.rounding[std::size_t(i)] : 0;
        if (turn >= kTurnFrom && radius < kMinRadius) out.push_back({ i, turn, radius });
    }
    return out;
}

// Lanes overlapping a solid: stations where a solid sample lies inside the road and outside the red divider band.
struct Overlap { int point = -1; double x = 0, y = 0, z = 0, across = 0; int samples = 0; };
inline std::vector<Overlap> Overlaps(const Survey& s, double median) {
    std::vector<Overlap> out;
    for (const auto& st : s.stations) {
        Overlap o; o.point = st.point;
        for (double a : st.solids) {
            if (std::fabs(a) > st.half - kStep * .5) continue;      // beside the road
            if (median > 0 && std::fabs(a) < median * .5) continue; // inside the red divider: expected
            if (!o.samples || std::fabs(a) < std::fabs(o.across)) o.across = a;
            ++o.samples;
        }
        if (!o.samples) continue;
        o.x = st.x + st.ux * o.across; o.y = st.y + st.uy * o.across; o.z = st.z;
        if (!out.empty() && out.back().point == o.point) { out.back().samples += o.samples; continue; }  // one per point
        out.push_back(o);
    }
    return out;
}
}
