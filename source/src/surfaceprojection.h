#pragma once
// Mathematical helpers for the surface-map adapter, not an engine ABI binding.
// The caller must establish the actual camera matrix, viewport and coordinate
// frame from the live game and must reject stale snapshots on camera changes.
#include <array>
#include <cmath>

namespace surfaceprojection {
struct Vec3 { double x=0,y=0,z=0; };
struct Vec2 { double x=0,y=0; };
struct Viewport { double left=0,right=1,bottom=0,top=1; };
inline bool Finite(Vec3 p){return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);}
inline bool Project(const std::array<double,16>& m,Viewport v,Vec3 p,Vec2& out,double& depth) {
    if(!Finite(p)||!std::isfinite(v.left)||!std::isfinite(v.right)||!std::isfinite(v.top)||!std::isfinite(v.bottom)||v.left>=v.right||v.bottom>=v.top)return false;
    for(double x:m)if(!std::isfinite(x))return false;
    // Rows and viewport mapping match the visible algebra of FUN_142be45c0.
    // Its bitmask/sign semantics require additional executable verification;
    // rejecting w<=0 here deliberately avoids displaying behind-camera points.
    const double w=m[12]*p.x+m[13]*p.y+m[14]*p.z+m[15];
    if(!std::isfinite(w)||w<=1e-9)return false;
    const double x=(m[0]*p.x+m[1]*p.y+m[2]*p.z+m[3])/w;
    const double y=(m[4]*p.x+m[5]*p.y+m[6]*p.z+m[7])/w;
    const double z=(m[8]*p.x+m[9]*p.y+m[10]*p.z+m[11])/w;
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
    const Vec2 result{(v.left+v.right)*.5+(v.right-v.left)*.5*x,
                      (v.bottom+v.top)*.5+(v.bottom-v.top)*.5*y};
    if(!std::isfinite(result.x)||!std::isfinite(result.y))return false;
    out=result;depth=z;return true;
}
inline bool IntersectHeight(Vec3 origin,Vec3 direction,double z,Vec3& out) {
    if(!Finite(origin)||!Finite(direction)||!std::isfinite(z)||std::fabs(direction.z)<1e-9)return false;
    const double t=(z-origin.z)/direction.z;
    if(!std::isfinite(t)||t<0)return false;
    const Vec3 result{origin.x+t*direction.x,origin.y+t*direction.y,z};
    if(!Finite(result))return false;
    out=result;return true;
}
}
