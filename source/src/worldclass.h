#pragma once
// World truth, Build A (notes/WORLD_TRUTH_PROPOSAL_20261107.md, design section 2, World). Pure.
// Every ray hit gets a CLASS (what it is) and a CONTEXT (does the known world already explain it). Build A only records them
// (Ledger); nothing here decides anything. Build B's stop rule reads WouldStop. Tests: tests/worldclass_test.cpp.
#include "carcap.h"
#include <cmath>
#include <cstdint>

namespace worldclass {
namespace rules {
constexpr double explainTol = .25;   // m: an up-facing hit this close to the measured floor is that floor
constexpr double bigStatic = 8.0;    // m: a static owner this long (largest side) is a structure
constexpr double smallStatic = 3.0;  // m: a static owner no longer than this is a loose object (post, crate, barrier piece)
constexpr int roadLearn = 3;         // floor-explained hits on one base before that base counts as road
// Drive over (user, 2026-11-07: "certain obstacles less than a certain size can be driven over"): a loose object, plant or unknown hit no
// higher than half the assumed tire over the measured floor is climbed; quarter-tire hits add no slowdown.
// Vehicles and people never are.
// B1.3 (user: "something on the ground ... less than half [or] a quarter of my tires isn't something to stop for"): every height limit
// follows the tire. tireHeight is the wheel's height (m); a quarter of it is dropped, not even slowed; half of it is climbed.
constexpr double tireHeight = 2 * carcap::kCar.climb;  // one car (2026-11-19): the record's climb is half a tyre, so the tyre is twice it (was an assumed 1.0)
constexpr double driveOverLow = tireHeight * .25, driveOverHigh = tireHeight * .5;
}
enum class Class : std::uint8_t { Unknown, Terrain, Road, Structure, Loose, Plant, Barrier, Vehicle, Person, Count };
inline const char* Name(Class c) { constexpr const char* n[]{ "unknown", "terrain", "road", "structure", "loose", "plant", "barrier", "vehicle", "person" }; return c < Class::Count ? n[int(c)] : "?"; }
enum class Context : std::uint8_t { Unexplained, Floor, KnownSolid, Seam, Count };
inline const char* Name(Context c) { constexpr const char* n[]{ "unexplained", "floor", "known_solid", "seam" }; return c < Context::Count ? n[int(c)] : "?"; }

struct Hit {
    int layer = -1;                    // the game's collision layer (-1 unknown)
    std::uint32_t owner = 0;           // the owning reference (0: none, e.g. landscape)
    std::uint32_t base = 0; double size = 0;   // the owner's base and its largest side (m), from the owner lookup
    bool haveNormal = false; double up = 0;    // |normal z|
    double z = 0;                      // hit height
    bool haveFloor = false; double floor = 0;  // measured floor here (route survey)
    bool knownSolid = false;           // the sweep already knows a solid here
    bool roadBase = false;             // the owner's base is in the learned road set
};

// Layers (vehicleflight.cpp kSolidLayers): 1 static, 2 anim static, 3 transparent, 4 clutter, 8 biped, 9 trees, 10 props,
// 13 terrain, 14 trap, 17 ground, 20 debris large, 26 transparent small, 27 invisible wall, 28 small anim, 30 char controller,
// 33 biped no CC, 35 collision box.
inline Class Classify(const Hit& h) {
    switch (h.layer) {
    case 13: case 17: return Class::Terrain;
    case 9: return Class::Plant;
    case 4: case 10: case 14: case 20: case 28: case 2: return Class::Loose;
    case 3: case 26: case 27: return Class::Barrier;
    case 8: case 30: case 33: return Class::Person;
    default: break;
    }
    if (h.owner == 0) return h.layer == 1 || h.layer == 35 ? Class::Terrain : Class::Unknown;  // nothing owns it: landscape
    if ((h.owner >> 24) == 0xFF) return Class::Loose;      // placed at runtime: it can move or be gone next time
    if (h.roadBase) return Class::Road;
    if (h.size >= rules::bigStatic) return Class::Structure;
    if (h.size > 0 && h.size <= rules::smallStatic) return Class::Loose;
    if (h.layer == 1 || h.layer == 35) return Class::Structure;
    return Class::Unknown;
}
// Does the known world already explain this hit? A floor where the floor was measured; a solid the sweep knows; a seam: a road
// or terrain face no higher than lipStep over the measured floor (the step a car drives up).
inline Context Explain(const Hit& h, Class c, double lipStep) {
    if (h.haveFloor && h.haveNormal && h.up > .7 && std::fabs(h.z - h.floor) <= rules::explainTol) return Context::Floor;
    if (h.knownSolid && (c == Class::Structure || c == Class::Road || c == Class::Terrain)) return Context::KnownSolid;
    if (h.haveFloor && (c == Class::Road || c == Class::Terrain) && h.z - h.floor <= lipStep) return Context::Seam;
    return Context::Unexplained;
}
// Build B's stop rule, recorded in Build A: an unexplained hit of these classes may stop the car; a structure needs the body check;
// terrain and road never stop it by a ray alone.
enum class Verdict : std::uint8_t { NoStop, MayStop, BodyCheck };
inline const char* Name(Verdict v) { constexpr const char* n[]{ "no_stop", "may_stop", "body_check" }; return n[int(v)]; }
// Small enough to drive over (needs the measured floor; the owner's size when it has one).
// Lower than a quarter tire over the measured floor, whatever it is (not a vehicle or person): not even slowed for.
inline bool UnderQuarterTire(const Hit& h, Class c) {
    if (c == Class::Vehicle || c == Class::Person || !h.haveFloor) return false;
    return h.z - h.floor <= rules::driveOverLow;
}
inline bool DriveOver(const Hit& h, Class c) {
    if (c != Class::Loose && c != Class::Plant && c != Class::Unknown) return false;
    if (!h.haveFloor) return false;
    const double rise = h.z - h.floor;
    if (rise <= rules::driveOverLow) return true;
    return rise <= rules::driveOverHigh;
}
inline Verdict WouldStop(Class c, Context x) {
    if (x != Context::Unexplained) return Verdict::NoStop;
    switch (c) {
    case Class::Loose: case Class::Plant: case Class::Barrier: case Class::Unknown: case Class::Vehicle: case Class::Person: return Verdict::MayStop;
    case Class::Structure: return Verdict::BodyCheck;
    default: return Verdict::NoStop;
    }
}
// The road set, learned from what the measured floor is made of (no list per city).
template <int N = 64>
struct RoadSet {
    std::uint32_t base[N] = {}; int hits[N] = {}; int count = 0;
    void NoteFloor(std::uint32_t b) {
        if (!b) return;
        for (int i = 0; i < count; ++i) if (base[i] == b) { ++hits[i]; return; }
        if (count < N) { base[count] = b; hits[count] = 1; ++count; }
    }
    bool Has(std::uint32_t b) const { for (int i = 0; i < count; ++i) if (base[i] == b) return hits[i] >= rules::roadLearn; return false; }
};
}
