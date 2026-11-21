// LF-1 (2026-11-12): the line a car follows never folds or kinks. The kink is the one recorded at lc_ring_road's hairpin in the 13:56 run
// (plan_replay --at -46.9,1746.2): yaw -92 -> +13 degrees within 1 m, a 0.71 m step rising 0.62 m.
#include "lanesmooth.h"
#include <cstdio>
static int failed = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failed; }
struct P { double x, y, z; };
static double WorstTurn(const P* p, int n) { double w = 0; for (int i = 1; i + 1 < n; ++i) w = std::max(w, lanesmooth::Turn(p[i - 1], p[i], p[i + 1]) * 57.29578); return w; }
static double SteepestRise(const P* p, int n) { double w = 0; for (int i = 1; i < n; ++i) { const double run = std::hypot(p[i].x - p[i - 1].x, p[i].y - p[i - 1].y); w = std::max(w, std::fabs(p[i].z - p[i - 1].z) / std::max(run, 1e-3)); } return w; }
int main() {
    {   P line[64] = { {-53.61,1751.52,-97.39},{-54.60,1751.45,-97.33},{-55.59,1751.33,-97.27},{-56.58,1751.18,-97.20},{-57.56,1751.02,-97.12},
                       {-58.41,1750.90,-96.70},{-59.12,1750.87,-96.07},{-58.91,1751.82,-95.82},{-58.69,1752.77,-95.59},{-58.47,1753.73,-95.46},
                       {-58.26,1754.70,-95.33},{-58.15,1755.68,-95.18},{-58.10,1756.66,-95.03} };
        int n = 13; const double before = WorstTurn(line, n), riseBefore = SteepestRise(line, n);
        const auto r = lanesmooth::Repair(line, n, 64);
        std::printf("  recorded hairpin: worst turn %.0f -> %.0f deg, steepest rise %.2f -> %.2f, removed %d rounded %d, points %d\n", before, WorstTurn(line, n), riseBefore, SteepestRise(line, n), r.removed, r.rounded, n);
        Check(before > 100 && WorstTurn(line, n) < 70, "LF-1: the recorded hairpin kink (105 deg in 1 m) is repaired to a corner a car can follow");
        Check(SteepestRise(line, n) < 1.0, "LF-1: no stretch of the repaired line rises more than it runs (no fake 'steep ahead')"); }
    {   P line[64] = { {0,0,0},{0,10,0},{0,20,1},{0,30,2},{0,40,3} }; int n = 5; const P copy[5] = { line[0], line[1], line[2], line[3], line[4] };
        const auto r = lanesmooth::Repair(line, n, 64); bool same = n == 5; for (int i = 0; i < 5 && same; ++i) same = line[i].x == copy[i].x && line[i].y == copy[i].y && line[i].z == copy[i].z;
        Check(same && r.removed == 0 && r.rounded == 0, "LF-1: a straight line comes back unchanged"); }
    {   P line[64] = { {0,0,0},{0,20,0},{20,20,0} }; int n = 3;  // a right-angle corner with long legs
        const auto r = lanesmooth::Repair(line, n, 64);
        Check(r.rounded == 1 && n == 4 && WorstTurn(line, n) <= 46, "LF-1: a 90 deg corner gets a short fillet (cut at most 2.5 m each side)");
        Check(std::fabs(line[1].y - 17.5) < 1e-9 && std::fabs(line[2].x - 2.5) < 1e-9, "LF-1: the fillet cuts 2.5 m back along each leg"); }
    {   P line[64] = { {0,0,0},{0,10,0},{0,10.8,0},{0,10.2,0},{0,20,0} }; int n = 5;  // a fold: forward, back 0.6 m, forward again
        const auto r = lanesmooth::Repair(line, n, 64);
        Check(r.removed >= 1 && WorstTurn(line, n) < 1, "LF-1: a fold (the line doubling back) is removed");
        int capped = 3; P full[3] = { {0,0,0},{0,20,0},{20,20,0} }; lanesmooth::Repair(full, capped, 3);
        Check(capped == 3, "LF-1: a full array is never written past its size"); }
    std::printf("%s\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
