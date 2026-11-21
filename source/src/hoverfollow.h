#pragma once
// D15 (one-authority step 5, notes/redesign-20261112/ONE_AUTHORITY_DESIGN.md): the hover follower. The energy Plan decides a hover
// trajectory (x, y, z, nose, speed, wheels or hover, every 0.2 s); this turns it into the channels the proven hover actuators already take
// (planhover.h feeds the same ones from the legacy plan): the travel bearing, the speed, the height the height hold tracks, where the nose
// points (a held facing while moving = the strafe channel, as CB-4), brake, and when to put the wheels down. It decides nothing: no
// gates, no speed caps, no "is it safe" of its own (the Plan answered all of that); only how to stay on the committed trajectory.
// Law (the wheels follower AF-1 is the model): the car's nearest point on the path gives where along the plan it is (space-indexed, so a
// car that lags still reads the plan for where it is, searched near the plan's age); the travel bearing is the path's direction there
// turned toward the path by the sideways offset; speed, nose, height and wheels are read from the plan `lead` seconds past that point (the
// actuators' lag); the height takes the higher of here and the next second (climb before what is climbed over, come down after it).
// Pure; tests: tests/hoverfollow_test.cpp.
#include "bicycle.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace hoverfollow {
constexpr int kMaxPoints = 32;
constexpr double kPi = 3.14159265358979323846;
inline double Wrap(double a) { return std::remainder(a, 2 * kPi); }

struct Point { double t = 0, x = 0, y = 0, z = 0, heading = 0, travel = 0, speed = 0; int mode = 1; };
struct Path { Point p[kMaxPoints]; int count = 0; double length = 0; };

// The plan's best rollout (states every `dt` s from now) as a follower path. The travel direction comes from the motion (nose + strafe),
// so a strafing plan reads as "move this way, nose that way".
inline Path FromTraj(const bicycle::State* traj, int count, double dt) {
    Path o; count = std::min(count, kMaxPoints);
    for (int i = 0; i < count; ++i) {
        Point& q = o.p[o.count++]; q.t = i * dt; q.x = traj[i].x; q.y = traj[i].y; q.z = traj[i].z; q.heading = traj[i].heading;
        q.speed = std::fabs(traj[i].speed); q.mode = traj[i].mode; q.travel = traj[i].heading;
    }
    // Travel: the motion's direction at each point, from its neighbours on both sides (a one-sided chord lags the nose by half a step's turn
    // on a curve and read as a strafe: hoverfollow_test arc, first try). Standing points keep the last direction found.
    for (int i = 0; i < o.count; ++i) {
        const int a = std::max(0, i - 1), b = std::min(o.count - 1, i + 1);
        const double dx = o.p[b].x - o.p[a].x, dy = o.p[b].y - o.p[a].y;
        if (std::hypot(dx, dy) > 1e-3) o.p[i].travel = std::atan2(dx, dy);
        else if (i > 0) o.p[i].travel = o.p[i - 1].travel;
        if (traj[i].speed < -.05 && std::hypot(dx, dy) <= 1e-3) o.p[i].travel = Wrap(traj[i].heading + kPi);
    }
    for (int i = 1; i < o.count; ++i) o.length += std::hypot(o.p[i].x - o.p[i - 1].x, o.p[i].y - o.p[i - 1].y);
    return o;
}

struct Car { double x = 0, y = 0, z = 0, heading = 0, speed = 0, originAbove = 0; };  // z: the plan's height reference (base); speed >= 0
struct Gains {
    double correction = 3.0;     // m over which a sideways offset is brought back (AF-1's length)
    double lead = .4;            // s: speed, nose, height and wheels are read this far ahead of where the car is (actuator lag)
    double climbAhead = 1.0;     // s: the height is the higher of here and this far ahead
    double splitAngle = .12;     // rad: a plan nose this far off its travel while moving is a held facing (the strafe channel; the Plan strafes only with a facing)
    double holdReach = .5;       // m: holding, farther than this from the plan's point: go there nose first; nearer: stand and turn
    double noseOff = .8;         // rad: not split and the nose this far off where it must go: turn first, do not move (the hover moves along its nose)
    double facingBelow = 1.5;    // m/s: slower than this the nose simply turns to the plan's nose (the hover yaws where it is told)
    double holdLength = .6;      // m: a plan shorter than this is holding position
};
struct Out {
    bool ok = false;
    double bearing = 0;          // where to travel (world heading)
    double speed = 0;            // m/s along it
    double height = 0;           // the height hold's target (plan z + originAbove)
    double nose = 0;             // where the nose should point
    bool split = false;          // the nose is held off the travel while moving: travel by thrust + strafe (CB-4 channel)
    bool facingUsed = false;     // slow: the nose turns to the plan's nose
    bool land = false;           // the plan is on its wheels at the lead time: come down (wheels mode)
    bool brake = false, hold = false;
    double cross = 0, along = 0, heightError = 0, tNear = 0;  // trace: sideways offset (+ = path to the right), arc position, plan time here
    int point = -1;
    const char* why = "";
};

// Interpolate the plan at time t (clamped to its span).
inline Point At(const Path& p, double t) {
    if (p.count == 0) return {};
    if (t <= p.p[0].t) return p.p[0];
    for (int i = 0; i + 1 < p.count; ++i) if (t <= p.p[i + 1].t) {
        const Point& a = p.p[i]; const Point& b = p.p[i + 1]; const double u = (t - a.t) / std::max(1e-9, b.t - a.t);
        Point q = a; q.t = t; q.x += (b.x - a.x) * u; q.y += (b.y - a.y) * u; q.z += (b.z - a.z) * u; q.speed += (b.speed - a.speed) * u;
        q.heading = a.heading + Wrap(b.heading - a.heading) * u; q.travel = a.travel + Wrap(b.travel - a.travel) * u; q.mode = u < .5 ? a.mode : b.mode;
        return q;
    }
    return p.p[p.count - 1];
}

// age: seconds since the plan was made (the car is about where the plan was then); NaN: unknown, the whole path is searched. A 6 s hover
// plan can close on itself (a tight circle), and the nearest point would then be on the far side of it.
inline Out Follow(const Path& p, const Car& c, double age = std::numeric_limits<double>::quiet_NaN(), const Gains& g = {}) {
    Out o; if (p.count < 2) return o;
    // Where the car is along the plan: the nearest point on its segments near the plan's time `age` (standing segments: their start).
    double best = 1e18, along = 0, run = 0, tNear = 0; int seg = 0;
    for (int i = 0; i + 1 < p.count; ++i) {
        const Point& a = p.p[i]; const Point& b = p.p[i + 1];
        if (std::isfinite(age) && (b.t < age - 1.0 || a.t > age + 2.0)) { run += std::hypot(b.x - a.x, b.y - a.y); continue; }
        const double dx = b.x - a.x, dy = b.y - a.y, l2 = dx * dx + dy * dy, l = std::sqrt(l2);
        const double u = l2 < 1e-9 ? 0.0 : std::clamp(((c.x - a.x) * dx + (c.y - a.y) * dy) / l2, 0.0, 1.0);
        const double qx = a.x + u * dx - c.x, qy = a.y + u * dy - c.y, d2 = qx * qx + qy * qy;
        if (d2 < best - 1e-9) { best = d2; seg = i; along = run + u * l; tNear = a.t + u * (b.t - a.t);
            o.cross = l > 1e-6 ? (qx * dy - qy * dx) / l : 0.0; }  // + = the path lies to the right of the car (along the path's direction)
        run += l;
    }
    o.point = seg; o.along = along; o.tNear = tNear;
    const Point lead = At(p, tNear + g.lead), ahead = At(p, tNear + std::max(g.lead, g.climbAhead));
    o.land = lead.mode == 0;
    o.speed = std::max(0.0, lead.speed);
    o.nose = lead.heading;
    if (p.length < g.holdLength) {
        // Holding position (or turning in place): go to the plan's last point, slowly, and turn the nose to the plan's.
        const Point& e = p.p[p.count - 1]; const double d = std::hypot(e.x - c.x, e.y - c.y);
        o.hold = true;
        if (d > g.holdReach) { o.bearing = std::atan2(e.x - c.x, e.y - c.y); o.nose = o.bearing; o.speed = std::min(1.5, d); o.why = "hold: back to the plan's point"; }
        else { o.bearing = c.heading; o.speed = 0; o.facingUsed = true; o.nose = e.heading; o.why = "hold: turning in place"; }  // a standing plan has no place along it: its final nose
    } else {
        // AF-1's law for a body that travels where it is pointed: the path's own direction, turned toward the path by the sideways offset
        // brought back over `correction` metres. (Pure pursuit, the first try, settled 3 m inside a tight hover circle: a body moving straight
        // at a point ahead cuts the curve: hoverfollow_test arc.) The direction is read ahead by the channel's lag: travelling along the nose,
        // the nose trails the bearing it is given (a full lead: arc 0.61 m -> 0.13 m); split, thrust + strafe steer the motion itself (a
        // full lead there cut the strafe's corner: 0.70 m -> 1.14 m), so only a fifth of it.
        const double off = std::fabs(Wrap(lead.heading - lead.travel));
        if (o.speed >= g.facingBelow && off > g.splitAngle) o.split = true;
        const Point tangent = At(p, tNear + g.lead * (o.split ? .2 : 1.0));
        o.bearing = Wrap(tangent.travel + std::atan(o.cross / g.correction));
        if (o.split) o.why = "follow, nose held (strafe)";
        else if (o.speed < g.facingBelow && std::fabs(Wrap(lead.heading - o.bearing)) > g.splitAngle) { o.facingUsed = true; o.why = "follow slow, nose to the plan's"; }
        else { o.nose = o.bearing; o.why = "follow"; }
    }
    // Height: climb early, come down late; landing: the plan's floor where it lands.
    // Not split, the hover travels along its nose: with the nose far off where it must go, turn first (moving would carry it off the plan).
    if (!o.split && std::fabs(Wrap((o.facingUsed && !o.hold ? o.nose : o.bearing) - c.heading)) > g.noseOff && !(o.hold && o.facingUsed)) { o.speed = 0; o.why = "turning to the plan first"; }
    o.height = (o.land ? lead.z : std::max(lead.z, ahead.mode == 1 ? ahead.z : lead.z)) + c.originAbove;
    o.heightError = c.z + c.originAbove - o.height;
    o.brake = o.speed < .1 || c.speed > o.speed + .5;
    o.ok = true;
    return o;
}
}
