#pragma once
#include <cstdint>

// Optional extension. The original createVehicle / createSeatedDriver table entries stay callable.
enum class FvoBirthSource : unsigned { Legacy, Fleet, DriverRepair, FactoryStock };
enum class FvoBirthWindow : unsigned { Closed, Loading, Panel, Diagnostic };
struct FvoTrafficBirthV1 {
    FvoBirthSource source;
    FvoBirthWindow window;
    int start;  // 0 collects this exact ticket and can never create a new one
    std::uint64_t grant;
    int slot;
    unsigned base, vehicle;  // zero vehicle means chassis; otherwise create the driver for this vehicle
    double x, y, z, heading;
};
enum class FvoBirthState : int { Missing, Pending, Completed };
using FvoTrafficBirthFnV1 = unsigned (*)(const FvoTrafficBirthV1*, FvoBirthState*);

struct FvoRoadSiteV1 { double x, y, z, heading, radius, halfHeight; };
struct FvoRoadProofV1 { FvoRoadSiteV1 site; std::uint64_t epoch, at; };
enum class FvoRoadClearance : int { Unknown, Occupied, Clear };
using FvoRoadCheckFnV1 = FvoRoadClearance (*)(const FvoRoadSiteV1*, FvoRoadProofV1*);
using FvoTrafficBirthFnV2 = unsigned (*)(const FvoTrafficBirthV1*, const FvoRoadProofV1*, FvoBirthState*);

inline const char* BirthSourceName(FvoBirthSource source) {
    switch (source) {
    case FvoBirthSource::Fleet: return "fleet";
    case FvoBirthSource::DriverRepair: return "driver_repair";
    case FvoBirthSource::FactoryStock: return "factory_stock";
    default: return "legacy_api";
    }
}
