// World radar (radar.h): the sweep schedule, the box from a body's shape and transform, the list rules, and what reaches the Plan.
#include "radar.h"
#include "planinput.h"
#include "energyplan.h"
#include <cstdio>
#include <cmath>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace radar;

static Lookup BoxLookup(double cx, double cy, double cz, double ex, double ey, double ez, double yaw, bool rows) {
    // A body at (cx, cy, cz) turned by `yaw` about z (FVO heading sense: +x when yaw = 90 deg is "along"), shape box +-e about its origin.
    Lookup L; L.ok = true; const double c = std::cos(yaw), s = std::sin(yaw);
    // Local y (along) maps to world (sin yaw, cos yaw); local x (across) to (cos yaw, -sin yaw); z up.
    const double col[3][3] = { { c, -s, 0 }, { s, c, 0 }, { 0, 0, 1 } };  // columns: images of local x, y, z
    for (int k = 0; k < 3; ++k) for (int i = 0; i < 3; ++i) { if (!rows) L.rot[4 * k + i] = float(col[k][i]); else L.rot[4 * i + k] = float(col[k][i]); }
    L.pos[0] = float(cx); L.pos[1] = float(cy); L.pos[2] = float(cz);
    L.lo[0] = float(-ex); L.lo[1] = float(-ey); L.lo[2] = float(-ez); L.hi[0] = float(ex); L.hi[1] = float(ey); L.hi[2] = float(ez);
    return L;
}

int main() {
    {   // Schedule: the rotating pair covers a full turn; the forward fan reaches further with speed and sweeps left to right; rays start outside the hull.
        State st; Ray r[8]; double minA = 1e9, maxA = -1e9; int forward = 0; double reachSlow = 0, reachFast = 0; bool outside = true;
        for (int t = 0; t < int(rules::turnSteps / rules::rotateRays); ++t) {
            const int n = Plan(st, 0, 0, 0, 0, t < 6 ? 2.0 : 15.0, 1.0, 2.3, r, 8);
            for (int i = 0; i < n; ++i) {
                if (std::hypot(r[i].from[0], r[i].from[1]) < 2.3) outside = false;
                if (r[i].kind == 0) { const double a = std::atan2(r[i].to[0], r[i].to[1]); minA = std::min(minA, a); maxA = std::max(maxA, a); }
                else { ++forward; const double d = r[i].to[1]; if (t < 6) reachSlow = std::max(reachSlow, d); else reachFast = std::max(reachFast, d); }
            }
        }
        Check(maxA - minA > 2 * kPi * .9, "radar plan: the rotating sweep covers a full turn");
        Check(forward > 0 && reachFast > reachSlow * 2 && reachSlow >= rules::aheadMin - 1e-6, "radar plan: the forward fan reaches further at speed (TTC)");
        Check(outside, "radar plan: every ray starts outside the vehicle's own hull");
    }
    {   // Box: an axis-aligned body gives its footprint and height; a body turned 90 deg gives the turned footprint.
        Box b; Lookup L = BoxLookup(10, 20, 1, 1.0, 3.0, .5, 0, false);
        Check(BoxFrom(L, 10, 17.0, 1.0, b) && std::fabs(b.cx - 10) < 1e-6 && std::fabs(b.cy - 20) < 1e-6 && std::fabs(b.halfL - 3) < 1e-6 && std::fabs(b.halfW - 1) < 1e-6
              && std::fabs(b.zlo - .5) < 1e-6 && std::fabs(b.zhi - 1.5) < 1e-6 && b.gap < 1e-6, "radar box: an unturned body's footprint, height, and the hit on its surface");
        Box t; Lookup R = BoxLookup(0, 0, 1, 1.0, 3.0, .5, kPi / 2, false);
        Check(BoxFrom(R, 3.0, 0, 1.0, t) && std::fabs(std::fabs(std::remainder(t.heading - kPi / 2, kPi))) < 1e-6 && std::fabs(t.halfL - 3) < 1e-6 && std::fabs(t.halfW - 1) < 1e-6,
              "radar box: a body turned 90 deg points along +x with the same footprint");
        // The transform layout (columns or rows) is picked from where the hit is: a turned body read the wrong way would put the hit 2+ m off its box.
        Box c1, c2; Lookup Lc = BoxLookup(0, 0, 1, .5, 4.0, .5, kPi / 3, false), Lr = BoxLookup(0, 0, 1, .5, 4.0, .5, kPi / 3, true);
        const double hx = std::sin(kPi / 3) * 4.0, hy = std::cos(kPi / 3) * 4.0;  // the far end of the turned body
        Check(BoxFrom(Lc, hx, hy, 1, c1) && BoxFrom(Lr, hx, hy, 1, c2) && c1.convention == 0 && c2.convention == 1 && c1.gap < .01 && c2.gap < .01
              && std::fabs(c1.heading - c2.heading) < 1e-6, "radar box: the transform layout is read from the hit (columns or rows), same box either way");
        Lookup bad = L; bad.lo[0] = float(NAN); Box x; Check(!BoxFrom(bad, 0, 0, 0, x), "radar box: an unusable box is refused (kept as a point instead)");
        Check(BoxIsSolid(0) && BoxIsSolid(5) && BoxIsSolid(16) && !BoxIsSolid(8) && !BoxIsSolid(11) && !BoxIsSolid(13) && !BoxIsSolid(19) && !BoxIsSolid(-1),
              "radar shape: only primitive and convex shapes (0-5, 16) are their own box; meshes (8), compounds (11), height fields (13), LOD (19) and unknown give points");
        Lookup huge = BoxLookup(0, 0, 0, 40, 40, 5, 0, false); Box hb; Check(BoxFrom(huge, 0, 40, 0, hb) && hb.big, "radar box: a body bigger than the plan's window (terrain, building) gives its points");
        Lookup ramp = BoxLookup(0, 0, 0, 2.0, 9.99, 5.7, 0, false); Box rb; Check(BoxFrom(ramp, 0, 9.99, 0, rb) && !rb.big, "radar box: a 19.98 m ramp piece is not cut by size (the floor evidence decides)");
        Box f1, f2; Lookup Lc2 = BoxLookup(0, 0, 1, .5, 4.0, .5, kPi / 3, false);
        Check(BoxFrom(Lc2, hx, hy, 1, f1) && BoxFrom(Lc2, 0, 0, 1, f2, f1.convention) && f2.convention == f1.convention, "radar check: a box is rebuilt with the convention it was built with");
    }
    {   // List: a second hit refreshes; the cap keeps the freshest; stale and far boxes leave.
        State st; Box b; b.body = 7; b.cx = 5; Note(st, b, 1.0); Check(Refresh(st, 7, 2.0, 5, 0, 0) && st.boxes[0].hits == 2 && st.boxes[0].lastHit == 2.0, "radar list: a hit on a known body refreshes it (no lookup)");
        for (int i = 0; i < kMaxBoxes + 5; ++i) { Box o; o.body = 100 + i; Note(st, o, 3.0 + i * .01); }
        Check(st.count == kMaxBoxes, "radar list: capped, the stalest makes room");
        State e; Box s1; s1.body = 1; s1.cx = 0; Box s2; s2.body = 2; s2.cx = 500; Box s3; s3.body = 3; s3.dynamic = true;
        Note(e, s1, 0); Note(e, s2, 0); Note(e, s3, 0); Expire(e, 2.0, 0, 0);
        Check(e.count == 1 && e.boxes[0].body == 1, "radar list: a far box and a stale moving body leave, a near static box stays");
        // 19:14 run: the tree was boxed 32 s before the car hit it and dropped 4 s later. A static box now stays while it is in range.
        State k; Box tree; tree.body = 9; tree.cx = 20; Note(k, tree, 0); Expire(k, 60.0, 0, 0);
        Check(k.count == 1, "radar list: a static box is still listed 60 s after its only hit while it is in range");
        planworld::Shape ks[4]; Check(ToShapes(k, 60.0, ks, 4) == 1 && ks[0].expires > 60.0, "radar list: ... and the Plan still trusts it (not expired)");
        Remove(k, NextToVerify(k)); Check(k.count == 0, "radar list: a box the game check finds gone or moved is removed");
    }
    {   // Drift: the rotating rays do not repeat the same angles turn after turn, so a thin object between two rays is reached within a few turns.
        State st; Ray r[8]; double seen[6][24]; int turns = 6;
        for (int t = 0; t < turns; ++t) for (int k = 0; k < 12; ++k) { const int n = Plan(st, 0, 0, 0, 0, 0, 1, 2, r, 8); int j = 0;
            for (int i = 0; i < n; ++i) if (r[i].kind == 0) { double a = std::atan2(r[i].to[0], r[i].to[1]); if (a < 0) a += 2 * kPi; seen[t][2 * k + j++] = a; } }
        // A target 0.3 m wide at the full radius (half a step wide in angle), centred midway between two first-turn rays.
        const double step = 2 * kPi / rules::turnSteps, target = seen[0][0] + step * .5, half = .3 / rules::radius / 2; bool hit = false, hitFirst = false;
        for (int t = 0; t < turns; ++t) for (int i = 0; i < 24; ++i) { const double d = std::fabs(std::remainder(seen[t][i] - target, 2 * kPi)); if (d <= half + 1e-9) { hit = true; if (t == 0) hitFirst = true; } }
        Check(!hitFirst && hit, "radar drift: a 0.3 m object between two rays is missed on the first turn and reached on a later one");
    }
    {   // Path sweep pieces: contiguous from the car to the reach, cut at bends, straight on past the path's end; no path = along the nose.
        Segment s[rules::sweepSegments];
        const int n0 = SweepPlan(nullptr, nullptr, 0, 5, 5, 0, 30, s, rules::sweepSegments); double len0 = 0; for (int i = 0; i < n0; ++i) len0 += s[i].length;
        Check(n0 == 2 && std::fabs(s[0].x0 - 5) < 1e-9 && std::fabs(s[0].y0 - 5) < 1e-9 && std::fabs(len0 - 30) < 1e-6 && std::fabs(s[n0 - 1].y1 - 35) < 1e-6,
              "radar sweep plan: no path -> straight along the nose from the car to the reach (pieces <= 20 m)");
        double px[12], py[12]; int pn = 0;  // 10 m north, then a 90 deg bend east for 10 m
        for (int i = 0; i <= 5; ++i) { px[pn] = 0; py[pn] = 2.0 * i; ++pn; } for (int i = 1; i <= 5; ++i) { px[pn] = 2.0 * i; py[pn] = 10; ++pn; }
        const int n1 = SweepPlan(px, py, pn, 0, 0, 0, 25, s, rules::sweepSegments); bool joined = true; double len1 = 0;
        for (int i = 0; i < n1; ++i) { len1 += s[i].length; if (i && (std::fabs(s[i].x0 - s[i - 1].x1) > 1e-9 || std::fabs(s[i].y0 - s[i - 1].y1) > 1e-9)) joined = false; }
        Check(n1 == 3 && joined && std::fabs(len1 - 25) < 1e-6 && std::fabs(s[1].heading - kPi / 2) < 1e-6 && std::fabs(s[0].x1) < 1e-9 && std::fabs(s[0].y1 - 10) < 1e-9 && std::fabs(s[2].x1 - 15) < 1e-6,
              "radar sweep plan: follows the bend piece by piece, joined end to start, and goes straight on past the path's end to the reach");
        const int n2 = SweepPlan(px, py, pn, 0, 0, 0, 6, s, rules::sweepSegments);
        Check(n2 == 1 && std::fabs(s[0].length - 6) < 1e-6, "radar sweep plan: a short reach stops inside the path");
    }
    {   // SR-1 (2026-11-19) replaced the radar's own Classify / Judge (surface.h, surface_test). Hit points of a static body stay while it is listed.
        State st; Box d; d.body = 1; d.cx = 0; d.cy = 10; d.heading = 0; d.halfL = 9.99; d.halfW = 2; d.zlo = -1; d.zhi = 10.5; d.big = true;
        Note(st, d, 0); NotePoint(st, 1, 0, 1, 3, 4, .5, true, 0, 0);
        Expire(st, 2.0, 0, 0); Check(st.pointCount == 1, "radar points: a static body's hit point stays while the body is listed");
        Remove(st, 0); Expire(st, 2.0, 0, 0); Check(st.pointCount == 0, "radar points: ... and leaves once the body is gone and the point is stale");
    }
    {   // Airy bodies (19:55 run): the fallen tree's box filled the road; lines through its core touched nothing of it.
        State st; Box tree; tree.body = 9; tree.collides = true; tree.cx = 0; tree.cy = 10; tree.heading = 0; tree.halfL = 2.8; tree.halfW = 2.6; tree.zlo = 0; tree.zhi = 5;
        Note(st, tree, 0); NotePoint(st, 9, 0, 1, 2.5, 8.0, .9, false, 0, 0);  // one branch tip the sweep touched
        Check(CrossesCore(tree, -5, 10, 1.5, 5, 10, 1.5) && !CrossesCore(tree, -5, 12.5, 1.5, 5, 12.5, 1.5) && !CrossesCore(tree, -5, 10, 4.9, 5, 10, 4.9),
              "radar air: a line through the box's middle crosses its core; one along its edge or top does not");
        int turned[4]; const std::uint32_t touchedTree = 9;
        Check(NoteAir(st, -5, 10, 1.5, 5, 10, 1.5, &touchedTree, 1, turned, 4) == 0 && st.boxes[0].air == 0, "radar air: a line that touched the body proves nothing");
        Check(NoteAir(st, -5, 10, 1.5, 5, 10, 1.5, nullptr, 0, turned, 4) == 0 && !st.boxes[0].airy, "radar air: one pass is not enough");
        Check(NoteAir(st, 0, 4, 1.5, 0, 16, 1.5, nullptr, 0, turned, 4) == 1 && st.boxes[0].airy, "radar air: the second pass makes the box airy");
        planworld::Shape out[8]; const int n = ToShapes(st, 1, out, 8);
        Check(n == 1 && out[0].source == planworld::Source::Ray && std::fabs(out[0].centre.x - 2.5) < 1e-9 && out[0].expires > 1,
              "radar air: an airy body reaches the Plan as its hit points (kept while listed), not its box");
        // The Plan's scene: the box blocked the road; the airy body leaves the far side open.
        planrun::Frame f; f.now = 1; f.carBase = 0; planscene::Scene sc; sc.floor = [](double, double, double& z) { z = 0; return true; };
        planinput::AddShapes(sc, f, out, n); sc.Index();
        bicycle::Spec k; k.car = carmodel::Fallback(2.3); k.halfWidth = 1.0; k.halfLength = 2.3;
        Check(sc.Clearance(-1.5, 10, 0, 0, k) > .5, "radar air: the open half of the tree's box is free road in the Plan's scene");
        State r; Box rock = tree; rock.body = 3; Note(r, rock, 0); NotePoint(r, 3, 0, 1, 0, 7.4, 1, false, 0, 0);
        planworld::Shape ro[4]; Check(ToShapes(r, 1, ro, 4) == 1 && ro[0].source == planworld::Source::Radar, "radar air: a solid body's points are not given while its box speaks for it");
    }
    {   // To the Plan: collides -> a measured solid with its real box; passes through -> nothing; ground -> nothing; big -> its hit point.
        State st; Box a; a.body = 1; a.collides = true; a.layer = 1; a.cx = 0; a.cy = 10; a.zlo = 0; a.zhi = 1.5; a.halfL = 3; a.halfW = 1;
        Box p = a; p.body = 2; p.collides = false; Box g = a; g.body = 3; g.layer = 13; Box big = a; big.body = 4; big.big = true; big.hitX = 1; big.hitY = 2; big.hitZ = .5;
        Note(st, a, 1); Note(st, p, 1); Note(st, g, 1); Note(st, big, 1); NotePoint(st, 4, 0, 1, 1, 2, .5, false, 0, 1);  // the engine notes a big body's hit point
        planworld::Shape out[8]; const int n = ToShapes(st, 1, out, 8);
        Check(n == 2 && out[0].source == planworld::Source::Radar && !planworld::Predicted(out[0]) && std::fabs(out[0].halfLength - 3) < 1e-9
              && out[1].source == planworld::Source::Ray && std::fabs(out[1].centre.x - 1) < 1e-9, "radar shapes: collides -> real box (measured, not predicted); passes / ground -> none; big -> hit point");
        // End to end: the box is a solid in the Plan's scene and blocks a straight rollout through it.
        planrun::Frame f; f.now = 1; f.carBase = 0;
        planscene::Scene sc; sc.floor = [](double, double, double& z) { z = 0; return true; };
        planinput::AddShapes(sc, f, out, 1); sc.Index();
        bicycle::Spec k; k.car = carmodel::Fallback(2.3); k.halfWidth = 1.0; k.halfLength = 2.3;
        Check(!sc.solids.empty() && sc.Clearance(0, 10, 0, 0, k) < 0 && sc.Clearance(6, 10, 0, 0, k) > .5, "radar shapes: the box is a solid in the Plan's scene where the body is, free beside it");
    }
    {   // MP-2 (19:44 run: the store full on the ramp, rail points beside the car dropped by age): a full store drops the farthest point.
        radar::State st;
        for (int i = 0; i < radar::kMaxPoints / 2; ++i) radar::NotePoint(st, 1, 0, 1, 1.0 + .5 * (i % 20), .5 * (i / 20), 0, false, 0, 0.0);      // near, oldest
        for (int i = 0; i < radar::kMaxPoints / 2; ++i) radar::NotePoint(st, 2, 0, 1, 200.0 + .5 * (i % 20), .5 * (i / 20), 0, false, 0, 1.0);  // far, newer
        const int full = st.pointCount;
        for (int i = 0; i < 50; ++i) radar::NotePoint(st, 3, 0, 1, 2.0 + .5 * i, -3.0, 0, false, 0, 2.0);  // new near points
        int nearKept = 0, farKept = 0, newKept = 0;
        for (int i = 0; i < st.pointCount; ++i) { nearKept += st.points[i].body == 1; farKept += st.points[i].body == 2; newKept += st.points[i].body == 3; }
        std::printf("   point store: %d of %d, near old kept %d of %d, far kept %d, new kept %d, dropped %ld (farthest %.0f m)\n",
                    st.pointCount, radar::kMaxPoints, nearKept, radar::kMaxPoints / 2, farKept, newKept, st.stats.pointsDropped, st.stats.dropFar);
        Check(full == radar::kMaxPoints && nearKept == radar::kMaxPoints / 2 && newKept == 50 && st.stats.pointsDropped == 50,
              "MP-2: a full point store drops the farthest points, never the near ones (however old)");
    }
    {   // "One car" (2026-11-19): the own car's size from its collision box, along and across its heading, under either transform layout.
        bool ok = true;
        for (bool rows : { false, true }) for (double yaw : { 0.0, 0.7, 2.4, -1.9 }) {
            OwnSize o; const bool got = OwnSizeFrom(BoxLookup(5, 7, 1, 1.0, 2.3, .8, yaw, rows), yaw, o);
            ok &= got && std::fabs(o.halfWidth - 1.0) < .01 && std::fabs(o.halfLength - 2.3) < .01 && std::fabs(o.height - 1.6) < .01;
            OwnSize b; ok &= OwnSizeFrom(BoxLookup(5, 7, 1, 1.0, 2.3, .8, yaw + kPi, rows), yaw, b) && std::fabs(b.halfLength - 2.3) < .01;  // frame facing back
        }
        Check(ok, "own size: 2.0 x 4.6 m car measured along and across its heading (columns or rows, any yaw)");
        OwnSize z, big, sideways;
        Check(!OwnSizeFrom(BoxLookup(0, 0, 0, 0, 0, 0, .3, false), .3, z), "own size: a zero box is not a car size (bounds kept)");
        Check(!OwnSizeFrom(BoxLookup(0, 0, 0, 1.0, 30, 1, .3, false), .3, big), "own size: a 60 m body is not a car size");
        Check(!OwnSizeFrom(BoxLookup(0, 0, 0, 2.3, 1.0, .8, .3, false), .3, sideways) || sideways.halfWidth > sideways.halfLength,
              "own size: measured across the heading, never swapped to fit");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
