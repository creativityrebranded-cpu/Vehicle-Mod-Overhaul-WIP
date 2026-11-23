// Hazard picking and placement (2026-11-22): hit testing on the projected footprint, selection state, two-click placement incl. Escape, height / base
// maths, index bookkeeping after delete, group turn, and "no drift": the same inputs always give the same projected box and the same pick.
#include "hazardpick.h"
#include <cstdio>
#include <cstring>
int main() {
    using namespace hazardpick; using routehazard::Box;
    int total = 0, failed = 0;
    auto check = [&](bool good, const char* name) { ++total; if (!good) { ++failed; std::printf("FAIL %s\n", name); } };
    // A top-down camera, 10 px per metre, 3 px per metre of height (so the top face sits above the base on screen). Screen y grows downward.
    auto project = [](double x, double y, double z, double& sx, double& sy) { sx = 500 + x * 10; sy = 500 - y * 10 - z * 3; return true; };
    Box a; a.x = 0; a.y = 0; a.z = 2; a.hx = 1.5; a.hy = 3; a.hz = 2; a.yaw = 0;      // 3 across x 6 along, heights 0..4
    Box b; b.x = 20; b.y = 0; b.z = 2; b.hx = 1; b.hy = 1; b.hz = 1; b.yaw = 90;
    std::vector<Box> two{ a, b };
    // --- hit testing: footprint, outline slack, miss
    check(Pick(two, project, 500, 500 - 2 * 3) == 0, "click in the middle of box 0 picks it (Select, Move)");
    check(Pick(two, project, 500 + 15 + 6, 500) == 0, "a click 6 px outside the outline still picks (slack 10)");
    check(Pick(two, project, 500 + 15 + 40, 500) == -1, "a click 40 px away picks nothing");
    check(Pick(two, project, 700, 500 - 3) == 1, "box 1 picked at its own spot");
    check(Pick(std::vector<Box>{}, project, 500, 500) == -1, "no boxes, no pick");
    // The footprint is the hull of ALL eight corners: a click between the bottom and top face of a tall box hits (the top quad alone missed it).
    Box tall; tall.x = 0; tall.y = 0; tall.z = 20; tall.hx = 1; tall.hy = 1; tall.hz = 20;  // 0..40 m tall: 120 px of screen between the faces
    check(Pick(std::vector<Box>{ tall }, project, 500, 500 - 60) == 0, "a click on the side of a tall box hits");
    // A small box lying on a large one stays selectable: smallest footprint wins among boxes holding the click.
    Box big; big.x = 0; big.y = 0; big.z = 2; big.hx = 10; big.hy = 10; big.hz = 2;
    Box small; small.x = 1; small.y = 1; small.z = 2; small.hx = .5; small.hy = .5; small.hz = 2;
    check(Pick(std::vector<Box>{ big, small }, project, 510, 500 - 10 - 6) == 1, "small box on a big one wins at its own place");
    check(Pick(std::vector<Box>{ big, small }, project, 450, 500 - 6) == 0, "the big box elsewhere");
    // Partly off view: a corner that does not project no longer hides the box.
    auto clip = [&](double x, double y, double z, double& sx, double& sy) { project(x, y, z, sx, sy); return x < 1.0; };  // the east side cannot be projected
    P2 pts[8]; check(ProjectCorners(a, clip, pts) == 4, "corners that fail are skipped, not the whole box");
    check(Pick(std::vector<Box>{ a }, clip, 490, 500 - 6) == 0, "a partly clipped box is still picked from what shows");
    auto none = [&](double, double, double, double&, double&) { return false; };
    check(ProjectCorners(a, none, pts) == 0 && Pick(std::vector<Box>{ a }, none, 500, 500) == -1, "nothing visible, nothing picked, no crash");
    // --- no drift: identical inputs, identical outputs, many frames
    P2 first[8], again[8]; ProjectCorners(a, project, first); bool same = true;
    for (int f = 0; f < 1000; ++f) { ProjectCorners(a, project, again); if (std::memcmp(first, again, sizeof first) != 0) same = false; if (Pick(two, project, 500, 494) != 0) same = false; }
    check(same, "no drift: the same box and camera give the same corners and the same pick every frame");
    Box c = routehazard::Clamped(a); check(c.x == a.x && c.y == a.y && c.z == a.z && c.hx == a.hx && c.hy == a.hy && c.hz == a.hz && c.yaw == a.yaw, "Clamped leaves a valid box exactly as it is");
    check(Base(a) == 0 && Height(a) == 4, "base and height of a box");
    // --- height / base maths
    Box h = a; SetHeight(h, 10); check(Base(h) == 0 && Height(h) == 10 && h.z == 5, "Height: the base stays, the top moves");
    SetBase(h, 3); check(Base(h) == 3 && Height(h) == 10, "Base: the height stays");
    Raise(h, kRaiseStep); check(Base(h) == 4 && Height(h) == 10, "PgUp one step lifts the whole box");
    Raise(h, -kRaiseBig); check(Base(h) == -1, "Shift PgDn lowers it five");
    SetHeight(h, 0); check(Height(h) == 2 * routehazard::kMinHalf, "height never below the minimum");
    // --- two-click placement
    Placing p; check(!p.active, "idle at the start");
    Box out; check(!Commit(p, 5, 5, 3, 4, 0, out), "a second click without a first does nothing");
    Begin(p, 10, 20, 7); check(p.active, "click 1 sets it down");
    check(Commit(p, 10, 30, 3, 4, 0, out) && !p.active, "click 2 finishes and ends the state");
    check(std::fabs(out.x - 10) < 1e-9 && std::fabs(out.y - 25) < 1e-9 && std::fabs(out.hy - 5) < 1e-9 && std::fabs(out.yaw) < 1e-9, "a line due north: 10 m long, centred between, yaw 0");
    check(std::fabs(out.hx - 1.5) < 1e-9 && std::fabs(Base(out) - 7) < 1e-9 && std::fabs(Height(out) - 4) < 1e-9, "across and height as set, base at the surface of click 1");
    Begin(p, 0, 0, 0); Commit(p, 10, 0, 3, 4, 0, out); check(std::fabs(out.yaw - 90) < 1e-9 && std::fabs(out.x - 5) < 1e-9, "a line due east: yaw 90, the along axis points east");
    check(routehazard::Touches(out, 9, 0, 0, 1, 0) && !routehazard::Touches(out, 5, 4, 0, 1, 0), "the east box really lies along the east line");
    Begin(p, 0, 0, 0); Commit(p, -10, -10, 3, 4, 0, out); check(std::fabs(out.yaw - 225) < 1e-9, "south-west is 225 degrees clockwise from north");
    Begin(p, 3, 3, 1); Commit(p, 3.1, 3.1, 3, 4, 45, out); check(out.yaw == 45 && out.hy == routehazard::kDefaultHalfXY && out.x == 3, "a second click on the first: default length, the last heading");
    Begin(p, 3, 3, 1); Cancel(p); check(!p.active && !Commit(p, 9, 9, 3, 4, 0, out), "Escape cancels: the next click starts again and nothing was made");
    Begin(p, 1, 1, 0); Begin(p, 2, 2, 5); check(p.x == 2 && p.base == 5, "a new first click replaces an abandoned one");
    check(routehazard::Valid(FromLine(0, 0, 0, 1e7, 1e7, 3, 4, 0)) == true, "an absurd line is clamped to a valid box");
    // --- bookkeeping after a delete
    std::vector<int> picks{ 0, 2, 3 }; AfterErase(picks, 2);
    check(picks.size() == 2 && picks[0] == 0 && picks[1] == 2, "erasing box 2: it leaves the group, box 3 becomes 2");
    check(AfterEraseOne(3, 1) == 2 && AfterEraseOne(1, 1) == -1 && AfterEraseOne(0, 1) == 0 && AfterEraseOne(-1, 1) == -1, "the single selection follows an erase");
    // --- group move / turn
    Box g; g.x = 10; g.y = 0; g.z = 1; g.yaw = 0; GroupTurn(g, 0, 0, 0, 0, 0, 90);   // 90 clockwise about the origin: east goes to south
    check(std::fabs(g.x) < 1e-9 && std::fabs(g.y + 10) < 1e-9 && std::fabs(g.yaw - 90) < 1e-9, "a clockwise group turn carries the box and its heading");
    Box m; m.x = 1; m.y = 2; m.z = 3; GroupTurn(m, 4, 5, 6, 0, 0, 0); check(m.x == 5 && m.y == 7 && m.z == 9 && m.yaw == 0, "a group move shifts position only");
    std::printf("%d checks, %d failed\n", total, failed);
    return failed ? 1 : 0;
}
