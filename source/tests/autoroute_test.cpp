#include "autoroute.h"
#include <cstdio>
#include <string>
// 3D auto-routing fixtures (F7 spec 7d): a wall to go round, a bridge over a road, a tunnel under a deck,
// an unknown area, and a gap too narrow for a wide cross-section.
using namespace autoroute;
int main(){int checks=0,fail=0;auto check=[&](bool yes,const char* name){++checks;if(!yes){++fail;std::printf("FAIL %s\n",name);}};
    auto ground=[](Grid& g,int top){for(int k=0;k<=top;++k)for(int j=0;j<g.ny;++j)for(int i=0;i<g.nx;++i)g.At(i,j,k).state=State::Solid;
        for(int k=top+1;k<g.nz;++k)for(int j=0;j<g.ny;++j)for(int i=0;i<g.nx;++i)g.At(i,j,k).state=State::Free;};
    auto at=[](const Grid& g,int i,int j,int k){return Point{g.X(i),g.Y(j),g.oz+k*kZ+.5};};
    auto crosses=[](const Grid& g,const Result& r,auto&& bad){for(std::size_t n=0;n+1<r.points.size();++n){const auto a=r.points[n],b=r.points[n+1];
        for(int s=0;s<=40;++s){const double f=s/40.0;if(bad(g.I(a.x+(b.x-a.x)*f),g.J(a.y+(b.y-a.y)*f),g.K(a.z+(b.z-a.z)*f+.01)))return true;}}return false;};
    Params drive;drive.halfWidth=1.5;drive.height=3.5;
    {   // 1. A wall with an opening at one end: the route goes round, never through.
        Grid g;g.Resize(0,0,0,30,30,6);ground(g,0);for(int j=0;j<25;++j)for(int k=1;k<=4;++k)g.At(15,j,k).state=State::Solid;
        const auto r=Search(g,at(g,2,10,1),at(g,28,10,1),drive);
        check(r.ok,"wall: a path is found");
        check(!crosses(g,r,[&](int i,int j,int k){return g.Inside(i,j,k)&&g.At(i,j,k).state==State::Solid;}),"wall: the simplified route never enters a solid");
        bool round=false;for(auto q:r.points)round=round||g.J(q.y)>=25;check(round,"wall: the route goes round the open end");
        check(r.points.size()>=3&&r.points.size()<20,"wall: string pulling leaves a few route points");
    }
    {   // 2 + 3. Plateaus joined by a bridge deck over a lower road; the lower road runs under the deck (a tunnel).
        Grid g;g.Resize(0,0,0,30,30,8);ground(g,0);
        for(int j=0;j<30;++j)for(int i=0;i<30;++i)if(i<10||i>20)for(int k=1;k<=3;++k)g.At(i,j,k).state=State::Solid;
        for(int j=13;j<=17;++j)for(int i=10;i<=20;++i)g.At(i,j,3).state=State::Solid;
        const auto bridge=Search(g,at(g,5,15,4),at(g,25,15,4),drive);
        check(bridge.ok,"bridge: the upper road crosses on the deck");
        bool level=true;for(auto q:bridge.points)level=level&&q.z>=g.oz+4*kZ-1e-6;check(level,"bridge: the route stays on the deck, above the lower road");
        const auto tunnel=Search(g,at(g,15,1,1),at(g,15,28,1),drive);
        check(tunnel.ok,"tunnel: the lower road passes under the deck");
        bool under=true;for(auto q:tunnel.points)under=under&&q.z<g.oz+3*kZ;check(under,"tunnel: the lower route stays under the deck");
        auto tall=drive;tall.height=6;check(!Search(g,at(g,15,1,1),at(g,15,28,1),tall).ok,"tunnel: a cross-section taller than the opening is refused");
    }
    {   // 4. An unknown band: wheels need known support (no path); a hover wire crosses it at extra cost and is flagged.
        Grid g;g.Resize(0,0,0,30,10,6);ground(g,0);for(int j=0;j<10;++j)for(int i=12;i<18;++i)for(int k=0;k<6;++k)g.At(i,j,k).state=State::Unknown;
        const auto wheels=Search(g,at(g,2,5,1),at(g,28,5,1),drive);check(!wheels.ok&&std::string(wheels.why)=="no_path","unknown: a drive wire has no path over unknown ground");
        auto hover=drive;hover.drive=false;const auto air=Search(g,at(g,2,5,1),at(g,28,5,1),hover);
        check(air.ok&&air.unknownCells>0,"unknown: a hover wire crosses and counts unknown cells");
        bool flagged=false;for(auto q:air.points)flagged=flagged||q.unknown;check(flagged,"unknown: route points through unknown space carry the warning");
    }
    {   // 5. A 4 m gap in a full-width wall: a narrow cross-section fits, a wide one does not.
        Grid g;g.Resize(0,0,0,30,30,6);ground(g,0);for(int j=0;j<30;++j)if(j!=15)for(int k=1;k<=4;++k)g.At(15,j,k).state=State::Solid;
        auto narrow=drive;narrow.halfWidth=1.5;auto wide=drive;wide.halfWidth=5;
        check(Search(g,at(g,2,15,1),at(g,28,15,1),narrow).ok,"gap: a narrow cross-section passes the gap");
        const auto blocked=Search(g,at(g,2,15,1),at(g,28,15,1),wide);check(!blocked.ok&&std::string(blocked.why)=="no_path","gap: a wide cross-section is refused at the gap");
    }
    {   // Other routes' fields cost a lot and are reported; blocked ends and budgets give reasons.
        Grid g;g.Resize(0,0,0,30,30,6);ground(g,0);for(int j=0;j<30;++j)for(int i=14;i<=16;++i)g.At(i,j,1).field=true;
        const auto r=Search(g,at(g,2,15,1),at(g,28,15,1),drive);check(r.ok&&r.fieldCells>0,"field: an unavoidable crossing is reported for a junction");
        auto tiny=drive;tiny.budget=10;check(std::string(Search(g,at(g,2,15,1),at(g,28,15,1),tiny).why)=="budget","budget: the search stops with a reason");
        g.At(2,15,1).state=State::Solid;check(std::string(Search(g,at(g,2,15,1),at(g,28,15,1),drive).why)=="start_blocked","blocked start is reported");
    }
    std::printf("autoroute: %d checks, %d failures\n",checks,fail);return fail?1:0;
}
