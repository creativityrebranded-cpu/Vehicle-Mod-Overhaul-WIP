#include "trafficroute.h"
#include "routefiles.h"
#include "routeeditor.h"
#include "drivecontrols.h"
#include "routeguidance.h"
#include "panelpaging.h"
#include <cstdio>
#include <chrono>
static FvoCorridorProfileV1 command;
static int Lane(void*,FvoLaneGuidanceV1* out){out->identity=42;out->count=2;out->points[1]={0,20,5};return 1;}
static int Policy(void*,FvoTrafficPolicyV1* out){out->identity=42;out->movement=1;return 1;}
static int Corridor(void*,FvoCorridorProfileV1* out){*out=command;return 1;}
int main(){int total=0,failed=0;auto check=[&](bool pass,const char* name){++total;if(!pass){++failed;std::printf("FAIL %s\n",name);}};
    trafficdata::Route r;r.name="freeform";r.world="#world";r.ring=false;r.lanes=1;r.points={{0,0,0},{0,25,20},{30,55,40},{20,90,10}};r.spacePlan.enabled=true;splinekit::AutoHandles(r.spacePlan,splinekit::Points(r.points),false);
    splinekit::ProfileKey a;a.id=r.spacePlan.nextId++;a.at=.3;a.profile.width=8;a.profile.height=12;
    auto b=a;b.id=r.spacePlan.nextId++;b.at=2.7;b.profile.width=16;b.profile.bank=lanevolume::kPi*2;r.spacePlan.profiles={a,b};
    auto line=trafficroute::Build(r);check(!line.error&&line.Count()>4,"authoring compiles to traffic geometry");
    for(double source:{0.0,.3,.7,1.0,1.5,2.7,3.0})check(std::fabs(line.ToSource(line.ToLine(source))-source)<1e-8,"adaptive source mapping round trip");
    check(std::fabs(trafficroute::SettingsAt(line,line.ToLine(1.5),0).profile.width-12)<1e-8,"traffic resolves the loft at actual source position");
    check(line.basis[0].right.size()==std::size_t(line.Count()),"guidance retains common transported frame");
    auto path=splinekit::LanePath{};path.lane=0;path.anchors=splinekit::Anchors(splinekit::Points(r.points),r.spacePlan);for(auto& p:path.anchors)p.point.x+=35;r.spacePlan.lanes.push_back(path);line=trafficroute::Build(r);check(!line.error&&std::fabs(line.lane[0][0].x-35)<1e-8,"independent lane changes actual traffic centreline");
    auto moved=routeeditor::Translated(r,10,20,30);check(std::fabs(moved.spacePlan.lanes[0].anchors[0].point.z-30)<1e-8&&moved.spacePlan.handles[1].incoming.z==r.spacePlan.handles[1].incoming.z,"whole route translation preserves relative handles and independent geometry");
    auto turned=routeeditor::Turned(r,.5);check(splinekit::Problem(turned.spacePlan,4)==nullptr,"rotation keeps valid freeform metadata");
    routeeditor::Draft draft(r);const auto original=splinekit::Write(r.spacePlan);draft.Remove(1);check(draft.Value().spacePlan.handles.size()==3,"remove anchor updates handle topology");draft.Undo();check(splinekit::Write(draft.Value().spacePlan)==original,"undo restores handles profiles and lane paths");draft.Reverse();draft.Reverse();check(draft.Value().spacePlan.profiles[0].id==r.spacePlan.profiles[0].id&&std::fabs(draft.Value().spacePlan.profiles[0].at-r.spacePlan.profiles[0].at)<1e-8,"double reverse preserves authoring identities");
    auto remap=splinekit::Remap(r.spacePlan,{1,2,3});check(remap.handles.size()==3&&std::any_of(remap.profiles.begin(),remap.profiles.end(),[&](auto key){return key.id==b.id&&std::fabs(key.at-1.7)<1e-8;}),"cut retains fractional profile stations and stable IDs");
    auto inserted=r.spacePlan;splinekit::Insert(inserted,1,{0,12,10});check(splinekit::Problem(inserted,5)==nullptr&&splinekit::Length(splinekit::Sub(inserted.lanes[0].anchors[0].point,inserted.lanes[0].anchors[1].point))>1,"insertion does not duplicate independent anchor positions");
    routefiles::File file;file.worldId="world";routefiles::Route stored;stored.id=r.name;stored.world=r.world;stored.ring=false;stored.points=r.points;stored.lanes=1;stored.spacePlan=r.spacePlan;file.routes.push_back(stored);const auto text=routefiles::Format(file);trafficdata::Data data;trafficdata::Read(text,"fixture.ini",data);check(data.routes.size()==1&&splinekit::Write(data.routes[0].spacePlan)==original,"editor format survives runtime and snapshot reader");
    routefiles::File parsed;std::vector<std::string> problems;check(routefiles::Parse(text,"fixture.ini",parsed,problems)&&routefiles::Format(parsed)==text,"editor read write retains exact toolkit records");
    auto snapshot=data;data.routes[0].spacePlan.handles[0].outgoing.x+=10;check(splinekit::Write(snapshot.routes[0].spacePlan)==original,"snapshot authoring remains immutable across working copy edits");
    stored.spacePlan={};stored.spacePlan.unsupported="9 future toolkit content";file.routes[0]=stored;routefiles::Parse(routefiles::Format(file),"fixture.ini",parsed,problems);check(parsed.routes[0].spacePlan.unsupported==stored.spacePlan.unsupported,"future records survive editor round trip");
    r.spacePlan={};const auto legacy=trafficroute::Build(r);check(!legacy.error&&legacy.Count()==4,"old routes retain old sampling and behavior");
    command.identity=42;command.active=1;command.count=4;command.width=10;command.height=10;command.outline[0]={-.5,0};command.outline[1]={.5,0};command.outline[2]={.5,1};command.outline[3]={-.5,1};
    drive::Input input;input.shape.halfWidth=1;input.shape.halfLength=2;input.shape.height=2;input.shape.originAbove=1;
    check(corridorprofile::Fits(command,input),"model fits a measured polygon envelope");command.width=1;check(!corridorprofile::Fits(command,input),"narrow polygon rejects model bounds");command.width=10;command.right={0,1,0};command.up={0,0,1};command.width=3;check(!corridorprofile::Fits(command,input),"fit includes vehicle length projected onto the cross section");command.width=10;command.right={1,0,0};
    routeguidance::provider=Lane;routeguidance::policyProvider=Policy;routeguidance::corridorProvider=Corridor;int owner=1;check(routeguidance::Capture(1,&owner,10)&&routeguidance::Corridor(1),"optional profile matches captured lane identity");command.identity=43;check(!routeguidance::Capture(1,&owner,20)&&routeguidance::Policy(1)&&!routeguidance::Get(1,20),"mismatched profile holds coordinated ownership");routeguidance::Clear(1);routeguidance::corridorProvider=nullptr;check(routeguidance::Capture(1,&owner,30),"legacy provider omitting profile protocol stays supported");
    auto malformed=r.spacePlan;malformed.enabled=false;malformed.profileSamples=100000000;const auto invalidText=splinekit::Write(malformed);check(splinekit::Read(invalidText).unsupported==invalidText,"malformed disabled records are retained without executable sampling settings");splinekit::Handle linked;linked.incoming={-3,0,0};linked.outgoing={3,0,0};splinekit::SetHandle(linked,false,{0,4,0});check(std::fabs(linked.incoming.y+3)<1e-9,"linked handle edit preserves opposing tangent length");
    const std::vector<panelpaging::Interval> rows{{78,104},{106,132},{134,254},{256,282},{284,310}};const auto pages=panelpaging::Cuts(78,312,170,rows);check(pages.size()>2&&pages[1]==134,"long panels break before a profile preview rather than cutting it");bool whole=true;for(std::size_t i=1;i+1<pages.size();++i)for(auto row:rows)if(row.first<pages[i]&&row.last>pages[i])whole=false;check(whole&&pages.back()==312,"panel paging keeps every row reachable without splitting controls");
    std::printf("freeform integration: %d/%d checks passed\n",total-failed,total);return failed?1:0;
}
