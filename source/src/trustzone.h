#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
// Trust first (2026-11-01, the user): the pairs around the player are assumed correct, so nothing has to be proven before it shows. The zone is then
// checked and grows outward as the game loads models. Replaces the certify-then-show rings: certifying needs live physics, which does not run under the
// loading screen, so they left the area around the player empty (trips within 120 s of a load: 46 before them, 8 with them).
// This decides nothing about showing. A pair is only ever removed for a proven fault, and only unseen (generator: ShelveBad, UnseenPair).
namespace trustzone {
using Time=unsigned long long;
// Where the player is heading. Fixed lead: a measured one fed back on itself and ran to its 120 s cap (2026-11-01 01:55 run).
inline constexpr double kLeadSeconds=12.0,kPathWidth=150.0,kMovingSpeed=2.0,kJump=200.0;
inline constexpr Time kSampleMs=100,kStaleMs=1000;
struct PlayerPath {
    double x=0,y=0,z=0,vx=0,vy=0,vz=0;Time at=0;
    void Sample(double nx,double ny,double nz,Time now){
        if(!std::isfinite(nx)||!std::isfinite(ny)||!std::isfinite(nz)){at=0;vx=vy=vz=0;return;}
        if(at&&now>at&&now-at<kSampleMs)return;
        const double dt=at&&now>at?double(now-at)/1000.0:0;
        const bool jumped=!at||dt<=0||dt>double(kStaleMs)/1000.0||std::hypot(std::hypot(nx-x,ny-y),nz-z)>kJump;
        if(jumped)vx=vy=vz=0;  // a load, a door, a pause: no direction is known yet
        else {
            // A reading with no change between two updates is common at speed: slowing is believed gradually, speeding up at once.
            const double rx=(nx-x)/dt,ry=(ny-y)/dt,rz=(nz-z)/dt;
            const bool faster=std::hypot(std::hypot(rx,ry),rz)>=Speed();
            vx=faster?rx:rx*0.65+vx*0.35;vy=faster?ry:ry*0.65+vy*0.35;vz=faster?rz:rz*0.65+vz*0.35;
        }
        x=nx;y=ny;z=nz;at=now;
    }
    double Speed()const{return std::hypot(std::hypot(vx,vy),vz);}
    // On the stretch the player reaches within the lead. Standing still, nothing is ahead.
    bool Ahead(double px,double py,double pz,Time now)const{
        if(!at||now<at||now-at>kStaleMs||Speed()<kMovingSpeed||!std::isfinite(px)||!std::isfinite(py)||!std::isfinite(pz))return false;
        const double sx=vx*kLeadSeconds,sy=vy*kLeadSeconds,sz=vz*kLeadSeconds,len2=sx*sx+sy*sy+sz*sz;
        const double rx=px-x,ry=py-y,rz=pz-z,along=(rx*sx+ry*sy+rz*sz)/len2;
        if(along<=0)return false;  // beside or behind
        const double t=std::min(along,1.0);
        return std::hypot(std::hypot(rx-sx*t,ry-sy*t),rz-sz*t)<=kPathWidth;
    }
};
// How long a proven fault waits for the seat repair before the pair is swapped. Ahead of the player it goes sooner, so it is put right before they arrive. Seating took
// 9.9 s at the median in the 11:57 run, so a recovering pair ahead can be swapped that would have healed; only ever where it cannot be seen.
inline constexpr Time kFaultWaitMs=10000,kFaultWaitAheadMs=5000;
inline Time FaultWaitMs(bool ahead){return ahead?kFaultWaitAheadMs:kFaultWaitMs;}
// A pair's place in the zone.
//  Unloaded: a model is missing, so nothing can be checked and nothing can be seen wrong.
//  Trusted: assumed correct, not yet seen passing. The state every pair starts in, and it shows.
//  Checked: seated at its vehicle with a chassis and something holding it up, for kPassHoldMs.
//  Faulty: the generator's verdict says it will not recover by itself.
enum class Stage {Unloaded,Trusted,Checked,Faulty};
inline const char* Name(Stage s){return s==Stage::Unloaded?"unloaded":s==Stage::Trusted?"trusted":s==Stage::Checked?"checked":"faulty";}
inline constexpr Time kPassHoldMs=2000;
struct Facts {bool vehicleModel=false,driverModel=false,fault=false,passes=false;};
struct Record {
    Stage stage=Stage::Unloaded;Time passSince=0;
    // Returns true when the stage changed.
    bool Update(const Facts& f,Time now){
        const Stage before=stage;
        if(!f.passes||f.fault||!f.vehicleModel||!f.driverModel)passSince=0;
        else if(!passSince)passSince=now;
        if(!f.vehicleModel||!f.driverModel)stage=Stage::Unloaded;
        else if(f.fault)stage=Stage::Faulty;
        else if(passSince&&now-passSince>=kPassHoldMs)stage=Stage::Checked;
        else stage=Stage::Trusted;
        return stage!=before;
    }
};
// A checked pair that stopped passing. Counted with the time since the last cover ended: this is the risk of trusting pairs, a fault that shows only once physics runs.
inline bool LostCheck(Stage before,Stage now){return before==Stage::Checked&&(now==Stage::Trusted||now==Stage::Faulty);}
// How far out the zone has been put right: everything loaded inside this distance is checked. It grows as the game loads models and they pass.
struct Seen {double distance=0;Stage stage=Stage::Unloaded;};
inline double CheckedRadius(std::vector<Seen> pairs){
    std::sort(pairs.begin(),pairs.end(),[](const Seen& a,const Seen& b){return a.distance<b.distance;});
    double radius=0;
    for(const auto& p:pairs){
        if(p.stage==Stage::Unloaded)continue;
        if(p.stage!=Stage::Checked)break;
        radius=p.distance;
    }
    return radius;
}
}
