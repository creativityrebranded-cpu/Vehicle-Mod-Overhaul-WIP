#include "hudspread.h"

#include <cstdio>
#include <cstdlib>

using hudspread::Box;
static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }
static bool Overlap(const Box& a, const std::array<double, 2>& oa, const Box& b, const std::array<double, 2>& ob)
{
    const double ax = a.x + oa[0], ay = a.y + oa[1], bx = b.x + ob[0], by = b.y + ob[1];
    return std::min(ax + a.w, bx + b.w) > std::max(ax, bx) && std::min(ay + a.h, by + b.h) > std::max(ay, by);
}

int main()
{
    {   // Two text lines on top of each other: pushed apart up/down, half each.
        const std::vector<Box> b = { { 100, 100, 200, 30 }, { 110, 110, 200, 30 } };
        const auto o = hudspread::Solve(b);
        Check(!Overlap(b[0], o[0], b[1], o[1]), "two overlapping lines end apart");
        Check(o[0][0] == 0 && o[1][0] == 0 && o[0][1] < 0 && o[1][1] > 0, "wide short lines separate vertically, in opposite directions");
    }
    {   // A line on a fixed key: only the line moves.
        const std::vector<Box> b = { { 100, 100, 60, 60, false }, { 120, 140, 200, 30 } };
        const auto o = hudspread::Solve(b);
        Check(o[0][0] == 0 && o[0][1] == 0, "fixed box never moves");
        Check(!Overlap(b[0], o[0], b[1], o[1]), "the movable line clears the fixed key");
    }
    {   // Apart already: nothing moves.
        const std::vector<Box> b = { { 0, 0, 100, 20 }, { 0, 50, 100, 20 }, { 300, 0, 50, 50, false } };
        const auto o = hudspread::Solve(b);
        bool still = true;
        for (const auto& v : o) still = still && v[0] == 0 && v[1] == 0;
        Check(still, "non-overlapping boxes keep their places");
    }
    {   // A stack of five lines dropped on one spot: all end apart.
        std::vector<Box> b;
        for (int i = 0; i < 5; ++i) b.push_back({ 100.0 + i, 100.0 + i * 2, 220, 28 });
        const auto o = hudspread::Solve(b, 4.0, 40);
        bool apart = true;
        for (std::size_t i = 0; i < b.size(); ++i) for (std::size_t j = i + 1; j < b.size(); ++j) apart = apart && !Overlap(b[i], o[i], b[j], o[j]);
        Check(apart, "five stacked lines all end apart");
    }
    {   // The shift is bounded (a box never flies off screen).
        const std::vector<Box> b = { { 0, 0, 100, 1000, false }, { 10, 10, 100, 1000 } };
        const auto o = hudspread::Solve(b, 4.0, 16, 160.0);
        Check(std::fabs(o[1][0]) <= 160.0 && std::fabs(o[1][1]) <= 160.0, "shift capped at 160");
    }
    {   // Two lines at the bottom edge: the push goes up, never below the screen.
        hudspread::Screen s; s.x0 = 0; s.y0 = 0; s.x1 = 1920; s.y1 = 1080; s.valid = true;
        const std::vector<Box> b = { { 100, 1030, 300, 30 }, { 120, 1040, 300, 30 } };
        const auto o = hudspread::Solve(b, 4.0, 40, 160.0, s);
        bool inside = true;
        for (std::size_t i = 0; i < b.size(); ++i) inside = inside && b[i].y + o[i][1] + b[i].h <= 1080 - 8 + 1e-6 && b[i].y + o[i][1] >= 8 - 1e-6;
        Check(inside && !Overlap(b[0], o[0], b[1], o[1]), "at the bottom edge: apart and both on screen");
    }
    {   // A line placed partly off the right edge comes back in, even with nothing to avoid.
        hudspread::Screen s; s.x1 = 1920; s.y1 = 1080; s.valid = true;
        const std::vector<Box> b = { { 1800, 500, 300, 30 } };
        const auto o = hudspread::Solve(b, 4.0, 16, 400.0, s);
        Check(b[0].x + o[0][0] + b[0].w <= 1912 + 1e-6, "a line hanging off the right edge is brought back on screen");
    }
    {   // Easing reaches the target and stays.
        double v = 0;
        for (int i = 0; i < 30; ++i) v = hudspread::Ease(v, 40.0);
        Check(v == 40.0, "easing settles exactly on the target");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
