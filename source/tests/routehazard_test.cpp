// Hazard boxes (2026-11-21): file key round trip, unreadable text kept, old routes unchanged, geometry, detour and stop-before, suggestions.
#include "routehazard.h"
#include <cstdio>
int main() {
    using namespace routehazard;
    int total = 0, failed = 0;
    auto check = [&](bool good, const char* name) { ++total; if (!good) { ++failed; std::printf("FAIL %s\n", name); } };
    // File key.
    auto p = Read("10 20 5 1.5 3 2 90 ; -4 8 0 2 2 2 0");
    check(p.boxes.size() == 2 && p.unsupported.empty() && p.boxes[0].hy == 3 && p.boxes[0].yaw == 90, "read two boxes");
    const auto text = Lines(p); check(text == "hazard_boxes = 10 20 5 1.5 3 2 90 ; -4 8 0 2 2 2 0\n", "written back the same");
    check(Lines(Read("")).empty() && Read("").boxes.empty(), "old route: no key, no boxes");
    auto bad = Read("1 2 3 0 0 0 0"); check(bad.boxes.empty() && Lines(bad) == "hazard_boxes = 1 2 3 0 0 0 0\n", "too small kept whole");
    auto fut = Read("1 2 3 1 1 1 0 extra"); check(!fut.unsupported.empty() && Lines(fut) == "hazard_boxes = 1 2 3 1 1 1 0 extra\n", "future text kept");
    check(!Read("nan 2 3 1 1 1 0").unsupported.empty(), "non-finite refused");
    auto s = Read("1 2 3 1 1 1 0"); Shift(s, 10, 20, 30); check(s.boxes[0].x == 11 && s.boxes[0].y == 22 && s.boxes[0].z == 33, "boxes follow an anchored city");
    check(Clamped(Box{0,0,0,0,999,1,-90}).hx == kMinHalf && Clamped(Box{0,0,0,0,999,1,-90}).yaw == 270, "clamp and wrap yaw");
    // Geometry: box centred (0, 50), 2 across x 4 along, heights 0..4, no rotation.
    Plan g; g.boxes.push_back({0, 50, 2, 2, 4, 2, 0});
    check(Touches(g.boxes[0], 0, 50, 0, 1, 0), "inside touches");
    check(!Touches(g.boxes[0], 3.5, 50, 0, 1, 1.0), "a body 1 m wide 3.5 m out clears a box 2 m half-wide");
    check(Touches(g.boxes[0], 2.9, 50, 0, 1, 1.0), "a body 1 m wide 2.9 m out touches");
    check(!Touches(g.boxes[0], 0, 50, 5, 6, 1.0) && !Touches(g.boxes[0], 0, 50, -3, -1, 1.0), "the lane above or below is not blocked");
    Box turned{0, 0, 0, 1, 5, 1, 90};  // along axis points east
    check(Touches(turned, 4.5, 0, 0, 0, 0) && !Touches(turned, 0, 4.5, 0, 0, 0), "yaw 90 puts the long side east");
    // Path along north at x = 0 from y = 0 to 100.
    std::vector<P3> path{{0, 0, 0}, {0, 100, 0}};
    const double hit = FirstHit(g, path, 0, 1.0, 2.0);
    check(hit > 43 && hit < 47.5, "first contact before the box");
    check(StopBefore(hit, 3.0, 2.0) < hit - 4.9, "stops short by half length and gap");
    const double off = DetourOffset(g, path, 1.0, 2.0, 8.0);
    check(off != 0 && std::fabs(off) >= 2.9 && std::fabs(off) <= 4.0, "detour found beside the box");
    check(DetourOffset(g, path, 1.0, 2.0, 1.5) == kNoDetour, "no detour inside the allowed width: stop");
    Plan above; above.boxes.push_back({0, 50, 12, 2, 4, 2, 0});
    check(FirstHit(above, path, 0, 1.0, 2.0) < 0, "a deck above is open to a car on the lower deck");
    // The virtual driver.
    auto line = [](double s) { return P3{0, s, 0}; };
    auto adv = Advise(g, line, 0, 100, 15, 1.0, 2.5, 2.0, 0, .1);
    check(adv.detour || adv.offset != 0 || adv.blocked == false, "advice computed");
    double eased = 0; for (int i = 0; i < 40; ++i) eased = Advise(g, line, i * .5, 100, 15, 1.0, 2.5, 2.0, eased, .1).offset;
    check(std::fabs(eased) >= 3.0, "the car eases sideways to the detour");
    Plan wide; wide.boxes.push_back({0, 50, 2, 9, 4, 2, 0});  // 18 m wide across the whole allowed shift
    auto stop = Advise(wide, line, 0, 100, 15, 1.0, 2.5, 2.0, 0, .1);
    check(stop.blocked && stop.stopIn > 30 && stop.stopIn < 45 && stop.speedCap < 20, "no way round: stop short of the box");
    auto atBox = Advise(wide, line, 44, 100, 15, 1.0, 2.5, 2.0, 0, .1);
    check(atBox.blocked && atBox.speedCap < .01, "at the stop point the speed cap is zero");
    check(Advise(above, line, 0, 100, 15, 1.0, 2.5, 2.0, 0, .1).speedCap > 1e8, "a deck above does not slow the lower car");
    check(Advise(Plan{}, line, 0, 100, 15, 1.0, 2.5, 2.0, 2.0, .1).offset < 2.0, "no boxes: the offset eases back to the line");
    // Suggestions merge.
    std::vector<Box> sug; check(Merge(sug, {0, 50, 2, 1, 1, 1, 0}), "first sighting kept");
    check(!Merge(sug, {.2, 50.1, 2, 1, 1, 1, 0}) && sug.size() == 1, "same object seen again is one suggestion");
    check(Merge(sug, {30, 50, 2, 1, 1, 1, 0}) && sug.size() == 2, "a different object is a second");
    std::printf("%d checks, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
