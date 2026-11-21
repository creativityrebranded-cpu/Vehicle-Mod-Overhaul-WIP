#pragma once
// Route item warnings (card RI-2, notes/route-items-20261107/ROUTE_ITEM_WARNINGS.html; user 2026-11-07: "warnings specific to an item").
// F7 READER: one row per object or ground feature inside a route's field, saying what it is, where it lies and what it will do to a
// driving car and to a birth there. It decides nothing for any car or spawn: the words come from the driving rules themselves
// (worldclass::rules, planbranch::rules, planworld::kLowTop), so a changed rule changes the warning; no number here is a second copy.
// Pure; tests: tests/routeitems_test.cpp.
#include "planrules.h"
#include "planworld.h"
#include "worldclass.h"
#include <cstdint>
#include <cstdio>
#include <string>

namespace routeitems {
enum class Kind : std::uint8_t { Object, TerrainStep, NoGround, Vehicle, Person, Doubt, Unchecked, SpawnSpot, LaneCut };  // LaneCut: LB-1 (2026-11-12)
inline const char* Name(Kind k) { constexpr const char* n[]{ "object", "terrain_step", "no_ground", "vehicle", "person", "doubt", "unchecked", "spawn_spot", "lane_cut" }; return n[int(k)]; }
enum class Severity : std::uint8_t { Fine, Info, Attention, Clearance };   // Fine: "not a problem"; colours: dim, dim, amber, red
inline const char* Name(Severity s) { constexpr const char* n[]{ "fine", "info", "attention", "clearance" }; return n[int(s)]; }

struct Item {
    Kind kind = Kind::Object;
    worldclass::Class cls = worldclass::Class::Unknown;
    std::uint32_t ref = 0, base = 0;          // 0: none (landscape)
    std::string name;                          // the game's name when it has one, else empty
    double x = 0, y = 0, top = 0;              // world position, top of the object (m)
    double size[3] = {};                       // base bounds (m)
    bool haveFloor = false; double floor = 0;  // measured floor under it
    double rise = 0;                           // TerrainStep: the step height; Object: top - floor
    int tile = -1, lane = -1; double along = 0, across = 0;  // across: + right of travel
    bool confirmed = false;                    // a ray hit this reference (it has collision)
    bool solidsAllowed = false;                // the tile's Solids = Allow (D14)
    bool roomToPass = false, roomKnown = false;// the sweep found a clear line beside it
    bool birthTile = false;                    // births can be placed on this tile and lane
    int spotFails = 0, spotModels = 0;         // SpawnSpot: Health's record
};
struct Warning { Severity sev = Severity::Info; std::string title, effect, spawn, fix; const char* reason = ""; };

inline std::string Num(double v, int d = 1) { char b[32]; std::snprintf(b, sizeof b, "%.*f", d, v); return b; }
inline std::string KindWord(const Item& it) {
    if (!it.name.empty()) return it.name;
    switch (it.cls) {
    case worldclass::Class::Loose: return "Loose object"; case worldclass::Class::Plant: return "Plant"; case worldclass::Class::Barrier: return "Barrier";
    case worldclass::Class::Structure: return "Structure"; case worldclass::Class::Road: return "Road piece"; case worldclass::Class::Terrain: return "Terrain";
    case worldclass::Class::Vehicle: return "Vehicle"; case worldclass::Class::Person: return "Person"; default: return "Object";
    }
}
inline double Largest(const Item& it) { return std::max({ it.size[0], it.size[1], it.size[2] }); }

// What it will do. Order follows the stop rule (WORKING_MANDATE.md, world truth): vehicles and people live; small loose things driven
// over or climbed; barriers and unexplained loose/plant/unknown may stop; structures need the body check; terrain/road never stop by a ray.
inline Warning Judge(const Item& it) {
    namespace wr = worldclass::rules; namespace pr = planbranch::rules;
    Warning w;
    const std::string where = it.tile >= 0 ? "tile " + std::to_string(it.tile + 1) : std::string();
    auto spawnInside = [&](const char* what) { if (it.birthTile) w.spawn = std::string("A car born on ") + where + " " + what + ": Siting trusts the field, so the spot test or the arrival check fails it."; };
    switch (it.kind) {
    case Kind::Unchecked:
        w.sev = Severity::Info; w.title = "Not checked"; w.effect = "Outside the loaded cells: the game has no physics there to ask."; w.fix = "Move closer (about 250 m) and it fills in."; w.reason = "outside_loaded_area"; return w;
    case Kind::Doubt:
        w.sev = Severity::Info; w.title = "Unconfirmed low edge"; w.effect = "Doubt: cars slow to " + Num(pr::creepSpeed, 0) + " m/s until a second ray confirms it. Never a stop."; w.reason = "doubt_creep_never_stop"; return w;
    case Kind::NoGround:
        w.sev = Severity::Clearance; w.title = "No ground"; w.effect = "Nothing under the floor: a car on wheels falls through."; w.fix = "Set the tile to Hover, or Snap road to deck.";
        if (it.birthTile) w.spawn = "A birth here drops and is lost.";
        w.reason = "no_surface_under_floor"; return w;
    case Kind::LaneCut:  // LB-1 (2026-11-12): a lane wider than this corner can hold would fold back; the builder cut it straight
        w.sev = Severity::Attention; w.title = "Lane cut at a tight corner, " + Num(it.rise, 0) + " deg fold";
        w.effect = "Lane " + std::to_string(it.lane + 1) + " is wider than this corner can hold, so it would fold back on itself. It is cut straight here and cars take the inside line.";
        w.fix = "Lower the route width or the corner rounding here, or move the points out to widen the turn."; w.reason = "lane_folds_at_corner"; return w;
    case Kind::SpawnSpot:
        w.sev = Severity::Attention; w.title = "Spawn spot Health rejected";
        w.effect = std::to_string(it.spotFails) + " births failed here, from " + std::to_string(it.spotModels) + " models."; w.spawn = "Health skips this spot for births.";
        w.fix = "Look for a slope or clutter under it."; w.reason = "health_spot_rejected"; return w;
    case Kind::Vehicle:
        w.sev = Severity::Clearance; w.title = "Vehicle in the field"; w.effect = "Vehicles are always live: cars stop behind it, then pass round if there is room.";
        if (it.birthTile) w.spawn = "Siting reads other vehicles, so births skip this spot.";
        w.fix = "Nothing to fix in the route."; w.reason = "vehicle_always_live"; return w;
    case Kind::Person:
        w.sev = Severity::Attention; w.title = "Person in the field"; w.effect = "People are always live: cars stop for them."; w.reason = "person_always_live"; return w;
    case Kind::TerrainStep: {
        w.title = "Terrain step, " + Num(it.rise) + " m";
        if (it.rise <= wr::driveOverLow) { w.sev = Severity::Fine; w.effect = "Under a quarter tire: not even slowed for."; w.reason = "under_quarter_tire"; }
        else if (it.rise <= pr::climbHeight) { w.sev = Severity::Attention; w.effect = "Under half a tire: cars climb it at " + Num(pr::climbSpeed) + " m/s."; w.reason = "climb"; }
        else if (it.rise <= pr::lipStep) { w.sev = Severity::Attention; w.effect = "A step with a floor on top is road: cars drive up it slowly."; w.reason = "lip_is_road"; }
        else if (it.rise < planworld::kLowTop) { w.sev = Severity::Attention; w.effect = "Over the " + Num(pr::lipStep, 2) + " m lip step: cars read it as a face and hop it."; w.reason = "hop"; spawnInside("lands on its edge"); }
        else { w.sev = Severity::Clearance; w.effect = "A wall to cars: too high to hop. They go round if there is room, else stop."; w.reason = "wall"; spawnInside("is against it"); }
        if (w.sev != Severity::Fine) w.fix = "Move the lane off it, or raise the points here to the upper level.";
        return w; }
    case Kind::Object: break;
    }
    w.title = KindWord(it) + (Largest(it) > 0 ? ", " + Num(Largest(it)) + " m" : std::string());
    if (!it.confirmed) { w.sev = Severity::Fine; w.effect = "No collision found on it: cars pass through."; w.reason = "no_collision_hit"; return w; }
    if (it.cls == worldclass::Class::Road || (it.haveFloor && it.rise <= wr::driveOverLow && it.cls != worldclass::Class::Barrier)) {
        w.sev = Severity::Fine; w.effect = it.cls == worldclass::Class::Road ? "Road: the floor cars drive on." : "Under a quarter tire: not even slowed for."; w.reason = it.cls == worldclass::Class::Road ? "road" : "under_quarter_tire"; return w; }
    if (!it.haveFloor) { w.sev = Severity::Attention; w.effect = "Floor not measured here yet: its height over the road is unknown."; w.fix = "Look at this tile so the floor is measured."; w.reason = "no_floor_measured"; return w; }
    const bool smallClass = it.cls == worldclass::Class::Loose || it.cls == worldclass::Class::Plant || it.cls == worldclass::Class::Unknown;
    if (smallClass && it.rise <= pr::climbHeight) { w.sev = Severity::Attention; w.effect = "Under half a tire: cars climb it at " + Num(pr::climbSpeed) + " m/s."; w.reason = "climb"; spawnInside("lands on it"); w.fix = "Fine to keep."; return w; }
    if (it.cls == worldclass::Class::Structure && it.solidsAllowed) { w.sev = Severity::Attention; w.effect = "Solids allowed on this tile: cars drive through it by design."; w.reason = "solids_allowed_d14"; spawnInside("starts inside it"); return w; }
    const std::string room = !it.roomKnown ? " Room beside it not swept yet." : it.roomToPass ? " The sweep found room beside it: cars steer round at creep speed and rejoin." : " No room beside it: cars stop and back off.";
    w.sev = Severity::Clearance;
    if (it.cls == worldclass::Class::Barrier) { w.effect = "Barriers may stop a car on one ray." + room; w.fix = "Move the lane off it. Allow solids does not cover barriers."; w.reason = "barrier_may_stop"; }
    else if (it.cls == worldclass::Class::Structure) { w.effect = "Blocks the lane." + room; w.fix = "Move the lane off it, or set Solids to Allow if the route goes through it on purpose."; w.reason = "structure_blocks"; }
    else { w.effect = "Over half a tire: it may stop cars until the body check clears it." + room; w.fix = "Move the lane off it."; w.reason = "may_stop_over_half_tire"; }
    spawnInside("starts inside it");
    return w;
}
}
