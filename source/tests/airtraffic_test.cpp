#include "airtrafficmath.h"
#include "airtrafficdata.h"
#include "aircontrol.h"
#include <thread>
#include <limits>
#include <cstdio>

int main(){
    using namespace airtrafficmath;
    int checks=0,failures=0;auto check=[&](bool b,const char* why){++checks;if(!b){++failures;std::printf("FAIL: %s\n",why);}};
    const auto ring=airtrafficdata::kRing;const double lap=Lap(ring);
    for(int cycle=-2;cycle<=2;++cycle){unsigned visits=0;for(unsigned i=0;i<kFleet;++i)visits+=Visiting(i,cycle);check(visits==kHalf,"exactly half the fleet gets a visit per rotation");}
    bool cohorts=true;for(unsigned i=0;i<kFleet;++i)cohorts=cohorts&&(Visiting(i,0)!=Visiting(i,1));check(cohorts,"cohorts exchange roles each rotation");
    bool finite=true,derivative=true,continuity=true,roles=true,uniquePads=true;double maxSpeed=0;
    for(unsigned i=0;i<kFleet;++i){
        for(unsigned j=0;j<i;++j)uniquePads=uniquePads&&airtrafficdata::kPads[i].key!=airtrafficdata::kPads[j].key;
        for(double t=0;t<lap*2;t+=.17){
            const auto p=At(ring,airtrafficdata::kPads[i],i,t);finite=finite&&Valid(p.p)&&Valid(p.v)&&std::isfinite(p.yaw);
            maxSpeed=std::max(maxSpeed,Length(p.v));
            if(p.visiting)roles=roles&&Visiting(i,std::int64_t(std::floor(t/lap)));
            const double h=1e-4;const auto a=At(ring,airtrafficdata::kPads[i],i,t-h),b=At(ring,airtrafficdata::kPads[i],i,t+h);
            derivative=derivative&&Length((b.p-a.p)*(1/(2*h))-p.v)<.2;
        }
        const double cycle=Visiting(i,0)?0:1,begin=(cycle+Stagger(i))*lap;
        for(const double edge:{begin,begin+.65*.22*lap,begin+.22*lap,begin+.40*lap,begin+.40*lap+.35*.22*lap,begin+.62*lap,lap,2*lap}){
            const auto a=At(ring,airtrafficdata::kPads[i],i,edge-1e-6),b=At(ring,airtrafficdata::kPads[i],i,edge+1e-6);
            continuity=continuity&&Length(a.p-b.p)<.001&&Length(a.v-b.v)<.001;
        }
    }
    check(finite,"all generated phases and analytic velocities remain finite");
    check(derivative,"published velocity is the derivative of the complete trajectory");
    check(continuity,"cruise/descent/dwell/ascent/rotation boundaries preserve position and velocity");
    check(roles,"only the active cohort leaves circulation");check(uniquePads,"fleet never shares an assigned placement");
    check(maxSpeed<75,"authored transfer paths fit the native controller speed envelope");
    const auto noVisit=At(ring,airtrafficdata::kPads[0],0,.3*lap,false),circle=Cruise(ring,0,.3*lap);
    check(Length(noVisit.p-circle.p)<1e-6&&!noVisit.visiting,"unavailable placements preserve circulation");
    Clock clock;const auto start=clock.Sample(10,20);check(clock.Sample(10,20)==start,"paused game time does not advance traffic");
    check(std::fabs(clock.Sample(10+20.0/86400,20)-start-1)<1e-6,"calendar increments convert to simulation seconds");
    check(std::isfinite(clock.Sample(1,20))&&clock.phase<start,"loading an earlier save resets the clock origin");
    const auto prior=clock.phase;check(clock.Sample(2,0)==prior&&clock.Sample(std::numeric_limits<double>::quiet_NaN(),20)==prior,"invalid clock readings cannot poison the schedule");
    const auto control=Follow({0,0,0},{2,0,0},{25,0,0},0,.1,0,0,.016);
    check(control.valid&&control.velocity.x>25&&control.velocity.x<=33,"native controller adds bounded tracking correction");
    check(!Follow({0,0,0},{1000,0,0},{},0,0,0,0,.016).valid,"large tracking jumps cannot force a catch-up launch");
    check(!Follow({},{},{},0,0,0,0,0).valid&&!Follow({},{},{},0,0,0,0,1).valid,"paused and invalid native steps are untouched");
    const auto local=FollowReference({0,0,0},{2,0,0},{20,0,0},{20,0,0},.1,0,0,0,0,.016);
    const auto distant=FollowReference({100000,-200000,300000},{100002,-200000,300000},{20,0,0},{20,0,0},.1,0,0,0,0,.016);
    check(local.valid&&distant.valid&&Length(local.velocity-distant.velocity)<1e-6,"tracking is invariant to a remote world origin and needs no Havok/reference offset");
    const auto prediction=FollowReference({},{},{20,0,0},{0,0,0},.1,0,0,0,0,.016);
    check(prediction.valid&&prediction.velocity.x>20&&prediction.velocity.x<22,"command age predicts reference and target motion in the same frame");
    check(!FollowReference({},{},{},{},1,0,0,0,0,.016).valid,"reference prediction rejects stale ages");
    StreamGate stream;
    check(stream.Request(250,false),"unloaded traffic can enter at its current simulation position without a loaded model");
    check(!stream.Request(250,false)&&!stream.Request(320,false),"one materialization request per streaming entry, no per-frame native move loop");
    check(!stream.Request(401,false)&&stream.Request(280,false),"a later route entry admits a fresh moving position");
    StreamGate loaded;check(!loaded.Request(200,true)&&!loaded.Request(200,false),"a loaded model consumes the same streaming admission");
    aircontrol::Mailbox mailbox;aircontrol::Target t{123,456,0x01000001,789,100,1,{1,2,3},{0,0,0},{25,0,0},.2},out;
    mailbox.Publish(t);check(mailbox.Read(123,t.body,600,out)&&out.epoch==1&&out.position.y==2,"identity-matched fresh native command is readable");
    check(!mailbox.Read(123,0x02000001,200,out),"recycled low body index cannot claim another generation");
    check(!mailbox.Read(124,t.body,200,out)&&!mailbox.Read(123,t.body,601,out)&&!mailbox.Read(123,t.body,99,out),"wrong instance and expired/future commands fail closed");
    mailbox.Publish({});check(!mailbox.Read(123,t.body,200,out),"revoking a slot removes its previous native command");
    std::atomic<bool> done=false;std::atomic<unsigned> torn=0,reads=0;
    std::thread producer([&]{for(unsigned i=0;i<50000;++i){auto x=t;x.position={double(i),double(i),double(i)};x.velocity=x.position;mailbox.Publish(x);}done=true;});
    while(!done){if(mailbox.Read(123,t.body,100,out)){++reads;if(out.position.x!=out.position.y||out.position.x!=out.velocity.z)++torn;}}
    producer.join();check(torn==0,"concurrent target publication never exposes a torn payload");
    check(std::isfinite(At(ring,{},0,std::numeric_limits<double>::quiet_NaN()).p.x),"invalid schedule times return an inert finite result");
    std::printf("airtraffic: %d checks, %d failures; max planned speed %.2f m/s, %u concurrent reads\n",checks,failures,maxSpeed,reads.load());
    return failures?1:0;
}
