#include "traffic/factorysite.h"
#include "traffic/factorydispatch_api.h"
#include "traffic/labpool.h"
#include "traffic/factoryfloat.h"
#include <cmath>

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

    // Dispatch reach: one named constant, 120 m by default, on each axis, plus the heading limit.
    {
        const FvoRoadSiteV1 at{100, 0, 0, 0.0, 3, 2};
        Check(factorydispatch::maxAxisShift == 120.0, "reach constant is 120 m");
        Check(factorydispatch::Reachable(0, 0, 0, 0.0, at), "100 m is reachable");
        FvoRoadSiteV1 edge = at; edge.x = 120.0;
        Check(factorydispatch::Reachable(0, 0, 0, 0.0, edge), "exactly the reach is reachable");
        edge.x = 120.5;
        Check(!factorydispatch::Reachable(0, 0, 0, 0.0, edge), "beyond the reach on x is refused");
        FvoRoadSiteV1 up = at; up.y = -121.0;
        Check(!factorydispatch::Reachable(0, 0, 0, 0.0, up), "beyond the reach on y is refused");
        FvoRoadSiteV1 high = at; high.z = 121.0;
        Check(!factorydispatch::Reachable(0, 0, 0, 0.0, high), "beyond the reach on z is refused");
        FvoRoadSiteV1 turned = at; turned.heading = 0.34;
        Check(factorydispatch::Reachable(0, 0, 0, 0.0, turned), "a small heading change is reachable");
        turned.heading = 0.5;
        Check(!factorydispatch::Reachable(0, 0, 0, 0.0, turned), "a large heading change is refused");
        turned.heading = 6.283185307179586 + 0.1;
        Check(factorydispatch::Reachable(0, 0, 0, 0.0, turned), "heading wraps around a full turn");
    }
    // Lab pool rules.
    {
        using labpool::Counts;
        Check(labpool::kPoolSize == 8 && labpool::kSlotCount == 16, "default pool 8 in 16 slots");
        Check(labpool::MayBirth(Counts{0, 0, 16}), "empty lab may birth");
        Check(!labpool::MayBirth(Counts{8, 0, 8}), "full pool may not birth");
        Check(!labpool::MayBirth(Counts{3, 13, 0}), "no empty slot may not birth");
        Check(labpool::MayBirth(Counts{4, 8, 4}), "stranded pairs do not count against the pool");
        Check(!labpool::NeedsRecycle(Counts{0, 8, 8}), "enough empty slots: no recycle");
        Check(labpool::NeedsRecycle(Counts{0, 16, 0}), "all slots stranded: recycle");
        Check(labpool::NeedsRecycle(Counts{2, 10, 4}), "short of room for the pool: recycle");
        Check(!labpool::NeedsRecycle(Counts{8, 8, 0}), "pool full: no recycle");
        Check(!labpool::NeedsRecycle(Counts{3, 0, 0}), "nothing stranded: no recycle");
        Check(!labpool::InRing(10) && labpool::InRing(25) && labpool::InRing(60) && labpool::InRing(110) && !labpool::InRing(111),
              "density ring edges");
        Check(!labpool::InRing(std::nan("")), "non-finite distance is outside the ring");
        Check(labpool::kRingOuterM <= factorydispatch::maxAxisShift, "ring fits inside the reach");
        Check(labpool::NearestFirst(30, 40) && !labpool::NearestFirst(40, 30), "nearest slot first");
        Check(labpool::YieldBackoffMs(0) == 0 && labpool::YieldBackoffMs(1) == 15000 && labpool::YieldBackoffMs(2) == 30000 &&
              labpool::YieldBackoffMs(3) == 60000 && labpool::YieldBackoffMs(50) == 60000, "yield backoff grows and caps");
    }

    std::printf("factorysite_test: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
