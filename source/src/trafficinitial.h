#pragma once
#include "trafficroute.h"
#include "fvolimits.h"
#include <string>
#include <vector>

namespace trafficinitial {
struct Slot {trafficroute::Vec at;double heading=0;std::string route;int lane=0;double u=0;};
inline double AtMetres(const trafficroute::Line& l,int lane,double metres){
    const auto& p=l.lane[lane];const int segments=l.closed?int(p.size()):int(p.size())-1;
    for(int i=0;i<segments;++i){const auto&a=p[i];const auto&b=p[(i+1)%p.size()];const double d=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);
        if(metres<=d&&d>0.001)return i+metres/d;metres-=d;}
    return l.Span();
}
inline std::vector<Slot> Route(const trafficdata::Route& r,int count){
    std::vector<Slot> out;const auto line=trafficroute::Build(r);if(line.Count()<2||count<1)return out;
    count=std::min(count,fvolimits::kVehicles);const int lanes=std::clamp(r.lanes,1,2);
    for(int i=0;i<count;++i){const int lane=i%lanes,n=(count+lanes-1-lane)/lanes;
        // Measure each lane in source order; reverse the samples for opposite travel.
        double total=0;const auto&p=line.lane[lane];for(int k=0;k<int(p.size())-(line.closed?0:1);++k){const auto&a=p[k];const auto&b=p[(k+1)%p.size()];total+=std::hypot(b.x-a.x,b.y-a.y,b.z-a.z);}
        const double fraction=(double(i/lanes)+0.5)/n,u=AtMetres(line,lane,total*(lane?1-fraction:fraction));
        const auto a=trafficroute::At(line,lane,u),b=trafficroute::At(line,lane,u+trafficroute::Direction(lane)*0.001);
        out.push_back({a,std::atan2(b.x-a.x,b.y-a.y),r.name,lane,u});
    }
    return out;
}
inline std::vector<Slot> Interleave(const std::vector<std::vector<Slot>>& routes){
    std::vector<Slot> out;for(std::size_t i=0;out.size()<std::size_t(fvolimits::kVehicles);++i){bool any=false;
        for(const auto& r:routes)if(i<r.size()){out.push_back(r[i]);any=true;if(out.size()==std::size_t(fvolimits::kVehicles))break;}
        if(!any)break;
    }return out;
}
}
