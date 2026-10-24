#pragma once
#include <algorithm>
#include <cmath>
#include "vtolphysics.h"

// AI pilots (autopilot, NPC drivers) on the physical VTOL chassis. Their planners command a yaw rate, a speed and a
// climb; this turns those into the attitude rates vtol::Solve feeds to torque. A turn banks the chassis (coordinated:
// the tilted collective supplies the sideways force), the nose holds level or the commanded pitch, and yaw turns
// about world up so a banked turn does not pull the nose down.
namespace vtolai {
constexpr double kGravity=9.8;
constexpr double kMaxBank=0.6;      // rad (34 degrees): the most a turn may bank
constexpr double kBankGain=3.0;     // roll rate per radian of bank error
constexpr double kPitchGain=2.0;    // pitch rate per radian of pitch error
constexpr double kPitchDeadzone=0.05;
constexpr double kLevelRate=1.2;    // 1/s, unaimed pitch levels at this rate
constexpr double kSideAcceleration=6;  // m/s^2: the AI chassis' slip-damping side thrusters (player: 3)
struct Input {
    vtol::Vec right{1,0,0}, forward{0,1,0}, up{0,0,1};
    double pitch=0, roll=0;  // nose up +, right side down +
    double speed=0;          // horizontal m/s
    double yawRate=0;        // commanded, rad/s, turn right +
    double yawLimit=1, pitchLimit=1, rollLimit=1;  // hull rate limits
    bool rollAimOn=false; double rollAim=0;
    bool pitchAimOn=false; double pitchAim=0;
};
// The bank a level coordinated turn needs: tan(bank) = v * yaw rate / g.
inline double TurnBank(double speed,double yawRate) {
    if (!std::isfinite(speed)||!std::isfinite(yawRate)) return 0;
    return std::clamp(std::atan2(speed*yawRate,kGravity),-kMaxBank,kMaxBank);
}
inline double Wrap(double a) {
    while (a>3.14159265358979) a-=6.28318530717959;
    while (a<-3.14159265358979) a+=6.28318530717959;
    return a;
}
// World angular rates for the solver (same convention as the stock model: turn right = negative about world up).
inline vtol::Vec Rates(const Input& in) {
    const double yaw=std::clamp(std::isfinite(in.yawRate)?in.yawRate:0.0,-in.yawLimit,in.yawLimit);
    const double bankWant=in.rollAimOn?std::clamp(in.rollAim,-kMaxBank,kMaxBank):TurnBank(in.speed,yaw);
    const double roll=std::clamp(kBankGain*Wrap(bankWant-in.roll),-in.rollLimit,in.rollLimit);
    double pitch=-in.pitch*kLevelRate;
    if (in.pitchAimOn) {
        double e=in.pitchAim-in.pitch;
        e=e>0?std::max(0.0,e-kPitchDeadzone):std::min(0.0,e+kPitchDeadzone);
        pitch=kPitchGain*e;
    }
    pitch=std::clamp(pitch,-in.pitchLimit,in.pitchLimit);
    return in.right*pitch+in.forward*roll+vtol::Vec{0,0,-yaw};
}
}
