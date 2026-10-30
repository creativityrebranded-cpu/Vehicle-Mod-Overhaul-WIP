#include "vehiclekeep.h"
#include "addresses.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "realvehicles.h"
#include "runtimeforms.h"
#include "runtimecells.h"
#include "vehicleflight.h"
#include "framework.h"
#include "npcvehicles.h"
#include "trafficpair.h"
#include "trafficpool.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace vehiclekeep
{
	namespace
	{
		using uptr = std::uintptr_t;

		// FUN_140b36640 (TESObjectREFR::SetFullLOD-like, vtable entry): exteriors only; for furniture it sets ref flag
		// 0x10000 and calls FUN_140b36c40(ref, 1) which sets 0x400 and moves the ref into the worldspace's persistent cell.
		// Clearing (ref, 0) only clears 0x10000; persistence is undone with FUN_140b36c40(ref, 0, 1) (content.cpp uses it).
		constexpr std::uint32_t kSetFullLod = 0x00B36640;
		constexpr std::uint32_t kFullLodFlag = 0x10000, kPersistentFlag = 0x400, kDisabledFlag = 0x800, kDeletedFlag = 0x20;
		constexpr ULONGLONG kEvalMs = 250, kReleaseOutOfSightMs = 10000, kForgetMs = 120000, kSettleMs = 10000;
		constexpr double kCellMetres = 100.0;
		// Where holding starts: a little inside the attached grid's half-width (grid entries, 25 for 5x5 -> 190 m).
		// A visibility estimate only, not where the game's physics stops.
		double HoldEdge(unsigned cells) { return cells ? std::max(120.0, std::sqrt(double(cells)) * 50.0 - 60.0) : 0.0; }

		struct Held { bool setLod = false, setPersist = false; ULONGLONG since = 0, seen = 0; double maxDist = 0; bool had3DFar = false; };

		bool g_enabled = true;
		float g_coneDeg = 60.0f;
		unsigned g_max = 48;
		ULONGLONG g_readAt = 0, g_evalAt = 0, g_reportAt = 0, g_resetAt = 0;
		bool g_journalPending = true;  // first start or a load: release what an earlier run left held
		framework::ScanCursor g_cursor{};
		std::unordered_map<std::uint32_t, ULONGLONG> g_candidates;  // vehicle id -> last time found in an attached cell
		std::unordered_map<std::uint32_t, Held> g_held;
		std::unordered_map<std::uint32_t, bool> g_native;          // already Full LOD by its plugin: never touched

		template <class T> T At(uptr p, std::uint32_t o) { return *reinterpret_cast<const T*>(p + o); }

		struct RefView { std::uint32_t id = 0, flags = 0; game::Vec3 pos{}; uptr world = 0; bool has3D = false; };

		bool ReadRef(uptr ref, RefView& v)
		{
			__try {
				v.flags = At<std::uint32_t>(ref, 0x20);
				v.id = At<std::uint32_t>(ref, 0x28);
				v.pos = At<game::Vec3>(ref, off::kRefPosition);
				const uptr cell = At<uptr>(ref, off::kRefParentCell);
				v.world = cell && !(At<std::uint8_t>(cell, 0x48) & 1) ? At<uptr>(cell, off::kCellWorldspace) : 0;
				const uptr loaded = At<uptr>(ref, off::kRefLoadedData);
				v.has3D = loaded && At<uptr>(loaded, off::kLoadedData3D);
				return std::isfinite(v.pos.x) && std::isfinite(v.pos.y) && std::isfinite(v.pos.z);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		uptr Player() { __try { return *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

		// The camera root's world transform (as scantarget reads it): forward = rotation row 1 (+0x90), position +0xB0.
		bool CameraView(float o[3], float f[3])
		{
			__try {
				const uptr cam = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCameraPtr);
				const uptr root = cam ? At<uptr>(cam, 0x08) : 0;
				if (!root) return false;
				const float fx = At<float>(root, 0x90), fy = At<float>(root, 0x94), fz = At<float>(root, 0x98);
				const float len = std::sqrt(fx * fx + fy * fy + fz * fz);
				if (!std::isfinite(len) || len < 0.5f || len > 1.5f) return false;
				f[0] = fx / len; f[1] = fy / len; f[2] = fz / len;
				const auto p = At<game::Vec3>(root, 0xB0);
				if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
				o[0] = p.x; o[1] = p.y; o[2] = p.z;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool CallSetLod(uptr ref, char on)
		{
			__try { reinterpret_cast<void (*)(uptr, char)>(game::Base() + kSetFullLod)(ref, on); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool CallUnpersist(uptr ref)
		{
			__try { reinterpret_cast<void (*)(uptr, char, char)>(game::Base() + addr::kMarkerPost1)(ref, 0, 1); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		uptr Resolve(std::uint32_t id, RefView& v)
		{
			const uptr ref = id ? runtimeforms::Lookup(id) : 0;
			if (!ref || !ReadRef(ref, v) || v.id != id) return 0;
			return ref;
		}

		std::wstring JournalPath() { return fvopaths::File(L"kept-vehicles.txt"); }

		void WriteJournal()
		{
			const auto path = JournalPath();
			if (path.empty()) return;
			FILE* f = nullptr;
			if (_wfopen_s(&f, path.c_str(), L"w") || !f) return;
			std::fprintf(f, "# vehicles held loaded by FlyingVehicleOverhaul (form id, set Full LOD, set persistent)\n");
			for (const auto& [id, h] : g_held) std::fprintf(f, "%08X %d %d\n", id, h.setLod ? 1 : 0, h.setPersist ? 1 : 0);
			std::fclose(f);
		}

		// Undo exactly what was set. A ref whose cell is detached is handed back to the cell at its position by the native
		// (ref, 0, 1) call, which unloads it like any other reference there.
		void Undo(uptr ref, bool setLod, bool setPersist)
		{
			if (setLod) CallSetLod(ref, 0);
			if (setPersist) CallUnpersist(ref);
		}

		void Release(std::uint32_t id, const char* why, double dist)
		{
			const auto it = g_held.find(id);
			if (it == g_held.end()) return;
			const Held h = it->second;
			g_held.erase(it);
			RefView v;
			const uptr ref = Resolve(id, v);
			if (ref) Undo(ref, h.setLod, h.setPersist);
			RefView after;
			const bool readAfter = ref && ReadRef(ref, after);
			logger::Write("Keep in sight: released %08X (%s) at %.0f m after %.1f s, farthest %.0f m, 3D beyond hold estimate: %s; flags now 0x%X",
				id, why, dist, double(GetTickCount64() - h.since) / 1000.0, h.maxDist, h.had3DFar ? "yes" : "no", readAfter ? after.flags : 0u);
			WriteJournal();
		}

		void ReleaseAll(const char* why)
		{
			std::vector<std::uint32_t> ids;
			for (const auto& [id, h] : g_held) ids.push_back(id);
			for (const auto id : ids) Release(id, why, 0.0);
		}

		// Leftovers from an earlier run or a save made while held: clear only what the journal says was set, and only
		// while the flag is still there (a ref the save no longer holds that way is left alone).
		void CleanJournal()
		{
			const auto path = JournalPath();
			FILE* f = nullptr;
			if (path.empty() || _wfopen_s(&f, path.c_str(), L"r") || !f) return;
			char line[128];
			unsigned cleaned = 0, skipped = 0;
			while (std::fgets(line, sizeof(line), f)) {
				unsigned id = 0; int lod = 0, persist = 0;
				if (line[0] == '#' || std::sscanf(line, "%x %d %d", &id, &lod, &persist) != 3 || g_held.count(id)) continue;
				RefView v;
				const uptr ref = Resolve(id, v);
				if (!ref || !(v.flags & kFullLodFlag)) { ++skipped; continue; }
				Undo(ref, lod != 0, persist != 0 && (v.flags & kPersistentFlag));
				++cleaned;
			}
			std::fclose(f);
			if (cleaned || skipped) logger::Write("Keep in sight: journal from the last run: %u released, %u already normal or gone", cleaned, skipped);
			WriteJournal();
		}

		void ReadSettings()
		{
			const auto& ini = fvopaths::Settings();
			const bool enabled = GetPrivateProfileIntW(L"view", L"keepInSight", 1, ini.c_str()) != 0;
			const float cone = std::clamp(float(GetPrivateProfileIntW(L"view", L"keepCone", 60, ini.c_str())), 5.0f, 89.0f);
			const unsigned max = std::clamp(GetPrivateProfileIntW(L"view", L"keepMax", 16, ini.c_str()), 1u, 256u);
			if (enabled != g_enabled || cone != g_coneDeg || max != g_max)
				logger::Write("Keep in sight: %s, view cone %.0f deg, at most %u vehicles (settings.ini [view])", enabled ? "on" : "off", cone, max);
			g_enabled = enabled; g_coneDeg = cone; g_max = max;
		}

		void Found(uptr ref, void*)
		{
			RefView v;
			if (ReadRef(ref, v) && v.id) g_candidates[v.id] = GetTickCount64();
		}

		void Evaluate(ULONGLONG now)
		{
			const uptr player = Player();
			RefView pv;
			if (!player || !ReadRef(player, pv) || !pv.world) {  // interior, menu, no player: hold nothing
				if (!g_held.empty()) ReleaseAll("player not in an exterior");
				return;
			}
			// 2026-10-27 crash (21:18 run): on leaving a world the camera/grid checks below failed during the transition and
			// vehicles stayed held 5-19 s in the world being torn down. A world change lets go of everything at once.
			static uptr lastWorld = 0;
			if (lastWorld && pv.world != lastWorld && !g_held.empty()) ReleaseAll("player changed world");
			lastWorld = pv.world;
			float co[3], cf[3];
			if (!CameraView(co, cf)) return;
			const unsigned grid = runtimecells::GridSize();
			if (!grid) return;
			// The attached area's half-width around the player; start holding a little inside it so nothing pops out.
			const double edge = HoldEdge(grid);
			const double cosCone = std::cos(double(g_coneDeg) * 3.14159265358979 / 180.0);
			const std::uint32_t own = vehicleflight::OwnRoverId();

			std::vector<std::uint32_t> ids;
			ids.reserve(g_candidates.size() + g_held.size());
			for (const auto& [id, t] : g_candidates) ids.push_back(id);
			for (const auto& [id, h] : g_held) if (!g_candidates.count(id)) ids.push_back(id);

			// 2026-10-29 (the user: stutter and freezes around New Atlantis): each hold or release moves a reference into or out of
			// the world's persistent cell, and a batch of them in one frame cost 13-21 ms (7 holds at once at 13:06:26). One change per
			// evaluation (250 ms); the rest wait for the next one.
			int changes = 0;
			std::size_t decorative=0;
			for(const auto& entry:g_held)if(!npcvehicles::UsesVehicle(entry.first)&&!npcvehicles::VehicleDriverId(entry.first))++decorative;
			std::unordered_map<std::uint32_t,bool> occupied;
			for(const auto id:ids)occupied[id]=npcvehicles::UsesVehicle(id);
			std::stable_sort(ids.begin(),ids.end(),[&](auto a,auto b){return occupied[a]>occupied[b];});
			for (const auto id : ids) {
				RefView v;
				const uptr ref = Resolve(id, v);
				const bool held = g_held.count(id) != 0;
				if (!ref || (v.flags & (kDisabledFlag | kDeletedFlag))) {
					if (held) Release(id, ref ? "disabled" : "gone", 0.0);
					g_candidates.erase(id);
					continue;
				}
				const double dx = double(v.pos.x) - pv.pos.x, dy = double(v.pos.y) - pv.pos.y;
				const double dPlayer = std::sqrt(dx * dx + dy * dy);
				const double cx = double(v.pos.x) - co[0], cy = double(v.pos.y) - co[1], cz = double(v.pos.z) - co[2];
				const double dCam = std::sqrt(cx * cx + cy * cy + cz * cz);
				const bool inSight = dCam > 1.0 && (cx * cf[0] + cy * cf[1] + cz * cf[2]) / dCam >= cosCone;
				// 2026-10-24 (23:11 run): releasing a vehicle hands it back to its cell, which reloads it and drops whoever sits in
				// it. A vehicle with a driver, or one of the scheduled-traffic vehicles, is held regardless of view past the edge
				// and never released for distance or view while that lasts.
				const bool busy = occupied[id] || npcvehicles::VehicleDriverId(id) != 0;
				if (held) {
					Held& h = g_held[id];
					if (v.world != pv.world) { Release(id, "other world", dPlayer); continue; }
					// The pair health record: a broken pair loses the far pass where nobody can see it go, and its model unloads; the removal rule can then take it.
					if (!changes && !inSight && trafficpair::PairHealth(trafficpool::PairHealth(id)) == trafficpair::PairHealth::Broken && dPlayer >= edge && trafficpool::WellHiddenFar(id, dPlayer)) { Release(id, "its pair is broken", dPlayer); ++changes; continue; }
					if (inSight || busy) h.seen = now;
					if (busy && id != own) { if (dPlayer > h.maxDist) h.maxDist = dPlayer; if (dPlayer > edge + 150.0 && v.has3D) h.had3DFar = true; continue; }
					if (dPlayer > h.maxDist) h.maxDist = dPlayer;
					if (dPlayer > edge + 150.0 && v.has3D) h.had3DFar = true;
					if (id == own) { Release(id, "player's own vehicle", dPlayer); ++changes; }
					else if (changes) continue;
					else if (dPlayer < edge - 50.0) { Release(id, "back inside the loaded area", dPlayer); ++changes; }
					else if (now - h.seen > kReleaseOutOfSightMs) { Release(id, "out of sight", dPlayer); ++changes; }
					continue;
				}
				if (now - g_candidates[id] > kForgetMs) { g_candidates.erase(id); continue; }
				if (!(inSight || busy) || dPlayer < edge || id == own || v.world != pv.world || g_native.count(id) || !trafficpair::MayHold(busy,decorative,g_max)) continue;
				if (!trafficpair::MayHoldFar(trafficpair::PairHealth(trafficpool::PairHealth(id)))) continue;  // the pair health record: no far pass until it is seated
				if (v.flags & kFullLodFlag) { g_native[id] = true; continue; }  // its plugin made it Full LOD: not ours
				if (changes) continue;
				++changes;
				if (!CallSetLod(ref, 1)) continue;
				RefView after;
				if (!ReadRef(ref, after) || !(after.flags & kFullLodFlag)) {
					logger::Write("Keep in sight: %08X refused Full LOD (flags 0x%X)", id, after.flags);
					g_native[id] = true;  // do not retry every frame
					continue;
				}
				Held h;
				h.setLod = true;
				h.setPersist = !(v.flags & kPersistentFlag) && (after.flags & kPersistentFlag);
				h.since = h.seen = now;
				h.maxDist = dPlayer;
				g_held[id] = h;
				if(!busy)++decorative;
				logger::Write("Keep in sight: holding %08X at %.0f m (visibility hold estimate %.0f m)%s, 3D %s, flags 0x%X -> 0x%X",
					id, dPlayer, edge, inSight ? "" : " (occupied / traffic, out of view)", after.has3D ? "loaded" : "none", v.flags, after.flags);
				WriteJournal();
			}

			if (now - g_reportAt >= 5000 && !g_held.empty()) {
				g_reportAt = now;
				unsigned with3D = 0; double farthest = 0;
				for (const auto& [id, h] : g_held) {
					RefView v;
					if (Resolve(id, v)) {
						if (v.has3D) ++with3D;
						farthest = std::max(farthest, std::hypot(double(v.pos.x) - pv.pos.x, double(v.pos.y) - pv.pos.y));
					}
				}
				logger::Write("Keep in sight: %zu held, %u with 3D loaded, farthest %.0f m (visibility hold estimate %.0f m)", g_held.size(), with3D, farthest, edge);
			}
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		static fvopaths::Watch watch;  // re-read only when settings.ini changed (2026-10-29 audit)
		if (watch.Changed(fvopaths::Settings())) { g_readAt = now; ReadSettings(); }
		if (!g_resetAt) g_resetAt = now;
		if (now - g_resetAt < kSettleMs) return;  // let a load settle before touching references
		if (g_journalPending) { g_journalPending = false; CleanJournal(); }
		if (!g_enabled) { if (!g_held.empty()) ReleaseAll("turned off"); return; }
		realvehicles::ForEachLoadedStep(Found, nullptr, g_cursor);
		if (now - g_evalAt < kEvalMs) return;
		g_evalAt = now;
		Evaluate(now);
	}

	bool InCameraView(double x, double y, double z)
	{
		float o[3], f[3];
		if (!CameraView(o, f)) return true;
		const double cx = x - o[0], cy = y - o[1], cz = z - o[2], d = std::sqrt(cx * cx + cy * cy + cz * cz);
		return d <= 1.0 || (cx * f[0] + cy * f[1] + cz * f[2]) / d >= std::cos(double(g_coneDeg) * 3.14159265358979 / 180.0);
	}
	void Reset()
	{
		// The held refs belong to the save being left; the loaded save decides their state, and the journal (still listing
		// them) releases any that came back held once the world settles.
		g_held.clear();
		g_candidates.clear();
		g_native.clear();
		g_cursor = {};
		g_resetAt = GetTickCount64();
		g_journalPending = true;
	}
}
