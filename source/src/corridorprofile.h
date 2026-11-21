#pragma once
#include "lanevolume.h"
#include <cstdint>
// Optional mod-owned protocol. Existing public API and policy layouts stay intact.
extern "C" {
struct FvoCorridorProfileV1 {
    std::uint32_t size=sizeof(FvoCorridorProfileV1),version=1;
    std::uint64_t identity=0;
    int active=0,count=0;
    double width=1,height=1;
    lanevolume::Vec3 right{1,0,0},up{0,0,1};
    lanevolume::Vec2 outline[64]{};
};
using FvoCorridorProviderV1=int (*)(void*,FvoCorridorProfileV1*);
using FvoRegisterCorridorV1=int (*)(FvoCorridorProviderV1);
}
namespace corridorprofile {
inline lanevolume::Profile Profile(const FvoCorridorProfileV1& p){lanevolume::Profile out;out.shape=lanevolume::Shape::Polygon;out.width=p.width;out.height=p.height;for(int i=0;i<std::clamp(p.count,0,64);++i)out.polygon.push_back(p.outline[i]);return out;}
inline bool Valid(const FvoCorridorProfileV1& p){if(p.size!=sizeof(p)||p.version!=1||p.active<0||p.active>1)return false;if(!p.active)return true;return p.count>=3&&p.count<=64&&p.width<=100&&p.height<=200&&lanevolume::Valid(Profile(p))&&routemap::Finite(p.right)&&routemap::Finite(p.up)&&std::fabs(lanevolume::Length(p.right)-1)<.001&&std::fabs(lanevolume::Length(p.up)-1)<.001&&std::fabs(routemap::Dot(p.right,p.up))<.001;}
// Project the existing measured model bounds; no engine queries or new physics claims.
template<class Input> inline bool Fits(const FvoCorridorProfileV1& p,const Input& in){if(!Valid(p))return false;if(!p.active)return true;const auto profile=Profile(p);std::vector<lanevolume::Vec2> corners;for(double x:{-in.shape.halfWidth-.1,in.shape.halfWidth+.1})for(double y:{-in.shape.halfLength-.1,in.shape.halfLength+.1})for(double z:{-in.shape.originAbove-.1,in.shape.height-in.shape.originAbove+.1}){const lanevolume::Vec3 v{x*std::cos(in.heading)+y*std::sin(in.heading),-x*std::sin(in.heading)+y*std::cos(in.heading),z};const lanevolume::Vec2 point{routemap::Dot(v,p.right),routemap::Dot(v,p.up)+p.height*.5};if(!lanevolume::Contains(profile,point.x,point.y))return false;corners.push_back({point.x/p.width,point.y/p.height});}for(std::size_t i=0;i<corners.size();++i)for(std::size_t j=i+1;j<corners.size();++j)for(std::size_t k=0;k<profile.polygon.size();++k){const auto a=corners[i],b=corners[j],c=profile.polygon[k],d=profile.polygon[(k+1)%profile.polygon.size()];if(lanevolume::Cross2(a,b,c)*lanevolume::Cross2(a,b,d)<-1e-12&&lanevolume::Cross2(c,d,a)*lanevolume::Cross2(c,d,b)<-1e-12)return false;}return true;}
}
