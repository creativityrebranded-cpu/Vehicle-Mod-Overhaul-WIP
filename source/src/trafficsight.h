#pragma once
// The N rings of visibility around the player (DW-3, 2026-11-21, the user's design). Pure: no engine calls, no files, no clocks.
// notes/VEHICLE_DECISION_DESIGN_20261106.md, "Dots: rings of visibility and car identity (DW-3)".
//
// The rings only RANK where a car is made first (priority); they decide nothing on their own. The core's own concealment test at the grant
// (FVO_TrafficBirthHiddenV1) and at creation is the gate.
//   ring 0 = the viewer's own position; ring k (1..rings) = the positions the viewer can reach after k steps of x metres.
// The time a car must stay unseen (T_unseen) = the create latency + the proving time (the car sits at rest, frozen, until it is seated and released) + a margin:
// a car is visible the moment it exists, long before it drives. The step x = (the distance the viewer can cover in T_unseen at its maximum speed) / rings; the
// maximum speed = the current speed plus an acceleration allowance, capped by the player's craft's own top speed (Reach / ReachOf).
#include <algorithm>
#include <cmath>

namespace trafficsight {
namespace rules {
inline constexpr double rangeM = 350.0;  // beyond this nothing is visible (= camera_visibility::rules::maxRange)
}  // namespace rules

// ---- the rings. Every number is cited; measured ones come from the live log.
namespace ringrules {
// T_unseen: how long a car must stay unseen = the time it takes to exist, plus the time it sits frozen until it is released, plus a margin.
inline constexpr int UnseenFor(int createLatencyMs, int provingMs, int marginMs) { return (createLatencyMs > 0 ? createLatencyMs : 0) + (provingMs > 0 ? provingMs : 0) + (marginMs > 0 ? marginMs : 0); }
inline constexpr int rings = 3;                  // the named number of rings (the user: default 3): ring 1..3 beyond ring 0 (the viewer's own position)
// Request -> the car (chassis and driver) exists. MEASURED (traffic.live create_ms, 2026-11-20 and 2026-11-21 runs): chassis 16-47 ms, driver 32-78 ms.
inline constexpr int createLatencyMs = 150;
// How long a fleet car stays frozen at its spot after it exists, until it is seated, proved and released to its pilot (the log line 'Traffic arrival: vehicle .. released to
// its pilot X s after its birth'): MEASURED in the 2026-11-21 runs: 10:25:38.412 -> 10:25:40.949 = 2.5 s (log FlyingVehicleOverhaul.log 10:25), 3.1 s (09:27:33), 3.0 s and 3.6 s
// (09:43:21/22). The worst of these ordinary releases, 3.6 s, is used; two later releases took 20 s (09:43:44, a deferred handoff) and are NOT covered.
// See notes/dot-run-20261121/ring-run-1021-1026-excerpts.txt. The proving time counts from the birth, so it includes the create latency above (a deliberate small double count).
inline constexpr int provingMs = 3600;
inline constexpr int marginMs = 100;             // slack on top of the latency
inline constexpr int unseenMs = UnseenFor(createLatencyMs, provingMs, marginMs);  // T_unseen = 3850 ms: a built car must stay unseen this long
// The viewer's acceleration allowance: the planner's own frame default accel = 6 m/s^2 (planframe.h Frame::accel); measured player accelerations were 2-3 m/s^2
// (carprofile.txt accel_avg 2.4-2.95, the 10:25 run 5.6 -> 24.6 m/s in 10 s), so 6 is about twice what was seen.
inline constexpr double accelMps2 = 6.0;
// The player craft's own top speed is not known to the add-on (no export gives it): the cap is the highest flight gear of the player's gear table, carcap.h kFlightTop[4] = 150 m/s.
inline constexpr double viewerTopMps = 150.0;
}  // namespace ringrules

// How far the viewer can get in the time a car must stay unseen.
struct Reach {
    int rings = ringrules::rings, unseenMs = ringrules::unseenMs;
    double topMps = 0;    // the viewer's maximum speed over the time: the current speed plus the acceleration allowance, capped by the craft's top (never below the current speed)
    double totalM = 0;    // topMps * T_unseen: the radius of the last ring
    double stepM = 0;     // x = totalM / rings
    double RadiusOf(int ring) const { return std::clamp(ring, 0, rings) * stepM; }
};
inline Reach ReachOf(double speedMps, int unseenMs = ringrules::unseenMs, int rings = ringrules::rings, double accel = ringrules::accelMps2, double top = ringrules::viewerTopMps) {
    Reach r; r.rings = std::max(1, rings); r.unseenMs = std::max(0, unseenMs);
    const double v = std::isfinite(speedMps) ? std::fabs(speedMps) : 0.0, t = double(r.unseenMs) / 1000.0;
    r.topMps = std::max(v, std::min(std::max(0.0, top), v + std::max(0.0, accel) * t));
    r.totalM = r.topMps * t; r.stepM = r.totalM / double(r.rings);
    return r;
}
}  // namespace trafficsight
