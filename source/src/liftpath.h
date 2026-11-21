#pragma once
#include "trafficroute.h"
// Lifts (2026-11-06). A lift is a shaft: one opening, on the route's own line. Lanes keep their sideways offset everywhere else, and
// on a vertical tile that offset is mitred out to about 9 m (15:24 run: lane 1 climbed at x -346 under an overhang, the shaft is at
// -354.7, while lane 0, 1 m from the line, rose at 3 m/s). So every lane is pulled onto the route line on a lift tile, easing in
// over the last kBlend metres of lane on either side. The route file and the road field's tiles stay as authored.
namespace liftpath {
constexpr double kBlend=25.0;
// A lift tile: open air, or a climb steeper than 35 degrees (rise over 3 m and over 0.7 of the run).
inline bool IsLift(const trafficdata::Route& r,int tile){
    const int n=int(r.points.size());if(n<2||tile<0||tile>=n)return false;
    if(!r.closed&&tile==n-1)return false;
    if(r.road.enabled&&roadplan::PrimitiveAt(r.road,std::size_t(tile)).kind==roadplan::Open)return true;
    const auto& a=r.points[std::size_t(tile)];const auto& b=r.points[std::size_t((tile+1)%n)];
    const double run=std::hypot(b.x-a.x,b.y-a.y),rise=std::fabs(b.z-a.z);
    return rise>3.0&&rise>run*.7;
}
struct Pull {double weight=0,x=0,y=0;};  // weight 1: on the route's line; 0: the lane's own position
inline Pull At(const trafficdata::Route& r,const trafficroute::Line& line,int lane,double u){
    Pull out;const int n=int(r.points.size());if(n<2||line.error)return out;
    const double source=line.ToSource(u);const int t=std::clamp(int(source),0,n-1);
    if(IsLift(r,t)){const auto& a=r.points[std::size_t(t)];const auto& b=r.points[std::size_t((t+1)%n)];const double f=std::clamp(source-t,0.0,1.0);
        out.weight=1;out.x=a.x+(b.x-a.x)*f;out.y=a.y+(b.y-a.y)*f;return out;}
    auto toward=[&](int lift,int mouth){  // the lift tile next to this one, and the marker where it opens
        if(!IsLift(r,lift))return;
        const double at=line.ToLine(double(mouth));
        const double d=std::min(trafficroute::DistanceAlong(line,lane,u,at),trafficroute::DistanceAlong(line,lane,at,u));
        if(d>=kBlend)return;
        const double k=1-d/kBlend,w=k*k*(3-2*k);
        if(w>out.weight){out.weight=w;out.x=r.points[std::size_t(mouth)].x;out.y=r.points[std::size_t(mouth)].y;}};
    if(t+1<n||r.closed)toward((t+1)%n,(t+1)%n);
    if(t>0||r.closed)toward((t+n-1)%n,t);
    return out;
}
inline trafficdata::Vec Apply(const Pull& p,trafficdata::Vec v){v.x+=(p.x-v.x)*p.weight;v.y+=(p.y-v.y)*p.weight;return v;}
}
