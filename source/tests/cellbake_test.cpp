#include "cellbakegrid.h"
#include <cstdio>

int main()
{
    using namespace cellbakegrid;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };

    Grid g;
    double z = 0;
    check(!g.Get(3, 3, z), "nothing baked: unknown");
    // A slope rising 1 m per 8 m along x.
    for (int ix = -2; ix <= 2; ++ix)
        for (int iy = -2; iy <= 2; ++iy) g.Put(ix, iy, float(10 + ix));
    check(g.Get(4, 4, z) && std::fabs(z - 10.5) < 1e-6, "blend between points");
    check(g.Get(-12, 0, z) && std::fabs(z - 8.5) < 1e-6, "negative coordinates");
    check(g.Get(0, 0, z) && std::fabs(z - 10) < 1e-6, "on a point");
    // Edge of the baked area: corners missing, the nearest point within half a step.
    check(g.Get(17, 0, z) && std::fabs(z - 12) < 1e-6, "edge: nearest point within half a step");
    check(!g.Get(30, 0, z), "beyond the edge: unknown");
    check(Index(-0.1) == -1 && Index(7.99) == 0 && Index(8.0) == 1, "index rounds down");
    check(Key(1, -1) != Key(-1, 1) && Key(0, 1) != Key(1, 0), "keys differ");

    Agreement a;
    a.Add(10.0, 10.2); a.Add(5.0, 8.0); a.Add(1.0, 1.0);
    check(a.samples == 3 && a.close == 2 && a.off2 == 1 && std::fabs(a.Mean() - (0.2 + 3.0) / 3) < 1e-9, "agreement counts");
    std::printf("cellbake: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
