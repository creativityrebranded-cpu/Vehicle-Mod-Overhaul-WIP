// Spawn flow v3 (src/traffic/spawnflow.h): each step alone, then the flow cases from notes/SPAWN_FLOW_20261106.html.
// Offline logic only: never proof of behaviour in game.
#include "traffic/spawnflow.h"
#include "traffic/retiringrecord.h"
#include "handoff.h"
#include <thread>
#include "traffic/roadclearance.h"
#include "tilebox.h"
#include "spawnrule.h"
#include <cstdio>
#include <cstring>
static int failed = 0, checks = 0;
static void Check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static bool Near(double a, double b, double e = .05) { return std::fabs(a - b) <= e; }
using namespace spawnflow;

static FvoFieldBoxV1 FlatBox(double cz, double w = 10, double h = 5, std::uint32_t faces = 1) {
    FvoFieldBoxV1 f; f.version = 2; f.cx = 0; f.cy = 20; f.cz = cz + h / 2; f.halfWidth = w / 2; f.halfLength = 20; f.halfHeight = h / 2; f.faces = faces; return f;
}

int main() {
    using Preparation=spawnrule::Preparation;
    constexpr auto limit=spawnrule::kResetLimitMs;
    Check(spawnrule::DecidePreparation(true,true,false,50000)==Preparation::Filling,
        "the editor's old cutoff cannot close an active preparation");
    Check(spawnrule::DecidePreparation(true,false,true,limit-1)==Preparation::Settling,
        "nearby handoffs can hold preparation within the same deadline");
    Check(spawnrule::DecidePreparation(true,true,true,limit)==Preparation::TimedOut,
        "admission and settling both use one hard deadline");
    Check(spawnrule::DecidePreparation(true,false,false,100)==Preparation::Complete &&
        spawnrule::DecidePreparation(false,true,true,100)==Preparation::CoverLost,
        "preparation publishes distinct completion and cover-loss results");
    Check(!spawnrule::IsPreparation(0)&&spawnrule::IsPreparation(int(Preparation::Filling)),
        "the editor recognizes new status values and retains the legacy path");
    Check(retiringrecord::RecoveryBaseMatches(0x6001111,0x6001111,0x6002222)&&
        !retiringrecord::RecoveryBaseMatches(0x6001111,0x6002222,0x6002222)&&
        !retiringrecord::RecoveryBaseMatches(0x6001111,0,0x6002222),
        "late Census evidence requires an observed exact base for an identified saved vehicle");
    Check(retiringrecord::RecoveryBaseMatches(0,0x6002222,0x6002222)&&
        retiringrecord::RecoveryBaseMatches(0,0xFE001111,0x6002222)&&
        !retiringrecord::RecoveryBaseMatches(0,0x6001111,0x6002222),
        "late Census keeps the old base-less compatibility rules");
    spawnflow::PopulationRequest parked{spawnflow::PopulationAction::Retirement};
    parked.window.cover=true;parked.worldOwned=true;
    Check(!spawnflow::ArbitratePopulation(parked).now,"covered parked cleanup cannot hide a reserved or protected chassis");
    parked.worldOwned=false;parked.window.cover=false;parked.unseen=false;
    Check(!spawnflow::ArbitratePopulation(parked).now,"parked cleanup without concealment waits on the same arbiter");
    parked.unseen=true;
    Check(spawnflow::ArbitratePopulation(parked).now,"an unprotected concealed parked duplicate can receive a retirement grant");

    using Phase=handoff::Phase;
    handoff::Progress handoff;
    Check(handoff.Observe(false,true,true,true,true,true,9)==Phase::Seat,"handoff requires the actual seat even when later evidence exists");
    Check(handoff.Observe(true,false,true,true,true,true,9)==Phase::Package,"handoff waits for package confirmation");
    Check(handoff.Observe(true,true,false,true,true,true,9)==Phase::Binding,"handoff waits for controller binding");
    Check(handoff.Observe(true,true,true,true,false,false,9)==Phase::Binding,"handoff waits for a valid baseline observation");
    Check(handoff.Observe(true,true,true,true,true,false,9)==Phase::Release,"a staged chassis can release before its first dynamic step");
    handoff.beforeRelease=9;
    Check(handoff.Observe(true,true,true,false,true,true,9)==Phase::Physics,"a step from before release does not confirm driving");
    Check(handoff.Observe(true,true,true,false,true,false,10)==Phase::Physics,"a stale step does not confirm driving");
    Check(handoff.Observe(true,true,true,false,true,true,10)==Phase::Driving,"a fresh later step confirms driving");
    Check(handoff.Observe(false,true,true,false,true,true,11)==Phase::Seat,"loss of seat evidence sends a pending handoff back to seat proof");

    struct Pedals { float steer=0,accel=0,brake=0;bool boost=false,vert=false; };
    handoff::WheelMailbox<Pedals> mailbox;
    std::atomic<bool> done=false, coherent=true;
    mailbox.Publish(12,{0,0,0,true,false},1);
    std::thread writer([&] {
        for(unsigned i=1;i<20000;++i)mailbox.Publish(12,{float(i),float(i),float(i),true,false},i+1);
        done=true;
    });
    do {
        Pedals read;std::uint64_t authority=0;
        if(mailbox.Read(12,read,authority)&&
           (read.steer!=read.accel||read.accel!=read.brake||!read.boost||read.vert||read.brake!=float(authority-1)))
            coherent=false;
    } while(!done);
    writer.join();
    Check(coherent,"wheel reader never sees a mixed command or authority");
    mailbox.Clear();
    Pedals read;std::uint64_t authority=0;
    Check(!mailbox.Read(12,read,authority),"a cleared pilot cannot return an old wheel frame");
    const auto lease = handoff::wheelGeneration.load();
    mailbox.PublishAdmitted(12,{0,.5f,0,false,false},{true,lease});
    Check(mailbox.Read(12,read,authority) && handoff::WheelGrantCurrent(authority) && read.accel == .5f,
          "an admitted handoff uses the shared wheel publisher");
    ++handoff::wheelGeneration;
    Check(!handoff::WheelGrantCurrent(lease), "a handoff revocation invalidates an in-flight wheel grant");
    mailbox.PublishAdmitted(12,{0,.5f,0,false,false},{true,lease});
    Check(!mailbox.Read(12,read,authority), "a revoked grant cannot republish a wheel command");
    mailbox.PublishAdmitted(12,{0,.5f,0,false,false},{true,0});
    Check(mailbox.Read(12,read,authority) && handoff::WheelGrantCurrent(authority) && read.accel == .5f,
          "other owners use the same publisher without a revocable grant");
    mailbox.PublishAdmitted(12,{0,.5f,0,false,false},{false,0});
    Check(!mailbox.Read(12,read,authority), "a denied handoff clears the existing wheel command");

    Check(spawnrule::Presentation(false,true,true,false)==2 &&
        spawnrule::Open(spawnrule::Presentation(false,true,true,false),true),
        "the running photo editor Start/Swap panel opens the same covered birth window");
    Check(spawnrule::Presentation(false,true,false,false)==1 && !spawnrule::Open(1,true) &&
        spawnrule::Presentation(false,false,true,false)==0 && spawnrule::Presentation(true,false,true,false)==3,
        "ordinary play stays closed and the native loading cover has precedence");
    Check(ObserveSeatProof(false,true,true,true,false) && ObserveSeatProof(true,false,true,true,false) &&
        !ObserveSeatProof(false,false,true,true,false) && !ObserveSeatProof(false,true,true,false,false) &&
        !ObserveSeatProof(false,true,true,true,true),
        "restored pairs can time out only after a seat request with models and body still live");
    using RoadResult = FvoRoadClearance;
    const FvoRoadSiteV1 roadSite{-1, 0, 5, 0, 8, 6};
    roadclearance::Range roadCells{};
    Check(roadclearance::Cells(roadSite, roadCells) && roadCells.left == -1 && roadCells.right == 0 &&
        roadCells.bottom == -1 && roadCells.top == 0, "road clearance covers the adjacent exterior cells across coordinate zero");
    {
        FvoRoadSiteV1 nearEdge{92,50,0,0,3,2};roadclearance::Range east{},west{};
        const bool eastValid=roadclearance::Cells(nearEdge,east,3);nearEdge.heading=3.14159265358979;
        Check(eastValid&&roadclearance::Cells(nearEdge,west,3)&&east.right==1&&west.right==0,
            "an adjacent cell is required for a departure toward it and avoided when the verified footprint stays inside");
    }
    const auto clear = [] { return RoadResult::Clear; };
    const auto attached = [](int, int) { return RoadResult::Clear; };
    const auto savedSeatless = [&] {
        return VehicleSpacing({roadSite.x, roadSite.y, roadSite.z, roadSite.heading}, 8, 6, {19, 0, 5}, 8, 6)
            ? RoadResult::Occupied : RoadResult::Clear;
    };
    Check(roadclearance::Verify(roadCells, savedSeatless, attached) == RoadResult::Occupied,
        "persistent seatless vehicle blocks the departure gap without a Census row");
    Check(roadclearance::Verify(roadCells, [] { return RoadResult::Unknown; }, attached) == RoadResult::Unknown,
        "an incomplete saved-vehicle scan cannot prove an empty road");
    Check(roadclearance::Verify(roadCells, clear, [](int x, int y) {
        return x == 0 && y == 0 ? RoadResult::Unknown : RoadResult::Clear;
    }) == RoadResult::Unknown, "one unobserved exterior cell defers the birth");
    Check(roadclearance::Verify(roadCells, clear, attached) == RoadResult::Clear &&
        roadclearance::Verify(roadCells, savedSeatless, attached) == RoadResult::Occupied,
        "the same site can change from clear at grant to occupied before commit");
    const FvoRoadProofV1 roadProof{roadSite, 1, 1000};
    FvoTrafficBirthV1 birth{FvoBirthSource::Fleet, FvoBirthWindow::Panel, 1, 1, 0, 2, 0, -1, 0, 5, 0};
    Check(roadclearance::Matches(roadProof, birth) &&
        roadclearance::Fresh(roadProof.at, 1250, roadclearance::ProofMs(false)) &&
        !roadclearance::Fresh(roadProof.at, 1251, roadclearance::ProofMs(false)) &&
        !roadclearance::Fresh(roadProof.at, 999, roadclearance::ProofMs(false)), "road receipts are time-bound and reject clock reversal");
    birth.heading = 1;
    Check(!roadclearance::Matches(roadProof, birth), "a road receipt does not transfer to a different birth pose");

    ObservedWait readiness;
    readiness.Observe(true,true,1000);
    readiness.Observe(true,true,2000);
    readiness.Observe(true,false,3000);
    readiness.Observe(true,true,45000);
    Check(readiness.elapsed==1000,"readiness waits count live samples and exclude time spent paused");
    readiness.Observe(false,true,46000);
    Check(readiness.elapsed==0,"readiness resets when its prerequisite returns or the pair leaves its observation range");
    Check(!SeatAttemptSpent(0,true)&&!SeatAttemptSpent(1,false)&&!SeatAttemptSpent(-3,false)&&
          SeatAttemptSpent(0,false)&&SeatAttemptSpent(2,false),"seat retry budget excludes queued, linked-only and out-of-reach replies");
    std::vector<Candidate> distributed{{1,1,4},{4,90,1},{2,12,2},{0,1,0},{6,200,3}};
    Order(distributed);
    Check(distributed[0].slot==0&&distributed[1].slot==4&&distributed[2].slot==2&&distributed[3].slot==6,
          "kilometre arbiter: broad coverage precedes a nearby infill");
    Check(RetirementDistance(10, -500, true) > 350 && RetirementDistance(10, -500, false) == 10,
          "FI-5: vertical distance applies only to a confirmed fall");
    Check(RetirementDistance(10, -25, true) < 350 && !MayRemove(false, false, false).now,
          "FI-5: near or visible traffic still waits");
    Check(DeepFallCandidate(true, 500, 350) && !DeepFallCandidate(false, 500, 350) &&
          !DeepFallCandidate(true, 20, 350) && !DeepFallCandidate(true, -500, 350) &&
          !DeepFallCandidate(true, 500, 0), "FF-3: only a confirmed fall currently deep below the player is eligible");
    Check(DeepFallUnseen(0).yes && !DeepFallUnseen(1).yes && !DeepFallUnseen(2).yes &&
          !DeepFallUnseen(3).yes, "FF-3: depth alone never allows retirement");
    Check(!MayRemove(false, false, DeepFallUnseen(0).yes && DeepFallUnseen(2).yes).now &&
          MayRemove(false, true, false).now && !MayRemove(true, true, true).now,
          "FF-3: both loaded halves must be unseen unless covered and the pair is traffic-owned");
    Check(BelowBakedTerrain(true, 12, -188) && !BelowBakedTerrain(true, 12, -187) &&
          !BelowBakedTerrain(false, 12, -1000) && !BelowBakedTerrain(true, NAN, -1000),
          "RU-1: terrain must be measured and deep below it is distinct from shallow or unknown");
    const bool undergroundCar=BelowBakedTerrain(true, 12, -400);
    Check(MayRemove(false, false, undergroundCar && BelowBakedTerrain(true, 8, -380)).now &&
          !MayRemove(false, false, undergroundCar && BelowBakedTerrain(true, 8, 7)).now &&
          !MayRemove(true, false, undergroundCar).now,
          "RU-1: both underground models may retire without camera evidence, a surfaced driver or World ownership still blocks");
    Check(UncontrolledDescent(0.2, 75, 1000) && !UncontrolledDescent(12, 33, 1000) &&
          !UncontrolledDescent(0, 12, 1000) && !UncontrolledDescent(0, 300, 3000) &&
          !UncontrolledDescent(0, 100, 100), "FF-5: observed free fall is distinct from road, normal descent and stale or brief samples");
    Check(!FellThroughWorld(true,false,UncontrolledDescent(0,0,1000)),
          "stacked deck replay: a stationary pair below a map floor is not a fall");
    Check(FellThroughWorld(true,false,UncontrolledDescent(0,40,1000)),
          "a fresh fast descent below the map floor confirms a fall");
    Check(FellThroughWorld(false,true,false),
          "keep the deep displacement proof if map evidence is missing");
    Check(QueueFallRetirement(Verdict::Broken, Life::Driving, true, false, false) &&
          !QueueFallRetirement(Verdict::Broken, Life::Driving, true, true, false),
          "RF-1: a confirmed fall queues job cleanup once");
    Check(!QueueFallRetirement(Verdict::Recovering, Life::Driving, true, false, false) &&
          !QueueFallRetirement(Verdict::Broken, Life::Driving, false, false, false) &&
          !QueueFallRetirement(Verdict::Broken, Life::World, true, false, false) &&
          !QueueFallRetirement(Verdict::Broken, Life::Driving, true, false, true),
          "RF-1: uncertain, recoverable and player or World-owned pairs cannot queue fall cleanup");
    Check(QueueFallRetirement(Verdict::Broken, Life::Driving, true, false, false) &&
          !MayRemove(false, false, false).now && MayRemove(false, false, true).now,
          "RF-1: a fall can release the job while removal still waits for visibility");
    // 1 Window
    Check(WindowNow(3, false, false).open, "window: loading screen is open");
    Check(WindowNow(2, true, false).open && WindowNow(2, true, false).cover, "window: X panel is open and covered");
    Check(!WindowNow(2, false, false).open, "window: the plain local map is closed");
    Check(!WindowNow(1, true, false).open && !std::strcmp(WindowNow(1, true, false).why, "in_play"), "window: never in play");
    Check(!WindowNow(3, false, true).open && !std::strcmp(WindowNow(3, false, true).why, "clock_pending"), "window: waits for the route clock");
    Check(WindowNow(1, false, false, true).open && !WindowNow(1, false, false, true).cover && !std::strcmp(WindowNow(1, false, false, true).why, "live_road"), "window (LV-3): the live rule opens it in play, uncovered");
    Check(!WindowNow(1, false, true, true).open && !WindowNow(2, false, false, true).open, "window (LV-3): the live rule still waits for the clock; the plain map stays closed");
    {   const auto road = WindowNow(1, false, false, true), plain = WindowNow(1, false, false);
        Check(road.live && road.open && !road.cover && !plain.live && !plain.open, "window (LV-3): only the live rule makes the play window live");
        Check(!WindowNow(3, false, false).live && !WindowNow(2, true, false).live, "window (LV-3): covered windows are not live");
        const Window built = { true, false, "live_road" };
        Check(built.open && !built.cover && !built.live, "window (LV-3): the generator's aggregate Windows keep working");
    }
    Check(MayCreate(WindowNow(1, false, false, true), 0, rules::maxWaitingPlay - 1) && !MayCreate(WindowNow(1, false, false, true), 0, rules::maxWaitingPlay), "arbiter (LV-3): smaller waiting cap in play");
    Check(!spawnrule::LiveOpen(1, false, spawnrule::kLiveSettleMs - 1) && spawnrule::LiveOpen(1, false, spawnrule::kLiveSettleMs) && !spawnrule::LiveOpen(1, true, 99999) &&
          !spawnrule::LiveOpen(2, false, 99999) && !spawnrule::LiveOpen(3, false, 99999) && !spawnrule::LiveOpen(0, false, 99999), "live rule: play only, after the settle time, never while the clock waits");
    {   const auto w = WindowNow(1, false, false, true);   // DW-2: the global live pause is gone; the arbiter's per-tick limit is a parameter, the in-flight cap is the rule
        Check(MayCreate(w, 0, 0) && !MayCreate(w, 1, 0) && MayCreate(w, 3, 0, 4) && !MayCreate(w, 4, 0, 4) && !MayCreate(w, 0, rules::maxWaitingPlay, 4), "arbiter: in play one create per tick by default, more only when the caller raises the limit; the in-flight cap stays rules::maxWaitingPlay");
        BirthBudget b; b.limit = 3; Check(b.Admit(w, false) && b.Admit(w, false) && b.Admit(w, false) && !b.Admit(w, false), "arbiter: the birth budget follows its per-tick limit"); }
    const auto loading = WindowNow(3, false, false);
    const auto panel = WindowNow(2, true, false);
    BirthBudget repairFirst, fleetFirst, play;
    Check(repairFirst.Admit(loading, true) && !repairFirst.Admit(loading, false), "birth arbiter: repair and fleet share the loading tick's grant");
    Check(fleetFirst.Admit(panel, false) && !fleetFirst.Admit(panel, true), "birth arbiter: fleet and repair share the X tick's grant");
    Check(!play.Admit(WindowNow(2, false, false), false) && !play.Admit(WindowNow(1, false, false), true) && play.granted == 0,
          "birth arbiter: closed windows never spend the grant");
    const auto diagnostic = WindowNow(1, false, false, true);  // the live window
    Check(!play.Admit(diagnostic, true) && play.Admit(diagnostic, false), "birth arbiter: the live window is for fleet requests only");
    PopulationRequest fleetRequest{PopulationAction::FleetBirth,panel};
    BirthBudget population;
    fleetRequest.profileHasRoom=false;
    Check(!ArbitratePopulation(fleetRequest,&population).now&&population.granted==0,"population: full traffic profile blocks a chassis");
    fleetRequest.profileHasRoom=true;fleetRequest.worldComplete=false;
    Check(!ArbitratePopulation(fleetRequest,&population).now&&population.granted==0,"population: an incomplete vehicle list blocks siting");
    fleetRequest.worldComplete=true;fleetRequest.sited=false;
    Check(!ArbitratePopulation(fleetRequest,&population).now&&population.granted==0,"population: the field and spacing must accept the pose");
    fleetRequest.sited=true;
    Check(ArbitratePopulation(fleetRequest,&population).now,"population: an allowed birth spends the shared grant");
    PopulationRequest existingVehicle{PopulationAction::FleetPath};
    Check(ArbitratePopulation(existingVehicle, &population).now, "population: visible existing car receives its committed path outside birth window");
    existingVehicle.unseen = true;
    Check(ArbitratePopulation(existingVehicle, &population).now, "population: concealment is irrelevant to existing-car path assignment");
    existingVehicle.worldOwned = true;
    Check(!ArbitratePopulation(existingVehicle, &population).now, "population: a car owned by the player is outside traffic scope");
    existingVehicle.worldOwned = false;
    existingVehicle.sited = false;
    Check(!ArbitratePopulation(existingVehicle, &population).now, "population: the executor needs a committed path");

    PopulationRequest retirement{PopulationAction::Retirement};
    Check(!ArbitratePopulation(retirement).now,"population: a visible retirement waits");
    retirement.unseen=true;
    Check(ArbitratePopulation(retirement,&population).now,"population: retirement uses concealment and does not spend a birth grant");
    retirement.worldOwned=true;
    Check(!ArbitratePopulation(retirement).now,"population: a pair outside traffic scope cannot be retired");

    // 2 Census
    Check(Allowed(Life::None, Life::Born) && Allowed(Life::Born, Life::Proving) && Allowed(Life::Proving, Life::Driving), "census: the birth path");
    Check(!Allowed(Life::World, Life::Retiring), "census: never retires what the world owns");
    Check(!Allowed(Life::Driving, Life::Gone) && Allowed(Life::Retiring, Life::Gone), "census: gone only through retirement");
    Check(Allowed(Life::None, Life::Driving), "census: a saved pair restored as driving");
    Check(Control(Life::Driving) == 1 && Control(Life::World) == 2 && Control(Life::Broken) == 3 && Control(Life::Proving) == 0, "census: control for core readers");
    Check(ToOwner(Life::Recovering) == planowner::Life::Recovering && ToOwner(Life::World) == planowner::Life::None, "census: owner record metadata");
    {   planowner::Table t; t.Claim(0xFF000001, planowner::Owner::Traffic, trafficrank::Rank::Parked, 3, 0, "spawn");
        Check(t.Traffic(0xFF000001, 0xFF000002, 4, 0x1234, ToOwner(Life::Proving), "born"), "census: traffic metadata on the owner record");
        t.Claim(0xFF000001, planowner::Owner::Player, trafficrank::Rank::Parked, 0, .5, "player");
        Check(!t.Traffic(0xFF000001, 0, 4, 0, planowner::Life::Driving, "x"), "census: no traffic metadata once a stronger owner holds it"); }

    // 7 Health records, read by 3 Demand
    // Seat reports at different spots (card HR1: a model is blamed only when its fails come from several spots).
    auto seat = [](Records& r, unsigned model, bool sat, int spot) { return r.Report(spawnhealth::Stage::Seat, model, "s" + std::to_string(spot), sat); };
    Records h;
    Check(!seat(h, 0xA, false, 1).modelRejected && seat(h, 0xA, false, 2).modelRejected && h.ModelRejected(0xA), "health: model rejected after 2 failed seats at 2 spots, never sat");
    seat(h, 0xB, true, 1); seat(h, 0xB, false, 2); seat(h, 0xB, false, 3); seat(h, 0xB, false, 4);
    Check(!h.ModelRejected(0xB), "health: a model that sat 1 of 4 is kept");
    {   Records r; seat(r, 0xD, true, 1); for (int k = 0; k < 5; ++k) seat(r, 0xD, false, 2 + k);   // 1 of 6
        Check(!r.ModelRejected(0xD) && seat(r, 0xD, false, 9).modelRejected && r.ModelRejected(0xD), "health (HR1): a model that sat 1 of 7 is rejected (mostly fails)");
        Records q; seat(q, 0xE, true, 1); seat(q, 0xE, true, 2); for (int k = 0; k < 4; ++k) seat(q, 0xE, false, 3 + k);
        Check(!q.ModelRejected(0xE), "health (HR1): a model that sat 2 of 6 is kept"); }
    const std::vector<unsigned> order{ 0xA, 0xB, 0xC };
    const char* why = "";
    Check(Model(order, 0, [](unsigned) { return false; }, h, &why) == 0xC, "demand (DR1): rejected A left out, B (1 of 4) under preferOdds, unknown C taken");
    Check(Model({ 0xA, 0xB }, 0, [](unsigned) { return false; }, h, &why) == 0xB && !std::strcmp(why, "best_odds_left"), "demand (DR1): low odds still used when nothing better is allowed");
    Check(Model(order, 0, [](unsigned id) { return id == 0xB; }, h, &why) == 0xC, "demand: your types first, then health");
    Check(Model(order, 0, [](unsigned id) { return id != 0xA; }, h, &why) == 0 && !std::strcmp(why, "health_rejected_every_allowed_type"), "demand: none left names health");
    Check(Model({}, 0, [](unsigned) { return false; }, h, &why) == 0 && !std::strcmp(why, "no_vehicle_types"), "demand: no types");
    h.Clear(); Check(!h.ModelRejected(0xA), "health: verdicts are reversible (reset)");

    // 4 Arbiter
    std::vector<Candidate> c{ { 0, 300 }, { 1, 40 }, { 2, 120 }, { 3, 40 } }; Order(c);
    Check(c[0].slot == 1 && c[1].slot == 3 && c[2].slot == 2 && c[3].slot == 0, "arbiter: nearest first, stable");
    Check(MayCreate(WindowNow(3, false, false), 0, 0) && !MayCreate(WindowNow(3, false, false), 1, 0), "arbiter: one create per tick");
    Check(!MayCreate(WindowNow(3, false, false), 0, rules::maxWaitingCovered), "arbiter: waiting cap");
    Check(!MayCreate(WindowNow(1, false, false), 0, 0), "arbiter: closed window creates nothing");
    Check(WindowNow(2,true,false,false,true).open,"window: X can fill gaps while its panel covers the world");
    Check(!WindowNow(2,true,false,false,false).open,"window: the ordinary map cannot keep an expired X admission open");
    Check(WindowNow(3,false,false,false,false).open,"window: first arrival has independent loading-screen cover");

    // 5 Siting
    {
        const Pose site{};
        const auto overlap=VehicleSpacing(site,3,2,{5,0,0},3,2);
        const auto ahead=VehicleSpacing(site,3,2,{8,0,0},3,2);
        Check(overlap&&!std::strcmp(overlap,"another_vehicle_there"),"siting: preserve hull overlap");
        Check(ahead&&!std::strcmp(ahead,"departure_gap_occupied"),"siting: leave room to depart");
        Check(!VehicleSpacing(site,3,2,{8,4,0},3,2),"siting: departure gap does not block the adjacent lane");
        Check(!VehicleSpacing(site,3,2,{8,0,5},3,2),"siting: departure gap does not block another elevation");
        Check(!VehicleSpacing(site,3,2,{-8,0,0},3,2),"siting: non-overlapping vehicle behind does not block departure");
        const auto turned=VehicleSpacing({0,0,0,1.5707963267948966},3,2,{0,8,0},3,2);
        Check(turned&&!std::strcmp(turned,"departure_gap_occupied"),"siting: departure gap follows the lane heading");
    }
    Check(!RetirementConcealed(true,0)&&!RetirementConcealed(true,1),"retirement: far unknown and far visible both wait");
    Check(RetirementConcealed(true,2)&&!RetirementConcealed(false,2),"retirement: confirmed blockage and range are separate");
    Shape car; car.halfWidth = 1.2; car.height = 2; car.originAbove = .9;
    Pose lane{ 2, 20, 99, 0 };
    auto s = Place(true, FlatBox(10), lane, car, false, false);
    Check(s.ok && Near(s.floor, 10) && Near(s.pose.z, 10 + .9 + rules::dropMargin), "siting: floor + rest height + drop margin");
    Check(Near(s.pose.x, 2) && Near(s.pose.y, 20), "siting: on the lane point");
    Check(!Place(false, {}, lane, car, false, false).ok && !std::strcmp(Place(false, {}, lane, car, false, false).why, "no_field_box"), "siting: no box, no birth");
    Check(!std::strcmp(Place(true, FlatBox(10, 10, 5, 0), lane, car, false, false).why, "no_floor_hover_tile"), "siting: hover tile has no floor");
    Check(!std::strcmp(Place(true, FlatBox(10, 6), { 2, 20, 0, 0 }, car, false, false).why, "model_wider_than_field"), "siting: body must fit inside the box");
    Check(!std::strcmp(Place(true, FlatBox(10, 10, 2), lane, car, false, false).why, "model_taller_than_field"), "siting: body must fit under the roof");
    Check(!std::strcmp(Place(true, FlatBox(10), lane, car, true, false).why, "another_vehicle_there"), "siting: occupied");
    Check(!std::strcmp(Place(true, FlatBox(10), lane, car, false, true).why, "health_rejected_spot"), "siting: health's spot verdict");
    Check(!std::strcmp(Place(true, FlatBox(10), { 0, 80, 0, 0 }, car, false, false).why, "lane_point_outside_its_tile"), "siting: lane point in its tile");
    {   auto f = FlatBox(10); f.pitch = std::atan2(4.0, 40.0);   // rising 4 m over the tile
        auto up = Place(true, f, { 0, 30, 0, 0 }, car, false, false);
        Check(up.ok && up.floor > 10.9 && up.floor < 11.1, "siting: a pitched floor gives its height at the lane point"); }
    {   // Default box (tilebox) for a route with no road_boxes: the same Siting answer, flagged default.
        trafficdata::Route r; r.ring = false; r.lanes = 2; r.corridor = 5; r.travel = routepolicy::Travel::Drive; r.points = { { 0, 0, 10 }, { 0, 40, 10 } };
        const auto line = trafficroute::Build(r); FvoFieldBoxV1 f;
        Check(tilebox::TileBox(r, line, 0, f) && fieldboxapi::IsDefault(f), "siting: an old route gets the default box");
        const auto d = Place(true, f, { 0, 20, 0, 0 }, car, false, false);
        Check(d.ok && d.defaultBox && Near(d.floor, 10, .3) && !std::strcmp(d.why, "default_box_floor"), "siting: default box floor at the route height");
        r.travel = routepolicy::Travel::Hover; FvoFieldBoxV1 g; tilebox::TileBox(r, line, 0, g);
        Check(!Place(true, g, { 0, 20, 0, 0 }, car, false, false).ok, "siting: hover-only routes stay out of spawning");
        r.travel = routepolicy::Travel::Drive; r.road.primitives.resize(1); r.road.primitives[0].kind = roadplan::Tube;
        Check(!tilebox::TileBox(r, line, 0, g), "siting: D11 shapes get no box, so no birth"); }

    // 7 Health verdicts
    Facts f;
    f.proving = true; Check(Judge(f).verdict == Verdict::Proving, "health: proving");
    f.proveExpired = true; Check(Judge(f).verdict == Verdict::Broken && !std::strcmp(Judge(f).why, "never_seated_in_time"), "health: never seated is broken");
    f = {}; Check(Judge(f).verdict == Verdict::Healthy && Judge(f).life == Life::Driving, "health: healthy drives");
    f.bad = true; Check(Judge(f).verdict == Verdict::Recovering, "health: bad waits as recovering");
    f.badWaitOver = true; Check(Judge(f).verdict == Verdict::Broken, "health: did not recover");
    { Facts g; g.spotFailed = true; Check(Judge(g).verdict == Verdict::Broken && !std::strcmp(Judge(g).why, "failed_spot_test"), "health: failed spot test is broken (ST-C)");
      g.taken = true; Check(Judge(g).verdict == Verdict::World, "health: the world owns it first, even with a failed spot test"); }
    { Facts g; g.misplaced = true; Check(Judge(g).verdict == Verdict::Broken && !std::strcmp(Judge(g).why, "misplaced_at_arrival"), "health: misplaced at arrival is broken (ST-A3)"); }
    f.taken = true; Check(Judge(f).verdict == Verdict::World, "health: the player's car is the world's, before anything else");
    f = {}; f.outOnGround = true; f.gaveUp = true; Check(Judge(f).verdict == Verdict::World, "health: a driver out on the ground is the world's");
    f = {}; f.fault = "no_physics_body"; Check(Judge(f).verdict == Verdict::Broken && !std::strcmp(Judge(f).why, "no_physics_body"), "health: a step's fault is judged by Health, with its reason");
    f.taken = true; Check(Judge(f).verdict == Verdict::World, "health: the world still comes first");
    Check(!std::strcmp(Waiting(true, false, true, true, true), "no_driver_model"), "ledger: a wait names what is missing");
    Check(!std::strcmp(Waiting(true, true, false, false, false), "no_physics_body") && !std::strcmp(Waiting(true, true, true, true, false), "not_sitting") && !std::strcmp(Waiting(true, true, true, true, true), "waiting_release"), "ledger: in the order the flow needs it");

    // 9 Retirement
    Check(!MayRemove(true, true, true).now, "retirement: never the world's");
    Check(MayRemove(false, true, false).now && MayRemove(false, false, true).now, "retirement: under cover or out of sight");
    Check(!MayRemove(false, false, false).now, "retirement: waits while seen");
    // PF-1 self-reports: recovering first, broken after the wait, the fact named.
    {   Facts f; f.bad = true; f.badWhy = "self_report_not_moving";
        Check(Judge(f).verdict == Verdict::Recovering && !std::strcmp(Judge(f).why, "self_report_not_moving"), "PF-1: a self-report: recovering, named");
        f.badWaitOver = true; Check(Judge(f).verdict == Verdict::Broken && !std::strcmp(Judge(f).why, "self_report_not_moving"), "PF-1: still after the wait: broken, named");
        Facts g; g.bad = true; Check(!std::strcmp(Judge(g).why, "recovering"), "PF-1: the core record's bad keeps its old reason"); }
    // CP-1 camera prediction: only widens the standing rule.
    Check(HalfUnseen(false, false, false, 1).yes && HalfUnseen(true, true, false, 1).yes, "CP-1: no model, or far and out of sight: unseen (switch off)");
    Check(!HalfUnseen(true, false, false, 0).yes, "CP-1: switch off: near and out of view is not enough");
    Check(HalfUnseen(true, false, true, 0).yes && !std::strcmp(HalfUnseen(true, false, true, 0).rule, "camera_prediction"), "CP-1: switch on: out now and ahead removes");
    Check(!HalfUnseen(true, false, true, 1).yes && !HalfUnseen(true, false, true, 2).yes && !HalfUnseen(true, false, true, 3).yes, "CP-1: in view, coming into view or unknown: waits");
    Check(HalfUnseen(true, true, true, 1).yes, "CP-1: switch on never narrows the standing rule");

    // 10 Ledger
    Tally t; t.made = 2; t.Refuse("no_field_box"); t.Refuse("no_field_box"); t.Refuse("another_vehicle_there");
    char line[200]; t.Line(line, sizeof line);
    Check(!std::strcmp(line, "made=2 kept=0 empty=0 retired=0 refused=another_vehicle_there:1,no_field_box:2"), "ledger: one line per window");
    Tally e; e.Line(line, sizeof line); Check(std::strstr(line, "refused=none") != nullptr, "ledger: nothing refused");

    // Flow: "Driver never sits" -> broken -> retire list -> removed under cover -> model rejected -> Demand leaves it out.
    {   Records hr; Facts p; p.proving = true; p.proveExpired = true;
        const auto j = Judge(p); Check(j.life == Life::Broken && Allowed(Life::Proving, j.life), "flow: never sits -> broken");
        Check(Allowed(j.life, Life::Retiring) && MayRemove(false, true, false).now && Allowed(Life::Retiring, Life::Gone), "flow: broken -> retiring -> gone under cover");
        seat(hr, 0x77, false, 1); seat(hr, 0x77, false, 2);
        const char* w = ""; Check(Model({ 0x77, 0x78 }, 0, [](unsigned) { return false; }, hr, &w) == 0x78, "flow: the failing model is left out next window"); }
    // Flow: "Player steals a car" -> world, never on the retire list.
    {   Facts p; p.taken = true; const auto j = Judge(p);
        Check(j.life == Life::World && !Allowed(j.life, Life::Retiring) && !MayRemove(true, true, true).now, "flow: a stolen car is never retired"); }
    // Flow: "New car to the road": window -> arbiter -> siting -> born -> proving -> healthy -> released.
    {   const auto w = WindowNow(3, false, false); Check(MayCreate(w, 0, 0), "flow: window open, arbiter may create");
        const auto site = Place(true, FlatBox(10), lane, car, false, false); Check(site.ok, "flow: sited");
        Check(Allowed(Life::None, Life::Born) && Allowed(Life::Born, Life::Proving), "flow: born, proving");
        Facts p; p.proving = false; p.seated = true; Check(Judge(p).life == Life::Driving, "flow: healthy, ready for the handoff"); }
    // EV-1: empty vehicles (user: "empty vehicles need clearing ... cleanup should make sure it works on both").
    {   Check(!EmptyCause(true, false, true, 0x11, 0x11), "empty (EV-1): its own driver aboard is not empty");
        Check(!EmptyCause(true, false, false, 0x22, 0x11) && !EmptyCause(false, false, false, 0x22, 0x11), "empty (EV-1): someone else in the seat (player, companion, job) is not traffic's to judge");
        Check(!std::strcmp(EmptyCause(true, true, false, 0, 0x11), "driver_dead"), "empty (EV-1): dead driver");
        Check(!std::strcmp(EmptyCause(true, false, false, 0, 0x11), "driver_out"), "empty (EV-1): driver out");
        Check(!std::strcmp(EmptyCause(false, false, false, 0, 0x11), "driver_gone"), "empty (EV-1): driver reference gone");
        Facts p; p.dead = true; p.empty = "driver_dead";
        Check(Judge(p).verdict == Verdict::World, "empty (EV-1): before the confirm time a dead driver's pair is still the world's");
        p.emptyConfirmed = true; const auto j = Judge(p);
        Check(j.verdict == Verdict::Broken && !std::strcmp(j.why, "empty_vehicle") && Allowed(Life::World, Life::Broken) && Allowed(j.life, Life::Retiring), "empty (EV-1): confirmed empty -> broken -> retiring (both halves)");
        Check(!Allowed(Life::World, Life::Retiring), "empty (EV-1): the world's pair still goes through Broken, never straight to retiring");
        Facts t; t.taken = true; t.empty = "driver_out"; t.emptyConfirmed = true;
        Check(Judge(t).verdict == Verdict::World, "empty (EV-1): a car the player took is never retired, even empty");
        Facts o; o.outOnGround = true; o.empty = "driver_out"; o.emptyConfirmed = true;
        Check(Judge(o).verdict == Verdict::Broken, "empty (EV-1): driver out on the ground and the car empty long enough -> broken"); }

    // CC-2: the cover is closing.
    {   Facts u; u.proving = true; u.coverClosing = true;
        Check(Judge(u).verdict == Verdict::Proving, "cover closing (CC-2a): models never loaded (behind the loading screen) -> kept, judged in play");
        Facts p; p.proving = true; p.coverClosing = true; p.seatTriable = true;
        const auto j = Judge(p); Check(j.verdict == Verdict::Broken && !std::strcmp(j.why, "unproven_at_cover_end") && Allowed(Life::Proving, j.life), "cover closing (CC-2a): seat triable, not seated -> broken");
        p.seated = true; Check(Judge(p).verdict == Verdict::Proving, "cover closing (CC-2): a seated pair waiting for release is kept");
        Facts q; q.proving = false; q.seated = true; q.coverClosing = true; Check(Judge(q).verdict == Verdict::Healthy, "cover closing (CC-2): a driving pair is not touched");
        Facts r; r.proving = true; Check(Judge(r).verdict == Verdict::Proving, "cover closing (CC-2): without the fact nothing changes"); }

    // SR-2: seated but not released.
    {   Facts p; p.proving = true; p.seated = true; p.proveExpired = true;
        Check(Judge(p).verdict == Verdict::Proving && !std::strcmp(Judge(p).why, "seated_waiting"), "seated (SR-2): a seated pair is never never_seated_in_time");
        p.seatedWaitMs = rules::seatedNoTripMs;
        const auto j = Judge(p); Check(j.verdict == Verdict::Broken && !std::strcmp(j.why, "seated_no_trip") && Allowed(Life::Proving, j.life), "seated (SR-2): seated past the limit with no release -> broken seated_no_trip");
        p.seatedWaitMs = rules::seatedNoTripMs - 1; Check(Judge(p).verdict == Verdict::Proving, "seated (SR-2): within the limit -> still proving");
        Facts n; n.proving = true; n.proveExpired = true; Check(!std::strcmp(Judge(n).why, "never_seated_in_time"), "seated (SR-2): an unseated pair past its time is still never_seated_in_time"); }

    // SR-3 Demand: a new pair only with trip room left on its route.
    {   Check(RouteHasRoom(2, 1) && !RouteHasRoom(2, 2) && !RouteHasRoom(0, 0), "demand (SR-3): room left beyond the pairs still being proved");
        Check(RouteHasRoom(-1, 50), "demand (SR-3): unknown room (no such route here) applies no limit"); }

    // KO-2a Handoff: a taken frozen pair is released to the player.
    {
        BirthBudget budget;
        PopulationRequest path{PopulationAction::FleetPath};
        Check(ArbitratePopulation(path,&budget).now&&budget.granted==0,
              "fleet path: an existing pair receives its path in play without a birth window or creation budget");
        path.worldOwned=true;
        Check(!ArbitratePopulation(path).now,"fleet path: a player-owned pair cannot be reassigned");
        path.worldOwned=false;
        path.sited=false;
        Check(!ArbitratePopulation(path).now,"fleet path: a real pair needs a committed route before assignment");
    }

    // Relocation hold and re-base (Health)
    { Check(!JudgeFall(true, true, true, true), "relocating: no fall verdict, even when it looks deep, under the floor and descending");
      Check(JudgeFall(false, false, true, false), "not relocating: a deep drop from the baseline is a fall as before");
      Check(JudgeFall(false, true, false, true), "not relocating: under the floor and descending is a fall as before");
      Check(!JudgeFall(false, true, false, false), "not relocating: a floor discrepancy without descent is still not a fall");
      Check(DeepBelowBase(true, 10.0, -300.0) && !DeepBelowBase(true, 10.0, 0.0) && !DeepBelowBase(false, 10.0, -300.0), "the fall baseline: deep below it, not near it, and none known means none");
      Check(!DeepBelowBase(true, -88.0, -90.0) && DeepBelowBase(true, -88.0, -400.0), "re-based to a destination far below the birth height, a fall is judged from there"); }
    std::printf("spawnflow_test: %d checks, %d failed\n%s\n", checks, failed, failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
