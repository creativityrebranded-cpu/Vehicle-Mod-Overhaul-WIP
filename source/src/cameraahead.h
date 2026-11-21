#pragma once
// Camera prediction (card CP-1, 2026-11-07; user: "also build camera prediction").
// Owner: World sensing (the camera is a source). Retirement reads it to decide "out of sight"; nothing here removes or moves anything.
// From two camera samples (origin + forward, with their times) the camera's turn and travel are extrapolated, and a point is
// "about to be in view" if it falls inside the view cone (plus a margin) at any of several moments up to `seconds` ahead.
// Pure math: no engine reads, so it is tested offline (tests/cameraahead_test.cpp).
#include <cmath>

namespace cameraahead
{
    struct Sample { double t = -1; double o[3]{}; double f[3]{}; };  // t in seconds; f a unit vector

    namespace rules {
        inline constexpr double maxGap = 0.30;    // older pair of samples than this: the turn is unknown
        inline constexpr double minGap = 0.03;    // closer than this: too noisy to read a turn from
        inline constexpr double marginDeg = 6.0;  // the predicted cone is this much wider than the real one
        inline constexpr int steps = 4;           // moments checked between now and `seconds` ahead
    }

    inline bool InCone(const double o[3], const double f[3], double x, double y, double z, double cosCone, double radius = 0)
    {
        const double cx = x - o[0], cy = y - o[1], cz = z - o[2], d = std::sqrt(cx * cx + cy * cy + cz * cz);
        if (d <= std::fmax(1.0, radius)) return true;
        if (radius > 0) {
            const double sinExtra = radius / d;
            cosCone = cosCone * std::sqrt(1 - sinExtra * sinExtra) - std::sqrt(1 - cosCone * cosCone) * sinExtra;
        }
        return (cx * f[0] + cy * f[1] + cz * f[2]) / d >= cosCone;
    }

    // Reason codes for the trace: 0 out of view now and ahead, 1 in view now, 2 comes into view ahead, 3 turn unknown (treated as seen).
    inline int Judge(const Sample& prev, const Sample& now, double x, double y, double z, double seconds, double coneDeg, double* turnDegPerS = nullptr, double radius = 0)
    {
        constexpr double kPi = 3.14159265358979;
        if (turnDegPerS) *turnDegPerS = 0;
        if (InCone(now.o, now.f, x, y, z, std::cos(coneDeg * kPi / 180.0), radius)) return 1;
        const double dt = now.t - prev.t;
        if (prev.t < 0 || !(dt >= rules::minGap) || dt > rules::maxGap) return 3;
        // Yaw and pitch rates, so a turn keeps its axis (a linear blend of forwards would shrink a fast turn).
        const double yaw0 = std::atan2(prev.f[1], prev.f[0]), yaw1 = std::atan2(now.f[1], now.f[0]);
        double dyaw = yaw1 - yaw0; dyaw = std::atan2(std::sin(dyaw), std::cos(dyaw));
        const double pitch0 = std::asin(std::fmax(-1.0, std::fmin(1.0, prev.f[2]))), pitch1 = std::asin(std::fmax(-1.0, std::fmin(1.0, now.f[2])));
        const double yawRate = dyaw / dt, pitchRate = (pitch1 - pitch0) / dt;
        if (turnDegPerS) *turnDegPerS = std::hypot(yawRate, pitchRate) * 180.0 / kPi;
        const double v[3] = { (now.o[0] - prev.o[0]) / dt, (now.o[1] - prev.o[1]) / dt, (now.o[2] - prev.o[2]) / dt };
        const double cosWide = std::cos((coneDeg + rules::marginDeg) * kPi / 180.0);
        for (int i = 0; i <= rules::steps; ++i) {  // i = 0: now, with the margin
            const double s = seconds * i / rules::steps;
            const double yaw = yaw1 + yawRate * s, pitch = std::fmax(-1.55, std::fmin(1.55, pitch1 + pitchRate * s));
            const double f[3] = { std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch) };
            const double o[3] = { now.o[0] + v[0] * s, now.o[1] + v[1] * s, now.o[2] + v[2] * s };
            if (InCone(o, f, x, y, z, cosWide, radius)) return 2;
        }
        return 0;
    }
    inline const char* Name(int judged) { return judged == 0 ? "out_now_and_ahead" : judged == 1 ? "in_view" : judged == 2 ? "comes_into_view" : "turn_unknown"; }
}
