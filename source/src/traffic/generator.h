#include "../handoff.h"
#pragma once
#include "../camera_visibility.h"
// Stable world-owned fleet. Reconcile edits without replacing or moving healthy pairs.
// Missing capacity is prepared under the loading screen or X panel.
// The core enforces engine-side admission.
// Driver/body readiness precedes trips; failures retain identities and defer safely.
#include <string>
#include <vector>
#include "traffic_api.h"
#include "../fleetrail_api.h"
#include "birth_api.h"
#include "factorybirth_api.h"
#include "factorydispatch_api.h"
#include "../trafficcensusapi.h"
#include "../relocationapi.h"
#include "../traffichealthapi.h"

namespace generator
{
	void Attach(const FvoTrafficApi* api, FvoLog_t log);
    void SetStorage(const wchar_t* registry); // bind a stable playthrough path before starting a frame
    // Optional save-profile prototype. The private and road phases share the same Route cookie.
    struct FactoryBay { std::string key; unsigned vehicle=0; FvoPoolSpot at{}, arrival{}; bool driverOriginReady=true; };
    enum class FactoryTripAction { Test, Dispatch, Rollback, Complete };
    using FactoryTrip=int (*)(FactoryTripAction action,unsigned vehicle,double x,double y,double z,FvoTripClaim* claim);
    void SetFactoryBay(const FactoryBay* bay,FactoryTrip trip,FvoSpotMotionProofQueryV1 proof,FvoTrafficFactoryLeaseFnV1 lease);
    void SetFactoryStockQuery(FvoFactoryStockQueryV1 query);
    void SetFactoryBirth(FvoFactorySiteFnV1 site, FvoFactoryBirthFnV1 birth, FvoFactoryStockBindFnV1 bind,
                         FvoFactoryStockObserveFnV1 observe);
    void SetFactoryAnchors(std::vector<FvoPoolSpot> anchors);
    enum class StockRouteAction { Reserve, Commit, Cancel };
    using StockRoute=int (*)(StockRouteAction,int roadSlot,unsigned vehicle,const FvoPoolSpot& at,FvoTripClaim* claim);
    void SetStockDispatch(FvoStockTransferFnV1 transfer,StockRoute route);
	// Authored ground origins for stable fleet names, then parking places with parking[i] = 1.
	void SetSpots(const char* world, const FvoPoolSpot* spots, int count, const int* parking, const char* const* names, FvoPoolTrip trip);
    // Pure mod-owned gate consumes the hull dimensions already measured by admission.
    using ModelGate=bool (*)(int slot,unsigned base,double radius,double halfHeight,bool hullKnown);
    void SetModelGate(ModelGate gate);
    // Spawn flow step 5, Siting (spawnflow.h, 2026-11-07): the birth pose from the slot's field box (tilebox::TileBox, default box for
    // routes with no road_boxes) and the model's bounds, or one reason. Replaces the old field gate (roadfield), the navmesh deck gate,
    // site rays, the ramp lift and the lane-ahead check. occupied: another vehicle stands there (Other Vehicles + Census pairs).
    // Returns 1 with `at` set (x, y on the lane, z = field floor + rest height + drop margin), or 0 with *why.
    struct RoadSite { double station=0, interval=0; };
    using SiteGate=int (*)(int slot,int attempt,unsigned base,double radius,double halfHeight,FvoPoolSpot& at,bool occupied,bool spotRejected,RoadSite& road,const char** why);
    using RoadSpacingGate=bool (*)(int slot,const RoadSite& road,const FvoPoolSpot& other,double& gap);
    void SetSiteGate(SiteGate gate,RoadSpacingGate spacing);
    // Background flow (2026-11-21): the pose of a route position's car now. car: the real car of that position (its station follows it),
    // or null while the position is only simulated (it moves along its route). Returns false when the position has no route geometry.
    using FlowPose=bool (*)(int slot,const FvoPoolSpot* car,FvoPoolSpot& out);
    void SetFlowPose(FlowPose pose);
    // Census copy for core readers (trafficcensusapi.h): optional; null when the core has no such export.
    void SetCensusPublisher(FvoTrafficCensusPublishV1 publish,FvoTrafficCensusForgetV1 forget);
    void SetBirthListener(FvoTrafficBirthFnV1 listener);
    void SetHandoffListeners(handoff::ReadFacts facts,handoff::Release release,handoff::RetireShow retire);
    void SetRoadClearance(int (*read)(FvoTrafficVehicle*,int), FvoRoadCheckFnV1 check, FvoTrafficBirthFnV2 birth);
    void ReportDriverRetirement(unsigned vehicle,unsigned driver);
    int DecideParkedRetirement(unsigned vehicle,const char* reason,bool protectedIdentity);
    void SetIdAdapter(unsigned (*adapter)(unsigned)); // stored form ids to this load order
    void SetInitialSpotKeys(const std::vector<std::string>& keys); // card HR1: Demand's spot key per initial slot (route/lane/10 m cell)
    // C4: Health's broken pairs (not the world's), for the coordinator to tell the core they are out of traffic. Returns the count.
    int OutOfTraffic(unsigned* vehicles, int max);
    void SetHealthSources(int (*sample)(unsigned,FvoPairHealthSampleV1*),void (*spot)(unsigned));  // CH-1/CH-4: core exports, optional
    void SetPlayClock(unsigned (*fn)());  // W1: core FVO_PlayClockV1, optional
    using FleetPath = bool (*)(unsigned vehicle, const FvoTrafficRef&, std::vector<FvoFleetRailPoint>&, double& speed);
    void SetPhysicalFleet(FvoFleetRailAssignV1Fn assign, FvoFleetRailReleaseV1Fn release,
                          FvoFleetRailStateV1Fn state, FleetPath path);
    bool PhysicalFleetActive();
    unsigned FleetPriority(double distance);
    void SetCameraAhead(int (*fn)(double x,double y,double z,double seconds,double* turnDegPerS));  // CP-1: core FVO_CameraAheadV1
    void SetCameraEnvelopeAhead(int (*fn)(double x,double y,double z,double seconds,double radius));  // optional, for deep falls
    void SetCameraEvidence(cameravisibility::Reader reader,cameravisibility::ForecastReader forecast);
    void SetBirthRefusal(FvoBirthRefusalFnV1 refusal);  // core FVO_TrafficBirthRefusalV1: the core's own reason for a refused birth
    void SetRemovalPrediction(bool on);  // CP-1: settings.ini [traffic] removeByCameraPrediction (Retirement also removes by camera prediction)
    void SetSpotTest(int (*fn)(unsigned vehicle));  // ST-B: the core's FVO_SpotTestV1 (null: an older core, no spot-test fact)
    void SetRelocation(FvoRelocationStatusFn fn);   // the core's FVO_TrafficRelocationV1 (null: an older core, no relocation receipts)
    bool HealthModel(unsigned model, FvoTrafficHealthModelV1& out);  // card FR1: Health's published record (any thread)
    void HealthAllow(unsigned model, bool allow);                    // card FR1: your call, applied by Health on its next tick (any thread)
    bool HealthReason(unsigned model, char* out, int n);             // card RI-4: the last fail reason of this model (any thread)
    bool HealthSpot(const char* route, int index, FvoTrafficHealthSpotV1& out);  // card RI-4: the index-th rejected spot of a route (any thread)
    void SetModelRules(const std::vector<std::vector<unsigned>>& skip); // per position (as SetSpots): models it may not hold (vehicle rules)
    void SetInitialScene(const FvoPoolSpot* spots,int count); // active, distributed lane positions; covered initialization only
    void SetInitialRoads(const std::vector<bool>& roads); // same covered admission, existing grounded birthSite adapter
    void SetInitialAlternatives(int slot,const FvoPoolSpot* spots,int count);
    void WaitForInitialClock(bool pending);
    bool InitialVehicle(int spot); // callback binding may select the authored initial route/lane
	void Tick();          // full update frames
	void SetTripReaders(const char* (*refusal)(unsigned vehicle,int* on,int* want),int (*routeRoom)(int slot,int* on,int* want,const char** route));  // SR-1/SR-3: the route side's facts
	int CoverClosingNow(int kind);  // CC-1: the core's cover owner reports the close frame; -1 when traffic does not run here
	void Housekeeping();  // paused frames: reconcile and safely admit pending capacity
	void Reset();         // a save load
	void Rush(const FvoRushPlace* places, int count);  // any thread
	void RushStatus(FvoRushState* out);                // any thread
    void SetLandingSpots(const FvoPoolSpot* spots,const char* const* names,int count); // the world's landing spots (kept in trafficpool.ini per world)
    void SetSavedFleetJourney(bool (*matches)(unsigned vehicle,unsigned base,const char* world));
    void SetPanelCover(int (*covered)());
}
