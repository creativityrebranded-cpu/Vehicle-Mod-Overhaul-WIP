#include "combat_tactics.h"
#include <cstdio>
using namespace tactics;
using steer::V3;
namespace {
struct Box { V3 lo, hi; };
struct FakeWorld : steer::World
{
    Box boxes[8]; int n = 0;
    double plateauX = 1e9, plateauH = 0;  // x >= plateauX: ground at plateauH (a cliff up, or a drop when negative)
    double hillX = 1e9, hillH = 0, hillR = 0, hillY = 0;  // a round hill
    bool Solid(const V3& p) const override
    {
        for (int i = 0; i < n; ++i) if (p.x >= boxes[i].lo.x && p.x <= boxes[i].hi.x && p.y >= boxes[i].lo.y && p.y <= boxes[i].hi.y && p.z >= boxes[i].lo.z && p.z <= boxes[i].hi.z) return true;
        return false;
    }
    double Ground(double x, double y) const override
    {
        if (std::hypot(x - hillX, y - hillY) < hillR) return hillH;
        return x >= plateauX ? plateauH : 0.0;
    }
    bool Blocked(const V3& a, const V3& b) const override
    {
        for (int k = 1; k < 40; ++k) { const V3 p = a + (b - a) * (k / 40.0); if (Solid(p) || Ground(p.x, p.y) > p.z) return true; }
        return false;
    }
};
Unit U(double x, double y, double vx, double vy, double heading = std::numeric_limits<double>::quiet_NaN())
{
    Unit u; u.body.pos = { x, y, 3 }; u.body.vel = { vx, vy, 0 };
    u.body.heading = std::isnan(heading) ? steer::HeadingOf((vx || vy) ? V3{ vx, vy, 0 } : V3{ 0, 1, 0 }) : heading;
    return u;
}
bfm::Fighter Fi(const Unit& u) { bfm::Fighter f; f.body = u.body; f.maxSpeed = u.maxSpeed; f.health = u.health; return f; }
bfm::Geo G(const Unit& a, const Unit& b) { return bfm::Measure(Fi(a), Fi(b)); }
}
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++failures; };
    FakeWorld open;
    Ctx c;
    {
        const Unit it = U(0, 0, 0, 10, 0.0);
        check(ClassifyRam(U(-20, 0, 20, 0), it) == RamKind::TBone, "into its side: T-bone");
        check(ClassifyRam(U(0, 30, 0, -20), it) == RamKind::HeadOn, "against its nose: head-on");
        check(ClassifyRam(U(0, -20, 0, 25), it) == RamKind::Rear, "from behind: rear ram");
        check(ClassifyRam(U(-10, -6, 20, 8), it) == RamKind::PIT, "its rear quarter at an angle: PIT");
    }
    {
        const RamResult slow = RamDamage(3.0, RamKind::TBone, 1500, 1500);
        check(slow.damageIt == 0 && slow.damageMe == 0, "a touch under 4 m/s does nothing");
        const RamResult t = RamDamage(20.0, RamKind::TBone, 1500, 1500), h = RamDamage(20.0, RamKind::HeadOn, 1500, 1500), r = RamDamage(20.0, RamKind::Rear, 1500, 1500);
        check(t.damageIt > t.damageMe && t.damageMe > 0, "a T-bone hurts the target more, the rammer a little");
        check(std::fabs(h.damageIt - h.damageMe) < 1e-9, "head-on: both the same");
        check(r.damageMe < t.damageMe, "a rear ram costs the rammer least");
        const RamResult heavy = RamDamage(20.0, RamKind::HeadOn, 4500, 1500);
        check(heavy.damageIt > heavy.damageMe * 2.0, "the heavier vehicle wins");
        check(RamDamage(20.0, RamKind::PIT, 1500, 1500).stunItMs > t.stunItMs && t.pushIt > 0, "the rammed one is pushed and a PIT spins it longest");
        RamResult fast = RamDamage(40.0, RamKind::TBone, 1500, 1500);
        double was = 0;
        check(fast.damageIt > 300.0, "uncapped, a 40 m/s T-bone would wreck a 300-health vehicle in one hit");
        check(CapRam(fast, 300.0, 300.0, 0.35, &was) && std::fabs(fast.damageIt - 105.0) < 1e-9 && was > 300.0, "the cap holds one ram to 35 % of the target's max health");
        RamResult ho = RamDamage(50.0, RamKind::HeadOn, 1500, 1500);
        CapRam(ho, 300.0, 300.0, 0.35);
        check(ho.damageIt <= 105.0 + 1e-9 && ho.damageMe <= 105.0 + 1e-9, "both sides are capped (head-on at 50 m/s)");
        RamResult big = RamDamage(40.0, RamKind::TBone, 1500, 1500);
        CapRam(big, 300.0, 1000.0, 0.35);
        check(std::fabs(big.damageIt - 350.0) < 1e-9, "the cap scales with that vehicle's own max health");
        RamResult small = RamDamage(10.0, RamKind::TBone, 1500, 1500);
        const double before = small.damageIt;
        check(!CapRam(small, 300.0, 300.0, 0.35) && small.damageIt == before, "a small ram is left alone");
    }
    {
        FakeWorld w; w.boxes[0] = { { 8, -20, -5 }, { 12, 20, 20 } }; w.n = 1;  // a wall 8 m east of it
        const Unit it = U(0, 0, 0, 2, 0.0), me = U(-25, 0, 10, 0);
        const Facts f = Read(w, me, it, nullptr, 0);
        check(f.pinSurface && f.pinDir.x > 0.8 && f.pinDist < 12, "a wall next to it is a pin surface");
        check(Pick(me, it, G(me, it), f, c) != T::Pin && Desire(T::Pin, me, it, G(me, it), f, c) == 0, "a slow target by a wall: no pin (contact moves removed)");
        TState s; Enter(s, T::Pin, me, it, f, 0);
        Plan p = Steer(s, me, it, G(me, it), f, 100);
        check(p.contact && p.r.req[0].point.x < 0, "pin: first round to the far side of it from the wall");
        const Unit me2 = U(-8, 0, 5, 0);
        p = Steer(s, me2, it, G(me2, it), f, 200);
        check(s.contactMade && p.r.req[0].point.x > 0 && p.r.speed < 10, "then press it into the wall and hold");
    }
    {
        FakeWorld w; w.plateauX = 9; w.plateauH = -20;  // a drop on its right
        const Unit it = U(0, 0, 0, 10, 0.0), me = U(-5, 0, 0, 18, 0.0);
        const Facts f = Read(w, me, it, nullptr, 0);
        check(f.hazard && f.hazardSide == 1 && f.edge, "a drop on its right: a sideswipe hazard and an edge");
        TState s; Enter(s, T::Sideswipe, me, it, f, 0);
        check(s.side == -1, "the sideswipe comes from the side away from the drop");
        const Unit beside = U(-3, 0, 0, 16, 0.0);
        const Plan p = Steer(s, beside, it, G(beside, it), f, 100);
        check(p.strafe > 0, "alongside: the push goes toward the drop");
    }
    {
        const Unit it = U(0, 0, 0, 10, 0.0), me = U(-20, 0, 18, 0);
        // Rams and contact moves were removed (2026-10-25): never wanted, whatever the geometry.
        check(Desire(T::Sideswipe, me, it, G(me, it), Facts{}, c) == 0, "side-on, close, faster: no sideswipe (removed)");
        const Unit tb = U(-40, 0, 25, 0);
        check(Desire(T::TBone, tb, it, G(tb, it), Facts{}, c) == 0, "T-bone geometry: no ram (removed)");
        const Unit pit = U(-6, -12, 3, 11);
        check(Desire(T::PIT, pit, it, G(pit, it), Facts{}, c) == 0, "behind its quarter: no PIT (removed)");
        TState s; Enter(s, T::PIT, pit, it, Facts{}, 0);
        const Plan p = Steer(s, pit, it, G(pit, it), Facts{}, 50);
        check(p.r.req[0].point.y < it.body.pos.y + it.body.vel.y * 0.3 && p.r.req[0].point.x < 0, "PIT aims at its rear quarter (on my side)");
        Unit weak = it; weak.health = 0.3; const Unit ho = U(0, 50, 0, -20);
        check(Desire(T::HeadOnRam, ho, weak, G(ho, weak), Facts{}, c) == 0 && Desire(T::HeadOnRam, ho, it, G(ho, it), Facts{}, c) == 0, "no head-on ram, even when much stronger (removed)");
    }
    {
        FakeWorld w; w.boxes[0] = { { -20, 55, -5 }, { -6, 65, 20 } }; w.boxes[1] = { { 6, 55, -5 }, { 20, 65, 20 } }; w.n = 2;  // a gap at y 60
        const Unit it = U(0, 0, 0, 12, 0.0), me = U(60, 40, 0, 0);
        const Facts f = Read(w, me, it, nullptr, 0);
        check(f.intercept && f.interceptPoint.y > 0, "an intercept point on its path ahead of it");
        check(f.choke, "the gap on its path is a chokepoint");
        check(Desire(T::Block, me, it, G(me, it), f, c) > 0.7, "a chokepoint I reach first: block");
        TState s; Enter(s, T::CutOff, me, it, f, 0);
        check(Steer(s, me, it, G(me, it), f, 10).r.req[0].kind == bfm::Request::Kind::Seek, "cut-off flies to the intercept point, not its tail");
    }
    {
        for (int slot = 0; slot < 4; ++slot) {
            const V3 o = BoxSlot(slot, 0), sq = BoxSlot(slot, 5);
            if (slot == 1 || slot == 2) check(std::fabs(sq.x) < std::fabs(o.x), "box-in flanks squeeze after 3 s");
        }
        check(BoxSlot(0, 0).y > 0 && BoxSlot(3, 0).y < 0, "box-in: one ahead, one behind");
        Ctx three = c; three.alliesOnTarget = 3; const Unit it = U(0, 0, 0, 12), me = U(-20, -20, 10, 10);
        check(Desire(T::BoxIn, me, it, G(me, it), Facts{}, three) >= 0.8, "three on a moving target: box in");
        TState s; Enter(s, T::BoxIn, me, it, Facts{}, 0, 0);
        check(Steer(s, me, it, G(me, it), Facts{}, 10).r.speed < 12, "the slot ahead slows it");
    }
    {
        Unit units[3] = { U(0, 50, 0, 10), U(10, 80, 0, 10), U(-10, 30, 0, 10) };
        units[0].cargo = true; units[1].escort = true;
        check(ConvoyTarget(U(0, 0, 0, 0), units, 3) == 1, "convoy attack: the escort first");
        units[1].escort = false;
        check(ConvoyTarget(U(0, 0, 0, 0), units, 3) == 0, "then the cargo vehicle");
        steer::Body d[3]; d[0].pos = { 0, 0, 0 }; d[1].pos = { 50, 0, 0 }; d[2].pos = { 100, 0, 0 };
        check(InterceptPair(d, 3, 2, { 120, 0, 0 }) && InterceptPair(d, 3, 1, { 120, 0, 0 }) && !InterceptPair(d, 3, 0, { 120, 0, 0 }), "the two nearest defenders break off to intercept");
        check(ConvoySlot(0).x < 0 && ConvoySlot(1).x > 0 && ConvoySlot(2).y < 0, "convoy slots: left, right, behind the leader");
    }
    {
        FakeWorld w; w.hillX = 0; w.hillY = 40; w.hillR = 15; w.hillH = 12;  // a hill 40 m north of me
        const Unit me = U(0, 0, 0, 0), it = U(0, 140, 0, 0);
        const Facts f = Read(w, me, it, nullptr, 0);
        check(f.highGround && f.highSpot.y > 20, "high ground near me that still sees it");
        check(Desire(T::HighGround, me, it, G(me, it), f, c) > 0, "terrain: take the high ground");
    }
    {
        FakeWorld w; w.boxes[0] = { { -10, 20, -5 }, { 10, 24, 6 } }; w.n = 1;  // a low wall between us
        const Unit me = U(0, 0, 0, 10), it = U(0, 50, 0, 0);
        const Facts f = Read(w, me, it, nullptr, 0);
        check(f.passOver && f.passHeight >= 6, "blocked low, clear higher: pass over");
        TState s; Enter(s, T::PassOver, me, it, f, 0);
        check(Steer(s, me, it, G(me, it), f, 10).r.vertical > 0, "pass over climbs");
    }
    {
        Unit me = U(0, 0, 0, 0); me.body.pos.z = 6; const Unit it = U(2, 0, 0, 0);
        check(Desire(T::PushDown, me, it, G(me, it), Facts{}, c) == 0, "a hover just above it: no push-down (contact moves removed)");
        TState s; Enter(s, T::PushDown, me, it, Facts{}, 0);
        check(Steer(s, me, it, G(me, it), Facts{}, 10).r.vertical < 0, "push-down goes down onto it");
    }
    {
        FakeWorld w; w.boxes[0] = { { 2, -20, -5 }, { 4, 20, 20 } }; w.n = 1;  // a wall 2-4 m on my right
        const Unit me = U(0, 0, 0, 5, 0.0);
        const Unit enemy = U(-15, 0, 10, 0);  // closing from my left
        const Facts f = Read(w, me, U(0, 100, 0, 0), &enemy, 1);
        check(f.pinnedMe && f.escapeDir.y > 0, "never pinned: against a wall with an enemy closing, escape along the wall");
    }
    return failures ? 1 : 0;
}
