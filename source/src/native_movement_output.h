#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace native_movement {

struct Vector { float x = 0, y = 0, z = 0; };

// Starfield 1.16.244 MovementHandlerAgentStorePlannerOutput, complete object.
// 14272ded0 receives the secondary handler interface at object + 0x18.
// Its movement-state +0x30 getter returns ROTATION (140689bd0), not position.
// The writer copies planner +0x10 to object +0x40 without dividing by time.
struct StoredPlannerOutput {
    std::byte interfaces[0x30];
    Vector movementAngleDelta;
    std::byte pad3c[4];
    float speed;
    std::byte pad44[12];
    Vector secondAngleDelta;
};
static_assert(offsetof(StoredPlannerOutput, movementAngleDelta) == 0x30);
static_assert(offsetof(StoredPlannerOutput, speed) == 0x40);
static_assert(offsetof(StoredPlannerOutput, secondAngleDelta) == 0x50);
static_assert(sizeof(StoredPlannerOutput) == 0x5c);

struct PlannerSample {
    Vector movementAngleDelta;
    Vector secondAngleDelta;
    float speed = 0;
};

inline bool Finite(Vector v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Copy only the values written by the native agent. The caller owns lifetime
// and synchronization, and must snapshot the rotation from this same update.
inline bool ReadStoredOutput(std::span<const std::byte> object, PlannerSample& result) {
    result = {};
    if (object.size() < sizeof(StoredPlannerOutput)) return false;
    PlannerSample sample;
    std::memcpy(&sample.movementAngleDelta, object.data() + 0x30, sizeof(Vector));
    std::memcpy(&sample.speed, object.data() + 0x40, sizeof(float));
    std::memcpy(&sample.secondAngleDelta, object.data() + 0x50, sizeof(Vector));
    if (!Finite(sample.movementAngleDelta) || !Finite(sample.secondAngleDelta) ||
        !std::isfinite(sample.speed) || sample.speed < 0) return false;
    result = sample;
    return true;
}

struct Motion {
    Vector velocity;
    Vector rotation;
    bool valid = false;
};

inline Motion Decode(const PlannerSample& sample, Vector rotationAtUpdate, bool flight) {
    if (!Finite(rotationAtUpdate) || !Finite(sample.movementAngleDelta) ||
        !std::isfinite(sample.speed) || sample.speed < 0) return {};
    constexpr double tau = 6.28318530717958647692;
    Motion result;
    result.rotation = {
        float(std::remainder(double(rotationAtUpdate.x) + sample.movementAngleDelta.x, tau)),
        float(std::remainder(double(rotationAtUpdate.y) + sample.movementAngleDelta.y, tau)),
        float(std::remainder(double(rotationAtUpdate.z) + sample.movementAngleDelta.z, tau))};
    // Same reference axes as 0.8: yaw=atan2(x,y), positive X rotation nose-down.
    // Ground movement follows native yaw; pitch of a grounded chassis is not lift.
    const double pitch = flight ? result.rotation.x : 0.0;
    const double horizontal = sample.speed * std::cos(pitch);
    result.velocity = {float(horizontal * std::sin(result.rotation.z)),
                       float(horizontal * std::cos(result.rotation.z)),
                       float(-sample.speed * std::sin(pitch))};
    result.valid = true;
    return result;
}

} // namespace native_movement
