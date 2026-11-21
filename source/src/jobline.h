#pragma once
// Job lines (card GD-1, 2026-11-07; user: "Jobs now, combat next"). Owner: Route (design section 4: "summon or job (PlanRoute)" reduces
// to a line). A job goal admitted at the goal door (PilotFly) becomes one line from the car toward the goal, packaged as the same lane
// frame the plan already follows, so World, Avoid, Speed, the arbiter and the locked controller drive it. Nothing else changes:
//   - the final approach (within rules::handBack of the goal), landing, holding and parking stay with the old autopilot;
//   - combat, scripted commands, hover jobs and pilots whose slot still carries an addon lane frame keep the old path;
//   - settings.ini [traffic] planJobs (1 in test builds, 0 in FVO_RELEASE) switches it; off = exactly the old behaviour.
// Pure: tests/jobline_test.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include "routeguidance.h"

namespace jobline
{
    namespace rules {
        inline constexpr double handBack = 40.0;      // m from the goal: the old autopilot takes the final approach
        inline constexpr double maxLength = 400.0;    // m: a longer job gets a line this long toward the goal, rebuilt as the car goes
        inline constexpr double rebuildMoved = 4.0;   // m the goal moved, or
        inline constexpr double rebuildEvery = 0.5;   // s since the last build
        inline constexpr double halfWidth = 3.5;      // m: the line's width (Avoid's own lane)
        inline constexpr double minSpeed = 4.0, maxSpeed = 30.0;
        // CB-3 combat lines (user: "build them"): short, rebuilt often, the hand-back only at the firing spot.
        inline constexpr double combatLength = 40.0, combatRebuildEvery = 0.1, combatHandBack = 6.0, combatMaxSpeed = 40.0;
    }
    struct Ask {
        bool on = false;           // the switch
        bool jobGoal = false;      // the goal door holds a Job goal for this pilot
        bool onWheels = false;     // driving (hover jobs keep the old path)
        bool combat = false, scripted = false;
        bool combatOnPlan = false; // CB-7: settings.ini [traffic] planCombat on and combat said this frame it may go on the plan (not stunned)
        bool laneFrame = false;    // the slot has (or recently had) an addon lane frame, policy or rules
        double distance = 0;       // to the goal
    };
    // Why not (nullptr: eligible).
    inline const char* Refuse(const Ask& a) {
        if (!a.on) return "switch_off";
        if (!a.jobGoal) return "no_job_goal";
        if (a.combat && !a.combatOnPlan) return "combat_keeps_its_control";
        if (a.scripted) return "scripted_command";
        if (a.laneFrame) return "slot_has_an_addon_lane_frame";
        if (!a.onWheels && !a.combat) return "hover_job_keeps_old_path";  // combat lines fly as well
        if (!(a.distance > (a.combat ? rules::combatHandBack : rules::handBack))) return a.combat ? "combat_firing_spot_old_autopilot" : "final_approach_old_autopilot";
        return nullptr;
    }
    // The line: straight from the car toward the goal, at most maxLength, up to 64 points; z from `groundAt` (+originAbove) where
    // known, else interpolated car height -> goal height. identity names the vehicle and the goal (so a new goal is a new line).
    template <class GroundFn>
    inline bool Build(FvoLaneGuidanceV1& out, std::uint32_t vehicle, double cx, double cy, double cz, double gx, double gy, double gz,
        double speed, double originAbove, GroundFn&& groundAt, int travel = 1, double maxLen = rules::maxLength, double topSpeed = rules::maxSpeed)
    {
        const double dx = gx - cx, dy = gy - cy, d = std::hypot(dx, dy);
        if (!(d > 1.0) || !std::isfinite(d)) return false;
        const double len = std::fmin(d, maxLen), ux = dx / d, uy = dy / d;
        const double endZ = std::isfinite(gz) ? gz : cz;
        out = FvoLaneGuidanceV1{};
        out.identity = (std::uint64_t(vehicle) << 32) ^ (std::uint64_t(std::llround(gx / 4.0)) * 73856093ull) ^ (std::uint64_t(std::llround(gy / 4.0)) * 19349663ull) ^ 0x4A0B000000000000ull;
        out.travel = travel;  // 1 on wheels (Drive) for jobs; a combat line in the air is 2 (hover)
        out.halfWidth = rules::halfWidth;
        out.speed = std::fmax(rules::minSpeed, std::fmin(topSpeed, speed > 0 ? speed : 15.0));
        out.stopDistance = -1;  // passes through: the hand-back comes before the line ends
        const int n = std::max(2, std::min(64, int(std::ceil(len / 2.0)) + 1));
        out.count = n;
        for (int i = 0; i < n; ++i) {
            const double s = len * i / (n - 1), x = cx + ux * s, y = cy + uy * s;
            double g = 0;
            const double z = groundAt(x, y, g) ? g + originAbove : cz + (endZ - cz) * (s / d);
            out.points[i] = { x, y, z };
        }
        return true;
    }
    inline bool Rebuild(double now, double builtAt, double gx, double gy, double builtGx, double builtGy, double every = rules::rebuildEvery) {
        return builtAt < 0 || now < builtAt || now - builtAt >= every || std::hypot(gx - builtGx, gy - builtGy) > rules::rebuildMoved;
    }
}
