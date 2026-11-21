#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>

namespace handoff {
enum class Phase { Seat, Package, Binding, Release, Physics, Driving };

struct Progress {
    Phase phase = Phase::Seat;
    std::uint64_t beforeRelease = 0;

    Phase Observe(bool seated, bool package, bool bound, bool staged,
                  bool sampled, bool recentStep, std::uint64_t step) {
        if (!seated) return phase = Phase::Seat;
        if (!package) return phase = Phase::Package;
        if (!bound || !sampled) return phase = Phase::Binding;
        if (staged) return phase = Phase::Release;
        return phase = recentStep && step != beforeRelease ? Phase::Driving : Phase::Physics;
    }
};

struct Facts {
    int package = 0, bound = 0, sampled = 0, recentStep = 0;
    std::uint64_t step = 0;
};
using Release = int (*)(unsigned, bool);
using ReadFacts = void (*)(unsigned, unsigned, Facts*);
using RetireShow = void (*)(unsigned, unsigned, bool, bool);

// Generation for revocable wheel handoffs. Zero denotes a command that carries
// no revocable handoff. A refused admission can only clear a mailbox.
inline std::atomic<std::uint64_t> wheelGeneration{1};
struct WheelAdmission {
    bool allowed = true;
    std::uint64_t generation = 0;
};
inline bool WheelGrantCurrent(std::uint64_t generation) {
    return !generation || generation == wheelGeneration.load();
}

// Atomic fields make unsuccessful reads race-free. Writers serialize publication;
// the native reader uses two attempts and takes no mailbox lock.
template<class Input>
class WheelMailbox {
    std::mutex writers;
    std::atomic<std::uint64_t> version{0};
    std::atomic<unsigned> vehicle{0};
    std::atomic<float> steer{0}, accel{0}, brake{0};
    std::atomic<bool> boost{false}, vert{false};
    std::atomic<std::uint64_t> authority{0};
public:
    bool Active() const { return vehicle.load(std::memory_order_relaxed) != 0; }

    void Publish(unsigned id, const Input& input, std::uint64_t generation) {
        std::lock_guard lock(writers);
        ++version;
        steer.store(input.steer);
        accel.store(input.accel);
        brake.store(input.brake);
        boost.store(input.boost);
        vert.store(input.vert);
        authority.store(generation);
        vehicle.store(id);
        ++version;
    }

    void PublishAdmitted(unsigned id, const Input& input, WheelAdmission admission) {
        if (admission.allowed && WheelGrantCurrent(admission.generation)) Publish(id, input, admission.generation);
        else Clear();
    }

    void Clear() {
        std::lock_guard lock(writers);
        ++version;
        vehicle.store(0);
        ++version;
    }

    bool Read(unsigned id, Input& out, std::uint64_t& generation) const {
        if (vehicle.load(std::memory_order_relaxed) != id) return false;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const auto before = version.load();
            if (before & 1) continue;
            const auto owner = vehicle.load();
            const Input value{steer.load(), accel.load(), brake.load(), boost.load(), vert.load()};
            const auto grant = authority.load();
            if (before != version.load()) continue;
            if (owner != id) return false;
            out = value;
            generation = grant;
            return true;
        }
        return false;
    }
};
}
