#include "npcactivity.h"
#include <cstdio>
#include <limits>
int main(){
    using namespace npcactivity;
    int checks=0,failures=0;auto check=[&](bool b,const char* why){++checks;if(!b){++failures;std::printf("FAIL: %s\n",why);}};
    Observation o{1,2,3,4,1000,0,0,0,18,true,true,true};Entry e;
    e.Add(o);check(e.status==Status::Observing,"one stationary observation cannot establish inactivity");
    for(unsigned i=1;i<=6;++i){o.at=1000+i*10000;e.Add(o);}
    check(e.status==Status::Low&&e.samples==7,"six sampled quiet intervals establish a review candidate");
    check(e.Current(76000)&&!e.Current(76001),"unloaded observations expire at the maximum sampling gap");
    e.Add(o);check(e.samples==7,"duplicate timestamps do not inflate sample evidence");
    o.at+=10000;o.x=20;e.Add(o);check(e.status==Status::Active&&e.quiet==o.at,"movement immediately revokes low activity");
    o.at+=10000;++o.package;e.Add(o);check(e.status==Status::Active&&e.changes==1,"package changes revoke quiet status");
    o.at+=10000;o.package=0;e.Add(o);check(e.status==Status::Unknown&&e.samples==0,"missing package is unknown, not idle");
    o.package=4;o.at+=10000;e.Add(o);check(e.status==Status::Observing&&e.samples==1,"valid data after an unknown reading starts new evidence");
    o.at+=16000;e.Add(o);check(e.samples==1&&e.status==Status::Observing,"sampling gaps cannot count as idle time");
    o.at-=100;e.Add(o);check(e.samples==1,"clock reversal resets evidence");
    o.at+=10000;o.free=false;e.Add(o);check(e.status==Status::Excluded,"protected busy or combat actor cannot qualify");
    o.at+=10000;o.free=true;e.Add(o);check(e.samples==1,"release from exclusion starts a new observation window");
    for(unsigned i=0;i<7;++i){o.at+=10000;o.crowd=false;e.Add(o);}
    check(e.status==Status::Excluded,"low movement does not qualify an unrecognized template");
    o.crowd=true;o.at+=10000;o.base=99;e.Add(o);check(e.samples==1,"base replacement under the same actor id resets evidence");
    o.at+=10000;o.cell=42;e.Add(o);check(e.samples==1,"cell changes do not join unrelated coordinates");
    o.at+=10000;o.x+=500;e.Add(o);check(e.status==Status::Unknown&&e.samples==1,"teleports and rebases invalidate a quiet window");
    o.at+=10000;o.x=std::numeric_limits<double>::quiet_NaN();e.Add(o);
    check(e.status==Status::Unknown,"invalid coordinates cannot qualify");
    o.x=0;o.actor=0;e.Add(o);check(e.samples==0,"missing identity is unknown");
    Census<2> c;o.actor=1;o.at=0;check(c.Observe(o)!=nullptr,"zero clock origin is a valid first sample");
    o.actor=2;check(c.Observe(o)!=nullptr,"fixed table stores independent identities");
    o.actor=3;check(!c.Observe(o),"full table drops observations without evicting live evidence");
    o.at=kStaleMs+1;check(c.Observe(o)!=nullptr,"expired entries are reclaimed");
    check(!c.Entries()[0].Current(0),"future timestamps are not current");
    c.Reset();check(!c.Entries()[0].last.actor&&!c.Entries()[1].last.actor,"load reset discards all old identities");
    Entry sparse;o={1,2,3,4,0,0,0,0,18,true,true,true};
    for(unsigned i=0;i<=4;++i){o.at=i*15000;sparse.Add(o);}
    check(sparse.status!=Status::Low,"one minute with too few samples is insufficient evidence");
    o.at=61000;sparse.Add(o);check(sparse.status==Status::Low,"additional valid sample completes sparse evidence");
    o.at=62000;o.packageType=-1;sparse.Add(o);check(sparse.status==Status::Unknown,"unknown package type revokes eligibility evidence");
    std::printf("npcactivity: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
