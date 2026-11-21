// Strip build 2, cards CH-3/CH-5: the continuous health account (traffic/healthscore.h). Offline only.
#include "traffic/healthscore.h"
#include <cstdio>
#include <cstring>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
using namespace healthscore;

static Input Driving() { Input in; in.driving = true; in.sample = true; in.askedKnown = true; in.asked = 8; in.actual = 8; in.steps = true; in.halfWidth = 2.5; in.fieldShare = 1; in.upZ = 1; in.moved = 4; return in; }
// Run n judgements 1 s apart, `bad` deciding each one's input.
template <class F> static State Run(Account& a, unsigned long long& t, int n, F&& make) { State s = State::Healthy; for (int i = 0; i < n; ++i) { t += 1000; s = Step(a, make(i), t); } return s; }

int main()
{
    {   Account a; unsigned long long t = 1000;
        Check(Run(a, t, 60, [](int) { return Driving(); }) == State::Healthy, "a car driving well stays healthy");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        const auto s = Run(a, t, 1, [](int) { auto in = Driving(); in.actual = 0; return in; });
        Check(s == State::Healthy && a.score < 2, "one bad second never breaks a pair");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        const auto s = Run(a, t, 40, [](int i) { auto in = Driving(); if (i % 10 != 0) in.actual = 0; return in; });  // stuck 90% of the time
        Check(s == State::Broken, "stuck 90% of the time adds up to broken (one good frame no longer resets it)");
        Check(!std::strcmp(a.why, "asked_not_moving"), "and names why: asked_not_moving");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        const auto s = Run(a, t, 30, [](int) { auto in = Driving(); in.actual = 0; in.steps = false; in.play = false; return in; });
        Check(s == State::Healthy && a.score == 0, "paused play (menu, photo mode) adds nothing");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        const auto s = Run(a, t, 15, [](int) { auto in = Driving(); in.actual = 0; in.steps = false; return in; });
        Check(s == State::Broken && !std::strcmp(a.why, "no_physics_steps"), "its own no-steps is evidence, never a pause");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        Run(a, t, 6, [](int) { auto in = Driving(); in.actual = 0; return in; });
        Check(a.state == State::Doubtful, "doubtful before broken (Health asks for a spot test)");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        Run(a, t, 15, [](int) { auto in = Driving(); in.actual = 0; return in; });
        Check(a.state == State::Broken, "broken by score");
        Run(a, t, 1, [](int) { auto in = Driving(); in.moved = 0.0; in.asked = 0; in.actual = 0; return in; });
        Check(a.state == State::Broken, "one clean frame does not clear it");
        Run(a, t, 10, [](int) { return Driving(); });
        Check(a.state == State::Healthy, "real driving for 8 s and 8 m clears it");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        const auto s = Run(a, t, 2, [](int) { auto in = Driving(); in.lateral = 7.0; in.fieldShare = 0; return in; });
        Check(s == State::Broken && a.decisive == Kind::OffPath, "off its path (outside its field, 4.5 m past the lane) for 1 s: broken at once");
        Run(a, t, 30, [](int) { return Driving(); });
        Check(a.state == State::Broken, "a decisive fault stays broken (only Retirement ends it)");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        const auto s = Run(a, t, 5, [](int) { auto in = Driving(); in.lateral = 4.0; in.fieldShare = 0.6; return in; });
        Check(s != State::Broken || a.decisive == Kind::None, "a branch inside its field (Avoid's way round) is never off its path");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        Run(a, t, 3, [](int) { auto in = Driving(); in.onRail = true; in.floorKnown = true; in.floorGap = -6; return in; });
        Check(a.decisive == Kind::OffLevel, "6 m under its floor on the rail for 2 s: off its level");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        Run(a, t, 3, [](int) { auto in = Driving(); in.upZ = 0.1; return in; });
        Check(a.decisive == Kind::Tipped, "on its side for 2 s: broken");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { auto in = Driving(); in.fell = true; return in; });
        Check(a.decisive == Kind::Fell, "fell through the world: broken at once");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 1, [](int) { return Driving(); });
        Run(a, t, 10, [](int) { auto in = Driving(); in.lateral = 9; in.fieldShare = 0; in.play = false; return in; });
        Check(a.decisive == Kind::None, "decisive faults confirm only in play time");
    }
    {   Account a; unsigned long long t = 1000;
        Input in; in.driving = false; in.lateral = 30; in.sample = true; in.actual = 0; in.asked = 8; in.askedKnown = true;
        Run(a, t, 20, [&](int) { return in; });
        Check(a.state == State::Healthy, "a pair still being proved is Birth's: the account waits");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 30, [](int) { auto in = Driving(); in.noJob = true; return in; });
        Check(a.decisive == Kind::None, "no trip for 30 s: still waiting for a new trip");
        Run(a, t, 35, [](int) { auto in = Driving(); in.noJob = true; return in; });
        Check(a.decisive == Kind::NoJob, "no trip for 60 s of play: broken (no in-play restart fixes it any more)");
    }
    {   Account a; unsigned long long t = 1000;
        Run(a, t, 12, [](int) { auto in = Driving(); in.driverNoModel = true; return in; });
        Check(a.decisive == Kind::DriverNoModel, "seated driver with no model near the player for 10 s: broken");
    }
    {
        Account a;
        auto failedSpot=Driving();failedSpot.spotDead=true;
        Step(a,Driving(),1000);Step(a,failedSpot,2000);Step(a,failedSpot,3000);
        Check(a.decisive==Kind::None,"a failed spot result waits for two live seconds from first observation");
        Step(a,Driving(),4000);
        Check(a.decisive==Kind::None,"a newer pass supersedes a recent failed spot result");
        Step(a,failedSpot,5000);Step(a,failedSpot,6000);Step(a,failedSpot,7000);
        Check(a.decisive==Kind::SpotDead,"a continuing failed spot result becomes decisive");
    }
    std::printf("healthscore_test: %s (%d failed)\n", failed ? "FAILED" : "ALL PASS", failed);
    return failed ? 1 : 0;
}
