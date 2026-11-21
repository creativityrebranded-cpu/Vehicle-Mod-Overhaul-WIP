#pragma once

#include "fleetrail_api.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace fleetrail {
// Reference: outside the attached cells the engine drops the body and stops stepping the car, so the rail moves its
// reference (and seated driver) along the path instead. Reported to the add-on as Rail (2): the core owns its motion.
enum class Mode { Auto = 1, Rail = 2, Awaiting = 3, Reference = 4 };

// Soft switching between autopilot and the rail.
//  - Buffer zone: autopilot hands to the rail when the cells kExitAhead metres ahead are not attached, and only takes the
//    car back once the cells kEnterAhead metres ahead are attached as well, so a car near the edge does not flap.
//  - Minimum time in a mode (kMinDwellMs) before a planned switch. The one exception: a car whose own cell is no longer
//    attached goes to the rail at once, because the engine may stop stepping it.
//  - Entry blend: the rail starts from the car's actual offset and speed and eases onto its line over kBlendSeconds.
//  - Exit ramp: for kRampMs after the rail hands back, autopilot's commanded speed may rise only kRampAccel m/s^2
//    above the rail's last speed. Braking is never limited.
namespace rules {
constexpr double kExitAheadMin = 30.0, kExitAheadMax = 80.0, kExitAheadSeconds = 2.5;
constexpr double kEnterAheadExtra = 25.0;
constexpr unsigned long long kMinDwellMs = 1500;
constexpr double kBlendSeconds = 1.0, kMaxBlendOffset = 6.0;
constexpr double kAccel = 2.5, kDecel = 4.0;        // m/s^2: rail speed changes
constexpr double kLateralAccel = 3.0;               // m/s^2: rail corner speed
constexpr double kCornerChord = 8.0;                // m: half chord used to measure a bend
constexpr unsigned long long kRampMs = 1500;
constexpr double kRampAccel = 2.5;
constexpr unsigned long long kReferenceStepMs = 250;  // how often a reference-mode car is moved
}

inline double ExitAhead(double speed)
{
    return std::clamp(speed * rules::kExitAheadSeconds, rules::kExitAheadMin, rules::kExitAheadMax);
}

struct Facts {
    bool attachedNow = false;        // the car's own cell is attached and the car is listed in it
    bool exitAheadAttached = false;  // the cell ExitAhead(speed) metres along the path is attached
    bool enterAheadAttached = false; // the cell ExitAhead(speed) + kEnterAheadExtra metres along is attached
    bool chassisReady = false;
    bool autopilotReady = false;
};

inline Mode Choose(Mode current, const Facts& f, unsigned long long sinceSwitchMs)
{
    if (!f.chassisReady) return Mode::Awaiting;
    const bool dwellDone = sinceSwitchMs >= rules::kMinDwellMs;
    if (current == Mode::Auto) {
        if (!f.attachedNow) return Mode::Rail;
        if (!f.exitAheadAttached && dwellDone) return Mode::Rail;
        return Mode::Auto;
    }
    if (f.attachedNow && f.exitAheadAttached && f.enterAheadAttached && f.autopilotReady && dwellDone) return Mode::Auto;
    if (current == Mode::Rail) return Mode::Rail;
    return (!f.attachedNow || !f.exitAheadAttached) ? Mode::Rail : Mode::Awaiting;
}

// 1 at the start of the entry blend, 0 once it is over (smoothstep).
inline double BlendWeight(double seconds)
{
    const double t = std::clamp(seconds / rules::kBlendSeconds, 0.0, 1.0);
    return 1.0 - t * t * (3.0 - 2.0 * t);
}

inline double Approach(double speed, double want, double dt)
{
    if (!std::isfinite(want)) return speed;
    return want > speed ? std::min(want, speed + rules::kAccel * dt) : std::max(want, speed - rules::kDecel * dt);
}

// Autopilot's speed ceiling during the exit ramp; infinity once it is over.
inline double RampCap(double railSpeed, unsigned long long sinceHandoffMs)
{
    if (sinceHandoffMs >= rules::kRampMs) return std::numeric_limits<double>::infinity();
    return railSpeed + rules::kRampAccel * double(sinceHandoffMs) / 1000.0;
}

inline double Length(const FvoFleetRailPoint& a, const FvoFleetRailPoint& b)
{
    return std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
}

class Path {
public:
    static constexpr unsigned kMaxPoints = 128;

    bool Set(const FvoFleetRailPoint* input, unsigned count)
    {
        if (!input || count < 2 || count > kMaxPoints) return false;
        std::vector<Knot> next;
        next.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            const auto p = input[i];
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
            const double segment = next.empty() ? 0.0 : Length(next.back().p, p);
            if (!std::isfinite(segment)) return false;
            if (!next.empty() && segment < 0.01) continue;
            const double s = next.empty() ? 0.0 : next.back().s + segment;
            if (!std::isfinite(s)) return false;
            next.push_back({p, s});
        }
        if (next.size() < 2) return false;
        knots_.swap(next);
        return true;
    }

    bool Same(const Path& other) const
    {
        return std::equal(knots_.begin(), knots_.end(), other.knots_.begin(), other.knots_.end(),
            [](const Knot& a, const Knot& b) {
                return a.p.x == b.p.x && a.p.y == b.p.y && a.p.z == b.p.z;
            });
    }

    double End() const { return knots_.empty() ? 0.0 : knots_.back().s; }

    double Advance(double station, double metres) const
    {
        return std::clamp(station + metres, 0.0, End());
    }

    FvoFleetRailPoint At(double station) const
    {
        if (knots_.empty()) return {};
        const auto it = std::lower_bound(knots_.begin(), knots_.end(), station,
            [](const Knot& a, double s) { return a.s < s; });
        if (it == knots_.begin()) return it->p;
        if (it == knots_.end()) return knots_.back().p;
        const auto& a = *(it - 1);
        const auto& b = *it;
        return Interpolate(a.p, b.p, (station - a.s) / (b.s - a.s));
    }

    double Project(double x, double y, double z) const
    {
        const FvoFleetRailPoint position{x, y, z};
        double best = std::numeric_limits<double>::infinity();
        double station = 0;
        for (std::size_t i = 1; i < knots_.size(); ++i) {
            const auto& a = knots_[i - 1];
            const auto& b = knots_[i];
            const double segment = b.s - a.s;
            const double t = std::clamp(((x - a.p.x) * (b.p.x - a.p.x) +
                (y - a.p.y) * (b.p.y - a.p.y) + (z - a.p.z) * (b.p.z - a.p.z))
                / (segment * segment), 0.0, 1.0);
            const double d = Length(position, Interpolate(a.p, b.p, t));
            if (d < best) {
                best = d;
                station = a.s + t * segment;
            }
        }
        return station;
    }

    // The speed the rail may hold here: the lowest corner cap within 60 m, each relaxed by the braking
    // distance to it. Infinity on a straight.
    double CornerSpeed(double station) const
    {
        double best = std::numeric_limits<double>::infinity();
        if (knots_.size() < 3) return best;
        for (double d = 0.0; d <= 60.0; d += 5.0) {  // 5 m apart, so the 2 x kCornerChord window never skips a bend
            const double s = station + d;
            if (s > End()) break;
            const double r = Radius(At(std::max(0.0, s - rules::kCornerChord)), At(s),
                                    At(std::min(End(), s + rules::kCornerChord)));
            if (!std::isfinite(r)) continue;
            const double cap = std::sqrt(rules::kLateralAccel * std::max(r, 3.0));
            best = std::min(best, std::sqrt(cap * cap + 2.0 * rules::kDecel * d));
        }
        return best;
    }

    static double Radius(const FvoFleetRailPoint& a, const FvoFleetRailPoint& b, const FvoFleetRailPoint& c)
    {
        const double ab = std::hypot(b.x - a.x, b.y - a.y), bc = std::hypot(c.x - b.x, c.y - b.y),
                     ca = std::hypot(a.x - c.x, a.y - c.y);
        const double area2 = std::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
        if (!(area2 > 1e-6)) return std::numeric_limits<double>::infinity();
        return ab * bc * ca / (2.0 * area2);
    }

private:
    struct Knot { FvoFleetRailPoint p; double s; };
    std::vector<Knot> knots_;

    static FvoFleetRailPoint Interpolate(const FvoFleetRailPoint& a, const FvoFleetRailPoint& b, double t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }
};
}
