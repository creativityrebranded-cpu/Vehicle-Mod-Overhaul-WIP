#pragma once
// Spawn flow v3 (notes/SPAWN_FLOW_20261106.html; WORKING_MANDATE.md "Spawn flow, one purpose per step"; change cards in
// notes/spawn-build-20261107/CHANGE_CARDS.md). One spawn tick in code order. Each step owns one purpose and only its own output:
//   1 Window (when)  2 Census (every pair and its one state)  3 Demand (wanted slots and models)  4 Arbiter (which slot, create now)
//   5 Siting (the birth pose or one reason)  6 Birth (make it, frozen; generator.cpp, through the service queue, decides nothing)
//   7 Health (the only judge; never removes)  8 Handoff (release to the driving pipeline)  9 Retirement (the only remover)  10 Ledger.
// Steps 3-6 run only while the Window is open. Every value a step decides with lives in `rules` below.
// Pure: no engine calls, no files. Tests: tests/spawnflow_test.cpp.
#include "../fieldbox.h"
#include "../fieldboxapi.h"
#include "../planowner.h"
#include "../spawnrule.h"
#include "spawnhealth.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace spawnflow {
// The rules table: every spawn decision value (mandate card item 7).
namespace rules {
inline constexpr double dropMargin = .3;            // m: births stand this far above the field floor and drop (user, 2026-11-06)
inline constexpr int maxCreatePerTick = 1;          // Arbiter: at most one create request per tick
inline constexpr int maxWaitingCovered = 12;        // Arbiter: pairs still being proved at once (loading screen / X panel)
inline constexpr int maxLookCovered = 32;           // Arbiter: slots looked at per tick
// Health's verdict values (models, spots, every stage, the seat time) live in spawnhealth::rules: one rule (card HR1).
inline constexpr unsigned long long seatProveMs = spawnhealth::rules::seatProveMs;  // minimum observed play time to record a terminal seat failure
inline constexpr double fitMargin = .15;            // Siting: the body's half width needs this much room inside the box
inline constexpr double departureGapRadii = 1.0;    // Siting: forward clearance in units of the new chassis radius
inline constexpr unsigned long long noBodyMs = 30000;   // Health: staged with a model but no physics body this long is a fault
inline constexpr double fellBelow = 200.0, fellBelowFloor = 20.0;  // Health: this far under its birth height (or the known floor), a pair has gone through the world
inline constexpr double uncontrolledDownSpeed = 30.0, uncontrolledSideSpeed = 5.0;  // Health: falling through the world versus following a slope or descending normally
inline constexpr unsigned long long fallSampleMinMs = 250, fallSampleMaxMs = 2500;
inline constexpr unsigned long long judgeEveryMs = 1000;  // Health: how often a pair is judged
inline constexpr unsigned long long maxObservedWaitMs = 1500;
inline constexpr unsigned long long seatedNoTripMs = 60000;  // SR-2 Health: seated and still not released (no trip) this long in play: broken seated_no_trip
inline constexpr unsigned long long emptyConfirmMs = 30000;  // EV-1 Health: a handed-off traffic vehicle with nobody of its pair in the seat this long (play) is empty
inline constexpr unsigned long long readinessEveryMs = 5000;  // Ledger: a waiting pair's readiness line
// Card CP-1: Retirement by camera prediction (settings.ini [traffic] removeByCameraPrediction=1; default off = far and out of sight only).
inline constexpr double predictSeconds = 0.5;       // Retirement: a half must stay out of view this long ahead, camera turn extrapolated
inline constexpr double fallVisibilityRadius = 72.0;  // Retirement: covers the existing 40 m per-axis bounds plus hull padding
inline constexpr double undergroundClearance = 200.0;  // Retirement: a confirmed fallen model this far under baked terrain counts as concealed
// Live road fill in play (LV-3, 2026-11-20; replaces the test-only spawnNearPlayer mode, card TS1): the Arbiter's play limits. Every value is a first,
// conservative choice: no creation cost was ever logged (the 2026-11-20 22:49 X fill: chassis 0.28 s and driver 0.12 s from queue to completion, no frame
// data), so the build logs traffic.live create_ms and tick_ms for the next run to measure.
inline constexpr int maxWaitingPlay = 2;            // Arbiter in play: pairs still being proved at once
inline constexpr int maxLookPlay = 8;               // Arbiter in play: slots looked at per tick (checks are cheap; one create per tick at most)
inline constexpr double liveRadius = 240.0;         // Demand: a slot within this horizontal distance of the player is in the live stretch (the loaded area's wall is ~250 m: loadedarea notes)
}

// ---------- 1 Window: when spawning may happen ----------
struct Window { bool open = false, cover = false; const char* why = ""; bool live = false; };  // live: the play road window (LV-3); generator.cpp builds Windows by aggregate {open, cover, why}
// screen: 3 loading screen, 2 local map / X panel, 1 play, 0 none. reset: X's fill is running. clockPending: routes wait for the clock.
// live: the play rule (spawnrule::LiveOpen, decided by the caller from the settle time) also opens it in play, for the road fill only.
inline Window WindowNow(int screen, bool reset, bool clockPending, bool live = false, bool panelCover = true) {
    Window w; w.cover = screen >= 2;
    if (!screen) { w.why = "no_screen"; return w; }
    if (live && screen == 1 && !clockPending) { w.open = true; w.live = true; w.why = "live_road"; return w; }
    if (!spawnrule::Open(screen, reset)) { w.why = screen == 1 ? "in_play" : "map_without_x"; return w; }
    if (screen == 2 && !panelCover) { w.why = "x_panel_not_covering"; return w; }
    if (clockPending) { w.why = "clock_pending"; return w; }
    w.open = true; w.why = screen == 3 ? "loading_screen" : "x_panel";
    return w;
}

// ---------- 2 Census: the one state per pair ----------
// The traffic part of the driving design's Owner Record (planowner::Table, Owner::Traffic). World = a stronger owner (player, job,
// combat) or the world itself (dead, out on the ground); traffic does nothing to it.
enum class Life : int { None, Born, Proving, Driving, Recovering, Broken, World, Retiring, Gone };
inline const char* Name(Life l) { constexpr const char* n[]{ "none", "born", "proving", "driving", "recovering", "broken", "world", "retiring", "gone" }; return n[int(l)]; }
// Census rejects transitions the flow never makes, so a bug shows as a refused change in the Ledger, not as a silent state.
inline bool Allowed(Life from, Life to) {
    if (from == to) return true;
    switch (to) {
    case Life::None: return true;                                             // a slot emptied (reset, save load)
    case Life::Born: return from == Life::None || from == Life::Gone;
    case Life::Proving: return from == Life::Born || from == Life::None;      // None: a pair restored from the save
    case Life::Driving: return from == Life::Proving || from == Life::Recovering || from == Life::None || from == Life::Born;
    case Life::Recovering: return from == Life::Driving || from == Life::Proving;
    // EV-1 (2026-11-07, user: "we cant have empty vehicles"): a pair the world had (driver dead or out) becomes Broken once Health
    // confirms its vehicle empty; Retirement then removes both halves out of sight. World -> Retiring stays refused.
    case Life::Broken: return from != Life::Retiring && from != Life::Gone;
    case Life::World: return from != Life::Retiring && from != Life::Gone;
    case Life::Retiring: return from != Life::Gone && from != Life::World;
    case Life::Gone: return from == Life::Retiring;
    }
    return false;
}
// Census to the Owner Record's traffic metadata (planowner::Life has no World or Gone: those rows are not traffic's).
inline planowner::Life ToOwner(Life l) {
    switch (l) {
    case Life::Born: return planowner::Life::Born; case Life::Proving: return planowner::Life::Proving;
    case Life::Driving: return planowner::Life::Driving; case Life::Recovering: return planowner::Life::Recovering;
    case Life::Broken: return planowner::Life::Broken; case Life::Retiring: return planowner::Life::Retiring;
    default: return planowner::Life::None;
    }
}
// Control the core readers consume (trafficcensusapi.h): 0 held, 1 driving pipeline, 2 world's, 3 retire.
inline int Control(Life l) { return l == Life::Driving ? 1 : l == Life::World ? 2 : (l == Life::Broken || l == Life::Retiring || l == Life::Gone) ? 3 : 0; }

// ---------- 7 Health records (read by Demand and Siting) ----------
// spawnhealth::Store (card HR1): every stage, models and spots, one rule; kept across loads by the generator (card HR2).
using Records = spawnhealth::Store;

// ---------- 3 Demand: a model for a wanted slot ----------
// Your Types first (userSkips: the route/lane vehicle rules and [types] skip), Health's model verdicts second (card DR1):
// a rejected model is left out unless you allowed it or its trial is due; of the rest, the first in the dealt order with odds of at
// least spawnhealth::rules::preferOdds, else the one with the best odds. order: the slot's dealt order (VehicleBase's seeded round).
// Returns 0 with *why when nothing is allowed; a pick names a trial ("health_trial") or low odds ("best_odds_left").
template <class UserSkips>
unsigned Model(const std::vector<unsigned>& order, std::size_t start, UserSkips userSkips, Records& health, const char** why) {
    *why = order.empty() ? "no_vehicle_types" : "every_type_left_out";
    bool healthOnly = false; unsigned best = 0; double bestOdds = -1;
    for (std::size_t k = 0; k < order.size(); ++k) {
        const unsigned id = order[(start + k) % order.size()];
        if (userSkips(id)) continue;
        if (health.ModelRejected(id)) { if (health.ModelTrial(id)) { *why = "health_trial"; return id; } healthOnly = true; continue; }
        const double odds = health.Odds(id);
        if (odds >= spawnhealth::rules::preferOdds) { *why = ""; return id; }
        if (odds > bestOdds) { bestOdds = odds; best = id; }
    }
    if (best) { *why = "best_odds_left"; return best; }
    if (healthOnly) *why = "health_rejected_every_allowed_type";
    return 0;
}

// ---------- 4 Arbiter: which slot first, create now ----------
struct Candidate { int slot = -1; double distance = 0; unsigned coverage = 0; };
// Nearest the player first; equal distance keeps slot order (stable, so the same save fills the same way).
inline void Order(std::vector<Candidate>& c) {
    std::stable_sort(c.begin(),c.end(),[](const Candidate& a,const Candidate& b){
        return a.coverage==b.coverage?a.distance<b.distance:a.coverage<b.coverage;
    });
}
// createLimit: the Arbiter's per-tick create limit (default the design's 1). In flight: rules::maxWaitingCovered under cover, rules::maxWaitingPlay in play.
inline bool MayCreate(const Window& w, int madeThisTick, int waiting, int createLimit = rules::maxCreatePerTick) { return w.open && madeThisTick < createLimit && waiting < (w.cover ? rules::maxWaitingCovered : rules::maxWaitingPlay); }
// The speaker spends one grant on a fleet pair or a driver repair. Completing either operation spends no grant.
struct BirthBudget {
    int granted = 0;
    int limit = rules::maxCreatePerTick;   // creates per tick
    bool Admit(const Window& window, bool repair) {
        if (!window.open || (repair && !window.cover) || granted >= limit) return false;
        ++granted;
        return true;
    }
};

// ---------- 4b Live road fill in play (LV-3, 2026-11-20) ----------
// In play the arbiter fills a physical fleet shortfall in the same slot order as the covered fill; the N rings (trafficsight.h) only rank it.
// Its limits are rules::maxWaitingPlay and rules::maxLookPlay above.

// ---------- 5 Siting: the birth pose, or one reason ----------
// Sources only: the field box at the slot (Field Provider), the model's bounds, other vehicles (occupied), Health's spot verdict.
struct Shape { double halfWidth = 1.2, halfLength = 2.3, height = 2, originAbove = 1; };  // originAbove: model origin over its lowest point
struct Pose { double x = 0, y = 0, z = 0, heading = 0; };
inline const char* VehicleSpacing(const Pose& site, double radius, double halfHeight,
                                 const Pose& other, double otherRadius, double otherHalfHeight) {
    const double dx = other.x - site.x, dy = other.y - site.y;
    if (!(std::fabs(other.z - site.z) < halfHeight + otherHalfHeight)) return nullptr;
    const double clearance = radius + otherRadius;
    if (std::hypot(dx, dy) < clearance) return "another_vehicle_there";
    const double front = dx * std::cos(site.heading) + dy * std::sin(site.heading);
    if (front <= 0 || front >= clearance + radius * rules::departureGapRadii) return nullptr;
    const double side = dy * std::cos(site.heading) - dx * std::sin(site.heading);
    return std::fabs(side) < radius ? "departure_gap_occupied" : nullptr;
}
struct Site { bool ok = false; Pose pose; double floor = 0; bool defaultBox = false; const char* why = ""; };
// The floor of a box under (x, y): its bottom face plane.
inline bool FloorAt(const fieldbox::Box& b, double x, double y, double& z) {
    const auto a = fieldbox::AxesOf(b); if (std::fabs(a.up.z) < .2) return false;   // a box on its side has no floor to stand on
    const auto p0 = fieldbox::World(b, { 0, 0, -b.halfHeight });
    z = p0.z - (a.up.x * (x - p0.x) + a.up.y * (y - p0.y)) / a.up.z; return std::isfinite(z);
}
// lane: the lane point at the slot (x, y, heading; z is ignored: the floor gives the height). haveBox false: the provider gave none.
inline Site Place(bool haveBox, const FvoFieldBoxV1& raw, Pose lane, const Shape& s, bool occupied, bool spotRejected) {
    Site r; r.pose = lane;
    if (spotRejected) { r.why = "health_rejected_spot"; return r; }
    if (!haveBox) { r.why = "no_field_box"; return r; }                 // D11 shapes, unreadable boxes, no tile: no birth there
    fieldbox::Box b; if (!fieldboxapi::ToBox(raw, b)) { r.why = "field_box_malformed"; return r; }
    r.defaultBox = fieldboxapi::IsDefault(raw);
    if (b.faces[fieldbox::Bottom] != fieldbox::State::Touch) { r.why = "no_floor_hover_tile"; return r; }  // hover-only tiles stay out for now
    if (!FloorAt(b, lane.x, lane.y, r.floor)) { r.why = "no_floor_plane"; return r; }
    const auto l = fieldbox::Local(b, { lane.x, lane.y, r.floor });
    if (std::fabs(l.y) > b.halfLength + .01) { r.why = "lane_point_outside_its_tile"; return r; }
    if (std::fabs(l.x) + s.halfWidth + rules::fitMargin > b.halfWidth) { r.why = "model_wider_than_field"; return r; }
    if (s.height + rules::dropMargin > 2 * b.halfHeight) { r.why = "model_taller_than_field"; return r; }
    if (occupied) { r.why = "another_vehicle_there"; return r; }
    r.pose.z = r.floor + s.originAbove + rules::dropMargin;
    r.ok = true; r.why = r.defaultBox ? "default_box_floor" : "authored_box_floor";
    return r;
}

// ---------- 7 Health: the pair verdict and its control ----------
struct ObservedWait {
    unsigned long long at=0, elapsed=0;
    void Observe(bool waiting,bool play,unsigned long long now) {
        if(!waiting){*this={};return;}
        if(!play){at=0;return;}
        if(at&&now>at)elapsed+=std::min<unsigned long long>(now-at,rules::maxObservedWaitMs);
        at=now;
    }
};
inline bool SeatAttemptSpent(int reply,bool pending) {return !pending&&reply!=-3&&reply!=1;}
inline bool ObserveSeatProof(bool proving,bool restoringAfterSeatRequest,bool models,bool body,bool sitting){
    return (proving||restoringAfterSeatRequest)&&models&&body&&!sitting;
}
enum class Verdict : int { Proving, Healthy, Recovering, Broken, World };
inline bool UncontrolledDescent(double side, double drop, unsigned long long ms) {
    if (ms < rules::fallSampleMinMs || ms > rules::fallSampleMaxMs) return false;
    const double seconds = ms * 0.001;
    return side <= rules::uncontrolledSideSpeed * seconds && drop >= rules::uncontrolledDownSpeed * seconds;
}
// A map floor may belong to the deck above an authored road. Require a real
// descent before treating that floor discrepancy as a terminal fault.
inline bool FellThroughWorld(bool belowMappedFloor, bool deepBelowBirth, bool descending) {
    return deepBelowBirth || (belowMappedFloor && descending);
}
// Relocation (notes/RELOCATION_DOER_AND_COMMUNICATOR_20261120.md): a pair that is being relocated gets no fall verdict from distance or
// descent; once its receipt says applied, the fall baseline is the destination instead of the birth height.
inline bool DeepBelowBase(bool haveBase, double baseZ, double z) { return haveBase && z < baseZ - rules::fellBelow; }
inline bool JudgeFall(bool relocating, bool belowMappedFloor, bool deepBelowBase, bool descending) { return !relocating && FellThroughWorld(belowMappedFloor, deepBelowBase, descending); }
inline const char* Name(Verdict v) { constexpr const char* n[]{ "proving", "healthy", "recovering", "broken", "world" }; return n[int(v)]; }
struct Facts {
    bool taken = false, dead = false, outOnGround = false, fighting = false;   // the world's
    bool unrecoverable = false, gaveUp = false;                                // broken at once
    bool seated = false;                                                       // aboard, sitting (sit state 3), together
    bool proving = false; bool proveExpired = false;                           // still being proved; its time ran out
    bool bad = false; bool badWaitOver = false;                                // a driving pair that went wrong; waited long enough
    const char* fault = nullptr;                                               // a fact seen at a step: no body, fell, floating
    bool spotFailed = false;                                                   // ST-C: failed two imperceptible spot tests (core FVO_SpotTestV1)
    bool misplaced = false;                                                    // ST-A3: off its lane or tipped at the arrival settle check
    const char* badWhy = nullptr;                                              // PF-1: which self-report made a driving pair bad (null: the core record)
    const char* empty = nullptr;                                               // EV-1: why the vehicle has nobody of its pair aboard (null: not empty)
    bool emptyConfirmed = false;                                               // EV-1: empty for rules::emptyConfirmMs of play
    bool coverClosing = false;                                                 // CC-2: the cover owner says the cover is closing now
    bool seatTriable = false;                                                  // CC-2a: both models loaded, so a seat could be tried
    unsigned long long seatedWaitMs = 0;                                       // SR-2: play time seated while still being proved (waiting for release)
};
// SR-3 Demand: a new pair only when its route has trip room left beyond the pairs of that route still being proved. left < 0: unknown
// (no limit applied).
inline bool RouteHasRoom(int left, int provingSameRoute) { return left < 0 || provingSameRoute < left; }
// EV-1: is a handed-off traffic vehicle empty? Someone else in its driver's seat (the player, a companion, a job) is not empty: that is
// not traffic's to judge. Returns the cause, or null.
inline const char* EmptyCause(bool driverRef, bool driverDead, bool driverAboard, unsigned seatOccupant, unsigned driver) {
    if (seatOccupant && seatOccupant != driver) return nullptr;
    if (!driverRef) return "driver_gone";
    if (driverDead) return "driver_dead";
    if (!driverAboard) return "driver_out";
    return nullptr;
}
struct Judgement { Verdict verdict = Verdict::Proving; Life life = Life::Proving; const char* why = ""; };
inline Judgement Judge(const Facts& f) {
    if (f.taken) return { Verdict::World, Life::World, "taken_by_the_player" };
    if (f.empty && f.emptyConfirmed) return { Verdict::Broken, Life::Broken, "empty_vehicle" };  // EV-1: both halves retired out of sight
    if (f.dead) return { Verdict::World, Life::World, "driver_dead" };
    if (f.fighting) return { Verdict::World, Life::World, "in_a_fight" };
    if (f.outOnGround) return { Verdict::World, Life::World, "driver_out_on_the_ground" };
    if (f.unrecoverable) return { Verdict::Broken, Life::Broken, "core_record_broken" };
    if (f.gaveUp) return { Verdict::Broken, Life::Broken, "gave_up" };
    if (f.fault) return { Verdict::Broken, Life::Broken, f.fault };
    if (f.spotFailed) return { Verdict::Broken, Life::Broken, "failed_spot_test" };
    if (f.misplaced) return { Verdict::Broken, Life::Broken, "misplaced_at_arrival" };
    // CC-2/CC-2a: a pair still being proved whose seat could be tried and did not take, when the cover closes, would fail in sight.
    // One whose models never loaded is not judged here (behind the loading screen nothing can be tried: 2026-11-07 22:26, 57 of 57).
    if (f.proving && f.coverClosing && f.seatTriable && !f.seated) return { Verdict::Broken, Life::Broken, "unproven_at_cover_end" };
    // SR-2: a seated pair is never a seat failure; it has its own limit for the release (the route side's trip).
    if (f.proving && f.seated) return f.seatedWaitMs >= rules::seatedNoTripMs ? Judgement{ Verdict::Broken, Life::Broken, "seated_no_trip" } : Judgement{ Verdict::Proving, Life::Proving, "seated_waiting" };
    if (f.proving) return f.proveExpired ?Judgement{ Verdict::Broken, Life::Broken, "never_seated_in_time" } : Judgement{ Verdict::Proving, Life::Proving, f.seated ? "seated_waiting" : "waiting_for_seat" };
    if (f.bad) return f.badWaitOver ? Judgement{ Verdict::Broken, Life::Broken, f.badWhy ? f.badWhy : "did_not_recover" } : Judgement{ Verdict::Recovering, Life::Recovering, f.badWhy ? f.badWhy : "recovering" };
    return { Verdict::Healthy, Life::Driving, "healthy" };
}

// What a pair still waits for, in the order the flow needs it (Ledger: a wait always names its reason).
inline const char* Waiting(bool vehicleModel, bool driverModel, bool body, bool aboard, bool sitting) {
    return !vehicleModel ? "no_vehicle_model" : !driverModel ? "no_driver_model" : !body ? "no_physics_body" : !aboard ? "not_aboard" : !sitting ? "not_sitting" : "waiting_release";
}

// ---------- 9 Retirement: the only remover ----------
// Never a pair the world owns; only under cover or out of sight.
struct Removal { bool now = false; const char* why = ""; };
inline bool QueueFallRetirement(Verdict verdict, Life life, bool uncontrolled, bool queued, bool playerTook) {
    return verdict == Verdict::Broken && life != Life::World && uncontrolled && !queued && !playerTook;
}
// A confirmed fall may separate a pair from the player vertically. Sight is evaluated separately.
inline double RetirementDistance(double horizontal, double vertical, bool fell) {
    return fell ? std::hypot(horizontal, vertical) : horizontal;
}
inline bool RetirementConcealed(bool beyondNotice, int exposure) { return beyondNotice && exposure == 2; }
inline bool DeepFallCandidate(bool fell, double depth, double notice) {
    return fell && std::isfinite(depth) && std::isfinite(notice) && notice > 0 && depth >= notice;
}
inline bool BelowBakedTerrain(bool known, double terrainZ, double modelZ) {
    return known && std::isfinite(terrainZ) && std::isfinite(modelZ) &&
           terrainZ - modelZ >= rules::undergroundClearance;
}
inline Removal MayRemove(bool worldOwned, bool cover, bool unseen) {
    if (worldOwned) return { false, "world_owns_it" };
    if (cover) return { true, "under_cover" };
    if (unseen) return { true, "out_of_sight" };
    return { false, "waiting_unseen" };
}
enum class PopulationAction { FleetBirth, DriverRepair, Retirement, FleetPath };
struct PopulationRequest {
    PopulationAction action;
    Window window{};
    bool worldOwned=false, unseen=false, worldComplete=true, sited=true, profileHasRoom=true;
};
inline Removal ArbitratePopulation(const PopulationRequest& request, BirthBudget* budget=nullptr) {
    if (request.action == PopulationAction::Retirement)
        return MayRemove(request.worldOwned, request.window.cover, request.unseen);
    if (request.action == PopulationAction::FleetPath)
        return request.worldOwned ? Removal{false, "world_owns_it"} :
               !request.sited ? Removal{false, "no_committed_path"} : Removal{true, "fleet_path_granted"};
    if (!request.window.open) return {false, request.window.why};
    if (request.action == PopulationAction::FleetBirth) {
        if (!request.profileHasRoom) return {false, "traffic_profile_full"};
        if (!request.worldComplete) return {false, "world_vehicle_list_incomplete"};
        if (!request.sited) return {false, "field_or_spacing_refused"};
    }
    if (!budget || !budget->Admit(request.window, request.action == PopulationAction::DriverRepair))
        return {false, "birth_budget_or_window"};
    return {true, "birth_granted"};
}
// Card CP-1: is one half of a pair unseen? farOutOfSight is the standing rule (far and out of the view or line of sight). With camera
// prediction on, a half is also unseen when it is out of view now and the camera is not about to turn onto it (cameraAhead code 0);
// prediction only widens the rule, never narrows it. Codes 1-3 (in view, comes into view, unknown) leave the standing rule alone.
struct Unseen { bool yes = false; const char* rule = ""; };
inline Unseen DeepFallUnseen(int cameraAhead) {
    if (cameraAhead == 0) return { true, "deep_fall_out_of_view" };
    return { false, cameraAhead == 1 ? "deep_fall_in_view" : cameraAhead == 2 ? "deep_fall_comes_into_view" : "deep_fall_camera_unknown" };
}
inline Unseen HalfUnseen(bool hasModel, bool farOutOfSight, bool predictionOn, int cameraAhead) {
    if (!hasModel) return { true, "no_model" };
    if (farOutOfSight) return { true, "far_and_out_of_sight" };
    if (predictionOn && cameraAhead == 0) return { true, "camera_prediction" };
    return { false, predictionOn ? (cameraAhead == 1 ? "in_view" : cameraAhead == 2 ? "comes_into_view" : cameraAhead == 3 ? "turn_unknown" : "near") : "near_or_in_view" };
}

// ---------- 10 Ledger: one line per window ----------
struct Tally {
    int made = 0, kept = 0, empty = 0, retired = 0; std::map<std::string, int> refused;
    void Refuse(const char* why) { ++refused[why ? why : "unknown"]; }
    void Clear() { *this = Tally{}; }
    // "made=2 kept=10 empty=3 retired=1 refused=no_field_box:2,another_vehicle_there:1"
    int Line(char* out, std::size_t n) const {
        int w = std::snprintf(out, n, "made=%d kept=%d empty=%d retired=%d refused=", made, kept, empty, retired);
        bool first = true;
        for (const auto& [k, v] : refused) { if (w < 0 || std::size_t(w) >= n) break; w += std::snprintf(out + w, n - std::size_t(w), "%s%s:%d", first ? "" : ",", k.c_str(), v); first = false; }
        if (first && w >= 0 && std::size_t(w) < n) w += std::snprintf(out + w, n - std::size_t(w), "none");
        return w;
    }
};
}
