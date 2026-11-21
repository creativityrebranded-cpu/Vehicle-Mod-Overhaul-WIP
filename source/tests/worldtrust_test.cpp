#include "worldtrust.h"
#include <cstdio>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace worldtrust;
using planworld::Shape; using planworld::Source;

static Shape Pred(Source s, double x, double y) { Shape p; p.source = s; p.centre = { x, y, 101.5 }; p.halfWidth = p.halfLength = 1; p.halfHeight = 1.5; return p; }
static Shape Hit(double x, double y, int support) { Shape r; r.source = Source::Ray; r.centre = { x, y, 101 }; r.support = support; r.haveNormal = true; r.seenAt = 10; return r; }

int main() {
    CleanStore clean; const double now = 10;
    {   const Shape box = Pred(Source::Box, 0, 10);
        Check(Judge(box, nullptr, 0, clean, now, true) == Why::Kept, "no evidence: a coarse box stands");
        const float a[3] = { -3, 10, 101 }, b[3] = { 1, 10, 101 }; clean.Add(a, b, 9);
        Check(Judge(box, nullptr, 0, clean, now, true) == Why::Clean, "a clean face ray through a coarse box clears it (was never cleared)");
        Check(Judge(box, nullptr, 0, clean, now, false) == Why::Kept, "rays down: nothing dropped (fail-safe)");
        Check(Judge(box, nullptr, 0, clean, 9 + rules::cleanAge + .1, true) == Why::Kept, "old clean ray: no longer evidence");
        const Shape far = Pred(Source::Box, 0, 13); Check(Judge(far, nullptr, 0, clean, now, true) == Why::Kept, "a clean ray elsewhere clears nothing"); }
    {   CleanStore c2; const float a[3] = { -3, 20, 106 }, b[3] = { 1, 20, 106 }; c2.Add(a, b, 9);
        Check(Judge(Pred(Source::Survey, 0, 20), nullptr, 0, c2, now, true) == Why::Kept, "a clean ray above the solid's top clears nothing"); }
    {   const Shape sv = Pred(Source::Survey, 0, 30); Shape hits[2] = { Hit(.5, 30, 3), Hit(.6, 30.2, 3) };
        Check(Judge(sv, hits, 2, CleanStore{}, now, true) == Why::Measured, "confirmed live hits measured the spot: the precise hits stand for it");
        Shape weak[1] = { Hit(.5, 30, 1) };
        Check(Judge(sv, weak, 1, CleanStore{}, now, true) == Why::Kept, "one stray hit does not replace a prediction"); }
    {   Shape v; v.source = Source::Vehicle; v.centre = { 0, 10, 101 };
        Check(Judge(v, nullptr, 0, clean, now, true) == Why::Kept, "vehicles are always live: never dropped");
        Shape r = Hit(0, 10, 3); Check(Judge(r, nullptr, 0, clean, now, true) == Why::Kept, "rays are never dropped by trust");
        Check(Of(r, now) == Level::ConfirmedRay && Of(Pred(Source::Box, 0, 0), now) == Level::Box && Of(v, now) == Level::Live, "trust order by source"); }
    {   CleanStore c; const float a[3] = { 5, 2, 105 }, b[3] = { 1, 2, 101 };
        c.Add(a, b, now);
        Check(c.Near(3, 2.59, 102.8, 103.2, now), "FI-3: reversed segment within radius and height still clears");
        Check(!c.Near(3, 2.61, 102.8, 103.2, now) && !c.Near(3, 2.59, 99, 101, now),
            "FI-3: outside radius or height still leaves the prediction standing"); }
    std::printf("worldtrust_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
