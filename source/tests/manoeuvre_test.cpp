#include "manoeuvre.h"
#include <cstdio>
using namespace manoeuvre;
namespace {
// Me at the origin heading north at 10 m/s; the enemy `range` m north, facing north (away from me) at `foeSpeed`.
Input Base(double range, double foeSpeed = 0)
{
    Input in;
    in.me = Body{ 0, 0, 10, 0, 10, 0 };
    in.foe = Body{ 0, range, 10, 0, foeSpeed, 0 };
    in.foeInVehicle = true;
    in.now = 100000;
    return in;
}
State Run(Input& in, Mind& m, const Settings& s) { const State w = Choose(in, m, s); Enter(m, w, in); return m.state; }
}
int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++failures; };
    Settings s;
    // ---- Stage 1 ----
    {
        Input in = Base(200); in.me.heading = 1.6;  // far, not facing it
        Mind m;
        check(Run(in, m, s) == State::CloseIn, "far and not facing: close in");
        const Plan p = Steer(in, m, s);
        check(std::hypot(p.x, p.y) >= kCarrotMin - 1e-6 && p.y > 0 && p.speed >= kMinSpeed, "close in: a far carrot toward it, at combat speed");
    }
    {
        Input in = Base(120); in.foe.heading = kPi;  // facing each other, 120 m
        Mind m;
        const char* why = nullptr;
        const State w = Choose(in, m, s, &why);
        Enter(m, w, in);
        check(w == State::AttackRun && why, "facing it beyond 80 m: attack run (with a reason for the log)");
        Plan p = Steer(in, m, s);
        check(p.aim && p.speed >= kFastSpeed - 1e-6 && std::fabs(p.x) > 0.5, "the attack run aims in the fire band, fast, on a line beside it");
        in.foe.y = 50; in.now += 2000; Run(in, m, s);
        in.foe.y = 25; in.now += 500;
        check(Run(in, m, s) == State::Extend, "inside 30 m the pass is done: extend");
        check(m.extendOut >= 150 && m.extendOut <= 250 && m.reengage, "extend 150-250 m after an attack run, then re-engage");
        in.foe.y = -(m.extendOut + 5); in.now += 3000;
        check(Run(in, m, s) == State::AttackRun, "far enough out: back with an attack run");
    }
    {
        Input in = Base(55, 15);  // right behind a moving enemy
        Mind m;
        check(Run(in, m, s) == State::LagPursuit, "behind its tail: lag pursuit");
        const Plan p = Steer(in, m, s);
        check(p.aim && p.speed >= kMinSpeed && p.speed <= kFastSpeed, "lag pursuit: nose on it, combat speed");
    }
    {
        Input in = Base(60); in.hitRecently = true;
        Mind m;
        check(Run(in, m, s) == State::Break, "hit: break turn");
        const Plan p = Steer(in, m, s);
        check(!std::isnan(p.vertical) && p.speed >= kFastSpeed - 1e-6, "the break turn changes height and boosts");
        in.hitRecently = false; in.now += 1000;
        check(Run(in, m, s) == State::Break, "the break runs its length");
        in.now += kBreakMs;
        check(Run(in, m, s) != State::Break, "then a new pick");
    }
    {
        Input in = Base(0); in.foe = Body{ 0, -50, 10, 0, 0, 0 };  // behind me, its nose on me
        Mind m;
        check(Run(in, m, s) == State::Break, "the enemy behind me, pointing at me: break turn");
        in.now += kBreakMs + 10; Run(in, m, s);
        in.now += 100;
        check(Run(in, m, s) != State::Break, "not straight back into another break");
    }
    {
        Input in = Base(10); Mind m;
        check(Run(in, m, s) == State::Extend, "too close: extend");
        check(Steer(in, m, s).y < 0, "extend goes away from it");
    }
    {
        Input in = Base(60); in.foe.heading = 1.6; Settings s1 = s; s1.stage = 1;  // side-on at 60 m, no circle in stage 1
        Mind m;
        check(Run(in, m, s1) == State::Extend, "stage 1, close and side-on: out and round again (no circle yet)");
        check(Run(in, m, s) == State::Extend && true, "(the extend keeps running)");
    }
    {
        Input in = Base(60); in.others = 1; in.otherX[0] = 6; in.otherY[0] = 0;  // a friend 6 m to my right
        Mind m; in.foe.heading = kPi; in.foe.y = 120; Run(in, m, s);
        const Plan p = Steer(in, m, s);
        check(p.x < 0, "separation: a vehicle inside 15 m pushes the carrot away");
    }
    {
        Input in = Base(200); in.me.x = 140; in.foe.x = 320; in.me.heading = 1.6; Mind m;
        Run(in, m, s);
        const Plan p = Steer(in, m, s);
        check(std::hypot(p.x, p.y) < in.me.x + kCarrotMin, "a carrot outside the zone bends back toward the anchor");
    }
    {
        Input in = Base(200); in.me.vy = 40; in.me.heading = 1.6; Mind m; Run(in, m, s);
        check(std::hypot(Steer(in, m, s).x, Steer(in, m, s).y) >= kCarrotTime * 40 - 1e-6, "the carrot stays seconds ahead at speed (no arrive braking)");
    }
    // ---- Stage 2 ----
    {
        Input in = Base(60); in.foe.heading = 1.6; Mind m;
        check(Run(in, m, s) == State::Circle, "stage 2, close and side-on: circle");
        Plan p = Steer(in, m, s);
        check(p.aim && !std::isnan(p.strafe) && m.radius >= kCircleMin && m.radius <= kCircleMax, "the circle keeps the nose on it at 40-80 m");
        const int side = m.side;
        in.now = m.nextFlip; Steer(in, m, s);
        check(m.side == -side, "the circle changes direction after 3-6 s");
    }
    {
        Input in = Base(0); in.foe = Body{ 0, -20, 10, 0, 0, 1.0 }; in.me.vy = 15;  // just passed it, opening
        Mind m;
        check(Run(in, m, s) == State::Scissors, "overshoot at close range: scissors");
        const Plan p = Steer(in, m, s);
        check(p.speed < kMinSpeed, "scissors slows down to make it overshoot");
    }
    {
        Input in = Base(60); in.foe.heading = 1.6; in.myHealth = 0.5; in.foeHealth = 0.9; Mind m;
        check(Run(in, m, s) == State::Extend && m.extendOut >= 200, "losing: extend 200-300 m and re-engage");
        Input in2 = Base(60); in2.foe.heading = 1.6; in2.enemiesOnMe = 2; Mind m2;
        check(Run(in2, m2, s) == State::Extend, "two enemies on me: extend");
    }
    // ---- Stage 3 ----
    {
        Input in = Base(60, 15); in.foe.heading = 1.6; in.sameSide = 3;
        Mind a, b, c;
        in.rank = 0; const State s0 = Run(in, a, s);
        in.rank = 1; const State s1 = Run(in, b, s);
        in.rank = 2; const State s2 = Run(in, c, s);
        check(s0 == State::Circle && s1 == State::LagPursuit && s2 == State::Overwatch, "three on one: engage, pincer to the tail, overwatch");
        const Plan p = Steer(in, c, s);
        check(p.hold && p.z > in.foe.z + 20, "overwatch holds a high spot");
        Settings s2set = s; s2set.stage = 2; Mind d; in.rank = 2;
        check(Run(in, d, s2set) != State::Overwatch, "roles only from stage 3");
    }
    {
        Input in = Base(60); in.myHealth = 0.2; Mind m;
        check(Run(in, m, s) == State::BreakOff, "low health: break off");
        in.myHealth = 0.4; in.now += 5000;
        check(Run(in, m, s) == State::BreakOff, "break off holds until healed past its end mark");
        in.myHealth = 0.6; in.now += 100;
        check(Run(in, m, s) != State::BreakOff, "healed: back in");
    }
    // ---- Stage 4 ----
    {
        Input in = Base(40); in.me.vy = 15; Mind m;
        // Rams were removed (2026-10-25): a slow enemy straight ahead is fought, never rammed, at any stage.
        check(Run(in, m, s) != State::Ram && !Steer(in, m, s).ram, "a slow enemy straight ahead: no ram (removed)");
        for (int k = 0; k < 20; ++k) { in.now += 500; check(Run(in, m, s) != State::Ram, "no ram over time"); }
    }
    {
        Input in = Base(20, 3); in.foe.heading = 1.57; in.me.vy = 20; in.me.heading = 0.6;  // side-on, close, me faster, not straight at it
        Mind m;
        check(Run(in, m, s) != State::Sideswipe, "side-on at close range and faster: no sideswipe (removed)");
    }
    {
        Mind m; m.ex = 0; m.ey = -1; double c[5][2];
        check(HideCandidates(m, c) == 5 && std::fabs(c[0][1] + 1) < 1e-9, "hide candidates start with the planned extend direction");
        Hide(m, 3, 4);
        check(std::fabs(m.ex - 0.6) < 1e-9 && std::fabs(m.ey - 0.8) < 1e-9, "hide sets the extend direction");
    }
    return failures ? 1 : 0;
}
