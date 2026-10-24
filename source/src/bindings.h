#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "bindingdefs.h"

// The controls toolkit (2026-10-21, the user: "set the buttons for each, that way it'll work for anything incl controllers").
// Every action of the mod reads its input here. Each one is set in Logs\FlyingVehicleOverhaul\bindings.ini as a list of
// alternatives (keys, mouse buttons, controller buttons, controller chords; bindlogic.h has the syntax), reloaded while the
// game runs. "default" keeps the key the mod picks for it, or the game's own binding it follows, and that binding's
// controller button where the game has one. The controller is read through XInput, so any XInput pad works whatever the
// game itself does with it.
namespace bindings
{

	// Once per frame (the vehicle task): bindings.ini changes, the controller's state.
	void Tick();
	void ResetRuntime();
	bool Triggered(Action a);
	bool Started(Action a);
	bool Allowed();
	// Held now. `defaultVk` is the key the calling module would use ("default").
	bool Held(Action a, int defaultVk);
	// The same as an amount 0 .. 1: a stick direction or trigger by how far it is pushed (past a deadzone), keys and
	// buttons 1. For analog control (the physical VTOL roll and throttle).
	double Amount(Action a, int defaultVk);
	// What the HUD and notices show: "T / LB+DUp"; "unbound" when nothing is set.
	std::string Label(Action a, int defaultVk);
	// The same, with the default key its own module last asked with (for the HUD, which does not know that key).
	std::string Label(Action a);
	// A controller is connected (XInput).
	bool PadConnected();
	// One of the game's own actions (e.g. "MainGameplay", "Activate") as the game binds it: its keyboard key (fallbackVk
	// when it binds none) or its controller button. GameLabel names the one for the device in use.
	bool GameHeld(const char* context, const char* event, int fallbackVk);
	std::string GameLabel(const char* context, const char* event, int fallbackVk);
	// The controller was used more recently than the keyboard / mouse (prompts name its buttons then).
	bool UsingPad();
	// A controller button's on-screen name: Xbox, or PlayStation with a Sony pad attached ([display] pad_names overrides).
	std::string PadName(std::uint32_t bit);
	// Gameplay Options: selecting a control captures its next input and saves it.
	const char* ActionName(Action a);
	const char* ActionDescription(Action a);
	std::string MenuLabel(Action a);
	unsigned Generation();
	void BeginCapture(Action a);
	void CancelCapture();
	bool Capturing(Action* action = nullptr);
	void CaptureTick(); // settings movie thread, only while Gameplay Options is visible
	void ResetAll();

	// The game's Controls page (controlspage.cpp): every action is one row per device whose slots are main (0) and alt (1).
	// "FVO_scanner": the row's user event and the middle of its translation key.
	const char* EventName(Action a);
	bool FromEvent(const char* event, Action* a);
	// The device's inputs in slot order, "default" resolved to the key / button it stands for now.
	std::vector<bindlogic::Input> Slots(Action a, bool pad);
	// Rebind just this scoped device slot. Reject a real overlap without altering
	// any other command; different contexts and gestures can share a button.
	bool Assign(Action a, bool pad, int slot, bindlogic::Input in);
    bool CanAssign(Action a, bool pad, int slot, bindlogic::Input in);
	void ClearSlot(Action a, bool pad, int slot);
}
