#pragma once
// The baked terrain grid of cellbake (2026-10-29): one terrain height every kStep metres, filled once per loaded cell with the same
// terrain probe the pilots ask for (terrain layers, cast from high above), read back with a bilinear blend of the four points around
// (x, y). Pure: tests/cellbake_test.cpp.
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace cellbakegrid {
constexpr double kStep = 8.0;   // m between baked points
inline std::int64_t Key(std::int32_t ix, std::int32_t iy) { return (std::int64_t(ix) << 32) ^ std::int64_t(std::uint32_t(iy)); }
inline std::int32_t Index(double v) { return std::int32_t(std::floor(v / kStep)); }

struct Grid {
    std::unordered_map<std::int64_t, float> h;
    void Put(std::int32_t ix, std::int32_t iy, float z) { h[Key(ix, iy)] = z; }
    bool Has(std::int32_t ix, std::int32_t iy) const { return h.count(Key(ix, iy)) != 0; }
    bool At(std::int32_t ix, std::int32_t iy, float& z) const
    {
        const auto it = h.find(Key(ix, iy));
        if (it == h.end()) return false;
        z = it->second;
        return true;
    }
    // The terrain height at (x, y): the blend of the four baked points around it; with a corner missing, the nearest baked point when it
    // lies within half a step. False when nothing is baked near.
    bool Get(double x, double y, double& z) const
    {
        const std::int32_t ix = Index(x), iy = Index(y);
        float a, b, c, d;
        if (At(ix, iy, a) && At(ix + 1, iy, b) && At(ix, iy + 1, c) && At(ix + 1, iy + 1, d)) {
            const double fx = x / kStep - ix, fy = y / kStep - iy;
            z = (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
            return true;
        }
        const std::int32_t nx = std::int32_t(std::lround(x / kStep)), ny = std::int32_t(std::lround(y / kStep));
        float n;
        if (At(nx, ny, n) && std::hypot(x - nx * kStep, y - ny * kStep) <= kStep * 0.5) { z = n; return true; }
        return false;
    }
    void Clear() { h.clear(); }
};

// How well the baked grid agrees with live terrain probes at the same places.
struct Agreement {
    unsigned samples = 0, close = 0, off2 = 0;  // within 0.5 m, more than 2 m apart
    double sumAbs = 0;
    void Add(double baked, double live)
    {
        const double d = std::fabs(baked - live);
        ++samples; sumAbs += d;
        if (d <= 0.5) ++close;
        if (d > 2.0) ++off2;
    }
    double Mean() const { return samples ? sumAbs / samples : 0.0; }
};
}
