#pragma once
// Map visuals for road, lane and tile parameters (2026-11-19, user: "anything that affects the route I need to see on that overlay how it
// affects it"). Pure helpers only: the drawing itself is in routeedit.cpp (DrawRouteFacts) and docklayout.inl (DockDrawMap, tile boxes).
// Tests: tests/roadvisual_test.cpp. Nothing here changes a saved value; it only reads the route.
#include "trafficdata.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace roadvisual {
// Floor tint by travel. Hover keeps the colour the road floor always had, so older hover routes look as before.
constexpr unsigned kHoverTint = 0x6196AB, kDriveTint = 0x7DBE6A, kBothTint = 0xB59BD6, kArrow = 0xF4F7FA, kHatch = 0xF2E6B0;
inline routepolicy::Travel Concrete(routepolicy::Travel t) { return t == routepolicy::Travel::Inherit ? routepolicy::Travel::Hover : t; }
inline unsigned TravelTint(routepolicy::Travel t) {
    t = Concrete(t);
    return t == routepolicy::Travel::Drive ? kDriveTint : t == routepolicy::Travel::Both ? kBothTint : kHoverTint;
}
inline const char* TravelWord(routepolicy::Travel t) {
    t = Concrete(t);
    return t == routepolicy::Travel::Drive ? "Drive" : t == routepolicy::Travel::Both ? "Both" : "Hover";
}
// What a tile really uses: its own section travel, else the route's.
inline routepolicy::Travel TileTravel(const trafficdata::Route& r, std::size_t tile) { return Concrete(routepolicy::Resolve(r.travel, r.sections, tile).travel); }
inline double TileSpeed(const trafficdata::Route& r, std::size_t tile) { return tile < r.sections.size() && r.sections[tile].speed > 0 ? r.sections[tile].speed : r.speed; }
inline bool SolidsAllowed(const trafficdata::Route& r, std::size_t tile) { return tile < r.road.boxes.size() && r.road.boxes[tile].solids == 1; }
// The tile a sample belongs to, from its source position (Line::ToSource); `tiles` is the route's tile count (>= 1).
inline std::size_t TileOf(double source, std::size_t tiles) {
    if (tiles == 0) return 0;
    if (!(source > 0)) return 0;
    return std::min(std::size_t(source), tiles - 1);
}
// Points spaced along a chain of segments: `first` metres in, then every `spacing`. Spacing grows when the chain would need more than `maxStops`.
struct Stop { std::size_t segment = 0; double t = 0; };
inline void StopsAlong(const std::vector<double>& lengths, double spacing, double first, std::size_t maxStops, std::vector<Stop>& out) {
    out.clear();
    double total = 0;
    for (double l : lengths) total += std::isfinite(l) && l > 0 ? l : 0;
    if (!(spacing > 0) || !(total > 0) || maxStops == 0) return;
    if (total / spacing > double(maxStops)) spacing = total / double(maxStops);
    double at = std::clamp(first, 0.0, total), start = 0;
    std::size_t seg = 0;
    while (at <= total && seg < lengths.size()) {
        const double l = std::isfinite(lengths[seg]) && lengths[seg] > 0 ? lengths[seg] : 0;
        if (l > 0 && at < start + l) { out.push_back({ seg, (at - start) / l }); at += spacing; continue; }
        start += l; ++seg;
    }
}
// Short words for the tile's settings that differ from the plain road (empty: nothing to say). Shown beside the tile.
inline std::string TileTag(const trafficdata::Route& r, std::size_t tile) {
    std::string out;
    auto add = [&](const std::string& s) { if (!out.empty()) out += ", "; out += s; };
    char b[64];
    const bool own = tile < r.sections.size() && r.sections[tile].travel != routepolicy::Travel::Inherit && Concrete(r.sections[tile].travel) != Concrete(r.travel);
    if (own) add(TravelWord(r.sections[tile].travel));
    if (tile < r.sections.size() && r.sections[tile].speed > 0 && std::fabs(r.sections[tile].speed - r.speed) > .05) { std::snprintf(b, sizeof b, "%.0f m/s", r.sections[tile].speed); add(b); }
    if (tile < r.sections.size()) {
        const auto c = r.sections[tile].control;
        if (c == routepolicy::Control::Stop) add("stop"); else if (c == routepolicy::Control::Yield) add("yield"); else if (c == routepolicy::Control::Signal) add("signal");
    }
    if (SolidsAllowed(r, tile)) add("solids ok");
    if (tile < r.road.boxes.size()) {
        const auto& f = r.road.boxes[tile];
        if (f.width > 0) { std::snprintf(b, sizeof b, "box W%.0f", f.width); add(b); }
        if (f.height > 0) { std::snprintf(b, sizeof b, "H%.0f", f.height); add(b); }
        if (std::fabs(f.yaw) > .05) { std::snprintf(b, sizeof b, "yaw %.0f", f.yaw); add(b); }
        if (std::fabs(f.pitch) > .05) { std::snprintf(b, sizeof b, "pitch %.0f", f.pitch); add(b); }
        if (std::fabs(f.roll) > .05) { std::snprintf(b, sizeof b, "roll %.0f", f.roll); add(b); }
    }
    if (r.road.enabled) {
        const auto k = roadplan::PrimitiveAt(r.road, tile);
        if (tile < r.road.primitives.size() && r.road.primitives[tile].kind != roadplan::Default && k.kind != roadplan::Flat && k.kind != roadplan::Box) add(roadplan::KindName(k.kind));
        const auto t = roadplan::At(r.road, tile);
        if (t.connector == 1) add("bend"); else if (t.connector == 3) add("taper");
        const std::size_t rows = std::max<std::size_t>(1, r.road.dividers.size());
        bool side = false, swing = false, tilt = false;
        for (std::size_t d = 0; d < rows; ++d) { const auto q = roadplan::DividerAt(r.road, d, tile); side = side || std::fabs(q.side) > 1e-3; swing = swing || std::fabs(q.swing) > 1e-3; tilt = tilt || std::fabs(q.tilt) > .05; }
        if (side) add("div off-centre"); if (swing) add("div swing"); if (tilt) add("div tilt");
    }
    return out;
}
struct LegendFlags { bool hover = false, drive = false, both = false, arrows = true, solids = false, edges = false, dividers = false, boxes = false; };
inline std::string Legend(const LegendFlags& f) {
    std::string out;
    auto add = [&](const char* s) { if (!out.empty()) out += "  |  "; out += s; };
    if (f.hover) add("blue floor: hover");
    if (f.drive) add("green floor: drive");
    if (f.both) add("violet floor: both");
    if (f.arrows) add("arrows: lane direction");
    if (f.dividers) add("red: divider");
    if (f.edges) add("amber: edge band");
    if (f.solids) add("hatch: solids allowed");
    if (f.boxes) add("box: tile field (teal touch, amber avoid)");
    return out;
}
}
