#pragma once
// Field boxes from the traffic addon (design section 3, work order step 3). Optional both ways, like FvoRoadLanesV1: an addon without the
// provider leaves the core reading the road exactly as before (derived intent: FvoRoadSurfaceV1, travel, lane height); a core without the
// export is never offered it. Additive only: no existing struct, key or export changes.
//
// The addon reads its route keys (agreed with the editor session, traffic files stay LF):
//   road_boxes = 1 <tiles>, then per tile `width height yaw pitch roll faces solids` (0 width or height inherits the road's; yaw and
//   pitch are degree offsets from the tile's points; faces is a 6-bit mask in fieldbox::Face order, 1 = Touch);
//   line_end = pass | stop | arrive;  line_arrive = x y z heading park.
// D11 is not approved: tiles stored as rounded, tube or trapezoid without road_boxes are NOT answered as boxes (return 0 for them).
#include <cstdint>
#include <cstddef>

struct FvoFieldBoxV1 {
    std::uint32_t size = sizeof(FvoFieldBoxV1), version = 1;
    double cx = 0, cy = 0, cz = 0;                       // centre, world
    double halfWidth = 0, halfLength = 0, halfHeight = 0;
    double yaw = 0, pitch = 0, roll = 0;                 // radians; game yaw atan2(x, y); pitch + nose up; roll + right side down
    std::uint32_t faces = 1;                             // bit i = face i (bottom, top, left, right, front, back) is Touch
    int solids = 0;                                      // 1: solids inside are allowed (route through a structure, D14)
    int tile = -1;                                       // which tile of the route
    int lineEnd = 0;                                     // the route line's end: 0 pass, 1 stop, 2 arrive
    double endX = 0, endY = 0, endZ = 0;                 // where the line ends (stop and arrive)
    double arriveHeading = 0; int arrivePark = 0;        // arrive: final heading (radians), park there
    int isDefault = 0; // V2 only; occupies V1 tail padding, never read for V1 records
};
static_assert(sizeof(FvoFieldBoxV1) == 136 && offsetof(FvoFieldBoxV1, isDefault) == 132, "V1 array ABI must stay frozen");
// For the lane of the pilot `owner` near (x, y, z): the tile there and its neighbours along the route (the chain counts as one space).
// Writes up to `max` boxes; returns how many (0: no boxes here, read the road as before).
using FvoFieldBoxProviderV1 = int (*)(void* owner, double x, double y, double z, FvoFieldBoxV1* out, int max);
using FvoRegisterFieldBoxV1 = int (*)(FvoFieldBoxProviderV1);
using FvoRegisterFieldBoxV2 = FvoRegisterFieldBoxV1; // optional export, identical array stride
#include "fieldbox.h"
#include <cmath>
namespace fieldboxapi {
inline FvoFieldBoxProviderV1 provider = nullptr;  // core side: set by FVO_RegisterFieldBoxV1
inline bool IsDefault(const FvoFieldBoxV1& a) { return a.size == sizeof(FvoFieldBoxV1) && a.version == 2 && a.isDefault == 1; }
inline int lastLineEnd = 0;                        // for traces: the last line end answered (0 pass, 1 stop, 2 arrive)
// One answered box as a fieldbox::Box; false for anything malformed (never trusted half-read).
inline bool ToBox(const FvoFieldBoxV1& a, fieldbox::Box& b) {
    if (a.size != sizeof(FvoFieldBoxV1) || (a.version != 1 && a.version != 2)) return false;
    for (double v : { a.cx, a.cy, a.cz, a.halfWidth, a.halfLength, a.halfHeight, a.yaw, a.pitch, a.roll }) if (!std::isfinite(v)) return false;
    if (!(a.halfWidth > .1 && a.halfLength > .1 && a.halfHeight > .1) || a.halfWidth > 200 || a.halfLength > 500 || a.halfHeight > 200) return false;
    b.centre = { a.cx, a.cy, a.cz }; b.halfWidth = a.halfWidth; b.halfLength = a.halfLength; b.halfHeight = a.halfHeight; b.yaw = a.yaw; b.pitch = a.pitch; b.roll = a.roll;
    for (int f = 0; f < fieldbox::FaceCount; ++f) b.faces[f] = (a.faces >> f) & 1u ? fieldbox::State::Touch : fieldbox::State::Avoid;
    b.solids = a.solids != 0;
    return true;
}
}
