// SD-2 Script drive rules (src/scriptdrive.h): centre line, lane speed, waits for vehicles and people on the line, never turns or reverses.
#include <string_view>
#include "scriptdrive.h"
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* what) { if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
struct P { double x, y, z; };
// A straight lane north along x = 0, and a bend.
P Straight(double s) { return { 0.0, s, 0.0 }; }
}

int main() {
    using namespace scriptdrive;
    Car c; c.along = 10; c.speed = 10; c.want = 15; c.brake = 4; c.halfWidth = 1.2; c.halfLength = 2.5;

    { const auto o = Speed(c, Straight, 0.0, 1e9, nullptr, 0); Check(std::fabs(o.speed - 15) < 1e-9, "open line: lane speed");
      Check(o.stopIn < 0, "open line: nothing ahead"); Check(o.ahead >= rules::kAheadMin && o.ahead <= rules::kAheadMax, "look ahead in range"); }

    { Car h = c; h.held = true; Check(Speed(h, Straight, 0.0, 1e9, nullptr, 0).speed == 0.0, "lane hold stops"); }

    { Car s = c; s.stopDistance = 3.0; Check(Speed(s, Straight, 0.0, 1e9, nullptr, 0).speed == 0.0, "at the stop line: stand"); }

    // A vehicle on the line 20 m ahead: wait behind it.
    { Blocker b{ 0.0, 30.0, 0.0, 2.5, 1.2, false }; const auto o = Speed(c, Straight, 0.0, 1e9, &b, 1);
      Check(o.stopIn >= 0 && o.stopIn < 20.0, "vehicle on the line is seen"); Check(o.speed < 15.0, "slows for it");
      Check(std::string_view(o.why) == "script_wait_for_vehicle", "reason names a vehicle"); }

    // A vehicle in the next lane (4 m to the side): not on the line, not a stop (no avoidance either way).
    { Blocker b{ 4.5, 30.0, 0.0, 2.5, 1.2, false }; const auto o = Speed(c, Straight, 0.0, 1e9, &b, 1);
      Check(o.stopIn < 0 && std::fabs(o.speed - 15) < 1e-9, "vehicle beside the line: keeps going"); }

    // A person 3 m ahead of the nose: stand.
    { Blocker b{ 0.3, 15.5, 0.0, 0.3, 0.3, true }; const auto o = Speed(c, Straight, 0.0, 1e9, &b, 1);
      Check(o.speed == 0.0, "person right ahead: stands"); Check(std::string_view(o.why) == "script_wait_for_person", "reason names a person"); }

    // Hazard detour: the line shifts right, a blocker on the shifted line is seen, one on the centre line is not.
    { const auto p = Line(Straight, 20.0, 2.0); Check(std::fabs(p.x - 2.0) < 1e-9 && std::fabs(p.y - 20.0) < 1e-9, "offset is to the right of travel");
      Blocker onCentre{ 0.0, 30.0, 0.0, 0.3, 0.3, true }; Check(Speed(c, Straight, 3.0, 1e9, &onCentre, 1).stopIn < 0, "shifted line passes a person on the centre line");
      Blocker onShift{ 3.0, 30.0, 0.0, 0.3, 0.3, true }; Check(Speed(c, Straight, 3.0, 1e9, &onShift, 1).stopIn >= 0, "shifted line sees a person on it"); }

    // Hazard cap and bend cap win when lower.
    { Check(std::fabs(Speed(c, Straight, 0.0, 5.0, nullptr, 0).speed - 5.0) < 1e-9, "hazard cap"); Car b = c; b.bendCap = 8.0;
      Check(std::fabs(Speed(b, Straight, 0.0, 1e9, nullptr, 0).speed - 8.0) < 1e-9, "bend cap"); }

    // The look ahead stops at the end of the visible window.
    { Car w = c; w.lookEnd = 15.0; Check(Speed(w, Straight, 0.0, 1e9, nullptr, 0).ahead <= 5.0 + 1e-9, "window end limits the look"); }

    // Never negative.
    { Car n = c; n.want = -3; Check(Speed(n, Straight, 0.0, 1e9, nullptr, 0).speed == 0.0, "never a reverse"); }

    std::printf("scriptdrive_test: %s (%d failures)\n", failures ? "FAILED" : "passed", failures);
    return failures ? 1 : 0;
}
