#pragma once
#include <algorithm>
#include <cmath>
#include "ringlogic.h"

// City ring traffic, the schedule (2026-10-24, the user: always-on traffic that is the same in every save). Every vehicle's
// place is a pure function of the game time, so loading any save at the same time shows the same picture.
//
// Vehicle k flies ring layer k % 2 (0: the lower, clockwise; 1: the upper, counter-clockwise) in a fixed slot: its ring angle
// advances at a constant rate from its own phase, evenly spaced from the others on its ring. Each vehicle has a home: its own
// parking spot (the user's recorded New Atlantis spots, the lower ring's on the city's lower level and the upper ring's on the
// upper level; trafficspots.h), and its exit is the ring exit nearest to that spot. On every kVisitEvery-th lap (a different lap
// for each vehicle) it leaves the ring when its slot passes that exit, flies down to its spot, parks, and flies back up so that
// it rejoins exactly when its slot comes round to the exit one lap later. So no two vehicles ever share a spot or a ring slot.
// A vehicle without a home spot stays on the ring. Pure math: tests/ringschedule_test.cpp.
namespace ringschedule {
constexpr int kVehicles = 32;
constexpr double kSpeed = 30;          // m/s along the ring
constexpr int kVisitEvery = 4;         // each vehicle leaves the ring every 4th lap
constexpr double kTransfer = 20;       // s from the ring to the spot, and back

struct Spot { bool valid = false; double x = 0, y = 0, z = 0, yaw = 0; };
enum class Kind { Ring, Leaving, Parked, Joining };
struct State {
    Kind kind = Kind::Ring;
    int layer = 0, exit = -1;
    double x = 0, y = 0, z = 0, heading = 0;
};

// The ring's mean length gives the angular rate: kSpeed along it.
inline double Rate(const ringlogic::Ring& r) {
    const double a = r.radius * r.aspect, b = r.radius;  // Ramanujan's ellipse perimeter
    const double p = ringlogic::kPi * (3 * (a + b) - std::sqrt((3 * a + b) * (a + 3 * b)));
    return 2 * ringlogic::kPi * kSpeed / std::max(p, 1.0);
}
inline double LapTime(const ringlogic::Ring& r) { return 2 * ringlogic::kPi / Rate(r); }

// Vehicle k's slot: evenly spaced on its ring; the upper ring's slots sit between the lower ring's.
inline double Phase(int k) {
    const int perLayer = kVehicles / 2, slot = k / 2;
    return 2 * ringlogic::kPi * (slot + (k % 2) * 0.5) / perLayer;
}
inline double Travel(const ringlogic::Ring& r, int k, double t) { return Phase(k) + Rate(r) * t; }  // grows 2 pi a lap
inline double AngleOf(int k, double travel) { return k % 2 ? -travel : travel; }  // the ring angle (ringlogic::At)

// The exit nearest a spot (its home exit).
inline int HomeExit(const ringlogic::Ring& r, const Spot& s) {
    int best = 0; double bd = 1e18;
    for (int e = 0; e < ringlogic::kExits; ++e) {
        const auto p = ringlogic::Exit(r, e);
        const double d = std::hypot(p.x - s.x, p.y - s.y);
        if (d < bd) { bd = d; best = e; }
    }
    return best;
}

inline double Smooth(double u) { u = std::clamp(u, 0.0, 1.0); return u * u * (3 - 2 * u); }

// Where vehicle k (home spot `home`) is at time t.
inline State At(const ringlogic::Ring& ring, const Spot& home, int k, double t) {
    State s;
    s.layer = k % 2;
    const double rate = Rate(ring), lapTime = 2 * ringlogic::kPi / rate;
    const double travel = Travel(ring, k, t);
    const long long lap = (long long)std::floor(travel / (2 * ringlogic::kPi));
    auto onRing = [&](double tr) {
        const auto p = ringlogic::OnLayer(ringlogic::At(ring, AngleOf(k, tr)), ring, s.layer);
        s.kind = Kind::Ring; s.x = p.x; s.y = p.y; s.z = p.z; s.heading = p.heading;
    };
    if (!home.valid) { onRing(travel); return s; }
    const int e = HomeExit(ring, home);
    // The slot's travel fraction at exit e: e/4 of a turn; the upper ring meets it going the other way.
    double f = double(e) / ringlogic::kExits;
    if (s.layer == 1) f = std::fmod(1.0 - f, 1.0);
    // A visit starts on lap L (every kVisitEvery-th lap for this vehicle) at the exit and lasts one lap: the one covering now
    // began on this lap (after the exit) or the previous one (before it).
    double start = 0; bool away = false;
    for (long long L : { lap, lap - 1 }) {
        if (((L + k) % kVisitEvery + kVisitEvery) % kVisitEvery != 0) continue;
        const double st = (double(L) + f) * 2 * ringlogic::kPi;
        if (travel >= st && travel < st + 2 * ringlogic::kPi) { away = true; start = st; break; }
    }
    if (!away) { onRing(travel); return s; }
    const double since = (travel - start) / rate;  // seconds since it left the ring
    const auto exitPoint = ringlogic::OnLayer(ringlogic::Exit(ring, e), ring, s.layer);
    s.exit = e;
    auto between = [&](double u, bool down) {
        const double w = Smooth(u);
        const double fx = down ? exitPoint.x : home.x, fy = down ? exitPoint.y : home.y, fz = down ? exitPoint.z : home.z;
        const double tx = down ? home.x : exitPoint.x, ty = down ? home.y : exitPoint.y, tz = down ? home.z : exitPoint.z;
        // Across first, then down (clear of the towers on the way in); up first, then across on the way out.
        const double wz = down ? Smooth(std::clamp(u * 2 - 1, 0.0, 1.0)) : Smooth(std::clamp(u * 2, 0.0, 1.0));
        s.x = fx + (tx - fx) * w; s.y = fy + (ty - fy) * w; s.z = fz + (tz - fz) * wz;
        const double dx = tx - fx, dy = ty - fy;
        s.heading = std::hypot(dx, dy) > 1 ? std::atan2(dx, dy) : home.yaw;
    };
    if (since < kTransfer) { s.kind = Kind::Leaving; between(since / kTransfer, true); }
    else if (since > lapTime - kTransfer) { s.kind = Kind::Joining; between((since - (lapTime - kTransfer)) / kTransfer, false); }
    else { s.kind = Kind::Parked; s.x = home.x; s.y = home.y; s.z = home.z; s.heading = home.yaw; }
    return s;
}
}
