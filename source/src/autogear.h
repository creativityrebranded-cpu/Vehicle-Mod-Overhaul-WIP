#pragma once
// AG-1 (2026-11-18, user: "let it pick by itself / can it know its own slip for gear shifting"): Cruise and AI cars choose their gear
// on wheels from what the wheels feel and the road ahead. Mode/arbiter rule (design section 6/9): a gear is a request the controller
// carries out (grip x gear, GR-1; top speed per gear). Inputs:
//   slip   - RB-1 (2026-11-18): the rear slip angle in degrees (wheelgrip::RearSlipDeg from the physics body: the back stepping out),
//            smoothed. REPLACED: the WheelInfo +0xB4 "forward slip" (it read ~0.8 x speed at any throttle in the 19:00 test drive);
//   sideways - sideways speed of the body (a skid).
// Wanted gear: 1 when slip >= slip1 or sideways >= side1; 2 when slip >= slip2; else 3.
// SP-1 (2026-11-19, one owner per output): the grade no longer picks a gear. A gear is the slide-control profile (grip x gear, turning
// weight, corner-force budget); a slope is the tyre controller's job (railtyres grade push). Gear 1 on the 23 deg lc_ring_road ramp
// crawled at 0.3-2.2 m/s while gears 2/3 climbed at 5.4-6.4 m/s and a hand-driven car climbed on full throttle.
// Downshifts at once once the cause has lasted downHold (2 reactions); upshifts one gear at a time after the cause has been gone upHold.
#include "wheelguard.h"
#include <algorithm>
#include <cmath>

namespace autogear {
namespace rules {
// Stage F (GENERATED_COSTS_DESIGN_20261122.md). Searched the one car record (carcap.h) and wheelgrip.h for the tyre's lateral-force curve
// (the slip angle where lateral grip saturates): there is none (carcap::Car holds mu 1.15, planLat 3 and tyreLat 6 m/s^2 but no cornering
// stiffness, and wheelgrip.h has only friction multipliers). A peak angle from an assumed tyre model would be a fake derivation.
// So slip1, slip2 and side1 stay as CHOSEN RESOLUTIONS (the user's RB-1 / 19:24 run values; tracking.h slipOk is the same 8 deg) until
// the game's tyre curve is measured. Reported, not derived.
constexpr double slip1 = 8.0, slip2 = 4.0;     // RB-1: degrees of rear slip angle (chosen)
constexpr double side1 = 1.5;                  // m/s sideways skid (chosen)
constexpr double reaction = wheelguard::reaction;  // s: the controller replaces a command in this long
// Down hold: the cause must outlast two reactions (one to replace the command, one to see it take effect). Derived: 2 t_r.
constexpr double downHold = 2.0 * reaction;
// Up hold: CHOSEN RESOLUTION kept at 2.5 s (the wheels have no thrust-response row; the hold is the anti-hunting dwell after a skid).
constexpr double upHold = 2.5;                 // s
// The slip filter's time constant: a filter faster than the controller's reaction passes noise it cannot act on, slower lags the cause.
// Derived: t_r.
constexpr double slipSmooth = reaction;
}
inline int Wanted(double slip, double sideways) {
    if (slip >= rules::slip1 || sideways >= rules::side1) return 1;
    if (slip >= rules::slip2) return 2;
    return 3;
}
struct State { int gear = 3; double lowerSince = -1, higherSince = -1, slip = 0, at = -1; };
// One update; returns the gear to drive in. `now` in seconds.
inline int Update(State& s, double now, double rawSlip, double sideways) {
    const double dt = s.at < 0 || now < s.at ? 0.0 : std::min(1.0, now - s.at); s.at = now;
    const double k = dt <= 0 ? 1.0 : 1.0 - std::exp(-dt / rules::slipSmooth);
    s.slip += (std::fabs(rawSlip) - s.slip) * k;
    const int want = Wanted(s.slip, sideways);
    if (want < s.gear) { if (s.lowerSince < 0) s.lowerSince = now; s.higherSince = -1; if (now - s.lowerSince >= rules::downHold) { s.gear = want; s.lowerSince = -1; } }
    else if (want > s.gear) { if (s.higherSince < 0) s.higherSince = now; s.lowerSince = -1; if (now - s.higherSince >= rules::upHold) { s.gear = s.gear + 1; s.higherSince = now; } }
    else { s.lowerSince = s.higherSince = -1; }
    return s.gear;
}
}
