#include "context.h"
#include "addresses.h"
#include "controlmap.h"
#include "crash.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "npcvehicles.h"
#include "vcontract.h"
#include "vehicleflight.h"
#include "vehicleworld.h"

#include <Windows.h>
#include <ShlObj.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace context
{
	namespace
	{
		using uptr = std::uintptr_t;
		constexpr ULONGLONG kTickMs = 500, kScanMs = 1000, kRetryMs = 5000, kCalmMs = 60000;
		constexpr double kBoardRange = 40.0;   // m: a free vehicle this close to the NPC
		constexpr double kActorRange = 80.0;   // m: NPCs further from the player are left alone
		constexpr float kStillSpeed = 2.0f;    // m/s: below this the vehicle counts as parked
		constexpr int kMaxAttemptsPerScan = 2;
		constexpr unsigned kActorLimit = 8000;  // the old force key searched 8000; 2000 cut off NPCs far down the cell list (13:41 fight never seen)

		ULONGLONG g_tickAt = 0, g_scanAt = 0, g_iniAt = 0;
		bool g_enabled = true;
		std::unordered_map<std::uint32_t, bool> g_pursuit;           // npc form id -> chosen profile (true = pursuit)
		std::unordered_map<std::uint32_t, ULONGLONG> g_triedAt;      // npc form id -> last board attempt
		std::unordered_map<std::uint32_t, ULONGLONG> g_calmSince;    // vehicle form id -> calm and still since
		world::Snapshot g_snap;

		uptr Player() { return *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr); }
		game::Vec3 PosOf(uptr ref) { return *reinterpret_cast<game::Vec3*>(ref + off::kRefPosition); }
		double Dist(const game::Vec3& a, const game::Vec3& b) { return std::hypot(double(a.x) - b.x, double(a.y) - b.y); }

		void ReadSwitch(ULONGLONG now)
		{
			if (g_iniAt && now - g_iniAt < 5000) return;
			g_iniAt = now;
			PWSTR docs = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return;
			const std::wstring ini = std::wstring(docs) + L"\My Games\Starfield\SFSE\Logs\SeamlessVehicle\seamless.ini";
			CoTaskMemFree(docs);
			const bool on = GetPrivateProfileIntW(L"context", L"enabled", 1, ini.c_str()) != 0;
			if (on != g_enabled) logger::Write("Context: %s", on ? "enabled" : "disabled (seamless.ini [context] enabled=0)");
			g_enabled = on;
		}

		// A vehicle nobody drives and no job uses, nearest to `p` within kBoardRange.
		const world::VehicleInfo* FreeVehicleNear(const game::Vec3& p)
		{
			const world::VehicleInfo* best = nullptr;
			double bestD = kBoardRange;
			for (std::uint32_t i = 0; i < g_snap.count && i < 64; ++i) {
				const auto& v = g_snap.v[i];
				if (!v.ref || v.who == world::VehicleInfo::Who::Player || v.who == world::VehicleInfo::Who::Npc) continue;
				if (v.formId == vehicleflight::OwnRoverId()) continue;  // your own vehicle: the flight code never lets an NPC fly it
				if (npcvehicles::UsesVehicle(v.formId) || npcvehicles::HasDriver(v.formId)) continue;
				const double d = std::hypot(double(v.pos[0]) - p.x, double(v.pos[1]) - p.y);
				if (d < bestD) { bestD = d; best = &v; }
			}
			return best;
		}

		// A vehicle an NPC drives (not the player's), nearest to `p` within kBoardRange: a passenger seat may be free.
		const world::VehicleInfo* DrivenVehicleNear(const game::Vec3& p)
		{
			const world::VehicleInfo* best = nullptr;
			double bestD = kBoardRange;
			for (std::uint32_t i = 0; i < g_snap.count && i < 64; ++i) {
				const auto& v = g_snap.v[i];
				if (!v.ref || v.who != world::VehicleInfo::Who::Npc || v.formId == vehicleflight::OwnRoverId()) continue;
				const double d = std::hypot(double(v.pos[0]) - p.x, double(v.pos[1]) - p.y);
				if (d < bestD) { bestD = d; best = &v; }
			}
			return best;
		}

		// Rule 1: fight -> fighters take free vehicles.
		void BoardFighters(uptr player, ULONGLONG now)
		{
			if (now - g_scanAt < kScanMs) return;
			g_scanAt = now;
			const auto pp = PosOf(player);
			int attempts = 0;
			crash::Crumb("context actor scan", 0);
			unsigned actors = 0, fighters = 0, inRange = 0, noVehicle = 0, cooling = 0, shown = 0;
			static ULONGLONG lastDiag = 0;
			const bool diagnose = now - lastDiag >= 15000;
			if (diagnose) lastDiag = now;
			for (const uptr r : modscene::LoadedRefs(kActorLimit)) {
				if (attempts >= kMaxAttemptsPerScan) break;
				if (r == player || !npcvehicles::LooksLikeActor(r) || npcvehicles::IsDead(r)) continue;
				++actors;
				const auto cs = npcvehicles::CombatOf(r);
				if (diagnose && shown < 6) {  // what the game says about the nearest few actors (the combat flag and the combat target)
					++shown;
					const auto dp = PosOf(r);
					logger::Write("Context:   actor %08X %.0f m: in combat %d, combat target %08X%s", *reinterpret_cast<std::uint32_t*>(r + off::kFormId), Dist(dp, pp), cs.inCombat ? 1 : 0, cs.target,
						cs.hasTarget ? " (set)" : "");
				}
				if (!cs.fighting) continue;
				++fighters;
				const auto np = PosOf(r);
				if (Dist(np, pp) > kActorRange) continue;
				++inRange;
				const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(r + off::kFormId);
				const auto it = g_triedAt.find(id);
				if (it != g_triedAt.end() && now - it->second < kRetryMs) { ++cooling; continue; }
				const auto* v = FreeVehicleNear(np);
				if (!v) {
					++noVehicle;
					if (!npcvehicles::IsAlly(r)) continue;
					const auto* dv = DrivenVehicleNear(np);
					if (!dv) continue;
					g_triedAt[id] = now;
					++attempts;
					const bool ok = npcvehicles::BoardNpc(r, dv->ref, false);
					logger::Write("Context: fight: ally %08X -> vehicle %08X as passenger -> %s", id, dv->formId, ok ? "boarding" : "refused (no free seat)");
					continue;
				}
				g_triedAt[id] = now;
				++attempts;
				const bool pursuit = !npcvehicles::IsAlly(r);
				const bool ok = npcvehicles::BoardNpc(r, v->ref, true);
				logger::Write("Context: fight: NPC %08X -> vehicle %08X (%.0f m), %s -> %s", id, v->formId, Dist(np, game::Vec3{ v->pos[0], v->pos[1], v->pos[2] }),
					pursuit ? "pursuit" : "escort", ok ? "boarding" : "refused");
				if (ok) g_pursuit[id] = pursuit;
			}
			static ULONGLONG lastSummary = 0;
			if (diagnose) {  // proof the scan sees actors at all
				logger::Write("Context: alive: %u actors scanned, %u in combat, %u vehicles known", actors, fighters, g_snap.count);
			}
			if (fighters && now - lastSummary >= 5000) {
				lastSummary = now;
				logger::Write("Context: scan: %u actors, %u in combat (%u within %.0f m of you), %u with no free vehicle within %.0f m, %u waiting to retry, %u vehicles known; you %s in combat",
					actors, fighters, inRange, kActorRange, noVehicle, kBoardRange, cooling, g_snap.count, npcvehicles::IsInCombat(player) ? "are" : "are not");
			}
		}

		// Rules 2 and 3 for every NPC vehicle job.
		void JobRules(uptr player, ULONGLONG now)
		{
			npcvehicles::JobInfo jobs[8];
			const int n = npcvehicles::Jobs(jobs, 8);
			const bool playerFight = npcvehicles::IsInCombat(player);
			for (int i = 0; i < n; ++i) {
				const auto& j = jobs[i];
				if (!j.following) continue;
				const auto want = g_pursuit.find(j.npc);
				if (want != g_pursuit.end() && want->second != j.pursuit && npcvehicles::SetVehicleProfile(j.vehicle, want->second))
					logger::Write("Context: vehicle %08X -> %s", j.vehicle, want->second ? "pursuit" : "escort");
				const world::VehicleInfo* vi = nullptr;
				for (std::uint32_t k = 0; k < g_snap.count && k < 64; ++k) {
					if (g_snap.v[k].formId == j.vehicle) vi = &g_snap.v[k];
				}
				const float speed = vi ? std::hypot(vi->vel[0], vi->vel[1]) : 99.0f;
				const bool calm = !playerFight && !npcvehicles::IsInCombat(j.npc) && speed < kStillSpeed;
				if (!calm) { g_calmSince.erase(j.vehicle); continue; }
				auto& since = g_calmSince[j.vehicle];
				if (!since) since = now;
				if (now - since < kCalmMs) continue;
				g_calmSince.erase(j.vehicle);
				const int out = vi && vi->ref ? npcvehicles::UnloadVehicle(vi->ref, vcontract::ExitRequest{}, true) : 0;
				g_pursuit.erase(j.npc);
				logger::Write("Context: vehicle %08X calm and parked for 60 s -> %d get out", j.vehicle, out);
			}
		}
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		ReadSwitch(now);
		static ULONGLONG firstAt = now;
		static bool tableLogged = false;
		if (!tableLogged && now - firstAt > 20000) {  // every module has chosen its keys by now
			tableLogged = true;
			controlmap::LogTable();
		}
		if (!g_enabled || now - g_tickAt < kTickMs) return;
		g_tickAt = now;
		const uptr player = Player();
		if (!player || !world::Vehicles(g_snap)) return;
		BoardFighters(player, now);
		JobRules(player, now);
	}

	void Reset()
	{
		g_pursuit.clear();
		g_triedAt.clear();
		g_calmSince.clear();
		g_tickAt = g_scanAt = 0;
	}
}
