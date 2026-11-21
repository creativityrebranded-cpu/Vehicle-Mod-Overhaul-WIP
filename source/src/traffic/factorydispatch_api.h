#pragma once

#include "birth_api.h"
#include "../fvo_addon_api.h"
#include <cmath>

enum class FvoStockTransferActionV1 { Check, Start, Promote };
enum class FvoStockTransferResultV1 { Pending, Refused, Ready, Armed, Promoted };

struct FvoStockTransferV1 {
    unsigned size = sizeof(FvoStockTransferV1), version = 1;
    unsigned vehicle = 0, driver = 0;
    std::uint64_t lease = 0, serial = 0;
    FvoRoadSiteV1 target{};
};

using FvoStockTransferFnV1 = FvoStockTransferResultV1 (*)(FvoStockTransferActionV1, const FvoStockTransferV1*,
                                                         std::uint64_t*, const FvoTripClaim*);

namespace factorydispatch {
// One reach for the stock-to-road move on each axis, in metres. The lab sits under the player's cell and the density ring (labpool.h) is
// narrower than this, so every route slot in the ring is inside the reach. The core's own check (api_traffic.cpp) uses the same Reachable().
inline constexpr double maxAxisShift = 120.0;
inline constexpr double maxHeadingDelta = .35;
inline bool Reachable(double x,double y,double z,double heading,const FvoRoadSiteV1& target) {
    constexpr double turn=6.283185307179586;
    return std::abs(target.x-x)<=maxAxisShift&&std::abs(target.y-y)<=maxAxisShift&&
        std::abs(target.z-z)<=maxAxisShift&&std::abs(std::remainder(target.heading-heading,turn))<=maxHeadingDelta;
}
}
