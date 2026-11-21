#pragma once
#include "cameraahead.h"
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <array>

namespace cameravisibility {
enum class Evidence : int { Unknown, Outside, MouseTurn, Waiting, Visible, Occluded };
using Reader = Evidence (*)(unsigned, unsigned, double, double, double, bool);

namespace rules {
inline constexpr std::uint64_t mouseMs = 120, launchMs = 100, sampleMs = 290, rayMs = 180;
inline constexpr unsigned sampleCount = 5, cacheSize = 8;
inline constexpr int mouseThreshold = 2;
inline constexpr double fringeDeg = 12, maxRange = 350, drift = .45;
inline constexpr double originGuard = 1, targetGuard = .5;
inline constexpr double fallbackRadius = 8, fallbackHeight = 6, driverRadius = .6, driverHeight = 1;
inline constexpr double mouseMultiplier = 1.25, maxRate = 12, maxGain = .08;
inline constexpr unsigned retireMs = 350, entryStepMs = 15, healthMs = 450, healthMinIntervalMs = 300, healthBudget = 2;
}

struct Mouse { std::int64_t x = 0, y = 0; std::uint64_t at = 0; };
struct Forecast {
    std::uint64_t at = 0, mouseAt = 0;
    double origin[3]{}, forward[3]{}, yawRate = 0, pitchRate = 0, coneDeg = 60;
};
using ForecastReader = int (*)(Forecast*);

inline bool Fresh(std::uint64_t at, std::uint64_t now, std::uint64_t limit) {
    return at && at <= now && now - at <= limit;
}
inline double Yaw(const double f[3]) { return std::atan2(f[1], f[0]); }
inline double Pitch(const double f[3]) { return std::asin(std::clamp(f[2], -1.0, 1.0)); }
inline double Wrap(double angle) { return std::atan2(std::sin(angle), std::cos(angle)); }
inline double MouseRate(double delta, double counts, double seconds, double& gain) {
    if (std::abs(counts) >= rules::mouseThreshold && std::abs(delta) > .001) {
        const double sample = delta / counts;
        if (std::abs(sample) <= rules::maxGain) {
            gain = gain * sample > 0 ? gain * .75 + sample * .25 : sample;
        }
    }
    const double rate = gain && std::abs(counts) >= rules::mouseThreshold ? gain * counts / seconds : delta / seconds;
    return std::clamp(rate * rules::mouseMultiplier, -rules::maxRate, rules::maxRate);
}
struct Projection {
    struct Sample { std::array<double,3> forward; unsigned ms; };
    std::array<Sample, rules::healthMs / rules::entryStepMs + 2> samples{};
    double origin[3]{}, cosCone = 0;
    unsigned count = 0;
};
inline Projection Project(const Forecast& look, unsigned horizon) {
    constexpr double rad = 3.14159265358979 / 180.0;
    Projection projection;
    std::copy_n(look.origin, 3, projection.origin);
    projection.cosCone = std::cos(look.coneDeg * rad);
    projection.samples[projection.count++] = {{look.forward[0], look.forward[1], look.forward[2]}, 0};
    if (!look.yawRate && !look.pitchRate) return projection;
    horizon = std::min(horizon, rules::healthMs);
    const auto yaw = Yaw(look.forward), pitch = Pitch(look.forward);
    for (unsigned ms = 0; ms < horizon;) {
        ms = std::min(ms + rules::entryStepMs, horizon);
        const double y1 = yaw + look.yawRate * (ms * .001);
        const double p1 = std::clamp(pitch + look.pitchRate * (ms * .001), -1.55, 1.55);
        projection.samples[projection.count++] = {{std::cos(p1) * std::cos(y1), std::cos(p1) * std::sin(y1), std::sin(p1)}, ms};
    }
    return projection;
}
inline int EntryMs(const Projection& p, double x, double y, double z, double radius) {
    for (unsigned i = 0; i < p.count; ++i) {
        const auto& sample = p.samples[i];
        if (cameraahead::InCone(p.origin, sample.forward.data(), x, y, z, p.cosCone, radius)) return int(sample.ms);
    }
    return -1;
}
inline int EntryMs(const Forecast& look, double x, double y, double z, double radius, unsigned horizon) {
    return EntryMs(Project(look,horizon),x,y,z,radius);
}
inline bool AdvanceHealth(int eta, std::uint64_t since, std::uint64_t normalInterval) {
    return eta > 0 && since >= rules::healthMinIntervalMs &&
        since < normalInterval && std::uint64_t(eta) < normalInterval - since;
}
inline Evidence RayEvidence(bool answered, bool hit, double distance, double length, double hull) {
    if (!answered || !std::isfinite(length) || length <= 0) return Evidence::Waiting;
    if (!hit) return Evidence::Visible;
    if (!std::isfinite(distance) || distance <= rules::originGuard || distance > length + rules::targetGuard)
        return Evidence::Waiting;
    return distance < length - hull - rules::targetGuard ? Evidence::Occluded : Evidence::Visible;
}
inline Evidence Combine(unsigned blocked, unsigned visible) {
    if (visible) return Evidence::Visible;
    return blocked == rules::sampleCount ? Evidence::Occluded : Evidence::Waiting;
}
inline const char* HoldReason(Evidence evidence) {
    switch (evidence) {
    case Evidence::Outside:
    case Evidence::Occluded: return nullptr;
    case Evidence::MouseTurn: return "camera_mouse_turn";
    case Evidence::Visible: return "camera_ray_visible";
    case Evidence::Waiting: return "camera_ray_waiting";
    default: return "camera_evidence_unknown";
    }
}
}
