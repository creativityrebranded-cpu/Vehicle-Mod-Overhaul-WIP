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
	// The game's load routine is about to run (load hook thread): note each listed vehicle's reference and cell object and
	// its form flags, to tell after the load whether a vehicle without a component stayed in memory through it.
	void NoteBeforeLoad();
	// Every frame from the load routine's return until the world's first live frame: the native attach for noted vehicles
	// kept in memory through the load without a component (confirmed in game 2026-11-17).
	void AfterLoadTick();
    void StageBirth(std::uint32_t formId); // creation callback, before the chassis can attach
    bool ReleaseBirth(std::uint32_t formId); // once its flight controller is armed and the game has built its physics vehicle
    bool ReleaseBirthToPlayer(std::uint32_t formId); // KO-2: a vehicle the player took; true when released or never held
    bool Built(std::uint32_t formId); // the game built its physics vehicle (instance) since the last reset
    void DropBirthHold(std::uint32_t formId); // hidden/replaced/deleted: forget the hold, motion untouched
    bool BirthHeld(std::uint32_t formId); // AC-4 (read-only): the vehicle is under its birth hold (not yet released to its pilot)
    bool BirthStageAvailable();
    std::uint64_t BirthCorrectionGeneration(std::uint32_t id);
    int StagedLoaded(); // staged chassis that have physics (loaded) and still wait for their autopilot
    // B2 (read-only, 2026-11-07): how many physics-world changes the existing hooks have counted so far (vehicle body builds and
    // static/keyframed motion-type sets). Rising means the broadphase was just rebuilt; rays read it and wait briefly.
    std::uint64_t PhysicsChangeCount();
    // Z-1 (read-only, 2026-11-07): when the game last built this vehicle's physics body (GetTickCount64), 0 = never seen. The
    // existing build hook already records it (NoteBuilt); the spot test re-checks a car whose body was (re)built.
    unsigned long long BuiltAt(std::uint32_t vehicle);
}
