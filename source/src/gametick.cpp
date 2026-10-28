#include "gametick.h"

#include "addresses.h"
#include "api_core.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "prefetch.h"
#include "shipmodel.h"
#include "telemetry.h"

#include <Windows.h>

#include <cstdint>

namespace gametick
{
	namespace
	{
		using Update_t = void (*)(std::uintptr_t, std::uintptr_t);
		Update_t g_original = nullptr;

		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		void Detour(std::uintptr_t system, std::uintptr_t arg)
		{
			g_original(system, arg);
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			const auto tes = Ptr(addr::kTESPtr);
			if (tes) {
				prefetch::ServiceProbesNow(tes);
			}
			if (player) {
				shipmodel::OnPlayerUpdate(player);
				coreapi::FirePlayerUpdate(player);
			}
			static unsigned calls = 0;
			static ULONGLONG since = 0;
			++calls;
			const ULONGLONG now = GetTickCount64();
			if (since == 0) {
				since = now;
			}
			if (now - since >= 10000) {
				telemetry::Record("game_tick", "\"per_s\":%.1f", calls * 1000.0 / double(now - since));
				calls = 0;
				since = now;
			}
		}
	}

	bool Install()
	{
		static const std::uint8_t kPrologue[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kShipSystemUpdate, kPrologue, sizeof(kPrologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Game tick: hook failed (ground probes and the ship model only run on cell changes)");
			return false;
		}
		g_original = reinterpret_cast<Update_t>(original);
		logger::Write("Game tick: per-frame context installed");
		return true;
	}
}
