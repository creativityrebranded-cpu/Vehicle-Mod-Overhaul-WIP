#pragma once
#include <cmath>
namespace travellogic {
struct Context { bool outdoors=false,known=false,changesSpace=false,door=false;double distance=0,pathLength=0; };
inline bool UseVehicle(const Context& c) {
    return c.outdoors&&c.known&&!c.changesSpace&&!c.door&&std::isfinite(c.distance)&&c.distance>=150 &&
        std::isfinite(c.pathLength)&&c.pathLength>=c.distance*0.8;
}
}
