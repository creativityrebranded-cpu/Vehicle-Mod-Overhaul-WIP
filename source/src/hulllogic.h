#pragma once
#include <algorithm>
#include <cmath>
namespace hulllogic {
struct Limits {
    double width=1.25,length=2.3,height=1;
    double yaw=1.6,pitch=0.9,roll=1,acceleration=1,brake=30,lateral=12;
    // 0: the standard model brakes at a constant `brake`. >0: the physical chassis (vtolphysics.h Solve) brakes at
    // min(brake, brakeGain * speed), so the last metres take longer: an exponential tail of speed / brakeGain metres.
    double brakeGain=0;
    double YawAt(double speed) const {return std::min(yaw,lateral/std::max(4.0,std::fabs(speed)));}
    double StopDistance(double speed) const {
        const double v=std::fabs(speed);
        if(brakeGain<=0||brake<=0)return v*v/(2*brake)+length+1;
        const double knee=brake/brakeGain;  // below this speed the braking force falls with the speed
        const double fast=v>knee?(v*v-knee*knee)/(2*brake):0;
        return fast+std::min(v,knee)/brakeGain+length+1;
    }
};
// The physical VTOL chassis (2026-10-24, the user: AI tuned for physical flight). It cannot hold the standard model's grip:
// sideways it has its slip-damping side thrusters (6 m/s^2, vtolai::kSideAcceleration) plus the tilt of a banked collective
// (g tan 0.6 = 6.7 m/s^2 at vtolai::kMaxBank), and the bank takes time to build, so a sustained turn holds about 9 m/s^2.
// Its brake is vtolphysics' min(brake, 2 * speed). Every planner that reads these limits (turn rates, arrival braking, combat
// turn radius) then plans for the momentum the chassis really has.
constexpr double kPhysicalLateral = 9.0, kPhysicalBrakeGain = 2.0;
inline Limits Physical(Limits v) {
    v.lateral=std::min(v.lateral,kPhysicalLateral);
    v.brakeGain=kPhysicalBrakeGain;
    return v;
}
inline Limits For(double width,double length,double height) {
    Limits v;
    if(std::isfinite(width)&&width>0.05)v.width=width;
    if(std::isfinite(length)&&length>0.05)v.length=length;
    if(std::isfinite(height)&&height>0.05)v.height=height;
    const double yawSize=std::sqrt((v.width*v.width+v.length*v.length)/(1.25*1.25+2.3*2.3));
    v.acceleration=std::clamp(1/std::sqrt(std::max(1.0,yawSize)),0.12,1.0);
    v.yaw=std::clamp(1.6/std::sqrt(yawSize),0.025,2.0);
    v.pitch=std::clamp(0.9*std::sqrt((2.3*2.3+1)/(v.length*v.length+v.height*v.height)),0.02,1.0);
    v.roll=std::clamp(std::sqrt((1.25*1.25+1)/(v.width*v.width+v.height*v.height)),0.02,1.2);
    v.brake=30*v.acceleration;v.lateral=12*v.acceleration;
    return v;
}
}
