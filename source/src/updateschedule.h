#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

// A producer announces completed native updates; a main-thread consumer admits
// at most one full update for each newly observed serial, with a bounded clock fallback and one bootstrap
// after reset. Missed serials are coalesced, never queued or replayed.
namespace updateschedule {
static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
    "Native update pulses must not use an internal mutex on this target");
class Pulse {
    std::atomic<std::uint64_t> serial_{0};
public:
    void Post(){serial_.fetch_add(1,std::memory_order_release);}
    std::uint64_t Serial()const{return serial_.load(std::memory_order_acquire);}
};
enum class Work { Skip, Housekeeping, Full };
struct Counts {std::uint64_t polls=0,full=0,housekeeping=0,skipped=0,coalesced=0,fallback=0;};
class Gate {
    bool started_=false;
    std::uint64_t serial_=0,observed_=0;
    double last_=0,nextIdle_=0,lastNative_=0,nextFull_=0,nextFallback_=0;
    Counts counts_{};
    static constexpr double kIdleMs=1000.0/60.0;
    static constexpr double kMinimumFullMs=1000.0/240.0;
    static constexpr double kNativeTimeoutMs=100.0;
public:
    Work Poll(double nowMs,std::uint64_t serial) {
        ++counts_.polls;
        if(!std::isfinite(nowMs)){++counts_.skipped;return Work::Skip;}
        const bool initial=!started_;
        if(initial||nowMs<last_){lastNative_=nowMs;nextFull_=nowMs;nextFallback_=nowMs+kIdleMs;nextIdle_=nowMs;}
        if(serial!=observed_){observed_=serial;lastNative_=nowMs;}
        last_=nowMs;
        const bool native=serial!=serial_;
        const bool fallback=nowMs-lastNative_>=kNativeTimeoutMs&&nowMs>=nextFallback_;
        if(initial||(nowMs>=nextFull_&&(native||fallback))) {
            if(!initial&&native&&serial>serial_&&serial-serial_>1)counts_.coalesced+=serial-serial_-1;
            if(!initial&&!native)++counts_.fallback;
            started_=true;serial_=serial;nextIdle_=nowMs+kIdleMs;
            nextFull_=nowMs+kMinimumFullMs;nextFallback_=nowMs+kIdleMs;
            ++counts_.full;return Work::Full;
        }
        if(nowMs>=nextIdle_) {
            nextIdle_=nowMs+kIdleMs;++counts_.housekeeping;return Work::Housekeeping;
        }
        ++counts_.skipped;return Work::Skip;
    }
    Counts TakeCounts(){const auto out=counts_;counts_={};return out;}
    void Reset(){*this=Gate{};}
};
}
