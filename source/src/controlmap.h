#pragma once

#include <initializer_list>
#include <string>

// The game's own key bindings, read from its ControlMap (DAT_1461DE668): 78 input contexts (names at 0x144A554E0),
// each with keyboard / mouse / gamepad lists of 40-byte entries (+0 event name BSFixedString*, +8 key code, +0xC second
// key or 0xFF). Keyboard codes are Windows virtual-key codes. Used to pick keys for our functions only where the game binds nothing.
namespace controlmap
{
	// True when no gameplay context (any context whose name does not contain "Menu") binds this virtual key on the
	// keyboard. False also when the map cannot be read (then nothing is assumed free).
	bool KeyFree(int vk);
	// True when the named input context (e.g. "Vehicle") binds this virtual key / mouse button to nothing.
	bool KeyFreeIn(int vk, const char* context);
	// The first free key of the list, or 0 when none is free.
	int FirstFree(std::initializer_list<int> vks);
	// Contexts/events that use this virtual key (for the log).
	std::string WhoUses(int vk);
	// The keyboard key (virtual-key code) bound to this event in this context, or the fallback. The map is re-read
	// every 5 s so rebinding in the game's settings is followed.
	int KeyFor(const char* context, const char* event, int fallback);
	// Key name for messages ("H", "F9").
	std::string KeyName(int vk);
	// Writes every context's keyboard bindings to Logs\SeamlessPlanets\controlmap.txt (once per session).
	void Dump();
}
