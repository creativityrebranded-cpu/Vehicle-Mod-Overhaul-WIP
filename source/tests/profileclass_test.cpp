#include "vcontract.h"
#include "profileclass.h"
#include "coverfight.h"
#include <cstdio>
#include <string>
namespace logger { void Write(const char*, ...) {} }
namespace {
bool Goal(vcontract::Context&, vcontract::Goal& g) { g.x = 1; return true; }
bool Done(vcontract::Context& c, vcontract::Goal&) { c.done = true; return false; }
}
int main(){
 int failures=0;auto check=[&](bool ok,const char*message){std::printf("%s %s\n",ok?"PASS":"FAIL",message);if(!ok)++failures;};
 using vcontract::Class;
 const std::uint8_t npc=std::uint8_t(vcontract::User::Npc);
 const vcontract::Profile escort{"escort",npc,&Goal,false},patrol{"patrol",npc,&Goal,false},trip{"courier",npc,&Done,false};
 const vcontract::Profile attack{"attack",npc,&Goal,false,Class::Combat},defend{"defend",npc,nullptr,false,Class::Combat};
 const vcontract::Profile hold{"hold fire",npc,nullptr,false,Class::Weapons};
 const std::uint32_t id=0x01000042;
 vcontract::Assign(id,&patrol,0,0);vcontract::Assign(id,&defend,0,0);vcontract::Assign(id,&hold,0,0);
 check(vcontract::AssignedTo(id)==&patrol&&vcontract::AssignedTo(id,Class::Combat)==&defend&&vcontract::AssignedTo(id,Class::Weapons)==&hold,"a vehicle holds one profile per class");
 vcontract::Assign(id,&escort,0,0);
 check(vcontract::AssignedTo(id)==&escort&&vcontract::AssignedTo(id,Class::Combat)==&defend,"choosing navigation never replaces combat");
 vcontract::Assign(id,&attack,0,0);
 check(vcontract::AssignedTo(id)==&escort&&vcontract::AssignedTo(id,Class::Combat)==&attack,"choosing combat never replaces navigation");
 vcontract::Unassign(id,Class::Combat);
 check(vcontract::AssignedTo(id)==&escort&&!vcontract::AssignedTo(id,Class::Combat)&&vcontract::AssignedTo(id,Class::Weapons)==&hold,"ending a fight clears combat only");
 vcontract::Assign(id,nullptr,0,0);
 check(!vcontract::AssignedTo(id)&&vcontract::AssignedTo(id,Class::Weapons)==&hold,"clearing navigation leaves weapons alone");
 std::uint8_t fake[0x40]={};*reinterpret_cast<std::uint32_t*>(fake+0x28)=id;
 vcontract::Assign(id,&trip,0,0);vcontract::Assign(id,&attack,0,0);vcontract::Goal g;
 check(!vcontract::GoalFor(vcontract::User::Npc,1,std::uintptr_t(fake),0,g)&&!vcontract::AssignedTo(id)&&vcontract::AssignedTo(id,Class::Combat)==&attack,
  "a finished navigation profile leaves combat assigned");
 check(vcontract::GoalFor(vcontract::User::Npc,1,std::uintptr_t(fake),0,g,Class::Combat)&&g.x==1,"a combat profile with a goal drives a vehicle without navigation");
 vcontract::Assign(id,&defend,0,0);
 check(!vcontract::GoalFor(vcontract::User::Npc,1,std::uintptr_t(fake),0,g,Class::Combat),"a selector profile gives no goal of its own");
 vcontract::Unassign(id);
 check(!vcontract::AssignedTo(id)&&!vcontract::AssignedTo(id,Class::Combat)&&!vcontract::AssignedTo(id,Class::Weapons),"the job ending clears every class");
 using profileclass::Weapons;
 check(std::string(profileclass::DefaultCombat("escort"))=="defend"&&std::string(profileclass::DefaultCombat("scout"))=="defend"&&
  std::string(profileclass::DefaultCombat("hauler"))=="defend"&&std::string(profileclass::DefaultCombat("return home"))=="defend","escort, scout, hauler and return home defend in a fight");
 check(std::string(profileclass::DefaultCombat("patrol"))=="attack"&&std::string(profileclass::DefaultCombat("hold"))=="attack","patrol and hold attack in a fight");
 check(std::string(profileclass::DefaultCombat(nullptr))=="defend","a driver without navigation defends");
 check(!profileclass::MayFire(Weapons::Free,true)&&!profileclass::MayAim(Weapons::Free,true),"the player's seat never fires or aims by itself");
 check(profileclass::MayFire(Weapons::Opportunistic,false)&&!profileclass::MayAim(Weapons::Opportunistic,false),"opportunistic fires without turning for a shot");
 check(!profileclass::MayFire(Weapons::HoldFire,false)&&!profileclass::MayFire(Weapons::Never,false),"hold fire and never do not shoot");
 check(profileclass::Effective(Weapons::Free,true,false)==Weapons::HoldFire&&profileclass::Effective(Weapons::Free,false,true)==Weapons::Opportunistic,
  "recall holds fire and cover shoots opportunistically");
 check(profileclass::Effective(Weapons::Never,false,true)==Weapons::Never&&profileclass::Effective(Weapons::HoldFire,false,true)==Weapons::HoldFire,"overrides never relax a stricter weapons profile");
 check(profileclass::State(nullptr,"patrol",nullptr,false,Weapons::Free)=="PATROL","row without a fight shows navigation only");
 check(profileclass::State(nullptr,"patrol","attack",true,Weapons::Free)=="PATROL \xC2\xB7 ATTACK","row in a fight adds the combat profile");
 check(profileclass::State(nullptr,"hauler","defend",true,Weapons::HoldFire)=="HAULER \xC2\xB7 DEFEND \xC2\xB7 HOLD FIRE","restricting weapons appear in the row");
 check(profileclass::State(nullptr,"escort","defend",false,Weapons::Opportunistic)=="ESCORT","combat and non-restricting weapons stay hidden outside a fight");
 check(profileclass::State("COVER","escort","defend",true,Weapons::HoldFire)=="COVER","an active override shows alone");
 check(!profileclass::ShowsDistance("RAM READY")&&!profileclass::ShowsDistance("RAMMING")&&profileclass::ShowsDistance("SUMMON"),"ram rows carry no distance");
 check(!coverfight::Allowed(true,false,true,false)&&!coverfight::Allowed(false,true,true,false)&&!coverfight::Allowed(false,false,false,false),
  "own-vehicle combat never runs while you drive, with a crew driver, or without cover or guard");
 check(coverfight::Allowed(false,false,true,false)&&coverfight::Allowed(false,false,false,true),"empty vehicle fights under cover and on a guard point");
 check(coverfight::Fighting(true,1000,1000+coverfight::kOverMs-1)&&!coverfight::Fighting(true,1000,1000+coverfight::kOverMs)&&!coverfight::Fighting(false,1000,1000),
  "the fight ends a while after the last enemy");
 check(coverfight::OnTarget(0.4,0.5,50)&&!coverfight::OnTarget(0.5,0,50)&&!coverfight::OnTarget(0,0.7,50)&&!coverfight::OnTarget(0,0,120),"fires within the wider aim cone and weapon range");
 {double x=0,y=0;coverfight::ShieldPoint(0,0,0,100,x,y);check(x==0&&y==coverfight::kShield,"cover's defend spot sits between the player and the threat");
  coverfight::ShieldPoint(0,0,0,10,x,y);check(y==5,"a close threat keeps the spot halfway, never past the enemy");}
 {double x=0,y=0;coverfight::AttackPoint(0,0,0,100,0,0,x,y);check(std::fabs(y-(100-coverfight::kStandOff))<1e-9&&x==0,"attack stance closes to its stand-off on its own side");
  coverfight::AttackPoint(0,0,0,300,0,500,x,y);check(std::hypot(x,y)<=coverfight::kRange+1e-9,"attack stance never leaves the protected range");}
 {coverfight::Burst b;check(b.Update(true,2000),"a burst starts on target");check(b.Update(true,2000+coverfight::kBurstMs-1),"a burst continues while on target");
  check(!b.Update(true,2000+coverfight::kBurstMs),"a burst ends after its length");check(!b.Update(true,2000+coverfight::kBurstMs+coverfight::kGapMs-1),"bursts keep a gap");
  check(b.Update(true,2000+coverfight::kBurstMs+coverfight::kGapMs),"the next burst follows the gap");check(!b.Update(false,9000),"off target stops the weapon");}
 return failures?1:0;
}
