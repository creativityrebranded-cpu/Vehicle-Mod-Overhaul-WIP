// VS-1 (src/vehiclevel.h): World's velocity from position differences. Offline logic only: never proof of behaviour in game.
#include "vehiclevel.h"
#include <cstdio>
#include <cmath>
static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace vehiclevel;
int main() {
    {   float v[3] = {}; for (int k = 0; k < 30; ++k) { const float d[3] = { .5f, 0, 0 }; Step(v, d, .05); }
        Check(std::fabs(v[0] - 10.f) < .5f, "VS-1: steady 10 m/s steps converge to 10 m/s"); }
    {   float v[3] = { 4, 0, 0 }; const float d[3] = { 20, 0, 0 }; const auto e = Step(v, d, .1);
        Check(e == Event::JumpIgnored && v[0] == 0, "VS-1: a 20 m step in 0.1 s (200 m/s) is a jump: speed 0"); }
    {   float v[3] = { 4, 0, 0 }; const float d[3] = { 436, 0, 0 }; Check(Step(v, d, .02) == Event::Reset && v[0] == 0, "VS-1: the 50 m reset is kept (436 m teleport)"); }
    {   float v[3] = { 6, 0, 0 }; const float d[3] = { 1, 0, 0 }; Check(Step(v, d, 2.0) == Event::Stale && v[0] == 0, "VS-1: a sample after 2 s: the old speed is not carried on"); }
    {   float v[3] = { 3, 0, 0 }; const float d[3] = { 0, 0, 0 }; Check(Step(v, d, 0) == Event::None && v[0] == 3, "VS-1: no time passed: nothing changes"); }
    {   float v[3] = {}; const float d[3] = { 2.9f, 0, 0 }; double implied = 0; Check(Step(v, d, .05, &implied) == Event::Updated && implied < 60, "VS-1: 58 m/s is still believed"); }
    std::printf("vehiclevel_test: %d checks, %d failed\n%s\n", checks, failed, failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
