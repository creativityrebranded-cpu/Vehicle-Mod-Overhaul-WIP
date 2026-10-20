#pragma once

#include <cstdint>

// Vehicle hit counter (2026-10-20, the user: "since vehicles don't have life just place in a counter ... at about a certain damage number that would've
// affected a player it should just turn off autopilot"). Vehicles take no damage in the game, so every projectile launched (vehicleaim's
// Projectile::Launch hook reports its handle) is followed frame by frame; a projectile whose path passes through a vehicle's space (the world
// list's vertical cylinder) counts as a hit for that vehicle, unless the shooter is that vehicle or sits in it. Damage per hit and the limit
// come from Logs\SeamlessVehicle\seamless.ini [hits] (damage=, limit=; limit 0 = the player's own health, read from the Health actor value,
// as the measure "a player could take"). At the limit the vehicle's autopilot is switched off for [hits] cooldown= seconds
// (vehicleflight::DisableAutopilot); the counter heals after a quiet spell.
namespace vehiclehits
{
	// Any thread (the launch hook): a projectile was launched by `shooterFormId` (0 unknown); `handle` is its reference handle.
	void NoteLaunch(std::uint32_t handle, std::uint32_t shooterFormId);
	void Tick();   // main thread, every frame, after world::Publish
	void Reset();  // a save load
	// The damage a vehicle carries now and the limit (for the HUD / logs); false when it was never hit.
	bool DamageOf(std::uint32_t vehicleFormId, float& damage, float& limit);
	// The player's health now as a share of the highest seen (0..1); false when it cannot be read.
	bool PlayerHealthShare(float& share);
	// Any actor's health now as a share of the highest seen for it (0..1; the first reading counts as full); false when unreadable.
	bool ActorHealthShare(std::uintptr_t actor, float& share);
}
