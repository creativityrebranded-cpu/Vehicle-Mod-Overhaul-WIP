#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Authored behaviour only. No engine queries, support fabrication or native
// motion-mode values. Existing routes opt in explicitly.
namespace travelrules {
enum Mode { Legacy, Drive, Hover, SurfaceFirst };
enum Bypass { Hold, Around, Over, AroundThenOver };
struct Rules {
    int mode=Legacy,bypass=Hold;
    double minHeight=0,maxHeight=40,hoverHeight=4,supportRange=8,settleSeconds=2;
};
inline bool Valid(const Rules& r) {
    for(double n:{r.minHeight,r.maxHeight,r.hoverHeight,r.supportRange,r.settleSeconds})if(!std::isfinite(n))return false;
    return r.mode>=Legacy&&r.mode<=SurfaceFirst&&r.bypass>=Hold&&r.bypass<=AroundThenOver&&
        r.minHeight>=0&&r.maxHeight>=r.minHeight&&r.maxHeight<=200&&r.hoverHeight>=r.minHeight&&r.hoverHeight<=r.maxHeight&&
        r.supportRange>=.25&&r.supportRange<=200&&r.settleSeconds>=.25&&r.settleSeconds<=30;
}
inline double VerticalCommand(double z,double minimum,double maximum,double command){if(!std::isfinite(z)||!std::isfinite(minimum)||!std::isfinite(maximum)||!std::isfinite(command)||minimum>maximum)return 0;if(z>=maximum&&command>0)return 0;if(z<=minimum&&command<0)return 0;return command;}
inline bool Sideways(const Rules& r){return r.bypass==Around||r.bypass==AroundThenOver;}
inline bool FlyOver(const Rules& r){return r.mode!=Drive&&(r.bypass==Over||r.bypass==AroundThenOver);}
inline const char* Name(Mode m){const char* s[]{"Existing rules","Drive only","Hover only","Surface-first"};return s[std::clamp(int(m),0,3)];}
inline const char* Name(Bypass m){const char* s[]{"Hold","Go around","Fly over","Around, then over"};return s[std::clamp(int(m),0,3)];}
inline bool Same(const Rules& a,const Rules& b){return a.mode==b.mode&&a.bypass==b.bypass&&a.minHeight==b.minHeight&&a.maxHeight==b.maxHeight&&a.hoverHeight==b.hoverHeight&&a.supportRange==b.supportRange&&a.settleSeconds==b.settleSeconds;}
}
extern "C" {
struct FvoTravelRulesV1 {
    std::uint32_t size=sizeof(FvoTravelRulesV1),version=1;
    std::uint64_t identity=0;
    travelrules::Rules rules;
};
using FvoTravelRulesProviderV1=int (*)(void*,FvoTravelRulesV1*);
using FvoRegisterTravelRulesV1=int (*)(FvoTravelRulesProviderV1);
}
