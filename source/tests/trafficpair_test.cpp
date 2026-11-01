#include "trafficpair.h"
#include "bodyowner.h"
#include <cstdio>
#include <limits>
int main(){int checks=0,failed=0;auto check=[&](bool ok,const char* why){++checks;if(!ok){++failed;std::printf("FAIL %s\n",why);}};
    for(unsigned gen=0;gen<256;++gen)check(!trafficpair::Body((gen<<24)|0xffffff),"every invalid body generation is rejected");
    check(!trafficpair::Body(0)&&trafficpair::Body(0x0200402e),"unset body rejected, real generation/index accepted");
    check(!trafficpair::ReadyAt(true,false,true,1,0),"invisible driver is not seated proof");
    check(!trafficpair::ReadyAt(true,true,true,0,0),"logical vehicle link without pose cannot depart");
    check(!trafficpair::ReadyAt(true,true,true,3,30),"detached airborne actor cannot depart");
    check(!trafficpair::ReadyAt(true,true,true,3,std::numeric_limits<double>::quiet_NaN()),"invalid transform rejects departure");
    check(trafficpair::ReadyAt(true,true,true,3,2),"loaded attached sitting pair may depart");
    check(!trafficpair::ReadyAt(true,true,true,1,0.5)&&!trafficpair::ReadyAt(true,true,true,2,0.5),"wants to sit / waiting for the sit animation is not sitting");
    check(trafficpair::MayHold(true,16,16)&&!trafficpair::MayHold(false,16,16),"occupied traffic does not lose retention to decoration budget");
    int calls=0;std::uintptr_t vtable=0x1100,fn=0x1800;
    auto read=[&](std::uintptr_t p){return p==5?vtable:fn;};
    auto native=[](std::uintptr_t p){return p>=0x1000&&p<0x2000;};
    auto invoke=[&](std::uintptr_t,std::uintptr_t){++calls;return std::uintptr_t(42);};
    auto r=bodyowner::Lookup(5,read,native,invoke);check(r.object==42&&!r.invalid&&calls==1,"normal body owner contract preserved");
    vtable=~std::uintptr_t(0);r=bodyowner::Lookup(5,read,native,invoke);check(!r.object&&r.invalid&&calls==1,"reported freed vtable never invoked");
    vtable=0x1f80;r=bodyowner::Lookup(5,read,native,invoke);check(r.invalid&&calls==1,"truncated vtable rejected before slot read");
    vtable=0x1100;fn=0x10080;r=bodyowner::Lookup(5,read,native,invoke);check(r.invalid&&calls==1,"earlier non-native method pointer rejected");
    r=bodyowner::Lookup(0,read,native,invoke);check(!r.object&&!r.invalid&&calls==1,"ownerless native bodies stay valid null queries");
    check(trafficpair::NeedsSnap(true,true,0,1.5),"linked at the seat but sit state 0: snap it (the creation link never sits a driver)");
    check(!trafficpair::NeedsSnap(true,true,3,1.5),"sitting: never snapped again");
    check(!trafficpair::NeedsSnap(true,true,0,36.6)&&!trafficpair::NeedsSnap(true,false,0,1.0),"far away or no driver model: no snap");
    using trafficpair::Bad;using trafficpair::JudgePair;
    check(JudgePair(true,true,false,true,2.2,true,false)==Bad::None,"seated at its seat with a pilot: healthy");
    check(JudgePair(true,true,false,true,1.0,false,true)==Bad::None,"parked on the ground without a pilot: healthy");
    check(JudgePair(false,true,false,false,50,false,false)==Bad::None&&JudgePair(true,false,false,false,50,false,false)==Bad::None,"a model missing: not judged");
    check(JudgePair(false,false,true,true,0,true,true)==Bad::DriverDead,"a dead driver is bad even unloaded");
    check(JudgePair(true,true,false,false,1.0,true,false)==Bad::DriverOut,"out of the vehicle");
    check(JudgePair(true,true,false,true,15,true,false)==Bad::DriverFar,"listed aboard but left behind");
    check(JudgePair(true,true,false,true,std::numeric_limits<double>::quiet_NaN(),true,false)==Bad::DriverFar,"invalid position counts as far");
    check(JudgePair(true,true,false,true,1.0,false,false)==Bad::NoPilotInAir,"seated in the air with no pilot");
    check(trafficpair::JudgeShown(true,true,7.9,3,false)==trafficpair::Seen::Seated,"sitting in a long vehicle, 7.9 m from its centre: seated");
    check(trafficpair::JudgeShown(true,true,7.9,0,false)==trafficpair::Seen::Apart&&trafficpair::JudgeShown(true,true,9,3,false)==trafficpair::Seen::Apart,"not sitting, or beyond any seat: away");
    using trafficpair::FlyWithoutDriverModel;using trafficpair::LandForMissingModel;
    check(FlyWithoutDriverModel(true,true,false,true,190),"far, following, aboard, driver model out of the cells: flies on");
    check(!FlyWithoutDriverModel(true,true,false,true,179)&&!FlyWithoutDriverModel(false,true,false,true,190),"near, or still boarding: does not");
    check(!FlyWithoutDriverModel(true,false,false,true,190)&&!FlyWithoutDriverModel(true,true,true,true,190)&&!FlyWithoutDriverModel(true,true,false,false,190),"no vehicle model, driver model present, or not aboard: does not");
    check(LandForMissingModel(true,false,trafficpair::kMissingModelLandMs+1,100),"near, past the wait with no driver model: lands");
    check(!LandForMissingModel(true,false,trafficpair::kMissingModelLandMs,100)&&!LandForMissingModel(true,false,60000,190)&&!LandForMissingModel(false,false,60000,100)&&!LandForMissingModel(true,true,60000,100),"too soon, far, no vehicle model, or the driver is there: does not");
    {using trafficpair::DriverMayShow;using S=trafficpair::Seen;
     check(DriverMayShow(false,false,false,S::Seated,3,true,false),"sitting at the vehicle: seen");
     check(!DriverMayShow(false,false,false,S::Seating,0,true,false)&&!DriverMayShow(false,false,false,S::Apart,3,true,false)&&!DriverMayShow(false,false,false,S::Alone,3,true,false),"at the seat not sitting, away, or with no vehicle model: never seen");
     check(!DriverMayShow(false,false,false,S::Air,0,true,false),"out and up in the air: never seen");
     check(DriverMayShow(false,false,false,S::Out,0,false,false),"out and standing on the ground: a real person, seen");
     check(DriverMayShow(true,false,false,S::Air,0,true,false)&&DriverMayShow(false,true,false,S::Apart,1,true,false)&&DriverMayShow(false,false,true,S::Apart,0,true,false),"dead, on their way out, or the player's now: always seen");
     check(DriverMayShow(false,false,false,S::Seating,4,false,false),"getting out at the ground (pulled out, parked): seen");
     check(!DriverMayShow(false,false,false,S::Seating,4,true,false),"standing up in the air: never seen");
     check(DriverMayShow(false,false,false,S::Air,0,true,true)&&!DriverMayShow(false,false,false,S::Apart,0,true,true),"thrown out in a fight: seen; still linked and adrift in one: hidden");}
    using trafficpair::DriverNeedsPass;using trafficpair::DriverSkipsCellCheck;using trafficpair::DriverDropsModel;using trafficpair::DriverCountsAsNear;
    check(DriverNeedsPass(true,true,true,false),"a driver on a job, vehicle held with a model: enters the far cell");
    check(!DriverNeedsPass(false,true,true,false)&&!DriverNeedsPass(true,false,true,false)&&!DriverNeedsPass(true,true,false,false),"anyone else, a vehicle not held, or a vehicle with no model: stays out");
    check(!DriverNeedsPass(true,true,true,true),"already there: not entered twice");
    check(trafficpair::DriverCanEnterPass(false,false,0,0),"a model-less driver with no seat link can enter at its assigned vehicle");
    check(trafficpair::DriverCanEnterPass(false,true,0,trafficpair::kSnapReach),"model-less entry includes the seat repair boundary");
    check(!trafficpair::DriverCanEnterPass(false,true,3,trafficpair::kSnapReach+0.01),"model-less drivers away from their vehicle must be brought back before entry");
    check(!trafficpair::DriverCanEnterPass(false,false,0,std::nan("")),"unknown model-less driver position cannot enter");
    check(trafficpair::DriverCanEnterPass(true,true,3,0)&&!trafficpair::DriverCanEnterPass(true,true,0,0)&&!trafficpair::DriverCanEnterPass(true,false,3,0),"loaded entry still requires an actual seated driver");
    check(trafficpair::DriverNeedsModelRequest(true,true,true,false,false),"a passed driver without a model requests the native loader");
    check(!trafficpair::DriverNeedsModelRequest(true,true,true,false,true),"an outstanding native load must not be submitted again");
    check(!trafficpair::DriverNeedsModelRequest(false,true,true,false,false)&&!trafficpair::DriverNeedsModelRequest(true,false,true,false,false)&&!trafficpair::DriverNeedsModelRequest(true,true,false,false,false)&&!trafficpair::DriverNeedsModelRequest(true,true,true,true,false),"load requests need far membership, a held loaded vehicle, and a missing driver model");
    check(DriverSkipsCellCheck(true,true),"in the far cell: the actor update's unload out of the grid is skipped");
    check(!DriverSkipsCellCheck(false,true)&&!DriverSkipsCellCheck(true,false),"not listed (a broken pair), or in an ordinary cell: the game's own check runs");
    check(DriverCountsAsNear(true,true,true),"a passed driver answers near, so entering does not queue its model's removal and the loader serves it");
    check(!DriverCountsAsNear(false,true,true)&&!DriverCountsAsNear(true,false,true),"anyone else: the game's own answer");
    check(!DriverCountsAsNear(true,true,false),"no vehicle model: the game's answer, so no driver model is loaded alone in the sky");
    check(DriverDropsModel(true,false,true),"the vehicle's model went: the driver's goes too");
    check(!DriverDropsModel(false,false,true)&&!DriverDropsModel(true,true,true)&&!DriverDropsModel(true,false,false),"otherwise nothing is dropped");
    {using trafficpair::NextHealth;using trafficpair::PairHealth;using trafficpair::Seen;using trafficpair::MayHoldFar;using trafficpair::KeepsDriverPass;
     check(NextHealth(Seen::Seated,0,30000)==PairHealth::Healthy&&NextHealth(Seen::Out,0,30000)==PairHealth::Healthy,"seated, or got out on the ground: healthy");
     check(NextHealth(Seen::Apart,1000,30000)==PairHealth::Recovering&&NextHealth(Seen::Seating,29999,30000)==PairHealth::Recovering&&NextHealth(Seen::Alone,0,30000)==PairHealth::Recovering,"not seated yet: recovering");
     check(NextHealth(Seen::Apart,30000,30000)==PairHealth::Broken&&NextHealth(Seen::Air,99999,30000)==PairHealth::Broken,"past the limit, not seated: broken");
     check(NextHealth(Seen::Seated,99999,30000)==PairHealth::Healthy,"seated again: healthy again");
     check(MayHoldFar(PairHealth::Healthy)&&MayHoldFar(PairHealth::Unproven)&&!MayHoldFar(PairHealth::Recovering)&&!MayHoldFar(PairHealth::Broken),"far pass: healthy and unproven only");
     check(KeepsDriverPass(PairHealth::Healthy)&&KeepsDriverPass(PairHealth::Recovering)&&!KeepsDriverPass(PairHealth::Broken),"driver pass: all but broken (a repair needs full simulation)");}
    {using namespace trafficpair;
     check(RecoverLimit(0)==kRecoverMaxMs,"no repair seen yet: the most time");
     check(RecoverLimit(10000)==kRecoverMinMs,"quick repairs: never under the floor");
     check(RecoverLimit(40000)==60000,"1.5 times the longest recent repair");
     check(RecoverLimit(100000)==kRecoverMaxMs,"never over the top");
     check(NoticeFar(7.2,190)>410&&NoticeFar(7.2,190)<411,"a 7 m vehicle: about 410 m");
     check(NoticeFar(kDriverSize,190)==190,"a driver: the loaded edge decides");
     check(NoticeFar(1.8,300)==300,"a bigger loaded area pushes it out");
     check(NeedsBringing(0,5)&&!NeedsBringing(0,3)&&NeedsBringing(3,28.7)&&!NeedsBringing(3,7.9),"not sitting: past a snap's reach; sitting: past any seat");
     check(!NeedsBringing(3,std::numeric_limits<double>::quiet_NaN()),"no reading: left");
     {   // A learned seat follows the vehicle wherever it is when the pose is worked out; the learning is HoldInSeat's.
         const float h=0.7f,c=std::cos(h),sn=std::sin(h),dx=1.2f,dy=-0.4f,dz=0.6f;float out[3];
         const SeatPose seat{dx*c-dy*sn,dx*sn+dy*c,dz,0.1f,true};
         SeatPoint(100,-50,30,h,seat,out);
         check(std::fabs(out[0]-101.2f)<1e-3f&&std::fabs(out[1]+50.4f)<1e-3f&&std::fabs(out[2]-30.6f)<1e-3f,"learned seat: back where it was learned");
         SeatPoint(400,200,90,h,seat,out);
         check(std::fabs(out[0]-401.2f)<1e-3f&&std::fabs(out[1]-199.6f)<1e-3f&&std::fabs(out[2]-90.6f)<1e-3f,"learned seat: carried by a vehicle that moved on");
         const float t=h+1.5707963f;SeatPoint(100,-50,30,t,seat,out);
         check(std::fabs(std::hypot(out[0]-100.0f,out[1]+50.0f)-std::hypot(dx,dy))<1e-3f&&std::fabs((out[0]-100.0f)-dy)<1e-3f&&std::fabs((out[1]+50.0f)+dx)<1e-3f,"learned seat: turns with the vehicle");
         SeatPoint(7,8,9,1.0f,SeatPose{},out);
         check(out[0]==7&&out[1]==8&&out[2]==9&&!SeatPose{}.hold,"no seat: the vehicle's centre, judged as a bring");
     }
     check(PutBackAfterBuild(62.3f,15.0f,0)&&PutBackAfterBuild(62.3f,15.0f,5),"gone down: put back");
     check(!PutBackAfterBuild(62.3f,15.0f,6),"after six tries it is left to the driver going to it");
     check(!PutBackAfterBuild(62.3f,61.0f,0)&&!PutBackAfterBuild(62.3f,70.0f,0),"a small settle or a rise: left");}
    using trafficpair::Floating;using trafficpair::Seen;using trafficpair::JudgeShown;using trafficpair::HideShown;
    check(Floating(true,false,0)&&Floating(true,true,36)&&Floating(true,true,std::numeric_limits<double>::quiet_NaN()),"a driver model with no vehicle model, or away from it, is floating");
    check(!Floating(false,false,50)&&!Floating(true,true,2),"no driver model, or at the seat: not floating");
    check(JudgeShown(true,false,0,3,false)==Seen::Alone,"linked, vehicle model missing: alone, even at sit state 3");
    check(JudgeShown(true,true,36,3,false)==Seen::Apart,"linked, far from the vehicle: apart");
    check(JudgeShown(true,true,1,3,false)==Seen::Seated&&JudgeShown(true,true,1,1,false)==Seen::Seating,"at the seat: sitting or not");
    check(JudgeShown(false,true,50,0,false)==Seen::Out&&JudgeShown(false,true,50,0,true)==Seen::Air,"no link: on the ground is ordinary, high up is not");
    check(HideShown(Seen::Alone,true,false,15000)&&HideShown(Seen::Apart,true,false,16000),"traffic driver, no job, 15 s with no vehicle: hidden");
    check(!HideShown(Seen::Alone,true,false,14999)&&!HideShown(Seen::Alone,true,true,60000)&&!HideShown(Seen::Alone,false,false,60000),"too soon, a pilot owns them, or not made by traffic: left alone");
    check(!HideShown(Seen::Out,true,false,60000)&&!HideShown(Seen::Air,true,false,60000)&&!HideShown(Seen::Seating,true,false,60000),"got out, or at the seat: never hidden");
    {   // Braking for a returned driver is bounded, once per spell out of the seat.
        using namespace trafficpair;
        unsigned long long since=0;
        check(!SeatBrake(false,since,1000)&&since==0,"seated: no brake");
        check(SeatBrake(true,since,2000)&&since==2000,"out of the seat: brakes, timer started");
        check(SeatBrake(true,since,2000+kSeatBrakeMs-1),"still braking just inside the limit");
        check(!SeatBrake(true,since,2000+kSeatBrakeMs),"limit reached: flies on");
        check(!SeatBrake(true,since,2000+kSeatBrakeMs*5)&&since==2000,"stays released, never brakes again while still unseated");
        check(!SeatBrake(false,since,90000)&&since==0&&SeatBrake(true,since,91000),"a later spell gets its own limit");
    }
    {   // Model teardowns are spread out; nothing near the player waits.
        using namespace trafficpair;
        HidePace pace;
        check(MayHide(false,5000,3,pace,1000),"no model: nothing to tear down, any time");
        check(MayHide(true,100,3,pace,1000)&&MayHide(true,kHideNear,1,pace,1000),"a near model is hidden at once, behind the screen or not");
        check(!MayHide(true,kHideNear+1,3,pace,1000),"a far model waits for the screen to close");
        int sent=0;for(int i=0;i<139;++i)if(MayHide(true,500,1,pace,2000))++sent;
        check(sent==int(kModelHideBurst),"139 far models in one moment: only the burst goes");
        check(!MayHide(true,500,1,pace,2100),"under one token back: still waiting");
        check(MayHide(true,500,1,pace,2300),"a few a second after that");
        sent=0;for(unsigned long long t=3000;t<13000;t+=50)if(MayHide(true,500,1,pace,t))++sent;
        check(sent>=38&&sent<=42,"about 4 a second over 10 s");
        pace=HidePace{};check(MayHide(true,500,1,pace,600000),"the first one after a quiet spell goes");
    }
    std::printf("trafficpair/owner: %d checks, %d failures\n",checks,failed);return failed?1:0;
}
