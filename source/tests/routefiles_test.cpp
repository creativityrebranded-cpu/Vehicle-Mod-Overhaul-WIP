#include "routefiles.h"
#include "trafficroute.h"
#include <cmath>
#include <cstdio>

int main()
{
    using namespace routefiles;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };
    check(Slug("City Loop #2!") == "city_loop_2" && Slug("  ") == "route", "slug");
    check(FileName("JemisonPlanetWorld157133511011091040") == "Editor_jemisonplanetworld157133511011091040.ini", "file name");
    std::set<std::string> taken{ "ed_city_loop", "ed_city_loop_2" };
    check(NewId("City loop", taken) == "ed_city_loop_3" && NewId("Night", taken) == "ed_night", "unique ids");
    check(CleanLabel("a;b\n[c]=d") == "a b  c  d", "label cannot break the file");

    File f;
    f.worldId = "JemisonPlanetWorld";
    Route a; a.id = "ed_city_loop"; a.label = "City loop"; a.closed = true; a.lanes = 2; a.speed = 22.5; a.color = 0x6FE0D0;
    a.points = { {-877, 1604, 20}, {-713.25, 1672, 35.5}, {-797, 1549, 60} };
    Route b; b.id = "ed_night"; b.label = "Night route"; b.closed = false; b.lanes = 1; b.test = true;
    b.points = { {0, 0, 100}, {300, 0, 140} };
    b.schedule.allDay = false; b.schedule.windows = { {18 * 60, 6 * 60, 1}, {6 * 60, 7 * 60, 0} };
    f.routes = { a, b };
    f.landings = { {"ed_pad_1", {-860, 1600, -161}, 0, 25, true} };
    f.spawns = { {"ed_spawn_1", {-850, 1590, -161}, 90, 0} };
    f.links = { {"ed_link_1", "ringroad", "ed_night", 135, false, 0.25}, {"ed_link_2", "ringroad", "ed_night", 200, true, 0} };
    const std::string text = Format(f);

    // The addon's own reader sees what the editor wrote.
    trafficdata::Data d;
    trafficdata::Read(text, "Editor_x.ini", d);
    check(d.problems.empty(), "no reader problems");
    check(d.routes.size() == 2 && d.places.size() == 1 && d.spawns.size() == 1 && d.traffic.size() == 1, "every section read");
    check(trafficdata::SameWorld(d, d.routes[0].world, "jemisonplanetworld"), "routes are in the editor's world");
    check(d.routes[0].points.size() == 3 && d.routes[0].points[1].x == -713.25 && d.routes[0].points[1].z == 35.5, "points keep their own height");
    check(d.routes[0].closed && d.routes[0].lanes == 2 && d.routes[0].speed == 22.5, "settings");
    check(d.routes[1].test && !d.routes[0].test && d.routes[1].label == "Night route", "label and test");
    check(d.routes[0].color == 0x6FE0D0 && d.routes[1].color == 0, "a route's colour is kept, and none is written when it has none");
    check(!d.routes[1].schedule.allDay && d.routes[1].schedule.windows.size() == 2 &&
          routeschedule::Evaluate(d.routes[1].schedule, true, 23 * 60).active && !routeschedule::Evaluate(d.routes[1].schedule, true, 6 * 60 + 30).active,
          "midnight window and a zero-density window survive");
    check(d.traffic[0].max == 3 && d.traffic[0].trips == trafficdata::Trips::Mixed && d.traffic[0].startOnRoute == 1, "traffic rule from the routes");
    const auto line = trafficroute::Build(d.routes[0]);
    check(line.closed && trafficroute::Length(line) > 300, "the route flies as a closed path");

    // Round trip through the editor's parser.
    File g; std::vector<std::string> problems;
    check(Parse(text, "Editor_x.ini", g, problems) && problems.empty(), "parse back");
    check(g.worldId == "JemisonPlanetWorld" && g.routes.size() == 2 && g.routes[1].label == "Night route" && g.routes[0].points[2].z == 60, "routes back");
    check(g.landings.size() == 1 && g.spawns.size() == 1 && g.spawns[0].heading == 90 && g.traffic, "spots back");
    check(g.landings[0].parked && d.places[0].parked, "baked parking flag survives");
    check(d.links.size() == 2 && d.links[0].from == "ringroad" && d.links[0].at == 135 && d.links[0].share == 0.25 && !d.links[0].toEnd && d.links[1].toEnd, "links read by the addon");
    check(g.links.size() == 2 && g.links[1].toEnd && g.links[0].share == 0.25, "links back");
    check(Format(g) == text, "format is stable");

    // No traffic rule when another file already has one; no rule for an empty file.
    f.traffic = false;
    trafficdata::Data e; trafficdata::Read(Format(f), "x", e);
    check(e.traffic.empty(), "rule left to the other file");
    File empty; empty.worldId = "W";
    trafficdata::Data n; trafficdata::Read(Format(empty), "x", n);
    check(n.routes.empty() && n.traffic.empty() && n.problems.empty(), "empty editor file is valid");
    // Edited originals (2026-10-29): same name replaces the original, removed = 1 hides one, rings keep their shape, an
    // anchored city's route keeps its world key.
    {
        File e; e.worldId = "JemisonPlanetWorld";
        Route ring; ring.id = "ringroad"; ring.label = "ringroad"; ring.world = "#jemisonplanetworld"; ring.ring = true;
        ring.shape = ringlogic::Clean({ -800, 1600, 120, 350, 1, 0, 40 }); ring.lanes = 2; ring.speed = 30;
        Route path; path.id = "newatlantis_loop"; path.label = "newatlantis_loop"; path.world = "newatlantis"; path.closed = true;
        path.points = { {0, 0, 50}, {100, 0, 60}, {100, 100, 70} };
        Route gone; gone.id = "cydonia_loop"; gone.world = "cydonia"; gone.removed = true;
        e.routes = { ring, path, gone };
        const std::string t = Format(e);
        trafficdata::Data all;
        trafficdata::Read("[world.newatlantis]\nid = JemisonPlanetWorld\n\n[route.newatlantis_loop]\nworld = newatlantis\ntype = path\npoints = 5 5 5, 9 9 9\n\n"
                          "[route.cydonia_loop]\nworld = newatlantis\ntype = path\npoints = 1 1 1, 2 2 2\n\n[exit.spaceport]\nroute = newatlantis_loop\nat = 0\n", "NewAtlantis.ini", all);
        trafficdata::Read(t, "Editor_jemisonplanetworld.ini", all);
        trafficdata::AddRingroad("JemisonPlanetWorld", "-800 1600 100 300 1 0 40", all);
        check(all.problems.empty(), "originals file and editor file read");
        trafficdata::PreferEditor(all);
        int loops = 0; const trafficdata::Route* loop = nullptr; const trafficdata::Route* rr = nullptr; bool cyd = false;
        for (const auto& r : all.routes) {
            if (r.name == "newatlantis_loop") { ++loops; loop = &r; }
            if (r.name == "ringroad") rr = &r;
            if (r.name == "cydonia_loop") cyd = true;
        }
        check(loops == 1 && loop && loop->points.size() == 3 && loop->file.rfind("Editor_", 0) == 0, "the editor's version replaces the original");
        check(rr && rr->ring && rr->file.rfind("Editor_", 0) == 0 && std::fabs(rr->shape.radius - 350) < 1e-6 && rr->lanes == 2, "an edited ring stays a ring");
        check(!cyd, "removed = 1 hides the original");
        check(all.exits.size() == 1 && all.exits[0].route == "newatlantis_loop", "exits still name the route");
        File back; std::vector<std::string> problems;
        check(Parse(t, "Editor_jemisonplanetworld.ini", back, problems) && back.routes.size() == 3, "editor file with originals parses");
        check(back.routes[0].ring && back.routes[0].world == "#jemisonplanetworld" && back.routes[1].world == "newatlantis" && back.routes[2].removed, "worlds, ring and removal kept");
        check(Format(back) == t, "stable");
    }
    // A parking spot inside an anchored city keeps the city's key (2026-10-29): the addon moves it with the city.
    {
        File e; e.worldId = "AkilaPlanetWorld";
        e.landings = { {"ed_landing_1", {12.5, -40, 30}, 0, 30, true, "akilacity"} };
        const std::string t = Format(e);
        trafficdata::Data d;
        trafficdata::Read("[world.akilacity]\nanchor = 0017C407 -174.56 106.84 36.64\n", "AkilaCity.ini", d);
        trafficdata::Read(t, "Editor_akila.ini", d);
        trafficdata::PlaceWorld(d, "akilacity", "AkilaPlanetWorld", { -2000, -700, -4 });
        check(d.places.size() == 1 && d.places[0].parked && d.places[0].at.x == 12.5 - 2000 && d.places[0].at.y == -740, "a city's parking spot moves with the city");
        File back; std::vector<std::string> problems;
        check(Parse(t, "Editor_akila.ini", back, problems) && back.landings.size() == 1 && back.landings[0].world == "akilacity" && Format(back) == t, "city key kept");
    }
    {
        File e; e.worldId = "W";
        e.links = { {"ed_link_1", "ring", "ed_b", 45, false, 0.5, 2}, {"ed_link_2", "ring", "ed_b", 90, true, 0, 0} };
        const std::string t = Format(e);
        File back; std::vector<std::string> problems;
        check(t.find("to_at = 2\n") != std::string::npos && Parse(t, "x", back, problems) && back.links.size() == 2 && back.links[0].toAt == 2 && !back.links[0].toEnd &&
              back.links[1].toEnd && Format(back) == t, "a middle junction is written and read back");
    }
    {
        File f;f.worldId="W";Route r;r.id="ed_round";r.label="Round";r.points={{0,0,0},{100,0,10},{100,100,20}};r.rounding={0,20,0};r.schedule.allDayVehicles=fvolimits::kVehicles;f.routes={r};
        f.links={{"ed_junction","ed_round","ed_b",1,false,0.5,0,25}};
        File back;std::vector<std::string> issues;const auto text=Format(f);
        check(Parse(text,"rounded.ini",back,issues)&&back.routes[0].rounding[1]==20&&back.links[0].radius==25&&Format(back)==text,"point and junction radius round-trip through the actual traffic format");
        check(back.routes[0].schedule.allDayVehicles==fvolimits::kVehicles&&text.find("max = "+std::to_string(fvolimits::kVehicles))!=std::string::npos,"density and fleet target retain the shared limit instead of silently clipping to 64");
    }
    {   // vehicle rules (2026-10-30): a route's exclusion list is written, read back and kept per route
        File f; f.worldId = "JemisonPlanetWorld";
        Route r; r.id = "ed_loop"; r.label = "Loop"; r.points = { {0,0,50},{100,0,50},{100,100,50} };
        r.exclude = { 0xFE001033u, 0x0001AB2Cu };
        Route open = r; open.id = "ed_open"; open.exclude.clear();
        f.routes = { r, open };
        const auto text = Format(f);
        File back; std::vector<std::string> issues;
        check(text.find("exclude = FE001033, 0001AB2C") != std::string::npos, "the exclusion list is written as hex form ids");
        check(Parse(text, "rules.ini", back, issues) && back.routes.size() == 2 && back.routes[0].exclude == r.exclude && back.routes[1].exclude.empty() && Format(back) == text,
              "the exclusion list round-trips, and a route without one stays open to every type");
        trafficdata::Data d;
        trafficdata::Read("[route.a]\ntype = path\npoints = 0 0 0, 10 0 0\nexclude = FE001033, nonsense\n", "bad.ini", d);
        check(d.routes.size() == 1 && d.routes[0].exclude.size() == 1 && !d.problems.empty(), "a bad id in the list is reported and the good ones kept");
    }
    {   // old files keep working: a link without a type stays one, and writes no new line
        File f; f.worldId = "OldWorld";
        Route a; a.id = "ed_a"; a.label = "A"; a.points = { { 0, 0, 10 }, { 100, 0, 10 }, { 100, 100, 10 } };
        Route b = a; b.id = "ed_b"; b.label = "B";
        f.routes = { a, b };
        Link k; k.id = "ed_k"; k.from = "ed_a"; k.to = "ed_b"; k.at = 1; f.links.push_back(k);
        const std::string text = Format(f);
        check(text.find("type = exit") == std::string::npos && text.find("\ntype = \n") == std::string::npos, "no type line for a link with no type");
        File back; std::vector<std::string> problems;
        check(Parse(text, "x.ini", back, problems) && problems.empty() && back.links.size() == 1 && back.links[0].type.empty(), "a link with no type reads back with none");
        f.links[0].type = "crossing";
        check(Parse(Format(f), "x.ini", back, problems) && back.links[0].type == "crossing", "a type round trips");
        trafficdata::Data d;
        trafficdata::Read("[link.x]\nfrom = a\nto = b\nat = 1\ntype = sideways\n", "x.ini", d);
        check(d.links.size() == 1 && d.links[0].type.empty() && !d.problems.empty(), "a type not understood is named and read as both ways");
    }
    {   // Traffic Manager V1: time rules by busy words, role, a landing area, a zone and the city settings round trip
        File f; f.worldId = "RoundWorld";
        Route r; r.id = "ed_day"; r.label = "Day"; r.points = { { 0, 0, 10 }, { 100, 0, 10 }, { 100, 100, 10 } };
        r.schedule.zones = { { 0, 1 }, { 480, 4 } }; r.schedule.peak = 12; r.schedule.levels[1] = 30; r.schedule.blend = 60;
        routeschedule::Expand(r.schedule); r.role = "cargo";
        f.routes.push_back(r);
        Spot s; s.id = "ed_land"; s.at = { 5, 6, 7 }; s.layout = "ring"; s.spaces = 8; s.turn = 45; s.purpose = "depot"; s.stay = 600; s.full = "queue"; s.routes = { "ed_day" };
        f.landings.push_back(s);
        Zone z; z.name = "ed_zone"; z.at = { 1, 2, 3 }; z.radius = 150; z.top = 90; f.zones.push_back(z);
        Zone z2; z2.name = "ed_slow"; z2.slow = true; z2.speed = 12; f.zones.push_back(z2);
        f.cityWritten = true; f.city.on = false; f.city.density = 150; f.city.limit = 40; f.city.originals = false;
        File back; std::vector<std::string> problems;
        check(Parse(Format(f), "x.ini", back, problems) && problems.empty(), "the new sections read back with no problems");
        const auto& q = back.routes[0];
        check(q.schedule.zones.size() == 2 && q.schedule.peak == 12 && q.schedule.levels[1] == 30 && q.schedule.blend == 60 && !q.schedule.windows.empty() && q.role == "cargo", "busy words and role round trip");
        const auto& l = back.landings[0];
        check(l.layout == "ring" && l.spaces == 8 && l.turn == 45 && l.purpose == "depot" && l.stay == 600 && l.full == "queue" && l.routes.size() == 1 && l.routes[0] == "ed_day", "landing area round trip");
        check(back.zones.size() == 2 && !back.zones[0].slow && back.zones[0].top == 90 && back.zones[1].slow && back.zones[1].speed == 12, "zones round trip");
        check(back.cityWritten && !back.city.on && back.city.density == 150 && back.city.limit == 40 && !back.city.originals, "city settings round trip");
    }
    {   // route groups and all-routes rules (2026-10-31)
        trafficdata::Data d;
        std::vector<trafficdata::RouteGroup> groups{ { "global", "All routes", { 0xAAu } }, { "flying", "Flying", { 0xBBu, 0xCCu } } };
        const std::string text = FormatGroups(groups);
        trafficdata::Read(text, "VehicleGroups.ini", d);
        check(d.problems.empty() && d.groups.size() == 2 && d.groups[1].label == "Flying" && d.groups[1].exclude.size() == 2, "groups survive being written and read");
        trafficdata::Read("[group.flying]\nlabel = Air\n", "later.ini", d);
        check(d.groups.size() == 2 && d.groups[1].label == "Air", "the same group named twice is one group");
        trafficdata::Route inGroup; inGroup.group = "flying"; inGroup.exclude = { 0xDDu };
        trafficdata::Route alone; alone.exclude = {};
        check(trafficdata::LeftOut(groups, inGroup, 0xDDu), "a route's own rule");
        check(trafficdata::LeftOut(groups, inGroup, 0xBBu) && !trafficdata::LeftOut(groups, alone, 0xBBu), "a group's rule reaches only its routes");
        check(trafficdata::LeftOut(groups, inGroup, 0xAAu) && trafficdata::LeftOut(groups, alone, 0xAAu), "all routes means all routes");
        check(!trafficdata::LeftOut(groups, inGroup, 0xEEu), "anything else may fly");
        check(trafficdata::AllLeftOut(groups, inGroup) == std::vector<unsigned>({ 0xAAu, 0xBBu, 0xCCu, 0xDDu }), "the whole list, once each");
        trafficdata::Route gone; gone.group = "deleted";
        check(trafficdata::AllLeftOut(groups, gone) == std::vector<unsigned>({ 0xAAu }), "a group that no longer exists adds nothing");
        Route r; r.id = "ed_a"; r.label = "A"; r.points = { {0,0,0},{10,0,0},{20,0,0} }; r.group = "flying";
        Route plain = r; plain.id = "ed_b"; plain.group.clear();
        File f; f.worldId = "W"; f.routes = { r, plain };
        const std::string file = Format(f);
        File back; std::vector<std::string> issues;
        check(Parse(file, "g.ini", back, issues) && back.routes[0].group == "flying" && back.routes[1].group.empty(), "a route keeps its group");
        check(file.find("group = ") == file.rfind("group = "), "a route with none writes nothing, so older files stay as they are");
    }
    std::printf("routefiles: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
