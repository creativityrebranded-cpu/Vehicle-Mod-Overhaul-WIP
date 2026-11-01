#pragma once
#include "fvopaths.h"

namespace trafficdiagnostics {
// Read once per process: the attach hook and frame guard must use the same switch.
inline bool ModelGuardEnabled()
{
    static const bool enabled = GetPrivateProfileIntW(L"traffic", L"modelGuard", 1, fvopaths::Settings().c_str()) != 0;
    return enabled;
}
}
