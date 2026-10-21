#pragma once
// The player's overrides for their own vehicle (2026-10-20, the user; notes/VEHICLE_HANDOFF_20261020.md "Approved design decisions"). They sit above the
// vehicle's Navigation/Combat/Weapons behaviour and set it temporarily:
//  N Summon: drive to / pull up beside the player to board it; no combat, hold fire. Pressing N again re-issues it, never toggles it off.
//  U Follow (out of combat) / Cover (in combat): follow the player and defend them; enemies around the player are shot at, never chased (no attack run, pursuit or combat repositioning).
//  T hold on a place (on foot): Guard the aimed point; the point beats a fight, defend from there.
namespace overrides {
enum class Mode : int { None = 0, Recall = 1, Cover = 2, Guard = 3 };
struct Rules {
    bool pickFoe = true;    // look for an enemy at all
    bool chase = true;      // pursue / attack runs / move to a combat position
    bool fire = true;       // vehicle weapon
    bool followPlayer = false, holdPoint = false;
};
inline Rules For(Mode m) {
    switch (m) {
    case Mode::Recall: return {false, false, false, true, false};
    case Mode::Cover: return {true, false, true, true, false};
    case Mode::Guard: return {true, false, true, false, true};
    default: return {};
    }
}
inline Mode OnSummon(Mode) { return Mode::Recall; }
inline Mode OnCover(Mode m) { return m == Mode::Cover ? Mode::None : Mode::Cover; }  // guarding: U resumes covering the player
// The player's words are only Summon, Follow and Cover (2026-10-21): N is always Summon; U is Follow out of combat and Cover in combat
// (the same press, the same mode: only its name follows the player's combat state).
inline const char* SummonLabel() { return "Summon"; }
inline const char* CoverLabel(bool inCombat) { return inCombat ? "Cover" : "Follow"; }
inline const char* CoverStatus(bool inCombat) { return inCombat ? "COVER" : "FOLLOW"; }
}
