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
    bool test = false;
    // removed = 1 (map route editor): hides the route of this name from every other file (PreferEditor).
    bool removed = false;
};
struct Exit { std::string name, route; double at = 0; };  // ring: degrees round from north; path: point index
// A junction (2026-10-29, the user: "a ring with a connection between a sub route with less density"): route `from` at `at`
// (as an exit: ring degrees, path point index) meets route `to` at its start or end (toEnd). A vehicle on a lap passing the
// junction turns onto the other route with probability `share`; at the end of an open branch it always takes the junction
// there. A branch joined at one end only is a spur: the vehicle lands at its far end and flies home to its own route.
// to_at may also be a point index of `to` (toAt, 2026-10-29, the user: two routes joined "at more than just 2" points): a junction
// in the middle of the branch; vehicles join or leave the branch there by the share, as at the main route.
struct Link { std::string name, from, to; double at = 0; bool toEnd = false; double share = 0.2; double toAt = 0; double radius=0; };
// parked = 1 (2026-10-29, map route editor "Bake parking"): a REV-8 is baked here (FVO_EditorPlacements.esm); the cold-start pool
// gives it a driver when a route needs one (core never creates a vehicle at such a spot).
struct Place { std::string name, world, exitName; Vec at; double radius = 40; bool parked = false; };
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
struct Data {
    std::map<std::string, std::string> worlds;  // world name -> id (the worldspace's editor name)
    std::map<std::string, std::vector<AnchorRef>> anchors;  // world name -> its anchor references (city coordinates)
    std::vector<Route> routes;
    std::vector<Exit> exits;
    std::vector<Link> links;
    std::vector<Place> places;
    std::vector<Spawn> spawns;
    std::vector<Traffic> traffic;
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
            r.label = get("label"); r.test = Trim(get("test")) == "1";
            if (Trim(get("removed")) == "1") { r.removed = true; r.ring = false; data.routes.push_back(r); continue; }
            const std::string type = Lower(get("type"));
            r.ring = type.empty() || type == "ring";
            if (!r.ring && type != "path") { bad(sec, "type", "ring or path"); continue; }
            double lanes = 2;
            num(sec, kv, "lanes", lanes);
            r.lanes = std::clamp(int(lanes), 1, 2);
            num(sec, kv, "width", r.width);
            num(sec, kv, "speed", r.speed);
            if (!get("windows").empty()) {
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
            data.links.push_back(k);
        }
        else bad(sec, "", "unknown section kind (file, world, route, exit, link, place, spawn, traffic)");
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
}
