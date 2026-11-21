#pragma once
// Contact events (notes/VEHICLE_DECISION_DESIGN_20261106.md, graph node "Contact Event"; register G6; work order step 6): one event per
// contact, published once, read by every listener: World (a shape with an expiry, so the next branches go round it) and Progress (stuck
// evidence). Health is a listener by count only: FVO has no crash damage today, and adding it is a gameplay decision for the user.
// Pure; tests: tests/contactevent_test.cpp.
#include <cstdint>

namespace contactevent {
enum class Kind : std::uint8_t { Push, Touch };   // Push: asked to move, pushing, not moving (what the rays missed is at the nose)
inline const char* Name(Kind k) { return k == Kind::Push ? "push" : "touch"; }
struct Event {
    std::uint32_t vehicle = 0; Kind kind = Kind::Push;
    double x = 0, y = 0, z = 0, heading = 0, halfWidth = .5;   // where, and how wide (the car's nose)
    double at = 0, expires = 0;
};
template <int N = 64>
struct Bus {
    Event e[N]; unsigned next = 0, published = 0;
    void Publish(const Event& ev) { e[next++ % N] = ev; ++published; }
    // Live events for one vehicle, newest first. Returns how many were written.
    int For(std::uint32_t vehicle, double now, Event* out, int max) const {
        int n = 0;
        for (unsigned k = 0; k < unsigned(N) && n < max; ++k) {
            const Event& v = e[(next + N - 1 - k) % N];
            if (v.vehicle == vehicle && v.vehicle != 0 && now >= v.at && now < v.expires) out[n++] = v;
        }
        return n;
    }
    // Health and the ledger: how many contacts this vehicle had since `since`.
    int Count(std::uint32_t vehicle, double since) const {
        int n = 0; for (const auto& v : e) if (v.vehicle == vehicle && v.vehicle != 0 && v.at >= since) ++n; return n;
    }
    void Reset() { for (auto& v : e) v = Event{}; next = published = 0; }
};
}
