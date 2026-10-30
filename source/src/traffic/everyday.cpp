// FVO Traffic: everyday traffic. Moved from core's context.cpp (CivilRules, the "Everyday traffic" block) on 2026-10-29 with the
// same rules and log lines. Before the move core ran this itself and offered each pick to the addon; with the addon installed that
// is exactly the path kept here (the no-addon "laps" branch is gone with the move: without FVO Traffic there is no traffic).
#include "everyday.h"
#include "fvolimits.h"
#include "trafficpick.h"

#include <Windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>
#include <vector>

namespace everyday
{
	namespace
	{
		const FvoTrafficApi* g_api = nullptr;
		FvoLog_t g_logFn = nullptr;
		FvoTrafficOffer g_offer = nullptr;  // the route side (traffic_main.cpp Offer)
		std::unordered_map<std::uint32_t, double> g_used;  // vehicle -> when it last started a traffic trip (s)

		void Log(const char* fmt, ...)
		{
			if (!g_logFn) return;
			char text[600];
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(text, sizeof text, fmt, args);
			va_end(args);
			g_logFn(text);
		}
	}

	void Attach(const FvoTrafficApi* api, FvoLog_t log, FvoTrafficOffer routeOffer) { g_api = api; g_logFn = log; g_offer = routeOffer; }
	void Reset() { g_used.clear(); }

	int Scan()
	{
		if (!g_api) return 0;
		FvoTrafficPlayer pl{};
		if (!g_api->player(&pl)) return 0;
		std::vector<FvoTrafficVehicle> rows(fvolimits::kVehicles);  // core's vehicle list capacity
		rows.resize(std::size_t(std::max(g_api->everydayVehicles(rows.data(), fvolimits::kVehicles), 0)));
		std::vector<trafficpick::Vehicle> tv;
		for (const auto& v : rows) tv.push_back({ v.id, v.x, v.y, true });
		std::vector<FvoTrafficPerson> people(512);
		people.resize(std::size_t(std::max(g_api->everydayPeople(people.data(), 512), 0)));
		std::vector<trafficpick::Person> tp;
		for (const auto& q : people) tp.push_back({ std::uintptr_t(q.id), q.x, q.y, true });  // ref carries the actor's form id here
		std::vector<trafficpick::Place> places;
		{
			std::vector<double> xy(2 * 256);
			const int n = g_api->mapPlaces(xy.data(), 256);
			for (int k = 0; k < n; ++k) places.push_back({ xy[std::size_t(2 * k)], xy[std::size_t(2 * k + 1)] });
		}
		const double nowS = double(GetTickCount64()) / 1000.0;
		const auto pick = trafficpick::Choose(tv, tp, places, pl.x, pl.y, nowS, g_api->civilJobs(), g_used);
		if (pick.kind == trafficpick::Kind::None) return 0;
		const auto& v = tv[pick.vehicle];
		const unsigned did = unsigned(tp[pick.person].ref);
		g_used[v.id] = nowS;  // rests even when refused, so one bad pair is not retried every scan
		if (!g_api->mayUse(did, v.id)) {
			Log("Traffic: NPC %08X may not use vehicle %08X; next scan", did, v.id);
			return 0;
		}
		if (FvoTripClaim claim{}; g_offer && g_offer(v.x, v.y, 0.0, v.id, &claim) && claim.step) {
			// The route side takes the pick as a scripted trip.
			const bool ok = g_api->startScripted(did, v.id, &claim) != 0;
			Log("Traffic: NPC %08X takes vehicle %08X on a %s -> %s", did, v.id, claim.name ? claim.name : "scripted trip", ok ? "driving" : "refused");
			return ok ? 1 : 0;
		}
		if (pick.kind == trafficpick::Kind::RoundTrip) {
			const bool ok = g_api->startSupply(did, v.id, pick.to.x, pick.to.y) != 0;
			Log("Traffic: NPC %08X takes vehicle %08X on a round trip, %.0f m out to (%.0f, %.0f) -> %s", did, v.id, pick.tripLength, pick.to.x, pick.to.y, ok ? "driving" : "refused");
			return ok ? 1 : 0;
		}
		// 2026-10-28 (the user: traffic should follow the routes they gave, not fly laps): a pick the routes decline (busy, departures
		// spaced, out of reach) waits for the next scan instead.
		Log("Traffic: NPC %08X / vehicle %08X: the ring takes no more trips right now; no laps, next scan", did, v.id);
		return 0;
	}
}
