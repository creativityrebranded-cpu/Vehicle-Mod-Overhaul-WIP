#pragma once
// When traffic may be created (2026-11-06, the user): only during the loading screen, or when X is pressed in the Traffic Manager.
namespace spawnrule {
inline int Presentation(bool loadingCover,bool worldReady,bool panelCover,bool pausedMap){
    if(loadingCover)return 3;
    if(!worldReady)return 0;
    return panelCover||pausedMap?2:1;
}
// screen: 3 loading screen, 2 under the local map, 1 play, 0 none. reset: X's fill is running.
inline bool Open(int screen,bool reset){return screen==3||(screen==2&&reset);}
inline constexpr unsigned long long kResetLimitMs=60000;
inline constexpr unsigned long long kSettleLimitMs=8000;

enum class Preparation : int { Filling=100, Settling, Complete, TimedOut, CoverLost };
inline bool IsPreparation(int phase) {
    return phase>=int(Preparation::Filling)&&phase<=int(Preparation::CoverLost);
}
inline bool Active(Preparation phase) {
    return phase==Preparation::Filling||phase==Preparation::Settling;
}
inline Preparation DecidePreparation(bool cover,bool pending,bool settling,unsigned long long elapsed) {
    if(!cover)return Preparation::CoverLost;
    if(elapsed>=kResetLimitMs)return Preparation::TimedOut;
    if(pending)return Preparation::Filling;
    return settling?Preparation::Settling:Preparation::Complete;
}
}
