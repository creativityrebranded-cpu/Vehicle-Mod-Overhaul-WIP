#pragma once
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
enum class Bad{None,DriverDead,DriverOut,DriverFar,NoPilotInAir,GaveUp,NoVehicleModel,Unrecoverable};
inline Bad JudgePair(bool vehicle3D,bool driver3D,bool driverDead,bool aboard,double distance,bool pilotHolds,bool grounded){
	if(driverDead)return Bad::DriverDead;
	if(!vehicle3D||!driver3D)return Bad::None;
	if(!aboard)return Bad::DriverOut;
	if(!std::isfinite(distance)||distance>kSnapReach)return Bad::DriverFar;
	if(!pilotHolds&&!grounded)return Bad::NoPilotInAir;
	return Bad::None;
}
inline const char* BadName(Bad b){return b==Bad::DriverDead?"its driver is dead":b==Bad::DriverOut?"its driver is out of the vehicle":b==Bad::DriverFar?"its driver is away from its seat":b==Bad::NoPilotInAir?"in the air with no pilot":b==Bad::GaveUp?"its driver never got seated":b==Bad::NoVehicleModel?"its driver showed with no vehicle model":b==Bad::Unrecoverable?"not recovered in time (pair health)":"healthy";}
// A driver's model exists only inside the game's grid of loaded cells; FVO keeps a vehicle's model loaded further out (Full LOD). So past about
// 190 m a driver with no model is normal, not a fault (2026-10-30 10:59 run: 153 such losses, all pilots stopped and landed, then restarted). Far from
// the player a pair flies on and nobody can see the empty seat; near, the seat can be seen, so the old rule stands: land and repair after 4 s.
inline constexpr double kDriverModelReach=180.0;
inline bool FlyWithoutDriverModel(bool following,bool vehicle3D,bool driver3D,bool aboard,double toPlayer){return following&&vehicle3D&&!driver3D&&aboard&&toPlayer>=kDriverModelReach;}
inline bool LandForMissingModel(bool vehicle3D,bool driver3D,unsigned long long waitedMs,double toPlayer){return vehicle3D&&!driver3D&&waitedMs>4000&&toPlayer<kDriverModelReach;}
// The driver's rule exemption (2026-10-30, the user: "a rule exemption like is done with vehicles to allow them to show far away"). The game takes a driver's model away when
// it moves into a cell that is not loaded and when its cell unloads. The vehicle's own way out (the persistent cell) refuses drivers: the setter returns at once, 237 of 237
// tries in the 13:11 run. Two native rules do it instead, stopping nothing part way (stopping the removal once an unload had started: the 12:39 crash, heap corruption):
//  - the cell unload FUN_140B16860 skips a reference that has something attached to it (ExtraAttachRefChildren, the script function ObjectReference.AttachTo). A small marker with
//    no physics is attached to the driver: attaching two things that both have physics would link them, and could pull the vehicle about.
//  - the actor update's cell check FUN_1418EBF00 is skipped while the driver is far, so it never moves into a cell that is not loaded. Near the player it runs, so the driver
//    is back in an ordinary loaded cell, and when the vehicle is not held or has no model, the game's own rules apply.
inline bool DriverNeedsPassMark(bool listedDriver,bool vehicleHeld,bool vehicle3D,bool driver3D,bool marked){return listedDriver&&vehicleHeld&&vehicle3D&&driver3D&&!marked;}
inline bool DriverSkipsCellCheck(bool listedDriver,bool vehicleHeld,bool vehicle3D,bool marked,double toPlayer){return listedDriver&&vehicleHeld&&vehicle3D&&marked&&toPlayer>=kDriverModelReach;}
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
// A held vehicle the game's build put down goes back, with room between tries: moves in the same 200 ms did not stay (11:21 run), one after the world settled did.
inline constexpr int kPutBackTries=6;inline constexpr unsigned long long kPutBackEveryMs=500,kPutBackWatchMs=8000;
inline bool PutBackAfterBuild(float zBefore,float zNow,int tries){return zBefore-zNow>=2.0f&&tries<kPutBackTries;}
inline PairHealth NextHealth(Seen seen,unsigned long long notSeatedMs,unsigned long long limitMs){return seen==Seen::Seated||seen==Seen::Out?PairHealth::Healthy:notSeatedMs>=limitMs?PairHealth::Broken:PairHealth::Recovering;}
inline bool MayHoldFar(PairHealth h){return h==PairHealth::Unproven||h==PairHealth::Healthy;}
inline bool KeepsDriverPass(PairHealth h){return h!=PairHealth::Broken;}
inline const char* HealthName(PairHealth h){return h==PairHealth::Healthy?"healthy":h==PairHealth::Recovering?"recovering":h==PairHealth::Broken?"broken":"unproven";}
}
