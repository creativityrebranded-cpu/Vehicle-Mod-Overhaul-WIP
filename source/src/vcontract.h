#pragma once

#include <cstdint>
#include <limits>

// The vehicle contract (2026-10-18, Profile Fork; agreed with the flight / seat owner on the board). Profiles plug in
// here and never touch the flight, job or seat code; owners call the hooks below and never look inside a profile. As
// long as both sides keep to these declarations, either side can change its own implementation freely.
//
// Owners' hooks (two call sites, owned by the flight / NPC session):
//   1. npcvehicles, top of the Following case:   if (vcontract::GoalFor(User::Npc, pilot, vehicle, player, g)) -> PilotFly(g)
//   2. vehicleflight, the player's goal block:    User::Player (driving, Auto on) and User::PlayerRemote (own vehicle,
//      nobody aboard, a profile assigned: vcontract::AssignedTo(OwnRoverId()) != nullptr keeps its autopilot on).
// Seat owner: implements SeatProvider and calls SetSeatProvider() once; profiles that need seats check Seats().
namespace vcontract
{
	// Who decides where a vehicle goes.
	enum class User : std::uint8_t { None = 0, Player = 1, PlayerRemote = 2, Npc = 4 };

	// How a profile wants to be driven (the shared navigation service reads these; 0 / defaults = the service's own).
	// Agreed with the Driving Behavior session 2026-10-18.
	struct NavParams
	{
		enum class Height : std::uint8_t { Auto, Hug, High };
		float speedLimit = 0;        // m/s cap; 0 = the controller's own (gears)
		float minClearance = 0;      // m kept over terrain / obstacles; 0 = default
		Height height = Height::Auto;
		float margin = 0;            // m kept from other vehicles and walls; 0 = default
		bool squeeze = false;        // may take a gap narrower than margin (slowly)
		int priority = 0;            // right of way, higher wins: player 100, wingman 60, courier 40, commuter 30, patrol 20, hauler 10
		bool passThrough = false;    // (x, y) is a waypoint: keep speed through it, do not brake to stop
		float arriveYaw = std::numeric_limits<float>::quiet_NaN();  // park facing this heading (atan2(x, y)); NaN: any
		bool landOnArrival = false;  // at a held goal: hover off and settle (parked) instead of hovering
	};

	// What a profile wants this frame.
	struct Goal
	{
		double x = 0, y = 0;
		double z = std::numeric_limits<double>::quiet_NaN();  // NaN: skim the terrain
		double stop = 3.0;          // metres short of (x, y)
		bool hold = false;          // a fixed spot: arrive and stay
		NavParams nav;
		// Set by the drive layer (vehicleflight), not by profiles: how the flight treats this goal.
		bool keepGoing = false;                 // arriving does not end the flight (follow / profiles keep going)
		unsigned long long timeoutMs = 90000;   // a flight that does not keep going gives up after this
		bool cruise = false;                    // Auto with the player aboard: a moving point ahead, flown at cruiseSpeed
		double cruiseSpeed = 0;
	};

	// What a profile is told each frame.
	struct Context
	{
		User user = User::None;
		int pilot = -1;                  // 0 = the player's vehicle, 1.. = NPC pilots
		std::uintptr_t vehicle = 0;      // valid this frame only
		std::uint32_t vehicleId = 0;
		std::uintptr_t player = 0;
		double homeX = 0, homeY = 0;     // where the vehicle was when the profile was assigned
		unsigned long long since = 0;    // GetTickCount64 at assignment
		int step = 0;                    // the profile's own progress (waypoint, phase)
		unsigned long long stepAt = 0;   // when `step` last changed
		bool done = false;               // set by the profile: finished; the owner's default takes over again
	};

	// The class of a profile (2026-10-20, the user): a vehicle holds one profile of each class at once, so choosing where it goes never replaces
	// how it fights or when it shoots. Navigation: escort, hold, return home, patrol, hauler, courier, scout, cruise. Combat: attack, defend (the
	// ram is a one-shot order, vehicleflight::RequestRam). Weapons: free fire (default), opportunistic, hold fire, never (the player's seat).
	enum class Class : std::uint8_t { Navigation = 0, Combat = 1, Weapons = 2 };
	inline constexpr int kClasses = 3;

	// A profile: a goal source. goal() returns false for "no goal this frame" (the owner keeps its own behaviour). Combat and Weapons
	// profiles are selectors the owner reads with AssignedTo(id, cls); their goal may be null.
	struct Profile
	{
		const char* name;
		std::uint8_t users;              // bit mask of User
		bool (*goal)(Context& ctx, Goal& out);
		bool needsSeats;                 // boards / unloads someone (waits for SetSeatProvider)
		Class cls = Class::Navigation;
	};

	// ---- registry ----
	void Register(const Profile* p);
	int Count();
	const Profile* ProfileAt(int i);
	const Profile* Find(const char* name);

	// ---- assignment (by vehicle form id), one per class ----
	// Assign fills p's own class and leaves the vehicle's other classes alone; a null p clears the Navigation class only.
	void Assign(std::uint32_t vehicleId, const Profile* p, double homeX, double homeY);
	void Unassign(std::uint32_t vehicleId);              // every class (the job is over, the driver left)
	void Unassign(std::uint32_t vehicleId, Class cls);
	const Profile* AssignedTo(std::uint32_t vehicleId);  // the Navigation profile; nullptr: the owner's default behaviour
	const Profile* AssignedTo(std::uint32_t vehicleId, Class cls);

	// ---- the owners' hook: the assigned profile's goal for this frame (false: none applies, keep your own) ----
	bool GoalFor(User user, int pilot, std::uintptr_t vehicle, std::uintptr_t player, Goal& out, Class cls = Class::Navigation);

	// ---- seats (implemented by the seat owner) ----
	// WaitingToLand: an exit is asked but the vehicle is still flying (NORMAL waits for the flight code's landing).
	enum class SeatState : std::uint8_t { None, Walking, Seating, Driver, Passenger, Refused, WaitingToLand, Exiting };

	// How to get out (agreed with the seat session 2026-10-18). Auto = Normal, falling back to Quick then Instant.
	enum class ExitMode : std::uint8_t { Auto, Normal, Quick, Instant };
	struct ExitRequest
	{
		ExitMode mode = ExitMode::Auto;
		// Where to stand afterwards: NaN = the seat owner's own rule (a free spot beside the vehicle, away from the
		// player's path). A point = as close to it as the seat owner can place them (e.g. near the player).
		float x = std::numeric_limits<float>::quiet_NaN(), y = std::numeric_limits<float>::quiet_NaN(), z = std::numeric_limits<float>::quiet_NaN();
		bool waitLanded = true;      // Normal / Auto: wait for the vehicle to be landed (the flight code lands it)
		unsigned timeoutMs = 8000;   // then escalate (Auto) or report Refused (a fixed mode)
	};

	struct SeatProvider
	{
		bool (*board)(std::uintptr_t actor, std::uintptr_t vehicle, bool asDriver);  // starts boarding; false: refused
		bool (*exit)(std::uintptr_t actor, const ExitRequest& req);                   // starts getting out; false: not aboard
		// Everyone aboard but (optionally) the driver gets out with `req` (squad unload); returns how many were asked.
		int (*unload)(std::uintptr_t vehicle, const ExitRequest& req, bool includeDriver);
		SeatState (*state)(std::uintptr_t actor);                                    // where the actor is right now (polled)
	};
	void SetSeatProvider(const SeatProvider* p);
	const SeatProvider* Seats();  // nullptr until the seat owner registers

	void Reset();  // a save load: assignments belong to one save
}
