#include "traffic/factorysite.h"

#include <cstdio>
#include <string_view>

namespace {
int failures = 0;

void Check(bool pass, const char* what) {
    std::printf("%s %s\n", pass ? "PASS" : "FAIL", what);
    if (!pass) ++failures;
}

constexpr std::string_view manifest = R"ini([file]
enabled = 0

[factory.fixture_bay]
enabled = 1
world = TestWorld
vehicle = FE007810
pose = 100 100 30 0
private_route = fixture_exit
private_lane = 0
public_route = public_road
public_lane = 0
link = fixture_merge

[route.fixture_exit]
type = path
world = testworld
travel = drive
lanes = 1
points = 100 95 30, 100 105 30, 100 115 30

[link.fixture_merge]
type = exit
from = fixture_exit
from_lane = 0
at = 2
to = public_road
to_lane = 0
to_at = 0
)ini";

unsigned RemapFixture(unsigned id) { return id == 0xFE007810 ? 0xFE008810 : id; }

trafficdata::Data TestRoutes() {
    trafficdata::Data data;
    trafficdata::Read(std::string(manifest), "FactoryBay.ini", data, true);
    trafficdata::Read(R"ini([route.public_road]
type = path
world = testworld
travel = drive
lanes = 1
points = 100 115 30, 100 145 30
)ini", "public.ini", data);
    return data;
}

std::string Replace(std::string_view before, std::string_view after) {
    std::string ini(manifest);
    ini.replace(ini.find(before), before.size(), after);
    return ini;
}

bool Reason(const factorysite::ParseResult& result, std::string_view reason) {
    return result.recognized && !result.enabled && result.reason == reason;
}

bool Reason(const char* result, std::string_view reason) {
    return result && result == reason;
}
}

int main() {
    Check(!factorysite::Parse({}).recognized, "missing file leaves existing traffic unchanged");
    Check(!factorysite::Parse("[route.ordinary]\ntype=path").recognized,
          "ordinary file with the same filename retains its legacy path");
    Check(Reason(factorysite::Parse("[file]\nenabled=0\n[factory.template]\nenabled=0"), "factory_disabled"),
          "disabled template requires no actual coordinates or reference ID");
    Check(Reason(factorysite::Parse(Replace("enabled = 0", "enabled = 1")), "legacy_file_must_be_disabled"),
          "older addons cannot see an enabled factory's private route");
    Check(Reason(factorysite::Parse(std::string(manifest) + "\n[factory.second]\nenabled=1\n"), "multiple_factory_bays"),
          "ambiguous manifests cannot enable two prototype bays");
    Check(Reason(factorysite::Parse(Replace("FE007810", "FF007810")), "invalid_placed_vehicle_id"),
          "a save-created reference is not an authored placement");
    Check(Reason(factorysite::Parse(Replace("100 100 30 0", "100 100 inf 0")), "invalid_pose_xyz_heading_radians"),
          "non-finite bay pose is rejected");
    Check(Reason(factorysite::Parse(Replace("[link.fixture_merge]", "[link.missing]")),
                 "manifest_requires_only_private_route_and_exit"),
          "the private exit must live in the dedicated file");

    trafficdata::idAdapter = RemapFixture;
    const auto result = factorysite::Parse(manifest);
    trafficdata::idAdapter = nullptr;
    Check(result.enabled && result.config.key == "fixture_bay" && result.config.vehicle == 0xFE008810,
          "valid bay ID follows the save profile's existing load-order remap");
    trafficdata::Data ordinary;
    trafficdata::Read(std::string(manifest), "FactoryBay.ini", ordinary);
    Check(ordinary.routes.empty() && ordinary.links.empty() && ordinary.problems.empty(),
          "legacy route reader ignores the private definitions");

    const auto& bay = result.config;
    const auto world = "TestWorld";
    auto data = TestRoutes();
    Check(data.problems.empty() && !factorysite::Validate(bay, data, world),
          "an authored forward private lane reaches the designated public lane");
    Check(Reason(factorysite::Validate(bay, data, "otherworld"), "wrong_factory_world"), "bay is scoped to its own world");
    auto badPose = bay;
    badPose.pose.z += 10.0;
    Check(Reason(factorysite::Validate(badPose, data, world), "factory_pose_outside_private_lane"),
          "the same X/Y on a different floor cannot be used as the bay");

    data.links[0].type = "crossing";
    Check(Reason(factorysite::Validate(bay, data, world), "factory_requires_explicit_directed_exit"),
          "crossing does not authorize transfer");
    data = TestRoutes();
    data.links[0].fromLane = -1;
    Check(Reason(factorysite::Validate(bay, data, world), "factory_requires_explicit_directed_exit"),
          "link must explicitly select both lanes");
    data = TestRoutes();
    data.links[0].at = 0;
    Check(Reason(factorysite::Validate(bay, data, world), "factory_exit_unreachable_on_private_lane"),
          "a link behind the one-way start does not supply a drive-out");
    data = TestRoutes();
    data.routes[1].points[0].z += 1;
    Check(Reason(factorysite::Validate(bay, data, world), "factory_exit_geometry_disconnected"),
          "the exit and arrival must meet at the same elevation");
    data = TestRoutes();
    data.routes[1].travel = routepolicy::Travel::Hover;
    Check(Reason(factorysite::Validate(bay, data, world), "factory_exit_requires_wheel_surface"),
          "a hover-only arrival cannot be published as a wheel drive-out");
    data = TestRoutes();
    data.routes[0].road.enabled = true;
    data.routes[0].road.primitives = {{roadplan::Flat, 0}, {roadplan::Open, 0}, {roadplan::Flat, 0}};
    Check(Reason(factorysite::Validate(bay, data, world), "private_route_has_open_air_tile"),
          "private wheel lane cannot cross an open-air tile");

    std::printf("factorysite_test: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
