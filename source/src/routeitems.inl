// Route item warnings (RI-1, RI-3, RI-5) and the drive preview (HV-3, HV-4): notes/route-items-20261107/CHANGE_CARDS.md.
// Included by routeedit.cpp right after docklayout.inl (same scope: it uses the dock's helpers). F7 READER ONLY: nothing here changes a car,
// a birth, a route file or a setting. Engine use: the existing read-only reference scan (modscene::ScanCellRange), reads of reference
// fields under SEH, and the editor's existing direct rays (vehicle_probe::CastNow), within the dock's ray budget. Everything else is data
// other systems already keep (route survey floor and sweep rows, tile checks, the addon's inspect rows, Health's records).

constexpr int kItemsBase = 21000;            // + 0..15: a row; filters and buttons below
constexpr int kItemsFilter = 21100;          // + filter
constexpr int kItemsPrev = 21120, kItemsNext = 21121, kItemsSame = 21122, kItemsShowAll = 21123, kItemsGoTile = 21124, kItemsRecheck = 21125;
enum ItemsFilter { kIfAll = 0, kIfDrive, kIfSpawn, kIfModels, kIfCars, kIfPreview, kIfFine, kIfCount };
constexpr const char* kItemsFilterName[kIfCount]{ "All", "Driving", "Spawning", "Models", "Cars", "Test preview", "Not a problem" };

// One row of the list: an item on the route, a model, a car, or a preview event.
struct ItemRow {
    enum Src : std::uint8_t { Route, Model, Car, Preview } src = Route;
    routeitems::Item item; routeitems::Warning warn;
    std::uint32_t car = 0; std::string ledger; double carX = 0, carY = 0, carZ = 0, carSpeed = 0;
    int lane = -1, event = -1;  // Preview: lane run and event index
};
// Plain facts about one reference, read under SEH on the main frame task (THREAD-1).
struct RefFacts { bool ok = false; std::uint32_t id = 0, base = 0; uptr baseForm = 0; float pos[3] = {}, rot[3] = {}, lo[3] = {}, hi[3] = {}; };
struct RefSeen { RefFacts f; std::string name; bool actor = false; };
struct ItemsState {
    std::string route, sig; int phase = 0; std::uint32_t cell = 0; ULONGLONG startedAt = 0, doneAt = 0;
    std::vector<RefSeen> facts; std::size_t refNext = 0; std::uint64_t scanAsk = 0; std::size_t refsSeen = 0;  // THREAD-1: plain facts from the main frame task
    struct Cand { uptr ref = 0; std::uint32_t id = 0, base = 0; int tile = -1; double x = 0, y = 0, z = 0, top = 0, size[3] = {}, rot = 0; std::string name; routeitems::Kind kind = routeitems::Kind::Object; bool vehicle = false; };
    std::vector<Cand> cand; std::size_t candNext = 0;
    std::vector<fieldbox::Box> boxes; std::vector<double> tileStart;
    std::vector<routeitems::Item> found;      // RI-1 result for the route (kept until the route changes or Recheck)
    std::vector<ItemRow> rows;                 // what the list shows (rebuilt twice a second)
    ULONGLONG rowsAt = 0;
    int filter = kIfAll, first = 0, sel = -1; bool sameBase = false;
    ULONGLONG previewAt = 0; std::string previewRoute; bool previewOn = false;
    std::map<std::uint32_t, std::vector<std::array<float, 3>>> tracks;  // HV-4: where the route's real cars went since Test (drawn beside the preview)
    // PERF-1 (2026-11-07, "large FPS drop at test"): same results, less work a frame.
    std::vector<fieldbox::Axes> axes; std::vector<double> reach;   // each tile box's axes and bounding radius, worked out once per scan
    ULONGLONG sigCheckAt = 0; int warnCount = 0;                   // geometry check every 2 s (was every frame); warnings counted once per scan
    std::string pendingSig; ULONGLONG pendingAt = 0;              // FPS-1: a changed geometry rescans only once it has stayed the same 3 s (not on every edit)
    int finishNext = -1;                                           // FPS-1: the floor-step pass, a few tiles a frame (was one 395 ms frame)
};
ItemsState g_items;
void ItemsTrace(const char* event, const char* fmt, ...) {
    static int told = 0; if (told >= 600) return; ++told;
    char b[600]; va_list a; va_start(a, fmt); std::vsnprintf(b, sizeof b, fmt, a); va_end(a);
    logger::Write("OBS system=f7.items event=%s %s", event, b);
}
// The drive preview's own trace (card name f7.preview), its own cap so it never eats the item lines.
void PreviewTrace(const char* fmt, ...) {
    static int told = 0; if (told >= 800) return; ++told;
    char b[600]; va_list a; va_start(a, fmt); std::vsnprintf(b, sizeof b, fmt, a); va_end(a);
    logger::Write("OBS system=f7.preview %s", b);
}
// PERF-1 cost meter: milliseconds a frame per part, one line every 5 s while F7 shows a route.
struct ItemsCost { LARGE_INTEGER f{}; double sum[6] = {}, peak[6] = {}; long long count[6] = {}; int frames = 0, segs = 0, labels = 0; ULONGLONG at = 0; };
ItemsCost g_cost; constexpr const char* kCostName[6]{ "tick", "sig", "preview", "draw", "rows", "layout" };
enum CostPart { kCTick = 0, kCSig, kCPreview, kCDraw, kCRows, kCLayout };
struct CostTimer { int part; LARGE_INTEGER t0;
    explicit CostTimer(int p) : part(p) { QueryPerformanceCounter(&t0); }
    ~CostTimer() { LARGE_INTEGER t1; QueryPerformanceCounter(&t1); if (!g_cost.f.QuadPart) QueryPerformanceFrequency(&g_cost.f);
        const double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(g_cost.f.QuadPart); g_cost.sum[part] += ms; g_cost.peak[part] = std::max(g_cost.peak[part], ms); ++g_cost.count[part]; } };
void ItemsCostFrame(const std::string& route) {
    ++g_cost.frames; const auto now = GetTickCount64(); if (!g_cost.at) g_cost.at = now; if (now - g_cost.at < 5000) return;
    char b[700]; int w = std::snprintf(b, sizeof b, "OBS system=f7.cost route=%s frames=%d", route.c_str(), g_cost.frames);
    for (int k = 0; k < 6 && w > 0 && w < int(sizeof b) - 80; ++k) w += std::snprintf(b + w, sizeof b - std::size_t(w), " %s_ms_per_frame=%.3f %s_ms_max=%.2f", kCostName[k], g_cost.frames ? g_cost.sum[k] / g_cost.frames : 0.0, kCostName[k], g_cost.peak[k]);
    std::snprintf(b + w, sizeof b - std::size_t(w), " draw_segments_per_frame=%d labels_per_frame=%d phase=%d tracked_cars=%d reason=f7_route_items_cost_per_part", g_cost.frames ? g_cost.segs / g_cost.frames : 0, g_cost.frames ? g_cost.labels / g_cost.frames : 0, g_items.phase, int(g_items.tracks.size()));
    logger::Write("%s", b); const auto f = g_cost.f; g_cost = ItemsCost{}; g_cost.f = f; g_cost.at = now;
}

// ---- reading a reference (SEH; plain data only) ----
RefFacts ReadRefFacts(uptr ref) {
    RefFacts f;
    __try {
        f.id = *reinterpret_cast<const std::uint32_t*>(ref + off::kFormId);
        for (int i = 0; i < 3; ++i) { f.pos[i] = reinterpret_cast<const float*>(ref + off::kRefPosition)[i]; f.rot[i] = reinterpret_cast<const float*>(ref + off::kRefRotation)[i]; }
        f.baseForm = *reinterpret_cast<const uptr*>(ref + 0x98);
        if (f.baseForm) { f.base = *reinterpret_cast<const std::uint32_t*>(f.baseForm + off::kFormId);
            for (int i = 0; i < 3; ++i) { f.lo[i] = *reinterpret_cast<const float*>(f.baseForm + 0x50 + i * 4); f.hi[i] = *reinterpret_cast<const float*>(f.baseForm + 0x5C + i * 4); } }  // handbook: bounds at +0x50/+0x5C
        f.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { f = RefFacts{}; }
    return f;
}
// A base form's bounds: the narrower of its two flat sides (its width) and its height (m). 0 when unreadable.
void ItemsModelSize(uptr form, double& wide, double& high) {
    wide = high = 0; if (!form) return;
    __try { const float* lo = reinterpret_cast<const float*>(form + 0x50); const float* hi = reinterpret_cast<const float*>(form + 0x5C);
        const double sx = std::fabs(hi[0] - lo[0]), sy = std::fabs(hi[1] - lo[1]); wide = sx < sy ? sx : sy; high = std::fabs(hi[2] - lo[2]); }
    __except (EXCEPTION_EXECUTE_HANDLER) { wide = high = 0; }
}
std::string ItemsName(uptr ref, uptr base) {
    const char* n = FormNameRaw(ref); if (n && *n) return n;
    if (base) { n = FullNameRaw(base); if (n && *n) return n; }
    return {};
}
// THREAD-1 (user 2026-11-07: "Yes, move it"): the reference scan and every read of a reference run on the main frame task
// (routeedit::ItemsMainTick, called beside GroundTick), where the existing callers of the cell iterator run. The F7 draw thread only asks
// and takes the finished list of plain facts; it never touches a reference.
struct ScanJob { std::mutex lock; std::uint64_t ask = 0, done = 0; bool working = false; std::uint32_t cell = 0, total = 0; std::size_t next = 0;
    double minX = 0, minY = 0, maxX = 0, maxY = 0; std::vector<uptr> refs; std::vector<RefSeen> out; ULONGLONG startedAt = 0;
    std::vector<fieldbox::Box> boxes; std::vector<fieldbox::Axes> axes; std::vector<double> reach; std::size_t outside = 0; };  // FPS-1: plain copies of the tile boxes
ScanJob g_scanJob;
int TileOfIn(const std::vector<fieldbox::Box>& boxes, const std::vector<fieldbox::Axes>& axes, const std::vector<double>& reach, double x, double y, double z, double radius, double height);  // FPS-1: defined below
void ItemsMainTickImpl() {
    std::lock_guard lk(g_scanJob.lock); auto& j = g_scanJob; if (!j.working) return;
    if (!j.total) j.total = modscene::GridCells();
    if (j.cell < j.total) { modscene::ScanCellRange(j.cell, 2, j.refs, 20000); j.cell += 2; return; }  // 2 cells a frame (the 2026-11-04 stutter rule)
    for (int k = 0; k < 1500 && j.next < j.refs.size(); ++k) {  // the facts, 1500 references a frame; names only near the route
        const uptr ref = j.refs[j.next++]; const auto f = ReadRefFacts(ref); if (!f.ok || !f.id || f.id == 0x14) continue;
        if (!std::isfinite(f.pos[0]) || f.pos[0] < j.minX || f.pos[0] > j.maxX || f.pos[1] < j.minY || f.pos[1] > j.maxY) continue;
        // FPS-1: the name and actor read only for a reference inside a tile box (the same test phase 2 makes; ~60 of ~7,000 on the ring road)
        if (!j.boxes.empty()) { const double sx = f.hi[0] - f.lo[0], sy = f.hi[1] - f.lo[1], sz = f.hi[2] - f.lo[2], radius = .5 * std::hypot(sx, sy);
            if (!(radius > .05 && radius < 60) || TileOfIn(j.boxes, j.axes, j.reach, f.pos[0], f.pos[1], f.pos[2] + (f.lo[2] + f.hi[2]) * .5, radius, sz) < 0) { ++j.outside; continue; } }
        RefSeen r; r.f = f; r.name = ItemsName(ref, f.baseForm); r.actor = modscene::IsActor(ref); j.out.push_back(std::move(r)); }
    if (j.next >= j.refs.size()) { j.working = false; j.done = j.ask;
        logger::Write("OBS system=f7.items event=main_scan ask=%llu refs=%zu in_tiles=%zu outside_tiles=%zu cells=%u ms=%llu reason=reference_scan_on_main_frame_task", j.ask, j.refs.size(), j.out.size(), j.outside, j.total, GetTickCount64() - j.startedAt); }
}
bool ItemsIsVehicleBase(std::uint32_t base) { for (const auto& t : VehicleTypesHere()) for (unsigned id : t.ids) if (id == base) return true; return false; }

// ---- RI-1: the scan, one slice a frame ----
std::string ItemsSig(const trafficdata::Route& r) { std::string s = r.name; const auto& line = EditorLine(r); for (int t = 0; t < DockTiles(r); ++t) s += DockCheckSig(r, line, t); return s; }  // FPS-1: the line once
void ItemsReset(const trafficdata::Route& r, const char* why) {
    const bool keepSel = g_items.route == r.name;
    const int filter = g_items.filter; auto tracks = std::move(g_items.tracks); const auto pr = g_items.previewRoute; const bool pon = g_items.previewOn; const auto pat = g_items.previewAt;
    g_items = ItemsState{}; g_items.filter = filter; if (keepSel && pr == r.name) { g_items.tracks = std::move(tracks); g_items.previewRoute = pr; g_items.previewOn = pon; g_items.previewAt = pat; }
    g_items.route = r.name; g_items.sig = ItemsSig(r); g_items.startedAt = GetTickCount64(); g_items.phase = 1;
    const int tiles = DockTiles(r); double at = 0; const auto& line = EditorLine(r);  // FPS-1: the line once
    for (int t = 0; t < tiles; ++t) { fieldbox::Box b; if (!DockWorldBox(r, line, t, b)) b.halfLength = 0; g_items.boxes.push_back(b); g_items.tileStart.push_back(at); at += 2 * b.halfLength;
        g_items.axes.push_back(fieldbox::AxesOf(b)); g_items.reach.push_back(std::sqrt(b.halfWidth * b.halfWidth + b.halfLength * b.halfLength + b.halfHeight * b.halfHeight)); }  // PERF-1
    ItemsTrace("scan", "route=%s tiles=%d reason=%s", r.name.c_str(), tiles, why);
}
int TileOfIn(const std::vector<fieldbox::Box>& boxes, const std::vector<fieldbox::Axes>& axes, const std::vector<double>& reach, double x, double y, double z, double radius, double height) {
    int best = -1; double bestScore = 1e9;
    for (int t = 0; t < int(boxes.size()); ++t) { const auto& b = boxes[std::size_t(t)]; if (b.halfLength <= 0) continue;
        const double dx = x - b.centre.x, dy = y - b.centre.y, dz = z - b.centre.z, lim = reach[std::size_t(t)] + radius + height + .5;
        if (dx * dx + dy * dy + dz * dz > lim * lim) continue;  // PERF-1: cannot touch this box (sphere test before the box test)
        const auto& a = axes[std::size_t(t)]; const fieldbox::V3 d{ dx, dy, dz };
        const fieldbox::V3 l{ fieldbox::Dot(d, a.right), fieldbox::Dot(d, a.forward), fieldbox::Dot(d, a.up) };  // = fieldbox::Local with the axes worked out once
        if (std::fabs(l.x) > b.halfWidth + radius || std::fabs(l.y) > b.halfLength + radius || l.z < -b.halfHeight - height - .5 || l.z > b.halfHeight + .5) continue;
        const double score = std::fabs(l.y) / std::max(b.halfLength, .1); if (score < bestScore) { bestScore = score; best = t; } }
    return best;
}
int ItemsTileOf(double x, double y, double z, double radius, double height) { return TileOfIn(g_items.boxes, g_items.axes, g_items.reach, x, y, z, radius, height); }
// The confirm ray: down through the object's spot; its owner must be this reference (it has collision). 1 yes, 0 no hit on it, -1 no answer.
int ItemsConfirm(const ItemsState::Cand& c, double& top) {
    const float from[3]{ float(c.x), float(c.y), float(c.top + 1.0) }, to[3]{ float(c.x), float(c.y), float(c.z - 2.0) };
    prefetch::RayHit h; const int k = vehicle_probe::CastNow(from, to, 0x22806, h);
    if (k <= 0) return -1;
    if (h.hit && h.formId == c.id) { top = h.z; return 1; }
    return 0;
}
void ItemsFinishRoute(const trafficdata::Route& r) {
    // Floor steps along each lane, from the route survey's measured floor (no rays): every 1 m, a jump over a quarter tire is a row.
    const int lanes = std::max(1, DockLanes(r)); const double width = DockRoadWidth(r), laneW = width / lanes;
    // FPS-1: 8 tiles a frame (the whole route in one frame was a 395 ms frame on the 177-tile ring road); the rest runs once all are done.
    if (g_items.finishNext < 0) g_items.finishNext = 0;
    const int tileEnd = std::min(int(g_items.boxes.size()), g_items.finishNext + 8);
    for (int t = g_items.finishNext; t < tileEnd; ++t) { const auto& b = g_items.boxes[std::size_t(t)]; if (b.halfLength <= 0) continue;
        for (int k = 0; k < lanes; ++k) { const double lx = -width / 2 + laneW * (k + .5); bool have = false; double last = 0, lastY = 0; double lastStepAt = -1e9;
            for (double y = -b.halfLength; y <= b.halfLength; y += 1.0) { const auto p = fieldbox::World(b, { lx, y, -b.halfHeight }); double z;
                if (!routesurvey::FloorAt(p.x, p.y, p.z, 1.5, z)) { have = false; continue; }
                if (have && std::fabs(z - last) > worldclass::rules::driveOverLow && y - lastStepAt > 3) {
                    routeitems::Item it; it.kind = routeitems::Kind::TerrainStep; it.x = p.x; it.y = p.y; it.top = std::max(z, last); it.floor = std::min(z, last); it.haveFloor = true; it.rise = std::fabs(z - last);
                    it.tile = t; it.lane = k; it.along = g_items.tileStart[std::size_t(t)] + y + b.halfLength; it.across = lx; it.confirmed = true; it.birthTile = DockTileTravel(r, t) != routepolicy::Travel::Hover;
                    g_items.found.push_back(it); lastStepAt = y; (void)lastY; }
                have = true; last = z; lastY = y; } } }
    g_items.finishNext = tileEnd; if (tileEnd < int(g_items.boxes.size())) return;  // FPS-1: more tiles next frame
    // No ground and not checked, from the tile checks.
    int uncheckedFrom = -1, unchecked = 0; const auto& line = EditorLine(r);  // FPS-1: the line once
    for (int t = 0; t < int(g_items.boxes.size()); ++t) { const auto& b = g_items.boxes[std::size_t(t)]; if (b.halfLength <= 0) continue;
        const auto* chk = DockCheckOf(r, line, t);
        if (!chk) { if (uncheckedFrom < 0) uncheckedFrom = t; ++unchecked; continue; }
        if (chk->floorBad && chk->floorDz <= -3) { routeitems::Item it; it.kind = routeitems::Kind::NoGround; it.x = b.centre.x; it.y = b.centre.y; it.top = b.centre.z - b.halfHeight; it.tile = t; it.along = g_items.tileStart[std::size_t(t)] + b.halfLength; it.birthTile = DockTileTravel(r, t) != routepolicy::Travel::Hover; g_items.found.push_back(it); } }
    // LB-1 (2026-11-12): every stretch where the lane builder had to cut a folding lane straight is a row, so the editor shows it.
    for (const auto& sq : line.squeezed) { routeitems::Item it; it.kind = routeitems::Kind::LaneCut; it.x = sq.at.x; it.y = sq.at.y; it.top = sq.at.z; it.lane = sq.lane;
        it.rise = sq.worstDeg; it.tile = std::clamp(int(line.ToSource(double(sq.from))), 0, std::max(0, int(g_items.boxes.size()) - 1));
        it.along = it.tile < int(g_items.tileStart.size()) ? g_items.tileStart[std::size_t(it.tile)] : 0.0; it.confirmed = true; g_items.found.push_back(it); }
    if (!line.squeezed.empty()) ItemsTrace("lane_cut", "route=%s cuts=%d reason=lane_wider_than_corner_folds_cut_straight", r.name.c_str(), int(line.squeezed.size()));
    if (unchecked) { routeitems::Item it; it.kind = routeitems::Kind::Unchecked; it.tile = uncheckedFrom; const auto& b = g_items.boxes[std::size_t(uncheckedFrom)]; it.x = b.centre.x; it.y = b.centre.y; it.top = b.centre.z; it.along = g_items.tileStart[std::size_t(uncheckedFrom)];
        it.name = std::to_string(unchecked) + " tile" + (unchecked == 1 ? "" : "s") + " not checked yet"; g_items.found.push_back(it); }
    std::sort(g_items.found.begin(), g_items.found.end(), [](const auto& a, const auto& b) { return a.along < b.along; });
    g_items.phase = 4; g_items.doneAt = GetTickCount64();
    int warn = 0; for (const auto& it : g_items.found) if (routeitems::Judge(it).sev >= routeitems::Severity::Attention) ++warn; g_items.warnCount = warn;  // PERF-1: counted once
    ItemsTrace("done", "route=%s items=%d warnings=%d refs=%d candidates=%d unchecked_tiles=%d ms=%llu reason=scan_complete", r.name.c_str(), int(g_items.found.size()), warn, int(g_items.refsSeen), int(g_items.cand.size()), unchecked, g_items.doneAt - g_items.startedAt);
}
// Once a frame while the dock shows a route (after DockCheckTick: its rays come first).
void ItemsTick(const trafficdata::Route* r) {
    if (!g_dock.on || !r || r->ring || DockTiles(*r) == 0) return;
    ItemsCostFrame(r->name); CostTimer ct(kCTick);  // PERF-1: the cost line every 5 s, this frame's tick timed
    const auto now = GetTickCount64();
    if (g_items.route != r->name) ItemsReset(*r, "route_shown");
    else if (g_items.phase == 4 && now - g_items.doneAt > 1000 && now - g_items.sigCheckAt >= 500 && [&] {  // FPS-1: cheap now (the line once), so twice a second
            CostTimer ct(kCSig); g_items.sigCheckAt = now; const auto sig = ItemsSig(*r);
            if (sig == g_items.sig) { g_items.pendingSig.clear(); return false; }
            if (sig != g_items.pendingSig) { g_items.pendingSig = sig; g_items.pendingAt = now; return false; }  // still being edited: wait
            return now - g_items.pendingAt >= 3000; }()) ItemsReset(*r, "route_geometry_changed");  // unchanged for 3 s: rescan once
    else if (g_items.phase == 4 && g_items.pendingSig.empty() && now - g_items.doneAt > 20000) ItemsReset(*r, "refresh_20s_things_move");
    if (g_items.phase == 1) {  // THREAD-1: ask the main frame task for the attached cells' references, then take its plain facts
        std::unique_lock lk(g_scanJob.lock, std::try_to_lock); if (!lk.owns_lock()) return;  // the main task is working on it this moment
        auto& j = g_scanJob;
        if (!g_items.scanAsk) {
            double minX = 1e18, minY = 1e18, maxX = -1e18, maxY = -1e18;
            for (std::size_t t = 0; t < g_items.boxes.size(); ++t) { const auto& b = g_items.boxes[t]; if (b.halfLength <= 0) continue; const double e = g_items.reach[t] + 60;
                minX = std::min(minX, b.centre.x - e); maxX = std::max(maxX, b.centre.x + e); minY = std::min(minY, b.centre.y - e); maxY = std::max(maxY, b.centre.y + e); }
            j.refs.clear(); j.out.clear(); j.cell = 0; j.total = 0; j.next = 0; j.minX = minX; j.minY = minY; j.maxX = maxX; j.maxY = maxY; j.startedAt = GetTickCount64();
            j.boxes = g_items.boxes; j.axes = g_items.axes; j.reach = g_items.reach; j.outside = 0;  // FPS-1: plain copies under the job lock
            j.working = true; g_items.scanAsk = ++j.ask; return; }
        if (j.working || j.done != g_items.scanAsk) return;
        g_items.facts.swap(j.out); g_items.refsSeen = j.refs.size(); j.refs.clear(); g_items.phase = 2;
        ItemsTrace("refs", "route=%s refs=%d near_route=%d cells=%u reason=attached_cells_scanned_on_main_task", r->name.c_str(), int(g_items.refsSeen), int(g_items.facts.size()), j.total);
        return;
    }
    if (g_items.phase == 2) {  // which references lie in a tile box: 600 a frame
        for (int k = 0; k < 600 && g_items.refNext < g_items.facts.size(); ++k) {
            const auto& seen = g_items.facts[g_items.refNext++]; const auto& f = seen.f;  // THREAD-1: facts only, no reference read here
            const double sx = f.hi[0] - f.lo[0], sy = f.hi[1] - f.lo[1], sz = f.hi[2] - f.lo[2]; const double radius = .5 * std::hypot(sx, sy);
            if (!(radius > .05 && radius < 60) || !std::isfinite(f.pos[0])) continue;
            const double cz = f.pos[2] + (f.lo[2] + f.hi[2]) * .5;
            const int t = ItemsTileOf(f.pos[0], f.pos[1], cz, radius, sz); if (t < 0) continue;
            ItemsState::Cand c; c.ref = 0; c.id = f.id; c.base = f.base; c.tile = t; c.x = f.pos[0]; c.y = f.pos[1]; c.z = f.pos[2] + f.lo[2]; c.top = f.pos[2] + f.hi[2];
            c.size[0] = sx; c.size[1] = sy; c.size[2] = sz; c.rot = f.rot[2]; c.name = seen.name;
            c.vehicle = ItemsIsVehicleBase(f.base); if (c.vehicle) c.kind = routeitems::Kind::Vehicle; else if (seen.actor) c.kind = routeitems::Kind::Person;
            g_items.cand.push_back(c);
        }
        if (g_items.refNext >= g_items.facts.size()) { g_items.phase = 3; ItemsTrace("candidates", "route=%s candidates=%d reason=inside_a_tile_box", r->name.c_str(), int(g_items.cand.size())); }
        return;
    }
    if (g_items.phase == 3) {  // one confirm ray each, 6 a frame (the dock's tile check has its own 12)
        if (!navmap::RaysEnabled()) return;
        for (int k = 0; k < 6 && g_items.candNext < g_items.cand.size(); ++k) {
            const auto& c = g_items.cand[g_items.candNext]; double top = c.top; int conf = 1;
            if (c.kind == routeitems::Kind::Object) { conf = ItemsConfirm(c, top); if (conf < 0) return; }  // no answer: ask again next frame, never "clear"
            ++g_items.candNext;
            routeitems::Item it; it.kind = c.kind; it.ref = c.id; it.base = c.base; it.name = c.name; it.x = c.x; it.y = c.y; it.top = top; it.confirmed = conf == 1;
            for (int i = 0; i < 3; ++i) it.size[i] = c.size[i];
            const auto& b = g_items.boxes[std::size_t(c.tile)]; const auto l = fieldbox::Local(b, { c.x, c.y, c.z });
            it.tile = c.tile; it.across = l.x; it.along = g_items.tileStart[std::size_t(c.tile)] + l.y + b.halfLength;
            const int lanes = std::max(1, DockLanes(*r)); const double width = DockRoadWidth(*r); it.lane = std::clamp(int((l.x + width / 2) / (width / lanes)), 0, lanes - 1);
            double fz; const double bottom = fieldbox::World(b, { l.x, l.y, -b.halfHeight }).z;
            if (routesurvey::FloorAt(c.x, c.y, bottom, 1.5, fz)) { it.haveFloor = true; it.floor = fz; }
            else if (const auto* chk = DockCheckOf(*r, c.tile); chk && chk->floorKnown && chk->floorDz > -3) { it.haveFloor = true; it.floor = bottom + chk->floorDz; }
            it.rise = it.haveFloor ? top - it.floor : 0;
            it.cls = c.kind == routeitems::Kind::Vehicle ? worldclass::Class::Vehicle : c.kind == routeitems::Kind::Person ? worldclass::Class::Person : [&] {
                worldclass::Hit h; h.owner = c.id; h.base = c.base; h.size = std::max({ c.size[0], c.size[1], c.size[2] }); h.layer = -1; return worldclass::Classify(h); }();
            it.solidsAllowed = DockBox(*r, c.tile).solids != 0; it.birthTile = DockTileTravel(*r, c.tile) != routepolicy::Travel::Hover;
            double ux, uy; unsigned blocked;
            if (routesurvey::RowAt(c.x, c.y, it.haveFloor ? it.floor : bottom, ux, uy, blocked)) {  // the sweep's row here: is there room for a car anywhere across it?
                it.roomKnown = true; const int bins = 2 * planbranch::rules::scanHalf + 1, body = planbranch::rules::bodyBins;
                for (int s = 0; s + 2 * body < bins && !it.roomToPass; ++s) { bool ok = true; for (int q = s; q <= s + 2 * body; ++q) if (blocked >> q & 1u) { ok = false; break; } it.roomToPass = ok; } }
            const auto w = routeitems::Judge(it);
            if (w.sev >= routeitems::Severity::Attention)
                ItemsTrace("warn", "route=%s tile=%d lane=%d ref=%08X base=%08X class=%s rise=%.2f across=%.1f along=%.0f confirmed=%d room=%d/%d severity=%s spawn=%d reason=%s",
                    r->name.c_str(), c.tile + 1, it.lane + 1, it.ref, it.base, worldclass::Name(it.cls), it.rise, it.across, it.along, int(it.confirmed), int(it.roomKnown), int(it.roomToPass), routeitems::Name(w.sev), int(!w.spawn.empty()), w.reason);
            g_items.found.push_back(std::move(it));
        }
        if (g_items.candNext >= g_items.cand.size()) ItemsFinishRoute(*r);
    }
}

// ---- HV-4: the drive preview ----
void ItemsStartPreview(const trafficdata::Route& r) {
    // Removal 2026-11-14: Test records the real cars' tracks on this route from now on (drawn thin white). The simulated drive of the old
    // branch planner is gone with that planner; the energy plan's chosen path shows on a selected car instead.
    g_items.tracks.clear(); g_items.previewRoute = r.name; g_items.previewAt = GetTickCount64(); g_items.previewOn = true;
    PreviewTrace("route=%s event=start reason=test_pressed_tracks_recorded", r.name.c_str());
    Status("Test: recording the real cars' tracks on this route.");
}
void ItemsRows(const trafficdata::Route& r) {
    const auto now = GetTickCount64(); if (now - g_items.rowsAt < 500 && !g_items.rows.empty()) return; g_items.rowsAt = now; CostTimer ct(kCRows);
    std::vector<ItemRow> rows;
    for (const auto& it : g_items.found) { ItemRow row; row.item = it; row.warn = routeitems::Judge(it); rows.push_back(std::move(row)); }
    // RI-5: models on this route's Types (Health's record; model bounds against the lane)
    { static const auto query = [] { const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); return addon ? reinterpret_cast<FvoTrafficHealthModelQueryV1>(GetProcAddress(addon, "FVO_Traffic_HealthModelV1")) : nullptr; }();
      const double laneW = DockRoadWidth(r) / std::max(1, DockLanes(r));
      for (const auto& t : VehicleTypesHere()) { if (TypeIn(r.exclude, t)) continue;
        for (unsigned id : t.ids) { ItemRow row; row.src = ItemRow::Model; row.item.kind = routeitems::Kind::Object; row.item.base = id; row.item.name = t.name;
            FvoTrafficHealthModelV1 h{}; const bool have = query && query(id, &h) && h.rejected;
            double wide = 0, high = 0; ItemsModelSize(runtimeforms::LookupChecked(id), wide, high);
            if (have) { row.warn.sev = h.allowed ? routeitems::Severity::Attention : routeitems::Severity::Clearance; row.warn.title = t.name + ": Health " + (h.allowed ? "flags it (you allow it)" : "left it out");
                row.warn.effect = std::string(h.stageName) + " stage: " + routeitems::Num(h.pass, 0) + " of " + routeitems::Num(h.pass + h.fail, 0) + " passed, fails at " + std::to_string(h.spots) + " spots.";
                { static const auto reason = [] { const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); return addon ? reinterpret_cast<FvoTrafficHealthReasonV1>(GetProcAddress(addon, "FVO_Traffic_HealthReasonV1")) : nullptr; }();
                  char why[64]; if (reason && reason(id, why, int(sizeof why))) row.warn.effect += std::string(" Last: ") + why + "."; }  // RI-4 (older addon: no reason)
                row.warn.fix = "Leave it out in Types, or keep Health's choice."; row.warn.reason = "health_model_rejected"; rows.push_back(row); }
            else if (wide > laneW + .05 && wide < 20) { row.warn.sev = routeitems::Severity::Attention; row.warn.title = t.name + ": " + routeitems::Num(wide) + " m wide";
                row.warn.effect = "Wider than this route's " + routeitems::Num(laneW) + " m lanes: it rubs the red dividers."; row.warn.spawn = "Siting may find no room for it on narrow tiles.";
                row.warn.fix = "Widen in Road > Width, or leave it out in Types."; row.warn.reason = "model_wider_than_lane"; rows.push_back(row); }
            else if (high > DockRoadHeight(r) + .05 && high < 20) { row.warn.sev = routeitems::Severity::Attention; row.warn.title = t.name + ": " + routeitems::Num(high) + " m tall";
                row.warn.effect = "Taller than this route's box height: its roof leaves the field."; row.warn.fix = "Raise the tile height, or leave it out in Types."; row.warn.reason = "model_taller_than_box"; rows.push_back(row); }
            break; } } }  // one row per type: its first id stands for the name
    // RI-4: spawn spots Health rejected on this route (older addon: none)
    { static const auto spotQuery = [] { const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); return addon ? reinterpret_cast<FvoTrafficHealthSpotQueryV1>(GetProcAddress(addon, "FVO_Traffic_HealthSpotV1")) : nullptr; }();
      for (int i = 0; spotQuery && i < 64; ++i) { FvoTrafficHealthSpotV1 h; if (!spotQuery(r.name.c_str(), i, &h)) break;
        ItemRow row; row.item.kind = routeitems::Kind::SpawnSpot; row.item.x = h.x; row.item.y = h.y; row.item.lane = h.lane; row.item.spotFails = int(h.fail); row.item.spotModels = h.models;
        row.item.tile = -1; for (int t = 0; t < int(g_items.boxes.size()); ++t) { const auto& b = g_items.boxes[std::size_t(t)]; if (b.halfLength <= 0) continue; const auto l = fieldbox::Local(b, { h.x, h.y, b.centre.z });
            if (std::fabs(l.x) <= b.halfWidth + 5 && std::fabs(l.y) <= b.halfLength + 5) { row.item.tile = t; row.item.top = b.centre.z - b.halfHeight; row.item.along = g_items.tileStart[std::size_t(t)] + l.y + b.halfLength; break; } }
        row.warn = routeitems::Judge(row.item); row.warn.effect += std::string(" Stage: ") + h.stageName + "."; rows.push_back(row); } }
    // HV-3: every plan car on the route (the addon's inspect rows), each with its ledger line
    { const auto addon = GetModuleHandleW(L"FVO_Traffic.dll"); const auto query = addon ? reinterpret_cast<FvoTrafficInspectQueryV1>(GetProcAddress(addon, "FVO_Traffic_InspectV1")) : nullptr;
      for (int i = 0; query && i < 32; ++i) { FvoTrafficInspectV1 q; if (!query(r.name.c_str(), i, &q)) break; if (!q.vehicle) continue;
        ItemRow row; row.src = ItemRow::Car; row.car = q.vehicle; row.carX = q.x; row.carY = q.y; row.carZ = q.z; row.carSpeed = q.speed; row.lane = q.lane;
        if (g_items.previewOn) { auto& tr = g_items.tracks[q.vehicle]; if (tr.size() < 4000 && (tr.empty() || std::hypot(tr.back()[0] - q.x, tr.back()[1] - q.y) > 1.0)) tr.push_back({ float(q.x), float(q.y), float(q.z) }); }
        char b[400]; const bool plan = vehicleflight::PlanLedgerLine(q.vehicle, b, int(sizeof b)); row.ledger = plan ? b : "no plan now (not a plan car, or planArbiter off)";
        char id[24]; std::snprintf(id, sizeof id, "%08X", q.vehicle); row.warn.sev = routeitems::Severity::Info; row.warn.title = std::string("Car ") + id + ", " + routeitems::Num(q.speed) + " m/s";
        row.warn.effect = row.ledger; row.warn.reason = "car_ledger"; rows.push_back(row); } }
    g_items.rows = std::move(rows);
}
bool ItemsPass(const ItemRow& row, int f) {
    const bool fine = row.warn.sev == routeitems::Severity::Fine;
    switch (f) {
    case kIfAll: return !fine && row.src != ItemRow::Preview;
    case kIfDrive: return !fine && row.src == ItemRow::Route;
    case kIfSpawn: return !fine && !row.warn.spawn.empty();
    case kIfModels: return row.src == ItemRow::Model;
    case kIfCars: return row.src == ItemRow::Car;
    case kIfPreview: return row.src == ItemRow::Preview;
    case kIfFine: return fine;
    }
    return false;
}
std::vector<int> ItemsShown() { std::vector<int> v; for (int i = 0; i < int(g_items.rows.size()); ++i) if (ItemsPass(g_items.rows[std::size_t(i)], g_items.filter)) v.push_back(i); return v; }
unsigned ItemsColor(routeitems::Severity s) { return s == routeitems::Severity::Clearance ? kClearanceRed : s == routeitems::Severity::Attention ? kAmber : kDim; }
const ItemRow* ItemsSelected() { return g_items.sel >= 0 && g_items.sel < int(g_items.rows.size()) ? &g_items.rows[std::size_t(g_items.sel)] : nullptr; }

// The Problems level of the dock: filters, rows as cards (8 a page), the selected row's values at the right.
void ItemsLayout(const trafficdata::Route& r, editorcontext::Rect d, double top, double area) {
    CostTimer ct(kCLayout);
    ItemsRows(r);
    double fx = d.x + 12; for (int f = 0; f < kIfCount; ++f) { const std::string n = kItemsFilterName[f]; const double w = TextWidth(n) + 22; DockButton(kItemsFilter + f, fx, top, w, 24, n, g_items.filter == f, true, 5); fx += w + 4; }
    DockButton(kItemsRecheck, fx + 8, top, 90, 24, "Recheck", false, g_items.phase == 4, 0);
    { std::string st = g_items.phase == 4 ? std::to_string(g_items.found.size()) + " items read" : g_items.phase == 1 ? "Reading the loaded cells..." : g_items.phase == 2 ? "Finding what lies in the field..." : "Checking each object (" + std::to_string(g_items.candNext) + " / " + std::to_string(g_items.cand.size()) + ")...";
      DockText(st, fx + 108, top + 5, 260, kDim); }
    const double listW = area * .62, cw = (listW - 10) / 2, cardH = 52; const double ly = top + 32; const auto shown = ItemsShown();
    g_items.first = std::clamp(g_items.first, 0, std::max(0, int(shown.size()) - 1)); int k = 0;
    for (int i = g_items.first; i < int(shown.size()) && k < 8; ++i, ++k) { const auto& row = g_items.rows[std::size_t(shown[std::size_t(i)])];
        const double px = d.x + 12 + (k % 2) * (cw + 10), py = ly + (k / 2) * (cardH + 6);
        std::string title = row.warn.title; if (!row.warn.spawn.empty()) title += "  [spawn]";
        const std::string where = row.src == ItemRow::Route && row.item.tile >= 0 ? "Tile " + std::to_string(row.item.tile + 1) + (row.item.lane >= 0 ? ", lane " + std::to_string(row.item.lane + 1) : "") + ", " + routeitems::Num(row.item.along, 0) + " m: " : "";
        DockButton(kItemsBase + k, px, py, cw, cardH, f7layers::Fit(title, cw - 24, f7layers::kLetterPx), shown[std::size_t(i)] == g_items.sel, true, 8, 0, f7layers::Fit(where + row.warn.effect, cw - 24, f7layers::kLetterPx), ItemsColor(row.warn.sev)); }
    if (shown.empty()) DockText(g_items.filter == kIfPreview ? "Press Test to draw where a car will drive." : g_items.phase == 4 ? "Nothing here." : "Still reading...", d.x + 14, ly + 6, listW, kDim);
    if (int(shown.size()) > 8) { DockButton(kItemsPrev, d.x + 12, d.y + d.h - 30, 90, 24, "Previous", false, g_items.first > 0, 0); DockButton(kItemsNext, d.x + 108, d.y + d.h - 30, 90, 24, "More", false, g_items.first + 8 < int(shown.size()), 0);
        char v[64]; std::snprintf(v, sizeof v, "%d-%d of %d", g_items.first + 1, std::min(int(shown.size()), g_items.first + 8), int(shown.size())); DockText(v, d.x + 210, d.y + d.h - 26, 200, kDim); }
    // the right: the selected row (dock: up to four values), or the preview's totals
    const double rx = d.x + 12 + listW + 14, rw = area - listW - 14; double y = ly;
    auto line = [&](const std::string& t, unsigned c) { DockText(f7layers::Fit(t, rw, f7layers::kLetterPx), rx, y, rw, c); y += 18; };
    auto wrap = [&](const std::string& t, unsigned c, int maxLines) { std::string rest = t; for (int n = 0; n < maxLines && !rest.empty(); ++n) { std::size_t cut = rest.size(); while (cut > 0 && TextWidth(rest.substr(0, cut)) > rw - 6) { const auto sp = rest.rfind(' ', cut - 1); cut = sp == std::string::npos || sp == 0 ? cut - 1 : sp; }
        line(rest.substr(0, cut), c); rest = cut < rest.size() ? rest.substr(cut + (rest[cut] == ' ' ? 1 : 0)) : std::string(); } };
    const auto* s = ItemsSelected();
    if (!s && g_items.filter == kIfPreview && !g_items.tracks.empty()) {
        line("Test: real cars' tracks", 0xFFFFFF);
        line(std::to_string(g_items.tracks.size()) + " cars recorded since Test.", kDim);
        return;
    }
    if (!s) { line("Pick a row: the view shows only that item.", kDim); return; }
    line(s->warn.title, 0xFFFFFF);
    wrap(s->warn.effect, ItemsColor(s->warn.sev), 3);
    if (!s->warn.spawn.empty()) wrap(s->warn.spawn, kCoral, 2);
    if (!s->warn.fix.empty()) wrap(s->warn.fix, kDim, 2);
    if (s->src == ItemRow::Route) { const auto& it = s->item; char b[160];
        std::snprintf(b, sizeof b, "Over floor %s   Across %.1f m %s", it.haveFloor ? (routeitems::Num(it.rise, 2) + " m").c_str() : "unknown", std::fabs(it.across), it.across >= 0 ? "right" : "left"); line(b, 0xDFE7EE);
        std::snprintf(b, sizeof b, "Size %.1f x %.1f x %.1f m   Ref %08X   Base %08X", it.size[0], it.size[1], it.size[2], it.ref, it.base); line(b, 0xDFE7EE);
        const double bw = 110; double bx = rx;
        if (it.base) { int same = 0; for (const auto& o : g_items.found) if (o.base == it.base) ++same; DockButton(kItemsSame, bx, y + 2, bw, 24, "Same base (" + std::to_string(same) + ")", g_items.sameBase, true, 0); bx += bw + 6; }
        if (it.tile >= 0) { DockButton(kItemsGoTile, bx, y + 2, bw, 24, "Go to tile " + std::to_string(it.tile + 1), false, true, 0, 1); bx += bw + 6; }
        DockButton(kItemsShowAll, bx, y + 2, bw, 24, "Show all", false, true, 0); }
    else DockButton(kItemsShowAll, rx, y + 2, 110, 24, "Show all", false, true, 0);
}
bool ItemsAction(int id) {
    if (id >= kItemsFilter && id < kItemsFilter + kIfCount) { g_items.filter = id - kItemsFilter; g_items.first = 0; g_items.sel = -1; g_items.sameBase = false; ItemsTrace("ui", "filter=%s reason=user", kItemsFilterName[g_items.filter]); return true; }
    if (id >= kItemsBase && id < kItemsBase + 8) { const auto shown = ItemsShown(); const int i = g_items.first + id - kItemsBase; if (i < int(shown.size())) { const int pick = shown[std::size_t(i)]; g_items.sel = g_items.sel == pick ? -1 : pick; g_items.sameBase = false;
        if (const auto* s = ItemsSelected()) ItemsTrace("ui", "select=%s ref=%08X base=%08X tile=%d reason=user_isolates_one_item", s->warn.title.c_str(), s->item.ref, s->item.base, s->item.tile + 1); } return true; }
    if (id == kItemsPrev || id == kItemsNext) { g_items.first = std::max(0, g_items.first + (id == kItemsNext ? 8 : -8)); return true; }
    if (id == kItemsSame) { g_items.sameBase = !g_items.sameBase; return true; }
    if (id == kItemsShowAll) { g_items.sel = -1; g_items.sameBase = false; return true; }
    if (id == kItemsRecheck) { trafficdata::Route scratch; if (const auto* r = Shown(scratch)) ItemsReset(*r, "user_recheck"); return true; }
    if (id == kItemsGoTile) { if (const auto* s = ItemsSelected(); s && s->item.tile >= 0) DockSelectOne(f7dock::Tiles, s->item.tile, "problem_go_tile"); return true; }
    return false;
}
int ItemsWarningCount() { return g_items.phase == 4 ? g_items.warnCount : 0; }  // PERF-1: counted when the scan ends (was every frame)

// ---- drawing: map and photo view (the same camera projection) ----
void ItemsBoxLines(Pen& pen, const routemap::Camera& c, double x, double y, double z0, double z1, double sx, double sy, double rot, unsigned color, double width) {
    const double cs = std::cos(rot), sn = std::sin(rot); double px[8], py[8]; bool ok[8];
    for (int i = 0; i < 8; ++i) { const double ax = (i & 1 ? .5 : -.5) * sx, ay = (i & 2 ? .5 : -.5) * sy, az = i & 4 ? z1 : z0;
        ok[i] = Local(c, { x + ax * cs - ay * sn, y + ax * sn + ay * cs, az }, px[i], py[i]); }
    constexpr int e[12][2]{ { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 }, { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
    pen.Style(width, color, .95); for (const auto& k : e) if (ok[k[0]] && ok[k[1]]) { pen.Move(px[k[0]], py[k[0]]); pen.Line(px[k[1]], py[k[1]]); } pen.NoLine();
}
void ItemsEdgeArrow(Pen& pen, uptr movie, double x, double y, unsigned color, const std::string& label) {
    const double W = g_ui.width, H = g_ui.height, cx = W / 2, cy = H / 2; double dx = x - cx, dy = y - cy; const double l = std::max(1e-6, std::hypot(dx, dy)); dx /= l; dy /= l;
    const double t = std::min((W / 2 - 40) / std::max(1e-6, std::fabs(dx)), (H / 2 - 40) / std::max(1e-6, std::fabs(dy))); const double ax = cx + dx * t, ay = cy + dy * t;
    pen.Style(4, color, 1); pen.Move(ax - dx * 26 - dy * 10, ay - dy * 26 + dx * 10); pen.Line(ax, ay); pen.Line(ax - dx * 26 + dy * 10, ay - dy * 26 - dx * 10); pen.NoLine();
    Text(movie, label, ax - dx * 40 - 60, ay - dy * 40 - 8, color, 220, true);
}
void ItemsDraw(Pen& pen, uptr movie, const routemap::Camera& c, const trafficdata::Route& r) {
    if (!g_dock.on || r.ring || g_items.route != r.name) return;
    const auto* s = ItemsSelected(); const bool problems = g_dock.level == f7dock::Problems;
    // isolate: everything already drawn is dimmed under a dark veil; then only the picked item (and its copies of the same base)
    if (s && problems) { pen.NoLine(); pen.Fill(0x05080C, .62); pen.Rect(0, 0, g_ui.width, g_ui.height); pen.End(); }
    // the drive preview: the line coloured by speed and its event markers
    // PERF-1: the same lines as before, drawn as few strokes as possible: one stroke per run of one colour, points closer than kMinPx on
    // screen to the last one drawn are skipped (the last point of every run is always drawn), and only on-screen markers get a label.
    constexpr double kMinPx = 3.0; constexpr int kMaxLabels = 24;
    const double W = g_ui.width, H = g_ui.height;
    auto onScreen = [&](double x, double y) { return x >= -20 && y >= -20 && x <= W + 20 && y <= H + 20; };
    struct Stroke { Pen& pen; unsigned col = 0; double width = 0, alpha = 0; bool open = false; double lx = 0, ly = 0; int segs = 0;
        void To(double x, double y, unsigned c, double w, double a, bool last, double minPx) {
            if (open && c == col) { if (!last && std::hypot(x - lx, y - ly) < minPx) return; pen.Line(x, y); lx = x; ly = y; ++segs; return; }
            if (open && c != col) { pen.Line(x, y); ++segs; }  // close the old run at this point so the colours meet
            pen.Style(w, c, a); pen.Move(x, y); col = c; width = w; alpha = a; open = true; lx = x; ly = y; }
        void Break() { open = false; } };
    const bool showPreview = problems && (g_items.filter == kIfPreview || (s && s->src == ItemRow::Preview));
    if (showPreview) { CostTimer ct(kCDraw);
        for (const auto& [car, tr] : g_items.tracks) { Stroke st{ pen };  // the real cars' tracks since Test, thin white, beside the preview
            for (std::size_t i = 0; i < tr.size(); ++i) { const auto& q = tr[i]; double x, y; if (!Local(c, { q[0], q[1], q[2] + .3 }, x, y) || !onScreen(x, y)) { st.Break(); continue; }
                st.To(x, y, 0xFFFFFF, 2, .7, i + 1 == tr.size(), kMinPx); }
            pen.NoLine(); g_cost.segs += st.segs; (void)car; }
        }
    if (!s || !problems) return;
    if (s->src == ItemRow::Car) {  // HV-3: the car, its ledger line, and every branch it weighed (HV-2): the plan solid, the losers thin with why
        vehicleflight::PlanBranchView bv[planbranch::kMaxBranches]; const int nb = vehicleflight::PlanBranchesView(s->car, bv, planbranch::kMaxBranches);
        for (int k = 0; k < nb; ++k) { const auto& b = bv[k]; double px = 0, py = 0, lx = 0, ly = 0; bool have = false;
            const unsigned col = b.picked ? kTeal : b.feasible ? kAmber : kClearanceRed; pen.Style(b.picked ? 5 : 2, col, b.picked ? 1 : .8);
            for (int j = 0; j < b.points; ++j) { double sx, sy; if (!Local(c, { b.x[j], b.y[j], b.z[j] }, sx, sy)) { have = false; continue; } if (have) pen.Line(sx, sy); else pen.Move(sx, sy); have = true; px = sx; py = sy; lx = sx; ly = sy; }
            pen.NoLine(); (void)px; (void)py;
            if (b.points > 0) { std::string why = std::string(b.family) + (b.picked ? " (picked)" : !b.feasible ? ": blocked by " + std::string(*b.blockedBy ? b.blockedBy : "?") : ": cost " + routeitems::Num(b.cost) + ", lost " + routeitems::Num(b.lost));
                Text(movie, f7layers::Fit(why, 300, f7layers::kLetterPx), lx + 8, ly - 8, col, 320, true); } }
        { static std::uint32_t told = 0; if (told != s->car) { told = s->car; ItemsTrace("draw", "car=%08X branches=%d reason=%s", s->car, nb, nb ? "hv2_branch_view" : "no_plan_branches_now"); } }
        double x, y; if (!Local(c, { s->carX, s->carY, s->carZ }, x, y) || x < 0 || y < 0 || x > g_ui.width || y > g_ui.height) { if (Local(c, { s->carX, s->carY, s->carZ }, x, y)) ItemsEdgeArrow(pen, movie, x, y, kTeal, s->warn.title); return; }
        ItemsBoxLines(pen, c, s->carX, s->carY, s->carZ - .2, s->carZ + 2.2, 2.6, 5.0, 0, kTeal, 2.5); Text(movie, f7layers::Fit(s->ledger, 520, f7layers::kLetterPx), x + 16, y - 24, kTeal, 540, true); return; }
    auto drawItem = [&](const routeitems::Item& it, unsigned col, bool primary) {
        const double z0 = it.haveFloor ? it.floor : it.top - std::max(.2, it.size[2]), z1 = std::max(it.top, z0 + .2);
        const double sx = it.size[0] > .05 ? it.size[0] : 1.0, sy = it.size[1] > .05 ? it.size[1] : 1.0;
        double x, y; const bool seen = Local(c, { it.x, it.y, (z0 + z1) * .5 }, x, y);
        if (!seen || x < 0 || y < 0 || x > g_ui.width || y > g_ui.height) { if (primary && seen) ItemsEdgeArrow(pen, movie, x, y, col, s->warn.title); return; }
        ItemsBoxLines(pen, c, it.x, it.y, z0, z1, sx, sy, 0, col, primary ? 3 : 1.5);
        if (primary) Text(movie, f7layers::Fit(s->warn.title, 320, f7layers::kLetterPx), x + 14, y - 30, col, 340, true); };
    const unsigned col = ItemsColor(s->warn.sev);
    if (s->src == ItemRow::Preview) { double x, y; if (Local(c, { s->item.x, s->item.y, s->item.top + .3 }, x, y)) { pen.Style(4, col, 1); pen.Circle(x, y, 12); pen.NoLine(); } return; }
    if (s->src == ItemRow::Route) { if (g_items.sameBase && s->item.base) for (const auto& o : g_items.found) if (o.base == s->item.base && o.ref != s->item.ref) drawItem(o, col, false); drawItem(s->item, col, true);
        if (s->item.tile >= 0 && s->item.tile < int(g_items.boxes.size())) { const auto& b = g_items.boxes[std::size_t(s->item.tile)];  // its tile, outlined in teal
            double px[4], py[4]; bool ok = true; for (int i = 0; i < 4; ++i) { const auto w = fieldbox::World(b, { (i == 1 || i == 2 ? 1 : -1) * b.halfWidth, (i >= 2 ? 1 : -1) * b.halfLength, -b.halfHeight }); ok = ok && Local(c, { w.x, w.y, w.z }, px[i], py[i]); }
            if (ok) { pen.Style(2, kTeal, .8); pen.Move(px[0], py[0]); for (int i = 1; i < 4; ++i) pen.Line(px[i], py[i]); pen.Line(px[0], py[0]); pen.NoLine(); } } }
}
// Test pressed (HV-4): the Problems level opens on the preview, nothing picked, so the whole line shows.
void ItemsShowPreview() { g_items.filter = kIfPreview; g_items.first = 0; g_items.sel = -1; g_items.sameBase = false; }
