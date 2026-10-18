#pragma once

// View distance for what the mod cares about (2026-10-18, the user: "I just want to be able to see the vehicles from far").
// The game fades 3D objects by distance / multiplier, per category: vehicles are furniture, which falls in the "objects" category
// (FUN_140583230 picks the category from the base form type: NPC_ = actors, items = items, everything else = objects). Logs\SeamlessVehicle\seamless.ini:
//   [view]  objectFade=3   ; multiplies the fade distance of every object (default 3; 1 = the game's own; 0 = never fade)
//           actorFade=4    ; the same for actors (default 1)
// Applied by writing the setting values (fFadeOutMultObjects / fFadeOutMultActors :ViewDistance) and asking the game to rebuild its table
// (FUN_140573d40), which is also what it does itself when a setting changes. Main thread; re-read every 5 s.
namespace viewdist
{
	void Tick();
	void Reset();
}
