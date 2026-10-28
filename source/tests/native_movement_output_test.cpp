#include "native_movement_output.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace native_movement;
static void Require(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
static bool Near(float a, float b) { return std::abs(a - b) < 0.0001f; }

int main() {
    // Distinct sentinels catch confusing handler-relative and object-relative
    // offsets, or reading an angle/padding value as the requested speed.
    StoredPlannerOutput stored{};
    stored.movementAngleDelta = {0, 0, 1.57079632679f};
    stored.speed = 12;
    stored.secondAngleDelta = {0.25f, 0.5f, -1.0f};
    PlannerSample sample;
    auto bytes = std::as_bytes(std::span(&stored, 1));
    Require(ReadStoredOutput(bytes, sample), "native layout rejected");
    Require(Near(sample.speed, 12) && Near(sample.secondAngleDelta.z, -1), "wrong native field offsets");
    auto motion = Decode(sample, {}, false);
    Require(motion.valid && Near(motion.velocity.x, 12) && Near(motion.velocity.y, 0), "east movement is not speed times heading");
    sample.movementAngleDelta = {};
    motion = Decode(sample, {}, false);
    Require(Near(motion.velocity.y, 12), "zero angular delta incorrectly interpreted as no movement");
    sample.speed = 0;
    sample.movementAngleDelta.z = 1;
    motion = Decode(sample, {}, false);
    Require(motion.valid && Near(motion.rotation.z, 1) && Near(motion.velocity.x, 0), "stationary turn lost or converted to translation");
    sample.speed = 12;
    sample.movementAngleDelta = {-0.5235987756f, 0, 0};
    motion = Decode(sample, {}, true);
    Require(Near(motion.velocity.z, 6), "flight pitch sign is incorrect");
    motion = Decode(sample, {}, false);
    Require(Near(motion.velocity.z, 0) && Near(motion.velocity.y, 12), "ground pitch produces lift");
    sample.movementAngleDelta = {0, 0, -6.1f};
    motion = Decode(sample, {0, 0, 6.2f}, false);
    Require(Near(motion.rotation.z, 0.1f), "rotation at publication was not preserved");
    Require(!ReadStoredOutput(bytes.first(0x58), sample), "truncated buffer accepted");
    stored.speed = std::numeric_limits<float>::quiet_NaN();
    Require(!ReadStoredOutput(bytes, sample) && sample.speed == 0, "invalid sample leaves previous output usable");
    stored.speed = -1;
    Require(!ReadStoredOutput(bytes, sample), "negative speed accepted");
    stored.speed = 1;
    stored.secondAngleDelta.z = std::numeric_limits<float>::infinity();
    Require(!ReadStoredOutput(bytes, sample), "nonfinite angular output accepted");
    Require(!Decode({}, {0, 0, std::numeric_limits<float>::infinity()}, false).valid, "nonfinite reference rotation accepted");
    std::puts("native movement output: layout, speed, angular wrap, stationary turn, flight and invalid-data checks passed");
}
