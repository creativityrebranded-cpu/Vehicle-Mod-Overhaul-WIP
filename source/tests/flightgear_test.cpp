// FC-1 / FG-1 (2026-11-19): per-gear flight handling. Proves gear 3 is today's flight exactly (physical VTOL: vtol::Solve against a frozen
// copy of the Solve that shipped before FC-1; standard hover: the spin easing and side leak against the frozen expressions), the gear
// ordering (rotation lock, drift damping, turn limit, top), the body-axis side push in every gear, the untouched lift, the planner reading
// the same turn as the hull, and the FG-1 autopilot gear rule with its holds.
#include "flightgear.h"
#include "hulllogic.h"
#include "bicycle.h"
#include "vtolphysics.h"
#include <cmath>
#include <cstdio>
#include <string_view>

static int checks = 0, failed = 0;
static void Check(bool ok, const char* name) { ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n", name); } }

// ---- Frozen: vtol::Solve as it was before FC-1 (vtolphysics.h, 2026-11-18). Do not edit: it is the reference gear 3 must equal. ----
namespace frozen {
using namespace vtol;
inline Output Solve(const Body& b, const Command& c, State& state, double dt) {
    Output out;
    if (!Valid(b)||!Finite(c.rates)||!std::isfinite(c.forwardAcceleration)||!std::isfinite(c.manualLift)||
        !std::isfinite(c.verticalSpeed)||!std::isfinite(c.brakeAcceleration)||!std::isfinite(c.speedCap)||!std::isfinite(c.radius)||
        !std::isfinite(c.strafeAcceleration)||!std::isfinite(c.sideAcceleration)||c.sideAcceleration<0||
        !(dt>0&&dt<=0.25)||c.brakeAcceleration<0||c.speedCap<0||c.radius<=0) return out;
    if (!state.referenceMass) state.referenceMass=1/b.motion.inverseMass;
    if (!std::isfinite(state.referenceMass)||state.referenceMass<=0||!std::isfinite(state.trim)) return out;
    constexpr double maxCollective=35, manualCollective=8, verticalGain=4, integralGain=1.5;
    state.trim=std::clamp(state.trim,0.0,30.0);
    const double error=std::clamp(c.verticalSpeed-b.velocity.z,-15.0,15.0);
    const double manual=std::clamp(c.manualLift,-1.0,1.0);
    double collective=manual<0?state.trim*(1+manual):state.trim+manual*manualCollective;
    if (c.assist&&b.up.z>0.35) {
        const double request=(state.trim+verticalGain*error)/b.up.z;
        collective=std::clamp(request,0.0,maxCollective);
        if ((request>0&&request<maxCollective)||(request>=maxCollective&&error<0)||(request<=0&&error>0))
            state.trim=std::clamp(state.trim+integralGain*error*dt,0.0,30.0);
        out.saturated=request<=0||request>=maxCollective;
    }
    collective=std::clamp(collective,0.0,maxCollective);
    const double along=Dot(b.velocity,b.forward);
    double forward=std::clamp(c.forwardAcceleration,-45.0,45.0);
    if (c.brake) forward=-std::clamp(along*2,-c.brakeAcceleration,c.brakeAcceleration);
    else if (c.assist&&std::hypot(b.velocity.x,b.velocity.y)>c.speedCap)
        forward=std::min(forward,-std::clamp(along,0.0,c.brakeAcceleration));
    Vec acceleration=b.forward*forward+b.up*collective;
    const double side=std::min(c.sideAcceleration,12.0);
    if (c.strafeAcceleration!=0) acceleration=acceleration+b.right*std::clamp(c.strafeAcceleration,-12.0,12.0);
    else if (c.assist) acceleration=acceleration+b.right*(-std::clamp(Dot(b.velocity,b.right)*1.2,-side,side));
    const Vec force=Limited(acceleration,65)*state.referenceMass;
    const Vec angularAcceleration=Limited((c.assist?c.rates-b.spin:c.rates)*5,6);
    const Vec angular=AngularImpulse(b.motion,angularAcceleration,dt);
    const double torqueBudget=state.referenceMass*std::max(0.25,c.radius*c.radius)*6;
    out.linear=force*dt;
    out.angular=Limited(angular,torqueBudget*dt);
    out.collective=collective;
    out.valid=Finite(out.linear)&&Finite(out.angular);
    return out;
}
// The standard hover's spin easing and side leak before FC-1 (vehicleflight.cpp kYawResp 4, kTiltResp 5, kSideLeak 1.2).
inline double EaseYaw(double w, double want, double dt) { w += (want - w) * (1.0 - std::exp(-dt * 4.0)); return w; }
inline double EaseTilt(double w, double want, double dt) { w += (want - w) * (1.0 - std::exp(-dt * 5.0)); return w; }
inline double Leak(double side) { return side * 1.2; }
}

static unsigned long long seed = 0x9E3779B97F4A7C15ull;
static double Rnd(double a, double b) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return a + (b - a) * double(seed >> 11) / 9007199254740992.0; }
static vtol::Vec V(double a, double b) { return { Rnd(a, b), Rnd(a, b), Rnd(a, b) }; }
// A body from yaw / pitch / roll (z up, y forward, x right), with a random principal frame.
static vtol::Body RandomBody() {
    vtol::Body b;
    const double y = Rnd(-3.1, 3.1), p = Rnd(-.6, .6), r = Rnd(-.8, .8);
    const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r);
    const vtol::Vec f0{ sy * cp, cy * cp, sp }, r0{ cy, -sy, 0 };
    const vtol::Vec u0 = vtol::Cross(r0, f0);
    b.forward = f0; b.right = r0 * cr - u0 * sr; b.up = vtol::Cross(b.right, b.forward);
    const double qa = Rnd(0, 3.1); b.motion.rotation = { std::sin(qa / 2) * .6, std::sin(qa / 2) * .8, 0, std::cos(qa / 2) };
    b.motion.inverseMass = 1 / Rnd(500, 8000); b.motion.inverseInertia = { 1 / Rnd(500, 9e4), 1 / Rnd(500, 9e4), 1 / Rnd(500, 9e4) };
    b.velocity = V(-25, 25); b.spin = V(-1.5, 1.5);
    return b;
}
static bool Same(const vtol::Output& a, const vtol::Output& b) {
    return a.valid == b.valid && a.saturated == b.saturated && a.collective == b.collective && a.linear.x == b.linear.x && a.linear.y == b.linear.y &&
        a.linear.z == b.linear.z && a.angular.x == b.angular.x && a.angular.y == b.angular.y && a.angular.z == b.angular.z;
}
// The command as vehicleflight.cpp StepPhysical builds it from the row.
static vtol::Command FromRow(vtol::Command c, const carcap::FlightRow& r) {
    c.sideAcceleration = r.sideBudget; c.rateGain = r.rateGain; c.angularLimit = r.angularLimit; c.sideGain = r.sideGain;
    c.sideDead0 = r.sideDead0; c.sideDeadPerSpeed = r.sideDeadPerSpeed; return c;
}

int main() {
    // ---- 1) gear 3 == today's physical VTOL, bit for bit (player and AI; assist on / off; strafe; brake; every dt Solve accepts) ----
    {
        int same = 0, total = 0, sameTrim = 0, valid = 0;
        for (int i = 0; i < 4000; ++i) {
            const bool ai = i & 1;
            const auto b = RandomBody();
            vtol::Command c; c.rates = V(-1.5, 1.5); c.forwardAcceleration = Rnd(-20, 40); c.manualLift = ai ? 0 : Rnd(-1, 1);
            c.verticalSpeed = Rnd(-8, 8); c.brakeAcceleration = Rnd(5, 30); c.speedCap = Rnd(5, 150); c.radius = Rnd(1, 12);
            c.assist = ai || (i % 5 != 0); c.brake = i % 7 == 0; c.strafeAcceleration = ai && i % 3 == 0 ? Rnd(-9, 9) : 0.0;
            c.sideAcceleration = ai ? 6.0 : 3.0;  // today's bound (vtolai::kSideAcceleration / 3.0)
            const double dt = i % 11 == 0 ? Rnd(.05, .25) : i % 13 == 0 ? Rnd(.17, .25) : 1.0 / Rnd(30, 144);
            vtol::State s1, s2; s1.trim = s2.trim = Rnd(4, 20);
            const auto today = frozen::Solve(b, c, s1, dt);
            const auto now = vtol::Solve(b, FromRow(c, carcap::Flight(3, ai)), s2, dt);
            ++total; valid += now.valid; same += Same(today, now); sameTrim += s1.trim == s2.trim && s1.referenceMass == s2.referenceMass;
        }
        std::printf("gear 3 vs frozen Solve: %d / %d identical outputs (%d valid), %d identical lift states\n", same, total, valid, sameTrim);
        Check(same == total && sameTrim == total && valid == total, "gear3_physical_equals_today_bit_for_bit");
        const auto p3 = carcap::Flight(3, false), a3 = carcap::Flight(3, true);
        Check(p3.sideBudget == 3 && a3.sideBudget == 6 && p3.rateGain == 5 && p3.angularLimit == 6 && p3.sideGain == 1.2 && p3.sideDead0 == 0 && p3.sideDeadPerSpeed == 0,
            "gear3_row_is_todays_numbers");
        vtol::Command d; Check(d.rateGain == 5 && d.angularLimit == 6 && d.sideGain == 1.2 && d.sideDead0 == 0 && d.sideDeadPerSpeed == 0, "command_defaults_are_gear3");
    }
    // ---- 2) gear 3 == today's standard hover (spin easing, side leak), exactly ----
    {
        const auto r = carcap::Flight(3, false); int same = 0, total = 0;
        for (int i = 0; i < 4000; ++i) {
            const double w = Rnd(-2, 2), want = Rnd(-2, 2), dt = 1.0 / Rnd(20, 144), side = i % 9 == 0 ? 0.0 : Rnd(-30, 30), hs = Rnd(0, 120);
            same += flightgear::Ease(w, want, r.yawResponse, dt) == frozen::EaseYaw(w, want, dt);
            same += flightgear::Ease(w, want, r.tiltResponse, dt) == frozen::EaseTilt(w, want, dt);
            same += flightgear::SideLeak(r, side, hs) == frozen::Leak(side);
            total += 3;
        }
        std::printf("gear 3 vs frozen standard hover: %d / %d identical\n", same, total);
        Check(same == total, "gear3_standard_equals_today_bit_for_bit");
        hulllogic::Limits base = hulllogic::For(1.6, 3.1, 1.2);
        const auto std3 = hulllogic::Flight(base, false, r.lateralScale);
        Check(std3.lateral == base.lateral && std3.yaw == base.yaw && std3.brakeGain == base.brakeGain, "gear3_standard_hull_is_the_base_hull");
        const auto ph3 = hulllogic::Flight(base, true, r.lateralScale);
        Check(ph3.lateral == std::min(base.lateral, 9.0) && ph3.brakeGain == 2.0, "gear3_physical_hull_is_todays_physical_hull");
    }
    // ---- 3) rotation locks harder in low gears (1-DOF yaw, unit inertia, 60 Hz, through Solve) ----
    {
        double rise[5] = {};
        for (int g = 1; g <= 4; ++g) {
            vtol::Body b; b.motion.inverseMass = 1; b.motion.inverseInertia = { 1, 1, 1 }; vtol::State st; double t = 0; const double dt = 1.0 / 60;
            while (b.spin.z < .9 && t < 10) { vtol::Command c = FromRow({}, carcap::Flight(g, false)); c.rates = { 0, 0, 1 }; c.radius = 1;
                const auto o = vtol::Solve(b, c, st, dt); b.spin = b.spin + o.angular; t += dt; }
            rise[g] = t; std::printf("gear %d yaw 90%% rise %.3f s\n", g, t);
        }
        Check(rise[1] < rise[2] && rise[2] < rise[3] && rise[3] < rise[4] && rise[1] < .3 && rise[4] > .6, "rotation_rise_ordered_by_gear");
        // standard hover: the written spin's 90 % rise
        double srise[5] = {};
        for (int g = 1; g <= 4; ++g) { double w = 0, t = 0; const auto r = carcap::Flight(g, false); while (w < .9 && t < 10) { w = flightgear::Ease(w, 1, r.yawResponse, 1.0 / 60); t += 1.0 / 60; } srise[g] = t; }
        std::printf("standard yaw 90%% rise g1 %.2f g2 %.2f g3 %.2f g4 %.2f s\n", srise[1], srise[2], srise[3], srise[4]);
        Check(srise[1] < srise[2] && srise[2] < srise[3] && srise[3] < srise[4], "standard_rotation_rise_ordered_by_gear");
    }
    // ---- 4) drift: damped hard in low gears, momentum carried in gear 4 (its deadband), both models ----
    {
        double d[5] = {}, sd[5] = {};
        for (int g = 1; g <= 4; ++g) {
            vtol::Body b; b.motion.inverseMass = 1; b.motion.inverseInertia = { 1, 1, 1 }; b.velocity = { 5, 20, 0 }; vtol::State st; double t = 0; const double dt = 1.0 / 60;
            while (std::fabs(b.velocity.x) >= .5 && t < 10) { vtol::Command c = FromRow({}, carcap::Flight(g, false)); c.speedCap = 200;
                const auto o = vtol::Solve(b, c, st, dt); b.velocity.x += o.linear.x; t += dt; }
            d[g] = t >= 10 ? -1 : t;
            double side = 5, ts = 0; const auto r = carcap::Flight(g, false);
            while (std::fabs(side) >= .5 && ts < 10) { side -= flightgear::SideLeak(r, side, std::hypot(side, 20.0)) / 60; ts += 1.0 / 60; }
            sd[g] = ts >= 10 ? -1 : ts;
            std::printf("gear %d drift 5->0.5 m/s physical %.2f s, standard %.2f s\n", g, d[g], sd[g]);
        }
        Check(d[1] > 0 && d[1] < d[2] && d[2] < d[3] && d[4] < 0, "physical_drift_ordered_gear4_carries");
        Check(sd[1] > 0 && sd[1] < sd[2] && sd[2] < sd[3] && sd[4] < 0, "standard_drift_ordered_gear4_carries");
    }
    // ---- 5) the side push stays on the body's right in every gear (real thrusters; the user, 2026-11-19); the lift is untouched ----
    {
        vtol::Body b; b.motion.inverseMass = 1; b.motion.inverseInertia = { 1, 1, 1 };
        const double a = .5; b.right = { std::cos(a), 0, -std::sin(a) }; b.up = { std::sin(a), 0, std::cos(a) }; b.velocity = { 3, 10, 0 };
        bool along = true, liftSame = true; double collective3 = 0, trim3 = 0;
        for (int g = 1; g <= 4; ++g) {
            vtol::State st; vtol::Command c = FromRow({}, carcap::Flight(g, true)); c.verticalSpeed = 1.0;
            const auto o = vtol::Solve(b, c, st, 1.0 / 60);
            vtol::State s0; vtol::Command c0 = c; c0.sideGain = 0; const auto none = vtol::Solve(b, c0, s0, 1.0 / 60);  // the same command without the side push
            const vtol::Vec push = (o.linear - none.linear) * (60.0 * b.motion.inverseMass);
            const double off = vtol::Length(push - b.right * vtol::Dot(push, b.right));
            along = along && off < 1e-9 && vtol::Dot(push, b.right) < 0 && std::fabs(push.z) > 0;  // banked: its vertical share stays
            if (g == 3) { collective3 = o.collective; trim3 = st.trim; }
            liftSame = liftSame && o.collective == none.collective;
        }
        for (int g = 1; g <= 4; ++g) { vtol::State st; vtol::Command c = FromRow({}, carcap::Flight(g, true)); c.verticalSpeed = 1.0;
            const auto o = vtol::Solve(b, c, st, 1.0 / 60); liftSame = liftSame && o.collective == collective3 && st.trim == trim3; }
        Check(along, "side_push_along_body_right_every_gear");
        Check(liftSame, "collective_and_trim_identical_across_rows");
    }
    // ---- 6) a high gain is held stable at a low frame rate; today's gain never is ----
    Check(vtol::StableRateGain(10, .25) == 5 && vtol::StableRateGain(10, 1.0 / 60) == 10 && vtol::StableRateGain(5, .25) == 5 && vtol::StableRateGain(3, .25) == 3,
        "rate_gain_stable_at_low_fps_gear3_untouched");
    // ---- 7) turn limits and tops by gear; the planner turns with the hull's own formula ----
    {
        const auto base = hulllogic::For(1.25, 2.3, 1);
        bool ordered = true, plannerSame = true;
        for (bool physical : { false, true }) for (double v : { 5.0, 15.0, 20.0, 40.0 }) {
            double y[5];
            for (int g = 1; g <= 4; ++g) {
                const auto h = hulllogic::Flight(base, physical, carcap::Flight(g, true).lateralScale); y[g] = h.YawAt(v);
                bicycle::Spec k; k.hoverYaw = h.yaw; k.hoverLat = h.lateral; plannerSame = plannerSame && k.HoverYawAt(v) == h.YawAt(v);
            }
            if (physical && v == 15.0) std::printf("physical yaw limit at 15 m/s g1 %.2f g2 %.2f g3 %.2f g4 %.2f rad/s\n", y[1], y[2], y[3], y[4]);
            ordered = ordered && y[1] >= y[2] && y[2] >= y[3] && y[3] >= y[4];
        }
        Check(ordered, "turn_limit_ordered_by_gear");
        Check(plannerSame, "planner_turn_equals_hull_turn");
        Check(carcap::Flight(1, false).top == 30 && carcap::Flight(2, true).top == 60 && carcap::Flight(3, false).top == 100 && carcap::Flight(4, false).top == 150, "top_per_gear");
        // A planned hover turn at 20 m/s stays within lateral / v (it turned 1.6 rad/s at any speed before FC-1).
        bicycle::Spec k; const auto h = hulllogic::Flight(base, true, carcap::Flight(3, true).lateralScale); k.hoverYaw = h.yaw; k.hoverLat = h.lateral; k.hoverTop = 40;
        bicycle::State s; s.speed = 20; s.mode = 1;
        const auto n = bicycle::HoverStep(s, 1.0, 20, 0, 0, k, .1);
        Check(std::fabs(n.heading - s.heading) / .1 <= h.lateral / 20 + 1e-12 && std::fabs(n.heading - s.heading) > 0, "planned_hover_turn_within_lateral_over_v");
    }
    // ---- 8) FG-1: the autopilot's flight gear ----
    {
        flightgear::Ask a; for (int g = 1; g <= 4; ++g) a.lateral[g] = 9 * carcap::Flight(g, true).lateralScale;
        a.planSpeed = 20; a.speed = 20; a.want = 35; a.floorGap = 20; a.toArrival = 120;
        Check(flightgear::Target(a).gear == 2 && std::string_view(flightgear::Target(a).reason) == "wanted_speed", "fg1_lowest_gear_reaching_the_wanted_speed");
        auto t = a; t.landing = true; Check(flightgear::Target(t).gear == 1 && std::string_view(flightgear::Target(t).reason) == "landing", "fg1_landing_gear1");
        t = a; t.takeOff = true; Check(flightgear::Target(t).gear == 1, "fg1_take_off_gear1");
        t = a; t.floorGap = 2; Check(flightgear::Target(t).gear == 1, "fg1_near_floor_gear1");
        t = a; t.toArrival = 10; Check(flightgear::Target(t).gear == 1, "fg1_near_arrival_gear1");
        t = a; t.obstacle = 12; Check(flightgear::Target(t).gear == 1 && std::string_view(flightgear::Target(t).reason) == "near_obstacle", "fg1_near_obstacle_gear1");
        t = a; t.planSpeed = .3; Check(flightgear::Target(t).gear == 1, "fg1_hover_in_place_gear1");
        t = a; t.strafe = true; Check(flightgear::Target(t).gear == 1, "fg1_strafe_gear1");
        t = a; t.want = 120; t.toArrival = 150; Check(flightgear::Target(t).gear == 4, "fg1_fast_route_gear4");
        t = a; t.peakTurn = .55; Check(flightgear::Target(t).gear == 1 && std::string_view(flightgear::Target(t).reason) == "tight_turn", "fg1_tight_turn_downshifts");
        t = a; t.toArrival = 900; t.peakTurn = 0; Check(flightgear::Target(t).gear == 3 && std::string_view(flightgear::Target(t).reason) == "long_straight_leg", "fg1_long_straight_leg_one_up");
        flightgear::Shift s; flightgear::Pick p; p.gear = 3; p.reason = "x";
        Check(flightgear::Update(s, p, 0) && s.gear == 3, "fg1_first_pick_at_once");
        p.gear = 1; Check(!flightgear::Update(s, p, .1) && !flightgear::Update(s, p, .45) && flightgear::Update(s, p, .55) && s.gear == 1, "fg1_down_after_0_4_s");
        p.gear = 4; Check(!flightgear::Update(s, p, 1) && !flightgear::Update(s, p, 3.4) && flightgear::Update(s, p, 3.6) && s.gear == 2, "fg1_up_one_gear_after_2_5_s");
        Check(!flightgear::Update(s, p, 4) && flightgear::Update(s, p, 6.2) && s.gear == 3, "fg1_next_up_waits_again");
        // Stage F: the thresholds are the car's own (Spec brake / hoverClear / climbRate / hoverAccel, wheelguard::reaction), not fixed metres.
        {
            const auto& k = a.car; const double tr = wheelguard::reaction;
            Check(std::fabs(flightgear::HoldBelow(k) - k.brake * energyplan::kDt) < 1e-12, "stageF_hold_below_is_brake_times_plan_step");
            Check(std::fabs(flightgear::NearFloor(k) - (k.hoverClear + k.climbRate * k.climbRate / (2 * k.hoverAccel))) < 1e-12, "stageF_near_floor_from_hover_clear_and_climb");
            Check(std::fabs(flightgear::StopDistance(20, k.brake) - (400 / (2 * k.brake) + 20 * tr)) < 1e-12, "stageF_stop_distance_v2_over_2b_plus_v_tr");
            Check(std::fabs(flightgear::kDownHold - 2 * tr) < 1e-12, "stageF_down_hold_is_two_reactions");
            // The same 30 m to a stop is near at 20 m/s (needs 37 m) and not near at 8 m/s (needs 11 m): the distance follows the speed.
            auto f = a; f.toArrival = 30; f.speed = 20; f.planSpeed = 20; Check(std::string_view(flightgear::Target(f).reason) == "near_arrival", "stageF_fast_car_30m_from_stop_is_near");
            f.speed = 8; f.planSpeed = 8; f.want = 8; Check(std::string_view(flightgear::Target(f).reason) != "near_arrival", "stageF_slow_car_30m_from_stop_is_not_near");
            // A better brake shortens the zone; a worse one lengthens it (same speed, same distance).
            f = a; f.toArrival = 30; f.speed = 20; f.planSpeed = 20; f.car.brake = 12; Check(std::string_view(flightgear::Target(f).reason) != "near_arrival", "stageF_stronger_brake_shrinks_near_zone");
            f.car.brake = 3; f.toArrival = 60; Check(std::string_view(flightgear::Target(f).reason) == "near_arrival", "stageF_weaker_brake_widens_near_zone");
            // Floor zone follows the climb: a faster climb needs more height.
            f = a; f.floorGap = 6; Check(std::string_view(flightgear::Target(f).reason) != "near_floor", "stageF_6m_over_floor_clear_at_default_climb");
            f.car.climbRate = 9; Check(std::string_view(flightgear::Target(f).reason) == "near_floor", "stageF_faster_climb_widens_floor_zone");
        }
        hoverfollow::Path path; path.count = 3; path.p[0] = { 0, 0, 0, 0, 0 }; path.p[1] = { .2, 0, 0, 0, .1 }; path.p[2] = { .4, 0, 0, 0, .3 };
        Check(std::fabs(flightgear::PeakTurn(path) - 1.0) < 1e-9, "peak_turn_of_a_plan");
    }
    // ---- 9) standard rows are distinct per gear ----
    {
        bool distinct = true;
        for (int g = 1; g < 4; ++g) { const auto a = carcap::Flight(g, false), b = carcap::Flight(g + 1, false);
            distinct = distinct && a.yawResponse > b.yawResponse && a.tiltResponse > b.tiltResponse && a.sideGain > b.sideGain && a.lateralScale > b.lateralScale && a.top < b.top; }
        Check(distinct, "standard_rows_distinct_and_ordered");
    }
    std::printf("%d/%d checks passed\n", checks - failed, checks);
    return failed ? 1 : 0;
}
