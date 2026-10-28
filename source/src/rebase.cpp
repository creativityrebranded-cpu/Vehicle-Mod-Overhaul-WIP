#include "rebase.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "notify.h"
#include "parking.h"
#include "recentre.h"
#include "telemetry.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace rebase
{
	namespace
	{

		using Writer_t = void (*)(std::uintptr_t);
		Writer_t g_original = nullptr;
		std::atomic<bool> g_shipHidden{ false };
		std::atomic<bool> g_saving{ false };
		bool g_flightHide = false;
		constexpr bool kMoveLanding = false;

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		// Packed planet cell id of a planet coordinate (the 0x2C-byte struct at mgr+0xC8, as the aligned origin), encoded
		// exactly as FUN_140424C50 does after decoding/offsetting: face byte +0x00; pattern +0x04/+0x08; block +0x14/+0x18;
		// cell +0x24/+0x28; S = uBlockSize + uBlockMergeZoneSize (Stitching); faces 2 and 3 mirrored with grid +0x50;
		// planet words from grid +0x58/+0x5C.
		std::uint64_t EncodeCoord(std::uintptr_t grid, const std::uint8_t* c)
		{
			const auto i32 = [c](int o) { return *reinterpret_cast<const std::int32_t*>(c + o); };
			const std::int32_t s = *reinterpret_cast<const std::int32_t*>(game::Base() + addr::kBlockSizeStitching) +
			                       *reinterpret_cast<const std::int32_t*>(game::Base() + addr::kBlockMergeZoneStitching);
			std::int32_t col = s * i32(0x14) + s * 0x10 * i32(0x04) + i32(0x24);
			std::int32_t row = s * 0x10 * i32(0x08) + s * i32(0x18) + i32(0x28);
			const std::uint8_t face = c[0];
			if (std::uint8_t(face - 2) < 2) {
				const std::int32_t m = s * At<std::int32_t>(grid, 0x50) * 0x10;
				col = m - col - 1;
				row = m - row - 1;
			}
			const std::uint64_t hi = ((std::uint64_t(At<std::uint32_t>(grid, 0x58)) << 6 | At<std::uint32_t>(grid, 0x5C)) << 3) | face;
			return ((hi << 16 | std::uint64_t(std::int64_t(row))) << 16) | std::uint64_t(std::int64_t(col));
		}

		struct Plan
		{
			bool ok = false;
			std::uint8_t oldLatLong[16]{};
			std::uint8_t newLatLong[16]{};
			float oldPos[3]{};
			float newPos[3]{};
			int dx = 0, dy = 0;  // new origin's offset from the old, in cells
			std::int32_t oldGrid[2]{};  // the loaded-grid centre cell stored in the record (TES +0xC4/+0xC8)
			std::int32_t newGrid[2]{};  // the player's cell in the new landing's frame
			std::uint64_t oldWsOrigin = ~0ull;  // the worldspace planet origin (ws+0x310) the writer serializes (live frame)
			std::uint64_t newWsOrigin = ~0ull;  // ... for the new landing's frame (grid (0, 0) there)
		};

		// Works out the re-based landing for the player's current position, or ok = false (then the save is vanilla).
		Plan Compute()
		{
			Plan p;
			const auto mgr = Ptr(addr::kPlanetManagerPtr);
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			if (!mgr || !player || At<std::uint8_t>(mgr, off::kMgrState) != 2) {
				return p;
			}
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			if (!cell || !At<std::uintptr_t>(cell, off::kCellWorldspace)) {
				return p;  // interior: nothing to do
			}
			const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			const auto pos = At<game::Vec3>(player, off::kRefPosition);
			alignas(16) std::uint8_t latlong[32] = {};
			const float fp[3] = { pos.x, pos.y, pos.z };
			EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, latlong, fp);
			const auto grid = EngineFn<std::uintptr_t (*)(void*, std::uint32_t, int)>(addr::kPlanetGridFor)(latlong, planet, 1);
			if (!grid) {
				return p;
			}
			alignas(16) std::uint8_t coord[0x60] = {}, origin[0x60] = {};
			EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kCoordFromLatLong)(grid, coord, latlong);
			EngineFn<void* (*)(std::uintptr_t, void*, void*)>(addr::kAlignedOrigin)(grid, origin, latlong);
			const auto oldOrigin = reinterpret_cast<const std::uint8_t*>(mgr + 0xC8);
			std::int32_t inOld[2], inNew[2], shift[2];
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, inOld, oldOrigin, coord);
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, inNew, origin, coord);
			EngineFn<void* (*)(std::uintptr_t, std::int32_t*, const void*, const void*)>(addr::kCoordOffset)(grid, shift, oldOrigin, origin);

			// Safety: the player's cell measured from the current origin must match their world position.
			const int cx = int(std::floor(pos.x / 100.0f)), cy = int(std::floor(pos.y / 100.0f));
			if (std::abs(inOld[0] - cx) > 1 || std::abs(inOld[1] - cy) > 1) {
				logger::Write("Rebase: frame check failed (cell %d,%d from origin vs %d,%d from position); save left as is", inOld[0], inOld[1], cx, cy);
				telemetry::Record("rebase_check_failed", "\"engine\":[%d,%d],\"pos\":[%d,%d]", inOld[0], inOld[1], cx, cy);
				return p;
			}
			// (No early return when the shift is zero: after a world re-centre the frame is the player's, but the
			// stored landing lat/long is still the original one, so it is always rewritten.)
			std::memcpy(p.oldLatLong, reinterpret_cast<const void*>(mgr + 0x98), 16);
			std::memcpy(p.newLatLong, latlong, 16);
			p.oldPos[0] = pos.x;
			p.oldPos[1] = pos.y;
			p.oldPos[2] = pos.z;
			p.newPos[0] = inNew[0] * 100.0f + (pos.x - cx * 100.0f);
			p.newPos[1] = inNew[1] * 100.0f + (pos.y - cy * 100.0f);
			p.newPos[2] = pos.z;
			p.dx = shift[0];
			p.dy = shift[1];
			// The record also stores the loaded-grid centre cell; the load builds the cells around it first (the
			// reader passes it to FUN_1418290d0 and sets TES +0xC4/+0xC8 from it). It must follow the position.
			const auto tes = Ptr(addr::kTESPtr);
			if (tes) {
				p.oldGrid[0] = At<std::int32_t>(tes, 0xC4);
				p.oldGrid[1] = At<std::int32_t>(tes, 0xC8);
			}
			p.newGrid[0] = std::int32_t(std::floor(p.newPos[0] / 100.0f));
			p.newGrid[1] = std::int32_t(std::floor(p.newPos[1] / 100.0f));
			// The writer also stores ws+0x310 after the position (worker R17); since re-centres move it, it must follow
			// the new landing too: the planet cell id of the new frame's grid (0, 0), i.e. the live (dx, dy).
			const auto ws = At<std::uintptr_t>(cell, off::kCellWorldspace);
			p.oldWsOrigin = At<std::uint64_t>(ws, off::kWsPlanetOrigin);
			if (p.oldWsOrigin != ~0ull) {
				// The save keeps the planet's ORIGINAL origin (ws+0x310 before any re-centre), as saves did when the anchoring
				// was proven (0.20: landing and position moved, origin untouched, loads worked). Storing the new landing's
				// aligned origin (0.37.1, 0.37.2; both independent computations agreed on it) left the load stuck on the
				// loading screen with the planet manager never starting. (Whether the live shifted origin, stored by 0.34-0.37.0,
				// loads was never tested.)
				std::int32_t tx = 0, ty = 0;
				recentre::TotalShift(tx, ty);
				p.newWsOrigin = (tx == 0 && ty == 0) ? p.oldWsOrigin : recentre::PlanetCellIdAt(ws, -tx, -ty);
				logger::Write("Rebase: ws+0x310 %016llX (total shift %d, %d); the save stores the original origin %016llX", static_cast<unsigned long long>(p.oldWsOrigin),
					tx, ty, static_cast<unsigned long long>(p.newWsOrigin));
			}
			p.ok = true;
			return p;
		}

		// Replaces the first occurrence of `from` in [begin, end) with `to`. Returns the offset or -1.
		long Replace(std::uint8_t* data, std::uint32_t begin, std::uint32_t end, const void* from, const void* to, std::size_t n)
		{
			for (std::uint32_t i = begin; i + n <= end; ++i) {
				if (std::memcmp(data + i, from, n) == 0) {
					std::memcpy(data + i, to, n);
					return long(i);
				}
			}
			return -1;
		}

		void Detour(std::uintptr_t buffer)
		{
			const bool wasSaving = g_saving.exchange(true);
			recentre::OnSaveWrite();
			const std::uint32_t begin = At<std::uint32_t>(buffer, 0x14);
			g_original(buffer);
			g_saving.store(wasSaving);
			// The landing is no longer moved: every save that moved it (0.37.1-0.38.4: by -96/0, 33/-44, -25/1 cells) hung
			// on load with the planet manager never starting, while saves with the landing unchanged loaded (0.38.5). With
			// SaveFrame the save is written in the original frame, as the game expects for the original landing.
			if (!kMoveLanding) {
				return;
			}
			const Plan plan = Compute();
			if (!plan.ok) {
				return;
			}
			const std::uint32_t end = At<std::uint32_t>(buffer, 0x14);
			auto* data = At<std::uint8_t*>(buffer, 8);
			// The record is: position (12) ... flag, two ids, landing lat/long (16) ... Position first, then lat/long.
			const long at = Replace(data, begin, end, plan.oldPos, plan.newPos, sizeof(plan.oldPos));
			const long ll = at >= 0 ? Replace(data, std::uint32_t(at) + 12, end, plan.oldLatLong, plan.newLatLong, 16) : -1;
			if (at < 0 || ll < 0) {
				if (at >= 0) {
					std::memcpy(data + at, plan.oldPos, sizeof(plan.oldPos));  // undo: keep the record consistent
				}
				logger::Write("Rebase: record layout not found (pos %ld, landing %ld); save left as is", at, ll);
				telemetry::Record("rebase_layout_missing", "\"pos\":%ld,\"landing\":%ld", at, ll);
				return;
			}
			// The grid centre comes before the position (worldspace id, grid x, grid y, cell id, position).
			const long gr = Replace(data, begin, std::uint32_t(at), plan.oldGrid, plan.newGrid, sizeof(plan.oldGrid));
			// The worldspace planet origin follows the position closely; only searched in the next 128 bytes.
			if (plan.oldWsOrigin != ~0ull && plan.newWsOrigin != ~0ull && plan.oldWsOrigin != plan.newWsOrigin) {
				const std::uint32_t from = std::uint32_t(at) + 12, to = std::min<std::uint32_t>(end, from + 128);
				const long og = Replace(data, from, to, &plan.oldWsOrigin, &plan.newWsOrigin, 8);
				logger::Write("Rebase: worldspace planet origin %016llX -> %016llX %s", static_cast<unsigned long long>(plan.oldWsOrigin),
					static_cast<unsigned long long>(plan.newWsOrigin), og >= 0 ? "rewritten" : "NOT FOUND in the record");
			}
			if (gr < 0) {
				logger::Write("Rebase: grid centre (%d, %d) not found in the record; the load will start from the old cells",
					plan.oldGrid[0], plan.oldGrid[1]);
				telemetry::Record("rebase_grid_missing", "\"grid\":[%d,%d]", plan.oldGrid[0], plan.oldGrid[1]);
			}
			logger::Write("Rebase: save anchored to the player's zone: landing moved %d, %d cells; position (%.0f, %.0f) -> (%.0f, %.0f); "
				"grid (%d, %d) -> (%d, %d)%s",
				plan.dx, plan.dy, plan.oldPos[0], plan.oldPos[1], plan.newPos[0], plan.newPos[1], plan.oldGrid[0], plan.oldGrid[1],
				plan.newGrid[0], plan.newGrid[1], gr < 0 ? " (grid not rewritten)" : "");
			telemetry::Record("rebase_save", "\"shift\":[%d,%d],\"old\":[%.1f,%.1f,%.1f],\"new\":[%.1f,%.1f,%.1f]", plan.dx, plan.dy,
				plan.oldPos[0], plan.oldPos[1], plan.oldPos[2], plan.newPos[0], plan.newPos[1], plan.newPos[2]);
		}
	}

	bool CanAnchor() { return Compute().ok; }

	bool Install()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kSaveLocationWriter, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Rebase: hook failed; saves keep the original landing");
			return false;
		}
		g_original = reinterpret_cast<Writer_t>(original);
		return true;
	}

	// The home ship and whether it is in the player's world (its own cell may be its exterior ship cell, which has no
	// worldspace of its own: then it counts as here).
	std::uintptr_t HomeShip(std::uintptr_t player)
	{
		std::uintptr_t ship = 0;
		EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
		return ship;
	}

	bool InPlayersWorld(std::uintptr_t ship, std::uintptr_t player)
	{
		const auto sc = At<std::uintptr_t>(ship, off::kRefParentCell);
		const auto pc = At<std::uintptr_t>(player, off::kRefParentCell);
		if (!sc || !pc || !At<std::uintptr_t>(pc, off::kCellWorldspace)) {
			return false;
		}
		const auto sws = At<std::uintptr_t>(sc, off::kCellWorldspace);
		return !sws || sws == At<std::uintptr_t>(pc, off::kCellWorldspace);
	}

	void Hide(std::uintptr_t ship, const char* why)
	{
		const auto sp = At<game::Vec3>(ship, off::kRefPosition);
		parking::Park(Ptr(addr::kPlanetManagerPtr), sp.x, sp.y, sp.z);  // the planet map marks where it was left
		EngineFn<void (*)(std::uintptr_t, char)>(addr::kDisableReference)(ship, 0);
		g_shipHidden = true;
		logger::Write("Ship: hidden (%s); parked at (%.0f, %.0f, %.0f)", why, sp.x, sp.y, sp.z);
		telemetry::Record("ship_hidden", "\"why\":\"%s\",\"pos\":[%.0f,%.0f]", why, sp.x, sp.y);
	}

	// 0.57.2: the ship's real place. While the home ship has its 3D loaded in the player's world (so its position is in this
	// frame), its planet lat/long is recorded (parking: per save, plus the planet's last known spot), every 5 s and only
	// when it moved more than 20 m. The ship beacon (SeamlessShip) guides to this record when the ship is out of range.
	void TrackShip()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 5000) {
			return;
		}
		last = now;
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto mgr = Ptr(addr::kPlanetManagerPtr);
		if (!player || !mgr || At<std::uint8_t>(mgr, off::kMgrState) != 2 || g_saving.load()) {
			return;
		}
		const auto ship = HomeShip(player);
		if (!ship) {
			return;
		}
		const bool disabled = (At<std::uint32_t>(ship, 0x20) & 0x800) != 0;
		const auto loaded = At<std::uintptr_t>(ship, 0xB8);
		const bool has3D = loaded && At<std::uintptr_t>(loaded, 0x08);
		const auto pcell = At<std::uintptr_t>(player, off::kRefParentCell);
		const bool playerOutside = pcell && At<std::uintptr_t>(pcell, off::kCellWorldspace);
		if (!disabled && has3D && playerOutside && InPlayersWorld(ship, player) && !g_flightHide) {
			const auto sp = At<game::Vec3>(ship, off::kRefPosition);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (std::isfinite(sp.x) && std::hypot(sp.x - pp.x, sp.y - pp.y) < 3000.0f) {
				parking::Track(mgr, sp.x, sp.y, sp.z);
			}
		}
		EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
	}

	// Undo our older distance-based hide when loading a save that has its parked-ship record.
	// Do not enable a ship disabled by another system, or the deliberate hide used during flight.
	void RestoreDistanceHiddenShip()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) {
			return;
		}
		const auto ship = HomeShip(player);
		if (!ship) {
			return;
		}
		const bool disabled = (At<std::uint32_t>(ship, 0x20) & 0x800) != 0;
		g_shipHidden = disabled;
		std::uint32_t parkedPlanet = 0;
		double lat = 0, lon = 0;
		if (!disabled) {
			g_flightHide = false;
			// 0.57.2: the record is kept. It is the ship's real place on the planet (TrackShip), which a save written as a
			// landing elsewhere no longer shows (the game's landing marker is then at the save's landing, near the player).
		}
		else if (!g_flightHide && parking::Get(parkedPlanet, lat, lon) && InPlayersWorld(ship, player)) {
			const auto mgr = Ptr(addr::kPlanetManagerPtr);
			if (mgr && At<std::uint8_t>(mgr, off::kMgrState) == 2 &&
				At<std::uint32_t>(mgr, off::kMgrPlanetId) == parkedPlanet) {
				EngineFn<void (*)(std::uintptr_t)>(addr::kEnableReference)(ship);
				g_shipHidden = (At<std::uint32_t>(ship, 0x20) & 0x800) != 0;
				if (!g_shipHidden) {
					parking::Clear();
					logger::Write("Ship: restored from the retired distance-hide behavior");
					notify::Show("Your ship is visible again");
				}
			}
		}
		EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
	}

	bool Saving() { return g_saving.load(); }

	void PreSave()
	{
		g_saving.store(true);
	}

	void PostSave()
	{
		g_saving.store(false);
	}

	void ResetAfterLoad()
	{
		g_flightHide = false;
		g_shipHidden = false;
	}

	void HideShipNow(const char* why)
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const auto ship = player ? HomeShip(player) : 0;
		if (!ship) {
			return;
		}
		g_flightHide = true;
		if (!(At<std::uint32_t>(ship, 0x20) & 0x800) && InPlayersWorld(ship, player)) {
			Hide(ship, why);
		}
		EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
	}


	void Tick()
	{
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 1000) {
			return;
		}
		last = now;
		RestoreDistanceHiddenShip();
		TrackShip();
	}

	bool ShipHidden() { return g_shipHidden.load(); }
}
