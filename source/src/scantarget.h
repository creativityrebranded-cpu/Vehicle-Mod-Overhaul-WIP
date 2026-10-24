#pragma once
#include <string>

// Scanner marker button (2026-10-18). While the scanner is open the plugin shows the distance to what the player is looking at (a ray along the view through
// prefetch::RequestRay: terrain and structures; beyond what is loaded, the flat ground plane under the player, shown with "~"), and the scanner key (T; ini
// Logs\FlyingVehicleOverhaul\settings.ini [scanner] key=<virtual-key code>) sets the game's own custom map marker there (a small magnetic pull to the structure the ray hit),
// then chains the vehicle: the player's own vehicle is summoned / follows, and when the player gets in its Auto flies to the marker by itself.
namespace scantarget
{
	void Tick();   // main thread, every frame
	void EndRide(); // the driver exited: discard route and selected-target context
	void Reset();  // a save load
	// The scanner key's name (T by default). Outside the scanner while driving, a tap of it turns Auto on / off.
	std::string KeyName();
}
