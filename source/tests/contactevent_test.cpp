// contactevent.h: one event, many listeners; events expire; other vehicles' events are not mine.
#include "contactevent.h"
#include <cstdio>
using namespace contactevent;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    Bus<4> b; Event e; e.vehicle = 0x10; e.at = 1; e.expires = 9; e.x = 5; b.Publish(e);
    Event other = e; other.vehicle = 0x20; b.Publish(other);
    Event out[8];
    Check(b.For(0x10, 2, out, 8) == 1 && out[0].x == 5, "World hears its own vehicle's contact");
    Check(b.For(0x10, 10, out, 8) == 0, "the shape expires");
    Check(b.Count(0x10, 0) == 1 && b.Count(0x20, 0) == 1, "health and the ledger count per vehicle");
    for (int i = 0; i < 6; ++i) { Event f = e; f.at = 3 + i; f.x = 100 + i; b.Publish(f); }
    Check(b.For(0x10, 8.5, out, 8) == 4 && out[0].x == 105, "the ring keeps the newest, newest first");
    Event zero; zero.at = 0; zero.expires = 5; b.Publish(zero); Check(b.For(0, 1, out, 8) == 0, "no vehicle, no listener");
    b.Reset(); Check(b.Count(0x10, 0) == 0 && b.published == 0, "reset on load");
    std::printf("%s (%d failed)\n", failed ? "FAILED" : "ALL PASS", failed);
    return failed ? 1 : 0;
}
