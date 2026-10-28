#include "trafficpick.h"

#include <cstdio>

using namespace trafficpick;
static int g_fail = 0;
static void Check(bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++g_fail; }

int main()
{
    const std::unordered_map<std::uint32_t, double> none;
    {   // A free vehicle by a calm person, a settlement 500 m away: a round trip there.
        const std::vector<Vehicle> v = { { 10, 50, 0, true } };
        const std::vector<Person> p = { { 0x100, 60, 10, true } };
        const std::vector<Place> q = { { 550, 0 } };
        const auto r = Choose(v, p, q, 0, 0, 100, 0, none);
        Check(r.kind == Kind::RoundTrip && r.to.x == 550 && r.person == 0, "round trip to the place in reach");
    }
    {   // No place in reach: laps round home.
        const std::vector<Vehicle> v = { { 10, 50, 0, true } };
        const std::vector<Person> p = { { 0x100, 60, 10, true } };
        const std::vector<Place> q = { { 5000, 0 }, { 80, 0 } };
        Check(Choose(v, p, q, 0, 0, 100, 0, none).kind == Kind::Laps, "laps when no place is 200-1000 m away");
    }
    {   // Limits: jobs at the cap, vehicle resting, too far from the player, nobody near it, the person busy.
        const std::vector<Vehicle> v = { { 10, 50, 0, true } };
        const std::vector<Person> p = { { 0x100, 60, 10, true } };
        const std::vector<Place> q = { { 550, 0 } };
        Check(Choose(v, p, q, 0, 0, 100, 4, none).kind == Kind::None, "no new trip at the job cap");
        std::unordered_map<std::uint32_t, double> used = { { 10, 50.0 } };
        Check(Choose(v, p, q, 0, 0, 100, 0, used).kind == Kind::None, "a vehicle rests 90 s between trips");
        Check(Choose(v, p, q, 0, 0, 200, 0, used).kind == Kind::RoundTrip, "after its rest it can go again");
        Check(Choose(v, p, q, 1000, 0, 100, 0, none).kind == Kind::None, "only vehicles within 250 m of the player");
        const std::vector<Person> far = { { 0x100, 200, 10, true } };
        Check(Choose(v, far, q, 0, 0, 100, 0, none).kind == Kind::None, "nobody within 60 m of the vehicle: no trip");
        const std::vector<Person> busy = { { 0x100, 60, 10, false } };
        Check(Choose(v, busy, q, 0, 0, 100, 0, none).kind == Kind::None, "a busy person never drives");
        const std::vector<Vehicle> taken = { { 10, 50, 0, false } };
        Check(Choose(taken, p, q, 0, 0, 100, 0, none).kind == Kind::None, "a vehicle in use is never taken");
    }
    {   // The vehicle nearest the player goes first; its nearest person drives.
        const std::vector<Vehicle> v = { { 10, 200, 0, true }, { 11, 40, 0, true } };
        const std::vector<Person> p = { { 0x100, 210, 0, true }, { 0x200, 45, 5, true }, { 0x300, 70, 0, true } };
        const auto r = Choose(v, p, {}, 0, 0, 100, 0, none);
        Check(r.vehicle == 1 && r.person == 1, "nearest vehicle to the player, nearest person to it");
    }
    {   // Several places: the longest trip under half the reach.
        const std::vector<Vehicle> v = { { 10, 0, 0, true } };
        const std::vector<Person> p = { { 0x100, 5, 0, true } };
        const std::vector<Place> q = { { 250, 0 }, { 480, 0 }, { 900, 0 } };
        Check(Choose(v, p, q, 0, 0, 100, 0, none).to.x == 480, "prefers the longest trip under 500 m");
    }
    std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
