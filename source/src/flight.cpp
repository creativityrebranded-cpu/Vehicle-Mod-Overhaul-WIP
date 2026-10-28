#include "flight.h"

#include "addresses.h"
#include "anchor.h"
#include "camtune.h"
#include "flightmodel.h"
#include "flighttune.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "parking.h"
#include "prefetch.h"
#include "rebase.h"
#include "recentre.h"
#include "handoff.h"
#include "shipflight.h"
#include "shipmodel.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

namespace flight
{
	namespace
	{
		// Flight laws: the flight model (flightmodel.h: attitude, momentum, banking, stall, terrain assist, hard floor).
		// Easy scheme: mouse Y pitch, mouse X turn (hull banks for looks), W/S throttle, A/D slide, Space/Ctrl up/down;
		// top speed 100 m/s (plus up to 30 m/s of slide / lift).
		constexpr double kMouseScale = 1.0;      // captured mouse counts -> model counts (fm::Params::mouseGain)
		constexpr float kProbeAbove = 2000.0f;   // ground samples cast top-down from this far above the ship
		constexpr float kProbeLength = 8000.0f;
		constexpr unsigned kProbeMaxAge = 2;     // frames: older answers count as no data
		constexpr double kLandHeight = 80.0;     // metres: below this (and slow) F7 lands
		constexpr double kLandSpeed = 30.0;      // m/s
		constexpr double kAutoLandHeight = 15.0; // metres: staying below this and slow starts the autoland countdown
		constexpr double kAutoLandSpeed = 15.0;  // m/s
		constexpr int kAutoLandSeconds = 3;

		struct State
		{
			bool active = false;
			double x = 0, y = 0, z = 0;        // our own position (we write it every frame)
			double vx = 0, vy = 0, vz = 0;
			double throttle = 0;
			double elevation = -1;             // metres above ground, -1 = unknown
			LARGE_INTEGER last{};
			ULONGLONG lastHud = 0, lastSample = 0;
			ULONGLONG lowSince = 0;      // autoland: when the player went low and slow
			int lastCountdown = -1;
			bool keyDown = false;
			bool shipHidden = false;
			ULONGLONG startMs = 0;
			// Flight model
			fm::Model model;
			std::vector<fm::Request> asked;   // requests sent to the probe slots last tick (answered by now)
			float setPitch = 0, setYaw = 0;   // the player rotation we wrote last tick (game: x pitch down+, z yaw)
			bool everFast = false;            // no stall until the ship has once flown faster than stall speed
			fm::Mode lastMode = fm::Mode::Cruise;
			ULONGLONG lastModeMs = 0;
			bool landing = false;             // F7: the ship settles straight down, then lands
			bool freeLook = false;            // middle mouse toggles: false = the mouse flies the ship, true = free camera
			bool toggleDown = false;
		} g;

		// Mouse counts: captured by the core plugin's look-handler hook (core_api: CaptureMouse / TakeMouse).

		double Wrap(double a)
		{
			while (a > 3.14159265358979) a -= 6.28318530717959;
			while (a < -3.14159265358979) a += 6.28318530717959;
			return a;
		}

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}
		bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

		// Third person while flying: the space flight camera sits far behind the ship (fFlightCameraFarOffsetBase 0, -30,
		// 20); on foot the zoom is capped at fMaxCurrentZoom (2.0). Raise the cap while flying so the view can pull far
		// out (mouse wheel), restore it on landing.
		constexpr float kFlightMaxZoom = 30.0f;
		float g_savedMaxZoom = -1.0f;
		void WideCamera(bool on)
		{
			auto& maxZoom = *reinterpret_cast<float*>(game::Base() + addr::kMaxCurrentZoom);
			if (on && g_savedMaxZoom < 0.0f) {
				g_savedMaxZoom = maxZoom;
				maxZoom = kFlightMaxZoom;
				logger::Write("Flight: third-person zoom cap %.1f -> %.1f", g_savedMaxZoom, maxZoom);
			}
			else if (!on && g_savedMaxZoom >= 0.0f) {
				maxZoom = g_savedMaxZoom;
				g_savedMaxZoom = -1.0f;
			}
		}

		bool NoClip() { return *reinterpret_cast<std::uint8_t*>(game::Base() + addr::kNoClipFlag) != 0; }

		// The console's ToggleCollision (tcl) with no target: no-clip on/off for the player (gravity and
		// player collision off), and the physics worlds' matching flag.
		void ToggleNoClip()
		{
			float result = 0;
			std::uint32_t opcode = 0;
			EngineFn<char (*)(void*, void*, std::uintptr_t, std::uintptr_t, void*, void*, float*, std::uint32_t*)>(addr::kToggleCollision)(
				nullptr, nullptr, 0, 0, nullptr, nullptr, &result, &opcode);
		}

		// Ground height under a point, via the probe service (runs inside the player's update; see prefetch.h).
		// Returns the latest result for the slot (one frame behind), -FLT_MAX if none or nothing was hit.
		float Probe(int slot, double x, double y, double z, float above, float length)
		{
			prefetch::RequestGround(slot, float(x), float(y), float(z), above, length);
			float ground = -3.4e38f;
			unsigned age = 0;
			if (!prefetch::LatestGround(slot, ground, age) || age > 30) {
				return -3.4e38f;
			}
			return ground;
		}
		float Ground(double x, double y, double z) { return Probe(prefetch::kSlotFlight, x, y, z, 10.0f, 5000.0f); }
		float GroundFromAbove(double x, double y, double z) { return Probe(prefetch::kSlotGuard, x, y, z, 4000.0f, 30000.0f); }

		// Whether the guard slot has a recent probe answer at all (vs "asked, no answer yet").
		bool GuardProbeAnswered()
		{
			float ground;
			unsigned age;
			return prefetch::LatestGround(prefetch::kSlotGuard, ground, age) && age <= 30;
		}

		// The player's own per-frame position call (what no-clip movement uses): actor vtable +0x9B8.
		void PlacePlayer(std::uintptr_t player, double x, double y, double z)
		{
			float p[3] = { float(x), float(y), float(z) };
			auto** vtbl = *reinterpret_cast<void***>(player);
			reinterpret_cast<void (*)(std::uintptr_t, float*, char)>(vtbl[0x9B8 / 8])(player, p, 1);
		}

		// Flight is third person only: if the camera is in first person, switch it (also blocks the view key).
		void KeepThirdPerson()
		{
			const auto cam = Ptr(addr::kPlayerCameraPtr);
			if (cam && At<std::uintptr_t>(cam, 0x10) && At<std::uintptr_t>(cam, 0x10) == At<std::uintptr_t>(cam, 0x188)) {
				EngineFn<void (*)(std::uintptr_t)>(addr::kForceThirdPerson)(cam);
			}
		}

		void Start(std::uintptr_t player, bool fromSeat = false)
		{
			if (!NoClip()) {
				ToggleNoClip();
			}
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			g = State{};
			g.active = true;
			g.x = p.x;
			g.y = p.y;
			g.z = p.z + (fromSeat ? 0.0 : 3.0);
			const auto r0 = At<game::Vec3>(player, off::kRefRotation);
			g.model.Reset(g.x, g.y, g.z, r0.z, 0.0);  // from a hover: W to go, nose to climb
			flighttune::Apply(g.model.p);  // in-game tuned values (flight.ini)
			g.setPitch = r0.x;
			g.setYaw = r0.z;
			QueryPerformanceCounter(&g.last);
			KeepThirdPerson();
			shipmodel::Show(true);  // the hull copied from the ship's module references (shipmodel.cpp)
			g.startMs = GetTickCount64();
			camtune::Begin();
			logger::Write("Flight: on at (%.0f, %.0f, %.0f)", p.x, p.y, p.z);
			notify::Show("Flight ON: mouse steers (middle mouse: free look), W/S speed, A/D slide, Space/Ctrl up/down, F7 land, Shift+F7 drop out");
			telemetry::Record("flight_on", "\"pos\":[%.1f,%.1f,%.1f]", p.x, p.y, p.z);
		}

		void Stop(const char* why)
		{
			CaptureMouse(false);
			if (NoClip()) {
				ToggleNoClip();
			}
			g.active = false;
			shipmodel::Show(false);
			camtune::End();
			logger::Write("Flight: off (%s)", why);
			notify::Show("Flight off (%s)", why);
			telemetry::Record("flight_off", "\"reason\":\"%s\"", why);
		}

		// Brings the player's home ship next to them on the ground (single move through the engine's move
		// routine), if it is in the same worldspace.
		void BringShip(std::uintptr_t player, double groundZ)
		{
			std::uintptr_t ship = 0;
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
			if (!ship) {
				notify::Show("Landed (no home ship found)");
				return;
			}
			const auto playerCell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = playerCell ? At<std::uintptr_t>(playerCell, off::kCellWorldspace) : 0;
			if (!ws) {
				notify::Show("Landed (not in an open world area: your ship stays where it is)");
				EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
				return;
			}
			// The ship lands here: visible again, moved by the engine's move routine into the player's cell and worldspace
			// (the same path Papyrus MoveTo uses for spaceships), its parked spot cleared.
			if (At<std::uint32_t>(ship, 0x20) & 0x800) {
				EngineFn<void (*)(std::uintptr_t)>(addr::kEnableReference)(ship);
			}
			const auto rot = At<game::Vec3>(player, off::kRefRotation);
			const double yaw = rot.z;
			float pos[3] = { float(g.x + std::sin(yaw) * 40.0), float(g.y + std::cos(yaw) * 40.0), float(groundZ) };
			float r[3] = { 0.0f, 0.0f, float(yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(
				ship, &handle, playerCell, ws, pos, r);
			parking::Clear();
			const auto now = At<game::Vec3>(ship, off::kRefPosition);
			logger::Write("Flight: ship landed at (%.0f, %.0f, %.0f) (asked (%.0f, %.0f, %.0f))", now.x, now.y, now.z, pos[0], pos[1], pos[2]);
			notify::Show("Landed: your ship is just ahead");
			telemetry::Record("flight_ship_moved", "\"pos\":[%.0f,%.0f,%.0f],\"got\":[%.0f,%.0f,%.0f]", pos[0], pos[1], pos[2], now.x, now.y, now.z);
			EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
		}
	}

	namespace
	{
		// Launch from the pilot seat of a landed ship: leave the seat (the console's ExitPilotSeat), step out of
		// the ship onto its cockpit (the engine's move routine, as Papyrus MoveTo does; this crosses from the
		// ship's interior cell to the planet, so the game shows its usual exit load), then fly from there.
		struct Launch
		{
			int stage = 0;  // 0 none, 3 snapshot of the seated pilot, 1 leaving the seat, 2 moving out, waiting to be outdoors
			ULONGLONG since = 0;
			std::uintptr_t ship = 0;
		} launch;

		bool InPilotSeat(std::uintptr_t player, std::uintptr_t ship)
		{
			std::uintptr_t subject = ship;
			float result = 0.0f;
			EngineFn<char (*)(std::uintptr_t*, std::uintptr_t, std::uintptr_t, float*)>(addr::kCondGetActorInShipPilotSeat)(&subject, player, 0, &result);
			return result > 0.5f;
		}

		bool InSpace(std::uintptr_t ref)
		{
			std::uintptr_t subject = ref;
			float result = 0.0f;
			EngineFn<char (*)(std::uintptr_t*, std::uintptr_t, std::uintptr_t, float*)>(addr::kCondIsInSpace)(&subject, 0, 0, &result);
			return result > 0.5f;
		}

		// The landed ship whose pilot seat the player is in, or 0.
		std::uintptr_t SeatedShip(std::uintptr_t player)
		{
			if (!anchor::InShipInterior(player)) {
				return 0;
			}
			const auto ship = anchor::ShipOf(player);
			return ship && InPilotSeat(player, ship) ? ship : 0;
		}

		void BeginLaunch(std::uintptr_t ship)
		{
			if (InSpace(ship)) {
				notify::Show("Flight: only on a planet (use the ship's own controls in space)");
				return;
			}
			const auto cell = At<std::uintptr_t>(ship, off::kRefParentCell);
			if (!cell || !At<std::uintptr_t>(cell, off::kCellWorldspace)) {
				notify::Show("Flight: the ship is not landed in an open world area");
				return;
			}
			EngineFn<char (*)()>(addr::kExitPilotSeat)();  // no pilot in the flown ship: just leave the seat
			launch = Launch{ 1, GetTickCount64(), ship };
			logger::Write("Flight: launching from the pilot seat");
			telemetry::Record("flight_launch", "\"stage\":\"leave_seat\"");
			notify::Show("Launching...");
		}

		// Returns true while a launch is in progress (the caller does nothing else then).
		bool RunLaunch(std::uintptr_t player, bool outdoors)
		{
			if (launch.stage == 0) {
				return false;
			}
			const ULONGLONG t = GetTickCount64() - launch.since;
			if (launch.stage == 3) {
				if (!shipmodel::PilotReady() && t < 1000) {
					return true;  // the snapshot is taken in the next game tick
				}
				EngineFn<char (*)()>(addr::kExitPilotSeat)();  // (the hull is copied once outside, next to the ship)
				launch.stage = 1;
				launch.since = GetTickCount64();
				return true;
			}
			if (launch.stage == 1) {
				if (InPilotSeat(player, launch.ship) && t < 3000) {
					return true;  // still getting up
				}
				if (t < 600) {
					return true;  // let the stand-up finish
				}
				const auto cell = At<std::uintptr_t>(launch.ship, off::kRefParentCell);
				const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
				if (!ws) {
					launch = Launch{};
					notify::Show("Flight: launch cancelled (the ship's area is not available)");
					return false;
				}
				const auto sp = At<game::Vec3>(launch.ship, off::kRefPosition);
				const float yaw = At<game::Vec3>(launch.ship, off::kRefRotation).z;
				float k[3] = { 0.0f, 0.0f, 12.0f };  // just above the ship, or on the cockpit once the model is copied
				shipmodel::CockpitOffset(k);
				float pos[3] = { sp.x + k[0] * std::cos(yaw) + k[1] * std::sin(yaw), sp.y - k[0] * std::sin(yaw) + k[1] * std::cos(yaw), sp.z + k[2] };
				float rot[3] = { 0.0f, 0.0f, yaw };
				std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
				EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(
					player, &handle, cell, ws, pos, rot);
				logger::Write("Flight: moved out of the ship to (%.0f, %.0f, %.0f) (above the ship)", pos[0], pos[1], pos[2]);
				launch.stage = 2;
				launch.since = GetTickCount64();
				return true;
			}
			// stage 2: outdoors and the cell ready, then fly.
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			if (outdoors && cell && At<std::uint8_t>(cell, off::kCellState) == 10) {
				launch = Launch{};
				Start(player, true);
				return false;
			}
			if (t > 60000) {
				launch = Launch{};
				notify::Show("Flight: launch timed out");
				logger::Write("Flight: launch timed out waiting to be outdoors");
			}
			return true;
		}

		// Load guard: after a load or transition the game rebuilds terrain around the zone's landing point, not
		// around the player, so far from it the ground under the player can take seconds to exist (observed: a
		// 16 km fall and the game's rescue back to the landing area). Hold the player in place with gravity off
		// until the ground under them is there; if they ever end up below it, put them back on top.
		struct Guard
		{
			bool holding = false;
			bool weEnabledNoClip = false;
			double x = 0, y = 0, z = 0;
			ULONGLONG since = 0;
			bool wasOutdoors = false;
		} guard;

		// Player height minus the ray's ground under them, measured on foot (see RunGuard).
		float g_standBias = 0.0f;
		bool g_haveBias = false;

		void ReleaseGuard(std::uintptr_t player, const char* why, float ground)
		{
			if (ground > -1e30f && guard.z < ground + 0.5) {
				PlacePlayer(player, guard.x, guard.y, ground + 0.5);
			}
			if (guard.weEnabledNoClip && NoClip()) {
				ToggleNoClip();
			}
			guard.holding = false;
			guard.weEnabledNoClip = false;
			logger::Write("Load guard: released (%s)", why);
			telemetry::Record("load_guard_release", "\"why\":\"%s\",\"held_ms\":%llu", why, GetTickCount64() - guard.since);
		}

		void RunGuard(std::uintptr_t player, bool outdoors, bool f7Pressed)
		{
			const bool arrived = outdoors && !guard.wasOutdoors;  // load, door, fast travel, landing
			guard.wasOutdoors = outdoors;
			if (!outdoors) {
				guard.holding = false;
				return;
			}
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const bool cellReady = cell && At<std::uint8_t>(cell, off::kCellState) == 10;
			const float ground = GroundFromAbove(p.x, p.y, p.z);
			const bool answered = GuardProbeAnswered();
			const bool groundMissing = answered && ground < -1e30f;  // asked and nothing there

			// Hold only while there is no ground under the player (a cell can stay "loading" long after its ground is
			// there; waiting for full attachment froze loads for the whole timeout).
			if (arrived && !guard.holding && (groundMissing || (!answered && !cellReady))) {
				guard = Guard{ true, false, p.x, p.y, p.z, GetTickCount64(), true };
				if (!NoClip()) {
					ToggleNoClip();
					guard.weEnabledNoClip = true;
				}
				logger::Write("Load guard: holding at (%.0f, %.0f, %.0f) until the ground loads (cell %s, ground %s)", p.x, p.y, p.z,
					cellReady ? "ready" : "loading", answered ? (groundMissing ? "missing" : "present") : "not checked yet");
				telemetry::Record("load_guard_hold", "\"pos\":[%.0f,%.0f,%.0f],\"cell_ready\":%d,\"answered\":%d", p.x, p.y, p.z, cellReady ? 1 : 0, answered ? 1 : 0);
				notify::Show("Waiting for the ground to load...");
			}
			if (guard.holding) {
				const ULONGLONG held = GetTickCount64() - guard.since;
				if (f7Pressed) {
					ReleaseGuard(player, "cancelled with F7", ground);
				}
				else if (answered && !groundMissing) {
					ReleaseGuard(player, "ground loaded", ground);
				}
				else if (cellReady && held > 1000) {
					// The player's own cell is attached: its ground and collision are in. (The ground probe runs in a
					// game update that only starts some seconds after a load, so it cannot be waited on alone.)
					ReleaseGuard(player, answered ? "cell ready (ground probe: nothing hit)" : "cell ready", ground);
				}
				else if (held > 30000) {
					ReleaseGuard(player, "timeout", ground);
				}
				else {
					PlacePlayer(player, guard.x, guard.y, guard.z);
				}
				return;
			}
			// Fell through the ground anyway: back on top. OFF: the ground ray answers tens of metres off the walking
			// surface in places (0.32.2: it lifted the player into the sky and held them there every frame).
			if (answered && ground > -1e30f && p.z < ground - 3.0f && ground - p.z < 100.0f) {  // sanity: never lift by 100 m or more
				PlacePlayer(player, p.x, p.y, ground + 0.5);
				logger::Write("Load guard: player was %.0f m below the ground, put back on top", ground - p.z);
				telemetry::Record("below_ground_fix", "\"depth\":%.1f", ground - p.z);
			}
			// The ray's ground against where the player actually stands (on foot: the true walking surface). Observed: the ray
			// answering about 40 m below the surface in places (summoned ship landed underground). Kept for corrections and
			// logged every 5 s so the offset can be mapped.
			if (answered && ground > -1e30f) {
				g_standBias = p.z - ground;
				g_haveBias = true;
				static ULONGLONG lastBiasLog = 0;
				const ULONGLONG nowMs = GetTickCount64();
				if (nowMs - lastBiasLog > 5000) {
					lastBiasLog = nowMs;
					telemetry::Record("ground_probe", "\"pos\":[%.0f,%.0f,%.1f],\"ray\":%.1f,\"offset\":%.1f", p.x, p.y, p.z, ground, g_standBias);
					if (std::fabs(g_standBias) > 2.0f) {
						logger::Write("Ground ray: player stands at %.1f m, the ray answers %.1f m (offset %.1f m) at (%.0f, %.0f)", p.z, ground,
							g_standBias, p.x, p.y);
					}
				}
			}
		}
	}

	namespace
	{
		// Mouse for the ship (user: the camera must not be the control): the game's mouse-look handler
		// (PlayerControls::LookHandler vftable[6], FUN_1412bc700) writes the frame's mouse counts into the controls data
		// (+0xC x, +0x10 y, y flipped so up is positive). While flying in control mode they are taken for the ship and
		// zeroed, so the camera does not orbit away: it stays behind the player, who is the ship. In free-look mode they
		// pass through (the camera is free, the ship holds its course).
	}

	bool Install()
	{
		// The look-handler mouse hook now lives in the core plugin (shared with the vehicle plugin).
		return true;
	}

	namespace
	{
		// Summon (F6, on foot outdoors), the way NPC ships come and go (user: the NPC take-off, never the player's own,
		// which launches to space): if the ship is in view (shown, within the unhidden range), it first takes off with
		// the NPC take-off (FUN_1421199b0, only when the player is neither aboard nor its pilot), then it is hidden and
		// lands next to the player with the landing that Papyrus SpaceshipReference.EnableWithLanding uses: a landing
		// marker at the spot, FUN_1421219b0(ship, marker), then enabled. No terrain probe: the landing height is the
		// player's own (the terrain assist is for flight only).
		constexpr ULONGLONG kTakeOffMs = 10000;  // the take-off plays this long before the ship is hidden for the landing
		struct Summon
		{
			int stage = 0;  // 0 none, 1 take off or not, 4 taking off, 2 hidden: link + place, 6 inbound (ETA), 3 enable, 5 watch
			ULONGLONG since = 0;
			float x = 0, y = 0, z = 0, yaw = 0;
			bool keyDown = false;
			std::uint32_t markerHandle = 0;
			float fromX = 0, fromY = 0;   // where the ship was when called
			ULONGLONG etaAt = 0, lastNotice = 0;
		} summon;

		std::uintptr_t RefFromHandle(std::uint32_t handle)
		{
			const auto handles = Ptr(addr::kRefHandleTablePtr);
			if (!handles || !handle) {
				return 0;
			}
			std::uintptr_t ref = 0;
			std::uintptr_t* out = &ref;
			struct
			{
				std::uintptr_t vtbl;
				std::uintptr_t** capture;
			} lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
			EngineFn<void (*)(std::uintptr_t, std::uint32_t, void*)>(addr::kHandleLookup)(handles, handle, &lookup);
			return ref;  // addref'd: release with kRefRelease
		}

		// A landing marker (the game's own landing-marker base) at the spot, reused for later summons.
		std::uintptr_t PlaceMarker(std::uintptr_t player)
		{
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			float pos[3] = { summon.x, summon.y, summon.z };
			float rot[3] = { 0.0f, 0.0f, summon.yaw };
			if (const auto old = RefFromHandle(summon.markerHandle)) {
				std::uint64_t h = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
				EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(
					old, &h, cell, ws, pos, rot);
				return old;
			}
			const auto base = Ptr(addr::kLandingMarkerBasePtr);
			const auto dataHandler = Ptr(addr::kDataHandlerPtr);
			if (!ws || !base || !dataHandler) {
				return 0;
			}
			alignas(16) std::uint8_t d[0xC0] = {};
			const auto dp = reinterpret_cast<std::uintptr_t>(d);
			At<std::uintptr_t>(dp, 0x00) = game::Base() + addr::kNewRefrDataVtbl;
			At<float>(dp, 0x10) = pos[0];
			At<float>(dp, 0x14) = pos[1];
			At<float>(dp, 0x18) = pos[2];
			At<float>(dp, 0x24) = rot[2];  // heading (rotation x, y, z at +0x1C)
			At<float>(dp, 0x28) = 1.0f;    // scale
			At<std::uintptr_t>(dp, 0x30) = base;
			At<std::uintptr_t>(dp, 0x38) = cell;
			At<std::uintptr_t>(dp, 0x40) = ws;
			At<std::uint8_t>(dp, 0x8C) = 1;
			At<std::uint8_t>(dp, 0x8D) = 1;
			At<std::uint32_t>(dp, 0x98) = 1;
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			EngineFn<void* (*)(std::uintptr_t, std::uint64_t*, void*)>(addr::kCreateReference)(dataHandler, &handle, d);
			summon.markerHandle = static_cast<std::uint32_t>(handle);
			return RefFromHandle(summon.markerHandle);
		}

		// ---- Aim (computed at the sync point, game-update thread) -------------------------------------------------
		// Where the player is looking, on the ground, up to kAimRange: a march along the look direction from eye height
		// with the game's ground ray (FUN_140583B70) at each step, refined by bisection; then a flatness check (ground
		// heights on a ring around the point) and, when too steep, the nearest flat spot within kSnapRange.
		constexpr float kAimRange = 500.0f;
		constexpr float kAimStep = 4.0f;
		constexpr float kPadRadius = 14.0f;     // ring of ground samples for the flatness check
		constexpr float kPadMaxRise = 3.5f;     // highest minus lowest ground on the pad
		constexpr float kSnapRange = 50.0f;
		constexpr std::uint32_t kAimLayers = 0x22006;

		struct AimResult
		{
			bool valid = false;
			bool snapped = false;
			float x = 0, y = 0, z = 0;
			float dist = 0, rise = 0, snapDist = 0;
			ULONGLONG at = 0;
		};
		std::mutex g_aimLock;
		AimResult g_aim;
		std::atomic<bool> g_aimWanted{ false };

		bool GroundAt(std::uintptr_t tes, float x, float y, float zHint, float& out)
		{
			const float p[3] = { x, y, zHint };
			const float g = EngineFn<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(addr::kGroundRayCast)(tes, p, kAimLayers, 600.0f, 1200.0f);
			if (!(g > -1e30f) || g == -500.0f) {
				return false;
			}
			out = g;
			return true;
		}

		// Highest minus lowest ground on a ring of 8 samples plus the centre; false when a sample finds no ground.
		bool PadRise(std::uintptr_t tes, float x, float y, float z, float& rise, float& centre)
		{
			float lo = 1e30f, hi = -1e30f;
			if (!GroundAt(tes, x, y, z, centre)) {
				return false;
			}
			lo = hi = centre;
			for (int i = 0; i < 8; ++i) {
				const float a = float(i) * 0.785398f;
				float g = 0;
				if (!GroundAt(tes, x + std::cos(a) * kPadRadius, y + std::sin(a) * kPadRadius, z, g)) {
					return false;
				}
				lo = std::min(lo, g);
				hi = std::max(hi, g);
			}
			rise = hi - lo;
			return true;
		}

		void ComputeAim(std::uintptr_t tes, std::uintptr_t player)
		{
			AimResult r;
			r.at = GetTickCount64();
			const auto p = At<game::Vec3>(player, off::kRefPosition);
			const auto rot = At<game::Vec3>(player, off::kRefRotation);
			const float cp = std::cos(rot.x), sp = std::sin(rot.x);
			const float dx = std::sin(rot.z) * cp, dy = std::cos(rot.z) * cp, dz = -sp;  // angle x > 0 looks down
			const float ex = p.x, ey = p.y, ez = p.z + 1.6f;
			float prev = 0.0f;
			float hitT = -1.0f;
			for (float t = kAimStep; t <= kAimRange; t += kAimStep) {
				float g = 0;
				const float qz = ez + dz * t;
				if (GroundAt(tes, ex + dx * t, ey + dy * t, qz, g) && qz <= g) {
					float lo = prev, hi = t;
					for (int i = 0; i < 6; ++i) {
						const float m = 0.5f * (lo + hi);
						float gm = 0;
						if (GroundAt(tes, ex + dx * m, ey + dy * m, ez + dz * m, gm) && ez + dz * m <= gm) {
							hi = m;
						}
						else {
							lo = m;
						}
					}
					hitT = hi;
					break;
				}
				prev = t;
			}
			static ULONGLONG lastPitchLog = 0;
			if (hitT <= 0.0f && GetTickCount64() - lastPitchLog > 2000) {
				lastPitchLog = GetTickCount64();
				logger::Write("Summon aim: no ground along the look ray (pitch %.3f rad, yaw %.3f rad, eye z %.1f)", rot.x, rot.z, ez);
			}
			if (hitT > 0.0f) {
				const float hx = ex + dx * hitT, hy = ey + dy * hitT;
				float rise = 0, gz = 0;
				r.dist = std::hypot(hx - p.x, hy - p.y);
				if (PadRise(tes, hx, hy, ez + dz * hitT, rise, gz) && rise <= kPadMaxRise) {
					r = { true, false, hx, hy, gz, r.dist, rise, 0.0f, r.at };
				}
				else {
					// Nearest flat spot on rings around the aimed point.
					for (float rad = 10.0f; rad <= kSnapRange && !r.valid; rad += 10.0f) {
						for (int i = 0; i < 12 && !r.valid; ++i) {
							const float a = float(i) * 0.523599f;
							const float cx = hx + std::cos(a) * rad, cy = hy + std::sin(a) * rad;
							if (PadRise(tes, cx, cy, ez + dz * hitT, rise, gz) && rise <= kPadMaxRise) {
								r = { true, true, cx, cy, gz, std::hypot(cx - p.x, cy - p.y), rise, rad, r.at };
							}
						}
					}
				}
			}
			std::lock_guard lk(g_aimLock);
			g_aim = r;
		}

		// ---- In-world beacon where the hidden ship is --------------------------------------------------------------
		// The outpost beacon model (default object OutpostBeaconObject_DO), placed where the ship stands while it is
		// hidden (parked out of sight), moved when the ship is parked elsewhere, disabled while the ship is shown.
		// Disabled around every save (BeforeSave / AfterSave) so a save never holds it.
		std::uint32_t g_beaconHandle = 0;
		bool g_beaconShown = false;
		std::atomic<bool> g_saving{ false };

		std::uintptr_t RefFromHandle(std::uint32_t handle);

		std::uintptr_t PlaceRef(std::uintptr_t base, std::uintptr_t cell, std::uintptr_t ws, const float pos[3], float yaw, std::uint32_t& handleOut)
		{
			const auto dataHandler = Ptr(addr::kDataHandlerPtr);
			if (!ws || !base || !dataHandler) {
				return 0;
			}
			alignas(16) std::uint8_t d[0xC0] = {};
			const auto dp = reinterpret_cast<std::uintptr_t>(d);
			At<std::uintptr_t>(dp, 0x00) = game::Base() + addr::kNewRefrDataVtbl;
			At<float>(dp, 0x10) = pos[0];
			At<float>(dp, 0x14) = pos[1];
			At<float>(dp, 0x18) = pos[2];
			At<float>(dp, 0x24) = yaw;
			At<float>(dp, 0x28) = 1.0f;
			At<std::uintptr_t>(dp, 0x30) = base;
			At<std::uintptr_t>(dp, 0x38) = cell;
			At<std::uintptr_t>(dp, 0x40) = ws;
			At<std::uint8_t>(dp, 0x8C) = 1;
			At<std::uint8_t>(dp, 0x8D) = 1;
			At<std::uint32_t>(dp, 0x98) = 1;
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			EngineFn<void* (*)(std::uintptr_t, std::uint64_t*, void*)>(addr::kCreateReference)(dataHandler, &handle, d);
			handleOut = static_cast<std::uint32_t>(handle);
			return RefFromHandle(handleOut);
		}

		constexpr float kBeaconFarMetres = 400.0f;  // the ship's beacon shows beyond this distance (it is in view nearer)
		void BeaconTick(std::uintptr_t player, bool summoning)
		{
			static ULONGLONG last = 0;
			if (GetTickCount64() - last < 500 || g_saving.load()) {
				return;
			}
			last = GetTickCount64();
			std::uintptr_t ship = 0;
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
			bool want = false;
			game::Vec3 sp{};
			const auto pcell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto pws = pcell ? At<std::uintptr_t>(pcell, off::kCellWorldspace) : 0;
			if (ship) {
				const bool hidden = (At<std::uint32_t>(ship, 0x20) & 0x800) != 0;
				const auto scell = At<std::uintptr_t>(ship, off::kRefParentCell);
				const auto sws = scell ? At<std::uintptr_t>(scell, off::kCellWorldspace) : 0;
				sp = At<game::Vec3>(ship, off::kRefPosition);
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				const float distance = std::hypot(sp.x - pp.x, sp.y - pp.y);
				const auto loaded = At<std::uintptr_t>(ship, 0xB8);
				const bool has3D = loaded && At<std::uintptr_t>(loaded, 0x08);
				// A landed ship commonly has a ship cell with no worldspace parent. Its position is
				// trustworthy while its 3D is loaded nearby. Farther away the globe marker uses
				// the canonical recorded lat/long; placing a beacon into the player's cell there
				// would create a false local target.
				want = !hidden && !summoning && pws && (sws == pws || !sws) && has3D &&
					std::isfinite(distance) && distance > kBeaconFarMetres && distance < 3000.0f;
				static ULONGLONG lastStateLog = 0;
				if (!want && GetTickCount64() - lastStateLog > 10000) {
					lastStateLog = GetTickCount64();
					logger::Write("Ship beacon: unavailable (hidden %d, summon %d, player world %p, ship world %p, 3D %d, distance %.0f m); globe uses recorded planet position",
						int(hidden), int(summoning), reinterpret_cast<void*>(pws), reinterpret_cast<void*>(sws), int(has3D), distance);
				}
				EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
			}
			auto beacon = RefFromHandle(g_beaconHandle);
			struct Rel { std::uintptr_t r; ~Rel() { if (r) EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(r); } } rel{ beacon };
			if (want) {
				const float pos[3] = { sp.x, sp.y, sp.z };
				const float rot[3] = { 0.0f, 0.0f, 0.0f };
				if (!beacon) {
					const auto base = Ptr(addr::kBeaconBaseObject);
					beacon = PlaceRef(base, pcell, pws, pos, 0.0f, g_beaconHandle);
					rel.r = beacon;
					logger::Write("Ship beacon: %s at the loaded ship (%.0f, %.0f, %.0f)", beacon ? "placed" : "NOT placed (no beacon form)", sp.x, sp.y, sp.z);
					g_beaconShown = beacon != 0;
					return;
				}
				const auto bp = At<game::Vec3>(beacon, off::kRefPosition);
				if (std::hypot(bp.x - sp.x, bp.y - sp.y) > 5.0f) {
					std::uint64_t h = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
					float p2[3] = { sp.x, sp.y, sp.z };
					float r2[3] = { 0.0f, 0.0f, 0.0f };
					EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(beacon, &h, pcell, pws, p2, r2);
					logger::Write("Ship beacon: moved to the hidden ship (%.0f, %.0f, %.0f)", sp.x, sp.y, sp.z);
				}
				if (!g_beaconShown) {
					EngineFn<void (*)(std::uintptr_t)>(addr::kEnableReference)(beacon);
					g_beaconShown = true;
				}
				(void)rot;
			}
			else if (beacon && g_beaconShown) {
				EngineFn<void (*)(std::uintptr_t, char)>(addr::kDisableReference)(beacon, 0);
				g_beaconShown = false;
				logger::Write("Ship beacon: hidden (the ship is shown, summoning, or in another world)");
			}
		}

		// ---- Summon ------------------------------------------------------------------------------------------------
		// Hold F6: aim at the ground (notices show the distance and whether the pad is flat). Release: the ship comes
		// to that spot. As often as wanted (no limits); F6 again while it is inbound retargets it. The landing is the one
		// the game uses when a script enables a ship linked to a landing marker (the Call Your Ship way): marker at
		// the spot, SetLinkedRef(ship, marker, SpaceshipEnabledLandingLink) (FUN_140BA0D40), ship moved to the marker,
		// then the Papyrus Enable core FUN_141F16D20 (it finds the landing link and lands the ship; the old path called
		// the landing step and FUN_140B4A9C0 directly and the ship stayed hidden, 0.32.x).
		void ShowAim()
		{
			AimResult a;
			{
				std::lock_guard lk(g_aimLock);
				a = g_aim;
			}
			static ULONGLONG last = 0;
			if (GetTickCount64() - last < 400) {
				return;
			}
			last = GetTickCount64();
			if (!a.at) {
				return;
			}
			if (!a.valid) {
				notify::Show("Ship landing: aim at open ground within %.0f m", kAimRange);
			}
			else if (a.snapped) {
				notify::Show("Ship landing: %.0f m (too steep there: nearest flat ground %.0f m away)", a.dist, a.snapDist);
			}
			else {
				notify::Show("Ship landing: %.0f m, flat ground - release F6 to call the ship", a.dist);
			}
		}

		void LinkAndPlace(std::uintptr_t ship, std::uintptr_t marker, std::uintptr_t player)
		{
			const auto kw = Ptr(addr::kLandingLinkKeyword);
			const bool kwOk = kw && At<std::uint8_t>(kw, 0x2E) == 4;
			// Linked twice: with SpaceshipEnabledLandingLink (plain Enable lands there, the Call Your Ship way) and with no
			// keyword, which EnableWithLanding (kind 3) requires; the marker carries SpaceshipLandingMarkerKeyword.
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, char)>(addr::kSetLinkedRef)(ship, marker, kwOk ? kw : 0, 1);
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, char)>(addr::kSetLinkedRef)(ship, marker, 0, 1);
			const auto cell = At<std::uintptr_t>(player, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			float pos[3] = { summon.x, summon.y, summon.z };
			float rot[3] = { 0.0f, 0.0f, summon.yaw };
			std::uint64_t h = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(ship, &h, cell, ws, pos, rot);
			logger::Write("Summon: ship linked to the landing marker (keyword %s) and moved to (%.0f, %.0f, %.0f)", kwOk ? "SpaceshipEnabledLandingLink" : "MISSING", pos[0], pos[1], pos[2]);
		}

		// Returns true while a summon is in progress.
		bool RunSummon(std::uintptr_t player, bool outdoors, bool focused)
		{
			// 0.59.0 (user): the ship is called with the Call Your Ship mod (Rysel), exactly as it is: F6 runs its
			// `cgf "CallYourShip.Here"` through the console executor, in place of its item. Its script does everything
			// (inside / registered / too close / landed checks, TakeOff, landing marker 30 m in front, ETA notices,
			// MoveTo + SpaceshipEnabledLandingLink + Enable after 45 s, door unlocked). Our own summon below is unused.
			(void)player;
			(void)outdoors;
			const bool f6 = focused && Held(VK_F6);
			if (f6 && !summon.keyDown) {
				const auto console = Ptr(addr::kConsoleManagerPtr);
				if (console) {
					EngineFn<void (*)(std::uintptr_t, const char*)>(addr::kConsoleExecute)(console, "cgf \"CallYourShip.Here\"");
					logger::Write("Summon: F6 -> cgf \"CallYourShip.Here\" (Call Your Ship)");
				}
				else {
					logger::Write("Summon: F6 pressed but the console manager is not available");
				}
			}
			summon.keyDown = f6;
			return false;
		}

		bool RunOwnSummon(std::uintptr_t player, bool outdoors, bool focused)
		{
			const bool down = focused && Held(VK_F6) && outdoors;
			const bool released = !down && summon.keyDown;
			if (focused && Held(VK_F6) && !outdoors && !summon.keyDown) {
				notify::Show("Summon ship: go outside first");
			}
			summon.keyDown = focused && Held(VK_F6);
			g_aimWanted.store(down);
			if (down) {
				ShowAim();
			}
			if (released) {
				AimResult a;
				{
					std::lock_guard lk(g_aimLock);
					a = g_aim;
				}
				if (!a.valid || GetTickCount64() - a.at > 1500) {
					notify::Show("Summon ship: no landing spot (aim at open ground within %.0f m)", kAimRange);
				}
				else {
					const auto p = At<game::Vec3>(player, off::kRefPosition);
					const float yaw = std::atan2(p.x - a.x, p.y - a.y);  // the ship faces the player
					const bool retarget = summon.stage == 1 || summon.stage == 4 || summon.stage == 2 || summon.stage == 6;
					summon.x = a.x;
					summon.y = a.y;
					summon.z = a.z;
					summon.yaw = yaw;
					logger::Write("Summon: %s at (%.0f, %.0f, %.0f), %.0f m away%s", retarget ? "retarget" : "called", a.x, a.y, a.z, a.dist,
						a.snapped ? " (moved to flat ground)" : "");
					telemetry::Record("ship_summon_call", "\"pos\":[%.0f,%.0f,%.0f],\"dist\":%.0f,\"snapped\":%d,\"retarget\":%d", a.x, a.y, a.z, a.dist, a.snapped ? 1 : 0, retarget ? 1 : 0);
					if (retarget) {
						if (summon.stage == 6) {
							// Already linked and waiting: move the marker and the ship, a few seconds more.
							if (const auto marker = RefFromHandle(summon.markerHandle)) {
								std::uintptr_t ship = 0;
								EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
								PlaceMarker(player);
								if (ship) {
									LinkAndPlace(ship, marker, player);
									EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(ship);
								}
								EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(marker);
							}
							summon.etaAt = std::max(summon.etaAt, GetTickCount64() + 4000);
						}
						notify::Show("Ship rerouted: new landing point %.0f m away", a.dist);
					}
					else {
						summon = Summon{ 1, GetTickCount64(), a.x, a.y, a.z, yaw, summon.keyDown, summon.markerHandle };
						notify::Show("Calling your ship: landing point %.0f m away", a.dist);
						return true;
					}
				}
			}
			if (summon.stage == 0) {
				return false;
			}
			std::uintptr_t ship = 0;
			EngineFn<void (*)(std::uintptr_t, std::uintptr_t*)>(addr::kGetPlayerHomeShip)(player, &ship);
			if (!ship) {
				summon.stage = 0;
				notify::Show("Summon ship: no home ship");
				return false;
			}
			struct ReleaseShip { std::uintptr_t s; ~ReleaseShip() { EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(s); } } rel{ ship };
			const ULONGLONG t = GetTickCount64() - summon.since;
			const bool disabled = (At<std::uint32_t>(ship, 0x20) & 0x800) != 0;
			if (summon.stage == 1) {
				const auto sp = At<game::Vec3>(ship, off::kRefPosition);
				const auto pp = At<game::Vec3>(player, off::kRefPosition);
				const double dist = std::hypot(sp.x - pp.x, sp.y - pp.y);
				summon.fromX = sp.x;
				summon.fromY = sp.y;
				const bool aboard = EngineFn<std::uintptr_t (*)(std::uintptr_t, char, std::uintptr_t, std::uintptr_t)>(addr::kShipOfRef)(player, 1, 0, 0) == ship;
				const bool piloting = EngineFn<std::uintptr_t (*)(std::uintptr_t)>(addr::kShipPilotActor)(ship) == player;
				if (!disabled && dist < 4000.0 && !aboard && !piloting) {
					EngineFn<void (*)(std::uintptr_t)>(addr::kShipTakeOff)(ship);  // in view: the NPC take-off first
					logger::Write("Summon: ship taking off from (%.0f, %.0f, %.0f), %.0f m away", sp.x, sp.y, sp.z, dist);
					notify::Show("Your ship is taking off...");
					summon.stage = 4;
					summon.since = GetTickCount64();
					return true;
				}
				if (!disabled) {
					EngineFn<void (*)(std::uintptr_t, bool)>(addr::kDisableReference)(ship, false);
				}
				summon.stage = 2;
				summon.since = GetTickCount64();
				return true;
			}
			if (summon.stage == 4) {
				if (t < kTakeOffMs && !disabled) {
					return true;
				}
				if (!disabled) {
					EngineFn<void (*)(std::uintptr_t, bool)>(addr::kDisableReference)(ship, false);
				}
				summon.stage = 2;
				summon.since = GetTickCount64();
				return true;
			}
			if (summon.stage == 2) {
				if (t < 500) {
					return true;  // let the disable take the ship's 3D away
				}
				const auto marker = PlaceMarker(player);
				if (!marker) {
					summon.stage = 0;
					notify::Show("Summon ship: could not place a landing marker");
					logger::Write("Summon: landing marker not placed");
					return false;
				}
				LinkAndPlace(ship, marker, player);
				EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(marker);
				const double from = std::hypot(summon.fromX - summon.x, summon.fromY - summon.y);
				const double eta = std::clamp(6.0 + from / 250.0, 6.0, 40.0);
				summon.etaAt = GetTickCount64() + ULONGLONG(eta * 1000.0);
				summon.lastNotice = GetTickCount64();
				notify::Show("Ship inbound: arriving in %.0f s", eta);
				logger::Write("Summon: inbound from %.0f m away, ETA %.0f s", from, eta);
				summon.stage = 6;
				summon.since = GetTickCount64();
				return true;
			}
			if (summon.stage == 6) {
				const ULONGLONG now = GetTickCount64();
				if (now < summon.etaAt) {
					const auto left = (summon.etaAt - now + 999) / 1000;
					if (now - summon.lastNotice >= 10000 || (left == 5 && now - summon.lastNotice >= 2000)) {
						summon.lastNotice = now;
						notify::Show("Ship inbound: %llu s", static_cast<unsigned long long>(left));
					}
					return true;
				}
				summon.stage = 3;
				return true;
			}
			if (summon.stage == 3) {
				// The Papyrus Enable core (EnableNoWait form: not latent, no script VM; the VM pointer is only read by its
				// error messages): with the ship linked to a landing marker it plays the landing.
				std::uintptr_t noVm = 0;
				char result = 0;
				// Kind 3 = EnableWithLanding, the descent NPC ships make (0.45.0 used kind 0: the ship appeared on the pad).
				const auto ok = EngineFn<char (*)(std::uintptr_t, char, char, std::uintptr_t*, std::uint32_t, char*)>(addr::kEnableCore)(ship, 3, 0, &noVm, 0, &result);
				const auto mk = RefFromHandle(summon.markerHandle);
				const auto kwm = Ptr(addr::kLandingMarkerKeyword);
				logger::Write("Summon: EnableWithLanding -> %d (out %d); marker %s, landing-marker keyword %s", int(ok), int(result), mk ? "present" : "missing",
					kwm ? "found" : "missing");
				if (mk) {
					EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(mk);
				}
				parking::Clear();
				notify::Show("Your ship is landing - stand clear");
				summon.stage = 5;
				summon.since = GetTickCount64();
				return true;
			}
			// stage 5: log the home ship while it lands (shown/hidden, model, position).
			static ULONGLONG lastWatch = 0;
			if (GetTickCount64() - lastWatch >= 2000) {
				lastWatch = GetTickCount64();
				const auto loaded = At<std::uintptr_t>(ship, 0xB8);
				const std::uintptr_t model = loaded ? At<std::uintptr_t>(loaded, 0x08) : 0;
				const auto sp = At<game::Vec3>(ship, off::kRefPosition);
				logger::Write("Summon: home ship %08X %s, model %s, at (%.0f, %.0f, %.0f)", At<std::uint32_t>(ship, 0x28),
					disabled ? "hidden" : "shown", model ? "loaded" : "none", sp.x, sp.y, sp.z);
			}
			if (t > 20000) {
				summon.stage = 0;
				return false;
			}
			return true;
		}
	}

	void SyncTick(std::uintptr_t tes)
	{
		static ULONGLONG last = 0;
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!tes || !player || !g_aimWanted.load() || GetTickCount64() - last < 150) {
			return;
		}
		last = GetTickCount64();
		ComputeAim(tes, player);
	}

	void BeforeSave()
	{
		g_saving.store(true);
		if (const auto beacon = RefFromHandle(g_beaconHandle)) {
			if (g_beaconShown) {
				EngineFn<void (*)(std::uintptr_t, char)>(addr::kDisableReference)(beacon, 0);
				g_beaconShown = false;
			}
			EngineFn<void (*)(std::uintptr_t)>(addr::kRefRelease)(beacon);
		}
	}

	void AfterSave() { g_saving.store(false); }

	bool Busy() { return g.active || g.landing || launch.stage != 0 || summon.stage != 0; }


	void Tick()
	{
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) {
			return;
		}
		const bool focused = Focused();
		const bool f7 = focused && Held(VK_F7);
		const bool pressed = f7 && !g.keyDown;
		g.keyDown = f7;

		const auto a = anchor::Current();
		const bool outdoors = a.ref == player;  // on foot outdoors
		if (!g.active) {
			if (RunLaunch(player, outdoors)) {
				return;
			}
			if (g.active) {
				guard.wasOutdoors = true;  // launched: this arrival is ours, not a load for the guard
				return;
			}
			if (pressed && shipflight::Active()) {
				shipflight::Stop("F7");
				return;
			}
			if (pressed && !outdoors) {
				if (const auto ship = SeatedShip(player)) {
					// Ship flight is removed (user, 2026-10-14): flight moves to vehicles. The flight model, terrain assist
					// and tuning stay for that. (BeginLaunch is kept for reference.)
					(void)ship;
					notify::Show("Ship flight is off: planet flight is moving to vehicles");
					return;
				}
			}
			BeaconTick(player, summon.stage != 0);
			if (RunSummon(player, outdoors, focused)) {
				return;
			}
			const bool wasHolding = guard.holding;
			RunGuard(player, outdoors, pressed);
			if (guard.holding || wasHolding) {
				return;  // (an F7 that cancelled the guard does not also start flight)
			}
			if (pressed) {
				notify::Show("Flight: sit in your ship's pilot seat and press F7 to fly");
			}
			return;
		}
		if (!outdoors) {
			Stop("not outdoors");
			return;
		}

		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		const double dt = std::clamp(double(now.QuadPart - g.last.QuadPart) / double(freq.QuadPart), 0.0, 0.1);
		g.last = now;

		auto& m = g.model;
		// Resync if something else moved the player (zone switch, teleport, load).
		const auto actual = At<game::Vec3>(player, off::kRefPosition);
		if (std::hypot(actual.x - m.s.x, actual.y - m.s.y) > 200.0) {
			m.Reset(actual.x, actual.y, actual.z, m.s.yaw, m.s.speed);
			g.asked.clear();
		}

		// Landing (F7) and drop-out (Shift+F7): the ground is the hull centre's last sample.
		const bool shift = focused && Held(VK_SHIFT);
		const double groundHere = g.elevation >= 0 ? m.s.z - g.elevation : m.s.z;
		if (pressed) {
			if (shift) {
				// Dropping out still lands the ship under you: it is never left behind where the flight began.
				Stop("dropped out");
				BringShip(player, groundHere);
				return;
			}
			if (g.landing) {
				g.landing = false;
				notify::Show("Landing cancelled");
			}
			else if (m.s.speed < kLandSpeed) {
				g.landing = true;  // the minimum distance is off; the ship settles straight down and lands
				notify::Show("Landing... (F7 to cancel)");
				logger::Write("Flight: landing from %.0f m", g.elevation);
			}
			else {
				notify::Show("Slow down to land (under %.0f m/s; S to slow)", kLandSpeed);
			}
		}

		KeepThirdPerson();
		camtune::Tick();
		flighttune::Tick(m.p);
		// The real ship parks out of sight while flying (once the hull look is copied, or after 3 s).
		if (!g.shipHidden && (shipmodel::Ready() || GetTickCount64() - g.startMs > 3000)) {
			g.shipHidden = true;
			rebase::HideShipNow("flying");
		}

		// Ground samples asked last tick (answered in the game tick in between; one frame late, as the model expects).
		if (!g.asked.empty()) {
			std::vector<fm::Result> res(g.asked.size());
			for (std::size_t i = 0; i < g.asked.size(); ++i) {
				float h = -3.4e38f;
				unsigned age = 0;
				if (prefetch::LatestGround(prefetch::kSlotModel0 + int(i), h, age) && age <= kProbeMaxAge && h > -1e30f) {
					res[i].valid = true;
					res[i].h = h;
				}
			}
			m.Feed(g.asked, res);
		}

		// Control vs free look (middle mouse): in control the mouse flies the ship and the camera stays behind it.
		const bool toggle = focused && Held(VK_MBUTTON);
		if (toggle && !g.toggleDown) {
			g.freeLook = !g.freeLook;
			notify::Show(g.freeLook ? "Free look (middle mouse: back to flying)" : "Flight control (middle mouse: free look)");
		}
		g.toggleDown = toggle;
		CaptureMouse(!g.freeLook);
		// Input: the mouse counts captured from the look handler since the last tick.
		double mx = 0, my = 0;
		TakeMouse(mx, my);
		fm::Input in;
		if (focused) {
			in.mouseDx = g.freeLook ? 0.0 : mx * kMouseScale;
			in.mouseDy = g.freeLook ? 0.0 : my * kMouseScale;
			in.strafe = (Held('D') ? 1.0 : 0.0) - (Held('A') ? 1.0 : 0.0);         // slide, nose unchanged
			in.lift = (Held(VK_SPACE) ? 1.0 : 0.0) - (Held(VK_CONTROL) ? 1.0 : 0.0);  // straight up / down
			in.faster = Held('W');
			in.slower = Held('S');
		}
		if (g.landing) {
			in.landing = true;
			in.faster = false;
			in.slower = true;
			in.lift = 0;
		}
		// Near the edge of the physics box with a re-centre overdue: ease to a hover so the world can settle and
		// re-centre (it needs a calm moment); flight resumes as normal afterwards.
		const double vMax = m.p.vMax;
		const float edge = handoff::PhysicsHalfExtent() * 0.92f;
		if (recentre::Pending() && (std::fabs(m.s.x) > edge || std::fabs(m.s.y) > edge)) {
			m.p.vMax = 0.0;
			if (GetTickCount64() - g.lastHud > 2000) {
				g.lastHud = GetTickCount64();
				notify::Log("Holding position: re-centring the world");
			}
		}
		// No stall on a launch from a hover until the ship has flown faster than stall speed once.
		const double vStall = m.p.vStall;
		if (!g.everFast) {
			m.p.vStall = 0.0;
		}
		m.Step(dt, in);
		m.p.vStall = vStall;
		m.p.vMax = vMax;
		g.everFast = g.everFast || m.s.speed > vStall;

		// The next ground samples (hull points and the look-ahead along the path).
		const auto& reqs = m.Requests();
		g.asked.assign(reqs.begin(), reqs.begin() + std::min<std::size_t>(reqs.size(), prefetch::kSlotModelCount));
		for (std::size_t i = 0; i < g.asked.size(); ++i) {
			// The ray returns the hit nearest the query height (FUN_140583cb0): query from high up to get the highest surface.
			prefetch::RequestGround(prefetch::kSlotModel0 + int(i), float(g.asked[i].x), float(g.asked[i].y), float(m.s.z) + kProbeAbove, 0.0f, kProbeLength);
		}

		// Outputs: position; the camera follows the ship's heading and pitch; the hull shows pitch and roll.
		g.x = m.s.x;
		g.y = m.s.y;
		g.z = m.s.z;
		g.vx = m.s.vx;
		g.vy = m.s.vy;
		g.vz = m.s.vz;
		g.throttle = m.s.throttle;
		g.elevation = m.s.elevation;
		const float ground = g.elevation >= 0 ? float(m.s.z - g.elevation) : -3.4e38f;
		// The camera is never driven by the ship's rotation (user): the player's rotation is not written; the heading,
		// pitch and bank are the hull's own rotation (shipmodel), relative to wherever the player faces.
		shipmodel::SetHeading(float(m.s.yaw));
		shipmodel::SetAttitude(float(m.s.pitch), float(m.s.roll));
		PlacePlayer(player, g.x, g.y, g.z);

		// Landing touchdown: on the hull floor and nearly still: out of flight, standing on the ground, ship beside.
		if (g.landing && g.elevation >= 0 && g.elevation <= m.p.clearance + 1.0 && m.s.speed < 5.0) {
			const double groundZ = m.s.z - g.elevation;
			g.landing = false;
			Stop("landed");
			PlacePlayer(player, g.x, g.y, groundZ + 0.3);
			BringShip(player, groundZ);
			return;
		}

		const ULONGLONG ms = GetTickCount64();

		// Autoland: low and slow for a few seconds lands by itself, with a countdown.
		const double hspeed = std::hypot(g.vx, g.vy);
		const bool lowSlow = m.p.minClearance <= 0 && g.elevation >= 0 && g.elevation < kAutoLandHeight && m.s.speed < kAutoLandSpeed && g.vz <= 1.0;
		(void)hspeed;
		if (lowSlow) {
			if (g.lowSince == 0) {
				g.lowSince = ms;
				g.lastCountdown = -1;
			}
			const int left = kAutoLandSeconds - int((ms - g.lowSince) / 1000);
			if (left <= 0) {
				notify::Show("Auto-landing");
				telemetry::Record("flight_autoland", "\"elev\":%.1f", g.elevation);
				Stop("auto-landed");
				BringShip(player, ground > -1e30f ? ground : g.z);
				return;
			}
			if (left != g.lastCountdown) {
				g.lastCountdown = left;
				notify::Show("Auto-landing in %d... (climb or speed up to cancel)", left);
				g.lastHud = ms;  // keep the countdown readable
			}
		}
		else if (g.lowSince != 0) {
			g.lowSince = 0;
			if (g.lastCountdown > 0) {
				notify::Show("Auto-landing cancelled");
			}
			g.lastCountdown = -1;
		}
		if (m.s.braking && ms - g.lastHud >= 1500) {
			g.lastHud = ms;
			notify::Show("BRAKING: terrain ahead (%.0f m)", m.s.obstacle);
		}
		if (m.s.mode != g.lastMode && ms - g.lastModeMs >= 1500) {
			g.lastMode = m.s.mode;
			g.lastModeMs = ms;
			if (m.s.mode != fm::Mode::Cruise) {
				notify::Log("%s", fm::ModeName(m.s.mode));
			}
			logger::Write("Flight: mode %s (speed %.0f, elev %.0f, time to impact %.1f s)", fm::ModeName(m.s.mode), m.s.speed, g.elevation,
				m.s.ttc < 1e8 ? m.s.ttc : -1.0);
		}
		if (ms - g.lastSample >= 250) {
			g.lastSample = ms;
			telemetry::Record("flight", "\"pos\":[%.0f,%.0f,%.0f],\"speed\":%.1f,\"throttle\":%.2f,\"elev\":%.1f,\"mode\":\"%s\",\"ttc\":%.1f,"
				"\"cap\":%.0f,\"pitch\":%.2f,\"roll\":%.2f,\"yaw\":%.2f,\"clamps\":%d",
				g.x, g.y, g.z, m.s.speed, g.throttle, g.elevation, fm::ModeName(m.s.mode), m.s.ttc < 1e8 ? m.s.ttc : -1.0,
				m.s.speedCap < 1e8 ? m.s.speedCap : -1.0, m.s.pitch, m.s.roll, m.s.yaw, m.s.clampEvents);
		}
		if (ms - g.lastHud >= 2000) {
			g.lastHud = ms;
			const double hs = std::hypot(g.vx, g.vy);
			(void)hs;
			if (g.elevation >= 0) {
				notify::Log("FLIGHT %.0f m/s | ELEV %.0f m | throttle %d%% | %s%s", m.s.speed, g.elevation, int(g.throttle * 100 + 0.5),
					fm::ModeName(m.s.mode), (g.elevation < kLandHeight && m.s.speed < kLandSpeed) ? " | F7 to land" : "");
			}
			else {
				notify::Log("FLIGHT %.0f m/s | ELEV ? (ground loading) | throttle %d%% | %s", m.s.speed, int(g.throttle * 100 + 0.5), fm::ModeName(m.s.mode));
			}
		}
	}
}
