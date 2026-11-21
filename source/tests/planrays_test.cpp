// Ray priority across cars (planrays.h, step 6).
#include "planrays.h"
#include <cstdio>
using namespace planrays;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    {   static Budget b; bool all = true;
        for (std::uint64_t f = 0; f < 20; ++f) for (int p = 0; p < 4; ++p) if (!b.Ask(f, p, 100.0 * p)) all = false;
        Check(all, "four cars or fewer: every car every frame, as before"); }
    {   static Budget b; int servedNear = 0, servedFar = 0, longestWait = 0, wait = 0;
        for (std::uint64_t f = 0; f < 200; ++f) for (int p = 0; p < 12; ++p) {
            const bool go = b.Ask(f, p, 10.0 + 20.0 * p);
            if (f > 1 && p == 0 && go) ++servedNear;
            if (f > 1 && p == 11) { if (go) { ++servedFar; wait = 0; } else longestWait = std::max(longestWait, ++wait); }
        }
        Check(servedNear > servedFar, "twelve cars: the nearest is measured more often");
        Check(servedFar > 0 && longestWait < 30, "and the farthest is never starved");
        int served = 0; for (int p = 0; p < 12; ++p) served += b.allowed[p] ? 1 : 0;
        Check(served == kCars, "the budget holds: four sweeps a frame"); }
    {   static Budget b; Check(b.Ask(0, -1, 0) && b.Ask(0, kMaxPilots, 0), "an unknown pilot is never refused"); }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
