#include "navfield.h"
#include "navmap.h"
#include <cmath>
#include <cstdio>
namespace { int mode=0; }
namespace navmap {
Sample At(double x,double y,double z) {
 Sample s; s.known=true;
 if(mode==1) s.blocked=(x>=-24&&x<=24&&y>=20&&y<=28)|| (std::abs(x)>=20&&std::abs(x)<=28&&y>=-24&&y<=28);
 if(mode==2) s.blocked=true;
 if(mode==3) s.objBlocked=x>=12&&x<=20;
 if(mode==4)s.objBlocked=x>=12&&x<=20&&z<24;
 if(mode==5)s.objBlocked=z>=11&&z<=13;
 if(mode==6)s.objBlocked=z>=18&&z<=20;
 if(mode==7&&x>=40){s.haveGround=true;s.ground=32;}
 if(mode==8)s.known=false;
 return s;
}
}
int main() {
 int failures=0;
 auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures;};
 auto value=[](double x,double y){const auto&f=navfield::Get(0,0,10,0,60,2.5,true);navfield::Service(10000);return f.Value(x,y);};
 check(value(0,8)<value(0,0),"open ground descends toward goal");
 const auto* a=&navfield::Get(0,0,10,0,60,2.5,true);
 check(a==&navfield::Get(4,0,10,0,60,2.5,true),"nearby vehicles share field");
 mode=1; navfield::Reset();
 check(value(0,-8)<value(0,8),"U pocket routes away from goal toward exit");
 mode=2; navfield::Reset(); check(value(0,0)>=1e12,"sealed map remains unreachable");
 mode=0; navfield::Reset();
 const auto& far=navfield::Get(0,0,10,1000,0,2.5,true);
 navfield::Service(10000);
 check(far.Value(8,0)<far.Value(0,0),"outside goal uses boundary seeds");
 mode=3; navfield::Reset();
 const auto& wall=navfield::Get(0,0,10,60,0,2.5,true);
 navfield::Service(10000);
 check(wall.Value(16,0)>=1e12,"object wall blocks field");
 const auto& ignored=navfield::Get(0,0,10,60,0,2.5,false);
 navfield::Service(10000);
 check(ignored.Value(16,0)<1e12,"object override has separate cache");
 mode=4;navfield::Reset();
 const auto& climb=navfield::Get(0,0,8,60,0,2.5,true,8);navfield::Service(10000);
 check(climb.Value(0,0,16)<climb.Value(0,0,8),"route climbs to cross a finite-height wall");
 mode=5;navfield::Reset();
 const auto& roof=navfield::Get(0,0,8,0,0,2.5,true,24);navfield::Service(10000);
 check(roof.Value(0,0,8)>=1e12,"thin roof between altitude layers cannot be crossed");
 mode=6;navfield::Reset();
 const auto& bridge=navfield::Get(0,0,8,60,0,2.5,true,8);navfield::Service(10000);
 check(bridge.Value(0,0,8)<1e12&&bridge.Value(0,0,24)>=1e12,"route stays under a continuous bridge deck");
 mode=7;navfield::Reset();
 const auto& platform=navfield::Get(0,0,8,60,0,2.5,true,40);navfield::Service(10000);
 check(platform.Value(60,0,40)<platform.Value(0,0,8),"elevated platform has a reachable height-aware destination");
 mode=8;navfield::Reset();
 const auto& unknown=navfield::Get(0,0,8,0,0,2.5,true,24);navfield::Service(10000);
 check(unknown.Value(0,0,8)>=1e12,"unknown altitude columns do not invent a climb path");
 mode=0;navfield::Reset();
 const auto& queued=navfield::Get(0,0,8,60,0,2.5,true,8);
 check(queued.cost.empty(),"new field waits for a complete worker result");
 navfield::Service(10000);check(!queued.cost.empty(),"completed field publishes atomically");
 for(int n=1;n<8;++n){navfield::Get(0,0,8,60+n*40,0,2.5,true,8);navfield::Service(10000);}
 const auto& reused=navfield::Get(0,0,8,-200,0,2.5,true,8);
 check(reused.cost.empty(),"evicted slot never exposes a field for an unrelated goal");
 navfield::Reset();
 return failures ? 1 : 0;
}
