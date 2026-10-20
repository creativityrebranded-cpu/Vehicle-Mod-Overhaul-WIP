#include "livetest.h"
#include "livetestcatalog.h"
#include "addresses.h"
#include "controlmap.h"
#include "environment.h"
#include "game.h"
#include "hoverui.h"
#include "log.h"
#include "npcvehicles.h"
#include "notify.h"
#include "vehicleflight.h"
#include "vehiclehits.h"
#include "vehicleworld.h"
#include "vcontract.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
namespace livetest {
namespace {
Session session;
int testKey=-1,cycleKey=-1;
bool testDown=false,cycleDown=false,wasFocused=false,startedAuto=false,changedMode=false;
std::uint32_t subject=0;
vehicleflight::AutoMode oldMode=vehicleflight::AutoMode::Stop,testMode=vehicleflight::AutoMode::Stop;
unsigned long long sampled=0,uiAt=0,lastFrame=0;
double frameTotal=0,frameMax=0;unsigned frames=0;
std::string result="Ready: prepare the location, then press Test.";
const Case& Selected(){return Cases[session.selected];}
bool Focused(){DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);return pid==GetCurrentProcessId();}
world::VehicleInfo Pick(const world::Snapshot&s){
 const auto own=vehicleflight::OwnRoverId();const bool npc=(Selected().need&Npc)!=0||std::strcmp(Selected().id,"civil-transit")==0;
 if(session.running&&subject){for(unsigned i=0;i<s.count&&i<64;++i)if(s.v[i].formId==subject)return s.v[i];return {};}
 if(std::strcmp(Selected().id,"profiles")==0)for(unsigned i=0;i<s.count&&i<64;++i)
  if(s.v[i].who==world::VehicleInfo::Who::Npc&&vcontract::AssignedTo(s.v[i].formId))return s.v[i];
 for(unsigned i=0;i<s.count&&i<64;++i)if(npc?s.v[i].who==world::VehicleInfo::Who::Npc:s.v[i].formId==own)return s.v[i];
 return {};
}
std::string Ready(const world::Snapshot&s,const world::VehicleInfo&v){
 Setup setup;setup.own=v.formId&&v.formId==vehicleflight::OwnRoverId();setup.npc=v.formId&&v.who==world::VehicleInfo::Who::Npc;
 setup.driver=setup.own&&v.who==world::VehicleInfo::Who::Player;
 double x,y;setup.marker=v.ref&&vehicleflight::MarkerPoint(v.ref,x,y);
 setup.disabled=setup.own&&vehicleflight::AutopilotDisabled(v.formId);
 for(unsigned i=0;i<s.count&&i<64;++i)if(s.v[i].who==world::VehicleInfo::Who::Npc||s.v[i].who==world::VehicleInfo::Who::Player)++setup.activeVehicles;
 return Missing(Selected(),setup);
}
void Finish(const char*reason,bool stopAction=true){
 if(!session.running)return;
 logger::Write("LiveTest END id=%s subject=%08X duration=%.1fs samples=%u outcome=REVIEW reason=%s",Selected().id,subject,
  (GetTickCount64()-session.started)/1000.0,session.samples,reason);
 session.Stop();
 if(stopAction&&startedAuto)vehicleflight::CancelScannerTrip();
 if(changedMode&&vehicleflight::GetAutoMode()==testMode)vehicleflight::SetAutoMode(oldMode);
 startedAuto=changedMode=false;
 result="Recorded for review. Test repeats; Test Cycle selects the next case.";
}
void Begin(const world::Snapshot&s,const world::VehicleInfo&v){
 const auto missing=Ready(s,v);if(!missing.empty()){result="Setup needed: "+missing;notify::Show("Live test: %s",missing.c_str());return;}
 if(!session.Start(GetTickCount64(),true))return;
 subject=v.formId;sampled=0;frameTotal=frameMax=0;frames=0;
 const auto action=Selected().action;
 oldMode=vehicleflight::GetAutoMode();testMode=oldMode;
 if(action==Action::Auto||action==Action::Orbit||action==Action::Patrol){
  testMode=action==Action::Orbit?vehicleflight::AutoMode::Orbit:action==Action::Patrol?vehicleflight::AutoMode::Patrol:vehicleflight::AutoMode::Stop;
  changedMode=testMode!=oldMode;vehicleflight::SetAutoMode(testMode);
  vehicleflight::RequestScannerTrip();startedAuto=true;
 }else if(action==Action::Recall)npcvehicles::Recall(15000);
 logger::Write("LiveTest BEGIN index=%zu/%zu id=%s subject=%08X place=%s setup=%s steps=%s expected=%s",session.selected+1,Count,
  Selected().id,subject,Selected().place,Selected().setup,Selected().steps,Selected().expect);
 result="Recording observations; press Test to finish. Visual review still required.";
 notify::Show("Test %zu/%zu: %s",session.selected+1,Count,Selected().name);
}
void Sample(const world::Snapshot&s,const world::VehicleInfo&v,const game::PlanetState&p,unsigned long long now){
 if(now-sampled<1000)return;sampled=now;++session.samples;
 const auto progress=vehicleflight::ProgressFor(subject);
 const auto site=v.ref?environment::BoardingSite(v.ref):environment::Site{};
 float damage=0,limit=0;vehiclehits::DamageOf(subject,damage,limit);
 logger::Write("LiveTest SAMPLE id=%s t=%.1f subject=%08X loaded=%d player=(%.1f,%.1f,%.1f) vehicle=(%.1f,%.1f,%.1f) speed=%.2f phase=%s active=%d distance=%.1f heightError=%.1f site=%d reason=%s vehicles=%u civil=%d damage=%.1f/%.1f frameMean=%.2fms frameMax=%.2fms",
  Selected().id,(now-session.started)/1000.0,subject,v.formId?1:0,p.playerPos.x,p.playerPos.y,p.playerPos.z,v.pos[0],v.pos[1],v.pos[2],
  std::hypot(std::hypot(v.vel[0],v.vel[1]),v.vel[2]),triplogic::Text(progress.phase),progress.active?1:0,progress.distance,progress.heightError,
  int(site.verdict),site.reason,s.count,npcvehicles::CivilJobs(),damage,limit,frames?frameTotal/frames:0,frameMax);
 for(unsigned i=0;i<s.count&&i<64;++i){const auto&o=s.v[i];if(o.who!=world::VehicleInfo::Who::Npc&&o.who!=world::VehicleInfo::Who::Player)continue;
  const auto trip=vehicleflight::ProgressFor(o.formId);const auto profile=vcontract::AssignedTo(o.formId);
  logger::Write("LiveTest VEHICLE id=%s vehicle=%08X pilot=%d profile=%s pos=(%.1f,%.1f,%.1f) velocity=(%.1f,%.1f,%.1f) phase=%s goal=(%.1f,%.1f) distance=%.1f",
   Selected().id,o.formId,npcvehicles::PilotFor(o.formId),profile?profile->name:"default",o.pos[0],o.pos[1],o.pos[2],o.vel[0],o.vel[1],o.vel[2],
   triplogic::Text(trip.phase),trip.x,trip.y,trip.distance);
 }
 frameTotal=frameMax=0;frames=0;
}
}
void Tick(){
 const auto now=GetTickCount64();const auto planet=game::ReadPlanetState();
 const bool live=planet.havePlayer&&planet.haveManager&&planet.managerState>=2&&planet.managerState<=4;
 const bool focused=Focused();
 if(!live){if(session.running)Finish("world unavailable",false);hoverui::SetTest("","","",false,false);lastFrame=0;wasFocused=false;return;}
 if(testKey<0)testKey=controlmap::ClaimFirst("livetest","Test",{VK_F6,VK_F8,VK_F10,VK_F11,VK_F12,VK_OEM_2});
 if(cycleKey<0)cycleKey=controlmap::ClaimFirst("livetest","Test Cycle",{VK_F8,VK_F10,VK_F11,VK_F12,VK_OEM_7,VK_OEM_2});
 world::Snapshot snapshot{};world::Vehicles(snapshot);auto vehicle=Pick(snapshot);
 if(session.running&&!subject&&vehicle.formId){subject=vehicle.formId;logger::Write("LiveTest SUBJECT id=%s vehicle=%08X",Selected().id,subject);}
 const bool test=focused&&testKey>0&&(GetAsyncKeyState(testKey)&0x8000)!=0;
 const bool cycle=focused&&cycleKey>0&&(GetAsyncKeyState(cycleKey)&0x8000)!=0;
 if(focused&&wasFocused&&cycle&&!cycleDown){Finish("selected another test");if(GetAsyncKeyState(VK_SHIFT)&0x8000)session.Previous();else session.Next();
  vehicle=Pick(snapshot);result="Ready: prepare the location, then press Test.";
  notify::Show("Test %zu/%zu: %s",session.selected+1,Count,Selected().name);uiAt=0;
 }else if(focused&&wasFocused&&test&&!testDown){if(session.running)Finish("user finished recording");else Begin(snapshot,vehicle);uiAt=0;}
 testDown=test;cycleDown=cycle;wasFocused=focused;
 if(session.running){if(lastFrame&&focused){const double ms=double(now-lastFrame);frameTotal+=ms;frameMax=std::max(frameMax,ms);++frames;}
  Sample(snapshot,vehicle,planet,now);}
 lastFrame=now;
 if(now-uiAt<200)return;uiAt=now;
 const auto&c=Selected();const auto progress=vehicleflight::ProgressFor(subject);
 char heading[200];std::snprintf(heading,sizeof(heading),"LIVE TEST %zu/%zu  %s\n%s",session.selected+1,Count,c.name,session.running?"RECORDING":"SETUP / REVIEW");
 std::string text=std::string(heading)+"\nWhere: "+c.place+"\nSetup: "+c.setup+"\nDo: "+c.steps+"\nExpect: "+c.expect;
 const auto missing=Ready(snapshot,vehicle);
 text+="\n"+(session.running?std::string(triplogic::Text(progress.phase))+" | "+result:missing.empty()?result:"Setup needed: "+missing);
 text+="\nTest: start/finish recording. Test Cycle: next (Shift: previous).";
 hoverui::SetTest(testKey>0?controlmap::KeyName(testKey).c_str():"unbound",cycleKey>0?controlmap::KeyName(cycleKey).c_str():"unbound",text.c_str(),session.running,true);
}
void Reset(){Finish("load/world reset",false);session=Session{};subject=0;testDown=cycleDown=wasFocused=false;sampled=uiAt=lastFrame=0;
 result="Ready: prepare the location, then press Test.";hoverui::SetTest("","","",false,false);}
}
