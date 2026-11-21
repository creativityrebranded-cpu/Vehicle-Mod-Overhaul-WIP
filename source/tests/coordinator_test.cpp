#include "traffic/coordinator.h"
#include "../src/heldwhy.h"
#include <string>
#include <cstdio>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
struct Lane { int held = 0; double stopDistance = -1; };  // the two route-data fields the coordinator writes (FvoLaneGuidanceV1)

int main() {
    using namespace coordinator;
    {   Lane l; Holds<Lane> h(l);
        Check(l.held == 0 && l.stopDistance < 0 && h.bits == 0, "no rule: no hold, no stop line");
        h.HoldIf(false, Holding); Check(l.held == 0, "a rule that does not apply holds nothing");
        h.Hold(ExcludedModel); h.Hold(Junction); Check(l.held != 0 && (unsigned(l.held) & 0xFFFFu) == (ExcludedModel | Junction) && (unsigned(l.held) & kHeldReasons) && h.bits == (ExcludedModel | Junction), "holds keep every reason (the lane_hold trace bits), and the lane carries them (PD-1c)"); Check(std::string(heldwhy::Name(l.held)) == "route hold: junction right of way" && std::string(heldwhy::Name(1)) == "route hold (reason not given)" && *heldwhy::Name(0) == 0, "the core names the reason an addon sends, and an older addon's plain 1 still reads as held (PD-1c)"); }
    {   Lane l; Holds<Lane> h(l);
        h.StopMin(30); h.StopMin(12); h.StopMin(20); Check(l.stopDistance == 12, "stop lines: the nearest wins");
        h.StopSet(40); Check(l.stopDistance == 40, "the section control replaces an earlier line (as before)");
        h.StopMin(25); Check(l.stopDistance == 25, "a nearer line after it still wins"); }
    std::printf("coordinator_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
