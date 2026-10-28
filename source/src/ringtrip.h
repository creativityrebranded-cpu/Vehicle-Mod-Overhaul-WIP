#pragma once
#include <algorithm>
#include <cmath>
#include "ringlogic.h"

// Scripted ring traffic (2026-10-27, the user: "a scripted ring route" for ambient traffic, using the parked vehicles of
// Parked Vehicles and Carfield). One trip is a fixed procedure on the saved ring (ringroad.ini, ringlogic.h):
//   Climb    straight up over its parking place to the lane's height (clear of the towers before moving across);
//   Join     across to the nearest point of its lane;
//   Circle   one full lap along the lane (layer 0 clockwise, layer 1 the other way, 40 m above), following a point
//            kLead metres ahead on the lane;
//   Exit     at the lane point nearest home after the lap, across to above home at lane height;
//   Descend  the caller then parks it at home (the existing parking: land, driver out).
// Pure: the caller flies the goal each frame with the autopilot and reports the vehicle's position.
namespace ringtrip {
enum class Leg { Climb, Join, Circle, Exit, Done };
inline const char* Name(Leg l)
{
    switch (l) {
    case Leg::Climb: return "climbing to the lane";
    case Leg::Join: return "joining the lane";
    case Leg::Circle: return "circling";
    case Leg::Exit: return "leaving the lane";
    default: return "done";
    }
}

constexpr double kLead = 70.0;       // m ahead along the lane
constexpr double kReach = 25.0;      // m: a waypoint this close is reached
constexpr double kClimbSlack = 8.0;  // m under the lane height that counts as up

struct Trip {
    Leg leg = Leg::Climb;
    int layer = 0;
    double homeX = 0, homeY = 0;
    double lastAngle = 0, travelled = 0;  // rad along its own direction
    bool angleKnown = false;
};
struct Goal { double x = 0, y = 0, z = 0, stop = 6.0; bool passThrough = false; };

// The ring angle (ringlogic::At's parameter) of the lane point nearest (x, y): undo the ring's rotation, then the aspect.
inline double AngleOf(const ringlogic::Ring& r, double x, double y)
{
    const double dx = x - r.cx, dy = y - r.cy;
    const double c = std::cos(r.rotation), s = std::sin(r.rotation);
    // ringlogic::At turns ring-frame (lx, ly) by: x = lx c + ly s, y = -lx s + ly c. Inverse: lx = x c - y s, ly = x s + y c.
    const double lx = dx * c - dy * s, ly = dx * s + dy * c;
    return std::atan2(lx / std::max(r.aspect, 1e-6), ly);
}
inline double Direction(int layer) { return layer == 1 ? -1.0 : 1.0; }  // layer 0 clockwise: the angle grows
inline ringlogic::Point Lane(const ringlogic::Ring& r, int layer, double angle) { return ringlogic::OnLayer(ringlogic::At(r, angle), r, layer); }

inline Trip Start(int layer, double homeX, double homeY) { Trip t; t.layer = layer & 1; t.homeX = homeX; t.homeY = homeY; return t; }

// This frame's goal, advancing the leg from where the vehicle is.
inline Goal Step(const ringlogic::Ring& r, Trip& t, double x, double y, double z)
{
    Goal g;
    const double laneZ = r.z + (t.layer == 1 ? r.gap : 0.0);
    if (t.leg == Leg::Climb) {
        if (z >= laneZ - kClimbSlack) t.leg = Leg::Join;
        else { g.x = t.homeX; g.y = t.homeY; g.z = laneZ; g.stop = 4.0; return g; }
    }
    if (t.leg == Leg::Join) {
        const auto p = Lane(r, t.layer, AngleOf(r, x, y));
        if (std::hypot(p.x - x, p.y - y) < kReach && std::fabs(z - laneZ) < kReach) {
            t.leg = Leg::Circle;
            t.lastAngle = AngleOf(r, x, y);
            t.angleKnown = true;
            t.travelled = 0;
        }
        else { g.x = p.x; g.y = p.y; g.z = laneZ; g.passThrough = true; return g; }
    }
    if (t.leg == Leg::Circle) {
        const double a = AngleOf(r, x, y);
        if (t.angleKnown) {
            const double step = ringlogic::Wrap(a - t.lastAngle) * Direction(t.layer);
            if (step > 0 && step < 0.5) t.travelled += step;  // forward progress only; a jump is not a lap
        }
        t.lastAngle = a;
        t.angleKnown = true;
        const double homeAngle = AngleOf(r, t.homeX, t.homeY);
        if (t.travelled >= 2.0 * ringlogic::kPi && std::fabs(ringlogic::Wrap(a - homeAngle)) < 0.2) t.leg = Leg::Exit;
        else {
            // Lead point: kLead metres along the lane (the mean radius turns metres into angle).
            const double mean = r.radius * (1.0 + r.aspect) * 0.5;
            const auto p = Lane(r, t.layer, a + Direction(t.layer) * kLead / std::max(mean, 1.0));
            g.x = p.x; g.y = p.y; g.z = laneZ; g.passThrough = true;
            return g;
        }
    }
    if (t.leg == Leg::Exit) {
        if (std::hypot(t.homeX - x, t.homeY - y) < kReach) t.leg = Leg::Done;
        else { g.x = t.homeX; g.y = t.homeY; g.z = laneZ; g.stop = 6.0; return g; }
    }
    g.x = t.homeX; g.y = t.homeY; g.z = laneZ; g.stop = 6.0;
    return g;
}
}
