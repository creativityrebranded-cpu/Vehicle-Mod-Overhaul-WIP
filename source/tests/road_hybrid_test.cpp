#include "trafficinitial.h"
#include "routeeditor.h"
#include "routefiles.h"
#include "trafficcoordinator.h"
#include <cstdio>
int main(){int checks=0,fail=0;auto check=[&](bool yes,const char* name){++checks;if(!yes){++fail;std::printf("FAIL %s\n",name);}};
    trafficdata::Route r;r.ring=false;r.closed=false;r.name="road";r.lanes=2;r.corridor=8;r.points={{0,0,0},{100,0,0},{100,100,0}};r.road.enabled=true;r.road.tiles.resize(3);
    auto line=trafficroute::Build(r);check(!line.error&&line.Count()==3&&line.road.size()==3,"sharp road uses shared miter boundaries");
    check(line.road[1].left.x==92&&line.road[1].left.y==8,"joined prisms meet at the shared miter");
    check(line.lane[0][0].y==-4&&line.lane[1][0].y==4,"forward and reverse lanes follow road halves");
    r.road.tiles[0].connector=1;auto bend=trafficroute::Build(r);check(!bend.error&&bend.Count()>3&&bend.road.size()==std::size_t(bend.Count()),"cylindrical bend is shared by preview and route");
    r.points.resize(2);r.road.tiles[0]={0,1,20,2};r.road.height=5;line=trafficroute::Build(r);
    check(line.road.front().divider.y==-8&&line.road.back().divider.y==8,"swing reaches opposite road edges");
    check(line.road.front().top.z==5&&line.road.front().top.y>line.road.front().divider.y,"tilted divider rises across the road");
    auto disabled=r.road;disabled.enabled=false;auto restored=roadplan::Read(roadplan::Write(disabled));check(!restored.enabled&&restored.tiles.size()==disabled.tiles.size()&&restored.height==disabled.height,"disabled roads retain editable data");
    check(roadplan::Read("1 0 1 0 0 0 2").enabled,"early tile format remains readable");
    const auto text=roadplan::Write(r.road);check(roadplan::Write(roadplan::Read(text))==text,"tile settings round trip");
    auto unknown=roadplan::Read("9 future payload");check(roadplan::Write(unknown)=="9 future payload"&&!roadplan::Valid(unknown),"future records retained and refused safely");
    check(!roadplan::Valid(roadplan::Read("1 2 1 1 1 0 2")),"out of bounds divider is refused");
    auto closed=r;closed.closed=true;closed.points={{0,0,0},{100,0,0},{100,100,0},{0,100,0}};closed.road.tiles.assign(4,{});closed.road.tiles[3].side=.5;
    auto loop=trafficroute::Build(closed);check(!loop.error&&loop.Count()==4&&loop.road.back().divider.x!=0,"closed road uses its final tile");
    auto empty=r;empty.points.clear();check(trafficroute::Build(empty).error!=nullptr,"empty road is rejected without unsigned underflow");
    roadplan::Plan reversed=closed.road;roadplan::Reverse(reversed,4,true);roadplan::Reverse(reversed,4,true);check(roadplan::Write(reversed)==roadplan::Write(closed.road),"double reverse restores closed tile mapping");
    routeeditor::Draft draft(closed);draft.Remove(1);check(draft.Value().road.tiles.size()==3,"delete remaps road records");draft.Undo();check(draft.Value().road.tiles.size()==4,"undo retains road records");
    auto cut=routeeditor::CutRoute(closed,{1});check(cut.pieces.size()==1&&cut.pieces[0].road.tiles.size()==5,"split retains closing tile and opened endpoints");
    routefiles::File file;file.worldId="Fixture";routefiles::Route saved;saved.id="road";saved.points=r.points;saved.road=r.road;file.routes.push_back(saved);
    trafficdata::Data loaded;trafficdata::Read(routefiles::Format(file),"Editor_fixture.ini",loaded);check(loaded.routes.size()==1&&roadplan::Write(loaded.routes[0].road)==text,"real editor writer and runtime loader preserve roads");
    r.points={{0,0,0},{100,0,0},{100,200,0}};r.road.tiles.assign(3,{});r.road.height=0;
    const auto slots=trafficinitial::Route(r,16,{{1.5,20}});check(slots.size()==16,"eligible spans retain requested population");
    line=trafficroute::Build(r);for(const auto& slot:slots){const auto source=line.ToSource(slot.u);check(std::fabs(source-1)>.15,"birth stays outside corner buffer");check(std::fabs(source-1.5)>.09,"birth stays outside junction buffer");}
    FvoTrafficPolicyV1 policy;check(trafficcoordinator::NexusPilot(&policy,false,false),"legacy paths choose Nexus pilot");policy.movement=1;check(!trafficcoordinator::NexusPilot(&policy,false,false),"locked lanes retain explicit control");policy.movement=0;check(!trafficcoordinator::NexusPilot(&policy,true,false)&&!trafficcoordinator::NexusPilot(&policy,false,true),"explicit travel and spatial constraints retain control");
    std::printf("road hybrid: %d checks, %d failures\n",checks,fail);return fail?1:0;
}
