#pragma once
#include "birth_api.h"
#include "spawnflow.h"
#include <algorithm>
#include <cmath>

namespace roadclearance {
inline constexpr double maxBound = 40;
inline constexpr double maxPeerRadius = 58; // hypot(40, 40) + model margin, rounded up
inline constexpr double cellSize = 100;
inline constexpr std::uint64_t evidenceMs = 8000, refreshMs = 2000;
inline constexpr unsigned scanRefs = 1024, maxSources = 64, maxCompareRefs = 250000;
inline constexpr std::uint64_t ProofMs(bool loading) { return loading ? 2000 : 250; }

struct Range { int left, right, bottom, top; };
inline bool Cells(const FvoRoadSiteV1& site, Range& out, double peerRadius = maxPeerRadius) {
    if (!std::isfinite(site.x) || !std::isfinite(site.y) || !std::isfinite(site.z) ||
        !std::isfinite(site.heading) || !(site.radius > 0 && site.radius <= maxPeerRadius) ||
        !(site.halfHeight > 0 && site.halfHeight <= maxBound + 1) ||
        !(peerRadius > 0 && peerRadius <= maxPeerRadius)) return false;
    const double clearance = site.radius + peerRadius;
    const double departure = clearance + site.radius * spawnflow::rules::departureGapRadii;
    const double reach = std::hypot(departure, site.radius);
    if (std::abs(site.x) + reach > 1e9 || std::abs(site.y) + reach > 1e9) return false;
    const double dx = departure * std::cos(site.heading), dy = departure * std::sin(site.heading);
    const double sideX = site.radius * std::abs(std::sin(site.heading)), sideY = site.radius * std::abs(std::cos(site.heading));
    const auto first = [=](double pos,double along,double side) { return int(std::floor((pos + std::min(-clearance,std::min(0.0,along)-side)) / cellSize)); };
    const auto last = [=](double pos,double along,double side) { return int(std::floor((pos + std::max(clearance,std::max(0.0,along)+side)) / cellSize)); };
    out = {first(site.x,dx,sideX),last(site.x,dx,sideX),first(site.y,dy,sideY),last(site.y,dy,sideY)};
    return true;
}

inline bool Fresh(std::uint64_t at, std::uint64_t now, std::uint64_t limit) {
    return at && at <= now && now - at <= limit;
}
inline bool Matches(const FvoRoadProofV1& proof, const FvoTrafficBirthV1& request) {
    const auto& site = proof.site;
    return proof.epoch && site.x == request.x && site.y == request.y && site.z == request.z && site.heading == request.heading;
}

template<class Persistent, class Attached>
FvoRoadClearance Verify(const Range& range, Persistent persistent, Attached attached) {
    using Result = FvoRoadClearance;
    const auto saved = persistent();
    if (saved != Result::Clear) return saved;
    for (int y = range.bottom; y <= range.top; ++y) for (int x = range.left; x <= range.right; ++x) {
        const auto current = attached(x, y);
        if (current != Result::Clear) return current;
    }
    return Result::Clear;
}
}
