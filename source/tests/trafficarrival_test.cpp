#include "trafficarrival.h"
#include "trafficinitial.h"
#include <cstdio>
int main(){int fail=0,checks=0;auto check=[&](bool yes,const char* why){++checks;if(!yes){++fail;std::printf("FAIL %s\n",why);}};
    trafficarrival::Reset();check(!trafficarrival::Covered(),"no arrival window before the core opens one");
    trafficarrival::Window(true);check(trafficarrival::Covered(),"the loading-screen window allows arrival placement");
    trafficarrival::Progress(120,40);check(trafficarrival::Covered()&&trafficarrival::expected==120&&trafficarrival::ready==40,"progress never closes the game's own screen window");
    trafficarrival::Window(false);check(!trafficarrival::Covered(),"the window closes with the loading screen, never held open");
    trafficarrival::Window(true);trafficarrival::Reset();check(!trafficarrival::Covered()&&trafficarrival::expected==0,"a new load starts without a window");
    trafficdata::Route path;path.ring=false;path.name="long";path.lanes=1;path.points={{0,0,100},{100,0,100},{100,900,100}};
    const auto slots=trafficinitial::Route(path,4);
    check(slots.size()==4&&std::fabs(slots[0].at.y-25)<0.01&&std::fabs(slots[3].at.y-775)<0.01,"initial population uses physical route length, independent of point density or ground origins");
    path.lanes=2;path.width=20;const auto two=trafficinitial::Route(path,6);
    check(two.size()==6&&two[1].lane==1&&two[0].lane==0&&std::isfinite(two[1].heading),"both authored travel lanes receive finite poses");
    trafficdata::Route ring;ring.name="ring";ring.lanes=2;ring.shape=ringlogic::Clean({0,0,300,600,1,0,40});
    const auto city=trafficinitial::Route(ring,121);check(city.size()==121,"dense initial city is fully planned before gameplay");
    const auto mixed=trafficinitial::Interleave({slots,city});check(mixed.size()==125&&mixed[0].route=="long"&&mixed[1].route=="ring","first initialization passes cover all configured routes");
    const auto capped=trafficinitial::Interleave({city,city,city});check(capped.size()==256,"initial plan respects physical fleet capacity");
    std::printf("traffic arrival: %d checks, %d failures\n",checks,fail);return fail?1:0;
}
