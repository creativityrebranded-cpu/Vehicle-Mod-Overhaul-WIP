#pragma once
#include <cstdint>
#include <string>

// The controls toolkit (2026-10-21, the user: "set the buttons for each, that way it'll work for anything incl controllers").
// Every action of the mod reads its input here. Each one is set in Logs\SeamlessVehicle\bindings.ini as a list of
// alternatives (keys, mouse buttons, controller buttons, controller chords; bindlogic.h has the syntax), reloaded while the
// game runs. "default" keeps the key the mod picks for it, or the game's own binding it follows, and that binding's
// controller button where the game has one. The controller is read through XInput, so any XInput pad works whatever the
// game itself does with it.
namespace bindings
{
	enum class Action
	{
		Scanner,        // T: tap go / pick, double tap clear, hold ram (and focus fire on foot in combat)
		Summon,         // N
		Cover,          // U
		Stance,         // the stance key
		HoverToggle,    // hover on / off (besides double-tapping the vertical boost)
		LiftUp,         // hover: up
		LiftDown,       // hover: down
		Gear,           // Alt: tap gear, hold descend
		Boost,          // LShift
		Accelerate,     // the game's VehicleAccel (W)
		Brake,          // the game's VehicleBrake (S)
		TurnLeft,       // the game's VehicleTurnLeft (A); hover: roll left
		TurnRight,      // the game's VehicleTurnRight (D); hover: roll right
		VerticalBoost,  // the game's VehicleVertBoost (Space); double tap: hover
		RideAlong,      // passenger: ride along / get out
		RetryTrip,      // route: retry
		SkipStop,       // route: skip
		OwnProfile,     // your vehicle's profile
		ShowHints,      // J: every hint and panel
		Count
	};

	// Once per frame (the vehicle task): bindings.ini changes, the controller's state.
	void Tick();
	// Held now. `defaultVk` is the key the calling module would use ("default").
	bool Held(Action a, int defaultVk);
	// What the HUD and notices show: "T / LB+DUp"; "unbound" when nothing is set.
	std::string Label(Action a, int defaultVk);
	// A controller is connected (XInput).
	bool PadConnected();
}
