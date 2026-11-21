#pragma once
// Ray freeze guard (change card F-LOCK, 2026-11-07, user-approved boundary change). Pure; tests: tests/probeguard_test.cpp.
// 1. Each thread keeps its own record of the physics-world read lock it holds. The old record was one global: two job threads cast
//    rays at once (crumbs 2026-11-07 13:17 and 13:21), one release cleared the other's record and that read count was never returned,
//    so the game waited on the lock for ever.
// 2. Rays pause for kProcgenHoldMs after the game's map.procgen: every in-play procgen in both freeze runs was followed within 2 s by a
//    ray fault (+0x3730CC0 / +0x3730D0B, freed memory) or the freeze.
#include <atomic>
#include <cstdint>

namespace probeguard {
constexpr unsigned long long kProcgenHoldMs = 3000;
inline bool ProcgenHold(unsigned long long now, unsigned long long last) { return last != 0 && now >= last && now - last < kProcgenHoldMs; }

// B2 (2026-11-07): the 3 faults the procgen hold missed followed other broadphase rebuilds: vehicle bodies built (03:16:30-31,
// fault 03:16:37) and motion types switched (12:46, 11:53). The existing hooks already count both (vehiclediag::PhysicsChangeCount,
// read-only); when the count rises, rays wait kSettleMs. Under continuous change they still cast after kMaxDeferMs (never blind).
// No hook or native call changes. `changing` is always 0 from the counter (a change is only seen once it has happened).
constexpr unsigned long long kSettleMs = 250, kMaxDeferMs = 500;
// May a ray walk the broadphase now? deferSince: when this caller first deferred (0: not deferring); updated here.
inline bool MayCast(unsigned long long nowMs, int changing, unsigned long long changedAt, unsigned long long& deferSince) {
    const bool settling = changing > 0 || (changedAt && nowMs >= changedAt && nowMs - changedAt < kSettleMs);
    if (!settling) { deferSince = 0; return true; }
    if (!deferSince) deferSince = nowMs;
    if (nowMs - deferSince >= kMaxDeferMs && changing == 0) { deferSince = 0; return true; }  // never blind: cast between changes
    return false;
}
struct Held { std::uintptr_t world = 0; };
inline thread_local Held held;
inline void Lock(std::uintptr_t world) { held.world = world; }
// Gives back only this thread's own lock, once.
template <class Release> inline void Unlock(Release release) { if (!held.world) return; const auto w = held.world; held.world = 0; release(w); }
}
