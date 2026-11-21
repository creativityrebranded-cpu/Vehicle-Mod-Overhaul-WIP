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
// LV-3 (2026-11-20, the user: "we create vehicles like X but this time LIVE ... for a large stretch of whatever road we're in"): the road is
// also filled during play. Open() above stays the covered rule (loading screen, X); LiveOpen is the play rule. It never replaces the core's own
// refusal of a pose the player can see; it only says WHEN the Arbiter may ask. Waits for the route clock and for a settle time after any cover
// (the loading screen's own fill, X, a map) so live births never overlap the covered fill or the world's first frames.
inline constexpr unsigned long long kLiveSettleMs=8000;
inline bool LiveOpen(int screen,bool clockPending,unsigned long long sinceCoverMs){return screen==1&&!clockPending&&sinceCoverMs>=kLiveSettleMs;}
// Live creation (2026-11-20): a car may be made in play wherever the player cannot see it: a line of sight blocked by the world, or
// outside the camera view (and the camera not about to look there) and at least this far from the player so the hull is clear of them.
inline constexpr double kUnseenMinM=40.0;
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
