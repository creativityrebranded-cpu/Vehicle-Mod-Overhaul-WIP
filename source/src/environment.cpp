#include "environment.h"
#include "log.h"
#include "cellbake.h"
#include "sitelogic.h"
#include "spawnfloor.h"
#include "addresses.h"
#include "game.h"
#include "prefetch.h"
#include "navmap.h"
#include "navfield.h"
#include "vehicleworld.h"
#include "camera_visibility.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <limits>
namespace environment {
namespace {
constexpr int E=prefetch::kRayEnvironment0;
constexpr int Slots[]={22,23,45,46,47,E,E+1,E+2,E+3,E+4,E+5,E+6,E+7,E+8,E+9,E+10};
constexpr int kFlights=int(std::size(Slots));
static_assert(kFlights==5+prefetch::kRayEnvironmentCount);
constexpr std::uint64_t Solid=0xA5C12671Eull;
constexpr unsigned long long kStartWindowMs=100;
constexpr std::size_t kMaxQueries=512;
struct Query {std::uint64_t key=0,mask=0;float from[3]={},to[3]={};prefetch::RayHit hit;unsigned long long asked=0,done=0,refused=0;bool inFlight=false;};
std::vector<Query> queries;
struct Flight {std::uint64_t key=0;float from[3]={},to[3]={};unsigned long long at=0;};
std::array<Flight,kFlights> flights;
std::uint64_t Key(std::uint32_t owner,const float from[3],const float to[3],std::uint64_t mask,bool camera=false){
 std::uint64_t h=owner^mask;
 if(camera)h^=0xA3D139BA9C44BFD7ull;
 for(const auto*p:{from,to})for(int i=0;i<3;++i){const auto v=std::int64_t(std::floor(p[i]*(camera?32:2)));h^=std::uint64_t(v)+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);}
 return h?h:1;
}
bool Same(const float a[3],const float b[3]){for(int i=0;i<3;++i)if(std::fabs(a[i]-b[i])>0.1)return false;return true;}
bool Ray(std::uint32_t owner,const float from[3],const float to[3],std::uint64_t mask,prefetch::RayHit&out,unsigned ttl=1000,bool camera=false){
 const auto now=GetTickCount64(),key=Key(owner,from,to,mask,camera);
 for(auto&q:queries)if(q.key==key){q.asked=now;if(q.done&&now-q.done<ttl){out=q.hit;
  logger::Observe("sensing.ray",owner,1u+unsigned(out.hit),"reason=cached_result query=%llX hit=%d age_ms=%llu layer=%u body=%08X hit_pos=%.2f,%.2f,%.2f",static_cast<unsigned long long>(key),out.hit,static_cast<unsigned long long>(now-q.done),out.layer,out.bodyId,out.x,out.y,out.z);return true;}
  logger::Observe("sensing.ray",owner,0,"reason=waiting query=%llX in_flight=%d from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f",static_cast<unsigned long long>(key),q.inFlight,from[0],from[1],from[2],to[0],to[1],to[2]);return false;}
 if(queries.size()>=kMaxQueries){
  auto oldest=queries.end();
  for(auto it=queries.begin();it!=queries.end();++it)
   if(!it->inFlight&&now-it->asked>=kStartWindowMs&&(oldest==queries.end()||it->asked<oldest->asked))oldest=it;
  if(oldest==queries.end()){
   for(auto it=queries.begin();it!=queries.end();++it)
    if(!it->inFlight&&it->done&&(oldest==queries.end()||it->done<oldest->done))oldest=it;
  }
  if(oldest==queries.end()){
   logger::Observe("sensing.ray",owner,3,"reason=query_cache_full cached=%zu query=%llX",queries.size(),static_cast<unsigned long long>(key));
   return false;
  }
  logger::Observe("sensing.ray",owner,4,"reason=replaced_inactive_query cached=%zu idle_ms=%llu",queries.size(),now-oldest->asked);
  queries.erase(oldest);
 }
 Query q;q.key=key;q.mask=mask;q.asked=now;std::copy(from,from+3,q.from);std::copy(to,to+3,q.to);queries.push_back(q);return false;
}
world::VehicleInfo Vehicle(std::uint32_t id){
 static thread_local world::Snapshot s;if(world::VehiclesAndHeld(s))for(unsigned i=0;i<s.count&&i<world::kMaxVehicles;++i)if(s.v[i].formId==id)return s.v[i];
 world::VehicleInfo result;
 __try {
  const auto ref=reinterpret_cast<std::uintptr_t(*)(std::uint32_t)>(game::Base()+0x005DE3B0)(id);
  if(!ref||*reinterpret_cast<std::uint32_t*>(ref+off::kFormId)!=id)return result;
  const auto base=*reinterpret_cast<std::uintptr_t*>(ref+0x98);if(!base)return result;
  result.formId=id;result.ref=ref;const auto p=*reinterpret_cast<game::Vec3*>(ref+off::kRefPosition);
  result.pos[0]=p.x;result.pos[1]=p.y;result.pos[2]=p.z;result.heading=reinterpret_cast<game::Vec3*>(ref+off::kRefRotation)->z;
  // TESBoundObject bounds in 1.16.244: min xyz at +0x50, max xyz at +0x5C (2026-10-25; +0x48/+0x54 read 8 bytes early).
  for(int i=0;i<3;++i){const double half=0.5*(*reinterpret_cast<float*>(base+0x5C+i*4)-*reinterpret_cast<float*>(base+0x50+i*4));
   result.half[i]=float(std::isfinite(half)&&half>=0.25&&half<=10?half:(i==1?5:3));}
 }__except(EXCEPTION_EXECUTE_HANDLER){return {};}
 return result;
}

bool MapBlocked(const float from[3],const float to[3]){
 const double length=std::hypot(std::hypot(to[0]-from[0],to[1]-from[1]),to[2]-from[2]);
 const int n=std::max(1,int(std::ceil(length/2)));
 for(int i=1;i<n;++i){const double t=double(i)/n;const auto m=navmap::At(from[0]+(to[0]-from[0])*t,from[1]+(to[1]-from[1])*t,from[2]+(to[2]-from[2])*t);if(m.blocked||m.objBlocked||(m.haveGround&&m.ground>from[2]+(to[2]-from[2])*t))return true;}
 return false;
}
}
bool LineBlocked(const float from[3],const float to[3]){return MapBlocked(from,to);}
// Downward rays along the way (2026-10-21, hover or drive): water (collision layer 11) or rough ground (a steep surface,
// or a big step between neighbouring points) within `reach` metres. Points snap to a 10 m grid so a moving vehicle keeps
// asking the same rays (cached 3 s); the answer fills in over a few frames.
Surface SurfaceAhead(std::uint32_t self,double x,double y,double z,double bearing,double reach){
 Surface out;
 static constexpr double kAt[]={20,45,80,130,190,250};
 double lastZ=0,lastD=0;bool haveLast=false;int known=0;
 for(const double d:kAt){
  if(d>reach+10)break;
  const double px=std::round((x+std::sin(bearing)*d)/10)*10,py=std::round((y+std::cos(bearing)*d)/10)*10,pz=std::round(z/20)*20;
  const float from[]={float(px),float(py),float(pz+80)},to[]={float(px),float(py),float(pz-200)};
  prefetch::RayHit hit;
  if(!Ray(self,from,to,0x22806,hit,3000)||!hit.hit||!std::isfinite(hit.z)){haveLast=false;continue;}
  ++known;
  if(hit.layer==11){out.water=true;if(!out.at)out.at=d;}
  if((hit.normalOk&&hit.nz<0.75f)||(haveLast&&std::fabs(hit.z-lastZ)>0.6*(d-lastD))){out.rough=true;if(!out.at)out.at=d;}
  lastZ=hit.z;lastD=d;haveLast=true;
 }
 out.known=known>=2;
 return out;
}
bool GroundAt(std::uint32_t self,double x,double y,double top,double bottom,Ground& out){
 const float from[]={float(x),float(y),float(top)},to[]={float(x),float(y),float(bottom)};
 prefetch::RayHit hit;if(!Ray(self,from,to,0x22806,hit,30000))return false;
 out={};out.hit=hit.hit&&std::isfinite(hit.z);
 if(out.hit){out.z=hit.z;out.water=hit.layer==11;out.normalOk=hit.normalOk&&std::isfinite(hit.nz);out.nz=out.normalOk?std::fabs(hit.nz):1.0;
  if(!out.water)navmap::NoteGround(hit.x,hit.y,hit.z);}
 return true;
}
bool RayHit(const float from[3],const float to[3],double&x,double&y,double&z,bool&hit){
 prefetch::RayHit h;if(!Ray(0,from,to,0x22806,h,3000))return false;
 hit=h.hit&&std::isfinite(h.x)&&std::isfinite(h.y)&&std::isfinite(h.z);if(hit){x=h.x;y=h.y;z=h.z;}
 return true;
}
bool CameraRay(std::uint32_t owner,const float from[3],const float to[3],bool& hit,double& distance){
 prefetch::RayHit sample;
 if(!Ray(owner,from,to,0x22806,sample,unsigned(cameravisibility::rules::rayMs),true)||
    !Same(from,sample.from)||!Same(to,sample.to))return false;
 hit=sample.hit;
 distance=hit?std::hypot(double(sample.x)-from[0],double(sample.y)-from[1],double(sample.z)-from[2]):0;
 return true;
}
unsigned Pending(){unsigned n=0;for(const auto&q:queries)if(!q.inFlight&&!q.done)++n;return n;}
void Tick(){
 const auto now=GetTickCount64();
 static unsigned long long expired=0,started=0,refusedRays=0,answered=0,timedOut=0;
 logger::Observe("sensing.queue",0,Pending(),"pending=%u cached=%zu active_slots=%zu expired=%llu started=%llu answered=%llu refused=%llu timed_out=%llu",Pending(),queries.size(),flights.size(),expired,started,answered,refusedRays,timedOut);
 for(int s=0;s<kFlights;++s){auto&f=flights[s];if(!f.key)continue;prefetch::RayHit hit;unsigned age=0;
  const bool ready=prefetch::LatestRay(Slots[s],hit,age)&&age<10&&Same(f.from,hit.from)&&Same(f.to,hit.to);
  // A ray from where no world is loaded gets no answer. Waiting out its second held every slot once traffic flew far (2026-10-31: about 20 starts a second).
  const bool refused=!ready&&prefetch::RayRefused(Slots[s],f.from,f.to);
  if(!ready&&!refused&&now-f.at<1000)continue;
  if(ready)++answered;else if(refused)++refusedRays;else ++timedOut;
  for(auto&q:queries)if(q.key==f.key){q.inFlight=false;if(ready){q.hit=hit;q.done=now;q.refused=0;}else if(refused)q.refused=now;else q.asked=0;break;}f={};
 }
 // A finished query is kept 2 s as a cached answer. One never answered can only start within kStartWindowMs of being asked, so past that it
 // only holds a table entry; a moving vehicle's key changes every 0.5 m and it would never be asked again (18:59 run: 4,516 refusals).
 expired+=std::erase_if(queries,[&](const Query&q){return !q.inFlight&&!q.done&&!q.refused&&now-q.asked>=kStartWindowMs;});
 std::erase_if(queries,[&](const Query&q){return !q.inFlight&&now-q.asked>2000;});
 for(int s=0;s<kFlights;++s)if(!flights[s].key){
  // Never answered first, and of those the latest asked: a moving vehicle has left its older queries behind. A refused one waits 2 s before it is tried again.
  auto it=queries.end();for(auto q=queries.begin();q!=queries.end();++q)if(!q->inFlight&&(!q->done||now-q->done>=150)&&now-q->asked<kStartWindowMs&&(!q->refused||now-q->refused>=2000)&&
   (it==queries.end()||q->done<it->done||(q->done==it->done&&q->asked>it->asked)))it=q;
  if(it==queries.end())break;
  ++started;it->refused=0;
  auto&f=flights[s];f.key=it->key;f.at=now;std::copy(it->from,it->from+3,f.from);std::copy(it->to,it->to+3,f.to);
  it->inFlight=true;prefetch::RequestRay(Slots[s],f.from,f.to,it->mask);
 }
}
static Site CheckFootprint(std::uint32_t self,const world::VehicleInfo& v,double x,double y,double z,double yaw,bool boarding,bool publishFloor=true){
 Site out;if(!v.formId){out.reason="Vehicle bounds unavailable";return out;}
 const double w=std::max(double(v.half[0])+0.5,1.5),l=std::max(double(v.half[1])+0.5,2.0);
 const double rx=std::cos(yaw),ry=-std::sin(yaw),fx=std::sin(yaw),fy=std::cos(yaw);
 auto tracePending=[&](const char* stage,int i,int j,const float* from,const float* to,std::uint64_t mask){
#ifndef FVO_RELEASE
  if(boarding)return;
  const auto key=Key(self,from,to,mask);
  const auto query=std::find_if(queries.begin(),queries.end(),[&](const Query& q){return q.key==key;});
  const char* state=query==queries.end()?"not_queued":query->inFlight?"in_flight":query->refused?"native_refused":query->done?"cached_answer_expired":"waiting_to_start";
  logger::Observe("spawn.support",v.formId,key,"stage=%s sample=%d,%d status=%s query=%llX site=%.2f,%.2f,%.2f from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f cached=%zu",stage,i,j,state,static_cast<unsigned long long>(key),x,y,z,from[0],from[1],from[2],to[0],to[1],to[2],queries.size());
#endif
 };
 std::array<double,9> heights;int index=0;bool pending=false;
 for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
  const float from[]={float(x+rx*w*i+fx*l*j),float(y+ry*w*i+fy*l*j),float(z+6)};
  const float to[]={from[0],from[1],float(z-120)};prefetch::RayHit hit;
  if(!Ray(self,from,to,0x22806,hit)){tracePending("floor",i,j,from,to,0x22806);pending=true;heights[index++]=0;continue;}
  if(!hit.hit||hit.layer==11||!std::isfinite(hit.z)||(hit.normalOk&&hit.nz<0.7f)){out.verdict=Verdict::Unsafe;out.reason="No dry, level support";return out;}
  heights[index++]=hit.z;
  if(publishFloor)navmap::NoteGround(hit.x,hit.y,hit.z);
 }
 if(pending)return out;
 if(!sitelogic::Support(heights)){out.verdict=Verdict::Unsafe;out.reason="Slope or platform edge";return out;}
 out.ground=*std::max_element(heights.begin(),heights.end());
 out.supported=true;
 // Verify headroom throughout the vehicle footprint, with the vehicle's own body exempted.
 for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
  const float from[]={float(x+rx*w*i+fx*l*j),float(y+ry*w*i+fy*l*j),float(out.ground+0.25)};
  const float to[]={from[0],from[1],float(out.ground+2.5+std::max(2.0,double(v.half[2])))};prefetch::RayHit hit;
  if(!Ray(self,from,to,Solid,hit)){tracePending("headroom",i,j,from,to,Solid);pending=true;continue;}
  if(hit.hit&&(!self||hit.formId!=self)){out.verdict=Verdict::Unsafe;out.reason="Vehicle clearance blocked";return out;}
  if(hit.hit&&hit.formId==self){
   // A first hit on our hull must not hide a ceiling above it.
   float above[]={from[0],from[1],float(v.pos[2]+v.half[2]+0.1)};
   if(above[2]<to[2]){
    prefetch::RayHit ceiling;
    if(!Ray(self,above,to,Solid,ceiling)){tracePending("above_body",i,j,above,to,Solid);pending=true;continue;}
    if(ceiling.hit){out.verdict=Verdict::Unsafe;out.reason="Ceiling above vehicle";return out;}
   }
  }
 }
 if(pending)return out;
 static thread_local world::Snapshot all;if(!world::VehiclesAndHeld(all))return out;
 for(unsigned i=0;i<all.count&&i<world::kMaxVehicles;++i){const auto&o=all.v[i];if(o.formId&&o.formId!=self&&std::fabs(o.pos[2]-out.ground)<6&&
  std::hypot(o.pos[0]-x,o.pos[1]-y)<std::hypot(w,l)+std::hypot(o.half[0],o.half[1])+1){out.verdict=Verdict::Unsafe;out.reason="Another vehicle occupies the site";return out;}}
 if(!boarding){out.verdict=Verdict::Safe;out.reason="Validated dry spawn footprint and headroom";return out;}
 // Both sides are tested; a missing side is not silently replaced with an unsafe position.
 for(int side:{-1,1}){
  const double sx=x+rx*(w+1.5)*side,sy=y+ry*(w+1.5)*side;
  const float down[]={float(sx),float(sy),float(out.ground+3)},end[]={float(sx),float(sy),float(out.ground-3)};prefetch::RayHit floor;
  if(!Ray(self,down,end,0x22806,floor))return out;
  if(!floor.hit||floor.layer==11||std::fabs(floor.z-out.ground)>0.6||(floor.normalOk&&floor.nz<0.7))continue;
  bool clear=true;
  for(int k=-1;k<=1;++k){
   const float from[]={float(sx+fx*k*0.5),float(sy+fy*k*0.5),floor.z+0.15f},to[]={from[0],from[1],floor.z+2.3f};prefetch::RayHit hit;
   if(!Ray(self,from,to,Solid,hit))return out;if(hit.hit){clear=false;break;}
  }
  const float from[]={float(x+rx*(w+0.1)*side),float(y+ry*(w+0.1)*side),floor.z+1},to[]={float(sx),float(sy),floor.z+1};prefetch::RayHit path;
  if(!Ray(self,from,to,Solid,path))return out;
  if(clear&&(!path.hit||path.formId==self)){out.verdict=Verdict::Safe;out.x=sx;out.y=sy;out.z=floor.z+0.1;out.reason="Validated footprint and boarding side";return out;}
 }
 out.verdict=Verdict::Unsafe;out.reason="No usable boarding side";return out;
}
Site CheckSite(std::uint32_t self,double x,double y,double z,double yaw){return CheckFootprint(self,Vehicle(self),x,y,z,yaw,true);}
Site SpawnSite(std::uint32_t base,double x,double y,double z,double yaw,bool publishFloor){
 world::VehicleInfo v{};double bottom=0;
 __try {
  const auto form=reinterpret_cast<std::uintptr_t(*)(std::uint32_t)>(game::Base()+0x005DE3B0)(base);
  if(!form)return {};
  for(int i=0;i<3;++i){const auto lo=*reinterpret_cast<float*>(form+0x50+i*4),hi=*reinterpret_cast<float*>(form+0x5C+i*4);
   if(!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo||hi-lo>40||std::max(std::fabs(lo),std::fabs(hi))>40)return {};
   // Conservative around the reference origin even for asymmetric model bounds.
   // Spawn clearance needs the full vertical extent above support, not half-height.
   v.half[i]=i==2?hi-lo:std::max(std::fabs(lo),std::fabs(hi));if(i==2)bottom=lo;
  }
  v.formId=base;
 }__except(EXCEPTION_EXECUTE_HANDLER){return {};}
 auto site=CheckFootprint(0,v,x,y,z,yaw,false,publishFloor);
 // The reference origin is not necessarily the bottom of the hull.
 site.z=site.ground-bottom+0.1;return site;
}
Site AirSite(std::uint32_t base,double x,double y,double z,double yaw){
 Site out;out.x=x;out.y=y;out.z=z;
 double half[3]{};
 double bottom=-1.5;  // hull bottom below the reference origin (model bounds; a mid-height guess without them)
 __try {
  const auto form=reinterpret_cast<std::uintptr_t(*)(std::uint32_t)>(game::Base()+0x005DE3B0)(base);
  if(!form)return out;
  // A model without stored bounds (most mod vehicles) gets a generous default envelope rather than skipping the checks.
  bool known=true;double read[3]{},lowZ=0;
  for(int i=0;i<3;++i){const double lo=*reinterpret_cast<float*>(form+0x50+i*4),hi=*reinterpret_cast<float*>(form+0x5C+i*4);
   if(!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo||hi-lo>40||std::max(std::fabs(lo),std::fabs(hi))>40){known=false;break;}
   read[i]=std::max(std::fabs(lo),std::fabs(hi))+2;if(i==2)lowZ=lo;
  }
  const double fallback[3]={4.5,6.0,3.5};
  for(int i=0;i<3;++i)half[i]=known?read[i]:fallback[i];
  if(known)bottom=lowZ;
 }__except(EXCEPTION_EXECUTE_HANDLER){return out;}
 // Ground guard (2026-10-29 late, the user: "guards that don't accept spawns like that"): 43 traffic vehicles were placed
 // 50-130 m underground on routes drawn below the terrain; with nothing mapped there the envelope test passed them, and many
 // fell through the world. The terrain there (the cell's baked grid, else the nav map's top surface) must be below the hull;
 // with neither known, the authored lane height is trusted.
 // 2026-11-01 fix (road-level lanes): the guard used the whole envelope plus margin, so a lane
 // point a metre over a road could never pass and road traffic was never placed near the player.
 // Now only a hull whose real bottom is over 0.5 m underground is refused, and the envelope is
 // tested from just above the ground.
 double floorZ=z-half[2];
 {
  double ground=0;bool known=cellbake::Ground(x,y,ground)&&std::isfinite(ground);
  if(!known){const auto top=navmap::At(x,y,z+1000);if(top.haveGround&&std::isfinite(top.ground)){ground=top.ground;known=true;}}
  if(known)out.ground=ground;
  if(spawnfloor::Check(z,bottom,half[2],known,ground,floorZ)<0){out.verdict=Verdict::Unsafe;out.reason="Spawn spot is under the ground";return out;}
  // Unknown ground is not refused (2026-10-29 23:01 run: New Atlantis lanes at -100 m, ~60 m over its -160 m ground, were all
  // deferred by a 0 floor while the ground was still unloaded). The vehicle stays frozen until its pilot holds it.
 }
 bool pending=false;
 // Road-level spot (2026-11-05: 64 spots on Roadrunner's deck refused, "flight envelope intersects mapped solid"): the hull sits on a
 // measured surface. The flight margins (2 m every side, the envelope reaching into the road) do not apply: the footprint is the
 // car plus 0.4 m, a solid counts only where a measured ray hit lies inside it (a 4 m map cell is too coarse for a railing),
 // collision object boxes are left out (on roads they are whole deck pieces, as the drive map treats them), and the rays start
 // just above the road. Spots in the air keep the full flight envelope.
 double roadGround=0;bool roadLevel=false;
 // The road under the spot, measured by one short ray straight down from just over the hull bottom (2026-11-05 06:04 run: the
 // nav map keeps the highest surface, so on a stacked road it gave the upper deck and no lower-deck spot ever read as road level).
 bool downPending=false;
 {const double hull=z+bottom;const float from[]={float(x),float(y),float(hull+1.0)},to[]={float(x),float(y),float(hull-2.5)};prefetch::RayHit down;
  if(!Ray(0,from,to,Solid,down))downPending=true;
  else if(down.hit&&std::isfinite(down.z)&&hull-down.z>=-0.6){roadGround=down.z;roadLevel=true;}}
 if(!roadLevel){const auto centre=navmap::At(x,y,z);const double hull=z+bottom;
  if(centre.haveGround&&std::isfinite(centre.ground)&&hull-centre.ground>=-0.6&&hull-centre.ground<=2.5){roadGround=centre.ground;roadLevel=true;}
  else if(out.ground!=0&&hull-out.ground>=-0.6&&hull-out.ground<=2.5){roadGround=out.ground;roadLevel=true;}}
 // The down ray is answered a frame or two later (2026-11-05 06:14 run: judged at once as a flight spot, refused, raised 0.5 m: a new
 // ray each time, so no road spot ever passed). Wait for it; the caller keeps the spot (behind the loading screen, where rays are
 // not answered, the nav map's answer above stands).
 if(downPending&&!roadLevel&&g_raysAnswered)return out;
 if(roadLevel){for(int i=0;i<2;++i)half[i]=std::max(0.8,half[i]-1.6);floorZ=std::max(floorZ,roadGround+0.6);}
 // Check the envelope, including its interior, with vertical and horizontal
 // cross-sections. This is admission at authored flight height, not ground support.
 const double rx=std::cos(yaw),ry=-std::sin(yaw),fx=std::sin(yaw),fy=std::cos(yaw);
 for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
  const double px=x+rx*half[0]*i+fx*half[1]*j,py=y+ry*half[0]*i+fy*half[1]*j;
  const float a[]={float(px),float(py),float(floorZ)},b[]={float(px),float(py),float(z+half[2])};prefetch::RayHit hit;
  const auto mapped=navmap::At(px,py,z);
  // Road-level spots allow 0.5 m of uneven ground under the footprint's corners.
  if(roadLevel){double hz=0;
   if(mapped.blocked&&navmap::HitNear(px,py,roadGround+0.6,z+half[2],0.4,&hz)){out.verdict=Verdict::Unsafe;out.reason="Road spot: a measured solid inside the car's footprint";return out;}
   if(mapped.haveGround&&mapped.ground>roadGround+1.0){out.verdict=Verdict::Unsafe;out.reason="Road spot: the surface rises under the car's footprint";return out;}
  }
  else if(mapped.blocked||mapped.objBlocked||(mapped.haveGround&&mapped.ground>=floorZ+(floorZ>z-half[2]?0.5:0.0))){out.verdict=Verdict::Unsafe;out.reason="Flight envelope intersects mapped solid";return out;}
  if(!Ray(0,a,b,Solid,hit)){pending=true;continue;}
  if(hit.hit){out.verdict=Verdict::Unsafe;out.reason="Flight envelope intersects collision";return out;}
 }
 for(int k=-1;k<=1;++k)for(int j=-1;j<=1;++j){
  const float a[]={float(x-rx*half[0]+fx*half[1]*j),float(y-ry*half[0]+fy*half[1]*j),float(k<0?std::max(z-half[2],floorZ+0.3):z+k*half[2])};
  const float b[]={float(x+rx*half[0]+fx*half[1]*j),float(y+ry*half[0]+fy*half[1]*j),a[2]};prefetch::RayHit hit;
  if(!Ray(0,a,b,Solid,hit)){pending=true;continue;}
  if(hit.hit){out.verdict=Verdict::Unsafe;out.reason="Flight envelope cross-section blocked";return out;}
 }
 if(pending)return out;
 out.verdict=Verdict::Safe;out.reason=roadLevel?"Validated road spot":"Validated flight envelope";return out;
}
Site MappedSite(std::uint32_t base,double x,double y,double z,double yaw){
 Site out;out.x=x;out.y=y;out.z=z;
 double half[3]={4.5,6.0,3.5};
 __try {
  const auto form=reinterpret_cast<std::uintptr_t(*)(std::uint32_t)>(game::Base()+0x005DE3B0)(base);
  if(form){bool known=true;double read[3]{};
   for(int i=0;i<3;++i){const double lo=*reinterpret_cast<float*>(form+0x50+i*4),hi=*reinterpret_cast<float*>(form+0x5C+i*4);
    if(!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo||hi-lo>40||std::max(std::fabs(lo),std::fabs(hi))>40){known=false;break;}
    read[i]=std::max(std::fabs(lo),std::fabs(hi))+1;}
   if(known)for(int i=0;i<3;++i)half[i]=read[i];}
 }__except(EXCEPTION_EXECUTE_HANDLER){}
 double ground=0;
 if(cellbake::Ground(x,y,ground)&&std::isfinite(ground)&&z-half[2]<ground+0.5){out.verdict=Verdict::Unsafe;out.reason="under the ground";out.ground=ground;return out;}
 const double rx=std::cos(yaw),ry=-std::sin(yaw),fx=std::sin(yaw),fy=std::cos(yaw);
 int mapped=0;
 for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
  const double px=x+rx*half[0]*i+fx*half[1]*j,py=y+ry*half[0]*i+fy*half[1]*j;
  for(const double dz:{-half[2]*0.5,0.0,half[2]*0.5}){
   const auto m=navmap::At(px,py,z+dz);
   if(!m.known)continue;
   ++mapped;
   if((m.blocked||m.objBlocked)&&!m.objOverruled){out.verdict=Verdict::Unsafe;out.reason="inside a building or other solid";return out;}
   if(m.haveGround&&m.ground>=z-half[2]){out.verdict=Verdict::Unsafe;out.reason="ground or a floor inside the hull";return out;}
  }
 }
 if(mapped<9)return out;  // too little mapped there yet: pending
 out.verdict=Verdict::Safe;out.reason="clear on the nav map";return out;
}
Site BoardingSite(std::uintptr_t ref){
 if(!ref)return {};const auto id=*reinterpret_cast<std::uint32_t*>(ref+off::kFormId);const auto v=Vehicle(id);
 auto result=CheckSite(id,v.pos[0],v.pos[1],v.pos[2],v.heading);
 if(result.verdict==Verdict::Safe&&!sitelogic::Standing(result.z-0.1,result.ground,std::hypot(std::hypot(v.vel[0],v.vel[1]),v.vel[2]),v.pos[2])){result.verdict=Verdict::Unsafe;result.reason="Vehicle must stop and descend before boarding or exit";}
 return result;
}
// Getting in only needs the vehicle at rest: the game's own seat walk / snap finds the way in. The full site (headroom, a free
// side, no vehicle beside it) is for putting someone OUT; on entry it refused every vehicle parked in a row (2026-10-21 20:37:
// 6 of 6 fight boardings timed out on "no validated boarding site", the instant seat behind the same gate).
Site EntrySite(std::uintptr_t ref){
 auto result=BoardingSite(ref);
 if(!ref||result.verdict==Verdict::Safe)return result;
 const auto v=Vehicle(*reinterpret_cast<std::uint32_t*>(ref+off::kFormId));
 const double speed=std::hypot(std::hypot(v.vel[0],v.vel[1]),v.vel[2]);
 if(result.supported){
  if(sitelogic::Standing(result.ground,result.ground,speed,v.pos[2])){result.verdict=Verdict::Safe;result.reason="At rest (entry)";}
  else{result.verdict=Verdict::Unsafe;result.reason="Vehicle must stop and descend before boarding";}
 }
 else if(result.verdict==Verdict::Unsafe&&speed<=0.75){result.verdict=Verdict::Safe;result.reason="At rest on uneven ground (entry)";}
 return result;
}
bool ClearShot(std::uint32_t self,std::uint32_t target,std::uint32_t targetVehicle,const float from[3],const float to[3]){
 float origin[3]={from[0],from[1],from[2]};const auto v=Vehicle(self);
 const double length=std::hypot(std::hypot(to[0]-from[0],to[1]-from[1]),to[2]-from[2]);
 const double offset=std::max(0.0,std::hypot(v.half[0],v.half[1])+0.5-std::hypot(std::hypot(from[0]-v.pos[0],from[1]-v.pos[1]),from[2]-v.pos[2]));
 if(length<offset+0.5)return false;
 for(int i=0;i<3;++i)origin[i]+=float((to[i]-from[i])*offset/length);
 prefetch::RayHit hit;if(!Ray(self,origin,to,Solid,hit,150))return false;
 const double distance=length-offset;
 return !hit.hit||(target&&hit.formId==target)||(targetVehicle&&hit.formId==targetVehicle)||hit.distance>=distance-0.5;
}
bool CombatPosition(std::uint32_t self,const float enemy[3],double&x,double&y,double&z,bool defensive){
 const auto v=Vehicle(self);if(!v.formId)return false;
 if(!std::isfinite(z))z=v.pos[2];
 const auto&field=navfield::Get(v.pos[0],v.pos[1],v.pos[2],x,y,std::max(2.5,double(v.half[0])+0.5),true,z);
 double best=1e12,bx=x,by=y;
 for(double radius:{0.0,8.0,16.0})for(int k=0;k<(radius==0?1:8);++k){
  const double px=x+radius*std::sin(k*0.7853981633974483),py=y+radius*std::cos(k*0.7853981633974483);
  const auto m=navmap::At(px,py,z);if(!m.known||m.blocked||m.objBlocked||(m.haveGround&&m.ground+2.5>z)||field.Value(px,py,z)>=1e12)continue;
  const float body[]={float(px),float(py),float(z+0.5)},muzzle[]={float(px),float(py),float(z+2.5)};
  const bool cover=MapBlocked(enemy,body),shot=!MapBlocked(muzzle,enemy);
  const double score=radius+std::hypot(px-v.pos[0],py-v.pos[1])*0.1-(defensive&&cover?24:0)-(shot?12:0);
  if(score<best){best=score;bx=px;by=py;}
 }
 if(best>=1e12)return false;x=bx;y=by;return true;
}
void Reset(){queries.clear();flights={};}
}
