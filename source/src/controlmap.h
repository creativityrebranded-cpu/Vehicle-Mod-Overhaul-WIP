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
    std::string KeyboardMouseLabel(const char* context,const char* event);
	// The gamepad code the game binds to this event in this context (the game's own code space; bindings.cpp maps it to a
	// controller button), 0 when none or unreadable. Also written to controlmap.txt as "gamepad" lines.
	std::uint32_t PadFor(const char* context, const char* event);
	// Contexts/events whose gamepad binding uses this game code (`only`: just that context), for the log.
	std::string WhoUsesPad(std::uint32_t code, const char* only = nullptr);
	// Key name for messages ("H", "F9").
	std::string KeyName(int vk);
	// ---- The one key table of the mod: every key we use is claimed here by (owner, action), so two of our modules never take the same key and the
	// whole table can be logged / shown in one place. Claim* only records and returns; the game's own binding check (KeyFree) still decides what is free.
	// Claim: take this key if the game leaves it free and we have not given it away; false (and a log line) otherwise.
	bool Claim(const char* owner, const char* action, int vk);
	// The first of the candidates that is free in the game and unclaimed by us, claimed; 0 when none.
	int ClaimFirst(const char* owner, const char* action, std::initializer_list<int> vks);
	// Records a key an owner chose by its own rule (no free check), so the table is complete and a later Claim cannot take it.
	void Note(const char* owner, const char* action, int vk);
	// Logs the table (owner, action, key), one line each.
	void LogTable();
	// Test / debug keys (board, exit, escort-pursuit, NPC profile cycle) exist only with Logs\FlyingVehicleOverhaul\settings.ini [keys] debug=1. The context engine does their job.
	bool DebugKeys();
	// Writes every context's keyboard bindings to Logs\SeamlessPlanets\controlmap.txt (once per session).
	void Dump();
}
