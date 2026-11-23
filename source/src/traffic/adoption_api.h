#pragma once

#include <cstdint>

// Keep authority over the chassis separate from eligibility to supply a driver.
enum class FvoAdoptionOwner : std::uint32_t {
    Unclaimed = 0,
    PlayerSavedAuto = 1,
    PlayerProtected = 2,
    OtherProtected = 3,
    FvoDisposable = 4
};

using FvoAdoptionFactsFnV1 = int (*)(unsigned vehicle, unsigned base, FvoAdoptionOwner* owner);
using FvoAdoptionClaimFnV1 = int (*)(unsigned vehicle, unsigned base, int claim);

namespace trafficadoption {
inline bool MayClaim(FvoAdoptionOwner owner, bool loading, bool covered, bool occupied) {
    if (!covered || occupied) return false;
    return owner == FvoAdoptionOwner::Unclaimed || owner == FvoAdoptionOwner::FvoDisposable ||
        (loading && owner == FvoAdoptionOwner::PlayerSavedAuto);
}

inline bool MayRetireChassis(FvoAdoptionOwner owner) {
    return owner == FvoAdoptionOwner::FvoDisposable;
}
}
