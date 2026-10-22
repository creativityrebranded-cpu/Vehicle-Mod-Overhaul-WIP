#pragma once
#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace physicssetup
{
    constexpr std::uint32_t kBuilderReturn = 0x0131B85C;
    struct Chassis {
        std::uintptr_t read = 0, write = 0;
        std::uint32_t id = 0xFFFFFF, flags = 0;
    };

    // Never turn a missing-world/invalid-body result into permission to build.
    // The native predicate must confirm that the actual motion change succeeded.
    template<class Inspect, class SetDynamic, class IsStatic>
    bool Reject(std::uintptr_t caller, bool rejected, Inspect inspect, SetDynamic setDynamic, IsStatic isStatic)
    {
        if (caller != kBuilderReturn || !rejected) return rejected;
        const Chassis c = inspect();
        if (!c.read || !c.write || c.read < 0x20 || c.write < 0x18 ||
            c.read - 0x20 != c.write - 0x18 || (c.id & 0xFFFFFF) == 0xFFFFFF ||
            (c.flags & 3) != 1) return true;
        setDynamic(c);
        return isStatic();
    }

    // Launch guard (2026-10-21 22:19 run): parked vehicles whose physics came up at the end of the load screen were thrown
    // (FE008802 ~300 m on a ballistic arc in 14 s, FF01CF4E from z -162 to 174): the chassis started inside collision that
    // streamed in around it and the solver pushed it out. For a while after a parked vehicle's first physics step, a push
    // faster than this is a launch, not driving or settling: horizontal speed is removed and upward speed stopped
    // (falling is left alone), the same answer as vehicleflight's release guard.
    constexpr float kLaunchHorizontal = 4.0f;  // m/s
    constexpr float kLaunchUp = 2.0f;          // m/s
    constexpr unsigned long long kLaunchGuardMs = 10000;

    // v: the chassis linear velocity (x, y, z up). True when it was a launch and was clamped.
    inline bool ClampLaunch(float* v)
    {
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
        bool hit = false;
        if (std::hypot(v[0], v[1]) > kLaunchHorizontal) { v[0] = 0.0f; v[1] = 0.0f; hit = true; }
        if (v[2] > kLaunchUp) { v[2] = 0.0f; hit = true; }
        return hit;
    }

    // The game's pending-vehicle list (2026-10-21 23:1x, root cause of "most vehicles never get physics"):
    // VehicleRuntimeComponent's VehicleEventListener (0x145979AF0) keeps the vehicles whose 3D has not loaded yet in an
    // inline array of 8 form ids (+0x20, count +0x40). Its adder FUN_141313400 empties the whole array when it already
    // holds more than 7, and the 3D-loaded handler FUN_1413161E0 creates the component and queues the attach only for
    // ids still listed (FUN_141316140). Twenty-two vehicles loading together in New Atlantis left six. This set shadows
    // the array without a limit: the adder and remover keep it, the membership test answers native || listed here.
    class PendingSet
    {
    public:
        void Add(std::uint32_t id) { if (!id) return; std::lock_guard lk(m_); ids_.insert(id); }
        void Remove(std::uint32_t id) { std::lock_guard lk(m_); ids_.erase(id); }
        bool Has(std::uint32_t id) const { std::lock_guard lk(m_); return ids_.count(id) != 0; }
        std::size_t Size() const { std::lock_guard lk(m_); return ids_.size(); }
        // The membership answer for the game: its own, else ours. `rescued` is true when only ours had it.
        bool Member(bool native, std::uint32_t id, bool& rescued) const
        {
            rescued = false;
            if (native) return true;
            rescued = Has(id);
            return rescued;
        }
    private:
        mutable std::mutex m_;
        std::unordered_set<std::uint32_t> ids_;
    };

    // The guard window opens at the first physics step (instances built during a load screen step only when it ends).
    // until: 0 = not stepped yet. Returns true while the window is open.
    inline bool GuardOpen(unsigned long long& until, unsigned long long now)
    {
        if (!until) until = now + kLaunchGuardMs;
        return now < until;
    }
}
