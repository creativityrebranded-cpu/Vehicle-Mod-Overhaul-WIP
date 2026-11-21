#pragma once
#include "lanevolume.h"
#include <cstdint>
// Read-only mod snapshot data. No form lookups, engine calls, or native layout changes.
extern "C" {
struct FvoObservedModelV1 {std::uint32_t size=sizeof(FvoObservedModelV1),version=1;double halfWidth=0,halfLength=0,height=0,originAbove=0;};
using FvoObservedModelProviderV1=int (*)(unsigned,FvoObservedModelV1*);
}
namespace modeladmission {
// Admission uses the same bottom offset as the field (fieldgate::kOriginAbove); the top above the origin is kept.
inline FvoObservedModelV1 Rested(FvoObservedModelV1 shape,double originAbove){
    const double top=shape.height-shape.originAbove;
    shape.originAbove=originAbove;shape.height=std::max(1.0,std::max(.5,top)+originAbove);return shape;
}
// The existing hull query publishes its latest result on the calling thread.
// Admission reads it immediately after Measure, without another native lookup.
inline thread_local unsigned storedBase=0;
inline thread_local FvoObservedModelV1 storedShape;
inline FvoObservedModelV1 FromBounds(const double* half,double bottom,double top){
    FvoObservedModelV1 shape;
    shape.halfWidth=half[0];shape.halfLength=half[1];
    shape.originAbove=std::max(0.0,-bottom);
    shape.height=std::max(0.0,top)+shape.originAbove;
    return shape;
}

inline bool Valid(const FvoObservedModelV1& shape){return shape.size==sizeof(shape)&&shape.version==1&&std::isfinite(shape.halfWidth)&&shape.halfWidth>0&&shape.halfWidth<100&&std::isfinite(shape.halfLength)&&shape.halfLength>0&&shape.halfLength<100&&std::isfinite(shape.height)&&shape.height>0&&shape.height<100&&std::isfinite(shape.originAbove)&&shape.originAbove>=0&&shape.originAbove<=shape.height;}
inline bool Fits(const lanevolume::Profile& profile,const FvoObservedModelV1& shape){if(!Valid(shape)||!lanevolume::Valid(profile))return false;const double x=shape.halfWidth+.1,bottom=profile.height*.5-shape.originAbove-.1,top=profile.height*.5+shape.height-shape.originAbove+.1;for(double side:{-x,x})if(!lanevolume::Contains(profile,side,bottom)||!lanevolume::Contains(profile,side,top))return false;return true;}
}
