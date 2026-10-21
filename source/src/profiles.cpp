#include "profiles.h"

#include "addresses.h"
#include "controlmap.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "npcvehicles.h"
#include "vcontract.h"
#include "vehicleflight.h"
#include "vehicleworld.h"
#include "environment.h"
#include "targetsession.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace profiles
{
	namespace
	{
		using uptr = std::uintptr_t;
		using vcontract::Context;
		using vcontract::Goal;
		using vcontract::User;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr double kPatrolRadius = 120.0;   // m: the patrol square around home
		constexpr double kReach = 15.0;           // m: a waypoint this close is reached
		constexpr unsigned long long kWaitMs = 5000;  // at each end of a haul / courier run
		constexpr double kCruiseAhead = 300.0;
		constexpr std::uint8_t kNpc = std::uint8_t(User::Npc), kRemote = std::uint8_t(User::PlayerRemote), kDriving = std::uint8_t(User::Player);

		game::Vec3 PosOf(uptr ref) { return At<game::Vec3>(ref, off::kRefPosition); }
		double Dist2(uptr ref, double x, double y) { const auto p = PosOf(ref); return std::hypot(p.x - x, p.y - y); }
		void Step(Context& c, int s) { c.step = s; c.stepAt = GetTickCount64(); }
		bool Waited(const Context& c) { return GetTickCount64() - c.stepAt >= kWaitMs; }

		// ---- goal sources ----
		bool HoldGoal(Context& c, Goal& g)
		{
			g.x = c.homeX; g.y = c.homeY; g.stop = 1.5; g.hold = true;
			g.nav.priority = 10;
			g.nav.landOnArrival = c.user == User::PlayerRemote;  // your parked vehicle settles; NPC drivers hover on station
			return true;
		}

		bool ReturnHomeGoal(Context& c, Goal& g)
		{
			g.x = c.homeX; g.y = c.homeY; g.stop = 3.0; g.hold = true;  // arrive, then stay
			g.nav.priority = 15;
			g.nav.landOnArrival = true;
			return true;
		}

		bool PatrolGoal(Context& c, Goal& g)
		{
			static const double kCorner[4][2] = { { 1, 1 }, { 1, -1 }, { -1, -1 }, { -1, 1 } };
			const int k = c.step & 3;
			g.x = c.homeX + kCorner[k][0] * kPatrolRadius;
			g.y = c.homeY + kCorner[k][1] * kPatrolRadius;
			g.stop = 5.0;
			g.nav.passThrough = true;   // corners: keep flying
			g.nav.speedLimit = 25.0f;
			g.nav.priority = 20;
			if (vehicleflight::AtDestination(c.vehicle, g.x, g.y, g.z, g.stop,false)) {
				Step(c, c.step + 1);
				logger::Write("Profiles: [%d] patrol corner %d", c.pilot, c.step & 3);
			}
			return true;
		}

		// Hauler: home <-> your map marker, waiting kWaitMs at each end; no marker: waits at home.
		bool HaulerGoal(Context& c, Goal& g)
		{
			double mx = 0, my = 0, mz = std::numeric_limits<double>::quiet_NaN();
			const bool marker = vehicleflight::MarkerPoint(c.vehicle, mx, my, &mz);
			const bool toMarker = (c.step & 1) == 0 && marker;
			g.x = toMarker ? mx : c.homeX;
			g.y = toMarker ? my : c.homeY;
			g.z = toMarker ? mz : std::numeric_limits<double>::quiet_NaN();
			g.stop = 6.0;
			g.nav.speedLimit = 18.0f;   // loaded: steady
			g.nav.height = vcontract::NavParams::Height::Hug;
			g.nav.priority = 10;
			if (Dist2(c.vehicle, g.x, g.y) < kReach) {
				g.hold = true;
				g.nav.landOnArrival = true;
				if(!vehicleflight::AtDestination(c.vehicle,g.x,g.y,g.z,g.stop) ||
					environment::BoardingSite(c.vehicle).verdict!=environment::Verdict::Safe)c.stepAt=GetTickCount64();
				else if (marker && Waited(c)) Step(c, c.step + 1);
			}
			else {
				c.stepAt = GetTickCount64();  // the wait starts on arrival
			}
			return true;
		}

		// Courier: to your marker, wait, back home, done.
		bool CourierGoal(Context& c, Goal& g)
		{
			g.nav.priority = 40;
			g.nav.height = vcontract::NavParams::Height::High;  // fast and over things
			g.nav.squeeze = true;
			double mx = 0, my = 0, mz = std::numeric_limits<double>::quiet_NaN();
			if (c.step == 0) {
				if (!vehicleflight::MarkerPoint(c.vehicle, mx, my, &mz)) return HoldGoal(c, g);  // waits for a marker
				g.x = mx; g.y = my; g.z = mz; g.stop = 6.0;
				g.hold = Dist2(c.vehicle,mx,my) < kReach;
				if (vehicleflight::AtDestination(c.vehicle, mx, my, mz, g.stop)) { g.hold = true; if (Waited(c)) Step(c, 1); }
				else c.stepAt = GetTickCount64();
				return true;
			}
			g.x = c.homeX; g.y = c.homeY; g.stop = 3.0; g.hold = true;
			if (vehicleflight::AtDestination(c.vehicle, g.x, g.y, g.z, g.stop)) c.done = true;
			return true;
		}

		// Scout (your vehicle, nobody aboard): to your marker, a look, then back to you; done there.
		bool ScoutGoal(Context& c, Goal& g)
		{
			g.nav.priority = 30;
			g.nav.height = vcontract::NavParams::Height::High;  // a look-out: up where it can see
			double mx = 0, my = 0, mz = std::numeric_limits<double>::quiet_NaN();
			if (c.step == 0) {
				if (!vehicleflight::MarkerPoint(c.vehicle, mx, my, &mz)) { c.done = true; return false; }
				g.x = mx; g.y = my; g.z = mz; g.stop = 6.0;
				g.hold = Dist2(c.vehicle,mx,my) < kReach;
				if (vehicleflight::AtDestination(c.vehicle, mx, my, mz, g.stop)) { g.hold = true; if (GetTickCount64() - c.stepAt >= 3000) Step(c, 1); }
				else c.stepAt = GetTickCount64();
				return true;
			}
			if (!c.player) return false;
			const auto pp = PosOf(c.player);
			g.x = pp.x; g.y = pp.y; g.stop = 8.0;
			g.hold = Dist2(c.vehicle,pp.x,pp.y) < 12.0;
			if (vehicleflight::AtDestination(c.vehicle, g.x, g.y, g.z, g.stop)) c.done = true;
			return true;
		}

		// Cruise (you driving, Auto on): your view heading, a point kCruiseAhead ahead.
		bool CruiseGoal(Context& c, Goal& g)
		{
			double yaw = 0;
			if (!vehicleflight::ViewHeading(yaw)) return false;
			const auto p = PosOf(c.vehicle);
			g.x = p.x + std::sin(yaw) * kCruiseAhead;
			g.y = p.y + std::cos(yaw) * kCruiseAhead;
			g.stop = 1.0;
			g.nav.passThrough = true;
			g.nav.priority = 100;  // you are aboard
			return true;
		}

		// ---- Escort and pursuit: the follow / orbit goal of an NPC driver. The combat and the weapon (aggro, attack run, aim, bursts) are actions of the seat
		// owner (npcvehicles), which reads these goals like any other profile's.
		constexpr double kFollowBack = 12.0, kFollowSide = 4.0, kFollowSpread = 6.0, kFollowNear = 6.0, kFollowStop = 3.0;
		constexpr double kFollowLead = 1.5;       // s: the follow point leads a moving target by its velocity
		constexpr double kAttackRadius = 30.0;    // m: the pursuit orbit around the player
		constexpr double kAttackAbove = 6.0;      // m: attackers fly this far over the player's height
		constexpr int kTracks = 8;                // pilots 1..6 (index = pilot)
		struct Track
		{
			double tx = 0, ty = 0, tvx = 0, tvy = 0;  // the followed target's last position and smoothed velocity
			unsigned long long tAt = 0;
			double px = 0, py = 0, pz = 0, pvx = 0, pvy = 0, pvz = 0;  // the player's last position and smoothed velocity (climb lead)
			unsigned long long pAt = 0;
			bool convoy = false;                      // in the convoy column (hysteresis on the target's speed)
		} g_track[kTracks];
		// Convoy (2026-10-20, the user): the target moving faster than kConvoyOn, escorts line up in one column behind it (kFollowBack, then
		// kConvoyGap more per slot), back to the side-by-side formation under kConvoyOff.
		constexpr double kConvoyOn = 8.0, kConvoyOff = 4.0, kConvoyGap = 12.0;

		uptr InVehicleOf(uptr actor)
		{
			__try {
				return EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(actor);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// The player's smoothed velocity (the smoothing the weapon aim used), for the height lead.
		void TrackPlayer(Track& t, const game::Vec3& pp)
		{
			const unsigned long long now = GetTickCount64();
			if (t.pAt && now > t.pAt) {
				const double dt = (now - t.pAt) / 1000.0;
				const double ix = (pp.x - t.px) / dt, iy = (pp.y - t.py) / dt, iz = (pp.z - t.pz) / dt;
				if (dt < 1.0 && std::sqrt(ix * ix + iy * iy + iz * iz) < 150.0) {
					const double a = std::min(1.0, dt * 4.0);
					t.pvx += (ix - t.pvx) * a;
					t.pvy += (iy - t.pvy) * a;
					t.pvz += (iz - t.pvz) * a;
				}
				else {
					t.pvx = t.pvy = t.pvz = 0;
				}
			}
			t.px = pp.x; t.py = pp.y; t.pz = pp.z; t.pAt = now;
		}

		bool EscortGoal(Context& c, Goal& g)
		{
			if (!c.player || !c.vehicle || c.pilot < 1 || c.pilot >= kTracks) return false;
			// Other allied vehicles defend in combat, then hold their original position.
			if(!targetsession::CanFollow(c.vehicleId,vehicleflight::OwnRoverId())) {
				g.x=c.homeX;g.y=c.homeY;g.hold=true;g.stop=3.0;return true;
			}
			Track& t = g_track[c.pilot];
			const auto pl = PosOf(c.player);
			TrackPlayer(t, pl);
			// The player in their vehicle: follow that vehicle (position and heading), led by its velocity so a fast one is kept up with.
			uptr target = InVehicleOf(c.player);
			if (!target || target == c.vehicle) target = c.player;
			const auto tp = PosOf(target);
			const unsigned long long now = GetTickCount64();
			if (t.tAt && now > t.tAt) {
				const double dt = (now - t.tAt) / 1000.0;
				const double vx = (tp.x - t.tx) / dt, vy = (tp.y - t.ty) / dt;
				if (dt < 1.0 && std::hypot(vx, vy) < 250.0) {
					const double a = std::min(1.0, dt * 3.0);
					t.tvx += (vx - t.tvx) * a;
					t.tvy += (vy - t.tvy) * a;
				}
				else {
					t.tvx = t.tvy = 0;
				}
			}
			t.tx = tp.x; t.ty = tp.y; t.tAt = now;
			const double tsp = std::hypot(t.tvx, t.tvy);
			game::Vec3 pp = tp;
			pp.x += float(t.tvx * kFollowLead);
			pp.y += float(t.tvy * kFollowLead);
			const auto vp = PosOf(c.vehicle);
			const double yaw = tsp > 3.0 ? std::atan2(t.tvx, t.tvy) : At<game::Vec3>(target, off::kRefRotation).z;  // moving: its track
			// Formation: slot 0 left, 1 right, then rows further back, alternating sides.
			const int slot = c.pilot - 1;
			if (!t.convoy && tsp > kConvoyOn) t.convoy = true;
			else if (t.convoy && tsp < kConvoyOff) t.convoy = false;
			double side = (slot % 2 ? -1.0 : 1.0) * (kFollowSide + 2.0 + 3.0 * (slot / 2));  // + left, - right
			double back = kFollowBack + kFollowSpread * (slot / 2);
			if (t.convoy) {
				// One column: the slots in order behind the target, on its track.
				side = 0.0;
				back = kFollowBack + kConvoyGap * slot;
			}
			g.x = pp.x - std::sin(yaw) * back - std::cos(yaw) * side;
			g.y = pp.y - std::cos(yaw) * back + std::sin(yaw) * side;
			const double to = std::hypot(double(vp.x) - pp.x, double(vp.y) - pp.y);
			if (to < kFollowNear && to > 0.1) {  // too close: back off along the line from the player
				g.x = pp.x + (vp.x - pp.x) / to * back;
				g.y = pp.y + (vp.y - pp.y) / to * back;
			}
			g.z = pl.z + std::clamp(t.pvz * 1.5, -15.0, 15.0);
			g.stop = kFollowStop;
			return true;
		}

		bool PursuitGoal(Context& c, Goal& g)
		{
			if (!c.player || !c.vehicle || c.pilot < 1 || c.pilot >= kTracks) return false;
			Track& t = g_track[c.pilot];
			const auto pp = PosOf(c.player);
			TrackPlayer(t, pp);
			const auto vp = PosOf(c.vehicle);
			const double dir = (c.pilot % 2) ? 1.0 : -1.0;  // each pilot its own side of the orbit
			const double ang = std::atan2(double(vp.x) - pp.x, double(vp.y) - pp.y) + dir * 0.6;
			g.x = pp.x + std::sin(ang) * kAttackRadius;
			g.y = pp.y + std::cos(ang) * kAttackRadius;
			g.z = pp.z + std::clamp(t.pvz * 1.5, -15.0, 15.0) + kAttackAbove;
			g.stop = kFollowStop;
			return true;
		}

		constexpr std::uint8_t kAll = std::uint8_t(kNpc | kRemote | kDriving);
		using vcontract::Class;
		const vcontract::Profile kEscort{ "escort", kNpc, &EscortGoal, false };
		// Combat (npcvehicles reads the selection; attack also gives a hostile pursuer, which has no navigation, its orbit round the player).
		const vcontract::Profile kAttack{ "attack", kNpc, &PursuitGoal, false, Class::Combat };
		const vcontract::Profile kDefend{ "defend", kAll, nullptr, false, Class::Combat };
		// Weapons (selectors; free fire is the default; the player's seat is "never" in npcvehicles' fire gate).
		const vcontract::Profile kFreeFire{ "free fire", kAll, nullptr, false, Class::Weapons };
		const vcontract::Profile kOpportunistic{ "opportunistic", kAll, nullptr, false, Class::Weapons };
		const vcontract::Profile kHoldFire{ "hold fire", kAll, nullptr, false, Class::Weapons };
		const vcontract::Profile kNever{ "never", kAll, nullptr, false, Class::Weapons };
		const vcontract::Profile kHold{ "hold", std::uint8_t(kNpc | kRemote), &HoldGoal, false };
		const vcontract::Profile kReturnHome{ "return home", std::uint8_t(kNpc | kRemote), &ReturnHomeGoal, false };
		const vcontract::Profile kPatrol{ "patrol", kNpc, &PatrolGoal, false };
		const vcontract::Profile kHauler{ "hauler", kNpc, &HaulerGoal, false };
		const vcontract::Profile kCourier{ "courier", kNpc, &CourierGoal, false };
		const vcontract::Profile kScout{ "scout", kRemote, &ScoutGoal, false };
		const vcontract::Profile kCruise{ "cruise", kDriving, &CruiseGoal, false };

		const vcontract::Profile* const kNpcCycle[] = { nullptr, &kPatrol, &kHauler, &kCourier, &kHold, &kReturnHome };
		const vcontract::Profile* const kRemoteCycle[] = { nullptr, &kScout, &kReturnHome, &kHold };
		int g_npcIndex = 0, g_remoteIndex = 0;
		int g_npcKey = 0, g_ownKey = 0;
		bool g_npcDown = false, g_ownDown = false, g_keysChosen = false;

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}

		void ChooseKeys()
		{
			if (g_keysChosen) return;
			g_keysChosen = true;
			g_npcKey = controlmap::DebugKeys() && controlmap::Claim("profiles", "npc profile (debug)", VK_OEM_4) ? VK_OEM_4 : 0;
			g_ownKey = controlmap::Claim("profiles", "own vehicle profile", VK_OEM_6) ? VK_OEM_6 : 0;
			logger::Write("Profiles: keys: NPC vehicles %s, your vehicle %s", g_npcKey ? controlmap::KeyName(g_npcKey).c_str() : "NONE FREE",
				g_ownKey ? controlmap::KeyName(g_ownKey).c_str() : "NONE FREE");
		}

		bool Pressed(int vk, bool& down)
		{
			const bool h = vk && Focused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
			const bool p = h && !down;
			down = h;
			return p;
		}

		// Every NPC vehicle following you (a pilot drives it) gets the next profile of the NPC cycle.
		void CycleNpc()
		{
			g_npcIndex = (g_npcIndex + 1) % int(sizeof(kNpcCycle) / sizeof(kNpcCycle[0]));
			const auto* p = kNpcCycle[g_npcIndex];
			world::Snapshot snap;
			int n = 0;
			if (world::Vehicles(snap)) {
				for (std::uint32_t i = 0; i < snap.count; ++i) {
					const auto& v = snap.v[i];
					if (!npcvehicles::PilotFor(v.formId)) continue;
					vcontract::Assign(v.formId, p, v.pos[0], v.pos[1]);
					++n;
				}
			}
			logger::Write("NPC profile change: %d vehicles",n);
			logger::Write("Profiles: NPC vehicles (%d) -> %s", n, p ? p->name : "default");
		}

		void CycleOwn(uptr player)
		{
			const std::uint32_t own = vehicleflight::OwnRoverId();
			if (!own) {
				notify::Show("You own no vehicle");
				return;
			}
			const uptr driven = EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(player);
			if (driven && At<std::uint32_t>(driven, 0x28) == own) {
				// Driving: what the autopilot does at your destination, Stop -> Orbit -> Patrol (the route loops) -> Cruise (where you look) -> Stop.
				using vehicleflight::AutoMode;
				const bool cruise = vcontract::AssignedTo(own) == &kCruise;
				const AutoMode mode = vehicleflight::GetAutoMode();
				if (cruise) {
					vcontract::Assign(own, nullptr, 0, 0);
					vehicleflight::SetAutoMode(AutoMode::Stop);
					notify::Show("Autopilot: stop at the destination");
				}
				else if (mode == AutoMode::Stop) { vehicleflight::SetAutoMode(AutoMode::Orbit); notify::Show("Autopilot: orbit the destination"); }
				else if (mode == AutoMode::Orbit) { vehicleflight::SetAutoMode(AutoMode::Patrol); notify::Show("Autopilot: patrol the route"); }
				else {
					vehicleflight::SetAutoMode(AutoMode::Stop);
					vcontract::Assign(own, &kCruise, 0, 0);
					notify::Show("Autopilot: cruise (where you look)");
				}
				return;
			}
			g_remoteIndex = (g_remoteIndex + 1) % int(sizeof(kRemoteCycle) / sizeof(kRemoteCycle[0]));
			const auto* p = kRemoteCycle[g_remoteIndex];
			world::Snapshot snap;
			double hx = 0, hy = 0;
			if (world::Vehicles(snap)) {
				for (std::uint32_t i = 0; i < snap.count; ++i) {
					if (snap.v[i].formId == own) { hx = snap.v[i].pos[0]; hy = snap.v[i].pos[1]; }
				}
			}
			vcontract::Assign(own, p, hx, hy);
			notify::Show(p ? p->name : "Your vehicle: default");
		}

	}

	void Install()
	{
		for (const auto* p : { &kEscort, &kHold, &kReturnHome, &kPatrol, &kHauler, &kCourier, &kScout, &kCruise, &kAttack, &kDefend,
			&kFreeFire, &kOpportunistic, &kHoldFire, &kNever }) vcontract::Register(p);
		logger::Write("Profiles: %d registered (navigation: escort, hold, return home, patrol, hauler, courier, scout, cruise; combat: attack, defend; "
			"weapons: free fire, opportunistic, hold fire, never)", vcontract::Count());
	}

	void Tick()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) return;
		ChooseKeys();
		if (Pressed(g_npcKey, g_npcDown)) CycleNpc();
		if (Pressed(g_ownKey, g_ownDown)) CycleOwn(player);
	}

	void Reset()
	{
		vcontract::Reset();
		g_npcIndex = g_remoteIndex = 0;
		for (auto& t : g_track) t = Track{};
	}
}
