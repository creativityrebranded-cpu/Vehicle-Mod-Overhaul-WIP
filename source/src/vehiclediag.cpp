#include "trafficpair.h"
#include "trafficpool.h"
#include "vehiclediag.h"
#include "loadedarea.h"
#include "fvolimits.h"
#include "placementenable.h"
#include "hookcost.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "listenerboot.h"
#include "physicssetup.h"
#include "vehicleflight.h"
#include "vehiclehits.h"
#include "vehicleworld.h"
#include "npcvehicles.h"
#include "runtimeforms.h"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vehiclediag
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }

		constexpr std::uint32_t kComponentUpdate = 0x0131D5F0;  // FUN_14131D5F0(component, dt): VehicleRuntimeComponent per-frame update
		constexpr ULONGLONG kEveryMs = 5000;
		constexpr float kNear = 60.0f;    // m
		constexpr unsigned kMaxLines = 8;

		struct Comp { uptr component = 0; unsigned updates = 0; };
		std::mutex g_lock;
		std::unordered_map<std::uint32_t, Comp> g_comps;          // vehicle form id -> its component and update count
		hookcost::KeyedCounter<> g_steps;               // vehicle instance -> physics steps
		std::unordered_map<std::uint32_t, game::Vec3> g_lastPos;  // form id -> position at the last report
		ULONGLONG g_at = 0;
		bool g_hooked = false;

		using Update_t = void (*)(uptr, float);
		Update_t g_original = nullptr;

		using IsStatic_t = bool (*)(uptr);
		IsStatic_t g_isStatic = nullptr;
		bool g_setupHooked = false;
		bool g_attachVerified = false;
		std::atomic<unsigned> g_setupLogs{0};
		struct ReloadEnable {
			uptr ref;
			void CancelRequests() { reinterpret_cast<void (*)(uptr)>(game::Base() + addr::kEnableReference)(ref); }
			bool Disabled() const { return (*reinterpret_cast<const std::uint32_t*>(ref + 0x20) & 0x800) != 0; }
			void Enable() { reinterpret_cast<void (*)(uptr, char)>(game::Base() + addr::kRefEnableTask)(ref, 1); }
		};
		// Guarded engine calls for the 3D reload (no C++ objects in these frames).
		void EnableAgain(std::uint32_t id, uptr ref)
		{
			__try {
				if (reinterpret_cast<uptr (*)(std::uint32_t)>(game::Base() + 0x005DE3B0)(id) == ref) { ReloadEnable engine{ ref }; placementenable::Restore(engine); }
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		bool DisableRef(uptr ref)
		{
			__try { reinterpret_cast<void (*)(uptr, char)>(game::Base() + addr::kDisableReference)(ref, 0); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		struct Retry { uptr component = 0; std::uint32_t body = 0; ULONGLONG since = 0; bool tried = false, reported = false; };
		std::unordered_map<std::uint32_t, Retry> g_retries;  // main thread only; reset on component/body replacement
		// 3D reload (2026-10-29, the user: after loading a save in an already running game "a vehicle might not have chassis, not
		// all but a few or one"; fresh launches never). Log: FE008804, a persistent parked REV-8, kept its old physics record and
		// scene body across the in-session load, so the game made no new VehicleRuntimeComponent for it, queued no attach, and it
		// had no chassis ("updates 0, instance NONE") while traffic and the player used it. A parked vehicle within kNear with 3D
		// whose component the game has not updated for two reports gets its 3D reloaded once per load (disable, then enable
		// 0.6 s later): the game then makes the component and the attach as on a fresh load.
		struct Reload { int strikes = 0; ULONGLONG disabledAt = 0; bool done = false; uptr ref = 0; };
		std::unordered_map<std::uint32_t, Reload> g_reloads;  // main thread only; cleared on a save load

		physicssetup::Chassis InspectChassis(uptr chassis)
		{
			__try {
				const uptr system = At<uptr>(chassis, 0x20);
				const uptr link = system ? At<uptr>(system, 0x20) : 0;
				if (!link) return {};
				physicssetup::Chassis c;
				c.read = At<uptr>(link, 0x20);
				c.write = At<uptr>(link, 0x28);
				if (!c.read || !c.write) return {};
				std::uint32_t id = 0xFFFFFF;
				reinterpret_cast<std::uint32_t* (*)(uptr, std::uint32_t*)>(game::Base() + 0x02376C70)(chassis, &id);
				if ((id & 0xFFFFFF) == 0xFFFFFF) return {};
				const uptr body = reinterpret_cast<uptr (*)(uptr, std::uint32_t)>((*reinterpret_cast<void***>(c.read))[0x70 / 8])(c.read, id);
				if (!body || At<std::uint32_t>(body, 0x50) != id) return {};
				c.id = id;
				c.flags = At<std::uint32_t>(body, 0x54);
				return c;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return {}; }
		}

		// What one native builder call (FUN_14131B730) did, filled on its own thread: whether it reached the static check,
		// what the check said and what our inspection of the chassis found. BuilderDetour logs it with the result.
		struct BuildTrace {
			bool active = false, checked = false, nativeRejected = false, converted = false, finalRejected = false;
			physicssetup::Chassis chassis{};
			std::uintptr_t system = 0, link = 0;
		};
		thread_local BuildTrace t_build;

		bool IsStaticDetour(uptr chassis)
		{
			const uptr caller = reinterpret_cast<uptr>(_ReturnAddress()) - game::Base();
			const bool rejected = g_isStatic(chassis);
			const bool mine = caller == physicssetup::kBuilderReturn && t_build.active;
			if (mine) {
				t_build.checked = true;
				t_build.nativeRejected = rejected;
				t_build.chassis = InspectChassis(chassis);
				__try {
					t_build.system = At<uptr>(chassis, 0x20);
					t_build.link = t_build.system ? At<uptr>(t_build.system, 0x20) : 0;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {}
			}
			const bool result = physicssetup::Reject(caller, rejected,
				[&] { return InspectChassis(chassis); },
				[&](const physicssetup::Chassis& c) {
					// Same write interface/arguments as the native builder's keyframed -> dynamic conversion.
					using Set_t = void (*)(uptr, std::uint32_t, std::uint32_t, int, int, int, int);
					reinterpret_cast<Set_t>((*reinterpret_cast<void***>(c.write))[0xD8 / 8])(c.write, c.id, 2, 0, 0, 0, 0);
					if (mine) t_build.converted = true;
				},
				[&] {
					const bool still = g_isStatic(chassis);
					if (g_setupLogs.fetch_add(1) < 128)
						logger::Write("Vehicle physics repair: static chassis %08X set dynamic for the native setup -> %s", InspectChassis(chassis).id,
							still ? "STILL STATIC, the setup gives up" : "accepted, the setup goes on");
					return still;
				});
			if (mine) t_build.finalRejected = result;
			return result;
		}

		// ---- The game's pending-vehicle list, made unbounded (physicssetup::PendingSet has the why).
		constexpr std::uint32_t kPendingAdd = 0x01313400, kPendingRemove = 0x01313530, kPendingHas = 0x01316140;
		constexpr std::uint32_t kListenerPtr = 0x061EEAD8;  // DAT_1461EEAD8: the VehicleEventListener
		using PendingAdd_t = void (*)(std::uint64_t, std::int64_t*, uptr, std::int64_t);
		using PendingRemove_t = void (*)(std::uint64_t, std::int64_t*, int);
		using PendingHas_t = bool (*)(uptr, int);
		PendingAdd_t g_pendingAdd = nullptr;
		PendingRemove_t g_pendingRemove = nullptr;
		PendingHas_t g_pendingHas = nullptr;
		physicssetup::PendingSet g_pending;
		std::atomic<unsigned> g_pendingLogs{0}, g_rescues{0}, g_wipes{0};

		void PendingAddDetour(std::uint64_t a1, std::int64_t* store, uptr ref, std::int64_t a4)
		{
			std::uint32_t id = 0;
			std::uint64_t before = 0;
			__try {
				id = At<std::uint32_t>(ref, 0x28);
				const uptr listener = *reinterpret_cast<uptr*>(game::Base() + kListenerPtr);
				if (listener) before = At<std::uint64_t>(listener, 0x40);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			g_pending.Add(id);
			if (before > 7) {
				g_wipes.fetch_add(1, std::memory_order_relaxed);
				if (g_pendingLogs.fetch_add(1, std::memory_order_relaxed) < 40)
					logger::Write("Vehicle pending list: the game's list was full (%llu) when %08X arrived and is wiped now; our list keeps all %zu",
						static_cast<unsigned long long>(before), id, g_pending.Size());
			}
			g_pendingAdd(a1, store, ref, a4);
		}

		void PendingRemoveDetour(std::uint64_t a1, std::int64_t* store, int id)
		{
			g_pending.Remove(static_cast<std::uint32_t>(id));
			g_pendingRemove(a1, store, id);
		}

		bool PendingHasDetour(uptr listener, int id)
		{
			bool rescued = false;
			const bool member = g_pending.Member(g_pendingHas(listener, id), static_cast<std::uint32_t>(id), rescued);
			if (rescued) {
				g_rescues.fetch_add(1, std::memory_order_relaxed);
				if (g_pendingLogs.fetch_add(1, std::memory_order_relaxed) < 120)
					logger::Write("Vehicle pending list: %08X kept for its attach (the game's list had lost it)", static_cast<std::uint32_t>(id));
			}
			return member;
		}

		// FUN_14131B730(component): the native physics-vehicle builder, called once by the attach. Logged per call with
		// how it ended, so a vehicle left without an instance names the step that stopped it.
        bool ReadBootMemory(uptr address, void* out, std::size_t size) {
            __try { std::memcpy(out,reinterpret_cast<const void*>(address),size); return true; }
            __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        }
        uptr PublishListener(uptr address, uptr desired) {
            return reinterpret_cast<uptr>(InterlockedCompareExchangePointer(
                reinterpret_cast<void* volatile*>(address),reinterpret_cast<void*>(desired),nullptr));
        }
        bool InstallPendingHooks() {
            static bool attempted=false, installed=false;
            if (attempted) return installed;
            attempted=true;
		static const std::uint8_t kAddPrologue[19] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56 };
		static const std::uint8_t kRemovePrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20 };
		static const std::uint8_t kHasPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20 };
		if (std::memcmp(reinterpret_cast<const void*>(game::Base()+kPendingAdd),kAddPrologue,sizeof(kAddPrologue)) ||
            std::memcmp(reinterpret_cast<const void*>(game::Base()+kPendingRemove),kRemovePrologue,sizeof(kRemovePrologue)) ||
            std::memcmp(reinterpret_cast<const void*>(game::Base()+kPendingHas),kHasPrologue,sizeof(kHasPrologue))) {
            logger::Write("Vehicle pending list: OFF (one or more signatures differ; nothing patched)");
            return false;
        }
		void* add = nullptr; void* remove = nullptr; void* has = nullptr;
		// Signature mismatch changes no code. If a later allocation fails, retain this
		// DLL: an installed query/remover without the adder still gives native answers.
		const bool ok = hook::Install(game::Base() + kPendingHas, kHasPrologue, sizeof(kHasPrologue), reinterpret_cast<void*>(&PendingHasDetour), &has);
		if (ok) g_pendingHas = reinterpret_cast<PendingHas_t>(has);
		const bool okRemove = ok && hook::Install(game::Base() + kPendingRemove, kRemovePrologue, sizeof(kRemovePrologue), reinterpret_cast<void*>(&PendingRemoveDetour), &remove);
		if (okRemove) g_pendingRemove = reinterpret_cast<PendingRemove_t>(remove);
		const bool okAdd = okRemove && hook::Install(game::Base() + kPendingAdd, kAddPrologue, sizeof(kAddPrologue), reinterpret_cast<void*>(&PendingAddDetour), &add);
		if (okAdd) g_pendingAdd = reinterpret_cast<PendingAdd_t>(add);
		logger::Write("Vehicle pending list: %s", okAdd ? "unbounded (the game's 8-entry list is shadowed; every loading vehicle gets its attach)"
			: ok ? "PARTIAL (hook allocation/protection failed): DLL retained; native pending-list answers preserved" : "OFF (hook allocation/protection failed)");
		installed = okAdd;
		return installed || ok;
        }

		constexpr std::uint32_t kBuilder = 0x0131B730;
		using Builder_t = void (*)(uptr);
		Builder_t g_builder = nullptr;
		std::atomic<unsigned> g_buildLogs{0};
		// Vehicles the game has built its physics vehicle for (2026-10-30). A traffic vehicle's birth hold is released only after
		// this: 26 of 54 arrival vehicles were released before their build, their bodies fell 30-36 m to the ground with nothing
		// flying them ("0 steps, body NOT BOUND"), the vehicle was then found on the ground and its driver stayed up at the lane.
		// The other 28 were built while still held (the hold is put back right after the build below) and flew normally.
		std::mutex g_builtLock;
		std::unordered_map<std::uint32_t,ULONGLONG> g_built;
		void NoteBuilt(std::uint32_t id) { std::lock_guard lock(g_builtLock); g_built[id] = GetTickCount64(); }
		// The game's vehicle build puts a vehicle on the ground when there is ground within ~45 m under it (2026-10-30 06:54 run:
		// every lane 19-43 m over the ground dropped by exactly that height at the build, lanes 50 m+ up stayed; the driver stayed
		// at the lane). The move is recorded here and its driver is carried by the same amount on the main thread.
		struct BuildMove { std::uint32_t id; float d[3]; };
		std::vector<BuildMove> g_buildMoves;
		void NoteBuildMove(std::uint32_t id, const float before[3], const float after[3]) {
			const float d[3] = { after[0] - before[0], after[1] - before[1], after[2] - before[2] };
			if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 1.0f) return;
			std::lock_guard lock(g_builtLock); g_buildMoves.push_back({ id, { d[0], d[1], d[2] } });
		}
		bool RefPosOf(std::uint32_t id, float out[3]) {
			const uptr ref = id ? runtimeforms::LookupChecked(id) : 0;
			if (!ref) return false;
			__try { const auto p = At<game::Vec3>(ref, off::kRefPosition); out[0] = p.x; out[1] = p.y; out[2] = p.z; return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// A vehicle still held when it is built goes down onto ground below it around its build (19-45 m, 11:57 run; it sat on its lane until then, frozen or not; only 2 of 24 had
		// sunk before), its driver left at the lane. What in the game does it is not found. It is put back, with room between tries, and a refused try (no player cell, in a load) uses
		// none of them. Measured in the 11:21 run with tries 100 ms apart: 6 stayed, 5 went down again, 9 unclear. If it will not stay, the driver goes to the vehicle.
		// Only logged reading of the reference: it shows the move just after the build returns.
		struct BuildWatch { std::uint32_t id; float before[3]; ULONGLONG at; ULONGLONG triedAt; int tries; };
		std::vector<BuildWatch> g_buildWatch;
        void ApplyBirthStage(std::uint32_t id);
		void WatchBuild(std::uint32_t id, const float before[3]);
		void BuilderDetour(uptr component)
		{
			const BuildTrace saved = t_build;
			t_build = BuildTrace{};
			t_build.active = true;
			std::uint32_t id = 0;
			__try { id = At<std::uint32_t>(component, 0x0C); } __except (EXCEPTION_EXECUTE_HANDLER) {}
			float before[3] = {}, after[3] = {};
			const bool hadPos = RefPosOf(id, before);
			g_builder(component);
			if (hadPos && RefPosOf(id, after)) NoteBuildMove(id, before, after);
            ApplyBirthStage(id);
			if (hadPos) WatchBuild(id, before);
			uptr instance = 0;
			__try { instance = At<uptr>(component, 0x10); } __except (EXCEPTION_EXECUTE_HANDLER) {}
			if (id && instance) NoteBuilt(id);
			const BuildTrace t = t_build;
			t_build = saved;
			if (g_buildLogs.fetch_add(1, std::memory_order_relaxed) >= 300) return;
			if (!t.checked) {
				logger::Write("Vehicle build: %08X -> instance %s; never reached the static check (no ref, 3D or physics object, or no chassis body found in its scene)",
					id, instance ? "yes" : "NONE");
				return;
			}
			const auto& c = t.chassis;
			logger::Write("Vehicle build: %08X -> instance %s; static check said %s; chassis world %s (system %p, link %p, read %p, write %p), body %06X flags %X (%s)%s%s",
				id, instance ? "yes" : "NONE", t.nativeRejected ? "give up" : "go on", !t.link ? "NONE" : !c.read ? "unreadable" : "ok",
				reinterpret_cast<void*>(t.system), reinterpret_cast<void*>(t.link), reinterpret_cast<void*>(c.read), reinterpret_cast<void*>(c.write),
				c.id & 0xFFFFFF, c.flags, (c.flags & 1) ? "static" : (c.flags & 4) ? "keyframed" : (c.flags & 2) ? "dynamic" : "none/invalid",
				t.converted ? "; set dynamic" : "", t.nativeRejected && !t.converted ? "; not converted (no world or not a valid static body)" : "");
		}

		// ---- Who makes a body static (2026-10-21: the Balutar's chassis was static, so the game never built its physics vehicle).
		// FUN_142357D20(physics object, motion type, ...) is the game's motion-type setter (30 callers). Types: 0 static,
		// 1 dynamic, 2 keyframed (corrected 2026-10-30: FUN_1423768B0 applies scene 1 only with positive mass, i.e. dynamic, and
		// maps scene 2 to the Havok keyframed type; this comment used to say the reverse). The vehicle builder turns a
		// keyframed chassis dynamic itself but gives up on a static one, and the load-time freeze in FUN_140B51100 sets
		// type 0 (notes/VEHICLE_NO_INSTANCE_FINDINGS_20261021_claude.md). Freezes (0 and 1) are counted per caller and
		// reported with the 5 s probe; a known vehicle's every change is logged with its caller.
		constexpr std::uint32_t kSetMotion = 0x02357D20;
		using SetMotion_t = std::uint64_t (*)(uptr, std::uint32_t, char, char, std::uint32_t);
		SetMotion_t g_setMotion = nullptr;
		struct Known { std::atomic<uptr> phys{0}; std::atomic<std::uint32_t> id{0}; };
		Known g_known[16];  // physics objects of the vehicles near the player (refreshed by Tick)
		std::atomic<unsigned> g_motionLogs{0};
		struct Freeze { std::atomic<std::uint64_t> key{0}; std::atomic<unsigned> count{0}; };  // key: caller << 8 | motion type
		Freeze g_freezes[24];
		// Extra data 0x39 on the ref (FUN_1402C5C90(ref +0xC8, 0x39)) and flag 4 for its form id in the per-form flag table
		// at *(*DAT_145E6D6E0 +0x2E8) (FUN_141805EF0). In FUN_140B51100 these (with extra 0x88) gate a scene-graph visitor,
		// not the type-0 freeze; that one is spared by ref flag +0x20 & 0x100 or by moving at load. Kept as a probe column.
		// Pure lookups; -1 when they cannot be read.
		constexpr std::uint32_t kHasExtra = 0x002C5C90, kFormFlags = 0x01805EF0, kFormFlagTable = 0x05E6D6E0;
		int Extra39(uptr ref)
		{
			__try { return reinterpret_cast<char (*)(uptr, int)>(game::Base() + kHasExtra)(At<uptr>(ref, 0xC8), 0x39) ? 1 : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		int Flag4(std::uint32_t formId)
		{
			__try {
				const uptr table = *reinterpret_cast<uptr*>(game::Base() + kFormFlagTable);
				if (!table) return -1;
				const uptr flags = At<uptr>(table, 0x2E8);
				return flags ? (reinterpret_cast<bool (*)(uptr, std::uint32_t, std::uint32_t)>(game::Base() + kFormFlags)(flags, formId, 4) ? 1 : 0) : -1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		uptr KnownPhys(std::uint32_t id)
		{
			for (const auto& k : g_known) if (id && k.id.load(std::memory_order_relaxed) == id) return k.phys.load(std::memory_order_relaxed);
			return 0;
		}
		std::uint32_t KnownVehicle(uptr phys)
		{
			for (const auto& k : g_known) if (phys && k.phys.load(std::memory_order_relaxed) == phys) return k.id.load(std::memory_order_relaxed);
			return 0;
		}
        // The scene motion setter's values (FUN_142357D20 -> 142359400 -> 1423768B0, read by Codex 2026-10-30 00:34, notes/
        // traffic-scene-motion-enum-20261030): 1 = dynamic, 2 = keyframed (the lower Havok call underneath uses the reverse). The
        // traffic hold had them swapped: "hold" left a chassis dynamic (it fell, its driver left floating) and "release" froze it.
        constexpr std::uint32_t kSceneDynamic = 1, kSceneKeyframed = 2;
        std::mutex g_birthLock;
        std::unordered_map<std::uint32_t,uptr> g_stagedBirths;
		uptr PhysicsOf(uptr ref);
		std::uint64_t SetMotionDetour(uptr phys, std::uint32_t motion, char a3, char a4, std::uint32_t a5)
		{
            // A held traffic chassis stays keyframed when the game asks to make it dynamic. Only that request is touched, and only
            // after checking the physics object still belongs to the held vehicle (2026-10-30 04:52 crash: holds were matched by
            // address alone; freed and reused objects of other things were forced keyframed and vehicle physics stopped).
            if(motion==kSceneDynamic&&phys){
                std::uint32_t heldId=0;
                {std::lock_guard lock(g_birthLock);for(const auto& entry:g_stagedBirths)if(entry.second==phys){heldId=entry.first;break;}}
                if(heldId){
                    const auto ref=runtimeforms::LookupChecked(heldId);const uptr current=ref?PhysicsOf(ref):0;
                    if(current==phys)motion=kSceneKeyframed;
                    else {std::lock_guard lock(g_birthLock);if(auto it=g_stagedBirths.find(heldId);it!=g_stagedBirths.end()&&it->second==phys)it->second=current;}
                }
            }
			const std::uint32_t vehicle = KnownVehicle(phys);
			const auto caller = reinterpret_cast<uptr>(_ReturnAddress()) - game::Base() + 0x140000000ull;
			if (motion == 0 || motion == kSceneKeyframed) {  // freezes: static or keyframed
				for (auto& f : g_freezes) {
					std::uint64_t key = f.key.load(std::memory_order_relaxed);
					const std::uint64_t want = (std::uint64_t(caller) << 8) | motion;
					if (!key && f.key.compare_exchange_strong(key, want)) key = want;
					if (key == want) { f.count.fetch_add(1, std::memory_order_relaxed); break; }
				}
			}
			if (vehicle && g_motionLogs.fetch_add(1, std::memory_order_relaxed) < 400) {
				logger::Write("Vehicle probe: motion type %u -> physics %p (%s %08X), args %d %d %u, called from %llX", motion, reinterpret_cast<void*>(phys),
					vehicle ? "VEHICLE" : "not a known vehicle", vehicle, int(a3), int(a4), a5, static_cast<unsigned long long>(caller));
			}
			return g_setMotion(phys, motion, a3, a4, a5);
		}
		// The ref's physics object (ref vt +0x560, the call the vehicle exit uses), its reference given back at once.
		uptr PhysicsOf(uptr ref)
		{
			__try {
				uptr out = 0;
				reinterpret_cast<uptr* (*)(uptr, uptr*)>((*reinterpret_cast<void***>(ref))[0x560 / 8])(ref, &out);
				if (out) _InterlockedDecrement(reinterpret_cast<volatile long*>(out + 8));  // the ref keeps its own reference
				return out;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		void WatchBuild(std::uint32_t id, const float before[3]) {
			bool held = false;
			{ std::lock_guard lock(g_birthLock); held = g_stagedBirths.count(id) != 0; }
			if (held) { std::lock_guard lock(g_builtLock); g_buildWatch.push_back({ id, { before[0], before[1], before[2] }, GetTickCount64(), 0, 0 }); }
		}
        void ApplyBirthStage(std::uint32_t id){
            std::lock_guard lock(g_birthLock);auto it=g_stagedBirths.find(id);
            if(it==g_stagedBirths.end()||!g_setMotion)return;
            const auto ref=runtimeforms::LookupChecked(id);const auto physics=ref?PhysicsOf(ref):0;
            if(physics){g_setMotion(physics,kSceneKeyframed,1,0,1);it->second=physics;}
        }
        // A held chassis is frozen the frame its physics object appears or is replaced (2026-10-30, 06:42 run): the hold was only
        // applied at creation (no physics yet), at the game's vehicle build and on motion requests for an object it already
        // knew, so a body made when the model loaded fell free until the build ~10 s later and lay 31-43 m under its lane spot,
        // under its driver. Main thread, once per frame; only entries whose physics object is new are touched.
        void HoldNewPhysics(){
            if(!g_setMotion)return;
            std::lock_guard lock(g_birthLock);
            for(auto& entry:g_stagedBirths){
                const auto ref=runtimeforms::LookupChecked(entry.first);const auto physics=ref?PhysicsOf(ref):0;
                if(!physics||physics==entry.second)continue;
                g_setMotion(physics,kSceneKeyframed,1,0,1);
                if(!entry.second)logger::Write("Traffic birth hold: vehicle %08X frozen the moment its physics appeared",entry.first);
                entry.second=physics;
            }
        }

		std::uint32_t FormIdOf(uptr component)
		{
			__try { return At<std::uint32_t>(component, 0x0C); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		void UpdateDetour(uptr component, float dt)
		{
			if (component) {
				const std::uint32_t id = FormIdOf(component);
				if (id) {
					std::lock_guard lk(g_lock);
					auto& c = g_comps[id];
					c.component = component;
					++c.updates;
				}
			}
			g_original(component, dt);
		}

		struct Read { uptr instance = 0; std::uint32_t body = 0; int player = -1; bool ok = false; };
		Read ReadComponent(uptr component)
		{
			Read r;
			__try {
				r.instance = At<uptr>(component, 0x10);
				r.player = At<std::uint8_t>(component, 0x579);
				r.body = r.instance ? At<std::uint32_t>(r.instance, 0x20) : 0;
				r.ok = true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				r.ok = false;
			}
			return r;
		}

		// Why a deferred attach may not run now; nullptr = it may. The cell must be attached (state 10, kCellState; the
		// first version stopped at >= 9 and so never retried) with its physics data ready (+0x120, its +0x16D), the same
		// gates FUN_141314780 waits for.
		const char* RetryBlocked(const world::VehicleInfo& v, uptr component, int& state)
		{
			state = -1;
			__try {
				if (reinterpret_cast<uptr (*)(std::uint32_t)>(game::Base() + 0x005DE3B0)(v.formId) != v.ref) return "the form id names another reference";
				if (At<std::uint32_t>(component, 0x0C) != v.formId) return "the component belongs to another reference";
				if (At<uptr>(component, 0x10)) return "it has an instance now";
				const uptr cell = At<uptr>(v.ref, off::kRefParentCell);
				if (!cell) return "no parent cell";
				state = At<std::uint8_t>(cell, off::kCellState);
				if (state <= 4 || state > 10) return "cell not attached";
				const uptr data = At<uptr>(cell, 0x120);
				if (!data) return "cell has no physics data (+0x120)";
				if (!At<std::uint8_t>(data, 0x16D)) return "cell physics not ready (+0x120 +0x16D)";
				return nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return "unreadable"; }
		}

		// Launch guard slots (physicssetup::ClampLaunch). The main thread arms a slot for each new instance of a parked
		// vehicle; the physics step (NoteStep, any thread) opens its window at the first step and clamps launches in it.
		// first / lastHit / maxSpeed / closed belong to the stepping thread once armed (the close line reports them).
		struct Guard {
			std::atomic<uptr> instance{0}; std::atomic<unsigned long long> until{0}; std::atomic<std::uint32_t> formId{0}; std::atomic<unsigned> hits{0};
			std::atomic<unsigned long long> first{0}, lastHit{0}; std::atomic<float> maxSpeed{0}; std::atomic<bool> closed{false};
		};
		Guard g_guards[16];
		struct Seen { std::uint32_t formId = 0; unsigned long long at = 0; };
		std::unordered_map<uptr, Seen> g_guardSeen;  // main thread: instances already considered -> form id, last frame present

		void ClampInstance(Guard& g, uptr instance)
		{
			unsigned long long until = g.until.load(std::memory_order_relaxed);
			const unsigned long long now = GetTickCount64();
			const bool first = !until;
			const bool open = physicssetup::GuardOpen(until, now);
			if (first) { g.first.store(now, std::memory_order_relaxed); g.until.store(until, std::memory_order_relaxed); }
			if (!open) {
				if (!g.closed.exchange(true, std::memory_order_relaxed)) {
					const unsigned long long last = g.lastHit.load(std::memory_order_relaxed);
					logger::Write("Vehicle physics repair: launch guard closed for %08X after %.1f s: %u clamps, last %s%.1f s ago, fastest %.1f m/s",
						g.formId.load(std::memory_order_relaxed), double(now - g.first.load(std::memory_order_relaxed)) / 1000.0,
						g.hits.load(std::memory_order_relaxed), last ? "" : "(none) ", last ? double(now - last) / 1000.0 : 0.0,
						g.maxSpeed.load(std::memory_order_relaxed));
				}
				return;
			}
			float before[3] = {}, after[3] = {};
			bool hit = false, spun = false;
			__try {
				// The vehicle instance's chassis, the way vehicleflight::FindBody reads it: read world +0x28, body id +0x20,
				// body vt +0x70, its motion (read vt +0xD0 with the body's motion id +0x80), linear velocity at motion +0x60.
				const uptr read = At<uptr>(instance, 0x28);
				if (!read) return;
				const std::uint32_t id = At<std::uint32_t>(instance, 0x20);
				const uptr body = reinterpret_cast<uptr (*)(uptr, std::uint32_t)>((*reinterpret_cast<void***>(read))[0x70 / 8])(read, id);
				if (!body) return;
				const uptr motion = reinterpret_cast<uptr (*)(uptr, std::uint32_t)>((*reinterpret_cast<void***>(read))[0xD0 / 8])(read, At<std::uint32_t>(body, 0x80));
				if (!motion) return;
				float* v = reinterpret_cast<float*>(motion + 0x60);
				for (int k = 0; k < 3; ++k) before[k] = v[k];
				hit = physicssetup::ClampLaunch(v);
				for (int k = 0; k < 3; ++k) after[k] = v[k];
				if (hit) {
					// The push-out also spins it (FF0141B3 tipped to pitch 101 while its speed was held at 0): stop the spin too,
					// through the body interfaces the step's own boost damper uses (FUN_1423CE0F0, called from FUN_1423CB260,
					// the step this runs in): read world (+0x28) vt +0xA8 gets the angular velocity, write world (+0x30) vt +0xB0
					// sets it.
					const uptr write = At<uptr>(instance, 0x30);
					if (write) {
						alignas(16) float w[4] = {};
						reinterpret_cast<void (*)(uptr, std::uint32_t, float*)>((*reinterpret_cast<void***>(read))[0xA8 / 8])(read, id, w);
						if (std::isfinite(w[0]) && std::isfinite(w[1]) && std::isfinite(w[2]) && w[0] * w[0] + w[1] * w[1] + w[2] * w[2] > 0.0f) {
							alignas(16) float zero[4] = {};
							reinterpret_cast<void (*)(uptr, std::uint32_t, float*, std::uint64_t)>((*reinterpret_cast<void***>(write))[0xB0 / 8])(write, id, zero, 0);
							spun = true;
						}
					}
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return; }
			if (!hit) return;
			const float speed = std::sqrt(before[0] * before[0] + before[1] * before[1] + std::max(before[2], 0.0f) * std::max(before[2], 0.0f));
			if (speed > g.maxSpeed.load(std::memory_order_relaxed)) g.maxSpeed.store(speed, std::memory_order_relaxed);
			g.lastHit.store(now, std::memory_order_relaxed);
			physicssetup::GuardHit(g.first.load(std::memory_order_relaxed), until, now);
			g.until.store(until, std::memory_order_relaxed);
			if (g.hits.fetch_add(1, std::memory_order_relaxed) < 3)
				logger::Write("Vehicle physics repair: launch guard caught %08X at %.1f m/s sideways, %.1f m/s up -> %.1f, %.1f%s (%.1f s left)",
					g.formId.load(std::memory_order_relaxed), std::hypot(before[0], before[1]), before[2], std::hypot(after[0], after[1]), after[2],
					spun ? ", spin stopped" : "", double(until - now) / 1000.0);
		}

		// Wreck glide slots (physicssetup::Glide). Armed by GuardTick when a vehicle becomes wrecked; applied in the
		// physics step for kGlideMs. floorVz / last belong to the stepping thread of that instance.
		struct GlideSlot { std::atomic<uptr> instance{0}; std::atomic<std::uint32_t> formId{0}; std::atomic<unsigned long long> until{0}; float floorVz = 0; LONGLONG last = 0; std::atomic<bool> logged{false}; };
		GlideSlot g_glides[8];
		std::unordered_set<std::uint32_t> g_wreckSeen;  // main thread: wrecked vehicles already given their glide

		void GlideInstance(GlideSlot& g, uptr instance)
		{
			if (GetTickCount64() >= g.until.load(std::memory_order_relaxed)) { g.instance.store(0); return; }
			LARGE_INTEGER qpc, freq;
			QueryPerformanceCounter(&qpc);
			QueryPerformanceFrequency(&freq);
			const float dt = g.last ? float(double(qpc.QuadPart - g.last) / double(freq.QuadPart)) : 1.0f / 60.0f;
			g.last = qpc.QuadPart;
			__try {
				const uptr read = At<uptr>(instance, 0x28);
				if (!read) return;
				const uptr body = reinterpret_cast<uptr (*)(uptr, std::uint32_t)>((*reinterpret_cast<void***>(read))[0x70 / 8])(read, At<std::uint32_t>(instance, 0x20));
				if (!body) return;
				const uptr motion = reinterpret_cast<uptr (*)(uptr, std::uint32_t)>((*reinterpret_cast<void***>(read))[0xD0 / 8])(read, At<std::uint32_t>(body, 0x80));
				if (!motion) return;
				physicssetup::Glide(reinterpret_cast<float*>(motion + 0x60), g.floorVz, dt);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return; }
			if (!g.logged.exchange(true))
				logger::Write("Vehicle wreck glide: %08X sinks for %.1f s (fall builds at %.0f m/s^2, sideways speed bleeds off)", g.formId.load(),
					physicssetup::kGlideMs / 1000.0, physicssetup::kGlideAccel);
		}

		void ArmGlide(std::uint32_t formId, uptr instance, float vz)
		{
			for (auto& g : g_glides) {
				if (g.instance.load(std::memory_order_relaxed) && GetTickCount64() < g.until.load(std::memory_order_relaxed)) continue;
				g.formId.store(formId);
				g.floorVz = std::min(vz, 0.0f);
				g.last = 0;
				g.logged.store(false);
				g.until.store(GetTickCount64() + physicssetup::kGlideMs);
				g.instance.store(instance, std::memory_order_release);
				return;
			}
		}

		// Main thread, every frame: arm a guard for each instance that appears on a parked vehicle; disarm when someone
		// takes the vehicle or the instance goes away.
		void GuardTick()
		{
			static world::Snapshot snap;
			if (!world::Vehicles(snap)) return;
			std::vector<std::pair<std::uint32_t, uptr>> comps;
			{
				std::lock_guard lk(g_lock);
				comps.reserve(g_comps.size());
				for (const auto& [id, c] : g_comps) if (c.component) comps.emplace_back(id, c.component);
			}
			const unsigned long long now = GetTickCount64();
			for (std::uint32_t i = 0; i < snap.count && i < world::kMaxVehicles; ++i) {
				const auto& v = snap.v[i];
				const auto it = std::find_if(comps.begin(), comps.end(), [&](const auto& c) { return c.first == v.formId; });
				if (it == comps.end()) continue;
				const Read r = ReadComponent(it->second);
				if (!r.ok || !r.instance) continue;
				if (vehiclehits::Wrecked(v.formId)) { if (g_wreckSeen.insert(v.formId).second) ArmGlide(v.formId, r.instance, v.vel[2]); }
				else g_wreckSeen.erase(v.formId);
				// Our own autopilot flying the empty rover (summon, cover) is driving it too: the guard fought it for 30 s in the
				// 23:38 run ("bumped", "cramped", no progress), since an undriven vehicle counts as parked.
				const bool autopilot = (v.formId == vehicleflight::OwnRoverId() && vehicleflight::Active());
				const bool parked = (v.who == world::VehicleInfo::Who::Parked || v.who == world::VehicleInfo::Who::None) && !autopilot;
				if (!parked || v.driverId || vehiclehits::Wrecked(v.formId))
					for (auto& g : g_guards)
						if (g.instance.load(std::memory_order_relaxed) == r.instance) g.instance.store(0);
				auto [seen, fresh] = g_guardSeen.try_emplace(r.instance, Seen{ v.formId, now });
				seen->second.at = now;
				if (!fresh && seen->second.formId != v.formId) { seen->second.formId = v.formId; fresh = true; }  // the address now holds another vehicle's instance
				if (!fresh || !parked || v.driverId || vehiclehits::Wrecked(v.formId)) continue;
				for (auto& g : g_guards)
					if (g.instance.load(std::memory_order_relaxed) == r.instance) g.instance.store(0);  // NoteStep uses the first match: no stale slot for it
				for (auto& g : g_guards) {
					const uptr cur = g.instance.load(std::memory_order_relaxed);
					const unsigned long long until = g.until.load(std::memory_order_relaxed);
					if (cur && (!until || now < until)) continue;
					g.formId.store(v.formId, std::memory_order_relaxed);
					g.hits.store(0, std::memory_order_relaxed);
					g.first.store(0, std::memory_order_relaxed);
					g.lastHit.store(0, std::memory_order_relaxed);
					g.maxSpeed.store(0, std::memory_order_relaxed);
					g.closed.store(false, std::memory_order_relaxed);
					g.until.store(0, std::memory_order_relaxed);
					g.instance.store(r.instance, std::memory_order_release);
					break;
				}
			}
			// A frame or two without the vehicle (a cell crossing) keeps its guard: only an instance missing kGuardMissMs is gone.
			for (auto it = g_guardSeen.begin(); it != g_guardSeen.end();) it = physicssetup::GuardGone(it->second.at, now) ? g_guardSeen.erase(it) : std::next(it);
			for (auto& g : g_guards) {
				const uptr cur = g.instance.load(std::memory_order_relaxed);
				if (cur && !g_guardSeen.count(cur)) g.instance.store(0);
			}
		}
	}

    bool PrepareForDataLoad() {
        const auto result=listenerboot::Prepare(game::Base(),ReadBootMemory,PublishListener);
        logger::Write("Vehicle listener before data load: %s", result==listenerboot::Result::Published
            ? "published the native static object (normal event initialization retained)"
            : result==listenerboot::Result::Existing ? "already present, preserved" : "REFUSED (native initialization signatures differ)");
        return result!=listenerboot::Result::Invalid && InstallPendingHooks();
    }

	void Install()
	{
		// Validate both the predicate entry and its exact vehicle-builder call site.
		// Other users of IsStatic retain the real answer without any motion changes.
		static const std::uint8_t kStaticPrologue[15] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30 };
		static const std::uint8_t kBuilderCall[] = { 0xE8, 0x84, 0xDD, 0x05, 0x01, 0x84, 0xC0, 0x0F, 0x85, 0x21, 0x06, 0x00, 0x00 };
		static const std::uint8_t kAttachPrologue[] = { 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
		void* predicate = nullptr;
		if (std::memcmp(reinterpret_cast<const void*>(game::Base() + 0x0131B857), kBuilderCall, sizeof(kBuilderCall)) == 0) {
			g_setupHooked = hook::Install(game::Base() + 0x023795E0, kStaticPrologue, sizeof(kStaticPrologue), reinterpret_cast<void*>(&IsStaticDetour), &predicate);
			if (g_setupHooked) g_isStatic = reinterpret_cast<IsStatic_t>(predicate);
		}
		InstallPendingHooks();
		static const std::uint8_t kBuilderPrologue[17] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
		void* builder = nullptr;
		const bool bh = hook::Install(game::Base() + kBuilder, kBuilderPrologue, sizeof(kBuilderPrologue), reinterpret_cast<void*>(&BuilderDetour), &builder);
		if (bh) g_builder = reinterpret_cast<Builder_t>(builder);
		logger::Write("Vehicle build trace: %s", bh ? "installed (every native physics-vehicle build is logged with how it ended)" : "FAILED (bytes differ)");
		g_attachVerified = std::memcmp(reinterpret_cast<const void*>(game::Base() + 0x01314780), kAttachPrologue, sizeof(kAttachPrologue)) == 0;
		logger::Write("Vehicle physics repair: static-chassis setup %s; deferred full attach %s", g_setupHooked ? "installed" : "OFF (signature mismatch)",
			g_setupHooked && g_attachVerified ? "available" : "OFF");
		static const std::uint8_t kPrologue[14] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55 };
		void* original = nullptr;
		g_hooked = hook::Install(game::Base() + kComponentUpdate, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&UpdateDetour), &original);
		if (g_hooked) g_original = reinterpret_cast<Update_t>(original);
		logger::Write("Vehicle probe: component update hook %s", g_hooked ? "installed (counting only)" : "FAILED (bytes differ); update counts unavailable");
		static const std::uint8_t kMotionPrologue[15] = { 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x70 };
		void* motion = nullptr;
		const bool mh = hook::Install(game::Base() + kSetMotion, kMotionPrologue, sizeof(kMotionPrologue), reinterpret_cast<void*>(&SetMotionDetour), &motion);
		if (mh) g_setMotion = reinterpret_cast<SetMotion_t>(motion);
		logger::Write("Vehicle launch guard: parked hold rollback active (velocity clamp only; no parked freeze)");
		logger::Write("Vehicle probe: motion-type hook %s", mh ? "installed (static / keyframed counts per caller every 5 s; every nearby vehicle change with its caller)" : "FAILED (bytes differ)");
	}

	void NoteStep(std::uintptr_t instance)
	{
		if (!instance) return;
		for (auto& g : g_guards)
			if (g.instance.load(std::memory_order_acquire) == instance) { ClampInstance(g, instance); break; }
		for (auto& g : g_glides)
			if (g.instance.load(std::memory_order_acquire) == instance) { GlideInstance(g, instance); break; }
        // No shared registry mutex and no allocation in the physics counter.
        g_steps.Note(instance);
	}

	std::uintptr_t ComponentOf(std::uint32_t formId)
	{
		std::lock_guard lk(g_lock);
		const auto it = g_comps.find(formId);
		return it == g_comps.end() ? 0 : it->second.component;
	}

	// Read-only evidence for active jobs, including those omitted from the nearby snapshot.
	// Parent cells can be persistent; positional grid coverage is measured separately.
	struct RangeView {
		game::Vec3 pos{};
		bool valid=false, loaded=false;
		int parentState=-1, positionState=-1;
		unsigned gridEntries=0;
	};
	RangeView ReadRange(uptr ref) {
		RangeView r;
		__try {
			if (!ref) return r;
			r.pos=At<game::Vec3>(ref,off::kRefPosition);
			if (!std::isfinite(r.pos.x)||!std::isfinite(r.pos.y)||!std::isfinite(r.pos.z)) return r;
			const uptr loaded=At<uptr>(ref,off::kRefLoadedData);
			r.loaded=loaded && At<uptr>(loaded,off::kLoadedData3D);
			const uptr parent=At<uptr>(ref,off::kRefParentCell);
			if(parent) r.parentState=At<std::uint8_t>(parent,off::kCellState);
			const uptr tes=*reinterpret_cast<uptr*>(game::Base()+addr::kTESPtr);
			if(tes) {
				r.gridEntries=At<unsigned>(tes,off::kTESGridCount);
				const auto entries=At<uptr*>(tes,off::kTESGridCells);
				// Bounded diagnostic only. Do not claim coverage for entries not inspected.
				for(unsigned i=0;entries && i<std::min(r.gridEntries,4096u);++i) {
					const uptr entry=entries[i], cell=entry?At<uptr>(entry,off::kGridEntryCell):0;
					if(!cell || (At<std::uint8_t>(cell,0x48)&1) || (At<unsigned>(cell,off::kCellFormFlags)&0x400)) continue;
					const uptr ext=At<uptr>(cell,off::kCellExterior);
					if(ext && double(At<int>(ext,0))==std::floor(r.pos.x/100.0) &&
						double(At<int>(ext,4))==std::floor(r.pos.y/100.0)) {
						r.positionState=At<std::uint8_t>(cell,off::kCellState); break;
					}
				}
			}
			r.valid=true;
		} __except(EXCEPTION_EXECUTE_HANDLER) { r.valid=false; }
		return r;
	}

    bool BirthStageAvailable(){return g_setMotion&&g_builder;}
    int StagedLoaded(){std::lock_guard lock(g_birthLock);int n=0;for(const auto& entry:g_stagedBirths)if(entry.second)++n;return n;}
    void StageBirth(std::uint32_t id){
        if(!id)return;{std::lock_guard lock(g_birthLock);g_stagedBirths.emplace(id,0);}
        ApplyBirthStage(id);
    }
    // A hidden, replaced or deleted traffic vehicle leaves the hold list at once, motion untouched (its physics object can be
    // freed and reused by something else).
    void DropBirthHold(std::uint32_t id){if(!id)return;std::lock_guard lock(g_birthLock);g_stagedBirths.erase(id);}
    static bool HasInstance(uptr component){if(!component)return false;__try{return At<uptr>(component,0x10)!=0;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
    // Built since the last reset, or (after a reset, for a vehicle that is not built again) its live component has an instance.
    bool Built(std::uint32_t id){
        {std::lock_guard lock(g_builtLock);if(g_built.count(id))return true;}
        return HasInstance(ComponentOf(id));
    }
    bool ReleaseBirth(std::uint32_t id){
        if(!vehicleflight::BirthControlReady(id))return false;
        if(!Built(id)){
            static std::unordered_map<std::uint32_t,ULONGLONG> waitLogged;const ULONGLONG now=GetTickCount64();
            auto& at=waitLogged[id];
            if(!at){at=now;logger::Write("Traffic birth hold: vehicle %08X stays frozen until the game builds its physics vehicle (released after the build)",id);}
            return false;
        }
        std::lock_guard lock(g_birthLock);auto it=g_stagedBirths.find(id);
        if(it==g_stagedBirths.end())return true;
        const auto ref=runtimeforms::LookupChecked(id);const auto physics=ref?PhysicsOf(ref):0;
        if(!physics||!g_setMotion)return false;
        g_setMotion(physics,kSceneDynamic,1,0,1);g_stagedBirths.erase(it);return true;
    }
	void Reset()
	{
		std::lock_guard lk(g_lock);
		g_comps.clear();
		g_retries.clear();
		g_reloads.clear();
		g_lastPos.clear();
        {std::lock_guard lock(g_birthLock);g_stagedBirths.clear();}
        {std::lock_guard lock(g_builtLock);g_built.clear();g_buildWatch.clear();}
	}

	static float PlayerDistance(const float at[3])
	{
		__try {
			const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
			if (!player) return -1;
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			return std::hypot(p.x - at[0], p.y - at[1]);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}
	// Main thread, once per frame. The move runs outside the lock: it can bring the game back into the build.
	static void PutBackBuildDrops()
	{
		std::vector<BuildWatch> watched;
		{ std::lock_guard lock(g_builtLock); if (g_buildWatch.empty()) return; watched.swap(g_buildWatch); }
		const ULONGLONG now = GetTickCount64();
		std::vector<BuildWatch> keep;
		for (auto& w : watched) {
			float at[3] = {};
			bool held = false;
			{ std::lock_guard lock(g_birthLock); held = g_stagedBirths.count(w.id) != 0; }
			if (!held || !RefPosOf(w.id, at)) continue;  // hidden, released or gone
			if (now - w.at >= trafficpair::kPutBackWatchMs) {
				if (w.tries) logger::Write("Vehicle build: %08X after %d put-back(s): z %.1f, wanted %.1f: %s", w.id, w.tries, at[2], w.before[2], w.before[2] - at[2] < 2.0f ? "it stayed" : "it did not stay");
				continue;
			}
			if (now - w.triedAt >= trafficpair::kPutBackEveryMs && trafficpair::PutBackAfterBuild(w.before[2], at[2], w.tries)) {
				w.triedAt = now;
				if (trafficpool::PutBack(w.id, w.before[0], w.before[1], w.before[2])) {
					if (!w.tries) logger::Write("Vehicle build: %08X went down %.1f m around its build (%.0f, %.0f, z %.1f to %.1f), %.0f m from the player: putting it back", w.id, w.before[2] - at[2], w.before[0], w.before[1], w.before[2], at[2], PlayerDistance(at));
					++w.tries;
				}
			}
			keep.push_back(w);
		}
		std::lock_guard lock(g_builtLock);
		g_buildWatch.insert(g_buildWatch.end(), keep.begin(), keep.end());
	}
	void CarryBuildMoves()
	{
		std::vector<BuildMove> moves;
		{ std::lock_guard lock(g_builtLock); moves.swap(g_buildMoves); }
		for (const auto& m : moves) {
			const bool carried = loadedarea::CarryDriverOf(m.id, m.d[0], m.d[1], m.d[2]);
			logger::Write("Vehicle build: %08X was moved %.1f m by the game's build (%.1f, %.1f, %.1f); its seated driver %s",
				m.id, std::sqrt(m.d[0] * m.d[0] + m.d[1] * m.d[1] + m.d[2] * m.d[2]), m.d[0], m.d[1], m.d[2], carried ? "moved with it" : "not moved (no FVO driver seated in it)");
		}
	}
	void Tick()
	{
		GuardTick();
		HoldNewPhysics();
		PutBackBuildDrops();
		CarryBuildMoves();
		const ULONGLONG now = GetTickCount64();
		// The second half of a 3D reload: enable what was disabled 0.6 s ago (placementenable::Restore, as placements do).
		for (auto& [id, rl] : g_reloads) {
			if (!rl.disabledAt || now - rl.disabledAt < 600) continue;
			rl.disabledAt = 0;
			EnableAgain(id, rl.ref);
			logger::Write("Vehicle physics repair: %08X enabled again after its 3D reload", id);
		}
		if (now - g_at < kEveryMs) return;
		const bool first = !g_at;
		g_at = now;
		if (first) return;  // the first window starts now
		const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
		static thread_local world::Snapshot snap;
		if (!player) return;
		if (!world::Vehicles(snap)) snap={}; // active-job evidence must survive an empty scan
		const auto pp = At<game::Vec3>(player, off::kRefPosition);
		std::vector<const world::VehicleInfo*> nearby;
		for (std::uint32_t i = 0; i < snap.count && i < world::kMaxVehicles; ++i) {
			const auto& v = snap.v[i];
			if (std::hypot(v.pos[0] - pp.x, v.pos[1] - pp.y) < kNear) nearby.push_back(&v);
		}
		std::sort(nearby.begin(), nearby.end(), [&](const auto* a, const auto* b) {
			return std::hypot(a->pos[0] - pp.x, a->pos[1] - pp.y) < std::hypot(b->pos[0] - pp.x, b->pos[1] - pp.y);
		});
		const std::uint8_t freeze = At<std::uint8_t>(game::Base(), addr::kFreezeVehicleOnExit);
		std::unordered_map<std::uint32_t, Comp> comps;
		std::unordered_map<uptr, unsigned> steps;
		{
			std::lock_guard lk(g_lock);
			comps.swap(g_comps);
			for (const auto& [id, c] : comps) g_comps[id] = Comp{ c.component, 0 };  // keep the component, restart the count
		}
        const auto skippedSteps=g_steps.Drain([&](uptr instance,unsigned count){steps[instance]=count;});
		npcvehicles::JobInfo jobs[fvolimits::kVehicles]{};
		const int jobCount=npcvehicles::Jobs(jobs,fvolimits::kVehicles);
		for(int j=0;j<jobCount;++j) {
			const auto& job=jobs[j];
			const uptr vehicle=runtimeforms::Lookup(job.vehicle), actor=runtimeforms::Lookup(job.npc);
			const auto vr=ReadRange(vehicle), ar=ReadRange(actor);
			const auto comp=comps.find(job.vehicle);
			const Read r=comp!=comps.end() && comp->second.component?ReadComponent(comp->second.component):Read{};
			const auto step=steps.find(r.instance);
			const int pilot=npcvehicles::PilotFor(job.vehicle);
			const auto physics=vehicleflight::PilotPhysics(pilot);
			bool inSnapshot=false; unsigned sceneBodies=0;
			for(unsigned i=0;i<snap.count && i<world::kMaxVehicles;++i) if(snap.v[i].formId==job.vehicle) {
				inSnapshot=true; sceneBodies=snap.v[i].bodyCount; break;
			}
			if (logger::Verbose()) logger::Write("Traffic range: %08X pilot %d, distance %.1f m, height %.1f; vehicle ref %d 3D %d, driver ref %d 3D %d seated %d; grid entries %u position cell %d parent cell %d; snapshot %d scene bodies %u; native updates %u instance %d steps %u; FVO active %d bound %d live %d serial %llu",
				job.vehicle,pilot,vr.valid?std::hypot(vr.pos.x-pp.x,vr.pos.y-pp.y):-1.0,vr.valid?double(vr.pos.z):0.0,
				vr.valid?1:0,vr.loaded?1:0,ar.valid?1:0,ar.loaded?1:0,actor&&vehicle&&npcvehicles::VehicleOf(actor)==vehicle?1:0,
				vr.gridEntries,vr.positionState,vr.parentState,inSnapshot?1:0,sceneBodies,comp!=comps.end()?comp->second.updates:0u,r.ok&&r.instance?1:0,
				step!=steps.end()?step->second:0u,vehicleflight::PilotActive(pilot)?1:0,physics.bound?1:0,physics.live?1:0,
				static_cast<unsigned long long>(physics.serial));
		}
		logger::Write("Vehicle probe: %zu vehicles within %.0f m; component updates hook %s; freeze on exit %u; %zu components updated, %zu instances stepped in %.0f s",
			nearby.size(), kNear, g_hooked ? "on" : "OFF", freeze, comps.size(), steps.size(), kEveryMs / 1000.0);
        if(skippedSteps)
            logger::Write("Vehicle probe: %u step-count samples skipped (bounded diagnostics table full)",skippedSteps);
		if (g_pendingAdd)
			logger::Write("Vehicle pending list:   %zu waiting for their 3D; %u kept that the game's list had lost, %u wipes of the game's list so far",
				g_pending.Size(), g_rescues.load(std::memory_order_relaxed), g_wipes.load(std::memory_order_relaxed));
		for (auto& f : g_freezes) {
			const std::uint64_t key = f.key.load(std::memory_order_relaxed);
			const unsigned n = f.count.exchange(0, std::memory_order_relaxed);
			if (key && n)
				logger::Write("Vehicle probe:   motion type %u (%s) set %u times by %llX", unsigned(key & 0xFF), (key & 0xFF) == kSceneKeyframed ? "keyframed" : (key & 0xFF) == kSceneDynamic ? "dynamic" : "static", n,
					static_cast<unsigned long long>(key >> 8));
		}
		{
			unsigned k = 0;
			for (const auto* v : nearby) {
				if (k >= std::size(g_known)) break;
				g_known[k].phys.store(v->ref ? PhysicsOf(v->ref) : 0, std::memory_order_relaxed);
				g_known[k].id.store(v->formId, std::memory_order_relaxed);
				++k;
			}
			for (; k < std::size(g_known); ++k) { g_known[k].phys.store(0); g_known[k].id.store(0); }
		}
		unsigned lines = 0;
		for (const auto* v : nearby) {
			if (lines++ >= kMaxLines) break;
			const auto it = comps.find(v->formId);
			const char* who = v->who == world::VehicleInfo::Who::Player ? "yours/driven" : v->who == world::VehicleInfo::Who::Npc ? "npc" : "parked";
			const auto last = g_lastPos.find(v->formId);
			const double moved = last == g_lastPos.end() ? -1.0 : std::hypot(std::hypot(double(v->pos[0]) - last->second.x, double(v->pos[1]) - last->second.y), double(v->pos[2]) - last->second.z);
			g_lastPos[v->formId] = game::Vec3{ v->pos[0], v->pos[1], v->pos[2] };
			if (it == comps.end() || !it->second.component) {
				logger::Write("Vehicle probe:   %08X (base %08X, %s, %.0f m): NO component update in %.0f s; moved %.1f m; scene bodies %u (first %08X); physics %p", v->formId, v->baseId, who,
					std::hypot(v->pos[0] - pp.x, v->pos[1] - pp.y), kEveryMs / 1000.0, moved, v->bodyCount, v->bodyCount ? v->bodyIds[0] : 0, reinterpret_cast<void*>(KnownPhys(v->formId)));
				continue;
			}
			const Read r = ReadComponent(it->second.component);
			const auto s = r.instance ? steps.find(r.instance) : steps.end();
			logger::Write("Vehicle probe:   %08X (base %08X, %s, %.0f m): updates %u, instance %s (body %08X, scene %08X), steps %u, player flag %d; moved %.1f m; physics %p; load exceptions: extra39 %d, flag4 %d",
				v->formId, v->baseId, who, std::hypot(v->pos[0] - pp.x, v->pos[1] - pp.y), it->second.updates, !r.ok ? "UNREADABLE" : r.instance ? "yes" : "NONE",
				r.body, v->bodyCount ? v->bodyIds[0] : 0, s == steps.end() ? 0u : s->second, r.player, moved, reinterpret_cast<void*>(KnownPhys(v->formId)),
				v->ref ? Extra39(v->ref) : -1, Flag4(v->formId));
		}
		// The initial attach can run before the chassis has a physics world. Retry
		// through the full native path, not BuildInstance alone (which would miss
		// reference registration and reorient actions). One request per component/body
		// generation, at most one vehicle per report, after a settling window.
		if (g_setupHooked && g_attachVerified) {
			for (const auto* v : nearby) {
				const auto it = comps.find(v->formId);
				if (!v->ref || !v->bodyCount || it == comps.end() || !it->second.updates) {
					g_retries.erase(v->formId); continue;
				}
				const auto component = it->second.component;
				const Read r = ReadComponent(component);
				if (!r.ok || r.instance) { g_retries.erase(v->formId); continue; }
				auto& retry = g_retries[v->formId];
				if (retry.component != component || retry.body != v->bodyIds[0])
					retry = Retry{ component, v->bodyIds[0], now, false };
				if (retry.tried || now - retry.since < 2000) continue;
				int state = -1;
				if (const char* why = RetryBlocked(*v, component, state)) {
					if (!retry.reported) logger::Write("Vehicle physics repair: no retry yet for %08X: %s (cell state %d)", v->formId, why, state);
					retry.reported = true;
					continue;
				}
				retry.tried = true;
				logger::Write("Vehicle physics repair: retry full native attach for %08X (scene body %08X, cell state %d)", v->formId, retry.body, state);
				reinterpret_cast<void (*)(std::uint32_t)>(game::Base() + 0x01314780)(v->formId);
				break;
			}
		}
		// A parked vehicle with 3D whose component the game does not update (kept across an in-session load): reload its 3D.
		for (const auto* v : nearby) {
			auto& rl = g_reloads[v->formId];
			const auto it = comps.find(v->formId);
			const bool updated = it != comps.end() && it->second.component && it->second.updates > 0;
			if (updated || v->who != world::VehicleInfo::Who::Parked || !v->ref || !v->bodyCount || rl.done || npcvehicles::UsesVehicle(v->formId)) {
				if (updated) rl.strikes = 0;
				continue;
			}
			if (++rl.strikes < 2) continue;
			rl.done = true; rl.ref = v->ref; rl.disabledAt = now;
			logger::Write("Vehicle physics repair: %08X has 3D (scene body %08X) but its physics component got no update for %.0f s: reloading its 3D once",
				v->formId, v->bodyIds[0], 2 * kEveryMs / 1000.0);
			if (!DisableRef(v->ref)) rl.disabledAt = 0;
			break;  // one per report
		}
	}
}
