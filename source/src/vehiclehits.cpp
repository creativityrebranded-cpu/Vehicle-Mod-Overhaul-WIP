#include "vehiclehits.h"
#include "features.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "npcvehicles.h"
#include "vehicleflight.h"
#include "vehicleworld.h"
#include "interactionlogic.h"
#include "weapondamage.h"
#include "combat_relations.h"
#include "environment.h"

#include <Windows.h>
#include <ShlObj.h>

#include "hook.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cstring>
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
		// Vehicle health (2026-10-21, the user approved the lifebar plan): every vehicle has [hits] health= (300); at 0 it is wrecked (autopilot,
		// hover and weapon off, the NPC driver gets out) until it heals over kRepairedShare; a wreck heals later and slower. A hit also takes from
		// the driver: a full bar = [hits] driverShare= percent (25) of the driver's own health; our share never takes a driver below 1 health
		// (direct shots kill as usual).
		constexpr ULONGLONG kWreckHealAfterMs = 30000;
		constexpr float kWreckHealShare = 0.01f;
		constexpr float kRepairedShare = 0.25f;             // health share: a wreck is repaired over this
		constexpr ULONGLONG kWreckRenewMs = 20000;          // a wreck's autopilot ban is renewed this often
		constexpr std::uint32_t kCrosshairHandle = 0x061FAE94;  // the HUD enemy list's crosshair target handle (FUN_141489EA0 reads DAT_1461FAE94)
		constexpr std::uint32_t kHostileCount = 0xEF8, kHostileArray = 0xF00;  // PlayerCharacter: the HUD enemy list's handles (FUN_141489B90)

		struct Proj
		{
			std::uint32_t handle = 0, shooter = 0, shooterVeh = 0, projectileId = 0;
			float weaponRange=0,falloff=1;
            float weaponDamage = NAN;                       // native total at launch; no borrowed weapon/projectile pointers
			std::uint32_t weaponId = 0, attackerId = 0;      // actual driver when the launch shooter is a vehicle
			bool resolved = false;                          // shooterVeh and the side looked up
			int side = 0;                                   // 1 the player's side (the player, a follower, their vehicles), -1 a pursuer, 0 anyone else
			bool seen = false;                              // a position was read
			bool haveLine = false;                          // the launch point and direction are known
			float o[3] = {}, d[3] = {};
			float p[3] = {}, v[3] = {};
			float closest = 1e9f;                           // the nearest any reading came to a vehicle's centre (the miss log)
			std::uint32_t closestId = 0;
			unsigned frames = 0;                            // positions read
			ULONGLONG at = 0, seenAt = 0;
		};
		std::mutex g_lock;
		Proj g_new[kMax];                                   // from the hook (any thread)
		int g_newCount = 0;
		struct NativeImpact { std::uint32_t projectile=0,shooterHandle=0,weapon=0; float damage=NAN,point[3]{}; ULONGLONG at=0; };
		NativeImpact g_newImpacts[256]; int g_newImpactCount=0;
		combatrelations::PairCooldown<512> g_projectileHits;
		struct WeaponFlight { Ballistics data; std::uint32_t weapon=0; ULONGLONG at=0; };
        std::unordered_map<std::uint32_t,WeaponFlight> g_ballistics;
        Proj g_live[kMax];                                  // main thread
		int g_liveCount = 0;

		struct Damage { std::uint32_t id = 0; float damage = 0; ULONGLONG lastHit = 0, lastHeal = 0, wreckRenew = 0; unsigned hits = 0; bool wrecked = false;
			int lastSide = 0; std::uint32_t lastShooter = 0; };
		Damage g_dmg[64];

		bool g_iniRead = false;
		float g_hitDamage = 20.0f, g_weaponScale = 1.0f, g_limitIni = 0.0f, g_playerHealth = 0.0f, g_vehicleHealth = 300.0f, g_driverShare = 0.25f, g_ramCap = 0.35f, g_shotCap = 0.20f;
		unsigned g_weaponChecks = 0;
		unsigned g_probes = 0, g_driverChecks = 0;
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
				g_hitDamage = float(GetPrivateProfileIntW(L"hits", L"damage", 20, ini.c_str()));
				wchar_t scale[64]{};
				GetPrivateProfileStringW(L"hits", L"weaponScale", L"1", scale, 64, ini.c_str());
				wchar_t* end = nullptr;
				const float parsed = std::wcstof(scale, &end);
				while (end && (*end == L' ' || *end == L'\t')) ++end;
				g_weaponScale = end && end != scale && !*end ? weapondamage::Scale(parsed) : 1.0f;
				g_vehicleHealth = float(GetPrivateProfileIntW(L"hits", L"health", 300, ini.c_str()));
				g_driverShare = float(GetPrivateProfileIntW(L"hits", L"driverShare", 25, ini.c_str())) / 100.0f;
				g_ramCap = float(GetPrivateProfileIntW(L"hits", L"ramCap", 35, ini.c_str())) / 100.0f;
				g_shotCap = float(GetPrivateProfileIntW(L"hits", L"shotCap", 20, ini.c_str())) / 100.0f;
				g_limitIni = float(GetPrivateProfileIntW(L"hits", L"limit", 0, ini.c_str()));
				g_cooldownS = GetPrivateProfileIntW(L"hits", L"cooldown", 30, ini.c_str());
			}
			g_hitDamage = std::clamp(g_hitDamage, 1.0f, 10000.0f);
			g_cooldownS = std::clamp(g_cooldownS, 1u, 3600u);
			g_vehicleHealth = std::clamp(g_vehicleHealth, 10.0f, 100000.0f);
			g_driverShare = std::clamp(g_driverShare, 0.0f, 1.0f);
			g_ramCap = std::clamp(g_ramCap, 0.0f, 1.0f);
			g_shotCap = std::clamp(g_shotCap, 0.01f, 1.0f);
			logger::Write("Vehicle hits: %.0f damage per hit, vehicle health %.0f%s; a full bar costs the driver %.0f%% of their health; wrecked at 0 (repaired over %.0f%%)",
				g_hitDamage, g_limitIni > 0 ? g_limitIni : g_vehicleHealth, g_limitIni > 0 ? " (ini limit=)" : "", g_driverShare * 100.0f, kRepairedShare * 100.0f);
			logger::Write("Vehicle hits: one ram deals at most %.0f%% of a vehicle's health (ramCap), one shot at most %.0f%% (shotCap)", g_ramCap * 100.0f, g_shotCap * 100.0f);
			logger::Write("Vehicle hits: native projectile weapon damage, weaponScale=%.3f; damage=%.0f only when native data is unavailable", g_weaponScale, g_hitDamage);
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

		// A reference from its handle; the lookup takes a reference, released at once (the pointer stays valid this frame on the main thread).
		uptr HandleRef(std::uint32_t handle,bool retain=false)
		{
			if (!handle) return 0;
			uptr ref = 0;
			uptr* outp = &ref;
			struct
			{
				uptr vtbl;
				uptr** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &outp };
			__try {
				EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup);
				if (ref&&!retain) EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref);
				return ref;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

        void Release(uptr ref) {
            if(!ref)return;
            __try {EngineFn<void(*)(uptr)>(addr::kRefRelease)(ref);}
            __except(EXCEPTION_EXECUTE_HANDLER){}
        }

		// Pinned 1.16.244 evidence: 141b3a990 stores CalculateWeaponDamage at +214;
		// 141b52190 adds typed weapon damage and applies +204 (charge/pellet/launch
		// multipliers). Its return is XMM0, confirmed in the saved assembly.
        bool CaptureWeaponDamage(std::uint32_t handle,float& damage,std::uint32_t& weapon,float& range)
        {
            uptr projectile=0;
            __try { __try {
                constexpr unsigned char sig[]{0x40,0x53,0x48,0x83,0xEC,0x30,0x8B,0x81,0xF8,0x01,0x00,0x00,0x48,0x8B,0xD9,0x48};
                if(std::memcmp(reinterpret_cast<void*>(game::Base()+0x01B52190),sig,sizeof(sig)))return false;
                projectile=HandleRef(handle,true);const auto source=projectile?At<uptr>(projectile,0x1E0):0;
                if(!source)return false;
                weapon=At<std::uint32_t>(source,off::kFormId);
                damage=EngineFn<float(*)(uptr)>(0x01B52190)(projectile);
                constexpr unsigned char rangeSig[]{0x48,0x89,0x5C,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xEC,0x50,0x33};
                if(!std::memcmp(reinterpret_cast<void*>(game::Base()+0x00D82AD0),rangeSig,sizeof(rangeSig))) {
                    float nativeRange[2]{};
                    // Native range getter handles a null actor context; weapon-instance changes still apply.
                    EngineFn<float*(*)(float*,uptr,uptr)>(0x00D82AD0)(nativeRange,0,projectile+0x1E0);
                    if(std::isfinite(nativeRange[1])&&nativeRange[1]>1&&nativeRange[1]<100000)range=nativeRange[1];
                }
                return weapondamage::Valid(damage);
            } __finally {if(projectile)EngineFn<void(*)(uptr)>(addr::kRefRelease)(projectile);} }
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
        bool CaptureFalloff(std::uint32_t handle,uptr target,float& factor)
        {
            uptr projectile=0;
            __try { __try {
                constexpr unsigned char sig[]{0x48,0x8B,0xC4,0x48,0x89,0x50,0x10,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41};
                if(std::memcmp(reinterpret_cast<void*>(game::Base()+0x01B51E00),sig,sizeof(sig)))return false;
                projectile=HandleRef(handle,true);if(!projectile)return false;
                const float value=EngineFn<float(*)(uptr,uptr)>(0x01B51E00)(projectile,target);
                if(!std::isfinite(value)||value<0||value>1000)return false;
                factor=value;return true;
            } __finally {if(projectile)EngineFn<void(*)(uptr)>(addr::kRefRelease)(projectile);} }
            __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }

		constexpr std::uint32_t kNativeImpact=0x01B47540;
		// Complete 15-byte prologue: mov r11,rsp; three register-to-stack moves; no RIP-relative instructions.
		constexpr std::uint8_t kImpactPrologue[]{0x4C,0x8B,0xDC,0x45,0x88,0x4B,0x20,0x49,0x89,0x53,0x10,0x49,0x89,0x4B,0x08};
		using Impact_t=void(*)(uptr,uptr,uptr,unsigned char,uptr,unsigned char);
		Impact_t g_impactOriginal=nullptr;
		bool ReadNativeImpact(uptr projectile,uptr result,NativeImpact& event) {
			// 141b47540: contact position at result+18h, origin index at +24h.
			// 141b3a990: projectile+170h holds the shooter reference handle.
			__try {
				if(!projectile||!result||!At<uptr>(result,0))return false;
				event.projectile=At<std::uint32_t>(projectile,off::kFormId);
				event.shooterHandle=At<std::uint32_t>(projectile,0x170);
				const auto p=At<game::Vec3>(result,0x18);
				event.point[0]=p.x;event.point[1]=p.y;event.point[2]=p.z;
				const auto origin=At<std::uint32_t>(result,0x24),count=At<std::uint32_t>(game::Base(),0x061FAE80);
				if(origin<count) {
					const auto table=Ptr(0x061FAE88);
					if(!table||count>1000000)return false;
					for(int axis=0;axis<3;++axis)event.point[axis]+=At<float>(table,origin*12+axis*4);
				}
				for(float coordinate:event.point)if(!std::isfinite(coordinate))return false;
				const auto weapon=At<uptr>(projectile,0x1E0);
				if(weapon) {
					event.weapon=At<std::uint32_t>(weapon,off::kFormId);
					constexpr unsigned char sig[]{0x40,0x53,0x48,0x83,0xEC,0x30,0x8B,0x81,0xF8,0x01,0x00,0x00,0x48,0x8B,0xD9,0x48};
					if(!std::memcmp(reinterpret_cast<void*>(game::Base()+0x01B52190),sig,sizeof(sig)))event.damage=EngineFn<float(*)(uptr)>(0x01B52190)(projectile);
				}
				event.at=GetTickCount64();return event.projectile&&event.shooterHandle;
			}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
		}
		void ImpactDetour(uptr projectile,uptr result,uptr a3,unsigned char a4,uptr a5,unsigned char a6) {
			NativeImpact event;
			if(ReadNativeImpact(projectile,result,event)) {
				std::lock_guard lock(g_lock);
				if(g_newImpactCount<256)g_newImpacts[g_newImpactCount++]=event;
			}
			g_impactOriginal(projectile,result,a3,a4,a5,a6);
		}
		std::uint32_t ProjectileId(std::uint32_t handle) {
			__try {const auto ref=HandleRef(handle);return ref?At<std::uint32_t>(ref,off::kFormId):0;}
			__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
		}

		// This is the game's read-only amount calculation, not ApplyDamage:
		// local perk entrypoint FC, then 140d83210's difficulty multiplier. The
		// native damage caller 141905d30 uses it only with the player involved.
		bool ScaleForDifficulty(uptr target, float amount, float& out)
		{
			__try {
				constexpr unsigned char sig[] = { 0x48,0x89,0x5C,0x24,0x18,0x48,0x89,0x74,0x24,0x20,0xC5,0xFA,0x11,0x4C,0x24,0x10 };
				if ((target&&!npcvehicles::LooksLikeActor(target)) || *reinterpret_cast<const std::uint8_t*>(game::Base() + 0x05F468B0) > 6 ||
					std::memcmp(reinterpret_cast<void*>(game::Base() + 0x018DCFC0), sig, sizeof(sig))) return false;
				out = EngineFn<float (*)(uptr, float, bool)>(0x018DCFC0)(target, amount, false);
				return weapondamage::Valid(out);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
			if(!npcvehicles::LooksLikeActor(r))return shooter; // a vehicle shooter is already the chassis, never an Actor argument
			__try {
				const uptr v = EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(r);
				return v ? At<std::uint32_t>(v, off::kFormId) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}
		std::uint32_t ShooterActorId(std::uint32_t shooter) {
			if(const auto driver=npcvehicles::VehicleDriverId(shooter))return driver;
			if(shooter==vehicleflight::OwnRoverId()) {
				const auto player=Ptr(addr::kPlayerCharacterPtr);if(player)return At<std::uint32_t>(player,off::kFormId);
			}
			return shooter;
		}

		float g_playerHealthNow = NAN;

		bool HealthOf(uptr actor,float& now,float& max);
		// Use the same current/maximum values as the native HUD, including an already injured player.
		float PlayerHealth()
		{
			float current=0,maximum=0;
            const bool ok=HealthOf(Ptr(addr::kPlayerCharacterPtr),current,maximum);
            g_playerHealthNow=ok?current:NAN;g_playerHealth=ok?maximum:0;
			return g_playerHealth;
		}

		float Limit() { return g_limitIni > 0 ? g_limitIni : g_vehicleHealth; }

		// An actor's Health now and its maximum, as the HUD's enemy bar computes them (FUN_14148A7F0): ActorValueOwner (+0x70) vtbl[1] GetValue,
		// vtbl[8] GetModifier(2 = damage, info); max = now - damage modifier.
		bool HealthOf(uptr actor, float& now, float& max)
		{
			const uptr av = RefById(kHealthAv);
			if (!actor || !av || !npcvehicles::LooksLikeActor(actor)) return false;
			__try {
				void** vt = *reinterpret_cast<void***>(actor + 0x70);
				now = reinterpret_cast<float (*)(uptr, uptr)>(vt[1])(actor + 0x70, av);
				const float mod = reinterpret_cast<float (*)(uptr, int, uptr)>(vt[8])(actor + 0x70, 2, av);
				max = now - mod;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
			return std::isfinite(now) && std::isfinite(max) && max > 0.0f && max < 1.0e6f;
		}

		constexpr std::uint32_t kActorDamage = 0x01905D30;
		// push rbp; push rsi; push r14; sub rsp,80h; mov rax,[rcx] (15 bytes including the initial REX prefix).
		constexpr std::uint8_t kActorDamagePrologue[]{0x40,0x55,0x56,0x41,0x56,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0x8B,0x01};
		using ActorDamage_t = std::uint64_t (*)(uptr, float, uptr, unsigned char);
		ActorDamage_t g_actorDamageOriginal = nullptr;
		std::atomic<unsigned> g_friendlyDamageBlocked{0};
		std::uint64_t ActorDamageDetour(uptr victim, float amount, uptr attacker, unsigned char reason)
		{
			bool block = false;
			std::uint32_t victimId = 0, attackerId = 0;
			__try {
				if (attacker && combatrelations::BlockFriendlyDamage(amount, victim == attacker, combatrelations::Relation::Ally)) {
					block = npcvehicles::FriendlyActorDamage(victim, attacker);
					if (block) { victimId = At<std::uint32_t>(victim, off::kFormId); attackerId = At<std::uint32_t>(attacker, off::kFormId); }
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { block = false; }
			if (block) {
				if (g_friendlyDamageBlocked.fetch_add(1, std::memory_order_relaxed) < 12)
					logger::Write("Friendly fire: native actor damage blocked (%08X -> %08X, %.1f damage, reason %u)", attackerId, victimId, amount, unsigned(reason));
				return 0; // Native invulnerability returns the same false result (actor-damage.c).
			}
			return g_actorDamageOriginal(victim, amount, attacker, reason);
		}
		// Native actor damage core retains invulnerability/protection and attacker attribution.
        bool DamageHealth(uptr actor,float amount,uptr attacker)
        {
            __try {
                constexpr unsigned char sig[]{0x40,0x55,0x56,0x41,0x56,0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0x8B,0x01,0x45};
                // Pinned executable signature; float amount uses XMM1, actor R8, reason R9B.
                if(!g_actorDamageOriginal && std::memcmp(reinterpret_cast<void*>(game::Base()+kActorDamage),sig,sizeof(sig)))return false;
                // Native damage reason 6 is the path that receives an already
                // scaled amount (141905d30 explicitly skips its difficulty pass).
                EngineFn<std::uint64_t(*)(uptr,float,uptr,unsigned char)>(0x01905D30)(actor,amount,attacker,6);
                return true;
            }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }

		// The driver takes their share of a hit on the vehicle (never below 1 health from us).
		void DriverShare(const world::VehicleInfo& v, float vehicleDamage,std::uint32_t attackerId)
		{
			const std::uint32_t driverId = npcvehicles::VehicleDriverId(v.formId);
			if (!driverId || g_driverShare <= 0.0f) return;
			const uptr driver = RefById(driverId);
			if (!driver || npcvehicles::IsDead(driver)) return;
			float now = 0, max = 0;
			if (!HealthOf(driver, now, max)) return;
			const float want = std::min(vehicleDamage / Limit() * g_driverShare * max, now - 1.0f);
			if (want <= 0.0f) return;
			uptr attacker=RefById(attackerId);
            if(attacker&&!npcvehicles::LooksLikeActor(attacker)){const auto id=npcvehicles::VehicleDriverId(attackerId);attacker=id?RefById(id):0;}
            const bool ok=DamageHealth(driver,want,attacker);
			if (g_driverChecks < 3 || logger::Verbose()) {
				float after = now, amax = max;
				HealthOf(driver, after, amax);
				++g_driverChecks;
				logger::Write("Vehicle hits: driver %08X of %08X takes %.1f: health %.1f -> %.1f of %.1f (%s)", driverId, v.formId, want, now, after, max,
					!ok ? "call FAILED" : after < now - 0.01f ? "applied" : "NO CHANGE: native damage was refused or actor is invulnerable");
			}
		}

		// Research probe (the first hits): does the game keep Health on a vehicle actor, is the vehicle on the HUD enemy list, what is under the crosshair.
		void Probe(const world::VehicleInfo& v)
		{
			if (g_probes >= 6 && !logger::Verbose()) return;
			++g_probes;
			float now = 0, max = 0;
			const bool have = v.ref && npcvehicles::LooksLikeActor(v.ref) && HealthOf(v.ref, now, max);
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			int listed = -1;
			unsigned count = 0;
			std::uint32_t cross = 0;
			if (player) {
				__try {
					count = At<std::uint32_t>(player, kHostileCount);
					const auto* arr = At<const std::uint32_t*>(player, kHostileArray);
					for (unsigned i = 0; arr && i < count && i < 64; ++i) if (HandleRef(arr[i]) == v.ref) listed = int(i);
					const uptr c = HandleRef(*reinterpret_cast<std::uint32_t*>(game::Base() + kCrosshairHandle));
					cross = c ? At<std::uint32_t>(c, off::kFormId) : 0;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					count = 0;
				}
			}
			logger::Write("Vehicle hits: probe %08X: game Health %s %.1f / %.1f; HUD enemy list %u handles, vehicle %s; crosshair target %08X",
				v.formId, have ? "reads" : "unreadable", now, max, count, listed >= 0 ? "listed" : "not listed", cross);
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

		int ShooterSide(const Proj& pr)
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			if (player && pr.shooter == At<std::uint32_t>(player, off::kFormId)) return 1;
			if (pr.shooter == vehicleflight::OwnRoverId() || pr.shooterVeh == vehicleflight::OwnRoverId()) return 1;
			const uptr r = RefById(pr.shooter);
			if (r && npcvehicles::LooksLikeActor(r) && npcvehicles::IsAlly(r)) return 1;
			for (const std::uint32_t id : { pr.shooter, pr.shooterVeh }) {
				const char* p = id ? npcvehicles::ProfileOf(id) : nullptr;
				if (p && p[0] == 'e') return 1;
				if (p && p[0] == 'p') return -1;
			}
			return 0;
		}

		// Damage to a vehicle (a shot, or a ram from combat_tactics' RamDamage): health, the driver's share, the wreck.
		void AddDamage(const world::VehicleInfo& v, float amount, std::uint32_t by, int side, ULONGLONG now, const char* what,std::uint32_t attacker=0)
		{
			if (!features::On(features::Id::VehicleDamage)) return;  // [hits] enabled, also the settings menu
			// Every damage source (shots, rams and driver health share) uses the same friendship gate.
			if (npcvehicles::VehicleFriendlyToShooter(v.ref, by ? by : attacker)) return;
			Damage* d = DamageFor(v.formId, true);
			const float limit = Limit();
			const float dealt = std::min(amount, std::max(0.0f, limit - d->damage));
			d->damage += dealt;
			d->lastHit = now;
			d->lastSide = side;
			d->lastShooter = by;
			++d->hits;
			++g_hitsTotal;
			if (logger::Verbose() || d->hits <= 3) {
				logger::Write("Vehicle hits: %08X hit by %s from %08X (health %.0f / %.0f)", v.formId, what, by, limit - d->damage, limit);
			}
			if (dealt > 0.0f) DriverShare(v,dealt,attacker?attacker:by);
			if (d->damage < limit || d->wrecked) return;
			const bool own = v.formId == vehicleflight::OwnRoverId();
			d->wrecked = true;
			d->wreckRenew = now;
			logger::Write("Vehicle hits: %08X WRECKED (%u hits): autopilot, hover and weapon off until repaired over %.0f%%", v.formId, d->hits, kRepairedShare * 100.0f);
			vehicleflight::DisableAutopilot(v.formId, unsigned(kWreckRenewMs + 5000), "wrecked");
			vcontract::ExitRequest eject;
			eject.mode = vcontract::ExitMode::Eject; eject.waitLanded = false;
			npcvehicles::UnloadVehicle(v.ref, eject, true);
			npcvehicles::WitnessCrash(v.pos[0], v.pos[1], v.pos[2], "a vehicle wrecked");
			if (own) notify::Show("Your vehicle is wrecked: it repairs itself slowly");
		}

		void Hit(const world::VehicleInfo& v, const Proj& pr, ULONGLONG now)
		{
			if(pr.projectileId && !g_projectileHits.Claim(pr.projectileId,v.formId,now,8000))return;
			Probe(v);
			float native = pr.weaponDamage;
			bool available = weapondamage::Valid(native);
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const std::uint32_t playerId = player ? At<std::uint32_t>(player, off::kFormId) : 0;
			const std::uint32_t targetId = npcvehicles::VehicleDriverId(v.formId);
			float factor=pr.falloff;CaptureFalloff(pr.handle,targetId?RefById(targetId):0,factor);
            if(available&&std::isfinite(factor)&&factor>=0)native*=factor;
            const bool playerTarget = playerId && targetId == playerId;
			const bool playerShooter = playerId && pr.attackerId == playerId;
			if (available && (playerTarget || playerShooter)) {
				// 1418dcfc0 explicitly accepts a null actor and selects outgoing
				// difficulty. Empty chassis use that path; no actor vcall on a vehicle.
				const uptr target = playerTarget ? player : targetId ? RefById(targetId) : 0;
				available = ScaleForDifficulty(target, native, native);
			}
			auto hit = weapondamage::Resolve(available, native, g_weaponScale, g_hitDamage);
			hit.damage = std::min(hit.damage, Limit() * g_shotCap);  // [hits] shotCap: one vehicle-gun hit never wrecks a vehicle
			if (g_weaponChecks < 12 || logger::Verbose()) {
				++g_weaponChecks;
				logger::Write("Vehicle hits: weapon %08X, shooter %08X (driver %08X), target driver %08X: launch %.3f, native scaled %.3f, weaponScale %.3f -> %.3f (%s; player difficulty %s)",
					pr.weaponId, pr.shooter, pr.attackerId, targetId, pr.weaponDamage, native, g_weaponScale, hit.damage,
					hit.native ? "native" : "fallback damage=", playerTarget ? "incoming" : playerShooter ? "outgoing" : "not involved");
			}
			AddDamage(v, hit.damage, pr.shooter, pr.side, now, "a shot",pr.attackerId);
		}

		// The last launch time of each recent shooter (hearing gunfire, combat_brain).
		struct Shot { std::uint32_t shooter = 0; ULONGLONG at = 0; };
		Shot g_shots[32];
	}

	void NoteLaunch(std::uint32_t handle, std::uint32_t shooterFormId, const float* origin, const float* dir)
	{
		if (!handle) return;
		// Capture immediately after native Launch completes, while the projectile
		// and its actual weapon instance exist. Queue values only, never pointers.
		float nativeDamage = NAN;
		std::uint32_t weapon = 0; float range=0;
		if (!CaptureWeaponDamage(handle, nativeDamage, weapon, range)) nativeDamage = NAN;
		std::lock_guard lk(g_lock);
		if (g_newCount >= kMax) return;
		{
			Shot* slot = &g_shots[0];
			for (auto& s : g_shots) { if (s.shooter == shooterFormId) { slot = &s; break; } if (s.at < slot->at) slot = &s; }
			slot->shooter = shooterFormId; slot->at = GetTickCount64();
		}
		Proj& p = g_new[g_newCount++];
		p = Proj{};
		p.handle = handle;
		p.projectileId=ProjectileId(handle);
		p.shooter = shooterFormId;
		p.weaponDamage = nativeDamage;
		p.weaponId = weapon; p.weaponRange=range;
		p.at = GetTickCount64();
		if (origin && dir && std::isfinite(origin[0]) && std::isfinite(dir[0])) {
			p.haveLine = true;
			for (int k = 0; k < 3; ++k) { p.o[k] = origin[k]; p.d[k] = dir[k]; }
		}
	}

	namespace { void EnemyBarTick(const world::Snapshot& snap, bool haveVehicles); }  // below, with the enemy bar hook

	void Tick()
	{
		ReadIni();
		const ULONGLONG now = GetTickCount64();
		NativeImpact impacts[256];int impactCount=0;
        if(g_ballistics.size()>256){std::erase_if(g_ballistics,[&](const auto& p){return now-p.second.at>60000;});while(g_ballistics.size()>256){auto oldest=std::min_element(g_ballistics.begin(),g_ballistics.end(),[](const auto&a,const auto&b){return a.second.at<b.second.at;});g_ballistics.erase(oldest);}}
		{
			std::lock_guard lk(g_lock);
			for (int i = 0; i < g_newCount; ++i) {
				// Native projectile references may be reused for a later shot.
				if(g_new[i].projectileId)g_projectileHits.Forget(g_new[i].projectileId);
				if (g_liveCount < kMax) g_live[g_liveCount++] = g_new[i];
				++g_launches;
			}
			g_newCount = 0;
			impactCount=g_newImpactCount;std::copy(g_newImpacts,g_newImpacts+impactCount,impacts);g_newImpactCount=0;
		}
		static world::Snapshot snap;
		const bool haveVehicles = world::Vehicles(snap);
		EnemyBarTick(snap, haveVehicles);
		for(int i=0;haveVehicles&&i<impactCount;++i) {
			const auto& impact=impacts[i];const auto shooter=HandleRef(impact.shooterHandle);
			if(!shooter)continue;
			Proj shot;shot.projectileId=impact.projectile;shot.shooter=At<std::uint32_t>(shooter,off::kFormId);
			shot.shooterVeh=VehicleOf(shot.shooter);shot.attackerId=ShooterActorId(shot.shooter);shot.side=ShooterSide(shot);
			shot.weaponId=impact.weapon;shot.weaponDamage=impact.damage;
			const world::VehicleInfo* struck=nullptr;double nearest=1e30;
			for(unsigned k=0;k<snap.count&&k<64;++k) {
				const auto& v=snap.v[k];if(!interactionlogic::OtherVehicle(v.formId,shot.shooter,shot.shooterVeh))continue;
				float center[3];const double age=now>=impact.at?std::min(double(now-impact.at)/1000.0,0.1):0;
				for(int axis=0;axis<3;++axis)center[axis]=v.center[axis]-float(v.vel[axis]*age);
				double t=0;if(!interactionlogic::HullHit(center,v.rot,v.half,impact.point,impact.point,t))continue;
				const double distance=std::hypot(std::hypot(impact.point[0]-center[0],impact.point[1]-center[1]),impact.point[2]-center[2]);
				if(distance<nearest){nearest=distance;struck=&v;}
			}
			static unsigned logs=0;
			if((struck&&logs<20)||logger::Verbose()) {
				++logs;logger::Write("Vehicle hits: native impact %08X from %08X at (%.1f, %.1f, %.1f): vehicle %08X",impact.projectile,shot.shooter,impact.point[0],impact.point[1],impact.point[2],struck?struck->formId:0);
			}
			if(struck&&!npcvehicles::VehicleFriendlyToShooter(struck->ref,shot.shooter)) {
				// Provocation is independent of health settings and whether a numerical damage amount is available.
				npcvehicles::VehicleAggression(struck->ref,shot.attackerId);Hit(*struck,shot,now);
			}
		}
		for (int i = 0; i < g_liveCount;) {
			Proj& pr = g_live[i];
			if (!pr.resolved) {
				pr.resolved = true;
				pr.shooterVeh = VehicleOf(pr.shooter);
				pr.attackerId = ShooterActorId(pr.shooter);
				pr.side = ShooterSide(pr);
				if (!weapondamage::Valid(pr.weaponDamage) && !CaptureWeaponDamage(pr.handle, pr.weaponDamage, pr.weaponId,pr.weaponRange)) pr.weaponDamage = NAN;
			}
			auto& ballistic=g_ballistics[pr.shooter];
            if(ballistic.weapon!=pr.weaponId){ballistic={};ballistic.weapon=pr.weaponId;}
            if(pr.weaponRange>1){ballistic.data.range=pr.weaponRange;ballistic.data.nativeRange=true;}
            ballistic.at=now;
            CaptureFalloff(pr.handle,0,pr.falloff);
            float cur[3];
            const bool alive = now - pr.at < kProjMaxMs && ProjPos(pr.handle, cur);
			bool done = !alive;
			if (!alive && !pr.seen && pr.haveLine && haveVehicles) {
				const double reach = std::clamp(double(pr.weaponRange > 1 ? pr.weaponRange : 300.0f), 20.0, 600.0);
				float b[3];
				for (int k = 0; k < 3; ++k) b[k] = float(pr.o[k] + pr.d[k] * reach);
				const world::VehicleInfo* struck = nullptr;
				double nearest = 2;
				for (std::uint32_t k = 0; k < snap.count && k < 64; ++k) {
					const auto& v = snap.v[k];
					if (!interactionlogic::OtherVehicle(v.formId, pr.shooter, pr.shooterVeh)) continue;
					double t = 0;
					if (interactionlogic::HullHit(v.center, v.rot, v.half, pr.o, b, t) && t < nearest) { nearest = t; struck = &v; }
				}
				static unsigned lineLogs = 0;
				if (lineLogs < 8 || logger::Verbose()) {
					++lineLogs;
					logger::Write("Vehicle hits: a shot from %08X was gone before its first frame; its launch line (%.0f m) %s%s", pr.shooter, reach,
						struck ? "passes through " : "misses every vehicle", struck ? "a vehicle" : "");
				}
				if (struck && !npcvehicles::VehicleFriendlyToShooter(struck->ref, pr.shooter)) {
					npcvehicles::VehicleAggression(struck->ref, pr.attackerId);
					Hit(*struck, pr, now);
				}
			}
			if ((pr.seen || (alive && pr.haveLine)) && haveVehicles) {
				float b[3];
				if (alive) {
					b[0] = cur[0]; b[1] = cur[1]; b[2] = cur[2];
				}
				else {
					// Gone (it struck something): the last stretch it would have flown, one and a half frames on.
					const double dt = std::clamp((now - pr.seenAt) / 1000.0, 0.016, 0.1) * 1.5;
					for (int k = 0; k < 3; ++k) b[k] = float(pr.p[k] + pr.v[k] * dt);
				}
				const world::VehicleInfo* struck=nullptr;double nearest=2;
				for(std::uint32_t k=0;k<snap.count&&k<64;++k){const auto&v=snap.v[k];
					if(!interactionlogic::OtherVehicle(v.formId,pr.shooter,pr.shooterVeh))continue;
					double t=0;if(interactionlogic::HullHit(v.center,v.rot,v.half,pr.seen?pr.p:pr.o,b,t)&&t<nearest){nearest=t;struck=&v;}
				}
				if(struck){
					if(!npcvehicles::VehicleFriendlyToShooter(struck->ref,pr.shooter)) {
						npcvehicles::VehicleAggression(struck->ref,pr.attackerId);
						Hit(*struck,pr,now);
					}
					else {
						static unsigned friendlyLogs = 0;  // 2026-10-21: shots at an enemy vehicle never counted; is this why?
						if (friendlyLogs < 10 || logger::Verbose()) {
							++friendlyLogs;
							logger::Write("Vehicle hits: a shot from %08X (driver %08X) passed through %08X but counts as friendly fire: no damage", pr.shooter, pr.attackerId, struck->formId);
						}
					}
					done=true;
				}
			}
			if (alive && !done) {
				if (pr.seen && now > pr.seenAt) {
					const double dt = (now - pr.seenAt) / 1000.0;
					for (int k = 0; k < 3; ++k) pr.v[k] = float((cur[k] - pr.p[k]) / dt);
                    float relative[3]{pr.v[0],pr.v[1],pr.v[2]};
                    if(haveVehicles)for(unsigned k=0;k<snap.count&&k<64;++k)if(snap.v[k].formId==pr.shooter||snap.v[k].formId==pr.shooterVeh)for(int axis=0;axis<3;++axis)relative[axis]-=snap.v[k].vel[axis];
                    const double speed=std::hypot(std::hypot(relative[0],relative[1]),relative[2]);
                    if(std::isfinite(speed)&&speed>1&&speed<100000){ballistic.data.speed=ballistic.data.observedSpeed?0.75*ballistic.data.speed+0.25*speed:speed;ballistic.data.observedSpeed=true;}
				}
				pr.p[0] = cur[0]; pr.p[1] = cur[1]; pr.p[2] = cur[2];
				pr.seen = true;
				pr.seenAt = now;
				++pr.frames;
				if (haveVehicles) for (std::uint32_t k = 0; k < snap.count && k < 64; ++k) {
					const auto& v = snap.v[k];
					if (!interactionlogic::OtherVehicle(v.formId, pr.shooter, pr.shooterVeh)) continue;
					const float dd = std::sqrt((cur[0] - v.center[0]) * (cur[0] - v.center[0]) + (cur[1] - v.center[1]) * (cur[1] - v.center[1]) + (cur[2] - v.center[2]) * (cur[2] - v.center[2]));
					if (dd < pr.closest) { pr.closest = dd; pr.closestId = v.formId; }
				}
			}
			if (done) {
				// A shot by the player's side that hit no vehicle: how close it came (2026-10-21: shots at an enemy never counted).
				static unsigned missLogs = 0;
				if (pr.side == 1 && pr.closestId && pr.closest < 40.0f && (missLogs < 15 || logger::Verbose())) {
					++missLogs;
					const world::VehicleInfo* nearVeh = nullptr;
					for (std::uint32_t k = 0; haveVehicles && k < snap.count && k < 64; ++k) if (snap.v[k].formId == pr.closestId) nearVeh = &snap.v[k];
					logger::Write("Vehicle hits: a shot from %08X ended (%u positions read%s): nearest vehicle %08X, %.1f m from its centre (its half size %.1f x %.1f x %.1f)%s",
						pr.shooter, pr.frames, pr.seen ? "" : ", none", pr.closestId, pr.closest, nearVeh ? nearVeh->half[0] : 0.0f, nearVeh ? nearVeh->half[1] : 0.0f,
						nearVeh ? nearVeh->half[2] : 0.0f, alive ? "" : " (the projectile is gone)");
				}
				g_live[i] = g_live[--g_liveCount];
				continue;
			}
			++i;
		}
		// Healing after a quiet spell (a wreck: later and slower); a wreck's autopilot ban is renewed until it is repaired.
		const float limit = Limit();
		for (auto& d : g_dmg) {
			if (!d.id || d.damage <= 0) continue;
			if (d.wrecked && now - d.wreckRenew >= kWreckRenewMs) {
				d.wreckRenew = now;
				vehicleflight::DisableAutopilot(d.id, unsigned(kWreckRenewMs + 5000), "wrecked");
			}
			if (now - d.lastHit < (d.wrecked ? kWreckHealAfterMs : kHealAfterMs)) { d.lastHeal = now; continue; }
			const double dt = d.lastHeal && now > d.lastHeal ? (now - d.lastHeal) / 1000.0 : 0.0;
			d.lastHeal = now;
			d.damage = std::max(0.0f, d.damage - float(limit * (d.wrecked ? kWreckHealShare : kHealShare) * dt));
			if (d.wrecked && d.damage < limit * (1.0f - kRepairedShare)) {
				d.wrecked = false;
				logger::Write("Vehicle hits: %08X repaired (health %.0f / %.0f)", d.id, limit - d.damage, limit);
				vehicleflight::DisableAutopilot(d.id, 1, "repaired");
				if (d.id == vehicleflight::OwnRoverId()) notify::Show("Your vehicle is repaired");
			}
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
			g_newImpactCount=0;
		}
		g_liveCount=0;g_ballistics.clear();g_projectileHits={};g_playerHealth=0;g_playerHealthNow=NAN;
		for (auto& d : g_dmg) d = Damage{};
	}

	bool ActorHealthShare(std::uintptr_t actor, float& share)
	{
		// The native current / maximum pair (as the HUD shows it), not the highest value seen.
		float now = 0, max = 0;
		if (!HealthOf(actor, now, max)) return false;
		share = std::clamp(now / max, 0.0f, 1.0f);
		return true;
	}

	bool PlayerHealthShare(float& share)
	{
		PlayerHealth();
		if (!std::isfinite(g_playerHealthNow) || g_playerHealth <= 0.0f) return false;
		share = std::clamp(g_playerHealthNow / g_playerHealth, 0.0f, 1.0f);
		return true;
	}

	float HealthShare(std::uint32_t vehicleFormId)
	{
		const Damage* d = vehicleFormId ? DamageFor(vehicleFormId, false) : nullptr;
		const float limit = Limit();
		return d && limit > 0 ? std::clamp(1.0f - d->damage / limit, 0.0f, 1.0f) : 1.0f;
	}

	unsigned long long LastHitMs(std::uint32_t vehicleFormId)
	{
		const Damage* d = vehicleFormId ? DamageFor(vehicleFormId, false) : nullptr;
		return d ? d->lastHit : 0;
	}

	bool Wrecked(std::uint32_t vehicleFormId)
	{
		const Damage* d = vehicleFormId ? DamageFor(vehicleFormId, false) : nullptr;
		return d && d->wrecked;
	}

	bool LastHitByYourSide(std::uint32_t& vehicleFormId, unsigned long long& at)
	{
		const Damage* best = nullptr;
		for (const auto& d : g_dmg) if (d.id && d.lastSide == 1 && (!best || d.lastHit > best->lastHit)) best = &d;
		if (!best) return false;
		vehicleFormId = best->id;
		at = best->lastHit;
		return true;
	}

	std::uint32_t CrosshairTarget()
	{
		const uptr c = HandleRef(*reinterpret_cast<std::uint32_t*>(game::Base() + kCrosshairHandle));
		return c ? At<std::uint32_t>(c, off::kFormId) : 0;
	}

	bool ActorHealth(std::uintptr_t actor, float& now, float& max) { return HealthOf(actor, now, max); }

	// ---- The game's enemy bar for a vehicle's driver (2026-10-21, the user: vehicle health as a second bar under the NPC's) ----
	// FUN_141489B90 builds the HUD's enemy list (HudEnemyData) from PlayerCharacter +0xEF8 / +0xF00 and flags the entry that
	// matches the crosshair handle DAT_1461FAE94 (FUN_141489EA0); aiming at a vehicle leaves that handle empty, so no bar.
	// While the view ray passes through a vehicle an enemy drives and the driver is on that list, the driver's handle stands
	// in for the crosshair during the call (restored after it). The vehicle's own bar is hoverui's, under the driver's.
	namespace
	{
		constexpr std::uint32_t kEnemyListBuild = 0x01489B90;
		constexpr std::uint32_t kAimedHoldMs = 400;   // the ray may slip off for a moment
		constexpr double kAimRange = 600.0;
		using Build_t = void (*)(uptr);
		Build_t g_buildOriginal = nullptr;
		std::atomic<std::uint32_t> g_driverHandle{ 0 };
		std::atomic<unsigned> g_overrides{ 0 };
		std::uint32_t g_barVehicle = 0, g_aimVehicle = 0;
		ULONGLONG g_aimAt = 0;

		void BuildDetour(uptr ctx)
		{
			const std::uint32_t h = g_driverHandle.load(std::memory_order_relaxed);
			if (!h) { g_buildOriginal(ctx); return; }
			auto* cross = reinterpret_cast<volatile std::uint32_t*>(game::Base() + kCrosshairHandle);
			const std::uint32_t old = *cross;
			*cross = h;
			g_buildOriginal(ctx);
			if (*cross == h) *cross = old;  // the game wrote its own pick meanwhile: keep that
			g_overrides.fetch_add(1, std::memory_order_relaxed);
		}

		// The view ray: the camera root's translate and forward row (scantarget's CameraRay); the player's eye without a camera.
		bool ViewRay(uptr player, double o[3], double d[3])
		{
			__try {
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				const uptr cam = Ptr(addr::kPlayerCameraPtr);
				const uptr root = cam ? At<uptr>(cam, 0x08) : 0;
				if (!root) return false;
				const double fx = At<float>(root, 0x90), fy = At<float>(root, 0x94), fz = At<float>(root, 0x98);
				const double len = std::sqrt(fx * fx + fy * fy + fz * fz);
				if (!std::isfinite(len) || len < 0.5 || len > 1.5) return false;
				d[0] = fx / len; d[1] = fy / len; d[2] = fz / len;
				const auto ct = At<game::Vec3>(root, 0xB0);
				if (std::isfinite(ct.x) && std::hypot(double(ct.x) - pp.x, double(ct.y) - pp.y) < 20.0 && std::fabs(double(ct.z) - pp.z) < 20.0) {
					o[0] = ct.x; o[1] = ct.y; o[2] = ct.z;
				}
				else {
					o[0] = pp.x; o[1] = pp.y; o[2] = pp.z + 1.6;
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The driver's handle on the HUD enemy list (0: not listed, so the game has no bar for them).
		std::uint32_t ListedHandle(uptr player, uptr driver)
		{
			__try {
				const std::uint32_t count = At<std::uint32_t>(player, kHostileCount);
				const auto* arr = At<const std::uint32_t*>(player, kHostileArray);
				for (std::uint32_t i = 0; arr && i < count && i < 64; ++i) if (arr[i] && HandleRef(arr[i]) == driver) return arr[i];
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return 0;
		}

		std::uint32_t ReadU32(uptr base, std::uint32_t offset)
		{
			__try { return base ? At<std::uint32_t>(base, offset) : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		// Main thread, every frame: which vehicle the view ray passes through, and whether its driver's bar can be shown.
		void EnemyBarTick(const world::Snapshot& snap, bool haveVehicles)
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const ULONGLONG now = GetTickCount64();
			double o[3], d[3];
			std::uint32_t hitId = 0;
			if (player && haveVehicles && ViewRay(player, o, d)) {
				double best = kAimRange;
				const std::uint32_t own = vehicleflight::OwnRoverId();
				for (std::uint32_t k = 0; k < snap.count && k < 64; ++k) {
					const auto& v = snap.v[k];
					if (v.formId == own || v.who == world::VehicleInfo::Who::Player) continue;
					const double c[3] = { v.pos[0] - o[0], v.pos[1] - o[1], v.pos[2] + v.half[2] - o[2] };
					const double t = c[0] * d[0] + c[1] * d[1] + c[2] * d[2];
					if (t < 1.0 || t >= best) continue;
					const double px = c[0] - t * d[0], py = c[1] - t * d[1], pz = c[2] - t * d[2];
					const double r = std::max({ double(v.half[0]), double(v.half[1]), double(v.half[2]) }) + 0.5 + t * 0.005;  // a little looser far away
					if (px * px + py * py + pz * pz < r * r) { best = t; hitId = v.formId; }
				}
			}
			if (hitId) { g_aimVehicle = hitId; g_aimAt = now; }
			else if (now - g_aimAt > kAimedHoldMs) g_aimVehicle = 0;
			std::uint32_t handle = 0, barVehicle = 0;
			if (g_aimVehicle && !Wrecked(g_aimVehicle)) {
				const uptr driver = RefById(npcvehicles::VehicleDriverId(g_aimVehicle));
				if (driver && driver != player && !npcvehicles::IsDead(driver) && npcvehicles::HostileToPlayer(driver)) {
					handle = ListedHandle(player, driver);
					if (handle) barVehicle = g_aimVehicle;
				}
			}
			// The game's own pick (you aim at the driver themselves): the bar under it is that driver's vehicle's.
			if (!barVehicle) {
				const std::uint32_t cross = CrosshairTarget();
				for (std::uint32_t k = 0; cross && haveVehicles && k < snap.count && k < 64; ++k)
					if (snap.v[k].who != world::VehicleInfo::Who::Player && npcvehicles::VehicleDriverId(snap.v[k].formId) == cross) { barVehicle = snap.v[k].formId; break; }
			}
			if (handle != g_driverHandle.load(std::memory_order_relaxed) || barVehicle != g_barVehicle) {
				static unsigned logs = 0;
				if (logs < 12 || logger::Verbose()) {
					++logs;
					logger::Write("Vehicle hits: enemy bar: aimed vehicle %08X, driver handle %08X (%s), bar vehicle %08X; crosshair handle %08X, player +0xEC8 %08X, overrides so far %u",
						g_aimVehicle, handle, handle ? "on the enemy list" : g_aimVehicle ? "driver not on the enemy list" : "none", barVehicle,
						*reinterpret_cast<std::uint32_t*>(game::Base() + kCrosshairHandle), ReadU32(player, 0xEC8), g_overrides.load());
				}
			}
			g_driverHandle.store(g_buildOriginal && features::On(features::Id::HealthBars) ? handle : 0, std::memory_order_relaxed);
			g_barVehicle = barVehicle;
		}
	}

	std::uint32_t EnemyBarVehicle() { return g_barVehicle; }

	void InstallEnemyBar()
	{
		void* impactOriginal=nullptr;
		if(hook::Install(game::Base()+kNativeImpact,kImpactPrologue,sizeof(kImpactPrologue),reinterpret_cast<void*>(&ImpactDetour),&impactOriginal)) {
			g_impactOriginal=reinterpret_cast<Impact_t>(impactOriginal);
			logger::Write("Vehicle hits: native projectile impact hook installed (141B47540); first flight segment tracked");
		}else logger::Write("Vehicle hits: native projectile impact hook FAILED; sampled flight segments remain active");
		void* damageOriginal = nullptr;
		if (hook::Install(game::Base() + kActorDamage, kActorDamagePrologue, sizeof(kActorDamagePrologue), reinterpret_cast<void*>(&ActorDamageDetour), &damageOriginal)) {
			g_actorDamageOriginal = reinterpret_cast<ActorDamage_t>(damageOriginal);
			logger::Write("Friendly fire: native actor damage protection installed; allied direct hits and attributed explosion damage blocked");
		} else {
			logger::Write("Friendly fire: native damage hook FAILED; only mod-applied vehicle damage protected");
		}
		// mov r11, rsp; mov [r11+18h], rbx; push rbp; push rsi; push rdi; push r12; push r13; push r14 (16 bytes, no RIP-relative).
		static const std::uint8_t kPrologue[16] = { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
		void* original = nullptr;
		if (hook::Install(game::Base() + kEnemyListBuild, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&BuildDetour), &original)) {
			g_buildOriginal = reinterpret_cast<Build_t>(original);
			logger::Write("Vehicle hits: enemy bar hook installed (FUN_141489B90): aiming at an enemy vehicle shows its driver's bar");
		}
		else {
			logger::Write("Vehicle hits: enemy bar hook NOT installed (FUN_141489B90 bytes differ); vehicle bars only when you aim at the driver");
		}
	}

	Ballistics BallisticsOf(std::uint32_t shooter) {
        auto it=g_ballistics.find(shooter);
        if(it==g_ballistics.end()) {const auto driver=npcvehicles::VehicleDriverId(shooter);if(driver)it=g_ballistics.find(driver);}
        return it!=g_ballistics.end()&&GetTickCount64()-it->second.at<60000?it->second.data:Ballistics{};
    }
    double ShotTime(std::uint32_t shooter,double distance) {
        const auto b=BallisticsOf(shooter);return b.observedSpeed&&b.speed>1?std::clamp(distance/b.speed,0.0,3.0):0.0;
    }
    float MaxHealth() { ReadIni(); return Limit(); }
	float RamCap() { ReadIni(); return g_ramCap; }

	void ApplyDamage(std::uint32_t vehicleFormId, float amount, std::uint32_t by, int side, const char* what)
	{
		if (!vehicleFormId || amount <= 0.0f) return;
		ReadIni();
		static world::Snapshot snap;
		if (!world::Vehicles(snap)) return;
		for (std::uint32_t k = 0; k < snap.count && k < 64; ++k) {
			if (snap.v[k].formId != vehicleFormId) continue;
			AddDamage(snap.v[k], amount, by, side, GetTickCount64(), what);
			return;
		}
	}

	unsigned long long LastShotBy(std::uint32_t shooterFormId)
	{
		if (!shooterFormId) return 0;
		std::lock_guard lk(g_lock);
		for (const auto& s : g_shots) if (s.shooter == shooterFormId) return s.at;
		return 0;
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
