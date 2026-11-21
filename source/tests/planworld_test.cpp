#include "planworld.h"
#include <cstdio>
using namespace planworld;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

static Shape Hit0(double x, double y, double z, double nz) { Shape s; s.centre = { x, y, z }; s.haveNormal = true; s.normalZ = nz; return s; }

int main() {
    Wanted deck; deck.have = true; deck.z = 100;
    const double base = 100, height = 2;
    Check(Judge(Hit0(0, 0, 100, 1), deck, base, height) == Verdict::Free, "the deck the car wants is not an obstacle");
    Check(Judge(Hit0(0, 0, 101, .9), deck, base, height) == Verdict::Free, "a ramp rising within a step is the road going on");
    Check(Judge(Hit0(0, 0, 101.3, 0), deck, base, height) == Verdict::Tall, "a railing face over the deck is a wall");
    Check(Judge(Hit0(0, 0, 100.6, 0), deck, base, height) == Verdict::Low, "a kerb-height face can be hopped");
    Check(Judge(Hit0(0, 0, 100.2, 0), deck, base, height) == Verdict::Free, "a face under the face step is nothing");
    Check(Judge(Hit0(0, 0, 90, 1), deck, base, height) == Verdict::Free, "the terrain under the deck is not touched");
    Check(Judge(Hit0(0, 0, 110, -1), deck, base, height) == Verdict::Free, "a roof above the car is not touched");
    Check(Judge(Hit0(0, 0, 101.5, -1), deck, base, height) == Verdict::Tall, "a low overhang facing down is not a floor");
    {   Wanted none; Check(Judge(Hit0(0, 0, 101, 0), none, base, height) == Verdict::Tall, "with no wanted surface a solid in the car's band blocks"); }

    {   // Compare the old inner test to reused inputs, including moving and tracked shapes.
        bool equal = true;
        for (int i = 0; i < 25; ++i) for (int j = 0; j < 25; ++j) {
            Pose p; p.x = i * .13; p.y = j * .19; p.heading = i * .37; p.base = 100; p.wanted = deck;
            Car c; Shape s = Hit0(j * .14, i * .12, 99.7 + j * .15, (j % 3) - 1.0);
            s.heading = j * .27; s.halfHeight = (i % 3) * .22;
            s.halfLength = .2 + i * .12; s.velocity = { .15, -.4, j * .018 };
            if (i % 2) { s.trackCount = 2; s.trackT[1] = 3; s.track[0] = s.centre; s.track[1] = { 2, 2, 102 }; }
            for (double t = 0; t <= 4; t += .25) {
                const auto at = At(s, t);
                const double as = std::sin(p.heading), ac = std::cos(p.heading), bs = std::sin(s.heading), bc = std::cos(s.heading);
                const double axes[4][2] = {{as,ac},{ac,-as},{bs,bc},{bc,-bs}};
                const double dx = at.x - p.x, dy = at.y - p.y, margin = c.gap + Margin(s, t);
                bool overlap = true;
                for (const auto& n : axes) {
                    const double dist = std::fabs(dx * n[0] + dy * n[1]);
                    const double a = c.halfLength * std::fabs(as*n[0]+ac*n[1]) + c.halfWidth * std::fabs(ac*n[0]-as*n[1]);
                    const double b = s.halfLength * std::fabs(bs*n[0]+bc*n[1]) + s.halfWidth * std::fabs(bc*n[0]-bs*n[1]);
                    if (dist > a + b + margin) { overlap = false; break; }
                }
                Shape moved = s; moved.centre = at;
                const auto expected = overlap ? Judge(moved, p.wanted, p.base, c.height) : Verdict::Free;
                equal &= Hit(p, HeadingAxes::Of(p.heading), c, s, HeadingAxes::Of(s.heading), Project(s, t)) == expected;
            }
        }
        Check(equal, "Same Work 1: 10,625 cached collision checks match the prior separating-axes calculation");
    }

    Car car; Pose p; p.base = 100; p.wanted = deck;
    {   Shape wall = Hit0(0, 0, 101.5, 0); wall.halfWidth = 1; wall.halfLength = 1; wall.halfHeight = 1.5;
        Check(Hit(p, car, wall, 0) == Verdict::Tall, "the car's box meets a wall");
        wall.centre.x = 10; Check(Hit(p, car, wall, 0) == Verdict::Free, "a wall beside the car is missed"); }
    {   // A long thin box turned 90 degrees: only an oriented test gets this right.
        Shape bar; bar.centre = { 4, 0, 101 }; bar.halfWidth = .2; bar.halfLength = 3; bar.halfHeight = 1;
        Check(Hit(p, car, bar, 0) == Verdict::Free, "a thin bar along the car's side is missed");
        bar.heading = 1.5707963; Check(Hit(p, car, bar, 0) == Verdict::Tall, "the same bar turned across hits"); }
    {   Shape v; v.source = Source::Vehicle; v.centre = { 30, 0, 101 }; v.velocity = { -10, 0, 0 }; v.halfWidth = 1; v.halfLength = 2; v.halfHeight = 1;
        Check(Hit(p, car, v, 0) == Verdict::Free, "a crossing car is clear now");
        Check(Hit(p, car, v, 3) == Verdict::Tall, "and here in 3 s");
        Check(Hit(p, car, v, 6) == Verdict::Free, "and gone in 6 s");
        Check(Margin(v, 2) > Margin(v, 0), "a guess gets less sure with time"); }
    {   Shape v; v.source = Source::Vehicle; v.halfWidth = 1; v.halfLength = 2; v.halfHeight = 1; v.velocity = { 10, 0, 0 };
        v.trackCount = 3; v.trackT[0] = 0; v.trackT[1] = 2; v.trackT[2] = 4; v.track[0] = { 0, 40, 101 }; v.track[1] = { 0, 20, 101 }; v.track[2] = { 0, 0, 101 };
        const V3 a = At(v, 3); Check(std::fabs(a.y - 10) < 1e-9 && std::fabs(a.x) < 1e-9, "a published plan is used, not the velocity");
        Check(Margin(v, 4) == 0, "a published plan is firm");
        Check(Hit(p, car, v, 4) == Verdict::Tall, "so a car turning into the path is seen"); }
    {   Shape man; man.source = Source::Person; Check(Margin(man, 0) >= kPersonMargin, "people get a wider margin"); }
    {   Shape r; r.source = Source::Ray; r.centre = { 0, 0, 101.9 }; r.haveNormal = true; r.normalZ = .86; Wanted w; w.have = true; w.z = 100; w.step = 1.1;
        Check(Judge(r, w, 100, 2) != Verdict::Free, "a surface far over a measured floor is not the road");
        w.measured = false; Check(Judge(r, w, 100, 2) == Verdict::Free, "with no measured floor, a surface facing up is the road (a ramp), not a wall"); }
    {   Shape box; box.source = Source::Box; box.centre = { 0, 0, 101 }; box.halfWidth = 5; box.halfLength = 5; box.halfHeight = 2;
        Check(Contradicted(box, p, car, false), "a box the car stands inside with nothing touching is a poor fit");
        Check(!Contradicted(box, p, car, true), "touching something believes it");
        box.source = Source::Vehicle; Check(!Contradicted(box, p, car, false), "a vehicle is never explained away"); }
    {   // The per-frame store: one list for every reader.
        Store st; st.Begin(7, 1.0); Shape a; a.owner = 1; a.centre = { 0, 0, 0 }; Shape b; b.owner = 2; b.centre = { 50, 0, 0 }; Shape c; c.owner = 3; c.centre = { 5, 5, 0 };
        st.Add(a); st.Add(b); st.Add(c); Shape out[4];
        Check(st.Near(0, 0, 20, 0, out, 4) == 2, "the store answers what is near");
        Check(st.Near(0, 0, 20, 1, out, 4) == 1 && out[0].owner == 3, "and leaves out the asking car's own body");
        Check(st.Near(0, 0, 100, 0, out, 2) == 2, "and never writes past the room given");
        st.Begin(8, 2.0); Check(st.Near(0, 0, 100, 0, out, 4) == 0 && st.frame == 8, "a new frame starts empty");
        StillClock k; k.Step(0, 5); k.Step(1, 0); Check(k.Step(21.5, 0) > 20 && k.Step(22, 3) == 0, "D1's clock counts stillness and resets on moving");
        Track2 t; t.Step(0, 0, 0); t.Step(1, 2, 0); Check(std::fabs(t.vx - 2) < 1e-9 && t.vy == 0, "a person's velocity from frame to frame"); }
    {   Store st; st.Begin(9, 3.0);
        for (unsigned i = 1; i <= 40; ++i) {
            Shape shape; shape.owner = i;
            shape.centre.x = i % 2 ? 33 : -33;
            if (i > 5 && i != 40) shape.centre.y = 300;
            st.Add(shape);
        }
        Shape out[3];
        Check(st.Near(0, 0, 33, 2, out, 3) == 3 && out[0].owner == 1 && out[1].owner == 3 && out[2].owner == 4,
              "PT-1: indexed range includes cell edges and preserves source order and cap");
        Check(st.AnyNearby(0, 0, 33, [](const Shape& s) { return s.owner == 40; }),
              "PT-1: nearby safety search finds a shape beyond the planning cap");
        st.Begin(9, 4.0);
        Check(!st.AnyNearby(0, 0, 40, [](const Shape&) { return true; }),
              "PT-1: a new publication does not expose old bucket contents");
    }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
