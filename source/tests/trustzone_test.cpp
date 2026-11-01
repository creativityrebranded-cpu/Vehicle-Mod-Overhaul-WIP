#include "../src/trustzone.h"
#include <cstdio>
using namespace trustzone;
int main(){int n=0,failed=0;auto check=[&](bool ok,const char* why){++n;if(!ok){++failed;std::printf("FAIL %s\n",why);}};
    PlayerPath path;
    check(!path.Ahead(100,0,0,1000),"nothing is ahead before the player has been seen");
    path.Sample(0,0,0,1000);
    check(!path.Ahead(100,0,0,1000),"nothing is ahead until a direction is known");
    path.Sample(1,0,0,1100);
    check(path.Speed()==10,"speed comes from real movement");
    check(path.Ahead(100,0,0,1100),"a pair on the way is ahead");
    check(path.Ahead(100,100,0,1100),"so is one beside the path within its width");
    check(!path.Ahead(100,400,0,1100),"one well off to the side is not");
    check(!path.Ahead(-100,0,0,1100),"one behind the player is not");
    check(path.Ahead(200,0,0,1100)&&!path.Ahead(500,0,0,1100),"the path ends at the lead: 12 s at 10 m/s, plus its width");
    check(!path.Ahead(100,0,0,3000),"a stale reading predicts nothing");
    path.Sample(2,0,0,1150);check(path.x==1&&path.at==1100,"samples closer than the interval are ignored");
    path.Sample(5000,0,0,1200);check(path.Speed()==0&&!path.Ahead(5100,0,0,1200),"a jump (a load, a door) forgets the direction");
    PlayerPath standing;standing.Sample(0,0,0,1000);standing.Sample(0.05,0,0,1100);
    check(standing.Speed()<kMovingSpeed&&!standing.Ahead(50,0,0,1100),"standing still, nothing is ahead");
    PlayerPath driving;driving.Sample(0,0,0,1000);driving.Sample(3,0,0,1100);driving.Sample(3,0,0,1200);
    check(driving.Speed()>=kMovingSpeed,"one reading with no change does not stop a moving player");
    for(Time t=1300;t<=2000;t+=100)driving.Sample(3,0,0,t);
    check(driving.Speed()<kMovingSpeed,"a real stop is believed within a second");
    PlayerPath paused;paused.Sample(0,0,0,1000);paused.Sample(1,0,0,1100);paused.Sample(1,0,0,9000);check(paused.Speed()==0,"a long gap between samples is not movement");
    PlayerPath bad;bad.Sample(0,0,0,1000);bad.Sample(1,0,0,1100);bad.Sample(NAN,0,0,1200);check(bad.Speed()==0&&!bad.Ahead(100,0,0,1200),"an unreadable position predicts nothing");

    check(FaultWaitMs(false)==10000,"the existing 10 s wait stands away from the player's path");
    check(FaultWaitMs(true)<FaultWaitMs(false),"a fault ahead is put right sooner");

    Record r;Facts f;
    check(r.stage==Stage::Unloaded&&!r.Update(f,1000),"no models: unloaded, nothing to judge");
    f.vehicleModel=f.driverModel=true;
    check(r.Update(f,1100)&&r.stage==Stage::Trusted,"models arrive: trusted from that moment, with no proof asked for");
    f.passes=true;r.Update(f,1200);check(r.stage==Stage::Trusted,"one good reading is not yet checked");
    r.Update(f,1200+kPassHoldMs-1);check(r.stage==Stage::Trusted,"nor before the hold");
    check(r.Update(f,1200+kPassHoldMs)&&r.stage==Stage::Checked,"held long enough: checked");
    f.passes=false;Stage before=r.stage;r.Update(f,4000);
    check(r.stage==Stage::Trusted&&LostCheck(before,r.stage),"a checked pair that stops passing goes back to trusted, and is counted");
    r.Update(f,4100);check(!LostCheck(r.stage,r.stage),"counted once");
    f.passes=true;r.Update(f,5000);check(r.stage==Stage::Trusted&&r.passSince==5000,"it has to hold again");
    f.fault=true;r.Update(f,5100);check(r.stage==Stage::Faulty&&!r.passSince,"a proven fault overrides everything");
    f.fault=false;r.Update(f,5200);check(r.stage==Stage::Trusted,"a fault that clears goes back to trusted");
    r.Update(f,5200+kPassHoldMs);before=r.stage;f.driverModel=false;r.Update(f,8000);
    check(before==Stage::Checked&&r.stage==Stage::Unloaded&&!LostCheck(before,r.stage),"losing a model to distance is not a lost check");
    f.driverModel=true;r.Update(f,8100);check(r.stage==Stage::Trusted,"coming back it is trusted again");

    check(CheckedRadius({})==0,"no pairs, no zone");
    check(CheckedRadius({{50,Stage::Checked},{100,Stage::Checked},{200,Stage::Trusted},{300,Stage::Checked}})==100,"the zone reaches to the first pair not yet checked");
    check(CheckedRadius({{200,Stage::Checked},{50,Stage::Checked}})==200,"the order given does not matter");
    check(CheckedRadius({{50,Stage::Unloaded},{100,Stage::Checked},{900,Stage::Unloaded}})==100,"unloaded pairs neither stop nor grow it");
    check(CheckedRadius({{50,Stage::Trusted},{100,Stage::Checked}})==0,"a trusted pair nearest keeps it at nothing");
    check(CheckedRadius({{50,Stage::Faulty},{100,Stage::Checked}})==0,"so does a faulty one");
    std::printf("trust zone: %d checks, %d failures\n",n,failed);return failed?1:0;
}
