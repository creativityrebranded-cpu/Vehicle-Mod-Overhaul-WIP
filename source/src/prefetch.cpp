#include "prefetch.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "telemetry.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace prefetch
{
	namespace
	{
		constexpr float kMinSpeed = 20.0f;     // m/s: below this the grid stays centred on the player
		std::atomic<float> g_leadSeconds{ 1.5f };  // how far ahead (in time) to centre the grid (SeamlessStream sets it)

		using GridUpdate_t = void (*)(std::uintptr_t, float*, char, char, std::uintptr_t);
		GridUpdate_t g_original = nullptr;

		struct Motion
		{
			float x = 0, y = 0;
			LARGE_INTEGER qpc{};
			float vx = 0, vy = 0;
			bool valid = false;
		} g_motion;

		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		// The ray's third argument is the collision layer mask of its filter (hknpBSCustomCollisionFilter). 0x800 (bit 11)
		// is the WATER layer: it hit only the water level (about -33 m everywhere), tens to 160 m below the land. The game's
		// own ground query (wrapper FUN_140583b50) passes 0x20002: bits 1 (static) and 17 (ground).
		// 0x20002 alone missed the planet terrain (answered -500 = no hit, 0.32.7): add anim static (2) and terrain (13).
		constexpr std::uint32_t kGroundLayers = 0x22006;

		struct Probe
		{
			bool requested = false;
			float x = 0, y = 0, z = 0, above = 0, length = 0;
			bool haveResult = false;
			float ground = -3.4e38f;
			unsigned age = 0;
			std::uint32_t mask = 0;  // 0: kGroundLayers
		};
		std::mutex g_probeLock;
		std::atomic<std::uintptr_t> g_probeAnchor{ 0 };
		std::atomic<bool> g_leadSuppressed{ false };
		Probe g_probes[kSlotCount];

		// Runs queued probes; called from inside the player's per-frame update (safe for physics queries).
		void ServiceProbes(std::uintptr_t tes)
		{
			Probe work[kSlotCount];
			{
				std::lock_guard lk(g_probeLock);
				for (int i = 0; i < kSlotCount; ++i) {
					work[i] = g_probes[i];
					g_probes[i].requested = false;
					if (g_probes[i].haveResult) {
						++g_probes[i].age;
					}
				}
			}
			for (int i = 0; i < kSlotCount; ++i) {
				if (!work[i].requested) {
					continue;
				}
				const float p[3] = { work[i].x, work[i].y, work[i].z };
				const float ground = reinterpret_cast<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(
					game::Base() + addr::kGroundRayCast)(tes, p, work[i].mask ? work[i].mask : kGroundLayers, work[i].above, work[i].length);
				std::lock_guard lk(g_probeLock);
				g_probes[i].ground = ground;
				g_probes[i].haveResult = true;
				g_probes[i].age = 0;
			}
		}

		void Detour(std::uintptr_t tes, float* pos, char a3, char a4, std::uintptr_t a5)
		{
			const auto callOriginal = [&](float* at) {
				const auto start = telemetry::NowUs();
				g_original(tes, at, a3, a4, a5);
				const auto elapsed = telemetry::NowUs() - start;
				if (elapsed >= 2000) {
					telemetry::Record("grid_update_slow", "\"us\":%llu,\"led\":%d,\"grid\":%u",
						static_cast<unsigned long long>(elapsed), at != pos ? 1 : 0, *reinterpret_cast<std::uint32_t*>(tes + 0x28));
				}
			};
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			const auto anchorRef = g_probeAnchor.load();
			if (anchorRef && pos == reinterpret_cast<float*>(anchorRef + off::kRefPosition)) {
				ServiceProbes(tes);  // the grid follows the flown ship: probes are safe here too
				callOriginal(pos);
				return;
			}
			// Most live grid updates pass a copied position, including roughly 400 matching rover calls per 10 s
			// in the 04:25-04:34 run. Classify the copy by its position, not only by pointer identity.
			static unsigned nPlayer = 0, nVehicle = 0, nOther = 0, nLead = 0;
			static unsigned nCopiedPlayer = 0, nCopiedVehicle = 0;
			const auto vehicle = player ? reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(game::Base() + addr::kPlayerVehicle)(player) : 0;
			const auto* playerPos = player ? reinterpret_cast<float*>(player + off::kRefPosition) : nullptr;
			const auto* vehiclePos = vehicle ? reinterpret_cast<float*>(vehicle + off::kRefPosition) : nullptr;
			const auto samePosition = [pos](const float* p) {
				return p && std::isfinite(pos[0]) && std::isfinite(pos[1]) && std::isfinite(p[0]) && std::isfinite(p[1]) &&
					std::hypot(pos[0] - p[0], pos[1] - p[1]) < 1.0f;
			};
			std::uintptr_t mover = 0;
			if (vehicle && pos == vehiclePos) mover = vehicle;
			else if (player && pos == playerPos) mover = player;
			else if (vehicle && samePosition(vehiclePos)) { mover = vehicle; ++nCopiedVehicle; }
			else if (player && samePosition(playerPos)) { mover = player; ++nCopiedPlayer; }
			{
				static ULONGLONG lastDiag = 0;
				const ULONGLONG t = GetTickCount64();
				if (!lastDiag) lastDiag = t;
				if (t - lastDiag >= 10000) {
					if (nPlayer + nVehicle + nOther > 0) {
						const auto d = *reinterpret_cast<std::uint32_t*>(tes + 0x28);
						logger::Write("Prefetch: last 10 s: %u player calls, %u vehicle calls, %u other (%u copied player, %u copied vehicle within 1 m); lead applied in %u; speed %.1f m/s, grid %u, lead suppressed %d",
							nPlayer, nVehicle, nOther, nCopiedPlayer, nCopiedVehicle, nLead, std::hypot(g_motion.vx, g_motion.vy), d, g_leadSuppressed.load() ? 1 : 0);
					}
					nPlayer = nVehicle = nOther = nLead = nCopiedPlayer = nCopiedVehicle = 0;
					lastDiag = t;
				}
			}
			if (!mover) {
				++nOther;
				callOriginal(pos);  // not the player's per-frame call
				return;
			}
			++(mover == player ? nPlayer : nVehicle);
			ServiceProbes(tes);
			{
				// How often the player's grid update really runs (the probe service depends on it).
				static unsigned calls = 0;
				static ULONGLONG since = 0;
				++calls;
				const ULONGLONG t = GetTickCount64();
				if (since == 0) {
					since = t;
				}
				if (t - since >= 5000) {
					telemetry::Record("player_grid_calls", "\"per_s\":%.1f", calls * 1000.0 / double(t - since));
					calls = 0;
					since = t;
				}
			}
			// Player velocity from successive calls (smoothed; jumps from teleports/re-centres reset it).
			LARGE_INTEGER now, freq;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&freq);
			if (g_motion.valid) {
				const float dt = static_cast<float>(double(now.QuadPart - g_motion.qpc.QuadPart) / double(freq.QuadPart));
				const float dx = pos[0] - g_motion.x, dy = pos[1] - g_motion.y;
				if (dt > 0.0f && dt < 0.5f && std::hypot(dx, dy) < 200.0f) {
					g_motion.vx = 0.8f * g_motion.vx + 0.2f * (dx / dt);
					g_motion.vy = 0.8f * g_motion.vy + 0.2f * (dy / dt);
				}
				else {
					g_motion.vx = g_motion.vy = 0.0f;
				}
			}
			g_motion.x = pos[0];
			g_motion.y = pos[1];
			g_motion.qpc = now;
			g_motion.valid = true;

			const float speed = std::hypot(g_motion.vx, g_motion.vy);
			// 0.53.2: no lead for the vehicle's call. Leading the grid moves TES's current cell ahead of the driver; when
			// the driver then crossed into that cell the engine set the player onto the ground every frame (writer
			// +0x193B442 against the vehicle's +0xBA900E) and the player dropped out of the rover at 100 m/s (2026-10-15,
			// three times, each exactly at a cell crossing; the lead ran in vehicles only since 0.51.5). Probes still run.
			// 0.53.3: back on for the diagnosis (the user: avoiding it is not a fix); the core logs the drop with its stack.
			constexpr bool kVehicleLead = true;
			if (!kVehicleLead && mover != player) {
				callOriginal(pos);
				return;
			}
			const auto dim = *reinterpret_cast<std::uint32_t*>(tes + 0x28);  // loaded grid size (uGridsToLoad)
			const float maxLead = std::max(0.0f, (static_cast<float>(dim / 2) - 1.0f) * off::kCellSize);
			if (speed < kMinSpeed || maxLead <= 0.0f || g_leadSuppressed.load()) {
				callOriginal(pos);
				return;
			}
			++nLead;
			const float lead = std::min(speed * g_leadSeconds.load(), maxLead);
			float ahead[3] = { pos[0] + g_motion.vx / speed * lead, pos[1] + g_motion.vy / speed * lead, pos[2] };
			callOriginal(ahead);

			static ULONGLONG lastLog = 0;
			const ULONGLONG ms = GetTickCount64();
			if (ms - lastLog > 2000) {
				lastLog = ms;
				telemetry::Record("prefetch", "\"speed\":%.1f,\"requested_s\":%.1f,\"lead_m\":%.0f,\"max_lead_m\":%.0f,\"nominal_forward_m\":%.0f,\"nominal_forward_s\":%.2f,\"grid\":%u",
					speed, g_leadSeconds.load(), lead, maxLead, dim * off::kCellSize * 0.5f + lead,
					(dim * off::kCellSize * 0.5f + lead) / speed, dim);
			}
		}
	}

	void SetProbeAnchor(std::uintptr_t ref) { g_probeAnchor = ref; }

	void SuppressLead(bool on) { g_leadSuppressed = on; }

	void SetLeadSeconds(float s) { g_leadSeconds = std::clamp(s, 0.0f, 10.0f); }

	void ServiceProbesNow(std::uintptr_t tes) { ServiceProbes(tes); }

	void RequestGround(int slot, float x, float y, float z, float above, float length)
	{
		if (slot < 0 || slot >= kSlotCount) {
			return;
		}
		std::lock_guard lk(g_probeLock);
		auto& p = g_probes[slot];
		p.requested = true;
		p.x = x;
		p.y = y;
		p.z = z;
		p.above = above;
		p.length = length;
		p.mask = 0;
	}

	void RequestGroundMask(int slot, float x, float y, float z, float above, float length, std::uint32_t mask)
	{
		RequestGround(slot, x, y, z, above, length);
		std::lock_guard lk(g_probeLock);
		g_probes[slot].mask = mask;
	}

	bool LatestGround(int slot, float& ground, unsigned& age)
	{
		if (slot < 0 || slot >= kSlotCount) {
			return false;
		}
		std::lock_guard lk(g_probeLock);
		const auto& p = g_probes[slot];
		if (!p.haveResult) {
			return false;
		}
		ground = p.ground;
		age = p.age;
		return true;
	}

	void ResetProbes()
	{
		std::lock_guard lk(g_probeLock);
		for (auto& p : g_probes) p = Probe{};
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[15] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x18, 0x48, 0x89, 0x78, 0x20 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kGridUpdate, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Detour), &original)) {
			return false;
		}
		g_original = reinterpret_cast<GridUpdate_t>(original);
		return true;
	}
}
