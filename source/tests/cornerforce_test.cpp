// CF-1 (2026-11-19): four corner forces from one 3 x 8 matrix hold the turn rate to the steering and the rear from sliding.
#include "cornerforce.h"
#include <cmath>
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
int main() {
    using namespace cornerforce;
    const Car car; const Rules r;
    {   Corner c[4]; Allocate(car, 1.0, 2.0, c); double fx = 0, fy = 0, mz = 0; double x[4], y[4]; Corners(car, x, y);
        for (int i = 0; i < 4; ++i) { fx += c[i].fx; fy += c[i].fy; mz += x[i] * c[i].fy - y[i] * c[i].fx; }
        Check(std::fabs(fx) < 1e-9 && std::fabs(fy - 1.0) < 1e-9 && std::fabs(mz - 2.0) < 1e-9, "the matrix: no net forward push, the wanted sideways push and turning moment exactly"); }
    auto learn = [&](State& s) { for (int i = 0; i < 40; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = .1; in.yawRate = .25; in.budget = 6; Update(s, in, car, r); } };
    {   State s; learn(s); Check(s.sign > 0, "the wheel angle's sign is learnt from how the car turns");
        State w; for (int i = 0; i < 40; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = -.1; in.yawRate = .25; in.budget = 6; Update(w, in, car, r); }
        Check(w.sign < 0, "a reversed sign is learnt too, never assumed"); }
    {   // steady turn exactly as the steering asks: nothing
        State s; learn(s); Out o;
        for (int i = 0; i < 40; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = .1; in.yawRate = 8 * std::tan(.1) / car.wheelbase; in.vSide = (car.wheelbase * .5) * in.yawRate; in.budget = 6; o = Update(s, in, car, r); }
        Check(std::fabs(o.ay) < 1e-3 && std::fabs(o.yawAccel) < 1e-3, "a turn the car makes as steered: no force at all (it does not feel like a hand)"); }
    {   // the 00:21 overshoot: asked +0.51 rad/s, turning +0.86 -> yaw accelerated back toward the ask
        State s; learn(s); Out o; const double ask = .51, delta = std::atan(ask * car.wheelbase / 8.0);
        for (int i = 0; i < 10; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = delta; in.yawRate = .86; in.vSide = 1.6 * .86; in.budget = 6; o = Update(s, in, car, r); }
        Check(o.yawAccel < -.1 && std::fabs(o.wantYaw - ask) < 1e-6, "overshoot (+0.86 against +0.51 asked): turned back toward the ask"); }
    {   // the lag: steering reversed (-0.46) while the car still turns +0.35
        State s; learn(s); Out o; const double delta = std::atan(-.46 * car.wheelbase / 8.0);
        for (int i = 0; i < 10; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = delta; in.yawRate = .35; in.vSide = 1.6 * .35; in.budget = 6; o = Update(s, in, car, r); }
        Check(o.yawAccel < -.3, "lag (steering reversed, still turning the old way): pushed into the new turn"); }
    {   // the back stepping out: rear axle sliding 2 m/s to the left with no turn asked
        State s; learn(s); Out o;
        for (int i = 0; i < 10; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = 0; in.yawRate = 0; in.vSide = 2.0; in.budget = 6; o = Update(s, in, car, r); }
        Check(o.rearSlide > 1.9 && o.ay < -.1, "the rear sliding left: pushed back to the right"); }
    {   // a small slide passes untouched (the car still feels alive)
        State s; learn(s); Out o;
        for (int i = 0; i < 10; ++i) { Input in; in.dt = .05; in.speed = 8; in.wheelAngle = 0; in.yawRate = 0; in.vSide = .4; in.budget = 6; o = Update(s, in, car, r); }
        Check(std::fabs(o.ay) < 1e-6 && std::fabs(o.yawAccel) < 1e-6, "a 0.4 m/s slide at 8 m/s passes untouched"); }
    {   // per gear: each corner at most budget / 4; a near-zero budget is negligible
        State s; learn(s); Out o;
        for (int i = 0; i < 10; ++i) { Input in; in.dt = .05; in.speed = 10; in.wheelAngle = 0; in.yawRate = 1.5; in.vSide = 5; in.budget = 6; o = Update(s, in, car, r); }
        double worst = 0; for (auto& q : o.c) worst = std::max(worst, std::hypot(q.fx, q.fy));
        Check(o.capped && worst <= 6.0 / 4 + 1e-9, "a big spin: each corner capped at the gear's budget / 4 (gear 1: 1.5 m/s^2)");
        State g3; learn(g3); Out q; for (int i = 0; i < 10; ++i) { Input in; in.dt = .05; in.speed = 10; in.wheelAngle = 0; in.yawRate = 1.5; in.vSide = 5; in.budget = .5; q = Update(g3, in, car, r); }
        Check(std::fabs(q.ay) < .5 && std::fabs(q.yawAccel) * YawInertia(car) < .5, "gear 3 (budget 0.5 m/s^2): negligible"); }
    {   State s; learn(s); Out o; Input in; in.dt = .05; in.speed = .5; in.yawRate = 2; in.budget = 6; o = Update(s, in, car, r);
        Check(!o.applied, "off below 1 m/s");
        in.speed = 8; in.onWheels = false; o = Update(s, in, car, r); Check(!o.applied || std::fabs(o.ay) < 1e-3, "off when not on the wheels"); }
    {   // smoothing: the first tick does not jump to the full value
        State s; learn(s); Input in; in.dt = .016; in.speed = 8; in.wheelAngle = 0; in.yawRate = 0; in.vSide = 3; in.budget = 6;
        const auto o1 = Update(s, in, car, r); Out o; for (int i = 0; i < 60; ++i) o = Update(s, in, car, r);
        Check(std::fabs(o1.ay) < std::fabs(o.ay) * .3, "eased in over about 0.1 s, never a jolt"); }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
