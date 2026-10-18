#pragma once

#include <cstdint>

// Passenger mode (2026-10-17): the player rides in a vehicle an NPC job is flying; the NPC's autopilot does the
// driving. Engine (1.16.244, notes/VEHICLE_INTERACTIONS_RESEARCH_20261017.md): an actor's seat is its
// PassengerInteraction (extra data 0xA5) whose +0x20 is the seat's keyword (null = driver: the player's own get-in,
// FUN_14219BE30, leaves it null and then makes the player the driver). FUN_141995460(actor, vehicle, &link) builds and
// starts that interaction; its seat is set to the vehicle's companion seat before FUN_141A9C8C0(actor) completes it, and
// FUN_14219BE30 (the driver setup) is never called. Out: FUN_141A9C530(actor), then the player is set beside the vehicle.
// While riding, the player's view picks the heading and W / S the cruise speed.
namespace passenger
{
	void Tick();   // main thread, every frame (the passenger key, riding checks)
	void Reset();  // a save load
	// The vehicle the player rides as a passenger (form id), or 0.
	std::uint32_t RidingVehicle();
	// npcvehicles, while its job flies the vehicle the player rides: flies it where the player looks.
	void Steer(int pilot, std::uintptr_t vehicle);
}
