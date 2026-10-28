#include "trafficschedule.h"
#include <cstdio>
#include <cstdlib>
using namespace trafficschedule;
int checks=0;
void Check(bool ok,const char*why){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
int main(){
    Schedule s;s.Reset();Check(s.Valid(),"initial ownership");
    for(int p=0;p<3;++p)Check(s.Occupied(p)==2,"capacity derives from the two physical bays");
    Check(!s.Board(0,-1,1,100000),"empty queue cannot become a driver");
    s.Ready(1,100);s.Ready(0,200);Check(s.ReadyActor(0)==1,"FIFO by ready ticket, not actor ID");
    Check(!s.Board(0,0,1,100000),"cannot skip queue front");
    Check(s.Board(0,1,1,100000),"assign front once");
    Check(!s.Depart(0,true,110000),"unconfirmed boarding never departs");
    Check(s.Seated(0,110000),"native seating acknowledgement");
    s.ports[0].enabled=false;Check(!s.Depart(0,true,120000),"missing exit holds parked vehicle");
    s.ports[0].enabled=true;Check(!s.Depart(0,false,120000),"unknown clearance holds parked vehicle");
    Check(s.Depart(0,true,120000),"departure corridor granted");
    Check(s.bays[0].owner==0,"starting a climb does not free physical space");
    Check(s.ClearedBay(0,130000),"verified clearance releases bay");Check(s.bays[0].owner==-1,"bay available after physical clearance");
    s.Set(0,Phase::Ring,140000);std::array<bool,6> clear;clear.fill(true);
    Check(!s.Reserve(0,1,clear,200000),"full destination adds a lap");
    s.Ready(6,150000);Check(s.Board(2,6,2,200000),"second departure");s.Seated(2,201000);s.Depart(2,true,210000);s.ClearedBay(2,220000);s.Set(2,Phase::Ring,220000);
    s.ports[1].enabled=false;Check(!s.Reserve(0,1,clear,230000),"closed exit keeps traffic on ring");
    s.ports[1].enabled=true;clear[2]=false;Check(!s.Reserve(0,1,clear,230000),"foreign vehicle blocks otherwise free bay");
    clear[2]=true;Check(s.Reserve(0,1,clear,230000),"free bay reserved before exit");
    Check(s.Occupied(1)==2,"inbound reservation counts against n");
    s.vehicles[2].destination=1;Check(!s.Reserve(2,1,clear,300000),"cannot double reserve a bay or corridor");
    Check(!s.Exited(0,240000),"cannot claim disembark before native unload");
    s.Set(0,Phase::Descend,240000);Check(s.Landed(0,250000),"landed retains bay and releases corridor");
    Check(s.bays[2].owner==0,"unload occupies space");Check(s.Exited(0,260000),"confirmed native exit rotates roster");
    Check(s.actors[1].duty==Duty::Walking&&s.vehicles[0].actor==-1,"old pilot walks away and is not immediately reused");
    Check(s.Valid(),"ownership remains consistent after rotation");
    Schedule failure;failure.Reset();failure.Ready(0,0);Check(failure.Board(0,0,1,100000),"boarding failure setup");failure.BoardFailed(0,100001);
    Check(failure.actors[0].duty==Duty::Rest&&failure.vehicles[0].phase==Phase::Wait,"failure retires actor for cooldown");
    Check(!failure.Board(0,0,1,200000),"resting actor is not retried by an empty queue");Check(failure.Valid(),"failure releases pilot but retains bay");
    // Repeated service under full/closed/blocked destinations. Every iteration
    // checks global ownership, not just the state change being exercised.
    Schedule run;run.Reset();std::uint64_t now=100000;unsigned completed=0;
    for(int a=0;a<18;++a)run.Ready(a,now+a);
    for(int tick=0;tick<50000;++tick){now+=1000;const int i=tick%6;auto&v=run.vehicles[i];
        for(int a=0;a<18;++a)if((run.actors[a].duty==Duty::Rest||run.actors[a].duty==Duty::Walking)&&now-run.actors[a].at>=60000){run.actors[a].bay=a/3;run.Ready(a,now);}
        run.ports[1].enabled=(tick%300)>70;
        switch(v.phase){
        case Phase::Wait:{const int a=run.ReadyActor(v.bay);if(a>=0)run.Board(i,a,i+1,now);break;}
        case Phase::Boarding:run.Seated(i,now);break;
        case Phase::Departure:run.Depart(i,true,now);break;
        case Phase::Climb:run.ClearedBay(i,now);break;
        case Phase::Join:run.Set(i,Phase::Ring,now);break;
        case Phase::Ring:run.Reserve(i,v.destination,clear,now);break;
        case Phase::Approach:run.Set(i,Phase::Descend,now);break;
        case Phase::Descend:run.Landed(i,now);break;
        case Phase::Unload:if(run.Exited(i,now))++completed;break;
        default:break;
        }
        Check(run.Valid(),"long-run ownership conservation");for(int p=0;p<3;++p)Check(run.Occupied(p)<=2,"never exceed physical capacity");
    }
    Check(completed>500,"schedule makes sustained progress despite periodic closure");
    std::printf("%d checks; %u completed services; PASS\n",checks,completed);
}
