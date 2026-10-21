#pragma once
#include <string>
#include <cstdint>

// Native hover buttons on the HUD's rover widget (hudmenu.swf: root.RightMeters_mc.HUDVehicle_mc, the widget that
// shows "hold [E] Exit vehicle"). The buttons are the game's own BasicButton (ButtonClips.swf) with ButtonBaseData +
// UserEventData of the Vehicle input events, so the key glyphs follow the player's bindings like every native prompt.
// Layout: a WASD cluster (labels under the keys; A and D level with S,
// W above S, all at the same key distance), a column of three rows right-aligned on their keys with equal spacing
// (Descend: hold Alt; Gear: tap Alt; Hover / Land: Space) and a key box reading "GEAR n". All Scaleform work runs inside the HUD movie's own per-frame Advance, never from the plugin task.
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
	// Main thread, every frame: the contextual HUD. On foot (only when the player owns a vehicle): Summon and Cover
	// with their keys, Get in (the game's Activate key) when near the own vehicle, and a status box (e.g. "FOLLOWING 42 m";
	// empty: hidden).
	// summonLabel / coverLabel: the button names for the context (overrides.h: Summon or Recall, Cover or Stop cover).
	void SetFoot(bool own, bool nearOwn, const char* summonKey, const char* followKey, const char* useKey, const char* status,
		const char* summonLabel, const char* coverLabel);
	// Main thread: facing an occupied vehicle on foot: the Passenger (tap) and Hold: eject prompts show, at the shared 75% scale,
	// with the Activate key's name.
	void SetOccupied(bool facing, const char* key);
	// Main thread: in the vehicle, trip status only; destinations activate autopilot without a separate button.
	void SetTripStatus(bool on, const char* line);
	// Main thread: one line per follower vehicle / rider below Target (at most kMaxRows; count 0 hides them).
	constexpr int kMaxRows = 4;
	void SetRows(int count, const char* const* lines);
	// A save load or reload: forget the button objects (the HUD movie is rebuilt).
	void Reset();
	// Main thread, every frame: layout editor. F7 on / off (saves). In the vehicle, Page Up / Page Down pick one element
	// (W, A, S, D, Descend, Gear, Hover, Gear box, Trip status, Target, each of the rover widget's own clips such as the
	// Exit button) or a group (WASD, Column, Extras, All); the pick is named in a notification and blinks. On foot they pick
	// Summon, Follow, Get in, Status, Target or the whole panel. Arrows move (Ctrl: x10); Delete puts the pick back to its
	// default; Home / End space the WASD keys or the column's rows. While editing every element shows (sample text where
	// empty). Saved to SFSE\Logs\SeamlessVehicle\hoverui.ini.
	void EditTick(bool focused);
	// The scanner (MonocleMenu) is open right now (it counts as open for 300 ms after its last frame). Main thread.
	bool IsScannerOpen();
	// Copied on the scanner movie thread. Empty unless the selected map-marker panel is active.
	struct ScannerDestination { std::string name; double distance = 0; };
	ScannerDestination ScannerDestinationContext();
	struct ScannerPick {std::uint32_t form=0,handle=0;};
	ScannerPick ScannerPickContext();
	// Scanner Target text (distance to what you look at); empty hides it. Uses the former Gang position and native style.
	// Select Target in F7 to move it, on foot or in the vehicle; the scanner follows the same position.
	void SetScan(const char* text);
	// Native status rows and BasicButton action prompts; empty text hides the route group.
	void SetRoute(const char* text,const char* skipKey="",const char* clearKey="",const char* retryKey="");
}
