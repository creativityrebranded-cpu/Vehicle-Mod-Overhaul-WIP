#pragma once
#include <cstdint>
#include <cmath>
// Optional mod-owned advice contract. Existing guidance/API layouts stay frozen.
extern "C" {
struct FvoTrafficPolicyV1 {
    std::uint32_t size=sizeof(FvoTrafficPolicyV1),version=1;
    std::uint64_t identity=0;
    int movement=0,obstacles=1,moving=0,people=0,recovery=1;
    int shape=0;
    double clearance=0,cornerRadius=0,topWidth=0;
};
using FvoTrafficPolicyProviderV1=int (*)(void*,FvoTrafficPolicyV1*);
using FvoRegisterTrafficPolicyV1=int (*)(FvoTrafficPolicyProviderV1);
}
namespace trafficcoordinator {
// Legacy authored goals use the Nexus general pilot. Explicit lane policies,
// travel rules and spatial envelopes retain the newer constrained controller.
inline bool NexusPilot(const FvoTrafficPolicyV1* policy,bool explicitTravel,bool corridor) {
    return (!policy||policy->movement==0)&&!explicitTravel&&!corridor;
}
inline bool Valid(const FvoTrafficPolicyV1& p) {
    return p.size==sizeof(p)&&p.version==1&&p.movement>=0&&p.movement<=5&&p.obstacles>=0&&p.obstacles<=2&&p.moving>=0&&p.moving<=2&&p.people>=0&&p.people<=2&&(p.recovery==0||p.recovery==1)&&p.shape>=0&&p.shape<=3&&std::isfinite(p.clearance)&&p.clearance>=0&&p.clearance<=200&&std::isfinite(p.cornerRadius)&&p.cornerRadius>=0&&p.cornerRadius<=100&&std::isfinite(p.topWidth)&&p.topWidth>=0&&p.topWidth<=100;
}
inline bool Strict(const FvoTrafficPolicyV1& p) {return p.movement!=0&&p.movement!=5;}
inline bool Offset(const FvoTrafficPolicyV1& p) {return p.movement!=1&&p.movement!=4&&p.obstacles==1;}
inline bool Bypass(const FvoTrafficPolicyV1& p) {return p.movement==0||(p.movement==2&&p.recovery);}
inline bool Fallback(const FvoTrafficPolicyV1& p) {return p.movement==0||p.movement==5;}
inline bool NeedsDecision(bool generalEnabled,bool commandAvailable,const FvoTrafficPolicyV1* p) {
    return generalEnabled||commandAvailable||(p&&Strict(*p));
}
enum class Owner { None, Lane, General, Hold };
struct Ownership {
    Owner owner=Owner::None;
    std::uint64_t identity=0;
    bool Change(Owner next,std::uint64_t command) {
        const bool changed=owner!=next||identity!=command;
        owner=next;identity=command;return changed;
    }
};
template<class Input> inline Input AdviceInput(Input in,const FvoTrafficPolicyV1& p) {
    if(p.moving==2)in.otherCount=0;
    return in;
}
template<class Cell> inline Cell AdviceCell(Cell cell,const FvoTrafficPolicyV1& p) {
    if(p.obstacles==2){cell.object=false;cell.solid=false;}
    return cell;
}
}
