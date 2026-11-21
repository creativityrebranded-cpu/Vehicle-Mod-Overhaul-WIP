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
    {
        Schedule sized; sized.allDayVehicles=47;
        check(Evaluate(sized,false,-1,9000).vehicles==47,"saved counts are independent of route length");
        sized.trafficLevel=Quiet;
        check(Evaluate(sized,false,-1,100).vehicles==1 && Evaluate(sized,false,-1,5000).vehicles==10,
              "a short quiet road keeps one vehicle and a long road uses its length");
        sized.trafficLevel=Busy;
        check(Evaluate(sized,false,-1,2000).vehicles==25 && PeakVehicles(sized,2000)==25,
              "busy rate uses the same midpoint target for admission and capacity");
        check(Evaluate(sized,false,-1,100000).vehicles==kV,"long routes remain under the vehicle limit");
        sized.trafficBands[Busy]={0,0};
        check(!Evaluate(sized,false,-1,4000).active,"a zero range stays off");
        night.windows={{1080,360,2},{360,720,14},{720,1080,0}};
        night.trafficLevel=Steady;
        check(Evaluate(night,true,0,2000).vehicles==8 && Evaluate(night,true,600,2000).vehicles==14 &&
              !Evaluate(night,true,800,2000).active && !Evaluate(night,false,0,2000).known &&
              PeakVehicles(night,2000)==14,"scheduled counts fit their scaled range and closed hours remain closed");
        TrafficBands bands=kTrafficBands;
        check(ReadTrafficBands("0 1 6 8 10 12",bands) && bands[Busy]==VehicleBand{10,12},
              "three editable ranges parse");
        check(!ReadTrafficBands("0 1 8 6 10 12",bands) && !ReadTrafficBands("0 1 6 8 10 12 extra",bands) &&
              bands[Busy]==VehicleBand{10,12},"invalid ranges leave the previous values intact");
    }
    {   // busy words become windows; the count eases between zones
        Schedule day; day.peak=20; day.zones={{0,1},{480,4},{1200,0}};
        Expand(day);
        check(Validate(day)==Issue::None&&!day.allDay,"busy words expand to a valid day");
        check(Evaluate(day,true,300).vehicles==5,"Quiet is 25% of the peak");
        check(Evaluate(day,true,600).vehicles==20,"Rush is 100% of the peak");
        const int eased=Evaluate(day,true,485).vehicles;
        check(eased>5&&eased<20,"the count eases into a new zone");
        check(!Evaluate(day,true,1300).active,"Closed flies nothing");
        Schedule one; one.peak=8; one.zones={{360,3}}; Expand(one);
        check(Validate(one)==Issue::None&&Evaluate(one,true,10).vehicles==6&&Evaluate(one,true,800).vehicles==6,"one zone holds all day");
        Schedule few; few.peak=1; few.zones={{0,1}}; Expand(few);
        check(Evaluate(few,true,5).vehicles==1,"a share of a small peak is still one vehicle");
        Schedule read; check(ParseDay("0 quiet, 360 BUSY",read)&&read.zones.size()==2&&read.zones[1].word==3,"day line parses");
        check(FormatDay(read)=="0 quiet, 360 busy","day line round trip");
        check(!ParseDay("0 quiet, 0 busy",read)&&!ParseDay("0 sleepy",read)&&!ParseDay("1500 busy",read)&&!ParseDay("",read),"bad day lines refused");
    }
    std::printf("routeschedule: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
