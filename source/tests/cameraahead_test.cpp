// Card CP-1: camera prediction (cameraahead.h). Offline only.
#include "cameraahead.h"
#include "camera_visibility.h"
#include <cmath>
#include <cstdio>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace cameraahead;

static Sample At(double t, double yawDeg, double ox = 0, double oy = 0)
{
    const double y = yawDeg * 3.14159265358979 / 180.0;
    Sample s; s.t = t; s.o[0] = ox; s.o[1] = oy; s.o[2] = 0; s.f[0] = std::cos(y); s.f[1] = std::sin(y); s.f[2] = 0; return s;
}

int main()
{
    {
        using namespace cameravisibility;
        Check(Fresh(100, 220, cameravisibility::rules::mouseMs) && !Fresh(100, 221, cameravisibility::rules::mouseMs) &&
            !Fresh(100, 99, cameravisibility::rules::mouseMs),
            "mouse hint expires and rejects future timestamps");
        double gain=0;
        Check(std::abs(MouseRate(.096,10,.06,gain)-2.0)<.001 &&
            MouseRate(0,10,.06,gain)>0 && MouseRate(0,-10,.06,gain)<0,
            "measured turn calibrates the signed mouse rate and applies the lookahead multiplier");
        Forecast look;
        look.forward[0]=1;look.yawRate=1.8;
        Check(EntryMs(look,0,100,0,0,250)==-1 && EntryMs(look,0,100,0,0,350)==300 &&
            EntryMs(look,100,0,0,0,250)==0 && EntryMs(look,0,-100,0,0,350)==-1,
            "entry time compares the same turn against spawn and retirement budgets");
        look.origin[1]=100;
        Check(EntryMs(look,20,100,0,0,250)==0,"a displaced camera defines the view from its own origin");
        Check(AdvanceHealth(149,850,1000) && !AdvanceHealth(150,850,1000) &&
            !AdvanceHealth(20,299,1000) && !AdvanceHealth(0,850,1000),
            "an upcoming view advances only eligible Health checks that would otherwise run too late");
        Check(RayEvidence(false, false, 0, 100, 4) == Evidence::Waiting &&
            RayEvidence(true, true, .5, 100, 4) == Evidence::Waiting,
            "unanswered and camera-adjacent hits cannot prove concealment");
        Check(RayEvidence(true, false, 0, 100, 4) == Evidence::Visible &&
            RayEvidence(true, true, 98, 100, 4) == Evidence::Visible &&
            RayEvidence(true, true, 50, 100, 4) == Evidence::Occluded,
            "clear rays and target-surface hits protect the vehicle, early hits can occlude a sample");
        Check(Combine(4, 0) == Evidence::Waiting && Combine(4, 1) == Evidence::Visible && Combine(5, 0) == Evidence::Occluded,
            "one open edge protects the target and concealment requires all five rays");
        Check(!HoldReason(Evidence::Outside) && !HoldReason(Evidence::Occluded) &&
            HoldReason(Evidence::Unknown) && HoldReason(Evidence::MouseTurn) &&
            HoldReason(Evidence::Waiting) && HoldReason(Evidence::Visible),
            "the camera extension can hold an eligible retirement or leave the earlier decision in force");
    }
    const double cone = 60;  // vehiclekeep's g_coneDeg
    // A point 100 m away at 90 degrees left of a camera facing east.
    const double lx = 0, ly = 100;
    {   const Sample a = At(0, 0), b = At(.1, 0);
        Check(Judge(a, b, 100, 0, 0, .5, cone) == 1, "in view now: seen");
        Check(Judge(a, b, lx, ly, 0, .5, cone) == 0, "still camera, point 90 deg off: out now and ahead"); }
    {   // Turning left at 60 deg/s: in 0.5 s the cone edge reaches 60 + 30 = 90 deg -> the point comes into view.
        const Sample a = At(0, 0), b = At(.1, 6);
        double rate = 0; const int j = Judge(a, b, lx, ly, 0, .5, cone, &rate);
        Check(j == 2, "turning towards it: comes into view ahead");
        Check(std::fabs(rate - 60) < 1, "turn rate read as about 60 deg/s"); }
    {   const Sample a = At(0, 6), b = At(.1, 0);  // turning right, away from it
        Check(Judge(a, b, lx, ly, 0, .5, cone) == 0, "turning away: out now and ahead"); }
    {   // A fast turn that sweeps past the point inside the window is still caught (intermediate moments are checked).
        const Sample a = At(0, 0), b = At(.05, 30);  // 600 deg/s: at 0.5 s it is far past 90 deg
        Check(Judge(a, b, lx, ly, 0, .5, cone) == 2, "fast turn sweeping over it: caught on the way"); }
    {   const Sample a = At(0, 0), b = At(.1, 0);
        Check(Judge(Sample{}, b, lx, ly, 0, .5, cone) == 3, "no earlier sample: turn unknown (seen)");
        Check(Judge(At(-1, 0), b, lx, ly, 0, .5, cone) == 3, "stale sample: turn unknown (seen)");
        Check(Judge(At(.09, 0), b, lx, ly, 0, .5, cone) == 3, "samples too close: turn unknown (seen)"); }
    {   // The margin: a point just outside the cone (62 deg off) counts as about to be seen.
        const double y = 62 * 3.14159265358979 / 180.0;
        Check(Judge(At(0, 0), At(.1, 0), 100 * std::cos(y), 100 * std::sin(y), 0, .5, cone) == 2, "just outside the cone: inside the margin, seen"); }
    {   // Camera travel: moving fast towards a point behind-left changes nothing about the cone direction, but a point we drive past
        // at 30 m/s is judged from the extrapolated position.
        const Sample a = At(0, 0, 0, 0), b = At(.1, 0, 3, 0);
        Check(Judge(a, b, -50, 60, 0, .5, cone) == 0, "behind-left while driving away: out"); }
    {   const Sample b = At(.1, 0);
        Check(InCone(b.o, b.f, 0.5, 0, 0, .5), "within 1 m: always in view"); }
    {   const auto a = At(0, 0), b = At(.1, 0);
        Check(Judge(a, b, 0, 100, 0, .5, cone, nullptr, 65) == 1,
              "fall envelope reaches into view although its center is outside");
        Check(Judge(a, b, 0, 100, 0, .5, cone, nullptr, 45) == 2,
              "fall envelope reaches the widened prediction cone");
        Check(Judge(a, b, 0, 500, 0, .5, cone, nullptr, 72) == 0 &&
              Judge(Sample{}, b, 0, 500, 0, .5, cone, nullptr, 72) == 3,
              "far envelope is clear only with known camera history");
        Check(Judge(a, b, 0, 0, -500, .5, cone, nullptr, 72) == 0 &&
              Judge(a, b, 0, 0, -50, .5, cone, nullptr, 72) == 1,
              "horizontal view clears a deep fall but a camera inside the envelope sees it"); }
    std::printf("cameraahead_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
