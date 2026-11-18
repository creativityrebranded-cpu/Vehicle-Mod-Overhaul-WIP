#pragma once
#include <cstdint>

// Saved hover (2026-11-17): a save made while the player hovers keeps a small record, FlightStates\<save>.txt next to
// FVO's settings, and loading that save turns the hover back on at the same spot instead of letting the vehicle drop.
// Rules: flightsavelogic.h. Saves without a record (older saves, hover off, no Traffic Addon: the SaveGame hook is
// installed only with the addon since 1.2.1) load exactly as before.
namespace flightsave {
// vehicleflight::PlayerTick, main thread: the player's own manual hover now (vehicle 0 = not hovering).
void Publish(std::uint32_t vehicle, float z, int gear);
// SaveGame hook, before the game writes the save: record (or remove) this save's hover.
void OnSave(const char* saveName);
// LoadGame hook, before the game loads: forget the old world's hover and read this save's record as a request.
void OnLoad(const char* saveName);
// vehicleflight::PlayerTick, main thread, while hover is off: true once = turn the hover on now with this gear.
// driven: the vehicle the player drives (form id, 0 none); z its height.
bool TakeRestore(std::uint32_t driven, double z, int& gear);
}
