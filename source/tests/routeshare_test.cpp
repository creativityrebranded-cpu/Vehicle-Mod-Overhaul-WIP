#include "routeshare.h"
#include <cstdio>

int main()
{
    using namespace routeshare;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };

    // The user's three routes (counts 16, 3, 2), every vehicle starting next to the big one: 12 vehicles handed out one by one.
    {
        int on[3] = { 0, 0, 0 };
        const int want[3] = { 16, 3, 2 };
        const double dist[3] = { 20, 300, 450 };
        for (int v = 0; v < 12; ++v) {
            std::vector<Candidate> c;
            for (int i = 0; i < 3; ++i) c.push_back({ on[i], want[i], dist[i], false });
            const int k = Pick(c, 600);
            if (k >= 0) ++on[k];
        }
        check(on[0] + on[1] + on[2] == 12, "all 12 placed");
        check(on[1] >= 1 && on[2] >= 1, "the small routes get vehicles though the big one is nearest");
        check(on[0] >= 8 && on[0] <= 10 && on[1] >= 1 && on[1] <= 2 && on[2] >= 1 && on[2] <= 2, "shares near 16 : 3 : 2");
    }
    // Enough vehicles: every route filled to its count, none over.
    {
        int on[3] = { 0, 0, 0 };
        const int want[3] = { 16, 3, 2 };
        int placed = 0;
        for (int v = 0; v < 30; ++v) {
            std::vector<Candidate> c;
            for (int i = 0; i < 3; ++i) c.push_back({ on[i], want[i], 10.0 * i, false });
            const int k = Pick(c, 600);
            if (k >= 0) { ++on[k]; ++placed; }
        }
        check(on[0] == 16 && on[1] == 3 && on[2] == 2 && placed == 21, "filled exactly to each count");
    }
    // Out of reach, inactive (count 0) and full routes are never picked; ties keep the vehicle's own route, then the nearer.
    {
        check(Pick({ { 0, 5, 700, false }, { 0, 0, 10, false }, { 3, 3, 10, false } }, 600) == -1, "nothing in reach with room");
        check(Pick({ { 1, 4, 50, false }, { 1, 4, 300, true } }, 600) == 1, "a tie keeps its own route");
        check(Pick({ { 1, 4, 300, false }, { 1, 4, 50, false } }, 600) == 1, "a tie otherwise takes the nearer");
        check(Pick({ { 2, 4, 10, true }, { 1, 4, 500, false } }, 600) == 1, "a less filled route wins over its own");
    }
    std::printf("routeshare: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
