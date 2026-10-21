#pragma once
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
// The rules around the three profile classes (vcontract::Class; 2026-10-20, the user, notes/VEHICLE_HANDOFF_20261020.md "Approved design decisions").
namespace profileclass {
inline bool Is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

// The combat profile a fight starts with, from the vehicle's navigation profile: escort / scout / cruise / hauler / courier / return home defend,
// patrol / hold attack. No navigation profile: an NPC driver's default is escort, so defend.
inline const char* DefaultCombat(const char* nav) { return Is(nav, "patrol") || Is(nav, "hold") ? "attack" : "defend"; }

enum class Weapons { Free, Opportunistic, HoldFire, Never };
inline Weapons WeaponsFor(const char* name) {
    return Is(name, "opportunistic") ? Weapons::Opportunistic : Is(name, "hold fire") ? Weapons::HoldFire : Is(name, "never") ? Weapons::Never : Weapons::Free;
}
// The one weapons gate. The player's seat never fires by itself: the player shoots from it. Opportunistic shoots only what is already in front of the
// nose: it never turns the vehicle or makes an attack run for a shot.
inline bool MayFire(Weapons w, bool playerSeat) { return !playerSeat && (w == Weapons::Free || w == Weapons::Opportunistic); }
inline bool MayAim(Weapons w, bool playerSeat) { return !playerSeat && w == Weapons::Free; }
// The player's overrides set the weapons for their duration (overrides.h): Recall holds fire, Cover shoots opportunistically; never is never relaxed.
inline Weapons Effective(Weapons w, bool recall, bool cover) {
    if (w == Weapons::Never || recall) return recall && w != Weapons::Never ? Weapons::HoldFire : w;
    if (cover && w == Weapons::Free) return Weapons::Opportunistic;
    return w;
}

inline std::string Upper(const char* s) {
    std::string out = s ? s : "";
    for (auto& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
// The follower row / status text: an active override shows alone ("SUMMON", "FOLLOW" / "COVER", "GUARD POINT", "RAM READY", "RAMMING"); otherwise
// "NAV · COMBAT" with the combat profile only during a fight, and the weapons only when they restrict ("HOLD FIRE").
inline std::string State(const char* override, const char* nav, const char* combat, bool fighting, Weapons w) {
    if (override && *override) return override;
    std::string s = Upper(nav && *nav ? nav : "escort");
    if (fighting && combat && *combat) s += " \xC2\xB7 " + Upper(combat);
    if (w == Weapons::HoldFire || w == Weapons::Never) s += " \xC2\xB7 HOLD FIRE";
    return s;
}
inline bool ShowsDistance(const char* override) { return !(Is(override, "RAM READY") || Is(override, "RAMMING")); }
}
