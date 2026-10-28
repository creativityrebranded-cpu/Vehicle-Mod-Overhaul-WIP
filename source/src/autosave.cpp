#include "autosave.h"

#include "addresses.h"
#include "anchor.h"
#include "flight.h"
#include "game.h"
#include "log.h"
#include "notify.h"
#include "recentre.h"
#include "telemetry.h"

#include <Windows.h>

#include <cstdint>

namespace autosave
{
	namespace
	{
		constexpr ULONGLONG kIntervalMs = 5ull * 60ull * 1000ull;  // since the last save (any kind)
		constexpr ULONGLONG kRetryMs = 20000;                      // between attempts while it is not safe / not allowed
		constexpr std::uint32_t kLastSaveTick = 0x60;              // saveLoadManager: GetTickCount64 of the last save

		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		ULONGLONG g_lastTry = 0;
	}

	void Tick()
	{
		const auto mgr = Ptr(addr::kSaveLoadManagerPtr);
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!mgr || !player) {
			return;
		}
		const ULONGLONG now = GetTickCount64();
		const ULONGLONG lastSave = At<ULONGLONG>(mgr, kLastSaveTick);
		if (now - lastSave < kIntervalMs || now - g_lastTry < kRetryMs) {
			return;
		}
		g_lastTry = now;
		// Safe moment: not in flight (a save mid-air would load falling), no re-centre waiting, cells settled outdoors.
		if (flight::Busy() || recentre::Pending()) {
			return;
		}
		const auto a = anchor::Current();
		if (a.ref == player && !anchor::WorldSettled()) {
			return;
		}
		const auto flagsBefore = At<std::uint32_t>(mgr, 0x50);
		EngineFn<void (*)(std::uintptr_t, std::uint32_t, char)>(addr::kRequestSave)(mgr, 1, 0);
		if ((At<std::uint32_t>(mgr, 0x50) & 1) && !(flagsBefore & 1)) {
			logger::Write("Autosave: requested (%.1f min since the last save)", double(now - lastSave) / 60000.0);
			telemetry::Record("autosave", "\"since_min\":%.1f", double(now - lastSave) / 60000.0);
		}
	}
}
