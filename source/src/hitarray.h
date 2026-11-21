#pragma once
// PF-1 (2026-11-12, freeze 13:11: a Havok broadphase rebuild spun forever in the game allocator's out-of-memory retry): the hits a
// Havok all-hits collector really holds. We hand Havok 10 inline slots (capacity word 0x8000000A, sign bit = not owned). Past 10 hits
// Havok grows the array onto the game's Havok heap (hkArrayUtil reserve) and stores the new pointer at +0x30 with an owned capacity.
// The forward ray and the hull sweep read only the 10 inline slots and never freed the grown array: a leak on the same heap that ran
// dry, and stale hits. QueryBoxNow/FreeHits already did it right. Pure (no engine calls); tests: tests/hitarray_test.cpp.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace hitarray {
inline constexpr std::uint32_t kInlineCapacity = 10, kNotOwned = 0x80000000u, kCapacityMask = 0x3FFFFFFFu, kHitBytes = 0x70;
inline constexpr std::uint32_t kMaxRead = 4096;  // never walk more than this, whatever the size word says

struct View {
    std::uint8_t* data = nullptr;  // where the hits are (the inline buffer, or Havok's grown array)
    std::uint32_t count = 0;       // hits that may be read
    std::uint32_t capacity = 0;    // the raw capacity word (for the free)
    bool owned = false;            // Havok's heap array: must be freed with the capacity
};

// collector: hknpAllHitsCollector as laid out in vehicle_probe.cpp (+0xC hit count, +0x30 array, +0x38 size, +0x3C capacity word).
inline View Read(const std::uint8_t* collector, const std::uint8_t* inlineBuffer) {
    View v;
    std::memcpy(&v.data, collector + 0x30, sizeof v.data);
    std::uint32_t count = 0, size = 0;
    std::memcpy(&count, collector + 0xC, 4); std::memcpy(&size, collector + 0x38, 4); std::memcpy(&v.capacity, collector + 0x3C, 4);
    if (!v.data) return v;
    v.owned = v.data != inlineBuffer && (v.capacity & kNotOwned) == 0;
    const std::uint32_t room = v.data == inlineBuffer ? kInlineCapacity : std::min(v.capacity & kCapacityMask, kMaxRead);
    v.count = std::min(std::max(count, size), room);
    return v;
}

// Counters for the probe.hits trace (all casters together).
struct Stats {
    std::atomic<std::uint64_t> casts{ 0 }, grown{ 0 }, freedBytes{ 0 }, droppedOnFault{ 0 };
    std::atomic<std::uint32_t> largest{ 0 };
    void Note(const View& v) {
        ++casts;
        if (v.owned) { ++grown; freedBytes += std::uint64_t(v.capacity & kCapacityMask) * kHitBytes; }
        std::uint32_t was = largest.load();
        while (v.count > was && !largest.compare_exchange_weak(was, v.count)) {}
    }
};
}
