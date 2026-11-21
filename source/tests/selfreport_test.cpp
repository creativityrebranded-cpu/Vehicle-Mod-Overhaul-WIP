// Card PF-1: pair self-reports (selfreport.h). Offline only.
#include "selfreport.h"
#include <cstdio>
#include <cstring>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace selfreport;

int main()
{
    {   Record r; std::uint64_t t = 1000;
        for (int i = 0; i < 40; ++i, t += 100) Update(r, t, true, false, false);  // 4 s not moving
        Check(Facts(r, t) == 0, "not moving 4 s: not published yet");
        for (int i = 0; i < 20; ++i, t += 100) Update(r, t, true, false, false);
        Check(Facts(r, t) == BitNotMoving && !std::strcmp(Name(Facts(r, t)), "asked_to_move_not_moving"), "not moving 6 s: published");
        Update(r, t, false, false, false);
        Check(Facts(r, t) == 0, "moving again: cleared at once"); }
    {   Record r; std::uint64_t t = 0;
        for (int i = 0; i < 31; ++i, t += 100) Update(r, t + 1, false, false, true);
        Check(Facts(r, t + 1) & BitTipped, "tipped 3 s: published"); }
    {   Record r; Update(r, 1000, true, true, true);
        Check(Facts(r, 1000 + rules::staleMs + 1) == 0, "no update for staleMs: nothing published (unknown, not bad)");
        Update(r, 1000 + rules::staleMs + 50, true, false, false);
        Check(r.notMovingSince == 1000 + rules::staleMs + 50, "after a gap the clocks start again"); }
    Check(!OffLane(2.9, 1.0) && OffLane(3.1, 1.0) && OffLane(-3.1, 1.0) && !OffLane(9, 0), "off lane: beyond half width + 2 m, either side; no lane width: never");
    Check(Tipped(true, 1) && Tipped(false, .4) && !Tipped(false, .9), "tipped: the rail flag or up-z under 0.5");
    std::printf("selfreport_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
