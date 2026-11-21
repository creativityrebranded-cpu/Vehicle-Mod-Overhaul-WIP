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
#include "cameraahead.h"
#include "camera_visibility.h"
#include "environment.h"
#include "inputgate.h"
#include "playclock.h"
#include "relocationconsumers.h"  // the relocation receipt: a held vehicle is re-evaluated once per applied serial

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
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
		constexpr ULONGLONG kEvalMs = 250, kReleaseOutOfSightMs = 10000, kForgetMs = 120000, kSettleMs = 10000, kNoCellHoldMs = 60000;  // kNoCellHoldMs: KW-1
		constexpr double kCellMetres = 100.0;
		// Where holding starts: a little inside the attached grid's half-width (grid entries, 25 for 5x5 -> 190 m).
		// A visibility estimate only, not where the game's physics stops.
		double HoldEdge(unsigned cells) { return cells ? std::max(120.0, std::sqrt(double(cells)) * 50.0 - 60.0) : 0.0; }

		struct Held { bool setLod = false, setPersist = false; ULONGLONG since = 0, seen = 0; double maxDist = 0; bool had3DFar = false; ULONGLONG noCellSince = 0; relocrebase::Seen relocSeen; };  // noCellSince: KW-1; relocSeen: the last applied relocation this hold re-based on

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

		struct RefView { std::uint32_t id = 0, flags = 0; game::Vec3 pos{}; uptr world = 0, cell = 0, cellWorld = 0; bool has3D = false; };

		// The worldspace's persistent cell, as mapguard reads it (FUN_140BB54A0 without creating one): +0x1D8, else its parent
		// world's when the world uses its parent's persistent references (+0xF2 bit 6, parent +0x1F0).
		uptr PersistentCell(uptr ws)
		{
			__try {
				for (int k = 0; ws && k < 4; ++k) {
					if (const uptr c = At<uptr>(ws, 0x1D8)) return c;
					if (!At<uptr>(ws, 0x1F0) || !((At<std::uint8_t>(ws, 0xF2) >> 6) & 1)) return 0;
					ws = At<uptr>(ws, 0x1F0);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			return 0;
		}
		// Same world as the player. A persistent ref beyond the loaded cells (or one made Full LOD) is listed in the persistent cell,
		// which on a planet world can be the PARENT world's, so its cell names the parent (2026-11-06 15:45: far traffic released
		// as 'other world' after 3 s, 11 of 14 never held; suspected, traced by keep.world).
		bool SameWorld(const RefView& v, uptr playerWorld)
		{
			if (!playerWorld) return false;
			if (v.world == playerWorld) return true;
			const uptr pc = PersistentCell(playerWorld);
			return pc && v.cell == pc;
		}

		bool ReadRef(uptr ref, RefView& v)
		{
			__try {
				v.flags = At<std::uint32_t>(ref, 0x20);
				v.id = At<std::uint32_t>(ref, 0x28);
				v.pos = At<game::Vec3>(ref, off::kRefPosition);
				const uptr cell = At<uptr>(ref, off::kRefParentCell);
				v.cell = cell;
				v.cellWorld = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
				v.world = cell && !(At<std::uint8_t>(cell, 0x48) & 1) ? v.cellWorld : 0;
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

		std::uint32_t FormIdOf(uptr form) { __try { return form ? At<std::uint32_t>(form, 0x28) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
		std::uint8_t CellFlagsOf(uptr cell) { __try { return cell ? At<std::uint8_t>(cell, off::kCellFlags) : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
		// keep.world / keep.skip: the cell and worlds behind a world decision, once per vehicle and reason (2026-11-06 far traffic).
		std::unordered_map<std::uint32_t, const char*> g_worldLogged, g_skipLogged;
		void TraceWorld(std::uint32_t id, const RefView& v, uptr playerWorld, const char* event)
		{
			if (g_worldLogged.size() > 4096) g_worldLogged.clear();
			auto& last = g_worldLogged[id];
			if (last == event) return;
			last = event;
			const uptr pc = PersistentCell(playerWorld);
			logger::Write("OBS system=keep.world vehicle=%08X event=%s cell=%08X cell_flags=0x%X cell_world=%08X player_world=%08X player_persistent_cell=%08X in_player_persistent=%d",
				id, event, FormIdOf(v.cell), unsigned(CellFlagsOf(v.cell)), FormIdOf(v.cellWorld), FormIdOf(playerWorld), FormIdOf(pc), int(pc && v.cell == pc));
		}
		void TraceSkip(std::uint32_t id, const RefView& v, uptr playerWorld, const char* reason, double dPlayer, int health)
		{
			if (g_skipLogged.size() > 4096) g_skipLogged.clear();
			auto& last = g_skipLogged[id];
			if (last == reason) return;
			last = reason;
			logger::Write("OBS system=keep.skip vehicle=%08X event=not_held reason=%s distance=%.0f has_3d=%d flags=0x%X pair_health=%d",
				id, reason, dPlayer, int(v.has3D), v.flags, health);
			if (std::strcmp(reason, "other_world") == 0) TraceWorld(id, v, playerWorld, "skip_other_world");
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

		unsigned g_birthMax = 40;
		void ReadSettings()
		{
			// Fixed (2026-11-05 settings strip; was settings.ini [view] keepInSight / keepCone / keepMax / holdFromBirth).
			g_enabled = true; g_coneDeg = 60.0f; g_max = 16; g_birthMax = 40;
			logger::Write("Keep in sight: on, view cone 60 deg, at most 16 vehicles; held from birth 40");
		}

		// Traffic vehicles held from birth, in view or not (2026-10-31, the foundation: 35-57 of 76-94 pairs never got a chassis because their model only loads when the player comes near,
		// and this list only saw vehicles already loaded). [view] holdFromBirth = how many at once; 0 = off. Paced like every hold: one change per evaluation.
		std::mutex g_wantedLock;
		std::unordered_set<std::uint32_t> g_wanted;
		bool Wanted(std::uint32_t id) { std::lock_guard lock(g_wantedLock); return g_wanted.count(id) != 0; }
		void Unwant(std::uint32_t id) { std::lock_guard lock(g_wantedLock); g_wanted.erase(id); }
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

			{
				std::lock_guard lock(g_wantedLock);
				for (const auto id : g_wanted) g_candidates[id] = now;  // not loaded yet, so the scan will not find them
			}
			unsigned heldFromBirth = 0;
			for (const auto& [id, h] : g_held) if (Wanted(id)) ++heldFromBirth;
			std::vector<std::uint32_t> ids;
			ids.reserve(g_candidates.size() + g_held.size());
			for (const auto& [id, t] : g_candidates) ids.push_back(id);
			for (const auto& [id, h] : g_held) if (!g_candidates.count(id)) ids.push_back(id);

			// 2026-10-29 (the user: stutter and freezes around New Atlantis): each hold or release moves a reference into or out of
			// the world's persistent cell, and a batch of them in one frame cost 13-21 ms (7 holds at once at 13:06:26). One change per
			// evaluation (250 ms); the rest wait for the next one.
			int changes = 0;
			std::size_t decorative=0;
			for(const auto& entry:g_held)if(!npcvehicles::UsesVehicle(entry.first)&&!npcvehicles::VehicleDriverId(entry.first)&&!Wanted(entry.first))++decorative;
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
					Unwant(id);
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
				const bool wanted = Wanted(id);
				const bool busy = occupied[id] || npcvehicles::VehicleDriverId(id) != 0 || (wanted && (held || heldFromBirth < g_birthMax));
				if (held) {
					Held& h = g_held[id];
					// Relocation receipt (applied, once per serial): the view, distance and cell facts of the old place no longer hold. The out-of-sight clock, the
					// no-cell clock and the farthest distance restart from the destination; the release rules below then judge fresh facts. Nothing is released here.
					if (relocrebase::Take(h.relocSeen, relocconsumers::Poll(id), id)) {
						h.seen = now; h.noCellSince = 0; h.maxDist = dPlayer; h.had3DFar = false;
						logger::Write("OBS system=keep.rebased vehicle=%08X event=rebased serial=%llu distance=%.0f reason=relocation_receipt_applied_hold_clocks_restarted", id, static_cast<unsigned long long>(relocconsumers::Peek(id).serial), dPlayer);
					}
					// KW-1 (2026-11-07, user: "build it"): past the loaded cells the game can report no parent cell at all (16:11 run: 6 held vehicles
						// released as "other world" at ~510 m with cell=00000000 while the player's world was FF013B0E). No cell says nothing about the
						// world: the hold stays, still bound by view, distance and every other release; a real cell in another world still releases.
						// Bound: kNoCellHoldMs without any cell releases it ("no cell for too long").
						if (!v.cell) {
							if (!h.noCellSince) { h.noCellSince = now; TraceWorld(id, v, pv.world, "held_no_cell_world_unknown"); }
							else if (now - h.noCellSince > kNoCellHoldMs) { TraceWorld(id, v, pv.world, "release_no_cell_too_long"); Release(id, "no cell for too long", dPlayer); continue; }
						}
						else { h.noCellSince = 0; if (!SameWorld(v, pv.world)) { TraceWorld(id, v, pv.world, "release_other_world"); Release(id, "other world", dPlayer); continue; } }
					if (v.cell && v.world != pv.world) TraceWorld(id, v, pv.world, "held_in_persistent_cell");  // KW-1: not for no cell
					// H1: Health's verdict through the Census copy (was the core's own pair health record, a second judge): a broken pair loses the far pass where nobody can see it go, and its model unloads; the removal rule can then take it.
					if (!changes && !inSight && vehicleflight::CensusBroken(id) && dPlayer >= edge && trafficpool::WellHiddenFar(id, dPlayer)) { Release(id, "its pair is broken", dPlayer); ++changes; continue; }
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
				// Wanted (traffic from birth) but not held: the reason is traced once per change (keep.skip), never silent.
				const char* skip = !(inSight || busy) ? nullptr : dPlayer < edge ? nullptr : id == own ? nullptr
					: !SameWorld(v, pv.world) ? "other_world" : g_native.count(id) ? "native_full_lod" : !trafficpair::MayHold(busy,decorative,g_max) ? "budget"
					: !trafficpair::MayHoldFar(trafficpair::PairHealth(trafficpool::PairHealth(id))) ? "pair_health" : nullptr;
				if (wanted && skip) TraceSkip(id, v, pv.world, skip, dPlayer, trafficpool::PairHealth(id));
				if (!(inSight || busy) || dPlayer < edge || id == own || !SameWorld(v, pv.world) || g_native.count(id) || !trafficpair::MayHold(busy,decorative,g_max)) continue;
				if (!trafficpair::MayHoldFar(trafficpair::PairHealth(trafficpool::PairHealth(id)))) continue;  // the pair health record: no far pass until it is seated
				if (v.world != pv.world) TraceWorld(id, v, pv.world, "hold_in_persistent_cell");
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
				if(wanted)++heldFromBirth;
				else if(!busy)++decorative;
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
				std::size_t wantedCount = 0;
				{ std::lock_guard lock(g_wantedLock); wantedCount = g_wanted.size(); }
				logger::Write("OBS system=keep.birth vehicle=00000000 event=state wanted=%zu held_from_birth=%u max=%u held=%zu with_model=%u", wantedCount, heldFromBirth, g_birthMax, g_held.size(), with3D);
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

	void WantFromBirth(std::uint32_t id)
	{
		if (!id || !g_birthMax) return;
		std::lock_guard lock(g_wantedLock);
		g_wanted.insert(id);
	}
#ifndef FVO_RELEASE
	bool ReadCameraPose(float origin[3], float forward[3]) { return CameraView(origin, forward); }
#endif
	bool InCameraView(double x, double y, double z)
	{
		float o[3], f[3];
		if (!CameraView(o, f)) return true;
		const double cx = x - o[0], cy = y - o[1], cz = z - o[2], d = std::sqrt(cx * cx + cy * cy + cz * cz);
		return d <= 1.0 || (cx * f[0] + cy * f[1] + cz * f[2]) / d >= std::cos(double(g_coneDeg) * 3.14159265358979 / 180.0);
	}
	// Card CP-1: camera prediction. Two camera samples at least cameraahead::rules::minGap apart give the turn; a stale pair
	// (older than maxGap) reads as "turn unknown", which counts as seen. Sampled on demand: Retirement asks every frame while a
	// broken pair waits, so the samples stay fresh exactly when they are needed. Any thread; the samples are under a lock.
	static int CameraAheadForEnvelope(double x, double y, double z, double seconds, double* turnDegPerS, double radius)
	{
		static std::mutex lock;
		static cameraahead::Sample prev, now;
		float o[3], f[3];
		if (!CameraView(o, f)) { if (turnDegPerS) *turnDegPerS = 0; return 3; }
		const double t = GetTickCount64() / 1000.0;
		cameraahead::Sample a, b;
		{
			std::lock_guard guard(lock);
			if (now.t < 0 || t - now.t >= cameraahead::rules::minGap) { prev = now; now.t = t; for (int i = 0; i < 3; ++i) { now.o[i] = o[i]; now.f[i] = f[i]; } }
			a = prev; b = now;
		}
		// Judge against the live camera (b's forward may be up to minGap old; the live one is the truth for "in view now").
		const double od[3] = { o[0], o[1], o[2] }, fd[3] = { f[0], f[1], f[2] };
		if (cameraahead::InCone(od, fd, x, y, z, std::cos(double(g_coneDeg) * 3.14159265358979 / 180.0), radius)) { if (turnDegPerS) *turnDegPerS = 0; return 1; }
		return cameraahead::Judge(a, b, x, y, z, seconds, double(g_coneDeg), turnDegPerS, radius);
	}
	int CameraAhead(double x, double y, double z, double seconds, double* turnDegPerS)
	{
		return CameraAheadForEnvelope(x, y, z, seconds, turnDegPerS, 0);
	}
	namespace {
		using cameravisibility::Evidence;
		namespace sight = cameravisibility::rules;
		using Point = std::array<float, 3>;
		struct CameraProbe {
			unsigned id = 0;
			std::uint64_t at = 0;
			Point origin{}, centre{};
			std::array<Point, sight::sampleCount> ends{};
			double hull = 0;
			bool visible = false;
		};
		std::array<CameraProbe, sight::cacheSize> g_cameraProbes{};
		std::uint64_t g_cameraLaunch = 0;
		std::mutex g_lookLock;
		struct LookHistory {
			cameravisibility::Forecast sample;
			cameravisibility::Mouse mouse;
			double yawGain = 0, pitchGain = 0;
		} g_look;

		bool ReadLookPose(cameravisibility::Forecast& out)
		{
			Point origin{},forward{};
			if(!CameraView(origin.data(),forward.data()))return false;
			for(unsigned i=0;i<3;++i){out.origin[i]=origin[i];out.forward[i]=forward[i];}
			out.coneDeg=g_coneDeg;
			return true;
		}
		bool CurrentLook(cameravisibility::Forecast& out)
		{
			const auto now=GetTickCount64();
			{
				std::lock_guard lock(g_lookLock);
				out=g_look.sample;
			}
			out.mouseAt=inputgate::LookMouse().at;
			if(!cameravisibility::Fresh(out.at,now,sight::mouseMs)||
			   !cameravisibility::Fresh(out.mouseAt,now,sight::mouseMs))out.yawRate=out.pitchRate=0;
			return ReadLookPose(out);
		}

		double CameraDistance(const Point& a, const Point& b)
		{
			return std::hypot(double(a[0])-b[0], double(a[1])-b[1], double(a[2])-b[2]);
		}
		CameraProbe MakeCameraProbe(unsigned id, std::uint64_t now, const Point& origin, const Point& centre, double radius, double height)
		{
			CameraProbe probe;
			probe.id=id; probe.at=now; probe.origin=origin; probe.centre=centre;
			probe.hull=std::max(radius,height);
			probe.ends.fill(centre);
			const double dx=double(centre[0])-origin[0],dy=double(centre[1])-origin[1],planar=std::hypot(dx,dy);
			const double rightX=planar>1e-3?-dy/planar:1, rightY=planar>1e-3?dx/planar:0;
			probe.ends[1][0]+=float(radius*rightX); probe.ends[1][1]+=float(radius*rightY);
			probe.ends[2][0]-=float(radius*rightX); probe.ends[2][1]-=float(radius*rightY);
			probe.ends[3][2]+=float(height); probe.ends[4][2]-=float(height);
			return probe;
		}
		Evidence ObserveCameraProbe(unsigned id, std::uint64_t now, const Point& origin, const Point& centre, double radius, double height)
		{
			if(CameraDistance(origin,centre)>sight::maxRange)return Evidence::Unknown;
			auto probe=std::find_if(g_cameraProbes.begin(),g_cameraProbes.end(),[&](const CameraProbe& p){
				return p.id==id&&cameravisibility::Fresh(p.at,now,sight::sampleMs)&&
					CameraDistance(origin,p.origin)<=sight::drift&&CameraDistance(centre,p.centre)<=sight::drift;
			});
			if(probe==g_cameraProbes.end()){
				if(cameravisibility::Fresh(g_cameraLaunch,now,sight::launchMs))return Evidence::Waiting;
				probe=std::min_element(g_cameraProbes.begin(),g_cameraProbes.end(),[](const auto& a,const auto& b){return a.at<b.at;});
				*probe=MakeCameraProbe(id,now,origin,centre,radius,height);g_cameraLaunch=now;
			}
			if(probe->visible)return Evidence::Visible;
			unsigned blocked=0,visible=0;
			for(const auto& end:probe->ends){
				bool hit=false;double distance=0;
				const bool answered=environment::CameraRay(id,probe->origin.data(),end.data(),hit,distance);
				const auto evidence=cameravisibility::RayEvidence(answered,hit,distance,CameraDistance(probe->origin,end),probe->hull);
				blocked+=evidence==Evidence::Occluded;visible+=evidence==Evidence::Visible;
			}
			const auto result=cameravisibility::Combine(blocked,visible);
			probe->visible=result==Evidence::Visible;
			logger::Observe("traffic.camera",id,unsigned(result),"event=envelope result=%d blocked=%u visible=%u range=%.1f",
				int(result),blocked,visible,CameraDistance(origin,centre));
			return result;
		}
	}
	void UpdateTrafficLook()
	{
		cameravisibility::Forecast next;
		const bool live=playclock::Running()&&ReadLookPose(next);
		next.at=GetTickCount64();
		std::lock_guard lock(g_lookLock);
		if(!live){g_look={};return;}
		auto& previous=g_look.sample;
		if(previous.at&&next.at-previous.at<30)return;
		const auto mouse=inputgate::LookMouse();
		next.mouseAt=mouse.at;
		if(cameravisibility::Fresh(previous.at,next.at,300)&&cameravisibility::Fresh(mouse.at,next.at,sight::mouseMs)){
			const auto seconds=(next.at-previous.at)*.001;
			const double yaw=cameravisibility::Wrap(cameravisibility::Yaw(next.forward)-cameravisibility::Yaw(previous.forward));
			const double pitch=cameravisibility::Pitch(next.forward)-cameravisibility::Pitch(previous.forward);
			next.yawRate=cameravisibility::MouseRate(yaw,double(mouse.x-g_look.mouse.x),seconds,g_look.yawGain);
			next.pitchRate=cameravisibility::MouseRate(pitch,double(mouse.y-g_look.mouse.y),seconds,g_look.pitchGain);
		}
		previous=next;g_look.mouse=mouse;
	}
	static int ReadTrafficForecast(cameravisibility::Forecast* out)
	{
		return out&&CurrentLook(*out)&&(out->yawRate||out->pitchRate)?1:0;
	}
	bool HoldBirthForLook(double x,double y,double z,unsigned milliseconds)
	{
		cameravisibility::Forecast look;
		if(!playclock::Running()||!CurrentLook(look))return false;
		const auto radius=std::hypot(sight::fallbackRadius,sight::fallbackHeight);
		const int eta=cameravisibility::EntryMs(look,x,y,z,radius,milliseconds);
		if(eta<0)return false;
		logger::Observe("traffic.camera",0,1,"event=birth_hold eta_ms=%d budget_ms=%u",eta,milliseconds);
		return true;
	}
	cameravisibility::Evidence TrafficCameraEvidence(unsigned id,unsigned base,double x,double y,double z,bool driver)
	{
		constexpr double radians=3.14159265358979/180.0;
		if(!id||!playclock::Running()||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return Evidence::Unknown;
		cameravisibility::Forecast look;
		if(!CurrentLook(look))return Evidence::Unknown;
		double radius=sight::driverRadius,height=sight::driverHeight;
		if(!driver){
			double half[3]{},bottom=0,top=0;
			if(realvehicles::ReadModelBounds(runtimeforms::Lookup(base),half,bottom,top)){
				radius=std::hypot(half[0],half[1])+1;height=half[2]+1;
			}else{radius=sight::fallbackRadius;height=sight::fallbackHeight;}
		}else z+=height;
		const Point centre{float(x),float(y),float(z)};
		const Point origin{float(look.origin[0]),float(look.origin[1]),float(look.origin[2])};
		const double envelope=std::hypot(radius,height);
		const auto now=GetTickCount64();
		const int eta=cameravisibility::EntryMs(look,x,y,z,envelope,sight::retireMs);
		const bool recent=cameravisibility::Fresh(look.mouseAt,now,sight::mouseMs);
		const bool fringe=eta<0&&recent&&cameraahead::InCone(look.origin,look.forward,x,y,z,
			std::cos((look.coneDeg+sight::fringeDeg)*radians),envelope);
		if(eta<0&&!fringe)return Evidence::Outside;
		const auto rays=ObserveCameraProbe(id,now,origin,centre,radius,height);
		const auto result=(eta!=0&&rays==Evidence::Waiting)?Evidence::MouseTurn:rays;
		logger::Observe("traffic.lookahead",id,unsigned(result),"event=retirement result=%d eta_ms=%d budget_ms=%u fringe=%d",
			int(result),eta,sight::retireMs,fringe);
		return result;
	}
	void ForEachHeld(void (*visit)(std::uint32_t))
	{
		for (const auto& [id, h] : g_held) visit(id);
	}

	void Reset()
	{
		g_cameraProbes={};g_cameraLaunch=0;
		{std::lock_guard lock(g_lookLock);g_look={};}
		{ std::lock_guard lock(g_wantedLock); g_wanted.clear(); }
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

// Card CP-1: camera prediction for the addon's Retirement. Optional export: an older addon never calls it; an older core lacks it and
// the addon keeps its far-and-out-of-sight rule. Returns vehiclekeep::CameraAhead's code; turnDegPerS may be null.
extern "C" __declspec(dllexport) int FVO_CameraAheadV1(double x, double y, double z, double seconds, double* turnDegPerS)
{
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !(seconds >= 0) || seconds > 3) return 3;
	return vehiclekeep::CameraAhead(x, y, z, seconds, turnDegPerS);
}

extern "C" __declspec(dllexport) int FVO_CameraEnvelopeAheadV1(double x, double y, double z, double seconds, double radius)
{
	if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
		!std::isfinite(seconds) || seconds < 0 || !std::isfinite(radius) || radius <= 0) return 3;
	return vehiclekeep::CameraAheadForEnvelope(x, y, z, seconds, nullptr, radius);
}

extern "C" __declspec(dllexport) cameravisibility::Evidence FVO_TrafficCameraEvidenceV1(unsigned id,unsigned base,double x,double y,double z,bool driver)
{
	return vehiclekeep::TrafficCameraEvidence(id,base,x,y,z,driver);
}

extern "C" __declspec(dllexport) int FVO_TrafficLookForecastV1(cameravisibility::Forecast* out)
{
	return vehiclekeep::ReadTrafficForecast(out);
}
