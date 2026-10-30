#include "routeedit.h"
#include "pilotlayout.h"
#include "routeeditor.h"
#include "routefiles.h"
#include "routemap.h"
#include "trafficroute.h"
#include "trafficlinks.h"
#include "strokefont.h"

#include "addresses.h"
#include "inputgate.h"
#include "trafficpool.h"
#include "fvopaths.h"
#include "game.h"
#include "log.h"
#include "environment.h"
#include "navmap.h"
#include "cellbake.h"
#include "vehicle_probe.h"
#include "prefetch.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include "secretunpause.h"
#include "notify.h"
#include "vehiclediag.h"
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

extern "C" int FVO_WorldName(char* out, int size);  // context.cpp

// Hooks (1.16.244, checked against the executable's bytes before patching, like hoverui's):
//  - StarMap::SurfaceMapState vtable 0x144C99A00 slot 4 = FUN_1416DA0B0 (state, ?, float dt): the local map's per-frame
//    update; it moves the camera rig and calls FUN_1416DF1A0, which projects the native markers. After it returns we copy
//    the camera (routemap.h has the offsets) under a lock; nothing else of the state is kept.
//  - StarMap::StarMapMenu vtable 0x144C97BC0 slot 8 = FUN_1416BDE10, the IMenu message handler (the same slot hoverui uses
//    for the scanner): the menu (root Value +0x58) and its movie (+0x88). The star map's movie hosts surfacemap.swf.
// All Scaleform work runs in that movie's own Advance (hoverui's MovieImpl detour calls Advance below).
namespace routeedit
{
	namespace
	{
		using uptr = std::uintptr_t;

		// ---- Scaleform GFx values (layout and functions as hoverui.cpp documents them) ----
		struct Value
		{
			Value* prev = nullptr;
			Value* next = nullptr;
			void* iface = nullptr;
			std::int32_t type = 0;
			std::int32_t pad = 0;
			union {
				double number;
				const char* string;
				void* object;
				bool boolean;
				std::uint64_t raw = 0;
			};
			std::size_t aux = 0;
		};
		static_assert(sizeof(Value) == 0x30);
		constexpr std::int32_t kBool = 2, kNumber = 5, kString = 6, kManaged = 0x40;
		constexpr std::uint32_t kRvaGetMember = 0x033528A0, kRvaSetMember = 0x03352C90, kRvaInvoke = 0x033530B0, kRvaRelease = 0x033524A0;
		uptr Fn(std::uint32_t rva) { return game::Base() + rva; }
		bool IsObject(const Value& v) { const int t = v.type & 0x8F; return t == 8 || t == 9 || t == 10; }
		bool IsDisplay(const Value& v) { return (v.type & 0x8F) == 10; }
		bool GetMember(const Value& o, const char* name, Value* out)
		{
			return reinterpret_cast<bool (*)(void*, void*, const char*, Value*, bool)>(Fn(kRvaGetMember))(o.iface, o.object, name, out, IsDisplay(o));
		}
		bool SetMember(const Value& o, const char* name, const Value& v)
		{
			return reinterpret_cast<bool (*)(void*, void*, const char*, const Value*, bool)>(Fn(kRvaSetMember))(o.iface, o.object, name, &v, IsDisplay(o));
		}
		bool Invoke(const Value& o, const char* name, const Value* args, std::size_t n)
		{
			return reinterpret_cast<bool (*)(void*, void*, Value*, const char*, const Value*, std::size_t, bool)>(Fn(kRvaInvoke))(o.iface, o.object, nullptr, name, args, n, IsDisplay(o));
		}
		bool CreateObject(uptr movie, Value* out, const char* cls, const Value* args, std::uint32_t n)
		{
			const uptr root = *reinterpret_cast<uptr*>(movie + 0x18);
			if (!root) return false;
			auto** vt = *reinterpret_cast<void***>(root);
			reinterpret_cast<void (*)(uptr, Value*, const char*, const Value*, std::uint32_t)>(vt[46])(root, out, cls, args, n);
			return IsObject(*out);
		}
		void Release(Value& v)
		{
			if ((v.type & kManaged) && v.iface && v.object) reinterpret_cast<void (*)(void*, Value*, void*)>(Fn(kRvaRelease))(v.iface, &v, v.object);
			v = Value{};
		}
		bool InvokeR(const Value& o, const char* name, const Value* args, std::size_t n, Value* result)
		{
			return reinterpret_cast<bool (*)(void*, void*, Value*, const char*, const Value*, std::size_t, bool)>(Fn(kRvaInvoke))(o.iface, o.object, result, name, args, n, IsDisplay(o));
		}
		// A managed string (kManaged set) holds a pointer to the string's node, whose first field is the text (Scaleform
		// GFx::Value::GetString: *pStringManaged); a plain one points at the text itself.
		std::string StringOf(const Value& v)
		{
			if ((v.type & 0x8F) != kString || !v.string) return {};
			const char* text = (v.type & kManaged) ? *reinterpret_cast<const char* const*>(v.string) : v.string;
			return text ? std::string(text) : std::string();
		}
		Value Arg(const Value& v) { Value a; a.iface = v.iface; a.type = v.type & ~kManaged; a.raw = v.raw; return a; }
		Value Str(const char* s) { Value v; v.type = kString; v.string = s; return v; }
		Value Num(double d) { Value v; v.type = kNumber; v.number = d; return v; }
		Value Bool(bool b) { Value v; v.type = kBool; v.boolean = b; return v; }
		bool GetNumber(const Value& o, const char* name, double& out)
		{
			Value v;
			if (!GetMember(o, name, &v)) return false;
			const bool ok = (v.type & 0x8F) == kNumber || (v.type & 0x8F) == 3 || (v.type & 0x8F) == 4;
			if ((v.type & 0x8F) == kNumber) out = v.number;
			else if (ok) out = double(static_cast<std::int32_t>(v.raw));
			return ok && std::isfinite(out);
		}

		// ---- Hooks ----
		constexpr std::uint32_t kRvaMapStateVtbl = 0x04C99A00, kRvaMapUpdate = 0x016DA0B0;
		constexpr int kSlotMapUpdate = 4;
		constexpr std::uint32_t kRvaStarMapVtbl = 0x04C97BC0, kRvaStarMapMessage = 0x016BDE10;
		constexpr int kSlotMessage = 8;
		constexpr std::uint32_t kRvaCursor = 0x061DE010;  // pointer: +0x60 / +0x64 cursor (pixels), +0x70 / +0x74 screen size
		using Update_t = void (*)(uptr, uptr, float);
		using Message_t = std::uint64_t (*)(uptr, void*);
		Update_t g_updateOriginal = nullptr;
		Message_t g_messageOriginal = nullptr;
		std::atomic<uptr> g_menu{ 0 }, g_movie{ 0 };

		struct Snap { routemap::Camera cam; ULONGLONG at = 0; };
		std::mutex g_snapLock;
		Snap g_snap;
		std::atomic<ULONGLONG> g_snapAt{ 0 };
		std::atomic<bool> g_captureFailed{ false };

		bool Capture(uptr state, routemap::Camera& c)
		{
			__try {
				const uptr rig = *reinterpret_cast<const uptr*>(state + 0x78);
				const uptr cam = rig ? *reinterpret_cast<const uptr*>(rig + 0x20) : 0;
				if (!cam) return false;
				std::memcpy(c.m, reinterpret_cast<const void*>(cam + 0x180), sizeof c.m);
				std::memcpy(c.port, reinterpret_cast<const void*>(cam + 0x1E4), sizeof c.port);
				std::memcpy(c.stage, reinterpret_cast<const void*>(state + 0xA0), sizeof c.stage);
				const float* f = reinterpret_cast<const float*>(rig + 0x30);
				const float* e = reinterpret_cast<const float*>(rig + 0x50);
				c.focus = { f[0], f[1], f[2] };
				c.eye = { e[0], e[1], e[2] };
				const uptr cursor = *reinterpret_cast<const uptr*>(game::Base() + kRvaCursor);
				if (cursor) {
					const float* m = reinterpret_cast<const float*>(cursor + 0x60);
					const float* s = reinterpret_cast<const float*>(cursor + 0x70);
					c.mouseX = m[0]; c.mouseY = m[1]; c.width = s[0]; c.height = s[1];
				}
				for (float v : c.m) if (!std::isfinite(v)) return false;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void MapUpdateDetour(uptr state, uptr a2, float dt)
		{
			g_updateOriginal(state, a2, dt);
			if (g_captureFailed.load()) return;
			routemap::Camera c;
			if (!Capture(state, c)) {
				static int misses = 0;
				if (++misses == 600) {
					g_captureFailed = true;
					logger::Write("Route editor: the local map's camera could not be read 600 times in a row; the editor stays off this session");
				}
				return;
			}
			const ULONGLONG now = GetTickCount64();
			{ std::lock_guard lk(g_snapLock); g_snap.cam = c; g_snap.at = now; }
			const ULONGLONG before = g_snapAt.exchange(now);
			if (!before || now - before > 2000) {
				logger::Write("Route editor: local map camera: focus (%.1f, %.1f, %.1f), eye (%.1f, %.1f, %.1f), port (%.3f %.3f %.3f %.3f), "
					"marker rect (%.1f %.1f %.1f %.1f), cursor %.0f,%.0f of %.0fx%.0f", c.focus.x, c.focus.y, c.focus.z, c.eye.x, c.eye.y, c.eye.z,
					c.port[0], c.port[1], c.port[2], c.port[3], c.stage[0], c.stage[1], c.stage[2], c.stage[3], c.mouseX, c.mouseY, c.width, c.height);
			}
		}
		std::uint64_t StarMapMessageDetour(uptr menu, void* msg)
		{
			const auto r = g_messageOriginal(menu, msg);
			g_movie.store(*reinterpret_cast<uptr*>(menu + 0x88));
			if (g_menu.exchange(menu) != menu) logger::Write("Route editor: star map menu %p, movie %p", reinterpret_cast<void*>(menu), reinterpret_cast<void*>(g_movie.load()));
			return r;
		}
		bool PatchSlot(std::uint32_t vtblRva, int slot, std::uint32_t expectRva, void* detour, void** original)
		{
			auto** p = reinterpret_cast<void**>(game::Base() + vtblRva) + slot;
			if (reinterpret_cast<uptr>(*p) != game::Base() + expectRva) {
				MEMORY_BASIC_INFORMATION mbi{};
				const bool foreign = VirtualQuery(*p, &mbi, sizeof(mbi)) && mbi.AllocationBase && reinterpret_cast<uptr>(mbi.AllocationBase) != game::Base();
				if (!foreign) return false;
				logger::Write("Route editor: vtable slot %d (rva %08X) was already patched by another plugin; chaining to it", slot, vtblRva);
			}
			DWORD old = 0;
			if (!VirtualProtect(p, sizeof(void*), PAGE_READWRITE, &old)) return false;
			*original = *p;
			*p = detour;
			VirtualProtect(p, sizeof(void*), old, &old);
			return true;
		}

		bool Snapshot(routemap::Camera& c)
		{
			std::lock_guard lk(g_snapLock);
			if (!g_snap.at || GetTickCount64() - g_snap.at > 300) return false;
			c = g_snap.cam;
			return true;
		}

		// ---- Files ----
		std::wstring TrafficDir()
		{
			HMODULE self = nullptr;
			GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&TrafficDir), &self);
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(self, path, MAX_PATH);
			std::wstring dir(path);
			return dir.substr(0, dir.find_last_of(L"\\/")) + L"\\FVO_Traffic";
		}
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		std::string Narrow(const std::wstring& w) { std::string s; for (const wchar_t c : w) s.push_back(c < 128 ? char(c) : '?'); return s; }
		bool ReadText(const std::wstring& path, std::string& out)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in) return false;
			std::ostringstream s; s << in.rdbuf(); out = s.str();
			return true;
		}
		// Temp file, flushed, then replaces the old one; the old one is kept as .bak. The addon only reads *.ini.
		bool WriteAtomic(const std::wstring& path, const std::string& text, std::string& why)
		{
			const std::wstring tmp = path + L".tmp", bak = path + L".bak";
			const HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE) { why = "cannot create " + Narrow(tmp) + " (error " + std::to_string(GetLastError()) + ")"; return false; }
			DWORD written = 0;
			const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size() && FlushFileBuffers(h);
			CloseHandle(h);
			if (!ok) { DeleteFileW(tmp.c_str()); why = "writing " + Narrow(tmp) + " failed"; return false; }
			if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) CopyFileW(path.c_str(), bak.c_str(), FALSE);
			if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
				DeleteFileW(tmp.c_str());
				why = "replacing " + Narrow(path) + " failed (error " + std::to_string(GetLastError()) + ")";
				return false;
			}
			trafficpool::ReloadFiles();  // FVO Traffic re-reads its files now (it no longer polls the folder)
			return true;
		}
		constexpr char kSignature[] = "written by the map route editor";

		// ---- Editor state (only the star map movie's thread touches it) ----
		enum class Mode { Select, Drop, Drag, Landing, Spawn, Connect };
		const char* ModeName(Mode m)
		{
			switch (m) { case Mode::Drop: return "Drop"; case Mode::Drag: return "Drag"; case Mode::Landing: return "Landing spot"; case Mode::Spawn: return "Spawn spot"; case Mode::Connect: return "Connect"; default: return "Select"; }
		}
		// A route of another file (game coordinates; anchored cities placed with the addon's shift).
		struct Other { std::string name, file; trafficroute::Line line; bool ring = false; trafficdata::Route route; };
		struct Editor {
			bool on = false, loaded = false;
			std::string world, fileName, status;
			ULONGLONG statusAt = 0;
			routefiles::File saved, file;           // on disk / being edited (spots edit `file` directly)
			std::optional<routeeditor::Draft> draft;
			int draftIndex = -1;                    // the route the draft replaces; -1: a new route
			int route = -1, point = -1;             // selection (route index in file.routes, point in the shown route)
			int spotKind = 0, spot = -1;            // 1 landing, 2 spawn
			int link = -1;                          // selected junction (file.links)
			Mode mode = Mode::Select;
			bool dragging = false;
			routemap::Vec3 dragAt;
			std::vector<Other> others;
			int other = -1;                         // selected route of another file (grey)
			trafficdata::Data othersData;           // their worlds (to place edited originals)
			// Edited originals of anchored cities: file -> game shift per route id; routes whose world is not placed here now
			// are kept as read (file coordinates) and written back unchanged.
			std::map<std::string, trafficdata::Vec> shifts;
			std::set<std::string> unplaced;
			std::map<std::string, trafficdata::Vec> spotShifts;  // the same for landing / spawn spots inside anchored cities
			std::set<std::string> unplacedSpots;
			trafficdata::Vec draftShift;
			bool othersHaveTraffic = false;
			ULONGLONG othersAt = 0, confirmDeleteAt = 0;
			double roundRadius=20;
            double previewHour = -1;                // -1: the live local time
			bool loading = false;                   // the traffic model restart's loading panel is up
			ULONGLONG loadingAt = 0;
			ULONGLONG filledAt = 0;                 // Start/Swap: positions filled; the world runs (secret unpause) until traffic is armed
			double phase = 0;
			ULONGLONG tickAt = 0;
		} g_ed;

		// Defined with the drawing below.
		bool Local(const routemap::Camera& c, routemap::Vec3 p, double& x, double& y);
		bool CursorLocal(const routemap::Camera& c, double& x, double& y);
		double SegmentDistance(double px, double py, double ax, double ay, double bx, double by);
		double PanelX();
		// Junction positions on a route follow its points (2026-10-29: junctions may sit at any point): an inserted point at
		// `index` moves later junctions one on; a removed one drops its junction and moves later ones back.
		void LinksPointInserted(const std::string& id, std::size_t index, std::size_t oldCount);
		void LinksPointRemoved(const std::string& id, std::size_t index, std::size_t oldCount);

		void Status(const std::string& s)
		{
			g_ed.status = s; g_ed.statusAt = GetTickCount64();
			logger::Write("Route editor: %s", s.c_str());
		}
		std::string EditorKey() { return routefiles::WorldKey(g_ed.world); }
		trafficdata::Route ToTd(const routefiles::Route& r, const std::string& world)
		{
			trafficdata::Route t;
			t.name = r.id; t.label = r.label; t.world = r.world.empty() ? world : r.world; t.ring = r.ring; t.shape = r.shape;
			t.tilt = r.tilt; t.tiltToward = r.tiltToward; t.points = r.points; t.rounding=r.rounding; t.closed = r.closed; t.removed = r.removed;
			t.lanes = r.lanes; t.speed = r.speed; t.width = r.width; t.schedule = r.schedule; t.test = r.test;
			return t;
		}
		routefiles::Route FromTd(const trafficdata::Route& t)
		{
			routefiles::Route r;
			r.id = t.name; r.label = t.label; r.points = t.points; r.rounding=t.rounding; r.closed = t.closed; r.lanes = t.lanes; r.speed = t.speed;
			r.width = t.width; r.schedule = t.schedule; r.test = t.test;
			r.world = t.world == EditorKey() ? std::string() : t.world;
			r.ring = t.ring; r.shape = t.shape; r.tilt = t.tilt; r.tiltToward = t.tiltToward; r.removed = t.removed;
			return r;
		}
		// An editor route that can be drawn and edited here (not a removal marker, its world placed in the player's world).
		bool Usable(const routefiles::Route& r) { return !r.removed && !g_ed.unplaced.count(r.id); }
		bool IsOriginal(const routefiles::Route& r) { return r.id.rfind("ed_", 0) != 0; }  // an edited or removed original
		// The route shown as selected: the draft while one is open, else the saved route, else a selected original (grey).
		const trafficdata::Route* Shown(trafficdata::Route& scratch)
		{
			if (g_ed.draft) return &g_ed.draft->Value();
			if (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size())) {
				const auto& r = g_ed.file.routes[std::size_t(g_ed.route)];
				if (!Usable(r)) return nullptr;
				scratch = ToTd(r, EditorKey());
				return &scratch;
			}
			if (g_ed.other >= 0 && g_ed.other < int(g_ed.others.size())) return &g_ed.others[std::size_t(g_ed.other)].route;
			return nullptr;
		}
		// The game shift of a route's world key (0 for the player's own world), false when that world is not here now.
		bool WorldShift(const std::string& key, trafficdata::Vec& shift);
		void EditOriginal();
		// Every change starts a draft of the selected route (an original of another file: its editable copy).
		bool EnsureDraft()
		{
			if (g_ed.draft) return true;
			if (g_ed.route < 0 && g_ed.other >= 0) { EditOriginal(); return bool(g_ed.draft); }
			if (g_ed.route < 0 || g_ed.route >= int(g_ed.file.routes.size()) || !Usable(g_ed.file.routes[std::size_t(g_ed.route)])) {
				Status("Pick a route first (click it, or Next route), or press New route"); return false;
			}
			g_ed.draft.emplace(ToTd(g_ed.file.routes[std::size_t(g_ed.route)], EditorKey()));
			g_ed.draftIndex = g_ed.route;
			const auto it = g_ed.shifts.find(g_ed.file.routes[std::size_t(g_ed.route)].id);
			g_ed.draftShift = it == g_ed.shifts.end() ? trafficdata::Vec{} : it->second;
			return true;
		}

		// Anchored traffic worlds as the addon found them (FVO_SetWorldShift): name -> (game world, shift).
		std::mutex g_shiftLock;
		std::map<std::string, std::pair<std::string, trafficdata::Vec>> g_worldShifts;
		std::vector<std::pair<std::string, trafficdata::Vec>> ShiftsHere()
		{
			std::vector<std::pair<std::string, trafficdata::Vec>> out;
			std::lock_guard lk(g_shiftLock);
			for (const auto& [name, w] : g_worldShifts) if (trafficdata::Lower(w.first) == trafficdata::Lower(g_ed.world)) out.push_back({ name, w.second });
			return out;
		}
		bool WorldShift(const std::string& key, trafficdata::Vec& shift)
		{
			shift = {};
			if (key.empty() || key == EditorKey()) return true;
			for (const auto& [name, s] : ShiftsHere()) if (name == key) { shift = s; return true; }
			if (g_ed.othersData.anchors.count(key)) return false;  // an anchored city not found in this world now
			return trafficdata::SameWorld(g_ed.othersData, key, g_ed.world);
		}
		void Move(routefiles::Route& r, const trafficdata::Vec& s, double sign)
		{
			for (auto& p : r.points) { p.x += sign * s.x; p.y += sign * s.y; p.z += sign * s.z; }
			r.shape.cx += sign * s.x; r.shape.cy += sign * s.y; r.shape.z += sign * s.z;
		}
		// File coordinates -> game coordinates for every editor route whose world is placed here (now or later).
		void PlaceFileRoutes()
		{
			for (auto& r : g_ed.file.routes) {
				if (r.removed || g_ed.shifts.count(r.id)) continue;
				trafficdata::Vec shift;
				if (!WorldShift(r.world, shift)) continue;
				Move(r, shift, +1);
				for (auto& q : g_ed.saved.routes) if (q.id == r.id && !q.removed) Move(q, shift, +1);  // the copy Cancel restores
				g_ed.shifts[r.id] = shift; g_ed.unplaced.erase(r.id);
			}
			for (const auto& r : g_ed.file.routes) if (!r.removed && !g_ed.shifts.count(r.id)) g_ed.unplaced.insert(r.id);
			for (int kind = 1; kind <= 2; ++kind)
				for (auto& sp : kind == 1 ? g_ed.file.landings : g_ed.file.spawns) {
					if (g_ed.spotShifts.count(sp.id)) continue;
					trafficdata::Vec shift;
					if (!WorldShift(sp.world, shift)) { g_ed.unplacedSpots.insert(sp.id); continue; }
					sp.at.x += shift.x; sp.at.y += shift.y; sp.at.z += shift.z;
					for (auto& q : kind == 1 ? g_ed.saved.landings : g_ed.saved.spawns)
						if (q.id == sp.id) { q.at.x += shift.x; q.at.y += shift.y; q.at.z += shift.z; }
					g_ed.spotShifts[sp.id] = shift; g_ed.unplacedSpots.erase(sp.id);
				}
		}
		bool SpotHere(const routefiles::Spot& sp) { return !g_ed.unplacedSpots.count(sp.id); }
		// The anchored city a spot at (x, y) belongs to: the one whose routes pass nearest (within 800 m); empty: the editor's world.
		std::string CityAt(double x, double y, trafficdata::Vec& shift)
		{
			shift = {};
			std::string best; double bestD = 800;
			const auto here = ShiftsHere();
			for (const auto& o : g_ed.others)
				for (const auto& [name, sh] : here) {
					if (o.route.world != name) continue;
					const auto q = trafficroute::At(o.line, 0, trafficroute::Nearest(o.line, 0, x, y));
					const double d = std::hypot(q.x - x, q.y - y);
					if (d < bestD) { bestD = d; best = name; shift = sh; }
				}
			return best;
		}
		void LoadOthers()
		{
			g_ed.othersAt = GetTickCount64();
			const std::wstring dir = TrafficDir();
			trafficdata::Data d;
			WIN32_FIND_DATAW fd{};
			std::vector<std::wstring> files;
			const HANDLE h = FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
			if (h != INVALID_HANDLE_VALUE) {
				do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName); } while (FindNextFileW(h, &fd));
				FindClose(h);
			}
			std::sort(files.begin(), files.end());
			for (const auto& f : files) {
				if (_wcsicmp(f.c_str(), Wide(g_ed.fileName).c_str()) == 0) continue;
				std::string text;
				if (ReadText(dir + L"\\" + f, text)) trafficdata::Read(text, Narrow(f), d);
			}
			// ringroad.ini: one ring per world id
			{
				const std::wstring ini = fvopaths::File(L"ringroad.ini");
				wchar_t ring[256]{};
				GetPrivateProfileStringW(Wide(g_ed.world).c_str(), L"ring", L"", ring, 256, ini.c_str());
				if (*ring) trafficdata::AddRingroad(g_ed.world, Narrow(ring), d);
			}
			const std::string keep = g_ed.other >= 0 && g_ed.other < int(g_ed.others.size()) ? g_ed.others[std::size_t(g_ed.other)].name : std::string();
			for (const auto& [name, shift] : ShiftsHere()) trafficdata::PlaceWorld(d, name, g_ed.world, shift);
			g_ed.others.clear(); g_ed.other = -1;
			g_ed.othersHaveTraffic = !d.traffic.empty();
			std::set<std::string> mine;  // edited or removed here: the editor's version is the one that counts
			for (const auto& r : g_ed.file.routes) mine.insert(r.id);
			if (g_ed.draft) mine.insert(g_ed.draft->Value().name);
			for (const auto& r : d.routes) {
				if (!trafficdata::SameWorld(d, r.world, g_ed.world) || mine.count(r.name)) continue;
				if (r.name == keep) g_ed.other = int(g_ed.others.size());
				g_ed.others.push_back({ r.name, r.file, trafficroute::Build(r), r.ring, r });
			}
			g_ed.othersData = std::move(d);
			g_ed.othersData.routes.clear();
			PlaceFileRoutes();
		}
		// Starts editing a route of another file: an editable copy under the same name, in the editor's file, that replaces the
		// original once saved (the addon prefers the editor's file). Anchored cities keep their world key and are saved back in
		// their own coordinates; other worlds become the editor's own key (the same game world).
		void EditOriginal()
		{
			if (g_ed.draft || g_ed.other < 0 || g_ed.other >= int(g_ed.others.size())) return;
			const auto& o = g_ed.others[std::size_t(g_ed.other)];
			trafficdata::Route t = o.route;
			trafficdata::Vec shift;
			bool anchored = false;
			for (const auto& [name, s] : ShiftsHere()) if (name == t.world) { shift = s; anchored = true; }
			if (!anchored) t.world = EditorKey();
			if (t.label.empty()) t.label = t.name;
			t.file.clear(); t.test = false;
			g_ed.draft.emplace(t);
			g_ed.draftIndex = -1; g_ed.draftShift = shift;
			g_ed.route = -1; g_ed.point = -1; g_ed.spot = -1;
			Status("Editing original '" + o.name + "' from " + o.file + ". Save puts your version in the editor's file, where it replaces the original; "
				"Del route on it later brings the original back." + (t.ring ? " A ring: Drag its centre, Radius - / +, Z sets its height." : ""));
			g_ed.other = -1;
		}
		std::set<std::string> TakenIds()
		{
			std::set<std::string> taken;
			for (const auto& o : g_ed.others) taken.insert(o.name);
			for (const auto& r : g_ed.file.routes) taken.insert(r.id);
			return taken;
		}
		void Open()
		{
			char world[201]{};
			if (!FVO_WorldName(world, sizeof world)) { Status("No world: the route editor works on a planet's local map"); return; }
			if (!g_ed.loaded || g_ed.world != world) {
				g_ed = Editor{};
				g_ed.world = world;
				g_ed.fileName = routefiles::FileName(world);
				g_ed.file.worldId = world;
				std::string text, why;
				const std::wstring path = TrafficDir() + L"\\" + Wide(g_ed.fileName);
				if (ReadText(path, text)) {
					std::vector<std::string> problems;
					if (text.find(kSignature) == std::string::npos) Status("Warning: " + g_ed.fileName + " was not written by the editor; saving is off for it");
					else if (!routefiles::Parse(text, g_ed.fileName, g_ed.file, problems)) Status("Could not read " + g_ed.fileName + "; starting empty");
					for (const auto& p : problems) logger::Write("Route editor: %s", p.c_str());
				}
				g_ed.file.worldId = world;
				g_ed.saved = g_ed.file;
				g_ed.loaded = true;
				for (std::size_t i = 0; i < g_ed.file.routes.size(); ++i) if (!g_ed.file.routes[i].removed) { g_ed.route = int(i); break; }
			}
			LoadOthers();
			g_ed.on = true;
			Status("Traffic routes for " + g_ed.world + ": " + std::to_string(g_ed.file.routes.size()) + " editor route(s), " +
				std::to_string(g_ed.others.size()) + " from other files (grey, read-only)");
			double h = 0;
			if (LocalHours(h)) logger::Write("Route editor: planet local time read as %.2f h (compare with the game's clock)", h);
			else logger::Write("Route editor: planet local time unknown here");
		}
		bool LocalMinute(int& minute)
		{
			double h = 0;
			if (g_ed.previewHour >= 0) h = g_ed.previewHour;
			else if (!LocalHours(h)) return false;
			minute = std::clamp(int(h * 60.0), 0, routeschedule::kMinutesPerDay - 1);
			return true;
		}
		std::string Clock(int minute) { char b[16]; std::snprintf(b, sizeof b, "%02d:%02d", minute / 60, minute % 60); return b; }
		std::string ScheduleText(const routeschedule::Schedule& s)
		{
			if (s.allDay) return "All day, " + std::to_string(s.allDayVehicles) + " vehicle(s)";
			std::string t;
			for (const auto& w : s.windows) t += (t.empty() ? "" : ", ") + Clock(w.start) + "-" + Clock(w.end) + " x" + std::to_string(w.vehicles);
			return t;
		}
		// Warnings that never block a save; the blocking problems are routeeditor::Validate's.
		std::vector<std::string> Warnings(const trafficdata::Route& r)
		{
			std::vector<std::string> w;
			const auto& p = r.points;
			const std::size_t n = p.size();
			for (std::size_t i = 0; i + 1 < n || (r.closed && i < n && n > 2); ++i) {
				const auto& a = p[i]; const auto& b = p[(i + 1) % n];
				const double flat = std::hypot(b.x - a.x, b.y - a.y);
				if (flat < 15) { w.push_back("points " + std::to_string(i + 1) + " and " + std::to_string((i + 1) % n + 1) + " are under 15 m apart"); break; }
				if (std::fabs(b.z - a.z) > flat) { w.push_back("a climb steeper than 45 degrees after point " + std::to_string(i + 1)); break; }
			}
			for (std::size_t i = 0; i < n && n > 2; ++i) {
				if (!r.closed && (i == 0 || i + 1 == n)) continue;
				const auto& a = p[(i + n - 1) % n]; const auto& b = p[i]; const auto& c = p[(i + 1) % n];
				const double ux = b.x - a.x, uy = b.y - a.y, vx = c.x - b.x, vy = c.y - b.y;
				const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
				if (lu < 1e-6 || lv < 1e-6) continue;
				if ((ux * vx + uy * vy) / (lu * lv) < -0.5) { w.push_back("a sharp turn (over 120 degrees) at point " + std::to_string(i + 1)); break; }
			}
			w.push_back("buildings, terrain and landing ground are not checked from the map; use Test route");
			return w;
		}
		const char* IssueText(routeeditor::Issue i)
		{
			switch (i) {
			case routeeditor::Issue::TooFewPoints: return "needs at least 2 points (3 for a loop)";
			case routeeditor::Issue::DuplicatePoints: return "two neighbouring points are in the same place";
			case routeeditor::Issue::InvalidPoint: return "a point has no valid position";
			case routeeditor::Issue::InvalidSettings: return "speed, lanes or width out of range";
			case routeeditor::Issue::MissingName: case routeeditor::Issue::MissingWorld: return "missing its name or world";
			case routeeditor::Issue::WrongType: return "not a path";
			default: return "ok";
			}
		}

		void Save()
		{
			routefiles::File next = g_ed.file;
			if (g_ed.draft) {
				const auto issue = g_ed.draft->Check();
				if (issue != routeeditor::Issue::None) { Status(std::string("Not saved: the route ") + IssueText(issue)); return; }
				if (const auto issue = routeschedule::Validate(g_ed.draft->Value().schedule); issue != routeschedule::Issue::None) {
					Status(std::string("Not saved: ") + routeschedule::IssueText(issue));
					return;
				}
				const auto r = FromTd(g_ed.draft->Value());
				if (g_ed.draftIndex >= 0 && g_ed.draftIndex < int(next.routes.size())) next.routes[std::size_t(g_ed.draftIndex)] = r;
				else { next.routes.push_back(r); g_ed.draftIndex = int(next.routes.size()) - 1; }
				g_ed.shifts[r.id] = g_ed.draftShift;
			}
			// On disk every route is in its own world's coordinates (anchored cities: minus the shift found in this game).
			routefiles::File disk = next;
			for (auto& r : disk.routes) if (const auto it = g_ed.shifts.find(r.id); it != g_ed.shifts.end() && !r.removed) Move(r, it->second, -1);
			for (auto* list : { &disk.landings, &disk.spawns })
				for (auto& sp : *list)
					if (const auto it = g_ed.spotShifts.find(sp.id); it != g_ed.spotShifts.end()) { sp.at.x -= it->second.x; sp.at.y -= it->second.y; sp.at.z -= it->second.z; }
			const std::wstring path = TrafficDir() + L"\\" + Wide(g_ed.fileName);
			std::string existing;
			if (ReadText(path, existing) && existing.find(kSignature) == std::string::npos) {
				Status("Not saved: " + g_ed.fileName + " exists and was not written by the editor (rename it to keep it)");
				return;
			}
			next.traffic = !g_ed.othersHaveTraffic;
			disk.traffic = next.traffic;
			CreateDirectoryW(TrafficDir().c_str(), nullptr);
			std::string why;
			if (!WriteAtomic(path, routefiles::Format(disk), why)) { Status("Not saved: " + why); return; }
			const int route = g_ed.draft ? g_ed.draftIndex : g_ed.route;
			g_ed.file = next; g_ed.saved = next;
			g_ed.draft.reset(); g_ed.draftIndex = -1; g_ed.route = route;
			LoadOthers();
			Status("Saved " + g_ed.fileName + ": traffic reads it within ~2 s. Vehicles already flying finish their lap and land before they change route.");
		}
		void Cancel()
		{
			g_ed.draft.reset(); g_ed.draftIndex = -1; g_ed.dragging = false;
			g_ed.file = g_ed.saved;
			if (g_ed.route >= int(g_ed.file.routes.size())) g_ed.route = g_ed.file.routes.empty() ? -1 : 0;
			g_ed.point = -1; g_ed.spot = -1; g_ed.link = -1;
			LoadOthers();
			Status("Changes discarded");
		}
		void NewRoute()
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			routefiles::Route r;
			r.label = "Route " + std::to_string(g_ed.file.routes.size() + 1);
			r.id = routefiles::NewId(r.label, TakenIds());
			g_ed.draft.emplace(ToTd(r, routefiles::WorldKey(g_ed.world)));
			g_ed.draftShift = {};  // a new route is in the player's world (2026-10-29 review: it kept the last edited city's shift)
			g_ed.draftIndex = -1; g_ed.route = -1; g_ed.point = -1; g_ed.other = -1;
			g_ed.mode = Mode::Drop;
			Status("New route: click the map (or press Insert) to drop points; Save when done");
		}
		void DeleteRoute()
		{
			const ULONGLONG nowMs = GetTickCount64();
			// A route of another file: hide it (a removal marker in the editor's file; Del route on the marker restores it).
			if (!g_ed.draft && g_ed.route < 0 && g_ed.other >= 0 && g_ed.other < int(g_ed.others.size())) {
				const auto& o = g_ed.others[std::size_t(g_ed.other)];
				if (nowMs - g_ed.confirmDeleteAt > 3000) { g_ed.confirmDeleteAt = nowMs; Status("Remove original route '" + o.name + "' (" + o.file + ")? Press Del route again within 3 s"); return; }
				g_ed.confirmDeleteAt = 0;
				routefiles::Route gone;
				gone.id = o.name; gone.label = o.name; gone.removed = true;
				trafficdata::Vec shift;
				bool anchored = false;
				for (const auto& [name, sh] : ShiftsHere()) if (name == o.route.world) anchored = true;
				gone.world = anchored ? o.route.world : std::string();
				(void)shift;
				g_ed.file.routes.push_back(gone);
				const std::string name = o.name;
				g_ed.other = -1;
				Save();
				g_ed.route = int(g_ed.file.routes.size()) - 1;
				Status("Removed original '" + name + "' (" + std::to_string(g_ed.file.routes.size()) + " editor entries). It stays listed under < Route / Route > as removed; Del route there brings it back.");
				return;
			}
			if (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size()) && !g_ed.draft && IsOriginal(g_ed.file.routes[std::size_t(g_ed.route)])) {
				const auto& r = g_ed.file.routes[std::size_t(g_ed.route)];
				if (nowMs - g_ed.confirmDeleteAt > 3000) {
					g_ed.confirmDeleteAt = nowMs;
					Status(std::string(r.removed ? "Bring back" : "Undo your edits of") + " original route '" + r.id + "'? Press Del route again within 3 s");
					return;
				}
				g_ed.confirmDeleteAt = 0;
				const std::string name = r.id;
				g_ed.shifts.erase(name); g_ed.unplaced.erase(name);
				g_ed.file.routes.erase(g_ed.file.routes.begin() + g_ed.route);
				g_ed.route = -1; g_ed.point = -1;
				Save();
				Status("Original route '" + name + "' is back as its file has it (grey).");
				return;
			}
			if (g_ed.route < 0 || g_ed.route >= int(g_ed.file.routes.size()) || (g_ed.draftIndex == -1 && g_ed.draft)) {
				if (g_ed.draft) { Cancel(); return; }
				Status("No saved route selected"); return;
			}
			const ULONGLONG now = GetTickCount64();
			if (now - g_ed.confirmDeleteAt > 3000) { g_ed.confirmDeleteAt = now; Status("Delete route '" + g_ed.file.routes[std::size_t(g_ed.route)].label + "'? Press Delete route again within 3 s"); return; }
			g_ed.confirmDeleteAt = 0;
			const std::string label = g_ed.file.routes[std::size_t(g_ed.route)].label;
			g_ed.draft.reset();
			const std::string gone = g_ed.file.routes[std::size_t(g_ed.route)].id;
			auto& links = g_ed.file.links;
			links.erase(std::remove_if(links.begin(), links.end(), [&](const routefiles::Link& k) { return k.to == gone || k.from == gone; }), links.end());
			g_ed.link = -1;
			g_ed.file.routes.erase(g_ed.file.routes.begin() + g_ed.route);
			g_ed.route = g_ed.file.routes.empty() ? -1 : 0;
			g_ed.point = -1;
			Save();
			Status("Deleted '" + label + "' and saved. Vehicles on it finish their lap and land.");
		}
		// Where a new point or spot goes up and down (2026-10-29, the user: the default height "should be zero or at the height of
		// the thing where it is placed and slightly above it"): it is placed at once at the baseline height, then a ray along the
		// cursor's line (main thread, GroundTick) finds what is under the cursor, terrain or a roof, and it moves there, kGroundLift
		// above it. No answer in kGroundWaitMs (cells not loaded, or the paused game runs no rays): the nav map's ground there, if known.
		constexpr double kGroundLift = 2.0;
		// The lowest a newly placed point or spot goes when the ground under it is unknown (2026-10-29 late, the user: "lowest point
		// when placed should be zero or at the height of the 3d spot we're pressing"). With the ground known the point goes to the
		// spot's own height (New Atlantis ground is about -160 m in places, so 0 is not a floor there).
		constexpr double kPlaceFloor = 0.0;
		// A vertical ray now (the map pauses the game: queued rays never answer there). 1 hit, 0 nothing, -1 not available.
		int DownRay(double x, double y, double fromZ, double toZ, double& hitZ)
		{
			const float a[3] = { float(x), float(y), float(fromZ) }, b[3] = { float(x), float(y), float(toZ) };
			prefetch::RayHit h;
			if (vehicle_probe::CastNow(a, b, 0x22806, h) != 1) return -1;
			if (!h.hit || !std::isfinite(h.z)) return 0;
			hitZ = h.z; return 1;
		}
		// Is (x, y, z) inside the ground? The surface seen from high above is over it AND nothing solid is below it (a point under a
		// bridge or a skywalk has ground below, so it is not refused). -1: rays not available.
		int Buried(double x, double y, double z, double& top)
		{
			const int above = DownRay(x, y, z + 1500.0, z - 1500.0, top);
			if (above < 0) return -1;
			if (above == 0 || top <= z + 0.5) return 0;
			double below = 0;
			const int under = DownRay(x, y, z, z - 1500.0, below);
			if (under < 0) return -1;
			return under == 0 ? 1 : 0;
		}
		// The terrain under (x, y): the cell's baked grid, else the nav map's top surface. False when neither knows it.
		bool TerrainAt(double x, double y, double z, double& ground)
		{
			if (cellbake::Ground(x, y, ground) && std::isfinite(ground)) return true;
			const auto top = navmap::At(x, y, z + 1000.0);
			if (top.haveGround && std::isfinite(top.ground)) { ground = top.ground; return true; }
			return false;
		}
		// A point or spot the editor refuses (2026-10-29 late, the user: the editor "should know when you placing a point that's bad
		// ... and should not accept those points"): below 0, under the terrain, or inside a mapped building or other solid.
		const char* PointProblem(double x, double y, double z)
		{
			double ground = 0;
			if (const int buried = Buried(x, y, z, ground); buried >= 0) { if (buried) return "under the ground"; }
			else if (TerrainAt(x, y, z, ground)) { if (z < ground + 1.0) return "under the ground"; }
			else if (z < kPlaceFloor - 0.01) return "below 0 with the ground there unknown";
			const auto m = navmap::At(x, y, z);
			if (m.blocked || m.objBlocked) return "inside a building or other solid";
			return nullptr;
		}
		void Refused(const char* what, double x, double y, double z, const char* why)
		{
			char b[200];
			std::snprintf(b, sizeof b, "%s refused: (%.0f, %.0f) at %.0f m is %s. Pick another spot or height.", what, x, y, z, why);
			Status(b);
			logger::Write("Route editor: %s", b);
		}
		constexpr ULONGLONG kGroundWaitMs = 3000;
		struct GroundAsk {
			int kind = 0;                 // 0 a route point, 1 a landing spot, 2 a spawn spot
			std::string id;               // the route's or the spot's id
			double px = 0, py = 0;        // where it was placed (it is found again by this)
			float from[3]{}, to[3]{};
			ULONGLONG at = 0;
			bool done = false, hit = false;
			const char* how = "";
			double x = 0, y = 0, z = 0;
			bool direct = false;          // the direct ray was tried (paused map)
		};
		std::mutex g_groundLock;
		std::vector<GroundAsk> g_groundAsks;
		void AskGround(int kind, const std::string& id, double px, double py, const routemap::Camera& c)
		{
			routemap::Vec3 o, d;
			if (!routemap::MouseRay(c, o, d) || !(d.z < -1e-6)) return;
			const double t = (c.focus.z - 1500.0 - o.z) / d.z;  // down to 1.5 km under the map's centre
			if (!std::isfinite(t) || t <= 0) return;
			GroundAsk a;
			a.kind = kind; a.id = id; a.px = px; a.py = py; a.at = GetTickCount64();
			a.from[0] = float(o.x); a.from[1] = float(o.y); a.from[2] = float(o.z);
			a.to[0] = float(o.x + t * d.x); a.to[1] = float(o.y + t * d.y); a.to[2] = float(o.z + t * d.z);
			std::lock_guard lk(g_groundLock);
			if (g_groundAsks.size() < 32) g_groundAsks.push_back(a);
		}
		// Map movie's thread: answered asks move their point or spot.
		void ApplyGround()
		{
			std::vector<GroundAsk> done;
			{
				std::lock_guard lk(g_groundLock);
				for (auto it = g_groundAsks.begin(); it != g_groundAsks.end();) {
					if (it->done) { done.push_back(*it); it = g_groundAsks.erase(it); }
					else ++it;
				}
			}
			for (const auto& a : done) {
				auto same = [&](double x, double y) { return std::fabs(x - a.px) < 0.01 && std::fabs(y - a.py) < 0.01; };
				const double z = a.z + kGroundLift;  // the spot's own height (the ground is known here)
				char b[160];
				if (a.kind == 0) {
					if (!g_ed.draft || g_ed.draft->Value().name != a.id) continue;
					auto r = g_ed.draft->Value();
					for (std::size_t i = 0; i < r.points.size(); ++i) {
						if (!same(r.points[i].x, r.points[i].y)) continue;
						if (!a.hit) {
							if (r.points[i].z < kPlaceFloor) { r.points[i].z = kPlaceFloor; g_ed.draft->Edit(r); }
							std::snprintf(b, sizeof b, "Point %zu: ground under the cursor unknown (paused map); height %.0f m (never below %.0f), set it with Z", i + 1, r.points[i].z, kPlaceFloor);
							Status(b); break;
						}
						r.points[i] = { a.x, a.y, z };
						g_ed.draft->Edit(r);
						std::snprintf(b, sizeof b, "Point %zu at (%.0f, %.0f), height %.0f m: %.0f m above the %s under the cursor", i + 1, a.x, a.y, z, kGroundLift, a.how);
						Status(b);
						break;
					}
					continue;
				}
				auto& list = a.kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				for (auto& s : list) {
					if (s.id != a.id || !same(s.at.x, s.at.y)) continue;
					if (!a.hit) {
						if (s.at.z < kPlaceFloor) s.at.z = kPlaceFloor;
						Status("Spot: ground under the cursor unknown (paused map); height " + std::to_string(int(s.at.z)) + " m (never below 0), set it with Z");
						break;
					}
					s.at = { a.x, a.y, z };
					std::snprintf(b, sizeof b, "%s spot at (%.0f, %.0f), height %.0f m: %.0f m above the %s under the cursor. Save to keep it.", a.kind == 1 ? "Landing" : "Spawn",
						a.x, a.y, z, kGroundLift, a.how);
					Status(b);
					break;
				}
			}
		}
		// The baseline height of new points: the selected point's, the last point's, else just over the map's centre (the map's
		// focus sits on the ground). The ground ask then moves the point onto what is under the cursor.
		double Baseline(const routemap::Camera& c)
		{
			trafficdata::Route scratch;
			if (const auto* r = Shown(scratch); r && !r->points.empty()) {
				if (g_ed.point >= 0 && g_ed.point < int(r->points.size())) return std::max(kPlaceFloor, r->points[std::size_t(g_ed.point)].z);
				return std::max(kPlaceFloor, r->points.back().z);
			}
			return std::max(kPlaceFloor, c.focus.z + kGroundLift);
		}
		void DropPoint(const routemap::Camera& c)
		{
			if (!g_ed.draft && g_ed.route < 0 && g_ed.other < 0) { NewRoute(); if (!g_ed.draft) return; }
			if (!EnsureDraft()) return;
			if (g_ed.draft->Value().ring) {  // a ring: Drop moves its centre to the cursor, at the ring's height
				auto r = g_ed.draft->Value();
				routemap::Vec3 at;
				if (!routemap::MouseAt(c, r.shape.z, at)) { Status("The cursor's ray does not reach the ring's height"); return; }
				r.shape.cx = at.x; r.shape.cy = at.y;
				g_ed.draft->Edit(r);
				char b[96]; std::snprintf(b, sizeof b, "Ring centre moved to (%.0f, %.0f)", at.x, at.y);
				Status(b); return;
			}
			auto r = g_ed.draft->Value();
			double z = Baseline(c);
			// A click on (or within 30 px of) a stretch of the route adds a point there, between its two points, at the height
			// in between (2026-10-29, the user: add points to a route "as opposed to having to delete and build again").
			std::size_t after = g_ed.point >= 0 && g_ed.point < int(r.points.size()) ? std::size_t(g_ed.point) + 1 : r.points.size();
			bool split = false;
			double mx = 0, my = 0;
			if (r.points.size() >= 2 && CursorLocal(c, mx, my)) {
				const std::size_t n = r.points.size(), segs = r.closed ? n : n - 1;
				double best = 30;
				for (std::size_t i = 0; i < segs; ++i) {
					const auto& a = r.points[i]; const auto& b = r.points[(i + 1) % n];
					double ax, ay, bx, by;
					if (!Local(c, { a.x, a.y, a.z }, ax, ay) || !Local(c, { b.x, b.y, b.z }, bx, by)) continue;
					const double d = SegmentDistance(mx, my, ax, ay, bx, by);
					if (d < best) {
						const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
						const double t = l2 > 1e-9 ? std::clamp(((mx - ax) * dx + (my - ay) * dy) / l2, 0.0, 1.0) : 0.5;
						best = d; after = i + 1; z = a.z + (b.z - a.z) * t; split = true;
					}
				}
			}
			routemap::Vec3 at;
			if (!routemap::MouseAt(c, z, at)) { Status("The cursor's ray does not reach that height; move the cursor or the camera"); return; }
			// The spot's own height when the terrain there is known (never under it), then the point check.
			if (double ground = 0; !split && (DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground))) {
				routemap::Vec3 lifted;
				if (routemap::MouseAt(c, ground + kGroundLift, lifted)) at = lifted; else at.z = ground + kGroundLift;
			}
			if (const char* why = PointProblem(at.x, at.y, at.z)) { Refused("New point", at.x, at.y, at.z, why); return; }
			const std::size_t oldCount = r.points.size();
            if(!r.rounding.empty()){r.rounding.resize(oldCount,0);r.rounding.insert(r.rounding.begin()+std::ptrdiff_t(after),0);}
			r.points.insert(r.points.begin() + std::ptrdiff_t(after), { at.x, at.y, at.z });
			g_ed.draft->Edit(r);
			LinksPointInserted(r.name, after, oldCount);
			g_ed.point = int(after);
			if (!split) AskGround(0, r.name, at.x, at.y, c);  // onto what is under the cursor (a point between two keeps their height)
			char b[128]; std::snprintf(b, sizeof b, "Point %d %s at (%.0f, %.0f), height %.0f m", g_ed.point + 1, split ? "added between two points" : "added",
				at.x, at.y, at.z);
			Status(b);
		}
		void AddSpot(const routemap::Camera& c, int kind)
		{
			routemap::Vec3 at;
			if (!routemap::MouseAt(c, c.focus.z, at)) { Status("The cursor's ray misses the ground plane; move the cursor"); return; }
			if (double ground = 0; DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground)) at.z = ground + kGroundLift;  // the spot's own height when known
			else at.z = std::max(kPlaceFloor, at.z);
			if (const char* why = PointProblem(at.x, at.y, at.z)) { Refused(kind == 1 ? "Landing spot" : "Spawn spot", at.x, at.y, at.z, why); return; }
			auto& list = kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
			std::set<std::string> taken;
			for (const auto& s : g_ed.file.landings) taken.insert(s.id);
			for (const auto& s : g_ed.file.spawns) taken.insert(s.id);
			routefiles::Spot s;
			s.id = routefiles::NewId(std::string(kind == 1 ? "landing " : "spawn ") + std::to_string(list.size() + 1), taken);
			s.at = { at.x, at.y, at.z };
			trafficdata::Vec shift;
			s.world = CityAt(at.x, at.y, shift);
			g_ed.spotShifts[s.id] = shift;
			list.push_back(s);
			g_ed.spotKind = kind; g_ed.spot = int(list.size()) - 1;
			AskGround(kind, s.id, at.x, at.y, c);
			Status(std::string(kind == 1 ? "Landing" : "Spawn") + " spot added; finding the ground under the cursor (" + std::to_string(int(at.z)) +
				" m for now). Save to keep it.");
		}
		void ChangeZ(double dz)
		{
			if (g_ed.spot >= 0) {
				auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				if (g_ed.spot < int(list.size())) {
					auto& at = list[std::size_t(g_ed.spot)].at;
					if (const char* why = PointProblem(at.x, at.y, at.z + dz)) { Refused("Spot height", at.x, at.y, at.z + dz, why); return; }
					at.z += dz; Status("Spot height " + std::to_string(int(at.z)) + " m (Save to keep)");
				}
				return;
			}
			if (!EnsureDraft()) return;
			if (g_ed.draft->Value().ring) {
				auto r = g_ed.draft->Value(); r.shape.z += dz; g_ed.draft->Edit(r);
				Status("Ring height " + std::to_string(int(r.shape.z)) + " m"); return;
			}
			const auto& pts = g_ed.draft->Value().points;
			if (g_ed.point < 0 || g_ed.point >= int(pts.size())) { Status("Select a point first (Select mode: click it; or Home / End)"); return; }
			{
				const auto& q = pts[std::size_t(g_ed.point)];
				if (const char* why = PointProblem(q.x, q.y, q.z + dz)) { Refused("Point height", q.x, q.y, q.z + dz, why); return; }
			}
			g_ed.draft->SetZ(std::size_t(g_ed.point), pts[std::size_t(g_ed.point)].z + dz);
			Status("Point " + std::to_string(g_ed.point + 1) + " height " + std::to_string(int(g_ed.draft->Value().points[std::size_t(g_ed.point)].z)) + " m");
		}
		// Sideways moves (2026-10-29, the user: buttons for moving a point "left and right too in x y and not only z"): along the
		// map view, so Left is left on screen whatever the camera's heading; Forward is away from the camera. The selected spot,
		// else the selected point, else an open ring's centre. Height is kept.
		void MoveXY(const routemap::Camera& c, double side, double ahead)
		{
			double fx = c.focus.x - c.eye.x, fy = c.focus.y - c.eye.y;
			const double l = std::hypot(fx, fy);
			if (l < 1e-3) { fx = 0; fy = 1; } else { fx /= l; fy /= l; }  // straight down: north is forward
			const double dx = side * fy + ahead * fx, dy = -side * fx + ahead * fy;
			char b[128];
			if (g_ed.spot >= 0) {
				auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				if (g_ed.spot < int(list.size())) {
					auto& at = list[std::size_t(g_ed.spot)].at;
					if (const char* why = PointProblem(at.x + dx, at.y + dy, at.z)) { Refused("Spot move", at.x + dx, at.y + dy, at.z, why); return; }
					at.x += dx; at.y += dy;
					std::snprintf(b, sizeof b, "Spot at (%.0f, %.0f), height %.0f m (Save to keep)", at.x, at.y, at.z);
					Status(b);
				}
				return;
			}
			if (!EnsureDraft()) return;
			auto r = g_ed.draft->Value();
			if (g_ed.point >= 0 && g_ed.point < int(r.points.size())) {
				auto& q = r.points[std::size_t(g_ed.point)];
				if (const char* why = PointProblem(q.x + dx, q.y + dy, q.z)) { Refused("Point move", q.x + dx, q.y + dy, q.z, why); return; }
				q.x += dx; q.y += dy;
				g_ed.draft->Edit(r);
				std::snprintf(b, sizeof b, "Point %d at (%.0f, %.0f), height %.0f m", g_ed.point + 1, q.x, q.y, q.z);
				Status(b);
			}
			else if (r.ring) {
				r.shape.cx += dx; r.shape.cy += dy;
				g_ed.draft->Edit(r);
				std::snprintf(b, sizeof b, "Ring centre at (%.0f, %.0f)", r.shape.cx, r.shape.cy);
				Status(b);
			}
			else Status("Select a point or a spot first (Select mode: click it; or Home / End for points)");
		}

		// ---- Junctions (2026-10-29): a branch's start or end joined to a point of another route ----
		constexpr double kDiamondLift = 18;  // px: the diamond floats above the junction, clear of the branch's end point
		// The line of a route by name (editor routes as saved, or the open draft; others read-only) and whether it is a ring.
		bool LineOf(const std::string& name, trafficroute::Line& line, bool& ring)
		{
			if (g_ed.draft && g_ed.draft->Value().name == name) { line = trafficroute::Build(g_ed.draft->Value()); ring = g_ed.draft->Value().ring; return true; }
			for (const auto& r : g_ed.file.routes) if (r.id == name && Usable(r)) { line = trafficroute::Build(ToTd(r, "")); ring = r.ring; return true; }
			for (const auto& o : g_ed.others) if (o.name == name) { line = o.line; ring = o.ring; return true; }
			return false;
		}
		trafficroute::Vec Middle(const trafficroute::Line& l, double u)
		{
			const auto a = trafficroute::At(l, 0, u), b = trafficroute::At(l, 1, u);
			return { (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2 };
		}
		bool JunctionAt(const routefiles::Link& k, trafficroute::Vec& at)
		{
			trafficroute::Line line; bool ring = false;
			if (!LineOf(k.from, line, ring)) return false;
			at = Middle(line, ring ? k.at / 360.0 * trafficroute::kRingSamples : line.ToLine(k.at));
			return true;
		}
		// The point of another route under the cursor: its name, u, and whether it is a ring.
		bool RouteUnderCursor(const routemap::Camera& c, double mx, double my, const std::string& skip, std::string& name, double& u, bool& ring)
		{
			double best = 16;
			auto test = [&](const std::string& n, const trafficroute::Line& l, bool isRing) {
				if (n == skip) return;
				const int count = l.Count(), segs = l.closed ? count : count - 1;
				for (int i = 0; i < segs; ++i) {
					const auto a = Middle(l, i), b = Middle(l, i + 1);
					double ax, ay, bx, by;
					if (!Local(c, { a.x, a.y, a.z }, ax, ay) || !Local(c, { b.x, b.y, b.z }, bx, by)) continue;
					const double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy;
					const double t = len > 1e-9 ? std::clamp(((mx - ax) * dx + (my - ay) * dy) / len, 0.0, 1.0) : 0.0;
					const double d = std::hypot(mx - (ax + t * dx), my - (ay + t * dy));
					if (d < best) { best = d; name = n; u = i + t; ring = isRing; }
				}
			};
			for (const auto& o : g_ed.others) test(o.name, o.line, o.ring);
			for (const auto& r : g_ed.file.routes) if (Usable(r)) test(r.id, trafficroute::Build(ToTd(r, "")), r.ring);
			return best < 16;
		}
		void Connect(const routemap::Camera& c, double mx, double my)
		{
			if (!g_ed.draft && g_ed.route < 0) NewRoute();
			if (!EnsureDraft()) return;
			if (g_ed.draft->Value().ring) { Status("Connect joins a path's end to another route; a ring cannot be the branch (connect a path to the ring instead)"); return; }
			const std::string me = g_ed.draft->Value().name;
			std::string other; double u = 0; bool ring = false;
			if (!RouteUnderCursor(c, mx, my, me, other, u, ring)) { Status("Connect: click on another route's line (grey or blue) where the branch should meet it"); return; }
			trafficroute::Line line; bool isRing = false;
			LineOf(other, line, isRing);
			// Anywhere along the route, exactly where clicked (2026-10-29, the user: it "snapped to only one part of the route"; the
			// old rounding to the nearest point of a path left long straight stretches with one place to join). Junctions turn at
			// any u (trafficlinks::Crossed), and the file keeps the fraction (at = 3.47).
			const auto at = Middle(line, u);
			auto r = g_ed.draft->Value();
			// The selected point joins (any point, 2026-10-29: "more than just 2"); with none selected, the nearer end.
			std::size_t idx = 0;
			if (r.points.empty()) r.points.push_back({ at.x, at.y, at.z });
			else {
				const std::size_t last = r.points.size() - 1;
				if (g_ed.point >= 0 && g_ed.point <= int(last)) idx = std::size_t(g_ed.point);
				else {
					const auto& f = r.points.front(); const auto& bk = r.points.back();
					idx = std::hypot(bk.x - at.x, bk.y - at.y) < std::hypot(f.x - at.x, f.y - at.y) ? last : 0;
				}
				r.points[idx] = { at.x, at.y, at.z };
			}
			const std::size_t last = r.points.size() - 1;
			const bool end = idx == last && last > 0;
			g_ed.draft->Edit(r);
			auto& links = g_ed.file.links;
			links.erase(std::remove_if(links.begin(), links.end(), [&](const routefiles::Link& k) {
				return k.to == me && (end ? k.toEnd : (!k.toEnd && std::lround(k.toAt) == long(idx))); }), links.end());  // that point's old junction
			std::set<std::string> taken;
			for (const auto& k : links) taken.insert(k.id);
			routefiles::Link k;
			k.id = routefiles::NewId("link " + std::to_string(links.size() + 1), taken);
			k.from = other; k.to = me; k.toEnd = end; k.toAt = end ? 0.0 : double(idx); k.share = 0.2;
			k.at = ring ? u / trafficroute::kRingSamples * 360.0 : line.ToSource(u);
			links.push_back(k);
			g_ed.link = int(links.size()) - 1; g_ed.point = int(idx); g_ed.spot = -1;
			const int joins = int(std::count_if(links.begin(), links.end(), [&](const routefiles::Link& o) { return o.to == me; }));
			Status("Joined point " + std::to_string(idx + 1) + (idx == 0 ? " (start)" : end ? " (end)" : "") + " of this route to " + other + " (" +
				std::to_string(joins) + " junction(s) on it). Vehicles turn in and out by the junction's share (Share - / +, now 20%); the far end of a branch "
				"is always taken. Select another point and Connect again for more.");
		}
		void ChangeShare(double d)
		{
			if (g_ed.link < 0 || g_ed.link >= int(g_ed.file.links.size())) { Status("Select a junction first (Select mode: click its diamond)"); return; }
			auto& k = g_ed.file.links[std::size_t(g_ed.link)];
			k.share = std::clamp(std::round((k.share + d) * 100.0) / 100.0, 0.0, 1.0);
			Status("Junction " + k.id + ": " + std::to_string(int(std::lround(k.share * 100))) + "% of laps passing it turn in (Save to keep)");
		}
		// The links of a route that was reversed: its start is now its end.
		void FlipLinks(const std::string& id)
		{
			std::size_t n = 0;
			if (g_ed.draft && g_ed.draft->Value().name == id) n = g_ed.draft->Value().points.size();
			for (auto& k : g_ed.file.links) {
				if (k.to != id) continue;
				if (k.toEnd) { k.toEnd = false; k.toAt = 0; }
				else if (std::lround(k.toAt) == 0) k.toEnd = true;
				else if (n) k.toAt = double(n - 1) - k.toAt;
			}
		}
		void LinksPointInserted(const std::string& id, std::size_t index, std::size_t oldCount)
		{
			for (auto& k : g_ed.file.links) {
				if (k.to != id) continue;
				if (k.toEnd && index >= oldCount && oldCount) { k.toEnd = false; k.toAt = double(oldCount - 1); }  // appended: the old end keeps it
				else if (!k.toEnd && k.toAt >= double(index)) k.toAt += 1;
			}
		}
		void LinksPointRemoved(const std::string& id, std::size_t index, std::size_t oldCount)
		{
			auto& links = g_ed.file.links;
			links.erase(std::remove_if(links.begin(), links.end(), [&](const routefiles::Link& k) {
				return k.to == id && ((k.toEnd && index + 1 == oldCount) || (!k.toEnd && std::lround(k.toAt) == long(index))); }), links.end());
			for (auto& k : links) if (k.to == id && !k.toEnd && k.toAt > double(index)) k.toAt -= 1;
			if (g_ed.link >= int(links.size())) g_ed.link = -1;
		}
		double FlowShare(const std::string& id)
		{
			double share = 0; bool any = false;
			for (const auto& k : g_ed.file.links) if (k.to == id && !k.toEnd) { share += k.share; any = true; }
			return any ? std::clamp(share, 0.1, 1.0) : 1.0;
		}

		void DeleteSelected()
		{
			if (g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size())) {
				g_ed.file.links.erase(g_ed.file.links.begin() + g_ed.link);
				g_ed.link = -1; Status("Junction removed (Save to keep)"); return;
			}
			if (g_ed.spot >= 0) {
				auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				if (g_ed.spot < int(list.size())) list.erase(list.begin() + g_ed.spot);
				// The next spot of the same kind stays selected, so Delete again keeps removing (2026-10-29: after one
				// removal nothing was selected and every further Delete said "Pick a route first").
				if (list.empty()) { g_ed.spot = -1; Status("Spot removed; no more " + std::string(g_ed.spotKind == 1 ? "landing" : "spawn") + " spots. Press Save to keep it."); }
				else {
					g_ed.spot = std::min(g_ed.spot, int(list.size()) - 1);
					Status("Spot removed. Next selected: " + list[std::size_t(g_ed.spot)].id + " (" + std::to_string(list.size()) +
						" left); Delete again removes it too. Press Save to keep the changes.");
				}
				return;
			}
			if (!g_ed.draft && (g_ed.route < 0 || g_ed.point < 0)) { Status("Nothing selected: in Select mode click a point, a spot (L / S) or a junction first"); return; }
			if (!EnsureDraft()) return;
			if (g_ed.point < 0 || g_ed.point >= int(g_ed.draft->Value().points.size())) { Status("Select a point first"); return; }
			const std::size_t oldCount = g_ed.draft->Value().points.size();
			g_ed.draft->Remove(std::size_t(g_ed.point));
			LinksPointRemoved(g_ed.draft->Value().name, std::size_t(g_ed.point), oldCount);
			g_ed.point = std::min(g_ed.point, int(g_ed.draft->Value().points.size()) - 1);
			Status("Point removed");
		}
		void EditRoute(void (*change)(trafficdata::Route&), const char* what)
		{
			if (!EnsureDraft()) return;
			auto r = g_ed.draft->Value();
			change(r);
			g_ed.draft->Edit(r);
			Status(what);
		}

		// ---- Drawing ----
		// Text is drawn as line strokes (strokefont.h): the movie's fonts drew boxes. Panel text is redrawn only when it changes.
		struct TextItem { std::string text; double x, y; unsigned color; std::size_t chars; bool map; };
		struct Button { int id; double x, y, w, h; std::string text; bool active, enabled; std::string key; };
		struct Ui {
			uptr movie = 0;
			bool built = false, failed = false;
			Value layer, lines, panel, cover, panelText, mapText;
			std::vector<TextItem> items;
			std::string panelSig;
			double width = 1920, height = 1080;
			// screen fraction -> our layer's space, learned from the cursor (both are known each frame)
			double ax = 1920, bx = 0, ay = 1080, by = 0;
			double panelBottom = 0;
			bool calibrated = false, calX = false, calY = false;
			double refScreenX = -1, refScreenY = -1, refLocalX = 0, refLocalY = 0;
			std::vector<Button> buttons;
		} g_ui;

		bool NewSprite(uptr movie, const Value& parent, Value& out, bool mouse)
		{
			if (!CreateObject(movie, &out, "flash.display.Sprite", nullptr, 0)) return false;
			SetMember(out, "mouseEnabled", Bool(mouse)); SetMember(out, "mouseChildren", Bool(false));
			const Value args[] = { Arg(out) };
			return Invoke(parent, "addChild", args, 1);
		}
		bool Build(uptr menu, uptr movie)
		{
			const auto& root = *reinterpret_cast<const Value*>(menu + 0x58);
			if (!IsObject(root)) return false;
			if (!NewSprite(movie, root, g_ui.layer, false)) return false;
			SetMember(g_ui.layer, "name", Str("FvoRouteEditor"));
			SetMember(g_ui.layer, "mouseChildren", Bool(true));
			// cover (absorbs map clicks while dropping / dragging), route lines, panel (absorbs clicks on the buttons), text
			if (!NewSprite(movie, g_ui.layer, g_ui.cover, true) || !NewSprite(movie, g_ui.layer, g_ui.lines, false) ||
				!NewSprite(movie, g_ui.layer, g_ui.panel, true) || !NewSprite(movie, g_ui.layer, g_ui.panelText, false) ||
				!NewSprite(movie, g_ui.layer, g_ui.mapText, false)) return false;
			Value stage;
			if (GetMember(root, "stage", &stage) && IsObject(stage)) { GetNumber(stage, "stageWidth", g_ui.width); GetNumber(stage, "stageHeight", g_ui.height); }
			Release(stage);
			g_ui.ax = g_ui.width; g_ui.ay = g_ui.height;
			logger::Write("Route editor: overlay added to the star map movie (stage %.0f x %.0f)", g_ui.width, g_ui.height);
			return true;
		}
		constexpr double kUnit = 1.5;       // px per stroke-font grid unit: letters 9 px high, 8.25 px apart
		constexpr double kButtonW = 124;    // a label of 11 letters and its key (Layout)
		constexpr double kLine = 16;        // px between text lines
		// Queues one line of text (top-left x, y), cut to `width`; map labels are redrawn every frame, panel text on change.
		void Text(uptr, const std::string& text, double x, double y, unsigned color, double width = 300, bool map = false)
		{
			g_ui.items.push_back({ text, x, y + 5, color, strokefont::Fit(width, kUnit), map });
		}
		struct Pen {
			Value g;
			bool ok = false;
			explicit Pen(const Value& sprite) { ok = GetMember(sprite, "graphics", &g) && IsObject(g); }
			~Pen() { Release(g); }
			Pen(const Pen&) = delete;
			Pen& operator=(const Pen&) = delete;
			void Clear() { if (ok) Invoke(g, "clear", nullptr, 0); }
			void Style(double w, unsigned c, double a) { const Value s[] = { Num(w), Num(c), Num(a) }; Invoke(g, "lineStyle", s, 3); }
			void NoLine() { Invoke(g, "lineStyle", nullptr, 0); }
			void Move(double x, double y) { const Value p[] = { Num(x), Num(y) }; Invoke(g, "moveTo", p, 2); }
			void Line(double x, double y) { const Value p[] = { Num(x), Num(y) }; Invoke(g, "lineTo", p, 2); }
			void Fill(unsigned c, double a) { const Value f[] = { Num(c), Num(a) }; Invoke(g, "beginFill", f, 2); }
			void End() { Invoke(g, "endFill", nullptr, 0); }
			void Rect(double x, double y, double w, double h) { const Value r[] = { Num(x), Num(y), Num(w), Num(h) }; Invoke(g, "drawRect", r, 4); }
			void Circle(double x, double y, double r) { const Value c[] = { Num(x), Num(y), Num(r) }; Invoke(g, "drawCircle", c, 3); }
		};
		void DrawText(Pen& pen, const TextItem& t, bool outline)
		{
			const auto lines = strokefont::Layout(t.text, t.x, t.y, kUnit, t.chars);
			for (int pass = outline ? 0 : 1; pass < 2; ++pass) {
				pen.Style(pass ? 1.4 : 3.6, pass ? t.color : 0x0B1620, pass ? 1.0 : 0.85);
				for (const auto& l : lines) {
					pen.Move(l[0].x, l[0].y);
					if (l.size() == 1) pen.Line(l[0].x + 0.5, l[0].y);
					for (std::size_t i = 1; i < l.size(); ++i) pen.Line(l[i].x, l[i].y);
				}
			}
		}
		// End of a frame: map labels always (they move with the camera), panel text only when it changed.
		void FlushText()
		{
			std::string sig;
			for (const auto& t : g_ui.items)
				if (!t.map) { sig += t.text; sig += '\x1f'; sig += std::to_string(int(t.x)) + "," + std::to_string(int(t.y)) + "," + std::to_string(t.color) + ";"; }
			{
				Pen map(g_ui.mapText);
				map.Clear();
				for (const auto& t : g_ui.items) if (t.map) DrawText(map, t, true);
			}
			if (sig != g_ui.panelSig) {
				Pen panel(g_ui.panelText);
				panel.Clear();
				for (const auto& t : g_ui.items) if (!t.map) DrawText(panel, t, false);
				g_ui.panelSig = sig;
			}
			g_ui.items.clear();
		}
		bool CursorLocal(const routemap::Camera& c, double& x, double& y)
		{
			if (!(c.width > 0) || !(c.height > 0)) return false;
			x = g_ui.ax * (c.mouseX / c.width) + g_ui.bx; y = g_ui.ay * (c.mouseY / c.height) + g_ui.by;
			return true;
		}
		bool Local(const routemap::Camera& c, routemap::Vec3 p, double& x, double& y)
		{
			double x01, y01, depth;
			if (!routemap::Screen01(c, p, x01, y01, depth)) return false;
			x = g_ui.ax * x01 + g_ui.bx; y = g_ui.ay * (1.0 - y01) + g_ui.by;
			return std::isfinite(x) && std::isfinite(y) && std::fabs(x) < 1e5 && std::fabs(y) < 1e5;
		}
		// Our layer's cursor and the engine's cursor: two samples far enough apart on an axis give its scale and offset.
		void CalibrateAxis(double screen, double local, double size, double& refS, double& refL, double& a, double& b, bool& done, const char* axis)
		{
			if (refS < 0) { refS = screen; refL = local; return; }
			const double ds = screen - refS;
			if (std::fabs(ds) < 0.15) return;
			const double na = (local - refL) / ds, nb = local - na * screen;
			refS = screen; refL = local;
			if (!(na > 0.6 * size && na < 1.7 * size)) return;
			if (!done || std::fabs(na - a) > 2 || std::fabs(nb - b) > 2)
				logger::Write("Route editor: screen -> overlay %s = %.1f s + %.1f (stage size %.0f)", axis, na, nb, size);
			a = na; b = nb; done = true;
		}
		void Calibrate(const routemap::Camera& c, double lx, double ly)
		{
			if (!(c.width > 0) || !(c.height > 0)) return;
			CalibrateAxis(c.mouseX / c.width, lx, g_ui.width, g_ui.refScreenX, g_ui.refLocalX, g_ui.ax, g_ui.bx, g_ui.calX, "x");
			CalibrateAxis(c.mouseY / c.height, ly, g_ui.height, g_ui.refScreenY, g_ui.refLocalY, g_ui.ay, g_ui.by, g_ui.calY, "y");
			g_ui.calibrated = g_ui.calX && g_ui.calY;
		}

		// ---- Parking (2026-10-29, the user: vehicles at the landing spots; spawned, "find a spawn approach that works best") ----
		// Parking marks every landing spot parked = 1 and saves. The traffic pool then keeps one REV-8 at each while the player is
		// near (made once per save, one at a time, hidden when far and shown again) and gives it a driver when a route needs one.
		void SetParking()
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			if (g_ed.file.landings.empty()) { Status("No landing spots: add some with Landing first"); return; }
			int marked = 0;
			for (auto& l : g_ed.file.landings) if (!l.parked) { l.parked = true; ++marked; }
			Save();
			Status("Parking: a vehicle stands at each of the " + std::to_string(g_ed.file.landings.size()) + " landing spot(s) (P) while you are near, placed "
				"one at a time; traffic gives it a driver when a route needs one." + (marked ? "" : " (They were all parking already.)"));
		}

		// Start / swap the traffic model (2026-10-29, the user: "a button that will cold start traffic ... a UI element that says
		// loading and won't allow me to leave the screen of map whilst cold starting"). Every landing spot becomes a parking spot
		// (saved), then the pool restarts: each spot's pair placed or moved back onto its spot, trips ended. The loading panel
		// stays until that is done (the game is paused on the map: vehicles and drivers are placed now, they drive once it closes).
		// Where the cold-start vehicles go (2026-10-29, the user: "spawn in the middle of traffic not now entering"): points on
		// the lanes of every route active now (planet local time), as many per route as its vehicle count (6 at most), spread
		// evenly along it and alternating lanes, each facing its lane's direction of travel; routes taken in turn so the first
		// few vehicles are spread over all of them.
		std::vector<trafficpool::RushPlace> LanePlaces()
		{
			double h = 0;
			const bool known = LocalHours(h);
			const int minute = known ? std::clamp(int(h * 60.0), 0, routeschedule::kMinutesPerDay - 1) : 0;
			std::vector<trafficdata::Route> routes;
			for (const auto& r : g_ed.file.routes) if (Usable(r)) routes.push_back(ToTd(r, EditorKey()));
			for (const auto& o : g_ed.others) routes.push_back(o.route);
			std::vector<std::vector<trafficpool::RushPlace>> per;
			for (const auto& r : routes) {
				const auto d = routeschedule::Evaluate(r.schedule, known, minute);
				if (!d.active) continue;
				const auto line = trafficroute::Build(r);
				const double span = line.Span();
				if (line.Count() < 2 || span <= 0) continue;
				const int lanes = std::clamp(r.lanes, 1, 2), n = std::clamp(d.vehicles, 1, fvolimits::kVehicles);  // the route's own count (2026-10-29: was 6 at most)
				std::vector<trafficpool::RushPlace> list;
				for (int j = 0; j < n; ++j) {
					const int lane = j % lanes;
					const double u = span * (j + 0.5) / n, dir = lane == 1 ? -1.0 : 1.0;
					const auto a = trafficroute::At(line, lane, u), b = trafficroute::At(line, lane, u + dir * 0.25);
					list.push_back({ a.x, a.y, a.z, std::atan2(b.x - a.x, b.y - a.y) });
				}
				per.push_back(list);
			}
			std::vector<trafficpool::RushPlace> out;
			for (std::size_t k = 0;; ++k) {
				bool any = false;
				for (const auto& list : per) if (k < list.size()) { out.push_back(list[k]); any = true; }
				if (!any) break;
			}
			return out;
		}

		void StartSwap()
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			// The update runs the world behind the panel, so it is never started in combat (2026-10-29, the user).
			if (secretunpause::PlayerInCombat()) {
				Status("In combat: traffic can't be updated. Exit combat first.");
				notify::Show("In combat: traffic can't be updated. Exit combat first.");
				logger::Write("Route editor: start / swap refused, the player is in combat");
				secretunpause::RefusedForCombat();
				return;
			}
			bool changed = false;
			for (auto& l : g_ed.file.landings) if (!l.parked) { l.parked = true; changed = true; }
			if (changed) Save();
			const auto places = LanePlaces();
			trafficpool::Rush(places.data(), int(places.size()));
			logger::Write("Route editor: start / swap: %zu lane place(s) on the active routes", places.size());
			g_ed.loading = true;
			g_ed.loadingAt = GetTickCount64();
			g_ed.filledAt = 0;
			// The world runs behind the panel so placed traffic is checked, seated and armed (never in combat).
			secretunpause::Begin();
			logger::Write("Route editor: start / swap traffic model (%zu landing spot(s) as parking)", g_ed.file.landings.size());
		}

		enum Id {
			kNew = 1, kSave, kCancel, kDeleteRoute, kPrevRoute, kNextRoute, kSelect, kDrop, kDrag, kLanding, kSpawn,
			kZUp, kZDown, kReverse, kDeletePoint, kLoop, kUndo, kRedo, kWays, kSpeedDown, kSpeedUp, kVehDown, kVehUp,
			kSchedule, kStartDown, kStartUp, kEndDown, kEndUp, kTest, kTimeDown, kTimeUp, kTimeLive, kClose, kConnect, kShareDown, kShareUp,
			kEditOriginal, kRadiusDown, kRadiusUp, kBake, kMoveLeft, kMoveRight, kMoveForward, kMoveBack, kStartSwap, kRoundPoint, kRoundDown, kRoundUp
		};
		// Keyboard shortcuts (2026-10-29, the user: "keyboard shortcuts for the traffic menu controls and they should show beside them").
		// Letters the map does not use while the editor holds it (inputgate: only Tab, Esc, WASD and the arrows reach the map); never
		// number keys or the in-game test keys , . / '. Shown at the right of each button; Ctrl and the older keys (Insert, Delete,
		// PgUp / PgDn, [ ], Home / End) keep working.
		struct Shortcut { int vk; int id; const char* label; };
		const Shortcut kShortcuts[] = {
			{ 'Q', kSelect, "Q" }, { 'E', kDrop, "E" }, { 'R', kDrag, "R" }, { 'C', kConnect, "C" }, { 'G', kLanding, "G" },
			{ 'N', kNew, "N" }, { 'O', kPrevRoute, "O" }, { 'P', kNextRoute, "P" }, { 'X', kStartSwap, "X" },
			{ 'I', kMoveForward, "I" }, { 'J', kMoveLeft, "J" }, { 'K', kMoveBack, "K" }, { 'L', kMoveRight, "L" },
			{ 'V', kReverse, "V" }, { 'B', kLoop, "B" }, { 'H', kWays, "H" }, { 'T', kTest, "T" },
			{ VK_OEM_MINUS, kVehDown, "-" }, { VK_OEM_PLUS, kVehUp, "=" },
		};
		const char* KeyOf(int id)
		{
			for (const auto& k : kShortcuts) if (k.id == id) return k.label;
			switch (id) {
			case kSave: return "Ctrl+S"; case kUndo: return "Ctrl+Z"; case kRedo: return "Ctrl+Y";
			case kZUp: return "PgU"; case kZDown: return "PgD"; case kDeletePoint: return "Del";
			default: return "";
			}
		}
		void Layout(const trafficdata::Route* shown)
		{
			auto& b = g_ui.buttons;
			b.clear();
			const double x0 = PanelX() + 12, w = kButtonW, h = 24, gap = 6;
			double y = 92;
			auto row = [&](std::initializer_list<std::pair<int, const char*>> items, std::initializer_list<bool> active = {}, std::initializer_list<bool> enabled = {}) {
				double x = x0; std::size_t i = 0;
				for (const auto& [id, text] : items) {
					const bool a = i < active.size() && active.begin()[i];
					const bool e = i >= enabled.size() || enabled.begin()[i];
					b.push_back({ id, x, y, w, h, text, a, e, KeyOf(id) });
					x += w + gap; ++i;
				}
				y += h + gap;
			};
			const bool draft = bool(g_ed.draft), route = shown != nullptr, ring = shown && shown->ring;
			row({ {kNew, "New route"}, {kSave, "Save"}, {kCancel, "Cancel"} }, {}, { !draft, true, true });
			row({ {kPrevRoute, "< Route"}, {kNextRoute, "Route >"}, {kDeleteRoute, "Del route"} });
			const Mode m = g_ed.mode;
			row({ {kSelect, "Select"}, {kDrop, "Drop"}, {kDrag, "Drag"} }, { m == Mode::Select, m == Mode::Drop, m == Mode::Drag });
			const bool pickedLink = g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size());
			row({ {kZUp, "Z up"}, {kZDown, "Z down"}, {kDeletePoint, "Del point"} }, {}, { route || g_ed.spot >= 0, route || g_ed.spot >= 0, route || g_ed.spot >= 0 || pickedLink });
			const bool movable = route || g_ed.spot >= 0;
			row({ {kMoveLeft, "Left"}, {kMoveRight, "Right"}, {kMoveForward, "Forward"} }, {}, { movable, movable, movable });
			row({ {kMoveBack, "Back"} }, {}, { movable });
			row({ {kReverse, "Reverse"}, {kLoop, shown && shown->closed ? "Loop: on" : "Loop: off"}, {kWays, shown && shown->lanes > 1 ? "Two-way" : "One-way"} },
				{ false, shown && shown->closed, shown && shown->lanes > 1 }, { route && !ring, route && !ring, route });
			row({ {kEditOriginal, "Edit this"}, {kRadiusDown, "Radius -"}, {kRadiusUp, "Radius +"} }, {},
				{ !draft && g_ed.route < 0 && g_ed.other >= 0, ring, ring });
			row({ {kUndo, "Undo"}, {kRedo, "Redo"}, {kTest, "Test route"} }, {}, { draft, draft, route });
			row({ {kSpeedDown, "Speed -"}, {kSpeedUp, "Speed +"}, {kSchedule, shown && !shown->schedule.allDay ? "Scheduled" : "All day"} },
				{ false, false, shown && !shown->schedule.allDay }, { route, route, route });
			row({ {kVehDown, "Vehicles -"}, {kVehUp, "Vehicles +"} }, {}, { route, route });
			if (shown && !shown->schedule.allDay) {
				row({ {kStartDown, "Start -1h"}, {kStartUp, "Start +1h"} });
				row({ {kEndDown, "End -1h"}, {kEndUp, "End +1h"} });
			}
			// Start / swap the traffic model replaces the Spawn and Parking buttons (2026-10-29, the user): it makes every landing
			// spot a parking spot and cold-starts the whole model at once.
			row({ {kLanding, "Landing"}, {kStartSwap, "Start/Swap"} }, { m == Mode::Landing, g_ed.loading }, { true, !draft });
			const bool link = g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size());
			row({ {kConnect, "Connect"}, {kShareDown, "Share -"}, {kShareUp, "Share +"} }, { m == Mode::Connect }, { true, link, link });
            const bool roundable=link||(route&&!ring&&g_ed.point>=0);
            const double rounding=link?g_ed.file.links[std::size_t(g_ed.link)].radius:
                shown&&g_ed.point>=0&&std::size_t(g_ed.point)<shown->rounding.size()?shown->rounding[std::size_t(g_ed.point)]:0;
            row({{kRoundPoint,rounding>0?"Round: on":"Round: off"},{kRoundDown,"Round R -"},{kRoundUp,"Round R +"}}, {rounding>0}, {roundable,roundable,roundable});
			row({ {kTimeDown, "Time -1h"}, {kTimeUp, "Time +1h"}, {kTimeLive, "Live time"} }, { false, false, g_ed.previewHour < 0 });
		}
		const Button* ButtonAt(double x, double y)
		{
			for (const auto& b : g_ui.buttons) if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return &b;
			return nullptr;
		}
		// The panel sits on the right (2026-10-29, the user: the map's own planet panel is on the left).
		constexpr double kPanelW = 3 * (kButtonW + 6) + 18;
		double PanelX() { return g_ui.width - kPanelW - 12; }
		bool OverPanel(double x, double y) { return x >= PanelX() && x <= PanelX() + kPanelW && y >= 12 && y <= g_ui.panelBottom; }

		// Schedule edits on the open draft: one window (start, end, vehicles) or all day.
		routeschedule::Window& FirstWindow(trafficdata::Route& r)
		{
			if (r.schedule.windows.empty()) r.schedule.windows.push_back({ 6 * 60, 10 * 60, std::max(1, r.schedule.allDayVehicles) });
			return r.schedule.windows.front();
		}
		void Press(int id, const routemap::Camera& c)
		{
			switch (id) {
			case kNew: NewRoute(); break;
			case kSave: Save(); break;
			case kCancel: Cancel(); break;
			case kDeleteRoute: DeleteRoute(); break;
			case kPrevRoute: case kNextRoute: {
				// Editor routes first, then the originals of other files (grey): every route can be picked without a precise
				// click (2026-10-29, the user: "I'm not able to modify the original traffic"; the log showed no original selected).
				if (g_ed.draft) { Status("Save or Cancel the open route first"); break; }
				const int mine = int(g_ed.file.routes.size()), total = mine + int(g_ed.others.size());
				if (!total) { Status("No routes in this world yet: press New route"); break; }
				int at = g_ed.route >= 0 ? g_ed.route : g_ed.other >= 0 ? mine + g_ed.other : (id == kNextRoute ? -1 : 0);
				at = (at + (id == kNextRoute ? 1 : total - 1) + total) % total;
				g_ed.point = -1; g_ed.spot = -1; g_ed.link = -1;
				if (at >= mine) {
					g_ed.route = -1; g_ed.other = at - mine;
					const auto& o = g_ed.others[std::size_t(g_ed.other)];
					Status("Original route '" + o.name + "' from " + o.file + " (white): Edit this, or any change, makes your version; Del route removes it");
					break;
				}
				g_ed.route = at; g_ed.other = -1;
				const auto& r = g_ed.file.routes[std::size_t(g_ed.route)];
				if (r.removed) Status("Removed original '" + r.id + "': Del route brings it back");
				else if (g_ed.unplaced.count(r.id)) Status("Route '" + r.label + "' belongs to a city not found in this world now; it is kept unchanged");
				else Status("Route '" + r.label + "'" + (IsOriginal(r) ? " (your version of an original; Del route reverts it)" : ""));
				break;
			}
			case kSelect: g_ed.mode = Mode::Select; Status("Select: click a point or a route"); break;
			case kDrop: g_ed.mode = Mode::Drop; Status("Drop: click the map to add a point after the selected one (Insert does the same in any mode)"); break;
			case kDrag: g_ed.mode = Mode::Drag; Status("Drag: hold the left button on a point and move it; it keeps its height"); break;
			case kLanding: g_ed.mode = Mode::Landing; Status("Landing spot: click where vehicles may land between laps"); break;
			case kSpawn: g_ed.mode = Mode::Spawn; Status("Spawn spot: click flat open ground where a vehicle may start (cold start)"); break;
			case kZUp: ChangeZ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 25 : 5); break;
			case kZDown: ChangeZ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? -25 : -5); break;
			case kDeletePoint: DeleteSelected(); break;
			case kReverse:
				if (EnsureDraft()) {
					if (g_ed.draft->Reverse()) {
						FlipLinks(g_ed.draft->Value().name);
						const int n = int(g_ed.draft->Value().points.size());
						if (g_ed.point >= 0) g_ed.point = n - 1 - g_ed.point;
						Status("Reversed: traffic now flies the other way (watch the dots)");
					}
					else Status("A route needs two points to reverse");
				}
				break;
			case kLoop:
				if (EnsureDraft()) { g_ed.draft->Close(!g_ed.draft->Value().closed); Status(g_ed.draft->Value().closed ? "Loop: the last point joins the first" : "Open route: vehicles turn back at the ends"); }
				break;
			case kWays: EditRoute([](trafficdata::Route& r) { r.lanes = r.lanes > 1 ? 1 : 2; }, "Lanes changed (two-way: a second lane runs the other way, 'width' apart)"); break;
			case kUndo: if (g_ed.draft && g_ed.draft->Undo()) Status("Undone"); break;
			case kRedo: if (g_ed.draft && g_ed.draft->Redo()) Status("Redone"); break;
			case kSpeedDown: EditRoute([](trafficdata::Route& r) { r.speed = std::max(5.0, r.speed - 5); }, "Speed lowered"); break;
			case kSpeedUp: EditRoute([](trafficdata::Route& r) { r.speed = std::min(80.0, r.speed + 5); }, "Speed raised"); break;
			case kVehDown: case kVehUp: {
				const int d = id == kVehUp ? 1 : -1;
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				if (r.schedule.allDay) r.schedule.allDayVehicles = std::clamp(r.schedule.allDayVehicles + d, 0, fvolimits::kVehicles);
				else FirstWindow(r).vehicles = std::clamp(FirstWindow(r).vehicles + d, 0, fvolimits::kVehicles);
				g_ed.draft->Edit(r);
				Status("Vehicles: " + ScheduleText(r.schedule));
				break;
			}
			case kSchedule: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				r.schedule.allDay = !r.schedule.allDay;
				if (!r.schedule.allDay) FirstWindow(r);
				g_ed.draft->Edit(r);
				Status(r.schedule.allDay ? "All day" : "Scheduled (planet local time): " + ScheduleText(r.schedule));
				break;
			}
			case kStartDown: case kStartUp: case kEndDown: case kEndUp: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				auto& w = FirstWindow(r);
				int& t = (id == kStartDown || id == kStartUp) ? w.start : w.end;
				t = (t + (id == kStartUp || id == kEndUp ? 60 : -60) + routeschedule::kMinutesPerDay) % routeschedule::kMinutesPerDay;
				if (w.start == w.end) t = (t + 60) % routeschedule::kMinutesPerDay;
				g_ed.draft->Edit(r);
				Status("Scheduled: " + ScheduleText(r.schedule) + " (windows may cross midnight)");
				break;
			}
			case kTest:
				if (EnsureDraft()) {
					auto r = g_ed.draft->Value(); r.test = true; g_ed.draft->Edit(r);
					Save();
					if (!g_ed.draft) Status("Test requested: the next free vehicle with a driver near the route flies it once (see the log's 'test route' lines)");
				}
				break;
			case kTimeDown: case kTimeUp: {
				double h = g_ed.previewHour;
				if (h < 0 && !LocalHours(h)) h = 12;
				g_ed.previewHour = std::fmod(std::floor(h) + (id == kTimeUp ? 1 : 23), 24.0);
				Status("Preview time " + Clock(int(g_ed.previewHour * 60)) + " (the game's clock is not changed)");
				break;
			}
			case kTimeLive: g_ed.previewHour = -1; Status("Preview follows the planet's local time"); break;
			case kBake: SetParking(); break;
			case kStartSwap: StartSwap(); break;
			case kMoveLeft: case kMoveRight: case kMoveForward: case kMoveBack: {
				const double step = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 25 : 5;
				MoveXY(c, id == kMoveLeft ? -step : id == kMoveRight ? step : 0, id == kMoveForward ? step : id == kMoveBack ? -step : 0);
				break;
			}
			case kEditOriginal:
				if (g_ed.draft) Status("Save or Cancel the open route first");
				else if (g_ed.other < 0) Status("Select a grey route first (Select mode: click its line)");
				else EditOriginal();
				break;
			case kRadiusDown: case kRadiusUp:
				if (EnsureDraft()) {
					auto r = g_ed.draft->Value();
					if (!r.ring) { Status("Radius is for rings"); break; }
					r.shape.radius = std::max(20.0, r.shape.radius + (id == kRadiusUp ? 10 : -10));
					g_ed.draft->Edit(r);
					Status("Ring radius " + std::to_string(int(r.shape.radius)) + " m");
				}
				break;
			case kConnect: g_ed.mode = Mode::Connect; Status("Connect: click another route's line to join the nearer end of this route to it (the selected end if one is selected)"); break;
            case kRoundPoint:case kRoundDown:case kRoundUp:{
                const bool junction=g_ed.link>=0&&g_ed.link<int(g_ed.file.links.size());
                double current=0;
                if(junction)current=g_ed.file.links[std::size_t(g_ed.link)].radius;
                else {
                    if(!EnsureDraft()||g_ed.point<0||g_ed.point>=int(g_ed.draft->Value().points.size())){Status("Select a route corner or a junction diamond first");break;}
                    const auto& radii=g_ed.draft->Value().rounding;
                    if(std::size_t(g_ed.point)<radii.size())current=radii[std::size_t(g_ed.point)];
                }
                const double radius=id==kRoundPoint?(current>0?0:g_ed.roundRadius):std::clamp((current>0?current:g_ed.roundRadius)+(id==kRoundUp?5:-5),5.0,200.0);
                if(junction)g_ed.file.links[std::size_t(g_ed.link)].radius=radius;
                else if(!g_ed.draft->Round(std::size_t(g_ed.point),radius)){Status("Rounding needs an interior corner (or a loop point); endpoints use their junction diamond");break;}
                if(radius>0)g_ed.roundRadius=radius;
                Status(std::string(junction?"Junction":"Point")+(radius>0?" rounded, radius "+std::to_string(int(radius))+" m (limited by available segment length)":" sharp")+"; Save to apply. Existing trips finish before updating.");break;
            }
			case kShareDown: ChangeShare(-0.05); break;
			case kShareUp: ChangeShare(0.05); break;
			default: break;
			}
			(void)c;
		}

		// The nearest point of the shown route to the cursor, in overlay pixels.
		int PointAt(const routemap::Camera& c, double mx, double my, double radius)
		{
			trafficdata::Route scratch;
			const auto* r = Shown(scratch);
			if (!r) return -1;
			int best = -1; double bestD = radius;
			for (std::size_t i = 0; i < r->points.size(); ++i) {
				double x, y;
				if (!Local(c, { r->points[i].x, r->points[i].y, r->points[i].z }, x, y)) continue;
				const double d = std::hypot(x - mx, y - my);
				if (d < bestD) { bestD = d; best = int(i); }
			}
			return best;
		}
		double SegmentDistance(double px, double py, double ax, double ay, double bx, double by)
		{
			const double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
			const double t = l > 1e-9 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / l, 0.0, 1.0) : 0.0;
			return std::hypot(px - (ax + t * dx), py - (ay + t * dy));
		}
		void ClickMap(const routemap::Camera& c, double mx, double my)
		{
			switch (g_ed.mode) {
			case Mode::Drop: DropPoint(c); return;
			case Mode::Landing: AddSpot(c, 1); return;
			case Mode::Spawn: AddSpot(c, 2); return;
			case Mode::Drag: return;  // handled on press / release
			case Mode::Connect: Connect(c, mx, my); return;
			default: break;
			}
			// Select: a junction first (its diamond).
			for (std::size_t i = 0; i < g_ed.file.links.size(); ++i) {
				trafficroute::Vec at; double x, y;
				if (JunctionAt(g_ed.file.links[i], at) && Local(c, { at.x, at.y, at.z }, x, y) && std::hypot(x - mx, y - kDiamondLift - my) < 10) {
					const auto& k = g_ed.file.links[i];
					g_ed.link = int(i); g_ed.spot = -1;
					Status("Junction " + k.from + " -> " + k.to + " (" + (k.toEnd ? std::string("end") : k.toAt > 0 ? "point " + std::to_string(std::lround(k.toAt) + 1) : std::string("start")) + "): " + std::to_string(int(std::lround(k.share * 100))) + "% turn in. Share - / +, Del point removes it");
					return;
				}
			}
			g_ed.link = -1;
			// Select: a point of the shown route, else a spot, else a route.
			if (const int p = PointAt(c, mx, my, 14); p >= 0) {
				g_ed.point = p; g_ed.spot = -1;
				trafficdata::Route scratch;
				const auto& q = Shown(scratch)->points[std::size_t(p)];
				char b[96]; std::snprintf(b, sizeof b, "Point %d: (%.0f, %.0f), height %.0f m", p + 1, q.x, q.y, q.z);
				Status(b); return;
			}
			for (int kind = 1; kind <= 2; ++kind) {
				const auto& list = kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				for (std::size_t i = 0; i < list.size(); ++i) {
					double x, y;
					if (SpotHere(list[i]) && Local(c, { list[i].at.x, list[i].at.y, list[i].at.z }, x, y) && std::hypot(x - mx, y - my) < 14) {
						g_ed.spotKind = kind; g_ed.spot = int(i); g_ed.point = -1;
						Status(std::string(kind == 1 ? "Landing" : "Spawn") + " spot " + list[i].id + ": Z up / Z down set its height, Del point removes it");
						return;
					}
				}
			}
			if (g_ed.draft) return;  // keep the open route selected
			auto onLine = [&](const trafficroute::Line& l) {
				const int count = l.Count(), segs = l.closed ? count : count - 1;
				for (int i = 0; i < segs; ++i) {
					const auto a = Middle(l, i), b = Middle(l, i + 1);
					double ax, ay, bx, by;
					if (Local(c, { a.x, a.y, a.z }, ax, ay) && Local(c, { b.x, b.y, b.z }, bx, by) && SegmentDistance(mx, my, ax, ay, bx, by) < 10) return true;
				}
				return false;
			};
			for (std::size_t k = 0; k < g_ed.file.routes.size(); ++k) {
				const auto& r = g_ed.file.routes[k];
				if (!Usable(r) || !onLine(trafficroute::Build(ToTd(r, EditorKey())))) continue;
				g_ed.route = int(k); g_ed.other = -1; g_ed.point = -1; g_ed.spot = -1;
				Status("Route '" + r.label + "'" + (IsOriginal(r) ? " (your version of an original; Del route reverts it)" : ""));
				return;
			}
			for (std::size_t k = 0; k < g_ed.others.size(); ++k) {
				if (!onLine(g_ed.others[k].line)) continue;
				g_ed.other = int(k); g_ed.route = -1; g_ed.point = -1; g_ed.spot = -1;
				Status("Original route '" + g_ed.others[k].name + "' from " + g_ed.others[k].file + ": Edit this (or any change) makes your editable version; Del route removes it");
				return;
			}
			g_ed.point = -1; g_ed.spot = -1; g_ed.other = -1;
			Status(std::string("Nothing here") + (g_ed.mode == Mode::Select ? "" : " (Select mode picks routes)") +
				": click on a line, or use < Route / Route > (it also goes through the grey routes of other files)");
		}

		bool Focused()
		{
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			return pid == GetCurrentProcessId();
		}
		struct Keys {
			bool down[256]{};
			bool Pressed(int vk, bool focused)
			{
				const bool held = focused && (GetAsyncKeyState(vk) & 0x8000) != 0;
				const bool fresh = held && !down[vk];
				down[vk] = held;
				return fresh;
			}
		} g_keys;

		void DrawRoute(Pen& pen, const routemap::Camera& c, const trafficroute::Line& line, unsigned color, double width, double alpha)
		{
			const int n = line.Count();
			if (n < 2) return;
			pen.Style(width, color, alpha);
			// the middle of the two lanes
			bool down = false;
			const int count = line.closed ? n + 1 : n;
			for (int i = 0; i < count; ++i) {
				const auto& a = line.lane[0][std::size_t(i % n)]; const auto& b = line.lane[1][std::size_t(i % n)];
				double x, y;
				if (!Local(c, { (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2 }, x, y)) { down = false; continue; }
				if (down) pen.Line(x, y); else pen.Move(x, y);
				down = true;
			}
		}
		// Moving dots along each lane in its direction of travel (a preview, not the real vehicles).
		void DrawFlow(Pen& pen, const routemap::Camera& c, const trafficroute::Line& line, int lanes, double speed, unsigned c0, unsigned c1, double share = 1.0)
		{
			const double length = trafficroute::Length(line), span = line.Span();
			if (length < 1 || span <= 0) return;
			const double spacing = std::max(40.0, length / 24.0) / share;  // a branch fed by junctions shows thinner flow
			const int dots = std::max(1, int(length / spacing));
			const double shift = std::fmod(g_ed.phase * speed * 2.0, spacing) / length * span;  // twice real speed reads better
			for (int lane = 0; lane < lanes; ++lane) {
				pen.NoLine();
				pen.Fill(lane ? c1 : c0, 0.95);
				for (int k = 0; k < dots; ++k) {
					double u = span * k / dots + (lane ? -shift : shift);
					if (!line.closed) { u = std::fmod(u + span * 4, span); }
					const auto p = trafficroute::At(line, lane, u);
					double x, y;
					if (Local(c, { p.x, p.y, p.z }, x, y)) pen.Circle(x, y, 3.2);
				}
				pen.End();
			}
		}

		void Hide()
		{
			if (g_ui.built && IsObject(g_ui.layer)) SetMember(g_ui.layer, "visible", Bool(false));
		}

		// The Traffic Manager's first-time message (2026-10-30, the user): the game's own message box, as for the first use of the console, the first time the local map is open in a
		// playthrough. The game's call, as Debug.MessageBox (FUN_1420035D0) makes it: FUN_141ED62B0(*0x1461DE720, &{callback, title, body, third, type 5}, 0); strings set by FUN_1402D31D0.
		// The flag is global FVO_TrafficManagerSeen in FVO_NPCPilots.esm: the game writes globals into the save, so later saves inherit it and a new game starts without it. Written as
		// GlobalVariable.SetValue does (14204FD20: the float at +0x48). Written first: if the call ever failed, it must not fail on every open.
		bool ShowGameMessage(const char* title, const char* body)
		{
			struct Text { void* data; std::uint32_t a; std::uint32_t b; };
			struct Box { void* callback; Text title, body, third; std::uint32_t type; std::uint8_t spare[0x44]; };
			static_assert(offsetof(Box, title) == 8 && offsetof(Box, body) == 0x18 && offsetof(Box, third) == 0x28 && offsetof(Box, type) == 0x38);
			__try {
				Box box{};
				box.title.b = box.body.b = box.third.b = 1;
				box.type = 5;
				const auto set = reinterpret_cast<void (*)(Text*, const char*, int)>(game::Base() + 0x002D31D0);
				set(&box.title, title, 0); set(&box.body, body, 0); set(&box.third, nullptr, 0);
				reinterpret_cast<void (*)(std::uintptr_t, Box*, char)>(game::Base() + 0x01ED62B0)(*reinterpret_cast<std::uintptr_t*>(game::Base() + 0x061DE720), &box, 0);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		int g_mapOpenFrames = 0;
		void FirstMapMessage(bool focused)
		{
			if (!focused || ++g_mapOpenFrames != 30) return;  // once per opening, half a second in: the map has finished opening
			const uptr seen = runtimeforms::Lookup(runtimeforms::Resolve(pilotlayout::kMapMessageSeen));
			if (!seen || *reinterpret_cast<const std::uint8_t*>(seen + 0x2E) != 10) {
				static bool told = false;
				if (!told) { told = true; logger::Write("Route editor: no first-time flag in FVO_NPCPilots.esm (an older copy?): no Traffic Manager message"); }
				return;
			}
			float& value = *reinterpret_cast<float*>(seen + 0x48);
			if (value != 0.f) return;
			value = 1.f;
			const bool shown = ShowGameMessage("Traffic Manager",
				"You can manage traffic routes for this planet from the local map. The routes you create decide where vehicles fly, and traffic is generated along them.\n\n"
				"Press F7 while on the local map to open the Traffic Manager, then create or edit a route.\n\n"
				"Autopilot near a traffic zone will follow the same path.");
			logger::Write("Route editor: first-time Traffic Manager message %s", shown ? "shown" : "could not be shown");
		}

		void WorkBody(uptr menu, uptr movie)
		{
			routemap::Camera c;
			const bool open = Snapshot(c);
			const bool focused = Focused();
			if (!open) { Hide(); g_ed.dragging = false; g_mapOpenFrames = 0; return; }
			FirstMapMessage(focused);
			// Our values belong to one movie; they are forgotten only after it is destroyed (MovieGone), never overwritten
			// while still linked into a live movie.
			if (!g_ui.movie) g_ui.movie = movie;
			else if (g_ui.movie != movie) return;
			if (!g_ui.built) {
				if (g_ui.failed) return;
				if (!Build(menu, movie)) { g_ui.failed = true; logger::Write("Route editor: overlay creation failed; editor off until the map reloads"); return; }
				g_ui.built = true;
			}
			SetMember(g_ui.layer, "visible", Bool(true));
			// Keep our layer on top of the map's own clips.
			{
				const auto& root = *reinterpret_cast<const Value*>(menu + 0x58);
				double count = 0;
				if (IsObject(root) && GetNumber(root, "numChildren", count) && count >= 1) {
					const Value args[] = { Arg(g_ui.layer), Num(count - 1) };
					Invoke(root, "setChildIndex", args, 2);
				}
			}
			double lx = 0, ly = 0;
			const bool haveMouse = GetNumber(g_ui.layer, "mouseX", lx) && GetNumber(g_ui.layer, "mouseY", ly);
			if (haveMouse) Calibrate(c, lx, ly);
			if (g_keys.Pressed(VK_F7, focused) && !g_ed.loading) {
				if (g_ed.on) { g_ed.on = false; g_ed.dragging = false; logger::Write("Route editor: closed%s", g_ed.draft ? " (the open route is kept until you save or cancel)" : ""); }
				else Open();
			}
			g_ui.items.clear();
			Pen lines(g_ui.lines), panel(g_ui.panel), cover(g_ui.cover);
			lines.Clear(); panel.Clear(); cover.Clear();
			if (!g_ed.on) {
				Text(movie, "F7  Traffic routes", g_ui.width - 232, 24, 0xBFD9E8, 220, true);
				FlushText();
				return;
			}
			inputgate::HoldMapForEditor();  // the map's own clicks and action keys are off while the editor is open
			const ULONGLONG now = GetTickCount64();
			if (g_ed.tickAt) g_ed.phase += std::min<ULONGLONG>(now - g_ed.tickAt, 100) / 1000.0;
			g_ed.tickAt = now;
			if (now - g_ed.othersAt > 3000) LoadOthers();

			// ---- the traffic model restart's loading panel (no other input until it is done) ----
			if (g_ed.loading) {
				inputgate::HoldMapAll();
				const auto st = trafficpool::RushStatus();
				// Positions filled while the world runs: keep the panel (and the running world) until the placed vehicles near
				// the player are released to their autopilots, at most 8 s, then the map pauses again.
				const bool settling = !st.active && now - g_ed.loadingAt > 800 && secretunpause::Active() &&
					(g_ed.filledAt ? now - g_ed.filledAt < 8000 && vehiclediag::StagedLoaded() > 0 : (g_ed.filledAt = now, true));
				if (st.active && now - g_ed.loadingAt > 50000) {
					// The paused map gave the restart no frames: it is still asked for and runs as soon as the map closes.
					g_ed.loading = false;
					secretunpause::End("panel time limit");
					Status("Traffic update is queued; vehicles remain in place until it can be applied safely");
				}
				else if (!st.active && now - g_ed.loadingAt > 800 && !settling) {
					g_ed.loading = false;
					const bool ran = secretunpause::Active();
					secretunpause::End("traffic armed");
					if (st.total == 0) {
						Status("No traffic spots are known in this world yet: add landing spots, close the map once (traffic reads them), then press Start/Swap again");
						goto loaded;
					}
					char b[260];
					std::snprintf(b, sizeof b, "Traffic started: %d of %d route positions hold vehicles (%d kept, %d placed now)%s. %s",
						st.placed, st.total, st.moved, st.made, st.skipped ? (", " + std::to_string(st.skipped) + " deferred: no clear lane spot").c_str() : "",
						ran ? "Already driving." : "They drive when the map closes.");
					Status(b);
				loaded:;
				}
				else {
					const double w = 560, h = 150, x = (g_ui.width - w) / 2, y = (g_ui.height - h) / 2;
					panel.NoLine(); panel.Fill(0x0B1620, 0.92); panel.Rect(x, y, w, h); panel.End();
					panel.Style(2, 0x4FE3FF, 1); panel.Move(x, y); panel.Line(x + w, y); panel.Line(x + w, y + h); panel.Line(x, y + h); panel.Line(x, y); panel.NoLine();
					const int dots = int(g_ed.phase * 3) % 4;
					Text(movie, std::string("UPDATING TRAFFIC") + std::string(std::size_t(dots), '.'), x + 24, y + 22, 0xFFFFFF, w - 48);
					char b[160];
					// Start/Swap cold start: every scheduled route position gets its vehicle under this panel (2026-10-29).
					if (g_ed.filledAt) std::snprintf(b, sizeof b, "Traffic taking off: %d vehicle(s) waiting for their pilots", vehiclediag::StagedLoaded());
					else std::snprintf(b, sizeof b, "Placing traffic on the routes: %d of %d positions", st.placed, st.total);
					Text(movie, b, x + 24, y + 54, 0xD8E4EC, w - 48);
					std::snprintf(b, sizeof b, "Kept %d, placed now %d, deferred %d", st.moved, st.made, st.skipped);
					Text(movie, b, x + 24, y + 76, 0x9AA4AE, w - 48);
					const double f = st.total > 0 ? std::clamp(double(st.placed + st.skipped) / st.total, 0.0, 1.0) : 0.0;
					panel.Fill(0x22384A, 1); panel.Rect(x + 24, y + 104, w - 48, 14); panel.End();
					panel.Fill(0x4FE3FF, 1); panel.Rect(x + 24, y + 104, (w - 48) * f, 14); panel.End();
					Text(movie, "The map stays open until every route position is filled", x + 24, y + 124, 0x9AA4AE, w - 48);
					FlushText();
					return;
				}
			}

			// ---- input ----
			const bool ctrl = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
			const bool shift = focused && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
			const bool mouseDown = focused && (GetAsyncKeyState(VK_LBUTTON) & 0x8000);
			static bool wasDown = false;
			const bool click = mouseDown && !wasDown, release = !mouseDown && wasDown;
			wasDown = mouseDown;
			if (click && haveMouse) {
				if (const Button* b = ButtonAt(lx, ly)) { if (b->enabled) Press(b->id, c); }
				else if (!OverPanel(lx, ly)) {
					trafficdata::Route ringScratch;
					const auto* ringShown = Shown(ringScratch);
					double hx = 0, hy = 0;
					if (g_ed.mode == Mode::Drag && ringShown && ringShown->ring &&
						Local(c, { ringShown->shape.cx, ringShown->shape.cy, ringShown->shape.z }, hx, hy) && std::hypot(hx - lx, hy - ly) < 18) {
						if (EnsureDraft()) { g_ed.point = -2; g_ed.dragging = true; g_ed.dragAt = { g_ed.draft->Value().shape.cx, g_ed.draft->Value().shape.cy, g_ed.draft->Value().shape.z }; }
					}
					else if (g_ed.mode == Mode::Drag) {
						const int p = PointAt(c, lx, ly, 16);
						if (p >= 0 && EnsureDraft()) {
							g_ed.point = p; g_ed.dragging = true;
							const auto& q = g_ed.draft->Value().points[std::size_t(p)];
							g_ed.dragAt = { q.x, q.y, q.z };
						}
					}
					else ClickMap(c, lx, ly);
				}
			}
			if (g_ed.dragging && g_ed.draft) {
				routemap::Vec3 at;
				if (routemap::MouseAt(c, g_ed.dragAt.z, at)) g_ed.dragAt = at;
				if ((release || !mouseDown) && g_ed.point == -2) {  // a ring's centre
					g_ed.dragging = false; g_ed.point = -1;
					auto r = g_ed.draft->Value(); r.shape.cx = g_ed.dragAt.x; r.shape.cy = g_ed.dragAt.y; g_ed.draft->Edit(r);
					Status("Ring centre moved");
				}
				else if (release || !mouseDown) {
					g_ed.dragging = false;
					if (const char* why = PointProblem(g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z)) Refused("Point move", g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z, why);
					else {
						g_ed.draft->Move(std::size_t(g_ed.point), { g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z });
						char b[96]; std::snprintf(b, sizeof b, "Point %d moved to (%.0f, %.0f), height %.0f m", g_ed.point + 1, g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z);
						Status(b);
					}
				}
			}
			else g_ed.dragging = false;
			for (const auto& k : kShortcuts)
				if (g_keys.Pressed(k.vk, focused) && !ctrl) {
					const Button* b = nullptr;
					for (const auto& o : g_ui.buttons) if (o.id == k.id) b = &o;
					if (b && !b->enabled) Status(std::string("'") + b->text + "' is not available now");
					else Press(k.id, c);
				}
			if (g_keys.Pressed(VK_INSERT, focused)) DropPoint(c);
			if (g_keys.Pressed(VK_DELETE, focused)) DeleteSelected();
			if (g_keys.Pressed(VK_PRIOR, focused) || g_keys.Pressed(VK_OEM_6, focused)) ChangeZ(shift ? 25 : 5);
			if (g_keys.Pressed(VK_NEXT, focused) || g_keys.Pressed(VK_OEM_4, focused)) ChangeZ(shift ? -25 : -5);
			{
				trafficdata::Route scratch;
				const auto* r = Shown(scratch);
				const int n = r ? int(r->points.size()) : 0;
				if (g_keys.Pressed(VK_HOME, focused) && n) { g_ed.point = g_ed.point <= 0 ? n - 1 : g_ed.point - 1; g_ed.spot = -1; }
				if (g_keys.Pressed(VK_END, focused) && n) { g_ed.point = (g_ed.point + 1) % n; g_ed.spot = -1; }
			}
			const bool keyZ = g_keys.Pressed('Z', focused), keyY = g_keys.Pressed('Y', focused), keyS = g_keys.Pressed('S', focused);
			if (ctrl && keyZ && g_ed.draft && g_ed.draft->Undo()) Status("Undone");
			if (ctrl && keyY && g_ed.draft && g_ed.draft->Redo()) Status("Redone");
			if (ctrl && keyS) Save();

			// ---- draw ----
			trafficdata::Route scratch;
			const trafficdata::Route* shown = Shown(scratch);
			int minute = 0;
			const bool timeKnown = LocalMinute(minute);
			for (std::size_t k = 0; k < g_ed.others.size(); ++k) {
				const bool sel = !g_ed.draft && int(k) == g_ed.other;
				DrawRoute(lines, c, g_ed.others[k].line, sel ? 0xFFFFFF : 0x9AA4AE, sel ? 3 : 2, sel ? 0.95 : 0.55);
			}
			for (std::size_t k = 0; k < g_ed.file.routes.size(); ++k) {
				if (g_ed.draft && int(k) == g_ed.draftIndex) continue;
				if (!Usable(g_ed.file.routes[k])) continue;
				const auto t = ToTd(g_ed.file.routes[k], "");
				const auto line = trafficroute::Build(t);
				const auto d = routeschedule::Evaluate(t.schedule, timeKnown, minute);
				const bool sel = !g_ed.draft && int(k) == g_ed.route;
				DrawRoute(lines, c, line, sel ? 0x4FE3FF : 0x2C8FB0, sel ? 3 : 2, d.active ? 0.9 : 0.4);
				if (d.active || FlowShare(t.name) < 1.0) DrawFlow(lines, c, line, std::clamp(t.lanes, 1, 2), t.speed, 0x9FF7FF, 0xFFB45A, FlowShare(t.name));
			}
			if (shown) {
				auto r = *shown;
				if (g_ed.dragging && g_ed.point >= 0 && g_ed.point < int(r.points.size())) r.points[std::size_t(g_ed.point)] = { g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z };
				if (g_ed.dragging && g_ed.point == -2) { r.shape.cx = g_ed.dragAt.x; r.shape.cy = g_ed.dragAt.y; }
				const auto line = trafficroute::Build(r);
				if (r.ring) {  // the ring's centre handle (Drag moves it, Drop sets it)
					double hx, hy;
					if (Local(c, { r.shape.cx, r.shape.cy, r.shape.z }, hx, hy)) {
						lines.Style(3, 0xFFE15A, 1);
						lines.Move(hx - 9, hy); lines.Line(hx + 9, hy); lines.Move(hx, hy - 9); lines.Line(hx, hy + 9);
						char b[64]; std::snprintf(b, sizeof b, "Centre  R %.0f  Z %.0f m", r.shape.radius, r.shape.z);
						Text(movie, b, hx + 12, hy - 10, 0xFFE15A, 200, true);
					}
				}
				const auto d = routeschedule::Evaluate(r.schedule, timeKnown, minute);
				if (g_ed.draft) DrawRoute(lines, c, line, 0xFFE15A, 3, 0.95);
				if (d.active || g_ed.draft) DrawFlow(lines, c, line, std::clamp(r.lanes, 1, 2), r.speed, 0x9FF7FF, 0xFFB45A, FlowShare(r.name));
				for (std::size_t i = 0; i < r.points.size(); ++i) {
					double x, yy;
					if (!Local(c, { r.points[i].x, r.points[i].y, r.points[i].z }, x, yy)) continue;
					const bool sel = int(i) == g_ed.point;
					lines.Style(2, 0x0B1620, 0.9);
					lines.Fill(sel ? 0xFFE15A : (i == 0 ? 0x7CFF8A : 0xFFFFFF), 1);
					lines.Circle(x, yy, sel ? 8 : 5.5);
					lines.End();
					char b[48];
					std::snprintf(b, sizeof b, sel ? "%zu  Z %.0f m" : "%zu", i + 1, r.points[i].z);
					Text(movie, b, x + 9, yy - 10, sel ? 0xFFE15A : 0xFFFFFF, sel ? 140 : 40, true);
				}
			}
			for (int kind = 1; kind <= 2; ++kind) {
				const auto& list = kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				for (std::size_t i = 0; i < list.size(); ++i) {
					double x, yy;
					if (!SpotHere(list[i]) || !Local(c, { list[i].at.x, list[i].at.y, list[i].at.z }, x, yy)) continue;
					const bool sel = g_ed.spotKind == kind && g_ed.spot == int(i);
					lines.Style(sel ? 3 : 2, kind == 1 ? 0x7CFF8A : 0xC58BFF, 1);
					lines.Move(x - 7, yy - 7); lines.Line(x + 7, yy - 7); lines.Line(x + 7, yy + 7); lines.Line(x - 7, yy + 7); lines.Line(x - 7, yy - 7);
					Text(movie, kind == 2 ? "S" : list[i].parked ? "P" : "L", x + 9, yy - 10, kind == 1 ? 0x7CFF8A : 0xC58BFF, 30, true);
				}
			}

			for (std::size_t i = 0; i < g_ed.file.links.size(); ++i) {
				trafficroute::Vec at; double x, yy;
				if (!JunctionAt(g_ed.file.links[i], at) || !Local(c, { at.x, at.y, at.z }, x, yy)) continue;
				const bool sel = g_ed.link == int(i);
                const auto& junction=g_ed.file.links[i];
                if(junction.radius>0){
                    trafficroute::Line from,to;bool fromRing=false,toRing=false;
                    if(LineOf(junction.from,from,fromRing)&&LineOf(junction.to,to,toRing)){
                        const double u=fromRing?junction.at/360.0*trafficroute::kRingSamples:from.ToLine(junction.at);
                        const double v=junction.toEnd?to.Span():to.ToLine(junction.toAt);
                        const int lane=trafficlinks::EntryLane(to,2,v);
                        const auto curve=trafficlinks::TurnPath(from,0,u,to,std::max(0,lane),v,junction.radius);
                        lines.Style(sel?3:2,0xFF8AD8,0.9);bool down=false;
                        for(const auto& point:curve){double cx=0,cy=0;if(Local(c,{point.x,point.y,point.z},cx,cy)){if(down)lines.Line(cx,cy);else lines.Move(cx,cy);down=true;}else down=false;}
                    }
                }
				lines.Style(1, 0xFF8AD8, 0.8); lines.Move(x, yy); lines.Line(x, yy - kDiamondLift + 8);  // stem down to the junction
				yy -= kDiamondLift;
				lines.Style(2, 0x0B1620, 1); lines.Fill(sel ? 0xFFE15A : 0xFF8AD8, 1);
				lines.Move(x, yy - 8); lines.Line(x + 8, yy); lines.Line(x, yy + 8); lines.Line(x - 8, yy); lines.Line(x, yy - 8);
				lines.End();
				Text(movie, std::to_string(int(std::lround(g_ed.file.links[i].share * 100))) + "%", x + 10, yy + 2, sel ? 0xFFE15A : 0xFF8AD8, 50, true);
			}
			// Cover: catches clicks on the map while dropping / dragging so the map does not also place its own marker.
			// (any mode: the editor owns map clicks while it is open)
			cover.NoLine(); cover.Fill(0x000000, 0.01); cover.Rect(0, 0, g_ui.width, g_ui.height); cover.End();

			// ---- panel ----
			Layout(shown);
			const double panelH = (g_ui.buttons.empty() ? 92 : g_ui.buttons.back().y + 32) + 236;
			// Above the map's own button bar at the bottom right.
			const double px = PanelX(), tx = px + 12;
			g_ui.panelBottom = 12 + std::min(panelH, g_ui.height - 140);
			panel.NoLine(); panel.Fill(0x0B1620, 0.82); panel.Rect(px, 12, kPanelW, g_ui.panelBottom - 12); panel.End();
			Text(movie, "TRAFFIC ROUTES   (F7 closes)", tx, 20, 0xFFFFFF, 300);
			Text(movie, g_ed.world, tx, 42, 0x9AA4AE, 300);
			Text(movie, std::string("Mode: ") + ModeName(g_ed.mode) + (g_ed.draft ? "    editing (unsaved)" : ""), tx, 64, g_ed.draft ? 0xFFE15A : 0xBFD9E8, 300);
			for (const auto& b : g_ui.buttons) {
				const unsigned fill = !b.enabled ? 0x1A2630 : b.active ? 0x2C8FB0 : 0x22384A;
				panel.Style(1, b.active ? 0x9FF7FF : 0x3E5A70, 1); panel.Fill(fill, 0.95); panel.Rect(b.x, b.y, b.w, b.h); panel.End();
				const double keyW = b.key.empty() ? 0 : double(b.key.size()) * 8.25 + 4;
				Text(movie, b.text, b.x + 6, b.y + 2, b.enabled ? 0xFFFFFF : 0x6A7680, b.w - 8 - keyW);
				if (!b.key.empty()) Text(movie, b.key, b.x + b.w - keyW - 2, b.y + 2, b.enabled ? 0x8FD8F0 : 0x56626C, keyW);
			}
			double y = (g_ui.buttons.empty() ? 92 : g_ui.buttons.back().y + 34);
			auto info = [&](const std::string& t, unsigned color = 0xD8E4EC) {
				for (const auto& line : strokefont::Wrap(t, strokefont::Fit(kPanelW - 24, kUnit))) {
					if (y < g_ui.panelBottom - kLine) Text(movie, line, tx, y, color, kPanelW - 24);
					y += kLine;
				}
			};
			if (!g_ed.status.empty() && now - g_ed.statusAt < 12000) { info(g_ed.status, 0xFFFFFF); y += 4; }
			if (shown) {
				const auto line = trafficroute::Build(*shown);
				char b[160];
				std::snprintf(b, sizeof b, "%s: %zu points, %.0f m", shown->label.empty() ? shown->name.c_str() : shown->label.c_str(), shown->points.size(), trafficroute::Length(line));
				info(b, 0xFFFFFF);
				std::snprintf(b, sizeof b, "%s, %s, %.0f m/s", shown->closed ? "loop" : "open", shown->lanes > 1 ? "two-way" : "one-way", shown->speed);
				info(b);
				info(ScheduleText(shown->schedule));
				const auto d = routeschedule::Evaluate(shown->schedule, timeKnown, minute);
				const auto issue = routeschedule::Validate(shown->schedule);
				info(issue != routeschedule::Issue::None ? std::string("Schedule not valid: ") + routeschedule::IssueText(issue)
					: !d.known ? "Local time unknown: scheduled routes wait" : d.active ? "Active now (" + std::to_string(d.vehicles) + " vehicle(s))" : "Not active at this time",
					d.active ? 0x7CFF8A : 0xFFB45A);
				if (g_ed.point >= 0 && g_ed.point < int(shown->points.size())) {
					const auto& p = shown->points[std::size_t(g_ed.point)];
					std::snprintf(b, sizeof b, "Point %d: x %.0f  y %.0f  Z %.0f m (world)", g_ed.point + 1, p.x, p.y, p.z);
					info(b, 0xFFE15A);
                    const double radius=std::size_t(g_ed.point)<shown->rounding.size()?shown->rounding[std::size_t(g_ed.point)]:0;
                    info("Corner: "+(radius>0?"rounded, R "+std::to_string(int(radius))+" m":std::string("sharp")),0xFFE15A);
				}
				if (g_ed.draft) {
					const auto issue = g_ed.draft->Check();
					if (issue != routeeditor::Issue::None) info(std::string("Cannot save yet: ") + IssueText(issue), 0xFF7A6A);
					const auto w = Warnings(*shown);
					for (std::size_t i = 0; i < w.size() && i < 2; ++i) info("Note: " + w[i], 0xFFB45A);
				}
			}
			else if (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size()) && g_ed.file.routes[std::size_t(g_ed.route)].removed)
				info("Removed original '" + g_ed.file.routes[std::size_t(g_ed.route)].id + "': Del route brings it back", 0xFFB45A);
			else info(g_ed.file.routes.empty() ? "No editor routes yet: press New route, or click a grey route to edit it" : "No route selected");
			if (!g_ed.draft && g_ed.other >= 0 && g_ed.other < int(g_ed.others.size()))
				info("Original, from " + g_ed.others[std::size_t(g_ed.other)].file + ": Edit this to change it", 0xFFFFFF);
            if(g_ed.link>=0&&g_ed.link<int(g_ed.file.links.size()))info("Junction radius: "+std::to_string(int(g_ed.file.links[std::size_t(g_ed.link)].radius))+" m (0 = sharp)",0xFF8AD8);
			info((g_ed.previewHour >= 0 ? "Preview " : "Local time ") + (timeKnown ? Clock(minute) : std::string("unknown")), 0xBFD9E8);
			info("Grey: routes from other files; click one, then Edit this", 0x9AA4AE);
			info("Dots: preview of the traffic flow, not real vehicles", 0x9AA4AE);
			info("Pink diamonds: junctions, with the share that turns in", 0x9AA4AE);
			if (!g_ui.calibrated) info("Move the mouse across the map once to line the overlay up", 0xFFB45A);
			Text(movie, "Q select  E drop  R drag  C connect  G landing  I J K L move  O/P route  N new  X start/swap  - = vehicles",
				24, g_ui.height - 50, 0xBFD9E8, g_ui.width * 0.6);
			Text(movie, "Insert drop   Delete remove   PgUp/PgDn or [ ] height (Shift x5)   Home/End point   Ctrl+Z/Y undo   Ctrl+S save",
				24, g_ui.height - 34, 0xBFD9E8, g_ui.width * 0.6);
			FlushText();
		}
	}

	bool LocalHours(double& hours)
	{
		__try {
			const uptr tes = *reinterpret_cast<const uptr*>(game::Base() + addr::kTESPtr);
			const uptr planet = tes ? *reinterpret_cast<const uptr*>(tes + 0x140) : 0;
			if (!planet) return false;
			const float h = *reinterpret_cast<const float*>(planet + 0xEF8);
			if (!std::isfinite(h) || h < 0 || h > 24.0f) return false;
			hours = h >= 24.0f ? 0.0 : double(h);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	bool MapOpen(unsigned withinMs) { const ULONGLONG at = g_snapAt.load(); return at && GetTickCount64() - at < withinMs; }

	void GroundTick()
	{
		std::lock_guard lk(g_groundLock);
		if (g_groundAsks.empty()) return;
		const ULONGLONG now = GetTickCount64();
		for (auto& a : g_groundAsks) {
			if (a.done) continue;
			double x = 0, y = 0, z = 0; bool hit = false;
			if (environment::RayHit(a.from, a.to, x, y, z, hit)) {
				a.done = true; a.hit = hit; a.x = x; a.y = y; a.z = z; a.how = "ground or object";
				logger::Write("Route editor: ground under the cursor for %s: %s (%.0f, %.0f, %.1f) after %llu ms", a.id.c_str(), hit ? "hit" : "nothing", x, y, z,
					static_cast<unsigned long long>(now - a.at));
				continue;
			}
			if (!a.direct && now - a.at >= 300) {  // the paused map never services the queued ray: cast it directly
				a.direct = true;
				prefetch::RayHit h;
				if (vehicle_probe::CastNow(a.from, a.to, 0x22806, h) == 1) {
					a.done = true; a.hit = h.hit && std::isfinite(h.x) && std::isfinite(h.y) && std::isfinite(h.z);
					if (a.hit) { a.x = h.x; a.y = h.y; a.z = h.z; }
					a.how = "ground or object";
					logger::Write("Route editor: ground under the cursor for %s (direct ray): %s (%.0f, %.0f, %.1f)", a.id.c_str(), a.hit ? "hit" : "nothing", a.x, a.y, a.z);
					continue;
				}
			}
			if (now - a.at < kGroundWaitMs) continue;
			const auto m = navmap::At(a.px, a.py, a.from[2]);  // no ray answer: the nav map's ground there, then the cell's baked terrain
			double baked = 0;
			a.done = true; a.x = a.px; a.y = a.py;
			if (m.haveGround && std::isfinite(m.ground)) { a.hit = true; a.z = m.ground; a.how = "mapped ground"; }
			else if (cellbake::Ground(a.px, a.py, baked) && std::isfinite(baked)) { a.hit = true; a.z = baked; a.how = "baked terrain"; }
			else a.hit = false;
			logger::Write("Route editor: no ray answer for %s in %llu ms; %s", a.id.c_str(), static_cast<unsigned long long>(kGroundWaitMs),
				a.hit ? a.how : "ground unknown (height kept, never below 0)");
		}
	}

	void Advance(std::uintptr_t movie)
	{
		if (!movie || movie != g_movie.load()) return;
		const uptr menu = g_menu.load();
		if (!menu || (g_ui.failed && g_ui.movie == movie)) return;
		__try { ApplyGround(); WorkBody(menu, movie); }
		__except (EXCEPTION_EXECUTE_HANDLER) {
			g_ui.failed = true;
			logger::Write("Route editor: exception 0x%08lX in the map overlay; editor off until the map movie reloads", GetExceptionCode());
		}
	}

	void MovieClosing(std::uintptr_t movie)
	{
		if (movie && movie == g_movie.load()) { g_snapAt.store(0);g_movie.store(0); g_menu.store(0); }
	}
	void MovieGone(std::uintptr_t movie)
	{
		if (!movie) return;
		if (movie == g_ui.movie) {
			// The movie unlinked its values (they live in our memory): forget them, never release.
			g_ui = Ui{};
			g_ed.dragging = false;
		}
	}

	bool Install()
	{
		void* orig = nullptr;
		if (!PatchSlot(kRvaStarMapVtbl, kSlotMessage, kRvaStarMapMessage, reinterpret_cast<void*>(&StarMapMessageDetour), &orig)) {
			logger::Write("Route editor: StarMapMenu vtable slot %d is not the expected function; map route editor off", kSlotMessage);
			return false;
		}
		g_messageOriginal = reinterpret_cast<Message_t>(orig);
		if (!PatchSlot(kRvaMapStateVtbl, kSlotMapUpdate, kRvaMapUpdate, reinterpret_cast<void*>(&MapUpdateDetour), &orig)) {
			logger::Write("Route editor: SurfaceMapState vtable slot %d is not the expected update; map route editor off", kSlotMapUpdate);
			return false;
		}
		g_updateOriginal = reinterpret_cast<Update_t>(orig);
		double h = 0;
		const bool known = LocalHours(h);
		logger::Write("Route editor: local map hooks installed (F7 on the local map); planet local time %s %.2f h", known ? "now" : "unknown,", known ? h : 0.0);
		return true;
	}
}

// From the traffic addon (fvo_addon_api.h): where an anchored traffic world lies in the player's world.
extern "C" __declspec(dllexport) void FVO_SetWorldShift(const char* name, const char* gameWorld, double x, double y, double z)
{
	if (!name || !gameWorld) return;
	std::lock_guard lk(routeedit::g_shiftLock);
	auto& w = routeedit::g_worldShifts[trafficdata::Lower(name)];
	w = { gameWorld, trafficdata::Vec{ x, y, z } };
}

// For the traffic addon (fvo_addon_api.h): the planet's local time in hours; 0 when unknown.
extern "C" __declspec(dllexport) int FVO_LocalTime(double* hours)
{
	double h = 0;
	if (!hours || !routeedit::LocalHours(h)) return 0;
	*hours = h;
	return 1;
}
