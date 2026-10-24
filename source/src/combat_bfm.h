#pragma once
#include "combat_steer.h"
// Part B of the full vehicle combat AI (2026-10-21, notes/VEHICLE_COMBAT_AI_FULL_SPEC_20261021.md): the basic fighter manoeuvres (BFM) of flight
// sims, all of them, on top of part A's steering. Measure() reads the fight geometry (range, range rate, angle off my nose = ATA, aspect angle
// off its tail = AA, its nose on me, heights, energy, turn radii, weapon zones, the lead point); Desire() scores every manoeuvre for the situation;
// Pick() takes the best with a bonus for the current one (hysteresis); Plan() turns a manoeuvre into steering requests (part A behaviours with
// weights), a vertical rate, the aim and whether boost may be spent (energy management: only extend, break, attack run and ram). FireSolution() is
// the weapon employment zone (range, cone, a lead solution, no ally in the line).
// Pure logic. Tests: tests/combat_bfm_test.cpp.
namespace bfm {
using steer::V3;
using steer::Body;

struct Fighter
{
    Body body;
    double maxSpeed = 40.0, turnRate = 1.6;  // m/s, rad/s
    double boost = 1.0;                       // boost left 0..1
    double health = 1.0;
};
struct Weapon { double range = 100.0, cone = 0.14, shotSpeed = 250.0, maxLeadTime = 1.5; };

struct Geo
{
    double range = 0, rangeRate = 0;  // m, m/s (+ closing)
    double ata = 0;                   // rad: my nose off the line to it (0: my nose on it)
    double aa = 0;                    // rad: aspect, off its tail (0: I sit right behind it; pi: head-on)
    double itsAta = 0;                // rad: its nose off the line to me
    double dz = 0;                    // m: my height over it
    double mySpeed = 0, itsSpeed = 0;
    double myEnergy = 0, itsEnergy = 0;  // specific energy: v^2/2 + g h + boost
    double myRadius = 0, itsRadius = 0;  // turn radius = speed / turn rate
    bool inMyZone = false, inItsZone = false;
    V3 lead;                          // where to aim (its position after the shot's flight)
    double leadTime = 0;
};
inline double Energy(const Fighter& f, double refZ) { const double v = steer::Len(f.body.vel); return 0.5 * v * v + 9.81 * (f.body.pos.z - refZ) + 600.0 * f.boost; }
inline Geo Measure(const Fighter& me, const Fighter& it, const Weapon& w = {})
{
    Geo g;
    const V3 to = it.body.pos - me.body.pos;
    g.range = steer::Len(to);
    const V3 u = steer::Norm(to);
    g.rangeRate = steer::Dot(me.body.vel - it.body.vel, u);
    g.ata = std::fabs(steer::Wrap(steer::HeadingOf(to) - me.body.heading));
    g.itsAta = std::fabs(steer::Wrap(steer::HeadingOf(to * -1.0) - it.body.heading));
    g.aa = 3.141592653589793 - g.itsAta;  // off its tail, seen from it
    g.dz = me.body.pos.z - it.body.pos.z;
    g.mySpeed = steer::Len2D(me.body.vel); g.itsSpeed = steer::Len2D(it.body.vel);
    const double ref = std::min(me.body.pos.z, it.body.pos.z);
    g.myEnergy = Energy(me, ref); g.itsEnergy = Energy(it, ref);
    g.myRadius = g.mySpeed / std::max(0.1, me.turnRate); g.itsRadius = g.itsSpeed / std::max(0.1, it.turnRate);
    double t = g.range / w.shotSpeed;
    for (int i = 0; i < 2; ++i) { g.lead = it.body.pos + it.body.vel * t; t = steer::Len(g.lead - me.body.pos) / w.shotSpeed; }
    g.leadTime = t;
    const double aimErr = std::fabs(steer::Wrap(steer::HeadingOf(g.lead - me.body.pos) - me.body.heading));
    g.inMyZone = g.range < w.range && aimErr < w.cone && t < w.maxLeadTime;
    g.inItsZone = g.range < w.range && g.itsAta < w.cone * 1.5;
    return g;
}

enum class M : std::uint8_t {
    LeadPursuit, PurePursuit, LagPursuit, HighYoYo, LowYoYo, LagRoll, AttackRun, Break, Jink, DefensiveSpiral, FlatScissors, RollingScissors,
    Extend, Reversal, MergeOneCircle, MergeTwoCircle, TurningFight, Separate, Count
};
inline const char* Name(M m)
{
    static const char* const n[] = { "lead pursuit", "pure pursuit", "lag pursuit", "high yo-yo", "low yo-yo", "lag roll", "attack run", "break turn", "jink",
        "defensive spiral", "flat scissors", "rolling scissors", "extend", "reversal", "merge (one-circle)", "merge (two-circle)", "turning fight", "separate" };
    return m < M::Count ? n[int(m)] : "?";
}
enum class Situation : std::uint8_t { Offensive, Defensive, Neutral };
inline const char* Name(Situation s) { return s == Situation::Offensive ? "offensive" : s == Situation::Defensive ? "defensive" : "neutral"; }
// Offensive: I am behind it (inside 60 deg of its tail) with my nose roughly on it. Defensive: its nose is on me and mine is not on it.
inline Situation Classify(const Geo& g)
{
    if (g.aa < 1.05 && g.ata < 1.57) return Situation::Offensive;
    if (g.itsAta < 0.79 && g.ata > 1.57) return Situation::Defensive;
    return Situation::Neutral;
}
inline bool HeadOn(const Geo& g) { return g.aa > 2.4 && g.ata < 0.8; }
inline bool BoostAllowed(M m) { return m == M::Extend || m == M::Break || m == M::AttackRun; }  // (and ram, part D)

struct Ctx
{
    bool underFire = false;       // hit in the last 2 s
    bool wasDefensive = false;    // defensive within the last 4 s (a reversal needs it)
    int enemiesOnMe = 1;
    double aggression = 0.5;      // personality (part C): more aggressive -> more offensive manoeuvres
};
// How much each manoeuvre fits now (0..1).
inline double Desire(M m, const Geo& g, const Ctx& c)
{
    const Situation s = Classify(g);
    const bool off = s == Situation::Offensive, def = s == Situation::Defensive;
    const double agg = (c.aggression - 0.5) * 0.2;  // -0.1 .. +0.1 on offensive choices
    switch (m) {
    case M::LeadPursuit: return (!def && g.range > 40.0 && !g.inItsZone) ? 0.55 + (g.ata < 0.5 ? 0.1 : 0.0) + agg : 0.1;
    case M::PurePursuit: return (g.inMyZone && g.range < 90.0) ? 0.72 + agg : (!def && g.ata < 0.3 && g.range < 100.0 ? 0.5 : 0.05);
    case M::LagPursuit: return (off && g.range < 90.0 && (g.rangeRate > 8.0 || (g.aa < 0.8 && g.range < 70.0))) ? 0.66 : 0.05;
    case M::HighYoYo: return (off && g.rangeRate > 10.0 && g.range < 60.0 && g.ata > 0.4) ? 0.76 : 0.0;
    case M::LowYoYo: return (off && g.rangeRate < -2.0 && g.range > 50.0 && g.ata < 1.2 && g.dz > -5.0) ? 0.7 + agg : 0.0;
    case M::LagRoll: return (off && g.range < 45.0 && g.rangeRate > 5.0 && g.ata > 0.6) ? 0.62 : 0.0;
    case M::AttackRun: return (!def && g.range > 80.0 && (g.dz > 8.0 || g.myEnergy > g.itsEnergy)) ? 0.6 + (g.dz > 15.0 ? 0.15 : 0.0) + agg : 0.0;
    // Under fire the break wins; its nose on me without hits: a break unless height (spiral) or energy (extend) offers better.
    case M::Break: return def ? (c.underFire ? 0.9 : (g.itsAta < 0.3 && g.range < 80.0) ? 0.8 : 0.0) : 0.0;
    case M::Jink: return c.underFire ? (def ? 0.8 : 0.68) : 0.0;
    case M::DefensiveSpiral: return (def && g.dz > 10.0 && g.range < 70.0) ? 0.85 : 0.0;
    case M::FlatScissors: return (!off && g.range < 35.0 && g.aa > 1.2 && g.aa < 2.0 && g.mySpeed < 20.0) ? 0.72 : 0.0;
    case M::RollingScissors: return (!off && g.range < 35.0 && g.aa > 1.2 && g.aa < 2.0 && g.mySpeed >= 20.0) ? 0.72 : 0.0;
    case M::Extend: return (g.myEnergy > g.itsEnergy * 1.15 && (def || c.enemiesOnMe >= 2)) ? 0.82 : 0.0;
    case M::Reversal: return (c.wasDefensive && g.range < 40.0 && g.ata < 1.2 && g.aa < 1.2 && g.rangeRate < 0.0) ? 0.88 : 0.0;
    case M::MergeOneCircle:
    case M::MergeTwoCircle: {
        if (!(HeadOn(g) && g.range < 150.0 && g.rangeRate > 5.0)) return 0.0;
        // One-circle (turn away after the pass, nose to nose) favours the smaller radius; two-circle (turn toward) the faster turn rate.
        const bool oneBetter = g.myRadius < g.itsRadius * 0.9;
        return (m == M::MergeOneCircle) == oneBetter ? 0.8 : 0.5;
    }
    case M::TurningFight: return (s == Situation::Neutral && g.range < 80.0) ? (g.myEnergy >= g.itsEnergy * 0.8 ? 0.58 : 0.2) : 0.1;
    case M::Separate: return g.range < 20.0 ? 0.6 : 0.0;
    default: return 0.0;
    }
}
// The best manoeuvre; the current one gets `hysteresis` extra so choices do not flicker. `enabled`: bit i allows manoeuvre i.
inline M Pick(const Geo& g, const Ctx& c, M current, bool haveCurrent, double hysteresis = 0.15, std::uint32_t enabled = 0xFFFFFFFFu, double* score = nullptr)
{
    M best = M::LeadPursuit;
    double top = -1;
    for (int i = 0; i < int(M::Count); ++i) {
        if (!(enabled & (1u << i))) continue;
        double d = Desire(M(i), g, c);
        if (haveCurrent && M(i) == current) d += hysteresis;
        if (d > top) { top = d; best = M(i); }
    }
    if (score) *score = top;
    return best;
}

// What a manoeuvre asks of the steering this tick.
struct Request
{
    enum class Kind : std::uint8_t { None, Seek, Pursuit, Offset, Flee, Evade, Direction, Arrive } kind = Kind::None;
    V3 point;    // Seek / Flee / Arrive: a point; Direction: a unit direction; Offset: the local offset (x right, y forward, z up)
    double weight = 1.0;
};
struct Recipe
{
    Request req[3];
    int n = 0;
    double speed = 0;          // m/s wanted (the agent's max speed is clamped to it)
    double vertical = std::numeric_limits<double>::quiet_NaN();  // m/s
    bool aim = false;          // nose on the lead point
    bool boost = false;
    bool done = false;         // a manoeuvre with an end (attack run, high yo-yo, extend...) finished
    void Add(Request::Kind k, V3 p, double w = 1.0) { if (n < 3) req[n++] = Request{ k, p, w }; }
};
struct MState
{
    M m = M::LeadPursuit;
    bool started = false;
    std::uint64_t since = 0, nextFlip = 0;
    int phase = 0, side = 1;
    double phi = 0, lat = 0, vert = 0;
    V3 dir{ 0, 1, 0 };
    double minRange = 1e9;
    steer::Rng rng{ 1 };
};
// A change of manoeuvre: phases, sides and directions set up.
inline bool Enter(MState& s, M m, const Fighter& me, const Fighter& it, std::uint64_t now)
{
    const Geo g = Measure(me, it);
    if (s.started && s.m == m) { s.minRange = std::min(s.minRange, g.range); return false; }
    s.started = true; s.m = m; s.since = now; s.phase = 0; s.minRange = g.range; s.phi = 0;
    const V3 to = it.body.pos - me.body.pos;
    const double lateral = steer::Dot(me.body.vel, steer::Right(steer::HeadingOf(to)));
    s.side = lateral >= 0 ? 1 : -1;
    s.nextFlip = now + 1200 + std::uint64_t(s.rng.Next() * 600.0);
    if (m == M::Extend || m == M::Separate) {
        const double a = steer::HeadingOf(to * -1.0) + (lateral >= 0 ? 0.3 : -0.3);
        s.dir = steer::Fwd(a);
    }
    return true;
}
// The steering requests for the current manoeuvre (`dt` seconds since the last call).
inline Recipe Plan(MState& s, const Fighter& me, const Fighter& it, const Geo& g, std::uint64_t now, double dt)
{
    Recipe r;
    const double t = (now - s.since) / 1000.0;
    const V3 to = it.body.pos - me.body.pos;
    const V3 u = steer::Norm(steer::Flat(to));
    const V3 right = steer::Right(steer::HeadingOf(to));
    const double maxS = me.maxSpeed;
    switch (s.m) {
    case M::LeadPursuit:
        r.Add(Request::Kind::Seek, g.lead + it.body.vel * 0.5);
        r.speed = maxS; r.aim = g.ata < 0.5;
        break;
    case M::PurePursuit:
        r.Add(Request::Kind::Seek, it.body.pos);
        r.speed = std::clamp(g.itsSpeed + (g.range - 30.0) * 0.5, 12.0, maxS); r.aim = true;
        break;
    case M::LagPursuit:
        r.Add(Request::Kind::Offset, V3{ 0, -50, 0 });
        r.speed = std::clamp(g.itsSpeed + (g.range - 50.0) * 0.4, 12.0, maxS); r.aim = g.ata < 0.6;
        break;
    case M::HighYoYo:
        // Up out of the plane of its turn to bleed closure (phase 0), then down behind it (phase 1).
        if (s.phase == 0 && t > 1.5) s.phase = 1;
        if (s.phase == 0) { r.Add(Request::Kind::Direction, steer::Norm(steer::Flat(me.body.vel)) + V3{ 0, 0, 0 }); r.vertical = 6.0; r.speed = std::max(12.0, g.mySpeed * 0.7); }
        else { r.Add(Request::Kind::Offset, V3{ 0, -40, 0 }); r.vertical = -4.0; r.speed = std::clamp(g.itsSpeed, 12.0, maxS); r.aim = g.ata < 0.6; r.done = t > 4.0; }
        break;
    case M::LowYoYo: {
        // Down and inside its turn to gain closing speed.
        const double inside = steer::Dot(it.body.vel, right) >= 0 ? 1.0 : -1.0;
        r.Add(Request::Kind::Seek, it.body.pos + it.body.vel * 1.5 + right * (inside * 10.0));
        r.vertical = -4.0; r.speed = maxS; r.done = g.rangeRate > 5.0 && t > 1.0;
        break;
    }
    case M::LagRoll:
        // Rolling around its flight path, behind it: the offset turns on a 12 m circle across its track.
        s.phi += 1.5 * dt;
        r.Add(Request::Kind::Offset, V3{ 12.0 * std::cos(s.phi), -35.0, 12.0 * std::sin(s.phi) });
        r.vertical = 18.0 * std::cos(s.phi) * 1.5 / 3.0;
        r.speed = std::clamp(g.itsSpeed, 12.0, maxS); r.aim = g.ata < 0.6;
        break;
    case M::AttackRun:
        // Boom and zoom: climb to the perch (0), dive with lead and fire (1), climb out past it (2).
        if (s.phase == 0 && (g.dz > 15.0 || g.range < 90.0)) s.phase = 1;
        if (s.phase == 1 && (g.range < 25.0 || (s.minRange < 60.0 && g.rangeRate < 0.0))) { s.phase = 2; s.since = now; }
        if (s.phase == 0) { r.Add(Request::Kind::Seek, it.body.pos + V3{ 0, 0, 25.0 }); r.vertical = 6.0; r.speed = maxS * 0.8; }
        else if (s.phase == 1) { r.Add(Request::Kind::Pursuit, V3{}); r.speed = maxS; r.aim = true; r.vertical = g.dz > 3.0 ? -3.0 : 0.0; }
        else { r.Add(Request::Kind::Direction, steer::Norm(steer::Flat(me.body.vel))); r.vertical = 6.0; r.speed = maxS; r.done = (now - s.since) > 3000; }
        r.boost = true;
        break;
    case M::Break: {
        // A hard turn toward the attacker's side, boosting, with height changes.
        const double h = me.body.heading;
        const double sideOf = steer::Dot(u, steer::Right(h)) >= 0 ? 1.0 : -1.0;
        r.Add(Request::Kind::Direction, steer::Norm(steer::Right(h) * sideOf + u * 0.5));
        r.vertical = (static_cast<std::uint64_t>(t / 0.7) % 2) ? -4.0 : 4.0;
        r.speed = maxS; r.boost = true; r.done = t > 2.0;
        break;
    }
    case M::Jink:
        // Random direction and height changes under fire, re-rolled every 0.4-0.9 s: never a repeating pattern.
        if (now >= s.nextFlip) { s.lat = s.rng.Signed(); s.vert = s.rng.Signed() * 5.0; s.nextFlip = now + 400 + std::uint64_t(s.rng.Next() * 500.0); }
        r.Add(Request::Kind::Direction, steer::Norm(steer::Fwd(me.body.heading) * 0.6 + steer::Right(me.body.heading) * s.lat));
        r.vertical = s.vert; r.speed = maxS * 0.9; r.done = t > 2.5;
        break;
    case M::DefensiveSpiral:
        // A descending turn when it sits on my tail and I have height.
        r.Add(Request::Kind::Direction, steer::Fwd(me.body.heading + s.side * 0.9));
        r.vertical = -4.0; r.speed = maxS;
        break;
    case M::FlatScissors:
    case M::RollingScissors:
        // Turn into it again and again, slowly (flat) or with a rolling up-and-down, so it overshoots.
        if (now >= s.nextFlip) { s.side = -s.side; s.nextFlip = now + 1200 + std::uint64_t(s.rng.Next() * 600.0); }
        r.Add(Request::Kind::Seek, it.body.pos + right * (s.side * 1.2 * std::max(10.0, g.range)));
        r.speed = s.m == M::FlatScissors ? 12.0 : 22.0;
        r.vertical = s.m == M::RollingScissors ? 4.0 * std::sin(t * 6.283185307179586 / 2.4) : 0.0;
        r.aim = g.ata < 0.6;
        break;
    case M::Extend:
        r.Add(Request::Kind::Direction, s.dir);
        r.speed = maxS; r.boost = true; r.done = g.range > 200.0;
        break;
    case M::Reversal:
        r.Add(Request::Kind::Seek, it.body.pos, 1.5);
        r.speed = std::clamp(g.itsSpeed, 12.0, maxS); r.aim = true; r.done = t > 3.0;
        break;
    case M::MergeOneCircle:
    case M::MergeTwoCircle: {
        // Pass it 10 m to one side; inside the lead-turn range start the turn: two-circle toward its side, one-circle away; keep turning after.
        const double leadTurn = g.myRadius * 2.0 + 20.0;
        const double turn = s.m == M::MergeTwoCircle ? s.side : -s.side;
        if (s.phase == 0 && (g.range < leadTurn || g.rangeRate < 0.0)) { s.phase = 1; s.since = now; }
        if (s.phase == 0) { r.Add(Request::Kind::Seek, it.body.pos + right * (s.side * 10.0)); }
        else { r.Add(Request::Kind::Direction, steer::Fwd(me.body.heading + turn * 0.8)); r.done = (now - s.since) > 3000; }
        r.speed = maxS * 0.85;
        break;
    }
    case M::TurningFight: {
        // Circle it at 40-80 m, the nose on it (the turning fight's circle-strafe).
        if (now >= s.nextFlip) { s.side = -s.side; s.nextFlip = now + 3000 + std::uint64_t(s.rng.Next() * 3000.0); s.phi = 40.0 + s.rng.Next() * 40.0; }
        const double radius = s.phi > 1.0 ? s.phi : 60.0;
        const double k = std::clamp((g.range - radius) / radius * 1.5, -1.0, 1.0);
        r.Add(Request::Kind::Direction, steer::Norm(right * double(s.side) + u * k));
        r.speed = 20.0; r.aim = true;
        break;
    }
    case M::Separate:
        r.Add(Request::Kind::Direction, s.dir);
        r.speed = maxS; r.done = g.range > 150.0;
        break;
    default:
        break;
    }
    // Guns defence: never sit in its forward cone while it is not in mine; slide out of it.
    if (g.inItsZone && !g.inMyZone && s.m != M::Break && s.m != M::Jink) r.Add(Request::Kind::Direction, right * double(-s.side), 0.5);
    return r;
}
// The weapon employment zone: inside range and cone with a lead solution, and no ally within 3 m of the shot line.
inline bool AllyInLine(const V3& from, const V3& to, const V3* allies, int count, double width = 3.0)
{
    const V3 ab = to - from;
    const double l2 = std::max(1e-9, steer::Dot(ab, ab));
    for (int i = 0; i < count; ++i) {
        const double t = steer::Dot(allies[i] - from, ab) / l2;
        if (t <= 0.0 || t >= 1.0) continue;
        if (steer::Len(allies[i] - (from + ab * t)) < width) return true;
    }
    return false;
}
inline bool FireSolution(const Fighter& me, const Geo& g, const V3* allies, int allyCount)
{
    return g.inMyZone && !AllyInLine(me.body.pos, g.lead, allies, allyCount);
}
// Hull-sized allies along the actual shot segment, including the muzzle's start.
inline bool AllyBodiesInLine(const V3& from, const V3& to, const Body* allies, int count)
{
    const V3 ab = to - from;
    const double l2 = std::max(1e-9, steer::Dot(ab, ab));
    for (int i = 0; i < count; ++i) {
        const double t = std::clamp(steer::Dot(allies[i].pos - from, ab) / l2, 0.0, 1.0);
        if (steer::Len(allies[i].pos - (from + ab * t)) < std::max(0.5, allies[i].radius) + 0.5) return true;
    }
    return false;
}
}
