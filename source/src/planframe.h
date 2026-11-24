#pragma once
// One update's reading of a lane car for the energy plan (removal 2026-11-14): the Frame World fills, the line point, and the field
// tests World uses on shapes. Types and pure tests only, no decision. Moved out of the old plan runner (planrun.h, removed 2026-11-14,
// kept outside FVO in fvo-removed-20261114/deciders); names kept so the energy plan, health and logs read the same.
#include "fieldbox.h"
#include "fieldslice.h"
#include "planrules.h"
#include "planworld.h"
#include "spottest.h"
#include <limits>

namespace planrun {
using planbranch::Branch; using planbranch::Family; using planbranch::kSamples; using planbranch::kStep;
using planworld::Shape; using planworld::Verdict;

struct LinePoint { double x = 0, y = 0, z = 0, yaw = 0; };

struct Frame {
    double now = 0, speed = 0, lateral = 0, station = 0, want = 10, sinceStart = 1e9;
    double accel = 6, brake = 6, topSpeed = 20, lineEnd = 1e9;
    planworld::Car car;
    fieldslice::Slice slice;  // World's own record of this car's ray volume (trace and the shadow body check). The planner does not read it:
                              // the volume reaches it as shapes (fieldvolume.h), through the one collision test.
    bool volumeWorld = false; double volumeEnd = 0;  // the volume is this update's fixed-obstacle source, measured this far ahead
    double volumeRange = 0;  // how far ahead the volume reaches when nothing stops its beams (its speed-scaled depth); Speed's measured-range limit
    int travel = 1; bool hoverPossible = true, paused = false, driverSeated = true, touching = false, held = false;
    const char* heldWhy = "";  // PD-1c: the route hold's reason (routeguidance::HeldWhy); empty: not held
    const char* wantWhy = "";  // PD-1c: why `want` is below the car's top speed (a lane speed limit); empty: it is not
    double aboveSurface = 0, goalDistance = 1e9, x = 0, y = 0, carBase = 0;
    double grip = 1.0;  // GR-1: the wheels' grip multiplier (the gear's); the plan's turn speed limit (latAccel) scales with it
    planbranch::Lanes lanes;
    double clearHeight = 0, headingError = 0; bool rearOpen = true;
    const Branch* extra = nullptr; int extraCount = 0;  // lines from elsewhere: a navmesh path, a trail, the swept clear line
    int sweepKnown = 0, sweepShapes = 0, sweepBand = 0; // Ledger only
    spottest::Ask test;    // ST-A: an imperceptible spot test pulse the arbiter commits this update (inactive: none)
    bool raysLive = true;  // source priority: with no live rays, predictions are the best evidence and may stop the car
    // Field boxes (step 3): the chain round the car, and whether it is contained (whole car inside for fieldbox::kSettle). Faces never keep
    // a car out; once contained, a branch that leaves the chain is out, or with no progress (D12) allowed at rules::leaveField.
    const fieldbox::Box* field = nullptr; int fieldCount = 0; bool contained = false;
    const char* airWhy = nullptr; const char* groundWhy = nullptr;  // G37 (travelmode.h), when this vehicle has a fresh reason
    double facing = std::numeric_limits<double>::quiet_NaN();     // where the nose should point (combat, an arrive heading); NaN: along the line
    bool flying = false;      // the car is in the air now (Mode starts from flight; rail only after landing)
    bool flightLine = false;  // the plan also flies (planHover): where nothing is wanted under a point, the car's base there is the line's own z
                              // (a 3D line), not the car's present height carried along. false: exactly as before.
};

// Fix B (2026-11-07): how sure World is of a shape. Ray hits are sure when enough of them agree (Shape.support, counted by World), the
// surface normal is known and the hit is fresh; other sources (boxes, vehicles, people, contacts) are always sure.
inline bool Sure(const Shape& s, double now) {
    if (s.source != planworld::Source::Ray) return true;
    return s.support >= planbranch::rules::confirmHits && s.haveNormal && now - s.seenAt <= planbranch::rules::freshHit;
}

// A static shape in line terms, or false if it is nothing to this car.
// D14: a solid inside a box that allows solids is not an obstacle: the route goes through it on purpose. Only fixed collision data:
// vehicles, people and contacts always count, so a real hit still shows.
// IR-1 (2026-11-07, user: "find if it's part of the route"): is a moving shape's projected footprint at time t inside the route field (the
// road the car is on)? Only asked for projections (t > 0) while the car itself is inside its field; with no field known the answer is yes
// (nothing changes). Uses World's prediction (planworld::At) and the Field Provider's boxes already in the Frame.
// Field Mask: is this pose inside the authored field (the road the car is on)? With no field known the answer is yes, so nothing changes.
inline bool PoseInField(const Frame& f, double x, double y, double z) {
    if (!f.field || f.fieldCount <= 0) return true;
    for (int k = 0; k < f.fieldCount; ++k) if (fieldbox::Contains(f.field[k], { x, y, z }, .3)) return true;
    return false;
}
inline bool ProjectionInRoute(const Shape& s, double t, const Frame& f, const planworld::Projection& q) {
    if (t <= 0 || !f.contained || !f.field || f.fieldCount <= 0) return true;
    const auto& at = q.at;
    const double reach = std::max(s.halfWidth, s.halfLength) + q.margin;
    for (int k = 0; k < f.fieldCount; ++k) if (fieldbox::Contains(f.field[k], { at.x, at.y, at.z }, reach)) return true;
    return false;
}
inline bool ProjectionInRoute(const Shape& s, double t, const Frame& f) {
    return ProjectionInRoute(s, t, f, planworld::Project(s, t));
}
inline bool SolidAllowed(const Shape& s, const Frame& f) {
    if (s.hazardMark) return false;  // a hazard box is a mark, not a structure the route runs through
    if (s.source != planworld::Source::Ray && s.source != planworld::Source::Box && s.source != planworld::Source::Ground && s.source != planworld::Source::Survey && s.source != planworld::Source::Volume && s.source != planworld::Source::Radar) return false;
    for (int k = 0; k < f.fieldCount && f.field; ++k) if (f.field[k].solids && fieldbox::Contains(f.field[k], { s.centre.x, s.centre.y, s.centre.z }, .5)) return true;
    return false;
}
// Real distance travelled along the road (the lane window's station restarts with each window); a jump is a move, not travel.
struct Odometer {
    bool have = false; double x = 0, y = 0, distance = 0;
    double Step(double nx, double ny, double yaw) {
        if (have) { const double dx = nx - x, dy = ny - y; if (std::hypot(dx, dy) < 30) distance += dx * std::sin(yaw) + dy * std::cos(yaw); }
        have = true; x = nx; y = ny; return distance;
    }
};
}
