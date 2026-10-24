#pragma once
#include <cstdint>

namespace vehiclecamera
{
    struct Attitude { std::uintptr_t vehicle = 0; double pitch = 0, roll = 0; };
    bool Install();
    // Main thread publishes a physics snapshot; neither render callback reads the Havok body.
    void Publish(std::uintptr_t vehicle, double pitch, double roll);
    void Reset();
    bool ReadAttitude(Attitude& out); // false when stopped, stale, faulted or a different vehicle is driven
	void LogStatus(); // main thread: bounded detailed-log diagnostics, independent of Free Roll/bank-test settings
}
