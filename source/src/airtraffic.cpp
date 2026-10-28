#include "airtraffic.h"
#include "airtrafficdata.h"
#include "addresses.h"
#include "bodybinding.h"
#include "cityprofile.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "prefetch.h"
#include "runtimeforms.h"
#include "vehiclediag.h"
#include "vehicleflight.h"
#include "vehiclehits.h"
#include "vehicleworld.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace airtraffic {
namespace {
using uptr=std::uintptr_t;
using namespace airtrafficmath;
template<class T>T At(uptr p,unsigned offset){return *reinterpret_cast<const T*>(p+offset);}
struct Slot {
    std::uint32_t vehicle=0,pilot=0;bool seeded=false,assembled=false,taken=false,permit=true;
    uptr scene=0;
    StreamGate stream;
    std::int64_t cycle=INT64_MIN;
    unsigned ray=0;bool pending=false,blocked=false;
    ULONGLONG asked=0;
    Vec from,to;
    bool abort=false;double abortAt=0;Vec abortFrom;
    Phase phase=Phase::Circulating;
};
std::array<Slot,kFleet> slots;
bool resolved=false,cityActive=false,missingLogged=false;
unsigned nextSlot=0,assemblyOk=0,assemblyFailed=0,blockedVisits=0;
ULONGLONG resolveAt=0,configAt=0,reportAt=0;
std::uint32_t daysId=0,scaleId=0;
Clock clock;
ringlogic::Ring ring=airtrafficdata::kRing;
double priorSeconds=0;

bool PlayerWorld(uptr& cell,uptr& ws,char (&world)[201],Vec& pos) {
    __try {
        const auto player=*reinterpret_cast<const uptr*>(game::Base()+addr::kPlayerCharacterPtr);
        if(!player)return false;cell=At<uptr>(player,off::kRefParentCell);ws=cell?At<uptr>(cell,off::kCellWorldspace):0;
        if(!ws)return false;
        auto** table=*reinterpret_cast<void***>(ws);const auto* name=reinterpret_cast<const char*(*)(uptr)>(table[0x4D])(ws);
        unsigned i=0;for(;name&&name[i]&&i<200;++i)world[i]=name[i];world[i]=0;
        const auto p=At<game::Vec3>(player,off::kRefPosition);pos={p.x,p.y,p.z};return Valid(pos);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool ReadPosition(uptr ref,Vec& pos,uptr& root,bool requireLoaded=true) {
    __try {
        if(!ref||(At<std::uint32_t>(ref,0x20)&0x820))return false;
        const auto data=At<uptr>(ref,off::kRefLoadedData);root=data?At<uptr>(data,off::kLoadedData3D):0;
        const auto p=At<game::Vec3>(ref,off::kRefPosition);pos={p.x,p.y,p.z};return (!requireLoaded||root)&&Valid(pos);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Global(std::uint32_t id,double& value) {
    __try {const auto f=runtimeforms::LookupChecked(id);if(!f)return false;value=At<float>(f,0x48);return std::isfinite(value);}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Move(uptr ref,uptr cell,uptr ws,Vec pos,double yaw) {
    __try {
        float p[3]{float(pos.x),float(pos.y),float(pos.z)},r[3]{0,0,float(yaw)};
        std::uint64_t handle=*reinterpret_cast<const std::uint32_t*>(game::Base()+addr::kInvalidHandle);
        reinterpret_cast<void(*)(uptr,std::uint64_t*,uptr,uptr,float*,float*)>(game::Base()+addr::kMoveReference)(ref,&handle,cell,ws,p,r);return true;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void Hide(uptr ref) {
    __try {if(ref&&!(At<std::uint32_t>(ref,0x20)&0x800))reinterpret_cast<void(*)(uptr,char)>(game::Base()+addr::kDisableReference)(ref,0);}
    __except(EXCEPTION_EXECUTE_HANDLER){}
}
bool Component(uptr component,std::uint32_t form,uptr& instance,std::uint32_t& body) {
    __try {
        if(!component||At<std::uint32_t>(component,0x0C)!=form)return false;
        instance=At<uptr>(component,0x10);body=instance?At<std::uint32_t>(instance,0x20):0;return instance&&bodybinding::Valid(body);
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void ClearControls(){epoch.fetch_add(1);for(auto& control:controls)control.Publish({});}
bool Resolve(ULONGLONG now) {
    if(resolved)return true;if(now<resolveAt)return false;resolveAt=now+5000;
    const auto file=runtimeforms::LoadedPlugin(kPlugin);
    if(!file){if(!missingLogged){logger::Write("Air traffic: optional %s absent; no airborne traffic",kPlugin);missingLogged=true;}return false;}
    for(unsigned i=0;i<kFleet;++i){slots[i].vehicle=runtimeforms::RuntimeId(file,0x800+i);slots[i].pilot=runtimeforms::RuntimeId(file,0x820+i);}
    daysId=runtimeforms::Resolve({"Starfield.esm",airtrafficdata::kDays});scaleId=runtimeforms::Resolve({"Starfield.esm",airtrafficdata::kTimeScale});
    resolved=daysId&&scaleId;logger::Write("Air traffic: %u vehicles, %u circulation / %u staggered visit opportunities per rotation; no boarding retries",kFleet,kHalf,kHalf);return resolved;
}
void LoadRing(const char* world,ULONGLONG now) {
    if(now<configAt)return;configAt=now+5000;
    const std::wstring section(world,world+std::strlen(world));wchar_t value[256]{};
    GetPrivateProfileStringW(section.c_str(),L"ring",L"",value,256,fvopaths::File(L"ringroad.ini").c_str());
    std::string text;for(const auto* p=value;*p;++p)text.push_back(char(*p));
    ringlogic::Ring candidate;
    if(ringlogic::Parse(text,candidate)&&
       (candidate.cx!=ring.cx||candidate.cy!=ring.cy||candidate.z!=ring.z||candidate.radius!=ring.radius||
        candidate.aspect!=ring.aspect||candidate.rotation!=ring.rotation||candidate.gap!=ring.gap)){
        ring=candidate;ClearControls();
        for(auto& s:slots){s.seeded=false;s.cycle=INT64_MIN;s.abort=false;}
        logger::Write("Air traffic: edited ring accepted; route initialization renewed once");
    }
}
bool PadFree(unsigned slot,const world::Snapshot& snapshot) {
    const auto& pad=airtrafficdata::kPads[slot];
    for(unsigned i=0;i<snapshot.count;++i){const auto& v=snapshot.v[i];
        if(v.formId==slots[slot].vehicle)continue;
        if(std::fabs(v.pos[2]-pad.p.z)<4&&std::hypot(v.pos[0]-pad.p.x,v.pos[1]-pad.p.y)<std::max(3.0,double(std::hypot(v.half[0],v.half[1]))+1))return false;
    }
    return true;
}
bool Same(const float (&a)[3],Vec b){return std::fabs(a[0]-b.x)<.1&&std::fabs(a[1]-b.y)<.1&&std::fabs(a[2]-b.z)<.1;}
void Clearance(unsigned i,Slot& s,double t,ULONGLONG now,bool& submitted) {
    if(s.ray>=3||s.blocked)return;
    if(s.pending){
        prefetch::RayHit hit{};unsigned age=0;
        if(prefetch::LatestRay(prefetch::kRayAirTraffic0+int(i),hit,age)&&age<120&&Same(hit.from,s.from)&&Same(hit.to,s.to)){
            const bool self=hit.formId==s.vehicle||hit.formId==s.pilot;
            if(hit.hit&&!self&&hit.distance+3<Length(s.to-s.from)){s.blocked=true;++blockedVisits;}
            s.pending=false;++s.ray;
        }else if(now-s.asked>2000){s.blocked=true;s.pending=false;++blockedVisits;}
        return;
    }
    if(submitted)return;
    const auto& pad=airtrafficdata::kPads[i];const double lap=Lap(ring),begin=(double(s.cycle)+Stagger(i))*lap;
    const auto entry=Cruise(ring,i,begin),exit=Cruise(ring,i,begin+.62*lap);
    const Vec above{pad.p.x,pad.p.y,std::max({entry.p.z,exit.p.z,pad.p.z+30})};
    if(s.ray==0){s.from=above;s.to={pad.p.x,pad.p.y,pad.p.z+2};}
    else if(s.ray==1){s.from=entry.p;s.to=above;}
    else{s.from=above;s.to=exit.p;}
    const float from[3]{float(s.from.x),float(s.from.y),float(s.from.z)},to[3]{float(s.to.x),float(s.to.y),float(s.to.z)};
    // Static/terrain/walls, not crowd bodies or triggers. One asynchronous
    // request globally per admitted update, only three per vehicle/rotation.
    prefetch::RequestRay(prefetch::kRayAirTraffic0+int(i),from,to,0xA5C12671Eull&~((1ull<<8)|(1ull<<30)|(1ull<<33)));
    s.pending=true;s.asked=now;submitted=true;(void)t;
}
}
bool IsVehicle(std::uint32_t id){if(!id)return false;for(const auto&s:slots)if(s.vehicle==id)return true;return false;}
bool OwnPair(uptr actor,uptr vehicle){const auto a=runtimeforms::Id(actor),v=runtimeforms::Id(vehicle);if(!a||!v)return false;for(const auto&s:slots)if(s.vehicle==v&&s.pilot==a)return true;return false;}
void Tick() {
    const auto now=GetTickCount64();if(!Resolve(now))return;
    uptr cell=0,ws=0;char name[201]{};Vec player;
    const bool inCity=PlayerWorld(cell,ws,name,player)&&
        (cityprofile::MatchesPlacement(cityprofile::kNewAtlantis,name)||std::strcmp(name,"NewAtlantis")==0)&&
        std::hypot(player.x-ring.cx,player.y-ring.cy)<4000;
    if(!inCity){if(cityActive){ClearControls();for(auto&s:slots){s.seeded=false;s.scene=0;}}cityActive=false;return;}
    cityActive=true;LoadRing(name,now);
    double days=0,scale=0;if(!Global(daysId,days)||!Global(scaleId,scale)||scale<=0){ClearControls();return;}
    const auto t=clock.Sample(days,scale);
    if(priorSeconds&&std::fabs(t-priorSeconds)>5){ClearControls();for(auto&s:slots)s.seeded=false;}
    priorSeconds=t;const auto cycle=std::int64_t(std::floor(t/Lap(ring)));
    world::Snapshot snapshot{};const bool haveWorld=world::Vehicles(snapshot);
    bool submitted=false,assembled=false;unsigned processed=0;
    LARGE_INTEGER began{},stamp{},frequency{};QueryPerformanceCounter(&began);QueryPerformanceFrequency(&frequency);
    while(processed<4){
        if(processed){QueryPerformanceCounter(&stamp);if(double(stamp.QuadPart-began.QuadPart)*1000/double(frequency.QuadPart)>.75)break;}
        const unsigned i=nextSlot++%kFleet;++processed;auto& s=slots[i];
        const auto ref=runtimeforms::LookupChecked(s.vehicle);Vec position;uptr root=0;
        if(!ReadPosition(ref,position,root,false)){controls[i].Publish({});s.seeded=false;s.scene=0;continue;}
        if(s.scene!=root){s.scene=root;s.seeded=false;}
        if(vehicleflight::PlayerDriving(s.vehicle)||vehicleflight::OwnRoverId()==s.vehicle||vehiclehits::Wrecked(s.vehicle)){controls[i].Publish({});s.taken=true;continue;}
        const auto driver=npcvehicles::VehicleDriverId(s.vehicle);
        if(driver&&driver!=s.pilot){controls[i].Publish({});s.taken=true;continue;}if(s.taken){controls[i].Publish({});continue;}
        const auto cruise=Cruise(ring,i,t);
        const bool request=s.stream.Request(Length(cruise.p-player),root!=0);
        if(!root){
            controls[i].Publish({});s.seeded=false;s.scene=0;
            if(request){
                // Advance a virtual vehicle to its current moving position
                // when that position enters the loaded neighborhood. Do not
                // wait for a model at the obsolete authored spawn position.
                const bool moved=Move(ref,cell,ws,cruise.p,cruise.yaw);
                const auto actor=runtimeforms::LookupChecked(s.pilot);
                if(moved&&!s.assembled&&actor)Move(actor,cell,ws,cruise.p,cruise.yaw);
                logger::Write("Air traffic: slot %u streaming admission at (%.1f, %.1f, %.1f): %s",i,cruise.p.x,cruise.p.y,cruise.p.z,moved?"requested":"native move failed");
            }
            continue;
        }
        if(s.cycle!=cycle){
            s.cycle=cycle;
            // Joining a running schedule always starts in the air. An
            // approach already under way waits until the next assigned lap.
            s.permit=haveWorld&&PadFree(i,snapshot)&&t<(double(cycle)+Stagger(i))*Lap(ring);
            s.ray=0;s.pending=false;s.blocked=false;
        }
        if(Visiting(i,cycle)&&s.permit)Clearance(i,s,t,now,submitted);
        if(s.blocked)s.permit=false;
        if(s.permit&&Visiting(i,cycle)&&s.ray<3&&t>=(double(cycle)+Stagger(i))*Lap(ring))s.permit=false;
        auto pose=airtrafficmath::At(ring,airtrafficdata::kPads[i],i,t,s.permit);
        if((s.blocked||(pose.visiting&&haveWorld&&!PadFree(i,snapshot)))&&pose.visiting&&!s.abort){
            s.abort=true;s.abortAt=t;s.abortFrom=position;s.permit=false;
            logger::Write("Air traffic: slot %u placement unavailable; returning to circulation without teleport",i);
        }
        if(s.abort){
            const double elapsed=t-s.abortAt,duration=30;const auto target=Cruise(ring,i,s.abortAt+duration);
            const Vec high{s.abortFrom.x,s.abortFrom.y,std::max(s.abortFrom.z,ring.z+ring.gap+30)};
            pose=elapsed<10?Hermite(s.abortFrom,{},high,{},std::max(0.0,elapsed)/10,10):Hermite(high,{},target.p,target.v,(elapsed-10)/20,20);
            pose.phase=Phase::Rising;if(elapsed>=duration)s.abort=false;
        }
        const auto actor=runtimeforms::LookupChecked(s.pilot);Vec actorPosition;uptr actorRoot=0;
        // The scene may precede vehicle physics by many seconds. Seed only
        // after the native body exists, so gravity cannot consume a seed and
        // leave a later controller chasing an already-fallen vehicle.
        std::uint32_t ids[16]{};auto count=world::SceneBodies(ref,ids);uptr instance=0;std::uint32_t body=0;
        if(!count||!Component(vehiclediag::ComponentOf(s.vehicle),s.vehicle,instance,body)||!bodybinding::Contains({ids,count},body)){controls[i].Publish({});continue;}
        if(!s.seeded){
            if(pose.visiting){s.permit=false;pose=Cruise(ring,i,t);}
            if(!Move(ref,cell,ws,pose.p,pose.yaw))continue;
            if(!ReadPosition(ref,position,root))continue;
            if(!s.assembled&&ReadPosition(actor,actorPosition,actorRoot)&&npcvehicles::VehicleOf(actor)!=ref)Move(actor,cell,ws,position,pose.yaw);
            s.seeded=true;
            count=world::SceneBodies(ref,ids);
            if(!count||!Component(vehiclediag::ComponentOf(s.vehicle),s.vehicle,instance,body)||!bodybinding::Contains({ids,count},body)){controls[i].Publish({});s.seeded=false;continue;}
        }
        if(!s.assembled&&!assembled&&ReadPosition(actor,actorPosition,actorRoot)){
            // A single native assembly attempt after both models exist. The
            // traffic schedule and physics never wait for or retry seating.
            s.assembled=true;assembled=true;
            if(npcvehicles::VehicleOf(actor)!=ref)Move(actor,cell,ws,position,pose.yaw);
            const bool success=npcvehicles::AssembleTrafficOccupant(actor,ref);
            if(success)++assemblyOk;else{++assemblyFailed;Hide(actor);}
            logger::Write("Air traffic: slot %u one-time occupant assembly %s; motion independent",i,success?"ready":"failed (actor hidden, no retries)");
        }
        controls[i].Publish({instance,ref,body,s.vehicle,now,epoch.load(),pose.p,position,pose.v,pose.yaw});s.phase=pose.phase;
        QueryPerformanceCounter(&stamp);if(double(stamp.QuadPart-began.QuadPart)*1000/double(frequency.QuadPart)>.75)break;
    }
    if(now>=reportAt){
        unsigned loaded=0,down=0,parked=0,up=0;for(const auto&s:slots)if(s.seeded&&!s.taken){++loaded;down+=s.phase==Phase::Descending;parked+=s.phase==Phase::Parked;up+=s.phase==Phase::Rising;}
        logger::Write("Air traffic: %u/%u initialized, %u descending / %u parked / %u rising; cohort %lld, %u visit slots; occupant assembly %u ready / %u failed; native steps %llu / rejected %llu; %u blocked visits (no census, no seating retries)",
            loaded,kFleet,down,parked,up,static_cast<long long>(cycle),kHalf,assemblyOk,assemblyFailed,
            static_cast<unsigned long long>(nativeSteps.exchange(0)),static_cast<unsigned long long>(nativeRejects.exchange(0)),blockedVisits);
        logger::Write("Air traffic rejects: time %llu, owner/epoch %llu, body identity %llu, target/velocity %llu, native exception %llu",
            static_cast<unsigned long long>(nativeRejectReasons[0].exchange(0)),static_cast<unsigned long long>(nativeRejectReasons[1].exchange(0)),
            static_cast<unsigned long long>(nativeRejectReasons[2].exchange(0)),static_cast<unsigned long long>(nativeRejectReasons[3].exchange(0)),
            static_cast<unsigned long long>(nativeRejectReasons[4].exchange(0)));
        reportAt=now+10000;
    }
}
void Reset(){ClearControls();slots={};resolved=cityActive=missingLogged=false;nextSlot=assemblyOk=assemblyFailed=blockedVisits=0;resolveAt=configAt=reportAt=0;daysId=scaleId=0;clock={};priorSeconds=0;ring=airtrafficdata::kRing;}
}
