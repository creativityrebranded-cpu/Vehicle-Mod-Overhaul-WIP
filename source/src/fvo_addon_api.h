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
// Optional cycle extension, negotiated by registration (old claim/goal layouts unchanged).
// With this registered, step may return 3 (park at home and repeat) or 4 (park at out->x,y and repeat).
// Core retains the driver, calls cycle(user,0,...) every parking/dwell tick to retain the slot,
// then cycle(user,1,...) after 10 continuously landed seconds to start another lap.
// Return 0 if the slot is no longer valid. Cancelled jobs stop calling; the provider expires them.
typedef int (*FvoTrafficCycle)(void* user, int restart, double x, double y, double z);
typedef int (*FvoRegisterTrafficCycle_t)(FvoTrafficCycle cycle); // export "FVO_RegisterTrafficCycle"
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
// export "FVO_RefPos" (2026-10-28): a reference's position (x, y, z) while it is loaded in the player's worldspace; 0 otherwise.
typedef int (*FvoRefPos_t)(unsigned refId, double* xyz);
// Cold-start pool (2026-10-28): spawn spots on the ground for `world`; core makes one vehicle with a seated citizen driver per
// spot (once per save, reused afterwards) and, once the driver sits, asks `trip` for a trip (return 1 and fill `out`; 0 = not
// yet, asked again in 2 s). heading: radians, clockwise from north.
struct FvoPoolSpot { double x, y, z, heading; };
typedef int (*FvoPoolTrip)(int spot, unsigned vehicleId, double x, double y, double z, FvoTripClaim* out);
typedef void (*FvoPoolSet_t)(const char* world, const FvoPoolSpot* spots, int count, FvoPoolTrip trip);  // export "FVO_PoolSet"
// export "FVO_PoolSetEx" (2026-10-29, parking): the same, with per spot parking[i] = 1 (a landing place where one parked vehicle
// stands while the player is near: core makes it once per save, hides it when far, and gives it a driver only when the trip
// callback, asked with vehicleId 0 and out = null, answers 1) and a stable name (core remembers its vehicle and driver by it,
// so a list that changes or a city laid down elsewhere never makes a second one). Up to 16 spots.
typedef void (*FvoPoolSetEx_t)(const char* world, const FvoPoolSpot* spots, int count, const int* parking, const char* const* names, FvoPoolTrip trip);
// Auto on routes (2026-10-28, the user: "a destination with a traffic route nearby adds as one of the vehicles and uses it to get
// to the nearest exit to the destination and then continue from there"). When the player's Auto starts for a marker, core asks
// the planner once: return 1 and fill `out` with a trip from (x, y, z) to (tx, ty, tz) along a route (climb, a free lane slot,
// the lane, out at the exit nearest the target); 0 = no route helps (Auto flies straight). Core flies the trip's goals, then
// the rest of the way to the marker when the step returns non-zero.
typedef int (*FvoRoutePlanner)(unsigned vehicleId, double x, double y, double z, double tx, double ty, double tz, FvoTripClaim* out);
typedef int (*FvoRegisterRoutePlanner_t)(FvoRoutePlanner planner);  // export "FVO_RegisterRoutePlanner"; 1: registered
// export "FVO_LocalTime" (2026-10-28, route schedules): the current planet's local time of day in hours (0 .. 24); 0 when there
// is no planet (scheduled routes then wait; all-day routes run).
typedef int (*FvoLocalTime_t)(double* hours);
// export "FVO_SetWorldShift" (2026-10-29, map route editor): the addon tells core where an anchored traffic world ([world.NAME]
// anchor = ...) lies in the player's world: `gameWorld` and the shift from file to game coordinates. Core draws and edits
// those routes in game coordinates and saves them back in the file's.
typedef void (*FvoSetWorldShift_t)(const char* name, const char* gameWorld, double x, double y, double z);
typedef void (*FvoTick_t)();                                         // called by core every frame on the main thread
typedef int (*FvoRegisterTick_t)(FvoTick_t tick);                    // export "FVO_RegisterTick"; 1: registered
}
