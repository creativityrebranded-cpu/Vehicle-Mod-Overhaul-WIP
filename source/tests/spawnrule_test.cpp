#include "spawnrule.h"
#include "trafficinitial.h"
#include <cstdio>
// When traffic may be created, and backup spots (2026-11-06).
int main(){int checks=0,fail=0;auto check=[&](bool yes,const char* name){++checks;if(!yes){++fail;std::printf("FAIL %s\n",name);}};
    check(spawnrule::Open(3,false),"loading screen opens the window");
    check(spawnrule::Open(2,true),"X under the map opens the window");
    check(!spawnrule::Open(2,false),"the map alone does not");
    check(!spawnrule::Open(1,false)&&!spawnrule::Open(1,true),"play never does, even with X pending");
    check(!spawnrule::Open(0,true),"no presentation never does");
    trafficdata::Route r;r.ring=false;r.closed=false;r.name="road";r.lanes=2;r.corridor=8;r.points={{0,0,0},{400,0,0}};r.road.enabled=true;
    const auto slots=trafficinitial::Route(r,4);check(slots.size()==4,"four cars get four positions");
    bool any=false,lane=true;for(const auto& s:slots){any=any||!s.backups.empty();for(const auto& b:s.backups)lane=lane&&std::hypot(b.at.x-s.at.x,b.at.y-s.at.y)<=18.5;}
    check(any,"road positions have backup spots");check(lane,"backups stay within 18 m along the lane");
    for(const auto& s:slots)for(const auto& b:s.backups)check(b.at.x>=9.9&&b.at.x<=390.1,"backups keep off the route ends");
    auto kilometre=r;kilometre.points={{0,0,0},{1000,0,0}};
    const auto spread=trafficinitial::RoadByKilometre(kilometre,8);
    check(spread.size()==8,"eight vehicles per kilometre keep eight profile slots");
    if(spread.size()==8){
        bool stations=true,directions=true;
        for(std::size_t i=0;i<spread.size();++i){
            stations=stations&&spread[i].eligible&&std::fabs(spread[i].at.x-(i+.5)*125)<.01;
            directions=directions&&spread[i].lane==int(i%2);
        }
        check(stations,"the station axis spaces both lanes together at 125 m");
        check(directions,"station slots alternate travel directions");
        const auto half=trafficinitial::Interleave({spread},4);
        check(half.size()==4&&std::fabs(half[0].at.x-62.5)<.01&&std::fabs(half[3].at.x-812.5)<.01,
            "a capped partial fleet retains alternate slots across the kilometre");
        bool backupsClear=true;
        for(std::size_t i=1;i<spread.size();++i)
            for(const auto& a:spread[i-1].backups)for(const auto& b:spread[i].backups)
                backupsClear=backupsClear&&trafficinitial::GapClear(a.at.x,b.at.x,1000,false,125);
        check(backupsClear,"opposing fallback shifts preserve the 100 m minimum across lanes");
        const auto line=trafficroute::Build(kilometre);
        const auto axis=trafficinitial::RoadStations(line);
        double station=-1;
        const auto lane=spread[1].at;
        check(trafficinitial::ProjectRoadStation(kilometre,line,axis,{lane.x,lane.y,lane.z+1},station)&&std::fabs(station-187.5)<.01,
            "an existing vehicle on the opposite lane projects onto the same station axis");
        check(!trafficinitial::ProjectRoadStation(kilometre,line,axis,{lane.x,lane.y,lane.z+25},station),
            "a vehicle at a different elevation is outside this road field");
    }
    const auto blocked=trafficinitial::RoadByKilometre(kilometre,8,{{.1875,30}});
    check(blocked.size()==8&&!blocked[1].eligible&&std::fabs(blocked[2].at.x-312.5)<.01,
        "a blocked interval stays empty without shifting the other stations");
    auto slope=kilometre;slope.points={{0,0,0},{800,0,600}};
    const auto climbing=trafficinitial::RoadByKilometre(slope,8);
    check(climbing.size()==8&&std::fabs(climbing.front().interval-125)<.01,"station metres include the slope");
    check(!trafficinitial::GapClear(20,970,1000,true,125)&&trafficinitial::GapClear(20,970,1000,false,125),
        "minimum gap crosses the seam only on closed roads");
    // LV-2 (2026-11-20): only a real corner blocks births; a bend of a few degrees is a straight road to a car.
    {trafficdata::Route gentle=r,sharp=r;gentle.closed=sharp.closed=false;gentle.rounding.clear();sharp.rounding.clear();
        gentle.points={{0,0,0},{100,0,0},{200,8,0},{300,8,0}};sharp.points={{0,0,0},{100,0,0},{100,100,0},{200,100,0}};gentle.road.tiles.assign(4,{});sharp.road.tiles.assign(4,{});
        check(trafficinitial::TurnAt(gentle.points,1,false)<trafficinitial::rules::cornerTurnRadians&&trafficinitial::TurnAt(sharp.points,1,false)>1.5,"turn angle: a 4.6 degree bend is gentle, a right angle is not");
        check(trafficinitial::TurnAt(sharp.points,0,false)==0&&trafficinitial::TurnAt(sharp.points,3,false)==0,"turn angle: route ends are never corners");
        const auto g=trafficroute::Build(gentle),s=trafficroute::Build(sharp);
        const auto gb=trafficinitial::RoadBlocks(gentle,g,trafficinitial::RoadStations(g),{}),sb=trafficinitial::RoadBlocks(sharp,s,trafficinitial::RoadStations(s),{});
        check(gb.size()==2&&sb.size()==4,"blocks: only the route ends for gentle bends; ends plus both corners for a right angle");}
    std::printf("spawnrule_test: %d checks, %d failed\n",checks,fail);return fail?1:0;}
