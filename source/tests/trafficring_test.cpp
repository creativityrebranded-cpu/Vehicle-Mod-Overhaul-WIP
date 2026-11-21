// DW-3: the rings of visibility (src/trafficsight.h: ringrules, Reach). OFFLINE logic only: never proof of anything in game.
// Cases: the T_unseen calculation and its cited constants; the reach (the viewer's maximum speed over T_unseen, the ring step), monotone in speed and time.
#include "trafficsight.h"
#include <cmath>
#include <cstdio>

static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace trafficsight;

int main() {
    // ---- T_unseen and the cited constants
    {
        Check(ringrules::UnseenFor(150, 3600, 100) == 3850 && ringrules::unseenMs == 3850, "T_unseen = create latency 150 + proving 3600 + margin 100 = 3850 ms");
        Check(ringrules::createLatencyMs == 150 && ringrules::provingMs == 3600 && ringrules::marginMs == 100, "the three terms are the cited constants (measured create latency, worst ordinary release 3.6 s, the planner margin)");
        Check(ringrules::UnseenFor(-5, 0, 100) == 100 && ringrules::UnseenFor(0, 0, 0) == 0, "T_unseen: a negative term counts as zero");
        Check(ringrules::rings == 3, "named constants: 3 rings");
        Check(ringrules::accelMps2 == 6.0 && ringrules::viewerTopMps == 150.0 && rules::rangeM == 350.0, "named constants: acceleration allowance 6 m/s^2 (planframe accel), craft top 150 m/s (carcap kFlightTop gear 4), visible range 350 m");
    }
    // ---- reach: the viewer's maximum speed over T_unseen and the ring step
    {
        const Reach z = ReachOf(0);
        Check(std::fabs(z.topMps - 6.0 * 3.85) < 1e-9 && std::fabs(z.totalM - 6.0 * 3.85 * 3.85) < 1e-9 && std::fabs(z.stepM - z.totalM / 3) < 1e-9, "reach: stationary = the acceleration allowance only: 23.1 m/s, 88.9 m in all, x = 29.6 m");
        const Reach v20 = ReachOf(20);
        Check(std::fabs(v20.topMps - (20 + 6.0 * 3.85)) < 1e-9 && std::fabs(v20.totalM - v20.topMps * 3.85) < 1e-9, "reach: at 20 m/s the maximum speed is 20 + 6 * 3.85 = 43.1 m/s, 166 m in all");
        Check(std::fabs(ReachOf(100).topMps - 123.1) < 1e-9 && ReachOf(140).topMps == 150.0, "reach: capped by the craft's own top speed (150 m/s)");
        Check(ReachOf(200).topMps == 200.0 && ReachOf(std::nan("")).topMps == ReachOf(0).topMps && ReachOf(-30).topMps == ReachOf(30).topMps, "reach: never below the current speed; a non-finite speed counts as stationary; direction does not matter");
        Check(ReachOf(30, 0).totalM == 0 && ReachOf(30, 3850, 5).rings == 5 && std::fabs(ReachOf(30, 3850, 5).stepM * 5 - ReachOf(30, 3850, 5).totalM) < 1e-9 && ReachOf(30, 3850, 0).rings == 1, "reach: zero time = no reach; the step is the total over the named number of rings (at least one)");
        bool mono = true; double prev = -1;
        for (double v = 0; v <= 300; v += 2) { const double t = ReachOf(v).totalM; if (t < prev - 1e-9) mono = false; prev = t; }
        Check(mono, "reach: a faster viewer never has a smaller reach (monotone in speed)");
        mono = true; prev = -1;
        for (int ms = 0; ms <= 8000; ms += 250) { const double t = ReachOf(25, ms).totalM; if (t < prev - 1e-9) mono = false; prev = t; }
        Check(mono, "reach: a longer unseen time never has a smaller reach (monotone in T_unseen)");
        Check(std::fabs(ReachOf(0).RadiusOf(3) - ReachOf(0).totalM) < 1e-9 && ReachOf(0).RadiusOf(0) == 0 && ReachOf(0).RadiusOf(9) == ReachOf(0).totalM, "reach: ring k has radius k * x; ring 0 is the viewer's own position");
    }
    std::printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
