#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ramlogic {
inline bool Owns(std::uint32_t order,std::uint32_t vehicle,unsigned long long elapsed){return order&&order==vehicle&&elapsed<15000;}
inline bool CanSelect(bool combat,bool actor,bool alive,bool aboardOwn) {return combat&&actor&&alive&&!aboardOwn;}
inline double Lead(double distance,double speed) {return std::clamp(distance/std::max(8.0,speed),0.0,1.2);}
inline bool Finished(bool valid,bool combat,unsigned long long elapsed,bool contact,double distance,double hullReach) {
    return !valid||!combat||elapsed>=15000||(contact&&distance<=hullReach+2);
}
}
