#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
// Part A of the full vehicle combat AI (2026-10-21, notes/VEHICLE_COMBAT_AI_FULL_SPEC_20261021.md; the user: "I need these added not simplified / in
// full"): Reynolds' steering behaviours, the whole set, combined by a prioritized truncated weighted sum (Reynolds 1999, Buckland "Programming Game AI
// by Example" ch. 3). Every behaviour returns a steering force (m/s^2, a wanted change of velocity); Combine adds them in priority order with their
// weights until the agent's max force is used up and drops the rest; Integrate turns the force into a desired velocity (clamped to max speed),
// Smooth low-passes heading and speed over ~0.2 s, ToCommands makes yaw rate / throttle / strafe / vertical / pitch for vehicleflight, and
// SafeAhead is the terrain check (a desired velocity that hits terrain within 2.5 s loses to the autopilot's own planner).
// Pure logic: the world (nav map lines of sight, solids, ground) comes in through the World interface. x/y horizontal, z up; heading h faces
// (sin h, cos h). Tests: tests/combat_steer_test.cpp.
namespace steer {
struct V3
{
    double x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 operator*(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
inline double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double Len(V3 a) { return std::sqrt(Dot(a, a)); }
inline double Len2D(V3 a) { return std::hypot(a.x, a.y); }
inline V3 Norm(V3 a) { const double l = Len(a); return l > 1e-9 ? a * (1.0 / l) : V3{}; }
inline V3 Trunc(V3 a, double max) { const double l = Len(a); return l > max && l > 1e-9 ? a * (max / l) : a; }
inline V3 Flat(V3 a) { return { a.x, a.y, 0 }; }
inline V3 Fwd(double h) { return { std::sin(h), std::cos(h), 0 }; }
inline V3 Right(double h) { return { std::cos(h), -std::sin(h), 0 }; }
inline double Wrap(double a) { while (a > 3.141592653589793) a -= 6.283185307179586; while (a < -3.141592653589793) a += 6.283185307179586; return a; }
inline double HeadingOf(V3 v) { return std::atan2(v.x, v.y); }
inline bool Finite(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }

// The world as the steering sees it (the engine side wraps navmap / environment; tests use a fake).
struct World
{
    virtual ~World() = default;
    virtual bool Blocked(const V3& from, const V3& to) const = 0;  // a solid or the terrain between
    virtual bool Solid(const V3& p) const = 0;                     // a mapped solid or object box at p
    virtual double Ground(double x, double y) const = 0;           // terrain height; NaN unknown
};

struct Agent
{
    V3 pos, vel;
    double heading = 0;
    double maxSpeed = 40.0;   // m/s
    double maxForce = 20.0;   // m/s^2
    double maxTurn = 1.6;     // rad/s
    double mass = 1500.0;     // kg
    double radius = 2.5;      // m
    double clearance = 2.0;   // m over the ground (hover)
};
struct Body
{
    V3 pos, vel;
    double heading = 0, radius = 2.5;
};

// A small deterministic generator (wander, jink): xorshift64*.
struct Rng
{
    std::uint64_t s = 0x9E3779B97F4A7C15ull;
    explicit Rng(std::uint64_t seed = 1) : s(seed ? seed * 0x9E3779B97F4A7C15ull : 1) {}
    double Next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return double((s * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0; }  // [0, 1)
    double Signed() { return Next() * 2.0 - 1.0; }
};

// ---- The behaviours ----
inline V3 Seek(const Agent& a, V3 target) { return Norm(target - a.pos) * a.maxSpeed - a.vel; }
inline V3 Flee(const Agent& a, V3 threat, double panic = 1e9)
{
    if (Len(a.pos - threat) > panic) return {};
    return Norm(a.pos - threat) * a.maxSpeed - a.vel;
}
// Slows down inside slowRadius (parking, held spots, formation slots only).
inline V3 Arrive(const Agent& a, V3 target, double slowRadius = 15.0)
{
    const V3 to = target - a.pos;
    const double d = Len(to);
    if (d < 0.3) return a.vel * -1.0;
    const double speed = std::min(a.maxSpeed, a.maxSpeed * d / std::max(1.0, slowRadius));
    return to * (speed / d) - a.vel;
}
// The look-ahead for pursuit / evasion: distance over the speeds, shorter when closing fast.
inline double Lookahead(const Agent& a, const Body& t)
{
    const V3 to = t.pos - a.pos;
    const double d = Len(to);
    const double closing = std::max(0.0, Dot(a.vel - t.vel, Norm(to)));
    return std::clamp(d / std::max(1.0, a.maxSpeed + closing), 0.0, 3.0);
}
inline V3 Predict(const Body& t, double s) { return t.pos + t.vel * s; }
inline V3 Pursuit(const Agent& a, const Body& t) { return Seek(a, Predict(t, Lookahead(a, t))); }
inline V3 Evasion(const Agent& a, const Body& t, double panic = 1e9) { return Flee(a, Predict(t, Lookahead(a, t)), panic); }
// A point offset from a moving body in its own frame (x right, y forward, z up): formation slots, flank positions, the pincer.
inline V3 OffsetPoint(const Body& t, V3 local) { return t.pos + Right(t.heading) * local.x + Fwd(t.heading) * local.y + V3{ 0, 0, local.z }; }
inline V3 OffsetPursuit(const Agent& a, const Body& leader, V3 local, double slowRadius = 10.0)
{
    const V3 slot = OffsetPoint(leader, local);
    const double la = Len(slot - a.pos) / std::max(1.0, a.maxSpeed + Len(leader.vel));
    return Arrive(a, slot + leader.vel * la, slowRadius);
}
// Between two moving bodies (an escort between the player and a threat).
inline V3 Interpose(const Agent& a, const Body& p, const Body& q)
{
    const V3 mid = (p.pos + q.pos) * 0.5;
    const double t = Len(mid - a.pos) / std::max(1.0, a.maxSpeed);
    const V3 midT = (Predict(p, t) + Predict(q, t)) * 0.5;
    return Arrive(a, midT, 8.0);
}
// Hide: the nearest point on a ring around me (20 / 40 / 60 m, 12 directions) that the threat cannot see (the far side of terrain or an
// obstacle); none: evade. `found` reports whether a hiding place was found and `spot` where.
inline V3 Hide(const Agent& a, const Body& threat, const World& w, bool* found = nullptr, V3* spot = nullptr)
{
    double best = 1e18;
    V3 pick{};
    const V3 eye = threat.pos + V3{ 0, 0, 1.5 };
    for (double r : { 20.0, 40.0, 60.0 }) {
        for (int k = 0; k < 12; ++k) {
            const double ang = k * 0.5235987755982988;
            V3 p = a.pos + V3{ std::sin(ang) * r, std::cos(ang) * r, 0 };
            const double g = w.Ground(p.x, p.y);
            p.z = std::isfinite(g) ? g + a.clearance + 0.5 : a.pos.z;
            if (w.Solid(p) || !w.Blocked(eye, p + V3{ 0, 0, 1.0 })) continue;
            const double score = r + Len(p - threat.pos) * -0.2;  // near me, not toward the threat
            if (score < best) { best = score; pick = p; }
        }
        if (best < 1e17) break;
    }
    if (found) *found = best < 1e17;
    if (best < 1e17) { if (spot) *spot = pick; return Arrive(a, pick, 10.0); }
    return Evasion(a, threat);
}
// Wander: a target on a circle projected ahead, jittered a little each tick; never sitting still.
struct WanderState { double angle = 0; };
inline V3 Wander(const Agent& a, WanderState& ws, Rng& rng, double dt, double dist = 12.0, double radius = 6.0, double jitter = 3.0)
{
    ws.angle = Wrap(ws.angle + rng.Signed() * jitter * dt);
    const double h = Len2D(a.vel) > 1.0 ? HeadingOf(a.vel) : a.heading;
    const V3 centre = a.pos + Fwd(h) * dist;
    const V3 target = centre + Fwd(h + ws.angle) * radius;
    return Norm(target - a.pos) * (a.maxSpeed * 0.5) - a.vel;
}
// Obstacle avoidance: a detection box ahead (length grows with speed) sampled against the solids; a lateral push away from the blocked side and a
// brake, both stronger the nearer the obstacle. `hitDist` reports the nearest obstacle in the box (1e9 none).
inline V3 ObstacleAvoidance(const Agent& a, const World& w, double* hitDist = nullptr)
{
    const double speed = Len2D(a.vel);
    const double len = 8.0 + speed * 1.5;
    const double h = speed > 1.0 ? HeadingOf(a.vel) : a.heading;
    const V3 f = Fwd(h), r = Right(h);
    double nearest = 1e9;
    double side = 0;  // + blocked on the right
    for (double d = a.radius; d <= len; d += std::max(1.5, a.radius)) {
        for (int k = -1; k <= 1; ++k) {
            const V3 p = a.pos + f * d + r * (k * a.radius);
            if (!w.Solid(p)) continue;
            if (d < nearest) nearest = d;
            side += k == 0 ? 0.0 : double(k) * (len - d);
        }
        if (nearest < 1e8) break;
    }
    if (hitDist) *hitDist = nearest;
    if (nearest > 1e8) return {};
    const double k = (len - nearest) / len;
    const double lat = side > 0 ? -1.0 : side < 0 ? 1.0 : (Dot(a.vel, r) >= 0 ? 1.0 : -1.0);  // straight ahead: keep the side we slide to
    return r * (lat * a.maxForce * (0.5 + k)) - f * (speed * k);
}
// Wall and terrain avoidance: three feelers (forward, +-35 deg) marched against solids and terrain; each pushes back by its penetration. Hovers
// also feel the ground under them and a ceiling over them.
inline V3 WallAvoidance(const Agent& a, const World& w)
{
    const double speed = Len2D(a.vel);
    const double len = 6.0 + speed * 1.0;
    const double h = speed > 1.0 ? HeadingOf(a.vel) : a.heading;
    V3 force{};
    const double angs[3] = { 0.0, 0.6108652381980153, -0.6108652381980153 };
    for (int i = 0; i < 3; ++i) {
        const double l = i == 0 ? len : len * 0.7;
        const V3 dir = Fwd(h + angs[i]);
        for (double d = 1.0; d <= l; d += 1.5) {
            const V3 p = a.pos + dir * d;
            const double g = w.Ground(p.x, p.y);
            const bool wall = w.Solid(p) || (std::isfinite(g) && g > a.pos.z - 0.5);
            if (!wall) continue;
            const double pen = (l - d) / l;
            force = force - dir * (pen * a.maxForce);
            if (std::isfinite(g) && g > a.pos.z - 0.5) force.z += pen * a.maxForce * 0.5;  // rising terrain: go up too
            break;
        }
    }
    const double g = w.Ground(a.pos.x, a.pos.y);
    if (std::isfinite(g) && a.pos.z - g < a.clearance) force.z += (a.clearance - (a.pos.z - g)) * 4.0;       // ground feeler
    if (w.Solid(a.pos + V3{ 0, 0, 3.0 })) force.z -= a.maxForce * 0.5;                                          // ceiling feeler
    return force;
}
// Containment: a soft boundary: nothing inside 80 % of the radius, then a pull back growing to max force at the edge and beyond.
inline V3 Containment(const Agent& a, V3 centre, double radius)
{
    const V3 off = Flat(a.pos - centre);
    const double d = Len(off);
    if (d < radius * 0.8 || d < 1e-6) return {};
    const double k = std::min(1.5, (d - radius * 0.8) / (radius * 0.2));
    return Norm(off) * (-a.maxForce * k);
}
// Unaligned collision avoidance: the closest approach to every nearby body within 3 s; the earliest predicted collision is steered away from
// (sideways from where it will be, braking a little when head-on). `when` reports the time of that collision (-1 none).
inline V3 CollisionAvoidance(const Agent& a, const Body* others, int count, double* when = nullptr)
{
    double first = 1e9;
    const Body* threat = nullptr;
    V3 myAt{}, itAt{};
    for (int i = 0; i < count; ++i) {
        const Body& o = others[i];
        const V3 rp = o.pos - a.pos, rv = o.vel - a.vel;
        const double rv2 = Dot(rv, rv);
        const double t = rv2 > 1e-6 ? -Dot(rp, rv) / rv2 : 0.0;
        if (t < 0.0 || t > 3.0) continue;
        const V3 mine = a.pos + a.vel * t, its = o.pos + o.vel * t;
        if (Len(mine - its) > a.radius + o.radius + 2.0) continue;
        if (t < first) { first = t; threat = &o; myAt = mine; itAt = its; }
    }
    if (when) *when = threat ? first : -1.0;
    if (!threat) return {};
    V3 away = Flat(myAt - itAt);
    if (Len(away) < 0.5) away = Right(a.heading) * (Dot(Flat(threat->pos - a.pos), Right(a.heading)) > 0 ? -1.0 : 1.0);  // dead on: sidestep
    const double urgency = 1.0 - first / 3.0;
    const double headOn = Dot(Norm(a.vel), Norm(threat->vel)) < -0.7 ? 0.3 : 0.0;
    return Norm(away) * (a.maxForce * (0.6 + urgency)) - Norm(a.vel) * (a.maxForce * headOn * urgency);
}
// Flocking (vehicles of one squad).
inline V3 Separation(const Agent& a, const Body* mates, int count, double radius = 15.0)
{
    V3 f{};
    for (int i = 0; i < count; ++i) {
        const V3 off = Flat(a.pos - mates[i].pos);
        const double d = Len(off);
        if (d < 0.1 || d > radius) continue;
        f = f + Norm(off) * (a.maxForce * (radius - d) / radius);
    }
    return f;
}
inline V3 Alignment(const Agent& a, const Body* mates, int count, double radius = 40.0)
{
    V3 v{};
    int n = 0;
    for (int i = 0; i < count; ++i) if (Len(mates[i].pos - a.pos) < radius) { v = v + mates[i].vel; ++n; }
    return n ? v * (1.0 / n) - a.vel : V3{};
}
inline V3 Cohesion(const Agent& a, const Body* mates, int count, double radius = 60.0)
{
    V3 c{};
    int n = 0;
    for (int i = 0; i < count; ++i) if (Len(mates[i].pos - a.pos) < radius) { c = c + mates[i].pos; ++n; }
    return n ? Seek(a, c * (1.0 / n)) * 0.5 : V3{};
}
// Leader following: a slot behind the leader (offset pursuit), out of its path when in front of it, apart from the other followers.
inline V3 LeaderFollowing(const Agent& a, const Body& leader, V3 slot, const Body* mates, int count)
{
    V3 f = OffsetPursuit(a, leader, slot, 12.0);
    const V3 rel = Flat(a.pos - leader.pos);
    const double ahead = Dot(rel, Fwd(leader.heading)), side = Dot(rel, Right(leader.heading));
    if (ahead > 0 && ahead < 10.0 + Len(leader.vel) * 1.5 && std::fabs(side) < 4.0) {  // in its path: step aside
        f = f + Right(leader.heading) * ((side >= 0 ? 1.0 : -1.0) * a.maxForce);
    }
    return f + Separation(a, mates, count, 10.0);
}
// Path following: predict half a second ahead; off the corridor -> seek the path point 5 m on from the nearest one.
inline V3 PathFollowing(const Agent& a, const V3* path, int count, double corridor, int* segment = nullptr)
{
    if (count < 2) return count == 1 ? Seek(a, path[0]) : V3{};
    const V3 future = a.pos + a.vel * 0.5;
    double best = 1e18;
    V3 nearest{};
    int seg = 0;
    double along = 0;
    for (int i = 0; i + 1 < count; ++i) {
        const V3 ab = path[i + 1] - path[i];
        const double l2 = std::max(1e-9, Dot(ab, ab));
        const double t = std::clamp(Dot(future - path[i], ab) / l2, 0.0, 1.0);
        const V3 p = path[i] + ab * t;
        const double d = Len(future - p);
        if (d < best) { best = d; nearest = p; seg = i; along = t; }
    }
    if (segment) *segment = seg;
    const V3 dir = Norm(path[seg + 1] - path[seg]);
    const V3 target = nearest + dir * 5.0;
    if (best <= corridor && !(seg + 2 == count && along > 0.99)) return Seek(a, target) * 0.3;  // inside: a light pull along the path
    return Seek(a, target);
}

// ---- Combination: prioritized truncated weighted sum ----
struct Combiner
{
    V3 total{};
    double budget = 0;
    std::uint32_t used = 0;   // bit i: entry i was (at least partly) added
    int entries = 0;
    explicit Combiner(double maxForce) : budget(maxForce) {}
    // Adds force * weight while the budget lasts; false once it is used up (the rest of this tick's behaviours are dropped).
    bool Add(V3 force, double weight)
    {
        const int idx = entries++;
        if (!Finite(force)) return budget > 0;
        const V3 f = force * weight;
        const double have = Len(total), left = budget - have;
        if (left <= 1e-6) return false;
        const double l = Len(f);
        if (l < 1e-9) return true;
        total = total + (l <= left ? f : f * (left / l));
        if (idx < 32) used |= 1u << idx;
        return Len(total) < budget - 1e-6;
    }
};
// The desired velocity from the combined force (a reaction time of tau seconds), clamped to max speed; vertical kept apart (+-8 m/s).
inline V3 Integrate(const Agent& a, V3 force, double tau = 0.5)
{
    V3 v = a.vel + force * tau;
    const double flat = Len2D(v);
    if (flat > a.maxSpeed) { v.x *= a.maxSpeed / flat; v.y *= a.maxSpeed / flat; }
    v.z = std::clamp(v.z, -8.0, 8.0);
    return v;
}
// Heading and speed smoothed over `seconds` (no jitter).
struct Smoother
{
    bool have = false;
    double heading = 0, speed = 0, vz = 0;
    V3 Apply(V3 v, double dt, double seconds = 0.2)
    {
        const double sp = Len2D(v), h = sp > 0.5 ? HeadingOf(v) : heading;
        if (!have) { have = true; heading = h; speed = sp; vz = v.z; }
        else {
            const double k = std::clamp(dt / (seconds + dt), 0.0, 1.0);
            heading = Wrap(heading + Wrap(h - heading) * k);
            speed += (sp - speed) * k;
            vz += (v.z - vz) * k;
        }
        return { std::sin(heading) * speed, std::cos(heading) * speed, vz };
    }
};
// Flight commands from a desired velocity (and an optional aim: the nose on a point instead of along the velocity).
struct Commands
{
    double yawRate = 0, throttle = 0, strafe = 0, vertical = 0;
    double pitch = std::numeric_limits<double>::quiet_NaN();  // NaN: leave the attitude to the autopilot
    int gear = 2;
    bool brake = false;
};
inline Commands ToCommands(const Agent& a, V3 desired, const V3* aimPoint = nullptr)
{
    Commands c;
    const double sp = Len2D(desired);
    double wantHeading = sp > 1.0 ? HeadingOf(desired) : a.heading;
    if (aimPoint) {
        const V3 to = *aimPoint - a.pos;
        wantHeading = HeadingOf(to);
        c.pitch = std::clamp(std::atan2(to.z, std::max(5.0, Len2D(to))), -0.6, 0.6);
    }
    c.yawRate = std::clamp(2.5 * Wrap(wantHeading - a.heading), -a.maxTurn, a.maxTurn);
    const V3 f = Fwd(a.heading), r = Right(a.heading);
    const double wantF = Dot(desired, f), wantR = Dot(desired, r), haveF = Dot(a.vel, f), haveR = Dot(a.vel, r);
    c.throttle = std::clamp((wantF - haveF) / 6.0 + wantF / std::max(1.0, a.maxSpeed) * 0.3, -1.0, 1.0);
    c.strafe = std::clamp((wantR - haveR) / 5.0, -1.0, 1.0);
    c.brake = wantF < haveF - 8.0 && haveF > 3.0;
    c.vertical = std::clamp(desired.z, -8.0, 8.0);
    c.gear = sp > 45.0 ? 4 : sp > 17.0 ? 3 : 2;
    return c;
}
// The terrain safety check: false when flying `desired` for `seconds` meets a solid or the ground (the autopilot's planner then wins).
inline bool SafeAhead(const Agent& a, V3 desired, const World& w, double seconds = 2.5, double* hitAt = nullptr)
{
    for (double t = 0.25; t <= seconds + 1e-9; t += 0.25) {
        const V3 p = a.pos + desired * t;
        const double g = w.Ground(p.x, p.y);
        if (w.Solid(p) || (std::isfinite(g) && g > p.z - 0.5)) { if (hitAt) *hitAt = t; return false; }
    }
    if (hitAt) *hitAt = -1;
    return true;
}
}
