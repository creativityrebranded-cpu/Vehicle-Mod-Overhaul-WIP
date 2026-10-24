#include "hook.h"
#include "cameraroll.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace logger { void Write(const char*,...) {} }
namespace {
    using Euler=void(*)(float*,float,float,float);
    int nativeCalls=0,detourCalls=0;
    float received[3]{};
    void Native(float* out,float yaw,float pitch,float roll) {
        ++nativeCalls;received[0]=yaw;received[1]=pitch;received[2]=roll;
        const float matrix[]={1,0,0,99,0,1,0,98,0,0,1,97};std::memcpy(out,matrix,sizeof(matrix));
    }
    bool active=false;
    void Detour(float* out,float yaw,float pitch,float roll) {
        ++detourCalls;Native(out,yaw,pitch,roll);if(active)cameraroll::Apply(out,cameraroll::kPi/2);
    }
}
int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* label){++checks;if(!ok){++failed;std::printf("FAIL %s\n",label);}};
    auto* code=static_cast<unsigned char*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    if(!code)return 2;
    // Real Windows x64 call fixture. RCX/XMM1/XMM2/XMM3 survive the redirected E8 and its absolute relay.
    const unsigned char caller[]={0x48,0x83,0xEC,0x28,0xE8,23,0,0,0,0x48,0x83,0xC4,0x28,0xC3};
    std::memcpy(code,caller,sizeof(caller));
    const unsigned char jump[]={0xFF,0x25,0,0,0,0};std::memcpy(code+32,jump,6);
    const auto destination=reinterpret_cast<std::uintptr_t>(&Native);std::memcpy(code+38,&destination,8);
    DWORD old=0;VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old);FlushInstructionCache(GetCurrentProcess(),code,4096);
    const auto call=reinterpret_cast<Euler>(code);
    float out[12];call(out,1.25f,-2.5f,0.75f);
    check(nativeCalls==1&&detourCalls==0,"unpatched native call works");
    unsigned char before[5];std::memcpy(before,code+4,5);
    check(!hook::RedirectCall(reinterpret_cast<std::uintptr_t>(code+4),reinterpret_cast<void*>(&Native),reinterpret_cast<void*>(&Detour)),"wrong expected destination refuses patch");
    check(std::memcmp(before,code+4,5)==0,"mismatched bytes unchanged");
    check(!hook::RedirectCall(1,code+32,reinterpret_cast<void*>(&Detour)),"unreadable call address fails safely");
    check(!hook::RedirectCall(reinterpret_cast<std::uintptr_t>(code),code+32,reinterpret_cast<void*>(&Detour)),"non-E8 opcode fails safely");
    check(hook::RedirectCall(reinterpret_cast<std::uintptr_t>(code+4),code+32,reinterpret_cast<void*>(&Detour)),"guarded call redirect installed via executable near relay");
    call(out,1.25f,-2.5f,0.75f);
    check(nativeCalls==2&&detourCalls==1&&received[0]==1.25f&&received[1]==-2.5f&&received[2]==0.75f,"all four native arguments and ABI preserved");
    check(out[0]==1&&out[2]==0&&out[10]==1,"inactive mode remains stock through patched call");
    active=true;call(out,0.5f,0.25f,-0.125f);
    check(nativeCalls==3&&detourCalls==2&&std::fabs(out[2]+1)<1e-6&&out[5]==1,"active mode inserts roll after native matrix");
    check(out[3]==99&&out[7]==98&&out[11]==97,"patch preserves native output padding");
    check(!hook::RedirectCall(reinterpret_cast<std::uintptr_t>(code+4),code+32,reinterpret_cast<void*>(&Detour)),"already redirected site refuses a second install");
    VirtualFree(code,0,MEM_RELEASE);
    std::printf("hook_call: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
