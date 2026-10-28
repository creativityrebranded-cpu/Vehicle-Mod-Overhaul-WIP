#pragma once

// View distance for what the mod cares about (2026-10-18, the user: "I just want to be able to see the vehicles from far").
// The game fades 3D objects by distance / multiplier, per category: vehicles are furniture, which falls in the "objects" category
// (FUN_140583230 picks the category from the base form type: NPC_ = actors, items = items, everything else = objects). Logs\FlyingVehicleOverhaul\settings.ini:
//   [view]  objectFade=0   ; native category distance fade disabled for loaded objects (default 0; positive = engine multiplier)
//           actorFade=4    ; the same for actors (default 1)
// Applied by writing the setting values (fFadeOutMultObjects / fFadeOutMultActors :ViewDistance) and asking the game to rebuild its table
// (FUN_140573d40), which is also what it does itself when a setting changes. Main thread; re-read every 5 s.
// This affects the objects category, including vehicles. It does not load distant
// cells, retain references, bypass every culling stage, or create replacement dots.
namespace viewdist
{
	void Tick();
	void Reset();
}
