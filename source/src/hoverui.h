#pragma once

// Native hover buttons on the HUD's rover widget (hudmenu.swf: root.RightMeters_mc.HUDVehicle_mc, the widget that
// shows "hold [E] Exit vehicle"). The buttons are the game's own BasicButton (ButtonClips.swf) with ButtonBaseData +
// UserEventData of the Vehicle input events, so the key glyphs follow the player's bindings like every native prompt.
// Layout: a WASD cluster (labels under the keys; A and D level with S,
// W above S, all at the same key distance), a column of three rows right-aligned on their keys with equal spacing
// (Gear-, Gear+: Shift with Brake / Accelerate; Hover / Land: Space) and a key box reading "GEAR n". All Scaleform work runs inside the HUD movie's own per-frame Advance, never from the plugin task.
namespace hoverui
{
	bool Install();
	// Main thread, every frame: what the buttons should show.
	void SetState(bool inVehicle, bool hovering, int gear, bool accelHeld);
	// Main thread: names of the keys bound to Accelerate / Brake (for the "+W" / "+S" of the gear rows).
	void SetKeyNames(const char* accel, const char* brake);
	// Main thread: whether the scanner shows the rover call button (a rover is known, on foot) and its key name.
	void SetCall(bool available, const char* key);
	// Main thread: follow mode on / off (the scanner's second button: hold the call key).
	void SetFollow(bool on);
	// Main thread: the scanner status box: 0 hidden, 1 SUMMONING, 2 FOLLOWING.
	void SetStatus(int state);
	// Main thread: the follow key's name (the scanner's Follow button).
	void SetFollowKey(const char* key);
	// A save load or reload: forget the button objects (the HUD movie is rebuilt).
	void Reset();
	// Main thread, every frame: layout editor (only in the rover). F7 on / off (saves). Page Up / Page Down pick W, A, S,
	// D, Gear-, Gear+, Hover, the gear box, the WASD cluster, the column or all (picked ones blink); arrows move
	// (Ctrl: x10); Home / End push the WASD keys apart / together at equal distance (or space the column's rows). Saved to SFSE\Logs\SeamlessVehicle\hoverui.ini.
	// In the scanner, F7 and the arrows move the scanner's call button.
	void EditTick(bool focused);
}
