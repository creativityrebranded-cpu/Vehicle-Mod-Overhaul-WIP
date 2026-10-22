#include "integrationlogic.h"
#include "interactionlogic.h"
#include "combat_ai.h"
#include "coverfight.h"
#include <cstdio>
#include <limits>
namespace {
struct World final : steer::World {
    double wall=1e9;
    double Ground(double,double y) const override {return y>wall?100:0;}
    bool Solid(const steer::V3&p) const override {return p.y>wall&&p.z<100;}
    bool Blocked(const steer::V3&a,const steer::V3&b) const override {return Solid(a)||Solid(b);}
};
combatai::Situation Fight() {
    combatai::Situation s;s.now=100000;s.dt=.05;s.me.id=1;s.me.unit.body.pos={0,0,8};s.me.unit.body.vel={0,20,0};
    s.haveFoe=true;s.foe.id=2;s.foe.inVehicle=true;s.foe.unit.body.pos={0,80,8};s.foe.unit.body.vel={0,10,0};s.zone=400;
    return s;
}
}
int main() {
    int failures=0,checks=0;
    auto check=[&](bool ok,const char*msg){++checks;std::printf("%s %s\n",ok?"PASS":"FAIL",msg);if(!ok)++failures;};
    using namespace integrationlogic;
    check(ControllerFor(true,true,true,true,true,true,true,true)==Controller::Wreck,"wreck releases all active decision owners");
    check(ControllerFor(false,true,true,true,true,true,true,true)==Controller::Ram,"explicit ram owns a live vehicle");
    check(ControllerFor(false,false,true,true,true,true,true,true)==Controller::Order,"explicit orders win over rescue and combat");
    check(ControllerFor(false,false,false,true,true,true,true,true)==Controller::Rescue,"rescue outranks ordinary combat");
    check(ControllerFor(false,false,false,false,true,true,true,true)==Controller::Combat,"taxi cannot replace combat goal");
    check(ControllerFor(false,false,false,false,false,true,true,true)==Controller::Search,"remembered search cannot be overwritten by taxi");
    check(ControllerFor(false,false,false,false,false,false,true,true)==Controller::Taxi,"taxi takes control when no stronger work exists");
    check(ControllerFor(false,false,false,false,false,false,false,true)==Controller::Task,"civil task precedes idle navigation");
    check(ControllerFor(false,false,false,false,false,false,false,false)==Controller::Navigation,"idle profile remains usable");
    MotionFence f;f.cap=40;
    check(MovementAllowed(f,true),"safe final motion proposal accepted");
    check(!MovementAllowed(f,false),"unsafe rollout rejected");
    f.emergency=true;check(!MovementAllowed(f,true),"combat cannot cancel emergency braking");f.emergency=false;
    f.wall=true;check(!MovementAllowed(f,true),"combat cannot cancel wall braking");f.wall=false;
    f.person=true;check(!MovementAllowed(f,true),"combat cannot cancel pedestrian braking");f.person=false;
    f.traffic=true;check(!MovementAllowed(f,true),"combat cannot cancel yielding to traffic");f.traffic=false;
    f.cap=0;check(!MovementAllowed(f,true),"no positive speed budget means no direct motion");
    check(Clearance(1,0,1,8)==2.5,"Hug uses hull-safe minimum");
    check(Clearance(2,0,1,8)==14.5,"High actually raises clearance");
    check(Clearance(0,1,3,8)==4.5,"explicit clearance respects hull size");
    check(Clearance(2,25,1,8)==25,"explicit higher clearance retained");
    check(std::isnan(ArrivalYaw(0,NAN,0,3,0,1)),"unspecified arrival heading leaves navigation alone");
    check(std::isnan(ArrivalYaw(0,1,30,3,0,1)),"arrival heading cannot steer en route");
    check(std::isnan(ArrivalYaw(0,1,0,3,5,1)),"vehicle slows before final heading");
    check(ArrivalYaw(0,2,0,3,0,.7)==.7,"arrival rotation respects actual turn limit");
    check(std::fabs(ArrivalYaw(3.13,-3.13,0,3,0,1))<.05,"arrival heading takes shortest turn across wrap");
    Box a,b;b.p[0]=2.05;
    check(Touching(a,b),"centimetres of solver separation accepted at chassis contact");
    b.p[0]=2.2;check(!Touching(a,b),"nearby chassis do not count as a ram");
    b.p[0]=0;b.p[2]=3;check(!Touching(a,b),"overpassing vehicle does not count as contact");
    b.p[2]=0;b.half[0]=.3;b.half[1]=5;b.p[0]=3;
    check(!Touching(a,b),"overlapping bounding circles with separated hulls rejected");
    b.p[0]=2;const double sn=std::sin(.7),cs=std::cos(.7);b.axis[0][0]=cs;b.axis[0][1]=sn;b.axis[1][0]=-sn;b.axis[1][1]=cs;
    check(Touching(a,b),"rotated long hull crossing chassis is actual contact");
    bool symmetric=true;for(int i=-20;i<=20;++i){b.p[0]=i*.3;for(int j=-10;j<=10;++j){b.p[1]=j*.4;symmetric=symmetric&&(Touching(a,b)==Touching(b,a));}}
    check(symmetric,"oriented contact is symmetric across 861 offsets");
    b=Box{};b.axis[0][0]=NAN;check(!Touching(a,b),"unreadable native orientation cannot raise a ram");
    b=Box{};b.axis[1][0]=1;check(!Touching(a,b),"invalid nonorthogonal native frame rejected");
    a=Box{};b=Box{};Box pa=a,pb=b;pa.p[0]=-4;a.p[0]=4;
    check(SweptTouch(a,b,pa,pb,.1),"fast actual frame crossing detected");
    pb.p[1]=b.p[1]=3;check(!SweptTouch(a,b,pa,pb,.1),"fast adjacent pass does not cause damage or crime");
    check(!SweptTouch(a,b,pa,pb,1),"stale frames cannot manufacture a collision");
    a.p[0]=100;check(!SweptTouch(a,b,pa,pb,.1),"teleport cannot manufacture a collision");
    check(!Impact(20,20,false),"contact without momentum change is not an impact");
    check(!Impact(3,0,true),"low-speed bump does not enter hard-ram path");
    check(Impact(20,10,false),"measured impact accepted without mod physics ownership");
    check(Impact(20,20,true),"native solver impact accepted despite smoothed velocity");
    check(!Impact(NAN,0,true),"invalid impact evidence rejected");
    check(coverfight::OnTarget(0,0,150,200),"remote cover uses native long weapon range");
    check(!coverfight::OnTarget(0,0,50,30),"remote cover respects native short weapon range");
    check(!coverfight::OnTarget(0,0,50,NAN),"invalid weapon range never enables firing");
    check(coverfight::OnTarget(.4,.5,60,100),"own cover accepts launch re-aim within the wider firing cone");
    check(coverfight::OnTarget(.65,.8,5,100),"close enemy has a wider bounded firing cone");
    check(!coverfight::OnTarget(1.0,0,5,100)&&!coverfight::OnTarget(3.0,0,5,100),"own cover never fires sideways or backwards");
    check(!coverfight::OnTarget(0,0,150,100),"700 metre pursuit never grants extra weapon range");
    coverfight::Burst burst;
    check(burst.Update(true,1000)&&burst.Update(true,3999),"clear target receives a full three-second burst");
    check(!burst.Update(true,4000)&&!burst.Update(true,4399)&&burst.Update(true,4400),"completed burst has a 400 ms pause");
    check(!burst.Update(false,4450)&&burst.Update(true,4460),"brief alignment loss stops fire immediately without a full pause penalty");
    check(coverfight::KeepTarget(true,true,3999)&&!coverfight::KeepTarget(true,true,4000),"higher-priority threat waits for minimum target commitment");
    check(coverfight::KeepTarget(true,false,10000)&&!coverfight::KeepTarget(false,false,1000),"valid same-tier target stays while invalid target drops immediately");
    check(!coverfight::Reposition(true,true,false,false,10000,1000),"firing from a useful position prevents combat repositioning");
    check(!coverfight::Reposition(true,false,false,false,3999,1000)&&!coverfight::Reposition(true,false,false,false,5000,749),"new positions require both hold time and sustained obstruction");
    check(coverfight::Reposition(true,false,false,false,5000,750),"sustained blocked lane permits a new firing position");
    check(coverfight::Reposition(true,true,true,false,100,0)&&coverfight::Reposition(true,true,false,true,100,0),"moving protectee or immediate danger overrides position commitment");
    brain::Squad squad;squad.count=3;for(int i=0;i<3;++i){squad.m[i].id=i+1;squad.m[i].pos={double(i*20),0,5};}
    brain::Enemy enemies[2];enemies[0].id=10;enemies[0].pos={0,150,5};enemies[0].vel={20,0,0};enemies[1]=enemies[0];enemies[1].id=11;enemies[1].health=.5;
    brain::Assign(squad,enemies,2,10000,true);
    check(squad.focus==11,"focus picks weakest known native enemy");
    const auto role=squad.m[0].role;const auto slot=squad.m[0].slot;
    squad.m[0].pos={200,0,5};squad.m[2].pos={0,150,5};
    const bool changed=brain::Assign(squad,enemies,2,12000,true);
    check(changed&&squad.m[0].role==role&&squad.m[0].slot!=slot,"same role with a different box slot is propagated");
    enemies[0].health=.1;check(brain::Assign(squad,enemies,2,14000,true)&&squad.focus==10,"focus change propagates without needing a new role enum");
    brain::Memory m,other;m.Sense(2,{10,0,0},{1,0,0},true,false,.5,1000);other.Sense(2,{80,0,0},{},false,true,1,2000);m.Merge(other);
    check(m.Find(2)->pos.x==80&&m.Find(2)->seenAt==1000&&m.Find(2)->heardAt==2000,"newer hearing updates shared location without inventing a sighting");
    World w;combatai::Switches sw;combatai::State st;combatai::Output out;auto s=Fight();
    combatai::Tick(st,s,w,sw,out);s.haveFoe=false;s.foe={};s.now+=1000;combatai::Tick(st,s,w,sw,out);
    check(st.search.active&&!out.fire&&!out.aim,"unloaded enemy still produces remembered search, never blind fire");
    check(st.search.origin.y==100,"search is anchored to last known motion");
    s.now+=61000;combatai::Tick(st,s,w,sw,out);check(!st.search.active,"search expires instead of pursuing an unloaded actor forever");
    st={};s=Fight();s.foe.visible=false;s.foe.heard=true;combatai::Tick(st,s,w,sw,out);s.haveFoe=false;s.now+=4000;combatai::Tick(st,s,w,sw,out);
    check(st.search.active&&st.search.origin.y==80,"hearing alone gives a real search location");
    st={};s=Fight();s.role=brain::Role::Flanker;combatai::Tick(st,s,w,sw,out);st.ms.phase=3;
    s.haveFoe=false;s.now+=4000;combatai::Tick(st,s,w,sw,out);s.haveFoe=true;s.foe.visible=true;s.now+=1000;combatai::Tick(st,s,w,sw,out);
    check(st.ms.started&&st.ms.phase==0&&st.ms.since==s.now,"returning to same manoeuvre after search resets its phases");
    st.ms.phase=3;s.foe.id=3;s.now+=100;combatai::Tick(st,s,w,sw,out);check(st.ms.phase==0&&st.ms.since==s.now,"new native enemy resets manoeuvre even if chosen action is unchanged");
    tactics::TState ts;tactics::Unit me,it;tactics::Facts facts;tactics::Enter(ts,tactics::T::BoxIn,me,it,facts,1000,0);tactics::Enter(ts,tactics::T::BoxIn,me,it,facts,1100,2);
    check(ts.slot==2,"live box-slot reassignment reaches tactic steering");
    Box ejectHull; ejectHull.p[2]=150; ejectHull.half[0]=8; ejectHull.half[1]=3; ejectHull.half[2]=2;
    double exitRight[3]{},exitLeft[3]{};
    check(EjectPosition(ejectHull,0,1,exitRight)&&EjectPosition(ejectHull,0,-1,exitLeft),"air ejection has both hull-relative escape sides");
    check(exitRight[0]==9.5&&exitLeft[0]==-9.5&&exitRight[2]==150&&exitLeft[2]==150,"wide vehicle clears its actual hull without teleporting NPC to ground");
    Box actorBox; actorBox.p[0]=exitRight[0];actorBox.p[2]=exitRight[2]+1;actorBox.half[0]=actorBox.half[1]=.5;
    check(!Touching(ejectHull,actorBox),"ejected upright actor has room outside the hull");
    ejectHull.axis[0][0]=0;ejectHull.axis[0][2]=1;ejectHull.axis[2][0]=-1;ejectHull.axis[2][2]=0;
    check(EjectPosition(ejectHull,0,1,exitRight)&&exitRight[0]==3.5&&exitRight[2]==150,"rolled vehicle uses projected hull extent at flight altitude");
    ejectHull=Box{};ejectHull.p[0]=100;ejectHull.p[1]=200;ejectHull.p[2]=80;
    check(EjectPosition(ejectHull,1.5707963267948966,1,exitRight)&&std::fabs(exitRight[0]-100)<1e-6&&std::fabs(exitRight[1]-197.5)<1e-6,"escape follows vehicle heading in world coordinates");
    ejectHull.half[0]=NAN;check(!EjectPosition(ejectHull,0,1,exitRight),"unreadable hull cannot manufacture an ejection point");
    check(!EjectPosition(Box{},NAN,1,exitRight),"unreadable heading refuses ejection");
    const float chassisCenter[]{0,50,5},chassisRotation[]{1,0,0,0,1,0,0,0,1},chassisHalf[]{1.2f,2.3f,1};
    const float launch[]{0,0,5},firstSample[]{0,100,5};double fraction=0;
    check(interactionlogic::HullHit(chassisCenter,chassisRotation,chassisHalf,launch,firstSample,fraction)&&fraction>0.47&&fraction<0.49,"fast shot crosses vehicle in launch-to-first-sample segment");
    const float impact[]{0,48,5};
    check(interactionlogic::HullHit(chassisCenter,chassisRotation,chassisHalf,impact,impact,fraction),"native contact inside chassis counts even without surviving projectile frames");
    const float miss[]{2,48,5},invalid[]{NAN,48,5};
    check(!interactionlogic::HullHit(chassisCenter,chassisRotation,chassisHalf,miss,miss,fraction),"native impact beside chassis does not manufacture vehicle damage");
    check(!interactionlogic::HullHit(chassisCenter,chassisRotation,chassisHalf,invalid,invalid,fraction),"invalid contact cannot damage every vehicle");
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
