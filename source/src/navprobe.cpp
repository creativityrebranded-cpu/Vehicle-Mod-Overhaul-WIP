#include "navprobe.h"

#include "addresses.h"
#include "controlmap.h"
#include "crash.h"
#include "engineworker.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "navbake.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace navprobe
{
	namespace
	{
		using uptr = std::uintptr_t;

		// First 16 bytes of each engine function the probe calls (read from Starfield.exe 1.16.244). A mismatch disables the probe.
		struct Sig { const char* name; std::uint32_t rva; std::uint8_t bytes[16]; };
		constexpr Sig kSigs[] = {
			{ "settings init FUN_140698480", 0x00698480, { 0x48, 0x83, 0xEC, 0x28, 0x66, 0xC7, 0x01, 0x00, 0x00, 0x45, 0x33, 0xD2, 0x44, 0x89, 0x51, 0x0C } },
			{ "settings fill FUN_1406986A0", 0x006986A0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48 } },
			{ "settings free FUN_140698630", 0x00698630, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0x89 } },
			{ "cell entry FUN_1406ACB50", 0x006ACB50, { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x41 } },
			{ "cell context FUN_1426EDE70", 0x026EDE70, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x66, 0xC7, 0x41, 0x08, 0x01, 0x01, 0x48, 0x8D, 0x05, 0xE5 } },
			{ "input step FUN_1406AD0F0", 0x006AD0F0, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 } },
			{ "data destructor FUN_14039E730", 0x0039E730, { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0xDA, 0x48, 0x8B, 0xF9, 0x48 } },
		};
		enum { kCfgInit, kCfgFill, kCfgFree, kCellEntry, kCtxCtor, kInputStep, kDataDtor, kSigCount };
		constexpr std::uint32_t kDataVtable = 0x04B053F8;       // NavMeshGenData::vftable, slot 0 = FUN_14039E730
		constexpr std::uint32_t kInputProcVtable = 0x04B371D8;  // NavMeshGenInputProcessor::vftable, slot 1 = FUN_1406AD3F0
		constexpr std::uint32_t kInputProcSlot1 = 0x006AD3F0;
		constexpr std::uint32_t kBufVerts = 0x00, kBufTris = 0x10, kBufThird = 0x20, kBufBounds = 0x7DC8, kBufClip = 0x7DE0;  // input buffer (0x7E18)
		constexpr std::uint32_t kDataBuffer = 0x28;              // NavMeshGenData +0x28: the input buffer
		constexpr std::uint32_t kMaxVerts = 8000000, kMaxTris = 8000000;

		// The objects the probe owns and hands to the engine (zeroed). Sizes have margin over what the engine writes.
		struct Raw
		{
			alignas(16) std::uint8_t cfg[0x400];     // settings (the engine uses ~0x1C0)
			alignas(16) std::uint8_t entry[0x40];    // cell entry (0x3C)
			alignas(16) std::uint8_t data[0x80];     // NavMeshGenData (0x78)
			alignas(16) std::uint8_t ctx[0x100];     // BSNavMeshGenContext (0xE0, base class)
			alignas(16) std::uint8_t parent[0x20];   // the constructor reads one byte at +1
			alignas(16) std::uint8_t proc[0x10];     // NavMeshGenInputProcessor (vtable only)
			std::uint32_t p5[4];
			std::int32_t sub[2];
		};
		struct Fault { DWORD code = 0; uptr at = 0; };
		struct Counts { uptr buf = 0, verts = 0, tris = 0; std::uint32_t nv = 0, nt = 0, n3 = 0; float bounds[6] = {}, clip[6] = {}; };
		struct Job
		{
			uptr cell = 0; std::uint32_t cellId = 0, worldId = 0; bool exterior = false; int gx = 0, gy = 0; bool gridOk = false;
			float px = 0, py = 0, pz = 0; std::wstring dir; int key = 0;
		};

		std::atomic<bool> g_busy{ false };
		int g_key = 0, g_claimTries = 0;
		bool g_down = false, g_sigChecked = false, g_sigOk = false;
		ULONGLONG g_claimAt = 0;
		unsigned g_runs = 0;

		int Filter(EXCEPTION_POINTERS* ep, Fault* f)
		{
			f->code = ep->ExceptionRecord->ExceptionCode;
			f->at = reinterpret_cast<uptr>(ep->ExceptionRecord->ExceptionAddress);
			return EXCEPTION_EXECUTE_HANDLER;
		}
		uptr Off(uptr a) { const uptr b = game::Base(); return a >= b ? a - b : a; }
		double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b)
		{
			LARGE_INTEGER f; QueryPerformanceFrequency(&f);
			return 1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart);
		}

		// Signatures and the two vtables we place in our objects; logs each mismatch. Main thread, once.
		bool CheckSignatures()
		{
			const uptr base = game::Base();
			bool ok = base != 0;
			for (int i = 0; i < kSigCount && ok; ++i) {
				bool same = false;
				__try { same = std::memcmp(reinterpret_cast<const void*>(base + kSigs[i].rva), kSigs[i].bytes, 16) == 0; }
				__except (EXCEPTION_EXECUTE_HANDLER) { same = false; }
				if (!same) { logger::Write("OBS system=navprobe event=disabled reason=signature_mismatch function=\"%s\" rva=%X", kSigs[i].name, kSigs[i].rva); ok = false; }
			}
			if (ok) {
				uptr dataSlot0 = 0, procSlot1 = 0;
				__try { dataSlot0 = *reinterpret_cast<const uptr*>(base + kDataVtable); procSlot1 = *reinterpret_cast<const uptr*>(base + kInputProcVtable + 8); }
				__except (EXCEPTION_EXECUTE_HANDLER) {}
				if (dataSlot0 != base + kSigs[kDataDtor].rva || procSlot1 != base + kInputProcSlot1) {
					logger::Write("OBS system=navprobe event=disabled reason=vtable_mismatch data_slot0=+%llX proc_slot1=+%llX", (unsigned long long)Off(dataSlot0), (unsigned long long)Off(procSlot1));
					ok = false;
				}
			}
			if (ok) logger::Write("OBS system=navprobe event=signatures result=ok functions=%d vtables=2", kSigCount);
			return ok;
		}

		// The engine steps. No C++ objects with destructors in here (SEH). stage: last step started.
		bool RunSteps(uptr base, uptr cell, Raw* r, Fault* f, int* stage, std::uint64_t* entryRet, std::uint64_t* inputRet, double* msPrep, double* msInput)
		{
			using Fn1 = void* (*)(void*);
			using EntryFn = std::uint64_t (*)(uptr, std::int32_t*, std::uint32_t*);
			using CtorFn = void* (*)(void*, std::intptr_t, std::uint32_t*, void**, std::uint32_t*);
			using InputFn = std::uint64_t (*)(void*, void*, void*);
			LARGE_INTEGER t0, t1, t2;
			QueryPerformanceCounter(&t0);
			__try {
				*stage = 1;
				reinterpret_cast<Fn1>(base + kSigs[kCfgInit].rva)(r->cfg);
				reinterpret_cast<Fn1>(base + kSigs[kCfgFill].rva)(r->cfg);
				*stage = 2;
				*entryRet = reinterpret_cast<EntryFn>(base + kSigs[kCellEntry].rva)(cell, r->sub, reinterpret_cast<std::uint32_t*>(r->entry));
				*stage = 3;
				*reinterpret_cast<uptr*>(r->data) = base + kDataVtable;
				*reinterpret_cast<std::int32_t*>(r->data + 8) = 1;
				void* dataPtr = r->data;
				reinterpret_cast<CtorFn>(base + kSigs[kCtxCtor].rva)(r->ctx, reinterpret_cast<std::intptr_t>(r->parent), reinterpret_cast<std::uint32_t*>(r->entry), &dataPtr, r->p5);
				*reinterpret_cast<uptr*>(r->proc) = base + kInputProcVtable;
				QueryPerformanceCounter(&t1);
				*stage = 4;
				*inputRet = reinterpret_cast<InputFn>(base + kSigs[kInputStep].rva)(r->proc, r->cfg, r->ctx);
				QueryPerformanceCounter(&t2);
				*msPrep = Ms(t0, t1); *msInput = Ms(t1, t2);
				*stage = 5;
				return true;
			}
			__except (Filter(GetExceptionInformation(), f)) { return false; }
		}
		bool ReadCounts(const Raw* r, Counts* c, Fault* f)
		{
			__try {
				c->buf = *reinterpret_cast<const uptr*>(r->data + kDataBuffer);
				if (!c->buf) return true;
				c->nv = *reinterpret_cast<const std::uint32_t*>(c->buf + kBufVerts);
				c->verts = *reinterpret_cast<const uptr*>(c->buf + kBufVerts + 8);
				c->nt = *reinterpret_cast<const std::uint32_t*>(c->buf + kBufTris);
				c->tris = *reinterpret_cast<const uptr*>(c->buf + kBufTris + 8);
				c->n3 = *reinterpret_cast<const std::uint32_t*>(c->buf + kBufThird);
				std::memcpy(c->bounds, reinterpret_cast<const void*>(c->buf + kBufBounds), sizeof c->bounds);
				std::memcpy(c->clip, reinterpret_cast<const void*>(c->buf + kBufClip), sizeof c->clip);
				return true;
			}
			__except (Filter(GetExceptionInformation(), f)) { return false; }
		}
		bool CopyArrays(const Counts* c, float* v, std::uint32_t* t, Fault* f)
		{
			__try {
				if (c->nv) std::memcpy(v, reinterpret_cast<const void*>(c->verts), std::size_t(c->nv) * 12);
				if (c->nt) std::memcpy(t, reinterpret_cast<const void*>(c->tris), std::size_t(c->nt) * 24);
				return true;
			}
			__except (Filter(GetExceptionInformation(), f)) { return false; }
		}
		bool Cleanup(uptr base, Raw* r, bool dataBuilt, Fault* f)
		{
			using DtorFn = void* (*)(void*, std::uint64_t);
			using Fn1 = void (*)(void*);
			__try {
				if (dataBuilt) reinterpret_cast<DtorFn>(base + kSigs[kDataDtor].rva)(r->data, 0);  // 0: do not free our memory
				reinterpret_cast<Fn1>(base + kSigs[kCfgFree].rva)(r->cfg);
				return true;
			}
			__except (Filter(GetExceptionInformation(), f)) { return false; }
		}

		const char* StageName(int s)
		{
			switch (s) { case 1: return "settings"; case 2: return "cell_entry"; case 3: return "cell_context"; case 4: return "input_step"; case 5: return "done"; default: return "none"; }
		}

		std::string Narrow(const std::wstring& w)
		{
			std::string s; s.reserve(w.size());
			for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?');
			return s;
		}

		// Top distinct values of one dword of the triangles' extra 12 bytes, as "value:count" pairs.
		std::string TopValues(const std::vector<std::uint32_t>& t, int dword, std::size_t nt)
		{
			std::map<std::uint32_t, std::uint32_t> m;
			for (std::size_t i = 0; i < nt; ++i) ++m[t[i * 6 + dword]];
			std::vector<std::pair<std::uint32_t, std::uint32_t>> v(m.begin(), m.end());
			std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
			std::string s; char b[48];
			for (std::size_t i = 0; i < v.size() && i < 8; ++i) { std::snprintf(b, sizeof b, "%s%X:%u", i ? "," : "", v[i].first, v[i].second); s += b; }
			if (v.size() > 8) { std::snprintf(b, sizeof b, ",+%zu more", v.size() - 8); s += b; }
			return s.empty() ? "-" : s;
		}

		// The game's navmesh input step for one cell into v (12-byte vertices) and t (24-byte triangles). Any thread (the game runs it on
		// its generator threads); signatures checked by the caller. sys/run tag the log lines (navprobe or navmesh).
		void CollectInto(const char* sys, unsigned run, uptr cell, std::uint32_t cellId, Counts& c, std::vector<float>& v, std::vector<std::uint32_t>& t,
			bool& copied, int& stage, std::uint64_t& inputRet, double& msInput)
		{
			const uptr base = game::Base();
			Raw* r = new Raw{};
			Fault f;
			std::uint64_t entryRet = 0;
			double msPrep = 0;
			const bool ran = RunSteps(base, cell, r, &f, &stage, &entryRet, &inputRet, &msPrep, &msInput);
			const std::uint32_t* e = reinterpret_cast<const std::uint32_t*>(r->entry);
			const float* ef = reinterpret_cast<const float*>(r->entry);
			logger::Write("OBS system=%s event=steps run=%u cell=%08X ok=%d stage=%s entry_ret=%llX input_ret=%llu prep_ms=%.1f input_ms=%.1f "
				"entry_world=%08X entry_cell=%08X entry_grid=%d,%d entry_sub=%d,%d entry_flags=%02X,%02X entry_f=(%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f)",
				sys, run, cellId, int(ran), StageName(stage), (unsigned long long)entryRet, (unsigned long long)inputRet, msPrep, msInput,
				e[0], e[1], int(e[2]), int(e[3]), int(e[4]), int(e[5]), r->entry[0x18], r->entry[0x19], ef[7], ef[8], ef[9], ef[10], ef[11], ef[12], ef[13], ef[14]);
			if (!ran)
				logger::Write("OBS system=%s event=fault run=%u cell=%08X stage=%s code=%08lX at=Starfield.exe+%llX", sys, run, cellId, StageName(stage), f.code, (unsigned long long)Off(f.at));

			if (stage >= 4) {
				Fault rf;
				if (!ReadCounts(r, &c, &rf))
					logger::Write("OBS system=%s event=fault run=%u stage=read_counts code=%08lX at=Starfield.exe+%llX", sys, run, rf.code, (unsigned long long)Off(rf.at));
				else if (!c.buf)
					logger::Write("OBS system=%s event=empty run=%u cell=%08X reason=no_input_buffer input_ret=%llu", sys, run, cellId, (unsigned long long)inputRet);
				else if (c.nv > kMaxVerts || c.nt > kMaxTris || (c.nv && !c.verts) || (c.nt && !c.tris))
					logger::Write("OBS system=%s event=refused run=%u reason=implausible_arrays verts=%u tris=%u vptr=%llX tptr=%llX", sys, run, c.nv, c.nt,
						(unsigned long long)c.verts, (unsigned long long)c.tris);
				else {
					try { v.resize(std::size_t(c.nv) * 3); t.resize(std::size_t(c.nt) * 6); }
					catch (...) { v.clear(); t.clear(); logger::Write("OBS system=%s event=refused run=%u reason=out_of_memory verts=%u tris=%u", sys, run, c.nv, c.nt); }
					if (v.size() == std::size_t(c.nv) * 3 && t.size() == std::size_t(c.nt) * 6) {
						Fault cf;
						copied = CopyArrays(&c, v.data(), t.data(), &cf);
						if (!copied) logger::Write("OBS system=%s event=fault run=%u stage=copy code=%08lX at=Starfield.exe+%llX", sys, run, cf.code, (unsigned long long)Off(cf.at));
					}
				}
			}
			Fault cl;
			const bool cleaned = stage >= 1 && Cleanup(base, r, stage >= 3, &cl);
			if (stage >= 1 && !cleaned)
				logger::Write("OBS system=%s event=fault run=%u stage=cleanup code=%08lX at=Starfield.exe+%llX (probe memory kept)", sys, run, cl.code, (unsigned long long)Off(cl.at));
			if (cleaned || stage == 0) delete r;

		}

		void Worker(Job j)
		{
			crash::SetExpectedFaults(true);
			crash::Crumb("navprobe begin", int(j.cellId));
			Counts c;
			std::vector<float> v;
			std::vector<std::uint32_t> t;
			bool copied = false;
			int stage = 0;
			std::uint64_t inputRet = 0;
			double msInput = 0, msPrep = 0;
			CollectInto("navprobe", g_runs, j.cell, j.cellId, c, v, t, copied, stage, inputRet, msInput);
			if (copied) {
				// What came back: index sanity, vertex bounds, triangles near the player, the extra data's values.
				std::uint32_t bad = 0, near15 = 0, degenerate = 0;
				float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
				for (std::size_t i = 0; i < c.nv; ++i) for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], v[i * 3 + k]); mx[k] = std::max(mx[k], v[i * 3 + k]); }
				for (std::size_t i = 0; i < c.nt; ++i) {
					const std::uint32_t a = t[i * 6], b = t[i * 6 + 1], d = t[i * 6 + 2];
					if (a >= c.nv || b >= c.nv || d >= c.nv) { ++bad; continue; }
					if (a == b || b == d || a == d) ++degenerate;
					const float cx = (v[a * 3] + v[b * 3] + v[d * 3]) / 3, cy = (v[a * 3 + 1] + v[b * 3 + 1] + v[d * 3 + 1]) / 3;
					if (std::hypot(cx - j.px, cy - j.py) < 15.0f) ++near15;
				}
				const std::string d3 = TopValues(t, 3, c.nt), d4 = TopValues(t, 4, c.nt), d5 = TopValues(t, 5, c.nt);
				wchar_t name[96];
				swprintf_s(name, L"cell_%08X_%llu", j.cellId, (unsigned long long)GetTickCount64());
				const std::wstring obj = j.dir + L"\\" + name + L".obj", txt = j.dir + L"\\" + name + L".txt";
				bool wrote = false;
				if (FILE* o = _wfopen(obj.c_str(), L"wb")) {
					std::fprintf(o, "# FVO navmesh input probe: the game's own navmesh input for one cell (Starfield coordinates, metres, Z up)\n");
					std::fprintf(o, "# cell %08X world %08X exterior %d grid %d,%d player %.2f %.2f %.2f\n# vertices %u triangles %u bad %u\n",
						j.cellId, j.worldId, int(j.exterior), j.gx, j.gy, j.px, j.py, j.pz, c.nv, c.nt, bad);
					for (std::size_t i = 0; i < c.nv; ++i) std::fprintf(o, "v %.3f %.3f %.3f\n", v[i * 3], v[i * 3 + 1], v[i * 3 + 2]);
					// Grouped by the triangles' first extra dword (likely area / material), so a viewer can show each kind on its own.
					std::map<std::uint32_t, std::vector<std::size_t>> groups;
					for (std::size_t i = 0; i < c.nt; ++i) {
						const std::uint32_t a = t[i * 6], b = t[i * 6 + 1], d = t[i * 6 + 2];
						if (a < c.nv && b < c.nv && d < c.nv) groups[t[i * 6 + 3]].push_back(i);
					}
					for (const auto& [key, list] : groups) {
						std::fprintf(o, "g extra_%08X\n", key);
						for (std::size_t i : list) std::fprintf(o, "f %u %u %u\n", t[i * 6] + 1, t[i * 6 + 1] + 1, t[i * 6 + 2] + 1);
					}
					wrote = std::fclose(o) == 0;
				}
				if (FILE* s = _wfopen(txt.c_str(), L"wb")) {
					std::fprintf(s, "cell %08X world %08X exterior %d grid %d,%d\nplayer %.2f %.2f %.2f\n", j.cellId, j.worldId, int(j.exterior), j.gx, j.gy, j.px, j.py, j.pz);
					std::fprintf(s, "input_ret %llu prep_ms %.1f input_ms %.1f\nvertices %u triangles %u third_array %u bad_index %u degenerate %u within_15m %u\n",
						(unsigned long long)inputRet, msPrep, msInput, c.nv, c.nt, c.n3, bad, degenerate, near15);
					std::fprintf(s, "vertex bounds %.2f %.2f %.2f .. %.2f %.2f %.2f\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
					std::fprintf(s, "buffer bounds %.2f %.2f %.2f / %.2f %.2f %.2f\nbuffer clip %.2f %.2f %.2f / %.2f %.2f %.2f\n",
						c.bounds[0], c.bounds[1], c.bounds[2], c.bounds[3], c.bounds[4], c.bounds[5], c.clip[0], c.clip[1], c.clip[2], c.clip[3], c.clip[4], c.clip[5]);
					std::fprintf(s, "triangle extra dword 3 (value:count) %s\ndword 4 %s\ndword 5 %s\n", d3.c_str(), d4.c_str(), d5.c_str());
					std::fprintf(s, "first triangles (a b c | extra as hex and as float):\n");
					for (std::size_t i = 0; i < c.nt && i < 12; ++i) {
						float ff[3]; std::memcpy(ff, &t[i * 6 + 3], 12);
						std::fprintf(s, "  %u %u %u | %08X %08X %08X | %g %g %g\n", t[i * 6], t[i * 6 + 1], t[i * 6 + 2], t[i * 6 + 3], t[i * 6 + 4], t[i * 6 + 5], ff[0], ff[1], ff[2]);
					}
					std::fclose(s);
				}
				logger::Write("OBS system=navprobe event=done run=%u cell=%08X world=%08X exterior=%d grid=%d,%d verts=%u tris=%u third=%u bad_index=%u degenerate=%u within_15m=%u "
					"input_ms=%.1f bounds=(%.1f %.1f %.1f)..(%.1f %.1f %.1f) extra3=%s extra4=%s extra5=%s file=%s wrote=%d",
					g_runs, j.cellId, j.worldId, int(j.exterior), j.gx, j.gy, c.nv, c.nt, c.n3, bad, degenerate, near15, msInput, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
					d3.c_str(), d4.c_str(), d5.c_str(), Narrow(obj).c_str(), int(wrote));

				// The car navmesh from the same triangles: Recast with car settings (navbake.h), our code only, on this thread. Written
				// beside the input as <name>_car.obj, with four 30 m A* tests from the player (east, west, north, south).
				try {
					navbake::Input bi;
					bi.verts = v.data(); bi.nverts = int(c.nv); bi.tris = t.data(); bi.ntris = int(c.nt); bi.stride = 6;
					for (int k = 0; k < 3; ++k) { bi.bmin[k] = mn[k] - 1; bi.bmax[k] = mx[k] + 1; }
					const navbake::Params bp;
					navbake::Result br = navbake::Bake(bi, bp);
					const std::wstring carObj = j.dir + L"\\" + name + L"_car.obj";
					bool carWrote = false;
					if (br.ok) if (FILE* o = _wfopen(carObj.c_str(), L"wb")) {
						std::fprintf(o, "# FVO car navmesh (Recast 1.6.0, car settings: radius %.2f height %.2f climb %.2f slope %.0f cell %.2f/%.2f) for cell %08X; Z up\n",
							bp.agentRadius, bp.agentHeight, bp.agentClimb, bp.slopeDeg, bp.cellSize, bp.cellHeight, j.cellId);
						for (std::size_t i = 0; i < br.outVerts.size(); i += 3) std::fprintf(o, "v %.3f %.3f %.3f\n", br.outVerts[i], br.outVerts[i + 1], br.outVerts[i + 2]);
						for (std::size_t i = 0; i + 2 < br.outTris.size(); i += 3) std::fprintf(o, "f %d %d %d\n", br.outTris[i] + 1, br.outTris[i + 1] + 1, br.outTris[i + 2] + 1);
						carWrote = std::fclose(o) == 0;
					}
					std::string here = "-"; char hb[32];
					if (br.ok) { here.clear(); for (float z : navbake::SurfaceAt(br, j.px, j.py)) { std::snprintf(hb, sizeof hb, "%s%.2f", here.empty() ? "" : ",", z); here += hb; } if (here.empty()) here = "none"; }
					std::string paths; char pb[96];
					const float dirs[4][2] = { { 30, 0 }, { -30, 0 }, { 0, 30 }, { 0, -30 } };
					const char* names[4] = { "east", "west", "north", "south" };
					for (int k = 0; k < 4 && br.ok; ++k) {
						const float from[3] = { j.px, j.py, j.pz }, to[3] = { j.px + dirs[k][0], j.py + dirs[k][1], j.pz };
						const navbake::Path pa = navbake::FindPath(br, from, to, 3.0f);
						std::snprintf(pb, sizeof pb, " path_%s=%s/%.1fm/snap%.1f", names[k], !pa.found ? "none" : pa.complete ? "complete" : "partial", pa.length, pa.endSnap);
						paths += pb;
					}
					logger::Write("OBS system=navprobe event=car_mesh run=%u cell=%08X ok=%d failed=%s grid=%dx%d input_walkable_tris=%d regions=%d polys=%d detail_tris=%d area_m2=%.0f "
						"ms=%.0f raster=%.0f compact=%.0f regions_ms=%.0f mesh=%.0f detour=%.0f tile_bytes=%d agent=(r %.2f h %.2f climb %.2f slope %.0f) surface_here=%s%s file=%s wrote=%d",
						g_runs, j.cellId, int(br.ok), br.failed, br.gridW, br.gridH, br.walkableTris, br.regions, br.polys, br.detailTris, br.walkableArea,
						br.msTotal, br.msRaster, br.msCompact, br.msRegions, br.msMesh, br.msDetour, br.navDataSize, bp.agentRadius, bp.agentHeight, bp.agentClimb, bp.slopeDeg,
						here.c_str(), paths.c_str(), Narrow(carObj).c_str(), int(carWrote));
					br.Free();
				}
				catch (...) { logger::Write("OBS system=navprobe event=fault run=%u stage=car_mesh reason=exception (caught, probe thread)", g_runs); }
			}
			crash::Crumb("navprobe end", int(stage));
			crash::SetExpectedFaults(false);
			g_busy = false;
		}

		// The player's cell as the game holds it (POD only: SEH).
		struct Seen { uptr player = 0, cell = 0; float px = 0, py = 0, pz = 0; std::uint32_t cellId = 0, worldId = 0; bool exterior = false, gridOk = false; int gx = 0, gy = 0; };
		bool ReadSeen(uptr base, Seen* s)
		{
			__try {
				s->player = *reinterpret_cast<const uptr*>(base + addr::kPlayerCharacterPtr);
				if (!s->player) return true;
				s->cell = *reinterpret_cast<const uptr*>(s->player + off::kRefParentCell);
				s->px = *reinterpret_cast<const float*>(s->player + off::kRefPosition);
				s->py = *reinterpret_cast<const float*>(s->player + off::kRefPosition + 4);
				s->pz = *reinterpret_cast<const float*>(s->player + off::kRefPosition + 8);
				if (!s->cell) return true;
				s->cellId = *reinterpret_cast<const std::uint32_t*>(s->cell + off::kFormId);
				s->exterior = (*reinterpret_cast<const std::uint8_t*>(s->cell + 0x48) & 1) == 0;
				const uptr world = s->exterior ? *reinterpret_cast<const uptr*>(s->cell + off::kCellWorldspace) : 0;
				s->worldId = world ? *reinterpret_cast<const std::uint32_t*>(world + off::kFormId) : 0;
				const std::int32_t* grid = s->exterior ? *reinterpret_cast<std::int32_t* const*>(s->cell + 0x58) : nullptr;
				if (grid) { s->gx = grid[0]; s->gy = grid[1]; s->gridOk = true; }
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void Start(int key)
		{
			++g_runs;
			if (g_busy) { logger::Write("OBS system=navprobe event=refused run=%u reason=busy (one probe at a time)", g_runs); return; }
			if (!g_sigChecked) { g_sigChecked = true; g_sigOk = CheckSignatures(); }
			if (!g_sigOk) { logger::Write("OBS system=navprobe event=refused run=%u reason=disabled_by_signature_check", g_runs); return; }
			Seen s;
			if (!ReadSeen(game::Base(), &s)) { logger::Write("OBS system=navprobe event=refused run=%u reason=player_read_fault", g_runs); return; }
			Job j; j.key = key;
			j.cell = s.cell; j.cellId = s.cellId; j.worldId = s.worldId; j.exterior = s.exterior; j.gx = s.gx; j.gy = s.gy; j.gridOk = s.gridOk;
			j.px = s.px; j.py = s.py; j.pz = s.pz;
			const uptr player = s.player;
			if (!player) { logger::Write("OBS system=navprobe event=refused run=%u reason=no_player", g_runs); return; }
			if (!j.cell) { logger::Write("OBS system=navprobe event=refused run=%u reason=no_parent_cell", g_runs); return; }
			j.dir = fvopaths::Dir() + L"\\navprobe";
			CreateDirectoryW(j.dir.c_str(), nullptr);
			logger::Write("OBS system=navprobe event=start run=%u key=%s cell=%08X world=%08X exterior=%d grid=%d,%d grid_known=%d player=(%.2f %.2f %.2f) out=%s",
				g_runs, controlmap::KeyName(key).c_str(), j.cellId, j.worldId, int(j.exterior), j.gx, j.gy, int(j.gridOk), j.px, j.py, j.pz, Narrow(j.dir).c_str());
			// On the one engine thread shared with the car navmesh (engineworker.h: the game's thread numbers run out at 64).
			g_busy = true;
			if (!engineworker::Post([job = std::move(j)]() mutable { Worker(std::move(job)); }, "navprobe")) {
				g_busy = false; logger::Write("OBS system=navprobe event=refused run=%u reason=thread_start_failed", g_runs);
			}
		}
	}

	bool EngineReady()
	{
		if (!g_sigChecked) { g_sigChecked = true; g_sigOk = CheckSignatures(); }
		return g_sigOk;
	}

	bool Collect(const char* sys, unsigned run, std::uintptr_t cell, std::uint32_t cellId, std::vector<float>& v, std::vector<std::uint32_t>& t, double& msInput)
	{
		Counts c;
		bool copied = false;
		int stage = 0;
		std::uint64_t inputRet = 0;
		crash::SetExpectedFaults(true);
		CollectInto(sys, run, cell, cellId, c, v, t, copied, stage, inputRet, msInput);
		crash::SetExpectedFaults(false);
		return copied && c.nv > 0 && c.nt > 0;
	}

	void Tick(bool loading)
	{
		if (loading) { g_down = false; return; }
		const ULONGLONG now = GetTickCount64();
		if (!g_key && g_claimTries < 5 && now >= g_claimAt) {
			++g_claimTries; g_claimAt = now + 10000;
			g_key = controlmap::ClaimFirst("navprobe", "navmesh input probe (diagnostic)", { VK_OEM_2, VK_OEM_PERIOD, VK_OEM_7 });
			if (g_key) logger::Write("OBS system=navprobe event=key key=%s (press in game: writes the cell's navmesh input to Logs\\FlyingVehicleOverhaul\\navprobe)", controlmap::KeyName(g_key).c_str());
			else logger::Write("OBS system=navprobe event=no_key try=%d reason=all_test_keys_taken (/ %s; . %s; ' %s)", g_claimTries,
				controlmap::WhoUses(VK_OEM_2).c_str(), controlmap::WhoUses(VK_OEM_PERIOD).c_str(), controlmap::WhoUses(VK_OEM_7).c_str());
		}
		if (!g_key) return;
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		const bool held = pid == GetCurrentProcessId() && (GetAsyncKeyState(g_key) & 0x8000) != 0;
		if (held && !g_down) Start(g_key);
		g_down = held;
	}
}
