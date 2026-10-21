#include "environment.h"
#include "sitelogic.h"
#include "addresses.h"
#include "game.h"
#include "prefetch.h"
#include "navmap.h"
#include "navfield.h"
#include "vehicleworld.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <limits>
namespace environment {
namespace {
constexpr int Slots[]={22,23,45,46,47};
constexpr std::uint64_t Solid=0xA5C12671Eull;
struct Query {std::uint64_t key=0,mask=0;float from[3]={},to[3]={};prefetch::RayHit hit;unsigned long long asked=0,done=0;bool inFlight=false;};
std::vector<Query> queries;
struct Flight {std::uint64_t key=0;float from[3]={},to[3]={};unsigned long long at=0;};
std::array<Flight,5> flights;
std::uint64_t Key(std::uint32_t owner,const float from[3],const float to[3],std::uint64_t mask){
 std::uint64_t h=owner^mask;
 for(const auto*p:{from,to})for(int i=0;i<3;++i){const auto v=std::int64_t(std::floor(p[i]*2));h^=std::uint64_t(v)+0x9e3779b97f4a7c15ull+(h<<6)+(h>>2);}
 return h?h:1;
}
bool Same(const float a[3],const float b[3]){for(int i=0;i<3;++i)if(std::fabs(a[i]-b[i])>0.1)return false;return true;}
bool Ray(std::uint32_t owner,const float from[3],const float to[3],std::uint64_t mask,prefetch::RayHit&out,unsigned ttl=1000){
 const auto now=GetTickCount64(),key=Key(owner,from,to,mask);
 for(auto&q:queries)if(q.key==key){q.asked=now;if(q.done&&now-q.done<ttl){out=q.hit;return true;}return false;}
 if(queries.size()>=256)return false;
 Query q;q.key=key;q.mask=mask;q.asked=now;std::copy(from,from+3,q.from);std::copy(to,to+3,q.to);queries.push_back(q);return false;
}
world::VehicleInfo Vehicle(std::uint32_t id){
 world::Snapshot s;if(world::Vehicles(s))for(unsigned i=0;i<s.count&&i<64;++i)if(s.v[i].formId==id)return s.v[i];
 world::VehicleInfo result;
 __try {
  const auto ref=reinterpret_cast<std::uintptr_t(*)(std::uint32_t)>(game::Base()+0x005DE3B0)(id);
  if(!ref||*reinterpret_cast<std::uint32_t*>(ref+off::kFormId)!=id)return result;
  const auto base=*reinterpret_cast<std::uintptr_t*>(ref+0x98);if(!base)return result;
  result.formId=id;result.ref=ref;const auto p=*reinterpret_cast<game::Vec3*>(ref+off::kRefPosition);
  result.pos[0]=p.x;result.pos[1]=p.y;result.pos[2]=p.z;result.heading=reinterpret_cast<game::Vec3*>(ref+off::kRefRotation)->z;
  for(int i=0;i<3;++i){const double half=0.5*(*reinterpret_cast<float*>(base+0x54+i*4)-*reinterpret_cast<float*>(base+0x48+i*4));
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
void Tick(){
 const auto now=GetTickCount64();
 for(int s=0;s<5;++s){auto&f=flights[s];if(!f.key)continue;prefetch::RayHit hit;unsigned age=0;
  const bool ready=prefetch::LatestRay(Slots[s],hit,age)&&age<10&&Same(f.from,hit.from)&&Same(f.to,hit.to);
  if(!ready&&now-f.at<1000)continue;
  for(auto&q:queries)if(q.key==f.key){q.inFlight=false;if(ready){q.hit=hit;q.done=now;}else q.asked=0;break;}f={};
 }
 std::erase_if(queries,[&](const Query&q){return !q.inFlight&&now-q.asked>2000;});
 for(int s=0;s<5;++s)if(!flights[s].key){
  auto it=queries.end();for(auto q=queries.begin();q!=queries.end();++q)if(!q->inFlight&&(!q->done||now-q->done>=150)&&now-q->asked<100&&
   (it==queries.end()||q->done<it->done))it=q;
  if(it==queries.end())break;
  auto&f=flights[s];f.key=it->key;f.at=now;std::copy(it->from,it->from+3,f.from);std::copy(it->to,it->to+3,f.to);
  it->inFlight=true;prefetch::RequestRay(Slots[s],f.from,f.to,it->mask);
 }
}
Site CheckSite(std::uint32_t self,double x,double y,double z,double yaw){
 Site out;const auto v=Vehicle(self);if(!v.formId){out.reason="Vehicle bounds unavailable";return out;}
 const double w=std::max(double(v.half[0])+0.5,1.5),l=std::max(double(v.half[1])+0.5,2.0);
 const double rx=std::cos(yaw),ry=-std::sin(yaw),fx=std::sin(yaw),fy=std::cos(yaw);
 std::array<double,9> heights;int index=0;bool pending=false;
 for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
  const float from[]={float(x+rx*w*i+fx*l*j),float(y+ry*w*i+fy*l*j),float(z+6)};
  const float to[]={from[0],from[1],float(z-120)};prefetch::RayHit hit;
  if(!Ray(self,from,to,0x22806,hit)){pending=true;heights[index++]=0;continue;}
  if(!hit.hit||hit.layer==11||!std::isfinite(hit.z)||(hit.normalOk&&hit.nz<0.7f)){out.verdict=Verdict::Unsafe;out.reason="No dry, level support";return out;}
  heights[index++]=hit.z;navmap::NoteGround(hit.x,hit.y,hit.z);
 }
 if(pending)return out;
 if(!sitelogic::Support(heights)){out.verdict=Verdict::Unsafe;out.reason="Slope or platform edge";return out;}
 out.ground=*std::max_element(heights.begin(),heights.end());
 out.supported=true;
 // Verify headroom throughout the vehicle footprint, with the vehicle's own body exempted.
 for(int j=-1;j<=1;++j)for(int i=-1;i<=1;++i){
  const float from[]={float(x+rx*w*i+fx*l*j),float(y+ry*w*i+fy*l*j),float(out.ground+0.25)};
  const float to[]={from[0],from[1],float(out.ground+2.5+std::max(2.0,double(v.half[2])))};prefetch::RayHit hit;
  if(!Ray(self,from,to,Solid,hit)){pending=true;continue;}
  if(hit.hit&&hit.formId!=self){out.verdict=Verdict::Unsafe;out.reason="Vehicle clearance blocked";return out;}
  if(hit.hit&&hit.formId==self){
   // A first hit on our hull must not hide a ceiling above it.
   float above[]={from[0],from[1],float(v.pos[2]+v.half[2]+0.1)};
   if(above[2]<to[2]){
    prefetch::RayHit ceiling;
    if(!Ray(self,above,to,Solid,ceiling)){pending=true;continue;}
    if(ceiling.hit){out.verdict=Verdict::Unsafe;out.reason="Ceiling above vehicle";return out;}
   }
  }
 }
 if(pending)return out;
 world::Snapshot all;if(!world::Vehicles(all))return out;
 for(unsigned i=0;i<all.count&&i<64;++i){const auto&o=all.v[i];if(o.formId&&o.formId!=self&&std::fabs(o.pos[2]-out.ground)<6&&
  std::hypot(o.pos[0]-x,o.pos[1]-y)<std::hypot(w,l)+std::hypot(o.half[0],o.half[1])+1){out.verdict=Verdict::Unsafe;out.reason="Another vehicle occupies the site";return out;}}
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
Site BoardingSite(std::uintptr_t ref){
 if(!ref)return {};const auto id=*reinterpret_cast<std::uint32_t*>(ref+off::kFormId);const auto v=Vehicle(id);
 auto result=CheckSite(id,v.pos[0],v.pos[1],v.pos[2],v.heading);
 if(result.verdict==Verdict::Safe&&!sitelogic::Standing(result.z-0.1,result.ground,std::hypot(std::hypot(v.vel[0],v.vel[1]),v.vel[2]),v.pos[2])){result.verdict=Verdict::Unsafe;result.reason="Vehicle must stop and descend before boarding or exit";}
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
