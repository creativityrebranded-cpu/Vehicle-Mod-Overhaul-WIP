#include "hulllogic.h"
#include "vtolplan.h"
#include <cstdio>

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name) { ++checks; if(!ok) { ++failed; std::printf("FAIL %s\n",name); } };
    const double dt=0.25;
    const vtolplan::Limits lim;

    // Straight flight: no slide, speed eases toward the target within the acceleration.
    vtolplan::State s{0,20,0};
    s=vtolplan::Step(s,0,0,30,lim,dt);
    check(std::fabs(s.vx)<1e-9&&std::fabs(s.vy-21.5)<1e-9,"straight flight accelerates along the nose with no slide");

    // A turn at speed: the velocity lags the nose (a slide toward the outside), so the path is wider than the nose's.
    s={0,30,0};double h=0,x=0,y=0,kx=0,ky=0,maxSlip=0;
    for(int k=0;k<8;++k){h+=0.6*dt;s=vtolplan::Step(s,h,0.6,30,lim,dt);x+=s.vx*dt;y+=s.vy*dt;kx+=std::sin(h)*30*dt;ky+=std::cos(h)*30*dt;
        maxSlip=std::max(maxSlip,-vtolplan::Slip(s,h));}
    check(maxSlip>1.0,"a right turn at 30 m/s slides outward (left of the nose)");
    check(x<kx,"the physical path turns less tightly than the nose-following (standard) path");
    check(s.bank>0.1,"the chassis banks into a right turn");

    // Rolling out: the slide is pulled back to the nose.
    for(int k=0;k<12;++k)s=vtolplan::Step(s,h,0,30,lim,dt);
    check(std::fabs(vtolplan::Slip(s,h))<0.5&&std::fabs(s.bank)<0.05,"after the turn the slide and bank settle");

    // Hovering in place: turning the nose moves nothing.
    s={};s=vtolplan::Step(s,0.5,1.0,0,lim,dt);
    check(std::fabs(s.vx)<1e-9&&std::fabs(s.vy)<1e-9,"a pure yaw in a hover does not move the chassis");

    // Bad input keeps the state.
    s={1,2,0.1};const auto same=vtolplan::Step(s,NAN,0,10,lim,dt);
    check(same.vx==1&&same.vy==2&&same.bank==0.1,"non-finite input leaves the state unchanged");

    // Hull limits for physical flight.
    const auto base=hulllogic::For(1.25,2.3,1),phys=hulllogic::Physical(base);
    check(phys.lateral<=hulllogic::kPhysicalLateral&&phys.lateral<=base.lateral,"physical grip is capped at the chassis' sustained turn");
    check(phys.YawAt(30)<base.YawAt(30)&&std::fabs(phys.YawAt(2)-base.YawAt(2))<1e-9,"physical turn rate is lower at speed, unchanged when slow");
    check(std::fabs(base.StopDistance(30)-(30.0*30/(2*base.brake)+base.length+1))<1e-9,"standard stop distance is unchanged");
    check(phys.StopDistance(30)>base.StopDistance(30),"physical stop distance includes the braking tail");
    const double knee=phys.brake/phys.brakeGain;
    check(std::fabs(phys.StopDistance(knee*0.5)-(knee*0.5/phys.brakeGain+phys.length+1))<1e-9,"below the knee the tail is speed / gain");
    check(std::fabs(phys.StopDistance(0)-(phys.length+1))<1e-9,"at rest only the hull length is kept");
    std::printf("vtolplan: %d checks, %d failures\n",checks,failed);
    return failed?1:0;
}
