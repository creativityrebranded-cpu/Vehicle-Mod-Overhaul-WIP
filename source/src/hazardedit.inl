// Hazards tool of the F7 route editor (2026-11-21, user: "custom boxes we can build as bounding boxes"). Included by routeedit.cpp inside its anonymous
// namespace after the lane editor. A route's hazard boxes (routehazard.h, key hazard_boxes) are edited through the route draft, so the editor's
// existing Undo, Redo and Save apply to them. The planner and the rail read the saved boxes through hazardfield.h; nothing here creates a game object.
//
// 2026-11-22 (user: "use the ACTUAL MOD ... select doesn't even select it"): a Hazard is an item of the editor's own selection / edit system, like a route point
// (notes/F7_HAZARD_INTEGRATION_20261122.md). Select (Q) picks it, Move (R) drags it, PgUp / PgDn and the wheel near it set its height, Del removes it, Ctrl+Z / Ctrl+Y / Save
// are the draft's, Ctrl+click groups it, the dock's step rows type its sizes. This file keeps only: the Hazard PLACE tool, the pure-maths glue (hazardpick.h) and drawing.
//
//   Place tool     click 1 sets it down on the surface under the cursor, click 2 sets length and direction (the line from click 1 to the cursor), Esc cancels
//   Select / Move  the editor's own: ClickMap, DockClick, PickClick (Ctrl), StartPlaceDrag (placeKind 4), ChangeZ, MoveXY, HubMove, DeleteSelected, GroupDelete, ApplyGroup
//   Ctrl + click   (Place tool, after Copy object) copy the shape of the game object under the cursor (its kept collision box) into a new Hazard
//   Suggestions    amber boxes from Test route: click one with Select, then Keep or Dismiss in the dock (Del dismisses); only kept ones are saved
constexpr unsigned kHazardColor = 0xFFD27C, kHazardPick = 0xFFE15A, kHazardSuggest = 0xF2B04E;
void HazardSay(const routehazard::Box& b, const char* what);

routehazard::Plan HazardsNow(std::string* name = nullptr)
{
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    if (name) *name = r ? r->name : std::string();
    return r ? r->hazards : routehazard::Plan{};
}
bool HazardOn() { const auto p = HazardsNow(); return g_ed.hazard >= 0 && g_ed.hazard < int(p.boxes.size()); }
std::vector<hazardfield::Suggestion> HazardSuggestions() { std::string name; HazardsNow(&name); return name.empty() ? std::vector<hazardfield::Suggestion>{} : hazardfield::Suggestions(name); }
bool HazardLocked()
{
    const auto p = HazardsNow();
    if (!p.unsupported.empty()) { Status("This route has hazard boxes this version cannot read: they are kept as they are and cannot be edited here"); return true; }
    return false;
}
// Edit the draft's boxes. step: a new undo step (one per click, drag or wheel notch); else amend the current one.
template <class F> bool HazardEdit(bool step, F&& change)
{
    if (!EnsureDraft() || HazardLocked()) return false;
    auto r = g_ed.draft->Value();
    change(r.hazards);
    for (auto& b : r.hazards.boxes) b = routehazard::Clamped(b);
    if (step) g_ed.draft->Edit(r); else g_ed.draft->Amend(r);
    return true;
}
// Picking and drawing use the SAME projection of the box's eight corners (hazardpick.h); a corner that cannot project is skipped, the box is not dropped.
auto HazardProject(const routemap::Camera& c) { return [&c](double x, double y, double z, double& sx, double& sy) { return Local(c, { x, y, z }, sx, sy); }; }
// Does the click touch this box's footprint on the screen (within `slack` pixels)?
bool HazardNearBox(const routemap::Camera& c, const routehazard::Box& b, double mx, double my, double slack)
{
    return hazardpick::Pick(std::vector<routehazard::Box>{ b }, HazardProject(c), mx, my, slack) == 0;
}
// The Hazard of the shown route under the cursor: inside or within 10 px of its screen footprint; the smallest one wins when several hold the click.
int HazardAt(const routemap::Camera& c, double mx, double my)
{
    return hazardpick::Pick(HazardsNow().boxes, HazardProject(c), mx, my);
}
int HazardSuggestAt(const routemap::Camera& c, double mx, double my)
{
    const auto list = HazardSuggestions();
    for (std::size_t i = 0; i < list.size(); ++i) if (HazardNearBox(c, list[i].box, mx, my, hazardpick::kPickSlack)) return int(i);
    return -1;
}
// The Select tool: the Hazard (or, failing that, the test suggestion) under the cursor becomes the one selected item, like a point. True when one was taken.
bool HazardSelectAt(const routemap::Camera& c, double mx, double my)
{
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    if (!r || r->ring) return false;
    if (const int h = HazardAt(c, mx, my); h >= 0) {
        g_ed.hazard = h; g_ed.suggest = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1; g_ed.lanePicks.clear();
        logger::Write("OBS routeedit.hazard event=select index=%d via=select_tool", h);
        HazardSay(r->hazards.boxes[std::size_t(h)], "selected");
        return true;
    }
    if (const int s = HazardSuggestAt(c, mx, my); s >= 0) {
        g_ed.suggest = s; g_ed.hazard = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1; g_ed.lanePicks.clear();
        Status("Suggested Hazard: Keep saves it with the route, Dismiss (or Del) drops it");
        return true;
    }
    return false;
}
// "At the route's height": the selected point's height, else the first point's, else the map focus. Only the fallback of the Place tool when no surface is under the cursor.
double HazardHeight(const routemap::Camera& c)
{
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    if (r && !r->ring && !r->points.empty()) return r->points[std::size_t(g_ed.point >= 0 && g_ed.point < int(r->points.size()) ? g_ed.point : 0)].z;
    return c.focus.z;
}
// The kept collision box of the game object nearest the map point (x, y) near height z: its real size and heading.
bool HazardObjectAt(double x, double y, double z, routehazard::Box& out)
{
    navmap::KeptBox kb[12]; const int n = navmap::BoxesNear(x, y, 3.0, kb, 12); int best = -1; double bestD = 1e18;
    for (int i = 0; i < n; ++i) {
        const auto& b = kb[i]; if (z < b.zLo - 3.0 || z > b.zHi + 3.0) continue;
        const double dx = x - b.x, dy = y - b.y, c = std::cos(b.yaw), s = std::sin(b.yaw), lx = dx * c - dy * s, ly = dx * s + dy * c;
        const double ex = std::max(0.0, std::fabs(lx) - b.hx), ey = std::max(0.0, std::fabs(ly) - b.hy), d = std::hypot(ex, ey);
        if (d < bestD) { bestD = d; best = i; }
    }
    if (best < 0 || bestD > 1.5) return false;
    const auto& b = kb[best];
    out.x = b.x; out.y = b.y; out.z = .5 * (b.zLo + b.zHi); out.hx = std::max(routehazard::kMinHalf, b.hx); out.hy = std::max(routehazard::kMinHalf, b.hy);
    out.hz = std::max(routehazard::kMinHalf, .5 * (b.zHi - b.zLo)); out.yaw = b.yaw * 180.0 / routehazard::kPi;
    return routehazard::Valid(routehazard::Clamped(out));
}
void HazardSay(const routehazard::Box& b, const char* what)
{
    char t[200]; std::snprintf(t, sizeof t, "Hazard box %s: %.1f x %.1f m across and along, %.1f m tall, base %.0f m, facing %.0f deg (Save keeps it)", what, 2 * b.hx, 2 * b.hy, 2 * b.hz, b.z - b.hz, b.yaw);
    Status(t);
}
// A click in the Hazard PLACE tool (Mode::Hazard); selecting and moving are the editor's own (Select, Move). True when it was taken.
//   click 1: the box is set down: the first surface under the cursor, exactly like a route point (DropPoint, PL-1), the flat plane at the route's height only without one;
//   click 2: the line from click 1 to the cursor is its length and direction (on the plane through click 1's base, so what the preview shows is what is made). Esc cancels.
// Copy object (the dock button) makes the next click copy the shape of the game object under the cursor instead.
constexpr double kHazardBelowSurface = 1.0;  // a new box reaches 1 m below the surface it was set on (so a box on a road covers the road), its Height above that
bool HazardClick(const routemap::Camera& c, double mx, double my)
{
    (void)mx; (void)my;
    if (g_ed.hazardLine.active && !g_ed.hazardCopy) {   // click 2
        routemap::Vec3 at;
        if (!routemap::MouseAt(c, g_ed.hazardLine.base + kHazardBelowSurface, at)) { Status("The cursor's ray does not reach the box's height; move the cursor or the camera. Esc cancels."); return true; }
        routehazard::Box made;
        if (!hazardpick::Commit(g_ed.hazardLine, at.x, at.y, g_ed.hazardAcross, g_ed.hazardTall, g_ed.hazardYaw, made)) return true;
        if (HazardEdit(true, [&](routehazard::Plan& p) { if (p.boxes.size() < routehazard::kMaxBoxes) p.boxes.push_back(made); })) {
            g_ed.hazard = int(HazardsNow().boxes.size()) - 1; g_ed.suggest = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1; g_ed.hazardYaw = made.yaw;
            logger::Write("OBS routeedit.hazard event=placed index=%d x=%.1f y=%.1f base=%.1f along=%.1f across=%.1f height=%.1f yaw=%.0f", g_ed.hazard, made.x, made.y, made.z - made.hz, 2 * made.hy, 2 * made.hx, 2 * made.hz, made.yaw);
            HazardSay(made, "placed");
        }
        return true;
    }
    // The SAME surface as a route point (DropPoint, PL-1): the first surface under the cursor (the deck, the ground, a roof), and only without
    // one the flat plane at the route's height (2026-11-21, the user: "it should be added with the same mechanism as a route point").
    routemap::Vec3 at; double height = HazardHeight(c);
    {   routemap::Vec3 hp; const int got = CursorSurface(c, hp);
        logger::Write("OBS routeedit.place event=hazard_surface_under_cursor hit=%d at=%.1f,%.1f,%.1f reason=%s", got, got == 1 ? hp.x : 0.0, got == 1 ? hp.y : 0.0, got == 1 ? hp.z : 0.0,
            got == 1 ? "placed_on_the_surface_the_mouse_points_at" : got == 0 ? "nothing_under_the_cursor_plane_placement" : "no_ray_plane_placement");
        if (got == 1) { at = hp; height = hp.z; }
        else if (!routemap::MouseAt(c, height, at)) { Status("The cursor's ray does not reach that height; move the cursor or the camera"); return true; } }
    if (g_ed.hazardCopy) {
        routehazard::Box object;
        if (!HazardObjectAt(at.x, at.y, height, object)) { Status("No game object box kept here: fly nearer so the map has seen it, or place a Hazard by hand"); return true; }
        g_ed.hazardCopy = false;
        if (HazardEdit(true, [&](routehazard::Plan& p) { if (p.boxes.size() < routehazard::kMaxBoxes) p.boxes.push_back(object); })) {
            g_ed.hazard = int(HazardsNow().boxes.size()) - 1; g_ed.suggest = -1; g_ed.point = -1; HazardSay(object, "copied from that object");
        }
        return true;
    }
    if (HazardsNow().boxes.size() >= routehazard::kMaxBoxes) { Status("A route holds up to 64 Hazards"); return true; }
    hazardpick::Begin(g_ed.hazardLine, at.x, at.y, height - kHazardBelowSurface);
    Status("Hazard set down: click again to set its length and direction (the line from here to the cursor); Esc cancels");
    return true;
}
// Esc in the Hazard dock level: the pending first click, then the selection, then the tool. True when it did one of the first two (the level stays).
bool HazardEscape()
{
    if (g_ed.hazardLine.active) { hazardpick::Cancel(g_ed.hazardLine); Status("Hazard placement cancelled"); return true; }
    if (g_ed.hazardCopy) { g_ed.hazardCopy = false; Status("Copy object cancelled"); return true; }
    if (g_ed.hazard >= 0 || g_ed.suggest >= 0) { g_ed.hazard = -1; g_ed.suggest = -1; g_ed.picks.clear(); Status("Hazard deselected"); return g_ed.mode == Mode::Hazard; }
    return false;
}
// The wheel near the selected Hazard (its screen footprint, 60 px of slack): it sets the height there, as it does near a selected point.
bool HazardWheelNear(const routemap::Camera& c, double mx, double my)
{
    if (!HazardOn() || Many()) return false;
    return HazardNearBox(c, HazardsNow().boxes[std::size_t(g_ed.hazard)], mx, my, 60);
}
// PgUp / PgDn, [ ], the wheel near it: the selected Hazard moves up or down (1 m, Shift 5 m: the editor's 5 / 25 m steps are too coarse for a box; same 1 : 5 shape).
// Several selected go through the editor's group step (Many()), not here.
bool HazardZ(double dz)
{
    if (!HazardOn() || Many()) return false;
    const int i = g_ed.hazard; const double step = dz * (hazardpick::kRaiseStep / 5.0);
    if (HazardEdit(true, [&](routehazard::Plan& p) { hazardpick::Raise(p.boxes[std::size_t(i)], step); })) HazardSay(HazardsNow().boxes[std::size_t(i)], "moved up or down");
    return true;
}
// The editor's nudges (left / right / forward / back buttons, I J K L): one undo step each. Alt + WASD / Q E: HazardNudge, amended into the burst's one step.
bool HazardMove(double dx, double dy, double dz)
{
    if (!HazardOn() || Many()) return false;
    const int i = g_ed.hazard;
    if (HazardEdit(true, [&](routehazard::Plan& p) { auto& b = p.boxes[std::size_t(i)]; b.x += dx; b.y += dy; b.z += dz; })) HazardSay(HazardsNow().boxes[std::size_t(i)], "moved");
    return true;
}
void HazardNudge(double dx, double dy, double dz)
{
    if (!HazardOn()) return;
    const int i = g_ed.hazard;
    HazardEdit(false, [&](routehazard::Plan& p) { auto& b = p.boxes[std::size_t(i)]; b.x += dx; b.y += dy; b.z += dz; });
}
void HazardTurn(double clockwise)
{
    if (!HazardOn()) return;
    const int i = g_ed.hazard;
    if (HazardEdit(true, [&](routehazard::Plan& p) { p.boxes[std::size_t(i)].yaw += clockwise; })) HazardSay(HazardsNow().boxes[std::size_t(i)], "turned");
}
// Del: the selected Hazard goes through the draft (Ctrl+Z brings it back, Save keeps the route without it); a selected test suggestion is dismissed.
bool HazardDelete()
{
    std::string name;
    if (g_ed.suggest >= 0 && !HazardOn()) { HazardsNow(&name); hazardfield::Dismiss(name, std::size_t(g_ed.suggest)); g_ed.suggest = -1; Status("Suggestion dismissed"); return true; }
    if (!HazardOn()) return false;
    const int i = g_ed.hazard;
    if (HazardEdit(true, [&](routehazard::Plan& p) { p.boxes.erase(p.boxes.begin() + i); })) { g_ed.hazard = hazardpick::AfterEraseOne(g_ed.hazard, i); g_ed.picks.clear(); logger::Write("OBS routeedit.hazard event=deleted index=%d", i); Status("Hazard deleted (Ctrl+Z brings it back; Save keeps the route without it)"); }
    return true;
}
// Keep a suggestion: it becomes a box of the route (and can then be edited); it is saved with the route's next Save.
bool HazardKeep(std::size_t index)
{
    std::string name; HazardsNow(&name); const auto list = HazardSuggestions();
    if (index >= list.size()) return false;
    const auto box = list[index].box;
    if (!HazardEdit(true, [&](routehazard::Plan& p) { if (p.boxes.size() < routehazard::kMaxBoxes) p.boxes.push_back(box); })) return false;
    hazardfield::Dismiss(name, index); g_ed.suggest = -1; g_ed.hazard = int(HazardsNow().boxes.size()) - 1;
    HazardSay(box, "kept from the suggestion");
    return true;
}
// A stepper or button of the tool. True when the id was one of ours.
bool HazardPress(int id, const routemap::Camera& c)
{
    (void)c;
    // Sizes are not here any more: Across / Along / Height / Base / Turn are the dock's own step rows (FHz*, DockApply: typed or - / +); the old stepper ids are gone with their cases.
    switch (id) {
    case kHazardTool: case kHazardPlace:  // the Place tool, toggled like Add: on = Mode::Hazard, off = Select
        if (g_ed.mode == Mode::Hazard) { g_ed.mode = Mode::Select; hazardpick::Cancel(g_ed.hazardLine); g_ed.hazardCopy = false; Status("Place off: Select (Q) picks a Hazard, Move (R) drags it"); return true; }
        g_ed.mode = Mode::Hazard; hazardpick::Cancel(g_ed.hazardLine); g_ed.hazardCopy = false; g_ed.hazard = -1; g_ed.suggest = -1;
        Status("Place Hazard: click the surface to set it down, click again to set its length and direction (Esc cancels, Insert clicks at the cursor)"); return true;
    case kHazardDone: g_ed.hazard = -1; g_ed.suggest = -1; g_ed.hazardCopy = false; hazardpick::Cancel(g_ed.hazardLine); g_ed.picks.clear(); if (g_ed.mode == Mode::Hazard) g_ed.mode = Mode::Select; Status("Hazard done. Save keeps your changes."); return true;
    case kHazardDelete: if (!HazardDelete()) Status("Select a Hazard first (Q, then click it)"); return true;
    case kHazardInherit: g_ed.mode = Mode::Hazard; hazardpick::Cancel(g_ed.hazardLine); g_ed.hazardCopy = true; Status("Click a game object on the map: the Hazard takes its size and heading (Esc cancels)"); return true;
    case kHazardKeep: if (g_ed.suggest >= 0) HazardKeep(std::size_t(g_ed.suggest)); else Status("Select a suggested box first (Q, then click an amber box)"); return true;
    case kHazardDismiss: if (!HazardDelete()) Status("Select a suggested box first (Q, then click an amber box)"); return true;
    case kHazardKeepAll: { int n = 0; while (!HazardSuggestions().empty() && HazardKeep(0)) ++n; Status(std::to_string(n) + " suggested box(es) kept (Save keeps them)"); return true; }
    case kHazardNextSuggest: { const auto l = HazardSuggestions(); if (l.empty()) Status("No suggestions: run Test route (T) and watch the map"); else { g_ed.suggest = (g_ed.suggest + 1) % int(l.size()); g_ed.hazard = -1; } return true; }
    default: return false;
    }
}
// ---- drawing: yellow wire boxes with a direction arrow; amber for suggestions; the aim ghost ----
void HazardDrawBox(Pen& pen, const routemap::Camera& c, const routehazard::Box& b, unsigned colour, double width, double fill, bool arrow)
{
    // The eight corners are projected one by one: an edge is drawn when BOTH its ends project, so a box partly behind the camera or off the view still shows what is visible
    // (it used to disappear whole when any corner failed, which read as a flicker at the screen edge). The same projection picks it (HazardAt).
    double c3[8][3]; hazardpick::Corners3(b, c3);
    double sx[8], sy[8]; bool ok[8];
    for (int i = 0; i < 8; ++i) ok[i] = Local(c, { c3[i][0], c3[i][1], c3[i][2] }, sx[i], sy[i]);
    auto edge = [&](int a, int d) { if (ok[a] && ok[d]) { pen.Move(sx[a], sy[a]); pen.Line(sx[d], sy[d]); } };
    if (fill > 0 && ok[4] && ok[5] && ok[6] && ok[7]) { pen.NoLine(); pen.Fill(colour, fill); pen.Move(sx[4], sy[4]); pen.Line(sx[5], sy[5]); pen.Line(sx[6], sy[6]); pen.Line(sx[7], sy[7]); pen.Line(sx[4], sy[4]); pen.End(); }
    pen.Style(width, colour, .95);
    for (int i = 0; i < 4; ++i) { edge(i, (i + 1) % 4); edge(i + 4, (i + 1) % 4 + 4); edge(i, i + 4); }  // bottom ring, top ring, the four uprights
    pen.NoLine();
    if (!arrow) return;
    const double r = b.yaw * routehazard::kPi / 180.0, s = std::sin(r), k = std::cos(r), z = b.z + b.hz;
    double x0, y0, x1, y1, xl, yl, xr, yr; const double tip = b.hy + 1.5, head = .6;
    if (!Local(c, { b.x, b.y, z }, x0, y0) || !Local(c, { b.x + s * tip, b.y + k * tip, z }, x1, y1)) return;
    if (!Local(c, { b.x + s * (tip - head) - k * head, b.y + k * (tip - head) + s * head, z }, xl, yl) || !Local(c, { b.x + s * (tip - head) + k * head, b.y + k * (tip - head) - s * head, z }, xr, yr)) return;
    pen.Style(width + 1, colour, 1.0); pen.Move(x0, y0); pen.Line(x1, y1); pen.Move(xl, yl); pen.Line(x1, y1); pen.Line(xr, yr); pen.NoLine();
}
// Always (not only in the tool): the shown route's Hazards, the selected one (Select / Move / the tool) highlighted with its direction arrow, picked ones of a group too; amber test
// suggestions; and, in the Place tool, what the next click makes (a pure function of the cursor and the first click: the same inputs draw the same box every frame).
void HazardDraw(Pen& pen, const routemap::Camera& c)
{
    std::string key; const auto plan = HazardsNow(&key);   // once per frame
    for (std::size_t i = 0; i < plan.boxes.size(); ++i) {
        const bool sel = int(i) == g_ed.hazard || IsPicked(kPickHazard, key, int(i));
        HazardDrawBox(pen, c, plan.boxes[i], sel ? kHazardPick : kHazardColor, sel ? 3.0 : 1.8, sel ? .30 : .16, sel || g_ed.mode == Mode::Hazard);
    }
    const auto list = key.empty() ? std::vector<hazardfield::Suggestion>{} : hazardfield::Suggestions(key);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const bool sel = int(i) == g_ed.suggest;
        HazardDrawBox(pen, c, list[i].box, kHazardSuggest, sel ? 3.0 : 1.6, sel ? .30 : .10, true);
        double x, y; if (Local(c, { list[i].box.x, list[i].box.y, list[i].box.z + list[i].box.hz }, x, y)) MapNote(sel ? "Suggested: Keep / Dismiss in the dock, Del dismisses" : "Suggested (seen on test)", x + 8, y - 16, kHazardSuggest, 260);
    }
    if (g_ed.mode != Mode::Hazard) { g_ed.hazardLine = hazardpick::Placing{}; return; }
    double lx = 0, ly = 0;
    if (!CursorLocal(c, lx, ly) || OverUi(lx, ly)) return;
    routemap::Vec3 at;
    if (g_ed.hazardLine.active && !g_ed.hazardCopy) {   // after click 1: the box the second click would make, on the plane through click 1's base
        if (!routemap::MouseAt(c, g_ed.hazardLine.base + kHazardBelowSurface, at)) return;
        const auto ghost = hazardpick::FromLine(g_ed.hazardLine.x, g_ed.hazardLine.y, g_ed.hazardLine.base, at.x, at.y, g_ed.hazardAcross, g_ed.hazardTall, g_ed.hazardYaw);
        HazardDrawBox(pen, c, ghost, 0xFFFFFF, 2.0, .12, true);
        MapNote("Click to set length and direction; Esc cancels", lx + 14, ly + 14, 0xFFFFFF, 340);
        return;
    }
    // Before click 1: where it would be set down, from the SAME ray the click casts (the cached answer, at most 250 ms old), else the plane at the route's height.
    AskCursor(c);
    double height = HazardHeight(c); routemap::Vec3 hp;
    const bool surface = CursorAnswer(hp);
    if (surface) { at = hp; height = hp.z; } else if (!routemap::MouseAt(c, height, at)) return;
    routehazard::Box ghost = hazardpick::FromLine(at.x, at.y, height - kHazardBelowSurface, at.x, at.y, g_ed.hazardAcross, g_ed.hazardTall, g_ed.hazardYaw);
    HazardDrawBox(pen, c, ghost, g_ed.hazardCopy ? kHazardSuggest : 0xFFFFFF, 1.4, .08, true);
    MapNote(g_ed.hazardCopy ? "Click an object to copy its shape" : "Click to set it down", lx + 14, ly + 14, 0xFFFFFF, 300);
}
