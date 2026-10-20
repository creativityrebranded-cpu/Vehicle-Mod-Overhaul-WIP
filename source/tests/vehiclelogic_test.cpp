#include "bodybinding.h"
#include "hoverlogic.h"
#include "targetsession.h"
#include "interactionlogic.h"
#include "travellogic.h"
#include "hulllogic.h"
#include "boostlogic.h"
#include "ramlogic.h"
#include <cstdio>
#include <cmath>
#include <limits>
int main(){
 int failures=0;auto check=[&](bool ok,const char*message){std::printf("%s %s\n",ok?"PASS":"FAIL",message);if(!ok)++failures;};
 bodybinding::Binding owners[]={{0x01000011,true},{0x01000022,true},{0x01000033,false}};
 check(bodybinding::Owner(0x01000011,owners)==0,"player body resolves only to player controller");
 check(bodybinding::Owner(0x01000022,owners)==1,"NPC body resolves only to its controller");
 check(bodybinding::Owner(0x01000044,owners)==-1,"nearby unknown vehicle never inherits player hover");
 check(bodybinding::Owner(0x02000011,owners)==-1,"reused body index with new generation is rejected");
 check(bodybinding::Owner(0x01000033,owners)==-1,"inactive owner cannot receive force");
 owners[1]=owners[0];check(bodybinding::Owner(0x01000011,owners)==-1,"duplicate ownership fails closed");
 check(bodybinding::Owner(0,owners)==-1,"missing collision identity fails closed");
 const std::uint32_t scene[]={0x01000055,0x01000011};
 check(bodybinding::Contains(scene,0x01000011),"native chassis can be found after a decorative collision body");
 check(!bodybinding::Contains(scene,0x02000011),"owned body set still checks generation");
 const double pi=std::acos(-1.0);
 check(std::fabs(hoverlogic::Roll(-1,0)-pi/2)<1e-8,"side-on chassis remains 90 degrees from world up");
 check(hoverlogic::Roll(-std::sin(2*pi/3),std::cos(2*pi/3))>pi/2,"roll does not fold after 90 degrees");
 check(std::fabs(hoverlogic::Roll(0,-1))>3,"upside-down chassis cannot appear level");
 check(hoverlogic::TiltLift(40,0,0.6)==0,"side-on lift cannot produce a huge sideways shove");
 check(std::fabs(hoverlogic::TiltLift(10,1,0.6)-6)<1e-8,"upright lift preserves normal bank response");
 const double angle=0.4;const float tilted[]={1,0,0,0,float(std::cos(angle)),float(std::sin(angle)),0,float(-std::sin(angle)),float(std::cos(angle))};
 const double attached[]={0,std::cos(angle),std::sin(angle)};
 check(std::fabs(hoverlogic::RelativePitch(tilted,attached))<1e-7,"first-person body pitch is removed from camera command");
 const double looking[]={0,std::cos(angle+0.2),std::sin(angle+0.2)};
 check(std::fabs(hoverlogic::RelativePitch(tilted,looking)-0.2)<1e-7,"first-person look input still commands pitch");
 targetsession::Session session;targetsession::Marker marker{12,10,20,30};session.Exit(marker);
 check(!session.Accept(marker),"driver exit consumes the previous target");
 check(session.Accept({12,20,20,30}),"moving a reused native marker re-arms destination");
 check(session.Accept({13,10,20,30}),"a new marker handle re-arms destination");
 session.Selected();check(session.Accept(marker),"explicit scanner selection can reuse same location");
 check(targetsession::CanFollow(8,8),"own vehicle may follow");
 check(!targetsession::CanFollow(9,8)&&!targetsession::CanFollow(0,0),"other and unowned vehicles cannot follow");
 travellogic::Context travel{true,true,false,false,400,450};
 check(travellogic::UseVehicle(travel),"known outdoor path can use a vehicle");
 travel.outdoors=false;check(!travellogic::UseVehicle(travel),"interior destinations stay on foot");travel.outdoors=true;
 travel.changesSpace=true;check(!travellogic::UseVehicle(travel),"path through another world stays on foot");travel.changesSpace=false;
 travel.door=true;check(!travellogic::UseVehicle(travel),"door route stays on foot");travel.door=false;
 travel.known=false;check(!travellogic::UseVehicle(travel),"unresolved path context cannot grab a nearby vehicle");travel.known=true;
 travel.distance=40;check(!travellogic::UseVehicle(travel),"short walk stays on foot");
 const auto rover=hulllogic::For(1.25,2.3,1),bus=hulllogic::For(2,8,2),wide=hulllogic::For(8,2,1),huge=hulllogic::For(30,80,15);
 check(bus.yaw<rover.yaw&&bus.pitch<rover.pitch,"long hull turns and pitches more slowly");
 check(wide.roll<rover.roll,"wide hull rolls more slowly");
 check(bus.StopDistance(25)>rover.StopDistance(25),"larger hull brakes earlier");
 check(rover.YawAt(60)<rover.YawAt(10),"turn rate decreases with speed");
 check(huge.length==80&&huge.width==30,"large vehicle dimensions are not clipped to rover bounds");
 check(std::isfinite(hulllogic::For(NAN,0,-1).yaw),"missing hull bounds retain finite fallback");
 const float pos[]={0,0,0},rot[]={1,0,0,0,1,0,0,0,1},half[]={1,3,1};double t=0;
 const float a[]={-10,0,0},b[]={10,0,0},missA[]={2,0,-5},missB[]={2,0,5};
 check(interactionlogic::HullHit(pos,rot,half,a,b,t)&&t>0&&t<1,"projectile crossing actual hull counts");
 check(!interactionlogic::HullHit(pos,rot,half,missA,missB,t),"shot beside narrow hull is not false aggression");
 check(!interactionlogic::OtherVehicle(5,7,5),"shooter's own vehicle is excluded");
 check(interactionlogic::OtherVehicle(8,7,5),"another person's vehicle counts regardless of faction side");
 auto meter=boostlogic::Step(1,1,true,false,0.1,2,1,4);
 check(meter.active&&std::fabs(meter.charge-0.95)<1e-8,"held boost consumes native meter at vehicle's burn rate");
 meter=boostlogic::Step(0.01,1,true,false,0.1,2,1,4);
 check(meter.charge==0&&meter.exhausted,"empty boost meter latches exhaustion");
 meter=boostlogic::Step(0,0,true,true,0.1,2,1,4);check(!meter.active&&meter.charge==0,"holding empty boost cannot regenerate an infinite pulse");
 meter=boostlogic::Step(0,0,false,true,0.1,2,1,4);check(!meter.exhausted&&meter.charge==0,"release resets exhaustion but respects recharge delay");
 meter=boostlogic::Step(0,1,false,false,0.1,2,1,4);check(meter.charge>0&&!meter.active,"native meter recharges after delay");
 check(boostlogic::kMultiplier>1,"paid boost multiplies every selected gear");
 check(ramlogic::Owns(8,8,1000),"explicit ram retains its driver over ordinary parking/follow orders");
 check(!ramlogic::Owns(8,9,1000)&&!ramlogic::Owns(8,8,15000),"ram ownership excludes unrelated vehicles and expired orders");
 check(ramlogic::CanSelect(true,true,true,false),"highlighted living actor in combat can be rammed");
 check(!ramlogic::CanSelect(false,true,true,false),"ordinary destination selection outside combat remains navigation");
 check(!ramlogic::CanSelect(true,false,true,false)&&!ramlogic::CanSelect(true,true,true,true),"ram selection excludes places and own occupants");
 check(ramlogic::Finished(true,true,15000,false,100,4),"ram order has a finite timeout");
 check(ramlogic::Finished(true,true,1000,true,3,4),"ram ends after nearby impact");
 check(!ramlogic::Finished(true,true,1000,true,50,4),"distant contact cannot falsely complete ram");
 check(ramlogic::Finished(false,true,100,false,50,4),"lost ram target cancels order");
 return failures?1:0;
}
