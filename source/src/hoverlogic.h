#pragma once
#include <algorithm>
#include <cmath>

namespace hoverlogic {
inline double Pitch(double fx, double fy, double fz) { return std::atan2(fz, std::hypot(fx, fy)); }
// asin loses the distinction between upright and overturned beyond 90 degrees.
inline double RelativePitch(const float basis[9],const double forward[3]) {
    double v[3]={};for(int row=0;row<3;++row)for(int k=0;k<3;++k)v[row]+=basis[row*3+k]*forward[k];
    return Pitch(v[0],v[1],v[2]);
}
inline double Roll(double rightZ, double upZ) { return std::atan2(-rightZ, upZ); }
inline double TiltLift(double lift, double upZ, double gain) {
    // Preserve the banked-flight push, fading it out as the chassis tips onto its side.
    const double upright = std::clamp((upZ - 0.3) / 0.4, 0.0, 1.0);
    return std::max(0.0, lift) * gain * upright / std::max(0.3, upZ);
}
}
