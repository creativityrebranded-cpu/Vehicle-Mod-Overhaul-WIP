#pragma once
// Junctions between traffic routes (2026-10-29, the user: "a ring with a connection between a sub route with less density").
// A [link] joins route `from` at `at` to route `to` at its start or end (trafficdata::Link). Seen from either route it is a
// junction: a point u on this route and where it leads (the other route and its u). Laps that pass a junction turn with
// probability `share`; at the end of an open branch the junction there is always taken; a branch with no junction at its
// far end is a spur (the vehicle lands there). The addon applies this in its trip step. Pure: tests/trafficlinks_test.cpp.
#include "trafficroute.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace trafficlinks {
struct Junction {
    double u = 0;            // on this route
    std::string other;       // the route it leads to
    double otherU = 0;       // where it arrives on that route
    double share = 0;        // the chance that a lap passing it turns
    std::string link;        // the [link] name (for the log)
    double radius=0;
};
// A link point on a route in its own u: rings in degrees (as exits), paths by point index; an end is the path's last index.
inline double U(const trafficdata::Route& r, double at) { return r.ring ? at / 360.0 * trafficroute::kRingSamples : trafficroute::Build(r).ToLine(at); }
inline double EndU(const trafficdata::Route& r, bool end)
{
    if (r.ring) return 0;
    return end && !r.points.empty() ? trafficroute::Build(r).Span() : 0.0;
}
inline const trafficdata::Route* Find(const std::vector<const trafficdata::Route*>& routes, const std::string& name)
{
    for (const auto* r : routes) if (r->name == name) return r;
    return nullptr;
}
// Every junction on route `name`, from the links whose both routes are in `routes` (the player's world).
inline std::vector<Junction> Of(const std::vector<trafficdata::Link>& links, const std::vector<const trafficdata::Route*>& routes, const std::string& name)
{
    std::vector<Junction> out;
    for (const auto& k : links) {
        const auto* a = Find(routes, k.from);
        const auto* b = Find(routes, k.to);
        if (!a || !b) continue;
        const double last = EndU(*b, true);
        const double ub = k.toEnd ? last : std::clamp(U(*b,k.toAt), 0.0, b->ring ? double(trafficroute::kRingSamples) : last);
        const double ua = U(*a, k.at);
        // The types only leave a way out of the list; what is left behaves as every junction did before types.
        if (k.from == name && k.type != "merge" && k.type != "crossing") out.push_back({ ua, k.to, ub, k.share, k.name, k.radius });
        if (k.to == name && k.type != "exit" && k.type != "crossing") out.push_back({ ub, k.from, ua, k.share, k.name, k.radius });
    }
    return out;
}
// The type a junction's shape asks for (2026-10-30, the user: a default based on the interaction, not one fixed answer). A one-way route
// that starts or ends at the other route only leaves or joins it; a two-way one does both. Two routes meeting mid-way swap
// traffic only when running side by side; at an angle (or head on) they cross. Empty: both ways.
inline std::string Suggested(const trafficroute::Line& from, int fromLanes, double ua, const trafficroute::Line& to, int toLanes, double ub)
{
    auto endOf = [](const trafficroute::Line& l, double u) { return l.closed ? 0 : u < 0.5 ? -1 : u > l.Span() - 0.5 ? 1 : 0; };
    const int toEnd = endOf(to, ub), fromEnd = endOf(from, ua);
    if (toEnd) return toLanes > 1 ? std::string() : toEnd < 0 ? "exit" : "merge";
    if (fromEnd) return fromLanes > 1 ? std::string() : fromEnd < 0 ? "merge" : "exit";
    auto heading = [](const trafficroute::Line& l, double u, double& x, double& y) {
        const auto a = trafficroute::At(l, 0, u - 0.05), b = trafficroute::At(l, 0, u + 0.05);
        x = b.x - a.x; y = b.y - a.y;
        const double n = std::hypot(x, y);
        if (n < 1e-9) return false;
        x /= n; y /= n;
        return true;
    };
    double ax = 0, ay = 0, bx = 0, by = 0;
    if (!heading(from, ua, ax, ay) || !heading(to, ub, bx, by)) return {};
    double along = ax * bx + ay * by;
    if (fromLanes > 1 || toLanes > 1) along = std::fabs(along);  // a two-way route runs both ways along that line
    return along > 0.82 ? std::string() : "crossing";  // within 35 degrees: side by side
}
// The lane a vehicle takes on route `q` arriving at uQ: a ring or the middle of a path, lane 0; a path's start, lane 0 (it
// runs forward); a path's end, lane 1 (back along it) on a two-way path, none (-1) on a one-way path, whose end is its exit.
inline int EntryLane(const trafficroute::Line& q, int lanes, double uQ)
{
    if (q.closed || q.Count() < 2) return 0;
    if (uQ > q.Span() - 0.5) return lanes > 1 ? 1 : -1;
    return 0;
}
// The lane passed u = at while moving from `from` to `to` this step (in its direction of travel; steps are short).
inline bool Crossed(const trafficroute::Line& l, int lane, double from, double to, double at)
{
    const double moved = trafficroute::Ahead(l, lane, from, to), reach = trafficroute::Ahead(l, lane, from, at);
    return std::isfinite(moved)&&moved>0&&trafficroute::DistanceAlong(l,lane,from,to)<150&&std::isfinite(reach)&&reach>0&&reach<=moved;
}
// The end an open path's lane runs to (lane 0: its last point, lane 1: its first).
inline double LaneEnd(const trafficroute::Line& l, int lane) { return lane == 1 ? 0.0 : l.Span(); }
// The junction at u (within half a point), if any.
inline const Junction* At(const std::vector<Junction>& js, double u)
{
    for (const auto& j : js) if (std::fabs(j.u - u) < 0.5) return &j;
    return nullptr;
}
// A rounded network transition, shared by the map preview and the actual trip.
inline std::vector<trafficroute::Vec> TurnPath(const trafficroute::Line& from,int inLane,double at,const trafficroute::Line& to,int outLane,double otherAt,double radius){
    if(!(radius>0))return {};
    const auto a=trafficroute::At(from,inLane,trafficroute::LeadU(from,inLane,at,2.5*radius,true));
    const auto b=trafficroute::At(from,inLane,at);
    const auto c=trafficroute::At(to,outLane,trafficroute::LeadU(to,outLane,otherAt,2.5*radius));
    return routecurve::Build({a,b,c},{0,radius,0},false).points;
}
// Deterministic per-vehicle dice (xorshift): 0 .. 1.
inline double Roll(std::uint64_t& state)
{
    if (!state) state = 0x9E3779B97F4A7C15ull;
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return double(state >> 11) / double(1ull << 53);
}
}
