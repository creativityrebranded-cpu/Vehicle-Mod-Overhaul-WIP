#include "combat_brain.h"
#include <cstdio>
#include <cstring>
using namespace brain;
using steer::V3;
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++failures; };
    // ---- Personality and difficulty ----
    {
        const Personality pirate = PersonalityFor(Temper::Reckless, 7, 2), uc = PersonalityFor(Temper::Disciplined, 7, 2), civ = PersonalityFor(Temper::Civilian, 7, 2);
        check(pirate.aggression > uc.aggression && uc.aggression > civ.aggression, "pirates more aggressive than UC, UC more than civilians");
        check(uc.accuracy > pirate.accuracy && civ.courage < 0.3, "UC more accurate; civilians have little courage");
        const Personality easy = PersonalityFor(Temper::Default, 7, 0), hard = PersonalityFor(Temper::Default, 7, 4);
        check(hard.reactionMs < easy.reactionMs && hard.accuracy > easy.accuracy, "harder difficulty: faster reactions, better aim");
        check(PersonalityFor(Temper::Default, 1, 2).aggression != PersonalityFor(Temper::Default, 2, 2).aggression, "each driver differs a little");
        steer::Rng r(3); Personality sharp; sharp.accuracy = 1.0;
        check(steer::Len(AimError(sharp, 100, r)) < 1e-9 && steer::Len(AimError(civ, 100, r)) > 0, "aim error from accuracy");
    }
    // ---- Impulses ----
    {
        Impulses im; im.Raise(Impulse::Hit, 1000);
        check(!im.Active(Impulse::Hit, 1200, 400) && im.Active(Impulse::Hit, 1500, 400), "an impulse acts after the reaction time");
        im.Handle(Impulse::Hit, 1500);
        check(!im.Active(Impulse::Hit, 1600, 400), "a handled impulse does not repeat");
        im.Raise(Impulse::Hit, 2000);
        check(!im.Active(Impulse::Hit, 6000, 400), "an old impulse expires");
    }
    // ---- Memory, hearing, search ----
    {
        Memory a, b;
        a.Sense(5, { 10, 0, 0 }, { 1, 0, 0 }, true, false, 0.8, 1000);
        b.Sense(5, { 50, 0, 0 }, { 2, 0, 0 }, true, false, 0.7, 2000);
        a.Merge(b);
        check(a.Find(5)->pos.x == 50 && a.Find(5)->seenAt == 2000, "squad memory: the newer sighting wins");
        a.Sense(5, {}, {}, false, false, 0, 3000);
        check(!a.Find(5)->visible && a.Find(5)->pos.x == 50, "out of sight: the last known position stays");
        a.Sense(9, { 0, 100, 0 }, {}, false, true, 1, 3000);
        check(a.Find(9) && a.Find(9)->heardAt == 3000 && !a.Find(9)->seenAt, "a heard enemy is remembered");
        check(Hears(150, true, 0) && !Hears(250, true, 0) && Hears(50, false, 15) && !Hears(50, false, 2), "hearing: gunfire 200 m, engines 60 m");
        Search s; Track k; k.pos = { 0, 0, 0 }; k.vel = { 5, 0, 0 }; StartSearch(s, k);
        check(s.next.x == 10, "search starts at the last known position pushed on by its velocity");
        V3 p = SearchStep(s, { 10, 0, 0 }); const V3 p2 = SearchStep(s, p);
        const V3 p3 = SearchStep(s, p2);
        check(steer::Len(p - V3{ 10, 0, 0 }) == 30 && steer::Len(p2 - p) == 30 && steer::Len(p3 - p2) == 60, "then an expanding square (legs 30, 30, 60, ...)");
    }
    // ---- Morale ----
    {
        Morale m; m.Init(0.5); const double start = m.value;
        m.Apply(MoraleEvent::AllyWrecked);
        check(m.value < start, "an ally wrecked lowers morale");
        Morale brave; brave.Init(1.0); const double bs = brave.value; brave.Apply(MoraleEvent::AllyWrecked);
        Morale coward; coward.Init(0.0); const double cs = coward.value; coward.Apply(MoraleEvent::AllyWrecked);
        check(bs - brave.value < cs - coward.value, "the brave lose less");
        Morale low; low.value = 0.3; check(low.Of(false, false, 1.0) == Mood::Shaken, "low morale: shaken (retreat)");
        low.value = 0.1; check(low.Of(false, false, 1.0) == Mood::Panic, "very low: panic (flee)");
        Morale ok; ok.value = 0.7; check(ok.Of(true, true, 1.0) == Mood::Panic, "alone with the leader dead: panic");
        ok.Apply(MoraleEvent::Reinforcements); check(ok.value > 0.7, "reinforcements rally");
        Morale out; out.Init(0.5); const double o0 = out.value; out.Tick(10.0, 3.0, 0.0);
        check(out.value < o0, "outnumbered drains morale");
    }
    // ---- Squad brain ----
    {
        Squad s; s.count = 2; s.m[0].id = 1; s.m[0].pos = { 0, 0, 0 }; s.m[1].id = 2; s.m[1].pos = { 50, 0, 0 };
        Enemy e[2]; e[0].id = 10; e[0].pos = { 0, 100, 0 }; e[0].health = 0.9; e[1].id = 11; e[1].pos = { 30, 100, 0 }; e[1].health = 0.4;
        check(Assign(s, e, 2, 1000) && s.tactic == SquadTactic::Pincer && s.focus == 11, "two on two: a pincer on the weakest enemy (focus fire)");
        check((s.m[0].role == Role::Engager) != (s.m[1].role == Role::Engager), "one engager, one flanker");
        check(!Assign(s, e, 2, 1500), "roles are re-assigned only every 1.5 s");
        Squad t; t.count = 3; for (int i = 0; i < 3; ++i) { t.m[i].id = 20 + i; t.m[i].pos = { i * 30.0, 0, 0 }; }
        Enemy slow; slow.id = 30; slow.pos = { 30, 150, 0 };
        Assign(t, &slow, 1, 1000);
        check(t.tactic == SquadTactic::SuppressAndFlank, "three on a slow target: suppress and flank");
        bool sup = false; for (int i = 0; i < 3; ++i) sup = sup || t.m[i].role == Role::Suppressor;
        check(sup, "one suppresses");
        Enemy fast = slow; fast.vel = { 12, 0, 0 };
        Assign(t, &fast, 1, 3000);
        check(t.tactic == SquadTactic::BoxIn && t.m[0].role == Role::BoxSlot, "three on a fast target: box in");
        Enemy chaser; chaser.id = 31; chaser.pos = { 0, 80, 0 }; chaser.vel = { 0, -15, 0 };
        Assign(t, &chaser, 1, 5000);
        check(t.tactic == SquadTactic::BaitAndAmbush && t.m[0].role == Role::Bait, "the enemy chases one: bait and ambush");
        Squad u = t; u.m[1].health = 0.2; Enemy near = slow; near.pos = { 30, 40, 0 };
        Assign(u, &near, 1, 9000);
        bool cover = false; for (int i = 0; i < 3; ++i) cover = cover || (u.m[i].role == Role::Cover && u.m[i].coverFor == u.m[1].id);
        check(cover && u.tactic == SquadTactic::CoverAlly, "a damaged ally under threat gets cover");
        Squad w = t; for (int i = 0; i < 3; ++i) w.m[i].morale = 0.2;
        Assign(w, &slow, 1, 12000);
        check(w.tactic == SquadTactic::RetreatTogether && w.m[2].role == Role::Retreat, "low squad morale: retreat together");
        Squad c = t; c.convoy = true; c.m[0].morale = c.m[1].morale = c.m[2].morale = 0.6;
        Assign(c, &slow, 1, 15000);
        check(c.tactic == SquadTactic::ConvoyDefence && c.m[2].role == Role::ConvoyGuard, "a convoy squad keeps convoy defence");
    }
    // ---- The tree ----
    {
        Impulses im;
        Blackboard b; b.now = 10000; b.impulses = &im; b.haveTarget = true; b.targetVisible = true;
        b.bfm[int(bfm::M::LeadPursuit)] = 0.6; b.tac[int(tactics::T::CutOff)] = 0.7;
        Decision d = Tick(b, {}, false);
        check(d.action == Action::Tactic && d.t == tactics::T::CutOff && std::strcmp(d.branch, "engage") == 0, "engage: the best scored leaf (a tactic)");
        b.bfm[int(bfm::M::PurePursuit)] = 0.78;
        const Decision held = Tick(b, d, true);
        check(held.Same(d) && std::strcmp(held.why, "held (hysteresis)") == 0, "a slightly better leaf does not replace the running one (hysteresis)");
        b.bfm[int(bfm::M::PurePursuit)] = 0.95;
        check(Tick(b, d, true).m == bfm::M::PurePursuit, "a clearly better one does");
        b.tacticsOn = false; b.tac[int(tactics::T::CutOff)] = 0.99;
        check(Tick(b, {}, false).action == Action::Bfm, "tactics switched off: no tactic leaves");
        b.tacticsOn = true;
        im.Raise(Impulse::Hit, 9000); b.bfm[int(bfm::M::Break)] = 0.9;
        d = Tick(b, d, true);
        check(d.action == Action::Bfm && d.m == bfm::M::Break && d.impulse == int(Impulse::Hit) && std::strcmp(d.branch, "react") == 0, "a hit interrupts: the react branch answers with a break");
        check(Tick(b, d, true).impulse < 0, "the impulse is handled once");
        im.Raise(Impulse::RamIncoming, 9800); b.now = 10500;
        check(Tick(b, d, true).action == Action::Evade, "a ram incoming: evade");
        b.health = 0.2;
        check(Tick(b, d, true).action == Action::Retreat && std::strcmp(Tick(b, d, true).branch, "self-preservation") == 0, "health low: self-preservation wins over everything");
        b.health = 1.0; b.mood = Mood::Panic;
        check(Tick(b, d, true).action == Action::Flee, "panic: flee");
        b.mood = Mood::Steady; b.civilian = true;
        check(Tick(b, d, true).action == Action::Flee, "a civilian flees a fight");
        b.civilian = false; b.wrecked = true;
        check(Tick(b, d, true).action == Action::Flee, "wrecked: out");
        b.wrecked = false;
        b.role = Role::Suppressor;
        check(Tick(b, {}, false).action == Action::Suppress, "the squad's role comes first in the fight");
        b.squadsOn = false;
        check(Tick(b, {}, false).action != Action::Suppress, "squads off: no roles");
        b.squadsOn = true; b.role = Role::Engager;
        b.protect = true; b.protectThreat = 1.0; b.tac[int(tactics::T::CutOff)] = 0.7;
        check(Tick(b, {}, false).action == Action::Interpose, "an escort with a threat closing on the player: interpose");
        b.protect = false;
        b.sinceSeen = 10; b.targetVisible = false;
        d = Tick(b, {}, false);
        check(d.action == Action::SearchLast && std::strcmp(d.branch, "search") == 0, "target lost: go to its last known position");
        b.reachedLast = true;
        check(Tick(b, {}, false).action == Action::SearchPattern, "then the search pattern");
        b.haveTarget = false; b.hasLeader = true;
        check(Tick(b, {}, false).action == Action::FollowLeader, "no target, a leader: follow it");
        b.hasLeader = false;
        check(Tick(b, {}, false).action == Action::Wander, "nothing at all: wander");
    }
    return failures ? 1 : 0;
}
