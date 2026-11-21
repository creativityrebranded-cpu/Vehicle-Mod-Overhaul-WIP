#pragma once
#include <cstddef>
#include <cstdint>

namespace slotcookie {
// The callback receives the address of an entry in a fixed array. Reject null, foreign
// and interior addresses before using the matching slot. The caller owns the slot lock.
template<class Slot, std::size_t Count>
Slot* Find(Slot (&slots)[Count], const void* cookie) {
    const auto address = reinterpret_cast<std::uintptr_t>(cookie);
    const auto start = reinterpret_cast<std::uintptr_t>(slots);
    if (address < start) return nullptr;
    const auto offset = address - start;
    if (offset >= sizeof slots || offset % sizeof(Slot) != 0) return nullptr;
    Slot* slot = &slots[offset / sizeof(Slot)];
    return slot == cookie ? slot : nullptr;
}
}
