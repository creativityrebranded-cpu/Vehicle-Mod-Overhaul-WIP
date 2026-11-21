#pragma once
// World radar (2026-11-13, user design; notes/radar-20261113/DECOMP_FINDINGS.md). Each vehicle sweeps its own surroundings: a rotating ray
// at a radius (a full turn every few ticks) and a forward fan from left to right across its path, as far ahead as its time-to-collision
// reach. Every hit is a LOOKUP, not a point: the game's own collision test says whether this vehicle collides with the body at all, and the
// body's own collision shape gives its exact box (the game's calcAabb), turned by the body's rotation. Each vehicle keeps a short local
// list of those boxes; the energy Plan reads them as measured solids. It decides nothing (World input; DECISION_MAP: no writer).
// Pure (schedule, box maths, list rules); the engine side (CastNow, body lookup, collision test) is in vehicleflight.cpp. Tests:
// tests/radar_test.cpp.
#include "planworld.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace radar {
constexpr int kMaxBoxes = 48;
constexpr double kPi = 3.14159265358979323846;
namespace rules {
constexpr int rotateRays = 2;          // rotating rays per tick per vehicle
constexpr int forwardRays = 2;         // forward-fan rays per tick per vehicle
constexpr double radius = 14.0;        // m: the rotating sweep's reach
constexpr double turnSteps = 24;       // rays per full turn (2 a tick: a turn every 12 ticks)
constexpr double drift = .6180339887;  // of one step: each turn starts this much further round, so no gap between rays stays in one place
constexpr double ttc = 2.5;            // s: the forward fan and the path sweep reach this far ahead at the present speed
constexpr double aheadMin = 8.0, aheadMax = 45.0;  // m
constexpr int fanSteps = 5;            // positions across the path, left to right
constexpr double fanMargin = 1.5;      // m beyond the body's half width on each side
constexpr double rayHeights[2] = { .6, 1.3 };  // m over the car's base: low (logs, kerbs, rocks) and body height (branches, walls)
constexpr double keepStatic = 4.0;     // s: how long the Plan trusts a static box past the latest time it was listed (the list keeps it while in range)
constexpr double keepDynamic = 1.5;    // s for a body that can move (debris, clutter): it leaves the list this long after its last hit
constexpr double range = 70.0;         // m: further boxes leave the list
constexpr double windowHalf = 30.0;    // m: a body larger than the plan's window (terrain tile, building) cannot be steered round as one solid: its points
constexpr double floorTol = .2;        // m: a hit point this close to the wanted surface is that surface (under the path sweep's 0.3 m lift)
constexpr int floorPoints = 2;         // points that must all lie on the wanted surface before a box is dropped as floor
constexpr int floorContrary = 2;       // points above the wanted surface that end a floor verdict
constexpr std::uint64_t groundLayers = (1ull << 13) | (1ull << 17);  // terrain, ground: the floor, never a box
// Path sweep (2026-11-13, after the 19:14 run's fallen tree): the vehicle's own hull swept along the path it is about to drive.
constexpr int sweepSegments = 4;       // straight pieces per pass
constexpr double segmentMax = 20.0;    // m: longest piece
constexpr double chordError = .2;      // m: a piece ends before any path point it covers would lie further than this off it (bends)
constexpr double sweepLift = .3;       // m: the hull is swept this high over the floor (a kerb lower than this is driven over)
constexpr double sweepSide = .5;       // m: every other pass also sweeps this far to one side (the follower's tracking error), alternating sides
// What a contact is (floor or not, and how high over the floor the wheels still roll) is the one surface rule (surface.h) with the one car
// record's climb (carcap.h, D2 2026-11-19); this file's own floor rule (normal z over 0.5, at most 0.4 m over the floor) is removed.
constexpr double pointKeep = 1.5;      // s: a hit point of a moving body, or one no longer listed
// Airy bodies (2026-11-13 19:55 run, S1: the fallen tree's box filled the road and the car stood 9 m short of an open half). A body's box
// is mostly air when a ray or the swept hull passes through the box's core without touching the body. Such a body is its hit points.
constexpr double airCore = .7;         // the core: this share of the box's half extents (a rock's corners are air; its core is not)
constexpr double airCross = .5;        // m a line must run inside the core
constexpr int airEvidence = 2;         // passes that prove it
constexpr double pointCell = .4;       // m: hit points of one body closer than this are one point
// Side sweeps: each pass also sweeps the hull along the path shifted to one side, cycling through these (m, + = right), so the Plan
// knows which shifted lines are clear (a way round) before it chooses one.
constexpr double sideOffsets[6] = { .5, -.5, 1.25, -1.25, 2.0, -2.0 };
}

struct Ray { float from[3] = {}, to[3] = {}; std::uint8_t kind = 0; };  // kind 0 rotating, 1 forward
// What the engine lookup read for one body (vehicleflight.cpp RadarLookup).
struct Lookup {
    bool ok = false; std::uint32_t info = 0; bool dynamic = false; int shapeType = -1;
    float rot[12] = {}; float pos[4] = {}; float lo[4] = {}, hi[4] = {};  // hkTransform (3 x float4) and translation; the shape's own box
};
struct Box {
    std::uint32_t body = 0, info = 0, owner = 0; int layer = -1; bool collides = true, dynamic = false, big = false;
    double cx = 0, cy = 0, zlo = 0, zhi = 0, heading = 0, halfW = 0, halfL = 0;  // oriented footprint, FVO heading (atan2(dx, dy))
    double hitX = 0, hitY = 0, hitZ = 0;  // the latest hit (kept as the shape of a big body)
    double lastHit = -1e9; int hits = 0; int convention = 0; double gap = 0;
    int air = 0; bool airy = false;       // passes through the box's core that touched nothing of it; airy = its box is not its solid
#ifndef FVO_RELEASE
    const char* sensor = nullptr;
#endif
    int shapeType = -1;                   // hknpShapeType (byte at shape + 0x1A); -1 unknown
    bool floor = false; int onFloor = 0, above = 0; double aboveMax = 0;  // the wanted surface continuing (no shape); its evidence (Judge)
};
struct Stats { long rays = 0, hits = 0, lookups = 0, lookupFailed = 0, collides = 0, passes = 0, ground = 0, big = 0, ownBody = 0, conventionA = 0, conventionB = 0;
                long sweeps = 0, sweepRefused = 0, sweepFault = 0, sweepBudget = 0, contacts = 0, floor = 0, floorBodies = 0, verified = 0, verifyDropped = 0;
                long airPasses = 0, airy = 0, sideSweeps = 0;
                long pointsDropped = 0; double dropFar = 0, dropNear = 1e9;  // MP-2: points dropped from a full store, the farthest and nearest dropped (m from the new point)
                unsigned sweepMicrosMax = 0; double sweepMicros = 0; };
// MP-2 (2026-11-14 19:44 run: 256 points full on the ramp, 100-340 contacts a sweep, the oldest dropped: rail points beside the car came and
// went and the plan met "new" walls): 1024 points a car, allocated when its radar first keeps one; a full store drops the point farthest
// from the new one (behind the car, far away), not the oldest.
constexpr int kMaxPoints = 1024;
struct Point { double x = 0, y = 0, z = 0, seenAt = -1e9; std::uint32_t body = 0, owner = 0; int layer = -1; double nz = 0; bool haveNormal = false;
#ifndef FVO_RELEASE
    const char* sensor = nullptr;
#endif
};
// A measured floor point (the path sweep's floor contacts and the points of bodies judged floor): the surface really there.
struct State {
    double angle = 0; int fan = 0; int height = 0; int turnStep = 0;
    Box boxes[kMaxBoxes]; int count = 0;
    std::vector<Point> points; int pointCount = 0;  // MP-2: sized kMaxPoints on first use (cars without radar hold none)
    int verifyNext = 0; int sweepPass = 0; unsigned long long sweptFrame = 0;
    Stats stats; double logAt = -1e9;
};

// The rays for this tick: the rotating pair, then the forward pair (left to right across the path at the TTC reach).
inline int Plan(State& st, double x, double y, double base, double heading, double speed, double halfWidth, double halfLength, Ray* out, int max) {
    int n = 0; const double h = rules::rayHeights[st.height]; st.height = 1 - st.height;
    const double start = std::max(halfWidth, halfLength) + .4;  // outside the own hull
    for (int i = 0; i < rules::rotateRays && n < max; ++i) {
        const double step = 2 * kPi / rules::turnSteps; const double a = st.angle;
        st.angle = std::fmod(st.angle + step, 2 * kPi);
        if (++st.turnStep >= int(rules::turnSteps)) { st.turnStep = 0; st.angle = std::fmod(st.angle + step * rules::drift, 2 * kPi); }  // a turn done: the next starts part of a step further round
        Ray r; r.kind = 0; const double sx = std::sin(a), sy = std::cos(a);
        r.from[0] = float(x + sx * start); r.from[1] = float(y + sy * start); r.from[2] = float(base + h);
        r.to[0] = float(x + sx * rules::radius); r.to[1] = float(y + sy * rules::radius); r.to[2] = float(base + h);
        out[n++] = r;
    }
    const double ahead = std::clamp(std::fabs(speed) * rules::ttc, rules::aheadMin, rules::aheadMax);
    const double fx = std::sin(heading), fy = std::cos(heading), rx = std::cos(heading), ry = -std::sin(heading);
    const double span = halfWidth + rules::fanMargin;
    for (int i = 0; i < rules::forwardRays && n < max; ++i) {
        const int k = st.fan; st.fan = (st.fan + 1) % rules::fanSteps;
        const double lat = -span + 2 * span * k / (rules::fanSteps - 1);
        Ray r; r.kind = 1;
        r.from[0] = float(x + fx * (halfLength + .3) + rx * lat * .5); r.from[1] = float(y + fy * (halfLength + .3) + ry * lat * .5); r.from[2] = float(base + h);
        r.to[0] = float(x + fx * ahead + rx * lat); r.to[1] = float(y + fy * ahead + ry * lat); r.to[2] = float(base + h);
        out[n++] = r;
    }
    return n;
}

// World from the body's local frame, under one of the two layouts the transform's 3 x float4 may have: 0 = columns (world = c0*x + c1*y +
// c2*z + t), 1 = rows (world.i = row_i . p + t). The layout is chosen per body by which one puts the hit point nearer the box's surface.
inline void ToWorld(const Lookup& L, int convention, const double p[3], double w[3]) {
    for (int i = 0; i < 3; ++i) w[i] = L.pos[i] + (convention == 0 ? L.rot[0 + i] * p[0] + L.rot[4 + i] * p[1] + L.rot[8 + i] * p[2]
                                                                  : L.rot[4 * i + 0] * p[0] + L.rot[4 * i + 1] * p[1] + L.rot[4 * i + 2] * p[2]);
}
inline void ToLocal(const Lookup& L, int convention, const double w[3], double p[3]) {
    const double d[3] = { w[0] - L.pos[0], w[1] - L.pos[1], w[2] - L.pos[2] };  // the inverse of a rotation is its transpose
    for (int i = 0; i < 3; ++i) p[i] = convention == 0 ? L.rot[4 * i + 0] * d[0] + L.rot[4 * i + 1] * d[1] + L.rot[4 * i + 2] * d[2]
                                                       : L.rot[0 + i] * d[0] + L.rot[4 + i] * d[1] + L.rot[8 + i] * d[2];
}
inline double SurfaceGap(const Lookup& L, int convention, double hx, double hy, double hz) {
    const double w[3] = { hx, hy, hz }; double p[3]; ToLocal(L, convention, w, p); double s = 0;
    for (int i = 0; i < 3; ++i) { const double c = .5 * (L.lo[i] + L.hi[i]), e = .5 * (L.hi[i] - L.lo[i]); const double o = std::max(0.0, std::fabs(p[i] - c) - e); s += o * o; }
    return std::sqrt(s);
}
// The body's oriented footprint and height range from its shape's own box and transform. False: an unusable box.
inline bool BoxFrom(const Lookup& L, double hx, double hy, double hz, Box& b, int forceConvention = -1) {
    if (!L.ok) return false;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(L.lo[i]) || !std::isfinite(L.hi[i]) || L.hi[i] < L.lo[i] || L.hi[i] - L.lo[i] > 4000) return false;
    const double gA = SurfaceGap(L, 0, hx, hy, hz), gB = SurfaceGap(L, 1, hx, hy, hz);
    const int cv = forceConvention == 0 || forceConvention == 1 ? forceConvention : gA <= gB ? 0 : 1; b.convention = cv; b.gap = cv == 0 ? gA : gB;
    const double c[3] = { .5 * (L.lo[0] + L.hi[0]), .5 * (L.lo[1] + L.hi[1]), .5 * (L.lo[2] + L.hi[2]) };
    const double e[3] = { .5 * (L.hi[0] - L.lo[0]), .5 * (L.hi[1] - L.lo[1]), .5 * (L.hi[2] - L.lo[2]) };
    double wc[3]; ToWorld(L, cv, c, wc);
    // The body's three axes in the world, and how far the box reaches up and across along each.
    double ax[3][3];
    for (int k = 0; k < 3; ++k) { const double u[3] = { k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0 }; double w0[3], o[3] = { 0, 0, 0 }, w1[3];
        ToWorld(L, cv, u, w1); ToWorld(L, cv, o, w0); for (int i = 0; i < 3; ++i) ax[k][i] = w1[i] - w0[i]; }
    double up = 0; for (int k = 0; k < 3; ++k) up += e[k] * std::fabs(ax[k][2]);
    b.zlo = wc[2] - up; b.zhi = wc[2] + up; b.cx = wc[0]; b.cy = wc[1];
    // Footprint: along = the horizontal direction of the body axis with the longest horizontal reach; the box's reach projected on it and across.
    int along = 0; double best = -1;
    for (int k = 0; k < 3; ++k) { const double r = e[k] * std::hypot(ax[k][0], ax[k][1]); if (r > best) { best = r; along = k; } }
    double dx = ax[along][0], dy = ax[along][1]; const double dl = std::hypot(dx, dy);
    if (dl < 1e-6) { dx = 0; dy = 1; } else { dx /= dl; dy /= dl; }
    b.heading = std::atan2(dx, dy);
    b.halfL = 0; b.halfW = 0;
    for (int k = 0; k < 3; ++k) { b.halfL += e[k] * std::fabs(ax[k][0] * dx + ax[k][1] * dy); b.halfW += e[k] * std::fabs(ax[k][0] * dy - ax[k][1] * dx); }
    b.big = std::max({ b.halfL, b.halfW }) > rules::windowHalf;
    b.hitX = hx; b.hitY = hy; b.hitZ = hz;
    return true;
}

// The own car's measured size (2026-11-19, "one car": the planner's size is the car's collision box, not a guessed default). The body's
// shape box measured along the car's heading (forward = sin h, cos h) and across it. The transform layout is the one that puts one of the
// body's horizontal axes nearest the heading (a car's frame has an axis along its nose; the wrong layout turns the axes the other way).
// False: an unusable box. Plausible car sizes only (half width 0.4-2.0 m, half length 0.8-4.0 m).
struct OwnSize { double halfWidth = 0, halfLength = 0, height = 0; int convention = 0; };
inline bool OwnSizeFrom(const Lookup& L, double heading, OwnSize& o) {
    o = OwnSize{};
    if (!L.ok || !std::isfinite(heading)) return false;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(L.lo[i]) || !std::isfinite(L.hi[i]) || L.hi[i] < L.lo[i]) return false;
    const double fx = std::sin(heading), fy = std::cos(heading), rx = std::cos(heading), ry = -std::sin(heading);
    const double e[3] = { .5 * (L.hi[0] - L.lo[0]), .5 * (L.hi[1] - L.lo[1]), .5 * (L.hi[2] - L.lo[2]) };
    double ax[2][3][3]; double bestAlign = -1; int cv = 0;
    for (int c = 0; c < 2; ++c) for (int k = 0; k < 3; ++k) {
        const double u[3] = { k == 0 ? 1.0 : 0.0, k == 1 ? 1.0 : 0.0, k == 2 ? 1.0 : 0.0 }, z0[3] = { 0, 0, 0 }; double w0[3], w1[3];
        ToWorld(L, c, u, w1); ToWorld(L, c, z0, w0); for (int i = 0; i < 3; ++i) ax[c][k][i] = w1[i] - w0[i];
        const double hl = std::hypot(ax[c][k][0], ax[c][k][1]); if (hl < .5) continue;  // a mostly vertical axis says nothing about the heading
        const double align = std::fabs(ax[c][k][0] * fx + ax[c][k][1] * fy) / hl;
        if (align > bestAlign + 1e-6) { bestAlign = align; cv = c; }
    }
    o.convention = cv;
    for (int k = 0; k < 3; ++k) { const double* a = ax[cv][k];
        o.halfLength += e[k] * std::fabs(a[0] * fx + a[1] * fy); o.halfWidth += e[k] * std::fabs(a[0] * rx + a[1] * ry); o.height += 2 * e[k] * std::fabs(a[2]); }
    return std::isfinite(o.halfWidth) && std::isfinite(o.halfLength) && std::isfinite(o.height)
        && o.halfWidth >= .4 && o.halfWidth <= 2.0 && o.halfLength >= .8 && o.halfLength <= 4.0;
}

// Is a body's physics shape its own box? (2026-11-14, decomp: hknpShapeType::Enum names at 0x143BA0980, value = index.) Primitives and convex
// shapes (0 CONVEX, 1 SPHERE, 2 CAPSULE, 3 CYLINDER, 4 TRIANGLE, 5 BOX, 16 SCALED_CONVEX): the bounding box is the solid. Every other shape
// (8 COMPRESSED_MESH, 9 EXTERN_MESH, 11 COMPOUND, 13 HEIGHT_FIELD, 17-18 MASKED, 19 LOD, ...) is a surface or a set of parts: its bounding box
// is mostly air (the ramp's and the rock-lined road's 20 m pieces, the fallen tree), so it gives the plan its real hit points instead.
inline bool BoxIsSolid(int shapeType) { return (shapeType >= 0 && shapeType <= 5) || shapeType == 16; }
// Record a looked-up body (new or seen again). The list is capped: the stalest entry makes room.
inline Box* Note(State& st, const Box& in, double now) {
    for (int i = 0; i < st.count; ++i) if (st.boxes[i].body == in.body) {
        Box& b = st.boxes[i]; const int hits = b.hits + 1;
        const bool same = std::hypot(in.cx - b.cx, in.cy - b.cy) < .5 && std::fabs(in.zlo - b.zlo) < .5 && std::fabs(in.halfL - b.halfL) < .5 && std::fabs(in.halfW - b.halfW) < .5;
        const Box was = b; b = in; b.hits = hits; b.lastHit = now;
        if (same) { b.air = was.air; b.airy = was.airy; b.floor = was.floor; b.onFloor = was.onFloor; b.above = was.above; b.aboveMax = was.aboveMax; }  // verdicts survive a re-note
        return &b; }
    int slot = st.count;
    if (st.count >= kMaxBoxes) { slot = 0; for (int i = 1; i < st.count; ++i) if (st.boxes[i].lastHit < st.boxes[slot].lastHit) slot = i; }
    else ++st.count;
    st.boxes[slot] = in; st.boxes[slot].hits = 1; st.boxes[slot].lastHit = now; return &st.boxes[slot];
}
// A hit on a body already in the list refreshes it without a lookup (its box does not change while it is static).
inline bool Refresh(State& st, std::uint32_t body, double now, double hx, double hy, double hz) {
    for (int i = 0; i < st.count; ++i) if (st.boxes[i].body == body && !st.boxes[i].dynamic) {
        auto& b = st.boxes[i]; b.lastHit = now; ++b.hits; b.hitX = hx; b.hitY = hy; b.hitZ = hz; return true; }
    return false;
}
inline void Expire(State& st, double now, double x, double y) {
    int w = 0;
    for (int i = 0; i < st.count; ++i) {
        const Box& b = st.boxes[i];
        // A static body does not leave while it is in range (2026-11-13: the 19:14 run's tree was boxed 32 s before the car hit it, then dropped
        // 4 s later for want of a fresh hit). It is checked against the game instead (vehicleflight.cpp RadarVerify): gone or moved, it leaves.
        if ((b.dynamic && now - b.lastHit > rules::keepDynamic) || std::hypot(b.cx - x, b.cy - y) > rules::range + std::max(b.halfL, b.halfW)) continue;
        st.boxes[w++] = b;
    }
    st.count = w;
    // A hit point of a listed static body stays while the body does (it does not move); any other leaves pointKeep after its last hit.
    int p = 0;
    for (int i = 0; i < st.pointCount; ++i) {
        const Point& q = st.points[i]; bool held = false;
        for (int k = 0; k < st.count && !held; ++k) held = st.boxes[k].body == q.body && !st.boxes[k].dynamic;
        if ((held || now - q.seenAt <= rules::pointKeep) && now >= q.seenAt - 1e-9) st.points[p++] = q;
    }
    st.pointCount = p;
}
// Does the line (x0, y0, z0) -> (x1, y1, z1) run more than airCross metres inside the core of box b? (b's footprint and height, shrunk to
// airCore of its half extents.)
inline bool CrossesCore(const Box& b, double x0, double y0, double z0, double x1, double y1, double z1) {
    const double ux = std::sin(b.heading), uy = std::cos(b.heading);
    auto local = [&](double x, double y, double& a, double& c) { const double dx = x - b.cx, dy = y - b.cy; a = dx * ux + dy * uy; c = dx * uy - dy * ux; };
    double a0, c0, a1, c1; local(x0, y0, a0, c0); local(x1, y1, a1, c1);
    const double zc = .5 * (b.zlo + b.zhi), zh = .5 * (b.zhi - b.zlo) * rules::airCore;
    const double lo[3] = { -b.halfL * rules::airCore, -b.halfW * rules::airCore, zc - zh }, hi[3] = { b.halfL * rules::airCore, b.halfW * rules::airCore, zc + zh };
    const double p0[3] = { a0, c0, z0 }, d[3] = { a1 - a0, c1 - c0, z1 - z0 };
    double t0 = 0, t1 = 1;
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(d[k]) < 1e-12) { if (p0[k] < lo[k] || p0[k] > hi[k]) return false; continue; }
        double ta = (lo[k] - p0[k]) / d[k], tb = (hi[k] - p0[k]) / d[k]; if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta); t1 = std::min(t1, tb); if (t0 >= t1) return false;
    }
    return (t1 - t0) * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) > rules::airCross;
}
// A line that touched nothing of these bodies (`touched`, n of them) proves air in every other listed box whose core it crosses. Returns
// how many boxes became airy now (their index in `turned`, up to maxTurned).
inline int NoteAir(State& st, double x0, double y0, double z0, double x1, double y1, double z1, const std::uint32_t* touched, int n, int* turned, int maxTurned) {
    int out = 0;
    for (int i = 0; i < st.count; ++i) {
        Box& b = st.boxes[i]; if (b.airy || b.big || b.floor || !b.collides) continue;
        bool hit = false; for (int k = 0; k < n && !hit; ++k) hit = touched[k] == b.body;
        if (hit || !CrossesCore(b, x0, y0, z0, x1, y1, z1)) continue;
        ++b.air; ++st.stats.airPasses;
        if (b.air >= rules::airEvidence) { b.airy = true; ++st.stats.airy; if (out < maxTurned) turned[out] = i; ++out; }
    }
    return out;
}
// The list's next static box to check against the game (round robin), or -1.
inline int NextToVerify(State& st) {
    for (int k = 0; k < st.count; ++k) { const int i = (st.verifyNext + k) % st.count; if (!st.boxes[i].dynamic) { st.verifyNext = i + 1; return i; } }
    return -1;
}
inline void Remove(State& st, int i) { if (i < 0 || i >= st.count) return; st.boxes[i] = st.boxes[st.count - 1]; --st.count; }
// A hit point of a body (every collides hit is kept; ToShapes gives them for bodies with no usable box: big, the car stands in it, or
// airy). Refreshed when hit again within pointCell.
inline void NotePoint(State& st, std::uint32_t body, std::uint32_t owner, int layer, double x, double y, double z, bool haveNormal, double nz, double now, [[maybe_unused]] const char* sensor = nullptr) {
    const double c = rules::pointCell;
    for (int i = 0; i < st.pointCount; ++i) { auto& q = st.points[i]; if (q.body == body && std::fabs(q.x - x) < c && std::fabs(q.y - y) < c && std::fabs(q.z - z) < c) { q.seenAt = now; return; } }
    if (st.points.size() < std::size_t(kMaxPoints)) st.points.resize(kMaxPoints);
    int slot = st.pointCount;
    if (st.pointCount >= kMaxPoints) {  // MP-2: full: the point farthest from this new one goes (it is behind or far off; the new one is near the car)
        double farthest = -1; slot = 0;
        for (int i = 0; i < st.pointCount; ++i) { const double d2 = (st.points[i].x - x) * (st.points[i].x - x) + (st.points[i].y - y) * (st.points[i].y - y); if (d2 > farthest) { farthest = d2; slot = i; } }
        const double dist = std::sqrt(std::max(0.0, farthest)); ++st.stats.pointsDropped; st.stats.dropFar = std::max(st.stats.dropFar, dist); st.stats.dropNear = std::min(st.stats.dropNear, dist); }
    else ++st.pointCount;
    Point q; q.x = x; q.y = y; q.z = z; q.seenAt = now; q.body = body; q.owner = owner; q.layer = layer; q.haveNormal = haveNormal; q.nz = nz;
#ifndef FVO_RELEASE
    q.sensor = sensor;
#endif
    st.points[slot] = q;
}

// ---- Path sweep ----
// One straight piece of the hull sweep, from (x0, y0) to (x1, y1), the hull turned to `heading` (FVO sense).
struct Segment { double x0 = 0, y0 = 0, x1 = 0, y1 = 0, heading = 0, length = 0; };
// The pieces that cover the path from the car out to `reach` metres: the path's own points (the stretch the car drives now), cut where it
// bends (a covered point more than chordError off the piece) or a piece passes segmentMax, then straight on along its last direction. No
// path: straight along the nose. With more bends than pieces, the last piece runs straight from its start toward the reach (traced).
// The first piece starts at the car and each piece starts where the last ended, so the whole way from the car to the reach is swept.
inline int SweepPlan(const double* px, const double* py, int pn, double x, double y, double heading, double reach, Segment* out, int max) {
    int n = 0; double sx = x, sy = y, travelled = 0, dirH = heading;
    double ex = x, ey = y;  // the end of the piece being built
    auto close = [&](double tx, double ty) {
        const double L = std::hypot(tx - sx, ty - sy); if (L < .05 || n >= max) return;
        Segment s; s.x0 = sx; s.y0 = sy; s.x1 = tx; s.y1 = ty; s.length = L; s.heading = std::atan2(tx - sx, ty - sy); out[n++] = s;
        travelled += L; sx = tx; sy = ty; ex = tx; ey = ty; dirH = s.heading;
    };
    bool reached = false; int from = 0;  // the path points inside the piece being built: [from, i)
    for (int i = 0; i < pn && n < max - 1 && !reached; ++i) {
        const double qx = px[i], qy = py[i];
        if (std::hypot(qx - ex, qy - ey) < .05) continue;
        if (std::hypot(ex - sx, ey - sy) > .05) {
            // Would the straight piece out to this point leave any path point it passes more than chordError off it? Then end the piece here.
            const double L = std::hypot(qx - sx, qy - sy), ux = (qx - sx) / L, uy = (qy - sy) / L; double off = 0;
            for (int j = from; j < i; ++j) off = std::max(off, std::fabs((px[j] - sx) * uy - (py[j] - sy) * ux));
            if (off > rules::chordError || L > rules::segmentMax) { close(ex, ey); from = i; }
        }
        ex = qx; ey = qy;
        const double L = std::hypot(ex - sx, ey - sy);
        if (travelled + L >= reach) { const double keep = std::max(0.0, reach - travelled); if (L > 1e-6) { ex = sx + (ex - sx) * keep / L; ey = sy + (ey - sy) * keep / L; } reached = true; }
    }
    if (!reached && n == max - 1) {  // one piece left and the reach not met: it runs from its start toward the latest path point, out to the reach
        const double L = std::hypot(ex - sx, ey - sy), h = L > .05 ? std::atan2(ex - sx, ey - sy) : dirH, rest = reach - travelled;
        if (rest > .05) close(sx + std::sin(h) * rest, sy + std::cos(h) * rest);
        return n;
    }
    if (std::hypot(ex - sx, ey - sy) > .05) close(ex, ey);
    while (travelled < reach - .05 && n < max) {  // straight on along the last direction (the last piece takes all that is left)
        const double L = n == max - 1 ? reach - travelled : std::min(rules::segmentMax, reach - travelled);
        close(sx + std::sin(dirH) * L, sy + std::cos(dirH) * L);
    }
    return n;
}
// SR-1 (2026-11-19): what a contact is and whether a body is floor is decided by the one surface rule (surface.h) when it is sensed;
// REMOVED here: Classify (TW-1) and the WM-1 Judge.
// The list as World shapes for the Plan: a body the vehicle collides with becomes a measured solid with its real box; one too big to steer round,
// one proven mostly air, or one with no box gives its hit points (as ray hits); the wanted surface continuing (Judge), a body it passes
// through, and the floor (terrain, ground) give nothing.
inline int ToShapes(const State& st, double now, planworld::Shape* out, int max) {
    int n = 0;
    for (int i = 0; i < st.count && n < max; ++i) {
        const Box& b = st.boxes[i];
        if (!b.collides || b.floor || (b.layer >= 0 && b.layer < 64 && ((rules::groundLayers >> b.layer) & 1))) continue;
        if (b.big || b.airy) continue;  // too big to steer round, no box, or a box that is mostly air: its hit points (below), not one box
        planworld::Shape s; s.seenAt = b.lastHit; s.expires = b.dynamic ? b.lastHit + rules::keepDynamic : now + rules::keepStatic; s.owner = b.owner; s.layer = b.layer;
        {
            s.source = planworld::Source::Radar; s.centre = { b.cx, b.cy, .5 * (b.zlo + b.zhi) };
            s.halfWidth = std::max(.1, b.halfW); s.halfLength = std::max(.1, b.halfL); s.halfHeight = std::max(.05, .5 * (b.zhi - b.zlo)); s.heading = b.heading;
        }
#ifndef FVO_RELEASE
        s.sensor = b.sensor ? b.sensor : "radar_box"; s.sensorBody = b.body;
#endif
        out[n++] = s;
    }
    for (int i = 0; i < st.pointCount && n < max; ++i) {
        const Point& q = st.points[i]; bool boxed = false, held = false;
        for (int k = 0; k < st.count; ++k) if (st.boxes[k].body == q.body) {
            const Box& b = st.boxes[k]; held = !b.dynamic; boxed = (!b.big && !b.airy) || b.floor; if (!b.collides) boxed = true; break; }
        if (boxed) continue;  // its body is a solid box already (or passes through): the box speaks for it
        planworld::Shape s; s.source = planworld::Source::Ray; s.centre = { q.x, q.y, q.z };
        s.seenAt = q.seenAt; s.expires = held ? now + rules::pointKeep : q.seenAt + rules::pointKeep; s.owner = q.owner; s.layer = q.layer; s.haveNormal = q.haveNormal; s.normalZ = q.nz;
#ifndef FVO_RELEASE
        s.sensor = q.sensor ? q.sensor : "radar_point"; s.sensorBody = q.body;
#endif
        s.floorProbeBody = held ? q.body : 0;
        out[n++] = s;
    }
    return n;
}
}
