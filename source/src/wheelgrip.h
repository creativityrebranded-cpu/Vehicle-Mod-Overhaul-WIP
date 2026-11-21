#pragma once
// GT-1 (2026-11-19, user: "Yes, restore both"): the gear's handling is tyre grip AND turning weight together. The 21:02 run with turning
// weight alone (WO-1) snaked up the ramp: back slip 21 deg (was 9), side-to-side turn rate 0.37 rad/s (was 0.08) and drove into the right
// wall; the 19:54 run that held about 3 deg at speed had both. Restored: GR-1 / RB-1 tyre friction x gear (front and rear axle) beside
// YW-1 turning weight. settings.ini [grip] gearN (friction %), rearN (rear %), weightN (%). Gear 3 = the game's own car.
// Tyres (decomp): data +0xA0 WheelComponentParams (0x28 a wheel): +0x0C friction, +0x14 max friction, read every step (FUN_1430A9750).
// WO-1 (2026-11-18, REPLACED by GT-1 above): the gear's handling
// is the body's turning weight (YW-1). The only change between the 19:24 run (rear grip x2.08, back slip ~19 deg in gear 1) and the
// 19:54 run (the same + turning weight, ~10 deg; 4 deg over 12 m/s) was the turning weight. REMOVED with it: GR-1 / RB-1 tyre friction
// x gear (front and rear; proven applied every shift, no felt change) and the YW-1 lever test keys. settings.ini [grip] gearN / rearN
// are no longer read (left in place, harmless). The tyres are the game's own (grip 1 for the plan and the steering cap).
// Actuator (design section 9). Decomp: hknpBSVehicleInstance +0x38 is its hknpVehicleData. hkpVehicleData init FUN_1430A7670 bakes
// data +0x1B0..+0x1BC = invMass x sum_k (torqueFactor_k / unitInertia_k) x |orientation row k, column i| (rows: +0x40 yaw, +0x50 roll,
// +0x60 pitch; factors +0x78 / +0x70 / +0x74 over inertias +0x84 / +0x88 / +0x8C), w = invMass; the step FUN_1430AA430 reads +0x1B0
// every step into the chassis accumulator for the friction solver FUN_143169320. The raw +0x70..+0x8C are read only at init.
// GO-1 (2026-11-19, stage 3 G "one owner per output"): the data block is the car's OWN, not the model's. Evidence: (a) logs, 9 runs, 62
// event=block lines: every car instance had its own data address, also same-model cars loaded together (identical inv_inertia and
// friction, different data=); 467 event=applied lines all cars_sharing_block=1; a reloaded car (body 010019F5, run-2123) got a new block;
// (b) decomp: the wheel params are filled by bhkVehicleSetup's virtual FUN_1423C3270 (vtable 0x144C47098 slot 4) into the data it is
// handed, the Havok setup pattern that builds one hknpVehicleData per vehicle instance; FUN_1430A7670 then bakes +0x1B0 in that block.
// So friction (+0xA0 params) and turning weight (+0x1B0) are written per car. Should a block ever be shared, one owner decides it:
// the player's car (pilot slot 0) owns a block it uses; AI cars do not change it while that car is fresh; without it, the highest ask.
#include "carcap.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace wheelgrip {
constexpr std::ptrdiff_t kData = 0x38, kWheels = 0xA0, kCount = 0x30, kStride = 0x28, kFriction = 0x0C, kMaxFriction = 0x14;
constexpr int kMaxWheelCount = 8;
constexpr std::ptrdiff_t kChassisInvInertia = 0x1B0, kOrientRows = 0x40, kTorqueFactors = 0x70, kUnitInertias = 0x84;
constexpr double kUserKeep = 2.0;  // s: a car that has not stepped for this long no longer asks anything of its block

struct Chassis { float inv[4] = {}, rows[3][4] = {}, torque[3] = {}, inertia[3] = {}; bool ok = false; };  // torque/inertia: yaw, roll, pitch
struct Tyres { float friction[kMaxWheelCount] = {}, maxFriction[kMaxWheelCount] = {}; int wheels = 0; };  // GT-1: the game's own, read once
constexpr int kPlayerPilot = 0;  // pilot slot 0: the car the player is in
struct User { double front = 1.0, rear = 1.0, weight = 1.0, at = -1e9; int pilot = -1; };  // pilot: FVO slot, -1 = a car FVO does not fly
struct Block { Chassis chassis; Tyres tyres; bool read = false; std::unordered_map<std::uintptr_t, User> users;
               double appliedFront = 1.0, appliedRear = 1.0, appliedWeight = 1.0; int appliedOwner = -1; bool everShared = false; };
// The baked inverse inertia with the yaw inertia x weight (weight 2 = twice as hard to turn the body about its up axis). Each component
// is rescaled by its own share: new_i = inv_i x sum_k r'_k |R_k[i]| / sum_k r_k |R_k[i]| (r = factor / inertia). False: not plausible.
inline bool YawWeighted(const Chassis& c, double weight, float out[4]) {
    double r[3], rn[3];
    for (int k = 0; k < 3; ++k) { if (!std::isfinite(c.torque[k]) || !std::isfinite(c.inertia[k]) || c.inertia[k] <= 1e-6 || c.torque[k] < 0) return false;
        r[k] = c.torque[k] / c.inertia[k]; rn[k] = r[k]; }
    rn[0] = r[0] / std::clamp(weight, .25, 10.0);
    for (int i = 0; i < 3; ++i) {
        double den = 0, num = 0; for (int k = 0; k < 3; ++k) { den += r[k] * std::fabs(c.rows[k][i]); num += rn[k] * std::fabs(c.rows[k][i]); }
        if (!std::isfinite(c.inv[i]) || c.inv[i] < 0) return false;
        out[i] = den > 1e-12 ? float(c.inv[i] * num / den) : c.inv[i]; }
    out[3] = c.inv[3];
    return true;
}
// GO-1: a car (`instance`, FVO pilot slot `pilot`, -1 if none) asks for its gear's front / rear tyre grip and turning weight at `now`.
// Returns what the block should have and who decided it. Stale cars are dropped first. One owner, in this order:
//   own     - one fresh car uses the block (the normal case: blocks are per car): its ask; perInstance = 1;
//   player  - several cars share it and the player's car (slot 0) is one of them: the player's ask; the others' asks are ignored;
//   highest - several share it, no player's car: per value the highest any fresh car asks (owner = the slot asking the most weight).
// held = this car's ask differs from what the block gets (another car owns it). Nobody fresh: the game's own (1).
struct Ask { double front = 1.0, rear = 1.0, weight = 1.0; };
enum class Rule { Own, Player, Highest };
inline const char* RuleName(Rule r) { return r == Rule::Own ? "own" : r == Rule::Player ? "player" : "highest"; }
struct Decision { Ask want; int owner = -1; Rule rule = Rule::Own; bool perInstance = true, held = false; int users = 0; };
inline Decision Want(Block& b, std::uintptr_t instance, int pilot, const Ask& a, double now) {
    b.users[instance] = { a.front, a.rear, a.weight, now, pilot };
    for (auto it = b.users.begin(); it != b.users.end();) {
        if (now - it->second.at > kUserKeep || now < it->second.at - 1e-9) it = b.users.erase(it); else ++it; }
    Decision d; d.users = int(b.users.size());
    if (d.users > 1) b.everShared = true;
    d.perInstance = d.users <= 1;
    const User* player = nullptr;
    for (const auto& u : b.users) if (u.second.pilot == kPlayerPilot) { player = &u.second; break; }
    if (d.users <= 1) { d.rule = Rule::Own; d.owner = pilot; d.want = a; }
    else if (player) { d.rule = Rule::Player; d.owner = kPlayerPilot; d.want = { player->front, player->rear, player->weight }; }
    else {
        d.rule = Rule::Highest; Ask best{ 0, 0, 0 }; double heaviest = -1;
        for (const auto& u : b.users) {
            best.front = std::max(best.front, u.second.front); best.rear = std::max(best.rear, u.second.rear); best.weight = std::max(best.weight, u.second.weight);
            if (u.second.weight > heaviest || (u.second.weight == heaviest && u.second.pilot >= 0 && (d.owner < 0 || u.second.pilot < d.owner))) { heaviest = u.second.weight; d.owner = u.second.pilot; } }
        d.want = best; }
    if (!(d.want.front > 0)) d.want.front = 1.0; if (!(d.want.rear > 0)) d.want.rear = 1.0; if (!(d.want.weight > 0)) d.want.weight = 1.0;
    d.held = std::fabs(d.want.front - a.front) > 1e-9 || std::fabs(d.want.rear - a.rear) > 1e-9 || std::fabs(d.want.weight - a.weight) > 1e-9;
    return d;
}
inline int FreshUsers(const Block& b, double now) { int n = 0; for (const auto& u : b.users) if (now - u.second.at <= kUserKeep) ++n; return n; }

// The gear profile, for every FVO car on its wheels, driven by hand or by the autopilot (auto gear AG-1 picks it for Cruise):
//   friction    - tyre friction x (GR-1), every wheel;
//   rear        - the rear wheels' extra friction on top (RB-1; the rear holds the back behind the front);
//   weight      - turning weight x (YW-1);
//   assist      - CF-1: the four corner forces' budget, m/s^2 (cornerforce.h; each corner at most a quarter of it);
//   slipTarget  - m/s of wheel slip the traction control allows (0 = off; off by default: its input WheelInfo +0xB4 is unproven).
// settings.ini [grip]: gearN (friction %), rearN (%), weightN (%), assistN (tenths of m/s^2), slipN (tenths of m/s). Gear 3 = the game's car.
struct Profile { double friction = 1.0, rear = 1.0, assist = 0.0, slipTarget = 0.0, weight = 1.0; };
constexpr int kDefaultPercent[5] = { 100, 160, 125, 100, 100 };
constexpr int kDefaultRearPercent[5] = { 100, 130, 120, 100, 115 };
constexpr int kDefaultAssistTenths[5] = { 0, 60, 40, 5, 20 };  // CF-1 corner budget (tenths of m/s^2): gear 3 negligible
constexpr int kDefaultSlipTenths[5] = { 0, 0, 0, 0, 0 };
constexpr int kDefaultWeightPercent[5] = { 100, 250, 175, 100, 150 };
struct Settings { int friction[5] = { 100, 160, 125, 100, 100 }, rear[5] = { 100, 130, 120, 100, 115 }, assist[5] = { 0, 60, 40, 5, 20 },
    slip[5] = { 0, 0, 0, 0, 0 }, weight[5] = { 100, 250, 175, 100, 150 }; };
inline Profile ProfileOf(int gear, const Settings& s) {
    const int g = std::clamp(gear, 1, 4); Profile p;
    p.friction = std::clamp(s.friction[g] / 100.0, .25, 4.0);
    p.rear = std::clamp(s.rear[g] / 100.0, .5, 3.0);
    p.assist = std::clamp(s.assist[g] / 10.0, 0.0, 30.0);
    p.slipTarget = s.slip[g] <= 0 ? 0.0 : std::clamp(s.slip[g] / 10.0, .1, 20.0);
    p.weight = std::clamp(s.weight[g] / 100.0, .25, 10.0);
    return p;
}
// The grip the plan and the steering cap may count on in this gear (the front tyres': they steer).
inline double PlanGrip(int gear, const Settings& s) { return gear > 0 ? ProfileOf(gear, s).friction : 1.0; }
// RB-1: the bicycle model's rear slip angle (deg): atan(|sideways speed at the rear axle| / forward speed); the rear axle's sideways
// speed is v_side - b x yaw rate (b: centre to rear axle). Under 2 m/s forward: 0 (not defined at a crawl).
inline double RearSlipDeg(double vForward, double vSide, double yawRate, double b) {
    if (std::fabs(vForward) < 2.0) return 0.0;
    return std::atan(std::fabs(vSide - b * yawRate) / std::fabs(vForward)) * 57.29577951308232;
}

// GK-1 Assist and GK-2 Yaw REMOVED 2026-11-19: CF-1 four corner forces (cornerforce.h) replace them.
struct V { double x = 0, y = 0, z = 0; };
inline double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V Sc(V a, double k) { return { a.x * k, a.y * k, a.z * k }; }
inline V Sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V Unit(V a) { const double l = std::sqrt(Dot(a, a)); return l > 1e-9 ? Sc(a, 1 / l) : V{}; }
inline V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

// Traction control: the engine's torque factor from the wheels' slip (the game's own forward slip). Over the target it falls (at most
// to minFactor); it comes back as the tyres bite. Smoothed so the engine does not pulse.
constexpr double kTcMin = .3, kTcFall = 3.0, kTcRise = 1.5;  // factor per second down / up
inline double Traction(double factor, double slip, double target, double dt) {
    const double want = slip <= target ? 1.0 : std::max(kTcMin, 1.0 - (slip - target) / (2.0 * target + 1.0));
    const double step = (want < factor ? kTcFall : kTcRise) * std::max(0.0, dt);
    return std::clamp(factor + std::clamp(want - factor, -step, step), kTcMin, 1.0);
}
// Engine fields (Havok's default engine, instance +0x50; layout from the SDK: +0x10 min rpm, +0x14 opt rpm, +0x18 max rpm, +0x1C max
// torque). GK-2 (18:11 run): the game's engines read min 0, opt 0, max 1000 rpm, torque 5500 (rejected by the old rising-rpm check, so
// traction control never ran). Touched when torque is 1..1e6 and the rpms are in order (0 allowed) under 30000.
constexpr std::ptrdiff_t kEngine = 0x50, kMinRpm = 0x10, kOptRpm = 0x14, kMaxRpm = 0x18, kMaxTorque = 0x1C;
inline bool EnginePlausible(float minRpm, float optRpm, float maxRpm, float torque) {
    (void)minRpm; (void)optRpm; (void)maxRpm;  // GK-3 (18:21 run): the rpms read ~0 / ~0 / 1000 and still failed an order test; torque decides
    return std::isfinite(torque) && torque >= 1 && torque <= 1e6;
}
// ST-1 (2026-11-18, user: "is the issue the steering strength?" / "Fix both"; Controller, design section 9): the autopilot's steering
// on its wheels before it goes to the game. 18:21 run: at 5-6 m/s the command flipped +0.2 / -0.45 / +0.3 / -0.39 every half second on a
// car with a 45 degree lock and no steering reduction until 45 m/s (vehicle.model full_steer_speed 45): it snaked. Two rules:
//   cap  - the wheel angle whose turn the tyres carry at this speed: atan(wheelbase x kSteerLat x grip / v^2) as a share of the lock (full
//          lock at walking pace, about 0.3 at 10 m/s, 0.13 at 15 m/s on a REV-8 in gear 3);
//   rate - the command moves at most kSteerRate per second (a flip from +0.4 to -0.4 takes 0.6 s, not one frame).
constexpr double kSteerRate = 1.3, kSteerSlow = 2.0;  // the tyre limit is the one car record's (carcap TyreLat)
struct Steer { double out = 0, at = -1; };
struct SteerOut { double steer = 0, cap = 1, wanted = 0; bool capped = false, rateLimited = false; };
inline SteerOut SmoothSteer(Steer& s, double wanted, double speed, double wheelbase, double lockRad, double grip, double now) {
    SteerOut o; o.wanted = wanted;
    const double v = std::fabs(speed), L = wheelbase > .5 ? wheelbase : 3.0, lock = lockRad > .05 ? lockRad : .6;
    o.cap = v <= kSteerSlow ? 1.0 : std::clamp(std::atan(L * carcap::kCar.TyreLat(grip) / (v * v)) / lock, .08, 1.0);
    double w = std::clamp(wanted, -o.cap, o.cap); o.capped = std::fabs(w - wanted) > 1e-9;
    const double dt = s.at < 0 || now < s.at ? 0.0 : std::min(.5, now - s.at); s.at = now;
    if (dt > 0) { const double step = kSteerRate * dt; const double moved = std::clamp(w - s.out, -step, step); o.rateLimited = std::fabs(moved - (w - s.out)) > 1e-9; w = s.out + moved; }
    s.out = w; o.steer = w; return o;
}
}
