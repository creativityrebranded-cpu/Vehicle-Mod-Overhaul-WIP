#include "reanchor.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "core_api.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace reanchor
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		// Save/load manager flags at +0x50 (FUN_14184FF90, the quick save/load input handler FUN_141ED3BF0):
		// 1 autosave, 8 quicksave, 0x10 quickload (the newest "Quicksave"). +0x60 = tick of the last save.
		constexpr std::uint32_t kFlagQuicksave = 8, kFlagQuickload = 0x10;
		constexpr std::uint32_t kLastSaveTick = 0x60;
		constexpr std::uint32_t kCanSave = 0x01846EF0;  // (saveLoadManager, 0) -> char: saving allowed now
		constexpr ULONGLONG kTimeoutMs = 120000;
		// The first drag appeared after about 3.5 km of travel. Measure from the
		// player's position at the last load: live shifts reset local x/y, and the
		// saved landing record can be offset to obtain a block-aligned origin.
		constexpr double kAutoRefreshDistance = 2600.0;
		// The 01:56:02 scheduled quickload left the planet manager loading with no terrain source.
		// Keep the measured manual action, but do not start a disk round trip during traversal.
		constexpr bool kAutomaticRefreshEnabled = false;
		constexpr ULONGLONG kStalledGridMs = 20000;
		constexpr ULONGLONG kStillMs = 2000, kRetryMs = 60000, kNoticeMs = 30000;

		enum class Phase { Idle, Saving, Loading, Settling };
		struct
		{
			Phase phase = Phase::Idle;
			int key = 0;
			bool keyDown = false;
			ULONGLONG t0 = 0, tSaved = 0, tLoadSeen = 0;
			ULONGLONG lastSave = 0;
			ULONGLONG lastAttempt = 0, stillSince = 0, lastNotice = 0;
			ULONGLONG gridUnreadySince = 0;
			bool baselineSet = false;
			std::uint32_t baselinePlanet = 0;
			double baseline[2] = {};
			ULONGLONG loadResetAt = 0;
			game::Vec3 stillPos{};
			bool automatic = false;
			double ll[2] = {};
			game::Vec3 pos{};
			float heading = 0;
			std::uintptr_t mgrBefore = 0;
		} g;

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		bool LatLong(std::uintptr_t player, double out[2])
		{
			const auto mgr = Ptr(addr::kPlanetManagerPtr);
			if (!mgr || !player) {
				return false;
			}
			alignas(16) double ll[4] = {};
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			const float fp[3] = { p.x, p.y, p.z };
			EngineFn<void* (*)(std::uintptr_t, void*, const float*)>(addr::kWorldToLatLong)(mgr, ll, fp);
			out[0] = ll[0];
			out[1] = ll[1];
			return std::isfinite(out[0]) && std::isfinite(out[1]);
		}

		const seamless::CoreApi* Api() { return seamless::Api(); }
		bool Settled() { return Api() && Api()->WorldSettled(); }
		double Distance(const double a[2], const double b[2]);
		bool TravelDistance(std::uintptr_t player, double& distance)
		{
			// PreLoadGame is sent before the old world disappears. Do not sample it
			// as the next load's travel baseline during that handoff.
			if (!g.baselineSet && g.loadResetAt && GetTickCount64() - g.loadResetAt < 3000) return false;
			const auto mgr = Ptr(addr::kPlanetManagerPtr);
			const auto cell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
			if (!mgr || At<std::uint8_t>(mgr, off::kMgrState) != 2 || !cell ||
				At<std::uint8_t>(cell, off::kCellState) != 10) return false;
			double here[2];
			if (!LatLong(player, here)) return false;
			const auto planet = At<std::uint32_t>(mgr, off::kMgrPlanetId);
			if (!g.baselineSet || g.baselinePlanet != planet) {
				g.baseline[0] = here[0];
				g.baseline[1] = here[1];
				g.baselinePlanet = planet;
				g.baselineSet = true;
				logger::Write("Re-anchor: travel baseline %.7f, %.7f on planet %08X", here[0], here[1], planet);
			}
			distance = Distance(g.baseline, here);
			return std::isfinite(distance) && distance >= 0.0;
		}
		void Shift(std::int32_t& x, std::int32_t& y)
		{
			x = y = 0;
			if (Api()) Api()->TotalShift(&x, &y);
		}
		// The player outdoors (an exterior cell), not in a vehicle.
		bool OnFootOutdoors(std::uintptr_t player)
		{
			const auto cell = player ? At<std::uintptr_t>(player, off::kRefParentCell) : 0;
			if (!cell || (At<std::uint8_t>(cell, off::kCellFlags) & 1)) return false;
			return EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kPlayerVehicle)(player) == 0;
		}

		// Great-circle distance (radians in, metres out); the pair order does not matter for a small distance.
		double Distance(const double a[2], const double b[2])
		{
			const double r = Api() ? Api()->PlanetRadius() : 0.0;
			if (r <= 0) {
				return -1.0;
			}
			const double s1 = std::sin((b[0] - a[0]) / 2), s2 = std::sin((b[1] - a[1]) / 2);
			const double h = s1 * s1 + std::cos(a[0]) * std::cos(b[0]) * s2 * s2;
			return 2.0 * r * std::asin(std::min(1.0, std::sqrt(h)));
		}

		void Fail(const char* why)
		{
			logger::Write("Re-anchor: stopped (%s)", why);
			notify::Log("Re-anchor stopped: %s", why);
			g.phase = Phase::Idle;
			g.tLoadSeen = 0;
			g.stillSince = 0;
		}
	}

	void Tick()
	{
		if (!Api()) return;
		const auto saveMgr = Ptr(addr::kSaveLoadManagerPtr);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		const ULONGLONG now = GetTickCount64();
		if (!g.key) {
			g.key = controlmap::FirstFree({ VK_INSERT, VK_DELETE, VK_PAUSE, VK_SCROLL, VK_OEM_5 });
			logger::Write("Re-anchor: test key %s (quicksave as a landing at the player, then quickload)",
				g.key ? controlmap::KeyName(g.key).c_str() : "NONE FREE");
			if (!g.key) {
				g.key = -1;
			}
		}
		const bool down = g.key > 0 && Focused() && (GetAsyncKeyState(g.key) & 0x8000);
		const bool pressed = down && !g.keyDown;
		g.keyDown = down;
		if (!saveMgr) {
			return;
		}

		switch (g.phase) {
		case Phase::Idle: {
			if (!player) {
				return;
			}
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			double reach = -1.0;
			const bool haveReach = TravelDistance(player, reach);
			const bool settled = Settled();
			const auto mgr = Ptr(addr::kPlanetManagerPtr);
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const bool attached = mgr && At<std::uint8_t>(mgr, off::kMgrState) == 2 &&
				cell && At<std::uint8_t>(cell, off::kCellState) == 10;
			if (settled || !attached || !OnFootOutdoors(player)) g.gridUnreadySince = 0;
			else if (!g.gridUnreadySince) g.gridUnreadySince = now;
			const bool stalledGrid = g.gridUnreadySince && now - g.gridUnreadySince >= kStalledGridMs;
			const bool refreshReady = settled || stalledGrid;
			bool automatic = false;
			if (kAutomaticRefreshEnabled && haveReach && reach >= kAutoRefreshDistance && Focused() && !pressed && now - g.lastAttempt >= kRetryMs) {
				if (OnFootOutdoors(player) && refreshReady) {
					if (!g.stillSince || std::hypot(p.x - g.stillPos.x, p.y - g.stillPos.y) > 1.0f || std::fabs(p.z - g.stillPos.z) > 1.0f) {
						g.stillSince = now;
						g.stillPos = p;
					}
					else if (now - g.stillSince >= kStillMs) {
						automatic = true;
					}
				}
				else {
					g.stillSince = 0;
					if (Focused() && now - g.lastNotice >= kNoticeMs) {
						g.lastNotice = now;
						notify::Show("Landing refresh ahead: pause outdoors on foot once the world settles");
						logger::Write("Re-anchor: waiting for a calm on-foot point %.0f m travelled since load (grid %s, unready %.1f s)",
							reach, settled ? "ready" : "unready", g.gridUnreadySince ? (now - g.gridUnreadySince) / 1000.0 : 0.0);
					}
				}
			}
			else if (!haveReach || reach < kAutoRefreshDistance) {
				g.stillSince = 0;
			}
			if (!pressed && !automatic) return;
			if (!OnFootOutdoors(player) || !refreshReady) {
				notify::Show("Re-anchor: only outdoors on foot with the world settled");
				logger::Write("Re-anchor: refused (not outdoors on foot, or the world is not settled)");
				return;
			}
			if (!EngineFn<char (*)(std::uintptr_t, int)>(kCanSave)(saveMgr, 0)) {
				notify::Show("Re-anchor: the game does not allow saving now");
				logger::Write("Re-anchor: refused (saving not allowed now)");
				if (automatic) { g.lastAttempt = now; g.stillSince = 0; }
				return;
			}
			if (!LatLong(player, g.ll)) {
				logger::Write("Re-anchor: refused (no lat/long for the player)");
				if (automatic) { g.lastAttempt = now; g.stillSince = 0; }
				return;
			}
			g.pos = At<game::Vec3>(player, off::kRefPosition);
			g.heading = At<game::Vec3>(player, off::kRefRotation).z;
			g.lastSave = At<ULONGLONG>(saveMgr, kLastSaveTick);
			g.t0 = now;
			g.lastAttempt = now;
			g.tLoadSeen = 0;
			g.stillSince = 0;
			g.automatic = automatic;
			std::int32_t tx = 0, ty = 0;
			Shift(tx, ty);
			logger::Write("Re-anchor: %s start at (%.1f, %.1f, %.1f), %.0f m travelled since load, grid %s, lat/long %.7f, %.7f, heading %.3f, total shift (%d, %d); quicksave requested",
				automatic ? "scheduled" : "manual", g.pos.x, g.pos.y, g.pos.z, reach, stalledGrid ? "stalled" : "ready", g.ll[0], g.ll[1], g.heading, tx, ty);
			notify::Show("Landing refresh: saving...");
			EngineFn<void (*)(std::uintptr_t, std::uint32_t, char)>(addr::kRequestSave)(saveMgr, kFlagQuicksave, 0);
			g.phase = Phase::Saving;
			return;
		}
		case Phase::Saving:
			if (At<ULONGLONG>(saveMgr, kLastSaveTick) != g.lastSave && !(At<std::uint32_t>(saveMgr, 0x50) & kFlagQuicksave)) {
				g.tSaved = now;
				logger::Write("Re-anchor: quicksave written in %llu ms; quickload requested", now - g.t0);
				EngineFn<void (*)(std::uintptr_t, std::uint32_t, char)>(addr::kRequestSave)(saveMgr, kFlagQuickload, 0);
				g.mgrBefore = Ptr(addr::kPlanetManagerPtr);
				g.phase = Phase::Loading;
			}
			else if (now - g.t0 > kTimeoutMs) {
				Fail("the quicksave never finished");
			}
			return;
		case Phase::Loading:
			// The load is underway once the player is gone or the planet manager leaves idle; then wait for a settled world.
			if (!g.tLoadSeen) {
				const auto mgr = Ptr(addr::kPlanetManagerPtr);
				if (!player || !mgr || At<std::uint8_t>(mgr, off::kMgrState) != 2 || !Settled()) {
					g.tLoadSeen = now;
					logger::Write("Re-anchor: load started %llu ms after the quickload request", now - g.tSaved);
				}
				else if (now - g.tSaved > 15000) {
					Fail("the quickload never started (no quicksave found?)");
				}
				return;
			}
			if (player && OnFootOutdoors(player) && Settled()) {
				g.phase = Phase::Settling;
			}
			else if (now - g.t0 > kTimeoutMs) {
				Fail("the world never settled after the load");
			}
			return;
		case Phase::Settling: {
			double ll[2];
			if (!LatLong(player, ll)) {
				Fail("no lat/long after the load");
				return;
			}
			const auto pos = At<game::Vec3>(player, off::kRefPosition);
			const float heading = At<game::Vec3>(player, off::kRefRotation).z;
			const double d = Distance(g.ll, ll);
			const auto newMgr = Ptr(addr::kPlanetManagerPtr);
			if (newMgr) {
				g.baseline[0] = ll[0];
				g.baseline[1] = ll[1];
				g.baselinePlanet = At<std::uint32_t>(newMgr, off::kMgrPlanetId);
				g.baselineSet = true;
				g.loadResetAt = 0;
			}
			std::int32_t tx = 0, ty = 0;
			Shift(tx, ty);
			logger::Write("Re-anchor: %s done: save %llu ms, load %llu ms, total %llu ms; now at (%.1f, %.1f, %.1f), lat/long %.7f, %.7f: %.2f m from the start, heading change %.3f; total shift (%d, %d)",
				g.automatic ? "scheduled" : "manual",
				g.tSaved - g.t0, now - g.tSaved, now - g.t0, pos.x, pos.y, pos.z, ll[0], ll[1], d, heading - g.heading, tx, ty);
			telemetry::Record("reanchor", "\"save_ms\":%llu,\"load_ms\":%llu,\"moved_m\":%.2f,\"dz\":%.2f", g.tSaved - g.t0, now - g.tSaved, d, pos.z - g.pos.z);
			notify::Log("Re-anchor done: %.1f s, %.1f m off", (now - g.t0) / 1000.0, d);
			g.phase = Phase::Idle;
			g.tLoadSeen = 0;
			return;
		}
		}
	}

	void LoadReset()
	{
		g.baselineSet = false;
		g.loadResetAt = GetTickCount64();
		g.gridUnreadySince = 0;
		g.stillSince = 0;
		logger::Write("Re-anchor: load reset; travel baseline will be taken from the rebuilt player position");
	}
}
