#pragma once
#include "trafficroute.h"
#include <array>
#include <numeric>
#include <string>
// Old lanes -> one road with red dividers (user choice, 2026-11-03). The route's
// current lanes, however they were made (pair, lane plan, freeform, transforms),
// are measured beside the road centre near both ends of every tile. A divider
// runs midway between neighbouring lanes as one straight side/swing line per
// tile, and the road is made just wide enough for the outer lanes. Each lane
// keeps its travel direction. The old lane records are left in the route,
// unused while road tiles are on. Crossing or stacked lanes are not guessed at.
namespace roadmigrate {
struct Result {bool changed=false;int lanes=0;std::string why;};
inline Result FromLanes(trafficdata::Route& r) {
    if(r.ring||r.points.size()<2)return {false,0,"rings and single points have no road tiles"};
    auto legacy=r;legacy.road={};
    const auto old=trafficroute::Build(legacy);
    if(old.error)return {false,0,std::string("old lanes do not compile: ")+old.error};
    const auto lanes=trafficroute::Lanes(old,r.lanes);
    if(lanes.empty())return {false,0,"the route has no lanes"};
    auto bare=r;bare.lanes=1;bare.road.enabled=true;bare.road.dividers.clear();bare.road.directions.clear();bare.road.width=0;bare.road.tiles.resize(r.points.size());
    const auto spine=trafficroute::Build(bare);
    if(spine.error)return {false,0,std::string("road centre does not compile: ")+spine.error};
    const std::size_t tiles=r.points.size()-(r.closed?0:1);
    constexpr double fa=.05,fb=.95;
    const std::size_t n=lanes.size();
    std::vector<std::vector<std::array<double,2>>> off(n,std::vector<std::array<double,2>>(tiles));
    std::vector<double> mean(n,0);double reach=0;
    for(std::size_t t=0;t<tiles;++t)for(int e=0;e<2;++e){
        const double source=double(t)+(e?fb:fa);
        const double us=spine.ToLine(source);
        const auto c=trafficroute::At(spine,0,us),a=trafficroute::At(spine,0,us-.01),b=trafficroute::At(spine,0,us+.01);
        const double len=std::hypot(b.x-a.x,b.y-a.y);if(len<1e-9)return {false,0,"road centre has no direction at a tile"};
        const double nx=-(b.y-a.y)/len,ny=(b.x-a.x)/len;  // left of travel, as the road builder's offsets
        const double uo=old.ToLine(source);
        for(std::size_t k=0;k<n;++k){
            const auto p=trafficroute::At(old,lanes[k],uo);
            if(std::fabs(p.z-c.z)>1.0)return {false,0,"old lanes run at different heights; a flat road cannot hold them"};
            const double width=old.plan.enabled?trafficroute::SettingsAt(old,uo,lanes[k]).profile.width:r.corridor;
            const double side=(p.x-c.x)*nx+(p.y-c.y)*ny;
            off[k][t][e]=side;mean[k]+=side;reach=std::max(reach,std::fabs(side)+std::max(.5,width)*.5);
        }
    }
    std::vector<std::size_t> order(n);std::iota(order.begin(),order.end(),0);
    std::sort(order.begin(),order.end(),[&](auto x,auto y){return mean[x]<mean[y];});  // right edge first
    for(std::size_t t=0;t<tiles;++t)for(int e=0;e<2;++e)for(std::size_t k=1;k<n;++k)
        if(off[order[k]][t][e]<=off[order[k-1]][t][e]+.05)return {false,0,"old lanes cross or overlap; draw the dividers by hand"};
    const double half=std::max(reach,.5);
    auto road=r.road;road.enabled=true;road.width=2*half;road.tiles.resize(r.points.size());road.dividers.clear();road.directions.clear();
    auto fit=[&](std::size_t lo,std::size_t hi,std::size_t t){
        const double ma=(off[lo][t][0]+off[hi][t][0])*.5,mb=(off[lo][t][1]+off[hi][t][1])*.5;
        const double swing=(mb-ma)/(2*(fb-fa)),side=ma-swing*(2*fa-1);
        roadplan::Tile tile=road.tiles[t];tile.side=side/half;tile.swing=swing/half;
        const double sum=std::fabs(tile.side)+std::fabs(tile.swing);if(sum>1){tile.side/=sum;tile.swing/=sum;}
        return tile;
    };
    if(n==2)for(std::size_t t=0;t<tiles;++t){const auto tile=fit(order[0],order[1],t);road.tiles[t].side=tile.side;road.tiles[t].swing=tile.swing;road.tiles[t].tilt=0;}
    if(n>=3){
        road.dividers.assign(n-1,std::vector<roadplan::Tile>(r.points.size()));
        for(std::size_t d=0;d+1<n;++d)for(std::size_t t=0;t<tiles;++t)road.dividers[d][t]=fit(order[d],order[d+1],t);
    }
    std::vector<int> directions;bool custom=false;
    for(std::size_t k=0;k<n;++k){directions.push_back(trafficroute::Direction(old,lanes[order[k]])>0?1:-1);custom=custom||directions.back()!=roadplan::Direction(road,int(k),int(n));}
    if(custom)road.directions=directions;
    auto out=r;out.road=road;out.lanes=n>=2?2:1;
    if(n==1){out.road.tiles.assign(r.points.size(),{});for(std::size_t t=0;t<r.points.size()&&t<r.road.tiles.size();++t)out.road.tiles[t].connector=r.road.tiles[t].connector;}
    if(!out.road.tiles.empty()&&!r.road.tiles.empty())for(std::size_t t=0;t<out.road.tiles.size()&&t<r.road.tiles.size();++t)out.road.tiles[t].connector=r.road.tiles[t].connector;
    if(!roadplan::Valid(out.road))return {false,0,"converted dividers are out of range"};
    if(const auto* error=trafficroute::Build(out).error)return {false,0,std::string("converted road does not compile: ")+error};
    r=out;return {true,int(n),""};
}
// Old lane geometry worth converting: anything beyond the default centred pair of a plain route.
inline bool HasLaneGeometry(const trafficdata::Route& r) {
    return r.lanePlan.enabled||!r.lanePlan.transforms.records.empty()||r.spacePlan.enabled||(r.lanes>1&&std::fabs(r.width-r.corridor)>1e-6);
}
}
