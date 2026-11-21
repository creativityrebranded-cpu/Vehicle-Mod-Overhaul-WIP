#pragma once
// Far tier test (2026-11-21, notes/FAR_TIER_TEST_20261121.md). Pure logic for the DIAGNOSTIC experiment that moves a seated pair past the
// loaded-area wall (about 250 m) and holds it on a kinematic "rail": each physics step the chassis is placed along a straight path at a set speed,
// its height taken from terrain data plus a ride height. No engine calls, no clock, no threads of its own (the profile is read by the physics
// thread and written by the main thread through atomics). Nothing here is built into player builds: the callers are under #ifndef FVO_RELEASE.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace farrail {

constexpr double kStartDistanceM = 300.0;   // first test distance: a distance the loaded-area wall (~250 m) cannot contain
constexpr double kStepM = 50.0;             // each further run adds this
constexpr double kMaxDistanceM = 600.0;     // named maximum
constexpr double kHopM = 20.0;              // one doer write (equals relocation::kMaxHopM; farrail_test pins it)
constexpr double kHopToleranceM = 3.0;      // an intermediate hop is a waypoint
constexpr double kArriveToleranceM = 1.5;   // the last hop (the step adds its own motion)
constexpr std::uint64_t kHopWindowMs = 3000;
constexpr double kObserveSeconds = 30.0;    // stage 1 observation, and the rail's first summary
constexpr double kParkedAutoReturnS = 120.0;// stage 1: the pair is walked back this long after arrival if nobody pressed
constexpr double kRailAutoReturnS = 60.0;   // stage 2: the pair is walked back this long after the rail started
constexpr double kRailSpeed = 15.0;         // default rail speed, m/s
constexpr double kRailMaxVertical = 8.0;    // m/s: the rail's own limit on how fast the height may change
constexpr double kMinRide = 0.0, kMaxRide = 10.0;
constexpr double kPathCapM = 1400.0;        // the rail never runs past this path length
constexpr double kProfileSpacingM = 8.0;    // the same spacing as the 8 m terrain grid (cellbakegrid.h)
constexpr int kProfileCap = 192;            // 192 * 8 m = 1536 m, above kPathCapM
constexpr double kFallAbortM = 60.0;        // stage 1: this far below the arrival height the observation stops and the pair is walked back
constexpr double kJumpM = 25.0;             // a position change this large between two 0.5 s samples that nobody asked for = something moved it
constexpr unsigned kMaxHopsOut(double distance) { return unsigned(distance / kHopM) + 12; }

// Terrain sources, in the order the test asks them (Cached is the heightfield the map reads: its window is 100 cells, far past the wall).
enum class Source : int { None = -1, Cached = 0, Cellbake = 1, NavMap = 2, Ray = 3 };
constexpr int kSources = 4;
inline const char* SourceName(Source s) {
    switch (s) { case Source::Cached: return "cached_heightfield"; case Source::Cellbake: return "cellbake_grid"; case Source::NavMap: return "navmap_ground"; case Source::Ray: return "down_ray"; default: return "none"; }
}

// The distance of run number `run` (0 first): 300, 350, ... capped at the named maximum.
inline double TargetDistance(unsigned run) {
    const double d = kStartDistanceM + kStepM * double(run);
    return d > kMaxDistanceM ? kMaxDistanceM : d;
}
// Distances along the heading at which every source is asked before the move: 0, 50, ... through the target (and the rail's first stretch).
inline int ProbeCount(double extent) { return int(std::floor(extent / kStepM + 1e-9)) + 1; }
inline double ProbeDistance(int i) { return kStepM * double(i); }

// Which source feeds the rail: the one that answered at every probe point; else the one with most answers beyond the wall (>= 250 m); else none.
// answered[s][i]: source s answered at probe i. points: how many probes. Ties go to the earlier source in the order above.
struct Coverage { int all = 0; int beyond = 0; };
inline Source PickSource(const Coverage (&c)[kSources], int points, int beyondPoints) {
    int best = -1;
    for (int s = 0; s < kSources; ++s) if (points > 0 && c[s].all == points) return Source(s);
    int most = 0;
    for (int s = 0; s < kSources; ++s) if (c[s].beyond > most) { most = c[s].beyond; best = s; }
    (void)beyondPoints;
    return best < 0 ? Source::None : Source(best);
}

// Ride height: how far the chassis origin stands over the terrain at the start (measured, where the source answers there).
inline double RideFromStart(double chassisZ, double groundZ, bool groundKnown, double fallback = 1.5) {
    if (!groundKnown || !std::isfinite(chassisZ) || !std::isfinite(groundZ)) return fallback;
    const double r = chassisZ - groundZ;
    return r < kMinRide || r > kMaxRide ? fallback : r;
}

// ---- Terrain profile along the path (index i = path length i * 8 m). Main thread writes, the physics thread reads. ----
class Profile {
    std::atomic<float> z_[kProfileCap];
public:
    Profile() { Clear(); }
    void Clear() { for (auto& a : z_) a.store(std::numeric_limits<float>::quiet_NaN(), std::memory_order_relaxed); }
    static int Count() { return kProfileCap; }
    static int IndexOf(double s) { return s < 0 ? -1 : int(std::floor(s / kProfileSpacingM + 1e-9)); }
    void Set(int i, double z) { if (i >= 0 && i < kProfileCap && std::isfinite(z)) z_[i].store(float(z), std::memory_order_relaxed); }
    bool Known(int i) const { return i >= 0 && i < kProfileCap && std::isfinite(z_[i].load(std::memory_order_relaxed)); }
    int KnownCount() const { int n = 0; for (int i = 0; i < kProfileCap; ++i) n += Known(i); return n; }
    // Height at path length s: linear between the two neighbouring samples; at the last known sample the nearest one; false when neither neighbour is known.
    bool Get(double s, double& z) const {
        const int i = IndexOf(s);
        if (i < 0 || i >= kProfileCap) return false;
        const double a = z_[i].load(std::memory_order_relaxed);
        const bool ka = std::isfinite(a);
        const double b = i + 1 < kProfileCap ? double(z_[i + 1].load(std::memory_order_relaxed)) : std::numeric_limits<double>::quiet_NaN();
        const bool kb = std::isfinite(b);
        if (ka && kb) { const double t = (s - i * kProfileSpacingM) / kProfileSpacingM; z = a + (b - a) * std::clamp(t, 0.0, 1.0); return true; }
        if (ka) { z = a; return true; }
        return false;
    }
};

// ---- The rail step ----
struct RailParams { double x0 = 0, y0 = 0, dirX = 0, dirY = 1, speed = kRailSpeed, ride = 1.5, maxVertical = kRailMaxVertical; };
struct RailState { double s = 0, z = 0; std::uint64_t steps = 0, holds = 0; };
struct RailOut { double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0, target = 0; bool terrainKnown = false, capped = false; };

// One physics step of dt seconds: advance along the path at the set speed (never past the cap), take the height from the terrain plus the ride
// (rate-limited, and held where the terrain is not known), and give the velocity that matches the move so the integrator adds nothing of its own.
inline RailOut Step(RailState& st, const RailParams& p, double dt, bool terrainKnown, double terrainZ) {
    RailOut o;
    if (!(dt > 0) || !std::isfinite(dt)) { o.x = p.x0 + p.dirX * st.s; o.y = p.y0 + p.dirY * st.s; o.z = st.z; o.target = st.z; return o; }
    double ds = p.speed * dt;
    if (st.s + ds > kPathCapM) { ds = std::max(0.0, kPathCapM - st.s); o.capped = true; }
    st.s += ds;
    o.terrainKnown = terrainKnown && std::isfinite(terrainZ);
    o.target = o.terrainKnown ? terrainZ + p.ride : st.z;
    if (!o.terrainKnown) ++st.holds;
    const double lim = p.maxVertical * dt;
    const double dz = std::clamp(o.target - st.z, -lim, lim);
    st.z += dz;
    ++st.steps;
    o.x = p.x0 + p.dirX * st.s; o.y = p.y0 + p.dirY * st.s; o.z = st.z;
    const double along = ds / dt;
    o.vx = p.dirX * along; o.vy = p.dirY * along; o.vz = dz / dt;
    return o;
}

// ---- Hops (the proven doer write, one bounded hop at a time) ----
struct P3 { double x = 0, y = 0, z = 0; };
inline double Dist(const P3& a, const P3& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
// The next hop from the measured position toward `to`: at most kHopM; last says it reaches `to`. Recomputed from where the chassis is, so no drift accumulates.
inline P3 NextHop(const P3& now, const P3& to, bool& last) {
    const double d = Dist(now, to);
    if (d <= kHopM) { last = true; return to; }
    last = false;
    const double k = kHopM / d;
    return { now.x + (to.x - now.x) * k, now.y + (to.y - now.y) * k, now.z + (to.z - now.z) * k };
}
inline unsigned HopsFor(double distance) { return distance > 0 && std::isfinite(distance) ? unsigned(std::ceil(distance / kHopM - 1e-9)) : 0u; }

enum class Hop : int { Wait = 0, Arrived, Failed };
// One hop's read-back. Forward hops require the seat link (a lost driver stops the move); return hops do not (the chassis must come home either way).
// A chassis that has not completed a step yet waits; one whose physics is not live waits for the window, then fails (return hops) or fails at once (forward).
inline Hop JudgeHop(bool bodyValid, bool live, std::uint64_t steps, double distanceToHopTarget, double tolerance, bool seated, bool requireSeat,
                    bool windowExpired, const char*& why) {
    if (!bodyValid) { why = "body_changed"; return Hop::Failed; }
    if (!live) { if (requireSeat || windowExpired) { why = "physics_not_live"; return Hop::Failed; } why = "physics_not_live_waiting"; return Hop::Wait; }
    if (steps < 1) { why = "no_fresh_step"; return Hop::Wait; }
    if (distanceToHopTarget > tolerance) { why = "hop_not_at_target"; return Hop::Failed; }
    if (requireSeat && !seated) { why = "seat_link_lost"; return Hop::Failed; }
    why = "hop_arrived"; return Hop::Arrived;
}

// ---- Observation tally ----
struct Sample {
    bool modelLoaded = true;   // chassis and driver both have a 3D model
    bool seated = true;
    bool kept = true;          // Keep in sight holds it
    bool physicsBound = true, physicsLive = true;
    bool bodyReadable = true;
    double z = 0;              // chassis height
    double errM = std::numeric_limits<double>::quiet_NaN();   // rail only: chassis height minus (terrain + ride); NaN where the terrain is unknown or no rail
    bool terrainKnown = false;
    double jumpM = 0;          // position change since the previous sample that nobody commanded
    double speed = 0;          // horizontal speed read from consecutive samples
};
struct Tally {
    unsigned samples = 0, modelLost = 0, notSeated = 0, notKept = 0, physicsDead = 0, unreadable = 0, terrainUnknown = 0, jumps = 0, errSamples = 0, moving = 0;
    double maxAbsErr = 0, sumAbsErr = 0, startZ = 0, minZ = 1e18, maxZ = -1e18, maxJump = 0, maxSpeed = 0;
    bool haveStart = false;
    void Add(const Sample& s) {
        if (!haveStart) { startZ = s.z; haveStart = true; }
        ++samples;
        modelLost += !s.modelLoaded; notSeated += !s.seated; notKept += !s.kept; physicsDead += !(s.physicsBound && s.physicsLive); unreadable += !s.bodyReadable;
        terrainUnknown += !s.terrainKnown;
        if (s.bodyReadable) { minZ = std::min(minZ, s.z); maxZ = std::max(maxZ, s.z); }
        if (s.jumpM >= kJumpM) { ++jumps; maxJump = std::max(maxJump, s.jumpM); }
        if (std::isfinite(s.errM)) { ++errSamples; const double a = std::fabs(s.errM); maxAbsErr = std::max(maxAbsErr, a); sumAbsErr += a; }
        if (s.speed > 1.0) ++moving;
        maxSpeed = std::max(maxSpeed, s.speed);
    }
    double MeanAbsErr() const { return errSamples ? sumAbsErr / errSamples : 0.0; }
    double Fall() const { return haveStart && minZ < 1e17 ? startZ - minZ : 0.0; }
};
// A jump nobody asked for: the chassis moved this far in dt seconds while the rail (or nothing) was supposed to move it `expected` metres.
inline double UncommandedJump(const P3& prev, const P3& now, double expected) {
    const double h = std::sqrt((now.x - prev.x) * (now.x - prev.x) + (now.y - prev.y) * (now.y - prev.y));
    return std::max(0.0, h - std::max(0.0, expected));
}
// One-grep verdict words for the summary line.
inline const char* Stage1Verdict(const Tally& t) {
    if (!t.samples) return "no_samples";
    if (t.unreadable == t.samples) return "body_unreadable";
    if (t.modelLost) return "model_lost";
    if (t.notSeated) return "driver_not_seated";
    if (t.jumps) return "moved_by_something_else";
    if (t.Fall() > 5.0) return "fell";
    return "stayed_loaded_and_still";
}
inline const char* Stage2Verdict(const Tally& t) {
    if (!t.samples) return "no_samples";
    if (t.unreadable == t.samples) return "body_unreadable";
    if (t.modelLost) return "model_lost";
    if (t.notSeated) return "driver_not_seated";
    if (t.jumps) return "moved_by_something_else";
    if (t.errSamples == 0) return "no_terrain_answer";
    if (t.maxAbsErr > 2.0) return "height_error_over_2m";
    return "rail_held";
}
inline const char* ReturnVerdict(unsigned samples, unsigned liveSamples, unsigned seatedSamples, unsigned movingSamples, unsigned censusBroken, double fall) {
    if (!samples) return "no_samples";
    if (liveSamples < samples) return "physics_not_clean";
    if (seatedSamples < samples) return "driver_not_seated";
    if (censusBroken) return "census_says_broken";
    if (fall > 3.0) return "fell";
    if (!movingSamples) return "clean_but_not_driving";
    return "clean_and_driving";
}

}  // namespace farrail
