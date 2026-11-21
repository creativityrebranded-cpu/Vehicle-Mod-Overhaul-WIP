// routeitems.h (card RI-2): each example row of notes/route-items-20261107/ROUTE_ITEM_WARNINGS.html gets the warning the page shows.
#include "routeitems.h"
#include <cstdio>
#include <cstring>

static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) ++g_fail; }
static routeitems::Item Obj(worldclass::Class c, double rise, double size) {
    routeitems::Item it; it.kind = routeitems::Kind::Object; it.cls = c; it.confirmed = true; it.haveFloor = true; it.floor = 0; it.rise = rise; it.top = rise;
    it.size[0] = size; it.tile = 5; it.lane = 0; return it;
}
int main() {
    using namespace routeitems; using C = worldclass::Class;
    { auto it = Obj(C::Loose, .30, 4.1); it.birthTile = true; const auto w = Judge(it);
      Check(w.sev == Severity::Attention && !std::strcmp(w.reason, "climb"), "log 0.30 m: climbed (under the one car's 0.35 m climb, carcap D2; was a 0.48 m log under 0.5)"); Check(!w.spawn.empty(), "log on a birth tile: spawn warning"); }
    { auto it = Obj(C::Structure, 1.6, 5.2); it.roomKnown = true; it.roomToPass = true; it.birthTile = true; const auto w = Judge(it);
      Check(w.sev == Severity::Clearance && !std::strcmp(w.reason, "structure_blocks"), "rock 1.6 m: blocks"); Check(w.effect.find("steer round") != std::string::npos, "rock: room beside it named"); }
    { auto it = Obj(C::Structure, 1.6, 5.2); it.solidsAllowed = true; Check(!std::strcmp(Judge(it).reason, "solids_allowed_d14"), "rock with Solids allowed: drives through"); }
    { auto it = Obj(C::Barrier, 2, 8); it.solidsAllowed = true; Check(!std::strcmp(Judge(it).reason, "barrier_may_stop"), "barrier: Allow solids does not cover it"); }
    { auto it = Obj(C::Loose, .48, 4.1); Check(!std::strcmp(Judge(it).reason, "may_stop_over_half_tire"), "log 0.48 m: over the one car's 0.35 m climb (carcap D2): may stop (was climbed under 0.5)"); }
    { auto it = Obj(C::Plant, .6, 1.2); Check(!std::strcmp(Judge(it).reason, "may_stop_over_half_tire"), "bush 0.6 m: may stop"); }
    { auto it = Obj(C::Loose, .15, .5); Check(Judge(it).sev == Severity::Fine, "curb 0.15 m (under a quarter of the one tyre): fine"); }
    { auto it = Obj(C::Road, 1.0, 20); Check(Judge(it).sev == Severity::Fine, "road piece: fine"); }
    { auto it = Obj(C::Loose, 1.0, 1); it.confirmed = false; Check(!std::strcmp(Judge(it).reason, "no_collision_hit"), "no ray hit on it: fine"); }
    { auto it = Obj(C::Loose, 1.0, 1); it.haveFloor = false; Check(!std::strcmp(Judge(it).reason, "no_floor_measured"), "no floor: height unknown"); }
    { Item it; it.kind = Kind::TerrainStep; it.rise = .9 - 1e-3; Check(!std::strcmp(Judge(it).reason, "hop"), "terrain step just under 0.9 m: hop"); }
    { Item it; it.kind = Kind::TerrainStep; it.rise = 1.2; Check(!std::strcmp(Judge(it).reason, "wall"), "terrain step 1.2 m: wall"); }
    { Item it; it.kind = Kind::TerrainStep; it.rise = .6; Check(!std::strcmp(Judge(it).reason, "lip_is_road"), "terrain step 0.6 m: lip is road"); }
    { Item it; it.kind = Kind::NoGround; it.birthTile = true; const auto w = Judge(it); Check(w.sev == Severity::Clearance && !w.spawn.empty(), "no ground: red, birth drops"); }
    { Item it; it.kind = Kind::Vehicle; Check(!std::strcmp(Judge(it).reason, "vehicle_always_live"), "parked vehicle: live"); }
    { Item it; it.kind = Kind::Doubt; Check(Judge(it).sev == Severity::Info, "doubt: info, never a stop"); }
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
