// Card GD-1: job lines (jobline.h). Offline only.
#include "jobline.h"
#include <cmath>
#include <cstdio>
#include <cstring>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace jobline;

int main()
{
    Ask a; a.on = true; a.jobGoal = true; a.onWheels = true; a.distance = 120;
    Check(Refuse(a) == nullptr, "a driving job 120 m away: on the plan");
    { Ask b = a; b.on = false; Check(!std::strcmp(Refuse(b), "switch_off"), "switch off: old autopilot"); }
    { Ask b = a; b.combat = true; Check(!std::strcmp(Refuse(b), "combat_keeps_its_control"), "combat keeps its control (user: jobs now, combat next)"); }
    { Ask b = a; b.combat = true; b.combatOnPlan = true; Check(Refuse(b) == nullptr, "CB-2: combat on the plan gets a combat line"); }
    { Ask b = a; b.combat = true; b.combatOnPlan = true; b.onWheels = false; Check(Refuse(b) == nullptr, "CB-3: a combat line flies as well"); }
    { Ask b = a; b.combat = true; b.combatOnPlan = true; b.distance = rules::combatHandBack; Check(!std::strcmp(Refuse(b), "combat_firing_spot_old_autopilot"), "CB-3: at the firing spot the old hold aims"); }
    { Ask b = a; b.combat = true; b.combatOnPlan = true; b.laneFrame = true; Check(!std::strcmp(Refuse(b), "slot_has_an_addon_lane_frame"), "CB-3: an addon lane frame wins over a combat line"); }
    { Ask b = a; b.scripted = true; Check(!std::strcmp(Refuse(b), "scripted_command"), "scripted command: old path"); }
    { Ask b = a; b.laneFrame = true; Check(!std::strcmp(Refuse(b), "slot_has_an_addon_lane_frame"), "a slot with an addon lane frame never gets a job line"); }
    { Ask b = a; b.onWheels = false; Check(!std::strcmp(Refuse(b), "hover_job_keeps_old_path"), "hover job: old path"); }
    { Ask b = a; b.distance = rules::handBack; Check(!std::strcmp(Refuse(b), "final_approach_old_autopilot"), "within 40 m: the old autopilot arrives"); }
    { Ask b = a; b.jobGoal = false; Check(!std::strcmp(Refuse(b), "no_job_goal"), "no job goal: nothing"); }

    auto noGround = [](double, double, double&) { return false; };
    auto flat = [](double, double, double& g) { g = 10; return true; };
    FvoLaneGuidanceV1 l;
    Check(Build(l, 0xFF001234u, 0, 0, 12, 0, 100, 12, 18, 0.6, flat) && l.count == 51, "100 m: a point every 2 m");
    Check(std::fabs(l.points[0].x) < 1e-9 && std::fabs(l.points[l.count - 1].y - 100) < 1e-9, "from the car to the goal");
    Check(std::fabs(l.points[10].z - 10.6) < 1e-9, "z on the known ground plus the rest height");
    Check(l.travel == 1 && l.stopDistance < 0 && l.speed == 18 && l.halfWidth == rules::halfWidth, "drive, passes through, the job's speed, the line width");
    FvoLaneGuidanceV1 far;
    Check(Build(far, 1, 0, 0, 0, 1000, 0, 0, 50, 0.6, noGround) && far.count == 64 && std::fabs(far.points[63].x - rules::maxLength) < 1e-9 && far.speed == rules::maxSpeed,
        "1 km: capped at 400 m, 64 points, speed capped");
    Check(std::fabs(far.points[32].z) < 1e-9, "no ground known: interpolated height");
    FvoLaneGuidanceV1 other; Build(other, 0xFF001234u, 0, 0, 12, 40, 100, 12, 18, 0.6, flat);
    Check(other.identity != l.identity, "a new goal is a new line identity");
    Check(!Build(l, 1, 5, 5, 0, 5.5, 5, 0, 10, 0.6, flat), "goal under the car: no line");
    Check(Rebuild(1.0, -1, 0, 0, 0, 0) && !Rebuild(1.2, 1.0, 1, 0, 0, 0) && Rebuild(1.6, 1.0, 0, 0, 0, 0) && Rebuild(1.1, 1.0, 5, 0, 0, 0), "rebuilt each 0.5 s or when the goal moves 4 m");
    std::printf("jobline_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
