// WO-1 (2026-11-18): the gear handling: turning weight per gear, assist, traction, steering; cars sharing one data block.
#include "wheelgrip.h"
#include <cmath>
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
int main() {
    using namespace wheelgrip;
    {   Settings s; const auto g3 = ProfileOf(3, s);
        Check(g3.weight == 1.0 && g3.friction == 1.0 && g3.rear == 1.0 && g3.assist <= .5 && g3.slipTarget == 0.0, "GT-1 / CF-1: gear 3 is the game's own car (grip, rear, turning weight 1, no assist)");
        Check(ProfileOf(1, s).friction == 1.6 && ProfileOf(1, s).rear == 1.3 && ProfileOf(1, s).weight == 2.5, "GT-1: gear 1 has tyre grip x1.6, rear x1.3 more and turning weight x2.5 together");
        Check(PlanGrip(1, s) == 1.6 && PlanGrip(0, s) == 1.0, "GT-1: the plan counts on the gear's front grip");
        Check(ProfileOf(1, s).weight > ProfileOf(2, s).weight && ProfileOf(2, s).weight > 1.0 && ProfileOf(4, s).weight > 1.0, "WO-1: gear 1 heaviest to turn, then 2; gear 4 a little");
        Check(ProfileOf(9, s).weight == ProfileOf(4, s).weight && ProfileOf(0, s).weight == ProfileOf(1, s).weight, "WO-1: gears outside 1-4 clamp to 1-4");
        Check(std::fabs(RearSlipDeg(10, 0, 0, 1.6)) < 1e-9 && RearSlipDeg(10, 1.0, -.5, 1.6) > 10 && RearSlipDeg(1, 1, 0, 1.6) == 0, "RB-1: rear slip angle: straight 0, back stepping out > 10 deg, not at a crawl"); }
    {   // GK-1: the gear is one grip profile (friction, sideways assist, traction slip target).
        Settings s; const auto g1 = ProfileOf(1, s), g3 = ProfileOf(3, s);
        Check(g1.assist > g3.assist && g3.assist > 0 && g3.assist <= .5, "CF-1: every gear has corner forces; gear 1 the most, gear 3 negligible (0.5 m/s^2)");
        double fac = 1; for (int k = 0; k < 60; ++k) fac = Traction(fac, 3.0, .4, 1.0 / 60);
        Check(fac < .5 && fac >= kTcMin, "GK-1: wheels spinning 3 m/s in gear 1: the engine eased within a second");
        for (int k = 0; k < 120; ++k) fac = Traction(fac, 0, .4, 1.0 / 60);
        Check(fac == 1.0, "GK-1: grip back: full torque again");
        Check(EnginePlausible(1000, 3500, 6000, 400) && !EnginePlausible(0, 0, 0, 0) && !EnginePlausible(0, 0, 1000, std::nanf("")), "GK-3: torque decides: a real torque yes, none or not a number no");
        {   // ST-1: the autopilot's steering: capped by speed, rate limited.
            Steer st; const double L = 3.0, lock = .7854;
            auto a = SmoothSteer(st, .45, 1.5, L, lock, 1.0, 0.0);
            Check(std::fabs(a.steer - .45) < 1e-9 && a.cap == 1.0, "ST-1: at walking pace the full command goes through");
            Steer s2; auto b10 = SmoothSteer(s2, .9, 10.0, L, lock, 1.0, 0.0);
            Check(b10.capped && b10.cap > .2 && b10.cap < .35, "ST-1: at 10 m/s the wheel angle is capped near 0.3 of the lock");
            Steer s3; SmoothSteer(s3, .3, 6.0, L, lock, 1.0, 0.0); auto c = SmoothSteer(s3, -.4, 6.0, L, lock, 1.0, .5);
            Check(c.rateLimited && std::fabs(c.steer - (.3 - 1.3 * .5)) < 1e-9, "ST-1: a flip from +0.3 to -0.4 in half a second is turned gradually");
            Steer s4; auto g1 = SmoothSteer(s4, .9, 10.0, L, lock, 1.6, 0.0); Steer s5; auto g3 = SmoothSteer(s5, .9, 10.0, L, lock, 1.0, 0.0);
            Check(g1.cap > g3.cap, "ST-1: more grip (gear 1) allows a little more steering at the same speed");
        }
        Check(EnginePlausible(0, 0, 1000, 5500), "GK-2: the game's own engine (0 / 0 / 1000 rpm, 5500 torque) is accepted");
    }
    {   // YW-1: the REV-8 car (all factors 1, inertias 1), body axes = an axis permutation (yaw row picks component 2)
        Chassis c; c.ok = true; for (int k = 0; k < 3; ++k) { c.torque[k] = 1; c.inertia[k] = 1; }
        c.torque[1] = .2f; c.torque[2] = .2f;  // roll / pitch factors 0.2
        c.rows[0][2] = 1; c.rows[1][0] = 1; c.rows[2][1] = 1;
        const float invMass = 1.0f / 1500; c.inv[0] = .2f * invMass; c.inv[1] = .2f * invMass; c.inv[2] = 1.0f * invMass; c.inv[3] = invMass;
        float o[4]; Check(YawWeighted(c, 1.0, o) && std::fabs(o[2] - c.inv[2]) < 1e-12 && o[0] == c.inv[0], "YW-1: weight 1 writes the game's own value back");
        Check(YawWeighted(c, 2.5, o) && std::fabs(o[2] - c.inv[2] / 2.5) < 1e-9 && std::fabs(o[0] - c.inv[0]) < 1e-12 && std::fabs(o[1] - c.inv[1]) < 1e-12 && o[3] == invMass,
            "YW-1: weight 2.5 makes only the yaw component 2.5 x heavier; roll, pitch and mass untouched");
        Chassis bad = c; bad.inertia[0] = 0; Check(!YawWeighted(bad, 2.0, o), "YW-1: implausible numbers are never written");
        // Two AI cars (slots 3 and 5), no player's car: the highest ask, owner = the slot asking the most weight.
        Block bl; const auto w1 = Want(bl, 1, 3, { 1.6, 2.08, 2.5 }, 1.0), w2 = Want(bl, 2, 5, { 1.0, 1.0, 1.0 }, 1.1);
        Check(w1.rule == Rule::Own && w1.perInstance && w1.owner == 3 && !w1.held && w1.want.weight == 2.5, "GO-1: one car on its block owns it (per instance)");
        Check(w2.rule == Rule::Highest && !w2.perInstance && w2.owner == 3 && w2.held && w2.want.weight == 2.5 && w2.want.rear == 2.08 && w2.want.front == 1.6 && FreshUsers(bl, 1.2) == 2,
            "GT-1/GO-1: a shared block without the player's car keeps the highest grip and weight any fresh car asks");
        const auto w3 = Want(bl, 2, 5, { 1.0, 1.0, 1.0 }, 4.0);
        Check(w3.want.weight == 1.0 && w3.want.front == 1.0 && w3.want.rear == 1.0 && w3.rule == Rule::Own && w3.owner == 5 && FreshUsers(bl, 4.0) == 1 && bl.everShared,
            "GT-1: the gear-1 car gone 3 s: the block goes back to the remaining car's ask (the game's own)");
    }
    {   // GO-1: the player's car (slot 0) owns a shared block; AI cars of the same model do not change it while it is fresh.
        Block bl; const Ask playerG3{ 1.0, 1.0, 1.0 }, aiG1{ 1.6, 2.08, 2.5 };
        const auto p = Want(bl, 0xA, 0, playerG3, 1.0); const auto ai = Want(bl, 0xB, 7, aiG1, 1.01);
        Check(p.rule == Rule::Own && ai.rule == Rule::Player && ai.owner == 0 && ai.held && ai.want.weight == 1.0 && ai.want.front == 1.0,
            "GO-1: an AI car in gear 1 does not put the player's gear-3 car into gear-1 grip or turning weight");
        const auto p2 = Want(bl, 0xA, 0, { 1.25, 1.5, 1.75 }, 1.02);
        Check(p2.rule == Rule::Player && p2.owner == 0 && !p2.held && p2.want.front == 1.25 && p2.want.rear == 1.5 && p2.want.weight == 1.75,
            "GO-1: the player shifts to gear 2: the block follows the player, not the AI's heavier gear 1");
        const auto ai2 = Want(bl, 0xB, 7, aiG1, 3.5);
        Check(ai2.rule == Rule::Own && ai2.owner == 7 && ai2.want.weight == 2.5 && !ai2.held, "GO-1: the player's car gone (stale 2.5 s): the AI car's own ask applies again");
        Block order; Want(order, 0xB, 7, aiG1, 1.0); const auto late = Want(order, 0xA, 0, playerG3, 1.01);
        Check(late.rule == Rule::Player && late.want.weight == 1.0 && !late.held, "GO-1: arrival order does not matter: the player's car arriving second still owns the block");
        Block solo; const auto s1 = Want(solo, 0xC, -1, { 1, 1, 1 }, 1.0);
        Check(s1.rule == Rule::Own && s1.owner == -1 && s1.perInstance && s1.want.weight == 1.0, "GO-1: a car FVO does not fly keeps the game's own on its own block");
    }
    std::printf(failed ? "FAILED\n" : "ALL PASS\n");
    return failed ? 1 : 0;
}
