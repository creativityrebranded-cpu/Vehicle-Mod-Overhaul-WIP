#include "vehiclecamera.h"
#include "vehicleflight.h"
#include "cameraroll.h"
#include "game.h"
#include "hook.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace {
    std::uintptr_t image=0,owned=111;
    bool allowed=true,patches=true;
    using Euler=void(*)(float*,float,float,float);
    Euler third=nullptr,first=nullptr;
    int calls=0;
    float last[3]{};
    void Native(float* out,float yaw,float pitch,float roll) {
        ++calls;last[0]=yaw;last[1]=pitch;last[2]=roll;
        const float basis[]={1,0,0,11,0,1,0,12,0,0,1,13};std::memcpy(out,basis,sizeof(basis));
    }
}
namespace logger { void Write(const char*,...) {} bool Verbose(){return true;} }
namespace game { std::uintptr_t Base(){return image;} }
namespace vehicleflight { bool PhysicalCameraActive(std::uintptr_t vehicle){return allowed&&vehicle==owned;} }
namespace hook {
    bool RedirectCall(std::uintptr_t site,void* expected,void* detour){
        if(expected!=reinterpret_cast<void*>(image+0x02BCE350))return false;
        if(site==image+0x00FB6CA2)third=reinterpret_cast<Euler>(detour);
        else if(site==image+0x00FB68C4)first=reinterpret_cast<Euler>(detour);
        else return false;
        return patches;
    }
}
int main(){
    int checks=0,failed=0;auto check=[&](bool ok,const char* label){++checks;if(!ok){++failed;std::printf("FAIL %s\n",label);}};
    auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,0x06200000,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!memory)return 2;image=reinterpret_cast<std::uintptr_t>(memory);
    // Independently verified against constructor 141E8A9F0..141E8AA01, rather than copied from the production gate.
    auto* state=reinterpret_cast<std::uintptr_t*>(image+0x10000);
    *state=image+0x04C28740;
    *reinterpret_cast<unsigned char*>(image+0x10074)=1;
    *reinterpret_cast<unsigned char*>(image+0x10138)=1;
    std::uintptr_t camera[3]={0,image+0x20000,reinterpret_cast<std::uintptr_t>(state)};
    *reinterpret_cast<float*>(image+0x20080)=1;*reinterpret_cast<float*>(image+0x20094)=1;*reinterpret_cast<float*>(image+0x200a8)=1;
    *reinterpret_cast<std::uintptr_t*>(image+0x061DD460)=reinterpret_cast<std::uintptr_t>(camera);
    const unsigned char jump[]={0xFF,0x25,0,0,0,0};std::memcpy(memory+0x02BCE350,jump,6);
    const auto destination=reinterpret_cast<std::uintptr_t>(&Native);std::memcpy(memory+0x02BCE356,&destination,8);
    DWORD old=0;VirtualProtect(memory+0x02BCE350,14,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),memory+0x02BCE350,14);
    check(vehiclecamera::Install()&&third&&first,"production camera installs two scoped callbacks");
    vehiclecamera::Attitude a;check(!vehiclecamera::ReadAttitude(a),"empty snapshot hidden");
    vehiclecamera::Publish(111,0.2,0.6);check(vehiclecamera::ReadAttitude(a)&&a.vehicle==111&&a.pitch==0.2&&a.roll==0.6,"physics attitude published intact");
    vehiclecamera::LogStatus(); // real native primary-vptr layout and root diagnostics must both be readable
    *state=image+0x04C28728;
    check(!vehiclecamera::ReadAttitude(a),"secondary interface vtable at +0x58 is rejected as a primary camera state");
    *state=image+0x04C28740;
    float out[12];third(out,0.25f,-0.75f,0.125f);
    check(calls==1&&last[0]==0.25f&&last[1]==-0.75f&&last[2]==0.125f,"production callback forwards native Euler arguments");
    check(out[2]<0&&out[2]>-0.6f&&out[5]==1&&out[3]==11&&out[7]==12&&out[11]==13,"production third-person wrapper inserts only smoothed bank");
    first(out,0.25f,-0.75f,0.125f);check(calls==2&&out[2]<0&&out[5]==1,"production first-person wrapper also banks around unchanged forward");
    allowed=false;third(out,0,0,0);check(out[0]==1&&out[2]==0,"mode disengagement or native fault immediately restores stock camera");
    check(!vehiclecamera::ReadAttitude(a),"disabled model hides instrument snapshot");allowed=true;
    owned=222;check(!vehiclecamera::ReadAttitude(a),"different driven vehicle cannot inherit camera or HUD");owned=111;
    *state=123;third(out,0,0,0);check(out[0]==1&&out[2]==0&&!vehiclecamera::ReadAttitude(a),"photo/on-foot/other camera state remains native");*state=image+0x04C28740;
    vehiclecamera::Publish(111,0.1,1);vehiclecamera::Reset();third(out,0,0,0);
    check(!vehiclecamera::ReadAttitude(a)&&out[2]==0,"load/stop reset clears snapshot and camera");
    vehiclecamera::Publish(111,0.1,1);
    const auto expiryDeadline=GetTickCount64()+550;
    while(GetTickCount64()<expiryDeadline)Sleep(5);
    check(!vehiclecamera::ReadAttitude(a),"stale main-thread command expires");
    vehiclecamera::Publish(111,0.1,1);third(out,0,0,0);check(out[2]<0,"fresh snapshot resumes after expiry");
    const auto saved=image;image=0;check(!vehiclecamera::ReadAttitude(a),"unreadable camera chain safely suppresses instrument");image=saved;
    patches=false;check(!vehiclecamera::Install(),"a failed call-site guard disables roll mode");
    vehiclecamera::Publish(111,0.1,1);third(out,0,0,0);check(out[0]==1&&out[2]==0,"partially installed callbacks pass through unchanged");
    vehiclecamera::Reset();VirtualFree(memory,0,MEM_RELEASE);
    std::printf("vehiclecamera: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
