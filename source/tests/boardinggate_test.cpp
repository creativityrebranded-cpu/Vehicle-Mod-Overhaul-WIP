#include "boardinggate.h"
#include "hook.h"
#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace logger { void Write(const char*,...) {} }
namespace {
using uptr=std::uintptr_t;
constexpr uptr player=0x14, npc=0x24C3E5, vehicle=0xFE008805;
thread_local boardinggate::Context context;
bool reserved=false;
int entries=0, refusals=0, playerFinishes=0, npcFinishes=0;
uptr finishedActor=0;
std::array<uptr,4> received{};
std::array<double,3> playerPosition{-871,1590,-161};
int furnitureState=0;
using Entry=void(*)(uptr,uptr);
using Sit=std::uint8_t(*)(uptr,uptr,uptr,uptr);
Entry entryOriginal=nullptr;
Sit sitOriginal=nullptr;
void NativeEntry(uptr actor,uptr target) { ++entries; received={actor,target,0,0}; furnitureState=1; }
void Finished(uptr target) {
    boardinggate::Complete(boardinggate::Finish(context,player,target,reserved),
        [] { ++playerFinishes; }, [] { ++npcFinishes; finishedActor=context.actor; });
}
std::uint8_t NativeSit(uptr process,uptr actor,uptr target,uptr action) {
    received={process,actor,target,action}; Finished(target); return 0x7f;
}
void EntryDetour(uptr actor,uptr target) {
    boardinggate::Begin(boardinggate::BeforeEntry(actor==player,target==vehicle,reserved,false,false),
        [&] { entryOriginal(actor,target); }, [](auto) { ++refusals; });
}
std::uint8_t SitDetour(uptr process,uptr actor,uptr target,uptr action) {
    boardinggate::Scope scope(context,{actor,target});
    return sitOriginal(process,actor,target,action);
}
void Jump(unsigned char* p,uptr destination) {
    const unsigned char jump[]={0xff,0x25,0,0,0,0};
    std::memcpy(p,jump,6); std::memcpy(p+6,&destination,8);
}
}
int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    using namespace boardinggate;
    check(BeforeEntry(true,true,true,false,false)==Refusal::Reserved,"NPC-reserved empty vehicle refuses before animation");
    check(BeforeEntry(true,true,false,true,false)==Refusal::Occupied,"occupied vehicle leaves native driver entry to passenger/take-wheel flow");
    check(BeforeEntry(true,true,false,false,true)==Refusal::Wrecked,"wrecked vehicle refuses before animation");
    check(BeforeEntry(true,true,false,false,false)==Refusal::None,"free vehicle uses ordinary native entry");
    check(BeforeEntry(false,true,true,true,true)==Refusal::None,"player restrictions do not block NPC entry");
    check(BeforeEntry(true,false,true,true,true)==Refusal::None,"ordinary furniture remains native");
    check(Finish({npc,vehicle},player,vehicle,true)==Completion::Actor,"NPC callback belongs to actual sitting actor");
    check(Finish({npc,vehicle},player,vehicle,false)==Completion::Actor,"NPC without a mod job cannot pull the player in");
    check(Finish({player,vehicle},player,vehicle,true)==Completion::Player,"late reservation cannot tear down an admitted player sit");
    check(Finish({npc,vehicle+1},player,vehicle,true)==Completion::Suppress,"another vehicle's callback cannot substitute its actor");
    check(Finish({},player,vehicle,true)==Completion::Suppress,"unscoped restricted callback performs no player or NPC mutation");
    check(Finish({},player,vehicle,false)==Completion::Player,"ordinary unscoped native completion remains available");
    {
        Scope outer(context,{player,vehicle});
        try { Scope inner(context,{npc,vehicle+1}); throw std::runtime_error("nested sit"); }
        catch(const std::runtime_error&) {}
        check(context.actor==player&&context.vehicle==vehicle,"nested exceptional callback restores outer actor");
    }
    check(!context.actor&&!context.vehicle,"actor scope cannot leak into the next sit");

    // Execute the actual x64 detour/trampoline with the verified native prologues,
    // rather than only testing a policy table. Tail callbacks model native state writes.
    auto* code=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!code) return 2;
    const unsigned char entryPrefix[]={0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8d,0x6c,0x24,0xe1};
    const unsigned char entryPop[]={0x41,0x5f,0x41,0x5e,0x41,0x5d,0x41,0x5c,0x5f,0x5e,0x5b,0x5d};
    const unsigned char sitPrefix[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x48,0x08,0x55,0x56,0x57,0x41,0x54};
    const unsigned char sitPop[]={0x41,0x5c,0x5f,0x5e,0x5d};
    std::memcpy(code,entryPrefix,sizeof(entryPrefix));
    std::memcpy(code+sizeof(entryPrefix),entryPop,sizeof(entryPop));
    Jump(code+sizeof(entryPrefix)+sizeof(entryPop),reinterpret_cast<uptr>(&NativeEntry));
    auto* sitCode=code+128;
    std::memcpy(sitCode,sitPrefix,sizeof(sitPrefix));
    std::memcpy(sitCode+sizeof(sitPrefix),sitPop,sizeof(sitPop));
    Jump(sitCode+sizeof(sitPrefix)+sizeof(sitPop),reinterpret_cast<uptr>(&NativeSit));
    DWORD old=0; VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old); FlushInstructionCache(GetCurrentProcess(),code,4096);
    void* original=nullptr;
    const bool entryOk=hook::Install(reinterpret_cast<uptr>(code),entryPrefix,sizeof(entryPrefix),reinterpret_cast<void*>(&EntryDetour),&original);
    entryOriginal=reinterpret_cast<Entry>(original);
    check(entryOk,"pre-entry prologue installs with complete instructions");
    const bool sitOk=hook::Install(reinterpret_cast<uptr>(sitCode),sitPrefix,sizeof(sitPrefix),reinterpret_cast<void*>(&SitDetour),&original);
    sitOriginal=reinterpret_cast<Sit>(original);
    check(sitOk,"actor-scoped prologue installs with complete instructions");
    if(!entryOk||!sitOk) return 2;
    const auto entry=reinterpret_cast<Entry>(code);
    const auto sit=reinterpret_cast<Sit>(sitCode);
    reserved=true;
    const auto before=playerPosition;
    for(int n=0;n<100;++n) entry(player,vehicle);
    check(refusals==100&&entries==0&&furnitureState==0&&playerPosition==before,
        "repeated attempts at reserved vehicle never start native seating or alter player state");
    check(sit(0x123,npc,vehicle,0x456)==0x7f,"sit result survives actor-aware trampoline");
    check(received==std::array<uptr,4>{0x123,npc,vehicle,0x456},"all four sit arguments survive trampoline");
    check(npcFinishes==1&&finishedActor==npc&&playerFinishes==0&&furnitureState==0&&playerPosition==before,
        "NPC completing immediately after player attempt leaves player pose and position untouched");
    check(!context.actor&&!context.vehicle,"native callback restores actor scope");
    Finished(vehicle);
    check(playerFinishes==0&&npcFinishes==1,"subsequent unscoped restricted callback does not reuse last actor");
    reserved=false; entry(player,vehicle);
    check(entries==1&&furnitureState==1&&received[0]==player&&received[1]==vehicle,"free vehicle entry reaches native once with original arguments");
    reserved=true; sit(0x123,player,vehicle,0x456);
    check(playerFinishes==1&&npcFinishes==1,"player already admitted completes natively even if reservation changes");
    VirtualFree(code,0,MEM_RELEASE);
    std::printf("boardinggate: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
