#pragma once
#include "aircontrol.h"
#include <array>
namespace airtraffic {
inline constexpr const char* kPlugin="FVO_AirTraffic.esm";
inline std::array<aircontrol::Mailbox,airtrafficmath::kFleet> controls;
inline std::atomic<std::uint64_t> epoch{1},nativeSteps{0},nativeRejects{0};
inline std::array<std::atomic<std::uint64_t>,5> nativeRejectReasons{};
void Tick();
void Reset();
bool IsVehicle(std::uint32_t form);
bool OwnPair(std::uintptr_t actor,std::uintptr_t vehicle);
}
