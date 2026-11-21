#pragma once
// Relocation status for the Traffic add-on (2026-11-20, notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md).
// Optional export of the diagnostic core: `int FVO_TrafficRelocationV1(unsigned vehicle, FvoRelocationStatusV1* out)`. An older core has no such
// export and an older add-on never asks, so neither side needs the other to change. Versioned and additive: a new field is appended.
// Main thread only (the add-on's Health runs on the game's frame task, as does the Coordinator).
#include <cstdint>

struct FvoRelocationStatusV1 {
    std::uint32_t size = sizeof(FvoRelocationStatusV1), version = 1;
    int relocating = 0;               // 1: a grant or an issued substitution is waiting for its receipt (no verdict from distance or descent)
    int result = 0;                   // the vehicle's latest receipt: 0 none, 1 applied, 2 refused, 3 failed, 4 restored
    std::uint64_t serial = 0, epoch = 0, stepAfter = 0;
    double to[3] = {};                // where the latest receipt says the chassis is (the new fall baseline when result == 1)
};
using FvoRelocationStatusFn = int (*)(unsigned vehicle, FvoRelocationStatusV1* out);   // 1: the vehicle has a status, 0: nothing known
