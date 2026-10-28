#pragma once
#include "bodybinding.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>

// Physics-thread observations, consumed without invoking a native physics visitor
// on the frame thread. No pointer survives the callback. Contention skips a
// sample rather than making either thread wait. Full body generations, a load
// epoch and a short lifetime prevent a recycled chassis from inheriting mass.
namespace bodymass {
class Cache {
    struct Sample { std::uint32_t body=0; float mass=0; std::uint64_t at=0,epoch=0; };
    std::array<Sample,256> samples_{};
    std::atomic_flag busy_=ATOMIC_FLAG_INIT;
    std::atomic<std::uint64_t> epoch_{1};
    struct Release { std::atomic_flag& flag; ~Release(){flag.clear(std::memory_order_release);} };
public:
    std::uint64_t Epoch() const {return epoch_.load(std::memory_order_acquire);}
    void Reset(){epoch_.fetch_add(1,std::memory_order_acq_rel);}
    bool Observe(std::uint32_t body,float mass,std::uint64_t now,std::uint64_t epoch) {
        if(!bodybinding::Valid(body)||!std::isfinite(mass)||mass<=0.01f||mass>=100000000.0f||
            busy_.test_and_set(std::memory_order_acquire))return false;
        Release release{busy_};
        if(epoch!=Epoch())return false;
        auto& sample=samples_[body% samples_.size()];
        // A hash collision can replace a measurement; lookup never accepts the
        // colliding identity and uses the existing geometric fallback instead.
        sample={body,mass,now,epoch};return true;
    }
    float Read(std::span<const std::uint32_t> bodies,std::uint64_t now) {
        if(busy_.test_and_set(std::memory_order_acquire))return 0;
        Release release{busy_};const auto epoch=Epoch();
        for(const auto body:bodies) {
            const auto& sample=samples_[body%samples_.size()];
            if(bodybinding::Valid(body)&&sample.body==body&&sample.epoch==epoch&&
                now>=sample.at&&now-sample.at<=2000)return sample.mass;
        }
        return 0;
    }
};
inline Cache observations;
}
