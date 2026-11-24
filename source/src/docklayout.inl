// The F7 dock (2026-11-06): the in-game Traffic Manager built from the approved HTML design, notes/traffic-editor-html/ (Show = Proposal:
// main dock; NOTES.md lists every level and control). Included by routeedit.cpp after contextedit.inl; pure parts in f7dock.h.
// Levels: Routes > Route > Tile / Point, plus Types, Arrive and City. Breadcrumbs go up; cards, the Point / Tile pickers, Types, Open and
// Arrive go down, and every one of those carries the navigation look (teal frame and a teal tab with an arrow, Button::nav). One save
// column: Test, Update Route, Cancel, Save, Permanent Save, Permanent Delete, then Delete point / Delete route. Every value with - and + also takes a
// typed number (click it, type, Enter; Tab moves on; Esc cancels).
// Selection (user, 2026-11-06): click a point = that point; click a line = the tile under the click; double-click a line = every tile;
// double-click a point = every point; Ctrl+click adds or removes one. Edits apply to every selected item. Selected points also go into
// g_ed.picks, so the existing group move, height keys and delete work on all of them unchanged.
// Field boxes and line endings are written as road_boxes / line_end / line_arrive (roadplan.h, routeend.h). D11 is NOT approved: a stored
// rounded, tube or trapezoid tile is never shown as a box; its cube says so and its faces only start once someone sets them.
// Every path traces OBS system=f7.dock with a reason. Reverse, Copy, Mirror, Colour and Readme stay out (user, 2026-11-06).
// 2026-11-08 restore (notes/f7-restore-20261108/CHANGE_CARDS.md, user: "I need everything in places built, then problem list and readme for
// each of the headings, and nodes and route extras too"): Places (landings with access and wires, zones, shapes, junctions), the Nodes
// view, Route extras (Reverse, Copy, Mirror and Colour come back here, at the user's new request), a Readme per menu (the only side box),
// Problems grouped in the bar, bars sized to their content. Every control is an existing editor action; the dock only shows it.
// Editor only: the traffic runtime still reads no landing, zone or junction (stripped 2026-11-05; turning that on is the user's call).

constexpr int kDockLevelBase = 20000;  // + f7dock::Level: breadcrumbs and the City / Types / Arrive doors
constexpr int kDockNavPointPrev = 20010, kDockNavPointNext = 20011, kDockNavTilePrev = 20012, kDockNavTileNext = 20013, kDockPickPoint = 20014, kDockPickTile = 20015;
constexpr int kDockSelOne = 20020, kDockSelAllPoints = 20021, kDockSelAllTiles = 20022, kDockTest = 20030, kDockCardPrev = 20031, kDockCardNext = 20032, kDockUpdateRoute = 20033;
constexpr int kDockTravel = 20040, kDockEnd = 20043, kDockAlong = 20046, kDockAgainst = 20047;
constexpr int kDockOneWay = 20048, kDockTwoWay = 20049;
constexpr int kDockRouteGroup = 20090, kDockRouteGroups = 5; constexpr double kDockTabH = 48;  // 2026-11-11: the Route menu's group tabs (20090-20094)  // 2026-11-11: the Lanes group's one-lane / two-lane choice (sets the lane count)
constexpr int kDockTileTravel = 20050, kDockSolids = 20053, kDockJoin = 20055, kDockSharp = 20060, kDockRound = 20061, kDockPark = 20070;
constexpr int kDockCityOn = 20080, kDockCityOff = 20081, kDockOrigOn = 20082, kDockOrigOff = 20083, kDockAllowAll = 20099, kDockType = 20100;
// Cards RI-3, HV-4 (routeitems.inl, included after this file): the Problems level, its actions and the drive preview.
bool ItemsAction(int id); void ItemsLayout(const trafficdata::Route& r, editorcontext::Rect d, double top, double area); void ItemsStartPreview(const trafficdata::Route& r); int ItemsWarningCount(); void ItemsShowPreview();
constexpr int kDockHealthAllow = 20098;  // card FR1: allow every model Health left out on this Types list
// Cards R2-R6 (2026-11-08). Levels from Places up use their own door ids (kDockLevelBase + 10.. would be the pickers' ids).
constexpr int kDockGoBase = 22000;  // + f7dock::Level, for levels >= Places
inline int DockLevelId(int level) { return level < f7dock::Places ? kDockLevelBase + level : kDockGoBase + level; }
inline int DockIdLevel(int id) { if (id >= kDockLevelBase && id < kDockLevelBase + f7dock::Places) return id - kDockLevelBase; if (id >= kDockGoBase + f7dock::Places && id < kDockGoBase + f7dock::LevelCount) return id - kDockGoBase; return -1; }
constexpr int kDockPlaceTool = 22100, kDockReadme = 22101, kDockHelp = 22102, kDockScript = 22103, kDockPlaceNext = 22110;  // kDockScript: Script mode toggle (2026-11-22)  // + 0 landing, 1 zone, 2 junction
constexpr int kDockProbView = 22120, kDockProbFilter = 22130, kDockFixSafe = 22140;  // view + 0 grouped, 1 items; filter + 0..3
constexpr int kDockNodeMake = 22150, kDockNodeClear = 22151, kDockNodeRemove = 22152, kDockWireAddIn = 22160, kDockWireAddOut = 22161;
constexpr int kDockProbShow = 22200, kDockProbFix = 22300, kDockWire = 22400;  // + problem row (< 100); + wire * 4 + op (< 64 wires)
constexpr int kDockLookSpacing = 22500, kDockLookPace = 22510;  // Design-by-Look cinematic presets (trafficlook.h)
constexpr int kDockProbTile = 23000;  // + tile: a warning tile's row opens that tile
// In nodeview.inl (included after this file): the Nodes view and the landing wires, read and pressed from the bar.
int F7WireButton(int wire, int op); int NodePickCount(); void NodePicksClear(); std::string NodeWireText(); bool NodeViewDelete(); bool MakeRouteFromPicks();
// Card FR1 (reader): Health's verdict on a type, from FVO_Traffic.dll. Empty: no verdict (or an older addon). Health stays the judge.
inline std::string DockHealth(const VehicleType& type, bool* rejected = nullptr, std::vector<unsigned>* ids = nullptr) {
    static const auto query = [] { const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); return addon ? reinterpret_cast<FvoTrafficHealthModelQueryV1>(GetProcAddress(addon, "FVO_Traffic_HealthModelV1")) : nullptr; }();
    if (!query) return {};
    for (unsigned id : type.ids) { FvoTrafficHealthModelV1 r{}; if (!query(id, &r) || !r.rejected) continue;
        if (rejected) *rejected = !r.allowed; if (ids) ids->push_back(id);
        char b[96]; std::snprintf(b, sizeof b, r.allowed ? "Health: %s %.0f of %.0f (you allow it)" : "Health: %s %.0f of %.0f, left out", r.stageName, r.pass, r.pass + r.fail); return b; }
    return {};
}
constexpr int kDockField = 20400;  // + field * 4: +0 the typed box, +1 minus, +2 plus
constexpr int kDockPickerEntry = 1000;  // entry.field for the pickers: + f7dock::Point / Tile
enum DockField { FSpeed, FRoadWidth, FMedian, FLanes, FLane, FTileWidth, FTileHeight, FYaw, FPitch, FRoll, FTileSpeed, FDivider, FSide, FSwing,
    FPZ, FPX, FPY, FRadius, FAX, FAY, FAZ, FAH, FDensity, FLimit, FEdgeLeft, FEdgeRight,
    FHzAcross, FHzAlong, FHzHeight, FHzBase, FHzTurn,  // the selected Hazard's numbers, typed or stepped like Width and Divider
    FTrafficBand, FFieldCount = FTrafficBand + routeschedule::LevelCount * 2 };
constexpr double kDockStep[FFieldCount]{ 1, .5, .5, 1, 1, .5, .5, 5, 5, 5, 5, 1, 5, 5, 5, 5, 5, 5, 1, 1, 1, 5, 10, 1, 1, 1, .25, .25, .5, .5, 5, 1, 1, 1, 1 };
inline bool TileField(int f) { return f >= FTileWidth && f <= FSwing && f != FDivider; }
inline bool PointField(int f) { return f >= FPZ && f <= FRadius; }
struct DockState {
    bool on = true;
    int level = f7dock::Routes;
    f7dock::Selection points, tiles;
    f7dock::Clicks clicks;
    f7dock::Entry entry;
    std::string routeKey;
    int seenPoint = -1, cardFirst = 0;
    double az = 1.2, el = .62;                       // the cube's view
    int cubeDrag = 0, dragFace = -1; double dragX = 0, dragY = 0, dragAz = 0, dragEl = 0; bool dragMoved = false, dragRotate = false; roadplan::FieldBox dragBox;
    std::vector<std::pair<int, std::vector<std::pair<double, double>>>> faces;  // the cube's faces on screen, in draw order
    std::vector<int> visibleFields;                  // Tab order
    std::vector<std::string> shownValues;            // per field, what the box shows (start of an entry)
    editorcontext::Rect rect{};
    // Cards R5, R6: the open menu's Readme; the Problems view, its filter, the row shown on the map, and the list (twice a second).
    bool readme = false, help = false;  // Readme: what each section is; Help: what each field does (2026-11-19)
    int routeGroup = 0;  // 2026-11-11: the Route menu's open group (Driving, Road, Lanes, Traffic, Extras)
    int probView = 0, probFilter = 0, probShown = -1;
    std::vector<Problem> problems; std::string probKey; ULONGLONG probAt = 0;
    editorcontext::Rect readmeRect{};
};
DockState g_dock;
void DockTrace(const char* event, const char* reason, int a = -1, int b = -1) {
    static int told = 0; if (told >= 400) return; ++told;
    logger::Write("OBS system=f7.dock event=%s level=%s points=%d tiles=%d a=%d b=%d reason=%s", event, f7dock::LevelName(g_dock.level), g_dock.points.Count(), g_dock.tiles.Count(), a, b, reason);
}
bool DockTyping() { return g_dock.on && g_dock.entry.Active(); }

// ---- the route shown and its numbers ----
// By name: a saved route that becomes a draft on its first edit (or an original that becomes your copy) is the same route.
std::string DockRouteKey() { trafficdata::Route scratch; const auto* r = Shown(scratch); return r ? r->name : std::string(); }
int DockTiles(const trafficdata::Route& r) { return r.ring ? 0 : f7dock::TileCount(int(r.points.size()), r.closed); }
routepolicy::Travel DockTileTravel(const trafficdata::Route& r, int t) {
    if (t >= 0 && t < int(r.sections.size()) && r.sections[std::size_t(t)].travel != routepolicy::Travel::Inherit) return r.sections[std::size_t(t)].travel;
    return r.travel;
}
bool DockTileHover(const trafficdata::Route& r, int t) { return DockTileTravel(r, t) == routepolicy::Travel::Hover || roadplan::PrimitiveAt(r.road, std::size_t(std::max(0, t))).kind == roadplan::Open; }
double DockRoadWidth(const trafficdata::Route& r) { return r.road.width > 0 ? r.road.width : r.corridor * std::clamp(r.lanes, 1, 2); }
double DockRoadHeight(const trafficdata::Route& r) { return r.road.height > 0 ? r.road.height : 5; }  // the editor's existing roof default
roadplan::FieldBox DockBox(const trafficdata::Route& r, int t) { roadplan::FieldBox b; if (tilebox::StoredBox(r, t, b)) return b; return roadplan::BoxAt(r.road, std::size_t(std::max(0, t)), DockTileHover(r, t)); }
// A stored rounded, tube or trapezoid tile without its own box: D11 (reading it as its enclosing box) is not approved.
bool DockOldShape(const trafficdata::Route& r, int t) {
    if (std::size_t(t) < r.road.boxes.size()) return false;
    const int k = std::size_t(t) < r.road.primitives.size() ? r.road.primitives[std::size_t(t)].kind : roadplan::Default;
    return k == roadplan::Rounded || k == roadplan::Tube || k == roadplan::Trapezoid;
}
// The tile reads the Field Provider's default box (tilebox::StoredBox, flagged isDefault=1 to readers): nothing stored for it yet.
bool DockDefaultBox(const trafficdata::Route& r, int t) { roadplan::FieldBox b; return t >= int(r.road.boxes.size()) && !DockOldShape(r, t) && tilebox::StoredBox(r, t, b); }
bool DockEnsureBoxes(trafficdata::Route& r) {
    if (!r.road.boxesUnsupported.empty()) { Status("This route's field boxes were written by a newer version: kept unchanged, not editable here."); DockTrace("refused", "boxes_unsupported"); return false; }
    for (std::size_t i = r.road.boxes.size(); i < r.points.size(); ++i) { roadplan::FieldBox b; if (!tilebox::StoredBox(r, int(i), b)) b = roadplan::DefaultBox(DockTileHover(r, int(i))); r.road.boxes.push_back(b); }
    return true;
}
int DockLanes(const trafficdata::Route& r) { return RoadLanes(r); }

// ---- selection ----
void DockSyncPicks() {
    if (g_dock.level != f7dock::Hazards) std::erase_if(g_ed.picks, [](const Pick& p) { return p.kind == kPickPoint; });  // at the Hazard level a group of points + Hazards (Ctrl+click) is the editor's own and stays
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    if (g_dock.level == f7dock::Point && g_dock.points.Many() && r) for (int i : g_dock.points.items) g_ed.picks.push_back({ kPickPoint, r->name, i });
    if (g_dock.level == f7dock::Point && g_dock.points.primary >= 0) g_ed.point = g_dock.points.primary;
    else if (g_dock.level != f7dock::Point) g_ed.point = -1;
    g_dock.seenPoint = g_ed.point;
    g_ed.pickPrimary = PrimaryNow();
}
void DockGo(int level, const char* reason) {
    if (level != g_dock.level) DockTrace("level", reason, level);
    // R3: leaving a selected place's menus for anything but another of them deselects it (the editor's own handlers read g_ed.spot /
    // zone / link first, so a stale one would take a route's corner or delete). Leaving Access ends a parking-path draw.
    if (f7dock::PlaceSelected(g_dock.level) && !f7dock::PlaceSelected(level)) {
        if (g_ed.spotKind == 1) g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1;
        DockTrace("place", "deselected_leaving", g_dock.level, level);
    }
    if (g_dock.level == f7dock::Access && level != f7dock::Access && (g_ed.mode == Mode::AccessEntry || g_ed.mode == Mode::AccessExit)) { g_ed.mode = Mode::Select; DockTrace("place", "access_draw_ended"); }
    // Leaving Places for a route menu: the place tools (Land, Zone, Link, Shape) stop taking map clicks; Select takes them again.
    if (f7dock::PlaceLevel(g_dock.level) && !f7dock::PlaceLevel(level) && level != f7dock::Nodes && (g_ed.mode == Mode::Landing || g_ed.mode == Mode::Zone || g_ed.mode == Mode::Connect || (g_ed.mode == Mode::Shape && !g_ed.shapeAt))) { g_ed.mode = Mode::Select; DockTrace("place", "place_tool_ended"); }
    g_dock.level = level; g_dock.entry.End();
    if (level != f7dock::Point) g_dock.points = {};
    if (level != f7dock::Tile) g_dock.tiles = {};
    DockSyncPicks();
}
void DockSelectOne(int kind, int i, const char* reason) {
    if (kind == f7dock::Points) { g_dock.points = f7dock::One(kind, i); DockGo(f7dock::Point, reason); }
    else { g_dock.tiles = f7dock::One(kind, i); DockGo(f7dock::Tile, reason); }
    DockSyncPicks();
}
void DockSelectAll(int kind, const char* reason) {
    trafficdata::Route scratch; const auto* r = Shown(scratch); if (!r || r->ring) return;
    if (kind == f7dock::Points) { const int keep = g_dock.level == f7dock::Point ? g_dock.points.primary : 0; g_dock.level = f7dock::Point; g_dock.tiles = {}; g_dock.points = f7dock::All(kind, int(r->points.size()), keep); }
    else { const int keep = g_dock.level == f7dock::Tile ? g_dock.tiles.primary : 0; g_dock.level = f7dock::Tile; g_dock.points = {}; g_dock.tiles = f7dock::All(kind, DockTiles(*r), keep); }
    g_dock.entry.End(); DockSyncPicks();
    const int n = kind == f7dock::Points ? g_dock.points.Count() : g_dock.tiles.Count();
    Status("All " + std::to_string(n) + (kind == f7dock::Points ? " points" : " tiles") + " of " + (r->label.empty() ? r->name : r->label) + " selected: edits apply to every one");
    DockTrace("select_all", reason, kind, n);
}
void DockFlip(int kind, int i) {
    auto& s = kind == f7dock::Points ? g_dock.points : g_dock.tiles;
    auto& other = kind == f7dock::Points ? g_dock.tiles : g_dock.points;
    const bool had = other.Count() > 0; other = {};
    const auto what = f7dock::Flip(s, kind, i);
    const char* noun = kind == f7dock::Points ? "point" : "tile";
    if (what == f7dock::Emptied) { DockGo(f7dock::Route, "ctrl_click_emptied"); Status("Nothing selected: back to the route"); return; }
    g_dock.level = kind == f7dock::Points ? f7dock::Point : f7dock::Tile; g_dock.entry.End(); DockSyncPicks();
    if (had && (what == f7dock::Started || what == f7dock::Switched)) Status(std::string("Started a ") + noun + " selection: points and tiles are selected apart");
    else Status(std::to_string(s.Count()) + " " + noun + (s.Count() == 1 ? "" : "s") + " selected. Ctrl+click adds or removes one; a plain click selects one.");
    DockTrace("ctrl_click", what == f7dock::Added ? "added" : what == f7dock::Removed ? "removed" : "started", kind, i);
}
// Every frame before input and layout: follow changes made elsewhere (keys, undo, Save, Cancel, a new route, delete).
void DockSync() {
    if (!g_dock.on) return;
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    const auto key = DockRouteKey();
    // R2: the Nodes view has its own menu while it is on (the Nodes tool turns it on and off).
    if (g_f7NodeView) { if (g_dock.level != f7dock::Nodes) DockGo(f7dock::Nodes, "node_view_on"); g_dock.routeKey = key; return; }
    if (g_dock.level == f7dock::Nodes) DockGo(r ? f7dock::Route : f7dock::Routes, "node_view_off");
    // Hazard (2026-11-22): a first-class item. Its selection is one of the editor's primary fields (g_ed.hazard, like g_ed.point): choosing anything else, another route or an index
    // that no longer exists ends it; the dock follows the selection (the Hazard level opens for a selected Hazard or test suggestion, and while the Place tool is on).
    {
        static std::string hazardKey;
        if (key != hazardKey) { hazardKey = key; g_ed.hazard = -1; g_ed.suggest = -1; hazardpick::Cancel(g_ed.hazardLine); g_ed.hazardCopy = false; }
        if (g_ed.hazard >= int(r ? r->hazards.boxes.size() : 0)) g_ed.hazard = -1;
        const bool other = g_ed.point >= 0 || g_ed.spot >= 0 || ZoneOn() || (g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size())) || !g_ed.lanePicks.empty();
        if (other && g_ed.hazard >= 0) { g_ed.hazard = -1; DockTrace("hazard", "deselected_other_selected"); }
        if (other) g_ed.suggest = -1;
        if (g_ed.mode != Mode::Hazard) { hazardpick::Cancel(g_ed.hazardLine); g_ed.hazardCopy = false; }
    }
    const bool hazardTool = g_ed.mode == Mode::Hazard && r && !r->ring, hazardSelected = r && !r->ring && (g_ed.hazard >= 0 || g_ed.suggest >= 0);
    if ((hazardTool || hazardSelected) && g_dock.level != f7dock::Hazards) DockGo(f7dock::Hazards, hazardSelected ? "hazard_selected" : "hazard_tool_on");
    if (g_dock.level == f7dock::Hazards) { if (hazardTool || hazardSelected) { g_dock.routeKey = key; return; } DockGo(r ? f7dock::Route : f7dock::Routes, "hazard_done"); }
    // R3: a selected landing, zone or junction (clicked on the map, placed, or picked by Next) opens its menu; none selected goes back to Places.
    {
        const bool link = g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size());
        const int want = AreaOn() ? f7dock::Landing : ZoneOn() ? f7dock::Zone : link ? f7dock::Junction : -1;
        const bool landingMenu = g_dock.level == f7dock::Landing || g_dock.level == f7dock::Access || g_dock.level == f7dock::Wires;
        if (want >= 0 && !(want == f7dock::Landing && landingMenu) && g_dock.level != want) DockGo(want, "place_selected");
        else if (want < 0 && f7dock::PlaceSelected(g_dock.level)) DockGo(f7dock::Places, "place_cleared");
        if (f7dock::PlaceLevel(g_dock.level)) {
            if (key != g_dock.routeKey) { g_dock.routeKey = key; g_dock.points = {}; g_dock.tiles = {};
                if (g_dock.level == f7dock::Places && g_ed.draft && r) { DockGo(f7dock::Route, "route_made_from_places"); return; } }  // a shape or parallel lanes just made a route
            g_dock.seenPoint = g_ed.point; return;
        }
    }
    if (key != g_dock.routeKey) {
        g_dock.routeKey = key;
        g_dock.points = {}; g_dock.tiles = {}; g_dock.entry.End();
        if (!r) { if (g_dock.level != f7dock::City) DockGo(f7dock::Routes, "no_route"); }
        else if (g_dock.level != f7dock::Route) DockGo(f7dock::Route, "route_changed");
    }
    if (!r) { if (g_dock.level != f7dock::Routes && g_dock.level != f7dock::City) DockGo(f7dock::Routes, "no_route"); return; }
    if (g_dock.level == f7dock::Routes && !g_dock.routeKey.empty() && g_ed.draft) g_dock.level = f7dock::Route;  // a new route opened by N
    f7dock::Clamp(g_dock.points, int(r->points.size())); f7dock::Clamp(g_dock.tiles, DockTiles(*r));
    // A point chosen by other code (Add, Home / End, a drag): the dock follows it.
    if (g_ed.point != g_dock.seenPoint) {
        if (g_ed.point >= 0 && g_ed.point < int(r->points.size())) { if (!g_dock.points.Has(g_ed.point) || g_dock.level != f7dock::Point) { g_dock.points = f7dock::One(f7dock::Points, g_ed.point); g_dock.tiles = {}; g_dock.level = f7dock::Point; DockTrace("level", "point_from_editor", g_ed.point); } else g_dock.points.primary = g_ed.point; }
        else if (g_dock.level == f7dock::Point) DockGo(f7dock::Route, "point_cleared");
        g_dock.seenPoint = g_ed.point;
    }
    if (g_dock.level == f7dock::Point && !g_dock.points.Count()) DockGo(f7dock::Route, "no_point");
    if (g_dock.level == f7dock::Tile && !g_dock.tiles.Count()) DockGo(f7dock::Route, "no_tile");
    if ((g_dock.level == f7dock::Arrive) && r->closed) DockGo(f7dock::Route, "loop_has_no_end");
}

// ---- values ----
double DockValue(int f, const trafficdata::Route& r, int at) {
    const int n = int(r.points.size());
    const auto box = DockBox(r, at);
    const auto tile = roadplan::DividerAt(r.road, std::size_t(std::max(0, g_roadDivider)), std::size_t(std::max(0, at)));
    switch (f) {
    case FHzAcross: case FHzAlong: case FHzHeight: case FHzBase: case FHzTurn: {
        const auto hz = HazardsNow(); if (g_ed.hazard < 0 || g_ed.hazard >= int(hz.boxes.size())) return 0;
        const auto& hb = hz.boxes[std::size_t(g_ed.hazard)];
        return f == FHzAcross ? 2 * hb.hx : f == FHzAlong ? 2 * hb.hy : f == FHzHeight ? 2 * hb.hz : f == FHzBase ? hb.z - hb.hz : hb.yaw; }
    case FSpeed: return r.speed;
    case FRoadWidth: return DockRoadWidth(r); case FMedian: return r.road.median; case FEdgeLeft: return r.road.edgeLeft; case FEdgeRight: return r.road.edgeRight; case FLanes: return DockLanes(r); case FLane: return g_roadLane + 1; case FDivider: return g_roadDivider + 1;
    case FTileWidth: return std::max(box.width > 0 ? box.width : tilebox::RoadWidth(r), tilebox::LaneSpanWidth(r));  // the width the field really has (tilebox.h FW-1: never narrower than its lanes) case FTileHeight:  // width 0 = what the Field Provider uses (tilebox.h) return box.height > 0 ? box.height : DockRoadHeight(r);
    case FYaw: return box.yaw; case FPitch: return box.pitch; case FRoll: return box.roll;
    case FTileSpeed: return at >= 0 && at < int(r.sections.size()) && r.sections[std::size_t(at)].speed > 0 ? r.sections[std::size_t(at)].speed : r.speed;
    case FSide: return tile.side * 100; case FSwing: return tile.swing * 100;
    case FPZ: return at >= 0 && at < n ? r.points[std::size_t(at)].z : 0; case FPX: return at >= 0 && at < n ? r.points[std::size_t(at)].x : 0; case FPY: return at >= 0 && at < n ? r.points[std::size_t(at)].y : 0;
    case FRadius: return at >= 0 && at < int(r.rounding.size()) ? r.rounding[std::size_t(at)] : 0;
    case FAX: return r.end.place.x; case FAY: return r.end.place.y; case FAZ: return r.end.place.z; case FAH: return r.end.place.heading;
    case FDensity: return g_ed.file.city.density; case FLimit: return g_ed.file.city.limit;
    default: return 0;
    }
}
const std::vector<int>& DockItems(int f) { static const std::vector<int> none{ -1 }; return TileField(f) ? g_dock.tiles.items : PointField(f) ? g_dock.points.items : none; }
bool DockMixed(int f, const trafficdata::Route& r) {
    const auto& list = DockItems(f); if (list.size() < 2) return false;
    const double v = DockValue(f, r, list.front());
    return std::any_of(list.begin(), list.end(), [&](int i) { return std::fabs(DockValue(f, r, i) - v) > 1e-9; });
}
// The default place for Arrive: the last point, facing along the last tile.
routeend::Place DockArriveDefault(const trafficdata::Route& r) {
    routeend::Place p; if (r.points.empty()) return p;
    const auto& b = r.points.back(); p.x = b.x; p.y = b.y; p.z = b.z;
    if (r.points.size() >= 2) { const auto& a = r.points[r.points.size() - 2]; p.heading = std::round(std::atan2(b.x - a.x, b.y - a.y) * 180 / 3.141592653589793); if (p.heading < 0) p.heading += 360; }
    return p;
}
// One typed value (delta = false) or one step (delta = true) for every selected item. false: refused, the draft unchanged.
bool DockApply(int f, double v, bool delta) {
    if (f >= FHzAcross && f <= FHzTurn) {   // the selected Hazard's size, base and heading: one typed value or one step, through the editor's own undo
        if (!HazardOn()) { Status("Select a Hazard first (click it on the map)"); return false; }
        const int i = g_ed.hazard; const auto b0 = HazardsNow().boxes[std::size_t(i)];
        const double cur = f == FHzAcross ? 2 * b0.hx : f == FHzAlong ? 2 * b0.hy : f == FHzHeight ? 2 * b0.hz : f == FHzBase ? b0.z - b0.hz : b0.yaw;
        const double next = delta ? cur + v : v;
        const bool ok = HazardEdit(true, [&](routehazard::Plan& p) { auto& b = p.boxes[std::size_t(i)];
            switch (f) {
            case FHzAcross: b.hx = next * .5; break;
            case FHzAlong: b.hy = next * .5; break;
            case FHzHeight: { const double base = b.z - b.hz; b.hz = next * .5; b.z = base + b.hz; break; }  // the base stays, the top moves
            case FHzBase: b.z = next + b.hz; break;
            default: { double yaw = std::fmod(next, 360.0); if (yaw < 0) yaw += 360.0; b.yaw = yaw; break; } } });
        if (ok && HazardOn()) HazardSay(HazardsNow().boxes[std::size_t(g_ed.hazard)], "changed");
        DockTrace(ok ? "field" : "refused", "hazard", int(next * 10)); return ok;
    }
    if (f >= FTrafficBand && f < FFieldCount) {
        trafficdata::Route scratch; const auto* r = Shown(scratch); if (!r) return false;
        const int i = f - FTrafficBand, level = i / 2;
        const bool upper = i % 2 != 0;
        const auto band = r->schedule.trafficBands[level];
        return EditTrafficBand(level, upper, delta ? (upper ? band.high : band.low) + v : v);
    }
    if (f == FLane || f == FDivider) {
        trafficdata::Route scratch; const auto* s = Shown(scratch); if (!s) return false;
        const int lanes = DockLanes(*s), now = f == FLane ? g_roadLane : g_roadDivider, count = f == FLane ? lanes : std::max(1, lanes - 1);
        (f == FLane ? g_roadLane : g_roadDivider) = std::clamp(int(std::lround(delta ? now + v : v - 1)), 0, count - 1);
        DockTrace("field", f == FLane ? "lane" : "divider", (f == FLane ? g_roadLane : g_roadDivider) + 1); return true;
    }
    if (f == FDensity || f == FLimit) {
        auto& c = g_ed.file.city; const double now = f == FDensity ? c.density : c.limit, next = delta ? now + v : v;
        if (f == FDensity) c.density = std::clamp(int(std::lround(next)), 0, 200); else c.limit = std::clamp(int(std::lround(next)), 0, fvolimits::kVehicles);
        g_ed.file.cityWritten = true; Status("City traffic changed: Save keeps it"); DockTrace("field", f == FDensity ? "city_density" : "city_limit", int(next)); return true;
    }
    if (f == FRoadWidth || f == FMedian || f == FLanes || f == FEdgeLeft || f == FEdgeRight) {
        trafficdata::Route scratch; const auto* s = Shown(scratch); if (!s) return false;
        const double next = delta ? DockValue(f, *s, -1) + v : v;
        if (f == FLanes && s->ring) {  // 2026-11-11: a ring has no road tiles; its stored lane field holds one way (1) or two way (2)
            const int want = int(std::lround(next));
            if (want < 1 || want > 2) { Status("A ring route has one or two lanes. For more, draw it as a point route with road tiles."); DockTrace("refused", "ring_lanes", want); return false; }
            if (want == DockLanes(*s)) return true;
            if (!EnsureDraft()) { DockTrace("refused", "no_draft", f); return false; }
            auto t = g_ed.draft->Value(); t.lanes = want; g_ed.draft->Edit(t);
            Status(want == 1 ? "One way: every vehicle runs the same way round (Save keeps it)" : "Two way: a second lane runs the other way (Save keeps it)");
            DockTrace("field", "ring_lanes", want); return true;
        }
        const bool ok = ContextField(f == FRoadWidth ? 15 : f == FMedian ? 17 : f == FEdgeLeft ? 18 : f == FEdgeRight ? 19 : 14, next);
        DockTrace(ok ? "field" : "refused", f == FRoadWidth ? "road_width" : f == FMedian ? "divider_width" : f == FEdgeLeft ? "edge_left" : f == FEdgeRight ? "edge_right" : "lanes", int(next * 10)); return ok;
    }
    if (!EnsureDraft()) { DockTrace("refused", "no_draft", f); return false; }
    auto r = g_ed.draft->Value();
    const int n = int(r.points.size());
    auto value = [&](int at) { return delta ? DockValue(f, r, at) + v : v; };
    switch (f) {
    case FSpeed: r.speed = std::clamp(value(-1), 1.0, 200.0); break;
    case FTileWidth: case FTileHeight: case FYaw: case FPitch: case FRoll: {
        if (f == FTileWidth) {  // the field is never narrower than the lanes it holds (tilebox.h FW-1): a narrower value would change nothing
            const double span = tilebox::LaneSpanWidth(r);
            for (int t : g_dock.tiles.items) { if (t < 0 || t >= n) continue; const double x = value(t);
                if (x > 0 && x < span - 1e-9) { Status("Tile width cannot be narrower than its lanes (" + routefiles::Num(span) + " m). To narrow the road, change Width in Route > Road."); DockTrace("refused", "tile_width_below_lane_span", int(x * 10)); return false; } } }
        if (!DockEnsureBoxes(r)) return false;
        for (int t : g_dock.tiles.items) { if (t < 0 || t >= n) continue; auto& b = r.road.boxes[std::size_t(t)]; const double x = value(t);
            if (f == FTileWidth) b.width = x <= 0 || std::fabs(x - tilebox::RoadWidth(r)) < 1e-9 ? 0 : std::clamp(x, .5, 400.0);  // 0 or the road's own = inherit
            else if (f == FTileHeight) b.height = x <= 0 ? 0 : std::clamp(x, .5, 200.0);
            else (f == FYaw ? b.yaw : f == FPitch ? b.pitch : b.roll) = std::clamp(x, -180.0, 180.0); }
        break;
    }
    case FTileSpeed: {
        r.sections.resize(std::size_t(n));
        for (int t : g_dock.tiles.items) if (t >= 0 && t < n) { const double x = value(t); r.sections[std::size_t(t)].speed = std::fabs(x - r.speed) < 1e-9 ? 0 : std::clamp(x, 1.0, 150.0); }
        break;
    }
    case FSide: case FSwing: {
        if (!AddonAuthoring(32)) { Status("Road tiles need a traffic addon with road support; existing routes remain available."); DockTrace("refused", "no_road_addon"); return false; }
        if (r.ring || n < 2) return false;
        EnableRoad(r); r.road.tiles.resize(std::size_t(n));
        for (int t : g_dock.tiles.items) { if (t < 0 || t >= n) continue;
            auto& tl = r.road.dividers.empty() ? r.road.tiles[std::size_t(t)] : r.road.dividers[std::size_t(std::clamp(g_roadDivider, 0, int(r.road.dividers.size()) - 1))][std::size_t(t)];
            (f == FSide ? tl.side : tl.swing) = std::clamp(value(t), -100.0, 100.0) / 100; }
        if (!roadplan::Valid(r.road) || trafficroute::Build(r).error) { Status("Dividers may not cross each other or leave the road; nothing changed."); DockTrace("refused", "divider_invalid"); return false; }
        break;
    }
    case FPZ: case FPX: case FPY: {
        for (int i : g_dock.points.items) { if (i < 0 || i >= n) continue; auto p = r.points[std::size_t(i)]; (f == FPZ ? p.z : f == FPX ? p.x : p.y) = value(i);
            if (const char* why = PointProblem(p.x, p.y, p.z)) { Refused("Point move", p.x, p.y, p.z, why); DockTrace("refused", "point_problem", i); return false; }
            r.points[std::size_t(i)] = p; }
        break;
    }
    case FRadius: {
        r.rounding.resize(std::size_t(n), 0); int skipped = 0;
        for (int i : g_dock.points.items) { if (i < 0 || i >= n) continue; if (!r.closed && (i == 0 || i + 1 == n)) { ++skipped; continue; } r.rounding[std::size_t(i)] = std::clamp(value(i), 0.0, 200.0); }
        if (skipped) Status("Route ends have no corner: " + std::to_string(skipped) + " left sharp");
        else if (g_dock.points.Count() == 1) { const int i = g_dock.points.items.front(); const double most = routecurve::CornerLimit(r.points, std::size_t(i), r.closed);  // the line can only round as far as its neighbours allow
            if (r.rounding[std::size_t(i)] > most) { char b[96]; std::snprintf(b, sizeof b, "Corner rounds to at most %.0f m here: the points beside it are close", most); Status(b); } }
        break;
    }
    case FAX: case FAY: case FAZ: case FAH: {
        if (!r.end.hasPlace) { r.end.place = DockArriveDefault(r); r.end.hasPlace = true; }
        const double x = value(-1);
        if (f == FAH) r.end.place.heading = std::fmod(std::fmod(x, 360.0) + 360.0, 360.0);
        else { auto p = r.end.place; (f == FAX ? p.x : f == FAY ? p.y : p.z) = x; if (const char* why = PointProblem(p.x, p.y, p.z)) { Refused("Arrive place", p.x, p.y, p.z, why); DockTrace("refused", "arrive_problem"); return false; } r.end.place = p; }
        r.end.placeUnsupported.clear();
        break;
    }
    default: return false;
    }
    g_ed.draft->Edit(r);
    const auto& list = DockItems(f);
    if (list.size() > 1 && list.front() >= 0) Status("Set on " + std::to_string(list.size()) + (TileField(f) ? " tiles" : " points") + " (Save keeps it)");
    DockTrace("field", delta ? "step" : "typed", f, int(list.size()));
    return true;
}
// Picks that change the selected tiles or points.
bool DockTilesEdit(const char* reason, const std::function<bool(trafficdata::Route&, int)>& change) {
    if (!EnsureDraft()) return false;
    auto r = g_ed.draft->Value(); bool any = false;
    for (int t : g_dock.tiles.items) if (t >= 0 && t < DockTiles(r)) any = change(r, t) || any;
    if (!any) return false;
    if (!roadplan::Valid(r.road) || trafficroute::Build(r).error) { Status("That leaves the road invalid; nothing changed."); DockTrace("refused", reason); return false; }
    g_ed.draft->Edit(r); DockTrace("tile_edit", reason, g_dock.tiles.Count());
    if (g_dock.tiles.Many()) Status(std::string("Set on ") + std::to_string(g_dock.tiles.Count()) + " tiles (Save keeps it)");
    return true;
}
void DockToggleFace(int face) {
    trafficdata::Route scratch; const auto* s = Shown(scratch); if (!s || g_dock.tiles.primary < 0) return;
    const bool touch = !f7dock::Touch(DockBox(*s, g_dock.tiles.primary).faces, face);
    if (!EnsureDraft()) return; auto r = g_ed.draft->Value(); if (!DockEnsureBoxes(r)) return;
    for (int t : g_dock.tiles.items) if (t >= 0 && t < int(r.road.boxes.size())) r.road.boxes[std::size_t(t)].faces = f7dock::SetFace(r.road.boxes[std::size_t(t)].faces, face, touch);
    g_ed.draft->Edit(r);
    Status(std::string(f7dock::FaceName(face)) + " face: " + (touch ? "Touch" : "Avoid") + (g_dock.tiles.Many() ? " on " + std::to_string(g_dock.tiles.Count()) + " tiles" : std::string()) + " (Save keeps it)");
    DockTrace("face", touch ? "touch" : "avoid", face, g_dock.tiles.Count());
}

// ---- typed entry ----
void DockBeginEntry(int field, const std::string& shown) { g_dock.entry.Begin(field, f7dock::Leading(shown)); DockTrace("entry", "begin", field); }
bool DockCommitEntry() {
    if (!g_dock.entry.Active()) return true;
    const int field = g_dock.entry.field; double v = 0;
    if (!g_dock.entry.Number(v)) { if (g_dock.entry.text.empty()) { g_dock.entry.End(); return true; } Status("Type a number. Enter keeps it, Esc cancels."); DockTrace("refused", "not_a_number", field); return false; }
    if (field >= kDockPickerEntry) {
        trafficdata::Route scratch; const auto* r = Shown(scratch); g_dock.entry.End(); if (!r) return true;
        const bool point = field == kDockPickerEntry + f7dock::Point; const int count = point ? int(r->points.size()) : DockTiles(*r);
        const auto [i, out] = f7dock::Typed(int(std::lround(v)), count); if (i < 0) return true;
        if (out) Status(std::string(point ? "Point " : "Tile ") + std::to_string(std::lround(v)) + " doesn't exist: this route has 1 to " + std::to_string(count));
        DockSelectOne(point ? f7dock::Points : f7dock::Tiles, i, "picker_typed"); return true;
    }
    g_dock.entry.End();
    return DockApply(field, v, false);
}
void DockEntryInput(bool focused) {
    if (!DockTyping()) return;
    inputgate::HoldMapAll();
    for (int vk = '0'; vk <= '9'; ++vk) if (g_keys.Pressed(vk, focused)) g_dock.entry.Key(char(vk));
    for (int vk = VK_NUMPAD0; vk <= VK_NUMPAD9; ++vk) if (g_keys.Pressed(vk, focused)) g_dock.entry.Key(char('0' + vk - VK_NUMPAD0));
    if (g_keys.Pressed(VK_OEM_PERIOD, focused) || g_keys.Pressed(VK_DECIMAL, focused)) g_dock.entry.Key('.');
    if (g_keys.Pressed(VK_OEM_MINUS, focused) || g_keys.Pressed(VK_SUBTRACT, focused)) g_dock.entry.Key('-');
    if (g_keys.Pressed(VK_BACK, focused)) g_dock.entry.Back();
    for (int vk = 'A'; vk <= 'Z'; ++vk) g_keys.Pressed(vk, focused);  // letters are not tools while typing
    if (g_keys.Pressed(VK_ESCAPE, focused)) { g_dock.entry.End(); Status("Typed value cancelled"); DockTrace("entry", "cancel"); return; }
    const bool tab = g_keys.Pressed(VK_TAB, focused);
    if (g_keys.Pressed(VK_RETURN, focused) || tab) {
        const int was = g_dock.entry.field;
        if (!DockCommitEntry()) return;
        if (tab && was < kDockPickerEntry && !g_dock.visibleFields.empty()) {
            const bool back = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0; auto at = std::find(g_dock.visibleFields.begin(), g_dock.visibleFields.end(), was);
            const int k = at == g_dock.visibleFields.end() ? 0 : int(at - g_dock.visibleFields.begin()), m = int(g_dock.visibleFields.size()), next = g_dock.visibleFields[std::size_t(((k + (back ? -1 : 1)) % m + m) % m)];
            DockBeginEntry(next, std::size_t(next) < g_dock.shownValues.size() ? g_dock.shownValues[std::size_t(next)] : std::string());
        }
    }
}

// ---- cards R2-R6: the restored menus' doors and buttons ----
bool LinkOn() { return g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size()); }
// The problems of the shown route, as the editor's own Problems(route, deep) gives them; asked again twice a second or when the route changes.
const std::vector<Problem>& DockProblems(const trafficdata::Route* r, bool fresh = false) {
    const auto now = GetTickCount64(); const std::string key = r ? r->name + "#" + std::to_string(r->points.size()) : std::string();
    if (fresh || key != g_dock.probKey || now - g_dock.probAt >= 500) { g_dock.problems = r ? Problems(*r, true) : std::vector<Problem>{}; g_dock.probKey = key; g_dock.probAt = now; }
    return g_dock.problems;
}
// Severity, as the old list coloured it: must fix (a climb over 45 deg, a sharp turn, through a keep-out zone), a warning (clearance,
// the road fit), a note (points close together).
int DockSeverity(const Problem& p) { return p.kind == 2 || p.kind == 4 || p.kind == 1 ? 1 : p.kind == 3 ? 3 : 2; }
bool DockSafeFix(const Problem& p) { return p.kind == 2 || p.kind == 4; }  // Ease it, Round it: one point's height or one corner's radius
void DockDoor(int level, const trafficdata::Route* r) {
    if (level == f7dock::Places) { if (g_f7NodeView) { g_f7NodeView = false; DockTrace("node", "view_off_for_places"); } DockGo(f7dock::Places, "door"); return; }
    if (level == f7dock::Extras || level == f7dock::TrafficLevels) { if (r) DockGo(level, "door"); return; }
    if (level == f7dock::Hazards) { if (r && !r->ring) { g_ed.mode = Mode::Hazard; g_ed.hazard = -1; g_ed.suggest = -1; hazardpick::Cancel(g_ed.hazardLine); DockGo(f7dock::Hazards, "door"); Status("Place Hazard: click the surface to set it down, click again to set its length and direction (Esc cancels). Q selects a Hazard, R moves it."); } else DockTrace("refused", "hazards_need_an_open_route"); return; }
    if ((level == f7dock::Access || level == f7dock::Wires || level == f7dock::Landing) && AreaOn()) { DockGo(level, "door"); return; }
    if (level == f7dock::Zone && ZoneOn()) { DockGo(level, "door"); return; }
    if (level == f7dock::Junction && LinkOn()) { DockGo(level, "door"); return; }
    DockTrace("refused", "door_needs_selection", level);
}
bool DockRestoreAction(int id, const trafficdata::Route* r) {
    if (id == kDockReadme) { g_dock.readme = !g_dock.readme; if (g_dock.readme) g_dock.help = false; DockTrace("readme", g_dock.readme ? "open" : "close"); return true; }
    if (id == kDockScript) {  // Script mode toggle: saved to settings.ini at once; the message every time it is switched on
        const bool on = !features::On(features::Id::ScriptMode);
        features::Set(features::Id::ScriptMode, on);
        DockTrace("script_mode", on ? "on" : "off");
        if (on) ShowScriptModeMessage();
        Status(on ? "Script mode on: vehicles follow the path; only Hazard boxes, vehicles and people are obstacles" : "Script mode off: vehicles detect objects again");
        return true;
    }
    if (id == kDockHelp) { g_dock.help = !g_dock.help; if (g_dock.help) g_dock.readme = false; DockTrace("help", g_dock.help ? "open" : "close"); return true; }
    if (id == kHazardTool) {  // the Hazards tool (key \ or its cards): its own dock level; pressed again it ends
        if (g_ed.mode == Mode::Hazard) { g_ed.mode = Mode::Select; g_ed.hazardCopy = false; hazardpick::Cancel(g_ed.hazardLine); Status("Place off: Select (Q) picks a Hazard, Move (R) drags it"); DockTrace("hazard", "place_tool_off"); return true; }  // the level follows the selection (DockSync)
        DockDoor(f7dock::Hazards, r); return true;
    }
    if (id == kDockPlaceTool || id == kShapeTool) {  // F (the shape key) opens Places too: the shapes are there now
        if (f7dock::PlaceLevel(g_dock.level) && id == kDockPlaceTool) { DockGo(r ? f7dock::Route : f7dock::Routes, "place_tool_off"); return true; }
        DockDoor(f7dock::Places, r); return true;
    }
    if ((id == kDrop || id == kDrag || id == kRouteTool) && f7dock::PlaceLevel(g_dock.level)) DockGo(r ? f7dock::Route : f7dock::Routes, "route_tool_from_places");  // then the tool itself, as before
    if (id >= kDockPlaceNext && id < kDockPlaceNext + 3) {  // pick the next landing, zone or junction of this world (the selection only)
        const int kind = id - kDockPlaceNext; const int count = kind == 0 ? int(g_ed.file.landings.size()) : kind == 1 ? int(g_ed.file.zones.size()) : int(g_ed.file.links.size());
        if (!count) { Status(kind == 0 ? "No landing areas yet: Landing, then click the map" : kind == 1 ? "No zones yet: Zone, then click the map" : "No junctions yet: Junction, then click another route"); DockTrace("place", "next_none", kind); return true; }
        const int cur = kind == 0 ? (AreaOn() ? g_ed.spot : -1) : kind == 1 ? (ZoneOn() ? g_ed.zone : -1) : (LinkOn() ? g_ed.link : -1);
        const int to = f7dock::Step(cur, 1, count);
        g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1;
        if (kind == 0) { g_ed.spotKind = 1; g_ed.spot = to; Status("Landing " + std::to_string(to + 1) + ": " + AreaWords(g_ed.file.landings[std::size_t(to)])); }
        else if (kind == 1) { g_ed.zone = to; Status("Zone " + std::to_string(to + 1) + ": " + ZoneWords(g_ed.file.zones[std::size_t(to)])); }
        else { g_ed.link = to; const auto& k = g_ed.file.links[std::size_t(to)]; Status("Junction " + RouteLabel(k.from) + " > " + RouteLabel(k.to)); }
        DockTrace("place", "next", kind, to); DockSync(); return true;
    }
    if (id >= kDockWire && id < kDockWire + 64 * 4) { const int w = (id - kDockWire) / 4, op = (id - kDockWire) % 4; F7WireAction(F7WireButton(w, op)); DockTrace("wire", op == 0 ? "next_route" : op == 1 ? "rebuild" : "remove", w); return true; }
    if (id == kDockWireAddIn || id == kDockWireAddOut) { F7WireAction(F7WireButton(id == kDockWireAddOut ? 1 : 0, -1)); DockTrace("wire", id == kDockWireAddOut ? "add_out" : "add_in"); return true; }
    if (id == kDockNodeMake) { const bool ok = MakeRouteFromPicks(); DockTrace("node", ok ? "make_route" : "make_route_refused", NodePickCount()); return true; }
    if (id == kDockNodeClear) { NodePicksClear(); Status("Picks cleared."); DockTrace("node", "picks_cleared"); return true; }
    if (id == kDockNodeRemove) { const bool ok = NodeViewDelete(); if (!ok) Status("Select a wire first: click it in the view"); DockTrace("node", ok ? "wire_removed" : "no_wire"); return true; }
    if (id == kDockProbView || id == kDockProbView + 1) { g_dock.probView = id - kDockProbView; g_dock.probShown = -1; DockTrace("problems", g_dock.probView ? "view_items" : "view_grouped"); return true; }
    if (id >= kDockProbFilter && id < kDockProbFilter + 4) { g_dock.probFilter = id - kDockProbFilter; g_dock.probShown = -1; DockTrace("problems", "filter", g_dock.probFilter); return true; }
    if (id >= kDockProbShow && id < kDockProbShow + 100) {
        const auto& list = DockProblems(r); const int k = id - kDockProbShow; if (k >= int(list.size())) return true;
        g_dock.probShown = g_dock.probShown == k ? -1 : k; if (g_dock.probShown >= 0) Status(list[std::size_t(k)].text + ": ringed on the map");
        DockTrace("problems", g_dock.probShown >= 0 ? "show" : "show_off", k, list[std::size_t(k)].kind); return true;
    }
    if (id >= kDockProbFix && id < kDockProbFix + 100) {  // the editor's own FixProblem, on the same problem in a fresh list (matched by its text)
        const auto shown = DockProblems(r); const int k = id - kDockProbFix; if (k >= int(shown.size()) || !r) return true;
        const auto& fresh = DockProblems(r, true); const auto at = std::find_if(fresh.begin(), fresh.end(), [&](const Problem& p) { return p.text == shown[std::size_t(k)].text && p.kind == shown[std::size_t(k)].kind; });
        if (at == fresh.end()) { Status("That problem is gone already"); DockTrace("problems", "fix_gone", k); return true; }
        const Problem pick = *at; FixProblem(pick); g_dock.probShown = -1; g_dock.probAt = 0;
        if (g_dock.level != f7dock::Problems) DockGo(f7dock::Problems, "after_fix");  // FixProblem selects the point it fixed; the list stays open
        DockTrace("problems", "fix", pick.kind, pick.a); return true;
    }
    if (id >= kDockProbTile && id < kDockProbTile + 1000) { const int t = id - kDockProbTile; if (r && t < DockTiles(*r)) DockSelectOne(f7dock::Tiles, t, "problem_tile_row"); return true; }
    if (id == kDockFixSafe) {  // R6: the safe fixes one at a time, the list asked again after each, at most 32
        if (!r) return true; int done = 0;
        for (int guard = 0; guard < 32; ++guard) { trafficdata::Route s2; const auto* now = Shown(s2); if (!now) break; const auto& list = DockProblems(now, true);
            const auto at = std::find_if(list.begin(), list.end(), DockSafeFix); if (at == list.end()) break; const Problem pick = *at; const auto before = list.size(); FixProblem(pick); ++done;
            trafficdata::Route s3; const auto* after = Shown(s3); if (!after || DockProblems(after, true).size() >= before) break; }  // a fix that changed nothing stops the run
        g_dock.probShown = -1; g_dock.probAt = 0; if (g_dock.level != f7dock::Problems) DockGo(f7dock::Problems, "after_fix_all");
        Status(done ? "Fixed " + std::to_string(done) + " safe problem" + (done == 1 ? "" : "s") + ". Undo brings them back; Save keeps it." : "No safe fixes left"); DockTrace("problems", "fix_all_safe", done); return true;
    }
    return false;
}

// ---- actions: the dock's own buttons; false = not a dock button (the editor's own Press handles it) ----
bool DockAction(int id, const routemap::Camera& c) {
    if (!g_dock.on) return false;
    const bool fieldBox = id >= kDockField && id < kDockField + FFieldCount * 4 && (id - kDockField) % 4 == 0;
    if (g_dock.entry.Active() && !(fieldBox && (id - kDockField) / 4 == g_dock.entry.field) && id != kDockPickPoint && id != kDockPickTile) DockCommitEntry();  // leaving a box keeps what was typed
    // Out of the dock by the user's decision (2026-11-06): the whole-file Readme and the More and View menus (key Y). Reverse, Copy,
    // Mirror and Colour are back as Route extras (card R4, 2026-11-08); their keys V and U work again.
    if (id == kReadme || id == kMoreMenu || id == kViewMenu) { DockTrace("refused", "not_in_dock", id); return true; }
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    if (ItemsAction(id)) return true;  // RI-3: the Problems level's rows, filters and buttons
    if (DockRestoreAction(id, r)) return true;  // cards R2-R6
    if (DockIdLevel(id) >= 0) {
        const int level = DockIdLevel(id);
        if (g_f7NodeView && level != f7dock::Nodes) { g_f7NodeView = false; DockTrace("node", "view_off_breadcrumb", level); }
        if (level >= f7dock::Places) { DockDoor(level, r); return true; }
        if (level == f7dock::Routes) { if (g_ed.draft) { Status("Save or Cancel the open route first"); DockTrace("refused", "draft_open"); return true; } g_ed.route = -1; g_ed.other = -1; g_dock.routeKey.clear(); DockGo(f7dock::Routes, "breadcrumb"); return true; }
        if (level == f7dock::City) { DockGo(f7dock::City, "door"); return true; }
        if (!r) return true;
        if (level == f7dock::Route) { DockGo(f7dock::Route, "breadcrumb"); return true; }
        if (level == f7dock::Tile && !r->ring && DockTiles(*r) > 0) { DockSelectOne(f7dock::Tiles, std::clamp(g_dock.tiles.primary, 0, DockTiles(*r) - 1), "rail"); return true; }
        if (level == f7dock::Point && !r->ring && !r->points.empty()) { DockSelectOne(f7dock::Points, std::clamp(g_dock.points.primary, 0, int(r->points.size()) - 1), "rail"); return true; }
        if (level == f7dock::Arrive && !r->closed) { if (r->end.end != routeend::Arrive) DockAction(kDockEnd + routeend::Arrive, c); DockGo(f7dock::Arrive, "door"); return true; }
        if (level == f7dock::Types) { DockGo(f7dock::Types, "door"); return true; }
        if (level == f7dock::Problems) { DockGo(f7dock::Problems, "door"); return true; }
        return true;
    }
    if (id >= kDockNavPointPrev && id <= kDockNavTileNext) {
        if (!r) return true; const bool point = id <= kDockNavPointNext; const int by = id == kDockNavPointNext || id == kDockNavTileNext ? 1 : -1;
        const int count = point ? int(r->points.size()) : DockTiles(*r), cur = point ? (g_dock.level == f7dock::Point ? g_dock.points.primary : -1) : (g_dock.level == f7dock::Tile ? g_dock.tiles.primary : -1);
        const int to = f7dock::Step(cur, by, count); if (to >= 0) DockSelectOne(point ? f7dock::Points : f7dock::Tiles, to, "picker_step"); return true;
    }
    if (id == kDockPickPoint || id == kDockPickTile) {
        if (!r) return true; const bool point = id == kDockPickPoint;
        const int cur = point ? (g_dock.level == f7dock::Point ? g_dock.points.primary : -1) : (g_dock.level == f7dock::Tile ? g_dock.tiles.primary : -1);
        g_dock.entry.Begin(kDockPickerEntry + (point ? f7dock::Point : f7dock::Tile), cur >= 0 ? std::to_string(cur + 1) : std::string()); DockTrace("entry", "picker", point); return true;
    }
    if (id == kDockSelOne) { if (g_dock.level == f7dock::Point) DockSelectOne(f7dock::Points, g_dock.points.primary, "select_one"); else if (g_dock.level == f7dock::Tile) DockSelectOne(f7dock::Tiles, g_dock.tiles.primary, "select_one"); return true; }
    if (id == kDockSelAllPoints || id == kDockSelAllTiles) { DockSelectAll(id == kDockSelAllPoints ? f7dock::Points : f7dock::Tiles, "button"); return true; }
    if (id == kDockCardPrev || id == kDockCardNext) { g_dock.cardFirst = std::max(0, g_dock.cardFirst + (id == kDockCardNext ? 8 : -8)); return true; }
    if (id == kDockUpdateRoute) {
        if (!r || r->ring || DockTiles(*r) == 0) { Status("Select a route with tiles to update"); DockTrace("update_route", "no_tiles"); return true; }
        auto candidate = *r; const int added = tilebox::MaterializeMissing(candidate);
        if (added < 0) {
            bool old = false; for (int t = int(r->road.boxes.size()); t < DockTiles(*r); ++t) old = old || DockOldShape(*r, t);
            if (old && r->road.boxesUnsupported.empty() && r->road.unsupported.empty()) { Status("Route unchanged: it has rounded, tube or trapezoid tiles, which wait for your D11 answer"); DockTrace("update_route", "d11_shapes_kept"); }
            else { Status("Route unchanged: stored boxes or shapes cannot be updated by this version"); DockTrace("update_route", "unsupported_kept"); }
            return true; }
        if (!added) { Status("Route is already updated; authored boxes kept"); DockTrace("update_route", "already_current"); return true; }
        if (!EnsureDraft()) return true;
        auto t = g_ed.draft->Value(); t.road.boxes = std::move(candidate.road.boxes); g_ed.draft->Edit(t);
        Status("Updated " + std::to_string(added) + " route tiles. Save keeps it; Cancel discards it.");
        DockTrace("update_route", "draft_defaults_added", added); return true;
    }
    if (id == kDockTest) {  // the editor's own Test (Press kTest): marks the route and saves it
        crash::Crumb("f7 test begin (dock)", 0);
        const std::optional<trafficdata::Route> shownCopy = r ? std::optional<trafficdata::Route>(*r) : std::nullopt;  // HV-4: the route as shown, before Save changes the draft
        if (EnsureDraft()) { auto t = g_ed.draft->Value(); t.test = true; g_ed.draft->Edit(t); Save(); crash::Crumb("f7 test saved (dock)", g_ed.draft ? 0 : 1); if (!g_ed.draft) Status("Test requested: the next free vehicle with a driver near the route flies it once (see the log's 'test route' lines)"); }
        if (shownCopy && !shownCopy->ring) { crash::Crumb("f7 test preview begin", 0); ItemsStartPreview(*shownCopy); ItemsShowPreview(); g_dock.probView = 1; DockGo(f7dock::Problems, "test_preview"); crash::Crumb("f7 test preview end", 0); }  // HV-4: and draw where a car will drive
        DockTrace("test", g_ed.draft ? "not_saved" : "saved"); crash::Crumb("f7 test end (dock)", 0); return true;
    }
    if (id >= kDockField && id < kDockField + FFieldCount * 4) {
        const int f = (id - kDockField) / 4, part = (id - kDockField) % 4;
        if (part == 0) { DockBeginEntry(f, std::size_t(f) < g_dock.shownValues.size() ? g_dock.shownValues[std::size_t(f)] : std::string()); return true; }
        DockApply(f, part == 1 ? -kDockStep[f] : kDockStep[f], true); return true;
    }
    if (id >= kDockLookSpacing && id < kDockLookSpacing + 4) {
        if (!r || !EnsureDraft()) return true;
        auto t = g_ed.draft->Value();
        const int preset = id - kDockLookSpacing;
        const double lenM = std::max(100.0, trafficdata::TrafficLength(t));
        trafficlook::ApplySpacing(t.schedule, preset, t.speed, DockLanes(*r), lenM);
        g_ed.draft->Edit(t);
        const auto lk = trafficlook::EvaluateRoadLook(t.schedule, t.speed, DockLanes(*r), lenM);
        Status(std::string("Spacing: ") + trafficlook::SpacingName(preset) + " -> " + trafficlook::FormatLookLine(lk) + " (Save keeps it)");
        DockTrace("look", "spacing", preset, lk.cars);
        return true;
    }
    if (id >= kDockLookPace && id < kDockLookPace + 3) {
        if (!r || !EnsureDraft()) return true;
        auto t = g_ed.draft->Value();
        const int preset = id - kDockLookPace;
        const bool isSky = t.travel == routepolicy::Travel::Hover;
        t.speed = trafficlook::PaceSpeed(preset, isSky);
        g_ed.draft->Edit(t);
        Status(std::string("Pace: ") + trafficlook::PaceName(preset) + " (" + std::to_string(int(t.speed)) + " m/s; Save keeps it)");
        DockTrace("look", "pace", preset, int(t.speed));
        return true;
    }
    if (id >= kDockTravel && id < kDockTravel + 3) {
        if (!EnsureDraft()) return true; auto t = g_ed.draft->Value(); t.travel = routepolicy::Travel(id - kDockTravel); g_ed.draft->Edit(t);
        Status(std::string("Route travel: ") + routepolicy::Name(t.travel) + " (Save keeps it)"); DockTrace("route", "travel", id - kDockTravel); return true;
    }
    if (id >= kDockEnd && id < kDockEnd + 3) {
        if (!r || r->closed) { Status("A loop has no end"); return true; }
        if (!EnsureDraft()) return true; auto t = g_ed.draft->Value(); t.end.end = id - kDockEnd; t.end.endUnsupported.clear();
        if (t.end.end == routeend::Arrive && !t.end.hasPlace) { t.end.place = DockArriveDefault(t); t.end.hasPlace = true; }
        g_ed.draft->Edit(t); Status(std::string("Line end: ") + routeend::Name(t.end.end) + " (Save keeps it)"); DockTrace("route", "line_end", t.end.end);
        if (t.end.end == routeend::Arrive) DockGo(f7dock::Arrive, "line_end_arrive");
        return true;
    }
    if (id >= kDockRouteGroup && id < kDockRouteGroup + kDockRouteGroups) {  // 2026-11-11: the Route menu's group tabs
        g_dock.routeGroup = id - kDockRouteGroup; DockTrace("route", "group", g_dock.routeGroup); return true;
    }
    if (id == kDockOneWay || id == kDockTwoWay) {  // 2026-11-11: the lane count, through the same path as the Lanes stepper
        const int want = id == kDockOneWay ? 1 : 2;
        const bool ok = DockApply(FLanes, want, false);
        logger::Write("OBS system=f7.dock event=ways want=%d ok=%d reason=%s", want, ok ? 1 : 0, ok ? "lane_count_set" : "refused_see_status");
        return true;
    }
    if (id == kDockAlong || id == kDockAgainst) {
        if (!AddonAuthoring(32)) { Status("Road tiles need a traffic addon with road support; existing routes remain available."); DockTrace("refused", "no_road_addon"); return true; }
        if (!EnsureDraft()) return true; auto t = g_ed.draft->Value(); if (t.ring || t.points.size() < 2) return true; EnableRoad(t);
        const int lanes = DockLanes(t); t.road.directions.resize(std::size_t(lanes)); for (int k = 0; k < lanes; ++k) if (t.road.directions[std::size_t(k)] == 0) t.road.directions[std::size_t(k)] = k < (lanes + 1) / 2 ? 1 : -1;
        t.road.directions[std::size_t(std::clamp(g_roadLane, 0, lanes - 1))] = id == kDockAlong ? 1 : -1;
        if (!roadplan::Valid(t.road)) { DockTrace("refused", "direction_invalid"); return true; }
        g_ed.draft->Edit(t); Status("Lane " + std::to_string(g_roadLane + 1) + (id == kDockAlong ? " runs along the markers" : " runs against the markers") + " (Save keeps it)"); DockTrace("route", "direction", g_roadLane + 1); return true;
    }
    if (id >= kDockTileTravel && id < kDockTileTravel + 3) {
        // Tile travel replaces the old presets, so faces and travel can't contradict: Hover = every face Avoid, Both / Drive = bottom Touch.
        // Choosing what every selected tile already has goes back to the route's own.
        const auto want = routepolicy::Travel(id - kDockTileTravel);
        const bool back = r && std::all_of(g_dock.tiles.items.begin(), g_dock.tiles.items.end(), [&](int t) { return t < int(r->sections.size()) && r->sections[std::size_t(t)].travel == want; });
        DockTilesEdit("tile_travel", [&](trafficdata::Route& q, int t) {
            q.sections.resize(q.points.size()); if (!DockEnsureBoxes(q)) return false;
            q.sections[std::size_t(t)].travel = back ? routepolicy::Travel::Inherit : want;
            q.road.boxes[std::size_t(t)].faces = DockTileHover(q, t) ? roadplan::kAllAvoid : roadplan::kFaceBottom; return true; });
        return true;
    }
    if (id == kDockSolids || id == kDockSolids + 1) { DockTilesEdit("solids", [&](trafficdata::Route& q, int t) { if (!DockEnsureBoxes(q)) return false; q.road.boxes[std::size_t(t)].solids = id - kDockSolids; return true; }); return true; }
    if (id == kDockJoin || id == kDockJoin + 1) {
        if (!AddonAuthoring(32)) { Status("Road tiles need a traffic addon with road support; existing routes remain available."); DockTrace("refused", "no_road_addon"); return true; }
        DockTilesEdit("join", [&](trafficdata::Route& q, int t) { EnableRoad(q); q.road.tiles.resize(q.points.size()); q.road.tiles[std::size_t(t)].connector = id == kDockJoin ? 1 : 2;
            for (auto& row : q.road.dividers) { row.resize(q.points.size()); row[std::size_t(t)].connector = id == kDockJoin ? 1 : 2; } return true; });
        return true;
    }
    if (id == kDockSharp || id == kDockRound) {
        if (!EnsureDraft()) return true; auto t = g_ed.draft->Value(); const int n = int(t.points.size()); t.rounding.resize(std::size_t(n), 0);
        for (int i : g_dock.points.items) if (i >= 0 && i < n && (t.closed || (i > 0 && i + 1 < n))) t.rounding[std::size_t(i)] = id == kDockSharp ? 0 : t.rounding[std::size_t(i)] > 0 ? t.rounding[std::size_t(i)] : 20;
        g_ed.draft->Edit(t); DockTrace("point_edit", id == kDockSharp ? "sharp" : "round", g_dock.points.Count()); return true;
    }
    if (id == kDockPark || id == kDockPark + 1) {
        if (!EnsureDraft()) return true; auto t = g_ed.draft->Value(); if (!t.end.hasPlace) { t.end.place = DockArriveDefault(t); t.end.hasPlace = true; }
        t.end.place.park = id - kDockPark; g_ed.draft->Edit(t); DockTrace("arrive", "park", t.end.place.park); return true;
    }
    if (id >= kDockCityOn && id <= kDockOrigOff) {
        auto& city = g_ed.file.city; if (id == kDockCityOn || id == kDockCityOff) city.on = id == kDockCityOn; else city.originals = id == kDockOrigOn;
        g_ed.file.cityWritten = true; Status("City traffic changed: Save keeps it"); DockTrace("city", "switch", id - kDockCityOn); return true;
    }
    if (id == kDockHealthAllow) {
        const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); const auto allow = addon ? reinterpret_cast<FvoTrafficHealthAllowV1>(GetProcAddress(addon, "FVO_Traffic_HealthAllowV1")) : nullptr;
        int n = 0; if (allow) for (const auto& type : VehicleTypesHere()) { bool out = false; std::vector<unsigned> ids; DockHealth(type, &out, &ids); if (out) for (unsigned x : ids) { allow(x, 1); ++n; } }
        Status(n ? "Health's left-out models allowed: used again from the next fill" : "Health has left nothing out"); DockTrace("types", "health_allow", n); return true;
    }
    if (id == kDockAllowAll || (id >= kDockType && id < kDockType + 1000)) {
        if (!EnsureDraft()) return true; auto t = g_ed.draft->Value();
        if (id == kDockAllowAll) t.exclude.clear();
        else { const auto& types = VehicleTypesHere(); const int k = id - kDockType; if (k >= int(types.size())) return true; const auto& type = types[std::size_t(k)];
            if (TypeIn(t.exclude, type)) std::erase_if(t.exclude, [&](unsigned x) { return std::find(type.ids.begin(), type.ids.end(), x) != type.ids.end(); });
            else for (unsigned x : type.ids) if (!trafficdata::InList(t.exclude, x)) t.exclude.push_back(x); }
        g_ed.draft->Edit(t); DockTrace("types", id == kDockAllowAll ? "allow_all" : "toggle", id - kDockType); return true;
    }
    (void)c;
    return false;
}

// ---- tile warnings (user, 2026-11-06): spawning trusts the field, so the editor warns while the field is drawn, never blocks ----
// Per tile, 12 rays through its box (vehicle_probe::CastNow, the editor's direct rays, which answer while the map is paused): 3 down rays
// for the floor (only when the bottom face is Touch), 3 across and 3 along at 0.8 m over the floor, and 3 down from the roof to 0.5 m over
// the floor for solids. At most one tile a frame (the handbook's ray budget of about 12 a frame); selected tiles first; a result is kept
// until the tile's geometry changes. Warnings: a solid inside the box while its solids flag is off; the floor more than 0.6 m off a real
// surface. Saving is never refused. Stored rounded / tube / trapezoid tiles without a box are not checked (D11 not approved).
struct DockCheck { std::string sig; int state = 0; bool solid = false, floorBad = false, floorKnown = false; double floorDz = 0, solidZ = 0; std::string why; std::uint64_t retryAt = 0; };
std::map<std::string, DockCheck> g_dockChecks;
std::uint64_t g_dockRaysOffUntil = 0;
// The tile's box in the world: bottom on the route line (as the addon's provider puts it), yaw and pitch from the points plus the offsets.
// FPS-1 (2026-11-07, user: "massive fps drop in the UI"): EditorLine's cache key is a full text dump of the route, so asking for it once
// per tile rebuilt that dump 177 times a pass (and every frame in DockCheckTick). Callers that walk tiles get the line once and pass it.
bool DockWorldBox(const trafficdata::Route& r, const trafficroute::Line& line, int t, fieldbox::Box& out) {
    FvoFieldBoxV1 box;
    return tilebox::TileBox(r, line, t, box) && fieldboxapi::ToBox(box, out);
}
bool DockWorldBox(const trafficdata::Route& r, int t, fieldbox::Box& out) { return DockWorldBox(r, EditorLine(r), t, out); }
std::string DockCheckSig(const trafficdata::Route& r, const trafficroute::Line& line, int t) {
    fieldbox::Box f; if (!DockWorldBox(r, line, t, f)) return {}; char b[224];
    std::snprintf(b, sizeof b, "%.2f %.2f %.2f %.2f %.2f %.2f %.3f %.3f %.3f %d %d", f.centre.x, f.centre.y, f.centre.z, f.halfWidth, f.halfLength, f.halfHeight, f.yaw, f.pitch, f.roll, DockBox(r, t).faces & 1, DockBox(r, t).solids);
    return b;
}
std::string DockCheckSig(const trafficdata::Route& r, int t) { return DockCheckSig(r, EditorLine(r), t); }
void DockCheckTile(const trafficdata::Route& r, int t, DockCheck& out) {
    fieldbox::Box f; if (!DockWorldBox(r, t, f)) { out.state = 0; return; } const double hw = f.halfWidth, hl = f.halfLength, hh = f.halfHeight;
    auto world = [&](double x, double y, double z) { const auto p = fieldbox::World(f, { x, y, z }); return std::array<float, 3>{ float(p.x), float(p.y), float(p.z) }; };
    auto cast = [&](std::array<float, 3> a, std::array<float, 3> b, prefetch::RayHit& h) { return vehicle_probe::CastNow(a.data(), b.data(), 0x22806, h); };
    DockCheck c; c.sig = out.sig; c.state = 2;
    const bool floorTouch = f7dock::Touch(DockBox(r, t).faces, 0);
    for (double y : { -.6, 0.0, .6 }) {  // the floor: down from 1.5 m over the bottom face to 3 m under it
        if (!floorTouch) break;
        prefetch::RayHit h; const int k = cast(world(0, y * hl, -hh + 1.5), world(0, y * hl, -hh - 3), h);
        if (k < 0) { g_dockRaysOffUntil = GetTickCount64() + 10000; out.state = 0; out.retryAt = g_dockRaysOffUntil; DockTrace("check", "ray_fault", t); return; }
        if (k == 0) { out.state = 0; out.retryAt = GetTickCount64() + 2000; return; }  // no world here now: ask again later
        const double bottom = fieldbox::World(f, { 0, y * hl, -hh }).z;
        if (!h.hit) { c.floorBad = true; c.floorKnown = true; c.floorDz = -3; continue; }
        c.floorKnown = true; const double dz = h.z - bottom;
        if (std::fabs(dz) > .6 && std::fabs(dz) > std::fabs(c.floorDz)) { c.floorBad = true; c.floorDz = dz; }
    }
    bool noWorld = false;
    auto solid = [&](std::array<float, 3> a, std::array<float, 3> b, bool fromRoof) {
        if (noWorld) return; prefetch::RayHit h; const int k = cast(a, b, h); if (k <= 0) { noWorld = true; return; } if (!h.hit || c.solid) return;
        if (fromRoof && h.normalOk && h.nz > .7) { const auto l = fieldbox::Local(f, { h.x, h.y, h.z }); const double over = h.z - fieldbox::World(f, { l.x, l.y, -hh }).z;
            if (!c.floorBad || std::fabs(over) > std::fabs(c.floorDz)) { c.floorBad = true; c.floorKnown = true; c.floorDz = over; } return; }  // a surface over the floor, inside the box
        c.solid = true; c.solidZ = h.z; };
    const double z = -hh + .8;
    for (double y : { -.66, 0.0, .66 }) solid(world(-hw + .2, y * hl, z), world(hw - .2, y * hl, z), false);
    for (double x : { -.66, 0.0, .66 }) solid(world(x * hw, -hl + .2, z), world(x * hw, hl - .2, z), false);
    for (double x : { -.66, 0.0, .66 }) solid(world(x * hw, 0, hh - .2), world(x * hw, 0, -hh + .5), true);
    if (noWorld) { out.state = 0; out.retryAt = GetTickCount64() + 2000; return; }  // unanswered: never read as clear
    char why[160]; why[0] = 0;
    const bool allowed = DockBox(r, t).solids != 0;
    if (c.solid && !allowed) std::snprintf(why, sizeof why, "Solid inside the box (at %.1f m height); Solids is Block", c.solidZ);
    if (c.floorBad) { char b2[96]; if (c.floorDz <= -3) std::snprintf(b2, sizeof b2, "No surface within 3 m under the floor"); else if (c.floorDz < 0) std::snprintf(b2, sizeof b2, "Floor %.1f m above the surface", -c.floorDz); else std::snprintf(b2, sizeof b2, "Floor %.1f m under a surface", c.floorDz);
        std::snprintf(why + std::strlen(why), sizeof why - std::strlen(why), "%s%s", why[0] ? "; " : "", b2); }
    if (routesurvey::NoClearNear(f.centre.x, f.centre.y, std::max(f.halfLength, 3.0)))  // the clear-path sweep; here, once per check: DockWarning runs per tile per frame
        std::snprintf(why + std::strlen(why), sizeof why - std::strlen(why), "%sSwept: no clear line for a car here", why[0] ? "; " : "");
    c.why = why;
    out = c;
    static int told = 0;
    if (told < 300 && (!c.why.empty())) { ++told; logger::Write("OBS system=f7.tilecheck event=warn route=%s tile=%d solid=%d allowed=%d floor_dz=%.2f reason=%s", r.name.c_str(), t + 1, c.solid ? 1 : 0, allowed ? 1 : 0, c.floorDz, c.why.c_str()); }
}
// FPS-1: per-tile loops pass the line they got once; the short forms ask EditorLine themselves (one tile, same answer).
const DockCheck* DockCheckOf(const trafficdata::Route& r, const trafficroute::Line& line, int t) { const auto it = g_dockChecks.find(r.name + "#" + std::to_string(t)); return it == g_dockChecks.end() || it->second.state != 2 || it->second.sig != DockCheckSig(r, line, t) ? nullptr : &it->second; }
const DockCheck* DockCheckOf(const trafficdata::Route& r, int t) { return DockCheckOf(r, EditorLine(r), t); }
// Geometry-only warning is available even when the world's ray service has no answer.
std::string DockWarning(const trafficdata::Route& r, const trafficroute::Line& line, int t) {
    std::string why;
    if (t >= 0 && t < int(r.road.boxes.size()) && r.road.boxesUnsupported.empty() && r.road.boxes[std::size_t(t)].width == 0 &&
        tilebox::RoadWidth(r) + .01 < tilebox::LaneSpanWidth(r))
        why = "Road-width box is narrower than the lane spacing";
    const auto* c = DockCheckOf(r, line, t);
    if (c && !c->why.empty()) { if (!why.empty()) why += "; "; why += c->why; }
    return why;
}
std::string DockWarning(const trafficdata::Route& r, int t) { return DockWarning(r, EditorLine(r), t); }
bool DockWarns(const trafficdata::Route& r, const trafficroute::Line& line, int t) { return !DockWarning(r, line, t).empty(); }
bool DockWarns(const trafficdata::Route& r, int t) { return !DockWarning(r, t).empty(); }
// Once a frame while the dock shows a route: check one tile that has no fresh result (selected tiles first).
void DockCheckTick(const trafficdata::Route* r) {
    if (!g_dock.on || !r || r->ring || !navmap::RaysEnabled()) return;
    const auto now = GetTickCount64(); if (now < g_dockRaysOffUntil) return;
    if (g_dockChecks.size() > 4000) g_dockChecks.clear();
    const int tiles = DockTiles(*r); std::vector<int> order = g_dock.tiles.items; for (int t = 0; t < tiles; ++t) order.push_back(t);
    const auto& line = EditorLine(*r);  // FPS-1: once a frame, not once per tile
    for (int t : order) {
        if (t < 0 || t >= tiles || DockOldShape(*r, t)) continue;
        auto& c = g_dockChecks[r->name + "#" + std::to_string(t)]; const auto sig = DockCheckSig(*r, line, t); if (sig.empty()) continue;
        if (c.sig == sig && (c.state == 2 || now < c.retryAt)) continue;
        c.sig = sig; DockCheckTile(*r, t, c); return;
    }
}
// The plan ledger (the driving session's planview.h): the first car on this route that has a plan, read-only, refreshed twice a second.
std::string DockLedger(const trafficdata::Route& r) {
    static std::string route, line; static ULONGLONG at = 0; const auto now = GetTickCount64();
    if (route == r.name && now - at < 500) return line;
    route = r.name; at = now; line.clear();
    const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); const auto query = addon ? reinterpret_cast<FvoTrafficInspectQueryV1>(GetProcAddress(addon, "FVO_Traffic_InspectV1")) : nullptr;
    if (!query) return line;
    for (int i = 0; i < 16; ++i) { FvoTrafficInspectV1 row; if (!query(r.name.c_str(), i, &row)) break; char b[160];
        if (row.vehicle && vehicleflight::PlanLedgerLine(row.vehicle, b, int(sizeof b))) { char id[16]; std::snprintf(id, sizeof id, "%08X: ", row.vehicle); line = id + std::string(b); break; } }
    return line;
}

// ---- layout ----
void DockButton(int id, double x, double y, double w, double h, const std::string& label, bool active, bool enabled, int style, int nav = 0, const std::string& value = {}, unsigned swatch = 0) {
    Button b{ id, x, y, w, h, label, active, enabled, KeyOf(id), style, value, swatch }; b.nav = nav; g_ui.buttons.push_back(b);
}
void DockText(const std::string& t, double x, double y, double w, unsigned color) { g_panelText.push_back({ f7layers::Fit(t, w, f7layers::kLetterPx), x, y, w, color, kUnit }); }
void DockLine(std::vector<std::pair<double, double>> pts, unsigned color, double alpha) { UiPolygon p; p.points = std::move(pts); p.color = color; p.alpha = alpha; g_ui.polygons.push_back(p); }
void DockStroke(std::pair<double, double> a, std::pair<double, double> b, double width, unsigned color, double alpha) {
    const double dx = b.first - a.first, dy = b.second - a.second, l = std::max(1e-6, std::hypot(dx, dy)), nx = -dy / l * width / 2, ny = dx / l * width / 2;
    DockLine({ { a.first + nx, a.second + ny }, { b.first + nx, b.second + ny }, { b.first - nx, b.second - ny }, { a.first - nx, a.second - ny } }, color, alpha);
}
// One cube, in the dock, in the style of the lane-type drawing: the tile as an open box, its lanes and arrows on the floor.
void DockCube(const trafficdata::Route& r, editorcontext::Rect area) {
    g_dock.faces.clear();
    const int t = g_dock.tiles.primary; if (t < 0) return;
    if (DockOldShape(r, t)) {  // D11 is not approved: never show an old rounded, tube or trapezoid tile as its enclosing box
        DockText(std::string("Stored shape: ") + roadplan::KindName(r.road.primitives[std::size_t(t)].kind), area.x + 4, area.y + 6, area.w - 8, kAmber);
        DockText("Not shown as a box until you", area.x + 4, area.y + 30, area.w - 8, kDim); DockText("approve that reading (D11).", area.x + 4, area.y + 48, area.w - 8, kDim);
        DockText("Choosing its Travel starts a box.", area.x + 4, area.y + 72, area.w - 8, kDim);
        return;
    }
    const auto box = DockBox(r, t); const int faces = box.faces, lanes = DockLanes(r);
    constexpr double kRad = 3.141592653589793 / 180;
    const double bx = area.x + area.w / 2, by = area.y + area.h / 2 + 6, sc = std::min(area.w / 3.9, area.h / 2.6), hx = .62, hy = 1.6;
    const double tw = DockValue(FTileWidth, r, t), th = DockValue(FTileHeight, r, t), hz = std::clamp(th / std::max(tw, 1.0), .3, .6);
    const double cy = std::cos(box.yaw * kRad + g_dock.az), sy = std::sin(box.yaw * kRad + g_dock.az), cp = std::cos(box.pitch * kRad), sp = std::sin(box.pitch * kRad), cr = std::cos(box.roll * kRad), sr = std::sin(box.roll * kRad), ce = std::cos(g_dock.el), se = std::sin(g_dock.el);
    struct V { double x, d, h; };
    auto T = [&](double vx, double vy, double vz) { double x = vx * hx, y = vy * hy, z = vz * hz;
        { const double a = x * cr + z * sr, b = -x * sr + z * cr; x = a; z = b; } { const double a = y * cp - z * sp, b = y * sp + z * cp; y = a; z = b; } { const double a = x * cy + y * sy, b = -x * sy + y * cy; x = a; y = b; }
        return V{ x, y * ce - z * se, y * se + z * ce }; };
    auto S2 = [&](double vx, double vy, double vz) { const auto q = T(vx, vy, vz); return std::pair<double, double>{ bx + q.x * sc, by - q.h * sc }; };
    { std::vector<std::pair<double, double>> plate; const double rx = area.w * .46, ry = rx * std::fabs(se), py = by + hz * sc * ce; for (int i = 0; i < 24; ++i) { const double a = 6.283185307179586 * i / 24; plate.push_back({ bx + std::cos(a) * rx, py + std::sin(a) * ry }); } DockLine(plate, 0x8C9AAB, .16); }
    constexpr double corners[6][4][3]{ { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 } }, { { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },
        { { -1, -1, -1 }, { -1, 1, -1 }, { -1, 1, 1 }, { -1, -1, 1 } }, { { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 } },
        { { -1, 1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { -1, 1, 1 } }, { { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } } };
    std::array<std::pair<int, double>, 6> order{};
    for (int i = 0; i < 6; ++i) { double d = 0; for (const auto& c : corners[i]) d += T(c[0], c[1], c[2]).d; order[std::size_t(i)] = { i, d / 4 }; }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::set<int> nearFaces{ order[0].first, order[1].first, order[2].first };
    for (const auto& [i, d] : order) {
        std::vector<std::pair<double, double>> pts; for (const auto& c : corners[i]) pts.push_back(S2(c[0], c[1], c[2]));
        const bool touch = f7dock::Touch(faces, i), isNear = nearFaces.count(i) != 0;
        DockLine(pts, touch ? kTeal : kAmber, isNear ? (touch ? .3 : .05) : (touch ? .42 : .1));
        g_dock.faces.push_back({ i, pts });
        (void)d;
    }
    for (int k = 1; k < lanes; ++k) { const double a = 1 - 2.0 * k / lanes; DockStroke(S2(a, -.97, -1), S2(a, .97, -1), 1.5, 0xE93452, .9); }
    for (int k = 0; k < lanes; ++k) {
        const double a = 1 - (2.0 * k + 1) / lanes; const int dir = roadplan::Direction(r.road, k, lanes) > 0 ? 1 : -1;
        DockStroke(S2(a, -.92, -1), S2(a, .92, -1), k == g_roadLane ? 4 : 2, kTeal, .95);
        const auto tip = S2(a, .2 * dir, -1); DockStroke(S2(a - .22, 0, -1), tip, 2, 0xFFFFFF, .95); DockStroke(S2(a + .22, 0, -1), tip, 2, 0xFFFFFF, .95);
    }
    for (const auto& [i, d] : order) if (nearFaces.count(i) || f7dock::Touch(faces, i)) {
        double x = 0, y = 0; for (const auto& c : corners[i]) { const auto p = S2(c[0], c[1], c[2]); x += p.first / 4; y += p.second / 4; }
        const std::string name = f7dock::FaceName(i); DockText(name, x - TextWidth(name) / 2, y - 4, TextWidth(name) + 4, f7dock::Touch(faces, i) ? kTeal : kAmber); (void)d;
    }
    const std::string type = f7dock::TravelName(faces, int(DockTileTravel(r, t)));
    if (!DockOldShape(r, t)) DockText(type, area.x + 4, area.y + 2, area.w / 2, 0xFFFFFF);
    if (box.solids) DockText("Solids allowed", area.x + area.w - TextWidth("Solids allowed") - 4, area.y + 2, TextWidth("Solids allowed") + 4, kAmber);
    if (DockDefaultBox(r, t)) DockText("Default box, not written: Update Route writes it", area.x + 4, area.y + area.h - 40, area.w - 8, kDim);  // the provider's isDefault
}
// R5: each menu's readme, the only side box (user, 2026-11-08: "readme for that menu, for example for routes, for landing, for tiles").
std::vector<std::pair<std::string, std::string>> DockReadme(int level) {
    switch (level) {
    case f7dock::Routes: return { { "Routes", "every route in this world; click one to open it." }, { "+ New route", "draw a new route (N)." }, { "City traffic", "on or off, how busy, how many at most." }, { "Prepare traffic", "applies the current traffic settings here (X)." }, { "Places", "landing areas, zones, junctions and shapes." } };
    case f7dock::Route: return { { "Driving", "name, the speed traffic wants, drive / hover / both, loop or open, how a line ends." }, { "Road", "width edge to edge and the divider between lanes; each lane's width is worked out for you." }, { "Lanes", "one way or two way, how many (1 to 16), which one is selected, which way it runs." }, { "Traffic", "cars on this route, which types, problems." }, { "Extras", "size, colour; Actions: copy, mirror, reverse, merge, original." }, { "", "Lanes under 2.2 m fit no vehicle: widen the road." } };
    case f7dock::Tile: return { { "Box", "the space traffic may use over this tile; faces Touch (drive on it) or Avoid." }, { "Cube", "click a face to switch it, drag to turn the view, Shift+drag to turn the box." }, { "Orientation", "yaw, pitch and roll of the box; Want speed on this tile only." }, { "Divider", "where the line between lanes sits on this tile." }, { "Warnings", "red tiles: a solid inside or the floor off a surface. Saving is never refused." } };
    case f7dock::Point: return { { "Position", "height, X and Y of the point; Above is its height over the ground." }, { "Corner", "sharp or round, and the radius." }, { "Disconnect", "cuts the route in two at this point." }, { "", "Ctrl+click adds points; double-click selects all of them." } };
    case f7dock::Types: return { { "Types", "click a vehicle type to leave it out of this route or allow it again." }, { "Health", "models the health check left out; you can allow them." } };
    case f7dock::Arrive: return { { "Arrive", "where a car on this line finishes: X, Y, Z and the heading it faces." }, { "Park", "Yes: it parks there; No: it stops and waits." } };
    case f7dock::City: return { { "Traffic", "on or off for this world." }, { "Density", "how busy, in percent." }, { "Limit", "how many vehicles at most." }, { "Original routes", "the game's own routes fly too, or only yours." } };
    case f7dock::Problems: return { { "Groups", "problems sit with the points or tiles they are on." }, { "Show", "rings it on the map. Fix applies the editor's fix; Undo brings it back." }, { "Fix all safe", "rounds sharp turns and eases steep climbs, one at a time." }, { "Items", "what lies in the road's space (objects, models, cars) and the drive preview." } };
    case f7dock::Places: return { { "Landing", "click the map to place a landing area; click one to change it." }, { "Zone", "click the map to place a keep-out or slow zone." }, { "Junction", "with a route open, click another route's line to join them." }, { "Shapes", "click the map for the centre, again for the size: a new route." }, { "Next", "picks the next landing, zone or junction of this world." }, { "", "Places are saved with the world. Traffic does not use them yet." } };
    case f7dock::Landing: return { { "Layout", "single, a row, bays or a ring of spaces; turn and height." }, { "Who lands", "taxi, depot or stop; how long they stay; what happens when it is full." }, { "Access", "entrance and departure paths to the spaces." }, { "Wires", "which routes arrive at or leave this landing." }, { "", "Drag it on the map with Landing on; the wheel turns it." } };
    case f7dock::Access: return { { "Entrance", "the route and lane it starts from, and its points: Draw, then click the map." }, { "Departure", "the same, from the spaces back to a route." }, { "Path", "the height of new points, the path width and its clearance." }, { "", "Draw points in travel order. Entrance ends at the spaces; departure begins there." } };
    case f7dock::Wires: return { { "In / Out", "a route that arrives at or leaves this landing." }, { "Next route", "moves the wire to the next route." }, { "Rebuild", "routes the wire again round what is in the way." }, { "Add", "a new wire to the nearest route." } };
    case f7dock::Zone: return { { "Keep out", "flying traffic stays above the height inside the circle." }, { "Slow", "a speed limit inside the circle." }, { "Radius", "the size of the circle; drag it on the map with Zone on." } };
    case f7dock::Junction: return { { "Type", "Both: either way; A > B: leave A for B; B > A: join A from B; Cross: they cross." }, { "Share turning", "how many cars take the junction." }, { "Lanes", "which lanes it joins; Any eligible lets traffic choose." }, { "Corner", "sharp or round, and the radius." } };
    case f7dock::Nodes: return { { "Wire", "drag a landing's socket to a route; to a landing makes a new route." }, { "Make route", "Ctrl+click nodes, then Make route (or Enter)." }, { "Tidy", "drag nodes; double-click puts one back. The world does not move." }, { "", "Del removes the selected wire. Nodes again goes back to the map." } };
    case f7dock::Hazards: return { { "Place", "click the surface: it is set down. Click again: the line from the first click to the cursor is its length and direction. Esc cancels." }, { "Hazard", "Q then click it to select, R to move it (Alt = up / down), PgUp / PgDn or the wheel near it set its height, Del deletes, Ctrl+Z undoes, Ctrl+click groups." }, { "Copy object", "click a game object: the Hazard takes its size and heading." }, { "Suggestions", "Test route shows what blocked the car; select one (Q), then Keep or Dismiss. Only kept ones are saved." } };
    case f7dock::Extras: return { { "Copy", "a copy beside this route." }, { "Mirror", "left and right swap, as the map shows it." }, { "Reverse", "traffic flies the other way." }, { "Merge", "joins the nearest open route that starts or ends near this one's end." }, { "Original", "edit the game's original as your copy, or go back to the original." } };
    case f7dock::TrafficLevels: return { { "Min / max", "vehicles per kilometre of this route. City traffic settings still apply." },
        { "All day", "uses the midpoint of the selected range." }, { "Scheduled", "keeps its hours and bounds each open period's count by the range." },
        { "Saved", "uses the route's original counts and hours." } };
    default: return {};
    }
}
// Help: what each field does, for the open menu (and, in the Route menu, its open group). Same side box as the Readme.
std::vector<std::pair<std::string, std::string>> DockHelp(int level, int group) {
    using V = std::vector<std::pair<std::string, std::string>>;
    switch (level) {
    case f7dock::Routes: return V{ { "Route row", "click to open that route; the dot is its colour on the map." }, { "+ New route", "starts drawing a route; click the map to add points." }, { "City traffic", "opens the on/off, density and limit for this whole world." }, { "Prepare traffic", "applies the current settings so cars spawn on the routes (X)." } };
    case f7dock::Route:
        if (group == 0) return V{ { "Name", "the route's name in the list and the files." }, { "Want speed", "the speed traffic aims for on this route, in m/s; a tile can set its own." }, { "Travel", "Both, Drive (wheels only) or Hover (flight only) for the whole route." }, { "Path", "loop joins the end back to the start; open stops at the last point." }, { "Line end", "what a car does at the end of an open route." } };
        if (group == 1) return V{ { "Width", "the whole road edge to edge in metres; each lane's width is worked out from it." }, { "Divider", "the red band between lanes in metres: cars must never touch it. 0 = a plain line." }, { "Left edge", "a no-go band along the road's left side in metres (amber on the map). 0 = none." }, { "Right edge", "the same along the right side." }, { "Each lane", "the lane width left after dividers and edges, and which vehicles fit." } };
        if (group == 2) return V{ { "Ways", "one way or two way." }, { "Lanes", "how many lanes across the road (1 to 16); dividers are added between them." }, { "Lane", "which lane the next change applies to." }, { "Runs", "which way the selected lane drives: along the markers or against them." } };
        if (group == 3) return V{ { "Traffic level", "how many cars per km, all day or on a schedule." }, { "Types", "which vehicle types may use this route." }, { "Problems", "warnings found on this route and how to fix them." } };
        return V{ { "Size", "scales the whole route, 10% a step." }, { "Colour", "the route's colour on the map." }, { "Actions", "copy, mirror, reverse, merge, or edit the game's original." } };
    case f7dock::Tile: return V{ { "Width", "how wide the box over this tile is, in metres." }, { "Height", "how tall the box is; 0 = flat floor." }, { "Length", "the length of the selected tiles (read only)." }, { "Travel", "Both, Drive or Hover on this tile only." }, { "Solids", "Block: cars stay out of solids. Allow: solids in the box are ignored." }, { "Yaw / Pitch / Roll", "turn the box left-right, up-down and sideways, in degrees." }, { "Want speed", "the speed on this tile only; 0 = use the route's." }, { "Divider", "which divider the fields below change (roads with 3+ lanes)." }, { "Side", "where the divider sits across the road, in percent." }, { "Swing", "how far the divider moves from one end of the tile to the other." }, { "Left / Right edge", "the no-go band along that road side in metres; one width for the whole route." }, { "Join", "Bend rounds the corner at this tile's start; Sharp keeps it square." } };
    case f7dock::Point: return V{ { "Height", "the point's height; Above shows it over the ground." }, { "X / Y", "the point's position on the map." }, { "Corner", "Sharp or Round, and the radius of the round." }, { "Disconnect", "cuts the route in two at this point." } };
    case f7dock::Types: return V{ { "Vehicle type", "click to leave it out of this route or allow it again." }, { "Health", "models the health check left out; you can allow them." } };
    case f7dock::Arrive: return V{ { "X / Y / Z", "where a car on this line finishes." }, { "Heading", "the direction it faces there, in degrees." }, { "Park", "Yes: it parks there. No: it stops and waits." } };
    case f7dock::City: return V{ { "Traffic", "on or off for the world." }, { "Density", "how busy, in percent." }, { "Limit", "the most vehicles at once." }, { "Use", "whether the game's own routes fly as well as yours." } };
    case f7dock::Hazards: return V{ { "Across / Along", "the Hazard's width and length in metres: type a number or use - / +." }, { "Height / Base", "how tall it is and where its floor sits: a Hazard can block one deck and leave the lane above open." }, { "Turn", "its heading in degrees; the arrow shows it." }, { "Test suggestions", "obstacles the test vehicle met; Keep adds them to the route." } };
    case f7dock::TrafficLevels: return V{ { "Min per km", "fewest cars per kilometre of this route." }, { "Max per km", "most cars per kilometre." }, { "This route", "applies the range to this route." }, { "Saved", "uses the route's original counts and hours." } };
    case f7dock::Zone: return V{ { "Kind", "Keep out or Slow." }, { "Radius", "size of the circle, in metres." }, { "Speed inside", "the speed limit in a Slow zone." }, { "Height / Stay above", "flying traffic stays above this height in a Keep-out zone." } };
    case f7dock::Junction: return V{ { "Type", "Both, A > B, B > A or Cross." }, { "Share turning", "how many cars take the junction." }, { "From / To lane", "which lanes it joins; Any lets traffic choose." }, { "Corner", "Sharp or Round, and the radius." } };
    case f7dock::Landing: return V{ { "Layout", "single, row, bays or ring of spaces." }, { "Spaces / Turn / Height", "how many spaces, which way they face, how high." }, { "Purpose / Parked for / When full", "who lands, how long they stay, what happens at capacity." } };
    case f7dock::Access: return V{ { "Route / Lane", "which route and lane the path starts from or joins." }, { "Points", "Draw, then click the map in travel order." }, { "New point Z / Path width / Clearance", "height of new points, how wide the path is, how much room it keeps." } };
    default: return DockReadme(level);
    }
}
void DockLayout(const trafficdata::Route* shown, const routemap::Camera& c) {
    (void)c;
    // Toolbar: tools only (Select, Add, Move, Route, Place, Nodes, Undo, Redo), side by side. Place and Nodes: cards R2, R3.
    std::vector<Button> tools; for (const auto& b : g_ui.buttons) if (b.style == 1 && (b.id == kSelect || b.id == kDrop || b.id == kDrag || b.id == kRouteTool || b.id == kF7NodeView || b.id == kUndo || b.id == kRedo)) tools.push_back(b);
    g_ui.buttons.clear(); g_ui.boxes.clear(); g_ui.polygons.clear(); g_panelText.clear(); g_contextRects.clear(); g_contextPanel = false; g_ed.menu = 0; g_dock.visibleFields.clear();
    g_dock.shownValues.assign(FFieldCount, std::string());
    { const auto route = std::find_if(tools.begin(), tools.end(), [](const Button& b) { return b.id == kRouteTool; });
      if (route != tools.end()) { Button p = *route; p.id = kDockPlaceTool; p.text = "Place"; p.key.clear(); p.active = f7dock::PlaceLevel(g_dock.level); p.enabled = true; tools.insert(route + 1, p); }
      std::stable_partition(tools.begin(), tools.end(), [](const Button& b) { return b.id != kUndo && b.id != kRedo; }); }  // Nodes before Undo / Redo
    { double x = g_ui.barX; for (auto b : tools) { if (b.id == kUndo) x += 8; b.x = x; b.w = 70; g_ui.buttons.push_back(b); x += 70; } g_ui.barW = std::max(70.0, x - g_ui.barX); }
    for (auto& a : g_f7AreaRect) a = {};
    const int level = g_dock.level; const auto* r = shown;
    const int n = r ? int(r->points.size()) : 0, tiles = r ? DockTiles(*r) : 0;
    const auto& types = VehicleTypesHere();
    const int list = ListTotal();
    constexpr double kSaveW = 170, kLabel = 104, kCubeW = 270, kRow = 28, kSaveStep = 30;
    const bool area1 = AreaOn(), zone1 = ZoneOn(), link1 = LinkOn();
    const routefiles::Spot* bay = area1 ? &g_ed.file.landings[std::size_t(g_ed.spot)] : nullptr;
    const routefiles::Zone* zone = zone1 ? &g_ed.file.zones[std::size_t(g_ed.zone)] : nullptr;
    const routefiles::Link* link = link1 ? &g_ed.file.links[std::size_t(g_ed.link)] : nullptr;
    const auto wires = bay ? placeaccess::Merge(bay->access, bay->wires) : std::vector<placeaccess::Wire>{};
    // ---- R7: the bar's height from what this menu shows (the save column no longer forces 240 px) ----
    const bool pickers = r && !r->ring && n > 0 && (level == f7dock::Route || level == f7dock::Tile || level == f7dock::Point);
    auto cardRows = [](int count) { return std::max(1, std::min(4, (count + 3) / 4)); };
    auto rowsH = [&](int rows) { return 42.0 + (pickers ? 32 : 0) + 20 + rows * kRow + 8; };
    int defaults = 0; std::string ledger; int healthOut = 0;
    if (r && level == f7dock::Route) { for (int t = 0; t < tiles; ++t) if (DockDefaultBox(*r, t)) ++defaults; ledger = DockLedger(*r); }
    if (level == f7dock::Types) for (const auto& t : types) { bool o = false; DockHealth(t, &o); healthOut += o; }
    const auto& problems = r && (level == f7dock::Problems || level == f7dock::Route) ? DockProblems(r) : g_dock.problems;
    // Problems rows (R6): the route's problems, then the tiles that warn, each with where it is and how bad.
    struct PRow { int problem = -1, tile = -1, sev = 2; std::string text; };
    std::vector<PRow> prow; std::vector<std::pair<int, int>> where; int sevCount[4]{};
    if (r && level == f7dock::Problems && g_dock.probView == 0) {
        for (std::size_t k = 0; k < problems.size(); ++k) { PRow p; p.problem = int(k); p.sev = DockSeverity(problems[k]); p.text = problems[k].text; prow.push_back(p); }
        if (!r->ring) { const auto& tline = EditorLine(*r); for (int t = 0; t < tiles; ++t) { if (DockOldShape(*r, t)) continue; const auto why = DockWarning(*r, tline, t); if (why.empty()) continue; PRow p; p.tile = t; p.sev = 2; p.text = "Tile " + std::to_string(t + 1) + ": " + why; prow.push_back(p); } }
        for (const auto& p : prow) ++sevCount[p.sev];
        std::erase_if(prow, [&](const PRow& p) { return g_dock.probFilter != 0 && p.sev != g_dock.probFilter; });
        for (const auto& p : prow) where.push_back(p.problem >= 0 ? std::pair<int, int>{ 0, problems[std::size_t(p.problem)].a } : std::pair<int, int>{ 1, p.tile });
    }
    const auto groups = f7dock::GroupRows(where);
    constexpr int kGroupCols = 2, kGroupRowsMax = 6;
    double h = 0;
    switch (level) {
    case f7dock::Routes: h = 42 + cardRows(std::min(list, 8) + 4) * 64.0 + (list > 8 ? 34 : 0); break;
    case f7dock::Types: h = 42 + cardRows(std::min(int(types.size()), 15) + 1 + (healthOut ? 1 : 0)) * 64.0 + 30; break;
    case f7dock::Problems: {
        if (g_dock.probView == 1) { h = 42 + 32 + 4 * 58.0 + 40; break; }  // RI-3's item list, as it was
        int most = 1; std::vector<int> colRows(kGroupCols, 0);
        for (const auto& g : groups) { auto& col = *std::min_element(colRows.begin(), colRows.end()); col += 1 + std::min<int>(kGroupRowsMax, int(g.rows.size())); }
        for (int v : colRows) most = std::max(most, v);
        h = 42 + std::min(most, 9) * 30.0 + 12; break;
    }
    case f7dock::Route: {
        // 2026-11-11: group tabs, then the open group in two columns of at most three rows (the Traffic notes: up to three).
        h = rowsH(3) + kDockTabH + 8 + (ledger.empty() ? 0 : 18); break;
    }
    case f7dock::Tile: h = 42 + 32 + std::max(20 + 5 * kRow, 190.0) + 26; break;
    case f7dock::Point: h = rowsH(4); break;
    case f7dock::Arrive: case f7dock::City: h = rowsH(3); break;
    case f7dock::Places: h = rowsH(3); break;
    case f7dock::Landing: h = rowsH(5); break;
    case f7dock::Access: h = rowsH(4) + (bay && !bay->access.unsupported.empty() ? 20 : 0); break;
    case f7dock::Wires: h = 42 + 20 + (std::min<int>(4, int(wires.size())) + 1) * 32.0 + (wires.size() > 4 ? 20 : 0) + 8; break;
    case f7dock::Zone: h = rowsH(4); break;
    case f7dock::Junction: h = rowsH(3); break;
    case f7dock::Nodes: h = rowsH(2); break;
    case f7dock::Extras: h = rowsH(2); break;
    case f7dock::TrafficLevels: h = rowsH(5); break;
    case f7dock::Hazards: h = rowsH(7); break;  // 7 rows in the Hazard box column (was 6: the Box row fell below the dock, 2026-11-21 screenshot)
    default: h = rowsH(3); break;
    }
    // Constellation Dock v2 (2026-11-24): unified bottom command strip replaces the tall right save column,
    // and a left domain rail gives 1-click jumps between Route, Tile, Point, Hazards, Places, and Problems.
    constexpr double kCmdBarH = 38.0, kRailW = 108.0;
    const bool placeMenu = f7dock::PlaceLevel(level) || level == f7dock::Nodes;
    const bool showRail = r != nullptr && !placeMenu && level != f7dock::Routes && level != f7dock::City && level != f7dock::Nodes;
    const int saveRows = 1;
    h += kCmdBarH;
    if (showRail) h = std::max(h, 42.0 + 6 * 28.0 + 12.0 + kCmdBarH);
    // The bar keeps its 1320 px cap (2026-11-11, user: "I wanted smarter layout not longer bar"); the Route menu shows one group at a time.
    const double w = std::min(1320.0, g_ui.width - 24);
    const editorcontext::Rect d{ (g_ui.width - w) / 2, g_ui.height - 12 - h, w, h };
    g_dock.rect = d; g_contextRects.push_back(d); g_f7AreaRect[1] = d;
    g_ui.boxes.push_back({ d.x, d.y, d.w, d.h, kDockBg, .97, kDockBorder, 1 });
    const double cmdY = d.y + d.h - kCmdBarH;
    const double contentBottom = cmdY;
    g_ui.boxes.push_back({ d.x + 1, cmdY, d.w - 2, kCmdBarH - 1, kWellBg, .92, 0, 0 });
    g_ui.boxes.push_back({ d.x + 1, cmdY, d.w - 2, 1, kDockBorder, .9, 0, 0 });
    // Breadcrumbs: going up has the up look; the last is where you are.
    double x = d.x + 12; const double y0 = d.y + 10;
    auto crumb = [&](int lvl, const std::string& label, bool last) { const double bw = TextWidth(label) + (last ? 20 : 58); DockButton(DockLevelId(lvl), x, y0, bw, 26, label, last, true, 0, last ? 0 : 2); x += bw; if (!last) { DockText(">", x + 5, y0 + 5, 12, kDim); x += 20; } };
    const std::string routeName = r ? (r->label.empty() ? r->name : r->label) : std::string();
    crumb(f7dock::Routes, "Routes", level == f7dock::Routes);
    if (level == f7dock::City) crumb(f7dock::City, "City", true);
    if (level == f7dock::Nodes) crumb(f7dock::Nodes, "Nodes", true);
    if (f7dock::PlaceLevel(level)) {
        crumb(f7dock::Places, "Places", level == f7dock::Places);
        if (level == f7dock::Landing || level == f7dock::Access || level == f7dock::Wires) crumb(f7dock::Landing, "Landing " + std::to_string(g_ed.spot + 1), level == f7dock::Landing);
        if (level == f7dock::Access) crumb(f7dock::Access, "Access", true);
        if (level == f7dock::Wires) crumb(f7dock::Wires, "Wires", true);
        if (level == f7dock::Zone) crumb(f7dock::Zone, "Zone " + std::to_string(g_ed.zone + 1), true);
        if (level == f7dock::Junction && link) crumb(f7dock::Junction, f7layers::Fit("Junction: " + RouteLabel(link->from) + " > " + RouteLabel(link->to), 360, f7layers::kLetterPx), true);
    }
    const int nSel = level == f7dock::Point ? g_dock.points.Count() : level == f7dock::Tile ? g_dock.tiles.Count() : 0;
    if (r && !f7dock::PlaceLevel(level) && level != f7dock::Routes && level != f7dock::City && level != f7dock::Nodes) {
        crumb(f7dock::Route, f7layers::Fit(routeName, 220, f7layers::kLetterPx), level == f7dock::Route);
        if (level == f7dock::Tile) { const int t = g_dock.tiles.primary; crumb(f7dock::Tile, nSel > 1 ? std::to_string(nSel) + " tiles" : "Tile " + std::to_string(t + 1) + "-" + std::to_string((t + 1) % std::max(1, n) + 1), true); }
        if (level == f7dock::Point) crumb(f7dock::Point, nSel > 1 ? std::to_string(nSel) + " points" : "Point " + std::to_string(g_dock.points.primary + 1), true);
        if (level == f7dock::Types) crumb(f7dock::Types, "Types", true);
        if (level == f7dock::Arrive) crumb(f7dock::Arrive, "Arrive", true);
        if (level == f7dock::Problems) crumb(f7dock::Problems, "Problems", true);
        if (level == f7dock::TrafficLevels) crumb(f7dock::TrafficLevels, "Traffic levels", true);
        if (level == f7dock::Extras) crumb(f7dock::Extras, "Actions", true);
        if (level == f7dock::Hazards) crumb(f7dock::Hazards, "Hazard", true);
    }
    // R5: Readme, Help, Script at the right of the header row (full dock width now that actions live in the bottom strip).
    const double readmeX = d.x + d.w - 12 - 84;
    DockButton(kDockReadme, readmeX, y0, 84, 26, "Readme", g_dock.readme, !DockReadme(level).empty(), 0);
    const double helpX = readmeX - 8 - 70;
    DockButton(kDockHelp, helpX, y0, 70, 26, "Help", g_dock.help, !DockHelp(level, g_dock.routeGroup).empty(), 0);
    // Script mode (2026-11-22): a toggle for every route; lit while on.
    const bool scriptOn = features::On(features::Id::ScriptMode);
    const double scriptX = helpX - 8 - 120;
    DockButton(kDockScript, scriptX, y0, 120, 26, scriptOn ? "Script mode: on" : "Script mode", scriptOn, true, 0);
    const double headRight = scriptX - 8;
    if (nSel > 1) { const std::string what = std::to_string(nSel) + (level == f7dock::Point ? " points" : " tiles") + " selected: edits apply to all"; DockText(what, x + 12, y0 + 5, TextWidth(what) + 4, kAmber); x += TextWidth(what) + 24; DockButton(kDockSelOne, x, y0, 104, 26, "Select one", false, true, 0); x += 112; }
    else if (level == f7dock::Point || level == f7dock::Tile) { DockButton(level == f7dock::Point ? kDockSelAllPoints : kDockSelAllTiles, x + 8, y0, 104, 26, level == f7dock::Point ? "All points" : "All tiles", false, true, 0); x += 120; }
    if (level == f7dock::Problems && r) {  // R6: the view, the filters with their counts, and Fix all safe, in the header row
        x += 12; DockButton(kDockProbView, x, y0, 84, 26, "Grouped", g_dock.probView == 0, true, 5); x += 86; DockButton(kDockProbView + 1, x, y0, 70, 26, "Items", g_dock.probView == 1, true, 5); x += 82;
        if (g_dock.probView == 0) {
            const int all = sevCount[1] + sevCount[2] + sevCount[3]; const char* names[4]{ "All", "Must fix", "Warnings", "Notes" };
            for (int f = 0; f < 4; ++f) { const std::string label = std::string(names[f]) + " " + std::to_string(f ? sevCount[f] : all); const double bw = TextWidth(label) + 18;
                if (x + bw > headRight - 140) break; DockButton(kDockProbFilter + f, x, y0, bw, 26, label, g_dock.probFilter == f, true, 5); x += bw + 4; }
            int safe = 0; for (const auto& p : problems) if (DockSafeFix(p)) ++safe;
            DockButton(kDockFixSafe, headRight - 130, y0, 130, 26, "Fix all safe (" + std::to_string(safe) + ")", safe > 0, safe > 0, 0);
        }
    }
    else if (placeMenu) {}
    else if (g_ed.draft) DockText("Editing, not saved yet", x + 12, y0 + 5, std::max(0.0, std::min(220.0, headRight - x - 12)), kAmber);
    else if (r) { char b[64]; std::snprintf(b, sizeof b, "%d pts", n); DockText(b, x + 12, y0 + 5, std::max(0.0, std::min(120.0, headRight - x - 12)), kDim); }
    // Unified Bottom Command Strip: left = Test / Update (or Done), right = Cancel, Save, Perm Save, Perm Delete, Delete.
    {
        const double cy = cmdY + 6;
        double lx = d.x + 12, rx = d.x + d.w - 12;
        auto cmdLeft = [&](int id, const std::string& label, double bw, bool enabled, bool active = false, int style = 0) {
            DockButton(id, lx, cy, bw, 26, label, active, enabled, style); lx += bw + 8;
        };
        auto cmdRight = [&](int id, const std::string& label, double bw, bool enabled, bool active = false, int style = 0) {
            rx -= bw; DockButton(id, rx, cy, bw, 26, label, active, enabled, style); rx -= 8;
        };
        if (placeMenu) {
            if (f7dock::PlaceSelected(level)) cmdLeft(kPlaceDone, "Done", 90, true, true);
            if (f7dock::PlaceSelected(level)) cmdRight(kDeletePoint, level == f7dock::Zone ? "Remove zone" : level == f7dock::Junction ? "Remove junction" : "Remove landing", 148, true, false, 7);
            cmdRight(kSave, "Save", 92, true, !f7dock::PlaceSelected(level));
            cmdRight(kCancel, "Cancel", 86, true);
        } else {
            cmdLeft(kDockTest, "Test (T)", 96, r != nullptr);
            cmdLeft(kDockUpdateRoute, "Update Route", 132, r && !r->ring && tiles > 0);
            if (level == f7dock::Point) cmdRight(kDeletePoint, nSel > 1 ? "Delete " + std::to_string(nSel) + " pts" : "Delete point", 136, true, false, 7);
            else if (level == f7dock::Route && r) { const bool armed = GetTickCount64() - g_ed.confirmDeleteAt < 3000; cmdRight(kDeleteRoute, armed ? "Click confirm" : "Delete route", 136, true, armed, 7); }
            cmdRight(kPermanentDelete, "Perm Delete", 118, r && g_ed.permanentFiles.count(r->name), false, 7);
            cmdRight(kPermanentSave, "Perm Save", 108, r != nullptr);
            cmdRight(kSave, "Save", 92, true, g_ed.draft.has_value());
            cmdRight(kCancel, "Cancel", 84, g_ed.draft.has_value());
        }
    }
    // Left Domain Rail (Constellation Dock v2): 1-click switching between Route, Tile, Point, Hazards, Places, Problems.
    double top = d.y + 42;
    const double railSpace = showRail ? kRailW + 14 : 0;
    const double bodyX = d.x + 12 + railSpace;
    const double area = d.w - 24 - railSpace;
    if (showRail) {
        double ry = top;
        g_ui.boxes.push_back({ d.x + 12 + kRailW + 6, top - 2, 1, contentBottom - top - 4, kDockBorder, .85, 0, 0 });
        auto railBtn = [&](int targetLvl, const std::string& label, bool enabled, int style = 5) {
            DockButton(DockLevelId(targetLvl), d.x + 12, ry, kRailW, 24, label, level == targetLvl, enabled, style);
            ry += 28;
        };
        railBtn(f7dock::Route, "Route", true);
        railBtn(f7dock::Tile, "Tile / Box", !r->ring && tiles > 0);
        railBtn(f7dock::Point, "Point", !r->ring && n > 0);
        railBtn(f7dock::Hazards, "Hazards", !r->ring);
        railBtn(f7dock::Places, "Places", true);
        const int probCnt = int(problems.size());
        railBtn(f7dock::Problems, probCnt > 0 ? "Health (" + std::to_string(probCnt) + ")" : "Health", true, probCnt > 0 && level != f7dock::Problems ? 6 : 5);
    }
    if (pickers) {
        double px = bodyX;
        auto picker = [&](int kind, const char* label, int count, int current) {
            const std::string of = "of " + std::to_string(count); const double pw = 8 + 50 + 30 + 68 + 34 + TextWidth(of) + 10;
            g_ui.boxes.push_back({ px - 6, top - 3, pw, 32, kTeal, .05, kTeal, .6 });
            DockText(label, px + 2, top + 5, 48, current >= 0 ? kTeal : 0xA3B1C6); px += 50;
            DockButton(kind == f7dock::Point ? kDockNavPointPrev : kDockNavTilePrev, px, top, 28, 26, "<", false, count > 0, 0, 1); px += 30;
            const bool typing = g_dock.entry.field == kDockPickerEntry + kind;
            DockButton(kind == f7dock::Point ? kDockPickPoint : kDockPickTile, px, top, 66, 26, typing ? g_dock.entry.text + "_" : current >= 0 ? std::to_string(current + 1) : "-", current >= 0 || typing, count > 0, 9, 1); px += 68;
            DockButton(kind == f7dock::Point ? kDockNavPointNext : kDockNavTileNext, px, top, 28, 26, ">", false, count > 0, 0, 1); px += 34;
            DockText(of, px, top + 5, TextWidth(of) + 4, kDim); px += TextWidth(of) + 30;
        };
        picker(f7dock::Point, "Point", n, level == f7dock::Point ? g_dock.points.primary : -1);
        if (tiles > 0) picker(f7dock::Tile, "Tile", tiles, level == f7dock::Tile ? g_dock.tiles.primary : -1);
        top += 32;
    }
    // Columns.
    const double left = level == f7dock::Tile ? kCubeW + 10 : 0;
    int cols = 2;
    switch (level) { case f7dock::Routes: case f7dock::Types: cols = 4; break; case f7dock::Route: cols = 2; break; case f7dock::Tile: case f7dock::Point: case f7dock::Places: case f7dock::Landing: case f7dock::Access: case f7dock::Junction: case f7dock::Extras: case f7dock::TrafficLevels: case f7dock::Hazards: cols = 3; break; default: cols = 2; break; }
    if (level == f7dock::Point && (!r || r->ring)) cols = 2;
    const double gap = 10, cw = (area - left - gap * (cols - 1)) / cols; auto cx = [&](int i) { return bodyX + left + i * (cw + gap); };
    // Labels take about a third of the column, between 92 and 132 px (10 to 14 letters), so a label never squeezes its values.
    const double labelW = std::clamp(std::floor(cw * 0.36), 92.0, 132.0);
    { static std::string told; char key[96]; std::snprintf(key, sizeof key, "%d/%d/%.0f/%.0f/%.0f", level, cols, d.w, cw, labelW);
      if (told != key) { told = key; logger::Write("OBS system=f7.dock event=layout level=%d cols=%d dock_w=%.0f column_w=%.0f label_w=%.0f value_w=%.0f letter_px=%.2f reason=%s",
          level, cols, d.w, cw, labelW, cw - labelW, f7layers::kLetterPx, cw - labelW < 150 ? "narrow_values_words_may_cut" : "fits"); } }
    char v[96];
    auto fmt = [&](const char* f, double value) { std::snprintf(v, sizeof v, f, value); return std::string(v); };
    struct Choice { int id; std::string text; bool on; int nav = 0; bool enabled = true; };
    auto column = [&](int i, const std::string& title, const std::function<void(double, double, double, double&)>& body) { const double x0 = cx(i); double y = top; DockText(title, x0, y, cw, kTeal); y += 20; body(x0, x0 + labelW, cw - labelW, y); };
    auto label = [&](const std::string& t, double x0, double y) { g_ui.boxes.push_back({ x0, y, labelW - 2, 26, kRowA, 1 }); DockText(t, x0 + 8, y + 5, labelW - 12, 0xCDD6DF); };
    auto valueBox = [&](int field, const std::string& shownValue, double bx, double by, double bw) {
        g_dock.shownValues[std::size_t(field)] = shownValue; g_dock.visibleFields.push_back(field);
        const bool typing = g_dock.entry.field == field;
        DockButton(kDockField + field * 4, bx, by, bw, 22, typing ? g_dock.entry.text + "_" : shownValue, typing, true, 9);
    };
    auto step = [&](double x0, double vx, double vw, double& y, const std::string& name, int field, const std::string& shownValue, bool enabled = true) {
        label(name, x0, y); g_ui.boxes.push_back({ vx, y, vw, 26, kRowB, 1 });
        DockButton(kDockField + field * 4 + 1, vx + 2, y + 2, 26, 22, "-", false, enabled, 0); valueBox(field, shownValue, vx + 30, y + 2, vw - 60); DockButton(kDockField + field * 4 + 2, vx + vw - 28, y + 2, 26, 22, "+", false, enabled, 0);
        y += kRow; };
    auto pick = [&](double x0, double vx, double vw, double& y, const std::string& name, const std::vector<Choice>& items) {
        label(name, x0, y); g_ui.boxes.push_back({ vx, y, vw, 26, kRowB, 1 }); const double iw = (vw - 4) / std::max<std::size_t>(1, items.size());
        for (std::size_t k = 0; k < items.size(); ++k) DockButton(items[k].id, vx + 2 + double(k) * iw, y + 2, iw - 2, 22, f7layers::Fit(items[k].text, iw - 8, f7layers::kLetterPx), items[k].on, items[k].enabled, 5, items[k].nav);
        y += kRow; };
    auto info = [&](double x0, double vx, double vw, double& y, const std::string& name, const std::string& text, unsigned color = 0xDFE7EE) { label(name, x0, y); g_ui.boxes.push_back({ vx, y, vw, 26, kRowB, 1 }); DockText(text, vx + 6, y + 5, vw - 12, color); y += kRow; };
    auto act = [&](double x0, double vx, double vw, double& y, const std::string& name, const std::string& text, int id, const std::string& button, int nav, bool enabled = true) { label(name, x0, y); g_ui.boxes.push_back({ vx, y, vw, 26, kRowB, 1 }); DockText(text, vx + 6, y + 5, vw - 82, 0xDFE7EE); DockButton(id, vx + vw - 70, y + 2, 68, 22, button, false, enabled, 0, nav); y += kRow; };
    // The editor's own - / + pair (an existing action id each way) around a shown value: the restored menus' steppers (R3, R4).
    auto old = [&](double x0, double vx, double vw, double& y, const std::string& name, int down, const std::string& text, int up, bool enabled = true) {
        label(name, x0, y); g_ui.boxes.push_back({ vx, y, vw, 26, kRowB, 1 });
        DockButton(down, vx + 2, y + 2, 26, 22, "-", false, enabled, 0); DockText(text, vx + 34, y + 5, vw - 68, enabled ? 0xDFE7EE : kDim); DockButton(up, vx + vw - 28, y + 2, 26, 22, "+", false, enabled, 0);
        y += kRow; };
    auto moveRow = [&](double x0, double vx, double vw, double& y) { pick(x0, vx, vw, y, "Move 5 m", { { kMoveLeft, "Left", false }, { kMoveRight, "Right", false }, { kMoveBack, "Back", false }, { kMoveForward, "Fwd", false } }); };
    auto note = [&](double x0, double& y, const std::string& text, unsigned color = 0x8C9AAB) { DockText(text, x0, y + 5, cw, color); y += kRow; };
    constexpr double cardH = 58; auto place = [&](int k, double& px, double& py) { px = cx(k % cols); py = top + (k / cols) * (cardH + 6); };
    if (level == f7dock::Routes) {
        const int per = 8; g_dock.cardFirst = std::clamp(g_dock.cardFirst, 0, std::max(0, list - 1)); int k = 0;
        for (int i = g_dock.cardFirst; i < list && k < per; ++i, ++k) {
            double px, py; place(k, px, py);
            const bool mine = i < int(g_ed.file.routes.size());
            std::string name, sum; unsigned swatch = 0; bool active = false;
            if (mine) { const auto& q = g_ed.file.routes[std::size_t(i)]; name = q.label.empty() ? q.id : q.label; active = g_ed.route == i;
                if (q.removed) sum = "Removed original"; else if (g_ed.unplaced.count(q.id)) sum = "City not here"; else { const auto t = ToTd(q, EditorKey()); std::snprintf(v, sizeof v, "%.0f m/s, %s veh, %d lane%s", t.speed, RouteVehicles(t).c_str(), DockLanes(t), DockLanes(t) == 1 ? "" : "s"); sum = v; }
                swatch = q.color; }
            else { const auto& o = g_ed.others[std::size_t(i - int(g_ed.file.routes.size()))]; name = o.name; active = g_ed.other == i - int(g_ed.file.routes.size()); sum = "Original: " + o.file; }
            DockButton(kRowBase + i, px, py, cw, cardH, f7layers::Fit(name, cw - 44, f7layers::kLetterPx), active, true, 8, 1, f7layers::Fit(sum, cw - 44, f7layers::kLetterPx), swatch);
        }
        double px, py; place(k, px, py); DockButton(kNew, px, py, cw, cardH, "+ New route", false, !g_ed.draft, 8, 1, "Draw a new route. N"); ++k;
        place(k, px, py); { const auto& city = g_ed.file.city; std::snprintf(v, sizeof v, "%d%%, up to %d", city.density, city.limit); DockButton(kDockLevelBase + f7dock::City, px, py, cw, cardH, "City traffic", false, true, 8, 1, city.on ? v : "Off"); } ++k;
        place(k, px, py); DockButton(kStartSwap, px, py, cw, cardH, "Prepare traffic", false, !g_ed.draft, 8, 0, "Apply traffic settings. X"); ++k;
        place(k, px, py); { std::snprintf(v, sizeof v, "%d landings, %d zones, %d junctions", int(g_ed.file.landings.size()), int(g_ed.file.zones.size()), int(g_ed.file.links.size()));
            DockButton(DockLevelId(f7dock::Places), px, py, cw, cardH, "Places", false, true, 8, 1, f7layers::Fit(v, cw - 44, f7layers::kLetterPx)); } ++k;  // R3
        if (list > per) { DockButton(kDockCardPrev, d.x + 12, d.y + d.h - 30, 90, 24, "Previous", false, g_dock.cardFirst > 0, 0); DockButton(kDockCardNext, d.x + 108, d.y + d.h - 30, 90, 24, "More", false, g_dock.cardFirst + per < list, 0);
            std::snprintf(v, sizeof v, "Routes %d-%d of %d", g_dock.cardFirst + 1, std::min(list, g_dock.cardFirst + per), list); DockText(v, d.x + 210, d.y + d.h - 26, 200, kDim); }
    }
    else if (level == f7dock::City) {
        const auto& city = g_ed.file.city;
        column(0, "City traffic", [&](double x0, double vx, double vw, double& y) { pick(x0, vx, vw, y, "Traffic", { { kDockCityOn, "On", city.on }, { kDockCityOff, "Off", !city.on } });
            step(x0, vx, vw, y, "Density", FDensity, std::to_string(city.density) + "%"); step(x0, vx, vw, y, "Limit", FLimit, std::to_string(city.limit)); });
        column(1, "Original routes", [&](double x0, double vx, double vw, double& y) { pick(x0, vx, vw, y, "Use", { { kDockOrigOn, "Use", city.originals }, { kDockOrigOff, "Leave out", !city.originals } }); });
    }
    else if (level == f7dock::Nodes) {  // R2: the existing Nodes view's picks and selected wire
        const int picks = NodePickCount(); const auto wire = NodeWireText();
        column(0, "Make a route", [&](double x0, double vx, double vw, double& y) { info(x0, vx, vw, y, "Picked", std::to_string(picks) + (picks == 1 ? " node" : " nodes"));
            pick(x0, vx, vw, y, "Picks", { { kDockNodeMake, "Make route", false, 0, picks >= 2 && !g_ed.draft }, { kDockNodeClear, "Clear", false, 0, picks > 0 } }); });
        column(1, "Wire", [&](double x0, double vx, double vw, double& y) { info(x0, vx, vw, y, "Selected", wire.empty() ? "None: click a wire" : wire, wire.empty() ? kDim : 0xDFE7EE);
            pick(x0, vx, vw, y, "Wire", { { kDockNodeRemove, "Remove wire", false, 0, !wire.empty() } }); });
    }
    else if (level == f7dock::Places) {  // R3: what can be placed, and what this world has
        const bool draft = g_ed.draft.has_value(), routeOk = r && !r->ring;
        column(0, "Add", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Place", { { kLanding, "Landing", g_ed.mode == Mode::Landing }, { kZoneTool, "Zone", g_ed.mode == Mode::Zone }, { kConnect, "Junction", g_ed.mode == Mode::Connect, 0, r != nullptr }, { kHazardTool, "Hazard", g_ed.mode == Mode::Hazard, 0, r != nullptr } }); });
        column(1, "Shapes", [&](double x0, double vx, double vw, double& y) {
            auto on = [&](int k) { return g_ed.mode == Mode::Shape && int(g_ed.shape) == k; };
            pick(x0, vx, vw, y, "Draw", { { kShapeBase + 0, "Circle", on(0), 0, !draft }, { kShapeBase + 1, "Ellipse", on(1), 0, !draft }, { kShapeBase + 2, "Rectangle", on(2), 0, !draft } });
            pick(x0, vx, vw, y, "More", { { kShapeBase + 3, "Helix", on(3), 0, !draft }, { kShapeBase + 4, "Ring", on(4), 0, !draft }, { kShapeBase + 5, "Parallel", false, 0, routeOk && !draft } }); });
        column(2, "In this world", [&](double x0, double vx, double vw, double& y) {
            act(x0, vx, vw, y, "Landings", std::to_string(g_ed.file.landings.size()), kDockPlaceNext + 0, "Next", 1, !g_ed.file.landings.empty());
            act(x0, vx, vw, y, "Zones", std::to_string(g_ed.file.zones.size()), kDockPlaceNext + 1, "Next", 1, !g_ed.file.zones.empty());
            act(x0, vx, vw, y, "Junctions", std::to_string(g_ed.file.links.size()), kDockPlaceNext + 2, "Next", 1, !g_ed.file.links.empty()); });
    }
    else if (level == f7dock::Landing && bay) {  // R3: the old landing area panel's fields, in the bar
        const auto& a = *bay; const bool single = a.layout == "single";
        column(0, "Layout", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Layout", { { kLayoutSingle, "Single", single }, { kLayoutRow, "Row", a.layout == "row" }, { kLayoutBays, "Bays", a.layout == "bays" }, { kLayoutRing, "Ring", a.layout == "ring" } });
            old(x0, vx, vw, y, "Spaces", kSpacesDown, std::to_string(single ? 1 : a.spaces), kSpacesUp, !single);
            old(x0, vx, vw, y, "Turn", kAreaTurnDown, fmt("%.0f deg", a.turn), kAreaTurnUp, !single);
            old(x0, vx, vw, y, "Height", kZDown, fmt("%.0f m", a.at.z), kZUp);
            moveRow(x0, vx, vw, y); });
        column(1, "Who lands", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Purpose", { { kPurposeTaxi, "Taxi", a.purpose == "taxi" }, { kPurposeDepot, "Depot", a.purpose == "depot" }, { kPurposeStop, "Stop", a.purpose == "stop" } });
            pick(x0, vx, vw, y, "Parked for", { { kStayShort, "2 min", a.stay == 120 }, { kStayLong, "10 min", a.stay == 600 }, { kStayNeeded, "Until", a.stay != 120 && a.stay != 600 } });
            pick(x0, vx, vw, y, "When full", { { kFullQueue, "Queue", a.full == "queue" }, { kFullCircle, "Circle", a.full == "circle" }, { kFullOn, "Go on", a.full == "on" } });
            act(x0, vx, vw, y, "Who parks", a.routes.empty() ? std::string("All routes") : RouteLabel(a.routes.front()), kWhoParks, "Change", 0); });
        column(2, "Connect", [&](double x0, double vx, double vw, double& y) {
            act(x0, vx, vw, y, "Access", std::to_string(a.access.entrance.size()) + " in, " + std::to_string(a.access.departure.size()) + " out", DockLevelId(f7dock::Access), "Open", 1);
            int in = 0; for (const auto& wv : wires) in += wv.socket == 0;
            act(x0, vx, vw, y, "Wires", wires.empty() ? std::string("Not connected") : std::to_string(in) + " in, " + std::to_string(int(wires.size()) - in) + " out", DockLevelId(f7dock::Wires), "Open", 1); });
    }
    else if (level == f7dock::Access && bay) {  // R3: the old parking-path sheet, in the bar
        const auto& ac = bay->access; const bool edit = ac.unsupported.empty();
        auto laneText = [](int lane) { return lane < 0 ? std::string("Any compatible") : std::to_string(lane + 1); };
        auto side = [&](int i, bool entry) {
            column(i, entry ? "Entrance" : "Departure", [&](double x0, double vx, double vw, double& y) {
                const auto& route = entry ? ac.entryRoute : ac.exitRoute; const auto& path = entry ? ac.entrance : ac.departure;
                const bool drawing = g_ed.mode == (entry ? Mode::AccessEntry : Mode::AccessExit);
                act(x0, vx, vw, y, "Route", route.empty() ? std::string("Unbound") : RouteTitle(route), entry ? kAccessEntryRoute : kAccessExitRoute, "Next", 0, edit);
                act(x0, vx, vw, y, "Lane", laneText(entry ? ac.entryLane : ac.exitLane), entry ? kAccessEntryLane : kAccessExitLane, "Next", 0, edit);
                act(x0, vx, vw, y, "Points", std::to_string(path.size()) + (drawing ? ": click the map" : ""), drawing ? kAccessDone : entry ? kAccessEntry : kAccessExit, drawing ? "Stop" : "Draw", 0, edit);
                act(x0, vx, vw, y, "Last point", path.empty() ? std::string("None") : fmt("%.0f m high", path.back().z), entry ? kAccessUndoEntry : kAccessUndoExit, "Remove", 0, edit && !path.empty()); });
        };
        side(0, true); side(1, false);
        column(2, "Path", [&](double x0, double vx, double vw, double& y) {
            old(x0, vx, vw, y, "New point Z", kAccessHeightDown, routefiles::Num(g_ed.accessHeight) + " m", kAccessHeightUp, edit);
            old(x0, vx, vw, y, "Path width", kAccessWidthDown, routefiles::Num(ac.width) + " m", kAccessWidthUp, edit);
            old(x0, vx, vw, y, "Clearance", kAccessClearanceDown, routefiles::Num(ac.height) + " m", kAccessClearanceUp, edit);
            if (!edit) note(x0, y, "Stored by a newer version: kept, not editable", kAmber); });
    }
    else if (level == f7dock::Wires && bay) {  // R3: the landing's wires (was the side panel F7WirePanel), one row each
        double y = top; const double x0 = bodyX, tw = std::min(420.0, area - 3 * 116 - 12);
        DockText("Wires of " + bay->id, x0, y, area, kTeal); y += 20;
        const int shownWires = std::min<int>(4, int(wires.size()));
        for (int i = 0; i < shownWires; ++i) { const auto& one = wires[std::size_t(i)];
            g_ui.boxes.push_back({ x0, y, tw, 26, kRowB, 1 });
            DockText(f7layers::Fit(std::string(one.socket ? "Out - " : "In - ") + RouteLabel(one.route) + (one.lane >= 0 ? " lane " + std::to_string(one.lane + 1) : ""), tw - 12, f7layers::kLetterPx), x0 + 6, y + 5, tw - 12, 0xDFE7EE);
            const char* ops[]{ "Next route", "Rebuild", "Remove" }; for (int k = 0; k < 3; ++k) DockButton(kDockWire + i * 4 + k, x0 + tw + 8 + k * 116, y + 1, 110, 24, ops[k], false, true, k == 2 ? 7 : 0);
            y += 32; }
        if (int(wires.size()) > shownWires) { DockText(std::to_string(int(wires.size()) - shownWires) + " more wires: remove or move one to see them", x0, y, area, kDim); y += 20; }
        if (wires.empty()) { DockText("Not connected: add a wire to join a route.", x0, y + 5, tw, kAmber); }
        const double ax = wires.empty() ? x0 + tw + 8 : x0; DockButton(kDockWireAddIn, ax, y, 140, 26, "Add In wire", false, true, 0); DockButton(kDockWireAddOut, ax + 148, y, 140, 26, "Add Out wire", false, true, 0);
    }
    else if (level == f7dock::Zone && zone) {  // R3: the old zone panel's fields
        const auto& z = *zone;
        column(0, "Zone", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Kind", { { kZoneKeep, "Keep out", !z.slow }, { kZoneSlow, "Slow", z.slow } });
            old(x0, vx, vw, y, "Radius", kZoneSizeDown, fmt("%.0f m", z.radius), kZoneSizeUp);
            if (z.slow) { old(x0, vx, vw, y, "Speed inside", kZoneSpeedDown, fmt("%.0f m/s", z.speed), kZoneSpeedUp); old(x0, vx, vw, y, "Height", kZDown, fmt("%.0f m", z.at.z), kZUp); }
            else old(x0, vx, vw, y, "Stay above", kZoneTopDown, fmt("%.0f m", z.top), kZoneTopUp); });
        column(1, "Place", [&](double x0, double vx, double vw, double& y) { moveRow(x0, vx, vw, y); });
    }
    else if (level == f7dock::Junction && link) {  // R3: the old junction panel's fields
        const auto& k = *link;
        auto laneName = [](int lane) { return lane < 0 ? std::string("Any eligible") : std::string((lane & 1) ? "Reverse " : "Forward ") + std::to_string(lane / 2 + 1); };
        column(0, "Type", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Type", { { kJunctionBoth, "Both", k.type.empty() }, { kJunctionExit, "A > B", k.type == "exit" }, { kJunctionMerge, "B > A", k.type == "merge" }, { kJunctionCross, "Cross", k.type == "crossing" } });
            if (const auto best = SuggestedType(k); best != k.type) act(x0, vx, vw, y, "Suggested", best.empty() ? "Both" : best == "exit" ? "A > B" : best == "merge" ? "B > A" : "Cross", kJunctionSuggest, "Use", 0);
            old(x0, vx, vw, y, "Share turning", kShareDown, std::to_string(int(std::lround(k.share * 100))) + "%", kShareUp); });
        column(1, "Lanes", [&](double x0, double vx, double vw, double& y) {
            old(x0, vx, vw, y, "From lane", kLinkLaneFromDown, laneName(k.fromLane), kLinkLaneFromUp); old(x0, vx, vw, y, "To lane", kLinkLaneToDown, laneName(k.toLane), kLinkLaneToUp); });
        column(2, "Corner", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Corner", { { kSharp, "Sharp", k.radius <= 0 }, { kRounded, "Round", k.radius > 0 } });
            if (k.radius > 0) old(x0, vx, vw, y, "Radius", kRoundDown, fmt("%.0f m", k.radius), kRoundUp); });
    }
    else if (f7dock::PlaceSelected(level)) DockText("Nothing selected: Places lists them.", d.x + 12, top + 6, area, kDim);
    else if (!r) {}
    else if (level == f7dock::Types) {
        int k = 0;
        for (; k < int(types.size()) && k < 15; ++k) { double px, py; place(k, px, py); const bool out = TypeIn(r->exclude, types[std::size_t(k)]); const auto above = TypeAbove(*r, types[std::size_t(k)]);
            const auto health = DockHealth(types[std::size_t(k)]);
            DockButton(kDockType + k, px, py, cw, cardH, f7layers::Fit(types[std::size_t(k)].name, cw - 24, f7layers::kLetterPx), !out && above.empty(), true, 8, 0, !above.empty() ? f7layers::Fit(above, cw - 24, f7layers::kLetterPx) : out ? "Left out" : !health.empty() ? f7layers::Fit(health, cw - 24, f7layers::kLetterPx) : "Allowed"); }
        double px, py; place(k, px, py); DockButton(kDockAllowAll, px, py, cw, cardH, "Allow all", false, !r->exclude.empty(), 8, 0, "Clear what's left out");
        if (healthOut && k + 1 < 16) { place(k + 1, px, py); DockButton(kDockHealthAllow, px, py, cw, cardH, "Allow Health's", false, true, 8, 0, std::to_string(healthOut) + " left out by Health"); }
        if (types.empty()) DockText("No vehicle types found yet: they are read once the game has loaded its vehicles.", d.x + 12, d.y + d.h - 26, area, kAmber);
        else if (types.size() > 15) DockText(std::to_string(types.size() - 15) + " more types: not shown here yet.", d.x + 12, d.y + d.h - 26, area, kDim);
    }
    else if (level == f7dock::Problems && g_dock.probView == 1) ItemsLayout(*r, d, top, area);  // RI-3, RI-5, HV-3, HV-4
    else if (level == f7dock::Problems) {  // R6: grouped by where they are, the old list's Show and Fix on each row
        const double gw = (area - gap * (kGroupCols - 1)) / kGroupCols; std::vector<double> colY(kGroupCols, top);
        const double bottom = d.y + d.h - 8;
        if (groups.empty()) DockText(sevCount[1] + sevCount[2] + sevCount[3] ? "None of this kind." : "No problems found. Buildings are checked by Test.", d.x + 12, top + 6, area, kDim);
        for (const auto& g : groups) {
            const int ci = int(std::min_element(colY.begin(), colY.end()) - colY.begin()); const double gx = d.x + 12 + ci * (gw + gap); double& y = colY[std::size_t(ci)];
            if (y + 30 > bottom) continue;
            const std::string head = g.kind == 2 ? "Whole route" : (g.kind == 0 ? (g.first == g.last ? "Point " + std::to_string(g.first + 1) : "Points " + std::to_string(g.first + 1) + " to " + std::to_string(g.last + 1))
                : (g.first == g.last ? "Tile " + std::to_string(g.first + 1) : "Tiles " + std::to_string(g.first + 1) + " to " + std::to_string(g.last + 1)));
            DockText(head, gx, y + 6, gw, kTeal); y += 30;
            int shownRows = 0;
            for (int ri : g.rows) {
                if (shownRows >= kGroupRowsMax || y + 28 > bottom) { DockText(std::to_string(int(g.rows.size()) - shownRows) + " more here: fix these first", gx + 8, y + 5, gw, kDim); y += 30; break; }
                const auto& p = prow[std::size_t(ri)]; const unsigned sevColor = p.sev == 1 ? kClearanceRed : p.sev == 2 ? kAmber : 0x9FB1BC;
                const Problem* pb = p.problem >= 0 ? &problems[std::size_t(p.problem)] : nullptr;
                const std::string fix = pb && pb->kind ? pb->fix : std::string(); const double fw = fix.empty() ? 0 : std::min(150.0, TextWidth(fix) + 18);
                const double showW = 60, tx = gx + 18, textW = gw - 18 - showW - (fw > 0 ? fw + 6 : 0) - 10;
                g_ui.boxes.push_back({ gx, y, gw, 26, kRowB, 1 }); g_ui.boxes.push_back({ gx + 6, y + 9, 8, 8, sevColor, 1 });
                DockText(f7layers::Fit(p.text, textW, f7layers::kLetterPx), tx, y + 5, textW, 0xDFE7EE);
                double bx = gx + gw - 4 - (fw > 0 ? fw + 6 : 0) - showW;
                if (pb) DockButton(kDockProbShow + p.problem, bx, y + 2, showW, 22, "Show", g_dock.probShown == p.problem, pb->a >= 0, 0);
                else DockButton(kDockProbTile + p.tile, bx, y + 2, showW, 22, "Open", false, true, 0, 1);
                if (fw > 0) DockButton(kDockProbFix + p.problem, gx + gw - 4 - fw, y + 2, fw, 22, f7layers::Fit(fix, fw - 8, f7layers::kLetterPx), true, true, 0);
                y += 30; ++shownRows;
            }
        }
        if (g_dock.probShown >= 0 && g_dock.probShown < int(problems.size())) DockText(f7layers::Fit(problems[std::size_t(g_dock.probShown)].text, area, f7layers::kLetterPx), bodyX, contentBottom - 20, area, kAmber);
    }
    else if (level == f7dock::Arrive) {
        const auto p = r->end.hasPlace ? r->end.place : DockArriveDefault(*r);
        column(0, "Place", [&](double x0, double vx, double vw, double& y) { step(x0, vx, vw, y, "X", FAX, fmt("%.1f m", p.x)); step(x0, vx, vw, y, "Y", FAY, fmt("%.1f m", p.y)); step(x0, vx, vw, y, "Z", FAZ, fmt("%.1f m", p.z)); });
        column(1, "Finish", [&](double x0, double vx, double vw, double& y) { step(x0, vx, vw, y, "Heading", FAH, fmt("%.0f deg", p.heading)); pick(x0, vx, vw, y, "Park", { { kDockPark + 1, "Yes", p.park == 1 }, { kDockPark, "No", p.park == 0 } });
            if (!r->end.placeUnsupported.empty()) note(x0, y, "Stored place unreadable: kept until set", kAmber); });
    }
    else if (level == f7dock::Hazards && r) {  // hazard boxes (hazardedit.inl): the route's boxes, the selected one, and what Test route suggested
            const auto boxes = HazardsNow(); const auto sugg = HazardSuggestions(); const bool one = g_ed.hazard >= 0 && g_ed.hazard < int(boxes.boxes.size());
            const routehazard::Box hb = one ? boxes.boxes[std::size_t(g_ed.hazard)] : routehazard::Box{};
            column(1, "Hazard", [&](double x0, double vx, double vw, double& y) {
                info(x0, vx, vw, y, "Placed", std::to_string(boxes.boxes.size()) + (one ? ", #" + std::to_string(g_ed.hazard + 1) + " selected" : ", none selected (Q, click one)"));
                step(x0, vx, vw, y, "Across", FHzAcross, one ? fmt("%.2f m", 2 * hb.hx) : std::string("-"), one);   // typed or - / +, like Width and Divider
                step(x0, vx, vw, y, "Along", FHzAlong, one ? fmt("%.2f m", 2 * hb.hy) : std::string("-"), one);
                step(x0, vx, vw, y, "Height", FHzHeight, one ? fmt("%.1f m", 2 * hb.hz) : std::string("-"), one);
                step(x0, vx, vw, y, "Base", FHzBase, one ? fmt("%.1f m", hb.z - hb.hz) : std::string("-"), one);
                step(x0, vx, vw, y, "Turn", FHzTurn, one ? fmt("%.0f deg", hb.yaw) : std::string("-"), one);
                pick(x0, vx, vw, y, "Hazard", { { kHazardInherit, "Copy object", g_ed.hazardCopy }, { kHazardDelete, "Delete", false, 0, one }, { kHazardDone, "Done", false } }); });
            column(2, "Test suggestions", [&](double x0, double vx, double vw, double& y) {
                info(x0, vx, vw, y, "Suggested", std::to_string(sugg.size()) + (sugg.empty() ? ": run Test (T), then watch" : " from the last test"), sugg.empty() ? kDim : 0xF2B04E);
                pick(x0, vx, vw, y, "Pick", { { kHazardNextSuggest, "Next", false, 0, !sugg.empty() } });
                pick(x0, vx, vw, y, "Selected", { { kHazardKeep, "Keep", false, 0, g_ed.suggest >= 0 }, { kHazardDismiss, "Dismiss", false, 0, g_ed.suggest >= 0 } });
                pick(x0, vx, vw, y, "All", { { kHazardKeepAll, "Keep all", false, 0, !sugg.empty() } }); });
        column(0, "Hazard", [&](double x0, double vx, double vw, double& y) {
            info(x0, vx, vw, y, "Route", r->label.empty() ? r->name : r->label);
            pick(x0, vx, vw, y, "Place", { { kHazardPlace, g_ed.hazardLine.active ? "Click again" : "Place", g_ed.mode == Mode::Hazard }, { kHazardInherit, "Copy object", g_ed.hazardCopy }, { kHazardDone, "Done", false } });
            act(x0, vx, vw, y, "Test route", "flies it once", kTest, "Test", 0, !r->ring);
            info(x0, vx, vw, y, "Keys", "Q select, R move, PgUp/Dn height, Del", kDim); });
    }
    else if (level == f7dock::Extras) {  // R4: the route's actions (existing ids; each handler keeps its own checks and messages)
        const bool draft = g_ed.draft.has_value();
        const bool edited = g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size()) && IsOriginal(g_ed.file.routes[std::size_t(g_ed.route)]);
        column(0, "Change", [&](double x0, double vx, double vw, double& y) {
            pick(x0, vx, vw, y, "Shape", { { kCopy, "Copy", false, 0, !draft }, { kMirror, "Mirror", false, 0, !r->ring }, { kReverse, "Reverse", false, 0, !r->ring && n >= 2 } });
            pick(x0, vx, vw, y, "Join", { { kJoin, "Merge nearest", false, 0, !r->ring } }); });
        column(1, "Original", [&](double x0, double vx, double vw, double& y) {
            act(x0, vx, vw, y, "Original", g_ed.other >= 0 ? "The game's route" : edited ? "Your version" : "Your own route", kEditOriginal, "Edit", 0, !draft && g_ed.route < 0 && g_ed.other >= 0);
            act(x0, vx, vw, y, "Restore", edited ? "Back to the original" : "Nothing to restore", kRestore, "Restore", 0, !draft && edited); });
    }
    else if (level == f7dock::TrafficLevels && r) {
        const auto& s = r->schedule;
        const double metres = trafficdata::TrafficLength(*r);
        for (int i = routeschedule::Quiet; i < routeschedule::LevelCount; ++i) {
            const auto band = s.trafficBands[i];
            const bool active = s.trafficLevel == i;
            const int low = routeschedule::ScaleRate(band.low, metres), high = routeschedule::ScaleRate(band.high, metres);
            column(i, routeschedule::LevelName(i), [&](double x0, double vx, double vw, double& y) {
                step(x0, vx, vw, y, "Min per km", FTrafficBand + 2 * i, std::to_string(band.low));
                step(x0, vx, vw, y, "Max per km", FTrafficBand + 2 * i + 1, std::to_string(band.high));
                act(x0, vx, vw, y, "This route", std::to_string(low) + "-" + std::to_string(high) + " vehicles",
                    kTrafficLevelPick + i + 1, active ? "Selected" : "Use", 0);
                if (i == routeschedule::Quiet)
                    act(x0, vx, vw, y, "Saved", "Original counts", kTrafficLevelPick, routeschedule::HasLevel(s) ? "Use" : "Selected", 0);
            });
        }
        DockText(fmt("Route %.2f km", metres / 1000) + "  |  All day uses the midpoint. Scheduled routes keep their hours.", d.x + 12, d.y + d.h - 24, area, kDim);
    }
    else if (level == f7dock::Route) {
        // 2026-11-11 (user: "I wanted smarter layout not longer bar"): five groups side by side left each about 217 px and cut
        // every word. Now a strip of five group tabs, each with a one-line summary (so every value is still seen at a glance),
        // and only the open group's controls below, in two wide columns. Same controls, ids and actions as before.
        const int lanes = DockLanes(*r); g_roadLane = std::clamp(g_roadLane, 0, lanes - 1);
        const double laneW = (DockRoadWidth(*r) - r->road.median * (lanes - 1)) / lanes; const bool none = laneW < kLaneNoneFit, tight = laneW < kLaneAllFit;
        int out = 0; for (const auto& t : types) if (TypeIn(r->exclude, t)) ++out;
        const int problemCount = int(problems.size());
        g_dock.routeGroup = std::clamp(g_dock.routeGroup, 0, kDockRouteGroups - 1);
        { const char* names[kDockRouteGroups]{ "Driving", "Road", "Lanes", "Traffic", "Extras" };
          std::string sums[kDockRouteGroups];
          sums[0] = fmt("%.0f m/s, ", r->speed) + routepolicy::Name(r->travel) + (r->ring ? ", ring" : r->closed ? ", loop" : ", open");
          sums[1] = fmt("%.1f m wide", DockRoadWidth(*r)) + (none ? ", no fit" : tight ? ", small only" : "");
          sums[2] = lanes == 1 ? std::string("1 lane, one way") : std::to_string(lanes) + " lanes";
          sums[3] = std::string(routeschedule::LevelName(r->schedule.trafficLevel)) + (problemCount ? ", " + std::to_string(problemCount) + (problemCount == 1 ? " problem" : " problems") : std::string());
          sums[4] = "Size, colour, actions";
          const double tw = (area - 4 * 6) / kDockRouteGroups;
          for (int k = 0; k < kDockRouteGroups; ++k) {
              const double tx = bodyX + k * (tw + 6);
              DockButton(kDockRouteGroup + k, tx, top, tw, kDockTabH, f7layers::Fit(names[k], tw - 24, f7layers::kLetterPx), k == g_dock.routeGroup, true, 8, 0, f7layers::Fit(sums[k], tw - 24, f7layers::kLetterPx));
          }
          top += kDockTabH + 8; }
        const int group = g_dock.routeGroup;
        const double routeLenM = std::max(100.0, trafficdata::TrafficLength(*r));
        const bool routeIsSky = r->travel == routepolicy::Travel::Hover;
        const int paceNow = trafficlook::DetectPace(r->speed, routeIsSky);
        const int spacingNow = trafficlook::DetectSpacing(r->schedule, r->speed, lanes, routeLenM);
        const auto roadLook = trafficlook::EvaluateRoadLook(r->schedule, r->speed, lanes, routeLenM);
        auto left = [&](const std::function<void(double, double, double, double&)>& body) { const double x0 = cx(0); double y = top; body(x0, x0 + labelW, cw - labelW, y); };
        auto right = [&](const std::function<void(double, double, double, double&)>& body) { const double x0 = cx(1); double y = top; body(x0, x0 + labelW, cw - labelW, y); };
        if (group == 0) {
            left([&](double x0, double vx, double vw, double& y) {
                act(x0, vx, vw, y, "Name", routeName, kRename, "Edit", 0);
                step(x0, vx, vw, y, "Want speed", FSpeed, fmt("%.0f m/s", r->speed));
                pick(x0, vx, vw, y, "Pace", { { kDockLookPace + 0, "Stroll", paceNow == 0 }, { kDockLookPace + 1, "City", paceNow == 1 }, { kDockLookPace + 2, "Express", paceNow == 2 } }); });
            right([&](double x0, double vx, double vw, double& y) {
                pick(x0, vx, vw, y, "Travel", { { kDockTravel + 0, "Both", r->travel == routepolicy::Travel::Both }, { kDockTravel + 1, "Drive", r->travel == routepolicy::Travel::Drive }, { kDockTravel + 2, "Hover", r->travel == routepolicy::Travel::Hover } });
                if (!r->ring) pick(x0, vx, vw, y, "Path", { { kLoopOn, "Loop", r->closed }, { kLoopOff, "Open", !r->closed, 0 } });
                if (!r->ring && !r->closed) pick(x0, vx, vw, y, "Line end", { { kDockEnd + 0, "Pass", r->end.end == routeend::Pass }, { kDockEnd + 1, "Stop", r->end.end == routeend::Stop }, { kDockEnd + 2, "Arrive", r->end.end == routeend::Arrive, 1 } });
                if (r->ring) note(x0, y, "A ring is always a loop."); });
        }
        else if (group == 1) {
            left([&](double x0, double vx, double vw, double& y) {
                step(x0, vx, vw, y, "Width", FRoadWidth, fmt("%.1f m", DockRoadWidth(*r)), !r->ring); step(x0, vx, vw, y, "Divider", FMedian, fmt("%.1f m", r->road.median), !r->ring && lanes > 1);
                step(x0, vx, vw, y, "Left edge", FEdgeLeft, fmt("%.1f m", r->road.edgeLeft), !r->ring); step(x0, vx, vw, y, "Right edge", FEdgeRight, fmt("%.1f m", r->road.edgeRight), !r->ring); });
            right([&](double x0, double vx, double vw, double& y) {
                std::snprintf(v, sizeof v, "%.1f m, %s", laneW, none ? "no vehicle fits" : tight ? "small vehicles only" : "all vehicles fit"); info(x0, vx, vw, y, "Each lane", v, none ? kClearanceRed : tight ? kAmber : 0xDFE7EE);
                act(x0, vx, vw, y, "Hazard", std::to_string(r->hazards.boxes.size()) + " placed, no-go volumes", kHazardTool, "Open", 1, !r->ring);  // 2026-11-21: under Road, where the user looks for it (was Extras)
                if (none || tight) note(x0, y, none ? "Widen the road or use fewer lanes." : "Widen the road for large vehicles.", none ? kClearanceRed : kAmber); });
        }
        else if (group == 2) {
            left([&](double x0, double vx, double vw, double& y) {
                pick(x0, vx, vw, y, "Ways", { { kDockOneWay, "One way", lanes == 1 }, { kDockTwoWay, "Two way", lanes == 2 } });  // one lane is a button, not a hunt
                step(x0, vx, vw, y, "Lanes", FLanes, std::to_string(lanes), true); });
            right([&](double x0, double vx, double vw, double& y) {
                step(x0, vx, vw, y, "Lane", FLane, std::to_string(g_roadLane + 1), lanes > 1);
                const bool along = roadplan::Direction(r->road, g_roadLane, lanes) > 0; pick(x0, vx, vw, y, "Runs", { { kDockAlong, "Along", along }, { kDockAgainst, "Against", !along } }); });
        }
        else if (group == 3) {
            left([&](double x0, double vx, double vw, double& y) {
                pick(x0, vx, vw, y, "Spacing", { { kDockLookSpacing + 0, "Sparse", spacingNow == 0 }, { kDockLookSpacing + 1, "Calm", spacingNow == 1 }, { kDockLookSpacing + 2, "Lively", spacingNow == 2 }, { kDockLookSpacing + 3, "Busy", spacingNow == 3 } });
                act(x0, vx, vw, y, "Fine bands", routeschedule::LevelName(r->schedule.trafficLevel), DockLevelId(f7dock::TrafficLevels), "Edit", 1);
                act(x0, vx, vw, y, "Types", r->exclude.empty() ? "All" : std::to_string(out) + " out", kDockLevelBase + f7dock::Types, "Open", 1); });
            right([&](double x0, double vx, double vw, double& y) { (void)vx; (void)vw;
                note(x0, y, trafficlook::FormatLookLine(roadLook), kTeal);
                if (!r->end.endUnsupported.empty()) note(x0, y, "Stored line end unreadable: kept", kAmber);
                int warn = 0, done = 0, all = 0; const auto& tline = EditorLine(*r);  // FPS-1: the line once, not twice per tile a frame
                for (int t = 0; t < DockTiles(*r); ++t) { if (DockOldShape(*r, t)) continue; ++all; if (DockCheckOf(*r, tline, t)) ++done; if (DockWarns(*r, tline, t)) ++warn; }
                if (warn) note(x0, y, std::to_string(warn) + " tile" + (warn == 1 ? "" : "s") + " warn: red on the map", kClearanceRed);
                else if (done < all) note(x0, y, "Checking tiles " + std::to_string(done) + " / " + std::to_string(all));
                else note(x0, y, "Tiles clear.", kEmerald);
                if (defaults) note(x0, y, std::to_string(defaults) + " tile" + (defaults == 1 ? " uses" : "s use") + " default box: Update Route writes them"); });
        }
        else {
            left([&](double x0, double vx, double vw, double& y) {  // R4
                // Role hidden 2026-11-11 (user: "also for now remove route role"). The stored role, its file field and the kRoleNext
                // action are kept unchanged, so saved routes load, save and drive exactly as before; only the control is gone.
                { static std::string told; if (told != r->name) { told = r->name; logger::Write("OBS system=f7.dock event=role_hidden route=%s stored_role=%s reason=user_removed_control_20261111", r->name.c_str(), r->role.empty() ? "civilian" : r->role.c_str()); } }
                old(x0, vx, vw, y, "Size", kSizeDown, "10% a step", kSizeUp, !r->ring);
                act(x0, vx, vw, y, "Colour", "", kColor, "Next", 0);
                g_ui.boxes.push_back({ vx + 8, y - kRow + 6, 40, 14, RouteColor(*r, ShownIndex()), 1 }); });
            right([&](double x0, double vx, double vw, double& y) { act(x0, vx, vw, y, "Actions", "Copy, mirror, reverse", DockLevelId(f7dock::Extras), "Open", 1); });
        }
        // LEDGER LINE: the plan's own account of the first planned car on this route (planview.h), read-only, under any group.
        if (!ledger.empty()) DockText(ledger, bodyX, contentBottom - 20, area, kDim);
        { static int toldGroup = -1; if (toldGroup != group) { toldGroup = group; logger::Write("OBS system=f7.dock event=route_group group=%d column_w=%.0f label_w=%.0f reason=open_group_only_two_columns", group, cw, labelW); } }
    }
    else if (level == f7dock::Tile) {
        const int t = g_dock.tiles.primary; const int lanes = DockLanes(*r); g_roadDivider = std::clamp(g_roadDivider, 0, std::max(0, lanes - 2));
        g_ui.boxes.push_back({ bodyX, top, kCubeW, contentBottom - top - 6, kWellBg, .55, kDockBorder, .7 });
        DockCube(*r, { bodyX, top, kCubeW, contentBottom - top - 6 });
        {  // the tile's warning under the cube: why, never a refusal
            int warn = 0; const auto& tline = EditorLine(*r); for (int k : g_dock.tiles.items) if (DockWarns(*r, tline, k)) ++warn;  // FPS-1: the line once
            const auto* chk = DockCheckOf(*r, tline, t); std::string text = DockWarning(*r, tline, t); unsigned color = kDim;
            if (!text.empty()) { color = kClearanceRed; } else if (chk) text = chk->solid ? "Solids inside, allowed" : "Box clear, floor on a surface"; else if (!DockOldShape(*r, t)) text = "Checking this tile...";
            if (g_dock.tiles.Many() && warn) { text = std::to_string(warn) + " of " + std::to_string(g_dock.tiles.Count()) + " tiles warn. " + text; color = kClearanceRed; }
            if (!text.empty()) DockText(text, bodyX + 2, contentBottom - 20, kCubeW + 10 + (area - kCubeW) / 3, color);
        }
        const auto box = DockBox(*r, t); const auto tl = roadplan::DividerAt(r->road, std::size_t(g_roadDivider), std::size_t(t));
        auto mix = [&](int f, const std::string& s) { return DockMixed(f, *r) ? std::string("mixed") : s; };
        auto all = [&](auto fn) { return std::all_of(g_dock.tiles.items.begin(), g_dock.tiles.items.end(), fn); };
        double length = 0; for (int k : g_dock.tiles.items) { const auto& a = r->points[std::size_t(k)]; const auto& b = r->points[std::size_t((k + 1) % n)]; length += std::hypot(a.x - b.x, a.y - b.y, a.z - b.z); }
        column(0, "Box", [&](double x0, double vx, double vw, double& y) {
            step(x0, vx, vw, y, "Width", FTileWidth, mix(FTileWidth, fmt("%.1f m", DockValue(FTileWidth, *r, t)))); step(x0, vx, vw, y, "Height", FTileHeight, mix(FTileHeight, fmt("%.1f m", DockValue(FTileHeight, *r, t))));
            info(x0, vx, vw, y, "Length", fmt(g_dock.tiles.Many() ? "%.0f m in all" : "%.0f m", length));
            auto sec = [&](routepolicy::Travel want) { return all([&](int k) { return k < int(r->sections.size()) && r->sections[std::size_t(k)].travel == want; }); };
            pick(x0, vx, vw, y, "Travel", { { kDockTileTravel + 0, "Both", sec(routepolicy::Travel::Both) }, { kDockTileTravel + 1, "Drive", sec(routepolicy::Travel::Drive) }, { kDockTileTravel + 2, "Hover", sec(routepolicy::Travel::Hover) } });
            pick(x0, vx, vw, y, "Solids", { { kDockSolids, "Block", all([&](int k) { return DockBox(*r, k).solids == 0; }) }, { kDockSolids + 1, "Allow", all([&](int k) { return DockBox(*r, k).solids == 1; }) } }); });
        column(1, "Orientation", [&](double x0, double vx, double vw, double& y) {
            step(x0, vx, vw, y, "Yaw", FYaw, mix(FYaw, fmt("%.0f deg", box.yaw))); step(x0, vx, vw, y, "Pitch", FPitch, mix(FPitch, fmt("%.0f deg", box.pitch))); step(x0, vx, vw, y, "Roll", FRoll, mix(FRoll, fmt("%.0f deg", box.roll)));
            step(x0, vx, vw, y, "Want speed", FTileSpeed, mix(FTileSpeed, fmt("%.0f m/s", DockValue(FTileSpeed, *r, t)))); note(x0, y, "0 = line. Shift+drag rotates"); });
        column(2, "Divider", [&](double x0, double vx, double vw, double& y) {
            if (lanes > 2) step(x0, vx, vw, y, "Divider", FDivider, std::to_string(g_roadDivider + 1));
            if (lanes > 1) { step(x0, vx, vw, y, "Side", FSide, mix(FSide, fmt("%.0f %%", tl.side * 100))); step(x0, vx, vw, y, "Swing", FSwing, mix(FSwing, fmt("%.0f %%", tl.swing * 100))); }
            else note(x0, y, "One lane: no divider");
            if (lanes <= 2 && !r->ring) { step(x0, vx, vw, y, "Left edge", FEdgeLeft, fmt("%.1f m", r->road.edgeLeft)); step(x0, vx, vw, y, "Right edge", FEdgeRight, fmt("%.1f m", r->road.edgeRight)); }  // edge no-go bands, amber on the map; 0 = none
            auto join = [&](int k) { return roadplan::DividerAt(r->road, std::size_t(g_roadDivider), std::size_t(k)).connector == 1; };
            pick(x0, vx, vw, y, "Join", { { kDockJoin, "Bend", all(join) }, { kDockJoin + 1, "Sharp", all([&](int k) { return !join(k); }) } }); });
    }
    else if (level == f7dock::Point) {
        const int i = g_dock.points.primary; const auto& p = r->points[std::size_t(i)];
        auto mix = [&](int f, const std::string& s) { return DockMixed(f, *r) ? std::string("mixed") : s; };
        auto endOf = [&](int k) { return !r->closed && (k == 0 || k + 1 == n); };
        const bool allEnds = std::all_of(g_dock.points.items.begin(), g_dock.points.items.end(), endOf);
        column(0, "Position", [&](double x0, double vx, double vw, double& y) {
            step(x0, vx, vw, y, "Height", FPZ, mix(FPZ, fmt("%.0f m", p.z))); step(x0, vx, vw, y, "X", FPX, mix(FPX, fmt("%.0f m", p.x))); step(x0, vx, vw, y, "Y", FPY, mix(FPY, fmt("%.0f m", p.y)));
            double ground = 0; info(x0, vx, vw, y, "Above", g_dock.points.Many() ? "per point" : CachedTerrain(p.x, p.y, ground) ? fmt("%.0f m", p.z - ground) : std::string("not known yet")); });
        column(1, "Corner", [&](double x0, double vx, double vw, double& y) {
            if (allEnds) { note(x0, y, "Route ends have no corner"); return; }
            auto round = [&](int k) { return !endOf(k) && k < int(r->rounding.size()) && r->rounding[std::size_t(k)] > 0; };
            pick(x0, vx, vw, y, "Corner", { { kDockSharp, "Sharp", std::none_of(g_dock.points.items.begin(), g_dock.points.items.end(), round) }, { kDockRound, "Round", std::all_of(g_dock.points.items.begin(), g_dock.points.items.end(), [&](int k) { return endOf(k) || round(k); }) } });
            step(x0, vx, vw, y, "Radius", FRadius, mix(FRadius, fmt("%.0f m", DockValue(FRadius, *r, i))));
            if (g_dock.points.Many() && std::any_of(g_dock.points.items.begin(), g_dock.points.items.end(), endOf)) note(x0, y, "Route ends are left sharp"); });
        if (cols >= 3) column(2, "Route", [&](double x0, double vx, double vw, double& y) {  // R4: Disconnect cuts the route at the selected points (the editor's DisconnectRoute)
            act(x0, vx, vw, y, "Disconnect", g_dock.points.Many() ? "at " + std::to_string(nSel) + " points" : "at this point", kSplit, "Cut", 0); });
    }
    // R5: the open menu's readme, the one side box, right-aligned just above the bar, sized to its lines.
    g_dock.readmeRect = {};
    if (g_dock.readme || g_dock.help) {
        const bool helpOpen = g_dock.help;
        static const char* kGroupNames[] = { "Driving", "Road", "Lanes", "Traffic", "Extras" };
        auto lines = helpOpen ? DockHelp(level, g_dock.routeGroup) : DockReadme(level);
        if (!helpOpen && !lines.empty()) {  // lead the reader to where they are
            std::string here = level == f7dock::Extras ? "actions" : f7dock::LevelName(level);
            if (!here.empty()) here[0] = char(std::toupper(static_cast<unsigned char>(here[0])));
            if (level == f7dock::Route) here += std::string(" > ") + kGroupNames[std::clamp(g_dock.routeGroup, 0, 4)];
            lines.insert(lines.begin(), { "You are here", here + ". Help (next to Readme) explains every field on this screen." });
        }
        if (!lines.empty()) {
            constexpr double rw = 400, lineH = 18; const int perLine = int((rw - 28) / f7layers::kLetterPx);
            std::vector<std::pair<std::string, unsigned>> out;
            for (const auto& [head, text] : lines) {
                std::string rest = head.empty() ? text : head + ": " + text; bool first = true;
                while (!rest.empty()) { std::size_t cut = rest.size(); if (int(cut) > perLine) { cut = rest.rfind(' ', std::size_t(perLine)); if (cut == std::string::npos || cut == 0) cut = std::size_t(perLine); }
                    out.push_back({ (first ? "" : "  ") + rest.substr(0, cut), first && !head.empty() ? 0xFFFFFFu : 0x9FB1BCu }); rest = cut < rest.size() ? rest.substr(cut + (rest[cut] == ' ' ? 1 : 0)) : std::string(); first = false; }
            }
            const double rh = 40 + out.size() * lineH + 8, rx = d.x + d.w - rw, ry = d.y - 10 - rh;
            const editorcontext::Rect box{ rx, ry, rw, rh }; g_dock.readmeRect = box; g_contextRects.push_back(box);
            g_ui.boxes.push_back({ rx, ry, rw, rh, kDockBg, .97, kDockBorder, 1 });
            { std::string title = level == f7dock::Extras ? "actions" : f7dock::LevelName(level); if (!title.empty()) title[0] = char(std::toupper(static_cast<unsigned char>(title[0]))); DockText((helpOpen ? "Help: " : "Readme: ") + title, rx + 14, ry + 12, rw - 110, kTeal); }
            DockButton(helpOpen ? kDockHelp : kDockReadme, rx + rw - 84, ry + 8, 70, 24, "Close", false, true, 0);
            double ly = ry + 40; for (const auto& [t, col] : out) { DockText(t, rx + 14, ly, rw - 28, col); ly += lineH; }
        }
    }
    static int lastLevel = -1, lastH = -1; if (lastLevel != level || lastH != int(h)) { lastLevel = level; lastH = int(h); DockTrace("bar", "sized_to_content", int(h), saveRows); }
}

// ---- mouse ----
bool DockInPolygon(const std::vector<std::pair<double, double>>& p, double x, double y) {
    bool in = false; for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) { const auto& a = p[i]; const auto& b = p[j]; if ((a.second > y) != (b.second > y) && x < (b.first - a.first) * (y - a.second) / (b.second - a.second) + a.first) in = !in; }
    return in;
}
// A click that hit no button. true: the dock took it.
bool DockClick(const routemap::Camera& c, double mx, double my, bool ctrl, bool shift) {
    if (!g_dock.on) return false;
    if (g_dock.entry.Active()) DockCommitEntry();
    if (g_dock.level == f7dock::Tile) for (auto it = g_dock.faces.rbegin(); it != g_dock.faces.rend(); ++it) if (DockInPolygon(it->second, mx, my)) {
        trafficdata::Route scratch; const auto* r = Shown(scratch);
        g_dock.cubeDrag = 1; g_dock.dragFace = it->first; g_dock.dragX = mx; g_dock.dragY = my; g_dock.dragAz = g_dock.az; g_dock.dragEl = g_dock.el; g_dock.dragMoved = false; g_dock.dragRotate = shift;
        if (r) g_dock.dragBox = DockBox(*r, g_dock.tiles.primary); return true;
    }
    if (g_dock.rect.Contains(mx, my)) return true;  // the dock's own background
    if (OverUi(mx, my)) return false;
    if (g_ed.mode != Mode::Select && g_ed.mode != Mode::Drag) return false;  // Add, Route and the rest keep their own clicks
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    const bool twice = g_dock.clicks.Double(GetTickCount64(), mx, my);
    const int p = r && !r->ring ? PointAt(c, mx, my, 14) : -1;
    if (p >= 0) {
        if (twice) { DockSelectAll(f7dock::Points, "double_click"); return true; }
        if (ctrl) { DockFlip(f7dock::Points, p); return true; }
        if (g_ed.mode == Mode::Drag) { if (!(g_dock.level == f7dock::Point && g_dock.points.Has(p))) DockSelectOne(f7dock::Points, p, "click_point"); else { g_dock.points.primary = p; DockSyncPicks(); } return false; }  // the editor's drag moves it (and every selected point with it)
        DockSelectOne(f7dock::Points, p, "click_point");
        const auto& q = r->points[std::size_t(p)]; char b[96]; std::snprintf(b, sizeof b, "Point %d: (%.0f, %.0f), height %.0f m", p + 1, q.x, q.y, q.z); Status(b);
        return true;
    }
    // A Hazard under the click (its footprint), before the tile: a Hazard usually sits ON the road, so the road line under it must not take the click. Select picks it,
    // Ctrl+click goes to the editor's own group (PickClick), the Move tool drags it (StartPlaceDrag). Points above stay first.
    if (r && !r->ring && HazardAt(c, mx, my) >= 0) {
        if (ctrl || g_ed.mode == Mode::Drag) return false;
        return HazardSelectAt(c, mx, my);
    }
    if (r && !r->ring && g_ed.mode == Mode::Select && !ctrl && HazardSuggestAt(c, mx, my) >= 0) return HazardSelectAt(c, mx, my);  // an amber test suggestion, the same way
    auto tileHere = [&](const trafficdata::Route& route) {
        std::vector<f7dock::Screen> sc; for (const auto& q : route.points) { f7dock::Screen s; s.seen = Local(c, { q.x, q.y, q.z }, s.x, s.y); sc.push_back(s); }
        return f7dock::TileAt(sc, route.closed, mx, my, 10); };
    int t = r && !r->ring ? tileHere(*r) : -1;
    if (t >= 0) {
        if (twice) { DockSelectAll(f7dock::Tiles, "double_click"); return true; }
        if (ctrl) { DockFlip(f7dock::Tiles, t); return true; }
        DockSelectOne(f7dock::Tiles, t, "click_line"); Status("Tile " + std::to_string(t + 1) + "-" + std::to_string((t + 1) % int(r->points.size()) + 1) + ": its box and faces are in the dock");
        return true;
    }
    if (ctrl) { Status("Ctrl+click adds or removes a point or tile of the selected route"); DockTrace("refused", "ctrl_click_missed"); return true; }
    // Not on the shown route: the editor's own selection (another route, a junction, a place), then the tile under the click.
    ClickMap(c, mx, my); DockSync();
    trafficdata::Route scratch2; const auto* now = Shown(scratch2);
    if (now && now != r && !now->ring && g_ed.point < 0) { t = tileHere(*now); if (t >= 0) DockSelectOne(f7dock::Tiles, t, "click_other_route"); }
    return true;
}
void DockMouseTick(double mx, double my, bool held, bool release) {
    if (!g_dock.cubeDrag) return;
    const double dx = mx - g_dock.dragX, dy = my - g_dock.dragY;
    if (std::hypot(dx, dy) > 4) g_dock.dragMoved = true;
    if (g_dock.dragMoved && held) {
        if (g_dock.dragRotate) {  // Shift+drag rotates the box and writes Yaw and Pitch on every selected tile
            if (g_ed.draft || EnsureDraft()) { auto r = g_ed.draft->Value(); if (DockEnsureBoxes(r)) {
                const double yaw = std::clamp(std::round(g_dock.dragBox.yaw + dx * .5), -180.0, 180.0), pitch = std::clamp(std::round(g_dock.dragBox.pitch - dy * .5), -180.0, 180.0);
                for (int t : g_dock.tiles.items) if (t >= 0 && t < int(r.road.boxes.size())) { r.road.boxes[std::size_t(t)].yaw = yaw; r.road.boxes[std::size_t(t)].pitch = pitch; }
                if (g_dock.cubeDrag == 1) { g_ed.draft->Edit(r); g_dock.cubeDrag = 2; } else g_ed.draft->Amend(r); } }
        }
        else { g_dock.az = g_dock.dragAz + dx * .01; g_dock.el = std::clamp(g_dock.dragEl + dy * .01, -1.4, 1.4); }
    }
    if (release || !held) {
        if (!g_dock.dragMoved) DockToggleFace(g_dock.dragFace);
        else if (g_dock.dragRotate) { Status("Box turned: Yaw and Pitch set (Save keeps it)"); DockTrace("rotate", "shift_drag", g_dock.tiles.Count()); }
        g_dock.cubeDrag = 0; g_dock.dragFace = -1;
    }
}

// ---- keys: Esc goes up a level, Home / End step. Typed values first. ----
bool DockKeys(bool focused, bool ctrl) {
    if (!g_dock.on) return false;
    DockEntryInput(focused);
    if (DockTyping() || ctrl || g_ed.naming) return false;
    if (g_context.popup != editorcontext::Popup::None || g_ed.sheet) return false;
    if (g_dock.level == f7dock::Nodes) return false;  // the Nodes view keeps its own Esc (clears its picks); the Nodes tool leaves it
    if (g_keys.Pressed(VK_ESCAPE, focused)) {
        trafficdata::Route escScratch; const bool haveRoute = Shown(escScratch) != nullptr;
        if (g_dock.readme) { g_dock.readme = false; DockTrace("readme", "close_esc"); return true; }
        if (g_dock.help) { g_dock.help = false; DockTrace("help", "close_esc"); return true; }
        switch (g_dock.level) {
        // R3: a place's menus go up to Places; Places ends a place tool first, then goes back to the route (or the routes).
        case f7dock::Access: case f7dock::Wires: DockGo(f7dock::Landing, "esc"); break;
        case f7dock::Landing: case f7dock::Zone: case f7dock::Junction: DockGo(f7dock::Places, "esc"); break;
        case f7dock::Places:
            if (g_ed.mode == Mode::Landing || g_ed.mode == Mode::Zone || g_ed.mode == Mode::Connect || g_ed.mode == Mode::Shape) { g_ed.mode = Mode::Select; g_ed.shapeAt = false; Status("Select: click a place to change it"); DockTrace("place", "tool_off_esc"); }
            else DockGo(haveRoute ? f7dock::Route : f7dock::Routes, "esc");
            break;
        case f7dock::Extras: case f7dock::TrafficLevels: DockGo(f7dock::Route, "esc"); break;
        case f7dock::Hazards:  // Esc steps back one thing at a time: the pending first click, the copy switch, the selection, then the tool and the level
            if (HazardEscape()) { DockTrace("hazard", "esc_step"); break; }
            g_ed.mode = Mode::Select; g_ed.hazard = -1; g_ed.suggest = -1; g_ed.hazardCopy = false; hazardpick::Cancel(g_ed.hazardLine); DockGo(haveRoute ? f7dock::Route : f7dock::Routes, "esc"); break;
        case f7dock::Tile: case f7dock::Point: case f7dock::Types: case f7dock::Arrive: case f7dock::Problems: DockGo(f7dock::Route, "esc"); break;
        case f7dock::Route: if (g_ed.draft) { Status("Save or Cancel the open route first"); DockTrace("refused", "esc_draft_open"); } else { g_ed.route = -1; g_ed.other = -1; g_dock.routeKey.clear(); DockGo(f7dock::Routes, "esc"); } break;
        case f7dock::City: DockGo(f7dock::Routes, "esc"); break;
        default: break;
        }
        return true;
    }
    trafficdata::Route scratch; const auto* r = Shown(scratch);
    const bool home = g_keys.Pressed(VK_HOME, focused), end = g_keys.Pressed(VK_END, focused);
    if ((home || end) && r && (g_dock.level == f7dock::Tile || g_dock.level == f7dock::Point)) {
        const bool point = g_dock.level == f7dock::Point; const int count = point ? int(r->points.size()) : DockTiles(*r);
        const int to = f7dock::Step(point ? g_dock.points.primary : g_dock.tiles.primary, end ? 1 : -1, count);
        if (to >= 0) DockSelectOne(point ? f7dock::Points : f7dock::Tiles, to, "home_end");
    }
    return false;
}

// 2026-11-19 (user: "anything that affects the route I need to see on that overlay"): one translucent world-space face on the map or photo
// view, cut at the camera like the road tiles' own faces. Shared by the tile boxes below and DrawRouteFacts in routeedit.cpp.
void MapFace(Pen& pen, const routemap::Camera& camera, const routemap::Vec3* v, int count, unsigned color, double alpha) {
    const auto poly = routemap::ClipPolygon(camera, std::vector<routemap::Vec3>(v, v + count));
    if (poly.size() < 3) return;
    double x[16], y[16]; int n = 0;
    for (const auto& vertex : poly) { if (n >= 16 || !Local(camera, vertex, x[n], y[n])) return; ++n; }
    double x0 = x[0], x1 = x[0], y0 = y[0], y1 = y[0]; for (int i = 1; i < n; ++i) { x0 = std::min(x0, x[i]); x1 = std::max(x1, x[i]); y0 = std::min(y0, y[i]); y1 = std::max(y1, y[i]); }
    if (pen.tape && pen.cull && (pen.Outside(x0, y0, x1, y1) || (x1 - x0 < 1.5 && y1 - y0 < 1.5))) { ++pen.culled; return; }
    pen.NoLine(); pen.Fill(color, alpha); pen.Move(x[0], y[0]); for (int i = 1; i < n; ++i) pen.Line(x[i], y[i]); pen.Line(x[0], y[0]); pen.End();
}
// The selected tile's real field box on the map: what traffic is told (tilebox.h through DockWorldBox, the same geometry the Field Provider
// answers with). Touch faces teal, Avoid faces faint amber (as the dock's cube), edges in the tile's travel colour, a hatch on the floor when solids are
// allowed, one label. Old rounded / tube / trapezoid tiles without a stored box have none (D11 not approved) and draw nothing.
void DockDrawTileBox(Pen& pen, const routemap::Camera& c, const trafficdata::Route& r, const trafficroute::Line& line, int t, bool primary, std::vector<std::string>* labels, std::vector<std::array<double, 3>>* where) {
    fieldbox::Box fb; if (!DockWorldBox(r, line, t, fb)) return;
    routemap::Vec3 P[8];
    for (int i = 0; i < 8; ++i) { const auto w = fieldbox::World(fb, { i & 1 ? fb.halfWidth : -fb.halfWidth, i & 2 ? fb.halfLength : -fb.halfLength, i & 4 ? fb.halfHeight : -fb.halfHeight }); P[i] = { w.x, w.y, w.z }; }
    const auto box = DockBox(r, t); const auto travel = roadvisual::TileTravel(r, std::size_t(t)); const unsigned edge = roadvisual::TravelTint(travel);
    constexpr int quads[6][4]{ { 0, 1, 3, 2 }, { 4, 5, 7, 6 }, { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 2, 3, 7, 6 }, { 0, 1, 5, 4 } };  // fieldbox::Face order
    for (int f = 0; f < 6; ++f) { const bool touch = f7dock::Touch(box.faces, f); const routemap::Vec3 q[4]{ P[quads[f][0]], P[quads[f][1]], P[quads[f][2]], P[quads[f][3]] }; MapFace(pen, c, q, 4, touch ? kTeal : kAmber, touch ? (f == 0 ? .30 : .20) : .06); }
    constexpr int edges[12][2]{ { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    pen.Style(primary ? 2.2 : 1.4, edge, .95);
    for (const auto& e : edges) { routemap::Vec3 a = P[e[0]], b = P[e[1]]; double ax, ay, bx, by; if (routemap::ClipSegment(c, a, b) && Local(c, a, ax, ay) && Local(c, b, bx, by)) { pen.Move(ax, ay); pen.Line(bx, by); } }
    pen.NoLine();
    if (box.solids == 1) {  // solids inside are allowed: hatch on the floor
        pen.Style(1.2, roadvisual::kHatch, .75);
        for (int k = 1; k <= 4; ++k) { const double f = k / 5.0, g = std::min(1.0, f + .18);
            routemap::Vec3 a{ P[0].x + (P[1].x - P[0].x) * f, P[0].y + (P[1].y - P[0].y) * f, P[0].z + (P[1].z - P[0].z) * f }, b{ P[2].x + (P[3].x - P[2].x) * g, P[2].y + (P[3].y - P[2].y) * g, P[2].z + (P[3].z - P[2].z) * g };
            double ax, ay, bx, by; if (routemap::ClipSegment(c, a, b) && Local(c, a, ax, ay) && Local(c, b, bx, by)) { pen.Move(ax, ay); pen.Line(bx, by); } }
        pen.NoLine();
    }
    if (labels && where) {
        char b[200]; std::snprintf(b, sizeof b, "Tile %d: %s, %s, W %.0f H %.0f, yaw %.0f pitch %.0f roll %.0f", t + 1, roadvisual::TravelWord(travel), box.solids == 1 ? "solids allowed" : "solids block", fb.halfWidth * 2, fb.halfHeight * 2, box.yaw, box.pitch, box.roll);
        labels->push_back(b); where->push_back({ (P[4].x + P[7].x) / 2, (P[4].y + P[7].y) / 2, (P[4].z + P[7].z) / 2 });
    }
}

// ---- the map: selected tiles and points stand out ----
void DockDrawMap(Pen& pen, uptr movie, const routemap::Camera& c, const trafficdata::Route& r) {
    if (!g_dock.on || r.ring) return;
    const int n = int(r.points.size());
    const auto& line = EditorLine(r);  // FPS-1: the line once a frame, not once per tile
    // DRAW-1 (2026-11-07): a full reason drawn as strokes was ~520 Flash calls a label, every frame. On the map now: the red line for every
    // warning tile, a short one-pass "Tile N !" note at its middle, the full reason only for selected tiles (the dock lists every reason);
    // at most 24 notes on screen, selected tiles first.
    const double vl = std::min(g_ui.bx, g_ui.bx + g_ui.ax), vr = std::max(g_ui.bx, g_ui.bx + g_ui.ax), vt = std::min(g_ui.by, g_ui.by + g_ui.ay), vb = std::max(g_ui.by, g_ui.by + g_ui.ay);
    const auto selected = [&](int t) { return std::find(g_dock.tiles.items.begin(), g_dock.tiles.items.end(), t) != g_dock.tiles.items.end(); };
    struct Note { int t; double x, y; bool full; std::string why; }; std::vector<Note> notes; int warned = 0, offScreen = 0;
    for (int t = 0; t < DockTiles(r); ++t) {  // tiles that warn: a red line along them and a note at their middle
        const auto why = DockWarning(r, line, t); if (why.empty()) continue; ++warned;
        const auto& a = r.points[std::size_t(t)]; const auto& b = r.points[std::size_t((t + 1) % n)];
        double ax, ay, bx, by; if (!Local(c, { a.x, a.y, a.z }, ax, ay) || !Local(c, { b.x, b.y, b.z }, bx, by)) continue;
        pen.Style(4, kClearanceRed, .9); pen.Move(ax, ay); pen.Line(bx, by); pen.NoLine();
        const double mx = (ax + bx) / 2, my = (ay + by) / 2;
        if (mx < vl || mx > vr || my < vt || my > vb) { ++offScreen; continue; }
        notes.push_back({ t, mx + 8, my + 6, selected(t), why });
    }
    std::stable_sort(notes.begin(), notes.end(), [](const Note& x, const Note& y) { return x.full > y.full; });
    int shown = 0;
    for (const auto& nt : notes) {
        if (shown >= 24) break; ++shown;
        if (nt.full) Text(movie, "Tile " + std::to_string(nt.t + 1) + ": " + nt.why, nt.x, nt.y, kClearanceRed, 360, true);
        else MapNote("Tile " + std::to_string(nt.t + 1) + " !", nt.x, nt.y, kClearanceRed, 120);
    }
    static ULONGLONG notesLogAt = 0;
    if (GetTickCount64() - notesLogAt >= 5000) { notesLogAt = GetTickCount64();
        logger::Write("OBS system=f7.draw event=warning_notes route=%s warned=%d on_screen=%d drawn=%d off_screen=%d reason=short_notes_full_reason_selected_only", r.name.c_str(), warned, int(notes.size()), shown, offScreen); }
    static std::vector<std::string> boxLabels; static std::vector<std::array<double, 3>> boxWhere; boxLabels.clear(); boxWhere.clear(); int boxes = 0;
    if (g_dock.level == f7dock::Tile) {
        // Which selected tiles get their box drawn (at most 12 a frame): the primary tile first, then the ones nearest the middle of the
        // screen. Index order drew the first 12 visible tiles, which with a whole route selected were far from the one being edited.
        struct Seen { int t; double ax, ay, bx, by, rank; }; static std::vector<Seen> seen; seen.clear();
        const double midX = (vl + vr) * .5, midY = (vt + vb) * .5;
        for (int t : g_dock.tiles.items) {
            if (t < 0 || t >= DockTiles(r)) continue; const auto& a = r.points[std::size_t(t)]; const auto& b = r.points[std::size_t((t + 1) % n)];
            double ax, ay, bx, by; if (!Local(c, { a.x, a.y, a.z }, ax, ay) || !Local(c, { b.x, b.y, b.z }, bx, by)) continue;
            const bool primary = t == g_dock.tiles.primary;
            seen.push_back({ t, ax, ay, bx, by, primary ? -1.0 : std::hypot((ax + bx) * .5 - midX, (ay + by) * .5 - midY) });
        }
        std::stable_sort(seen.begin(), seen.end(), [](const Seen& x, const Seen& y) { return x.rank < y.rank; });
        for (const auto& v : seen) {
            const bool primary = v.t == g_dock.tiles.primary;
            pen.Style(primary ? 11 : 9, primary ? 0xFFFFFF : kTeal, primary ? .7 : .55); pen.Move(v.ax, v.ay); pen.Line(v.bx, v.by); pen.NoLine();
            if (++boxes <= 12) DockDrawTileBox(pen, c, r, line, v.t, primary, &boxLabels, &boxWhere);  // the tile's real box
        }
    }
    for (std::size_t k = 0; k < boxLabels.size() && k < 6; ++k) { double x, y; if (Local(c, { boxWhere[k][0], boxWhere[k][1], boxWhere[k][2] }, x, y)) MapNote(boxLabels[k], x + 6, y - 14, 0xFFFFFF, 520); }
    { static ULONGLONG boxLogAt = 0; if (boxes && GetTickCount64() - boxLogAt >= 5000) { boxLogAt = GetTickCount64(); logger::Write("OBS system=f7.draw event=tile_boxes route=%s selected=%d drawn=%d labels=%d reason=selected_tile_field_box_on_map", r.name.c_str(), boxes, std::min(boxes, 12), int(std::min<std::size_t>(boxLabels.size(), 6))); } }
    if (g_dock.level == f7dock::Point) for (int i : g_dock.points.items) {
        if (i < 0 || i >= n) continue; const auto& q = r.points[std::size_t(i)]; double x, y; if (!Local(c, { q.x, q.y, q.z }, x, y)) continue;
        pen.Style(3, i == g_dock.points.primary ? 0xFFFFFF : kTeal, 1); pen.Circle(x, y, 15); pen.NoLine();
    }
    // R6: Show rings the problem's points (yellow) while the Problems menu is open.
    if (g_dock.level == f7dock::Problems && g_dock.probShown >= 0 && g_dock.probShown < int(g_dock.problems.size()) && g_dock.probKey.rfind(r.name + "#", 0) == 0) {
        const auto& pb = g_dock.problems[std::size_t(g_dock.probShown)];
        for (int i : { pb.a, pb.b }) { if (i < 0 || i >= n) continue; const auto& q = r.points[std::size_t(i)]; double x, y; if (!Local(c, { q.x, q.y, q.z }, x, y)) continue; pen.Style(4, 0xFFE15A, 1); pen.Circle(x, y, 22); pen.NoLine(); }
    }
}
