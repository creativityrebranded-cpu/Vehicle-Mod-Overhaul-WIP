#pragma once
#include "combat_brain.h"
// FACT form IDs read from the installed Starfield.esm, 2026-10-21.
// Starfield.esm is master index 00. Runtime membership still uses IsInFaction;
// editor IDs being stripped from the executable cannot disable these defaults.
// Full record evidence: notes/native-integration-20261021/native-factions.json.
namespace nativepersonality {
struct Faction {std::uint32_t id;brain::Temper temper;};
inline constexpr Faction factions[]{
    {0x0004CE24,brain::Temper::Reckless}, // GroupFactionCrimsonFleet
    {0x0004CE25,brain::Temper::Reckless}, // GroupFactionEcliptic
    {0x0004CE26,brain::Temper::Reckless}, // GroupFactionSpacers
    {0x0027028D,brain::Temper::Reckless}, // EclipticFaction
    {0x0027BB8C,brain::Temper::Reckless}, // SpacerFaction
    {0x00052ADC,brain::Temper::Disciplined}, // UCSecurityFaction
    {0x00096529,brain::Temper::Disciplined}, // FreestarSecurityFaction
    {0x000D1850,brain::Temper::Disciplined}, // UCVanguardFaction
    {0x002733AF,brain::Temper::Disciplined}, // UCSysDefFaction
    {0x0029A8EF,brain::Temper::Disciplined}, // FreestarRangerFaction
    {0x0011F9ED,brain::Temper::Civilian}, // SQ_Group_OccupationFaction_Colonist
    {0x001A2C9C,brain::Temper::Civilian}, // LISTColonistFaction
    {0x0020A931,brain::Temper::Civilian}, // REFaction_NPCCivilian
    {0x00273F95,brain::Temper::Civilian}, // ECSConstantColonistFaction
    {0x002C5F7E,brain::Temper::Civilian}, // SettlerFaction
};
}
