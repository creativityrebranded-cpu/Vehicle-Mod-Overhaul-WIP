#pragma once
#include <cmath>
#include <cstdint>
// Combat of the player's own EMPTY vehicle under Cover (U) or a guard point (T while covering) (2026-10-20, the user): enemies of the player
// around the protected point (the player, or the guard point) are picked, the vehicle takes a defensive spot (environment::CombatPosition: cover
// from the enemy, a firing lane) between the player and the threat or at the guard point, faces the threat there and fires bursts with the nose on
// target. Never while the player drives it, never with a crew driver aboard (npcvehicles fights that one), and never outside Cover / Guard.
namespace coverfight {
inline constexpr double kRange = 120.0;                 // m around the protected point
inline constexpr double kKeep = 1.3;                    // the current enemy is kept out to kRange * kKeep
inline constexpr double kShield = 12.0;                 // m from the player toward the threat (Cover's defend spot)
inline constexpr double kFireRange = 80.0;              // m (the NPC vehicles' weapon range)
inline constexpr double kCone = 0.14, kPitchCone = 0.21;  // rad: the weapon fires along the nose
inline constexpr double kAtSpot = 6.0;                  // m: holding its spot, the vehicle turns to face the threat
inline constexpr std::uint64_t kOverMs = 8000;          // the fight is over this long after the last enemy was seen
inline constexpr std::uint64_t kPickMs = 500, kBurstMs = 1200, kGapMs = 1500;

inline bool Allowed(bool playerDrives, bool crewDrives, bool cover, bool guard) { return !playerDrives && !crewDrives && (cover || guard); }
inline bool Fighting(bool haveFoe, std::uint64_t lastSeen, std::uint64_t now) { return haveFoe && now - lastSeen < kOverMs; }
inline bool OnTarget(double yawErr, double pitchErr, double range) {
    return std::fabs(yawErr) < kCone && std::fabs(pitchErr) < kPitchCone && range < kFireRange;
}
// The defend spot's centre for Cover: kShield from the player toward the threat (the vehicle shields the player).
inline void ShieldPoint(double px, double py, double ex, double ey, double& x, double& y) {
    const double d = std::hypot(ex - px, ey - py);
    x = d > 0.1 ? px + (ex - px) / d * std::fmin(kShield, d * 0.5) : px;
    y = d > 0.1 ? py + (ey - py) / d * std::fmin(kShield, d * 0.5) : py;
}
// Bursts: fire up to kBurstMs while on target, then at least kGapMs off.
struct Burst {
    bool firing = false;
    std::uint64_t at = 0;
    bool Update(bool canFire, std::uint64_t now) {
        if (firing) { if (!canFire || now - at >= kBurstMs) { firing = false; at = now; } }
        else if (canFire && now - at >= kGapMs) { firing = true; at = now; }
        return firing;
    }
};
}
