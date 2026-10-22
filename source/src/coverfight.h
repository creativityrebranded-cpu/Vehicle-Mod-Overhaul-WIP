#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>
// Combat of the player's own EMPTY vehicle under Cover (U) or a guard point (T while covering) (2026-10-20, the user): enemies of the player
// around the protected point (the player, or the guard point) are picked, the vehicle takes a defensive spot (environment::CombatPosition: cover
// from the enemy, a firing lane) between the player and the threat or at the guard point, faces the threat there and fires bursts with the nose on
// target. Never while the player drives it, never with a crew driver aboard (npcvehicles fights that one), and never outside Cover / Guard.
namespace coverfight {
inline constexpr double kRange = 120.0;                 // m around the protected point
inline constexpr double kKeep = 1.3;                    // the current enemy is kept out to kRange * kKeep
inline constexpr double kShield = 12.0;                 // m from the player toward the threat (Cover's defend spot)
inline constexpr double kFireRange = 100.0;             // m (the NPC vehicles' weapon range)
// Launch re-aim permits a 25 degree cone, widening to 40 degrees nearby.
inline constexpr double kCone = 0.4363323129985824, kConeNear = 0.6981317007977318;
inline double FireCone(double range) { return std::clamp(std::atan2(8.0, std::max(1.0, range)), kCone, kConeNear); }
inline constexpr double kChaseRange = 700.0, kChaseKeep = 840.0;
inline constexpr double kAtSpot = 15.0;                 // m: near its spot, the vehicle turns to face the threat
inline constexpr std::uint64_t kOverMs = 8000;          // the fight is over this long after the last enemy was seen
inline constexpr std::uint64_t kPickMs = 500, kBurstMs = 3000, kGapMs = 400;
inline constexpr std::uint64_t kTargetHoldMs = 4000, kPositionHoldMs = 4000, kBlockedMs = 750;
inline bool KeepTarget(bool valid, bool higherPriority, std::uint64_t heldMs) {
    return valid && (!higherPriority || heldMs < kTargetHoldMs);
}
inline bool Reposition(bool haveSpot, bool firing, bool playerMoved, bool danger, std::uint64_t heldMs, std::uint64_t blockedMs) {
    return !haveSpot || danger || playerMoved || (!firing && heldMs >= kPositionHoldMs && blockedMs >= kBlockedMs);
}
inline constexpr std::uint64_t kJinkHitMs = 1500, kJinkMs = 900;  // hit this recently: jink sideways (break), flipping every kJinkMs

inline bool Allowed(bool playerDrives, bool crewDrives, bool cover, bool guard) { return !playerDrives && !crewDrives && (cover || guard); }
inline bool Fighting(bool haveFoe, std::uint64_t lastSeen, std::uint64_t now) { return haveFoe && now - lastSeen < kOverMs; }
inline bool OnTarget(double yawErr, double pitchErr, double range, double weaponRange=kFireRange) {
    const double cone=FireCone(range);
    return std::isfinite(range) && range>=0 && std::isfinite(weaponRange) && weaponRange>0 && std::fabs(yawErr) < cone && std::fabs(pitchErr) < std::min(cone*1.5,0.9) && range < weaponRange;
}
// The defend spot's centre for Cover: kShield from the player toward the threat (the vehicle shields the player).
inline void ShieldPoint(double px, double py, double ex, double ey, double& x, double& y) {
    const double d = std::hypot(ex - px, ey - py);
    x = d > 0.1 ? px + (ex - px) / d * std::fmin(kShield, d * 0.5) : px;
    y = d > 0.1 ? py + (ey - py) / d * std::fmin(kShield, d * 0.5) : py;
}
// The attack stance's spot: kStandOff from the enemy on the vehicle's side of it, kept within kRange of the protected point (the player or guard point).
inline constexpr double kStandOff = 25.0;
inline void AttackPoint(double cx, double cy, double ex, double ey, double vx, double vy, double& x, double& y) {
    const double d = std::hypot(vx - ex, vy - ey);
    x = d > 0.1 ? ex + (vx - ex) / d * kStandOff : ex + kStandOff;
    y = d > 0.1 ? ey + (vy - ey) / d * kStandOff : ey;
    const double r = std::hypot(x - cx, y - cy);
    if (r > kRange) { x = cx + (x - cx) / r * kRange; y = cy + (y - cy) / r * kRange; }
}
// Bursts: fire up to kBurstMs while on target, then at least kGapMs off.
struct Burst {
    bool firing = false;
    std::uint64_t at = 0, restUntil = 0;
    bool Update(bool canFire, std::uint64_t now) {
        if (firing && now - at >= kBurstMs) { firing=false; restUntil=now+kGapMs; }
        if (!canFire) { firing=false; return false; }
        if (!firing && now>=restUntil) { firing=true; at=now; }
        return firing;
    }
};
}
