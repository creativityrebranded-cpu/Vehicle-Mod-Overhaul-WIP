#pragma once
#include <cmath>
namespace trafficadmission {
// Traffic rules own the population target. Creation pacing and outstanding
// retirement are separate from that target; neither imposes a smaller fleet.
inline bool ValidOrigin(double x,double y) {
    return std::isfinite(x)&&std::isfinite(y);
}
inline bool Capacity(int pairs,int pending,int retiredReferences,int target) {
    return target>0&&pairs<target&&pending<target-pairs&&retiredReferences<2*target;
}
}
