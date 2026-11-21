#pragma once
// Look-ahead doubt probe (design section 2, World "Doubt"; user 2026-11-07: "find a place that would cause doubt and resolve it before
// getting there"). Picks the doubtful spots ahead of a car: low ray shapes World is not sure of (too few agreeing hits, no normal, stale)
// and wanted floor samples with no measured floor, between the stopping distance and rules::probeFar, near the line. The game side
// (vehicleflight.cpp) casts direct rays at them and feeds the answers back into World's stores. Decides nothing. Pure; tests:
// tests/planprobe_test.cpp.
#include "planrules.h"
#include "planworld.h"
#include <algorithm>
#include <cmath>

namespace planprobe {
enum class Kind : unsigned char { DoubtfulShape, UnmeasuredFloor };
struct Target { double x = 0, y = 0, z = 0, along = 0, heading = 0; Kind kind = Kind::DoubtfulShape; };
struct Sample { double x = 0, y = 0, z = 0, along = 0, heading = 0; bool wanted = false, measured = false; };  // the line, ahead of the car

// World's sureness of a ray shape (as planrun::Sure).
inline bool Sure(const planworld::Shape& s, double now) {
    if (s.source != planworld::Source::Ray) return true;
    return s.support >= planbranch::rules::confirmHits && s.haveNormal && now - s.seenAt <= planbranch::rules::freshHit;
}
inline double StopDistance(double speed, double brake) { return speed * speed / (2.0 * std::max(.5, brake)) + 2.0; }

// Recently probed spots (per car).
struct Recent {
    struct Spot { double x = 0, y = 0, at = -1e9; } spots[16]; int next = 0;
    bool Seen(double x, double y, double now) const {
        for (const auto& s : spots) if (now - s.at < planbranch::rules::probeCooldown && std::hypot(s.x - x, s.y - y) < planbranch::rules::probeSame) return true;
        return false;
    }
    void Note(double x, double y, double now) { spots[next] = { x, y, now }; next = (next + 1) % 16; }
};

// Up to `max` targets, nearest first. carX/carY/heading (atan2(dx, dy)) place the car; `span` is how far either side of the line counts.
// Shapes: the car's World this frame; samples: the line ahead with whether a floor is wanted and measured there.
inline int Pick(const planworld::Shape* shapes, int count, const Sample* samples, int nSamples, double carX, double carY, double heading,
    double speed, double brake, double span, double now, const Recent& recent, Target* out, int max) {
    const double nearest = StopDistance(speed, brake), farthest = planbranch::rules::probeFar;
    const double fx = std::sin(heading), fy = std::cos(heading);
    int n = 0; Target found[64];
    // World owns measurement scheduling. Keep the nearest candidates across the
    // entire snapshot, rather than letting source iteration order exhaust it.
    auto offer = [&](Target t) {
        int at = n;
        if (n == 64) { if (t.along >= found[63].along) return; at = 63; }
        else ++n;
        while (at > 0 && t.along < found[at - 1].along) { found[at] = found[at - 1]; --at; }
        found[at] = t;
    };
    for (int i = 0; i < count; ++i) {
        const auto& s = shapes[i];
        if (!planworld::Predicted(s) && (s.source != planworld::Source::Ray || Sure(s, now))) continue;  // predictions are always worth checking
        const double dx = s.centre.x - carX, dy = s.centre.y - carY;
        double along = dx * fx + dy * fy, side = dx * fy - dy * fx, aim = heading;
        // Follow the supplied route polyline around corners. The nose-line
        // fallback is retained for callers that do not supply a usable line.
        double best = 1e30;
        for (int j = 1; j < nSamples; ++j) {
            const auto& a = samples[j - 1]; const auto& b = samples[j];
            const double ux = b.x - a.x, uy = b.y - a.y, len2 = ux * ux + uy * uy;
            if (len2 < 1e-8) continue;
            const double f = std::clamp(((s.centre.x - a.x) * ux + (s.centre.y - a.y) * uy) / len2, 0.0, 1.0);
            const double distance = std::hypot(s.centre.x - a.x - f * ux, s.centre.y - a.y - f * uy);
            if (distance < best) { best = distance; side = distance; along = a.along + f * (b.along - a.along); aim = std::atan2(ux, uy); }
        }
        if (along < nearest || along > farthest || std::fabs(side) > span || recent.Seen(s.centre.x, s.centre.y, now)) continue;
        offer({ s.centre.x, s.centre.y, s.centre.z, along, aim, Kind::DoubtfulShape });
    }
    for (int i = 0; i < nSamples; ++i) {
        const auto& p = samples[i];
        if (!p.wanted || p.measured || p.along < nearest || p.along > farthest || recent.Seen(p.x, p.y, now)) continue;
        offer({ p.x, p.y, p.z, p.along, p.heading, Kind::UnmeasuredFloor });
    }
    std::sort(found, found + n, [](const Target& a, const Target& b) { return a.along < b.along; });
    const int take = std::min({ n, max, planbranch::rules::probeTargets });
    for (int i = 0; i < take; ++i) out[i] = found[i];
    return take;
}
}
