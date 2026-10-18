#pragma once

#include <cstdint>

// NPCs getting into vehicles (notes/NPC_VEHICLE_STAGE1_RESEARCH_20261017.md). Nothing automatic and nothing
// combat-related (removed 2026-10-17 at the user's request): the force key ',' sends the NPC the player faces into the
// nearest free vehicle (the game's get-in package; the vehicle's driver-seat keyword given when the seat refuses), and
// aboard it follows the player like the rover follow. The game's put-the-player-in call seats that NPC instead of the
// player for vehicles an NPC job uses.
namespace npcvehicles
{
	void Install(); // hooks the game's put-the-player-in-this-vehicle call (seats the NPC instead)
	void Tick();   // main thread, every frame
	void Reset();  // a save load
	bool UsesVehicle(std::uint32_t vehicleId);  // an NPC job is boarding / driving it
	// The pilot (1..) whose NPC drives this vehicle (a job following), else 0.
	int PilotFor(std::uint32_t vehicleId);
	// Take the wheel: stops that job and takes its NPC out at once (the NPC's form id, 0 when nobody drives it).
	std::uint32_t TakeDriver(std::uint32_t vehicleId);
	// Seats the player as the driver of the vehicle (the game's own put-the-player-in), true when the game agrees.
	bool SeatPlayerDriver(std::uintptr_t vehicle);
	const char* ProfileOf(std::uint32_t vehicleId);  // "escort" / "pursuit" for a vehicle an NPC drives, else null
	// Any thread (vehicleaim's projectile launch hook): the led world aim point (player chest + velocity x flight time,
	// ref +0x8C frame) for a job allowed to fire whose vehicle or NPC is `shooterFormId`; false otherwise.
	bool AimFor(std::uint32_t shooterFormId, double& x, double& y, double& z);
}
