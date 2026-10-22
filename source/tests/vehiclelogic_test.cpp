#include "bodybinding.h"
#include "hoverlogic.h"
#include "healthlogic.h"
#include "targetsession.h"
#include "interactionlogic.h"
#include "travellogic.h"
#include "hulllogic.h"
#include "boostlogic.h"
#include "altcontrol.h"
#include "targetinput.h"
#include "ramlogic.h"
#include "overrides.h"
#include "keygesture.h"
#include <cstdio>
#include <string>
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
 const double lookingDown[]={0,std::cos(angle-0.6),std::sin(angle-0.6)};
 check(std::fabs(hoverlogic::RelativePitch(tilted,lookingDown)+0.6)<1e-7,"first-person look down remains negative through a pitched chassis");
 check(healthlogic::RepairSeconds(60000,59000,1000,15000,true)==0,"combat blocks repair even long after the last hit");
 check(healthlogic::RepairSeconds(74999,74000,60000,15000,false)==0,"leaving combat starts a fresh quiet interval");
 check(healthlogic::RepairSeconds(75500,74000,60000,15000,false)==.5,"cooldown crossing heals only time after its deadline");
 check(healthlogic::RepairSeconds(80000,79000,60000,15000,false)==1,"quiet vehicle repairs at the normal per-second rate");
 check(healthlogic::RepairSeconds(80000,79000,60000,30000,false)==0,"wreck keeps its longer quiet interval after ejection");
 check(healthlogic::RepairSeconds(100000,99000,60000,30000,false)==1,"wreck resumes slow repair after a full quiet interval");
 check(healthlogic::RepairSeconds(80000,0,60000,15000,false)==0,"new record cannot heal a whole session in one tick");
 check(healthlogic::RepairSeconds(78000,79000,60000,15000,false)==0,"clock rollback cannot create repair credit");
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
 check(ramlogic::CanSelect(true,true,false),"a T hold on a highlighted living actor rams it");
 check(!ramlogic::CanSelect(false,true,false)&&!ramlogic::CanSelect(true,true,true),"ram selection excludes places and own occupants");
 check(ramlogic::Reject(true,false,false)!=nullptr&&ramlogic::Reject(false,true,false)!=nullptr,"refused rams carry a logged reason");
 check(ramlogic::Throttle(0.2)==1.0&&ramlogic::Throttle(-1.0)==1.0,"a ram runs flat out toward a target ahead");
 check(ramlogic::Throttle(2.5)<1.0,"a target behind is turned onto first, then the run is flat out");
 check(ramlogic::SteerRate(1.0,0.8)==0.8&&ramlogic::SteerRate(-1.0,0.8)==-0.8,"the ram turns as fast as the hull allows");
 check(std::fabs(ramlogic::SteerRate(0.1,0.8)-0.25)<1e-9,"near the line the ram steers gently onto it");
 check(ramlogic::Finished(true,15000,false,100,4),"ram order has a finite timeout");
 check(ramlogic::Finished(true,1000,true,3,4),"ram ends after nearby impact");
 check(!ramlogic::Finished(true,1000,true,50,4),"distant contact cannot falsely complete ram");
 check(ramlogic::Finished(false,100,false,50,4),"lost ram target cancels order");
 check(!ramlogic::Finished(true,1000,false,50,4),"an explicit ram is not cut short by the player's combat flag");
 altcontrol::Input alt;
 alt.Update(false,true,0);
 auto action=alt.Update(true,true,10);
 check(!action.cycle&&!action.drop,"Alt press waits to distinguish tap from hold");
 action=alt.Update(false,true,110);
 check(action.cycle&&!action.drop,"Alt tap changes gear on release");
 check(!alt.Update(false,true,120).cycle,"Alt tap cannot repeat while released");
 alt.Update(true,true,200);
 action=alt.Update(true,true,500);
 check(action.drop&&!action.cycle,"Alt hold descends after 300 ms without changing gear");
 check(alt.Update(true,true,800).drop,"descent continues while Alt remains held");
 action=alt.Update(false,true,810);
 check(!action.drop&&!action.cycle,"releasing a held Alt ends descent without a gear change");
 alt.Update(true,true,900);
 action=alt.Update(false,true,1400);
 check(!action.cycle,"long press never changes gear even if an intervening frame was missed");
 alt.Update(true,true,1500);alt.Update(true,false,1510);
 action=alt.Update(false,true,1520);
 check(!action.cycle&&!action.drop,"focus loss cancels a pending Alt tap");
 alt.Update(true,false,1600);
 action=alt.Update(true,true,2000);
 check(!action.cycle&&!action.drop,"Alt held while boarding cannot trigger a command");
 alt.Update(false,true,2010);alt.Update(true,true,2020);
 check(alt.Update(false,true,2100).cycle,"fresh Alt press works after boarding or focus returns");
 check(altcontrol::NextGear(1)==2&&altcontrol::NextGear(2)==3&&altcontrol::NextGear(3)==4&&altcontrol::NextGear(4)==1,"Alt cycles all four gears and wraps to first");
 targetinput::Input targeting; targeting.Update(false,true,0);
 auto targetAction=targeting.Update(true,true,10);
 check(targetAction.capture&&!targetAction.tap&&!targetAction.hold,"T press captures the aimed selection without acting");
 targetAction=targeting.Update(false,true,110);
 check(!targetAction.tap&&!targetAction.doubleTap,"single tap waits out the double-tap window before selecting");
 check(!targeting.Update(false,true,300).tap,"tap is not reported inside the double-tap window");
 targetAction=targeting.Update(false,true,410);
 check(targetAction.tap&&!targetAction.doubleTap&&!targetAction.hold,"single tap selects once the window has passed");
 check(!targeting.Update(false,true,500).tap,"released T cannot repeatedly select targets");
 targeting.Update(true,true,1000);targeting.Update(false,true,1080);
 targetAction=targeting.Update(true,true,1200);
 check(targetAction.doubleTap&&!targetAction.tap&&!targetAction.capture,"second press inside the window clears the route and keeps the first capture");
 targetAction=targeting.Update(false,true,1260);
 check(!targetAction.tap&&!targeting.Update(false,true,2000).tap,"a double tap never also selects a destination or ram");
 targeting.Update(true,true,3000);targetAction=targeting.Update(true,true,3900);
 check(!targetAction.hold,"T hold waits a full second");
 targetAction=targeting.Update(true,true,4010);
 check(targetAction.hold&&!targetAction.tap,"holding T arms the ram instead of selecting");
 check(!targeting.Update(true,true,4300).hold,"T hold fires once per press");
 targetAction=targeting.Update(false,true,4310);
 check(!targetAction.tap&&!targetAction.hold&&!targeting.Update(false,true,4800).tap,"releasing a hold cannot select or ram");
 targeting.Update(true,true,5000);targetAction=targeting.Update(false,true,6100);
 check(targetAction.hold&&!targetAction.tap&&!targeting.Update(false,true,6600).tap,"missed hold frames still count as a hold, never a selection");
 targetinput::Input almost; almost.Update(false,true,0); almost.Update(true,true,10);
 check(!almost.Update(false,true,960).hold&&almost.Update(false,true,1300).tap,"a 950 ms press is still a tap, not a hold");
 targeting.Update(true,true,7000);targeting.Update(false,true,7050);targeting.Update(false,false,7100);
 check(!targeting.Update(false,true,7500).tap,"loss of input context cancels a pending tap");
 targeting.Update(true,true,8000);targeting.Update(false,true,8050);targeting.Cancel(false);
 check(!targeting.Update(false,true,8500).tap,"scanner or session change cancels a pending tap");
 targeting.Update(true,false,9000);
 check(!targeting.Update(true,true,9100).capture&&!targeting.Update(true,true,9700).hold,"T already held when input becomes usable needs a fresh press");
 targeting.Update(false,true,9800);targeting.Update(true,true,9900);targeting.Update(false,true,9950);
 targetAction=targeting.Update(true,true,10400);
 check(targetAction.tap&&targetAction.capture&&!targetAction.doubleTap,"a late second press after a frame gap delivers the first tap and starts a new one");
 using overrides::Mode;
 check(overrides::OnSummon(Mode::Cover)==Mode::Recall&&overrides::OnSummon(Mode::Guard)==Mode::Recall&&overrides::OnSummon(Mode::Recall)==Mode::Recall,"N always recalls and never toggles off");
 {const auto r=overrides::For(Mode::Recall);check(!r.pickFoe&&!r.fire&&!r.chase&&r.followPlayer,"recall drops the fight and holds fire");}
 {const auto r=overrides::For(Mode::Cover);check(r.pickFoe&&r.fire&&!r.chase&&r.followPlayer,"cover defends the player without chasing");}
 {const auto r=overrides::For(Mode::Guard);check(r.holdPoint&&!r.chase&&r.fire,"guard holds its point and defends from it");}
 {const auto r=overrides::For(Mode::None);check(r.pickFoe&&r.chase&&r.fire,"without an override the vehicle's own combat behaviour runs");}
 check(overrides::OnCover(Mode::None)==Mode::Cover&&overrides::OnCover(Mode::Cover)==Mode::None&&overrides::OnCover(Mode::Guard)==Mode::Cover,"U toggles cover and resumes it from a guard point");
 keygesture::Input key; key.Update(false,true,0);
 key.Update(true,true,10);auto g=key.Update(false,true,200);
 check(g.tap&&!g.hold,"N/U tap acts on release");
 key.Update(true,true,1000);check(!key.Update(true,true,1900).hold,"N/U hold waits a full second");
 g=key.Update(true,true,2010);check(g.hold&&!g.tap,"N/U hold acts once at one second");
 check(!key.Update(true,true,2500).hold,"N/U hold fires once per press");
 g=key.Update(false,true,2600);check(!g.tap&&!g.hold,"releasing a hold never also taps");
 key.Update(true,true,3000);g=key.Update(false,true,4200);check(g.hold&&!g.tap,"a missed frame still counts as the hold");
 key.Update(true,false,5000);g=key.Update(true,true,5100);check(!g.tap&&!g.hold&&!key.Update(false,true,5200).tap,"a key held while unusable needs a fresh press");
 check(std::string(overrides::SummonLabel())=="Summon","N is always Summon, never Recall");
 check(std::string(overrides::CoverLabel(false))=="Follow"&&std::string(overrides::CoverLabel(true))=="Cover","U is Follow out of combat, Cover in combat");
 check(std::string(overrides::CoverStatus(false))=="FOLLOW"&&std::string(overrides::CoverStatus(true))=="COVER","U's status follows the combat state");
 return failures?1:0;
}
