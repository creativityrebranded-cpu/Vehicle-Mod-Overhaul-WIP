// AG-1 (2026-11-18): Cruise picks its gear from wheel slip and skids. SP-1 (2026-11-19): the grade no longer picks a gear.
#include "autogear.h"
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
int main() {
    using namespace autogear;
    Check(Wanted(0, 0) == 3 && Wanted(5.0, 0) == 2 && Wanted(9.0, 0) == 1, "RB-1: the rear slip angle picks 3 / 2 / 1 (4 and 8 degrees)");
    Check(Wanted(0, 2.0) == 1, "AG-1: a skid is gear 1");
    State s; double t = 0; int g = 3;
    for (; t < .3; t += .05) g = Update(s, t, 9.0, 0);
    Check(g == 3, "AG-1: a downshift waits for the cause to last");
    for (; t < .8; t += .05) g = Update(s, t, 9.0, 0);
    Check(g == 1, "AG-1: the back stepping out: down to gear 1 in under a second");
    for (; t < 2.8; t += .05) g = Update(s, t, 0, 0);
    Check(g == 1, "AG-1: once it settles it does not shift up at once");
    for (; t < 4.0; t += .05) g = Update(s, t, 0, 0);
    Check(g == 2, "AG-1: then one gear up");
    for (; t < 7.0; t += .05) g = Update(s, t, 0, 0);
    Check(g == 3, "AG-1: and one more after another hold");
    State w; double tt = 0; int gw = 3; for (; tt < .2; tt += .05) gw = Update(w, tt, 12.0, 0);
    Check(gw == 3, "AG-1: one spike of slip is smoothed and held");
    for (; tt < 1.0; tt += .05) gw = Update(w, tt, 12.0, 0);
    Check(gw == 1, "RB-1: the back stepping out 12 degrees: gear 1");
    State ramp; double tr = 0; int gr = 3; for (; tr < 5.0; tr += .05) gr = Update(ramp, tr, 0, 0);
    Check(gr == 3, "SP-1: a straight climb with no slip stays in gear 3 (the slope is the tyre controller's)");
    Check(std::fabs(rules::downHold - 2 * wheelguard::reaction) < 1e-12 && std::fabs(rules::slipSmooth - wheelguard::reaction) < 1e-12, "stageF: down hold = 2 reactions, slip filter = 1 reaction");
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
