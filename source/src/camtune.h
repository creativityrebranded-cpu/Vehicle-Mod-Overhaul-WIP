#pragma once

// In-game camera tuning for ship flight, saved to Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets\camera.ini.
// While flying (game window focused):
//   Page Up / Page Down  third-person distance (the zoom is held at this value: min and max zoom both set)
//   Home / End           third-person height (fOverShoulderPosZ)
//   Numpad 8 / 2         first-person eye forward / back
//   Numpad 6 / 4         first-person eye right / left
//   Numpad 9 / 3         first-person eye up / down
//   Numpad 5             reset to defaults
namespace camtune
{
	struct Eye
	{
		float forward = 0, right = 0, up = 0;  // metres, in the ship's frame, added to the cockpit eye point
	};

	void Load();               // at startup
	void Begin();              // flight starts: apply the third-person values (originals kept)
	void End();                // flight ends: restore the game's values
	void Tick();               // per frame while flying: keys
	Eye EyeOffset();
}
