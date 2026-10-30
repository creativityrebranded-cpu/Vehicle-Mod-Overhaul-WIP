#include "routeschedule.h"
#include <cstdio>

int main() {
    using namespace routeschedule;
    int checks=0,failures=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failures;std::printf("FAIL %s\n",name);}};
    Schedule all; all.allDayVehicles=3;
    auto d=Evaluate(all,false,-1);
    check(d.known && d.active && d.vehicles==3,"all-day route does not require a local clock");
    Schedule night; night.allDay=false;night.windows={{18*60,6*60,2}};
    check(Validate(night)==Issue::None,"midnight-spanning window valid");
    check(!Evaluate(night,true,17*60+59).active,"before start inactive");
    check(Evaluate(night,true,18*60).active && Evaluate(night,true,0).active && Evaluate(night,true,5*60+59).active,"night start and both sides of midnight active");
    check(!Evaluate(night,true,6*60).active,"end is exclusive");
    check(!Evaluate(night,false,0).known && !Evaluate(night,true,1440).known,"unknown or invalid local time cannot select a scheduled route");
    night.windows.push_back({6*60,18*60,1});
    check(Validate(night)==Issue::None && Evaluate(night,true,6*60).vehicles==1,"adjacent day/night windows do not overlap");
    night.windows.push_back({5*60,7*60,3});
    check(Validate(night)==Issue::Overlap,"overlap across midnight rejected");
    night.windows={{0,0,1}};check(Validate(night)==Issue::EmptyWindow,"same start/end is not silently all day");
    night.windows={{-1,10,1}};check(Validate(night)==Issue::InvalidTime,"invalid minute rejected");
    constexpr int kV=fvolimits::kVehicles;  // the shared vehicle limit
    night.windows={{0,10,kV+1}};check(Validate(night)==Issue::InvalidDensity,"density limit enforced (the shared vehicle limit)");
    night.windows={{0,10,kV}};check(Validate(night)!=Issue::InvalidDensity,"the shared limit's vehicles allowed");
    {Schedule all; all.allDay=true; all.allDayVehicles=kV; check(Validate(all)==Issue::None && Evaluate(all,false,0).active,"all day at the limit is valid and active");
     all.allDayVehicles=kV+1; check(Validate(all)==Issue::InvalidDensity,"all day over the limit refused");}
    night.windows={{0,10,0}};d=Evaluate(night,true,5);check(d.known && !d.active && d.vehicles==0,"zero density deliberately pauses traffic");
    night.windows.clear();check(Validate(night)==Issue::NoWindows,"scheduled route needs a window");
    Schedule parsed;
    check(ParseWindows("1080 360 2, 360 1080 4",parsed)&&!parsed.allDay&&parsed.windows.size()==2,"daily windows parse with midnight wrap");
    check(!ParseWindows("1080 360 2, 300 600 3",parsed),"overlap parsing rejected");
    check(!ParseWindows("100 200 2,",parsed),"trailing empty window rejected");
    check(!ParseWindows("100 200 2 extra",parsed),"unexpected fields rejected");
    std::printf("routeschedule: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
