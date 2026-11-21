// goaldoor.h: one way in for every goal; endings from the old contracts' own flags.
#include "goaldoor.h"
#include <cstdio>
using namespace goaldoor;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    Check(JobEnding(false, false, false, false) == Ending::Stop, "a job goal with no flags stops at the goal");
    Check(JobEnding(true, false, false, false) == Ending::Arrive, "hold: arrive and stay");
    Check(JobEnding(true, true, false, false) == Ending::Land, "hold + land on arrival: a landing (an arrive line, not a separate sequence)");
    Check(JobEnding(true, true, true, false) == Ending::Pass, "pass-through wins: it never stops there");
    Check(JobEnding(false, false, false, true) == Ending::Pass, "keep going (follow): pass");
    Check(LaneEnding(0, false) == Ending::Pass && LaneEnding(1, false) == Ending::Stop && LaneEnding(2, false) == Ending::Arrive && LaneEnding(2, true) == Ending::Park,
        "a lane's line_end maps to pass / stop / arrive / park");
    Door<4> d; Goal a; a.source = Source::Lane; a.identity = 7; a.x = 10; a.y = 20;
    Check(d.Admit(1, a), "the first goal is a change");
    Check(!d.Admit(1, a), "the same goal again is not (one trace line per change)");
    Goal b = a; b.x = 10.4; Check(!d.Admit(1, b), "a moving goal that moves a little is not a change");
    Goal c = a; c.ending = Ending::Arrive; c.heading = 1.0; Check(d.Admit(1, c), "a new ending is a change");
    Check(d.Of(1) && d.Of(1)->ending == Ending::Arrive && d.Of(1)->heading == 1.0, "Route reads the ending and heading from the door");
    Goal cmd; cmd.source = Source::Command; cmd.direct = true; Check(d.Admit(2, cmd) && d.Of(2)->direct, "a command is recorded as direct actuation");
    Check(!d.Admit(9, a) && d.Of(9) == nullptr, "out of range pilots are refused");
    d.Clear(1); Check(d.Of(1) == nullptr, "cleared when the trip ends");
    std::printf("%s (%d failed)\n", failed ? "FAILED" : "ALL PASS", failed);
    return failed ? 1 : 0;
}
