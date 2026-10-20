#pragma once
#include <algorithm>
#include <cmath>
namespace boostlogic {
inline constexpr double kMultiplier=1.75;
struct Meter {double charge=0,delay=0;bool active=false,exhausted=false;};
inline Meter Step(double charge,double delay,bool request,bool exhausted,double dt,double duration,double rechargeDelay,double rechargeTime) {
    Meter m{std::clamp(std::isfinite(charge)?charge:0.0,0.0,1.0),std::max(0.0,delay),false,exhausted};
    if(!request)m.exhausted=false;
    if(request&&!m.exhausted&&m.charge>0) {
        m.active=true;m.delay=0;m.charge=std::max(0.0,m.charge-dt/std::max(0.1,duration));
        if(m.charge==0)m.exhausted=true;
    } else if(request) {m.delay=0;m.exhausted=true;}
    else {
        m.delay+=dt;
        if(m.delay>=rechargeDelay)m.charge=std::min(1.0,m.charge+dt/std::max(0.1,rechargeTime));
    }
    return m;
}
}
