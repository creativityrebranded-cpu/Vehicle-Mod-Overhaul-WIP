#include "../src/freeroll.h"
#include "../src/hoverlogic.h"
#include <cmath>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

// A body basis (rows right, forward, up) turned by a world angular velocity for dt (Rodrigues, re-orthonormalised).
struct Body { float R[3] = { 1, 0, 0 }, F[3] = { 0, 1, 0 }, U[3] = { 0, 0, 1 }; };
static void Turn(float v[3], const float w[4], double dt) {
    const double wx = w[0], wy = w[1], wz = w[2], a = std::sqrt(wx * wx + wy * wy + wz * wz) * dt;
    if (a < 1e-12) return;
    const double k[3] = { wx * dt / a, wy * dt / a, wz * dt / a };
    const double c = std::cos(a), s = std::sin(a), d = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    const double x[3] = { k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0] };
    for (int i = 0; i < 3; ++i) v[i] = float(v[i] * c + x[i] * s + k[i] * d * (1 - c));
}
static void Step(Body& b, const float w[4], double dt) { Turn(b.R, w, dt); Turn(b.F, w, dt); Turn(b.U, w, dt); }
static double Roll(const Body& b) { return hoverlogic::Roll(b.R[2], b.U[2]); }

int main()
{
    using namespace freeroll;
    {
        Body b; float w[4];
        Compose(b.R, b.F, b.U, 0, 1.0, 0, w);
        Step(b, w, 0.1);
        check(b.R[2] < 0 && Roll(b) > 0, "a positive roll rate puts the right side down (hoverlogic::Roll +)");
        Body c; Compose(c.R, c.F, c.U, 1.0, 0, 0, w); Step(c, w, 0.1);
        check(c.F[2] > 0, "a positive pitch rate lifts the nose");
        Body d; Compose(d.R, d.F, d.U, 0, 0, 1.0, w); Step(d, w, 0.1);
        check(d.F[0] > 0, "a positive yaw rate turns the nose right");
    }
    {
        // A held roll key goes all the way round and back to level (no limit).
        Body b; float w[4]; double turned = 0, last = 0;
        const double rate = Rate(1.0);
        for (int i = 0; i < 2000 && turned < 6.3; ++i) {
            const double want = RollWant(Roll(b), 1.0, rate, 1.2, 0.0, true, false, false);
            Compose(b.R, b.F, b.U, 0, want, 0, w); Step(b, w, 1.0 / 60.0);
            const double r = Roll(b); turned += std::fabs(Wrap(r - last)); last = r;
        }
        check(turned >= 6.28, "a held roll key rolls a full 360 degrees");
        check(std::fabs(Roll(b)) < 0.1 && b.U[2] > 0.99, "after 360 degrees the hull is upright again");
        check(rate >= 3.0 - 1e-9 && Rate(0.02) == kMinRate, "roll rate 3 rad/s for a REV-8 hull, never under the floor for big hulls");
    }
    {
        // Inverted, level, camera aim 20 degrees above the horizon straight ahead: the body-frame error must turn the nose UP in
        // world terms (the stock world-pitch loop turned it down past 90 degrees of bank).
        Body b; float w[4];
        Compose(b.R, b.F, b.U, 0, 3.14159265358979, 0, w); Step(b, w, 1.0);
        check(b.U[2] < -0.99, "test setup: inverted");
        const auto e = AimError(b.R, b.F, b.U, AimDir(0.0, 0.35));
        Compose(b.R, b.F, b.U, 2.0 * e.pitch, 0, 0, w); Step(b, w, 0.05);
        check(b.F[2] > 0.0, "inverted: the pitch command lifts the nose toward an aim above the horizon");
        check(std::fabs(e.yaw) < 1e-3, "inverted: an aim straight ahead has no yaw error");
    }
    {
        // Banked 90 degrees right, aim 30 degrees to the right on the horizon: the turn is a pull (pitch), not rudder.
        Body b; float w[4];
        Compose(b.R, b.F, b.U, 0, 1.5707963267949, 0, w); Step(b, w, 1.0);
        const auto e = AimError(b.R, b.F, b.U, AimDir(0.52, 0.0));
        check(e.pitch > 0.4 && std::fabs(e.yaw) < 0.05, "banked 90 right: an aim to the right is a pull up (body pitch)");
        Compose(b.R, b.F, b.U, 2.0 * e.pitch, 0, 0, w); Step(b, w, 0.05);
        check(b.F[0] > 0.0, "banked 90 right: pulling turns the nose to the right");
        const auto behind = AimError(Body{}.R, Body{}.F, Body{}.U, AimDir(3.0, 0.0));
        check(std::fabs(behind.yaw) > 2.9, "an aim behind is a full-circle yaw error, not a pitch flip");
    }
    {
        // Released roll: hold, assist band, near-ground cap, landing.
        check(RollWant(2.0, 0.0, 3.0, 1.2, 0.0, false, false, false) == 0.0, "assist off: a released roll holds");
        check(RollWant(2.0, 0.0, 3.0, 1.2, 0.0, true, false, false) == 0.0, "assist on: a released roll past 60 degrees holds (stays inverted)");
        check(RollWant(0.5, 0.0, 3.0, 1.2, 0.0, true, false, false) < 0.0, "assist on: a released roll inside 60 degrees levels");
        check(RollWant(3.0, 0.0, 3.0, 1.2, 0.0, true, false, false) == 0.0, "assist on, inverted (172 degrees): no levelling");
        check(RollWant(1.3, 1.0, 3.0, 1.2, 0.0, true, true, false) == 0.0, "near the ground a held roll stops at about 70 degrees");
        check(RollWant(1.3, -1.0, 3.0, 1.2, 0.0, true, true, false) < 0.0, "near the ground the roll may still come back");
        check(RollWant(0.8, 1.0, 3.0, 1.2, 0.0, true, true, false) == 3.0, "near the ground a small roll is still free");
        check(RollWant(2.5, 0.0, 3.0, 1.2, 0.0, false, true, false) < 0.0, "near the ground a released roll levels, even with assist off");
        check(RollWant(-2.5, 1.0, 3.0, 1.2, 0.0, false, false, true) > 0.0, "landing always levels, whatever the key");
        check(RollWant(3.0, 0.0, 3.0, 5.0, 0.0, false, true, false) < 0.0 && RollWant(-3.0, 0.0, 3.0, 5.0, 0.0, false, true, false) > 0.0, "levelling from +-172 degrees goes the short way");
        check(RollToward(0.0, 2.094, 3.0, 3.0) == 3.0 && std::fabs(RollToward(2.094, 2.094, 3.0, 3.0)) < 1e-9, "a held bank target of 120 degrees is reached and held");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
