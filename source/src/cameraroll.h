#pragma once
#include <algorithm>
#include <cmath>
#include <numbers>

namespace cameraroll
{
    constexpr double kPi = std::numbers::pi;
    struct Filter {
        double angle = 0;
        double Step(double target, double dt) {
            if (!std::isfinite(target) || !std::isfinite(dt) || dt <= 0) return angle;
            // Circular difference prevents 179 -> -179 degrees from unwinding through zero.
            angle += std::remainder(target - angle, 2 * kPi) * -std::expm1(-10 * std::min(dt, 0.1));
            angle = std::remainder(angle, 2 * kPi);
            return angle;
        }
    };
    // Native padded R/F/U rows. Bank about the existing forward, preserving forward and padding exactly.
    inline void Apply(float* rows, double bank) {
        if (!std::isfinite(bank)) return;
        const double c = std::cos(bank), s = std::sin(bank);
        for (int i = 0; i < 3; ++i) {
            const double r = rows[i], u = rows[8+i];
            rows[i] = float(c*r-s*u);
            rows[8+i] = float(s*r+c*u);
        }
    }
    // Aircraft reference stays fixed; horizon moves opposite bank and down for nose-up pitch.
    struct Horizon { double rotation = 0, x = 0, y = 0; };
    inline Horizon Indicator(double pitch, double bank) {
        if (!std::isfinite(pitch) || !std::isfinite(bank)) return {};
        const double offset = std::clamp(pitch * 180 / kPi * 2, -150.0, 150.0);
        return {-bank * 180 / kPi, std::sin(bank)*offset, std::cos(bank)*offset};
    }
}
