#include "roadvisual.h"
#include <cstdio>
int main(){int checks=0,fail=0;auto check=[&](bool yes,const char* name){++checks;if(!yes){++fail;std::printf("FAIL %s\n",name);}};
    trafficdata::Route r;r.points={{0,0,0},{100,0,0},{200,0,0}};r.speed=25;r.travel=routepolicy::Travel::Hover;
    check(roadvisual::TravelTint(routepolicy::Travel::Hover)==0x6196AB,"hover keeps the old floor colour");
    check(roadvisual::TravelTint(routepolicy::Travel::Drive)!=roadvisual::TravelTint(routepolicy::Travel::Hover)&&roadvisual::TravelTint(routepolicy::Travel::Both)!=roadvisual::TravelTint(routepolicy::Travel::Drive)&&roadvisual::TravelTint(routepolicy::Travel::Both)!=roadvisual::TravelTint(routepolicy::Travel::Hover),"three travel tints differ");
    check(roadvisual::TravelTint(routepolicy::Travel::Inherit)==roadvisual::kHoverTint,"inherit reads as hover");
    check(roadvisual::TileTravel(r,0)==routepolicy::Travel::Hover,"no sections: route travel");
    r.sections.resize(2);r.sections[1].travel=routepolicy::Travel::Drive;r.sections[1].speed=12;
    check(roadvisual::TileTravel(r,1)==routepolicy::Travel::Drive&&roadvisual::TileTravel(r,0)==routepolicy::Travel::Hover,"tile travel overrides");
    check(roadvisual::TileSpeed(r,1)==12&&roadvisual::TileSpeed(r,0)==25&&roadvisual::TileSpeed(r,9)==25,"tile speed falls back to the route");
    check(roadvisual::TileTag(r,0).empty(),"plain tile has no tag");
    check(roadvisual::TileTag(r,1)=="Drive, 12 m/s","tag names travel and speed");
    r.sections[0].speed=25;check(roadvisual::TileTag(r,0).empty(),"same speed as route: no tag");
    r.road.enabled=true;r.road.boxes.resize(2);r.road.boxes[0].solids=1;r.road.boxes[0].yaw=10;r.road.boxes[0].width=12;
    check(roadvisual::SolidsAllowed(r,0)&&!roadvisual::SolidsAllowed(r,1),"solids allowed read");
    check(roadvisual::TileTag(r,0)=="solids ok, box W12, yaw 10","box tag");
    r.road.tiles.resize(2);r.road.tiles[1].connector=1;r.road.tiles[1].tilt=15;
    check(roadvisual::TileTag(r,1).find("bend")!=std::string::npos&&roadvisual::TileTag(r,1).find("div tilt")!=std::string::npos,"join and divider tilt");
    check(roadvisual::TileOf(-1,3)==0&&roadvisual::TileOf(1.5,3)==1&&roadvisual::TileOf(99,3)==2&&roadvisual::TileOf(5,0)==0,"tile of source position");
    std::vector<roadvisual::Stop> s;
    roadvisual::StopsAlong({100,100},50,25,100,s);
    check(s.size()==4&&s[0].segment==0&&std::fabs(s[0].t-.25)<1e-9&&s[2].segment==1&&std::fabs(s[2].t-.25)<1e-9,"stops every spacing across segments");
    roadvisual::StopsAlong({100,100},1,0,10,s);check(s.size()<=11&&s.size()>=9,"spacing grows to respect the cap");
    roadvisual::StopsAlong({},10,0,10,s);check(s.empty(),"empty chain");
    roadvisual::StopsAlong({0,0,40},10,5,100,s);check(s.size()==4&&s[0].segment==2,"zero-length segments skipped");
    roadvisual::StopsAlong({10},0,0,10,s);check(s.empty(),"zero spacing gives nothing");
    roadvisual::LegendFlags f;f.drive=true;f.edges=true;auto text=roadvisual::Legend(f);
    check(text.find("green floor")!=std::string::npos&&text.find("amber")!=std::string::npos&&text.find("blue")==std::string::npos,"legend lists only what is drawn");
    std::printf("roadvisual_test: %d checks, %d failed\n",checks,fail);return fail?1:0;}
