#include "relocation.h"
#include <cstdio>
#include <string>
using namespace relocation;
static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* what) { ++g_checks; if (!ok) { ++g_fail; std::printf("FAIL %s\n", what); } }

struct Log { int calls = 0; std::uint64_t last = 0; Result result = Result::None; bool accept = true; };
static bool Apply(const Receipt& r, void* u) { auto* l = static_cast<Log*>(u); ++l->calls; l->last = r.serial; l->result = r.result; return l->accept; }

static Request Make(std::uint32_t v = 0xFE001, std::uint64_t epoch = 7) {
    Request q; q.vehicle = v; q.driver = 0x2525; q.epoch = epoch; q.from = { 100, 200, 10, 0 }; q.to = { 100, 206, 10, 0 }; q.stepBefore = 40; q.seatedBefore = true; return q;
}
static Readback Good() {
    Readback r; r.bodyValid = true; r.chassis = { 100.2, 205.9, 10.1, 0 }; r.driver = { 100.2, 206.3, 10.9, 0 };
    r.seatedAfter = true; r.aboardAfter = true; r.physicsLive = true; r.driverPoseKnown = true; r.stepAfter = 42; return r;
}
static bool Is(const char* got, const char* want) { return std::string(got) == want; }

int main() {
    const char* why = "";
    // Judge: every outcome and its reason
    { auto q = Make(); auto r = Good(); Check(Judge(q, r, why) == Result::Applied, "a pair that arrived together is applied"); }
    { auto q = Make(); auto r = Good(); r.bodyValid = false; Check(Judge(q, r, why) == Result::Failed && Is(why, "body_changed"), "a different body fails"); }
    { auto q = Make(); auto r = Good(); r.physicsLive = false; Check(Judge(q, r, why) == Result::Failed && Is(why, "physics_not_live"), "no live physics fails"); }
    { auto q = Make(); auto r = Good(); r.stepAfter = 40; Check(Judge(q, r, why) == Result::None && Is(why, "no_fresh_step"), "no fresh step is undecided, not a verdict"); }
    { auto q = Make(); auto r = Good(); r.chassis.y = 200.5; Check(Judge(q, r, why) == Result::Failed && Is(why, "chassis_not_at_target"), "a chassis left behind fails"); }
    { auto q = Make(); auto r = Good(); r.seatedAfter = false; Check(Judge(q, r, why) == Result::Failed && Is(why, "seat_link_lost"), "a lost seat link fails even if the chassis arrived"); }
    { auto q = Make(); auto r = Good(); r.aboardAfter = false; Check(Judge(q, r, why) == Result::Failed, "not aboard fails"); }
    { auto q = Make(); auto r = Good(); r.driver = { 100, 200, 10, 0 }; Check(Judge(q, r, why) == Result::Failed && Is(why, "driver_left_behind"), "a driver left 6 m behind fails"); }

    { auto q = Make(); auto r = Good(); r.driver = { 100, 200, 10, 0 }; r.driverPoseKnown = false; Check(Judge(q, r, why) == Result::Applied, "a driver position known to lag is not judged; the seat link decides"); }

    // Arm: one relocation per vehicle, epoch checked, bounded room
    {
        Coordinator c; Log l; c.Subscribe("log", Apply, &l);
        auto s = c.Arm(Make(), 7, why); Check(s == 1 && Is(why, "granted"), "the first grant gets serial 1");
        Check(c.Relocating(0xFE001), "a granted pair is relocating");
        Check(c.Arm(Make(), 7, why) == 0 && Is(why, "already_relocating"), "a second grant for the same vehicle is refused");
        Check(c.Arm(Make(0xFE002, 6), 7, why) == 0 && Is(why, "stale_epoch"), "a request from an old epoch is refused");
        Check(c.Arm(Make(0), 7, why) == 0, "a request with no vehicle is refused");
        Check(c.Arm(Make(0xFE003), 7, why) == 2, "a different vehicle is granted its own serial");
    }
    // Full protocol: grant, issued, verify, one receipt to every consumer, exactly once
    {
        Coordinator c; Log a, b; c.Subscribe("health", Apply, &a); c.Subscribe("handoff", Apply, &b);
        auto s = c.Arm(Make(), 7, why); Check(c.MarkIssued(s, 1000), "the doer marks it issued");
        Check(!c.MarkIssued(s, 1001), "it cannot be issued twice");
        Receipt out; auto early = Good(); early.stepAfter = 40;
        Check(!c.Verify(s, early, 1500, out) && a.calls == 0, "no fresh step: nothing is published and nobody is told");
        Check(c.Relocating(0xFE001), "still relocating while undecided");
        Check(c.Verify(s, Good(), 1600, out) && out.result == Result::Applied, "a fresh step with the pair intact publishes applied");
        Check(a.calls == 1 && b.calls == 1 && a.result == Result::Applied, "both consumers were told once");
        Check(out.displacement > 5.9 && out.displacement < 6.1, "the receipt carries the real displacement");
        Check(out.stepBefore == 40 && out.stepAfter == 42 && out.seatedBefore && out.seatedAfter && out.physicsLive, "the receipt carries the steps and seat evidence");
        Check(!c.Relocating(0xFE001), "after the receipt the pair is no longer relocating");
        Check(!c.Verify(s, Good(), 1700, out) && a.calls == 1, "a duplicate verify publishes nothing and cannot re-apply");
    }
    // Failure is published too, and the window expires
    {
        Coordinator c; Log a; c.Subscribe("health", Apply, &a);
        auto s = c.Arm(Make(), 7, why); c.MarkIssued(s, 1000);
        auto rb = Good(); rb.seatedAfter = false; Receipt out;
        Check(c.Verify(s, rb, 1200, out) && out.result == Result::Failed && Is(out.reason, "seat_link_lost") && a.result == Result::Failed, "a broken seat is published as failed with its reason");
        auto s2 = c.Arm(Make(0xFE004), 7, why); c.MarkIssued(s2, 2000);
        auto idle = Good(); idle.stepAfter = 40;
        Check(!c.Verify(s2, idle, 3000, out), "inside the window an undecided read-back keeps waiting");
        Check(c.Verify(s2, idle, 2000 + kVerifyWindowMs + 1, out) && out.result == Result::Failed && Is(out.reason, "no_completed_step"), "no completed step before the window ends is failed, not left hanging");
    }
    // Refusal before anything was changed
    {
        Coordinator c; Log a; c.Subscribe("health", Apply, &a);
        auto s = c.Arm(Make(), 7, why);
        auto r = c.Refuse(s, "destination_not_clear", 500);
        Check(r.result == Result::Refused && a.calls == 1 && !c.Relocating(0xFE001), "a refusal is published and releases the untouched pair");
        Check(c.Refuse(s, "again", 600).result == Result::None, "a refusal cannot be repeated");
    }
    // Restore: only published when the read-back really shows the return
    {
        Coordinator c; Log a; c.Subscribe("health", Apply, &a);
        auto s = c.Arm(Make(), 7, why); c.MarkIssued(s, 10); Receipt out; auto rb = Good(); rb.chassis.y = 200.4; c.Verify(s, rb, 20, out);
        Readback back = Good(); back.chassis = { 100.1, 200.2, 10, 0 }; back.stepAfter = 50;
        Check(c.Restored(s, back, 30).result == Result::Restored, "a proved return to origin is published as restored");
        Readback no = back; no.seatedAfter = false;
        Check(c.Restored(s, no, 40).result == Result::Failed, "a return without the seat is not called restored");
    }
    // Consumers: at most once, refusals counted, subscribe rules, epoch helper
    {
        Coordinator c; Log a, b; b.accept = false;
        Check(c.Subscribe("health", Apply, &a) && c.Subscribe("route", Apply, &b), "consumers subscribe");
        Check(!c.Subscribe("health", Apply, &a), "a name subscribes once");
        Check(!c.Subscribe(nullptr, Apply, &a) && !c.Subscribe("x", nullptr, &a), "invalid subscriptions are refused");
        auto s = c.Arm(Make(), 7, why); c.MarkIssued(s, 1); Receipt out; c.Verify(s, Good(), 2, out);
        Check(c.ConsumerAt(0)->appliedCount == 1 && c.ConsumerAt(1)->refusedCount == 1, "an unapplied receipt is counted as refused, not lost");
        Check(Coordinator::Current(out, 7) && !Coordinator::Current(out, 8), "a consumer rejects a receipt from another epoch");
        static const char* names[] = { "a1","a2","a3","a4","a5","a6","a7","a8","a9","b1","b2","b3","b4","b5","b6","b7","b8","b9" };
        for (const char* n : names) c.Subscribe(n, Apply, &a);
        Check(c.ConsumerCount() <= kMaxConsumers, "the registry is bounded");
    }
    // Room: the active table is bounded and recycles published slots
    {
        Coordinator c; Log a; c.Subscribe("h", Apply, &a);
        int granted = 0; for (std::uint32_t v = 1; v <= 20; ++v) if (c.Arm(Make(v), 7, why)) ++granted;
        Check(granted == int(kMaxActive), "no more than the table holds are active at once");
        auto first = c.Lookup(1); Check(first && first->req.vehicle == 1, "an active record can be looked up by serial");
        c.MarkIssued(1, 1); Receipt out; c.Verify(1, Good(), 2, out);
        Check(c.Arm(Make(99), 7, why) != 0, "a published slot is recycled");
    }
    // The latest receipt per vehicle is kept for a polling consumer
    {
        Coordinator c; Log a; c.Subscribe("h", Apply, &a); Receipt got;
        Check(!c.LastFor(0xFE001, got), "no receipt before any relocation");
        auto s = c.Arm(Make(), 7, why); c.MarkIssued(s, 1); Receipt out; c.Verify(s, Good(), 2, out);
        Check(c.LastFor(0xFE001, got) && got.serial == s && got.result == Result::Applied, "the applied receipt is remembered for the vehicle");
        auto s2 = c.Arm(Make(), 7, why); c.Refuse(s2, "destination_not_clear", 3);
        Check(c.LastFor(0xFE001, got) && got.serial == s2 && got.result == Result::Refused, "a later receipt replaces the earlier one for the same vehicle");
        for (std::uint32_t v = 1; v <= 40; ++v) { auto t = c.Arm(Make(v), 7, why); if (t) c.Refuse(t, "x", 4); }
        Check(c.LastFor(40, got), "the table stays bounded and keeps the newest");
    }
    std::printf("relocation: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
