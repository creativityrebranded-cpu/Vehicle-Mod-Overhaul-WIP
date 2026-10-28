#include "trafficroute.h"
#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }

using namespace trafficroute;

// Flies the trip with a perfect pilot (moves up to `speed` m per step toward the goal) until done or out of steps.
static int Fly(const Line& l, Trip& t, Vec& p, double speed, int steps)
{
    for (int i = 0; i < steps; ++i) {
        if (t.leg == Leg::Done) return i;
        const Goal g = Step(l, t, p.x, p.y, p.z);
        const double dx = g.x - p.x, dy = g.y - p.y, dz = g.z - p.z, d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double k = d > speed ? speed / d : 1.0;
        p.x += dx * k; p.y += dy * k; p.z += dz * k;
    }
    return t.leg == Leg::Done ? steps : -1;
}

int main()
{
    trafficdata::Route ring;
    ring.shape = ringlogic::Clean({ 0, 0, 300, 600, 1, 0, 40 });
    const Line rl = Build(ring);
    {
        // The user's case: parked 300 m under the lane. The climb must reach the lane, then a full lap, then home.
        Vec p{ 50, 40, 0 };
        Trip t = StartLap(rl, 0, p);
        const int n = Fly(rl, t, p, 10.0, 5000);
        Check(n > 0, "a lap from 300 m below finishes");
        Check(std::hypot(p.x - 50, p.y - 40) < kReach + 1, "the lap ends over home");
    }
    {
        // To a destination east of centre: leave at the exit there; the lane chosen is the shorter way.
        const double exitU = Nearest(rl, 0, 600, 0);
        Vec home{ 0, -500, 0 }, dest{ 550, 10, 0 };
        Trip t = StartTo(rl, 2, home, dest, exitU);
        Vec p = home;
        const int n = Fly(rl, t, p, 10.0, 5000);
        Check(n > 0 && std::hypot(p.x - dest.x, p.y - dest.y) < kReach + 1, "a trip lands at its destination");
        // From the south (u = 48) to the east (u = 24): counter-clockwise is a quarter, clockwise three quarters.
        Check(t.lane == 1, "the shorter way round is taken");
    }
    {
        trafficdata::Route path;
        path.ring = false; path.lanes = 2; path.width = 20;
        path.points = { { 0, 0, 100 }, { 400, 0, 100 }, { 800, 0, 100 } };
        const Line pl = Build(path);
        Check(!pl.closed && pl.Count() == 3, "a path is an open line");
        Check(At(pl, 0, 0).y < 0 && At(pl, 1, 0).y > 0, "path lanes sit either side (lane 0 on the right going +x)");
        Vec home{ 100, -30, 0 }, dest{ 790, 40, 0 };
        Trip t = StartTo(pl, 2, home, dest, Nearest(pl, 0, 800, 0));
        Check(t.lane == 0, "a path's lane goes the way of the exit");
        Vec p = home;
        Check(Fly(pl, t, p, 10.0, 5000) > 0 && std::hypot(p.x - dest.x, p.y - dest.y) < kReach + 1, "a path trip lands at its destination");
        Check(std::isinf(Ahead(pl, 0, 2.0, 0.0)), "an open lane never runs backwards");
    }
    {
        // A closed path (a figure eight): laps round and comes home.
        trafficdata::Route eight;
        eight.ring = false; eight.closed = true; eight.lanes = 2; eight.width = 20;
        for (int i = 0; i < 24; ++i) {
            const double t = 2.0 * ringlogic::kPi * i / 24;
            eight.points.push_back({ 600 * std::sin(t), 600 * std::sin(t) * std::cos(t), 300 });
        }
        const Line el = Build(eight);
        Check(el.closed && el.Count() == 24, "a closed path loops");
        Vec p{ 300, 200, 0 };
        Trip t = StartLap(el, 0, p);
        Check(Fly(el, t, p, 10.0, 8000) > 0 && std::hypot(p.x - 300, p.y - 200) < kReach + 1, "a figure-eight lap comes home");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
