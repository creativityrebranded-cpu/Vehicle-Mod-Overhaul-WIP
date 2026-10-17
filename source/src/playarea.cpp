#include "playarea.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "telemetry.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>

namespace playarea
{
	namespace
	{
		using Test_t = bool (*)(void*, std::uint32_t, const std::uint8_t*, int);
		constexpr std::uintptr_t kLandingMoveCheck = 0x0078BCDD;  // return address of the test inside FUN_14078BC30
		Test_t g_original = nullptr;

		// The player's cell offset from the manager origin (x low, y high) and the planet it is for (0 = unknown).
		std::atomic<std::uint64_t> g_offset{ 0 };
		std::atomic<std::uint32_t> g_planet{ 0 };
		std::atomic<std::uint32_t> g_rescued{ 0 };  // tests turned to "inside" by the hook (telemetry)
		std::atomic<std::uint32_t> g_farNonContentRejects{ 0 };  // read-only audit of other landing-area consumers

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }
		std::uint32_t Setting(std::uint32_t rva) { return *reinterpret_cast<std::uint32_t*>(game::Base() + rva); }

		std::uintptr_t Origin(std::uintptr_t mgr) { return mgr + (At<std::uint8_t>(mgr, off::kMgrState) == 3 ? 0x1C0 : 0xC8); }

		// Planet coordinate (block/cell struct) of a lat/long, and its cell offset from another coordinate.
		bool CellOffset(void* latlong, std::uint32_t planet, const std::uint8_t* from, std::int32_t out[2])
		{
			const auto grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)(latlong, planet, 1);
			if (!grid) {
				return false;
			}
			alignas(16) std::uint8_t coord[0x60] = {};
			EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kCoordFromLatLong)(grid, coord, latlong);
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const std::uint8_t*, const std::uint8_t*)>(addr::kCoordOffset)(grid, out, from, coord);
			return true;
		}

		bool Detour(void* latlong, std::uint32_t planet, const std::uint8_t* origin, int buffer)
		{
			const bool inside = g_original(latlong, planet, origin, buffer);
			if (inside) {
				return true;
			}
			const auto ret = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - game::Base();
			if (ret == kLandingMoveCheck) {
				// 0.57.1: the landing move (FUN_14078BC30) checks its own target against the play area of the block-aligned
				// origin. A save written as a landing at the player can put the player more than the play-area half-width
				// (39 cells on Mars) from that origin; the engine then refused its own landing and the load hung forever
				// (2026-10-15 17:25 / 18:03: players 44 and 46 cells out). With the wall off everywhere it is inside.
				static std::atomic<unsigned> n{ 0 };
				if (n.fetch_add(1) < 20) {
					logger::Write("Play area: landing move target outside the aligned origin's play area: let through (load would hang)");
				}
				return true;
			}
			if (ret < addr::kContentCodeBegin || ret >= addr::kContentCodeEnd) {
				// Reloading as a landing near the player cleared a reported no-clip drag/wall about 3.5 km
				// from the old landing. Identify any non-content engine reader of this landing-centred test.
				const std::uint64_t packed = g_offset.load();
				const std::int32_t px = static_cast<std::int32_t>(packed & 0xFFFFFFFF), py = static_cast<std::int32_t>(packed >> 32);
				if (std::max(std::abs(px), std::abs(py)) >= 30) {
					const auto n = g_farNonContentRejects.fetch_add(1, std::memory_order_relaxed) + 1;
					if (n <= 40 || n % 1000 == 0) {
						logger::Write("Play area: non-content reject #%u caller Starfield.exe+0x%llX, player (%d,%d) cells from landing, buffer %d, origin %p", n,
							static_cast<unsigned long long>(ret), px, py, buffer, origin);
					}
				}
				return false;  // not the content manager: engine answer stands
			}
			const auto mgr = Ptr(addr::kPlanetManagerPtr);
			if (!mgr || reinterpret_cast<std::uintptr_t>(origin) != Origin(mgr) || g_planet.load() != planet) {
				return false;
			}
			std::int32_t d[2];
			if (!CellOffset(latlong, planet, origin, d)) {
				return false;
			}
			const std::uint64_t packed = g_offset.load();
			const std::int32_t px = static_cast<std::int32_t>(packed & 0xFFFFFFFF), py = static_cast<std::int32_t>(packed >> 32);
			// Same square as the engine: min(8 blocks, playable area) cells, half of it minus the buffer.
			const std::uint32_t blocks8 = (Setting(addr::kBlockSize) + Setting(addr::kBlockMergeZoneSize)) * 8;
			const std::uint32_t playable = Setting(addr::kMaximumPlayableAreaWorldCoord) * 2 / 100;
			const int limit = int(std::min(blocks8, playable) / 2) - buffer;
			if (std::abs(d[0] - px) <= limit && std::abs(d[1] - py) <= limit) {
				g_rescued.fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			return false;
		}
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kPlayableAreaTest, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Play area: hook failed; content stays around the landing site");
			return false;
		}
		g_original = reinterpret_cast<Test_t>(original);
		logger::Write("Play area: content area follows the player");
		return true;
	}

	void Tick()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 250) {
			return;
		}
		last = now;
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!mgr || !player || At<std::uint8_t>(mgr, off::kMgrState) != 2) {
			g_planet.store(0);
			return;
		}
		const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
		if (!cell || !At<std::uintptr_t>(cell, off::kCellWorldspace)) {
			return;  // indoors: keep the last value
		}
		const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
		const auto pos = At<game::Vec3>(player, off::kRefPosition);
		alignas(16) double latlong[4] = {};
		const float p[3] = { pos.x, pos.y, pos.z };
		EngineFn<void* (*)(std::uintptr_t, double*, const float*)>(addr::kWorldToLatLong)(mgr, latlong, p);
		std::int32_t d[2];
		if (!CellOffset(latlong, planet, reinterpret_cast<const std::uint8_t*>(Origin(mgr)), d)) {
			return;
		}
		g_offset.store((static_cast<std::uint64_t>(static_cast<std::uint32_t>(d[1])) << 32) | static_cast<std::uint32_t>(d[0]));
		g_planet.store(planet);

		static ULONGLONG lastLog = 0;
		if (now - lastLog > 10000) {
			lastLog = now;
			const auto rescued = g_rescued.exchange(0);
			logger::Write("Play area: player %d, %d cells from the landing origin; %u content tests let through in 10 s", d[0], d[1], rescued);
			telemetry::Record("play_area", "\"cells\":[%d,%d],\"rescued\":%u", d[0], d[1], rescued);
		}
	}
}
