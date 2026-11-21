#pragma once
// Additive addon-to-core Census copy. Census owns lifecycle; core readers consume control.
// No engine layout or existing traffic API table changes. Version/size gate every call.
#include <cstdint>
struct FvoTrafficCensusV1 {
    std::uint32_t size = sizeof(FvoTrafficCensusV1), version = 1;
    std::uint32_t vehicle = 0, driver = 0, model = 0;
    int slot = -1;
    // 0 none, 1 born, 2 proving, 3 driving, 4 recovering, 5 broken, 6 retiring.
    int life = 0;
    // 0 held, 1 driving pipeline, 2 world's (dead/taken/fighting/out), 3 retire.
    int control = 0;
    std::uint64_t at = 0;
    char reason[96]{};
};
// Returns 1 accepted, 2 stronger owner (traffic must give the pair to the world), 0 invalid/unavailable.
using FvoTrafficCensusPublishV1 = int (*)(const FvoTrafficCensusV1*);
// Forget only the traffic metadata; never end or remove a stronger owner's job.
using FvoTrafficCensusForgetV1 = void (*)(std::uint32_t vehicle);
