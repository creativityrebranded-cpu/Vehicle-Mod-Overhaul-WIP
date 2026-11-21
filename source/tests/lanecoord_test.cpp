// The coordinator's lane arbiter (lanecoord.h): gap acceptance, reservations, fixed blockages and deadlock yield.
#include "lanecoord.h"
#include <cstdio>
using namespace lanecoord;
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
static Ask Asking(int lane, bool sameWay, double speed) { Ask a; a.vehicle = 0xA; a.own = 1; a.lane = lane; a.sameWay = sameWay; a.speed = speed; a.halfLength = 2.3; return a; }
static Other Car(int lane, double ahead, double speed) { Other o; o.vehicle = 0xB; o.lane = lane; o.ahead = ahead; o.speed = speed; o.halfLength = 2.3; return o; }
int main() {
    {   Other none[1]{};
        Check(Decide(Asking(2, true, 10), none, 0).reason == Open, "an empty lane is open");
        auto own = Asking(1, true, 10); own.yielding = true; own.blockAhead = 5;
        Check(Decide(own, none, 0).reason == Open, "the car's own lane is never closed"); }
    {   Other beside[1]{ Car(2, 1.0, 10) };
        Check(Decide(Asking(2, true, 10), beside, 1).reason == Occupied, "a car alongside in the target lane: occupied");
        Other wrongLane[1]{ Car(0, 1.0, 10) };
        Check(Decide(Asking(2, true, 10), wrongLane, 1).reason == Open, "a car in another lane does not close this one");
        Other self[1]{ Car(2, 1.0, 10) }; self[0].vehicle = 0xA;
        Check(Decide(Asking(2, true, 10), self, 1).reason == Open, "a car never closes a lane to itself"); }
    {   // A follower coming up behind in the target lane.
        Other fast[1]{ Car(2, -20, 18) };   // 15.4 m bumper gap, closing at 8 m/s: needs 3 + 11.2 + 10.7
        const auto a = Decide(Asking(2, true, 10), fast, 1);
        Check(a.reason == Closing && a.by == 0xB, "a faster car closing from behind: the gap is refused, and named");
        Other same[1]{ Car(2, -20, 10) };
        Check(Decide(Asking(2, true, 10), same, 1).reason == Open, "the same car at our speed leaves the gap open");
        Other far[1]{ Car(2, -60, 18) };
        Check(Decide(Asking(2, true, 10), far, 1).reason == Open, "far enough behind, even closing, it is open"); }
    {   // A slower car ahead in the target lane.
        Other slow[1]{ Car(2, 12, 2) };     // 7.4 m gap, we close at 8 m/s: needs 3 + 11.2
        Check(Decide(Asking(2, true, 10), slow, 1).reason == Closing, "moving in right behind a much slower car is refused");
        Other away[1]{ Car(2, 12, 14) };
        Check(Decide(Asking(2, true, 10), away, 1).reason == Open, "behind a faster car there is room"); }
    {   // The oncoming lane: the target lane's direction runs against us, so traffic in it comes from "upstream" (negative ahead).
        Other coming[1]{ Car(0, -80, 12) };  // 75.4 m gap, closing 20 m/s over 6 s: needs 123 m
        Check(Decide(Asking(0, false, 8), coming, 1).reason == Closing, "an oncoming car inside the passing time closes the oncoming lane (D5)");
        Other distant[1]{ Car(0, -200, 12) };
        Check(Decide(Asking(0, false, 8), distant, 1).reason == Open, "an oncoming car far enough away leaves it open");
        Other gone[1]{ Car(0, 30, 12) };
        Check(Decide(Asking(0, false, 8), gone, 1).reason == Open, "an oncoming car already past us is moving away: open"); }
    {   // Hysteresis: separate on and off thresholds.
        Other edge[1]{ Car(2, -24, 15) };   // gap 19.4; closing 5: need 3 + 7 + 4.17 = 14.17 (open); with the margin 17.7 (still open)
        auto a = Asking(2, true, 10);
        Check(Decide(a, edge, 1).reason == Open, "a gap just over the need is open");
        Other tight[1]{ Car(2, -20, 15) };  // gap 15.4: open at 14.17, but not at 17.7
        Check(Decide(a, tight, 1).reason == Open, "the same gap, a little smaller, is still open to a car that had it open");
        a.wasClosed = true;
        Check(Decide(a, tight, 1).reason == Closing, "and stays closed to a car it was closed for, until there is more room"); }
    {   // Blockage and yield come first; a held reservation keeps the lane through gap changes.
        Other fast[1]{ Car(2, -20, 18) };
        auto a = Asking(2, true, 10); a.holdsReservation = true;
        Check(Decide(a, fast, 1).reason == Open, "a committed change keeps its lane while the follower closes (the follower is told instead)");
        a.blockAhead = 20;
        Check(Decide(a, fast, 1).reason == Blocked, "a fixed blockage just ahead in the target lane closes it even to a reservation");
        a.blockAhead = 80;
        Check(Decide(a, fast, 1).reason == Open, "a blockage far ahead does not");
        a.blockAhead = -3;
        Check(Decide(a, fast, 1).reason == Blocked, "nor may it merge onto the blockage itself");
        a.blockAhead = 1e18; a.yielding = true;
        Check(Decide(a, fast, 1).reason == Yield, "a car told to yield keeps to its own lane");
        auto b = Asking(2, true, 10); b.reservedBy = 0xC;
        const auto r = Decide(b, fast, 0);
        Check(r.reason == Reserved && r.by == 0xC, "another car's earlier reservation closes the lane, and names it"); }
    {   Reservations res;
        Check(res.Hold(0xA, 7, 2, 100, 2.3, 10.0), "a plan heading for a lane reserves its gap");
        Check(res.Before(0xB, 7, 2, 105, 2.3, 10.5) == 0xA, "a second car beside that gap sees the earlier reservation");
        Check(res.Before(0xA, 7, 2, 100, 2.3, 10.5) == 0, "the holder does not see its own");
        Check(res.Before(0xB, 7, 2, 200, 2.3, 10.5) == 0, "far along the road the lane is free");
        Check(res.Before(0xB, 7, 3, 105, 2.3, 10.5) == 0 && res.Before(0xB, 8, 2, 105, 2.3, 10.5) == 0, "another lane or another road is not reserved");
        res.Hold(0xB, 7, 2, 105, 2.3, 10.6);
        Check(res.Before(0xA, 7, 2, 100, 2.3, 10.7) == 0 && res.Before(0xB, 7, 2, 105, 2.3, 10.7) == 0xA, "first come keeps it: the later reservation waits");
        Check(res.Before(0xB, 7, 2, 105, 2.3, 10.0 + rules::reserveLapse + .7) == 0, "a reservation nobody renews lapses");
        Reservations tie; tie.Hold(0x20, 7, 2, 100, 2.3, 5.0); tie.Hold(0x10, 7, 2, 104, 2.3, 5.0);
        Check(tie.Before(0x20, 7, 2, 100, 2.3, 5.1) == 0x10 && tie.Before(0x10, 7, 2, 104, 2.3, 5.1) == 0, "the same moment goes to the lower id, so two cars never both wait");
        tie.Drop(0x10);
        Check(tie.Before(0x20, 7, 2, 100, 2.3, 5.1) == 0, "a dropped reservation frees the lane");
        Reservations full; bool all = true; for (int i = 0; i < kReservations; ++i) all = all && full.Hold(100 + i, 7, 2, i * 100.0, 2.3, 1.0);
        Check(all && !full.Hold(999, 7, 2, 5, 2.3, 1.1), "a full table refuses, it never overruns");
        Check(full.Hold(999, 7, 2, 5, 2.3, 1.0 + rules::reserveLapse + 1), "and takes new ones once old ones lapse"); }
    {   Blocks blocks;
        Check(blocks.Report(7, 1, 500, 0xA, 10.0), "a fixed blockage is new the first time");
        Check(!blocks.Report(7, 1, 503, 0xB, 10.5), "the same blockage reported again is refreshed, not doubled");
        Check(std::fabs(blocks.Ahead(7, 1, 480, +1, 0, 10.6) - 23) < 1e-9, "it is known ahead along the lane's travel");
        Check(std::fabs(blocks.Ahead(7, 1, 520, -1, 0, 10.6) - 17) < 1e-9, "and ahead for a lane that runs the other way along the road");
        Check(blocks.Ahead(7, 1, 520, +1, 0, 10.6) < 0, "past it, it is behind");
        Check(blocks.Ahead(7, 2, 480, +1, 0, 10.6) > 1e17 && blocks.Ahead(8, 1, 480, +1, 0, 10.6) > 1e17, "another lane or road knows nothing of it");
        Check(blocks.Ahead(7, 1, 480, +1, 0, 10.5 + rules::blockMemory + .2) > 1e17, "a blockage nobody reports any more is forgotten");
        Blocks ring; ring.Report(7, 1, 10, 0xA, 1.0);
        Check(std::fabs(ring.Ahead(7, 1, 990, +1, 1000, 1.1) - 20) < 1e-9, "on a closed road it is found across the seam"); }
    {   Waiter a{ 0x10, 0x20, 6, 0 }, b{ 0x20, 0x10, 5.5, 0 };
        Check(Deadlocked(a, b), "two cars that each stand held by the other are a deadlock");
        Waiter moving = b; moving.stoppedFor = 1;
        Check(!Deadlocked(a, moving), "not while one of them has only just stopped");
        Waiter other = b; other.heldBy = 0x30;
        Check(!Deadlocked(a, other), "not when one is held by somebody else");
        Check(Loser(a, b) == 0x20 && Loser(b, a) == 0x20, "the same car yields whichever way it is asked (higher id on a tie)");
        Waiter bus = b; bus.priority = 5;
        Check(Loser(a, bus) == 0x10, "the higher priority goes");
        Waiter longer = b; longer.stoppedFor = 30;
        Check(Loser(a, longer) == 0x10, "then the one that has stood longer goes");
        Yields y;
        Check(y.Set(0x20, 0x10, 100.0) && !y.Set(0x20, 0x10, 101.0), "a yield is set once and then only renewed");
        Check(y.To(0x20, 105.0) == 0x10 && y.To(0x10, 105.0) == 0, "the loser yields, the winner does not");
        Check(y.To(0x20, 101.0 + rules::yieldSeconds + .1) == 0, "a yield ends by itself"); }
    Check(Covers(-1.75, 1.75, -1, 1, 2.6) && !Covers(-1.75, 1.75, .5, 1.75, 2.0) && !Covers(-1.75, 1.75, 3, 5, 2.6), "corridor: a lane is blocked only where neither side leaves the car room");
    std::printf(failed ? "\nFAILED %d\n" : "\nALL PASS\n", failed);
    return failed ? 1 : 0;
}
