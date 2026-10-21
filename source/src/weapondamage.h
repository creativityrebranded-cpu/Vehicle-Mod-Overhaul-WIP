#pragma once

#include <cmath>

// Numeric policy shared by the projectile follower and offline tests. A native
// zero is a real zero (nonlethal/disabled projectile), never the fallback hit.
namespace weapondamage
{
    inline bool Valid(float damage)
    {
        return std::isfinite(damage) && damage >= 0.0f && damage <= 1000000.0f;
    }

    inline float Scale(float value)
    {
        return std::isfinite(value) && value >= 0.0f && value <= 1000.0f ? value : 1.0f;
    }

    struct Result { float damage; bool native; };
    inline Result Resolve(bool available, float nativeDamage, float scale, float fallback)
    {
        const double scaled = double(nativeDamage) * Scale(scale);
        if (available && Valid(nativeDamage) && std::isfinite(scaled) && scaled <= 1000000.0)
            return { float(scaled), true };
        return { Valid(fallback) ? fallback : 20.0f, false };
    }
}
