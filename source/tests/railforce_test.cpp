// railforce.h: the rail's push follows the road, limits slip and acceleration on its own commanded speed, and leaves jumps and
// rolled cars alone (2026-11-05).
#include "railforce.h"
#include <cmath>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* what) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", what); } }
}

int main()
{
    using namespace railforce;
    const double dt = 1.0 / 60;
    const V3 north{ 0, 1, 0 }, flatUp{ 0, 0, 1 };
    {   double c = -1;
        auto o = Step({ 0, 0, 0 }, { 0, 20, 0 }, north, flatUp, dt, false, c);
        check(o.grounded && std::fabs(o.v.y - kAccel * dt) < 1e-9, "from rest: speeds up at 6 m/s^2, not a jump to the target");
        V3 v{ 0, 0, 0 }; c = -1; for (int i = 0; i < 60; ++i) v = Step(v, { 0, 20, 0 }, north, flatUp, dt, false, c).v;
        check(std::fabs(v.y - 6.0) < 1e-6, "one second: 6 m/s");
        c = -1; auto stop = Step({ 0, 10, 0 }, { 0, 0, 0 }, north, flatUp, dt, false, c);
        check(std::fabs(stop.v.y - (10 - kBrake * dt)) < 1e-9, "slowing down: 9 m/s^2");
    }
    {   // The game's own drive model drags the car every step (no throttle): the rail still reaches its target. The old rule (a limit on
        // the measured speed) only added back 6 m/s^2 and the car coasted to a stop (2026-11-05 18:55).
        V3 v{ 0, 8, 0 }; double c = -1;
        for (int i = 0; i < 600; ++i) { v = Step(v, { 0, 22, 0 }, north, flatUp, dt, false, c).v; v.y = std::max(0.0, v.y - 0.25); }
        check(v.y > 21, "dragged by the game: reaches about the target speed anyway");
    }
    {   // Held against a wall (the car does not move): the command never runs more than 4 m/s ahead.
        double c = -1; V3 v{ 0, 0, 0 };
        for (int i = 0; i < 300; ++i) Step(v, { 0, 22, 0 }, north, flatUp, dt, false, c);
        check(c <= 4.0 + 1e-9, "held in place: the push stays small");
    }
    {   // A 20 degree ramp climbing north: up vector tilted back.
        const double a = 20 * 3.141592653589793 / 180;
        const V3 up{ 0, -std::sin(a), std::cos(a) }, fwd{ 0, std::cos(a), std::sin(a) };
        V3 v{ 0, 10 * std::cos(a), 10 * std::sin(a) }; double c = -1;
        for (int i = 0; i < 120; ++i) v = Step(v, { 0, 10, 0 }, fwd, up, dt, false, c).v;
        check(std::fabs(v.z / v.y - std::tan(a)) < 1e-3, "on a ramp the velocity runs along the slope (no push into the road)");
        check(std::fabs(Len(v) - 10) < 0.05, "and keeps the wanted speed along it");
    }
    {   // Wanted direction 40 degrees off the nose: the sideways part is capped.
        const double sp = 10; double c = 10;
        auto o = Step({ 0, 10, 0 }, { sp * std::sin(0.7), sp * std::cos(0.7), 0 }, north, flatUp, 1.0, false, c);
        check(std::fabs(o.v.x) <= 0.5 + 0.15 * sp + 1e-9, "sideways slip capped (0.5 + 15 % of the speed)");
    }
    {   // In the air (a jump): vertical untouched.
        double c = -1;
        auto o = Step({ 0, 5, 7 }, { 0, 10, 0 }, north, flatUp, dt, true, c);
        check(!o.grounded && std::fabs(o.v.z - 7) < 1e-12 && o.v.y >= 5, "jumping: the game's vertical motion is kept");
        c = -1; auto fall = Step({ 0, 5, -9 }, { 0, 10, 0 }, north, flatUp, dt, false, c);
        check(!fall.grounded && std::fabs(fall.v.z + 9) < 1e-12, "falling fast: not treated as on the road");
    }
    {   // Rolled onto its side: no push at all.
        double c = 5;
        auto o = Step({ 0, 3, 0 }, { 0, 20, 0 }, north, { 1, 0, 0.2 }, dt, false, c);
        check(o.tipped && o.v.y == 3 && o.v.x == 0 && c < 0, "on its side: the rail does not push (and restarts its command)");
    }
    {   // Road normal (2026-11-05): the body pitched 8 degrees on its suspension at 22 m/s on flat road is still on the road, and the push
        // stays flat (the road's plane), not tilted into the body's.
        const double a = 8 * 3.14159265358979 / 180;
        const V3 pitched{ 0, -std::sin(a), std::cos(a) }, fwd{ 0, std::cos(a), std::sin(a) };
        double c = 22; auto bodyOnly = Step({ 0, 22, 0 }, { 0, 22, 0 }, fwd, pitched, dt, false, c);
        check(!bodyOnly.grounded, "body plane only: the pitch reads as airborne (the old fault)");
        c = 22; const V3 road{ 0, 0, 1 }; auto o = Step({ 0, 22, 0 }, { 0, 22, 0 }, fwd, pitched, dt, false, c, &road);
        check(o.grounded && std::fabs(o.v.z) < 1e-6, "road plane: on the road, pushed level along it");
        // In the air and not jumping: never more push than the car has.
        c = 22; auto air = Step({ 0, 18, -6 }, { 0, 22, 0 }, north, flatUp, dt, false, c, &road);
        check(!air.grounded && air.v.y <= 18 + 1e-9, "thrown off a crest: no extra push in the air");
    }
    std::printf("railforce: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
