#include "fleetrail_policy.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0;
int checks = 0;

void Check(bool ok, const char* name)
{
    ++checks;
    if (ok) return;
    std::fprintf(stderr, "FAIL %s\n", name);
    ++failures;
}

bool Near(double left, double right, double tolerance = 1e-8)
{
    return std::abs(left - right) < tolerance;
}

fleetrail::Facts F(bool now, bool exit, bool enter, bool chassis, bool autopilot)
{
    fleetrail::Facts f;
    f.attachedNow = now;
    f.exitAheadAttached = exit;
    f.enterAheadAttached = enter;
    f.chassisReady = chassis;
    f.autopilotReady = autopilot;
    return f;
}
}

int main()
{
    using fleetrail::Choose;
    using fleetrail::Mode;
    constexpr unsigned long long kLong = 10000, kShort = 200;

    // Mode choice: buffer zone and minimum time in a mode.
    Check(Choose(Mode::Auto, F(true, true, true, true, true), kLong) == Mode::Auto, "keep autopilot well inside the physics area");
    Check(Choose(Mode::Auto, F(true, false, false, true, true), kLong) == Mode::Rail, "take rail before the buffer edge");
    Check(Choose(Mode::Auto, F(true, false, false, true, true), kShort) == Mode::Auto, "no planned switch before the minimum time");
    Check(Choose(Mode::Auto, F(false, false, false, true, true), kShort) == Mode::Rail, "own cell gone: rail at once, minimum time ignored");
    Check(Choose(Mode::Rail, F(false, false, false, true, false), kLong) == Mode::Rail, "hold rail outside attached cells");
    Check(Choose(Mode::Rail, F(true, true, false, true, true), kLong) == Mode::Rail, "stay on rail until the far point is attached too");
    Check(Choose(Mode::Rail, F(true, true, true, true, false), kLong) == Mode::Rail, "hold rail until autopilot is ready");
    Check(Choose(Mode::Rail, F(true, true, true, true, true), kShort) == Mode::Rail, "no handoff before the minimum time");
    Check(Choose(Mode::Rail, F(true, true, true, true, true), kLong) == Mode::Auto, "hand off once inside the buffer and ready");
    Check(Choose(Mode::Rail, F(false, false, false, false, false), kLong) == Mode::Awaiting, "lose body without claiming rail motion");
    Check(Choose(Mode::Awaiting, F(false, false, false, true, false), kLong) == Mode::Rail, "resume rail after chassis returns");
    Check(Choose(Mode::Awaiting, F(true, true, true, true, false), kLong) == Mode::Awaiting, "no automatic handoff from missing evidence");
    // A car near the edge that alternates between attached and not attached ahead does not flap.
    {
        Mode m = Mode::Rail;
        int switches = 0;
        for (int tick = 0; tick < 50; ++tick) {
            const bool exitOk = tick % 2 == 0;
            const Mode next = Choose(m, F(true, exitOk, false, true, true), 100ull * (tick % 10));
            if (next != m) ++switches;
            m = next;
        }
        Check(switches == 0, "edge flicker of the near point does not hand back to autopilot");
    }

    // Exit ahead distance.
    Check(Near(fleetrail::ExitAhead(0), fleetrail::rules::kExitAheadMin), "exit look-ahead has a minimum");
    Check(Near(fleetrail::ExitAhead(20), 50), "exit look-ahead grows with speed");
    Check(Near(fleetrail::ExitAhead(100), fleetrail::rules::kExitAheadMax), "exit look-ahead has a maximum");

    // Blends and ramps.
    Check(Near(fleetrail::BlendWeight(0), 1) && Near(fleetrail::BlendWeight(fleetrail::rules::kBlendSeconds), 0), "entry blend runs from 1 to 0");
    Check(fleetrail::BlendWeight(0.25) > fleetrail::BlendWeight(0.5) && fleetrail::BlendWeight(0.5) > fleetrail::BlendWeight(0.75), "entry blend falls steadily");
    Check(Near(fleetrail::Approach(10, 20, 1), 10 + fleetrail::rules::kAccel), "rail speeds up gradually");
    Check(Near(fleetrail::Approach(10, 0, 1), 10 - fleetrail::rules::kDecel), "rail slows gradually");
    Check(Near(fleetrail::Approach(10, 10.5, 1), 10.5), "rail reaches its target without overshoot");
    Check(Near(fleetrail::RampCap(12, 0), 12) && Near(fleetrail::RampCap(12, 1000), 12 + fleetrail::rules::kRampAccel), "exit ramp starts at the rail speed and rises");
    Check(std::isinf(fleetrail::RampCap(12, fleetrail::rules::kRampMs)), "exit ramp ends");

    // Path geometry.
    constexpr FvoFleetRailPoint points[] = {{0, 0, 0}, {0, 0, 0}, {10, 0, 0}, {10, 8, 6}};
    fleetrail::Path path;
    Check(path.Set(points, 4), "accept route with repeated point");
    Check(Near(path.End(), 20), "measure two three-dimensional segments");
    const auto afterTurn = path.At(15);
    Check(Near(afterTurn.x, 10) && Near(afterTurn.y, 4) && Near(afterTurn.z, 3), "interpolate a graded segment after a turn");
    Check(Near(path.Project(9, 4, 3), 15), "recover nearest station after switching modes");
    Check(Near(path.Advance(19, 10), path.End()), "stop at route end without overshoot");
    Check(Near(path.Advance(3, -10), 0), "clamp before route start");
    const FvoFleetRailPoint invalid[] = {{0, 0, 0}, {0, 0, std::numeric_limits<double>::infinity()}};
    Check(!path.Set(invalid, 2) && Near(path.End(), 20), "reject invalid path and preserve active path");

    // Corner speed.
    constexpr FvoFleetRailPoint straight[] = {{0, 0, 0}, {100, 0, 0}, {200, 0, 0}};
    fleetrail::Path line;
    Check(line.Set(straight, 3) && std::isinf(line.CornerSpeed(50)), "no corner cap on a straight");
    constexpr FvoFleetRailPoint bend[] = {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}};
    fleetrail::Path corner;
    Check(corner.Set(bend, 3), "accept a right-angle route");
    const double atCorner = corner.CornerSpeed(100), before = corner.CornerSpeed(60), far = corner.CornerSpeed(10);
    Check(atCorner < 10, "slow at a sharp corner");
    Check(before > atCorner && far > before, "corner cap relaxes with braking distance");
    Check(std::isinf(far) || far > 15, "a corner beyond the look window does not slow the car");

    if (failures) return 1;
    std::printf("PASS fleet rail soft switching, reference mode rules and path policy (%d checks)\n", checks);
    return 0;
}
