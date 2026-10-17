#include "shipflight.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "prefetch.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace shipflight
{
	namespace
	{
		// Flight laws (same feel as the player flight).
		constexpr double kCruise = 154.0, kBoost = 308.0;  // m/s
		constexpr double kAccel = 30.0;                    // m/s^2
		constexpr double kThrottleRate = 0.6;              // per second while W/S is held
		constexpr double kClimb = 40.0;                    // m/s from Space / Ctrl
		constexpr double kTurnPerLook = 0.02;              // radians of yaw per unit of mouse look
		constexpr double kClearance = 1.0;                 // metres above the ground
		constexpr ULONGLONG kSyncMs = 1000;                // full move routine (cell, collision, physics) this often
		constexpr ULONGLONG kLogMs = 1000;

		// NiAVObject / NiNode (see the engine reference).
		constexpr std::uint32_t kNodeLocal = 0x40;    // rotation 3x4 floats, translate +0x30
		constexpr std::uint32_t kVUpdateDownward = 0x248;
		constexpr std::uint32_t kUpdateSkipPhysics = 0x800;  // NiUpdateData +0x38: do not take transforms from collision

		struct State
		{
			bool active = false;
			std::uintptr_t ship = 0;
			std::uint32_t shipId = 0;
			double x = 0, y = 0, z = 0, yaw = 0;
			double vx = 0, vy = 0, vz = 0, throttle = 0;
			double startZ = 0, ground = -1e30;
			double syncedX = 0, syncedY = 0, syncedZ = 0, syncedYaw = 0;  // what the last full move set
			float rot0[12]{};          // the 3D root's local rotation at the last full move
			float t0[3]{};             // and its local translation
			int yawSign = 0;           // +1/-1 once calibrated against the engine's own rotation
			LARGE_INTEGER last{};
			ULONGLONG since = 0, lastSync = 0, lastLog = 0;
			double maxSyncMs = 0;
			bool keyDown = false;
		} g;

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

		void Menu(std::uint32_t nameGetter, int mode)
		{
			const auto ui = Ptr(addr::kUIPtr);
			if (ui) {
				EngineFn<void (*)(std::uintptr_t, void*, int)>(addr::kShowMenu)(ui, EngineFn<void* (*)()>(nameGetter)(), mode);
			}
		}
		bool Landed(std::uintptr_t ship) { return EngineFn<char (*)(std::uintptr_t)>(addr::kShipIsLanded)(ship) != 0; }
		void SetLanded(std::uintptr_t ship, bool landed) { EngineFn<void (*)(std::uintptr_t, std::uint8_t)>(addr::kShipSetLanded)(ship, landed ? 1 : 0); }

		std::uintptr_t Root(std::uintptr_t ref)
		{
			const auto data = At<std::uintptr_t>(ref, off::kRefLoadedData);
			return data ? At<std::uintptr_t>(data, off::kLoadedData3D) : 0;
		}

		// The game's full move (position, 3D, collision, cell), timed.
		double FullMove(double x, double y, double z, double yaw)
		{
			const auto cell = At<std::uintptr_t>(g.ship, off::kRefParentCell);
			const auto ws = cell ? At<std::uintptr_t>(cell, off::kCellWorldspace) : 0;
			if (!cell || !ws) {
				return 0;
			}
			const auto rot = At<game::Vec3>(g.ship, off::kRefRotation);
			float p[3] = { float(x), float(y), float(z) };
			float r[3] = { rot.x, rot.y, float(yaw) };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			LARGE_INTEGER a, b, f;
			QueryPerformanceCounter(&a);
			EngineFn<void (*)(std::uintptr_t, std::uint64_t*, std::uintptr_t, std::uintptr_t, float*, float*)>(addr::kMoveReference)(g.ship, &handle, cell, ws, p, r);
			QueryPerformanceCounter(&b);
			QueryPerformanceFrequency(&f);
			return double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart);
		}

		// After a full move: remember what the engine set (the smooth steps start from there). The first time the
		// heading has changed, compare our rotation with the engine's to pick the turning direction.
		void Resync(double predictedYawDelta, const float* predicted)
		{
			const auto root = Root(g.ship);
			if (!root) {
				return;
			}
			const float* now = reinterpret_cast<const float*>(root + kNodeLocal);
			if (g.yawSign == 0 && std::fabs(predictedYawDelta) > 0.05 && predicted) {
				float d = 0;
				for (int i = 0; i < 12; ++i) {
					d += std::fabs(now[i] - predicted[i]);
				}
				// predicted used +1; if it does not match the engine, the convention is the other way.
				g.yawSign = d < 0.05f ? 1 : -1;
				logger::Write("Ship flight: turning direction calibrated (%s, difference %.3f)", g.yawSign > 0 ? "as predicted" : "flipped", d);
			}
			std::memcpy(g.rot0, now, sizeof(g.rot0));
			g.t0[0] = now[12];
			g.t0[1] = now[13];
			g.t0[2] = now[14];
			g.syncedX = At<game::Vec3>(g.ship, off::kRefPosition).x;
			g.syncedY = At<game::Vec3>(g.ship, off::kRefPosition).y;
			g.syncedZ = At<game::Vec3>(g.ship, off::kRefPosition).z;
			g.syncedYaw = At<game::Vec3>(g.ship, off::kRefRotation).z;
		}

		// Rotation about the vertical axis applied to rot0 (rows of 4 floats): out = Rz(a) * rot0.
		void RotateZ(const float* rot0, double a, float* out)
		{
			const float c = float(std::cos(a)), s = float(std::sin(a));
			std::memcpy(out, rot0, 12 * sizeof(float));
			for (int col = 0; col < 3; ++col) {
				const float r0 = rot0[0 * 4 + col], r1 = rot0[1 * 4 + col];
				out[0 * 4 + col] = c * r0 - s * r1;
				out[1 * 4 + col] = s * r0 + c * r1;
			}
		}

		// Smooth step: the ship's position/heading and its 3D, every frame (physics untouched until the next sync).
		void SmoothPlace()
		{
			At<float>(g.ship, off::kRefPosition) = float(g.x);
			At<float>(g.ship, off::kRefPosition + 4) = float(g.y);
			At<float>(g.ship, off::kRefPosition + 8) = float(g.z);
			At<float>(g.ship, off::kRefRotation + 8) = float(g.yaw);
			const auto root = Root(g.ship);
			if (!root) {
				return;
			}
			float* m = reinterpret_cast<float*>(root + kNodeLocal);
			const int sign = g.yawSign == 0 ? 1 : g.yawSign;
			RotateZ(g.rot0, sign * (g.yaw - g.syncedYaw), m);
			// Translate by how far we are from the last synced position (the root's parent is the cell node).
			m[12] = g.t0[0] + float(g.x - g.syncedX);
			m[13] = g.t0[1] + float(g.y - g.syncedY);
			m[14] = g.t0[2] + float(g.z - g.syncedZ);
			alignas(16) std::uint8_t data[0x80] = {};
			At<std::uint32_t>(reinterpret_cast<std::uintptr_t>(data), 0x38) = kUpdateSkipPhysics;
			reinterpret_cast<void (*)(std::uintptr_t, void*)>((*reinterpret_cast<void***>(root))[kVUpdateDownward / 8])(root, data);
		}
	}

	bool Active() { return g.active; }

	void Start(std::uintptr_t ship)
	{
		if (g.active || !ship) {
			return;
		}
		g = State{};
		g.ship = ship;
		g.shipId = At<std::uint32_t>(ship, 0x28);
		const auto pos = At<game::Vec3>(ship, off::kRefPosition);
		g.x = pos.x;
		g.y = pos.y;
		g.z = g.startZ = pos.z;
		g.yaw = At<game::Vec3>(ship, off::kRefRotation).z;
		const bool landedBefore = Landed(ship);
		Menu(addr::kTakeoffMenuName, 2);
		Menu(addr::kSpaceshipHudMenuName, 0);
		SetLanded(ship, false);
		prefetch::SetProbeAnchor(ship);
		Resync(0, nullptr);
		g.active = true;
		g.since = g.lastSync = g.lastLog = GetTickCount64();
		QueryPerformanceCounter(&g.last);
		logger::Write("Ship flight: started on ship %08X at (%.0f, %.0f, %.0f) yaw %.2f; landed before %d, after %d", g.shipId, g.x, g.y, g.z, g.yaw,
			landedBefore ? 1 : 0, Landed(ship) ? 1 : 0);
		telemetry::Record("shipflight_start", "\"ship\":\"%08X\"", g.shipId);
		notify::Show("Ship flight: W/S throttle, mouse turns, Space/Ctrl up/down, Shift boost. F7 lands where you are.");
	}

	void Stop(const char* why)
	{
		if (!g.active) {
			return;
		}
		double z = g.z;
		if (g.ground > -1e29) {
			z = g.ground;  // set it down on the ground under it
		}
		const double ms = FullMove(g.x, g.y, z, g.yaw);
		SetLanded(g.ship, true);
		Menu(addr::kSpaceshipHudMenuName, 2);
		Menu(addr::kTakeoffMenuName, 0);
		prefetch::SetProbeAnchor(0);
		logger::Write("Ship flight: stopped (%s) at (%.0f, %.0f, %.0f); last move %.1f ms, slowest move %.1f ms", why, g.x, g.y, z, ms, g.maxSyncMs);
		telemetry::Record("shipflight_stop", "\"why\":\"%s\",\"pos\":[%.0f,%.0f,%.0f],\"max_sync_ms\":%.1f", why, g.x, g.y, z, g.maxSyncMs);
		notify::Show("Ship flight: landed (%s)", why);
		g = State{};
	}

	void Tick()
	{
		if (!g.active) {
			return;
		}
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		std::uintptr_t subject = g.ship;
		float pilot = 0.0f;
		if (player) {
			EngineFn<char (*)(std::uintptr_t*, std::uintptr_t, std::uintptr_t, float*)>(addr::kCondGetActorInShipPilotSeat)(&subject, player, 0, &pilot);
		}
		if (pilot < 0.5f) {
			Stop("left the pilot seat");
			return;
		}
		LARGE_INTEGER now, freq;
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&freq);
		const double dt = std::clamp(double(now.QuadPart - g.last.QuadPart) / double(freq.QuadPart), 0.0, 0.1);
		g.last = now;

		const bool focused = Focused();
		const bool shift = focused && Held(VK_SHIFT);
		if (focused && Held('W')) g.throttle += kThrottleRate * dt;
		if (focused && Held('S')) g.throttle -= kThrottleRate * dt;
		g.throttle = std::clamp(g.throttle, 0.0, 1.0);
		const double climb = (focused && Held(VK_SPACE) ? kClimb : 0.0) - (focused && Held(VK_CONTROL) ? kClimb : 0.0);
		// Mouse look (the flight HUD delivers it to PlayerControls) turns the ship.
		const auto pc = Ptr(addr::kPlayerControlsPtr);
		const float lookX = pc ? At<float>(pc + off::kPlayerControlsData, 0xC) : 0.0f;
		g.yaw += lookX * kTurnPerLook;

		const double speed = g.throttle * (shift ? kBoost : kCruise);
		const double tx = std::sin(g.yaw) * speed, ty = std::cos(g.yaw) * speed;
		const double maxDv = kAccel * dt;
		const auto approach = [maxDv](double v, double t) { return v + std::clamp(t - v, -maxDv, maxDv); };
		g.vx = approach(g.vx, tx);
		g.vy = approach(g.vy, ty);
		g.vz = approach(g.vz, climb);
		double nx = g.x + g.vx * dt, ny = g.y + g.vy * dt, nz = g.z + g.vz * dt;

		// Ground: the engine ray cast (serviced while the grid updates around the ship); else the take-off height.
		prefetch::RequestGround(prefetch::kSlotShip, float(nx), float(ny), float(nz), 2000.0f, 6000.0f);
		float ground = -3.4e38f;
		unsigned age = 0;
		if (prefetch::LatestGround(prefetch::kSlotShip, ground, age) && age <= 30 && ground > -1e30f) {
			g.ground = ground;
		}
		const double floor = (g.ground > -1e29 ? g.ground : g.startZ) + kClearance;
		if (nz < floor) {
			nz = floor;
			g.vz = std::max(0.0, g.vz);
		}
		g.x = nx;
		g.y = ny;
		g.z = nz;

		const ULONGLONG ms = GetTickCount64();
		if (ms - g.lastSync >= kSyncMs) {
			g.lastSync = ms;
			const double predictedDelta = g.yaw - g.syncedYaw;
			float predicted[12];
			RotateZ(g.rot0, predictedDelta, predicted);
			const double took = FullMove(g.x, g.y, g.z, g.yaw);
			g.maxSyncMs = std::max(g.maxSyncMs, took);
			Resync(predictedDelta, predicted);
			if (took > 50.0) {
				logger::Write("Ship flight: full move took %.1f ms", took);
			}
		}
		else {
			SmoothPlace();
		}

		if (ms - g.lastLog >= kLogMs) {
			g.lastLog = ms;
			logger::Write("Ship flight: t=%.0fs pos (%.0f, %.0f, %.0f) yaw %.2f speed %.0f m/s throttle %.2f ground %s %.0f | ref (%.0f, %.0f, %.0f)",
				(ms - g.since) / 1000.0, g.x, g.y, g.z, g.yaw, std::hypot(g.vx, g.vy), g.throttle, g.ground > -1e29 ? "probe" : "start", g.ground > -1e29 ? g.ground : g.startZ,
				At<float>(g.ship, off::kRefPosition), At<float>(g.ship, off::kRefPosition + 4), At<float>(g.ship, off::kRefPosition + 8));
			telemetry::Record("shipflight", "\"pos\":[%.0f,%.0f,%.0f],\"yaw\":%.3f,\"speed\":%.1f,\"throttle\":%.2f,\"ground_probe\":%d", g.x, g.y, g.z, g.yaw,
				std::hypot(g.vx, g.vy), g.throttle, g.ground > -1e29 ? 1 : 0);
		}
	}

	void Reset()
	{
		prefetch::SetProbeAnchor(0);
		g = State{};
	}
}
