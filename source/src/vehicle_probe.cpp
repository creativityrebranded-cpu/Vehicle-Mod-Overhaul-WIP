#include "vehicle_probe.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "prefetch.h"
#include "shipmodel.h"

#include <Windows.h>
#include <array>
#include <mutex>

namespace vehicle_probe
{
	namespace
	{
		constexpr std::uint32_t kGroundLayers = 0x22006;
		struct Probe
		{
			bool pending = false;
			bool valid = false;
			float x = 0, y = 0, z = 0, above = 0, length = 0;
			float height = -3.4e38f;
			std::uint32_t mask = 0;
			unsigned age = 0;
		};
		std::array<Probe, prefetch::kSlotCount> g_probes{};
		std::mutex g_lock;
		using Update = void (*)(std::uintptr_t, std::uintptr_t);
		Update g_original = nullptr;

		void Detour(std::uintptr_t system, std::uintptr_t argument)
		{
			g_original(system, argument);
			const auto base = game::Base();
			const auto tes = *reinterpret_cast<std::uintptr_t*>(base + addr::kTESPtr);
			if (!tes) return;
			const auto ray = reinterpret_cast<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(base + addr::kGroundRayCast);
			for (int i = 0; i < prefetch::kSlotCount; ++i) {
				Probe work;
				{
					std::lock_guard lk(g_lock);
					work = g_probes[i];
					g_probes[i].pending = false;
					if (g_probes[i].valid && g_probes[i].age < 1000) ++g_probes[i].age;
				}
				if (!work.pending) continue;
				const float pos[3] = { work.x, work.y, work.z };
				const float height = ray(tes, pos, work.mask ? work.mask : kGroundLayers, work.above, work.length);
				{
					std::lock_guard lk(g_lock);
					g_probes[i].height = height;
					g_probes[i].valid = true;
					g_probes[i].age = 0;
				}
			}
			const auto player = *reinterpret_cast<std::uintptr_t*>(base + addr::kPlayerCharacterPtr);
			if (player) shipmodel::OnPlayerUpdate(player);
		}
	}

	bool Install()
	{
		static const std::uint8_t prologue[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kShipSystemUpdate, prologue, sizeof(prologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Vehicle: standalone ground-update hook unavailable");
			return false;
		}
		g_original = reinterpret_cast<Update>(original);
		logger::Write("Vehicle: standalone ground probes installed");
		return true;
	}

	void Request(int slot, float x, float y, float z, float above, float length, std::uint32_t mask)
	{
		if (slot < 0 || slot >= prefetch::kSlotCount) return;
		std::lock_guard lk(g_lock);
		auto& p = g_probes[slot];
		p.pending = true;
		p.x = x; p.y = y; p.z = z; p.above = above; p.length = length; p.mask = mask;
	}

	bool Latest(int slot, float& height, unsigned& age)
	{
		if (slot < 0 || slot >= prefetch::kSlotCount) return false;
		std::lock_guard lk(g_lock);
		const auto& p = g_probes[slot];
		height = p.height;
		age = p.age;
		return p.valid;
	}

	void Reset()
	{
		std::lock_guard lk(g_lock);
		g_probes = {};
	}
}
