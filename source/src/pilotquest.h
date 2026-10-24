#pragma once

#include <cstdint>

// Quest-package pilots (2026-10-17): the data-driven way to send ANY actor to a vehicle. FVO_NPCPilots.esm
// (scripts/make_pilots_esm.py) holds one start-enabled quest with 6 pilot aliases and 6 vehicle aliases; each pilot alias
// carries a quest package "activate the vehicle alias". Filling the aliases with the engine's own ForceRefTo
// (Papyrus ReferenceAlias.ForceRefTo -> FUN_141ff7b80 -> FUN_140cfc990(quest, aliasId, ref, 6)) makes the actor run that package
// with a quest package's priority (above its own schedule), so it walks to the vehicle and activates it: the game's get-in.
namespace pilotquest
{
	// The plugin and its quest are loaded and running (probed every few seconds; logs once what it found).
	bool Available();
	// Slot 0..5: the vehicle alias, then the pilot alias; then the actor re-evaluates its packages. Returns false with the reason logged.
	bool Assign(int slot, std::uintptr_t npc, std::uintptr_t vehicle);
	// Logs (after delayMs) what the pilot alias holds and which package the actor runs; call from the main thread each frame via Tick.
	void Diagnose(int slot, std::uintptr_t npc, unsigned delayMs);
	void Tick();
	// The control test: points the slot's vehicle alias at another reference (any plain object).
	bool Retarget(int slot, std::uintptr_t ref, std::uintptr_t npc);
	// The second quest: the slot's pilot alias carries an ACTIVATE package on the vehicle alias (the game's own Activate handler for vehicles).
	bool AssignActivate(int slot, std::uintptr_t npc, std::uintptr_t vehicle);
	// Empties both aliases of the slot (the actor goes back to its own packages).
	void Release(int slot);
}
