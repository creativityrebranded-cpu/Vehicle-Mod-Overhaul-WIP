// Route survey, game side (routesurvey.h has the store and the rules). Main thread, from the frame task after the loading screen:
// reads this world's routes the way the route editor does (trafficprofile::Routes(), trafficdata), walks every lane every metre and
// casts three down rays (centre, both wheel tracks) with vehicle_probe::CastNow, the editor's direct ray (same physics read lock and
// fault guard as the queued rays). Only points near the player: only loaded cells have collision. Time-boxed per frame. Again when the
// player has moved on, the world changed, or a load finished. Decides nothing: it fills World's measured floor. After the floors, the
// clear-path sweep: level rays along the lane on lines across the road find the static solids; each row's blocked lines are stored
// when the pass ends. The line through them is Avoid's. Rays only: the game's hull sweep needs a live vehicle.
#include "routesurvey.h"
#include "routesurvey_run.h"
#include "planworld.h"
#include "addresses.h"
#include "game.h"
#include "log.h"
#include "prefetch.h"
#include "trafficdata.h"
#include "trafficprofile.h"
#include "trafficroute.h"
#include "vehicle_probe.h"
#include "vehiclediag.h"
#include <Windows.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" int FVO_WorldName(char* out, int size);  // context.cpp

namespace routesurvey {
namespace {
using uptr = std::uintptr_t;
constexpr double kRadius = 220.0;     // m round the player: the loaded area (handbook: ~250 m)
constexpr double kMoveAgain = 120.0;  // m the player moves before the next sweep
constexpr double kFrameMs = 6.0;      // ms of rays a frame (the first sweep runs right after the load)
constexpr double kAcross = 4.0;       // m: WM-1 down rays across the lane line every metre out to this (was kTrack 1.0, the wheel tracks)
constexpr double kAbove = 3.0, kBelow = 4.0;  // m: each ray from over the authored line to under it (authored floors sit up to 1.4 m off)
constexpr std::uint64_t kMask = 0x22806;      // the editor's ray layers (F7 tile check)

// Collision layers that stay where they are: navmap.cpp kPersistentLayers. A parked car is not a solid of the road.
constexpr std::uint64_t kStaticLayers = (1ull << 1) | (1ull << 2) | (1ull << 9) | (1ull << 10) | (1ull << 13) | (1ull << 17) | (1ull << 35);
struct Job { float x, y, z; int lane; bool lineStart; bool scan = false; float fx = 0, fy = 0, dz = 0, h = 0; int run = 0, row = 0, bin = 0, side = 0; };
std::mutex g_lock;
Store g_store;
SweepStore g_sweep;
std::vector<ClearCell> g_noWay;  // the rows with no room: few, so the F7 warning never walks the whole map
std::vector<std::vector<Row>> g_runs;
std::vector<Job> g_jobs; std::size_t g_next = 0;
std::string g_world; double g_cx = 1e18, g_cy = 1e18; bool g_wasLoading = true;
std::uint32_t g_stamp = 1; unsigned long long g_retryAt = 0;
std::uint64_t g_physicsSeen = 0;
struct Pass { int routes = 0, lanes = 0, samples = 0, rays = 0, hits = 0, misses = 0, noWorld = 0, faults = 0, frames = 0, scanRays = 0, faces = 0, lips = 0, otherLayers = 0, noNormal = 0; double ms = 0, worstFrameMs = 0; EdgeCount edges; float lastZ = 0; bool haveLast = false; } g_pass;

uptr Player() { __try { return *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
bool PlayerAt(double& x, double& y) {
    const uptr p = Player(); if (!p) return false;
    __try { const float* v = reinterpret_cast<const float*>(p + off::kRefPosition); x = v[0]; y = v[1]; return std::isfinite(x) && std::isfinite(y); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadText(const std::filesystem::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary); if (!f) return false; std::ostringstream s; s << f.rdbuf(); out = s.str(); return true;
}
double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) { static LARGE_INTEGER q{}; if (!q.QuadPart) QueryPerformanceFrequency(&q); return 1000.0 * double(b.QuadPart - a.QuadPart) / double(q.QuadPart); }

// The jobs for one sweep: every lane of this world's routes, every metre, within kRadius of (px, py).
void Plan(const std::string& world, double px, double py) {
    g_jobs.clear(); g_next = 0; g_pass = Pass{}; g_runs.clear();
    std::vector<Job> scan;
    // Reading every route file took 10-18 ms in one frame (02:56 run), outside the ray budget. Only read again when a file changed.
    static trafficdata::Data d; static std::filesystem::file_time_type newest{}; static std::wstring dir; static int files = -1;
    try {
        const std::wstring now = trafficprofile::Routes(); std::filesystem::file_time_type latest{}; int count = 0;
        for (const auto& e : std::filesystem::directory_iterator(now)) { if (!e.is_regular_file() || e.path().extension() != ".ini") continue; ++count; latest = std::max(latest, e.last_write_time()); }
        if (now != dir || latest != newest || count != files) {
            d = trafficdata::Data{}; dir = now; newest = latest; files = count;
            for (const auto& e : std::filesystem::directory_iterator(now)) {
                if (!e.is_regular_file() || e.path().extension() != ".ini") continue;
                std::string text; if (ReadText(e.path(), text)) trafficdata::Read(text, e.path().filename().string(), d);
            }
            trafficdata::PreferEditor(d); trafficdata::ApplyLaneDefaults(d);
            logger::Write("OBS system=world.survey event=routes_read files=%d reason=route_files_changed", count);
        }
    } catch (const std::exception& error) { files = -1; logger::Write("OBS system=world.survey event=routes_unread reason=%s", error.what()); return; }
    for (const auto& r : d.routes) {
        if (!trafficdata::SameWorld(d, r.world, world)) continue;
        const auto line = trafficroute::Build(r); if (line.error) continue;
        ++g_pass.routes;
        for (const int lane : trafficroute::Lanes(line, r.lanes)) {
            if (lane < 0 || lane >= int(line.lane.size())) continue;
            const auto& p = line.lane[std::size_t(lane)]; const int n = int(p.size()); if (n < 2) continue;
            ++g_pass.lanes; bool start = true; int metre = 0;
            const int segs = line.closed ? n : n - 1;
            for (int i = 0; i < segs; ++i) {
                const auto& a = p[std::size_t(i)]; const auto& b = p[std::size_t((i + 1) % n)];
                const double len = std::hypot(b.x - a.x, b.y - a.y); if (!(len > .01)) continue;
                for (double s = 0; s < len; s += 1.0) {
                    const double f = s / len, x = a.x + (b.x - a.x) * f, y = a.y + (b.y - a.y) * f, z = a.z + (b.z - a.z) * f;
                    if (std::hypot(x - px, y - py) > kRadius) { start = true; continue; }
                    if (start) { g_runs.emplace_back(); metre = 0; }
                    g_jobs.push_back({ float(x), float(y), float(z), lane, start }); start = false;
                    // the wheel tracks: across the segment
                    const double ux = (b.y - a.y) / len, uy = -(b.x - a.x) / len;
                    if (metre++ % int(kScanStep) == 0) {  // a row across the road: scan lines along the lane, low and body height
                        auto& run = g_runs.back(); const int row = int(run.size()); Row rw; rw.x = float(x); rw.y = float(y); rw.z = float(z); rw.ux = float(ux); rw.uy = float(uy); run.push_back(rw);
                        const double fx = (b.x - a.x) / len, fy = (b.y - a.y) / len, dz = (b.z - a.z) / len * kScanStep;
                        for (int bin = 0; bin < kScanBins; ++bin) for (const double h : { kScanLow, kScanHigh }) { const double o = bin - kScanHalf;
                            Job s{ float(x + ux * o), float(y + uy * o), float(z), -1, false }; s.scan = true; s.fx = float(fx); s.fy = float(fy); s.dz = float(dz); s.h = float(h); s.run = int(g_runs.size()) - 1; s.row = row; s.bin = bin; scan.push_back(s); }
                        // Side rays from the lane line outward: a wall running beside the road gives scan lines along the lane nothing to hit.
                        for (const int side : { -1, 1 }) for (const double h : { kScanLow, kScanHigh }) {
                            Job s{ float(x), float(y), float(z), -1, false }; s.scan = true; s.fx = float(ux * side); s.fy = float(uy * side); s.h = float(h); s.run = int(g_runs.size()) - 1; s.row = row; s.side = side; scan.push_back(s); }
                    }
                    // WM-1: down rays across the lane corridor every metre (was the two wheel tracks only): the map covers the whole road.
                    for (double o = -kAcross; o <= kAcross + 1e-9; o += 1.0) if (std::fabs(o) > .5) g_jobs.push_back({ float(x + ux * o), float(y + uy * o), float(z), -1, false });
                }
            }
        }
    }
    g_pass.scanRays = int(scan.size());
    g_jobs.insert(g_jobs.end(), scan.begin(), scan.end());  // floors first: the scan rays stand on them
    g_pass.samples = int(g_jobs.size());
    logger::Write("OBS system=world.survey event=start world=%s routes=%d lanes=%d rays_planned=%d centre=%.0f,%.0f radius_m=%.0f reason=after_load_or_moved",
        world.c_str(), g_pass.routes, g_pass.lanes, g_pass.samples, px, py, kRadius);
}

void Work() {
    if (g_next >= g_jobs.size()) return;
    LARGE_INTEGER a{}, now{}; QueryPerformanceCounter(&a);
    ++g_pass.frames;
    while (g_next < g_jobs.size()) {
        const Job j = g_jobs[g_next++];
        double floorZ = j.z;
        if (j.scan) { std::lock_guard<std::mutex> lk(g_lock); double fz = 0; if (g_store.At(j.x, j.y, j.z, 1.5, fz)) floorZ = fz; }
        const float sz = float(floorZ + j.h);
        const float from[3] = { j.x, j.y, j.scan ? sz : j.z + float(kAbove) };
        const float reach = float(j.side ? kScanHalf : kScanStep);
        const float to[3] = { j.scan ? j.x + j.fx * reach : j.x, j.scan ? j.y + j.fy * reach : j.y, j.scan ? sz + j.dz : j.z - float(kBelow) };
        prefetch::RayHit h; const int k = vehicle_probe::CastNow(from, to, kMask, h); ++g_pass.rays;
        if (k < 0) { ++g_pass.faults; g_next = g_jobs.size(); break; }  // CastNow backs off after a fault: stop this sweep
        if (k == 0) { ++g_pass.noWorld; if (!g_pass.hits && g_pass.noWorld >= 64) { g_next = g_jobs.size(); break; } }  // rays are paused: this pass would measure nothing
        else if (j.scan) {
            if (h.hit) {
                if (!h.normalOk) ++g_pass.noNormal;
                else if (std::fabs(h.nz) < .5) {  // a face; a floor or ramp the ray ran into is the road, not a solid
                    if (h.layer >= 64 || !((kStaticLayers >> h.layer) & 1ull)) ++g_pass.otherLayers;
                    else {
                        // The real step here: the floor just before the face and on top of it.
                        auto down = [&](float x, float y, float top, float bottom, double& z) { const float a[3] = { x, y, top }, b[3] = { x, y, bottom }; prefetch::RayHit d; const int r = vehicle_probe::CastNow(a, b, kMask, d); ++g_pass.rays;
                            if (r == 1 && d.hit && (!d.normalOk || d.nz > .5)) { z = d.z; return true; } return false; };  // MB-1: signed
                        double before = 0, after = 0; const float p = float(planbranch::rules::lipProbe);
                        const bool haveBefore = down(h.x - j.fx * p, h.y - j.fy * p, h.z + 1.5f, h.z - 3.0f, before), haveAfter = down(h.x + j.fx * p, h.y + j.fy * p, h.z + 2.6f, h.z - 1.5f, after);
                        if (IsLip(haveBefore, before, haveAfter, after)) ++g_pass.lips;
                        else { ++g_pass.faces;
                        std::lock_guard<std::mutex> lk(g_lock); g_sweep.Write().solids.Note(h.x, h.y, floorZ, h.z, g_stamp);
                        if (j.run < int(g_runs.size()) && j.row < int(g_runs[std::size_t(j.run)].size())) { auto& run = g_runs[std::size_t(j.run)];
                            if (j.side) { const int from = int(std::lround(h.distance)); for (int o = from; o <= kScanHalf; ++o) run[std::size_t(j.row)].blocked[kScanHalf + j.side * o] = true; }  // the wall, and whatever is behind it
                            else { run[std::size_t(j.row)].blocked[j.bin] = true; if (h.distance > 1 && j.row + 1 < int(run.size())) run[std::size_t(j.row + 1)].blocked[j.bin] = true; } } } }
                }
            }
        }
        else if (!h.hit) ++g_pass.misses;
        else {
            ++g_pass.hits;
            const bool up = !h.normalOk || h.nz > .5;  // a floor: up-facing only (MB-1: sign proven by plan.normals, falling rays 3881 up / 3 down)
            if (up) { std::lock_guard<std::mutex> lk(g_lock); g_store.Note(h.x, h.y, h.z, h.normalOk ? std::fabs(h.nz) : -1.0, g_stamp); }
            if (j.lane >= 0) { if (j.lineStart) g_pass.haveLast = false; if (g_pass.haveLast) CountEdge(g_pass.edges, g_pass.lastZ, h.z, .35); g_pass.lastZ = h.z; g_pass.haveLast = true; }
        }
        QueryPerformanceCounter(&now);
        if (Ms(a, now) >= kFrameMs) break;
    }
    QueryPerformanceCounter(&now);
    const double ms = Ms(a, now); g_pass.ms += ms; if (ms > g_pass.worstFrameMs) g_pass.worstFrameMs = ms;
    if (g_next >= g_jobs.size() && (g_pass.faults || g_pass.noWorld * 2 > g_pass.rays)) {
        // A pass the rays did not answer measured nothing. It must never read as clear road: keep what we have and try again.
        logger::Write("OBS system=world.survey event=abandoned rays=%d no_world=%d faults=%d reason=rays_not_answering_kept_the_last_measurement", g_pass.rays, g_pass.noWorld, g_pass.faults);
        g_retryAt = GetTickCount64() + 3000; g_jobs.clear(); g_next = 0;
    }
    else if (g_next >= g_jobs.size()) {
        std::size_t cells = 0; int rows = 0, withSolids = 0, noWay = 0, told = 0, solids = 0;
        { std::lock_guard<std::mutex> lk(g_lock); cells = g_store.cells.size();
            auto& sweep = g_sweep.Write();
            sweep.solids.Prune(g_cx, g_cy, kRadius, g_stamp);
            solids = sweep.solids.count;
            g_noWay.erase(std::remove_if(g_noWay.begin(), g_noWay.end(), [](const ClearCell& c) { return std::hypot(c.x - g_cx, c.y - g_cy) < kRadius; }), g_noWay.end());
            for (const auto& run : g_runs) {
                for (std::size_t i = 0; i < run.size(); ++i) { const auto& r = run[i]; ++rows; const auto mask = MaskOf(r); if (mask) ++withSolids; const bool room = RowHasRoom(run, int(i)); if (!room) { ++noWay;
                        if (told++ < 20) logger::Write("OBS system=world.sweep event=no_clear_line pos=%.1f,%.1f,%.1f reason=no_room_for_a_car_in_this_row", r.x, r.y, r.z); }
                    ClearCell c; c.x = r.x; c.y = r.y; c.z = r.z; c.ux = r.ux; c.uy = r.uy; c.blocked = mask; c.noWay = !room; sweep.rows.Put(c); if (c.noWay) g_noWay.push_back(c); } } }
        logger::Write("OBS system=world.sweep event=done world=%s scan_rays=%d faces=%d lips=%d solids=%d rows=%d rows_with_solids=%d rows_no_way=%d other_layers=%d no_normal=%d reason=known_solids_and_rows_ready",
            g_world.c_str(), g_pass.scanRays, g_pass.faces, g_pass.lips, solids, rows, withSolids, noWay, g_pass.otherLayers, g_pass.noNormal);
        logger::Write("OBS system=world.survey event=done world=%s rays=%d hits=%d misses=%d no_world=%d faults=%d total_ms=%.2f us_per_ray=%.1f frames=%d worst_frame_ms=%.2f "
            "floor_cells=%zu edges=%d edges_over_face_step=%d edges_over_half_m=%d highest_edge_m=%.2f reason=timing_probe",
            g_world.c_str(), g_pass.rays, g_pass.hits, g_pass.misses, g_pass.noWorld, g_pass.faults, g_pass.ms, g_pass.rays ? 1000.0 * g_pass.ms / g_pass.rays : 0.0,
            g_pass.frames, g_pass.worstFrameMs, cells, g_pass.edges.edges, g_pass.edges.overFace, g_pass.edges.overHalf, g_pass.edges.highest);
        ++g_stamp;
    }
}
}

namespace { std::atomic<unsigned long> g_mainThread{ 0 }; std::atomic<unsigned long long> g_frame{ 0 }; }
unsigned long long Frame() { return g_frame.load(std::memory_order_relaxed); }
bool OnMainThread() { const auto id = g_mainThread.load(std::memory_order_relaxed); return id && id == GetCurrentThreadId(); }
void NoteFloor(double x, double y, double z, double nz) {
    std::lock_guard<std::mutex> lk(g_lock); g_store.Note(x, y, z, nz, g_stamp);
}

void Tick(bool loading) {
    g_mainThread.store(GetCurrentThreadId(), std::memory_order_relaxed);  // the frame task's thread: CastNow is allowed here
    g_frame.fetch_add(1, std::memory_order_relaxed);
    if (loading) { g_wasLoading = true; g_jobs.clear(); g_next = 0; return; }
    char w[256]{}; if (!FVO_WorldName(w, sizeof w)) return;  // interiors / no player
    double px = 0, py = 0; if (!PlayerAt(px, py)) return;
    const bool newWorld = g_world != w;
    if (newWorld) { std::lock_guard<std::mutex> lk(g_lock); g_store.Clear(); g_sweep.Clear(); g_noWay.clear(); g_world = w; }
    const bool retry = g_retryAt && GetTickCount64() >= g_retryAt;
    if (g_next >= g_jobs.size() && (retry || newWorld || g_wasLoading || (!g_retryAt && std::hypot(px - g_cx, py - g_cy) > kMoveAgain))) {
        g_wasLoading = false; g_retryAt = 0; g_cx = px; g_cy = py; Plan(g_world, px, py);
    }
    if (g_next < g_jobs.size()) {
        const auto physics = vehiclediag::PhysicsChangeCount();
        if (physics != g_physicsSeen) {
            g_physicsSeen = physics;
            logger::Observe("world.survey", 0, 0, "event=deferred reason=physics_changed queued=%zu", g_jobs.size() - g_next);
            return;  // no query this frame, so do not consume a job or record a miss
        }
        Work();
    }
}

std::shared_ptr<const SweepView> ReadSweep() {
    std::lock_guard lk(g_lock);
    return g_sweep.Read();
}
int ForgetSolid(double x, double y) {
    std::lock_guard lk(g_lock);
    bool found = false;
    g_sweep.View().solids.Near(x, y, 1.0, [&](const Solid&) { found = true; });
    return found ? g_sweep.Write().solids.Forget(x, y) : 0;
}
bool KnownSolid(const SweepView& view, double x, double y, double z) { return view.solids.Known(x, y, z); }
bool KnownSolid(double x, double y, double z) {
    std::lock_guard lk(g_lock);
    return KnownSolid(g_sweep.View(), x, y, z);
}
int SolidsNear(const SweepView& view, double x, double y, double z, double radius, SweptSolid* out, int max) {
    int n = 0;
    view.solids.Near(x, y, radius, [&](const Solid& s) { if (n < max && s.hi > z - 1 && s.lo < z + 3) out[n++] = { s.x, s.y, s.lo, s.hi }; });
    return n;
}
int SolidsNear(double x, double y, double z, double radius, SweptSolid* out, int max) {
    std::lock_guard lk(g_lock);
    return SolidsNear(g_sweep.View(), x, y, z, radius, out, max);
}
bool RowAt(const SweepView& view, double x, double y, double z, double& ux, double& uy, unsigned& blocked) {
    const auto* c = view.rows.At(x, y, z); if (!c) return false;
    ux = c->ux; uy = c->uy; blocked = c->blocked; return true;
}
bool RowAt(double x, double y, double z, double& ux, double& uy, unsigned& blocked) {
    std::lock_guard lk(g_lock);
    return RowAt(g_sweep.View(), x, y, z, ux, uy, blocked);
}
bool NoClearNear(double x, double y, double radius) {
    std::lock_guard<std::mutex> lk(g_lock);
    for (const auto& c : g_noWay) if (std::hypot(c.x - x, c.y - y) <= radius) return true;
    return false;
}

bool FloorAt(double x, double y, double zRef, double window, double& z) {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_store.At(x, y, zRef, window, z);
}
namespace { struct ReachOf { Reach reach; double at = -1e9, x = 0, y = 0; int levels = 0; std::uint32_t stamp = 0; }; std::unordered_map<int, ReachOf> g_reach; }
int UpdateReach(int slot, double x, double y, double z, double now, double& ms) {
    ms = 0; std::lock_guard<std::mutex> lk(g_lock); auto& r = g_reach[slot];
    if (now - r.at < planbranch::rules::mapReachEvery && now >= r.at && std::hypot(x - r.x, y - r.y) < 5.0 && r.stamp == g_stamp) return r.levels;
    LARGE_INTEGER a{}, b{}; QueryPerformanceCounter(&a);
    r.levels = g_store.Flood(x, y, z, planbranch::rules::mapReach, planbranch::rules::mapMaxCells, r.reach); r.at = now; r.x = x; r.y = y; r.stamp = g_stamp;
    QueryPerformanceCounter(&b); ms = Ms(a, b); return r.levels;
}
bool FloorReachAt(int slot, double x, double y, double zRef, double window, double& z) {
    std::lock_guard<std::mutex> lk(g_lock); const auto it = g_reach.find(slot); if (it == g_reach.end() || it->second.reach.empty()) return false;
    return g_store.At(x, y, zRef, window, z, &it->second.reach);
}
void CheckRadarPoints(int slot, const planworld::Shape* shapes, int count, double step, double carHeight,
                      bool* below, double* floorZ) {
    std::lock_guard<std::mutex> lk(g_lock);
    const auto it = g_reach.find(slot);
    if (it == g_reach.end()) return;
    for (int i = 0; i < count; ++i) {
        if (shapes[i].source != planworld::Source::Ray) continue;
        const auto& p = shapes[i].centre;
        below[i] = g_store.BelowReachableFloor(p.x, p.y, p.z, step, carHeight,
                                               it->second.reach, floorZ[i]);
    }
}
bool RoadFit(int slot, double x, double y, double zRef, double window, RoadSurface& out) {
    out = {};
    std::lock_guard<std::mutex> lk(g_lock); const auto it = g_reach.find(slot);
    if (it == g_reach.end() || it->second.reach.empty()) return false;
    const auto f = g_store.FitSurface(x, y, zRef, window, &it->second.reach);
    out.ok = f.ok; out.z = f.z; out.spread = f.spread; out.samples = f.samples; out.outliers = f.outliers;
    return f.samples > 0;
}
int MapVerdict(int slot, double x, double y, double z, double tol, double carHeight, double& floorZ) {
    std::lock_guard<std::mutex> lk(g_lock); const auto it = g_reach.find(slot);
    const Reach* reach = it != g_reach.end() && !it->second.reach.empty() ? &it->second.reach : nullptr;
    double fz = 0;
    if (reach && g_store.At(x, y, z, carHeight, fz, reach)) {  // the car's own floor here, nearest the point
        floorZ = fz; const double over = z - fz;
        if (std::fabs(over) <= tol) return 1;
        if (over > tol && over < carHeight) return 2;
        return 3;
    }
    if (g_store.At(x, y, z, tol, fz)) { floorZ = fz; return 3; }  // a measured floor the car cannot reach (rail top, other deck, lower copy)
    return 0;
}
}
