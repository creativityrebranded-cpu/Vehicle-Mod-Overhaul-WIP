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

	// Semantic clamp: Havok's broadphase world half-extent (havokCellExtents * 50 m = 6400 m) is NOT the loaded
	// road-collider radius around the player (~160 m). Clamping here makes broadphase-as-collider bugs structurally impossible.
	inline constexpr float kLoadedColliderMaxM = 160.0f;
	inline constexpr float kLoadedColliderMinM = 80.0f;
	inline float ClampLoadedColliderMetres(float broadphaseMetres)
	{
		if (!(broadphaseMetres > 0.0f)) return kLoadedColliderMaxM;
		if (broadphaseMetres < kLoadedColliderMinM) return kLoadedColliderMinM;
		if (broadphaseMetres > kLoadedColliderMaxM) return kLoadedColliderMaxM;
		return broadphaseMetres;
	}

	struct Limits
	{
		bool         borderEnabled;
		std::uint32_t maxPlayableCoord, blockSize, mergeZone, terrainShiftCells;
		std::int32_t  havokCellExtents;
		float        borderMetres;          // where the landing wall stops the player
		float        physicsMetres;         // Havok broadphase world half-extent (~6400 m; NOT loaded collider radius)
		float        loadedColliderMetres;  // clamped loaded road-collider radius around the player (80..160 m)
	};

	Limits ReadLimits();
}
