// road_boxes, line_end and line_arrive (2026-11-06): read, write, keep unreadable text, follow points, and leave old routes exactly as before.
#include "routefiles.h"
#include "f7dock.h"
#include <cstdio>
int main() {
    int total = 0, failed = 0;
    auto check = [&](bool good, const char* name) { ++total; if (!good) { ++failed; std::printf("FAIL %s\n", name); } };
    // Boxes: round trip.
    roadplan::Plan p; check(roadplan::ReadBoxes("1 2 0 0 0 0 0 1 0 11.5 6 10 -5 3 63 1", p), "boxes read");
    check(p.boxes.size() == 2 && p.boxes[1].width == 11.5 && p.boxes[1].faces == 63 && p.boxes[1].solids == 1 && p.boxes[1].pitch == -5, "box values");
    check(roadplan::WriteBoxes(p, 2) == "1 2 0 0 0 0 0 1 0 11.5 6 10 -5 3 63 1", "boxes written back the same");
    check(roadplan::WriteBoxes(p, 3) == "1 3 0 0 0 0 0 1 0 11.5 6 10 -5 3 63 1 0 0 0 0 0 1 0", "a missing box writes as the default");
    roadplan::Plan none; check(roadplan::WriteBoxes(none, 4).empty(), "no boxes: no key");
    // Unreadable text is kept whole.
    roadplan::Plan bad; check(!roadplan::ReadBoxes("2 1 0 0 0 0 0 1 0", bad) && bad.boxes.empty() && roadplan::WriteBoxes(bad, 1) == "2 1 0 0 0 0 0 1 0", "future version kept");
    check(!roadplan::ReadBoxes("1 1 0 0 0 0 0 99 0", bad) && roadplan::WriteBoxes(bad, 1) == "1 1 0 0 0 0 0 99 0", "out of range kept");
    check(!roadplan::ReadBoxes("1 2 0 0 0 0 0 1 0", bad), "short list refused");
    // Boxes follow points.
    roadplan::Plan q; roadplan::ReadBoxes("1 3 1 0 0 0 0 1 0 2 0 0 0 0 1 0 3 0 0 0 0 0 0", q);
    roadplan::Insert(q, 1); check(q.boxes.size() == 4 && q.boxes[1].width == 1 && q.boxes[2].width == 2, "insert splits the tile, others shift");
    roadplan::Remove(q, 1); check(q.boxes.size() == 3 && q.boxes[1].width == 2, "remove");
    roadplan::Plan r; roadplan::ReadBoxes("1 3 1 0 0 2 3 5 0 2 0 0 0 0 1 0 3 0 0 0 0 16 0", r);
    roadplan::Reverse(r, 4, false);
    check(r.boxes[0].width == 3 && r.boxes[0].faces == 32 && r.boxes[2].width == 1 && r.boxes[2].pitch == -2 && r.boxes[2].roll == -3 && r.boxes[2].faces == 9, "reverse: tiles reversed, left/right and front/back swap");
    // Line end.
    auto e = routeend::Read("arrive", "10 20 30 90 1");
    check(e.end == routeend::Arrive && e.hasPlace && e.place.z == 30 && e.place.park == 1, "line end read");
    check(routeend::Lines(e) == "line_end = arrive\nline_arrive = 10 20 30 90 1\n", "line end written");
    check(routeend::Lines(routeend::Read("", "")).empty(), "pass: no keys");
    auto u = routeend::Read("hover_land", "1 2"); check(u.end == routeend::Pass && routeend::Lines(u) == "line_end = hover_land\nline_arrive = 1 2\n", "unreadable end kept");
    routeend::Shift(e, 1, 2, 3); check(e.place.x == 11 && e.place.z == 33, "shift");
    // Whole file: an old route reads and writes exactly as before; a new one keeps its keys.
    const std::string old = "[file]\nname = t\n\n[world.ed_w]\nid = W\n\n[route.ed_a]\nlabel = A\nworld = ed_w\ntype = path\npoints = 0 0 0, 100 0 0, 100 100 0\nclosed = 0\nlanes = 1\nspeed = 25\nwidth = 20\n";
    routefiles::File f; std::vector<std::string> problems; check(routefiles::Parse(old, "t.ini", f, problems) && problems.empty(), "old file parses");
    const auto text = routefiles::Format(f);
    check(text.find("road_boxes") == std::string::npos && text.find("line_end") == std::string::npos && text.find("line_arrive") == std::string::npos, "old route gains no key");
    f.routes[0].road.boxes = { roadplan::DefaultBox(false), roadplan::DefaultBox(true) }; f.routes[0].road.boxes[0].solids = 1;
    f.routes[0].end = routeend::Read("stop", "");
    const auto text2 = routefiles::Format(f);
    check(text2.find("road_boxes = 1 2 0 0 0 0 0 1 1 0 0 0 0 0 0 0\n") != std::string::npos, "boxes written to the file");
    check(text2.find("line_end = stop\n") != std::string::npos && text2.find("\r") == std::string::npos, "line end written, LF only");
    routefiles::File g; check(routefiles::Parse(text2, "t.ini", g, problems) && problems.empty() && g.routes[0].road.boxes.size() == 2 && g.routes[0].road.boxes[0].solids == 1 && g.routes[0].end.end == routeend::Stop, "new keys read back");
    check(routefiles::Format(g) == text2, "second write is the same");
    trafficdata::Data d; trafficdata::Read(text2 + "line_arrive = garbage\n", "t.ini", d);
    check(d.routes.size() == 1 && d.problems.empty() && !d.routes[0].end.placeUnsupported.empty(), "a bad new key never skips the route");
    // The dock's selection rules.
    f7dock::Selection s = f7dock::One(f7dock::Tiles, 2);
    check(f7dock::Flip(s, f7dock::Tiles, 5) == f7dock::Added && s.Count() == 2 && s.primary == 5, "ctrl+click adds");
    check(f7dock::Flip(s, f7dock::Tiles, 5) == f7dock::Removed && s.Count() == 1 && s.primary == 2, "ctrl+click removes, primary falls back");
    check(f7dock::Flip(s, f7dock::Points, 1) == f7dock::Switched && s.kind == f7dock::Points && s.Count() == 1, "points and tiles never mix");
    check(f7dock::Flip(s, f7dock::Points, 1) == f7dock::Emptied && s.kind == f7dock::None, "removing the last empties");
    s = f7dock::All(f7dock::Tiles, 7, 3); check(s.Count() == 7 && s.primary == 3, "double-click selects all");
    f7dock::Clamp(s, 4); check(s.Count() == 4 && s.primary == 3, "clamp after points go");
    f7dock::Clamp(s, 2); check(s.Count() == 2 && s.primary == 1, "clamp moves the primary");
    check(f7dock::Typed(250, 193).first == 192 && f7dock::Typed(250, 193).second && !f7dock::Typed(4, 193).second, "typed picker clamps and says so");
    check(f7dock::Step(-1, 1, 5) == 0 && f7dock::Step(-1, -1, 5) == 4 && f7dock::Step(4, 1, 5) == 0, "picker wraps");
    std::vector<f7dock::Screen> sc{ {0, 0, true}, {100, 0, true}, {100, 100, true} };
    check(f7dock::TileAt(sc, false, 50, 4, 10) == 0 && f7dock::TileAt(sc, false, 96, 60, 10) == 1 && f7dock::TileAt(sc, false, 50, 50, 10) == -1, "tile under the click");
    check(f7dock::TileAt(sc, true, 50, 50, 10) == 2, "closed route has the closing tile");
    f7dock::Clicks c; check(!c.Double(1000, 5, 5) && c.Double(1300, 7, 6) && !c.Double(1350, 7, 6), "double-click");
    check(!c.Double(2000, 5, 5) && !c.Double(2600, 5, 5), "slow second click is not double");
    f7dock::Entry en; en.Begin(3, "12.5"); en.Key('7'); double v = 0; check(en.Number(v) && v == 7, "typing replaces");
    en.Key('.'); en.Key('.'); en.Key('5'); check(en.Number(v) && v == 7.5, "one point only");
    check(f7dock::Leading("-12.5 deg") == "-12.5" && f7dock::Leading("mixed").empty(), "leading number");
    std::printf("%d/%d passed\n", total - failed, total);
    return failed ? 1 : 0;
}
