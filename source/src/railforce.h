#pragma once
// Rail force rules (2026-11-05, the user: "force not being in the right direction causing suspension to experience pressure"). One
// physics step of the rail's push on the body's velocity, with rules so it works with the car's wheels and suspension, not against:
//   1. along the road: the target is tilted into the plane the car stands on (its up vector), so on a ramp the car moves along the
//      slope instead of being pushed flat into it;
//   2. little sideways slip: relative to the nose, the sideways part is capped (0.5 m/s + 15 % of the speed); the nose turns first;
//   3. real acceleration: at most kAccel speeding up and kBrake slowing down, instead of a share of the gap per step;
//   4. hands off in the air: jumping, falling or launching, the vertical motion stays the game's; only the horizontal part is eased.
// Pure; tests: tests/railforce_test.cpp.
#include <algorithm>
#include <cmath>

namespace railforce {
constexpr double kAccel = 6.0, kBrake = 9.0;  // m/s^2
struct V3 { double x = 0, y = 0, z = 0; };
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Mul(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline V3 Unit(V3 a) { const double l = Len(a); return l > 1e-9 ? Mul(a, 1 / l) : V3{}; }
inline V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

// Supplemental traction after the native wheel step. It neither overwrites the
// tyres' velocity nor counteracts a requested reorientation.
inline V3 TyreAssist(V3 v, V3 target, V3 normal, double dt) {
    const double want = std::hypot(target.x, target.y);
    if (want <= .25 || dt <= 0) return {};
    const V3 n = Unit(normal);
    if (n.z < .75) return {};
    const V3 dir = Unit(Sub(target, Mul(n, Dot(target, n))));
    const double measured = Dot(v, dir);
    if (!std::isfinite(measured) || measured < -.15) return {};
    const double added = std::clamp(std::min(.65, want) - measured, 0.0, .65*dt);
    return Mul(dir, added);
}

struct Out { V3 v; bool grounded = false, tipped = false; double slip = 0, command = 0; };
// v: the body's velocity now; target: the wanted horizontal velocity (x, y; z ignored); forward / up: the body's axes;
// airborne: the caller knows the car is jumping (the game's vertical boost is held);
// command: the rail's own speed (m/s along the travel direction), kept by the caller between steps, < 0 = not started (taken from
// the car's speed). The acceleration limits apply to it, not to the measured speed: the game's own drive model drags a car with no
// throttle every step, and a limit on the measured speed only ever added back 6 m/s^2 of a stronger drag, so Cruise coasted to a
// stop and stayed there (2026-11-05 18:55). The body is then set to the command each step, whatever the drag took off.
// road (optional, 2026-11-05): the road's own up vector (from the lane's grade). The body's up vector pitches on its suspension: at
// 22 m/s an 8 degree pitch read as 3 m/s of vertical speed ("airborne" on flat road) and tilted the push into and away from the
// road every bounce (the slope hiccups). The body's up still decides tipped.
inline Out Step(V3 v, V3 target, V3 forward, V3 up, double dt, bool airborne, double& command, const V3* road = nullptr)
{
    Out out;
    const V3 body = Unit(up);
    const V3 n = road && Unit(*road).z > 0.5 ? Unit(*road) : body;
    // On its side or roof (up vector under 60 degrees from vertical): no push at all. Pushing a rolled car scraped it along a railing
    // on its side (2026-11-05 09:51 recording).
    if (body.z < 0.5) { out.tipped = true; out.v = v; command = -1; out.command = command; return out; }
    const double speed = std::hypot(target.x, target.y);
    const V3 dir = speed > 1e-6 ? V3{ target.x / speed, target.y / speed, 0 } : Unit(V3{ forward.x, forward.y, 0 });
    // On the road, the velocity lies in the road plane (v . n = 0): its vertical part is what the slope implies at this horizontal
    // speed. Off by 3 m/s or more = launched, falling or jumping.
    const double slopeVz = n.z > 1e-6 ? -(n.x * v.x + n.y * v.y) / n.z : 0.0;
    out.grounded = !airborne && n.z > 0.75 && body.z > 0.75 && std::fabs(v.z - slopeVz) < 3.0;
    // 3. The rail's own speed ramps toward the target at real acceleration limits, never more than 4 m/s ahead of what the car does
    // (a car held against something does not build up a push).
    const double measured = std::max(0.0, v.x * dir.x + v.y * dir.y);
    if (!(command >= 0)) command = measured;
    const double room = (speed > command ? kAccel : kBrake) * std::max(0.0, dt);
    command += std::clamp(speed - command, -room, room);
    command = std::clamp(command, 0.0, measured + 4.0);
    // Off the road and not jumping (thrown off a crest, falling): no more push than the car already has. Holding 22 m/s in the
    // air carried it further off the ramp and it landed hard (2026-11-05 19:37:30: grounded 0 most of a downhill).
    if (!out.grounded && !airborne) command = std::min(command, measured);
    out.command = command;
    V3 want{ dir.x * command, dir.y * command, 0 };
    if (out.grounded && command > 1e-6) {
        // 1. Tilt into the road plane, keeping the speed.
        V3 along = Sub(want, Mul(n, Dot(want, n)));
        along = Mul(Unit(along), command);
        // 2. Cap the sideways part relative to the nose (both in the road plane).
        V3 f = Sub(forward, Mul(n, Dot(forward, n)));
        f = Unit(f);
        if (Len(f) > 0.5) {
            const V3 side = Unit(Cross(n, f));
            const double ahead = Dot(along, f), lateral = Dot(along, side);
            const double cap = 0.5 + 0.15 * command;
            out.slip = lateral;
            along = Add(Mul(f, ahead), Mul(side, std::clamp(lateral, -cap, cap)));
        }
        // Keep the car's own motion across the road plane (suspension), replace its motion in the plane.
        out.v = Add(along, Mul(n, Dot(v, n)));
        return out;
    }
    // In the air (or stopping): horizontal set to the command, the vertical stays the game's.
    out.v = { want.x, want.y, v.z };
    return out;
}
}
