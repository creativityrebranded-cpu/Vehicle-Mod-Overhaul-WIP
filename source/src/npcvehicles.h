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
	const char* ProfileOf(std::uint32_t vehicleId);  // "escort" / "pursuit" for a vehicle an NPC drives, else null
}
