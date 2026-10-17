#pragma once

#include <cstdint>

// The ship model for flight (the player becomes the ship). A copy of the home ship's loaded 3D is made with the
// engine's own scene-graph clone (NiCloningProcess: vtable +0x148 CreateClone, +0x178 ProcessClone, as the
// clone pools do), stripped of collision so it is only visual, and attached to the player's 3D root
// (NiNode::AttachChild, vtable +0x2A0) with the cockpit placed on the player, so the view is from the pilot
// seat. The player's own body is hidden while it is shown. The real ship is never touched.
//
// Scene-graph work runs in the per-frame game tick (gametick.h, inside the game's own update), never from the plugin
// task. (The first version ran it in the grid-update hook, which only sees the player on cell changes: it never ran.)
namespace shipmodel
{
	// Main thread (plugin task): what we want.
	void Show(bool show);
	// Vehicle plugin: display the copied hull on this driven vehicle's 3D instead of the player.
	// A null target restores the vehicle's own visible parts. Its Havok chassis is untouched.
	void ShowOnVehicle(std::uintptr_t vehicle);
	void SetPitch(float radians);  // nose up/down with the look direction
	// The flown attitude shown on the hull: pitch (nose up positive) and roll (right wing down positive), radians,
	// about the cockpit. Yaw comes from the player's own heading.
	void SetAttitude(float pitch, float roll);
	// The flown heading (world yaw, radians): the hull turns by (heading - the player's own heading), so the player's
	// rotation (and the camera) is never written.
	void SetHeading(float yaw);
	// Clone the home ship now if its 3D is loaded (used before leaving the pilot seat), without showing it.
	void Capture();
	// Snapshot the player's own 3D as posed now (seated in the pilot seat): flown as the pilot in the cockpit.
	void CapturePilot();
	bool PilotReady();

	// Cockpit position in the ship's own frame (metres from the ship's origin), once a clone exists.
	bool CockpitOffset(float out[3]);
	// Whether a clone exists / showing failed because the ship's 3D was never loaded.
	bool Ready();
	bool Unavailable();

	// Forget the clone and attachments (loading a save destroys the player's 3D).
	void Reset();

	// Called every frame from the game tick.
	void OnPlayerUpdate(std::uintptr_t player);
}
