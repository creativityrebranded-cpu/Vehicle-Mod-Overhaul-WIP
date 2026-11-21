#pragma once

#include "birth_api.h"

#include <cstddef>

// Optional diagnostic stock path. Input site Z is the Route draft altitude.
// Ready returns the measured reference-origin pose to use verbatim for the
// arbiter's birth request. An unknown or expired observation cannot create.
enum class FvoFactorySiteStatusV1 : int {
    Pending, NoSupport, Occupied, Visible, WrongCell, Unsafe, Ready
};

struct FvoFactorySiteProofV1 {
    // `flags` occupies the 4 bytes that were alignment padding before the doubles: sizeof/offsets/version are unchanged,
    // so older add-ons and cores keep passing and checking the same struct. Read it only through
    // factoryfloat::IsFloatSite (tagged value; padding written by an older core never matches).
    unsigned size = sizeof(FvoFactorySiteProofV1), version = 1, base = 0, flags = 0;
    FvoRoadSiteV1 site{};
    std::uint64_t nonce = 0, at = 0;
};
static_assert(sizeof(FvoFactorySiteProofV1) == 80 && offsetof(FvoFactorySiteProofV1, site) == 16 &&
              offsetof(FvoFactorySiteProofV1, nonce) == 64, "FvoFactorySiteProofV1 layout is frozen");

using FvoFactorySiteFnV1 = FvoFactorySiteStatusV1 (*)(unsigned, const FvoRoadSiteV1*, FvoFactorySiteProofV1*);
using FvoFactoryBirthFnV1 = unsigned (*)(const FvoTrafficBirthV1*, const FvoFactorySiteProofV1*, FvoBirthState*);
using FvoFactoryStockBindFnV1 = int (*)(unsigned, unsigned, const FvoRoadSiteV1*);
using FvoFactoryStockObserveFnV1 = FvoFactorySiteStatusV1 (*)(unsigned, unsigned, const FvoRoadSiteV1*);


namespace factorybirth::rules {
inline constexpr double layerGap = 8.0;
// Existing SpawnSite rays start 6 m above the supplied probe origin.
inline constexpr double probeTopOffset = 6.0;
inline constexpr double hullMargin = 1.0;
inline constexpr std::uint64_t liveProofMs = 250, coveredProofMs = 2000;
inline constexpr std::uint64_t stockBirthGapMs = 300;
inline constexpr std::uint64_t leaseProofLimitMs = 20000;
inline constexpr double maxPlayerDrift = .45;
inline constexpr double maxStockPoseDrift = .25;
}
