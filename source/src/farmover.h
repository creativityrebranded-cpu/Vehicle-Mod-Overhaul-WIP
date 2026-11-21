#pragma once
#include <algorithm>
#include <cmath>

namespace farmover {
struct Pose { double x, y, z, heading, pitch; };

namespace rules {
inline constexpr int defaultDistance = 650, minDistance = 30, maxDistance = 2000;
inline constexpr double height = 25.0, speed = 12.0, climb = 0.35;
inline constexpr double duration = 22.0, maxStep = 0.10;
inline constexpr double halfPi = 1.5707963267948966;
}

inline Pose FlightArc(double x, double y, double z, double heading, double distance, double elapsed) {
    const double angle = heading + rules::speed * elapsed / distance;
    return { x + distance * std::sin(angle), y + distance * std::cos(angle), z + rules::height + elapsed * rules::climb,
        angle + rules::halfPi, std::atan2(rules::climb, rules::speed) };
}

inline double Advance(double elapsed, double seconds, bool playing) {
    return playing ? std::min(rules::duration, elapsed + std::clamp(seconds, 0.0, rules::maxStep)) : elapsed;
}

// NiTransform stores a row-major basis in three padded rows, then translation and scale.
// Solve parent * local = the wanted world pose.
inline bool LocalTransform(const float parent[16], const Pose& pose, float worldScale, float local[16]) {
    for (int i = 0; i < 16; ++i)
        if ((i >= 12 || i % 4 != 3) && !std::isfinite(parent[i])) return false;
    if (parent[15] <= 0.01f || !std::isfinite(worldScale) || worldScale <= 0.01f ||
        !std::isfinite(pose.x) || !std::isfinite(pose.y) || !std::isfinite(pose.z) ||
        !std::isfinite(pose.heading) || !std::isfinite(pose.pitch)) return false;

    const double cy = std::cos(pose.heading), sy = std::sin(pose.heading);
    const double cp = std::cos(pose.pitch), sp = std::sin(pose.pitch);
    const double rotation[3][3] = {{cy, sy * cp, -sy * sp}, {-sy, cy * cp, -cy * sp}, {0, sp, cp}};
    const double delta[] = {pose.x - parent[12], pose.y - parent[13], pose.z - parent[14]};
    for (int i = 0; i < 3; ++i) {
        const double x = parent[i], y = parent[4 + i], z = parent[8 + i];
        for (int j = 0; j < 3; ++j)
            local[4 * i + j] = float(x * rotation[0][j] + y * rotation[1][j] + z * rotation[2][j]);
        local[4 * i + 3] = 0.0f;
        local[12 + i] = float((x * delta[0] + y * delta[1] + z * delta[2]) / parent[15]);
    }
    local[15] = worldScale / parent[15];
    return true;
}
}
