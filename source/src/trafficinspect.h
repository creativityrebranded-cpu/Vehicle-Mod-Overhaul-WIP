#pragma once
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
extern "C" {
struct FvoTrafficInspectV1 {
    std::uint32_t size=sizeof(FvoTrafficInspectV1),version=1,vehicle=0;
    int lane=0,leg=0,holding=0,scriptStop=0,scriptCount=0;
    double x=0,y=0,z=0,speed=0;
    std::uint64_t at=0;
    char route[128]{},destination[128]{};
};
struct FvoTrafficInspectV2 {std::uint32_t size=sizeof(FvoTrafficInspectV2),version=2;FvoTrafficInspectV1 journey;int draining=0,olderPlan=0,turning=0;char waitReason[192]{},restriction[192]{};};
using FvoTrafficInspectQueryV2=int (*)(const char*,int,FvoTrafficInspectV2*);
struct FvoTrafficPathInspectV1 {std::uint32_t size=sizeof(FvoTrafficPathInspectV1),version=1,vehicle=0;int kind=0,count=0;std::uint64_t at=0;double width=7;struct Point{double x,y,z;} points[64]{};};
using FvoTrafficPathQueryV1=int (*)(int,FvoTrafficPathInspectV1*);
using FvoTrafficInspectQueryV1=int (*)(const char*,int,FvoTrafficInspectV1*);
}
namespace trafficinspect {
struct Control {
    std::uint64_t at=0;
    double askedSpeed=0,actualSpeed=0,laneError=0,height=0;
    int mode=0,owner=0;
    char reason[192]{};
};
inline std::mutex lock;
inline std::map<unsigned,Control> controls;
inline void Publish(unsigned vehicle,const Control& value){std::lock_guard guard(lock);if(controls.size()>512)controls.clear();controls[vehicle]=value;}
inline bool Read(unsigned vehicle,Control& value){std::lock_guard guard(lock);const auto found=controls.find(vehicle);if(found==controls.end())return false;value=found->second;return true;}
}
