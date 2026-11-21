#pragma once
// One tile's field box (fieldboxapi.h, design section 3): the single placement used by the traffic addon's provider (driving, spawning)
// and by the F7 editor's tile check, so both look at the same box. Agreed 2026-11-07 between the driving side and the F7 side:
//   - missing boxes derive from the shared stored-format defaults below; unreadable records and unapproved D11 shapes give none;
//     road tiles need not be switched on. Derived boxes serve driving and spawning alike.
//   - the box sits on the ROUTE line (the middle of all its lanes), bottom face on that line; length from the tile's two points;
//   - width 0 = the road's width (road.width, else corridor x lanes); height 0 = fieldbox::kDefaultHeight (5 m, the editor's roof);
//   - yaw and pitch are the tile's own direction plus the stored degree offsets; roll as stored; radians out.
// Pure (no engine calls); tests: tests/tilebox_test.cpp.
#include "fieldboxapi.h"
#include "trafficroute.h"
#include <cmath>

namespace tilebox {
// The route line at u: the middle of every lane line there.
inline trafficdata::Vec RouteAt(const trafficroute::Line& l, double u) {
    trafficdata::Vec m{}; const int n = int(l.lane.size()); int k = 0;
    for (int i = 0; i < n; ++i) { if (l.lane[std::size_t(i)].empty()) continue; const auto p = trafficroute::At(l, i, u); m.x += p.x; m.y += p.y; m.z += p.z; ++k; }
    if (k) { m.x /= k; m.y /= k; m.z /= k; }
    return m;
}
inline double RoadWidth(const trafficdata::Route& r) { return r.road.width > 0 ? r.road.width : std::max(1.0, r.corridor * std::max(1, r.lanes)); }
// Outer lane edges for compatibility defaults; authored road geometry already divides its full width.
inline double LaneSpanWidth(const trafficdata::Route& r) {
    if (r.road.enabled) return RoadWidth(r);
    if (!r.lanePlan.enabled) return std::max(1.0, r.corridor + (r.lanes > 1 ? std::abs(r.width) : 0));
    double extent = 0;
    for (int section = -1; section < int(r.points.size()); ++section) {
        const auto settings = laneplan::Resolve(r.lanePlan, section);
        for (int lane : laneplan::Lanes(settings)) {
            const auto v = laneplan::Resolve(r.lanePlan, section, lane);
            extent = std::max(extent, std::abs(laneplan::Centre(r.lanePlan, section, lane) + v.profile.offset) + v.profile.width * .5);
        }
    }
    return extent * 2;
}
inline int TileCount(const trafficdata::Route& r) { return r.ring || r.points.size() < 2 ? 0 : int(r.points.size()) - (r.closed ? 0 : 1); }
// Field Provider's one default policy, also read by F7 authoring. Never changes route data.
inline bool StoredBox(const trafficdata::Route& r, int tile, roadplan::FieldBox& b) {
    if (tile < 0 || tile >= TileCount(r) || !r.road.boxesUnsupported.empty() || !r.road.unsupported.empty()) return false;
    if (tile < int(r.road.boxes.size())) { b = r.road.boxes[std::size_t(tile)]; return roadplan::ValidBox(b); }
    // D11 (not approved) is judged on the STORED kind: PrimitiveAt resolves a roofless road's shapes to Flat, which read an old stored
    // tube / rounded / trapezoid tile as a box (spawn fork 2026-11-07; tilebox_test "D11 excluded and failed update atomic").
    const int stored = tile < int(r.road.primitives.size()) ? r.road.primitives[std::size_t(tile)].kind : int(roadplan::Default);
    if (stored == roadplan::Rounded || stored == roadplan::Tube || stored == roadplan::Trapezoid) return false;
    const auto kind = roadplan::PrimitiveAt(r.road, std::size_t(tile)).kind;
    auto travel = r.travel;
    if (tile < int(r.sections.size()) && r.sections[std::size_t(tile)].travel != routepolicy::Travel::Inherit) travel = r.sections[std::size_t(tile)].travel;
    b = roadplan::DefaultBox(travel == routepolicy::Travel::Hover || kind == roadplan::Open);
    b.width = std::max(RoadWidth(r), LaneSpanWidth(r));
    b.height = r.road.height > 0 ? r.road.height : fieldbox::kDefaultHeight;
    return roadplan::ValidBox(b);
}
// F7 Update Route: atomic working-copy edit; -1 means unavailable, 0 already current.
inline int MaterializeMissing(trafficdata::Route& r) {
    if (!r.road.boxesUnsupported.empty() || !r.road.unsupported.empty() || TileCount(r) == 0) return -1;
    auto boxes = r.road.boxes;
    for (int tile = int(boxes.size()); tile < TileCount(r); ++tile) {
        roadplan::FieldBox b;
        if (!StoredBox(r, tile, b)) return -1;
        boxes.push_back(b);
    }
    const int added = int(boxes.size() - r.road.boxes.size());
    r.road.boxes = std::move(boxes);
    return added;
}
// False: unavailable field (unsupported format, unapproved old shape or invalid geometry).
inline bool TileBox(const trafficdata::Route& r, const trafficroute::Line& l, int tile, FvoFieldBoxV1& f) {
    constexpr double kDeg = 3.14159265358979323846 / 180.0;
    const int points = int(r.points.size());
    if (l.error || points < 2) return false;
    roadplan::FieldBox b;
    if (!StoredBox(r, tile, b)) return false;
    const double u0 = l.ToLine(double(tile)), u1 = l.ToLine(double(tile + 1));
    const auto a = RouteAt(l, u0), c = RouteAt(l, u1), m = RouteAt(l, .5 * (u0 + u1));
    const double run = std::hypot(c.x - a.x, c.y - a.y), len = std::hypot(run, c.z - a.z);
    if (!(len > .2) || !std::isfinite(len)) return false;
    // FW-1 (2026-11-12, 14:57 run: lc_ring_road edited to a 15.1 m road kept 11 m boxes, so its lanes ran at and past the box sides; the
    // plan refused the route for 1 cm of overshoot 38 m ahead and flew where the lane left the boxes): a field is never narrower than the
    // lanes it holds. A narrower stored or default width is widened to the route's lane span.
    const double width = std::max(b.width > 0 ? b.width : RoadWidth(r), LaneSpanWidth(r)), height = b.height > 0 ? b.height : fieldbox::kDefaultHeight;
    f = FvoFieldBoxV1{};
    f.version = 2; f.isDefault = tile >= int(r.road.boxes.size()) ? 1 : 0;
    f.halfWidth = width * .5; f.halfLength = len * .5; f.halfHeight = height * .5;
    f.yaw = std::atan2(c.x - a.x, c.y - a.y) + b.yaw * kDeg; f.pitch = std::atan2(c.z - a.z, std::max(run, 1e-6)) + b.pitch * kDeg; f.roll = b.roll * kDeg;
    f.cx = m.x; f.cy = m.y; f.cz = m.z + f.halfHeight;
    f.faces = std::uint32_t(b.faces) & 63u; f.solids = b.solids; f.tile = tile;
    return true;
}
}
