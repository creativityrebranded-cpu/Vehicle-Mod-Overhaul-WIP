// Card R6 (notes/f7-restore-20261108/CHANGE_CARDS.md): F7 Problems grouped by where they are (f7dock::GroupRows), and the restored levels' names.
#include "f7dock.h"
#include <cstdio>
#include <cstring>
int main() {
    int total = 0, failed = 0;
    auto check = [&](bool good, const char* name) { ++total; if (!good) { ++failed; std::printf("FAIL %s\n", name); } };
    // Points 3 and 4 close together share a group; point 9 is its own; a tile and the whole route are apart.
    const std::vector<std::pair<int, int>> where{ { 0, 4 }, { 0, 3 }, { 1, 12 }, { 0, -1 }, { 0, 9 }, { 1, 13 } };
    const auto g = f7dock::GroupRows(where);
    check(g.size() == 4, "four groups");
    check(g[0].kind == 0 && g[0].first == 3 && g[0].last == 4 && g[0].rows.size() == 2 && g[0].rows[0] == 1 && g[0].rows[1] == 0, "points 4-5 together, in point order");
    check(g[1].kind == 0 && g[1].first == 9 && g[1].rows.size() == 1, "point 10 alone");
    check(g[2].kind == 1 && g[2].first == 12 && g[2].last == 13 && g[2].rows.size() == 2, "tiles 13-14 together");
    check(g[3].kind == 2 && g[3].rows.size() == 1 && g[3].rows[0] == 3, "whole route last");
    check(f7dock::GroupRows({}).empty(), "nothing: no groups");
    check(f7dock::GroupRows({ { 0, 1 }, { 0, 4 } }).size() == 2, "points 3 apart: two groups");
    check(f7dock::GroupRows({ { 0, 1 }, { 0, 3 } }).size() == 1, "points 2 apart: one group");
    // Levels: the old numbers are unchanged; the new ones have names; place levels.
    check(f7dock::Routes == 0 && f7dock::Problems == 7 && f7dock::Places == 8 && f7dock::Wires == 15 && f7dock::TrafficLevels == 16 && f7dock::LevelCount == 17, "level numbers");  // variant: the 2026-11-10 traffic levels menu was appended as 16; the old numbers are unchanged
    check(std::strcmp(f7dock::LevelName(f7dock::Junction), "junction") == 0 && std::strcmp(f7dock::LevelName(16), "traffic levels") == 0 && std::strcmp(f7dock::LevelName(17), "?") == 0, "level names");
    check(f7dock::PlaceSelected(f7dock::Access) && !f7dock::PlaceSelected(f7dock::Places) && f7dock::PlaceLevel(f7dock::Places) && !f7dock::PlaceLevel(f7dock::Nodes), "place levels");
    std::printf("f7restore_test: %d of %d passed\n", total - failed, total);
    return failed ? 1 : 0;
}
