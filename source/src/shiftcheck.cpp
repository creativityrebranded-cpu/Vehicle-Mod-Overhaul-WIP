#include "shiftcheck.h"

#include "addresses.h"
#include "core_api.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "prefetch.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace shiftcheck
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::size_t kMaxRefs = 4000;
		constexpr float kStaticTolerance = 0.5f;   // m
		constexpr float kActorTolerance = 3.0f;    // m: an actor may move within the shift frame
		constexpr float kFallMetres = 10.0f;

		// Class name from the MSVC RTTI behind the vtable (SEH-guarded, no C++ objects here).
		const char* RttiNameOf(std::uintptr_t obj)
		{
			__try {
				const auto vtbl = *reinterpret_cast<std::uintptr_t*>(obj);
				const auto col = *reinterpret_cast<std::uintptr_t*>(vtbl - 8);
				const auto td = game::Base() + *reinterpret_cast<std::uint32_t*>(col + 0x0C);
				return reinterpret_cast<const char*>(td + 0x10);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return "?";
			}
		}

		bool IsActor(std::uintptr_t ref)
		{
			const char* n = RttiNameOf(ref);
			return std::strstr(n, "Actor@@") != nullptr || std::strstr(n, "PlayerCharacter@@") != nullptr;
		}

		struct Snap
		{
			std::uint32_t formId;
			game::Vec3 pos;
			bool actor;
		};

		struct State
		{
			std::int32_t sx = 0, sy = 0;      // core total shift at the last baseline
			bool haveShift = false;
			std::unordered_map<std::uintptr_t, Snap> refs;  // the loaded references of the last baseline
			ULONGLONG snapAt = 0;
			game::Vec3 playerPos{};
			// the check running after a shift
			bool active = false;
			std::int32_t dx = 0, dy = 0;    // the shift (cells)
			ULONGLONG shiftAt = 0;
			int stage = 0;                  // 0 objects, 1..3 ground at +0.1 / +1 / +3 s
			bool asked = false;
			game::Vec3 ringCentre{};
			std::unordered_map<std::uintptr_t, float> actorZ;  // actors' z right after the shift
			std::string summary;
			unsigned ringHits[3][2] = {};   // [stage][near/far]
			unsigned shifts = 0;
		} g;

		struct RefCollector
		{
			void* const* vtbl;
			std::vector<std::uintptr_t>* out;
			static void Noop(RefCollector*) {}
			static int Invoke(RefCollector* self, std::uintptr_t* refPtr)
			{
				if (refPtr && *refPtr && self->out->size() < kMaxRefs) {
					self->out->push_back(*refPtr);
				}
				return 1;
			}
			inline static void* const kVtbl[4] = { reinterpret_cast<void*>(&Noop), reinterpret_cast<void*>(&Noop),
				reinterpret_cast<void*>(&Invoke), reinterpret_cast<void*>(&Noop) };
		};

		// Every reference in the loaded grid cells (state 10 only: attached).
		std::vector<std::uintptr_t> LoadedRefs()
		{
			std::vector<std::uintptr_t> out;
			const auto tes = Ptr(addr::kTESPtr);
			if (!tes) {
				return out;
			}
			const auto entries = At<std::uintptr_t*>(tes, off::kTESGridCells);
			const auto count = std::min<std::uint32_t>(At<std::uint32_t>(tes, off::kTESGridCount), 64);
			for (std::uint32_t i = 0; entries && i < count; ++i) {
				if (!entries[i]) continue;
				const auto cell = At<std::uintptr_t>(entries[i], off::kGridEntryCell);
				if (!cell || At<std::uint8_t>(cell, off::kCellState) != 10) continue;
				RefCollector cb{ RefCollector::kVtbl, &out };
				EngineFn<int (*)(std::uintptr_t, RefCollector*, char)>(addr::kForEachRefInCell)(cell, &cb, 0);
			}
			return out;
		}

		void Baseline(ULONGLONG now)
		{
			g.refs.clear();
			for (const auto r : LoadedRefs()) {
				g.refs[r] = Snap{ At<std::uint32_t>(r, off::kFormId), At<game::Vec3>(r, off::kRefPosition), IsActor(r) };
			}
			if (const auto pl = Ptr(addr::kPlayerCharacterPtr)) {
				g.playerPos = At<game::Vec3>(pl, off::kRefPosition);
			}
			g.snapAt = now;
		}

		// Ground ring: 0 = centre, 1..8 = the 8 near points at 100 m, 9..24 = 16 far points at 200 m.
		void AskRing(const game::Vec3& c)
		{
			const auto* api = seamless::Api();
			if (!api) return;
			int slot = prefetch::kSlotCheck0;
			api->RequestGround(slot++, c.x, c.y, c.z + 2.0f, 0.0f, 3000.0f);  // 0.2: from just above the feet (50 m up started inside city structures)
			for (int i = 0; i < 8; ++i) {
				const double a = i * 3.14159265 / 4;
				api->RequestGround(slot++, float(c.x + 100 * std::cos(a)), float(c.y + 100 * std::sin(a)), c.z + 500.0f, 0.0f, 3000.0f);
			}
			for (int i = 0; i < 16; ++i) {
				const double a = i * 3.14159265 / 8;
				api->RequestGround(slot++, float(c.x + 200 * std::cos(a)), float(c.y + 200 * std::sin(a)), c.z + 500.0f, 0.0f, 3000.0f);
			}
		}

		bool ReadRing(unsigned& centre, unsigned& nearHits, unsigned& farHits)
		{
			const auto* api = seamless::Api();
			if (!api) return false;
			centre = nearHits = farHits = 0;
			for (int i = 0; i < prefetch::kSlotCheckCount; ++i) {
				float h = 0;
				unsigned age = 0;
				if (!api->LatestGround(prefetch::kSlotCheck0 + i, &h, &age) || age > 3) {
					return false;  // not serviced yet
				}
				const bool hit = h > -1e30f && h != -500.0f;
				if (i == 0) centre += hit;
				else if (i <= 8) nearHits += hit;
				else farHits += hit;
			}
			return true;
		}

		void CheckObjects()
		{
			unsigned staticOk = 0, staticBad = 0, actorOk = 0, actorBad = 0, gone = 0;
			std::string worst;
			unsigned listed = 0;
			const float ex = float(g.dx) * off::kCellSize, ey = float(g.dy) * off::kCellSize;
			const auto now = LoadedRefs();
			std::unordered_map<std::uintptr_t, bool> present;
			for (const auto r : now) present[r] = true;
			g.actorZ.clear();
			for (const auto& [ref, s] : g.refs) {
				if (!present.count(ref) || At<std::uint32_t>(ref, off::kFormId) != s.formId) {
					++gone;
					continue;
				}
				const auto p = At<game::Vec3>(ref, off::kRefPosition);
				const float err = std::hypot((p.x + ex) - s.pos.x, (p.y + ey) - s.pos.y);
				const float tol = s.actor ? kActorTolerance : kStaticTolerance;
				if (err <= tol) {
					++(s.actor ? actorOk : staticOk);
				}
				else {
					++(s.actor ? actorBad : staticBad);
					if (listed < 5) {
						++listed;
						char b[160];
						std::snprintf(b, sizeof(b), "%s%08X %s off by %.1f m", worst.empty() ? "" : "; ", s.formId, RttiNameOf(ref), err);
						worst += b;
					}
				}
				if (s.actor) {
					g.actorZ[ref] = p.z;
				}
			}
			const bool pass = staticBad == 0 && actorBad == 0;
			char line[512];
			std::snprintf(line, sizeof(line), "objects %s: statics %u ok / %u wrong, actors %u ok / %u wrong, %u unloaded since the baseline (%.1f s old)%s%s",
				pass ? "PASS" : "FAIL", staticOk, staticBad, actorOk, actorBad, gone, (GetTickCount64() - g.snapAt) / 1000.0, worst.empty() ? "" : " - ", worst.c_str());
			logger::Write("Check: %s", line);
			g.summary = pass ? "objects ok" : "OBJECTS WRONG";
			telemetry::Record("check_objects", "\"shift\":[%d,%d],\"static_ok\":%u,\"static_bad\":%u,\"actor_ok\":%u,\"actor_bad\":%u,\"gone\":%u",
				g.dx, g.dy, staticOk, staticBad, actorOk, actorBad, gone);

			// The player.
			if (const auto pl = Ptr(addr::kPlayerCharacterPtr)) {
				const auto p = At<game::Vec3>(pl, off::kRefPosition);
				const float err = std::hypot((p.x + ex) - g.playerPos.x, (p.y + ey) - g.playerPos.y);
				logger::Write("Check: player %s: %.1f m from the expected position (moves allowed: %.0f m)", err <= kActorTolerance ? "PASS" : "FAIL", err, kActorTolerance);
				g.ringCentre = p;
			}
		}

		void CheckActorsFell()
		{
			unsigned fell = 0, checked = 0;
			std::string who;
			const auto now = LoadedRefs();
			std::unordered_map<std::uintptr_t, bool> present;
			for (const auto r : now) present[r] = true;
			for (const auto& [ref, z0] : g.actorZ) {
				if (!present.count(ref)) continue;
				++checked;
				const float z = At<game::Vec3>(ref, off::kRefPosition).z;
				if (z0 - z > kFallMetres) {
					++fell;
					if (fell <= 5) {
						char b[96];
						std::snprintf(b, sizeof(b), "%s%08X dropped %.0f m", who.empty() ? "" : "; ", At<std::uint32_t>(ref, off::kFormId), z0 - z);
						who += b;
					}
				}
			}
			logger::Write("Check: actors %s: %u of %u dropped more than %.0f m in the 3 s after the shift%s%s", fell ? "FAIL" : "PASS", fell, checked, kFallMetres,
				who.empty() ? "" : " - ", who.c_str());
			telemetry::Record("check_actors", "\"checked\":%u,\"fell\":%u", checked, fell);
		}
	}

	void LoadReset()
	{
		g.refs.clear();
		g.haveShift = false;
		g.active = false;
	}

	// The core's shift events (0.2): the baseline is taken right before the shift, in the same frame, so statics must
	// match to the millimetre and actors to their movement in one frame.
	void BeforeShift(std::int32_t, std::int32_t, std::uintptr_t)
	{
		Baseline(GetTickCount64());
	}

	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t)
	{
		g.dx = cx;
		g.dy = cy;
		g.active = true;
		g.shiftAt = GetTickCount64();
		g.stage = 0;
		g.asked = false;
		std::memset(g.ringHits, 0, sizeof(g.ringHits));
		++g.shifts;
		std::int32_t sx = 0, sy = 0;
		if (const auto* api = seamless::Api()) api->TotalShift(&sx, &sy);
		g.sx = sx;
		g.sy = sy;
		g.haveShift = true;
		logger::Write("Check: --- shift %u: (%d, %d) cells (total now %d, %d) ---", g.shifts, cx, cy, sx, sy);
		CheckObjects();
	}

	// Runs at the core's sync point (game-update thread, the same thread as the shift), so the cells are never read
	// while a shift is changing them.
	void Sync(std::uintptr_t)
	{
		const auto* api = seamless::Api();
		if (!api) {
			return;
		}
		const ULONGLONG now = GetTickCount64();
		std::int32_t sx = 0, sy = 0;
		api->TotalShift(&sx, &sy);
		if (!g.haveShift) {
			g.sx = sx;
			g.sy = sy;
			g.haveShift = true;
		}
		// A shift happened: (total now - total before) is the shift of this re-centre.
		if (sx != g.sx || sy != g.sy) {
			const bool reset = (sx == 0 && sy == 0);  // a frame reset (load / transition), not a shift
			if (!reset && !g.refs.empty() && !api->OnShift) {
				g.dx = sx - g.sx;
				g.dy = sy - g.sy;
				g.active = true;
				g.shiftAt = now;
				g.stage = 0;
				g.asked = false;
				std::memset(g.ringHits, 0, sizeof(g.ringHits));
				++g.shifts;
				logger::Write("Check: --- shift %u: (%d, %d) cells (total now %d, %d) ---", g.shifts, g.dx, g.dy, sx, sy);
				CheckObjects();
			}
			g.sx = sx;
			g.sy = sy;
		}
		if (g.active) {
			const ULONGLONG since = now - g.shiftAt;
			const ULONGLONG at[3] = { 100, 1000, 3000 };
			if (g.stage < 3 && since >= at[g.stage]) {
				if (!g.asked) {
					AskRing(g.ringCentre);
					g.asked = true;
				}
				else {
					unsigned c, n, f;
					if (ReadRing(c, n, f)) {
						float pz = 0;
						if (const auto pl = *reinterpret_cast<const std::uintptr_t*>(game::Base() + addr::kPlayerCharacterPtr)) pz = At<game::Vec3>(pl, off::kRefPosition).z;
						logger::Write("Check: ground at +%.1f s: under the player %s, near ring %u/8, far ring %u/16; player z %.1f (%.1f since the shift)", at[g.stage] / 1000.0,
							c ? "yes" : "NO", n, f, pz, pz - g.ringCentre.z);
						telemetry::Record("check_ground", "\"t\":%.1f,\"centre\":%u,\"near\":%u,\"far\":%u", at[g.stage] / 1000.0, c, n, f);
						g.ringHits[g.stage][0] = c * 100 + n;
						g.ringHits[g.stage][1] = f;
						++g.stage;
						g.asked = false;
					}
					else if (since > at[g.stage] + 2000) {
						logger::Write("Check: ground probes not serviced at +%.1f s", at[g.stage] / 1000.0);
						++g.stage;
						g.asked = false;
					}
				}
			}
			if (g.stage >= 3 && since >= 3000) {
				CheckActorsFell();
				const bool groundNow = (g.ringHits[0][0] / 100) != 0;
				const bool groundLater = (g.ringHits[2][0] % 100) == 8 && g.ringHits[2][1] == 16;
				logger::Write("Check: --- shift %u verdict: %s; ground under the player at +0.1 s %s; full ring by +3 s %s ---", g.shifts, g.summary.c_str(),
					groundNow ? "PASS" : "FAIL", groundLater ? "PASS" : "WARN");
				notify::Log("Shift check %u: %s, ground %s", g.shifts, g.summary.c_str(), groundNow ? "ok" : "MISSING");
				g.active = false;
			}
		}
		// Fallback baseline (older core without shift events): refreshed once a second while no check runs.
		if (!api->OnBeforeShift && !g.active && now - g.snapAt >= 1000) {
			Baseline(now);
		}
	}
}
