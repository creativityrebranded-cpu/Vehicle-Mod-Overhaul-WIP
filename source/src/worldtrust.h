#pragma once
// Trust by source (change card T1, 2026-11-07; WORKING_MANDATE.md "Source priority": the newest, closest measurement wins; rays that
// pass clean through a predicted solid clear it). World only: it decides which predictions still stand, never a speed or a line.
// Trust order: contact, live vehicles and people, confirmed rays, rays, the sweep (Survey), the coarse map box (Box).
// A prediction (Survey or Box) is dropped where a fresher, higher-trust measurement disagrees:
//   - a clean probe face ray passed within rules::cleanRadius of it, at its height, in the last rules::cleanAge s;
//   - confirmed live ray hits measured that spot (within rules::rayRadius): the precise hit stands for it.
// Before T1, clean rays cleared only Survey solids; Box cells were rebuilt every frame and never cleared, so cars crept at creep speed
// ("predicted: slow and look") past solids the rays had already measured. With the rays down nothing is dropped (fail-safe).
// Pure; tests: tests/worldtrust_test.cpp.
#include "planworld.h"
#include <algorithm>
#include <cmath>

namespace worldtrust {
namespace rules {
constexpr double cleanRadius = .6;    // m across: a clean ray this close to a prediction's centre measured it
constexpr double cleanAge = 20.0;     // s a clean ray stays evidence
constexpr double cleanBelow = .3;     // m: the ray may run this far under the prediction's bottom and still count (the probe casts at tz and tz-0.2)
constexpr double rayRadius = 1.5;     // m: confirmed live hits within this of a prediction measured that spot
constexpr int confirmHits = 2;        // agreeing hits that make a ray confirmed (planbranch::rules::confirmHits)
constexpr double rayFresh = 3.0;      // s (planbranch::rules::freshHit)
}

enum class Level : int { Box = 1, Survey = 2, Ray = 3, ConfirmedRay = 4, Live = 5, Contact = 6 };
inline Level Of(const planworld::Shape& s, double now) {
    using planworld::Source;
    switch (s.source) {
    case Source::Box: return Level::Box;
    case Source::Survey: return Level::Survey;
    case Source::Ray: return s.support >= rules::confirmHits && s.haveNormal && now - s.seenAt <= rules::rayFresh ? Level::ConfirmedRay : Level::Ray;
    case Source::Contact: return Level::Contact;
    default: return Level::Live;  // vehicles, people, ground
    }
}

struct Segment { float a[3] = {}, b[3] = {}; double at = -1; };
// Clean face rays the probe cast (main thread writes, World reads; the caller locks).
struct CleanStore {
    static constexpr int kMax = 256;
    Segment seg[kMax]; int next = 0;
    void Add(const float a[3], const float b[3], double now) { auto& s = seg[next]; for (int i = 0; i < 3; ++i) { s.a[i] = a[i]; s.b[i] = b[i]; } s.at = now; next = (next + 1) % kMax; }
    void Clear() { for (auto& s : seg) s.at = -1; next = 0; }
    // A fresh clean ray passing within cleanRadius of (x, y) at a height between lo - cleanBelow and hi.
    bool Near(double x, double y, double lo, double hi, double now) const {
        for (const auto& s : seg) {
            if (s.at < 0 || now - s.at > rules::cleanAge || now < s.at) continue;
            // Points outside this envelope cannot be within cleanRadius of the segment.
            if (x < std::min<double>(s.a[0], s.b[0]) - rules::cleanRadius ||
                x > std::max<double>(s.a[0], s.b[0]) + rules::cleanRadius ||
                y < std::min<double>(s.a[1], s.b[1]) - rules::cleanRadius ||
                y > std::max<double>(s.a[1], s.b[1]) + rules::cleanRadius) continue;
            const double dx = s.b[0] - s.a[0], dy = s.b[1] - s.a[1], len2 = dx * dx + dy * dy;
            double u = len2 > 1e-9 ? ((x - s.a[0]) * dx + (y - s.a[1]) * dy) / len2 : 0; u = u < 0 ? 0 : u > 1 ? 1 : u;
            const double px = s.a[0] + dx * u, py = s.a[1] + dy * u, pz = s.a[2] + (s.b[2] - s.a[2]) * u;
            if (pz >= lo - rules::cleanBelow && pz <= hi && std::hypot(x - px, y - py) <= rules::cleanRadius) return true;
        }
        return false;
    }
};

enum class Why : int { Kept, Clean, Measured };
inline const char* Name(Why w) { constexpr const char* n[]{ "kept", "clean_ray_passed", "measured_by_rays" }; return n[int(w)]; }
// Does a prediction still stand? Only Survey and Box are ever dropped.
inline Why Judge(const planworld::Shape& p, const planworld::Shape* shapes, int n, const CleanStore& clean, double now, bool raysLive) {
    if (!raysLive || !planworld::Predicted(p)) return Why::Kept;
    const double lo = p.centre.z - p.halfHeight, hi = p.centre.z + p.halfHeight;
    if (clean.Near(p.centre.x, p.centre.y, lo, hi, now)) return Why::Clean;
    for (int i = 0; i < n; ++i) {
        const auto& r = shapes[i];
        if (r.source != planworld::Source::Ray || Of(r, now) != Level::ConfirmedRay) continue;
        if (std::hypot(r.centre.x - p.centre.x, r.centre.y - p.centre.y) <= rules::rayRadius && r.centre.z >= lo - .5 && r.centre.z <= hi + .5) return Why::Measured;
    }
    return Why::Kept;
}
}
