#pragma once

#include <cstdint>
#include <optional>

namespace game
{
	// Image base of the running Starfield.exe.
	std::uintptr_t Base();

	// Checks every signature in addresses.h against the running exe. Logs each mismatch.
	bool VerifySignatures();

	// Reads memory without crashing if the address is invalid (returns nullopt instead).
	template <class T>
	std::optional<T> SafeRead(std::uintptr_t address);

	struct Vec3
	{
		float x, y, z;
	};

	struct PlanetState
	{
		bool          havePlayer = false;
		bool          haveManager = false;
		Vec3          playerPos{};
		std::uint8_t  managerState = 0;
		std::int32_t  gridCentreX = 0, gridCentreY = 0;
		std::int32_t  shiftDeltaX = 0, shiftDeltaY = 0;
	};

	// Snapshot of the player and planet manager, read-only.
	PlanetState ReadPlanetState();

	struct Limits
	{
		bool         borderEnabled;
		std::uint32_t maxPlayableCoord, blockSize, mergeZone, terrainShiftCells;
		std::int32_t  havokCellExtents;
		float        borderMetres;    // where the landing wall stops the player
		float        physicsMetres;   // half-extent of the physics world
	};

	Limits ReadLimits();
}
