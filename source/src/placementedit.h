#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Placement mode (2026-10-24, the user): with the settings toggle on, the scanner's T saves a parking spot (tap), deletes
// the aimed one (hold) or makes the aimed spot the area's summon point (double tap). A far or unloaded summon then puts
// the player's vehicle on the area's summon point; a close one still flies to the player. The pure part: which saved
// spot an aim means, one summon point per area, and which summon point a summon uses. realvehicles.cpp does the files.
namespace placementedit {
constexpr double kPickRadius = 5.0;    // m: a delete takes the saved spot nearest the aim within this
constexpr double kPickDz = 3.0;        // m: and on about the same level
constexpr double kAreaRadius = 500.0;  // m: one summon point per area; a summon within this of one uses it

struct Point { double x = 0, y = 0, z = 0; };
struct SummonPoint { std::string world; double x = 0, y = 0, z = 0, yaw = 0; };

inline double Flat(double ax, double ay, double bx, double by) { return std::hypot(ax - bx, ay - by); }
inline bool Finite(const Point& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

// The saved spot the aim means: the nearest within kPickRadius (flat) and kPickDz (height), or -1.
inline int Pick(const std::vector<Point>& spots, const Point& aim, double radius = kPickRadius, double dz = kPickDz)
{
    if (!Finite(aim)) return -1;
    int best = -1;
    double bestD = radius;
    for (std::size_t i = 0; i < spots.size(); ++i) {
        const auto& s = spots[i];
        if (!Finite(s) || std::fabs(s.z - aim.z) > dz) continue;
        const double d = Flat(s.x, s.y, aim.x, aim.y);
        if (d <= bestD) { bestD = d; best = int(i); }
    }
    return best;
}

// The summon point for a summon at (x, y) in `world`: the nearest within kAreaRadius, or -1.
inline int SummonFor(const std::vector<SummonPoint>& points, const std::string& world, double x, double y, double radius = kAreaRadius)
{
    if (world.empty() || !std::isfinite(x) || !std::isfinite(y)) return -1;
    int best = -1;
    double bestD = radius;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& p = points[i];
        if (p.world != world || !std::isfinite(p.x) || !std::isfinite(p.y)) continue;
        const double d = Flat(p.x, p.y, x, y);
        if (d <= bestD) { bestD = d; best = int(i); }
    }
    return best;
}

// Setting a summon point replaces the area's old one (any in the same world within kAreaRadius); returns how many went.
inline int SetSummon(std::vector<SummonPoint>& points, const SummonPoint& p, double radius = kAreaRadius)
{
    int removed = 0;
    for (std::size_t i = points.size(); i-- > 0;) {
        if (points[i].world == p.world && Flat(points[i].x, points[i].y, p.x, p.y) <= radius) { points.erase(points.begin() + std::ptrdiff_t(i)); ++removed; }
    }
    points.push_back(p);
    return removed;
}

// summon-points.tsv: one line per point, "world<TAB>x<TAB>y<TAB>z<TAB>yaw" (world = the worldspace's editor id, which,
// unlike a generated planet's runtime id, stays the same between sessions).
inline std::string Line(const SummonPoint& p)
{
    char buf[96];
    std::snprintf(buf, sizeof(buf), "\t%.3f\t%.3f\t%.3f\t%.5f", p.x, p.y, p.z, p.yaw);
    return p.world + buf;
}
inline bool Parse(const std::string& line, SummonPoint& out)
{
    const auto tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) return false;
    SummonPoint p;
    p.world = line.substr(0, tab);
    if (std::sscanf(line.c_str() + tab + 1, "%lf\t%lf\t%lf\t%lf", &p.x, &p.y, &p.z, &p.yaw) != 4) return false;
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) || !std::isfinite(p.yaw)) return false;
    out = p;
    return true;
}
}
