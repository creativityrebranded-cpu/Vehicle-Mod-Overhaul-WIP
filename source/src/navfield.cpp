#include "navfield.h"
#include "navmap.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <queue>
#include <utility>
#include <future>
#include <chrono>
#include <thread>
#include <memory>
#include <cstdint>
namespace navfield {
namespace {
constexpr int N=125,L=9,Plane=N*N;
constexpr double Cell=4,Step=8,Inf=1e12;
std::array<Field,8> cache;
int Index(double v){return int(std::floor(v/Cell));}
bool Solid(const Field& f,double x,double y,double z) {
 const auto m=navmap::At(x,y,z);
 return m.blocked || (f.objects && m.objBlocked) || (m.haveGround && m.ground+2.5>z);
}
constexpr int Samples=(L-1)*4+1;
struct Raster {Field field;std::vector<unsigned char> solid,known;std::vector<float> weight;std::size_t copied=0;};
std::array<std::unique_ptr<Raster>,8> work;
std::future<Field> search;
int searching=-1;
Field Build(Raster raster) {
 Field f=std::move(raster.field);
 f.cost.assign(Plane*L,float(Inf));
 std::vector<unsigned char> blocked(Plane*L),known(Plane*L),vertical(Plane*(L-1));
 std::vector<float> weight(Plane*L,1);
 // Inflate every two-metre altitude sample on the worker, including intermediate deck heights.
 const auto raw=raster.solid;
 const int radius=int(std::ceil(f.width/Cell))+1;
 for(int h=0;h<Samples;++h) for(int j=0;j<N;++j) for(int i=0;i<N;++i) if(raw[h*Plane+j*N+i]) {
  for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx) {
   const int a=i+dx,b=j+dy;if(a<0||b<0||a>=N||b>=N)continue;
   const double gap=std::hypot(double(dx),double(dy))*Cell;
   if(gap<f.width+2)raster.solid[h*Plane+b*N+a]=1;
   else if(gap<f.width+8)raster.weight[h*Plane+b*N+a]+=0.5f;
  }
 }
 for(int h=0;h<L;++h)for(int k=0;k<Plane;++k){
  blocked[h*Plane+k]=raster.solid[h*4*Plane+k];known[h*Plane+k]=raster.known[h*4*Plane+k];weight[h*Plane+k]=raster.weight[h*4*Plane+k];
  if(h<L-1){bool clear=true;for(int d=0;d<=4;++d)if(raster.solid[(h*4+d)*Plane+k]||!raster.known[(h*4+d)*Plane+k]){clear=false;break;}vertical[h*Plane+k]=clear;}
 }
 using Entry=std::pair<float,int>;
 std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> q;
 auto seed=[&](int k,double d){if(!blocked[k]&&d<f.cost[k]) {f.cost[k]=float(d);q.emplace(f.cost[k],k);}};
 const int gi=Index(f.gx)-f.ox,gj=Index(f.gy)-f.oy;
 const double target=std::isfinite(f.gz) ? f.gz : f.z;
 if(gi>=0&&gj>=0&&gi<N&&gj<N) {
  int best=-1;double distance=Inf;
  for(int h=0;h<L;++h) for(int j=0;j<N;++j) for(int i=0;i<N;++i) {
   const int k=h*Plane+j*N+i;if(blocked[k]) continue;
   const double d=std::hypot(std::hypot((f.ox+i+0.5)*Cell-f.gx,(f.oy+j+0.5)*Cell-f.gy),1.5*(f.bottom+h*Step-target));
   if(d<distance) {distance=d;best=k;}
  }
  if(best>=0) seed(best,distance);
 } else {
  for(int h=0;h<L;++h) for(int j=0;j<N;++j) for(int i=0;i<N;++i) if(i==0||j==0||i==N-1||j==N-1)
   seed(h*Plane+j*N+i,std::hypot(std::hypot((f.ox+i+0.5)*Cell-f.gx,(f.oy+j+0.5)*Cell-f.gy),1.5*(f.bottom+h*Step-target)));
 }
 while(!q.empty()) {
  const auto [d,k]=q.top();q.pop();if(d!=f.cost[k]) continue;
  const int h=k/Plane,i=k%N,j=(k%Plane)/N;
  for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
   if(!dx&&!dy) continue;const int a=i+dx,b=j+dy;
   if(a<0||b<0||a>=N||b>=N||blocked[h*Plane+b*N+a]) continue;
   if(dx&&dy&&(blocked[h*Plane+j*N+a]||blocked[h*Plane+b*N+i])) continue;
   seed(h*Plane+b*N+a,d+Cell*(dx&&dy?1.4142135623730951:1)*(weight[k]+weight[h*Plane+b*N+a])*0.5);
  }
  if(h>0 && vertical[k-Plane]) seed(k-Plane,d+Step*1.5);
  if(h<L-1 && vertical[k]) seed(k+Plane,d+Step*1.5);
 }
 f.built=GetTickCount64();return f;
}
}
double Field::Value(double x,double y) const{return Value(x,y,z);}
double Field::Value(double x,double y,double height) const {
 const int i=Index(x)-ox,j=Index(y)-oy;
 const int h=int(std::round((height-bottom)/Step));
 if(i<0||j<0||i>=N||j>=N||h<0||h>=L||cost.empty()) return Inf;
 const int k=h*Plane+j*N+i;if(cost[k]>=float(Inf)) return Inf;
 double best=Inf;
 for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
  const int a=i+dx,b=j+dy;if(a<0||b<0||a>=N||b>=N) continue;
  if(dx&&dy&&(cost[h*Plane+j*N+a]>=float(Inf)||cost[h*Plane+b*N+i]>=float(Inf))) continue;
  best=std::min(best,double(cost[h*Plane+b*N+a])+std::hypot(x-(ox+a+0.5)*Cell,y-(oy+b+0.5)*Cell));
 }
 // Interpolate toward an adjacent height only through a clear column (never across a ceiling).
 const int next=height>bottom+h*Step ? h+1 : h-1;
 if(next>=0 && next<L && cost[next*Plane+j*N+i]<float(Inf)) {
  bool clear=true;
  const double lo=std::min(height,bottom+next*Step),hi=std::max(height,bottom+next*Step);
  for(double level=lo;level<=hi;level+=1) if(Solid(*this,x,y,level)) {clear=false;break;}
  if(clear) {
   const double t=std::clamp(std::fabs(height-(bottom+h*Step))/Step,0.0,1.0);
   best=best*(1-t)+cost[next*Plane+j*N+i]*t;
  }
 }
 return best;
}
const Field& Get(double x,double y,double z,double gx,double gy,double width,bool objects,double gz) {
 const auto now=GetTickCount64();
 auto matches=[&](const Field&f){return std::hypot(f.gx-gx,f.gy-gy)<16&&std::fabs(f.z-z)<4&&
  std::isfinite(f.gz)==std::isfinite(gz)&&(!std::isfinite(gz)||std::fabs(f.gz-gz)<4)&&std::fabs(f.width-width)<0.25&&f.objects==objects&&
  std::abs(Index(x)-f.ox-N/2)<40&&std::abs(Index(y)-f.oy-N/2)<40;};
 for(int i=0;i<8;++i)if(work[i]&&matches(work[i]->field))return cache[i];
 int slot=-1;for(int i=0;i<8;++i)if(!cache[i].cost.empty()&&matches(cache[i])){if(now-cache[i].built<1000)return cache[i];slot=i;break;}
 if(slot<0){for(int i=0;i<8;++i)if(!work[i]&&(slot<0||cache[i].built<cache[slot].built))slot=i;}
 if(slot<0){static const Field empty;return empty;}
 if(!work[slot]){
  // Slot reuse must not temporarily steer a new goal using an unrelated old field.
  if(!matches(cache[slot]))cache[slot]=Field{};
  auto r=std::make_unique<Raster>();auto&f=r->field;
  f.ox=Index(x)-N/2;f.oy=Index(y)-N/2;f.bottom=std::floor(z/Step)*Step-2*Step;
  f.gx=gx;f.gy=gy;f.z=z;f.gz=gz;f.width=width;f.objects=objects;
  r->solid.resize(Plane*Samples);r->known.resize(Plane*Samples);r->weight.resize(Plane*Samples);
  work[slot]=std::move(r);
 }
 return cache[slot];
}
void Service(double milliseconds){
 const auto until=std::chrono::steady_clock::now()+std::chrono::microseconds(std::int64_t(milliseconds*1000));
 do {
  if(searching>=0&&search.wait_for(std::chrono::milliseconds(0))==std::future_status::ready){cache[searching]=search.get();work[searching].reset();searching=-1;}
  // 2026-10-24 CPU fix: return as soon as no raster copying is left. The old loop spun (yield) for the whole
  // budget while the worker searched, costing ~2 ms of main thread per update; the result is picked up next call.
  bool progressed=false;
  for(int slot=0;slot<8;++slot)if(work[slot]&&slot!=searching){
   auto&r=*work[slot];const auto&f=r.field;
   if(r.copied==r.solid.size()&&searching>=0)continue;  // copied, waiting for the worker: nothing to do now
   progressed=true;
   const auto end=std::min(r.copied+std::size_t(256),r.solid.size());
   for(;r.copied<end;++r.copied){const int k=int(r.copied),h=k/Plane,i=k%N,j=(k%Plane)/N;
    const auto m=navmap::At((f.ox+i+0.5)*Cell,(f.oy+j+0.5)*Cell,f.bottom+h*2);
    r.solid[k]=m.blocked||(f.objects&&m.objBlocked)||(m.haveGround&&m.ground+2.5>f.bottom+h*2);
    r.known[k]=m.known;r.weight[k]=float(1+(!m.known?0.5:0)+std::min(m.contacts,10)*0.25);
   }
   if(r.copied==r.solid.size()&&searching<0){searching=slot;Raster ready=std::move(r);search=std::async(std::launch::async,[r=std::move(ready)]()mutable{return Build(std::move(r));});}
   if(std::chrono::steady_clock::now()>=until)return;
  }
  if(!progressed)return;
 }while(std::chrono::steady_clock::now()<until);
}
bool Idle(){if(searching>=0)return false;for(const auto&w:work)if(w)return false;return true;}
void Reset(){if(searching>=0){search.wait();searching=-1;}for(auto&w:work)w.reset();for(auto&f:cache)f=Field{};}
}
