#include "ringtrip.h"
#include <cstdio>

using namespace ringtrip;
static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }

// Flies a point toward each goal at `speed` (straight line, climbs at 5 m/s) and counts the legs.
static Leg Fly(const ringlogic::Ring& r, Trip& t, double& x, double& y, double& z, double seconds, double& travelledAtExit, bool& sawCircle)
{
    const double dt = 0.1, speed = 25.0;
    for (double s = 0; s < seconds && t.leg != Leg::Done; s += dt) {
        const Leg before = t.leg;
        const Goal g = Step(r, t, x, y, z);
        if (t.leg == Leg::Circle) sawCircle = true;
        if (before == Leg::Circle && t.leg == Leg::Exit) travelledAtExit = t.travelled;
        const double dx = g.x - x, dy = g.y - y, d = std::hypot(dx, dy);
        if (d > 1e-6) { const double m = std::min(d, speed * dt); x += dx / d * m; y += dy / d * m; }
        z += std::clamp(g.z - z, -5.0 * dt, 5.0 * dt);
    }
    return t.leg;
}

int main()
{
    ringlogic::Ring r; r.cx = -858.7; r.cy = 1567.4; r.z = 143.5; r.radius = 600; r.aspect = 1.0; r.rotation = 0; r.gap = 40;
    {   // The angle inverse matches ringlogic::At (with rotation and aspect).
        ringlogic::Ring e = r; e.aspect = 1.6; e.rotation = 0.7;
        bool ok = true;
        for (double a = -3.0; a < 3.0; a += 0.37) { const auto p = ringlogic::At(e, a); ok = ok && std::fabs(ringlogic::Wrap(AngleOf(e, p.x, p.y) - a)) < 1e-6; }
        Check(ok, "lane angle inverse matches the ring geometry");
    }
    for (int layer = 0; layer < 2; ++layer) {  // A full trip from a parking spot inside the ring, on each lane.
        double x = r.cx + 200, y = r.cy - 150, z = 2.0, atExit = 0;
        bool circled = false;
        Trip t = Start(layer, x, y);
        const Leg end = Fly(r, t, x, y, z, 600, atExit, circled);
        char msg[96];
        std::snprintf(msg, sizeof msg, "layer %d: climbs, joins, laps once, exits and is home (lap %.2f rad)", layer, atExit);
        Check(end == Leg::Done && circled && atExit >= 2 * ringlogic::kPi && std::hypot(x - t.homeX, y - t.homeY) < kReach, msg);
        const double laneZ = r.z + (layer ? r.gap : 0);
        Check(std::fabs(z - laneZ) < 6.0, "flies at its own lane's height");
    }
    {   // Climbs before moving across: the first goal is straight above home.
        Trip t = Start(0, -500, 1400);
        const Goal g = Step(r, t, -500, 1400, 2.0);
        Check(t.leg == Leg::Climb && g.x == -500 && g.y == 1400 && g.z == r.z, "first leg is straight up over home");
    }
    {   // Direction: layer 0 leads with a growing angle, layer 1 with a shrinking one.
        Trip a = Start(0, 0, 0), b = Start(1, 0, 0);
        const auto p = ringlogic::At(r, 1.0);
        a.leg = b.leg = Leg::Circle;
        const Goal ga = Step(r, a, p.x, p.y, r.z), gb = Step(r, b, p.x, p.y, r.z + r.gap);
        Check(ringlogic::Wrap(AngleOf(r, ga.x, ga.y) - 1.0) > 0 && ringlogic::Wrap(AngleOf(r, gb.x, gb.y) - 1.0) < 0, "lanes run opposite ways");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
