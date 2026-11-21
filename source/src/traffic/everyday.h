#pragma once
// FVO Traffic: everyday traffic (moved from core's context.cpp on 2026-10-29, stage 2 of notes/TRAFFIC_ADDON_PROPOSAL_20261029.md).
// On core's civil scan (every 15 s, traffic on, nobody fighting, civil jobs under the cap) it picks the free parked vehicle nearest
// the player that has a calm person within 60 m (trafficpick.h) and starts that person's trip: along a route when the route side
// takes it, else a round trip to a settlement / outpost 200-1000 m away; never laps (the user, 2026-10-28: traffic follows the
// routes). Same rules and "Traffic:" log lines as before the move.
#include "traffic_api.h"

namespace everyday
{
	void Attach(const FvoTrafficApi* api, FvoLog_t log, FvoTrafficOffer routeOffer);
	int Scan();    // core's hook: 1 = a trip started
	void Reset();  // a save load
}
