// World, one flow: this car's ray volume as World shapes (fieldvolume.h). The cell vocabulary and the one collision test's reading of it.
#include "fieldvolume.h"
#include "carcap.h"
#include <cmath>
#include <cstdio>
using fieldslice::Answer; using planworld::Shape; using planworld::Verdict;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static const auto flat = [](double, double) { return 100.0; };
static const auto everywhere = [](double, double, double) { return true; };
static fieldslice::Slice Clear(double end = 16) {
    fieldslice::Slice m; m.active = true; m.start = 2; m.end = end; m.base = 100; m.left = -3.75; m.spacing = 1.5;
    for (auto& column : m.rays) for (auto& ray : column) ray = {Answer::Clear, end};
    return m;
}
static int Count(const Shape* s, int n, bool unmeasured, bool mayStop) { int k = 0; for (int i = 0; i < n; ++i) if (s[i].unmeasured == unmeasured && s[i].mayStop == mayStop) ++k; return k; }
static Verdict CarAt(const Shape& s, double x, double y, double base = 100) {
    planworld::Pose p; p.x = x; p.y = y; p.base = base; p.wanted.have = true; p.wanted.z = base; planworld::Car c; c.halfWidth = 1; c.halfLength = 2.3; c.height = 2;
    return planworld::Hit(p, c, s, 0);
}
int main() {
    Shape out[48]; fieldvolume::Counts c;
    {   auto m = Clear();
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 0 && c.doubt == 0, "a volume measured clear holds no measured shape or doubt");
        Check(n == 0 && c.unmeasured == 0, "space nothing measured beside or beyond it is cut out: no shape");
        bool inFront = false; for (int i = 0; i < n; ++i) if (CarAt(out[i], 0, 8) != Verdict::Free) inFront = true;
        Check(!inFront, "nothing in a clear volume touches a car driving through it");
        m.active = false; Check(fieldvolume::Shapes(m, 2, flat, everywhere, out, 48) == 0, "no volume, no shapes"); }
    {   auto m = Clear(); m.rays[2][0] = {Answer::Hit, 7};
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 0 && c.doubt == 1, "one hit nothing agrees with is doubt, not a solid");
        Check(Count(out, n, true, false) == n, "and nothing in that volume may stop the car");
        m.rays[2][1] = {Answer::Hit, 7.1};
        n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 1 && c.doubt == 0, "agreeing hits are one solid");
        const Shape* solid = nullptr; for (int i = 0; i < n; ++i) if (out[i].mayStop) solid = &out[i];
        Check(solid && solid->source == planworld::Source::Volume && !planworld::Predicted(*solid), "the solid is a live measurement, not a prediction");
        Check(solid && std::abs(solid->centre.x + .75) < 1e-9 && std::abs(solid->centre.y - 7.1) < 1e-9, "it stands where the rays hit, its near face at the hit depth");
        Check(solid && solid->halfWidth <= .1 + 1e-9 && solid->halfLength <= .1 + 1e-9, "VT-1: a solid is the measured hit (0.2 m), not the column's width");
        Check(solid && CarAt(*solid, 0, 6) != Verdict::Free, "the one collision test finds it in the car's own lane");
        Check(solid && CarAt(*solid, 2.25, 6) == Verdict::Free, "and the neighbouring lane still fits the car");
        Check(solid && CarAt(*solid, 0, 6, 96) == Verdict::Free, "a car on a road deck below is not touched by it"); }
    {   auto m = Clear(); m.rays[2][0] = {Answer::Hit, 8}; m.rays[3][0] = {Answer::Hit, 8.1};  // tyre height only, two columns agree
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 2 && n == 2, "E: agreeing hits only at tyre height are measured shapes too (the volume has no low-edge verdict of its own; was: low edge)");
        bool all = true, climbs = false; double top = 0; for (int i = 0; i < n; ++i) { if (!out[i].mayStop) all = false; if (out[i].tireClimb) climbs = true; top = std::max(top, planworld::Top(out[i])); }
        Check(all && !climbs, "E: a measured shape may stop; whether it is climbed is Plan's, against World's road");
        Check(std::abs(top - 100.30) < 1e-9, "E: its top is the measured hit (the 0.30 m beam), unpadded: under the one car's 0.35 m climb (carcap)");
        Check(n > 0 && CarAt(out[0], -.75, 6) == Verdict::Free, "E: Plan's one test reads a 0.30 m measured face on the road as driven over (within the 0.35 m climb)");
        m.rays[2][1] = {Answer::Hit, 8.2};
        n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        const Shape* tall = nullptr; for (int i = 0; i < n; ++i) if (planworld::Top(out[i]) > 100.6) tall = &out[i];
        Check(c.solid == 2 && tall && std::abs(planworld::Top(*tall) - 100.67) < 1e-9, "occupied up to the 0.67 m beam at one face: one shape reaching 0.67 m");
        Check(tall && CarAt(*tall, -.75, 6) != Verdict::Free, "and Plan meets it: over the 0.35 m climb (was: the volume's own 0.5 m pass height)"); }
    {   auto m = Clear(); m.rays[2][2] = {Answer::Hit, 9, false, 0, 101.0}; m.rays[2][3] = {Answer::Hit, 9.1, false, 0, 101.4};  // an overhang at 1.0-1.4 m
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        const Shape* solid = nullptr; for (int i = 0; i < n; ++i) if (out[i].mayStop) solid = &out[i];
        Check(solid && std::abs(planworld::Base(*solid) - 100.85) < 1e-9 && std::abs(planworld::Top(*solid) - 101.4) < 1e-9, "a solid takes its height from where the rays really hit (top: the highest hit, E; bottom: the lowest less the pad)");
        planworld::Pose p; p.x = -.75; p.y = 8; p.base = 100; p.wanted.have = true; p.wanted.z = 100; planworld::Car low; low.halfWidth = .5; low.halfLength = 1; low.height = .7;
        Check(solid && planworld::Hit(p, low, *solid, 0) == Verdict::Free && CarAt(*solid, -.75, 8) != Verdict::Free, "a low vehicle passes under an overhang a tall one meets"); }
    {   auto m = Clear(); for (auto& ray : m.rays[2]) ray = {Answer::Unknown, 0};
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.unmeasured == 0 && n == 0, "a column no ray answered for is no information: cut out, no shape");
        bool found = false; for (int i = 0; i < n; ++i) if (CarAt(out[i], 0, 6) != Verdict::Free) found = true;
        Check(!found, "so nothing there slows or stops the car");
        for (auto& ray : m.rays[2]) ray = {Answer::Clear, m.start, true, 0};  // never started inside the field
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.unmeasured == 0, "a column outside the field is none of the doubts");
        m = Clear(); for (auto& column : m.rays) for (auto& ray : column) ray = {Answer::Clear, 5, false, 0};  // ended on drivable floor at 5 m
        n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        bool before = false, past = false; for (int i = 0; i < n; ++i) { if (CarAt(out[i], 0, 2) != Verdict::Free) before = true; if (CarAt(out[i], 0, 9) != Verdict::Free) past = true; }
        Check(!before && !past, "a ray that ended on drivable floor is clear up to it; past it is no information, no shape"); }
    {   auto m = Clear(); for (auto& column : m.rays) for (auto& ray : column) ray = {Answer::Clear, 7, true, 0};  // clear to where the field ended
        const auto field = [](double, double y, double) { return y <= 7; };
        int n = fieldvolume::Shapes(m, 2, flat, field, out, 48, &c);
        bool beyond = false, inLane = false;
        for (int i = 0; i < n; ++i) { if (out[i].centre.y + out[i].halfLength > 8.01) beyond = true; if (CarAt(out[i], 0, 4) != Verdict::Free) inLane = true; }
        Check(!beyond && !inLane, "past the field's end, outside the field, nothing is said: that is the field's own rule");
        const auto bend = [](double, double y, double) { return y <= 7 || y >= 11; };  // the field comes back into the volume (a bend)
        fieldvolume::Shapes(m, 2, flat, bend, out, 48, &c);
        Check(c.unmeasured == 0, "legal space past the gap that no ray reached is no information, no shape"); }
    {   auto m = Clear(); m.yaw = 1.5707963267948966; m.x = 50; m.y = -20; m.rays[2][0] = {Answer::Hit, 7}; m.rays[2][1] = {Answer::Hit, 7.1};  // facing +x
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        const Shape* solid = nullptr; for (int i = 0; i < n; ++i) if (out[i].mayStop) solid = &out[i];
        Check(solid && std::abs(solid->centre.x - 57.1) < 1e-6 && std::abs(solid->centre.y + 19.25) < 1e-6, "shapes are placed in the world from the volume's own position and heading");
        m.rays[6][0] = {Answer::Hit, 7}; m.rays[6][1] = {Answer::Hit, 7.1}; m.rays[10][0] = {Answer::Hit, 7}; m.rays[10][1] = {Answer::Hit, 7.1};  // three separate solids
        Check(fieldvolume::Shapes(m, 2, flat, everywhere, out, 2, &c) == 2 && c.dropped > 0, "a full output is counted, never overrun"); }
    {   // F1: the volume as one more source beside the older ones (flight, job and combat lines): only what the rays hit becomes shapes.
        auto m = Clear();
        for (auto& ray : m.rays[1]) ray = {Answer::Unknown, 0};
        m.rays[3][0] = {Answer::Hit, 9}; m.rays[4][0] = {Answer::Hit, 9.2};  // two low hits that agree: in the air that is a solid
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, false);
        Check(c.unmeasured == 0 && c.solid == 2 && n == 2, "as an extra source the volume gives its measured shapes and says nothing about what it did not measure");
        Check(n == 2 && out[0].mayStop && out[1].mayStop, "E: in the air or on wheels the same measured shapes (was: a low edge on wheels by the pass height)"); }
    {   // Redundancy in space: dense beams agree by lateral distance, not by column index.
        auto m = Clear(); m.spacing = .5;
        m.rays[6][1] = {Answer::Hit, 9}; m.rays[8][1] = {Answer::Hit, 9.2};  // 1.0 m apart across
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 2 && c.doubt == 0, "dense beams 1 m apart that hit at one depth agree: solid");
        m = Clear(); m.spacing = .5;
        m.rays[2][1] = {Answer::Hit, 9}; m.rays[8][1] = {Answer::Hit, 9.2};  // 3.0 m apart across
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 0 && c.doubt == 2, "hits 3 m apart do not agree however many columns lie between");
        Check(fieldslice::Neighbours(.5) == 3 && fieldslice::Neighbours(1.5) == 1, "agreement reach follows the spacing"); }
    {   // Ground truth (2026-11-11 06:00:44 stop on a 2.4 m ramp): a hit's recorded height is its height; with none, its beam's own height.
        auto m = Clear();
        m.rays[2][1] = {Answer::Hit, 9}; m.rays[2][2] = {Answer::Hit, 9.1};  // beams flown at 0.67 m and 1.03 m, no hit height recorded
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 1 && n == 1 && std::abs(planworld::Top(out[0]) - 101.03) < 1e-9, "two agreeing beams with no recorded height stand at their beams' heights");
        m.rays[2][1].z = 100.1; m.rays[2][2].z = 100.3;  // the hits were recorded 0.1-0.3 m over the road
        n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 1 && n == 1 && CarAt(out[0], -.75, 7) == Verdict::Free, "E: the same hits recorded 0.1-0.3 m over the road are driven over by Plan (under the 0.35 m climb), never a stop by the volume's own verdict");
        m.rays[2][1].z = 100.9; m.rays[2][2].z = 101.2;
        n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c);
        Check(c.solid == 1 && n == 1 && CarAt(out[0], -.75, 7) == Verdict::Tall, "a face recorded 0.9-1.2 m tall is met"); }
    {   // E (ramp, 2026-11-19): a 23 deg ramp rises from 4 m ahead; the slice's straight grade is flat, so its beams meet the deck at their
        // own heights, each level at its own depth. Merged into one shape per column (the old rule) the far beams' heights stood 1.2 m over
        // the near road: a ramp read as a wall. Each face keeps its own height, and Plan, given the road there, drives over all of them.
        auto m = Clear(); const double g = std::tan(23 * 3.14159265358979323846 / 180);
        const auto road = [&](double d) { return 100 + std::max(0.0, d - 4) * g; };
        for (int col = 1; col <= 4; ++col) for (int h = 0; h < fieldslice::levels; ++h) m.rays[col][h] = {Answer::Hit, 4 + m.heightAbove[h] / g, false, 0, 100 + m.heightAbove[h]};
        int n = fieldvolume::Shapes(m, 2, [&](double d, double) { return road(d); }, everywhere, out, 48, &c);
        Check(c.solid == 16 && n == 16 && c.doubt == 0, "E (ramp): every beam on the deck agrees: one shape per beam, none merged across depths");
        bool over = false, met = false;
        for (int i = 0; i < n; ++i) {
            const double r = road(out[i].centre.y);
            if (planworld::Top(out[i]) - r > carcap::kCar.climb) over = true;
            planworld::Pose p; p.x = out[i].centre.x; p.y = out[i].centre.y - 2.0; p.base = r; p.wanted.have = true; p.wanted.z = r;
            planworld::Car car; car.halfWidth = 1; car.halfLength = 2.3; car.height = 2;
            if (planworld::Hit(p, car, out[i], 0) != Verdict::Free) met = true;
        }
        Check(!over, "E (ramp): no shape stands more than the one car's climb (0.35 m, carcap) over the ramp's road under it");
        Check(!met, "E (ramp): Plan's one collision test, given the road there (as AssignRoad gives World's fitted road), drives over every one"); }
    {   // OA-1 (2026-11-11 10:49 run, a fallen branch): the same object hit again agrees; remembered until a beam passes through the spot.
        const auto hitAt = [](fieldslice::Slice& m, int col, int level, double depth, unsigned owner, double at) {
            auto& r = m.rays[col][level];
            r = {Answer::Hit, depth, false, at, 100 + m.heightAbove[level]}; r.owner = owner; r.hx = m.left + col * m.spacing; r.hy = depth;
            m.Remember(owner, r.hx, r.hy, r.z, m.heightAbove[level], at);
        };
        static fieldslice::Memory mem;
        const auto fresh = [&]() { auto s = Clear(); mem = {}; s.memory = &mem; return s; };
        auto m = fresh();
        hitAt(m, 1, 0, 7, 0x6002AB2, 1); hitAt(m, 2, 2, 8.5, 0x6002AB3, 1);  // trunk at tyre height and a twig 2.1 m away: two objects
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, true, 1);
        Check(c.solid == 0 && c.byObject == 0, "two different objects too far apart for the beam rule do not agree");
        m = fresh();
        hitAt(m, 1, 0, 7, 0x6002AB2, 1); hitAt(m, 2, 2, 8.5, 0x6002AB2, 1);  // the same branch: trunk low, twig high, 1.5 m apart in depth
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, true, 1);
        Check(c.solid >= 1 && c.byObject >= 1 && c.doubt == 0, "the same object hit low and high close by is one solid, not doubt");
        m = fresh();
        hitAt(m, 2, 2, 8.5, 0x6002ABA, 1); hitAt(m, 2, 2, 8.5, 0x6002ABA, 2);  // one beam meets the same twig on two refreshes
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, true, 2);
        Check(c.solid == 1 && c.doubt == 0, "the same spot hit again on a later refresh agrees in time");
        m = fresh();
        hitAt(m, 1, 0, 7, 0x6002AB2, 1); hitAt(m, 2, 2, 8.5, 0x6002AB2, 1);
        for (auto& column : m.rays) for (auto& ray : column) ray = {Answer::Clear, 16, false, 2};  // next refresh: every beam missed it
        m.rays[2][2].depth = 8.0; m.rays[1][0].depth = 6.5;  // the beams through the twig and the trunk ended short of them
        Check(m.ClearPassed(2) == 0, "beams that missed beside a remembered point or ended short of it do not clear it");
        int n = fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, true, 2);
        bool stops = false; for (int i = 0; i < n; ++i) if (out[i].mayStop) stops = true;
        Check(c.remembered >= 1 && stops, "the branch stays a solid on a refresh where no beam met it");
        m.rays[2][2].depth = 16;  // now a beam at the twig's height passes right through its spot
        Check(m.ClearPassed(2) == 1, "a beam measured passing through the spot clears that point");
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, true, 2);
        Check(c.remembered == 0, "with the twig's spot measured clear the trunk alone is no agreement: no remembered shape");
        m = fresh();
        hitAt(m, 1, 0, 7, 0x6002AB2, 1); hitAt(m, 2, 2, 8.5, 0x6002AB2, 1);
        for (auto& column : m.rays) for (auto& ray : column) ray = {Answer::Clear, 16, false, 0};
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c, true, 1 + fieldslice::memorySeconds + .5);
        Check(c.remembered == 0, "a remembered point no beam came back to ages out");
        m = fresh();
        hitAt(m, 1, 0, 7, 0x6002AB2, 1); hitAt(m, 2, 2, 8.5, 0x6002AB2, 1);
        for (int k = 0; k < 40; ++k) m.Remember(0x1234, 3.0 + k * .5, 12, 101, 1, 1.5);  // a long wall hit by many beams
        Check(m.SameObject(0x6002AB2, -2.25, 7, 100.3, -1, 1.5) == 2, "one big object keeps its own share and never crowds the branch out of memory");
        Check(m.SameObject(0x1234, 3.0, 12, 101, -1, 1.5) <= fieldslice::perObjectPoints, "the wall keeps at most its share of points");
        auto none = Clear(); none.Remember(0x6002AB2, 0, 7, 100, 1, 1);
        Check(none.MemoryLive(1) == 0 && none.SameObject(0x6002AB2, 0, 7, 100, -1, 1) == 0, "a volume without a memory remembers nothing and agrees by beams only"); }
    {   // WT-2 (2026-11-12, ramp wall lip at 13:11:41): an underside was raised over the pass height so the volume would not call it a low edge.
        // E: the volume has no low edge; a hit's aboveGround decides nothing in it. The same hits give the same measured shapes either way.
        auto m = Clear(); m.rays[2][0] = {Answer::Hit, 8}; m.rays[3][0] = {Answer::Hit, 8.1};
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c); const int before = c.solid;
        m.rays[2][0].aboveGround = .51; m.rays[3][0].aboveGround = .51;
        fieldvolume::Shapes(m, 2, flat, everywhere, out, 48, &c); Check(before == 2 && c.solid == 2, "E: two hits at the lowest beam are measured shapes, aboveGround or not (was: low edge, or solid when raised by WT-2)"); }
    std::printf(failed ? "\nFAILED %d\n" : "\nALL PASS\n", failed);
    return failed ? 1 : 0;
}
