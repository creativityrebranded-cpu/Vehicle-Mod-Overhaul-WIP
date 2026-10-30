#pragma once
// Fair share of traffic vehicles between routes (2026-10-29, the user: "each has their own veh count ... I need to be able to fill
// each route"; then "Fair share first"). A free vehicle used to take the NEAREST route with room, so the route by the spawn and
// parking spots took nearly every vehicle and the others stayed empty. Now it takes the route with the lowest share of its count
// filled (on / want), among the routes in reach that are active and have room; ties: the vehicle's own route, then the nearer.
// With fewer vehicles than the counts add up to, every route gets its share (12 for 3 : 2 : 16 -> about 2, 1, 9).
// Pure: tests/routeshare_test.cpp.
#include <cstddef>
#include <vector>

namespace routeshare {
struct Candidate {
    int on = 0, want = 0;   // vehicles on the route now (not counting the one asking), its count now
    double dist = 0;        // m from the vehicle to the route
    bool own = false;       // the route the vehicle just flew
};
// Index of the route to take, -1 when none is in reach with room.
inline int Pick(const std::vector<Candidate>& c, double reach)
{
    int best = -1;
    for (std::size_t i = 0; i < c.size(); ++i) {
        const auto& k = c[i];
        if (k.want <= 0 || k.on >= k.want || !(k.dist <= reach)) continue;
        if (best < 0) { best = int(i); continue; }
        const auto& b = c[std::size_t(best)];
        // on / want compared without division: k.on * b.want vs b.on * k.want
        const long long lhs = (long long)k.on * b.want, rhs = (long long)b.on * k.want;
        if (lhs < rhs || (lhs == rhs && ((k.own && !b.own) || (k.own == b.own && k.dist < b.dist)))) best = int(i);
    }
    return best;
}
}
