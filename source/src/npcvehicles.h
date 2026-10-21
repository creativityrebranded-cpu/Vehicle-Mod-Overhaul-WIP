#pragma once

#include <cstdint>

#include "vcontract.h"

// NPCs getting into vehicles (notes/NPC_VEHICLE_ENTRY_RESEARCH_20261018.md, notes/MINTING_STATUS_20261018.md). The BOARD key (;) sends the
// NPC the player faces to the nearest free vehicle: the SeamlessPilots.esm quest package walks it there (running in combat or when the vehicle
// is over 40 m away), the seat keyword is given, and the instant seat puts it in (the animated entry is paused: kAnimatedEntry = false);
// aboard it follows the player. The EXIT key (Insert) and RequestExit take NPCs out (the game's dismount, then the instant exit). The context
// engine (context.cpp) can also board fighters by itself. The game's put-the-player-in call seats the NPC instead of the player for
// vehicles an NPC job uses.
namespace npcvehicles
{
	void Install(); // hooks the game's put-the-player-in-this-vehicle call (seats the NPC instead)
	void BeginFrame(); // native driver index once per frame, before world::Publish
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
	// The player's own vehicle's context behaviours (vehicleflight: overwatch, scout). The nearest enemy of the player around (x, y, z) within `range`:
	// fighting actors hostile to the player, enemies in vehicles first; `count` = how many enemies there are; 0 when none.
	std::uintptr_t EnemyNear(std::uintptr_t player, double x, double y, double z, double range, bool& inVehicle, int& count);
	// The vehicle weapon of a vehicle no NPC job drives (the player's own rover on overwatch): held while `fire`; the shots are turned at (x, y, z).
	bool FireVehicleWeapon(std::uint32_t vehicleId, bool fire, double x, double y, double z);
	// Crew orders (2026-10-20, the user: with followers, the vehicle buttons send a follower to drive the player's own vehicle and do what was asked).
	// Follow: the follower drives it after the player (an escort: it defends, rescues, scouts); Marker: it flies to the player's map marker and
	// does the Auto behaviour there (vehicleflight::GetAutoMode: stop / orbit / patrol).
	// Recall (N), Cover (U) and Guard (T while covering) are the player's overrides (overrides.h): Recall drives to the player and the driver gets out
	// for the player; Cover follows and defends the player without chasing; Guard holds the point set by SetCrewGuard and defends from there.
	enum class CrewOrder : int { None = 0, Follow = 1, Marker = 2, Hold = 3, Recall = 4, Cover = 5, Guard = 6 };
	// A follower free to drive (on foot, not busy, alive) within `range` metres of (x, y); 0 when none.
	std::uintptr_t FreeFollower(std::uintptr_t player, double x, double y, double range);
	// That follower walks to `vehicle` (the player's own one allowed) and drives it with `order`; false when nobody could be sent.
	bool CrewBoard(std::uintptr_t player, std::uintptr_t vehicle, CrewOrder order);
	CrewOrder CrewOrderOf(std::uint32_t vehicleId);       // None: no crew job on that vehicle
	void SetCrewOrder(std::uint32_t vehicleId, CrewOrder order);
	bool SetCrewGuard(std::uint32_t vehicleId, double x, double y, double z);  // Guard at (x, y, z); false: no crew job on that vehicle
	void ReleaseCrew(std::uint32_t vehicleId);             // the crew driver gets out
	// Civil tasks (2026-10-20, the user: everyday traffic). The NPC walks to the vehicle, drives it and finally parks it in a clear spot and gets out:
	//  transit: to (x, y), then it walks the rest of its way; patrol: two laps round the vehicle's parking place, then home; a fight ends any task.
	bool StartTransit(std::uintptr_t actor, std::uintptr_t vehicle, double x, double y);
	bool StartPatrol(std::uintptr_t actor, std::uintptr_t vehicle);
	// Return home: a following job's vehicle flies back to where it was taken from and parks there (false: no home known, or already there).
	bool SendHome(std::uint32_t vehicleId);
	int CivilJobs();                                         // jobs running a civil task now
	// The actor may take this vehicle: unowned, or owned by the actor or a faction it belongs to (never the player's).
	bool MayUse(std::uintptr_t actor, std::uintptr_t vehicle);
	// An enemy in a fight may take this vehicle (2026-10-21, the user): one its own faction (or itself) owns, or an unowned one
	// standing in its own camp (the place's owner is its faction). Parked town vehicles stay put. Never the player's.
	bool FighterMayTake(std::uintptr_t actor, std::uintptr_t vehicle);
	// Main thread: the game's own exit for the player (FUN_141952530, with its obstruction checks); true when it let them out.
	bool ExitPlayer();
	bool ActorFree(std::uintptr_t actor);                    // alive, on foot, no job, not a follower
	// Supply run: drives to (x, y) (another settlement / outpost), waits there, comes back, parks.
	bool StartSupply(std::uintptr_t actor, std::uintptr_t vehicle, double x, double y);
	// Recall: every escort drops its fight for `ms` and comes back into formation.
	void Recall(unsigned ms);
	// The player's stance (the stance key, 2026-10-21): it sets the Combat and Weapons classes of the player's own vehicle and every follower's
	// vehicle. Defend (the start), Attack, Hold fire (defend, weapons held). The player's seat still never fires by itself.
	enum class Stance : int { Defend = 0, Attack = 1, HoldFire = 2 };
	Stance CycleStance();
	Stance CurrentStance();
	// Witness reactions (2026-10-20, notes/VEHICLE_BEHAVIOUR_RESEARCH_20261020.md): a crash at (x, y, z) -> calm bystanders within 10 m flee
	// (ActionFleeStart), within 25 m cower (ActionCower). Never followers, quest NPCs, anyone fighting or in a vehicle. Seamless.ini [witness] enabled=0 off.
	// Native friendship gate for shot hits: neutral drivers remain damageable; player and companion pairs are protected. Main thread only.
	bool VehicleFriendlyToShooter(std::uintptr_t vehicle, std::uint32_t shooterFormId);
	// Actual seated driver (player included), 0 when empty or unreadable. Main thread only.
	std::uint32_t VehicleDriverId(std::uint32_t vehicleId);
	void VehicleAggression(std::uintptr_t vehicle, std::uint32_t shooterId, bool reportCrime=true);
	void WitnessCrash(double x, double y, double z, const char* why);
	// The actor's current AI package (FUN_1418ddb70), 0 when none: a change is a schedule switch (shift change).
	std::uintptr_t CurrentPackage(std::uintptr_t actor);
	bool HostileToPlayer(std::uintptr_t actor);
	std::uint32_t NativeEnemyId(std::uintptr_t actor); // native combat target, validated hostility
	bool AreHostile(std::uintptr_t a, std::uintptr_t b);
	bool AreAllies(std::uintptr_t a, std::uintptr_t b);
	// Enemy patrol convoy: the second vehicle follows `leadVehicleId`'s patrol at 12 m and parks when the lead's job ends.
	bool StartConvoy(std::uintptr_t actor, std::uintptr_t vehicle, std::uint32_t leadVehicleId);
}
