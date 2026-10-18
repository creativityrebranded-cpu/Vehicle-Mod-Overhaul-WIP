#include "npcvehicles.h"

#include "addresses.h"
#include "controlmap.h"
#include "crash.h"
#include "game.h"
#include "hook.h"
#include "hoverui.h"
#include "hoverui.h"
#include "log.h"
#include "modscene.h"
#include "notify.h"
#include "passenger.h"
#include "pilotquest.h"
#include "vcontract.h"
#include "vehicleflight.h"

#include <Windows.h>
#include <intrin.h>

#include <cmath>
#include <mutex>
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
		constexpr double kGiveUp = 300.0;        // m: farther than this from the player ends following when the job has no origin (never, normally)
		constexpr double kLeash = 600.0;         // m: following ends when the vehicle is this far from where the NPC started (its origin: an outpost, wherever it stood)
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
			bool standing = false;           // the get-in only stood the NPC up: ask again shortly
			bool pkgSampled = false;         // the 1 s package check of this boarding was logged
			bool reasked = false;            // the get-in was asked a second time (its package was gone at 1 s)
			bool moved = false;                // moved next to the vehicle (step 3)
			double startDist = 0;              // NPC -> vehicle when boarding began
			ULONGLONG directAt = 0;
			double tx = 0, ty = 0, tvx = 0, tvy = 0;  // the followed target's last position and smoothed velocity
			ULONGLONG aggroAt = 0;             // last combat request (attack)
			Profile profile = Profile::Escort;
			bool gang = false;                 // started by the gang (mirrors the player), not by the force key
			double originX = 0, originY = 0;   // where the NPC stood when the job began (the leash is measured from here)
			bool haveOrigin = false;
			bool passenger = false;            // boarding as a passenger (seat index `seat` >= 1): once aboard it moves to g_riders and the slot is freed
			unsigned seat = 0;
			bool cmd = false;                  // sent with the game's own follower command (Walk key): no instant-seat fallback
			ULONGLONG cmdLog = 0;
			unsigned cmdTries = 0;              // get-in attempts after arriving
			double arrX = 0, arrY = 0;          // where the NPC last moved from (arrival = it stopped walking)
			bool cmdInstant = false;            // the instant-seat fallback was used / the no-path give-up was taken
			bool forcedRun = false;             // we set the actor's force-run bit for the walk (combat); cleared again afterwards
			ULONGLONG arrStill = 0;
			ULONGLONG cmdTryAt = 0;
			bool driving = false;              // drive mode controls are being submitted
			bool firing = false;               // the vehicle weapon's fire input is held
			ULONGLONG fireAt = 0;              // when the current burst / gap started
			unsigned bursts = 0;
			double aimX = 0, aimY = 0, aimZ = 0, aimVx = 0, aimVy = 0, aimVz = 0;  // the player's last position and smoothed velocity (aim lead)
			ULONGLONG aimAt = 0;
			ULONGLONG tAt = 0;
		} g_jobs[kJobs];
		// Boarding steps (2026-10-17 15:21-15:22: the walk package was accepted, board -> 1, but 5 of 6 NPCs never moved):
		// 1 walk (FUN_141951FB0); 2 no 2 m of progress after kWalkMs: direct board (FUN_141993710); 3 still out kDirectMs
		// later: moved next to the vehicle and direct-boarded again.
		constexpr ULONGLONG kWalkMs = 5000, kDirectMs = 4000;
		constexpr ULONGLONG kStandMs = 1500;  // a sitting NPC asked to get in stands first; asked again after this
		bool g_told = false;
		// Aggro (the user, 2026-10-17: "a button to make them aggro so they can shoot at me with the vehicle"): the
		// period key turns every following NPC driver hostile; they keep their seat, close to an orbit around the player
		// facing them, and the vehicle weapon's fire events are logged (the weapon's own fire call is not hooked yet).
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
		constexpr double kAttackAbove = 6.0;       // m: attackers fly this far over the player's height
		constexpr double kAttackRun = 70.0;        // m: closer than this the attack controls take over from the autopilot
		constexpr ULONGLONG kReaggroMs = 5000;     // combat re-asked this often while out of combat
		bool g_traceSeat = false;                  // log the seat check's helpers while one of OUR get-in calls runs
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
		bool g_keyChosen = false;
		int g_walkKey = 0, g_exitKey = 0, g_aggroKey = 0;
		std::uint32_t g_ctrlTarget = 0;
		bool g_walkDown = false, g_exitDown = false;
		// The led aim point of each job that may fire, for vehicleaim's projectile launch hook (any thread): refreshed by
		// Update() every frame while attacking; older than kAimSnapMs counts as gone.
		struct AimSnap
		{
			std::uint32_t vehicle = 0, npc = 0;
			double x = 0, y = 0, z = 0;
			bool fire = false;
			ULONGLONG at = 0;
		};
		std::mutex g_aimLock;
		AimSnap g_aimSnap[8];
		constexpr ULONGLONG kAimSnapMs = 500;
		void StoreAim(int slot, const AimSnap& a)  // (its own function: Update has __try blocks, no unwinding there)
		{
			std::lock_guard lk(g_aimLock);
			g_aimSnap[slot] = a;
		}

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
		uptr DriverSeatKeyword(uptr vehicle);
		bool GiveKeyword(uptr ref, uptr kw);
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

		// The actor's current package type (process +8 middle-high +0x40 package, its +0x3C type; else process +0x20) and
		// IsInCombat (vtable +0xB58), for the boarding check. -1 when unreadable.
		void PackageState(uptr actor, int& type, int& combat)
		{
			type = combat = -1;
			__try {
				const uptr proc = At<uptr>(actor, 0x228);
				uptr pkg = 0;
				if (proc) {
					const uptr mh = At<uptr>(proc, 8);
					pkg = mh ? At<uptr>(mh, 0x40) : 0;
					if (!pkg) pkg = At<uptr>(proc, 0x20);
				}
				if (pkg) type = At<std::uint8_t>(pkg, 0x3C);
				combat = reinterpret_cast<bool (*)(uptr)>((*reinterpret_cast<void***>(actor))[0xB58 / 8])(actor) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		// What FUN_141951FB0(actor, vehicle) really does (2026-10-17 evening, decompile; the 19:56 log: one NPC got
		// "get-in -> 1" and never moved, another "-> 0"):
		//  - Actor +0x378 bit 3 ("getting on / off") set: the package push (Actor vtable +0xB38 = FUN_1418D9EA0) returns at
		//    once, yet FUN_141951FB0 still says 1. The game clears the bit when a mount completes (FUN_141993710 /
		//    FUN_141A03F70), is cancelled (FUN_141995310) or times out (FUN_141A098E0). A stale one is cleared here first,
		//    the way FUN_141995310 does it (bit off, the middle-high +0x4D8 mount timer to -FLT_MAX).
		//  - Sitting / in furniture (the state object at actor +0xF0, vfunc +0x1A0, not 0): it only makes the actor get up
		//    (vtable +0xB30) and says 1, no package. Asked again once standing.
		//  - Process level 0: the get-in package (type 0x29), unless no seat entry is reachable (FUN_141A05D40: says 0).
		//    Never followed by EvaluatePackage: with immediate = 1 that clears the process's package stack
		//    (FUN_141A07C90) and picks the actor's own package again (FUN_1419B5E40); only a type 0x27 package survives.
		//    That threw away every get-in package since 16:52.
		//  - Level 1: seats at once (FUN_141995460 + FUN_141993710 snap) and returns whether the vehicle has a passenger
		//    keyword, 0 for a REV-8 even when seated. Level 2+: only builds the interaction and always says 0.
		// Returns 1 walking (package given), 2 standing up (ask again), 3 seated, 0 refused, -1 fault.
		// SetForceRun / GetForceRun (console handlers FUN_140C3A040 / FUN_140C39FA0): actor +0xF8 bit 26. Used so an NPC RUNS to the vehicle in combat and
		// walks (the package's own pace) otherwise.
		bool GetForceRun(uptr actor) { return (At<std::uint32_t>(actor, 0xF8) & 0x4000000u) != 0; }
		void SetForceRun(uptr actor, bool on)
		{
			auto& f = At<std::uint32_t>(actor, 0xF8);
			f = on ? (f | 0x4000000u) : (f & ~0x4000000u);
		}

		// The seat keyword the seat check needs on the ACTOR (seat trace 11:11: without it FUN_1412B2C40 and FUN_1412B2A90 return null and the check refuses).
		bool EnsureSeatKeyword(uptr actor, uptr vehicle)
		{
			if (CanMount(actor, vehicle)) return true;
			const uptr kw = DriverSeatKeyword(vehicle);
			const bool gave = kw && GiveKeyword(actor, kw);
			logger::Write("NPC vehicles: NPC %08X lacks the vehicle's seat keyword; %s %08X", At<std::uint32_t>(actor, off::kFormId), gave ? "gave" : "could NOT give", kw ? At<std::uint32_t>(kw, off::kFormId) : 0);
			return gave;
		}

		int Mount(uptr actor, uptr vehicle)
		{
			const uptr proc = At<uptr>(actor, 0x228);
			const int level = proc ? At<std::uint8_t>(proc, 0xE7) : 0xFF;
			int sit = 0;
			bool r = false;
			__try {
				const uptr state = actor + 0xF0;
				sit = reinterpret_cast<int (*)(uptr)>((*reinterpret_cast<void***>(state))[0x1A0 / 8])(state);
				auto& flags = At<std::uint32_t>(actor, 0x378);
				if ((flags & 8) && InVehicle(actor) != vehicle) {
					flags &= ~8u;
					if (proc && level < 2 && At<uptr>(proc, 8)) At<std::uint32_t>(At<uptr>(proc, 8), 0x4D8) = 0xFF7FFFFF;
					logger::Write("NPC vehicles: NPC %08X had a stale get-on flag; cleared", At<std::uint32_t>(actor, off::kFormId));
				}
				// SEAT TRACE 11:11: FUN_1412B2C40 (the actor's seat data) and its fallback FUN_1412B2A90 both returned null, so FUN_141A05D40 stopped at the
				// first step. FUN_1412B2C40 walks the vehicle base's seat table (+0x278, count +0x270, mask +0x264) and asks the ACTOR whether it has each seat's
				// keyword (actor +0x68). So the actor needs the seat keyword first, as StartJob already did (the quest-walk flow skipped it).
				EnsureSeatKeyword(actor, vehicle);
				g_traceSeat = true;
				r = EngineFn<bool (*)(uptr, uptr)>(kMountActor)(actor, vehicle);
				g_traceSeat = false;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				g_traceSeat = false;
				return -1;
			}
			int out = 0;
			if (InVehicle(actor) == vehicle) out = 3;
			else if (level == 0 && sit != 0) out = 2;
			else if (level == 0 && r) out = 1;
			else if (level >= 1 && level != 0xFF) {
				// Level 1 said "no passenger keyword", level 2+ never seats: the game's instant seat.
				int seated = -1;
				InstantSeat(actor, vehicle, seated);
				out = InVehicle(actor) == vehicle ? 3 : 0;
			}
			logger::Write("NPC vehicles: get-in NPC %08X -> vehicle %08X: process level %d, sit state %d, game says %d -> %s", At<std::uint32_t>(actor, off::kFormId),
				At<std::uint32_t>(vehicle, off::kFormId), level, sit, r ? 1 : 0,
				out == 3 ? "seated" : out == 2 ? "standing up first" : out == 1 ? "walking to it" : "refused (no reachable seat)");
			if (out == 0) {
				// Why? FUN_141A05D40 needs (1) a seat entry point on the vehicle, (2) a navmesh path from the actor to it, (3) a facing angle within limits.
				// What can be measured from here: heights, the vehicle's tilt and heading against the line to the actor.
				const auto ap = At<game::Vec3>(actor, off::kRefPosition), vp = At<game::Vec3>(vehicle, off::kRefPosition);
				const auto vr = At<game::Vec3>(vehicle, off::kRefRotation);
				double toActor = std::atan2(double(ap.x) - vp.x, double(ap.y) - vp.y) - (1.5707963 - vr.z);
				while (toActor > 3.14159265) toActor -= 6.28318531;
				while (toActor < -3.14159265) toActor += 6.28318531;
				logger::Write("NPC vehicles: refused: NPC z %.1f, vehicle z %.1f (vehicle %.1f m %s the NPC), horizontal %.1f m; vehicle pitch %.1f roll %.1f deg; NPC is %.0f deg off the vehicle's nose",
					ap.z, vp.z, std::fabs(double(vp.z) - ap.z), vp.z > ap.z ? "above" : "below", std::hypot(double(ap.x) - vp.x, double(ap.y) - vp.y), vr.x * 57.2958, vr.y * 57.2958, toActor * 57.2958);
			}
			return out;
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

		std::uint32_t OwnerId(uptr ref);
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
		double Dist3(uptr a, uptr b)
		{
			const auto p = At<game::Vec3>(a, off::kRefPosition), q = At<game::Vec3>(b, off::kRefPosition);
			const double dx = double(p.x) - q.x, dy = double(p.y) - q.y, dz = double(p.z) - q.z;
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		}

		double Dist(uptr a, uptr b)
		{
			const auto p = At<game::Vec3>(a, off::kRefPosition), q = At<game::Vec3>(b, off::kRefPosition);
			return std::hypot(double(p.x) - q.x, double(p.y) - q.y);
		}

		void ChooseKey()
		{
			if (g_keyChosen) return;
			g_keyChosen = true;
			// Test keys only (the context engine does this job): board the faced NPC, exit all, escort <-> pursuit.
			if (!controlmap::DebugKeys()) return;
			g_walkKey = controlmap::ClaimFirst("npcveh", "board (debug)", { VK_OEM_1, VK_OEM_7, VK_OEM_4 });  // ; first
			g_exitKey = controlmap::Claim("npcveh", "exit (debug)", VK_INSERT) ? VK_INSERT : 0;
			g_aggroKey = controlmap::Claim("npcveh", "escort/pursuit (debug)", VK_OEM_PERIOD) ? VK_OEM_PERIOD : 0;
			logger::Write("NPC vehicles: debug keys: BOARD %s, EXIT %s, PROFILE %s", g_walkKey ? controlmap::KeyName(g_walkKey).c_str() : "none",
				g_exitKey ? controlmap::KeyName(g_exitKey).c_str() : "none", g_aggroKey ? controlmap::KeyName(g_aggroKey).c_str() : "none");
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
			if (p == Phase::Idle && j.vehicle && !j.passenger) {  // the job is over: an escort / pursuit assignment ends with it (other profiles are left alone)
				const vcontract::Profile* ap = vcontract::AssignedTo(j.vehicle);
				if (ap && (std::strcmp(ap->name, "escort") == 0 || std::strcmp(ap->name, "pursuit") == 0)) vcontract::Unassign(j.vehicle);
			}
			if (p == Phase::Boarding) {
				j.cmdTries = 0;
				j.cmdTryAt = 0;
				j.cmdInstant = false;
				const uptr n = RefById(j.npc), v = RefById(j.vehicle);
				j.startDist = n && v ? Dist(n, v) : 0.0;
			}
			if (j.forcedRun && p != Phase::Boarding) {
				if (const uptr n0 = RefById(j.npc)) SetForceRun(n0, false);
				j.forcedRun = false;
			}
			j.nearSince = 0;
			j.parked = false;
			j.relaunchAt = 0;
			j.stillSince = 0;
			if (j.cmd && p != Phase::Boarding) pilotquest::Release(j.pilot - 1);
			if (p == Phase::Idle) {
				j.gang = false;
				j.cmd = false;
				j.directTried = false;
				j.standing = false;
				j.pkgSampled = false;
				j.reasked = false;
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

		bool PlaceBeside(uptr actor, uptr vehicle, int kind, double dist)
		{
			const uptr cell = At<uptr>(vehicle, off::kRefParentCell);
			const uptr ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
			if (!cell) return false;
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double h = At<game::Vec3>(vehicle, off::kRefRotation).z;  // heading, forward = (sin h, cos h)
			const double fx = std::sin(h), fy = std::cos(h), rx = std::cos(h), ry = -std::sin(h);
			double ox = 0, oy = 0;
			if (kind == 0) { ox = rx; oy = ry; }
			else if (kind == 1) { ox = -rx; oy = -ry; }
			else if (kind == 2) { ox = fx; oy = fy; }
			else { ox = -fx; oy = -fy; }
			const double px = vp.x + ox * dist, py = vp.y + oy * dist;
			float pos[3] = { float(px), float(py), vp.z + 0.2f };
			float rot[3] = { 0.0f, 0.0f, float(std::atan2(vp.x - px, vp.y - py)) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(actor, &handle, cell, ws, pos, rot);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Puts the actor at an exact place (MoveReference): the test's fixed start, a spot the NPC stood on at the start (so it is on the navmesh).
		bool PlaceAt(uptr actor, uptr cell, float x, float y, float z, float yaw)
		{
			if (!cell) return false;
			const uptr ws = At<uptr>(cell, off::kCellWorldspace);
			float pos[3] = { x, y, z };
			float rot[3] = { 0.0f, 0.0f, yaw };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(actor, &handle, cell, ws, pos, rot);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void Leave(Job& j, uptr npc, const char* why);
		bool ApplyProfile(Job& j, Profile to, uptr player);
		// escort / pursuit are profiles of the shared table (profiles.cpp); an assignment of one of them decides the job's behaviour.
		void SyncProfile(Job& j, uptr player)
		{
			const vcontract::Profile* ap = vcontract::AssignedTo(j.vehicle);
			if (!ap) return;
			const Profile want = std::strcmp(ap->name, "pursuit") == 0 ? Profile::Pursuit : std::strcmp(ap->name, "escort") == 0 ? Profile::Escort : j.profile;
			if (want != j.profile) ApplyProfile(j, want, player);
		}

		bool HasSeatInteraction(uptr npc)
		{
			__try {
				return EngineFn<char (*)(uptr, int)>(0x002C5C90)(At<uptr>(npc, 0xC8), 0xA5) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The animated entry (the Activate quest package; the game's mount package starts but the NPC only reaches the door in some spots) is PAUSED
		// (2026-10-18, user): on arrival the instant seat is used. Set true to try the animation first again.
		constexpr bool kAnimatedEntry = false;
		constexpr ULONGLONG kAnimWaitMs = 6000;   // how long the animated entry may take before the instant seat
		constexpr double kSnapRange = 8.0;         // m: the instant seat is only used this close to the vehicle

		void LogCmdState(const Job& j, uptr npc, uptr vehicle);

		// ---- Passenger seats. A passenger boards like a driver (the quest-package walk, the instant seat) but with the keyword of a seat index >= 1 of the
		// vehicle's seat table; once seated its job slot is freed and it is kept here until it gets out. Seat 0 is the driver's.
		struct Rider { std::uint32_t npc, vehicle; unsigned seat; ULONGLONG since; };
		std::vector<Rider> g_riders;

		// The first free passenger seat index of the vehicle (>= 1) and its keyword; 0 when the vehicle has one seat or every seat is taken.
		int PickPassengerSeat(uptr vehicle, uptr& kwOut)
		{
			const uptr base = At<uptr>(vehicle, 0x98);
			if (!base) return 0;
			std::uint32_t mask = 0, count = 0;
			uptr kw = 0;
			if (!ReadSeat(base, 0, mask, count, kw) || count < 2) return 0;
			const std::uint32_t vid = At<std::uint32_t>(vehicle, off::kFormId);
			unsigned taken = 1;  // the driver's seat
			for (const auto& r : g_riders) {
				if (r.vehicle == vid && r.seat < 32) taken |= 1u << r.seat;
			}
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.passenger && j.vehicle == vid && j.seat < 32) taken |= 1u << j.seat;
			}
			unsigned aboard = 0;
			for (const uptr a : modscene::LoadedRefs(2000)) {
				if (IsActorRef(a) && InVehicle(a) == vehicle) ++aboard;
			}
			if (aboard >= count) return 0;
			for (unsigned i = 1; i < count && i < 32; ++i) {
				if ((taken >> i) & 1u) continue;
				if (!ReadSeat(base, i, mask, count, kw) || !kw || At<std::uint8_t>(kw, 0x2E) != 4) continue;
				kwOut = kw;
				return int(i);
			}
			return 0;
		}

		// The seat keyword the seat check needs on the actor: the driver's for a driver job, the job's passenger seat's for a passenger job.
		void EnsureSeat(const Job& j, uptr actor, uptr vehicle)
		{
			if (!j.passenger) {
				EnsureSeatKeyword(actor, vehicle);
				return;
			}
			const uptr base = At<uptr>(vehicle, 0x98);
			std::uint32_t mask = 0, count = 0;
			uptr kw = 0;
			const bool read = base && ReadSeat(base, j.seat, mask, count, kw) && kw;
			const bool gave = read && GiveKeyword(actor, kw);
			logger::Write("NPC vehicles: [%d] NPC %08X passenger seat %u of vehicle %08X: keyword %08X %s", j.pilot, At<std::uint32_t>(actor, off::kFormId), j.seat,
				At<std::uint32_t>(vehicle, off::kFormId), kw ? At<std::uint32_t>(kw, off::kFormId) : 0, gave ? "given" : "NOT given");
		}

		void RidersTick()
		{
			const ULONGLONG now = GetTickCount64();
			for (auto it = g_riders.begin(); it != g_riders.end();) {
				const uptr npc = RefById(it->npc), veh = RefById(it->vehicle);
				const bool gone = !npc || !veh || Dead(npc) || (now - it->since > 3000 && InVehicle(npc) != veh);
				if (gone) {
					logger::Write("NPC vehicles: passenger %08X left vehicle %08X (seat %u)", it->npc, it->vehicle, it->seat);
					it = g_riders.erase(it);
				}
				else {
					++it;
				}
			}
		}

		bool Busy(std::uint32_t id)
		{
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && (j.npc == id || j.vehicle == id)) return true;
			}
			for (const auto& r : g_riders) {
				if (r.npc == id) return true;
			}
			return false;
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
			crash::Crumb("npc calm", int(At<std::uint32_t>(actor, off::kFormId)));
			__try {
				EngineFn<char (*)(uptr, uptr, uptr)>(kStopCombatCmd)(0, 0, actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		bool Aggro(uptr actor, uptr target)
		{
			crash::Crumb("npc aggro", int(At<std::uint32_t>(actor, off::kFormId)));
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
			// The input block (handler + 0x80; FUN_141324650): +0x10 steer, +0x14 accelerate, +0x18 brake, +0x1C forward
			// boost, +0x1E vertical boost, +0x1F fire. Drive mode (vehicleflight) fills the movement fields.
			vehicleflight::DriveInput di;
			if (vehicleflight::DriveFor(vehicleId, di)) {
				*reinterpret_cast<float*>(h + 0x90) = di.steer;
				*reinterpret_cast<float*>(h + 0x94) = di.accel;
				*reinterpret_cast<float*>(h + 0x98) = di.brake;
				h[0x9C] = di.boost ? 1 : 0;
				h[0x9E] = di.vert ? 1 : 0;
			}
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

		// A vehicle in drive mode gets its controls every tick (and one zero block when drive mode ends).
		void PushDrive(Job& j)
		{
			vehicleflight::DriveInput di;
			const bool drv = vehicleflight::DriveFor(j.vehicle, di);
			if (drv || j.driving) {
				SubmitFire(j.pilot - 1, j.vehicle, false);
				j.driving = drv;
			}
		}

		double WrapAngle(double a)
		{
			while (a > 3.14159265358979) a -= 6.28318530717959;
			while (a < -3.14159265358979) a += 6.28318530717959;
			return a;
		}

		// The aim point: the player led by their velocity over the shot's flight time, at chest height.
		void AimPoint(Job& j, uptr player, uptr vehicle, double& ax, double& ay, double& az)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const ULONGLONG now = GetTickCount64();
			if (j.aimAt && now > j.aimAt) {
				const double dt = (now - j.aimAt) / 1000.0;
				const double ix = (pp.x - j.aimX) / dt, iy = (pp.y - j.aimY) / dt, iz = (pp.z - j.aimZ) / dt;
				if (dt < 1.0 && std::sqrt(ix * ix + iy * iy + iz * iz) < 150.0) {
					const double a = std::min(1.0, dt * 4.0);
					j.aimVx += (ix - j.aimVx) * a;
					j.aimVy += (iy - j.aimVy) * a;
					j.aimVz += (iz - j.aimVz) * a;
				}
				else {
					j.aimVx = j.aimVy = j.aimVz = 0;
				}
			}
			j.aimX = pp.x; j.aimY = pp.y; j.aimZ = pp.z; j.aimAt = now;
			const double dz = double(pp.z) + kAimHeight - vp.z;
			const double dist = std::sqrt(std::pow(double(pp.x) - vp.x, 2) + std::pow(double(pp.y) - vp.y, 2) + dz * dz);
			const double t = dist / kShotSpeed;
			ax = pp.x + j.aimVx * t;
			ay = pp.y + j.aimVy * t;
			az = pp.z + kAimHeight + j.aimVz * t;
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
			c.pitch = std::clamp(std::atan2(double(pp.z) - vp.z, std::max(dist, 5.0)), -0.6, 0.6);
			c.gear = 2;
			c.holdMs = 300;
			vehicleflight::PilotCommand(j.pilot, c);
		}

		// ---- Exit (2026-10-18). The game's dismount FUN_141952530(actor) mirrors the mount: it refuses (returns 0) when actor +0x378 bit 1 is set, when the
		// actor has no seat interaction (extra 0xA5), when bit 3 ("getting on / off") is set, and at process level above 1 it does the instant exit
		// FUN_141A9C530 and still returns 0. Otherwise it pushes a runtime package of type 0x2A (dismount) which the AI may drop like the mount one.
		// Past runs: "dismount (parked) -> 0" at 20:01 and 21:03 left the NPC aboard. So an exit is: ask the game's dismount (animated), watch it, and fall
		// back to the instant exit; afterwards make sure the NPC does not stand inside the vehicle.
		constexpr ULONGLONG kExitWatchMs = 6000;  // the animated exit gets this long before the instant exit is used

		bool InstantExit(uptr npc)
		{
			g_allowDismount = true;
			__try {
				EngineFn<void (*)(uptr)>(0x01A9C530)(npc);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				g_allowDismount = false;
				return false;
			}
			g_allowDismount = false;
			return true;
		}

		// Out of the vehicle but overlapping it (the instant exit leaves the actor at the seat): put it beside the vehicle.
		void ClearOfVehicle(uptr npc, uptr vehicle, int pilot)
		{
			if (!npc || !vehicle || InVehicle(npc)) return;
			if (Dist(npc, vehicle) < 2.2) {
				const bool ok = PlaceBeside(npc, vehicle, 0, 3.0);
				logger::Write("NPC vehicles: [%d] exit: NPC %08X was %.1f m from the vehicle centre; placed beside it (%s)", pilot, At<std::uint32_t>(npc, off::kFormId), Dist(npc, vehicle), ok ? "ok" : "FAILED");
			}
		}

		void Leave(Job& j, uptr npc, const char* why)
		{
			const uptr vehicle = RefById(j.vehicle);
			if (!npc) {
				Set(j, Phase::Idle);
				return;
			}
			const std::uint32_t flags = At<std::uint32_t>(npc, 0x378);
			const uptr proc = At<uptr>(npc, 0x228);
			const int level = proc ? At<std::uint8_t>(proc, 0xE7) : -1;
			const bool interaction = HasSeatInteraction(npc);
			g_allowDismount = true;
			const int r = Dismount(npc);
			g_allowDismount = false;
			logger::Write("NPC vehicles: [%d] dismount (%s) -> %d  [flags %08X: bit1 %u, getting on/off %u; seat interaction %s; process level %d]", j.pilot, why, r, flags, (flags >> 1) & 1, (flags >> 3) & 1,
				interaction ? "yes" : "NO", level);
			if (r == 1) {
				Set(j, Phase::Leaving);  // the animated exit was started; the Leaving case watches it and falls back to the instant exit
				return;
			}
			// Refused (or fault): the instant exit now.
			const bool ok = InstantExit(npc);
			logger::Write("NPC vehicles: [%d] exit: dismount refused; instant exit %s; NPC %s the vehicle", j.pilot, ok ? "called" : "FAULTED", InVehicle(npc) ? "is STILL in" : "left");
			ClearOfVehicle(npc, vehicle, j.pilot);
			Set(j, Phase::Idle);
		}


		// ---- The exit service (2026-10-18; spec from Profile Fork, vcontract.h ExitRequest / SeatState; polled, no callbacks) ----
		// One task per actor. Stages: 1 wait for the vehicle to be landed (Normal / Auto with waitLanded), 2 the game's animated dismount watched for
		// kExitWatchMs, 3 the same with a short watch (Quick), 4 the instant exit. Auto = 1 -> 2 -> 4, Normal = 1 -> 2 (then Refused), Quick = 3 (Refused),
		// Instant = 4. "Landed" is a placeholder until the flight code offers one: the vehicle moved under 0.5 m/s for 1 s.
		// Finished: the actor is placed (the request's point, else beside the vehicle on the side away from the player); a job's driver ends its job,
		// stops the autopilot and the vehicle is Unassigned from its profile (parked).
		struct ExitTask
		{
			bool used = false;
			std::uint32_t npc = 0, veh = 0;
			vcontract::ExitRequest req;
			int stage = 0;
			ULONGLONG t0 = 0, tStage = 0, stillSince = 0, lastAsk = 0;
			double lx = 0, ly = 0, lz = 0;
			bool refused = false;  // finished with the actor still aboard (the instant exit failed too)
		};
		constexpr int kExitTasks = 12;
		ExitTask g_exitTasks[kExitTasks];
		std::uint32_t g_exitRefused[8] = {};  // actors whose last exit was Refused (polled by ExitStateOf)

		ExitTask* ExitTaskFor(std::uint32_t npcId)
		{
			for (auto& t : g_exitTasks) {
				if (t.used && t.npc == npcId) return &t;
			}
			return nullptr;
		}

		void MarkRefused(std::uint32_t id, bool on)
		{
			for (auto& r : g_exitRefused) {
				if (on && !r) { r = id; return; }
				if (!on && r == id) r = 0;
			}
		}

		// Beside the vehicle on the side away from the player (or at the request's point).
		void PlaceAfterExit(uptr npc, uptr vehicle, const vcontract::ExitRequest& req)
		{
			const bool hasPoint = !std::isnan(req.x) && !std::isnan(req.y);
			if (hasPoint) {
				const uptr cell = At<uptr>(vehicle, off::kRefParentCell);
				const float z = std::isnan(req.z) ? At<game::Vec3>(npc, off::kRefPosition).z : req.z;
				const bool ok = PlaceAt(npc, cell, req.x, req.y, z, At<game::Vec3>(npc, off::kRefRotation).z);
				logger::Write("NPC vehicles: exit: NPC %08X placed at the requested point (%.0f, %.0f) (%s)", At<std::uint32_t>(npc, off::kFormId), req.x, req.y, ok ? "ok" : "FAILED");
				return;
			}
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double h = At<game::Vec3>(vehicle, off::kRefRotation).z;
			const double rx = std::cos(h), ry = -std::sin(h);
			int kind = 0;  // 0 right, 1 left: the one farther from the player
			if (player) {
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				const double dR = std::hypot(vp.x + rx * 3.0 - pp.x, vp.y + ry * 3.0 - pp.y), dL = std::hypot(vp.x - rx * 3.0 - pp.x, vp.y - ry * 3.0 - pp.y);
				kind = dL > dR ? 1 : 0;
			}
			const bool ok = PlaceBeside(npc, vehicle, kind, 3.0);
			logger::Write("NPC vehicles: exit: NPC %08X placed %s of the vehicle, away from you (%s)", At<std::uint32_t>(npc, off::kFormId), kind ? "left" : "right", ok ? "ok" : "FAILED");
		}

		void FinishExit(ExitTask& t, uptr npc, uptr vehicle, bool out)
		{
			if (out) {
				PlaceAfterExit(npc, vehicle, t.req);
				MarkRefused(t.npc, false);
			}
			else {
				MarkRefused(t.npc, true);
			}
			for (auto& j : g_jobs) {
				if (j.phase == Phase::Idle || j.npc != t.npc) continue;
				if (out) {
					vcontract::Unassign(j.vehicle);  // the driver left: the vehicle is parked, no profile drives it
					Set(j, Phase::Idle);
				}
			}
			logger::Write("NPC vehicles: exit task for NPC %08X finished after %.1f s: %s", t.npc, (GetTickCount64() - t.t0) / 1000.0, out ? "OUT" : "REFUSED (still aboard)");
			t.used = false;
		}

		void ExitTick()
		{
			const ULONGLONG now = GetTickCount64();
			for (auto& t : g_exitTasks) {
				if (!t.used) continue;
				const uptr npc = RefById(t.npc), vehicle = RefById(t.veh);
				if (!npc || !vehicle) {
					t.used = false;
					continue;
				}
				if (InVehicle(npc) != vehicle) {
					FinishExit(t, npc, vehicle, true);
					continue;
				}
				if (t.stage == 1) {
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					const double moved = std::sqrt(std::pow(double(vp.x) - t.lx, 2) + std::pow(double(vp.y) - t.ly, 2) + std::pow(double(vp.z) - t.lz, 2));
					t.lx = vp.x; t.ly = vp.y; t.lz = vp.z;
					if (moved > 0.01 * 0.5 * 16.0) t.stillSince = now;  // > ~0.08 m per frame is "moving" (0.5 m/s at 16 ms)
					if (!t.stillSince) t.stillSince = now;
					const bool landed = !t.req.waitLanded || now - t.stillSince >= 1000;
					if (landed || now - t.tStage > t.req.timeoutMs) {
						if (!landed) logger::Write("NPC vehicles: exit: NPC %08X: the vehicle did not land within %u ms; going on", t.npc, t.req.timeoutMs);
						t.stage = 2;
						t.tStage = now;
					}
					else {
						continue;
					}
				}
				if (t.stage == 2 || t.stage == 3) {
					if (!t.lastAsk) {
						t.lastAsk = now;
						if (t.stage == 3) {
							// Quick: the game's dismount like Normal, with the stale mounting flag cleared first and a short watch.
							auto& f = At<std::uint32_t>(npc, 0x378);
							if (f & 8) f &= ~8u;
						}
						g_allowDismount = true;
						const int r = Dismount(npc);
						g_allowDismount = false;
						{
							const std::uint32_t fl = At<std::uint32_t>(npc, 0x378);
							const uptr pr = At<uptr>(npc, 0x228);
							logger::Write("NPC vehicles: exit: NPC %08X dismount (%s) -> %d  [flags %08X: bit1 %u, getting on/off %u; seat interaction %s; process level %d]", t.npc, t.stage == 3 ? "quick" : "normal", r, fl,
								(fl >> 1) & 1, (fl >> 3) & 1, HasSeatInteraction(npc) ? "yes" : "NO", pr ? int(At<std::uint8_t>(pr, 0xE7)) : -1);
						}
						if (r != 1) t.stage = 4;  // refused outright: no point watching
					}
					else if (now - t.lastAsk > (t.stage == 3 ? 2500u : unsigned(kExitWatchMs))) {
						t.stage = 4;
					}
					if (t.stage != 4) continue;
					if (t.req.mode == vcontract::ExitMode::Normal || t.req.mode == vcontract::ExitMode::Quick) {
						FinishExit(t, npc, vehicle, false);  // a fixed mode reports Refused at its end; the profile decides
						continue;
					}
				}
				if (t.stage == 4) {
					const bool ok = InstantExit(npc);
					logger::Write("NPC vehicles: exit: NPC %08X instant exit %s; NPC %s the vehicle", t.npc, ok ? "called" : "FAULTED", InVehicle(npc) == vehicle ? "is STILL in" : "left");
					FinishExit(t, npc, vehicle, InVehicle(npc) != vehicle);
				}
			}
		}

		// Starts (or replaces) the exit of `npc`; false when it is not aboard a vehicle.
		bool StartExit(uptr npc, const vcontract::ExitRequest& req)
		{
			const uptr vehicle = InVehicle(npc);
			if (!npc || !vehicle) return false;
			const std::uint32_t id = At<std::uint32_t>(npc, off::kFormId);
			ExitTask* t = ExitTaskFor(id);
			if (!t) {
				for (auto& x : g_exitTasks) {
					if (!x.used) { t = &x; break; }
				}
			}
			if (!t) return false;
			*t = ExitTask{};
			t->used = true;
			t->npc = id;
			t->veh = At<std::uint32_t>(vehicle, off::kFormId);
			t->req = req;
			t->t0 = t->tStage = GetTickCount64();
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			t->lx = vp.x; t->ly = vp.y; t->lz = vp.z;
			t->stage = req.mode == vcontract::ExitMode::Instant ? 4 : req.mode == vcontract::ExitMode::Quick ? 3 : (req.waitLanded ? 1 : 2);
			// A job's driver: the autopilot stops (the flight code lands the vehicle); the job is ended when the exit finishes.
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.npc == id && j.phase == Phase::Following) vehicleflight::PilotStop(j.pilot, "exit requested");
			}
			logger::Write("NPC vehicles: exit requested for NPC %08X in vehicle %08X: mode %d, wait landed %s, timeout %u ms, target %s", id, t->veh, int(req.mode), req.waitLanded ? "yes" : "no", req.timeoutMs,
				(!std::isnan(req.x) && !std::isnan(req.y)) ? "a point" : "beside the vehicle");
			return true;
		}

		// The exit key: Auto (wait landed, the game's dismount, then the instant exit) for every NPC aboard a job vehicle.
		void ExitAllJobs()
		{
			int n = 0;
			for (auto& jj : g_jobs) {
				if (jj.phase == Phase::Idle) continue;
				const uptr nn = RefById(jj.npc);
				if (nn && InVehicle(nn)) n += StartExit(nn, vcontract::ExitRequest{}) ? 1 : 0;
				else if (nn) Set(jj, Phase::Idle);
			}
			notify::Show(n ? "Exit requested for %d NPC(s)" : "No NPC aboard a job vehicle", n);
		}

		void Update(Job& j, uptr player)
		{
			const ULONGLONG now = GetTickCount64();
			if (ExitTaskFor(j.npc)) return;  // an exit request owns this NPC until it finishes
			const uptr npc = RefById(j.npc), vehicle = RefById(j.vehicle);
			if (!npc || !vehicle) {
				logger::Write("NPC vehicles: [%d] %s lost", j.pilot, !npc ? "NPC" : "vehicle");
				vehicleflight::PilotStop(j.pilot, "lost");
				Set(j, Phase::Idle);
				return;
			}
			// Life is the driver's licence: a dead NPC's vehicle stops firing and flying (its hover ends and it comes
			// down); the job ends, so a passenger player is let out and the vehicle is free again.
			if (Dead(npc)) {
				if (j.firing) SubmitFire(j.pilot - 1, j.vehicle, false);
				j.firing = false;
				vehicleflight::PilotStop(j.pilot, "driver died");
				logger::Write("NPC vehicles: [%d] NPC %08X died; vehicle %08X stopped", j.pilot, j.npc, j.vehicle);
				Set(j, Phase::Idle);
				return;
			}
			const bool aboard = InVehicle(npc) == vehicle;
			const double toPlayer = Dist(vehicle, player);
			if (j.phase == Phase::Following) vehicleflight::WatchFlip(vehicle, "NPC vehicles");
			switch (j.phase) {
			case Phase::Boarding:
				if (logger::Verbose() && j.cmd && now - j.cmdLog >= 1000) {
					j.cmdLog = now;
					LogCmdState(j, npc, vehicle);
				}
				// Arrived (the package stops the walk ~3 m from the vehicle): ONE plain get-in, logged. Everything else is a probe key (ProbeKeys).
				{
					const auto np = At<game::Vec3>(npc, off::kRefPosition);
					if (std::hypot(double(np.x) - j.arrX, double(np.y) - j.arrY) > 0.3) {
						j.arrX = np.x;
						j.arrY = np.y;
						j.arrStill = now;
					}
				}
				if (const bool settled = j.arrStill && now - j.arrStill >= 1200 && now - j.since > 3000; j.cmd && !aboard && j.cmdTries < 1 && (Dist(npc, vehicle) < 3.5 || (Dist(npc, vehicle) < 8.0 && settled))) {
					j.cmdTryAt = now;
					++j.cmdTries;
					EnsureSeat(j, npc, vehicle);
					pilotquest::Release(j.pilot - 1);
					if (kAnimatedEntry) {
						const bool act = pilotquest::AssignActivate(j.pilot - 1, npc, vehicle);
						logger::Write("NPC vehicles: [%d] arrived (%.1f m): Activate quest package assigned -> %s", j.pilot, Dist(npc, vehicle), act ? "yes" : "NO");
					}
					else {
						j.cmdInstant = true;
						int seated = -1;
						const int link = InstantSeat(npc, vehicle, seated);
						logger::Write("NPC vehicles: [%d] arrived (%.1f m): instant seat -> interaction %d, seat %d, the game says it is in %08X", j.pilot, Dist(npc, vehicle), link, seated,
							InVehicle(npc) ? At<std::uint32_t>(InVehicle(npc), off::kFormId) : 0);
					}
				}
				// The animated entry (when on) gets kAnimWaitMs; then, if the NPC is close, the instant seat finishes the job.
				if (kAnimatedEntry && j.cmd && !aboard && j.cmdTryAt && !j.cmdInstant && now - j.cmdTryAt >= kAnimWaitMs) {
					j.cmdInstant = true;
					if (Dist(npc, vehicle) < kSnapRange) {
						pilotquest::Release(j.pilot - 1);
						EnsureSeat(j, npc, vehicle);
						int seated = -1;
						const int link = InstantSeat(npc, vehicle, seated);
						logger::Write("NPC vehicles: [%d] the animated entry did not finish in %.0f s (%.1f m away): instant seat -> interaction %d, seat %d, the game says it is in %08X", j.pilot,
							kAnimWaitMs / 1000.0, Dist(npc, vehicle), link, seated, InVehicle(npc) ? At<std::uint32_t>(InVehicle(npc), off::kFormId) : 0);
					}
				}
				if (j.cmd && !aboard && j.cmdTries == 0 && j.arrStill && now - j.arrStill >= 8000 && now - j.since > 15000 && Dist(npc, vehicle) >= kSnapRange) {
					logger::Write("NPC vehicles: [%d] NPC %08X stopped %.1f m from the vehicle: no path to it from here; giving up", j.pilot, j.npc, Dist(npc, vehicle));
					notify::Show("The NPC can't reach that vehicle from here");
					pilotquest::Release(j.pilot - 1);
					Set(j, Phase::Idle);
					break;
				}
				if (aboard) {
					logger::Write("NPC vehicles: [%d] NPC %08X aboard after %.1f s", j.pilot, j.npc, (now - j.since) / 1000.0);
					if (j.passenger) {  // a passenger needs no pilot: keep it in g_riders, free the slot
						g_riders.push_back({ j.npc, j.vehicle, j.seat, now });
						pilotquest::Release(j.pilot - 1);
						logger::Write("NPC vehicles: [%d] NPC %08X rides vehicle %08X as passenger (seat %u)", j.pilot, j.npc, j.vehicle, j.seat);
						Set(j, Phase::Idle);
						break;
					}
					const auto vpos = At<game::Vec3>(vehicle, off::kRefPosition);
					if (!vcontract::AssignedTo(j.vehicle)) vcontract::Assign(j.vehicle, vcontract::Find("escort"), vpos.x, vpos.y);  // the default behaviour of an NPC driver
					vcontract::Goal first;
					if (vcontract::GoalFor(vcontract::User::Npc, j.pilot, vehicle, player, first) && vehicleflight::PilotFly(j.pilot, vehicle, first)) {
						if (!g_told) notify::Show("An NPC is following you in a vehicle");
						g_told = true;
						Set(j, Phase::Following);
					}
					else {
						Leave(j, npc, "pilot refused");
					}
				}
				else if (!j.cmd && !j.pkgSampled && now - j.since > 1000) {
					// 20:45 / 20:47: "walking to it", then 15 m -> 15 m in 5 s and the fallback seated it (the "teleport").
					// Is the get-in package (type 0x29) still the current one, and is the NPC fighting?
					j.pkgSampled = true;
					int type = -1, combat = -1;
					PackageState(npc, type, combat);
					logger::Write("NPC vehicles: [%d] NPC %08X 1 s into boarding: current package type %d (0x29 = get in), in combat %d, %.1f m from the vehicle (started %.1f)",
						j.pilot, j.npc, type, combat, Dist(npc, vehicle), j.startDist);
					// The package push (FUN_1418D9EA0) re-evaluates the AI only out of combat (vtable +0xB58); in combat the
					// get-in waits behind the combat AI and the NPC never walks. Not the get-in now: calm, ask once more.
					if (type != 0x29 && !j.reasked) {
						j.reasked = true;
						if (combat == 1) Calm(npc);
						const int r = Mount(npc, vehicle);
						logger::Write("NPC vehicles: [%d] NPC %08X get-in asked again (%s) -> %d", j.pilot, j.npc, combat == 1 ? "calmed first" : "package was replaced", r);
						if (r >= 1) {
							Set(j, Phase::Boarding);
							j.standing = r == 2;
							j.pkgSampled = false;
						}
					}
				}
				else if (j.standing && now - j.since > kStandMs) {
					// Up from the chair / furniture: the get-in for real now (the walk timer starts again).
					const int r = Mount(npc, vehicle);
					const bool keep = r >= 1;
					if (keep) {
						Set(j, Phase::Boarding);
						j.standing = r == 2;
					}
					else {
						logger::Write("NPC vehicles: [%d] NPC %08X stood up but the get-in was refused", j.pilot, j.npc);
						Set(j, Phase::Idle);
					}
				}
				else if (!j.cmd && !j.directTried && now - j.since > kWalkMs && Dist(npc, vehicle) > j.startDist - 2.0) {
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
				else if (now - j.since > (j.cmd ? 120000 : kBoardTimeoutMs)) {
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
				SyncProfile(j, player);
				// Following, as the player's rover follow (U): trail behind the player, park when the player stands still,
				// lift off again when the player moves away; the NPC stays aboard throughout.
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				if (!j.stillSince || std::hypot(pp.x - j.stillX, pp.y - j.stillY) > 1.0) {
					j.stillX = pp.x;
					j.stillY = pp.y;
					j.stillSince = now;
				}
				const bool playerStill = now - j.stillSince >= kFollowStillMs;
				// The goal comes from the vehicle's assigned profile (escort and pursuit by default); the actions (park, relaunch, attack run, weapon) are this owner's.
				vcontract::Goal pg;
				const bool have = vcontract::GoalFor(vcontract::User::Npc, j.pilot, vehicle, player, pg);
				const vcontract::Profile* ap = vcontract::AssignedTo(j.vehicle);
				const bool owned = ap && (std::strcmp(ap->name, "escort") == 0 || std::strcmp(ap->name, "pursuit") == 0);
				if (!aboard) {
					logger::Write("NPC vehicles: [%d] NPC %08X left the vehicle", j.pilot, j.npc);
					vehicleflight::PilotStop(j.pilot, "driver got out");
					Set(j, Phase::Idle);
				}
				else if (passenger::RidingVehicle() == j.vehicle) {
					passenger::Steer(j.pilot, vehicle);  // taxi: the map marker, else where the passenger looks
				}
				else if (!Outdoors(player) || (j.haveOrigin ? std::hypot(double(At<game::Vec3>(vehicle, off::kRefPosition).x) - j.originX, double(At<game::Vec3>(vehicle, off::kRefPosition).y) - j.originY) > kLeash : toPlayer > kGiveUp)) {
					logger::Write("NPC vehicles: [%d] stopped following (%s, %.0f m from you, %.0f m from where it started)", j.pilot, Outdoors(player) ? "too far from its origin" : "you went indoors", toPlayer,
						j.haveOrigin ? std::hypot(double(At<game::Vec3>(vehicle, off::kRefPosition).x) - j.originX, double(At<game::Vec3>(vehicle, off::kRefPosition).y) - j.originY) : 0.0);
					vehicleflight::PilotStop(j.pilot, "gave up");
					Set(j, Phase::Settling);
				}
				else if (have && !owned) {
					// HOOK 1 of the vehicle contract: another assigned profile (patrol, hauler, ...) drives.
					vehicleflight::PilotFly(j.pilot, vehicle, pg);
				}
				else if (have && vehicleflight::PilotActive(j.pilot) && kProfiles[int(j.profile)].orbit) {
					if (kProfiles[int(j.profile)].hostile && !InCombat(npc) && now - j.aggroAt >= kReaggroMs) {
						j.aggroAt = now;
						const bool ok = Aggro(npc, player);
						logger::Write("NPC vehicles: [%d] NPC %08X attack: combat with you -> %s", j.pilot, j.npc, ok ? "asked" : "failed");
					}
					vehicleflight::PilotFly(j.pilot, vehicle, pg);
					if (toPlayer < kAttackRun) AttackRun(j, player, vehicle);
					// Vehicle weapon: bursts with the nose on the player and in range.
					double axp = 0, ayp = 0, azp = 0;
					if (toPlayer >= kAttackRun) AimPoint(j, player, vehicle, axp, ayp, azp);  // (inside, AttackRun updated it)
					else {
						const auto ppl = At<game::Vec3>(player, off::kRefPosition);
						const double t = Dist3(vehicle, player) / kShotSpeed;
						axp = ppl.x + j.aimVx * t; ayp = ppl.y + j.aimVy * t; azp = ppl.z + kAimHeight + j.aimVz * t;
					}
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					const auto vr = At<game::Vec3>(vehicle, off::kRefRotation);
					const double aimErr = std::fabs(WrapAngle(std::atan2(axp - vp.x, ayp - vp.y) - vr.z));
					// The weapon fires along the nose (the projectile turn never took: 19:58 run, 0 aimed): yaw and pitch
					// both on the led aim point, in 3D range. Rotation x is nose down +.
					const double pitchWant = std::atan2(azp - vp.z, std::max(1.0, std::hypot(axp - vp.x, ayp - vp.y)));
					const double pitchErr = std::fabs(pitchWant + vr.x);
					const bool onTarget = aimErr < kFireCone && pitchErr < kFireCone * 1.5;
					const bool inRange = kProfiles[int(j.profile)].fire && Dist3(vehicle, player) < kFireRange;
					const bool canFire = inRange && onTarget;
					StoreAim(j.pilot - 1, AimSnap{ j.vehicle, j.npc, axp, ayp, azp, inRange, now });
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
							logger::Write("NPC vehicles: [%d] vehicle %08X weapon burst %u (%.0f m, aim off %.0f deg, pitch off %.0f deg) -> %s", j.pilot, j.vehicle, j.bursts, toPlayer, aimErr * 57.2958, pitchErr * 57.2958,
								ok ? "input sent" : "input FAILED");
						}
					}
				}
				else if (have && vehicleflight::PilotActive(j.pilot)) {
					if (kProfiles[int(j.profile)].parkWhenStill && playerStill && toPlayer < kFollowFar) {
						vehicleflight::PilotStop(j.pilot, "follow: player stopped; parked");
						j.parked = true;
						logger::Write("NPC vehicles: [%d] parked %.0f m from you (NPC stays aboard)", j.pilot, toPlayer);
					}
					else {
						vehicleflight::PilotFly(j.pilot, vehicle, pg);
					}
				}
				else if (have) {
					// Not flying: parked by us, or the flight ended (collision, stuck, landed). Relaunch like the rover follow.
					if (!j.parked && !j.relaunchAt) {
						logger::Write("NPC vehicles: [%d] flight ended (%s), %.0f m from you; relaunch in %.0f s", j.pilot, vehicleflight::PilotEndReason(j.pilot), toPlayer,
							kRelaunchWaitMs / 1000.0);
						j.relaunchAt = now + kRelaunchWaitMs;
					}
					const bool waitOver = j.parked || now >= j.relaunchAt;
					const bool orbit = kProfiles[int(j.profile)].orbit;
					if (waitOver && (toPlayer > kFollowRelaunch || orbit)) {
						if (vehicleflight::PilotFly(j.pilot, vehicle, pg)) {
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
					ClearOfVehicle(npc, vehicle, j.pilot);
					Set(j, Phase::Idle);
				}
				else if (now - j.since > kExitWatchMs) {
					// The animated exit did not finish (the AI may have dropped its package): the instant exit.
					const bool ok = InstantExit(npc);
					logger::Write("NPC vehicles: [%d] exit: still aboard after %.0f s; instant exit %s; NPC %s the vehicle", j.pilot, kExitWatchMs / 1000.0, ok ? "called" : "FAULTED", InVehicle(npc) ? "is STILL in" : "left");
					ClearOfVehicle(npc, vehicle, j.pilot);
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

		// ---- Seat rules (2026-10-17; notes/VEHICLE_INTERACTIONS_RESEARCH_20261017.md) ----
		// FUN_14219BE30 runs after ANY actor's sit (caller 1419F7750), so the player's own attempt is told apart by the
		// player's Activate press at a vehicle (the intent below). Rules for the player and an occupied vehicle:
		//   gang driver -> HAND-OVER (out at once, the player sits);  neutral / hostile -> BLOCK on a tap,
		//   EJECT on Activate held kPullMs (neutral: assault alarm, plus a theft alarm when someone else owns it).
		// Out at once = FUN_141A9C530(actor) (what the game's dismount uses for distant actors); the alarms are the
		// Papyrus natives' cores: Actor.SendAssaultAlarm FUN_141FC9770(_, _, victim), ObjectReference.SendStealAlarm
		// FUN_14207F050(_, _, ref, thief).
		constexpr std::uint32_t kEndInteraction = 0x01A9C530, kAssaultAlarm = 0x01FC9770, kStealAlarm = 0x0207F050;
		constexpr ULONGLONG kPullMs = 1000;        // hold Activate this long to pull a driver out
		constexpr ULONGLONG kIntentMs = 2500;      // a player-seat call this soon after the press is the player's
		constexpr double kReachRange = 5.0;        // m: the vehicle in front of the player
		constexpr double kReachCone = 0.8;         // rad

		enum class Relation : std::uint8_t { None, Gang, Neutral, Hostile };
		const char* RelationName(Relation r) { return r == Relation::Gang ? "gang" : r == Relation::Neutral ? "neutral" : r == Relation::Hostile ? "hostile" : "none"; }

		struct Intent
		{
			std::uint32_t vehicle = 0, occupant = 0;
			Relation relation = Relation::None;
			ULONGLONG pressAt = 0;
			bool acted = false;                    // hand-over / eject done for this press
			ULONGLONG seatAt = 0;                  // seat the player at this time (after the occupant is out)
		} g_intent;
		bool g_activateDown = false;
		int g_activateKey = 0;

		bool IsFollower(uptr player, uptr actor);

		Relation RelationOf(uptr player, uptr npc)
		{
			if (!npc) return Relation::None;
			const std::uint32_t id = At<std::uint32_t>(npc, off::kFormId);
			for (const auto& j : g_jobs) {
				if (j.npc == id && j.gang) return Relation::Gang;
			}
			if (IsFollower(player, npc)) return Relation::Gang;
			return InCombat(npc) ? Relation::Hostile : Relation::Neutral;
		}

		// The actor in the vehicle's driver seat (the one the game says is in it; the player excluded).
		uptr DriverOf(uptr vehicle, uptr player)
		{
			const std::uint32_t vid = At<std::uint32_t>(vehicle, off::kFormId);
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.vehicle == vid) {
					const uptr n = RefById(j.npc);
					if (n && InVehicle(n) == vehicle) return n;
				}
			}
			for (const uptr r : modscene::LoadedRefs(200)) {
				if (r == player || !IsActorRef(r) || IsVehicle(r) || Dead(r)) continue;
				if (Dist(r, vehicle) < 8.0 && InVehicle(r) == vehicle) return r;
			}
			return 0;
		}

		uptr FacingVehicle(uptr player)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const double facing = At<game::Vec3>(player, off::kRefRotation).z;
			uptr best = 0;
			double bestD = 1e9;
			for (const uptr r : modscene::LoadedRefs(200)) {
				if (!IsVehicle(r) || Disabled(r)) continue;
				const auto q = At<game::Vec3>(r, off::kRefPosition);
				const double dx = double(q.x) - pp.x, dy = double(q.y) - pp.y, d = std::hypot(dx, dy);
				if (d > kReachRange) continue;
				double off = std::atan2(dx, dy) - facing;
				while (off > 3.14159265) off -= 6.28318531;
				while (off < -3.14159265) off += 6.28318531;
				if (d > 2.0 && std::fabs(off) > kReachCone) continue;
				if (d < bestD) { bestD = d; best = r; }
			}
			return best;
		}

		bool EndInteraction(uptr actor)
		{
			__try {
				EngineFn<void (*)(uptr)>(kEndInteraction)(actor);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		void CrimeAlarms(uptr victim, uptr vehicle, uptr player, bool assault)
		{
			__try {
				if (assault) EngineFn<void (*)(uptr, uptr, uptr)>(kAssaultAlarm)(0, 0, victim);
				if (OwnerId(vehicle) && OwnerId(vehicle) != At<std::uint32_t>(At<uptr>(player, 0x98), off::kFormId))
					EngineFn<void (*)(uptr, std::uint32_t, uptr, uptr)>(kStealAlarm)(0, 0, vehicle, player);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				logger::Write("NPC vehicles: seat rules: crime alarm raised an exception");
			}
		}

		// Takes the occupant out (ends its job if it had one) and seats the player a moment later.
		void TakeSeat(uptr player, uptr vehicle, uptr occupant, const char* why)
		{
			const std::uint32_t oid = At<std::uint32_t>(occupant, off::kFormId);
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.npc == oid) {
					vehicleflight::PilotStop(j.pilot, why);
					Set(j, Phase::Idle);
				}
			}
			g_allowDismount = true;
			const bool out = EndInteraction(occupant);
			g_allowDismount = false;
			g_intent.acted = true;
			g_intent.seatAt = GetTickCount64() + 150;
			logger::Write("NPC vehicles: seat rules: %s: %08X out of %08X (%s) -> %s; you get in next", why, oid, At<std::uint32_t>(vehicle, off::kFormId),
				RelationName(g_intent.relation), out ? "out" : "FAILED");
			(void)player;
		}

		void SeatPlayerDetour(uptr vehicle)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - game::Base();
			const std::uint32_t id = vehicle ? At<std::uint32_t>(vehicle, off::kFormId) : 0;
			// The player's own attempt at an occupied vehicle: the seat rules decide (a tap never seats past a driver).
			if (id && g_intent.vehicle == id && GetTickCount64() - g_intent.pressAt < kIntentMs && !g_intent.acted) {
				const uptr player = Ptr(addr::kPlayerCharacterPtr);
				const uptr occ = player ? DriverOf(vehicle, player) : 0;
				if (occ) {
					logger::Write("NPC vehicles: seat rules: you tried vehicle %08X with %s driver %08X -> BLOCKED (%s)", id, RelationName(g_intent.relation),
						At<std::uint32_t>(occ, off::kFormId), g_intent.relation == Relation::Gang ? "handing over" : "hold to pull out");
					return;
				}
			}
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
		void LogCmdState(const Job& j, uptr npc, uptr vehicle)
		{
			// Which slots hold which package: process +0x20 = the ActorPackage's package (what this line has always shown); mid-high (+8) +0x38 = the
			// RUN-ONCE package (a pushed one such as the mount package wins over the current one), +0x40 = the package being run.
			{
				std::uint32_t idCur = 0, idOnce = 0, idRun = 0;
				int tyOnce = -1, tyRun = -1;
				__try {
					const uptr proc0 = At<uptr>(npc, 0x228);
					const uptr mh = proc0 ? At<uptr>(proc0, 8) : 0;
					if (mh) {
						const uptr once = At<uptr>(mh, 0x38), run = At<uptr>(mh, 0x40);
						if (once && !IsBadReadPtr(reinterpret_cast<void*>(once), 0x40)) { idOnce = At<std::uint32_t>(once, 0x28); tyOnce = At<std::int8_t>(once, 0x3C); }
						if (run && !IsBadReadPtr(reinterpret_cast<void*>(run), 0x40)) { idRun = At<std::uint32_t>(run, 0x28); tyRun = At<std::int8_t>(run, 0x3C); }
					}
					const uptr cur = proc0 ? At<uptr>(proc0, 0x20) : 0;
					if (cur && !IsBadReadPtr(reinterpret_cast<void*>(cur), 0x40)) idCur = At<std::uint32_t>(cur, 0x28);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
				}
				logger::Write("NPC vehicles: [%d] packages: current %08X, run-once %08X (type %d), running %08X (type %d)", j.pilot, idCur, idOnce, tyOnce, idRun, tyRun);
			}
			int pkg = -1;
			unsigned level = 0xFF, flags = 0, interaction = 0, pkgId = 0, procIdx = 0xFFFF;
			uptr target = 0;
			__try {
				const uptr proc = At<uptr>(npc, 0x228);
				if (proc) {
					level = At<std::uint8_t>(proc, 0xE7);
					target = At<uptr>(proc, 0x30);                 // ActorPackage +0x18 (raw: a handle or a ref)
					procIdx = At<std::uint32_t>(proc, 0x34);           // ActorPackage +0x1C: the procedure running
					const uptr tp = At<uptr>(proc, 0x20);  // ActorPackage is inline at +0x18; its TESPackage* is +0x08 (Traffic reads the same)
					if (tp && !IsBadReadPtr(reinterpret_cast<void*>(tp), 0x40)) {
						pkg = At<std::int8_t>(tp, 0x3C);
						pkgId = At<std::uint32_t>(tp, 0x28);
					}
				}
				flags = At<std::uint32_t>(npc, 0x378);
				interaction = EngineFn<char (*)(uptr, int)>(0x002C5C90)(At<uptr>(npc, 0xC8), 0xA5) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			const uptr in = InVehicle(npc);
			logger::Write("NPC vehicles: [%d] command: NPC %08X %.1f m from %08X; process level %u, package %08X type %d, flags %08X (getting on/off %u), seat interaction %u, procedure %u, target %p, %s", j.pilot,
				j.npc, Dist(npc, vehicle), j.vehicle, level, pkgId, pkg, flags, (flags >> 3) & 1, interaction, procIdx, reinterpret_cast<void*>(target),
				in == vehicle ? "SEATED" : in ? "in another vehicle" : "not in a vehicle");
			if (g_ctrlTarget) {
				const uptr c = RefById(g_ctrlTarget);
				if (c) logger::Write("NPC vehicles: [%d] control target %08X is %.1f m from the NPC", j.pilot, g_ctrlTarget, Dist(npc, c));
			}
		}

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
			// In combat the get-in package never runs (it waits behind the combat AI): calm first.
			if (seat && InCombat(npc)) {
				Calm(npc);
				logger::Write("NPC vehicles: [%d] NPC %08X was in combat; calmed for the get-in", slot.pilot, npcId);
			}
			const int r = seat ? Mount(npc, vehicle) : 0;
			slot.standing = r == 2;
			const uptr base = At<uptr>(vehicle, 0x98);
			logger::Write("NPC vehicles: [%d] NPC %08X -> vehicle %08X (base %08X, %.0f m from it); seat %s; get-in -> %d", slot.pilot, npcId,
				At<std::uint32_t>(vehicle, off::kFormId), base ? At<std::uint32_t>(base, off::kFormId) : 0, Dist(npc, vehicle), seat ? "free" : "REFUSED", r);
			if (r < 1) return false;
			slot.npc = npcId;
			slot.vehicle = At<std::uint32_t>(vehicle, off::kFormId);
			slot.gang = gang;
			{ const auto op = At<game::Vec3>(npc, off::kRefPosition); slot.originX = op.x; slot.originY = op.y; slot.haveOrigin = true; }
			slot.profile = Profile::Escort;
			Set(slot, Phase::Boarding);
			return true;
		}

		constexpr double kRunRange = 40.0;  // m: further than this the NPC runs to the vehicle (also when in combat)
		bool StartWalkBoard(Job* slot, uptr npc, uptr vehicle, unsigned seat = 0)
		{
			// The Walk key: the game's own follower command. The job only watches (no instant seat); Enter and the seated check follow.
			// Native, data-driven: SeamlessPilots.esm's quest package (any actor); else the follower command (followers only).
			// Context: in combat the NPC is calmed (or the fight would outrank the package) and RUNS to the vehicle; otherwise it walks at the package's own pace.
			if (InCombat(npc)) {
				Calm(npc);
				SetForceRun(npc, true);
				slot->forcedRun = true;
				logger::Write("NPC vehicles: [%d] NPC %08X is in combat: calmed, running to the vehicle", slot->pilot, At<std::uint32_t>(npc, off::kFormId));
			}
			else if (Dist(npc, vehicle) > kRunRange) {
				// 12:04: a vehicle 160-290 m away took over 2 minutes at walking pace (the 120 s timeout ended it); far = run, cleared again once aboard.
				SetForceRun(npc, true);
				slot->forcedRun = true;
				logger::Write("NPC vehicles: [%d] NPC %08X is calm but the vehicle is %.0f m away (over %.0f m): running to the vehicle", slot->pilot, At<std::uint32_t>(npc, off::kFormId), Dist(npc, vehicle), kRunRange);
			}
			else {
				logger::Write("NPC vehicles: [%d] NPC %08X is calm: walking to the vehicle", slot->pilot, At<std::uint32_t>(npc, off::kFormId));
			}
			const bool viaQuest = pilotquest::Assign(slot->pilot - 1, npc, vehicle);
			if (!viaQuest) {
				notify::Show("SeamlessPilots.esm is not loaded: NPCs cannot be sent to vehicles");
				return false;
			}
			slot->npc = At<std::uint32_t>(npc, off::kFormId);
			slot->vehicle = At<std::uint32_t>(vehicle, off::kFormId);
			slot->gang = false;
			{ const auto op = At<game::Vec3>(npc, off::kRefPosition); slot->originX = op.x; slot->originY = op.y; slot->haveOrigin = true; }
			slot->passenger = seat > 0;
			slot->seat = seat;
			slot->profile = Profile::Escort;
			Set(*slot, Phase::Boarding);
			slot->cmd = true;
			slot->cmdLog = 0;
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
			if (!StartWalkBoard(slot, npc, vehicle)) return;
			notify::Show("NPC sent to a vehicle (%.0f m)", bestV);
		}

		// ---- The player's followers: the game's own follower list (used by the seat rules: a follower hands the wheel over) ----
		// Game.GetPlayerFollowers (FUN_14200B760): the player's extra data of type 0x1B holds {.. +0x18 count, +0x20
		// array of 8-byte entries, handle first}; each handle's actor counts when FUN_14192A290(actor, player) is true.
		constexpr std::uint32_t kGetExtra = 0x00347170, kIsFollowing = 0x0192A290;

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

		bool IsFollower(uptr player, uptr actor)
		{
			uptr gang[32];
			const int n = Followers(player, gang, 32);
			for (int i = 0; i < n; ++i) {
				if (gang[i] == actor) return true;
			}
			return false;
		}

		void SeatPlayerNow(uptr vehicle)
		{
			__try {
				g_seatPlayerOriginal(vehicle);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}

		// The player's Activate at a vehicle: tap = hand-over (gang) or blocked; held kPullMs = pull the driver out.
		void SeatTick(uptr player)
		{
			const ULONGLONG now = GetTickCount64();
			if (g_intent.seatAt && now >= g_intent.seatAt) {
				g_intent.seatAt = 0;
				const uptr v = RefById(g_intent.vehicle);
				if (v && !InVehicle(player)) {
					SeatPlayerNow(v);
					logger::Write("NPC vehicles: seat rules: you get in %08X -> %s", g_intent.vehicle, InVehicle(player) == v ? "seated" : "not seated yet");
				}
			}
			if (!g_activateKey || now % 5000 < 20) g_activateKey = controlmap::KeyFor("Gameplay", "Activate", 'E');
			const bool down = Focused() && (GetAsyncKeyState(g_activateKey) & 0x8000) != 0;
			const bool pressed = down && !g_activateDown;
			const bool released = !down && g_activateDown;
			g_activateDown = down;
			{
				// The HUD prompts (Passenger / Hold: eject) while the player faces an occupied vehicle on foot.
				static ULONGLONG lastHud = 0;
				static bool shown = false;
				if (now - lastHud >= 200) {
					lastHud = now;
					bool occupied = false;
					if (!InVehicle(player)) {
						const uptr fv = FacingVehicle(player);
						occupied = fv && DriverOf(fv, player);
					}
					if (occupied || shown) hoverui::SetOccupied(occupied, controlmap::KeyName(g_activateKey).c_str());
					shown = occupied;
				}
			}
			if (InVehicle(player)) return;
			if (pressed) {
				g_intent = Intent{};
				const uptr v = FacingVehicle(player);
				if (!v) return;
				const uptr occ = DriverOf(v, player);
				g_intent.vehicle = At<std::uint32_t>(v, off::kFormId);
				g_intent.pressAt = now;
				if (!occ) return;  // empty: the game's own get-in (and its theft rules)
				g_intent.occupant = At<std::uint32_t>(occ, off::kFormId);
				g_intent.relation = RelationOf(player, occ);
				// A tap (released before kPullMs) = the passenger seat; holding = pull the driver out (below).
				return;
			}
			if (released && g_intent.occupant && !g_intent.acted && now - g_intent.pressAt < kPullMs) {
				g_intent.acted = true;
				const uptr v = RefById(g_intent.vehicle);
				if (v && Dist(player, v) <= kReachRange + 1.0) {
					const bool ok = passenger::BoardVehicle(player, v);
					logger::Write("NPC vehicles: seat rules: tap on occupied vehicle %08X (%s driver %08X) -> passenger seat %s", g_intent.vehicle, RelationName(g_intent.relation),
						g_intent.occupant, ok ? "taken" : "NOT taken");
				}
				return;
			}
			if (down && g_intent.occupant && !g_intent.acted && now - g_intent.pressAt >= kPullMs) {
				const uptr v = RefById(g_intent.vehicle), occ = RefById(g_intent.occupant);
				if (!v || !occ || InVehicle(occ) != v || Dist(player, v) > kReachRange + 1.0) return;
				TakeSeat(player, v, occ, g_intent.relation == Relation::Gang ? "hand-over" : g_intent.relation == Relation::Hostile ? "hijack" : "pulled out");
				if (g_intent.relation != Relation::Gang) CrimeAlarms(occ, v, player, g_intent.relation == Relation::Neutral);
				if (g_intent.relation == Relation::Hostile) Aggro(occ, player);
				notify::Show(g_intent.relation == Relation::Gang ? "They hand you the wheel" : g_intent.relation == Relation::Hostile ? "Hijacked" : "You pulled them out");
			}
		}

		// ---- NPCs forcing the player out (2026-10-17) ----
		// Owners (the vehicle's owner, or a member of its owning faction) reclaim it; hostile NPCs hijack it; guards
		// (hostile and with a crime faction) take the player out. Only while the vehicle is on the ground (hover off) and
		// slow, after a kForceWarnMs warning the player can escape by driving off. The player's own vehicle is never
		// reclaimed, the gang never forces anyone. Game checks: IsInFaction = actor vtable +0xBA0(actor, faction);
		// IsHostileToActor = FUN_14191FCA0(actor, target); GetCrimeFaction = FUN_141949090(actor); exits: the animated
		// dismount FUN_141952530 (owners, guards), the instant FUN_141A9C530 (hijackers, and when the exit is blocked).
		constexpr std::uint32_t kIsHostileTo = 0x0191FCA0, kCrimeFaction = 0x01949090;
		constexpr double kForceReach = 4.0;        // m from the vehicle
		constexpr double kForceSlow = 3.0;         // m/s
		constexpr ULONGLONG kForceWarnMs = 2000, kForceCooldownMs = 15000;
		enum class Forcer : std::uint8_t { None, Owner, Hijacker, Guard };
		const char* ForcerName(Forcer f) { return f == Forcer::Owner ? "owner" : f == Forcer::Hijacker ? "hijacker" : f == Forcer::Guard ? "guard" : "none"; }

		struct ForceOut
		{
			std::uint32_t actor = 0, vehicle = 0;
			Forcer kind = Forcer::None;
			ULONGLONG at = 0;                      // eject at this time (after the warning)
			ULONGLONG cooldownUntil = 0;
			std::uint32_t cooldownActor = 0;
			double vx = 0, vy = 0;                 // the player's vehicle: last position (speed)
			ULONGLONG lastAt = 0;
			double speed = 0;
		} g_force;

		bool HostileTo(uptr actor, uptr target)
		{
			__try {
				return EngineFn<bool (*)(uptr, uptr)>(kIsHostileTo)(actor, target);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool HasCrimeFaction(uptr actor)
		{
			__try {
				return EngineFn<uptr (*)(uptr)>(kCrimeFaction)(actor) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool InFaction(uptr actor, uptr faction)
		{
			__try {
				auto** vt = *reinterpret_cast<void***>(actor);
				return reinterpret_cast<bool (*)(uptr, uptr)>(vt[0xBA0 / 8])(actor, faction);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		uptr OwnerForm(uptr ref)
		{
			__try {
				const uptr extra = At<uptr>(ref, 0xC8);
				return extra ? EngineFn<uptr (*)(uptr)>(kGetOwner)(extra) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// Who may force the player out of `vehicle`: a guard first, then the owner, then a hijacker.
		Forcer ForcerKind(uptr player, uptr vehicle, uptr actor)
		{
			const bool hostile = HostileTo(actor, player) && InCombat(actor);
			if (hostile && HasCrimeFaction(actor)) return Forcer::Guard;
			const std::uint32_t vid = At<std::uint32_t>(vehicle, off::kFormId);
			const uptr owner = OwnerForm(vehicle);
			const uptr playerBase = At<uptr>(player, 0x98);
			if (owner && owner != playerBase && vid != vehicleflight::OwnRoverId() && !hostile) {
				const uptr base = At<uptr>(actor, 0x98);
				if (base == owner || (At<std::uint8_t>(owner, 0x2E) == 0x0D && InFaction(actor, owner))) return Forcer::Owner;
			}
			if (hostile) return Forcer::Hijacker;
			return Forcer::None;
		}

		void EjectPlayer(uptr player, Forcer kind)
		{
			bool out = false;
			if (kind != Forcer::Hijacker) out = Dismount(player) == 1;  // the game's own exit (with its obstruction checks)
			if (!out) out = EndInteraction(player);
			logger::Write("NPC vehicles: force-out: you were taken out by a %s (%s exit) -> %s", ForcerName(kind), kind == Forcer::Hijacker ? "instant" : "animated",
				out ? "out" : "FAILED");
		}

		void ForceOutTick(uptr player)
		{
			const ULONGLONG now = GetTickCount64();
			const uptr pv = InVehicle(player);
			if (pv && passenger::RidingVehicle() && passenger::RidingVehicle() == At<std::uint32_t>(pv, off::kFormId)) {
				g_force.kind = Forcer::None;  // riding as a passenger: nobody forces a passenger out
				g_force.at = 0;
				return;
			}
			if (!pv) {
				if (g_force.kind != Forcer::None && g_force.at) {
					// out (by the force-out or by the player): the forcer takes over
					const uptr a = RefById(g_force.actor), v = RefById(g_force.vehicle);
					if (a && v && !InVehicle(player)) {
						if (g_force.kind == Forcer::Owner) {
							const int r = Mount(a, v);
							logger::Write("NPC vehicles: force-out: owner %08X gets back in %08X -> %d", g_force.actor, g_force.vehicle, r);
						}
						else if (g_force.kind == Forcer::Hijacker) {
							for (auto& j : g_jobs) {
								if (j.phase != Phase::Idle) continue;
								if (StartJob(j, a, v, false)) {
									j.profile = Profile::Pursuit;
									logger::Write("NPC vehicles: [%d] force-out: hijacker %08X takes %08X and comes after you", j.pilot, g_force.actor, g_force.vehicle);
								}
								break;
							}
						}
					}
				}
				g_force.kind = Forcer::None;
				g_force.at = 0;
				g_force.lastAt = 0;
				return;
			}
			// the vehicle's ground speed
			const auto p = At<game::Vec3>(pv, off::kRefPosition);
			if (g_force.lastAt && now > g_force.lastAt) {
				const double dt = (now - g_force.lastAt) / 1000.0;
				const double inst = std::hypot(p.x - g_force.vx, p.y - g_force.vy) / dt;
				if (dt < 1.0) g_force.speed += (inst - g_force.speed) * std::min(1.0, dt * 4.0);
			}
			g_force.vx = p.x; g_force.vy = p.y; g_force.lastAt = now;
			const bool slow = g_force.speed < kForceSlow && !vehicleflight::Active();
			if (g_force.kind != Forcer::None) {
				const uptr a = RefById(g_force.actor);
				if (!slow || !a || Dead(a) || Dist(a, pv) > kForceReach + 2.0) {
					logger::Write("NPC vehicles: force-out: %s %08X gave up (%s)", ForcerName(g_force.kind), g_force.actor, !slow ? "you drove off" : "gone");
					g_force.cooldownActor = g_force.actor;
					g_force.cooldownUntil = now + kForceCooldownMs;
					g_force.kind = Forcer::None;
					g_force.at = 0;
					return;
				}
				if (now >= g_force.at) EjectPlayer(player, g_force.kind);
				return;
			}
			if (!slow || now % 500 > 40) return;
			for (const uptr r : modscene::LoadedRefs(30)) {
				if (r == player || !IsActorRef(r) || IsVehicle(r) || Dead(r) || InVehicle(r)) continue;
				if (Dist(r, pv) > kForceReach) continue;
				const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
				if (id == g_force.cooldownActor && now < g_force.cooldownUntil) continue;
				if (Locked(id) || IsFollower(player, r)) continue;  // the gang never forces anyone
				const Forcer k = ForcerKind(player, pv, r);
				if (k == Forcer::None) continue;
				g_force.kind = k;
				g_force.actor = id;
				g_force.vehicle = At<std::uint32_t>(pv, off::kFormId);
				g_force.at = now + kForceWarnMs;
				notify::Show(k == Forcer::Owner ? "Owner: \"Get out of my vehicle!\"" : k == Forcer::Guard ? "Guard: \"Step out of the vehicle!\"" : "Hijacker! Drive off!");
				logger::Write("NPC vehicles: force-out: %s %08X at your vehicle %08X (%.1f m); out in %.0f s unless you drive off", ForcerName(k), id, g_force.vehicle,
					Dist(r, pv), kForceWarnMs / 1000.0);
				return;
			}
		}

	}

	namespace
	{
		// One job's profile (escort / pursuit): pursuit puts the driver in combat with the player, escort calms it and stops the weapon.
		bool ApplyProfile(Job& j, Profile to, uptr player)
		{
			const uptr npc = RefById(j.npc);
			if (!npc) return false;
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
			return true;
		}
	}

	void Tick()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) return;
		// The player died: NPC vehicles stop firing and flying (they stay where they are, the NPCs aboard).
		static bool deadDone = false;
		if (Dead(player)) {
			if (!deadDone) {
				deadDone = true;
				for (auto& j : g_jobs) {
					if (j.phase == Phase::Idle) continue;
					if (j.firing) SubmitFire(j.pilot - 1, j.vehicle, false);
					j.firing = false;
					vehicleflight::PilotStop(j.pilot, "you died");
				}
				logger::Write("NPC vehicles: you died; NPC vehicles stopped");
			}
			return;
		}
		deadDone = false;
		ChooseKey();
		const bool wdown = g_walkKey && Focused() && (GetAsyncKeyState(g_walkKey) & 0x8000) != 0;
		if (wdown && !g_walkDown) Force(player);
		g_walkDown = wdown;
		const bool xdown = g_exitKey && Focused() && (GetAsyncKeyState(g_exitKey) & 0x8000) != 0;
		if (xdown && !g_exitDown) ExitAllJobs();
		g_exitDown = xdown;
		pilotquest::Tick();
		ExitTick();
		RidersTick();
		SeatTick(player);
		ForceOutTick(player);
		const bool adown = g_aggroKey && Focused() && (GetAsyncKeyState(g_aggroKey) & 0x8000) != 0;
		if (adown && !g_aggroDown) {
			// Any vehicle in escort -> all to pursuit; otherwise all back to escort.
			bool anyEscort = false;
			for (const auto& j : g_jobs) anyEscort = anyEscort || (j.phase == Phase::Following && j.profile == Profile::Escort);
			const Profile to = anyEscort ? Profile::Pursuit : Profile::Escort;
			int n = 0;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Following) continue;
				const uptr veh = RefById(j.vehicle);
				if (!veh) continue;
				const auto vp = At<game::Vec3>(veh, off::kRefPosition);
				vcontract::Assign(j.vehicle, vcontract::Find(to == Profile::Pursuit ? "pursuit" : "escort"), vp.x, vp.y);
				if (ApplyProfile(j, to, player)) ++n;
			}
			notify::Show(to == Profile::Pursuit ? "Vehicles: pursuit" : "Vehicles: escort");
			logger::Write("NPC vehicles: profile %s (%d following)", kProfiles[int(to)].name, n);
		}
		g_aggroDown = adown;
		for (int i = 0; i < kJobs; ++i) {
			g_jobs[i].pilot = i + 1;
			if (g_jobs[i].phase != Phase::Idle) Update(g_jobs[i], player);
			if (g_jobs[i].phase == Phase::Following && !g_jobs[i].firing) PushDrive(g_jobs[i]);
		}
		// The HUD's gang line: what the NPC vehicles are doing, e.g. "2  1 pursuit, 1 escort, 1 boarding" (the test profile key shows only with debug keys).
		int following[2] = {}, boarding = 0, leaving = 0, total = 0;
		for (const auto& j : g_jobs) {
			if (j.phase == Phase::Idle) continue;
			++total;
			if (j.phase == Phase::Following) ++following[int(j.profile)];
			else if (j.phase == Phase::Boarding) ++boarding;
			else ++leaving;  // Settling / Leaving
		}
		char what[96] = "";
		auto add = [&](int n, const char* label) {
			if (n <= 0) return;
			char part[32];
			std::snprintf(part, sizeof(part), "%s%d %s", what[0] ? ", " : "", n, label);
			std::strncat(what, part, sizeof(what) - std::strlen(what) - 1);
		};
		add(following[int(Profile::Pursuit)], "pursuit");
		add(following[int(Profile::Escort)], "escort");
		add(boarding, "boarding");
		add(leaving, "getting out");
		add(int(g_riders.size()), "riding");
		total += int(g_riders.size());
		hoverui::SetGang(total, "", g_aggroKey ? controlmap::KeyName(g_aggroKey).c_str() : "");  // the count; the rows below say what each vehicle does
		{
			// One line per NPC vehicle / rider: "pursuit  REV8  120 m".
			static char line[hoverui::kMaxRows][80];
			const char* ptrs[hoverui::kMaxRows];
			int nr = 0;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			for (const auto& j : g_jobs) {
				if (j.phase == Phase::Idle || nr >= hoverui::kMaxRows) continue;
				const uptr v = RefById(j.vehicle);
				char name[48] = "";
				if (v) CopyEditorId(At<uptr>(v, 0x98), name, sizeof(name));
				const char* state = j.phase == Phase::Following ? kProfiles[int(j.profile)].name : j.passenger ? "boarding as passenger" : j.phase == Phase::Boarding ? "boarding" : "getting out";
				const double d = v ? Dist(v, player) : 0.0;
				std::snprintf(line[nr], sizeof(line[nr]), "%s%s%s  %.0f m", state, name[0] ? " " : "", name, d);
				ptrs[nr] = line[nr];
				++nr;
			}
			for (const auto& r : g_riders) {
				if (nr >= hoverui::kMaxRows) break;
				const uptr v = RefById(r.vehicle);
				char name[48] = "";
				if (v) CopyEditorId(At<uptr>(v, 0x98), name, sizeof(name));
				std::snprintf(line[nr], sizeof(line[nr]), "riding%s%s  %.0f m", name[0] ? " " : "", name, v ? Dist(v, player) : 0.0);
				ptrs[nr] = line[nr];
				++nr;
			}
			(void)pp;
			hoverui::SetRows(nr, ptrs);
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
			if (!ours && component && input) {
				// The player's vehicle while Auto drives it (drive mode): the autopilot's controls replace the player's input.
				vehicleflight::DriveInput di;
				if (vehicleflight::DriveFor(At<std::uint32_t>(component, 0xC), di)) {
					auto* in = reinterpret_cast<std::uint8_t*>(input);
					*reinterpret_cast<float*>(in + 0x10) = di.steer;
					*reinterpret_cast<float*>(in + 0x14) = di.accel;
					*reinterpret_cast<float*>(in + 0x18) = di.brake;
					in[0x1C] = di.boost ? 1 : 0;
					in[0x1E] = di.vert ? 1 : 0;
				}
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
					// Shots are aimed at launch now (vehicleaim, Projectile::Launch); the projectiles here are not set up yet
					// (direction and speed 0), so nothing is done with them.
					(void)&AimProjectiles;
					break;
				}
			}
			return g_fireOriginal(component, event);
		}
	}

	namespace
	{
		// ---- Seat-check trace (2026-10-18): which gate of FUN_141A05D40 refuses the REV-8? Logged only while one of OUR get-in calls runs (g_traceSeat).
		using Check_t = std::uint8_t (*)(uptr, uptr, uptr);
		using SeatData_t = uptr (*)(uptr, uptr);
		using SeatData2_t = uptr (*)(uptr);
		using Finder_t = void (*)(uptr, std::uint32_t, std::uint8_t, int, uptr, uptr, uptr, uptr, uptr);
		using Pose_t = std::uint64_t (*)(float, uptr, uptr, float*, uptr, float*, float*, float*, float*);
		Check_t g_checkOrig = nullptr;
		SeatData_t g_seatDataOrig = nullptr;
		SeatData2_t g_seatData2Orig = nullptr;
		Finder_t g_finderOrig = nullptr;
		Pose_t g_poseOrig = nullptr;

		std::uint8_t CheckDetour(uptr process, uptr actor, uptr vehicle)
		{
			const std::uint8_t r = g_checkOrig(process, actor, vehicle);
			if (g_traceSeat)
				logger::Write("SEAT TRACE: FUN_141A05D40(actor %08X, vehicle %08X) -> %d  [process mid-high data %s]", At<std::uint32_t>(actor, off::kFormId), At<std::uint32_t>(vehicle, off::kFormId), int(r),
					process && At<uptr>(process, 8) ? "present" : "ABSENT (the check returns 0 at once)");
			return r;
		}

		uptr SeatDataDetour(uptr actor, uptr vehicle)
		{
			const uptr r = g_seatDataOrig(actor, vehicle);
			if (g_traceSeat) logger::Write("SEAT TRACE: FUN_1412B2C40 (the actor's seat data for this vehicle) -> %p", reinterpret_cast<void*>(r));
			return r;
		}

		uptr SeatData2Detour(uptr vehicle)
		{
			const uptr r = g_seatData2Orig(vehicle);
			if (g_traceSeat) logger::Write("SEAT TRACE: FUN_1412B2A90 (the vehicle's own default seat data) -> %p", reinterpret_cast<void*>(r));
			return r;
		}

		void FinderDetour(uptr a1, std::uint32_t a2, std::uint8_t a3, int a4, uptr a5, uptr a6, uptr a7, uptr a8, uptr a9)
		{
			g_finderOrig(a1, a2, a3, a4, a5, a6, a7, a8, a9);
			static int budget = 40;  // the AI calls the finder too (a mount package running); log a limited number of those
			if (g_traceSeat || budget > 0) {
				if (!g_traceSeat) --budget;
				const std::uint32_t n = a9 ? At<std::uint32_t>(a9, 0) : 0u;
				logger::Write("SEAT TRACE: entry-point finder FUN_14062FC20 (race/skeleton %p, flags %u) found %u entry point(s)%s", reinterpret_cast<void*>(a1), unsigned(a3), n, g_traceSeat ? "" : " [AI]");
			}
		}

		std::uint64_t PoseDetour(float f, uptr a2, uptr a3, float* a4, uptr a5, float* a6, float* a7, float* a8, float* a9)
		{
			const std::uint64_t r = g_poseOrig(f, a2, a3, a4, a5, a6, a7, a8, a9);
			static int budget = 40;
			if (g_traceSeat || budget > 0) {
				if (!g_traceSeat) --budget;
				// a4 = the entry node position (scaled by f), a7 = a second vector (the approach / facing delta); both are floats x y z.
				logger::Write("SEAT TRACE: entry-node pose FUN_140EFB310 -> %d  scale %.2f  pos (%.2f, %.2f, %.2f)  vec (%.2f, %.2f, %.2f)%s", int(r & 0xFF), f, a4 ? a4[0] : 0.f, a4 ? a4[1] : 0.f, a4 ? a4[2] : 0.f,
					a7 ? a7[0] : 0.f, a7 ? a7[1] : 0.f, a7 ? a7[2] : 0.f, g_traceSeat ? "" : " [AI]");
			}
			return r;
		}

		void InstallSeatTrace()
		{
			static const std::uint8_t kCheck[14] = { 0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x53, 0x56 };
			static const std::uint8_t kData[14] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x54 };
			static const std::uint8_t kData2[16] = { 0x40, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38, 0x4C, 0x8B, 0xF1 };
			static const std::uint8_t kFind[14] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54 };
			static const std::uint8_t kPose[18] = { 0x48, 0x8B, 0xC4, 0x55, 0x53, 0x41, 0x57, 0x48, 0x8D, 0x68, 0xC9, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };
			void* o = nullptr;
			int ok = 0;
			if (hook::Install(game::Base() + 0x01A05D40, kCheck, sizeof(kCheck), reinterpret_cast<void*>(&CheckDetour), &o)) { g_checkOrig = reinterpret_cast<Check_t>(o); ++ok; }
			if (hook::Install(game::Base() + 0x012B2C40, kData, sizeof(kData), reinterpret_cast<void*>(&SeatDataDetour), &o)) { g_seatDataOrig = reinterpret_cast<SeatData_t>(o); ++ok; }
			if (hook::Install(game::Base() + 0x012B2A90, kData2, sizeof(kData2), reinterpret_cast<void*>(&SeatData2Detour), &o)) { g_seatData2Orig = reinterpret_cast<SeatData2_t>(o); ++ok; }
			if (hook::Install(game::Base() + 0x0062FC20, kFind, sizeof(kFind), reinterpret_cast<void*>(&FinderDetour), &o)) { g_finderOrig = reinterpret_cast<Finder_t>(o); ++ok; }
			if (hook::Install(game::Base() + 0x00EFB310, kPose, sizeof(kPose), reinterpret_cast<void*>(&PoseDetour), &o)) { g_poseOrig = reinterpret_cast<Pose_t>(o); ++ok; }
			logger::Write("NPC vehicles: seat-check trace hooks installed: %d of 5", ok);
		}
	}

	void Install()
	{
		static const vcontract::SeatProvider kSeats{ &BoardNpc, &RequestExit, &UnloadVehicle, &ExitStateOf };
		vcontract::SetSeatProvider(&kSeats);
		InstallSeatTrace();
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
	}

	bool UsesVehicle(std::uint32_t vehicleId) { return vehicleId && Busy(vehicleId); }

	int PilotFor(std::uint32_t vehicleId)
	{
		for (const auto& j : g_jobs) {
			if (vehicleId && j.phase == Phase::Following && j.vehicle == vehicleId) return j.pilot;
		}
		return 0;
	}

	bool HasDriver(std::uint32_t vehicleId)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		const uptr v = RefById(vehicleId);
		return player && v && DriverOf(v, player) != 0;
	}

	std::uint32_t TakeDriver(std::uint32_t vehicleId)
	{
		for (auto& j : g_jobs) {
			if (!vehicleId || j.phase != Phase::Following || j.vehicle != vehicleId) continue;
			const std::uint32_t npcId = j.npc;
			const uptr npc = RefById(npcId);
			vehicleflight::PilotStop(j.pilot, "the player took the wheel");
			Set(j, Phase::Idle);
			if (npc) {
				g_allowDismount = true;
				EndInteraction(npc);
				g_allowDismount = false;
			}
			return npc ? npcId : 0;
		}
		return 0;
	}

	bool SeatPlayerDriver(std::uintptr_t vehicle)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !vehicle || !g_seatPlayerOriginal) return false;
		SeatPlayerNow(vehicle);
		return InVehicle(player) == vehicle;
	}

	bool RequestExit(std::uintptr_t actor, const vcontract::ExitRequest& req) { return StartExit(actor, req); }

	int UnloadVehicle(std::uintptr_t vehicle, const vcontract::ExitRequest& req, bool includeDriver)
	{
		int n = 0;
		const std::uint32_t vid = At<std::uint32_t>(vehicle, off::kFormId);
		for (const uptr r : modscene::LoadedRefs(8000)) {
			if (!IsActorRef(r) || InVehicle(r) != vehicle) continue;
			bool isDriver = false;
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.vehicle == vid && j.npc == At<std::uint32_t>(r, off::kFormId)) isDriver = true;
			}
			if (isDriver && !includeDriver) continue;
			if (StartExit(r, req)) ++n;
		}
		return n;
	}

	// board(actor, vehicle, asDriver): the NPC walks to the vehicle (running when in combat), gets in (animated first, the instant seat after kAnimWaitMs) and
	// then follows like a job. false: refused (not a driver request, no free slot, the vehicle or the actor is busy / dead, no vehicle seat for it).
	bool BoardNpc(std::uintptr_t actor, std::uintptr_t vehicle, bool asDriver)
	{
		if (!actor || !vehicle) return false;
		crash::Crumb(asDriver ? "board npc as driver" : "board npc as passenger", int(At<std::uint32_t>(actor, off::kFormId)));
		const std::uint32_t npcId = At<std::uint32_t>(actor, off::kFormId), vehId = At<std::uint32_t>(vehicle, off::kFormId);
		if (!asDriver) {  // a passenger seat: the vehicle may be in use (its driver's job), the actor must be free
			if (Locked(npcId) || Dead(actor) || InVehicle(actor) || Busy(npcId)) return false;
			uptr kw = 0;
			const int seat = PickPassengerSeat(vehicle, kw);
			if (!seat) return false;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Idle) continue;
				j.pilot = int(&j - g_jobs) + 1;
				return StartWalkBoard(&j, actor, vehicle, unsigned(seat));
			}
			return false;
		}
		if (vehId == vehicleflight::OwnRoverId()) return false;  // never the player's own vehicle: PilotFly refuses it, the NPC would sit and then get out
		if (Locked(npcId) || Dead(actor) || InVehicle(actor) || Busy(vehId) || Busy(npcId)) return false;
		if (!DriverSeatKeyword(vehicle) && !CanMount(actor, vehicle)) return false;
		for (auto& j : g_jobs) {
			if (j.phase != Phase::Idle) continue;
			j.pilot = int(&j - g_jobs) + 1;
			return StartWalkBoard(&j, actor, vehicle);
		}
		return false;
	}

	bool IsInCombat(std::uintptr_t actor) { return actor && InCombat(actor); }
	bool IsDead(std::uintptr_t actor) { return !actor || Dead(actor); }
	bool LooksLikeActor(std::uintptr_t ref) { return ref && IsActorRef(ref); }
	CombatState CombatOf(std::uintptr_t actor)
	{
		CombatState c{};
		if (!actor) return c;
		c.inCombat = InCombat(actor);
		__try {
			c.target = At<std::uint32_t>(actor, 0x298);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			c.target = 0;
		}
		const std::uint32_t none = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
		c.hasTarget = c.target != 0 && c.target != none;
		c.fighting = c.inCombat || c.hasTarget;
		return c;
	}
	bool IsInCombat(std::uint32_t actorFormId) { const uptr a = RefById(actorFormId); return a && InCombat(a); }
	bool IsAlly(std::uintptr_t actor)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		return player && actor && IsFollower(player, actor);
	}
	int Jobs(JobInfo* out, int cap)
	{
		int n = 0;
		for (const auto& j : g_jobs) {
			if (j.phase == Phase::Idle || n >= cap) continue;
			out[n++] = { j.npc, j.vehicle, j.phase == Phase::Following, j.profile == Profile::Pursuit };
		}
		return n;
	}
	bool SetVehicleProfile(std::uint32_t vehicleId, bool pursuit)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		for (auto& j : g_jobs) {
			if (!vehicleId || !player || j.phase != Phase::Following || j.vehicle != vehicleId) continue;
			const uptr veh = RefById(j.vehicle);
			if (!veh) return false;
			const auto vp = At<game::Vec3>(veh, off::kRefPosition);
			vcontract::Assign(vehicleId, vcontract::Find(pursuit ? "pursuit" : "escort"), vp.x, vp.y);
			return ApplyProfile(j, pursuit ? Profile::Pursuit : Profile::Escort, player);
		}
		return false;
	}

	vcontract::SeatState ExitStateOf(std::uintptr_t actor)
	{
		const std::uint32_t id = At<std::uint32_t>(actor, off::kFormId);
		if (const ExitTask* t = ExitTaskFor(id)) return t->stage == 1 ? vcontract::SeatState::WaitingToLand : vcontract::SeatState::Exiting;
		for (const auto& j : g_jobs) {
			if (j.phase == Phase::Boarding && j.npc == id && !InVehicle(actor)) return vcontract::SeatState::Walking;  // on its way (the quest package) or getting in
		}
		if (!InVehicle(actor)) {
			for (const auto r : g_exitRefused) {
				if (r == id) return vcontract::SeatState::Refused;
			}
			return vcontract::SeatState::None;
		}
		for (const auto& j : g_jobs) {
			if (j.phase != Phase::Idle && j.npc == id) return j.phase == Phase::Boarding ? vcontract::SeatState::Seating : vcontract::SeatState::Driver;
		}
		return vcontract::SeatState::Passenger;
	}

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
		g_riders.clear();
		g_seatLogged.clear();
		g_told = false;
	}

	bool AimFor(std::uint32_t shooterFormId, double& x, double& y, double& z)
	{
		if (!shooterFormId) return false;
		const ULONGLONG now = GetTickCount64();
		std::lock_guard lk(g_aimLock);
		for (const auto& a : g_aimSnap) {
			if (!a.fire || now - a.at > kAimSnapMs) continue;
			if (shooterFormId != a.vehicle && shooterFormId != a.npc) continue;
			x = a.x;
			y = a.y;
			z = a.z;
			return true;
		}
		return false;
	}
}
