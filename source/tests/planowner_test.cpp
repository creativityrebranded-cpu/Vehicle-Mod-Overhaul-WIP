// Owner and rank record (planowner.h, step 5).
#include "planowner.h"
#include <cstdio>
using namespace planowner;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    Table t;
    auto c = t.Claim(0xFF001234, Owner::Traffic, trafficrank::Rank::Traffic, 3, 0, "traffic lane");
    Check(c.changed && c.to == Owner::Traffic && t.Of(0xFF001234, 0)->pilot == 3, "traffic claims a car");
    c = t.Claim(0xFF001234, Owner::Traffic, trafficrank::Rank::Traffic, 3, 1, "traffic lane");
    Check(!c.changed && !c.refused, "the same owner only refreshes");
    c = t.Claim(0xFF001234, Owner::Combat, trafficrank::Rank::Combat, 3, 1.5, "fighting");
    Check(c.changed && c.from == Owner::Traffic && t.Of(0xFF001234, 1.5)->rank == trafficrank::Rank::Combat, "combat takes it from traffic");
    c = t.Claim(0xFF001234, Owner::Traffic, trafficrank::Rank::Traffic, 3, 2, "traffic lane");
    Check(c.refused && t.Of(0xFF001234, 2)->owner == Owner::Combat, "traffic cannot take it back while combat holds it");
    c = t.Claim(0xFF001234, Owner::Player, trafficrank::Rank::Player, 0, 2.2, "you got in");
    Check(c.changed && t.Of(0xFF001234, 2.2)->owner == Owner::Player, "the player always wins");
    Check(t.Of(0xFF001234, 10) == nullptr, "a claim nobody refreshes lapses");
    c = t.Claim(0xFF001234, Owner::Traffic, trafficrank::Rank::Traffic, 3, 10, "traffic lane");
    Check(c.changed && !c.refused, "and a lapsed claim can be taken");
    Check(t.Of(0xFF001234, 10)->life == Life::None, "reclaimed record has no stale traffic state");
    Check(t.Traffic(0xFF001234, 7, 4, 9, Life::Proving, "waiting seat"), "Census sets metadata on a traffic claim");
    t.Claim(0xFF001234, Owner::Traffic, trafficrank::Rank::Traffic, 3, 10.1, "refresh");
    Check(t.Of(0xFF001234, 10.1)->driver == 7 && t.Of(0xFF001234, 10.1)->life == Life::Proving, "ordinary Claim refresh retains Census state");
    t.Claim(0xFF001234, Owner::Player, trafficrank::Rank::Player, 0, 10.2, "player");
    Check(t.Of(0xFF001234, 10.2)->life == Life::None && !t.Traffic(0xFF001234, 7, 4, 9, Life::Driving, "late census"), "stronger ownership clears metadata and refuses late Census writes");
    Check(!t.Traffic(888, 7, 4, 9, Life::Born, "missing") && t.Find(888) == nullptr, "metadata setter never creates a second claim");
    for (std::uint32_t v = 1; v <= kMax + 10; ++v) t.Claim(v, Owner::Job, trafficrank::Rank::Scripted, 1, 20 + v * 0.0001, "job");
    Check(t.count <= kMax, "the table never overflows");
    t.Release(5); Check(t.Of(5, 20) == nullptr, "release forgets a vehicle");
    {   // Census publishes only on transitions: a held pair's claim lapses before its pilot claims; traffic's own metadata survives that.
        Table u; u.Claim(0xFF00AAAA, Owner::Traffic, trafficrank::Rank::Parked, -1, 0, "spawn census");
        u.Traffic(0xFF00AAAA, 7, 2, 9, Life::Proving, "held at birth");
        c = u.Claim(0xFF00AAAA, Owner::Traffic, trafficrank::Rank::Traffic, 4, 30, "traffic lane");
        Check(c.changed && u.Of(0xFF00AAAA, 30)->life == Life::Proving && u.Of(0xFF00AAAA, 30)->slot == 2, "traffic re-claiming after a lapse keeps the Census state");
        u.Claim(0xFF00AAAA, Owner::Job, trafficrank::Rank::Scripted, 4, 60, "job");
        Check(u.Of(0xFF00AAAA, 60)->life == Life::None, "a different owner after a lapse starts clean");
    }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
