// navbake: the car navmesh from triangles (2026-11-05). No arguments: a synthetic deck (post, 1 m railing, low kerb, low slab
// overhead) checked against car rules. With arguments: bake a real capture from the game (navprobe's .obj) and report.
//   navbake_test <in.obj> <out.obj> [x y z]   (x y z: a start point, e.g. the player, for four 30 m path tests)
#include "navbake.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
static void check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    ok ? ++g_pass : ++g_fail;
}

struct Mesh { std::vector<float> v; std::vector<std::uint32_t> t; };
static void Quad(Mesh& m, float x0, float y0, float x1, float y1, float z)  // up-facing (counter-clockwise from above)
{
    const std::uint32_t b = std::uint32_t(m.v.size() / 3);
    for (float p : { x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z }) m.v.push_back(p);
    for (std::uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) m.t.push_back(b + i);
}
static void Box(Mesh& m, float x0, float y0, float z0, float x1, float y1, float z1)  // closed box, outward faces
{
    const std::uint32_t b = std::uint32_t(m.v.size() / 3);
    const float c[8][3] = { {x0,y0,z0},{x1,y0,z0},{x1,y1,z0},{x0,y1,z0},{x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1} };
    for (auto& p : c) for (int k = 0; k < 3; ++k) m.v.push_back(p[k]);
    const std::uint32_t f[12][3] = { {4,5,6},{4,6,7}, {0,2,1},{0,3,2}, {0,1,5},{0,5,4}, {1,2,6},{1,6,5}, {2,3,7},{2,7,6}, {3,0,4},{3,4,7} };
    for (auto& t : f) for (int k = 0; k < 3; ++k) m.t.push_back(b + t[k]);
}
using navbake::SurfaceAt;
static bool HasNear(const std::vector<float>& zs, float z, float tol) { for (float h : zs) if (std::fabs(h - z) < tol) return true; return false; }

static int Synthetic()
{
    Mesh m;
    Quad(m, -20, -10, 20, 10, 0);            // the deck
    Box(m, -0.15f, -0.15f, 0, 0.15f, 0.15f, 1.0f);   // a post at the centre
    Box(m, -20, 5.9f, 0, 20, 6.1f, 1.0f);    // a 1 m railing across the deck at y = 6
    Box(m, -20, -6.1f, 0, 20, -5.9f, 0.2f);  // a 0.2 m kerb at y = -6 (under the climb)
    Box(m, 10, -4, 1.4f, 20, 4, 1.5f);       // a slab 1.4 m over the deck (too low for a car), its top walkable
    navbake::Input in;
    in.verts = m.v.data(); in.nverts = int(m.v.size() / 3); in.tris = m.t.data(); in.ntris = int(m.t.size() / 3); in.stride = 3;
    in.bmin[0] = -21; in.bmin[1] = -11; in.bmin[2] = -1; in.bmax[0] = 21; in.bmax[1] = 11; in.bmax[2] = 4;
    navbake::Params p;
    navbake::Result r = navbake::Bake(in, p);
    std::printf("bake: ok %d failed '%s' grid %dx%d polys %d detail tris %d area %.0f m2 regions %d, %.1f ms\n", int(r.ok), r.failed, r.gridW, r.gridH,
        r.polys, r.detailTris, r.walkableArea, r.regions, r.msTotal);
    check(r.ok && r.polys > 0 && r.navData, "bake succeeds with a Detour tile");
    check(r.walkableArea > 300 && r.walkableArea < 800, "walkable area within the deck's minus the car margins");
    const auto deck = SurfaceAt(r, -10, -2);
    check(HasNear(deck, 0, 0.3f), "open deck is walkable at deck height");
    check(!HasNear(SurfaceAt(r, 0.9f, 0), 0, 0.3f), "no surface within the car's half-width of the post");
    check(HasNear(SurfaceAt(r, 3.0f, 0), 0, 0.3f), "surface again 3 m from the post");
    check(!HasNear(SurfaceAt(r, 15, 0), 0, 0.3f), "nothing under the 1.4 m slab (car does not fit)");
    check(HasNear(SurfaceAt(r, 15, 0), 1.5f, 0.3f), "the slab's top is walkable");
    check(!HasNear(SurfaceAt(r, -10, 6.0f), 0, 0.3f) && !HasNear(SurfaceAt(r, -10, 5.0f), 0, 0.3f), "no surface on or next to the 1 m railing");
    {
        const float a[3] = { -15, 0, 0 }, b[3] = { 5, 0, 0 };
        const auto path = navbake::FindPath(r, a, b);
        std::printf("  post path: found %d complete %d polys %d corners %d length %.2f\n", int(path.found), int(path.complete), path.polys, path.corners, path.length);
        check(path.found && path.complete && path.length > 20.05 && path.length < 23, "A* goes around the post (longer than the straight 20 m)");
    }
    {
        const float a[3] = { -15, -8.5f, 0 }, b[3] = { -15, -2, 0 };
        const auto path = navbake::FindPath(r, a, b);
        std::printf("  kerb path: found %d complete %d length %.2f\n", int(path.found), int(path.complete), path.length);
        check(path.found && path.complete, "A* crosses the 0.2 m kerb");
    }
    {
        const float a[3] = { -15, 0, 0 }, b[3] = { -15, 8, 0 };
        const auto path = navbake::FindPath(r, a, b);
        std::printf("  railing path: found %d complete %d length %.2f end snap %.2f\n", int(path.found), int(path.complete), path.length, path.endSnap);
        check(!path.complete, "A* cannot cross the 1 m railing");
    }
    r.Free();
    {   // Tiles (navmesh.cpp): two game cells, a deck across their shared edge (x = 0) with a post standing on that edge.
        Mesh t;
        Quad(t, -60, 10, 60, 90, 0);
        Box(t, -0.15f, 49.85f, 0, 0.15f, 50.15f, 1.0f);
        navbake::Input ti;
        ti.verts = t.v.data(); ti.nverts = int(t.v.size() / 3); ti.tris = t.t.data(); ti.ntris = int(t.t.size() / 3); ti.stride = 3;
        ti.bmin[2] = -1; ti.bmax[2] = 4; ti.tile = true; ti.tileSize = 100;
        dtNavMesh* nav = dtAllocNavMesh();
        const dtNavMeshParams np = navbake::TiledParams(100);
        check(nav && dtStatusSucceed(nav->init(&np)), "tiled navmesh initialises (10 tile bits, 12 polygon bits)");
        int added = 0;
        for (int gx : { -1, 0 }) {
            ti.tileX = gx; ti.tileY = 0;
            navbake::Result tr = navbake::Bake(ti, navbake::Params{});
            std::printf("  tile %d,0: ok %d failed '%s' polys %d area %.0f m2\n", gx, int(tr.ok), tr.failed, tr.polys, tr.walkableArea);
            if (tr.ok && tr.navData && dtStatusSucceed(nav->addTile(tr.navData, tr.navDataSize, DT_TILE_FREE_DATA, 0, nullptr))) { ++added; tr.navData = nullptr; }
            else tr.Free();
        }
        check(added == 2, "both cells bake as tiles and join one mesh");
        dtNavMeshQuery* q = dtAllocNavMeshQuery();
        check(q && dtStatusSucceed(q->init(nav, 2048)), "query on the tiled mesh");
        const float a[3] = { -40, 50, 0 }, b[3] = { 40, 50, 0 };
        std::vector<float> corners;
        const auto p = navbake::QueryPath(q, a, b, 2.0f, &corners);
        std::printf("  across the cell edge: found %d complete %d corners %d length %.2f\n", int(p.found), int(p.complete), p.corners, p.length);
        check(p.found && p.complete && p.length > 80.05 && p.length < 84, "A* crosses the cell edge and goes round the post standing on it");
        check(corners.size() >= 6 && std::fabs(corners[0] + 40) < 0.5f && std::fabs(corners[corners.size() - 3] - 40) < 0.5f, "corners come back in Starfield coordinates");
        dtFreeNavMeshQuery(q);
        dtFreeNavMesh(nav);
    }
    {   // Cell edges (2026-11-06 10:37 run): the road across the x = 0 edge is two pieces, and the game files the piece from x = -3 under
        // cell 0. Each cell's own input alone: cell -1's road "ends" 3 m short of the edge, the car margin is cut back, the tiles do not
        // join. With the neighbour's edge band in each tile's border: they join.
        Mesh a, b;  // cell -1's input, cell 0's input
        Quad(a, -60, 40, -3, 60, 0);
        Quad(b, -3, 40, 60, 60, 0);
        auto toGame = [](const Mesh& m) { std::vector<std::uint32_t> t6; for (std::size_t i = 0; i < m.t.size(); i += 3) for (std::uint32_t x : { m.t[i], m.t[i + 1], m.t[i + 2], 0u, 0u, 0u }) t6.push_back(x); return t6; };
        const auto a6 = toGame(a), b6 = toGame(b);
        const float pad = navbake::BorderPad();
        auto bakeBoth = [&](bool withNeighbours, int& added) {
            added = 0;
            dtNavMesh* nav = dtAllocNavMesh(); const dtNavMeshParams np = navbake::TiledParams(100); nav->init(&np);
            for (int gx : { -1, 0 }) {
                const Mesh& own = gx < 0 ? a : b; const auto& own6 = gx < 0 ? a6 : b6;
                const Mesh& nb = gx < 0 ? b : a; const auto& nb6 = gx < 0 ? b6 : a6;
                std::vector<float> v = own.v; std::vector<std::uint32_t> t = own6;
                if (withNeighbours) {
                    std::vector<float> bv; std::vector<std::uint32_t> bt;
                    const float nx0 = gx < 0 ? 0.0f : -100.0f;
                    navbake::AppendEdgeBand(nb.v.data(), int(nb.v.size() / 3), nb6.data(), int(nb6.size() / 6), 6, nx0, 0, nx0 + 100, 100, pad, bv, bt);
                    navbake::AppendOverlapping(bv.data(), int(bv.size() / 3), bt.data(), int(bt.size() / 6), 6, gx * 100.0f - pad, -pad, gx * 100.0f + 100 + pad, 100 + pad, v, t);
                }
                navbake::Input ti; ti.verts = v.data(); ti.nverts = int(v.size() / 3); ti.tris = t.data(); ti.ntris = int(t.size() / 6); ti.stride = 6;
                ti.bmin[2] = -1; ti.bmax[2] = 4; ti.tile = true; ti.tileSize = 100; ti.tileX = gx; ti.tileY = 0;
                navbake::Result tr = navbake::Bake(ti, navbake::Params{});
                if (tr.ok && tr.navData && dtStatusSucceed(nav->addTile(tr.navData, tr.navDataSize, DT_TILE_FREE_DATA, 0, nullptr))) { ++added; tr.navData = nullptr; } else tr.Free();
            }
            dtNavMeshQuery* q = dtAllocNavMeshQuery(); q->init(nav, 2048);
            const float s[3] = { -40, 50, 0 }, e[3] = { 40, 50, 0 };
            const auto p = navbake::QueryPath(q, s, e, 2.0f);
            dtFreeNavMeshQuery(q); dtFreeNavMesh(nav);
            return p;
        };
        int addedOwn = 0, addedNb = 0;
        const auto own = bakeBoth(false, addedOwn), joined = bakeBoth(true, addedNb);
        std::printf("  split road, own input only: tiles %d found %d complete %d length %.1f; with edge bands: tiles %d found %d complete %d length %.1f\n",
            addedOwn, int(own.found), int(own.complete), own.length, addedNb, int(joined.found), int(joined.complete), joined.length);
        check(addedOwn == 2 && !own.complete, "split road, each cell's own input only: the tiles do not join (the 10:38 stop)");
        check(addedNb == 2 && joined.found && joined.complete && joined.length > 79.5 && joined.length < 81, "with the neighbour's edge band: the tiles join, A* crosses the edge straight");
        std::vector<float> ev; std::vector<std::uint32_t> et;
        Mesh inner; Quad(inner, 20, 20, 80, 80, 0); Quad(inner, 90, 20, 99, 30, 0);
        const auto i6 = toGame(inner);
        const int band = navbake::AppendEdgeBand(inner.v.data(), int(inner.v.size() / 3), i6.data(), int(i6.size() / 6), 6, 0, 0, 100, 100, pad, ev, et);
        check(band == 2 && et.size() == 12 && ev.size() == 12, "edge band: only triangles within the border pad of the cell's edge are kept");
    }
    {   // Input fingerprint (2026-11-05, the user: "Add fingerprint"): the same collision in another order, numbering or extra data is the
        // same cell; anything added or moved is not.
        Mesh f;
        Quad(f, -20, -10, 20, 10, 0);
        Box(f, -0.15f, -0.15f, 0, 0.15f, 0.15f, 1.0f);
        std::vector<std::uint32_t> t6;  // the game's 6-dword triangles
        for (std::size_t i = 0; i < f.t.size(); i += 3) for (std::uint32_t x : { f.t[i], f.t[i + 1], f.t[i + 2], 0x3FE66666u, 0u, std::uint32_t(0x200000 + i) }) t6.push_back(x);
        const int nv = int(f.v.size() / 3), nt = int(t6.size() / 6);
        const auto base = navbake::InputFingerprint(f.v.data(), nv, t6.data(), nt, 6);
        // Reversed triangle order, renumbered vertices (reversed), other extra data.
        std::vector<float> rv(f.v.size());
        for (int i = 0; i < nv; ++i) for (int k = 0; k < 3; ++k) rv[std::size_t(nv - 1 - i) * 3 + k] = f.v[std::size_t(i) * 3 + k];
        std::vector<std::uint32_t> rt;
        for (int i = nt - 1; i >= 0; --i) { const std::uint32_t* s = &t6[std::size_t(i) * 6]; for (std::uint32_t x : { std::uint32_t(nv - 1) - s[1], std::uint32_t(nv - 1) - s[2], std::uint32_t(nv - 1) - s[0], 1u, 2u, 3u }) rt.push_back(x); }
        check(navbake::InputFingerprint(rv.data(), nv, rt.data(), nt, 6) == base, "fingerprint: same collision in another order, numbering and extra data is unchanged");
        std::vector<float> moved = f.v; moved[std::size_t(nv - 1) * 3] += 0.05f;  // a post corner 5 cm over
        check(navbake::InputFingerprint(moved.data(), nv, t6.data(), nt, 6) != base, "fingerprint: something moved 5 cm changes it");
        Mesh g = f; Box(g, 5, 5, 0, 6, 6, 1);  // a mod adds a barrier
        std::vector<std::uint32_t> g6;
        for (std::size_t i = 0; i < g.t.size(); i += 3) for (std::uint32_t x : { g.t[i], g.t[i + 1], g.t[i + 2], 0u, 0u, 0u }) g6.push_back(x);
        check(navbake::InputFingerprint(g.v.data(), int(g.v.size() / 3), g6.data(), int(g6.size() / 6), 6) != base, "fingerprint: added collision changes it");
        check(navbake::InputFingerprint(f.v.data(), nv, t6.data(), nt - 2, 6) != base, "fingerprint: removed collision changes it");
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

static int RealCapture(int argc, char** argv)
{
    Mesh m;
    std::ifstream f(argv[1]);
    if (!f) { std::printf("cannot read %s\n", argv[1]); return 2; }
    std::string line;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    while (std::getline(f, line)) {
        if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            std::istringstream s(line.substr(2)); float x, y, z; s >> x >> y >> z;
            m.v.push_back(x); m.v.push_back(y); m.v.push_back(z);
            mn[0] = std::min(mn[0], x); mn[1] = std::min(mn[1], y); mn[2] = std::min(mn[2], z); mx[0] = std::max(mx[0], x); mx[1] = std::max(mx[1], y); mx[2] = std::max(mx[2], z);
        } else if (line.size() > 2 && line[0] == 'f' && line[1] == ' ') {
            std::istringstream s(line.substr(2)); std::uint32_t a, b, c; s >> a >> b >> c;
            m.t.push_back(a - 1); m.t.push_back(b - 1); m.t.push_back(c - 1);
        }
    }
    navbake::Input in;
    in.verts = m.v.data(); in.nverts = int(m.v.size() / 3); in.tris = m.t.data(); in.ntris = int(m.t.size() / 3); in.stride = 3;
    for (int k = 0; k < 3; ++k) { in.bmin[k] = mn[k] - 1; in.bmax[k] = mx[k] + 1; }
    navbake::Params p;
    navbake::Result r = navbake::Bake(in, p);
    std::printf("input %d verts %d tris; bake ok %d failed '%s' grid %dx%d walkable input tris %d spans %d compact %d regions %d contours %d polys %d detail tris %d area %.0f m2\n",
        in.nverts, in.ntris, int(r.ok), r.failed, r.gridW, r.gridH, r.walkableTris, r.spans, r.compactSpans, r.regions, r.contours, r.polys, r.detailTris, r.walkableArea);
    std::printf("ms: raster %.0f compact+erode %.0f regions %.0f mesh %.0f detour %.0f total %.0f; tile %d bytes\n",
        r.msRaster, r.msCompact, r.msRegions, r.msMesh, r.msDetour, r.msTotal, r.navDataSize);
    if (argc > 2) {
        if (FILE* o = std::fopen(argv[2], "wb")) {
            std::fprintf(o, "# FVO car navmesh (Recast, car settings) from %s; Starfield coordinates, Z up\n", argv[1]);
            for (std::size_t i = 0; i < r.outVerts.size(); i += 3) std::fprintf(o, "v %.3f %.3f %.3f\n", r.outVerts[i], r.outVerts[i + 1], r.outVerts[i + 2]);
            for (std::size_t i = 0; i + 2 < r.outTris.size(); i += 3) std::fprintf(o, "f %d %d %d\n", r.outTris[i] + 1, r.outTris[i + 1] + 1, r.outTris[i + 2] + 1);
            std::fclose(o);
        }
    }
    if (argc > 5) {
        const float s[3] = { float(std::atof(argv[3])), float(std::atof(argv[4])), float(std::atof(argv[5])) };
        const auto here = SurfaceAt(r, s[0], s[1]);
        std::printf("surface heights at the start:"); for (float z : here) std::printf(" %.2f", z); std::printf("\n");
        const float dirs[4][2] = { {30, 0}, {-30, 0}, {0, 30}, {0, -30} };
        for (auto& d : dirs) {
            const float e[3] = { s[0] + d[0], s[1] + d[1], s[2] };
            const auto path = navbake::FindPath(r, s, e, 3.0f);
            std::printf("  path to (%+.0f, %+.0f): found %d complete %d polys %d corners %d length %.1f start snap %.2f end snap %.2f\n",
                d[0], d[1], int(path.found), int(path.complete), path.polys, path.corners, path.length, path.startSnap, path.endSnap);
        }
    }
    r.Free();
    return r.ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    return argc > 1 ? RealCapture(argc, argv) : Synthetic();
}
