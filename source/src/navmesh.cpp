#include "navmesh.h"

#include "addresses.h"
#include "engineworker.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "navbake.h"
#include "navprobe.h"
#include "runtimecells.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

extern "C" int FVO_WorldName(char* out, int size);  // context.cpp

namespace navmesh
{
	namespace
	{
		using uptr = std::uintptr_t;
		constexpr float kCell = 100.0f;                    // a game cell, metres
		constexpr std::size_t kKeepTiles = 256;            // tiles kept in memory (~25 MB); the farthest from the player go first
		constexpr std::size_t kMaxWanted = 96;
		constexpr std::size_t kKeepBands = 96;             // cells' edge bands kept for their neighbours' borders (the oldest go first)
		// 2: the input fingerprint in the header. 3 (2026-11-06): the neighbours' edge bands are in each tile's input, with one
		// fingerprint per neighbour (version 2 files are deleted and baked again: their cell edges did not join).
		constexpr std::uint32_t kFileVersion = 3;
		constexpr ULONGLONG kRetryMs = 20000;
		constexpr int kMaxCaptures = 4;                    // input captures per world (test builds)
		// Neighbour i of a cell: (dx, dy); bit i of a neighbour mask.
		constexpr int kNb[8][2] = { {-1,-1},{0,-1},{1,-1},{-1,0},{1,0},{-1,1},{0,1},{1,1} };
		const float g_pad = navbake::BorderPad();          // Recast's tile border with the car settings, plus a margin

		struct Key
		{
			int gx = 0, gy = 0;
			bool operator<(const Key& o) const { return gx < o.gx || (gx == o.gx && gy < o.gy); }
			bool operator==(const Key& o) const { return gx == o.gx && gy == o.gy; }
		};
		Key KeyOf(double x, double y) { return { int(std::floor(x / kCell)), int(std::floor(y / kCell)) }; }
		Key Near(Key k, int i) { return { k.gx + kNb[i][0], k.gy + kNb[i][1] }; }
		int Ring(Key a, Key b) { return std::max(std::abs(a.gx - b.gx), std::abs(a.gy - b.gy)); }

		// nbMask: the neighbours whose edge bands were in the input when it was baked; nbFp[i]: the fingerprint of neighbour i's
		// triangles inside this tile's border (meaningful where nbMask has bit i).
		struct FileHead { char magic[4]; std::uint32_t version, settings; std::int32_t gx, gy; std::uint32_t size; std::uint64_t input; std::uint32_t nbMask, reserved; std::uint64_t nbFp[8]; };
		static_assert(sizeof(FileHead) == 104, "tile file header layout");
		// verified: the tile matches the cell's collision as the game has it now (fingerprint compared); matched: a verify job found it
		// unchanged; replace: a verify job rebaked it (the old tile is swapped out). bandChanged: this cell's edge band differs from the one
		// kept before (its neighbours' borders changed: they are checked again).
		struct Done { unsigned gen = 0; Key k; unsigned char* data = nullptr; int size = 0; bool cached = false, verified = false, matched = false, replace = false, bandChanged = false;
			double msInput = 0, msBake = 0, msNb = 0; int polys = 0, inputTris = 0, nbTris = 0, nbCollected = 0, nbCached = 0; std::uint8_t nbMask = 0; float zmin = 0, zmax = 0;
			std::uint64_t fpOld = 0, fpNew = 0; const char* why = ""; };
		struct Job { unsigned gen = 0; Key k; uptr cell = 0; std::uint32_t cellId = 0; uptr nbCell[8] = {}; std::uint32_t nbId[8] = {}; std::wstring path, dir;
			bool fromFile = false, verifyOnly = false, capture = false; float car[3] = {}; const char* captureWhy = ""; unsigned run = 0; std::uint32_t settings = 0; };

		std::string g_world;
		std::wstring g_dir;
		unsigned g_gen = 0, g_runs = 0;
		dtNavMesh* g_nav = nullptr;
		dtNavMeshQuery* g_query = nullptr;
		std::map<Key, dtTileRef> g_tiles;
		std::map<Key, std::uint8_t> g_nbMask;    // per tile in the mesh: the neighbours it was baked (or checked) with
		std::map<Key, std::uint8_t> g_nbTried;   // per tile: neighbours already checked again after they became available (once per world)
		std::set<Key> g_want;
		std::deque<Key> g_order;
		std::map<Key, ULONGLONG> g_retryAt;
		std::map<Key, const char*> g_wantWhy;
		std::set<Key> g_unverified;   // tiles to compare with the game's input once their cell is next to the player
		std::map<Key, const char*> g_verifyWhy;  // why (cached while far, a neighbour loaded, a neighbour changed)
		// When a tile was last sent to be checked: a neighbour's changed band asks again at most once a minute (a cell whose input
		// changes between collections, e.g. with vehicles moving in it, would otherwise keep its neighbours checking each other).
		std::map<Key, ULONGLONG> g_checkedAt;
		constexpr ULONGLONG kRecheckMs = 60000;
		std::atomic<bool> g_busy{ false };
		std::mutex g_doneLock;
		std::vector<Done> g_done;
		ULONGLONG g_waitLogAt = 0, g_nbCheckAt = 0;
		std::set<Key> g_captured; int g_captures = 0; bool g_captureWanted = false; Key g_captureKey; float g_captureCar[3] = {}; const char* g_captureWhy = "";

		// Each collected cell's edge band (its triangles within the border pad of its edges), kept for its neighbours' borders. Written
		// and read by the worker; cleared on a world change (generation g_bandGen).
		struct Band { std::vector<float> v; std::vector<std::uint32_t> t; std::uint64_t fp = 0; };
		std::mutex g_bandLock;
		std::map<Key, std::shared_ptr<const Band>> g_bands;
		std::deque<Key> g_bandOrder;
		unsigned g_bandGen = 0;

		std::shared_ptr<const Band> MakeBand(Key k, const std::vector<float>& v, const std::vector<std::uint32_t>& t)
		{
			auto b = std::make_shared<Band>();
			navbake::AppendEdgeBand(v.data(), int(v.size() / 3), t.data(), int(t.size() / 6), 6, k.gx * kCell, k.gy * kCell, (k.gx + 1) * kCell, (k.gy + 1) * kCell, g_pad, b->v, b->t);
			b->fp = navbake::InputFingerprint(b->v.data(), int(b->v.size() / 3), b->t.data(), int(b->t.size() / 6), 6);
			return b;
		}
		// True when a band was already kept for the cell and this one differs (the neighbours' borders changed).
		bool StoreBand(unsigned gen, Key k, std::shared_ptr<const Band> b)
		{
			std::lock_guard lk(g_bandLock);
			if (gen != g_bandGen || !b) return false;
			const auto it = g_bands.find(k);
			if (it != g_bands.end()) { const bool changed = it->second->fp != b->fp; it->second = std::move(b); return changed; }
			g_bands[k] = std::move(b); g_bandOrder.push_back(k);
			while (g_bands.size() > kKeepBands && !g_bandOrder.empty()) { g_bands.erase(g_bandOrder.front()); g_bandOrder.pop_front(); }
			return false;
		}
		std::shared_ptr<const Band> FindBand(unsigned gen, Key k)
		{
			std::lock_guard lk(g_bandLock);
			if (gen != g_bandGen) return nullptr;
			const auto it = g_bands.find(k);
			return it != g_bands.end() ? it->second : nullptr;
		}

		std::uint32_t SettingsHash(const navbake::Params& p)
		{
			// FNV-1a over the bake settings and the file version: a tile baked with other settings is baked again.
			const float f[] = { p.cellSize, p.cellHeight, p.agentRadius, p.agentHeight, p.agentClimb, p.slopeDeg, p.regionMin, p.regionMerge,
				p.edgeMaxLen, p.edgeMaxError, p.detailSample, p.detailMaxError, float(p.vertsPerPoly), float(kFileVersion), kCell };
			std::uint32_t h = 2166136261u;
			const auto* b = reinterpret_cast<const unsigned char*>(f);
			for (std::size_t i = 0; i < sizeof f; ++i) { h ^= b[i]; h *= 16777619u; }
			return h;
		}
		const std::uint32_t g_settings = SettingsHash(navbake::Params{});

		std::wstring Slug(const std::string& s)
		{
			std::wstring w;
			for (char c : s) w.push_back((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? wchar_t(c) : (c >= 'A' && c <= 'Z') ? wchar_t(c - 'A' + 'a') : L'_');
			return w.empty() ? L"unknown" : w;
		}
		std::wstring TilePath(const std::wstring& dir, Key k) { return dir + L"\\" + std::to_wstring(k.gx) + L"_" + std::to_wstring(k.gy) + L".tile"; }
		bool Exists(const std::wstring& p) { const DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
		std::string Narrow(const std::wstring& w) { std::string s; for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?'); return s; }

		// The header alone (`data` null) or the header and the tile.
		const char* ReadTile(const std::wstring& path, Key k, std::uint32_t settings, FileHead& h, unsigned char** data, int* size)
		{
			if (data) *data = nullptr;
			if (size) *size = 0;
			h = FileHead{};
			FILE* f = _wfopen(path.c_str(), L"rb");
			if (!f) return "cache_open_failed";
			const char* why = "";
			if (std::fread(&h, sizeof h, 1, f) != 1) why = "cache_short_header";
			else if (std::memcmp(h.magic, "FVNM", 4) != 0 || h.version != kFileVersion) why = "cache_other_format";
			else if (h.settings != settings) why = "cache_other_settings";
			else if (h.gx != k.gx || h.gy != k.gy) why = "cache_other_cell";
			else if (h.size == 0 || h.size > 16u * 1024 * 1024) why = "cache_bad_size";
			else if (data) {
				*data = static_cast<unsigned char*>(dtAlloc(int(h.size), DT_ALLOC_PERM));
				if (!*data) why = "cache_out_of_memory";
				else if (std::fread(*data, 1, h.size, f) != h.size) { dtFree(*data); *data = nullptr; why = "cache_short_data"; }
				else if (size) *size = int(h.size);
			}
			std::fclose(f);
			return why;
		}
		bool WriteTile(const std::wstring& path, Key k, std::uint32_t settings, std::uint64_t input, std::uint8_t nbMask, const std::uint64_t nbFp[8], const unsigned char* data, int size)
		{
			const std::wstring tmp = path + L".tmp";
			FILE* f = _wfopen(tmp.c_str(), L"wb");
			if (!f) return false;
			FileHead h{}; std::memcpy(h.magic, "FVNM", 4); h.version = kFileVersion; h.settings = settings; h.gx = k.gx; h.gy = k.gy; h.size = std::uint32_t(size); h.input = input;
			h.nbMask = nbMask; for (int i = 0; i < 8; ++i) h.nbFp[i] = nbFp[i];
			bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 && std::fwrite(data, 1, std::size_t(size), f) == std::size_t(size);
			ok = std::fclose(f) == 0 && ok;
			if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
			if (!ok) DeleteFileW(tmp.c_str());
			return ok;
		}

		// The neighbours' triangles inside this tile's border, appended to (v, t) (2026-11-06 10:37 run: a road piece the game files under
		// the next cell was missing, the road "ended" 4 m before the cell edge and the tiles never joined). From the kept edge bands, or
		// collected now where the neighbour is loaded (its band then kept). Returns the neighbours present; adds: those that put
		// triangles into the border; fp[i]: the fingerprint of neighbour i's part.
		std::uint8_t AddNeighbours(const Job& j, std::vector<float>& v, std::vector<std::uint32_t>& t, std::uint64_t fp[8], std::uint8_t& adds, Done& d)
		{
			std::uint8_t mask = 0; adds = 0;
			const float x0 = j.k.gx * kCell - g_pad, y0 = j.k.gy * kCell - g_pad, x1 = (j.k.gx + 1) * kCell + g_pad, y1 = (j.k.gy + 1) * kCell + g_pad;
			for (int i = 0; i < 8; ++i) {
				const Key nk = Near(j.k, i);
				std::shared_ptr<const Band> band = FindBand(j.gen, nk);
				if (band) ++d.nbCached;
				else if (j.nbCell[i]) {
					std::vector<float> bv; std::vector<std::uint32_t> bt; double ms = 0;
					if (navprobe::Collect("navmesh", j.run, j.nbCell[i], j.nbId[i], bv, bt, ms)) { band = MakeBand(nk, bv, bt); StoreBand(j.gen, nk, band); ++d.nbCollected; }
					d.msNb += ms;
				}
				if (!band) continue;
				mask |= std::uint8_t(1u << i);
				std::vector<float> sv; std::vector<std::uint32_t> st;
				const int n = navbake::AppendOverlapping(band->v.data(), int(band->v.size() / 3), band->t.data(), int(band->t.size() / 6), 6, x0, y0, x1, y1, sv, st);
				fp[i] = navbake::InputFingerprint(sv.data(), int(sv.size() / 3), st.data(), int(st.size() / 6), 6);
				if (n <= 0) continue;
				adds |= std::uint8_t(1u << i); d.nbTris += n;
				const std::uint32_t base = std::uint32_t(v.size() / 3);
				v.insert(v.end(), sv.begin(), sv.end());
				for (std::size_t q = 0; q < st.size(); ++q) t.push_back(q % 6 < 3 ? st[q] + base : st[q]);
			}
			return mask;
		}

		navbake::Input TileInput(const Job& j, const std::vector<float>& v, const std::vector<std::uint32_t>& t, float& zmin, float& zmax)
		{
			navbake::Input in;
			in.verts = v.data(); in.nverts = int(v.size() / 3); in.tris = t.data(); in.ntris = int(t.size() / 6); in.stride = 6;
			zmin = 1e30f; zmax = -1e30f;
			for (std::size_t i = 2; i < v.size(); i += 3) { zmin = std::min(zmin, v[i]); zmax = std::max(zmax, v[i]); }
			in.bmin[2] = zmin - 1; in.bmax[2] = zmax + 1;
			in.tile = true; in.tileX = j.k.gx; in.tileY = j.k.gy; in.tileSize = kCell;
			return in;
		}

		// Test builds: the input and the car mesh of one cell, and what each has over the car (see navmesh.h Capture).
		bool WriteObj(const std::wstring& path, const char* title, const float* v, int nv, const std::uint32_t* t, int nt, int stride, int ownTris)
		{
			FILE* f = _wfopen(path.c_str(), L"wb");
			if (!f) return false;
			std::fprintf(f, "# %s; Starfield coordinates, Z up\n", title);
			for (int i = 0; i < nv; ++i) std::fprintf(f, "v %.3f %.3f %.3f\n", v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
			for (int i = 0; i < nt; ++i) {
				if (i == 0) std::fprintf(f, "g own\n");
				if (i == ownTris) std::fprintf(f, "g neighbours\n");
				const std::uint32_t* x = t + std::size_t(i) * stride;
				std::fprintf(f, "f %u %u %u\n", x[0] + 1, x[1] + 1, x[2] + 1);
			}
			return std::fclose(f) == 0;
		}
		// The input's surfaces over (x, y), highest first: height, u/d (facing up or down), slope in degrees, the triangle's flags dword,
		// "n" when it came from a neighbour's band. nearCar: how many lie within 0.6 m of the car's height.
		std::string LevelsAt(const float* v, int nv, const std::uint32_t* t, int nt, int stride, int ownTris, float x, float y, float carZ, int& nearCar)
		{
			struct L { float z; bool up; int slope; std::uint32_t flags; bool nb; };
			std::vector<L> ls;
			for (int i = 0; i < nt; ++i) {
				const std::uint32_t* tri = t + std::size_t(i) * stride;
				if (tri[0] >= std::uint32_t(nv) || tri[1] >= std::uint32_t(nv) || tri[2] >= std::uint32_t(nv)) continue;
				const float* a = v + std::size_t(tri[0]) * 3; const float* b = v + std::size_t(tri[1]) * 3; const float* c = v + std::size_t(tri[2]) * 3;
				const float den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
				if (std::fabs(den) < 1e-9f) continue;
				const float l1 = ((b[1] - c[1]) * (x - c[0]) + (c[0] - b[0]) * (y - c[1])) / den, l2 = ((c[1] - a[1]) * (x - c[0]) + (a[0] - c[0]) * (y - c[1])) / den, l3 = 1 - l1 - l2;
				if (l1 < -1e-4f || l2 < -1e-4f || l3 < -1e-4f) continue;
				const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], wx = c[0] - a[0], wy = c[1] - a[1], wz = c[2] - a[2];
				const float nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx, len = std::sqrt(nx * nx + ny * ny + nz * nz);
				const int slope = len > 0 ? int(std::lround(std::acos(std::min(1.0f, std::fabs(nz) / len)) * 57.29578f)) : 90;
				ls.push_back({ l1 * a[2] + l2 * b[2] + l3 * c[2], nz > 0, slope, stride > 5 ? tri[5] : 0u, i >= ownTris });
			}
			std::sort(ls.begin(), ls.end(), [](const L& p, const L& q) { return p.z > q.z; });
			nearCar = 0;
			std::string s; char buf[64]; int n = 0;
			for (const auto& l : ls) {
				if (std::fabs(l.z - carZ) < 0.6f) ++nearCar;
				if (n >= 16) continue;
				std::snprintf(buf, sizeof buf, "%s%.2f%c%d/%X%s", n ? " " : "", l.z, l.up ? 'u' : 'd', l.slope, l.flags, l.nb ? "n" : "");
				s += buf; ++n;
			}
			return s.empty() ? "none" : s;
		}
		void CaptureNow(const Job& j, const std::vector<float>& v, const std::vector<std::uint32_t>& t, int ownTris)
		{
			const int nv = int(v.size() / 3), nt = int(t.size() / 6);
			float zmin = 0, zmax = 0;
			const navbake::Input in = TileInput(j, v, t, zmin, zmax);
			navbake::Result r = navbake::Bake(in, navbake::Params{});
			const std::wstring stem = j.dir + L"\\capture_" + std::to_wstring(j.k.gx) + L"_" + std::to_wstring(j.k.gy) + L"_" + std::to_wstring(j.run);
			const bool wroteIn = WriteObj(stem + L"_input.obj", "FVO car navmesh capture: the game's input for this cell (g own) and its neighbours' edge bands (g neighbours)",
				v.data(), nv, t.data(), nt, 6, ownTris);
			bool wroteCar = false;
			if (FILE* f = _wfopen((stem + L"_car.obj").c_str(), L"wb")) {
				std::fprintf(f, "# FVO car navmesh (Recast, car settings) baked from the capture; Starfield coordinates, Z up\n");
				for (std::size_t i = 0; i < r.outVerts.size(); i += 3) std::fprintf(f, "v %.3f %.3f %.3f\n", r.outVerts[i], r.outVerts[i + 1], r.outVerts[i + 2]);
				for (std::size_t i = 0; i + 2 < r.outTris.size(); i += 3) std::fprintf(f, "f %d %d %d\n", r.outTris[i] + 1, r.outTris[i + 1] + 1, r.outTris[i + 2] + 1);
				wroteCar = std::fclose(f) == 0;
			}
			int nearCar = 0;
			const std::string levels = LevelsAt(v.data(), nv, t.data(), nt, 6, ownTris, j.car[0], j.car[1], j.car[2], nearCar);
			const auto mesh = navbake::SurfaceAt(r, j.car[0], j.car[1]);
			std::string ml; char b[24];
			for (std::size_t i = 0; i < mesh.size() && i < 12; ++i) { std::snprintf(b, sizeof b, "%s%.2f", i ? " " : "", mesh[i]); ml += b; }
			logger::Write("OBS system=navmesh event=capture cell=%d,%d run=%u reason=%s car=%.2f,%.2f,%.2f input_tris=%d own_tris=%d input_at_car_height=%d "
				"input_levels=\"%s\" car_mesh_levels=\"%s\" bake=%s polys=%d input_z=%.1f..%.1f files=%d path=%s",
				j.k.gx, j.k.gy, j.run, j.captureWhy, j.car[0], j.car[1], j.car[2], nt, ownTris, nearCar, levels.c_str(), ml.empty() ? "none" : ml.c_str(),
				r.ok ? "ok" : r.failed, r.polys, zmin, zmax, int(wroteIn && wroteCar), Narrow(stem).c_str());
			r.Free();
		}

		// A cached tile is used only when the cell's collision input, as the game collects it now, has the fingerprint stored with it
		// (2026-11-05, the user: "Add fingerprint"): a road mod or anything else that adds, removes or moves collision rebakes the cell.
		// The same holds for each neighbour's part inside the tile's border (2026-11-06), where that neighbour is available now. The input
		// is cheap (10-55 ms), the bake is not (0.1-1.8 s). Without the cell (not near the player), the tile is loaded unverified and
		// checked once the player comes next to it.
		void Worker(Job j)
		{
			Done d; d.gen = j.gen; d.k = j.k; d.replace = j.verifyOnly;
			try {
				std::vector<float> v; std::vector<std::uint32_t> t;
				bool haveInput = false;
				if (j.cell) {
					haveInput = navprobe::Collect("navmesh", j.run, j.cell, j.cellId, v, t, d.msInput);
					if (haveInput) {
						d.fpNew = navbake::InputFingerprint(v.data(), int(v.size() / 3), t.data(), int(t.size() / 6), 6);
						d.bandChanged = StoreBand(j.gen, j.k, MakeBand(j.k, v, t));
					}
				}
				const int ownTris = int(t.size() / 6);
				std::uint64_t nbFp[8] = {}; std::uint8_t nbAdds = 0;
				if (haveInput) d.nbMask = AddNeighbours(j, v, t, nbFp, nbAdds, d);
				if (j.capture) {
					if (haveInput) CaptureNow(j, v, t, ownTris);
					else logger::Write("OBS system=navmesh event=capture cell=%d,%d run=%u result=no_input_from_game reason=%s", j.k.gx, j.k.gy, j.run, j.captureWhy);
					g_busy = false;
					return;
				}
				bool bake = false;
				if (j.fromFile) {
					FileHead h;
					const char* headWhy = ReadTile(j.path, j.k, j.settings, h, nullptr, nullptr);
					d.fpOld = h.input;
					if (*headWhy) {
						// A file of another format or settings (22:39 run: a version 1 file held its cell back 20 s, until after the crash) is
						// deleted, so the cell is baked fresh as soon as it is near, with no retry wait.
						const bool old = std::strcmp(headWhy, "cache_open_failed") != 0;
						if (old) DeleteFileW(j.path.c_str());
						d.why = haveInput ? headWhy : old ? "cache_old_deleted" : headWhy;
						bake = haveInput;
					}
					else if (haveInput) {
						const char* diff = h.input != d.fpNew ? "input_changed" : "";
						for (int i = 0; i < 8 && !*diff; ++i) {
							const std::uint8_t bit = std::uint8_t(1u << i);
							if (!(d.nbMask & bit)) continue;                                   // not available now: cannot be checked
							if (h.nbMask & bit) { if (h.nbFp[i] != nbFp[i]) diff = "neighbour_changed"; }
							else if (nbAdds & bit) diff = "neighbour_added";                   // baked without it, and it reaches into the border
						}
						if (!*diff) {
							d.matched = d.verified = true; d.why = "cache_input_unchanged";
							d.nbMask = std::uint8_t(h.nbMask | (d.nbMask & ~nbAdds));         // stored ones, and present ones that add nothing
							if (!j.verifyOnly) { d.why = ReadTile(j.path, j.k, j.settings, h, &d.data, &d.size); if (!*d.why) d.why = "cache_input_unchanged"; d.cached = d.data != nullptr; }
						} else { d.why = diff; bake = true; }
					}
					else if (!j.verifyOnly) {
						d.why = ReadTile(j.path, j.k, j.settings, h, &d.data, &d.size);
						if (!*d.why) d.why = j.cell ? "cache_unverified_no_input" : "cache_unverified_cell_not_near";
						d.cached = d.data != nullptr; d.nbMask = std::uint8_t(h.nbMask);
					} else d.why = "verify_no_input";
				} else if (haveInput) bake = true;
				else d.why = j.cell ? "no_input_from_game" : "cell_not_loaded";
				if (bake) {
					const char* cause = d.why;
					const navbake::Input in = TileInput(j, v, t, d.zmin, d.zmax);
					navbake::Result r = navbake::Bake(in, navbake::Params{});
					d.msBake = r.msTotal; d.polys = r.polys; d.inputTris = in.ntris;
					if (r.ok && r.navData) {
						d.data = r.navData; d.size = r.navDataSize; r.navData = nullptr; d.verified = true; d.cached = false;
						const bool wrote = WriteTile(j.path, j.k, j.settings, d.fpNew, d.nbMask, nbFp, d.data, d.size);
						d.why = !wrote ? "baked_cache_write_failed" : std::strcmp(cause, "input_changed") == 0 ? "rebaked_input_changed"
							: std::strcmp(cause, "neighbour_changed") == 0 ? "rebaked_neighbour_changed" : std::strcmp(cause, "neighbour_added") == 0 ? "rebaked_neighbour_added" : "baked";
					} else { d.why = r.failed && *r.failed ? r.failed : "bake_failed"; r.Free(); }
				}
			}
			catch (...) { if (d.data) { dtFree(d.data); d.data = nullptr; } d.why = "exception"; }
			if (!j.capture) { std::lock_guard lk(g_doneLock); g_done.push_back(d); }
			g_busy = false;
		}

		void Reset(const std::string& world)
		{
			if (g_query) { dtFreeNavMeshQuery(g_query); g_query = nullptr; }
			if (g_nav) { dtFreeNavMesh(g_nav); g_nav = nullptr; }
			g_tiles.clear(); g_nbMask.clear(); g_nbTried.clear(); g_want.clear(); g_order.clear(); g_retryAt.clear(); g_wantWhy.clear(); g_unverified.clear(); g_verifyWhy.clear(); g_checkedAt.clear();
			g_captured.clear(); g_captures = 0; g_captureWanted = false;
			++g_gen;
			{ std::lock_guard lk(g_doneLock); for (auto& d : g_done) if (d.data) dtFree(d.data); g_done.clear(); }
			{ std::lock_guard lk(g_bandLock); g_bands.clear(); g_bandOrder.clear(); g_bandGen = g_gen; }
			g_world = world;
			if (world.empty()) { g_dir.clear(); return; }
			g_dir = fvopaths::Dir() + L"\\navcache\\" + Slug(world);
			SHCreateDirectoryExW(nullptr, g_dir.c_str(), nullptr);
			g_nav = dtAllocNavMesh();
			const dtNavMeshParams np = navbake::TiledParams(kCell);
			if (!g_nav || !dtStatusSucceed(g_nav->init(&np))) {
				logger::Write("OBS system=navmesh event=refused reason=navmesh_init_failed world=%s", world.c_str());
				if (g_nav) { dtFreeNavMesh(g_nav); g_nav = nullptr; }
				return;
			}
			g_query = dtAllocNavMeshQuery();
			if (!g_query || !dtStatusSucceed(g_query->init(g_nav, 4096))) { if (g_query) dtFreeNavMeshQuery(g_query); g_query = nullptr; }
		}

		// POD only (SEH).
		struct Player { bool ok = false; float x = 0, y = 0, z = 0; };
		Player ReadPlayer()
		{
			Player p;
			__try {
				const uptr pl = *reinterpret_cast<const uptr*>(game::Base() + addr::kPlayerCharacterPtr);
				if (pl) { p.x = *reinterpret_cast<const float*>(pl + off::kRefPosition); p.y = *reinterpret_cast<const float*>(pl + off::kRefPosition + 4);
					p.z = *reinterpret_cast<const float*>(pl + off::kRefPosition + 8); p.ok = true; }
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { p.ok = false; }
			return p;
		}
		struct CellAt { uptr cell = 0; std::uint32_t id = 0; };
		CellAt ReadCell(uptr c, int gx, int gy)
		{
			CellAt out;
			__try {
				if (!c || (*reinterpret_cast<const std::uint8_t*>(c + 0x48) & 1)) return out;  // exteriors only
				const std::int32_t* g = *reinterpret_cast<std::int32_t* const*>(c + 0x58);
				if (g && g[0] == gx && g[1] == gy) { out.cell = c; out.id = *reinterpret_cast<const std::uint32_t*>(c + off::kFormId); }
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { out.cell = 0; }
			return out;
		}
		CellAt AttachedCell(int gx, int gy)
		{
			const unsigned n = runtimecells::GridSize(64);
			for (unsigned i = 0; i < n; ++i) { const CellAt c = ReadCell(runtimecells::Attached(i), gx, gy); if (c.cell) return c; }
			return {};
		}
		// The job's neighbour cells that are attached now (the worker collects those whose edge band it does not have yet).
		std::uint8_t FillNeighbours(Job& j)
		{
			std::uint8_t m = 0;
			for (int i = 0; i < 8; ++i) {
				const Key nk = Near(j.k, i);
				const CellAt c = AttachedCell(nk.gx, nk.gy);
				j.nbCell[i] = c.cell; j.nbId[i] = c.id;
				if (c.cell) m |= std::uint8_t(1u << i);
			}
			return m;
		}
		// A cell's edge band changed: its neighbours' tiles are compared with the game again once next to the player.
		void RecheckNeighbours(Key k)
		{
			for (int i = 0; i < 8; ++i) {
				const Key nk = Near(k, i);
				if (!g_tiles.count(nk) || g_unverified.count(nk)) continue;
				const auto at = g_checkedAt.find(nk);
				if (at != g_checkedAt.end() && GetTickCount64() < at->second + kRecheckMs) {
					logger::Write("OBS system=navmesh event=recheck cell=%d,%d reason=neighbour_changed neighbour=%d,%d result=skipped_checked_within_60s", nk.gx, nk.gy, k.gx, k.gy);
					continue;
				}
				g_unverified.insert(nk); g_verifyWhy[nk] = "neighbour_changed";
				logger::Write("OBS system=navmesh event=recheck cell=%d,%d reason=neighbour_changed neighbour=%d,%d", nk.gx, nk.gy, k.gx, k.gy);
			}
		}
		void Dispatch(Job&& j, Key k)
		{
			// On the one engine thread (engineworker.h: a new thread per job used up the game's 64 thread numbers in the 12:04 run).
			g_busy = true;
			if (!engineworker::Post([job = std::move(j)]() mutable { Worker(std::move(job)); }, "navmesh")) {
				g_busy = false; g_want.erase(k); logger::Write("OBS system=navmesh event=refused cell=%d,%d reason=thread_start_failed", k.gx, k.gy);
			}
		}

		void Evict(const Player& pl)
		{
			while (g_tiles.size() > kKeepTiles) {
				const Key here = KeyOf(pl.x, pl.y);
				auto farthest = g_tiles.begin(); int farD = -1;
				for (auto it = g_tiles.begin(); it != g_tiles.end(); ++it) {
					const int d = Ring(it->first, here);
					if (d > farD) { farD = d; farthest = it; }
				}
				g_nav->removeTile(farthest->second, nullptr, nullptr);  // added with DT_TILE_FREE_DATA: Detour frees the data
				logger::Write("OBS system=navmesh event=evict cell=%d,%d cells_away=%d tiles=%zu", farthest->first.gx, farthest->first.gy, farD, g_tiles.size() - 1);
				const Key k = farthest->first;
				g_tiles.erase(farthest); g_nbMask.erase(k); g_nbTried.erase(k); g_unverified.erase(k); g_verifyWhy.erase(k); g_checkedAt.erase(k);
			}
		}
	}

	void Tick(bool loading)
	{
		if (loading) return;
		char w[256] = {};
		if (!FVO_WorldName(w, sizeof w)) return;  // interiors / no player: keep what there is
		const std::string world = w;
		if (world != g_world) {
			const std::size_t had = g_tiles.size();
			Reset(world);
			logger::Write("OBS system=navmesh event=world world=%s dropped_tiles=%zu cache=%s settings=%08X mesh=%d version=%u border_pad=%.2f", world.c_str(), had,
				Narrow(g_dir).c_str(), g_settings, int(g_nav != nullptr), kFileVersion, g_pad);
		}
		if (!g_nav) return;
		const ULONGLONG now = GetTickCount64();

		// Finished jobs into the mesh.
		std::vector<Done> done;
		{ std::lock_guard lk(g_doneLock); done.swap(g_done); }
		for (auto& d : done) {
			if (d.gen != g_gen) { if (d.data) dtFree(d.data); continue; }
			if (d.bandChanged) RecheckNeighbours(d.k);
			if (d.replace) {
				// A verify job: unchanged (keep the tile), rebaked (swap it), or not decided (try again later).
				if (d.matched) {
					g_unverified.erase(d.k); g_verifyWhy.erase(d.k);
					if (g_tiles.count(d.k)) g_nbMask[d.k] = d.nbMask;
					logger::Write("OBS system=navmesh event=verified cell=%d,%d result=unchanged fingerprint=%016llX input_ms=%.0f neighbours=%02X nb_collected=%d nb_cached=%d nb_ms=%.0f",
						d.k.gx, d.k.gy, (unsigned long long)d.fpNew, d.msInput, d.nbMask, d.nbCollected, d.nbCached, d.msNb);
					continue;
				}
				if (!d.data) {
					g_retryAt[d.k] = now + kRetryMs;
					logger::Write("OBS system=navmesh event=verified cell=%d,%d result=undecided reason=%s", d.k.gx, d.k.gy, d.why);
					continue;
				}
				const auto old = g_tiles.find(d.k);
				if (old != g_tiles.end()) { g_nav->removeTile(old->second, nullptr, nullptr); g_tiles.erase(old); }
				g_unverified.erase(d.k); g_verifyWhy.erase(d.k);
			}
			g_want.erase(d.k);
			if (!d.data) {
				const bool old = std::strcmp(d.why, "cache_old_deleted") == 0;
				if (old) g_retryAt.erase(d.k); else g_retryAt[d.k] = now + kRetryMs;
				logger::Write("OBS system=navmesh event=refused cell=%d,%d reason=%s retry_s=%llu", d.k.gx, d.k.gy, d.why, (unsigned long long)(old ? 0 : kRetryMs / 1000));
				continue;
			}
			dtTileRef ref = 0;
			const dtStatus st = g_nav->addTile(d.data, d.size, DT_TILE_FREE_DATA, 0, &ref);
			if (!dtStatusSucceed(st)) {
				dtFree(d.data);
				g_retryAt[d.k] = now + kRetryMs;
				logger::Write("OBS system=navmesh event=refused cell=%d,%d reason=add_tile_failed status=%08X", d.k.gx, d.k.gy, unsigned(st));
				continue;
			}
			g_tiles[d.k] = ref; g_nbMask[d.k] = d.nbMask;
			if (!d.verified) { g_unverified.insert(d.k); g_verifyWhy[d.k] = "cached_tile_loaded_while_far"; }
			const dtMeshTile* t = g_nav->getTileByRef(ref);
			logger::Write("OBS system=navmesh event=tile cell=%d,%d source=%s verified=%d polys=%d bytes=%d input_tris=%d nb_tris=%d input_ms=%.0f nb_ms=%.0f bake_ms=%.0f tiles=%zu "
				"neighbours=%02X nb_collected=%d nb_cached=%d input_z=%.0f..%.0f fingerprint_old=%016llX fingerprint_new=%016llX why=%s",
				d.k.gx, d.k.gy, d.cached ? "cache" : "bake", int(d.verified), t && t->header ? t->header->polyCount : d.polys, d.size, d.inputTris, d.nbTris, d.msInput, d.msNb, d.msBake,
				g_tiles.size(), d.nbMask, d.nbCollected, d.nbCached, d.zmin, d.zmax, (unsigned long long)d.fpOld, (unsigned long long)d.fpNew, d.why);
		}

		const Player pl = ReadPlayer();
		if (!pl.ok) return;
		Evict(pl);

		// The next job to the worker, one at a time: a capture (test builds), a tile to check against the game, the next wanted cell.
		if (g_busy) return;
		const Key here = KeyOf(pl.x, pl.y);
		if (g_captureWanted) {
			g_captureWanted = false;
			const CellAt c = navprobe::EngineReady() ? AttachedCell(g_captureKey.gx, g_captureKey.gy) : CellAt{};
			if (c.cell) {
				Job j; j.gen = g_gen; j.k = g_captureKey; j.run = ++g_runs; j.settings = g_settings; j.capture = true; j.cell = c.cell; j.cellId = c.id; j.dir = g_dir;
				for (int i = 0; i < 3; ++i) j.car[i] = g_captureCar[i];
				j.captureWhy = g_captureWhy;
				const std::uint8_t nb = FillNeighbours(j);
				++g_captures;
				logger::Write("OBS system=navmesh event=capture_start cell=%d,%d run=%u game_cell=%08X car=%.1f,%.1f,%.1f neighbours_loaded=%02X captures=%d", j.k.gx, j.k.gy, j.run, j.cellId,
					j.car[0], j.car[1], j.car[2], nb, g_captures);
				Dispatch(std::move(j), g_captureKey);
				return;
			}
			logger::Write("OBS system=navmesh event=capture cell=%d,%d result=refused reason=%s", g_captureKey.gx, g_captureKey.gy, navprobe::EngineReady() ? "cell_not_attached" : "engine_signatures");
		}
		// A tile next to the player baked or checked before some neighbour was available: check it again once that neighbour is (its
		// edge band kept, or its cell attached), so a road the neighbour holds reaches this tile's edge too. Once per neighbour per world.
		if (now >= g_nbCheckAt) {
			g_nbCheckAt = now + 500;
			for (const auto& [k, mask] : g_nbMask) {
				if (mask == 0xFF || g_unverified.count(k) || Ring(k, here) > 1) continue;
				std::uint8_t& tried = g_nbTried[k];
				std::uint8_t found = 0;
				for (int i = 0; i < 8; ++i) {
					const std::uint8_t bit = std::uint8_t(1u << i);
					if ((mask | tried) & bit) continue;
					const Key nk = Near(k, i);
					if (FindBand(g_gen, nk) || AttachedCell(nk.gx, nk.gy).cell) found |= bit;
				}
				if (found) { tried |= found; g_unverified.insert(k); g_verifyWhy[k] = "neighbour_loaded"; }
			}
		}
		// A tile to compare with the game's input (loaded from the cache while far, or a neighbour loaded or changed): rebaked if it
		// differs.
		for (auto it = g_unverified.begin(); it != g_unverified.end(); ++it) {
			const Key k = *it;
			if (Ring(k, here) > 1) continue;
			const auto r = g_retryAt.find(k);
			if (r != g_retryAt.end() && now < r->second) continue;
			if (!navprobe::EngineReady()) break;
			const CellAt c = AttachedCell(k.gx, k.gy);
			if (!c.cell) continue;
			Job j; j.gen = g_gen; j.k = k; j.path = TilePath(g_dir, k); j.settings = g_settings; j.run = ++g_runs; j.fromFile = true; j.verifyOnly = true;
			j.cell = c.cell; j.cellId = c.id;
			const std::uint8_t nb = FillNeighbours(j);
			g_checkedAt[k] = now;
			const auto why = g_verifyWhy.find(k);
			logger::Write("OBS system=navmesh event=verify cell=%d,%d run=%u game_cell=%08X player_cell=%d,%d neighbours_loaded=%02X reason=%s", k.gx, k.gy, j.run, j.cellId, here.gx, here.gy, nb,
				why != g_verifyWhy.end() ? why->second : "cached_tile_loaded_while_far");
			Dispatch(std::move(j), k);
			return;
		}
		if (g_order.empty()) return;
		for (std::size_t i = 0; i < g_order.size(); ++i) {
			const Key k = g_order[i];
			if (!g_want.count(k) || g_tiles.count(k)) { g_order.erase(g_order.begin() + std::ptrdiff_t(i)); --i; continue; }
			Job j; j.gen = g_gen; j.k = k; j.path = TilePath(g_dir, k); j.settings = g_settings; j.run = ++g_runs;
			j.fromFile = Exists(j.path);
			// Baked within one cell of the player, or two for a cell ahead on the route (it stays loaded while the car drives toward it),
			// so the tile is ready 100-200 m before the car gets there.
			const int ring = Ring(k, here);
			const auto whyIt = g_wantWhy.find(k);
			const bool near1 = ring <= 1 || (ring <= 2 && whyIt != g_wantWhy.end() && std::strcmp(whyIt->second, "route_ahead") == 0);
			std::uint8_t nb = 0;
			if (!j.fromFile) {
				if (!near1) continue;                         // wait until the player is next to it
				if (!navprobe::EngineReady()) { g_want.erase(k); logger::Write("OBS system=navmesh event=refused cell=%d,%d reason=engine_signatures", k.gx, k.gy); continue; }
				const CellAt c = AttachedCell(k.gx, k.gy);
				if (!c.cell) {
					if (now >= g_waitLogAt) { g_waitLogAt = now + 5000; logger::Write("OBS system=navmesh event=wait cell=%d,%d reason=cell_not_attached_yet", k.gx, k.gy); }
					continue;
				}
				j.cell = c.cell; j.cellId = c.id; nb = FillNeighbours(j);
			} else if (near1 && navprobe::EngineReady()) {
				const CellAt c = AttachedCell(k.gx, k.gy);  // near: the cached tile is checked against the game's input (rebaked if it changed)
				j.cell = c.cell; j.cellId = c.id;
				if (c.cell) nb = FillNeighbours(j);
			}
			g_order.erase(g_order.begin() + std::ptrdiff_t(i));
			logger::Write("OBS system=navmesh event=%s cell=%d,%d run=%u game_cell=%08X player_cell=%d,%d neighbours_loaded=%02X reason=%s", j.fromFile ? "cache" : "bake", k.gx, k.gy, j.run,
				j.cellId, here.gx, here.gy, nb, g_wantWhy.count(k) ? g_wantWhy[k] : "-");
			Dispatch(std::move(j), k);
			return;
		}
	}

	void Want(double x, double y, const char* reason)
	{
		if (!g_nav || !std::isfinite(x) || !std::isfinite(y)) return;
		const Key k = KeyOf(x, y);
		if (g_tiles.count(k) || g_want.count(k)) return;
		const auto r = g_retryAt.find(k);
		if (r != g_retryAt.end() && GetTickCount64() < r->second) return;
		if (g_want.size() >= kMaxWanted) return;
		g_want.insert(k); g_order.push_back(k); g_wantWhy[k] = reason;
		logger::Write("OBS system=navmesh event=want cell=%d,%d reason=%s wanted=%zu tiles=%zu", k.gx, k.gy, reason, g_want.size(), g_tiles.size());
	}

	bool Has(double x, double y) { return g_tiles.count(KeyOf(x, y)) != 0; }
	int Tiles() { return int(g_tiles.size()); }

	double Clearance(double x, double y, double z)
	{
		if (!g_query || !Has(x, y)) return -1;
		// Cached per 0.25 m and 1 m of height for a second: the planner asks ~2,000 spots a plan, many of them again next plan.
		static std::map<std::uint64_t, float> cache;
		static ULONGLONG cacheAt = 0;
		static unsigned cacheGen = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - cacheAt > 1000 || cacheGen != g_gen) { cache.clear(); cacheAt = now; cacheGen = g_gen; }
		const std::uint64_t key = (std::uint64_t(std::int64_t(std::floor(x * 4)) & 0x1FFFFF) << 42) ^ (std::uint64_t(std::int64_t(std::floor(y * 4)) & 0x1FFFFF) << 21)
			^ std::uint64_t(std::int64_t(std::floor(z)) & 0x1FFFFF);
		const auto hit = cache.find(key);
		if (hit != cache.end()) return hit->second;
		const float s[3] = { float(x), float(y), float(z) };
		float p[3], np[3];
		navbake::ToRecast(s, p);
		const float ext[3] = { 2.0f, 1.5f, 2.0f };
		dtQueryFilter f; f.setIncludeFlags(0xffff); f.setExcludeFlags(0);
		dtPolyRef ref = 0;
		float e = 9.0f;
		if (dtStatusSucceed(g_query->findNearestPoly(p, ext, &f, &ref, np)) && ref) e = std::hypot(np[0] - p[0], np[2] - p[2]);
		cache[key] = e;
		return e;
	}

	int Deck(double x, double y, double z, double& deck)
	{
		struct Side { dtNavMesh* mesh = nullptr; dtNavMeshQuery* query = nullptr; ULONGLONG missingAt = 0; };
		static std::map<Key, Side> sides;
		static std::string sidesWorld;
		const auto clear = [] { for (auto& [k, s] : sides) { if (s.query) dtFreeNavMeshQuery(s.query); if (s.mesh) dtFreeNavMesh(s.mesh); } sides.clear(); };
		char w[256] = {};
		if (!FVO_WorldName(w, sizeof w) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return 0;
		if (sidesWorld != w) { clear(); sidesWorld = w; }
		if (sides.size() >= 64) clear();
		const Key k = KeyOf(x, y);
		Side& s = sides[k];
		const ULONGLONG now = GetTickCount64();
		if (!s.query) {
			if (s.missingAt && now - s.missingAt < 30000) return 0;
			FileHead h; unsigned char* data = nullptr; int size = 0;
			const std::wstring dir = fvopaths::Dir() + L"\\navcache\\" + Slug(sidesWorld);
			const char* why = ReadTile(TilePath(dir, k), k, g_settings, h, &data, &size);
			if (*why || !data) {
				s.missingAt = now;
				logger::Write("OBS system=navmesh event=deck_no_tile cell=%d,%d reason=%s", k.gx, k.gy, why);
				return 0;
			}
			s.mesh = dtAllocNavMesh();
			s.query = dtAllocNavMeshQuery();
			// The same tiled layout as the main mesh: a single-tile init keeps the tile's own grid position, so no query ever found it (15:22 run: 135 spots "no deck").
			const dtNavMeshParams np = navbake::TiledParams(kCell);
			bool added = false;
			if (s.mesh && s.query && dtStatusSucceed(s.mesh->init(&np))) added = dtStatusSucceed(s.mesh->addTile(data, size, DT_TILE_FREE_DATA, 0, nullptr));
			if (!added || !dtStatusSucceed(s.query->init(s.mesh, 256))) {
				if (!added) dtFree(data);
				if (s.query) dtFreeNavMeshQuery(s.query);
				if (s.mesh) dtFreeNavMesh(s.mesh);
				s = Side{}; s.missingAt = now;
				logger::Write("OBS system=navmesh event=deck_no_tile cell=%d,%d reason=tile_init_failed", k.gx, k.gy);
				return 0;
			}
		}
		const float at[3] = { float(x), float(y), float(z) };
		float p[3], near_[3];
		navbake::ToRecast(at, p);
		const float ext[3] = { 1.0f, 5.0f, 1.0f };
		dtQueryFilter f; f.setIncludeFlags(0xffff); f.setExcludeFlags(0);
		dtPolyRef ref = 0;
		if (!dtStatusSucceed(s.query->findNearestPoly(p, ext, &f, &ref, near_)) || !ref) return -1;
		deck = near_[1];
		return 1;
	}

	void Capture(double x, double y, double z, const char* reason)
	{
#ifdef FVO_RELEASE
		(void)x; (void)y; (void)z; (void)reason;
#else
		if (!g_nav || g_captureWanted || g_captures >= kMaxCaptures || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return;
		const Key k = KeyOf(x, y);
		if (g_captured.count(k) || !g_tiles.count(k)) return;
		g_captured.insert(k); g_captureWanted = true; g_captureKey = k; g_captureWhy = reason ? reason : "-";
		g_captureCar[0] = float(x); g_captureCar[1] = float(y); g_captureCar[2] = float(z);
		logger::Write("OBS system=navmesh event=capture_wanted cell=%d,%d car=%.1f,%.1f,%.1f reason=%s", k.gx, k.gy, x, y, z, g_captureWhy);
#endif
	}

	void Rebake(const char* reason)
	{
		if (g_world.empty()) return;
		unsigned removed = 0;
		WIN32_FIND_DATAW fd;
		const HANDLE h = FindFirstFileW((g_dir + L"\\*.tile").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do { if (DeleteFileW((g_dir + L"\\" + fd.cFileName).c_str())) ++removed; } while (FindNextFileW(h, &fd));
			FindClose(h);
		}
		const std::string world = g_world;
		Reset(world);
		logger::Write("OBS system=navmesh event=rebake world=%s files_removed=%u reason=%s", world.c_str(), removed, reason);
	}

	Route Find(const float from[3], const float to[3], float snap)
	{
		Route r;
		if (!g_query) { r.why = "no_mesh"; return r; }
		if (!Has(from[0], from[1])) { r.why = "no_tile_at_start"; return r; }
		if (!Has(to[0], to[1])) { r.why = "no_tile_at_end"; return r; }
		std::vector<float> corners;
		const navbake::Path p = navbake::QueryPath(g_query, from, to, snap, &corners);
		r.found = p.found; r.complete = p.complete; r.polys = p.polys; r.length = p.length; r.startSnap = p.startSnap; r.endSnap = p.endSnap;
		r.corners = std::min(int(corners.size() / 3), 32);
		for (int i = 0; i < r.corners; ++i) for (int k = 0; k < 3; ++k) r.pts[i][k] = corners[std::size_t(i) * 3 + k];
		r.why = p.complete ? "complete" : p.found ? "partial" : p.startSnap < 0 ? "start_off_mesh" : p.endSnap < 0 ? "end_off_mesh" : "no_path";
		return r;
	}
}

// Core export for FVO_Traffic.dll (optional, read with GetProcAddress): see navmesh::Want.
extern "C" __declspec(dllexport) void FVO_NavWantV1(double x, double y) { navmesh::Want(x, y, "spawn_spot"); }
