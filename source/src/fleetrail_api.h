#pragma once

struct FvoFleetRailPoint {
    double x, y, z;
};

struct FvoFleetRailRequest {
    unsigned vehicle, driver;
    const FvoFleetRailPoint* points;
    unsigned count;
    double speed;
};

using FvoFleetRailAssignV1Fn = int (*)(const FvoFleetRailRequest*);
using FvoFleetRailReleaseV1Fn = void (*)(unsigned vehicle);
using FvoFleetRailStateV1Fn = int (*)(unsigned vehicle);

// Core-only save/load coordination. These functions never call the engine.
void FvoFleetRailFence();
void FvoFleetRailReset();
