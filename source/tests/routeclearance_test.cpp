#include "routeclearance.h"
#include <iostream>
#include <limits>

int main() {
    using namespace routeclearance;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* why) { ++checks; if (!ok) { ++failures; std::cerr << "FAIL " << why << '\n'; } };
    const auto interior = Judge(1, 2, 30, true, 0, true);
    check(interior.block == Block::None && interior.structureWarning, "building intersection allows the authored route and warns");
    const auto unknownInterior = Judge(1, 2, 30, false, 0, true);
    check(unknownInterior.block == Block::None && unknownInterior.structureWarning, "mapped building is advisory when terrain is unavailable");
    const auto lowerInterior = Judge(1, 2, -20, false, 0, true);
    check(lowerInterior.block == Block::None && lowerInterior.structureWarning, "negative-height building interior is not automatically refused when terrain is unknown");
    check(Judge(1, 2, 30, true, 0, false).block == Block::None, "clear points remain allowed");
    check(!Judge(1, 2, 30, true, 0, false).structureWarning, "clear points have no false structure warning");
    check(Judge(1, 2, -10, true, 0, false).block == Block::UnderTerrain, "known underground points retain terrain validation");
    check(Judge(1, 2, -10, true, 0, true).block == Block::UnderTerrain, "building overlap does not hide a proven terrain violation");
    check(Judge(1, 2, -10, false, 0, false).block == Block::BelowUnknownFloor, "unknown negative terrain without a building retains its prior floor guard");
    check(Judge(1, 2, std::numeric_limits<double>::quiet_NaN(), false, 0, true).block == Block::InvalidPosition, "invalid coordinates are still rejected");
    std::cout << "route clearance: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
