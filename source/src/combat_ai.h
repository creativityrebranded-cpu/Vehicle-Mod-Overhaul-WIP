#pragma once
#include "combat_brain.h"
#include <cstdio>
// Part E of the full vehicle combat AI (2026-10-21, notes/VEHICLE_COMBAT_AI_FULL_SPEC_20261021.md): one tick for one vehicle, all parts together.
// Perception and memory -> impulses -> morale -> the geometry and the scored manoeuvres (B) and tactics (D) -> the behaviour tree (C) -> the
// chosen leaf's steering requests -> the prioritized truncated weighted sum with the safety behaviours first (A) -> the terrain check (the
// autopilot's planner wins when the steering would meet terrain within 2.5 s) -> flight commands, the aim point (with the driver's accuracy),
// whether to fire, and one log line when the decision changes. The engine side (npcvehicles.cpp) fills Situation and applies Output.
// Pure logic. Tests: tests/combat_ai_test.cpp.
namespace combatai {
using steer::V3;

struct Switches { bool steering = true, dogfight = true, tactics = true, squads = true, morale = true, personality = true; };

struct Self
{
    std::uint32_t id = 0;
    tactics::Unit unit;
    double turnRate = 1.6, boost = 1.0;
    bool wrecked = false;
    brain::Personality person;
};
struct Foe
{
    std::uint32_t id = 0;
    tactics::Unit unit;
    bool inVehicle = false, visible = true, heard = false, firing = false;
};
struct Situation
{
    std::uint64_t now = 0;
    double dt = 0.016;
    Self me;
    bool haveFoe = false;
    Foe foe;
    steer::Body others[8]; int otherCount = 0;   // vehicles near me (collision avoidance)
    steer::Body mates[8]; int mateCount = 0;     // my squad (flocking, line of fire)
    tactics::Unit enemies[8]; int enemyCount = 0;
    V3 anchor; double zone = 150.0;              // the fight zone (containment)
    bool protect = false; steer::Body protectee; double protectThreat = 0;  // an escort's player
    bool hasLeader = false; steer::Body leader; V3 slot{ 0, -15, 0 };
    brain::Role role = brain::Role::Engager; int roleSlot = 0; bool haveCover = false; steer::Body coverBody;
    bool hitRecently = false, allyWrecked = false, allyDriverKilled = false, leaderKilled = false, playerOnFoot = false, reinforcements = false;
    int alliesOnTarget = 1, enemiesOnMe = 1;
    bool targetInConvoy = false, targetIsCargo = false, iDefendConvoy = false;
    bool allowRam = true;
    double breakOff = 0.3;
    bfm::Weapon weapon;
    bool civilian = false;
};
struct State
{
    brain::Decision dec; bool haveDec = false;
    bfm::MState ms; tactics::TState ts;
    brain::Impulses imp; brain::Memory mem; brain::Morale morale; bool moraleInit = false; brain::Mood mood = brain::Mood::Steady;
    brain::Search search;
    steer::Smoother smooth; steer::WanderState wander; steer::Rng rng{ 1 };
    std::uint32_t lastFoe = 0; bool wasVisible = false; std::uint64_t defensiveAt = 0;
    V3 aimErr; std::uint64_t aimErrAt = 0;
    bool unsafeTold = false;
    bool baitTurned = false;
    bfm::M completedM=bfm::M::LeadPursuit; std::uint64_t completedUntil=0;
    bool completedLastTick=false;
    // The world reads are the costly part (nav map lookups): the tactic facts every kFactsMs, a hiding spot every kHideMs.
    tactics::Facts facts; std::uint64_t factsAt = 0; std::uint32_t factsFoe = 0;
    bool hideFound = false; V3 hideSpot; std::uint64_t hideAt = 0;
};
inline constexpr std::uint64_t kFactsMs = 250, kHideMs = 500;
struct Output
{
    steer::Commands cmd;
    bool useCommands = false;           // false: only the carrot (the planner flies; the terrain check failed or steering is off)
    V3 carrot; double speed = 0; bool hold = false;
    bool aim = false; V3 aimPoint;
    bool fire = false;
    bool contact = false;               // a tactic that touches the target (rams, pin, sideswipe, push-down)
    double strafe = std::numeric_limits<double>::quiet_NaN();
    bool changed = false; char log[240] = "";
    bool moodChanged = false;
    const char* leaf = "";
    std::uint32_t used = 0;             // the behaviours the combiner used (for the verbose log)
};

inline const char* LeafName(const brain::Decision& d)
{
    if (d.action == brain::Action::Bfm) return bfm::Name(d.m);
    if (d.action == brain::Action::Tactic) return tactics::Name(d.t);
    return brain::Name(d.action);
}

inline bfm::Fighter FighterOf(const tactics::Unit& u, double turnRate, double boost) { bfm::Fighter f; f.body = u.body; f.maxSpeed = u.maxSpeed; f.turnRate = turnRate; f.boost = boost; f.health = u.health; return f; }

// The steering force of one bfm request (resolved against the agent and the enemy).
inline V3 Resolve(const bfm::Request& r, const steer::Agent& a, const steer::Body& foe)
{
    switch (r.kind) {
    case bfm::Request::Kind::Seek: return steer::Seek(a, r.point);
    case bfm::Request::Kind::Pursuit: return steer::Pursuit(a, foe);
    case bfm::Request::Kind::Offset: return steer::OffsetPursuit(a, foe, r.point);
    case bfm::Request::Kind::Flee: return steer::Flee(a, r.point);
    case bfm::Request::Kind::Evade: return steer::Evasion(a, foe);
    case bfm::Request::Kind::Direction: return steer::Seek(a, a.pos + steer::Norm(r.point) * 50.0);
    case bfm::Request::Kind::Arrive: return steer::Arrive(a, r.point, 12.0);
    default: return {};
    }
}

inline void Tick(State& st, const Situation& input, const steer::World& w, const Switches& sw, Output& out)
{
    Situation s=input;
    out = Output{};
    const auto now = s.now;
    const steer::Body& foeB = s.foe.unit.body;
    // ---- Perception and memory ----
    if (s.haveFoe) {
        st.mem.Sense(s.foe.id, foeB.pos, foeB.vel, s.foe.visible, s.foe.heard || (s.foe.firing && steer::Len(foeB.pos - s.me.unit.body.pos) < brain::kHearGunfire), s.foe.unit.health, now);
        if (st.lastFoe != s.foe.id) { st.imp.Raise(brain::Impulse::NewEnemy, now); st.ms.started=false; st.ts.started=false; st.factsAt=0; st.search.active=false; }
        if (st.wasVisible && !s.foe.visible) st.imp.Raise(brain::Impulse::TargetLost, now);
        st.wasVisible = s.foe.visible;
        st.lastFoe = s.foe.id;
    }
    const brain::Track* track=st.mem.Find(s.haveFoe?s.foe.id:st.lastFoe);
    const auto observed=track?std::max(track->seenAt,track->heardAt):0;
    const bool remembered=track&&observed&&now>=observed&&now-observed<60000;
    const double sinceSeen=remembered?double(now-observed)/1000.0:1e9;
    if (!s.haveFoe) { s.foe.visible=false; s.foe.heard=false; }
    if (remembered&&!s.foe.visible) {
        s.foe.unit.body.pos=track->pos+track->vel*std::min(sinceSeen,2.0);
        s.foe.unit.body.vel=track->vel; s.foe.unit.health=track->health;
    }
    // ---- Impulses (raised on the stimulus; the tree reacts after the reaction time) ----
    auto edge = [&](bool on, brain::Impulse i, std::uint64_t gap) { if (on && now - st.imp.at[int(i)] > gap) st.imp.Raise(i, now); };
    edge(s.hitRecently, brain::Impulse::Hit, 2000);
    edge(s.allyWrecked, brain::Impulse::AllyWrecked, 5000);
    edge(s.allyDriverKilled, brain::Impulse::AllyDriverKilled, 5000);
    edge(s.leaderKilled, brain::Impulse::LeaderKilled, 10000);
    edge(s.playerOnFoot, brain::Impulse::PlayerOnFoot, 5000);
    steer::Agent a;
    a.pos = s.me.unit.body.pos; a.vel = s.me.unit.body.vel; a.heading = s.me.unit.body.heading; a.maxSpeed = s.me.unit.maxSpeed; a.mass = s.me.unit.mass;
    a.radius = s.me.unit.body.radius; a.maxTurn = s.me.turnRate;
    if (s.haveFoe && s.foe.inVehicle) {
        double when = -1;
        steer::CollisionAvoidance(a, &foeB, 1, &when);
        const bool coming = when >= 0 && when < 1.5 && steer::Len2D(foeB.vel) > 10.0 &&
            std::fabs(steer::Wrap(steer::HeadingOf(foeB.vel) - steer::HeadingOf(a.pos - foeB.pos))) < 0.35;
        edge(coming, brain::Impulse::RamIncoming, 3000);
    }
    // ---- Morale ----
    const brain::Personality person = sw.personality ? s.me.person : brain::Personality{};
    const brain::Mood moodBefore = st.mood;
    if (sw.morale) {
        if (!st.moraleInit) { st.moraleInit = true; st.morale.Init(person.courage); }
        if (st.imp.at[int(brain::Impulse::AllyWrecked)] == now) st.morale.Apply(brain::MoraleEvent::AllyWrecked);
        if (st.imp.at[int(brain::Impulse::LeaderKilled)] == now) st.morale.Apply(brain::MoraleEvent::LeaderKilled);
        if (st.imp.at[int(brain::Impulse::Hit)] == now) st.morale.Apply(brain::MoraleEvent::Hit);
        if (s.reinforcements) st.morale.Apply(brain::MoraleEvent::Reinforcements);
        const double odds = double(std::max(1, s.enemyCount)) / double(s.mateCount + 1);
        st.morale.Tick(s.dt, odds, s.me.unit.health - (s.haveFoe ? s.foe.unit.health : s.me.unit.health));
        st.mood = st.morale.Of(s.mateCount == 0, s.leaderKilled, s.me.unit.health);
    }
    else st.mood = brain::Mood::Steady;
    out.moodChanged = st.mood != moodBefore;
    // ---- Geometry, scored manoeuvres and tactics ----
    brain::Blackboard b;
    b.now = now; b.person = person; b.mood = st.mood; b.health = s.me.unit.health; b.breakOff = s.breakOff; b.wrecked = s.me.wrecked;
    b.impulses = &st.imp; b.haveTarget = s.haveFoe||remembered; b.targetVisible = s.foe.visible; b.sinceSeen = s.foe.visible ? 0.0 : s.haveFoe?sinceSeen:std::max(3.0,sinceSeen);
    b.searching = st.search.active; b.reachedLast = st.search.active && st.search.leg >= 0;
    b.role = s.role; b.hasLeader = s.hasLeader; b.protect = s.protect; b.protectThreat = s.protectThreat; b.civilian = s.civilian;
    b.bfmOn = true; b.tacticsOn = sw.tactics; b.squadsOn = sw.squads;
    const bfm::Fighter me = FighterOf(s.me.unit, s.me.turnRate, s.me.boost);
    const bfm::Fighter it = FighterOf(s.foe.unit, 1.6, 0.5);
    bfm::Geo g;
    tactics::Facts facts;
    if (s.haveFoe) {
        g = bfm::Measure(me, it, s.weapon);
        if (bfm::Classify(g) == bfm::Situation::Defensive) st.defensiveAt = now;
        bfm::Ctx bc; bc.underFire = s.hitRecently; bc.wasDefensive = st.defensiveAt && now - st.defensiveAt < 4000; bc.enemiesOnMe = s.enemiesOnMe; bc.aggression = person.aggression;
        // Dogfight off: the three pursuits only.
        for (int i = 0; i < int(bfm::M::Count); ++i) {
            const bool basic = i <= int(bfm::M::LagPursuit);
            b.bfm[i] = (sw.dogfight || basic) && !(now<st.completedUntil&&bfm::M(i)==st.completedM) ? bfm::Desire(bfm::M(i), g, bc) : 0.0;
        }
        if (sw.tactics) {
            if (!st.factsAt || now - st.factsAt >= kFactsMs || st.factsFoe != s.foe.id) { st.facts = tactics::Read(w, s.me.unit, s.foe.unit, s.enemies, s.enemyCount); st.factsAt = now; st.factsFoe = s.foe.id; }
            facts = st.facts;
            tactics::Ctx tc; tc.allowRam = s.allowRam; tc.alliesOnTarget = s.alliesOnTarget; tc.targetInConvoy = s.targetInConvoy; tc.targetIsCargo = s.targetIsCargo;
            tc.iDefendConvoy = s.iDefendConvoy; tc.aggression = person.aggression;
            for (int i = 0; i < int(tactics::T::Count); ++i) b.tac[i] = s.foe.inVehicle || !tactics::MakesContact(tactics::T(i)) ? tactics::Desire(tactics::T(i), s.me.unit, s.foe.unit, g, facts, tc) : 0.0;
            if (facts.pinnedMe) edge(true, brain::Impulse::Pinned, 3000);
        }
    }
    // ---- The tree ----
    const brain::Decision prev = st.dec;
    st.dec = brain::Tick(b, prev, st.haveDec&&!st.completedLastTick);
    st.completedLastTick=false;
    const bool changed = !st.haveDec || !st.dec.Same(prev) || std::string_view(st.dec.branch) != prev.branch;
    st.haveDec = true;
    out.leaf = LeafName(st.dec);
    if (changed) {
        out.changed = true;
        std::snprintf(out.log, sizeof(out.log), "%s: %s (%s%s%s)%s%s", st.dec.branch, out.leaf, st.dec.why, st.dec.impulse >= 0 ? "; impulse: " : "",
            st.dec.impulse >= 0 ? brain::Name(brain::Impulse(st.dec.impulse)) : "", s.haveFoe ? "; " : "", s.haveFoe ? bfm::Name(bfm::Classify(g)) : "");
    }
    if(changed) { st.ms.started=false; st.ts.started=false; }
    if (st.dec.action == brain::Action::Bfm && s.haveFoe) bfm::Enter(st.ms, st.dec.m, me, it, now);
    if (st.dec.action == brain::Action::Tactic && s.haveFoe) tactics::Enter(st.ts, st.dec.t, s.me.unit, s.foe.unit, facts, now, s.roleSlot);
    if (st.dec.action == brain::Action::SearchLast && !st.search.active && track) brain::StartSearch(st.search, *track);
    if (st.dec.action != brain::Action::SearchLast && st.dec.action != brain::Action::SearchPattern) st.search.active = false;
    if (st.dec.action != brain::Action::Bait) st.baitTurned = false;
    // ---- The leaf's steering requests ----
    bfm::Recipe r;
    bool contact = false;
    double strafe = std::numeric_limits<double>::quiet_NaN();
    const V3 u = s.haveFoe ? steer::Norm(steer::Flat(foeB.pos - a.pos)) : steer::Fwd(a.heading);
    switch (st.dec.action) {
    case brain::Action::Bfm:
        if (s.haveFoe) r = bfm::Plan(st.ms, me, it, g, now, s.dt);
        break;
    case brain::Action::Tactic:
        if (s.haveFoe) { const tactics::Plan p = tactics::Steer(st.ts, s.me.unit, s.foe.unit, g, facts, now); r = p.r; contact = p.contact; strafe = p.strafe; }
        break;
    case brain::Action::Flee:
        r.Add(bfm::Request::Kind::Evade, V3{}, 1.0); r.speed = a.maxSpeed; r.boost = true;
        break;
    case brain::Action::Retreat: {
        if (s.haveFoe && (!st.hideAt || now - st.hideAt >= kHideMs)) { st.hideAt = now; steer::Hide(a, foeB, w, &st.hideFound, &st.hideSpot); }
        const bool found = s.haveFoe && st.hideFound; const V3 spot = st.hideSpot;
        if (found) r.Add(bfm::Request::Kind::Arrive, spot, 1.0);
        r.Add(bfm::Request::Kind::Evade, V3{}, found ? 0.4 : 1.0);
        r.Add(bfm::Request::Kind::Seek, s.anchor, 0.4);
        r.speed = a.maxSpeed; r.boost = true;
        break;
    }
    case brain::Action::Evade:
        r.Add(bfm::Request::Kind::Direction, steer::Right(steer::HeadingOf(u)) * (steer::Dot(a.vel, steer::Right(steer::HeadingOf(u))) >= 0 ? 1.0 : -1.0), 1.5);
        r.speed = a.maxSpeed; r.boost = true;
        break;
    case brain::Action::Escape:
        r.Add(bfm::Request::Kind::Direction, facts.escapeDir, 1.5); r.speed = a.maxSpeed; r.boost = true;
        break;
    case brain::Action::Interpose:
        r.speed = a.maxSpeed;
        break;
    case brain::Action::Suppress:
        r.Add(bfm::Request::Kind::Arrive, foeB.pos - u * 90.0); r.speed = a.maxSpeed * 0.8; r.aim = true;
        break;
    case brain::Action::Overwatch:
        r.Add(bfm::Request::Kind::Arrive, foeB.pos - u * 100.0 + V3{ 0, 0, 25.0 }); r.speed = a.maxSpeed * 0.8; r.aim = true;
        break;
    case brain::Action::Bait:
        // In to about 70 m, then run toward the squad (the ambush), keeping it on my tail.
        if (g.range < 75.0) st.baitTurned = true;
        if (!st.baitTurned) r.Add(bfm::Request::Kind::Seek, foeB.pos);
        else {
            V3 c = a.pos; for (int i = 0; i < s.mateCount; ++i) c = c + s.mates[i].pos;
            r.Add(bfm::Request::Kind::Seek, s.mateCount ? c * (1.0 / (s.mateCount + 1)) + (steer::Norm(steer::Flat(c * (1.0 / (s.mateCount + 1)) - foeB.pos)) * 60.0) : a.pos - u * 100.0);
        }
        r.speed = a.maxSpeed;
        break;
    case brain::Action::Reserve:
        r.Add(bfm::Request::Kind::Arrive, foeB.pos - u * 150.0); r.speed = a.maxSpeed * 0.7;
        break;
    case brain::Action::Cover:
        r.speed = a.maxSpeed;
        break;
    case brain::Action::SearchLast:
    case brain::Action::SearchPattern:
        if (st.search.active) r.Add(bfm::Request::Kind::Seek, brain::SearchStep(st.search, a.pos)); r.speed = a.maxSpeed * 0.6;
        break;
    case brain::Action::FollowLeader:
        r.speed = a.maxSpeed;
        break;
    default:
        r.speed = a.maxSpeed * 0.4;
        break;
    }
    if(r.done&&st.dec.action==brain::Action::Bfm) {
        st.completedM=st.dec.m; st.completedUntil=now+1000; st.completedLastTick=true; st.ms.started=false;
    }
    // ---- Steering: the prioritized truncated weighted sum ----
    if (r.speed > 0) a.maxSpeed = std::min(a.maxSpeed * (r.boost ? 1.2 : 1.0), std::max(4.0, r.speed * (r.boost ? 1.2 : 1.0)));
    steer::Combiner c(a.maxForce);
    c.Add(steer::WallAvoidance(a, w), 1.5);
    c.Add(steer::ObstacleAvoidance(a, w), 1.2);
    {
        steer::Body others[9]; int n = 0;
        for (int i = 0; i < s.otherCount && n < 8; ++i) if (!(contact && steer::Len(s.others[i].pos - foeB.pos) < 1.0)) others[n++] = s.others[i];
        if (s.haveFoe && !contact && s.foe.inVehicle) others[n++] = foeB;
        c.Add(steer::CollisionAvoidance(a, others, n), 1.0);
        c.Add(steer::Separation(a, s.mates, s.mateCount, 15.0), 1.0);
    }
    c.Add(steer::Containment(a, s.anchor, s.zone), 1.0);
    // The leaf's own behaviours.
    if (st.dec.action == brain::Action::Interpose) c.Add(steer::Interpose(a, s.protectee, foeB), 1.0);
    else if (st.dec.action == brain::Action::Cover && s.haveCover) c.Add(steer::Interpose(a, s.coverBody, foeB), 1.0);
    else if (st.dec.action == brain::Action::FollowLeader) c.Add(steer::LeaderFollowing(a, s.leader, s.slot, s.mates, s.mateCount), 1.0);
    else if (st.dec.action == brain::Action::Wander || st.dec.action == brain::Action::None) c.Add(steer::Wander(a, st.wander, st.rng, s.dt), 1.0);
    for (int i = 0; i < r.n; ++i) c.Add(Resolve(r.req[i], a, foeB), r.req[i].weight);
    if (s.mateCount && st.dec.action != brain::Action::Bfm && st.dec.action != brain::Action::Tactic) {
        c.Add(steer::Alignment(a, s.mates, s.mateCount), 0.3);
        c.Add(steer::Cohesion(a, s.mates, s.mateCount), 0.2);
    }
    c.Add(steer::Wander(a, st.wander, st.rng, s.dt), 0.1);
    out.used = c.used;
    V3 desired = steer::Integrate(a, c.total);
    if (!std::isnan(r.vertical)) desired.z = r.vertical;
    desired = st.smooth.Apply(desired, s.dt);
    // ---- Aim and fire ----
    if (s.haveFoe && (r.aim || st.dec.action == brain::Action::Suppress || st.dec.action == brain::Action::Overwatch)) {
        if (now - st.aimErrAt > 600) { st.aimErrAt = now; st.aimErr = brain::AimError(person, g.range, st.rng); }
        out.aim = true;
        out.aimPoint = g.lead + st.aimErr + V3{ 0, 0, s.foe.inVehicle ? 1.0 : 1.2 };
    }
    const bool mayFire = st.dec.action != brain::Action::Flee && st.dec.action != brain::Action::Retreat && st.dec.action != brain::Action::Bait &&
        st.dec.action != brain::Action::Reserve && s.haveFoe && s.foe.visible && !s.me.wrecked;
    out.fire = mayFire && bfm::FireSolution(me, g, nullptr, 0);
    if (out.fire && s.mateCount) {
        V3 lines[8]; for (int i = 0; i < s.mateCount; ++i) lines[i] = s.mates[i].pos;
        out.fire = !bfm::AllyInLine(a.pos, g.lead, lines, s.mateCount);
    }
    // ---- Commands, or the planner when the terrain check fails ----
    const double sp = steer::Len2D(desired);
    const double len = std::max(50.0, 2.2 * sp);
    const V3 dir = sp > 0.5 ? steer::Norm(steer::Flat(desired)) : steer::Fwd(a.heading);
    out.carrot = a.pos + dir * len; out.carrot.z = a.pos.z + desired.z * 2.0;
    out.speed = std::max(sp, 4.0);
    out.contact = contact; out.strafe = strafe;
    const bool safe = steer::SafeAhead(a, desired, w);
    out.useCommands = sw.steering && safe;
    if (out.useCommands) {
        out.cmd = steer::ToCommands(a, desired, out.aim ? &out.aimPoint : nullptr);
        if (!std::isnan(strafe)) out.cmd.strafe = strafe;
    }
    st.unsafeTold = !safe;
}
}
