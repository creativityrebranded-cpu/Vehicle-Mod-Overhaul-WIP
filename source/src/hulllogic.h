#pragma once
#include <algorithm>
#include <cmath>
namespace hulllogic {
struct Limits {
    double width=1.25,length=2.3,height=1;
    double yaw=1.6,pitch=0.9,roll=1,acceleration=1,brake=30,lateral=12;
    double YawAt(double speed) const {return std::min(yaw,lateral/std::max(4.0,std::fabs(speed)));}
    double StopDistance(double speed) const {return speed*speed/(2*brake)+length+1;}
};
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
