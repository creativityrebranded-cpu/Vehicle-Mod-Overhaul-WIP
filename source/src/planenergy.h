#pragma once
// ED-1 (2026-11-12, user: "Yes, move to it"): the energy planner DRIVES your car (pilot 0) on wheels, through the AF-1 path follower.
// Design: notes/redesign-20261112/AUDIT_AND_REDESIGN.md; card: notes/energy-drive-20261112/CHANGE_CARD.md.
//
// It runs off the game thread: each update the game hands a copied snapshot (World shapes, field boxes, the route line, the floor, the car)
// to a plan worker when that worker is free; the worker keeps the planner's memory between plans (commitment holds across updates) and
// uses the newest cost-to-go field, which a second worker rebuilds in the background only when the car nears its window's edge, the goal
// has moved on, or it is old. The game takes the newest result while it is fresh and hands it to the Controller as a path + speed.
//
// Removal 2026-11-14: the energy plan is the only decider for every car, in test and player builds alike (the legacy plan, the old
// autopilot and the shadow comparison are gone). A car whose result is missing or stale stands still (step 7 in vehicleflight.cpp).
#include "energyplan.h"
#include "hoverfollow.h"
#include "pathfollow.h"
#include <limits>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>

namespace planenergy {
// The part of the best trajectory the car drives now: its states in the current direction, from the start, as follower points.
// A gear change ends it (the next direction comes with a later plan, after the car has stopped there).
inline pathfollow::Path DrivenPath(const bicycle::State* traj, int count, bool& reverse) {
    pathfollow::Path p; reverse = false; int dir = 0;
    for (int i = 1; i < count; ++i) { const double v = traj[i].speed; if (std::fabs(v) > .05) { dir = v > 0 ? 1 : -1; break; } }
    reverse = dir < 0;
    double lx = 1e18, ly = 1e18;
    for (int i = 0; i < count && p.count < pathfollow::kMaxPoints; ++i) {
        if (i > 0 && dir != 0 && traj[i].speed * dir < -.05) break;  // the direction changes here
        if (std::hypot(traj[i].x - lx, traj[i].y - ly) < .3 && i + 1 < count) continue;  // keep points about 0.3 m apart
        p.x[p.count] = traj[i].x; p.y[p.count] = traj[i].y; ++p.count; lx = traj[i].x; ly = traj[i].y;
    }
    // A plan that stands still (or barely moves) still needs a path: a short one along the nose.
    if (p.count < 2) { const double h = traj[0].heading; p.count = 2; p.x[0] = traj[0].x; p.y[0] = traj[0].y; p.x[1] = traj[0].x + std::sin(h); p.y[1] = traj[0].y + std::cos(h); }
    return p;
}

struct Result {
    bool ok = false; std::uint64_t at = 0;    // GetTickCount64 when published
    double speed = 0; bool reverse = false;    // what to ask now (signed speed; reverse = backing)
    pathfollow::Path path;                     // collision-scored rollout, also used by radar and plan exports
    pathfollow::Path laneCentre;               // optional, local route geometry for forward Follow steering only
    bool UsesLaneCentre() const { return laneCentre.count >= 2; }
    const pathfollow::Path& SteeringPath() const { return UsesLaneCentre() ? laneCentre : path; }
    const char* why = ""; double energy = 0, clearance = 0, workerMs = 0; int gears = 0, feasible = 0, candidates = 0; bool field = false;
    bool hover = false;                        // the plan flies now (the hover follower flies it)
    hoverfollow::Path hoverPath;               // step 5 (D15): the whole best rollout for the hover follower (data only; read from step 7)
    bool goal = false; std::uint32_t vehicle = 0;  // a goal scene (a car with no lane); the car this result is for
    bool kept = false; const char* rival = ""; double rivalGap = 0; const char* switchWhy = ""; double offset = 0, speedAhead = 0;  // O1-O4
    int rel[6] = {};  // HC-3: energyplan::Result::rel
    double fullSpeed = 0, fullEnergy = 0; energyplan::Terms fullTerms;  // HC-4: energyplan::Result::full*
    const char* rejectName[energyplan::Result::kRejectKinds] = {}; int rejectCount[energyplan::Result::kRejectKinds] = {}; int rejectKinds = 0;  // CT-1
    energyplan::GradeFit startGrade; double maxGrade = 0; int floorSetAside = 0;  // GR-2 (traced)
    energyplan::Result::LaneSpeed laneSpeeds[energyplan::Result::kLaneSpeeds]; int laneSpeedCount = 0;  // CT-1
    energyplan::Terms terms;  // the chosen plan's energy by term (Ledger: what holds a slow car)
    bool pulse = false, testRefused = false;   // D12 (removal step 1): this result is the spot test's pulse / the pulse was refused (not safe)
    std::uint64_t spotPulseSerial = 0;         // Health evidence only
    bool factorySpotPulse = false;            // this temporary result needs a matching live request
};

// In Follow, track the lane that the rollout is trying to reach. Taking feed-forward curvature from the
// rollout itself makes the correction depend on the latest yaw even while the lane and the decision are held.
// Keep that rollout in Result::path. The local centre path is strictly a steering reference, not a second plan.
inline pathfollow::Path LaneCentrePath(const energyplan::Result& plan, const planscene::Scene& scene,
                                      const bicycle::State& car, const bicycle::Spec& spec,
                                      double station, double lateral, double half, bool reverse, bool hover,
                                      double clearanceMargin) {
    const auto& candidate = plan.best;
    if (!plan.ok || plan.pulse || reverse || hover || car.mode != 0 || plan.first.speed <= .1 ||
        car.speed < -.1 || std::strcmp(plan.layer, "follow") != 0 || !candidate.count ||
        !scene.lanes || scene.travel == 2 || scene.route.size() < 2 || !std::isfinite(station) ||
        plan.minClearance < clearanceMargin || std::fabs(lateral) + spec.halfWidth >= half)
        return {};
    for (int i = 0; i < candidate.count; ++i) {
        const auto& leg = candidate.seg[i];
        if (!leg.follow || leg.mode != 0 || leg.speed <= .1 || std::fabs(leg.offset) > 1e-6) return {};
    }
    // Preserve the authored vertices and their direction. Keep the window local so nearest-point
    // projection cannot pick a far branch around a tight turn or crossing.
    constexpr double behind = 4.0, ahead = 10.0;
    const double endStation = std::min(scene.stopStation, scene.route.back().station);
    if (endStation <= station + 2.0) return {};
    const double first = std::max(station - behind, scene.route.front().station);
    const double last = std::min(station + ahead, endStation);
    pathfollow::Path path;
    auto add = [&](double x, double y) {
        if (path.count && std::hypot(x - path.x[path.count - 1], y - path.y[path.count - 1]) < .05) return;
        if (path.count >= pathfollow::kMaxPoints) return;
        path.x[path.count] = x; path.y[path.count] = y; ++path.count;
    };
    double x, y;
    if (!scene.RouteAt(first, 0, x, y)) return {};
    add(x, y);
    for (const auto& point : scene.route)
        if (point.station > first && point.station < last) add(point.x, point.y);
    if (scene.RouteAt(last, 0, x, y)) add(x, y);
    if (path.count < 2) return {};
    // The scored car rollout is not the centre reference. Also check the reference against
    // this same scene at bounded one-metre samples, with the vehicle aligned to the lane.
    // This static check is a gate for the reference, not a substitute for rollout scoring.
    auto clearAt = [&](double at) {
        double px, py, ax, ay, bx, by, floorZ;
        if (!scene.RouteAt(at, 0, px, py) ||
            !scene.RouteAt(at - .5, 0, ax, ay) || !scene.RouteAt(at + .5, 0, bx, by) ||
            !scene.Floor(px, py, floorZ)) return false;
        if (std::hypot(bx - ax, by - ay) < .05) return false;
        return scene.Clearance(px, py, std::atan2(bx - ax, by - ay), floorZ, spec) >= clearanceMargin;
    };
    for (double at = station; at < last; at += 1.0)
        if (!clearAt(at)) return {};
    if (!clearAt(last)) return {};
    const auto following = pathfollow::Follow(path, car.x, car.y, car.heading, car.speed, false);
    if (!following.ok || std::fabs(following.headingError) >= pathfollow::kPi * .5) return {};
    return path;
}

// station by `advance` m, or older than `maxAge` s.
inline bool FieldStale(bool have, double carX, double carY, double fx0, double fy0, double size, double ageS, double goalAhead,
                       double edge = 14.0, double maxAge = 4.0) {
    if (!have) return true;
    const double ex = std::min(carX - fx0, fx0 + size - carX), ey = std::min(carY - fy0, fy0 + size - carY);
    return ex < edge || ey < edge || ageS > maxAge || goalAhead < 8.0;
}
}

namespace planenergy {
// The budget for cars other than yours (was shadowcompare::rules / MaySubmit): at most plansInFlight plans running, each car at most every
// `every` s, and fieldsInFlight cost-to-go fields building at once.
namespace budget {
constexpr double every = .2; constexpr int plansInFlight = 4, fieldsInFlight = 1;
inline bool MaySubmit(double now, double lastAt, int inFlight) { return inFlight < plansInFlight && (now - lastAt >= every || now < lastAt); }
}
}
#if !defined(PLANENERGY_PURE)  // tests include only the pure part above (removal 2026-11-14: player builds plan too)
#include "planinput.h"
#include "corridor.h"
#include "relocationconsumers.h"  // the relocation receipt: Plan drops its memory once per applied serial
#include "log.h"
#include <windows.h>
#include <latch>
#include <thread>
namespace planenergy {
struct Snapshot {
    planrun::Frame frame;
    std::array<planrun::LinePoint, 101> line;
    std::array<planworld::Wanted, planrun::kSamples> floor;
    std::vector<fieldbox::Box> field;
    std::vector<planworld::Shape> shapes;
    bicycle::State car; bicycle::Spec spec; std::uint32_t vehicle = 0; double nowS = 0;
    // A budgeted snapshot (a car other than yours: one thread, counted in g_plansInFlight), and for a car with no lane its goal scene inputs.
    bool floorPointCleared = false;
    const char* trafficHoldReason = "";
    relocrebase::Event relocation;  // the vehicle's latest applied relocation, read on the game thread when the job was queued
    bool budgeted = false;
    bool goal = false; planinput::GoalAsk goalAsk; std::vector<planinput::FloorPoint> goalFloor; double want = 0;
};
struct FloorProbeRequest {
    std::uint32_t vehicle = 0, body = 0;
    double snapshot = 0, x = 0, y = 0, z = 0, carX = 0, carY = 0, carZ = 0, carHeight = 0;
};
struct Driver {
    std::vector<planscene::Scene::CheckedSpan> sweptTrail;
    std::uint32_t sweepVehicle = 0, lastStopVehicle = 0;
    double stillSince = -1;
    bool physicalStallLogged = false;
    const char* lastStopReason = "";  // plan worker only
    std::mutex lock;                               // guards latest and field
    Result latest;
    std::shared_ptr<const planlattice::Field> field; std::uint64_t fieldAt = 0; double fieldGoalX = 0, fieldGoalY = 0;  // the goal point the field leads to (world)
    energyplan::Memory memory;                     // this car's plan memory (one plan at a time per car)
#ifndef FVO_RELEASE
    // Solely owned by this slot's plan worker, even when the pool changes threads.
    std::uint32_t traceVehicle = 0, traceOwner = 0, traceBody = 0;
    const char* traceSensor = "";
    bool traceBlocked = false;
    double traceAt = -1;
#endif
    double floorProbeAskedAt = -1e9;  // plan worker only
    bool nogoInside = false; int nogoRefused = 0; double nogoLogAt = -1e9;  // known no-go trace state (plan worker only)
    double corridorShift = 0, corridorAt = -1, corridorLogAt = -1e9;  // CC-1: the followed line's eased sideways shift (plan worker only)
    corridor::Memory corridorMemory;  // CC-2: this car's remembered edge points (plan worker only; restarts with the car, as the shift does)
    std::optional<FloorProbeRequest> floorProbe;  // main thread takes it under lock
    std::optional<FloorProbeRequest> lastFloorProbe;  // plan worker only
    std::atomic_flag planBusy{}, fieldBusy{};
    HMODULE module = nullptr;
    // Step 6: the car this driver plans for (memory and field restart when it changes, or between lane and goal scenes), and the game-thread
    // budget bookkeeping (only touched by the game thread).
    std::uint32_t vehicle = 0; bool goalKind = false;
    relocrebase::Seen relocSeen;  // plan worker only: the last relocation serial this slot re-based on
    double submitAt = -1e9, floorRetryUntil = 0;
};
// Step 6: one driver per pilot slot (0 = your car: ED-1's driver, unchanged), made when first used.
constexpr int kSlots = 257;
inline Driver& For(int slot) {
    static std::unique_ptr<Driver> slots[kSlots]; static std::mutex made;
    slot = std::clamp(slot, 0, kSlots - 1);
    std::lock_guard lk(made); if (!slots[slot]) slots[slot] = std::make_unique<Driver>();
    return *slots[slot];
}
inline Driver& The() { return For(0); }

inline void NoteCheckedPath(int slot, std::uint32_t vehicle, const planscene::Scene::CheckedSpan& span) {
    auto& d = For(slot);
    std::lock_guard lock(d.lock);
    auto& points = d.sweptTrail;
    if (d.sweepVehicle != vehicle || (!points.empty() && span.at < points.back().at)) {
        d.sweepVehicle = vehicle;
        points.clear();
    }
    std::erase_if(points, [&](const auto& old) { return span.at - old.at > 12; });
    for (auto& old : points) {
        if (std::hypot(old.x0-span.x0,old.y0-span.y0) < .5 &&
            std::hypot(old.x1-span.x1,old.y1-span.y1) < .5 && std::fabs(old.z1-span.z1) < .5) {
            old = span;
            return;
        }
    }
    if (points.size() >= 48) points.erase(points.begin());
    points.push_back(span);
}


inline void TraceManagedHalt(Driver& d, const Snapshot& s, const energyplan::Result& p) {
    const bool still = !p.ok || std::fabs(p.first.speed) <= .1;
    const bool traffic = s.frame.held || (s.frame.lineEnd >= 0 && s.frame.lineEnd <= 1);
    const bool physicallyStill = std::fabs(s.car.speed) < .15 && !s.frame.paused;
    if (d.lastStopVehicle != s.vehicle || !physicallyStill || traffic || still) {
        d.stillSince = -1; d.physicalStallLogged = false;
    } else if (d.stillSince < 0) d.stillSince = s.nowS;
    else if (!d.physicalStallLogged && s.nowS - d.stillSince >= 1.5) {
        d.physicalStallLogged = true;
        logger::Write("OBS system=traffic.halt vehicle=%08X snapshot_s=%.6f event=actual_stall owner=physics state=%s reason=commanded_motion_without_measured_speed plan=%s",
            s.vehicle, s.nowS, s.car.mode ? "airborne" : "ground", p.why ? p.why : "");
    }
    const char* reason = !still ? "" : s.frame.held ? s.trafficHoldReason :
        traffic ? "traffic_stop_line" : !p.ok ? "stalled_no_feasible_maneuver" : "maneuver_transition";
    if (d.lastStopVehicle == s.vehicle && std::strcmp(d.lastStopReason, reason) == 0) return;
    if (still || (d.lastStopVehicle == s.vehicle && *d.lastStopReason))
        logger::Write("OBS system=traffic.halt vehicle=%08X snapshot_s=%.6f event=%s owner=%s state=%s reason=%s",
            s.vehicle, s.nowS, still ? "enter" : "release", still && traffic ? "traffic" : "plan",
            s.car.mode ? "airborne" : "ground", still ? reason : "motion_available");
    d.lastStopVehicle = s.vehicle;
    d.lastStopReason = reason;
}
inline std::atomic<int> g_plansInFlight{ 0 }, g_fieldsInFlight{ 0 };  // budgeted work in flight (budget:: above)

// Step 2: the plan's candidates scored on the Windows thread pool. The calling worker scores one share itself; the plan waits for every share
// (std::latch), so nothing outlives the call. A share that cannot be submitted runs inline. Same picks as one thread (energyplan::ForEach).
struct PoolShare { const std::function<void(int)>* fn = nullptr; int n = 0, parts = 0, part = 0; std::latch* done = nullptr; HMODULE module = nullptr; };
inline void CALLBACK PoolShareWork(PTP_CALLBACK_INSTANCE inst, void* ctx) {
    std::unique_ptr<PoolShare> sh(static_cast<PoolShare*>(ctx)); if (sh->module) FreeLibraryWhenCallbackReturns(inst, sh->module);
    try { for (int i = sh->part; i < sh->n; i += sh->parts) (*sh->fn)(i); } catch (...) {}
    sh->done->count_down();
}
inline void PoolForEach(int n, const std::function<void(int)>& fn) {
    const int parts = std::clamp(int(std::thread::hardware_concurrency()) - 2, 1, 4);
    if (parts <= 1 || n < 32) { for (int i = 0; i < n; ++i) fn(i); return; }
    std::latch done(parts - 1);
    for (int part = 1; part < parts; ++part) {
        auto sh = std::make_unique<PoolShare>(); sh->fn = &fn; sh->n = n; sh->parts = parts; sh->part = part; sh->done = &done;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&PoolShareWork), &sh->module);
        PoolShare* raw = sh.release();
        if (!TrySubmitThreadpoolCallback(PoolShareWork, raw, nullptr)) {  // could not submit: run it here
            for (int i = raw->part; i < n; i += parts) fn(i);
            if (raw->module) FreeLibrary(raw->module); delete raw; done.count_down();
        }
    }
    for (int i = 0; i < n; i += parts) fn(i);  // share 0 on this thread
    done.wait();
}
struct PlanJob { Snapshot s; Driver* d = nullptr; HMODULE module = nullptr; };
struct FieldJob { planscene::Scene scene; bicycle::Spec spec; double cx = 0, cy = 0, goal = 0, hint = 0, gx = 0, gy = 0, carX = 0, carY = 0; Driver* d = nullptr; bool budgeted = false; HMODULE module = nullptr; };

inline void CALLBACK FieldWork(PTP_CALLBACK_INSTANCE inst, void* ctx) {
    std::unique_ptr<FieldJob> job(static_cast<FieldJob*>(ctx)); FreeLibraryWhenCallbackReturns(inst, job->module);
    auto& d = *job->d;
    try {
        const auto t0 = GetTickCount64();
        auto f = std::make_shared<planlattice::Field>(planlattice::Build(job->scene, job->spec, job->cx, job->cy, job->goal, job->hint, {}, job->carX, job->carY));  // step 2: early stop past the car
        { std::lock_guard lk(d.lock); d.field = std::move(f); d.fieldAt = GetTickCount64(); d.fieldGoalX = job->gx; d.fieldGoalY = job->gy; }
        if (!job->budgeted) logger::Write("OBS system=plan.energy event=field_built ms=%llu centre=%.1f,%.1f goal_station=%.1f reason=cost_to_go_rebuilt_in_background",
            static_cast<unsigned long long>(GetTickCount64() - t0), job->cx, job->cy, job->goal);
    } catch (...) { logger::Write("OBS system=plan.energy event=failed reason=field_worker_exception"); }
    if (job->budgeted) g_fieldsInFlight.fetch_sub(1, std::memory_order_relaxed);
    d.fieldBusy.clear(std::memory_order_release);
}

// D13: a car with no lane. Goal scene from the snapshot's World shapes and floor points; the cost-to-go field to
// the goal (or the sub-goal mark), rebuilt in this job when the goal moved 3 m, the car moved 5 m, or it is 2 s old (budgeted work is
// already budgeted, so it is built here rather than on a second worker); then the same Plan.
inline void PlanGoal(Driver& d, Snapshot& s, std::chrono::steady_clock::time_point start) {
    Result r; r.goal = true; r.vehicle = s.vehicle;
    auto scene = planinput::GoalScene(s.goalAsk, s.shapes.data(), int(s.shapes.size()), s.goalFloor.data(), int(s.goalFloor.size()));
    std::shared_ptr<const planlattice::Field> field; std::uint64_t fieldAt; double gx, gy;
    { std::lock_guard lk(d.lock); field = d.field; fieldAt = d.fieldAt; gx = d.fieldGoalX; gy = d.fieldGoalY; }
    const double ageS = field ? (GetTickCount64() - fieldAt) / 1000.0 : 1e9;
    const bool carNearEdge = field && FieldStale(true, s.car.x, s.car.y, field->x0, field->y0, field->p.size, 0, 1e9, 10.0, 1e9);
    if (!field || ageS > 2.0 || carNearEdge || std::hypot(gx - s.goalAsk.goal.x, gy - s.goalAsk.goal.y) > 3.0) {
        double cx = s.goalAsk.goal.x, cy = s.goalAsk.goal.y;  // the window: between the car and the field's goal (the goal, or the mark)
        if (!scene.goalFinal && !scene.route.empty() && scene.route.back().station > 1e-6) {
            const double f = scene.goalStation / scene.route.back().station; cx = s.car.x + (cx - s.car.x) * f; cy = s.car.y + (cy - s.car.y) * f; }
        auto built = std::make_shared<planlattice::Field>(planlattice::Build(scene, s.spec, .5 * (s.car.x + cx), .5 * (s.car.y + cy), scene.goalStation, std::nan(""), {}, s.car.x, s.car.y));
        { std::lock_guard lk(d.lock); d.field = built; d.fieldAt = GetTickCount64(); d.fieldGoalX = s.goalAsk.goal.x; d.fieldGoalY = s.goalAsk.goal.y; }
        field = built;
    }
    const auto p = energyplan::Plan(s.car, s.spec, s.want, scene, d.memory, s.nowS, {}, std::nan(""),
        scene.travel == 2 ? nullptr : field.get(), energyplan::Serial);
    r.ok = p.ok; r.why = p.why; r.energy = p.energy; r.clearance = p.minClearance; r.gears = p.gearChanges; r.feasible = p.feasible; r.candidates = p.candidates; r.field = field != nullptr;
    r.path = DrivenPath(p.traj, energyplan::kSteps + 1, r.reverse);
    r.hoverPath = hoverfollow::FromTraj(p.traj, energyplan::kSteps + 1, energyplan::kDt);
    r.speed = p.first.speed; r.hover = p.firstMode == 1 || p.traj[1].mode == 1;
    if (r.reverse && r.speed > 0) r.speed = 0; if (!r.reverse && r.speed < 0) r.speed = 0;
    r.kept = p.kept; r.rival = p.rival; r.rivalGap = p.rivalGap; r.switchWhy = p.switchWhy; r.offset = p.offset; r.speedAhead = p.speedAhead; r.terms = p.terms; for (int q = 0; q < 6; ++q) r.rel[q] = p.rel[q]; r.fullSpeed = p.fullSpeed; r.fullEnergy = p.fullEnergy; r.fullTerms = p.fullTerms;
    r.rejectKinds = p.rejectKinds; for (int q = 0; q < p.rejectKinds; ++q) { r.rejectName[q] = p.rejectName[q]; r.rejectCount[q] = p.rejectCount[q]; } r.startGrade = p.startGrade; r.maxGrade = p.maxGrade; r.floorSetAside = p.floorSetAside;
    r.laneSpeedCount = p.laneSpeedCount; for (int q = 0; q < p.laneSpeedCount; ++q) r.laneSpeeds[q] = p.laneSpeeds[q];
    r.workerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    r.at = GetTickCount64();
    std::lock_guard lk(d.lock); d.latest = r;
}

inline std::optional<FloorProbeRequest> TakeFloorProbe() {
    auto& d = The();
    std::lock_guard lk(d.lock);
    const auto pending = d.floorProbe;
    d.floorProbe.reset();
    return pending;
}

#ifndef FVO_RELEASE
// Player lane snapshots only. Log the first rejection immediately, changes of source/owner/body,
// and a periodic update while it persists. Each record refers to the input used by this worker.
inline void TraceSensors(Driver& d, const Snapshot& s, const energyplan::BlockTrace& block) {
    const auto& b = block.shape;
    const bool changed = d.traceVehicle != s.vehicle || block.valid != d.traceBlocked ||
        (block.valid && (d.traceOwner != b.owner || d.traceBody != b.body || std::strcmp(d.traceSensor, b.sensor) != 0));
    if (!changed && s.nowS >= d.traceAt && s.nowS - d.traceAt < 1.0) return;
    d.traceVehicle = s.vehicle; d.traceOwner = b.owner; d.traceBody = b.body;
    d.traceSensor = b.sensor; d.traceBlocked = block.valid; d.traceAt = s.nowS;
    const char* names[] = {"retained_ray","box","ground","vehicle","person","contact","survey","volume","radar",
                           "rotating_ray","forward_ray","path_sweep","radar_box","radar_point","body_check","inferred_push"};
    int counts[std::size(names)] = {}, other = 0, floorSamples = 0;
    for (const auto& w : s.floor) floorSamples += w.have && w.measured;
    for (const auto& shape : s.shapes) {
        const auto* sensor = planinput::Sensor(shape);
        std::size_t i = 0;
        for (; i < std::size(names); ++i) if (std::strcmp(names[i], sensor) == 0) { ++counts[i]; break; }
        if (i == std::size(names)) ++other;
    }
    char countsText[384]; std::size_t used = 0;
    for (std::size_t i = 0; i < std::size(names); ++i)
        used += std::snprintf(countsText + used, sizeof countsText - used, "%s%s:%d", i ? "," : "", names[i], counts[i]);
    logger::Write("OBS system=world.sensors vehicle=%08X snapshot_s=%.6f floor_samples=%d inputs={%s} other=%d centre_solid=%d",
                  s.vehicle, s.nowS, floorSamples, countsText, other, int(block.valid));
    if (block.valid)
        logger::Write("OBS system=plan.blocker vehicle=%08X snapshot_s=%.6f sensor=%s source=%s owner=%08X body=%08X layer=%d support=%d age_s=%.3f normal_ok=%d normal_z=%.3f tile=%.3f,%.3f,%.3f..%.3f tile_half=%.3f predicted=%.3f,%.3f,%.3f yaw=%.4f predicted_speed=%.2f wanted_speed=%.2f at_s=%.2f floor=%.3f floor_measured=%d clearance=%.3f initial_clearance=%.3f road=%.3f road_spread=%.3f",
                      s.vehicle, s.nowS, b.sensor, b.source >= 0 ? planworld::Name(static_cast<planworld::Source>(b.source)) : "unattributed",
                      b.owner, b.body, b.layer, b.support, b.seenAt > 0 ? s.nowS - b.seenAt : -1.0,
                      int(b.haveNormal), b.normalZ, b.x, b.y, b.base, b.top, b.half,
                      block.x, block.y, block.z, block.heading, block.speed, block.wantedSpeed, block.seconds,
                      block.floor, int(block.floorKnown), block.clearance, block.initialClearance,
                      std::isfinite(b.road) ? b.road : -9999.0, b.roadSpread);
}
#endif

// Script mode (2026-11-22, the user: "just drive and never have to recover because it assumes a perfect path and can't see anything
// that will stop it"). On wheels on a drive lane the route's line IS the plan: no candidate search, no avoidance arcs, no recovery
// routine (back up, K-turn, turn to lane). Speed = the lane speed World already capped for bends and crests (Frame::want), 0 under a
// lane hold or stop line, and one stop rule: along the line ahead, the first point where the car's body would touch a scene solid
// (in Script mode only other vehicles, people and hazard boxes are in the scene; edge bands are skipped: the path is taken as perfect)
// limits the speed to sqrt(2 * brake * distance left). It drives the line or waits; it never decides it is stuck.
inline void CALLBACK PlanWork(PTP_CALLBACK_INSTANCE inst, void* ctx) {
    std::unique_ptr<PlanJob> job(static_cast<PlanJob*>(ctx)); FreeLibraryWhenCallbackReturns(inst, job->module);
    auto& d = *job->d; const bool budgetJob = job->s.budgeted;
    try {
        const auto start = std::chrono::steady_clock::now();
        auto& s = job->s; auto& f = s.frame; f.field = s.field.data(); f.fieldCount = int(s.field.size());
        // Step 6: a different car in this slot, or a switch between lane and goal scenes: the memory and the field are that car's no longer.
        if (d.vehicle != s.vehicle || d.goalKind != s.goal) { d.memory = {}; d.floorRetryUntil = 0; d.corridorShift = 0; d.corridorAt = -1; d.corridorMemory = {}; std::lock_guard lk(d.lock); d.field.reset(); d.vehicle = s.vehicle; d.goalKind = s.goal; }
        // Relocation receipt (applied, once per serial, this vehicle only): the committed plan, its memory, the corridor edges, the cost-to-go field and the
        // swept trail all describe the old surroundings. Dropped here; the plan below starts fresh at the destination. Plan decides nothing about the move.
        if (relocrebase::Take(d.relocSeen, s.relocation, s.vehicle)) {
            d.memory = {}; d.floorRetryUntil = 0; d.corridorShift = 0; d.corridorAt = -1; d.corridorMemory = {}; d.stillSince = -1; d.physicalStallLogged = false;
            { std::lock_guard lk(d.lock); d.field.reset(); d.sweptTrail.clear(); d.sweepVehicle = 0; }
            logger::Write("OBS system=plan.energy event=rebased vehicle=%08X serial=%llu epoch=%llu to=%.2f,%.2f,%.2f reason=relocation_receipt_applied_plan_memory_corridor_field_and_swept_trail_dropped",
                s.vehicle, static_cast<unsigned long long>(s.relocation.serial), static_cast<unsigned long long>(s.relocation.epoch), s.relocation.to[0], s.relocation.to[1], s.relocation.to[2]);
        }
        const energyplan::ForEach scoring = s.budgeted ? energyplan::ForEach(energyplan::Serial) : energyplan::ForEach(PoolForEach);  // budgeted cars: one thread each
        if (s.goal) PlanGoal(d, s, start);
        else {
        auto line = [&](double dd) { return s.line[std::clamp(int(std::lround(dd)) + 20, 0, 100)]; };
        auto wanted = [&](double dd, double) { return s.floor[std::clamp(int(std::lround(dd / planrun::kStep)), 0, planrun::kSamples - 1)]; };
        auto scene = planinput::Scene(f, line, wanted, s.shapes.data(), int(s.shapes.size()));
        {
            std::lock_guard lock(d.lock);
            if (d.sweepVehicle == s.vehicle) scene.checkedPath = d.sweptTrail;
        }
        // CC-1: the line this car follows keeps a margin from the measured edges beside the lane (corridor.h). On wheels only.
        if (scene.route.size() >= 2 && scene.travel != 2 && s.car.mode == 0) {
            double cst, clat, chalf; scene.Route(s.car.x, s.car.y, cst, clat, chalf);
            const auto edges = corridor::Measure(scene, cst, s.car.x, s.car.y, s.car.z, s.spec, &d.corridorMemory, s.nowS);  // CC-2: with the remembered edges
            const double dt = d.corridorAt < 0 || s.nowS < d.corridorAt ? 0.0 : s.nowS - d.corridorAt;
            const double before = d.corridorShift;
            d.corridorShift = corridor::Ease(d.corridorShift, edges.target, dt); d.corridorAt = s.nowS;
            corridor::ShiftRoute(scene, d.corridorShift);
#ifndef FVO_RELEASE
            if (!s.budgeted && (std::fabs(d.corridorShift - before) > .05 || ((d.corridorShift != 0 || edges.solids || edges.remembered || edges.known) && s.nowS - d.corridorLogAt >= 2.0))) {
                d.corridorLogAt = s.nowS;
                logger::Write("OBS system=plan.corridor vehicle=%08X shift=%+.2f target=%+.2f left=%.2f right=%.2f edges=%d remembered=%d memory=%d known=%d why=%s reason=%s",
                    s.vehicle, d.corridorShift, edges.target, std::isfinite(edges.left) ? edges.left : -99.0,
                    std::isfinite(edges.right) ? edges.right : 99.0, edges.solids, edges.remembered, edges.memory, edges.known, edges.why,
                    edges.known ? "known_band_or_median_edge_counted_with_the_measured_ones" : "measured_edges_only");
            }
#endif
        } else { d.corridorShift = 0; d.corridorAt = -1; }
        Result r;
        if (scene.route.size() >= 2) {
            double station, lateral, half; scene.Route(s.car.x, s.car.y, station, lateral, half);
            const double goal = std::min(station + 30.0, scene.route.back().station - 2.0);
            std::shared_ptr<const planlattice::Field> field; std::uint64_t fieldAt; double gx, gy;
            { std::lock_guard lk(d.lock); field = d.field; fieldAt = d.fieldAt; gx = d.fieldGoalX; gy = d.fieldGoalY; }
            // The field is in world coordinates; route stations restart with every snapshot, so how far the car still is from the field's goal
            // is measured to the goal POINT (straight line: a little short on a bend, which only rebuilds a little early).
            const double goalAhead = field ? std::hypot(gx - s.car.x, gy - s.car.y) : 0.0;
            const double ageS = field ? (GetTickCount64() - fieldAt) / 1000.0 : 1e9;
            if (FieldStale(field != nullptr, s.car.x, s.car.y, field ? field->x0 : 0, field ? field->y0 : 0, field ? field->p.size : 0, ageS, goalAhead)
                && (!s.budgeted || g_fieldsInFlight.load(std::memory_order_relaxed) < budget::fieldsInFlight)
                && !d.fieldBusy.test_and_set(std::memory_order_acquire)) {
                if (s.budgeted) g_fieldsInFlight.fetch_add(1, std::memory_order_relaxed);
                auto fj = std::make_unique<FieldJob>(); fj->scene = scene; fj->spec = s.spec; fj->goal = goal; fj->hint = station; fj->d = &d; fj->budgeted = s.budgeted;
                const auto at = std::lower_bound(scene.route.begin(), scene.route.end(), goal, [](const planscene::RoutePoint& p, double v) { return p.station < v; });
                const auto& end = at == scene.route.end() ? scene.route.back() : *at;
                fj->cx = .5 * (s.car.x + end.x); fj->cy = .5 * (s.car.y + end.y); fj->gx = end.x; fj->gy = end.y; fj->carX = s.car.x; fj->carY = s.car.y;
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&FieldWork), &fj->module)) {
                    auto* raw = fj.release(); if (!TrySubmitThreadpoolCallback(FieldWork, raw, nullptr)) { const bool sh = raw->budgeted; FreeLibrary(raw->module); delete raw; if (sh) g_fieldsInFlight.fetch_sub(1); d.fieldBusy.clear(); }
                } else { if (s.budgeted) g_fieldsInFlight.fetch_sub(1); d.fieldBusy.clear(); }
            }
            // D12 (removal step 1): the health check's spot test asks the energy plan for its pulse (the old branch planner did this before).
            energyplan::TestPulse pulse; if (f.test.active) { pulse.active = true; pulse.speed = f.test.reverse ? -std::max(.3, std::fabs(f.test.speed)) : std::max(.3, std::fabs(f.test.speed)); }
            energyplan::Weights gripWt; gripWt.grip = f.grip;  // one car: the plan's sideways limit and steepest grade come from the gear's grip (carcap)
            const bool checkFloor = !s.budgeted && !pulse.active && s.car.mode == 0 &&
                std::fabs(s.car.speed) < 2.0 && scene.travel != 2 &&
                (s.nowS - d.floorProbeAskedAt >= .25 || s.nowS < d.floorProbeAskedAt);
            bool traceBlock = checkFloor;
#ifndef FVO_RELEASE
            traceBlock |= !s.budgeted;
#endif
            const auto p = energyplan::Plan(s.car, s.spec, f.want, scene, d.memory, s.nowS, gripWt, station,
                scene.travel == 2 ? nullptr : field.get(), scoring, &pulse, s.floorPointCleared, traceBlock);
            TraceManagedHalt(d, s, p);
#ifndef FVO_RELEASE
            if (!scene.legal.empty() && s.car.mode == 0) {  // known no-go (route edge bands, medians): the car's own state and the rollouts refused for it
                double yaw = s.car.heading, a = 0, b = 0, h = 0; scene.Route(s.car.x, s.car.y, a, b, h, station, &yaw);
                const double depth = scene.NoGoDepth(lateral, std::remainder(s.car.heading - yaw, 6.283185307179586), s.spec);
                const bool inside = depth > energyplan::kNoGoTolerance;
                int refused = 0; for (int q = 0; q < p.rejectKinds; ++q) if (std::strcmp(p.rejectName[q], "enters_a_known_band") == 0) refused = p.rejectCount[q];
                if (inside != d.nogoInside || (refused != d.nogoRefused && s.nowS - d.nogoLogAt >= 2.0)) {
                    d.nogoLogAt = s.nowS; d.nogoInside = inside; d.nogoRefused = refused;
                    logger::Write("OBS system=plan.nogo vehicle=%08X event=%s depth=%.2f lateral=%.2f refused_candidates=%d intervals=%d reason=%s",
                        s.vehicle, inside ? "body_in_known_no_go" : "body_clear_of_known_no_go", depth, lateral, refused, int(scene.legal.size()),
                        refused ? "candidate_paths_into_a_band_or_median_refused_unless_leaving_one" : "route_edge_band_and_median_are_known_not_sensed");
                }
            }
#endif
            const auto& block = p.centreBlock;
            if (checkFloor && block.valid && block.shape.floorProbeBody && block.shape.half <= .21) {
                const auto& hit = block.shape;
                d.floorProbeAskedAt = s.nowS;
                const auto& last = d.lastFloorProbe;
                const bool tooSoon = last && last->vehicle == s.vehicle && last->body == hit.floorProbeBody &&
                    s.nowS >= last->snapshot && s.nowS - last->snapshot < 1.5 &&
                    std::hypot(hit.x - last->x, hit.y - last->y) <= .02 && std::fabs(hit.top - last->z) <= .02;
                if (!tooSoon) {
                    d.lastFloorProbe = FloorProbeRequest{s.vehicle, hit.floorProbeBody, s.nowS,
                        hit.x, hit.y, hit.top, s.car.x, s.car.y, s.car.z, s.spec.height};
                    std::lock_guard lk(d.lock);
                    d.floorProbe = d.lastFloorProbe;
                }
            }
#ifndef FVO_RELEASE
            if (!s.budgeted) TraceSensors(d, s, block);
#endif
            r.pulse = p.pulse; r.testRefused = p.testRefused;
            if (p.pulse || p.testRefused) r.spotPulseSerial = f.test.serial;
            r.factorySpotPulse = p.pulse && f.test.factory;
            if (p.layerChanged)  // RV-1 trace: each change of the mode or the routine's step, and why
                logger::Write("OBS system=plan.mode vehicle=%08X mode=%s step=%s reason=%s chose=%s energy=%.2f feasible=%d lane_err=%.3f leg_f=%d leg_faces=%d leg_r=%d rec_tries=%d rec_cycles=%d turn_sign=%d",
                    s.vehicle, p.layer, p.step, p.layerWhy, p.why ? p.why : "", p.energy, p.feasible,
                    p.laneErr, p.legF, p.legFaces, p.legR, p.recTries, p.recCycles, p.turnSign);
            r.ok = p.ok; r.why = p.why; r.energy = p.energy; r.clearance = p.minClearance; r.gears = p.gearChanges; r.feasible = p.feasible; r.candidates = p.candidates; r.field = field != nullptr;
            r.path = DrivenPath(p.traj, energyplan::kSteps + 1, r.reverse);
            r.hoverPath = hoverfollow::FromTraj(p.traj, energyplan::kSteps + 1, energyplan::kDt);  // step 5
            r.speed = p.first.speed; r.hover = p.firstMode == 1 || p.traj[1].mode == 1;  // hovering or still coming down: not a wheels path yet
            r.laneCentre = LaneCentrePath(p, scene, s.car, s.spec, station, lateral, half, r.reverse, r.hover, energyplan::Margin(s.car.speed, s.spec));  // the line keeps the plan's own collision margin (v x reaction) from the edges
            if (p.layerChanged && std::strcmp(p.layerWhy, "verified_floor_retry") == 0)
                d.floorRetryUntil = s.nowS + .75;
            if (r.reverse && r.speed > 0) r.speed = 0; if (!r.reverse && r.speed < 0) r.speed = 0;  // the first move and the path agree (a gear change waits for a stop)
            r.kept = p.kept; r.rival = p.rival; r.rivalGap = p.rivalGap; r.switchWhy = p.switchWhy; r.offset = p.offset; r.speedAhead = r.factorySpotPulse ? r.speed : p.speedAhead; r.terms = p.terms; for (int q = 0; q < 6; ++q) r.rel[q] = p.rel[q]; r.fullSpeed = p.fullSpeed; r.fullEnergy = p.fullEnergy; r.fullTerms = p.fullTerms;
    r.rejectKinds = p.rejectKinds; for (int q = 0; q < p.rejectKinds; ++q) { r.rejectName[q] = p.rejectName[q]; r.rejectCount[q] = p.rejectCount[q]; } r.startGrade = p.startGrade; r.maxGrade = p.maxGrade; r.floorSetAside = p.floorSetAside;
    r.laneSpeedCount = p.laneSpeedCount; for (int q = 0; q < p.laneSpeedCount; ++q) r.laneSpeeds[q] = p.laneSpeeds[q];
            if (!r.hover && !r.pulse && d.floorRetryUntil > s.nowS) {
                r.speed = std::clamp(r.speed, -wheelguard::crawl, wheelguard::crawl);
                r.speedAhead = std::clamp(r.speedAhead, -wheelguard::crawl, wheelguard::crawl);
            }
        }
        r.vehicle = s.vehicle;
        r.workerMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        r.at = GetTickCount64();
        { std::lock_guard lk(d.lock); d.latest = r; }
        }  // lane scene
    } catch (...) { logger::Write("OBS system=plan.energy event=failed reason=plan_worker_exception"); }
    if (budgetJob) g_plansInFlight.fetch_sub(1, std::memory_order_relaxed);
    d.planBusy.clear(std::memory_order_release);
}

// Hand a job to the pool; false (and the job freed) when it could not be submitted.
inline bool Queue(std::unique_ptr<PlanJob> job) {
    job->s.relocation = relocconsumers::Poll(job->s.vehicle);   // game thread: the Coordinator is read where it is written
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&PlanWork), &job->module)) return false;
    auto* raw = job.release(); if (TrySubmitThreadpoolCallback(PlanWork, raw, nullptr)) return true;
    FreeLibrary(raw->module); delete raw; return false;
}
template <class Line, class Wanted>
inline void FillLane(Snapshot& s, std::uint32_t vehicle, const planrun::Frame& f, const Line& line, const Wanted& wanted, const std::vector<planworld::Shape>& shapes,
                     const bicycle::State& car, const bicycle::Spec& spec) {
    s.trafficHoldReason = f.heldWhy && *f.heldWhy ? f.heldWhy : "traffic_hold_reason_unavailable";
    s.vehicle = vehicle; s.frame = f; s.frame.slice.memory = nullptr; s.frame.field = nullptr; s.frame.extra = nullptr;
    s.frame.airWhy = s.frame.groundWhy = s.frame.heldWhy = s.frame.wantWhy = nullptr;
    if (f.fieldCount && f.field) s.field.assign(f.field, f.field + f.fieldCount);
    s.shapes = shapes; s.car = car; s.spec = spec; s.nowS = GetTickCount64() / 1000.0;
    for (int i = -20; i <= 80; ++i) s.line[i + 20] = line(i);
    for (int i = 0; i < planrun::kSamples; ++i) s.floor[i] = wanted(i * planrun::kStep, 0);
}

// Game thread: hand a copied snapshot to the plan worker if it is free (otherwise this update's data is simply not used). ED-1, slot 0.
template <class Line, class Wanted>
inline void Submit(std::uint32_t vehicle, const planrun::Frame& f, const Line& line, const Wanted& wanted, const std::vector<planworld::Shape>& shapes,
                   const bicycle::State& car, const bicycle::Spec& spec, bool floorPointCleared = false) {
    auto& d = The();
    if (d.planBusy.test_and_set(std::memory_order_acquire)) return;
    try {
        auto job = std::make_unique<PlanJob>(); job->d = &d;
        FillLane(job->s, vehicle, f, line, wanted, shapes, car, spec);
        job->s.floorPointCleared = floorPointCleared;
        if (Queue(std::move(job))) return;
    } catch (...) {}
    d.planBusy.clear(std::memory_order_release);
}

// A lane car other than yours: its snapshot for its own slot, within the budget (budget::MaySubmit). Returns whether it was handed over.
template <class Line, class Wanted>
inline bool SubmitLane(int slot, std::uint32_t vehicle, const planrun::Frame& f, const Line& line, const Wanted& wanted, const std::vector<planworld::Shape>& shapes,
                         const bicycle::State& car, const bicycle::Spec& spec, double nowS) {
    auto& d = For(slot);
    if (!budget::MaySubmit(nowS, d.submitAt, g_plansInFlight.load(std::memory_order_relaxed))) return false;
    if (d.planBusy.test_and_set(std::memory_order_acquire)) return false;
    g_plansInFlight.fetch_add(1, std::memory_order_relaxed); d.submitAt = nowS;
    try {
        auto job = std::make_unique<PlanJob>(); job->d = &d;
        FillLane(job->s, vehicle, f, line, wanted, shapes, car, spec); job->s.budgeted = true;
        if (Queue(std::move(job))) return true;
    } catch (...) {}
    g_plansInFlight.fetch_sub(1, std::memory_order_relaxed);
    d.planBusy.clear(std::memory_order_release);
    return false;
}

// D13: a car with no lane, from its goal, World shapes and measured floor points. Same budget.
inline bool SubmitGoal(int slot, std::uint32_t vehicle, const planinput::GoalAsk& ask, const std::vector<planworld::Shape>& shapes,
                       const std::vector<planinput::FloorPoint>& floor, const bicycle::State& car, const bicycle::Spec& spec, double want,
                       double nowS) {
    auto& d = For(slot);
    if (!budget::MaySubmit(nowS, d.submitAt, g_plansInFlight.load(std::memory_order_relaxed))) return false;
    if (d.planBusy.test_and_set(std::memory_order_acquire)) return false;
    g_plansInFlight.fetch_add(1, std::memory_order_relaxed); d.submitAt = nowS;
    try {
        auto job = std::make_unique<PlanJob>(); job->d = &d; auto& s = job->s;
        s.vehicle = vehicle; s.goal = true; s.goalAsk = ask; s.shapes = shapes; s.goalFloor = floor; s.car = car; s.spec = spec; s.want = want;
        s.budgeted = true; s.nowS = GetTickCount64() / 1000.0; s.frame.now = ask.now;
        if (Queue(std::move(job))) return true;
    } catch (...) {}
    g_plansInFlight.fetch_sub(1, std::memory_order_relaxed);
    d.planBusy.clear(std::memory_order_release);
    return false;
}
inline Result Latest(int slot = 0) { auto& d = For(slot); std::lock_guard lk(d.lock); return d.latest; }
}
#endif
