#pragma once
#include <array>
#include <algorithm>
#include <cmath>
namespace sitelogic {
inline bool Support(const std::array<double,9>&h){
 for(double z:h)if(!std::isfinite(z))return false;
 const auto [lo,hi]=std::minmax_element(h.begin(),h.end());return *hi-*lo<=1.0;
}
inline bool Standing(double floor,double vehicleFloor,double speed,double originHeight){
 return std::isfinite(floor)&&std::fabs(floor-vehicleFloor)<=0.6&&speed<=0.75&&originHeight-vehicleFloor<=4.0&&originHeight>=vehicleFloor-0.5;
}
}
