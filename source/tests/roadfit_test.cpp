// roadfit.h: deck snap, lane fit, Drive corners and lane overlap warnings (2026-11-05).
#include "roadfit.h"
#include <cmath>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
trafficdata::Route Road(double floorZ, double width, int dividers)
{
    trafficdata::Route r; r.name = "test_road"; r.ring = false; r.closed = false; r.lanes = 2; r.corridor = width / 2;
    r.points = { { 0, 0, floorZ }, { 0, 60, floorZ }, { 0, 120, floorZ } };
    r.road.enabled = true; r.road.width = width;
    if (dividers) roadplan::SetDividerCount(r.road, dividers, r.points.size());
    return r;
}
}

int main()
{
    // A deck at z 0, railings 4.5 m either side of the centre line and a 0.8 m central railing; the authored floor sits 1.3 m up.
    auto deck = [](double, double, double, double* g) { *g = 0; return true; };
    auto railings = [](double x, double, double lo, double hi) { return lo < 1.2 && hi > .5 && (std::fabs(x) >= 4.5 || std::fabs(x) <= .4); };
    auto r = Road(1.3, 12, 1);
    const auto line = trafficroute::Build(r);
    if (line.error || line.road.size() < 3) std::printf("build: %s stations=%d count=%d\n", line.error ? line.error : "ok", int(line.road.size()), line.Count());
    check(!line.error && line.road.size() >= 3, "test road builds");
    const auto survey = roadfit::Measure(r, line, deck, railings);
    check(survey.deckKnown == int(survey.stations.size()) && !survey.stations.empty(), "the deck is measured at every station");

    const auto snap = roadfit::SnapPoints(r, survey);
    check(snap.moved == 3 && std::fabs(snap.z[0]) < 1e-6 && std::fabs(snap.largest - 1.3) < 1e-6, "snap: every point drops 1.3 m onto the deck");

    // Measured from the floor the railings stand in the car band (deck 0 -> band 0.3..2.5 over a floor raised to max(deck, z-1)).
    const auto fit = roadfit::FitWidth(survey);
    check(fit.ok && fit.width > 7.5 && fit.width < 8.6, "fit: a 12 m road between railings 9 m apart narrows to about 8 m");
    check(fit.median > .9 && fit.median < 1.8, "fit: the central railing sets a red divider about its own width plus margins");
    check(std::fabs(fit.centreShift) < .3, "fit: railings symmetric, no shift");

    const auto over = roadfit::Overlaps(survey, 0);
    check(!over.empty() && over.front().samples > 0, "overlap: with no divider the lanes overlap the central railing and the side railings");
    const auto clear = roadfit::Overlaps(survey, fit.median);
    bool any = false; for (const auto& o : clear) if (std::fabs(o.across) < 4.4) any = true;
    check(!any, "overlap: with the fitted divider nothing solid is left inside the lanes near the centre");

    {   // Edge no-go bands: railings 4.5 m out; a 12 m road (half 6) has them 1.5 m inside its edge, plus the margin.
        const auto e = roadfit::EdgeBands(survey, 12);
        check(e.seenLeft > 0 && e.seenRight > 0, "edges: railings measured on both sides");
        check(e.left > 1.5 && e.left < 2.2 && e.right > 1.5 && e.right < 2.2, "edges: inset of the outermost railing plus the margin (about 1.9 m)");
        const auto fitted = roadfit::EdgeBands(survey, fit.width);
        check(fitted.left == 0 && fitted.right == 0, "edges: the fitted width already clears the railings by the margin, so no band");
        check(fit.edgeLeft == 0 && fit.edgeRight == 0 && fit.edgeSeenLeft > 0, "edges: Fit carries the bands for its own width (none here)");
        const auto wide = roadfit::EdgeBands(survey, 30);
        check(wide.left == 0 && wide.right == 0, "edges: a railing more than 3 m inside the road edge is not near it");
    }
    {   // One side only: a railing on the left at 4.5 m, open ground on the right: right stays 0 = none.
        auto leftOnly = [](double x, double, double lo, double hi) { return lo < 1.2 && hi > .5 && x >= 4.5; };
        const auto s = roadfit::Measure(r, line, deck, leftOnly);
        const auto e = roadfit::EdgeBands(s, 12);
        check((e.left > 1.5) != (e.right > 1.5) && (e.left == 0 || e.right == 0) && (e.seenLeft == 0 || e.seenRight == 0), "edges: only the side with a measured railing gets a band");
    }
    {   // Open ground: nothing measured, no bands.
        auto open = [](double, double, double, double) { return false; };
        const auto e = roadfit::EdgeBands(roadfit::Measure(r, line, deck, open), 12);
        check(e.left == 0 && e.right == 0 && e.seenLeft == 0 && e.seenRight == 0, "edges: open ground leaves both bands at 0");
    }
    {   // Pinch on one side at the start of the road only (a short pinch is ignored by the percentile).
        auto pinch = [](double x, double y, double lo, double hi) { return lo < 1.2 && hi > .5 && (x >= (y < 10 ? 3.0 : 4.5) || x <= -4.5); };
        const auto s = roadfit::Measure(r, line, deck, pinch);
        const auto e = roadfit::EdgeBands(s, 12);
        check(std::max(e.left, e.right) > 1.5 && std::max(e.left, e.right) < 2.2, "edges: a short pinch does not change the band (percentile)");
    }
    {   // Open ground: nothing solid, nothing measured, width untouched.
        auto open = [](double, double, double, double) { return false; };
        const auto s = roadfit::Measure(r, line, deck, open);
        check(!roadfit::FitWidth(s).ok && roadfit::Overlaps(s, 0).empty(), "open ground: no fit and no warnings");
    }
    {   // A deck 5 m under the floor is another level: never snapped to.
        auto low = [](double, double, double, double* g) { *g = -5; return true; };
        const auto s = roadfit::Measure(r, line, low, railings);
        check(roadfit::SnapPoints(r, s).moved == 0, "a surface beyond 3 m is another level, not this road's deck");
    }
    {   // Corners: a 90 degree sharp corner warns; rounded to 20 m it does not.
        trafficdata::Route c; c.ring = false; c.closed = false; c.points = { { 0, 0, 0 }, { 0, 50, 0 }, { 50, 50, 0 } };
        auto corners = roadfit::Corners(c);
        check(corners.size() == 1 && corners[0].point == 1 && std::fabs(corners[0].turnDeg - 90) < 1e-6, "a sharp 90 degree corner is reported");
        c.rounding = { 0, 20, 0 };
        check(roadfit::Corners(c).empty(), "rounded to 20 m: fine for Drive");
        c.points[2] = { 5, 100, 0 };
        c.rounding.clear();
        check(roadfit::Corners(c).empty(), "a gentle bend (under 25 degrees) is not a corner");
    }
    std::printf("roadfit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
