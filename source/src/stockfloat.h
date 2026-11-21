#pragma once
// Lab stock float hold (user decision 2026-11-20: "the lab stock pair's spot check must NOT need driving, wheels or a floor under the road").
// Pure logic; tests: tests/stockfloat_test.cpp. Place in the decision order (notes/VEHICLE_DECISION_DESIGN_20261106.md, section 6 Mode + the
// 2026-11-20 lab section): Mode for a private lab pair is "flight at one commanded ABSOLUTE height" (no ground reference); the Controller
// (the hover actuator, vehicleflight.cpp Fly) holds that height and answers the spot test's pulse; Health reads the same spot proof as before.
//
//  - Capture: the first frame a lab lease exists, the car's own height is the commanded height (born at its slot, so no lift and no fake fall:
//    Health's fall baseline is the birth height).
//  - Relocation: while a seated move of this car is in progress the height follows the car (the move is not an error to correct); when the
//    receipt says applied, the commanded height becomes the receipt's destination height, once per (epoch, serial).
//  - Held: flight mode (not the game's wheels), engaged for settleMs, at the commanded height within heightTolerance, vertical speed within
//    verticalTolerance. Only a held float may be pulsed.
//  - Lapse: a hold nobody renews within freshMs is not active (the lease ended, the car was promoted to a route): the hover goes back to its
//    ordinary rules, which choose hover or wheels for the route (hover-or-drive, travelmode) - there is no second mode decider here.
#include <cmath>
#include <cstdint>
#include <limits>

namespace stockfloat {
namespace rules {
constexpr double heightTolerance = .30;         // m from the commanded height
constexpr double verticalTolerance = .35;       // m/s
constexpr std::uint64_t settleMs = 1000;        // flight must have been engaged this long before the spot pulse
constexpr std::uint64_t freshMs = 500;          // a hold not renewed within this has lapsed
constexpr double pulseThrottle = .25;           // fraction of the gear's thrust, backwards (the spot test's reverse pulse)
constexpr unsigned pulseHoldMs = 120;           // one pulse command's life; renewed every frame while the test asks
constexpr int pulseGear = 1;                    // the gentlest gear
}

struct Hold {
    std::uint32_t vehicle = 0;
    std::uint64_t lease = 0, engagedAt = 0, lastAt = 0, epoch = 0, serial = 0;
    double z = std::numeric_limits<double>::quiet_NaN();
    bool following = false;                      // a relocation of this car is in progress
};

struct Sample {
    std::uint32_t vehicle = 0;
    std::uint64_t lease = 0, now = 0;
    double z = 0;
    bool onWheels = true;                        // still on the game's own wheels (flight not engaged yet)
    bool relocating = false;                     // npcvehicles::RelocationMark(vehicle) & 1
    std::uint64_t relocEpoch = 0, relocSerial = 0;   // the latest APPLIED receipt (0: none)
    double relocToZ = std::numeric_limits<double>::quiet_NaN();
};

enum class Event { None, Captured, Following, Rebased };
inline const char* Name(Event e) {
    constexpr const char* n[]{ "none", "captured", "following_relocation", "rebased_on_receipt" };
    return n[int(e)];
}

inline bool Active(const Hold& h, std::uint64_t now) {
    return h.lease && std::isfinite(h.z) && now >= h.lastAt && now - h.lastAt <= rules::freshMs;
}

// One update per frame while the lab lease exists. Returns what changed (for the trace).
inline Event Update(Hold& h, const Sample& s) {
    if (!s.lease || !s.vehicle || !std::isfinite(s.z)) { h = {}; return Event::None; }
    if (h.lease != s.lease || h.vehicle != s.vehicle || !std::isfinite(h.z)) {
        // A receipt already applied before the capture is history: the capture height is the car's own.
        h = {};
        h.vehicle = s.vehicle; h.lease = s.lease; h.z = s.z; h.lastAt = s.now;
        h.epoch = s.relocEpoch; h.serial = s.relocSerial;
        h.engagedAt = s.onWheels ? 0 : s.now;
        return Event::Captured;
    }
    Event ev = Event::None;
    h.lastAt = s.now;
    const bool newer = s.relocSerial && (s.relocEpoch > h.epoch || (s.relocEpoch == h.epoch && s.relocSerial > h.serial));
    if (s.relocating) {
        if (!h.following) ev = Event::Following;
        h.following = true; h.z = s.z; h.engagedAt = 0;
        return ev;
    }
    h.following = false;
    if (newer) {
        h.epoch = s.relocEpoch; h.serial = s.relocSerial;
        h.z = std::isfinite(s.relocToZ) ? s.relocToZ : s.z;
        h.engagedAt = 0;
        ev = Event::Rebased;
    }
    if (s.onWheels) h.engagedAt = 0;
    else if (!h.engagedAt) h.engagedAt = s.now;
    return ev;
}

// May the spot test pulse now?
inline bool Held(const Hold& h, std::uint64_t now, double z, double vz, bool onWheels) {
    return Active(h, now) && !h.following && !onWheels && h.engagedAt && now >= h.engagedAt &&
        now - h.engagedAt >= rules::settleMs && std::isfinite(z) && std::isfinite(vz) &&
        std::fabs(z - h.z) <= rules::heightTolerance && std::fabs(vz) <= rules::verticalTolerance;
}
}  // namespace stockfloat
