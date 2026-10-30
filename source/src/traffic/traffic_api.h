#pragma once
// FVO core API v2 for the traffic addon (2026-10-29, stage 1 of notes/TRAFFIC_ADDON_PROPOSAL_20261029.md).
// Core FlyingVehicleOverhaul.dll does the engine work (create, seat, persist, move, ground, jobs); FVO_Traffic.dll makes every
// traffic decision. Plain C types only, so the two DLLs never have to be built together. The addon asks once, after all
// plugins have loaded:  const FvoTrafficApi* api = FVO_TrafficApi(kFvoTrafficApiVersion, &hooks);  (null: core too old).
// Everything is by form id and runs on the main thread unless said otherwise.
#include "fvo_addon_api.h"

extern "C" {

enum { kFvoTrafficApiVersion = 8 };  // v8: the pair health record; v7: pre-armed traffic pilots; v6: covered arrival population; v4 read-only prefix remains queryable

struct FvoTrafficPlayer {
    char world[201];   // the worldspace's editor name
    double x, y, z;    // the player's position
};
struct FvoTrafficRef {
    unsigned id, baseId;
    int usable;        // not disabled and not deleted
    int disabled;      // the disabled flag alone
    int has3D;
    int isActor;       // looks like an actor (has an AI process slot)
    int dead;          // an actor that is dead
    double x, y, z;
};
enum { kFvoWhoNone = 0, kFvoWhoPlayer = 1, kFvoWhoNpc = 2, kFvoWhoParked = 3 };
struct FvoTrafficVehicle {   // one row of core's vehicle snapshot (this frame)
    unsigned id, baseId;
    int who;                 // kFvoWho*
    double x, y, z;
    int hasBody;             // FVO found its physics body (a vehicle without one never joins traffic)
    double radius, halfHeight; // conservative model envelope for admission separation
    unsigned driver;         // v7: who sits in the driver's seat (0 none)
};
struct FvoTrafficPerson {    // a calm, free, non-hostile person of core's current civil scan
    unsigned id;
    double x, y;
};
struct FvoRushPlace { double x, y, z, heading; };
struct FvoRushState { int active, phase, total, done, moved, made, skipped, placed, seated, driving; };

// The addon's side: core calls these where its own pool used to run.
struct FvoTrafficHooks {
    void (*tick)();                                         // full update frames ("traffic pool" in the spike log)
    void (*housekeeping)();                                 // paused frames (the local map): Start / Swap placing
    void (*reset)();                                        // a save load (the world is gone)
    void (*rush)(const FvoRushPlace* places, int count);    // Start / Swap asked for (any thread: the map movie's)
    void (*rushStatus)(FvoRushState* out);                  // any thread
    // Everyday traffic: core's civil scan (every 15 s, traffic on, nobody fighting, civil jobs under the cap) asks the addon to
    // pick a vehicle and a driver from everydayVehicles / everydayPeople and start the trip. Return 1 when one started.
    int (*everyday)();
    // Re-read the traffic files (any thread: the settings menu's "Reload traffic files", or the map route editor after it saves).
    // The files are read once at start and then only when this is called (2026-10-29: no more polling of the folder).
    void (*reload)();
};

// Core's side.
struct FvoTrafficApi {
    int version;
    int (*player)(FvoTrafficPlayer* out);                   // 1: the player is in a worldspace (0: an interior, no world)
    int (*trafficOn)();                                     // the "Everyday traffic" switch
    int (*ref)(unsigned id, FvoTrafficRef* out);            // 1: the id is a live reference (its own id checked)
    int (*formExists)(unsigned formId);                     // a base form (or any form) by id
    int (*vehicles)(FvoTrafficVehicle* out, int max);       // the vehicle snapshot; -1: none this frame
    // Raw ground readings at (x, y): the nav map's asked at height zQuery, and the cell bake's. Returns bit 1 when `nav` is
    // known, bit 2 when `baked` is known.
    int (*ground)(double x, double y, double zQuery, double* nav, double* baked);
    // Creation, in the player's cell and worldspace. 0: not made.
    unsigned (*createVehicle)(unsigned baseId, double x, double y, double z, double heading);
    // The driver is seated inside its creation (core's seated-creation hook); seatResult: the seat call's answer (-99 none).
    unsigned (*createSeatedDriver)(unsigned baseId, unsigned vehicleId, double x, double y, double z, double heading, int* seatResult);
    int (*makeTraffic)(unsigned id, int actor);             // persistent (+ never essential for actors); 1: persistent now
    void (*show)(unsigned id, int on);                      // enable / disable (no-op when already so)
    int (*moveTo)(unsigned id, double x, double y, double z, double heading);
    unsigned (*vehicleOf)(unsigned actorId);                // the vehicle the actor sits in (0: none)
    int (*sitState)(unsigned actorId);
    int (*snapSeat)(unsigned actorId, unsigned vehicleId);  // the instant seat
    int (*usesVehicle)(unsigned vehicleId);                 // an NPC job drives it
    void (*endJob)(unsigned vehicleId, const char* why);
    int (*tripRefusal)(unsigned actorId, unsigned vehicleId, char* why, int size);  // 1: a trip would be refused now (why filled)
    int (*startTrip)(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim);  // the driver is already aboard
    int (*canManagePlacement)(unsigned vehicleId);          // a plugin's placed vehicle traffic may use
    // ---- v2: everyday traffic ----
    int (*everydayVehicles)(FvoTrafficVehicle* out, int max);  // free parked vehicles traffic may take (3D loaded, safe bases)
    int (*everydayPeople)(FvoTrafficPerson* out, int max);     // only inside the everyday hook
    int (*mapPlaces)(double* xy, int maxPlaces);               // settlements / outposts on the planet map near the player (x, y pairs)
    int (*civilJobs)();                                        // civil NPC vehicle jobs running now
    int (*mayUse)(unsigned actorId, unsigned vehicleId);
    int (*startScripted)(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim);  // the driver walks over, boards, drives
    int (*startSupply)(unsigned actorId, unsigned vehicleId, double x, double y);          // round trip out to (x, y) and back
    int (*startPatrol)(unsigned actorId, unsigned vehicleId);                              // laps round its home
    // v5. No world mutations during loading. Covered means the live local map,
    // never an arbitrary pause menu. Births must recheck this at execution time.
    int (*presentation)();                               // 0 blocked/loading, 1 gameplay, 2 local map covers world
    int (*birthSite)(unsigned baseId, double x, double y, double z, double heading, double* originZ); // 0 pending, 1 safe, -1 unsafe
    int (*exposure)(double x, double y, double z);        // 0 unknown, 1 observable, 2 occluded (actual LOS, not a view cone)
    int (*tripActive)(unsigned vehicleId);                // owner still holds its callback, including player Auto
    // Covered arrival initialization only; never a gameplay airborne fallback.
    int (*airSite)(unsigned baseId,double x,double y,double z,double heading);
    void (*arrivalProgress)(int expected,int ready);
    int (*releaseBirth)(unsigned vehicleId); // release staged chassis only when its pilot owns control
    void (*holdBirth)(unsigned vehicleId); // restore a save-persisted staged chassis
    int (*hullSize)(unsigned baseId,double* radius,double* halfHeight);
    // v7: pre-armed pilot (2026-10-29). Free asks before a birth; start binds the new pair and its claimed trip in the same call.
    int (*trafficPilotFree)();
    int (*startArmed)(unsigned actorId,unsigned vehicleId,const FvoTripClaim* claim);
    int (*pilotHolds)(unsigned vehicleId); // a pilot is flying it now (fall guard)
    // Every distinct vehicle model in the load order (vehicle keyword, readable bounds; variants of one model counted once),
    // sorted by form id. Returns the count written (2026-10-29: randomized traffic models).
    int (*vehicleTypes)(unsigned* outBaseIds,int max);
    // v7: a placed vehicle's spot against the nav map / baked terrain only (no rays): 1 clear, 0 not mapped yet, -1 blocked.
    int (*mappedSpot)(unsigned baseId,double x,double y,double z,double heading);
    // v8: the pair health record: 0 unproven, 1 healthy, 2 recovering, 3 broken (trafficpair::PairHealth).
    int (*pairHealth)(unsigned vehicleId);
    double (*noticeFar)(unsigned baseId);      // v8: how far a thing of that base must be not to be made out, metres (0: a driver's size)
    int (*inCameraView)(double x,double y,double z); // v8: inside the camera's view cone now (1 when unknown)
    int (*playerTook)(unsigned vehicleId);     // v8: the player has had this vehicle as their own since the load: it is no traffic any more
};

typedef const FvoTrafficApi* (*FvoTrafficApi_t)(int version, const FvoTrafficHooks* hooks);  // export "FVO_TrafficApi"
}
