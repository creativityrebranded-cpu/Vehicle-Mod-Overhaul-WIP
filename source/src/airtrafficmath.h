#pragma once
#include "ringlogic.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Vehicle schedules exist independently of loaded references or occupants.
// Each rotation gives exactly half the slots one staggered pad visit; the
// other cohort circulates. Both cohorts rejoin before exchanging roles.
namespace airtrafficmath {
constexpr unsigned kFleet=16,kHalf=kFleet/2;
constexpr double kCruise=20.0;
struct Vec {
    double x=0,y=0,z=0;
    Vec operator+(Vec b)const{return{x+b.x,y+b.y,z+b.z};}
    Vec operator-(Vec b)const{return{x-b.x,y-b.y,z-b.z};}
    Vec operator*(double s)const{return{x*s,y*s,z*s};}
};
inline double Length(Vec p){return std::hypot(std::hypot(p.x,p.y),p.z);}
inline Vec Limit(Vec p,double max){const auto n=Length(p);return n>max?p*(max/n):p;}
inline bool Valid(Vec p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
struct Pad {bool valid=false;Vec p;double yaw=0;std::uint64_t key=0;};
enum class Phase {Circulating,Descending,Parked,Rising};
inline const char* Name(Phase p){switch(p){case Phase::Descending:return "descending";case Phase::Parked:return "parked";case Phase::Rising:return "rising";default:return "circulating";}}
struct Pose {Vec p,v;double yaw=0;Phase phase=Phase::Circulating;bool visiting=false;};
inline double Lap(const ringlogic::Ring&r){
    const double a=r.radius*r.aspect,b=r.radius;
    return ringlogic::kPi*(3*(a+b)-std::sqrt((3*a+b)*(a+3*b)))/kCruise;
}
inline bool Visiting(unsigned slot,std::int64_t cycle){return ((slot/2)&1u)==unsigned((cycle%2+2)%2);}
inline double Stagger(unsigned slot){const auto rank=(slot/4)*2+(slot%2);return 0.02+0.30*double(rank)/double(kHalf-1);}
inline Pose Cruise(const ringlogic::Ring&r,unsigned slot,double t){
    const double sign=slot%2?-1.0:1.0,rate=2*ringlogic::kPi/Lap(r);
    const double angle=sign*(2*ringlogic::kPi*(double(slot/2)+(slot%2)*0.5)/kHalf+rate*t);
    const auto at=ringlogic::OnLayer(ringlogic::At(r,angle),r,int(slot%2));
    const double a=r.radius*r.aspect,b=r.radius,dx=std::cos(angle)*a*sign*rate,dy=-std::sin(angle)*b*sign*rate;
    const Vec velocity{dx*std::cos(r.rotation)+dy*std::sin(r.rotation),dy*std::cos(r.rotation)-dx*std::sin(r.rotation),0};
    return{{at.x,at.y,at.z},velocity,std::atan2(velocity.x,velocity.y),Phase::Circulating,false};
}
inline Pose Hermite(Vec p0,Vec v0,Vec p1,Vec v1,double u,double seconds){
    u=std::clamp(u,0.0,1.0);const double u2=u*u,u3=u2*u;
    Pose out;out.p=p0*(2*u3-3*u2+1)+v0*((u3-2*u2+u)*seconds)+p1*(-2*u3+3*u2)+v1*((u3-u2)*seconds);
    out.v=(p0*(6*u2-6*u)+v0*((3*u2-4*u+1)*seconds)+p1*(-6*u2+6*u)+v1*((3*u2-2*u)*seconds))*(1/seconds);
    out.yaw=Length(out.v)>0.1?std::atan2(out.v.x,out.v.y):0;return out;
}
inline Pose At(const ringlogic::Ring&r,const Pad& pad,unsigned slot,double t,bool permitVisit=true){
    if(!ringlogic::Valid(r)||slot>=kFleet||!std::isfinite(t)||std::fabs(t)>1e13)return{};
    auto out=Cruise(r,slot,t);
    if(!pad.valid||!permitVisit||!std::isfinite(t))return out;
    const auto lap=Lap(r);const auto cycle=std::int64_t(std::floor(t/lap));
    if(!Visiting(slot,cycle))return out;
    const double begin=(double(cycle)+Stagger(slot))*lap,down=.22*lap,dwell=.18*lap,up=.22*lap,end=begin+down+dwell+up;
    if(t<begin||t>=end)return out;
    out.visiting=true;const auto initial=Cruise(r,slot,begin),last=Cruise(r,slot,end);
    const double deck=std::max({initial.p.z,last.p.z,pad.p.z+30});const Vec above{pad.p.x,pad.p.y,deck};
    const double elapsed=t-begin;
    if(elapsed<down){
        const double horizontal=.65*down;
        out=elapsed<horizontal?Hermite(initial.p,initial.v,above,{},elapsed/horizontal,horizontal):
            Hermite(above,{},pad.p,{},(elapsed-horizontal)/(down-horizontal),down-horizontal);
        out.phase=Phase::Descending;
    }else if(elapsed<down+dwell){out.p=pad.p;out.v={};out.yaw=pad.yaw;out.phase=Phase::Parked;}
    else{
        const double local=elapsed-down-dwell,vertical=.35*up;
        out=local<vertical?Hermite(pad.p,{},above,{},local/vertical,vertical):
            Hermite(above,{},last.p,last.v,(local-vertical)/(up-vertical),up-vertical);
        out.phase=Phase::Rising;
    }
    out.visiting=true;
    if(Length(out.v)<0.1||std::hypot(out.v.x,out.v.y)<0.1)out.yaw=pad.yaw;
    return out;
}
struct Clock {
    bool have=false;double days=0,phase=0;
    double Sample(double observedDays,double scale){
        if(!std::isfinite(observedDays)||observedDays<0||observedDays>1e6||!std::isfinite(scale)||scale<.01||scale>10000)return phase;
        if(!have||observedDays<days){have=true;days=observedDays;phase=observedDays*86400/scale;return phase;}
        const double elapsed=(observedDays-days)*86400/scale;days=observedDays;phase+=elapsed;return phase;
    }
};
struct Control {Vec velocity,spin;bool valid=false;};
struct StreamGate {
    bool admitted=false;
    bool Request(double distance,bool loaded){
        if(!std::isfinite(distance)||distance>400){admitted=false;return false;}
        if(loaded){admitted=true;return false;}
        if(distance<=300&&!admitted){admitted=true;return true;}
        return false;
    }
};
inline Control Follow(Vec current,Vec target,Vec velocity,double heading,double yaw,double pitch,double roll,double dt){
    if(!Valid(current)||!Valid(target)||!Valid(velocity)||!std::isfinite(heading)||!std::isfinite(yaw)||
        !std::isfinite(pitch)||!std::isfinite(roll)||dt<=0||dt>.25)return{};
    const Vec error=target-current;if(Length(error)>150)return{};
    const Vec correction=Limit(error*.9,8);
    Vec wanted=Limit(velocity+correction,75);wanted.z=std::clamp(wanted.z,-20.0,20.0);
    return{wanted,{std::clamp(-pitch*2.5,-1.0,1.0),std::clamp(-roll*2.5,-1.0,1.0),std::clamp(ringlogic::Wrap(yaw-heading)*1.8,-1.2,1.2)},true};
}
// Both points use the reference/world frame. Havok's local origin is unrelated
// and must never be subtracted from a reference position or range-checked.
inline Control FollowReference(Vec reference,Vec target,Vec velocity,Vec measuredVelocity,double age,
    double heading,double yaw,double pitch,double roll,double dt){
    if(!Valid(measuredVelocity)||!std::isfinite(age)||age<0||age>.5)return{};
    return Follow(reference+measuredVelocity*age,target+velocity*age,velocity,heading,yaw,pitch,roll,dt);
}
}
