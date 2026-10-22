#include "physicssetup.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

int main()
{
    int failed = 0, checks = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failed; std::printf("FAIL %s\n", name); } };
    physicssetup::Chassis c{0x1020, 0x1018, 0x01000013, 1};
    unsigned inspected = 0, writes = 0, rechecks = 0;
    bool stillStatic = false;
    auto run = [&](std::uintptr_t caller, bool rejected) {
        inspected = writes = rechecks = 0;
        return physicssetup::Reject(caller, rejected, [&] { ++inspected; return c; },
            [&](auto) { ++writes; }, [&] { ++rechecks; return stillStatic; });
    };
    check(!run(physicssetup::kBuilderReturn, true) && writes == 1 && rechecks == 1,
        "a valid static chassis is changed then checked by the real predicate");
    stillStatic = true;
    check(run(physicssetup::kBuilderReturn, true) && writes == 1 && rechecks == 1,
        "a failed motion change never bypasses native rejection");
    check(run(0xB50000, true) && !inspected && !writes, "other callers keep static bodies static");
    check(!run(physicssetup::kBuilderReturn, false) && !inspected && !writes, "working vehicles are untouched");
    for (const auto invalid : {physicssetup::Chassis{}, physicssetup::Chassis{0x1020, 0, 3, 1},
        physicssetup::Chassis{0x1020, 0x2018, 3, 1}, physicssetup::Chassis{0x1020, 0x1018, 0xFFFFFFFF, 1},
        physicssetup::Chassis{0x1020, 0x1018, 3, 0}, physicssetup::Chassis{0x1020, 0x1018, 3, 2},
        physicssetup::Chassis{0x1020, 0x1018, 3, 4}, physicssetup::Chassis{0x1020, 0x1018, 3, 3}}) {
        c = invalid;
        check(run(physicssetup::kBuilderReturn, true) && !writes && !rechecks,
            "missing worlds, mismatched worlds, invalid and non-static bodies stay rejected");
    }
    {
        float launch[3] = {29.0f, 3.0f, 60.0f};
        check(physicssetup::ClampLaunch(launch) && launch[0] == 0 && launch[1] == 0 && launch[2] == 0,
            "a launch loses its horizontal and upward speed");
        float falling[3] = {0.5f, -0.3f, -9.0f};
        check(!physicssetup::ClampLaunch(falling) && falling[2] == -9.0f && falling[0] == 0.5f,
            "settling and falling are left alone");
        float sideways[3] = {6.0f, 0.0f, -1.0f};
        check(physicssetup::ClampLaunch(sideways) && sideways[0] == 0 && sideways[2] == -1.0f,
            "a sideways shove is stopped, its fall kept");
        float bad[3] = {NAN, 50.0f, 50.0f};
        check(!physicssetup::ClampLaunch(bad) && bad[1] == 50.0f, "unreadable velocities are not touched");
        unsigned long long until = 0;
        check(physicssetup::GuardOpen(until, 1000) && until == 1000 + physicssetup::kLaunchGuardMs,
            "the window opens at the first step");
        check(physicssetup::GuardOpen(until, 1000 + physicssetup::kLaunchGuardMs - 1) &&
            !physicssetup::GuardOpen(until, 1000 + physicssetup::kLaunchGuardMs), "and closes after the guard time");
    }
    {
        // 22 vehicles pending at once: the game's 8-entry list would keep the last few; ours keeps them all.
        physicssetup::PendingSet set;
        for (std::uint32_t id = 0xFE008800; id < 0xFE008816; ++id) set.Add(id);
        bool rescued = false, all = true;
        for (std::uint32_t id = 0xFE008800; id < 0xFE008816; ++id) all = all && set.Member(false, id, rescued) && rescued;
        check(all && set.Size() == 22, "every pending vehicle stays a member however many load together");
        check(set.Member(true, 0x12345678, rescued) && !rescued, "the game's own answer wins and is not counted as a rescue");
        check(!set.Member(false, 0x12345678, rescued) && !rescued, "a vehicle nobody listed is not a member");
        set.Remove(0xFE008803);
        check(!set.Member(false, 0xFE008803, rescued) && set.Size() == 21, "the remover takes a vehicle out");
        set.Add(0);
        check(set.Size() == 21, "an empty form id is never listed");
    }
    {
        // A wreck hovering still, then native gravity pulls 10 m/s^2: over 1 s the glide lets it reach only ~4 m/s down.
        float v[3] = {-4.5f, -20.8f, 0.0f}, floorVz = 0.0f;
        for (int i = 0; i < 60; ++i) { v[2] -= 10.0f / 60.0f; physicssetup::Glide(v, floorVz, 1.0f / 60.0f); }
        check(v[2] < -3.9f && v[2] > -4.1f, "the fall builds up at the glide rate, not full gravity");
        check(std::hypot(v[0], v[1]) < 21.3f * 0.56f && std::hypot(v[0], v[1]) > 21.3f * 0.54f, "sideways speed bleeds off");
        float up[3] = {0.0f, 0.0f, 5.0f}, f2 = 0.0f;
        physicssetup::Glide(up, f2, 1.0f / 60.0f);
        check(up[2] == 5.0f, "the glide never stops or adds an upward motion");
        float slow[3] = {0.0f, 0.0f, -1.0f}, f3 = -1.0f;  // wrecked while already sinking at 1 m/s
        physicssetup::Glide(slow, f3, 1.0f / 60.0f);
        check(slow[2] == -1.0f, "a fall slower than the glide floor is left alone");
        float bad[3] = {0.0f, 0.0f, -50.0f}, f4 = 0.0f;
        physicssetup::Glide(bad, f4, 1.0f);
        check(bad[2] == -50.0f, "a long gap between steps is skipped, not applied as one jump");
    }
    std::printf("physicssetup: %d checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
