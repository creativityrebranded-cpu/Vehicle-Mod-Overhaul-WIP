#include "passenger.h"
#include "inputgate.h"
#include "bindings.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "notify.h"
#include "npcvehicles.h"
#include "vehicleflight.h"
#include "environment.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <string>
#include <cstdint>
#include <intrin.h>

namespace passenger
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kSeatLink = 0x01995460, kSeatFinish = 0x01A9C8C0, kEndInteraction = 0x01A9C530;
		constexpr std::uint32_t kGetInteraction = 0x00531E20;  // (extraList, smart ptr* out): the actor's 0xA5 interaction
		constexpr std::uint32_t kHasExtra = 0x002C5C90;        // (extraList, type)
		constexpr std::uint32_t kLookupForm = 0x005DE3B0;
		constexpr double kReach = 6.0;                         // m: a vehicle this close can be boarded
		constexpr double kLeadMin = 0.0, kLeadMax = 120.0;     // m ahead the autopilot aims (sets the cruise speed)
		constexpr double kLeadStep = 25.0;                     // m per second of W / S held

		constexpr double kTaxiStop = 6.0;                      // m: the taxi holds this close over the marker
		constexpr ULONGLONG kArriveMs = 2500;                  // near the marker this long: arrived, the player is let out

		std::atomic<std::uint32_t> g_riding{ 0 };
		bool g_arrived = false;
		ULONGLONG g_nearSince = 0, g_downAt = 0;
		bool g_exitPending = false;  // the player asked to get out while the vehicle moved: retried until it is at rest
		// Passenger control (2026-11-17, the user: "rather than doing any of these we can just take control directly / we dont need to swap
		// seats / at least whilst in vehicle"): hold G in the passenger seat and your driving keys go to the vehicle's autopilot as the
		// player's own command (vehicleflight Command::kManual), the NPC staying at the wheel. Replaces the take-the-wheel seat swap.
		struct Control { bool on = false; int travel = 0, gear = 2; bool refusedSaid = false, holdSaid = false; ULONGLONG adoptAt = 0; } g_ctrl;
		int g_key = 0;
		bool g_keyDown = false;
		double g_lead = 50.0;
		ULONGLONG g_lastTick = 0;

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
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

		uptr VehicleOf(uptr actor)
		{
			__try {
				return EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// The actor's seat keyword (its interaction +0x20; 0 = driver or none), -1 when unreadable.
		std::intptr_t SeatOf(uptr actor)
		{
			__try {
				const uptr extra = At<uptr>(actor, 0xC8);
				if (!extra || !EngineFn<char (*)(uptr, int)>(kHasExtra)(extra, 0xA5)) return 0;
				uptr inter = 0;
				EngineFn<void (*)(uptr, uptr*)>(kGetInteraction)(extra, &inter);
				if (!inter) return 0;
				const uptr seat = At<uptr>(inter, 0x20);
				auto* rc = reinterpret_cast<volatile long*>(inter + 8);
				if (_InterlockedDecrement(rc) == 0) {
					auto** vt = *reinterpret_cast<void***>(inter);
					reinterpret_cast<void (*)(uptr, int)>(vt[0])(inter, 1);
				}
				return static_cast<std::intptr_t>(seat);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		// The vehicle's seats (base +0x270 count, +0x264 enabled mask, +0x278 array of 0x28-byte entries, keyword at
		// +0x10; FUN_1412B2C40): the first enabled seat after the driver's (seat 0) = the companion seat.
		uptr PassengerSeat(uptr vehicle)
		{
			__try {
				const uptr base = At<uptr>(vehicle, 0x98);
				const std::uint32_t n = std::min<std::uint32_t>(At<std::uint32_t>(base, 0x270), 20);
				const std::uint32_t mask = At<std::uint32_t>(base, 0x264);
				const uptr arr = At<uptr>(base, 0x278);
				for (std::uint32_t i = 1; arr && i < n; ++i) {
					if (!((mask >> i) & 1)) continue;
					const uptr kw = At<uptr>(arr + i * 0x28, 0x10);
					if (kw) return kw;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return 0;
		}

		// The driver's seat keyword (seat 0), 0 when unreadable.
		uptr DriverSeat(uptr vehicle)
		{
			__try {
				const uptr base = At<uptr>(vehicle, 0x98);
				const uptr arr = At<uptr>(base, 0x278);
				return arr && At<std::uint32_t>(base, 0x270) ? At<uptr>(arr, 0x10) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}

		// A vehicle within kReach that an NPC job is flying (nearest).
		uptr NpcVehicleNear(uptr player)
		{
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			uptr best = 0;
			double bestD = kReach;
			for (const uptr r : modscene::LoadedRefs(4000)) {
				const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
				if (!npcvehicles::PilotFor(id)) continue;
				const auto q = At<game::Vec3>(r, off::kRefPosition);
				const double d = std::sqrt(double(q.x - pp.x) * (q.x - pp.x) + double(q.y - pp.y) * (q.y - pp.y) + double(q.z - pp.z) * (q.z - pp.z));
				if (d < bestD) { bestD = d; best = r; }
			}
			return best;
		}

		bool EndSeat(uptr actor)
		{
			__try {
				EngineFn<void (*)(uptr)>(kEndInteraction)(actor);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// Sets the player at the vehicle's boarding site, facing its way; `roof`: on top of the vehicle instead (no free side).
		bool PlaceBeside(uptr player, uptr vehicle, bool roof = false)
		{
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double yaw = At<game::Vec3>(vehicle, off::kRefRotation).z;
			const uptr cell = At<uptr>(vehicle, off::kRefParentCell);
			const uptr ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
			if (!cell) return false;
			const auto site=environment::BoardingSite(vehicle);
            if(!roof && site.verdict!=environment::Verdict::Safe)return false;
            float pos[3]={float(site.x),float(site.y),float(site.z)};
            if (roof) { pos[0] = vp.x; pos[1] = vp.y; pos[2] = vp.z + 2.5f; }
            // 2026-10-28 (the user, out of the passenger seat 330 m up: "I ended up immediately on the floor, didn't even
            // fall"): the boarding site is on the ground. Well above it, the player is set 3.5 m to the vehicle's left at
            // its own height and falls from there.
            if (!roof && vp.z - site.z > 6.0) {
                pos[0] = float(vp.x - std::cos(yaw) * 3.5); pos[1] = float(vp.y + std::sin(yaw) * 3.5); pos[2] = vp.z;
            }
			float rot[3] = { 0.0f, 0.0f, float(yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try {
				EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(player, &handle, cell, ws, pos, rot);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		// `asked`: the player asked to get out (G, or the game's exit refused). 2026-10-29 12:30 run (the user: "lots of cant leave
		// vehicle that it's obstructed and G isn't really doing anything"): traffic parks REV-8s in rows, so a full boarding site (a
		// free side, no vehicle beside it) never came and G only said "Waiting". Asked for and at rest, the player is let out
		// anyway: beside the vehicle when that side is free, else on its roof.
		void GetOut(uptr player, const char* why, bool asked = false)
		{
			const std::uint32_t id = g_riding.load();
            const auto checked=RefById(id);
            const bool safe = checked && environment::BoardingSite(checked).verdict==environment::Verdict::Safe;
            const bool atRest = checked && environment::EntrySite(checked).verdict==environment::Verdict::Safe;
            if(!safe && !(asked && atRest)){
                if (asked && !g_exitPending) {  // moving: the driver's trip ends (it sets down), then out once at rest
                    g_exitPending = true;
                    const bool ended = npcvehicles::EndJobOf(id, "passenger getting out");
                    logger::Write("Passenger: you asked to get out of %08X while it moves: %s; out once it is at rest", id, ended ? "its trip ended" : "no trip to end");
                    notify::Show("Stopping to let you out");
                }
                else if (!asked) notify::Show("Waiting for a safe drop-off site");
                return;
            }
            g_exitPending = false;
            vehicleflight::EndTargetSession(checked);
            g_riding.store(0);
			const uptr vehicle = RefById(id);
			const bool out = EndSeat(player);
			const bool placed = vehicle && PlaceBeside(player, vehicle, !safe);
			logger::Write("Passenger: out of %08X (%s) -> %s, %s", id, why, out ? "seat ended" : "seat end FAILED", placed ? (safe ? "set beside it" : "no free side: set on its roof") : "not moved");
			notify::Show("Passenger: out");
		}

		void ControlOff(const char* why)
		{
			if (!g_ctrl.on) return;
			g_ctrl = Control{};
			logger::Write("Passenger: your control of the vehicle ended (%s)", why);
			notify::Show("The driver has control");
		}
		// One command a frame while you drive from the passenger seat (the command lasts 250 ms: if this stops, the autopilot resumes).
		void ControlTick(uptr vehicle, std::uint32_t id)
		{
			const int pilot = npcvehicles::PilotFor(id);
			if (!pilot) {  // the driver has no autopilot job (its trip ended): it gets one again, then your control starts
				const ULONGLONG t0 = GetTickCount64();
				if (!g_ctrl.adoptAt) {
					g_ctrl.adoptAt = t0;
					if (!npcvehicles::StartSeatedForPassenger(vehicle)) ControlOff("nobody drives it");
				}
				else if (t0 - g_ctrl.adoptAt > 5000) ControlOff("the driver did not take its autopilot in 5 s");
				return;
			}
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			if (!vehicleflight::PilotActive(pilot)) {  // a resting job: its autopilot holds here so your command has something to steer
				vehicleflight::PilotFly(pilot, vehicle, vp.x, vp.y, 3.0, 0, true);
				if (!g_ctrl.holdSaid) { g_ctrl.holdSaid = true; logger::Write("Passenger: pilot %d of %08X was not flying; it holds here under your control", pilot, id); }
			}
			const bool f = Focused();
			const int kW = controlmap::KeyFor("Vehicle", "VehicleAccel", 'W'), kS = controlmap::KeyFor("Vehicle", "VehicleBrake", 'S');
			const int kA = controlmap::KeyFor("Vehicle", "VehicleTurnLeft", 'A'), kD = controlmap::KeyFor("Vehicle", "VehicleTurnRight", 'D');
			const bool fwd = f && bindings::Held(bindings::Action::Accelerate, kW), back = f && bindings::Held(bindings::Action::Brake, kS);
			const bool left = f && bindings::Held(bindings::Action::TurnLeft, kA), right = f && bindings::Held(bindings::Action::TurnRight, kD);
			const bool up = f && (bindings::Held(bindings::Action::LiftUp, VK_SPACE) || bindings::Held(bindings::Action::VerticalBoost, VK_SPACE));
			const bool down = f && bindings::Held(bindings::Action::LiftDown, 'C');
			const bool boost = f && bindings::Held(bindings::Action::Boost, VK_LSHIFT);
			if (f && bindings::Triggered(bindings::Action::Gear)) { g_ctrl.gear = g_ctrl.gear % 4 + 1; logger::Write("Passenger: gear %d", g_ctrl.gear); notify::Show(("Gear " + std::to_string(g_ctrl.gear)).c_str()); }
			vehicleflight::DriveInput now{};
			const bool onWheels = vehicleflight::DriveFor(id, now);
			// Hovering: it flies through your own hover model (vehicleflight::PlayerHands), exactly as your own vehicle does; on its
			// wheels (or switching to them) your keys reach the game's own driving through the command below.
			if (!onWheels && g_ctrl.travel != 2) vehicleflight::PlayerHands(pilot, g_ctrl.gear);
			if (f && bindings::Triggered(bindings::Action::HoverToggle)) {
				g_ctrl.travel = onWheels ? 1 : 2;
				logger::Write("Passenger: you switch %08X to %s", id, g_ctrl.travel == 1 ? "hover" : "its wheels");
				notify::Show(g_ctrl.travel == 1 ? "Hover" : "Wheels");
			}
			vehicleflight::Command c;
			c.set = vehicleflight::Command::kManual | vehicleflight::Command::kThrottle | vehicleflight::Command::kSteer | vehicleflight::Command::kYaw |
				vehicleflight::Command::kGear | vehicleflight::Command::kBoost;
			c.gear = g_ctrl.gear;
			c.boost = boost;
			c.throttle = fwd ? 1.0 : back ? -1.0 : 0.0;
			c.steer = (right ? 1.0 : 0.0) - (left ? 1.0 : 0.0);
			c.yawRate = 1.2 * c.steer;
			if (up || down) { c.set |= vehicleflight::Command::kVertical; c.vertical = up ? 5.0 : -5.0; }
			// Switching to its wheels from the air: it comes down to the ground first (00:31 run: it stayed 15 m up "held low").
			else if (g_ctrl.travel == 2 && !onWheels) { c.set |= vehicleflight::Command::kVertical; c.vertical = -4.0; }
			if (g_ctrl.travel) { c.set |= vehicleflight::Command::kTravel; c.travel = g_ctrl.travel; }
			c.holdMs = 250;
			const bool taken = vehicleflight::PilotCommand(pilot, c);
			if (!taken && !g_ctrl.refusedSaid) { g_ctrl.refusedSaid = true; logger::Write("Passenger: pilot %d of %08X refused your command (not on autopilot yet)", pilot, id); }
			if (taken) g_ctrl.refusedSaid = false;
			static ULONGLONG said = 0;
			const ULONGLONG t = GetTickCount64();
			if (t - said >= 2000) {
				said = t;
				logger::Write("Passenger: driving %08X from the passenger seat (pilot %d, %s): throttle %+.0f steer %+.0f vertical %+.0f",
					id, pilot, onWheels ? "wheels" : "hover", c.throttle, c.steer, (c.set & vehicleflight::Command::kVertical) ? c.vertical : 0.0);
			}
		}

		void BoardTo(uptr player, uptr vehicle);

		void Board(uptr player)
		{
			if (VehicleOf(player)) {
				logger::Write("Passenger: you are already in a vehicle");
				return;
			}
			const uptr vehicle = NpcVehicleNear(player);
			if (!vehicle) {
				notify::Show("No NPC vehicle near you");
				logger::Write("Passenger: no vehicle an NPC is flying within %.0f m", kReach);
				return;
			}
			BoardTo(player, vehicle);
		}

		// The companion seat of `vehicle` (any occupied vehicle: a gang driver, a job, a hijacker).
		void BoardTo(uptr player, uptr vehicle)
		{
			if (!npcvehicles::RequestPassengerBoard(player, vehicle)) notify::Show("Passenger: could not start boarding");
		}
	}
	std::uint32_t RidingVehicle() { return g_riding.load(); }
	bool Controlling() { return g_riding.load() && g_ctrl.on; }
	std::uintptr_t PassengerSeatKeyword(std::uintptr_t vehicle) { return vehicle ? PassengerSeat(vehicle) : 0; }
	bool FinishPassengerSeat(std::uintptr_t player, std::uintptr_t vehicle)
	{
		const uptr seat = vehicle ? PassengerSeat(vehicle) : 0;
		if (!player || !seat) return false;
		const std::uint32_t id = At<std::uint32_t>(vehicle, off::kFormId);
		g_riding = id;  // the flight hooks must see a passenger during native seat completion
		uptr link = 0;
		__try {
			EngineFn<void (*)(uptr, uptr, uptr*)>(kSeatLink)(player, vehicle, &link);
			if (link) {
				At<uptr>(link, 0x20) = seat;
				EngineFn<void (*)(uptr)>(kSeatFinish)(player);
				auto* rc = reinterpret_cast<volatile long*>(link + 8);
				if (_InterlockedDecrement(rc) == 0) reinterpret_cast<void (*)(uptr, int)>((*reinterpret_cast<void***>(link))[0])(link, 1);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { link = 0; }
		if (!link || VehicleOf(player) != vehicle || SeatOf(player) != static_cast<std::intptr_t>(seat)) { g_riding = 0; return false; }
		g_arrived = false; g_nearSince = 0;
		g_lead = 50.0;
		notify::Show("Passenger");
		npcvehicles::AdoptDriver(vehicle);
		return true;
	}

	bool BoardVehicle(std::uintptr_t player, std::uintptr_t vehicle)
	{
		return npcvehicles::RequestPassengerBoard(player, vehicle);
	}

	void Steer(int pilot, std::uintptr_t vehicle)
	{
		const ULONGLONG now = GetTickCount64();
		// Taxi: a map marker is the destination; arriving lets the player out.
		double mx = 0, my = 0, mz = std::numeric_limits<double>::quiet_NaN();
		if (vehicleflight::MarkerPoint(vehicle, mx, my, &mz)) {
			vcontract::Goal goal;goal.x=mx;goal.y=my;goal.z=mz;goal.stop=kTaxiStop;goal.hold=true;
            vehicleflight::PilotFly(pilot,vehicle,goal);
			const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
			const double d = std::hypot(mx - vp.x, my - vp.y);
			if (vehicleflight::AtDestination(vehicle,mx,my,mz,kTaxiStop) && environment::BoardingSite(vehicle).verdict==environment::Verdict::Safe) {
				if (!g_nearSince) g_nearSince = now;
				else if (now - g_nearSince >= kArriveMs) g_arrived = true;
			}
			else {
				g_nearSince = 0;
			}
			static ULONGLONG lastTaxi = 0;
			if (now - lastTaxi >= 5000) {
				lastTaxi = now;
				logger::Write("Passenger: taxi to your map marker, %.0f m to go", d);
			}
			return;
		}
		g_nearSince = 0;
		// No marker: W / S: the aim point ahead moves out / in (the gap sets the cruise speed; 0 = hover in place).
		const double dt = g_lastTick && now > g_lastTick ? std::min(0.1, (now - g_lastTick) / 1000.0) : 0.0;
		g_lastTick = now;
		if (Focused()) {
			const int fwd = controlmap::KeyFor("Vehicle", "VehicleAccel", 'W'), back = controlmap::KeyFor("Vehicle", "VehicleBrake", 'S');
			if (bindings::Held(bindings::Action::Accelerate, fwd)) g_lead = std::min(kLeadMax, g_lead + kLeadStep * dt);
			if (bindings::Held(bindings::Action::Brake, back)) g_lead = std::max(kLeadMin, g_lead - kLeadStep * 2.0 * dt);
		}
		double yaw = 0;
		if (!vehicleflight::ViewHeading(yaw)) yaw = At<game::Vec3>(vehicle, off::kRefRotation).z;
		const auto vp = At<game::Vec3>(vehicle, off::kRefPosition);
		const double lead = std::max(g_lead, 3.5);  // at 0 the goal sits inside the stop radius: it holds still
		vehicleflight::PilotFly(pilot, vehicle, vp.x + std::sin(yaw) * lead, vp.y + std::cos(yaw) * lead, 3.0, 0, true);
		static ULONGLONG lastSay = 0;
		if (now - lastSay >= 5000) {
			lastSay = now;
			logger::Write("Passenger: pilot %d flying where you look (heading %.0f deg), aim %.0f m ahead", pilot, yaw * 57.2958, g_lead);
		}
	}

	void Tick()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) return;
        g_key='G';
        const bool press=bindings::Triggered(bindings::Action::RideAlong);
        const bool toggle=bindings::Triggered(bindings::Action::PassengerExit);  // G in the passenger seat: take / give back control
		const ULONGLONG now = GetTickCount64();
		const std::uint32_t riding = g_riding.load();
		if (riding) {
			const uptr vehicle = RefById(riding);
			if (!vehicle || VehicleOf(player) != vehicle) {
				if(vehicle)vehicleflight::EndTargetSession(vehicle);
				g_riding = 0;
				g_ctrl = Control{};
				logger::Write("Passenger: no longer in %08X (the game ended the seat)", riding);
				return;
			}
			if (g_arrived) {
				g_arrived = false;
				logger::Write("Passenger: arrived at your map marker");
				GetOut(player, "arrived");
				return;
			}
            // 2026-10-28 (the user: "g didn't work" in the passenger seat; no exit or take-the-wheel line in the log): when the
            // bindings do not take input there, G is read directly (a tap, on release).
            static bool gDown = false;
            bool rawToggle = false;
            // While you have control your keys are a driver's (bindings.cpp Situation), so G is read here to give it back.
            const bool rawG = (!bindings::Allowed() || g_ctrl.on) && inputgate::ModKeysAllowed() && Focused() && (GetAsyncKeyState('G') & 0x8000) != 0;
            if (rawG && !gDown && !g_ctrl.on) logger::Write("Passenger: G pressed (bindings not taking input here; read directly)");
            if (!rawG && gDown) rawToggle = true;
            gDown = rawG;
            // The game's own exit (E) refused in the passenger seat ("obstructed"): still seated 1.5 s after the key went up -> out
            // our way, as with G.
            static bool eDown = false;
            static ULONGLONG eUpAt = 0;
            static int exitKey = 0;
            if (!exitKey) exitKey = controlmap::KeyFor("Vehicle", "VehicleExit", 'E');
            const bool eNow = Focused() && (GetAsyncKeyState(exitKey) & 0x8000) != 0;
            if (!eNow && eDown) eUpAt = now;
            eDown = eNow;
            const bool refused = eUpAt && now - eUpAt >= 1500;
            if (refused) { eUpAt = 0; logger::Write("Passenger: the game's exit did not let you out of %08X in 1.5 s", riding); }
            if(refused){ControlOff("getting out");GetOut(player,"the game's exit refused",true);return;}
            // G (2026-11-17, the user: "pressing G in a vehicle ... swap [control] if [a driver] exists otherwise does nothing / pressing G
            // again takes it back / that's all"): no seat change; leaving is the game's own hold E.
            if(toggle||rawToggle){
                if(g_ctrl.on)ControlOff("G: control back to the driver");
                else if(npcvehicles::PilotFor(riding)||npcvehicles::HasDriver(riding)){g_ctrl=Control{};g_ctrl.on=true;logger::Write("Passenger: G: you take control of %08X",riding);notify::Show("You have control");}
                else logger::Write("Passenger: G: nobody drives %08X; nothing to take",riding);
            }
            if (g_exitPending) {
                static ULONGLONG retryAt = 0;
                if (now - retryAt >= 500) { retryAt = now; GetOut(player, "asked earlier, now at rest", true); }
                return;
            }
			if (g_ctrl.on) ControlTick(vehicle, riding);  // your keys drive it; the NPC stays at the wheel
			if (!npcvehicles::PilotFor(riding) && !npcvehicles::HasDriver(riding)) GetOut(player, "the driver left");
			return;
		}
		g_arrived = false;
		g_exitPending = false;
		// Seated as a passenger by the game itself (its own get-in, our seat choice picking the passenger seat; 2026-10-29 12:30 run:
		// in FF0247C6 that way, nothing here knew, so G did nothing): taken as riding from here on.
		{
			static ULONGLONG checkAt = 0;
			if (now - checkAt >= 500) {
				checkAt = now;
				const uptr vehicle = VehicleOf(player);
				const std::intptr_t seat = vehicle ? SeatOf(player) : 0;
				if (vehicle && seat > 0 && seat != static_cast<std::intptr_t>(DriverSeat(vehicle))) {
					g_riding = At<std::uint32_t>(vehicle, off::kFormId);
					g_nearSince = 0; g_lead = 50.0;
					logger::Write("Passenger: the game seated you in %08X's passenger seat; G gets you out from here", g_riding.load());
					return;
				}
			}
		}
		if (press) Board(player);
	}

	void Reset()
	{
		g_riding = 0;
		g_keyDown = false;
		g_ctrl = Control{};
	}
}
