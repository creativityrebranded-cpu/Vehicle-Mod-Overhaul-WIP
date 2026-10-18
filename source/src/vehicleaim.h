#pragma once

// Vehicle weapon aim at launch (research: notes/VEHICLE_WEAPON_AIM_RESEARCH_20261017.md). Projectile::Launch
// (FUN_141B3BD50, (out handle, ProjectileLaunchData*, int)) builds the projectile's orientation from the launch data's
// angles (+0x24 pitch, +0x2C heading; confirmed in game); the shooter is at +0x38. For an NPC job's vehicle the angles are turned at
// the led aim point (npcvehicles::AimFor) before the projectile is built. The player's own rover shots are only logged
// (launch angles next to the camera's), to confirm the angle convention.
namespace vehicleaim
{
	void Install();
}
