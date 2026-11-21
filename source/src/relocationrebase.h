#pragma once
// Relocation consumers, pure part (2026-11-20, notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md, "Who was expecting an answer").
// The Coordinator publishes ONE receipt after a seated pair is moved. Every consumer below Plan/World/Route/Keep-in-sight/Spot test re-bases
// on an APPLIED receipt, at most once per serial, only for its own vehicle, and never on a receipt from an older epoch. A consumer never
// decides about the move. Nothing here calls the engine, a clock or the Coordinator: the source is a function the caller passes in
// (relocationconsumers.h supplies the core's own export), so the logic is testable offline (tests/relocationrebase_test.cpp).
// Threads: Board is mutex-guarded; Refresh must run where the Coordinator may be read (the game's main thread), Latest may run anywhere.
#include "relocationapi.h"
#include <cstdint>
#include <mutex>

namespace relocrebase {

// One applied move, as consumers see it.
struct Event {
    std::uint32_t vehicle = 0;
    std::uint64_t serial = 0, epoch = 0, stepAfter = 0;
    double to[3] = {};
    bool valid() const { return vehicle && serial; }
};

// What one consumer remembers about the last move it applied, for ONE vehicle.
struct Seen {
    std::uint32_t vehicle = 0;
    std::uint64_t epoch = 0, serial = 0;
};

enum class Verdict { None, Apply, Duplicate, StaleEpoch, OtherVehicle };
inline const char* Name(Verdict v) {
    switch (v) { case Verdict::Apply: return "apply"; case Verdict::Duplicate: return "duplicate"; case Verdict::StaleEpoch: return "stale_epoch";
                 case Verdict::OtherVehicle: return "other_vehicle"; default: return "none"; }
}

// Would `ev` be applied for `vehicle`? Does not change `seen`.
inline Verdict Judge(const Seen& seen, const Event& ev, std::uint32_t vehicle) {
    if (!ev.valid()) return Verdict::None;
    if (!vehicle || ev.vehicle != vehicle) return Verdict::OtherVehicle;
    if (seen.vehicle == vehicle) {
        if (ev.epoch < seen.epoch) return Verdict::StaleEpoch;       // a receipt of an earlier save / world generation
        if (ev.epoch == seen.epoch && ev.serial <= seen.serial) return Verdict::Duplicate;   // applied already, or older than what was applied
    }
    return Verdict::Apply;                                            // a new vehicle for this record, a newer epoch (serials restart) or a newer serial
}
// Judge, and on Apply remember it. True exactly once per serial.
inline bool Take(Seen& seen, const Event& ev, std::uint32_t vehicle) {
    if (Judge(seen, ev, vehicle) != Verdict::Apply) return false;
    seen = Seen{ vehicle, ev.epoch, ev.serial };
    return true;
}

// An applied receipt out of the core's status answer (relocationapi.h). result 1 = applied; anything else is not a move to re-base on.
inline bool FromStatus(std::uint32_t vehicle, const FvoRelocationStatusV1& st, Event& out) {
    if (!vehicle || st.result != 1 || !st.serial) return false;
    out = Event{ vehicle, st.serial, st.epoch, st.stepAfter, { st.to[0], st.to[1], st.to[2] } };
    return true;
}

using SourceFn = int (*)(unsigned vehicle, FvoRelocationStatusV1* out);

// The latest applied move per vehicle, copied out of the Coordinator's answer so any thread can read it. Small and fixed: the oldest slot is
// reused when it is full (a consumer that missed a serial that long ago has long since moved on).
class Board {
public:
    static constexpr int kSlots = 64;
    // Poll the source for `vehicle` (game thread). Returns true when this call stored a NEWER applied move than the board held.
    bool Refresh(std::uint32_t vehicle, SourceFn source) {
        if (!vehicle || !source) return false;
        FvoRelocationStatusV1 st{};
        if (!source(vehicle, &st)) return false;
        Event ev;
        if (!FromStatus(vehicle, st, ev)) return false;
        std::lock_guard lk(m_);
        for (auto& e : slot_) if (e.vehicle == vehicle) {
            if (ev.epoch < e.epoch || (ev.epoch == e.epoch && ev.serial <= e.serial)) return false;
            e = ev; return true;
        }
        for (auto& e : slot_) if (!e.vehicle) { e = ev; return true; }
        slot_[next_++ % kSlots] = ev; return true;
    }
    bool Latest(std::uint32_t vehicle, Event& out) const {
        if (!vehicle) return false;
        std::lock_guard lk(m_);
        for (const auto& e : slot_) if (e.vehicle == vehicle) { out = e; return true; }
        return false;
    }
    void Clear() { std::lock_guard lk(m_); for (auto& e : slot_) e = Event{}; next_ = 0; }
private:
    mutable std::mutex m_;
    Event slot_[kSlots]{};
    unsigned next_ = 0;
};

}  // namespace relocrebase
