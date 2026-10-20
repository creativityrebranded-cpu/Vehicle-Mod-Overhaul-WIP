#pragma once
#include <cmath>
#include <cstdint>

namespace targetsession {
// The native marker handle can be reused when the marker is moved. Include its position.
struct Marker { std::uint32_t handle=0; double x=0,y=0,z=0; };
inline bool Same(const Marker& a, const Marker& b) {
    return a.handle && a.handle==b.handle && std::hypot(a.x-b.x,a.y-b.y)<0.5 && std::fabs(a.z-b.z)<0.5;
}
struct Session {
    Marker consumed;
    bool Accept(const Marker& m) const { return m.handle && !Same(m,consumed); }
    void Exit(const Marker& m) { consumed=m; }
    void Selected() { consumed={}; }
};
inline bool CanFollow(std::uint32_t vehicle, std::uint32_t own) { return vehicle && vehicle==own; }
}
