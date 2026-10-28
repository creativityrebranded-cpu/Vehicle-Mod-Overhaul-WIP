#include "trafficdata.h"
#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }

int main()
{
    using namespace trafficdata;
    {
        Data d;
        Read(R"(; comment
[file]
name = test
[world.NewAtlantis]
id = JemisonPlanetWorld157133511011091040
[route.ring]
world = NewAtlantis
type = ring
centre = -858.7 1567.4
height = 143.5
radius = 600
lane_gap = 40
speed = 25
[route.run]
world = NewAtlantis
type = path
points = 0 0 100, 100 0 100, 200 50 120
lanes = 2
width = 20
[exit.east]
route = ring
at = 90
[place.wells]
world = NewAtlantis
point = -620 1340 -158
radius = 40
near = east
[traffic.NewAtlantis]
world = NewAtlantis
max = 4
gap = 20
vehicles = FE001033, FE00780B
trips = mixed
start_on_route = 3
)", "a.ini", d);
        Check(d.problems.empty(), "a clean file has no problems");
        Check(d.routes.size() == 2 && d.routes[0].ring && !d.routes[1].ring, "a ring and a path");
        Check(std::fabs(d.routes[0].shape.z - 143.5) < 1e-9 && std::fabs(d.routes[0].shape.gap - 40) < 1e-9, "ring height and gap");
        Check(d.routes[1].points.size() == 3 && d.routes[1].points[2].z == 120, "path points");
        Check(d.exits.size() == 1 && d.exits[0].route == "ring" && d.exits[0].at == 90, "exit");
        Check(d.places.size() == 1 && d.places[0].exitName == "east" && d.places[0].at.z == -158, "place");
        Check(d.traffic.size() == 1 && d.traffic[0].max == 4 && d.traffic[0].trips == Trips::Mixed && d.traffic[0].bases.size() == 2 &&
            d.traffic[0].bases[0] == 0xFE001033 && d.traffic[0].startOnRoute == 3, "traffic rules");
        Check(SameWorld(d, "newatlantis", "JemisonPlanetWorld157133511011091040"), "world names map to ids");
        Check(!SameWorld(d, "newatlantis", "Akila"), "another world does not match");
    }
    {
        Data d;
        Read("[route.bad]\ntype = road\n[place.p]\npoint = 1 2\nhello\n[traffic.t]\ntrips = sometimes\n", "b.ini", d);
        Check(d.routes.empty() && d.places.empty(), "bad sections are skipped");
        Check(d.problems.size() == 4, "each problem is named (type, point, stray line, trips)");
        Check(d.traffic.size() == 1 && d.traffic[0].trips == Trips::Laps, "a bad value keeps the default");
    }
    {
        Data d;
        Read("[file]\nenabled = 0\n[route.r]\ncentre = 0 0\n", "off.ini", d);
        Check(d.routes.empty(), "a switched-off file adds nothing");
    }
    {
        Data d;
        AddRingroad("JemisonPlanetWorld1", ringlogic::Format(ringlogic::Default(10, 20, 0)), d);
        Check(d.routes.size() == 1 && d.routes[0].name == "ringroad" && SameWorld(d, d.routes[0].world, "JemisonPlanetWorld1"), "the editor's ring is a route in its world");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
