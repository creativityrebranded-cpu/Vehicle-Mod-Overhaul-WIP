#pragma once
// Lane line geometry (removal 2026-11-14): where a point lies along an authored lane, and the lane point at a distance. Pure geometry, no
// decision. Moved out of the old lane driver (lanedriver.h, removed 2026-11-14, kept outside FVO in fvo-removed-20261114/deciders).
#include "routeguidance.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace laneline {
constexpr double kFaceStep = 0.35;  // a vertical face blocks a car from this height (kerb height); lower is a step
struct Projection { double distance = 1e30, along = 0, x = 0, y = 0, z = 0, bearing = 0; int segment = 0; };
inline Projection Project(const FvoLaneGuidanceV1& l, double x, double y, double z, bool vertical = false) {
    Projection best; double along = 0;
    for (int i = 0; i + 1 < l.count; ++i) {
        const auto& a = l.points[i]; const auto& b = l.points[i + 1];
        const double dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        const double flat = std::hypot(dx, dy), length = std::hypot(flat, dz);
        const double den = dx*dx + dy*dy + (vertical ? dz*dz : 0);
        const double t = den > 1e-10 ? std::clamp(((x-a.x)*dx + (y-a.y)*dy + (vertical ? (z-a.z)*dz : 0))/den, 0.0, 1.0) : 0;
        const double px = a.x+t*dx, py = a.y+t*dy, pz = a.z+t*dz;
        const double d = std::hypot(x-px, y-py, vertical ? z-pz : 0.0);
        if (d < best.distance) best = {d, along+t*length, px, py, pz, std::atan2(dx,dy), i};
        along += length;
    }
    return best;
}
inline double SegmentLength(const FvoLaneGuidanceV1& l, int i) {
    const auto& a = l.points[i]; const auto& b = l.points[i + 1];
    return std::hypot(b.x - a.x, b.y - a.y, b.z - a.z);
}
template<class Length>
inline FvoLanePointV1 AtWithLengths(const FvoLaneGuidanceV1& l, double along, Length&& length) {
    for (int i = 0; i + 1 < l.count; ++i) {
        const auto& a = l.points[i]; const auto& b = l.points[i + 1];
        const double d = length(i);
        if (along <= d || i + 2 == l.count) {
            const double t = d > 1e-8 ? std::clamp(along / d, 0.0, 1.0) : 0;
            return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
        }
        along -= d;  // preserve the original subtraction order at segment boundaries
    }
    return l.points[0];
}
inline FvoLanePointV1 At(const FvoLaneGuidanceV1& l, double along) {
    return AtWithLengths(l, along, [&](int i) { return SegmentLength(l, i); });
}
// One reader's immutable guidance window. The original projection uses a different norm
// and keeps its own arithmetic. These lengths are only for At and WindowLength.
class LaneWindow {
public:
    explicit LaneWindow(const FvoLaneGuidanceV1& l) : lane(l) {
        for (int i = 0; i + 1 < lane.count; ++i) {
            lengths[i] = SegmentLength(lane, i);
            total += lengths[i];
        }
    }
    FvoLanePointV1 At(double along) const {
        return AtWithLengths(lane, along, [&](int i) { return lengths[i]; });
    }
    double Length() const { return total; }
private:
    const FvoLaneGuidanceV1& lane;
    std::array<double, std::extent_v<decltype(FvoLaneGuidanceV1::points)> - 1> lengths;
    double total = 0;
};
}
