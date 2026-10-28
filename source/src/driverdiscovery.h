#pragma once
#include <cstdint>

namespace driverdiscovery {
// Only background discovery is throttled. Existing entries are validated when
// consumed, and the player and managed jobs are refreshed on every update.
class Cadence {
    std::uint64_t last_ = 0;
    bool started_ = false;
public:
    bool Due(std::uint64_t now) {
        if (started_ && now >= last_ && now - last_ < 250) return false;
        started_ = true;
        last_ = now;
        return true;
    }
    void Reset() { started_ = false; last_ = 0; }
};
}
