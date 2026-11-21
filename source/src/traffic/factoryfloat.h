#pragma once
// Lab site height rule (pure; no engine calls). A floating stock pair is held by the hover at a commanded absolute height,
// so its site needs NO supported footing. The height is chosen from the code:
//   * one measured dry support ray below the public road surface, when it answers in time and the hull fits above it
//     (measured z, as before: the support point is only evidence, never a requirement);
//   * otherwise the float height: the hull top sits layerGap (+ a margin) under the public road surface.
// A solid inside the float hull's own envelope is refused (Blocked). Clear-volume and concealment checks stay in the caller.
#include "factorybirth_api.h"
#include <cmath>

namespace factoryfloat {
inline constexpr double floatMargin = 0.5;      // extra metres between the hull top and the layer gap
inline constexpr double probeDepth = 120.0;     // same reach the old 9-ray support probe had
inline constexpr double supportLift = 0.1;      // reference origin above the measured support (as environment::SpawnSite)
inline constexpr double levelNormalZ = 0.7;     // as environment::CheckFootprint
inline constexpr unsigned long long supportWaitMs = 500;  // an unanswered support ray stops blocking after this

// FvoFactorySiteProofV1::flags. Tagged so an older core that never wrote the field (padding bytes) is not misread.
inline constexpr unsigned flagsMagic = 0xF1000000u, flagsTagMask = 0xFF000000u;
inline constexpr unsigned flagFloatSite = flagsMagic | 1u;
inline bool IsFloatSite(unsigned flags) { return (flags & flagsTagMask) == flagsMagic && (flags & 1u) != 0; }

struct Probe {
    bool answered = false, hit = false, water = false, normalOk = false;
    double z = 0, nz = 1;
};
enum class Kind { Wait, Supported, Float, Blocked };
struct Pick {
    Kind kind = Kind::Wait;
    double z = 0;       // reference-origin height to use for the site (valid unless Wait/Blocked)
    const char* why = "support_ray_pending";
};

inline double FloatZ(double ceiling, double top) { return ceiling - factorybirth::rules::layerGap - top - floatMargin; }
inline double SupportZ(double groundZ, double bottom) { return groundZ - bottom + supportLift; }

inline Pick Decide(const Probe& probe, double ceiling, double bottom, double top, unsigned long long waitedMs) {
    const double floating = FloatZ(ceiling, top);
    if (!std::isfinite(ceiling) || !std::isfinite(bottom) || !std::isfinite(top) || top <= bottom)
        return {Kind::Blocked, 0, "bad_bounds_or_ceiling"};
    if (!probe.answered) {
        if (waitedMs < supportWaitMs) return {Kind::Wait, 0, "support_ray_pending"};
        return {Kind::Float, floating, "support_ray_unanswered_floating"};
    }
    if (!probe.hit || probe.water || !std::isfinite(probe.z)) return {Kind::Float, floating, "no_dry_support_floating"};
    // The measured surface against the float hull's envelope.
    if (probe.z >= floating + bottom && probe.z <= floating + top) return {Kind::Blocked, 0, "solid_inside_float_envelope"};
    if (probe.z > floating + top) return {Kind::Float, floating, "slab_above_hull_floating"};
    if (probe.normalOk && probe.nz < levelNormalZ) return {Kind::Float, floating, "steep_support_floating"};
    const double supported = SupportZ(probe.z, bottom);
    if (supported + top >= ceiling - factorybirth::rules::layerGap) return {Kind::Float, floating, "hull_would_cross_layer_gap"};
    return {Kind::Supported, supported, "measured_support"};
}
}  // namespace factoryfloat
