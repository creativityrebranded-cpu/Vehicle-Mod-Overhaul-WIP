#pragma once
// Route survey (design section 2, World: a new source beside cellbake and the navmesh cache; user 2026-11-07: "a fast sweep can be
// done exactly after [physics is up] in ms"). Down rays along every lane of this world's routes, in the loaded cells, give the measured
// floor every metre: the real height of decks, ramps and crests, and the real step edges between them. World only: it records surfaces
// and decides nothing. Readers: the plan's wanted surface (fix A: the field picks the level, the measured floor gives the height), the
// Ledger (timing probe, edge counts). Pure; tests: tests/routesurvey_test.cpp. The game side (CastNow sweep) is routesurvey.cpp.
#include "planrules.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace routesurvey {
constexpr double kCell = 1.0;      // m: one floor record per metre square
constexpr int kLevels = 3;         // stacked decks over one spot
constexpr double kSameLevel = .6;  // m: two hits closer than this are one level (the newer wins)

// WM-1 (2026-11-18, one world map): each level keeps the exact point measured (x, y, z), so the surface between measurements is a
// local plane (slope-correct on ramps and banks), and the car's reachable levels can be flooded (Flood: the wheels' climb).
struct Floor { float z[kLevels]; float nz[kLevels]; float x[kLevels]; float y[kLevels]; std::uint32_t at[kLevels]; std::uint8_t n = 0; };
// Which levels of which squares the car can reach (bit i = level i of that square).
using Reach = std::unordered_map<std::uint64_t, std::uint8_t>;

struct Store {
    std::unordered_map<std::uint64_t, Floor> cells;
    static std::uint64_t KeyOf(std::int64_t ix, std::int64_t iy) { return (std::uint64_t(std::uint32_t(ix)) << 32) | std::uint32_t(iy); }
    static std::uint64_t Key(double x, double y) { return KeyOf(std::int64_t(std::floor(x / kCell)), std::int64_t(std::floor(y / kCell))); }
    // A measured floor point (a down ray's hit facing up). `stamp` orders updates (the newest of a level wins).
    void Note(double x, double y, double z, double nz, std::uint32_t stamp) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
        auto& f = cells[Key(x, y)];
        int k = -1, oldest = 0;
        for (int i = 0; i < f.n; ++i) { if (std::fabs(f.z[i] - z) < kSameLevel) { k = i; break; } if (f.at[i] < f.at[oldest]) oldest = i; }
        if (k < 0) k = f.n < kLevels ? f.n++ : oldest;
        f.z[k] = float(z); f.nz[k] = float(nz); f.x[k] = float(x); f.y[k] = float(y); f.at[k] = stamp;
    }
    // The measured floor at (x, y) nearest `zRef`, within `window`: the level of this square nearest zRef, then a plane through it and
    // the same level in the 8 squares round it (neighbours within lip + grade x distance of it) gives the height at (x, y) itself.
    // reach: only levels the car can reach count (nullptr: any). False: nothing measured in this square near zRef.
    bool At(double x, double y, double zRef, double window, double& z, const Reach* reach = nullptr) const {
        const auto ix = std::int64_t(std::floor(x / kCell)), iy = std::int64_t(std::floor(y / kCell));
        const auto it = cells.find(KeyOf(ix, iy)); if (it == cells.end()) return false;
        std::uint8_t mask = 0xFF; if (reach) { const auto r = reach->find(KeyOf(ix, iy)); if (r == reach->end()) return false; mask = r->second; }
        const Floor& f = it->second; int k = -1; double best = window + 1e-9;
        for (int i = 0; i < f.n; ++i) { if (!((mask >> i) & 1)) continue; const double d = std::fabs(f.z[i] - zRef); if (d <= best) { best = d; k = i; } }
        if (k < 0) return false;
        FitHeight(ix, iy, f, k, x, y, z, reach);
        return true;
    }

private:
    // Sample height is the fallback for At. Excluding a blocker requires a fitted surface.
    bool FitHeight(std::int64_t ix, std::int64_t iy, const Floor& f, int k,
                   double x, double y, double& z, const Reach* reach) const {
        double px[9], py[9], pz[9]; int m = 0; px[m] = f.x[k]; py[m] = f.y[k]; pz[m] = f.z[k]; ++m;
        for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) { if (!dx && !dy) continue;
            const auto key = KeyOf(ix + dx, iy + dy); const auto jt = cells.find(key); if (jt == cells.end()) continue;
            std::uint8_t nm = 0xFF; if (reach) { const auto r = reach->find(key); if (r == reach->end()) continue; nm = r->second; }
            const Floor& g = jt->second; int kb = -1; double bd = 1e9;
            for (int i = 0; i < g.n; ++i) { if (!((nm >> i) & 1)) continue; const double dist = std::hypot(g.x[i] - f.x[k], g.y[i] - f.y[k]);
                const double dz = std::fabs(g.z[i] - f.z[k]); if (dz <= planbranch::rules::mapLip + planbranch::rules::mapNeighbour * dist && dz < bd) { bd = dz; kb = i; } }
            if (kb >= 0) { px[m] = g.x[kb]; py[m] = g.y[kb]; pz[m] = g.z[kb]; ++m; } }
        z = f.z[k];
        if (m >= 3) {  // least squares z = c + a (x - x0) + b (y - y0), about the anchor
            double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sz = 0, sxz = 0, syz = 0;
            for (int i = 0; i < m; ++i) { const double u = px[i] - f.x[k], v = py[i] - f.y[k]; sx += u; sy += v; sz += pz[i]; sxx += u * u; sxy += u * v; syy += v * v; sxz += u * pz[i]; syz += v * pz[i]; }
            const double M[3][3] = { { double(m), sx, sy }, { sx, sxx, sxy }, { sy, sxy, syy } }, R[3] = { sz, sxz, syz };
            auto det3 = [](const double A[3][3]) { return A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]); };
            const double D = det3(M);
            if (std::fabs(D) > 1e-6 * std::max(1.0, sxx * syy)) {
                double C[3]; for (int c = 0; c < 3; ++c) { double T[3][3]; for (int r = 0; r < 3; ++r) for (int q = 0; q < 3; ++q) T[r][q] = q == c ? R[r] : M[r][q]; C[c] = det3(T) / D; }
                if (std::hypot(C[1], C[2]) <= planbranch::rules::mapGrade * 1.5) {
                    z = C[0] + C[1] * (x - f.x[k]) + C[2] * (y - f.y[k]);
                    return true;
                }
            }
        }
        return false;
    }

public:
    // RF-1 (2026-11-19): the road at (x, y) at the car's scale, and how sure that is. The anchor is the reachable level nearest zRef
    // (within window) in the nearest square that has one; every square within `radius` adds its level joined to the anchor (the same
    // lip + grade rule as Flood). A plane is fitted, points further than 3 spreads (at least 0.08 m) from it are outliers and set aside,
    // and the plane is fitted again on the rest: one tooth too high cannot pull the road or widen its pattern. spread is the RMS of the
    // kept points (at least 0.02 m). ok needs 5 kept points that span both axes and a drivable grade.
    struct SurfaceFit { bool ok = false; double z = 0, spread = 0, gradeX = 0, gradeY = 0; int samples = 0, outliers = 0; };
    SurfaceFit FitSurface(double x, double y, double zRef, double window, const Reach* reach, int radius = 2) const {
        SurfaceFit out;
        const auto ix = std::int64_t(std::floor(x / kCell)), iy = std::int64_t(std::floor(y / kCell));
        auto levelsOf = [&](std::int64_t cx, std::int64_t cy, const Floor*& f, std::uint8_t& mask) {
            const auto key = KeyOf(cx, cy); const auto it = cells.find(key); if (it == cells.end()) return false;
            mask = 0xFF; if (reach) { const auto r = reach->find(key); if (r == reach->end()) return false; mask = r->second; }
            f = &it->second; return true;
        };
        double ax = 0, ay = 0, az = 0, bestH = 1e9, bestV = 1e9; bool anchored = false;
        for (int dx = -radius; dx <= radius; ++dx) for (int dy = -radius; dy <= radius; ++dy) {
            const Floor* f = nullptr; std::uint8_t mask = 0; if (!levelsOf(ix + dx, iy + dy, f, mask)) continue;
            for (int i = 0; i < f->n; ++i) {
                if (!((mask >> i) & 1)) continue;
                const double v = std::fabs(f->z[i] - zRef); if (v > window) continue;
                const double h = std::hypot(f->x[i] - x, f->y[i] - y);
                if (h < bestH - .3 || (h < bestH + .3 && v < bestV)) { bestH = h; bestV = v; ax = f->x[i]; ay = f->y[i]; az = f->z[i]; anchored = true; }
            }
        }
        if (!anchored) return out;
        constexpr int kMax = 81;
        double px[kMax], py[kMax], pz[kMax]; int m = 0;
        for (int dx = -radius; dx <= radius && m < kMax; ++dx) for (int dy = -radius; dy <= radius && m < kMax; ++dy) {
            const Floor* f = nullptr; std::uint8_t mask = 0; if (!levelsOf(ix + dx, iy + dy, f, mask)) continue;
            int k = -1; double bd = 1e9;
            for (int i = 0; i < f->n; ++i) {
                if (!((mask >> i) & 1)) continue;
                const double dz = std::fabs(f->z[i] - az), dist = std::hypot(f->x[i] - ax, f->y[i] - ay);
                if (dz <= planbranch::rules::mapLip + planbranch::rules::mapNeighbour * dist && dz < bd) { bd = dz; k = i; }
            }
            if (k >= 0) { px[m] = f->x[k]; py[m] = f->y[k]; pz[m] = f->z[k]; ++m; }
        }
        bool keep[kMax]; for (int i = 0; i < m; ++i) keep[i] = true;
        double c = 0, a = 0, b = 0;
        auto fit = [&]() {
            int n = 0; double sx = 0, sy = 0, sz = 0;
            for (int i = 0; i < m; ++i) if (keep[i]) { sx += px[i] - x; sy += py[i] - y; sz += pz[i]; ++n; }
            if (n < 3) return false;
            const double mx = sx / n, my = sy / n, mz = sz / n;
            double sxx = 0, sxy = 0, syy = 0, sxz = 0, syz = 0;
            for (int i = 0; i < m; ++i) if (keep[i]) { const double u = px[i] - x - mx, v = py[i] - y - my, w = pz[i] - mz;
                sxx += u * u; sxy += u * v; syy += v * v; sxz += u * w; syz += v * w; }
            const double D = sxx * syy - sxy * sxy;
            if (D <= .25 * n * n * .01) return false;  // the points lie on a line: no plane across it
            a = (sxz * syy - syz * sxy) / D; b = (syz * sxx - sxz * sxy) / D; c = mz - a * mx - b * my;
            return true;
        };
        if (!fit()) { out.samples = m; return out; }
        double r[kMax]; double sq = 0; int n = 0;
        for (int i = 0; i < m; ++i) { r[i] = pz[i] - (c + a * (px[i] - x) + b * (py[i] - y)); sq += r[i] * r[i]; ++n; }
        double abs[kMax]; for (int i = 0; i < m; ++i) abs[i] = std::fabs(r[i]);
        std::nth_element(abs, abs + m / 2, abs + m);
        const double robust = std::max(.02, 1.4826 * abs[m / 2]);  // the median distance: one tooth does not move it
        const double cut = std::max(.08, 3 * robust);
        int kept = 0; for (int i = 0; i < m; ++i) { keep[i] = std::fabs(r[i]) <= cut; kept += keep[i]; }
        if (kept < 5 || !fit()) { out.samples = kept; out.outliers = m - kept; return out; }
        sq = 0; n = 0;
        for (int i = 0; i < m; ++i) if (keep[i]) { const double e = pz[i] - (c + a * (px[i] - x) + b * (py[i] - y)); sq += e * e; ++n; }
        out.samples = kept; out.outliers = m - kept;
        out.spread = std::max(.02, std::sqrt(sq / n));
        out.gradeX = a; out.gradeY = b; out.z = c;
        out.ok = std::hypot(a, b) <= planbranch::rules::mapGrade * 1.5;
        return out;
    }

    // Recheck a remembered point against the fitted reachable surface at its own XY.
    // A second measured level near or below the point can put it inside a car's clearance.
    bool BelowReachableFloor(double x, double y, double hitZ, double step, double carHeight,
                             const Reach& reach, double& floorZ) const {
        const auto ix = std::int64_t(std::floor(x / kCell)), iy = std::int64_t(std::floor(y / kCell));
        const auto key = KeyOf(ix, iy);
        const auto it = cells.find(key);
        const auto r = reach.find(key);
        if (it == cells.end() || r == reach.end()) return false;

        constexpr double maxBelow = 4.0;  // existing SB-1 below-deck search limit
        const Floor& f = it->second;
        double nearestAbove = hitZ + maxBelow;
        bool found = false;
        for (int k = 0; k < f.n; ++k) {
            const bool connected = ((r->second >> k) & 1) != 0;
            double z = 0;
            const bool fitted = FitHeight(ix, iy, f, k, x, y, z, connected ? &reach : nullptr);
            if (z >= hitZ - carHeight && z <= hitZ + step) return false;
            if (connected && fitted && z > hitZ + step && z <= nearestAbove) {
                nearestAbove = z;
                found = true;
            }
        }
        if (found) floorZ = nearestAbove;
        return found;
    }
    // The levels the car can reach from (x, y, z): region growing over the measured floor (8 ways). Each reached level carries the slope
    // of the surface round it (a plane through it and its reached neighbours); a neighbour level is the same surface when it lies within
    // mapLip + mapBend x distance of the height that slope predicts there. So a ramp continues (its slope is predicted) and a flat road
    // runs onto a ramp (the bend), while a rail top, a crate top or a deck 1 m over the road stands ~1 m off the prediction and is not
    // reached. Out to `radius` m and at most `maxCells` squares. Returns the reachable levels counted.
    int Flood(double x, double y, double z, double radius, int maxCells, Reach& out) const {
        out.clear();
        const auto sx = std::int64_t(std::floor(x / kCell)), sy = std::int64_t(std::floor(y / kCell));
        const auto it = cells.find(KeyOf(sx, sy)); if (it == cells.end()) return 0;
        int k = -1; double best = 1.5; for (int i = 0; i < it->second.n; ++i) { const double d = std::fabs(it->second.z[i] - z); if (d <= best) { best = d; k = i; } }
        if (k < 0) return 0;
        struct Item { std::int64_t ix, iy; int level; double gx, gy; };
        std::unordered_map<std::uint64_t, std::array<float, 2 * kLevels>> slope;  // reached level -> its gx, gy
        auto fitSlope = [&](std::int64_t ix, std::int64_t iy, int level, double& gx, double& gy) {
            const Floor& f = cells.find(KeyOf(ix, iy))->second; double su = 0, sv = 0, sz = 0, suu = 0, suv = 0, svv = 0, suz = 0, svz = 0; int m = 1;
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) { if (!dx && !dy) continue;
                const auto key = KeyOf(ix + dx, iy + dy); const auto r = out.find(key); if (r == out.end()) continue; const Floor& g = cells.find(key)->second;
                for (int i = 0; i < g.n; ++i) { if (!((r->second >> i) & 1)) continue; const double u = g.x[i] - f.x[level], v = g.y[i] - f.y[level], w = g.z[i] - f.z[level];
                    if (std::fabs(w) > planbranch::rules::mapLip + planbranch::rules::mapNeighbour * std::hypot(u, v)) continue;
                    su += u; sv += v; sz += w; suu += u * u; suv += u * v; svv += v * v; suz += u * w; svz += v * w; ++m; } }
            if (m < 3) return false;  // through the level itself (u = v = w = 0): solve [suu suv; suv svv] g = [suz svz]
            const double D = suu * svv - suv * suv; if (std::fabs(D) < 1e-6) return false;
            gx = (suz * svv - svz * suv) / D; gy = (svz * suu - suz * suv) / D;
            const double g = std::hypot(gx, gy); if (g > planbranch::rules::mapGrade) { gx *= planbranch::rules::mapGrade / g; gy *= planbranch::rules::mapGrade / g; }
            return true; };
        std::vector<Item> q; q.reserve(1024);
        out[KeyOf(sx, sy)] = std::uint8_t(1u << k); int levels = 1;
        { double gx = 0, gy = 0;  // the start: a plane through its measured neighbours (unreached yet, so fitted directly)
            const Floor& f = it->second; double su = 0, sv = 0, suu = 0, suv = 0, svv = 0, suz = 0, svz = 0; int m = 1;
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) { if (!dx && !dy) continue; const auto jt = cells.find(KeyOf(sx + dx, sy + dy)); if (jt == cells.end()) continue;
                const Floor& g = jt->second; for (int i = 0; i < g.n; ++i) { const double u = g.x[i] - f.x[k], v = g.y[i] - f.y[k], w = g.z[i] - f.z[k];
                    if (std::fabs(w) > planbranch::rules::mapLip + planbranch::rules::mapNeighbour * std::hypot(u, v)) continue; su += u; sv += v; suu += u * u; suv += u * v; svv += v * v; suz += u * w; svz += v * w; ++m; } }
            const double D = suu * svv - suv * suv; if (m >= 3 && std::fabs(D) > 1e-6) { gx = (suz * svv - svz * suv) / D; gy = (svz * suu - suz * suv) / D; }
            q.push_back({ sx, sy, k, gx, gy }); }
        const double r2 = radius * radius;
        for (std::size_t head = 0; head < q.size() && int(out.size()) < maxCells; ++head) {
            const Item c = q[head]; const Floor& f = cells.find(KeyOf(c.ix, c.iy))->second;
            for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) { if (!dx && !dy) continue;
                const std::int64_t nx = c.ix + dx, ny = c.iy + dy; const double cxm = (nx + .5) * kCell - x, cym = (ny + .5) * kCell - y; if (cxm * cxm + cym * cym > r2) continue;
                const auto key = KeyOf(nx, ny); const auto jt = cells.find(key); if (jt == cells.end()) continue;
                const Floor& g = jt->second; std::uint8_t mask = 0; { const auto r = out.find(key); if (r != out.end()) mask = r->second; }
                for (int i = 0; i < g.n; ++i) { if ((mask >> i) & 1) continue;
                    const double u = g.x[i] - f.x[c.level], v = g.y[i] - f.y[c.level], dist = std::hypot(u, v);
                    const double predicted = f.z[c.level] + c.gx * u + c.gy * v;
                    if (std::fabs(g.z[i] - predicted) > planbranch::rules::mapLip + planbranch::rules::mapBend * dist) continue;
                    mask |= std::uint8_t(1u << i); out[key] = mask; ++levels;
                    double gx = c.gx, gy = c.gy; fitSlope(nx, ny, i, gx, gy);
                    q.push_back({ nx, ny, i, gx, gy }); } }
        }
        return levels;
    }
    void Clear() { cells.clear(); }
};

// ---- Clear-path sweep (user 2026-11-07: sweep for the non-colliding paths, reactiveness only for what changes) ----
// Level rays along each lane at wheel and body height, on lines across the road, find the static solids near it. World keeps them; the
// line through them is Avoid's (planbranch::ClearBranch). Live hits on a known solid are not news; everything else stays reactive.
constexpr double kScanStep = planbranch::rules::scanStep, kScanLow = planbranch::rules::scanLow, kScanHigh = planbranch::rules::scanHigh;
constexpr int kScanHalf = planbranch::rules::scanHalf, kScanBins = 2 * kScanHalf + 1;
constexpr double kSolidBucket = 8.0, kSolidMerge = .75;   // storage only
// A face is only a solid if it really stands up from the floor. A scan ray runs level, so on a rising road it meets the edge of the next
// road block at scan height though the step there is small (2026-11-07 run: every stop was a false solid on open road, 0.2-0.5 m lips).
// So the floor is measured just before and just behind each face: a small step with a floor on top is road. Live rays still see it.
inline bool IsLip(bool haveBefore, double before, bool haveAfter, double after) { return haveBefore && haveAfter && after - before <= planbranch::rules::lipStep; }

struct Solid { float x = 0, y = 0, lo = 0, hi = 0; std::uint32_t at = 0; };
struct Solids {
    std::unordered_map<std::uint64_t, std::vector<Solid>> buckets; int count = 0;
    static std::uint64_t Key(double x, double y) {
        const auto ix = std::int64_t(std::floor(x / kSolidBucket)), iy = std::int64_t(std::floor(y / kSolidBucket));
        return (std::uint64_t(std::uint32_t(ix)) << 32) | std::uint32_t(iy);
    }
    // A face hit at height z over the floor `floorZ`. The same solid seen again (or higher up) grows, it is not a second one.
    void Note(double x, double y, double floorZ, double z, std::uint32_t stamp) {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(floorZ)) return;
        auto& list = buckets[Key(x, y)];
        for (auto& s : list) if (std::hypot(s.x - x, s.y - y) < kSolidMerge && z >= s.lo - 2 && z <= s.hi + 2) { s.hi = std::max(s.hi, float(z + .1)); s.lo = std::min(s.lo, float(floorZ)); s.at = stamp; return; }
        list.push_back({ float(x), float(y), float(floorZ), float(z + .1), stamp }); ++count;
    }
    template <class Fn> void Near(double x, double y, double radius, Fn&& fn) const {
        const int n = int(std::ceil(radius / kSolidBucket));
        for (int i = -n; i <= n; ++i) for (int j = -n; j <= n; ++j) {
            const auto it = buckets.find(Key(x + i * kSolidBucket, y + j * kSolidBucket)); if (it == buckets.end()) continue;
            for (const auto& s : it->second) if (std::hypot(s.x - x, s.y - y) <= radius) fn(s);
        }
    }
    // A live hit at this point is on a solid the sweep already knows.
    bool Known(double x, double y, double z, double radius = .9) const {
        bool got = false; Near(x, y, radius, [&](const Solid& s) { if (z >= s.lo - .3 && z <= s.hi + .6) got = true; }); return got;
    }
    // Rays passed clean through here: the prediction was wrong, and the newer measurement wins.
    int Forget(double x, double y, double radius = 1.0) { int gone = 0;
        for (int i = -1; i <= 1; ++i) for (int j = -1; j <= 1; ++j) { const auto it = buckets.find(Key(x + i * kSolidBucket, y + j * kSolidBucket)); if (it == buckets.end()) continue;
            const auto before = it->second.size(); it->second.erase(std::remove_if(it->second.begin(), it->second.end(), [&](const Solid& s) { return std::hypot(s.x - x, s.y - y) <= radius; }), it->second.end()); gone += int(before - it->second.size()); }
        count -= gone; return gone; }
    // After a sweep: solids in its area that it did not see again are gone (a solid is never kept on an old answer).
    void Prune(double cx, double cy, double radius, std::uint32_t stamp) {
        for (auto& [k, list] : buckets) { const auto before = list.size();
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Solid& s) { return s.at != stamp && std::hypot(s.x - cx, s.y - cy) < radius; }), list.end());
            count -= int(before - list.size()); }
    }
    void Clear() { buckets.clear(); count = 0; }
};

// One row across the road, every kScanStep along a lane.
struct Row { float x = 0, y = 0, z = 0, ux = 0, uy = 0; bool blocked[kScanBins] = {}; };
inline std::uint16_t MaskOf(const Row& r) { std::uint16_t m = 0; for (int b = 0; b < kScanBins; ++b) if (r.blocked[b]) m |= std::uint16_t(1u << b); return m; }
// A fact about the row: is there room for a car anywhere across it? (The F7 warning. The line itself is Avoid's.)
inline bool RowHasRoom(const std::vector<Row>& rows, int i) {
    for (int b = 0; b < kScanBins; ++b) { bool ok = true;
        for (int r = std::max(0, i - 1); r <= std::min(int(rows.size()) - 1, i + 1) && ok; ++r)
            for (int c = std::max(0, b - planbranch::rules::bodyBins); c <= std::min(kScanBins - 1, b + planbranch::rules::bodyBins); ++c) if (rows[std::size_t(r)].blocked[c]) { ok = false; break; }
        if (ok) return true; }
    return false;
}

// What the sweep measured across a lane at a point. Lookup by position.
struct ClearCell { float x = 0, y = 0, z = 0, ux = 0, uy = 0; std::uint16_t blocked = 0; bool noWay = false; };
struct ClearMap {
    std::unordered_map<std::uint64_t, std::vector<ClearCell>> cells;
    static std::uint64_t Key(double x, double y) {
        const auto ix = std::int64_t(std::floor(x / kScanStep)), iy = std::int64_t(std::floor(y / kScanStep));
        return (std::uint64_t(std::uint32_t(ix)) << 32) | std::uint32_t(iy);
    }
    void Put(const ClearCell& c) { auto& l = cells[Key(c.x, c.y)]; for (auto& o : l) if (std::hypot(o.x - c.x, o.y - c.y) < .5 && std::fabs(o.z - c.z) < 3) { o = c; return; } l.push_back(c); }
    const ClearCell* At(double x, double y, double z) const {
        const ClearCell* got = nullptr; double best = 2.5;
        for (int i = -1; i <= 1; ++i) for (int j = -1; j <= 1; ++j) { const auto it = cells.find(Key(x + i * kScanStep, y + j * kScanStep)); if (it == cells.end()) continue;
            for (const auto& c : it->second) { if (std::fabs(c.z - z) >= 3) continue; const double d = std::hypot(c.x - x, c.y - y); if (d < best) { best = d; got = &c; } } }
        return got;
    }
    void Clear() { cells.clear(); }
};

struct SweepView { Solids solids; ClearMap rows; };

// The source owner serializes Read, Write and Clear. A reader never sees a partial mutation.
class SweepStore {
public:
    std::shared_ptr<const SweepView> Read() const { return current; }
    const SweepView& View() const { return *current; }
    SweepView& Write() {
        if (current.use_count() != 1) current = std::make_shared<SweepView>(*current);
        return *current;
    }
    void Clear() { current = std::make_shared<SweepView>(); }
private:
    std::shared_ptr<SweepView> current = std::make_shared<SweepView>();
};

// One step along a surveyed lane: the jump between two neighbouring floor samples (1 m apart). A jump this high is a step edge, not
// a ramp: a ramp rises a little each metre.
constexpr double kEdge = .15;
struct EdgeCount { int edges = 0, overFace = 0, overHalf = 0; double highest = 0; };
inline void CountEdge(EdgeCount& e, double from, double to, double faceStep) {
    const double jump = std::fabs(to - from);
    if (jump < kEdge) return;
    ++e.edges; if (jump > faceStep) ++e.overFace; if (jump > .5) ++e.overHalf; if (jump > e.highest) e.highest = jump;
}
}
