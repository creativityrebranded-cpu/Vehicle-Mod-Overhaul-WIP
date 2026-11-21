#pragma once
// The goal door (notes/VEHICLE_DECISION_DESIGN_20261106.md sections 4 and 9; work order step 4): one way in for every goal. Whatever asks
// (an NPC job through PilotFly, a scripted or combat command through PilotCommand, the traffic addon's lane, your Cruise), the goal is
// recorded here once, with where it ends and how (pass, stop, arrive) and an optional facing. Route reads its ending and facing from the
// door; the three old entry points are adapters that keep their signatures and call Admit. Pure; tests: tests/goaldoor_test.cpp.
//
// Honest scope (2026-11-07): the plan follows lane goals (traffic and Cruise). Job goals (PilotFly) and commands (PilotCommand) are
// recorded and traced here, but are still driven by the old autopilot, because the plan has no Route line for them yet (PlanRoute ->
// line, design section 4). A command is direct actuation (throttle, yaw, strafe): it is recorded as such, so the ledger can name it.
#include <cmath>
#include <cstdint>
#include <limits>

namespace goaldoor {
enum class Source : std::uint8_t { None, Lane, Cruise, Job, Command, Combat };
inline const char* Name(Source s) { constexpr const char* n[]{ "none", "lane", "cruise", "job", "command", "combat" }; return n[int(s)]; }
// How the line ends. Lift-off, landing and parking are arrive lines (design section 4), not separate sequences.
enum class Ending : std::uint8_t { Pass, Stop, Arrive, Land, Park };
inline const char* Name(Ending e) { constexpr const char* n[]{ "pass", "stop", "arrive", "land", "park" }; return n[int(e)]; }

struct Goal {
    Source source = Source::None;
    Ending ending = Ending::Pass;
    double x = 0, y = 0, z = std::numeric_limits<double>::quiet_NaN();  // where it ends (z NaN: the surface there)
    double stop = 0;                                                    // metres short of it
    double heading = std::numeric_limits<double>::quiet_NaN();          // arrive heading (NaN: any)
    double facing = std::numeric_limits<double>::quiet_NaN();           // where the nose points while travelling (NaN: along the line)
    bool keepGoing = false;                                             // arriving does not end the trip (follow, profiles)
    bool direct = false;                                                // direct actuation (a command): not on the plan, the ledger names it
    std::uint64_t identity = 0;                                         // the lane's identity, the job's vehicle, ...
    double at = 0;                                                      // when it was admitted (s)
};

// The ending a job goal asks for, from the shared goal contract's own flags (vcontract::Goal): hold + land = land; hold = arrive and stay;
// pass-through or cruise = pass; otherwise stop at the goal.
inline Ending JobEnding(bool hold, bool landOnArrival, bool passThrough, bool keepGoing) {
    if (passThrough) return Ending::Pass;
    if (hold && landOnArrival) return Ending::Land;
    if (hold) return Ending::Arrive;
    if (keepGoing) return Ending::Pass;
    return Ending::Stop;
}
// A lane's line_end (fieldboxapi lineEnd: 0 pass, 1 stop, 2 arrive) with its park flag.
inline Ending LaneEnding(int lineEnd, bool park) { return lineEnd >= 2 ? (park ? Ending::Park : Ending::Arrive) : lineEnd == 1 ? Ending::Stop : Ending::Pass; }

template <int N>
struct Door {
    Goal g[N];
    // Admit a goal for a pilot. Returns true when it changed what this pilot is asked to do (the caller traces it: one line per change).
    bool Admit(int pilot, const Goal& goal) {
        if (pilot < 0 || pilot >= N) return false;
        Goal& was = g[pilot];
        // A line that passes through has no real end: its sampled end point moves with the car, and that is not a new goal.
        const bool moved = goal.ending != Ending::Pass && (std::fabs(was.x - goal.x) > 1.0 || std::fabs(was.y - goal.y) > 1.0);
        const bool changed = was.source != goal.source || was.ending != goal.ending || was.identity != goal.identity || was.direct != goal.direct ||
            moved || (std::isfinite(was.heading) != std::isfinite(goal.heading)) ||
            (std::isfinite(goal.heading) && std::fabs(std::remainder(was.heading - goal.heading, 6.283185307179586)) > .05);
        was = goal;
        return changed;
    }
    void Clear(int pilot) { if (pilot >= 0 && pilot < N) g[pilot] = Goal{}; }
    const Goal* Of(int pilot) const { return pilot >= 0 && pilot < N && g[pilot].source != Source::None ? &g[pilot] : nullptr; }
};
}
