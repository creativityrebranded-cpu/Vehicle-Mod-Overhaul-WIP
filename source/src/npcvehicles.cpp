#include "npcvehicles.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "modscene.h"
#include "notify.h"
#include "vehicleflight.h"

#include <Windows.h>
#include <intrin.h>

#include <cmath>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

// Engine pieces (1.16.244, static reading of the decompile; notes/NPC_VEHICLE_STAGE1_RESEARCH_20261017.md):
//  - Vehicles (REV-8, Deimog, Moon Jumper and every modded one) are FURN records; a vehicle reference is recognised as
//    the game does (FUN_140EC72A0: ref vtable +0x200 and HasKeyword(ref, DAT_145F04028), the vehicle keyword).
//  - FUN_141951FB0(actor, vehicle) -> bool: the board call the game uses for followers getting into a vehicle
//    (FUN_14192A3F0, after FUN_14063AF00). A high-process NPC gets a temporary mount package (TESPackage type 0x29) and
//    walks over and gets in; a low-process one is snapped in. (The console MountActor wraps it for actor mounts only.)
//  - FUN_14063AF00(actor, vehicle) -> bool: the vehicle can take this actor (a matching seat, not taken).
//  - FUN_141952530(actor) -> bool: the console DismountActor core.
//  - FUN_14219BB40(actor) -> the vehicle the actor is in (its interaction's vehicle), or null.
//  - Actor vtable +0x868 (arg 0): the dead check the game's re-mount (FUN_141956A70) makes; used on NPCs only.
namespace npcvehicles
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kMountActor = 0x01951FB0, kCanMount = 0x0063AF00, kDismount = 0x01952530, kLookupForm = 0x005DE3B0,
			kGetOwner = 0x0050AAD0, kIsVehicle = 0x00EC72A0;
		constexpr int kJobs = 6;                 // NPC vehicles at once (vehicleflight pilots 1..6)
		constexpr double kFaceRange = 60.0;      // m: the force key picks an NPC in front of the player within this ...
		constexpr double kFaceCone = 0.35;       // rad (20 degrees) either side of where the player faces
		// Follow (the user, 2026-10-17: "the same as my rover", to see both): the rover's follow numbers (vehicleflight
		// kFollow*), on the player's left (the rover trails on the right), each pilot a little further out.
		constexpr double kFollowBack = 12.0, kFollowSide = 4.0, kFollowSpread = 6.0, kFollowNear = 6.0, kFollowStop = 3.0;
		constexpr double kFollowRelaunch = 25.0;   // m: a parked NPC vehicle lifts off again when the player is this far
		constexpr double kFollowLead = 1.5;        // s: the follow point leads a moving target by its velocity
		constexpr double kFollowFar = 20.0;        // m: closer than this with the player still: it parks
		constexpr ULONGLONG kFollowStillMs = 5000, kRelaunchWaitMs = 4000;
		constexpr double kGiveUp = 300.0;        // m: farther than this following ends
		constexpr ULONGLONG kBoardTimeoutMs = 45000, kSettleMs = 2500, kLeaveTimeoutMs = 10000;

		enum class Phase { Idle, Boarding, Following, Settling, Leaving };
		const char* Name(Phase p)
		{
			switch (p) {
			case Phase::Boarding: return "boarding";
			case Phase::Following: return "following";
			case Phase::Settling: return "settling";
			case Phase::Leaving: return "getting out";
			default: return "idle";
			}
		}

		enum class Profile : std::uint8_t { Escort, Pursuit };  // see kProfiles
		struct Job
		{
			Phase phase = Phase::Idle;
			int pilot = 1;
			std::uint32_t npc = 0, vehicle = 0;
			ULONGLONG since = 0, nearSince = 0;
			bool parked = false;               // following: landed because the player stood still (stays aboard)
			ULONGLONG relaunchAt = 0;          // following: no relaunch before this (after a collision stop)
			double stillX = 0, stillY = 0;
			ULONGLONG stillSince = 0;
			bool directTried = false;
			bool moved = false;                // moved next to the vehicle (step 3)
			double startDist = 0;              // NPC -> vehicle when boarding began
			ULONGLONG directAt = 0;
			double tx = 0, ty = 0, tvx = 0, tvy = 0;  // the followed target's last position and smoothed velocity
			ULONGLONG aggroAt = 0;             // last combat request (attack)
			Profile profile = Profile::Escort;
			bool gang = false;                 // started by the gang (mirrors the player), not by the force key
			bool firing = false;               // the vehicle weapon's fire input is held
			ULONGLONG fireAt = 0;              // when the current burst / gap started
			unsigned bursts = 0;
			double aimX = 0, aimY = 0, aimVx = 0, aimVy = 0;  // the player's last position and smoothed velocity (aim lead)
			ULONGLONG aimAt = 0;
			ULONGLONG tAt = 0;
		} g_jobs[kJobs];
		// Boarding steps (2026-10-17 15:21-15:22: the walk package was accepted, board -> 1, but 5 of 6 NPCs never moved):
		// 1 walk (FUN_141951FB0); 2 no 2 m of progress after kWalkMs: direct board (FUN_141993710); 3 still out kDirectMs
		// later: moved next to the vehicle and direct-boarded again.
		constexpr ULONGLONG kWalkMs = 5000, kDirectMs = 4000;
		bool g_told = false;
		int g_forceKey = 0;
		// Aggro (the user, 2026-10-17: "a button to make them aggro so they can shoot at me with the vehicle"): the
		// period key turns every following NPC driver hostile; they keep their seat, close to an orbit around the player
		// facing them, and the vehicle weapon's fire events are logged (the weapon's own fire call is not hooked yet).
		constexpr int kAggroKey = VK_OEM_PERIOD;
		bool g_aggroDown = false;
		// Profiles (the agreed protocol: one autopilot, a profile per vehicle). Escort: trail the player, park when they
		// stop, never fire. Pursuit: hostile driver, circle the player, fire the vehicle weapon. The period key switches
		// every following vehicle between the two.
		struct ProfileParams
		{
			const char* name;
			bool parkWhenStill;   // lands when the player stands still
			bool hostile;         // the driver is put in combat with the player
			bool fire;            // vehicle weapon bursts
			bool orbit;           // circles the player at kAttackRadius instead of trailing
		};
		constexpr ProfileParams kProfiles[2] = {
			{ "escort", true, false, false, false },
			{ "pursuit", false, true, true, true },
		};
		constexpr double kAttackRadius = 30.0;     // m: the orbit around the player
		constexpr double kAttackRun = 70.0;        // m: closer than this the attack controls take over from the autopilot
		constexpr ULONGLONG kReaggroMs = 5000;     // combat re-asked this often while out of combat
		bool g_allowDismount = false;              // only our own Leave() takes a job's NPC out of its vehicle
		// Vehicle weapon (the game's own path, 1.16.244): the player's VehicleHandler (PlayerControls, vtable
		// 0x144C43E58) sets its input block at +0x80 (fire = +0x9F, from "VehicleFireWeapon"), and each frame
		// FUN_1412BFEC0 hands it to the vehicle: FUN_1412C0550(*(FUN_1423FF640() + 0x8B0), &handler, &vehicleFormId) ->
		// the vehicle's component -> FUN_141324650(component + 0x20, handler + 0x80): fire rising -> FUN_14131D120 ->
		// the weapon's FUN_14128EE90 (start); fire falling -> each weapon's vfunc +0x58 (stop). An NPC vehicle gets the
		// same input from a handler-shaped block of ours.
		constexpr std::uint32_t kVehicleSystem = 0x023FF640, kSubmitVehicleInput = 0x012C0550;
		constexpr ULONGLONG kBurstMs = 1200, kBurstGapMs = 1500;
		constexpr double kFireCone = 0.14;         // rad (8 deg): fires only with the nose on the aim point
		// Aim: the weapon fires along the vehicle's nose, so the whole vehicle aims. The aim point leads the player by
		// the shot's flight time (kShotSpeed) and sits at chest height.
		constexpr double kShotSpeed = 250.0;       // m/s (assumed)
		constexpr double kAimHeight = 1.2;         // m above the player's feet
		// The gate (FUN_141317C90): a vehicle's component applies input only when it is the player's vehicle
		// (component +0x579 = player vehicle == this). FUN_141324650(component, input) returns at once otherwise, so
		// an NPC vehicle never took our fire input (2026-10-17 17:53-17:57: 51 bursts sent, no shot). Hooked: for our
		// own input blocks only, the gate is opened for that one call.
		constexpr std::uint32_t kApplyVehicleInput = 0x01324650;
		constexpr double kFireRange = 80.0;        // m
		bool g_forceDown = false, g_keyChosen = false;

		// ---- Combat (from the console commands' cores; the pre-revoke build) ----
		//  IsInCombat: actor vtable +0xB58. StopCombat: the console handler FUN_140C256C0(_, _, ref). StartCombat:
		//  FUN_1418C10A0(*0x145E71728, actor, target, flag), the core of Actor.StartCombat.
		constexpr std::uint32_t kStopCombatCmd = 0x00C256C0, kStartCombat = 0x018C10A0, kCombatManager = 0x05E71728;

		// ---- Guarded engine calls (no C++ objects in these frames) ----
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

		uptr InVehicle(uptr actor)
		{
			__try {
				return EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		bool CanMount(uptr actor, uptr vehicle)
		{
			__try {
				return EngineFn<bool (*)(uptr, uptr)>(kCanMount)(actor, vehicle);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// FUN_141993710(actor, vehicle, 0): builds the seat interaction, sets the process state and sends the get-in action,
		// without the walk (mount package). Its first step is the same seat check (FUN_14063AF00).
		constexpr std::uint32_t kBoardDirect = 0x01993710;
		// The game's own instant seat (FUN_141951FB0's branch for middle / low process actors, notes/NPC_STRUCTURE_20261017.md):
		// FUN_141995460(actor, vehicle, &link) builds and starts the Mount / Passenger interaction, then FUN_141993710(actor,
		// vehicle, 1) seats with snap = 1. (FUN_141993710 with 0 alone, tried earlier, is the walk variant and returned 0.)
		constexpr std::uint32_t kInteraction = 0x01995460;
		int InstantSeat(uptr actor, uptr vehicle, int& seated)
		{
			seated = -1;
			uptr link = 0;
			__try {
				EngineFn<void (*)(uptr, uptr, uptr*)>(kInteraction)(actor, vehicle, &link);
				seated = EngineFn<char (*)(uptr, uptr, char)>(kBoardDirect)(actor, vehicle, 1) ? 1 : 0;
				if (link) {
					auto* rc = reinterpret_cast<volatile long*>(link + 8);  // our reference to the link (the actor keeps its own)
					if (_InterlockedDecrement(rc) == 0) {
						auto** vt = *reinterpret_cast<void***>(link);
						reinterpret_cast<void (*)(uptr, int)>(vt[0])(link, 1);
					}
				}
				return link ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}
		bool CanMount(uptr actor, uptr vehicle);
		int BoardDirect(uptr actor, uptr vehicle)
		{
			if (!CanMount(actor, vehicle)) return -2;  // never without a free seat (15:24:53: 'success' on a seatless T-47 near the player)
			__try {
				return EngineFn<char (*)(uptr, uptr, char)>(kBoardDirect)(actor, vehicle, 0) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		// Puts the actor 2.5 m beside the vehicle (MoveReference, as the rover call uses), facing it.
		bool MoveBeside(uptr actor, uptr vehicle)
		{
			const uptr cell = At<uptr>(vehicle, off::kRefParentCell);
			const uptr ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
			if (!cell) return false;
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double yaw = At<game::Vec3>(vehicle, off::kRefRotation).z;
			float pos[3] = { float(vp.x + std::cos(yaw) * 2.5), float(vp.y - std::sin(yaw) * 2.5), vp.z + 0.2f };
			float rot[3] = { 0.0f, 0.0f, float(yaw - 1.5707963) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(actor, &handle, cell, ws, pos, rot);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The game's own way an actor gets into a vehicle (2026-10-17 research): Activate (FUN_140B47920) ->
		// FUN_14192A3F0: vehicle test, seat check FUN_14063AF00, then FUN_141951FB0(actor, vehicle), which gives the actor
		// a temporary get-in package (type 0x29); the package procedure (FUN_1419DAC80 case 0x29) walks the actor to the
		// seat and, in range, seats it (FUN_1419E5CF0 -> FUN_141993710). The package only runs once the actor's AI
		// re-evaluates: FUN_141914750(actor, 0, 0) is that (the console's EvaluatePackage / evp, FUN_140C33A30).
		constexpr std::uint32_t kEvaluatePackage = 0x01914750;
		int Evaluate(uptr actor)
		{
			__try {
				// immediate = 1: switch to the new package now, not at the AI's next routine check (2026-10-17 16:52: with
				// 0 the high-process NPC kept its own package and never walked).
				EngineFn<void (*)(uptr, char, char)>(kEvaluatePackage)(actor, 1, 0);
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		int Mount(uptr actor, uptr vehicle)
		{
			int r = 0;
			__try {
				r = EngineFn<bool (*)(uptr, uptr)>(kMountActor)(actor, vehicle) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
			if (r == 1) Evaluate(actor);
			return r;
		}

		int Dismount(uptr actor)
		{
			__try {
				return EngineFn<bool (*)(uptr)>(kDismount)(actor) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		bool Dead(uptr actor)
		{
			__try {
				auto** vt = *reinterpret_cast<void***>(actor);
				return reinterpret_cast<bool (*)(uptr, int)>(vt[0x868 / 8])(actor, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return true;
			}
		}

		std::uint32_t OwnerId(uptr ref)
		{
			__try {
				const uptr extra = At<uptr>(ref, 0xC8);
				const uptr o = extra ? EngineFn<uptr (*)(uptr)>(kGetOwner)(extra) : 0;
				return o ? At<std::uint32_t>(o, off::kFormId) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		bool IsActorRef(uptr r) { return At<std::uint8_t>(r, 0x2E) == 'K'; }
		bool IsVehicle(uptr r)
		{
			__try {
				return EngineFn<bool (*)(uptr)>(kIsVehicle)(r);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		// ---- Seat diagnostics (FUN_14063AF00 refuses an NPC when neither a keyword seat nor the 3D fallback seat fits) ----
		// Seats: base form (ref +0x98) +0x264 mask, +0x270 count, +0x278 entries of 0x28 bytes, keyword at +0x10
		// (FUN_1412B2C40: the actor must have that keyword). Fallback: FUN_1412B2A90(vehicle) from the vehicle's 3D.
		constexpr std::uint32_t kFallbackSeat = 0x012B2A90;

		void CopyEditorId(uptr form, char* out, std::size_t n)
		{
			out[0] = 0;
			if (!form) return;
			__try {
				auto** vt = *reinterpret_cast<void***>(form);
				const char* t = reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(form);
				std::size_t i = 0;
				for (; t && i + 1 < n && t[i] >= 0x20 && t[i] < 0x7F; ++i) out[i] = t[i];
				out[i] = 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				out[0] = 0;
			}
		}

		uptr FallbackSeat(uptr vehicle)
		{
			__try {
				return EngineFn<uptr (*)(uptr)>(kFallbackSeat)(vehicle);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		bool ReadSeat(uptr base, std::uint32_t i, std::uint32_t& mask, std::uint32_t& count, uptr& kw)
		{
			__try {
				mask = At<std::uint32_t>(base, 0x264);
				count = At<std::uint32_t>(base, 0x270);
				const uptr arr = At<uptr>(base, 0x278);
				kw = (arr && i < count) ? At<uptr>(arr + i * 0x28, 0x10) : 0;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::unordered_map<std::uint32_t, bool> g_seatLogged;  // vehicle base -> logged

		// Seats are keyword gated (VehicleDriverSeat / VehicleCompanionSeat / VehicleVascoSeat): only the player and
		// companions carry them (log 2026-10-17 15:13). The NPC that will drive gets the vehicle's own driver-seat keyword
		// (FUN_140B42710 = the core of ObjectReference.AddKeyword, native FUN_1420680E0). It stays on that NPC.
		constexpr std::uint32_t kAddKeyword = 0x00B42710;

		uptr DriverSeatKeyword(uptr vehicle)
		{
			const uptr base = At<uptr>(vehicle, 0x98);
			std::uint32_t mask = 0, count = 0;
			uptr kw = 0;
			if (!base || !ReadSeat(base, 0, mask, count, kw) || !count || !(mask & 1) || !kw) return 0;
			return At<std::uint8_t>(kw, 0x2E) == 4 ? kw : 0;
		}

		bool GiveKeyword(uptr ref, uptr kw)
		{
			__try {
				EngineFn<void (*)(uptr, uptr)>(kAddKeyword)(ref, kw);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void LogSeats(uptr npc, uptr vehicle)
		{
			const uptr base = At<uptr>(vehicle, 0x98);
			if (!base) return;
			const std::uint32_t baseId = At<std::uint32_t>(base, off::kFormId);
			if (g_seatLogged[baseId]) return;
			g_seatLogged[baseId] = true;
			std::uint32_t mask = 0, count = 0;
			uptr kw = 0;
			if (!ReadSeat(base, 0, mask, count, kw)) {
				logger::Write("NPC vehicles: seats of %08X unreadable", baseId);
				return;
			}
			char name[96];
			CopyEditorId(base, name, sizeof(name));
			const uptr fb = FallbackSeat(vehicle);
			char fbName[96];
			CopyEditorId(fb, fbName, sizeof(fbName));
			logger::Write("NPC vehicles: seats of %08X %s: %u (mask %X); 3D fallback seat %08X %s; refused NPC %08X (%s)", baseId, name, count, mask,
				fb ? At<std::uint32_t>(fb, off::kFormId) : 0, fbName, At<std::uint32_t>(npc, off::kFormId), modscene::RttiName(npc));
			for (std::uint32_t i = 0; i < count && i < 20; ++i) {
				if (!ReadSeat(base, i, mask, count, kw)) break;
				char k[96];
				CopyEditorId(kw, k, sizeof(k));
				logger::Write("NPC vehicles:   seat %u%s: keyword %08X %s", i, (mask >> i) & 1 ? "" : " (off)", kw ? At<std::uint32_t>(kw, off::kFormId) : 0, k);
			}
		}

		bool Has3D(uptr r)
		{
			const uptr loaded = At<uptr>(r, off::kRefLoadedData);
			return loaded && At<uptr>(loaded, 0x08);
		}
		bool Disabled(uptr r) { return (At<std::uint32_t>(r, 0x20) & 0x800) != 0; }
		double Dist(uptr a, uptr b)
		{
			const auto p = At<game::Vec3>(a, off::kRefPosition), q = At<game::Vec3>(b, off::kRefPosition);
			return std::hypot(double(p.x) - q.x, double(p.y) - q.y);
		}

		void ChooseKey()
		{
			if (g_keyChosen) return;
			g_keyChosen = true;
			g_forceKey = VK_OEM_COMMA;  // fixed: the comma key (the user wants one known button)
			if (!controlmap::KeyFree(g_forceKey)) logger::Write("NPC vehicles: note: the game also binds the force key ,");
			logger::Write("NPC vehicles: force key %s (the NPC you face gets into the nearest vehicle)", controlmap::KeyName(g_forceKey).c_str());
		}

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		bool Outdoors(uptr ref)
		{
			const uptr cell = At<uptr>(ref, off::kRefParentCell);
			return cell && At<uptr>(cell, off::kCellWorldspace);
		}

		void Set(Job& j, Phase p)
		{
			logger::Write("NPC vehicles: [%d] %s -> %s (NPC %08X, vehicle %08X)", j.pilot, Name(j.phase), Name(p), j.npc, j.vehicle);
			j.phase = p;
			j.since = GetTickCount64();
			if (p == Phase::Boarding) {
				const uptr n = RefById(j.npc), v = RefById(j.vehicle);
				j.startDist = n && v ? Dist(n, v) : 0.0;
			}
			j.nearSince = 0;
			j.parked = false;
			j.relaunchAt = 0;
			j.stillSince = 0;
			if (p == Phase::Idle) {
				j.gang = false;
				j.directTried = false;
				j.moved = false;
				j.directAt = 0;
				j.npc = 0;
				j.vehicle = 0;
			}
		}

		// Held by a job (boarding, following or getting out): the force key never takes these.
		bool Locked(std::uint32_t id)
		{
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && (j.npc == id || j.vehicle == id)) return true;
			}
			return false;
		}

		bool Busy(std::uint32_t id)
		{
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && (j.npc == id || j.vehicle == id)) return true;
			}
			return false;
		}

		void FollowPoint(Job& j, uptr player, uptr vehicle, double& x, double& y)
		{
			// The player in their rover: follow the rover (its position and heading), led by its velocity so a fast
			// rover is kept up with instead of chased to a point it has already left.
			uptr target = InVehicle(player);
			if (!target || target == vehicle) target = player;
			const auto tp = At<game::Vec3>(target, off::kRefPosition);
			const ULONGLONG now = GetTickCount64();
			if (j.tAt && now > j.tAt) {
				const double dt = (now - j.tAt) / 1000.0;
				const double vx = (tp.x - j.tx) / dt, vy = (tp.y - j.ty) / dt;
				if (dt < 1.0 && std::hypot(vx, vy) < 250.0) {
					const double a = std::min(1.0, dt * 3.0);
					j.tvx += (vx - j.tvx) * a;
					j.tvy += (vy - j.tvy) * a;
				}
				else {
					j.tvx = j.tvy = 0;
				}
			}
			j.tx = tp.x;
			j.ty = tp.y;
			j.tAt = now;
			const double tsp = std::hypot(j.tvx, j.tvy);
			game::Vec3 pp = tp;
			pp.x += float(j.tvx * kFollowLead);
			pp.y += float(j.tvy * kFollowLead);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double yaw = tsp > 3.0 ? std::atan2(j.tvx, j.tvy) : At<game::Vec3>(target, off::kRefRotation).z;  // moving: its track
			// Formation (2026-10-17, gang): slot 0 left, 1 right, then rows further back, alternating sides.
			const int slot = j.pilot - 1;
			const double side = (slot % 2 ? -1.0 : 1.0) * (kFollowSide + 2.0 + 3.0 * (slot / 2));  // + left, - right
			const double back = kFollowBack + kFollowSpread * (slot / 2);
			(void)kFollowSpread;
			x = pp.x - std::sin(yaw) * back - std::cos(yaw) * side;
			y = pp.y - std::cos(yaw) * back + std::sin(yaw) * side;
			const double to = std::hypot(double(vp.x) - pp.x, double(vp.y) - pp.y);
			if (to < kFollowNear && to > 0.1) {  // too close: back off along the line from the player
				x = pp.x + (vp.x - pp.x) / to * back;
				y = pp.y + (vp.y - pp.y) / to * back;
			}
		}

		bool InCombat(uptr actor)
		{
			__try {
				auto** vt = *reinterpret_cast<void***>(actor);
				return reinterpret_cast<bool (*)(uptr)>(vt[0xB58 / 8])(actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void Calm(uptr actor)
		{
			__try {
				EngineFn<char (*)(uptr, uptr, uptr)>(kStopCombatCmd)(0, 0, actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		bool Aggro(uptr actor, uptr target)
		{
			__try {
				const uptr mgr = Ptr(kCombatManager);
				if (!mgr) return false;
				EngineFn<void (*)(uptr, uptr, uptr, char)>(kStartCombat)(mgr, actor, target, 0);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The input block the vehicle reads (a handler-shaped buffer: the submit reads +0x80..+0xA0 and +0xAF).
		alignas(16) std::uint8_t g_fireHandler[kJobs][0xC0] = {};

		bool SubmitFire(int slot, std::uint32_t vehicleId, bool fire)
		{
			auto* h = g_fireHandler[slot];
			std::memset(h + 0x80, 0, 0x20);
			h[0x9F] = fire ? 1 : 0;
			__try {
				const uptr sys = EngineFn<uptr (*)()>(kVehicleSystem)();
				const uptr inputs = sys ? At<uptr>(sys, 0x8B0) : 0;
				if (!inputs) return false;
				uptr handler = reinterpret_cast<uptr>(h);
				std::uint32_t id = vehicleId;
				EngineFn<std::uint64_t (*)(uptr, uptr*, std::uint32_t*, std::uint64_t)>(kSubmitVehicleInput)(inputs, &handler, &id, 0);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		double WrapAngle(double a)
		{
			while (a > 3.14159265358979) a -= 6.28318530717959;
			while (a < -3.14159265358979) a += 6.28318530717959;
			return a;
		}

		// Attack: a point on the orbit around the player a little ahead of the vehicle (each pilot its own direction).
		void AttackPoint(const Job& j, uptr player, uptr vehicle, double& x, double& y)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double dir = (j.pilot % 2) ? 1.0 : -1.0;
			const double ang = std::atan2(double(vp.x) - pp.x, double(vp.y) - pp.y) + dir * 0.6;
			x = pp.x + std::sin(ang) * kAttackRadius;
			y = pp.y + std::cos(ang) * kAttackRadius;
		}

		// The aim point: the player led by their velocity over the shot's flight time, at chest height.
		void AimPoint(Job& j, uptr player, uptr vehicle, double& ax, double& ay, double& az)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const ULONGLONG now = GetTickCount64();
			if (j.aimAt && now > j.aimAt) {
				const double dt = (now - j.aimAt) / 1000.0;
				const double ix = (pp.x - j.aimX) / dt, iy = (pp.y - j.aimY) / dt;
				if (dt < 1.0 && std::hypot(ix, iy) < 150.0) {
					const double a = std::min(1.0, dt * 4.0);
					j.aimVx += (ix - j.aimVx) * a;
					j.aimVy += (iy - j.aimVy) * a;
				}
				else {
					j.aimVx = j.aimVy = 0;
				}
			}
			j.aimX = pp.x; j.aimY = pp.y; j.aimAt = now;
			const double dist = std::hypot(double(pp.x) - vp.x, double(pp.y) - vp.y);
			const double t = dist / kShotSpeed;
			ax = pp.x + j.aimVx * t;
			ay = pp.y + j.aimVy * t;
			az = pp.z + kAimHeight;
		}

		// Attack run (close): every control channel: nose on the aim point (yaw and pitch), circle the player (strafe;
		// slower while a burst is on), hold the orbit radius (throttle, reverse when too close).
		void AttackRun(Job& j, uptr player, uptr vehicle)
		{
			double axp, ayp, azp;
			AimPoint(j, player, vehicle, axp, ayp, azp);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const game::Vec3 pp{ float(axp), float(ayp), float(azp) };
			const double dx = double(pp.x) - vp.x, dy = double(pp.y) - vp.y;
			const double dist = std::hypot(dx, dy);
			const double heading = At<game::Vec3>(vehicle, off::kRefRotation).z;
			vehicleflight::Command c;
			c.set = vehicleflight::Command::kYaw | vehicleflight::Command::kStrafe | vehicleflight::Command::kThrottle |
				vehicleflight::Command::kPitch | vehicleflight::Command::kGear;
			c.yawRate = std::clamp((j.firing ? 2.5 : 1.6) * WrapAngle(std::atan2(dx, dy) - heading), -1.2, 1.2);
			c.strafe = ((j.pilot % 2) ? 1.0 : -1.0) * (j.firing ? 0.3 : 0.6);
			c.throttle = std::clamp((dist - kAttackRadius) / 20.0, -0.6, 0.6);
			c.pitch = std::clamp(std::atan2(double(pp.z) - vp.z, std::max(dist, 5.0)), -0.3, 0.3);
			c.gear = 2;
			c.holdMs = 300;
			vehicleflight::PilotCommand(j.pilot, c);
		}

		void Leave(Job& j, uptr npc, const char* why)
		{
			g_allowDismount = true;
			const int r = npc ? Dismount(npc) : -1;
			g_allowDismount = false;
			logger::Write("NPC vehicles: [%d] dismount (%s) -> %d", j.pilot, why, r);
			Set(j, r == 1 ? Phase::Leaving : Phase::Idle);
		}

		void Update(Job& j, uptr player)
		{
			const ULONGLONG now = GetTickCount64();
			const uptr npc = RefById(j.npc), vehicle = RefById(j.vehicle);
			if (!npc || !vehicle) {
				logger::Write("NPC vehicles: [%d] %s lost", j.pilot, !npc ? "NPC" : "vehicle");
				vehicleflight::PilotStop(j.pilot, "lost");
				Set(j, Phase::Idle);
				return;
			}
			const bool aboard = InVehicle(npc) == vehicle;
			const double toPlayer = Dist(vehicle, player);
			if (j.phase == Phase::Following) vehicleflight::WatchFlip(vehicle, "NPC vehicles");
			switch (j.phase) {
			case Phase::Boarding:
				if (aboard) {
					logger::Write("NPC vehicles: [%d] NPC %08X aboard after %.1f s", j.pilot, j.npc, (now - j.since) / 1000.0);
					double fx, fy;
					FollowPoint(j, player, vehicle, fx, fy);
					if (vehicleflight::PilotFly(j.pilot, vehicle, fx, fy, kFollowStop, 0, true)) {
						if (!g_told) notify::Show("An NPC is following you in a vehicle");
						g_told = true;
						Set(j, Phase::Following);
					}
					else {
						Leave(j, npc, "pilot refused");
					}
				}
				else if (!j.directTried && now - j.since > kWalkMs && Dist(npc, vehicle) > j.startDist - 2.0) {
					// Not walking (its AI never ran the get-in package): the game's own instant seat, as it does for middle / low
					// process NPCs. The player-seat hook stays in front of it (seats this NPC, never the player).
					j.directTried = true;
					std::uint8_t level = 0xFF;
					const uptr proc = At<uptr>(npc, 0x228);
					if (proc) level = At<std::uint8_t>(proc, 0xE7);
					if (!CanMount(npc, vehicle)) {
						logger::Write("NPC vehicles: [%d] NPC %08X not walking; instant seat skipped (seat refused)", j.pilot, j.npc);
					}
					else {
						int seated = -1;
						const int link = InstantSeat(npc, vehicle, seated);
						const uptr in = InVehicle(npc);
						logger::Write("NPC vehicles: [%d] NPC %08X not walking (%.0f m -> %.0f m in %.0f s, process level %u); instant seat: interaction %d, seat %d; "
							"the game says it is in %08X", j.pilot, j.npc, j.startDist, Dist(npc, vehicle), (now - j.since) / 1000.0, level, link, seated,
							in ? At<std::uint32_t>(in, off::kFormId) : 0);
					}
				}
				else if (now - j.since > kBoardTimeoutMs) {
					{
						const uptr in = InVehicle(npc);
						bool link = false;
						__try {
							link = EngineFn<bool (*)(uptr, int)>(0x002C5C90)(At<uptr>(npc, 0xC8), 0xA5);
						}
						__except (EXCEPTION_EXECUTE_HANDLER) {
						}
						logger::Write("NPC vehicles: [%d] NPC %08X did not board (%s; %.0f m from the vehicle; the game says it is in vehicle %08X, seat link %s)", j.pilot, j.npc,
							"timed out", Dist(npc, vehicle), in ? At<std::uint32_t>(in, off::kFormId) : 0, link ? "yes" : "no");
					}
					Set(j, Phase::Idle);
				}
				break;
			case Phase::Following: {
				// Following, as the player's rover follow (U): trail behind the player, park when the player stands still,
				// lift off again when the player moves away; the NPC stays aboard throughout.
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				if (!j.stillSince || std::hypot(pp.x - j.stillX, pp.y - j.stillY) > 1.0) {
					j.stillX = pp.x;
					j.stillY = pp.y;
					j.stillSince = now;
				}
				const bool playerStill = now - j.stillSince >= kFollowStillMs;
				if (!aboard) {
					logger::Write("NPC vehicles: [%d] NPC %08X left the vehicle", j.pilot, j.npc);
					vehicleflight::PilotStop(j.pilot, "driver got out");
					Set(j, Phase::Idle);
				}
				else if (!Outdoors(player) || toPlayer > kGiveUp) {
					logger::Write("NPC vehicles: [%d] stopped following (%s, %.0f m)", j.pilot, Outdoors(player) ? "too far" : "you went indoors", toPlayer);
					vehicleflight::PilotStop(j.pilot, "gave up");
					Set(j, Phase::Settling);
				}
				else if (vehicleflight::PilotActive(j.pilot) && kProfiles[int(j.profile)].orbit) {
					if (kProfiles[int(j.profile)].hostile && !InCombat(npc) && now - j.aggroAt >= kReaggroMs) {
						j.aggroAt = now;
						const bool ok = Aggro(npc, player);
						logger::Write("NPC vehicles: [%d] NPC %08X attack: combat with you -> %s", j.pilot, j.npc, ok ? "asked" : "failed");
					}
					double fx, fy;
					AttackPoint(j, player, vehicle, fx, fy);
					vehicleflight::PilotFly(j.pilot, vehicle, fx, fy, kFollowStop, 0, true);
					if (toPlayer < kAttackRun) AttackRun(j, player, vehicle);
					// Vehicle weapon: bursts with the nose on the player and in range.
					double axp = 0, ayp = 0, azp = 0;
					if (toPlayer >= kAttackRun) AimPoint(j, player, vehicle, axp, ayp, azp);  // (inside, AttackRun updated it)
					else {
						const double t = toPlayer / kShotSpeed;
						const auto ppl = At<game::Vec3>(player, off::kRefPosition);
						axp = ppl.x + j.aimVx * t; ayp = ppl.y + j.aimVy * t;
					}
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					const double aimErr = std::fabs(WrapAngle(std::atan2(axp - vp.x, ayp - vp.y) - At<game::Vec3>(vehicle, off::kRefRotation).z));
					(void)kFireCone;
					const bool canFire = kProfiles[int(j.profile)].fire && toPlayer < kFireRange;  // shots are aimed at the player, any direction
					const int slot = j.pilot - 1;
					if (j.firing) {
						if (!canFire || now - j.fireAt >= kBurstMs) {
							SubmitFire(slot, j.vehicle, false);
							j.firing = false;
							j.fireAt = now;
						}
						else {
							SubmitFire(slot, j.vehicle, true);
						}
					}
					else if (canFire && now - j.fireAt >= kBurstGapMs) {
						const bool ok = SubmitFire(slot, j.vehicle, true);
						j.firing = true;
						j.fireAt = now;
						if (j.bursts++ % 10 == 0) {
							logger::Write("NPC vehicles: [%d] vehicle %08X weapon burst %u (%.0f m, aim off %.0f deg) -> %s", j.pilot, j.vehicle, j.bursts, toPlayer, aimErr * 57.2958,
								ok ? "input sent" : "input FAILED");
						}
					}
				}
				else if (vehicleflight::PilotActive(j.pilot)) {
					if (kProfiles[int(j.profile)].parkWhenStill && playerStill && toPlayer < kFollowFar) {
						vehicleflight::PilotStop(j.pilot, "follow: player stopped; parked");
						j.parked = true;
						logger::Write("NPC vehicles: [%d] parked %.0f m from you (NPC stays aboard)", j.pilot, toPlayer);
					}
					else {
						double fx, fy;
						FollowPoint(j, player, vehicle, fx, fy);
						vehicleflight::PilotFly(j.pilot, vehicle, fx, fy, kFollowStop, 0, true);
					}
				}
				else {
					// Not flying: parked by us, or the flight ended (collision, stuck, landed). Relaunch like the rover follow.
					if (!j.parked && !j.relaunchAt) {
						logger::Write("NPC vehicles: [%d] flight ended (%s), %.0f m from you; relaunch in %.0f s", j.pilot, vehicleflight::PilotEndReason(j.pilot), toPlayer,
							kRelaunchWaitMs / 1000.0);
						j.relaunchAt = now + kRelaunchWaitMs;
					}
					const bool waitOver = j.parked || now >= j.relaunchAt;
					const bool orbit = kProfiles[int(j.profile)].orbit;
					if (waitOver && (toPlayer > kFollowRelaunch || orbit)) {
						double fx, fy;
						if (orbit) AttackPoint(j, player, vehicle, fx, fy);
						else FollowPoint(j, player, vehicle, fx, fy);
						if (vehicleflight::PilotFly(j.pilot, vehicle, fx, fy, kFollowStop, 0, true)) {
							logger::Write("NPC vehicles: [%d] following again (%.0f m from you)", j.pilot, toPlayer);
							j.parked = false;
							j.relaunchAt = 0;
						}
					}
				}
				break;
			}
			case Phase::Settling:
				if (now - j.since >= kSettleMs) Leave(j, npc, "parked");
				break;
			case Phase::Leaving:
				if (!InVehicle(npc)) {
					logger::Write("NPC vehicles: [%d] NPC %08X out after %.1f s", j.pilot, j.npc, (now - j.since) / 1000.0);
					Set(j, Phase::Idle);
				}
				else if (now - j.since > kLeaveTimeoutMs) {
					logger::Write("NPC vehicles: [%d] NPC %08X still aboard after %.0f s", j.pilot, j.npc, kLeaveTimeoutMs / 1000.0);
					Set(j, Phase::Idle);
				}
				break;
			default:
				break;
			}
		}

		// ---- The player is pulled into a vehicle an NPC boards (2026-10-17 15:47: the user shot NPCs, one boarded and the
		// player was put into a vehicle too; the player's "own vehicle" link changed). FUN_14219BE30(vehicle) is a virtual
		// (vtable 1486CA880, no direct callers) that seats the PLAYER (DAT_145F43230) in `vehicle` as a passenger
		// (PassengerInteraction), starts it and records the vehicle as the player's. Every call is logged with its caller;
		// a vehicle one of our NPC jobs is using is refused, so NPCs ride without the player.
		constexpr std::uint32_t kSeatPlayer = 0x0219BE30;
		using SeatPlayer_t = void (*)(uptr);
		SeatPlayer_t g_seatPlayerOriginal = nullptr;

		bool OursVehicle(std::uint32_t id)
		{
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.vehicle == id) return true;
			}
			return false;
		}

		// What FUN_14219BE30 does for the player, done for the NPC: the seat link (FUN_141995460 builds the Mount / Passenger
		// interaction for that actor, keyword seat, and starts it), then FUN_141A9C8C0(actor) completes it (interaction +0x58).
		// The sit action FUN_1419F7100 calls FUN_14219BE30 after ANY actor's successful sit on a vehicle (2026-10-17 15:57:15,
		// caller 1419F7750): the game's only vehicle seat path is player-only, which is why an NPC getting in seated the player.
		constexpr std::uint32_t kSeatLink = 0x01995460, kSeatFinish = 0x01A9C8C0;
		int SeatNpc(uptr npc, uptr vehicle)
		{
			uptr link = 0;
			__try {
				EngineFn<void (*)(uptr, uptr, uptr*)>(kSeatLink)(npc, vehicle, &link);
				if (!link) return 0;
				EngineFn<void (*)(uptr)>(kSeatFinish)(npc);
				// release our reference to the link (the actor's extra data keeps its own)
				auto* rc = reinterpret_cast<volatile long*>(link + 8);
				if (_InterlockedDecrement(rc) == 0) {
					auto** vt = *reinterpret_cast<void***>(link);
					reinterpret_cast<void (*)(uptr, int)>(vt[0])(link, 1);
				}
				return 1;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		uptr JobNpcFor(std::uint32_t vehicleId)
		{
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.vehicle == vehicleId) return RefById(j.npc);
			}
			return 0;
		}

		void SeatPlayerDetour(uptr vehicle)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - game::Base();
			const std::uint32_t id = vehicle ? At<std::uint32_t>(vehicle, off::kFormId) : 0;
			const bool block = id && OursVehicle(id);
			logger::Write("NPC vehicles: the game seats the player in vehicle %08X (caller 14%07llX) -> %s", id, static_cast<unsigned long long>(caller),
				block ? "BLOCKED (an NPC job's vehicle)" : "allowed");
			if (block) {
				const uptr npc = JobNpcFor(id);
				const int r = npc ? SeatNpc(npc, vehicle) : 0;
				logger::Write("NPC vehicles: seated NPC %08X in vehicle %08X instead of the player -> %d (game says the NPC is in %08X)", npc ? At<std::uint32_t>(npc, off::kFormId) : 0, id, r,
					npc && InVehicle(npc) ? At<std::uint32_t>(InVehicle(npc), off::kFormId) : 0);
				return;
			}
			g_seatPlayerOriginal(vehicle);
		}

		// The force key: the NPC the player faces (within kFaceCone of the player's heading, up to kFaceRange; the most
		// centred, then the nearest) gets into the free vehicle nearest that NPC.
		// Sends `npc` to `vehicle` (the game's get-in; a driver-seat keyword when the seat refuses) and starts the job.
		bool StartJob(Job& slot, uptr npc, uptr vehicle, bool gang)
		{
			const std::uint32_t npcId = At<std::uint32_t>(npc, off::kFormId);
			if (!CanMount(npc, vehicle)) {
				LogSeats(npc, vehicle);
				const uptr kw = DriverSeatKeyword(vehicle);
				if (kw) GiveKeyword(npc, kw);
			}
			const bool seat = CanMount(npc, vehicle);
			const int r = seat ? Mount(npc, vehicle) : 0;
			const uptr base = At<uptr>(vehicle, 0x98);
			logger::Write("NPC vehicles: [%d] NPC %08X -> vehicle %08X (base %08X, %.0f m from it); seat %s; get-in -> %d", slot.pilot, npcId,
				At<std::uint32_t>(vehicle, off::kFormId), base ? At<std::uint32_t>(base, off::kFormId) : 0, Dist(npc, vehicle), seat ? "free" : "REFUSED", r);
			if (r != 1) return false;
			slot.npc = npcId;
			slot.vehicle = At<std::uint32_t>(vehicle, off::kFormId);
			slot.gang = gang;
			slot.profile = Profile::Escort;
			Set(slot, Phase::Boarding);
			return true;
		}

		void Force(uptr player)
		{
			Job* slot = nullptr;
			for (auto& j : g_jobs) {
				if (j.phase == Phase::Idle) {
					slot = &j;
					break;
				}
			}
			if (!slot) {
				notify::Show("%d NPC vehicles already busy", kJobs);
				logger::Write("NPC vehicles: force: all %d slots busy", kJobs);
				return;
			}
			const auto refs = modscene::LoadedRefs(8000);
			const std::uint32_t own = vehicleflight::OwnRoverId();
			const uptr playerVehicle = InVehicle(player);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const double facing = At<game::Vec3>(player, off::kRefRotation).z;  // heading: atan2(x, y)
			uptr npc = 0;
			double bestScore = 1e9, npcDist = 0, npcOff = 0;
			std::vector<uptr> vehicles;
			for (const uptr r : refs) {
				if (r == player || Disabled(r) || !Has3D(r)) continue;
				const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
				if (IsVehicle(r)) {
					if (id != own && r != playerVehicle && !Locked(id)) vehicles.push_back(r);
					continue;
				}
				if (!IsActorRef(r) || Locked(id) || Dead(r) || InVehicle(r)) continue;
				const auto q = At<game::Vec3>(r, off::kRefPosition);
				const double dx = double(q.x) - pp.x, dy = double(q.y) - pp.y, d = std::hypot(dx, dy);
				if (d < 0.5 || d > kFaceRange) continue;
				double offAngle = std::atan2(dx, dy) - facing;
				while (offAngle > 3.14159265) offAngle -= 6.28318531;
				while (offAngle < -3.14159265) offAngle += 6.28318531;
				offAngle = std::fabs(offAngle);
				if (offAngle > kFaceCone) continue;
				const double score = offAngle * 30.0 + d;  // centred first (1 degree ~ half a metre), then near
				if (score < bestScore) {
					bestScore = score;
					npc = r;
					npcDist = d;
					npcOff = offAngle;
				}
			}
			if (!npc || vehicles.empty()) {
				logger::Write("NPC vehicles: force: %s", !npc ? "no NPC in front of you" : "no free vehicle loaded");
				notify::Show(!npc ? "No NPC in front of you" : "No free vehicle nearby");
				return;
			}
			uptr vehicle = 0;
			double bestV = 1e9;
			for (const uptr v : vehicles) {
				if (!DriverSeatKeyword(v) && !CanMount(npc, v)) continue;  // no seat for anyone
				const double d = Dist(v, npc);
				if (d < bestV) {
					bestV = d;
					vehicle = v;
				}
			}
			if (!vehicle) {
				logger::Write("NPC vehicles: force: no loaded vehicle has a seat");
				notify::Show("No free vehicle nearby");
				return;
			}
			logger::Write("NPC vehicles: [%d] force: NPC %08X (%s, %.0f m in front of you, %.0f deg off)", slot->pilot, At<std::uint32_t>(npc, off::kFormId),
				modscene::RttiName(npc), npcDist, npcOff * 57.2958);
			if (!StartJob(*slot, npc, vehicle, false)) {
				notify::Show("That NPC can't get in");
				return;
			}
			notify::Show("NPC sent to a vehicle (%.0f m)", bestV);
		}

		// ---- The gang (The Gang's All Here, or any followers): the game's own follower list ----
		// Game.GetPlayerFollowers (FUN_14200B760): the player's extra data of type 0x1B holds {.. +0x18 count, +0x20
		// array of 8-byte entries, handle first}; each handle's actor counts when FUN_14192A290(actor, player) is true.
		constexpr std::uint32_t kGetExtra = 0x00347170, kIsFollowing = 0x0192A290;
		constexpr double kGangRange = 80.0;      // m: followers this close to the player get a vehicle
		constexpr double kSpareRange = 400.0;    // m: the player's spare vehicles are used from this far
		constexpr double kLocalRange = 120.0;    // m: other vehicles (the registry's) within this of the follower
		constexpr ULONGLONG kMirrorMs = 1200;    // the player in / out of a vehicle this long before the gang follows

		uptr HandleRef(std::uint32_t handle)
		{
			uptr ref = 0;
			uptr* out = &ref;
			struct
			{
				uptr vtbl;
				uptr** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup);
			if (ref) EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref);  // the world keeps the actor; we only read it this frame
			return ref;
		}

		int Followers(uptr player, uptr* out, int max)
		{
			int n = 0;
			__try {
				const uptr data = EngineFn<uptr (*)(uptr, std::uint32_t)>(kGetExtra)(At<uptr>(player, 0xC8), 0x1B);
				if (!data) return 0;
				const std::uint32_t count = std::min<std::uint32_t>(At<std::uint32_t>(data, 0x18), 64);
				const auto* arr = At<const std::uint32_t*>(data, 0x20);
				for (std::uint32_t i = 0; arr && i < count && n < max; ++i) {
					const uptr a = HandleRef(arr[i * 2]);
					if (a && EngineFn<bool (*)(uptr, uptr)>(kIsFollowing)(a, player)) out[n++] = a;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return n;
		}

		// The vehicle for a gang member: the player's spare vehicles first (owned by the player, or the player's own
		// vehicle when the player is in another one), then the nearest other free vehicle within kLocalRange of them
		// (unowned before owned). Never generated: only vehicles already in the world (the registry's).
		uptr GangVehicle(uptr player, uptr npc, const std::vector<uptr>& vehicles, bool& spare)
		{
			const uptr playerBase = At<uptr>(player, 0x98);
			const std::uint32_t playerBaseId = playerBase ? At<std::uint32_t>(playerBase, off::kFormId) : 0;
			const std::uint32_t own = vehicleflight::OwnRoverId();
			uptr best = 0;
			double bestScore = 1e18;
			for (const uptr v : vehicles) {
				if (!DriverSeatKeyword(v) && !CanMount(npc, v)) continue;
				const std::uint32_t id = At<std::uint32_t>(v, off::kFormId);
				const std::uint32_t owner = OwnerId(v);
				const bool isSpare = id == own || (playerBaseId && owner == playerBaseId);
				const double dn = Dist(v, npc), dp = Dist(v, player);
				double score;
				if (isSpare && dp < kSpareRange) score = dn;                      // spares first
				else if (!isSpare && dn < kLocalRange) score = 1e6 + dn + (owner ? 1e5 : 0.0);  // then local, unowned first
				else continue;
				if (score < bestScore) { bestScore = score; best = v; spare = isSpare; }
			}
			return best;
		}

		bool g_playerRiding = false;
		ULONGLONG g_ridingSince = 0, g_mirrorAt = 0;

		void GangTick(uptr player)
		{
			const ULONGLONG now = GetTickCount64();
			const bool riding = InVehicle(player) != 0;
			if (riding != g_playerRiding) {
				g_playerRiding = riding;
				g_ridingSince = now;
			}
			if (now - g_ridingSince < kMirrorMs || now - g_mirrorAt < 1000) return;
			g_mirrorAt = now;
			if (!riding) {
				// The player got out: gang vehicles land and their drivers get out (the vehicles stay where they land).
				for (auto& j : g_jobs) {
					if (!j.gang) continue;
					if (j.phase == Phase::Boarding) {
						logger::Write("NPC vehicles: [%d] gang: you got out; NPC %08X stops boarding", j.pilot, j.npc);
						Set(j, Phase::Idle);
					}
					else if (j.phase == Phase::Following) {
						vehicleflight::PilotStop(j.pilot, "gang: the player got out");
						logger::Write("NPC vehicles: [%d] gang: you got out; NPC %08X lands and gets out", j.pilot, j.npc);
						Set(j, Phase::Settling);
					}
				}
				return;
			}
			// The player is in a vehicle: every follower near the player without one gets one.
			uptr gang[32];
			const int n = Followers(player, gang, 32);
			if (!n) return;
			std::vector<uptr> vehicles;
			bool haveList = false;
			const uptr playerVehicle = InVehicle(player);
			for (int i = 0; i < n; ++i) {
				const uptr npc = gang[i];
				const std::uint32_t npcId = At<std::uint32_t>(npc, off::kFormId);
				if (Locked(npcId) || Dead(npc) || InVehicle(npc) || Dist(npc, player) > kGangRange) continue;
				Job* slot = nullptr;
				for (auto& j : g_jobs) {
					if (j.phase == Phase::Idle) { slot = &j; break; }
				}
				if (!slot) {
					static ULONGLONG lastSay = 0;
					if (now - lastSay > 30000) { lastSay = now; logger::Write("NPC vehicles: gang: all %d vehicle slots busy; the rest stay on foot", kJobs); }
					return;
				}
				if (!haveList) {
					haveList = true;
					const std::uint32_t ownId = vehicleflight::OwnRoverId();
					for (const uptr r : modscene::LoadedRefs(8000)) {
						if (!IsVehicle(r) || Disabled(r) || !Has3D(r) || r == playerVehicle) continue;
						const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
						if (Locked(id)) continue;
						(void)ownId;
						vehicles.push_back(r);
					}
				}
				bool spare = false;
				const uptr v = GangVehicle(player, npc, vehicles, spare);
				if (!v) {
					static ULONGLONG lastNone = 0;
					if (now - lastNone > 30000) {
						lastNone = now;
						logger::Write("NPC vehicles: gang: no vehicle for NPC %08X (no spare within %.0f m, no free vehicle within %.0f m of them)", npcId, kSpareRange, kLocalRange);
					}
					continue;
				}
				vehicles.erase(std::remove(vehicles.begin(), vehicles.end(), v), vehicles.end());
				logger::Write("NPC vehicles: [%d] gang: NPC %08X (%s) -> %s vehicle %08X (owner %08X, %.0f m from them)", slot->pilot, npcId, modscene::RttiName(npc),
					spare ? "your spare" : "local", At<std::uint32_t>(v, off::kFormId), OwnerId(v), Dist(v, npc));
				StartJob(*slot, npc, v, true);
			}
		}
	}

	void Tick()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) return;
		ChooseKey();
		const bool fdown = g_forceKey && Focused() && (GetAsyncKeyState(g_forceKey) & 0x8000) != 0;
		if (fdown && !g_forceDown) Force(player);
		g_forceDown = fdown;
		GangTick(player);
		const bool adown = Focused() && (GetAsyncKeyState(kAggroKey) & 0x8000) != 0;
		if (adown && !g_aggroDown) {
			// Any vehicle in escort -> all to pursuit; otherwise all back to escort.
			bool anyEscort = false;
			for (const auto& j : g_jobs) anyEscort = anyEscort || (j.phase == Phase::Following && j.profile == Profile::Escort);
			const Profile to = anyEscort ? Profile::Pursuit : Profile::Escort;
			int n = 0;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Following) continue;
				const uptr npc = RefById(j.npc);
				if (!npc) continue;
				++n;
				j.profile = to;
				if (kProfiles[int(to)].hostile) {
					j.aggroAt = GetTickCount64();
					Aggro(npc, player);
				}
				else {
					Calm(npc);
					if (j.firing) SubmitFire(j.pilot - 1, j.vehicle, false);
					j.firing = false;
				}
			}
			notify::Show(to == Profile::Pursuit ? "Vehicles: pursuit" : "Vehicles: escort");
			logger::Write("NPC vehicles: profile %s (%d following)", kProfiles[int(to)].name, n);
		}
		g_aggroDown = adown;
		for (int i = 0; i < kJobs; ++i) {
			g_jobs[i].pilot = i + 1;
			if (g_jobs[i].phase != Phase::Idle) Update(g_jobs[i], player);
		}
	}

	namespace
	{
		// The game's dismount FUN_141952530(actor): refused for an NPC our job keeps seated (combat AI would get out to
		// fight on foot), unless our own Leave() is the caller.
		constexpr std::uint32_t kDismountFn = 0x01952530;
		using Dismount_t = char (*)(uptr);
		Dismount_t g_dismountOriginal = nullptr;

		char DismountDetour(uptr actor)
		{
			if (!g_allowDismount && actor) {
				const std::uint32_t id = At<std::uint32_t>(actor, off::kFormId);
				for (const auto& j : g_jobs) {
					if (j.npc == id && j.phase == Phase::Following) {
						static ULONGLONG lastSay = 0;
						const ULONGLONG now = GetTickCount64();
						if (now - lastSay >= 2000) {
							lastSay = now;
							logger::Write("NPC vehicles: [%d] the game tried to dismount NPC %08X (caller 14%07llX): refused", j.pilot, id,
								static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - game::Base()));
						}
						return 0;
					}
				}
			}
			return g_dismountOriginal(actor);
		}

		using ApplyInput_t = void (*)(uptr, uptr);
		ApplyInput_t g_applyOriginal = nullptr;
		unsigned g_gateOpened = 0;

		void ApplyInputDetour(uptr component, uptr input)
		{
			bool ours = false;
			for (int i = 0; i < kJobs; ++i) {
				if (input == reinterpret_cast<uptr>(g_fireHandler[i]) + 0x80) { ours = true; break; }
			}
			if (!ours || !component) {
				g_applyOriginal(component, input);
				return;
			}
			auto* gate = reinterpret_cast<std::uint8_t*>(component + 0x579);
			const std::uint8_t was = *gate;
			*gate = 1;
			g_applyOriginal(component, input);
			*gate = was;
			if (!was && g_gateOpened++ % 200 == 0) logger::Write("NPC vehicles: weapon input applied to an NPC vehicle (gate opened %u times)", g_gateOpened);
		}

		// The vehicle's weapon-fired event sink (vtable slot at 0x144C46FA0 = FUN_14131D2C0: recoil from
		// fVehicleWeaponForceTorque*; event +0x10 = the shooter, +0x18 = its projectiles). Hooked only to log who fires,
		// so a test shows whether a hostile driver fires the vehicle weapon on its own.
		constexpr std::uint32_t kFireSinkSlot = 0x04C46FA0, kFireSink = 0x0131D2C0;
		using FireSink_t = std::uint64_t (*)(uptr, uptr);
		FireSink_t g_fireOriginal = nullptr;

		// Aim (the user, 2026-10-17: the weapon's aim is free, not the nose): each projectile a job's vehicle launches
		// is turned at the player, led by the player's velocity over its own flight time. Projectile (CommonLibSF
		// 1.14 layout): movementDirection +0x158, velocity +0x164 (both checked for sane values before any write).
		constexpr std::uint32_t kProjDir = 0x158, kProjVel = 0x164;
		bool g_projLayoutBad = false;

		int AimProjectiles(uptr event, const Job& j, uptr player)
		{
			int aimed = 0;
			const auto* list = At<const std::uint32_t*>(event, 0x18);
			if (!list) return 0;
			const std::uint32_t n = std::min<std::uint32_t>(list[0], 16);
			const auto* arr = *reinterpret_cast<const uptr* const*>(reinterpret_cast<const std::uint8_t*>(list) + 0x10);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			for (std::uint32_t i = 0; arr && i < n; ++i) {
				const uptr proj = arr[i];
				if (!proj) continue;
				auto* dir = reinterpret_cast<float*>(proj + kProjDir);
				auto* vel = reinterpret_cast<float*>(proj + kProjVel);
				const double dl = std::sqrt(double(dir[0]) * dir[0] + double(dir[1]) * dir[1] + double(dir[2]) * dir[2]);
				const double speed = std::sqrt(double(vel[0]) * vel[0] + double(vel[1]) * vel[1] + double(vel[2]) * vel[2]);
				if (std::fabs(dl - 1.0) > 0.05 || !(speed > 5.0 && speed < 20000.0)) {
					if (!g_projLayoutBad) {
						g_projLayoutBad = true;
						logger::Write("NPC vehicles: projectile layout differs (direction length %.2f, speed %.1f); shots not aimed", dl, speed);
					}
					continue;
				}
				const auto op = At<game::Vec3>(proj, off::kRefPosition);
				double dx = pp.x - op.x, dy = pp.y - op.y, dz = pp.z + kAimHeight - op.z;
				const double t = std::sqrt(dx * dx + dy * dy + dz * dz) / speed;
				dx += j.aimVx * t;
				dy += j.aimVy * t;
				const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (len < 1.0) continue;
				dx /= len; dy /= len; dz /= len;
				dir[0] = float(dx); dir[1] = float(dy); dir[2] = float(dz);
				vel[0] = float(dx * speed); vel[1] = float(dy * speed); vel[2] = float(dz * speed);
				auto* rot = reinterpret_cast<float*>(proj + off::kRefRotation);
				rot[0] = float(-std::atan2(dz, std::hypot(dx, dy)));  // nose down +
				rot[2] = float(std::atan2(dx, dy));
				++aimed;
			}
			return aimed;
		}

		std::uint64_t FireSinkDetour(uptr component, uptr event)
		{
			std::uint32_t shooter = 0;
			uptr who = 0;
			__try {
				who = At<uptr>(event, 0x10);
				if (who) shooter = At<std::uint32_t>(who, off::kFormId);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			if (who && shooter) {
				const uptr player = Ptr(addr::kPlayerCharacterPtr);
				for (const auto& j : g_jobs) {
					if (j.phase != Phase::Following || !player || !kProfiles[int(j.profile)].fire) continue;
					bool mine = shooter == j.npc || shooter == j.vehicle;
					if (!mine) {
						// the vehicle's weapon may fire as another object: anything within 6 m of the job's vehicle
						const uptr v = RefById(j.vehicle);
						mine = v && Dist(v, who) < 6.0 && shooter != At<std::uint32_t>(player, off::kFormId);
					}
					if (!mine) continue;
					int aimed = -1;
					__try {
						aimed = AimProjectiles(event, j, player);
					}
					__except (EXCEPTION_EXECUTE_HANDLER) {
						aimed = -1;
					}
					static ULONGLONG lastSay = 0;
					const ULONGLONG now = GetTickCount64();
					if (now - lastSay >= 1000) {
						lastSay = now;
						logger::Write("NPC vehicles: [%d] shot fired by %08X (%s); %d projectile(s) aimed at you", j.pilot, shooter,
							shooter == j.vehicle ? "the vehicle" : shooter == j.npc ? "the NPC" : "near the vehicle", aimed);
					}
					break;
				}
			}
			return g_fireOriginal(component, event);
		}
	}

	void Install()
	{
		static const std::uint8_t kPrologue[18] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE1 };
		void* original = nullptr;
		if (hook::Install(game::Base() + kSeatPlayer, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&SeatPlayerDetour), &original)) {
			g_seatPlayerOriginal = reinterpret_cast<SeatPlayer_t>(original);
			logger::Write("NPC vehicles: player-seat hook installed (FUN_14219BE30)");
		}
		else {
			logger::Write("NPC vehicles: player-seat hook FAILED (bytes differ); NPC boarding may pull the player in");
		}
		static const std::uint8_t kDismountPrologue[18] = { 0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xE8 };
		void* dis = nullptr;
		if (hook::Install(game::Base() + kDismountFn, kDismountPrologue, sizeof(kDismountPrologue), reinterpret_cast<void*>(&DismountDetour), &dis)) {
			g_dismountOriginal = reinterpret_cast<Dismount_t>(dis);
			logger::Write("NPC vehicles: dismount hook installed (following NPCs keep their seat)");
		}
		else {
			logger::Write("NPC vehicles: dismount hook FAILED (bytes differ)");
		}
		{
			auto** slot = reinterpret_cast<void**>(game::Base() + kFireSinkSlot);
			DWORD old = 0;
			if (reinterpret_cast<uptr>(*slot) == game::Base() + kFireSink && VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
				g_fireOriginal = reinterpret_cast<FireSink_t>(*slot);
				*slot = reinterpret_cast<void*>(&FireSinkDetour);
				VirtualProtect(slot, sizeof(void*), old, &old);
				logger::Write("NPC vehicles: vehicle weapon fire log hooked");
			}
			else {
				logger::Write("NPC vehicles: vehicle weapon fire sink not found (slot holds %p)", *slot);
			}
		}
		static const std::uint8_t kApplyPrologue[22] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x68, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00 };
		void* apply = nullptr;
		if (hook::Install(game::Base() + kApplyVehicleInput, kApplyPrologue, sizeof(kApplyPrologue), reinterpret_cast<void*>(&ApplyInputDetour), &apply)) {
			g_applyOriginal = reinterpret_cast<ApplyInput_t>(apply);
			logger::Write("NPC vehicles: vehicle input hook installed (NPC vehicle weapons)");
		}
		else {
			logger::Write("NPC vehicles: vehicle input hook FAILED (bytes differ); NPC vehicles cannot fire");
		}
		logger::Write("NPC vehicles: aggro key . (following NPC drivers attack you / follow again)");
	}

	bool UsesVehicle(std::uint32_t vehicleId) { return vehicleId && Busy(vehicleId); }

	const char* ProfileOf(std::uint32_t vehicleId)
	{
		for (const auto& j : g_jobs) {
			if (vehicleId && j.vehicle == vehicleId && j.phase == Phase::Following) return kProfiles[int(j.profile)].name;
		}
		return nullptr;
	}

	void Reset()
	{
		for (auto& j : g_jobs) j = Job{};
		g_seatLogged.clear();
		g_told = false;
	}
}
