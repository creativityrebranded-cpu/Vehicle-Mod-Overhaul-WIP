#pragma once
#include "trafficroute.h"
#include "fieldgate.h"
#include "roadlanes.h"
#include <algorithm>
#include <cstring>
namespace roadfield {
inline bool Triangle(trafficdata::Vec p,trafficdata::Vec a,trafficdata::Vec b,trafficdata::Vec c,double& z) {
    const double den=(b.y-c.y)*(a.x-c.x)+(c.x-b.x)*(a.y-c.y);
    if(std::fabs(den)<1e-10)return false;
    const double u=((b.y-c.y)*(p.x-c.x)+(c.x-b.x)*(p.y-c.y))/den;
    const double v=((c.y-a.y)*(p.x-c.x)+(a.x-c.x)*(p.y-c.y))/den;
    if(u< -1e-8||v< -1e-8||u+v>1.00000001)return false;
    z=u*a.z+v*b.z+(1-u-v)*c.z;return true;
}
inline bool Quad(trafficdata::Vec p,trafficdata::Vec a,trafficdata::Vec b,trafficdata::Vec c,trafficdata::Vec d,double& z) {
    return Triangle(p,a,b,c,z)||Triangle(p,a,c,d,z);
}
// Is a point at lateral offset s from the road centre and height h above the floor inside the cross-section?
// Tube is an arch (flat floor, half-ellipse roof); rounded box rounds its top corners; trapezoid narrows
// linearly to param x width at the roof. Each narrows monotonically with height, so a body's floor and
// roof outlines bound every point between them.
inline bool InShape(const roadplan::Primitive& k,double s,double h,double half,double height) {
    if(k.kind==roadplan::Default||k.kind==roadplan::Flat||k.kind==roadplan::Open||height<=1e-8||half<=1e-8)return true;
    const double a=std::fabs(s),y=std::clamp(h,0.0,height);
    switch(k.kind){
        case roadplan::Box:return true;
        case roadplan::Rounded:{const double r=std::min({k.param,half,height});if(a<=half-r||y<=height-r)return true;
            const double dx=a-(half-r),dy=y-(height-r);return dx*dx+dy*dy<=r*r+1e-9;}
        case roadplan::Tube:return (a/half)*(a/half)+(y/height)*(y/height)<=1+1e-9;
        case roadplan::Trapezoid:return a<=half*(1-(1-k.param)*y/height)+1e-9;
    }
    return false;  // unknown shape: never permitted
}
// Lateral offset and half width of the whole road at p between two stations.
inline void Across(const trafficroute::Line::RoadSection& a,const trafficroute::Line::RoadSection& b,trafficdata::Vec p,double& s,double& half,double* along=nullptr) {
    const double ax=(a.left.x+a.right.x)*.5,ay=(a.left.y+a.right.y)*.5,bx=(b.left.x+b.right.x)*.5,by=(b.left.y+b.right.y)*.5;
    const double dx=bx-ax,dy=by-ay,len=dx*dx+dy*dy;const double t=len>1e-12?std::clamp(((p.x-ax)*dx+(p.y-ay)*dy)/len,0.0,1.0):0;
    if(along)*along=t;
    const double rx=a.right.x+(b.right.x-a.right.x)*t,ry=a.right.y+(b.right.y-a.right.y)*t,lx=a.left.x+(b.left.x-a.left.x)*t,ly=a.left.y+(b.left.y-a.left.y)*t;
    const double wx=lx-rx,wy=ly-ry,w=std::hypot(wx,wy);half=w*.5;
    s=w>1e-9?((p.x-(rx+lx)*.5)*wx+(p.y-(ry+ly)*.5)*wy)/w:0;
}
// Every lane's edges at progress u on `own`, measured from own's centre line (roadlanes.h). The stations are the route's markers,
// 100-300 m apart, so the edges are interpolated between the two around u: taking the nearest marker's edges shifted the answer
// 4-30 m on anything but a straight (2026-11-06 12:04 run: own lane read 0.5..8.4, 15.7..26.3) and the core threw it away.
inline bool LanesAtFull(const trafficroute::Line& line,int own,double u,FvoRoadLanesV1& out) {
    const long long size=(long long)line.road.size();if(size<2||own<0)return false;
    const auto c=trafficroute::At(line,own,u),n=trafficroute::At(line,own,u+trafficroute::Direction(line,own)*0.5);
    double fx=n.x-c.x,fy=n.y-c.y;const double fl=std::hypot(fx,fy);if(fl<1e-6)return false;fx/=fl;fy/=fl;
    const double rx=fy,ry=-fx;  // right of the own lane's travel
    const double whole=std::floor(u);const double t=std::clamp(u-whole,0.0,1.0);
    long long i=(long long)whole,j=i+1;
    if(line.closed){i%=size;if(i<0)i+=size;j=(i+1)%size;}else{i=std::clamp<long long>(i,0,size-1);j=std::clamp<long long>(j,0,size-1);}
    const auto& a=line.road[std::size_t(i)].boundaries;const auto& b=line.road[std::size_t(j)].boundaries;
    if(a.size()<2||a.size()!=b.size())return false;
    const int count=int(std::min<std::size_t>(8,a.size()-1));const double gap=line.median*.5;
    auto lateral=[&](std::size_t k){const double x=a[k].x+(b[k].x-a[k].x)*t,y=a[k].y+(b[k].y-a[k].y)*t;return (x-c.x)*rx+(y-c.y)*ry;};
    out=FvoRoadLanesV1{};out.count=count;out.own=own;
    for(int k=0;k<count;++k){
        double l=lateral(std::size_t(k)),e=lateral(std::size_t(k+1));if(l>e)std::swap(l,e);
        out.left[k]=l;out.right[k]=e;
        out.sameWay[k]=trafficroute::Direction(line,k)*trafficroute::Direction(line,own)>0?1:0;}
    // A median between lanes is no lane: each lane gives up half of it on any side that borders another lane.
    if(line.edgeLeft>0||line.edgeRight>0){  // edge no-go bands: the outermost lane gives up the band on its outer side
        const bool rightIsLow=lateral(0)<lateral(std::size_t(count));
        const double lowBand=rightIsLow?line.edgeRight:line.edgeLeft,highBand=rightIsLow?line.edgeLeft:line.edgeRight;
        double lowest=1e300,highest=-1e300;for(int k=0;k<count;++k){lowest=std::min(lowest,out.left[k]);highest=std::max(highest,out.right[k]);}
        for(int k=0;k<count;++k){if(out.left[k]<lowest+1e-6)out.left[k]=std::min(out.right[k],out.left[k]+lowBand);if(out.right[k]>highest-1e-6)out.right[k]=std::max(out.left[k],out.right[k]-highBand);}
        out.bandLow=lowBand;out.bandHigh=highBand;}  // the planner refuses a body past the lane edge on a side that has a band (energyplan.h Score, corridor.h)
    if(gap>0){double lowest=1e300,highest=-1e300;for(int k=0;k<count;++k){lowest=std::min(lowest,out.left[k]);highest=std::max(highest,out.right[k]);}
        for(int k=0;k<count;++k){if(out.left[k]>lowest+1e-6)out.left[k]+=gap;if(out.right[k]<highest-1e-6)out.right[k]-=gap;out.right[k]=std::max(out.left[k],out.right[k]);}}
    return true;
}
// Older cores hand a smaller FvoRoadLanesV1 (no bandLow/bandHigh): only the bytes the caller's own `size` covers are written, and `size` is kept.
inline bool LanesAt(const trafficroute::Line& line,int own,double u,FvoRoadLanesV1& out) {
    const std::uint32_t callerSize=std::clamp<std::uint32_t>(out.size,kFvoRoadLanesV1BaseSize,std::uint32_t(sizeof(FvoRoadLanesV1)));
    FvoRoadLanesV1 full;if(!LanesAtFull(line,own,u,full))return false;
    std::memcpy(&out,&full,callerSize);out.size=callerSize;return true;
}
inline std::size_t Sections(const trafficroute::Line& line) {return line.road.size()<2?0:line.road.size()-(line.closed?0:1);}
// Sections whose plan-view box comes within `radius` of (x,y); no other section can hold a footprint point.
inline std::vector<std::size_t> Near(const trafficroute::Line& line,double x,double y,double radius) {
    std::vector<std::size_t> out;
    for(std::size_t i=0;i<Sections(line);++i) {
        const auto& a=line.road[i];const auto& b=line.road[(i+1)%line.road.size()];
        double lx=1e300,ly=1e300,hx=-1e300,hy=-1e300;
        auto grow=[&](const std::vector<trafficdata::Vec>& vs){for(const auto& v:vs){lx=std::min(lx,v.x);ly=std::min(ly,v.y);hx=std::max(hx,v.x);hy=std::max(hy,v.y);}};
        grow(a.boundaries);grow(b.boundaries);grow(a.tops);grow(b.tops);
        if(x+radius>=lx&&x-radius<=hx&&y+radius>=ly&&y-radius<=hy)out.push_back(i);
    }
    return out;
}
inline bool Point(const trafficroute::Line& line,int lane,trafficdata::Vec p,bool support,const std::vector<std::size_t>& near,bool relativeToFloor=false) {
    if(line.error||line.road.size()<2||lane<0)return false;
    for(const auto i:near) {
        const auto& a=line.road[i];const auto& b=line.road[(i+1)%line.road.size()];
        if(std::size_t(lane+1)>=a.boundaries.size()||a.boundaries.size()!=b.boundaries.size())return false;
        double floor=0;
        const auto al=a.boundaries[lane],ar=a.boundaries[lane+1],bl=b.boundaries[lane],br=b.boundaries[lane+1];
        if(relativeToFloor&&a.primitive.kind==roadplan::Open)continue;
        if(!Quad(p,al,bl,br,ar,floor))continue;
        const double z=relativeToFloor?floor+p.z:p.z;
        const double height=a.primitive.kind==roadplan::Flat?0:a.tops[lane].z-al.z;
        if(line.median>0){  // the median band beside an interior divider belongs to no lane
            auto dist=[&](trafficdata::Vec u,trafficdata::Vec v){const double dx=v.x-u.x,dy=v.y-u.y,l2=dx*dx+dy*dy;const double t=l2>1e-12?std::clamp(((p.x-u.x)*dx+(p.y-u.y)*dy)/l2,0.0,1.0):0;return std::hypot(u.x+dx*t-p.x,u.y+dy*t-p.y);};
            const double gap=line.median*.5;if((lane>0&&dist(al,bl)<gap)||(std::size_t(lane+2)<a.boundaries.size()&&dist(ar,br)<gap))continue;}
        if(line.edgeLeft>0||line.edgeRight>0){  // an edge band beside the outermost boundary belongs to no lane
            auto dist=[&](trafficdata::Vec u,trafficdata::Vec v){const double dx=v.x-u.x,dy=v.y-u.y,l2=dx*dx+dy*dy;const double t=l2>1e-12?std::clamp(((p.x-u.x)*dx+(p.y-u.y)*dy)/l2,0.0,1.0):0;return std::hypot(u.x+dx*t-p.x,u.y+dy*t-p.y);};
            if((lane==0&&line.edgeRight>0&&dist(al,bl)<line.edgeRight)||(std::size_t(lane+2)==a.boundaries.size()&&line.edgeLeft>0&&dist(ar,br)<line.edgeLeft))continue;}
        if(height<=1e-8){if(!support||std::fabs(z-floor)<=.35)return true;continue;}
        double s=0,half=0,along=0;Across(a,b,p,s,half,&along);
        if(!InShape(a.primitive,s,z-floor,half,height)||(along<=a.blendEnd&&!InShape(a.blend,s,z-floor,half,height)))continue;
        // Wheels follow the surface within 0.35 m, as on flat roads: a slope under a footprint is not "below the road".
        if(z<floor-.35||z>floor+height+.35)continue;
        const double f=std::clamp((z-floor)/height,0.0,1.0);
        auto mix=[f](auto x,auto y){return trafficdata::Vec{x.x+(y.x-x.x)*f,x.y+(y.y-x.y)*f,x.z+(y.z-x.z)*f};};
        double unused;
        if(Quad(p,mix(al,a.tops[lane]),mix(bl,b.tops[lane]),mix(br,b.tops[lane+1]),mix(ar,a.tops[lane+1]),unused))return true;
    }
    return false;
}
inline bool Point(const trafficroute::Line& line,int lane,trafficdata::Vec p,bool support=false) {
    std::vector<std::size_t> all(Sections(line));for(std::size_t i=0;i<all.size();++i)all[i]=i;
    return Point(line,lane,p,support,all);
}
// Authored floor of `lane` under (x,y); false when (x,y) is outside that lane's cells.
inline bool Floor(const trafficroute::Line& line,int lane,double x,double y,double& z) {
    if(line.error||lane<0)return false;
    for(const auto i:Near(line,x,y,0)) {
        const auto& a=line.road[i];const auto& b=line.road[(i+1)%line.road.size()];
        if(std::size_t(lane+1)>=a.boundaries.size()||a.boundaries.size()!=b.boundaries.size())return false;
        if(Quad({x,y,0},a.boundaries[lane],b.boundaries[lane],b.boundaries[lane+1],a.boundaries[lane+1],z))return true;
    }
    return false;
}
// Test the footprint outline (wheels on the floor) and roof outline over the union
// of the lane's connected cells, at <=0.25 m spacing plus the centre. A lane strip
// has no holes, so an outline inside it holds the whole footprint.
inline bool Fits(const trafficroute::Line& line,int lane,const FvoFieldPoseV1& pose,bool surfaceAligned=false) {
    if(!fieldgate::Valid(pose)||line.error||line.road.empty())return false;
    const int nx=std::max(1,int(std::ceil(pose.halfWidth*2/.25)));
    const int ny=std::max(1,int(std::ceil(pose.halfLength*2/.25)));
    if(nx>256||ny>256)return false;
    const auto near=Near(line,pose.x,pose.y,std::hypot(pose.halfWidth,pose.halfLength)+.5);
    if(near.empty())return false;
    const double cs=std::cos(pose.heading),sn=std::sin(pose.heading);
    auto test=[&](double dx,double dy){
        const double px=pose.x+dx*cs+dy*sn,py=pose.y-dx*sn+dy*cs;
        // Births are evaluated seated on the road. Runtime poses retain their measured height.
        const double bottom=surfaceAligned?0:pose.z-pose.originAbove;
        return Point(line,lane,{px,py,bottom},true,near,surfaceAligned)&&
            Point(line,lane,{px,py,bottom+pose.height},false,near,surfaceAligned);
    };
    if(!test(0,0))return false;
    for(int x=0;x<=nx;++x){const double dx=-pose.halfWidth+2*pose.halfWidth*x/nx;if(!test(dx,-pose.halfLength)||!test(dx,pose.halfLength))return false;}
    for(int y=1;y<ny;++y){const double dy=-pose.halfLength+2*pose.halfLength*y/ny;if(!test(-pose.halfWidth,dy)||!test(pose.halfWidth,dy))return false;}
    return true;
}
}
