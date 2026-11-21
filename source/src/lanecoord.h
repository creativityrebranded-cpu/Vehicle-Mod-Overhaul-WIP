#pragma once
// Lane coordination (design section 9, D15; notes/one-flow-plan-20261110/PLAN.md cards C1-C4). Variant build, 2026-11-10.
//
// Two additive, optional interfaces between the core and the traffic addon, and the coordinator's one arbiter for lane changes.
//   World -> Coordinator (C2): FVO_CorridorSnapshotV1, a read-only row per planned vehicle: how far each lane of its road is free of
//     fixed solids (from the same shapes its plan was scored against), what holds it, and how long it has stood. Nothing is written back.
//   Coordinator -> vehicle (C3, C4): FVO_RegisterLanePermitV1, lane permissions as route data. The core reads them into the lanes Avoid
//     may use (planbranch::Lanes::open); a closed lane is simply not a lane for that car. The car's own lane is never closed, and
//     collision safety stays per car and ignores all of this (G15).
// The arbiter (addon side) is the one place that decides whether a neighbouring lane is open to a car: Avoid proposes the change (its
// plan heads for the lane, read from FVO_OwnerSnapshotV2), the plan Arbiter commits it, and this arbiter grants or closes the lane.
// An older core has no exports and an older addon registers nothing: both then behave exactly as before.
// Pure; tests: tests/lanecoord_test.cpp.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lanecoord {
constexpr int kLanes = 8;

// ---- C2: the corridor summary (core export "FVO_CorridorSnapshotV1") ----
struct RowV1 {
    std::uint32_t size = sizeof(RowV1), version = 1;
    std::uint32_t vehicle = 0, blockOwner = 0;       // blockOwner: the vehicle that holds this one's plan (0: none, or a fixed thing)
    std::uint64_t laneIdentity = 0;                  // the lane guidance this row was measured on (FvoLaneGuidanceV1::identity)
    int ownLane = -1, plannedLane = -1, laneCount = 0;
    float freeLength[kLanes] = {};                   // metres ahead each lane is free of fixed solids; `measured` when nothing was found
    float measured = 0;                              // how far ahead this car measured (its volume's depth)
    float blockedAt = -1;                            // the route line's first fixed block, metres ahead (-1: none)
    float stoppedFor = 0;                            // seconds this car has stood (paused time not counted)
    int family = 0, reverse = 0;                     // the committed plan's family (planbranch::Family) and direction
    float age = 0;                                   // seconds since this row was written
};
typedef int (*SnapshotV1)(RowV1* out, int max);

// ---- C3 / C4: lane permissions (core export "FVO_RegisterLanePermitV1"; the addon registers its provider) ----
enum Reason : int { Open = 0, Occupied = 1, Closing = 2, Reserved = 3, Blocked = 4, Yield = 5 };
inline const char* Name(int r) { constexpr const char* n[]{ "open", "occupied", "closing_gap", "reserved", "blocked_ahead", "yield" }; return r >= 0 && r <= 5 ? n[r] : "?"; }
struct PermitV1 {
    std::uint32_t size = sizeof(PermitV1), version = 1;
    std::uint64_t identity = 0;                      // must match the lane guidance the car holds, or the answer is not for it
    int count = 0, open[kLanes] = {}, reason[kLanes] = {};
};
using PermitProviderV1 = int (*)(void* owner, PermitV1* out);  // 1 = answered
using RegisterPermitV1 = int (*)(PermitProviderV1);

// The corridor from a plan's own obstacle list (World): a lane is blocked at an obstacle when neither side of it leaves the car room.
// left/right/obsLeft/obsRight are lateral offsets from the route line; `from` is metres ahead.
inline bool Covers(double laneLeft, double laneRight, double obsLeft, double obsRight, double carWidth) {
    if (obsRight <= laneLeft || obsLeft >= laneRight) return false;
    return std::max(obsLeft - laneLeft, laneRight - obsRight) < carWidth;
}

// ---- the coordinator's lane arbiter (addon) ----
namespace rules {
constexpr double standstillGap = 3.0;     // bumper to bumper, as following (planspeed::following)
constexpr double headway = 1.4;           // s a follower in the target lane keeps to the car that moves in
constexpr double comfortableBrake = 3.0;  // m/s^2 that follower may be asked to brake
constexpr double passSeconds = 6.0;       // an oncoming lane must stay clear this long (D5: clear for the whole time)
constexpr double openMargin = 1.25;       // a closed lane opens again only with this much more room (separate on and off thresholds)
constexpr double permitHold = 1.0;        // s a closed answer stands at least
constexpr double reserveLapse = 1.5;      // s after the plan stops heading for a lane its reservation ends
constexpr double reserveWindow = 12.0;    // m each way round the car a reservation covers, plus both half lengths
constexpr double blockMemory = 4.0;       // s a reported fixed blockage is kept without hearing it again
constexpr double blockBefore = 35.0, blockPast = 6.0;  // a lane is no merge target this close before / past its blockage
constexpr double approachReach = 60.0, approachSpeed = 8.0;  // cars coming up to a blockage in their own lane slow to this
constexpr double deadlockAfter = 4.0;     // s both cars have stood, each held by the other
constexpr double yieldSeconds = 8.0;      // the loser keeps to its own lane this long
}

// Another vehicle as the target lane sees it. `ahead` is metres along the target lane's own travel direction from the asking car
// (negative: upstream of it); `speed` is its speed along that direction.
struct Other { std::uint32_t vehicle = 0; int lane = -1, planned = -1; double ahead = 0, speed = 0, halfLength = 2.8; };
struct Ask {
    std::uint32_t vehicle = 0; int own = -1, lane = -1; bool sameWay = true;
    double speed = 0, halfLength = 2.8, now = 0;
    double blockAhead = 1e18;   // metres to the nearest known fixed blockage in the target lane (negative: just past it); 1e18 none
    bool holdsReservation = false, yielding = false;
    std::uint32_t reservedBy = 0;  // another car's earlier reservation overlapping here (0: none)
    bool wasClosed = false;     // the last answer for this lane (hysteresis)
};
struct Answer { int reason = Open; std::uint32_t by = 0; double gap = 1e18, need = 0; };

// One lane, one car. Order: a deadlock yield, a fixed blockage, another car's reservation, then the gaps. A car that already holds the
// lane's reservation keeps it (the change is committed): only a yield or a blockage takes it back.
inline Answer Decide(const Ask& a, const Other* others, int count) {
    Answer out;
    if (a.lane == a.own) return out;
    if (a.yielding) { out.reason = Yield; return out; }
    if (a.blockAhead > -rules::blockPast && a.blockAhead < rules::blockBefore) { out.reason = Blocked; out.gap = a.blockAhead; return out; }
    if (a.holdsReservation) return out;
    if (a.reservedBy) { out.reason = Reserved; out.by = a.reservedBy; return out; }
    const double margin = a.wasClosed ? rules::openMargin : 1.0;
    for (int i = 0; i < count; ++i) {
        const Other& o = others[i];
        if (!o.vehicle || o.vehicle == a.vehicle || o.lane != a.lane) continue;
        const double bodies = a.halfLength + o.halfLength, gap = std::fabs(o.ahead) - bodies;
        // The asking car's speed along the target lane's direction: with it on a same-way lane, against it on an oncoming one.
        const double mine = a.sameWay ? a.speed : -a.speed;
        double need = rules::standstillGap;
        if (o.ahead < 0) {            // upstream of us in that lane: it comes up behind (same way) or head-on (oncoming)
            const double closing = std::max(0.0, o.speed - mine);
            need += a.sameWay ? closing * rules::headway + closing * closing / (2 * rules::comfortableBrake) : closing * rules::passSeconds;
        } else {                       // downstream: we would come up behind it (same way); on an oncoming lane it is moving away
            const double closing = std::max(0.0, mine - o.speed);
            need += a.sameWay ? closing * rules::headway : 0;
        }
        need *= margin;
        if (gap < need && gap < out.gap) { out.reason = gap < rules::standstillGap * margin ? Occupied : Closing; out.by = o.vehicle; out.gap = gap; out.need = need; }
    }
    return out;
}

// Reservations: the target-lane gap a committed lane change holds. First come keeps it; the same moment goes to the lower id.
constexpr int kReservations = 96;
struct Reservation { std::uint32_t vehicle = 0; std::uint64_t road = 0; int lane = -1; double since = 0, seen = 0, station = 0, reach = 0; };
struct Reservations {
    Reservation rows[kReservations]; int count = 0;
    Reservation* Find(std::uint32_t v, std::uint64_t road, int lane) { for (int i = 0; i < count; ++i) if (rows[i].vehicle == v && rows[i].road == road && rows[i].lane == lane) return &rows[i]; return nullptr; }
    void Sweep(double now) { for (int i = 0; i < count;) if (now - rows[i].seen > rules::reserveLapse) rows[i] = rows[--count]; else ++i; }
    // The plan heads for `lane`: keep or make its reservation. Returns false when the table is full (the car then has none: it waits).
    bool Hold(std::uint32_t v, std::uint64_t road, int lane, double station, double halfLength, double now) {
        Reservation* r = Find(v, road, lane);
        if (!r) { if (count >= kReservations) Sweep(now); if (count >= kReservations) return false; r = &rows[count++]; *r = Reservation{}; r->vehicle = v; r->road = road; r->lane = lane; r->since = now; }
        r->seen = now; r->station = station; r->reach = rules::reserveWindow + halfLength;
        return true;
    }
    void Drop(std::uint32_t v) { for (int i = 0; i < count;) if (rows[i].vehicle == v) rows[i] = rows[--count]; else ++i; }
    // Whose earlier reservation overlaps `station` on this lane, other than `v`'s own? 0: nobody's.
    std::uint32_t Before(std::uint32_t v, std::uint64_t road, int lane, double station, double halfLength, double now) const {
        const Reservation* mine = nullptr;
        for (int i = 0; i < count; ++i) if (rows[i].vehicle == v && rows[i].road == road && rows[i].lane == lane) mine = &rows[i];
        std::uint32_t by = 0; double since = 1e18;
        for (int i = 0; i < count; ++i) {
            const auto& r = rows[i];
            if (r.vehicle == v || r.road != road || r.lane != lane || now - r.seen > rules::reserveLapse) continue;
            if (std::fabs(r.station - station) > r.reach + rules::reserveWindow + halfLength) continue;
            if (mine && (mine->since < r.since || (mine->since == r.since && v < r.vehicle))) continue;  // ours came first
            if (r.since < since || (r.since == since && r.vehicle < by)) { since = r.since; by = r.vehicle; }
        }
        return by;
    }
};

// C3: fixed blockages cars reported in a lane (road identity + lane + station along the road).
constexpr int kBlocks = 64;
struct Block { std::uint64_t road = 0; int lane = -1; double station = 0, seen = 0; std::uint32_t reporter = 0; };
struct Blocks {
    Block rows[kBlocks]; int count = 0;
    void Sweep(double now) { for (int i = 0; i < count;) if (now - rows[i].seen > rules::blockMemory) rows[i] = rows[--count]; else ++i; }
    // Returns true when this is a blockage not known before (for the trace).
    bool Report(std::uint64_t road, int lane, double station, std::uint32_t reporter, double now) {
        for (int i = 0; i < count; ++i) if (rows[i].road == road && rows[i].lane == lane && std::fabs(rows[i].station - station) < 8.0) { rows[i].seen = now; rows[i].station = station; rows[i].reporter = reporter; return false; }
        if (count >= kBlocks) Sweep(now);
        if (count >= kBlocks) return false;
        rows[count++] = { road, lane, station, now, reporter };
        return true;
    }
    // Metres from `station` to the nearest known blockage in this lane, signed along `direction` (+1 / -1: the lane's travel along the
    // station axis); 1e18 when none. wrap > 0: a closed road of that length.
    double Ahead(std::uint64_t road, int lane, double station, double direction, double wrap, double now) const {
        double best = 1e18;
        for (int i = 0; i < count; ++i) {
            const auto& b = rows[i];
            if (b.road != road || b.lane != lane || now - b.seen > rules::blockMemory) continue;
            double d = (b.station - station) * (direction < 0 ? -1.0 : 1.0);
            if (wrap > 0) { d = std::fmod(d, wrap); if (d > wrap * .5) d -= wrap; if (d < -wrap * .5) d += wrap; }
            if (std::fabs(d) < std::fabs(best)) best = d;
        }
        return best;
    }
};

// C4: two cars that each stand held by the other. One of them yields: it keeps to its own lane (every other lane closed for it) until
// the other has gone. The higher priority goes; then the one that has stood longer; then the lower id.
struct Waiter { std::uint32_t vehicle = 0, heldBy = 0; double stoppedFor = 0; int priority = 0; };
inline bool Deadlocked(const Waiter& a, const Waiter& b) {
    return a.vehicle && b.vehicle && a.vehicle != b.vehicle && a.heldBy == b.vehicle && b.heldBy == a.vehicle &&
        a.stoppedFor >= rules::deadlockAfter && b.stoppedFor >= rules::deadlockAfter;
}
// The vehicle that yields.
inline std::uint32_t Loser(const Waiter& a, const Waiter& b) {
    if (a.priority != b.priority) return a.priority < b.priority ? a.vehicle : b.vehicle;
    if (std::fabs(a.stoppedFor - b.stoppedFor) > 1.0) return a.stoppedFor < b.stoppedFor ? a.vehicle : b.vehicle;
    return a.vehicle > b.vehicle ? a.vehicle : b.vehicle;
}
constexpr int kYields = 32;
struct Yields {
    struct Row { std::uint32_t vehicle = 0, to = 0; double until = 0; } rows[kYields]; int count = 0;
    void Sweep(double now) { for (int i = 0; i < count;) if (now >= rows[i].until) rows[i] = rows[--count]; else ++i; }
    bool Set(std::uint32_t v, std::uint32_t to, double now) {   // true: a new yield (for the trace)
        for (int i = 0; i < count; ++i) if (rows[i].vehicle == v) { rows[i].to = to; rows[i].until = now + rules::yieldSeconds; return false; }
        if (count >= kYields) Sweep(now);
        if (count >= kYields) return false;
        rows[count++] = { v, to, now + rules::yieldSeconds };
        return true;
    }
    std::uint32_t To(std::uint32_t v, double now) const { for (int i = 0; i < count; ++i) if (rows[i].vehicle == v && now < rows[i].until) return rows[i].to ? rows[i].to : 1u; return 0; }
};
}
