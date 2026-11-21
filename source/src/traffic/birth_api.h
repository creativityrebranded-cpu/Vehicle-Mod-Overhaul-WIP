#pragma once
#include <cstdint>

inline constexpr unsigned kFvoTrafficDriverBase = 0x001591EA; // Starfield.esm LvlCitizen_UC.

// Optional extension. The original createVehicle / createSeatedDriver table entries stay callable.
// The numbering of both enums crosses the DLL boundary: never renumber or remove a value.
// FvoBirthSource::FactoryStock and FvoBirthWindow::FactoryConcealed are reserved: the factory stock path is gone and neither is produced any more.
enum class FvoBirthSource : unsigned { Legacy, Fleet, DriverRepair, FactoryStock /* reserved, no longer produced */ };
enum class FvoBirthWindow : unsigned {
    Closed, Loading, Panel, Diagnostic,
    FactoryConcealed,  // reserved, no longer produced
    FleetShortfall   // Matched physical fleet grant. V2 requires a current road receipt for a chassis.
};
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
// 2026-11-21 (DW-2): the core's own reason when it refused a birth, so the add-on logs it in its own line instead of pointing at another one. The core
// exports FVO_TrafficBirthRefusalV1(grant, slot, out); the add-on looks it up by name and logs no core reason when it is absent. stage: "request", "service", "sited_proof", "engine".
struct FvoBirthRefusalV1 {
    unsigned size;                 // sizeof(FvoBirthRefusalV1), set by the caller
    unsigned base;
    std::uint64_t grant, atMs;     // the ticket the refusal belongs to; GetTickCount64 when it was recorded
    int slot;
    char stage[16];
    char reason[48];
};
using FvoBirthRefusalFnV1 = int (*)(std::uint64_t grant, int slot, FvoBirthRefusalV1* out);
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
    case FvoBirthSource::FactoryStock: return "factory_stock";  // reserved: still named so old logs and values read
    default: return "legacy_api";
    }
}
