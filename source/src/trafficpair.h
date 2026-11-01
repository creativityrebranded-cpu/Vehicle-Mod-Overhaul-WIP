#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cmath>
namespace trafficpair {
// A 24-bit invalid index remains invalid for every generation byte.
inline bool Body(std::uint32_t id){return id&&(id&0xFFFFFFu)!=0xFFFFFFu;}
// The game's sit/sleep state (actor state +0xF0, vtable +0x1A0): 0 normal, 1 wants to sit, 2 waiting for the sit animation,
// 3 sitting, 4 wants to stand. Only 3 is a driver in the seat (2026-10-30: decompile of the seat path FUN_141993710 ->
// FUN_1419C3980 -> FUN_141AFA090: a creation-time seat link alone leaves sit state 0; only the snap, once the driver's
// animation graph exists, sets 3. Drivers accepted at sit 0 "because they were aboard and near" never sat, so the game
// did not carry them with the vehicle and they stayed in the air when it moved).
inline constexpr int kSitting=3;
inline bool Sitting(int sit){return sit==kSitting;}
inline bool Ready(bool vehicle3D,bool driver3D,bool aboard,int sit){return vehicle3D&&driver3D&&aboard&&Sitting(sit);}
inline bool ReadyAt(bool vehicle3D,bool driver3D,bool aboard,int sit,double distance){return Ready(vehicle3D,driver3D,aboard,sit)&&std::isfinite(distance)&&distance<=8;}
// A driver at its vehicle that is not sitting gets the snap (the confirmed 2026-10-25 path); one farther away is not snapped
// (a snap from tens of metres left drivers stuck mid-air in sit state 1).
inline constexpr double kSnapReach=4.0;  // 3.0 refused a driver at the seat of a larger model (2026-10-30 10:52, spot 80: 3.0 m)
inline bool NeedsSnap(bool vehicle3D,bool driver3D,int sit,double distance){return vehicle3D&&driver3D&&!Sitting(sit)&&std::isfinite(distance)&&distance<=kSnapReach;}
inline bool MayHold(bool busy,std::size_t decorative,unsigned budget){return busy||decorative<budget;}
// A pair that will not recover by itself, replaced under a cover or out of sight (2026-10-30, the user: bad pairs should never be
// seen). Sit state is not judged: it changes for seconds after a load and the snap repair handles it. With a model missing, a
// position and a seat cannot be judged, so the pair waits for its next check.
enum class Bad{None,DriverDead,DriverOut,DriverFar,NoPilotInAir,GaveUp,NoVehicleModel,Unrecoverable,NoPhysics};
// A pilot flying a loaded vehicle that gets no physics steps: it hangs where it is, and no stuck escape can move it (2026-11-01: 60 of 61 stuck reports). Swapping it reloads the model, which builds a new chassis.
inline constexpr unsigned long long kNoPhysicsFaultMs=20000;
inline Bad JudgePair(bool vehicle3D,bool driver3D,bool driverDead,bool aboard,double distance,bool pilotHolds,bool grounded){
	if(driverDead)return Bad::DriverDead;
	if(!vehicle3D||!driver3D)return Bad::None;
	if(!aboard)return Bad::DriverOut;
	if(!std::isfinite(distance)||distance>kSnapReach)return Bad::DriverFar;
	if(!pilotHolds&&!grounded)return Bad::NoPilotInAir;
	return Bad::None;
}
inline const char* BadName(Bad b){return b==Bad::DriverDead?"its driver is dead":b==Bad::DriverOut?"its driver is out of the vehicle":b==Bad::DriverFar?"its driver is away from its seat":b==Bad::NoPilotInAir?"in the air with no pilot":b==Bad::GaveUp?"its driver never got seated":b==Bad::NoVehicleModel?"its driver showed with no vehicle model":b==Bad::Unrecoverable?"not recovered in time (pair health)":b==Bad::NoPhysics?"its pilot gets no physics steps":"healthy";}
// A driver's model exists only inside the game's grid of loaded cells; FVO keeps a vehicle's model loaded further out (Full LOD). So past about
// 190 m a driver with no model is normal, not a fault (2026-10-30 10:59 run: 153 such losses, all pilots stopped and landed, then restarted). Far from
// the player a pair flies on and nobody can see the empty seat; near, the seat can be seen, so the old rule stands: land and repair after 4 s.
inline constexpr double kDriverModelReach=180.0;
inline bool FlyWithoutDriverModel(bool following,bool vehicle3D,bool driver3D,bool aboard,double toPlayer){return following&&vehicle3D&&!driver3D&&aboard&&toPlayer>=kDriverModelReach;}
// 12 s, not 4: the driver is now brought into a cell first (npcvehicles::BringDriverIntoCell), and left alone a model took 3 to 293 s to return (2026-10-31: 45 landings in one run).
inline constexpr unsigned long long kMissingModelLandMs=12000;
inline bool LandForMissingModel(bool vehicle3D,bool driver3D,unsigned long long waitedMs,double toPlayer){return vehicle3D&&!driver3D&&waitedMs>kMissingModelLandMs&&toPlayer<kDriverModelReach;}
// The driver's rule exemption (2026-10-31, notes/driver-pass-20261031/TRACE.md). The game's own way for an actor to be in a world's far cell is the one flying actors use: FUN_140BB5180 in, FUN_140BB53C0 out.
// One native question then decides everything else, FUN_1418DB800 "this far-cell actor is too far": if yes, the move in queues its model's removal, the loader skips it and its level drops. A passed driver
// answers no while its vehicle has a model. The actor update's unload out of the grid is skipped too. The pair is one decision: a driver with no vehicle model has no model either (never a person alone in the sky),
// and a broken pair is not listed, so it unloads the native way.
inline bool DriverNeedsPass(bool listedDriver,bool vehicleHeld,bool vehicle3D,bool inFarCell){return listedDriver&&vehicleHeld&&vehicle3D&&!inFarCell;}
// Model-less job drivers cannot satisfy a seat check. Their position is kept on their
// assigned vehicle until load; allow native far-cell entry there, then prove the seat.
// A loaded actor still needs the real seat link and sitting state before entry.
inline bool DriverCanEnterPass(bool driver3D,bool aboard,int sit,double distance){return driver3D?(aboard&&Sitting(sit)):(std::isfinite(distance)&&distance<=kSnapReach);}
inline bool DriverNeedsModelRequest(bool inFarCell,bool vehicleHeld,bool vehicle3D,bool driver3D,bool pending){return inFarCell&&vehicleHeld&&vehicle3D&&!driver3D&&!pending;}
inline bool DriverSkipsCellCheck(bool listedDriver,bool inFarCell){return listedDriver&&inFarCell;}
inline bool DriverCountsAsNear(bool listedDriver,bool inFarCell,bool vehicle3D){return listedDriver&&inFarCell&&vehicle3D;}
inline bool DriverDropsModel(bool inFarCell,bool vehicle3D,bool driver3D){return inFarCell&&!vehicle3D&&driver3D;}
// A driver the player would see with no vehicle around them (2026-10-30, FF01B0FD: a model at the lane, the vehicle's not loaded).
inline bool Floating(bool driver3D,bool vehicle3D,double distance){return driver3D&&(!vehicle3D||!std::isfinite(distance)||distance>kSnapReach);}
// The driver watch's verdict on a driver whose model is loaded (api_traffic.cpp). Out: got out, on the ground. Air: got out, in the air.
enum class Seen{Seated,Seating,Alone,Apart,Out,Air};
inline Seen JudgeShown(bool linked,bool vehicle3D,double distance,int sit,bool highAboveGround){
	if(!linked)return highAboveGround?Seen::Air:Seen::Out;
	if(!vehicle3D)return Seen::Alone;
	if(Sitting(sit)&&std::isfinite(distance)&&distance<=8)return Seen::Seated;  // a long vehicle's seat is far from its centre (SebPod: 7.9 m)
	if(!std::isfinite(distance)||distance>kSnapReach)return Seen::Apart;
	return Seen::Seating;
}
inline const char* SeenName(Seen s){return s==Seen::Seated?"seated":s==Seen::Seating?"at the seat, not sitting":s==Seen::Alone?"ALONE, the vehicle has no model":s==Seen::Apart?"AWAY from the vehicle":s==Seen::Out?"out, on the ground":"out, HIGH above the ground";}
// How long a verdict lasts before it is reported: models and seats change for a few seconds after a load.
inline unsigned long long SeenReportMs(Seen s){return s==Seen::Seating?15000:s==Seen::Alone||s==Seen::Apart||s==Seen::Air?4000:0;}
// Last resort, when the generator has not dealt with it: a traffic driver with no job, seen with no vehicle for 15 s: the whole pair is hidden. With a job the pilot code owns them.
inline bool HideShown(Seen s,bool madeByTraffic,bool job,unsigned long long forMs){return madeByTraffic&&!job&&(s==Seen::Alone||s==Seen::Apart)&&forMs>=15000;}
// One health record per traffic pair, every pass asks it (2026-10-30, the user: "query all at launch, if healthy assign all passes, if unhealthy dont give an LOD pass,
// attempt recover, if not recoverable whilst far away delete"; "should be unified with the entire systems"). Core keeps the record (api_traffic.cpp), from the driver watch.
//  Unproven: no driver model yet. Passes as before: nothing is known against it.
//  Healthy: seated at its vehicle, or got out on the ground. All passes.
//  Recovering: both loaded, not seated. No Full LOD; the seat repair goes on, with full simulation (a snap needs it).
//  Broken: not seated within RecoverLimit. No Full LOD and no driver pass, so its models unload past the loaded cells; removed under the removal rule (generator: UnseenPair).
// A record that sits again is Healthy again, so a repair that takes long gets its passes back.
enum class PairHealth{Unproven=0,Healthy=1,Recovering=2,Broken=3};
// How long a repair may take is worked out from how long repairs are really taking this session (the user: "no eqn to make it dynamic"; 11:57 run: seating took 9.9 s at the
// median, 57 s at most, so a fixed 30 s would give up on healthy pairs): 1.5 times the longest recent one, within limits. Until there is a sample: the top of them.
inline constexpr unsigned long long kRecoverMinMs=20000,kRecoverMaxMs=120000;
inline unsigned long long RecoverLimit(unsigned long long longestRecentMs){if(!longestRecentMs)return kRecoverMaxMs;const unsigned long long t=longestRecentMs+longestRecentMs/2;return t<kRecoverMinMs?kRecoverMinMs:t>kRecoverMaxMs?kRecoverMaxMs:t;}
// How far something must be not to be made out: under about 1 degree of view, so distance = size x 57; never inside the loaded cells' edge.
inline constexpr double kDriverSize=1.8,kUnknownVehicleSize=7.0;
inline double NoticeFar(double sizeMetres,double loadedEdge){const double d=sizeMetres*57.0;return d>loadedEdge?d:loadedEdge;}
// A driver not in reach of a seat is written to the vehicle. One sitting (sit state 3) too: a vehicle moved by the game's build leaves a sitting driver behind (11:57 run: 35 of 74
// "knocked out" were sitting, up to 28.7 m away; a snap refuses them, and five refusals landed the vehicle).
inline bool NeedsBringing(int sit,double distance){return std::isfinite(distance)&&distance>(Sitting(sit)?8.0:kSnapReach);}
// Where a driver belongs relative to its vehicle. hold: the seat learned for a driver the game is not carrying; otherwise brought to the vehicle's centre.
struct SeatPose{float lx=0,ly=0,lz=0,yaw=0;bool hold=false;};
inline void SeatPoint(float x,float y,float z,float heading,const SeatPose& pose,float out[3]){
    const float c=std::cos(heading),s=std::sin(heading);
    out[0]=x+pose.lx*c+pose.ly*s;out[1]=y-pose.lx*s+pose.ly*c;out[2]=z+pose.lz;
}
// A held vehicle the game's build put down goes back, with room between tries: moves in the same 200 ms did not stay (11:21 run), one after the world settled did.
inline constexpr int kPutBackTries=6;inline constexpr unsigned long long kPutBackEveryMs=500,kPutBackWatchMs=8000;
inline bool PutBackAfterBuild(float zBefore,float zNow,int tries){return zBefore-zNow>=2.0f&&tries<kPutBackTries;}
inline PairHealth NextHealth(Seen seen,unsigned long long notSeatedMs,unsigned long long limitMs){return seen==Seen::Seated||seen==Seen::Out?PairHealth::Healthy:notSeatedMs>=limitMs?PairHealth::Broken:PairHealth::Recovering;}
// The model guard (2026-10-31, the user: make floating impossible). One rule whatever the cause: a traffic driver is seen only sitting at their vehicle. Out of the seat is what hides them.
// Real people stay real: someone dead, on their way out through an exit, in the player's hands now, standing on the ground, or thrown out in a fight. Getting out (sit 4) shows only near the ground, so a stand-up in the air stays hidden.
inline bool DriverMayShow(bool dead,bool exiting,bool playerTook,Seen seen,int sit,bool highAboveGround,bool inFight){
	if(dead||exiting||playerTook||seen==Seen::Seated||seen==Seen::Out)return true;
	if(sit==4&&!highAboveGround)return true;
	return seen==Seen::Air&&inFight;
}
inline bool MayHoldFar(PairHealth h){return h==PairHealth::Unproven||h==PairHealth::Healthy;}
inline bool KeepsDriverPass(PairHealth h){return h!=PairHealth::Broken;}
// Every new pair proves itself before it joins traffic (2026-10-31, the user: "always check for healthy before placing them in traffic"): both models, a body, linked, sitting at
// the vehicle, and still sitting kProveHoldMs later (a seat could break about 1 s after a load, 08:15 run). kProveLimitMs without that: the pair is deleted.
inline constexpr unsigned long long kProveHoldMs=2000,kProveLimitMs=30000;
inline bool ProveHeld(unsigned long long seatedSince,unsigned long long now){return seatedSince&&now-seatedSince>=kProveHoldMs;}
// A driver's model that comes back in flight comes back out of its seat, and a vehicle at speed outruns the snap: tries are a second apart and the snap reaches 4 m
// (2026-10-31: 39 of 46 first knock-outs over three runs came within 15 s of a model returning). The vehicle holds still for this long at most, then flies on and the repair goes on as before.
inline constexpr unsigned long long kSeatBrakeMs=8000;
inline bool SeatBrake(bool unseated,unsigned long long& since,unsigned long long now){
    if(!unseated){since=0;return false;}
    if(!since)since=now;
    return now-since<kSeatBrakeMs;
}
// Hiding a vehicle with a loaded model tears the model down. All three hang dumps of 2026-10-31 had a model release blocked against the physics step, and the
// 19:46 run hid 139 in one second, then froze. Not proven as the trigger. Near ones go as the screen closes, where they could be seen; far ones follow a few a second.
inline constexpr double kHideNear=170.0,kModelHidesPerSecond=4.0,kModelHideBurst=8.0;
struct HidePace {double tokens=kModelHideBurst;unsigned long long at=0;};
inline bool MayHide(bool hasModel,double distance,int screen,HidePace& pace,unsigned long long now){
    if(!hasModel||distance<=kHideNear)return true;
    if(screen==3)return false;
    if(pace.at)pace.tokens=std::min(kModelHideBurst,pace.tokens+double(now-pace.at)/1000.0*kModelHidesPerSecond);
    pace.at=now;
    if(pace.tokens<1.0)return false;
    pace.tokens-=1.0;return true;
}
// The check needs the driver's model, so it happens within the loaded cells: directly over the player, high enough not to be made out (NoticeFar) and over any ground the vehicle
// build could set it on. Cells load by x, y, so height does not cost the models. A slot's spot stays apart from every other slot's.
inline constexpr double kProveAbove=500.0,kProveSpacing=40.0,kProveLayer=60.0,kProveLeave=150.0;
inline constexpr int kProveRow=6;
struct ProveOffset{double x,y,z;};
inline ProveOffset ProveAt(int slot,double above){const int n=slot<0?0:slot;const int a=n%kProveRow,b=(n/kProveRow)%kProveRow,c=n/(kProveRow*kProveRow);
	return {(a-(kProveRow-1)*0.5)*kProveSpacing,(b-(kProveRow-1)*0.5)*kProveSpacing,(above>kProveAbove?above:kProveAbove)+c*kProveLayer};}
// In play new pairs leave concealed landing spots. Actual model/body availability and support checks determine readiness,
// rather than an arbitrary player-distance cutoff.
inline const char* HealthName(PairHealth h){return h==PairHealth::Healthy?"healthy":h==PairHealth::Recovering?"recovering":h==PairHealth::Broken?"broken":"unproven";}
}
