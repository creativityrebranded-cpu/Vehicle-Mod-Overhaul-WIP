#pragma once

// Vehicle profiles on the contract (vcontract.h). Flight-only profiles that need no seat changes:
//   NPC driving:            Patrol, Hauler, Courier, Hold, ReturnHome
//   your vehicle, empty:    Scout, ReturnHome, Hold
//   you driving (Auto on):  Cruise
// Keys (picked free at start; the log names them): '[' cycles the profile of every NPC vehicle following you,
// ']' cycles your own vehicle's (on foot: Scout / ReturnHome / Hold; driving: Cruise). Default = the owner's own
// behaviour (Escort / Pursuit for NPCs; Summon / Follow / Auto for yours).
namespace profiles
{
	void Install();  // registers the profiles with vcontract
	void Tick();     // main thread, every frame: the keys
	void Reset();    // a save load
}
