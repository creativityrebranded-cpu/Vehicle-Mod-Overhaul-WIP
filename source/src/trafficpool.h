#pragma once
// Stable traffic fleet facade: the addon reconciles configuration and the core validates engine actions.
namespace trafficpool
{
	void Tick();   // main thread, every frame
	bool WellHiddenFar(unsigned vehicleId,double distance);  // past the distance that vehicle can be made out at
	bool PutBack(unsigned id,double x,double y,double z);    // back to where it was, no sight check: undoes the game's own move (vehiclediag)
	bool MarkDriverForFarPass(unsigned driverId);  // something attached to the driver: the cell unload skips it. True: it has that now
	int PairHealth(unsigned vehicleId);  // trafficpair::PairHealth as an int; a vehicle that is no traffic pair is 0 (unproven)
	void Reset();  // a save load
    void WorldActionsAllowed(bool allowed); // main scheduler: false during save loads and world teardown
    void ArrivalChecks(int& rayChecked,int& mapOnly,bool reset); // arrival placements since the last reset, by clearance check
	// Start/Swap requests reconciliation; existing identities and live trips are retained.
	// Legacy lane-place arguments are accepted for ABI compatibility, never airborne relocation.
	struct RushPlace { double x, y, z, heading; };
	void Rush(const RushPlace* places, int count);
	void RushTick();  // main thread, housekeeping frames (paused) and every Tick
	// phase 1 reconciliation; moved reports retained identities, skipped reports pending admission
	struct RushState { bool active = false; int phase = 0, total = 0, done = 0, moved = 0, made = 0, skipped = 0, placed = 0, seated = 0, driving = 0; };
	RushState RushStatus();
	// Everyday traffic (stage 2, 2026-10-29): core's civil scan hands the pick to FVO_Traffic.dll; 1 = it started a trip.
	int Everyday();
	// FVO_Traffic.dll is attached; ask it to re-read its traffic files (any thread).
	bool AddonAttached();
	void ReloadFiles();
}
