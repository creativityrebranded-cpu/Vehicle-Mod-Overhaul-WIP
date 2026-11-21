#pragma once
// SR-1 (2026-11-19, user: "take relativity ... with what we consider a wall when gravity is on ... if the floor I'm on has a steep
// angle above certain ranges it's no longer climbable"): ONE rule that answers, for every sensed hit, "can the car's wheels drive on
// this surface?"
//
// Physics: on a slope of angle t the tyres hold while tan(t) <= mu. Gravity's size cancels out of that friction limit, so the rule is
// relative to the gravity direction only ("up" = minus the gravity direction), never to its strength. The steepest climbable grade is
// the lower of the friction limit (mu x the gear's grip multiplier x the planner's safety share) and the grade the engine can pull.
// A surface whose normal leans further from "up" than that grade's angle is a wall; a surface that stands higher over the floor than
// the wheels roll over is a wall whatever its normal says.
//
// The car's numbers (climb, mu, safety, engineGrade, MaxGrade) are the one car record (carcap.h, D2 2026-11-19); this file adds only the
// gear's grip. "climb" below is carcap's climb (0.35 m, half a tyre; it replaced this file's own 0.25 m step).
// Order of the rule (Judge):
//   a. more than climb over the floor                   -> Solid (regardless of normal)
//   b. trusted normal: facing down (upDot < 0)          -> Solid
//                      upDot <  MinUpDot                -> Solid (too steep: wall, rock face, side of a box)
//                      upDot >= MinUpDot, at the floor (within +-climb) or no floor known -> Drivable
//   c. no trusted normal, at the floor (within +-climb)  -> Drivable (hull contacts at the floor have unreliable normals,
//      e.g. ramp deck contacts read -0.39); no floor known -> Unknown
//   a2. more than climb under the floor                 -> Below (SB-1: under the road; callers drop it, it is not ground)
//   d. anything else (no floor known, no trusted normal) -> Unknown. Unknown is never Drivable.
//
// It replaces: the radar's terrain face rule (TW-1, fixed 45 deg), the down-ray wall check (TP-1/2/3), the floor-map vote in the
// radar judge, road-base learning (RT-1) and the eases-only/may-stop split.
// Pure logic: no game calls, no globals, no logging. Tests: tests/surface_test.cpp.
#include "carcap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace surface {

// The one car record (climb, mu, safety, engineGrade) plus the gear's grip multiplier.
struct Car : carcap::Car {
    double gripMult = 1.0;     // the gear's grip multiplier
};

// Steepest climbable grade (tan of the angle): carcap MaxGrade at this gear's grip.
inline double MaxGrade(const Car& c) { return c.MaxGrade(c.gripMult); }

// Smallest dot product between a surface normal and "up" that is still drivable: cos(atan(g)) = 1 / sqrt(1 + g^2).
inline double MinUpDot(const Car& c) {
    const double g = MaxGrade(c);
    return 1.0 / std::sqrt(1.0 + g * g);
}

struct Hit {
    double upDot = 0;             // surface normal . up (up = minus the gravity direction, unit)
    bool haveNormal = false;
    bool normalTrusted = true;
    double heightOverFloor = 0;   // hit height minus the floor under the car's path there (m)
    bool haveFloor = false;
    bool groundLayer = false;     // hit is on the ground/terrain layer; carried for traces, the rule does not depend on it
};

// Below (SB-1, 2026-11-19): more than climb under the floor the car drives on here: under the road, never in the car's way, so callers
// drop it (it is not ground either: nothing is learnt from it). Log 00:55 (core 93E40866): on the ramp at -1262,1449 the rays hit the ramp
// pieces' own undersides and side faces 0.5-2.5 m under the measured deck (normals -0.31 and 0.01); rule b made them Solid before the height
// was looked at, every lane speed "entered a solid", and the car backed up, retried and stood for about 50 s, three times on the drive.
enum class Verdict { Drivable, Solid, Unknown, Below };

inline const char* Name(Verdict v) {
    switch (v) {
    case Verdict::Drivable: return "Drivable";
    case Verdict::Solid: return "Solid";
    case Verdict::Unknown: return "Unknown";
    case Verdict::Below: return "Below";
    }
    return "?";
}

// Centre, then -X, +X, -Y, +Y at equal distance from the candidate hit.
constexpr double kFloorProbeOffset = .45;
struct FloorSample { bool measured = false, connected = false; double z = 0; std::uint32_t body = 0; };
struct FloorConfidence { bool fitted = false, clear = false; double height = 0, error = 0; };
inline FloorConfidence FitProbedFloor(const FloorSample (&s)[5], double hit, std::uint32_t hitBody, bool competingFloor, const Car& car) {
    FloorConfidence result;
    int connected = 0;
    bool matchesBody = false;
    for (const auto& p : s) {
        if (!p.measured || !std::isfinite(p.z)) return result;
        connected += p.connected;
        matchesBody |= p.connected && hitBody && p.body == hitBody;
    }
    if (connected < 3 || (!s[1].connected && !s[2].connected) || (!s[3].connected && !s[4].connected)) return result;
    const double dx = (s[2].z - s[1].z) / (2 * kFloorProbeOffset);
    const double dy = (s[4].z - s[3].z) / (2 * kFloorProbeOffset);
    const double crossX = (s[1].z + s[2].z) / 2;
    const double crossY = (s[3].z + s[4].z) / 2;
    result.height = s[0].z;
    result.error = .03 + std::max(std::fabs(crossX - s[0].z), std::fabs(crossY - s[0].z));
    if (std::hypot(dx, dy) > MaxGrade(car) || result.error > .10) return result;
    result.fitted = true;
    // A connected measurement from the hit body proves it contributes to the
    // road surface; so do five connected measurements of one road around it
    // (FC-2: a seam under the neighbouring deck piece). Require the entire
    // fitted interval above the point.
    result.clear = (matchesBody || connected == 5) && !competingFloor && hit < result.height - result.error &&
        result.height - hit + result.error <= 4.0;
    return result;
}

struct VerifiedFloorPoint {
    std::uint32_t vehicle = 0, body = 0;
    double x = 0, y = 0, z = 0, at = 0;
    double anchorX = 0, anchorY = 0, anchorZ = 0;  // connected same-body sample that proved the point
    bool Matches(std::uint32_t self, std::uint32_t hitBody, double px, double py, double pz, double now) const {
        return vehicle && vehicle == self && body && body == hitBody && now >= at && now - at <= 3.0 &&
            std::hypot(px - x, py - y) <= .02 && std::fabs(pz - z) <= .02;
    }
};

// A sweep station is only continuity evidence near its own floor band;
// it cannot declare a wall or prove that an offset hit lies below the road.
inline bool SweepFloorSupports(double heightDifference, const Car& car) {
    return std::fabs(heightDifference) <= car.climb;
}

inline Verdict Judge(const Hit& h, const Car& c) {
    const bool atFloor = h.haveFloor && std::fabs(h.heightOverFloor) <= c.climb;
    // a. Stands above what the wheels roll over.
    if (h.haveFloor && h.heightOverFloor > c.climb) return Verdict::Solid;
    // a2. Lies under the floor (SB-1): whatever its normal faces, the car cannot run into it from this floor.
    if (h.haveFloor && h.heightOverFloor < -c.climb) return Verdict::Below;
    // b. A trusted normal decides the face.
    if (h.haveNormal && h.normalTrusted) {
        if (h.upDot < 0) return Verdict::Solid;              // faces down: underside of a deck or overhang
        if (h.upDot < MinUpDot(c)) return Verdict::Solid;    // too steep for these tyres / this engine
        return Verdict::Drivable;                             // at the floor, or no floor known
    }
    // c. No trusted normal: only the height over the floor can say it.
    if (atFloor) return Verdict::Drivable;
    // d. Doubt is never Drivable.
    return Verdict::Unknown;
}

} // namespace surface
