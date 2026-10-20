#include "navfield.h"
#include "navmap.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <utility>
namespace navfield {
namespace {
constexpr int N=125;
constexpr double Cell=4.0, Inf=1e12;
std::array<Field, 8> cache;
int Index(double v) { return int(std::floor(v/Cell)); }
void Build(Field& f, double x, double y) {
 f.ox=Index(x)-N/2; f.oy=Index(y)-N/2;
 f.cost.assign(N*N, Inf);
 std::vector<unsigned char> blocked(N*N);
 std::vector<double> weight(N*N, 1.0);
 for(int j=0;j<N;++j) for(int i=0;i<N;++i) {
  const auto m=navmap::At((f.ox+i+0.5)*Cell,(f.oy+j+0.5)*Cell,f.z);
  const int k=j*N+i;
  blocked[k]=m.blocked || (f.objects && m.objBlocked) || (m.haveGround && m.ground>f.z+2.0);
  weight[k]=1.0+(!m.known ? 0.5 : 0.0)+std::min(m.contacts,10)*0.25;
 }
 // Inflate by hull radius and add a soft margin beside obstacles.
 const auto raw=blocked;
 const int radius=int(std::ceil(f.width/Cell))+1;
 for(int j=0;j<N;++j) for(int i=0;i<N;++i) if(raw[j*N+i]) {
  for(int dy=-radius;dy<=radius;++dy) for(int dx=-radius;dx<=radius;++dx) {
   const int a=i+dx,b=j+dy;
   if(a<0||b<0||a>=N||b>=N) continue;
   const double gap=std::hypot(double(dx),double(dy))*Cell;
   if(gap<f.width+2.0) blocked[b*N+a]=1;
   else if(gap<f.width+8.0) weight[b*N+a]+=0.5;
  }
 }
 using Entry=std::pair<double,int>;
 std::priority_queue<Entry,std::vector<Entry>,std::greater<Entry>> q;
 auto seed=[&](int k,double d) { if(!blocked[k] && d<f.cost[k]) { f.cost[k]=d; q.emplace(d,k); } };
 const int gi=Index(f.gx)-f.ox, gj=Index(f.gy)-f.oy;
 if(gi>=0&&gj>=0&&gi<N&&gj<N) {
  // A blocked destination uses the nearest open cell, never a zero-cost wall.
  int best=-1; double distance=Inf;
  for(int j=0;j<N;++j) for(int i=0;i<N;++i) if(!blocked[j*N+i]) {
   const double d=std::hypot((f.ox+i+0.5)*Cell-f.gx,(f.oy+j+0.5)*Cell-f.gy);
   if(d<distance) { distance=d; best=j*N+i; }
  }
  if(best>=0) seed(best,distance);
 } else {
  for(int j=0;j<N;++j) for(int i=0;i<N;++i) if(i==0||j==0||i==N-1||j==N-1)
   seed(j*N+i,std::hypot((f.ox+i+0.5)*Cell-f.gx,(f.oy+j+0.5)*Cell-f.gy));
 }
 while(!q.empty()) {
  const auto [d,k]=q.top(); q.pop(); if(d!=f.cost[k]) continue;
  const int i=k%N,j=k/N;
  for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
   if(!dx&&!dy) continue;
   const int a=i+dx,b=j+dy;
   if(a<0||b<0||a>=N||b>=N||blocked[b*N+a]) continue;
   if(dx&&dy&&(blocked[j*N+a]||blocked[b*N+i])) continue;
   seed(b*N+a,d+Cell*(dx&&dy ? 1.4142135623730951 : 1.0)*(weight[k]+weight[b*N+a])*0.5);
  }
 }
 f.built=GetTickCount64();
}
}
double Field::Value(double x,double y) const {
 const int i=Index(x)-ox,j=Index(y)-oy;
 if(i<0||j<0||i>=N||j>=N||cost.empty()) return Inf;
 // Continuous within a cell so small rollout movements still have a gradient.
 double best=Inf;
 for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
  const int a=i+dx,b=j+dy;
  if(a<0||b<0||a>=N||b>=N) continue;
  if(dx&&dy&&(cost[j*N+a]>=Inf||cost[b*N+i]>=Inf)) continue;
  if(cost[j*N+i]>=Inf) return Inf;
  best=std::min(best,cost[b*N+a]+std::hypot(x-(ox+a+0.5)*Cell,y-(oy+b+0.5)*Cell));
 }
 return best;
}
const Field& Get(double x,double y,double z,double gx,double gy,double width,bool objects) {
 const auto now=GetTickCount64();
 Field* slot=nullptr;
 for(auto& f:cache) if(f.built && std::hypot(f.gx-gx,f.gy-gy)<2.0 && std::fabs(f.z-z)<1.0 &&
  std::fabs(f.width-width)<0.25 && f.objects==objects && std::abs(Index(x)-f.ox-N/2)<40 && std::abs(Index(y)-f.oy-N/2)<40) { slot=&f; break; }
 if(!slot) slot=&*std::min_element(cache.begin(),cache.end(),[](const Field& a,const Field& b){return a.built<b.built;});
 if(!slot->built || now-slot->built>=1000 || std::hypot(slot->gx-gx,slot->gy-gy)>=2.0 ||
  std::fabs(slot->z-z)>=1.0 || std::fabs(slot->width-width)>=0.25 || slot->objects!=objects ||
  std::abs(Index(x)-slot->ox-N/2)>=40 || std::abs(Index(y)-slot->oy-N/2)>=40) {
  slot->gx=gx; slot->gy=gy; slot->z=z; slot->width=width; slot->objects=objects; Build(*slot,x,y);
 }
 return *slot;
}
void Reset() { for(auto& f:cache) f=Field{}; }
}
