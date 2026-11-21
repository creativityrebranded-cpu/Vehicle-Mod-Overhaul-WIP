#include "roadfield.h"
#include "routefiles.h"
#include <cstdio>
#include <cstring>
#include <cmath>
int main(){int checks=0,fail=0;auto check=[&](bool yes,const char* name){++checks;if(!yes){++fail;std::printf("FAIL %s\n",name);}};
    trafficdata::Route r;r.ring=false;r.closed=false;r.name="edge";r.lanes=2;r.corridor=8;r.points={{0,0,0},{200,0,0}};r.road.enabled=true;r.road.tiles.resize(2);r.road.width=16;
    // 1. old route text is unchanged and reads edge-free
    const auto before=roadplan::Write(r.road);auto back=roadplan::Read(before);check(back.unsupported.empty()&&back.edgeLeft==0&&back.edgeRight==0&&roadplan::Write(back)==before,"no edges: text unchanged");
    r.road.median=1;const auto med=roadplan::Write(r.road);back=roadplan::Read(med);check(back.unsupported.empty()&&back.median==1&&back.edgeLeft==0&&roadplan::Write(back)==med,"median-only v4 text reads as before");
    // 2. edge round trip, both with and without a median
    r.road.edgeLeft=1.5;r.road.edgeRight=.5;auto text=roadplan::Write(r.road);back=roadplan::Read(text);
    check(back.unsupported.empty()&&back.edgeLeft==1.5&&back.edgeRight==.5&&back.median==1&&roadplan::Write(back)==text,"edges round trip with a median");
    auto only=r.road;only.median=0;text=roadplan::Write(only);back=roadplan::Read(text);check(back.unsupported.empty()&&back.edgeLeft==1.5&&back.edgeRight==.5&&back.median==0,"edges round trip without a median");
    auto bad=r.road;bad.edgeLeft=-1;check(!roadplan::Valid(bad),"negative edge refused");bad.edgeLeft=60;check(!roadplan::Valid(bad),"huge edge refused");
    check(roadplan::Read(text+" 7").unsupported==text+" 7","extra tokens stay unsupported and are kept whole");
    // 3. real file writer / loader
    routefiles::File file;file.worldId="Fixture";routefiles::Route saved;saved.id="edge";saved.points=r.points;saved.road=r.road;file.routes.push_back(saved);
    trafficdata::Data loaded;trafficdata::Read(routefiles::Format(file),"Editor_fixture.ini",loaded);check(loaded.routes.size()==1&&loaded.routes[0].road.edgeLeft==1.5&&loaded.routes[0].road.edgeRight==.5,"route file keeps edge widths");
    // 4. reversal swaps the sides
    auto rev=r.road;roadplan::Reverse(rev,2,false);check(rev.edgeLeft==.5&&rev.edgeRight==1.5,"reverse swaps edges");
    // 5. lane trimming; routes without edges are untouched
    auto plain=r;plain.road.edgeLeft=plain.road.edgeRight=0;plain.road.median=0;
    const auto p=trafficroute::Build(plain),e=trafficroute::Build(r);
    check(!p.error&&!e.error&&p.lane[0][0].y<p.lane[1][0].y,"fixture builds");
    const double plainSpan=std::fabs(p.lane[1][0].y-p.lane[0][0].y),edgeSpan=std::fabs(e.lane[1][0].y-e.lane[0][0].y);
    check(edgeSpan<plainSpan||std::fabs(edgeSpan-plainSpan)<1e-9,"edges never widen the lane spread");
    check(e.edgeLeft==1.5&&e.edgeRight==.5&&p.edgeLeft==0,"line carries the widths");
    // 6. the field refuses points inside a band, accepts the rest
    const auto& s0=e.road[0];const double yRight=s0.boundaries.front().y,yLeft=s0.boundaries.back().y;const double dir=yLeft>yRight?1:-1;
    auto at=[&](double offsetFromRight){return trafficdata::Vec{20,yRight+dir*offsetFromRight,0};};
    check(!roadfield::Point(e,0,at(.25)),"point in the right edge band is out");check(roadfield::Point(e,0,at(1.0)),"point just inside the right band is in");
    const int last=int(e.road[0].boundaries.size())-2;
    check(!roadfield::Point(e,last,at(16-.5)),"point in the left edge band is out");check(roadfield::Point(e,last,at(16-3)),"point inside the left band is in");
    check(roadfield::Point(p,0,at(.25)),"same point on an edge-free route is still in");
    // 7. planner lanes shrink at the outer sides only
    FvoRoadLanesV1 a,b;const bool oka=roadfield::LanesAt(p,0,.5,a),okb=roadfield::LanesAt(e,0,.5,b);
    check(oka&&okb&&a.count==b.count,"lanes answered");
    if(oka&&okb){double lowA=1e9,highA=-1e9,lowB=1e9,highB=-1e9;for(int k=0;k<a.count;++k){lowA=std::min(lowA,a.left[k]);highA=std::max(highA,a.right[k]);lowB=std::min(lowB,b.left[k]);highB=std::max(highB,b.right[k]);}
        check(std::fabs((highB-lowB)-((highA-lowA)-2.0))<1e-6,"the road span loses exactly both bands");}
    // 8. the planner is told the band widths (additive fields); an older caller's smaller struct is never written past its size
    if(oka&&okb){check(a.bandLow==0&&a.bandHigh==0,"no band on an edge-free route: none reported");
        check(std::fabs(b.bandLow+b.bandHigh-2.0)<1e-9&&(b.bandLow==.5||b.bandLow==1.5),"the band widths are reported on the lowest / highest lateral side");
        alignas(8) unsigned char buf[kFvoRoadLanesV1BaseSize+16];std::memset(buf,0xAB,sizeof buf);
        FvoRoadLanesV1 fresh;std::memcpy(buf,&fresh,kFvoRoadLanesV1BaseSize);auto& old=*reinterpret_cast<FvoRoadLanesV1*>(buf);  // what an older core hands over
        old.size=kFvoRoadLanesV1BaseSize;const bool oko=roadfield::LanesAt(e,0,.5,old);
        bool intact=true;for(unsigned i=kFvoRoadLanesV1BaseSize;i<sizeof buf;++i)intact=intact&&buf[i]==0xAB;
        check(oko&&old.size==kFvoRoadLanesV1BaseSize&&old.count==b.count&&old.left[0]==b.left[0],"an older caller (smaller size) gets the lanes and keeps its size");
        check(intact,"an older caller's struct is never written past its own size");}
    // 8. a band wider than a bend's radius may not fold on the inside of the bend: reach is limited there, full elsewhere
    {   trafficdata::Route b;b.ring=false;b.closed=false;b.name="bend";b.lanes=2;b.corridor=20;b.points={{0,0,0},{200,0,0},{200,200,0}};b.road.enabled=true;b.road.tiles.resize(3);b.road.tiles[0].connector=1;b.road.width=11;
        const auto bl=trafficroute::Build(b);check(!bl.error&&bl.road.size()>4,"bend fixture builds");
        double innerMin=1e9,outerMin=1e9,straight=0;
        for(std::size_t i=1;i+1<bl.road.size();++i){const double r=trafficroute::BandReach(bl.road,false,i,true,50),l=trafficroute::BandReach(bl.road,false,i,false,50);innerMin=std::min(innerMin,std::min(r,l));outerMin=std::min(outerMin,std::max(r,l));if(i==1)straight=std::max(r,l);}
        check(innerMin>10&&innerMin<25,"the side a band shrinks toward the bend centre is limited to about 90% of that boundary radius (outer boundary 25.5 m)");check(outerMin==50,"the other side of the bend keeps the full width");check(straight==50,"a straight stretch keeps the full width");
        check(trafficroute::BandReach(bl.road,false,0,true,50)==50&&trafficroute::BandReach(bl.road,false,bl.road.size()-1,true,50)==50,"route ends keep the full width");}
    std::printf("road edge: %d checks, %d failures\n",checks,fail);return fail?1:0;
}
