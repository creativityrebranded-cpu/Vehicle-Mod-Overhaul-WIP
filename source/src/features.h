#pragma once

#include <cstdint>

// The mod's features as switches (2026-10-21, the user: "modular features to turn on and off in the mod menu"): each one
// is a seamless.ini key (the ones that existed keep their section and key), shown as a checkbox in the game's settings
// menu inside Gameplay Options (hoverui) and read live by the code it gates. On() reads a cached value (any
// thread); the main thread reloads the ini every 5 s (Tick), so a hand edit still works.
namespace features
{
	enum class Id : int
	{
		NpcVehicles,    // [context] enabled: NPCs board, drive and fight in vehicles
		CombatAi,       // [combat] ai: the full combat AI (off: the staged manoeuvres)
		Rams,           // [combat] ram
		HoverOrDrive,   // [nav] travel: vehicles choose to fly or drive (travelmode)
		VehicleDamage,  // [hits] enabled: vehicle health, the driver's share, wrecks
		HealthBars,     // [hud] healthBars: the bar under the game's enemy bar and the status card's health lines
		StatusCard,     // [hud] card: the vehicle status card
		Witnesses,      // [witness] enabled: bystanders flee or cower near a crash
		Placement,      // [placement] enabled: parked vehicles for Carfield / Vehicles.esm (after a save load)
		FreeRoll,       // [flight] freeRoll: your hover rolls all the way round, turning about its own axes (freeroll.h)
		FlightAssist,   // [flight] assist: with free roll, a released roll levels when near level (off: the roll holds)
		PhysicalVtol,   // [flight] physicalVtol: experimental player-only native impulse flight
		PhysicalAi,     // [flight] physicalAi: experimental native impulse flight for the autopilot and NPC pilots
		DebugLog,       // [log] verbose
		Count
	};

	struct Info
	{
		Id id;
		const char* label;
		const char* description;
		const wchar_t* section;
		const wchar_t* key;
		bool defaultOn;
	};

	const Info& InfoOf(Id id);
	bool On(Id id);
	void Set(Id id, bool on);         // saves to seamless.ini at once
	void ResetAll();                  // every feature to its default
	unsigned Generation();            // changes whenever any value does
	void Tick();                      // main thread, every frame (reloads the ini every 5 s)
}
