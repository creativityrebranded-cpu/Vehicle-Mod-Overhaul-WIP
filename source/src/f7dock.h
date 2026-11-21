#pragma once
// The F7 dock (2026-11-06): pure parts shared by the editor (docklayout.inl) and tests/f7dock_test.cpp. Design: notes/traffic-editor-html/
// (Show = Proposal: main dock). Levels Routes > Route > Tile / Point, plus Types, Arrive and City. Selection rules (user, 2026-11-06): click a
// point = that point; click a line = the tile under the click; double-click a line = every tile of the route; double-click a point = every
// point; Ctrl+click adds or removes one point or tile. Points and tiles are never mixed. Edits apply to every selected item.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace f7dock {
// Problems: card RI-3 (route item warnings). Places .. Wires: notes/f7-restore-20261108/CHANGE_CARDS.md (R3-R6), the restored places,
// Nodes view and route extras, each a bottom-bar menu.
enum Level { Routes = 0, Route = 1, Tile = 2, Point = 3, Types = 4, Arrive = 5, City = 6, Problems = 7,
    Places = 8, Landing = 9, Zone = 10, Junction = 11, Nodes = 12, Extras = 13, Access = 14, Wires = 15, TrafficLevels = 16, LevelCount = 17 };
inline const char* LevelName(int l) {
    constexpr const char* n[]{ "routes", "route", "tile", "point", "types", "arrive", "city", "problems", "places", "landing", "zone", "junction", "nodes", "extras", "access", "wires", "traffic levels" };
    return l >= 0 && l < LevelCount ? n[l] : "?";
}
// A selected place's menus (a landing, its access and wires, a zone, a junction): they follow the map's selection.
inline bool PlaceSelected(int l) { return l == Landing || l == Zone || l == Junction || l == Access || l == Wires; }
inline bool PlaceLevel(int l) { return l == Places || PlaceSelected(l); }
// Problems grouped by where they are on the route (card R6): consecutive points / tiles close together share a group.
struct Group { int kind = 0, first = -1, last = -1; std::vector<int> rows; };  // kind 0 points, 1 tiles, 2 the whole route
inline std::vector<Group> GroupRows(const std::vector<std::pair<int, int>>& where, int join = 2) {
    // where[i] = { kind, index } (index -1: the whole route). Rows keep their order inside a group; groups go by kind, then position.
    std::vector<Group> out;
    std::vector<int> order(where.size()); for (std::size_t i = 0; i < where.size(); ++i) order[i] = int(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { const auto& x = where[std::size_t(a)]; const auto& y = where[std::size_t(b)];
        const int kx = x.second < 0 ? 2 : x.first, ky = y.second < 0 ? 2 : y.first; return kx != ky ? kx < ky : x.second < y.second; });
    for (int i : order) {
        const auto& w = where[std::size_t(i)]; const int kind = w.second < 0 ? 2 : w.first;
        if (!out.empty() && out.back().kind == kind && (kind == 2 || w.second - out.back().last <= join)) { out.back().last = std::max(out.back().last, w.second); out.back().rows.push_back(i); continue; }
        Group g; g.kind = kind; g.first = g.last = w.second; g.rows.push_back(i); out.push_back(g);
    }
    return out;
}
enum Kind { None = 0, Points = 1, Tiles = 2 };

inline int TileCount(int points, bool closed) { return closed ? (points >= 2 ? points : 0) : std::max(0, points - 1); }

// One kind of item, a primary (shown in the dock and the cube: the last clicked) and the list. The list always holds the primary.
struct Selection {
    int kind = None, primary = -1;
    std::vector<int> items;
    int Count() const { return int(items.size()); }
    bool Has(int i) const { return std::find(items.begin(), items.end(), i) != items.end(); }
    bool Many() const { return items.size() >= 2; }
};
inline Selection One(int kind, int i) { Selection s; s.kind = kind; s.primary = i; s.items = { i }; return s; }
inline Selection All(int kind, int count, int keep) {
    Selection s; if (count <= 0) return s;
    s.kind = kind; s.primary = std::clamp(keep, 0, count - 1); for (int i = 0; i < count; ++i) s.items.push_back(i); return s;
}
// Ctrl+click. Returns what happened, for the status line and the trace.
enum Toggle { Started = 0, Added = 1, Removed = 2, Emptied = 3, Switched = 4 };
inline Toggle Flip(Selection& s, int kind, int i) {
    if (s.kind != kind || s.items.empty()) { const bool other = s.kind != None && !s.items.empty() && s.kind != kind; s = One(kind, i); return other ? Switched : Started; }
    const auto at = std::find(s.items.begin(), s.items.end(), i);
    if (at == s.items.end()) { s.items.push_back(i); s.primary = i; return Added; }
    s.items.erase(at);
    if (s.items.empty()) { s = {}; return Emptied; }
    if (s.primary == i) s.primary = s.items.back();
    return Removed;
}
// Points or tiles were added or removed elsewhere (undo, delete): drop what no longer exists; the primary falls back to the last one left.
inline void Clamp(Selection& s, int count) {
    std::erase_if(s.items, [&](int i) { return i < 0 || i >= count; });
    if (s.items.empty()) { s = {}; return; }
    if (!s.Has(s.primary)) s.primary = s.items.back();
}
// A number typed in a picker (1-based): the index it goes to, and whether it was out of range (then the nearest real one).
inline std::pair<int, bool> Typed(int typed, int count) {
    if (count <= 0) return { -1, true };
    const int i = std::clamp(typed, 1, count) - 1; return { i, typed < 1 || typed > count };
}
// < and >: wrap at the ends. From no current item, > goes to the first and < to the last.
inline int Step(int current, int by, int count) {
    if (count <= 0) return -1;
    if (current < 0) return by > 0 ? 0 : count - 1;
    return ((current + by) % count + count) % count;
}

// The tile under a click: the nearest piece of the route on screen. `screen` holds each point's position on screen, or nothing (behind
// the camera). Returns -1 when no piece is within `reach` pixels.
struct Screen { double x = 0, y = 0; bool seen = false; };
inline double SegmentDistance(double px, double py, double ax, double ay, double bx, double by) {
    const double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
    const double t = l > 1e-9 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l, 0.0, 1.0) : 0.0;
    return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
}
inline int TileAt(const std::vector<Screen>& screen, bool closed, double mx, double my, double reach) {
    const int n = int(screen.size()), tiles = TileCount(n, closed);
    int best = -1; double bestD = reach;
    for (int t = 0; t < tiles; ++t) {
        const auto& a = screen[std::size_t(t)]; const auto& b = screen[std::size_t((t + 1) % n)];
        if (!a.seen || !b.seen) continue;
        const double d = SegmentDistance(mx, my, a.x, a.y, b.x, b.y);
        if (d < bestD) { bestD = d; best = t; }
    }
    return best;
}

// Two clicks on the same spot within this time make a double-click.
struct Clicks {
    std::uint64_t at = 0; double x = -1e9, y = -1e9;
    static constexpr std::uint64_t kWindowMs = 400; static constexpr double kReachPx = 8;
    // Call on every click; true when it completes a double-click (a third click then starts over).
    bool Double(std::uint64_t now, double cx, double cy) {
        const bool two = at && now - at <= kWindowMs && std::hypot(cx - x, cy - y) <= kReachPx;
        if (two) { at = 0; x = y = -1e9; return true; }
        at = now; x = cx; y = cy; return false;
    }
};

// Faces: bit 0 bottom, 1 top, 2 left, 3 right, 4 front, 5 back; 1 = Touch.
inline const char* FaceName(int f) { constexpr const char* n[]{ "Bottom", "Top", "Left", "Right", "Front", "Back" }; return f >= 0 && f < 6 ? n[f] : "?"; }
inline int SetFace(int faces, int face, bool touch) { return touch ? faces | (1 << face) : faces & ~(1 << face); }
inline bool Touch(int faces, int face) { return ((faces >> face) & 1) != 0; }
// The name the cube shows for a tile: Hover only (all Avoid), Drive only / Both (bottom Touch only, by travel), Wall (back only), else Custom.
inline const char* TravelName(int faces, int travel) {
    if (faces == 0) return "Hover only";
    if (faces == 1) return travel == 1 ? "Drive only" : "Both";
    if (faces == 32) return "Wall";
    return "Custom";
}

// A typed value box: digits, one point, a leading minus. Enter commits, Esc cancels, Tab moves on.
struct Entry {
    int field = -1; std::string text; bool replace = true;
    bool Active() const { return field >= 0; }
    void Begin(int f, const std::string& shown) { field = f; text = shown; replace = true; }
    void Key(char c) {
        if (replace) { text.clear(); replace = false; }
        if (text.size() >= 16) return;
        if (c == '-' && !text.empty()) return;
        if (c == '.' && text.find('.') != std::string::npos) return;
        text.push_back(c);
    }
    void Back() { if (replace) { text.clear(); replace = false; } else if (!text.empty()) text.pop_back(); }
    void End() { field = -1; text.clear(); replace = true; }
    bool Number(double& v) const {
        if (text.empty() || text == "-" || text == "." || text == "-.") return false;
        char* end = nullptr; v = std::strtod(text.c_str(), &end); return end && !*end && std::isfinite(v);
    }
};
// The number in a shown value ("12.5 m" -> "12.5"), for starting an entry.
inline std::string Leading(const std::string& s) {
    std::size_t i = 0; std::string out;
    if (i < s.size() && s[i] == '-') out += s[i++];
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.')) out += s[i++];
    return out == "-" ? std::string() : out;
}
}
