#include "probeguard.h"
#include <atomic>
#include <cstdio>
#include <thread>

static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }

int main() {
    using namespace probeguard;
    Check(!ProcgenHold(1000, 0), "no procgen yet: no hold");
    Check(ProcgenHold(5000, 5000), "hold starts at procgen");
    Check(ProcgenHold(5000 + kProcgenHoldMs - 1, 5000), "hold inside the window");
    Check(!ProcgenHold(5000 + kProcgenHoldMs, 5000), "hold ends after the window");
    Check(!ProcgenHold(4000, 5000), "clock before procgen: no hold");

    // Two threads interleave: A locks, B locks, A unlocks, B unlocks. Every read count must come back (the old global lost B's).
    std::atomic<int> count{0}, step{0};
    auto lock = [&](std::uintptr_t w) { ++count; Lock(w); };
    auto release = [&](std::uintptr_t) { --count; };
    auto wait = [&](int s) { while (step.load() != s) std::this_thread::yield(); };
    std::thread a([&] { wait(0); lock(0x1000); step = 1; wait(2); Unlock(release); step = 3; });
    std::thread b([&] { wait(1); lock(0x1000); step = 2; wait(3); Unlock(release); step = 4; });
    a.join(); b.join();
    Check(count.load() == 0, "two threads: every read count returned");
    Unlock(release); Check(count.load() == 0, "unlock without a lock gives nothing back");
    lock(0x2000); Unlock(release); Unlock(release); Check(count.load() == 0, "double unlock gives back once");

    {   unsigned long long d = 0;
        Check(MayCast(10000, 0, 0, d), "no change: cast");
        Check(!MayCast(10000, 1, 0, d), "a change running: skip");
        Check(!MayCast(10100, 0, 10000, d), "just after a change: skip (settle)");
        Check(MayCast(10000 + kSettleMs, 0, 10000, d), "settled: cast");
        d = 0; Check(!MayCast(20000, 0, 19990, d), "continuous changes: first defer");
        Check(!MayCast(20000 + kMaxDeferMs, 1, 20000 + kMaxDeferMs, d), "deferred long but a change is running: still skip");
        Check(MayCast(20000 + kMaxDeferMs + 10, 0, 20000 + kMaxDeferMs, d), "deferred long, nothing running this instant: cast (never blind)"); }
    std::printf("probeguard_test: %s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
