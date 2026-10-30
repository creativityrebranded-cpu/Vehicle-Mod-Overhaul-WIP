#pragma once
#include <cstdint>

// The vehicle chain probe (2026-10-21: no vehicle moved, driving or hover, and the hover hook saw 0 physics steps).
// notes/VEHICLE_SYSTEM_RESEARCH_20261021.md has the chain: VehicleRuntimeComponent update (FUN_14131D5F0) -> its Havok vehicle
// instance (component +0x10) -> the physics step (FUN_1423CB260, our hook). Every 5 s it logs, per vehicle near the player,
// each link: the component and its update count, the instance, the step count, the player-vehicle flag, the freeze-on-exit
// setting and how far the vehicle moved. The first link that reads 0 is the broken one.
namespace vehiclediag
{
	bool PrepareForDataLoad();               // before persistent vehicle records are read
	void Install();                          // hooks the vehicle component update (counting only)
	void NoteStep(std::uintptr_t instance);  // from the physics step hook, every call (any owner)
	void Tick();                             // main thread, once per frame
	std::uintptr_t ComponentOf(std::uint32_t formId);  // the VehicleRuntimeComponent last updated for this vehicle form id, or 0
	// A save load: forget every component pointer of the old game (they may be freed) and the per-vehicle repair state.
	void Reset();
    void StageBirth(std::uint32_t formId); // creation callback, before the chassis can attach
    bool ReleaseBirth(std::uint32_t formId); // once its flight controller is armed and the game has built its physics vehicle
    bool Built(std::uint32_t formId); // the game built its physics vehicle (instance) since the last reset
    void DropBirthHold(std::uint32_t formId); // hidden/replaced/deleted: forget the hold, motion untouched
    bool BirthStageAvailable();
    int StagedLoaded(); // staged chassis that have physics (loaded) and still wait for their autopilot
}
