#include "navfield.h"
#include "navmap.h"
#include <cmath>
#include <cstdio>
namespace { int mode=0; }
namespace navmap {
Sample At(double x,double y,double) {
 Sample s; s.known=true;
 if(mode==1) s.blocked=(x>=-24&&x<=24&&y>=20&&y<=28)|| (std::abs(x)>=20&&std::abs(x)<=28&&y>=-24&&y<=28);
 if(mode==2) s.blocked=true;
 if(mode==3) s.objBlocked=x>=12&&x<=20;
 return s;
}
}
int main() {
 int failures=0;
 auto check=[&](bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name); if(!ok) ++failures;};
 auto value=[](double x,double y){return navfield::Get(0,0,10,0,60,2.5,true).Value(x,y);};
 check(value(0,8)<value(0,0),"open ground descends toward goal");
 const auto* a=&navfield::Get(0,0,10,0,60,2.5,true);
 check(a==&navfield::Get(4,0,10,0,60,2.5,true),"nearby vehicles share field");
 mode=1; navfield::Reset();
 check(value(0,-8)<value(0,8),"U pocket routes away from goal toward exit");
 mode=2; navfield::Reset(); check(value(0,0)>=1e12,"sealed map remains unreachable");
 mode=0; navfield::Reset();
 const auto& far=navfield::Get(0,0,10,1000,0,2.5,true);
 check(far.Value(8,0)<far.Value(0,0),"outside goal uses boundary seeds");
 mode=3; navfield::Reset();
 const auto& wall=navfield::Get(0,0,10,60,0,2.5,true);
 check(wall.Value(16,0)>=1e12,"object wall blocks field");
 const auto& ignored=navfield::Get(0,0,10,60,0,2.5,false);
 check(ignored.Value(16,0)<1e12,"object override has separate cache");
 return failures ? 1 : 0;
}
