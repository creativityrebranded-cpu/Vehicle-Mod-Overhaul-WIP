#pragma once
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

// Everyday traffic (2026-10-27, the user: "if you can build and test traffic I would like that"). No spawning: the
// parked vehicles already in the world are the pool (each spawn hitched, 2026-10-25), and calm NPCs near them drive
// round trips with the civil tasks 0.8 already has (supply run: out to a settlement / outpost, wait, back home, park;
// patrol: laps round home). The 0.8 triggers never fired in the last three logs (a walk goal over 300 m with a vehicle
// within 30 m, or an owner by its vehicle with a scheduled trip); this picks a trip on a steady cadence instead.
// Pure: the caller supplies vehicles, people and places, and starts the chosen task.
namespace trafficpick {
struct Vehicle { std::uint32_t id = 0; double x = 0, y = 0; bool free = false; };   // free: parked, nobody's, no job, not yours
struct Person { std::uintptr_t ref = 0; double x = 0, y = 0; bool free = false; }; // free: calm, on foot, no job, not a follower or quest NPC
struct Place { double x = 0, y = 0; };
struct Settings {
    int maxJobs = 4;              // civil jobs at once (fights keep the other pilot slots)
    double range = 250.0;         // m from the player: traffic you can see
    double driverNear = 60.0;     // m: a calm person this close to a free vehicle can take it
    double tripMin = 200.0, tripMax = 1000.0;  // m to a place for a round trip
    double restSeconds = 90.0;    // a vehicle rests at least this long between trips
    double everySeconds = 15.0;   // one new trip at most this often
};
enum class Kind { None, RoundTrip, Laps };
struct Pick { Kind kind = Kind::None; std::size_t vehicle = 0, person = 0; Place to{}; double tripLength = 0; };

// usedAt: vehicle id -> when it last started a trip (s). Prefers the vehicle nearest the player (traffic you see), then
// the person nearest that vehicle; a place in [tripMin, tripMax] from the vehicle gives a round trip (the farthest
// such place under tripMax / 2 first, so trips are long enough to be seen moving yet come back), else laps round home.
inline Pick Choose(const std::vector<Vehicle>& vehicles, const std::vector<Person>& people, const std::vector<Place>& places,
    double px, double py, double now, int jobs, const std::unordered_map<std::uint32_t, double>& usedAt, const Settings& s = {})
{
    Pick out;
    if (jobs >= s.maxJobs) return out;
    double bestV = 1e18;
    std::size_t vi = vehicles.size(), pi = people.size();
    for (std::size_t i = 0; i < vehicles.size(); ++i) {
        const auto& v = vehicles[i];
        if (!v.free || !v.id) continue;
        const double dp = std::hypot(v.x - px, v.y - py);
        if (dp > s.range || dp >= bestV) continue;
        if (const auto u = usedAt.find(v.id); u != usedAt.end() && now - u->second < s.restSeconds) continue;
        double bestP = s.driverNear;
        std::size_t found = people.size();
        for (std::size_t k = 0; k < people.size(); ++k) {
            if (!people[k].free || !people[k].ref) continue;
            const double d = std::hypot(people[k].x - v.x, people[k].y - v.y);
            if (d <= bestP) { bestP = d; found = k; }
        }
        if (found == people.size()) continue;
        bestV = dp; vi = i; pi = found;
    }
    if (vi == vehicles.size()) return out;
    out.vehicle = vi; out.person = pi;
    const auto& v = vehicles[vi];
    double best = -1;
    for (const auto& q : places) {
        const double d = std::hypot(q.x - v.x, q.y - v.y);
        if (d < s.tripMin || d > s.tripMax) continue;
        const double score = d <= s.tripMax * 0.5 ? d : s.tripMax - d;  // longest under half the reach, else the nearest beyond it
        if (score > best) { best = score; out.to = q; out.tripLength = d; }
    }
    out.kind = best >= 0 ? Kind::RoundTrip : Kind::Laps;
    return out;
}
}
