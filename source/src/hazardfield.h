#pragma once
// Hazard field (2026-11-21): the boxes of every route in this world, read from the route files, for World. A box is a planner volume: it reaches
// the planner as planworld::Shape (Source::Volume, a measured fixed solid that may stop a car) and the rail path reads the same boxes. No game
// object is created. Decides nothing: it answers "which boxes are near here". Game side: hazardfield.cpp. Tests: tests/hazardfield_test.cpp.
#include "planworld.h"
#include "routehazard.h"
#include <string>
#include <vector>

namespace hazardfield {
struct Entry { routehazard::Box box; std::string route; };

// The box as a World shape: the same oriented box the collision test uses for kept game boxes. mayStop: it is a measured fixed volume (the
// user's own mark), so it can stop a car when no way round exists; steerRound lets Avoid plan round it first.
inline planworld::Shape ShapeOf(const routehazard::Box& b, double now) {
    planworld::Shape s; s.source = planworld::Source::Volume;
    s.centre = { b.x, b.y, b.z }; s.halfWidth = b.hx; s.halfLength = b.hy; s.halfHeight = b.hz;
    s.heading = b.yaw * routehazard::kPi / 180.0;  // planworld: heading in radians, sin = x, cos = y (the KeptBox yaw convention)
    s.haveNormal = true; s.normalZ = 0; s.support = 2; s.seenAt = now > 0 ? now : 1.0;
    s.mayStop = true; s.steerRound = true; s.unmeasured = false; s.hazardMark = true;
    return s;
}
// Boxes within `radius` of (x, y), and whose heights reach zLo..zHi (a box on another deck is not returned). Returns the count written.
inline int Near(const std::vector<Entry>& all, double x, double y, double zLo, double zHi, double radius, routehazard::Box* out, int max) {
    int n = 0;
    for (const auto& e : all) {
        if (n >= max) break;
        const auto& b = e.box; const double reach = radius + std::hypot(b.hx, b.hy);
        if (std::hypot(b.x - x, b.y - y) > reach) continue;
        if (zHi < routehazard::Lo(b) || zLo > routehazard::Hi(b)) continue;
        out[n++] = b;
    }
    return n;
}
// Shapes for the planner round (x, y): appended to `shapes` unless one at the same place is already there.
inline int AppendShapes(const std::vector<Entry>& all, double x, double y, double zLo, double zHi, double radius, double now, std::vector<planworld::Shape>& shapes, std::size_t cap) {
    routehazard::Box found[16]; const int n = Near(all, x, y, zLo, zHi, radius, found, 16); int added = 0;
    for (int i = 0; i < n && shapes.size() < cap; ++i) {
        bool dup = false;
        for (const auto& o : shapes) if (o.hazardMark && std::fabs(o.centre.x - found[i].x) < .05 && std::fabs(o.centre.y - found[i].y) < .05 && std::fabs(o.centre.z - found[i].z) < .05) { dup = true; break; }
        if (dup) continue;
        shapes.push_back(ShapeOf(found[i], now)); ++added;
    }
    return added;
}
// ---- suggestions: what a test vehicle met in its path ----
struct Suggestion { routehazard::Box box; std::string route; int seen = 0; double lastSeen = 0; };
constexpr int kSeenToShow = 3;                    // sightings (frames) before it is offered: one stray hit is not a hazard
constexpr double kMaxSuggestHalf = 12.0;          // a larger solid is a structure, not a small hazard
// One sighting joins the suggestion that already holds its centre or touches it, else starts a new one.
inline void Sight(std::vector<Suggestion>& list, const routehazard::Box& b, const std::string& route, double now, std::size_t cap = 24) {
    if (b.hx > kMaxSuggestHalf || b.hy > kMaxSuggestHalf) return;
    for (auto& s : list) {
        if (s.route != route) continue;
        if (routehazard::Touches(s.box, b.x, b.y, b.z, b.z, .6)) { if (now - s.lastSeen > .05) ++s.seen; s.lastSeen = now; if (s.seen <= 12) routehazard::Grow(s.box, b); return; }
    }
    if (list.size() >= cap) return;
    list.push_back({ routehazard::Clamped(b), route, 1, now });
}
// Game side (hazardfield.cpp).
void Tick(bool loading);                 // main thread, every frame: re-reads the route files when they changed
void Invalidate();                       // the editor saved: read again at the next Tick
std::vector<Entry> Snapshot();           // a copy of this world's boxes
int NearNow(double x, double y, double zLo, double zHi, double radius, routehazard::Box* out, int max);
// Test route: the add-on names the vehicle flying a route test; its path is watched for fixed obstacles (export FVO_HazardTestV1, hazardfield.cpp).
void TestSet(unsigned vehicle, const std::string& route);
void TestClear(unsigned vehicle);
bool IsTest(unsigned vehicle, std::string* route = nullptr);
void NoteSighting(unsigned vehicle, const routehazard::Box& b, double now);
std::vector<Suggestion> Suggestions(const std::string& route);   // those seen often enough, for the editor
void Dismiss(const std::string& route, std::size_t index);       // index into Suggestions(route)
void ClearSuggestions(const std::string& route);
int AppendNow(double x, double y, double zLo, double zHi, double radius, double now, std::vector<planworld::Shape>& shapes, std::size_t cap);
}
