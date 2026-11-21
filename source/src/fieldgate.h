#pragma once
#include "lanecoord.h"
#include "roadlanes.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

// Mod-owned advice only. The established engine adapters remain the executor.
struct FvoFieldPoseV1 {
    std::uint32_t size=sizeof(FvoFieldPoseV1),version=1;
    std::uint64_t identity=0;
    int probe=0;
    double x=0,y=0,z=0,heading=0;
    double halfWidth=0,halfLength=0,height=0,originAbove=0;
};
using FvoFieldProviderV1=int (*)(void*,const FvoFieldPoseV1*);
// 0 unavailable, 1 no field, 2 permitted, 3 outside/invalid.
using FvoRegisterFieldV1=int (*)(FvoFieldProviderV1);
using FvoFieldRecoveryV1=int (*)(void*,const FvoFieldPoseV1*,FvoFieldPoseV1*);
// The traffic addon's own knowledge of its road (2026-11-04, the user: the traffic mod knows the ground and the type,
// so it tells directly instead of waiting for the game to measure). Answers for the lane of the pilot `user` at (x, y):
// 1 an authored road surface, its height in *floorZ (drive); 2 a lift / open-air tile (hover); 0 not this lane's road.
using FvoRoadSurfaceV1=int (*)(void*,double,double,double*);
using FvoRegisterRoadSurfaceV1=int (*)(FvoRoadSurfaceV1);
using FvoRegisterFieldRecoveryV1=int (*)(FvoFieldRecoveryV1);
namespace fieldgate {
// How far a vehicle's origin sits above its hull bottom. Model bounds put it 1.5-3 m up, but vehicles resting
// on Roadrunner settled with their origin 0.07 m over the road (2026-11-04 13:05 run: -178.41 on ground -178.48).
// The bounds figure spawned traffic 3 m high, read road-resting vehicles as under the floor and lifted them,
// and let the deck count as a wall. One small measured offset is used for spawn, field and lane driving.
constexpr double kOriginAbove=.3;
// Hull height from the top of the model above its origin, plus the fixed bottom offset.
inline double RestHeight(double topAboveOrigin){return std::max(1.0,std::clamp(topAboveOrigin,.5,8.0)+kOriginAbove);}
inline FvoFieldRecoveryV1 recoveryProvider=nullptr;
inline FvoRoadSurfaceV1 surfaceProvider=nullptr;
inline FvoRoadLanesProviderV1 roadLanesProvider=nullptr;  // optional (roadlanes.h): the whole road's lanes at a point
inline lanecoord::PermitProviderV1 lanePermitProvider=nullptr;  // optional (lanecoord.h): the coordinator's lane permissions; none: every lane the road lists is open
// The addon's road has priority: its floor is the ground from the first frame. The game's measurement is only
// compared afterwards (a disagreement is traced, never applied).
inline bool Disagrees(bool measured,double measuredZ,double floorZ){return measured&&std::isfinite(measuredZ)&&std::fabs(measuredZ-floorZ)>.5;}
inline double FloorClearance(bool autopilot,double field,double ordinary){
    return autopilot&&std::isfinite(field)&&field>=0?field:ordinary;
}
template<class Test> bool Sweep(const FvoFieldPoseV1& from,const FvoFieldPoseV1& to,Test&& test){
    const double distance=std::hypot(std::hypot(to.x-from.x,to.y-from.y),to.z-from.z);
    const double rotation=std::fabs(std::remainder(to.heading-from.heading,6.283185307179586))*std::hypot(from.halfWidth,from.halfLength);
    if(!std::isfinite(distance)||!std::isfinite(rotation)||distance+rotation>32)return false;
    const int count=std::max(1,int(std::ceil((distance+rotation)/.25)));
    if(count>128)return false;
    for(int i=0;i<=count;++i){
        const double t=double(i)/count;auto p=from;
        p.x+=(to.x-from.x)*t;p.y+=(to.y-from.y)*t;p.z+=(to.z-from.z)*t;
        p.heading+=std::remainder(to.heading-from.heading,6.283185307179586)*t;
        if(!test(p))return false;
    }
    return true;
}
// Nearest permitted origin height to `wanted` at this footprint (0.25 m steps, within 8 m).
// A hover height above the roof is lowered into the field, never answered with a stop.
template<class Test> bool FitHeight(FvoFieldPoseV1 p,double wanted,Test&& test,double& out){
    if(!std::isfinite(wanted))return false;
    for(int k=0;k<=32;++k)for(int s=1;s>=-1;s-=2){
        if(k==0&&s<0)continue;
        p.z=wanted+s*k*.25;
        if(test(p)){out=p.z;return true;}
    }
    return false;
}
// Authored road floors are approximate: Roadrunner's points sit up to ~2 m over the real deck in places
// (2026-11-04 13:14: authored floor 6.0, measured ground 4.04). A vehicle on or just over the real ground,
// under the authored floor, is judged as if lifted onto it (up to 3 m) instead of being lifted into the air.
// Returns that lift; 0 when the pose already fits, the ground is unknown, or the vehicle is not near it.
template<class Test> double GroundLift(FvoFieldPoseV1 p,bool groundOk,double ground,Test&& test){
    if(!groundOk||!std::isfinite(ground)||test(p))return 0;
    const double bottom=p.z-p.originAbove;
    if(bottom<ground-.4||bottom>ground+1.0)return 0;
    const double z=p.z;
    for(int k=1;k<=12;++k){p.z=z+k*.25;if(test(p))return k*.25;}
    return 0;
}
inline bool Valid(const FvoFieldPoseV1& p) {
    return p.size==sizeof(p)&&p.version==1&&std::isfinite(p.x)&&std::isfinite(p.y)&&
        std::isfinite(p.z)&&std::isfinite(p.heading)&&std::isfinite(p.halfWidth)&&
        std::isfinite(p.halfLength)&&std::isfinite(p.height)&&std::isfinite(p.originAbove)&&
        p.halfWidth>=0&&p.halfLength>=0&&p.height>=0&&p.originAbove>=0&&p.originAbove<=p.height;
}
template<class Input> FvoFieldPoseV1 Pose(const Input& in,std::uint64_t identity) {
    FvoFieldPoseV1 p;p.identity=identity;p.x=in.x;p.y=in.y;p.z=in.z;p.heading=in.heading;
    p.halfWidth=in.shape.halfWidth;p.halfLength=in.shape.halfLength;
    p.height=in.shape.height;p.originAbove=in.shape.originAbove;return p;
}
}
