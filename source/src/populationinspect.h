#pragma once
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>
extern "C" {
struct FvoPopulationInspectV1 {std::uint32_t size=sizeof(FvoPopulationInspectV1),version=1,vehicle=0,driver=0;int state=0,retired=0,staged=0,armed=0,vehiclePending=0,driverPending=0;std::uint64_t at=0;char name[192]{},reason[192]{};};
using FvoPopulationQueryV1=int (*)(int,FvoPopulationInspectV1*);
}
namespace populationinspect {inline std::mutex mutex;inline std::vector<FvoPopulationInspectV1> rows;inline void Publish(std::vector<FvoPopulationInspectV1> value){std::lock_guard lock(mutex);rows=std::move(value);}inline bool Read(int index,FvoPopulationInspectV1& out){std::lock_guard lock(mutex);if(index<0||index>=int(rows.size()))return false;out=rows[index];return true;}}

namespace populationinspect {
inline bool Surplus(unsigned vehicle) {
    if(!vehicle)return false;
    std::lock_guard lock(mutex);
    for(const auto& row:rows)if(row.vehicle==vehicle&&row.retired)return true;
    return false;
}
}
