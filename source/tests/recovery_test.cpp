#include "recovery.h"
#include "sitelogic.h"
#include <cstdio>
#include <array>
int main(){int failures=0;auto check=[&](bool ok,const char*s){std::printf("%s %s\n",ok?"PASS":"FAIL",s);if(!ok)++failures;};
 recovery::Budget b;b.Update(1,0,0,0,100,0,0,false,false);
 check(b.Update(20001,0,0,0,100,0,0,false,false)&&b.attempts==1,"stationary trip gets first bounded retry");
 check(b.Update(40001,0,0,0,100,0,0,false,false)&&b.attempts==2,"stationary trip gets second bounded retry");
 b.Update(60001,0,0,0,100,0,0,false,false);check(b.failed&&b.attempts==3,"third failed attempt stops recovery");
 b.Update(90001,50,0,0,100,0,0,false,false);check(b.failed,"movement cannot silently restart a failed trip");
 b.Reset();b.Update(100000,0,0,0,100,0,0,false,false);check(!b.failed&&b.attempts==0,"explicit retry resets the budget");
 b.Update(200000,0,0,0,100,0,0,false,true);check(!b.failed&&b.attempts==0,"waiting for terrain consumes no attempt");
 b.Update(200100,0,0,5,100,0,0,false,false);check(b.watch==200100,"vertical progress prevents false stuck detection");
 b.Update(200200,0,0,5,150,0,0,false,false);check(b.since==200200&&b.attempts==0,"new schedule goal starts a new budget");
 std::array<double,9> floor{};check(sitelogic::Support(floor),"level footprint supports parking");floor[0]=-4;check(!sitelogic::Support(floor),"drop at one wheel rejects parking");
 check(!sitelogic::Standing(0,0,8,2.5),"moving vehicle cannot unload passengers");
 check(!sitelogic::Standing(0,0,0,30),"stationary airborne vehicle cannot unload passengers");
 return failures?1:0;}
