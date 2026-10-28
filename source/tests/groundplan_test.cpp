// groundplan.h: the ground-first planner, its speed profile and its path follower (2026-10-25).
#include "../src/groundplan.h"
#include <cstdio>
#include <cstdlib>

using namespace groundplan;
static int g_checks = 0, g_fail = 0;
static void Check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } else std::printf("PASS %s\n", what); }

// A 161-cell (644 m) flat, mapped grid centred on the origin.
static Grid Flat(int n = 161) {
    Grid g; g.Resize(-n / 2, -n / 2, n);
    for (auto& c : g.cells) { c.ground = 0; c.nz = 1; }
    return g;
}
static void Band(Grid& g, double y0, double y1, double x0, double x1, void (*mark)(Cell&)) {
    for (int j = 0; j < g.n; ++j) for (int i = 0; i < g.n; ++i)
        if (g.Y(j) >= y0 && g.Y(j) <= y1 && g.X(i) >= x0 && g.X(i) <= x1) mark(g.At(i, j));
}
static double Length(const Plan& p) { double l = 0; for (std::size_t k = 1; k < p.pts.size(); ++k) l += std::hypot(p.pts[k].x - p.pts[k - 1].x, p.pts[k].y - p.pts[k - 1].y); return l; }

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Limits lim;
    // 1. Open, mapped ground: a straight drive, nothing in the air.
    {
        auto g = Flat();
        auto p = Search(g, lim, 0, -200, 0, Mode::Ground, 0, 200, std::numeric_limits<double>::quiet_NaN());
        Check(p.ok && p.reachesGoal, "open ground: the goal is reached");
        Check(p.airMetres == 0 && p.modeChanges == 0, "open ground: no flying");
        Check(Length(p) < 410 && p.pts.size() <= 4, "open ground: string pulling gives a straight line");
    }
    // 2. A lake with a way round 60 m to the side: drive round it.
    {
        auto g = Flat();
        Band(g, -20, 20, -60, 400, [](Cell& c) { c.water = true; });
        auto p = Search(g, lim, 0, -150, 0, Mode::Ground, 0, 150, std::numeric_limits<double>::quiet_NaN());
        Check(p.ok && p.reachesGoal && p.airMetres == 0, "short way round a lake: driven, not flown");
        bool wet = false;
        for (std::size_t k = 1; k < p.pts.size(); ++k) for (int s = 0; s <= 20; ++s) {
            const double t = s / 20.0, x = p.pts[k - 1].x + (p.pts[k].x - p.pts[k - 1].x) * t, y = p.pts[k - 1].y + (p.pts[k].y - p.pts[k - 1].y) * t;
            const int i = Grid::Index(x) - g.ox, j = Grid::Index(y) - g.oy; if (g.Inside(i, j) && g.At(i, j).water) wet = true; }
        Check(!wet, "short way round a lake: the route never crosses water");
    }
    // 3. A lake across the whole map: fly over it (take off, land), only the water flown.
    {
        auto g = Flat();
        Band(g, -20, 20, -1000, 1000, [](Cell& c) { c.water = true; });
        auto p = Search(g, lim, 0, -150, 0, Mode::Ground, 0, 150, std::numeric_limits<double>::quiet_NaN());
        Check(p.ok && p.reachesGoal && p.modeChanges == 2, "lake across the map: one take-off, one landing");
        Check(p.airMetres > 30 && p.airMetres < 80 && p.groundMetres > 200, "lake across the map: only the water is flown");
        // 4. The same lake for a vehicle that cannot fly: a partial route up to the shore.
        Limits noFly = lim; noFly.canFly = false;
        auto q = Search(g, noFly, 0, -150, 0, Mode::Ground, 0, 150, std::numeric_limits<double>::quiet_NaN());
        Check(q.ok && !q.reachesGoal && q.airMetres == 0, "cannot fly: partial route, never in the air");
        Check(!q.pts.empty() && q.pts.back().y < -20 && q.pts.back().y > -40, "cannot fly: the partial route ends at the near shore");
    }
    // 5. A wall with a 12 m gap: through the gap; a gap narrower than the hull is refused.
    {
        auto g = Flat();
        Band(g, -2, 2, -1000, -6, [](Cell& c) { c.solidLow = true; c.solidHigh = true; });
        Band(g, -2, 2, 6, 1000, [](Cell& c) { c.solidLow = true; c.solidHigh = true; });
        auto p = Search(g, lim, 30, -100, 0, Mode::Ground, 30, 100, std::numeric_limits<double>::quiet_NaN());
        Check(p.ok && p.reachesGoal && p.airMetres == 0, "wall with a gap: driven through the gap");
        Limits wide = lim; wide.halfWidth = 7; wide.canFly = false;
        auto q = Search(g, wide, 30, -100, 0, Mode::Ground, 30, 100, std::numeric_limits<double>::quiet_NaN());
        Check(!q.reachesGoal, "wall with a gap: a hull wider than the gap does not fit");
    }
    // 6. A cliff (10 m step): no driving down it; with flight, flown only across the step.
    {
        auto g = Flat();
        Band(g, 0, 1000, -1000, 1000, [](Cell& c) { c.ground = -10; });
        Limits noFly = lim; noFly.canFly = false;
        auto q = Search(g, noFly, 0, -100, 0, Mode::Ground, 0, 100, std::numeric_limits<double>::quiet_NaN());
        Check(!q.reachesGoal, "cliff: no ground way down a 10 m step");
        auto p = Search(g, lim, 0, -100, 0, Mode::Ground, 0, 100, std::numeric_limits<double>::quiet_NaN());
        Check(p.reachesGoal && p.modeChanges == 2 && p.airMetres < 30, "cliff: a short hop over the step, then driving again");
    }
    // 7. Ground steeper than the vehicle's limit is not driven; a ramp gentle enough is.
    {
        auto g = Flat();
        Band(g, -30, 30, -1000, 1000, [](Cell& c) { c.nz = 0.6f; });
        Limits noFly = lim; noFly.canFly = false;
        Check(!Search(g, noFly, 0, -100, 0, Mode::Ground, 0, 100, std::numeric_limits<double>::quiet_NaN()).reachesGoal, "slope steeper than the limit: refused");
        Band(g, -30, 30, -1000, 1000, [](Cell& c) { c.nz = 0.9f; });
        Check(Search(g, noFly, 0, -100, 0, Mode::Ground, 0, 100, std::numeric_limits<double>::quiet_NaN()).reachesGoal, "gentle slope: driven");
    }
    // 8. Two routes round a block, the longer one on a road: the road is taken while it is not much longer.
    {
        auto g = Flat();
        Band(g, -40, 40, -40, 40, [](Cell& c) { c.solidLow = true; });
        Band(g, -120, 120, 44, 56, [](Cell& c) { c.road = true; });
        auto p = Search(g, lim, 50, -120, 0, Mode::Ground, 50, 120, std::numeric_limits<double>::quiet_NaN());
        bool onRoad = true;
        for (const auto& w : p.pts) if (std::fabs(w.y) < 40 && (w.x < 44 || w.x > 56)) onRoad = false;
        Check(p.reachesGoal && onRoad, "road beside a block: the route keeps to the road");
    }
    // 9. A goal 30 m up (a platform): the route ends in the air; with no flight it stops below.
    {
        auto g = Flat();
        auto p = Search(g, lim, 0, -100, 0, Mode::Ground, 0, 100, 30);
        Check(p.reachesGoal && p.pts.back().mode == Mode::Air && p.groundMetres > p.airMetres, "high goal: drive most of the way, fly up at the end");
    }
    // 10. Unmapped ground is crossed, but mapped ground is preferred when the detour is small.
    {
        auto g = Flat();
        Band(g, -30, 30, -20, 1000, [](Cell& c) { c.ground = std::numeric_limits<float>::quiet_NaN(); c.nz = std::numeric_limits<float>::quiet_NaN(); });
        auto p = Search(g, lim, 0, -100, 0, Mode::Ground, 0, 100, std::numeric_limits<double>::quiet_NaN());
        Check(p.reachesGoal && p.airMetres == 0, "unmapped band: still driven (not flown)");
        Check(p.unknownMetres < 20, "unmapped band: a small detour onto mapped ground is preferred");
    }
    // 11. A goal beyond the map: a partial route that heads its way.
    {
        auto g = Flat();
        auto p = Search(g, lim, 0, 0, 0, Mode::Ground, 2000, 0, std::numeric_limits<double>::quiet_NaN());
        Check(p.ok && !p.reachesGoal && !p.pts.empty() && p.pts.back().x > 250, "goal beyond the map: a partial route toward it");
    }
    // 12. Speed profile: slower through a sharp corner, stopped at the end, capped on unmapped ground.
    {
        Plan p; p.ok = true;
        p.pts = { {0, 0, 0}, {0, 200, 0}, {200, 200, 0}, {200, 400, 0} };
        SpeedProfile(p, lim, 10, true);
        Check(p.pts[1].speed < lim.cruise && p.pts[1].speed > 3, "speed: a 90 degree corner is taken slower");
        Check(p.pts.back().speed == 0, "speed: stopped at the end");
        Plan u; u.ok = true; u.pts = { {0, 0, 0}, {0, 300, 0}, {0, 600, 0} }; u.pts[1].unknown = true;
        SpeedProfile(u, lim, 20, false);
        Check(u.pts[1].speed <= lim.unknownSpeed + 1e-9, "speed: capped over unmapped ground");
        Plan m; m.ok = true; m.pts = { {0, 0, 0}, {0, 300, 0}, {0, 300, 6, Mode::Air}, {0, 600, 6, Mode::Air} };
        SpeedProfile(m, lim, 20, false);
        Check(m.pts[1].speed <= lim.modeChangeSpeed + 1e-9, "speed: slow at a take-off");
    }
    // 13. Follow: steers along the route, sees the next mode change, notices being off it, slows for what is ahead.
    {
        Plan p; p.ok = true;
        p.pts = { {0, 0, 0}, {0, 100, 0}, {0, 100, 6, Mode::Air}, {0, 200, 6, Mode::Air} };
        SpeedProfile(p, lim, 0, true);
        std::size_t cursor = 0;
        auto gd = Follow(p, 1, 20, 10, cursor, lim);
        Check(gd.ok && gd.mode == Mode::Ground && std::fabs(gd.bearing) < 0.1, "follow: steers up the route");
        Check(gd.nextMode == Mode::Air && std::fabs(gd.toModeChange - 80) < 1.5, "follow: the take-off is 80 m ahead");
        Check(gd.speed < lim.cruise, "follow: slowing for the take-off ahead");
        auto off = Follow(p, 30, 20, 10, cursor, lim);
        Check(off.offPath, "follow: 30 m to the side is off the route");
        std::size_t c2 = 0;
        auto air = Follow(p, 0, 150, 10, c2, lim);
        Check(air.mode == Mode::Air && air.tz > 5, "follow: on the air leg, steering at its height");
        auto end = Follow(p, 0, 199.5, 0, c2, lim);
        Check(end.done, "follow: done at the end");
    }
    // 14. Starting in the air over water: lands on the far shore and drives on.
    {
        auto g = Flat();
        Band(g, -60, 20, -1000, 1000, [](Cell& c) { c.water = true; });
        auto p = Search(g, lim, 0, 0, 6, Mode::Air, 0, 150, std::numeric_limits<double>::quiet_NaN());
        Check(p.reachesGoal && p.pts.front().mode == Mode::Air && p.pts.back().mode == Mode::Ground, "airborne over water: lands on the shore and drives on");
    }
    std::printf("groundplan: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
