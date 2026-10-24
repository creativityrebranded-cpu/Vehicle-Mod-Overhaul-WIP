#include "combat_bfm.h"
#include <cstdio>
using namespace bfm;
using steer::V3;
namespace {
Fighter F(double x, double y, double z, double vx, double vy, double heading = std::numeric_limits<double>::quiet_NaN())
{
    Fighter f; f.body.pos = { x, y, z }; f.body.vel = { vx, vy, 0 };
    f.body.heading = std::isnan(heading) ? steer::HeadingOf((vx || vy) ? V3{ vx, vy, 0 } : V3{ 0, 1, 0 }) : heading;
    return f;
}
}
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++failures; };
    Ctx c;
    {
        const Fighter me = F(0, 0, 10, 0, 20), it = F(0, 60, 10, 0, 15);  // right behind it
        const Geo g = Measure(me, it);
        check(Classify(g) == Situation::Offensive && g.aa < 0.1 && g.ata < 0.1, "behind it: offensive, aspect and ATA near 0");
        check(g.inMyZone && !g.inItsZone, "in my weapon zone, not in its");
        check(Pick(g, c, M::LeadPursuit, false) == M::PurePursuit, "a firing solution: pure pursuit");
    }
    {
        const Fighter me = F(0, 0, 10, 0, 20), it = F(0, 60, 10, 0, -20);  // head-on
        const Geo g = Measure(me, it);
        check(HeadOn(g) && Classify(g) == Situation::Neutral, "head-on: neutral");
        const M m = Pick(g, c, M::LeadPursuit, false);
        check(m == M::MergeOneCircle || m == M::MergeTwoCircle, "head-on and closing: the merge");
        Fighter slowTurner = it; slowTurner.turnRate = 0.5;
        check(Pick(Measure(me, slowTurner), c, M::LeadPursuit, false) == M::MergeOneCircle, "my smaller turn radius: one-circle");
        Fighter fastTurner = it; fastTurner.turnRate = 4.0;
        check(Pick(Measure(me, fastTurner), c, M::LeadPursuit, false) == M::MergeTwoCircle, "its smaller radius: two-circle");
    }
    {
        const Fighter me = F(0, 0, 10, 0, 20), it = F(0, -40, 10, 0, 20);  // it sits on my tail
        Ctx hit = c; hit.underFire = true;
        const Geo g = Measure(me, it);
        check(Classify(g) == Situation::Defensive && g.inItsZone, "it on my tail: defensive, in its zone");
        check(Pick(g, hit, M::LeadPursuit, false) == M::Break, "hit with it on my tail: break turn");
        Fighter high = me; high.body.pos.z = 30;
        check(Pick(Measure(high, it), c, M::LeadPursuit, false) == M::DefensiveSpiral, "on my tail and I have height: defensive spiral");
        Fighter fast = me; fast.body.vel = { 0, 45, 0 }; fast.boost = 1.0; Fighter it2 = it; it2.boost = 0.0;
        check(Pick(Measure(fast, it2), c, M::LeadPursuit, false) == M::Extend, "defensive with more energy: extend");
        check(BoostAllowed(M::Extend) && BoostAllowed(M::Break) && BoostAllowed(M::AttackRun) && !BoostAllowed(M::TurningFight), "boost only for extend, break, attack run");
    }
    {
        Ctx hit = c; hit.underFire = true;
        const Fighter me = F(0, 0, 10, 20, 0), it = F(0, 70, 10, 20, 0);  // side by side, hit, not defensive
        check(Pick(Measure(me, it), hit, M::LeadPursuit, false) == M::Jink, "under fire, not on my tail: jink");
    }
    {
        const Fighter me = F(0, 0, 10, 0, 35), it = F(20, 40, 10, 8, 15);  // behind it, closing fast, off its axis
        check(Pick(Measure(me, it), c, M::LeadPursuit, false) == M::HighYoYo, "closing fast behind it, off its axis: high yo-yo");
        const Fighter me2 = F(0, 0, 10, 0, 15), it2 = F(30, 80, 10, 5, 30);  // behind, it pulls away (no shot yet)
        check(Pick(Measure(me2, it2), c, M::LeadPursuit, false) == M::LowYoYo, "it opens the range: low yo-yo");
    }
    {
        const Fighter me = F(0, 0, 30, 0, 30), it = F(0, 200, 10, 10, 0);  // far, above it
        check(Pick(Measure(me, it), c, M::LeadPursuit, false) == M::AttackRun, "far and higher: attack run (boom and zoom)");
    }
    {
        const Fighter me = F(0, 0, 10, 0, 10), it = F(25, 5, 10, 0, 10, 0.0);  // side by side, close, slow
        const M m = Pick(Measure(me, it), c, M::LeadPursuit, false);
        check(m == M::FlatScissors, "close, side-on, slow: flat scissors");
        Fighter quick = me; quick.body.vel = { 0, 25, 0 };
        check(Pick(Measure(quick, it), c, M::LeadPursuit, false) == M::RollingScissors, "the same faster: rolling scissors");
    }
    {
        Ctx was = c; was.wasDefensive = true;
        const Fighter me = F(0, 0, 10, 0, 10), it = F(0, 30, 10, 0, 25);  // it overshot and is now in front, moving away
        check(Pick(Measure(me, it), was, M::LeadPursuit, false) == M::Reversal, "it overshot after I was defensive: reversal");
    }
    {
        const Fighter me = F(0, 0, 10, 0, 20), it = F(60, 0, 10, 0, 20);  // side by side at 60 m
        const Geo g = Measure(me, it);
        check(Pick(g, c, M::LeadPursuit, false) == M::TurningFight, "side by side, neutral: turning fight");
        double a = 0, b = 0;
        Pick(g, c, M::TurningFight, true, 0.15, 0xFFFFFFFFu, &a); Pick(g, c, M::TurningFight, false, 0.15, 0xFFFFFFFFu, &b);
        check(a > b, "the current manoeuvre gets a hysteresis bonus");
        check(Pick(g, c, M::LeadPursuit, false, 0.15, ~(1u << int(M::TurningFight))) != M::TurningFight, "a disabled manoeuvre is never picked");
    }
    {
        Fighter me = F(0, 0, 10, 0, 20), it = F(0, 60, 10, 0, 15); const Geo g = Measure(me, it);
        MState s; Enter(s, M::LagPursuit, me, it, 1000);
        const Recipe r = Plan(s, me, it, g, 1100, 0.1);
        check(r.n >= 1 && r.req[0].kind == Request::Kind::Offset && r.req[0].point.y < 0, "lag pursuit sits behind its tail (offset pursuit)");
        MState hy; Enter(hy, M::HighYoYo, me, it, 0);
        check(Plan(hy, me, it, g, 500, 0.1).vertical > 0 && Plan(hy, me, it, g, 2000, 0.1).vertical < 0, "high yo-yo: up first, then down behind it");
        MState lr; Enter(lr, M::LagRoll, me, it, 0); const Recipe r1 = Plan(lr, me, it, g, 100, 0.5); const Recipe r2 = Plan(lr, me, it, g, 600, 0.5);
        check(r1.req[0].point.x != r2.req[0].point.x, "lag roll: the offset rolls around its track");
        MState br; Enter(br, M::Break, me, it, 0); const Recipe b = Plan(br, me, it, g, 100, 0.1);
        check(b.boost && !std::isnan(b.vertical), "break: boost and a height change");
        MState jk; Enter(jk, M::Jink, me, it, 0);
        double lats[6]; for (int i = 0; i < 6; ++i) { Plan(jk, me, it, g, std::uint64_t(1000 * (i + 1)), 0.1); lats[i] = jk.lat; }
        bool varied = false; for (int i = 1; i < 6; ++i) varied = varied || lats[i] != lats[i - 1];
        check(varied, "jink re-rolls its direction (no repeating pattern)");
        MState ar; Fighter low = F(0, 0, 10, 0, 30); Fighter far = F(0, 200, 10, 0, 0); Enter(ar, M::AttackRun, low, far, 0);
        const Recipe a0 = Plan(ar, low, far, Measure(low, far), 100, 0.1);
        check(ar.phase == 0 && a0.vertical > 0 && a0.boost, "attack run: climbs to the perch first, boost allowed");
        Fighter close = F(0, 120, 10, 0, 30);
        Plan(ar, close, far, Measure(close, far), 200, 0.1);
        check(ar.phase == 1, "within reach: the dive");
        MState mg; const Fighter h1 = F(0, 0, 10, 0, 20), h2 = F(0, 100, 10, 0, -20); Enter(mg, M::MergeTwoCircle, h1, h2, 0);
        Plan(mg, h1, h2, Measure(h1, h2), 100, 0.1);
        check(mg.phase == 0, "merge: lining up the pass first");
        const Fighter h3 = F(0, 40, 10, 0, -20);
        Plan(mg, h1, h3, Measure(h1, h3), 200, 0.1);
        check(mg.phase == 1, "inside the lead-turn range: the turn starts");
    }
    {
        const Fighter me = F(0, 0, 10, 0, 20), it = F(0, 60, 10, 0, 15); const Geo g = Measure(me, it);
        const V3 ally[1] = { { 0.5, 30, 10 } };
        check(FireSolution(me, g, nullptr, 0) && !FireSolution(me, g, ally, 1), "no firing with an ally in the line");
        const Fighter far = F(0, 300, 10, 0, 15);
        check(!FireSolution(me, Measure(me, far), nullptr, 0), "out of range: no firing");
        const Fighter it2 = F(0, 60, 10, 30, 0);
        const Geo g2 = Measure(me, it2);
        check(g2.lead.x > 0, "the lead point is ahead of a crossing target");
    }
    {
        Fighter a = F(0, 0, 10, 0, 30), b = F(0, 100, 10, 0, 10); a.boost = b.boost = 0;
        const Geo g = Measure(a, b);
        check(g.myEnergy > g.itsEnergy, "energy: speed counts");
        Fighter hi = F(0, 0, 40, 0, 10); hi.boost = 0; Fighter lo = F(0, 0, 10, 0, 10); lo.boost = 0;
        check(Measure(hi, b).myEnergy > Measure(lo, b).myEnergy, "energy: height counts");
        Fighter slow = F(0, 0, 10, 0, 5); slow.boost = 0; Fighter fastIt = F(60, 0, 10, 0, 35);
        check(Desire(M::TurningFight, Measure(slow, fastIt), c) < 0.3, "low on energy: avoid the turning fight");
    }
    {
        Body ally;ally.pos={5,30,10};ally.radius=6;
        check(AllyBodiesInLine({0,0,10},{0,60,10},&ally,1),"a wide allied hull blocks a shot even when its centre is outside the old fixed width");
        ally.pos={0,30,20};ally.radius=2;
        check(!AllyBodiesInLine({0,0,10},{0,60,10},&ally,1),"an ally well above the actual firing segment does not block it");
        ally.pos={0,1,10};
        check(AllyBodiesInLine({0,0,10},{0,60,10},&ally,1),"an ally overlapping the start of the shot still blocks fire");
    }
    return failures ? 1 : 0;
}
