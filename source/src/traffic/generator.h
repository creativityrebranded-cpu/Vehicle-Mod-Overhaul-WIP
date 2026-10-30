#pragma once
// Stable world-owned fleet. Reconcile edits without replacing or moving healthy pairs.
// Missing capacity departs from validated authored ground origins under map cover or
// proven concealment with observation memory. The core enforces engine-side admission.
// Driver/body readiness precedes trips; failures retain identities and defer safely.
#include "traffic_api.h"

namespace generator
{
	void Attach(const FvoTrafficApi* api, FvoLog_t log);
	// Authored ground origins for stable fleet names, then parking places with parking[i] = 1.
	void SetSpots(const char* world, const FvoPoolSpot* spots, int count, const int* parking, const char* const* names, FvoPoolTrip trip);
    void SetInitialScene(const FvoPoolSpot* spots,int count); // active, distributed lane positions; covered initialization only
    void SetInitialAlternatives(int slot,const FvoPoolSpot* spots,int count);
    void WaitForInitialClock(bool pending);
    bool InitialVehicle(int spot); // callback binding may select the authored initial route/lane
	void Tick();          // full update frames
	void Housekeeping();  // paused frames: reconcile and safely admit pending capacity
	void Reset();         // a save load
	void Rush(const FvoRushPlace* places, int count);  // any thread
	void RushStatus(FvoRushState* out);                // any thread
}
