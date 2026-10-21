#include "vehicleaim.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "npcvehicles.h"
#include "vehicleflight.h"
#include "vehiclehits.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace vehicleaim
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }

		constexpr std::uint32_t kLaunch = 0x01B3BD50;
		// mov rax,rsp; mov [rax+18h],rbx; mov [rax+8],rcx; push rbp; push rsi; push rdi (14 bytes, no RIP-relative)
		const std::uint8_t kLaunchPrologue[14] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x48, 0x08, 0x55, 0x56, 0x57 };
		// ProjectileLaunchData (1.16.244): origin +0x00 (x y z floats, the ref position frame), angles +0x24 X (pitch,
		// nose down +), +0x28 Y (0 in every shot), +0x2C Z (heading, atan2(dx, dy)). Confirmed by the player's own rover
		// shots 2026-10-17 20:36-20:39: +0x2C = camera heading (2.260 / 2.237, -1.876 / -1.862), +0x24 = -camera pitch.
		// (Launch's log line prints +0x24 and +0x28 as "x" and "z"; that label is not the heading.) Shooter ref +0x38.
		constexpr std::uint32_t kOrigin = 0x00, kAngleX = 0x24, kAngleY = 0x28, kAngleZ = 0x2C, kShooter = 0x38;

		using Launch_t = void* (*)(void*, float*, std::uint64_t, std::uint64_t);
		Launch_t g_original = nullptr;
		std::atomic<unsigned> g_aimed{ 0 }, g_playerShots{ 0 };

		// The camera's forward, read like vehicleflight's CameraHeadings / CameraPitch (both readings, for the log).
		bool Camera(double& headCol, double& headRow, double& pitchRow, double& pitchCol)
		{
			const uptr cam = Ptr(addr::kPlayerCameraPtr);
			const uptr root = cam ? At<uptr>(cam, 0x08) : 0;
			if (!root) return false;
			headCol = std::atan2(At<float>(root, 0x84), At<float>(root, 0x94));
			headRow = std::atan2(At<float>(root, 0x90), At<float>(root, 0x94));
			pitchRow = std::asin(std::clamp(double(At<float>(root, 0x98)), -1.0, 1.0));
			pitchCol = std::asin(std::clamp(double(At<float>(root, 0xA4)), -1.0, 1.0));
			return true;
		}

		void Before(float* data)
		{
			const uptr shooter = *reinterpret_cast<uptr*>(reinterpret_cast<std::uint8_t*>(data) + kShooter);
			if (!shooter) return;
			const std::uint32_t id = At<std::uint32_t>(shooter, off::kFormId);
			float* pitch = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(data) + kAngleX);
			float* heading = reinterpret_cast<float*>(reinterpret_cast<std::uint8_t*>(data) + kAngleZ);
			const float roll = *reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(data) + kAngleY);
			const float* org = reinterpret_cast<const float*>(reinterpret_cast<std::uint8_t*>(data) + kOrigin);
			// The player's own rover: log only (ground truth for the angle convention) for the player's own shots. Its automatic fire (Cover,
			// npcvehicles::FireVehicleWeapon stores an aim point) is aimed like an NPC's (2026-10-21: those shots flew along the nose, 0 hits).
			double ax, ay, az;
			const bool automatic = npcvehicles::AimFor(id, ax, ay, az);
			if (id && id == vehicleflight::OwnRoverId() && !automatic) {
				const unsigned n = g_playerShots++;
				if (n < 20 || n % 50 == 0) {
					double hc = 0, hr = 0, pr = 0, pc = 0;
					const bool cam = Camera(hc, hr, pr, pc);
					const auto sp = At<game::Vec3>(shooter, off::kRefPosition);
					logger::Write("Vehicle aim: your shot %u: launch pitch %.3f, y %.3f, heading %.3f, origin (%.1f, %.1f, %.1f) (vehicle at %.1f, %.1f, %.1f); camera %s heading col %.3f row %.3f, pitch row %.3f col %.3f",
						n + 1, *pitch, roll, *heading, org[0], org[1], org[2], sp.x, sp.y, sp.z, cam ? "ok" : "none", hc, hr, pr, pc);
				}
				return;
			}
			// An NPC job's vehicle (or its NPC), or the own rover's automatic fire: angles at the led aim point.
			if (!automatic) return;
			const double dx = ax - org[0], dy = ay - org[1], dz = az - org[2];
			const double horiz = std::hypot(dx, dy);
			if (horiz + std::fabs(dz) < 1.0) return;
			const float wasX = *pitch, wasZ = *heading;
			*pitch = float(-std::atan2(dz, horiz));  // nose down +
			*heading = float(std::atan2(dx, dy));    // (sin z, cos z) = forward
			const unsigned n = g_aimed++;
			if (n < 10 || n % 50 == 0) {
				logger::Write("Vehicle aim: shot %u by %08X aimed: pitch %.3f -> %.3f, heading %.3f -> %.3f (target %.0f m)", n + 1, id, wasX, *pitch, wasZ, *heading,
					std::sqrt(horiz * horiz + dz * dz));
			}
		}

		void* LaunchDetour(void* out, float* data, std::uint64_t a3, std::uint64_t a4)
		{
			if (data) {
				__try {
					Before(data);
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					static bool told = false;
					if (!told) { told = true; logger::Write("Vehicle aim: exception reading the launch data; shot left as the game made it"); }
				}
			}
			std::uint32_t shooter = 0;
			if (data) {
				__try {
					const uptr s = *reinterpret_cast<uptr*>(reinterpret_cast<std::uint8_t*>(data) + kShooter);
					shooter = s ? At<std::uint32_t>(s, off::kFormId) : 0;
				}
				__except (EXCEPTION_EXECUTE_HANDLER) {
					shooter = 0;
				}
			}
			void* r = g_original(out, data, a3, a4);
			// The vehicle hit counter follows every projectile from its handle (the launch's out parameter).
			std::uint32_t handle = 0;
			__try {
				handle = out ? *reinterpret_cast<std::uint32_t*>(out) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				handle = 0;
			}
			if (handle) vehiclehits::NoteLaunch(handle, shooter);
			return r;
		}
	}

	void Install()
	{
		void* orig = nullptr;
		if (hook::Install(game::Base() + kLaunch, kLaunchPrologue, sizeof(kLaunchPrologue), reinterpret_cast<void*>(&LaunchDetour), &orig)) {
			g_original = reinterpret_cast<Launch_t>(orig);
			logger::Write("Vehicle aim: Projectile::Launch hooked (NPC vehicle shots aimed at launch)");
		}
		else {
			logger::Write("Vehicle aim: Projectile::Launch prologue differs; shots not aimed at launch");
		}
	}
}
