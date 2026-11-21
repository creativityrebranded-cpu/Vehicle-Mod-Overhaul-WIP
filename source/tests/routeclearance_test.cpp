#include "routeclearance.h"
#include "routeeditor.h"
#include "routefiles.h"
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
    // An intentional low indoor route survives the real editor/addon format,
    // including edits and undo, without a clearance warning rewriting coordinates.
    routefiles::File file;
    file.worldId = "JemisonPlanetWorld";
    routefiles::Route route;
    route.id = "indoor_legacy";
    route.closed = false;
    route.points = {{10, 20, -20}, {110, 20, -20}};
    file.routes.push_back(route);
    trafficdata::Data data;
    trafficdata::Read(routefiles::Format(file), "Editor_indoor.ini", data);
    check(data.problems.empty() && data.routes.size() == 1, "indoor route loads through the addon reader in the existing format");
    if (data.routes.size() == 1) {
        routeeditor::Draft draft(data.routes.front());
        check(draft.Check() == routeeditor::Issue::None && draft.Value().points[0].z == -20,
              "save validation accepts an intentional indoor route without changing its height");
        check(draft.SetZ(0, -15) && draft.Undo() && draft.Value().points[0].z == -20,
              "indoor route editing and undo retain authored heights");
        const auto& p = draft.Value().points[0];
        const auto warning = Judge(p.x, p.y, p.z, true, -50, true);
        check(warning.block == Block::None && warning.structureWarning && p.z == -20,
              "structure warning preserves the authored route above confirmed terrain");
    }
    std::cout << "route clearance: " << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
