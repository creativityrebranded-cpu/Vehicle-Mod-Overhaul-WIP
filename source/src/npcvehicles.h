#pragma once

#include <cstdint>

#include "vcontract.h"

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
	// Anyone (a job NPC, a gang driver, a stranger) sits in the driver seat of this vehicle.
	bool HasDriver(std::uint32_t vehicleId);
	// Take the wheel: stops that job and takes its NPC out at once (the NPC's form id, 0 when nobody drives it).
	std::uint32_t TakeDriver(std::uint32_t vehicleId);
	// Seats the player as the driver of the vehicle (the game's own put-the-player-in), true when the game agrees.
	bool SeatPlayerDriver(std::uintptr_t vehicle);
	// ---- Exit service (spec: vcontract.h ExitRequest / SeatState; polled). An actor aboard any vehicle gets out; a job's driver also ends its job and the
	// vehicle is Unassigned from its profile. Modes: Auto = wait landed, the game's animated dismount, then the instant exit; Normal = the first two (Refused
	// if they fail); Quick = the dismount with a short watch; Instant = the game's instant exit at once. After it the actor stands at the request's point or
	// beside the vehicle away from the player.
	bool RequestExit(std::uintptr_t actor, const vcontract::ExitRequest& req);
	int UnloadVehicle(std::uintptr_t vehicle, const vcontract::ExitRequest& req, bool includeDriver);  // how many were asked
	vcontract::SeatState ExitStateOf(std::uintptr_t actor);
	// Boarding for vcontract::SeatProvider::board: asDriver only (companion seats are not solved). The NPC walks, gets in, then follows like a job.
	bool BoardNpc(std::uintptr_t actor, std::uintptr_t vehicle, bool asDriver);
	// ---- For the context engine (context.cpp): main thread only.
	bool IsInCombat(std::uintptr_t actor);
	bool IsDead(std::uintptr_t actor);
	// The actor's combat state for the context engine: IsInCombat (vtable +0xB58) and the combat target handle (+0x298, the invalid handle = none). `fighting` = either one.
	struct CombatState { bool inCombat; std::uint32_t target; bool hasTarget; bool fighting; };
	CombatState CombatOf(std::uintptr_t actor);
	bool LooksLikeActor(std::uintptr_t ref);  // the form type check the NPC code uses (characters are class Character, which an RTTI name test for "Actor" misses)
	bool IsAlly(std::uintptr_t actor);  // one of the player's followers
	bool IsInCombat(std::uint32_t actorFormId);  // by form id (false when not loaded)
	struct JobInfo { std::uint32_t npc, vehicle; bool following, pursuit; };
	int Jobs(JobInfo* out, int cap);    // every NPC vehicle job that is not idle (boarding or following)
	bool SetVehicleProfile(std::uint32_t vehicleId, bool pursuit);  // a following job: escort (false) or pursuit (true)
	const char* ProfileOf(std::uint32_t vehicleId);  // "escort" / "pursuit" for a vehicle an NPC drives, else null
	// Any thread (vehicleaim's projectile launch hook): the led world aim point (player chest + velocity x flight time,
	// ref +0x8C frame) for a job allowed to fire whose vehicle or NPC is `shooterFormId`; false otherwise.
	bool AimFor(std::uint32_t shooterFormId, double& x, double& y, double& z);
}
