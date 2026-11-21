// AF-1: the path follower, closed loop on the rail's own turn behaviour (the turn command eases 0.3 per step toward its target, capped at
// min(1.6, 0.4 + |v|/4) rad/s, exactly as vehicleflight's rail stage), compared with the locked handoff it replaces.
#include "pathfollow.h"
#include "wheelgrip.h"
#include <cstdio>
#include <cmath>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
constexpr double kPi = pathfollow::kPi;

struct Car { double x = 0, y = 0, heading = 0, yawCmd = 0; };
// Old forward law: aim at the path point `look` m ahead (bearing rate-limited 2 rad/s), turn at 2.5 x the bearing error.
// Old reverse law: target heading = own heading (no turn).
struct Result { double worstLat = 0, endLat = 0, endHeadingErr = 0; };

static void Arc(pathfollow::Path& p, double cx, double cy, double r, double a0, double a1, int n) {
    p.count = 0; for (int i = 0; i < n && i < pathfollow::kMaxPoints; ++i) { const double a = a0 + (a1 - a0) * i / (n - 1); p.x[p.count] = cx + r * std::cos(a); p.y[p.count] = cy + r * std::sin(a); ++p.count; }
}
static double LatTo(const pathfollow::Path& p, const Car& c, bool reverse) { return pathfollow::Follow(p, c.x, c.y, c.heading, reverse ? -1 : 1, reverse).lateral; }

static Result Run(const pathfollow::Path& p, Car c, double speed, bool reverse, bool newLaw, double seconds, int skipStart = 20) {
    Result r; double aim = c.heading, aimAt = 0; const double dt = 1.0 / 30;
    for (int k = 0; k < int(seconds / dt); ++k) {
        double yawTarget;
        if (newLaw) yawTarget = pathfollow::Follow(p, c.x, c.y, c.heading, reverse ? -speed : speed, reverse).yawRate;
        else if (reverse) yawTarget = 0;  // bearing = own heading: error 0
        else {
            // pure pursuit point 5 m ahead along the path from the nearest point
            const auto f = pathfollow::Follow(p, c.x, c.y, c.heading, speed, false);
            double left = 5.0; int i = f.segment; double tx = p.x[p.count - 1], ty = p.y[p.count - 1];
            for (int j = std::max(i, 0); j + 1 < p.count; ++j) { const double l = std::hypot(p.x[j + 1] - p.x[j], p.y[j + 1] - p.y[j]); if (l >= left) { tx = p.x[j] + (p.x[j + 1] - p.x[j]) * left / l; ty = p.y[j] + (p.y[j + 1] - p.y[j]) * left / l; break; } left -= l; }
            const double raw = std::atan2(tx - c.x, ty - c.y); (void)aimAt;
            aim += std::clamp(std::remainder(raw - aim, 2 * kPi), -2.0 * dt, 2.0 * dt);
            yawTarget = 2.5 * std::remainder(aim - c.heading, 2 * kPi);
        }
        const double maxYaw = std::min(1.6, .4 + speed / 4.0);
        c.yawCmd += (std::clamp(yawTarget, -maxYaw, maxYaw) - c.yawCmd) * .3;
        c.heading += c.yawCmd * dt;
        const double v = reverse ? -speed : speed;
        c.x += v * std::sin(c.heading) * dt; c.y += v * std::cos(c.heading) * dt;
        if (k >= skipStart) r.worstLat = std::max(r.worstLat, std::fabs(LatTo(p, c, reverse)));
    }
    const auto f = pathfollow::Follow(p, c.x, c.y, c.heading, reverse ? -speed : speed, reverse);
    r.endLat = f.lateral; r.endHeadingErr = f.headingError;
    return r;
}

// Approximate the wheel actuator and body's lag, using the real command slew limit.
static double DelayedWheelRun(bool damped, double initialHeading) {
    pathfollow::Path lane;
    lane.count = 30;
    for (int i = 0; i < lane.count; ++i) lane.y[i] = i * 5.0;
    constexpr double dt = 1.0 / 60, speed = 4.5, wheelbase = 3.3, lock = kPi / 4;
    double x = -1.25, y = 5, heading = initialHeading, yaw = .42, lastYawWant = 0, latePeak = 0;
    wheelgrip::Steer steering;
    steering.out = .37; steering.at = 0;
    for (int i = 1; i <= 720; ++i) {
        const auto f = pathfollow::Follow(lane, x, y, heading, speed, false);
        const double target = damped ? pathfollow::DampedLaneYaw(f, speed, yaw, steering.out, wheelgrip::kSteerRate) : f.yawRate;
        lastYawWant += .3 * (std::clamp(target, -1.525, 1.525) - lastYawWant);
        const double want = std::clamp(std::atan(wheelbase * lastYawWant / speed) / lock, -1.0, 1.0);
        const auto so = wheelgrip::SmoothSteer(steering, want, speed, wheelbase, lock, 1.25, i * dt);
        const double wheelYaw = speed * std::tan(so.steer * lock) / wheelbase;
        yaw += (wheelYaw - yaw) * dt / .28;
        heading += yaw * dt;
        x += speed * std::sin(heading) * dt;
        y += speed * std::cos(heading) * dt;
        if (i > 300) latePeak = std::max(latePeak, std::fabs(x));
    }
    return latePeak;
}

int main() {
    {
        pathfollow::Out f;
        f.ok = true; f.headingError = .20; f.yawRate = .56;
        const auto coasting = pathfollow::DampedLaneYaw(f, 4.5, .5, 0, wheelgrip::kSteerRate);
        const auto unwinding = pathfollow::DampedLaneYaw(f, 4.5, .5, .65, wheelgrip::kSteerRate);
        Check(coasting > 0 && unwinding < 0, "existing wheel command makes the follower unwind before crossing the heading target");
        f.headingError = 0; f.curvature = .08;
        Check(std::fabs(pathfollow::DampedLaneYaw(f, 5, .4, .5, wheelgrip::kSteerRate) - .4) < 1e-9,
              "yaw damping preserves the nominal turn of a steady curve");
        for (double heading : {-.32, .32}) {
            const double before = DelayedWheelRun(false, heading), after = DelayedWheelRun(true, heading);
            std::printf("   delayed wheel model, initial yaw angle %+.2f: late lateral peak %.3f -> %.3f m\n", heading, before, after);
            Check(after < .25 && after < before, "rate-limited wheel model settles with the damped lane controller");
        }
    }
    {   // The hairpin case: a right-hand curve of 10 m radius at 2 m/s, the car starting on it.
        pathfollow::Path p; Arc(p, 10, 0, 10, kPi, kPi - 2.4, 30);  // from (0,0) heading north, curving right (clockwise round (10,0))
        Car c; c.heading = 0;
        const auto older = Run(p, c, 2.0, false, false, 9), newer = Run(p, c, 2.0, false, true, 9);
        std::printf("   10 m curve at 2 m/s: worst lateral old %.2f m, new %.2f m\n", older.worstLat, newer.worstLat);
        Check(newer.worstLat < .3, "forward 10 m radius at 2 m/s: stays within 0.3 m of the plan");
        Check(newer.worstLat < older.worstLat, "forward: tighter than the locked handoff");
    }
    {   // Straight at 12 m/s, starting 1.5 m off: converges without swinging past.
        pathfollow::Path p; p.count = 30; for (int i = 0; i < 30; ++i) { p.x[i] = 0; p.y[i] = i * 4.0; }
        Car c; c.x = -1.5;
        const auto r = Run(p, c, 12.0, false, true, 6, 120);
        Check(std::fabs(r.endLat) < .15 && std::fabs(r.endHeadingErr) < .05, "forward straight 12 m/s from 1.5 m off: converges");
    }
    {   // Reverse along a curve (radius 8 m) behind the car: the old handoff backs straight off it, the new one follows.
        pathfollow::Path p; Arc(p, -8, 0, 8, 0, -1.6, 24);   // from (0,0) backing toward -y, the path curving away to the left of travel
        Car c; c.heading = 0;                                   // nose north, backing south
        const auto older = Run(p, c, 1.8, true, false, 6), newer = Run(p, c, 1.8, true, true, 6);
        std::printf("   reverse 8 m curve: worst lateral old %.2f m, new %.2f m\n", older.worstLat, newer.worstLat);
        Check(newer.worstLat < .35, "reverse along an 8 m radius: stays within 0.35 m of the plan");
        Check(older.worstLat > 1.0, "reverse: the locked handoff (steer 0) leaves that path");
    }
    {   // Realign while backing: the car 30 deg off a straight lane, the plan backs along the lane. The angle must close.
        pathfollow::Path p; p.count = 20; for (int i = 0; i < 20; ++i) { p.x[i] = 0; p.y[i] = -i * 1.0; }
        Car c; c.heading = 30 * kPi / 180;
        const auto older = Run(p, c, 1.8, true, false, 6, 0), newer = Run(p, c, 1.8, true, true, 6, 0);
        std::printf("   reverse realign: end heading error old %.1f deg, new %.1f deg\n", older.endHeadingErr * 180 / kPi, newer.endHeadingErr * 180 / kPi);
        Check(std::fabs(newer.endHeadingErr) < 5 * kPi / 180, "reverse realign: the 30 deg angle to the lane closes");
        Check(std::fabs(older.endHeadingErr) > 25 * kPi / 180, "reverse realign: the locked handoff keeps the angle");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
