#include "../src/placementedit.h"
#include <cstdio>

static int failures = 0, checks = 0;
static void check(bool ok, const char* what) { ++checks; std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++failures; }

int main()
{
    using namespace placementedit;
    // Deleting: the saved spot nearest the aim, close enough and on the same level.
    const std::vector<Point> spots = { { 0, 0, 0 }, { 4, 0, 0 }, { 0, 3, 10 } };
    check(Pick(spots, { 3.5, 0, 0.5 }) == 1, "the nearest saved spot to the aim is picked");
    check(Pick(spots, { 0, 3, 0 }) == 0, "a spot on another level is not picked");
    check(Pick(spots, { 20, 20, 0 }) == -1, "nothing near the aim: nothing deleted");
    check(Pick(spots, { NAN, 0, 0 }) == -1 && Pick({}, { 0, 0, 0 }) == -1, "bad aim or no spots: nothing");

    // Summon points: one per area, per world.
    std::vector<SummonPoint> points;
    check(SetSummon(points, { "Jemison", 0, 0, 0, 1 }) == 0 && points.size() == 1, "the first summon point is added");
    check(SetSummon(points, { "Jemison", 100, 0, 0, 2 }) == 1 && points.size() == 1 && points[0].x == 100, "a new point in the same area replaces the old one");
    SetSummon(points, { "Jemison", 2000, 0, 0, 0 });
    SetSummon(points, { "Akila", 100, 0, 0, 0 });
    check(points.size() == 3, "other areas and other worlds keep their own points");
    check(SummonFor(points, "Jemison", 150, 20) == 0, "a summon nearby uses the area's point");
    check(SummonFor(points, "Jemison", 1000, 0) == -1, "a summon far from every point: next to the player as before");
    check(SummonFor(points, "Akila", 0, 0) == 2 && SummonFor(points, "Nowhere", 0, 0) == -1, "only points in the player's world count");
    check(SummonFor(points, "", 0, 0) == -1, "no world: no point");

    // The file line survives a save and a load.
    SummonPoint back;
    const SummonPoint p{ "JemisonPlanetWorld157133511011091040", -867.709, 1593.156, -161.54, 2.45390 };
    check(Parse(Line(p), back) && back.world == p.world && std::fabs(back.x - p.x) < 1e-3 && std::fabs(back.yaw - p.yaw) < 1e-4, "a summon point line round-trips");
    check(!Parse("broken", back) && !Parse("\t1\t2\t3\t4", back) && !Parse("W\tx\t2\t3\t4", back), "broken lines are skipped");

    std::printf("placementedit: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
