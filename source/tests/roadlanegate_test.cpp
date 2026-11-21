// The plausibility gate for road-lane answers (roadlanes.h RoadLanesPlausible, used by vehicleflight.cpp). Offline; not in-game proof.
#include "roadlanes.h"
#include <cstdio>
#include <limits>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
int main() {
    FvoRoadLanesV1 r; r.count = 2; r.own = 0; r.left[0] = -1.75; r.right[0] = 1.75; r.left[1] = 1.75; r.right[1] = 5.25;
    Check(RoadLanesPlausible(r), "ordinary two lane road: plausible");
    FvoRoadLanesV1 t = r; t.right[1] = 2.35;  // outer lane trimmed to 0.6 m
    Check(!RoadLanesPlausible(t), "trimmed outer lane 0.6 m, no band known: rejected as before");
    t.bandHigh = .8; bool helped = false;
    Check(RoadLanesPlausible(t, nullptr, nullptr, nullptr, &helped) && helped, "same lane with the 0.8 m band added back (1.4 m untrimmed): accepted, and says why");
    FvoRoadLanesV1 wrong = t; wrong.bandLow = .8; wrong.bandHigh = 0;
    Check(!RoadLanesPlausible(wrong), "a band on the OTHER side does not rescue the lane");
    FvoRoadLanesV1 thin = r; thin.left[1] = 1.75; thin.right[1] = 1.9; thin.bandHigh = .3;
    int bad = -1; double w = 0, full = 0;
    Check(!RoadLanesPlausible(thin, &bad, &w, &full) && bad == 1 && full < 1.0, "a truly thin lane still fails even with the band (0.45 m untrimmed)");
    FvoRoadLanesV1 inner; inner.count = 3; inner.own = 0; inner.left[0] = -1.75; inner.right[0] = 1.2; inner.left[1] = 1.2; inner.right[1] = 1.9; inner.left[2] = 1.9; inner.right[2] = 5.0; inner.bandLow = 5; inner.bandHigh = 5;
    Check(!RoadLanesPlausible(inner), "a thin INNER lane is never rescued by the outer bands (0.7 m)");
    FvoRoadLanesV1 old = t; old.size = kFvoRoadLanesV1BaseSize;
    Check(!RoadLanesPlausible(old), "an older caller's smaller struct: band fields are not read (unchanged behaviour)");
    FvoRoadLanesV1 wide = r; wide.right[1] = 30; Check(!RoadLanesPlausible(wide), "over 20 m wide still fails");
    FvoRoadLanesV1 far = r; far.left[0] = -45; Check(!RoadLanesPlausible(far), "beyond 40 m still fails");
    FvoRoadLanesV1 off = r; off.own = 1; Check(!RoadLanesPlausible(off), "own lane not at the car still fails");
    FvoRoadLanesV1 single; single.count = 1; single.own = 0; single.left[0] = -.2; single.right[0] = .5; single.bandLow = .5; single.bandHigh = .5;
    Check(RoadLanesPlausible(single), "a single lane trimmed on both sides: both bands added back");
    std::printf("%s\n", failed ? "SOME FAILED" : "ALL PASSED"); return failed ? 1 : 0;
}
