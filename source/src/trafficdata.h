#pragma once
#include "fvolimits.h"
// FVO traffic files (2026-10-28, the user: "a type of mod that only needs a certain set of data to read so players can create
// their own traffic"). Every *.ini in Data\SFSE\Plugins\FVO_Traffic\ is read; sections are [file], [world.NAME], [route.NAME],
// [exit.NAME], [place.NAME] and [traffic.NAME]; names join them across files. The rings the in-game ring editor saves in
// ringroad.ini are routes too, named "ringroad" in their world. Pure: tests/trafficdata_test.cpp.
#include "ringlogic.h"
#include "routeschedule.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace trafficdata {
struct Vec { double x = 0, y = 0, z = 0; };
struct Route {
    std::string name, world, file;
    bool ring = true;
    ringlogic::Ring shape;           // a ring
    std::vector<Vec> points;         // a path, in order
    std::vector<double> rounding;    // optional radius per authored point; 0 keeps its sharp corner
    bool closed = false;             // a path that joins its last point back to its first (a loop: figure eight, oval)
    // A slanted ring (2026-10-28, the user: "the ring for traffic for new atlantis could be slanted"): tilted `tilt` degrees, its
    // high side toward `tiltToward` (degrees clockwise from north). `height` is the height at the centre.
    double tilt = 0, tiltToward = 0;
    int lanes = 2;
    double width = 20, speed = 25;
    routeschedule::Schedule schedule{true,16,{}};  // old files: all day, no per-route restriction
    // The map route editor (2026-10-28): its display name, and test = 1 sends the next free vehicle round it once.
    std::string label;
    unsigned color = 0;  // its line on the map (color = RRGGBB, map route editor); 0: chosen by its place in the list
    bool test = false;
    // removed = 1 (map route editor): hides the route of this name from every other file (PreferEditor).
    bool removed = false;
    // Vehicle rules (2026-10-30, the user: an exclusion list, not a pick list): exclude = FORMID, FORMID names vehicle models
    // (base form ids) that never fly this route. Empty: every model may.
    std::vector<unsigned> exclude;
    // What the route's vehicles do (2026-10-30, Traffic Manager V1): taxi lands at Taxi areas, cargo flies at 80% speed and lands at
    // Depots, patrol only flies laps, civilian (the default) lands anywhere but a depot.
    std::string role;
    std::string group;               // a route group (VehicleGroups.ini): its vehicle rules apply here too; empty: none
};
// A list of hex form ids ("FE001033, 0001AB2C"); false when a word is not one.
inline bool FormIds(const std::string& s, std::vector<unsigned>& out)
{
    std::string list = s;
    for (auto& c : list) if (c == ',') c = ' ';
    std::istringstream ids(list);
    std::string w;
    bool ok = true;
    while (ids >> w) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(w.c_str(), &end, 16);
        if (end && !*end && id) out.push_back(unsigned(id)); else ok = false;
    }
    return ok;
}
struct Exit { std::string name, route; double at = 0; };  // ring: degrees round from north; path: point index
// A junction (2026-10-29, the user: "a ring with a connection between a sub route with less density"): route `from` at `at`
// (as an exit: ring degrees, path point index) meets route `to` at its start or end (toEnd). A vehicle on a lap passing the
// junction turns onto the other route with probability `share`; at the end of an open branch it always takes the junction
// there. A branch joined at one end only is a spur: the vehicle lands at its far end and flies home to its own route.
// to_at may also be a point index of `to` (toAt, 2026-10-29, the user: two routes joined "at more than just 2" points): a junction
// in the middle of the branch; vehicles join or leave the branch there by the share, as at the main route.
// type (2026-10-30): how traffic uses it. Empty or both: either way, as every link before types. exit: `from` traffic turns onto
// `to`, never back. merge: `to` traffic turns onto `from`, never back. crossing: no turning; they pass and one gives way.
struct Link { std::string name, from, to; double at = 0; bool toEnd = false; double share = 0.2; double toAt = 0; double radius=0; std::string type; };
// parked = 1 (2026-10-29, map route editor "Bake parking"): a REV-8 is baked here (FVO_EditorPlacements.esm); the cold-start pool
// gives it a driver when a route needs one (core never creates a vehicle at such a spot).
// Landing areas (2026-10-30, Traffic Manager V1): layout = single, row, bays (two facing rows) or ring; spaces; turn in degrees
// clockwise from north. Occupancy rules: purpose = taxi, depot or stop; stay = seconds parked (0: until a route needs it);
// full = queue (wait above it), circle (take another lap) or on (try another area); routes = the only routes that park here.
struct Place {
    std::string name, world, exitName; Vec at; double radius = 40; bool parked = false;
    std::string layout = "single", purpose = "stop", full = "circle";
    int spaces = 1; double turn = 0, stay = 0;
    std::vector<std::string> routes;
};
constexpr double kSpaceGap = 14.0;  // m between the middles of two spaces
// Where each space of a landing area is.
inline std::vector<Vec> Spaces(const Place& p)
{
    std::vector<Vec> out;
    const int n = p.layout == "single" ? 1 : std::clamp(p.spaces, 1, 24);
    const double t = p.turn * 3.14159265358979 / 180.0, ax = std::sin(t), ay = std::cos(t);  // along the turn; across is to its right
    auto at = [&](double along, double across) { out.push_back({ p.at.x + along * ax + across * ay, p.at.y + along * ay - across * ax, p.at.z }); };
    if (p.layout == "ring" && n > 1) {
        const double radius = std::max(kSpaceGap, n * kSpaceGap / (2 * 3.14159265358979));
        for (int i = 0; i < n; ++i) { const double a = t + i * 2 * 3.14159265358979 / n; out.push_back({ p.at.x + radius * std::sin(a), p.at.y + radius * std::cos(a), p.at.z }); }
    }
    else if (p.layout == "bays") {
        const int rows = (n + 1) / 2;
        for (int i = 0; i < n; ++i) at((i / 2 - (rows - 1) / 2.0) * kSpaceGap, i % 2 ? kSpaceGap / 2 : -kSpaceGap / 2);
    }
    else for (int i = 0; i < n; ++i) at((i - (n - 1) / 2.0) * kSpaceGap, 0);
    return out;
}
inline bool RouteMayPark(const Place& p, const std::string& route, const std::string& role)
{
    if (!p.routes.empty() && std::find(p.routes.begin(), p.routes.end(), route) == p.routes.end()) return false;
    if (role == "patrol") return false;
    if (role == "taxi") return p.purpose == "taxi";
    if (role == "cargo") return p.purpose == "depot";
    return p.purpose != "depot";
}
// A keep-out zone (traffic stays above `top`) or a slow zone (speed limit inside): a circle on the map.
struct Zone { std::string name, world; Vec at; double radius = 100, top = 0, speed = 10; bool slow = false; };
// A city's settings (2026-10-30, Traffic Manager V1): traffic on or off, density in percent of every route's count, the most
// vehicles at once, and whether the routes of other files fly.
struct City { std::string world; bool on = true, originals = true; int density = 100, limit = fvolimits::kVehicles; };
inline int Scaled(int vehicles, const City& c)
{
    if (!c.on || vehicles <= 0) return 0;
    return c.density <= 0 ? 0 : std::max(1, int(std::lround(vehicles * c.density / 100.0)));
}
// A cold-start spawn spot (2026-10-28): on the ground, flat and open; heading in degrees clockwise from north.
struct Spawn { std::string name, world; Vec at; double heading = 0; };
enum class Trips { Laps, Places, Mixed };
struct Traffic {
    std::string name, world;
    int max = 2;
    double gap = 30, reach = 400;
    bool anyVehicle = true, rev8 = false;
    std::vector<unsigned> bases;     // vehicle base form ids allowed (empty with anyVehicle)
    Trips trips = Trips::Laps;
    int startOnRoute = 0;            // the cold-start pool: vehicles spawned with seated drivers at the world's [spawn.*] spots
    std::string route;               // only this route (empty: the nearest route of the world)
};
// A world written in a city's own coordinates (2026-10-28, the user: "install for all the auto method"): the game lays a
// city over its planet at a spot it picks when you land, so neither the planet world's name nor the shift is known ahead.
// anchor = FORMID x y z, ... names references of the city with their positions in the city's .esm; the first one found
// loaded in the player's world gives both: the world is the player's, the shift is its live position minus its file one.
struct AnchorRef { unsigned id = 0; Vec at; };
// Vehicle rules beyond one route (2026-10-31, the user: "global exclusions and route group types ... custom groups I can assign each route to"). Kept in FVO_Traffic\\VehicleGroups.ini
// as [group.name] label = ..., exclude = FORMID, FORMID, so they hold on every planet. The group named "global" applies to every route.
struct RouteGroup { std::string name, label; std::vector<unsigned> exclude; };
inline const char* GlobalGroup() { return "global"; }
inline bool InList(const std::vector<unsigned>& list, unsigned id) { return std::find(list.begin(), list.end(), id) != list.end(); }
// Left out by the route's group or by all routes, whatever the route itself says.
inline bool LeftOutAbove(const std::vector<RouteGroup>& groups, const Route& r, unsigned id)
{
    for (const auto& g : groups) if ((g.name == GlobalGroup() || (!r.group.empty() && g.name == r.group)) && InList(g.exclude, id)) return true;
    return false;
}
inline bool LeftOut(const std::vector<RouteGroup>& groups, const Route& r, unsigned id) { return InList(r.exclude, id) || LeftOutAbove(groups, r, id); }
// Everything this route may not fly: its own list, its group's and all routes'.
inline std::vector<unsigned> AllLeftOut(const std::vector<RouteGroup>& groups, const Route& r)
{
    std::vector<unsigned> out = r.exclude;
    for (const auto& g : groups) if (g.name == GlobalGroup() || (!r.group.empty() && g.name == r.group)) out.insert(out.end(), g.exclude.begin(), g.exclude.end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}
struct Data {
    std::vector<RouteGroup> groups;
    std::map<std::string, std::string> worlds;  // world name -> id (the worldspace's editor name)
    std::map<std::string, std::vector<AnchorRef>> anchors;  // world name -> its anchor references (city coordinates)
    std::vector<Route> routes;
    std::vector<Exit> exits;
    std::vector<Link> links;
    std::vector<Place> places;
    std::vector<Spawn> spawns;
    std::vector<Traffic> traffic;
    std::vector<Zone> zones;
    std::vector<City> cities;
    std::vector<std::string> problems;          // "file: [section] key: why"
    int files = 0;
};

inline std::string Trim(std::string s)
{
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
inline std::string Lower(std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
inline bool Number(const std::string& s, double& out)
{
    const std::string t = Trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end && Trim(end).empty() && std::isfinite(out);
}
inline std::vector<double> Numbers(const std::string& s, bool& ok)
{
    std::vector<double> v;
    std::string t = s;
    for (auto& c : t) if (c == ',') c = ' ';
    std::istringstream in(t);
    std::string w;
    ok = true;
    while (in >> w) { double d; if (Number(w, d)) v.push_back(d); else ok = false; }
    return v;
}

// One file's text into `data`; `file` names it in problems.
inline void Read(const std::string& text, const std::string& file, Data& data)
{
    ++data.files;
    std::map<std::string, std::map<std::string, std::string>> sections;
    std::vector<std::string> order;
    std::string current;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (const auto semi = line.find(';'); semi != std::string::npos) line.resize(semi);
        line = Trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            current = Lower(Trim(line.substr(1, line.find(']') == std::string::npos ? std::string::npos : line.find(']') - 1)));
            if (!sections.count(current)) order.push_back(current);
            sections[current];
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos || current.empty()) { data.problems.push_back(file + ": not understood: " + line); continue; }
        sections[current][Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    auto bad = [&](const std::string& sec, const std::string& key, const std::string& why) { data.problems.push_back(file + ": [" + sec + "] " + key + ": " + why); };
    auto num = [&](const std::string& sec, const std::map<std::string, std::string>& kv, const char* key, double& out) {
        const auto it = kv.find(key);
        if (it == kv.end()) return;
        double d;
        if (Number(it->second, d)) out = d; else bad(sec, key, "not a number");
    };
    if (const auto f = sections.find("file"); f != sections.end()) {
        const auto e = f->second.find("enabled");
        if (e != f->second.end() && Trim(e->second) == "0") return;  // the whole file switched off
    }
    for (const auto& sec : order) {
        const auto& kv = sections[sec];
        const auto dot = sec.find('.');
        const std::string kind = sec.substr(0, dot), name = dot == std::string::npos ? std::string() : sec.substr(dot + 1);
        auto get = [&](const char* key) { const auto it = kv.find(key); return it == kv.end() ? std::string() : it->second; };
        if (kind == "file") continue;
        if (name.empty()) { bad(sec, "", "a section needs a name (kind.name)"); continue; }
        if (kind == "world") {
            if (!get("anchor").empty()) {
                std::string list = get("anchor");
                for (auto& c : list) if (c == ',') c = ' ';
                std::istringstream items(list);
                std::string id, x, y, z;
                std::vector<AnchorRef> refs;
                while (items >> id) {
                    char* end = nullptr;
                    const unsigned long form = std::strtoul(id.c_str(), &end, 16);
                    AnchorRef a;
                    if (!(items >> x >> y >> z) || !end || *end || !form || !Number(x, a.at.x) || !Number(y, a.at.y) || !Number(z, a.at.z)) {
                        bad(sec, "anchor", "needs FORMID x y z (comma between anchors)"); refs.clear(); break;
                    }
                    a.id = unsigned(form);
                    refs.push_back(a);
                }
                if (!refs.empty()) data.anchors[name] = refs;
            }
            if (!get("id").empty()) data.worlds[name] = get("id");
            else if (!data.anchors.count(name)) bad(sec, "id", "missing (or give anchor = ...)");
        }
        else if (kind == "route") {
            Route r;
            r.name = name; r.world = Lower(get("world")); r.file = file;
            r.label = get("label"); r.test = Trim(get("test")) == "1"; r.color = unsigned(std::strtoul(Trim(get("color")).c_str(), nullptr, 16)) & 0xFFFFFF;
            if (Trim(get("removed")) == "1") { r.removed = true; r.ring = false; data.routes.push_back(r); continue; }
            if (!get("exclude").empty() && !FormIds(get("exclude"), r.exclude)) bad(sec, "exclude", "form ids in hex, comma between them");
            const std::string type = Lower(get("type"));
            r.ring = type.empty() || type == "ring";
            if (!r.ring && type != "path") { bad(sec, "type", "ring or path"); continue; }
            double lanes = 2;
            num(sec, kv, "lanes", lanes);
            r.lanes = std::clamp(int(lanes), 1, 2);
            num(sec, kv, "width", r.width);
            num(sec, kv, "speed", r.speed);
            r.role = Lower(Trim(get("role")));
            r.group = Lower(Trim(get("group")));
            if (!r.role.empty() && r.role != "civilian" && r.role != "taxi" && r.role != "cargo" && r.role != "patrol") { bad(sec, "role", "civilian, taxi, cargo or patrol"); r.role.clear(); }
            if (!get("day").empty()) {
                routeschedule::Schedule day;
                if (!routeschedule::ParseDay(get("day"), day)) { bad(sec, "day", "expected start-minute word pairs (closed, quiet, steady, busy or rush), comma between them"); continue; }
                double peak = 0, blend = day.blend;
                num(sec, kv, "peak", peak);
                num(sec, kv, "blend", blend);
                day.peak = std::clamp(int(peak), 0, fvolimits::kVehicles);
                day.blend = std::clamp(int(blend), 0, 120);
                if (!get("levels").empty()) {
                    bool ok = true;
                    const auto v = Numbers(get("levels"), ok);
                    if (!ok || v.size() != routeschedule::kWords) bad(sec, "levels", "five percentages");
                    else for (int k = 0; k < routeschedule::kWords; ++k) day.levels[k] = std::clamp(int(v[std::size_t(k)]), 0, 100);
                }
                routeschedule::Expand(day);
                if (routeschedule::Validate(day) != routeschedule::Issue::None) { bad(sec, "day", "the zones do not make a valid day"); continue; }
                r.schedule = day;
            }
            else if (!get("windows").empty()) {
                if (!routeschedule::ParseWindows(get("windows"), r.schedule)) { bad(sec, "windows", "expected non-overlapping start-minute end-minute density triples"); continue; }
            }
            else if (!get("density").empty()) {
                const std::string density=Trim(get("density")); char* end=nullptr;
                const long n=std::strtol(density.c_str(),&end,10);
                if(!end || *end || end==density.c_str() || n<0 || n>fvolimits::kVehicles) { bad(sec,"density","expected an integer from 0 to "+std::to_string(fvolimits::kVehicles)); continue; }
                r.schedule.allDayVehicles=int(n);
            }
            if (r.ring) {
                bool ok = true;
                const auto c = Numbers(get("centre").empty() ? get("center") : get("centre"), ok);
                if (c.size() != 2 || !ok) { bad(sec, "centre", "needs x y"); continue; }
                r.shape.cx = c[0]; r.shape.cy = c[1];
                num(sec, kv, "height", r.shape.z);
                num(sec, kv, "radius", r.shape.radius);
                num(sec, kv, "aspect", r.shape.aspect);
                double rot = 0;
                num(sec, kv, "rotation", rot);
                r.shape.rotation = rot * ringlogic::kPi / 180.0;
                num(sec, kv, "lane_gap", r.shape.gap);
                num(sec, kv, "tilt", r.tilt);
                num(sec, kv, "tilt_toward", r.tiltToward);
                r.tilt = std::clamp(r.tilt, -45.0, 45.0);
                r.shape = ringlogic::Clean(r.shape);
            }
            else {
                bool ok = true;
                const auto p = Numbers(get("points"), ok);
                if (!ok || p.size() < 6 || p.size() % 3) { bad(sec, "points", "needs at least two points, each x y z"); continue; }
                for (std::size_t i = 0; i + 2 < p.size(); i += 3) r.points.push_back({ p[i], p[i + 1], p[i + 2] });
                r.closed = Trim(get("closed")) == "1";
                if(!get("rounding").empty()){
                    r.rounding=Numbers(get("rounding"),ok);
                    if(!ok||r.rounding.size()!=r.points.size()||std::any_of(r.rounding.begin(),r.rounding.end(),[](double radius){return !std::isfinite(radius)||radius<0||radius>200;})){
                        bad(sec,"rounding","one radius 0..200 per point required");continue;
                    }
                }
            }
            data.routes.push_back(r);
        }
        else if (kind == "exit") {
            Exit e;
            e.name = name; e.route = Lower(get("route"));
            if (e.route.empty()) { bad(sec, "route", "missing"); continue; }
            if (!Number(get("at"), e.at)) { bad(sec, "at", "not a number"); continue; }
            data.exits.push_back(e);
        }
        else if (kind == "place") {
            Place p;
            p.name = name; p.world = Lower(get("world")); p.exitName = Lower(get("near")); p.parked = Trim(get("parked")) == "1";
            bool ok = true;
            const auto v = Numbers(get("point"), ok);
            if (!ok || v.size() != 3) { bad(sec, "point", "needs x y z"); continue; }
            p.at = { v[0], v[1], v[2] };
            num(sec, kv, "radius", p.radius);
            const std::string layout = Lower(Trim(get("layout")));
            if (layout == "row" || layout == "bays" || layout == "ring" || layout == "single") p.layout = layout; else if (!layout.empty()) bad(sec, "layout", "single, row, bays or ring");
            const std::string purpose = Lower(Trim(get("purpose")));
            if (purpose == "taxi" || purpose == "depot" || purpose == "stop") p.purpose = purpose; else if (!purpose.empty()) bad(sec, "purpose", "taxi, depot or stop");
            const std::string full = Lower(Trim(get("full")));
            if (full == "queue" || full == "circle" || full == "on") p.full = full; else if (!full.empty()) bad(sec, "full", "queue, circle or on");
            double spaces = p.spaces;
            num(sec, kv, "spaces", spaces);
            p.spaces = std::clamp(int(spaces), 1, 24);
            num(sec, kv, "turn", p.turn);
            num(sec, kv, "stay", p.stay);
            p.stay = std::max(0.0, p.stay);
            {
                std::string list = Lower(get("routes"));
                for (auto& c : list) if (c == ',') c = ' ';
                std::istringstream names(list);
                std::string w;
                while (names >> w) p.routes.push_back(w);
            }
            data.places.push_back(p);
        }
        else if (kind == "spawn") {
            Spawn s;
            s.name = name; s.world = Lower(get("world"));
            bool ok = true;
            const auto v = Numbers(get("point"), ok);
            if (!ok || v.size() != 3) { bad(sec, "point", "needs x y z"); continue; }
            s.at = { v[0], v[1], v[2] };
            num(sec, kv, "heading", s.heading);
            data.spawns.push_back(s);
        }
        else if (kind == "traffic") {
            Traffic t;
            t.name = name; t.world = Lower(get("world")); t.route = Lower(get("route"));
            double max = t.max, start = 0;
            num(sec, kv, "max", max);
            t.max = std::clamp(int(max), 0, fvolimits::kVehicles);
            num(sec, kv, "gap", t.gap);
            num(sec, kv, "reach", t.reach);
            num(sec, kv, "start_on_route", start);
            t.startOnRoute = std::max(0, int(start));
            const std::string veh = Lower(get("vehicles"));
            if (!veh.empty() && veh != "any") {
                t.anyVehicle = false;
                if (veh == "rev8") t.rev8 = true;
                else {
                    std::string list = veh;
                    for (auto& c : list) if (c == ',') c = ' ';
                    std::istringstream ids(list);
                    std::string w;
                    while (ids >> w) {
                        char* end = nullptr;
                        const unsigned long id = std::strtoul(w.c_str(), &end, 16);
                        if (end && !*end && id) t.bases.push_back(unsigned(id)); else bad(sec, "vehicles", "not a form id: " + w);
                    }
                }
            }
            const std::string trips = Lower(get("trips"));
            if (trips == "places") t.trips = Trips::Places;
            else if (trips == "mixed") t.trips = Trips::Mixed;
            else if (!trips.empty() && trips != "laps") bad(sec, "trips", "places, laps or mixed");
            data.traffic.push_back(t);
        }
        else if (kind == "link") {
            Link k;
            k.name = name; k.from = Lower(get("from")); k.to = Lower(get("to"));
            if (k.from.empty() || k.to.empty() || k.from == k.to) { bad(sec, "from/to", "two different routes needed"); continue; }
            if (!Number(get("at"), k.at)) { bad(sec, "at", "not a number"); continue; }
            const std::string end = Lower(Trim(get("to_at")));
            if (end == "end") k.toEnd = true;
            else if (!end.empty() && end != "start" && !(Number(end, k.toAt) && k.toAt >= 0)) { bad(sec, "to_at", "start, end or a point index"); continue; }
            if (!get("share").empty() && (!Number(get("share"), k.share) || k.share < 0 || k.share > 1)) { bad(sec, "share", "0 to 1"); continue; }
            if(!get("radius").empty()&&(!Number(get("radius"),k.radius)||k.radius<0||k.radius>200)){bad(sec,"radius","0..200 metres required");continue;}
            k.type = Lower(Trim(get("type")));
            if (k.type == "both") k.type.clear();
            if (!k.type.empty() && k.type != "exit" && k.type != "merge" && k.type != "crossing") { bad(sec, "type", "both, exit, merge or crossing; read as both"); k.type.clear(); }
            data.links.push_back(k);
        }
        else if (kind == "zone") {
            Zone z;
            z.name = name; z.world = Lower(get("world"));
            bool ok = true;
            const auto v = Numbers(get("point"), ok);
            if (!ok || v.size() != 3) { bad(sec, "point", "needs x y z"); continue; }
            z.at = { v[0], v[1], v[2] };
            z.slow = Lower(Trim(get("kind"))) == "slow";
            num(sec, kv, "radius", z.radius);
            num(sec, kv, "top", z.top);
            num(sec, kv, "speed", z.speed);
            z.radius = std::clamp(z.radius, 10.0, 2000.0); z.speed = std::clamp(z.speed, 2.0, 80.0);
            data.zones.push_back(z);
        }
        else if (kind == "city") {
            City c;
            c.world = Lower(get("world"));
            c.on = Trim(get("traffic")) != "0"; c.originals = Trim(get("originals")) != "0";
            double density = c.density, limit = c.limit;
            num(sec, kv, "density", density);
            num(sec, kv, "limit", limit);
            c.density = std::clamp(int(density), 0, 200); c.limit = std::clamp(int(limit), 0, fvolimits::kVehicles);
            data.cities.push_back(c);
        }
        else if (kind == "group") {
            RouteGroup g;
            g.name = Lower(name); g.label = get("label").empty() ? name : get("label");
            if (!get("exclude").empty() && !FormIds(get("exclude"), g.exclude)) bad(sec, "exclude", "form ids in hex, comma between them");
            const auto same = std::find_if(data.groups.begin(), data.groups.end(), [&](const RouteGroup& o) { return o.name == g.name; });
            if (same != data.groups.end()) *same = g; else data.groups.push_back(g);
        }
        else bad(sec, "", "unknown section kind (file, world, route, exit, link, place, spawn, traffic, zone, city, group)");
    }
}

// The map route editor's file (FVO_Traffic\Editor_*.ini) wins (2026-10-29, the user: "I need to be able to edit the traffic
// from the original files too"): a route there replaces every route of the same name from the other files (exits, links and
// route = rules keep pointing at it by name), and removed = 1 drops that name altogether. Call once all files are read.
inline bool EditorFile(const std::string& file) { return Lower(file).rfind("editor_", 0) == 0; }
inline void PreferEditor(Data& d)
{
    std::set<std::string> edited;
    for (const auto& r : d.routes) if (EditorFile(r.file)) edited.insert(r.name);
    d.routes.erase(std::remove_if(d.routes.begin(), d.routes.end(),
        [&](const Route& r) { return r.removed || (edited.count(r.name) && !EditorFile(r.file)); }), d.routes.end());
}

// The editor's rings (ringroad.ini: one section per world id, "ring=" in ringlogic::Format) as routes named "ringroad".
inline void AddRingroad(const std::string& worldId, const std::string& ringText, Data& data)
{
    ringlogic::Ring r;
    if (!ringlogic::Parse(ringText, r)) return;
    Route route;
    route.name = "ringroad"; route.world = "#" + Lower(worldId); route.file = "ringroad.ini"; route.shape = r;
    data.routes.push_back(route);
}

// An anchored world found in game: it is `gameWorld`, and everything written in it moves by `shift` into game coordinates.
inline void PlaceWorld(Data& d, const std::string& name, const std::string& gameWorld, const Vec& shift)
{
    d.worlds[name] = gameWorld;
    for (auto& r : d.routes) {
        if (r.world != name) continue;
        r.shape.cx += shift.x; r.shape.cy += shift.y; r.shape.z += shift.z;
        for (auto& p : r.points) { p.x += shift.x; p.y += shift.y; p.z += shift.z; }
    }
    for (auto& p : d.places)
        if (p.world == name) { p.at.x += shift.x; p.at.y += shift.y; p.at.z += shift.z; }
    for (auto& s : d.spawns)
        if (s.world == name) { s.at.x += shift.x; s.at.y += shift.y; s.at.z += shift.z; }
    for (auto& z : d.zones)
        if (z.world == name) { z.at.x += shift.x; z.at.y += shift.y; z.at.z += shift.z; z.top += shift.z; }
}

// The world key of a [world.NAME] or of a ringroad.ini ring ("#" + its id), lower case; matched against the game's world.
inline bool SameWorld(const Data& d, const std::string& key, const std::string& gameWorld)
{
    if (key.empty()) return true;  // no world given: any world (matched by distance only)
    const std::string g = Lower(gameWorld);
    if (key[0] == '#') return key.substr(1) == g;
    const auto it = d.worlds.find(key);
    return it != d.worlds.end() ? Lower(it->second) == g : key == g;
}
// This world's settings (none written: traffic on, as the files say).
inline City CityOf(const Data& d, const std::string& gameWorld)
{
    for (const auto& c : d.cities) if (SameWorld(d, c.world, gameWorld)) return c;
    return {};
}
// A flight goal under the zones: inside (or about to enter) a keep-out zone the goal stays above its top; inside a slow zone the
// speed drops to its limit. (x, y) is where the vehicle is; (gx, gy) where it is heading.
inline void ApplyZones(const Data& d, const std::string& gameWorld, double x, double y, double gx, double gy, double& gz, double& speed, bool keepOut)
{
    for (const auto& z : d.zones) {
        if (!SameWorld(d, z.world, gameWorld)) continue;
        const double here = std::hypot(x - z.at.x, y - z.at.y), there = std::hypot(gx - z.at.x, gy - z.at.y);
        if (z.slow) { if (here <= z.radius) speed = std::min(speed, z.speed); }
        else if (keepOut && (here <= z.radius + 30 || there <= z.radius + 30)) gz = std::max(gz, z.top + 5);
    }
}
}
