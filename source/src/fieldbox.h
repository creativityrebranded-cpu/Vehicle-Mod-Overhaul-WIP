#pragma once
// Field boxes (2026-11-06, the user: "from now on it's only box and then orientation of the box"). A route's field is a chain of
// oriented boxes, one per tile. Each of the six faces is Touch (the car is meant to be on it: the bottom of a road touches the ground; a
// wall set to Touch gives the context for rising up a surface) or Avoid (keep off it and do not leave through it). Faces box a car in
// but never keep it out: containment starts only once the whole car is inside, so entering a field is always allowed.
// notes/VEHICLE_DECISION_DESIGN_20261106.md section 3. Pure; tests: tests/fieldbox_test.cpp.
// Game convention: yaw atan2(x, y), forward = (sin, cos), right = (cos, -sin), z up.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace fieldbox {
struct V3 { double x = 0, y = 0, z = 0; };
inline V3 Add(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 Sub(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 Mul(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

enum Face : int { Bottom = 0, Top = 1, Left = 2, Right = 3, Front = 4, Back = 5, FaceCount = 6 };
enum class State : std::uint8_t { Avoid = 0, Touch = 1 };
inline const char* Name(Face f) { constexpr const char* n[]{ "bottom", "top", "left", "right", "front", "back" }; return n[f]; }
inline const char* Name(State s) { return s == State::Touch ? "touch" : "avoid"; }

struct Axes { V3 forward, right, up; };

struct Box {
    V3 centre;
    double halfWidth = 3.5, halfLength = 5, halfHeight = 2.5;
    double yaw = 0, pitch = 0, roll = 0;  // radians; pitch + = nose up, roll + = right side down
    std::array<State, FaceCount> faces{ State::Touch, State::Avoid, State::Avoid, State::Avoid, State::Avoid, State::Avoid };
    bool solids = false;  // D14: solids inside are allowed (a route through a structure); the editor keeps its red warning
};

inline Axes AxesOf(const Box& b) {
    const double sy = std::sin(b.yaw), cy = std::cos(b.yaw), sp = std::sin(b.pitch), cp = std::cos(b.pitch), sr = std::sin(b.roll), cr = std::cos(b.roll);
    const V3 f0{ sy, cy, 0 }, r0{ cy, -sy, 0 }, u0{ 0, 0, 1 };
    const V3 f = Add(Mul(f0, cp), Mul(u0, sp)), u1 = Add(Mul(f0, -sp), Mul(u0, cp));
    return { f, Sub(Mul(r0, cr), Mul(u1, sr)), Add(Mul(r0, sr), Mul(u1, cr)) };
}
// A world point in the box's own frame: x right, y forward, z up, from its centre.
inline V3 Local(const Box& b, V3 p) {
    const auto a = AxesOf(b); const V3 d = Sub(p, b.centre);
    return { Dot(d, a.right), Dot(d, a.forward), Dot(d, a.up) };
}
inline V3 World(const Box& b, V3 l) {
    const auto a = AxesOf(b);
    return Add(b.centre, Add(Mul(a.right, l.x), Add(Mul(a.forward, l.y), Mul(a.up, l.z))));
}
inline bool Contains(const Box& b, V3 p, double margin = 0) {
    const V3 l = Local(b, p);
    return std::fabs(l.x) <= b.halfWidth + margin && std::fabs(l.y) <= b.halfLength + margin && std::fabs(l.z) <= b.halfHeight + margin;
}

// The car's own box: position of its centre, heading, half sizes.
struct Body { V3 centre; double heading = 0, halfWidth = 1.2, halfLength = 2.3, halfHeight = 1; };
// Nine test points: the eight corners and the centre.
inline std::array<V3, 9> Points(const Body& c) {
    std::array<V3, 9> out; int n = 0; const double s = std::sin(c.heading), k = std::cos(c.heading);
    for (double a : { -1.0, 1.0 }) for (double f : { -1.0, 1.0 }) for (double u : { -1.0, 1.0 }) {
        const double r = a * c.halfWidth, l = f * c.halfLength;
        out[n++] = { c.centre.x + k * r + s * l, c.centre.y - s * r + k * l, c.centre.z + u * c.halfHeight };
    }
    out[8] = c.centre;
    return out;
}
// Share of the car inside any box of the chain, 0..1. A car may span two tiles: the chain counts as one space.
inline double Share(const Box* boxes, int count, const Body& car) {
    int in = 0;
    for (const V3& p : Points(car)) for (int i = 0; i < count; ++i) if (Contains(boxes[i], p, 1e-6)) { ++in; break; }
    return in / 9.0;
}

// D13: one box per tile leaves a gap on the outside of a bend. Neighbouring boxes are lengthened to meet, so the chain reads as one space.
// For containment only: the wanted surface still reads the boxes as drawn. kJoinMax bounds it on a very sharp corner.
constexpr double kJoinMax = 6.0;
inline void JoinAtBends(Box* b, int n) {
    double grow[16] = {}; n = std::min(n, 16);  // FB-1: up to 12 boxes now come from the addon
    for (int i = 0; i < n; ++i) for (int j = i + 1; j < n; ++j) {
        const double dx = b[i].centre.x - b[j].centre.x, dy = b[i].centre.y - b[j].centre.y, width = std::max(b[i].halfWidth, b[j].halfWidth);
        if (std::hypot(dx, dy) > b[i].halfLength + b[j].halfLength + width) continue;  // not neighbours
        const double turn = std::fabs(std::remainder(b[i].yaw - b[j].yaw, 6.283185307179586));
        if (turn < .02 || turn > 1.4) continue;
        const double need = std::min(kJoinMax, width * std::tan(turn * .5));
        grow[i] = std::max(grow[i], need); grow[j] = std::max(grow[j], need);
    }
    for (int i = 0; i < n; ++i) b[i].halfLength += grow[i];
}

// The same share over the ground plane only: a lane box keeps a car between its side, front and back faces. Authored floors sit up to
// 1.4 m off the real deck, so the height test would read a car on the road as outside its box. Height is the bottom face's job.
inline double ShareFlat(const Box* boxes, int count, const Body& car) {
    int in = 0;
    for (const V3& p : Points(car)) for (int i = 0; i < count; ++i) { const V3 l = Local(boxes[i], p);
        if (std::fabs(l.x) <= boxes[i].halfWidth + 1e-6 && std::fabs(l.y) <= boxes[i].halfLength + 1e-6) { ++in; break; } }
    return in / 9.0;
}
// Entering versus exiting, judged from the car's share at two poses.
enum class Move { Outside, Entering, Inside, Exiting };
inline const char* Name(Move m) { constexpr const char* n[]{ "outside", "entering", "inside", "exiting" }; return n[int(m)]; }
inline Move Judge(double shareNow, double shareNext) {
    if (shareNow >= 1.0 && shareNext >= 1.0) return Move::Inside;
    if (shareNext > shareNow) return Move::Entering;
    if (shareNow <= 0.0 && shareNext <= 0.0) return Move::Outside;
    return shareNext < shareNow ? Move::Exiting : Move::Entering;  // part way in and holding: not leaving
}

// Containment: one way, with a latch so a car can never be trapped on a face. It starts once the whole car has been inside for
// kSettle seconds, and ends if the car is found mostly outside (pushed out, teleported): it may then simply enter again.
constexpr double kSettle = 0.25, kLostShare = 0.5;
struct Containment {
    bool contained = false; double insideSince = -1;
    bool Update(double now, double share) {
        if (share >= 1.0) { if (insideSince < 0) insideSince = now; if (now - insideSince >= kSettle) contained = true; }
        else insideSince = -1;
        if (contained && share < kLostShare) contained = false;
        return contained;
    }
    // A planned move is refused only when contained and it leaves. Entering and moving about inside are never refused.
    bool Refuses(double shareNow, double shareNext) const { return contained && Judge(shareNow, shareNext) == Move::Exiting; }
};

// What a box says about the surface under a point: the Touch bottom's height, or nothing to touch and the height to fly from.
struct Surface { bool inside = false, touch = false, wall = false; double z = 0, baseZ = 0; Face face = Bottom; };
inline Surface SurfaceAt(const Box& b, double x, double y, double z) {
    Surface s; const auto a = AxesOf(b);
    // The bottom plane's height at (x, y): solve along the box's up axis.
    const V3 base = World(b, { 0, 0, -b.halfHeight });
    const double nz = a.up.z;
    if (std::fabs(nz) < 1e-6) return s;
    const double planeZ = base.z - (a.up.x * (x - base.x) + a.up.y * (y - base.y)) / nz;
    const V3 onPlane{ x, y, planeZ }, l = Local(b, onPlane);
    if (std::fabs(l.x) > b.halfWidth + 1e-6 || std::fabs(l.y) > b.halfLength + 1e-6) return s;
    s.inside = true; s.baseZ = planeZ; s.z = planeZ;
    if (b.faces[Bottom] == State::Touch) { s.touch = true; s.face = Bottom; return s; }
    // A wall set to Touch: the surface goes on up that wall; height comes from where the car is on it.
    for (Face f : { Left, Right, Front, Back }) if (b.faces[f] == State::Touch) { s.touch = true; s.wall = true; s.face = f; s.z = z; return s; }
    return s;
}

// Old tile shapes read as boxes (design section 3). Stored values are never rewritten; this is only how they read.
// Kinds as roadplan::Kind: 0 default, 1 flat, 2 box, 3 rounded, 4 tube, 5 trapezoid, 6 open air.
// D11 is NOT approved by the user: rounded, tube and trapezoid tiles give no box (false) and keep today's behaviour exactly. Reading them as
// their enclosing box is a compatibility exception; flip kEnclosingOldShapes only on the user's explicit yes.
constexpr double kDefaultHeight = 5.0;
constexpr bool kEnclosingOldShapes = false;  // D11
inline bool FromKind(Box& b, int kind, double storedHeight) {
    if (!kEnclosingOldShapes && (kind == 3 || kind == 4 || kind == 5)) return false;
    b.halfHeight = (storedHeight > 0 ? storedHeight : kDefaultHeight) * 0.5;
    b.faces = { State::Touch, State::Avoid, State::Avoid, State::Avoid, State::Avoid, State::Avoid };
    if (kind == 6) b.faces[Bottom] = State::Avoid;  // open air: nothing to touch, the car is boxed into its corridor
    return true;
}
}
