#pragma once
#include "runtimeforms.h"
#include <array>
#include <cmath>
#include <span>
#include <string_view>

namespace cityprofile {
struct Frame {
    double shiftX=0,shiftY=0,shiftZ=0;
    std::array<double,3> ToLocal(double x,double y,double z) const {return {x-shiftX,y-shiftY,z-shiftZ};}
};
struct RingAnchor {const char* worldPrefix="";double x=0,y=0,top=0,reach=0;};
struct Definition {
    const char* name="";
    const char* plugin="";
    const char* placementWorldPrefix="";
    runtimeforms::Range vehicles,pilots,pool;
    std::uint32_t firstSpot=0,lastSpot=0;
    // IDs encoded for this content file's existing master layout, not runtime IDs.
    std::uint32_t serializedVehicleBase=0,serializedPilotBase=0,serializedWorld=0,serializedCell=0;
    Frame frame;
    RingAnchor ring;
};
inline constexpr Definition kNewAtlantis{
    "New Atlantis","FVO_CityTraffic.esm","JemisonPlanetWorld",
    {"FVO_CityTraffic.esm",0x800,32},{"FVO_CityTraffic.esm",0x820,32},{"FVO_CityTraffic.esm",0xF00,64},
    0x900,0xEFF,0xFE000033,0x001591EA,0x01251B,0x01251C,
    {-700,1700,-238.97},{"Jemison",-756.2,1841.6,3.5,3000}
};
inline constexpr std::array kCities{kNewAtlantis};
inline bool MatchesPlacement(const Definition& city,std::string_view world) {
    const std::string_view prefix=city.placementWorldPrefix;
    if(prefix.empty()||!world.starts_with(prefix))return false;
    for(const char c:world.substr(prefix.size()))if(c<'0'||c>'9')return false;
    return true;
}
inline const Definition* ForPlacement(std::string_view world,std::span<const Definition> cities=kCities) {
    for(const auto& city:cities)if(MatchesPlacement(city,world))return &city;
    return nullptr;
}
inline const Definition* ForRing(std::string_view world,double x,double y,std::span<const Definition> cities=kCities) {
    for(const auto& city:cities){const std::string_view prefix=city.ring.worldPrefix;
        if(!prefix.empty()&&world.starts_with(prefix)&&std::hypot(x-city.ring.x,y-city.ring.y)<city.ring.reach)return &city;}
    return nullptr;
}
}
