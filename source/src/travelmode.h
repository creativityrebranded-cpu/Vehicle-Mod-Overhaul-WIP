#pragma once
#include <cmath>
#include <cstdint>

// Hover or drive (2026-10-21, the user): every NPC vehicle chooses per moment whether it flies (the mod's hover) or drives
// on the ground with the game's own controls, from its navigation profile's default and the reasons around it. Pure: inputs
// in, a mode and its reason out; the caller measures the inputs (vehicleflight). The first matching reason wins:
//  catching the player from below > damaged (hover fails first) > cramped > water ahead > obstacle taller than a jump > rough ground ahead (rays, up to 250 m)
//  > fighting or fleeing > the goal 75 m+ higher > inside a settlement or town > trip length > the profile's default.
// A non-blocking reason must hold kSettleMs before the mode changes, and a mode is kept kMinModeMs; blocking reasons
// (damaged, cramped, water, too tall, rough) switch at once.
namespace travelmode {
enum class Mode : std::uint8_t { Hover, Drive };
enum class Default : std::uint8_t { Auto, Drive, Hover, CopyPlayer };

inline constexpr double kDamaged = 0.30;        // health share under which the hover fails
inline constexpr double kGoalAbove = 75.0;      // m the goal lies above the vehicle (z only; lower goals are driven to)
inline constexpr double kShortTrip = 300.0, kLongTrip = 500.0;  // m
inline constexpr std::uint64_t kSettleMs = 2000, kMinModeMs = 10000;
// After a blocking reason switched the mode (water, a tall obstacle, rough ground...), keep it this long so the vehicle gets
// past what forced it (2026-10-21 run: a town's buildings flipped an NPC drive -> hover -> drive every 10 s).
inline constexpr std::uint64_t kBlockedHoldMs = 30000;
// The corridor's climb need above this is a wall to steer around, never a reason to fly over (buildings in towns).
inline constexpr double kWallNeed = 25.0;

struct Inputs
{
    Default profile = Default::Auto;
    bool playerHovering = false;   // CopyPlayer: the player's own vehicle hovers
    double tripDist = 0;           // m to the goal
    double goalAbove = 0;          // goal z - vehicle z (z only, + when the goal is higher), 0 when the goal has no height
    bool waterAhead = false, roughAhead = false;  // seen by rays along the way (up to 250 m)
    double clearNeed = 0;          // m the way ahead needs to be cleared by (the corridor rays), 0 open
    double jumpHeight = 4.0;       // m a vertical boost clears on the ground
    bool fighting = false, fleeing = false;
    bool damaged = false, cramped = false;
    bool catching = false;         // the player's own vehicle going under a falling / jumping player
    bool inSettlement = false;     // the place it stands in has an owner (a settlement, outpost or town)
};
struct Reason { Mode mode = Mode::Hover; bool blocking = false; const char* why = ""; };

inline Reason Decide(const Inputs& in)
{
    if (in.catching) return { Mode::Hover, true, "catching you" };
    if (in.damaged) return { Mode::Drive, true, "damaged: the hover fails" };
    if (in.cramped) return { Mode::Drive, true, "cramped" };
    if (in.waterAhead) return { Mode::Hover, true, "water ahead" };
    if (in.clearNeed > in.jumpHeight && in.clearNeed <= kWallNeed) return { Mode::Hover, true, "obstacle taller than a jump" };
    if (in.roughAhead) return { Mode::Hover, true, "rough ground ahead" };
    if (in.fighting) return { Mode::Hover, false, "fighting" };
    if (in.fleeing) return { Mode::Hover, false, "fleeing" };
    if (in.goalAbove > kGoalAbove) return { Mode::Hover, false, "goal far above" };
    if (in.inSettlement) return { Mode::Drive, false, "in a settlement" };
    if (in.tripDist > kLongTrip) return { Mode::Hover, false, "long trip" };
    if (in.tripDist < kShortTrip && in.profile != Default::Hover) return { Mode::Drive, false, "short trip" };
    switch (in.profile) {
    case Default::Drive: return { Mode::Drive, false, "profile: drive" };
    case Default::CopyPlayer: return { in.playerHovering ? Mode::Hover : Mode::Drive, false, in.playerHovering ? "copying you: hover" : "copying you: drive" };
    case Default::Hover: return { Mode::Hover, false, "profile: hover" };
    default: return { Mode::Hover, false, "default" };
    }
}

// The mode actually used: changes only when a reason holds (kSettleMs) and the current mode had its time (kMinModeMs),
// or at once for a blocking reason.
struct Hold
{
    Mode mode = Mode::Hover;
    bool started = false;
    std::uint64_t since = 0;       // the current mode began
    Mode want = Mode::Hover;
    std::uint64_t wantSince = 0;   // the wanted mode began to be wanted
    std::uint64_t blockedUntil = 0;  // a blocking reason's mode is kept until then
};
// Returns true when the mode changed this call.
inline bool Step(Hold& h, const Reason& r, std::uint64_t now)
{
    if (!h.started) { h = { r.mode, true, now, r.mode, now, r.blocking ? now + kBlockedHoldMs : 0 }; return true; }
    if (r.blocking && r.mode == h.mode) h.blockedUntil = now + kBlockedHoldMs;  // still blocked: keep holding
    if (r.mode != h.want) { h.want = r.mode; h.wantSince = now; }
    if (r.mode == h.mode) return false;
    const bool settled = now - h.wantSince >= kSettleMs && now - h.since >= kMinModeMs && now >= h.blockedUntil;
    if (!r.blocking && !settled) return false;
    h.mode = r.mode; h.since = now;
    h.blockedUntil = r.blocking ? now + kBlockedHoldMs : 0;
    return true;
}
}
