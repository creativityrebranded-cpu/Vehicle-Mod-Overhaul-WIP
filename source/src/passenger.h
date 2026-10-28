#pragma once

#include <cstdint>

// Passenger mode (2026-10-17): the player rides in a vehicle an NPC job is flying; the NPC's autopilot does the
// driving. Engine (1.16.244, notes/VEHICLE_INTERACTIONS_RESEARCH_20261017.md): an actor's seat is its
// PassengerInteraction (extra data 0xA5) whose +0x20 is the seat's keyword (null = driver: the player's own get-in,
// FUN_14219BE30, leaves it null and then makes the player the driver). FUN_141995460(actor, vehicle, &link) builds and
// starts that interaction; its seat is set to the vehicle's companion seat before FUN_141A9C8C0(actor) completes it, and
// FUN_14219BE30 (the driver setup) is never called. Out: FUN_141A9C530(actor), then the player is set beside the vehicle.
// While riding, the driver's own job flies the vehicle; the player has no steering (2026-10-28), only exit / take the wheel.
namespace passenger
{
	void Tick();   // main thread, every frame (the passenger key, riding checks)
	void Reset();  // a save load
	// The vehicle the player rides as a passenger (form id), or 0.
	std::uint32_t RidingVehicle();
	// Takes the companion seat of ANY vehicle that has a driver (tap on an occupied vehicle); true when seated.
	bool BoardVehicle(std::uintptr_t player, std::uintptr_t vehicle);
	// The keyword of the vehicle's first passenger seat (after the driver's), 0 when it has none.
	std::uintptr_t PassengerSeatKeyword(std::uintptr_t vehicle);
	// The game's get-in put the player at the passenger seat (npcvehicles' seat choice): builds the player's seat link with that
	// seat's keyword and finishes it (in place of FUN_14219BE30, which makes the player the driver); passenger mode on success.
	bool FinishPassengerSeat(std::uintptr_t player, std::uintptr_t vehicle);
	// npcvehicles, while its job flies the vehicle the player rides: flies it where the player looks.
	void Steer(int pilot, std::uintptr_t vehicle);
}
