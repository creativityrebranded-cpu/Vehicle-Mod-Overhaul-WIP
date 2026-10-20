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
#include "traffic.h"
#include "mappoints.h"

#include <Windows.h>
#include <ShlObj.h>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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
			const std::wstring ini = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\seamless.ini";
			CoTaskMemFree(docs);
			const bool on = GetPrivateProfileIntW(L"context", L"enabled", 1, ini.c_str()) != 0;
			if (on != g_enabled) logger::Write("Context: %s", on ? "enabled" : "disabled (seamless.ini [context] enabled=0)");
			g_enabled = on;
		}

		// Fetch (2026-10-20, the user: enemy behaviours by context): in a fight with vehicles in it (the player in one, or NPC vehicles following),
		// an enemy with no free vehicle within kBoardRange runs to one within kFetchRange.
		constexpr double kFetchRange = 150.0;

		// A vehicle nobody drives and no job uses, nearest to `p` within `range` (one whose autopilot the hit counter switched off is left alone).
		const world::VehicleInfo* FreeVehicleNear(const game::Vec3& p, double range = kBoardRange)
		{
			const world::VehicleInfo* best = nullptr;
			double bestD = range;
			for (std::uint32_t i = 0; i < g_snap.count && i < 64; ++i) {
				const auto& v = g_snap.v[i];
				if (!v.ref || v.who == world::VehicleInfo::Who::Player || v.who == world::VehicleInfo::Who::Npc) continue;
				if (v.formId == vehicleflight::OwnRoverId()) continue;  // your own vehicle: the flight code never lets an NPC fly it
				if (npcvehicles::UsesVehicle(v.formId) || npcvehicles::HasDriver(v.formId) || vehicleflight::AutopilotDisabled(v.formId)) continue;
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

		// ---- Civil rules (2026-10-20, the user: everyday traffic), only when nobody near fights and NPC vehicles are free for it (at most kCivilMax
		// civil jobs, so fights keep the other slots):
		//  - Transit: an NPC (not a follower, calm, within kCivilRange of the player) whose walk goal is over kTransitMin away, with a vehicle it may
		//    use within kTransitNear -> drives there, parks in a clear spot, gets out and walks on.
		//  - Patrol: an owned, parked vehicle within kCivilRange, its owner (or a member of the owning faction) within kPatrolNear and calm -> two laps
		//    round its place, home, park. Each vehicle at most once per kPatrolEveryMs.
		constexpr int kCivilMax = 2;
		constexpr double kShareNear = 15.0, kShareGoal = 100.0;
		// Supply run: an owner by its parked vehicle, a settlement or outpost (the planet's map points) kSupplyMin..kSupplyMax away -> out, wait, back.
		constexpr double kSupplyMin = 200.0, kSupplyMax = 1000.0;
		constexpr int kSupplyChance = 3;  // of 10 patrol chances become supply runs (when a place is in reach)
		constexpr double kCivilRange = 200.0, kTransitMin = 300.0, kTransitNear = 30.0, kPatrolNear = 30.0;
		constexpr ULONGLONG kCivilScanMs = 2000, kCivilRetryMs = 60000, kPatrolEveryMs = 240000;
		ULONGLONG g_civilAt = 0;
		std::unordered_map<std::uint32_t, ULONGLONG> g_civilTried, g_patrolledAt;
		// Shift change (2026-10-20): each calm actor's current AI package (npcvehicles::CurrentPackage); a change = its schedule moved it on, and a far new
		// goal then starts the transit at once (no retry wait).
		std::unordered_map<std::uint32_t, std::uintptr_t> g_lastPackage;
		// Enemy patrol convoys: a hostile owner's patrol takes a second hostile vehicle within kConvoyNear along.
		constexpr double kConvoyNear = 60.0;

		bool VehicleFight(uptr player);

		void CivilRules(uptr player, ULONGLONG now)
		{
			if (now - g_civilAt < kCivilScanMs) return;
			g_civilAt = now;
			if (npcvehicles::IsInCombat(player) || VehicleFight(player) || npcvehicles::CivilJobs() >= kCivilMax) return;
			const auto pp = PosOf(player);
			std::vector<uptr> actors;
			for (const uptr r : modscene::LoadedRefs(kActorLimit)) {
				if (r == player || !npcvehicles::LooksLikeActor(r) || npcvehicles::IsDead(r)) continue;
				if (Dist(PosOf(r), pp) > kCivilRange) continue;
				if (npcvehicles::IsInCombat(r)) return;  // somebody fights here: no civil traffic now
				actors.push_back(r);
			}
			// Transit (a shift change first: the package just changed).
			if (g_lastPackage.size() > 2048) g_lastPackage.clear();
			for (const uptr r : actors) {
				const std::uint32_t id = *reinterpret_cast<std::uint32_t*>(r + off::kFormId);
				const std::uintptr_t pkg = npcvehicles::CurrentPackage(r);
				const auto lp = g_lastPackage.find(id);
				const bool shift = lp != g_lastPackage.end() && lp->second && pkg && lp->second != pkg;
				g_lastPackage[id] = pkg;
				if (shift) {
					g_civilTried.erase(id);
					if (logger::Verbose()) logger::Write("Context: shift change: NPC %08X switched package", id);  // (packages change often: verbose only)
				}
				const auto t = g_civilTried.find(id);
				if (t != g_civilTried.end() && now - t->second < kCivilRetryMs) continue;
				if (!npcvehicles::ActorFree(r)) continue;
				traffic::Destination d;
				if (!traffic::DestinationOf(r, d)) continue;
				const auto np = PosOf(r);
				if (std::hypot(d.x - np.x, d.y - np.y) < kTransitMin) continue;
				g_civilTried[id] = now;
				const auto* v = FreeVehicleNear(np, kTransitNear);
				if (!v || !npcvehicles::MayUse(r, v->ref)) continue;
				const bool ok = npcvehicles::StartTransit(r, v->ref, d.x, d.y);
				logger::Write("Context: transit: NPC %08X walks %.0f m; vehicle %08X -> %s", id, std::hypot(d.x - np.x, d.y - np.y), v->formId, ok ? "taking it" : "refused");
				if (ok) {
					// Shared ride: one more NPC within kShareNear of the driver whose own goal lies within kShareGoal of this one rides along.
					for (const uptr r2 : actors) {
						if (r2 == r || !npcvehicles::ActorFree(r2)) continue;
						const auto p2 = PosOf(r2);
						if (std::hypot(p2.x - np.x, p2.y - np.y) > kShareNear) continue;
						traffic::Destination d2;
						if (!traffic::DestinationOf(r2, d2) || std::hypot(d2.x - d.x, d2.y - d.y) > kShareGoal) continue;
						const bool rideOk = npcvehicles::BoardNpc(r2, v->ref, false);
						logger::Write("Context: shared ride: NPC %08X goes the same way -> %s", *reinterpret_cast<std::uint32_t*>(r2 + off::kFormId), rideOk ? "boarding as a passenger" : "refused");
						break;
					}
					return;
				}
			}
			// Patrol.
			for (std::uint32_t i = 0; i < g_snap.count && i < 64; ++i) {
				const auto& v = g_snap.v[i];
				if (!v.ref || v.who == world::VehicleInfo::Who::Player || v.who == world::VehicleInfo::Who::Npc) continue;
				if (v.formId == vehicleflight::OwnRoverId() || npcvehicles::UsesVehicle(v.formId) || npcvehicles::HasDriver(v.formId) || vehicleflight::AutopilotDisabled(v.formId)) continue;
				if (std::hypot(double(v.pos[0]) - pp.x, double(v.pos[1]) - pp.y) > kCivilRange) continue;
				const auto pt = g_patrolledAt.find(v.formId);
				if (pt != g_patrolledAt.end() && now - pt->second < kPatrolEveryMs) continue;
				for (const uptr r : actors) {
					const auto np = PosOf(r);
					if (std::hypot(double(v.pos[0]) - np.x, double(v.pos[1]) - np.y) > kPatrolNear) continue;
					if (!npcvehicles::ActorFree(r) || !npcvehicles::MayUse(r, v.ref)) continue;
					g_patrolledAt[v.formId] = now;
					// Some of these become supply runs to the nearest settlement / outpost in reach.
					if (int(now / 1000 % 10) < kSupplyChance) {
						std::vector<mappoints::Point> pts;
						const float p3[3] = { v.pos[0], v.pos[1], v.pos[2] };
						const mappoints::Point* best = nullptr;
						double bestD = kSupplyMax;
						if (mappoints::WorldPoints(p3, pts)) {
							for (const auto& q : pts) {
								const double dq = std::hypot(double(q.x) - v.pos[0], double(q.y) - v.pos[1]);
								if (dq >= kSupplyMin && dq < bestD) { bestD = dq; best = &q; }
							}
						}
						if (best) {
							const bool ok = npcvehicles::StartSupply(r, v.ref, best->x, best->y);
							logger::Write("Context: supply run: NPC %08X, vehicle %08X, %.0f m to %s -> %s", *reinterpret_cast<std::uint32_t*>(r + off::kFormId), v.formId, bestD,
								best->outpost ? "an outpost" : "a map point", ok ? "driving" : "refused");
							if (ok) return;
						}
					}
					const bool ok = npcvehicles::StartPatrol(r, v.ref);
					logger::Write("Context: patrol: NPC %08X by its vehicle %08X -> %s", *reinterpret_cast<std::uint32_t*>(r + off::kFormId), v.formId, ok ? "patrolling" : "refused");
					if (ok && npcvehicles::HostileToPlayer(r)) {
						// An enemy patrol: a second hostile vehicle close by with a hostile owner near it follows in convoy.
						for (std::uint32_t k = 0; k < g_snap.count && k < 64; ++k) {
							const auto& v2 = g_snap.v[k];
							if (!v2.ref || v2.formId == v.formId || v2.who == world::VehicleInfo::Who::Player || v2.who == world::VehicleInfo::Who::Npc) continue;
							if (npcvehicles::UsesVehicle(v2.formId) || npcvehicles::HasDriver(v2.formId) || vehicleflight::AutopilotDisabled(v2.formId)) continue;
							if (std::hypot(double(v2.pos[0]) - v.pos[0], double(v2.pos[1]) - v.pos[1]) > kConvoyNear) continue;
							bool sent = false;
							for (const uptr r2 : actors) {
								if (r2 == r || !npcvehicles::ActorFree(r2) || !npcvehicles::HostileToPlayer(r2) || !npcvehicles::MayUse(r2, v2.ref)) continue;
								const auto p2 = PosOf(r2);
								if (std::hypot(double(v2.pos[0]) - p2.x, double(v2.pos[1]) - p2.y) > kPatrolNear) continue;
								sent = npcvehicles::StartConvoy(r2, v2.ref, v.formId);
								logger::Write("Context: enemy convoy: NPC %08X takes vehicle %08X behind %08X -> %s", *reinterpret_cast<std::uint32_t*>(r2 + off::kFormId), v2.formId, v.formId,
									sent ? "following" : "refused");
								break;
							}
							if (sent) break;
						}
					}
					if (ok) return;
				}
				g_patrolledAt[v.formId] = now - kPatrolEveryMs / 2;  // nobody suitable: look again in a while
			}
		}

		// A fight with vehicles in it: the player drives one, or NPC vehicles are following.
		bool VehicleFight(uptr player)
		{
			for (std::uint32_t i = 0; i < g_snap.count && i < 64; ++i) {
				if (g_snap.v[i].who == world::VehicleInfo::Who::Player) return true;
			}
			npcvehicles::JobInfo jobs[8];
			const int n = npcvehicles::Jobs(jobs, 8);
			for (int i = 0; i < n; ++i) {
				if (jobs[i].following) return true;
			}
			(void)player;
			return false;
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
				if (!v && !npcvehicles::IsAlly(r) && VehicleFight(player)) {
					v = FreeVehicleNear(np, kFetchRange);
					if (v) logger::Write("Context: fight with vehicles: enemy %08X runs to vehicle %08X %.0f m away", *reinterpret_cast<std::uint32_t*>(r + off::kFormId), v->formId,
						Dist(np, game::Vec3{ v->pos[0], v->pos[1], v->pos[2] }));
				}
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
				// Return home (2026-10-20): a non-follower's vehicle is flown back to where it was taken and parked there before they get out.
				if (npcvehicles::SendHome(j.vehicle)) {
					g_pursuit.erase(j.npc);
					logger::Write("Context: vehicle %08X calm for 60 s -> flying home before getting out", j.vehicle);
					continue;
				}
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
		CivilRules(player, now);
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
