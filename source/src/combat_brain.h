#pragma once
#include "combat_tactics.h"
#include <string_view>
// Part C of the full vehicle combat AI (2026-10-21, notes/VEHICLE_COMBAT_AI_FULL_SPEC_20261021.md): decisions and squads after Halo 2/3 (Damian Isla,
// "Handling Complexity in the Halo 2 AI", GDC 2005, and the Halo 3 talks): a behaviour tree per driver (prioritized selectors, sequences, conditions,
// and a desirability selector whose children are scored, with a bonus for the running one: hysteresis), impulses (stimuli that interrupt the
// tree at their branch, after the driver's reaction time), perception and memory (sight, hearing, last known position and velocity; shared in
// the squad), search (last known position, then an expanding square), a squad brain (roles every 1.5 s: engager, flanker, suppressor, overwatch,
// bait, reserve, cover, box slots; tactics: pincer, suppress-and-flank, bait-and-ambush, focus fire, cover a damaged ally, box in, retreat
// together), morale (courage, events, retreat / panic / rally), personality (from the NPC's faction) and difficulty (the game's setting).
// Pure logic. Tests: tests/combat_brain_test.cpp.
namespace brain {
using steer::V3;

// ---- Personality and difficulty ----
enum class Temper : std::uint8_t { Default, Reckless, Disciplined, Civilian };
inline const char* Name(Temper t) { return t == Temper::Reckless ? "reckless" : t == Temper::Disciplined ? "disciplined" : t == Temper::Civilian ? "civilian" : "default"; }
struct Personality { double aggression = 0.5, courage = 0.5, accuracy = 0.7; unsigned reactionMs = 400; Temper temper = Temper::Default; };
// Crimson Fleet / pirates: aggressive and reckless; UC / Freestar: disciplined and coordinated; civilians flee. A per-driver spread of +-0.1 from its id.
// difficulty: the game's uDifficulty (0 very easy .. 4 very hard): reaction time and accuracy scale with it.
inline Personality PersonalityFor(Temper t, std::uint32_t id, int difficulty)
{
    Personality p;
    p.temper = t;
    switch (t) {
    case Temper::Reckless: p.aggression = 0.85; p.courage = 0.75; p.accuracy = 0.6; p.reactionMs = 350; break;
    case Temper::Disciplined: p.aggression = 0.55; p.courage = 0.7; p.accuracy = 0.8; p.reactionMs = 300; break;
    case Temper::Civilian: p.aggression = 0.1; p.courage = 0.15; p.accuracy = 0.4; p.reactionMs = 600; break;
    default: break;
    }
    steer::Rng r(id ? id : 1);
    p.aggression = std::clamp(p.aggression + r.Signed() * 0.1, 0.0, 1.0);
    p.courage = std::clamp(p.courage + r.Signed() * 0.1, 0.0, 1.0);
    static const double kReact[5] = { 1.6, 1.3, 1.0, 0.8, 0.65 }, kAcc[5] = { 0.6, 0.8, 1.0, 1.15, 1.3 };
    const int d = std::clamp(difficulty, 0, 4);
    p.reactionMs = unsigned(p.reactionMs * kReact[d]);
    p.accuracy = std::clamp(p.accuracy * kAcc[d], 0.05, 1.0);
    return p;
}
// The aim error from accuracy: a sideways and vertical offset of the aim point that grows with range (re-rolled by the caller's rng).
inline V3 AimError(const Personality& p, double range, steer::Rng& rng) { const double s = (1.0 - p.accuracy) * 0.06 * range; return { rng.Signed() * s, rng.Signed() * s, rng.Signed() * s * 0.5 }; }

// ---- Impulses ----
enum class Impulse : std::uint8_t { Hit, AllyWrecked, AllyDriverKilled, TargetLost, NewEnemy, RamIncoming, PlayerOnFoot, LeaderKilled, Pinned, Count };
inline const char* Name(Impulse i)
{
    static const char* const n[] = { "hit", "ally wrecked", "ally driver killed", "target lost", "new enemy", "ram incoming", "player on foot near", "leader killed", "pinned" };
    return i < Impulse::Count ? n[int(i)] : "?";
}
struct Impulses
{
    std::uint64_t at[int(Impulse::Count)] = {};
    std::uint64_t handled[int(Impulse::Count)] = {};
    void Raise(Impulse i, std::uint64_t now) { at[int(i)] = now; }
    // Effective once the reaction time has passed, until handled or 3 s old.
    bool Active(Impulse i, std::uint64_t now, unsigned reactionMs) const
    {
        const auto a = at[int(i)];
        return a && a > handled[int(i)] && now >= a + reactionMs && now < a + reactionMs + 3000;
    }
    void Handle(Impulse i, std::uint64_t now) { handled[int(i)] = now; }
};

// ---- Perception and memory ----
struct Track { std::uint32_t id = 0; V3 pos, vel; std::uint64_t seenAt = 0, heardAt = 0; bool visible = false; double health = 1.0; };
struct Memory
{
    static constexpr int kMax = 8;
    Track t[kMax];
    Track* Find(std::uint32_t id) { for (auto& x : t) if (x.id == id) return &x; return nullptr; }
    const Track* Find(std::uint32_t id) const { for (const auto& x : t) if (x.id == id) return &x; return nullptr; }
    Track& Slot(std::uint32_t id)
    {
        if (Track* f = Find(id)) return *f;
        Track* old = &t[0];
        for (auto& x : t) { if (!x.id) { old = &x; break; } if (std::max(x.seenAt, x.heardAt) < std::max(old->seenAt, old->heardAt)) old = &x; }
        *old = Track{}; old->id = id;
        return *old;
    }
    // Sight (the caller tests the line of sight) or hearing (gunfire or an engine within its radius) refresh the track.
    void Sense(std::uint32_t id, const V3& pos, const V3& vel, bool seen, bool heard, double health, std::uint64_t now)
    {
        if (!seen && !heard) { if (Track* f = Find(id)) f->visible = false; return; }
        Track& k = Slot(id);
        if (seen) { k.pos = pos; k.vel = vel; k.seenAt = now; k.health = health; }
        else { k.heardAt = now; k.pos = pos; k.vel = {}; }
        k.visible = seen;
    }
    // Shared within the squad: the newer sighting of each enemy wins.
    void Merge(const Memory& o)
    {
        for (const auto& x : o.t) {
            if (!x.id) continue;
            Track& k = Slot(x.id);
            if (std::max(x.seenAt,x.heardAt)>std::max(k.seenAt,k.heardAt)) { k.pos=x.pos; k.vel=x.vel; k.health=x.health; }
            k.seenAt=std::max(k.seenAt,x.seenAt);
            k.heardAt = std::max(k.heardAt, x.heardAt);
        }
    }
};
inline constexpr double kHearGunfire = 200.0, kHearEngine = 60.0;
inline bool Hears(double distance, bool firing, double speed) { return (firing && distance < kHearGunfire) || (speed > 8.0 && distance < kHearEngine); }
// Search: the last known position (pushed on by the last velocity 2 s), then an expanding square around it (legs 30, 30, 60, 60, 90, ...).
struct Search { V3 origin; int leg = -1; V3 next; bool active = false; };
inline void StartSearch(Search& s, const Track& k) { s.active = true; s.leg = -1; s.origin = k.pos + k.vel * 2.0; s.next = s.origin; }
inline V3 SearchStep(Search& s, const V3& me, double reach = 12.0)
{
    if (steer::Len2D(me - s.next) > reach) return s.next;
    ++s.leg;
    static const V3 dirs[4] = { { 0, 1, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { -1, 0, 0 } };
    s.next = s.next + dirs[s.leg % 4] * (30.0 * double(s.leg / 2 + 1));
    return s.next;
}

// ---- Morale ----
enum class Mood : std::uint8_t { Steady, Shaken, Panic };
inline const char* Name(Mood m) { return m == Mood::Shaken ? "shaken" : m == Mood::Panic ? "panic" : "steady"; }
enum class MoraleEvent : std::uint8_t { AllyWrecked, LeaderKilled, Hit, EnemyWrecked, Reinforcements };
struct Morale
{
    double value = 0.6, courage = 0.5;
    void Init(double c) { courage = c; value = 0.4 + 0.5 * c; }
    void Apply(MoraleEvent e)
    {
        const double brave = 1.0 - 0.5 * courage;  // the brave lose less
        switch (e) {
        case MoraleEvent::AllyWrecked: value -= 0.15 * brave; break;
        case MoraleEvent::LeaderKilled: value -= 0.25 * brave; break;
        case MoraleEvent::Hit: value -= 0.02 * brave; break;
        case MoraleEvent::EnemyWrecked: value += 0.2; break;
        case MoraleEvent::Reinforcements: value += 0.2; break;  // rally
        }
        value = std::clamp(value, 0.0, 1.0);
    }
    // Per second: outnumbered drains, winning (health ahead) lifts, and it drifts back toward its courage.
    void Tick(double dt, double odds /* enemies / allies */, double healthLead)
    {
        if (odds > 1.0) value -= 0.02 * (odds - 1.0) * dt * (1.0 - 0.5 * courage);
        if (healthLead > 0.2) value += 0.01 * dt;
        const double base = 0.4 + 0.5 * courage;
        value += (base - value) * 0.01 * dt;
        value = std::clamp(value, 0.0, 1.0);
    }
    Mood Of(bool alone, bool leaderDead, double health) const
    {
        if (value < 0.15 || (alone && leaderDead) || (alone && health < 0.2)) return Mood::Panic;
        if (value < 0.35) return Mood::Shaken;
        return Mood::Steady;
    }
};

// ---- Squad brain ----
enum class Role : std::uint8_t { Engager, Flanker, Suppressor, Overwatch, Bait, Reserve, Cover, BoxSlot, Retreat, ConvoyGuard };
inline const char* Name(Role r)
{
    static const char* const n[] = { "engager", "flanker", "suppressor", "overwatch", "bait", "reserve", "cover", "box slot", "retreat", "convoy guard" };
    return int(r) < 10 ? n[int(r)] : "?";
}
enum class SquadTactic : std::uint8_t { Solo, Pincer, SuppressAndFlank, BaitAndAmbush, BoxIn, CoverAlly, RetreatTogether, ConvoyDefence };
inline const char* Name(SquadTactic t)
{
    static const char* const n[] = { "solo", "pincer", "suppress and flank", "bait and ambush", "box in", "cover a damaged ally", "retreat together", "convoy defence" };
    return n[int(t)];
}
struct Member { std::uint32_t id = 0; V3 pos, vel; double health = 1.0, morale = 0.6; Role role = Role::Engager; int slot = 0; std::uint32_t coverFor = 0; };
struct Enemy { std::uint32_t id = 0; V3 pos, vel; double health = 1.0; bool known = true; };
struct Squad
{
    static constexpr int kMax = 8;
    Member m[kMax];
    int count = 0;
    SquadTactic tactic = SquadTactic::Solo;
    std::uint32_t focus = 0;     // the enemy everyone fires at (the weakest)
    std::uint64_t assignedAt = 0;
    Memory memory;               // the shared memory
    bool convoy = false;         // defending a convoy
};
inline constexpr std::uint64_t kAssignMs = 1500;
// Roles and the tactic; returns true when any role changed (logged by the caller).
inline bool Assign(Squad& s, const Enemy* enemies, int enemyCount, std::uint64_t now, bool force = false)
{
    if (!force && s.assignedAt && now - s.assignedAt < kAssignMs) return false;
    s.assignedAt = now;
    Member before[Squad::kMax];
    const auto oldFocus=s.focus; const auto oldTactic=s.tactic;
    for (int i = 0; i < s.count; ++i) before[i] = s.m[i];
    // Focus fire: the weakest known enemy.
    const Enemy* focus = nullptr;
    for (int i = 0; i < enemyCount; ++i) if (enemies[i].known && (!focus || enemies[i].health < focus->health)) focus = &enemies[i];
    s.focus = focus ? focus->id : 0;
    double morale = 0;
    for (int i = 0; i < s.count; ++i) morale += s.m[i].morale;
    morale = s.count ? morale / s.count : 0.6;
    for (int i = 0; i < s.count; ++i) { s.m[i].coverFor = 0; s.m[i].slot = 0; }
    if (s.count && (morale < 0.35 || enemyCount >= 2 * s.count + 1)) {
        s.tactic = SquadTactic::RetreatTogether;
        for (int i = 0; i < s.count; ++i) s.m[i].role = Role::Retreat;
    }
    else if (s.convoy) {
        s.tactic = SquadTactic::ConvoyDefence;
        for (int i = 0; i < s.count; ++i) { s.m[i].role = Role::ConvoyGuard; s.m[i].slot = i; }
    }
    else if (!focus || s.count <= 1) {
        s.tactic = SquadTactic::Solo;
        for (int i = 0; i < s.count; ++i) s.m[i].role = Role::Engager;
    }
    else {
        // Order the members by distance to the focus enemy.
        int order[Squad::kMax];
        for (int i = 0; i < s.count; ++i) order[i] = i;
        std::sort(order, order + s.count, [&](int a, int b) { return steer::Len(s.m[a].pos - focus->pos) < steer::Len(s.m[b].pos - focus->pos); });
        // Bait: the enemy chases a member (its velocity points at it and it closes).
        int chased = -1;
        for (int i = 0; i < s.count; ++i) {
            const V3 to = s.m[i].pos - focus->pos;
            if (steer::Len2D(focus->vel) > 5.0 && std::fabs(steer::Wrap(steer::HeadingOf(focus->vel) - steer::HeadingOf(to))) < 0.4 && steer::Len(to) < 120.0) { chased = i; break; }
        }
        const bool fastTarget = steer::Len2D(focus->vel) > 5.0;
        if (s.count >= 3 && chased >= 0) {
            s.tactic = SquadTactic::BaitAndAmbush;
            int k = 0;
            for (int j = 0; j < s.count; ++j) {
                const int i = order[j];
                if (i == chased) { s.m[i].role = Role::Bait; continue; }
                s.m[i].role = (k++ % 2 == 0) ? Role::Flanker : Role::Overwatch;
            }
        }
        else if (s.count >= 3 && fastTarget) {
            s.tactic = SquadTactic::BoxIn;
            for (int j = 0; j < s.count; ++j) { const int i = order[j]; s.m[i].role = j < 4 ? Role::BoxSlot : Role::Reserve; s.m[i].slot = j; }
        }
        else if (s.count >= 3) {
            s.tactic = SquadTactic::SuppressAndFlank;
            static const Role roles[] = { Role::Engager, Role::Flanker, Role::Suppressor, Role::Overwatch };
            for (int j = 0; j < s.count; ++j) s.m[order[j]].role = j < 4 ? roles[j] : Role::Reserve;
        }
        else {
            s.tactic = SquadTactic::Pincer;
            s.m[order[0]].role = Role::Engager;
            s.m[order[1]].role = Role::Flanker;
        }
        // Cover a damaged ally: the nearest healthy member to an ally under 35 % with an enemy within 60 m of it.
        for (int i = 0; i < s.count; ++i) {
            if (s.m[i].health >= 0.35) continue;
            bool threatened = false;
            for (int e = 0; e < enemyCount; ++e) threatened = threatened || steer::Len(enemies[e].pos - s.m[i].pos) < 60.0;
            if (!threatened) continue;
            int best = -1;
            for (int j = 0; j < s.count; ++j) {
                if (j == i || s.m[j].health < 0.5 || s.m[j].role == Role::Bait) continue;
                if (best < 0 || steer::Len(s.m[j].pos - s.m[i].pos) < steer::Len(s.m[best].pos - s.m[i].pos)) best = j;
            }
            if (best >= 0) { s.m[best].role = Role::Cover; s.m[best].coverFor = s.m[i].id; s.tactic = SquadTactic::CoverAlly; }
        }
    }
    bool changed = oldFocus!=s.focus || oldTactic!=s.tactic;
    for (int i = 0; i < s.count; ++i) changed = changed || before[i].role!=s.m[i].role || before[i].slot!=s.m[i].slot || before[i].coverFor!=s.m[i].coverFor;
    return changed;
}
inline const Member* MemberOf(const Squad& s, std::uint32_t id) { for (int i = 0; i < s.count; ++i) if (s.m[i].id == id) return &s.m[i]; return nullptr; }

// ---- The behaviour tree ----
enum class Action : std::uint8_t { None, Flee, Retreat, Evade, Escape, Bfm, Tactic, Interpose, Suppress, Overwatch, Bait, Reserve, Cover, SearchLast, SearchPattern, FollowLeader, Wander };
inline const char* Name(Action a)
{
    static const char* const n[] = { "none", "flee", "retreat", "evade", "escape", "dogfight", "tactic", "interpose", "suppress", "overwatch", "bait", "reserve",
        "cover ally", "search last known position", "search pattern", "follow leader", "wander" };
    return n[int(a)];
}
// Everything a decision reads (filled by the caller each tick).
struct Blackboard
{
    std::uint64_t now = 0;
    Personality person;
    Mood mood = Mood::Steady;
    double health = 1.0, breakOff = 0.3;
    bool wrecked = false;
    Impulses* impulses = nullptr;
    bool haveTarget = false, targetVisible = false;
    double sinceSeen = 0;          // s since the target was seen (0 visible)
    bool searching = false;        // a search is under way
    bool reachedLast = false;      // the last known position was reached
    Role role = Role::Engager;
    bool hasLeader = false;
    bool protect = false;          // an escort with someone to protect (interpose)
    double protectThreat = 0;      // 0..1: how hard the threat closes on the protected one
    bool civilian = false;
    // The scored candidates of part B and D (the caller fills them from bfm::Desire / tactics::Desire).
    double bfm[int(bfm::M::Count)] = {};
    double tac[int(tactics::T::Count)] = {};
    bool bfmOn = true, tacticsOn = true, squadsOn = true;
};
struct Decision
{
    Action action = Action::None;
    bfm::M m = bfm::M::LeadPursuit;
    tactics::T t = tactics::T::CutOff;
    const char* branch = "";
    const char* why = "";
    int impulse = -1;              // the impulse that caused it (-1 none)
    double score = 0;
    std::uint64_t since = 0;       // when it was chosen (the minimum hold counts from here)
    bool Same(const Decision& o) const { return action == o.action && (action != Action::Bfm || m == o.m) && (action != Action::Tactic || t == o.t); }
};
inline constexpr double kHysteresis = 0.15;
// A chosen engage leaf runs at least this long while it stays possible (2026-10-21 run: cut-off and lag pursuit swapped every
// few milliseconds, 260 decisions in 3 minutes, because their scores crossed each frame). Interrupts (react, evade,
// self-preservation) are other branches and still act at once.
inline constexpr std::uint64_t kMinHoldMs = 1500;

// Node kinds of the tree; the tree itself is static data (kTree) walked by Tick.
enum class NodeKind : std::uint8_t { Selector, Sequence, Condition, Leaf, DesireSelector };
using CondFn = bool (*)(const Blackboard&);
using LeafFn = bool (*)(const Blackboard&, Decision&);  // false: fails (the parent selector tries the next)
struct Node { NodeKind kind; const char* name; int first, count; CondFn cond; LeafFn leaf; };

namespace detail {
inline bool React(const Blackboard& b, Impulse i) { return b.impulses && b.impulses->Active(i, b.now, b.person.reactionMs); }
// Conditions
inline bool SelfDanger(const Blackboard& b) { return b.wrecked || b.mood != Mood::Steady || b.health < b.breakOff || (b.civilian && b.haveTarget); }
inline bool AnyImpulse(const Blackboard& b) { for (int i = 0; i < int(Impulse::Count); ++i) if (React(b, Impulse(i))) return true; return false; }
inline bool CanEngage(const Blackboard& b) { return b.haveTarget && b.sinceSeen < 3.0; }
inline bool Lost(const Blackboard& b) { return b.haveTarget && b.sinceSeen >= 3.0 && b.sinceSeen < 60.0; }
inline bool HasLeader(const Blackboard& b) { return b.hasLeader; }
inline bool Always(const Blackboard&) { return true; }
// Leaves of self-preservation
inline bool Flee(const Blackboard& b, Decision& d) { if (!(b.mood == Mood::Panic || b.civilian || b.wrecked)) return false; d.action = Action::Flee; d.why = b.wrecked ? "wrecked" : b.civilian ? "civilian" : "panic"; return true; }
inline bool Retreat(const Blackboard& b, Decision& d) { d.action = Action::Retreat; d.why = b.mood == Mood::Shaken ? "morale low" : "health low"; return true; }
// Leaves of react (impulses)
inline bool ReactRam(const Blackboard& b, Decision& d) { if (!React(b, Impulse::RamIncoming)) return false; d.action = Action::Evade; d.impulse = int(Impulse::RamIncoming); d.why = "ram incoming"; return true; }
inline bool ReactPinned(const Blackboard& b, Decision& d) { if (!React(b, Impulse::Pinned)) return false; d.action = Action::Escape; d.impulse = int(Impulse::Pinned); d.why = "pinned"; return true; }
inline bool ReactHit(const Blackboard& b, Decision& d)
{
    if (!React(b, Impulse::Hit) || !b.bfmOn) return false;
    // The best defensive answer of part B (break, jink, spiral, extend).
    const bfm::M defs[] = { bfm::M::Break, bfm::M::Jink, bfm::M::DefensiveSpiral, bfm::M::Extend };
    double top = 0.3; bool any = false;
    for (const auto m : defs) if (b.bfm[int(m)] > top) { top = b.bfm[int(m)]; d.m = m; any = true; }
    if (!any) { d.m = bfm::M::Jink; top = 0.6; }
    d.action = Action::Bfm; d.impulse = int(Impulse::Hit); d.why = "hit"; d.score = top;
    return true;
}
inline bool ReactLost(const Blackboard& b, Decision& d) { if (!React(b, Impulse::TargetLost)) return false; d.action = Action::SearchLast; d.impulse = int(Impulse::TargetLost); d.why = "target lost"; return true; }
inline bool ReactLeader(const Blackboard& b, Decision& d)
{
    if (!React(b, Impulse::LeaderKilled) && !React(b, Impulse::AllyWrecked) && !React(b, Impulse::AllyDriverKilled)) return false;
    if (b.mood == Mood::Steady) return false;  // a steady driver fights on (the morale hit was taken)
    d.action = Action::Retreat; d.impulse = int(React(b, Impulse::LeaderKilled) ? Impulse::LeaderKilled : Impulse::AllyWrecked); d.why = "lost an ally";
    return true;
}
inline bool ReactPlayer(const Blackboard& b, Decision& d) { if (!React(b, Impulse::PlayerOnFoot) || !b.civilian) return false; d.action = Action::Flee; d.impulse = int(Impulse::PlayerOnFoot); d.why = "player near"; return true; }
// The engage branch: the squad's order first, else the best of interpose / part D tactics / part B manoeuvres.
inline bool Engage(const Blackboard& b, Decision& d)
{
    if (b.squadsOn) {
        switch (b.role) {
        case Role::Suppressor: d.action = Action::Suppress; d.why = "squad: suppress"; return true;
        case Role::Overwatch: d.action = Action::Overwatch; d.why = "squad: overwatch"; return true;
        case Role::Bait: d.action = Action::Bait; d.why = "squad: bait"; return true;
        case Role::Reserve: d.action = Action::Reserve; d.why = "squad: reserve"; return true;
        case Role::Cover: d.action = Action::Cover; d.why = "squad: cover a damaged ally"; return true;
        case Role::Retreat: d.action = Action::Retreat; d.why = "squad: retreat together"; return true;
        case Role::BoxSlot: if (b.tacticsOn) { d.action = Action::Tactic; d.t = tactics::T::BoxIn; d.why = "squad: box in"; return true; } break;
        case Role::ConvoyGuard: if (b.tacticsOn) { d.action = Action::Tactic; d.t = tactics::T::ConvoyDefend; d.why = "squad: convoy defence"; return true; } break;
        case Role::Flanker: if (b.bfmOn) { d.action = Action::Bfm; d.m = bfm::M::LagPursuit; d.why = "squad: flank to its tail"; return true; } break;
        default: break;
        }
    }
    const double cur = d.score;  // (unused: the hysteresis is applied in Tick against the previous decision)
    (void)cur;
    double top = -1;
    if (b.protect) { top = 0.6 + 0.4 * b.protectThreat; d.action = Action::Interpose; d.why = "protect"; }  // a hard-closing threat beats attacking
    if (b.tacticsOn) for (int i = 0; i < int(tactics::T::Count); ++i) if (b.tac[i] > top) { top = b.tac[i]; d.action = Action::Tactic; d.t = tactics::T(i); d.why = "tactic fits"; }
    if (b.bfmOn) for (int i = 0; i < int(bfm::M::Count); ++i) if (b.bfm[i] > top) { top = b.bfm[i]; d.action = Action::Bfm; d.m = bfm::M(i); d.why = "manoeuvre fits"; }
    if (top < 0) { d.action = Action::Bfm; d.m = bfm::M::LeadPursuit; d.why = "default"; top = 0; }
    d.score = top;
    return true;
}
inline bool SearchLast(const Blackboard& b, Decision& d) { if (b.reachedLast) return false; d.action = Action::SearchLast; d.why = "going to the last known position"; return true; }
inline bool SearchPattern(const Blackboard&, Decision& d) { d.action = Action::SearchPattern; d.why = "searching around it"; return true; }
inline bool Follow(const Blackboard&, Decision& d) { d.action = Action::FollowLeader; d.why = "leader"; return true; }
inline bool Wander(const Blackboard&, Decision& d) { d.action = Action::Wander; d.why = "nothing to do"; return true; }
}

// The tree: root selector -> self-preservation, react, engage, search, follow, idle (in that priority).
inline const Node kTree[] = {
    /* 0 */ { NodeKind::Selector, "root", 1, 6, nullptr, nullptr },
    /* 1 */ { NodeKind::Sequence, "self-preservation", 7, 2, nullptr, nullptr },
    /* 2 */ { NodeKind::Sequence, "react", 9, 2, nullptr, nullptr },
    /* 3 */ { NodeKind::Sequence, "engage", 11, 2, nullptr, nullptr },
    /* 4 */ { NodeKind::Sequence, "search", 13, 2, nullptr, nullptr },
    /* 5 */ { NodeKind::Sequence, "follow", 15, 2, nullptr, nullptr },
    /* 6 */ { NodeKind::Leaf, "idle", 0, 0, nullptr, &detail::Wander },
    /* 7 */ { NodeKind::Condition, "in danger", 0, 0, &detail::SelfDanger, nullptr },
    /* 8 */ { NodeKind::Selector, "flee or retreat", 17, 2, nullptr, nullptr },
    /* 9 */ { NodeKind::Condition, "an impulse", 0, 0, &detail::AnyImpulse, nullptr },
    /*10 */ { NodeKind::Selector, "impulse", 19, 6, nullptr, nullptr },
    /*11 */ { NodeKind::Condition, "target in sight", 0, 0, &detail::CanEngage, nullptr },
    /*12 */ { NodeKind::DesireSelector, "fight", 0, 0, nullptr, &detail::Engage },
    /*13 */ { NodeKind::Condition, "target lost", 0, 0, &detail::Lost, nullptr },
    /*14 */ { NodeKind::Selector, "search", 25, 2, nullptr, nullptr },
    /*15 */ { NodeKind::Condition, "has a leader", 0, 0, &detail::HasLeader, nullptr },
    /*16 */ { NodeKind::Leaf, "follow leader", 0, 0, nullptr, &detail::Follow },
    /*17 */ { NodeKind::Leaf, "flee", 0, 0, nullptr, &detail::Flee },
    /*18 */ { NodeKind::Leaf, "retreat", 0, 0, nullptr, &detail::Retreat },
    /*19 */ { NodeKind::Leaf, "evade ram", 0, 0, nullptr, &detail::ReactRam },
    /*20 */ { NodeKind::Leaf, "escape pin", 0, 0, nullptr, &detail::ReactPinned },
    /*21 */ { NodeKind::Leaf, "answer a hit", 0, 0, nullptr, &detail::ReactHit },
    /*22 */ { NodeKind::Leaf, "lost an ally", 0, 0, nullptr, &detail::ReactLeader },
    /*23 */ { NodeKind::Leaf, "target lost", 0, 0, nullptr, &detail::ReactLost },
    /*24 */ { NodeKind::Leaf, "player near", 0, 0, nullptr, &detail::ReactPlayer },
    /*25 */ { NodeKind::Leaf, "last known position", 0, 0, nullptr, &detail::SearchLast },
    /*26 */ { NodeKind::Leaf, "search pattern", 0, 0, nullptr, &detail::SearchPattern },
};
inline bool Run(int node, const Blackboard& b, Decision& d)
{
    const Node& n = kTree[node];
    switch (n.kind) {
    case NodeKind::Condition: return n.cond(b);
    case NodeKind::Leaf:
    case NodeKind::DesireSelector: { Decision t = d; if (!n.leaf(b, t)) return false; if (!t.branch[0]) t.branch = n.name; d = t; return true; }
    case NodeKind::Selector:
        for (int i = 0; i < n.count; ++i) if (Run(n.first + i, b, d)) { if (node && !d.branch[0]) d.branch = n.name; return true; }
        return false;
    case NodeKind::Sequence:
        for (int i = 0; i < n.count; ++i) if (!Run(n.first + i, b, d)) return false;
        d.branch = n.name;
        return true;
    }
    return false;
}
// One decision. The previous one keeps running when the new one scores less than kHysteresis better (the engage branch only; impulses and
// self-preservation always win).
inline Decision Tick(const Blackboard& b, const Decision& previous, bool havePrevious)
{
    Decision d;
    Run(0, b, d);
    if (havePrevious && std::string_view(d.branch) == "engage" && std::string_view(previous.branch) == "engage" && !d.Same(previous)) {
        double prevScore = -1;
        if (previous.action == Action::Bfm && b.bfmOn) prevScore = b.bfm[int(previous.m)];
        else if (previous.action == Action::Tactic && b.tacticsOn) prevScore = b.tac[int(previous.t)];
        if (prevScore >= 0 && prevScore + kHysteresis >= d.score) { Decision keep = previous; keep.score = prevScore; keep.why = "held (hysteresis)"; return keep; }
        if (prevScore > 0 && b.now >= previous.since && b.now - previous.since < kMinHoldMs) { Decision keep = previous; keep.score = prevScore; keep.why = "held (minimum time)"; return keep; }
    }
    if (!havePrevious || !d.Same(previous) || std::string_view(d.branch) != previous.branch) d.since = b.now;
    else d.since = previous.since;
    if (d.impulse >= 0 && b.impulses) b.impulses->Handle(Impulse(d.impulse), b.now);
    return d;
}
}
