#pragma once

#include <cstdint>

// Vehicle health (2026-10-21: health, driver share, wreck; see vehiclehits.cpp) on top of the hit counter (2026-10-20, the user: "since vehicles don't have life just place in a counter ... at about a certain damage number that would've
// affected a player it should just turn off autopilot"). Vehicles take no damage in the game, so every projectile launched (vehicleaim's
// Projectile::Launch hook reports its handle) is followed frame by frame; a projectile whose path passes through a vehicle's space (the world
// list's vertical cylinder) counts as a hit for that vehicle, unless the shooter is that vehicle or sits in it. Damage per hit and the limit
// use the native projectile weapon total and the game's player difficulty calculation, scaled by [hits] weaponScale=1 (float). [hits] damage=20
// is used only when native data cannot be read. Health/limit come from Logs\SeamlessVehicle\seamless.ini [hits] (health=300, limit= overrides health,
// driverShare=25 percent). At 0 health the
// vehicle is wrecked (autopilot off, renewed until repaired over 25%); the counter heals after a quiet spell (a wreck later and slower).
namespace vehiclehits
{
	// Any thread (the launch hook): a projectile was launched by `shooterFormId` (0 unknown); `handle` is its reference handle.
	void NoteLaunch(std::uint32_t handle, std::uint32_t shooterFormId);
	void Tick();   // main thread, every frame, after world::Publish
	void Reset();  // a save load
    struct Ballistics { double speed=0, range=100; bool nativeRange=false, observedSpeed=false; };
    Ballistics BallisticsOf(std::uint32_t shooter); // main thread; speed 0 means use direct aim until observed
    double ShotTime(std::uint32_t shooter,double distance);
	// The damage a vehicle carries now and the limit (for the HUD / logs); false when it was never hit.
	bool DamageOf(std::uint32_t vehicleFormId, float& damage, float& limit);
	// The player's health now as a share of the highest seen (0..1); false when it cannot be read.
	bool PlayerHealthShare(float& share);
	// Any actor's health now as a share of the highest seen for it (0..1; the first reading counts as full); false when unreadable.
	bool ActorHealthShare(std::uintptr_t actor, float& share);
	// Vehicle health (2026-10-21): the share left (1 = never hit), when it was last hit (GetTickCount64, 0 never), whether it is wrecked.
	float HealthShare(std::uint32_t vehicleFormId);
	unsigned long long LastHitMs(std::uint32_t vehicleFormId);
	bool Wrecked(std::uint32_t vehicleFormId);
	// The vehicle most recently hit by the player's side (the HUD lifebar); false when none.
	bool LastHitByYourSide(std::uint32_t& vehicleFormId, unsigned long long& at);
	// The form id under the crosshair (the HUD enemy list's target handle; 0 none).
	std::uint32_t CrosshairTarget();
	// The game's enemy bar (2026-10-21): the vehicle whose driver's bar the HUD shows now (aimed at the vehicle or the driver),
	// 0 none; the vehicle's own bar goes under it (hoverui). InstallEnemyBar hooks the HUD enemy list (once, at load).
	std::uint32_t EnemyBarVehicle();
	void InstallEnemyBar();
	// An actor's Health now and maximum, as the game's enemy bar computes them.
	bool ActorHealth(std::uintptr_t actor, float& now, float& max);
	// Damage from outside the projectile follower (rams: combat_tactics::RamDamage): the same health, driver share and wreck. side as the shots'.
	void ApplyDamage(std::uint32_t vehicleFormId, float amount, std::uint32_t by, int side, const char* what);
	// A vehicle's max health (ini [hits] health=, or limit=) and the share of it one ram may take (ini [hits] ramCap= percent, 35).
	float MaxHealth();
	float RamCap();
	// When `shooterFormId` last launched a projectile (GetTickCount64, 0 never seen): hearing gunfire.
	unsigned long long LastShotBy(std::uint32_t shooterFormId);
}
