// The S-bend of lc_ring_road (2026-11-20 08:00-08:03 stall, own car FF01E702, Cruise, lane 1 = reverse direction): route points 41-53
// with their roundings, a lane line 2.75 m right of the road centre, own lane [-2.75, +2.8] about the line, road 15.1 m, edge bands 2.0 m, so
// legal = [-8.3, 2.8] (the log's corridor right=2.79, slice road -10.0..4.5). Offline checks on the real planner (energyplan::Plan) with the
// scene rebuilt every tick as planinput::Scene does (a 100 m window of the lane line, band solids built before Index()). Not in-game proof.
//  - Scene::BuildKnownBandSolids: the band is the band: no spur inside a bend (inner side of a corner), no fold across a tight arc, no hole
//    on the outer side of a corner; a straight road builds the same rows as before
//  - energyplan::Score: a body within kNoGoTolerance of a known band alone is clear (planning must not stop for 3 cm of a line on a map)
//  - closed loop: the car passes the S-bend from the approach (8.5 m/s), from rest 30 m before it, and from the stuck pose (heading toward
//    the outer band, a few cm into it); it gets out in bounded time with only a few plan switches per second
#include "energyplan.h"
#include "planlattice.h"
#include "planscene.h"
#include "routecurve.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kPi = 3.14159265358979323846, kInf = std::numeric_limits<double>::infinity();

// lc_ring_road points 53..38 (file order 38..53 -> reversed: the lane 1 direction runs from 53 down to 38)
struct P { double x, y, r; };
static const P kPts[] = {  // driving order (lane 1 travels against the markers)
    { -82.47, 1566.22, 20 }, { -78.47, 1576.27, 0 }, { -70.92, 1619.0, 0 }, { -64.88, 1654.5, 0 }, { -58.85, 1690.0, 0 }, { -51.07, 1734.12, 0 },
    { -47.91, 1738.03, 20 }, { -46.9, 1741.86, 20 }, { -48.87, 1746.21, 20 }, { -54.16, 1748.73, 0 }, { -59.83, 1754.28, 0 }, { -62.01, 1755.75, 0 },
    { -69.49, 1758.5, 0 }, { -141.0, 1754.0, 20 }, { -170.3, 1741.41, 20 } };

struct Road {
    std::vector<planscene::RoutePoint> lane;  // dense lane line (0.25 m), station = arc length
    double Length() const { return lane.back().station; }
    planscene::RoutePoint At(double s) const {
        s = std::clamp(s, 0.0, Length());
        auto it = std::lower_bound(lane.begin(), lane.end(), s, [](const planscene::RoutePoint& p, double v) { return p.station < v; });
        if (it == lane.begin()) return lane.front();
        const auto& b = *it; const auto& a = *(it - 1); const double t = b.station > a.station ? (s - a.station) / (b.station - a.station) : 0;
        return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, s, 0 };
    }
    double Station(double x, double y) const {
        double best = 1e18, st = 0;
        for (const auto& p : lane) { const double d = (p.x - x) * (p.x - x) + (p.y - y) * (p.y - y); if (d < best) { best = d; st = p.station; } }
        return st;
    }
};
// The lane line: the rounded road centre, offset `off` to the right of travel at every vertex along the mitre (the road's own stations do the same).
static Road MakeRoad(const std::vector<P>& pts, double off) {
    std::vector<trafficdata::Vec> v; std::vector<double> rad;
    for (const auto& p : pts) { v.push_back({ p.x, p.y, 0 }); rad.push_back(p.r); }
    const auto c = routecurve::Build(v, rad, false).points;
    std::vector<planscene::RoutePoint> raw;
    for (std::size_t i = 0; i < c.size(); ++i) {
        const auto a = c[i > 0 ? i - 1 : i], b = c[i], d = c[i + 1 < c.size() ? i + 1 : i];
        double ax = b.x - a.x, ay = b.y - a.y, bx = d.x - b.x, by = d.y - b.y; const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
        if (la > 1e-9) { ax /= la; ay /= la; } else { ax = bx / lb; ay = by / lb; }
        if (lb > 1e-9) { bx /= lb; by /= lb; } else { bx = ax; by = ay; }
        double nx = ay + by, ny = -(ax + bx); const double nl = std::hypot(nx, ny); nx /= nl; ny /= nl;  // mitre normal, right of travel
        const double cosHalf = std::max(.3, nx * ay - ny * ax);  // normal . segment normal
        raw.push_back({ b.x + nx * off / cosHalf, b.y + ny * off / cosHalf, 0, 0 });
    }
    Road r; double st = 0;
    for (std::size_t i = 0; i + 1 < raw.size(); ++i) {
        const double len = std::hypot(raw[i + 1].x - raw[i].x, raw[i + 1].y - raw[i].y); if (len < 1e-6) continue;
        const int n = std::max(1, int(std::ceil(len / .25)));
        for (int k = 0; k < n; ++k) { const double f = double(k) / n; r.lane.push_back({ raw[i].x + (raw[i + 1].x - raw[i].x) * f, raw[i].y + (raw[i + 1].y - raw[i].y) * f, st + len * f, 0 }); }
        st += len;
    }
    r.lane.push_back({ raw.back().x, raw.back().y, st, 0 });
    return r;
}
// The scene the planner reads, built as planinput::Scene does: 100 m of the lane line at 1 m (i = -20..80), legal, band solids, Index.
static bool g_trace = false;
static bool g_asWalls = false;  // control: the same band solids as plain measured walls (no known-band rules at all)
static planscene::Scene MakeScene(const Road& road, double carX, double carY, std::pair<double, double> legal, double ownHalf) {
    planscene::Scene sc; sc.travel = 1; sc.hoverPossible = false; sc.continuousRecovery = true; sc.laneHalf = ownHalf;
    const double s0 = road.Station(carX, carY);
    for (int i = -20; i <= 80; ++i) { const auto p = road.At(s0 + i); if (sc.route.empty() || std::hypot(p.x - sc.route.back().x, p.y - sc.route.back().y) > .001) sc.route.push_back({ p.x, p.y, 0, 0 }); }
    sc.floor = [](double, double, double& z) { z = 0; return true; };
    sc.legal = { legal };
    sc.BuildKnownBandSolids();
    if (g_asWalls) { for (auto& s : sc.solids) s.band = false; sc.legal.clear(); }
    sc.Index();
    return sc;
}
static bicycle::Spec CarSpec() {
    bicycle::Spec k; k.car.lock = .5236; k.car.wheelbase = 3.633; k.car.slopePoint = .8; k.car.initialSlope = .7; k.car.fullSpeed = 45;
    k.halfWidth = 1.75; k.halfLength = 3.1; k.height = 2.4; k.maxSpeed = 16.5;
    return k;
}

struct Outcome { bool through = false; double seconds = 0, maxSwitchRate = 0, worstDepth = 0; int switches = 0, reverseTicks = 0; double endStation = 0; };
// Drive the real planner closed loop (plan every 0.05 s; the field every 2 s). done when the car is `past` m beyond `goalStation`.
static Outcome Drive(const Road& road, bicycle::State s, double want, double goalStation, double maxSeconds, const bicycle::Spec& k,
                     std::pair<double, double> legal, double ownHalf, bool verbose = false) {
    Outcome o; energyplan::Memory mem; planlattice::Field togo; double builtAt = -1e9, builtSt = -1e9;
    std::vector<double> switchAt; bool havePlan = false;
    for (double t = 0; t < maxSeconds; t += .05) {
        auto sc = MakeScene(road, s.x, s.y, legal, ownHalf);
        double hint = 0, lat = 0, half = 0; sc.Route(s.x, s.y, hint, lat, half);
        if (t - builtAt >= 2.0 || hint - builtSt >= 8.0 || !togo.ready) {
            const double goal = hint + 30; double gx = s.x, gy = s.y;
            for (const auto& q : sc.route) if (q.station >= goal) { gx = q.x; gy = q.y; break; }
            togo = planlattice::Build(sc, k, .5 * (s.x + gx), .5 * (s.y + gy), goal, hint); builtAt = t; builtSt = hint;
        }
        const auto p = energyplan::Plan(s, k, want, sc, mem, t, {}, hint, &togo);
        if (havePlan && !p.kept) { ++o.switches; switchAt.push_back(t); }
        havePlan = true;
        if (p.first.speed < -.05) ++o.reverseTicks;
        for (int i = 0; i < 1; ++i) { s.mode = 0; s = bicycle::Step(s, p.first, k, .05); }
        const double depth = sc.NoGoDepth(lat, s.heading - std::atan2(0, 1), k);
        (void)depth;
        const double st = road.Station(s.x, s.y);
        // worst: the body's clearance against band solids (negative = inside)
        double fz = 0; sc.Floor(s.x, s.y, fz);
        const double cl = sc.Clearance(s.x, s.y, s.heading, fz, k);
        o.worstDepth = std::max(o.worstDepth, -cl);
        if (g_trace && !p.kept) std::printf("      t=%5.2f pos=%7.2f,%7.2f hd=%6.1f v=%5.2f clear=%5.2f plan=%s why=%s rival=%s gap=%.1f tierbest=%d\n", t, s.x, s.y, s.heading * 57.29578, s.speed, cl, p.why, p.switchWhy, p.rival, p.rivalGap, 0);
        if (verbose && std::fmod(t, 1.0) < .05) std::printf("   t=%5.1f pos=%7.2f,%7.2f hd=%6.1f v=%5.2f st=%6.1f clear=%5.2f %s\n", t, s.x, s.y, s.heading * 57.29578, s.speed, st, cl, p.why);
        if (st >= goalStation) { o.through = true; o.seconds = t; o.endStation = st; break; }
        o.endStation = st; o.seconds = t;
    }
    for (std::size_t i = 0; i < switchAt.size(); ++i) { std::size_t j = i; while (j < switchAt.size() && switchAt[j] < switchAt[i] + 1.0) ++j; o.maxSwitchRate = std::max(o.maxSwitchRate, double(j - i)); }
    return o;
}

int main(int argc, char** argv) {
    const auto k = CarSpec();
    const std::vector<P> pts(std::begin(kPts), std::end(kPts));
    const Road road = MakeRoad(pts, 2.75);
    const std::pair<double, double> legal = { -8.3, 2.8 };
    const double ownHalf = 2.775;
    // station of road point 43 (-59.83, 1754.28), the exit of the S-bend, and 25 m after it
    const double st43 = road.Station(-57.0, 1752.0), goal = st43 + 25;

    if (argc > 3) { g_trace = true; const double lat = std::atof(argv[1]), hd = std::atof(argv[2]), v = std::atof(argv[3]); const double s0 = st43 - 1.0; const auto a = road.At(s0), b = road.At(s0 + 1.0); const double dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy);
        bicycle::State s; s.x = a.x + dy / l * lat; s.y = a.y - dx / l * lat; s.heading = hd * kPi / 180; s.speed = v; Drive(road, s, 6.0, goal + 10, 14, k, legal, ownHalf, true); return 0; }
    if (argc > 1) {  // exploration: poses about the exit of the S-bend (not run by the suite)
        for (double lat : { 0.0, .6, 1.0, 1.4 }) for (double hd : { -20.0, -32.0, -45.0 }) for (double v : { 0.0, 2.0, 4.0 }) {
            const double s0 = st43 - 1.0; const auto a = road.At(s0), b = road.At(s0 + 1.0);
            const double dx = b.x - a.x, dy = b.y - a.y, l = std::hypot(dx, dy);
            bicycle::State s; s.x = a.x + dy / l * lat; s.y = a.y - dx / l * lat; s.heading = hd * kPi / 180; s.speed = v;
            const auto o = Drive(road, s, 6.0, goal + 10, 40, k, legal, ownHalf);
            std::printf("lat %.1f hd %4.0f v %.0f -> through=%d %.1f s switches %d max/s %.0f reverse %d depth %.2f\n", lat, hd, v, o.through, o.seconds, o.switches, o.maxSwitchRate, o.reverseTicks, o.worstDepth);
        }
        return 0;
    }
    {   // BuildKnownBandSolids geometry: every band solid sits at the distance from the lane line it was built for, on the side it was built for
        planscene::Scene sc = MakeScene(road, -47, 1740, legal, ownHalf);
        const int built = int(std::count_if(sc.solids.begin(), sc.solids.end(), [](const planscene::Solid& s) { return s.band; }));
        double worstIn = 0, worstRoad = 0; int inRoad = 0;
        for (const auto& s : sc.solids) {
            if (!s.band) continue;
            double st, lat, half; sc.Route(s.x, s.y, st, lat, half);
            // distance of the solid centre to the nearest point of the line (segment distance)
            double d = 1e18; for (std::size_t i = 0; i + 1 < sc.route.size(); ++i) {
                const auto& a = sc.route[i]; const auto& b = sc.route[i + 1]; const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy; if (l2 < 1e-12) continue;
                const double t = std::clamp(((s.x - a.x) * dx + (s.y - a.y) * dy) / l2, 0.0, 1.0); d = std::min(d, std::hypot(s.x - a.x - t * dx, s.y - a.y - t * dy)); }
            // the right band lies 2.8..3.8 from the line, the left band 8.3..9.3: nothing of a band may lie closer to the line than its own edge
            const double edge = lat > 0 ? 2.8 : 8.3;
            if (d < edge - 1e-6) { ++inRoad; worstIn = std::max(worstIn, edge - d); }
            worstRoad = std::max(worstRoad, 0.0);
        }
        std::printf("   band solids %d, %d closer to the line than their own edge (worst %.2f m)\n", built, inRoad, worstIn);
        Check(inRoad == 0, "band: no solid lies inside the road (no spur at a corner, no fold across a tight arc)");
    }
    {   // a straight road builds what it always built
        planscene::Scene sc; sc.legal = { { -3.0, 1.5 } };
        for (double y = -20; y <= 80; y += 1) sc.route.push_back({ 0, y, 0, 0 });
        sc.floor = [](double, double, double& z) { z = 0; return true; };
        const int n = sc.BuildKnownBandSolids();
        // 2 bands x 2 columns x 100 m / 0.5 m
        Check(n >= 790 && n <= 810, "band: a straight road builds the same rows as before (about 800 solids)");
        bool right = true; for (const auto& s : sc.solids) right &= s.band && (std::fabs(s.x - 1.75) < 1e-6 || std::fabs(s.x - 2.25) < 1e-6 || std::fabs(s.x + 3.25) < 1e-6 || std::fabs(s.x + 3.75) < 1e-6);
        Check(right, "band: straight road: columns at the edge plus a quarter and three quarters of a metre, as before");
    }
    {   // Score: the deepest a rollout may go into a known band alone and still be clear is kNoGoTolerance (the heading sweeps how deep it drifts)
        planscene::Scene sc; sc.legal = { { -3.5, 1.4 } }; sc.continuousRecovery = true;
        for (double y = -20; y <= 120; y += 1) sc.route.push_back({ 0, y, 0, 0 });
        sc.floor = [](double, double, double& z) { z = 0; return true; };
        sc.BuildKnownBandSolids(); sc.Index();
        bicycle::Spec q; q.car = carmodel::Fallback(2.3); q.halfWidth = 1.0; q.halfLength = 2.3;
        double deepestFree = 0; int tierAt12 = -1;
        for (double a = 0; a <= .5; a += .001) {  // a swerve to the right and back (heading returns): the lateral reach grows with a
            bicycle::State s; s.x = .2; s.speed = 4; energyplan::Candidate c; c.count = 3; c.kind = "swerve";
            c.seg[0] = { a, 4, 1.0 }; c.seg[1] = { -a, 4, 1.0 }; c.seg[2] = { 0, 4, energyplan::kSteps * energyplan::kDt };
            static bicycle::State traj[energyplan::kSteps + 1]; energyplan::Terms t; double cl = 0;
            const double v = energyplan::Score(c, s, q, 4, sc, {}, nullptr, 0, t, traj, cl, std::nan(""), nullptr, nullptr, true, false);
            double worst = 0; for (int i = 0; i <= energyplan::kSteps; ++i) { double fz = 0; sc.Floor(traj[i].x, traj[i].y, fz); worst = std::max(worst, -sc.Clearance(traj[i].x, traj[i].y, traj[i].heading, fz, q)); }
            if (std::isfinite(v) && t.clearanceTier < 10) deepestFree = std::max(deepestFree, worst);
            if (std::isfinite(v) && std::fabs(a - .45) < .0006) tierAt12 = t.clearanceTier;
        }
        std::printf("   deepest drift into a band that is still tier 0: %.3f m (kBandSoft %.3f); tier of the largest swerve: %d\n", deepestFree, energyplan::kBandSoft, tierAt12);
        Check(deepestFree >= energyplan::kBandSoft - .012 && deepestFree <= energyplan::kBandSoft + .012, "tolerance: a rollout into a known band alone is a cost, not a tier, down to kBandSoft (3-6 cm of the stalled run included), no deeper");
        Check(tierAt12 >= 10, "tolerance: a swerve well past it is a clearance tier");
    }
    {   // a hairpin of 180 degrees, road 15.1 m, bands 2.0 m: the lane line 2.75 m right of the centre; outside lane (left turn) and inside lane (right turn)
        for (int right = 0; right < 2; ++right) {
            std::vector<P> hp = right ? std::vector<P>{ { -18, 0, 0 }, { -18, 50, 9 }, { 0, 50, 9 }, { 0, 0, 0 } } : std::vector<P>{ { 0, 0, 0 }, { 0, 50, 9 }, { -18, 50, 9 }, { -18, 0, 0 } };
            const Road hr = MakeRoad(hp, 2.75); const double end = hr.Length() - 15;
            const auto a = hr.At(15), b2 = hr.At(16);
            bicycle::State s; s.x = a.x; s.y = a.y; s.heading = std::atan2(b2.x - a.x, b2.y - a.y); s.speed = 8;
            const auto o = Drive(hr, s, 7.0, end, 70, k, legal, ownHalf);
            std::printf("   hairpin (%s lane): through=%d in %.1f s, switches %d (max %.0f in 1 s), reverse ticks %d, worst band depth %.2f\n", right ? "inside" : "outside", o.through, o.seconds, o.switches, o.maxSwitchRate, o.reverseTicks, o.worstDepth);
            // control: the same hairpin with no known bands (an older road): the planner's own switching in a 180 degree turn
            g_asWalls = true; const auto wall = Drive(hr, s, 7.0, end, 70, k, legal, ownHalf); g_asWalls = false;
            std::printf("   hairpin control, bands as measured walls: through=%d in %.1f s, switches %d (max %.0f in 1 s)\n", wall.through, wall.seconds, wall.switches, wall.maxSwitchRate);
            const auto ctl = Drive(hr, s, 7.0, end, 70, k, { -kInf, kInf }, ownHalf);
            std::printf("   hairpin control, no bands: through=%d in %.1f s, switches %d (max %.0f in 1 s)\n", ctl.through, ctl.seconds, ctl.switches, ctl.maxSwitchRate);
            Check(o.through && o.seconds < 50, right ? "hairpin: the inside lane is driven through" : "hairpin: the outside lane is driven through");
            Check(o.maxSwitchRate <= std::max(6.0, std::max(ctl.maxSwitchRate, wall.maxSwitchRate) + 2) && o.switches <= std::max(3.0 * o.seconds, std::max(ctl.switches, wall.switches) * 1.3 + 3), right ? "hairpin: inside lane: plan switches stay at the normal rate" : "hairpin: outside lane: plan switches stay at the normal rate");
            // mid-hairpin, standing 5 cm into the outer band, nose toward it: it must get out in bounded time
            const double mid = hr.Length() * .5; const auto m0 = hr.At(mid), m1 = hr.At(mid + 1);
            const double hd = std::atan2(m1.x - m0.x, m1.y - m0.y), rx = std::cos(hd), ry = -std::sin(hd);  // right of the nose
            bicycle::State t0; t0.heading = hd + (right ? .35 : -.35); t0.speed = 0;
            // lateral so that the body (turned .35 rad to the road) is 5 cm into the band on the outer side (the right side outside, the left inside)
            const double ext = k.halfWidth * std::cos(.35) + k.halfLength * std::sin(.35), side = right ? -1.0 : 1.0, edge = right ? 8.3 : 2.8;
            const double lat = side * (edge - ext + .05); t0.x = m0.x + rx * lat; t0.y = m0.y + ry * lat;
            const auto o2 = Drive(hr, t0, 6.0, end, 60, k, legal, ownHalf);
            std::printf("   hairpin (%s lane) from 5 cm into the band: through=%d in %.1f s, switches %d (max %.0f in 1 s), reverse ticks %d, worst %.2f\n", right ? "inside" : "outside", o2.through, o2.seconds, o2.switches, o2.maxSwitchRate, o2.reverseTicks, o2.worstDepth);
            Check(o2.through && o2.seconds < 40, "hairpin: from 5 cm into a band the car gets out and on in bounded time");
            Check(o2.maxSwitchRate <= 6 && o2.switches <= 3 * o2.seconds, "hairpin: from the band: plan switches stay at the normal rate");
        }
    }
    // approach from 40 m before the S-bend at 8.5 m/s
    {
        const auto st = road.At(road.Station(-50.5, 1722.0));
        const auto nx = road.At(road.Station(-50.5, 1722.0) + 1.0);
        bicycle::State s; s.x = st.x; s.y = st.y; s.heading = std::atan2(nx.x - st.x, nx.y - st.y); s.speed = 8.5;
        const auto o = Drive(road, s, 8.0, goal, 40, k, legal, ownHalf, true);
        std::printf("   approach 8.5 m/s: through=%d in %.1f s, switches %d (max %.0f in 1 s), reverse ticks %d, worst band depth %.2f\n", o.through, o.seconds, o.switches, o.maxSwitchRate, o.reverseTicks, o.worstDepth);
        Check(o.through && o.seconds < 30, "S-bend: from the approach at 8.5 m/s the car passes the exit in under 30 s");
        Check(o.maxSwitchRate <= 6 && o.switches <= 3 * o.seconds, "S-bend: approach: plan switches stay at the normal rate (log baseline 2-8 a second)");
    }
    {   // from rest 30 m before it
        const double s0 = road.Station(-50.5, 1706.0);
        const auto st = road.At(s0), nx = road.At(s0 + 1.0);
        bicycle::State s; s.x = st.x; s.y = st.y; s.heading = std::atan2(nx.x - st.x, nx.y - st.y); s.speed = 0;
        const auto o = Drive(road, s, 6.0, goal, 60, k, legal, ownHalf, true);
        std::printf("   from rest: through=%d in %.1f s, switches %d (max %.0f in 1 s), reverse ticks %d, worst band depth %.2f\n", o.through, o.seconds, o.switches, o.maxSwitchRate, o.reverseTicks, o.worstDepth);
        Check(o.through && o.seconds < 45, "S-bend: from rest the car passes the exit in under 45 s");
        Check(o.maxSwitchRate <= 6 && o.switches <= 3 * o.seconds, "S-bend: from rest: plan switches stay at the normal rate");
    }
    {   // the stuck pose of 08:01:00: (-55.95, 1755.70), heading -32.6 deg (toward the outer band), a few cm into it
        bicycle::State s; s.x = -55.95; s.y = 1755.70; s.heading = -32.6 * kPi / 180; s.speed = .3;
        const auto o = Drive(road, s, 6.0, goal + 10, 60, k, legal, ownHalf, true);
        std::printf("   stuck pose: through=%d in %.1f s, switches %d (max %.0f in 1 s), reverse ticks %d, worst band depth %.2f, end station %.1f (goal %.1f)\n", o.through, o.seconds, o.switches, o.maxSwitchRate, o.reverseTicks, o.worstDepth, o.endStation, goal + 10);
        Check(o.through && o.seconds < 30, "stuck pose: the car gets out and past the exit within 30 s");
        Check(o.maxSwitchRate <= 6 && o.switches <= 3 * o.seconds, "stuck pose: plan switches stay at the normal rate (the stalled run flipped 4.4 a second for 146 s, peaks of 13)");
    }
    std::printf("%s\n", failed ? "SOME FAILED" : "ALL PASSED");
    return failed ? 1 : 0;
}
