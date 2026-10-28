#pragma once
// The interface FVO addons use (2026-10-27): core FlyingVehicleOverhaul.dll exports these C functions; an addon DLL
// (FVO_CityPlacements.dll: the ring ambient traffic) looks them up with GetProcAddress once all plugins have loaded.
// Core knows nothing of rings or routes: it offers each everyday-traffic pick to the registered provider, and flies a
// claimed trip by asking the provider's step function for a goal every frame. Plain C types only (stable across builds).
extern "C" {
struct FvoTripGoal {
    double x, y, z, stop;  // world position (z: absolute height) and stop radius in metres
    int passThrough;       // keep moving through the point (lanes, corners)
    int fly;               // prefer the hover for this leg
    float speed;           // m/s limit (0: the autopilot's own)
};
// Called every frame while the trip runs. Return 0 and fill `out` to keep going; return 1 when done (core then parks the
// vehicle at its starting place and the driver gets out); return 2 when done at a destination: core parks it at out->x, y.
typedef int (*FvoTripStep)(void* user, double x, double y, double z, FvoTripGoal* out);
struct FvoTripClaim {
    FvoTripStep step;
    void* user;            // the provider's own trip record
    const char* name;      // for the log ("ring trip")
};
// Core offers a traffic pick: a free parked vehicle at (x, y, z) with a calm driver near it. Fill `out` and return 1 to
// take it as a scripted trip; return 0 to let core run its own trip.
typedef int (*FvoTrafficOffer)(double x, double y, double z, unsigned vehicleId, FvoTripClaim* out);

typedef int (*FvoRegisterTrafficProvider_t)(FvoTrafficOffer offer);  // export "FVO_RegisterTrafficProvider"; 1: registered
typedef void (*FvoLog_t)(const char* line);                          // export "FVO_Log": one line in FlyingVehicleOverhaul.log
typedef int (*FvoWorldName_t)(char* out, int size);                  // export "FVO_WorldName": the player's worldspace editor name; 0 none
typedef unsigned (*FvoBaseOf_t)(unsigned refId);                     // export "FVO_BaseOf": a reference's base form id; 0 unknown
// export "FVO_SetRouteMarkers": probes for `world` at `count` points (x, y, z, scale each); shown while "Show traffic routes" is on.
typedef void (*FvoSetRouteMarkers_t)(const char* world, const float* xyzs, int count);
typedef void (*FvoTick_t)();                                         // called by core every frame on the main thread
typedef int (*FvoRegisterTick_t)(FvoTick_t tick);                    // export "FVO_RegisterTick"; 1: registered
}
