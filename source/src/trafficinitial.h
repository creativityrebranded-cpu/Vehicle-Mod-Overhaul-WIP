#pragma once
#include "trafficroute.h"
#include "tilebox.h"
#include "fvolimits.h"
#include <algorithm>
#include <string>
#include <vector>

namespace trafficinitial {
namespace rules {
inline constexpr double fallbackStep = 6.0, maxFallback = 18.0, fallbackFraction = .10;
// LV-2 (2026-11-20): a vertex is a corner for births only when the road turns at least this much there. A bend of a few degrees (the ring road's
// ramps and long sweeps have vertices every 20-40 m) is a straight road to a car; blocking +-16 m round each of them left 16 of 61 ring slots
// with no place (refusal kilometre_slot_no_field_room). Real corners (road_hybrid_test: 90 degrees) keep the whole buffer.
inline constexpr double cornerTurnRadians = 3.141592653589793 / 12;  // 15 degrees
inline constexpr double gapFraction = 1 - 2 * fallbackFraction;
}
struct Backup {trafficroute::Vec at;double heading=0,u=0;};
struct Slot {
    trafficroute::Vec at;
    double heading=0;
    std::string route;
    int lane=0;
    double u=0;
    std::vector<Backup> backups{};
    double interval=0;
    unsigned coverage=0;
    bool eligible=true;
};
inline std::vector<double> RoadStations(const trafficroute::Line& line) {
    if(!line.distance.empty())return line.distance;
    std::vector<double> arc{0};
    for(int i=0;i<int(line.Span());++i){
        const auto a=tilebox::RouteAt(line,i),b=tilebox::RouteAt(line,i+1);
        arc.push_back(arc.back()+std::hypot(b.x-a.x,b.y-a.y,b.z-a.z));
    }
    return arc;
}
inline double StationAt(const std::vector<double>& arc,double u) {
    u=std::clamp(u,0.0,double(arc.size()-1));
    const auto i=std::size_t(u);
    return i+1<arc.size()?arc[i]+(arc[i+1]-arc[i])*(u-i):arc.back();
}
inline double AtStation(const std::vector<double>& arc,double metres) {
    const auto it=std::upper_bound(arc.begin(),arc.end(),metres);
    if(it==arc.end())return double(arc.size()-1);
    if(it==arc.begin())return 0;
    const auto i=std::size_t(it-arc.begin()-1);
    return double(i)+(metres-arc[i])/(arc[i+1]-arc[i]);
}
inline double StationGap(double a,double b,double length,bool closed) {
    const double gap=std::fabs(a-b);
    return closed?std::min(gap,std::max(0.0,length-gap)):gap;
}
inline double MinimumGap(double interval) {return interval*rules::gapFraction;}
inline bool GapClear(double a,double b,double length,bool closed,double interval) {
    return StationGap(a,b,length,closed)+1e-6>=MinimumGap(interval);
}
using Blocks = std::vector<std::pair<double,double>>;
// How far the road turns at authored point i (radians, 0 = straight on); 0 for an end or a degenerate vertex.
inline double TurnAt(const std::vector<trafficdata::Vec>& pts,std::size_t i,bool closed) {
    if(pts.size()<3||i>=pts.size()||(!closed&&(i==0||i+1==pts.size())))return 0;
    const auto a=pts[(i+pts.size()-1)%pts.size()],b=pts[i],c=pts[(i+1)%pts.size()];
    const double first=std::hypot(a.x-b.x,a.y-b.y),second=std::hypot(c.x-b.x,c.y-b.y);
    if(first<.1||second<.1)return 0;
    const double interior=std::acos(std::clamp(((a.x-b.x)*(c.x-b.x)+(a.y-b.y)*(c.y-b.y))/(first*second),-1.0,1.0));
    return 3.141592653589793-interior;
}
// laneOut / distanceOut (optional): the lane the point is nearest to and its 3D distance from that lane's line (the dot simulation needs both).
inline bool ProjectRoadStation(const trafficdata::Route& route,const trafficroute::Line& line,
    const std::vector<double>& arc,trafficroute::Vec point,double& metres,int* laneOut=nullptr,double* distanceOut=nullptr) {
    const int tiles=tilebox::TileCount(route);
    if(tiles<=0)return false;
    double best=std::numeric_limits<double>::infinity();
    for(int lane:trafficroute::Lanes(line,route.lanes)){
        const double u=trafficroute::NearestProgress(line,lane,point.x,point.y,point.z,false,0);
        if(!trafficroute::Available(line,lane,u))continue;
        const auto on=trafficroute::At(line,lane,u);
        const double distance=std::hypot(on.x-point.x,on.y-point.y,on.z-point.z);
        if(distance>=best)continue;
        const int tile=std::clamp(int(line.ToSource(u)),0,tiles-1);
        FvoFieldBoxV1 field;fieldbox::Box box;
        if(!tilebox::TileBox(route,line,tile,field)||!fieldboxapi::ToBox(field,box)||
           !fieldbox::Contains(box,{point.x,point.y,point.z}))continue;
        best=distance;metres=StationAt(arc,u);if(laneOut)*laneOut=lane;if(distanceOut)*distanceOut=distance;
    }
    return std::isfinite(best);
}
inline Blocks RoadBlocks(const trafficdata::Route& r,const trafficroute::Line& line,
    const std::vector<double>& arc,const Blocks& junctions) {
    Blocks blocked;
    const double buffer=std::max(10.0,r.corridor*2);
    const auto block=[&](double at,double radius){
        blocked.push_back({at-radius,at+radius});
        if(line.closed){
            blocked.push_back({at-radius-arc.back(),at+radius-arc.back()});
            blocked.push_back({at-radius+arc.back(),at+radius+arc.back()});
        }
    };
    if(!line.closed){block(0,buffer);block(arc.back(),buffer);}
    for(std::size_t i=0;i<r.points.size();++i){
        const double corner=TurnAt(r.points,i,r.closed)>=rules::cornerTurnRadians?routecurve::CornerLimit(r.points,i,r.closed):0;
        if(corner>0)block(StationAt(arc,line.ToLine(double(i))),buffer+std::min(r.corridor,corner));
    }
    for(auto [u,radius]:junctions)block(StationAt(arc,line.ToLine(u)),std::max(buffer,radius));
    return blocked;
}
inline bool RoadLaneAvailable(const trafficroute::Line& line,int lane,double u) {
    const auto segment=std::size_t(u);
    return segment<std::size_t(line.Span())&&
        (line.road.empty()||line.road[segment].primitive.kind!=roadplan::Open)&&
        trafficroute::Available(line,lane,u);
}
inline unsigned CoverageRank(unsigned index,unsigned count) {
    // Reverse the significant bits: for eight stations the order is 0,4,2,6,1,5,3,7.
    unsigned rank=0;
    for(unsigned span=1;span<count;span*=2){rank=(rank<<1)|(index&1);index>>=1;}
    return rank;
}
inline std::vector<Slot> RoadByKilometre(const trafficdata::Route& r,int count,const Blocks& junctions={}) {
    std::vector<Slot> out;
    const auto line=trafficroute::Build(r);
    const auto lanes=trafficroute::Lanes(line,r.lanes);
    if(line.Count()<2||lanes.empty()||count<1)return out;
    const auto arc=RoadStations(line);
    if(!std::isfinite(arc.back())||arc.back()<=0)return out;
    count=std::min(count,fvolimits::kVehicles);
    const double interval=arc.back()/count;
    const double reach=std::min(rules::maxFallback,interval*rules::fallbackFraction);
    const auto blocks=RoadBlocks(r,line,arc,junctions);
    std::vector<int> forward,reverse;
    for(int lane:lanes)(trafficroute::Direction(line,lane)>0?forward:reverse).push_back(lane);
    std::vector<double> offsets{0};
    for(double delta=std::min(reach,rules::fallbackStep);delta>0;delta=std::min(reach,delta+rules::fallbackStep)){
        offsets.push_back(delta);offsets.push_back(-delta);
        if(delta==reach)break;
    }
    for(int index=0;index<count;++index){
        const double centre=interval*(index+.5);
        const auto& direction=!reverse.empty()&&(forward.empty()||index%2)?reverse:forward;
        const int preferred=direction[(forward.empty()||reverse.empty()?index:index/2)%direction.size()];
        const auto seed=[&](int lane,double metres){
            const double u=AtStation(arc,metres);
            const auto a=trafficroute::At(line,lane,u),b=trafficroute::At(line,lane,u+trafficroute::Direction(line,lane)*.001);
            return Backup{a,std::atan2(b.x-a.x,b.y-a.y),u};
        };
        const auto eligible=[&](int lane,double metres){
            return RoadLaneAvailable(line,lane,AtStation(arc,metres))&&
                std::none_of(blocks.begin(),blocks.end(),[&](const auto& b){return metres>b.first&&metres<b.second;});
        };
        const auto nominal=seed(preferred,centre);
        Slot slot{nominal.at,nominal.heading,r.name,preferred,nominal.u,{},interval,CoverageRank(index,count),false};
        for(double offset:offsets){
            const double at=centre+offset;
            if(slot.eligible){
                if(eligible(slot.lane,at))slot.backups.push_back(seed(slot.lane,at));
                continue;
            }
            int lane=preferred;
            if(!eligible(lane,at)){
                const auto alternate=std::find_if(lanes.begin(),lanes.end(),[&](int l){return eligible(l,at);});
                if(alternate==lanes.end())continue;
                lane=*alternate;
            }
            const auto selected=seed(lane,at);
            slot.at=selected.at;slot.heading=selected.heading;slot.u=selected.u;slot.lane=lane;slot.eligible=true;
        }
        out.push_back(std::move(slot));
    }
    return out;
}
inline double AtMetres(const trafficroute::Line& l,int lane,double metres){
    const auto& p=l.lane[lane];const int segments=l.closed?int(p.size()):int(p.size())-1;
    for(int i=0;i<segments;++i){const auto&a=p[i];const auto&b=p[(i+1)%p.size()];const double d=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);
        if(metres<=d&&d>0.001)return i+metres/d;metres-=d;}
    return l.Span();
}
inline std::vector<Slot> Route(const trafficdata::Route& r,int count,const std::vector<std::pair<double,double>>& junctions={}){
    std::vector<Slot> out;const auto line=trafficroute::Build(r);if(line.Count()<2||count<1)return out;
    if(r.road.enabled||r.travel==routepolicy::Travel::Drive){
        struct Span{int lane;double begin,end;};std::vector<Span> spans;double total=0;
        for(int lane:trafficroute::Lanes(line,r.lanes)){
            std::vector<double> arc{0};for(int i=0;i<int(line.Span());++i){auto a=trafficroute::At(line,lane,i),b=trafficroute::At(line,lane,i+1);arc.push_back(arc.back()+std::hypot(b.x-a.x,b.y-a.y,b.z-a.z));}
            const auto blocked=RoadBlocks(r,line,arc,junctions);
            for(int i=0;i<int(line.Span());++i){
                if(!RoadLaneAvailable(line,lane,i+.5))continue;
                std::vector<std::pair<double,double>> intervals{{arc[i],arc[i+1]}};
                for(auto [a,b]:blocked){std::vector<std::pair<double,double>> remaining;for(auto [lo,hi]:intervals){if(b<=lo||a>=hi)remaining.push_back({lo,hi});else {if(a>lo)remaining.push_back({lo,a});if(b<hi)remaining.push_back({b,hi});}}intervals=std::move(remaining);}
                for(auto [lo,hi]:intervals)if(hi-lo>.001){spans.push_back({lane,lo,hi});total+=hi-lo;}
            }
        }
        if(total<=0)return out;count=std::min(count,fvolimits::kVehicles);
        for(int i=0;i<count;++i){double distance=total*(i+.5)/count;for(const auto& span:spans){const double length=span.end-span.begin;if(distance>length){distance-=length;continue;}const double u=AtMetres(line,span.lane,span.begin+distance);const auto a=trafficroute::At(line,span.lane,u),b=trafficroute::At(line,span.lane,u+trafficroute::Direction(line,span.lane)*.001);out.push_back({a,std::atan2(b.x-a.x,b.y-a.y),r.name,span.lane,u});
            // Backup spots (2026-11-06): 6 m steps either way along the same lane, only inside the stretches a car may start on.
            const double at=span.begin+distance;for(int step=1;step<=6;++step){const double metres=at+6.0*((step+1)/2)*(step%2?1:-1);
                const bool allowed=std::any_of(spans.begin(),spans.end(),[&](const Span& s){return s.lane==span.lane&&metres>=s.begin&&metres<=s.end;});if(!allowed)continue;
                const double v=AtMetres(line,span.lane,metres);const auto c=trafficroute::At(line,span.lane,v),d=trafficroute::At(line,span.lane,v+trafficroute::Direction(line,span.lane)*.001);
                out.back().backups.push_back({c,std::atan2(d.x-c.x,d.y-c.y)});}
            break;}}
        return out;
    }
    if(r.lanePlan.enabled) {
        struct Span {int lane,segment;double length;};std::vector<Span> spans;double total=0;
        for(int lane:line.lanes)for(int i=0;i<int(line.Span());++i)if(trafficroute::Available(line,lane,i+.5)) {
            const auto a=trafficroute::At(line,lane,i),b=trafficroute::At(line,lane,i+1);const double length=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);
            if(length>.01){spans.push_back({lane,i,length});total+=length;}
        }
        if(total<=0)return out;
        count=std::min(count,fvolimits::kVehicles);
        for(int i=0;i<count;++i){double distance=total*(i+.5)/count;for(const auto& span:spans){if(distance>span.length){distance-=span.length;continue;}
            const double u=span.segment+distance/span.length;const auto a=trafficroute::At(line,span.lane,u),b=trafficroute::At(line,span.lane,u+trafficroute::Direction(line,span.lane)*.001);
            out.push_back({a,std::atan2(b.x-a.x,b.y-a.y),r.name,span.lane,u});break;}}
        return out;
    }
    count=std::min(count,fvolimits::kVehicles);const auto available=trafficroute::Lanes(line,r.lanes);const int lanes=int(available.size());
    for(int i=0;i<count;++i){const int rank=i%lanes,lane=available[rank],n=(count+lanes-1-rank)/lanes;
        // Measure each lane in source order; reverse the samples for opposite travel.
        double total=0;const auto&p=line.lane[lane];for(int k=0;k<int(p.size())-(line.closed?0:1);++k){const auto&a=p[k];const auto&b=p[(k+1)%p.size()];total+=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);}
        const double fraction=(double(i/lanes)+0.5)/n,u=AtMetres(line,lane,total*(trafficroute::Direction(line,lane)<0?1-fraction:fraction));
        const auto a=trafficroute::At(line,lane,u),b=trafficroute::At(line,lane,u+trafficroute::Direction(line,lane)*0.001);
        out.push_back({a,std::atan2(b.x-a.x,b.y-a.y),r.name,lane,u});
    }
    return out;
}
inline std::vector<Slot> Interleave(const std::vector<std::vector<Slot>>& routes,int limit=fvolimits::kVehicles){
    const auto cap=std::size_t(std::clamp(limit,0,fvolimits::kVehicles));
    std::size_t total=0;for(const auto& r:routes)total+=r.size();
    std::vector<std::vector<std::size_t>> selected(routes.size());
    for(std::size_t r=0;r<routes.size();++r){
        auto& ids=selected[r];ids.resize(routes[r].size());
        for(std::size_t i=0;i<ids.size();++i)ids[i]=i;
        if(total>cap)std::stable_sort(ids.begin(),ids.end(),[&](auto a,auto b){return routes[r][a].coverage<routes[r][b].coverage;});
    }
    if(total>cap){
        std::vector<std::size_t> lengths(routes.size());
        std::size_t chosen=0;
        for(std::size_t i=0;chosen<cap;++i)
            for(std::size_t r=0;r<routes.size()&&chosen<cap;++r)
                if(i<selected[r].size()){++lengths[r];++chosen;}
        for(std::size_t r=0;r<routes.size();++r){
            selected[r].resize(lengths[r]);
            std::sort(selected[r].begin(),selected[r].end());
        }
    }
    std::vector<Slot> out;for(std::size_t i=0;out.size()<cap;++i){bool any=false;
        for(std::size_t r=0;r<routes.size();++r)if(i<selected[r].size()){out.push_back(routes[r][selected[r][i]]);any=true;if(out.size()==cap)break;}
        if(!any)break;
    }return out;
}
}
