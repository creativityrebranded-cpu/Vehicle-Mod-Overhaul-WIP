#include "fvo_addon_api.h"
#include "npcvehicles.h"
#include "trafficpool.h"
#include "loadedarea.h"
#include "relocation.h"
#include "relocationapi.h"
#include "trafficpair.h"
#include "fvolimits.h"
#include "pilotlayout.h"
#include "prefetch.h"
#include "features.h"
#include "overrides.h"
#include "profileclass.h"
#include "driverdiscovery.h"
#include "boardinggate.h"
#include "trafficcycle.h"
#include "lifecycle.h"
#include "routeguidance.h"

#include "addresses.h"
#include "bindings.h"
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
#include "runtimecells.h"
#include "pilotquest.h"
#include "vcontract.h"
#include "vehicleflight.h"
#include "vehiclediag.h"
#include "vehiclekeep.h"
#include "environment.h"
#include "targetsession.h"
#include "traffic.h"
#include "vehiclehits.h"
#include "vehicleworld.h"
#include "manoeuvre.h"
#include "combat_ai.h"
#include "native_personalities.h"
#include "combat_relations.h"
#include "integrationlogic.h"
#include "coverfight.h"
#include "hulllogic.h"
#include "navmap.h"
#include "cellbake.h"
#include "farrail.h"
#include "realvehicles.h"
#include "fvopaths.h"
#include <ShlObj.h>
#include <string>
#include <unordered_map>

#include <Windows.h>
#include <intrin.h>

#include <cmath>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include <algorithm>
#include <atomic>
#include <limits>
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
	FvoTrafficCycle g_trafficCycle = nullptr;
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
		constexpr int kJobs = fvolimits::kVehicles;  // NPC vehicles at once (vehicleflight pilots 1..kJobs; fvolimits.h)
		static_assert(kJobs == pilotlayout::kSlots && kJobs == prefetch::kSlotPilotCount, "a pilot quest slot and probe slots per job");
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
		constexpr ULONGLONG kIdleAboardMs = 10000;  // the aboard watchdog's window and line interval
		// A traffic trip (task 9) that has not moved 2 m for this long is ended (2026-10-29 log: a cold-start vehicle fell off its
		// spot, backed into a wall and sat "IDLE ABOARD" 264 s with its trip still holding a traffic slot; nothing ever ended it).
		// The driver stays aboard; the traffic pool starts it again (a few times at most).
		constexpr ULONGLONG kTripStuckMs = 90000;

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
			bool scheduledTraffic = false;
			Phase phase = Phase::Idle;
			int pilot = 1;
			std::uint32_t npc = 0, vehicle = 0;
			ULONGLONG since = 0, nearSince = 0;
			bool parked = false;               // following: landed because the player stood still (stays aboard)
			ULONGLONG relaunchAt = 0;          // following: no relaunch before this (after a collision stop)
			double stillX = 0, stillY = 0;
			ULONGLONG stillSince = 0;
			double watchX = 0, watchY = 0, watchZ = 0;  // the aboard watchdog: where the vehicle stood when it last moved
			ULONGLONG watchAt = 0, watchLog = 0;
			ULONGLONG watchTickAt = 0;  // the last idle-aboard check (paused time is skipped)
			bool directTried = false;
			bool standing = false;           // the get-in only stood the NPC up: ask again shortly
			bool pkgSampled = false;         // the 1 s package check of this boarding was logged
			bool reasked = false;            // the get-in was asked a second time (its package was gone at 1 s)
			bool calmChecked = false;        // walk boarding: the 2.5 s Ignore Combat check ran (calm only if the NPC did not come closer)
			uptr combatReq = 0;              // boarding as a combat move: our reference to the CombatMovementRequestActivateTarget (0 = none)
			ULONGLONG combatReqAt = 0, combatDoneAt = 0;
			int combatReqState = -1;         // its last state (flags >> 29), for the log
			bool combatSeatTried = false;    // the instant seat at the vehicle ran (once per request)
			bool moved = false;                // moved next to the vehicle (step 3)
			double startDist = 0;              // NPC -> vehicle when boarding began
			ULONGLONG directAt = 0;
			double tx = 0, ty = 0, tvx = 0, tvy = 0;  // the followed target's last position and smoothed velocity
			ULONGLONG aggroAt = 0;             // last combat request (attack)
			Profile profile = Profile::Escort;
			bool autoCombat = false;           // the combat profile was chosen because a fight started (cleared when it is over)
			ULONGLONG fightAt = 0;             // last frame with an enemy picked
			bool fighting = false;             // an enemy is picked now (the follower row shows the combat profile)
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
			unsigned bursts = 0;
			coverfight::Burst weaponBurst;
			ULONGLONG fireBlockedLogAt = 0;
			double aimX = 0, aimY = 0, aimZ = 0, aimVx = 0, aimVy = 0, aimVz = 0;  // the player's last position and smoothed velocity (aim lead)
			ULONGLONG aimAt = 0;
			ULONGLONG tAt = 0;
			std::uint32_t resumeFoe=0, lastEnemyActor=0, squadFocus=0;
			combatrelations::BoardingFight boardingFight;
			integrationlogic::Controller controller=integrationlogic::Controller::Navigation;
			std::uint32_t foe = 0;             // the combat target (an actor; its vehicle is aimed at when it sits in one), 0 = none
			int foeTier = -1;                  // 0 = in a vehicle, 1 = on foot
			ULONGLONG foeAt = 0;               // last pick
			double foeX = 0, foeY = 0, foeVx = 0, foeVy = 0;  // the target's last position and smoothed velocity (intercept)
			ULONGLONG foeSeenAt = 0;
			int defMode = -1;                  // escort in a fight: 0 attack, 1 shield, 2 intercept, 3 overwatch
			ULONGLONG exitWaitSince=0;
			ULONGLONG no3DSince = 0;           // aboard but the vehicle has no 3D yet: the pilot waits for it (0 = not waiting)
			int passTries = 0; bool passMarked = false; ULONGLONG passAt = 0, passLoadAt = 0;  // observed far-cell membership
            bool questPending=false;
			ULONGLONG flyOnSince = 0;  // flying with the driver's model out of the loaded cells (trafficpair::FlyWithoutDriverModel)
			int flyOnPins = 0;
			int order = 0;
			double orderX=0, orderY=0, orderZ=0;                     // npcvehicles::CrewOrder: a crew job on the player's own vehicle (0 none)
			bool rescueParked = false;         // rescue: landed beside the player
			bool scoutReported = false;        // scout: the enemies at the marker were reported
			bool waiting = false;              // the player is indoors: parked, waiting at the door
			int pMode = -1;                    // pursuer in a fight: 10 attack / flank, 12 intercept, 13 overwatch, 14 retreat, 15 block
			int task = 0;                      // civil task: 0 none, 1 transit, 2 patrol, 3 return home, 4 parking, 9 addon scripted trip
			bool coldStart = false;            // a cold-start pool driver (seated at creation, not by the game's get-in): its AI's dismount wish is ignored on a trip
			bool armed = false;                // pre-armed traffic pilot: made in the same call as its vehicle; flies the first frame the vehicle has a model
			ULONGLONG armedAt = 0;
			bool coldStartLogged = false;
			int reseats = 0; ULONGLONG reseatAt = 0;  // a cold-start driver knocked out of its seat on a trip: instant seats tried
			ULONGLONG seatBrakeSince = 0;             // holding still while that driver is out of the seat (trafficpair::SeatBrake)
			ULONGLONG cellBringAt = 0;                // last BringDriverIntoCell
			int boardSnaps = 0; ULONGLONG seatWatchAt = 0, dropTraceUntil = 0; bool wasAboard = false;
			bool haveSeatOff = false; float seatOff[3] = {}; float seatYaw = 0; unsigned seatHolds = 0;  // the driver's seat, vehicle frame                      // cold-start boarding snaps tried before the pilot takes the vehicle
			int pins = 0; double pinMax = 0;          // an unloaded cold-start driver put back on its vehicle (drifted away while unloaded)
			FvoTripClaim trip{};               // task 9: an addon's scripted trip (fvo_addon_api.h), e.g. the ring traffic
#ifndef FVO_RELEASE
            std::uint64_t factoryLease = 0;     // a live private claim held by this admitted job
            bool factoryReleased = false;
            bool factoryStock = false;        // route-free qualification; no seated transfer service
#endif
			bool repeatTraffic = false;
			trafficcycle::Dwell trafficDwell;
			double taskX = 0, taskY = 0;       // transit: where to; parking: the clear spot
			std::uint32_t startPlace = 0;      // transit: the owner of the place it set out from (0: open ground)
			double homeX = 0, homeY = 0;       // where the vehicle was taken from
			bool haveHome = false;
			int taskStep = 0;
			ULONGLONG taskAt = 0;
			std::uint32_t lead = 0;            // convoy (task 8): the lead vehicle
			bool retreating = false;           // pursuer near its hit limit: pulling back to get out and fight on foot
			bool defDefensive = false;         // DefendGoal: this escort is held defensive (second escort, high threat)
			manoeuvre::Mind man;               // the vehicle-vs-vehicle manoeuvre state (manoeuvre.h)
			bool manOn = false;                // manoeuvring this frame
			double manMx = 0, manMy = 0, manMvx = 0, manMvy = 0, manFx = 0, manFy = 0, manFvx = 0, manFvy = 0;  // own / enemy position and smoothed velocity
			ULONGLONG manAt = 0;
			std::uint32_t manFoe = 0;
			combatai::State ai;                // the full combat AI (combat_ai.h)
			bool aiOn = false;                 // the full AI flies it this frame
			bool aiFire = false;               // its weapon zone says fire
			double aiAimX = 0, aiAimY = 0, aiAimZ = 0;
			bool aiPlan = false;               // CB-2: a combat line drives it (the plan moves it; only the aim pitch is sent)
			ULONGLONG aiAt = 0;
			ULONGLONG stunUntil = 0;           // rammed: out of control until then
			double stunX = 0, stunY = 0, stunSpin = 0;
			brain::Role role = brain::Role::Engager;
			int roleSlot = 0;
			std::uint32_t coverFor = 0;
			int squad = -1;                    // native-relation squad slot (neutral actors remain separate)
			bool sideEvents[3] = {};           // this frame: an ally wrecked, an ally driver killed, the leader killed
			bool reinforced = false;
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
		std::atomic<std::uint32_t> g_nativeExits[kJobs];
		std::atomic<std::uint32_t> g_ignoredColdExit{ 0 };  // logged on the main thread
		thread_local bool g_allowDismount = false; // only our own Leave() takes a job's NPC out of its vehicle; per thread: the service and the controller both exit drivers, and the hook runs inside the call
		// Vehicle weapon (the game's own path, 1.16.244): the player's VehicleHandler (PlayerControls, vtable
		// 0x144C43E58) sets its input block at +0x80 (fire = +0x9F, from "VehicleFireWeapon"), and each frame
		// FUN_1412BFEC0 hands it to the vehicle: FUN_1412C0550(*(FUN_1423FF640() + 0x8B0), &handler, &vehicleFormId) ->
		// the vehicle's component -> FUN_141324650(component + 0x20, handler + 0x80): fire rising -> FUN_14131D120 ->
		// the weapon's FUN_14128EE90 (start); fire falling -> each weapon's vfunc +0x58 (stop). An NPC vehicle gets the
		// same input from a handler-shaped block of ours.
		constexpr std::uint32_t kVehicleSystem = 0x023FF640, kSubmitVehicleInput = 0x012C0550;
		// 2026-10-21: longer bursts, shorter gaps (1.2 s / 1.5 s fired 44 % of the time, about 2 shots a burst). The cone stays 8 deg (the approved
		// proposal); the nose is now held on the aim point while moving (manoeuvres), and the shots are also turned onto it at launch (vehicleaim).
		// The fire gate (2026-10-21, the user: "vehicles spend too much time nose pointing to shoot"): shots are re-aimed at the
		// target at launch (vehicleaim), so the nose only needs to be roughly on it: 25 deg, widening up to 40 deg up close (a
		// near target fills more of the view); pitch 1.5x. Never wider, so a gun never fires sideways or backwards.
		constexpr double kFireCone = 0.44;         // rad (25 deg) yaw; pitch 1.5x
		constexpr double kFireConeNear = 0.70;     // rad (40 deg) at close range
		double FireCone(double dist) { return coverfight::FireCone(dist); }
		// Aim: the weapon fires along the vehicle's nose, so the whole vehicle aims. The aim point leads the player by
		// the shot's flight time (kShotSpeed) and sits at chest height.
		// Lead speed comes from the native projectile flight, through vehiclehits::ShotTime.
		constexpr double kAimHeight = 1.2;         // m above the player's feet
		// The gate (FUN_141317C90): a vehicle's component applies input only when it is the player's vehicle
		// (component +0x579 = player vehicle == this). FUN_141324650(component, input) returns at once otherwise, so
		// an NPC vehicle never took our fire input (2026-10-17 17:53-17:57: 51 bursts sent, no shot). Hooked: for our
		// own input blocks only, the gate is opened for that one call.
		constexpr std::uint32_t kApplyVehicleInput = 0x01324650;
		constexpr double kFireRange = 100.0;       // m
		bool g_keyChosen = false;
		int g_walkKey = 0, g_exitKey = 0, g_aggroKey = 0;
		std::uint32_t g_ctrlTarget = 0;
		bool g_walkDown = false, g_exitDown = false;
#ifndef FVO_RELEASE
        int g_pairMoveKey=0;
        bool g_pairMoveDown=false;
        int g_fastSeatKey=0;
        bool g_fastSeatDown=false;
        std::uint32_t g_testPairVehicle=0,g_testPairDriver=0;
        int g_farKey=0;   // far tier test key (notes/FAR_TIER_TEST_20261121.md)
        bool g_farDown=false;
#endif
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
		AimSnap g_aimSnap[kJobs + 2];
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
		// Whether the game can find a seat of `vehicle` for `actor` at all: the seat picker FUN_1412B2C40 (the first seat marker
		// whose keyword the actor carries), else its fallback FUN_1412B2A90 (a seat from the vehicle's 3D). With neither, the seat
		// link FUN_141995460 reads a null seat (2026-10-28 17:56 fault at Starfield.exe+0x1A9C41A, read 0x8: an NPC given the
		// REV-8's seat keyword was snapped into an FE00780B vehicle). Called direct: FVO hooks this picker at call sites only.
		bool HasSeatFor(uptr actor, uptr vehicle)
		{
			__try {
				if (EngineFn<uptr (*)(uptr, uptr)>(0x012B2C40)(actor, vehicle)) return true;
				return EngineFn<uptr (*)(uptr)>(0x012B2A90)(vehicle) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		bool EnsureSeatKeyword(uptr actor,uptr vehicle);
        bool IsActorRef(uptr actor);
		int SnapSeatNative(uptr actor, uptr vehicle, int& seated)
		{
			seated = -1;
			uptr link = 0;
			// The seat link's interaction start (FUN_141A9C350) reads the actor's AI process (+0x228) +8 unchecked: an actor without
			// a process faulted there (2026-10-28 17:56, Starfield.exe+0x1A9C41A, rax 0, read 0x8). Not loaded enough: no seat now.
			bool hasProcess = false;
			__try { hasProcess = At<uptr>(actor, 0x228) != 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) {}
			if (!hasProcess) {
				logger::Write("NPC vehicles: NPC %08X has no AI process yet: instant seat into %08X skipped", At<std::uint32_t>(actor, off::kFormId),
					At<std::uint32_t>(vehicle, off::kFormId));
				seated = 0;
				return 0;
			}
			if (!HasSeatFor(actor, vehicle)) {
				logger::Write("NPC vehicles: no seat in vehicle %08X for NPC %08X (no seat keyword it carries, no 3D seat): instant seat skipped",
					At<std::uint32_t>(vehicle, off::kFormId), At<std::uint32_t>(actor, off::kFormId));
				seated = 0;
				return 0;
			}
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

		int SnapSeat(uptr actor, uptr vehicle, int& seated)
		{
			if(!lifecycle::InService()) {
				const unsigned id=At<unsigned>(actor,off::kFormId),vid=At<unsigned>(vehicle,off::kFormId);
				const auto result=lifecycle::Poll("seat:"+std::to_string(id)+":"+std::to_string(vid),id,[=]()->lifecycle::Work {
					const auto a=lifecycle::Keep(actor,id),v=lifecycle::Keep(vehicle,vid);if(!a||!v)return {};
					return [=]()->std::uint64_t {
						if(!lifecycle::InService())return 0;
						const auto ar=reinterpret_cast<uptr>(a.get()),vr=reinterpret_cast<uptr>(v.get());
						if(!IsActorRef(ar)||IsActorRef(vr))return 0;EnsureSeatKeyword(ar,vr);int done=0;SnapSeat(ar,vr,done);return done==1?1:0;
					};
				});
				seated=result?int(*result):0;return seated;
			}
			return SnapSeatNative(actor,vehicle,seated);
		}

        int InstantSeat(uptr actor,uptr vehicle,int& seated) {
            if(environment::EntrySite(vehicle).verdict!=environment::Verdict::Safe){seated=0;return 0;}
            return SnapSeat(actor,vehicle,seated);
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
			if(!actor||!vehicle||!IsActorRef(actor)||IsActorRef(vehicle))return false;
			if (CanMount(actor, vehicle)) return true;
			if(!lifecycle::InService()){
				const unsigned id=At<unsigned>(actor,off::kFormId),vid=At<unsigned>(vehicle,off::kFormId);
				lifecycle::Post("seat-keyword:"+std::to_string(id),id,[=]()->lifecycle::Work {
					const auto a=lifecycle::Keep(actor,id),v=lifecycle::Keep(vehicle,vid);if(!a||!v)return {};
					return [=]()->std::uint64_t {return lifecycle::InService()&&EnsureSeatKeyword(reinterpret_cast<uptr>(a.get()),reinterpret_cast<uptr>(v.get()))?1:0;};
				});return false;
			}
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
		// SL-0 / SL-1 / SL-2 (2026-11-08, the seat ladder; notes/seat-ladder-20261108/CHANGE_CARDS.md). The marker a laddered vehicle uses
		// (vehicle form id -> marker index; absent: the game's own choice), the marker that seated per model (+1; this session), logged tables.
		std::mutex g_ladderLock;
		std::unordered_map<std::uint32_t, std::uint32_t> g_ladderMarker, g_learnedMarker;
		std::unordered_map<std::uint32_t, bool> g_seatTableLogged;
		// A marker's keyword when the marker is enabled and its keyword is a keyword form (type 4); 0 otherwise.
		uptr MarkerKeyword(uptr base, std::uint32_t i, std::uint32_t* countOut = nullptr)
		{
			std::uint32_t mask = 0, count = 0; uptr kw = 0;
			if (!base || !ReadSeat(base, i, mask, count, kw)) return 0;
			if (countOut) *countOut = count;
			if (i >= count || i >= 32 || !((mask >> i) & 1u) || !kw) return 0;
			__try { return At<std::uint8_t>(kw, 0x2E) == 4 ? kw : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

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
		// Where the model is drawn (the 3D root's world position), or false without a model.
		bool ScenePos(uptr r, game::Vec3& out)
		{
			__try {
				const uptr loaded = At<uptr>(r, off::kRefLoadedData);
				const uptr node = loaded ? At<uptr>(loaded, off::kLoadedData3D) : 0;
				if (!node) return false;
				out = At<game::Vec3>(node, off::kNodeWorldT);
				return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// Puts an actor's reference on the vehicle's position with the game's own position setter (what the loaded-area sync uses).
		// For an actor without a model only (pure data, nothing visible moves).
		bool PinToVehicle(uptr actor, uptr vehicle)
		{
			__try {
				float p[3];const auto v = At<game::Vec3>(vehicle, off::kRefPosition);p[0]=v.x;p[1]=v.y;p[2]=v.z;
				if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) return false;
				float* rp = reinterpret_cast<float*>(actor + off::kRefPosition); rp[0] = p[0]; rp[1] = p[1]; rp[2] = p[2];  // direct write (the setter takes a seated actor out of its seat)
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// What the game holds about a driver's seat (2026-10-30 probe: sitting drivers, sit state 3, still did not move with their
		// vehicle). Process level (process +0xE7: 0 high .. 3 low), the snapped-into-the-seat flag (middle-high +0x5A6, set by
		// FUN_1419C3980, cleared by FUN_1419C3B50), the furniture handles (middle-high +0x424 / +0x428) and actor flags +0x378.
		void SeatProbe(uptr actor, char* out, std::size_t n, uptr vehicle = 0)
		{
			int level = -1, snapped = -1; std::uint32_t h1 = 0, h2 = 0, flags = 0, dCell = 0, vCell = 0; int dState = -1, vState = -1, d3D = -1, v3D = -1;
			__try {
				// +0xD2: FUN_1419C3B50 takes a driver out of its snap when the furniture's byte is not 1.
				d3D = At<std::uint8_t>(actor, 0xD2);
				if (vehicle) v3D = At<std::uint8_t>(vehicle, 0xD2);
				// Parent cells (2026-10-30: the game picks the process level from the actor's parent cell, FUN_141935730; FVO moves
				// drivers' positions without changing their cell).
				if (const uptr c = At<uptr>(actor, off::kRefParentCell)) { dCell = At<std::uint32_t>(c, off::kFormId); dState = At<std::uint8_t>(c, off::kCellState); }
				if (vehicle) if (const uptr c = At<uptr>(vehicle, off::kRefParentCell)) { vCell = At<std::uint32_t>(c, off::kFormId); vState = At<std::uint8_t>(c, off::kCellState); }
				flags = At<std::uint32_t>(actor, 0x378);
				const uptr proc = At<uptr>(actor, 0x228);
				if (proc) {
					level = At<std::uint8_t>(proc, 0xE7);
					const uptr mh = At<uptr>(proc, 8);
					if (mh) { snapped = At<std::uint8_t>(mh, 0x5A6); h1 = At<std::uint32_t>(mh, 0x424); h2 = At<std::uint32_t>(mh, 0x428); }
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {}
			std::snprintf(out, n, "process level %d, seat snapped %d, furniture handles %08X/%08X, flags %08X, D2 driver %d vehicle %d, driver cell %08X (state %d), vehicle cell %08X (state %d)%s",
				level, snapped, h1, h2, flags, d3D, v3D, dCell, dState, vCell, vState, vehicle && dCell != vCell ? " CELLS DIFFER" : "");
		}
		// The reference's parent cell is attached (states 9 and 10; FUN_140574EF0 treats them as fully loaded).
		bool CellAttached(uptr ref)
		{
			__try { const uptr c = At<uptr>(ref, off::kRefParentCell); if (!c) return false; const auto s = At<std::uint8_t>(c, off::kCellState); return s == 9 || s == 10; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		int ProcessLevel(uptr actor)
		{
			__try { const uptr proc = At<uptr>(actor, 0x228); return proc ? At<std::uint8_t>(proc, 0xE7) : -1; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		// Moves an actor's reference and model root to `target` (world) and turns its reference to `yaw` (what the loaded-area
		// sync does for vehicles). For a seated driver the game has stopped moving.
		bool PlaceSeatedNative(uptr actor, const float target[3], float yaw)
		{
			__try {
				const float* p = reinterpret_cast<const float*>(actor + off::kRefPosition);
				const float d[3] = { target[0] - p[0], target[1] - p[1], target[2] - p[2] };
				// Written directly, not through the game's position setter (2026-10-30 07:49 run: drivers held through the setter
				// were out of their seat, aboard 0, within a second: moving a seated actor that way takes it out of the furniture).
				float* rp = reinterpret_cast<float*>(actor + off::kRefPosition);
				rp[0] = target[0]; rp[1] = target[1]; rp[2] = target[2];
				At<float>(actor, off::kRefRotation + 8) = yaw;
				if (const uptr loaded = At<uptr>(actor, off::kRefLoadedData)) if (const uptr root = At<uptr>(loaded, off::kLoadedData3D)) {
					float* w = reinterpret_cast<float*>(root + off::kNodeWorldT);
					float* l = reinterpret_cast<float*>(root + off::kNodeLocalT);
					for (int i = 0; i < 3; ++i) { w[i] += d[i]; l[i] += d[i]; }
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// Worked out in the service from where the vehicle is at that moment: a point taken by the controller is a frame behind a flying
		// vehicle. Judged again there too: since it was asked, the driver may have got out, or the game may be carrying it again.
		bool PlaceSeatedNow(uptr actor, uptr vehicle, const trafficpair::SeatPose& pose)
		{
			if (InVehicle(actor) != vehicle) return false;
			if (pose.hold ? (ProcessLevel(actor) == 0 && CellAttached(actor)) : !trafficpair::NeedsBringing(SitState(actor), Dist3(actor, vehicle))) return false;
			const auto v = At<game::Vec3>(vehicle, off::kRefPosition);
			const float heading = At<game::Vec3>(vehicle, off::kRefRotation).z;
			float target[3];trafficpair::SeatPoint(v.x, v.y, v.z, heading, pose, target);
			return PlaceSeatedNative(actor, target, heading + pose.yaw);
		}
		bool PlaceSeated(uptr actor, uptr vehicle, const trafficpair::SeatPose& pose)
		{
			if(!lifecycle::InService()) {
				const unsigned id=At<unsigned>(actor,off::kFormId),vid=At<unsigned>(vehicle,off::kFormId);
				return lifecycle::Post("seat-pose:"+std::to_string(id),id,[=]()->lifecycle::Work {
					const auto a=lifecycle::Keep(actor,id),v=lifecycle::Keep(vehicle,vid);if(!a||!v)return {};
					return [=]()->std::uint64_t {
						return lifecycle::InService()&&PlaceSeatedNow(reinterpret_cast<uptr>(a.get()),reinterpret_cast<uptr>(v.get()),pose)?1:0;
					};
				});
			}
			return PlaceSeatedNow(actor,vehicle,pose);
		}
		// Driver to vehicle for the seat check (2026-10-29 21:27 run: 44 of 91 traffic pilots were stopped as "knocked out" while
		// the game still had the driver in the seat). A seated driver's model rides with the vehicle, but its reference position
		// is only refreshed when the game processes that actor, so it lags behind a flying vehicle. Both models: their distance.
		double SeatDist(uptr actor, uptr vehicle)
		{
			game::Vec3 a{}, v{};
			if (ScenePos(actor, a) && ScenePos(vehicle, v)) {
				const double dx = double(a.x) - v.x, dy = double(a.y) - v.y, dz = double(a.z) - v.z;
				return std::sqrt(dx * dx + dy * dy + dz * dz);
			}
			return Dist3(actor, vehicle);
		}
		// Seated for the traffic seat check (2026-10-29 22:36 run: "aboard 1, sit 0, model distance 0.0 m" on 122 checks; the game
		// reports sit state 0 until it next processes the actor, e.g. still behind the loading screen). A driver that is aboard and
		// whose model sits on the vehicle's (under 1 m) is in the seat; a driver standing beside its vehicle is metres away.
		bool SeatedNow(uptr actor, uptr vehicle, bool aboard)
		{
			const double d = SeatDist(actor, vehicle);
			return trafficpair::ReadyAt(Has3D(vehicle), Has3D(actor), aboard, SitState(actor), d);  // sitting (sit state 3), not merely linked
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
			// The escort/pursuit key was removed. The context engine handles it now.
			g_walkKey = controlmap::ClaimFirst("npcveh", "board (debug)", { VK_OEM_1, VK_OEM_4 });  // ; first
			g_exitKey = controlmap::Claim("npcveh", "exit (debug)", VK_INSERT) ? VK_INSERT : 0;
#ifndef FVO_RELEASE
            g_pairMoveKey=controlmap::Claim("npcveh","move seated test pair (debug)",VK_NUMPAD3)?VK_NUMPAD3:0;
            g_fastSeatKey=controlmap::Claim("npcveh","spawn and time a seated test driver",VK_OEM_PERIOD)?VK_OEM_PERIOD:0;
            logger::Write("Fast seat test: aim at an empty vehicle and press %s",
                g_fastSeatKey?controlmap::KeyName(g_fastSeatKey).c_str():"no available key");
            logger::Write("Pair move test: press %s after the debug-boarded NPC is seated and stopped",
                g_pairMoveKey?controlmap::KeyName(g_pairMoveKey).c_str():"no available key");
            // Starfield's own bindings can hold Numpad 1 (2026-11-21 log: no available key), so later keys are tried in turn.
            g_farKey=controlmap::ClaimFirst("npcveh","far tier test: move the faced seated pair past the loaded wall (debug)",
                {VK_NUMPAD1,VK_NUMPAD5,VK_NUMPAD6,VK_NUMPAD9,VK_NUMPAD7,VK_NUMPAD8});
            logger::Write("Far tier test: aim at a seated NPC (or use the board pair) and press %s (stage 1 no rail; again = rail; again = walk back); logs OBS system=far.test",
                g_farKey?controlmap::KeyName(g_farKey).c_str():"no available key");
#endif
			g_aggroKey = 0;
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

		bool Aggro(uptr actor, uptr target);
        bool HostileTo(uptr actor, uptr target);
        uptr HandleRef(std::uint32_t handle);
        void CombatBoardRelease(uptr& req);
        int CombatBoardState(uptr req);
        const char* CombatBoardStateName(int st);
        uptr CombatBoardStart(uptr npc, uptr vehicle, const char*& why);
        constexpr ULONGLONG kCombatBoardMs = 25000, kCombatStartMs = 3000, kCombatActivatedMs = 8000;  // combat boarding: run limit, start limit, after activating
        bool PlayerTeam(uptr actor);
        bool InCombat(uptr actor);
        std::uint32_t BoardingEnemy(uptr npc) {
            if(const auto enemy=NativeEnemyId(npc))return enemy;
            const auto target=HandleRef(At<std::uint32_t>(npc,0x298));
            if(target&&IsActorRef(target)&&!Dead(target)&&target!=npc&&!(PlayerTeam(npc)&&PlayerTeam(target))&&
                (InCombat(npc)||HostileTo(npc,target)||HostileTo(target,npc)))return At<std::uint32_t>(target,off::kFormId);
            const auto player=Ptr(addr::kPlayerCharacterPtr);
            return player&&InCombat(npc)&&!(PlayerTeam(npc)&&PlayerTeam(player))&&(HostileTo(npc,player)||HostileTo(player,npc))?At<std::uint32_t>(player,off::kFormId):0;
        }
        void ResumeCombat(Job& j) {
            const auto npc=RefById(j.npc), enemy=RefById(j.resumeFoe);
            // StopCombat can erase the hostility it is meant to pause. The captured native enemy is the evidence;
            // do not demand a still-active fight, or another NPC fighting the player, after boarding.
            if(npc&&enemy&&!Dead(enemy)&&!(PlayerTeam(npc)&&PlayerTeam(enemy))) {
                const auto now=GetTickCount64();
                j.boardingFight.Remember(j.resumeFoe,now);j.aggroAt=now;
                const bool restored=Aggro(npc,enemy);
                logger::Write("NPC vehicles: [%d] boarding fight restored: %08X -> %08X (%s); target retained independently of other fighters",j.pilot,j.npc,j.resumeFoe,restored?"requested":"retry needed");
            }
            j.resumeFoe=0;
        }
        void LeaveFarCell(uptr actor);
        void Set(Job& j, Phase p)
		{
			logger::Write("OBS system=npc.assignment vehicle=%08X driver=%08X pilot=%d event=transition from=%s to=%s task=%d passenger=%d cold_start=%d",j.vehicle,j.npc,j.pilot,Name(j.phase),Name(p),j.task,j.passenger,j.coldStart);
			logger::Write("NPC vehicles: [%d] %s -> %s (NPC %08X, vehicle %08X)", j.pilot, Name(j.phase), Name(p), j.npc, j.vehicle);
			if (p == Phase::Idle) { if(j.coldStart)pilotquest::Release(j.pilot-1);lifecycle::Cancel(j.npc);lifecycle::Cancel(j.vehicle);j.passMarked = false; LeaveFarCell(RefById(j.npc)); }
			if((p==Phase::Following||p==Phase::Idle)&&j.resumeFoe) ResumeCombat(j);
            vehicleflight::ClearPilotCommand(j.pilot);
            if (p != Phase::Boarding && j.combatReq) CombatBoardRelease(j.combatReq);
            j.phase = p;
            j.since = GetTickCount64();
			if (p != Phase::Following) { j.foe = 0; j.foeTier = -1; j.foeAt = 0; j.rescueParked = j.scoutReported = j.waiting = j.retreating = false; j.pMode = -1;
				j.manOn = false; j.man = manoeuvre::Mind{}; j.manAt = 0; j.aiOn = false; j.ai = combatai::State{}; j.role = brain::Role::Engager; }
			if (p == Phase::Idle) { j.resumeFoe=j.lastEnemyActor=j.squadFocus=0; j.boardingFight={}; j.exitWaitSince=0; j.order = 0; j.task = 0; j.haveHome = false;
#ifndef FVO_RELEASE
                j.factoryLease = 0; j.factoryReleased = false; j.factoryStock = false;
#endif
            }
			if (p == Phase::Idle && j.vehicle && !j.passenger) {  // the job is over: an escort / pursuit assignment ends with it (other profiles are left alone)
				vcontract::Unassign(j.vehicle, vcontract::Class::Combat);
				vcontract::Unassign(j.vehicle, vcontract::Class::Weapons);
				const vcontract::Profile* ap = vcontract::AssignedTo(j.vehicle);
				if (ap && std::strcmp(ap->name, "escort") == 0) vcontract::Unassign(j.vehicle, vcontract::Class::Navigation);
			}
			if (p == Phase::Boarding) {
				j.exitWaitSince=0;
				j.boardingFight={};
				j.cmdTries = 0;
				j.cmdTryAt = 0;
				j.cmdInstant = false;
				j.calmChecked = false;
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
			// A cold-start driver keeps the pilot quest package for its whole trip (2026-10-30 07:16 run: once drivers really sat,
			// sit state 3, their own AI stood them up (sit 4) seconds after the package was released at the trip start, and they fell
			// out in mid-air; the 2026-10-28 runs saw the same wish to dismount right after each trip start).
			if (j.cmd && p != Phase::Boarding && !(j.coldStart && p == Phase::Following)) pilotquest::Release(j.pilot - 1);
			if (p == Phase::Idle) {
				j.scheduledTraffic = false;
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
            const auto site=environment::BoardingSite(vehicle);
            if(site.verdict!=environment::Verdict::Safe)return false;

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
			float pos[3]={float(site.x),float(site.y),float(site.z)};
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
		bool ApplyProfile(Job& j, Profile to, uptr player, bool provokePlayer=true);
		// escort / pursuit are profiles of the shared table (profiles.cpp); an assignment of one of them decides the job's behaviour.

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
		alignas(16) std::uint8_t g_fireHandler[kJobs + 1][0xC0] = {};  // the last one: the player's own rover (FireVehicleWeapon)

		bool SubmitFire(int slot, std::uint32_t vehicleId, bool fire)
		{
			if (fire && vehiclehits::Wrecked(vehicleId)) fire = false;  // a wreck's weapon is dead
			// The one Weapons gate for the player's seat (profileclass "never"): every automatic shot comes through here, and a vehicle the player
			// drives never fires by itself; the player shoots from it.
			if (fire) {
				const uptr player = Ptr(addr::kPlayerCharacterPtr);
				const uptr driven = player && VehicleDriverId(vehicleId)==At<std::uint32_t>(player,off::kFormId) ? RefById(vehicleId) : 0;
				if (!profileclass::MayFire(profileclass::Weapons::Free, driven && At<std::uint32_t>(driven, off::kFormId) == vehicleId)) {
					static ULONGLONG said = 0;
					if (GetTickCount64() - said > 10000) { said = GetTickCount64(); logger::Write("NPC vehicles: vehicle %08X weapon refused (the player drives it)", vehicleId); }
					fire = false;
				}
			}
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
			const double t = vehiclehits::ShotTime(j.vehicle,dist);
			ax = pp.x + j.aimVx * t;
			ay = pp.y + j.aimVy * t;
			az = pp.z + kAimHeight + j.aimVz * t;
		}

		// Close-range aiming changes yaw/pitch only while nearly stationary at the planned position.
		void AttackRun(Job& j, uptr player, uptr vehicle)
		{
            const auto progress=vehicleflight::ProgressFor(j.vehicle);
            if(!progress.active)return;  // (2026-10-21, the proposal: no "nearly stopped" rule; aims while moving)
            double x,y,z;AimPoint(j,player,vehicle,x,y,z);
            const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
            const double heading=At<game::Vec3>(vehicle,off::kRefRotation).z;
            vehicleflight::Command c;c.set=vehicleflight::Command::kYaw|vehicleflight::Command::kPitch;
            c.yawRate=std::clamp(1.6*WrapAngle(std::atan2(x-pos.x,y-pos.y)-heading),-1.2,1.2);
            c.pitch=std::clamp(std::atan2(z-pos.z,std::max(1.0,std::hypot(x-pos.x,y-pos.y))),-0.6,0.6);
            vehicleflight::PilotCommand(j.pilot,c);

		}

		// ---- Combat targets (the user, 2026-10-19: "fighting enemies on vehicles is the number 1 priority"; escorts defend the player). Every armed NPC
		// vehicle picks its target in tiers: 0 an enemy sitting in a vehicle (the vehicle is in the fight), 1 an enemy on foot; the closest within a tier.
		// Escort = defender: the player's side; enemies are fighting actors hostile to the player or to the driver, never the player or a follower; only
		// while the player or the driver is in combat, and never farther than kDefendLeash from the player. Pursuit: the player's enemy; the player and
		// the followers, or anyone fighting the driver; with nothing picked it attacks the player (the old behaviour).
		bool HostileTo(uptr actor, uptr target);
		bool IsFollower(uptr player, uptr actor);
		bool PlayerTeam(uptr actor)
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			if (!actor || !player) return false;
			if (actor == player || IsFollower(player, actor)) return true;
			const auto id = At<std::uint32_t>(actor, off::kFormId);
			for (const auto& j : g_jobs) if (j.phase != Phase::Idle && j.npc == id && (j.gang || j.order)) return true;
			return false;
		}
		int FactionReaction(uptr actor, uptr target)
		{
			// GetFactionRelation console/condition wrappers 140C439A0 / 140BEE910 call this exact two-actor function.
			// 141928140 folds actor relationships, faction reactions and current overrides; 14191F780 treats 2/3 as ally/friend.
			__try { return EngineFn<int (*)(uptr, uptr)>(0x01927D70)(actor, target); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}
		using CombatRelation = combatrelations::Relation;
		std::atomic<std::uint32_t> g_damageCrew[kJobs]{};
		struct RelationCache { CombatRelation relation = CombatRelation::Neutral; ULONGLONG until = 0; };
		std::unordered_map<std::uint64_t, RelationCache> g_relations;
		CombatRelation CombatRelationOf(uptr a, uptr b)
		{
			if (!a || !b) return CombatRelation::Neutral;
			if (a == b) return CombatRelation::Ally;
			const auto aid = At<std::uint32_t>(a, off::kFormId), bid = At<std::uint32_t>(b, off::kFormId);
			const std::uint64_t key = (std::uint64_t(std::min(aid, bid)) << 32) | std::max(aid, bid);
			const auto now = GetTickCount64();
			if (auto it = g_relations.find(key); it != g_relations.end() && now < it->second.until) return it->second.relation;
			const bool protectedPair = PlayerTeam(a) && PlayerTeam(b);
			const bool hostile = !protectedPair && (HostileTo(a, b) || HostileTo(b, a));
			const auto relation = protectedPair ? CombatRelation::Ally : hostile ? CombatRelation::Enemy : combatrelations::Classify(false, false, FactionReaction(a, b), FactionReaction(b, a));
			if (g_relations.size() > 1024) g_relations.clear();
			g_relations[key] = {relation, now + 500};
			return relation;
		}
		bool FriendlyJobs(const Job& a, const Job& b) { return CombatRelationOf(RefById(a.npc), RefById(b.npc)) == CombatRelation::Ally; }
		std::uintptr_t SeatKeyword(uptr actor);
		// Seat proof: the interaction's seat keyword (+0x20) is null or the vehicle's driver-seat keyword. The 2026-10-21 05:20 run showed
		// the player's driver seat carries a keyword, so "+0x20 == null" alone wrongly called the driver a passenger.
		bool InDriverSeat(uptr actor)
		{
			__try {
				const uptr extra = At<uptr>(actor, 0xC8);
				if (!extra || !EngineFn<char (*)(uptr, int)>(0x002C5C90)(extra, 0xA5)) return false;
				uptr inter = 0;
				EngineFn<void (*)(uptr, uptr*)>(0x00531E20)(extra, &inter);
				if (!inter) return false;
				const uptr seat = At<uptr>(inter, 0x20);
				if (_InterlockedDecrement(reinterpret_cast<volatile long*>(inter + 8)) == 0)
					reinterpret_cast<void (*)(uptr, int)>((*reinterpret_cast<void***>(inter))[0])(inter, 1);
				return !seat || seat == DriverSeatKeyword(InVehicle(actor));
			} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		std::uintptr_t SeatKeyword(uptr actor)
		{
			__try {
				const uptr extra = At<uptr>(actor, 0xC8);
				if (!extra || !EngineFn<char (*)(uptr, int)>(0x002C5C90)(extra, 0xA5)) return 0;
				uptr inter = 0;
				EngineFn<void (*)(uptr, uptr*)>(0x00531E20)(extra, &inter);
				if (!inter) return 0;
				const uptr seat = At<uptr>(inter, 0x20);
				if (_InterlockedDecrement(reinterpret_cast<volatile long*>(inter + 8)) == 0)
					reinterpret_cast<void (*)(uptr, int)>((*reinterpret_cast<void***>(inter))[0])(inter, 1);
				return seat;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		// The driver test. The player keeps the engine's own rule (the player's vehicle, as every player path reads it), so a seat read
		// can never take the player's own vehicle away; NPCs need the driver-seat proof.
		bool Drives(uptr actor, uptr vehicle)
		{
			if (!actor || !vehicle || InVehicle(actor) != vehicle) return false;
			return actor == Ptr(addr::kPlayerCharacterPtr) || InDriverSeat(actor);
		}
		std::unordered_map<std::uint32_t,std::uint32_t> g_driverIndex;
        driverdiscovery::Cadence g_driverDiscovery;
        uptr CombatDriver(uptr vehicle)
        {
            if(!vehicle||!IsVehicle(vehicle))return 0;
            const auto id=At<std::uint32_t>(vehicle,off::kFormId);
            if(auto it=g_driverIndex.find(id);it!=g_driverIndex.end()) {
                const auto actor=RefById(it->second);
                if(actor&&!Dead(actor)&&Drives(actor,vehicle))return actor;
                g_driverIndex.erase(it);
            }
            // A seat may have changed after the frame's index was built.
            for(const auto& j:g_jobs)if(j.phase!=Phase::Idle&&!j.passenger&&j.vehicle==id) {
                const auto actor=RefById(j.npc);
                if(actor&&!Dead(actor)&&Drives(actor,vehicle)) {g_driverIndex[id]=j.npc;return actor;}
            }
            const auto player=Ptr(addr::kPlayerCharacterPtr);
            if(player&&!Dead(player)&&Drives(player,vehicle)) {g_driverIndex[id]=At<std::uint32_t>(player,off::kFormId);return player;}
            return 0;
        }

		void CrimeAlarms(uptr victim, uptr vehicle, uptr player, bool assault, bool theft = true);
		template<class... Args>void FollowerNotice(std::uint32_t id,const char*format,Args...args){
			const auto player=Ptr(addr::kPlayerCharacterPtr),actor=RefById(id);
			if(player&&actor&&IsFollower(player,actor))notify::Show(format,args...);
		}

		struct Aggression {std::uint32_t attacker=0;ULONGLONG until=0,nativeAt=0;bool crimeRaised=false;};
		std::unordered_map<std::uint32_t,Aggression> g_aggression;
		uptr Aggressor(std::uint32_t vehicle,ULONGLONG now) {
			const auto it=g_aggression.find(vehicle);if(it==g_aggression.end()||now>=it->second.until)return 0;
			const auto actor=RefById(it->second.attacker);return actor&&!Dead(actor)?actor:0;
		}
		ULONGLONG g_recallUntil = 0;             // escorts recalled into formation until then
		Stance g_stance = Stance::Defend;
		// Keeps a player-side vehicle's Combat / Weapons classes on the stance; assigns only on a change (no log spam).
		void ApplyStance(std::uint32_t vehicleId, double x, double y)
		{
			if (!vehicleId) return;
			const auto* want = vcontract::Find(g_stance == Stance::Attack ? "attack" : "defend");
			if (want && vcontract::AssignedTo(vehicleId, vcontract::Class::Combat) != want) vcontract::Assign(vehicleId, want, x, y);
			const auto* hold = vcontract::Find("hold fire");
			const auto* wp = vcontract::AssignedTo(vehicleId, vcontract::Class::Weapons);
			if (g_stance == Stance::HoldFire) { if (hold && wp != hold && !profileclass::Is(wp ? wp->name : nullptr, "never")) vcontract::Assign(vehicleId, hold, x, y); }
			else if (wp == hold) vcontract::Unassign(vehicleId, vcontract::Class::Weapons);
		}
		constexpr ULONGLONG kFightOverMs = 20000;  // an automatically chosen combat profile ends this long after the last enemy
		constexpr double kRecallBoard = 10.0;   // m: a recalled vehicle this close to the player parks and its crew driver gets out
		overrides::Mode ModeOf(int order)
		{
			return order == int(CrewOrder::Recall) ? overrides::Mode::Recall : order == int(CrewOrder::Cover) ? overrides::Mode::Cover :
				order == int(CrewOrder::Guard) ? overrides::Mode::Guard : overrides::Mode::None;
		}
		const char* OrderText(CrewOrder order)
		{
			switch (order) {
			case CrewOrder::Marker: return "to your marker";
			case CrewOrder::Hold: return "hold position";
			case CrewOrder::Recall: return "summon to you";
			case CrewOrder::Cover: return "cover you";
			case CrewOrder::Guard: return "guard the point";
			default: return "follow you";
			}
		}
		constexpr double kDefendRange = 150.0;
		std::uint32_t g_combatFocus = 0;
		std::uint32_t g_focusVehicle = 0;  // focus on a vehicle (2026-10-25): whoever drives it is the target, a new driver too
		constexpr double kDefendLeash = 250.0;   // m: a defender's target stays this close to the player
		constexpr ULONGLONG kFoePickMs = 500;

		bool IsFoe(const Job& j, uptr npc, uptr player, uptr cand)
		{
			if (cand == npc || !IsActorRef(cand) || Dead(cand)) return false;
			if (PlayerTeam(npc) && PlayerTeam(cand)) return false;
			if (CombatRelationOf(npc, cand) == CombatRelation::Ally) return false;
			const bool hostile = HostileTo(npc, cand) || HostileTo(cand, npc) || (PlayerTeam(npc) && HostileTo(cand, player));
			return hostile && (InCombat(npc) || InCombat(cand) || (j.profile == Profile::Pursuit && cand == player));
		}

		// (no __try here: the scan holds a vector)
		uptr PickFoe(Job& j, uptr npc, uptr vehicle, uptr player, ULONGLONG now)
		{
			const auto combat=vcontract::AssignedTo(j.vehicle,vcontract::Class::Combat);
			const bool defend = j.profile == Profile::Escort && !(combat&&profileclass::Is(combat->name,"attack"));
			const double kFoeRange = defend ? kDefendRange : coverfight::kChaseRange;
			if (const auto focus=RefById(CombatFocus()); focus && PlayerTeam(npc) && IsFoe(j,npc,player,focus) &&
				InVehicle(focus)!=vehicle && Dist3(vehicle,focus)<coverfight::kChaseKeep) {
				j.foe=At<std::uint32_t>(focus,off::kFormId);j.foeAt=now;return focus;
			}
			if(const auto aggressor=Aggressor(j.vehicle,now);aggressor && aggressor!=npc && !(PlayerTeam(npc) && PlayerTeam(aggressor)) && InVehicle(aggressor)!=vehicle && Dist3(vehicle,aggressor)<kFoeRange*1.2) {
				j.foe=At<std::uint32_t>(aggressor,off::kFormId);j.foeAt=now;return aggressor;
			}
			if (defend && now < g_recallUntil) {
				if (j.foe) { logger::Write("NPC vehicles: [%d] recalled: target %08X dropped", j.pilot, j.foe); j.foe = 0; }
				return 0;
			}
			const auto boardingEnemy=RefById(j.boardingFight.target);
			if(j.boardingFight.Active(now,boardingEnemy&&!Dead(boardingEnemy),boardingEnemy&&PlayerTeam(npc)&&PlayerTeam(boardingEnemy)) &&
				InVehicle(boardingEnemy)!=vehicle && Dist3(vehicle,boardingEnemy)<kFoeRange*1.2) {
				if(!InCombat(npc)&&now-j.aggroAt>=2000){j.aggroAt=now;Aggro(npc,boardingEnemy);g_relations.clear();}
				j.foe=j.boardingFight.target;j.foeAt=now;return boardingEnemy;
			}
			if(const auto focus=RefById(j.squadFocus);focus&&IsFoe(j,npc,player,focus)&&InVehicle(focus)!=vehicle&&
                Dist3(vehicle,focus)<kFoeRange*1.2&&(!defend||Dist(focus,player)<kDefendLeash)) {
                j.foe=At<std::uint32_t>(focus,off::kFormId);j.foeAt=now;return focus;
            }
            const uptr cur = RefById(j.foe);
			const bool curOk = cur && IsFoe(j, npc, player, cur) && Dist3(vehicle, cur) < kFoeRange * 1.2 && (!defend || Dist(cur, player) < kDefendLeash);
			if (now - j.foeAt < kFoePickMs) return curOk ? cur : 0;
			j.foeAt = now;
			uptr best=RefById(NativeEnemyId(npc));
            if(best&&(!IsFoe(j,npc,player,best)||Dist3(vehicle,best)>kFoeRange||InVehicle(best)==vehicle))best=0;
            int bestTier=best?(InVehicle(best)?0:1):9;
			double bestD = best?Dist3(vehicle,best):1e9;
			if (!defend || InCombat(player) || InCombat(npc)) {
				for (const uptr r : modscene::LoadedRefs(8000)) {
					if (!r || r == vehicle || !IsActorRef(r)) continue;
					const double d = Dist3(vehicle, r);
					if (d > kFoeRange) continue;
					if (defend && Dist(r, player) > kDefendLeash) continue;
					const uptr in = InVehicle(r);
					if (in == vehicle) continue;  // riding with us
					if (!IsFoe(j, npc, player, r)) continue;
					const int tier = in ? 0 : 1;
					// Spread (2026-10-20): every other vehicle of this side on the same target adds 25 m, so two do not pile on one while another is free.
					const std::uint32_t rid = At<std::uint32_t>(r, off::kFormId);
					double score = d;
					for (const auto& o : g_jobs) {
						if (&o != &j && o.phase == Phase::Following && FriendlyJobs(j, o) && o.foe == rid) score += 25.0;
					}
					if (tier < bestTier || (tier == bestTier && score < bestD)) { best = r; bestTier = tier; bestD = score; }
				}
			}
			// Sticky: the current target stays unless a higher tier appears (a closer one of the same tier is not worth the turn).
			if (curOk && best && best != cur) {
				const int curTier = InVehicle(cur) ? 0 : 1;
				if (curTier <= bestTier) { best = cur; bestTier = curTier; bestD = Dist3(vehicle, cur); }
			}
			const std::uint32_t id = best ? At<std::uint32_t>(best, off::kFormId) : 0;
			if (id != j.foe) {
				if (id) logger::Write("NPC vehicles: [%d] %s target -> %08X (%s, %.0f m)", j.pilot, kProfiles[int(j.profile)].name, id, bestTier == 0 ? "in a vehicle" : "on foot", bestD);
				else logger::Write("NPC vehicles: [%d] %s target %08X dropped (no enemy within %.0f m)", j.pilot, kProfiles[int(j.profile)].name, j.foe, kFoeRange);
				j.foe = id;
				j.aimAt = 0;  // the aim lead starts again on the new target
				j.aimVx = j.aimVy = j.aimVz = 0;
			}
			j.foeTier = best ? bestTier : -1;
			return best;
		}

		// The orbit around a target that is not the player (the pursuit goal circles the player only).
		void OrbitGoal(const Job& j, uptr target, uptr vehicle, vcontract::Goal& g)
		{
			const auto tp = At<game::Vec3>(target, off::kRefPosition);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double dir = (j.pilot % 2) ? 1.0 : -1.0;
			const double ang = std::atan2(double(vp.x) - tp.x, double(vp.y) - tp.y) + dir * 0.6;
			g.x = tp.x + std::sin(ang) * kAttackRadius;
			g.y = tp.y + std::cos(ang) * kAttackRadius;
			g.z = tp.z + kAttackAbove;
			g.stop = 3.0;
			g.hold = false;
		}

		// The escort's place in a fight (2026-10-20, the user picked shield and intercept): an enemy vehicle closing on the player from farther than
		// kInterceptMin -> intercept (a point between the player and where it will be in kInterceptLead); the target out of weapon range -> shield
		// (kShieldDist from the player toward it, a little above); in range -> attack (orbit it). Returns the mode.
		constexpr double kInterceptMin = 45.0, kInterceptLead = 2.0, kInterceptShare = 0.4, kShieldDist = 14.0;
		constexpr double kOverwatchUp = 18.0, kOverwatchBack = 10.0;

		// ---- Escort context (2026-10-20, the user: these are for NPCs in vehicles, not for the empty vehicle). Once a frame for all escorts:
		//  - Rescue: the player's health under 30% of the highest seen -> the escort nearest the player flies to them and lands beside them (off over 55%).
		//  - Scout: the player walks toward their map marker (over kScoutMin away, within kScoutCone for kScoutMs) -> one escort flies ahead, holds over
		//    the marker and says how many enemies are there; ends within kScoutDone of it, on turning away, or in a fight.
		//  - Wait at the door (the Following case): the player indoors -> parked, waiting aboard; following again when they come out.
		//  - Overwatch is the fight mode against an enemy on foot (DefendGoal mode 3).
		enum class EscMode { None, Rescue, Scout };
		constexpr int kThreatHigh = 4;
		constexpr double kScoutMin = 120.0, kScoutCone = 0.6, kScoutDone = 40.0, kScoutHold = 8.0;
		constexpr ULONGLONG kScoutMs = 3000;
		struct EscCtx
		{
			EscMode mode = EscMode::None;
			int rescuer = 0, scout = 0;         // the pilots doing it
			int threat = 0;                     // enemies of the player within 150 m (counted once a second in a fight)
			ULONGLONG threatAt = 0;
			double sx = 0, sy = 0;              // the marker (scout)
			ULONGLONG towardSince = 0, awaySince = 0;
			double px = 0, py = 0, vx = 0, vy = 0;
			ULONGLONG at = 0;
		} g_ec;

		void SetEscMode(EscMode m, const char* why)
		{
			if (m == g_ec.mode) return;
			logger::Write("NPC vehicles: escorts: %s -> %s (%s)", g_ec.mode == EscMode::Rescue ? "rescue" : g_ec.mode == EscMode::Scout ? "scout" : "follow",
				m == EscMode::Rescue ? "rescue" : m == EscMode::Scout ? "scout" : "follow", why);
			if (m == EscMode::Rescue) FollowerNotice(g_ec.rescuer>0?g_jobs[g_ec.rescuer-1].npc:0,"A follower is coming to pick you up");
			else if (m == EscMode::Scout) FollowerNotice(g_ec.scout>0?g_jobs[g_ec.scout-1].npc:0,"A follower scouts ahead to your marker");
			g_ec.mode = m;
			for (auto& j : g_jobs) j.rescueParked = j.scoutReported = false;
		}

		void EscortContextTick(uptr player, ULONGLONG now)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (g_ec.at && now > g_ec.at && now - g_ec.at < 1000) {
				const double dt = (now - g_ec.at) / 1000.0, a = std::min(1.0, dt * 3.0);
				g_ec.vx += ((pp.x - g_ec.px) / dt - g_ec.vx) * a;
				g_ec.vy += ((pp.y - g_ec.py) / dt - g_ec.vy) * a;
			}
			else {
				g_ec.vx = g_ec.vy = 0;
			}
			g_ec.px = pp.x; g_ec.py = pp.y; g_ec.at = now;
			if (now - g_ec.threatAt >= 1000) {
				g_ec.threatAt = now;
				int n = 0;
				bool inVeh = false;
				if (InCombat(player)) EnemyNear(player, pp.x, pp.y, pp.z, 150.0, inVeh, n);
				if ((n >= kThreatHigh) != (g_ec.threat >= kThreatHigh)) logger::Write("NPC vehicles: escorts: %d enemies around -> %s", n, n >= kThreatHigh ? "defensive" : "normal");
				g_ec.threat = n;
			}
			// The escorts following now (the nearest to the player rescues; the first scouts).
			int nearest = 0, first = 0;
			uptr firstVeh = 0;
			double best = 1e9;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Following || j.profile != Profile::Escort || j.order == int(CrewOrder::Marker) || j.order == int(CrewOrder::Recall) ||
					j.order == int(CrewOrder::Guard) || !targetsession::CanFollow(j.vehicle,vehicleflight::OwnRoverId())) continue;
				const uptr v = RefById(j.vehicle);
				if (!v) continue;
				const double d = Dist(v, player);
				if (d < best) { best = d; nearest = j.pilot; }
				if (!first) { first = j.pilot; firstVeh = v; }
			}
			float share = 1.0f;
			const bool haveHealth = vehiclehits::PlayerHealthShare(share);
			if (g_ec.mode == EscMode::Rescue) {
				if (!haveHealth || share > 0.55f) SetEscMode(EscMode::None, "health back");
				else if (!g_ec.rescuer || !nearest) g_ec.rescuer = nearest;
				return;
			}
			if (haveHealth && share < 0.30f && nearest) {
				g_ec.rescuer = nearest;
				SetEscMode(EscMode::Rescue, "your health is low");
				return;
			}
			double mx = 0, my = 0;
			const bool marker = firstVeh && vehicleflight::MarkerPoint(firstVeh, mx, my);
			const double toMarker = marker ? std::hypot(mx - pp.x, my - pp.y) : 0.0;
			const double sp = std::hypot(g_ec.vx, g_ec.vy);
			const double off = marker ? std::fabs(WrapAngle(std::atan2(g_ec.vx, g_ec.vy) - std::atan2(mx - pp.x, my - pp.y))) : 9.0;
			if (g_ec.mode == EscMode::Scout) {
				if (sp > 2.0 && off > 1.6) { if (!g_ec.awaySince) g_ec.awaySince = now; }
				else g_ec.awaySince = 0;
				if (!marker || !first || InCombat(player) || toMarker < kScoutDone || (g_ec.awaySince && now - g_ec.awaySince >= kScoutMs)) {
					SetEscMode(EscMode::None, !marker ? "marker gone" : InCombat(player) ? "a fight" : toMarker < kScoutDone ? "you reached the marker" : "you turned away");
				}
				else {
					g_ec.sx = mx; g_ec.sy = my;
					if (g_ec.scout != first && !RefById(g_ec.scout ? g_jobs[g_ec.scout - 1].vehicle : 0)) g_ec.scout = first;
				}
				return;
			}
			if (marker && first && !InCombat(player) && sp > 2.0 && off < kScoutCone && toMarker > kScoutMin) {
				if (!g_ec.towardSince) g_ec.towardSince = now;
				if (now - g_ec.towardSince >= kScoutMs) {
					g_ec.scout = first;
					g_ec.sx = mx; g_ec.sy = my;
					g_ec.awaySince = 0;
					SetEscMode(EscMode::Scout, "you walk toward your marker");
				}
			}
			else {
				g_ec.towardSince = 0;
			}
		}

		// ---- Enemy context (2026-10-20, the user: "add for them based on contexts"): what a pursuer does, from the fight's state. In order:
		//  - Retreat: its vehicle carries over kRetreatShare of the hit limit -> it pulls kRetreatDist away from the target, lands, and the driver gets
		//    out to fight on foot (that vehicle's autopilot stays off a while, so nobody boards it again at once).
		//  - Block: the target sits in a vehicle that is slow (under kBlockSlow) and this is the first pursuer -> it lands kBlockAhead in front of it.
		//  - Intercept: the target drives away faster than kInterceptFast -> it flies at where the target will be in kInterceptLead.
		//  - Overwatch: the target is on foot and there are two pursuers or more -> the first hovers kOverwatchUp over it and fires down.
		//  - Flank: two pursuers or more -> each takes its own slot round the target (the slots turn slowly), so they come from different sides.
		//  - Attack: one pursuer -> the orbit and attack runs as before.
		constexpr float kRetreatShare = 0.7f;
		constexpr double kRetreatDist = 90.0, kBlockSlow = 4.0, kBlockAhead = 9.0, kInterceptFast = 12.0;
		int PursueGoal(Job& j, uptr target, bool inVehicle, uptr vehicle, ULONGLONG now, vcontract::Goal& g)
		{
			const auto tp = At<game::Vec3>(target, off::kRefPosition);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			if (j.foeSeenAt && now > j.foeSeenAt && now - j.foeSeenAt < 1000) {
				const double dt = (now - j.foeSeenAt) / 1000.0;
				const double ix = (tp.x - j.foeX) / dt, iy = (tp.y - j.foeY) / dt;
				if (std::hypot(ix, iy) < 150.0) {
					const double a = std::min(1.0, dt * 3.0);
					j.foeVx += (ix - j.foeVx) * a;
					j.foeVy += (iy - j.foeVy) * a;
				}
			}
			else {
				j.foeVx = j.foeVy = 0;
			}
			j.foeX = tp.x; j.foeY = tp.y; j.foeSeenAt = now;
			int idx = 0, count = 0;
			for (const auto& o : g_jobs) {
				if (o.phase != Phase::Following || o.profile != Profile::Pursuit) continue;
				++count;
				if (o.pilot < j.pilot) ++idx;
			}
			float dmg = 0, lim = 0;
			if (vehiclehits::DamageOf(j.vehicle, dmg, lim) && lim > 0 && dmg >= kRetreatShare * lim) j.retreating = true;
			const double dx = double(tp.x) - vp.x, dy = double(tp.y) - vp.y, d = std::hypot(dx, dy);
			const double tsp = std::hypot(j.foeVx, j.foeVy);
			const double away = d > 0.1 && tsp > 0.1 ? (j.foeVx * dx + j.foeVy * dy) / (d * tsp) : 0.0;  // 1: the target moves straight away from us
			int mode = 10;
			g.hold = false;
			if (j.retreating) {
				const double ux = d > 0.1 ? -dx / d : 0.0, uy = d > 0.1 ? -dy / d : 1.0;
				g.x = tp.x + ux * kRetreatDist;
				g.y = tp.y + uy * kRetreatDist;
				g.z = std::numeric_limits<double>::quiet_NaN();
				g.stop = 4.0;
				mode = 14;
			}
			else if (inVehicle && tsp < kBlockSlow && idx == 0 && d < 60.0) {
				const double h = At<game::Vec3>(target, off::kRefRotation).z;
				g.x = tp.x + std::sin(h) * kBlockAhead;
				g.y = tp.y + std::cos(h) * kBlockAhead;
				g.z = std::numeric_limits<double>::quiet_NaN();
				g.stop = 1.5;
				g.hold = true;
				mode = 15;
			}
			else if (inVehicle && tsp > kInterceptFast && away > 0.5) {
				g.x = tp.x + j.foeVx * kInterceptLead;
				g.y = tp.y + j.foeVy * kInterceptLead;
				g.z = tp.z + 4.0;
				g.stop = 2.0;
				mode = 12;
			}
			else if (!inVehicle && idx == 0 && count >= 2) {
				g.x = d > 0.1 ? tp.x - dx / d * 12.0 : tp.x;
				g.y = d > 0.1 ? tp.y - dy / d * 12.0 : tp.y;
				g.z = tp.z + kOverwatchUp + 2.0;
				g.stop = 2.0;
				mode = 13;
			}
			else if (count >= 2) {
				const double slot = 6.283185307179586 * double(idx) / double(count) + std::fmod(double(now) * 0.0003, 6.283185307179586);
				g.x = tp.x + std::sin(slot) * kAttackRadius;
				g.y = tp.y + std::cos(slot) * kAttackRadius;
				g.z = tp.z + kAttackAbove;
				g.stop = 3.0;
			}
			else {
				OrbitGoal(j, target, vehicle, g);
			}
			if (mode != j.pMode || (mode == 10 && (count >= 2) != (j.pMode == 10 && count >= 2))) {
				logger::Write("NPC vehicles: [%d] pursuer: %s %08X (%.0f m%s)", j.pilot,
					mode == 14 ? "retreating from" : mode == 15 ? "blocking" : mode == 12 ? "intercepting" : mode == 13 ? "overwatch over" : count >= 2 ? "flanking" : "attacking",
					At<std::uint32_t>(target, off::kFormId), d, mode == 14 ? ", hit limit near" : "");
				j.pMode = mode;
			}
			return mode;
		}

		// ---- Vehicle combat AI (2026-10-21, the approved proposal notes/VEHICLE_COMBAT_AI_PROPOSAL_20261021.md, stages 1-4; manoeuvre.h). Replaces the
		// orbit: steering (carrot, minimum combat speed, separation, zone), manoeuvres (attack run, lag pursuit, break turn, circle, scissors, extend and
		// re-engage), roles (engage, pincer, overwatch), ram / sideswipe and hiding behind terrain on the way out.
		// settings.ini [combat]: stage= 4 (1-4: the states of stages up to it), zoneEscort= 150, zonePursuit= 400 (m around the player), ram= 1,
		// boost= 1, breakOff= 30 (percent health), hold= 1500 (ms).
		// The full AI (notes/VEHICLE_COMBAT_AI_FULL_SPEC_20261021.md, part E): [combat] ai= 1 (0: the staged manoeuvres above), and a switch per part:
		// steering= dogfight= tactics= squads= morale= personality= (1 on, 0 off).
		struct CombatIni { double zoneEscort = 150, zonePursuit = 400, breakOff = 0.30; bool boost = true; unsigned holdMs = 1500; int stage = 4; bool read = false;
			bool ai = true; combatai::Switches sw; } g_combatIni;
		void ReadCombatIni()
		{
			static unsigned gen = 0;
			if (g_combatIni.read && gen == features::Generation()) return;
			g_combatIni.read = true;
			gen = features::Generation();  // the settings menu changed a switch: read again
			PWSTR docs = nullptr;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
				const std::wstring ini = fvopaths::Settings();
				CoTaskMemFree(docs);
				g_combatIni.zoneEscort = GetPrivateProfileIntW(L"combat", L"zoneEscort", 150, ini.c_str());
				g_combatIni.zonePursuit = GetPrivateProfileIntW(L"combat", L"zonePursuit", 400, ini.c_str());
				g_combatIni.stage = GetPrivateProfileIntW(L"combat", L"stage", 4, ini.c_str());
				g_combatIni.ai = features::On(features::Id::CombatAi);  // [combat] ai
				g_combatIni.sw.steering = GetPrivateProfileIntW(L"combat", L"steering", 1, ini.c_str()) != 0;
				g_combatIni.sw.dogfight = GetPrivateProfileIntW(L"combat", L"dogfight", 1, ini.c_str()) != 0;
				g_combatIni.sw.tactics = GetPrivateProfileIntW(L"combat", L"tactics", 1, ini.c_str()) != 0;
				g_combatIni.sw.squads = GetPrivateProfileIntW(L"combat", L"squads", 1, ini.c_str()) != 0;
				g_combatIni.sw.morale = GetPrivateProfileIntW(L"combat", L"morale", 1, ini.c_str()) != 0;
				g_combatIni.sw.personality = GetPrivateProfileIntW(L"combat", L"personality", 1, ini.c_str()) != 0;
				g_combatIni.boost = GetPrivateProfileIntW(L"combat", L"boost", 1, ini.c_str()) != 0;
				g_combatIni.breakOff = GetPrivateProfileIntW(L"combat", L"breakOff", 30, ini.c_str()) / 100.0;
				g_combatIni.holdMs = GetPrivateProfileIntW(L"combat", L"hold", 1500, ini.c_str());
			}
			g_combatIni.zoneEscort = std::clamp(g_combatIni.zoneEscort, 40.0, 1000.0);
			g_combatIni.zonePursuit = std::clamp(g_combatIni.zonePursuit, coverfight::kChaseRange, 1000.0);
			g_combatIni.breakOff = std::clamp(g_combatIni.breakOff, 0.0, 0.9);
			g_combatIni.holdMs = std::clamp(g_combatIni.holdMs, 300u, 10000u);
			g_combatIni.stage = std::clamp(g_combatIni.stage, 1, 4);
			logger::Write("NPC vehicles: combat AI stage %d: fight zone %.0f m (escorts) / %.0f m (pursuers) around you, boost %s, break off under %.0f%%, states held %u ms",
				g_combatIni.stage, g_combatIni.zoneEscort, g_combatIni.zonePursuit, g_combatIni.boost ? "on" : "off", g_combatIni.breakOff * 100.0,
				g_combatIni.holdMs);
			const auto& w = g_combatIni.sw;
			logger::Write("NPC vehicles: full combat AI %s: steering %d, dogfight %d, tactics %d, squads %d, morale %d, personality %d", g_combatIni.ai ? "ON" : "off (staged manoeuvres)",
				w.steering, w.dogfight, w.tactics, w.squads, w.morale, w.personality);
		}

		// One frame of the manoeuvre fight: sets the goal (a cruise carrot) and sends the nose / throttle / strafe commands. pursuer: the 250 m zone;
		// aggressive (the attack profile): may ram. False only while a ram it ordered takes the pilot.
		bool ManoeuvreTick(Job& j, uptr foe, uptr aimRef, bool foeInVeh, uptr vehicle, uptr player, bool pursuer, bool aggressive, bool mayAim,
			ULONGLONG now, vcontract::Goal& g)
		{
			ReadCombatIni();
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const auto vr = At<game::Vec3>(vehicle, off::kRefRotation);
			const auto fp = At<game::Vec3>(aimRef, off::kRefPosition);
			const auto fr = At<game::Vec3>(aimRef, off::kRefRotation);
			const std::uint32_t foeId = At<std::uint32_t>(aimRef, off::kFormId);
			if (j.manFoe != foeId || !j.manAt || now <= j.manAt || now - j.manAt > 1000) {
				j.manMvx = j.manMvy = j.manFvx = j.manFvy = 0;
			}
			else {
				const double dt = (now - j.manAt) / 1000.0, a = std::min(1.0, dt * 4.0);
				const auto smooth = [&](double& v, double cur, double was) { const double inst = (cur - was) / dt; if (std::fabs(inst) < 150.0) v += (inst - v) * a; };
				smooth(j.manMvx, vp.x, j.manMx); smooth(j.manMvy, vp.y, j.manMy);
				smooth(j.manFvx, fp.x, j.manFx); smooth(j.manFvy, fp.y, j.manFy);
			}
			j.manMx = vp.x; j.manMy = vp.y; j.manFx = fp.x; j.manFy = fp.y; j.manAt = now; j.manFoe = foeId;
			manoeuvre::Input in;
			in.me = manoeuvre::Body{ vp.x, vp.y, vp.z, j.manMvx, j.manMvy, vr.z };
			in.foe = manoeuvre::Body{ fp.x, fp.y, fp.z, j.manFvx, j.manFvy, fr.z };
			in.foeInVehicle = foeInVeh;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			in.anchorX = pp.x; in.anchorY = pp.y;
			in.myHealth = vehiclehits::HealthShare(j.vehicle);
			if (foeInVeh) in.foeHealth = vehiclehits::HealthShare(foeId);
			else { float hn = 0, hm = 0; if (vehiclehits::ActorHealth(foe, hn, hm)) in.foeHealth = hn / hm; }
			const auto lastHit = vehiclehits::LastHitMs(j.vehicle);
			in.hitRecently = lastHit && now - lastHit < manoeuvre::kHitMs;
			int same = j.manOn ? 0 : 1, rank = 0;
			for (const auto& o : g_jobs) {
				if (o.phase != Phase::Following || !o.manOn || o.foe != j.foe || !FriendlyJobs(j, o)) continue;
				++same;
				if (o.pilot < j.pilot) ++rank;
			}
			in.sameSide = std::max(1, same);
			in.rank = rank;
			// Enemy vehicles on me: jobs fighting this NPC (pursuers on an escort, escorts on a pursuer).
			int onMe = 0;
			for (const auto& o : g_jobs) if (o.phase == Phase::Following && o.manOn && o.foe == j.npc) ++onMe;
			in.enemiesOnMe = std::max(1, onMe);
			// Separation: the other vehicles within 30 m (the enemy's own vehicle too, unless ramming or sideswiping it).
			static world::Snapshot snap;
			if (world::Vehicles(snap)) {
				for (std::uint32_t k = 0; k < snap.count && k < world::kMaxVehicles && in.others < manoeuvre::kMaxOthers; ++k) {
					const auto& v = snap.v[k];
					if (v.formId == j.vehicle) continue;
					if (v.formId == foeId && (j.man.state == manoeuvre::State::Ram || j.man.state == manoeuvre::State::Sideswipe)) continue;
					if (std::hypot(double(v.pos[0]) - vp.x, double(v.pos[1]) - vp.y) > 30.0) continue;
					in.otherX[in.others] = v.pos[0]; in.otherY[in.others] = v.pos[1]; ++in.others;
				}
			}
			in.hover = true;  // every job vehicle flies (NPC rover hover)
			in.now = now;
			manoeuvre::Settings set;
			set.zone = pursuer || aggressive ? g_combatIni.zonePursuit : g_combatIni.zoneEscort;
			set.breakOff = g_combatIni.breakOff;
			set.holdMs = g_combatIni.holdMs;
			set.speedScale = g_combatIni.boost ? 1.0 : 0.7;
			set.stage = g_combatIni.stage;
			const char* why = "";
			const auto want = manoeuvre::Choose(in, j.man, set, &why);
			if (manoeuvre::Enter(j.man, want, in)) {
				// Stage 4: the way out (extend, break off) goes behind terrain when a direction hides it from the enemy (nav map line of sight).
				bool hidden = false;
				if (set.stage >= 4 && (want == manoeuvre::State::Extend || want == manoeuvre::State::BreakOff)) {
					double dirs[5][2];
					const int n = manoeuvre::HideCandidates(j.man, dirs);
					const double out = std::min(j.man.extendOut, 120.0);
					const float eye[3] = { fp.x, fp.y, fp.z + 1.5f };
					for (int i = 0; i < n && !hidden; ++i) {
						const float to[3] = { float(vp.x + dirs[i][0] * out), float(vp.y + dirs[i][1] * out), vp.z + 1.0f };
						if (environment::LineBlocked(eye, to)) { manoeuvre::Hide(j.man, dirs[i][0], dirs[i][1]); hidden = true; }
					}
				}
				const double behind = std::fabs(manoeuvre::Wrap(std::atan2(double(fp.x) - vp.x, double(fp.y) - vp.y) - vr.z)) > 1.57 ? 1.0 : 0.0;
				logger::Write("NPC vehicles: [%d] %s: %s; enemy %08X %.0f m %s (health %.0f%%, its %.0f%%%s)", j.pilot, manoeuvre::Name(want), why, foeId,
					std::hypot(double(fp.x) - vp.x, double(fp.y) - vp.y), behind > 0 ? "behind" : "ahead", in.myHealth * 100.0,
					std::isnan(in.foeHealth) ? -1.0 : in.foeHealth * 100.0, hidden ? "; out behind terrain" : "");
			}
			j.manOn = true;
			auto plan = manoeuvre::Steer(in, j.man, set);
			g.x = plan.x; g.y = plan.y; g.z = plan.z;
			g.stop = plan.hold ? 3.0 : 0.0; g.hold = plan.hold; g.cruise = !plan.hold; g.cruiseSpeed = plan.speed;
			vehicleflight::Command c;
			c.set = 0;
			if (plan.aim && mayAim) {
				const double t = vehiclehits::ShotTime(j.vehicle,Dist3(vehicle,aimRef));
				const double ax = fp.x + j.manFvx * t, ay = fp.y + j.manFvy * t, az = fp.z + (foeInVeh ? 1.0 : kAimHeight);
				c.set |= vehicleflight::Command::kYaw | vehicleflight::Command::kPitch;
				c.yawRate = std::clamp(2.5 * WrapAngle(std::atan2(ax - vp.x, ay - vp.y) - vr.z), -1.6, 1.6);
				c.pitch = std::clamp(std::atan2(az - vp.z, std::max(5.0, std::hypot(ax - vp.x, ay - vp.y))), -0.6, 0.6);
				if (!std::isnan(plan.throttle)) { c.set |= vehicleflight::Command::kThrottle | vehicleflight::Command::kGear; c.throttle = plan.throttle; c.gear = 2; }
				if (!std::isnan(plan.strafe)) { c.set |= vehicleflight::Command::kStrafe; c.strafe = plan.strafe; }
			}
			if (!std::isnan(plan.vertical)) { c.set |= vehicleflight::Command::kVertical; c.vertical = plan.vertical; }
			c.holdMs=100; vehicleflight::PilotCommand(j.pilot,c);
			return true;
		}

		// ---- The full combat AI: engine glue (combat_ai.h does the thinking) ----
		// The world as the steering sees it: the nav map (rays, terrain, object boxes) and its line of sight.
		struct NavWorld : steer::World
		{
			bool Blocked(const steer::V3& a, const steer::V3& b) const override
			{
				const float f[3] = { float(a.x), float(a.y), float(a.z) }, t[3] = { float(b.x), float(b.y), float(b.z) };
				return environment::LineBlocked(f, t);
			}
			bool Solid(const steer::V3& p) const override
			{
				const auto m = navmap::At(p.x, p.y, p.z);
				return m.known && (m.blocked || (m.objBlocked && !m.objOverruled));
			}
			double Ground(double x, double y) const override
			{
				const auto m = navmap::At(x, y, 0.0);
				return m.haveGround ? double(m.ground) : std::numeric_limits<double>::quiet_NaN();
			}
		};
		bool InFaction(uptr actor, uptr faction);
		// Personality from the driver's factions: their editor ids, found once in the data handler's faction list (type 0x0D at +0x78 + type * 0x18,
		// the layout realvehicles found for vehicle forms). Crimson Fleet / pirates / spacers / Ecliptic: reckless; UC security / SysDef / Vanguard,
		// Freestar / Rangers: disciplined; settlers / farmers / civilians: civilian. Editor ids may be stripped in the release build: then everyone is
		// the default temper (logged once).
		struct FactionSet { std::vector<uptr> reckless, disciplined, civilian; bool read = false; } g_factions;
		const char* FormEditorId(uptr form)
		{
			__try {
				auto** vt = *reinterpret_cast<void***>(form);
				return reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(form);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}
		bool FactionList(uptr*& data, std::uint32_t& size)
		{
			__try {
				const uptr dh = Ptr(addr::kDataHandlerPtr);
				if (!dh) return false;
				const uptr fa = dh + 0x78 + 0x0D * 0x18;
				size = At<std::uint32_t>(fa, 8);
				data = At<uptr*>(fa, 16);
				return data && size && size < 20000;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		void ReadFactions()
		{
			if (g_factions.read) return;
			g_factions.read = true;
			uptr* data = nullptr;
			std::uint32_t size = 0;
			unsigned named = 0;
            unsigned builtins=0;
            auto add=[](std::vector<uptr>&list,uptr f){if(std::find(list.begin(),list.end(),f)==list.end())list.push_back(f);};
            for(const auto&known:nativepersonality::factions) {
                const auto f=RefById(known.id);if(!f||At<std::uint8_t>(f,0x2E)!=0x0D)continue;
                auto&list=known.temper==brain::Temper::Reckless?g_factions.reckless:known.temper==brain::Temper::Disciplined?g_factions.disciplined:g_factions.civilian;
                add(list,f);++builtins;
            }
			if (FactionList(data, size)) {
				for (std::uint32_t i = 0; i < size; ++i) {
					const uptr f = data[i];
					if (!f || At<std::uint8_t>(f, 0x2E) != 0x0D) continue;
					const char* id = FormEditorId(f);
					if (!id || !id[0]) continue;
					++named;
					std::string low(id);
					for (auto& ch : low) ch = char(std::tolower(static_cast<unsigned char>(ch)));
					auto has = [&](const char* k) { return low.find(k) != std::string::npos; };
					if (has("crimson") || has("pirate") || has("spacer") || has("ecliptic") || has("scavenger")) add(g_factions.reckless,f);
					else if (has("ucsecurity") || has("ucsysdef") || has("sysdef") || has("vanguard") || has("freestar") || has("ranger") || has("ucmarine")) add(g_factions.disciplined,f);
					else if (has("civilian") || has("settler") || has("farmer") || has("colonist")) add(g_factions.civilian,f);
				}
			}
			logger::Write("NPC vehicles: personality factions: %u loaded, %u named, %u native master identities; reckless %zu, disciplined %zu, civilian %zu%s", size, named,builtins,
				g_factions.reckless.size(), g_factions.disciplined.size(), g_factions.civilian.size(), builtins||named ? "" : " (native faction data unavailable; default temper)");
		}
		std::unordered_map<std::uint32_t,brain::Temper> g_temperCache;
        brain::Temper TemperOf(uptr npc)
		{
			auto& cache=g_temperCache;
			if (!npc) return brain::Temper::Default;
			const std::uint32_t id = At<std::uint32_t>(npc, off::kFormId);
			const auto it = cache.find(id);
			if (it != cache.end()) return it->second;
			ReadFactions();
			brain::Temper t = brain::Temper::Default;
			for (const uptr f : g_factions.reckless) if (t == brain::Temper::Default && InFaction(npc, f)) t = brain::Temper::Reckless;
			for (const uptr f : g_factions.disciplined) if (t == brain::Temper::Default && InFaction(npc, f)) t = brain::Temper::Disciplined;
			for (const uptr f : g_factions.civilian) if (t == brain::Temper::Default && InFaction(npc, f)) t = brain::Temper::Civilian;
			if (cache.size() > 512) cache.clear();
			cache[id] = t;
			return t;
		}
		// The game's difficulty: the setting uDifficulty:GamePlay (initialiser FUN_1401C5F20, its value at DAT_145F468B0), native byte index 0..6 (AI tuning still caps at 4).
		int Difficulty()
		{
			static int logged = -2;
			int d = 2;
			__try {
				d = int(*reinterpret_cast<std::uint8_t*>(game::Base() + 0x05F468B0));
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				d = 2;
			}
			if (d < 0 || d > 6) d = 2;
			if (d != logged) { logged = d; logger::Write("NPC vehicles: game difficulty native index %d (0..6; AI tuning caps at 4)", d); }
			return std::clamp(d, 0, 4);
		}
		// Native-relation squads, one slot per possible job. A complete-link partition cannot merge two enemies through a shared ally.
		struct SideState { brain::Squad squad; std::uint32_t members[kJobs] = {}; int count = 0; std::uint32_t leader = 0; bool wrecked[kJobs] = {}; brain::SquadTactic tactic = brain::SquadTactic::Solo; } g_sides[kJobs];
		int SideOf(const Job& j) { return j.squad; }
		struct CombatVehicle { world::VehicleInfo vehicle; std::uint32_t driver = 0; };
		CombatVehicle g_combatVehicles[world::kMaxVehicles];
		int g_combatVehicleCount = 0;
		ULONGLONG g_squadRefreshAt = 0;
		double g_aiUs = 0, g_aiMaxUs = 0;
		unsigned g_aiFrames = 0;
		ULONGLONG g_aiLogAt = 0;
		void RefreshSquads(ULONGLONG now)
		{
			if (g_squadRefreshAt && now - g_squadRefreshAt < 500) return;
			g_squadRefreshAt = now;
			int active[kJobs] = {}, n = 0;
			for (int i = 0; i < kJobs; ++i) { g_jobs[i].squad = -1; if (g_jobs[i].phase == Phase::Following && g_jobs[i].aiOn) active[n++] = i; }
			std::sort(active, active + n, [](int a, int b) { return g_jobs[a].npc < g_jobs[b].npc; });
			static CombatRelation relations[kJobs][kJobs];  // (kJobs x kJobs: off the stack)
			std::fill(&relations[0][0], &relations[0][0] + kJobs * kJobs, CombatRelation::Neutral);
			int groups[kJobs] = {};
			for (int i = 0; i < n; ++i) for (int j = 0; j < n; ++j) relations[i][j] = i == j ? CombatRelation::Ally : CombatRelationOf(RefById(g_jobs[active[i]].npc), RefById(g_jobs[active[j]].npc));
			const int count = combatrelations::Partition(n, relations, groups);
			static SideState previous[kJobs];  // (64 squad states: too big for the stack)
			for (int i = 0; i < kJobs; ++i) previous[i] = g_sides[i];
			bool used[kJobs] = {};
			for (int g = 0; g < count; ++g) {
				int old = -1, overlap = 0;
				for (int k = 0; k < kJobs; ++k) if (!used[k]) {
					int matches = 0;
					for (int i = 0; i < n; ++i) if (groups[i] == g) for (int m = 0; m < previous[k].count; ++m) if (previous[k].members[m] == g_jobs[active[i]].vehicle) ++matches;
					if (matches > overlap) { old = k; overlap = matches; }
				}
				g_sides[g] = old >= 0 ? previous[old] : SideState{};
				if (old >= 0) used[old] = true;
				// Do not carry another faction's shared target memory when a relationship has changed.
				int members = 0; for (int i = 0; i < n; ++i) if (groups[i] == g) ++members;
				bool changed = members != g_sides[g].count;
				for (int i = 0; i < n; ++i) if (groups[i] == g) {
					bool was = false; for (int m = 0; m < g_sides[g].count; ++m) was = was || g_sides[g].members[m] == g_jobs[active[i]].vehicle;
					changed = changed || !was; g_jobs[active[i]].squad = g;
				}
				if (changed) {
					g_sides[g].squad.memory = {}; g_sides[g].squad.assignedAt = 0;
					for (int i = 0; i < n; ++i) if (groups[i] == g) { auto& job = g_jobs[active[i]]; job.squadFocus=0; job.ai.mem = {}; job.role = brain::Role::Engager; job.roleSlot = 0; job.coverFor = 0; }
				}
				// A truce may change an enemy without changing squad membership. Remove those loaded tracks before memory is merged again.
				for (int i = 0; i < n; ++i) if (groups[i] == g) {
					const auto npc = RefById(g_jobs[active[i]].npc);
					auto prune = [&](brain::Memory& memory) {
						for (auto& track : memory.t) if (track.id) {
							const auto target = RefById(track.id); if (!target) continue;
							const auto actor = IsActorRef(target) ? target : IsVehicle(target) ? CombatDriver(target) : 0;
							if (actor && CombatRelationOf(npc, actor) != CombatRelation::Enemy) track = {};
						}
					};
					prune(g_jobs[active[i]].ai.mem); prune(g_sides[g].squad.memory);
				}
			}
			for (int g = count; g < kJobs; ++g) g_sides[g] = {};
			for (auto& job : g_jobs) if (job.squad < 0) { job.squadFocus=0; job.role = brain::Role::Engager; job.coverFor = 0; job.roleSlot = 0; }
			// Include every occupied hostile vehicle, even before its first shot and even when it has no mod job. Treat unknown weapon capability conservatively.
			static world::Snapshot snap;
			g_combatVehicleCount = 0;
			if (!world::Vehicles(snap)) return;
			for (std::uint32_t i = 0; i < snap.count && i < world::kMaxVehicles; ++i) g_combatVehicles[g_combatVehicleCount++] = {snap.v[i], 0};
            for(int i=0;i<g_combatVehicleCount;++i)
                g_combatVehicles[i].driver=VehicleDriverId(g_combatVehicles[i].vehicle.formId);
		}
		void SquadTick(ULONGLONG now)
		{
			if (!g_combatIni.read) return;
			RefreshSquads(now);
			for (auto& j : g_jobs) {
				for (bool& e : j.sideEvents) e = false;
				j.reinforced = false;
			}
			for (int side = 0; side < kJobs; ++side) {
				SideState& ss = g_sides[side];
				brain::Squad& sq = ss.squad;
				// This frame's members: the jobs of the side flown by the AI last frame.
				std::uint32_t ids[kJobs] = {};
				int n = 0;
				for (const auto& j : g_jobs) if (j.phase == Phase::Following && j.aiOn && SideOf(j) == side && n < kJobs) ids[n++] = j.vehicle;
				// Events: a member gone wrecked, a member's job gone (its driver dead or out), the leader among them; more members = reinforcements.
				bool allyWrecked = false, driverLost = false, leaderLost = false;
				for (int i = 0; i < ss.count; ++i) {
					bool still = false;
					for (int k = 0; k < n; ++k) still = still || ids[k] == ss.members[i];
					const bool wreck = vehiclehits::Wrecked(ss.members[i]);
					if (wreck && !ss.wrecked[i]) { allyWrecked = true; if (ss.members[i] == ss.leader) leaderLost = true; }
					if (!still && !wreck) { driverLost = true; if (ss.members[i] == ss.leader) leaderLost = true; }
				}
				const bool reinforced = ss.count && n > ss.count;
				if (allyWrecked || driverLost || leaderLost || reinforced) {
					for (auto& j : g_jobs) {
						if (j.phase != Phase::Following || SideOf(j) != side) continue;
						j.sideEvents[0] = allyWrecked; j.sideEvents[1] = driverLost; j.sideEvents[2] = leaderLost; j.reinforced = reinforced;
					}
					logger::Write("NPC vehicles: squad %d: %s%s%s%s", side + 1, allyWrecked ? "an ally wrecked; " : "", driverLost ? "an ally's driver lost; " : "",
						leaderLost ? "the leader lost; " : "", reinforced ? "reinforcements" : "");
				}
				ss.count = n;
				for (int i = 0; i < n; ++i) { ss.members[i] = ids[i]; ss.wrecked[i] = vehiclehits::Wrecked(ids[i]); }
				if (!n) { ss.leader = 0; sq.count = 0; continue; }
				bool leaderIn = false;
				for (int i = 0; i < n; ++i) leaderIn = leaderIn || ids[i] == ss.leader;
				if (!leaderIn) ss.leader = ids[0];
				// The squad brain's input: members and the enemies they fight.
				sq.count = 0;
				brain::Enemy enemies[kJobs];
				int en = 0;
				sq.convoy = false;
				for (auto& j : g_jobs) {
					if (j.phase != Phase::Following || !j.aiOn || SideOf(j) != side || sq.count >= brain::Squad::kMax) continue;
					const uptr veh = RefById(j.vehicle);
					if (!veh) continue;
					brain::Member& m = sq.m[sq.count++];
					const auto p = At<game::Vec3>(veh, off::kRefPosition);
					m.id = j.vehicle; m.pos = { p.x, p.y, p.z }; m.vel = { j.manMvx, j.manMvy, 0 }; m.health = vehiclehits::HealthShare(j.vehicle);
					m.morale = j.ai.moraleInit ? j.ai.morale.value : 0.6; m.role = j.role; m.slot=j.roleSlot; m.coverFor=j.coverFor;
					sq.convoy = sq.convoy || j.lead != 0;
					sq.memory.Merge(j.ai.mem);
					const uptr foe = RefById(j.foe);
					if (!foe || en >= 64) continue;
					const uptr fv = InVehicle(foe);
					const uptr aim = fv ? fv : foe;
					const std::uint32_t aid = At<std::uint32_t>(aim, off::kFormId);
					bool dup = false;
					for (int e = 0; e < en; ++e) dup = dup || enemies[e].id == aid;
					if (dup) continue;
					const auto fp = At<game::Vec3>(aim, off::kRefPosition);
					enemies[en].id = aid; enemies[en].pos = { fp.x, fp.y, fp.z }; enemies[en].vel = { j.manFvx, j.manFvy, 0 }; enemies[en].inVehicle = fv != 0;
					float hn = 0, hm = 0;
					enemies[en].health = fv ? vehiclehits::HealthShare(aid) : (vehiclehits::ActorHealth(foe, hn, hm) ? hn / hm : 1.0);
					++en;
				}
				// Native hostile drivers in the world snapshot supplement the selected targets (not only g_jobs).
				for (int k = 0; k < g_combatVehicleCount && en < kJobs; ++k) {
					const auto& cv = g_combatVehicles[k]; const auto& v = cv.vehicle;
					if (!cv.driver || vehiclehits::Wrecked(v.formId)) continue;
					bool hostile = false, friendly = false, nearby = false;
					const auto driver = RefById(cv.driver); if (!driver) continue;
					for (const auto& j : g_jobs) if (j.phase == Phase::Following && j.aiOn && SideOf(j) == side) {
						const auto npc = RefById(j.npc), veh = RefById(j.vehicle); if (!npc || !veh) continue;
						const auto relation = CombatRelationOf(npc, driver); hostile = hostile || relation == CombatRelation::Enemy; friendly = friendly || relation == CombatRelation::Ally;
						const auto mp = At<game::Vec3>(veh, off::kRefPosition); nearby = nearby || std::hypot(double(v.pos[0]) - mp.x, double(v.pos[1]) - mp.y) < 400.0;
					}
					if (!hostile || friendly || !nearby) continue;
					bool duplicate = false; for (int e = 0; e < en; ++e) duplicate = duplicate || enemies[e].id == v.formId;
					if (!duplicate) { auto& e = enemies[en++]; e.id = v.formId; e.pos = {v.pos[0], v.pos[1], v.pos[2]}; e.vel = {v.vel[0], v.vel[1], v.vel[2]}; e.health = vehiclehits::HealthShare(v.formId); }
				}
				if (!g_combatIni.sw.squads) continue;
				const brain::SquadTactic before = sq.tactic;
				brain::Assign(sq, enemies, en, now);
                {
					for (int i = 0; i < sq.count; ++i) {
						for (auto& j : g_jobs) {
							if (j.vehicle != sq.m[i].id || j.phase != Phase::Following) continue;
							if (j.role != sq.m[i].role) logger::Write("NPC vehicles: [%d] squad role: %s -> %s (%s, %d vehicles)", j.pilot, brain::Name(j.role), brain::Name(sq.m[i].role),
								brain::Name(sq.tactic), sq.count);
							j.role = sq.m[i].role; j.roleSlot = sq.m[i].slot; j.coverFor = sq.m[i].coverFor;
                            const auto focus=RefById(sq.focus);
                            const auto actor=focus?(IsActorRef(focus)?focus:CombatDriver(focus)):0;
                            j.squadFocus=actor?At<std::uint32_t>(actor,off::kFormId):0;
						}
					}
				}
				if (sq.tactic != before) logger::Write("NPC vehicles: squad %d tactic: %s -> %s (%d vehicles, %d enemies)", side + 1, brain::Name(before),
					brain::Name(sq.tactic), sq.count, en);
				// The shared memory back to every member.
				for (auto& j : g_jobs) if (j.phase == Phase::Following && j.aiOn && SideOf(j) == side) j.ai.mem.Merge(sq.memory);
			}
		}
		// Rams: two vehicles touching while one closes on the other at 4 m/s or more: tactics::RamDamage to both (through vehiclehits), the rammed
		// job vehicle loses control for a moment (stunned: pushed along the hit, PIT: spinning). Only in fights (a job flown by the AI involved).
		combatrelations::PairCooldown<128> g_ramPairs;
        world::Snapshot g_ramPrevious; ULONGLONG g_ramPreviousAt=0;
        struct PendingImpact { std::uint32_t a=0,b=0; ULONGLONG at=0; double closing=0,ux=0,uy=0; float av[3]{},bv[3]{}; } g_pendingImpacts[128];
        integrationlogic::Box HullBox(const world::VehicleInfo& v) {
            integrationlogic::Box b;for(int k=0;k<3;++k){b.p[k]=v.center[k];b.half[k]=v.half[k];for(int j=0;j<3;++j)b.axis[k][j]=v.rot[k*3+j];}return b;
        }
		void RamTick(ULONGLONG now)
		{
			ReadCombatIni();
			static world::Snapshot snap;
			if (!world::Vehicles(snap)) return;
            const double dt=g_ramPreviousAt&&now>=g_ramPreviousAt?double(now-g_ramPreviousAt)/1000.0:0;
            auto previous=[&](std::uint32_t id)->const world::VehicleInfo*{for(unsigned k=0;k<g_ramPrevious.count&&k<world::kMaxVehicles;++k)if(g_ramPrevious.v[k].formId==id)return &g_ramPrevious.v[k];return nullptr;};
            for (std::uint32_t x = 0; x < snap.count && x < world::kMaxVehicles; ++x) {
				for (std::uint32_t y = x + 1; y < snap.count && y < world::kMaxVehicles; ++y) {
					const auto& A = snap.v[x];
					const auto& B = snap.v[y];
                    const double dx=double(B.center[0])-A.center[0],dy=double(B.center[1])-A.center[1],d=std::hypot(dx,dy);
                    if(!std::isfinite(d)||d<0.01)continue;
                    const double ra=std::hypot(A.half[0],A.half[1]),rb=std::hypot(B.half[0],B.half[1]);
                    PendingImpact* pending=nullptr; PendingImpact* oldest=&g_pendingImpacts[0];
                    for(auto& impact:g_pendingImpacts) {
                        if(impact.a==A.formId&&impact.b==B.formId&&now>=impact.at&&now-impact.at<=250)pending=&impact;
                        if(impact.at<oldest->at)oldest=&impact;
                    }
                    const auto beforeA=previous(A.formId),beforeB=previous(B.formId);
                    if(!pending) {
                        if(!beforeA||!beforeB||dt<=0||dt>0.2)continue;
                        const double travel=std::hypot(double(A.center[0])-beforeA->center[0],double(A.center[1])-beforeA->center[1])+std::hypot(double(B.center[0])-beforeB->center[0],double(B.center[1])-beforeB->center[1]);
                        if(d>ra+rb+travel+0.1)continue;
                        const auto ab=HullBox(A),bb=HullBox(B);
                        if(!integrationlogic::Touching(ab,bb)&&!integrationlogic::SweptTouch(ab,bb,HullBox(*beforeA),HullBox(*beforeB),dt))continue;
                        double ux=double(beforeB->center[0])-beforeA->center[0],uy=double(beforeB->center[1])-beforeA->center[1];const double length=std::hypot(ux,uy);
                        if(length<0.01)continue;ux/=length;uy/=length;
                        const double closing=(double(beforeA->vel[0])-beforeB->vel[0])*ux+(double(beforeA->vel[1])-beforeB->vel[1])*uy;
                        if(closing<4.0||!std::isfinite(closing))continue;
                        *oldest={A.formId,B.formId,now,closing,ux,uy};pending=oldest;
                        std::copy(beforeA->vel,beforeA->vel+3,pending->av);std::copy(beforeB->vel,beforeB->vel+3,pending->bv);
                    }
                    const double ux=pending->ux,uy=pending->uy,closing=pending->closing;
                    const double afterClosing=(double(A.vel[0])-B.vel[0])*ux+(double(A.vel[1])-B.vel[1])*uy;
                    const bool nativeImpact=vehicleflight::ImpactSince(A.formId,pending->at>100?pending->at-100:0)||vehicleflight::ImpactSince(B.formId,pending->at>100?pending->at-100:0);
                    if(!integrationlogic::Impact(closing,afterClosing,nativeImpact))continue;
                    if(!g_ramPairs.Claim(A.formId,B.formId,now))continue;
                    const double aInto=pending->av[0]*ux+pending->av[1]*uy,bInto=-(pending->bv[0]*ux+pending->bv[1]*uy);
                    auto R=aInto>=bInto?A:B; auto V=aInto>=bInto?B:A;
                    const float* rv=aInto>=bInto?pending->av:pending->bv;const float* vv=aInto>=bInto?pending->bv:pending->av;
                    std::copy(rv,rv+3,R.vel);std::copy(vv,vv+3,V.vel);
                    pending->at=0;

					tactics::Unit ru, vu;
					ru.body.pos = { R.pos[0], R.pos[1], R.pos[2] }; ru.body.vel = { R.vel[0], R.vel[1], R.vel[2] }; ru.body.heading = R.heading; ru.body.radius = aInto >= bInto ? ra : rb;
					vu.body.pos = { V.pos[0], V.pos[1], V.pos[2] }; vu.body.vel = { V.vel[0], V.vel[1], V.vel[2] }; vu.body.heading = V.heading; vu.body.radius = aInto >= bInto ? rb : ra;
					const auto kind = tactics::ClassifyRam(ru, vu);
					auto res = tactics::RamDamage(closing, kind == tactics::RamKind::None ? tactics::RamKind::HeadOn : kind, R.mass, V.mass);
					if (res.damageIt <= 0.0) continue;
					// One ram never takes more than ramCap of a vehicle's own max health (every vehicle has the configured health).
					const double maxHealth = vehiclehits::MaxHealth();
					double rawIt = 0, rawMe = 0;
					const bool capped = tactics::CapRam(res, maxHealth, maxHealth, vehiclehits::RamCap(), &rawIt, &rawMe);
					const uptr rammerVehicle = RefById(R.formId), victimVehicle = RefById(V.formId);
					const uptr rammer = CombatDriver(rammerVehicle), victim = CombatDriver(victimVehicle), player = Ptr(addr::kPlayerCharacterPtr);
					// Allied collisions cause no damage, driver injury, aggression, crime or stun.
					if (VehicleFriendlyToShooter(victimVehicle, R.formId) || VehicleFriendlyToShooter(rammerVehicle, V.formId)) continue;
					// Neutrality must be sampled before StartCombat changes it; a ram raises assault, never a theft alarm.
					const bool protectedVictim = victim && PlayerTeam(victim);
					const bool hostileBefore = victim && player && (HostileTo(victim, player) || HostileTo(player, victim) || FactionReaction(victim, player) == 1);
					const auto policy = combatrelations::RamConsequences(closing, rammer != 0, victim != 0, rammer && rammer == player, protectedVictim, hostileBefore);
					if (policy.assault) CrimeAlarms(victim, victimVehicle, player, true, false);
					if (policy.attack && victimVehicle && !VehicleFriendlyToShooter(victimVehicle, At<std::uint32_t>(rammer, off::kFormId)))
						VehicleAggression(victimVehicle, At<std::uint32_t>(rammer, off::kFormId), !policy.assault);
					if (policy.witness) WitnessCrash((double(R.pos[0]) + V.pos[0]) * 0.5, (double(R.pos[1]) + V.pos[1]) * 0.5, (double(R.pos[2]) + V.pos[2]) * 0.5, "hard vehicle ram");
					const int side = rammer && PlayerTeam(rammer) ? 1 : 0;
					vehiclehits::ApplyDamage(V.formId, float(res.damageIt), R.formId, side, "a ram");
					vehiclehits::ApplyDamage(R.formId, float(res.damageMe), V.formId, 0, "its own ram");
					static const char* const kinds[] = { "bump", "T-bone", "head-on", "rear", "PIT" };
					char cap[64] = "";
					if (capped) std::snprintf(cap, sizeof(cap), " (capped from %.0f / %.0f)", rawIt, rawMe);
					logger::Write("NPC vehicles: ram: %08X -> %08X, %s at %.0f m/s: %.0f / %.0f damage%s, %08X out of control %u ms", R.formId, V.formId, kinds[int(kind)], closing,
						res.damageIt, res.damageMe, cap, V.formId, res.stunItMs);
					for (auto& j : g_jobs) {
						if (j.vehicle != V.formId || j.phase != Phase::Following) continue;
						j.stunUntil = now + res.stunItMs;
						j.stunX = ux * (aInto >= bInto ? 1.0 : -1.0); j.stunY = uy * (aInto >= bInto ? 1.0 : -1.0);
						j.stunSpin = kind == tactics::RamKind::PIT ? 2.5 : 0.8;
					}
				}
			}
            g_ramPrevious=snap;g_ramPreviousAt=now;
		}
		void AiBudget(double us)
		{
			g_aiUs += us;
			g_aiMaxUs = std::max(g_aiMaxUs, us);
			const ULONGLONG now = GetTickCount64();
			if (!g_aiLogAt) g_aiLogAt = now;
			if (now - g_aiLogAt >= 60000) {
				logger::Write("NPC vehicles: combat decision kernel: %.3f ms per frame over %u frames, worst kernel tick %.3f ms (full frame measured separately)", g_aiFrames ? g_aiUs / g_aiFrames / 1000.0 : 0.0,
					g_aiFrames, g_aiMaxUs / 1000.0);
				g_aiLogAt = now; g_aiUs = 0; g_aiMaxUs = 0; g_aiFrames = 0;
			}
		}
		// One frame of the full AI for a job in a fight: fills combatai::Situation, runs it, applies the output (goal, commands, aim, fire).
		// False only when the AI does not fly it this frame.
		bool CombatAITick(Job& j, uptr npc, uptr foe, uptr aimRef, bool foeInVeh, uptr vehicle, uptr player, bool pursuer, bool aggressive, bool mayAim,
			bool protect, ULONGLONG now, vcontract::Goal& g)
		{
			LARGE_INTEGER t0, t1, fq;
			QueryPerformanceCounter(&t0);
			ReadCombatIni();
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const auto vr = At<game::Vec3>(vehicle, off::kRefRotation);
			const auto fp = At<game::Vec3>(aimRef, off::kRefPosition);
			const auto fr = At<game::Vec3>(aimRef, off::kRefRotation);
			const std::uint32_t foeId = At<std::uint32_t>(aimRef, off::kFormId);
			// Velocities (smoothed from positions, as the staged manoeuvres did).
			if (j.manFoe != foeId || !j.manAt || now <= j.manAt || now - j.manAt > 1000) { j.manMvx = j.manMvy = j.manFvx = j.manFvy = 0; }
			else {
				const double dt = (now - j.manAt) / 1000.0, a = std::min(1.0, dt * 4.0);
				const auto smooth = [&](double& v, double cur, double was) { const double inst = (cur - was) / dt; if (std::fabs(inst) < 150.0) v += (inst - v) * a; };
				smooth(j.manMvx, vp.x, j.manMx); smooth(j.manMvy, vp.y, j.manMy);
				smooth(j.manFvx, fp.x, j.manFx); smooth(j.manFvy, fp.y, j.manFy);
			}
			j.manMx = vp.x; j.manMy = vp.y; j.manFx = fp.x; j.manFy = fp.y; j.manAt = now; j.manFoe = foeId;
			static world::Snapshot snap;
			const bool haveSnap = world::Vehicles(snap);
            auto verticalOf=[&](std::uint32_t id){if(haveSnap)for(unsigned k=0;k<snap.count&&k<world::kMaxVehicles;++k)if(snap.v[k].formId==id)return double(snap.v[k].vel[2]);return 0.0;};
			auto radiusOf = [&](std::uint32_t id) { if (haveSnap) for (std::uint32_t k = 0; k < snap.count && k < world::kMaxVehicles; ++k) if (snap.v[k].formId == id) return std::max(1.5, std::hypot(double(snap.v[k].half[0]), double(snap.v[k].half[1])) * 0.8); return 2.5; };
			combatai::Situation s;
			s.now = now;
			s.dt = j.aiAt && now > j.aiAt ? std::min(0.25, (now - j.aiAt) / 1000.0) : 0.016;
			j.aiAt = now;
			s.me.id = j.vehicle;
			s.me.unit.body.pos = { vp.x, vp.y, vp.z }; s.me.unit.body.vel = { j.manMvx, j.manMvy, verticalOf(j.vehicle) }; s.me.unit.body.heading = vr.z; s.me.unit.body.radius = radiusOf(j.vehicle);
			if(haveSnap)for(unsigned k=0;k<snap.count&&k<world::kMaxVehicles;++k){if(snap.v[k].formId==j.vehicle)s.me.unit.mass=snap.v[k].mass;if(snap.v[k].formId==foeId)s.foe.unit.mass=snap.v[k].mass;}
            s.me.unit.maxSpeed=vehicleflight::CruiseLimit(j.vehicle); s.me.turnRate=vehicleflight::TurnLimit(j.vehicle); s.me.unit.health = vehiclehits::HealthShare(j.vehicle); s.me.unit.hover = true;
			s.me.wrecked = vehiclehits::Wrecked(j.vehicle);
			s.me.person = brain::PersonalityFor(g_combatIni.sw.personality ? TemperOf(npc) : brain::Temper::Default, j.npc, Difficulty());
			s.haveFoe = true; j.lastEnemyActor=At<std::uint32_t>(foe,off::kFormId);
			s.foe.id = foeId; s.foe.inVehicle = foeInVeh;
			s.foe.unit.body.pos = { fp.x, fp.y, fp.z }; s.foe.unit.body.vel = { j.manFvx, j.manFvy, verticalOf(foeId) }; s.foe.unit.body.heading = fr.z; s.foe.unit.body.radius = foeInVeh ? radiusOf(foeId) : 0.6;
			s.foe.unit.maxSpeed=foeInVeh?vehicleflight::CruiseLimit(foeId):8.0;
			float hn = 0, hm = 0;
			s.foe.unit.health = foeInVeh ? vehiclehits::HealthShare(foeId) : (vehiclehits::ActorHealth(foe, hn, hm) ? hn / hm : 1.0);
			s.enemies[s.enemyCount++] = s.foe.unit;
			{
				const float eye[3] = { vp.x, vp.y, vp.z + 1.5f }, at[3] = { fp.x, fp.y, fp.z + 1.0f };
				s.foe.visible = !environment::LineBlocked(eye, at);
			}
			const std::uint32_t foeActor = At<std::uint32_t>(foe, off::kFormId);
			const auto shot = std::max(vehiclehits::LastShotBy(foeActor), vehiclehits::LastShotBy(foeId));
			s.foe.firing = shot && now - shot < 1500;
			s.foe.heard = brain::Hears(std::hypot(double(fp.x) - vp.x, double(fp.y) - vp.y), s.foe.firing, std::hypot(j.manFvx, j.manFvy));
			// Nearby vehicles, the squad, the enemies.
			if (haveSnap) {
				for (std::uint32_t k = 0; k < snap.count && k < world::kMaxVehicles && s.otherCount < combatai::Situation::kOtherMax; ++k) {
					const auto& v = snap.v[k];
					if (v.formId == j.vehicle || v.formId == foeId) continue;
					auto& b = s.others[s.otherCount++];
					b.pos = { v.pos[0], v.pos[1], v.pos[2] }; b.vel = { v.vel[0], v.vel[1], v.vel[2] }; b.heading = v.heading; b.radius = std::max(1.5, std::hypot(double(v.half[0]), double(v.half[1])) * 0.8);
				}
			}
			int rank = 0;
			for (const auto& o : g_jobs) {
				if (&o == &j || o.phase != Phase::Following || !o.aiOn) continue;
				const uptr ov = RefById(o.vehicle);
				if (!ov) continue;
				const auto op = At<game::Vec3>(ov, off::kRefPosition);
				const auto relation = CombatRelationOf(npc, RefById(o.npc));
				if (relation == CombatRelation::Ally) {
					if (s.mateCount < 8) { auto& m = s.mates[s.mateCount++]; m.pos = { op.x, op.y, op.z }; m.vel = { o.manMvx, o.manMvy, verticalOf(o.vehicle) }; m.heading = At<game::Vec3>(ov, off::kRefRotation).z; m.radius = radiusOf(o.vehicle); }
					if (o.foe == j.foe) { ++s.alliesOnTarget; if (o.pilot < j.pilot) ++rank; }
				}
				else if (relation == CombatRelation::Enemy) {
					if (o.vehicle != foeId && s.enemyCount < combatai::Situation::kOtherMax) { auto& e = s.enemies[s.enemyCount++]; e.body.pos = { op.x, op.y, op.z }; e.body.vel = { o.manMvx, o.manMvy, 0 }; e.health = vehiclehits::HealthShare(o.vehicle); }
					if (o.foe == j.npc) ++s.enemiesOnMe;
				}
			}
			// Non-job enemy vehicles count for threat, terrain tactics and squad odds too.
			for (int k = 0; k < g_combatVehicleCount && s.enemyCount < combatai::Situation::kOtherMax; ++k) {
				const auto& cv = g_combatVehicles[k]; const auto& v = cv.vehicle;
				if (!cv.driver || v.formId == j.vehicle || v.formId == foeId || vehiclehits::Wrecked(v.formId)) continue;
				bool job = false; for (const auto& o : g_jobs) job = job || (o.phase == Phase::Following && o.aiOn && o.vehicle == v.formId);
				if (job || std::hypot(double(v.pos[0]) - vp.x, double(v.pos[1]) - vp.y) > 400.0 || CombatRelationOf(npc, RefById(cv.driver)) != CombatRelation::Enemy) continue;
				auto& e = s.enemies[s.enemyCount++]; e.body.pos = {v.pos[0], v.pos[1], v.pos[2]}; e.body.vel = {v.vel[0], v.vel[1], v.vel[2]};
				e.body.heading = v.heading; e.body.radius = std::max(1.5, std::hypot(double(v.half[0]), double(v.half[1])) * 0.8); e.health = vehiclehits::HealthShare(v.formId);
			}
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			s.anchor = { pp.x, pp.y, pp.z };
			s.zone = pursuer || aggressive ? g_combatIni.zonePursuit : g_combatIni.zoneEscort;
			if (protect) {
				s.protect = true;
				s.protectee.pos = { pp.x, pp.y, pp.z };
				const double dx = pp.x - fp.x, dy = pp.y - fp.y, d = std::max(1.0, std::hypot(dx, dy));
				s.protectThreat = std::clamp((j.manFvx * dx + j.manFvy * dy) / d / 20.0, 0.0, 1.0);
			}
			if (j.lead) {
				const uptr lv = RefById(j.lead);
				if (lv) {
					const auto lp = At<game::Vec3>(lv, off::kRefPosition);
					s.hasLeader = true; s.leader.pos = { lp.x, lp.y, lp.z }; s.leader.heading = At<game::Vec3>(lv, off::kRefRotation).z;
					s.slot = tactics::ConvoySlot(j.roleSlot); s.iDefendConvoy = true;
				}
			}
			s.role = g_combatIni.sw.squads ? j.role : brain::Role::Engager;
			s.roleSlot = j.roleSlot;
			if (g_combatIni.sw.squads && SideOf(j) >= 0 && g_sides[SideOf(j)].squad.focus == foeId)
				s.firingHeading = g_sides[SideOf(j)].squad.firingHeading;
			if (j.coverFor) {
				const uptr cv = RefById(j.coverFor);
				if (cv) { const auto cp = At<game::Vec3>(cv, off::kRefPosition); s.haveCover = true; s.coverBody.pos = { cp.x, cp.y, cp.z }; }
			}
			const auto lastHit = vehiclehits::LastHitMs(j.vehicle);
			s.hitRecently = lastHit && now - lastHit < 2000;
			s.allyWrecked = j.sideEvents[0]; s.allyDriverKilled = j.sideEvents[1]; s.leaderKilled = j.sideEvents[2]; s.reinforcements = j.reinforced;
			s.playerOnFoot = !EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(player) && Dist(vehicle, player) < 30.0;
			for (const auto& o : g_jobs) {
				if (o.phase != Phase::Following || !o.lead) continue;
				if (o.vehicle == foeId || o.lead == foeId) s.targetInConvoy = true;
				if (o.lead == foeId) s.targetIsCargo = true;
			}
			s.breakOff = g_combatIni.breakOff;
			const auto ballistic=vehiclehits::BallisticsOf(j.vehicle);
            s.weapon.range=ballistic.range;s.weapon.cone=FireCone(Dist3(vehicle,aimRef));s.weapon.shotSpeed=ballistic.observedSpeed?ballistic.speed:std::numeric_limits<double>::infinity();
			s.civilian = s.me.person.temper == brain::Temper::Civilian;
			(void)rank;
			static NavWorld world;
			combatai::Output out;
			combatai::Tick(j.ai, s, world, g_combatIni.sw, out);
			j.aiOn = true;
			if (out.changed) {
				logger::Write("NPC vehicles: [%d] %s; enemy %08X %.0f m, health %.0f%% vs %.0f%%, morale %.2f (%s), %s", j.pilot, out.log, foeId,
					std::hypot(double(fp.x) - vp.x, double(fp.y) - vp.y), s.me.unit.health * 100.0, s.foe.unit.health * 100.0, j.ai.morale.value, brain::Name(j.ai.mood),
					out.useCommands ? (out.avoiding ? "steering (avoidance)" : "steering") : out.terrainBlocked ? "planner (terrain ahead)" : out.hold ? "planner (firing hold)" : "planner (steering disabled)");
			}
			if (out.moodChanged) logger::Write("NPC vehicles: [%d] morale %.2f: %s", j.pilot, j.ai.morale.value, brain::Name(j.ai.mood));
			// The goal: the carrot (cruise; a held spot arrives); the planner keeps it safe.
			g.x = out.carrot.x; g.y = out.carrot.y; g.z = out.carrot.z;
			g.stop = out.hold ? 3.0 : 0.0; g.hold = out.hold; g.cruise = !out.hold; g.cruiseSpeed = out.speed;
            if (out.hold) {
                g.nav.passThrough=false;
                const float cap=float(std::max(1.0,out.speed));
                g.nav.speedLimit=g.nav.speedLimit>0?std::min(g.nav.speedLimit,cap):cap;
            }
			vehicleflight::Command c;
			c.set = 0;
			// CB-2 (combat on the plan): tactics unchanged; the carrot above is the goal, the aim is Route's facing. While a combat line
			// drives this pilot only the aim pitch goes as a stick; a stun, or a refused line, keeps today's stick path.
			const bool aimNow = out.aim && mayAim;
			const bool onPlan = vehicleflight::PilotCombatAim(j.pilot, out.aimPoint.x, out.aimPoint.y, out.aimPoint.z, aimNow, now >= j.stunUntil);
			if (onPlan != j.aiPlan) {
				j.aiPlan = onPlan;
				logger::Write("OBS system=plan.combat vehicle=%08X pilot=%d event=%s aim=%d reason=%s", j.vehicle, j.pilot, onPlan ? "sticks_dropped" : "sticks_back", int(aimNow),
					onPlan ? "combat_line_drives_the_plan_moves_it" : now < j.stunUntil ? "stunned" : "no_combat_line_this_frame");
			}
			if (onPlan) {
				if (aimNow && !std::isnan(out.cmd.pitch)) { c.set = vehicleflight::Command::kPitch; c.pitch = out.cmd.pitch; }
				else if (aimNow) { c.set = vehicleflight::Command::kPitch; c.pitch = std::clamp(std::atan2(out.aimPoint.z-vp.z,std::max(1.0,std::hypot(out.aimPoint.x-vp.x,out.aimPoint.y-vp.y))),-0.6,0.6); }
			}
			else if (now < j.stunUntil) {
				// Rammed: out of control, drifting along the hit and spinning.
				const double h = vr.z;
				c.set = vehicleflight::Command::kYaw | vehicleflight::Command::kThrottle | vehicleflight::Command::kStrafe;
				c.yawRate = j.stunSpin; c.throttle = j.stunX * std::sin(h) + j.stunY * std::cos(h); c.strafe = j.stunX * std::cos(h) - j.stunY * std::sin(h);
			}
			else if (out.useCommands) {
				c.set = vehicleflight::Command::kYaw | vehicleflight::Command::kThrottle | vehicleflight::Command::kStrafe | vehicleflight::Command::kVertical |
					vehicleflight::Command::kGear | vehicleflight::Command::kBrake;
				c.yawRate = out.cmd.yawRate; c.throttle = out.cmd.throttle; c.strafe = out.cmd.strafe; c.vertical = out.cmd.vertical; c.gear = out.cmd.gear; c.brake = out.cmd.brake;
				if (out.aim && mayAim && !std::isnan(out.cmd.pitch)) { c.set |= vehicleflight::Command::kPitch; c.pitch = out.cmd.pitch; }
				if (!mayAim && out.aim) {
					// Weapons "never": no nose on the target; fly along the desired velocity instead.
					const double want = std::atan2(out.carrot.x - vp.x, out.carrot.y - vp.y);
					c.yawRate = std::clamp(2.5 * WrapAngle(want - vr.z), -s.me.turnRate, s.me.turnRate);
				}
			}
			else if (out.hold && out.aim && mayAim && std::hypot(out.carrot.x-vp.x,out.carrot.y-vp.y)<10.0) {
				// At the firing position, let navigation hold still while the hull aims.
				c.set = vehicleflight::Command::kYaw | vehicleflight::Command::kPitch;
				c.yawRate = std::clamp(2.5 * WrapAngle(std::atan2(out.aimPoint.x-vp.x,out.aimPoint.y-vp.y)-vr.z),-s.me.turnRate,s.me.turnRate);
				c.pitch = std::clamp(std::atan2(out.aimPoint.z-vp.z,std::max(1.0,std::hypot(out.aimPoint.x-vp.x,out.aimPoint.y-vp.y))),-0.6,0.6);
			}
			c.holdMs=100; vehicleflight::PilotCommand(j.pilot,c);
			j.aiFire = out.fire && mayAim;
			if (out.aim) { j.aiAimX = out.aimPoint.x; j.aiAimY = out.aimPoint.y; j.aiAimZ = out.aimPoint.z; }
			else { j.aiAimX = fp.x; j.aiAimY = fp.y; j.aiAimZ = fp.z + (foeInVeh ? 1.0 : kAimHeight); }
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&fq);
			AiBudget(double(t1.QuadPart - t0.QuadPart) * 1e6 / double(fq.QuadPart));
			return true;
		}

        bool SearchAITick(Job& j,uptr npc,uptr vehicle,ULONGLONG now,vcontract::Goal& goal) {
            const auto track=j.ai.mem.Find(j.ai.lastFoe);
            const auto observed=track?std::max(track->seenAt,track->heardAt):0;
            const auto actor=RefById(j.lastEnemyActor);
            if(!track||!observed||now<observed||now-observed>=60000||
                (actor&&(Dead(actor)||CombatRelationOf(npc,actor)!=CombatRelation::Enemy))) {
                j.ai.search.active=false;return false;
            }
            combatai::Situation situation; situation.now=now;situation.dt=j.aiAt&&now>j.aiAt?std::min(0.25,double(now-j.aiAt)/1000.0):0.016;
            j.aiAt=now;const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
            situation.me.id=j.vehicle;situation.me.unit.body.pos={pos.x,pos.y,pos.z};
            situation.me.unit.body.heading=At<game::Vec3>(vehicle,off::kRefRotation).z;
            static thread_local world::Snapshot snap;if(world::Vehicles(snap))for(unsigned k=0;k<snap.count&&k<world::kMaxVehicles;++k)if(snap.v[k].formId==j.vehicle){const auto&v=snap.v[k];situation.me.unit.body.vel={v.vel[0],v.vel[1],v.vel[2]};situation.me.unit.mass=v.mass;situation.me.unit.body.radius=std::hypot(v.half[0],v.half[1]);}
            situation.me.unit.maxSpeed=vehicleflight::CruiseLimit(j.vehicle);situation.me.turnRate=vehicleflight::TurnLimit(j.vehicle);
            situation.me.unit.health=vehiclehits::HealthShare(j.vehicle);situation.me.wrecked=vehiclehits::Wrecked(j.vehicle);
            situation.me.person=brain::PersonalityFor(TemperOf(npc),j.npc,Difficulty());
            situation.haveFoe=false;situation.foe.visible=false;situation.anchor={j.originX,j.originY,pos.z};situation.zone=g_combatIni.zonePursuit;
            static NavWorld world;combatai::Output out;combatai::Tick(j.ai,situation,world,g_combatIni.sw,out);
            if(out.changed)logger::Write("NPC vehicles: [%d] %s (remembered native enemy %08X)",j.pilot,out.log,j.lastEnemyActor);
            goal.x=out.carrot.x;goal.y=out.carrot.y;goal.z=out.carrot.z;goal.stop=0;goal.hold=false;goal.cruise=true;goal.cruiseSpeed=out.speed;
            j.aiOn=true;j.aiFire=false;vehicleflight::ClearPilotCommand(j.pilot);return true;
        }

		// ---- Civil tasks (2026-10-20, the user: transit, drop-off and walk, return home, parking etiquette, outpost patrol) ----
		constexpr double kPatrolR = 60.0, kTaskReach = 14.0, kTransitArrive = 25.0, kHomeArrive = 20.0;
		constexpr double kParkClear = 7.0;     // m: no other vehicle this close to a parking spot
		constexpr int kPatrolCorners = 8;      // two laps of four corners
		constexpr ULONGLONG kTaskMaxMs = 240000;
		constexpr double kFleeNear = 80.0, kFleeDist = 150.0, kOfferNear = 15.0;
		constexpr ULONGLONG kOfferMs = 10000, kSupplyWaitMs = 20000;
		void CallReinforcements(uptr player, uptr at);  // (one unnamed namespace per file: defined further down)
		void PickUps(uptr player, ULONGLONG now);
		void FollowersRide(uptr player, ULONGLONG now);

		// Parking etiquette: a clear spot near (x, y): the point itself, else rings of 8, 14 and 20 m (8 directions); no other vehicle within
		// kParkClear, never on top of the player.
		bool ParkSpot(std::uint32_t self, uptr player, double x, double y, double& ox, double& oy)
		{
			static world::Snapshot snap;
			const bool have = world::Vehicles(snap);
			const auto pp = player ? At<game::Vec3>(player, off::kRefPosition) : game::Vec3{};
			const auto playerVehicle = player ? InVehicle(player) : 0;
			const bool playerAboard = playerVehicle && At<std::uint32_t>(playerVehicle, off::kFormId) == self;
			const double rings[4] = { 0.0, 8.0, 14.0, 20.0 };
			for (const double r : rings) {
				for (int k = 0; k < (r == 0.0 ? 1 : 8); ++k) {
					const double a = k * 0.7853981633974483;
					const double cx = x + std::sin(a) * r, cy = y + std::cos(a) * r;
					bool clear = !player || playerAboard || std::hypot(cx - pp.x, cy - pp.y) > 6.0;
					for (std::uint32_t i = 0; clear && have && i < snap.count && i < world::kMaxVehicles; ++i) {
						const auto& v = snap.v[i];
						if (v.formId && v.formId != self && std::hypot(cx - v.pos[0], cy - v.pos[1]) < kParkClear) clear = false;
					}
					if (clear) {
                        const auto vehicle=RefById(self);if(!vehicle)return false;
                        const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
                        const auto site=environment::CheckSite(self,cx,cy,pos.z,At<game::Vec3>(vehicle,off::kRefRotation).z);
                        if(site.verdict==environment::Verdict::Pending)return false;
                        if(site.verdict==environment::Verdict::Safe){ox=cx;oy=cy;return true;}
                    }
				}
			}
			return false;
		}

		void BeginPark(Job& j, uptr player, double x, double y, const char* why)
		{
			j.repeatTraffic = false;
			j.trafficDwell = {};
			if(!ParkSpot(j.vehicle,player,x,y,j.taskX,j.taskY)) {
                // Keep the parking search active; an unchecked original point is never treated as safe.
                j.taskX=x;j.taskY=y;
            }
			j.task = 4;
			j.taskAt = GetTickCount64();
			logger::Write("NPC vehicles: [%d] %s; parking at a clear spot (%.0f, %.0f)", j.pilot, why, j.taskX, j.taskY);
		}



		// Seat hold (2026-10-30 07:34 probe run): sitting drivers (sit state 3) at the game's reduced simulation (process level 1+)
		// were not moved with their flying vehicle (17 of the 19 left behind were level 1; 155 of 174 carried ones were level 0).
		// While the game moves a driver (level 0, at its seat) its offset in the vehicle's frame is recorded; while it does not,
		// FVO puts the driver there every frame, turned with the vehicle. Yaw h: forward (sin h, cos h), right (cos h, -sin h).
		void HoldInSeat(Job& j, uptr npc, uptr vehicle, bool aboard)
		{
			// Any driver listed aboard (2026-10-30 07:44 run: after the game reloaded drivers' models their sit state reset to 0, and
			// at reduced simulation a snap does not stick (the sit animation does not run): they stayed at sit 2 or fell out while
			// the hold only covered sit 3, and their vehicles flew off without them).
			if (!aboard || !Has3D(npc) || !Has3D(vehicle)) return;
			const bool sitting = trafficpair::Sitting(SitState(npc));
			const auto v = At<game::Vec3>(vehicle, off::kRefPosition), d = At<game::Vec3>(npc, off::kRefPosition);
			const float h = At<game::Vec3>(vehicle, off::kRefRotation).z, c = std::cos(h), sn = std::sin(h);
			const float dx = d.x - v.x, dy = d.y - v.y, dz = d.z - v.z;
			const int level = ProcessLevel(npc);
			const bool inReach = dx * dx + dy * dy + dz * dz < 2.5f * 2.5f;
			auto learn = [&] { j.seatOff[0] = dx * c - dy * sn; j.seatOff[1] = dx * sn + dy * c; j.seatOff[2] = dz;
				j.seatYaw = At<game::Vec3>(npc, off::kRefRotation).z - h; j.haveSeatOff = true; };
			// The game carries a driver only while its own parent cell is attached (08:52 trace run, sitting drivers: driver cell state 6
			// with the vehicle's at 10, the vehicle being persistent: left behind in 68 of 69 readings; driver cell 10: at its seat in 266
			// of 276). The cell, not the level: with the level kept at 0 these drivers are still left behind.
			const bool attached = CellAttached(npc);
			if (level == 0 && attached) {
				if (sitting && inReach) learn();  // the game seats and moves it: learn its seat, never write its position
				// Not sitting: the snap repair handles it; the hold stays off (07:49 run: holding these drivers at the vehicle centre
				// before a snap knocked them out).
				return;
			}
			// Seat not learned yet: its current place in the vehicle is taken as its seat when it is within reach; never a guess
			// such as the vehicle centre.
			if (!j.haveSeatOff) { if (inReach) learn(); return; }
			const float lx = j.seatOff[0], ly = j.seatOff[1], lz = j.seatOff[2];
			const float target[3] = { v.x + lx * c + ly * sn, v.y - lx * sn + ly * c, v.z + lz };
			const float ex = d.x - target[0], ey = d.y - target[1], ez = d.z - target[2];
			if (ex * ex + ey * ey + ez * ez < 0.0025f) return;
			if (PlaceSeated(npc, vehicle, trafficpair::SeatPose{ lx, ly, lz, j.seatYaw, true }) && ++j.seatHolds == 1)
				logger::Write("NPC vehicles: [%d] seat hold: driver %08X in %08X (sit %d) is not moved by the game (process level %d, %.1f m off its seat); FVO keeps it in the seat",
					j.pilot, j.npc, j.vehicle, SitState(npc), level, std::sqrt(ex * ex + ey * ey + ez * ez));
		}

		// A traffic driver whose seat link dropped, kept at its seat, in the service (the vehicle's position at that moment). Only TaskTick's
		// re-seat branch calls it, where a fight, a wreck or the player pulling the driver out has already been ruled out.
		constexpr double kHoldReach = 15.0;
		void HoldUnlinked(uptr npc, uptr vehicle)
		{
			const unsigned id = At<unsigned>(npc, off::kFormId), vid = At<unsigned>(vehicle, off::kFormId);
			static std::atomic<unsigned> held{0};
			lifecycle::Post("seat-hold:" + std::to_string(id), id, [=]() -> lifecycle::Work {
				const auto a = lifecycle::Keep(npc, id), v = lifecycle::Keep(vehicle, vid);
				if (!a || !v) return {};
				return [=]() -> std::uint64_t {
					if (!lifecycle::InService()) return 0;
					const uptr actor = reinterpret_cast<uptr>(a.get()), veh = reinterpret_cast<uptr>(v.get());
					// Eligibility can change while this command waits in the service queue.
					if (IsDead(actor) || !Has3D(actor) || !Has3D(veh) || InVehicle(actor) || InCombat(actor) || vehiclehits::Wrecked(vid)) return 0;
					const double d = Dist3(actor, veh);
					if (!std::isfinite(d) || d > kHoldReach || !trafficpair::NeedsBringing(SitState(actor), d)) return 0;
					const auto vp = At<game::Vec3>(veh, off::kRefPosition);
					const float heading = At<game::Vec3>(veh, off::kRefRotation).z;
					float target[3]; trafficpair::SeatPoint(vp.x, vp.y, vp.z, heading, trafficpair::SeatPose{}, target);
					if (!PlaceSeatedNative(actor, target, heading)) return 0;
					if (held.fetch_add(1) < 8) logger::Write("NPC vehicles: traffic driver %08X out of the seat of %08X with no seat link: held at the seat (%.1f m off) until it can be seated", id, vid, d);
					return 1;
				};
			});
		}

		bool InstantExit(uptr npc, bool eject);
		bool WarpExitController(uptr npc, float x, float y, float z);

		// One frame of a civil task (a Following job with j.task set).
		void TaskTick(Job& j, uptr npc, uptr vehicle, uptr player, bool aboard, ULONGLONG now)
		{
			const int observedSit=SitState(npc);const bool observedDriver3D=Has3D(npc),observedVehicle3D=Has3D(vehicle);
			logger::Observe("driver.seat",j.vehicle,unsigned(aboard)|(std::uint64_t(unsigned(observedSit))<<1)|(std::uint64_t(observedDriver3D)<<8)|(std::uint64_t(observedVehicle3D)<<9),
				"driver=%08X pilot=%d task=%d aboard=%d sit=%d driver_model=%d vehicle_model=%d separation=%.2f driver_cell_ready=%d vehicle_cell_ready=%d reseats=%d",
				j.npc,j.pilot,j.task,aboard,observedSit,observedDriver3D,observedVehicle3D,Dist3(npc,vehicle),CellAttached(npc),CellAttached(vehicle),j.reseats);
			logger::Observe("npc.behaviour",j.vehicle,unsigned(j.phase)|(std::uint64_t(j.foe)<<8),"driver=%08X pilot=%d task=%d phase=%s enemy=%08X combat_ai=%d retreating=%d cold_start=%d",j.npc,j.pilot,j.task,Name(j.phase),j.foe,j.aiOn,j.retreating,j.coldStart);
			if (j.coldStart && j.task == 9) HoldInSeat(j, npc, vehicle, aboard);
			// At reduced simulation (process level 1+) an aboard driver is held in its seat by HoldInSeat and is snapped only once the
			// game fully simulates it again: a snap there does not stick and knocked drivers out (07:44 run).
			const bool heldReduced = aboard && j.coldStart && j.task == 9 && ProcessLevel(npc) >= 1 && Has3D(npc) && Has3D(vehicle);
			// In a fight or from a wreck a driver out of the seat stays out (below); so does one the player pulled out, whose job has already ended.
			const bool fightOut = !aboard && (vehiclehits::Wrecked(j.vehicle) || InCombat(npc) || (player && InCombat(player) && Dist(vehicle, player) < 120.0));
			if (!heldReduced && !j.flyOnSince && !fightOut && !SeatedNow(npc, vehicle, aboard) && j.coldStart && j.task == 9 && npc && !IsDead(npc)) {
				// Held at the vehicle, it flies on, and the model guard keeps it out of sight: nothing shows a repair. After five failed seats it descends through the existing stop path, and at rest the
				// driver simply gets out beside it.
				// Held at the vehicle every frame, linked or not (09:04 run, FF01F04E: brought once a second at most and only if still linked, it fell 60 m between tries, out of snap reach each time).
				// PlaceSeated refuses a driver whose seat link has dropped (aboard 0), and that is what happens when a model reloads far away:
				// 17:20 run, FF1001A6 came back 1.0 m from its vehicle, unlinked, and was 66 m away on the ground 3 s later. This branch has
				// already excluded fights, wrecks and pull-outs, so an unlinked traffic driver is held at its seat too (HoldUnlinked), but only
				// within kHoldReach: a driver already far off is never pulled back in sight; the pair check retires that pair.
				if (Has3D(npc) && Has3D(vehicle) && trafficpair::NeedsBringing(SitState(npc), Dist3(npc, vehicle))) {
					if (InVehicle(npc) == vehicle) PlaceSeated(npc, vehicle, {});
					else if (Dist3(npc, vehicle) <= kHoldReach) HoldUnlinked(npc, vehicle);
				}
				// While the driver's or the vehicle's cell is still loading (state below 9) a seat never sticks (16:55 run: FF10E559 and its
				// vehicle both in state 6, knocked out and seated again every second for over a minute, then out of the seat 46 m up). The
				// hold above keeps it at the vehicle; tries wait, uncounted, until both cells are attached.
				const bool cellsReady = CellAttached(npc) && CellAttached(vehicle);
				if (!cellsReady && now - j.reseatAt >= 1000u) {
					j.reseatAt = now;
					static std::unordered_map<std::uint32_t, ULONGLONG> told;
					ULONGLONG& at = told[j.npc];
					if (now - at > 10000) { at = now; logger::Write("NPC vehicles: [%d] traffic driver %08X out of the seat of %08X while a cell is still loading: seat tries wait for it (none counted)", j.pilot, j.npc, j.vehicle); }
				}
				if (cellsReady && now - j.reseatAt >= (j.reseats < 5 ? 1000u : 5000u)) {  // past five it lands; slower tries go on, since at rest a seat often takes
					j.reseatAt = now;
					if (j.reseats < 5) ++j.reseats;
					// Every input of the seat check, so a run shows which one failed (vehicle model, driver model, aboard, sit, distance).
					const int sitBefore = SitState(npc);const double seatDist = SeatDist(npc, vehicle), refDist = Dist3(npc, vehicle);
					// Only a driver at its vehicle is snapped in (2026-10-30, 06:42 run): a snap from tens of metres left drivers stuck in the
					// getting-in state (sit 1) frozen in mid-air, or threw them to one far point. -3: too far, not snapped.
					// A linked driver that is not sitting (sit state 0-2) at its vehicle gets the snap; a far one is not snapped (-3).
					const int r = trafficpair::NeedsSnap(Has3D(vehicle), Has3D(npc), sitBefore, seatDist) ? SeatNewborn(npc, vehicle) : -3;
					// A snap still in the queue answers 0 on this poll and 2 on the next: not a failed try (2026-10-31: 65 of 186 reports; five polls ended trips over about two real snaps).
					const bool snapPending = r == 0 && lifecycle::Pending(j.npc, "seat");
					if (snapPending && j.reseats > 0 && j.reseats < 5) --j.reseats;
					logger::Observe("driver.seat", j.vehicle, 0x900u | unsigned(r + 8), "driver=%08X event=reseat result=%d pending=%d tries=%d sit=%d model_distance=%.1f reference_distance=%.1f", j.npc, r, snapPending, j.reseats, sitBefore, seatDist, refDist);
					// Where each one is (2026-10-30: drivers 50-97 m from vehicles that had not moved; which one moved is unknown).
					const auto dp = At<game::Vec3>(npc, off::kRefPosition), vp0 = At<game::Vec3>(vehicle, off::kRefPosition);
					game::Vec3 dm{}, vm{};const bool dmOk = ScenePos(npc, dm), vmOk = ScenePos(vehicle, vm);
					char probe[256];SeatProbe(npc, probe, sizeof(probe), vehicle);
					logger::Write("NPC vehicles: [%d] traffic driver %08X knocked out of %08X (vehicle 3D %d, driver 3D %d, aboard %d, sit %d, model distance %.1f m, reference distance %.1f m): seated again -> %d (try %d); driver ref (%.0f, %.0f, %.0f) model (%.0f, %.0f, %.0f)%s, vehicle ref (%.0f, %.0f, %.0f) model (%.0f, %.0f, %.0f)%s, vehicle built %d; %s",
						j.pilot, j.npc, j.vehicle, Has3D(vehicle) ? 1 : 0, Has3D(npc) ? 1 : 0, aboard ? 1 : 0, sitBefore, seatDist, refDist, r, j.reseats,
						dp.x, dp.y, dp.z, dm.x, dm.y, dm.z, dmOk ? "" : " (no model)", vp0.x, vp0.y, vp0.z, vm.x, vm.y, vm.z, vmOk ? "" : " (no model)",
						vehiclediag::Built(j.vehicle) ? 1 : 0, probe);
				}
				if(j.reseats>=5){
					if(vehicleflight::PilotActive(j.pilot))vehicleflight::PilotStop(j.pilot,"traffic seat repair exhausted: land before repair");
					else{
						// In the service, like every other change to a traffic pair: several pairs can reach this together. Id 0 so the end of the job
						// below does not cancel it; that job's seat release queues after it, the order this always ran in.
						const unsigned npcId=j.npc,vehicleId=j.vehicle;const int pilot=j.pilot;
						const bool taken=lifecycle::Post("traffic-exit:"+std::to_string(npcId),0,[=]()->lifecycle::Work {
							const auto a=lifecycle::Keep(npc,npcId),v=lifecycle::Keep(vehicle,vehicleId);if(!a||!v)return {};
							return [=]()->std::uint64_t {
								if(!lifecycle::InService())return 0;
								const auto driver=reinterpret_cast<uptr>(a.get()),carrier=reinterpret_cast<uptr>(v.get());
								if(InVehicle(driver)==carrier)InstantExit(driver,true);
								const auto vr=At<game::Vec3>(carrier,off::kRefPosition);const double yaw=At<game::Vec3>(carrier,off::kRefRotation).z;
								const bool placed=WarpExitController(driver,float(vr.x+std::cos(yaw)*3.0),float(vr.y-std::sin(yaw)*3.0),vr.z+0.5f);
								logger::Write("NPC vehicles: [%d] traffic driver %08X would not take its seat in %08X: got out beside it at rest (%s)",pilot,npcId,vehicleId,placed?"placed":"place failed");
								return 1;
							};
						});
						if(!taken)return;  // table full: asked again next update
						Set(j,Phase::Idle);
					}
				}
				return;
			}
			if (aboard && j.reseats && now - j.reseatAt > 5000) j.reseats = 0;  // seated and settled: replenish retries
			// Seat watch (2026-10-30 probe): the same readings for drivers that pass the seat check, every 5 s for their first
			// 60 s, so a carried driver can be compared with one that is left behind.
			if (j.coldStart && j.task == 9 && now - j.since < 60000 && now - j.seatWatchAt >= 5000) {
				j.seatWatchAt = now;
				char probe[256];SeatProbe(npc, probe, sizeof(probe), vehicle);
				const auto vp1 = At<game::Vec3>(vehicle, off::kRefPosition);
				logger::Write("NPC vehicles: [%d] seat watch: driver %08X in %08X sit %d, model distance %.1f m, vehicle at (%.0f, %.0f, %.0f), vehicle built %d; %s; seat holds %u (offset %s)",
					j.pilot, j.npc, j.vehicle, SitState(npc), SeatDist(npc, vehicle), vp1.x, vp1.y, vp1.z, vehiclediag::Built(j.vehicle) ? 1 : 0, probe, j.seatHolds, j.haveSeatOff ? "learned" : "not learned");
			}
			if (!aboard && j.flyOnSince && j.coldStart) return;  // no model, link dropped: kept on its vehicle (above) until it loads
			if (!aboard) {
				logger::Write("NPC vehicles: [%d] NPC %08X left the vehicle (task)", j.pilot, j.npc);
				vehicleflight::PilotStop(j.pilot, "driver got out");
				Set(j, Phase::Idle);
				return;
			}
			if (player && (InCombat(npc) || (InCombat(player) && Dist(vehicle, player) < 120.0)) && HostileTo(npc, player)) {
				// An enemy driver (a patrol / convoy at a hostile outpost) stays aboard and attacks: the task becomes a pursuit.
				j.task = 0;
				ApplyProfile(j, Profile::Pursuit, player, false);
				logger::Write("NPC vehicles: [%d] enemy driver %08X turns on you (task ended, pursuit)", j.pilot, j.npc);
				return;
			}
			if(NativeEnemyId(npc)) {
                j.task=0;ApplyProfile(j,Profile::Pursuit,player,false);return;
            }
            if (InCombat(npc) || (player && InCombat(player) && Dist(vehicle, player) < 120.0)) {
                // A fight: the task ends; the NPC gets out (the fight rules may put it back in a vehicle with a combat profile).
				vehicleflight::PilotStop(j.pilot, "a fight");
				logger::Write("NPC vehicles: [%d] task ended by a fight; NPC %08X gets out", j.pilot, j.npc);
				j.task = 0;
				Set(j, Phase::Settling);
				return;
			}
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			if (j.task == 4 && j.repeatTraffic && (!g_trafficCycle || !g_trafficCycle(j.trip.user, 0, vp.x, vp.y, vp.z))) {
				j.repeatTraffic = false;
				logger::Write("NPC vehicles: [%d] traffic cycle unavailable; parking normally", j.pilot);
			}
			// Flee (2026-10-20): a hostile vehicle (a pursuer) within kFleeNear -> drive kFleeDist away from it, then park.
			const bool enemyDriver = player && HostileTo(npc, player);  // an enemy patrol / convoy: never flees from its own side, offers no rides
			// 2026-10-28 (03:24 run): a ring trip (task 9) fled from the other ring vehicle, became a "transit" that parked 310 m
			// up; an addon's scripted trip never turns into a flight.
			if (j.task != 4 && j.task != 9 && j.taskStep >= 0 && !enemyDriver) {
				for (const auto& o : g_jobs) {
					if (o.phase != Phase::Following || o.profile != Profile::Pursuit || o.task) continue;
					const uptr ov = RefById(o.vehicle);
					if (!ov || Dist(ov, vehicle) > kFleeNear) continue;
					const auto op = At<game::Vec3>(ov, off::kRefPosition);
					const double ex = vp.x - op.x, ey = vp.y - op.y, ed = std::max(0.1, std::hypot(ex, ey));
					j.task = 1;
					j.taskStep = -1;  // fleeing: no second flight, no ride offers
					j.taskX = vp.x + ex / ed * kFleeDist;
					j.taskY = vp.y + ey / ed * kFleeDist;
					logger::Write("NPC vehicles: [%d] civil driver flees from hostile vehicle %08X (%.0f m)", j.pilot, o.vehicle, Dist(ov, vehicle));
					break;
				}
			}
			// Offer a ride (2026-10-20): a transit passing the player on foot within kOfferNear stops for kOfferMs once (tap it to ride along).
			if (j.task == 1 && j.taskStep == 0 && player && !enemyDriver && !InVehicle(player) && Dist(vehicle, player) < kOfferNear) {
				j.taskStep = 1;  // offered
				j.task = 6;
				j.taskAt = now;
				vehicleflight::PilotStop(j.pilot, "stopping for you");
				logger::Write("A driver stops for you - tap the vehicle to ride along");
				logger::Write("NPC vehicles: [%d] transit stops for you (%.0f m)", j.pilot, Dist(vehicle, player));
				return;
			}
			if (j.task == 6) {  // waiting for the player: they ride along (passenger) or it drives on
				if (now - j.taskAt < kOfferMs && passenger::RidingVehicle() != j.vehicle) return;
				j.task = 1;
			}
			vcontract::Goal g;
			g.nav.speedLimit = 22.0f;
			g.nav.priority = 10;
			g.nav.travel = travelmode::Default::Drive;  // civil trips drive; terrain, water or a tall obstacle make them hover
			if (j.task == 8) {  // convoy: behind the lead vehicle; the lead's job over -> park
				const uptr lead = RefById(j.lead);
				if (!lead || !Busy(j.lead)) { BeginPark(j, player, vp.x, vp.y, "convoy: the lead is done"); return; }
				const auto lp = At<game::Vec3>(lead, off::kRefPosition);
				const double h = At<game::Vec3>(lead, off::kRefRotation).z;
				g.x = lp.x - std::sin(h) * 12.0;
				g.y = lp.y - std::cos(h) * 12.0;
				g.stop = 3.0;
				g.nav.speedLimit = 18.0f;
			}
			else if (j.task == 5) {  // supply run: out to the other place
				g.x = j.taskX; g.y = j.taskY; g.stop = 6.0;g.hold=true;
				if (vehicleflight::AtDestination(vehicle,g.x,g.y,g.z,g.stop) && environment::BoardingSite(vehicle).verdict==environment::Verdict::Safe) {
					j.task = 7;
					j.taskAt = now;
					vehicleflight::PilotStop(j.pilot, "supply run: unloading");
					logger::Write("NPC vehicles: [%d] supply run: arrived; waiting %.0f s", j.pilot, kSupplyWaitMs / 1000.0);
					return;
				}
			}
			else if (j.task == 7) {  // supply run: waiting at the far end
				if (now - j.taskAt < kSupplyWaitMs) return;
				j.task = 3;
				logger::Write("NPC vehicles: [%d] supply run: going home", j.pilot);
				return;
			}
			if (j.task == 1) {  // transit
				g.x = j.taskX; g.y = j.taskY; g.stop = 6.0;
				if (std::hypot(vp.x - j.taskX, vp.y - j.taskY) < kTransitArrive) { BeginPark(j, player, j.taskX, j.taskY, "transit: arrived"); return; }
				// In settlements people walk (2026-10-21, the user): a trip that reaches another settlement parks at its edge and
				// the driver walks the rest.
				if (const uptr place = realvehicles::PlaceOwner(vehicle); place && At<std::uint32_t>(place, off::kFormId) != j.startPlace) {
					BeginPark(j, player, vp.x, vp.y, "transit: reached a settlement; parks and walks the rest");
					return;
				}
			}
			else if (j.task == 2) {  // patrol: corners round home
				static const double kCorner[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
				const int k = j.taskStep & 3;
				g.x = j.homeX + kCorner[k][0] * kPatrolR;
				g.y = j.homeY + kCorner[k][1] * kPatrolR;
				g.stop = 5.0;
				g.nav.passThrough = true;
				g.nav.speedLimit = 15.0f;
				if (std::hypot(vp.x - g.x, vp.y - g.y) < kTaskReach) {
					++j.taskStep;
					if (j.taskStep >= kPatrolCorners) {
						j.task = 3;
						logger::Write("NPC vehicles: [%d] patrol done; going home", j.pilot);
					}
				}
			}
			else if (j.task == 3) {  // return home
				g.x = j.homeX; g.y = j.homeY; g.stop = 6.0;
				if (std::hypot(vp.x - j.homeX, vp.y - j.homeY) < kHomeArrive) { BeginPark(j, player, j.homeX, j.homeY, "home"); return; }
			}
			else if (j.task == 4) {  // parking: to the clear spot, land, get out
				double parkX=j.taskX,parkY=j.taskY;
                if(!ParkSpot(j.vehicle,player,j.taskX,j.taskY,parkX,parkY)) {
					j.trafficDwell.Step(now, false);
                    // 2026-10-28 (04:08 run, the user: "if arrived why not slowly descend?"): a route trip arrives ~330 m over its
                    // place, too high for the spot checks to see the ground, and this held it there for good. Until a spot is
                    // confirmed it now sinks slowly over the place (the terrain clearance still floors the goal).
                    g.x=j.taskX;g.y=j.taskY;g.z=vp.z-20.0;g.stop=1.5;g.hold=true;g.nav.speedLimit=8.0f;
                    vehicleflight::PilotFly(j.pilot,vehicle,g);return;
                }
                j.taskX=parkX;j.taskY=parkY;
                const auto site=environment::CheckSite(j.vehicle,parkX,parkY,vp.z,At<game::Vec3>(vehicle,off::kRefRotation).z);
                g.x=parkX;g.y=parkY;g.z=site.ground+2.5;g.stop=1.5;g.hold=true;
				g.nav.landOnArrival = true;
				g.nav.speedLimit = 8.0f;
				if (vehicleflight::AtDestination(vehicle,g.x,g.y,g.z,g.stop) && environment::BoardingSite(vehicle).verdict==environment::Verdict::Safe) {
					vehicleflight::PilotStop(j.pilot, "parked");
					if (j.repeatTraffic) {
						if (!j.trafficDwell.waiting) logger::Write("NPC vehicles: [%d] traffic landed; driver stays aboard, next full lap in 10 s", j.pilot);
						if (j.trafficDwell.Step(now, true) && g_trafficCycle(j.trip.user, 1, vp.x, vp.y, vp.z)) {
							j.task = 9; j.taskAt = j.since = now; j.repeatTraffic = false; j.trafficDwell = {};
							j.homeX = vp.x; j.homeY = vp.y;
							logger::Write("NPC vehicles: [%d] traffic taking off for another full lap", j.pilot);
						}
						return;
					}
					const int out = UnloadVehicle(vehicle, vcontract::ExitRequest{}, false);  // passengers (a shared ride) get out too
					if (out) logger::Write("NPC vehicles: [%d] %d passenger(s) get out", j.pilot, out);
					logger::Write("NPC vehicles: [%d] parked; NPC %08X gets out%s", j.pilot, j.npc, j.taskStep < 0 ? "" : "");
					j.task = 0;
					Set(j, Phase::Settling);
					return;
				}
				j.trafficDwell.Step(now, false);
			}
			else if (j.task == 9) {  // an addon's scripted trip (fvo_addon_api.h): the addon gives the goal each frame
				FvoTripGoal tg{};
				const int r = j.trip.step ? j.trip.step(j.trip.user, vp.x, vp.y, vp.z, &tg) : 1;
#ifndef FVO_RELEASE
                if (j.factoryLease && r != 0) {
                    EndJobOf(j.vehicle, "factory private route ended");
                    return;
                }
#endif
				if (trafficcycle::Destination(r)) {
					BeginPark(j, player, tg.x, tg.y, j.trip.name ? j.trip.name : "scripted trip: at its destination");
					j.repeatTraffic = trafficcycle::Repeat(r) && g_trafficCycle; return;
				}
				if (r != 0) {
					BeginPark(j, player, j.homeX, j.homeY, j.trip.name ? j.trip.name : "scripted trip: done");
					j.repeatTraffic = trafficcycle::Repeat(r) && g_trafficCycle;
					return;
				}
				g.x = tg.x; g.y = tg.y; g.z = tg.z; g.stop = tg.stop > 0 ? tg.stop : 6.0;
				g.nav.passThrough = tg.passThrough != 0;
#ifndef FVO_RELEASE
                if (j.factoryStock) {
                    routeguidance::Clear(j.pilot);
                    g.nav.travel = travelmode::Default::Hover;  // lab stock waits and is spot-tested in FLIGHT (stockfloat.h); the route's own travel rule applies after transfer
                    g.hold = true;
                } else
#endif
                vehicleflight::RouteNav(j.pilot,j.trip.user,tg,g.nav);
#ifndef FVO_RELEASE
                if (j.factoryLease && !j.factoryStock) {
                    const auto* lane = routeguidance::Get(j.pilot, GetTickCount64());
                    if (!lane || lane->travel != 1 || !lane->held) {
                        EndJobOf(j.vehicle, "factory route no longer supplies a held drive lane");
                        return;
                    }
                }
#endif
				if (tg.speed > 0) g.nav.speedLimit = tg.speed;
			}
			if (j.task != 4 &&
#ifndef FVO_RELEASE
                !j.factoryLease &&
#endif
                now - j.since > (j.task == 9 ? kTaskMaxMs * 3 : kTaskMaxMs)) {
				BeginPark(j, player, vp.x, vp.y, "task took too long");
				return;
			}
			vehicleflight::PilotFly(j.pilot, vehicle, g);
		}

		// A crew job's marker goal: the player's map marker, held over (stop) or circled (orbit); patrol: the scanner moves the marker along the chain.
		bool MarkerGoal(Job& j, uptr vehicle, vcontract::Goal& g)
		{
			double mx = 0, my = 0;
			if (!vehicleflight::MarkerPoint(vehicle, mx, my, &g.z)) return false;
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double left = std::hypot(mx - vp.x, my - vp.y);
			g.x = mx;
			g.y = my;
			g.stop = 6.0;
			g.hold = true;
			if (vehicleflight::GetAutoMode() == vehicleflight::AutoMode::Orbit && left < 45.0) {
				const double ang = std::atan2(vp.x - mx, vp.y - my) + 0.45;
				g.x = mx + std::sin(ang) * 25.0;
				g.y = my + std::cos(ang) * 25.0;
				g.stop = 1.0;
				g.hold = false;
			}
			(void)j;
			return true;
		}
		int DefendGoal(Job& j, uptr aimRef, bool inVehicle, uptr player, uptr vehicle, ULONGLONG now, vcontract::Goal& g)
		{
			const auto fp = At<game::Vec3>(aimRef, off::kRefPosition);
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (j.foeSeenAt && now > j.foeSeenAt && now - j.foeSeenAt < 1000) {
				const double dt = (now - j.foeSeenAt) / 1000.0;
				const double ix = (fp.x - j.foeX) / dt, iy = (fp.y - j.foeY) / dt;
				if (std::hypot(ix, iy) < 150.0) {
					const double a = std::min(1.0, dt * 3.0);
					j.foeVx += (ix - j.foeVx) * a;
					j.foeVy += (iy - j.foeVy) * a;
				}
			}
			else {
				j.foeVx = j.foeVy = 0;
			}
			j.foeX = fp.x; j.foeY = fp.y; j.foeSeenAt = now;
			const double dx = double(pp.x) - fp.x, dy = double(pp.y) - fp.y, d = std::hypot(dx, dy);
			const double closing = d > 0.1 ? (j.foeVx * dx + j.foeVy * dy) / d : 0.0;
			// Roles and threat (2026-10-20): with several escorts in the fight the second one always shields the player; with many enemies around
			// (g_ec.threat, kThreatHigh) every escort stays defensive (shield or intercept only).
			int role = 0, escorts = 0;
			for (const auto& o : g_jobs) {
				if (o.phase != Phase::Following || o.profile != Profile::Escort || !o.foe) continue;
				if (o.pilot < j.pilot) ++role;
				++escorts;
			}
			const bool defensive = g_ec.threat >= kThreatHigh || (escorts >= 2 && role == 1);
			j.defDefensive = defensive;
			int mode = 0;
			if (inVehicle && d > kInterceptMin && closing > 3.0) {
				const double px = fp.x + j.foeVx * kInterceptLead, py = fp.y + j.foeVy * kInterceptLead;
				g.x = pp.x + (px - pp.x) * kInterceptShare;
				g.y = pp.y + (py - pp.y) * kInterceptShare;
				g.z = pp.z + 5.0;
				g.stop = 2.0;
				g.hold = false;
				mode = 2;
			}
			else if (!inVehicle && !defensive) {
				// Overwatch (an enemy on foot): high behind the player, away from the enemy; the nose is put on it from there (the combat branch).
				g.x = d > 0.1 ? pp.x + dx / d * kOverwatchBack : pp.x;
				g.y = d > 0.1 ? pp.y + dy / d * kOverwatchBack : pp.y;
				g.z = pp.z + kOverwatchUp;
				g.stop = 2.0;
				g.hold = false;
				mode = 3;
			}
			else if (defensive || Dist3(vehicle, aimRef) > vehiclehits::BallisticsOf(j.vehicle).range) {
				const double ux = d > 0.1 ? -dx / d : 0.0, uy = d > 0.1 ? -dy / d : 1.0;  // from the player toward the target
				g.x = pp.x + ux * kShieldDist;
				g.y = pp.y + uy * kShieldDist;
				g.z = pp.z + 4.0;
				g.stop = 2.0;
				g.hold = false;
				mode = 1;
			}
			else {
				OrbitGoal(j, aimRef, vehicle, g);
			}
			if (mode != j.defMode) {
				logger::Write("NPC vehicles: [%d] escort in the fight: %s %08X (%.0f m from you%s)", j.pilot, mode == 3 ? "overwatch over you against" : mode == 2 ? "intercepting" : mode == 1 ? "shielding you from" : "attacking",
					j.foe, d, closing > 3.0 ? ", closing on you" : "");
				j.defMode = mode;
			}
			return mode;
		}

		// ---- Exit (2026-10-18). The game's dismount FUN_141952530(actor) mirrors the mount: it refuses (returns 0) when actor +0x378 bit 1 is set, when the
		// actor has no seat interaction (extra 0xA5), when bit 3 ("getting on / off") is set, and at process level above 1 it does the instant exit
		// FUN_141A9C530 and still returns 0. Otherwise it pushes a runtime package of type 0x2A (dismount) which the AI may drop like the mount one.
		// Past runs: "dismount (parked) -> 0" at 20:01 and 21:03 left the NPC aboard. So an exit is: ask the game's dismount (animated), watch it, and fall
		// back to the instant exit; afterwards make sure the NPC does not stand inside the vehicle.
		constexpr ULONGLONG kExitWatchMs = 6000;  // the animated exit gets this long before the instant exit is used

		// The game's own get-up out of a seat (actor vtable +0xB30): what FUN_141951FB0 does for a seated actor and what the
		// kick-out (TakeSeat) uses. Unlike the instant exit and the forced furniture state 0, the NPC plays its stand-up and
		// ends on foot (2026-10-28 21:24 run: a shot-down driver ejected with the instant exit stayed in the sitting pose,
		// "animation reset not taken"; another stayed aboard when the instant exit was refused).
		bool StandUp(uptr npc)
		{
			__try {
				reinterpret_cast<void (*)(uptr)>((*reinterpret_cast<void***>(npc))[0xB30 / 8])(npc);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool InstantExit(uptr npc, bool eject = false)
		{
            const auto vehicle=InVehicle(npc);
            if(!eject&&vehicle&&environment::BoardingSite(vehicle).verdict!=environment::Verdict::Safe)return false;
			g_allowDismount = true;
			const auto freeze=At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit);
			__try {
				__try {
					if(eject)At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit)=0;
					EngineFn<void (*)(uptr)>(0x01A9C530)(npc);
				} __finally { if(eject)At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit)=freeze; }
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
            if(vehicle && npc && InVehicle(npc)==vehicle) {
                const auto site=environment::BoardingSite(vehicle);
                if(site.verdict!=environment::Verdict::Safe) {
                    if(!j.exitWaitSince)j.exitWaitSince=GetTickCount64();
                    if(GetTickCount64()-j.exitWaitSince>=8000){
                        vehicleflight::PilotStop(j.pilot,"no safe exit site; native AI resumes");
                        vcontract::Unassign(j.vehicle);Set(j,Phase::Idle);return;
                    }
                    const auto p=At<game::Vec3>(vehicle,off::kRefPosition);
                    vcontract::Goal hold;hold.x=p.x;hold.y=p.y;hold.stop=1.5;hold.hold=true;
                    hold.z=site.supported?site.ground+2.5:p.z;
                    if(vehicleflight::PilotFly(j.pilot,vehicle,hold))return;
                    // 2026-10-28: the pilot refused (no 3D): hold impossible, take the ordinary exit below.
                }
            }
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
			// Refused: the game's get-up first (the kick-out's way out); Leaving watches it and falls back to the instant exit.
			if (StandUp(npc)) {
				logger::Write("NPC vehicles: [%d] exit: dismount refused; asked the game's get-up (actor vtable +0xB30); the instant exit after %.0f s if still seated",
					j.pilot, kExitWatchMs / 1000.0);
				Set(j, Phase::Leaving);
				return;
			}
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
			bool landed = false;   // Eject with waitLanded: the vehicle came to rest (or the wait ran out)
			bool rested = false;   // ... and it really came to rest
			bool standAsked = false, standLogged = false; ULONGLONG standAt = 0;  // the game's get-up was asked (wreck at rest)
		};
		constexpr ULONGLONG kEjectLandMs = 6000;
		constexpr int kExitTasks = 12;
		ExitTask g_exitTasks[kExitTasks];
		// After an eject: did the actor fall? One that hangs where it was placed (no drop in 1.5 s, still out of the
		// vehicle) is put on the ground beside the vehicle once the vehicle's boarding site is safe (it has landed),
		// retried every second up to kHangTries times.
		struct FallWatch { std::uint32_t npc=0, veh=0; float z=0; ULONGLONG at=0; int tries=0; } g_fallWatch[kExitTasks];
		constexpr int kHangTries = 10;
		std::uint32_t g_exitRefused[8] = {};  // actors whose last exit was Refused (polled by ExitStateOf)

		// The native furniture-state cleanup alone (state 0: controller and graph restored to on-foot), no position change.
		bool ClearSeatState(uptr actor)
		{
			__try {
				constexpr unsigned char sig[]{0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x44,0x88,0x48,0x20,0x55,0x56,0x57,0x41,0x54};
				if(std::memcmp(reinterpret_cast<void*>(game::Base()+addr::kActorFurnitureState),sig,sizeof(sig)))return false;
				const auto process = At<uptr>(actor, 0x228);
				const auto high = process ? At<uptr>(process, 8) : 0;
				if (!high || !At<uptr>(high, 0x458)) return false;
				const auto state=actor+0xF0;
				const int before=reinterpret_cast<int(*)(uptr)>((*reinterpret_cast<void***>(state))[0x1A0/8])(state);
				const auto freeze=At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit);
				__try {
					At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit)=0;
					EngineFn<void(*)(uptr,uptr,int,char)>(addr::kActorFurnitureState)(process,actor,0,1);
				} __finally {At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit)=freeze;}
				const int after=reinterpret_cast<int(*)(uptr)>((*reinterpret_cast<void***>(state))[0x1A0/8])(state);
				logger::Write("NPC vehicles: seat state cleanup %08X: furniture state %d -> %d",At<std::uint32_t>(actor,off::kFormId),before,after);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		// One animation-graph event to an actor (as Papyrus PlayAnimation sends it, FUN_14207C1E0: the graph manager holder at
		// +0x60, vtable +0x8 (holder, const BSFixedString& event, 0) -> processed). Main thread.
		bool AnimEvent(uptr actor, const char* name)
		{
			alignas(8) std::uint8_t str[16] = {};
			bool made = false, ok = false;
			__try {
				EngineFn<void* (*)(void*, const char*, bool)>(addr::kFixedStringCreate)(str, name, true);
				made = true;
				const uptr holder = actor + 0x60;
				ok = reinterpret_cast<bool (*)(uptr, void*, int)>((*reinterpret_cast<void***>(holder))[1])(holder, str, 0);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
			if (made) { __try { EngineFn<void (*)(void*)>(addr::kFixedStringRelease)(str); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
			return ok;
		}
		// 2026-10-28 (the user: a pulled-out NPC "still sit in air"; 10:45 run: seat state 3 -> 0 at ground height, but the sitting
		// animation kept playing): the instant exit and the furniture state do not reset the animation graph. The standard
		// reset events, first one the graph takes.
		const char* ResetAnimation(uptr actor)
		{
			for (const char* e : { "IdleForceDefaultState", "IdleStop", "IdleStopInstant" })
				if (AnimEvent(actor, e)) return e;
			return nullptr;
		}

		// Asks the native furniture state machine (FUN_141A03F70) for `state`; returns the state before (-1: refused). States
		// (Bethesda's sit/sleep numbering): 0 normal, 1 want to sit, 2 waiting for the sit animation, 3 sitting, 4 want to stand.
		int RequestFurnitureState(uptr actor, int state)
		{
			__try {
				constexpr unsigned char sig[]{0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x44,0x88,0x48,0x20,0x55,0x56,0x57,0x41,0x54};
				if(std::memcmp(reinterpret_cast<void*>(game::Base()+addr::kActorFurnitureState),sig,sizeof(sig)))return -1;
				const auto process = At<uptr>(actor, 0x228);
				const auto high = process ? At<uptr>(process, 8) : 0;
				if (!high) return -1;
				const auto st = actor + 0xF0;
				const int before = reinterpret_cast<int(*)(uptr)>((*reinterpret_cast<void***>(st))[0x1A0/8])(st);
				EngineFn<void(*)(uptr,uptr,int,char)>(addr::kActorFurnitureState)(process, actor, state, 0);
				return before;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
		}

		bool WarpExitController(uptr npc, float x, float y, float z)
		{
			__try {
				const auto process = At<uptr>(npc, 0x228);
				const auto high = process ? At<uptr>(process, 8) : 0;
				if (!high || !At<uptr>(high, 0x458)) return false;
				constexpr unsigned char sig[]{0x48,0x8B,0xC4,0x48,0x89,0x58,0x10,0x44,0x88,0x48,0x20,0x55,0x56,0x57,0x41,0x54};
				if(std::memcmp(reinterpret_cast<void*>(game::Base()+addr::kActorFurnitureState),sig,sizeof(sig)))return false;
				const auto state=npc+0xF0;
				const int before=reinterpret_cast<int(*)(uptr)>((*reinterpret_cast<void***>(state))[0x1A0/8])(state);
				const auto freeze=At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit);
				__try {
					At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit)=0;
					// Native furniture state 0 performs controller and graph cleanup,
					// including restoring normal movement; clearing extra A5 alone does not.
					EngineFn<void(*)(uptr,uptr,int,char)>(addr::kActorFurnitureState)(process,npc,0,1);
				} __finally {At<std::uint8_t>(game::Base(),addr::kFreezeVehicleOnExit)=freeze;}
				const int after=reinterpret_cast<int(*)(uptr)>((*reinterpret_cast<void***>(state))[0x1A0/8])(state);
				const char* reset = ResetAnimation(npc);
				logger::Write("NPC vehicles: eject cleanup %08X: furniture state %d -> %d; animation reset %s",At<std::uint32_t>(npc,off::kFormId),before,after,
					reset ? reset : "not taken (no event accepted)");
				// A spot away from where the actor is was a bad spot (21:19: the player put 161 m up and flung 27 km): cleaned up, not moved.
				const auto here=At<game::Vec3>(npc,off::kRefPosition);
				const double away=std::hypot(std::hypot(double(x)-here.x,double(y)-here.y),double(z)-here.z);
				if(!std::isfinite(away)||away>8.0){logger::Write("NPC vehicles: exit warp for %08X refused: the spot is %.1f m away (cleaned up in place)",At<std::uint32_t>(npc,off::kFormId),away);return true;}
				alignas(16) float point[4]{x,y,z,0};
				// Native warp clears the controller's support/velocity and synchronizes
				// its physics proxy. A generic MoveReference leaves stale seat support.
				EngineFn<void(*)(uptr,float*,char)>(addr::kActorSetPosition)(npc, point, 1);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

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
				const bool ok = req.mode == vcontract::ExitMode::Eject ? WarpExitController(npc, req.x, req.y, z) :
					PlaceAt(npc, cell, req.x, req.y, z, At<game::Vec3>(npc, off::kRefRotation).z);
				if (ok && req.mode == vcontract::ExitMode::Eject) for (auto& watch : g_fallWatch) if (!watch.npc) {
					watch = {At<std::uint32_t>(npc, off::kFormId), At<std::uint32_t>(vehicle, off::kFormId), z, GetTickCount64(), 0}; break;
				}
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
				if (j.phase == Phase::Idle || j.npc != t.npc || j.vehicle!=t.veh) continue;
				{
					vehicleflight::PilotStop(j.pilot,out?"driver exited":"exit refused; native AI resumes");
					vcontract::Unassign(j.vehicle);  // the driver left: the vehicle is parked, no profile drives it
					// Keep the timetable's slot leased until its sliced update
					// observes this exit; another service must not inherit it yet.
					if (!j.scheduledTraffic) Set(j, Phase::Idle);
				}
			}
			logger::Write("NPC vehicles: exit task for NPC %08X finished after %.1f s: %s", t.npc, (GetTickCount64() - t.t0) / 1000.0, out ? "OUT" : "REFUSED (still aboard)");
			t.used = false;
		}

		void ExitTick()
		{
			const ULONGLONG now = GetTickCount64();
			for (auto& watch : g_fallWatch) if (watch.npc && now - watch.at >= (watch.tries ? 1000u : 1500u)) {
				const auto actor = RefById(watch.npc);
				if (!actor || InVehicle(actor)) { watch = {}; continue; }
				const float z = At<game::Vec3>(actor, off::kRefPosition).z;
				if (!watch.tries) logger::Write("NPC vehicles: air eject fall check %08X: height %.2f -> %.2f after %.2f s", watch.npc, watch.z, z, (now-watch.at)/1000.0);
				if (watch.z - z >= 0.3f) { watch = {}; continue; }  // it falls (or stands where it was put): fine
				const uptr vehicle = RefById(watch.veh);
				const bool placed = vehicle && PlaceBeside(actor, vehicle, 0, 3.0);
				if (placed || ++watch.tries >= kHangTries) {
					logger::Write("NPC vehicles: ejected NPC %08X hung at height %.2f: %s", watch.npc, z,
						placed ? "put on the ground beside the vehicle" : "the vehicle never offered a safe spot; left where it is");
					watch = {};
					continue;
				}
				watch.at = now;
			}
			for (auto& t : g_exitTasks) {
				if (!t.used) continue;
				const uptr npc = RefById(t.npc), vehicle = RefById(t.veh);
				if (!npc || !vehicle) {
					t.used = false;
					continue;
				}
				if (InVehicle(npc) != vehicle) {
					if(InVehicle(npc)){t.used=false;continue;} // a different seat must not be moved back to the old vehicle
					// Allow native interaction cleanup to complete before the one-shot
					// controller warp. Repeated warps would erase the falling velocity.
					if (t.req.mode == vcontract::ExitMode::Eject && t.stage == 5) {
						if (now - t.tStage < 100) continue;
						const auto process = At<uptr>(npc, 0x228), high = process ? At<uptr>(process, 8) : 0;
						if ((!high || !At<uptr>(high, 0x458)) && now - t.tStage < 2000) continue;
					}
					FinishExit(t, npc, vehicle, true);
					continue;
				}
				if (t.req.mode == vcontract::ExitMode::Eject) {
					// A wreck falls (hover off, native gravity): its occupants ride it down and get out once it rests
					// (2026-10-21 23:24 / 23:25 runs: NPCs ejected 18-37 m up hung where they were placed; the
					// game's exit is only made for a vehicle on the ground). waitLanded = at rest for 0.5 s, at most
					// kEjectLandMs; then the eject goes ahead wherever the vehicle is.
					if (t.req.waitLanded && !t.landed) {
						const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
						const double moved = std::sqrt(std::pow(double(vp.x) - t.lx, 2) + std::pow(double(vp.y) - t.ly, 2) + std::pow(double(vp.z) - t.lz, 2));
						t.lx = vp.x; t.ly = vp.y; t.lz = vp.z;
						if (!t.stillSince || moved > 0.08) t.stillSince = now;
						const bool rest = now - t.stillSince >= 500;
						if (!rest && now - t.t0 < kEjectLandMs) continue;
						t.landed = true;
						t.rested = rest;
						logger::Write("NPC vehicles: eject NPC %08X: vehicle %08X %s after %.1f s (height %.1f)", t.npc, t.veh,
							rest ? "came to rest" : "still moving, ejecting anyway", (now - t.t0) / 1000.0, vp.z);
					}
					// At rest: the game's get-up (the kick-out's way), 3 s to play; the NPC being out ends the task above.
					// Still aboard after that (or the vehicle never rested): the instant eject below.
					if (t.rested && !t.standAsked) {
						t.standAsked = true; t.standAt = now;
						if (StandUp(npc)) {
							logger::Write("NPC vehicles: eject NPC %08X: asked the game's get-up (actor vtable +0xB30); the instant eject in 3 s if still seated", t.npc);
							continue;
						}
					}
					if (t.standAsked && now - t.standAt < 3000) continue;
					if (t.standAsked && !t.standLogged && (t.standLogged = true)) logger::Write("NPC vehicles: eject NPC %08X: still seated 3 s after the get-up; instant eject", t.npc);
					static thread_local world::Snapshot ejectWorld;
					const world::VehicleInfo* hull = nullptr;
					if (world::Vehicles(ejectWorld)) for (unsigned k = 0; k < ejectWorld.count && k < world::kMaxVehicles; ++k)
						if (ejectWorld.v[k].formId == t.veh) { hull = &ejectWorld.v[k]; break; }
					double point[3]{};
					bool ready = false;
					if (hull) {
						const auto player = Ptr(addr::kPlayerCharacterPtr);
						const auto pp = player ? At<game::Vec3>(player, off::kRefPosition) : game::Vec3{};
						const double towardPlayer = (pp.x - hull->center[0]) * std::cos(hull->heading) - (pp.y - hull->center[1]) * std::sin(hull->heading);
						const int preferred = player && towardPlayer > 0 ? -1 : 1;
						for (int side : {preferred, -preferred}) {
							if (!integrationlogic::EjectPosition(HullBox(*hull), hull->heading, side, point)) break;
							const float from[] = {hull->center[0], hull->center[1], hull->center[2] + 1.0f};
							const float to[] = {float(point[0]), float(point[1]), float(point[2]) + 1.0f};
							if (!environment::LineBlocked(from, to)) { ready = true; break; }
						}
					}
					if (!ready) {
						if (now - t.t0 > t.req.timeoutMs) FinishExit(t, npc, vehicle, false);
						continue;
					}
					const bool called = InstantExit(npc, true);
					const bool out = !InVehicle(npc);
					t.req.x = float(point[0]); t.req.y = float(point[1]); t.req.z = float(point[2]);
					logger::Write("NPC vehicles: air eject NPC %08X from %08X at altitude %.1f: native %s, %s", t.npc, t.veh, point[2], called ? "called" : "failed", out ? "OUT" : "still aboard");
					if (out) { t.stage = 5; t.tStage = now; }
					else if (now - t.t0 > t.req.timeoutMs) FinishExit(t, npc, vehicle, false);
					continue;
				}
                const auto site=environment::BoardingSite(vehicle);
                if(site.verdict!=environment::Verdict::Safe) {
                    if(now-t.t0>t.req.timeoutMs){logger::Write("Exit refused: %s",site.reason);FinishExit(t,npc,vehicle,false);}
                    continue;
                }
                t.req.x=float(site.x);t.req.y=float(site.y);t.req.z=float(site.z);
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
			if(t)return true; // polling/repeated AI intent must not restart the deadline
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
			t->req.timeoutMs=std::clamp(req.timeoutMs,1000u,30000u);
			t->t0 = t->tStage = GetTickCount64();
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			t->lx = vp.x; t->ly = vp.y; t->lz = vp.z;
			t->stage = req.mode == vcontract::ExitMode::Instant || req.mode == vcontract::ExitMode::Eject ? 4 : req.mode == vcontract::ExitMode::Quick ? 3 : (req.waitLanded ? 1 : 2);
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
				if (nn && InVehicle(nn)) {
					vcontract::ExitRequest req;
					if (environment::BoardingSite(InVehicle(nn)).verdict != environment::Verdict::Safe) {
						req.mode = vcontract::ExitMode::Eject; req.waitLanded = false;
					}
					n += StartExit(nn, req) ? 1 : 0;
				}
				else if (nn) Set(jj, Phase::Idle);
			}
			logger::Write(n ? "Exit requested for %d NPC(s)" : "No NPC aboard a job vehicle", n);
		}

		void Update(Job& j, uptr player)
		{
			if (j.scheduledTraffic) return; // timetable owns boarding, movement, unload and recovery
			const ULONGLONG now = GetTickCount64();
			if (ExitTaskFor(j.npc)) return;  // an exit request owns this NPC until it finishes
			const uptr npc = RefById(j.npc), vehicle = RefById(j.vehicle);
			// Route traffic stays on native physics at every distance. The former virtual
			// cutoff was derived from a visibility heuristic, not measured physics coverage.
			if (!npc || !vehicle) {
				logger::Write("NPC vehicles: [%d] %s lost", j.pilot, !npc ? "NPC" : "vehicle");
				vehicleflight::PilotStop(j.pilot, !npc ? "driver lost" : "vehicle lost");  // a driver lost leaves the vehicle to descend first
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
			const double modelsToPlayer = Dist(vehicle, player);
			// With no model the driver's seat link can drop too (2026-10-31 09:04 run, FF01F027: that read as "got out", the job ended 322 m up and the driver was left hanging). Not aboard counts here:
			// nobody can leave a seat with no model, so it stays on the vehicle and the repair seats it when it loads.
			if(j.coldStart&&trafficpair::FlyWithoutDriverModel(j.phase==Phase::Following,Has3D(vehicle),Has3D(npc),true,modelsToPlayer)){
				// Out of the loaded cells: only data until it comes back. Kept on its vehicle (see below), so it loads in its seat.
				const double drift=Dist3(npc,vehicle);
				if(drift>2.0&&std::isfinite(drift)&&PinToVehicle(npc,vehicle))++j.flyOnPins;
				if(!j.flyOnSince){j.flyOnSince=now;j.flyOnPins=0;j.no3DSince=0;
					logger::Write("NPC vehicles: [%d] driver %08X has no model, out of the loaded cells at %.0f m from the player: vehicle %08X flies on",j.pilot,j.npc,modelsToPlayer,j.vehicle);}
			}else if(j.flyOnSince&&Has3D(npc)){
				char probe[256];SeatProbe(npc,probe,sizeof(probe),vehicle);
				logger::Write("NPC vehicles: [%d] driver %08X has a model again after %.0f s, at %.0f m from the player (%.1f m from the vehicle, kept on it %d time(s)); %s",
					j.pilot,j.npc,(now-j.flyOnSince)/1000.0,modelsToPlayer,Dist3(npc,vehicle),j.flyOnPins,probe);
				j.flyOnSince=0;
			}else if(j.flyOnSince&&modelsToPlayer<trafficpair::kDriverModelReach){
				// Back inside with no model: this is what the rule below is for. Logged, since a direct position write does not change the actor's cell.
				logger::Write("NPC vehicles: [%d] driver %08X still has no model %.0f m from the player after %.0f s out; the near rule takes over",j.pilot,j.npc,modelsToPlayer,(now-j.flyOnSince)/1000.0);
				j.flyOnSince=0;
			}
			if(j.coldStart&&!j.flyOnSince&&(j.phase==Phase::Boarding||j.phase==Phase::Following)&&(!Has3D(vehicle)||!Has3D(npc))){
				// Do not dismount in mid-air or continue invisible driving while a
				// pair's scene unloads. Preserve its lease until both models return.
				vehicleflight::Command hold;hold.set=hold.kThrottle|hold.kStrafe|hold.kBrake|hold.kYaw;hold.brake=true;
				vehicleflight::PilotCommand(j.pilot,hold);
				j.watchAt=now;j.watchTickAt=now;
				if(!j.no3DSince){j.no3DSince=now;logger::Write("NPC vehicles: [%d] traffic pair waiting for vehicle/driver 3D at %.0f m from the player; no airborne exit",j.pilot,modelsToPlayer);}
				// 2026-10-30 (06:24 run): five pre-armed pairs waited 24 s unloaded; their drivers, listed aboard, came back 50-97 m from
				// vehicles that had not moved (the game moves unloaded actors along their AI package without a model) and floated there.
				// While the driver has no model its position is only data: it is kept on its vehicle, so it loads in its seat.
				if(!Has3D(npc)){const double drift=Dist3(npc,vehicle);
					if(drift>2.0&&std::isfinite(drift)&&PinToVehicle(npc,vehicle)){++j.pins;j.pinMax=std::max(j.pinMax,drift);
						if(j.pins==1)logger::Write("NPC vehicles: [%d] unloaded traffic driver %08X had drifted %.1f m from vehicle %08X: kept on its vehicle until both load",j.pilot,j.npc,drift,j.vehicle);}}
				if(trafficpair::LandForMissingModel(Has3D(vehicle),Has3D(npc),now-j.no3DSince,modelsToPlayer))vehicleflight::PilotStop(j.pilot,"traffic driver model missing: land before repair");
				return;
			}
			if(j.coldStart&&j.no3DSince&&Has3D(vehicle)&&Has3D(npc)){j.no3DSince=0;
				logger::Write("NPC vehicles: [%d] traffic pair 3D restored; validating seat (driver %.1f m from the vehicle; kept on it %d time(s) while unloaded, largest drift %.0f m)",
					j.pilot,Dist3(npc,vehicle),j.pins,j.pinMax);j.pins=0;j.pinMax=0;}
			// Both models here and the driver not sitting: hold still so the seat repair can reach. Bounded (trafficpair::SeatBrake).
			if(j.coldStart&&j.phase==Phase::Following){
				const int sit=SitState(npc);const double offSeat=Dist3(npc,vehicle);
				const bool unseated=Has3D(vehicle)&&Has3D(npc)&&!trafficpair::Sitting(sit)&&std::isfinite(offSeat)&&offSeat<=kHoldReach;
				const bool wasBraking=j.seatBrakeSince!=0;
				const auto startedAt=j.seatBrakeSince;
				if(trafficpair::SeatBrake(unseated,j.seatBrakeSince,now)){
					vehicleflight::Command hold;hold.set=hold.kThrottle|hold.kStrafe|hold.kBrake|hold.kYaw;hold.brake=true;
					vehicleflight::PilotCommand(j.pilot,hold);
					j.watchAt=now;j.watchTickAt=now;
					if(!wasBraking)logger::Write("OBS system=driver.seat vehicle=%08X driver=%08X event=seat_brake_start pilot=%d sit=%d off_seat=%.1f player=%.0f",j.vehicle,j.npc,j.pilot,sit,offSeat,modelsToPlayer);
				}
				else if(wasBraking&&!j.seatBrakeSince)logger::Write("OBS system=driver.seat vehicle=%08X driver=%08X event=seat_brake_end seated=%d sit=%d after_ms=%llu",j.vehicle,j.npc,trafficpair::Sitting(sit),sit,static_cast<unsigned long long>(now-startedAt));
				else if(j.seatBrakeSince&&now-j.seatBrakeSince<trafficpair::kSeatBrakeMs+250)logger::Observe("driver.seat",j.vehicle,0xB00u,"driver=%08X event=seat_brake_gave_up sit=%d off_seat=%.1f",j.npc,sit,offSeat);
			}
			const double toPlayer = Dist(vehicle, player);
			// Watchdog (2026-10-21, the user: "an NPC sat in the vehicle and did nothing"; log it instead of asking): an NPC aboard whose
			// vehicle has not moved 2 m in kIdleAboardMs gets one line every kIdleAboardMs with what the job, the pilot and the vehicle say.
			if (aboard && j.phase != Phase::Boarding) {
				const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
				// 2026-10-29 14:38 run (the user: vehicles on the route "just fall"): Start/Swap runs on the local map, which pauses the
				// game; nothing moved, the wall clock ran on, and 90 s later nine trips were "stuck" at once and dropped. A gap of more than
				// a second between these checks (map, menus, a load) is paused time: the watch moves forward by it.
				if (j.watchAt && j.watchTickAt && now - j.watchTickAt > 1000) j.watchAt += now - j.watchTickAt;
				j.watchTickAt = now;
				// A traffic pilot held still far from you (vehicleflight::PilotHeldFar, the 2026-10-29 performance pass) is waiting, not stuck.
				if (!j.watchAt || vehicleflight::PilotHeldFar(j.pilot) || std::hypot(std::hypot(vp.x - j.watchX, vp.y - j.watchY), vp.z - j.watchZ) > 2.0) {
					j.watchX = vp.x; j.watchY = vp.y; j.watchZ = vp.z; j.watchAt = now;
				}
				else if (now - j.watchAt >= kIdleAboardMs && now - j.watchLog >= kIdleAboardMs && !(j.phase == Phase::Following && j.parked)) {
					j.watchLog = now;
					const auto rot = At<game::Vec3>(vehicle, off::kRefRotation);
					const uptr base = At<uptr>(vehicle, 0x98);
					const uptr foe = j.foe ? RefById(j.foe) : 0;
					logger::Write("NPC vehicles: [%d] IDLE ABOARD %.0f s: NPC %08X in %08X (base %08X) at (%.0f, %.0f, %.0f); phase %s, profile %s, foe %08X%s %.0f m, you %.0f m; "
						"pilot %s (last end: %s); pitch %.0f roll %.0f deg", j.pilot, (now - j.watchAt) / 1000.0, j.npc, j.vehicle, base ? At<std::uint32_t>(base, off::kFormId) : 0,
						vp.x, vp.y, vp.z, Name(j.phase), kProfiles[int(j.profile)].name, j.foe, foe && Dead(foe) ? " (dead)" : "", foe ? Dist(vehicle, foe) : -1.0, toPlayer,
						vehicleflight::PilotActive(j.pilot) ? "flying" : "OFF", vehicleflight::PilotEndReason(j.pilot) ? vehicleflight::PilotEndReason(j.pilot) : "-", rot.x * 57.29578, rot.y * 57.29578);
					if (j.task == 9 && j.trip.step &&
#ifndef FVO_RELEASE
                        !j.factoryLease &&
#endif
                        now - j.watchAt >= kTripStuckMs) {
						logger::Write("NPC vehicles: [%d] traffic trip of vehicle %08X stuck %.0f s at (%.0f, %.0f, %.0f): trip ended, driver stays aboard",
							j.pilot, j.vehicle, (now - j.watchAt) / 1000.0, vp.x, vp.y, vp.z);
						vehicleflight::PilotStop(j.pilot, "traffic trip stuck");
						Set(j, Phase::Idle);
						return;
					}
				}
			}
			else j.watchAt = 0;
			if (j.phase == Phase::Following) vehicleflight::WatchFlip(vehicle, "NPC vehicles");
			logger::Observe("npc.job",j.vehicle,unsigned(j.phase)|(std::uint64_t(j.foe)<<8),"driver=%08X pilot=%d phase=%s task=%d enemy=%08X combat_ai=%d retreating=%d passenger=%d",j.npc,j.pilot,Name(j.phase),j.task,j.foe,j.aiOn,j.retreating,j.passenger);
			switch (j.phase) {
			case Phase::Boarding:
                if(j.questPending){
                    if(pilotquest::AssignTraffic(j.pilot-1,npc,vehicle))j.questPending=false;
                    else if(lifecycle::Pending(j.npc,"pilot-assign"))return;
                    else {EndJobOf(j.vehicle,"traffic pilot package assignment failed");return;}
                }
                // Boarding as a combat move: its state decides until the NPC is aboard, or it ends and the get-in package takes over.
                if (j.combatReq) {
                    const int st = CombatBoardState(j.combatReq);
                    const double d = Dist(npc, vehicle);
                    if (st != j.combatReqState) {
                        logger::Write("NPC vehicles: [%d] combat boarding: %s (%.1f s, %.1f m from the vehicle, NPC in combat %d)", j.pilot, CombatBoardStateName(st),
                            (now - j.combatReqAt) / 1000.0, d, InCombat(npc) ? 1 : 0);
                        j.combatReqState = st;
                        if ((st == 2 || st == 4) && !j.combatDoneAt) j.combatDoneAt = now;
                    }
                    // Arrived: the request's own activation does not seat anyone in a vehicle (21:17 run: "done" at 0.4 m, not aboard 8 s
                    // later), and seating mid-combat left the NPC in a weapon-up seat pose. So at the vehicle: calm for this moment (the combat
                    // AI lets go of the body), the game's instant seat, and the enemy is restored once aboard (Set Following -> ResumeCombat).
                    if (!aboard && !j.combatSeatTried && (st == 2 || st == 4 || (st == 1 && d < 2.5)) && d <= 4.0) {
                        j.combatSeatTried = true;
                        Calm(npc);
                        int seated = 0;
                        const int r = InstantSeat(npc, vehicle, seated);
                        const bool in = InVehicle(npc) == vehicle;
                        logger::Write("NPC vehicles: [%d] combat move brought NPC %08X to the vehicle (%.1f m, %.1f s): instant seat -> %d, seat %d, %s", j.pilot, j.npc, d,
                            (now - j.combatReqAt) / 1000.0, r, seated, in ? "aboard (its enemy is restored next)" : "NOT aboard");
                        if (in) { CombatBoardRelease(j.combatReq); return; }
                    }
                    const char* end = aboard ? nullptr :
                        st == 3 ? "the request failed" :
                        st == 0 && now - j.combatReqAt > kCombatStartMs ? "the combat AI never started it" :
                        st == 1 && now - j.combatReqAt > kCombatBoardMs ? "it ran too long" :
                        j.combatDoneAt && now - j.combatDoneAt > kCombatActivatedMs ? "it activated the vehicle but the NPC is not aboard" : nullptr;
                    if (aboard) {
                        logger::Write("NPC vehicles: [%d] NPC %08X aboard by the combat move after %.1f s (still in combat %d)", j.pilot, j.npc,
                            (now - j.combatReqAt) / 1000.0, InCombat(npc) ? 1 : 0);
                        CombatBoardRelease(j.combatReq);
                    }
                    else if (end) {
                        CombatBoardRelease(j.combatReq);
                        const bool pkg = pilotquest::Assign(j.pilot - 1, npc, vehicle);
                        logger::Write("NPC vehicles: [%d] combat boarding ended (%s, %.1f s, %.1f m): %s", j.pilot, end, (now - j.combatReqAt) / 1000.0, d,
                            pkg ? "falling back to the get-in package (Ignore Combat, then calm)" : "no get-in package either");
                        if (!pkg) { Set(j, Phase::Idle); return; }
                        j.since = now;
                        j.startDist = d;
                        j.calmChecked = false;
                        return;
                    }
                    else return;  // the combat AI is bringing it in
                }
                // Walk boarding (quest package, j.cmd) has no 1 s package check: at 2.5 s a fighter that came no closer is calmed once (the
                // Ignore Combat package did not outrank its fight); one that did keeps fighting status all the way in.
                if (j.cmd && !aboard && !j.calmChecked && now - j.since > 2500) {
                    j.calmChecked = true;
                    const double d = Dist(npc, vehicle);
                    if (InCombat(npc) && d > j.startDist - 1.0) {
                        Calm(npc);
                        logger::Write("NPC vehicles: [%d] NPC %08X still fighting and no closer (%.1f -> %.1f m): Ignore Combat was not enough, calmed (fallback)", j.pilot, j.npc, j.startDist, d);
                    }
                    else logger::Write("NPC vehicles: [%d] NPC %08X on its way without calm (%.1f -> %.1f m, in combat %d)", j.pilot, j.npc, j.startDist, d, InCombat(npc) ? 1 : 0);
                }
                if(const auto entry=environment::EntrySite(vehicle);!j.armed && !aboard && entry.verdict!=environment::Verdict::Safe) {
                    if(now-j.since>kBoardTimeoutMs){pilotquest::Release(j.pilot-1);Set(j,Phase::Idle);logger::Write("Boarding refused: vehicle %08X not at rest (%s)",j.vehicle,entry.reason);}
                    return;
                }
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
				if constexpr(kAnimatedEntry) if (j.cmd && !aboard && j.cmdTryAt && !j.cmdInstant && now - j.cmdTryAt >= kAnimWaitMs) {
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
					FollowerNotice(j.npc,"The NPC can't reach that vehicle from here");
					pilotquest::Release(j.pilot - 1);
					Set(j, Phase::Idle);
					break;
				}
				if (aboard && !j.passenger && !Has3D(vehicle)) {
					// 2026-10-28 crash (01:16 run): the vehicle had no 3D yet, the pilot refused it, and this branch ran every frame
					// (the "aboard" line 50 times a second) until the game faulted. Wait for the 3D; after 10 s get out.
					if (!j.no3DSince) {
						j.no3DSince = now;
						logger::Write("NPC vehicles: [%d] NPC %08X aboard, vehicle %08X has no 3D yet: waiting", j.pilot, j.npc, j.vehicle);
					}
					else if (now - j.no3DSince > 10000) {
						logger::Write("NPC vehicles: [%d] vehicle %08X still has no 3D after 10 s: the NPC gets out", j.pilot, j.vehicle);
						j.no3DSince = 0;
						Leave(j, npc, "vehicle never loaded");
						if (j.phase == Phase::Boarding) { pilotquest::Release(j.pilot - 1); Set(j, Phase::Idle); }
					}
				}
				else if (aboard && j.coldStart && !trafficpair::Sitting(SitState(npc)) && j.boardSnaps < 5) {
					// 2026-10-30 (decompile FUN_141993710 -> FUN_1419C3980): the seat link made at the driver's creation never sits it
					// (sit state 0). The snap, once both models exist, sits it (sit state 3, the confirmed 2026-10-25 path); only a
					// sitting driver is carried by its vehicle, so the pilot gets the vehicle after that.
					if (now - j.reseatAt >= 1000) {
						j.reseatAt = now; ++j.boardSnaps;
						const double d = SeatDist(npc, vehicle);const int before = SitState(npc);
						const int r = trafficpair::NeedsSnap(true, true, before, d) ? SeatNewborn(npc, vehicle) : -3;
						logger::Write("NPC vehicles: [%d] cold-start driver %08X linked but not sitting (sit %d, %.1f m): snap -> %d, sit now %d (try %d)",
							j.pilot, j.npc, before, d, r, SitState(npc), j.boardSnaps);
					}
				}
				else if (aboard) {
					j.no3DSince = 0;
					logger::Write("NPC vehicles: [%d] NPC %08X aboard after %.1f s%s (sit state %d)", j.pilot, j.npc, (now - j.since) / 1000.0, j.armed ? " (pre-armed pilot)" : "", SitState(npc));
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
					// A traffic trip starts on its route (2026-10-30): its first goal is the trip's own next waypoint, not the escort
					// default (the vehicle's own spot), which read as "arrived" at once and, over a roof, switched the hover off.
					bool haveFirst = false;
					if (j.task == 9 && j.trip.step) {
						FvoTripGoal tg{};
						if (j.trip.step(j.trip.user, vpos.x, vpos.y, vpos.z, &tg) == 0) {
							first.x = tg.x; first.y = tg.y; first.z = tg.z; first.stop = tg.stop > 0 ? tg.stop : 6.0;
							first.nav.passThrough = tg.passThrough != 0;
#ifndef FVO_RELEASE
                            if (j.factoryStock) {
                                routeguidance::Clear(j.pilot);
                                first.nav.travel = travelmode::Default::Drive;
                                first.hold = true;
                            } else
#endif
                            vehicleflight::RouteNav(j.pilot,j.trip.user,tg,first.nav);
							if (tg.speed > 0) first.nav.speedLimit = tg.speed;
							haveFirst = true;
						}
					}
#ifndef FVO_RELEASE
                    if (j.factoryLease && !j.factoryStock) {
                        const auto* lane = routeguidance::Get(j.pilot, GetTickCount64());
                        if (!haveFirst || !lane || lane->travel != 1 || !lane->held) {
                            EndJobOf(j.vehicle, "factory route did not supply a held drive lane");
                            return;
                        }
                    }
#endif
					if ((haveFirst || vcontract::GoalFor(vcontract::User::Npc, j.pilot, vehicle, player, first)) && vehicleflight::PilotFly(j.pilot, vehicle, first)) {
						if (!g_told) FollowerNotice(j.npc,j.vehicle==vehicleflight::OwnRoverId()?"Your driver is ready":"Vehicle driver ready");
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
						if (combat == 1) {
							if (!j.resumeFoe) j.resumeFoe = BoardingEnemy(npc);
							Calm(npc);
						}
						const int r = Mount(npc, vehicle);
						logger::Write("NPC vehicles: [%d] NPC %08X get-in asked again (%s) -> %d", j.pilot, j.npc, combat == 1 ? "Ignore Combat was not enough: calmed first (fallback)" : "package was replaced", r);
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
				if (aboard && vehiclehits::Wrecked(j.vehicle)) {
                    j.controller=integrationlogic::Controller::Wreck;
					// Wrecked (vehiclehits: 0 health): the weapon is dead (SubmitFire), the autopilot banned; the driver gets out and fights on foot.
					if (j.firing) { SubmitFire(j.pilot - 1, j.vehicle, false); j.firing = false; }
					vehicleflight::PilotStop(j.pilot, "wrecked");
					FollowerNotice(j.npc, "Your follower's vehicle is wrecked | they get out");
					logger::Write("NPC vehicles: [%d] vehicle %08X wrecked; NPC %08X gets out", j.pilot, j.vehicle, j.npc);
					vcontract::ExitRequest eject;
					eject.mode = vcontract::ExitMode::Eject; eject.waitLanded = true; eject.timeoutMs = 12000;  // ride the wreck down, out at rest
					StartExit(npc, eject);
					break;
				}
				if(j.task && Aggressor(j.vehicle,now)) {
#ifndef FVO_RELEASE
                    if (j.factoryLease) {
                        EndJobOf(j.vehicle, "factory private lease interrupted by combat");
                        return;
                    }
#endif
					j.task=0;const auto vp=At<game::Vec3>(vehicle,off::kRefPosition);
					vcontract::Assign(j.vehicle,vcontract::Find("escort"),vp.x,vp.y);
				}
				if (j.task) {
                    j.controller=integrationlogic::Controller::Task;
					TaskTick(j, npc, vehicle, player, aboard, now);
					break;
				}
				if (j.profile == Profile::Escort && !vcontract::AssignedTo(j.vehicle)) {
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					vcontract::Assign(j.vehicle, vcontract::Find("escort"), vp.x, vp.y);  // a follower's navigation default
				}
				// The player's override (N recall / U cover / T guard) limits what the fight below may do.
				const auto mode = ModeOf(j.order);
				const auto rules = overrides::For(mode);
				if (!rules.fire && j.firing) { SubmitFire(j.pilot - 1, j.vehicle, false); j.firing = false; }
				// Following, as the player's rover follow (U): trail behind the player, park when the player stands still,
				// lift off again when the player moves away; the NPC stays aboard throughout.
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				if (!j.stillSince || std::hypot(pp.x - j.stillX, pp.y - j.stillY) > 1.0) {
					j.stillX = pp.x;
					j.stillY = pp.y;
					j.stillSince = now;
				}
				const bool playerStill = now - j.stillSince >= kFollowStillMs;
				// Three classes (vcontract::Class): Navigation says where the vehicle goes (escort by default), Combat how it fights (attack / defend,
				// chosen from Navigation when a fight starts), Weapons when it shoots. A hostile pursuer has only its attack profile, which also gives its goal.
				vcontract::Goal pg;
				bool have = vcontract::GoalFor(vcontract::User::Npc, j.pilot, vehicle, player, pg);
				if (!have) have = vcontract::GoalFor(vcontract::User::Npc, j.pilot, vehicle, player, pg, vcontract::Class::Combat);
				const vcontract::Profile* ap = vcontract::AssignedTo(j.vehicle);
				const char* navName = ap ? ap->name : nullptr;
				const bool escortNav = profileclass::Is(navName, "escort");
				// Your side (a follower's vehicle, or yours with a crew driver) fights by your stance; everyone else by its navigation profile.
				const bool playerSide = IsFollower(player, npc) || j.vehicle == vehicleflight::OwnRoverId();
				if (playerSide && j.profile == Profile::Escort) {
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					ApplyStance(j.vehicle, vp.x, vp.y);
					j.autoCombat = false;
				}
				const vcontract::Profile* cp = vcontract::AssignedTo(j.vehicle, vcontract::Class::Combat);
				const vcontract::Profile* wp = vcontract::AssignedTo(j.vehicle, vcontract::Class::Weapons);
				const auto weapons = profileclass::Effective(profileclass::WeaponsFor(wp ? wp->name : nullptr),
					mode == overrides::Mode::Recall, mode == overrides::Mode::Cover);
				// The combat target: defenders protect the player (vehicles in the fight first), attackers go for the player's enemies; nothing picked: a
				// hostile pursuer attacks the player.
				uptr foe = 0;
				if (aboard && have && Outdoors(player) && rules.pickFoe) foe = PickFoe(j, npc, vehicle, player, now);
				if (foe) j.fightAt = now;
				if (foe && !cp) {
					// A fight starts: the combat profile follows from the navigation profile; navigation itself stays assigned.
					if ((cp = vcontract::Find(profileclass::DefaultCombat(navName)))) {
						const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
						vcontract::Assign(j.vehicle, cp, vp.x, vp.y);
						j.autoCombat = true;
					}
				}
				else if (!foe && cp && j.autoCombat && now - j.fightAt > kFightOverMs) {
					vcontract::Unassign(j.vehicle, vcontract::Class::Combat);
					cp = nullptr;
					j.autoCombat = false;
				}
				j.fighting = foe != 0;
				// Cover and a guard point lock the combat profile to defend.
				const bool attacking = cp && profileclass::Is(cp->name, "attack") && mode == overrides::Mode::None;
				const bool retaliating=foe && foe==Aggressor(j.vehicle,now);
				const bool defending = foe && !attacking && !retaliating;
				if (!defending) j.defMode = -1;
				// Native target selection is authoritative; a profile never invents an enemy.
				const uptr foeVeh = foe ? InVehicle(foe) : 0;
				const uptr aimRef = foeVeh ? foeVeh : foe;
				const double toFoe = aimRef ? Dist(vehicle, aimRef) : 1e9;
				int defMode = 0, pMode = -1;
				if (foe && have && attacking && rules.chase) pMode = PursueGoal(j, aimRef, foeVeh != 0, vehicle, now, pg);
				else if (foe && (foe != player || retaliating) && have && rules.chase) {
					if (defending) defMode = DefendGoal(j, aimRef, foeVeh != 0, player, vehicle, now, pg);
					else OrbitGoal(j, aimRef, vehicle, pg);
				}
				// Orders and context for escorts: rescue wins over everything, a fight over the rest; then a crew marker order, then scouting.
				const bool escortJob = j.profile == Profile::Escort && have && escortNav && targetsession::CanFollow(j.vehicle,vehicleflight::OwnRoverId());
				const bool rescuer = escortJob && mode==overrides::Mode::None && j.order!=int(CrewOrder::Marker) && g_ec.mode == EscMode::Rescue && g_ec.rescuer == j.pilot;
				const bool scouting = escortJob && !defending && !rescuer && g_ec.mode == EscMode::Scout && g_ec.scout == j.pilot && j.order != int(CrewOrder::Marker) &&
					mode == overrides::Mode::None;
				bool toMarker = false;
				if (!rescuer) j.rescueParked = false;
				if (rescuer) {
					pg.nav.travel = travelmode::Default::Hover;  // speed first
					pg.x = pp.x;
					pg.y = pp.y;
					pg.z = std::numeric_limits<double>::quiet_NaN();
					pg.stop = 4.0;
					pg.hold = false;
				}
				else if (mode == overrides::Mode::Recall) {
					pg.x = pp.x; pg.y = pp.y; pg.z = std::numeric_limits<double>::quiet_NaN(); pg.stop = kRecallBoard * 0.6; pg.hold = false;
					toMarker = true;  // N: straight to the player, whatever the fight
				}
				else if (mode == overrides::Mode::Guard) {
					pg.x=j.orderX; pg.y=j.orderY; pg.z=j.orderZ; pg.stop=1.5; pg.hold=true;
					toMarker=true; // the guard point beats a fight; the escort defends from it
				}
				else if (escortJob && !defending && j.order == int(CrewOrder::Hold)) {
					pg.x=j.orderX; pg.y=j.orderY; pg.z=j.orderZ; pg.stop=1.5; pg.hold=true;
					toMarker=true; // explicit hold takes priority over the ordinary escort formation
				}
				else if (escortJob && !defending && j.order == int(CrewOrder::Marker)) {
					toMarker = MarkerGoal(j, vehicle, pg);
				}
				else if (scouting) {
					pg.x = g_ec.sx;
					pg.y = g_ec.sy;
					pg.z = std::numeric_limits<double>::quiet_NaN();
					pg.stop = kScoutHold;
					pg.hold = true;
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					if (!j.scoutReported && std::hypot(g_ec.sx - vp.x, g_ec.sy - vp.y) < kScoutHold + 6.0) {
						j.scoutReported = true;
						bool inVeh = false;
						int n = 0;
						EnemyNear(player, vp.x, vp.y, vp.z, 80.0, inVeh, n);
						if (n) FollowerNotice(j.npc,"Scout: %d %s at your marker", n, n == 1 ? "enemy" : "enemies");
						else FollowerNotice(j.npc,"Scout: your marker looks clear");
						logger::Write("NPC vehicles: [%d] scouted the marker: %d enemies within 80 m", j.pilot, n);
					}
				}
				// Manoeuvres replace the orbit for attackers, retaliators and escorts not held on a protective spot (shield when defensive, intercept,
				// overwatch keep their own goals); a pursuer near its hit limit keeps its retreat.
				bool manActive = false, manRam = false;
				ReadCombatIni();
				if (foe && have && aboard && rules.chase && mode == overrides::Mode::None && !rescuer && !toMarker && !scouting) {
					const bool use = attacking ? !j.retreating : retaliating || (defending && (defMode == 0 || (defMode == 1 && !j.defDefensive)));
					const bool shield = defending && defMode == 1 && j.defDefensive;  // the full AI interposes for a defensive escort
					if (g_combatIni.ai && (use || shield)) {
						manActive = CombatAITick(j, npc, foe, aimRef, foeVeh != 0, vehicle, player, j.profile == Profile::Pursuit, attacking,
							profileclass::MayAim(weapons, false), shield || (defending && j.profile == Profile::Escort), now, pg);
					}
					else if (use) {
						manActive = ManoeuvreTick(j, foe, aimRef, foeVeh != 0, vehicle, player, j.profile == Profile::Pursuit, attacking,
							profileclass::MayAim(weapons, false), now, pg);
						manRam = !manActive;
					}
				}
				bool searching=false;
                if(!foe&&have&&aboard&&g_combatIni.ai&&rules.chase&&mode==overrides::Mode::None&&!rescuer&&!toMarker&&!scouting)
                    searching=SearchAITick(j,npc,vehicle,now,pg);
                manActive=manActive||searching;
                // 2026-10-28 (the user: "I shouldn't have control in passenger seat, vehicle does so it's no different than being in an
                // autopilot seat"): a riding player no longer hands the vehicle to the taxi steering (look + W/S); the driver's own
                // job keeps flying it. Exit and take-the-wheel stay on the passenger keys.
                const auto owner=integrationlogic::ControllerFor(false,false,mode!=overrides::Mode::None||toMarker,rescuer,foe!=0,searching,false,j.task!=0);
                j.controller=owner;
                if (!manActive && j.aiOn) { j.aiOn=false; j.aiFire=false; j.aiPlan=false; vehicleflight::ClearPilotCommand(j.pilot); }
				if (!manActive && !manRam && j.manOn) { j.manOn = false; j.man = manoeuvre::Mind{}; }
				if (manRam) break;  // the ram order flies this vehicle (the Ramming branch above from the next frame)
                if(foe && !rescuer && !toMarker && !scouting && rules.chase && !manActive) {
                    const auto enemy=At<game::Vec3>(aimRef,off::kRefPosition);
                    const float threat[]={enemy.x,enemy.y,enemy.z+1.2f};
                    environment::CombatPosition(j.vehicle,threat,pg.x,pg.y,pg.z,defending||j.retreating);
                }
				const bool busyGoal = rescuer || toMarker || scouting;
				const bool attackRun = !manActive && defMode == 0 && (pMode < 0 || pMode == 10) && toFoe < kAttackRun && !rescuer && rules.chase &&
					profileclass::MayAim(weapons, false);
				if (!aboard) {
					if(j.vehicle==vehicleflight::OwnRoverId() && j.order)vehicleflight::EndTargetSession(vehicle);
					logger::Write("NPC vehicles: [%d] NPC %08X left the vehicle", j.pilot, j.npc);
					vehicleflight::PilotStop(j.pilot, "driver got out");
					Set(j, Phase::Idle);
				}
				else if(searching) {
                    vehicleflight::PilotFly(j.pilot,vehicle,pg);
                    if(j.firing){SubmitFire(j.pilot-1,j.vehicle,false);j.firing=false;}
                }
                else if (owner==integrationlogic::Controller::Taxi) {
					passenger::Steer(j.pilot, vehicle);  // taxi: the map marker, else where the passenger looks (a fight: the escort defends)
				}
				else if (!Outdoors(player)) {
					// Wait at the door: parked, aboard; following again when the player comes out (the relaunch below).
					if (!j.waiting) {
						j.waiting = true;
						if (vehicleflight::PilotActive(j.pilot)) vehicleflight::PilotStop(j.pilot, "you went indoors; waiting at the door");
						j.parked = true;
						logger::Write("NPC vehicles: [%d] you went indoors; NPC %08X waits at the door", j.pilot, j.npc);
					}
				}
				else if (mode == overrides::Mode::Recall && toPlayer < kRecallBoard) {
					vehicleflight::PilotStop(j.pilot, "recall: beside the player");
					j.parked = true;
					FollowerNotice(j.npc, "Your vehicle is here | your follower gets out");
					logger::Write("NPC vehicles: [%d] recall: %.0f m from you; NPC %08X gets out for you", j.pilot, toPlayer, j.npc);
					Set(j, Phase::Settling);
				}
				else if (!j.order && (!foe || !rules.chase) && (j.haveOrigin ? std::hypot(double(At<game::Vec3>(vehicle, off::kRefPosition).x) - j.originX, double(At<game::Vec3>(vehicle, off::kRefPosition).y) - j.originY) > kLeash : toPlayer > kGiveUp)) {
					logger::Write("NPC vehicles: [%d] stopped following (%s, %.0f m from you, %.0f m from where it started)", j.pilot, Outdoors(player) ? "too far from its origin" : "you went indoors", toPlayer,
						j.haveOrigin ? std::hypot(double(At<game::Vec3>(vehicle, off::kRefPosition).x) - j.originX, double(At<game::Vec3>(vehicle, off::kRefPosition).y) - j.originY) : 0.0);
					vehicleflight::PilotStop(j.pilot, "gave up");
					Set(j, Phase::Settling);
				}
				else if (have && !escortNav && !foe && ap) {
					// HOOK 1 of the vehicle contract: another navigation profile (patrol, hauler, ...) drives; a fight goes to the combat code below.
					vehicleflight::PilotFly(j.pilot, vehicle, pg);
				}
				else if (pMode == 14 && toFoe > kRetreatDist - 20.0 && j.profile == Profile::Pursuit) {
					// Retreated far enough: land, the driver gets out and fights on foot; the vehicle's autopilot stays off a minute.
					vehicleflight::PilotStop(j.pilot, "retreated");
					vehicleflight::DisableAutopilot(j.vehicle, 60000, "retreated (hit limit near)");
					logger::Write("NPC vehicles: [%d] pursuer retreated %.0f m; NPC %08X gets out to fight on foot", j.pilot, toFoe, j.npc);
					CallReinforcements(player, vehicle);
					Set(j, Phase::Settling);
				}
				else if (rescuer && (j.rescueParked || (vehicleflight::AtDestination(vehicle,pg.x,pg.y,pg.z,pg.stop) && environment::BoardingSite(vehicle).verdict==environment::Verdict::Safe))) {
					if (!j.rescueParked) {
						j.rescueParked = true;
						j.parked = true;
						vehicleflight::PilotStop(j.pilot, "rescue: landed beside you");
						FollowerNotice(j.npc,"Your follower is here - get in");
						logger::Write("NPC vehicles: [%d] rescue: landed %.0f m from you", j.pilot, toPlayer);
					}
				}
				else if (have && vehicleflight::PilotActive(j.pilot) && foe) {
					if ((kProfiles[int(j.profile)].hostile || defending || retaliating || attacking) && !InCombat(npc) && now - j.aggroAt >= kReaggroMs) {
						j.aggroAt = now;
						const bool ok = Aggro(npc, foe);
						logger::Write("NPC vehicles: [%d] NPC %08X attack: combat with %08X -> %s", j.pilot, j.npc, At<std::uint32_t>(foe, off::kFormId), ok ? "asked" : "failed");
					}
					vehicleflight::PilotFly(j.pilot, vehicle, pg);
					if (attackRun) AttackRun(j, aimRef, vehicle);
					// Vehicle weapon: bursts with the nose on the target and in range.
					double axp = 0, ayp = 0, azp = 0;
					if (!attackRun) AimPoint(j, aimRef, vehicle, axp, ayp, azp);  // (inside, AttackRun updated it)
					else {
						const auto ppl = At<game::Vec3>(aimRef, off::kRefPosition);
						const double t = vehiclehits::ShotTime(j.vehicle,Dist3(vehicle,aimRef));
						axp = ppl.x + j.aimVx * t; ayp = ppl.y + j.aimVy * t; azp = ppl.z + kAimHeight + j.aimVz * t;
					}
					const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
					const auto vr = At<game::Vec3>(vehicle, off::kRefRotation);
					if (j.aiOn) { axp=j.aiAimX; ayp=j.aiAimY; azp=j.aiAimZ; }
					if (!manActive && (defMode == 3 || pMode == 13 || mode == overrides::Mode::Guard) && profileclass::MayAim(weapons, false) && std::hypot(pg.x - vp.x, pg.y - vp.y) < 12.0) {
						// Overwatch: in place, the nose on the enemy (yaw and pitch), the autopilot holds the spot.
						vehicleflight::Command c;
						c.set = vehicleflight::Command::kYaw | vehicleflight::Command::kPitch;
						c.yawRate = std::clamp(1.6 * WrapAngle(std::atan2(axp - vp.x, ayp - vp.y) - vr.z), -1.2, 1.2);
						c.pitch = std::clamp(std::atan2(azp - vp.z, std::max(5.0, std::hypot(axp - vp.x, ayp - vp.y))), -0.6, 0.6);
						c.holdMs = 300;
						vehicleflight::PilotCommand(j.pilot, c);
					}
					const double aimErr = std::fabs(WrapAngle(std::atan2(axp - vp.x, ayp - vp.y) - vr.z));
					// The weapon fires along the nose (the projectile turn never took: 19:58 run, 0 aimed): yaw and pitch
					// both on the led aim point, in 3D range. Rotation x is nose down +.
					const double pitchWant = std::atan2(azp - vp.z, std::max(1.0, std::hypot(axp - vp.x, ayp - vp.y)));
					const double pitchErr = std::fabs(pitchWant + vr.x);
					const double cone = FireCone(Dist3(vehicle, aimRef));
					const bool onTarget = aimErr < cone && pitchErr < std::min(cone * 1.5, 0.9);
					const bool inRange = rules.fire && profileclass::MayFire(weapons, false) && (kProfiles[int(j.profile)].fire || defending || retaliating || attacking) &&
						Dist3(vehicle, aimRef) < vehiclehits::BallisticsOf(j.vehicle).range;
                    const float muzzle[]={vp.x+float(std::sin(vr.z)*4),vp.y+float(std::cos(vr.z)*4),vp.z+1.5f};
                    const float shot[]={float(axp),float(ayp),float(azp)};
                    const bool clearShot=environment::ClearShot(j.vehicle,At<std::uint32_t>(aimRef,off::kFormId),
                        aimRef==vehicle?0:At<std::uint32_t>(aimRef,off::kFormId),muzzle,shot);
                    const bool canFire = inRange && clearShot && onTarget && (!j.aiOn || j.aiFire);
					// ClearShot and launch now use the same final AI aim point.
					StoreAim(j.pilot - 1, AimSnap{ j.vehicle, j.npc, axp, ayp, azp, inRange && clearShot, now });
					const int slot = j.pilot - 1;
					const bool wasFiring = j.firing;
					const bool wantFire = j.weaponBurst.Update(canFire, now);
					if (wantFire) {
						const bool ok = SubmitFire(slot, j.vehicle, true);
						j.firing = ok || wasFiring;
						if (!wasFiring && ok) {
							if (j.bursts++ % 10 == 0) {
								logger::Write("NPC vehicles: [%d] vehicle %08X weapon burst %u (%.0f m, aim off %.0f deg, pitch off %.0f deg) -> input sent", j.pilot, j.vehicle, j.bursts, toFoe, aimErr * 57.2958, pitchErr * 57.2958);
							}
						}
					}
					else if (wasFiring) { j.firing = !SubmitFire(slot, j.vehicle, false); }
					if (!canFire && now-j.fireBlockedLogAt>=3000) {
						j.fireBlockedLogAt=now;
						logger::Write("NPC fire: [%d] %08X holding fire: %s (range %.0f m, yaw %.0f deg, pitch %.0f deg; role %s)",j.pilot,j.vehicle,
							!inRange?"profile or range":!clearShot?"blocked shot":!onTarget?"turning to aim":"AI safety or ally in line",Dist3(vehicle,aimRef),aimErr*57.2958,pitchErr*57.2958,brain::Name(j.role));
					}
					else if (wantFire && !j.firing && now-j.fireBlockedLogAt>=3000) {
						j.fireBlockedLogAt=now;
						logger::Write("NPC fire: [%d] %08X native fire input FAILED",j.pilot,j.vehicle);
					}
				}
				else if (have && vehicleflight::PilotActive(j.pilot)) {
					const double above = vehicleflight::PilotAboveGround(j.pilot, vehicle);
					// Parked only once down (2026-10-28: a follower parked in the air dropped); ground unknown: as before.
					// Never while you drive it from the passenger seat (2026-11-18 00:30:37: parked ~20 times a second under your control).
					if (kProfiles[int(j.profile)].parkWhenStill && playerStill && toPlayer < kFollowFar && !busyGoal && (!std::isfinite(above) || above < 3.5) &&
						!(passenger::Controlling() && passenger::RidingVehicle() == j.vehicle)) {
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
					const bool orbit = kProfiles[int(j.profile)].orbit || defending || attacking || toMarker || scouting || (rescuer && !j.rescueParked);
					if (j.waiting) {
						j.waiting = false;
						logger::Write("NPC vehicles: [%d] you are outside again; following", j.pilot);
					}
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
		constexpr std::uint32_t kPlayerEntry = 0x01A66BA0, kSitAction = 0x019F7100;
		using PlayerEntry_t = void (*)(uptr, uptr);
		using SitAction_t = std::uint8_t (*)(uptr, uptr, uptr, uptr);
		PlayerEntry_t g_playerEntryOriginal = nullptr;
		SitAction_t g_sitActionOriginal = nullptr;
		thread_local boardinggate::Context g_sitContext;
		// The game's seat choice (decompile 2026-10-28): FUN_141951fb0 (get into a vehicle) asks FUN_1412B2C40(actor, vehicle) for
		// the seat via FUN_141A05D40 (entry) and FUN_141995460 (seat link): the FIRST enabled seat marker of the base (+0x270
		// count, +0x278 x 0x28, +0x264 mask) whose keyword (+0x10) the actor carries. It never asks whether the seat is taken,
		// so the player (driver keyword) was seated in the driver seat on top of an NPC driver. While the player faces a
		// vehicle someone else drives, the player's seat is that vehicle's passenger seat; the game then does its own
		// passenger get-in. Facing state is kept on the main thread (SeatTick); the hook only reads atomics.
		constexpr std::uint32_t kSeatKeyword = 0x012B2C40;
		using SeatKeyword_t = uptr (*)(uptr, uptr);
		SeatKeyword_t g_seatKeywordOriginal = nullptr;
		std::atomic<uptr> g_swapVehicle{ 0 }, g_swapKeyword{ 0 }, g_swapped{ 0 };
		std::atomic<uptr> g_boardVehicle{ 0 }, g_boardKeyword{ 0 };
		std::atomic<ULONGLONG> g_boardUntil{ 0 };
		std::uint32_t g_passengerRequest = 0;
		boardinggate::PassengerAttempt g_passengerAttempt;
		// 2026-10-28 (04:36 run: hold E pulled the driver out, then the player glitched in the seat): seating the player as the
		// driver after a pull-out asked the seat picker too, and the swap, still seeing the driver who had just left, gave the
		// passenger seat to the driver setup. While FVO seats the player as driver, the swap is off for that vehicle.
		std::atomic<uptr> g_driverFor{ 0 };
		std::uint32_t g_vehFlagsFor = 0, g_vehFlagsBack = 0;
		std::uint32_t g_standNpc = 0, g_standVehicle = 0;  // a pulled-out NPC standing up out of its seat (furniture state 4)
		ULONGLONG g_standAt = 0;
		void TakeSeat(uptr player, uptr vehicle, uptr occupant, const char* why);  // a vehicle whose +0x378 was changed for a get-out, and its flags before
		ULONGLONG g_vehFlagsUntil = 0;
		std::atomic<ULONGLONG> g_driverUntil{ 0 };
		uptr SeatKeywordDetour(uptr actor, uptr vehicle)
		{
			// SL-1: a laddered vehicle seats an NPC at its current ladder marker (never the player: the player's seat logic below is unchanged).
			if (actor && vehicle && actor != Ptr(addr::kPlayerCharacterPtr)) {
				std::uint32_t marker = 0; bool laddered = false;
				{ std::lock_guard lk(g_ladderLock); const auto it = g_ladderMarker.find(At<std::uint32_t>(vehicle, off::kFormId)); if (it != g_ladderMarker.end()) { marker = it->second; laddered = true; } }
				if (laddered) { if (const uptr kw = MarkerKeyword(At<uptr>(vehicle, 0x98), marker)) return kw; }
			}
			const bool asDriver = vehicle == g_driverFor.load() && GetTickCount64() < g_driverUntil.load();
			const uptr pinned = GetTickCount64() < g_boardUntil.load() ? g_boardVehicle.load() : 0;
			const uptr kw = boardinggate::PassengerKeyword(vehicle, asDriver, pinned, g_boardKeyword.load(), g_swapVehicle.load(), g_swapKeyword.load());
			if (actor && vehicle && kw && actor == Ptr(addr::kPlayerCharacterPtr)) {
				// Keep the seat stable when the boarding camera stops facing the vehicle.
				g_boardKeyword = kw; g_boardVehicle = vehicle;
				g_boardUntil = GetTickCount64() + 10000;
				if (g_swapped.exchange(vehicle) != vehicle) logger::Write("Seat choice: vehicle %08X has a driver: your seat is its passenger seat (asked from 14%07llX)",
					At<std::uint32_t>(vehicle, off::kFormId), static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - game::Base()));
				return kw;
			}
			return g_seatKeywordOriginal(actor, vehicle);
		}

		// An actor's name as the game shows it (vtable +0x268, what the console's PickLastRef prints); the form id when it has none.
		const char* RawDisplayName(uptr ref)
		{
			__try {
				return reinterpret_cast<const char* (*)(uptr)>((*reinterpret_cast<void***>(ref))[0x268 / 8])(ref);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		// The name of what a reference is: its own display name, else its base object's (vtable +0x268 on the base, which the
		// game's own display name falls back to (FUN_140B252F0) but skips in some states: the HUD showed form ids 2026-10-21).
		// "" when neither has one.
		const char* BaseName(uptr ref)
		{
			const char* n = ref ? RawDisplayName(ref) : nullptr;
			if (n && n[0]) return n;
			const uptr base = ref ? At<uptr>(ref, 0x98) : 0;
			n = base ? RawDisplayName(base) : nullptr;
			if (n && n[0]) return n;
			static int told = 0;  // why a name is missing (2026-10-21: the card showed "Vehicle")
			if (ref && told < 6) {
				++told;
				logger::Write("NPC vehicles: no name for %08X (base %08X, form type %u; the reference's name \"%s\", the base's \"%s\")", At<std::uint32_t>(ref, off::kFormId),
					base ? At<std::uint32_t>(base, off::kFormId) : 0, base ? unsigned(At<std::uint8_t>(base, 0x2E)) : 0u, RawDisplayName(ref) ? RawDisplayName(ref) : "(null)",
					base && RawDisplayName(base) ? RawDisplayName(base) : "(null)");
			}
			return "";
		}

		void ActorName(uptr actor, std::uint32_t id, char* out, size_t cap)
		{
			const char* n = BaseName(actor);
			if (n[0]) std::snprintf(out, cap, "%s", n);
			else std::snprintf(out, cap, "%08X", id);
		}

		// One vehicle's health line: "REV-8 (Sarah)  [#######---] 70%  driver 94%".
		bool LifeOf(uptr player, std::uint32_t id, uptr driven, char* out, std::size_t size)
		{
			const uptr veh = RefById(id);
			if (!veh) return false;
			// Short enough for the card's field (2026-10-21, the user: the line overflowed): names cut at 14 characters (the game's
			// own enemy-name limit), no text bar (the bar under the enemy's health bar is the graphic one).
			char name[15] = "", driverName[15] = "";
			std::snprintf(name, sizeof(name), "%s", BaseName(veh)[0] ? BaseName(veh) : "Vehicle");
			uptr driver = 0;
			if (veh == driven) driver = player;
			for (const auto& j : g_jobs) if (!driver && j.vehicle == id && j.phase == Phase::Following) driver = RefById(j.npc);
			static world::Snapshot snap;
			if (!driver && world::Vehicles(snap)) for (std::uint32_t k = 0; k < snap.count && k < world::kMaxVehicles; ++k) if (snap.v[k].formId == id && snap.v[k].driverId) driver = RefById(snap.v[k].driverId);
			if (driver && driver != player) std::snprintf(driverName, sizeof(driverName), "%s", BaseName(driver));
			const float share = vehiclehits::HealthShare(id);
			char drv[40] = "";
			float hn = 0, hm = 0;
			if (driver && !Dead(driver) && vehiclehits::ActorHealth(driver, hn, hm)) std::snprintf(drv, sizeof(drv), "  %s %.0f%%", driver == player ? "you" : "driver", hn / hm * 100.0f);
			std::snprintf(out, size, "%s%s%s%s  %.0f%%%s%s", name, driverName[0] ? " (" : "", driverName, driverName[0] ? ")" : "", share * 100.0f, drv,
				vehiclehits::Wrecked(id) ? "  WRECKED" : "");
			return true;
		}

		// ---- Vehicle lifebar (2026-10-21, the user approved the HUD row first; enemy health asked for 2026-10-21): up to two
		// lines in the status card. The other vehicle: the one under the crosshair (and 3 s after), else the one your side hit
		// last (6 s), else the nearest vehicle an enemy drives within 400 m. Then your own vehicle (the one you drive, else
		// your own one within 300 m), always.
		bool LifebarLine(uptr player, char* out, std::size_t size)
		{
			static std::uint32_t aimed = 0;
			static ULONGLONG aimedAt = 0;
			const ULONGLONG now = GetTickCount64();
			const uptr driven = EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(player);
			std::uint32_t mine = driven ? At<std::uint32_t>(driven, off::kFormId) : 0;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			if (!mine) {
				const uptr own = RefById(vehicleflight::OwnRoverId());
				if (own && At<uptr>(own, off::kRefParentCell) && At<uptr>(player, off::kRefParentCell)) {
					const auto a = At<game::Vec3>(own, off::kRefPosition);
					if (std::hypot(a.x - pp.x, a.y - pp.y, a.z - pp.z) < 300.0) mine = vehicleflight::OwnRoverId();
				}
			}
			std::uint32_t id = 0;
			const std::uint32_t cross = vehiclehits::CrosshairTarget();
			if (cross) {
				const uptr r = RefById(cross);
				const uptr veh = r && IsVehicle(r) ? r : r && IsActorRef(r) ? InVehicle(r) : 0;
				if (veh && At<std::uint32_t>(veh, off::kFormId) != mine) { aimed = At<std::uint32_t>(veh, off::kFormId); aimedAt = now; }
			}
			if (aimed && now - aimedAt < 3000) id = aimed;
			std::uint32_t hit = 0;
			ULONGLONG hitAt = 0;
			if (!id && vehiclehits::LastHitByYourSide(hit, hitAt) && now - hitAt < 6000 && hit != mine) id = hit;
			if (!id) {
				// The nearest enemy-driven vehicle (checked twice a second; the world snapshot carries the seated driver).
				static std::uint32_t enemy = 0;
				static ULONGLONG enemyAt = 0;
				if (now - enemyAt >= 500) {
					enemyAt = now;
					enemy = 0;
					static world::Snapshot snap;
					double best = 400.0;
					if (world::Vehicles(snap)) for (std::uint32_t k = 0; k < snap.count && k < world::kMaxVehicles; ++k) {
						const auto& v = snap.v[k];
						if (!v.driverId || v.formId == mine || vehiclehits::Wrecked(v.formId)) continue;
						const uptr d = RefById(v.driverId);
						if (!d || d == player || Dead(d) || IsFollower(player, d) || !(HostileTo(d, player) || HostileTo(player, d))) continue;
						const double dist = std::hypot(v.pos[0] - pp.x, v.pos[1] - pp.y, v.pos[2] - pp.z);
						if (dist < best) { best = dist; enemy = v.formId; }
					}
				}
				id = enemy;
			}
			out[0] = 0;
			std::size_t used = 0;
			if (id && LifeOf(player, id, driven, out, size)) used = std::strlen(out);
			if (mine && used + 2 < size) {
				if (used) out[used++] = '\n';
				if (!LifeOf(player, mine, driven, out + used, size - used) && used) out[--used] = 0;
			}
			return out[0] != 0;
		}

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

		// ---- Seat rules (2026-10-17; notes/VEHICLE_INTERACTIONS_RESEARCH_20261017.md) ----
		// FUN_14219BE30 runs after ANY actor's sit (caller 1419F7750); the enclosing sit action supplies actor identity.
		// Refuse unsafe player entry before native seat transitions. Rules for the player and an occupied vehicle:
		//   gang driver -> HAND-OVER (out at once, the player sits);  neutral / hostile -> BLOCK on a tap,
		//   EJECT on Activate held kPullMs (neutral: assault alarm, plus a theft alarm when someone else owns it).
		// Out at once = FUN_141A9C530(actor) (what the game's dismount uses for distant actors); the alarms are the
		// Papyrus natives' cores: Actor.SendAssaultAlarm FUN_141FC9770(_, _, victim), ObjectReference.SendStealAlarm
		// FUN_14207F050(_, _, ref, thief).
		constexpr std::uint32_t kEndInteraction = 0x01A9C530, kAssaultAlarm = 0x01FC9770, kStealAlarm = 0x0207F050;
		constexpr ULONGLONG kPullHoldMs = 500;     // Activate held this long on an occupied vehicle = pull the occupant out
		// (2026-10-24, the user approved the gesture review: tap = the PASSENGER seat at once, hold = pull the occupant out.
		// 2026-10-18 to 10-24 it was tap after a double-tap window / double tap, which made the seat feel late.)
		constexpr double kReachRange = 3.0;        // m: from the vehicle's side (its footprint), not its centre (2026-10-23)
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
			ULONGLONG seatUntil = 0;               // keep waiting for the driver seat to empty until then
		} g_intent;
		bool g_activateDown = false;

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
			for (const uptr r : modscene::LoadedRefs(8000)) {  // the frame's shared scan (200 missed most of a city)
				if (r == player || !IsActorRef(r) || Dist(r, vehicle) >= 8.0 || IsVehicle(r) || Dead(r)) continue;
				if (InVehicle(r) == vehicle) return r;
			}
			return 0;
		}

		// The vehicle's footprint radius (its larger half-extent from the shared vehicle list), 1.5 m when unknown.
		double Footprint(std::uint32_t formId)
		{
			static ::world::Snapshot snap;
			static ULONGLONG at = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - at > 250) { at = now; if (!::world::Vehicles(snap)) snap.count = 0; }
			for (std::uint32_t i = 0; i < snap.count && i < world::kMaxVehicles; ++i)
				if (snap.v[i].formId == formId) return std::clamp(double(std::max(snap.v[i].half[0], snap.v[i].half[1])), 1.0, 8.0);
			return 1.5;
		}
		double EdgeDistance(uptr player, uptr v) { return std::max(0.0, Dist(player, v) - Footprint(At<std::uint32_t>(v, off::kFormId))); }

		// Why the nearest vehicle someone else drives is or is not the one in front of the player (the prompt log).
		struct Reach { uptr vehicle = 0; double centre = 0, edge = 0, off = 0, cone = 0; };
		// The vehicle in front of the player: within kReachRange of its side and facing it (the cone widens with its size;
		// right beside it any direction counts).
		uptr FacingVehicle(uptr player, Reach* nearestDriven = nullptr)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			const double facing = At<game::Vec3>(player, off::kRefRotation).z;
			uptr best = 0;
			double bestD = 1e9, nearD = 15.0;
			// 2026-10-28: this scanned LoadedRefs(200), the first 200 references in cell order, which in New Atlantis rarely
			// held the vehicle in front of the player: the seat prompt and the seat swap never saw it. The vehicle snapshot
			// lists every loaded vehicle.
			static ::world::Snapshot snap;
			static ULONGLONG at = 0;
			if (const ULONGLONG now = GetTickCount64(); now - at > 100) { at = now; if (!::world::Vehicles(snap)) snap.count = 0; }
			for (std::uint32_t i = 0; i < snap.count && i < world::kMaxVehicles; ++i) {
				const uptr r = RefById(snap.v[i].formId);
				if (!r || Disabled(r)) continue;
				const auto q = At<game::Vec3>(r, off::kRefPosition);
				const double dx = double(q.x) - pp.x, dy = double(q.y) - pp.y, d = std::hypot(dx, dy);
				if (d > 15.0) continue;
				const double fp = Footprint(At<std::uint32_t>(r, off::kFormId)), edge = std::max(0.0, d - fp);
				double off = std::atan2(dx, dy) - facing;
				while (off > 3.14159265) off -= 6.28318531;
				while (off < -3.14159265) off += 6.28318531;
				const double cone = kReachCone + std::atan2(fp, std::max(d, 0.1));
				if (nearestDriven && d < nearD && DriverOf(r, player)) { nearD = d; *nearestDriven = { r, d, edge, off, cone }; }
				if (edge > kReachRange) continue;
				if (edge > 1.5 && std::fabs(off) > cone) continue;
				if (edge < bestD) { bestD = edge; best = r; }
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

		void CrimeAlarms(uptr victim, uptr vehicle, uptr player, bool assault, bool theft)
		{
			__try {
				if (assault) EngineFn<void (*)(uptr, uptr, uptr)>(kAssaultAlarm)(0, 0, victim);
				if (theft && OwnerId(vehicle) && OwnerId(vehicle) != At<std::uint32_t>(At<uptr>(player, 0x98), off::kFormId))
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
			// KO-1 (2026-11-08, kick-out port): the player takes this vehicle now: recorded at once, so traffic lets go of the pair (no re-seat, bring or move
			// of its driver) and a birth-held chassis is released to the player (2026-11-07 21:12: FF100075 re-seated 2 s after the pull-out, FF10006C frozen).
			trafficpool::NotePlayerTook(At<std::uint32_t>(vehicle, off::kFormId), !std::strcmp(why, "hijack") ? "hijack" : !std::strcmp(why, "hand-over") ? "hand_over" : "pulled_out");
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.npc == oid) {
					vehicleflight::PilotStop(j.pilot, why);
					Set(j, Phase::Idle);
				}
			}
			// 2026-10-28 (the user: the pulled-out NPC "was seated in the air"): ending the interaction alone left the NPC in its
			// sitting pose at the seat. The instant exit, the native seat-state cleanup (on-foot controller and animation) and a
			// place beside the vehicle, as the exit service does.
			// 10:57 run (the user: the pulled-out NPC still sits in the air): FUN_141952530 is the creature-mount dismount (it needs
			// the seat link to point at an Actor, never a vehicle), and forcing furniture state 0 skips the stand-up animation. The
			// NPC is in state 3 (sitting): ask for state 4 (want to stand), the game's own get-up out of the seat. SeatTick falls
			// back to the instant exit if the NPC is still seated after 4 s.
			{
				// 11:01 run: furniture state 4 was taken but nothing stood up in 4 s. The game's own get-up is actor vtable +0xB30: what
				// FUN_141951FB0 does for a seated actor, and FUN_14192A3F0 when an actor activates the vehicle it sits in.
				bool asked = false;
				__try {
					reinterpret_cast<void (*)(uptr)>((*reinterpret_cast<void***>(occupant))[0xB30 / 8])(occupant);
					asked = true;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) { asked = false; }
				const int before = asked ? 3 : -1;
				if (before == 3) {
					g_standNpc = oid; g_standVehicle = At<std::uint32_t>(vehicle, off::kFormId); g_standAt = GetTickCount64();
					logger::Write("NPC vehicles: seat rules: %08X asked to get up out of the seat (the game's get-up, actor vtable +0xB30)", oid);
					g_intent.acted = true;
					g_intent.seatAt = GetTickCount64() + 150;
					g_intent.seatUntil = GetTickCount64() + 10000;
					g_driverFor = vehicle; g_driverUntil = GetTickCount64() + 11000;
					logger::Write("NPC vehicles: seat rules: %s: %08X out of %08X (%s) -> standing up; you get in next", why, oid, At<std::uint32_t>(vehicle, off::kFormId),
						RelationName(g_intent.relation));
					(void)player;
					return;
				}
				logger::Write("NPC vehicles: seat rules: %08X stand-up request: furniture state was %d; the instant exit instead", oid, before);
			}
			// 10:42 run: the instant exit + warp put the NPC on the ground but still in its sitting animation ("sit in air"). First
			// the game's animated dismount (FUN_141952530): the NPC gets out with its own animation, and the player's get-in
			// waits for the seat to empty. Only when the game refuses it, the instant exit below.
			// The game's dismount (FUN_141952530) refuses when actor +0x378 bit 1 or bit 3 ("getting on / off") is set, when there is no
			// seat interaction (extra 0xA5), and does the instant exit at process level > 1. NPCs FVO seats can keep a stale bit 3
			// (NPC boarding causes, 2026-10-17): cleared here so the game plays its own get-out.
			std::uint32_t& flags = At<std::uint32_t>(occupant, 0x378);
			const std::uint32_t flagsBefore = flags;
			const uptr proc = At<uptr>(occupant, 0x228);
			const int level = proc ? At<std::uint8_t>(proc, 0xE7) : -1;
			const bool seatLink = HasSeatInteraction(occupant);
			if ((flags >> 3) & 1) flags &= ~(1u << 3);
			// 10:53 run: every listed check passed and it still refused. Further in, the dismount takes the vehicle from the seat
			// link and needs ITS +0x378 bit 1 set, which the native mount sets and FVO's instant seat never does (why no FVO NPC
			// dismount ever succeeded). Set for the get-out; restored once the NPC is out (SeatTick) or after 10 s.
			std::uint32_t& vflags = At<std::uint32_t>(vehicle, 0x378);
			const std::uint32_t vflagsBefore = vflags;
			vflags |= 1u << 1;
			g_allowDismount = true;
			const int dismount = Dismount(occupant);
			g_allowDismount = false;
			if (dismount == 1) { g_vehFlagsFor = At<std::uint32_t>(vehicle, off::kFormId); g_vehFlagsBack = vflagsBefore; g_vehFlagsUntil = GetTickCount64() + 10000; }
			else vflags = vflagsBefore;
			logger::Write("NPC vehicles: seat rules: %08X get-out asked: flags %08X (bit1 %u, bit3 %u%s), seat interaction %s, process level %d -> %s", oid, flagsBefore,
				(flagsBefore >> 1) & 1, (flagsBefore >> 3) & 1, (flagsBefore >> 3) & 1 ? ", cleared" : "", seatLink ? "yes" : "NO", level,
				dismount == 1 ? "the game's get-out animation" : "refused");
			if (dismount == 1) {
				logger::Write("NPC vehicles: seat rules: %08X pulled out with the game's own get-out animation", oid);
				g_intent.acted = true;
				g_intent.seatAt = GetTickCount64() + 150;
				g_intent.seatUntil = GetTickCount64() + 10000;
				g_driverFor = vehicle; g_driverUntil = GetTickCount64() + 11000;
				logger::Write("NPC vehicles: seat rules: %s: %08X out of %08X (%s) -> getting out; you get in next", why, oid, At<std::uint32_t>(vehicle, off::kFormId),
					RelationName(g_intent.relation));
				(void)player;
				return;
			}
			const bool out = InstantExit(occupant, true);
			// 10:29 run: the seat state cleared but a MoveReference placement keeps the stale seat support (the NPC floats). The
			// eject path's warp: furniture-state cleanup, then the native actor warp that resets the controller's support and
			// physics proxy, 3 m to the vehicle's right at its height; the fall watch grounds it if it still hangs.
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double h = At<game::Vec3>(vehicle, off::kRefRotation).z;
			const float sx = float(vp.x + std::cos(h) * 3.0), sy = float(vp.y - std::sin(h) * 3.0), sz = vp.z + 0.5f;
			const bool placed = WarpExitController(occupant, sx, sy, sz);
			for (auto& watch : g_fallWatch) if (!watch.npc) { watch = { oid, At<std::uint32_t>(vehicle, off::kFormId), sz, GetTickCount64(), 0 }; break; }
			logger::Write("NPC vehicles: seat rules: %08X pulled out: exit %s, %s", oid, out ? "done" : "FAILED",
				placed ? "seat cleared and warped beside the vehicle" : "warp FAILED");
			g_intent.acted = true;
			g_intent.seatAt = GetTickCount64() + 150;
			g_intent.seatUntil = GetTickCount64() + 10000;  // the vehicle's seat record clears some seconds after the exit (10:29 run: 3 s was short)
			g_driverFor = vehicle; g_driverUntil = GetTickCount64() + 11000;  // the player takes the driver seat: no seat swap meanwhile
			logger::Write("NPC vehicles: seat rules: %s: %08X out of %08X (%s) -> %s; you get in next", why, oid, At<std::uint32_t>(vehicle, off::kFormId),
				RelationName(g_intent.relation), out ? "out" : "FAILED");
			(void)player;
		}

		// 2026-10-28 (02:17 run: the player still got the driver seat of FE007004 and FVO saw nobody in it): an NPC can sit in
		// a vehicle's seat as furniture (posed by a placement mod) without the game calling it "in the vehicle". Its current
		// furniture is a ref handle at process mid-high +0x424 and its seat index at +0x4F8 (FUN_141A08780, FUN_1419F7100);
		// seat 0 is the driver's. The actor within 8 m in that seat, or 0.
		uptr DriverSeatFurniture(uptr handles, uptr actor)  // the furniture `actor` sits in at seat 0 (addref'd), or 0
		{
			uptr ref = 0;
			__try {
				const uptr proc = At<uptr>(actor, 0x228), mh = proc ? At<uptr>(proc, 8) : 0;
				if (!mh || At<std::int32_t>(mh, 0x4F8) != 0) return 0;
				const std::uint32_t handle = At<std::uint32_t>(mh, 0x424);
				if (!handle) return 0;
				uptr* out = &ref;
				struct { uptr vtbl; uptr** capture; } lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
				EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(handles, handle, &lookup);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { ref = 0; }
			return ref;
		}
		uptr FurnitureDriver(uptr vehicle, uptr player)
		{
			const auto handles = Ptr(addr::kRefHandleTablePtr);
			if (!handles) return 0;
			for (const uptr r : modscene::LoadedRefs(8000)) {
				if (r == player || !IsActorRef(r) || Dist(r, vehicle) >= 8.0 || IsVehicle(r) || Dead(r)) continue;
				const uptr ref = DriverSeatFurniture(handles, r);
				if (!ref) continue;
				const bool here = ref == vehicle;
				EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref);
				if (here) return r;
			}
			return 0;
		}

		// Someone else drives it: a follower at the wheel, or anyone in the player's own rover, never keeps the player out
		// (as before the 2026-10-28 boarding gate). Seated in the vehicle, or sitting in its driver seat as furniture.
		bool DrivenByOther(uptr vehicle, uptr player)
		{
			if (!vehicle || !player || At<std::uint32_t>(vehicle, off::kFormId) == vehicleflight::OwnRoverId()) return false;
			uptr other = DriverOf(vehicle, player);
			if (!other) other = FurnitureDriver(vehicle, player);
			return other && !IsFollower(player, other);
		}

		// 2026-10-28 (the user was refused an empty parked REV-8 a traffic NPC was still walking to): an everyday-traffic or
		// scripted trip whose NPC is not in the vehicle yet gives way to the player. Crew, gang and combat jobs keep it.
		bool YieldToPlayer(std::uint32_t vehicleId, uptr vehicle, uptr player)
		{
			if (DriverOf(vehicle, player)) return false;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Boarding || j.vehicle != vehicleId || !j.task || j.gang || j.order || j.combatReq) continue;
				logger::Write("NPC vehicles: [%d] NPC %08X was walking to vehicle %08X; the player got there first: trip cancelled", j.pilot, j.npc, vehicleId);
				pilotquest::Release(j.pilot - 1);
				Set(j, Phase::Idle);
				return !OursVehicle(vehicleId);
			}
			return false;
		}

		// Reject before FUN_141A66BA0 changes process state or sends ActionActivate.
		// The late player-seat callback runs after the sit action and cannot safely
		// undo that action by ending/clearing the player's interaction in its stack.
		void PlayerEntryDetour(uptr actor, uptr target)
		{
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const bool vehicle = actor && actor == player && target && IsVehicle(target);
			const std::uint32_t id = vehicle ? At<std::uint32_t>(target, off::kFormId) : 0;
			const auto refusal = boardinggate::BeforeEntry(actor && actor == player, vehicle,
				id && OursVehicle(id) && !YieldToPlayer(id, target, player), vehicle && DrivenByOther(target, player), id && vehiclehits::Wrecked(id));
			if (vehicle) logger::Write("Boarding gate: player entry into %08X: reserved %d, driven by another %d, wrecked %d -> %s", id, id && OursVehicle(id),
				DrivenByOther(target, player), id && vehiclehits::Wrecked(id), refusal == boardinggate::Refusal::None ? "native get-in" : "refused");
			boardinggate::Begin(refusal, [&] { g_playerEntryOriginal(actor, target); }, [&](auto why) {
				notify::Show("%s", why == boardinggate::Refusal::Wrecked ? "That vehicle is wrecked" :
					why == boardinggate::Refusal::Occupied ? "Tap to ride; hold to take the wheel" : "That vehicle is taken");
				logger::Write("Boarding gate: player entry into %08X refused BEFORE animation (%s); player state untouched", id,
					why == boardinggate::Refusal::Wrecked ? "wrecked" : why == boardinggate::Refusal::Occupied ? "occupied" : "NPC reservation");
			});
		}

		// Verified 1.16.244 ABI: process, actor (RDX -> RSI), target (R8 -> RDI),
		// action data (R9 -> R14); result is AL. The actor survives until the
		// call to the player-only helper at 1419F774B. No input timing guesses.
		std::uint8_t SitActionDetour(uptr process, uptr actor, uptr target, uptr action)
		{
			boardinggate::Scope scope(g_sitContext, {actor, target});
			return g_sitActionOriginal(process, actor, target, action);
		}

		void SeatPlayerDetour(uptr vehicle)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - game::Base();
			const std::uint32_t id = vehicle ? At<std::uint32_t>(vehicle, off::kFormId) : 0;
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const bool restricted = id && (OursVehicle(id) || (player && DrivenByOther(vehicle, player)) || vehiclehits::Wrecked(id));
			const auto completion = boardinggate::Finish(g_sitContext, player, vehicle, restricted);
			boardinggate::Complete(completion, [&] {
				// 2026-10-28 (01:29 run): the pre-entry gate did not stop a get-in at a vehicle an NPC was driving and the player
				// got the driver seat beside it. Someone else at the wheel: the player completes into a passenger seat instead.
				// The seat choice gave the player the passenger seat: finish that seat (FUN_141A9C8C0), never FUN_14219BE30, which
				// makes the player the driver (passenger.h).
				// 02:39 run: finishing alone left no seat (for the player, FUN_14219BE30 is what builds the seat link) and the game
				// ended it 19 ms later. The player's link is built here with the passenger seat's keyword, as passenger.cpp does.
				const bool asDriver = vehicle == g_driverFor.load() && GetTickCount64() < g_driverUntil.load();
				if (!asDriver && (g_swapped.load() == vehicle || DrivenByOther(vehicle, player))) {
					g_swapped = 0;
					if (passenger::FinishPassengerSeat(player, vehicle)) {
						g_boardVehicle = 0; g_boardKeyword = 0; g_swapped = 0;
						logger::Write("Boarding gate: %08X has a driver: you got in as a passenger (caller 14%07llX); driver setup skipped", id, static_cast<unsigned long long>(caller));
						return;
					}
					logger::Write("Boarding gate: %08X passenger seat could not be finished; occupied driver seat preserved", id);
					return;
				}
				logger::Write("Boarding gate: player completion %08X (caller 14%07llX); native completion retained", id, static_cast<unsigned long long>(caller));
				g_seatPlayerOriginal(vehicle);
			}, [&] {
				const uptr actor = g_sitContext.actor;
				const int result = SeatNpc(actor, vehicle);
				logger::Write("Boarding gate: NPC %08X completes its own sit in %08X -> %d; player state untouched",
					At<std::uint32_t>(actor, off::kFormId), id, result);
			});
			if (completion == boardinggate::Completion::Suppress)
				logger::Write("Boarding gate: unscoped callback for restricted vehicle %08X (caller 14%07llX) suppressed; no actor replaced or cleaned up", id, static_cast<unsigned long long>(caller));
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
			// In combat: no calm up front any more (2026-10-21, the user: make the get-in the highest priority instead). The get-in
			// packages carry Ignore Combat (FVO_NPCPilots.esm); the 1 s check in Boarding calms only if the package still did not take over.
			if(seat)slot.resumeFoe=BoardingEnemy(npc);
			if (seat && InCombat(npc)) logger::Write("NPC vehicles: [%d] NPC %08X is in combat; get-in with Ignore Combat (no calm)", slot.pilot, npcId);
			const int r = seat ? Mount(npc, vehicle) : 0;
			slot.standing = r == 2;
			const uptr base = At<uptr>(vehicle, 0x98);
			logger::Write("NPC vehicles: [%d] NPC %08X -> vehicle %08X (base %08X, %.0f m from it); seat %s; get-in -> %d", slot.pilot, npcId,
				At<std::uint32_t>(vehicle, off::kFormId), base ? At<std::uint32_t>(base, off::kFormId) : 0, Dist(npc, vehicle), seat ? "free" : "REFUSED", r);
			if (r < 1) { slot.npc=npcId;ResumeCombat(slot);return false; }
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
			if(!slot||!npc||!vehicle)return false;
			// The Walk key: the game's own follower command. The job only watches (no instant seat); Enter and the seated check follow.
			// Native, data-driven: FVO_NPCPilots.esm's quest package (any actor); else the follower command (followers only).
			// Context: in combat the NPC RUNS to the vehicle, still hostile (the package's Ignore Combat outranks the fight; calm is the 1 s fallback); otherwise it walks at the package's own pace.
			slot->resumeFoe=BoardingEnemy(npc);
			if (InCombat(npc)) {
				// Still hostile: the get-in package's Ignore Combat outranks the fight; the 1 s check in Boarding calms only as a fallback.
				SetForceRun(npc, true);
				slot->forcedRun = true;
				logger::Write("NPC vehicles: [%d] NPC %08X is in combat: running to the vehicle (Ignore Combat, no calm)", slot->pilot, At<std::uint32_t>(npc, off::kFormId));
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
			// In combat (a driver): the combat AI's own path-and-activate first; the get-in package (Ignore Combat, then calm) is its fallback.
			uptr combatReq = 0;
			if (InCombat(npc) && !seat) {
				const char* why = "";
				combatReq = CombatBoardStart(npc, vehicle, why);
				logger::Write("NPC vehicles: [%d] NPC %08X in combat: %s%s", slot->pilot, At<std::uint32_t>(npc, off::kFormId),
					combatReq ? "boarding as a combat move (CombatMovementRequestActivateTarget)" : "combat move not possible, get-in package: ", combatReq ? "" : why);
			}
			const bool viaQuest = combatReq || pilotquest::Assign(slot->pilot - 1, npc, vehicle);
			if (!viaQuest) {
				logger::Write("FVO_NPCPilots.esm is not loaded: NPCs cannot be sent to vehicles");
                slot->npc=At<std::uint32_t>(npc,off::kFormId); ResumeCombat(*slot);
                if(slot->forcedRun){SetForceRun(npc,false);slot->forcedRun=false;}
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
			slot->combatReq = combatReq;
			slot->combatReqAt = GetTickCount64();
			slot->combatDoneAt = 0;
			slot->combatReqState = -1;
			slot->combatSeatTried = false;
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
				logger::Write("%d NPC vehicles already busy", kJobs);
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
				logger::Write(!npc ? "No NPC in front of you" : "No free vehicle nearby");
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
				logger::Write("No free vehicle nearby");
				return;
			}
			logger::Write("NPC vehicles: [%d] force: NPC %08X (%s, %.0f m in front of you, %.0f deg off)", slot->pilot, At<std::uint32_t>(npc, off::kFormId),
				modscene::RttiName(npc), npcDist, npcOff * 57.2958);
			if (!StartWalkBoard(slot, npc, vehicle)) return;
#ifndef FVO_RELEASE
            g_testPairVehicle=slot->vehicle;g_testPairDriver=slot->npc;
            logger::Write("OBS system=pair.move_test event=pair_selected source=debug_board vehicle=%08X driver=%08X",
                g_testPairVehicle,g_testPairDriver);
#endif
			logger::Write("NPC sent to a vehicle (%.0f m)", bestV);
		}

#ifndef FVO_RELEASE
        struct FastSeatRun {
            unsigned vehicle=0, driver=0, base=0, frame=0, receiptFrame=0, attempts=0;
            std::uint64_t serial=0, requested=0, first3D=0, firstProcess=0, firstLink=0, firstSeat=0, seatedSince=0, lastSnap=0;
            std::uint64_t lease=0, firstSpot=0, lastPilotAsk=0;
            trafficpair::SeatPose offset{};
        } g_fastSeat;
        struct FastSeatEvidence {
            trafficpair::SeatPose offset{};
            unsigned count=0;
            std::uint64_t total=0, fastest=0, slowest=0;
        };
        std::map<unsigned,FastSeatEvidence> g_fastSeatEvidence;
        std::map<unsigned,unsigned> g_fastSeatDrivers;
        std::uint64_t g_fastSeatSerial=0;
        unsigned g_fastSeatMoveVehicle=0, g_fastSeatMoveDriver=0;
        std::uint64_t g_fastSeatMoveLease=0;
        constexpr std::uint64_t kFastSeatLimitMs=20000, kFastSeatRetryMs=250;

        void FastSeatStart(uptr player)
        {
            const auto vehicle=FacingVehicle(player);
            const auto id=vehicle?At<unsigned>(vehicle,off::kFormId):0;
            const auto prev=g_fastSeatDrivers.find(id);
            const auto previous=prev!=g_fastSeatDrivers.end()?RefById(prev->second):0;
            const char* why=g_fastSeat.vehicle?"trial_pending":!id?"no_faced_vehicle":
                previous&&!IsDead(previous)?"test_actor_already_exists":
                (!Has3D(vehicle)||!CellAttached(vehicle)||!DriverSeatKeyword(vehicle))?"no_loaded_driver_seat":
                (Busy(id)||UsesVehicle(id)||DriverOf(vehicle,player)||trafficpool::MadeVehicle(id)||
                 trafficpool::PlayerOwnsHandoff(id))?"vehicle_occupied_or_claimed":nullptr;
            if(why){
                logger::Write("OBS system=seat.fast event=refused vehicle=%08X reason=%s",id,why);
                notify::Show("Fast seat test needs a nearby empty vehicle");
                return;
            }
            if(g_fastSeatMoveLease)CancelFactoryLease(g_fastSeatMoveVehicle,g_fastSeatMoveDriver,g_fastSeatMoveLease);
            g_fastSeatMoveVehicle=g_fastSeatMoveDriver=0;g_fastSeatMoveLease=0;
            g_fastSeat={};
            auto& t=g_fastSeat;
            t.vehicle=id;
            if(const auto base=At<uptr>(vehicle,0x98))t.base=At<unsigned>(base,off::kFormId);
            t.serial=++g_fastSeatSerial;
            t.requested=GetTickCount64();
            const auto learned=g_fastSeatEvidence.find(t.base);
            t.offset=learned==g_fastSeatEvidence.end()?trafficpair::SeatPose{0,0,.3f}:learned->second.offset;
            logger::Write("OBS system=seat.fast event=request trial=%llu vehicle=%08X base=%08X offset_source=%s",
                t.serial,id,t.base,learned==g_fastSeatEvidence.end()?"chassis_origin":"measured_seat");
        }

        void FastSeatSnap(uptr actor,uptr vehicle,const FastSeatRun& t)
        {
            lifecycle::Post("fast-seat-snap:"+std::to_string(t.serial),t.driver,[=]()->lifecycle::Work {
                const auto a=lifecycle::Keep(actor,t.driver),v=lifecycle::Keep(vehicle,t.vehicle);
                if(!a||!v)return {};
                return [=]()->std::uint64_t {
                    const auto driver=reinterpret_cast<uptr>(a.get()),chassis=reinterpret_cast<uptr>(v.get());
                    if(!lifecycle::InService()||IsDead(driver)||Disabled(chassis)||trafficpool::PlayerOwnsHandoff(t.vehicle))return 0;
                    const auto linked=InVehicle(driver);
                    if((linked&&linked!=chassis)||ProcessLevel(driver)!=0||!Has3D(driver)||!Has3D(chassis))return 0;
                    if(SeatedNow(driver,chassis,linked==chassis))return 1;
                    if(!trafficpair::NeedsSnap(true,true,SitState(driver),SeatDist(driver,chassis)))return 0;
                    const auto result=SeatNewborn(driver,chassis);
                    logger::Write("OBS system=seat.fast event=snap trial=%llu vehicle=%08X driver=%08X attempt=%u elapsed_ms=%llu result=%d sit=%d",
                        t.serial,t.vehicle,t.driver,t.attempts,GetTickCount64()-t.requested,result,SitState(driver));
                    return result>0;
                };
            });
        }

        void FastSeatTick(uptr player)
        {
            auto& t=g_fastSeat;
            if(!t.vehicle)return;
            const auto now=GetTickCount64();
            ++t.frame;
            if(!t.driver){
                const auto receipt=trafficpool::FastSeatBirth(t.vehicle,t.serial,t.offset,t.requested);
                if(!receipt)return;
                t.driver=unsigned(*receipt);
                if(!t.driver){
                    logger::Write("OBS system=seat.fast event=refused trial=%llu vehicle=%08X reason=birth_service_refused",t.serial,t.vehicle);
                    notify::Show("Fast seat test could not create the driver");
                    t={};return;
                }
                g_fastSeatDrivers[t.vehicle]=t.driver;
                t.receiptFrame=t.frame;
            }
            const auto v=RefById(t.vehicle),a=RefById(t.driver);
            if(a&&Has3D(a)&&!t.first3D)t.first3D=now;
            if(a&&ProcessLevel(a)==0&&!t.firstProcess)t.firstProcess=now;
            const bool linked=a&&v&&InVehicle(a)==v;
            if(linked&&!t.firstLink)t.firstLink=now;
            const bool seated=linked&&SeatedNow(a,v,true);
            if(seated){
                if(!t.seatedSince)t.seatedSince=now;
                if(!t.firstSeat){
                    t.firstSeat=now;
                    g_testPairVehicle=t.vehicle;g_testPairDriver=t.driver;
                    logger::Write("OBS system=seat.fast event=first_seated trial=%llu vehicle=%08X driver=%08X elapsed_ms=%llu frames=%u frames_after_receipt=%u snaps=%u",
                        t.serial,t.vehicle,t.driver,now-t.requested,t.frame,t.frame-t.receiptFrame,t.attempts);
                    notify::Show("Driver seated in %llu ms. Look away for the spot test",now-t.requested);
                }
                if(!t.lease&&!Busy(t.vehicle)&&!InVehicle(player)&&now-t.lastPilotAsk>=kFastSeatRetryMs){
                    t.lastPilotAsk=now;
                    t.lease=StartFactoryStockLease(a,v);
                    if(t.lease)logger::Write("OBS system=seat.fast event=spot_requested trial=%llu vehicle=%08X elapsed_ms=%llu lease=%llu",
                        t.serial,t.vehicle,now-t.requested,t.lease);
                }
                if(t.lease){
                    const auto pilot=PilotFor(t.vehicle);
                    if(vehicleflight::HandoffPhysics(t.vehicle).live)FactoryLeaseReleased(t.vehicle);
                    if(!t.firstSpot&&pilot>0&&vehicleflight::FactoryMotionProved(pilot,t.vehicle,t.lease)){
                        t.firstSpot=now;
                        logger::Write("OBS system=seat.fast event=spot_proved trial=%llu vehicle=%08X elapsed_ms=%llu",
                            t.serial,t.vehicle,now-t.requested);
                    }
                }
            }else {
                t.seatedSince=0;
                if(a&&v&&t.first3D&&ProcessLevel(a)==0&&t.attempts<6&&now-t.lastSnap>=kFastSeatRetryMs&&
                   !lifecycle::Pending(t.driver,"fast-seat-snap")){
                    t.lastSnap=now;
                    ++t.attempts;
                    FastSeatSnap(a,v,t);
                }
            }
            const bool held=trafficpair::ProveHeld(t.seatedSince,now);
            if((!held||!t.firstSpot)&&now-(t.firstSeat?t.firstSeat:t.requested)<kFastSeatLimitMs)return;
            const auto elapsed=[&](std::uint64_t stage){return stage?static_cast<long long>(stage-t.requested):-1ll;};
            logger::Write("OBS system=seat.fast event=complete trial=%llu vehicle=%08X driver=%08X held=%d first_3d_ms=%lld first_process_ms=%lld first_link_ms=%lld first_seated_ms=%lld spot_proved_ms=%lld total_ms=%llu",
                t.serial,t.vehicle,t.driver,int(held),elapsed(t.first3D),elapsed(t.firstProcess),elapsed(t.firstLink),elapsed(t.firstSeat),elapsed(t.firstSpot),now-t.requested);
            if(held&&t.firstSpot){
                g_fastSeatMoveVehicle=t.vehicle;g_fastSeatMoveDriver=t.driver;g_fastSeatMoveLease=t.lease;
                notify::Show("Spot test passed. Numpad 3 moves the seated pair");
            }else {
                if(t.lease)CancelFactoryLease(t.vehicle,t.driver,t.lease);
                notify::Show("Fast seat test timed out. See seat.fast in the log");
            }
            game::Vec3 da{},dv{};
            if(held&&t.base&&ScenePos(a,da)&&ScenePos(v,dv)){
                auto& evidence=g_fastSeatEvidence[t.base];
                const auto heading=At<game::Vec3>(v,off::kRefRotation).z;
                const float dx=da.x-dv.x,dy=da.y-dv.y,dz=da.z-dv.z;
                if(std::hypot(dx,dy,dz)<=trafficpair::kSnapReach){
                    const float c=std::cos(heading),s=std::sin(heading);
                    evidence.offset={dx*c-dy*s,dx*s+dy*c,dz};
                }else if(!evidence.count)evidence.offset=t.offset;
                const auto seat=t.firstSeat-t.requested;
                evidence.fastest=evidence.count?std::min(evidence.fastest,seat):seat;
                evidence.slowest=std::max(evidence.slowest,seat);
                evidence.total+=seat;
                ++evidence.count;
                logger::Write("OBS system=seat.fast event=model_timing base=%08X samples=%u min_ms=%llu mean_ms=%llu max_ms=%llu",
                    t.base,evidence.count,evidence.fastest,evidence.total/evidence.count,evidence.slowest);
            }
            t={};
        }

        // ---- Seated-pair relocation experiment (diagnostic builds only; notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md) ----
        // Numpad 3 relocates the seated NPC you face (else the pair made with the board key) 6 m ahead along its vehicle's heading.
        // Roles: this file is the Coordinator (relocation.h: grant, receipt, one delivery to every consumer); vehicleflight::relocate is the
        // doer (one substitution of the chassis pose at one physics step, then facts). Nothing else decides here, and nothing moves the actor.
        relocation::Coordinator g_reloc;
        std::uint64_t g_relocEpoch = 1, g_relocSerial = 0, g_relocArmedAt = 0, g_relocPendingLogAt = 0;
        std::uint32_t g_relocVehicle = 0, g_relocDriver = 0;
        struct StockTransferGrant {
            std::uint32_t driver=0;
            std::uint64_t lease=0,serial=0;
            relocation::Pose target{};
        };
        std::map<unsigned,StockTransferGrant> g_stockTransfers;
        std::atomic<std::uint64_t> g_relocPose{1ull << 32}; // high word generation, low word vehicle with exclusive pose authority
        bool g_relocIssued = false, g_relocSubscribed = false;
        // Long and cross-cell moves (2026-11-20): the doer writes one bounded hop at a time (relocation.h kMaxHopM); this Coordinator plans the hops from
        // the measured position, reads each back before the next, walks a failed move back to its origin when the pair is still one live seated pair, and
        // for a cross-cell move waits until the chassis reference is listed in the destination cell (the existing loadedarea refile) before it publishes.
        relocation::Pose g_relocFrom{}, g_relocTo{}, g_relocHop{}, g_relocLastChassis{}, g_relocArrived{};
        bool g_relocHopLast = false, g_relocFirstIssued = false, g_relocRestoring = false, g_relocCross = false, g_relocSettling = false, g_relocPoseReleased = false;
        const char* g_relocFailWhy = "";
        std::uint64_t g_relocAuthority = 0, g_relocSettleAt = 0, g_relocSettleLogAt = 0, g_relocRefileAt = 0;
        // Consumers that compare positions over time (the drive-mode speed estimate, the plan's object memory) ask whether this vehicle's pose was
        // moved: the mark changes when a relocation starts and when it ends, and its low bit says one is in progress.
        std::mutex g_relocMarkLock;
        std::map<std::uint32_t, std::uint64_t> g_relocMarks;
        std::uint64_t g_relocMarkCounter = 0;
        void NoteRelocationMark(std::uint32_t vehicle, bool active)
        {
            std::lock_guard lock(g_relocMarkLock);
            g_relocMarks[vehicle] = (++g_relocMarkCounter << 1) | (active ? 1u : 0u);
        }
        void SetRelocationPoseOwner(std::uint32_t vehicle)
        {
            const auto generation = (g_relocPose.load(std::memory_order_relaxed) >> 32) + 1;
            g_relocPose.store((generation << 32) | vehicle, std::memory_order_release);
        }
        void ReleaseRelocationPose()
        {
            if (g_relocPoseReleased) return;
            g_relocPoseReleased = true;
            SetRelocationPoseOwner(0);
        }
        void FinishRelocation()
        {
            if(g_relocSerial){vehicleflight::relocate::Disarm(g_relocSerial);NoteRelocationMark(g_relocVehicle,false);}
            g_relocSerial=0;
            g_relocIssued=false;
            g_relocFirstIssued=g_relocRestoring=g_relocCross=g_relocSettling=g_relocHopLast=false;
            g_relocFailWhy="";g_relocAuthority=0;
            if(!g_relocPoseReleased)SetRelocationPoseOwner(0);
            g_relocPoseReleased=false;
        }
        struct PairMoveObservation {
            std::uint64_t serial = 0, startedAt = 0, nextSampleAt = 0, initialAutopilotStep = 0;
            std::uint32_t vehicle = 0, driver = 0;
            relocation::Pose arrival;
            bool hasStepBaseline = false;
        };
        PairMoveObservation g_pairMoveObservation;
        std::uint64_t g_pairMoveReceipt = 0;
        constexpr std::uint64_t kPairMoveObserveMs = 10000, kPairMoveSampleMs = 200;
        constexpr double kPairMoveProofM = 1.0;

        bool RelocLog(const relocation::Receipt& r, void*)
        {
            logger::Write("OBS system=relocation event=published serial=%llu epoch=%llu vehicle=%08X driver=%08X result=%s reason=%s from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f measured=%.2f,%.2f,%.2f displacement=%.2f step_before=%llu step_after=%llu seat_before=%d seat_after=%d live=%d",
                (unsigned long long)r.serial, (unsigned long long)r.epoch, r.vehicle, r.driver, relocation::ResultName(r.result), r.reason,
                r.from.x, r.from.y, r.from.z, r.to.x, r.to.y, r.to.z, r.measured.x, r.measured.y, r.measured.z, r.displacement,
                (unsigned long long)r.stepBefore, (unsigned long long)r.stepAfter, r.seatedBefore ? 1 : 0, r.seatedAfter ? 1 : 0, r.physicsLive ? 1 : 0);
            if (r.serial == g_pairMoveReceipt) {
                g_pairMoveReceipt = 0;
                if (r.result == relocation::Result::Applied) {
                    const auto autopilot = vehicleflight::HandoffPhysics(r.vehicle);
                    g_pairMoveObservation = {r.serial, r.publishedAtMs, r.publishedAtMs, autopilot.serial,
                        r.vehicle, r.driver, r.measured, autopilot.bound};
                }
            }
            return true;
        }
        // The seated NPC in front of the player (the same facing rule as the board key): the actor and the vehicle it sits in.
        bool FacedSeatedPair(uptr player, uptr& npc, uptr& vehicle)
        {
            const auto refs = modscene::LoadedRefs(8000);
            const auto pp = At<game::Vec3>(player, off::kRefPosition);
            const double facing = At<game::Vec3>(player, off::kRefRotation).z;
            double best = 1e9;
            for (const uptr r : refs) {
                if (r == player || Disabled(r) || !Has3D(r) || !IsActorRef(r) || Dead(r)) continue;
                const uptr v = InVehicle(r);
                if (!v) continue;
                const auto q = At<game::Vec3>(r, off::kRefPosition);
                const double dx = double(q.x) - pp.x, dy = double(q.y) - pp.y, d = std::hypot(dx, dy);
                if (d < 0.5 || d > kFaceRange) continue;
                double off = std::atan2(dx, dy) - facing;
                while (off > 3.14159265) off -= 6.28318531;
                while (off < -3.14159265) off += 6.28318531;
                off = std::fabs(off);
                if (off > kFaceCone) continue;
                const double score = off * 30.0 + d;
                if (score < best) { best = score; npc = r; vehicle = v; }
            }
            return npc != 0;
        }
        void RelocationRefuse(std::uint32_t v, std::uint32_t d, const char* reason)
        {
            logger::Write("OBS system=relocation event=refused vehicle=%08X driver=%08X reason=%s", v, d, reason);
        }
        std::uint64_t ArmRelocation(relocation::Request q,const char* source,std::uint64_t authority=0)
        {
            if(g_relocSerial){RelocationRefuse(q.vehicle,q.driver,"previous_relocation_still_running");return 0;}
            q.epoch=g_relocEpoch;
            q.seatedBefore=true;
            const double dx=q.to.x-q.from.x,dy=q.to.y-q.from.y,dz=q.to.z-q.from.z;
            const double distance=std::sqrt(dx*dx+dy*dy+dz*dz);
            {
                const char* acceptable="";
                if(!relocation::MoveAcceptable(distance,acceptable)){
                    logger::Write("OBS system=relocation event=refused vehicle=%08X driver=%08X reason=%s distance=%.1f limit=%.0f source=%s",
                        q.vehicle,q.driver,acceptable,distance,relocation::kMaxMoveM,source);
                    return 0;
                }
            }
            // Cross-cell: the destination must be in a loaded cell, else the chassis reference could not be refiled and would stand in a cell it is not listed in.
            loadedarea::RelocationCell cell{};
            if(loadedarea::RelocationDestination(q.vehicle,float(q.to.x),float(q.to.y),cell)){
                q.crossCell=cell.crossCell;
                if(cell.crossCell&&!cell.cellLoaded){
                    logger::Write("OBS system=relocation event=refused vehicle=%08X driver=%08X reason=destination_cell_not_loaded from_cell=%d,%d to_cell=%d,%d distance=%.1f source=%s",
                        q.vehicle,q.driver,cell.fromX,cell.fromY,cell.toX,cell.toY,distance,source);
                    return 0;
                }
            }
            if(!g_relocSubscribed){g_reloc.Subscribe("log",RelocLog,nullptr);g_relocSubscribed=true;}
            const char* why="";
            const auto serial=g_reloc.Arm(q,g_relocEpoch,why);
            if(!serial){RelocationRefuse(q.vehicle,q.driver,why);return 0;}
            const auto now=GetTickCount64();
            logger::Write("OBS system=relocation event=grant serial=%llu epoch=%llu vehicle=%08X driver=%08X from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f delta=%.1f,%.1f,%.1f distance=%.1f hops=%u cross_cell=%d from_cell=%d,%d to_cell=%d,%d source=%s",
                static_cast<unsigned long long>(serial),static_cast<unsigned long long>(q.epoch),q.vehicle,q.driver,
                q.from.x,q.from.y,q.from.z,q.to.x,q.to.y,q.to.z,dx,dy,dz,distance,relocation::HopCount(distance),q.crossCell?1:0,
                cell.fromX,cell.fromY,cell.toX,cell.toY,source);
            g_relocPoseReleased=false;
            SetRelocationPoseOwner(q.vehicle);
            // The first hop starts from the vehicle reference (as the proven 6 m move did); later hops start from the measured chassis.
            bool last=false;
            const auto hop=relocation::NextHop(q.from,q.to,last);
            if(const char* refusal=vehicleflight::relocate::Arm(q.vehicle,hop.x-q.from.x,hop.y-q.from.y,hop.z-q.from.z,serial,authority)){
                g_reloc.Refuse(serial,refusal,now);
                SetRelocationPoseOwner(0);
                return 0;
            }
            g_relocSerial=serial;g_relocVehicle=q.vehicle;g_relocDriver=q.driver;g_relocArmedAt=now;
            g_relocIssued=false;g_relocPendingLogAt=0;
            g_relocFrom=q.from;g_relocTo=q.to;g_relocHop=hop;g_relocHopLast=last;g_relocAuthority=authority;g_relocLastChassis=q.from;
            g_relocFirstIssued=g_relocRestoring=g_relocSettling=false;g_relocCross=q.crossCell;g_relocFailWhy="";
            NoteRelocationMark(q.vehicle,true);
            return serial;
        }
        void RelocationStart(uptr player)
        {
            if (g_relocSerial) { RelocationRefuse(g_relocVehicle, g_relocDriver, "previous_relocation_still_running"); return; }
            uptr d = 0, v = 0;
            if (!FacedSeatedPair(player, d, v)) {
                v = RefById(g_testPairVehicle); d = RefById(g_testPairDriver);
                if (!v || !d || InVehicle(d) != v) { RelocationRefuse(g_testPairVehicle, g_testPairDriver, "no_seated_npc_in_front_and_no_board_pair"); return; }
            }
            const std::uint32_t vid = At<std::uint32_t>(v, off::kFormId), did = At<std::uint32_t>(d, off::kFormId);
            if(const auto test=g_fastSeatDrivers.find(vid);test!=g_fastSeatDrivers.end()&&test->second==did&&
               (g_fastSeatMoveVehicle!=vid||g_fastSeatMoveDriver!=did||!CheckFactoryLease(vid,did,g_fastSeatMoveLease))){
                RelocationRefuse(vid,did,"fast_seat_spot_not_proved");return;
            }
            if (Disabled(v) || trafficpool::MadeVehicle(vid) || trafficpool::PlayerOwnsHandoff(vid) || VehicleInCombat(vid)) { RelocationRefuse(vid, did, "vehicle_unavailable"); return; }
            if (!SeatedNow(d, v, true)) { RelocationRefuse(vid, did, "driver_not_seated"); return; }
            const auto vp = At<game::Vec3>(v, off::kRefPosition), vr = At<game::Vec3>(v, off::kRefRotation);
            const double dx = std::sin(vr.z) * 6.0, dy = std::cos(vr.z) * 6.0, dz = 0.3;
            const auto serial = ArmRelocation({vid,did,{vp.x,vp.y,vp.z,vr.z},{vp.x+dx,vp.y+dy,vp.z+dz,vr.z}},
                vid==g_testPairVehicle?"board_pair":"faced_seated_npc");
            if (serial) g_pairMoveReceipt = serial;
        }
        // The read-back of the pair now, from the doer's last write (body record, completed steps) and the game's references.
        bool RelocationReadback(relocation::Readback& rb, uptr& v, uptr& d, double& seatDist)
        {
            v = RefById(g_relocVehicle); d = RefById(g_relocDriver);
            vehicleflight::relocate::Chassis ch;
            const bool have = vehicleflight::relocate::Read(g_relocSerial, ch);
            rb = {};
            rb.bodyValid = have && ch.bodyValid && v && d;
            rb.chassis = { ch.pos[0], ch.pos[1], ch.pos[2], 0 };
            if (d) { const auto dp = At<game::Vec3>(d, off::kRefPosition); rb.driver = { dp.x, dp.y, dp.z, 0 }; }
            rb.aboardAfter = d && v && InVehicle(d) == v;
            rb.seatedAfter = rb.aboardAfter && SeatedNow(d, v, true);
            rb.physicsLive = have && ch.live;
            rb.driverPoseKnown = false;   // the stored actor position may lag its seat: until the actor's own body is read, the seat link is the evidence
            rb.stepAfter = have ? ch.completedSteps : 0;
            seatDist = (d && v) ? SeatDist(d, v) : -1.0;
            if (have && ch.bodyValid) g_relocLastChassis = rb.chassis;
            return have;
        }
        // Arm the next hop from the measured chassis toward `goal` (the destination, or the origin when walking back). False: the doer refused.
        bool RelocationArmHop(const relocation::Pose& at, const relocation::Pose& goal, const char* why)
        {
            vehicleflight::relocate::Disarm(g_relocSerial);   // the previous hop's facts are consumed; Arm needs an idle doer
            bool last = false;
            const auto hop = relocation::NextHop(at, goal, last);
            const char* refusal = vehicleflight::relocate::Arm(g_relocVehicle, hop.x - at.x, hop.y - at.y, hop.z - at.z, g_relocSerial, g_relocAuthority);
            logger::Write("OBS system=relocation event=hop_armed serial=%llu vehicle=%08X driver=%08X hop=%u phase=%s last=%d from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f remaining=%.1f result=%s reason=%s",
                (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, g_reloc.Hops(g_relocSerial) + 1, g_relocRestoring ? "restore" : "forward", last ? 1 : 0,
                at.x, at.y, at.z, hop.x, hop.y, hop.z, relocation::Distance(at, goal), refusal ? "refused" : "armed", refusal ? refusal : why);
            if (refusal) return false;
            g_relocHop = hop; g_relocHopLast = last; g_relocArmedAt = GetTickCount64(); g_relocIssued = false; g_relocPendingLogAt = 0;
            return true;
        }
        // The move cannot go on. A pair that is still one live seated pair is walked back to where it was; anything else is published failed with the
        // last observed state. Single-hop moves keep the proven behaviour (Verify publishes their failure) and never reach here.
        void RelocationHopFailed(const relocation::Readback& rb, const char* why, ULONGLONG now)
        {
            logger::Write("OBS system=relocation event=hop_failed serial=%llu vehicle=%08X driver=%08X hops=%u phase=%s reason=%s recoverable=%d chassis=%.2f,%.2f,%.2f steps=%llu seat=%d live=%d body_valid=%d",
                (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, g_reloc.Hops(g_relocSerial), g_relocRestoring ? "restore" : "forward", why,
                relocation::Recoverable(rb) ? 1 : 0, rb.chassis.x, rb.chassis.y, rb.chassis.z, (unsigned long long)rb.stepAfter, rb.seatedAfter ? 1 : 0, rb.physicsLive ? 1 : 0, rb.bodyValid ? 1 : 0);
            if (g_relocRestoring) { g_reloc.Conclude(g_relocSerial, rb, false, "hop_failed_restore_failed", now); FinishRelocation(); return; }
            if (!relocation::Recoverable(rb)) { g_reloc.Conclude(g_relocSerial, rb, false, why, now); FinishRelocation(); return; }
            g_relocRestoring = true; g_relocFailWhy = why;
            if (!RelocationArmHop(rb.chassis, g_relocFrom, "walking_back_after_failed_hop")) {
                g_reloc.Conclude(g_relocSerial, rb, false, "hop_failed_restore_refused", now); FinishRelocation();
            }
        }
        void RelocationTick()
        {
            if (!g_relocSerial) return;
            const ULONGLONG now = GetTickCount64();
            relocation::Readback rb; uptr v = 0, d = 0; double seatDist = -1;
            if (g_relocSettling) {
                // Cross-cell: the chassis is at the destination and the doer is idle. The existing refile (loadedarea) lists the reference in the destination
                // cell; the receipt waits until it shows, with the seat link and both models verified after it.
                RelocationReadback(rb, v, d, seatDist);
                rb.chassis = g_relocArrived;   // the chassis position the last hop was judged at: a car that drives on while the refile settles is not "left behind"
                const auto* act = g_reloc.Lookup(g_relocSerial);
                const char* judged = "";
                const auto res = act ? relocation::Judge(act->req, rb, judged) : relocation::Result::Failed;
                loadedarea::RelocationSettle st{};
                const bool known = loadedarea::RelocationSettled(g_relocVehicle, float(g_relocTo.x), float(g_relocTo.y), float(g_relocTo.z), st);
                const bool models = v && d && Has3D(v) && Has3D(d);
                const bool settled = known && st.atTarget && st.inDestinationCell && models;
                if (now - g_relocSettleLogAt >= 500) {
                    g_relocSettleLogAt = now;
                    logger::Write("OBS system=relocation event=settle serial=%llu vehicle=%08X driver=%08X ref_known=%d at_target=%d gap=%.2f listed_grid=%d in_destination_cell=%d models=%d aboard=%d seated=%d seat_dist=%.2f judge=%s waited_ms=%llu",
                        (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, known ? 1 : 0, st.atTarget ? 1 : 0, st.gap, st.listedGrid ? 1 : 0, st.inDestinationCell ? 1 : 0,
                        models ? 1 : 0, rb.aboardAfter ? 1 : 0, rb.seatedAfter ? 1 : 0, seatDist, judged, (unsigned long long)(now - g_relocSettleAt));
                }
                if (res == relocation::Result::Failed) {   // the pair broke while settling: Verify publishes failed with Judge's own reason
                    relocation::Receipt out; g_reloc.Verify(g_relocSerial, rb, now, out); FinishRelocation(); return;
                }
                if (settled && res == relocation::Result::Applied) {
                    rb.refiled = true;
                    relocation::Receipt out;
                    if (g_reloc.Verify(g_relocSerial, rb, now, out)) FinishRelocation();
                    return;
                }
                if (now - g_relocSettleAt > relocation::kSettleWindowMs) {
                    g_reloc.Conclude(g_relocSerial, rb, false, "refile_not_settled", now); FinishRelocation(); return;
                }
                if (!known || !st.inDestinationCell || !st.atTarget) {
                    if (now - g_relocRefileAt >= 500) { g_relocRefileAt = now; loadedarea::RefileRelocated(g_relocVehicle, g_relocDriver, float(g_relocTo.x), float(g_relocTo.y), float(g_relocTo.z)); }
                }
                return;
            }
            if (!g_relocIssued) {
                vehicleflight::relocate::Facts f;
                if (vehicleflight::relocate::Take(g_relocSerial, f)) {
                    if (f.issued) {
                        if (!g_relocFirstIssued) { g_reloc.MarkIssued(g_relocSerial, now); g_relocFirstIssued = true; }
                        else g_reloc.HopIssued(g_relocSerial, now);
                        g_relocIssued = true;
                        logger::Write("OBS system=relocation event=issued serial=%llu vehicle=%08X driver=%08X hop=%u phase=%s ring_first_ok=%d com_near_body=%d speed=%.2f com_before=%.3f,%.3f,%.3f com_after=%.3f,%.3f,%.3f body_before=%.3f,%.3f,%.3f body_after=%.3f,%.3f,%.3f",
                            (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, g_reloc.Hops(g_relocSerial), g_relocRestoring ? "restore" : "forward", f.ringFirstOk ? 1 : 0, f.comNearBody ? 1 : 0, f.speed,
                            f.comBefore[0], f.comBefore[1], f.comBefore[2], f.comAfter[0], f.comAfter[1], f.comAfter[2],
                            f.bodyBefore[0], f.bodyBefore[1], f.bodyBefore[2], f.bodyAfter[0], f.bodyAfter[1], f.bodyAfter[2]);
                    } else if (!g_relocFirstIssued) {
                        g_reloc.Refuse(g_relocSerial, f.refusal, now);
                        FinishRelocation(); return;
                    } else {   // a later hop refused by the doer's own checks: the pair already moved, so it is judged like a failed hop
                        RelocationReadback(rb, v, d, seatDist);
                        RelocationHopFailed(rb, f.refusal, now); return;
                    }
                } else if (now - g_relocArmedAt > 3000) {
                    if (!g_relocFirstIssued) {
                        g_reloc.Refuse(g_relocSerial, "not_issued_in_time", now);   // the chassis took no physics step in 3 s (asleep, or not owned by a pilot)
                        FinishRelocation(); return;
                    }
                    RelocationReadback(rb, v, d, seatDist);
                    RelocationHopFailed(rb, "hop_not_issued_in_time", now); return;
                } else return;
            }
            RelocationReadback(rb, v, d, seatDist);
            if (now - g_relocPendingLogAt >= 500) {
                g_relocPendingLogAt = now;
                const auto vp = v ? At<game::Vec3>(v, off::kRefPosition) : game::Vec3{};
                logger::Write("OBS system=relocation event=pending serial=%llu vehicle=%08X driver=%08X hop=%u phase=%s steps=%llu live=%d body_valid=%d chassis=%.2f,%.2f,%.2f vehicle_ref=%.2f,%.2f,%.2f driver_ref=%.2f,%.2f,%.2f aboard=%d seated=%d seat_dist=%.2f",
                    (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, g_reloc.Hops(g_relocSerial), g_relocRestoring ? "restore" : "forward", (unsigned long long)rb.stepAfter, rb.physicsLive ? 1 : 0, rb.bodyValid ? 1 : 0,
                    rb.chassis.x, rb.chassis.y, rb.chassis.z, vp.x, vp.y, vp.z, rb.driver.x, rb.driver.y, rb.driver.z, rb.aboardAfter ? 1 : 0, rb.seatedAfter ? 1 : 0, seatDist);
            }
            const relocation::Pose& goal = g_relocRestoring ? g_relocFrom : g_relocTo;
            const bool singleHop = g_reloc.Hops(g_relocSerial) <= 1 && g_relocHopLast && !g_relocRestoring;
            if (!g_relocRestoring && g_relocHopLast) {
                // The last hop of the move: the whole-move judgement (chassis at the target, seat link, live physics, a fresh step) as in the proven 6 m move.
                const auto* act = g_reloc.Lookup(g_relocSerial);
                const char* judged = "";
                const auto res = act ? relocation::Judge(act->req, rb, judged) : relocation::Result::Failed;
                if (res == relocation::Result::None) {
                    relocation::Receipt out;
                    if (g_reloc.Verify(g_relocSerial, rb, now, out)) FinishRelocation();   // the window ran out without a completed step
                    return;
                }
                if (res == relocation::Result::Applied && g_relocCross) {
                    // The doer is done. Release the pose so the existing refile may run (it yields to the doer), queue it now, then wait for it.
                    g_relocSettling = true; g_relocArrived = rb.chassis; g_relocSettleAt = now; g_relocSettleLogAt = 0; g_relocRefileAt = now;
                    ReleaseRelocationPose();
                    const bool queued = loadedarea::RefileRelocated(g_relocVehicle, g_relocDriver, float(g_relocTo.x), float(g_relocTo.y), float(g_relocTo.z));
                    logger::Write("OBS system=relocation event=settle_begin serial=%llu vehicle=%08X driver=%08X hops=%u refile_queued=%d to=%.2f,%.2f,%.2f reason=cross_cell_refile_before_receipt",
                        (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, g_reloc.Hops(g_relocSerial), queued ? 1 : 0, g_relocTo.x, g_relocTo.y, g_relocTo.z);
                    return;
                }
                if (res == relocation::Result::Failed && !singleHop && relocation::Recoverable(rb)) { RelocationHopFailed(rb, judged, now); return; }
                relocation::Receipt out;
                if (g_reloc.Verify(g_relocSerial, rb, now, out)) FinishRelocation();
                return;
            }
            // An intermediate forward hop, or any hop of a walk back: a waypoint judged on the doer's read-back alone.
            const char* why = "";
            const auto verdict = relocation::JudgeHop(rb, g_relocHop, g_relocHopLast ? relocation::kArriveToleranceM : relocation::kHopToleranceM, why);
            if (verdict == relocation::HopVerdict::Wait) {
                if (now - g_relocArmedAt > relocation::kHopWindowMs) RelocationHopFailed(rb, "no_completed_step", now);
                return;
            }
            if (verdict == relocation::HopVerdict::Failed) { RelocationHopFailed(rb, why, now); return; }
            if (g_relocRestoring && g_relocHopLast) {
                g_reloc.Conclude(g_relocSerial, rb, true, "hop_failed_returned_to_origin", now);
                logger::Write("OBS system=relocation event=restored serial=%llu vehicle=%08X driver=%08X cause=%s chassis=%.2f,%.2f,%.2f origin=%.2f,%.2f,%.2f",
                    (unsigned long long)g_relocSerial, g_relocVehicle, g_relocDriver, g_relocFailWhy, rb.chassis.x, rb.chassis.y, rb.chassis.z, g_relocFrom.x, g_relocFrom.y, g_relocFrom.z);
                FinishRelocation(); return;
            }
            if (g_reloc.Hops(g_relocSerial) >= relocation::kMaxHops * (g_relocRestoring ? 2u : 1u)) { RelocationHopFailed(rb, "too_many_hops", now); return; }
            if (!RelocationArmHop(rb.chassis, goal, g_relocRestoring ? "walking_back" : "next_hop")) RelocationHopFailed(rb, "hop_arm_refused", now);
        }
        void ObservePairMoveAutopilot()
        {
            auto& observed = g_pairMoveObservation;
            const auto now = GetTickCount64();
            if (!observed.serial || now < observed.nextSampleAt) return;
            observed.nextSampleAt = now + kPairMoveSampleMs;
            const bool anotherMove = g_relocSerial && g_relocVehicle == observed.vehicle;
            const auto driver = RefById(observed.driver), vehicle = RefById(observed.vehicle);
            const bool seated = driver && vehicle && InVehicle(driver) == vehicle && SeatedNow(driver, vehicle, true);
            const bool playerTookOver = vehicleflight::PlayerDriving(observed.vehicle);
            const auto autopilot = vehicleflight::HandoffPhysics(observed.vehicle);
            if (autopilot.bound && !observed.hasStepBaseline) {
                observed.initialAutopilotStep = autopilot.serial;
                observed.hasStepBaseline = true;
            }
            const auto steps = observed.hasStepBaseline && autopilot.serial >= observed.initialAutopilotStep
                ? autopilot.serial - observed.initialAutopilotStep : 0;
            vehicleflight::relocate::ChassisNow chassis;
            const bool readable = vehicleflight::relocate::ReadChassisNow(observed.vehicle, chassis);
            const double travel = readable
                ? std::hypot(chassis.pos[0] - observed.arrival.x, chassis.pos[1] - observed.arrival.y) : 0.0;
            const bool resumed = !anotherMove && !playerTookOver && seated && autopilot.live && steps > 0 && travel >= kPairMoveProofM;
            if (!resumed && !anotherMove && !playerTookOver && seated && now - observed.startedAt < kPairMoveObserveMs) return;

            const char* reason = resumed ? "seated_pair_moved_after_fresh_autopilot_steps" :
                anotherMove ? "another_relocation_started" : playerTookOver ? "player_took_control" :
                !seated ? "driver_not_seated" : !readable ? "chassis_unreadable" :
                !autopilot.live || !steps ? "no_fresh_autopilot_steps" : "no_measurable_travel";
            logger::Write("OBS system=relocation event=autopilot_after_move serial=%llu vehicle=%08X driver=%08X outcome=%s reason=%s arrival=%.2f,%.2f,%.2f current=%.2f,%.2f,%.2f travel_m=%.2f elapsed_ms=%llu steps=%llu live=%d seated=%d body_readable=%d",
                (unsigned long long)observed.serial, observed.vehicle, observed.driver, resumed ? "confirmed" : "not_confirmed", reason,
                observed.arrival.x, observed.arrival.y, observed.arrival.z, chassis.pos[0], chassis.pos[1], chassis.pos[2], travel,
                (unsigned long long)(now - observed.startedAt), (unsigned long long)steps, autopilot.live ? 1 : 0, seated ? 1 : 0, readable ? 1 : 0);
            observed = {};
        }

        void PairMoveKey(uptr player){
            ObservePairMoveAutopilot();
            RelocationTick();
            const bool down=g_pairMoveKey&&Focused()&&(GetAsyncKeyState(g_pairMoveKey)&0x8000)!=0;
            const bool pressed=down&&!g_pairMoveDown;
            g_pairMoveDown=down;
            if(!pressed||!bindings::Allowed()||hoverui::IsScannerOpen()||InVehicle(player))return;
            RelocationStart(player);
        }

        // ---- Far tier test (diagnostic builds only; notes/FAR_TIER_TEST_20261121.md) ----
        // Moves the seated pair the player faces N m along its heading PAST the loaded-area wall (300 m, +50 per run, 600 m at most), then observes
        // (stage 1, no rail) or holds it on the kinematic rail (stage 2: vehicleflight::relocate::Rail*, height = terrain + ride), then walks it back
        // and watches the handoff to a fully real car. This is a SEPARATE mover: relocation.h's 250 m limit and the "destination cell must be loaded"
        // refusal in ArmRelocation stay exactly as they are for production; the hops reuse only the doer (Arm/Take/Read, 20 m per write).
        // Press the key: Idle = start stage 1 | during/after stage 1 = start the rail (stage 2) | during the rail = walk back.
        // Everything is logged as OBS system=far.test event=... (start, terrain_probe, arrived, sample, summary, return_check, return_summary, refused, ...).
        constexpr std::uint32_t kFarLoadingFlag = 0x05E60AFA;   // the game's loading-screen byte (main_vehicle.cpp kLoadingScreenFlag)
        constexpr std::uint64_t kFarSerialBase = 0xFA00000000000000ull;   // far test hop serials never meet relocation.h's (those count from 1)
        constexpr int kFarProbeMax = 24;
        enum class FarPhase { Idle, Probe, Out, Observe1, Parked1, Rail, Back, Check };
        const char* FarPhaseName(FarPhase p)
        {
            switch (p) {
            case FarPhase::Probe: return "probe"; case FarPhase::Out: return "out"; case FarPhase::Observe1: return "observe_no_rail";
            case FarPhase::Parked1: return "parked_no_rail"; case FarPhase::Rail: return "rail"; case FarPhase::Back: return "back"; case FarPhase::Check: return "return_check";
            default: return "idle";
            }
        }
        struct FarAnswer { bool ok = false, pending = false; double z = 0; };
        struct FarRun {
            FarPhase phase = FarPhase::Idle;
            unsigned finished = 0, id = 0;                      // runs finished (the next distance), this run's number
            std::uint32_t vehicle = 0, driver = 0;
            farrail::P3 start{}, goal{};
            double dirX = 0, dirY = 1, yaw = 0, distance = 0, ride = 1.5, speed = farrail::kRailSpeed, arrivalZ = 0;
            farrail::Source source = farrail::Source::None;
            ULONGLONG runAt = 0, arrivedAt = 0, railAt = 0, backAt = 0, checkAt = 0, sampleAt = 0, probeAt = 0, hopArmedAt = 0, fillPendingAt = 0;
            // terrain probe
            int probeN = 0; bool probeDone[farrail::kSources][kFarProbeMax]{}; FarAnswer probe[farrail::kSources][kFarProbeMax]{};
            int fillNext = 0, fillMiss = 0;
            // hops
            bool hopArmed = false, hopLast = false, back = false;
            std::uint64_t hopSerial = 0, hopCounter = 0; unsigned hops = 0;
            farrail::P3 hop{};
            // observation
            farrail::Tally tally1, tally2; bool summary1 = false, summary2 = false;
            farrail::P3 prev{}; bool havePrev = false; double prevSpeed = 0;
            unsigned driverGone = 0, unreadable = 0;
            const char* backWhy = "";
            // return check
            unsigned chkSamples = 0, chkLive = 0, chkSeated = 0, chkMoving = 0, chkBroken = 0; double chkMinZ = 1e18;
        };
        FarRun g_far;
        bool FarLoadingScreen()
        {
            __try { return *reinterpret_cast<volatile std::uint8_t*>(game::Base() + kFarLoadingFlag) != 0; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return true; }
        }
        // The game's cached terrain heightfield (the map reads it; window = uDefaultGameTerrainWidthInCells = 100 cells, 10 km): no objects here for __try.
        bool FarCachedTerrain(double x, double y, double& z)
        {
            float xy[4] = { float(x), float(y), 0.0f, 0.0f }, h = 0.0f;
            __try { if (!EngineFn<std::uint8_t (*)(void*, float*, float*)>(addr::kCachedTerrainHeight)(nullptr, xy, &h)) return false; }
            __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
            if (!std::isfinite(h)) return false;
            z = h; return true;
        }
        FarAnswer FarAsk(farrail::Source s, double x, double y, double zRef, std::uint32_t self)
        {
            FarAnswer a;
            switch (s) {
            case farrail::Source::Cached: a.ok = FarCachedTerrain(x, y, a.z); break;
            case farrail::Source::Cellbake: { double z = 0; a.ok = cellbake::Ground(x, y, z) && std::isfinite(z); a.z = z; break; }
            case farrail::Source::NavMap: { const auto m = navmap::At(x, y, zRef + 1000.0); a.ok = m.haveGround && std::isfinite(m.ground); a.z = m.ground; break; }
            case farrail::Source::Ray: {
                environment::Ground g;
                if (!environment::GroundAt(self, x, y, zRef + 1500.0, zRef - 1500.0, g)) a.pending = true;
                else { a.ok = g.hit && std::isfinite(g.z); a.z = g.z; }
                break;
            }
            default: break;
            }
            return a;
        }
        std::uint32_t g_farKeptQuery = 0; bool g_farKeptFound = false;
        bool FarKept(std::uint32_t id)
        {
            g_farKeptQuery = id; g_farKeptFound = false;
            vehiclekeep::ForEachHeld([](std::uint32_t v) { if (v == g_farKeptQuery) g_farKeptFound = true; });
            return g_farKeptFound;
        }
        void FarRefuse(const char* reason, std::uint32_t v = 0, std::uint32_t d = 0)
        {
            logger::Write("OBS system=far.test event=refused reason=%s vehicle=%08X driver=%08X phase=%s", reason, v, d, FarPhaseName(g_far.phase));
        }
        double FarAlong(const farrail::P3& p) { return (p.x - g_far.start.x) * g_far.dirX + (p.y - g_far.start.y) * g_far.dirY; }
        // Stop everything the test holds. The pose owner and the relocation mark are given back; the doer and the rail are idle.
        void FarRelease(const char* why)
        {
            vehicleflight::relocate::RailStop();
            if (g_far.hopSerial) vehicleflight::relocate::Disarm(g_far.hopSerial);
            g_far.hopSerial = 0; g_far.hopArmed = false;
            if (g_far.vehicle) NoteRelocationMark(g_far.vehicle, false);
            if (!g_relocSerial) ReleaseRelocationPose();
            logger::Write("OBS system=far.test event=released run=%u reason=%s vehicle=%08X phase=%s", g_far.id, why, g_far.vehicle, FarPhaseName(g_far.phase));
            g_far.phase = FarPhase::Idle;
        }
        // The pair cannot be walked back. Driverless chassis: retire the pair through the existing job path; otherwise say loudly where it is.
        void FarAbandon(const char* why)
        {
            const uptr v = RefById(g_far.vehicle), d = RefById(g_far.driver);
            const bool aboard = v && d && InVehicle(d) == v;
            vehicleflight::relocate::ChassisNow ch;
            vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch);
            logger::Write("OBS system=far.test event=abandoned run=%u reason=%s vehicle=%08X driver=%08X driver_aboard=%d chassis_readable=%d chassis=%.1f,%.1f,%.1f from_start=%.1f action=%s",
                g_far.id, why, g_far.vehicle, g_far.driver, aboard ? 1 : 0, ch.bodyValid ? 1 : 0, ch.pos[0], ch.pos[1], ch.pos[2],
                farrail::Dist({ ch.pos[0], ch.pos[1], ch.pos[2] }, g_far.start), aboard ? "left_to_existing_systems" : "pair_retired_via_EndJobOf");
            const auto vid = g_far.vehicle;
            FarRelease(why);
            if (!aboard && vid) EndJobOf(vid, "far tier test: driver lost, pair retired");
        }
        void FarSummary(int stage, const farrail::Tally& t, bool cutShort)
        {
            const char* verdict = stage == 1 ? farrail::Stage1Verdict(t) : farrail::Stage2Verdict(t);
            logger::Write("OBS system=far.test event=summary run=%u stage=%d verdict=%s samples=%u seconds=%.1f model_lost=%u not_seated=%u not_kept=%u physics_dead=%u unreadable=%u jumps=%u max_jump=%.1f fall=%.1f min_z=%.1f max_z=%.1f terrain_unknown=%u err_samples=%u max_abs_err=%.2f mean_abs_err=%.2f moving=%u max_speed=%.1f source=%s distance=%.0f cut_short=%d",
                g_far.id, stage, verdict, t.samples, t.samples * 0.5, t.modelLost, t.notSeated, t.notKept, t.physicsDead, t.unreadable, t.jumps, t.maxJump, t.Fall(),
                t.minZ < 1e17 ? t.minZ : 0.0, t.maxZ > -1e17 ? t.maxZ : 0.0, t.terrainUnknown, t.errSamples, t.maxAbsErr, t.MeanAbsErr(), t.moving, t.maxSpeed,
                farrail::SourceName(g_far.source), g_far.distance, cutShort ? 1 : 0);
        }
        // Terrain profile along the path, filled ahead of the pair from the chosen source (the rail's physics thread only reads it).
        void FarFill(double upToS)
        {
            if (g_far.source == farrail::Source::None) return;
            int guard = 0;
            while (g_far.fillNext < farrail::kProfileCap && g_far.fillNext * farrail::kProfileSpacingM <= upToS && g_far.fillNext * farrail::kProfileSpacingM <= farrail::kPathCapM && guard < 40) {
                const double s = g_far.fillNext * farrail::kProfileSpacingM;
                const auto a = FarAsk(g_far.source, g_far.start.x + g_far.dirX * s, g_far.start.y + g_far.dirY * s, g_far.start.z, g_far.vehicle);
                if (a.pending) {
                    const ULONGLONG now = GetTickCount64();
                    if (!g_far.fillPendingAt) g_far.fillPendingAt = now;
                    if (now - g_far.fillPendingAt < 3000) break;
                }
                g_far.fillPendingAt = 0;
                if (a.ok) vehicleflight::relocate::RailSetTerrain(g_far.fillNext, a.z); else ++g_far.fillMiss;
                ++g_far.fillNext; ++guard;
            }
        }
        // ---- stage 0: ask every terrain source at every probe distance along the heading (the answer beyond the wall is the point) ----
        // Returns true when every probe has an answer (rays may take a few frames; after 3 s a pending ray counts as not answered).
        bool FarProbeStep(ULONGLONG now)
        {
            bool all = true;
            const bool expired = now - g_far.probeAt > 3000;
            for (int i = 0; i < g_far.probeN; ++i) {
                const double s = farrail::ProbeDistance(i), x = g_far.start.x + g_far.dirX * s, y = g_far.start.y + g_far.dirY * s;
                for (int k = 0; k < farrail::kSources; ++k) {
                    if (g_far.probeDone[k][i]) continue;
                    const auto a = FarAsk(farrail::Source(k), x, y, g_far.start.z, g_far.vehicle);
                    if (a.pending && !expired) { all = false; continue; }
                    g_far.probe[k][i] = a; g_far.probe[k][i].pending = false; g_far.probeDone[k][i] = true;
                }
            }
            return all;
        }
        void FarProbeReport()
        {
            farrail::Coverage cov[farrail::kSources]{};
            int beyondPoints = 0;
            for (int i = 0; i < g_far.probeN; ++i) {
                const double s = farrail::ProbeDistance(i);
                if (s >= 250.0) ++beyondPoints;
                logger::Write("OBS system=far.test event=terrain_probe run=%u dist=%.0f x=%.1f y=%.1f cached=%d,%.2f cellbake=%d,%.2f navmap=%d,%.2f ray=%d,%.2f",
                    g_far.id, s, g_far.start.x + g_far.dirX * s, g_far.start.y + g_far.dirY * s,
                    g_far.probe[0][i].ok ? 1 : 0, g_far.probe[0][i].z, g_far.probe[1][i].ok ? 1 : 0, g_far.probe[1][i].z,
                    g_far.probe[2][i].ok ? 1 : 0, g_far.probe[2][i].z, g_far.probe[3][i].ok ? 1 : 0, g_far.probe[3][i].z);
                for (int k = 0; k < farrail::kSources; ++k) if (g_far.probe[k][i].ok) { ++cov[k].all; if (s >= 250.0) ++cov[k].beyond; }
            }
            g_far.source = farrail::PickSource(cov, g_far.probeN, beyondPoints);
            g_far.ride = farrail::RideFromStart(g_far.start.z, g_far.source != farrail::Source::None && g_far.probe[int(g_far.source)][0].ok ? g_far.probe[int(g_far.source)][0].z : 0.0,
                g_far.source != farrail::Source::None && g_far.probe[int(g_far.source)][0].ok);
            logger::Write("OBS system=far.test event=terrain_probe_summary run=%u points=%d beyond_wall_points=%d cached=%d/%d cellbake=%d/%d navmap=%d/%d ray=%d/%d beyond_wall_answers=cached:%d,cellbake:%d,navmap:%d,ray:%d picked=%s ride=%.2f",
                g_far.id, g_far.probeN, beyondPoints, cov[0].all, g_far.probeN, cov[1].all, g_far.probeN, cov[2].all, g_far.probeN, cov[3].all, g_far.probeN,
                cov[0].beyond, cov[1].beyond, cov[2].beyond, cov[3].beyond, farrail::SourceName(g_far.source), g_far.ride);
            g_far.fillNext = 0; g_far.fillMiss = 0; g_far.fillPendingAt = 0;
            vehicleflight::relocate::RailClearTerrain();
        }
        // ---- hops (the proven doer write, one bounded hop at a time, read back before the next) ----
        bool FarArmHop(const farrail::P3& at, const char* why)
        {
            if (g_far.hopSerial) vehicleflight::relocate::Disarm(g_far.hopSerial);
            const farrail::P3 goal = g_far.back ? farrail::P3{ g_far.start.x, g_far.start.y, g_far.start.z + 0.3 } : g_far.goal;
            bool last = false;
            const auto hop = farrail::NextHop(at, goal, last);
            const auto serial = kFarSerialBase + (++g_far.hopCounter);
            const char* refusal = vehicleflight::relocate::Arm(g_far.vehicle, hop.x - at.x, hop.y - at.y, hop.z - at.z, serial, 0);
            logger::Write("OBS system=far.test event=hop_armed run=%u hop=%u phase=%s last=%d from=%.2f,%.2f,%.2f to=%.2f,%.2f,%.2f remaining=%.1f result=%s reason=%s",
                g_far.id, g_far.hops + 1, g_far.back ? "back" : "out", last ? 1 : 0, at.x, at.y, at.z, hop.x, hop.y, hop.z, farrail::Dist(at, goal), refusal ? "refused" : "armed", refusal ? refusal : why);
            if (refusal) { g_far.hopSerial = 0; g_far.hopArmed = false; return false; }
            g_far.hopSerial = serial; g_far.hop = hop; g_far.hopLast = last; g_far.hopArmed = true; g_far.hopArmedAt = GetTickCount64(); ++g_far.hops;
            return true;
        }
        enum class FarHopResult { Working, Arrived, Failed };
        FarHopResult FarHopTick(ULONGLONG now, const char*& why)
        {
            why = "";
            const uptr v = RefById(g_far.vehicle), d = RefById(g_far.driver);
            if (!g_far.hopArmed) {
                vehicleflight::relocate::ChassisNow ch;
                if (!vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch)) { why = ch.bound ? "chassis_body_unreadable" : "no_physics_instance"; return FarHopResult::Failed; }
                if (!FarArmHop({ ch.pos[0], ch.pos[1], ch.pos[2] }, g_far.hops ? "next_hop" : "first_hop")) { why = "hop_arm_refused"; return FarHopResult::Failed; }
                return FarHopResult::Working;
            }
            const unsigned limit = farrail::kMaxHopsOut(g_far.distance) * (g_far.back ? 2u : 1u);
            vehicleflight::relocate::Facts f;
            if (!vehicleflight::relocate::Take(g_far.hopSerial, f)) {
                if (now - g_far.hopArmedAt > farrail::kHopWindowMs) { why = "hop_not_issued_in_time"; return FarHopResult::Failed; }
                return FarHopResult::Working;
            }
            if (!f.issued) { why = f.refusal && *f.refusal ? f.refusal : "doer_refused"; return FarHopResult::Failed; }
            vehicleflight::relocate::Chassis ch;
            const bool have = vehicleflight::relocate::Read(g_far.hopSerial, ch);
            const bool seated = d && v && InVehicle(d) == v && SeatedNow(d, v, true);
            const farrail::P3 at{ ch.pos[0], ch.pos[1], ch.pos[2] };
            const bool expired = now - g_far.hopArmedAt > farrail::kHopWindowMs;
            const auto verdict = farrail::JudgeHop(have && ch.bodyValid, have && ch.live, have ? ch.completedSteps : 0, farrail::Dist(at, g_far.hop),
                g_far.hopLast ? farrail::kArriveToleranceM : farrail::kHopToleranceM, seated, !g_far.back, expired, why);
            if (verdict == farrail::Hop::Wait) {
                if (expired) { why = "no_completed_step"; return FarHopResult::Failed; }
                return FarHopResult::Working;
            }
            if (verdict == farrail::Hop::Failed) return FarHopResult::Failed;
            if (g_far.hopLast) return FarHopResult::Arrived;
            if (g_far.hops >= limit) { why = "too_many_hops"; return FarHopResult::Failed; }
            if (!FarArmHop(at, "next_hop")) { why = "hop_arm_refused"; return FarHopResult::Failed; }
            return FarHopResult::Working;
        }
        void FarBeginBack(const char* why)
        {
            vehicleflight::relocate::RailStop();
            if (g_far.hopSerial) vehicleflight::relocate::Disarm(g_far.hopSerial);
            if (g_far.phase == FarPhase::Rail && !g_far.summary2) { FarSummary(2, g_far.tally2, true); g_far.summary2 = true; }
            if (g_far.phase == FarPhase::Observe1 && !g_far.summary1) { FarSummary(1, g_far.tally1, true); g_far.summary1 = true; }
            vehicleflight::relocate::ChassisNow ch;
            const bool readable = vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch);
            logger::Write("OBS system=far.test event=back_begin run=%u reason=%s from_phase=%s chassis=%.1f,%.1f,%.1f from_start=%.1f readable=%d",
                g_far.id, why, FarPhaseName(g_far.phase), ch.pos[0], ch.pos[1], ch.pos[2], readable ? farrail::Dist({ ch.pos[0], ch.pos[1], ch.pos[2] }, g_far.start) : -1.0, readable ? 1 : 0);
            if (!readable) { FarAbandon("chassis_unreadable_cannot_walk_back"); return; }
            g_far.back = true; g_far.hops = 0; g_far.hopArmed = false; g_far.hopSerial = 0; g_far.backWhy = why; g_far.backAt = GetTickCount64();
            SetRelocationPoseOwner(g_far.vehicle);   // while the doer writes, the engine-side refile yields (as in the relocation test)
            g_relocPoseReleased = false;
            g_far.phase = FarPhase::Back;
        }
        void FarBeginRail(const char* why)
        {
            const uptr v = RefById(g_far.vehicle), d = RefById(g_far.driver);
            vehicleflight::relocate::ChassisNow ch;
            if (!vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch)) { FarRefuse("rail_chassis_unreadable", g_far.vehicle, g_far.driver); return; }
            if (!(d && v && InVehicle(d) == v && SeatedNow(d, v, true))) { FarRefuse("rail_pair_not_intact", g_far.vehicle, g_far.driver); return; }
            if (g_far.phase == FarPhase::Observe1 && !g_far.summary1) { FarSummary(1, g_far.tally1, true); g_far.summary1 = true; }
            const farrail::P3 at{ ch.pos[0], ch.pos[1], ch.pos[2] };
            const double s0 = FarAlong(at);
            const char* refusal = vehicleflight::relocate::RailStart(g_far.vehicle, g_far.start.x, g_far.start.y, g_far.dirX, g_far.dirY, s0, at.z, g_far.speed, g_far.ride);
            logger::Write("OBS system=far.test event=rail_start run=%u reason=%s result=%s refusal=%s s0=%.1f z0=%.2f speed=%.1f ride=%.2f source=%s profile_known=%d lateral_offset=%.2f",
                g_far.id, why, refusal ? "refused" : "started", refusal ? refusal : "", s0, at.z, g_far.speed, g_far.ride, farrail::SourceName(g_far.source),
                vehicleflight::relocate::RailTerrainKnown(), std::fabs((at.x - g_far.start.x) * g_far.dirY - (at.y - g_far.start.y) * g_far.dirX));
            if (refusal) return;
            g_far.tally2 = {}; g_far.summary2 = false; g_far.havePrev = false; g_far.railAt = GetTickCount64(); g_far.sampleAt = 0;
            g_far.driverGone = g_far.unreadable = 0;
            g_far.phase = FarPhase::Rail;
        }
        // One observation (every 0.5 s in observe / parked / rail / return check): everything the open questions ask about, one line.
        bool FarSample(ULONGLONG now, bool railOn, farrail::Sample& s, vehicleflight::relocate::ChassisNow& ch)
        {
            const uptr v = RefById(g_far.vehicle), d = RefById(g_far.driver);
            const bool have = vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch);
            s = farrail::Sample{};
            s.bodyReadable = have && ch.bodyValid;
            if (s.bodyReadable) { s.z = ch.pos[2]; s.speed = std::hypot(ch.vel[0], ch.vel[1]); }
            s.modelLoaded = v && d && Has3D(v) && Has3D(d);
            const bool aboard = v && d && InVehicle(d) == v;
            s.seated = aboard && SeatedNow(d, v, true);
            s.kept = FarKept(g_far.vehicle);
            const auto ph = vehicleflight::HandoffPhysics(g_far.vehicle);
            s.physicsBound = ph.bound; s.physicsLive = ph.live;
            FarAnswer ground;
            if (s.bodyReadable && g_far.source != farrail::Source::None) ground = FarAsk(g_far.source, ch.pos[0], ch.pos[1], ch.pos[2], g_far.vehicle);
            s.terrainKnown = ground.ok;
            if (railOn && ground.ok) s.errM = ch.pos[2] - (ground.z + g_far.ride);
            const farrail::P3 cur{ ch.pos[0], ch.pos[1], ch.pos[2] };
            if (s.bodyReadable && g_far.havePrev) {
                const double dt = 0.5, expected = (std::max(g_far.prevSpeed, s.speed)) * dt * 1.5 + 3.0;
                s.jumpM = farrail::UncommandedJump(g_far.prev, cur, expected);
            }
            if (s.bodyReadable) { g_far.prev = cur; g_far.havePrev = true; g_far.prevSpeed = s.speed; }
            // cells at the chassis (is the cell there loaded, is the reference listed elsewhere)
            loadedarea::RelocationCell cell{};
            const bool cellKnown = s.bodyReadable && loadedarea::RelocationDestination(g_far.vehicle, float(ch.pos[0]), float(ch.pos[1]), cell);
            const auto vp = v ? At<game::Vec3>(v, off::kRefPosition) : game::Vec3{};
            game::Vec3 mp{};
            const bool haveModel = v && ScenePos(v, mp);
            vehicleflight::relocate::RailFacts rf;
            const bool haveRail = railOn && vehicleflight::relocate::RailRead(rf);
            const double t = (now - (g_far.phase == FarPhase::Rail ? g_far.railAt : g_far.phase == FarPhase::Check ? g_far.checkAt : g_far.arrivedAt)) / 1000.0;
            logger::Write("OBS system=far.test event=sample run=%u phase=%s t=%.1f chassis=%.2f,%.2f,%.2f vel=%.1f,%.1f,%.1f from_start=%.1f along=%.1f vehicle_ref=%.1f,%.1f,%.1f ref_gap=%.1f model=%.1f,%.1f,%.1f model_gap=%.1f model3d=%d,%d seated=%d kept=%d physics_bound=%d live=%d steps=%llu terrain=%d,%.2f,%s ride=%.2f err=%.2f cell_known=%d cell=%d,%d at_cell=%d,%d at_cell_loaded=%d rail_s=%.1f rail_steps=%llu rail_holds=%llu rail_faults=%llu rail_fault=%s jump=%.1f",
                g_far.id, FarPhaseName(g_far.phase), t, ch.pos[0], ch.pos[1], ch.pos[2], ch.vel[0], ch.vel[1], ch.vel[2], farrail::Dist(cur, g_far.start), FarAlong(cur),
                vp.x, vp.y, vp.z, std::hypot(double(vp.x) - ch.pos[0], double(vp.y) - ch.pos[1]), mp.x, mp.y, mp.z, haveModel ? std::hypot(double(mp.x) - ch.pos[0], double(mp.y) - ch.pos[1]) : -1.0,
                v && Has3D(v) ? 1 : 0, d && Has3D(d) ? 1 : 0, s.seated ? 1 : 0, s.kept ? 1 : 0, ph.bound ? 1 : 0, ph.live ? 1 : 0, static_cast<unsigned long long>(ph.serial),
                ground.ok ? 1 : 0, ground.z, farrail::SourceName(g_far.source), g_far.ride, std::isfinite(s.errM) ? s.errM : 0.0,
                cellKnown ? 1 : 0, cell.fromX, cell.fromY, cell.toX, cell.toY, cell.cellLoaded ? 1 : 0,
                haveRail ? rf.s : 0.0, static_cast<unsigned long long>(haveRail ? rf.steps : 0), static_cast<unsigned long long>(haveRail ? rf.holds : 0),
                static_cast<unsigned long long>(haveRail ? rf.faults : 0), haveRail && rf.fault ? rf.fault : "", s.jumpM);
            return have;
        }
        // Common watchdog for the phases where the pair stands out there: lost driver / unreadable chassis / fall / rail fault.
        // Returns true when it took the pair out of the phase (back or abandoned).
        bool FarWatch(const farrail::Sample& s, bool have, bool railOn)
        {
            g_far.driverGone = (!s.seated) ? g_far.driverGone + 1 : 0;
            g_far.unreadable = !have ? g_far.unreadable + 1 : 0;
            if (g_far.unreadable >= 6) { FarAbandon("chassis_unreadable_for_3s"); return true; }
            if (g_far.driverGone >= 3) { logger::Write("OBS system=far.test event=driver_lost run=%u samples=%u model3d=%d", g_far.id, g_far.driverGone, s.modelLoaded ? 1 : 0); FarBeginBack("driver_not_seated"); return true; }
            if (railOn) {
                vehicleflight::relocate::RailFacts rf;
                if (vehicleflight::relocate::RailRead(rf) && !rf.active) { logger::Write("OBS system=far.test event=rail_stopped run=%u fault=%s faults=%llu", g_far.id, rf.fault, static_cast<unsigned long long>(rf.faults)); FarBeginBack("rail_stopped"); return true; }
            } else if (have && s.z < g_far.arrivalZ - farrail::kFallAbortM) {
                logger::Write("OBS system=far.test event=fall_abort run=%u z=%.1f arrival_z=%.1f", g_far.id, s.z, g_far.arrivalZ);
                FarBeginBack("fell_below_arrival");
                return true;
            }
            return false;
        }
        void FarStart(uptr player)
        {
            if (InVehicle(player)) { FarRefuse("player_in_vehicle"); return; }
            if (FarLoadingScreen()) { FarRefuse("loading_screen"); return; }
            if (g_relocSerial) { FarRefuse("relocation_test_running", g_relocVehicle, g_relocDriver); return; }
            uptr d = 0, v = 0;
            if (!FacedSeatedPair(player, d, v)) {
                v = RefById(g_testPairVehicle); d = RefById(g_testPairDriver);
                if (!v || !d || InVehicle(d) != v) { FarRefuse("no_seated_npc_in_front_and_no_board_pair", g_testPairVehicle, g_testPairDriver); return; }
            }
            const std::uint32_t vid = At<std::uint32_t>(v, off::kFormId), did = At<std::uint32_t>(d, off::kFormId);
            if (Disabled(v) || trafficpool::MadeVehicle(vid) || trafficpool::PlayerOwnsHandoff(vid) || VehicleInCombat(vid)) { FarRefuse("vehicle_unavailable", vid, did); return; }
            if (!SeatedNow(d, v, true)) { FarRefuse("driver_not_seated", vid, did); return; }
            vehicleflight::relocate::ChassisNow ch;
            if (!vehicleflight::relocate::ReadChassisNow(vid, ch)) { FarRefuse(ch.bound ? "chassis_body_unreadable" : "no_physics_instance", vid, did); return; }
            const auto vr = At<game::Vec3>(v, off::kRefRotation);
            const auto pp = At<game::Vec3>(player, off::kRefPosition);
            auto& r = g_far;
            const unsigned finished = r.finished; const unsigned id = r.id + 1;
            r = FarRun{};
            r.finished = finished; r.id = id; r.vehicle = vid; r.driver = did;
            r.yaw = vr.z; r.dirX = std::sin(vr.z); r.dirY = std::cos(vr.z);
            r.start = { ch.pos[0], ch.pos[1], ch.pos[2] };
            r.distance = farrail::TargetDistance(finished);
            r.goal = { r.start.x + r.dirX * r.distance, r.start.y + r.dirY * r.distance, r.start.z + 0.3 };
            r.runAt = GetTickCount64();
            r.probeN = std::min(kFarProbeMax, farrail::ProbeCount(r.distance + 150.0));
            r.probeAt = r.runAt;
            loadedarea::RelocationCell cell{};
            loadedarea::RelocationDestination(vid, float(r.goal.x), float(r.goal.y), cell);
            const bool destInView = vehiclekeep::InCameraView(r.goal.x, r.goal.y, r.goal.z);
            logger::Write("OBS system=far.test event=start run=%u stage=1 vehicle=%08X driver=%08X distance=%.0f next_distance=%.0f max_distance=%.0f from=%.1f,%.1f,%.1f to=%.1f,%.1f,%.1f yaw=%.3f dir=%.3f,%.3f player=%.1f,%.1f,%.1f dest_in_camera_view=%d to_cell=%d,%d to_cell_loaded=%d hops_needed=%u rail_speed=%.1f source=%s",
                r.id, vid, did, r.distance, farrail::TargetDistance(finished + 1), farrail::kMaxDistanceM, r.start.x, r.start.y, r.start.z, r.goal.x, r.goal.y, r.goal.z, r.yaw, r.dirX, r.dirY,
                pp.x, pp.y, pp.z, destInView ? 1 : 0, cell.toX, cell.toY, cell.cellLoaded ? 1 : 0, farrail::HopsFor(r.distance), r.speed, "pending_probe");
            NoteRelocationMark(vid, true);
            r.phase = FarPhase::Probe;
        }
        void FarPress(uptr player)
        {
            switch (g_far.phase) {
            case FarPhase::Idle: FarStart(player); break;
            case FarPhase::Observe1: case FarPhase::Parked1: FarBeginRail("second_press"); break;
            case FarPhase::Rail: FarBeginBack("press_during_rail"); break;
            default: FarRefuse("busy_wait_for_the_current_step", g_far.vehicle, g_far.driver); break;
            }
        }
        void FarReset()
        {
            if (g_far.phase != FarPhase::Idle) { vehicleflight::relocate::RailStop(); if (g_far.hopSerial) vehicleflight::relocate::Disarm(g_far.hopSerial); }
            const auto finished = g_far.finished, id = g_far.id;
            g_far = FarRun{}; g_far.finished = finished; g_far.id = id; g_farDown = false;
        }
        void FarTick(uptr player)
        {
            const bool down = g_farKey && Focused() && (GetAsyncKeyState(g_farKey) & 0x8000) != 0;
            const bool pressed = down && !g_farDown;
            g_farDown = down;
            if (pressed && bindings::Allowed() && !hoverui::IsScannerOpen()) FarPress(player);
            if (g_far.phase == FarPhase::Idle) return;
            const ULONGLONG now = GetTickCount64();
            if (FarLoadingScreen()) { FarAbandon("loading_screen_while_running"); return; }
            switch (g_far.phase) {
            case FarPhase::Probe:
                if (FarProbeStep(now)) {
                    FarProbeReport();
                    logger::Write("OBS system=far.test event=out_begin run=%u distance=%.0f source=%s ride=%.2f", g_far.id, g_far.distance, farrail::SourceName(g_far.source), g_far.ride);
                    SetRelocationPoseOwner(g_far.vehicle);
                    g_relocPoseReleased = false;
                    g_far.back = false; g_far.hops = 0;
                    g_far.phase = FarPhase::Out;
                }
                break;
            case FarPhase::Out: {
                FarFill(g_far.distance + 200.0);
                const char* why = "";
                const auto res = FarHopTick(now, why);
                if (res == FarHopResult::Failed) {
                    const uptr v = RefById(g_far.vehicle), d = RefById(g_far.driver);
                    vehicleflight::relocate::ChassisNow ch;
                    const bool ok = vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch);
                    logger::Write("OBS system=far.test event=hop_failed run=%u hop=%u phase=out reason=%s chassis=%.1f,%.1f,%.1f readable=%d seated=%d",
                        g_far.id, g_far.hops, why, ch.pos[0], ch.pos[1], ch.pos[2], ok ? 1 : 0, (d && v && InVehicle(d) == v && SeatedNow(d, v, true)) ? 1 : 0);
                    if (ok && v && d && InVehicle(d) == v) FarBeginBack("out_hop_failed"); else FarAbandon("out_hop_failed_not_recoverable");
                } else if (res == FarHopResult::Arrived) {
                    vehicleflight::relocate::ChassisNow ch;
                    vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch);
                    g_far.arrivalZ = ch.pos[2]; g_far.arrivedAt = now; g_far.sampleAt = 0; g_far.havePrev = false;
                    vehicleflight::relocate::Disarm(g_far.hopSerial); g_far.hopSerial = 0; g_far.hopArmed = false;
                    ReleaseRelocationPose();   // from here the engine-side systems (refile, wall pass, move-back) act as they would: that is what stage 1 observes
                    loadedarea::RelocationCell cell{};
                    const bool cellKnown = loadedarea::RelocationDestination(g_far.vehicle, float(ch.pos[0]), float(ch.pos[1]), cell);
                    logger::Write("OBS system=far.test event=arrived run=%u hops=%u seconds=%.1f chassis=%.2f,%.2f,%.2f wanted=%.2f,%.2f,%.2f cell_known=%d from_cell=%d,%d to_cell=%d,%d to_cell_loaded=%d cross_cell=%d stage=1 observing_s=%.0f",
                        g_far.id, g_far.hops, (now - g_far.runAt) / 1000.0, ch.pos[0], ch.pos[1], ch.pos[2], g_far.goal.x, g_far.goal.y, g_far.goal.z,
                        cellKnown ? 1 : 0, cell.fromX, cell.fromY, cell.toX, cell.toY, cell.cellLoaded ? 1 : 0, cell.crossCell ? 1 : 0, farrail::kObserveSeconds);
                    g_far.phase = FarPhase::Observe1;
                }
                break;
            }
            case FarPhase::Observe1: case FarPhase::Parked1: case FarPhase::Rail: {
                const bool railOn = g_far.phase == FarPhase::Rail;
                if (railOn) {
                    vehicleflight::relocate::RailFacts rf;
                    if (vehicleflight::relocate::RailRead(rf)) FarFill(rf.s + 200.0);
                } else FarFill(g_far.distance + 200.0);
                if (now - g_far.sampleAt < 500) break;
                g_far.sampleAt = now;
                farrail::Sample s; vehicleflight::relocate::ChassisNow ch;
                const bool have = FarSample(now, railOn, s, ch);
                (railOn ? g_far.tally2 : g_far.tally1).Add(s);
                if (FarWatch(s, have, railOn)) break;
                const double since = (now - (railOn ? g_far.railAt : g_far.arrivedAt)) / 1000.0;
                if (railOn) {
                    if (!g_far.summary2 && since >= farrail::kObserveSeconds) { FarSummary(2, g_far.tally2, false); g_far.summary2 = true; }
                    if (since >= farrail::kRailAutoReturnS) FarBeginBack("rail_auto_return");
                } else {
                    if (!g_far.summary1 && since >= farrail::kObserveSeconds) { FarSummary(1, g_far.tally1, false); g_far.summary1 = true; g_far.phase = FarPhase::Parked1;
                        logger::Write("OBS system=far.test event=parked run=%u hint=press_again_for_the_rail_or_wait_for_auto_return auto_return_in_s=%.0f", g_far.id, farrail::kParkedAutoReturnS - since); }
                    if (since >= farrail::kParkedAutoReturnS) FarBeginBack("parked_auto_return");
                }
                break;
            }
            case FarPhase::Back: {
                const char* why = "";
                const auto res = FarHopTick(now, why);
                if (res == FarHopResult::Failed) {
                    vehicleflight::relocate::ChassisNow ch;
                    vehicleflight::relocate::ReadChassisNow(g_far.vehicle, ch);
                    logger::Write("OBS system=far.test event=hop_failed run=%u hop=%u phase=back reason=%s chassis=%.1f,%.1f,%.1f", g_far.id, g_far.hops, why, ch.pos[0], ch.pos[1], ch.pos[2]);
                    FarAbandon("walk_back_failed");
                } else if (res == FarHopResult::Arrived) {
                    vehicleflight::relocate::Disarm(g_far.hopSerial); g_far.hopSerial = 0; g_far.hopArmed = false;
                    ReleaseRelocationPose();
                    NoteRelocationMark(g_far.vehicle, false);   // consumers act normally again: the next ten seconds ask whether the handoff to a fully real car is clean
                    g_far.checkAt = now; g_far.sampleAt = 0; g_far.havePrev = false; g_far.chkSamples = g_far.chkLive = g_far.chkSeated = g_far.chkMoving = g_far.chkBroken = 0; g_far.chkMinZ = 1e18;
                    logger::Write("OBS system=far.test event=returned run=%u hops=%u seconds=%.1f reason=%s", g_far.id, g_far.hops, (now - g_far.backAt) / 1000.0, g_far.backWhy);
                    g_far.phase = FarPhase::Check;
                }
                break;
            }
            case FarPhase::Check: {
                if (now - g_far.sampleAt < 500) break;
                g_far.sampleAt = now;
                farrail::Sample s; vehicleflight::relocate::ChassisNow ch;
                FarSample(now, false, s, ch);
                const bool broken = vehicleflight::CensusBroken(g_far.vehicle);
                int jobPhase = -1, jobTask = -1;
                for (const auto& j : g_jobs) if (j.vehicle == g_far.vehicle && j.phase != Phase::Idle) { jobPhase = int(j.phase); jobTask = int(j.task); break; }
                ++g_far.chkSamples; g_far.chkLive += (s.physicsBound && s.physicsLive); g_far.chkSeated += s.seated; g_far.chkMoving += s.speed > 1.0; g_far.chkBroken += broken;
                if (s.bodyReadable) g_far.chkMinZ = std::min(g_far.chkMinZ, s.z);
                logger::Write("OBS system=far.test event=return_check run=%u t=%.1f physics_bound=%d live=%d seated=%d speed=%.1f census_broken=%d job_phase=%d job_task=%d z=%.2f start_z=%.2f",
                    g_far.id, (now - g_far.checkAt) / 1000.0, s.physicsBound ? 1 : 0, s.physicsLive ? 1 : 0, s.seated ? 1 : 0, s.speed, broken ? 1 : 0, jobPhase, jobTask, s.z, g_far.start.z);
                if (now - g_far.checkAt >= 10000) {
                    const double fall = g_far.chkMinZ < 1e17 ? g_far.start.z - g_far.chkMinZ : 0.0;
                    logger::Write("OBS system=far.test event=return_summary run=%u verdict=%s samples=%u live=%u seated=%u moving=%u census_broken=%u fall=%.1f stage_reached=%s",
                        g_far.id, farrail::ReturnVerdict(g_far.chkSamples, g_far.chkLive, g_far.chkSeated, g_far.chkMoving, g_far.chkBroken, fall), g_far.chkSamples, g_far.chkLive, g_far.chkSeated,
                        g_far.chkMoving, g_far.chkBroken, fall, g_far.summary2 ? "rail" : "no_rail");
                    ++g_far.finished;
                    logger::Write("OBS system=far.test event=done run=%u next_distance=%.0f", g_far.id, farrail::TargetDistance(g_far.finished));
                    FarRelease("run_finished");
                }
                break;
            }
            default: break;
            }
        }
#endif

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

		// ---- Boarding as a combat move (2026-10-21, the user: make the get-in part of the fight instead of calming the NPC first;
		// notes/COMBAT_ACTIVATE_REQUEST_20261021.md). The combat AI's own CombatMovementRequestActivateTarget: the NPC paths to the
		// vehicle and activates it (the native get-in) while it stays in combat. Built like FUN_141cfd5b0 builds it (no item),
		// registered (FUN_1410856c0) and submitted to the NPC's combat controller (FUN_1410858f0). State = flags (+0x10) >> 29:
		// 0 not started, 1 running, 2 done, 3 failed, 4 handed to the AI's activate procedure.
		constexpr std::uint32_t kCmrActivateVtbl = 0x04C32C78, kCmrDestructor = 0x01084EC0, kGameAlloc = 0x022C7320, kCmrInit = 0x010856C0,
			kCmrSubmit = 0x010858F0, kNiRefCount = 0x05E7A41C;
		bool CodeIs(std::uint32_t rva, std::initializer_list<std::uint8_t> bytes)
		{
			std::size_t i = 0;
			for (const auto b : bytes) if (*reinterpret_cast<const std::uint8_t*>(game::Base() + rva + i++) != b) return false;
			return true;
		}
		bool CombatBoardReady()
		{
			static int ok = -1;
			if (ok >= 0) return ok == 1;
			ok = CodeIs(kGameAlloc, { 0x40, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x30 }) &&
				CodeIs(kCmrInit, { 0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xF9 }) &&
				CodeIs(kCmrSubmit, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x33, 0xFF }) &&
				*reinterpret_cast<const uptr*>(game::Base() + kCmrActivateVtbl) == game::Base() + kCmrDestructor ? 1 : 0;
			logger::Write("NPC vehicles: combat boarding (CombatMovementRequestActivateTarget) %s", ok ? "available" : "UNAVAILABLE (code differs); get-in package only");
			return ok == 1;
		}
		// A reference's own handle (ref +0x24, as FUN_141cfd5b0 reads it), only when it resolves back to the reference.
		std::uint32_t OwnHandle(uptr ref)
		{
			const std::uint32_t h = ref ? At<std::uint32_t>(ref, 0x24) : 0;
			return h && HandleRef(h) == ref ? h : 0;
		}
		int CombatBoardState(uptr req) { return req ? int(At<std::uint32_t>(req, 0x10) >> 29) : -1; }
		const char* CombatBoardStateName(int st)
		{
			switch (st) {
			case 0: return "not started";
			case 1: return "running";
			case 2: return "done";
			case 3: return "failed";
			case 4: return "activating";
			default: return "?";
			}
		}
		void CombatBoardRelease(uptr& req)
		{
			const uptr r = req;
			req = 0;
			if (!r) return;
			__try {
				if (_InterlockedDecrement(reinterpret_cast<volatile long*>(r + 8)) == 0)
					reinterpret_cast<void (*)(uptr)>((*reinterpret_cast<void***>(r))[1])(r);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
		}
		// The submitted request (we hold one reference), or 0 with `why`.
		uptr CombatBoardStart(uptr npc, uptr vehicle, const char*& why)
		{
			why = "";
			if (!CombatBoardReady()) { why = "unavailable"; return 0; }
			if (!At<uptr>(npc, 0x250)) { why = "no combat controller"; return 0; }
			const std::uint32_t hn = OwnHandle(npc), hv = OwnHandle(vehicle);
			if (!hn || !hv) { why = !hn ? "no NPC handle" : "no vehicle handle"; return 0; }
			uptr req = 0;
			__try {
				req = EngineFn<uptr (*)(std::uint64_t)>(kGameAlloc)(0x40);
				if (!req) { why = "allocation failed"; return 0; }
				std::memset(reinterpret_cast<void*>(req), 0, 0x40);
				At<uptr>(req, 0x00) = game::Base() + kCmrActivateVtbl;
				At<std::uint32_t>(req, 0x08) = 1;            // our reference
				At<std::uint32_t>(req, 0x0C) = 0xFFFFFFFFu;
				At<std::uint32_t>(req, 0x10) = 0x08000000u;  // as the game's node sets it
				At<std::uint32_t>(req, 0x28) = hn;           // the mover
				At<std::uint32_t>(req, 0x2C) = hv;           // the target: the vehicle
				_InterlockedIncrement(reinterpret_cast<volatile long*>(game::Base() + kNiRefCount));  // the NiRefObject count its destructor lowers
				EngineFn<void (*)(uptr)>(kCmrInit)(req);
				if (!EngineFn<char (*)(uptr, uptr)>(kCmrSubmit)(npc, req)) {
					why = "the combat controller refused it";
					CombatBoardRelease(req);
					return 0;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				why = "exception";
				return 0;  // a half-built request is left alone
			}
			return req;
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
			if(!player||!actor)return false;
			uptr gang[32];
			const int n = Followers(player, gang, 32);
			for (int i = 0; i < n; ++i) {
				if (gang[i] == actor) return true;
			}
			return false;
		}

		// The player's own get-in, as pressing E runs it (decompile 2026-10-28: activation FUN_14192A3F0(actor, vehicle) calls
		// the seat check FUN_14063AF00, then the get-in FUN_141951FB0 with its walk-in and sit). Seating with only the final
		// seat step (FUN_14219BE30) left the player floating by the seat, not driving (the user, 10:16 run).
		bool PlayerGetIn(uptr player, uptr vehicle)
		{
			__try {
				if (!EngineFn<bool (*)(uptr, uptr)>(0x0063AF00)(player, vehicle)) return false;
				return EngineFn<bool (*)(uptr, uptr)>(0x01951FB0)(player, vehicle);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// The player's Activate at a vehicle: tap = hand-over (gang) or blocked; held kPullMs = pull the driver out.
		void SeatTick(uptr player)
		{
			const ULONGLONG now = GetTickCount64();
			if (g_passengerRequest) {
				const uptr v = RefById(g_passengerRequest), in = InVehicle(player);
				const bool complete = v && in == v && passenger::RidingVehicle() == g_passengerRequest;
				const bool invalid = !v || (in && in != v) || vehiclehits::Wrecked(g_passengerRequest) ||
					(!in && v && EdgeDistance(player, v) > kReachRange + 3.0);
				const auto action = g_passengerAttempt.Step(now, complete, invalid, SitState(player) > 0 || (At<std::uint32_t>(player, 0x378) & 8) != 0);
				if (action == boardinggate::PassengerAction::Start) {
					const bool started = PlayerGetIn(player, v);
					logger::Write("Passenger: native boarding request for %08X -> %s", g_passengerRequest, started ? "started; waiting for completion" : "refused by the game");
					if (!started) { g_passengerRequest = 0; g_boardVehicle = 0; g_boardKeyword = 0; g_swapped = 0; }
				}
				else if (action == boardinggate::PassengerAction::Complete || action == boardinggate::PassengerAction::Cancel) {
					logger::Write("Passenger: boarding %08X %s", g_passengerRequest, complete ? "completed" : invalid ? "cancelled: target unavailable or out of reach" : "timed out after 10 s");
					g_passengerRequest = 0; g_boardVehicle = 0; g_boardKeyword = 0; g_swapped = 0;
				}
			}
			if (g_standNpc) {  // a pulled-out NPC standing up: out of the vehicle -> done; still seated after 4 s -> the instant exit
				const uptr npc = RefById(g_standNpc), v = RefById(g_standVehicle);
				if (!npc || !v || InVehicle(npc) != v) {
					if (npc) logger::Write("NPC vehicles: seat rules: %08X stood up and is out of the vehicle", g_standNpc);
					g_standNpc = 0;
				}
				else if (now - g_standAt > 4000) {
					logger::Write("NPC vehicles: seat rules: %08X still seated 4 s after the stand-up request; instant exit", g_standNpc);
					g_standNpc = 0;
					const bool out = InstantExit(npc, true);
					const auto vp = At<game::Vec3>(v, off::kRefPosition);
					const double h = At<game::Vec3>(v, off::kRefRotation).z;
					const bool placed = WarpExitController(npc, float(vp.x + std::cos(h) * 3.0), float(vp.y - std::sin(h) * 3.0), vp.z + 0.5f);
					logger::Write("NPC vehicles: seat rules: fallback exit %s, %s", out ? "done" : "FAILED", placed ? "warped beside the vehicle" : "warp FAILED");
				}
			}
			if (g_vehFlagsFor) {  // the get-out is done (nobody in the driver seat) or ran out: the vehicle's flags as they were
				const uptr v = RefById(g_vehFlagsFor);
				if (!v || !DriverOf(v, player) || now > g_vehFlagsUntil) {
					if (v) At<std::uint32_t>(v, 0x378) = (At<std::uint32_t>(v, 0x378) & ~(1u << 1)) | (g_vehFlagsBack & (1u << 1));
					logger::Write("NPC vehicles: seat rules: vehicle %08X flags restored after the get-out", g_vehFlagsFor);
					g_vehFlagsFor = 0;
				}
			}
			if (g_intent.seatAt && now >= g_intent.seatAt) {
				g_intent.seatAt = 0;
				const uptr v = RefById(g_intent.vehicle);
				// 2026-10-28: the occupant's exit can take longer than 150 ms; seating the player over an NPC still in the
				// seat is the half-done state that threw the player before. Wait (up to 3 s) until nobody is in it.
				if (v && !InVehicle(player) && DriverOf(v, player)) {
					if (now < g_intent.seatUntil) g_intent.seatAt = now + 100;
					else logger::Write("NPC vehicles: seat rules: %08X still occupied 10 s after the pull-out; you are not seated", g_intent.vehicle);
				}
				else if (v && !InVehicle(player)) {
					g_driverFor = v; g_driverUntil = now + 11000; g_swapVehicle = 0; g_swapKeyword = 0;
					const bool started = PlayerGetIn(player, v);
					if (!started && now < g_intent.seatUntil) g_intent.seatAt = now + 150;  // the seat may need a moment to clear: ask again
					else logger::Write("NPC vehicles: seat rules: you get in %08X through the game's own get-in -> %s", g_intent.vehicle,
						started ? (InVehicle(player) == v ? "seated" : "getting in") : "the game refused the get-in (10 s)");
				}
			}
            if(hoverui::IsScannerOpen()){g_activateDown=false;g_intent=Intent{};return;}
			// The game's own Activate, keyboard and controller (the context is "MainGameplay"; "Gameplay" never matched).
			const bool down=Focused()&&bindings::Allowed()&&!hoverui::IsScannerOpen()&&bindings::GameHeld("MainGameplay","Activate",'E');
			const bool pressed=bindings::Started(bindings::Action::BoardPassenger)||bindings::Started(bindings::Action::PullDriver);
			const bool released=bindings::Triggered(bindings::Action::BoardPassenger);
			g_activateDown = down;
			{
				// The HUD prompts (Passenger / Hold: eject) while the player faces an occupied vehicle on foot.
				static ULONGLONG lastHud = 0;
				static bool shown = false;
				if (now - lastHud >= 200) {
					lastHud = now;
					bool occupied = false;
					Reach nearby;
					uptr fv = 0;
					if (!InVehicle(player)) {
						fv = FacingVehicle(player, &nearby);
						occupied = fv && DriverOf(fv, player);
					}
					const bool swap = fv && !InVehicle(player) && DrivenByOther(fv, player) && !(fv == g_driverFor.load() && now < g_driverUntil.load());
					g_swapKeyword = swap ? passenger::PassengerSeatKeyword(fv) : 0;
					g_swapVehicle = swap ? fv : 0;
					static uptr loggedSwap = 0;
					if ((swap ? fv : 0) != loggedSwap) {
						loggedSwap = swap ? fv : 0;
						if (swap) logger::Write("Seat choice: facing %08X with someone at the wheel; a get-in now takes its passenger seat (keyword %p)",
							At<std::uint32_t>(fv, off::kFormId), reinterpret_cast<void*>(g_swapKeyword.load()));
					}
					if (occupied || shown) hoverui::SetOccupied(occupied, bindings::GameLabel("MainGameplay", "Activate", 'E').c_str());
					// Evidence: each time the nearest NPC-driven vehicle (15 m) changes or the prompt turns on / off, why.
					static std::uint32_t loggedVehicle = 0;
					static bool loggedShown = false;
					const std::uint32_t nearId = nearby.vehicle ? At<std::uint32_t>(nearby.vehicle, off::kFormId) : 0;
					if (nearId != loggedVehicle || occupied != loggedShown) {
						if (nearId || occupied != loggedShown) {
							const char* why = occupied ? "shown" : !nearby.vehicle ? "no driven vehicle nearby" : nearby.edge > kReachRange ? "too far from its side"
								: "not facing it";
							logger::Write("NPC vehicles: seat prompt: nearest driven vehicle %08X centre %.1f m, side %.1f m (reach %.1f), facing off %.2f rad (cone %.2f) -> %s",
								nearId, nearby.centre, nearby.edge, kReachRange, nearby.off, nearby.cone, why);
						}
						loggedVehicle = nearId;
						loggedShown = occupied;
					}
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
			}
			if (bindings::Triggered(bindings::Action::PullDriver) && g_intent.occupant && !g_intent.acted) {
				// Held: pull the occupant out (a gang member hands over the wheel).
				g_intent.acted = true;
				const uptr v = RefById(g_intent.vehicle);
				const uptr occ = v ? DriverOf(v, player) : 0;
				if (!v || !occ || EdgeDistance(player, v) > kReachRange + 1.0) return;
				TakeSeat(player, v, occ, g_intent.relation == Relation::Gang ? "hand-over" : g_intent.relation == Relation::Hostile ? "hijack" : "pulled out");
				g_passengerRequest = 0; g_boardVehicle = 0; g_boardKeyword = 0; g_swapped = 0;
				if (g_intent.relation != Relation::Gang) CrimeAlarms(occ, v, player, g_intent.relation == Relation::Neutral);
				if (g_intent.relation == Relation::Hostile) Aggro(occ, player);
				logger::Write(g_intent.relation == Relation::Gang ? "They hand you the wheel" : g_intent.relation == Relation::Hostile ? "Hijacked" : "You pulled them out");
				return;
			}
			if (released && g_intent.occupant && !g_intent.acted) {
				// A tap: the passenger seat at once. The native driver entry was
				// refused before animation; clearing the occupant ends this press.
				const std::uint32_t occupant = g_intent.occupant;
				g_intent.occupant = 0;
				const uptr v = RefById(g_intent.vehicle);
				if (v && EdgeDistance(player, v) <= kReachRange + 1.0) {
					const bool ok = passenger::BoardVehicle(player, v);
					logger::Write("NPC vehicles: seat rules: tap on occupied vehicle %08X (%s driver %08X) -> passenger seat %s", g_intent.vehicle, RelationName(g_intent.relation),
						occupant, ok ? "requested (native completion pending)" : "request refused");
				}
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
				logger::Write(k == Forcer::Owner ? "Owner: \"Get out of my vehicle!\"" : k == Forcer::Guard ? "Guard: \"Step out of the vehicle!\"" : "Hijacker! Drive off!");
				logger::Write("NPC vehicles: force-out: %s %08X at your vehicle %08X (%.1f m); out in %.0f s unless you drive off", ForcerName(k), id, g_force.vehicle,
					Dist(r, pv), kForceWarnMs / 1000.0);
				return;
			}
		}

	}

	namespace
	{
		// One job's profile (escort / pursuit): pursuit puts the driver in combat with the player, escort stops the weapon. The mod
		// never calms anyone in a seat (2026-10-21, the user: "mod should never turn anyone calm in the seat"); only an NPC walking
		// to a vehicle in a fight is calmed (the get-in package waits behind combat), and its fight resumes once it sits.
		bool ApplyProfile(Job& j, Profile to, uptr player, bool provokePlayer)
		{
			if(j.scheduledTraffic)return false;
			const uptr npc = RefById(j.npc);
			if (!npc) return false;
			j.profile = to;
			if (const uptr veh = RefById(j.vehicle)) {
				const auto vp = At<game::Vec3>(veh, off::kRefPosition);
				if (to == Profile::Pursuit) {
					// Hostile: the attack profile drives and fights; a follower's escort navigation would pull it back to the player.
					const auto* nav = vcontract::AssignedTo(j.vehicle);
					if (nav && std::strcmp(nav->name, "escort") == 0) vcontract::Unassign(j.vehicle, vcontract::Class::Navigation);
					vcontract::Assign(j.vehicle, vcontract::Find("attack"), vp.x, vp.y);
					j.autoCombat = false;
				}
				else {
					const auto* cmb = vcontract::AssignedTo(j.vehicle, vcontract::Class::Combat);
					if (cmb && std::strcmp(cmb->name, "attack") == 0 && !j.autoCombat) vcontract::Unassign(j.vehicle, vcontract::Class::Combat);
					if (!vcontract::AssignedTo(j.vehicle)) vcontract::Assign(j.vehicle, vcontract::Find("escort"), vp.x, vp.y);
				}
			}
			if (kProfiles[int(to)].hostile && provokePlayer) {
				j.aggroAt = GetTickCount64();
				Aggro(npc, player);
			}
			else if(provokePlayer) {
				if (j.firing) SubmitFire(j.pilot - 1, j.vehicle, false);
				j.firing = false;
			}
			return true;
		}

		// Full simulation for traffic drivers (2026-10-30 07:57 run + decompile). FUN_141935730 picks an actor's process level from
		// its parent cell (attached 0, loaded 1, else 2/3); the actor update FUN_1418E8E50 queues a mismatch and the game applies it
		// (FUN_141A8BA60 -> FUN_1418E8AD0), as do cell events. A demotion from 0 runs FUN_1418E3660: it frees the high-process data and
		// releases a non-sitting actor's furniture (FUN_1419942C0), and at level 1 the sit animation does not run. The game's own
		// follower flag (+0x37C bit 21) keeps an actor at level 0 this same way, but it means "teammate" elsewhere, so the answer is
		// overridden for drivers on a trip only.
		constexpr std::uint32_t kDesiredLevelFn = 0x01935730;
		using DesiredLevel_t = std::uint64_t (*)(uptr);
		DesiredLevel_t g_desiredLevelOriginal = nullptr;
		std::atomic<std::uint32_t> g_fullSimVehicles[kJobs] = {};  // each listed driver's vehicle, same index
		std::atomic<std::uint32_t> g_fullSimIds[kJobs] = {};    // per job, written on the main thread, read on any
		std::atomic<std::uint64_t> g_fullSimFilter[64] = {};    // by the id's low 12 bits, so other actors skip the list
		std::atomic<std::uint32_t> g_fullSimKept{ 0 };          // times the game would have lowered a listed driver

		bool FullSimListed(std::uint32_t id)
		{
			if (!id || !(g_fullSimFilter[(id >> 6) & 63].load(std::memory_order_relaxed) >> (id & 63) & 1)) return false;
			for (const auto& s : g_fullSimIds) if (s.load(std::memory_order_relaxed) == id) return true;
			return false;
		}

		std::uint64_t DesiredLevelDetour(uptr actor)
		{
			// Model retention is not permission to force walking/pathing/high
			// process at arbitrary distance. Keep the engine's complete decision.
			return g_desiredLevelOriginal(actor);
		}

		void FullSimClear()
		{
			for (auto& w : g_fullSimFilter) w.store(0, std::memory_order_relaxed);
			for (auto& s : g_fullSimIds) s.store(0, std::memory_order_relaxed);
		}

		// Seat trace (2026-10-30 08:15 run; notes/driver-seat-chain-20261030/CHAIN.md section 7): which native code changes a trip
		// driver's sit state or releases its seat. FUN_1418EC2B0 is the sit state setter (ActorState, actor +0xF0, vtable +0x198),
		// FUN_1419942C0 the seat release, FUN_140BA9770 attaches the carrier (REFRSyncController) that moves a seated passenger.
		// Any thread. Only listed drivers, and a bounded number of lines per load.
		std::atomic<int> g_seatTraceBudget{ 4000 };
		int g_dropTraces = 0;

		std::uint32_t TraceId(uptr actor)
		{
			__try { return actor ? At<std::uint32_t>(actor, off::kFormId) : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		bool TraceWanted(std::uint32_t id) { return id && FullSimListed(id) && g_seatTraceBudget.fetch_sub(1, std::memory_order_relaxed) > 0; }

		// The callers, as decompile addresses; "dll" for code outside the game.
		__declspec(noinline) void CallerText(char* out, std::size_t n)
		{
			void* frames[10] = {};
			const USHORT count = CaptureStackBackTrace(2, 10, frames, nullptr);
			const auto base = static_cast<std::uint64_t>(game::Base());
			std::size_t used = 0;
			out[0] = 0;
			for (USHORT i = 0; i < count && used + 18 < n; ++i) {
				const auto a = reinterpret_cast<std::uint64_t>(frames[i]);
				const bool inGame = a >= base && a - base < 0x10000000ull;
				const int w = inGame ? std::snprintf(out + used, n - used, "%s%llX", i ? " " : "", a - base + 0x140000000ull)
					: std::snprintf(out + used, n - used, "%sdll", i ? " " : "");
				if (w <= 0) break;
				used += static_cast<std::size_t>(w);
			}
		}

		// The driver's rule exemption (trafficpair.h has the why).
		constexpr std::uint32_t kFullLodFlag = 0x10000;
		using ActorCellCheck_t = void (*)(uptr);
		ActorCellCheck_t g_actorCellCheckOriginal = nullptr;
		std::atomic<std::uint32_t> g_passMarked{ 0 }, g_passRefused{ 0 };
		std::atomic<std::uint64_t> g_passSkips{ 0 };
		std::atomic<bool> g_fullSimMarked[kJobs] = {};
		bool VehicleHeld(uptr vehicle) { __try { return vehicle && (At<std::uint32_t>(vehicle, 0x20) & kFullLodFlag) != 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
		void ActorCellCheckDetour(uptr actor)
		{
			// 1418EBF00 -> 1418EBF90 maintains collision representation.
			// Never skip it to retain a driver's model.
			g_actorCellCheckOriginal(actor);
		}
		// Main thread. One per pass of the list: each makes a reference (2026-10-29: a batch of reference changes in one frame was a visible hitch).

		// The driver pass (notes/driver-pass-20261031/TRACE.md): the game's own far-cell route for actors, the one flying actors use.
		constexpr std::uint32_t kFarCellEnter = 0x00BB5180, kFarCellLeave = 0x00BB53C0;  // (world, ref)
		constexpr std::uint32_t kFarCellFlag = 0x2000;  // set on a world's far cell where it is made (FUN_140BB5840)
		bool InFarCell(uptr ref)
		{
			__try { const uptr cell = At<uptr>(ref, off::kRefParentCell); return cell && (At<std::uint32_t>(cell, 0x20) & kFarCellFlag) != 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		uptr WorldOf(uptr ref)
		{
			__try { const uptr cell = At<uptr>(ref, off::kRefParentCell); return cell ? At<uptr>(cell, off::kCellWorldspace) : 0; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		// FUN_1418DB800, "too far" for an actor in a far cell: 11 callers, among them the move in, the loader and the process level. A passed driver is not too far while its vehicle has a model.
		using FarTest_t = bool (*)(uptr);
		FarTest_t g_farTestOriginal = nullptr;
		bool FarTestDetour(uptr actor)
		{
			const std::uint32_t id = TraceId(actor);
			if (id && (g_fullSimFilter[(id >> 6) & 63].load(std::memory_order_relaxed) >> (id & 63) & 1)) {
				for (int i = 0; i < kJobs; ++i) {
					if (g_fullSimIds[i].load(std::memory_order_relaxed) != id) continue;
					const std::uint32_t vehicleId = g_fullSimVehicles[i].load(std::memory_order_relaxed);
					const uptr vehicle = vehicleId ? runtimeforms::LookupChecked(vehicleId) : 0;
					// The pass also covers arrival and seat repair: a driver without a
					// model cannot already be sitting. Pair health revokes broken jobs.
					if (vehicle && VehicleHeld(vehicle) &&
						trafficpair::DriverCountsAsNear(true,InFarCell(actor)||g_fullSimMarked[i].load(std::memory_order_relaxed),Has3D(vehicle))) return false;
					break;
				}
			}
			return g_farTestOriginal(actor);
		}
		// Out of the far cell when its job is over: someone standing around lives in an ordinary cell. The game's own exit: the mover then picks the grid cell at its position.
		void LeaveFarCell(uptr actor)
		{
			if (!actor || !InFarCell(actor)) return;
			if(!lifecycle::InService()) {
				const unsigned id=TraceId(actor);
				lifecycle::Post("far-leave:"+std::to_string(id),id,[=]()->lifecycle::Work {
					const auto owned=lifecycle::Keep(actor,id);if(!owned)return {};
					return [=]()->std::uint64_t {if(lifecycle::InService())LeaveFarCell(reinterpret_cast<uptr>(owned.get()));return 1;};
				});return;
			}
			const uptr world = WorldOf(actor);
			if (!world) return;
			EngineFn<void (*)(uptr,uptr)>(kFarCellLeave)(world,actor);
		}
		void DriverPassTick(Job& j, uptr npc, uptr vehicle, ULONGLONG now)
		{
			if (!g_farTestOriginal) return;
			const bool held = VehicleHeld(vehicle), v3D = Has3D(vehicle), d3D = Has3D(npc);
            const bool observed=InFarCell(npc);
            if(observed&&!j.passMarked){++g_passMarked;j.passTries=0;logger::Write("NPC vehicles: driver pass completed for %08X / %08X",j.npc,j.vehicle);}
            j.passMarked=observed;
			if(d3D&&j.passLoadAt){
				logger::Write("OBS system=driver.pass vehicle=%08X driver=%08X event=model_ready since_request_ms=%llu far=%d aboard=%d sit=%d",j.vehicle,j.npc,static_cast<unsigned long long>(now-j.passLoadAt),observed,InVehicle(npc)==vehicle,SitState(npc));
				j.passLoadAt=0;
			}
            if (trafficpair::DriverDropsModel(j.passMarked, v3D, d3D)) {
				// The native wrapper retains and dispatches model teardown. A
				// direct vt+538 call from an SFSE worker violates that contract.
				EngineFn<void(*)(uptr,uptr)>(0x018C1AD0)(Ptr(0x05E71728),npc);
				return;
			}
			static ULONGLONG lastMove = 0;
			if (trafficpair::DriverNeedsModelRequest(observed,held,v3D,d3D,At<std::uint8_t>(npc,0xD2)==1)) {
				if (now-lastMove<250 || (j.passLoadAt && now-j.passLoadAt<3000)) return;
				const unsigned actorId=j.npc,vehicleId=j.vehicle;
				const bool posted=lifecycle::Post("driver-model:"+std::to_string(actorId),actorId,[=]()->lifecycle::Work {
					const auto a=lifecycle::Keep(npc,actorId),v=lifecycle::Keep(vehicle,vehicleId);if(!a||!v)return {};
					return [=]()->std::uint64_t {
						if(!lifecycle::InService()||!FullSimListed(actorId))return 0;
						const auto actor=reinterpret_cast<uptr>(a.get()),carrier=reinterpret_cast<uptr>(v.get());
						bool assigned=false;
						for(int i=0;i<kJobs;++i)if(g_fullSimIds[i].load()==actorId&&g_fullSimVehicles[i].load()==vehicleId)assigned=true;
						if(!assigned||Dead(actor)||!InFarCell(carrier)||WorldOf(actor)!=WorldOf(carrier)||
						   !trafficpair::DriverNeedsModelRequest(InFarCell(actor),VehicleHeld(carrier),Has3D(carrier),Has3D(actor),At<std::uint8_t>(actor,0xD2)==1))return 0;
						// Actor vt+848 (1418DBA40): checks pending state, attached cell,
						// load eligibility and queues through 14056EFA0. Call ONCE; the
						// process-list caller restarts on true, which we must never do.
						const uptr fn=At<uptr>(At<uptr>(actor,0),0x848);
						if(fn!=game::Base()+0x018DBA40)return 0;
						const bool requested=reinterpret_cast<bool(*)(uptr)>(fn)(actor);
						logger::Write("OBS system=driver.pass vehicle=%08X driver=%08X event=model_request native=%d pending=%d model=%d aboard=%d sit=%d",vehicleId,actorId,requested,At<std::uint8_t>(actor,0xD2)==1,Has3D(actor),InVehicle(actor)==carrier,SitState(actor));
						return requested?1:0;
					};
				});
				if(posted)lastMove=j.passLoadAt=now;
				return;
			}
			if(!trafficpair::DriverNeedsPass(true,held,v3D,j.passMarked)||!InFarCell(vehicle)||
			   !trafficpair::DriverCanEnterPass(d3D,InVehicle(npc)==vehicle,SitState(npc),Dist3(npc,vehicle)))return;
			if (now - lastMove < 250) return;  // like the vehicles' own holds: a move costs, one at a time
			if (j.passTries >= 3) { if (now - j.passAt < 30000) return; j.passTries = 0; }
			const uptr world = WorldOf(vehicle);
			if (!world) return;
			lastMove = j.passAt = now; ++j.passTries;
			const unsigned actorId=j.npc,vehicleId=j.vehicle;
			lifecycle::Post("far-enter:"+std::to_string(actorId),actorId,[=]()->lifecycle::Work {
				const auto a=lifecycle::Keep(npc,actorId),v=lifecycle::Keep(vehicle,vehicleId);if(!a||!v)return {};
				return [=]()->std::uint64_t {
					if(!lifecycle::InService()||!FullSimListed(actorId))return 0;
					const auto actor=reinterpret_cast<uptr>(a.get()),carrier=reinterpret_cast<uptr>(v.get());
					bool assigned=false;
					for(int i=0;i<kJobs;++i)if(g_fullSimIds[i].load()==actorId&&g_fullSimVehicles[i].load()==vehicleId)assigned=true;
					if(!assigned||TraceId(carrier)!=vehicleId||Dead(actor)||!Has3D(carrier)||
					   !trafficpair::DriverCanEnterPass(Has3D(actor),InVehicle(actor)==carrier,SitState(actor),Dist3(actor,carrier))||
					   !VehicleHeld(carrier)||!InFarCell(carrier)||InFarCell(actor))return 0;
					const auto ws=WorldOf(carrier);if(!ws)return 0;
					const auto actorWorld=WorldOf(actor);if(actorWorld&&actorWorld!=ws)return 0;
					// Only the atomic exemption table is touched here. Controller
					// Job state is observed and updated on its next ordinary tick.
					for(int i=0;i<kJobs;++i)if(g_fullSimIds[i].load()==actorId&&g_fullSimVehicles[i].load()==vehicleId)g_fullSimMarked[i].store(true);
					EngineFn<void(*)(uptr,uptr)>(kFarCellEnter)(ws,actor);
					const bool entered=InFarCell(actor);
					for(int i=0;i<kJobs;++i)if(g_fullSimIds[i].load()==actorId&&g_fullSimVehicles[i].load()==vehicleId)g_fullSimMarked[i].store(entered);
					logger::Write("OBS system=driver.pass vehicle=%08X driver=%08X event=entered far=%d model=%d aboard=%d sit=%d",vehicleId,actorId,entered,Has3D(actor),InVehicle(actor)==carrier,SitState(actor));
					return InFarCell(actor)?1:0;
				};
			});
			logger::Write("NPC vehicles: [%d] driver pass queued for %08X / %08X; awaiting native cell membership",j.pilot,j.npc,j.vehicle);
		}




		// Lists this frame's drivers and follows a driver's position every frame for 1.5 s after it leaves its vehicle (one step: an
		// old controller position came back; a smooth curve: a real fall).
		void FullSimTick(ULONGLONG now)
		{
			std::uint64_t filter[64] = {};
			int listed = 0;
			for (int i = 0; i < kJobs; ++i) {
				auto& j = g_jobs[i];
				const std::uint32_t id = j.phase != Phase::Idle && j.coldStart && j.task == 9 && trafficpair::KeepsDriverPass(trafficpair::PairHealth(trafficpool::PairHealth(j.vehicle))) ? j.npc : 0;
				g_fullSimVehicles[i].store(id ? j.vehicle : 0, std::memory_order_relaxed);
				g_fullSimMarked[i].store(id && j.passMarked, std::memory_order_relaxed);
				g_fullSimIds[i].store(id, std::memory_order_relaxed);
				if (!id) { j.wasAboard = false; continue; }
				filter[(id >> 6) & 63] |= 1ull << (id & 63);
				++listed;
				const uptr npc = RefById(id);
				const uptr vehicle = RefById(j.vehicle);
				if (!npc || !vehicle) continue;
				DriverPassTick(j, npc, vehicle, now);
				const bool aboard = InVehicle(npc) != 0;
				if (j.wasAboard && !aboard && g_dropTraces < 10) { ++g_dropTraces; j.dropTraceUntil = now + 1500; }
				j.wasAboard = aboard;
				if (now < j.dropTraceUntil) {
					const auto d = At<game::Vec3>(npc, off::kRefPosition), v = At<game::Vec3>(vehicle, off::kRefPosition);
					game::Vec3 m{};
					const bool ok = ScenePos(npc, m);
					logger::Write("NPC vehicles: [%d] drop trace: driver %08X ref (%.1f, %.1f, %.1f) model (%.1f, %.1f, %.1f)%s, vehicle (%.1f, %.1f, %.1f), sit %d, level %d",
						j.pilot, id, d.x, d.y, d.z, m.x, m.y, m.z, ok ? "" : " (no model)", v.x, v.y, v.z, SitState(npc), ProcessLevel(npc));
				}
			}
			for (int i = 0; i < 64; ++i) g_fullSimFilter[i].store(filter[i], std::memory_order_relaxed);
			static ULONGLONG reportAt = 0;
			static std::uint32_t reported = 0;
			if (now - reportAt >= 30000) {
				reportAt = now;
				const auto kept = g_fullSimKept.load(std::memory_order_relaxed);
				if (kept != reported) logger::Write("NPC vehicles: full simulation: %d traffic driver(s) listed; the game would have lowered them %u time(s) so far", listed, kept);
				reported = kept;
				static std::uint64_t passReported = 0;
				const std::uint64_t skips = g_passSkips.load(std::memory_order_relaxed);
				if (skips != passReported) logger::Write("NPC vehicles: driver pass so far: %u driver(s) exempt, %u refused, cell check skipped %llu time(s)", g_passMarked.load(), g_passRefused.load(), static_cast<unsigned long long>(skips));
				passReported = skips;
			}
		}
	}

	void Tick()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) { FullSimClear(); return; }
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
			FullSimClear();
			return;
		}
		deadDone = false;
		if (const auto own = vehicleflight::OwnRoverId()) {
			if (const uptr ov = RefById(own)) { const auto op = At<game::Vec3>(ov, off::kRefPosition); ApplyStance(own, op.x, op.y); }
		}
		EscortContextTick(player, GetTickCount64());
		PickUps(player, GetTickCount64());
		FollowersRide(player, GetTickCount64());
		ChooseKey();
		const bool wdown = g_walkKey && Focused() && (GetAsyncKeyState(g_walkKey) & 0x8000) != 0;
		if (wdown && !g_walkDown) Force(player);
		g_walkDown = wdown;
#ifndef FVO_RELEASE
        const bool fastDown=g_fastSeatKey&&Focused()&&(GetAsyncKeyState(g_fastSeatKey)&0x8000)!=0;
        if(fastDown&&!g_fastSeatDown&&bindings::Allowed()&&!hoverui::IsScannerOpen()&&!InVehicle(player))FastSeatStart(player);
        g_fastSeatDown=fastDown;
        FastSeatTick(player);
        PairMoveKey(player);
        FarTick(player);
#endif
		const bool xdown = g_exitKey && Focused() && (GetAsyncKeyState(g_exitKey) & 0x8000) != 0;
		if (xdown && !g_exitDown) ExitAllJobs();
		g_exitDown = xdown;
		pilotquest::Tick();
		FullSimTick(GetTickCount64());
		if (const auto cold = g_ignoredColdExit.exchange(0))
			logger::Write("NPC vehicles: cold-start driver %08X: its AI wants to dismount; refused while its autopilot flies it", cold);
		for(auto&pending:g_nativeExits)if(const auto id=pending.exchange(0)) {
			if(const auto actor=RefById(id);actor && InVehicle(actor)) {
				vcontract::ExitRequest req;req.timeoutMs=8000;StartExit(actor,req);
			}
		}
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
				if (ApplyProfile(j, to, player)) ++n;
			}
			logger::Write(to == Profile::Pursuit ? "Vehicles: pursuit" : "Vehicles: escort");
			logger::Write("NPC vehicles: profile %s (%d following)", kProfiles[int(to)].name, n);
		}
		g_aggroDown = adown;
		{
			const ULONGLONG nowMs = GetTickCount64();
			SquadTick(nowMs);
			RamTick(nowMs);
			bool anyAi = false;
			for (const auto& j : g_jobs) anyAi = anyAi || j.aiOn;
			if (anyAi) ++g_aiFrames;
		}
		for (int i = 0; i < kJobs; ++i) {
			g_jobs[i].pilot = i + 1;
			if (g_jobs[i].phase != Phase::Idle) Update(g_jobs[i], player);
			if (g_jobs[i].phase == Phase::Following && !g_jobs[i].firing) PushDrive(g_jobs[i]);
		}
		{
			// One line per NPC vehicle / rider: "pursuit  REV8  120 m".
			static char line[hoverui::kMaxRows][80];
			const char* ptrs[hoverui::kMaxRows];
			int nr = 0;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			{
				static char life[200];
				const bool bars = features::On(features::Id::HealthBars);
				hoverui::SetLifebar(bars && LifebarLine(player, life, sizeof(life)) ? life : "");  // the status card's health line
				// The bar under the game's enemy bar, while it shows a vehicle's driver (vehiclehits::EnemyBarVehicle).
				const std::uint32_t barId = bars ? vehiclehits::EnemyBarVehicle() : 0;
				const uptr barVeh = RefById(barId);
				char barName[15] = "";  // the game's own enemy names stop at 14 characters
				if (barVeh) std::snprintf(barName, sizeof(barName), "%s", BaseName(barVeh)[0] ? BaseName(barVeh) : "Vehicle");
				hoverui::SetVehicleBar(barName, barVeh ? vehiclehits::HealthShare(barId) : 0.0f);
			}
			for (const auto& j : g_jobs) {
				if (j.phase == Phase::Idle || !IsFollower(player,RefById(j.npc)) || nr >= hoverui::kMaxRows) continue;
				const uptr v = RefById(j.vehicle);
				char name[40] = "";
				ActorName(RefById(j.npc), j.npc, name, sizeof(name));  // who, not which vehicle
				const double d = v ? Dist(v, player) : 0.0;
				if (j.phase == Phase::Following && !j.task) {
					// The approved row: an active override alone, else NAV (· COMBAT in a fight) (· HOLD FIRE).
					const auto mode = ModeOf(j.order);
					const char* over = mode == overrides::Mode::Recall ? "SUMMON" : mode == overrides::Mode::Cover ? overrides::CoverStatus(InCombat(player)) : mode == overrides::Mode::Guard ? "GUARD POINT" :
						j.order == int(CrewOrder::Hold) ? "HOLDING" : nullptr;
					const auto* nav = vcontract::AssignedTo(j.vehicle);
					const auto* cmb = vcontract::AssignedTo(j.vehicle, vcontract::Class::Combat);
					const auto* wpn = vcontract::AssignedTo(j.vehicle, vcontract::Class::Weapons);
					const std::string state = profileclass::State(over, nav ? nav->name : nullptr, cmb ? cmb->name : nullptr, j.fighting,
						profileclass::WeaponsFor(wpn ? wpn->name : nullptr));
					if (profileclass::ShowsDistance(over)) std::snprintf(line[nr], sizeof(line[nr]), "%s  %s  %.0f m", name, state.c_str(), d);
					else std::snprintf(line[nr], sizeof(line[nr]), "%s  %s", name, state.c_str());
				}
				else {
					const char* state = j.phase == Phase::Following ? "driving" : j.passenger ? "boarding as passenger" : j.phase == Phase::Boarding ? "boarding" : "getting out";
					std::snprintf(line[nr], sizeof(line[nr]), "%s  %s  %.0f m", name, state, d);
				}
				ptrs[nr] = line[nr];
				++nr;
			}
			for (const auto& r : g_riders) {
				if (nr >= hoverui::kMaxRows) break;
				if(!IsFollower(player,RefById(r.npc)))continue;
				const uptr v = RefById(r.vehicle);
				char name[40] = "";
				ActorName(RefById(r.npc), r.npc, name, sizeof(name));
				std::snprintf(line[nr], sizeof(line[nr]), "%s  riding  %.0f m", name, v ? Dist(v, player) : 0.0);
				ptrs[nr] = line[nr];
				++nr;
			}
			(void)pp;
			hoverui::SetRows(nr, ptrs);
		}
	}

	namespace
	{
		// Native exit intent is allowed immediately and queued for bounded main-thread cleanup.
		constexpr std::uint32_t kDismountFn = 0x01952530;
		using Dismount_t = char (*)(uptr);
		Dismount_t g_dismountOriginal = nullptr;

		char DismountDetour(uptr actor)
		{
			if(!g_allowDismount && actor) {
				const auto id=At<std::uint32_t>(actor,off::kFormId);
				for(int i=0;i<kJobs;++i)if(g_jobs[i].npc==id && g_jobs[i].phase==Phase::Following) {
					// 2026-10-28 18:07 run: every cold-start driver's AI asked to dismount within seconds of each trip start (it was
					// seated directly, not by the game's get-in, so its own package keeps wanting to walk). The game refused each exit,
					// but turning the wish into an exit stopped the trip. On its scripted trip it is ignored; the game decides as before.
					// Task 4 too: when a trip ends the vehicle flies down to park, and an exit there left a driver 193 m up (2026-11-01 01:37). It gets out once parked.
					if (g_jobs[i].coldStart && (g_jobs[i].task == 9 || g_jobs[i].task == 4)) {
						if (!g_jobs[i].coldStartLogged) { g_jobs[i].coldStartLogged = true; g_ignoredColdExit.store(id); }
						// Refused while its autopilot flies it (07:16 run: a sitting driver stood up and fell out in mid-air). Only during
						// the trip: when it ends the job leaves Following and the game decides as before.
						if (vehicleflight::PilotActive(i + 1)) return 0;
						continue;
					}
					g_nativeExits[i].store(id);
				}
			}
			// Native AI owns its exit intent. Never trap a driver in an endless refusal loop.
			return g_dismountOriginal(actor);
		}

		using ApplyInput_t = void (*)(uptr, uptr);
		ApplyInput_t g_applyOriginal = nullptr;
		unsigned g_gateOpened = 0;

		void ApplyInputDetour(uptr component, uptr input)
		{
			bool ours = false;
			for (int i = 0; i < kJobs + 1; ++i) {
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
                }else if(vehicleflight::PlayerDriving(At<std::uint32_t>(component,0xC))&&bindings::Allowed()){
                    auto* in=reinterpret_cast<std::uint8_t*>(input);
                    if(hoverui::Editing()||!hoverui::IsScannerOpen()){
                        const bool edit=hoverui::Editing();
                        const float right=edit?0:float(bindings::Amount(bindings::Action::TurnRight,'D'));
                        const float left=edit?0:float(bindings::Amount(bindings::Action::TurnLeft,'A'));
                        float accel=edit?0:float(bindings::Amount(bindings::Action::Accelerate,'W'));
                        const float brake=edit?0:float(bindings::Amount(bindings::Action::Brake,'S'));
                        const bool boost=!edit&&bindings::Held(bindings::Action::Boost,VK_LSHIFT);
                        if(boost&&brake<=0)accel=1;
                        *reinterpret_cast<float*>(in+0x10)=right-left;
                        *reinterpret_cast<float*>(in+0x14)=accel;
                        *reinterpret_cast<float*>(in+0x18)=brake;
                        in[0x1C]=boost?1:0;
                        in[0x1D]=0; // no native L3 toggle survives a mod hover toggle
                        in[0x1E]=!edit&&bindings::Held(bindings::Action::VerticalBoost,VK_SPACE)?1:0;
                    }
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
			static int budget = 40;  // with verbose logging the AI's own calls are logged too (a limited number)
			if (g_traceSeat || (budget > 0 && logger::Verbose())) {
				if (!g_traceSeat) --budget;
				const std::uint32_t n = a9 ? At<std::uint32_t>(a9, 0) : 0u;
				logger::Write("SEAT TRACE: entry-point finder FUN_14062FC20 (race/skeleton %p, flags %u) found %u entry point(s)%s", reinterpret_cast<void*>(a1), unsigned(a3), n, g_traceSeat ? "" : " [AI]");
			}
		}

		std::uint64_t PoseDetour(float f, uptr a2, uptr a3, float* a4, uptr a5, float* a6, float* a7, float* a8, float* a9)
		{
			const std::uint64_t r = g_poseOrig(f, a2, a3, a4, a5, a6, a7, a8, a9);
			static int budget = 40;
			if (g_traceSeat || (budget > 0 && logger::Verbose())) {
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
		// Whole, non-RIP-relative instructions verified against the installed
		// 1.16.244 executable. Refuse installation if either function changes.
		static const std::uint8_t kEntryPrologue[18] = { 0x40,0x55,0x53,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,0x48,0x8D,0x6C,0x24,0xE1 };
		static const std::uint8_t kSitPrologue[16] = { 0x48,0x8B,0xC4,0x48,0x89,0x58,0x18,0x48,0x89,0x48,0x08,0x55,0x56,0x57,0x41,0x54 };
		void* entryOriginal = nullptr;
		if (hook::Install(game::Base()+kPlayerEntry, kEntryPrologue, sizeof(kEntryPrologue), reinterpret_cast<void*>(&PlayerEntryDetour), &entryOriginal)) {
			g_playerEntryOriginal = reinterpret_cast<PlayerEntry_t>(entryOriginal);
			logger::Write("Boarding gate: pre-animation player entry hook installed (141A66BA0)");
		} else logger::Write("Boarding gate: pre-animation hook FAILED; late player cleanup remains disabled");
		{
			// 02:11 run: patching FUN_1412B2C40's own start failed (bytes differ: another mod hooks it). Its two callers in the
			// get-in path are redirected instead (both checked to call it); the original is called through its address, so
			// whatever else hooks it still runs.
			const uptr fn = game::Base() + kSeatKeyword;
			g_seatKeywordOriginal = reinterpret_cast<SeatKeyword_t>(fn);
			const std::uint8_t* at = reinterpret_cast<const std::uint8_t*>(fn);
			logger::Write("Seat choice: FUN_1412B2C40 starts %02X %02X %02X %02X %02X %02X", at[0], at[1], at[2], at[3], at[4], at[5]);
			// Every call site of FUN_1412B2C40 (02:34 run: the two get-in calls alone never ran for the player; the player's
			// activation goes FUN_14192A3F0 -> the seat check FUN_14063AF00 -> here).
			static const std::uint32_t kSites[] = { 0x01A05E26, 0x019955B3, 0x0063AF4F, 0x012B4E6C, 0x012B3954 };
			char line[160] = "Seat choice: call sites";
			for (const auto site : kSites) {
				const bool ok = hook::RedirectCall(game::Base() + site, reinterpret_cast<void*>(fn), reinterpret_cast<void*>(&SeatKeywordDetour));
				char part[32];
				std::snprintf(part, sizeof part, " 14%07X %s", site, ok ? "ok" : "NO");
				strncat_s(line, part, _TRUNCATE);
			}
			logger::Write("%s", line);
		}
		void* sitOriginal = nullptr;
		if (hook::Install(game::Base()+kSitAction, kSitPrologue, sizeof(kSitPrologue), reinterpret_cast<void*>(&SitActionDetour), &sitOriginal)) {
			g_sitActionOriginal = reinterpret_cast<SitAction_t>(sitOriginal);
			logger::Write("Boarding gate: actor-scoped sit hook installed (1419F7100)");
		} else logger::Write("Boarding gate: sit-actor hook FAILED; unscoped restricted completions will be suppressed");
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
			logger::Write("NPC vehicles: dismount hook installed (native exits allowed; bounded cleanup)");
		}
		else {
			logger::Write("NPC vehicles: dismount hook FAILED (bytes differ)");
		}
		logger::Write("NPC vehicles: native process-level selection preserved (no forced high process)");
        // Preserve native 1418EBF00/1418EBF90 collision housekeeping.
        {  // driver pass: always on (2026-11-05 settings strip; was settings.ini [traffic] driverPass, which the user ran on)
            static const std::uint8_t prologue[]={0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x48};
            void* farOriginal=nullptr;
            if(hook::Install(game::Base()+0x018DB800,prologue,sizeof(prologue),reinterpret_cast<void*>(&FarTestDetour),&farOriginal))g_farTestOriginal=reinterpret_cast<FarTest_t>(farOriginal);
            logger::Write("NPC vehicles: driver pass %s; native cell/physics housekeeping preserved",g_farTestOriginal?"queued":"unavailable");
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

	int FlownVehicles(std::uint32_t* vehicles, std::uint32_t* drivers, int max)
	{
		int n = 0;
		for (const auto& j : g_jobs) {
			if (n >= max) break;
			if (j.phase == Phase::Idle || !j.vehicle) continue;
			vehicles[n] = j.vehicle; drivers[n] = j.npc; ++n;
		}
		return n;
	}

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

    std::uint64_t RelocationMark(std::uint32_t vehicle)
    {
#ifndef FVO_RELEASE
        std::lock_guard lock(g_relocMarkLock);
        const auto found = g_relocMarks.find(vehicle);
        return found == g_relocMarks.end() ? 0 : found->second;
#else
        (void)vehicle;
        return 0;
#endif
    }

    std::uint64_t PoseGeneration(std::uint32_t vehicle)
    {
#ifndef FVO_RELEASE
        const auto value=g_relocPose.load(std::memory_order_acquire);
        if(vehicle&&std::uint32_t(value)==vehicle)return 0;
        return value>>32;
#else
        (void)vehicle;
        return 1;
#endif
    }
	bool RequestExit(std::uintptr_t actor, const vcontract::ExitRequest& req) { return StartExit(actor, req); }

	int UnloadVehicle(std::uintptr_t vehicle, const vcontract::ExitRequest& req, bool includeDriver)
	{
		int n = 0;
		const std::uint32_t vid = At<std::uint32_t>(vehicle, off::kFormId);
		for (const uptr r : modscene::LoadedRefs(8000)) {
			if (!IsActorRef(r) || InVehicle(r) != vehicle) continue;
			if (req.mode == vcontract::ExitMode::Eject && r == Ptr(addr::kPlayerCharacterPtr)) continue;
			bool isDriver = false;
			for (const auto& j : g_jobs) {
				if (j.phase != Phase::Idle && j.vehicle == vid && j.npc == At<std::uint32_t>(r, off::kFormId)) isDriver = true;
			}
			if (isDriver && !includeDriver) continue;
			if (StartExit(r, req)) ++n;
		}
		return n;
	}

	// board(actor, vehicle, asDriver): the NPC walks to the vehicle (running when in combat), gets in (the instant seat; the animated entry is paused, kAnimatedEntry) and
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
			if (!StartWalkBoard(&j, actor, vehicle)) return false;
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			j.homeX = vp.x; j.homeY = vp.y; j.haveHome = true;
			return true;
		}
		return false;
	}

    int ClaimScheduled(std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle||Dead(actor))return 0;
        const auto a=At<std::uint32_t>(actor,off::kFormId),v=At<std::uint32_t>(vehicle,off::kFormId);
        if(v==vehicleflight::OwnRoverId()||Busy(a)||Busy(v)||vehicleflight::PlayerDriving(v))return 0;
        for(auto&j:g_jobs)if(j.phase==Phase::Idle){
            j=Job{};j.pilot=int(&j-g_jobs)+1;j.npc=a;j.vehicle=v;
            j.scheduledTraffic=true;j.phase=Phase::Following;j.since=GetTickCount64();return j.pilot;
        }
        return 0;
    }
    bool ScheduledLease(int pilot,std::uint32_t actor,std::uint32_t vehicle) {
        if(pilot<1||pilot>kJobs)return false;
        const auto&j=g_jobs[pilot-1];return j.scheduledTraffic&&j.phase!=Phase::Idle&&j.npc==actor&&j.vehicle==vehicle;
    }
    void ReleaseScheduled(int pilot,std::uint32_t expectedVehicle) {
        if(pilot<1||pilot>kJobs)return;
        auto&j=g_jobs[pilot-1];if(!j.scheduledTraffic||j.vehicle!=expectedVehicle)return;
        vehicleflight::PilotStop(pilot,"scheduled traffic lease released");pilotquest::Release(pilot-1);j=Job{};
    }
    bool PrepareScheduledSeat(std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle||Dead(actor))return false;
        EnsureSeatKeyword(actor,vehicle);return CanMount(actor,vehicle);
    }
	bool MayUse(std::uintptr_t actor, std::uintptr_t vehicle)
	{
		if (!actor || !vehicle) return false;
		if (At<std::uint32_t>(vehicle, off::kFormId) == vehicleflight::OwnRoverId()) return false;
		const uptr owner = OwnerForm(vehicle);
		if (!owner) return true;
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (player && owner == At<uptr>(player, 0x98)) return false;
		const uptr base = At<uptr>(actor, 0x98);
		return base == owner || (At<std::uint8_t>(owner, 0x2E) == 0x0D && InFaction(actor, owner));
	}

	bool PilotFighting(int pilot)
	{
		for (const auto& j : g_jobs) if (j.pilot == pilot && j.phase == Phase::Following) return j.fighting;
		return false;
	}

	bool PilotFleeing(int pilot)
	{
		for (const auto& j : g_jobs) if (j.pilot == pilot && j.phase == Phase::Following && j.aiOn)
			return j.ai.dec.action == brain::Action::Retreat || j.ai.dec.action == brain::Action::Flee || j.retreating;
		return false;
	}

	// The player's way out of a wreck (2026-10-28 22:23 run: the forced exit left the player in the sitting pose, "animation
	// reset not taken"). First the game's own animated get-out (what E does), else its get-up (actor vtable +0xB30, the kick-out's
	// way). The caller falls back to ExitPlayer(true) if the player is still seated a moment later.
	bool BeginPlayerExit()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !InVehicle(player)) return false;
		g_allowDismount = true;
		const int dismount = Dismount(player);
		g_allowDismount = false;
		if (dismount == 1) { logger::Write("NPC vehicles: player exit: the game's own get-out started"); return true; }
		const bool asked = StandUp(player);
		logger::Write("NPC vehicles: player exit: the game's get-out refused (%d); %s", dismount,
			asked ? "asked the game's get-up (actor vtable +0xB30)" : "the get-up faulted");
		return asked;
	}

	bool ExitPlayer(bool force)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player || !InVehicle(player)) return false;
		if (!force) return Dismount(player)==1;
		const auto vehicle=InVehicle(player);
		double point[3]{};bool havePoint=false;static thread_local world::Snapshot snapshot;
		if(world::Vehicles(snapshot))for(unsigned i=0;i<snapshot.count&&i<world::kMaxVehicles;++i)if(snapshot.v[i].ref==vehicle){
			const auto& hull=snapshot.v[i];
			for(int side:{1,-1})if(integrationlogic::EjectPosition(HullBox(hull),hull.heading,side,point)){
				const float from[]={hull.center[0],hull.center[1],hull.center[2]+1},to[]={float(point[0]),float(point[1]),float(point[2])+1};
				if(!environment::LineBlocked(from,to)){havePoint=true;break;}
			}
			break;
		}
		g_allowDismount = true;
		const bool out = InstantExit(player,true) && !InVehicle(player);
		g_allowDismount = false;
		if(out){
			const auto pos=At<game::Vec3>(player,off::kRefPosition);
			WarpExitController(player,havePoint?float(point[0]):pos.x,havePoint?float(point[1]):pos.y,havePoint?float(point[2]):pos.z);
		}
		return out;
	}

	bool FighterMayTake(std::uintptr_t actor, std::uintptr_t vehicle)
	{
		if (!actor || !vehicle || At<std::uint32_t>(vehicle, off::kFormId) == vehicleflight::OwnRoverId()) return false;
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		const uptr base = At<uptr>(actor, 0x98);
		auto belongs = [&](uptr owner) {
			if (!owner || (player && owner == At<uptr>(player, 0x98))) return false;
			return owner == base || (At<std::uint8_t>(owner, 0x2E) == 0x0D && InFaction(actor, owner));
		};
		if (const uptr owner = OwnerForm(vehicle)) return belongs(owner);
		// Unowned: in an owned place (a settlement, a town, a camp) only the place's own people take it; out in the open anyone may
		// (2026-10-21, the user: unowned vehicles outside any settlement are free for enemies too).
		const uptr place = realvehicles::PlaceOwner(vehicle);
		return !place || belongs(place);
	}

	bool ActorFree(std::uintptr_t actor)
	{
		if (!actor || !IsActorRef(actor) || Dead(actor) || InVehicle(actor)) return false;
		// Quest NPCs are never taken for civil traffic: essential (actor +0x378 bit 18) or protected (bit 19), the bits the game's own
		// IsEssential / IsProtected conditions read (FUN_140bed740, FUN_140c566a0; 2026-10-20 research). A quest alias that makes an actor essential most likely sets the same bit (unproven).
		if (At<std::uint32_t>(actor, 0x378) & ((1u << 18) | (1u << 19))) return false;
		const std::uint32_t id = At<std::uint32_t>(actor, off::kFormId);
		if (Locked(id) || Busy(id)) return false;
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		return !(player && IsFollower(player, actor));
	}

	namespace
	{
		// Reinforcements (2026-10-20): a pursuer pulled out; up to 3 calm NPCs within 150 m who are hostile to the player join the fight
		// (the fight rules then put them into vehicles near them).
		void CallReinforcements(uptr player, uptr at)
		{
			if (!player || !at) return;
			int n = 0;
			for (const uptr r : modscene::LoadedRefs(8000)) {
				if (n >= 3) break;
				if (!r || r == player || !IsActorRef(r) || Dead(r) || InCombat(r) || IsFollower(player, r)) continue;
				if (Dist(r, at) > 150.0 || !HostileTo(r, player)) continue;
				if (!CanMount(r, at)) continue;  // people only (the game's own mount check: creatures cannot ride)
				if (Aggro(r, player)) {
					++n;
					logger::Write("NPC vehicles: reinforcement: NPC %08X joins the fight (%.0f m)", At<std::uint32_t>(r, off::kFormId), Dist(r, at));
				}
			}
		}

		// Pick-ups (2026-10-20), every 2 s: a crew job following the player takes free followers within 40 m as passengers (squad); any vehicle in
		// a fight takes a wounded ally on foot within 50 m (health under 30%) as a passenger: escorts a follower, pursuers a hostile, who then retreats.
		// 2026-10-28 (the user: with a follower, getting in should be "the same as theirs": the follower takes the other seat, as
		// NPC passengers do). While the player drives a stopped vehicle, followers within 40 m board its free passenger seats, one
		// a second; when the player gets out, the followers riding in it get out too.
		std::uint32_t g_rideVehicle = 0;
		ULONGLONG g_rideAt = 0;
		game::Vec3 g_ridePos{};
		void FollowersRide(uptr player, ULONGLONG now)
		{
			const uptr pv = InVehicle(player);
			const std::uint32_t pid = pv ? At<std::uint32_t>(pv, off::kFormId) : 0;
			uptr gang[32];
			if (pv && passenger::RidingVehicle() != pid) {
				if (now - g_rideAt < 1000) return;
				g_rideAt = now;
				const auto p = At<game::Vec3>(pv, off::kRefPosition);
				const bool stopped = g_rideVehicle == pid && std::hypot(p.x - g_ridePos.x, p.y - g_ridePos.y) < 2.0;
				g_rideVehicle = pid;
				g_ridePos = p;
				if (!stopped) return;
				const int n = Followers(player, gang, 32);
				for (int i = 0; i < n; ++i) {
					const uptr f = gang[i];
					if (!f || Dead(f) || InVehicle(f) || Busy(At<std::uint32_t>(f, off::kFormId)) || Dist(f, pv) > 40.0) continue;
					if (BoardNpc(f, pv, false)) {
						logger::Write("NPC vehicles: follower %08X takes a passenger seat in your vehicle %08X", At<std::uint32_t>(f, off::kFormId), pid);
						return;
					}
				}
				return;
			}
			if (!g_rideVehicle) return;
			const uptr v = RefById(g_rideVehicle);
			const std::uint32_t was = g_rideVehicle;
			g_rideVehicle = 0;
			if (!v || pv == v) return;
			const int n = Followers(player, gang, 32);
			for (int i = 0; i < n; ++i) {
				if (!gang[i] || InVehicle(gang[i]) != v) continue;
				const bool ok = StartExit(gang[i], vcontract::ExitRequest{});
				logger::Write("NPC vehicles: you got out of %08X: follower %08X gets out -> %s", was, At<std::uint32_t>(gang[i], off::kFormId), ok ? "ok" : "refused");
			}
		}

		ULONGLONG g_pickAt = 0;
		void PickUps(uptr player, ULONGLONG now)
		{
			if (now - g_pickAt < 2000) return;
			g_pickAt = now;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Following || j.task) continue;
				const uptr v = RefById(j.vehicle), npc = RefById(j.npc);
				if (!v || !npc || InVehicle(npc) != v) continue;
				uptr gang[32];
				const int n = Followers(player, gang, 32);
				if (j.order == int(CrewOrder::Follow) || j.order == int(CrewOrder::Cover)) {
					for (int i = 0; i < n; ++i) {
						const uptr f = gang[i];
						if (!f || f == npc || Dead(f) || InVehicle(f) || Dist(f, v) > 40.0) continue;
						if (BoardNpc(f, v, false)) {
							logger::Write("NPC vehicles: [%d] squad: follower %08X boards as a passenger", j.pilot, At<std::uint32_t>(f, off::kFormId));
							return;  // one a round
						}
					}
				}
				if (!InCombat(player) && !InCombat(npc)) continue;
				if (j.profile == Profile::Escort) {
					for (int i = 0; i < n; ++i) {
						const uptr f = gang[i];
						float share = 1.0f;
						if (!f || f == npc || Dead(f) || InVehicle(f) || Dist(f, v) > 50.0 || !vehiclehits::ActorHealthShare(f, share) || share >= 0.3f) continue;
						if (!CanMount(f, v)) continue;
						if (BoardNpc(f, v, false)) {
							logger::Write("NPC vehicles: [%d] picks up wounded follower %08X (%.0f%%)", j.pilot, At<std::uint32_t>(f, off::kFormId), share * 100.0f);
							FollowerNotice(j.npc,"A follower vehicle picks up a wounded follower");
							return;
						}
					}
				}
				else if (j.profile == Profile::Pursuit && !j.retreating) {
					for (const uptr r : modscene::LoadedRefs(8000)) {
						if (!r || r == npc || r == player || !IsActorRef(r) || Dead(r) || InVehicle(r) || Dist(r, v) > 50.0) continue;
						if (IsFollower(player, r) || !HostileTo(r, player) || !CanMount(r, v)) continue;  // people only
						float share = 1.0f;
						if (!vehiclehits::ActorHealthShare(r, share) || share >= 0.3f) continue;
						if (BoardNpc(r, v, false)) {
							j.retreating = true;
							logger::Write("NPC vehicles: [%d] pursuer picks up wounded %08X (%.0f%%) and pulls out", j.pilot, At<std::uint32_t>(r, off::kFormId), share * 100.0f);
							return;
						}
					}
				}
			}
		}

		bool StartTask(uptr actor, uptr vehicle, int task, double x, double y)
		{
			if (!ActorFree(actor) || !vehicle) return false;
			const std::uint32_t vehId = At<std::uint32_t>(vehicle, off::kFormId);
			if (Busy(vehId) || vehicleflight::AutopilotDisabled(vehId)) return false;
			if (!DriverSeatKeyword(vehicle) && !CanMount(actor, vehicle)) return false;
			for (auto& j : g_jobs) {
				if (j.phase != Phase::Idle) continue;
				j.pilot = int(&j - g_jobs) + 1;
				if (!StartWalkBoard(&j, actor, vehicle)) return false;
				const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
				j.homeX = vp.x; j.homeY = vp.y; j.haveHome = true;
				j.task = task;
				j.taskX = x; j.taskY = y;
				j.taskStep = 0;
				j.taskAt = GetTickCount64();
				logger::Write("NPC vehicles: [%d] civil task %s: NPC %08X takes vehicle %08X%s", j.pilot, task == 1 ? "transit" : task == 5 ? "supply run" : "patrol",
					At<std::uint32_t>(actor, off::kFormId), vehId, task == 1 ? " for a long walk" : task == 5 ? " to the next settlement" : " round its post");
				return true;
			}
			return false;
		}
	}

	bool StartTransit(std::uintptr_t actor, std::uintptr_t vehicle, double x, double y) {
		traffic::Destination destination;
		if (!traffic::DestinationOf(actor,destination)||!destination.vehicleSuitable ||
			std::hypot(destination.x-x,destination.y-y)>=2.0 || !StartTask(actor,vehicle,1,x,y)) return false;
		const uptr place = realvehicles::PlaceOwner(vehicle);
		for (auto& j : g_jobs) if (j.phase != Phase::Idle && j.npc == At<std::uint32_t>(actor, off::kFormId)) j.startPlace = place ? At<std::uint32_t>(place, off::kFormId) : 0;
		return true;
	}
	bool StartSupply(std::uintptr_t actor, std::uintptr_t vehicle, double x, double y) { return StartTask(actor, vehicle, 5, x, y); }
	bool RequestPassengerBoard(std::uintptr_t player, std::uintptr_t vehicle)
	{
		if (!player || !vehicle) return false;
		const auto id = At<std::uint32_t>(vehicle, off::kFormId);
		if (passenger::RidingVehicle() == id && InVehicle(player) == vehicle) return true;
		const auto in = InVehicle(player);
		const auto keyword = passenger::PassengerSeatKeyword(vehicle);
		if ((in && in != vehicle) || !keyword || vehiclehits::Wrecked(id) || EdgeDistance(player, vehicle) > kReachRange + 1.0 ||
			(!DriverOf(vehicle, player) && !FurnitureDriver(vehicle, player))) {
			logger::Write("Passenger: boarding request for %08X refused (seat, driver, reach or vehicle state)", id);
			return false;
		}
		if (g_passengerRequest == id) return true;
		const auto now = GetTickCount64();
		g_passengerRequest = id; g_passengerAttempt = {now, false};
		g_boardKeyword = keyword; g_boardVehicle = vehicle; g_boardUntil = now + 10000;
		// Native entry determines reachability; the separate exit-site/ground probe was
		// rejecting entries which native Activate completed successfully in the logs.
		logger::Write("Passenger: queued native boarding for %08X; passenger seat held through the animation", id);
		return true;
	}

	bool StartScripted(std::uintptr_t actor, std::uintptr_t vehicle, const FvoTripClaim& claim)
	{
		if (!vehicle || !claim.step) return false;
		const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
		if (!StartTask(actor, vehicle, 9, vp.x, vp.y)) return false;
		for (auto& j : g_jobs) if (j.phase != Phase::Idle && j.npc == At<std::uint32_t>(actor, off::kFormId)) {
			j.trip = claim;
			logger::Write("NPC vehicles: [%d] %s: NPC %08X takes vehicle %08X", j.pilot, claim.name ? claim.name : "scripted trip", j.npc, j.vehicle);
		}
		return true;
	}

	bool EndJobOf(std::uint32_t vehicleId, const char* why)
	{
		for (auto& j : g_jobs) {
			if (j.phase == Phase::Idle || j.vehicle != vehicleId) continue;
			vehicleflight::PilotStop(j.pilot, why);
			logger::Write("NPC vehicles: [%d] job of vehicle %08X ended: %s", j.pilot, vehicleId, why);
			Set(j, Phase::Idle);
			return true;
		}
		return false;
	}

	const char* ScriptedRefusal(std::uintptr_t actor, std::uintptr_t vehicle)
	{
		if (!actor || !vehicle) return "no vehicle or driver";
		if (IsDead(actor)) return "the driver is dead";
		if (InVehicle(actor) != vehicle) return "the driver is not in this vehicle";
		if(!Has3D(actor)||!Has3D(vehicle))return "vehicle/driver 3D is not ready";
		if(SitState(actor)<=0)return "the driver has no seated pose";
		if(!SeatedNow(actor,vehicle,true))return "the seated driver is detached from the vehicle";
		const std::uint32_t vehId = At<std::uint32_t>(vehicle, off::kFormId), npcId = At<std::uint32_t>(actor, off::kFormId);
		static thread_local world::Snapshot trafficWorld;
		bool bodyReady=false;
		if(world::Vehicles(trafficWorld))for(unsigned i=0;i<trafficWorld.count;++i)if(trafficWorld.v[i].formId==vehId&&trafficpair::Body(trafficWorld.v[i].bodyId)){bodyReady=true;break;}
		if(!bodyReady)return "the vehicle has no live chassis in this world";
		if (Busy(vehId)) return "the vehicle already has an NPC job";
		if (Busy(npcId)) return "the driver already has an NPC job";
		if (vehicleflight::AutopilotDisabled(vehId)) return "autopilot is off for this vehicle";

		for (const auto& j : g_jobs) if (j.phase == Phase::Idle) return "";
		return "every NPC job slot is in use";
	}

	bool StartScriptedSeated(std::uintptr_t actor, std::uintptr_t vehicle, const FvoTripClaim& claim)
	{
		if(*ScriptedRefusal(actor,vehicle))return false;
		if (!actor || !vehicle || !claim.step || IsDead(actor) || !SeatedNow(actor,vehicle,InVehicle(actor)==vehicle)) return false;
		const std::uint32_t vehId = At<std::uint32_t>(vehicle, off::kFormId), npcId = At<std::uint32_t>(actor, off::kFormId);
		if (Busy(vehId) || Busy(npcId) || vehicleflight::AutopilotDisabled(vehId)) return false;
		for (auto& j : g_jobs) {
			if (j.phase != Phase::Idle) continue;
			j = Job{};
			j.pilot = int(&j - g_jobs) + 1;
			j.npc = npcId; j.vehicle = vehId;
			j.profile = Profile::Escort;
			j.cmd = true;  // no instant-seat fallback: it is already seated
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			j.homeX = vp.x; j.homeY = vp.y; j.haveHome = true;
			j.originX = vp.x; j.originY = vp.y; j.haveOrigin = true;
			j.task = 9; j.taskX = vp.x; j.taskY = vp.y; j.taskStep = 0; j.taskAt = GetTickCount64();
			j.trip = claim;
			j.coldStart = true;
			// The pilot quest's get-in package, as a walking driver gets: without a package that keeps it in, the citizen's own AI
			// stood every cold-start driver up 6 s after it sat (2026-10-28 17:34 run, all three at once).
			Set(j,Phase::Boarding);
            const bool pkg=pilotquest::AssignTraffic(j.pilot-1,actor,vehicle);
            j.questPending=!pkg&&lifecycle::Pending(npcId,"pilot-assign");
			if (!pkg&&!j.questPending) {
                logger::Write("NPC vehicles: [%d] cold start: no pilot quest package for NPC %08X (FVO_NPCPilots.esm loaded?)",j.pilot,npcId);
                j=Job{};return false;
            }
			logger::Write("NPC vehicles: [%d] %s: seated NPC %08X drives vehicle %08X (cold start)", j.pilot, claim.name ? claim.name : "scripted trip", npcId, vehId);
			return true;
		}
		return false;
	}

#ifndef FVO_RELEASE
    static std::uint64_t StartFactoryLeaseImpl(std::uintptr_t actor, std::uintptr_t vehicle,
                                               const FvoTripClaim& claim, bool stock)
    {
        if (!actor || !vehicle || !claim.step || (!stock && !claim.user)) return 0;
        const auto vehicleId = At<std::uint32_t>(vehicle, off::kFormId);
        const auto driverId = At<std::uint32_t>(actor, off::kFormId);
        for (const auto& job : g_jobs) {
            if (job.factoryLease && (!stock || !job.factoryStock ||
                !vehicleflight::FactoryMotionProved(job.pilot, job.vehicle, job.factoryLease))) return 0;
        }
        if (!StartScriptedSeated(actor, vehicle, claim)) return 0;
        static std::uint64_t nextLease = 0;
        for (auto& job : g_jobs) {
            if (job.phase != Phase::Boarding || job.vehicle != vehicleId || job.npc != driverId) continue;
            job.factoryLease = ++nextLease;
            job.factoryStock = stock;
            logger::Write("OBS system=factory.lease vehicle=%08X driver=%08X event=granted token=%llu pilot=%d mode=%s reason=seated_job_admitted",
                vehicleId, driverId, job.factoryLease, job.pilot, stock ? "stationary_goal" : "held_lane");
            return job.factoryLease;
        }
        return 0;
    }

    std::uint64_t StartFactoryLease(std::uintptr_t actor, std::uintptr_t vehicle, const FvoTripClaim& claim)
    {
        return StartFactoryLeaseImpl(actor, vehicle, claim, false);
    }

    std::uint64_t StartFactoryStockLease(std::uintptr_t actor, std::uintptr_t vehicle)
    {
        const FvoTripClaim hold{
            [](void*, double x, double y, double z, FvoTripGoal* out) {
                *out = {x, y, z, 1.5, 0, 0, 0};
                return 0;
            },
            nullptr, "factory stock qualification"
        };
        return StartFactoryLeaseImpl(actor, vehicle, hold, true);
    }

    static Job* FactoryJob(unsigned vehicle, unsigned driver, std::uint64_t token)
    {
        if (!token) return nullptr;
        for (auto& job : g_jobs) {
            if (job.factoryLease == token && job.vehicle == vehicle && job.npc == driver &&
                job.phase != Phase::Idle) return &job;
        }
        return nullptr;
    }

    bool CheckFactoryLease(unsigned vehicle, unsigned driver, std::uint64_t token)
    {
        const auto* job = FactoryJob(vehicle, driver, token);
        return job && job->task == 9 && (job->phase == Phase::Boarding || job->phase == Phase::Following);
    }

    static Job* SeatedFactoryStock(std::uintptr_t actor,std::uintptr_t vehicle,std::uint64_t lease)
    {
        if(!actor||!vehicle||IsDead(actor)||Disabled(vehicle))return nullptr;
        const auto vehicleId=At<unsigned>(vehicle,off::kFormId),driverId=At<unsigned>(actor,off::kFormId);
        auto* job=FactoryJob(vehicleId,driverId,lease);
        if(!job||!job->factoryStock||job->task!=9||job->phase!=Phase::Following||job->questPending||
           !job->factoryReleased||trafficpool::PlayerOwnsHandoff(vehicleId)||VehicleInCombat(vehicleId)||
           !SeatedNow(actor,vehicle,InVehicle(actor)==vehicle))return nullptr;
        const auto physics=vehicleflight::HandoffPhysics(vehicleId);
        return physics.bound&&physics.live?job:nullptr;
    }

    bool FactoryStockTransferReady(std::uintptr_t actor,std::uintptr_t vehicle,std::uint64_t lease)
    {
        const auto* job=SeatedFactoryStock(actor,vehicle,lease);
        return job&&vehicleflight::FactoryMotionProved(job->pilot,job->vehicle,lease);
    }

    bool FactoryStockTransferBusy(){return g_relocSerial!=0;}

    std::uint64_t StartFactoryStockTransfer(std::uintptr_t actor,std::uintptr_t vehicle,std::uint64_t lease,
        double x,double y,double z,double heading)
    {
        if(!FactoryStockTransferReady(actor,vehicle,lease))return 0;
        const auto vehicleId=At<unsigned>(vehicle,off::kFormId),driverId=At<unsigned>(actor,off::kFormId);
        if(auto found=g_stockTransfers.find(vehicleId);found!=g_stockTransfers.end()){
            relocation::Receipt last{};
            if(found->second.lease==lease&&(!g_reloc.LastFor(vehicleId,last)||last.serial!=found->second.serial||
               last.result!=relocation::Result::Refused))return 0;
            g_stockTransfers.erase(found);
        }
        const auto from=At<game::Vec3>(vehicle,off::kRefPosition);
        const auto rotation=At<game::Vec3>(vehicle,off::kRefRotation);
        const relocation::Pose target{x,y,z,heading};
        const auto authority=trafficpool::CaptureWheelHandoff(vehicleId);
        if(!authority)return 0;
        const auto serial=ArmRelocation({vehicleId,driverId,{from.x,from.y,from.z,rotation.z},target},
            "factory_stock_delivery",authority);
        if(serial)g_stockTransfers[vehicleId]={driverId,lease,serial,target};
        return serial;
    }

    bool PromoteFactoryStockTransfer(std::uintptr_t actor,std::uintptr_t vehicle,std::uint64_t lease,
        std::uint64_t serial,double x,double y,double z,double heading,const FvoTripClaim& claim)
    {
        if(!claim.step||!claim.user||!serial)return false;
        auto* job=SeatedFactoryStock(actor,vehicle,lease);
        if(!job)return false;
        const auto vehicleId=job->vehicle;
        const auto grant=g_stockTransfers.find(vehicleId);
        if(grant==g_stockTransfers.end()||grant->second.driver!=job->npc||grant->second.lease!=lease||
           grant->second.serial!=serial||grant->second.target.x!=x||grant->second.target.y!=y||grant->second.target.z!=z||
           grant->second.target.yaw!=heading)return false;
        relocation::Receipt receipt{};
        if(!g_reloc.LastFor(vehicleId,receipt)||receipt.epoch!=g_relocEpoch||receipt.serial!=serial||
           receipt.driver!=job->npc||receipt.result!=relocation::Result::Applied||!receipt.seatedAfter||
           !receipt.physicsLive||receipt.stepAfter<=receipt.stepBefore)return false;
        const auto at=At<game::Vec3>(vehicle,off::kRefPosition);
        if(!std::isfinite(at.x)||!std::isfinite(at.y)||!std::isfinite(at.z)||
           relocation::Distance({at.x,at.y,at.z},grant->second.target)>2.0)return false;

        vehicleflight::RebaseFactoryMotionAfterTransfer(job->pilot,vehicleId,lease);
        job->trip=claim;
        job->factoryLease=0;
        job->factoryReleased=job->factoryStock=false;
        job->homeX=at.x;job->homeY=at.y;job->haveHome=true;
        job->originX=at.x;job->originY=at.y;job->haveOrigin=true;
        job->since=job->taskAt=job->watchAt=job->watchTickAt=GetTickCount64();
        routeguidance::Clear(job->pilot);
        g_stockTransfers.erase(grant);
        logger::Write("OBS system=factory.lease vehicle=%08X driver=%08X event=promoted token=%llu pilot=%d relocation_serial=%llu",
            vehicleId,job->npc,static_cast<unsigned long long>(lease),job->pilot,static_cast<unsigned long long>(serial));
        return true;
    }

    bool DepartFactoryLease(std::uintptr_t actor, std::uintptr_t vehicle, std::uint64_t token)
    {
        if (!actor || !vehicle) return false;
        const auto vehicleId = At<std::uint32_t>(vehicle, off::kFormId);
        const auto driverId = At<std::uint32_t>(actor, off::kFormId);
        auto* job = FactoryJob(vehicleId, driverId, token);
        if (!job || job->factoryStock || job->task != 9 || job->phase != Phase::Following || job->questPending || !job->factoryReleased ||
            IsDead(actor) || !SeatedNow(actor, vehicle, InVehicle(actor) == vehicle) ||
            !vehicleflight::HandoffPhysics(vehicleId).bound ||
            !vehicleflight::FactoryMotionProved(job->pilot, vehicleId, token))
            return false;
        logger::Write("OBS system=factory.lease vehicle=%08X driver=%08X event=departed token=%llu pilot=%d reason=proof_current",
            vehicleId, driverId, token, job->pilot);
        job->factoryLease = 0;
        job->factoryReleased = false;
        job->since = job->taskAt = job->watchAt = job->watchTickAt = GetTickCount64();
        return true;
    }

    bool CancelFactoryLease(unsigned vehicle, unsigned driver, std::uint64_t token)
    {
        if (!FactoryJob(vehicle, driver, token)) return false;
        g_stockTransfers.erase(vehicle);
        logger::Write("OBS system=factory.lease vehicle=%08X driver=%08X event=cancelled token=%llu reason=request",
            vehicle, driver, token);
        return EndJobOf(vehicle, "factory private lease cancelled");
    }

    void FactoryLeaseReleased(unsigned vehicle)
    {
        for (auto& job : g_jobs) {
            if (job.vehicle != vehicle || !job.factoryLease || job.factoryReleased || job.phase != Phase::Following) continue;
            job.factoryReleased = true;
            logger::Write("OBS system=factory.lease vehicle=%08X driver=%08X event=released token=%llu pilot=%d reason=handoff",
                vehicle, job.npc, job.factoryLease, job.pilot);
            return;
        }
    }

    std::uint64_t FactoryLeaseOf(int pilot, unsigned vehicle, bool releasedOnly)
    {
        if (pilot < 1 || pilot > kJobs || !vehicle) return 0;
        const auto& job = g_jobs[pilot - 1];
        return job.vehicle == vehicle && job.phase == Phase::Following &&
            (!releasedOnly || job.factoryReleased) ? job.factoryLease : 0;
    }

    std::uint64_t FactoryStockLeaseOf(int pilot, unsigned vehicle, bool releasedOnly)
    {
        if (pilot < 1 || pilot > kJobs || !g_jobs[pilot - 1].factoryStock) return 0;
        return FactoryLeaseOf(pilot, vehicle, releasedOnly);
    }
#endif

	// Passenger control (2026-11-18 00:31 run: back in the passenger seat after the driver's trip had ended, G found "nobody drives it"):
	// the seated driver gets an autopilot job again, as a cold-start driver does (no walk, no trip), so G has a pilot to hand you.
	bool StartSeatedForPassenger(std::uintptr_t vehicle)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!vehicle || !player) return false;
		const std::uint32_t vehId = At<std::uint32_t>(vehicle, off::kFormId);
		if (PilotFor(vehId)) return true;
		const uptr actor = DriverOf(vehicle, player);
		const char* refusal = actor ? ScriptedRefusal(actor, vehicle) : "no seated driver";
		if (*refusal || IsFollower(player, actor)) {
			logger::Write("NPC vehicles: passenger control: %08X gets no autopilot job: %s", vehId, *refusal ? refusal : "the driver is your follower");
			return false;
		}
		const std::uint32_t npcId = At<std::uint32_t>(actor, off::kFormId);
		for (auto& j : g_jobs) {
			if (j.phase != Phase::Idle) continue;
			j = Job{};
			j.pilot = int(&j - g_jobs) + 1;
			j.npc = npcId; j.vehicle = vehId;
			j.profile = Profile::Escort;
			j.cmd = true;  // already seated: no instant-seat fallback
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			j.homeX = vp.x; j.homeY = vp.y; j.haveHome = true;
			j.originX = vp.x; j.originY = vp.y; j.haveOrigin = true;
			j.coldStart = true;
			Set(j, Phase::Boarding);
			const bool pkg = pilotquest::AssignTraffic(j.pilot - 1, actor, vehicle);
			j.questPending = !pkg && lifecycle::Pending(npcId, "pilot-assign");
			logger::Write("NPC vehicles: [%d] passenger control: seated NPC %08X takes vehicle %08X on its autopilot again (no trip)", j.pilot, npcId, vehId);
			return true;
		}
		logger::Write("NPC vehicles: passenger control: %08X gets no autopilot job: every NPC job slot is in use", vehId);
		return false;
	}

	// Pre-armed traffic pilot (2026-10-29, the user: autopilot "set with a virtual vehicle to control before summon at the exact point
	// of summon so when it appears it appears with it already on"). Called in the same call that created the vehicle and its seated
	// driver, with the trip already claimed: no readiness waits, no trip request, no boarding walk. The job hands the vehicle to the
	// pilot the first frame the driver is aboard and the vehicle has its model; until then the cold-start rule holds it (no exit).
	bool BaseHasDriverSeat(std::uintptr_t base)
	{
		std::uint32_t mask = 0, count = 0;
		uptr kw = 0;
		if (!base || !ReadSeat(base, 0, mask, count, kw) || !count || count > 16 || !(mask & 1) || !kw) return false;
		__try { return At<std::uint8_t>(kw, 0x2E) == 4; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	bool TrafficPilotFree()
	{
		for (const auto& j : g_jobs) if (j.phase == Phase::Idle) return true;
		return false;
	}
    handoff::WheelAdmission AdmitWheelOutput(int pilot, unsigned vehicle)
    {
        if (pilot < 1 || pilot > kJobs) return {};
        const auto& job = g_jobs[pilot - 1];
        if (job.phase == Phase::Idle || job.task != 9 || !job.coldStart || job.vehicle != vehicle) return {};
        const auto generation = trafficpool::CaptureWheelHandoff(vehicle);
        return {generation != 0, generation};
    }
    bool TrafficPackageReady(unsigned vehicle,unsigned driver)
    {
        for(const auto& j:g_jobs)
            if(j.phase!=Phase::Idle&&j.task==9&&j.coldStart&&j.vehicle==vehicle&&j.npc==driver)
                return !j.questPending;
        return false;
    }
    bool StartTrafficArmed(std::uintptr_t actor, std::uintptr_t vehicle, const FvoTripClaim& claim)
	{
		if (!actor || !vehicle || !claim.step || !IsActorRef(actor) || IsActorRef(vehicle) || IsDead(actor)) return false;  // 2026-10-30 18:43 crash: a reused id
		const std::uint32_t vehId = At<std::uint32_t>(vehicle, off::kFormId), npcId = At<std::uint32_t>(actor, off::kFormId);
		if (Busy(vehId) || Busy(npcId) || vehicleflight::AutopilotDisabled(vehId)) return false;
		for (auto& j : g_jobs) {
			if (j.phase != Phase::Idle) continue;
			j = Job{};
			j.pilot = int(&j - g_jobs) + 1;
			j.npc = npcId; j.vehicle = vehId;
			j.profile = Profile::Escort;
			j.cmd = true;
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			j.homeX = vp.x; j.homeY = vp.y; j.haveHome = true;
			j.originX = vp.x; j.originY = vp.y; j.haveOrigin = true;
			j.task = 9; j.taskX = vp.x; j.taskY = vp.y; j.taskStep = 0; j.taskAt = GetTickCount64();
			j.trip = claim;
			j.coldStart = true;
			j.armed = true; j.armedAt = GetTickCount64();
			Set(j,Phase::Boarding);
            const bool pkg=pilotquest::AssignTraffic(j.pilot-1,actor,vehicle);
            j.questPending=!pkg&&lifecycle::Pending(npcId,"pilot-assign");
			if (!pkg&&!j.questPending) {
                logger::Write("NPC vehicles: [%d] pre-armed pilot: no pilot quest package for NPC %08X (FVO_NPCPilots.esm loaded?)",j.pilot,npcId);
                j=Job{};return false;
            }
			logger::Write("NPC vehicles: [%d] %s: pre-armed pilot for vehicle %08X / driver %08X at its birth", j.pilot, claim.name ? claim.name : "traffic trip", vehId, npcId);
			return true;
		}
		return false;
	}

	bool SeatNow(std::uintptr_t actor, std::uintptr_t vehicle)
	{
		if (!actor || !vehicle || IsDead(actor)) return false;
		if (InVehicle(actor) == vehicle) return true;
		EnsureSeatKeyword(actor, vehicle);
		int seated = -1;
		InstantSeat(actor, vehicle, seated);
		return InVehicle(actor) == vehicle;
	}

    // 2026-10-24: a vehicle 3D reload drops its seated actor; the timetable puts its own leased driver back with the
    // native snap seat (no walk, no entry-site check, so it also works for a vehicle that reloaded off the ground).
    bool ReseatScheduled(int pilot,std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle||Dead(actor))return false;
        if(!ScheduledLease(pilot,At<std::uint32_t>(actor,off::kFormId),At<std::uint32_t>(vehicle,off::kFormId)))return false;
        const auto seated=InVehicle(actor);if(seated)return seated==vehicle;
        EnsureSeatKeyword(actor,vehicle);
        if(!CanMount(actor,vehicle))return false;
        int result=-1;SnapSeat(actor,vehicle,result);
        return InVehicle(actor)==vehicle;
    }
    // 2026-10-25 one-step seated creation (notes/road-traffic-20261025/SEATED_SPAWN_RESEARCH.md): called from inside the
    // game's create-reference, on an actor that has its base and place but has not joined a cell or loaded 3D. Builds and
    // starts the Mount interaction and the snap seat, the state a seated character is loaded from a save in.
    // -1 fault, 0 no interaction built, 1 interaction built but the snap refused, 2 seated.
    // The actor's sit state (actor +0xF0, vtable +0x1A0; what the get-in log prints): 0 = not sitting. -1 unreadable.
    int SitState(std::uintptr_t actor) {
        if(!actor)return -1;
        __try{const uptr state=actor+0xF0;return reinterpret_cast<int(*)(uptr)>((*reinterpret_cast<void***>(state))[0x1A0/8])(state);}
        __except(EXCEPTION_EXECUTE_HANDLER){return -1;}
    }
    // The instant seat on a loaded actor, even when the game already links it to the vehicle (a link made inside creation
    // does not sit the driver down: 2026-10-25, drivers stood beside their vehicles). Same results as SeatNewborn.
    // Only a driver at its vehicle (2026-10-30): a snap from far away left drivers frozen mid-air in the getting-in state.
    // The vehicle went away from a driver not yet in it (2026-10-30: it goes to the ground around its build, 16-47 m). Written directly: disabling and
    // enabling the driver instead left them far below (10:48 run, 7 of 9), and the game's setter takes an actor out of the furniture.
    void HoldAtVehicle(std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle||IsDead(actor)||InVehicle(actor)!=vehicle)return;
        if(!trafficpair::NeedsBringing(SitState(actor),Dist3(actor,vehicle)))return;
        if(!Has3D(actor)){PinToVehicle(actor,vehicle);return;}
        PlaceSeated(actor,vehicle,{});
    }
    double SeatDistance(std::uintptr_t actor,std::uintptr_t vehicle) { return SeatDist(actor,vehicle); }
    bool DriverLeaving(std::uintptr_t actor) {
        if(!actor)return false;
        std::uint32_t id=0;
        __try{id=At<std::uint32_t>(actor,off::kFormId);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
        if(g_standNpc==id||ExitTaskFor(id))return true;
        for(const auto& j:g_jobs)if(j.npc==id&&j.phase!=Phase::Idle&&j.phase!=Phase::Following&&j.phase!=Phase::Boarding)return true;
        return false;
    }
    bool BringToVehicle(std::uintptr_t actor,std::uintptr_t vehicle) {
        // Only while still listed in that vehicle. One who got out (pulled out by the player, a fight, a wreck) has no link and is a person in the world, never pulled back.
        if(!actor||!vehicle||!Has3D(actor)||!Has3D(vehicle)||IsDead(actor)||InVehicle(actor)!=vehicle)return false;
        if(!trafficpair::NeedsBringing(SitState(actor),Dist3(actor,vehicle)))return false;
        return PlaceSeated(actor,vehicle,{});
    }
    int SnapNow(std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle)return 0;
        const double d=SeatDist(actor,vehicle);
        const int sit=SitState(actor);
        if(InVehicle(actor)==vehicle&&trafficpair::Sitting(sit)&&std::isfinite(d)&&d<=8.0)return 2;  // sitting in it: never snapped again
        // Sitting with a pilot flying it: the reference lags behind in flight (2026-11-01: "30 m away" once a second for 165 s, with the model 1 m from the seat).
        if(InVehicle(actor)==vehicle&&trafficpair::Sitting(sit)&&vehicleflight::PilotHolds(At<std::uint32_t>(vehicle,off::kFormId)))return 2;
        if(!Has3D(actor)||!Has3D(vehicle))return 0;  // the snap needs both models (the driver's animation graph)
        if(InVehicle(actor)==vehicle&&ProcessLevel(actor)>=1)return 1;  // reduced simulation: held in its seat, snapped once fully simulated
        if(!trafficpair::NeedsSnap(true,true,sit,d)){
            struct Told {ULONGLONG at=0;unsigned quiet=0;};static std::map<std::uint32_t,Told> told;
            auto& t=told[At<std::uint32_t>(actor,off::kFormId)];const auto now=GetTickCount64();
            if(!t.at||now-t.at>=10000){
                logger::Write("NPC vehicles: driver %08X is %.1f m from vehicle %08X: not snapped into the seat from there (%u more in the last 10 s)",At<std::uint32_t>(actor,off::kFormId),d,At<std::uint32_t>(vehicle,off::kFormId),t.quiet);
                t.at=now;t.quiet=0;
            }else ++t.quiet;
            return -3;
        }
        return SeatNewborn(actor,vehicle);
    }
    bool ResumeSavedSeat(std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle||IsDead(actor)||InVehicle(actor)!=vehicle)return false;
        // snap=1 fetches existing ExtraInteraction via 140531E20. This avoids
        // 141995460, which would construct another interaction during loading.
        __try { EngineFn<char(*)(uptr,uptr,char)>(kBoardDirect)(actor,vehicle,1); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        return InVehicle(actor)==vehicle&&SitState(actor)>0;
    }
    // SL-0: a keyword's form id and form type, guarded (kept apart: __try cannot share a function with objects that unwind).
    void KeywordInfo(uptr kw,std::uint32_t& id,unsigned& type){id=0;type=0;if(!kw)return;
        __try{id=At<std::uint32_t>(kw,off::kFormId);type=At<std::uint8_t>(kw,0x2E);}__except(EXCEPTION_EXECUTE_HANDLER){id=0;type=0;}}
    // SL-0: a model's seat markers, once (read only).
    void LogSeatTable(std::uintptr_t vehicle,std::uintptr_t base,std::uint32_t vid,std::uint32_t bid){
        std::uint32_t mask=0,count=0;uptr k0=0;if(!ReadSeat(base,0,mask,count,k0))return;
        std::string list;
        for(std::uint32_t i=0;i<count&&i<16;++i){std::uint32_t m=0,c=0;uptr kw=0;ReadSeat(base,i,m,c,kw);std::uint32_t kwid=0;unsigned type=0;
            KeywordInfo(kw,kwid,type);
            char b[48];std::snprintf(b,sizeof b,"%s%u:%08X/%u/%u",list.empty()?"":" ",i,kwid,type,unsigned((mask>>i)&1u));list+=b;}
        (void)vehicle;
        logger::Write("OBS system=seat.table base=%08X vehicle=%08X count=%u mask=%X markers=[%s] reason=seat_markers_of_this_model_once",bid,vid,count,mask,list.c_str());
    }
    int SeatNewborn(std::uintptr_t actor,std::uintptr_t vehicle) {
        if(!actor||!vehicle)return 0;
        EnsureSeatKeyword(actor,vehicle);
        // SL-0..SL-2 (seat ladder): the model's seat table once; a driver stuck getting on (sit 1) moves to the next marker; a marker that seats is learned per model.
        const uptr base=At<uptr>(vehicle,0x98);const std::uint32_t vid=At<std::uint32_t>(vehicle,off::kFormId),bid=base?At<std::uint32_t>(base,off::kFormId):0;
        if(base){
            const int sitNow=SitState(actor);
            bool logTable=false;{std::lock_guard lk(g_ladderLock);logTable=!g_seatTableLogged[bid];g_seatTableLogged[bid]=true;}
            if(logTable)LogSeatTable(vehicle,base,vid,bid);
            std::uint32_t useMarker=0,from=0;bool give=false;const char* ev="";
            {   std::lock_guard lk(g_ladderLock);
                const auto it=g_ladderMarker.find(vid);
                if(sitNow==3){if(it!=g_ladderMarker.end()&&g_learnedMarker[bid]!=it->second+1){g_learnedMarker[bid]=it->second+1;useMarker=it->second;from=it->second;ev="learned";}}
                else if(it==g_ladderMarker.end()){
                    const auto l=g_learnedMarker.find(bid);
                    if(l!=g_learnedMarker.end()&&l->second>0){useMarker=l->second-1;g_ladderMarker[vid]=useMarker;give=true;ev="use_learned";}
                    else if(sitNow==1){std::uint32_t count=0;MarkerKeyword(base,0,&count);
                        for(std::uint32_t k=1;k<count&&k<32;++k)if(MarkerKeyword(base,k)){useMarker=k;g_ladderMarker[vid]=k;give=true;ev="next_marker";break;}}
                }
                else if(sitNow==1){from=it->second;std::uint32_t count=0;MarkerKeyword(base,0,&count);
                    for(std::uint32_t step=1;step<=count&&count;++step){const std::uint32_t k=(from+step)%count;if(k!=from&&MarkerKeyword(base,k)){useMarker=k;it->second=k;give=true;ev="next_marker";break;}}}
            }
            if(give){if(const uptr kw=MarkerKeyword(base,useMarker))GiveKeyword(actor,kw);}
            if(*ev)logger::Write("OBS system=seat.ladder vehicle=%08X driver=%08X base=%08X event=%s from=%u to=%u sit=%d reason=%s",vid,At<std::uint32_t>(actor,off::kFormId),bid,ev,from,useMarker,sitNow,
                !std::strcmp(ev,"learned")?"this_marker_seated_remembered_for_the_model":!std::strcmp(ev,"use_learned")?"a_marker_that_seated_this_model_before":"stuck_getting_on_try_the_next_seat_marker");
        }
        int seated=-1;const int link=SnapSeat(actor,vehicle,seated);
        if(link<0)return -1;
        if(seated>0&&base){std::lock_guard lk(g_ladderLock);const auto it=g_ladderMarker.find(vid);
            if(it!=g_ladderMarker.end()&&g_learnedMarker[bid]!=it->second+1){g_learnedMarker[bid]=it->second+1;
                logger::Write("OBS system=seat.ladder vehicle=%08X base=%08X event=learned to=%u reason=snap_seated_at_this_marker",vid,bid,it->second);}}
        return link==0?0:(seated>0?2:1);
    }
	std::uintptr_t VehicleOf(std::uintptr_t actor) { return actor ? InVehicle(actor) : 0; }

	bool StartConvoy(std::uintptr_t actor, std::uintptr_t vehicle, std::uint32_t leadVehicleId)
	{
		if (!StartTask(actor, vehicle, 8, 0, 0)) return false;
		for (auto& j : g_jobs) {
			if (j.phase != Phase::Idle && j.npc == At<std::uint32_t>(actor, off::kFormId)) j.lead = leadVehicleId;
		}
		return true;
	}

	bool HostileToPlayer(std::uintptr_t actor)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		return player && actor && HostileTo(actor, player);
	}

	namespace
	{
		uptr CurrentPackageRaw(uptr actor)
		{
			__try {
				return EngineFn<uptr (*)(uptr)>(0x018DDB70)(actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// PerformAction as the console does it (FUN_140c74670): a BGSActionData on the stack, then FUN_141afdb10(actor, &data, &emptyString, 0, 0, emptyStringHolder).
		// Layout from that handler: +0x00 vtable (BGSActionData, rva 4AFAE18), +0x08 source ref (counted), +0x10 target, +0x18 action form, +0x20 int,
		// +0x28 second vtable (rva 4AFAE08), +0x30 / +0x38 strings (counted), +0x60 flags, +0x78 -> +0x28.
		constexpr std::uint32_t kActionDataVtbl = 0x04AFAE18, kActionDataVtbl2 = 0x04AFAE08, kPerformAction = 0x01AFDB10, kEmptyString = 0x002B7DD0,
			kStringRelease = 0x028CAEA0;
		bool PerformActionRaw(uptr actor, uptr action)
		{
			alignas(16) std::uint8_t d[0x90] = {};
			uptr strTemp = 0;
			bool ok = false;
			__try {
				const uptr holder = EngineFn<uptr (*)()>(kEmptyString)();
				auto add = [&](std::uint32_t off) {
					const uptr e = holder ? *reinterpret_cast<uptr*>(holder) : 0;
					*reinterpret_cast<uptr*>(d + off) = e;
					if (e) _InterlockedIncrement(reinterpret_cast<volatile long*>(e + 0x10));
				};
				*reinterpret_cast<uptr*>(d + 0x00) = game::Base() + kActionDataVtbl;
				_InterlockedExchangeAdd64(reinterpret_cast<volatile long long*>(actor + 8), 0x80000200001LL);
				*reinterpret_cast<uptr*>(d + 0x08) = actor;
				*reinterpret_cast<uptr*>(d + 0x18) = action;
				*reinterpret_cast<uptr*>(d + 0x28) = game::Base() + kActionDataVtbl2;
				add(0x30);
				add(0x38);
				*reinterpret_cast<uptr*>(d + 0x78) = reinterpret_cast<uptr>(d + 0x28);
				strTemp = holder ? *reinterpret_cast<uptr*>(holder) : 0;
				if (strTemp) _InterlockedIncrement(reinterpret_cast<volatile long*>(strTemp + 0x10));
				ok = EngineFn<char (*)(uptr, uptr, uptr*, char, char, uptr)>(kPerformAction)(actor, reinterpret_cast<uptr>(d), &strTemp, 0, 0, holder) != 0;
				EngineFn<void (*)(uptr*)>(kStringRelease)(&strTemp);
				EngineFn<void (*)(uptr*)>(kStringRelease)(reinterpret_cast<uptr*>(d + 0x38));
				EngineFn<void (*)(uptr*)>(kStringRelease)(reinterpret_cast<uptr*>(d + 0x30));
				EngineFn<void (*)(uptr)>(addr::kRefRelease)(actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
			return ok;
		}

		bool g_witnessRead = false, g_witnessOn = true;
		ULONGLONG g_witnessAt = 0;
		std::unordered_map<std::uint32_t, ULONGLONG> g_witnessed;
	}

	std::uintptr_t CurrentPackage(std::uintptr_t actor) { return actor ? CurrentPackageRaw(actor) : 0; }

	void WitnessCrash(double x, double y, double z, const char* why)
	{
		g_witnessOn = features::On(features::Id::Witnesses);  // [witness] enabled, also the settings menu
		const ULONGLONG now = GetTickCount64();
		if (!g_witnessOn || now - g_witnessAt < 3000) return;
		g_witnessAt = now;
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		const uptr flee = RefById(0x00047188), cower = RefById(0x00047166);  // AACT ActionFleeStart / ActionCower (Starfield.esm)
		if (!player || (!flee && !cower)) return;
		int n = 0, fled = 0, cowered = 0;
		for (const uptr r : modscene::LoadedRefs(8000)) {
			if (n >= 6) break;
			if (!r || r == player || !IsActorRef(r) || Dead(r) || InVehicle(r) || InCombat(r) || IsFollower(player, r)) continue;
			if (At<std::uint32_t>(r, 0x378) & ((1u << 18) | (1u << 19))) continue;  // essential / protected
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			const double d = std::sqrt(std::pow(p.x - x, 2) + std::pow(p.y - y, 2) + std::pow(p.z - z, 2));
			if (d > 25.0) continue;
			const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
			const auto t = g_witnessed.find(id);
			if (t != g_witnessed.end() && now - t->second < 20000) continue;
			g_witnessed[id] = now;
			++n;
			const bool f = d < 10.0 && flee;
			if (PerformActionRaw(r, f ? flee : cower)) ++(f ? fled : cowered);
		}
		if (n) logger::Write("NPC vehicles: witnesses of %s: %d near, %d flee, %d cower", why ? why : "a crash", n, fled, cowered);
		if (g_witnessed.size() > 512) g_witnessed.clear();
	}

	Stance CycleStance()
	{
		g_stance = g_stance == Stance::Defend ? Stance::Attack : g_stance == Stance::Attack ? Stance::HoldFire : Stance::Defend;
		logger::Write("NPC vehicles: stance -> %s", g_stance == Stance::Attack ? "attack" : g_stance == Stance::HoldFire ? "hold fire" : "defend");
		return g_stance;
	}

	Stance CurrentStance() { return g_stance; }

	void Recall(unsigned ms)
	{
		g_recallUntil = GetTickCount64() + ms;
		for (auto& j : g_jobs) {
			if (j.profile == Profile::Escort) j.foeAt = 0;
		}
		logger::Write("NPC vehicles: escorts recalled for %.0f s", ms / 1000.0);
		logger::Write("Followers: back to formation");
	}
	bool StartPatrol(std::uintptr_t actor, std::uintptr_t vehicle) { return StartTask(actor, vehicle, 2, 0, 0); }
	bool AdoptDriver(std::uintptr_t vehicle)
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!vehicle || !player) return false;
		const std::uint32_t id = At<std::uint32_t>(vehicle, off::kFormId);
		if (PilotFor(id)) return true;
		uptr driver = DriverOf(vehicle, player);
		if (!driver) driver = FurnitureDriver(vehicle, player);
		const bool ok = driver && !IsFollower(player, driver) && StartPatrol(driver, vehicle);
		logger::Write("NPC vehicles: you ride %08X as a passenger: driver %08X %s", id, driver ? At<std::uint32_t>(driver, off::kFormId) : 0,
			ok ? "takes it on its autopilot (patrol)" : "has no autopilot job (the vehicle stays put)");
		return ok;
	}

	bool SendHome(std::uint32_t vehicleId)
	{
		for (auto& j : g_jobs) {
			if (!vehicleId || j.phase != Phase::Following || j.vehicle != vehicleId || !j.haveHome || j.task || j.order) continue;
			const uptr player = Ptr(addr::kPlayerCharacterPtr), npc = RefById(j.npc);
			if (player && npc && IsFollower(player, npc)) return false;  // followers get out by the player, as before
			const uptr v = RefById(j.vehicle);
			if (!v) return false;
			const auto vp = At<game::Vec3>(v, off::kRefPosition);
			if (std::hypot(vp.x - j.homeX, vp.y - j.homeY) < kHomeArrive) return false;
			j.task = 3;
			j.taskAt = GetTickCount64();
			j.since = GetTickCount64();
			logger::Write("NPC vehicles: [%d] fight over; vehicle %08X goes home (%.0f m)", j.pilot, j.vehicle, std::hypot(vp.x - j.homeX, vp.y - j.homeY));
			return true;
		}
		return false;
	}

	int CivilJobs()
	{
		int n = 0;
		for (const auto& j : g_jobs) {
			if (j.phase != Phase::Idle && j.task) ++n;
		}
		return n;
	}

	int EnemyJobs()
	{
		int n = 0;
		for (const auto& j : g_jobs) {
			if (j.phase == Phase::Idle || j.task) continue;
			if (const uptr npc = RefById(j.npc); npc && !PlayerTeam(npc)) ++n;
		}
		return n;
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
			return ApplyProfile(j, pursuit ? Profile::Pursuit : Profile::Escort, player, false);
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
#ifndef FVO_RELEASE
        g_fastSeat={};g_fastSeatDrivers.clear();g_fastSeatEvidence.clear();g_fastSeatDown=false;
        g_fastSeatMoveVehicle=g_fastSeatMoveDriver=0;g_fastSeatMoveLease=0;
        g_testPairVehicle=0;g_testPairDriver=0;g_pairMoveDown=false;
        g_pairMoveReceipt=0;g_pairMoveObservation={};
        ++g_relocEpoch;FinishRelocation();g_stockTransfers.clear();FarReset();
#endif
		g_passengerRequest = 0; g_passengerAttempt = {};
		g_boardVehicle = 0; g_boardKeyword = 0; g_boardUntil = 0;
		g_swapVehicle = 0; g_swapKeyword = 0; g_swapped = 0;
		g_driverFor = 0; g_driverUntil = 0;
		g_intent = Intent{};
		g_activateDown = false;
		g_combatFocus=0;g_focusVehicle=0;
		for (auto& member : g_damageCrew) member.store(0, std::memory_order_relaxed);
		for (auto& j : g_jobs) j = Job{};
		FullSimClear();  // the game reuses FF ids across saves
		g_seatTraceBudget.store(4000, std::memory_order_relaxed);
		g_dropTraces = 0;
		g_riders.clear();
		g_seatLogged.clear();
		g_aggression.clear();
		g_relations.clear();g_temperCache.clear();g_factions={};
        g_driverIndex.clear();
		g_driverDiscovery.Reset();
		for (auto& side : g_sides) side = {};
		g_ramPairs = {}; g_ramPrevious={};g_ramPreviousAt=0;for(auto& impact:g_pendingImpacts)impact={};
		g_squadRefreshAt = 0;
		g_combatVehicleCount = 0;
		for(auto&t:g_exitTasks)t={};
		for(auto&watch:g_fallWatch)watch={};
		for(auto&r:g_exitRefused)r=0;
		for(auto&pending:g_nativeExits)pending.store(0);
		g_told = false;
	}

	std::uintptr_t EnemyNear(std::uintptr_t player, double x, double y, double z, double range, bool& inVehicle, int& count)
	{
		inVehicle = false;
		count = 0;
		if (!player) return 0;
		uptr best = 0;
		int bestTier = 9;
		double bestD = 1e9;
		for (const uptr r : modscene::LoadedRefs(8000)) {
			if (!r || r == player || !IsActorRef(r) || Dead(r)) continue;
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			const double d = std::sqrt(std::pow(p.x - x, 2) + std::pow(p.y - y, 2) + std::pow(p.z - z, 2));
			if (d > range) continue;
			if (IsFollower(player, r) || !InCombat(r)) continue;
			if (!HostileTo(r, player) && !HostileTo(player, r)) continue;
			++count;
			const int tier = InVehicle(r) ? 0 : 1;
			if (tier < bestTier || (tier == bestTier && d < bestD)) { best = r; bestTier = tier; bestD = d; }
		}
		inVehicle = best && bestTier == 0;
		return best;
	}

	std::uintptr_t FreeFollower(std::uintptr_t player, double x, double y, double range)
	{
		if (!player) return 0;
		uptr gang[32];
		const int n = Followers(player, gang, 32);
		uptr best = 0;
		double bestD = range;
		for (int i = 0; i < n; ++i) {
			const uptr a = gang[i];
			if (!a || Dead(a) || InVehicle(a)) continue;
			const std::uint32_t id = At<std::uint32_t>(a, off::kFormId);
			if (Locked(id) || Busy(id)) continue;
			const auto p = At<game::Vec3>(a, off::kRefPosition);
			const double d = std::hypot(p.x - x, p.y - y);
			if (d < bestD) { bestD = d; best = a; }
		}
		return best;
	}

	bool CrewBoard(std::uintptr_t player, std::uintptr_t vehicle, CrewOrder order)
	{
		if (!player || !vehicle) return false;
		const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
		const uptr actor = FreeFollower(player, vp.x, vp.y, 80.0);
		const std::uint32_t vehId = At<std::uint32_t>(vehicle, off::kFormId);
		if(!targetsession::CanFollow(vehId,vehicleflight::OwnRoverId()))return false;
		if (!actor || Busy(vehId)) return false;
		for (auto& j : g_jobs) {
			if (j.phase != Phase::Idle) continue;
			j.pilot = int(&j - g_jobs) + 1;
			if (!StartWalkBoard(&j, actor, vehicle)) return false;
			j.order = int(order);
			logger::Write("NPC vehicles: [%d] crew: follower %08X drives your vehicle %08X (%s)", j.pilot, At<std::uint32_t>(actor, off::kFormId), vehId, OrderText(order));
			FollowerNotice(At<std::uint32_t>(actor,off::kFormId),order == CrewOrder::Marker ? "A follower takes your vehicle to the marker" :
				order == CrewOrder::Cover ? "A follower drives your vehicle to cover you" : "A follower drives your vehicle");
			return true;
		}
		return false;
	}

	CrewOrder CrewOrderOf(std::uint32_t vehicleId)
	{
		for (const auto& j : g_jobs) {
			if (vehicleId && j.phase != Phase::Idle && j.vehicle == vehicleId && j.order) return CrewOrder(j.order);
		}
		return CrewOrder::None;
	}

	void SetCrewOrder(std::uint32_t vehicleId, CrewOrder order)
	{
		if((order==CrewOrder::Follow||order==CrewOrder::Cover||order==CrewOrder::Recall) && !targetsession::CanFollow(vehicleId,vehicleflight::OwnRoverId()))return;
		for (auto& j : g_jobs) {
			if (!vehicleId || j.phase == Phase::Idle || j.vehicle != vehicleId || !j.order || (j.order == int(order) && order!=CrewOrder::Hold)) continue;
			j.order = int(order);
				if (order==CrewOrder::Hold) {
					if (const auto vehicle=RefById(j.vehicle)) {
						const auto pos=At<game::Vec3>(vehicle,off::kRefPosition);
						j.orderX=pos.x; j.orderY=pos.y; j.orderZ=pos.z;
					}
				}
			logger::Write("NPC vehicles: [%d] crew order: %s", j.pilot, OrderText(order));
			FollowerNotice(j.npc,order == CrewOrder::Marker ? "Your follower drives to the marker" : order == CrewOrder::Hold ? "Your follower holds position" :
				order == CrewOrder::Recall ? "Your follower brings your vehicle" : order == CrewOrder::Cover ? "Your follower covers you" : "Your follower follows you");
		}
	}

	bool SetCrewGuard(std::uint32_t vehicleId, double x, double y, double z)
	{
		bool set = false;
		for (auto& j : g_jobs) {
			if (!vehicleId || j.phase != Phase::Following || j.vehicle != vehicleId || !j.order) continue;
			j.order = int(CrewOrder::Guard);
			j.orderX = x; j.orderY = y; j.orderZ = z;
			j.parked = false; j.relaunchAt = 0;
			logger::Write("NPC vehicles: [%d] crew order: guard the point (%.0f, %.0f, %.0f)", j.pilot, x, y, z);
			FollowerNotice(j.npc, "Your follower guards the point");
			set = true;
		}
		return set;
	}

	void ReleaseCrew(std::uint32_t vehicleId)
	{
		for (auto& j : g_jobs) {
			if (!vehicleId || j.phase != Phase::Following || j.vehicle != vehicleId || !j.order) continue;
			vehicleflight::PilotStop(j.pilot, "crew released");
			logger::Write("NPC vehicles: [%d] crew released; NPC %08X gets out", j.pilot, j.npc);
			Set(j, Phase::Settling);
		}
	}

	bool FireVehicleWeapon(std::uint32_t vehicleId, bool fire, double x, double y, double z, std::uint32_t target, std::uint32_t targetVehicle)
	{
		if (!vehicleId) return false;
        const auto vehicle=RefById(vehicleId);
        if(fire&&vehicle){const auto p=At<game::Vec3>(vehicle,off::kRefPosition);const double h=At<game::Vec3>(vehicle,off::kRefRotation).z;
            const float from[]={p.x+float(std::sin(h)*4),p.y+float(std::cos(h)*4),p.z+1.5f},to[]={float(x),float(y),float(z)};
            fire=environment::ClearShot(vehicleId,target,targetVehicle,from,to);
        }
		StoreAim(7, AimSnap{ vehicleId, 0, x, y, z, fire, GetTickCount64() });
		return SubmitFire(kJobs, vehicleId, fire);
	}

	std::uint32_t CombatFocus()
	{
		if(!g_combatFocus&&!g_focusVehicle)return 0;
		const auto player=Ptr(addr::kPlayerCharacterPtr);
		// A focused vehicle: its current driver is the target. A new driver takes the old one's place; an empty vehicle ends it.
		if(g_focusVehicle){
			const auto vehicle=RefById(g_focusVehicle);
			const auto driver=vehicle&&!vehiclehits::Wrecked(g_focusVehicle)?CombatDriver(vehicle):0;
			const auto driverId=driver?At<std::uint32_t>(driver,off::kFormId):0;
			const char* why=!player?nullptr:!vehicle?"gone":vehiclehits::Wrecked(g_focusVehicle)?"wrecked":!driver?"empty (nobody drives it)":nullptr;
			if(why||!player){
				logger::Write("Combat focus: vehicle %08X %s; normal target selection resumes",g_focusVehicle,why?why:"(no player)");
				if(why)notify::Show("Focus vehicle %s | automatic targeting",why);
				g_focusVehicle=0;g_combatFocus=0;return 0;
			}
			if(driverId!=g_combatFocus){
				logger::Write("Combat focus: vehicle %08X now driven by %08X; allied vehicles follow the vehicle",g_focusVehicle,driverId);
				g_combatFocus=driverId;
				for(auto& job:g_jobs)if(job.phase!=Phase::Idle)if(const auto npc=RefById(job.npc);npc&&PlayerTeam(npc)){job.foe=g_combatFocus;job.foeAt=0;job.aimAt=0;job.squadFocus=0;Aggro(npc,driver);}
			}
		}
		const auto target=RefById(g_combatFocus);
		// Dropped by itself (polled every frame by the scanner key): said once, so the player knows targeting is automatic again.
		const char* why=!player?nullptr:!target?"gone":Dead(target)?"down":PlayerTeam(target)||!AreHostile(player,target)?"no longer hostile":
			Dist3(player,target)>coverfight::kChaseKeep?"out of range":nullptr;
		if(why||!player) {
			logger::Write("Combat focus: %08X %s; normal target selection resumes",g_combatFocus,why?why:"(no player)");
			if(why)notify::Show("Focus target %s | automatic targeting",why);
			g_combatFocus=0;g_focusVehicle=0;
		}
		return g_combatFocus;
	}
	bool SetCombatFocus(std::uint32_t id)
	{
		if(!id){g_combatFocus=0;g_focusVehicle=0;logger::Write("Combat focus cleared; normal target selection resumes");return true;}
		const auto player=Ptr(addr::kPlayerCharacterPtr);auto target=RefById(id);
		// A vehicle, or a person who sits in one, focuses the vehicle (vehicles first): its driver now and whoever drives it next.
		std::uint32_t vehicleId=0;
		if(target&&IsVehicle(target)){vehicleId=id;target=CombatDriver(target);}
		else if(target&&IsActorRef(target))if(const auto in=InVehicle(target);in&&CombatDriver(in)==target)vehicleId=At<std::uint32_t>(in,off::kFormId);
		if(vehicleId&&vehiclehits::Wrecked(vehicleId)){logger::Write("Combat focus: vehicle %08X refused (wrecked)",vehicleId);return false;}
		if(!player||!target||!IsActorRef(target)||Dead(target)||PlayerTeam(target)||!AreHostile(player,target)||Dist3(player,target)>coverfight::kChaseKeep){
			logger::Write("Combat focus: %08X refused (%s)",id,!target?(vehicleId?"nobody drives that vehicle":"not a person or vehicle"):Dead(target)?"down":
				PlayerTeam(target)||!AreHostile(player,target)?"not hostile":"out of range");
			return false;
		}
		g_combatFocus=At<std::uint32_t>(target,off::kFormId);g_focusVehicle=vehicleId;
		for(auto& job:g_jobs)if(job.phase!=Phase::Idle)if(const auto npc=RefById(job.npc);npc&&PlayerTeam(npc)) {
			job.foe=g_combatFocus;job.foeAt=0;job.aimAt=0;job.squadFocus=0;
			Aggro(npc,target);
		}
		if(vehicleId)logger::Write("Combat focus: player ordered allied vehicles onto vehicle %08X (driver %08X)",vehicleId,g_combatFocus);
		else logger::Write("Combat focus: player ordered allied vehicles onto %08X",g_combatFocus);
		return true;
	}
	std::uint32_t CombatFocusVehicle(){CombatFocus();return g_focusVehicle;}
	bool IsVehicleRef(std::uintptr_t ref){return ref&&IsVehicle(ref);}

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
void BeginFrame()
    {
		for (int i = 0; i < kJobs; ++i) {
			const auto& job = g_jobs[i];
			g_damageCrew[i].store(job.phase != Phase::Idle && (job.gang || job.order) ? job.npc : 0, std::memory_order_relaxed);
		}
        const bool discover = g_driverDiscovery.Due(GetTickCount64());
        if (discover) g_driverIndex.clear();
        auto note=[](uptr actor) {
            if(!actor||!IsActorRef(actor))return;
            const auto vehicle=InVehicle(actor);if(!vehicle||Dead(actor)||!IsVehicle(vehicle)||!Drives(actor,vehicle))return;
            g_driverIndex[At<std::uint32_t>(vehicle,off::kFormId)]=At<std::uint32_t>(actor,off::kFormId);
        };
        const auto player=Ptr(addr::kPlayerCharacterPtr);note(player);
        {   // Evidence for the seat rule: once per boarding, the player's seat keyword against the vehicle's driver seat.
            static std::uint32_t told=0;
            const auto pv=player?InVehicle(player):0;const auto pid=pv?At<std::uint32_t>(pv,off::kFormId):0;
            if(pid!=told){told=pid;if(pv)logger::Write("NPC vehicles: seat proof: player in %08X, seat keyword %p, driver seat keyword %p -> %s",pid,
                reinterpret_cast<void*>(SeatKeyword(player)),reinterpret_cast<void*>(DriverSeatKeyword(pv)),InDriverSeat(player)?"driver seat":"NOT the driver seat");}
        }
        // CombatDriver revalidates cached occupants before use. Background
        // actors may be discovered up to 250 ms later; player and managed jobs
        // retain immediate boarding/exit handling without scanning the city.
        for (const auto& job : g_jobs)
            if (job.phase != Phase::Idle && !job.passenger) note(RefById(job.npc));
        if (discover) {
            for(const auto actor:modscene::LoadedRefs(8000))if(actor!=player)note(actor);
        }
        static bool reported = false;
        if (!reported) {
            reported = true;
            logger::Write("Driver discovery: background scan at most 4 Hz; player and managed drivers refreshed each update");
        }
    }
    bool AreHostile(uptr a,uptr b){return CombatRelationOf(a,b)==CombatRelation::Enemy;}
    bool AreAllies(uptr a,uptr b){return CombatRelationOf(a,b)==CombatRelation::Ally;}
	bool FriendlyActorDamage(uptr victim, uptr attacker)
	{
		if (!victim || !attacker || victim == attacker || !IsActorRef(victim) || !IsActorRef(attacker)) return false;
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		bool victimTeam = victim == player || (player && IsFollower(player, victim));
		bool attackerTeam = attacker == player || (player && IsFollower(player, attacker));
		const auto victimId = At<std::uint32_t>(victim, off::kFormId), attackerId = At<std::uint32_t>(attacker, off::kFormId);
		for (const auto& member : g_damageCrew) {
			const auto id = member.load(std::memory_order_relaxed);
			victimTeam = victimTeam || (id && id == victimId);
			attackerTeam = attackerTeam || (id && id == attackerId);
		}
		// No main-thread relation map or mutable job storage is touched from the native callback.
		if (victimTeam && attackerTeam) return true;
		if (HostileTo(victim, attacker) || HostileTo(attacker, victim)) return false;
		return combatrelations::Classify(false, false, FactionReaction(victim, attacker), FactionReaction(attacker, victim)) == CombatRelation::Ally;
	}
    std::uint32_t NativeEnemyId(uptr actor)
    {
        __try {
            if(!actor||!IsActorRef(actor))return 0;
            const auto enemy=HandleRef(At<std::uint32_t>(actor,0x298));
            return enemy&&IsActorRef(enemy)&&!Dead(enemy)&&AreHostile(actor,enemy)?At<std::uint32_t>(enemy,off::kFormId):0;
        } __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    }
    std::uint32_t VehicleDriverId(std::uint32_t vehicleId)
	{
		const auto driver = CombatDriver(RefById(vehicleId));
		return driver ? At<std::uint32_t>(driver, off::kFormId) : 0;
	}

	bool VehicleInCombat(std::uint32_t vehicleId, std::uint32_t departedDriver)
	{
		const auto now = GetTickCount64();
		const auto attack = g_aggression.find(vehicleId);
		if (attack != g_aggression.end() && now < attack->second.until) return true;
		const auto vehicle = RefById(vehicleId), driver = CombatDriver(vehicle);
		for (const auto actor : {driver, RefById(departedDriver)}) {
			if (!actor || Dead(actor)) continue;
			if (InCombat(actor) || NativeEnemyId(actor)) return true;
			const auto shot = vehiclehits::LastShotBy(At<std::uint32_t>(actor, off::kFormId));
			if (shot && now >= shot && now - shot < 15000) return true;
		}
		for (const auto& j : g_jobs) if (j.vehicle == vehicleId && j.phase != Phase::Idle) {
			if (j.firing) return true;
			if (j.boardingFight.target && now < j.boardingFight.until) {
				const auto enemy = RefById(j.boardingFight.target);
				if (enemy && !Dead(enemy)) return true;
			}
		}
		const auto shot = vehiclehits::LastShotBy(vehicleId);
		return shot && now >= shot && now - shot < 15000;
	}

	bool VehicleFriendlyToShooter(std::uintptr_t vehicle, std::uint32_t shooterFormId)
	{
		if (!vehicle || !shooterFormId) return false;
		uptr shooter = RefById(shooterFormId);
		if (!shooter) return false;
		if (!IsActorRef(shooter)) {
			if (!IsVehicle(shooter)) return false;
			shooter = CombatDriver(shooter);
			if (!shooter && shooterFormId == vehicleflight::OwnRoverId()) shooter = Ptr(addr::kPlayerCharacterPtr);
		}
		if (!shooter) return false;
		if (InVehicle(shooter) == vehicle) return true;
		const uptr driver = CombatDriver(vehicle);
		if (driver) {
            if(PlayerTeam(shooter)&&PlayerTeam(driver))return true;
            return CombatRelationOf(shooter,driver)==CombatRelation::Ally;
        }
		const auto directOwner = OwnerForm(vehicle);
		const auto owner = directOwner ? directOwner : realvehicles::PlaceOwner(vehicle);
		if (owner && (owner == At<uptr>(shooter, 0x98) || (At<std::uint8_t>(owner, 0x2E) == 0x0D && InFaction(shooter, owner)))) return true;
		return At<std::uint32_t>(vehicle, off::kFormId) == vehicleflight::OwnRoverId() && PlayerTeam(shooter);
	}

	void VehicleAggression(std::uintptr_t vehicle,std::uint32_t shooterId,bool reportCrime)
	{
		const auto shooter=RefById(shooterId);
		if(!vehicle||!shooter||!IsActorRef(shooter)||Dead(shooter)||InVehicle(shooter)==vehicle)return;
		if (VehicleFriendlyToShooter(vehicle, shooterId)) return;
		const auto id=At<std::uint32_t>(vehicle,off::kFormId);const auto now=GetTickCount64();
		if(g_aggression.size()>256)std::erase_if(g_aggression,[&](const auto&p){return now>=p.second.until;});
		g_relations.clear();
		auto&event=g_aggression[id];
        if(event.attacker!=shooterId||now>=event.until)event={};
        const bool refresh=event.attacker!=shooterId || now-event.nativeAt>=2000;
		event.attacker=shooterId;event.until=now+60000;if(!refresh)return;event.nativeAt=now;
		const auto player=Ptr(addr::kPlayerCharacterPtr);const auto directOwner=OwnerForm(vehicle);
		const auto owner=directOwner?directOwner:realvehicles::PlaceOwner(vehicle);
		const auto actors=modscene::LoadedRefs(8000);uptr principal=CombatDriver(vehicle);int reacted=0,assisted=0;
		if(!principal&&id==vehicleflight::OwnRoverId())principal=player;
		// The job's driver even when the seat check fails (2026-10-30 log 08:18: a traffic vehicle hit while its driver was off the seat: "0 reacted", driver 00000000).
		if(!principal)for(const auto& job:g_jobs)if(job.phase!=Phase::Idle&&!job.passenger&&job.vehicle==id) {
			const auto occupant=RefById(job.npc);
			if(occupant&&!Dead(occupant)&&!(PlayerTeam(occupant)&&PlayerTeam(shooter))){principal=occupant;break;}
		}
		if(!principal&&owner)for(const auto actor:actors) {
			if(actor&&IsActorRef(actor)&&!Dead(actor)&&Dist(actor,vehicle)<=80.0&&At<uptr>(actor,0x98)==owner){principal=actor;break;}
		}
		for(auto& job:g_jobs)if(job.phase!=Phase::Idle&&job.vehicle==id) {
			const auto occupant=RefById(job.npc);if(!occupant||(PlayerTeam(occupant)&&PlayerTeam(shooter)))continue;
			if(job.phase==Phase::Boarding)job.resumeFoe=shooterId;
			else job.boardingFight.Remember(shooterId,now);
		}
		bool principalSeen=false;
		for(const auto actor:actors) {
			if(!actor||actor==player||actor==shooter||!IsActorRef(actor)||Dead(actor))continue;
			const bool aboard=InVehicle(actor)==vehicle;
			const bool owns=owner && (At<uptr>(actor,0x98)==owner || (At<std::uint8_t>(owner,0x2E)==0x0D && InFaction(actor,owner)));
			const bool ally=principal&&actor!=principal&&CombatRelationOf(actor,principal)==CombatRelation::Ally;
			const auto role=combatrelations::AttackRole(aboard,owns,ally,Dist(actor,vehicle)<=80.0,PlayerTeam(actor)&&PlayerTeam(shooter));
			if(role==combatrelations::VehicleAttackRole::None)continue;
			if(actor==principal)principalSeen=true;
            if(reportCrime&&!event.crimeRaised&&shooter==player && !HostileTo(actor,shooter)&&!HostileTo(shooter,actor)&&FactionReaction(actor,shooter)!=1) {
                CrimeAlarms(actor,vehicle,player,true,false);event.crimeRaised=true;
            }
			if(Aggro(actor,shooter)){++reacted;if(role==combatrelations::VehicleAttackRole::Ally)++assisted;}
		}
		if(principal&&!principalSeen&&principal!=shooter&&principal!=player&&!(PlayerTeam(principal)&&PlayerTeam(shooter)))if(Aggro(principal,shooter))++reacted;
		if(reportCrime) {
			const auto p=At<game::Vec3>(vehicle,off::kRefPosition);WitnessCrash(p.x,p.y,p.z,"an attack on a vehicle");
		}
		logger::Write("Vehicle aggression: %08X attacked vehicle %08X; %d occupants/owners/allies reacted (%d faction allies); assault %s; driver %08X (model %d, in combat %d)",shooterId,id,reacted,assisted,event.crimeRaised?"reported":"not applicable",
			principal?At<std::uint32_t>(principal,off::kFormId):0,principal&&Has3D(principal)?1:0,principal&&InCombat(principal)?1:0);
	}

}

#ifndef FVO_RELEASE
// The relocation receipt status for the Traffic add-on's Health (optional, diagnostic cores only; relocationapi.h).
extern "C" __declspec(dllexport) int FVO_TrafficRelocationV1(unsigned vehicle, FvoRelocationStatusV1* out)
{
    if (!vehicle || !out || out->size != sizeof(*out) || out->version != 1) return 0;
    relocation::Receipt r{};
    const bool has = npcvehicles::g_reloc.LastFor(vehicle, r), relocating = npcvehicles::g_reloc.Relocating(vehicle);
    if (!has && !relocating) return 0;
    out->relocating = relocating ? 1 : 0;
    if (has) {
        out->serial = r.serial; out->epoch = r.epoch; out->stepAfter = r.stepAfter; out->result = int(r.result);
        out->to[0] = r.to.x; out->to[1] = r.to.y; out->to[2] = r.to.z;
    }
    return 1;
}
#endif

extern "C" __declspec(dllexport) int FVO_RegisterTrafficCycle(FvoTrafficCycle cycle)
{
    npcvehicles::g_trafficCycle = cycle;
    return cycle ? 1 : 0;
}
