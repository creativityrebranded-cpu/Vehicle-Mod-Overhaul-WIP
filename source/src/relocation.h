#pragma once
// Relocation: the doer's record, the receipt and the communicator's registry (2026-11-20, notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md).
// Pure logic: no engine calls, no clock of its own, no threads. Main thread only; the physics-thread half of the doer lives in vehicleflight.cpp
// and hands its facts to this file through Verify().
//
// Roles (VEHICLE_DECISION_DESIGN_20261106.md): the Population Arbiter requests, Vehicle Coordinator Handoff grants (Arm), the doer issues one
// pose substitution and later reads back (Issued / Verify), and this Coordinator publishes ONE receipt that every consumer applies at most once.
// A consumer never decides about the move; it only re-bases on the receipt. The receipt is never a second physics write.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace relocation {

enum class Result : std::uint8_t { None = 0, Applied, Refused, Failed, Restored };
inline const char* ResultName(Result r) {
    switch (r) { case Result::Applied: return "applied"; case Result::Refused: return "refused"; case Result::Failed: return "failed"; case Result::Restored: return "restored"; default: return "none"; }
}
enum class Phase : std::uint8_t { Idle = 0, Granted, Issued, Published };
inline const char* PhaseName(Phase p) {
    switch (p) { case Phase::Granted: return "granted"; case Phase::Issued: return "issued"; case Phase::Published: return "published"; default: return "idle"; }
}

struct Pose { double x = 0, y = 0, z = 0, yaw = 0; };
inline double Distance(const Pose& a, const Pose& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }

// What the Arbiter asked for and Handoff granted.
struct Request {
    std::uint32_t vehicle = 0, driver = 0;
    Pose from, to;
    std::uint64_t epoch = 0;          // the save / world generation the request belongs to
    std::uint64_t stepBefore = 0;     // the pair's fresh-step serial when the request was granted
    bool seatedBefore = false;
    bool crossCell = false;           // the destination is in another grid cell than the one the chassis is listed in (appended, 2026-11-20)
};

// The one message every consumer gets. Versioned and additive: a new field is appended, an old one is never reused.
struct Receipt {
    std::uint32_t version = 1;
    std::uint64_t serial = 0, epoch = 0;
    std::uint32_t vehicle = 0, driver = 0;
    Pose from, to, measured;          // measured: the chassis where the read-back found it
    double displacement = 0;
    std::uint64_t stepBefore = 0, stepAfter = 0;
    bool seatedBefore = false, seatedAfter = false, physicsLive = false;
    Result result = Result::None;
    const char* reason = "";
    std::uint64_t issuedAtMs = 0, publishedAtMs = 0;
    // Appended in version 2 (long and cross-cell moves). A version 1 reader never looks at them and every older field keeps its meaning.
    bool crossCell = false;           // the destination was in another grid cell: the chassis was refiled before this receipt
    bool refiled = false;             // cross-cell only: the read-back found the chassis listed in the destination cell
    std::uint32_t hops = 0;           // bounded hops the doer issued (1 for a short move)
};
constexpr std::uint32_t kReceiptVersion = 2;

// The doer's read-back, taken after the next completed world step (never from the call's return value).
struct Readback {
    bool bodyValid = false;           // same full body id, same motion, same ring as when it was issued
    Pose chassis, driver;
    bool seatedAfter = false, aboardAfter = false, physicsLive = false;
    bool driverPoseKnown = false;     // false while the driver's stored position is known to lag its seat: then only the seat link is evidence
    std::uint64_t stepAfter = 0;      // the pair's fresh-step serial now
    bool refiled = false;             // cross-cell: chassis listed in the destination cell and its model is present (set by the settle check)
};

constexpr double kArriveToleranceM = 1.0;   // chassis within this of the target: the step also adds its own normal motion
constexpr double kDriverGapM = 3.0;         // a seated driver stays this close to the chassis
constexpr std::uint64_t kVerifyWindowMs = 4000;  // no completed step in this long after the last hop was issued: failed
// Long moves (2026-11-20). One big write is NOT used: each hop is a write that the 20 m centre-of-mass-near-body check (made before the write) and the
// seat/live read-back vouch for, the engine's own loaded-area correction moves bodies in bounded steps too, and a failed hop leaves a pair that is
// still near where it was proven alive and can be walked back. A hop is the distance one step may move the chassis; the doer's own limit is above it.
constexpr double kMaxHopM = 20.0;            // per hop (the doer refuses more than 25 per write)
constexpr double kMaxMoveM = 250.0;          // total: the game's loaded area is about this wide (project loaded-area wall)
constexpr double kHopToleranceM = 3.0;       // an intermediate hop is a waypoint: the car's own motion in the steps that completed is not a failure
constexpr std::uint64_t kHopWindowMs = 3000; // a hop that completes no step in this long failed
constexpr std::uint64_t kSettleWindowMs = 5000; // cross-cell: the refile must show within this after the last hop
constexpr unsigned kMaxHops = 16;            // 250 m / 20 m = 13, plus slack for a car that drifted

// How many hops a displacement needs.
inline unsigned HopCount(double distance) {
    if (!(distance > 0) || !std::isfinite(distance)) return 0;
    return unsigned(std::ceil(distance / kMaxHopM - 1e-9));
}
// The next hop from where the chassis is now: at most kMaxHopM toward `to`; `last` says it reaches `to`. Recomputed from the measured position
// each time, so a hop that came up short or drifted corrects itself instead of accumulating.
inline Pose NextHop(const Pose& now, const Pose& to, bool& last) {
    const double d = Distance(now, to);
    if (d <= kMaxHopM) { last = true; return to; }
    last = false;
    const double k = kMaxHopM / d;
    return { now.x + (to.x - now.x) * k, now.y + (to.y - now.y) * k, now.z + (to.z - now.z) * k, to.yaw };
}
// Move size accepted at all (the add-on's own reach is smaller); the reason is for the trace.
inline bool MoveAcceptable(double distance, const char*& reason) {
    if (!std::isfinite(distance)) { reason = "distance_not_finite"; return false; }
    if (distance > kMaxMoveM) { reason = "move_above_core_limit"; return false; }
    reason = "ok"; return true;
}
enum class HopVerdict : std::uint8_t { Wait = 0, Arrived, Failed };
// One hop's read-back (stepAfter = completed steps since the hop was issued). Nothing is decided on a hop that has not completed a step.
inline HopVerdict JudgeHop(const Readback& r, const Pose& hopTarget, double tolerance, const char*& reason) {
    if (!r.bodyValid) { reason = "body_changed"; return HopVerdict::Failed; }
    if (!r.physicsLive) { reason = "physics_not_live"; return HopVerdict::Failed; }
    if (r.stepAfter < 1) { reason = "no_fresh_step"; return HopVerdict::Wait; }
    if (Distance(r.chassis, hopTarget) > tolerance) { reason = "hop_not_at_target"; return HopVerdict::Failed; }
    if (!r.seatedAfter || !r.aboardAfter) { reason = "seat_link_lost"; return HopVerdict::Failed; }
    reason = "hop_arrived"; return HopVerdict::Arrived;
}
// After a failed hop: the pair can be walked back only if it is still one live seated pair on its body.
inline bool Recoverable(const Readback& r) { return r.bodyValid && r.physicsLive && r.seatedAfter && r.aboardAfter; }

// Judge a read-back against its request. None = not decidable yet (no fresh step); everything else is final.
inline Result Judge(const Request& q, const Readback& r, const char*& reason) {
    if (!r.bodyValid) { reason = "body_changed"; return Result::Failed; }
    if (!r.physicsLive) { reason = "physics_not_live"; return Result::Failed; }
    if (r.stepAfter <= q.stepBefore) { reason = "no_fresh_step"; return Result::None; }
    if (Distance(r.chassis, q.to) > kArriveToleranceM) { reason = "chassis_not_at_target"; return Result::Failed; }
    if (!r.seatedAfter || !r.aboardAfter) { reason = "seat_link_lost"; return Result::Failed; }
    if (r.driverPoseKnown && Distance(r.chassis, r.driver) > kDriverGapM) { reason = "driver_left_behind"; return Result::Failed; }
    reason = "pair_arrived_together"; return Result::Applied;
}

// One consumer: a name for the trace, a callback and the last serial it applied. A callback returns false when it could not apply (traced).
struct Consumer {
    const char* name = nullptr;
    bool (*apply)(const Receipt&, void*) = nullptr;
    void* user = nullptr;
    std::uint64_t lastSerial = 0, appliedCount = 0, refusedCount = 0;
};

constexpr std::size_t kMaxConsumers = 16, kMaxActive = 8;

class Coordinator {
public:
    struct Active { Request req; Phase phase = Phase::Idle; std::uint64_t serial = 0, issuedAtMs = 0, lastHopAtMs = 0; std::uint32_t hops = 0; };

    // Consumers subscribe once; the order of subscription is the order of delivery.
    bool Subscribe(const char* name, bool (*apply)(const Receipt&, void*), void* user) {
        if (!name || !apply || consumers_.size_ >= kMaxConsumers) return false;
        for (std::size_t i = 0; i < consumers_.size_; ++i) if (consumers_.at[i].name == name) return false;
        consumers_.at[consumers_.size_++] = Consumer{ name, apply, user, 0, 0, 0 };
        return true;
    }
    std::size_t ConsumerCount() const { return consumers_.size_; }
    const Consumer* ConsumerAt(std::size_t i) const { return i < consumers_.size_ ? &consumers_.at[i] : nullptr; }

    // Handoff's grant. 0 = refused (a pair relocates one at a time, never two for one vehicle, never against an old epoch).
    std::uint64_t Arm(const Request& q, std::uint64_t epoch, const char*& reason) {
        if (!q.vehicle) { reason = "no_vehicle"; return 0; }
        if (q.epoch != epoch) { reason = "stale_epoch"; return 0; }
        if (Relocating(q.vehicle)) { reason = "already_relocating"; return 0; }
        for (auto& a : active_) if (a.phase == Phase::Idle || a.phase == Phase::Published) {
            a = Active{ q, Phase::Granted, ++serial_, 0 };
            reason = "granted"; return a.serial;
        }
        reason = "no_room"; return 0;
    }
    // The doer says it changed the pose (on the physics thread, at one step). Until Verify publishes, the pair is relocating.
    bool MarkIssued(std::uint64_t serial, std::uint64_t nowMs) {
        if (auto* a = Find(serial); a && a->phase == Phase::Granted) { a->phase = Phase::Issued; a->issuedAtMs = nowMs; a->lastHopAtMs = nowMs; a->hops = 1; return true; }
        return false;
    }
    // The doer issued a further hop (a restore hop counts too). The pair stays relocating; the verify window restarts from this hop.
    bool HopIssued(std::uint64_t serial, std::uint64_t nowMs) {
        if (auto* a = Find(serial); a && a->phase == Phase::Issued) { a->lastHopAtMs = nowMs; ++a->hops; return true; }
        return false;
    }
    std::uint32_t Hops(std::uint64_t serial) const { const auto* a = Lookup(serial); return a ? a->hops : 0; }
    // The move ended without arriving: `restored` = the pair was walked back and read back at `from` (published as restored), otherwise failed with
    // the pair's last observed state. Only an issued relocation can end this way; a refusal before any write is Refuse().
    Receipt Conclude(std::uint64_t serial, const Readback& rb, bool restored, const char* reason, std::uint64_t nowMs) {
        auto* a = Find(serial);
        if (!a || a->phase != Phase::Issued) return {};
        Receipt r = Base(*a);
        r.measured = rb.chassis; r.stepAfter = rb.stepAfter; r.seatedAfter = rb.seatedAfter; r.physicsLive = rb.physicsLive;
        r.displacement = Distance(a->req.from, rb.chassis);
        const bool back = restored && rb.bodyValid && rb.physicsLive && rb.seatedAfter && rb.aboardAfter && Distance(rb.chassis, a->req.from) <= kArriveToleranceM;
        r.result = back ? Result::Restored : Result::Failed; r.reason = back ? reason : (restored ? "restore_not_proved" : reason); r.publishedAtMs = nowMs;
        return Publish(*a, r);
    }
    // The doer says it refused before changing anything: publish `refused`; the untouched pair is released.
    Receipt Refuse(std::uint64_t serial, const char* reason, std::uint64_t nowMs) {
        auto* a = Find(serial);
        if (!a || a->phase == Phase::Idle || a->phase == Phase::Published) return {};
        Receipt r = Base(*a); r.result = Result::Refused; r.reason = reason; r.publishedAtMs = nowMs;
        return Publish(*a, r);
    }
    // The doer's read-back. A receipt (applied or failed) is published once; an undecided read-back publishes nothing.
    bool Verify(std::uint64_t serial, const Readback& rb, std::uint64_t nowMs, Receipt& out) {
        auto* a = Find(serial);
        if (!a || a->phase != Phase::Issued) return false;
        const char* reason = "";
        Result res = Judge(a->req, rb, reason);
        if (res == Result::None) {
            if (nowMs - a->lastHopAtMs < kVerifyWindowMs) return false;   // keep waiting for a completed step
            reason = "no_completed_step"; res = Result::Failed;
        }
        Receipt r = Base(*a);
        r.measured = rb.chassis; r.stepAfter = rb.stepAfter; r.seatedAfter = rb.seatedAfter; r.physicsLive = rb.physicsLive; r.refiled = rb.refiled;
        r.displacement = Distance(a->req.from, rb.chassis); r.result = res; r.reason = reason; r.publishedAtMs = nowMs;
        out = Publish(*a, r);
        return true;
    }
    // A restore trial finished (the doer returned the pair to `from` and read it back): published as `restored` only when it truly is.
    Receipt Restored(std::uint64_t serial, const Readback& rb, std::uint64_t nowMs) {
        auto* a = Find(serial);
        if (!a || a->phase != Phase::Published) return {};
        Receipt r = Base(*a); r.measured = rb.chassis; r.stepAfter = rb.stepAfter; r.seatedAfter = rb.seatedAfter; r.physicsLive = rb.physicsLive;
        const bool back = rb.bodyValid && rb.physicsLive && Distance(rb.chassis, a->req.from) <= kArriveToleranceM && rb.seatedAfter;
        r.result = back ? Result::Restored : Result::Failed; r.reason = back ? "returned_to_origin" : "restore_not_proved"; r.publishedAtMs = nowMs;
        return PublishAgain(*a, r);
    }
    // The latest receipt published for a vehicle (applied, refused, failed or restored). A polling consumer reads this; a pushed one gets Deliver().
    bool LastFor(std::uint32_t vehicle, Receipt& out) const {
        for (const auto& r : last_) if (r.vehicle == vehicle && r.serial) { out = r; return true; }
        return false;
    }
    bool Relocating(std::uint32_t vehicle) const {
        for (const auto& a : active_) if (a.req.vehicle == vehicle && (a.phase == Phase::Granted || a.phase == Phase::Issued)) return true;
        return false;
    }
    const Active* Lookup(std::uint64_t serial) const { for (const auto& a : active_) if (a.serial == serial && a.phase != Phase::Idle) return &a; return nullptr; }
    std::uint64_t LastSerial() const { return serial_; }
    // Late or duplicate receipts: a consumer holds the epoch it lives in and applies a serial once.
    static bool Current(const Receipt& r, std::uint64_t epoch) { return r.epoch == epoch; }

private:
    Active* Find(std::uint64_t serial) { for (auto& a : active_) if (a.serial == serial && a.phase != Phase::Idle) return &a; return nullptr; }
    Receipt Base(const Active& a) const {
        Receipt r; r.serial = a.serial; r.epoch = a.req.epoch; r.vehicle = a.req.vehicle; r.driver = a.req.driver;
        r.from = a.req.from; r.to = a.req.to; r.stepBefore = a.req.stepBefore; r.seatedBefore = a.req.seatedBefore; r.issuedAtMs = a.issuedAtMs;
        r.version = kReceiptVersion; r.crossCell = a.req.crossCell; r.hops = a.hops; return r;
    }
    Receipt Publish(Active& a, Receipt r) { a.phase = Phase::Published; Deliver(r); return r; }
    Receipt PublishAgain(Active&, Receipt r) { Deliver(r); return r; }
    void Remember(const Receipt& r) {
        for (auto& e : last_) if (e.vehicle == r.vehicle) { e = r; return; }
        for (auto& e : last_) if (!e.serial) { e = r; return; }
        last_[lastNext_++ % last_.size()] = r;   // the table is full: the oldest slot is reused
    }
    void Deliver(const Receipt& r) {
        Remember(r);
        for (std::size_t i = 0; i < consumers_.size_; ++i) {
            auto& c = consumers_.at[i];
            if (c.lastSerial == r.serial && r.result != Result::Restored) continue;   // a serial is applied at most once (a restore is its own event)
            const bool ok = c.apply(r, c.user);
            c.lastSerial = r.serial; ok ? ++c.appliedCount : ++c.refusedCount;
        }
    }
    struct ConsumerList { std::array<Consumer, kMaxConsumers> at{}; std::size_t size_ = 0; } consumers_;
    std::array<Active, kMaxActive> active_{};
    std::array<Receipt, 16> last_{};
    std::size_t lastNext_ = 0;
    std::uint64_t serial_ = 0;
};

}  // namespace relocation
