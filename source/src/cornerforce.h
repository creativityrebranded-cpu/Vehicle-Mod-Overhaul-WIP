#pragma once
// CF-1 (2026-11-19, user: "we will add four torques at each side of the vehicle front left front right, back left and back right / or just
// forces actually it'll automatically be a torque then build a matrix for the strength of the force / ... it'll serve as a counter balance"
// / "it'll use our steering plus the oscillation and deviation" / "how can it be done without feeling like it's affecting the game" /
// "some gears have max and others have it low till it's negligible"). Controller (design section 9), after the game's wheel step.
// Why: the 00:21 run: the follower asked +0.51 rad/s and the car turned +0.86; asked -0.46 and the car kept turning +0.35 for half a
// second (overshoot and lag about the up axis: the back stepping out); manual driving slid the same way.
// What it does: four corner forces (front left / right, rear left / right; each with a forward and a sideways part) found from one 3 x 8
// matrix so that together they give: no net forward push, the sideways push wanted at the rear axle (the rear stops sliding) and the
// turning moment wanted (the turn rate follows the steering). The minimum-norm solution of that matrix spreads the work over all four
// corners; each corner is capped by the gear's budget / 4, so it can only add grip, never drag the car like a rail.
// Feel: it acts on the difference only: the turn rate the front wheels ask for (v x tan(wheel angle) / wheelbase, never above what the
// tyres hold) against the measured one, and the rear axle's sideways speed; both inside deadbands (small slides pass), outputs smoothed,
// off below minSpeed and when not on the wheels. The sign of the wheel angle is learnt from how the car really turns before the turning
// target is used (until then only the rear's slide is held).
// Units: per unit mass (accelerations, m/s^2) and per unit yaw inertia (radius of gyration k^2 of a uniform box: (L^2 + t^2) / 12).
// Frame: x forward, y left, z up; yaw rate positive counter-clockwise (turning left). Pure: no game calls, no globals, no logging.
#include "carcap.h"
#include <algorithm>
#include <cmath>

namespace cornerforce {

struct Car { double wheelbase = 3.2, track = 1.8, frontShare = .5; };  // frontShare: centre of mass to the front axle as a share of the wheelbase
struct Rules {
    double yawGain = 4.0;        // 1/s: yaw acceleration per rad/s of turn-rate error
    double yawDeadband = .06;    // rad/s
    double latGain = 3.0;        // 1/s: sideways acceleration per m/s of rear slide past its deadband
    double latDead0 = .3, latDeadPerSpeed = .05;  // m/s + share of the speed: the rear's sideways speed that passes untouched
    double minSpeed = 1.0;       // m/s
    double smooth = .10;         // s: time constant of the output filter
    double signEvidence = 1.0;   // s of the car turning the way the wheels point (or always the other way) before the sign is trusted
};
// grip: the gear's tyre grip multiplier (wheelgrip::PlanGrip); the turning target never exceeds the one car record's tyre limit at it
// (carcap TyreLat: 6 m/s^2 x grip, D2 2026-11-19; was a fixed 6 here): target turn rate <= TyreLat(grip) / v.
struct Input { double dt = 0, speed = 0, vSide = 0, yawRate = 0, wheelAngle = 0, budget = 0, grip = 1.0; bool onWheels = true; };
struct Corner { double fx = 0, fy = 0; };
struct Out { double ay = 0, yawAccel = 0, wantYaw = 0, yawErr = 0, rearSlide = 0; Corner c[4]; bool applied = false, capped = false; const char* why = ""; };
struct State { double sign = 0, evidence = 0, ay = 0, yawAccel = 0; };

// Corner positions (x forward, y left): 0 front left, 1 front right, 2 rear left, 3 rear right.
inline void Corners(const Car& car, double x[4], double y[4]) {
    const double a = car.wheelbase * car.frontShare, b = car.wheelbase - a, h = car.track * .5;
    x[0] = a; y[0] = h; x[1] = a; y[1] = -h; x[2] = -b; y[2] = h; x[3] = -b; y[3] = -h;
}
// The 3 x 8 allocation: rows sum Fx = 0, sum Fy = fy, sum (x Fy - y Fx) = mz. Minimum-norm solution u = A^T (A A^T)^-1 b.
inline void Allocate(const Car& car, double fy, double mz, Corner out[4]) {
    double x[4], y[4]; Corners(car, x, y);
    double A[3][8] = {};
    for (int i = 0; i < 4; ++i) { A[0][2 * i] = 1; A[1][2 * i + 1] = 1; A[2][2 * i] = -y[i]; A[2][2 * i + 1] = x[i]; }
    double M[3][3] = {};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) for (int k = 0; k < 8; ++k) M[r][c] += A[r][k] * A[c][k];
    const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    for (int i = 0; i < 4; ++i) out[i] = Corner{};
    if (std::fabs(det) < 1e-12) return;
    double inv[3][3];
    inv[0][0] = (M[1][1] * M[2][2] - M[1][2] * M[2][1]) / det; inv[0][1] = (M[0][2] * M[2][1] - M[0][1] * M[2][2]) / det; inv[0][2] = (M[0][1] * M[1][2] - M[0][2] * M[1][1]) / det;
    inv[1][0] = (M[1][2] * M[2][0] - M[1][0] * M[2][2]) / det; inv[1][1] = (M[0][0] * M[2][2] - M[0][2] * M[2][0]) / det; inv[1][2] = (M[0][2] * M[1][0] - M[0][0] * M[1][2]) / det;
    inv[2][0] = (M[1][0] * M[2][1] - M[1][1] * M[2][0]) / det; inv[2][1] = (M[0][1] * M[2][0] - M[0][0] * M[2][1]) / det; inv[2][2] = (M[0][0] * M[1][1] - M[0][1] * M[1][0]) / det;
    const double bvec[3] = { 0, fy, mz }; double lam[3] = {};
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) lam[r] += inv[r][c] * bvec[c];
    for (int i = 0; i < 4; ++i) { double u0 = 0, u1 = 0; for (int r = 0; r < 3; ++r) { u0 += A[r][2 * i] * lam[r]; u1 += A[r][2 * i + 1] * lam[r]; } out[i] = { u0, u1 }; }
}
inline double YawInertia(const Car& car) { return (car.wheelbase * car.wheelbase + car.track * car.track) / 12.0; }  // k^2 per unit mass

inline Out Update(State& s, const Input& in, const Car& car = {}, const Rules& r = {}) {
    Out o; const double v = std::fabs(in.speed);
    const double k = in.dt <= 0 ? 0.0 : 1.0 - std::exp(-in.dt / std::max(1e-3, r.smooth));
    auto relax = [&](const char* why) { s.ay -= s.ay * k; s.yawAccel -= s.yawAccel * k; o.why = why; return o; };
    if (in.dt <= 0 || in.budget <= 0) return relax("no_budget");
    if (!in.onWheels || v < r.minSpeed) return relax(!in.onWheels ? "not_on_wheels" : "too_slow");
    // learn the wheel angle's sign from how the car turns (forward, a clear angle, a clear turn)
    if (in.speed > 3.0 && std::fabs(in.wheelAngle) > .03 && std::fabs(in.yawRate) > .05) {
        s.evidence += (in.wheelAngle > 0 ? 1.0 : -1.0) * (in.yawRate > 0 ? 1.0 : -1.0) * in.dt;  // seconds of agreement (+) or disagreement (-)
        if (std::fabs(s.evidence) >= r.signEvidence) s.sign = s.evidence > 0 ? 1.0 : -1.0; }
    const double a = car.wheelbase * car.frontShare, b = car.wheelbase - a;
    // the turn rate the steering asks for (reverse turns the other way), never above what the tyres hold
    double want = in.yawRate;
    if (s.sign != 0) { want = s.sign * in.speed * std::tan(in.wheelAngle) / std::max(.5, car.wheelbase);
        const double lim = carcap::kCar.TyreLat(in.grip) / std::max(v, 1.0); want = std::clamp(want, -lim, lim); }
    o.wantYaw = want; o.yawErr = want - in.yawRate;
    const double yawErr = std::fabs(o.yawErr) > r.yawDeadband ? o.yawErr - (o.yawErr > 0 ? r.yawDeadband : -r.yawDeadband) : 0.0;
    // the rear axle's sideways speed (y left): v_rear = vSide - b x yawRate
    o.rearSlide = in.vSide - b * in.yawRate;
    const double dead = r.latDead0 + r.latDeadPerSpeed * v;
    const double slide = std::fabs(o.rearSlide) > dead ? o.rearSlide - (o.rearSlide > 0 ? dead : -dead) : 0.0;
    const double alpha = r.yawGain * yawErr;              // wanted yaw acceleration
    const double aRear = -r.latGain * slide;              // wanted sideways acceleration at the rear axle
    const double ayWant = aRear + b * alpha;              // rear point: a_rear = ay - b x alpha
    Corner c[4]; Allocate(car, ayWant, alpha * YawInertia(car), c);
    double worst = 0; for (auto& q : c) worst = std::max(worst, std::hypot(q.fx, q.fy));
    const double capCorner = in.budget / 4.0, scale = worst > capCorner ? capCorner / worst : 1.0; o.capped = scale < 1.0;
    double ay = 0, mz = 0; double x[4], y[4]; Corners(car, x, y);
    for (int i = 0; i < 4; ++i) { c[i].fx *= scale; c[i].fy *= scale; ay += c[i].fy; mz += x[i] * c[i].fy - y[i] * c[i].fx; o.c[i] = c[i]; }
    s.ay += (ay - s.ay) * k; s.yawAccel += (mz / YawInertia(car) - s.yawAccel) * k;
    o.ay = s.ay; o.yawAccel = s.yawAccel; o.applied = std::fabs(o.ay) > 1e-4 || std::fabs(o.yawAccel) > 1e-4;
    o.why = yawErr != 0 && slide != 0 ? "turn_and_slide" : yawErr != 0 ? (s.sign == 0 ? "learning_steer_sign" : "turn_rate_off_steering") : slide != 0 ? "rear_sliding" : "easing_out";
    return o;
}
}
