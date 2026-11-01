#pragma once
// The map route editor's own traffic file (2026-10-28). One file per world, FVO_Traffic\Editor_<world id>.ini, written
// only by the editor: hand-written files and ringroad.ini are never touched. It is an ordinary traffic file
// (trafficdata.h), so the addon reads it like any other; route names join across files, so every editor name starts "ed_".
// Pure: tests/routefiles_test.cpp. Writing to disk (temp file + replace, one backup) is the caller's.
#include "trafficdata.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace routefiles {
using Vec = trafficdata::Vec;
struct Route {
    std::string id, label;             // id: the section name (ed_..., or an original route's own name), label: what the player typed
    // An edited original (2026-10-29) keeps its own name and world key (empty: the editor's world); it replaces the original.
    // removed: hides the original of that name. ring: a ring route (shape; points unused).
    std::string world;
    bool removed = false, ring = false;
    ringlogic::Ring shape;
    double tilt = 0, tiltToward = 0;
    std::vector<double> rounding;
    std::vector<Vec> points;           // world coordinates, each with its own height
    bool closed = true;
    int lanes = 1;                     // 1 one way, 2 both ways (the second lane runs the other way)
    double speed = 25, width = 20;     // m/s along the lane; m between the two lanes
    routeschedule::Schedule schedule{ true, 2, {} };
    bool test = false;
    unsigned color = 0;                // the line's colour on the map, RRGGBB; 0: chosen by its place in the list
    std::vector<unsigned> exclude;     // vehicle rules: models (base form ids) left out of this route
    std::string role;                  // empty: civilian
    std::string group;                 // a route group's name (VehicleGroups.ini); empty: none
};
// A landing place (parked: a vehicle stands there) or a spawn spot. world: empty for the editor's world, else an anchored city's
// key (2026-10-29): the spot is written in the city's own coordinates, so it follows the city wherever the game lays it down.
struct Spot {
    std::string id; Vec at; double heading = 0; double radius = 30; bool parked = false; std::string world;
    // A landing area and its occupancy rules (trafficdata::Place).
    std::string layout = "single", purpose = "stop", full = "circle"; int spaces = 1; double turn = 0, stay = 0; std::vector<std::string> routes;
};
using Zone = trafficdata::Zone;
// A junction (trafficdata::Link): `from` (any route of the world, a ring in degrees, a path by point index) at `at` meets the
// editor route `to` at its start or end; `share` of the laps passing it turn in.
struct Link { std::string id, from, to; double at = 0; bool toEnd = false; double share = 0.2; double toAt = 0; double radius=0; std::string type; };  // toAt: a point index of `to` (0: its start); type: trafficdata::Link
struct File {
    std::string worldId;               // the worldspace's editor name, as FVO_WorldName gives it
    std::vector<Route> routes;
    std::vector<Spot> landings, spawns;
    std::vector<Link> links;
    std::vector<Zone> zones;           // keep-out and slow zones; world as a spot's
    trafficdata::City city;
    bool cityWritten = false;          // false: no [city] section (traffic on, as the files say)
    bool traffic = true;               // write a [traffic] rule (only when no other file has one for this world)
};

inline std::string Slug(const std::string& s)
{
    std::string out;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) out.push_back(char(std::tolower(c)));
        else if (!out.empty() && out.back() != '_') out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.size() > 40) out.resize(40);
    return out.empty() ? "route" : out;
}
inline std::string WorldKey(const std::string& worldId) { return "ed_" + Slug(worldId); }
inline std::string FileName(const std::string& worldId) { return "Editor_" + Slug(worldId) + ".ini"; }
// A free section name for `label`: ed_<slug>, then ed_<slug>_2, _3 ... past every name in `taken` (all files' routes).
inline std::string NewId(const std::string& label, const std::set<std::string>& taken)
{
    const std::string base = "ed_" + Slug(label);
    if (!taken.count(base)) return base;
    for (int n = 2;; ++n) { const std::string id = base + "_" + std::to_string(n); if (!taken.count(id)) return id; }
}
// A label never breaks the file: no newlines, no ';' (a comment), no '[' at the start.
inline std::string CleanLabel(std::string s)
{
    for (auto& c : s) if (c == '\r' || c == '\n' || c == ';' || c == '[' || c == ']' || c == '=') c = ' ';
    s = trafficdata::Trim(s);
    if (s.size() > 48) s.resize(48);
    return s.empty() ? "Route" : s;
}
inline std::string Num(double v)
{
    char b[48];
    std::snprintf(b, sizeof b, "%.2f", std::fabs(v) < 0.005 ? 0.0 : v);
    std::string s = b;
    while (s.find('.') != std::string::npos && (s.back() == '0' || s.back() == '.')) { const bool dot = s.back() == '.'; s.pop_back(); if (dot) break; }
    return s;
}
inline std::string ColorLine(unsigned c)
{
    if (!c) return "";
    char b[32];
    std::snprintf(b, sizeof b, "color = %06X\n", c & 0xFFFFFF);
    return b;
}
inline std::string ExcludeLine(const std::vector<unsigned>& ids)
{
    if (ids.empty()) return "";
    std::string t = "exclude = ";
    for (std::size_t i = 0; i < ids.size(); ++i) { char b[16]; std::snprintf(b, sizeof b, "%s%08X", i ? ", " : "", ids[i]); t += b; }
    return t + "\n";
}
inline std::string Windows(const routeschedule::Schedule& s);
// The time rules lines of a route: busy words, plain windows or all day.
inline std::string ScheduleLines(const routeschedule::Schedule& s)
{
    if (!s.zones.empty()) {
        std::string t = "day = " + routeschedule::FormatDay(s) + "\npeak = " + std::to_string(s.peak) + "\nblend = " + std::to_string(s.blend) + "\nlevels =";
        for (const int l : s.levels) t += " " + std::to_string(l);
        return t + "\n";
    }
    if (s.allDay) return "density = " + std::to_string(std::clamp(s.allDayVehicles, 0, fvolimits::kVehicles)) + "\n";
    return "windows = " + Windows(s) + "\n";
}
inline std::string RoleLine(const std::string& role) { return role.empty() || role == "civilian" ? std::string() : "role = " + role + "\n"; }
inline std::string GroupLine(const std::string& group) { return group.empty() ? std::string() : "group = " + group + "\n"; }
// FVO_Traffic\\VehicleGroups.ini: the same on every planet.
inline std::string FormatGroups(const std::vector<trafficdata::RouteGroup>& groups)
{
    std::string t = "; FVO Traffic Manager: route groups and the vehicles each leaves out. The group named global applies to every route, on every planet.\n\n";
    for (const auto& g : groups) t += "[group." + g.name + "]\nlabel = " + CleanLabel(g.label) + "\n" + ExcludeLine(g.exclude) + "\n";
    return t;
}
inline std::string Windows(const routeschedule::Schedule& s)
{
    std::string out;
    for (const auto& w : s.windows) {
        if (!out.empty()) out += ", ";
        out += std::to_string(w.start) + " " + std::to_string(w.end) + " " + std::to_string(w.vehicles);
    }
    return out;
}
inline int TotalVehicles(const File& f)
{
    int n = 0;
    for (const auto& r : f.routes) {
        if (r.removed) continue;
        int most = r.schedule.allDay ? r.schedule.allDayVehicles : 0;
        for (const auto& w : r.schedule.windows) most = std::max(most, w.vehicles);
        n += most;
    }
    return std::clamp(n, 1, fvolimits::kVehicles);
}

inline std::string Format(const File& f)
{
    const std::string w = WorldKey(f.worldId);
    std::string t =
        "; FVO traffic file written by the map route editor (F7 on the local map). It is rewritten on every save:\n"
        "; edit routes in game, or copy them into a file of your own. Times are minutes of the planet's local day.\n\n"
        "[file]\nname = Map editor routes (" + f.worldId + ")\nenabled = 1\n\n"
        "[world." + w + "]\nid = " + f.worldId + "\n\n";
    for (const auto& r : f.routes) {
        const std::string rw = r.world.empty() ? w : r.world;
        if (r.removed) { t += "[route." + r.id + "]\nworld = " + rw + "\nremoved = 1\n\n"; continue; }
        if (r.ring) {
            t += "[route." + r.id + "]\nlabel = " + CleanLabel(r.label) + "\nworld = " + rw + "\ntype = ring\ncentre = " + Num(r.shape.cx) + " " + Num(r.shape.cy) +
                 "\nheight = " + Num(r.shape.z) + "\nradius = " + Num(r.shape.radius) + "\naspect = " + Num(r.shape.aspect) + "\nrotation = " +
                 Num(r.shape.rotation * 180.0 / ringlogic::kPi) + "\nlane_gap = " + Num(r.shape.gap) + "\ntilt = " + Num(r.tilt) + "\ntilt_toward = " + Num(r.tiltToward) +
                 "\nlanes = " + std::to_string(std::clamp(r.lanes, 1, 2)) + "\nspeed = " + Num(r.speed) + "\n";
            t += ScheduleLines(r.schedule) + ColorLine(r.color) + ExcludeLine(r.exclude) + RoleLine(r.role) + GroupLine(r.group);
            if (r.test) t += "test = 1\n";
            t += "\n";
            continue;
        }
        t += "[route." + r.id + "]\nlabel = " + CleanLabel(r.label) + "\nworld = " + rw + "\ntype = path\npoints = ";
        for (std::size_t i = 0; i < r.points.size(); ++i)
            t += (i ? ", " : "") + Num(r.points[i].x) + " " + Num(r.points[i].y) + " " + Num(r.points[i].z);
        if(!r.rounding.empty()){t+="\nrounding = ";for(std::size_t i=0;i<r.points.size();++i)t+=(i?" ":"")+Num(i<r.rounding.size()?r.rounding[i]:0);}
        t += "\nclosed = " + std::string(r.closed ? "1" : "0") + "\nlanes = " + std::to_string(std::clamp(r.lanes, 1, 2)) +
             "\nspeed = " + Num(r.speed) + "\nwidth = " + Num(r.width) + "\n";
        t += ScheduleLines(r.schedule) + ColorLine(r.color) + ExcludeLine(r.exclude) + RoleLine(r.role) + GroupLine(r.group);
            if (r.test) t += "test = 1\n";
        t += "\n";
    }
    for (const auto& k : f.links)
        t += "[link." + k.id + "]\nfrom = " + k.from + "\nat = " + Num(k.at) + "\nto = " + k.to + "\nto_at = " + (k.toEnd ? std::string("end") : k.toAt > 0 ? Num(k.toAt) : std::string("start")) +
             "\nshare = " + Num(k.share) + (k.radius>0?"\nradius = "+Num(k.radius):"") + (k.type.empty()?"":"\ntype = "+k.type) + "\n\n";
    for (const auto& p : f.landings)
    {
        std::string who;
        for (const auto& name : p.routes) who += (who.empty() ? "" : ", ") + name;
        t += "[place." + p.id + "]\nworld = " + (p.world.empty() ? w : p.world) + "\npoint = " + Num(p.at.x) + " " + Num(p.at.y) + " " + Num(p.at.z) + "\nradius = " + Num(p.radius) +
             (p.parked ? "\nparked = 1" : "") + "\nlayout = " + p.layout + "\nspaces = " + std::to_string(p.spaces) + "\nturn = " + Num(p.turn) +
             "\npurpose = " + p.purpose + "\nstay = " + Num(p.stay) + "\nfull = " + p.full + (who.empty() ? "" : "\nroutes = " + who) + "\n\n";
    }
    for (const auto& p : f.spawns)
        t += "[spawn." + p.id + "]\nworld = " + (p.world.empty() ? w : p.world) + "\npoint = " + Num(p.at.x) + " " + Num(p.at.y) + " " + Num(p.at.z) + "\nheading = " + Num(p.heading) + "\n\n";
    for (const auto& z : f.zones)
        t += "[zone." + z.name + "]\nworld = " + (z.world.empty() ? w : z.world) + "\npoint = " + Num(z.at.x) + " " + Num(z.at.y) + " " + Num(z.at.z) + "\nradius = " + Num(z.radius) +
             "\nkind = " + (z.slow ? "slow" : "keepout") + "\ntop = " + Num(z.top) + "\nspeed = " + Num(z.speed) + "\n\n";
    if (f.cityWritten)
        t += "[city." + w + "]\nworld = " + w + "\ntraffic = " + (f.city.on ? "1" : "0") + "\ndensity = " + std::to_string(f.city.density) + "\nlimit = " + std::to_string(f.city.limit) +
             "\noriginals = " + (f.city.originals ? "1" : "0") + "\n\n";
    if (f.traffic && !f.routes.empty())
        t += "[traffic." + w + "]\nworld = " + w + "\nmax = " + std::to_string(TotalVehicles(f)) + "\ngap = 20\nvehicles = any\ntrips = " +
             std::string(f.landings.empty() ? "laps" : "mixed") + "\nreach = 600\nstart_on_route = " + std::to_string(std::min<std::size_t>(f.spawns.size(), 6)) + "\n";
    return t;
}

// The editor's file back from its text (what Format wrote, or an older save). Anything that is not the editor's own
// shape (a ring, another world) is left out and named in `problems`.
inline bool Parse(const std::string& text, const std::string& name, File& out, std::vector<std::string>& problems)
{
    trafficdata::Data d;
    trafficdata::Read(text, name, d);
    problems = d.problems;
    File f;
    for (const auto& [key, id] : d.worlds) if (key.rfind("ed_", 0) == 0) { f.worldId = id; break; }
    if (f.worldId.empty()) { problems.push_back(name + ": no [world.ed_*] section"); return false; }
    const std::string w = WorldKey(f.worldId);
    for (const auto& r : d.routes) {
        Route e;
        e.id = r.name; e.label = r.label.empty() ? r.name : r.label; e.color = r.color; e.points = r.points; e.rounding=r.rounding; e.closed = r.closed;
        e.lanes = r.lanes; e.speed = r.speed; e.width = r.width; e.schedule = r.schedule; e.test = r.test; e.exclude = r.exclude; e.role = r.role; e.group = r.group;
        e.world = r.world == w ? std::string() : r.world;
        e.removed = r.removed; e.ring = r.ring; e.shape = r.shape; e.tilt = r.tilt; e.tiltToward = r.tiltToward;
        f.routes.push_back(e);
    }
    for (const auto& k : d.links) f.links.push_back({ k.name, k.from, k.to, k.at, k.toEnd, k.share, k.toAt, k.radius, k.type });
    for (const auto& p : d.places) {
        Spot s;
        s.id = p.name; s.at = p.at; s.radius = p.radius; s.parked = p.parked; s.world = p.world == w ? std::string() : p.world;
        s.layout = p.layout; s.purpose = p.purpose; s.full = p.full; s.spaces = p.spaces; s.turn = p.turn; s.stay = p.stay; s.routes = p.routes;
        f.landings.push_back(s);
    }
    for (const auto& q : d.spawns) {
        Spot s;
        s.id = q.name; s.at = q.at; s.heading = q.heading; s.radius = 0; s.world = q.world == w ? std::string() : q.world;
        f.spawns.push_back(s);
    }
    for (auto z : d.zones) { if (z.world == w) z.world.clear(); f.zones.push_back(z); }
    for (const auto& c : d.cities) if (c.world == w) { f.city = c; f.cityWritten = true; }
    f.traffic = false;
    for (const auto& t : d.traffic) if (t.world == w) f.traffic = true;
    out = std::move(f);
    return true;
}
}
