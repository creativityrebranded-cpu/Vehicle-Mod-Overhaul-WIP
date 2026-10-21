#include "../src/travelmode.h"
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

int main()
{
    using namespace travelmode;
    Inputs base; base.tripDist = 400; base.profile = Default::Drive;
    check(Decide(base).mode == Mode::Drive, "a drive profile drives a mid-length trip");
    Inputs in = base; in.tripDist = 800;
    check(Decide(in).mode == Mode::Drive, "driving is preferred: a long trip drives");
    in.profile = Default::Auto;
    check(Decide(in).mode == Mode::Drive, "with no profile and no reason to fly, it drives");
    in.profile = Default::Hover;
    check(Decide(in).mode == Mode::Hover, "a hover profile (courier, scout) still flies a long trip");
    in = base; in.tripDist = 120; in.profile = Default::Auto;
    check(Decide(in).mode == Mode::Drive, "a short trip drives");
    in = base; in.tripDist = 120; in.profile = Default::Hover;
    check(Decide(in).mode == Mode::Hover, "a hover profile keeps hovering on a short trip");
    in = base; in.waterAhead = true;
    check(Decide(in).mode == Mode::Hover && Decide(in).blocking, "water ahead: hover at once");
    in = base; in.clearNeed = 6; in.jumpHeight = 4;
    check(Decide(in).mode == Mode::Hover && Decide(in).blocking, "an obstacle taller than a jump: hover");
    in.clearNeed = 2;
    check(Decide(in).mode == Mode::Drive, "an obstacle it can jump: keep driving");
    in.clearNeed = 41;
    check(Decide(in).mode == Mode::Drive, "a building wall (need over 25 m) is steered around, not flown over");
    in = base; in.roughAhead = true;
    check(Decide(in).mode == Mode::Hover, "rough ground ahead: hover");
    in = base; in.fighting = true;
    check(Decide(in).mode == Mode::Hover, "a fight: hover");
    in = base; in.goalAbove = 30;
    check(Decide(in).mode == Mode::Drive, "a goal 30 m higher: still drives");
    in.goalAbove = 80;
    check(Decide(in).mode == Mode::Hover, "a goal 75 m+ higher: hover");
    in.goalAbove = -200;
    check(Decide(in).mode == Mode::Drive, "a goal far below: drives down (z only, upward counts)");
    in = base; in.tripDist = 900; in.inSettlement = true;
    check(Decide(in).mode == Mode::Drive, "inside a settlement: drive even for a long trip");
    in = base; in.damaged = true; in.waterAhead = true; in.fighting = true;
    check(Decide(in).mode == Mode::Drive, "damaged: the hover fails, it drives whatever else is true");
    in = base; in.tripDist = 50; in.catching = true; in.damaged = true;
    check(Decide(in).mode == Mode::Hover && Decide(in).blocking, "catching you from below: hover at once, before anything else");
    in = base; in.profile = Default::CopyPlayer; in.playerHovering = true;
    check(Decide(in).mode == Mode::Hover, "an escort copies a hovering player");
    in.playerHovering = false;
    check(Decide(in).mode == Mode::Drive, "an escort copies a driving player");

    // The hold: settle time, minimum time, blocking reasons.
    Hold h;
    Step(h, { Mode::Drive, false, "" }, 1000);
    check(h.mode == Mode::Drive, "the first reason sets the mode");
    check(!Step(h, { Mode::Hover, false, "" }, 3000) && h.mode == Mode::Drive, "a new reason waits for the minimum time");
    Step(h, { Mode::Drive, false, "" }, 10000);
    check(!Step(h, { Mode::Hover, false, "" }, 11500) && h.mode == Mode::Drive, "after the minimum time a fresh reason still waits to settle");
    check(Step(h, { Mode::Hover, false, "" }, 13600) && h.mode == Mode::Hover, "then it switches");
    check(!Step(h, { Mode::Drive, false, "" }, 14000), "no flip-flop straight back");
    check(Step(h, { Mode::Drive, true, "" }, 14100) && h.mode == Mode::Drive, "a blocking reason switches at once");
    Hold b; Step(b, { Mode::Drive, false, "" }, 0);
    Step(b, { Mode::Hover, true, "" }, 20000);
    check(b.mode == Mode::Hover, "a blocking reason switches");
    check(!Step(b, { Mode::Drive, false, "" }, 35000) && b.mode == Mode::Hover, "and its mode is kept 30 s, past what forced it");
    check(Step(b, { Mode::Drive, false, "" }, 52000) && b.mode == Mode::Drive, "after the 30 s the normal rule returns");
    Hold f; Step(f, { Mode::Drive, false, "" }, 0);
    for (std::uint64_t t = 100; t < 20000; t += 100) Step(f, { (t / 100) % 2 ? Mode::Hover : Mode::Drive, false, "" }, t);
    check(f.mode == Mode::Drive, "a flickering reason never switches");
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
