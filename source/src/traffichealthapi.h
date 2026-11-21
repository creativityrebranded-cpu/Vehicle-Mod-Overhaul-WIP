#pragma once
#include <cstdint>
// Health's model record for readers (card FR1, notes/health-records-20261107/CHANGE_CARDS.md). FVO_Traffic.dll exports:
//   int  FVO_Traffic_HealthModelV1(unsigned model, FvoTrafficHealthModelV1* out)  1: Health has a record of this model
//   void FVO_Traffic_HealthAllowV1(unsigned model, int allow)                     your call: use it whatever Health says (1) or not (0)
// Additive: older addons lack both exports and readers show nothing. Health stays the only judge; the allow is applied by Health.
extern "C" {
struct FvoTrafficHealthModelV1 {
    unsigned size = sizeof(FvoTrafficHealthModelV1), version = 1;
    int known = 0, rejected = 0, allowed = 0, stage = -1;   // stage: the stage that rejected it (-1: none)
    float pass = 0, fail = 0; int spots = 0;                // at that stage (seat when none): passes, fails, distinct spots of the fails
    char stageName[16]{};
};
using FvoTrafficHealthModelQueryV1 = int (*)(unsigned, FvoTrafficHealthModelV1*);
using FvoTrafficHealthAllowV1 = void (*)(unsigned, int);
// Card RI-4 (notes/route-items-20261107/CHANGE_CARDS.md), additive, read-only; FvoTrafficHealthModelV1 is unchanged (its export checks the size):
//   int FVO_Traffic_HealthReasonV1(unsigned model, char* out, int n)   1: the last reason a pair with this model failed (never_sat, no_physics_body, ...)
//   int FVO_Traffic_HealthSpotV1(const char* route, int index, FvoTrafficHealthSpotV1* out)   1: the index-th rejected spawn spot of that route
// Older addons lack both: F7 shows no reason and no spot rows.
struct FvoTrafficHealthSpotV1 {
    unsigned size = sizeof(FvoTrafficHealthSpotV1), version = 1;
    int lane = 0, stage = -1;            // lane of the spot; the stage that rejected it
    double x = 0, y = 0;                 // centre of its 10 m cell
    float pass = 0, fail = 0; int models = 0;  // at that stage; distinct models of the fails
    char stageName[16]{};
};
// CH-1 (strip build 2, notes/spawn-one-place-20261107/CHANGE_CARDS.md): the core's raw per-update health sample of a vehicle the
// driving pipeline runs. Facts only, no thresholds: Health (healthscore.h) decides. FlyingVehicleOverhaul.dll exports
//   int FVO_PairHealthSampleV1(unsigned vehicle, FvoPairHealthSampleV1* out)   1: a sample newer than 2 s exists (size checked)
// An older core lacks it: Health keeps reading the facts it can see itself.
struct FvoPairHealthSampleV1 {
    unsigned size = sizeof(FvoPairHealthSampleV1), version = 1;
    unsigned long long at = 0;     // GetTickCount64 of the sample
    float asked = 0, actual = 0;   // m/s the plan asks / the body moves
    float lateral = 0, halfWidth = 0;  // m from the route line; the lane's half width
    float fieldShare = -1;         // share of the body inside its route's field boxes (0..1); -1: no boxes for this route
    float floorGap = 0;            // m from the car's base to the wanted floor here (+ above); 0 when unknown
    float upZ = 1;                 // the body's up axis z (1 upright, 0 on its side, <0 upside down)
    int steps = 1;                 // 1: physics stepped it recently
    int onRail = 0;                // 1: the plan's mode is Rail (on wheels on its surface); floorGap only means something then
    int floorKnown = 0;            // 1: floorGap is measured
};
using FvoPairHealthSampleQueryV1 = int (*)(unsigned, FvoPairHealthSampleV1*);
// Read-only evidence from a concealed, admitted spot-test pulse. The reader
// supplies the start of its proving interval and verifies current ownership,
// seat, bay and body before using the observation as a Health verdict.
struct FvoSpotMotionProofV1 {
    unsigned size = sizeof(FvoSpotMotionProofV1), version = 1;
    unsigned body = 0;
    int direction = -1;
    unsigned long long started = 0, completed = 0, builtAt = 0, steps = 0;
    double moved = 0;
};
using FvoSpotMotionProofQueryV1 = int (*)(unsigned, unsigned long long, FvoSpotMotionProofV1*);
struct FvoTripClaim;
// Optional diagnostic capability. A lease belongs to one admitted seated
// job in the current world. It is never saved or restored.
enum class FvoTrafficFactoryLeaseActionV1 : unsigned { Start, Status, Depart, Cancel, StartStock };
using FvoTrafficFactoryLeaseFnV1 = int (*)(FvoTrafficFactoryLeaseActionV1, unsigned, unsigned,
                                          std::uint64_t*, const FvoTripClaim*);
inline constexpr int kFvoFactoryStockCountV1 = 16;
struct FvoFactoryStockV1 {
    unsigned local = 0, vehicle = 0, base = 0;
    int usable = 0, modelLoaded = 0, inPlayerCell = 0, inPlayerWorld = 0;
    double x = 0, y = 0, z = 0;
};
// Returns the number of authored stock identities when their plugin is loaded.
using FvoFactoryStockQueryV1 = int (*)(FvoFactoryStockV1*, int);
using FvoTrafficHealthReasonV1 = int (*)(unsigned, char*, int);
using FvoTrafficHealthSpotQueryV1 = int (*)(const char*, int, FvoTrafficHealthSpotV1*);
}
