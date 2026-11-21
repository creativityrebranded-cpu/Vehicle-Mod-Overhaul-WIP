#pragma once
#include "trafficdata.h"
#include <algorithm>
#include <cmath>
#include <vector>

// Circular fillets in XY with continuous interpolated height. Authored control
// points keep their identities; the expanded path is shared by preview/flight.
namespace routecurve {
struct Result {std::vector<trafficdata::Vec> points;std::vector<double> authored;};
// Same geometric limit used by Build. Exposed to authoring so a requested bend
// never masquerades as an achievable radius on short adjoining sections.
inline double CornerLimit(const std::vector<trafficdata::Vec>& pts,std::size_t i,bool closed){
    if(pts.size()<3||i>=pts.size()||(!closed&&(i==0||i+1==pts.size())))return 0;
    const auto a=pts[(i+pts.size()-1)%pts.size()],b=pts[i],c=pts[(i+1)%pts.size()];
    const double first=std::hypot(a.x-b.x,a.y-b.y),second=std::hypot(c.x-b.x,c.y-b.y);if(first<.1||second<.1)return 0;
    const double angle=std::acos(std::clamp(((a.x-b.x)*(c.x-b.x)+(a.y-b.y)*(c.y-b.y))/(first*second),-1.0,1.0));
    constexpr double pi=3.141592653589793;if(angle<pi/36||angle>pi*35/36)return 0;return .45*std::min(first,second)*std::tan(angle/2);
}
inline Result Build(const std::vector<trafficdata::Vec>& pts,const std::vector<double>& radii,bool closed){
    Result out;const auto count=pts.size();if(count<2){out.points=pts;return out;}
    constexpr double pi=3.141592653589793;
    for(std::size_t i=0;i<count;++i){
        const auto b=pts[i];const double wanted=i<radii.size()?radii[i]:0;
        if(!(wanted>0)||!std::isfinite(wanted)||(!closed&&(i==0||i+1==count))){out.authored.push_back(double(out.points.size()));out.points.push_back(b);continue;}
        const auto a=pts[(i+count-1)%count],c=pts[(i+1)%count];
        const double l1=std::hypot(a.x-b.x,a.y-b.y),l2=std::hypot(c.x-b.x,c.y-b.y);
        if(l1<0.1||l2<0.1){out.authored.push_back(double(out.points.size()));out.points.push_back(b);continue;}
        const double ux=(a.x-b.x)/l1,uy=(a.y-b.y)/l1,vx=(c.x-b.x)/l2,vy=(c.y-b.y)/l2;
        const double angle=std::acos(std::clamp(ux*vx+uy*vy,-1.0,1.0));
        if(angle<pi/36||angle>pi*35/36){out.authored.push_back(double(out.points.size()));out.points.push_back(b);continue;}
        const double trim=std::min(wanted/std::tan(angle/2),0.45*std::min(l1,l2));
        const double radius=trim*std::tan(angle/2),bis=std::hypot(ux+vx,uy+vy);
        const double cx=b.x+(ux+vx)/bis*radius/std::sin(angle/2),cy=b.y+(uy+vy)/bis*radius/std::sin(angle/2);
        const double start=std::atan2(b.y+uy*trim-cy,b.x+ux*trim-cx);
        const double finish=std::atan2(b.y+vy*trim-cy,b.x+vx*trim-cx),sweep=std::remainder(finish-start,2*pi);
        int steps=std::max(2,int(std::ceil(std::fabs(sweep)/(pi/18))));if(steps%2)++steps;
        out.authored.push_back(double(out.points.size()+std::size_t(steps/2)));
        const double za=b.z+(a.z-b.z)*trim/l1,zc=b.z+(c.z-b.z)*trim/l2;
        for(int k=0;k<=steps;++k){const double f=double(k)/steps,t=start+sweep*f;
            out.points.push_back({cx+radius*std::cos(t),cy+radius*std::sin(t),za+(zc-za)*f});}
    }
    if(closed&&!out.authored.empty()){
        const auto offset=std::size_t(out.authored[0]);std::rotate(out.points.begin(),out.points.begin()+offset,out.points.end());
        for(auto& u:out.authored)u=std::fmod(u+double(out.points.size())-double(offset),double(out.points.size()));
    }
    return out;
}
}
