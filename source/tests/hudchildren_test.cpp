#include "hudchildren.h"
#include "huddefaults.h"
#include <sstream>
#include <set>
#include <cstdio>
int main(){
    using namespace hudchildren;
    int checks=0,fail=0;auto check=[&](bool v,const char* t){++checks;if(!v){++fail;std::printf("FAIL %s\n",t);}};
    auto close=[](double a,double b){return std::abs(a-b)<.00001;};
    Layout l;auto* trip=Find(l,true,CardBase+3);
    const auto old=Apply(*trip,130,60,1.5);
    check(old.x==130&&old.y==60&&old.scale==1.5,"legacy layout has unchanged child positions and sizes");
    Move(*trip,10,-3,1.5);trip->scale=1.2;
    auto at=Apply(*trip,130,60,1.5);
    check(close(at.x,140)&&close(at.y,57)&&close(at.scale,1.8),"child nudge is in displayed units at a scaled parent");
    const auto moved=Apply(*trip,180,80,1.5);
    check(close(moved.x-at.x,50)&&close(moved.y-at.y,20),"group translation carries child arrangement");
    const auto grown=Apply(*trip,130,60,3);
    check(close(grown.x-130,20)&&close(grown.y-60,-6)&&close(grown.scale,3.6),"group resize scales child offset and size together");
    check(l.card[1][2].x==0&&l.card[0][3].x==0,"child edits leave siblings and other mode untouched");
    l.route[15]={-12,40,.75};l.actions[2]={123,-2,2};
    std::istringstream in(Save(l));std::string line;Layout loaded;
    while(std::getline(in,line))check(Load(line,loaded),"saved child entry loads");
    check(close(loaded.card[1][3].x,trip->x)&&loaded.card[1][3].scale==1.2,"card child survives save/reload");
    check(loaded.route[15].y==40&&loaded.actions[2].x==123,"last escort and action survive save/reload");
    for(const char* bad:{"child_car_-1 = 0 0 1","child_car_8 = 0 0 1","child_route_16 = 0 0 1","child_action_3 = 0 0 1","child_car_3 = nan 0 1","child_car_3 = 0 inf 1","child_car_3 = 0 0 nan","cluster = 1 2 3"})
        check(!Load(bad,loaded),"invalid/legacy keys cannot alter children");
    check(loaded.card[1][3].scale==1.2,"malformed inputs preserve prior position");
    check(Load("child_car_3 = 1 2 100",loaded)&&loaded.card[1][3].scale==2.5,"child size is bounded");
    *trip={};check(trip->x==0&&trip->y==0&&trip->scale==1&&l.route[15].x==-12,"reset restores only selected child");
    std::set<std::string> names;
    for(int s=0;s<750;++s){auto* p=Find(l,false,s);check(bool(p)==IsSelection(s),"selection bounds match lookup");if(p)check(!Name(s).empty()&&names.insert(Name(s)).second,"child selection names are unique");}
    check(names.size()==27,"all eight status, sixteen route and three action slots are selectable");
    const auto defaults=huddefaults::Children();
    check(defaults.card[1][2].x==-400&&close(defaults.card[1][2].y,407.407407),"fresh layout retains the user's individually moved health line");
    check(defaults.card[0][2].x==0&&defaults.card[0][2].y==0,"foot health placement remains independent");
    auto customized=defaults;
    check(Load("child_car_2 = 0 0 1",customized)&&customized.card[1][2].x==0,"an explicit saved neutral offset overrides the new nonzero default");
    customized.card[1][2]=huddefaults::Child(true,CardBase+2);
    check(customized.card[1][2].x==-400,"editor reset returns to the captured layout rather than zero");
    check(huddefaults::Children().card[1][2].x==-400,"saved edits cannot mutate the shared default preset");
    check(std::string(huddefaults::Layout).find("rides =")==std::string::npos,"personal ride history is not shipped as a default");
    check(std::string(huddefaults::Layout).find("physicalVtol")==std::string::npos,"UI preset cannot enable physical flight");
    std::printf("HUD children: %d checks, %d failures\n",checks,fail);return fail?1:0;
}
