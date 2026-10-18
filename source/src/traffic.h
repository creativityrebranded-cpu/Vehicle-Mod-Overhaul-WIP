#pragma once

#include <cstdint>

// Traffic (the Traffic session; notes/HANDOFF_TRAFFIC_PROFILES_20261017.md). Step 1: where NPCs want to go, read
// from each actor's current path (notes/TRAFFIC_DESTINATION_RESEARCH.md). Read-only: nothing is changed in the game.
namespace traffic
{
	// Kinds of destination.
	enum class Kind : std::uint8_t { None, Short, Trip };

	struct Destination
	{
		Kind kind = Kind::None;
		float x = 0, y = 0, z = 0;   // the path's goal (world)
		float remaining = 0;          // metres left along the path (0 when unknown)
		unsigned points = 0;          // path points
		int packageType = -1;         // the current package's type (TESPackage + 0x3C), -1 unknown
	};

	// The goal of the actor's current path; false when it has no path right now.
	bool DestinationOf(std::uintptr_t actor, Destination& out);

	void Tick();   // main thread, every frame (works every 5 s: the probe log)
	void Reset();  // a save load
}
