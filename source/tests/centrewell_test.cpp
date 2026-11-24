// CW-1 step 1: the centre-line well solver and side choice (src/centrewell.h). notes/centreline-well-20261122/PLAN.md section 7.1.
#include "centrewell.h"
#include <cmath>
#include <cstdio>
#include <string_view>

namespace {
int failures = 0;
void Check(bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
using namespace centrewell;

Problem Road(int n = 60, double lane = 3.5) { Problem p; p.Reset(n, -lane, lane); p.a = 1.0; p.c = 20.0; return p; }
Result WithObstacles(Problem p, std::vector<Obstacle> obs, std::vector<Choice>* chosen = nullptr) {
    const auto sides = Choose(p, obs); ApplySides(p, obs, sides); if (chosen) *chosen = sides; return Solve(p);
}
bool WithinBounds(const Problem& p, const Result& r) { for (int i = 0; i < r.n; ++i) if (r.y[i] < p.lo[i] - 1e-6 || r.y[i] > p.hi[i] + 1e-6) return false; return true; }
}

void Show(const char* t, const Result& r) { std::printf("%s ok=%d n=%d rounds=%d why=%s\n", t, int(r.ok), r.n, r.rounds, r.why); for (int i = 0; i < r.n; i += 3) std::printf("%d:%.3f ", i, r.y[i]); std::printf("\n"); }

int main(int argc, char**) {
    if (argc > 1) {  // --show: print the three shaped cases
        { Problem q = Road(); std::vector<Obstacle> o = { { 1, 20, 24, -1.5, 1.5 } }; auto s = Choose(q, o); ApplySides(q, o, s); Show("box", Solve(q)); }
        { Problem q = Road(); q.y0 = 2; Show("off", Solve(q)); }
        { Problem q = Road(); for (int i = 28; i <= 32; ++i) q.Soft(i, +1, 1.0, 500.0); Show("soft", Solve(q)); }
        return 0;
    }
    {   // clear road: the centre line
        const auto r = Solve(Road());
        Check(r.ok && r.n == 60 && r.maxOffset < 1e-9, "clear road drives the centre line");
        Check(std::string_view(r.why) == "centre_line", "clear road reason");
    }
    {   // one answer for one input
        Problem p = Road(); p.y0 = 1.2; p.slope0 = -.05; p.Bound(25, 0.8, 3.5);
        const auto a = Solve(p), b = Solve(p); bool same = a.n == b.n;
        for (int i = 0; i < a.n && same; ++i) same = a.y[i] == b.y[i];
        Check(same, "same input, same path");
    }
    {   // a box on the line: pass on the preferred side by just enough, then back to the centre
        std::vector<Choice> s;
        const Problem p = Road();
        Problem q = p; const auto r = WithObstacles(p, { { 1, 20, 24, -1.5, 1.5 } }, &s);
        q = p; ApplySides(q, { { 1, 20, 24, -1.5, 1.5 } }, s);
        Check(s.size() == 1 && s[0].side == rules::kPreferredSide && s[0].fits, "tie: preferred side");
        Check(r.ok && r.blockAt < 0, "box on the line: a path exists");
        Check(WithinBounds(q, r), "box on the line: never inside it");
        Check(r.maxOffset >= 1.5 - 1e-6 && r.maxOffset < 1.7, "box on the line: offset just enough");
        bool oneSide = true; for (int i = 0; i < r.n; ++i) if (r.y[i] < -1e-6) oneSide = false;
        Check(oneSide, "box on the line: no swing to the other side");
        Check(std::fabs(r.y[59]) < 0.3, "box on the line: back to the centre after it");
        Check(std::fabs(r.y[0]) < 1e-12, "box on the line: starts where the car is");
    }
    {   // a box off to the left that leaves the centre free: no movement at all
        const auto r = WithObstacles(Road(), { { 2, 20, 24, -3.0, -0.5 } });
        Check(r.ok && r.maxOffset < 1e-9, "box beside the line: stays on the centre line");
    }
    {   // the whole road blocked: the plan ends at the block point
        const auto r = WithObstacles(Road(), { { 3, 20, 24, -3.6, 3.6 } });
        Check(r.blockAt == 20 && r.n == 20, "road blocked: block point at the obstacle");
        Check(std::string_view(r.why) == "plan_ends_at_a_block", "road blocked reason");
    }
    {   // more room on the left
        std::vector<Choice> s; const auto r = WithObstacles(Road(), { { 4, 20, 24, -1.0, 2.5 } }, &s);
        Check(s[0].side == -1, "less offset needed on the left: goes left");
        bool ok = true; for (int i = 20; i <= 24; ++i) if (r.y[i] > -1.0 + 1e-6) ok = false;
        Check(ok, "goes left of the box");
    }
    {   // kept side wins a tie, and gives way only when it no longer fits
        std::vector<Choice> s;
        WithObstacles(Road(), { { 5, 20, 24, -1.5, 1.5, -1 } }, &s);
        Check(s[0].side == -1 && std::string_view(s[0].why) == "kept_until_passed", "kept side is kept");
        Problem tight = Road(); for (int i = 0; i < 60; ++i) tight.lo[i] = -1.0;  // the left closed
        WithObstacles(tight, { { 5, 20, 24, -1.5, 1.5, -1 } }, &s);
        Check(s[0].side == +1, "kept side that no longer fits is dropped");
    }
    {   // start off the line: a smooth rejoin, no overshoot worth the name
        Problem p = Road(); p.y0 = 2.0;
        const auto r = Solve(p);
        double minY = 1e9; for (int i = 0; i < r.n; ++i) minY = std::min(minY, r.y[i]);
        Check(r.ok && std::fabs(r.y[59]) < 0.3, "off the line: rejoins");
        Check(minY > -1e-6, "off the line: never crosses to the other side (critical damping)");
        bool down = true; for (int i = 2; i < 30; ++i) if (r.y[i] > r.y[i - 1] + 1e-9) down = false;
        Check(down, "off the line: comes back steadily");
    }
    {   // soft push: partial for a weak one, nearly all the way for a strong one
        Problem weak = Road(); for (int i = 28; i <= 32; ++i) weak.Soft(i, +1, 1.0, 0.5);
        Problem strong = Road(); for (int i = 28; i <= 32; ++i) strong.Soft(i, +1, 1.0, 500.0);
        const auto rw = Solve(weak), rs = Solve(strong);
        Check(rw.ok && rw.y[30] > 0.01 && rw.y[30] < 0.6, "weak soft push moves it a little");
        Check(rs.ok && rs.y[30] > 0.9 && rs.y[30] < 1.1, "strong soft push moves it close to its edge");
    }
    {   // two staggered boxes: a slalom only because each one needs little offset on alternate sides
        std::vector<Choice> s;
        WithObstacles(Road(), { { 6, 15, 18, -1.5, 0.5 }, { 7, 30, 33, -0.5, 1.5 } }, &s);
        Check(s.size() == 2 && s[0].side == +1 && s[1].side == -1, "staggered boxes: right then left");
        // the same two boxes each needing a lot on the alternate side: one side for both
        WithObstacles(Road(), { { 8, 15, 18, -1.5, 0.3 }, { 9, 20, 23, -0.3, 1.5 } }, &s);
        Check(s.size() == 2, "two boxes: two choices");
    }
    {   // memory: kept until passMargin past the end
        Memory m; const std::vector<Obstacle> obs = { { 10, -1.0, 3.0, -1, 1 } }; const std::vector<Choice> ch = { { -1, 1, true, false, "" } };
        m.Update(obs, ch); Check(m.Side(10) == -1, "memory keeps a side");
        const std::vector<Obstacle> passed = { { 10, -9.0, -2.5, -1, 1 } };
        m.Update(passed, ch); Check(m.Side(10) == 0, "memory drops it once passed");
    }
    {   // hard bounds hold exactly through the active set
        Problem p = Road(); for (int i = 10; i < 20; ++i) p.Bound(i, 1.0, 3.5); for (int i = 30; i < 35; ++i) p.Bound(i, -3.5, -0.5);
        const auto r = Solve(p);
        Check(r.ok && WithinBounds(p, r), "alternating hard bounds are met");
        Check(r.rounds < rules::kActiveSetRounds, "active set settles");
    }
    {   // level with the car: never "passed" on the far side (that is through it); inside its margin: keeps its distance, no block
        Problem p = Road(); p.y0 = 0.5;
        std::vector<Choice> s; const auto r = WithObstacles(p, { { 11, -3, 10, -1.0, 0.2 } }, &s);  // beside, on the car's left, car outside it
        Check(s[0].side == +1, "beside: passed on the car's side");
        Check(r.ok && r.blockAt < 0, "beside: no block");
        Problem q = Road(); q.y0 = 0.5;
        const auto r2 = WithObstacles(q, { { 12, -3, 10, -1.0, 1.0 } }, &s);  // the car inside its margin
        Check(s[0].touching && s[0].side == +1, "inside the margin: keeps its side");
        Check(r2.ok && r2.blockAt < 0, "inside the margin: no block");
        bool neverCloser = true; for (int i = 0; i < r2.n; ++i) if (r2.y[i] < 0.5 - 1e-6 && i * 1.0 <= 10) neverCloser = false;
        Check(neverCloser, "inside the margin: never closer than it was");
    }
    {   // floor bounds: a drop right of 1.0 m from station 20 to 40 narrows the road there; a hole on the line at 30 m ends the plan
        Problem p = Road();
        const int nar = FloorBounds(p, [](double s, double y) { return !(s >= 20 && s <= 40 && y > 1.0); });
        Check(nar > 0, "floor: a drop narrows stations");
        Check(p.hi[30] <= 1.0 + 1e-9 && p.hi[10] > 3.0, "floor: only where the drop is");
        const auto r = Solve(p); bool inside = true; for (int i = 20; i <= 40; ++i) if (r.y[i] > 1.0 + 1e-6) inside = false;
        Check(r.ok && inside, "floor: the path keeps off the drop");
        Problem h = Road();
        FloorBounds(h, [](double s, double y) { return !(s >= 30 && s <= 31 && std::fabs(y) < 4.0); });
        const auto rh = Solve(h);
        Check(rh.blockAt >= 29 && rh.blockAt <= 31, "floor: a hole across the road is a block point");
        Problem u = Road(); FloorBounds(u, [](double, double) { return true; });
        Check(Solve(u).maxOffset < 1e-9, "floor: unknown or flat floor changes nothing");
    }
    std::printf("centrewell_test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
