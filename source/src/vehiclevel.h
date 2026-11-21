#pragma once
// VS-1 (2026-11-07): World's velocity from position differences, believed only when physically possible and fresh. Owner: World
// (vehicleworld.cpp). Pure: tests/vehiclevel_test.cpp. Cars we fly keep their body's own velocity first (VL-1, unchanged).
#include <algorithm>
#include <cmath>
namespace vehiclevel
{
    namespace rules {
        inline constexpr double maxBelievable = 60.0;  // m/s a step may imply; more is a jump (teleport, summon, reload, a placement move)
        inline constexpr double staleAfter = 0.5;      // s between samples; longer: the old speed is not carried on and the step is not a speed
        inline constexpr double resetJump = 50.0;      // m: the existing coarse reset (kept)
        inline constexpr double gain = 8.0;            // the existing smoothing: share = min(1, dt * gain)
    }
    enum class Event { None, Updated, JumpIgnored, Stale, Reset };
    inline const char* Name(Event e) { constexpr const char* n[]{ "none", "updated", "jump_ignored", "stale", "reset" }; return n[int(e)]; }
    // vel: the running estimate (updated); delta: position change since the last sample (m); dt: seconds since it.
    inline Event Step(float vel[3], const float delta[3], double dt, double* implied = nullptr) {
        const double jump = std::hypot(double(delta[0]), double(delta[1]));
        if (implied) *implied = dt > 1e-6 ? jump / dt : 0;
        if (jump >= rules::resetJump) { vel[0] = vel[1] = vel[2] = 0; return Event::Reset; }
        if (dt <= 1e-4) return Event::None;
        if (dt >= rules::staleAfter) { vel[0] = vel[1] = vel[2] = 0; return Event::Stale; }
        if (jump / dt > rules::maxBelievable) { vel[0] = vel[1] = vel[2] = 0; return Event::JumpIgnored; }
        const float a = float(std::min(1.0, dt * rules::gain));
        for (int k = 0; k < 3; ++k) vel[k] += (float(delta[k] / dt) - vel[k]) * a;
        return Event::Updated;
    }
}
