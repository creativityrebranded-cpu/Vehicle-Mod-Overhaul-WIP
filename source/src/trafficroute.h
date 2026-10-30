#pragma once
// Trips along an FVO traffic route (trafficdata.h), 2026-10-28; replaces ringtrip.h's one-lap ring trip. A route is two lanes
// sampled as lines with one shared position u (point index + fraction): lane 0 travels with increasing u, lane 1 against it.
// A ring's lane 1 is lane_gap higher; a path's lanes sit width/2 to either side. Legs:
//   Climb   straight up over the start to the lane's height;
//   Join    across to the nearest point of the lane;
//   Follow  along the lane (a point kLead ahead) to the target: the exit nearest the destination, or home after a full lap;
//   Leave   across to above the destination at lane height;
//   Done    the caller lands and parks there.
// Pure: tests/trafficroute_test.cpp.
#include "trafficdata.h"
#include "routecurve.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace trafficroute {
using trafficdata::Vec;
constexpr int kRingSamples = 96;
constexpr double kLead = 70.0, kReach = 25.0, kClimbSlack = 8.0, kClimbLean = 20.0;

struct Line {
    bool closed = true;
    std::vector<Vec> lane[2];
    std::vector<double> authored;
    int Count() const { return int(lane[0].size()); }
    double Span() const { return closed ? double(Count()) : double(Count() - 1); }  // u runs [0, Span)
    double ToLine(double source) const {
        if(authored.empty())return source;
        const double span=closed?double(authored.size()):double(authored.size()-1);
        source=std::clamp(source,0.0,span);const auto i=std::min(std::size_t(source),authored.size()-1);
        const double end=i+1<authored.size()?authored[i+1]:Span();
        return authored[i]+(end-authored[i])*(source-double(i));
    }
    double ToSource(double u) const {
        if(authored.empty())return u;
        u=std::clamp(u,0.0,Span());auto it=std::upper_bound(authored.begin(),authored.end(),u);
        const auto i=it==authored.begin()?0:std::size_t(it-authored.begin()-1);
        const double end=i+1<authored.size()?authored[i+1]:Span();
        return double(i)+(end>authored[i]?(u-authored[i])/(end-authored[i]):0);
    }
};

inline Line Build(const trafficdata::Route& r)
{
    Line l;
    if (r.ring) {
        l.closed = true;
        const double toward = r.tiltToward * ringlogic::kPi / 180.0, slope = std::tan(r.tilt * ringlogic::kPi / 180.0);
        for (int i = 0; i < kRingSamples; ++i) {
            const auto p = ringlogic::At(r.shape, 2.0 * ringlogic::kPi * i / kRingSamples);
            // slanted: higher toward `tiltToward`, by the distance from the centre along that direction
            const double z = p.z + slope * ((p.x - r.shape.cx) * std::sin(toward) + (p.y - r.shape.cy) * std::cos(toward));
            l.lane[0].push_back({ p.x, p.y, z });
            l.lane[1].push_back({ p.x, p.y, z + r.shape.gap });
        }
        return l;
    }
    l.closed = r.closed;
    const auto curve=routecurve::Build(r.points,r.rounding,r.closed);l.authored=curve.authored;
    const auto& pts = curve.points;
    const std::size_t n = pts.size();
    for (std::size_t i = 0; i < n; ++i) {
        const Vec& a = pts[i > 0 ? i - 1 : (l.closed ? n - 1 : i)];
        const Vec& b = pts[i + 1 < n ? i + 1 : (l.closed ? 0 : i)];
        double dx = b.x - a.x, dy = b.y - a.y;
        const double run = std::hypot(dx, dy);
        if (run > 1e-6) { dx /= run; dy /= run; }
        const double side = r.lanes > 1 ? r.width * 0.5 : 0.0;  // lane 0 on the right of travel (x right, y forward)
        l.lane[0].push_back({ pts[i].x + dy * side, pts[i].y - dx * side, pts[i].z });
        l.lane[1].push_back({ pts[i].x - dy * side, pts[i].y + dx * side, pts[i].z });
    }
    return l;
}

inline double Wrap(const Line& l, double u)
{
    const double s = l.Span();
    if (s <= 0) return 0;
    if (l.closed) { u = std::fmod(u, s); return u < 0 ? u + s : u; }
    return std::clamp(u, 0.0, s);
}
inline Vec At(const Line& l, int lane, double u)
{
    const auto& p = l.lane[lane & 1];
    if (p.empty()) return {};
    u = Wrap(l, u);
    const int i = int(std::floor(u)), n = int(p.size());
    const int j = l.closed ? (i + 1) % n : std::min(i + 1, n - 1);
    const double f = u - i;
    const Vec& a = p[std::min(i, n - 1)];
    const Vec& b = p[j];
    return { a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f };
}
// The u of the lane point nearest (x, y).
inline double Nearest(const Line& l, int lane, double x, double y)
{
    const auto& p = l.lane[lane & 1];
    double best = 0, bestD = std::numeric_limits<double>::infinity();
    const int n = int(p.size()), segs = l.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) {
        const Vec& a = p[i];
        const Vec& b = p[(i + 1) % n];
        const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        const double f = len2 > 1e-9 ? std::clamp(((x - a.x) * dx + (y - a.y) * dy) / len2, 0.0, 1.0) : 0.0;
        const double d = std::hypot(a.x + dx * f - x, a.y + dy * f - y);
        if (d < bestD) { bestD = d; best = i + f; }
    }
    return best;
}
inline double SegmentLength(const Line& l, double u)
{
    const Vec a = At(l, 0, std::floor(Wrap(l, u))), b = At(l, 0, std::floor(Wrap(l, u)) + 1.0);
    return std::max(1.0, std::hypot(b.x - a.x, b.y - a.y));
}
inline double Direction(int lane) { return lane == 1 ? -1.0 : 1.0; }
// How far (in u) the lane travels from `from` to `to`; infinity when an open path's lane never gets there.
inline double Ahead(const Line& l, int lane, double from, double to)
{
    double d = (to - from) * Direction(lane);
    if (l.closed) { d = std::fmod(d, l.Span()); if (d < 0) d += l.Span(); return d; }
    return d >= 0 ? d : std::numeric_limits<double>::infinity();
}
// The lane reaching `to` from `from` sooner (one-lane routes: lane 0).
inline int BestLane(const Line& l, int lanes, double from, double to)
{
    if (lanes < 2) return 0;
    return Ahead(l, 1, from, to) < Ahead(l, 0, from, to) ? 1 : 0;
}
inline double Length(const Line& l)
{
    double s = 0;
    const auto& p = l.lane[0];
    const int n = int(p.size()), segs = l.closed ? n : n - 1;
    for (int i = 0; i < segs; ++i) s += std::hypot(p[(i + 1) % n].x - p[i].x, p[(i + 1) % n].y - p[i].y);
    return s;
}

// Move by metres along the polyline, not a fraction of whichever segment the
// vehicle currently occupies. Do not cut diagonally across sharp corners.
inline double LeadU(const Line& l,int lane,double u,double metres,bool reverse=false){
    const double dir=Direction(lane)*(reverse?-1:1);u=Wrap(l,u);
    for(int step=0;step<l.Count()+1&&metres>0.001;++step){
        double edge=dir>0?std::floor(u+1e-6)+1:std::ceil(u-1e-6)-1;
        if(!l.closed)edge=std::clamp(edge,0.0,l.Span());
        const Vec a=At(l,lane,u),b=At(l,lane,edge);
        const double length=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);
        if(length>=metres)return Wrap(l,u+(edge-u)*metres/length);
        if(length<0.001&&edge==u)return u;
        const Vec c=At(l,lane,edge+dir*0.01);
        const double ax=b.x-a.x,ay=b.y-a.y,bx=c.x-b.x,by=c.y-b.y;
        const double norms=std::hypot(ax,ay)*std::hypot(bx,by);
        // Inside 4 m, lead just beyond the vertex so it can be traversed;
        // otherwise target the corner, rather than a chord through a building.
        if(norms>1e-6&&(ax*bx+ay*by)/norms<0.866&&length>4)return Wrap(l,edge);
        metres-=length;u=Wrap(l,edge);
    }
    return u;
}
inline double DistanceAlong(const Line& l,int lane,double from,double to){
    double remaining=Ahead(l,lane,from,to);if(!std::isfinite(remaining))return remaining;
    double result=0;double u=Wrap(l,from);const double dir=Direction(lane);
    for(int i=0;i<l.Count()+1&&remaining>1e-6;++i){
        const double edge=dir>0?std::floor(u+1e-6)+1:std::ceil(u-1e-6)-1;
        const double part=std::min(remaining,std::fabs(edge-u));
        const auto a=At(l,lane,u),b=At(l,lane,u+dir*part);
        result+=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);remaining-=part;u=Wrap(l,u+dir*part);
    }
    return result;
}

enum class Leg { Climb, Join, Follow, Leave, Done };
inline const char* Name(Leg l)
{
    switch (l) {
    case Leg::Climb: return "climbing to the lane";
    case Leg::Join: return "joining the lane";
    case Leg::Follow: return "following the route";
    case Leg::Leave: return "leaving the route";
    default: return "done";
    }
}
struct Trip {
    Leg leg = Leg::Climb;
    int lane = 0;
    bool lap = true;               // lap and home, else to `dest`
    double targetU = 0;            // where Follow ends
    double lastU = 0, travelled = 0;
    bool uKnown = false;
    Vec home, dest;
    double laneZ = 0;              // the lane's height at the join
};
struct Goal { double x = 0, y = 0, z = 0, stop = 6.0; bool passThrough = false; };

// A lap from home: join the lane nearest home, once round (a path: to its end and back is not a lap; it goes to the end).
inline Trip StartLap(const Line& l, int lane, Vec home)
{
    Trip t; t.lane = lane & 1; t.lap = true; t.home = home;
    t.targetU = Nearest(l, t.lane, home.x, home.y);
    if (!l.closed) t.targetU = t.lane == 0 ? l.Span() : 0.0;  // a path's lap: to the end of its lane
    return t;
}
// To a destination: leave at `exitU` (the exit nearest it), on the lane that gets there sooner.
inline Trip StartTo(const Line& l, int lanes, Vec home, Vec dest, double exitU)
{
    Trip t; t.lap = false; t.home = home; t.dest = dest; t.targetU = exitU;
    t.lane = BestLane(l, lanes, Nearest(l, 0, home.x, home.y), exitU);
    return t;
}

inline Goal Step(const Line& l, Trip& t, double x, double y, double z)
{
    Goal g;
    double joinU = Nearest(l, t.lane, x, y);
    if(t.leg==Leg::Follow&&t.uKnown){
        const auto old=At(l,t.lane,t.lastU);
        const double direct=std::hypot(old.x-x,old.y-y,old.z-z),limit=std::max(20.0,2*direct+5);
        const double along=std::min(DistanceAlong(l,t.lane,t.lastU,joinU),DistanceAlong(l,t.lane,joinU,t.lastU));
        if(along>limit)joinU=t.lastU; // don't jump to another arm of a crossing route
    }
    const Vec join = At(l, t.lane, joinU);
    if (t.leg == Leg::Climb) {
        if (z >= join.z - kClimbSlack) { t.leg = Leg::Join; }
        else {
            // Up, leaning up to kClimbLean toward the lane: a goal straight overhead is 0 m away sideways, which the autopilot took
            // for "arrived" in a settlement and stayed on the ground (2026-10-28 18:11, a cold-start vehicle idle 131 s at its spot).
            const double dx = join.x - t.home.x, dy = join.y - t.home.y, d = std::hypot(dx, dy);
            const double k = d > 1.0 ? std::min(kClimbLean, d) / d : 0.0;
            g.x = t.home.x + dx * k; g.y = t.home.y + dy * k; g.z = join.z; g.stop = 4.0;
            return g;
        }
    }
    if (t.leg == Leg::Join) {
        if (std::hypot(join.x - x, join.y - y) < kReach && std::fabs(z - join.z) < kReach) {
            t.leg = Leg::Follow; t.lastU = joinU; t.uKnown = true; t.travelled = 0;
        }
        else { g.x = join.x; g.y = join.y; g.z = join.z; g.passThrough = true; return g; }
    }
    if (t.leg == Leg::Follow) {
        const double u = joinU;
        bool crossedTarget=false;
        if (t.uKnown) {
            double step = (u - t.lastU) * Direction(t.lane);
            if (l.closed) step = std::remainder(step, l.Span());
            if (step > 0 && DistanceAlong(l,t.lane,t.lastU,u)<150.0){
                const double target=Ahead(l,t.lane,t.lastU,t.targetU);
                crossedTarget=std::isfinite(target)&&target<=step+1e-6;
                t.travelled+=step;
            }
        }
        t.lastU = u; t.uKnown = true;
        const double left = Ahead(l, t.lane, u, t.targetU);
        const bool lapDone = !t.lap || !l.closed || t.travelled >= l.Span() * 0.9;
        const bool there = lapDone && (crossedTarget || left * SegmentLength(l, u) < kReach || (!l.closed && left == 0.0));
        if (there) t.leg = Leg::Leave;
        else {
            const double leadU = LeadU(l,t.lane,u,kLead);
            const Vec p = At(l, t.lane, leadU);
            g.x = p.x; g.y = p.y; g.z = p.z; g.passThrough = true;
            t.laneZ = p.z;
            return g;
        }
    }
    const Vec to = t.lap ? t.home : t.dest;
    if (t.leg == Leg::Leave) {
        if (std::hypot(to.x - x, to.y - y) < kReach) t.leg = Leg::Done;
        else { g.x = to.x; g.y = to.y; g.z = t.laneZ ? t.laneZ : join.z; g.stop = 6.0; return g; }
    }
    g.x = to.x; g.y = to.y; g.z = z; g.stop = 6.0;
    return g;
}

}
