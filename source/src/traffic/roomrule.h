#pragma once
// RR-1 (2026-11-07): the route side's room decision, pure, so it can be tested alone (tests/traffic_rules_test.cpp). Owner: the route
// side's room check (traffic_main.cpp Room), which gathers the facts and calls Decide. Same order and results as the inline check it replaces.
namespace roomrule
{
    struct Facts {
        bool timeKnown = true, active = true;   // the route's schedule this minute
        bool cityFlies = true; int count = 0;   // the city flies this route; wanted vehicles on it (scaled)
        bool revisionReady = true;
        int onRoute = 0, all = 0, cityLimit = 0;  // trips on this route, trips in traffic, the city's limit
        bool pool = false, editorRoute = false; int ruled = 0, ruledMax = 0;
    };
    // Returns "room" or the refusal's name.
    inline const char* Decide(const Facts& f) {
        if (!f.timeKnown) return "schedule_time_unknown";
        if (!f.active) return "schedule_inactive";
        if (!f.cityFlies) return "city_not_flying";
        if (f.count <= 0) return "count_zero";
        if (!f.revisionReady) return "revision_not_ready";
        if (f.onRoute >= f.count) return "route_full";
        if (f.all >= f.cityLimit) return "city_limit";
        if (!(f.pool || f.editorRoute || f.ruled < f.ruledMax)) return "ruled_max";
        return "room";
    }
    inline bool Has(const char* why) { return why && why[0] == 'r' && why[1] == 'o' && why[2] == 'o' && why[3] == 'm' && !why[4]; }
}
