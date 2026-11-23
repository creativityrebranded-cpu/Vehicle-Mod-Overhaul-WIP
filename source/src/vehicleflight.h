#pragma once

#include "fvo_addon_api.h"
#include <limits>

#include <cstdint>

#include "vcontract.h"
#include "triplogic.h"
#include "fleetownershipstore.h"

// Planetary flight on the REV-8 rover (user, 2026-10-14; design: notes/plan-vehicle-flight.md). While driving, H
// toggles hover: the rover holds a height above the ground with an infinite boost and flies with the flight model's
// controls (W/S speed lever, mouse yaw with hysteresis, A/D slide, Space/Ctrl move the hold height), with the
// predictive terrain assist. Applied as velocity in the rover's own physics step (FUN_1423cb260): Havok still resolves
// every collision. The rover is kept upright (yaw only).
namespace vehicleflight
{
	bool Install();  // hooks the vehicle physics step
	void BeginFrame(); // revoke previous-frame command ownership before decision systems run
	void Tick();     // per frame, main thread (frame task)
	void LoadReset(); // discard hover/body state before a different save replaces the rover
	bool Active();
    bool RouteTripActive(std::uint32_t vehicleId);
    bool BirthControlReady(std::uint32_t vehicleId);
    bool PilotHolds(std::uint32_t vehicleId);
    int PilotLive(std::uint32_t vehicleId);  // -1 no pilot, 0 a pilot but no physics steps, 1 stepping (or a scripted or physical flight, which does not use them)
    bool PlayerDriving(std::uint32_t vehicle);
    fleetownershipstore::PlayerControl PlayerControlForSave();
    void ResumeSavedControl(fleetownershipstore::PlayerControl control);  // RS-1: resume the player's saved Auto/Cruise once on load
	std::uint64_t NativeStepCount(); // load trace: vehicle physics steps seen by the step hook so far
	bool PhysicalCameraActive(std::uintptr_t vehicle); // atomic identity gate for the VTOL camera, instrument and key set: physical flight with you at the wheel, you or the autopilot flying

	// NPC pilots 1..3 (the "pursuit" side; pilot 0 is the player's escort autopilot): the same flight model, terrain
	// assist and avoidance, flying `vehicle` to (x, y) and settling `stop` metres short. Called again while flying it
	// only changes the target. keepGoing: arriving does not end the flight (it holds `stop` metres off; no timeout).
	// z: the world height to fly at (NaN: skim the terrain); never under the terrain clearance.
	// False: not allowed (the player's rover, or another pilot has this vehicle).
	bool PilotFly(int pilot, std::uintptr_t vehicle, double x, double y, double stop, unsigned timeoutMs, bool keepGoing = false,
		double z = std::numeric_limits<double>::quiet_NaN());
	// The same from a profile's goal (vcontract::Goal): the nav settings and the target in one call. The one drive call for goal-driven pilots.
	bool PilotFly(int pilot, std::uintptr_t vehicle, const vcontract::Goal& goal, unsigned timeoutMs = 0, bool keepGoing = true);
	bool PilotActive(int pilot);
	// A traffic pilot held still far from the player (1 km or more; it flies on within 900 m): its job and trip wait, not stuck.
	bool PilotHeldFar(int pilot);
	// Height of `vehicle` over the ground this pilot last read (0 = the player's rover); NaN when not known.
	double PilotAboveGround(int pilot, std::uintptr_t vehicle);
	struct PhysicsStatus { bool bound = false, live = false; std::uint64_t serial = 0; };
	// Fresh successful steps for the current bound instance; never saved.
	PhysicsStatus PilotPhysics(int pilot);
    PhysicsStatus HandoffPhysics(std::uint32_t vehicle);
#ifndef FVO_RELEASE
    bool FactoryMotionProved(int pilot, unsigned vehicle, std::uint64_t lease);
    void RebaseFactoryMotionAfterTransfer(int pilot,unsigned vehicle,std::uint64_t lease);
#endif
	const char* PilotEndReason(int pilot);  // why the pilot last stopped ("arrived", "stuck", ...)
	void PilotStop(int pilot, const char* why);
	std::uint32_t OwnRoverId();  // the player's own vehicle (form id), never flown by an NPC
	// The player's view heading (world yaw, atan2(x, y)); false when unreadable. For a passenger steering an NPC pilot.
	bool ViewHeading(double& yaw);
	// Drive mode (hover off, the game's own controls): the controls an autopilot wants for this vehicle right now. For the
	// input block of an NPC-driven vehicle (npcvehicles) and for the player's vehicle while Auto drives it.
	struct DriveInput { float steer = 0, accel = 0, brake = 0; bool boost = false, vert = false; };  // steer + is right
	bool DriveFor(std::uint32_t vehicleFormId, DriveInput& out);
	// The player's custom map marker as a point in `vehicle`'s world (false: none, not loaded, another worldspace, too far).
	// Hold T on a place (on foot): the player's own vehicle (empty, or its crew driver) holds that point and defends from it; false when there is
	// no vehicle or it cannot fly (logged). U afterwards makes it cover the player again.
	bool SetGuardPoint(double x, double y);
	void FreshScannerTarget(); // explicit selection can reuse the same marker coordinates
	void EndTargetSession(std::uintptr_t vehicle);
	bool MarkerPoint(std::uintptr_t vehicle, double& x, double& y, double* z = nullptr);
	// The scanner marker button (scantarget): the player's own vehicle is summoned (as the call key does), and when the player gets in, Auto flies it to the
	// marker. Driving already: Auto starts at once. Main thread.
	void RequestScannerTrip();
	// T tapped outside the scanner while driving (2026-10-21, the user): Auto on (to the marker / route) or off (you drive).
	void ToggleAuto();
	void ToggleCruise();  // Auto with no destination: joins traffic at random
	bool Cruising();
	// The traffic addon's route planner for Auto (fvo_addon_api.h FVO_RegisterRoutePlanner).
	void SetRoutePlanner(FvoRoutePlanner planner);
    void RouteNav(int pilot, void* trip, const FvoTripGoal& goal, vcontract::NavParams& nav);
	void SetCruisePlanner(FvoCruisePlanner planner);
	void RetryScannerTrip();
    void CancelScannerTrip(); // hold current position, preserving hover and the crew driver
	struct TripProgress {
		bool active = false;
		triplogic::Phase phase = triplogic::Phase::Idle;
		double x = 0, y = 0, distance = 0, speed = 0, heightError = 0;
		unsigned long long at = 0;
	};
	TripProgress ProgressFor(std::uint32_t vehicleId);
	bool AtDestination(std::uintptr_t vehicle, double x, double y, double z, double stop, bool requireStopped=true);
	// The vehicle hit counter (vehiclehits): this vehicle's autopilot is switched off now and refused for `ms` (its hover ends; the player can still
	// drive it by hand). Main thread.
	void DisableAutopilot(std::uint32_t vehicleFormId, unsigned ms, const char* why);
	bool AutopilotDisabled(std::uint32_t vehicleFormId);
	// What Auto does at the player's marker (the scanner's behaviour key cycles it): Stop there, Orbit it, or Patrol the scanner's marker chain
	// in a loop (each point goes back to the end of the chain when it is passed). Main thread.
	enum class AutoMode : int { Stop = 0, Orbit = 1, Patrol = 2 };
	void SetAutoMode(AutoMode m);
	AutoMode GetAutoMode();

	// Direct control of an autopilot (pilot 1..3, or 0 = the player's rover while it flies itself): every channel in
	// `set` overrides the autopilot's own for holdMs, then hands back. Channels left out stay with the autopilot.
	struct Command
	{
		enum : unsigned { kThrottle = 1, kStrafe = 2, kVertical = 4, kYaw = 8, kPitch = 16, kRoll = 32, kBrake = 64, kGear = 128,
			// Scripted flight (flightscript.h): the command is the plan. Skips the planner's rollout check, avoidance speed caps and
			// stuck recovery; stops only for a wall/person ahead (forward thrust) or a vehicle within 5 m in the direction of travel.
			kScripted = 256,
			// With kScripted: the script picks how it travels (travel: 1 float = the hover at the commanded height, 2 drive = the hover
			// held just over the surface under it, following the road; the automatic hover-or-drive choice is skipped).
			kTravel = 512,
			// 2026-11-17 passenger control (the user: "take control directly ... we dont need to swap seats"): the player drives an
			// NPC-driven vehicle from the passenger seat. kManual: the player's own hands, as at their own wheel: no planner veto, no
			// arrival turn or avoidance braking, kTravel picks hover / wheels (wheels = the game's own driving). kSteer: wheel
			// steering -1 left .. 1 right for drive mode (hover keeps kYaw).
			kSteer = 1024, kManual = 2048,
			// With kManual: the game's forward boost on its own wheels (drive mode).
			kBoost = 4096 };
		unsigned set = 0;
		double throttle = 0;   // -1 full reverse .. 1 full forward (the gear's thrust)
		double strafe = 0;     // -1 slide left .. 1 slide right
		double vertical = 0;   // m/s up (+) / down (-); replaces the height hold
		double yawRate = 0;    // rad/s, + turns right
		double pitch = 0;      // rad, nose up + (attitude wanted)
		double roll = 0;       // rad, right side down + (attitude wanted)
		bool brake = false;
		int gear = 0;          // 1-4
		unsigned holdMs = 300;
		int travel = 0;        // with kTravel: 1 float, 2 drive
		double steer = 0;      // with kSteer: -1 full left .. 1 full right (drive mode)
		bool boost = false;    // with kBoost
	};
	void ClearPilotCommand(int pilot);
    bool ImpactSince(std::uint32_t vehicleId, unsigned long long since);
    double CruiseLimit(std::uint32_t vehicleId);
    double TurnLimit(std::uint32_t vehicleId);
	bool PilotCommand(int pilot, const Command& c);  // false: that pilot is not on autopilot
	// Passenger control while hovering (2026-11-17, the user: "just the flying matters", as at your own wheel): this pilot flies through
	// the player's own hover model (your keys, camera turn, roll, lift, gear, boost) every frame, its autopilot off for that pass.
	// Renew every frame (lasts 250 ms); gear 1-4.
	void PlayerHands(int pilot, int gear);
	// CB-2 (combat on the plan): combat's aim point this frame and whether it may go on the plan (not stunned). True: a combat line
	// drives this pilot now (settings.ini [traffic] planCombat), so combat sends only the aim pitch, no movement sticks.
	bool PilotCombatAim(int pilot, double x, double y, double z, bool aim, bool onPlan);
	// H1 (strip build 4): the Census copy says this traffic pair is broken or retiring (Health's verdict, the addon's Census). Readers
	// (Keep in sight) use this, never a health judgement of their own.
	bool CensusBroken(std::uint32_t vehicle);

	// The navigation settings of a profile (vcontract::Goal::nav) travel with the goal: PilotFly(pilot, vehicle, Goal) stores them; nothing else sets them.

	// Flipped-vehicle protocol: call every frame for a vehicle we look after (the player's own rover, NPC job
	// vehicles). Tipped past ~65 degrees and slow for 2 s: its hover (if any) ends and it is set upright 1.5 m higher
	// in place, heading kept.
	void WatchFlip(std::uintptr_t vehicle, const char* who);
	// CM (read-only measurement): physics steps of every piloted vehicle since start (sum of the per-pilot counters), and how many pilots are bound to a body.
	std::uint64_t TotalPhysicsSteps(int* boundPilots = nullptr);
#ifndef FVO_RELEASE
	// Relocation doer (diagnostic builds; notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md): one pose substitution for one chassis in one
	// physics step. The main thread arms and reads; the physics thread issues it inside the vehicle step hook.
	namespace relocate {
		struct Facts {
			bool issued = false, bodyOk = false, ringFirstOk = false, comNearBody = false;
			const char* refusal = "";
			double comBefore[3]{}, comAfter[3]{}, bodyBefore[3]{}, bodyAfter[3]{};
			float speed = 0;
			std::uint64_t actionCalls = 0;
		};
		struct Chassis { bool bodyValid = false, live = false; double pos[3]{}; std::uint64_t completedSteps = 0; };
		const char* Arm(std::uint32_t vehicleId,double dx,double dy,double dz,std::uint64_t serial,
            std::uint64_t authority=0);  // nullptr = armed, else why not
		bool Take(std::uint64_t serial, Facts& out);   // true once the physics thread has issued (or refused)
		bool Read(std::uint64_t serial, Chassis& out); // the chassis where the body record says it is now, and completed steps since issue
		void Disarm(std::uint64_t serial);

		// Far tier test (2026-11-21, notes/FAR_TIER_TEST_20261121.md; diagnostic builds only). Beyond the loaded cells nothing collides, so the test
		// can hold a chassis on a kinematic "rail": every physics step the doer places it along a straight path at a set speed, its height from a
		// terrain profile the main thread fills (farrail.h) plus a ride height. Exclusive with the hop doer above (one writer of the pose).
		struct RailFacts {
			bool active = false, terrainKnown = false;
			std::uint64_t steps = 0, faults = 0, holds = 0, missed = 0;
			double s = 0, x = 0, y = 0, z = 0, target = 0;
			const char* fault = "";
		};
		// nullptr = started. x0,y0 and dirX,dirY: the path (dir is a unit vector); s0: path length where the chassis is now; z0: its height.
		const char* RailStart(std::uint32_t vehicleId, double x0, double y0, double dirX, double dirY, double s0, double z0, double speed, double ride);
		void RailStop();
		bool RailRead(RailFacts& out);
		void RailSetTerrain(int index, double z);   // profile sample at path length index * 8 m (main thread)
		void RailClearTerrain();
		int RailTerrainKnown();
		// The chassis' body now, wherever it is (main thread read of the body record, as the hop read-back does): false when the pilot has no bound physics instance.
		struct ChassisNow { bool bound = false, bodyValid = false; double pos[3]{}, vel[3]{}; };
		bool ReadChassisNow(std::uint32_t vehicleId, ChassisNow& out);
	}
#endif
}
