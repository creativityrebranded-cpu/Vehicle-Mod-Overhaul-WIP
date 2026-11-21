#pragma once

#include "birth_api.h"

// Optional diagnostic stock path. Input site Z is the Route draft altitude.
// Ready returns the measured reference-origin pose to use verbatim for the
// arbiter's birth request. An unknown or expired observation cannot create.
enum class FvoFactorySiteStatusV1 : int {
    Pending, NoSupport, Occupied, Visible, WrongCell, Unsafe, Ready
};

struct FvoFactorySiteProofV1 {
    unsigned size = sizeof(FvoFactorySiteProofV1), version = 1, base = 0;
    FvoRoadSiteV1 site{};
    std::uint64_t nonce = 0, at = 0;
};

using FvoFactorySiteFnV1 = FvoFactorySiteStatusV1 (*)(unsigned, const FvoRoadSiteV1*, FvoFactorySiteProofV1*);
using FvoFactoryBirthFnV1 = unsigned (*)(const FvoTrafficBirthV1*, const FvoFactorySiteProofV1*, FvoBirthState*);
using FvoFactoryStockBindFnV1 = int (*)(unsigned, unsigned, const FvoRoadSiteV1*);

namespace factorybirth::rules {
inline constexpr double layerGap = 8.0;
// Existing SpawnSite rays start 6 m above the supplied probe origin.
inline constexpr double probeTopOffset = 6.0;
inline constexpr double hullMargin = 1.0;
inline constexpr std::uint64_t liveProofMs = 250, coveredProofMs = 2000;
inline constexpr double maxPlayerDrift = .45;
inline constexpr double maxStockPoseDrift = .25;
}
