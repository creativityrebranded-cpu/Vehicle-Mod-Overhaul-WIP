#pragma once
#include <algorithm>
#include <cmath>

// The wheeled car as Havok drives it (2026-11-04, measured in game, not guessed).
// Starfield vehicles are hknpVehicleInstance. FUN_141324650 runs a per-vehicle PID that moves the driver input
// toward the commanded steering (clamped to +-1); Havok's default analog input shapes it (initialSlope up to
// slopeChangePointX, then linear to 1); default steering turns it into the wheel angle:
//     angle = Shaped(input) * maxSteeringAngle
// Confirmed on 38 live samples, largest error 0.0002 rad (speeds up to 24.5 m/s). Above maxSpeedFullSteeringAngle
// (45 m/s on every measured model) Havok reduces the angle; that branch is not yet observed in game.
// Measured models: FE00900A/FE001033 lock 0.7854 wheelbase 3.344; FE004001 0.6981 / 3.430; FE005005 0.5236 / 3.633.
namespace carmodel {
struct Car {
    double lock=0;            // maxSteeringAngle (rad); 0 = unknown, use the fallback
    double wheelbase=0;       // front to rear axle (m); 0 = unknown
    double slopePoint=.8,initialSlope=.7;
    double fullSpeed=45;      // maxSpeedFullSteeringAngle (m/s)
    bool Known() const {return lock>0&&wheelbase>0;}
};
// Havok default analog driver input: the steering-wheel shaping.
inline double Shaped(double x,double slopePoint=.8,double initialSlope=.7){
    const double a=std::min(1.0,std::fabs(x));double y;
    if(a<slopePoint||slopePoint>=1)y=initialSlope*a;
    else y=initialSlope*slopePoint+(a-slopePoint)*(1-initialSlope*slopePoint)/(1-slopePoint);
    return x<0?-y:y;
}
// Wheel angle for an input, with the (unobserved) high-speed reduction (Havok default: (full/speed)^2).
inline double WheelAngle(const Car& c,double input,double speed){
    double angle=Shaped(input,c.slopePoint,c.initialSlope)*c.lock;
    if(speed>c.fullSpeed&&c.fullSpeed>0){const double s=c.fullSpeed/speed;angle*=s*s;}
    return angle;
}
// Yaw rate of the bicycle model for a steering input.
inline double YawRate(const Car& c,double input,double speed){
    return speed/std::max(1.0,c.wheelbase)*std::tan(WheelAngle(c,input,speed));
}
// Fallback for a model never measured: the old controller's assumptions (0.55 rad, 1.4 x half length).
inline Car Fallback(double halfLength){Car c;c.lock=.55;c.wheelbase=std::max(1.0,halfLength*1.4);c.slopePoint=1;c.initialSlope=1;c.fullSpeed=1e9;return c;}
}
