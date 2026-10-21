#pragma once
#include "combat_bfm.h"
// Part D of the full vehicle combat AI (2026-10-21, notes/VEHICLE_COMBAT_AI_FULL_SPEC_20261021.md): vehicle tactics after Mad Max (2015) car combat.
// Ram types (T-bone, head-on, rear, PIT) and ram damage to both vehicles from relative speed, angle and mass (the rammed one is pushed and loses
// control for a moment); pinning against terrain / walls / cliff edges; boxing in with allies (slots ahead, flanks, behind); cut-off and block
// (intercept on its path, a chokepoint bonus); sideswipe (push it off course, off a ledge, into an obstacle); convoy attack (escorts first, then the
// cargo) and defence (formation slots, a pair breaks off to intercept); terrain use (high ground, dips via part A's hide, never pinned ourselves);
// hover pushes (down into the ground or over a drop; climbing over what blocks the way to it).
// Facts() reads the world once per tick (nav map), Desire() scores each tactic, Plan() makes part B's steering recipe. Pure logic.
// Tests: tests/combat_tactics_test.cpp.
namespace tactics {
using steer::V3;
using steer::Body;
using steer::World;

enum class T : std::uint8_t { TBone, HeadOnRam, RearRam, PIT, Pin, BoxIn, CutOff, Block, Sideswipe, ConvoyAttack, ConvoyDefend, HighGround, PushDown, PassOver, Count };
inline const char* Name(T t)
{
    static const char* const n[] = { "T-bone ram", "head-on ram", "rear ram", "PIT", "pin", "box in", "cut-off", "block", "sideswipe", "convoy attack",
        "convoy defence", "high ground", "hover push-down", "pass over" };
    return t < T::Count ? n[int(t)] : "?";
}
inline bool IsRam(T t) { return t == T::TBone || t == T::HeadOnRam || t == T::RearRam || t == T::PIT; }
inline bool MakesContact(T t) { return IsRam(t) || t == T::Pin || t == T::Sideswipe || t == T::PushDown; }

struct Unit
{
    Body body;
    double mass = 1500.0, health = 1.0, maxSpeed = 40.0;
    bool hover = true, cargo = false, escort = false;
};
inline double Strength(const Unit& u) { return u.mass * std::max(0.05, u.health); }

// ---- Rams ----
enum class RamKind : std::uint8_t { None, TBone, HeadOn, Rear, PIT };
// From the angle between my velocity and its heading and where on it I would hit.
inline RamKind ClassifyRam(const Unit& me, const Unit& it)
{
    const double sp = steer::Len2D(me.body.vel);
    if (sp < 2.0) return RamKind::None;
    const double cross = std::fabs(steer::Wrap(steer::HeadingOf(me.body.vel) - it.body.heading));
    const V3 rel = steer::Flat(me.body.pos - it.body.pos);
    const double along = steer::Dot(rel, steer::Fwd(it.body.heading));  // + : I am ahead of its centre
    if (cross < 0.5) return RamKind::Rear;
    if (cross > 2.6) return RamKind::HeadOn;
    if (cross > 1.1 && cross < 2.0) return along < -it.body.radius * 0.4 ? RamKind::PIT : RamKind::TBone;
    return along < 0 ? RamKind::PIT : RamKind::TBone;
}
struct RamResult { double damageMe = 0, damageIt = 0, pushIt = 0, pushMe = 0; unsigned stunItMs = 0; };
// Damage (vehicle health points) to both from the closing speed along the contact line, the kind of hit and the masses; the push (m/s) and the
// rammed vehicle's loss of control. Below 4 m/s nothing.
inline RamResult RamDamage(double closingSpeed, RamKind kind, double myMass, double itsMass)
{
    RamResult r;
    if (closingSpeed < 4.0) return r;
    const double e = closingSpeed * closingSpeed, total = std::max(1.0, myMass + itsMass);
    double hitIt = 1.0, hitMe = 1.0, stun = 1.0;
    switch (kind) {
    case RamKind::TBone: hitIt = 1.3; hitMe = 0.6; stun = 1.2; break;
    case RamKind::HeadOn: hitIt = 1.0; hitMe = 1.0; stun = 1.0; break;
    case RamKind::Rear: hitIt = 0.8; hitMe = 0.5; stun = 0.6; break;
    case RamKind::PIT: hitIt = 0.5; hitMe = 0.3; stun = 2.0; break;
    default: break;
    }
    r.damageIt = 0.35 * e * (myMass / total) * hitIt;
    r.damageMe = 0.35 * e * (itsMass / total) * hitMe;
    r.pushIt = closingSpeed * (myMass / total) * 0.6;
    r.pushMe = closingSpeed * (itsMass / total) * 0.3;
    r.stunItMs = unsigned(std::clamp(closingSpeed * 60.0 * stun, 0.0, 2500.0));
    return r;
}

// The cap on one ram (2026-10-21, the coordinator's review: uncapped, a 40 m/s T-bone dealt ~364 of 300 health, a one-hit wreck): each side's damage
// from a single ram is at most capShare of that vehicle's own max health. Returns true when either side was capped.
inline bool CapRam(RamResult& r, double myMax, double itsMax, double capShare, double* uncappedIt = nullptr, double* uncappedMe = nullptr)
{
    if (uncappedIt) *uncappedIt = r.damageIt;
    if (uncappedMe) *uncappedMe = r.damageMe;
    const double capIt = std::max(0.0, capShare) * std::max(0.0, itsMax), capMe = std::max(0.0, capShare) * std::max(0.0, myMax);
    const bool capped = r.damageIt > capIt || r.damageMe > capMe;
    r.damageIt = std::min(r.damageIt, capIt);
    r.damageMe = std::min(r.damageMe, capMe);
    return capped;
}

// ---- What the world offers around the target (read once per tick) ----
struct Facts
{
    bool pinSurface = false; V3 pinDir; double pinDist = 0;        // a wall / steep rise / cliff it can be pushed into
    bool edge = false; V3 edgeDir;                                  // a drop next to it (push it over)
    bool hazard = false; int hazardSide = 0;                        // sideswipe: an obstacle or drop on its side (+1 its right)
    bool choke = false;                                             // its path ahead runs through a gap
    bool highGround = false; V3 highSpot;                           // a higher spot near me that still sees it
    bool passOver = false; double passHeight = 0;                   // the way to it is blocked low but clear higher
    bool pinnedMe = false; V3 escapeDir;                            // I am against a wall with an enemy closing from the other side
    bool intercept = false; V3 interceptPoint; double interceptT = 0;
};
inline bool Steep(const World& w, const V3& from, const V3& p, double rise = 2.0)
{
    const double g = w.Ground(p.x, p.y), g0 = w.Ground(from.x, from.y);
    return w.Solid(p) || (std::isfinite(g) && std::isfinite(g0) && g - g0 > rise);
}
inline bool Drop(const World& w, const V3& from, const V3& p, double depth = 6.0)
{
    const double g = w.Ground(p.x, p.y), g0 = w.Ground(from.x, from.y);
    return std::isfinite(g) && std::isfinite(g0) && g0 - g > depth;
}
// The point on its path I can reach at least `margin` s before it does (0..10 s ahead).
inline bool InterceptPoint(const Unit& me, const Unit& it, double margin, V3& point, double& when)
{
    const double speed = std::max(5.0, me.maxSpeed);
    for (double t = 0.5; t <= 10.0; t += 0.25) {
        const V3 p = it.body.pos + it.body.vel * t;
        if (steer::Len2D(p - me.body.pos) / speed <= t - margin) { point = p; when = t; return true; }
    }
    return false;
}
inline Facts Read(const World& w, const Unit& me, const Unit& it, const Unit* enemies, int enemyCount)
{
    Facts f;
    // Pin surface / edge: 12 directions out from it, 4-25 m.
    double best = 1e9;
    for (int k = 0; k < 12; ++k) {
        const V3 dir = steer::Fwd(k * 0.5235987755982988);
        for (double d = 4.0; d <= 25.0; d += 3.0) {
            const V3 p = it.body.pos + dir * d;
            if (Steep(w, it.body.pos, p)) { if (d < best) { best = d; f.pinSurface = true; f.pinDir = dir; f.pinDist = d; } break; }
            if (!f.edge && d <= 15.0 && Drop(w, it.body.pos, p)) { f.edge = true; f.edgeDir = dir; }
        }
    }
    // Sideswipe hazard on its sides (6-15 m).
    for (int side = 1; side >= -1 && !f.hazard; side -= 2) {
        for (double d = 6.0; d <= 15.0; d += 3.0) {
            const V3 p = it.body.pos + steer::Right(it.body.heading) * (side * d);
            if (Steep(w, it.body.pos, p) || Drop(w, it.body.pos, p)) { f.hazard = true; f.hazardSide = side; break; }
        }
    }
    // Intercept on its path: the earliest point I reach first, or better a chokepoint further along it (solid or steep on both sides within
    // 12 m) that I also reach first.
    f.intercept = InterceptPoint(me, it, 1.0, f.interceptPoint, f.interceptT);
    if (f.intercept) {
        const V3 r = steer::Right(steer::Len2D(it.body.vel) > 1.0 ? steer::HeadingOf(it.body.vel) : it.body.heading);
        const double speed = std::max(5.0, me.maxSpeed);
        for (double t = f.interceptT; t <= 10.0 && !f.choke; t += 0.5) {
            const V3 p = it.body.pos + it.body.vel * t;
            if (steer::Len2D(p - me.body.pos) / speed > t - 1.0) continue;
            bool left = false, right = false;
            for (double d = 4.0; d <= 12.0; d += 4.0) {
                left = left || Steep(w, p, p - r * d);
                right = right || Steep(w, p, p + r * d);
            }
            if (left && right) { f.choke = true; f.interceptPoint = p; f.interceptT = t; }
        }
    }
    // High ground near me that still sees it.
    const double myG = w.Ground(me.body.pos.x, me.body.pos.y);
    double gain = 5.0;
    for (int k = 0; k < 8; ++k) {
        V3 p = me.body.pos + steer::Fwd(k * 0.7853981633974483) * 40.0;
        const double g = w.Ground(p.x, p.y);
        if (!std::isfinite(g) || !std::isfinite(myG) || g - myG <= gain) continue;
        p.z = g + 3.0;
        if (w.Blocked(p, it.body.pos + V3{ 0, 0, 1.0 })) continue;
        gain = g - myG; f.highGround = true; f.highSpot = p;
    }
    // Pass over: blocked at its height, clear 6-15 m up.
    if (w.Blocked(me.body.pos, it.body.pos)) {
        for (double h = 6.0; h <= 15.0; h += 3.0) {
            if (!w.Blocked(me.body.pos + V3{ 0, 0, h }, it.body.pos + V3{ 0, 0, h })) { f.passOver = true; f.passHeight = h; break; }
        }
    }
    // Never pinned ourselves: a wall within 4 m to one side and an enemy closing from the other.
    for (int side = 1; side >= -1; side -= 2) {
        const V3 r = steer::Right(me.body.heading) * double(side);
        bool wall = false;
        for (double d = 1.5; d <= 4.0; d += 1.25) wall = wall || Steep(w, me.body.pos, me.body.pos + r * d);
        if (!wall) continue;
        for (int i = 0; i < enemyCount; ++i) {
            const V3 rel = steer::Flat(enemies[i].body.pos - me.body.pos);
            const double closing = steer::Dot(enemies[i].body.vel - me.body.vel, steer::Norm(rel) * -1.0);
            if (steer::Dot(rel, r) < 0 && steer::Len(rel) < 25.0 && closing > 2.0) {
                f.pinnedMe = true;
                f.escapeDir = steer::Fwd(me.body.heading) * (steer::Dot(me.body.vel, steer::Fwd(me.body.heading)) >= 0 ? 1.0 : -1.0);
            }
        }
    }
    return f;
}

// ---- Choosing ----
struct Ctx
{
    bool allowRam = true;
    int alliesOnTarget = 1;          // my side's vehicles on this target, me included
    bool targetInConvoy = false, targetIsCargo = false, convoyHasEscorts = false;
    bool iDefendConvoy = false;
    double aggression = 0.5;
};
inline double Desire(T t, const Unit& me, const Unit& it, const bfm::Geo& g, const Facts& f, const Ctx& c)
{
    const double ratio = Strength(me) / std::max(1.0, Strength(it));
    const double agg = (c.aggression - 0.5) * 0.2;
    if (IsRam(t) && !c.allowRam) return 0.0;
    switch (t) {
    case T::TBone: return (g.aa > 1.1 && g.aa < 2.0 && g.range > 15.0 && g.range < 80.0 && g.mySpeed > g.itsSpeed && ratio >= 1.0) ? 0.75 + agg : 0.0;
    case T::RearRam: return (g.aa < 0.5 && g.mySpeed > g.itsSpeed + 5.0 && g.range < 50.0 && ratio >= 0.8) ? 0.7 + agg : 0.0;
    case T::PIT: return (g.aa > 0.3 && g.aa < 0.9 && std::fabs(g.mySpeed - g.itsSpeed) < 8.0 && g.range < 30.0 && g.itsSpeed > 8.0) ? 0.78 + agg : 0.0;
    case T::HeadOnRam: return (bfm::HeadOn(g) && ratio >= 1.6 && g.range < 80.0) ? 0.7 + agg : 0.0;
    case T::Pin: return (f.pinSurface && f.pinDist < 15.0 && g.itsSpeed < 10.0 && g.range < 40.0) ? 0.75 + (c.alliesOnTarget >= 2 ? 0.1 : 0.0) : 0.0;
    case T::BoxIn: return (c.alliesOnTarget >= 3 && g.itsSpeed > 5.0 && g.range < 80.0) ? 0.8 : 0.0;
    case T::CutOff: return (f.intercept && g.range > 40.0 && (g.rangeRate < -2.0 || g.itsSpeed > g.mySpeed)) ? 0.72 + (f.choke ? 0.1 : 0.0) : 0.0;
    case T::Block: return (f.intercept && f.choke && f.interceptT > 2.0) ? 0.76 : 0.0;
    case T::Sideswipe: return (g.aa > 1.2 && g.aa < 1.95 && g.range < 25.0 && g.mySpeed > g.itsSpeed + 5.0) ? 0.7 + (f.hazard || f.edge ? 0.15 : 0.0) : 0.0;
    case T::ConvoyAttack: return c.targetInConvoy ? 0.7 : 0.0;
    case T::ConvoyDefend: return c.iDefendConvoy ? 0.72 : 0.0;
    case T::HighGround: return (f.highGround && g.range > 60.0 && g.range < 150.0) ? 0.5 : 0.0;
    case T::PushDown: return (me.hover && g.dz > 1.0 && g.dz < 8.0 && steer::Len2D(it.body.pos - me.body.pos) < 8.0) ? 0.8 : 0.0;
    case T::PassOver: return (me.hover && f.passOver) ? 0.65 : 0.0;
    default: return 0.0;
    }
}
inline T Pick(const Unit& me, const Unit& it, const bfm::Geo& g, const Facts& f, const Ctx& c, double* score = nullptr, std::uint32_t enabled = 0xFFFFFFFFu)
{
    T best = T::CutOff;
    double top = -1;
    for (int i = 0; i < int(T::Count); ++i) {
        if (!(enabled & (1u << i))) continue;
        const double d = Desire(T(i), me, it, g, f, c);
        if (d > top) { top = d; best = T(i); }
    }
    if (score) *score = top;
    return best;
}

// The box-in slot offsets in the target's frame (x right, y forward); the flanks squeeze in after 3 s. Slot 0 ahead (slows it), 1 left, 2 right, 3 behind.
inline V3 BoxSlot(int slot, double seconds)
{
    const double squeeze = seconds > 3.0 ? 4.0 : 7.0;
    switch (slot & 3) {
    case 0: return { 0, 14, 0 };
    case 1: return { -squeeze, 0, 0 };
    case 2: return { squeeze, 0, 0 };
    default: return { 0, -14, 0 };
    }
}
// Convoy attack order: escorts first (the nearest), then the cargo vehicle, else the nearest.
inline int ConvoyTarget(const Unit& me, const Unit* units, int count)
{
    int best = -1, bestClass = 9;
    double bestD = 1e18;
    for (int i = 0; i < count; ++i) {
        const int cls = units[i].escort ? 0 : units[i].cargo ? 1 : 2;
        const double d = steer::Len(units[i].body.pos - me.body.pos);
        if (cls < bestClass || (cls == bestClass && d < bestD)) { best = i; bestClass = cls; bestD = d; }
    }
    return best;
}
// Convoy defence: the formation slot of defender `rank` around the leader (left, right, behind, then wider), and whether it is one of the pair
// that breaks off to intercept (the two defenders nearest the attacker).
inline V3 ConvoySlot(int rank) { const int ring = rank / 3; const double s = 12.0 + ring * 8.0; return rank % 3 == 0 ? V3{ -s, -4, 0 } : rank % 3 == 1 ? V3{ s, -4, 0 } : V3{ 0, -s - 4, 0 }; }
inline bool InterceptPair(const Body* defenders, int count, int me, const V3& attacker)
{
    int closer = 0;
    const double mine = steer::Len(defenders[me].pos - attacker);
    for (int i = 0; i < count; ++i) if (i != me && steer::Len(defenders[i].pos - attacker) < mine) ++closer;
    return closer < 2;
}

struct TState
{
    T t = T::CutOff;
    bool started = false;
    std::uint64_t since = 0;
    int side = 1, slot = 0;
    bool contactMade = false;
};
inline bool Enter(TState& s, T t, const Unit& me, const Unit& it, const Facts& f, std::uint64_t now, int slot = 0)
{
    if (s.started && s.t == t) { s.slot=slot; return false; }
    s.started = true; s.t = t; s.since = now; s.slot = slot; s.contactMade = false;
    const double lateral = steer::Dot(steer::Flat(me.body.pos - it.body.pos), steer::Right(it.body.heading));
    s.side = lateral >= 0 ? 1 : -1;  // the side of it I am on
    if (t == T::Sideswipe && (f.hazard || f.edge)) {
        // Push toward the hazard: come from the other side.
        const int push = f.hazard ? f.hazardSide : (steer::Dot(f.edgeDir, steer::Right(it.body.heading)) >= 0 ? 1 : -1);
        s.side = -push;
    }
    return true;
}
struct Plan
{
    bfm::Recipe r;
    bool contact = false;        // this tactic touches the target (no collision avoidance / separation against it)
    double strafe = std::numeric_limits<double>::quiet_NaN();  // a sideways push (sideswipe, PIT)
};
inline Plan Steer(TState& s, const Unit& me, const Unit& it, const bfm::Geo& g, const Facts& f, std::uint64_t now)
{
    Plan p;
    bfm::Recipe& r = p.r;
    const double secs = (now - s.since) / 1000.0;
    const V3 itF = steer::Fwd(it.body.heading), itR = steer::Right(it.body.heading);
    const double lead = std::clamp(g.range / std::max(10.0, g.mySpeed), 0.0, 2.0);
    const V3 itAt = it.body.pos + it.body.vel * lead;
    p.contact = MakesContact(s.t);
    switch (s.t) {
    case T::TBone:
        // Line up on its side 25 m out, then straight into its centre at its predicted position.
        if (g.range > 30.0 && std::fabs(steer::Wrap(steer::HeadingOf(it.body.pos - me.body.pos) - it.body.heading)) < 1.0) r.Add(bfm::Request::Kind::Seek, itAt + itR * (s.side * 25.0));
        else r.Add(bfm::Request::Kind::Seek, itAt);
        r.speed = me.maxSpeed; r.boost = true;
        break;
    case T::HeadOnRam:
        r.Add(bfm::Request::Kind::Seek, itAt); r.speed = me.maxSpeed; r.boost = true;
        break;
    case T::RearRam:
        r.Add(bfm::Request::Kind::Seek, it.body.pos - itF * it.body.radius); r.speed = std::min(me.maxSpeed, g.itsSpeed + 8.0); r.boost = true;
        break;
    case T::PIT: {
        // Nudge its rear quarter at a shallow angle, a little faster than it, then push sideways to spin it.
        const V3 quarter = it.body.pos - itF * (it.body.radius * 0.8) + itR * (s.side * it.body.radius * 0.6);
        r.Add(bfm::Request::Kind::Seek, quarter + it.body.vel * 0.3);
        r.speed = std::min(me.maxSpeed, g.itsSpeed + 4.0);
        if (g.range < it.body.radius + 4.0) { p.strafe = -s.side * 1.0; s.contactMade = true; }
        break;
    }
    case T::Pin:
        // Round to the far side from the surface, then press it in and hold.
        if (f.pinSurface) {
            const V3 behind = it.body.pos - f.pinDir * 8.0;
            if (steer::Len2D(me.body.pos - behind) > 6.0 && !s.contactMade) r.Add(bfm::Request::Kind::Seek, behind);
            else { s.contactMade = true; r.Add(bfm::Request::Kind::Seek, it.body.pos + f.pinDir * 2.0); }
        }
        else r.Add(bfm::Request::Kind::Seek, it.body.pos);
        r.speed = s.contactMade ? 6.0 : std::min(me.maxSpeed, g.itsSpeed + 10.0);
        break;
    case T::BoxIn:
        r.Add(bfm::Request::Kind::Offset, BoxSlot(s.slot, secs));
        r.speed = (s.slot & 3) == 0 ? g.itsSpeed * 0.7 : g.itsSpeed + 3.0;
        break;
    case T::CutOff:
    case T::Block:
        // Get onto its path ahead of it; a block then holds across the path, facing it.
        if (f.intercept) {
            if (s.t == T::Block && steer::Len2D(me.body.pos - f.interceptPoint) < 8.0) { r.Add(bfm::Request::Kind::Arrive, f.interceptPoint); r.speed = 4.0; r.aim = true; }
            else { r.Add(bfm::Request::Kind::Seek, f.interceptPoint); r.speed = me.maxSpeed; }
        }
        else { r.Add(bfm::Request::Kind::Pursuit, V3{}); r.speed = me.maxSpeed; }
        break;
    case T::Sideswipe: {
        // Alongside it on side s.side, then push into it (toward the hazard when there is one).
        r.Add(bfm::Request::Kind::Offset, V3{ s.side * 3.5, 0, 0 });
        r.speed = std::min(me.maxSpeed, g.itsSpeed + 6.0);
        if (std::fabs(steer::Dot(steer::Flat(me.body.pos - it.body.pos), itR)) < 6.0 && std::fabs(steer::Dot(steer::Flat(me.body.pos - it.body.pos), itF)) < 5.0) {
            p.strafe = -s.side * 1.0; s.contactMade = true;
        }
        break;
    }
    case T::ConvoyAttack:
        r.Add(bfm::Request::Kind::Pursuit, V3{}); r.speed = me.maxSpeed; r.aim = g.ata < 0.6;
        break;
    case T::ConvoyDefend:
        r.Add(bfm::Request::Kind::Offset, ConvoySlot(s.slot)); r.speed = std::max(10.0, g.itsSpeed);
        break;
    case T::HighGround:
        if (f.highGround) { r.Add(bfm::Request::Kind::Arrive, f.highSpot); r.speed = me.maxSpeed * 0.7; }
        r.aim = steer::Len2D(me.body.pos - f.highSpot) < 10.0;
        break;
    case T::PushDown:
        // Over it, then down onto it.
        r.Add(bfm::Request::Kind::Seek, it.body.pos + V3{ 0, 0, 3.0 });
        r.vertical = steer::Len2D(it.body.pos - me.body.pos) < 5.0 ? -6.0 : 0.0;
        r.speed = std::max(8.0, g.itsSpeed);
        break;
    case T::PassOver:
        r.Add(bfm::Request::Kind::Pursuit, V3{});
        r.vertical = me.body.pos.z < it.body.pos.z + f.passHeight ? 6.0 : 0.0;
        r.speed = me.maxSpeed * 0.8;
        break;
    default:
        break;
    }
    return p;
}
}
