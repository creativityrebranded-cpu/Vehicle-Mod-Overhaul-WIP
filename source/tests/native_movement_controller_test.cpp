#include "native_movement_controller.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace native_movement;
namespace {
unsigned checks = 0;
void Require(bool pass, const char* why) {
    ++checks;
    if (!pass) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
template<class T> T Read(const void* p, std::size_t n) {
    T v; std::memcpy(&v,static_cast<const std::byte*>(p)+n,sizeof v); return v;
}
template<class T> void Write(void* p, std::size_t n, T v) {
    std::memcpy(static_cast<std::byte*>(p)+n,&v,sizeof v);
}
template<class Fn> Fn Method(void* p, std::size_t n) {
    return reinterpret_cast<Fn>(Read<std::uintptr_t>(Read<void*>(p,0),n));
}
int goalKey, outputKey, pathingObject, requestA, requestB;
std::array<std::uintptr_t,49> nativeTable{};
std::array<std::uintptr_t,8> setterTable{};
struct Fixture {
    std::array<std::byte,0x90> data{};
    std::array<std::byte,0x70> setter{};
    std::array<std::byte,0x70> output{};
    std::uint32_t id = 0;
    std::uint64_t mailbox = 0;
    void* request = nullptr;
    bool active = false, normal = false, emit = true, accept = true, outputPresent = true;
    unsigned cancels = 0, updates = 0;
    std::uint32_t nextState = 2;
    Vector rotation{0,0,0.5f};
};
std::unordered_map<void*,std::unique_ptr<Fixture>> live;
std::vector<char> events;
bool wrongOwner = false, failActivation = false;
unsigned destroys = 0;
Fixture& ForData(void* p) {
    for (auto& [_,f]:live) if (f->data.data()==p) return *f;
    std::abort();
}
Fixture& ForSetter(void* p) {
    for (auto& [_,f]:live) if (f->setter.data()==p) return *f;
    std::abort();
}
std::uint32_t OwnerId(void* p) { return ForData(p).id + (wrongOwner?1:0); }
void Rotation(void* p,Vector* out) { *out=ForData(p).rotation; }
bool NonActor(std::uint32_t id) { return id >= 0x100; }
void Construct(void* component,void* unused,std::uint32_t id,const PathingDataView* pathing) {
    Require(unused==nullptr,"reserved constructor argument changed");
    Require(pathing->link->object==&pathingObject,"lost fourth constructor argument / DB indirection");
    auto f=std::make_unique<Fixture>();
    f->id=id;
    Write(f->data.data(),0,nativeTable.data());
    Write(f->setter.data(),0,setterTable.data());
    Write(component,0x78,f->data.data());
    Write(component,0x48,f.get());
    live[component]=std::move(f);
    events.push_back('C');
}
void Destroy(void* component) {
    auto& f=*live.at(component);
    Require(f.request==nullptr,"destroyed controller with a request still owned");
    Require(Read<std::uintptr_t*>(f.data.data(),0)==nativeTable.data(),"private geometry table outlives its owner");
    live.erase(component); ++destroys; events.push_back('D');
}
void Activate(void* component) {
    auto& f=*live.at(component);
    Require(!f.normal,"normal set selected before full activation");
    f.active=!failActivation;
    Write(component,0x5c,std::uint8_t(f.active?2:1)); events.push_back('A');
}
void Normal(void* component) {
    auto& f=*live.at(component); f.normal=f.active; events.push_back('N');
}
void* Lookup(void* registry,const void* key) {
    auto& f=*static_cast<Fixture*>(registry);
    if (key==&goalKey) return f.setter.data();
    if (key==&outputKey && f.normal && f.outputPresent) return f.output.data()+0x20;
    return nullptr;
}
bool Submit(void* setter,void** request) {
    auto& f=ForSetter(setter);
    Require(f.active&&f.normal,"submission before planner activation");
    f.request=*request;
    Write(setter,0x48,std::uint32_t{1}); events.push_back('S');
    return f.accept;
}
void Cancel(void* setter) {
    auto& f=ForSetter(setter);
    f.request=nullptr; ++f.mailbox; ++f.cancels;
    Write(setter,0x48,std::uint32_t{0}); events.push_back('X');
}
void Update(void* component,float dt,const std::uint8_t* flags,void* result,void* aux,bool finalFlag) {
    auto& f=*live.at(component);
    Require(dt>0&&dt<=0.25f&&*flags==0&&!finalFlag,"wrong native update arguments");
    // Exercise the full output buffer extents in the production adapter.
    std::memset(result,0xa5,0x48); std::memset(aux,0x5a,0x18);
    Write(f.setter.data(),0x48,f.nextState); ++f.updates;
    if (f.emit) {
        Write(f.output.data(),0x30,Vector{0,0,0.25f});
        Write(f.output.data(),0x40,12.0f);
        Write(f.output.data(),0x50,Vector{0,0,-0.25f});
    }
}
ControllerApi Api() {
    return {NonActor,Construct,Destroy,Activate,Normal,Lookup,Update,&goalKey,&outputKey};
}
Fixture& Find(std::uint32_t id) {
    for (auto& [_,f]:live) if (f->id==id) return *f;
    std::abort();
}
bool Near(float a,float b) { return std::abs(a-b)<0.0001f; }
}

int main() {
    nativeTable[0x10/8]=reinterpret_cast<std::uintptr_t>(&OwnerId);
    nativeTable[0x30/8]=reinterpret_cast<std::uintptr_t>(&Rotation);
    setterTable[1]=reinterpret_cast<std::uintptr_t>(&Submit);
    setterTable[4]=reinterpret_cast<std::uintptr_t>(&Cancel);
    PathingDataLink link{&pathingObject}; PathingDataView pathing{&link};
    Bounds hull{{-2,-5,-1},{3,4,2},true};
    OwnedController a(Api()),b(Api());
    Require(!a.Open(0x14,pathing,hull)&&live.empty(),"player/actor owner reached native constructor");
    auto fallback=hull; fallback.measured=false;
    Require(!a.Open(0x100,pathing,fallback)&&live.empty(),"unmeasured fallback bounds accepted");
    Require(!a.Open(0x100,{},hull)&&live.empty(),"missing native pathing data fabricated");
    auto invalid=hull; invalid.max.x=std::numeric_limits<float>::infinity();
    Require(!a.Open(0x100,pathing,invalid)&&live.empty(),"nonfinite hull accepted");
    Require(a.Open(0x100,pathing,hull),"first private controller did not open");
    Require(events.size()>=3&&events[0]=='C'&&events[1]=='A'&&events[2]=='N',"wrong native construction sequence");
    Require(b.Open(0x200,pathing,{{-1,-1,0},{1,1,1},true}),"second private controller did not open");
    auto& fa=Find(0x100); auto& fb=Find(0x200);
    Require(Read<void*>(fa.data.data(),0)!=Read<void*>(fb.data.data(),0),"geometry tables shared between vehicles");
    const float radius=Method<float (*)(void*)>(fa.data.data(),0x70)(fa.data.data());
    Require(Near(radius,std::hypot(3.f,5.f)),"offset hull corners are outside planner radius");
    Require(Near(Method<float (*)(void*)>(fa.data.data(),0x78)(fa.data.data()),5),"wrong hull width");
    Require(Near(Method<float (*)(void*)>(fa.data.data(),0x80)(fa.data.data()),9),"wrong hull length");
    Require(Near(Method<float (*)(void*)>(fa.data.data(),0x88)(fa.data.data()),3),"wrong hull height");
    std::array<Vector,2> box;
    Method<void* (*)(void*,void*)>(fa.data.data(),0x90)(fa.data.data(),box.data());
    Require(box[0].y==-5&&box[1].x==3,"bounds centre offset lost");
    Require(a.Submit(&requestA)&&b.Submit(&requestB),"independent requests rejected");
    const auto generation=a.Generation(), oldMailbox=fa.mailbox;
    auto first=a.Update(0.016f,false);
    Require(first.status==ControllerStatus::Following&&first.motion.valid,"fresh output not delivered");
    Require(Near(first.motion.rotation.z,0.75f),"output did not use native state rotation");
    Require(Near(std::hypot(first.motion.velocity.x,first.motion.velocity.y),12),"speed interpreted as positional delta");
    fa.emit=false;
    Require(!a.Update(0.016f,false).motion.valid,"stale planner output replayed");
    fa.nextState=1; fa.emit=true;
    Require(!a.Update(0.016f,false).motion.valid,"pending path produced a drive command");
    const auto updates=fa.updates;
    Require(!a.Update(1,false).motion.valid&&fa.updates==updates,"large timestep reached engine");
    Require(!a.Update(std::numeric_limits<float>::quiet_NaN(),false).motion.valid,"invalid timestep accepted");
    Require(a.Submit(&requestB)&&a.Generation()>generation&&fa.mailbox>oldMailbox,"replacement kept old generation/mailbox");
    Require(fb.request==&requestB&&fb.cancels==1,"other vehicle request was cancelled");
    fa.accept=false;
    Require(!a.Submit(&requestA)&&fa.request==nullptr,"rejected enqueue kept its partial request");
    Require(a.Status()==ControllerStatus::Blocked,"enqueue failure not reported");
    fa.accept=true; Require(a.Submit(&requestA),"retry rejected");
    fa.nextState=3;
    Require(a.Update(0.016f,false).status==ControllerStatus::Arrived,"native completion not delivered");
    Require(!a.Update(0.016f,false).motion.valid,"complete path still actuates");
    Require(a.Submit(&requestA),"new trip after arrival rejected");
    fa.nextState=0;
    Require(a.Update(0.016f,false).status==ControllerStatus::Blocked,"native failure not delivered");
    a.Close(); a.Close();
    Require(destroys==1&&live.size()==1,"close not idempotent or destroyed wrong controller");
    wrongOwner=true;
    Require(!a.Open(0x300,pathing,hull)&&live.size()==1,"native data bound to a different reference accepted");
    wrongOwner=false; failActivation=true;
    Require(!a.Open(0x300,pathing,hull)&&live.size()==1,"virtual-follower controller reported ready");
    failActivation=false; b.Close();
    Require(live.empty(),"native controller leaked");
    std::printf("native controller: %u checks passed (engine ABI fixture; not an in-game test)\n",checks);
}
