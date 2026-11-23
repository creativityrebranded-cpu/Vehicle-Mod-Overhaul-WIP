#include "planspeed.h"
#include <chrono>
#include <cstdio>
#include <cstring>
using namespace planspeed;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

// Run the solver as the car would: apply its first step, solve again.
static double Drive(Input in, double seconds, double* finalAlong = nullptr, double* peak = nullptr, int* flips = nullptr) {
    double v = in.speed, along = 0, last = 0, pk = 0; int sign = 0, fl = 0;
    Limit limits[kMaxLimits]; Block blocks[kMaxBlocks];
    for (double t = 0; t < seconds; t += .1) {
        Input s = in; s.speed = v; s.lastAccel = last; s.length = in.length < 1e8 ? in.length - along : 1e9;
        for (int i = 0; i < in.limitCount; ++i) { limits[i] = in.limits[i]; limits[i].from -= along; limits[i].to -= along; }
        for (int i = 0; i < in.blockCount; ++i) { blocks[i] = in.blocks[i]; blocks[i].from -= along; blocks[i].to -= along;
            if (blocks[i].tTo < 1e8 || blocks[i].tFrom > 0) { blocks[i].tFrom -= t; blocks[i].tTo -= t; } }
        s.limits = limits; s.blocks = blocks;
        const auto o = Solve(s);
        last = o.accel;
        const int g = last > .5 ? 1 : last < -.5 ? -1 : 0;
        if (g && sign && g != sign) ++fl;
        if (g) sign = g;
        const double nv = std::max(0.0, v + last * .1);
        along += (v + nv) * .05; v = nv; pk = std::max(pk, v);
    }
    if (finalAlong) *finalAlong = along;
    if (peak) *peak = pk;
    if (flips) *flips = fl;
    return v;
}

int main() {
    const double kL = 2.3, kB = 6.0;  // a car half length and brake for the following checks

    Check(FollowingCap(2*kL,12,kL,kB)==0,"following: minimum bumper gap applies even to a moving leader");
    Check(std::fabs(FollowingCap(2*kL+(wheelguard::reaction+8/kB)*8,8,kL,kB)-8)<1e-9,
        "following: stable at the leader's speed and the configured headway");
    Check(FollowingCap(25,0,kL,kB)<FollowingCap(25,10,kL,kB)&&FollowingCap(15,0,kL,kB)<FollowingCap(25,0,kL,kB),
        "following: brake more for a nearer or stopped leader");
    {   Input in; in.want = 18; const double v = Drive(in, 8);
        Check(std::fabs(v - 18) < 1, "an open road reaches the wanted speed"); }
    {   Input in; in.speed = 18; in.want = 18; Block b; b.from = 60; b.to = 64; b.why = "wall"; in.blocks = &b; in.blockCount = 1;
        double along = 0; const double v = Drive(in, 15, &along);
        Check(v < .3, "it stops for a fixed block"); Check(along <= 60 - 1.5 + .3, "short of it, by the gap"); Check(along > 50, "and not far short"); }
    {   Input in; in.speed = 10; in.want = 18; Limit l; l.from = 50; l.to = 70; l.speed = 6; l.why = "corner"; in.limits = &l; in.limitCount = 1;
        const auto o = Solve(in); Check(o.feasible, "a corner ahead can be planned for");
        double along = 0, v = 18; { Input r = in; r.speed = 18; v = Drive(r, 3.5, &along); }
        Check(v < 16, "it is slowing before the corner");
        Input r = in; r.speed = 18; double a2 = 0; Drive(r, 6, &a2);
        Check(a2 > 50, "and through it"); }
    {   Input in; in.speed = 10; in.want = 10; Block b; b.from = 20; b.to = 26; b.tFrom = 1; b.tTo = 3; b.gap = 1.5; b.why = "crossing car"; in.blocks = &b; in.blockCount = 1;
        double along = 0; Drive(in, 2.9, &along); Check(along < 18.6, "it gives way to a crossing car");
        double later = 0; const double v = Drive(in, 9, &later); Check(later > 30 && v > 7, "then goes on, without being told"); }
    {   Input in; in.speed = 18; in.want = 18; in.brake = 6; Block b; b.from = 8; b.to = 12; b.why = "wall"; in.blocks = &b; in.blockCount = 1;
        const auto o = Solve(in); Check(!o.feasible, "too close to stop: this branch is out"); Check(o.blockedAt == 8, "and it says where");
        Check(o.accel <= -5.9, "the least bad answer is full braking"); }
    {   Input in; in.speed = 10; in.want = 20; in.length = 40; double along = 0; const double v = Drive(in, 12, &along);
        Check(v < .3 && along <= 39.5, "the end of the line is a stop"); }
    {   Input in; in.speed = 12; in.want = 12; Limit l; l.from = 0; l.to = 1e9; l.speed = 12; l.why = "lane speed"; in.limits = &l; in.limitCount = 1;
        double peak = 0; int flips = 0; Drive(in, 10, nullptr, &peak, &flips);
        Check(peak <= 12.05, "a limit is never passed"); Check(flips == 0, "no see-sawing at a limit"); }
    {   Input in; in.speed = 0; in.want = 0; const auto o = Solve(in); Check(o.feasible && o.speed[kSteps] == 0, "wanting nothing stays stopped"); }
    {   Input in; in.speed = 10; in.want = 18; Limit l; l.from = 5; l.to = 9; l.speed = 1; l.why = "corner"; in.limits = &l; in.limitCount = 1;
        const auto o = Solve(in); Check(o.why[0] != 0, "it names what held the speed down"); }
    Check(std::fabs(CornerLimit(1.0 / 14, 3.5) - 7) < .01, "corner speed: sqrt(a_lat / curvature) (3.5 m/s^2 given)");
    Check(std::fabs(SlopeLimit(20, .12, .5) - 15.2) < .01, "slope: the old rule is the rule with MaxGrade .5");
    Check(CrestLimit(0, .5) > 1e8 && CrestLimit(.05, .5) < 10, "crest: sqrt(share g / curvature), flat is no limit");
    Check(std::fabs(SideLimit(.1) - 12) < 1e-9 && SideLimit(.6) == 3 && SideLimit(0) > 1e8, "sideways limit is D3");
    {   // Stage E: values come from the car record
        const carcap::Car car = carcap::kCar;
        Check(FollowingCap(2*kL,0,kL,kB)==0 && FollowingCap(2*kL+1,0,kL,kB)>0, "E: standing gap is two half lengths");
        Check(FollowingCap(30,0,3.0,kB)<FollowingCap(30,0,1.5,kB), "E: a longer car keeps a longer gap");
        const double room=20,lead=10,h=wheelguard::reaction+lead/kB;
        { const double bt=.5*kB*wheelguard::reaction; Check(std::fabs(FollowingCap(2*kL+room,lead,kL,kB)-(room/h<=lead?room/h:std::sqrt(bt*bt+lead*lead+kB*room)-bt))<1e-9,"E: headway = t_r + v_lead/brake, comfort brake = brake/2, one reaction before braking"); }
        Check(FollowingCap(40,8,kL,12)>FollowingCap(40,8,kL,3),"E: better brakes follow closer");
        { const double a=1e9; double prev=-1; bool mono=true; for(double g=2*kL;g<80;g+=.5){const double c=FollowingCap(g,10,kL,kB); if(c<prev-1e-9)mono=false; prev=c; (void)a;} Check(mono,"E: following cap grows with the gap (continuous at the steady branch)"); }
        Check(std::fabs(CornerLimit(1.0/14,car.PlanLat(1.0))-std::sqrt(car.PlanLat(1.0)*14))<1e-9,"E: corner speed uses the car's PlanLat");
        Check(CornerLimit(1.0/14,car.PlanLat(2.0))>CornerLimit(1.0/14,car.PlanLat(1.0)),"E: a grippier gear corners faster");
        Check(SlopeLimit(20,.12,car.MaxGrade(1.0))>SlopeLimit(20,.12,.5)&&SlopeLimit(20,0,car.MaxGrade(1.0))==20,"E: slope loss follows the car's MaxGrade");
        Check(SlopeLimit(20,.6,car.MaxGrade(1.0))==20*kMinShare&&SlopeLimit(20,.2,.8)>SlopeLimit(20,.2,.4),"E: steeper than it climbs bottoms at the floor; more grade = less loss");
        Check(std::fabs(CrestShare(car)-(1-car.safety))<1e-12&&CrestLimit(.05,CrestShare(car))<CrestLimit(.05,.5),"E: crest share is what the safety margin spares");
    }    {   Input in; in.speed = 15; in.want = 18; Limit ls[6]; Block bs[4];
        for (int i = 0; i < 6; ++i) { ls[i].from = 10.0 * i; ls[i].to = ls[i].from + 8; ls[i].speed = 12 + i; ls[i].why = "x"; }
        for (int i = 0; i < 4; ++i) { bs[i].from = 30 + 8.0 * i; bs[i].to = bs[i].from + 3; bs[i].tFrom = i; bs[i].tTo = i + 1.5; bs[i].why = "y"; }
        in.limits = ls; in.limitCount = 6; in.blocks = bs; in.blockCount = 4;
        const auto t0 = std::chrono::steady_clock::now(); double sum = 0;
        for (int i = 0; i < 2000; ++i) { in.speed = 15 + (i % 7) * .1; sum += Solve(in).cost; }
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000;
        std::printf("COST one solve %.1f us (checksum %.1f); 8 branches x 256 cars x 10 Hz = %.1f%% of one core\n", us, sum, us * 8 * 256 * 10 / 1e6 * 100); }
    {   // PD-1c (2026-11-12): a stop says why. A route hold names its reason; a lane speed limit names itself instead of nothing.
        Input in; in.speed = 4; in.want = 10; in.length = 6; in.endWhy = "route hold: junction right of way";
        Check(!std::strcmp(Solve(in).why, "route hold: junction right of way"), "PD-1c: a held line names the hold's reason, not end of the line");
        Input w; w.speed = 0; w.want = 0; w.wantWhy = "lane speed limit";
        Check(!std::strcmp(Solve(w).why, "lane speed limit"), "PD-1c: a wanted speed of 0 from the lane is named, never empty");
        Input o; o.speed = 18; o.want = 18;
        Check(!*Solve(o).why, "PD-1c: an open road at the wanted speed names nothing"); }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
