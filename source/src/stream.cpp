#include "stream.h"

#include "addresses.h"
#include "core_api.h"
#include "game.h"
#include "log.h"
#include "anchor.h"

#include <Windows.h>

#include <cmath>
#include <array>
#include <cstdint>

namespace stream
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		constexpr float kLeadSeconds = 2.5f;  // core default 1.5 s; capped at 150 m by the 5x5 grid
		game::Vec3 g_last{};
		ULONGLONG g_lastAt = 0, g_lastLog = 0;
		double g_speed = 0;
	}

	void Start()
	{
		if (const auto* api = seamless::Api()) {
			api->SetLeadSeconds(kLeadSeconds);
			logger::Write("Stream: load-ahead lead set to %.1f s", kLeadSeconds);
		}
	}

	void Sync(std::uintptr_t /*tes*/)
	{
		const auto* api = seamless::Api();
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		if (!api || !player) return;
		const ULONGLONG now = GetTickCount64();
		const auto vehicle = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(game::Base() + addr::kPlayerVehicle)(player);
		const auto p = At<game::Vec3>(vehicle ? vehicle : player, off::kRefPosition);
		if (g_lastAt && now > g_lastAt) {
			const double d = std::hypot(p.x - g_last.x, p.y - g_last.y);
			if (d < 300.0) {
				g_speed += (d / ((now - g_lastAt) / 1000.0) - g_speed) * 0.1;
			} else {
				g_speed = 0;
			}
		}
		g_last = p;
		g_lastAt = now;
		// Use the shorter lead while moving slowly so most of the 5x5 grid remains behind and to the sides.
		// Above 40 m/s the 2.5 s request reaches its 150 m cap by 60 m/s.
		static float appliedLead = kLeadSeconds;
		const float wantedLead = g_speed >= 40.0 ? 2.5f : 1.5f;
		if (wantedLead != appliedLead) {
			api->SetLeadSeconds(wantedLead);
			appliedLead = wantedLead;
		}
		// Off-grid requests use the same native priority-5 LoadCell lane as the active grid.
		// In the 11:30 run this submitted 128 speculative cells while active-grid readiness
		// still stalled. Keep the native forward grid lead, but do not compete with cells
		// needed now until off-grid completion/retention can be budgeted by stable cell ID.
		if (g_speed > 15.0 && now - g_lastLog >= 10000) {
			g_lastLog = now;
			logger::Write("Stream: %.0f m/s, %u grid cells waiting for collision, demand grid %s, off-grid preload suspended", g_speed, api->CollisionPending(), anchor::DemandReady() ? "ready" : "loading");
		}
	}
}
