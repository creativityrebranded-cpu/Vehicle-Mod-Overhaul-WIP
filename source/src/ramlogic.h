#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ramlogic {
inline bool Owns(std::uint32_t order,std::uint32_t vehicle,unsigned long long elapsed){return order&&order==vehicle&&elapsed<15000;}
// Holding T on a highlighted actor orders the ram (2026-10-21, the user); only that exact actor, never a place or the vehicle's own occupants.
// Why a selection cannot be rammed; nullptr: it can.
inline const char* Reject(bool actor,bool alive,bool aboardOwn) {
    return !actor?"no actor highlighted":!alive?"actor is dead":aboardOwn?"actor rides this vehicle":nullptr;
}
inline bool CanSelect(bool actor,bool alive,bool aboardOwn) {return !Reject(actor,alive,aboardOwn);}
inline double Lead(double distance,double speed) {return std::clamp(distance/std::max(8.0,speed),0.0,1.2);}
// The straight run: the nose turns onto the led point as fast as the hull allows; full throttle unless the target is behind (then a slow,
// tight turn first).
inline double SteerRate(double headingErr,double maxRate) {return std::clamp(2.5*headingErr,-maxRate,maxRate);}
inline double Throttle(double headingErr) {return std::fabs(headingErr)>1.6?0.25:1.0;}
// An explicit ram ends on its own outcome, not on the player's combat flag (which can differ from the target's or driver's).
inline bool Finished(bool valid,unsigned long long elapsed,bool contact,double distance,double hullReach) {
    return !valid||elapsed>=15000||(contact&&distance<=hullReach+2);
}
}
