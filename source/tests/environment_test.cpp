#include "environment.h"
#include "navmap.h"
#include "vehicleworld.h"
#include "prefetch.h"
#include "navfield.h"
#include "addresses.h"
#include "game.h"
#include <Windows.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <algorithm>
namespace logger {void Write(const char*,...){}}
namespace {int mode=0;std::array<prefetch::RayHit,prefetch::kRaySlots> hits;std::array<bool,prefetch::kRaySlots> have{};
bool refusing=false;float refuseAtX=1e9f;std::array<std::array<float,3>,prefetch::kRaySlots> asked{};std::array<bool,prefetch::kRaySlots> refusedSlots{};}
namespace game {std::uintptr_t Base(){return 0;}}
namespace cellbake {bool Ground(double,double,double&){return false;}}
namespace navmap {Sample At(double,double,double){Sample s;s.known=true;return s;}void NoteGround(double,double,double){}void Column(double x,double y,const double* zs,int n,Sample* out){for(int k=0;k<n;++k)out[k]=At(x,y,zs[k]);}}
namespace world {
bool Vehicles(Snapshot& s){s={};s.count=1;s.v[0].formId=1;s.v[0].pos[2]=2.5;s.v[0].half[0]=2;s.v[0].half[1]=3;s.v[0].half[2]=1.2f;return true;}
}
namespace prefetch {
void RequestRay(int slot,const float from[3],const float to[3],std::uint64_t){
 asked[slot]={from[0],from[1],to[0]};
 if(refusing&&from[0]>=refuseAtX){refusedSlots[slot]=true;have[slot]=false;return;}  // no loaded world there: the engine gives no answer
 refusedSlots[slot]=false;
 auto& h=hits[slot];h={};std::copy(from,from+3,h.from);std::copy(to,to+3,h.to);have[slot]=true;
 const float dz=to[2]-from[2];
 if(dz<0 && from[2]>=0 && to[2]<=0 && !(mode==1&&from[0]>2)) {
  const float t=-from[2]/dz;h.hit=true;h.x=from[0]+t*(to[0]-from[0]);h.y=from[1]+t*(to[1]-from[1]);h.z=0;h.nz=1;h.normalOk=true;h.layer=mode==2?11:17;
 }
 if(mode==3 && dz>0 && from[2]<3 && to[2]>=3){h.hit=true;h.z=3;h.formId=50;}
 if(mode==4 && std::fabs(dz)<0.01 && to[0]>from[0]+10){h.hit=true;h.distance=4;h.formId=77;}
 if(mode==5 && std::fabs(dz)<0.01 && to[0]>from[0]+10){h.hit=true;h.distance=19;h.formId=2;}
 if(mode==6 && dz>0 && std::fabs(from[0])>3){h.hit=true;h.formId=55;}
 if(mode==7 && dz>0 && std::fabs(from[0])<3){h.hit=true;h.formId=from[2]<3.8f?1:50;}
 if(mode==8 && std::fabs(dz)<0.01 && to[0]>from[0]+10){h.hit=true;h.distance=4;h.formId=0;}
}
bool LatestRay(int slot,RayHit&h,unsigned&age){h=hits[slot];age=0;return have[slot];}
bool RayRefused(int slot,const float from[3],const float to[3]){return refusing&&asked[slot][0]==from[0]&&asked[slot][1]==from[1]&&asked[slot][2]==to[0]&&refusedSlots[slot];}
}
int main(){
 int failures=0;auto check=[&](bool ok,const char*s){std::printf("%s %s\n",ok?"PASS":"FAIL",s);if(!ok)++failures;};
 auto site=[](){environment::Site out;for(int n=0;n<100;++n){out=environment::CheckSite(1,0,0,2.5,0);environment::Tick();if(out.verdict!=environment::Verdict::Pending)break;}return out;};
 auto reset=[](int next){mode=next;environment::Reset();have={};};
 reset(0);check(site().verdict==environment::Verdict::Safe,"flat ground with boarding space is safe");
 reset(1);check(site().verdict==environment::Verdict::Unsafe,"platform edge rejects unsupported footprint");
 reset(2);check(site().verdict==environment::Verdict::Unsafe,"water rejects parking and drop-off");
 reset(3);check(site().verdict==environment::Verdict::Unsafe,"low ceiling rejects vehicle clearance");
 reset(6);check(site().verdict==environment::Verdict::Unsafe,"blocked sides reject boarding and exit");
 reset(7);check(site().verdict==environment::Verdict::Unsafe,"own hull cannot conceal a ceiling above it");
 reset(0);check(site().supported,"zero-height ground retains explicit support proof");
 const float from[]={0,0,2},to[]={20,0,2};
 auto shot=[&](){bool clear=false;for(int n=0;n<10;++n){clear=environment::ClearShot(1,2,2,from,to);environment::Tick();}return clear;};
 reset(0);check(!environment::ClearShot(1,2,2,from,to),"uncast line of fire is not assumed clear");check(shot(),"fresh clear ray permits firing");
 reset(4);check(!shot(),"wall or unrelated actor blocks firing");
 reset(5);check(shot(),"hit on intended target permits firing");
 reset(8);bool unknownClear=false;for(int n=0;n<10;++n){unknownClear=environment::ClearShot(1,0,0,from,to);environment::Tick();}
 check(!unknownClear,"unnamed obstacle is not mistaken for an unspecified target");
 reset(0);const auto pending=environment::CheckSite(1,0,0,2.5,0);check(pending.verdict==environment::Verdict::Pending,"fresh site awaits actual probe results");
 environment::Reset();check(environment::CheckSite(1,0,0,2.5,0).verdict==environment::Verdict::Pending,"load reset discards site proof");
 // Moving vehicles leave queries nobody asks for again (2026-10-31 18:59 run: 213-219 of 256 pending). Those must not hold the table for 2 s.
 reset(0);for(int i=0;i<400;++i){const float a[]={float(i),50,2},b[]={float(i),70,2};environment::ClearShot(100+i,0,0,a,b);}
 environment::Tick();Sleep(150);environment::Tick();
 check(site().verdict==environment::Verdict::Safe,"dead queries from moving vehicles leave room for new ones");
 // A query still being asked for keeps its place, however long the queue takes.
 reset(0);const float k1[]={0,50,2},k2[]={0,70,2};bool answered=false;
 for(int n=0;n<4&&!answered;++n){answered=environment::ClearShot(1,0,0,k1,k2);Sleep(60);environment::Tick();}
 check(environment::ClearShot(1,0,0,k1,k2)||answered,"a query still asked for is answered");
 // Rays from where no world is loaded get no answer. They must not hold every slot until their timeout (2026-10-31: about 20 starts a second against 900 asked).
 reset(0);refusing=true;refuseAtX=5000;
 for(int i=0;i<30;++i){const float a[]={float(5000+i),50,2},b[]={float(5000+i),70,2};environment::ClearShot(200+i,0,0,a,b);}
 environment::Tick();environment::Tick();
 const float near1[]={0,60,2},near2[]={0,80,2};bool nearAnswered=false;
 for(int n=0;n<6&&!nearAnswered;++n){nearAnswered=environment::ClearShot(7,0,0,near1,near2);environment::Tick();}
 check(nearAnswered,"refused rays free their slots at once: a good query is answered in a few updates");
 const float far1[]={5000,50,2},far2[]={5000,70,2};bool farAnswered=false;
 for(int n=0;n<4;++n){farAnswered=environment::ClearShot(200,0,0,far1,far2)||farAnswered;environment::Tick();}
 check(!farAnswered,"a refused query is not reported as an answer");
 refusing=false;refusedSlots={};
 return failures?1:0;
}
