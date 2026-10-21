#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace interactionlogic {
inline bool OtherVehicle(std::uint32_t vehicle,std::uint32_t shooter,std::uint32_t shooterVehicle) {
    return vehicle && shooter && vehicle!=shooter && vehicle!=shooterVehicle;
}
// A segment must enter the actual oriented hull, rather than a wide circle around it.
inline bool HullHit(const float pos[3],const float rot[9],const float half[3],const float from[3],const float to[3],double& enter) {
    for(int i=0;i<3;++i)if(!std::isfinite(pos[i])||!std::isfinite(half[i])||half[i]<=0||!std::isfinite(from[i])||!std::isfinite(to[i]))return false;
    for(int i=0;i<9;++i)if(!std::isfinite(rot[i]))return false;
    double lo=0,hi=1;
    for(int axis=0;axis<3;++axis) {
        double a=0,d=0;
        for(int k=0;k<3;++k){a+=(from[k]-pos[k])*rot[axis*3+k];d+=(to[k]-from[k])*rot[axis*3+k];}
        const double extent=std::max(0.25f,half[axis])+0.15;
        if(std::fabs(d)<1e-8){if(std::fabs(a)>extent)return false;continue;}
        double p=(-extent-a)/d,q=(extent-a)/d;if(p>q)std::swap(p,q);
        lo=std::max(lo,p);hi=std::min(hi,q);if(lo>hi)return false;
    }
    enter=lo;return true;
}
}
