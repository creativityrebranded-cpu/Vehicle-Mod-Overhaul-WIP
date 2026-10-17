#pragma once

#include <cstdint>

// Interface between the core plugin (SeamlessPlanets.dll: world re-centre, zones, saves, terrain, probes, shared engine
// hooks) and the feature plugins (SeamlessContent, SeamlessShip, SeamlessVehicle). Every engine hook has exactly one
// owner; a feature that needs a shared hook registers a callback with the core instead of hooking it again (two hooks
// on one function cannot both install: the second sees changed bytes). The core sends this table through SFSE messaging
// (sender "SeamlessPlanets", type kMessageApi) at PostLoad; a feature plugin without the core stays disabled.
namespace seamless
{
	constexpr std::uint32_t kApiVersion = 1;
	constexpr std::uint32_t kMessageApi = 0x53504150;  // 'SPAP'
	constexpr const char* kCorePluginName = "SeamlessPlanets";

	using FrameFn = void (*)();                       // main frame task (SFSE task, main thread)
	using SyncFn = void (*)(std::uintptr_t tes);      // sync point: game-update thread, once per frame
	using VoidFn = void (*)();
	using PlayerFn = void (*)(std::uintptr_t player); // the player's per-frame update
	using BoolFn = bool (*)();
	using TimeFn = unsigned long long (*)();
	using ZoneEnterFn = void (*)(std::uint64_t key, float x, float y, float z, float vx, float vy, bool start);
	// A world re-centre by (cx, cy) whole cells: positions change by -(cx, cy) * 100 m. Game-update thread, inside the
	// shift (before: nothing moved yet; after: everything the core moves has moved, derived parts may still be pending).
	using ShiftFn = void (*)(std::int32_t cx, std::int32_t cy, std::uintptr_t tes);

	struct CoreApi
	{
		std::uint32_t version;

		// World state
		bool (*RecentrePending)();
		float (*PhysicsHalfExtent)();
		void (*TotalShift)(std::int32_t* x, std::int32_t* y);

		// Zones
		bool (*ZoneKeyAt)(float x, float y, float z, std::uint64_t* key);
		bool (*ZoneKeyAtLatLong)(double lat, double lon, std::uint64_t* key);
		double (*ZoneSide)();

		// Ground probes (serviced in the core's player grid update; slot numbers from prefetch.h)
		void (*RequestGround)(int slot, float x, float y, float z, float above, float length);
		void (*RequestGroundMask)(int slot, float x, float y, float z, float above, float length, std::uint32_t mask);
		bool (*LatestGround)(int slot, float* ground, unsigned* age);
		void (*SetProbeAnchor)(std::uintptr_t ref);

		// Ship parking (the core hides the far ship around saves)
		void (*HideShipNow)(const char* why);
		void (*ParkClear)();
		bool (*ParkGet)(std::uint32_t* planet, double* lat, double* lon);

		// Mouse counts captured from the look handler (core owns the hook)
		void (*CaptureMouse)(bool on);
		void (*TakeMouse)(double* x, double* y);

		// Callbacks (registered once, called by the core)
		void (*OnFrame)(FrameFn fn);
		void (*OnSync)(SyncFn fn);
		void (*OnBeforeSave)(VoidFn fn);
		void (*OnAfterSave)(VoidFn fn);
		void (*OnLoadReset)(VoidFn fn);
		void (*OnZoneEnter)(ZoneEnterFn fn);
		void (*OnPlayerUpdate)(PlayerFn fn);
		void (*AddBusy)(BoolFn fn);              // autosave and re-centres wait while any returns true
		void (*SetContentPostSteps)(TimeFn fn);  // time of the last landing post-steps (re-centres wait 20 s after)

		// ---- Appended for the module plugins (2026-10-15); older plugins see a shorter table and stay compatible ----
		void (*OnBeforeShift)(ShiftFn fn);       // right before a re-centre moves anything
		void (*OnShift)(ShiftFn fn);             // right after a re-centre completed
		bool (*WorldSettled)();                  // outdoors, planet manager idle, the anchor's and every grid cell attached
		double (*PlanetRadius)();                // metres, 0 until measured
		void (*SetLeadSeconds)(float seconds);   // load-ahead lead (prefetch), default 1.5 s
		unsigned (*CollisionPending)();          // grid cells still waiting for their terrain collision rebuild

	};

	// Feature plugins: the table received from the core (null until PostLoad, or when the core is missing).
	const CoreApi* Api();
}
