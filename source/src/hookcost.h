#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

// Bounded telemetry: physics callbacks never wait for a reporter or allocate.
namespace hookcost {
template<std::size_t Capacity=1024> class KeyedCounter {
    struct Entry {std::atomic<std::uintptr_t> key{0};std::atomic<unsigned> count{0};};
    std::array<Entry,Capacity> entries_{};
    std::atomic<unsigned> skipped_{0};
public:
    // Entries retain identity across report windows. A full table drops only
    // telemetry; gameplay never depends on these counters.
    void Note(std::uintptr_t key) {
        if(!key)return;
        const auto hash=(key>>4)^(key>>20);
        for(std::size_t probe=0;probe<std::min<std::size_t>(Capacity,16);++probe) {
            auto& entry=entries_[(hash+probe)%Capacity];auto found=entry.key.load(std::memory_order_relaxed);
            if(!found){entry.key.compare_exchange_strong(found,key,std::memory_order_relaxed);found=entry.key.load(std::memory_order_relaxed);}
            if(found==key){entry.count.fetch_add(1,std::memory_order_relaxed);return;}
        }
        skipped_.fetch_add(1,std::memory_order_relaxed);
    }
    template<class Visitor> unsigned Drain(Visitor visit) {
        for(auto& entry:entries_) {
            const auto key=entry.key.load(std::memory_order_relaxed);
            if(key)if(const auto count=entry.count.exchange(0,std::memory_order_relaxed))visit(key,count);
        }
        return skipped_.exchange(0,std::memory_order_relaxed);
    }
};
struct Samples {
    std::uint64_t count=0,hook=0,native=0,maxHook=0,maxNative=0,skipped=0;
};
class Counter {
    std::atomic_flag busy_=ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> skipped_{0};
    Samples samples_{};
public:
    void Add(std::uint64_t hook,std::uint64_t native) {
        if(busy_.test_and_set(std::memory_order_acquire)){skipped_.fetch_add(1,std::memory_order_relaxed);return;}
        ++samples_.count;samples_.hook+=hook;samples_.native+=native;
        samples_.maxHook=std::max(samples_.maxHook,hook);samples_.maxNative=std::max(samples_.maxNative,native);
        busy_.clear(std::memory_order_release);
    }
    bool Take(Samples& out) {
        if(busy_.test_and_set(std::memory_order_acquire))return false;
        out=samples_;samples_={};out.skipped=skipped_.exchange(0,std::memory_order_relaxed);
        busy_.clear(std::memory_order_release);return true;
    }
};
}
