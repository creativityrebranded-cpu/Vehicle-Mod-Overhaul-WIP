#pragma once

#include "flightmodel.h"

// In-game tuning of the flight values (user, 2026-10-14: everything that can be tweaked in game is adjustable by keys;
// the user picks which become fixed). Saved to Documents\My Games\Starfield\SFSE\Logs\SeamlessPlanets\flight.ini.
// While flying (game window focused):
//   Numpad / and *   previous / next value
//   Numpad + and -   change the selected value (applies at once)
//   Numpad 0         selected value back to its default
//   Numpad 7         show the selected value
namespace flighttune
{
	void Load();                   // at startup
	void Apply(fm::Params& p);     // copy the tuned values into the model (after a model reset)
	void Tick(fm::Params& p);      // per tick while flying: keys; changes go straight into p and are saved
}
