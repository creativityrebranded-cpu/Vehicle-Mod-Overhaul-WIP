#include "ringschedule.h"
#include "trafficspots.h"
#include "own_spots.h"
#include <cstdio>
#include <set>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    using namespace ringschedule;
    // The user's ring and recorded New Atlantis spots (ringroad.ini, own_spots.h).
    ringlogic::Ring ring=ringlogic::Default(0,0,0);
    ring.cx=-858.7;ring.cy=1567.4;ring.z=153.5;
    std::vector<trafficspots::In> in;
    for(const auto& s:ownspots::kSpots)in.push_back({s.x,s.y,s.z,s.heading,s.key});
    Spot homes[kVehicles];
    trafficspots::Assign(in.data(),int(in.size()),homes);

    int lower=0,upper=0;std::set<std::pair<long long,long long>> used;bool levelsRight=true;
    for(int k=0;k<kVehicles;++k){
        if(!homes[k].valid)continue;
        (k%2?upper:lower)++;
        used.insert({std::llround(homes[k].x*10),std::llround(homes[k].y*10)});
        if(k%2==0&&homes[k].z>-80)levelsRight=false;
        if(k%2==1&&homes[k].z<-80)levelsRight=false;
    }
    check(lower==16&&upper==16,"all 32 vehicles get a home spot (16 per level)");
    check(int(used.size())==lower+upper,"no two vehicles share a home spot");
    check(levelsRight,"the lower ring's homes are on the lower level, the upper ring's on the upper");

    const double lap=LapTime(ring);
    check(lap>100&&lap<160,"a 600 m ring at 30 m/s takes about two minutes a lap");
    const auto a=At(ring,homes[7],7,12345.6),b=At(ring,homes[7],7,12345.6);
    check(a.kind==b.kind&&a.x==b.x&&a.y==b.y&&a.z==b.z,"the schedule depends only on the time");

    // Continuity through every state change, for every vehicle, over two visit cycles.
    double worst=0;int kinds[4]={};
    for(int k=0;k<kVehicles;++k){
        auto prev=At(ring,homes[k],k,0);
        for(double t=0.05;t<lap*kVisitEvery*2;t+=0.05){
            const auto s=At(ring,homes[k],k,t);++kinds[int(s.kind)];
            worst=std::max(worst,std::sqrt((s.x-prev.x)*(s.x-prev.x)+(s.y-prev.y)*(s.y-prev.y)+(s.z-prev.z)*(s.z-prev.z)));
            prev=s;
        }
    }
    check(worst<6.0,"no vehicle jumps more than 6 m in 50 ms");
    check(kinds[0]&&kinds[1]&&kinds[2]&&kinds[3],"vehicles cruise, leave, park and rejoin");

    // Parked at its own spot, leaving from its own nearest exit.
    bool home=true;
    for(int k=0;k<kVehicles;++k)for(double t=0;t<lap*kVisitEvery;t+=1.0){
        const auto s=At(ring,homes[k],k,t);
        if(s.kind==Kind::Parked&&(std::fabs(s.x-homes[k].x)>1e-6||std::fabs(s.y-homes[k].y)>1e-6))home=false;
        if(s.exit>=0&&s.exit!=HomeExit(ring,homes[k]))home=false;
    }
    check(home,"each vehicle parks at its own spot and uses its own nearest exit");

    // On the ring: spread out.
    double closest=1e9;
    for(double t=0;t<lap*kVisitEvery;t+=0.5){
        State s[kVehicles];for(int k=0;k<kVehicles;++k)s[k]=At(ring,homes[k],k,t);
        for(int i=0;i<kVehicles;++i)for(int j=i+1;j<kVehicles;++j){
            if(s[i].kind!=Kind::Ring||s[j].kind!=Kind::Ring||s[i].layer!=s[j].layer)continue;
            closest=std::min(closest,std::hypot(s[i].x-s[j].x,s[i].y-s[j].y));
        }
    }
    check(closest>150,"vehicles on one ring stay well apart (16 per ring)");
    int away=0;for(int k=0;k<kVehicles;++k)if(At(ring,homes[k],k,500).kind!=Kind::Ring)++away;
    check(away<=kVehicles/kVisitEvery+2,"about a quarter of the vehicles are off the ring at a time");

    // No home: the vehicle stays on the ring.
    bool off=false;for(double t=0;t<lap*kVisitEvery*2;t+=3.0)if(At(ring,Spot{},5,t).kind!=Kind::Ring)off=true;
    check(!off,"a vehicle without a home spot stays on the ring");
    // Directions and heights.
    const auto l0=At(ring,Spot{},0,0),l1=At(ring,Spot{},0,1),u0=At(ring,Spot{},1,0),u1=At(ring,Spot{},1,1);
    const double cl=((l0.x-ring.cx)*(l1.y-ring.cy)-(l0.y-ring.cy)*(l1.x-ring.cx)),cu=((u0.x-ring.cx)*(u1.y-ring.cy)-(u0.y-ring.cy)*(u1.x-ring.cx));
    check(cl<0&&cu>0,"the lower ring turns clockwise, the upper counter-clockwise");
    check(std::fabs(u0.z-l0.z-ring.gap)<1e-9,"the upper ring flies the gap higher");
    std::printf("ringschedule: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
