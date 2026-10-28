#pragma once
// FVO traffic files (2026-10-28, the user: "a type of mod that only needs a certain set of data to read so players can create
// their own traffic"). Every *.ini in Data\SFSE\Plugins\FVO_Traffic\ is read; sections are [file], [world.NAME], [route.NAME],
// [exit.NAME], [place.NAME] and [traffic.NAME]; names join them across files. The rings the in-game ring editor saves in
// ringroad.ini are routes too, named "ringroad" in their world. Pure: tests/trafficdata_test.cpp.
#include "ringlogic.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
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
    bool closed = false;             // a path that joins its last point back to its first (a loop: figure eight, oval)
    int lanes = 2;
    double width = 20, speed = 25;
};
struct Exit { std::string name, route; double at = 0; };  // ring: degrees round from north; path: point index
struct Place { std::string name, world, exitName; Vec at; double radius = 40; };
enum class Trips { Laps, Places, Mixed };
struct Traffic {
    std::string name, world;
    int max = 2;
    double gap = 30, reach = 400;
    bool anyVehicle = true, rev8 = false;
    std::vector<unsigned> bases;     // vehicle base form ids allowed (empty with anyVehicle)
    Trips trips = Trips::Laps;
    int startOnRoute = 0;            // read; not acted on yet
    std::string route;               // only this route (empty: the nearest route of the world)
};
struct Data {
    std::map<std::string, std::string> worlds;  // world name -> id (the worldspace's editor name)
    std::vector<Route> routes;
    std::vector<Exit> exits;
    std::vector<Place> places;
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
            if (get("id").empty()) bad(sec, "id", "missing");
            else data.worlds[name] = get("id");
        }
        else if (kind == "route") {
            Route r;
            r.name = name; r.world = Lower(get("world")); r.file = file;
            const std::string type = Lower(get("type"));
            r.ring = type.empty() || type == "ring";
            if (!r.ring && type != "path") { bad(sec, "type", "ring or path"); continue; }
            double lanes = 2;
            num(sec, kv, "lanes", lanes);
            r.lanes = std::clamp(int(lanes), 1, 2);
            num(sec, kv, "width", r.width);
            num(sec, kv, "speed", r.speed);
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
                r.shape = ringlogic::Clean(r.shape);
            }
            else {
                bool ok = true;
                const auto p = Numbers(get("points"), ok);
                if (!ok || p.size() < 6 || p.size() % 3) { bad(sec, "points", "needs at least two points, each x y z"); continue; }
                for (std::size_t i = 0; i + 2 < p.size(); i += 3) r.points.push_back({ p[i], p[i + 1], p[i + 2] });
                r.closed = Trim(get("closed")) == "1";
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
            p.name = name; p.world = Lower(get("world")); p.exitName = Lower(get("near"));
            bool ok = true;
            const auto v = Numbers(get("point"), ok);
            if (!ok || v.size() != 3) { bad(sec, "point", "needs x y z"); continue; }
            p.at = { v[0], v[1], v[2] };
            num(sec, kv, "radius", p.radius);
            data.places.push_back(p);
        }
        else if (kind == "traffic") {
            Traffic t;
            t.name = name; t.world = Lower(get("world")); t.route = Lower(get("route"));
            double max = t.max, start = 0;
            num(sec, kv, "max", max);
            t.max = std::clamp(int(max), 0, 16);
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
        else bad(sec, "", "unknown section kind (file, world, route, exit, place, traffic)");
    }
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
