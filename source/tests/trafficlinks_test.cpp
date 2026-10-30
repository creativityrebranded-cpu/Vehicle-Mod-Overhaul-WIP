#include "trafficlinks.h"
#include <cstdio>

int main()
{
    using namespace trafficlinks;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } };
    trafficdata::Data d;
    trafficdata::Read(
        "[route.ring]\ncentre = 0 0\nheight = 100\nradius = 500\n\n"
        "[route.ed_branch]\ntype = path\npoints = 0 500 100, 0 900 100, 500 0 100\nlanes = 1\n\n"
        "[route.ed_two]\ntype = path\npoints = 0 -500 100, 0 -900 100\nlanes = 2\n\n"
        "[link.in]\nfrom = ring\nat = 0\nto = ed_branch\nto_at = start\nshare = 0.25\n\n"
        "[link.out]\nfrom = ring\nat = 90\nto = ed_branch\nto_at = end\nshare = 0\n\n"
        "[link.bad]\nfrom = ring\nat = x\nto = ed_two\n\n"
        "[link.self]\nfrom = ring\nat = 3\nto = ring\n", "t.ini", d);
    check(d.links.size() == 2 && d.problems.size() == 2, "links read, bad ones named");
    check(d.links[0].share == 0.25 && !d.links[0].toEnd && d.links[1].toEnd, "share and end");
    std::vector<const trafficdata::Route*> routes;
    for (const auto& r : d.routes) routes.push_back(&r);
    const auto ring = Of(d.links, routes, "ring");
    check(ring.size() == 2 && ring[0].u == 0 && ring[1].u == 24 && ring[0].other == "ed_branch" && ring[1].otherU == 2, "ring sees both junctions in its own u");
    const auto branch = Of(d.links, routes, "ed_branch");
    check(branch.size() == 2 && branch[0].u == 0 && branch[1].u == 2 && branch[1].otherU == 24, "branch sees its two ends");
    check(Of(d.links, { routes[0] }, "ring").empty(), "links to routes of other worlds are ignored");
    const auto bl = trafficroute::Build(*routes[1]), two = trafficroute::Build(*routes[2]), rl = trafficroute::Build(*routes[0]);
    check(EntryLane(bl, 1, 0) == 0 && EntryLane(bl, 1, 2) == -1 && EntryLane(two, 2, 1) == 1 && EntryLane(rl, 2, 30) == 0, "entry lanes");
    check(LaneEnd(bl, 0) == 2 && LaneEnd(bl, 1) == 0, "lane ends");
    check(Crossed(rl, 0, 23.5, 24.2, 24) && !Crossed(rl, 0, 24, 24.5, 24) && !Crossed(rl, 1, 23.5, 24.2, 24) && Crossed(rl, 1, 24.2, 23.5, 24), "crossing in the direction of travel");
    check(Crossed(rl, 0, 95.6, 0.3, 0), "crossing the ring's seam");
    check(At(ring, 24.2) && !At(ring, 12), "junction at a point");
    std::uint64_t dice = 7; int below = 0;
    for (int i = 0; i < 10000; ++i) if (Roll(dice) < 0.25) ++below;
    check(below > 2300 && below < 2700, "share 0.25 turns about a quarter of the time");
    // A junction in the middle of a branch (to_at = a point index, 2026-10-29).
    {
        trafficdata::Data m;
        trafficdata::Read("[route.ring]\ncentre = 0 0\nheight = 100\nradius = 500\n\n"
                          "[route.ed_b]\ntype = path\npoints = 0 500 100, 0 700 100, 300 700 100, 500 0 100\nlanes = 1\n\n"
                          "[link.mid]\nfrom = ring\nat = 45\nto = ed_b\nto_at = 2\nshare = 0.5\n\n"
                          "[link.bad]\nfrom = ring\nat = 10\nto = ed_b\nto_at = -3\n", "m.ini", m);
        check(m.links.size() == 1 && m.links[0].toAt == 2 && !m.links[0].toEnd && m.problems.size() == 1, "to_at as a point index; a negative one refused");
        std::vector<const trafficdata::Route*> rs;
        for (const auto& r : m.routes) rs.push_back(&r);
        const auto onBranch = Of(m.links, rs, "ed_b"), onRing = Of(m.links, rs, "ring");
        check(onBranch.size() == 1 && onBranch[0].u == 2 && onBranch[0].other == "ring" && onBranch[0].otherU == 12, "the branch sees it at its point 3");
        check(onRing.size() == 1 && onRing[0].u == 12 && onRing[0].otherU == 2, "the ring leads into the branch's middle");
        const auto bl = trafficroute::Build(*rs[1]);
        check(EntryLane(bl, 1, 2) == 0, "entering a one-way branch in its middle runs forward");
    }
    {
        trafficdata::Route a;a.ring=false;a.closed=false;a.lanes=1;a.name="main";a.points={{0,-100,20},{0,100,20}};
        trafficdata::Route b=a;b.name="branch";b.points={{0,0,20},{100,0,20}};
        const auto from=trafficroute::Build(a),to=trafficroute::Build(b);
        const auto curve=TurnPath(from,0,0.5,to,0,0,20);
        bool finite=curve.size()>4;for(const auto& p:curve)finite=finite&&std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z);
        check(finite&&curve.front().y<0&&curve.back().x>0,"rounded junction creates an approach, circular turn and departure");
    }
    std::printf("trafficlinks: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
