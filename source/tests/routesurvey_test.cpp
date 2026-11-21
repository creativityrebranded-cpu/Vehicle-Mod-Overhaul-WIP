// Route survey store (routesurvey.h): measured floor levels per metre, nearest level to the field's, step edges.
#include "routesurvey.h"
#include <cstdio>
using namespace routesurvey;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

static std::vector<Row> Road(int n) { std::vector<Row> r(n); for (int i = 0; i < n; ++i) { r[i].y = float(i * kScanStep); r[i].ux = 1; } return r; }
static void Sweep() {
    {   Solids s; s.Note(10, 10, 20, 20.55, 1); s.Note(10.2, 10.1, 20, 21.2, 1);
        Check(s.count == 1, "sweep: the same solid at two heights is one solid");
        Check(s.Known(10.3, 10.2, 21.0), "sweep: a live hit on it is known");
        Check(!s.Known(13, 10, 21.0), "sweep: a hit elsewhere is news");
        Check(!s.Known(10, 10, 30.0), "sweep: a deck above it is not that solid");
        s.Note(40, 40, 20, 20.55, 2); s.Prune(10, 10, 5, 2); Check(s.count == 1 && !s.Known(10, 10, 21), "sweep: a solid the next sweep did not see again is gone");
        Check(s.Known(40, 40, 20.5), "sweep: outside that sweep it stays"); }
    {   Solids s; s.Note(10, 10, 20, 20.6, 1); s.Note(30, 30, 20, 20.6, 1); Check(s.Forget(10.2, 10.2) == 1 && !s.Known(10, 10, 20.5) && s.count == 1, "rays through it clear a predicted solid"); Check(s.Known(30, 30, 20.5), "and only that one"); }
    Check(IsLip(true, 20.0, true, 20.45), "lip: a small step with a floor on top is road");
    Check(IsLip(true, 20.0, true, 19.5), "lip: a step down is road");
    Check(!IsLip(true, 20.0, true, 21.5), "lip: a rock's top is a solid");
    Check(!IsLip(true, 20.0, false, 0), "lip: nothing on top within reach is a wall");
    Check(!IsLip(false, 0, true, 20.2), "lip: no floor before it is not proven road");
    {   auto r = Road(10); Check(RowHasRoom(r, 5), "row: an open road has room"); r[5].blocked[kScanHalf] = true; Check(MaskOf(r[5]) == (1u << kScanHalf), "row: blocked lines as bits"); Check(RowHasRoom(r, 5), "row: one solid leaves room beside it");
        for (int b = 0; b < kScanBins; ++b) r[5].blocked[b] = true; Check(!RowHasRoom(r, 5) && !RowHasRoom(r, 4), "row: a wall right across has no room"); }
    {   ClearMap c; ClearCell a; a.x = 10; a.y = 10; a.z = 20; a.blocked = 1; c.Put(a); ClearCell d = a; d.z = 30; d.blocked = 2; c.Put(d);
        Check(c.At(10.5, 10.5, 20)->blocked == 1 && c.At(10.5, 10.5, 30)->blocked == 2, "rows: stacked decks keep their own row");
        Check(!c.At(30, 30, 20), "rows: not swept here"); }
}

static void CachedRadarFloor() {
    Store map;
    const double x = -133.883, y = 1756.508, hit = -65.171;
    auto road = [&](double px) { return -64.70 - .44 * (px - x); };
    for (int dx = -3; dx <= 3; ++dx) {
        for (int dy = -3; dy <= 3; ++dy) {
            const double px = -134 + dx + .45;
            map.Note(px, 1756 + dy + .45, road(px), .91, 1);
        }
    }
    Reach reach;
    map.Flood(x + 2, y, road(x + 2), 10, 100, reach);
    double z = 0;
    Check(map.BelowReachableFloor(x, y, hit, .25, 2, reach, z) && std::fabs(z - road(x)) < .01,
          "cached sweep point: the ramp's own height wins over the downhill sweep base");
    Check(!map.BelowReachableFloor(x, y, road(x) + .65, .25, 2, reach, z),
          "rail over connected floor remains an obstacle");
    Check(!map.BelowReachableFloor(x, y, road(x) - .1, .25, 2, reach, z),
          "a near-surface point is not discarded");
    Check(!map.BelowReachableFloor(x, y - 5, hit, .25, 2, reach, z),
          "an offset lane with no coverage does not provide local clearance");
    Check(!map.BelowReachableFloor(x, y, hit, .25, 2, Reach{}, z),
          "an unconnected upper deck is not evidence for clearance");

    map.Note(x, y, hit - 1.20, .99, 2);
    Check(!map.BelowReachableFloor(x, y, hit, .25, 2, reach, z),
          "a measured lower road preserves a low ceiling even when disconnected from the upper road");

    Store sparse;
    sparse.Note(x, y, road(x), .91, 1);
    sparse.Flood(x, y, road(x), 10, 100, reach);
    Check(!sparse.BelowReachableFloor(x, y, hit, .25, 2, reach, z),
          "a single sample cannot establish a sloped floor at the hit");
}


// RF-1 (2026-11-19): the road surface at the car's scale, with its pattern and outliers.
static void RoadFitTests() {
    auto grid = [](Store& s, auto height) { for (int i = -3; i <= 3; ++i) for (int j = -3; j <= 3; ++j) { const double x = i + .5, y = j + .5; s.Note(x, y, height(x, y, i, j), 1, 1); } };
    {   Store s; grid(s, [](double, double y, int, int) { return .42 * y; });
        const auto f = s.FitSurface(.5, .5, .21, 2, nullptr);
        Check(f.ok && std::fabs(f.z - .21) < .02 && f.spread < .03, "RF-1: a 23 deg ramp: the road under the hit, with no pattern"); }
    {   Store s; grid(s, [](double, double y, int, int j) { return .42 * y + ((j & 1) ? .1 : -.1); });
        const auto f = s.FitSurface(.5, .5, .21, 2, nullptr);
        Check(f.ok && std::fabs(f.z - .21) < .08 && f.spread > .06 && f.spread < .175, "RF-1: a sawtooth deck: the road plus a 0.1 m pattern, not rough"); }
    {   Store s; grid(s, [](double, double y, int i, int j) { return .42 * y + (i == 1 && j == 0 ? .5 : 0.0); });
        const auto f = s.FitSurface(.5, .5, .21, 2, nullptr);
        Check(f.ok && f.outliers == 1 && std::fabs(f.z - .21) < .02 && f.spread < .03,
              "RF-1: one tooth too high is an outlier: it neither moves the road nor widens the pattern"); }
    {   Store s; grid(s, [](double x, double, int, int) { return .2 * x; });
        const auto f = s.FitSurface(1.5, .5, .3, 2, nullptr);
        Check(f.ok && std::fabs(f.z - .3) < .02, "RF-1: a banked road: the cross slope is in the surface"); }
    {   Store s; grid(s, [](double, double, int, int) { return 0.0; }); grid(s, [](double, double, int, int) { return -5.0; });
        const auto f = s.FitSurface(.5, .5, -.3, 2.5, nullptr);
        Check(f.ok && std::fabs(f.z) < .02, "RF-1: a deck over a lower road: the level nearest the hit, never a mix"); }
    {   Store s; grid(s, [](double, double, int i, int j) { return ((i * 7 + j * 3) % 5) * .2; });
        const auto f = s.FitSurface(.5, .5, .4, 2, nullptr);
        Check(f.spread > .175 || !f.ok, "RF-1: rubble: the spread is more than half a wheel's climb (rough, not a road pattern)"); }
    {   Store s; s.Note(.5, .5, 0, 1, 1); s.Note(1.5, .5, 0, 1, 1); s.Note(2.5, .5, 0, 1, 1);
        Check(!s.FitSurface(.5, .5, 0, 2, nullptr).ok, "RF-1: three points in a row are not a surface"); }
}

int main() {
    RoadFitTests();
    Sweep();
    CachedRadarFloor();
    {   SweepStore sweep;
        sweep.Write().solids.Note(10, 10, 20, 20.6, 1);
        const auto prior = sweep.Read();
        Check(sweep.Read() == prior, "PT-1: readers share unchanged survey geometry");
        sweep.Write().solids.Forget(10, 10);
        Check(prior->solids.Known(10, 10, 20.5) && !sweep.Read()->solids.Known(10, 10, 20.5),
              "PT-1: clean evidence is visible to new readers and old readers stay immutable");
        sweep.Clear();
        Check(sweep.Read() != prior && sweep.Read()->solids.count == 0, "PT-1: changing world publishes empty survey geometry");
    }
    Store s; double z = 0;
    Check(!s.At(10.2, 5.5, 0, 1, z), "nothing surveyed: no floor");
    s.Note(10.2, 5.5, 20.1, .99, 1);
    Check(s.At(10.7, 5.9, 20.0, 1.0, z) && std::fabs(z - 20.1) < 1e-4, "a floor in the same metre square");
    Check(!s.At(11.2, 5.5, 20.0, 1.0, z), "the next square is not measured");
    s.Note(10.4, 5.2, 26.0, .98, 1);  // a deck over it
    Check(s.At(10.5, 5.5, 25.5, 1.0, z) && std::fabs(z - 26.0) < 1e-4, "stacked decks: the level near the field's wins (upper)");
    Check(s.At(10.5, 5.5, 20.5, 1.0, z) && std::fabs(z - 20.1) < 1e-4, "stacked decks: the level near the field's wins (lower)");
    Check(!s.At(10.5, 5.5, 23.0, 1.0, z), "no level within the window: not measured");
    s.Note(10.5, 5.5, 20.3, .99, 2);  // the same level measured again
    Check(s.At(10.5, 5.5, 20.0, 1.0, z) && std::fabs(z - 20.3) < 1e-4, "a newer hit of the same level replaces it");
    Check(s.cells.begin()->second.n == 2, "one level per height, not one per hit");
    for (int i = 0; i < 5; ++i) s.Note(10.5, 5.5, 30.0 + i * 2, .99, 3 + i);
    Check(s.cells.begin()->second.n == kLevels, "at most three levels; the oldest is replaced");
    Check(!s.At(-3.5, -7.5, 0, 1, z), "negative coordinates key apart");
    s.Note(-3.5, -7.5, 1.0, .9, 9); Check(s.At(-3.1, -7.9, 1.0, .5, z), "negative coordinates keep their own square");
    EdgeCount e; double prev = 0;
    for (int i = 1; i <= 10; ++i) { const double h = i * .08; CountEdge(e, prev, h, .35); prev = h; }  // a ramp: 8 cm a metre
    Check(e.edges == 0, "a ramp is not an edge");
    CountEdge(e, prev, prev + .4, .35);
    Check(e.edges == 1 && e.overFace == 1 && e.overHalf == 0 && std::fabs(e.highest - .4) < 1e-9, "a 0.4 m step is an edge over the face step");
    {   // WM-1 (2026-11-18, one world map): the 11:57 ramp. Deck rising 0.42 per metre along +y, measured every metre across x -6..6;
        // a rail top 1 m over the deck at x = 6.5; the deck's copy 3.06 m lower; a crate top 1 m over the deck at (0, 12).
        Store m; std::uint32_t st = 1;
        for (int yi = 0; yi <= 30; ++yi) for (int xi = -6; xi <= 6; ++xi) { const double x = xi + .3, y = yi + .4, z = .42 * y; m.Note(x, y, z, .92, st); m.Note(x, y, z - 3.06, .92, st); }
        for (int yi = 0; yi <= 30; ++yi) m.Note(6.6, yi + .5, .42 * (yi + .5) + 1.0, .99, st);
        m.Note(0.5, 12.5, .42 * 12.5 + 1.0, .99, st);
        double z = 0;
        Check(m.At(2.9, 7.9, .42 * 7.9, 1.0, z) && std::fabs(z - .42 * 7.9) < .02, "WM-1: the height between measurements follows the slope (plane through the neighbours)");
        Reach r; const int levels = m.Flood(0.4, 2.4, .42 * 2.4, 60, 16000, r);
        Check(levels > 300, "WM-1: the flood reaches the deck");
        double fz = 0;
        Check(m.At(4.2, 20.3, .42 * 20.3 + .5, 2.0, fz, &r) && std::fabs(fz - .42 * 20.3) < .05, "WM-1: the reachable floor 20 m up the ramp, 4 m to the side");
        Check(!(m.At(6.6, 10.5, .42 * 10.5 + 1.0, .2, fz, &r)), "WM-1: the rail top 1 m over the deck is not reachable");
        Check(!(m.At(.5, 12.5, .42 * 12.5 + 1.0, .2, fz, &r)), "WM-1: the crate top 1 m over the deck is not reachable");
        Check(m.At(1.3, 9.4, .42 * 9.4 - 3.06, .2, fz) && !m.At(1.3, 9.4, .42 * 9.4 - 3.06, .2, fz, &r), "WM-1: the lower copy is measured floor but not the car's");
        Store c; c.Note(0.5, 0.5, 0, .99, 1); c.Note(1.5, 0.5, .25, .99, 1); c.Note(2.5, 0.5, 1.25, .99, 1);
        Reach rc; c.Flood(0.5, 0.5, 0, 10, 100, rc);
        Check(rc.count(Store::Key(1.5, .5)) && !rc.count(Store::Key(2.5, .5)), "WM-1: a 0.25 m lip is climbed; a 1 m step over 1 m is not");
    }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
