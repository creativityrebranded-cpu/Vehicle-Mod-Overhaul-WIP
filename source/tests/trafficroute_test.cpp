#include "trafficroute.h"
#include "trafficcycle.h"
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
        trafficcycle::Dwell dwell;
        Check(!dwell.Step(1000,false) && !dwell.Step(60000,false), "descent never consumes landed dwell");
        Check(!dwell.Step(61000,true) && !dwell.Step(70999,true), "landed pause lasts ten seconds");
        Check(!dwell.Step(71000,false) && !dwell.Step(72000,true), "lost ground restarts dwell");
        Check(dwell.Step(82000,true), "continuous landed pause permits takeoff");
        Check(!trafficcycle::Repeat(1) && !trafficcycle::Repeat(2) && trafficcycle::Repeat(3) && trafficcycle::Repeat(4), "legacy trips remain one shot");
        Check(trafficcycle::Destination(2) && trafficcycle::Destination(4) && !trafficcycle::Destination(3), "destination and home cycles park correctly");
        // More than the old five-restart limit: every new lap must clear progress,
        // climb again, complete a circuit, and return to the actual parking point.
        for (int lane=0; lane<2; ++lane) {
            Vec home{50,40,0}, p=home;
            for (int lap=0; lap<12; ++lap) {
                auto trip=StartLap(rl,lane,p);
                Check(trip.leg==Leg::Climb && trip.travelled==0 && !trip.uKnown,"next lap starts fresh");
                Check(Fly(rl,trip,p,10.0,5000)>0 && trip.travelled>=rl.Span()*0.9,"every repeated trip traverses the lane");
                Check(std::hypot(p.x-home.x,p.y-home.y)<kReach+1,"repeated trip returns over landing spot");
                p=home; // core lands, then the lifecycle callback begins the next lap here
            }
        }
    }
    {
        trafficdata::Route slant = ring;
        slant.tilt = 10; slant.tiltToward = 0;  // high side north
        const Line sl = Build(slant);
        double north = -1e9, south = 1e9;
        for (const auto& p : sl.lane[0]) { if (p.y > 590) north = p.z; if (p.y < -590) south = p.z; }
        const double rise = 600 * std::tan(10 * 3.14159265358979 / 180);
        Check(std::fabs(north - (300 + rise)) < 1 && std::fabs(south - (300 - rise)) < 1, "a slanted ring is higher on its high side, lower opposite");
        Check(std::fabs(sl.lane[1][0].z - sl.lane[0][0].z - 40) < 1e-9, "lane 1 keeps its gap on a slanted ring");
    }
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
    {
        trafficdata::Route corner;corner.ring=false;corner.closed=false;corner.lanes=1;
        corner.points={{0,0,20},{0,100,20},{100,100,20}};
        const auto line=Build(corner);const auto goal=At(line,0,LeadU(line,0,0.8,70));
        Check(std::fabs(goal.x)<0.01&&std::fabs(goal.y-100)<0.01,"lookahead stops at a sharp corner instead of cutting a diagonal through obstacles");
        const auto reverse=At(line,1,LeadU(line,1,1.2,70));
        Check(std::fabs(reverse.x)<0.01&&std::fabs(reverse.y-100)<0.01,"reverse lane preserves the same sharp corner");
    }
    {
        trafficdata::Route r;r.ring=false;r.closed=false;r.lanes=1;
        r.points={{0,0,10},{100,0,20},{100,100,30}};r.rounding={0,20,0};
        auto rounded=Build(r);Check(rounded.Count()>3,"rounded point expands into actual shared flight geometry");
        Check(std::fabs(rounded.lane[0][1].x-80)<1e-5&&std::fabs(rounded.lane[0][1].y)<1e-5,"20m fillet starts on the incoming tangent");
        bool circle=true;for(std::size_t i=1;i+1<rounded.lane[0].size();++i){const auto& p=rounded.lane[0][i];circle=circle&&std::fabs(std::hypot(p.x-80,p.y-20)-20)<1e-5&&std::isfinite(p.z);}
        Check(circle,"all arc samples lie on requested-radius circle with finite continuous heights");
        for(double u:{0.0,0.5,1.0,1.5,2.0})Check(std::fabs(rounded.ToSource(rounded.ToLine(u))-u)<1e-6,"authored junction/exit positions survive path expansion");
        r.rounding={0,200,0};auto clamped=Build(r);Check(clamped.lane[0][1].x>=55-1e-5,"oversized radius is limited to adjacent segment length");
        r.closed=true;r.points={{0,0,20},{100,0,20},{100,100,20},{0,100,20}};r.rounding={20,20,20,20};
        const auto loop=Build(r);Vec position{0,0,0};Trip trip=StartLap(loop,0,position);
        const int completed=Fly(loop,trip,position,10,8000);if(completed<0)std::printf("rounded debug: p %.2f %.2f %.2f leg %d u %.3f travelled %.3f span %.3f target %.3f\n",position.x,position.y,position.z,int(trip.leg),trip.lastU,trip.travelled,loop.Span(),trip.targetU);
        Check(completed>0,"rounded dense loop makes full lap progress instead of rejecting jumps of three sample indices");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
