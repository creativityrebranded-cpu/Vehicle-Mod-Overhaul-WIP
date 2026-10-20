#include "vehiclehits.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "npcvehicles.h"
#include "vehicleflight.h"
#include "vehicleworld.h"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vehiclehits
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class F>
		F EngineFn(std::uint32_t rva) { return reinterpret_cast<F>(game::Base() + rva); }

		constexpr std::uint32_t kLookupForm = 0x005DE3B0;   // (form id) -> TESForm*
		constexpr std::uint32_t kHealthAv = 0x000002D4;     // ActorValueInfo "Health" (Starfield.esm AVIF EDID Health = 000002D4, checked 2026-10-20)
		constexpr int kMax = 96;                            // projectiles followed at once
		constexpr ULONGLONG kProjMaxMs = 4000;              // followed at most this long
		constexpr ULONGLONG kHealAfterMs = 15000;           // no hit this long: the counter heals ...
		constexpr float kHealShare = 0.05f;                 // ... this share of the limit per second

		struct Proj
		{
			std::uint32_t handle = 0, shooter = 0, shooterVeh = 0;
			bool resolved = false;                          // shooterVeh and the side looked up
			int side = 0;                                   // 1 the player's side (the player, a follower, their vehicles), -1 a pursuer, 0 anyone else
			bool seen = false;                              // a position was read
			float p[3] = {}, v[3] = {};
			ULONGLONG at = 0, seenAt = 0;
		};
		std::mutex g_lock;
		Proj g_new[kMax];                                   // from the hook (any thread)
		int g_newCount = 0;
		Proj g_live[kMax];                                  // main thread
		int g_liveCount = 0;

		struct Damage { std::uint32_t id = 0; float damage = 0; ULONGLONG lastHit = 0, lastHeal = 0; unsigned hits = 0; };
		Damage g_dmg[64];

		bool g_iniRead = false;
		float g_hitDamage = 15.0f, g_limitIni = 0.0f, g_playerHealth = 0.0f;
		unsigned g_cooldownS = 30;
		unsigned g_launches = 0, g_hitsTotal = 0;

		void ReadIni()
		{
			if (g_iniRead) return;
			g_iniRead = true;
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const std::wstring ini = std::wstring(docs) + L"\\My Games\\Starfield\\SFSE\\Logs\\SeamlessVehicle\\seamless.ini";
				CoTaskMemFree(docs);
				g_hitDamage = float(GetPrivateProfileIntW(L"hits", L"damage", 15, ini.c_str()));
				g_limitIni = float(GetPrivateProfileIntW(L"hits", L"limit", 0, ini.c_str()));
				g_cooldownS = GetPrivateProfileIntW(L"hits", L"cooldown", 30, ini.c_str());
			}
			g_hitDamage = std::clamp(g_hitDamage, 1.0f, 10000.0f);
			g_cooldownS = std::clamp(g_cooldownS, 1u, 3600u);
			logger::Write("Vehicle hits: %.0f damage per hit, limit %s, autopilot off for %u s at the limit", g_hitDamage, g_limitIni > 0 ? "from the ini" : "= your health",
				g_cooldownS);
		}

		uptr RefById(std::uint32_t id)
		{
			if (!id) return 0;
			__try {
				const uptr r = EngineFn<uptr (*)(std::uint32_t)>(kLookupForm)(id);
				return r && At<std::uint32_t>(r, off::kFormId) == id ? r : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// The projectile's position from its handle (the handle table's lookup takes a reference; it is released at once).
		bool ProjPos(std::uint32_t handle, float* out)
		{
			uptr ref = 0;
			uptr* outp = &ref;
			struct
			{
				uptr vtbl;
				uptr** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &outp };
			__try {
				EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup);
				if (!ref) return false;
				const auto p = At<game::Vec3>(ref, off::kRefPosition);
				out[0] = p.x; out[1] = p.y; out[2] = p.z;
				EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref);
				return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::uint32_t VehicleOf(std::uint32_t shooter)
		{
			const uptr r = RefById(shooter);
			if (!r) return 0;
			__try {
				const uptr v = EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(r);
				return v ? At<std::uint32_t>(v, off::kFormId) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		float g_playerHealthNow = NAN;

		// The player's health (the Health actor value through the ActorValueOwner at +0x70, as gsflight reads ship values); the highest seen is kept.
		float PlayerHealth()
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const uptr av = RefById(kHealthAv);
			float v = NAN;
			if (player && av) {
				__try {
					void** vt = *reinterpret_cast<void***>(player + 0x70);
					v = reinterpret_cast<float (*)(uptr, uptr)>(vt[1])(player + 0x70, av);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					v = NAN;
				}
			}
			g_playerHealthNow = std::isfinite(v) && v >= 0.0f && v <= 100000.0f ? v : NAN;
			if (std::isfinite(v) && v >= 20.0f && v <= 100000.0f && v > g_playerHealth) {
				if (g_playerHealth == 0.0f) logger::Write("Vehicle hits: your health reads %.0f; that is the vehicles' hit limit", v);
				g_playerHealth = v;
			}
			return g_playerHealth;
		}

		float Limit()
		{
			if (g_limitIni > 0) return g_limitIni;
			const float h = PlayerHealth();
			return h > 0 ? h : 300.0f;  // unreadable: a mid-high value
		}

		Damage* DamageFor(std::uint32_t id, bool create)
		{
			Damage* freeSlot = nullptr;
			for (auto& d : g_dmg) {
				if (d.id == id) return &d;
				if (!d.id && !freeSlot) freeSlot = &d;
			}
			if (!create) return nullptr;
			if (!freeSlot) {  // full: the longest-quiet one goes
				freeSlot = &g_dmg[0];
				for (auto& d : g_dmg) if (d.lastHit < freeSlot->lastHit) freeSlot = &d;
			}
			*freeSlot = Damage{};
			freeSlot->id = id;
			return freeSlot;
		}

		// The side a vehicle is on: 1 the player's (their own vehicle, the one they drive, an escort's), -1 a pursuer's, 0 anyone else's.
		int VehicleSide(const world::VehicleInfo& v)
		{
			if (v.who == world::VehicleInfo::Who::Player || v.formId == vehicleflight::OwnRoverId()) return 1;
			const char* p = npcvehicles::ProfileOf(v.formId);
			if (p && p[0] == 'e') return 1;   // escort
			if (p && p[0] == 'p') return -1;  // pursuit
			return 0;
		}

		int ShooterSide(const Proj& pr)
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			if (player && pr.shooter == At<std::uint32_t>(player, off::kFormId)) return 1;
			if (pr.shooter == vehicleflight::OwnRoverId() || pr.shooterVeh == vehicleflight::OwnRoverId()) return 1;
			const uptr r = RefById(pr.shooter);
			if (r && npcvehicles::IsAlly(r)) return 1;
			for (const std::uint32_t id : { pr.shooter, pr.shooterVeh }) {
				const char* p = id ? npcvehicles::ProfileOf(id) : nullptr;
				if (p && p[0] == 'e') return 1;
				if (p && p[0] == 'p') return -1;
			}
			return 0;
		}

		// Does the segment a -> b pass through the vehicle's space (a vertical cylinder, 0.5 m of slack)?
		bool Through(const world::VehicleInfo& v, const float* a, const float* b)
		{
			const double r = (std::max(v.half[0], v.half[1]) > 0.4f ? std::max(v.half[0], v.half[1]) : 2.5f) + 0.5;
			const double hz = v.half[2] > 0.3f ? v.half[2] : 1.5f;
			const double dx = double(b[0]) - a[0], dy = double(b[1]) - a[1], dz = double(b[2]) - a[2];
			const double l2 = dx * dx + dy * dy;
			double t = l2 > 1e-6 ? ((v.pos[0] - a[0]) * dx + (v.pos[1] - a[1]) * dy) / l2 : 0.0;
			t = std::clamp(t, 0.0, 1.0);
			const double cx = a[0] + dx * t, cy = a[1] + dy * t, cz = a[2] + dz * t;
			if (std::hypot(cx - v.pos[0], cy - v.pos[1]) > r) return false;
			return cz >= v.pos[2] - hz - 0.5 && cz <= v.pos[2] + 2.0 * hz + 0.5;
		}

		void Hit(const world::VehicleInfo& v, const Proj& pr, ULONGLONG now)
		{
			Damage* d = DamageFor(v.formId, true);
			const float limit = Limit();
			d->damage += g_hitDamage;
			d->lastHit = now;
			++d->hits;
			++g_hitsTotal;
			if (logger::Verbose() || d->hits <= 3) {
				logger::Write("Vehicle hits: %08X hit by a shot from %08X (%.0f / %.0f)", v.formId, pr.shooter, d->damage, limit);
			}
			if (d->damage < limit) return;
			const bool own = v.formId == vehicleflight::OwnRoverId();
			logger::Write("Vehicle hits: %08X reached the limit (%u hits, %.0f / %.0f): autopilot off for %u s", v.formId, d->hits, d->damage, limit, g_cooldownS);
			vehicleflight::DisableAutopilot(v.formId, g_cooldownS * 1000u, "too many hits");
			if (own) {
				char msg[80];
				std::snprintf(msg, sizeof(msg), "Your vehicle took too many hits: autopilot off for %u s", g_cooldownS);
				notify::Show(msg);
			}
			d->damage = 0;
			d->hits = 0;
		}
	}

	void NoteLaunch(std::uint32_t handle, std::uint32_t shooterFormId)
	{
		if (!handle) return;
		std::lock_guard lk(g_lock);
		if (g_newCount >= kMax) return;
		Proj& p = g_new[g_newCount++];
		p = Proj{};
		p.handle = handle;
		p.shooter = shooterFormId;
		p.at = GetTickCount64();
	}

	void Tick()
	{
		ReadIni();
		const ULONGLONG now = GetTickCount64();
		{
			std::lock_guard lk(g_lock);
			for (int i = 0; i < g_newCount; ++i) {
				if (g_liveCount < kMax) g_live[g_liveCount++] = g_new[i];
				++g_launches;
			}
			g_newCount = 0;
		}
		static world::Snapshot snap;
		const bool haveVehicles = world::Vehicles(snap);
		for (int i = 0; i < g_liveCount;) {
			Proj& pr = g_live[i];
			if (!pr.resolved) {
				pr.resolved = true;
				pr.shooterVeh = VehicleOf(pr.shooter);
				pr.side = ShooterSide(pr);
			}
			float cur[3];
			const bool alive = now - pr.at < kProjMaxMs && ProjPos(pr.handle, cur);
			bool done = !alive;
			if (pr.seen && haveVehicles) {
				float b[3];
				if (alive) {
					b[0] = cur[0]; b[1] = cur[1]; b[2] = cur[2];
				}
				else {
					// Gone (it struck something): the last stretch it would have flown, one and a half frames on.
					const double dt = std::clamp((now - pr.seenAt) / 1000.0, 0.016, 0.1) * 1.5;
					for (int k = 0; k < 3; ++k) b[k] = float(pr.p[k] + pr.v[k] * dt);
				}
				for (std::uint32_t k = 0; k < snap.count && k < 64; ++k) {
					const auto& v = snap.v[k];
					if (!v.formId || v.formId == pr.shooter || v.formId == pr.shooterVeh) continue;
					if (Through(v, pr.p, b)) {
						if (pr.side != 0 && pr.side == VehicleSide(v)) {  // friendly fire: no damage counted (the shot is still used up)
							done = true;
							break;
						}
						Hit(v, pr, now);
						done = true;
						break;
					}
				}
			}
			if (alive && !done) {
				if (pr.seen && now > pr.seenAt) {
					const double dt = (now - pr.seenAt) / 1000.0;
					for (int k = 0; k < 3; ++k) pr.v[k] = float((cur[k] - pr.p[k]) / dt);
				}
				pr.p[0] = cur[0]; pr.p[1] = cur[1]; pr.p[2] = cur[2];
				pr.seen = true;
				pr.seenAt = now;
			}
			if (done) {
				g_live[i] = g_live[--g_liveCount];
				continue;
			}
			++i;
		}
		// Healing after a quiet spell.
		const float limit = Limit();
		for (auto& d : g_dmg) {
			if (!d.id || d.damage <= 0) continue;
			if (now - d.lastHit < kHealAfterMs) { d.lastHeal = now; continue; }
			const double dt = d.lastHeal && now > d.lastHeal ? (now - d.lastHeal) / 1000.0 : 0.0;
			d.lastHeal = now;
			d.damage = std::max(0.0f, d.damage - float(limit * kHealShare * dt));
			if (d.damage <= 0) d.hits = 0;
		}
		static ULONGLONG lastLog = 0;
		if (logger::Verbose() && now - lastLog >= 30000 && g_launches) {
			lastLog = now;
			logger::Write("Vehicle hits: %u projectiles launched, %u followed now, %u vehicle hits so far", g_launches, g_liveCount, g_hitsTotal);
		}
	}

	void Reset()
	{
		{
			std::lock_guard lk(g_lock);
			g_newCount = 0;
		}
		g_liveCount = 0;
		for (auto& d : g_dmg) d = Damage{};
	}

	namespace
	{
		float ReadHealth(uptr actor, uptr av)  // (its own function: no __try next to the map below)
		{
			__try {
				void** vt = *reinterpret_cast<void***>(actor + 0x70);
				return reinterpret_cast<float (*)(uptr, uptr)>(vt[1])(actor + 0x70, av);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return NAN;
			}
		}
	}

	bool ActorHealthShare(std::uintptr_t actor, float& share)
	{
		static std::unordered_map<std::uint32_t, float> best;
		const uptr av = RefById(kHealthAv);
		if (!actor || !av) return false;
		const float v = ReadHealth(actor, av);
		if (!std::isfinite(v) || v < 0.0f || v > 100000.0f) return false;
		if (best.size() > 512) best.clear();
		float& b = best[At<std::uint32_t>(actor, off::kFormId)];
		if (v > b) b = v;
		share = b > 0.0f ? std::clamp(v / b, 0.0f, 1.0f) : 1.0f;
		return true;
	}

	bool PlayerHealthShare(float& share)
	{
		PlayerHealth();
		if (!std::isfinite(g_playerHealthNow) || g_playerHealth <= 0.0f) return false;
		share = std::clamp(g_playerHealthNow / g_playerHealth, 0.0f, 1.0f);
		return true;
	}

	bool DamageOf(std::uint32_t vehicleFormId, float& damage, float& limit)
	{
		const Damage* d = DamageFor(vehicleFormId, false);
		if (!d) return false;
		damage = d->damage;
		limit = Limit();
		return true;
	}
}
