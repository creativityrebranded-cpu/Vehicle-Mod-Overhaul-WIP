#pragma once
#include "framework.h"
#include <cstdint>
namespace vehiclelinks {
using Visitor=void(*)(std::uintptr_t,void*);
// Composition supplies these services; the snapshot consumer does not know
// which registry, flight controller or NPC/seat implementation owns them.
struct Services {
    bool (*scan)(Visitor,void*,framework::ScanCursor&)=nullptr;
    std::uint32_t (*owned)()=nullptr;
    std::uint32_t (*driver)(std::uint32_t)=nullptr;
    bool (*inUse)(std::uint32_t)=nullptr;
    constexpr explicit operator bool() const {return scan&&owned&&driver&&inUse;}
};
}
