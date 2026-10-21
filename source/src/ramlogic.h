#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace ramlogic {
inline constexpr unsigned long long kReadyMs = 60000;  // an armed vehicle waits this long for the player to pick a ram target
inline bool Owns(std::uint32_t order,std::uint32_t vehicle,unsigned long long elapsed){return order&&order==vehicle&&elapsed<15000;}
// Hold T arms the ram (2026-10-20, the user); only the explicitly highlighted actor is rammed, never a place or the vehicle's own occupants.
inline bool Armed(std::uint32_t ready,std::uint32_t vehicle,unsigned long long elapsed){return ready&&ready==vehicle&&elapsed<kReadyMs;}
// Why a selection cannot be rammed; nullptr: it can.
inline const char* Reject(bool armed,bool actor,bool alive,bool aboardOwn) {
    return !armed?"not armed (hold T first)":!actor?"no actor highlighted":!alive?"actor is dead":aboardOwn?"actor rides this vehicle":nullptr;
}
inline bool CanSelect(bool armed,bool actor,bool alive,bool aboardOwn) {return !Reject(armed,actor,alive,aboardOwn);}
inline double Lead(double distance,double speed) {return std::clamp(distance/std::max(8.0,speed),0.0,1.2);}
// An explicit ram ends on its own outcome, not on the player's combat flag (which can differ from the target's or driver's).
inline bool Finished(bool valid,unsigned long long elapsed,bool contact,double distance,double hullReach) {
    return !valid||elapsed>=15000||(contact&&distance<=hullReach+2);
}
}
