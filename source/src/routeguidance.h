#pragma once
#include "fieldgate.h"
#include "heldwhy.h"
#include "lanesmooth.h"
#include <cstdint>
#include <cmath>
#include "trafficcoordinator.h"
#include "travelrules.h"
#include "corridorprofile.h"

// Optional capability, independent of API 13 and the existing claim/goal layouts.
// Older DLL pairs keep their original ABI and can omit this registration.
extern "C" {
struct FvoLanePointV1 { double x, y, z; };
struct FvoLaneGuidanceV1 {
    std::uint32_t size = sizeof(FvoLaneGuidanceV1), version = 1;
    std::uint64_t identity = 0;
    int count = 0, travel = 2, held = 0, priority = 0;
    double halfWidth = 3.5, speed = 25, stopDistance = -1, targetHeight = 0;
    FvoLanePointV1 points[64]{};
};
typedef int (*FvoLaneProviderV1)(void* user, FvoLaneGuidanceV1* out);
typedef int (*FvoRegisterLaneProviderV1)(FvoLaneProviderV1 provider);
// Optional script API exported by the addon. Requests copy all route names and
// stops; caller storage may be released after the call. Units: metres/seconds.
struct FvoJourneyStopV1 {
    double x=0,y=0,z=0,dwellSeconds=0;
    int routeCount=0;
    const char* const* routes=nullptr;
};
struct FvoJourneyRequestV1 {
    std::uint32_t size=sizeof(FvoJourneyRequestV1),version=1,vehicle=0;
    double x=0,y=0,z=0,joinReach=400;
    int stopCount=0;
    const FvoJourneyStopV1* stops=nullptr;
};
struct FvoJourneyStatusV1 {
    std::uint32_t size=sizeof(FvoJourneyStatusV1),version=1;
    int stopIndex=0,stopCount=0,dwelling=0,blocked=0;
    double laneError=0;
};
}
namespace routeguidance {
// PD-1c: the route hold reason the addon sends in `held` (heldwhy.h).
inline constexpr int kHeldReasons = heldwhy::kHeldReasons;
inline const char* HeldWhy(int held) { return heldwhy::Name(held); }
inline FvoCorridorProviderV1 corridorProvider=nullptr;
inline FvoFieldProviderV1 fieldProvider=nullptr;
inline FvoLaneProviderV1 provider = nullptr;
inline FvoTrafficPolicyProviderV1 policyProvider=nullptr;
inline FvoTravelRulesProviderV1 rulesProvider=nullptr;
inline int (*beforeSave)() = nullptr;
struct Frame { FvoLaneGuidanceV1 lane; std::uint64_t at = 0; FvoTrafficPolicyV1 policy;void* owner=nullptr;bool coordinated=false;const char* refusal=nullptr;travelrules::Rules rules; FvoCorridorProfileV1 corridor;
               lanesmooth::Result repaired{}; };  // LF-1: what the line repair changed in this command (trace)
// Main-thread behavior state, not a new physics hook or public NavParams field.
inline Frame frames[257];
inline void Clear(int pilot) { if (pilot >= 0 && pilot < 257) frames[pilot] = {}; }
inline bool Capture(int pilot, void* user, std::uint64_t now) {
    if (pilot < 0 || pilot >= 257) return false;
    const auto previous=frames[pilot];Clear(pilot);
    // A coordinated owner that loses its command holds; it must not silently
    // acquire the general driver's escape behaviour on a failed capture.
    if(previous.owner==user&&previous.coordinated&&policyProvider){frames[pilot]=previous;frames[pilot].at=0;frames[pilot].refusal="missing_or_invalid_guidance";}
    FvoLaneGuidanceV1 out;
    if (!provider || !provider(user, &out)) return false;
    // Registration alone does not claim unrelated integration users. A new
    // coordinated lease starts only when the lane provider recognizes this user.
    if(policyProvider&&!frames[pilot].coordinated){frames[pilot].owner=user;frames[pilot].coordinated=true;frames[pilot].policy.movement=1;frames[pilot].refusal="missing_or_invalid_guidance";}
    if (out.size != sizeof(out) || out.version != 1 || out.count < 2 || out.count > 64 || out.travel < 0 || out.travel > 2) return false;
    if(!std::isfinite(out.halfWidth)||out.halfWidth<0.5||out.halfWidth>100||!std::isfinite(out.speed)||out.speed<0||out.speed>200||!std::isfinite(out.stopDistance)||!std::isfinite(out.targetHeight))return false;
    for(int i=0;i<out.count;++i)if(!std::isfinite(out.points[i].x)||!std::isfinite(out.points[i].y)||!std::isfinite(out.points[i].z))return false;
    FvoTrafficPolicyV1 policy;const bool coordinated=policyProvider&&policyProvider(user,&policy)&&trafficcoordinator::Valid(policy)&&policy.identity==out.identity;
    if(policyProvider&&!coordinated) {
        if(!(previous.owner==user&&previous.coordinated)) {policy={};policy.movement=1;policy.identity=out.identity;frames[pilot]={out,0,policy,user,true};}
        frames[pilot].refusal="missing_or_invalid_policy";
        return false;
    }
    FvoTravelRulesV1 rules;
    if(rulesProvider&&(!rulesProvider(user,&rules)||rules.size!=sizeof(rules)||rules.version!=1||rules.identity!=out.identity||!travelrules::Valid(rules.rules))) {
        frames[pilot].policy.movement=1;frames[pilot].coordinated=true;frames[pilot].refusal="missing_or_invalid_travel_rules";return false;
    }
    if(rules.rules.mode!=travelrules::Legacy&&!trafficcoordinator::Strict(policy))policy.movement=2;
    // Alignment (2026-11-04): explicit Drive/Hover rules set the travel kind; SurfaceFirst keeps the kind the addon already
    // decided (road tile drive, lift hover) and only means "both" where the addon left it open.
    if(rules.rules.mode==travelrules::Drive)out.travel=1;else if(rules.rules.mode==travelrules::Hover)out.travel=2;
    else if(rules.rules.mode!=travelrules::Legacy&&out.travel!=1&&out.travel!=2)out.travel=0;
    FvoCorridorProfileV1 corridor;if(corridorProvider&&(!corridorProvider(user,&corridor)||!corridorprofile::Valid(corridor)||corridor.identity!=out.identity)){frames[pilot].policy.movement=1;frames[pilot].coordinated=true;frames[pilot].refusal="missing_or_invalid_corridor_profile";return false;}
    const auto repaired=lanesmooth::Repair(out.points,out.count,64);  // LF-1: no fold, no kink in the line a car follows
    frames[pilot] = {out, now,policy,user,coordinated,nullptr,rules.rules,corridor,repaired}; return true;
}
inline const FvoLaneGuidanceV1* Get(int pilot, std::uint64_t now) {
    return pilot >= 0 && pilot < 257 && frames[pilot].at && now >= frames[pilot].at && now - frames[pilot].at <= 800 ? &frames[pilot].lane : nullptr;
}
// A late lane command (no explicit refusal, last accepted <= `graceMs` ago): the last accepted lane, so a car mid-road is not
// held for one missed refresh (2026-11-04 review: one car hit "expired_lane_command" 35 times). Returns the age in `ageMs`.
inline const FvoLaneGuidanceV1* Grace(int pilot, std::uint64_t now, std::uint64_t graceMs, std::uint64_t* ageMs=nullptr) {
    if(pilot<0||pilot>=257||!frames[pilot].at||now<frames[pilot].at||frames[pilot].refusal)return nullptr;
    const auto age=now-frames[pilot].at;if(ageMs)*ageMs=age;
    return age<=graceMs?&frames[pilot].lane:nullptr;
}
inline const FvoTrafficPolicyV1* Policy(int pilot) {return pilot>=0&&pilot<257&&frames[pilot].coordinated?&frames[pilot].policy:nullptr;}
inline travelrules::Rules Rules(int pilot){return pilot>=0&&pilot<257?frames[pilot].rules:travelrules::Rules{};}
inline const FvoCorridorProfileV1* Corridor(int pilot){return pilot>=0&&pilot<257&&frames[pilot].corridor.active?&frames[pilot].corridor:nullptr;}
inline const char* Refusal(int pilot) {return pilot>=0&&pilot<257&&frames[pilot].refusal?frames[pilot].refusal:"expired_lane_command";}
}
