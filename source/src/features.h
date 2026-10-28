#pragma once

#include <cstdint>

// The mod's features as switches (2026-10-21, the user: "modular features to turn on and off in the mod menu"): each one
// is a settings.ini key (the ones that existed keep their section and key), shown as a checkbox in the game's settings
// menu inside Gameplay Options (hoverui) and read live by the code it gates. On() reads a cached value (any
// thread); the main thread reloads the ini every 5 s (Tick), so a hand edit still works.
namespace features
{
	enum class Id : int
	{
		NpcVehicles,    // [context] enabled: NPCs board, drive and fight in vehicles
		CombatAi,       // [combat] ai: the full combat AI (off: the staged manoeuvres)
		HoverOrDrive,   // [nav] travel: vehicles choose to fly or drive (travelmode)
		VehicleDamage,  // [hits] enabled: vehicle health, the driver's share, wrecks
		HealthBars,     // [hud] healthBars: the bar under the game's enemy bar and the status card's health lines
		StatusCard,     // [hud] card: the vehicle status card
		Witnesses,      // [witness] enabled: bystanders flee or cower near a crash
		Placement,      // retired randomizer; numeric ID/key retained for old installer profiles
		FreeRoll,       // [flight] freeRoll: your hover rolls all the way round, turning about its own axes (freeroll.h)
		FlightAssist,   // [flight] assist: with free roll, a released roll levels when near level (off: the roll holds)
		PhysicalVtol,   // [flight] physicalVtol: experimental player-only native impulse flight
		PhysicalAi,     // [flight] physicalAi: experimental native impulse flight for the autopilot and NPC pilots
		PlacementMode,  // [placement] recording: the scanner's T saves / deletes parking spots and sets summon points (placementedit.h)
		NativeGround,   // [nav] groundPlanner: the ground-first route planner (groundnav) and the game's own driving for occupied vehicles
		DebugLog,      // [log] verbose
		Traffic,        // [context] traffic: everyday traffic from parked vehicles (trafficpick.h); after DebugLog so earlier ids keep their numbers
		RouteMarkers,   // [traffic] markers: probes along the traffic files' routes, exits and places (routeprobes.h)
		Count
	};
	inline constexpr bool Available(Id id) { return id != Id::Placement; }

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
	void Set(Id id, bool on);         // saves to settings.ini at once
	void ResetAll();                  // every feature to its default
	unsigned Generation();            // changes whenever any value does
	void Tick();                      // main thread, every frame (reloads the ini every 5 s)
}
