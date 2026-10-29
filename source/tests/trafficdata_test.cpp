#include "trafficdata.h"
#include <cstdio>
#include <fstream>
#include <sstream>

static int g_fail = 0;
static void Check(bool ok, const char* what) { if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }

int main(int argc, char** argv)
{
    using namespace trafficdata;
    if (argc > 1) {  // trafficdata_test FILE.ini ...: read real traffic files and list what was understood and what was skipped
        Data d;
        for (int i = 1; i < argc; ++i) { std::ifstream f(argv[i]); std::stringstream t; t << f.rdbuf(); Read(t.str(), argv[i], d); }
        std::printf("%d file(s): %zu route(s), %zu exit(s), %zu place(s), %zu rule(s), %zu anchored world(s)\n", d.files, d.routes.size(),
            d.exits.size(), d.places.size(), d.traffic.size(), d.anchors.size());
        for (const auto& r : d.routes) std::printf("  route %s in %s: %zu point(s)\n", r.name.c_str(), r.world.c_str(), r.points.size());
        for (const auto& p : d.problems) std::printf("  skipped %s\n", p.c_str());
        return d.problems.empty() ? 0 : 1;
    }
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
    {
        Data d;
        Read("[world.akila]\nanchor = 0027D001 10 20 30, 0027D002 -5 6 7\n"
             "[route.loop]\nworld = akila\ntype = path\npoints = 0 0 100, 50 0 110\nclosed = 1\n"
             "[route.ring]\nworld = akila\ncentre = 1 2\nheight = 50\n"
             "[place.pad]\nworld = akila\npoint = 5 5 40\n[spawn.s1]\nworld = akila\npoint = 1 1 41\nheading = 90\n"
             "[traffic.akila]\nworld = akila\nstart_on_route = 4\n", "akila.ini", d);
        Check(d.problems.empty(), "an anchored world needs no id");
        Check(d.anchors.count("akila") && d.anchors["akila"].size() == 2 && d.anchors["akila"][1].id == 0x0027D002 &&
            d.anchors["akila"][1].at.x == -5, "anchors read");
        Check(!SameWorld(d, "akila", "CheyennePlanetWorld9"), "not active until an anchor is found");
        PlaceWorld(d, "akila", "CheyennePlanetWorld9", { 300, -400, -20 });
        Check(SameWorld(d, "akila", "CheyennePlanetWorld9"), "found: the player's world is the city's");
        Check(d.routes[0].points[1].x == 350 && d.routes[0].points[1].y == -400 && d.routes[0].points[1].z == 90, "path moved into game coordinates");
        Check(d.routes[1].shape.cx == 301 && d.routes[1].shape.cy == -398 && d.routes[1].shape.z == 30, "ring moved");
        Check(d.places[0].at.x == 305 && d.places[0].at.z == 20, "place moved");
        Check(d.spawns.size() == 1 && d.spawns[0].heading == 90 && d.spawns[0].at.x == 301 && d.spawns[0].at.z == 21, "spawn spot read and moved");
        Check(d.traffic[0].startOnRoute == 4, "pool size");
        Data t;
        Read("[route.r]\ncentre = 0 0\nheight = 100\ntilt = 8\ntilt_toward = 200\n", "t.ini", t);
        Check(t.routes.size() == 1 && t.routes[0].tilt == 8 && t.routes[0].tiltToward == 200, "ring tilt read");
        Data e;
        Read("[world.x]\nanchor = 0027D001 10 20\n", "x.ini", e);
        Check(e.anchors.empty() && e.problems.size() == 2, "a bad anchor is named and the world is skipped");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
