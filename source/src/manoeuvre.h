#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
// Vehicle-vs-vehicle combat AI (2026-10-21; the approved proposal notes/VEHICLE_COMBAT_AI_PROPOSAL_20261021.md, the user: "Approved ... build stage 1",
// then "do all"). Three layers:
//  1. Movement (Reynolds steering): Steer() always flies a carrot 50 m or more ahead (cruise: no arrive braking) at the manoeuvre's speed, never
//     under kMinSpeed while fighting (the circle, scissors and overwatch steer by throttle / strafe / a held spot instead), keeps kSeparation from
//     other vehicles and stays in the fight zone; terrain and obstacles stay with the autopilot's planner.
//  2. Manoeuvres (dogfight basics): Choose() picks from the geometry (range, angle off the enemy's tail, who faces whom, closure, hits) with a
//     minimum hold for the freely chosen states. Stage 1: close in, attack run (boom and zoom: in with lead, fire, pass, extend 150-250 m, back),
//     lag pursuit (40-70 m behind its tail), break turn (hit, or the enemy behind us). Stage 2: scissors (overshoot), extend and re-engage
//     (losing, outnumbered), circle at 40-80 m changing radius and direction every 3-6 s. Stage 3: roles for several vehicles on one target
//     (engage, pincer to the tail, overwatch) and aggression from health. Stage 4: ram, sideswipe, hiding behind terrain on the way out (the
//     caller tests the lines of sight: HideCandidates / Hide).
//  3. Decisions: Settings::stage (ini [combat] stage=) limits the states to the stages built so far, for testing stage by stage.
// Additions kept from the first manoeuvre set (2026-10-21, approved by the user in the vehicle session): break off (low health: out to 150 m, back when
// healed) and the fight zones. Pure geometry: x/y horizontal, heading h faces (sin h, cos h); no engine calls (tests/manoeuvre_test.cpp).
namespace manoeuvre {
enum class State : std::uint8_t { CloseIn, AttackRun, LagPursuit, Circle, Break, Scissors, Extend, Overwatch, Ram, Sideswipe, BreakOff };
inline const char* Name(State s) {
    switch (s) {
    case State::CloseIn: return "close in";
    case State::AttackRun: return "attack run";
    case State::LagPursuit: return "lag pursuit";
    case State::Circle: return "circle";
    case State::Break: return "break turn";
    case State::Scissors: return "scissors";
    case State::Extend: return "extend";
    case State::Overwatch: return "overwatch";
    case State::Ram: return "ram";
    case State::Sideswipe: return "sideswipe";
    case State::BreakOff: return "break off";
    }
    return "?";
}

inline constexpr double kPi = 3.141592653589793;
// Movement
inline constexpr double kTopSpeed = 50.0;                 // m/s: AI gear 3 (vehicleflight kAiGearTop[3])
inline constexpr double kMinSpeed = 0.4 * kTopSpeed;      // the minimum combat speed (proposal: about 40 % of top)
inline constexpr double kFastSpeed = 40.0;                // attack runs, extends, breaks
inline constexpr double kSeparation = 15.0;               // m kept from other vehicles
inline constexpr double kCarrotMin = 50.0, kCarrotTime = 2.2;  // the carrot: at least this far, or this many seconds of speed ahead
// Manoeuvres
inline constexpr double kAttackFrom = 80.0;               // m: an attack run starts farther than this, facing the enemy
inline constexpr double kFireFar = 150.0, kFireNear = 30.0;  // m: the attack run aims (and may fire) inside this band
inline constexpr double kPassBy = 8.0;                    // m: the attack run's line passes this far beside the enemy
inline constexpr double kLagMin = 40.0, kLagMax = 70.0;   // m behind the enemy's tail
inline constexpr double kTailCone = 0.785;                // rad: inside 45 deg off its tail = behind it
inline constexpr double kFacing = 0.7;                    // rad: my nose this close to the enemy = facing it
inline constexpr double kEnemyOnMe = 0.6;                 // rad: the enemy's nose this close to me = it points at me
inline constexpr double kBehindMe = 2.3;                  // rad: the enemy this far off my nose = behind me
inline constexpr double kBehindRange = 90.0;
inline constexpr std::uint64_t kHitMs = 2000;             // hit in the last 2 s: break turn
inline constexpr std::uint64_t kBreakMs = 2000;
inline constexpr double kTooClose = 14.0;
inline constexpr double kOvershoot = 30.0;                // m: passed it closer than this: scissors
inline constexpr std::uint64_t kScissorsMs = 6000, kScissorFlipMs = 1200;
inline constexpr std::uint64_t kExtendMaxMs = 10000;
inline constexpr double kCircleMin = 40.0, kCircleMax = 80.0;
inline constexpr std::uint64_t kCircleMinMs = 3000, kCircleMaxMs = 6000;
inline constexpr std::uint64_t kReengageCooldownMs = 15000;
inline constexpr double kLosing = 0.25;                   // my health this far under the enemy's = losing
inline constexpr double kOverwatchRange = 100.0, kOverwatchUp = 25.0;
inline constexpr std::uint64_t kRamCooldownMs = 20000, kSideswipeMs = 3000;
inline constexpr double kBreakOffDone = 0.55, kBreakOffDist = 150.0;
inline constexpr int kMaxOthers = 8;

struct Settings
{
    int stage = 4;                // states of stages 1..stage
    double zone = 150.0;          // m around the anchor
    double breakOff = 0.30;       // health share
    std::uint64_t holdMs = 1500;  // a freely chosen state is kept at least this long
    double speedScale = 1.0;      // [combat] boost=0 -> 0.7
};
struct Body { double x = 0, y = 0, z = 0, vx = 0, vy = 0, heading = 0; };
struct Input
{
    Body me, foe;
    bool foeInVehicle = false;
    double anchorX = 0, anchorY = 0;  // the fight zone's centre (the player)
    double myHealth = 1.0;            // share 0..1
    double foeHealth = std::numeric_limits<double>::quiet_NaN();  // NaN: unknown
    bool hitRecently = false;         // a hit within kHitMs
    int hitsRecent = 0;               // hits in the last 2 s (for the log)
    int sameSide = 1, rank = 0;       // vehicles of my side on this enemy (me included), my place among them
    int enemiesOnMe = 1;              // enemy vehicles attacking me
    bool hover = true;
    bool ramming = false;             // the ram order is still running
    double otherX[kMaxOthers] = {}, otherY[kMaxOthers] = {};  // other vehicles near me (separation)
    int others = 0;
    std::uint64_t now = 0;
};
struct Mind
{
    State state = State::CloseIn;
    bool started = false;
    std::uint64_t since = 0, lastRam = 0, lastReengage = 0, nextFlip = 0, breakEnd = 0;
    int side = 1;                     // pass / circle / scissors side (+1 right of the line to the enemy)
    double ex = 0, ey = 1;            // extend: the direction out
    double extendOut = 200.0;         // extend: how far out
    double radius = 60.0;             // circle radius
    double minRange = 1e9;            // closest this attack run / sideswipe came
    bool reengage = false;            // the extend ends in an attack run
};
struct Plan
{
    double x = 0, y = 0, z = std::numeric_limits<double>::quiet_NaN();  // the carrot
    double speed = 0;                 // m/s (cruise)
    bool hold = false;                // a held spot (overwatch): arrive and stay
    bool aim = false;                 // hold the nose (yaw + pitch) on the aim point
    double throttle = std::numeric_limits<double>::quiet_NaN();  // NaN: the planner's
    double strafe = std::numeric_limits<double>::quiet_NaN();
    double vertical = std::numeric_limits<double>::quiet_NaN();  // m/s
    bool ram = false;                 // order the ram (the caller)
};

inline double Wrap(double a) { while (a > kPi) a -= 2 * kPi; while (a < -kPi) a += 2 * kPi; return a; }
inline void Norm(double& x, double& y) { const double l = std::hypot(x, y); if (l > 1e-6) { x /= l; y /= l; } else { x = 0; y = 1; } }
struct Geo { double range, ux, uy, aspect, offTail, angleOff, closure, mySpeed, foeSpeed; };
inline Geo Measure(const Input& in)
{
    Geo g{};
    const double dx = in.foe.x - in.me.x, dy = in.foe.y - in.me.y;
    g.range = std::hypot(dx, dy);
    g.ux = g.range > 0.1 ? dx / g.range : 0.0; g.uy = g.range > 0.1 ? dy / g.range : 1.0;
    g.aspect = std::fabs(Wrap(std::atan2(-dx, -dy) - in.foe.heading));   // 0: its nose on me
    g.offTail = kPi - g.aspect;                                         // 0: I sit right on its tail
    g.angleOff = std::fabs(Wrap(std::atan2(dx, dy) - in.me.heading));   // 0: my nose on it
    g.closure = (in.me.vx - in.foe.vx) * g.ux + (in.me.vy - in.foe.vy) * g.uy;  // + closing
    g.mySpeed = std::hypot(in.me.vx, in.me.vy);
    g.foeSpeed = std::hypot(in.foe.vx, in.foe.vy);
    return g;
}
inline int StageOf(State s)
{
    switch (s) {
    case State::Scissors: case State::Circle: return 2;
    case State::Overwatch: return 3;
    case State::Ram: case State::Sideswipe: return 4;
    default: return 1;
    }
}
inline bool Allowed(State s, const Settings& set)
{
    const bool ramLike = s == State::Ram || s == State::Sideswipe;
    return StageOf(s) <= set.stage && !ramLike;  // rams and sideswipes removed (2026-10-25)
}

// The state wanted now and why (for the log). The hold is applied here; Enter() records a change.
inline State Choose(const Input& in, const Mind& m, const Settings& s, const char** why = nullptr)
{
    const char* dummy = nullptr;
    const char*& w = why ? *why : dummy;
    const Geo g = Measure(in);
    const auto since = in.now - m.since;
    const bool losing = !std::isnan(in.foeHealth) && in.myHealth < in.foeHealth - kLosing;
    // Orders that run to their end.
    if (m.started && m.state == State::Ram && in.ramming) { w = "ram order running"; return State::Ram; }
    if (in.myHealth < s.breakOff || (m.started && m.state == State::BreakOff && in.myHealth < kBreakOffDone)) { w = "health low"; return State::BreakOff; }
    if (m.started && m.state == State::Sideswipe && since < kSideswipeMs && m.minRange >= 4.0) { w = "sideswipe running"; return State::Sideswipe; }
    // Threats cut in at once.
    const bool behindMe = in.foeInVehicle && g.angleOff > kBehindMe && g.aspect < kEnemyOnMe && g.range < kBehindRange &&
        (!m.breakEnd || in.now - m.breakEnd >= kBreakMs);  // not straight back into another break
    if (in.hitRecently || behindMe) {
        if (m.started && m.state == State::Break && since >= kBreakMs && !in.hitRecently) { /* the break ran: pick again below */ }
        else { w = in.hitRecently ? "hit" : "enemy behind"; return State::Break; }
    }
    if (m.started && m.state == State::Break && since < kBreakMs) { w = "break running"; return State::Break; }
    // Close quarters: scissors after an overshoot (stage 2), else extend.
    const bool passed = g.range < kOvershoot && g.angleOff > kPi / 2 && g.closure < 0;
    if (in.foeInVehicle && passed && Allowed(State::Scissors, s) && !(m.started && m.state == State::Scissors && since >= kScissorsMs)) {
        w = "overshoot"; return State::Scissors;
    }
    if (m.started && m.state == State::Scissors && since < kScissorsMs && g.range < 40.0) { w = "scissors running"; return State::Scissors; }
    if (g.range < kTooClose && !(m.started && m.state == State::Scissors)) { w = "too close"; return State::Extend; }
    if (m.started) {
        if (m.state == State::Extend && g.range < m.extendOut && since < kExtendMaxMs) { w = "extend running"; return State::Extend; }
        if (m.state == State::Extend && m.reengage) { w = "re-engage"; return State::AttackRun; }
        if (m.state == State::AttackRun) {
            const bool past = g.range < kFireNear || (m.minRange < 60.0 && g.closure < 0);
            if (past) { w = "pass done"; return State::Extend; }
            if (since < kExtendMaxMs * 2) { w = "attack run running"; return State::AttackRun; }
        }
        const bool chosen = m.state == State::CloseIn || m.state == State::LagPursuit || m.state == State::Circle || m.state == State::Overwatch;
        if (chosen && since < s.holdMs) { w = "held"; return m.state; }
    }
    // Aggression (stage 2/3): losing or outnumbered -> extend and come back with an attack run.
    if ((losing || in.enemiesOnMe >= 2) && Allowed(State::Circle, s) && in.now - m.lastReengage >= kReengageCooldownMs && g.range < 120.0) {
        w = losing ? "losing" : "outnumbered"; return State::Extend;
    }
    // Stage 4: ram a slow enemy straight ahead when stronger; sideswipe one side-on at close range when faster.
    if (in.foeInVehicle && Allowed(State::Ram, s) && in.now - m.lastRam >= kRamCooldownMs) {
        const bool stronger = in.myHealth >= (std::isnan(in.foeHealth) ? 0.6 : in.foeHealth + 0.15);
        if (stronger && g.range > 18.0 && g.range < 70.0 && g.angleOff < 0.5 && g.foeSpeed < std::max(6.0, g.mySpeed * 0.7)) { w = "slow target ahead"; return State::Ram; }
        if (g.range < 25.0 && g.aspect > 1.2 && g.aspect < 1.95 && g.mySpeed > g.foeSpeed + 5.0) { w = "side-on, faster"; return State::Sideswipe; }
    }
    // Stage 3 roles: several on one target -> the first engages, the second takes the tail (pincer), the rest hold overwatch.
    if (in.sameSide >= 2 && Allowed(State::Overwatch, s)) {
        if (in.rank >= 2) { w = "role: overwatch"; return State::Overwatch; }
        if (in.rank == 1) { w = "role: pincer to its tail"; return in.foeInVehicle ? State::LagPursuit : State::Circle; }
    }
    // Geometry.
    if (in.foeInVehicle && g.offTail < kTailCone && g.range < kFireFar && g.foeSpeed > 6.0) { w = "behind it"; return State::LagPursuit; }
    if (g.range > kAttackFrom && g.angleOff < kFacing) { w = "facing it, in reach"; return State::AttackRun; }
    if (g.range > kAttackFrom) { w = "far"; return State::CloseIn; }
    if (Allowed(State::Circle, s)) { w = "close, side-on"; return State::Circle; }
    w = "close"; return State::Extend;  // stage 1 without the circle: out and round again with an attack run
}

// Records a state change (sides, the extend direction and distance, cooldowns, the circle's first radius).
inline bool Enter(Mind& m, State want, const Input& in)
{
    const Geo g = Measure(in);
    if (m.started && want == m.state) { m.minRange = std::min(m.minRange, g.range); return false; }
    const State was = m.state;
    const double rx = g.uy, ry = -g.ux;  // right of the line to the enemy
    const double lateral = in.me.vx * rx + in.me.vy * ry;
    m.started = true;
    m.state = want;
    m.since = in.now;
    m.minRange = g.range;
    if (was == State::Break) m.breakEnd = in.now;
    const std::uint64_t pseudo = (in.now / 7) % 1000;  // a cheap spread for distances and timings
    if (want == State::AttackRun || want == State::Circle || want == State::LagPursuit || want == State::Sideswipe) m.side = lateral >= 0 ? 1 : -1;
    if (want == State::Break) m.side = (in.me.vx * rx + in.me.vy * ry) >= 0 ? 1 : -1;
    if (want == State::Scissors) { m.side = lateral >= 0 ? -1 : 1; m.nextFlip = in.now + kScissorFlipMs; }
    if (want == State::Circle) { m.radius = kCircleMin + (kCircleMax - kCircleMin) * (pseudo / 1000.0); m.nextFlip = in.now + kCircleMinMs + pseudo * (kCircleMaxMs - kCircleMinMs) / 1000; }
    if (want == State::Ram) m.lastRam = in.now;
    if (want == State::Sideswipe) m.lastRam = in.now;
    if (want == State::Extend) {
        const bool fromRun = was == State::AttackRun || was == State::Sideswipe;
        m.reengage = true;
        m.extendOut = fromRun ? 150.0 + 100.0 * (pseudo / 1000.0) : 200.0 + 100.0 * (pseudo / 1000.0);
        if (!fromRun) m.lastReengage = in.now;
        const double a = std::atan2(-g.ux, -g.uy) + (lateral >= 0 ? 0.35 : -0.35);
        m.ex = std::sin(a); m.ey = std::cos(a);
    }
    return true;
}

// Stage 4 terrain: the directions an extend / break off may take to get out of the enemy's sight (the caller tests each line of sight from the
// enemy to me + dir * dist and calls Hide with the first hidden one).
inline int HideCandidates(const Mind& m, double out[5][2])
{
    const double a = std::atan2(m.ex, m.ey);
    const double offs[5] = { 0.0, 0.5, -0.5, 1.0, -1.0 };
    for (int i = 0; i < 5; ++i) { out[i][0] = std::sin(a + offs[i]); out[i][1] = std::cos(a + offs[i]); }
    return 5;
}
inline void Hide(Mind& m, double dx, double dy) { Norm(dx, dy); m.ex = dx; m.ey = dy; }

// The carrot and the controls for the current state.
inline Plan Steer(const Input& in, Mind& m, const Settings& s)
{
    Plan p;
    const Geo g = Measure(in);
    const double rx = g.uy, ry = -g.ux;
    const double t = std::clamp(g.range / std::max(20.0, g.mySpeed + 10.0), 0.0, 2.5);
    const double lx = in.foe.x + in.foe.vx * t, ly = in.foe.y + in.foe.vy * t;  // where the enemy will be
    double dx = 0, dy = 1;
    bool minSpeed = true;
    p.z = in.foe.z + 4.0;
    switch (m.state) {
    case State::CloseIn:
        dx = lx - in.me.x; dy = ly - in.me.y; p.speed = kFastSpeed;
        break;
    case State::AttackRun: {
        // In with lead on a line that passes kPassBy beside it, a little above; the nose corrects onto the aim point inside the fire band.
        dx = lx + rx * m.side * kPassBy - in.me.x; dy = ly + ry * m.side * kPassBy - in.me.y;
        p.z = in.foe.z + 6.0;
        p.speed = kFastSpeed;
        p.aim = g.range < kFireFar && g.range > kFireNear && g.angleOff < 0.35;
        break;
    }
    case State::LagPursuit: {
        // Sit kLagMin..kLagMax behind its tail, following its turns a little late (its heading, not its lead); nose on it.
        const double fx = std::sin(in.foe.heading), fy = std::cos(in.foe.heading);
        const double want = 0.5 * (kLagMin + kLagMax);
        const double bx = in.foe.x - fx * want, by = in.foe.y - fy * want;
        dx = bx - in.me.x; dy = by - in.me.y;
        const double gap = std::hypot(dx, dy);
        if (gap < 10.0) { dx = fx; dy = fy; }  // on the spot: fly along its track
        p.speed = std::clamp(g.foeSpeed + (g.range - want) * 0.4, kMinSpeed, kFastSpeed);
        p.aim = g.angleOff < 0.6;
        break;
    }
    case State::Circle: {
        if (in.now >= m.nextFlip) {  // a new radius and direction every 3-6 s
            const std::uint64_t pseudo = (in.now / 7) % 1000;
            m.radius = kCircleMin + (kCircleMax - kCircleMin) * (pseudo / 1000.0);
            m.side = -m.side;
            m.nextFlip = in.now + kCircleMinMs + pseudo * (kCircleMaxMs - kCircleMinMs) / 1000;
        }
        const double k = std::clamp((g.range - m.radius) / m.radius * 1.5, -1.0, 1.0);
        dx = rx * m.side + g.ux * k; dy = ry * m.side + g.uy * k;
        p.speed = kMinSpeed;
        p.aim = true;
        p.throttle = std::clamp(k, -0.5, 1.0);
        p.strafe = m.side * 0.8;
        minSpeed = false;
        break;
    }
    case State::Break: {
        // A hard turn toward the threat's side with boost and a height change.
        const double hx = std::sin(in.me.heading), hy = std::cos(in.me.heading);
        const double sideOfThreat = (g.ux * hy - g.uy * hx) >= 0 ? 1.0 : -1.0;  // + : the threat is to my right
        const double px = hy * sideOfThreat, py = -hx * sideOfThreat;          // my right (or left) wing
        dx = px + g.ux * 0.5; dy = py + g.uy * 0.5;
        p.speed = kFastSpeed;
        if (in.hover) p.vertical = ((in.now - m.since) / 700) % 2 ? -4.0 : 4.0;
        break;
    }
    case State::Scissors: {
        // Turn into it again and again, slowly, so it overshoots back.
        if (in.now >= m.nextFlip) { m.side = -m.side; m.nextFlip = in.now + kScissorFlipMs; }
        dx = g.ux + rx * m.side * 1.2; dy = g.uy + ry * m.side * 1.2;
        p.speed = 12.0;
        p.aim = g.angleOff < 0.6;
        minSpeed = false;
        break;
    }
    case State::Extend:
        dx = m.ex; dy = m.ey; p.speed = kFastSpeed;
        break;
    case State::Overwatch: {
        // A held spot kOverwatchRange out on my side of it, high; nose on it.
        p.x = in.foe.x - g.ux * kOverwatchRange; p.y = in.foe.y - g.uy * kOverwatchRange; p.z = in.foe.z + kOverwatchUp;
        p.hold = true; p.speed = kFastSpeed * s.speedScale; p.aim = std::hypot(p.x - in.me.x, p.y - in.me.y) < 20.0;
        return p;
    }
    case State::Ram:
        p.ram = true; dx = lx - in.me.x; dy = ly - in.me.y; p.speed = kFastSpeed;
        break;
    case State::Sideswipe:
        // Brush its side: a line 2 m off its flank, flat out.
        dx = in.foe.x + rx * m.side * 2.0 - in.me.x; dy = in.foe.y + ry * m.side * 2.0 - in.me.y;
        p.speed = kFastSpeed; p.z = in.foe.z + 1.0;
        break;
    case State::BreakOff: {
        const double ax = in.anchorX - in.me.x, ay = in.anchorY - in.me.y, ad = std::hypot(ax, ay);
        if (g.range > kBreakOffDist) { dx = rx * m.side; dy = ry * m.side; p.speed = 12.0; minSpeed = false; }
        else { dx = -g.ux + (ad > 1 ? ax / ad * 0.6 : 0.0); dy = -g.uy + (ad > 1 ? ay / ad * 0.6 : 0.0); p.speed = kFastSpeed; }
        break;
    }
    }
    Norm(dx, dy);
    // Separation: other vehicles closer than kSeparation push the carrot away (not the enemy itself in a ram or sideswipe).
    for (int i = 0; i < in.others && i < kMaxOthers; ++i) {
        const double ox = in.me.x - in.otherX[i], oy = in.me.y - in.otherY[i], d = std::hypot(ox, oy);
        if (d < kSeparation && d > 0.1) { const double w = (kSeparation - d) / kSeparation * 2.0; dx += ox / d * w; dy += oy / d * w; }
    }
    Norm(dx, dy);
    if (minSpeed) p.speed = std::max(p.speed, kMinSpeed);
    p.speed *= s.speedScale;
    const double l = std::max(kCarrotMin, kCarrotTime * g.mySpeed);
    // The zone: a carrot outside it bends back toward the anchor.
    for (int i = 0; i < 3 && std::hypot(in.me.x + dx * l - in.anchorX, in.me.y + dy * l - in.anchorY) > s.zone; ++i) {
        double ax = in.anchorX - in.me.x, ay = in.anchorY - in.me.y; Norm(ax, ay);
        dx += ax * 1.5; dy += ay * 1.5; Norm(dx, dy);
    }
    p.x = in.me.x + dx * l; p.y = in.me.y + dy * l;
    return p;
}
}
