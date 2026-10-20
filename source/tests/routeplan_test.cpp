#include "routeplan.h"
#include "vehicleworld.h"
#include "navmap.h"
#include <cstdarg>
#include <cstdio>
#include <cmath>
namespace {int mode=0;}
namespace logger {void Write(const char*,...) {}}
namespace world {
bool Vehicles(Snapshot& s){s={};s.count=1;s.v[0].formId=2;s.v[0].pos[1]=40;s.v[0].pos[2]=10;
 s.v[0].vel[1]=-10;s.v[0].half[0]=1.25f;s.v[0].half[1]=2.3f;s.v[0].half[2]=1;
 s.v[0].who=VehicleInfo::Who::Player;return true;}
}
namespace navmap {
Sample At(double,double,double z){Sample s;s.known=mode!=3;
 s.objBlocked=mode==2||(mode==1&&z>=13&&z<=15);return s;}
}
int main(){
 int failures=0;auto check=[&](bool ok,const char*name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
 routeplan::Query q;q.selfId=1;q.pos[2]=10;q.dir[1]=1;q.speed=10;q.distToGoal=100;
 auto plan=[&](int next){mode=next;routeplan::Reset();routeplan::Frame();return routeplan::Update(1,q);};
 auto open=plan(0);check(open.laneZ>0,"open sky permits a lane to avoid oncoming traffic");
 auto roof=plan(1);check(roof.laneZ==0,"thin overhead deck forbids an avoidance climb through it");
 auto blocked=plan(2);check(blocked.laneZ==0&&blocked.yawOffset==0&&blocked.speedCap>=0,"boxed traffic brakes when every environmental detour is blocked");
 auto unknown=plan(3);check(unknown.laneZ==0&&unknown.yawOffset==0&&unknown.speedCap>=0,"unknown space cannot authorize a traffic lane change");
 mode=0;routeplan::Reset();routeplan::Frame();routeplan::Update(1,q);mode=2;
 auto changed=routeplan::Update(1,q);check(changed.laneZ==0&&changed.yawOffset==0&&changed.speedCap==0,"new obstruction cancels an already held traffic lane");
 q.ignoreId=2;auto ram=plan(0);check(ram.laneZ==0&&ram.yawOffset==0&&ram.speedCap<0,"explicit ram target does not trigger an avoidance lane");
 q.ignoreId=99;auto other=plan(0);check(other.laneZ>0,"ram exemption never disables avoidance of unrelated vehicles");
 return failures?1:0;
}
