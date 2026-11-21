// TK-1 (2026-11-19): tracking quality -> speed cap, with numbers from the 21:26 snake logs (0.1 s ticks).
#include "tracking.h"
#include <cstdio>
#include <cmath>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static bool Eq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }
static tracking::Sample Straight(double t, double v) {
    tracking::Sample s; s.now = t; s.lateralErr = 0.05; s.headingErr = 0.01; s.yawRate = 0.02; s.rearSlipDeg = 1.0; s.speed = v; s.onWheels = true; return s;
}
static tracking::Sample Snake(double t, double v) {
    tracking::Sample s = Straight(t, v);
    const int half = static_cast<int>(std::floor(t / 0.5 + 1e-9));
    s.yawRate = (half % 2 == 0) ? 0.5 : -0.5;
    s.rearSlipDeg = 13.0 + 12.0 * (0.5 + 0.5 * std::sin(t * 3.0));  // 13..25 deg
    return s;
}
int main() { using namespace tracking;
    // 1. Steady straight driving at 14 m/s.
    {
        State st; bool allInf = true;
        for (int i = 0; i <= 100; ++i) { Out o = Update(st, Straight(i * 0.1, 14.0)); if (!std::isinf(o.cap)) allInf = false; }
        Check(allInf, "straight 14 m/s: cap stays +inf for 10 s");
    }
    // 2. The 21:26 snake: +/-0.5 rad/s every 0.5 s at 8 m/s, slip 13-25 deg.
    State snake; double capAt1 = 1e9; bool sawWeaving = false; double t = 0.0, snakeDrop = 0.0, snakePrev = snake.cap;
    for (int i = 0; i <= 30; ++i) {
        t = i * 0.1; Out o = Update(snake, Snake(t, 8.0));
        if (i == 10) capAt1 = o.cap;
        if (o.cap < snakePrev) snakeDrop = t; snakePrev = o.cap;
        if (o.weaveFlips >= 3 && o.score >= 0.5) sawWeaving = true;
    }
    Check(capAt1 < 6.0, "snake: cap below 6 m/s within 1 s");
    std::printf("     cap at 1.0 s = %.2f\n", capAt1);
    Check(sawWeaving, "snake: >= 3 yaw flips seen in the window and score >= 0.5");
    // 3. Recovery after the snake stops: hold dropHold, then rise <= 1 m/s per s, then +inf.
    {
        double lastDrop = snakeDrop, prevCap = snake.cap; bool riseOk = true, heldOk = true, released = false; double releasedAt = 0.0;
        for (int i = 31; i <= 400 && !released; ++i) {
            double now = i * 0.1; Out o = Update(snake, Straight(now, 8.0));
            if (o.cap < prevCap) lastDrop = now;
            else if (std::isfinite(o.cap) && o.cap > prevCap) {
                if (now - lastDrop < 1.0 - 1e-9) heldOk = false;
                if (o.cap - prevCap > 1.0 * 0.1 + 1e-9) riseOk = false;
            }
            if (std::isinf(o.cap)) { released = true; releasedAt = now; if (!Eq(o.why, "tracking_fine")) riseOk = false; }
            prevCap = o.cap;
        }
        Check(heldOk, "recovery: lowered cap held 1 s before rising");
        Check(riseOk, "recovery: cap rises at <= 1 m/s per s");
        Check(released, "recovery: cap returns to +inf (tracking_fine)");
        std::printf("     released at %.1f s (last snake sample 3.0 s)\n", releasedAt);
    }
    // 4. Back slip 20 deg alone at 10 m/s -> 10 * 8 / 20 = 4.0.
    {
        State st; Sample s = Straight(0.0, 10.0); s.yawRate = 0.0; s.rearSlipDeg = 20.0;
        Out o = Update(st, s);
        Check(std::fabs(o.cap - 4.0) < 1e-6 && Eq(o.why, "back_stepping_out"), "back slip 20 deg: cap = speed*8/20 = 4.0");
    }
    // 5. Hard turn 0.6 rad/s at 10 m/s (6 m/s^2 sideways) -> sqrt(PlanLat/0.06). D2: PlanLat is the one car record's planning sideways
    //    acceleration (carcap: 3.0 x the gear's grip; was a separate 3.5 here): sqrt(3.0/0.06) = 7.07 at grip 1, sqrt(4.8/0.06) = 8.94 at 1.6.
    {
        State st; Out o{};
        for (int i = 0; i <= 10; ++i) { Sample s = Straight(i * 0.1, 10.0); s.yawRate = 0.6; o = Update(st, s); }
        Check(std::fabs(o.cap - std::sqrt(carcap::kCar.PlanLat(1.0) / 0.06)) < 1e-6 && std::fabs(o.cap - std::sqrt(3.0 / 0.06)) < 1e-6 && Eq(o.why, "turn_too_hard"),
              "hard turn 0.6 rad/s at 10 m/s: cap = sqrt(3.0/0.06) = 7.07 (carcap PlanLat at grip 1; was 3.5 -> 7.64)");
        std::printf("     cap = %.3f\n", o.cap);
        State g1; for (int i = 0; i <= 10; ++i) { Sample s = Straight(i * 0.1, 10.0); s.yawRate = 0.6; s.grip = 1.6; o = Update(g1, s); }
        Check(std::fabs(o.cap - std::sqrt(4.8 / 0.06)) < 1e-6, "D2: in gear 1 (grip 1.6) the same turn caps at sqrt(4.8/0.06) = 8.94");
    }
    // 6. Airborne samples change nothing.
    {
        State st; Sample s = Straight(0.0, 10.0); s.rearSlipDeg = 20.0; Update(st, s);
        double capBefore = st.cap; std::size_t countBefore = st.count; bool same = true;
        for (int i = 1; i <= 30; ++i) {
            Sample a = Straight(i * 0.1, 10.0); a.onWheels = false; a.yawRate = (i % 2) ? 2.0 : -2.0; a.rearSlipDeg = 40.0;
            Out o = Update(st, a); if (o.cap != capBefore || o.changed) same = false;
        }
        Check(same && st.count == countBefore, "airborne: cap and history unchanged");
        Out o = Update(st, Straight(3.1, 10.0));
        Check(o.cap == capBefore, "airborne: hold timer did not run out in the air");
        Sample fresh = Straight(0.0, 10.0); fresh.onWheels = false; State st2; Out o2 = Update(st2, fresh);
        Check(std::isinf(o2.cap), "airborne from start: cap stays +inf");
    }
    // 7. minCap respected.
    {
        State st; bool ok = true;
        for (int i = 0; i <= 50; ++i) {
            Sample s = Snake(i * 0.1, 3.5); s.rearSlipDeg = 60.0; s.lateralErr = 3.0; s.headingErr = 1.0; s.yawRate *= 4.0;
            Out o = Update(st, s); if (o.cap < 3.0 - 1e-9) ok = false;
        }
        Check(ok && std::fabs(st.cap - 3.0) < 1e-9, "minCap: cap never below 3 m/s and pinned at 3");
    }
    // 8. Off the plan needs 0.5 s sustained.
    {
        State st; Out o{}; double firstCapT = -1.0;
        for (int i = 0; i <= 10; ++i) {
            Sample s = Straight(i * 0.1, 10.0); s.lateralErr = 0.8; o = Update(st, s);
            if (firstCapT < 0.0 && std::isfinite(o.cap)) firstCapT = i * 0.1;
        }
        Check(std::fabs(firstCapT - 0.5) < 1e-6 && std::fabs(o.cap - 8.0) < 1e-6 && Eq(o.why, "off_the_plan"), "off the plan 0.8 m: cap 8.0 after 0.5 s");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n"); return failed ? 1 : 0; }
