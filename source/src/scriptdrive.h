#pragma once
// Script drive (2026-11-22, the user: "script mode turns off planning and only just drives in the direction of the path", "only the center
// line", "and no obstacle avoidance"; hazard boxes are the one thing it goes round: "the hazard boxes are the only thing there so that we
// can make it dodge those").
//
// Place in notes/VEHICLE_DECISION_DESIGN_20261106.md: a Plan speaker that replaces the energy plan for a lane car on wheels while Script
// mode is on (Route -> Plan -> Controller order kept: the route's centre line IS the plan; the existing path follower and pedals are the
// controller). No search, no candidates, no avoidance, no recovery, no arbiter hold. The line is shifted only round hazard boxes
// (routehazard::Advise, the same detour the rail and the virtual actuator use). The speed is the lane's, with one stop rule: a vehicle or
// a person whose known box touches the line ahead (within the car's own width) stops the car short of it; it never steers round them.
// Pure logic: no game reads, tested offline (tests/scriptdrive_test.cpp).
// CW-1 step 3 (2026-11-22): the lateral line now comes from the centre-line well (centrewell.h; in Script mode its only pushes are hazard
// boxes). What stays here is the speed rule (Speed): lane speed, hold, stop line, bend, caps, and the wait for vehicles and people on the path.
#include <algorithm>
#include <cmath>

namespace scriptdrive {

namespace rules {
inline constexpr double kBehind = 2.0;      // m of line kept behind the car (the follower starts from where the car is)
inline constexpr double kAheadMin = 20.0;   // m of line ahead at least ...
inline constexpr double kAheadMax = 60.0;   // ... and at most
inline constexpr double kAheadSeconds = 3.0;  // ... else this many seconds at the car's speed
inline constexpr double kStopGap = 2.0;     // m left between the car's nose and a vehicle or person it waits for
inline constexpr double kStopLineGap = 3.0; // m short of a lane stop line (as the virtual actuator)
inline constexpr double kWidthMargin = 0.3; // m added to the car's half width when a blocker touches the line
inline constexpr double kStep = 1.0;        // m between line samples
}

struct Point { double x = 0, y = 0; };
// A vehicle or a person from its known position (World's store): its box, heading clockwise from north.
struct Blocker { double x = 0, y = 0, heading = 0, halfLength = 0.4, halfWidth = 0.4; bool person = false; };
struct Car {
    double along = 0;        // the car's station on the lane line
    double speed = 0;        // its forward speed now (m/s)
    double halfWidth = 1.4, halfLength = 3.0;
    double brake = 4.0;      // m/s^2 it can brake
    double want = 0;         // the lane's speed for it
    bool held = false;       // a lane hold
    double stopDistance = -1;  // m to a lane stop line (<0: none)
    double end = 1e9;        // the route's end station (open routes; 1e9: none)
    double lookEnd = 1e9;    // the last station of line this car can see (its lane window)
    double bendCap = 0;      // the bend speed the rail measured (<=0: none)
};
struct Out {
    double speed = 0;
    double stopIn = -1;      // m to the first blocker on the line (<0: none)
    int blocker = -1;        // its index
    double ahead = 0;        // m of line looked at
    const char* why = "script_follow_centre_line";
};

inline double Ahead(double speed) { return std::clamp(std::fabs(speed) * rules::kAheadSeconds, rules::kAheadMin, rules::kAheadMax); }

// The centre line point at station s, shifted `offset` metres to the right of travel (a hazard detour; 0 = the centre line).
template <class At>
inline Point Line(At&& at, double s, double offset) {
    const auto p = at(s), q = at(s + 1.0);
    Point o{ double(p.x), double(p.y) };
    if (offset != 0.0) {
        const double dx = double(q.x) - double(p.x), dy = double(q.y) - double(p.y), len = std::hypot(dx, dy);
        if (len > 1e-6) { o.x += dy / len * offset; o.y -= dx / len * offset; }
    }
    return o;
}

// Does a blocker's box touch the line point p, widened by the car's half width?
inline bool Touches(const Blocker& b, const Point& p, double halfWidth) {
    const double dx = p.x - b.x, dy = p.y - b.y, s = std::sin(b.heading), c = std::cos(b.heading);
    const double along = dx * s + dy * c, across = dx * c - dy * s, reach = halfWidth + rules::kWidthMargin;
    return std::fabs(along) <= b.halfLength + reach && std::fabs(across) <= b.halfWidth + reach;
}

// The speed along the line: lane speed, lane hold, stop line, open route end, the bend cap, the hazard cap, and the first vehicle or person
// on the line ahead. Never a reverse, never a turn.
template <class At>
inline Out Speed(const Car& c, At&& at, double hazardOffset, double hazardCap, const Blocker* blockers, int count) {
    Out o;
    o.ahead = std::min(Ahead(c.speed), std::max(0.0, std::min(c.end, c.lookEnd) - c.along));
    const double brake = std::max(0.5, c.brake);
    double v = c.held ? 0.0 : std::max(0.0, c.want);
    if (c.held) o.why = "script_lane_hold";
    if (c.stopDistance >= 0) { const double cap = std::sqrt(2.0 * brake * std::max(0.0, c.stopDistance - rules::kStopLineGap)); if (cap < v) { v = cap; o.why = "script_stop_line"; } }
    if (c.end < 1e8) { const double cap = std::sqrt(2.0 * brake * std::max(0.0, c.end - c.along - 2.0)); if (cap < v) { v = cap; o.why = "script_route_end"; } }
    if (c.bendCap > 0 && c.bendCap < v) { v = c.bendCap; o.why = "script_bend"; }
    if (hazardCap < v) { v = std::max(0.0, hazardCap); o.why = "script_hazard_box"; }
    for (double d = 0; d <= o.ahead + 1e-6 && o.stopIn < 0; d += rules::kStep) {
        const auto p = Line(at, c.along + d, hazardOffset);
        for (int k = 0; k < count && blockers; ++k)
            if (Touches(blockers[k], p, c.halfWidth)) { o.stopIn = d; o.blocker = k; break; }
    }
    if (o.stopIn >= 0) {
        const double cap = std::sqrt(2.0 * brake * std::max(0.0, o.stopIn - c.halfLength - rules::kStopGap));
        if (cap < v) { v = cap; o.why = blockers[o.blocker].person ? "script_wait_for_person" : "script_wait_for_vehicle"; }
    }
    o.speed = v;
    return o;
}

}  // namespace scriptdrive
