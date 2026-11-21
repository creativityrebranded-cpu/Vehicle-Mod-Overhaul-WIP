#pragma once
// Car navmesh bake (2026-11-05, the user: "Download Recast"; design notes/navmesh-format-20261105/FORMAT.md). Pure: no engine calls.
// Input: triangles in Starfield's frame (metres, Z up), e.g. the game's own navmesh input collected by navprobe (12-byte vertices,
// 24-byte triangles: 3 indices + extra) or any plain mesh. Output: the walkable surface for a car (Recast with car settings) and a
// Detour navmesh that answers A* path queries.
// Recast is Y up: Starfield (x, y, z) <-> Recast (x, z, -y), a proper rotation, so triangle winding (and walkable up-facing) is kept.
#include "Recast.h"
#include "DetourCommon.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace navbake
{
    // Car-sized defaults. agentRadius: the car's half-width plus margin (Recast keeps the agent's centre this far from any edge or wall).
    struct Params
    {
        float cellSize = 0.25f, cellHeight = 0.10f;
        float agentRadius = 1.3f, agentHeight = 2.2f, agentClimb = 0.35f, slopeDeg = 30.0f;
        float regionMin = 8.0f, regionMerge = 20.0f;      // cells per side of the smallest island kept / merged
        float edgeMaxLen = 12.0f, edgeMaxError = 1.3f;     // metres, cells
        int vertsPerPoly = 6;
        float detailSample = 6.0f, detailMaxError = 1.0f;  // in cells / cell heights
        int maxCells = 16 * 1024 * 1024;                   // grid columns refused above this (time and memory)
    };

    struct Input
    {
        const float* verts = nullptr; int nverts = 0;          // x y z, Starfield frame
        const std::uint32_t* tris = nullptr; int ntris = 0;    // triangle i = tris[i * stride + 0..2]
        int stride = 3;                                        // 6 for the game's 24-byte triangles
        float bmin[3] = {}, bmax[3] = {};                      // the box to bake, Starfield frame (tile: only z is used)
        // Tile mode (navmesh.cpp): bake the game cell (tileX, tileY) of tileSize metres as one Detour tile with a border, so tiles of
        // neighbouring cells join. Starfield cell (gx, gy) = x [gx*S, gx*S+S], y [gy*S, gy*S+S] -> Recast tile (gx, -gy-1).
        bool tile = false; int tileX = 0, tileY = 0; float tileSize = 100.0f;
    };

    // The tiled Detour mesh all cell tiles go into (origin 0, one tile per game cell). 10 tile bits + 12 poly bits leave 10 salt bits.
    constexpr int kMaxTiles = 1024, kMaxTilePolys = 4096;
    inline dtNavMeshParams TiledParams(float tileSize = 100.0f)
    {
        dtNavMeshParams np{};
        np.orig[0] = np.orig[1] = np.orig[2] = 0; np.tileWidth = np.tileHeight = tileSize; np.maxTiles = kMaxTiles; np.maxPolys = kMaxTilePolys;
        return np;
    }

    struct Result
    {
        bool ok = false; const char* failed = "";            // the stage that failed
        int gridW = 0, gridH = 0, walkableTris = 0, spans = 0, compactSpans = 0, regions = 0, contours = 0;
        int polys = 0, polyVerts = 0, detailTris = 0;
        double walkableArea = 0;                             // m^2 of the detail surface
        double msRaster = 0, msCompact = 0, msRegions = 0, msMesh = 0, msDetour = 0, msTotal = 0;
        std::vector<float> outVerts;                         // the walkable surface, Starfield frame
        std::vector<int> outTris;
        unsigned char* navData = nullptr; int navDataSize = 0;  // Detour tile (dtFree with Free())
        void Free() { if (navData) { dtFree(navData); navData = nullptr; navDataSize = 0; } }
    };

    inline void ToRecast(const float* s, float* r) { r[0] = s[0]; r[1] = s[2]; r[2] = -s[1]; }
    inline void ToStarfield(const float* r, float* s) { s[0] = r[0]; s[1] = -r[2]; s[2] = r[1]; }

    inline Result Bake(const Input& in, const Params& p)
    {
        using clk = std::chrono::steady_clock;
        auto ms = [](clk::time_point a, clk::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
        const auto t0 = clk::now();
        Result res;
        if (!in.verts || !in.tris || in.nverts < 3 || in.ntris < 1 || in.stride < 3) { res.failed = "no_input"; return res; }

        std::vector<float> rv(std::size_t(in.nverts) * 3);
        for (int i = 0; i < in.nverts; ++i) ToRecast(in.verts + i * 3, rv.data() + i * 3);
        std::vector<int> rt; rt.reserve(std::size_t(in.ntris) * 3);
        for (int i = 0; i < in.ntris; ++i) {
            const std::uint32_t* t = in.tris + std::size_t(i) * in.stride;
            if (t[0] >= std::uint32_t(in.nverts) || t[1] >= std::uint32_t(in.nverts) || t[2] >= std::uint32_t(in.nverts)) continue;
            rt.push_back(int(t[0])); rt.push_back(int(t[1])); rt.push_back(int(t[2]));
        }
        const int nt = int(rt.size() / 3);
        float sb0[3] = { in.bmin[0], in.bmin[1], in.bmin[2] }, sb1[3] = { in.bmax[0], in.bmax[1], in.bmax[2] };
        float bmin[3], bmax[3];
        bmin[0] = sb0[0]; bmin[1] = sb0[2]; bmin[2] = -sb1[1];
        bmax[0] = sb1[0]; bmax[1] = sb1[2]; bmax[2] = -sb0[1];

        rcConfig cfg{};
        cfg.cs = p.cellSize; cfg.ch = p.cellHeight;
        cfg.walkableSlopeAngle = p.slopeDeg;
        cfg.walkableHeight = int(std::ceil(p.agentHeight / cfg.ch));
        cfg.walkableClimb = int(std::floor(p.agentClimb / cfg.ch));
        cfg.walkableRadius = int(std::ceil(p.agentRadius / cfg.cs));
        cfg.maxEdgeLen = int(p.edgeMaxLen / cfg.cs);
        cfg.maxSimplificationError = p.edgeMaxError;
        cfg.minRegionArea = int(p.regionMin * p.regionMin);
        cfg.mergeRegionArea = int(p.regionMerge * p.regionMerge);
        cfg.maxVertsPerPoly = std::clamp(p.vertsPerPoly, 3, DT_VERTS_PER_POLYGON);
        cfg.detailSampleDist = p.detailSample < 0.9f ? 0 : cfg.cs * p.detailSample;
        cfg.detailSampleMaxError = cfg.ch * p.detailMaxError;
        rcVcopy(cfg.bmin, bmin); rcVcopy(cfg.bmax, bmax);
        if (in.tile) {
            // The cell's own square plus a border of (radius + 3) cells: Recast trims the border back off, so each tile ends exactly on the
            // cell edge with the walls of its neighbours' edges already seen.
            const float S = in.tileSize;
            cfg.borderSize = cfg.walkableRadius + 3;
            cfg.tileSize = int(S / cfg.cs + 0.5f);
            cfg.width = cfg.height = cfg.tileSize + 2 * cfg.borderSize;
            const float pad = cfg.borderSize * cfg.cs;
            cfg.bmin[0] = in.tileX * S - pad; cfg.bmax[0] = (in.tileX + 1) * S + pad;
            cfg.bmin[2] = -(in.tileY + 1) * S - pad; cfg.bmax[2] = -in.tileY * S + pad;
        } else
            rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
        res.gridW = cfg.width; res.gridH = cfg.height;
        if (cfg.width <= 0 || cfg.height <= 0) { res.failed = "empty_box"; return res; }
        if (double(cfg.width) * cfg.height > p.maxCells) { res.failed = "box_too_large"; return res; }

        rcContext ctx(false);
        rcHeightfield* hf = rcAllocHeightfield();
        rcCompactHeightfield* chf = nullptr;
        rcContourSet* cset = nullptr;
        rcPolyMesh* pm = nullptr;
        rcPolyMeshDetail* dm = nullptr;
        auto done = [&](const char* failed) {
            res.failed = failed; res.ok = !failed || !*failed;
            rcFreeHeightField(hf); rcFreeCompactHeightfield(chf); rcFreeContourSet(cset); rcFreePolyMesh(pm); rcFreePolyMeshDetail(dm);
            res.msTotal = ms(t0, clk::now());
            return res;
        };
        if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return done("heightfield");
        std::vector<unsigned char> areas(std::size_t(nt), 0);
        rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, rv.data(), in.nverts, rt.data(), nt, areas.data());
        for (unsigned char a : areas) if (a) ++res.walkableTris;
        if (!rcRasterizeTriangles(&ctx, rv.data(), in.nverts, rt.data(), areas.data(), nt, *hf, cfg.walkableClimb)) return done("rasterize");
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);
        res.spans = 0;
        for (int i = 0; i < hf->width * hf->height; ++i) for (const rcSpan* s = hf->spans[i]; s; s = s->next) ++res.spans;
        const auto t1 = clk::now(); res.msRaster = ms(t0, t1);

        chf = rcAllocCompactHeightfield();
        if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)) return done("compact");
        rcFreeHeightField(hf); hf = nullptr;
        if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) return done("erode");
        res.compactSpans = chf->spanCount;
        const auto t2 = clk::now(); res.msCompact = ms(t1, t2);

        if (!rcBuildDistanceField(&ctx, *chf)) return done("distance_field");
        if (!rcBuildRegions(&ctx, *chf, cfg.borderSize, cfg.minRegionArea, cfg.mergeRegionArea)) return done("regions");
        res.regions = chf->maxRegions;
        const auto t3 = clk::now(); res.msRegions = ms(t2, t3);

        cset = rcAllocContourSet();
        if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) return done("contours");
        res.contours = cset->nconts;
        pm = rcAllocPolyMesh();
        if (!pm || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pm)) return done("poly_mesh");
        dm = rcAllocPolyMeshDetail();
        if (!dm || !rcBuildPolyMeshDetail(&ctx, *pm, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dm)) return done("detail_mesh");
        res.polys = pm->npolys; res.polyVerts = pm->nverts; res.detailTris = dm->ntris;
        const auto t4 = clk::now(); res.msMesh = ms(t3, t4);

        // The walkable surface (detail triangles) back in Starfield's frame, with its area.
        res.outVerts.resize(std::size_t(dm->nverts) * 3);
        for (int i = 0; i < dm->nverts; ++i) ToStarfield(dm->verts + i * 3, res.outVerts.data() + i * 3);
        for (int m = 0; m < dm->nmeshes; ++m) {
            const unsigned int vb = dm->meshes[m * 4 + 0], tb = dm->meshes[m * 4 + 2], tn = dm->meshes[m * 4 + 3];
            for (unsigned int k = 0; k < tn; ++k) {
                const unsigned char* t = &dm->tris[(tb + k) * 4];
                const int a = int(vb + t[0]), b = int(vb + t[1]), c = int(vb + t[2]);
                res.outTris.push_back(a); res.outTris.push_back(b); res.outTris.push_back(c);
                const float* A = &res.outVerts[a * 3]; const float* B = &res.outVerts[b * 3]; const float* C = &res.outVerts[c * 3];
                const double ux = B[0] - A[0], uy = B[1] - A[1], uz = B[2] - A[2], vx = C[0] - A[0], vy = C[1] - A[1], vz = C[2] - A[2];
                res.walkableArea += 0.5 * std::sqrt((uy * vz - uz * vy) * (uy * vz - uz * vy) + (uz * vx - ux * vz) * (uz * vx - ux * vz) + (ux * vy - uy * vx) * (ux * vy - uy * vx));
            }
        }

        // Detour tile (single tile). Every walkable polygon gets flag 1 so the default filter accepts it.
        if (in.tile && pm->npolys > kMaxTilePolys) return done("too_many_polys");
        if (pm->npolys > 0 && pm->nverts < 0xffff) {
            for (int i = 0; i < pm->npolys; ++i) pm->flags[i] = pm->areas[i] == RC_WALKABLE_AREA ? 1 : 0;
            dtNavMeshCreateParams np{};
            np.verts = pm->verts; np.vertCount = pm->nverts; np.polys = pm->polys; np.polyAreas = pm->areas; np.polyFlags = pm->flags;
            np.polyCount = pm->npolys; np.nvp = pm->nvp;
            np.detailMeshes = dm->meshes; np.detailVerts = dm->verts; np.detailVertsCount = dm->nverts; np.detailTris = dm->tris; np.detailTriCount = dm->ntris;
            np.walkableHeight = p.agentHeight; np.walkableRadius = p.agentRadius; np.walkableClimb = p.agentClimb;
            rcVcopy(np.bmin, pm->bmin); rcVcopy(np.bmax, pm->bmax);
            np.cs = cfg.cs; np.ch = cfg.ch; np.buildBvTree = true;
            if (in.tile) { np.tileX = in.tileX; np.tileY = -in.tileY - 1; np.tileLayer = 0; }
            if (!dtCreateNavMeshData(&np, &res.navData, &res.navDataSize)) { res.navData = nullptr; res.navDataSize = 0; }
        }
        res.msDetour = ms(t4, clk::now());
        return done(res.navData ? "" : "detour_tile");
    }

    // Fingerprint of a cell's collision input (2026-11-05, the user: "Add fingerprint"; road mods change cells): triangles' positions
    // rounded to 1 cm, each triangle's three corners sorted, combined by addition, so neither the game's collection order nor its
    // vertex numbering nor the per-triangle extra data change it. Anything with collision added, removed or moved by more than ~1 cm does.
    inline std::uint64_t Mix(std::uint64_t x)
    {
        x += 0x9E3779B97F4A7C15ull; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull; x = (x ^ (x >> 27)) * 0x94D049BB133111EBull; return x ^ (x >> 31);
    }
    inline std::uint64_t InputFingerprint(const float* v, int nv, const std::uint32_t* t, int nt, int stride)
    {
        std::uint64_t sum = 0; int used = 0;
        for (int i = 0; i < nt; ++i) {
            const std::uint32_t* tri = t + std::size_t(i) * stride;
            if (tri[0] >= std::uint32_t(nv) || tri[1] >= std::uint32_t(nv) || tri[2] >= std::uint32_t(nv)) continue;
            std::uint64_t k[3];
            for (int c = 0; c < 3; ++c) {
                const float* p = v + std::size_t(tri[c]) * 3;
                const auto q = [](float f) { return std::uint64_t(std::int64_t(std::llround(double(f) * 100.0)) & 0x1FFFFF); };
                k[c] = Mix((q(p[0]) << 42) ^ (q(p[1]) << 21) ^ q(p[2]));
            }
            std::sort(k, k + 3);
            sum += Mix(k[0] ^ Mix(k[1] ^ Mix(k[2])));
            ++used;
        }
        return Mix(sum ^ (std::uint64_t(used) << 1));
    }

    // Cell edges (2026-11-06 10:37 run: at the -6,14 / -6,15 edge the road stopped 4 m short on one side, the two tiles never joined
    // and A* ended at the edge every time). Recast trims a tile's border back off, but it only knows what is in the input: a road piece
    // the game files under the next cell is missing from this cell's input, the road "ends" at the line, and the car margin is cut
    // back from that end. So a tile is baked with its neighbours' triangles that reach into its border as well.
    // BorderPad: Recast's tile border with these settings (car radius + 3 cells) plus a margin, metres.
    inline float BorderPad(const Params& p = Params{}) { return float(std::ceil(p.agentRadius / p.cellSize) + 3) * p.cellSize + 0.5f; }
    // Appends to (ov, ot) every triangle of (v, t) whose xy box passes `keep(lx, ly, hx, hy)`, with the vertices it uses (renumbered
    // after those already in ov) and its extra dwords (stride). Returns the number appended.
    template <class Keep>
    inline int AppendIf(const float* v, int nv, const std::uint32_t* t, int nt, int stride, Keep keep, std::vector<float>& ov, std::vector<std::uint32_t>& ot)
    {
        if (!v || !t || nv <= 0 || nt <= 0 || stride < 3) return 0;
        std::vector<std::uint32_t> remap(std::size_t(nv), 0xFFFFFFFFu);
        int added = 0;
        for (int i = 0; i < nt; ++i) {
            const std::uint32_t* tri = t + std::size_t(i) * stride;
            if (tri[0] >= std::uint32_t(nv) || tri[1] >= std::uint32_t(nv) || tri[2] >= std::uint32_t(nv)) continue;
            float lx = 1e30f, ly = 1e30f, hx = -1e30f, hy = -1e30f;
            for (int c = 0; c < 3; ++c) { const float* p = v + std::size_t(tri[c]) * 3; lx = std::min(lx, p[0]); hx = std::max(hx, p[0]); ly = std::min(ly, p[1]); hy = std::max(hy, p[1]); }
            if (!keep(lx, ly, hx, hy)) continue;
            for (int c = 0; c < 3; ++c) {
                std::uint32_t& r = remap[tri[c]];
                if (r == 0xFFFFFFFFu) { r = std::uint32_t(ov.size() / 3); const float* p = v + std::size_t(tri[c]) * 3; ov.push_back(p[0]); ov.push_back(p[1]); ov.push_back(p[2]); }
                ot.push_back(r);
            }
            for (int k = 3; k < stride; ++k) ot.push_back(tri[k]);
            ++added;
        }
        return added;
    }
    // The triangles overlapping the box [x0, x1] x [y0, y1] (a tile and its border).
    inline int AppendOverlapping(const float* v, int nv, const std::uint32_t* t, int nt, int stride, float x0, float y0, float x1, float y1, std::vector<float>& ov, std::vector<std::uint32_t>& ot)
    {
        return AppendIf(v, nv, t, nt, stride, [&](float lx, float ly, float hx, float hy) { return hx >= x0 && lx <= x1 && hy >= y0 && ly <= y1; }, ov, ot);
    }
    // A cell's edge band: its triangles not wholly inside the cell's square shrunk by `pad` (the only ones a neighbour's border can need).
    inline int AppendEdgeBand(const float* v, int nv, const std::uint32_t* t, int nt, int stride, float x0, float y0, float x1, float y1, float pad, std::vector<float>& ov, std::vector<std::uint32_t>& ot)
    {
        return AppendIf(v, nv, t, nt, stride, [&](float lx, float ly, float hx, float hy) { return !(lx > x0 + pad && hx < x1 - pad && ly > y0 + pad && hy < y1 - pad); }, ov, ot);
    }

    // Heights of the baked walkable surface at (x, y), Starfield frame (several where decks stack).
    inline std::vector<float> SurfaceAt(const Result& r, float x, float y)
    {
        std::vector<float> zs;
        for (std::size_t i = 0; i + 2 < r.outTris.size(); i += 3) {
            const float* a = &r.outVerts[r.outTris[i] * 3]; const float* b = &r.outVerts[r.outTris[i + 1] * 3]; const float* c = &r.outVerts[r.outTris[i + 2] * 3];
            const float d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
            if (std::fabs(d) < 1e-9f) continue;
            const float l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / d, l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / d, l3 = 1 - l1 - l2;
            if (l1 >= -1e-4f && l2 >= -1e-4f && l3 >= -1e-4f) zs.push_back(l1 * a[2] + l2 * b[2] + l3 * c[2]);
        }
        std::sort(zs.begin(), zs.end(), [](float p, float q) { return p > q; });
        return zs;
    }

    // A* on a baked tile: from `from` to `to` (Starfield frame). found: both ends on the mesh within `snap` metres (horizontal) / 3 m
    // (vertical) and a path; complete: it reaches the end polygon (else partial: as close as the mesh allows). length: the straight path.
    struct Path { bool found = false, complete = false; int polys = 0, corners = 0; double length = 0, startSnap = -1, endSnap = -1; };
    // corners: the straight path's points (Starfield frame), when given.
    inline Path QueryPath(const dtNavMeshQuery* q, const float from[3], const float to[3], float snap, std::vector<float>* corners = nullptr)
    {
        Path out;
        dtQueryFilter filter; filter.setIncludeFlags(0xffff); filter.setExcludeFlags(0);
        const float ext[3] = { snap, 3.0f, snap };
        float a[3], b[3], na[3], nb[3];
        ToRecast(from, a); ToRecast(to, b);
        dtPolyRef ra = 0, rb = 0;
        q->findNearestPoly(a, ext, &filter, &ra, na);
        q->findNearestPoly(b, ext, &filter, &rb, nb);
        if (ra) out.startSnap = std::hypot(na[0] - a[0], na[2] - a[2]);
        if (rb) out.endSnap = std::hypot(nb[0] - b[0], nb[2] - b[2]);
        if (!ra || !rb) return out;
        dtPolyRef path[512]; int n = 0;
        const dtStatus st = q->findPath(ra, rb, na, nb, &filter, path, &n, 512);
        if (!dtStatusSucceed(st) || n <= 0) return out;
        out.found = true; out.polys = n; out.complete = path[n - 1] == rb && !dtStatusDetail(st, DT_PARTIAL_RESULT);
        float sp[256 * 3]; unsigned char fl[256]; dtPolyRef refs[256]; int ns = 0;
        q->findStraightPath(na, nb, path, n, sp, fl, refs, &ns, 256);
        out.corners = ns;
        for (int i = 1; i < ns; ++i) out.length += std::sqrt(double(dtVdistSqr(&sp[(i - 1) * 3], &sp[i * 3])));
        if (corners) { corners->resize(std::size_t(ns) * 3); for (int i = 0; i < ns; ++i) ToStarfield(&sp[i * 3], corners->data() + i * 3); }
        return out;
    }

    inline Path FindPath(const Result& r, const float from[3], const float to[3], float snap = 2.0f)
    {
        Path out;
        if (!r.navData) return out;
        // dtNavMesh::init with flag 0 does not take ownership: the data stays the Result's.
        dtNavMesh* nav = dtAllocNavMesh();
        dtNavMeshQuery* q = dtAllocNavMeshQuery();
        if (nav && q && dtStatusSucceed(nav->init(r.navData, r.navDataSize, 0)) && dtStatusSucceed(q->init(nav, 4096))) {
            dtQueryFilter filter; filter.setIncludeFlags(0xffff); filter.setExcludeFlags(0);
            const float ext[3] = { snap, 3.0f, snap };
            float a[3], b[3], na[3], nb[3];
            ToRecast(from, a); ToRecast(to, b);
            dtPolyRef ra = 0, rb = 0;
            q->findNearestPoly(a, ext, &filter, &ra, na);
            q->findNearestPoly(b, ext, &filter, &rb, nb);
            if (ra) out.startSnap = std::hypot(na[0] - a[0], na[2] - a[2]);
            if (rb) out.endSnap = std::hypot(nb[0] - b[0], nb[2] - b[2]);
            if (ra && rb) {
                dtPolyRef path[512]; int n = 0;
                const dtStatus st = q->findPath(ra, rb, na, nb, &filter, path, &n, 512);
                if (dtStatusSucceed(st) && n > 0) {
                    out.found = true; out.polys = n; out.complete = path[n - 1] == rb && !dtStatusDetail(st, DT_PARTIAL_RESULT);
                    float sp[256 * 3]; unsigned char fl[256]; dtPolyRef refs[256]; int ns = 0;
                    q->findStraightPath(na, nb, path, n, sp, fl, refs, &ns, 256);
                    out.corners = ns;
                    for (int i = 1; i < ns; ++i) out.length += std::sqrt(double(dtVdistSqr(&sp[(i - 1) * 3], &sp[i * 3])));
                }
            }
        }
        dtFreeNavMeshQuery(q);
        dtFreeNavMesh(nav);  // flag 0 above: frees the mesh object, not r.navData
        return out;
    }
}
