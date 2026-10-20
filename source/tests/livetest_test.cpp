#include "livetestcatalog.h"
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
int main(){
 using namespace livetest;int failures=0;auto check=[&](bool ok,const char*name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
 std::set<std::string> ids;bool complete=true,guarded=true;
 for(const auto&c:Cases){ids.insert(c.id);complete=complete&&c.id[0]&&c.name[0]&&c.place[0]&&c.setup[0]&&c.steps[0]&&c.expect[0];
  if(c.action==Action::Auto||c.action==Action::Orbit||c.action==Action::Patrol)guarded=guarded&&((c.need&(Own|Marker|Driver))==(Own|Marker|Driver));}
 check(ids.size()==Count,"every live scenario has a unique log identity");
 check(complete,"every scenario supplies location setup actions and expected observations");
 check(guarded,"every automated route requires the owned rover driver and marker");
 const Case*route=nullptr,*fleet=nullptr;for(const auto&c:Cases){if(c.action==Action::Auto)route=&c;if(c.need&Three)fleet=&c;}
 Setup setup;check(Missing(*route,setup)[0]!=0,"a missing owned rover blocks automatic test start");
 setup.own=true;check(Missing(*route,setup)[0]!=0,"automatic test cannot start outside the driver seat");
 setup.driver=true;check(Missing(*route,setup)[0]!=0,"automatic test cannot start without a marker");
 setup.marker=true;check(Missing(*route,setup)[0]==0,"prepared route can start through normal Auto");
 setup.disabled=true;check(Missing(*route,setup)[0]!=0,"autopilot damage cooldown blocks route launch");
 setup={};setup.npc=true;setup.activeVehicles=2;check(Missing(*fleet,setup)[0]!=0,"fleet load case requires three active vehicles");
 setup.activeVehicles=3;check(Missing(*fleet,setup)[0]==0,"fleet load case accepts three real active vehicles");
 Session s;check(!s.Start(100,false)&&!s.running,"missing setup cannot create an active recording");
 check(s.Start(200,true)&&s.started==200,"valid run starts a timestamped recording");
 check(!s.Start(300,true)&&s.started==200,"a second start cannot overwrite an active run");
 s.Next();check(!s.running&&s.selected==1,"cycling ends recording before selecting a different scenario");
 s.selected=Count-1;s.Next();check(s.selected==0,"cycling past the final case wraps to the first");
 s.Previous();check(s.selected==Count-1,"reverse cycling wraps to the final case");
 std::printf("CATALOG %zu live cases\n",Count);return failures?1:0;
}
