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
	// Main thread, every frame: the contextual HUD. On foot (only when the player owns a vehicle): Summon and Follow
	// with their keys, Get in (the game's Activate key) when near the own vehicle, and a status box (e.g. "FOLLOWING 42 m";
	// empty: hidden).
	void SetFoot(bool own, bool nearOwn, const char* summonKey, const char* followKey, const char* useKey, const char* status);
	// Main thread: facing an occupied vehicle on foot: the Passenger (tap) and Hold: eject prompts show, at half size,
	// with the Activate key's name.
	void SetOccupied(bool facing, const char* key);
	// Main thread: in the vehicle, the Auto button (its key; empty: hidden), whether Auto flies, and its status line.
	void SetAuto(const char* key, bool on, const char* line);
	// Main thread: the Auto behaviour button (in the vehicle and on foot): its key (empty: hidden), its label ("Auto: Orbit") and a press
	// (the button flashes blue for a moment).
	void SetAutoMode(const char* key, const char* label, bool pressed);
	// Main thread: the gang line (NPC vehicles following: count, profile, the profile key); count 0 hides it.
	void SetGang(int count, const char* profile, const char* key);
	// Main thread: one line per NPC vehicle / rider under the gang line (at most kMaxRows; count 0 hides them). Placed relative to the gang element.
	constexpr int kMaxRows = 4;
	void SetRows(int count, const char* const* lines);
	// A save load or reload: forget the button objects (the HUD movie is rebuilt).
	void Reset();
	// Main thread, every frame: layout editor. F7 on / off (saves). In the vehicle, Page Up / Page Down pick one element
	// (W, A, S, D, Gear-, Gear+, Hover, Gear box, Auto, Auto line, Gang, each of the rover widget's own clips such as the
	// Exit button) or a group (WASD, Column, Extras, All); the pick is named in a notification and blinks. On foot they pick
	// Summon, Follow, Get in, Status, Gang or the whole panel. Arrows move (Ctrl: x10); Delete puts the pick back to its
	// default; Home / End space the WASD keys or the column's rows. While editing every element shows (sample text where
	// empty). Saved to SFSE\Logs\SeamlessVehicle\hoverui.ini.
	void EditTick(bool focused);
	// The scanner (MonocleMenu) is open right now (it counts as open for 300 ms after its last frame). Main thread.
	bool IsScannerOpen();
	// The text of the scanner screen's status box (the distance to what you look at); empty hides it. The box is movable with the F7 editor while the scanner is open.
	void SetScan(const char* text);
}
