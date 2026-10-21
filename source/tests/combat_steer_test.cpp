#include "combat_steer.h"
#include <cstdio>
using namespace steer;
namespace {
// A flat world at z 0 with optional solid boxes and a raised plateau.
struct Box { V3 lo, hi; };
struct FakeWorld : World
{
    Box boxes[8]; int n = 0;
    double plateauX = 1e9, plateauH = 0;  // x >= plateauX: ground at plateauH
    bool Solid(const V3& p) const override
    {
        for (int i = 0; i < n; ++i) if (p.x >= boxes[i].lo.x && p.x <= boxes[i].hi.x && p.y >= boxes[i].lo.y && p.y <= boxes[i].hi.y && p.z >= boxes[i].lo.z && p.z <= boxes[i].hi.z) return true;
        return false;
    }
    double Ground(double x, double) const override { return x >= plateauX ? plateauH : 0.0; }
    bool Blocked(const V3& a, const V3& b) const override
    {
        for (int k = 1; k < 40; ++k) { const V3 p = a + (b - a) * (k / 40.0); if (Solid(p) || Ground(p.x, p.y) > p.z) return true; }
        return false;
    }
};
Agent At(double x, double y, double vx, double vy)
{
    Agent a; a.pos = { x, y, 3 }; a.vel = { vx, vy, 0 }; a.heading = HeadingOf(a.vel.x || a.vel.y ? a.vel : V3{ 0, 1, 0 }); return a;
}
}
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++failures; };
    FakeWorld w;
    {
        Agent a = At(0, 0, 0, 0);
        check(Seek(a, { 0, 100, 3 }).y > 0 && Flee(a, { 0, 100, 3 }).y < 0, "seek goes toward, flee away");
        check(Len(Flee(a, { 0, 100, 3 }, 50)) == 0, "flee ignores a threat beyond its panic distance");
        Agent b = At(0, 0, 0, 30);
        check(Arrive(b, { 0, 5, 3 }, 15).y < Seek(b, { 0, 5, 3 }).y, "arrive brakes inside its radius");
    }
    {
        Agent a = At(0, 0, 0, 20); Body t; t.pos = { 50, 100, 3 }; t.vel = { 20, 0, 0 };
        const V3 p = Pursuit(a, t), s = Seek(a, t.pos);
        check(HeadingOf(p + a.vel) > HeadingOf(s + a.vel), "pursuit leads a crossing target (steers ahead of it)");
        check(Dot(Evasion(a, t), t.pos - a.pos) < 0, "evasion goes away from where it will be");
        Body l; l.pos = { 0, 100, 3 }; l.heading = 0; l.vel = { 0, 10, 0 };
        const V3 slot = OffsetPoint(l, { 10, -20, 0 });
        check(std::fabs(slot.x - 10) < 1e-9 && std::fabs(slot.y - 80) < 1e-9, "offset point: 10 m right, 20 m behind the leader");
        check(OffsetPursuit(a, l, { 10, -20, 0 }).x > 0, "offset pursuit heads for the slot");
    }
    {
        Agent a = At(-50, 0, 0, 0); Body p, q; p.pos = { 0, 0, 3 }; q.pos = { 0, 100, 3 };
        const V3 f = Interpose(a, p, q);
        check(f.x > 0 && f.y > 0, "interpose heads for the point between the two");
    }
    {
        FakeWorld hw; hw.boxes[0] = { { -10, 25, -5 }, { 10, 30, 20 } }; hw.n = 1;  // a wall 25-30 m north
        Agent a = At(0, 0, 0, 0); Body threat; threat.pos = { 0, 120, 3 };
        bool found = false; V3 spot{};
        Hide(a, threat, hw, &found, &spot);
        check(found && spot.y < 25 && hw.Blocked(threat.pos + V3{ 0, 0, 1.5 }, spot + V3{ 0, 0, 1 }), "hide finds the far side of the wall");
        FakeWorld open;
        found = true; const V3 f = Hide(a, threat, open, &found);
        check(!found && f.y < 0, "nowhere to hide: evade");
    }
    {
        Agent a = At(0, 0, 0, 10); WanderState ws; Rng rng(7);
        V3 first = Wander(a, ws, rng, 0.1); bool moved = false;
        for (int i = 0; i < 20; ++i) { const V3 f = Wander(a, ws, rng, 0.1); moved = moved || Len(f - first) > 0.01; }
        check(moved && Len(first) > 0, "wander keeps changing and never stands still");
    }
    {
        FakeWorld ow; ow.boxes[0] = { { 0.5, 15, -5 }, { 6, 18, 20 } }; ow.n = 1;  // an obstacle ahead, a little to the right
        Agent a = At(0, 0, 0, 15); double hit = 0;
        const V3 f = ObstacleAvoidance(a, ow, &hit);
        check(hit < 30 && f.x < 0 && f.y < 0, "obstacle avoidance: away from the blocked side and braking");
        check(Len(ObstacleAvoidance(a, w)) == 0, "no obstacle: no force");
    }
    {
        FakeWorld tw; tw.plateauX = 5; tw.plateauH = 10;  // a cliff 5 m to the east
        Agent a = At(0, 0, 15, 0);
        const V3 f = WallAvoidance(a, tw);
        check(f.x < 0 && f.z > 0, "wall / terrain avoidance pushes back and up from rising terrain");
        Agent low = At(0, 0, 0, 0); low.pos.z = 0.5;
        check(WallAvoidance(low, w).z > 0, "the ground feeler lifts a hover that is too low");
    }
    {
        Agent a = At(140, 0, 0, 0);
        check(Containment(a, { 0, 0, 0 }, 150).x < 0 && Len(Containment(At(50, 0, 0, 0), { 0, 0, 0 }, 150)) == 0, "containment pulls back only near the edge");
    }
    {
        Agent a = At(0, 0, 0, 20); Body o; o.pos = { 1, 60, 3 }; o.vel = { 0, -20, 0 }; double when = 0;
        const V3 f = CollisionAvoidance(a, &o, 1, &when);
        check(when > 0 && when < 3 && f.x < 0, "collision avoidance: the earliest head-on collision is side-stepped");
        Body far; far.pos = { 100, 0, 3 }; far.vel = { 0, 20, 0 };
        check(Len(CollisionAvoidance(a, &far, 1, &when)) == 0 && when < 0, "no predicted collision: no force");
    }
    {
        Agent a = At(0, 0, 0, 10); Body m[2]; m[0].pos = { 5, 0, 3 }; m[0].vel = { 10, 0, 0 }; m[1].pos = { 30, 0, 3 }; m[1].vel = { 10, 0, 0 };
        check(Separation(a, m, 2).x < 0, "separation away from a close squad mate");
        check(Alignment(a, m, 2).x > 0, "alignment toward the mates' velocity");
        check(Cohesion(a, m, 2).x > 0, "cohesion toward the mates' centre");
        Body leader; leader.pos = { 0, -5, 3 }; leader.heading = 0; leader.vel = { 0, 10, 0 };
        check(std::fabs(LeaderFollowing(a, leader, { 0, -15, 0 }, nullptr, 0).x) > 1.0, "leader following steps out of the leader's path");
    }
    {
        const V3 path[3] = { { 0, 0, 3 }, { 0, 100, 3 }, { 100, 100, 3 } };
        Agent a = At(20, 50, 0, 10); int seg = -1;
        check(PathFollowing(a, path, 3, 5, &seg).x < 0 && seg == 0, "path following pulls back into the corridor");
        Agent on = At(0, 50, 0, 10);
        check(Len(PathFollowing(on, path, 3, 5)) < Len(PathFollowing(a, path, 3, 5)), "inside the corridor only a light pull");
    }
    {
        Combiner c(20.0);
        c.Add({ 15, 0, 0 }, 1.0);
        c.Add({ 0, 15, 0 }, 1.0);
        c.Add({ 0, 0, 50 }, 1.0);
        check(Len(c.total) <= 20.0 + 1e-6 && (c.used & 1u) && (c.used & 2u) && std::fabs(c.total.y - 5.0) < 1e-9, "the weighted sum is truncated at max force, in priority order");
        check(c.total.z < 5.0, "lower priorities get only what is left");
        Combiner full(20.0); const bool more = full.Add({ 30, 0, 0 }, 1.0); full.Add({ 0, 30, 0 }, 1.0);
        check(!more && full.total.y == 0.0 && !(full.used & 2u), "once the budget is used the rest are dropped this tick");
        Combiner d(20.0); d.Add({ 0, 0, 0 }, 1.0); d.Add({ 5, 0, 0 }, 2.0);
        check(std::fabs(d.total.x - 10) < 1e-9, "weights scale the forces");
    }
    {
        Agent a = At(0, 0, 0, 10);
        const V3 v = Integrate(a, { 0, 200, 30 }, 0.5);
        check(Len2D(v) <= a.maxSpeed + 1e-9 && v.z <= 8.0, "integration clamps to max speed and vertical rate");
        Smoother s; s.Apply({ 0, 10, 0 }, 0.05);
        const V3 o = s.Apply({ 10, 0, 0 }, 0.05);
        check(o.x > 0 && o.y > o.x, "the smoother turns gradually (no jitter)");
    }
    {
        Agent a = At(0, 0, 0, 10);
        const Commands c = ToCommands(a, { 20, 20, 2 });
        check(c.yawRate > 0 && c.throttle > 0 && c.vertical > 0 && std::isnan(c.pitch), "commands: turn right, speed up, climb");
        const V3 aim{ -50, 50, 13 };
        const Commands c2 = ToCommands(a, { 0, 20, 0 }, &aim);
        check(c2.yawRate < 0 && c2.pitch > 0, "an aim point turns the nose onto it (yaw and pitch)");
        check(ToCommands(a, { 0, 50, 0 }).gear == 4 && ToCommands(a, { 0, 10, 0 }).gear == 2, "the gear follows the wanted speed");
    }
    {
        FakeWorld tw; tw.plateauX = 30; tw.plateauH = 20;
        Agent a = At(0, 0, 20, 0); double at = 0;
        check(!SafeAhead(a, { 20, 0, 0 }, tw, 2.5, &at) && at > 1.0, "the safety check sees terrain within 2.5 s");
        check(SafeAhead(a, { 0, 20, 0 }, tw), "a clear way is safe");
    }
    return failures ? 1 : 0;
}
