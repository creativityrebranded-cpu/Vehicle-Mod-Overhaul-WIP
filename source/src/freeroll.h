#pragma once
#include <algorithm>
#include <cmath>

// Free roll (2026-10-23, notes/FLIGHT_MODEL_STUDY_20261023.md stage 1): the player's hover turns about its own axes, so the
// roll can go all the way round. The nose follows the camera aim measured in the body frame (a world pitch applied about the
// body's right axis turns the wrong way past 90 degrees of bank), and the roll has no limit. Pure math: tests/freeroll_test.cpp.
namespace freeroll {
inline constexpr double kRate = 3.0;          // rad/s: roll rate while a roll key is held (scaled by the hull)
inline constexpr double kMinRate = 1.5;       // rad/s: the slowest free roll, however big the hull
inline constexpr double kAssistBand = 1.05;   // rad (60 degrees): assist levels a released roll only inside this
inline constexpr double kGroundLevel = 6.0;   // m: this close over the ground a roll stops at kGroundMaxRoll and levels when released
inline constexpr double kGroundMaxRoll = 1.2; // rad (about 70 degrees): never on its side or roof that low (the hover height is ~3 m)

struct Vec { double x = 0, y = 0, z = 0; };

inline double Wrap(double a) {
    if (!std::isfinite(a)) return 0.0;
    return std::remainder(a, 6.283185307179586);
}
inline double Dot(const float a[3], const Vec& b) { return a[0] * b.x + a[1] * b.y + a[2] * b.z; }

// The camera aim as a world direction: heading atan2(x, y) (the camera's frame) and pitch above level.
inline Vec AimDir(double heading, double pitch) {
    return { std::sin(heading) * std::cos(pitch), std::cos(heading) * std::cos(pitch), std::sin(pitch) };
}

// The aim in the body frame (rows right, forward, up): `pitch` = its elevation toward the body's up axis (turn about the
// right axis, nose up +), `yaw` = its bearing toward the body's right (turn about the up axis, right +), full circle.
struct Error { double pitch = 0, yaw = 0; };
inline Error AimError(const float R[3], const float F[3], const float U[3], const Vec& aim) {
    const double r = Dot(R, aim), f = Dot(F, aim), u = Dot(U, aim);
    return { std::atan2(u, std::hypot(f, r)), std::atan2(r, f) };
}

// The roll rate for this hull (hulllogic Limits::roll is 1 for a REV-8-sized hull).
inline double Rate(double hullRoll) { return std::max(kMinRate, kRate * std::clamp(hullRoll, 0.0, 1.0)); }

// The roll rate wanted. A held key rolls at `rate`; released, the roll holds, unless assist is on and the roll is within
// kAssistBand. Near the ground (`nearGround`) a held key stops at kGroundMaxRoll and a released roll levels; landing always
// levels. Levelling goes to `lean` the short way round.
inline double RollWant(double roll, double rollIn, double rate, double levelGain, double lean, bool assist, bool nearGround, bool landing) {
    const double level = std::clamp(Wrap(lean - roll) * levelGain, -rate, rate);
    if (landing) return level;
    if (rollIn != 0.0) {
        if (nearGround && std::fabs(roll) >= kGroundMaxRoll) return rollIn * roll > 0.0 ? 0.0 : level;
        return std::clamp(rollIn, -1.0, 1.0) * rate;
    }
    if (nearGround || (assist && std::fabs(roll) < kAssistBand)) return level;
    return 0.0;
}

// The roll a held bank target asks for (the bank test, autopilot roll aims): proportional, the short way round.
inline double RollToward(double roll, double target, double gain, double rate) {
    return std::clamp(gain * Wrap(target - roll), -rate, rate);
}

// Body angular velocity (world vector) from body rates: pitch about right (nose up +), roll about forward (right side
// down +), yaw about up (turn right +, which is a negative turn about the up axis).
inline void Compose(const float R[3], const float F[3], const float U[3], double pitch, double roll, double yaw, float out[4]) {
    for (int i = 0; i < 3; ++i) out[i] = float(R[i] * pitch + F[i] * roll - U[i] * yaw);
    out[3] = 0.0f;
}
}
