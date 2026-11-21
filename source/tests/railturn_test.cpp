// railturn.h: rail drive turns, look-ahead bearing and corner speed (2026-11-05).
#include "laneline.h"
#include "railturn.h"
#include <cmath>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
auto at = [](const FvoLaneGuidanceV1& l, double s) { return laneline::At(l, s); };
}

int main()
{
    const double deg = railturn::kPi / 180;
    FvoLaneGuidanceV1 straight; straight.count = 2; straight.points[0] = { 0, 0, 0 }; straight.points[1] = { 0, 300, 0 };
    auto p = railturn::Pursue(straight, at, 50, 0, 50, 20, 0, 3.75, 1.2);
    check(p.ok && std::fabs(p.bearing) < 1e-6 && p.cornerCap > 1e8, "straight lane: straight ahead, no corner limit");
    p = railturn::Pursue(straight, at, 50, 2, 50, 10, 0, 3.75, 1.2);
    check(p.bearing < 0 && p.bearing > -30 * deg, "2 m right of the line: aims back gently, not sharply");

    // 90 degree corner (north then east) at 100 m: Roadrunner's points 17-19.
    FvoLaneGuidanceV1 corner; corner.count = 3; corner.points[0] = { 0, 0, 0 }; corner.points[1] = { 0, 100, 0 }; corner.points[2] = { 100, 100, 0 };
    const auto near = railturn::Pursue(corner, at, 98, 0, 98, 5, 0, 3.75, 1.2);
    check(near.cornerCap > 3.5 && near.cornerCap < 7.5, "at a 90 degree corner: about 4-7 m/s (the wheel rule allowed 1.5)");
    check(near.bearing > 20 * deg && near.bearing < 80 * deg, "before the corner point the car already aims into the turn");
    const auto far = railturn::Pursue(corner, at, 20, 0, 20, 20, 0, 3.75, 1.2);
    check(far.cornerCap > 20, "80 m before the corner: full speed still allowed (braking starts in time)");
    const auto mid = railturn::Pursue(corner, at, 70, 0, 70, 15, 0, 3.75, 1.2);
    check(mid.cornerCap < far.cornerCap && mid.cornerCap > near.cornerCap, "the limit falls smoothly while approaching");

    // A passing offset shifts the aim sideways (right = + offset, the lane driver's convention).
    p = railturn::Pursue(straight, at, 50, 0, 50, 10, 1.5, 3.75, 1.2);
    check(p.bearing > 0, "passing on the right: aims right of the centre line (east, heading > 0)");
    p = railturn::Pursue(straight, at, 50, 0, 50, 10, -1.5, 3.75, 1.2);
    check(p.bearing < 0, "a negative offset: aims left");

    FvoLaneGuidanceV1 none;
    check(!railturn::Pursue(none, at, 0, 0, 0, 5, 0, 3.75, 1.2).ok, "no lane: no plan");
    std::printf("railturn: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
