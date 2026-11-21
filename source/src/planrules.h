#pragma once
// The plan's shared types and numbers (removal 2026-11-14): line samples, the road's lanes, a branch (as World and the F7 views record
// one), and every rule number World, the survey and the energy plan read. No decision lives here. Moved out of the old branch planner
// (planbranch.h, removed 2026-11-14, kept outside FVO in fvo-removed-20261114/deciders); names kept so logs and settings read the same.
#include "carcap.h"
#include "worldclass.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace planbranch {
constexpr int kSamples = 24, kMaxBranches = 12, kMaxLanes = 8, kMaxObstacles = 16;
constexpr double kStep = 2.0;

namespace rules {  // design sections 12 and 14: working defaults until the user decides otherwise
constexpr double ownPerMetre = .05, sameWay = .6, oncoming = 3.0, offRoad = 6.0, doubt = .3;  // D4
constexpr double sideRate = 1.2;                 // D3, m/s
constexpr double stoppedWait = 20.0;             // D1, s before an occupied stopped vehicle may be gone round
// PS-1 (2026-11-12): a vehicle ahead that is not getting anywhere may be gone round. Net movement over progressWindow s under progressNet m
// (rocking back and forth counts as none). A car the core drives is judged by its own plan instead (route hold = a queue; recovery = stuck).
constexpr double progressWindow = 10.0, progressNet = 2.0;
constexpr int budget = 8;                        // D9
constexpr double lostWeight = 12.0, smooth = 8.0, change = .1, length = .05;
constexpr double creepSpeed = 2.0, reverseSpeed = 2.0;  // m/s: doubt and backing off
constexpr double hopSpeed = 6.0;                 // m/s run-up for a hop (was forced in the wheel stage)
constexpr double hopRise = 1.2, hopLead = 2.0;   // a hop's height profile: over a low face (planworld::kLowTop .9) plus .3, lifting from hopLead m before the nose meets it
constexpr double boostShare = 1.75;              // traffic may reach lane speed up to this times its own top (boostlogic::kMultiplier)
// BO-1 (2026-11-19, user: "when the control has an issue moving forward fast enough or with enough thrust it can just use boost"; Speed, boostneed.h):
// the car's possible speed with the game's forward boost. Start: the speed falls short of the plan's speed by boostGap m/s, the tyre pedal is
// already full (boostPedal), the car gains under boostMinAccel m/s^2, for boostOnHold s together, and the native charge is at least boostMeterOn.
// Stop: the shortfall is under boostMetGap, or the charge is under boostMeterOff (then no restart until boostMeterOn again), or any halt. A boost
// runs at least boostMinOn s and is off at least boostMinOff s (no flicker); a halt cuts it at once. Not under boostMinWanted m/s asked, nor
// while sliding past boostSlipDeg or steering past boostSteer, nor with a stop line within boostStopLook m. Samples older than boostSampleAge s
// (pedal, native meter) are unknown: no boost.
constexpr double boostGap = 2.5, boostMetGap = 1.0, boostPedal = .95, boostMinAccel = 1.5, boostOnHold = .8, boostAccelTau = .5;
constexpr double boostMeterOn = .35, boostMeterOff = .05, boostMinOn = .8, boostMinOff = .6, boostMinWanted = 3.0;
constexpr double boostSlipDeg = 12.0, boostSteer = .5, boostStopLook = 60.0, boostSampleAge = 1.0;
// BO-2 (2026-11-19, hover cars; same user requirement, same Speed place): a hovering car has no tyre pedal; the thrust demand is the hover
// throttle before its clamp ((plan speed - speed) / 4, the same form), and what the hover can hold is also capped by the ground-loaded cap and
// the flight gear's top. A hover car is thrust-limited only when it is short of the plan's speed, the throttle is full, it is not gaining, and
// those caps are NOT what holds it: boost adds thrust only (boostlogic::kMultiplier on the thrust, the caps are not raised). Under the plan's
// speed by more than boostCapSlack m/s the cap is the limit, not the thrust: no boost.
constexpr double boostCapSlack = .5;
constexpr double leaveField = 6.0;               // D12: a widened branch (after no progress) may leave through an Avoid face, at this cost
constexpr double stuckPerSecond = 2.0, stuckMax = 200.0, standShare = 1.5;  // D7: once no progress has widened Avoid, standing still costs this much more each second
// Fix A (2026-11-07): a measured floor within this many metres of the level the field or road picked gives the wanted surface's height.
constexpr double levelWindow = 1.2;
// WM-1 (2026-11-18, one world map): a neighbouring floor measurement is the same surface within mapLip + mapGrade x distance (the
// wheels' climb); the car's reachable levels are flooded out to mapReach m every mapReachEvery s; the plan's floor samples search
// reachable levels within wantWindow of the picked height (reachability, not the authored line, picks the level).
// mapNeighbour: a neighbour measurement belongs to the same surface for a plane within mapLip + this x distance; mapBend: how far a reached
// surface may bend from its own slope per metre (a flat road running onto a ramp) before a level is another surface (a rail or crate top).
constexpr double mapNeighbour = .45, mapBend = .25;  // WM-2: bend .3 -> .25 (with mapLip .25 a 0.7 m lower surface 1.4 m away is no longer joined)
// SC-1 (2026-11-18, Speed: what the car can hold; user: "it makes no sense to crash and then recover"): over a crest the wheels stay on
// the road while v^2 x the road's downward bend stays under crestLift x gravity (bend from the map's measured floor over 4 m), and the car
// must be able to brake to that speed by the crest at crestBrake. Sliding sideways faster than slideLimit m/s (or off its wheels while
// driving) holds the speed at slideKeep x the present speed for slideHold s (at least slideFloor), so it does not accelerate into a skid.
constexpr double crestLift = .6, crestBrake = 3.0, gravity = 9.81, slideLimit = 1.5, slideHold = 1.5, slideKeep = .7, slideFloor = 3.0;
// BC-1 (2026-11-19, 19:57 run: 15-18 m/s asked into the ramp's switchback; only the crest capped speed): a bend of the lane line ahead
// caps the speed at sqrt(bendLat / curvature) (curvature from the line's heading change over 2 samples), and the car must be able to
// brake to it by the bend at crestBrake. The sideways acceleration is the plan's own (carcap PlanLat at the gear's grip, D2 2026-11-19;
// was a separate 3.5 m/s^2 here): a bend is taken as the energy plan's rollouts turn.
constexpr double bendMin = .02;  // 1/m: straighter than a 50 m radius is no bend
inline double BendCap(double curvature, double distance, double grip = 1.0) {
    if (!(curvature > bendMin)) return 1e9;
    const double vc = std::sqrt(carcap::kCar.PlanLat(grip) / curvature);
    return std::sqrt(vc * vc + 2 * crestBrake * std::max(0.0, distance));
}
constexpr double mapLip = .25, mapGrade = 1.0, mapReach = 60.0, mapReachEvery = .5, wantWindow = 2.0; constexpr int mapMaxCells = 16000;
    constexpr double wholeBoxMaxHalf = 6.0;  // WB-1: a game box up to this half size (m) stands in World whole; bigger ones (deck pieces) stay band cells
// Fix C: Avoid stays widened until the car is this far past the spot that blocked it (measured along the line), not after any 12 s of
// movement; with no blocked spot known, the old rule (planchoice::kNarrowAfter of moving) applies, and passGiveUp of moving in any case.
constexpr double passMargin = 2.0, passGiveUp = 60.0;
// Fix D: a spot where the car had to back off is remembered for failMemory s. A forward branch that meets its first block there again
// pays failedSpot each time it failed; backing off there again pays repeatReverse each time. spotRadius: the same spot.
constexpr double failMemory = 20.0, failedSpot = 30.0, repeatReverse = 20.0, spotRadius = 3.0;
// Fix F: a low face no higher than climbHeight over the floor is driven over at climbSpeed from climbLead m before it. climbHeight is the
// one car record's climb (carcap.h, D2 2026-11-19: 0.35 m, half a tyre; was tireHeight x 0.5 = 0.5 m here, beside the plan's 0.35).
constexpr double climbHeight = carcap::kCar.climb, climbSpeed = 2.5, climbLead = 4.0;  // half a tire: climbed, never a stop
// Fix B: a low ray edge counts as sure with confirmHits agreeing hits within confirmRadius m, a known normal, and seen within freshHit s;
// otherwise it is doubt: slow to creepSpeed from doubtLead m before it, never a stop (design section 2, "Doubt").
constexpr int confirmHits = 2; constexpr double confirmRadius = .6, freshHit = 3.0, doubtLead = 8.0;
// Climb price (2026-11-07, user: a car forced its way over something it could easily have driven round): climbing is allowed, never free.
// Each low edge a branch climbs costs climbCost, counted up to climbCostCount (several ray hits on one rock are one climb, near enough).
// A round line in the own lane then beats the climb; an edge right across the road has no round line, so every branch pays and it climbs.
constexpr double climbCost = 6.0; constexpr int climbCostCount = 2;
// Asked to move and not moving for this long: that move is impossible, whatever holds the car. Longer than a sleeping body's wake (2.3 s).
constexpr double notMovingAfter = 3.0, notMovingAsked = .5, notMovingSpeed = .15;
// P1 (2026-11-07): judged by distance with hysteresis, not one frame. A gap in asking under notMovingGap s keeps the window open;
// moving notMovingClear m from where it opened closes it; under notMovingMoved m in notMovingAfter s is not moving.
constexpr double notMovingGap = 1.0, notMovingClear = 1.0, notMovingMoved = .3;
// P2: where a forward way did not move the car (m ahead of the nose); forward paths passing it pay failedSpot per failure.
constexpr double pinnedAhead = 1.0;
// R1 (2026-11-07): a committed plan standing at a static block this long gives way to a forward way that clears the block (recovery
// order, not a cost race). Queues and people are waited for, never this. standRelease: it must clear for this long to come back.
constexpr double standWithdraw = 1.0, standRelease = .5;
// L1: a stop this long names what holds the car and every branch tried, then again every stopReportEvery s.
constexpr double stopReportAfter = 10.0, stopReportEvery = 10.0;
// RC-1: backing off to make room for a lane beside a block goes at most this far per stop; it stops as soon as a way forward clears.
constexpr double roomBackMax = 20.0;
// TB-1 (2026-11-12, hairpin 14:04:18-14:05:00: reverse / own lane every 2-5 s against the wall): a committed back-up backs at least this
// far before a forward way takes over, like a three-point turn, unless it stops moving or a confirmed shape makes it infeasible.
constexpr double turnBackMin = 3.0;
// RC-3 (2026-11-07, standing rule: hysteresis): no forward way clears for roomOnS before backing starts; a forward way stays clear for
// roomOffS before backing ends (22:40 run: 494 starts, start/end every 0.2-0.5 s, 216 backed under 0.5 m).
constexpr double roomOnS = 1.0, roomOffS = .5;  // planrun_test sweep 2026-11-07: (1.0, 0.5) the only pair tried that passes all RC-1 layouts
// RC-3: a flicker shorter than this does not restart either timer (the clear-way answer flickers update to update while backing).
constexpr double roomGapS = .3;
// L1: a car rocking within this distance of where it stopped still counts as stopped (a stall loop is a stop).
constexpr double stopRockRadius = 2.5;
// C1 (2026-11-07, source priority: contact first): on its wheels, the car's own floor beats an authored wanted height that disagrees by more
// than contactTrust m; the correction is carried to unmeasured samples ahead, fading to nothing over contactCarry m.
constexpr double contactTrust = .3, contactCarry = 12.0;
// World truth A, body check (shadow): while a ray holds the car, at most every bodyEvery s, rays shaped like the body laid on the measured
// floor along the plan: three across at bodyLow over the floor (above the wheels), one at bodyHigh; segments of kStep to bodyPast m past
// the block; at most bodyRays per check. Recorded only.
// B1.3: the low body rays start just above half a tire, so anything the car may climb is never a body touch.
constexpr double bodyEvery = 1.0, bodyLow = climbHeight + .02, bodyHigh = 1.1, bodyPast = 2.0; constexpr int bodyRays = 12;
// World truth B1 (stop rule, user approval 2026-11-07): an unexplained road or terrain ray hit eases the car to truthEase m/s from doubtLead m
// before it; the body check runs while one lies within bodyNear m ahead, and a touch stands as a stopping shape for bodyHold s.
constexpr double truthEase = 6.0, bodyNear = 14.0, bodyHold = 1.5, truthCacheTtl = 2.0;
// B1.2: a vehicle behind our rear within this much (m) of our line, beyond both half widths, is a follower: it yields to us.
constexpr double followerAcross = .5;
constexpr double followerAlignment = .85;        // cosine of the maximum angle between the lane and a following vehicle
constexpr double rearHold = 6.0;       // s the rear counts as closed after the field refuses a back-up
constexpr double planKeepOff = 2.0;    // m: a committed line further than this from the car is not kept
// The clear-path sweep. World measures with these; Avoid makes the line.
constexpr double scanStep = 2.0, scanLow = .55, scanHigh = 1.2;  // rows along the lane; heights over the floor: over a climbable lip, and body height
constexpr int scanHalf = 6, bodyBins = 2;   // scan lines 1 m apart either side of the lane; metres a car needs clear either side
constexpr double lipProbe = .35, lipStep = .75;   // a step this high with a floor on top is road, not a solid
constexpr int sweepShapes = 48;             // swept solids given to one car per update
constexpr double sweepReach = 8.0, bandClear = 2.5;  // m round the line to look for swept solids; m free of them before the coarse map is overruled
// Look-ahead doubt probe (World, 2026-11-07): up to probeTargets doubtful spots per car per frame, from the stopping distance to probeFar m
// ahead, each probed by 3 direct rays; probeFrameRays across all cars per frame; a spot probed within probeCooldown s and probeSame m is
// skipped. Measures only: the next frame's World sees the answer.
constexpr int maskFrameRays = 320;  // World's volume: one full refresh at the deepest volume (16 columns x 4 levels x 5 segments) fits; a phase is a quarter  // Field Mask: its own per-frame ray budget (four 24-ray masks a frame), counted apart from the doubt probe's. 48 ran out 13 times in 95 casts (19:28 run); a survey ray measured 1.8 us
constexpr int probeTargets = 4, probeFrameRays = 12; constexpr double probeFar = 40.0, probeCooldown = 1.0, probeSame = .5;
// W1 (2026-11-07): each car's share of probeFrameRays is the budget split over the cars that asked last frame, never under probeCarMin.
constexpr int probeCarMin = 3;
// W2 (2026-11-07): a step's floor on top is looked for this far past the face (m), as the survey's lip check does (lipProbe, lipStep).
constexpr double stepLookFrom = 1.0, stepLookTo = 3.0;
// Complete approach planner (shadow first). Avoid geometry, Speed sampling, Arbiter and Ledger budgets.
constexpr int approachBudget = 8, approachNodes = 128;
constexpr double approachStep = .5, approachSeconds = 40, approachDt = .1;
constexpr double approachReverse[3] = {4, 8, 12};
constexpr double approachStopSpeed = .15, approachSettle = .4, approachRejoin = .25;
constexpr double approachDeviation = .05, approachTime = 1, approachReverseCost = 8;
constexpr double approachMargin = .15, approachMarginAbs = .3, approachDwell = .4, approachHold = .6;
constexpr double approachTrackTolerance = 1.5, approachShadowPeriod = 1.0, approachShadowBudgetMs = 4;
// PC-2 (2026-11-08, distance-based replan cadence): near cars and the player's car replan every frame (0 s); middle-distance cars replan
// at 10 Hz (0.10 s) and far cars at 4 Hz (0.25 s), tracking their committed plan between replans and replanning immediately on any
// sudden blocker within stopping distance + replanStopMargin or state change.
constexpr double replanNearDist = 80.0, replanMidDist = 180.0;
constexpr unsigned replanNearPhases = 2;
constexpr int parallelMinBranches = 4, parallelMinShapes = 24;
constexpr unsigned parallelMissBackoffMs = 50;
constexpr double replanStopLineAdvance = .75, replanThreatSide = 2.2, replanThreatPadding = 8.0;
constexpr double replanMidEvery = 0.10, replanFarEvery = 0.25, replanStopMargin = 4.0;
}

// Cheapest first. The order is the order no progress opens them.
// Test (T-2, 2026-11-07): an imperceptible spot test pulse (spottest.h). Never an Avoid family: widening stops at Up.
enum class Family : std::uint8_t { Route, OwnLane, SameWay, Oncoming, Navmesh, Doubt, Hop, Reverse, Trail, Up, Test, Count };
inline const char* Name(Family f) { constexpr const char* n[]{ "route", "own_lane", "same_way", "oncoming", "navmesh", "doubt", "hop", "reverse", "trail", "up", "test" }; return n[int(f)]; }

struct Branch {
    Family family = Family::Route;
    bool valid = false, reverse = false;
    int side = 0;                       // -1 left, +1 right, 0 none
    double lat[kSamples] = {}, up[kSamples] = {};
    double leaveAt = 0, rejoinAt = 0;   // distance ahead it leaves and rejoins the line
    const char* why = "";
};

// Something across the line, in line terms: distance ahead, and its left and right edge.
struct Obstacle { double from = 0, to = 0, left = 0, right = 0, top = 0; bool steerRound = true, low = false; const char* why = ""; bool confirmed = true; };  // RC-2 confirmed: not a prediction, not a person

struct Lanes {
    double ownLeft = -1.75, ownRight = 1.75;
    int count = 0, own = -1;
    double left[kMaxLanes] = {}, right[kMaxLanes] = {};
    bool sameWay[kMaxLanes] = {}, open[kMaxLanes] = {};
    double bandLow = 0, bandHigh = 0;  // route edge no-go bands (m) beyond the lowest / highest lateral lane edge; 0 = none known (older addons)
};
// Field Mask: the lane a lateral offset lies in (-1: none known). The plan's destination lane, for the coordinator's read-only record.
inline int LaneAt(const Lanes& l, double lateral) {
    for (int k = 0; k < l.count && k < kMaxLanes; ++k) if (lateral >= l.left[k] && lateral <= l.right[k]) return k;
    return -1;
}
// Route owns what is wanted: the lane speed, capped at the car's top (traffic may use its boost share).
inline double WantedSpeed(double laneSpeed, double driveTop, bool traffic) { return std::min(laneSpeed, traffic ? driveTop * rules::boostShare : driveTop); }
}
