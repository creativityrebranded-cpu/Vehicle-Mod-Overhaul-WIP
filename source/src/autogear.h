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
// Downshifts at once once the cause has lasted downHold s; upshifts one gear at a time after the cause has been gone upHold s.
#include <algorithm>
#include <cmath>

namespace autogear {
namespace rules {
constexpr double slip1 = 8.0, slip2 = 4.0;     // RB-1: degrees of rear slip angle
constexpr double side1 = 1.5;                  // m/s sideways skid
constexpr double downHold = .4, upHold = 2.5;  // s
constexpr double slipSmooth = .25;             // s time constant of the slip filter
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
