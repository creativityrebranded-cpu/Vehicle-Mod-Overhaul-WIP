#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Policies for the gaps the native game does not implement for hovering vehicles.
// No engine layouts or executable addresses belong in this file.
namespace integrationlogic {
enum class Controller : std::uint8_t { Navigation, Task, Taxi, Search, Combat, Rescue, Order, Ram, Wreck };
inline Controller ControllerFor(bool wreck, bool ram, bool order, bool rescue, bool combat, bool search, bool taxi, bool task) {
    return wreck ? Controller::Wreck : ram ? Controller::Ram : order ? Controller::Order : rescue ? Controller::Rescue :
        combat ? Controller::Combat : search ? Controller::Search : taxi ? Controller::Taxi : task ? Controller::Task : Controller::Navigation;
}
inline double Clearance(int heightMode, double requested, double hullHeight, double normal) {
    const double minimum = std::max(1.0, hullHeight + 1.5);
    const double base = requested > 0 && std::isfinite(requested) ? std::max(requested, minimum) : std::max(normal, minimum);
    return heightMode == 1 ? std::max(minimum, requested > 0 ? requested : minimum) : heightMode == 2 ? std::max(base, minimum + 12.0) : base;
}
inline double Wrap(double a) { return std::remainder(a, 6.283185307179586); }
inline double ArrivalYaw(double heading, double requested, double distance, double stop, double speed, double maxTurn) {
    return std::isfinite(requested) && distance <= stop + 3.0 && speed < 2.0 ?
        std::clamp(Wrap(requested - heading) * 2.0, -maxTurn, maxTurn) : NAN;
}
struct Box { double p[3]{}, axis[3][3]{{1,0,0},{0,1,0},{0,0,1}}, half[3]{1,1,1}; };
inline bool Valid(const Box& b) {
    for (int i=0;i<3;++i) {
        if (!std::isfinite(b.p[i]) || !std::isfinite(b.half[i]) || b.half[i] <= 0) return false;
        double n=0; for (int k=0;k<3;++k) { if(!std::isfinite(b.axis[i][k])) return false; n+=b.axis[i][k]*b.axis[i][k]; }
        if (std::fabs(n-1.0)>0.02) return false;
    }
    for(int i=0;i<3;++i)for(int j=i+1;j<3;++j){double d=0;for(int k=0;k<3;++k)d+=b.axis[i][k]*b.axis[j][k];if(std::fabs(d)>0.02)return false;}
    return true;
}
// A horizontal escape outside the hull, including rolled/pitched and unusually wide vehicles.
// Keep the current altitude: the engine handles falling after the seat is released.
inline bool EjectPosition(const Box& hull, double heading, int side, double out[3]) {
    if (!Valid(hull) || !std::isfinite(heading)) return false;
    const double dx = std::cos(heading) * (side < 0 ? -1 : 1);
    const double dy = -std::sin(heading) * (side < 0 ? -1 : 1);
    double radius = 0;
    for (int i = 0; i < 3; ++i) radius += std::fabs(dx * hull.axis[i][0] + dy * hull.axis[i][1]) * hull.half[i];
    out[0] = hull.p[0] + dx * (radius + 1.5);
    out[1] = hull.p[1] + dy * (radius + 1.5);
    out[2] = hull.p[2];
    return true;
}
// Full oriented-box separating-axis test, including pitch/roll. The tolerance is
// centimetres of solver separation, not a radius around the chassis.
inline bool Touching(const Box& a, const Box& b, double tolerance=0.08) {
    if(!Valid(a)||!Valid(b))return false;
    auto separated=[&](const double axis[3]) {
        double n=0,d=0,ra=0,rb=0;
        for(int k=0;k<3;++k){n+=axis[k]*axis[k];d+=(b.p[k]-a.p[k])*axis[k];}
        if(n<1e-10)return false;
        for(int i=0;i<3;++i){double x=0,y=0;for(int k=0;k<3;++k){x+=a.axis[i][k]*axis[k];y+=b.axis[i][k]*axis[k];}ra+=std::fabs(x)*a.half[i];rb+=std::fabs(y)*b.half[i];}
        return std::fabs(d)>ra+rb+tolerance*std::sqrt(n);
    };
    for(int i=0;i<3;++i)if(separated(a.axis[i])||separated(b.axis[i]))return false;
    for(int i=0;i<3;++i)for(int j=0;j<3;++j){double c[3]{a.axis[i][1]*b.axis[j][2]-a.axis[i][2]*b.axis[j][1],a.axis[i][2]*b.axis[j][0]-a.axis[i][0]*b.axis[j][2],a.axis[i][0]*b.axis[j][1]-a.axis[i][1]*b.axis[j][0]};if(separated(c))return false;}
    return true;
}
inline bool SweptTouch(Box a, Box b, const Box& beforeA, const Box& beforeB, double dt) {
    if(!(dt>0&&dt<=0.2)||!Valid(beforeA)||!Valid(beforeB))return false;
    double travel=0;
    for(int k=0;k<3;++k){const double d=(a.p[k]-beforeA.p[k])-(b.p[k]-beforeB.p[k]);travel+=d*d;}
    if(travel>625.0)return false; // teleport/rebase is not a collision
    const int count=std::clamp(int(std::ceil(std::sqrt(travel)/0.15)),1,128);
    const Box endA=a,endB=b;
    for(int i=0;i<=count;++i){const double t=double(i)/count;for(int k=0;k<3;++k){a.p[k]=beforeA.p[k]+(endA.p[k]-beforeA.p[k])*t;b.p[k]=beforeB.p[k]+(endB.p[k]-beforeB.p[k])*t;}if(Touching(a,b))return true;}
    return false;
}
inline bool Impact(double beforeClosing, double afterClosing, bool solverImpact) {
    return std::isfinite(beforeClosing) && beforeClosing>=4.0 &&
        (solverImpact || (std::isfinite(afterClosing)&&beforeClosing-afterClosing>=std::max(1.5,beforeClosing*0.2)));
}
struct MotionFence { bool emergency=false, wall=false, person=false, traffic=false; double cap=0,speed=0; };
inline bool MovementAllowed(const MotionFence& f, bool rolloutSafe) {
    return rolloutSafe&&!f.emergency&&!f.wall&&!f.person&&!f.traffic&&std::isfinite(f.cap)&&f.cap>0;
}
}
