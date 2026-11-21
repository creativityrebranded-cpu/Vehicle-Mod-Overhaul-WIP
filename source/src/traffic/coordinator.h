#pragma once
// The traffic coordinator (design section 9, D15 as amended 2026-11-07; change cards C3, C4). It lives in the addon.
// Route data: every rule that holds a lane or sets a stop line reports here by name, and Holds is the one writer of `held` and
// `stopDistance` (before: ten rules wrote them directly in LaneProvider). The rules keep their own conditions, unchanged.
// World data (C4): which vehicles are out of traffic (Health called them broken, or they are retiring) goes to the core's Vehicle
// List, the source that says who a vehicle is; World reads it like "parked" (D1 steer round). Collision safety stays per car.
// Pure; tests: tests/coordinator_test.cpp.
#include <algorithm>

namespace coordinator {
// Why a lane is held or stopped (bit values are the traffic.lane_hold trace's, unchanged).
inline constexpr unsigned kHeldReasons = 0x40000000u;  // PD-1c: set in `held` when the low 16 bits are reasons (the core's routeguidance::HeldWhy)
enum Reason : unsigned {
    Holding = 1u, Script = 2u, Connection = 4u, ExcludedModel = 8u, Zone = 16u, ZoneMode = 32u, LanePlan = 64u, Pickup = 128u,
    StopControl = 256u, Junction = 512u,
    Following = 1024u,
};
namespace rules {
constexpr double followingReach=110, poseFresh=1, bumperHalfFallback=2.8;
constexpr double laneHeightTolerance=3, laneSideAllowance=.6;
}

template <class Lane> struct Holds {
    Lane& lane; unsigned bits = 0;
    explicit Holds(Lane& l) : lane(l) {}
    // PD-1c (2026-11-12): `held` carries the reasons (kHeldReasons | bits) so the core can name them; any non-zero value is still "held"
    // to an older core, exactly as before.
    void Hold(unsigned why) { bits |= why; lane.held = int(kHeldReasons | bits); }
    void HoldIf(bool on, unsigned why) { if (on) Hold(why); }
    // A stop line: the nearest one wins.
    void StopMin(double d) { lane.stopDistance = lane.stopDistance < 0 ? d : std::min(double(lane.stopDistance), d); }
    // The section control's own line, which replaces any earlier one (as before).
    void StopSet(double d) { lane.stopDistance = d; }
};

// C4: how long the core keeps an "out of traffic" mark without hearing it again.
constexpr unsigned long long kOutOfTrafficMs = 3000;
}
