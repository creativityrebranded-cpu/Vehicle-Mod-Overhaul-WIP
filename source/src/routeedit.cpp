#include "routeedit.h"
#include "trafficprofile.h"
#include "pilotlayout.h"
#include "routeeditor.h"
#include "bindings.h"
#include "bindlogic.h"
#include "routefiles.h"
#include "routemap.h"
#include "trafficroute.h"
#include "trafficlinks.h"
#include "strokefont.h"

#include <array>
#include <functional>
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
#include "realvehicles.h"
#include "npcvehicles.h"

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
#include <tuple>
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

		// ---- Turning the map (2026-10-30, the user: "the map should be able to rotate"; notes/traffic-manager-stage2-20261030/RESULT.md) ----
		// StarMap::SurfaceMapState keeps yaw at +0x8F4 and pitch at +0x8F0, in degrees; its update (FUN_1416da0b0) builds the camera
		// from them every frame. FUN_1416dca30 sets yaw (mod 360) and moves the native markers with it. The right button is the map's
		// own "rotate", but outdoors (FUN_1416d5cd0 false) it only spins yaw by circling the screen centre and never tilts: that is the
		// locked feeling. While the editor's right-drag runs, the editor owns both angles (set just before the update, so the native
		// spin from the same drag is overwritten); otherwise the game's angle is left alone (the gamepad's right stick still turns it).
		// Pitch stays inside the game's own fSurfaceMapPitchMin/MaxAngleDegrees (10 and 85 in 1.16.244).
		constexpr std::uint32_t kRvaSetYaw = 0x016DCA30, kRvaPitchMin = 0x05F36208, kRvaPitchMax = 0x05F36228;
		std::atomic<bool> g_orbitOn{ false }, g_orbitOnce{ false };
		std::atomic<float> g_orbitYaw{ 0 }, g_orbitPitch{ 45 }, g_mapYaw{ 0 }, g_mapPitch{ 45 }, g_pitchMin{ 10 }, g_pitchMax{ 85 };
		void ApplyOrbit(uptr state)
		{
			__try {
				const float lo = *reinterpret_cast<const float*>(game::Base() + kRvaPitchMin), hi = *reinterpret_cast<const float*>(game::Base() + kRvaPitchMax);
				if (std::isfinite(lo) && std::isfinite(hi) && lo < hi && lo >= -90.f && hi <= 90.f) { g_pitchMin = lo; g_pitchMax = hi; }
				float& yaw = *reinterpret_cast<float*>(state + 0x8F4);
				float& pitch = *reinterpret_cast<float*>(state + 0x8F0);
				// As the game's own auto-rotate: only with the map's terrain (+0x8E8) and camera rig (+0x78) in place.
				const bool ready = *reinterpret_cast<const uptr*>(state + 0x8E8) && *reinterpret_cast<const uptr*>(state + 0x78);
				if (ready && (g_orbitOn.load() || g_orbitOnce.exchange(false))) {
					const float want = g_orbitYaw.load();
					if (std::isfinite(want) && std::fabs(std::remainder(want - yaw, 360.f)) > 0.01f)
						reinterpret_cast<void (*)(uptr, float)>(game::Base() + kRvaSetYaw)(state, want);
					const float p = g_orbitPitch.load();
					if (std::isfinite(p)) pitch = std::clamp(p, g_pitchMin.load(), g_pitchMax.load());
				}
				if (std::isfinite(yaw)) g_mapYaw = yaw;
				if (std::isfinite(pitch)) g_mapPitch = pitch;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}

		void MapUpdateDetour(uptr state, uptr a2, float dt)
		{
			ApplyOrbit(state);
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
			return trafficprofile::Routes();
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
		// Route (2026-10-30, Traffic Manager stage 2): the whole route at once: drag moves it, its handle turns it, the wheel raises it.
		enum class Mode { Select, Drop, Drag, Landing, Spawn, Connect, Route, Shape, Zone };
		// Ctrl+click (2026-10-31): anything placed can join one selection. Found again by id, so it survives other things being added or removed; a point by its place in the open route.
		enum : int { kPickPoint = 1, kPickLanding, kPickSpawn, kPickZone, kPickLink, kPickRoute };
		struct Pick { int kind = 0; std::string id; int index = -1; };
		struct MassSnap { routefiles::File file; bool draftChanged = false; std::size_t depth = 0; };
		const char* ModeName(Mode m)
		{
			switch (m) { case Mode::Drop: return "Add"; case Mode::Drag: return "Move"; case Mode::Landing: return "Land"; case Mode::Spawn: return "Spawn spot"; case Mode::Connect: return "Link"; case Mode::Route: return "Route"; case Mode::Shape: return "Shape"; case Mode::Zone: return "Zone"; default: return "Select"; }
		}
		// What the map shows (the View menu, stage 2). Session only.
		struct View { bool others = true, flow = true, stems = true, numbers = true, names = true, spots = true; };
		// A whole-route drag in progress (Route tool): 1 move, 2 turn. The draft changes once, on release (one undo step).
		struct RouteDrag { int kind = 0; trafficdata::Route base, preview; routeeditor::Point centre{}; routemap::Vec3 start{}; double a0 = 0, turned = 0; };
		// A route of another file (game coordinates; anchored cities placed with the addon's shift).
		struct Other { std::string name, file; trafficroute::Line line; bool ring = false; trafficdata::Route route; };
		struct Editor {
			bool on = false, loaded = false;
			std::wstring profileDir;
			std::string world, fileName, status;
			ULONGLONG statusAt = 0;
			routefiles::File saved, file;           // on disk / being edited (spots edit `file` directly)
			std::optional<routeeditor::Draft> draft;
			int draftIndex = -1;                    // the route the draft replaces; -1: a new route
			int route = -1, point = -1;             // selection (route index in file.routes, point in the shown route)
			std::vector<trafficdata::RouteGroup> routeGroups, routeGroupsSaved;  // VehicleGroups.ini: Save writes it, Cancel puts it back
			int ruleScope = 0;                      // the vehicle rules sheet is for: 0 the route, 1 the group ruleGroup, 2 all routes
			std::string ruleGroup;
			int namingGroup = 0;                    // the name being typed is for: 0 a route, 1 a new group, 2 the group ruleGroup
			std::vector<Pick> picks;                // Ctrl+click: read through PicksNow. The fields here stay the one the panel shows.
			std::array<int, 7> pickPrimary{};       // those fields when the selection last changed: anything else selecting something ends it
			int picksCount = 0;                     // points in the open route then
			std::vector<MassSnap> massUndo, massRedo;
			int groupDrag = 0;                      // 1 a point, 2 a place, 3 the Route tool
			double groupTurn = 0;                   // the turn under way, degrees clockwise: landing areas and spawn spots turn with it
			routemap::Vec3 groupLast;
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
			bool naming = false;                    // typing a route's name (the letter keys are text, not shortcuts)
			std::string nameBuf;
            double previewHour = -1;                // -1: the live local time
			bool loading = false;                   // the traffic model restart's loading panel is up
			ULONGLONG loadingAt = 0;
			ULONGLONG filledAt = 0;                 // Start/Swap: positions filled; the world runs (secret unpause) until traffic is armed
			double phase = 0;
			ULONGLONG tickAt = 0;
			// Stage 2 panel: which folded groups are open, an open toolbar menu
			// (0 none, 1 More, 2 View), what the map shows, and the Route tool's drag.
			int sheet = 0, typePage = 0;  // a sheet over the panel: 1 vehicle rules, 2 time rules, 3 city
			int zone = -1;                // selected keep-out or slow zone (file.zones)
			// The Shape tool: which shape, and its centre once the first click has placed it.
			routeeditor::Shape shape = routeeditor::Shape::Circle;
			bool shapeAt = false;
			routemap::Vec3 shapeCentre{};
			// Moving a landing area, spawn spot or zone in 3D (2026-10-30, the user: 3D for everything that can be placed): 1 landing, 2 spawn, 3 zone; Alt moves it up and down.
			int placeKind = 0, placeIndex = -1;
			routemap::Vec3 placeAt{}, placeFrom{};
			bool placeAlt = false, placeMoved = false;
			double placeAltY = 0, placeAltZ = 0, placePpm = 1;
			// The Alt key hub: a burst of moves (one undo step), checked when the keys are let go.
			bool hubOn = false;
			int hubKind = 0;                    // 1 the route or its point, 2 a spot, 3 a zone
			trafficdata::Vec hubFrom{};
			std::uint32_t hubWas = 0;
			ULONGLONG hubAt = 0, hubSince = 0, hubTurnAt = 0;
			// The pad cursor (overlay pixels): on while the left stick last moved it, off when the mouse moves.
			bool padCursor = false;
			double padX = 0, padY = 0, mouseWasX = -1, mouseWasY = -1;
			ULONGLONG padAt = 0;
			int groups = 1, lastGroup = 0, menu = 0;  // groups: a bit per open group (1 Flow, 2 Position, 4 Rules, 8 Problems); several may be open
			View view;
			RouteDrag routeDrag;
			// 3D placement (2026-10-30, the user: points are hard to place in 3D): right-drag turns the map; Alt while moving a point
			// moves it straight up and down.
			bool orbiting = false, altDrag = false;
			double orbitX = 0, orbitY = 0, altY = 0, altZ = 0, altPpm = 1;
			float orbitYaw0 = 0, orbitPitch0 = 45;
		} g_ed;
		// What is under a point (3D placement): the first surface below it from a ray (a roof, a bridge or the ground), else the
		// nav map / baked terrain; and whether something is above it (covered: under a roof or bridge; buried: inside the ground).
		struct Surface { double below = 0, above = 0; bool known = false, rays = false, rough = false, covered = false, buried = false; ULONGLONG at = 0; };  // above: the top of what covers it; rays: a ray hit answered (kept); rough: terrain data only, no buildings
		const Surface& SurfaceUnder(double x, double y, double z);

		// Defined with the drawing below.
		bool Local(const routemap::Camera& c, routemap::Vec3 p, double& x, double& y);
		bool CursorLocal(const routemap::Camera& c, double& x, double& y);
		double SegmentDistance(double px, double py, double ax, double ay, double bx, double by);
		double PanelX();
		// Junction positions on a route follow its points (2026-10-29: junctions may sit at any point): an inserted point at
		// `index` moves later junctions one on; a removed one drops its junction and moves later ones back.
		void LinksPointInserted(const std::string& id, std::size_t index, std::size_t oldCount);
		void LinksPointRemoved(const std::string& id, std::size_t index, std::size_t oldCount);
		bool Many();
		void GroupDelete();

		// Every route has its own colour (2026-10-30, the user): its own if it chose one, else by its place in the list.
		constexpr unsigned kRouteColors[] = { 0x6FE0D0, 0x7FB2E6, 0xB79CF2, 0xF08AC0, 0xB8E06A, 0xF4F0D8, 0x5FD3F5, 0xD9A5FF };
		constexpr int kRouteColorCount = int(sizeof(kRouteColors) / sizeof(kRouteColors[0]));
		unsigned RouteColor(const trafficdata::Route& r, int index) { return r.color ? r.color : kRouteColors[(index % kRouteColorCount + kRouteColorCount) % kRouteColorCount]; }
		int ShownIndex()
		{
			if (g_ed.draft) return g_ed.draftIndex >= 0 ? g_ed.draftIndex : int(g_ed.file.routes.size());
			if (g_ed.route >= 0) return g_ed.route;
			return int(g_ed.file.routes.size()) + g_ed.other;
		}
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
			t.lanes = r.lanes; t.speed = r.speed; t.width = r.width; t.schedule = r.schedule; t.test = r.test; t.color = r.color; t.exclude = r.exclude; t.role = r.role; t.group = r.group;
			return t;
		}
		routefiles::Route FromTd(const trafficdata::Route& t)
		{
			routefiles::Route r;
			r.id = t.name; r.label = t.label; r.points = t.points; r.rounding=t.rounding; r.closed = t.closed; r.lanes = t.lanes; r.speed = t.speed;
			r.width = t.width; r.schedule = t.schedule; r.test = t.test; r.color = t.color; r.exclude = t.exclude; r.role = t.role; r.group = t.group;
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
			for (auto& z : g_ed.file.zones) {
				if (g_ed.spotShifts.count(z.name)) continue;
				trafficdata::Vec shift;
				if (!WorldShift(z.world, shift)) { g_ed.unplacedSpots.insert(z.name); continue; }
				z.at.x += shift.x; z.at.y += shift.y; z.at.z += shift.z; z.top += shift.z;
				for (auto& q : g_ed.saved.zones) if (q.name == z.name) { q.at.x += shift.x; q.at.y += shift.y; q.at.z += shift.z; q.top += shift.z; }
				g_ed.spotShifts[z.name] = shift; g_ed.unplacedSpots.erase(z.name);
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
			if (dir.empty()) return;
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
				const std::wstring ini = trafficprofile::File(L"ringroad.ini");
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
		void LoadGroups();
		void Open()
		{
			const auto profile = TrafficDir();
			if (profile.empty()) { Status("Traffic setup is not available yet"); return; }
			char world[201]{};
			if (!FVO_WorldName(world, sizeof world)) { Status("No world: the route editor works on a planet's local map"); return; }
			if (!g_ed.loaded || g_ed.world != world || g_ed.profileDir != profile) {
				g_ed = Editor{};
				g_ed.profileDir = profile;
				g_ed.world = world;
				g_ed.fileName = routefiles::FileName(world);
				g_ed.file.worldId = world;
				std::string text, why;
				const std::wstring path = profile + L"\\" + Wide(g_ed.fileName);
				if (ReadText(path, text)) {
					std::vector<std::string> problems;
					if (text.find(kSignature) == std::string::npos) Status("Warning: " + g_ed.fileName + " was not written by the editor; saving is off for it");
					else if (!routefiles::Parse(text, g_ed.fileName, g_ed.file, problems)) Status("Could not read " + g_ed.fileName + "; starting empty");
					for (const auto& p : problems) logger::Write("Route editor: %s", p.c_str());
				}
				g_ed.file.worldId = world;
				g_ed.saved = g_ed.file;
				LoadGroups();
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
			if (!s.zones.empty()) return "Busy words, up to " + std::to_string(s.peak) + " vehicle(s)";
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

		std::wstring GroupsPath() { return TrafficDir() + L"\\VehicleGroups.ini"; }
		void LoadGroups()
		{
			std::string text;
			trafficdata::Data d;
			if (ReadText(GroupsPath(), text)) trafficdata::Read(text, "VehicleGroups.ini", d);
			g_ed.routeGroups = d.groups; g_ed.routeGroupsSaved = d.groups;
		}
		void Save()
		{
			const auto profile = TrafficDir();
			if (profile.empty() || profile != g_ed.profileDir) { Status("Playthrough changed: reopen the Traffic Manager before saving"); return; }
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
			for (auto& z : disk.zones)
				if (const auto it = g_ed.spotShifts.find(z.name); it != g_ed.spotShifts.end()) { z.at.x -= it->second.x; z.at.y -= it->second.y; z.at.z -= it->second.z; z.top -= it->second.z; }
			const std::wstring path = profile + L"\\" + Wide(g_ed.fileName);
			std::string existing;
			if (ReadText(path, existing) && existing.find(kSignature) == std::string::npos) {
				Status("Not saved: " + g_ed.fileName + " exists and was not written by the editor (rename it to keep it)");
				return;
			}
			next.traffic = !g_ed.othersHaveTraffic;
			disk.traffic = next.traffic;
			CreateDirectoryW(profile.c_str(), nullptr);
			std::string why;
			if (!WriteAtomic(path, routefiles::Format(disk), why)) { Status("Not saved: " + why); return; }
			if (routefiles::FormatGroups(g_ed.routeGroups) != routefiles::FormatGroups(g_ed.routeGroupsSaved)) {
				if (!WriteAtomic(profile + L"\\VehicleGroups.ini", routefiles::FormatGroups(g_ed.routeGroups), why)) { Status("Routes saved, but not the route groups: " + why); return; }
				g_ed.routeGroupsSaved = g_ed.routeGroups;
			}
			const int route = g_ed.draft ? g_ed.draftIndex : g_ed.route;
			g_ed.massUndo.clear(); g_ed.massRedo.clear();
			g_ed.file = next; g_ed.saved = next;
			g_ed.draft.reset(); g_ed.draftIndex = -1; g_ed.route = route;
			LoadOthers();
			Status("Saved " + g_ed.fileName + ": traffic takes it up by itself: vehicles finish their lap and land before they change route. X clears everything and starts over.");
		}
		void Cancel()
		{
			g_ed.picks.clear(); g_ed.massUndo.clear(); g_ed.massRedo.clear(); g_ed.groupDrag = 0;
			g_ed.routeGroups = g_ed.routeGroupsSaved;
			g_ed.draft.reset(); g_ed.draftIndex = -1; g_ed.dragging = false;
			g_ed.file = g_ed.saved;
			if (g_ed.route >= int(g_ed.file.routes.size())) g_ed.route = g_ed.file.routes.empty() ? -1 : 0;
			g_ed.point = -1; g_ed.spot = -1; g_ed.link = -1; g_ed.zone = -1; g_ed.shapeAt = false;
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
			g_ed.naming = true; g_ed.nameBuf.clear();
			Status("New route '" + r.label + "': type a name and press Enter, or just click the map to drop points");
		}
		void DeleteRoute()
		{
			if (Many()) { GroupDelete(); return; }
			const ULONGLONG nowMs = GetTickCount64();
			// A route of another file: hide it (a removal marker in the editor's file; Del route on the marker restores it).
			if (!g_ed.draft && g_ed.route < 0 && g_ed.other >= 0 && g_ed.other < int(g_ed.others.size())) {
				const auto& o = g_ed.others[std::size_t(g_ed.other)];
				if (nowMs - g_ed.confirmDeleteAt > 3000) { g_ed.confirmDeleteAt = nowMs; Status("Remove original route '" + o.name + "' (" + o.file + ")? Click again (or press Del) within 3 s"); return; }
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
					Status(std::string(r.removed ? "Bring back" : "Undo your edits of") + " original route '" + r.id + "'? Click again (or press Del) within 3 s");
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
			if (now - g_ed.confirmDeleteAt > 3000) { g_ed.confirmDeleteAt = now; Status("Delete route '" + g_ed.file.routes[std::size_t(g_ed.route)].label + "'? Click again (or press Del) within 3 s"); return; }
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
		// The game's own terrain height at (x, y), from the heightfield the map itself reads (2026-10-30, the user: past the loaded cells the height line was gone and
		// points were "operating blind"): rays and the baked grid only know ~25 cells round the player, this knows the terrain window. No buildings. False outside the window.
		bool CachedTerrain(double x, double y, double& ground)
		{
			float xy[4] = { float(x), float(y), 0.0f, 0.0f }, h = 0.0f;
			__try { if (!reinterpret_cast<std::uint8_t (*)(void*, float*, float*)>(Fn(addr::kCachedTerrainHeight))(nullptr, xy, &h)) return false; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
			if (!std::isfinite(h)) return false;
			ground = h;
			return true;
		}
		// The terrain under (x, y): the cell's baked grid, else the nav map's top surface, else the game's terrain. False when none knows it.
		bool TerrainAt(double x, double y, double z, double& ground)
		{
			if (cellbake::Ground(x, y, ground) && std::isfinite(ground)) return true;
			const auto top = navmap::At(x, y, z + 1000.0);
			if (top.haveGround && std::isfinite(top.ground)) { ground = top.ground; return true; }
			return CachedTerrain(x, y, ground);
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
						// 2026-10-30, the user: "moving my points". This answer comes up to 3 s after the click, so it moved points already placed (in Neon, from 167 m down to the sea
						// floor at -246 m), and each move took an undo step. A point keeps where it was put; the answer only lifts one that is under the ground.
						if (!a.hit) {
							if (r.points[i].z < kPlaceFloor) {
								r.points[i].z = kPlaceFloor; g_ed.draft->Edit(r);
								std::snprintf(b, sizeof b, "Point %zu: ground under the cursor unknown (paused map); height %.0f m (never below %.0f), set it with Z", i + 1, r.points[i].z, kPlaceFloor);
								Status(b);
							}
							break;
						}
						if (routeeditor::PlacedHeight(r.points[i].z, a.z, kGroundLift) == r.points[i].z) break;
						r.points[i].z = z;
						g_ed.draft->Edit(r);
						std::snprintf(b, sizeof b, "Point %zu raised to %.0f m: it was under the %s", i + 1, z, a.how);
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
		// focus sits on the ground). The ground ask then only lifts the point if it is under the ground.
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
			// A click on (or within 12 px of) a stretch of the route adds a point there, between its two points, at the height
			// in between (2026-10-29, the user: add points to a route "as opposed to having to delete and build again").
			// (Was 30 px: a click meant to extend the route near its own line went into the middle of it instead; 2026-10-30 log, points 14 to 16.)
			std::size_t after = g_ed.point >= 0 && g_ed.point < int(r.points.size()) ? std::size_t(g_ed.point) + 1 : r.points.size();
			bool split = false;
			double mx = 0, my = 0;
			if (r.points.size() >= 2 && CursorLocal(c, mx, my)) {
				const std::size_t n = r.points.size(), segs = r.closed ? n : n - 1;
				double best = 12;
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
			// (It keeps the height it inherited: heights jumped 167, -13, 132, -13 from one click to the next, by where the ground happened to be known.)
			if (double ground = 0; !split && (DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground)) &&
				routeeditor::PlacedHeight(at.z, ground, kGroundLift) != at.z) {
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
		// ---- One selection for everything placed (2026-10-31, the user: Ctrl+click, "universal"). The route's point, spot, zone and junction fields stay the one the panel
		// shows; this adds the others. Anything that selects something else ends it, without having to know about it: those fields no longer match pickPrimary. ----
		std::array<int, 7> PrimaryNow() { return { g_ed.route, g_ed.other, g_ed.point, g_ed.spot, g_ed.spotKind, g_ed.zone, g_ed.link }; }
		int LandingIndex(const std::string& id) { for (std::size_t i = 0; i < g_ed.file.landings.size(); ++i) if (g_ed.file.landings[i].id == id) return int(i); return -1; }
		int SpawnIndex(const std::string& id) { for (std::size_t i = 0; i < g_ed.file.spawns.size(); ++i) if (g_ed.file.spawns[i].id == id) return int(i); return -1; }
		int ZoneIndex(const std::string& id) { for (std::size_t i = 0; i < g_ed.file.zones.size(); ++i) if (g_ed.file.zones[i].name == id) return int(i); return -1; }
		int LinkIndex(const std::string& id) { for (std::size_t i = 0; i < g_ed.file.links.size(); ++i) if (g_ed.file.links[i].id == id) return int(i); return -1; }
		int RouteIndex(const std::string& id) { for (std::size_t i = 0; i < g_ed.file.routes.size(); ++i) if (g_ed.file.routes[i].id == id) return int(i); return -1; }
		bool IsDraftRoute(const std::string& id) { return g_ed.draft && g_ed.draft->Value().name == id; }
		const std::vector<Pick>& PicksNow()
		{
			auto& picks = g_ed.picks;
			if (picks.empty()) return picks;
			if (PrimaryNow() != g_ed.pickPrimary) { picks.clear(); return picks; }
			const int n = g_ed.draft ? int(g_ed.draft->Value().points.size()) : 0;
			const bool pointsOk = g_ed.draft && n == g_ed.picksCount;
			picks.erase(std::remove_if(picks.begin(), picks.end(), [&](const Pick& p) {
				switch (p.kind) {
				case kPickPoint: return !pointsOk || !IsDraftRoute(p.id) || p.index < 0 || p.index >= n;
				case kPickLanding: return LandingIndex(p.id) < 0;
				case kPickSpawn: return SpawnIndex(p.id) < 0;
				case kPickZone: return ZoneIndex(p.id) < 0;
				case kPickLink: return LinkIndex(p.id) < 0;
				default: return !IsDraftRoute(p.id) && RouteIndex(p.id) < 0;
				}
			}), picks.end());
			if (picks.size() < 2) picks.clear();
			return picks;
		}
		bool Many() { return PicksNow().size() >= 2; }
		bool IsPicked(int kind, const std::string& id, int index = -1)
		{
			for (const auto& p : g_ed.picks) if (p.kind == kind && p.index == index && p.id == id) return true;
			return false;
		}
		std::string PicksWords()
		{
			int count[7] = {};
			for (const auto& p : PicksNow()) ++count[p.kind];
			static const char* const one[7] = { "", "point", "landing area", "spawn spot", "zone", "junction", "route" };
			std::string out = std::to_string(g_ed.picks.size()) + " selected:";
			bool first = true;
			for (int k = 1; k < 7; ++k) {
				if (!count[k]) continue;
				out += (first ? " " : ", ") + std::to_string(count[k]) + " " + one[k] + (count[k] > 1 ? "s" : "");
				first = false;
			}
			return out;
		}
		template <class R> void MoveWholeRoute(R& r, const routeeditor::GroupMove& m)
		{
			if (r.ring) { routeeditor::Apply(m, r.shape.cx, r.shape.cy, r.shape.z); r.shape.radius *= m.factor; return; }
			for (auto& p : r.points) routeeditor::Apply(m, p.x, p.y, p.z);
			if (m.factor != 1) for (auto& q : r.rounding) if (q > 0) q = std::clamp(q * m.factor, 5.0, 200.0);
		}
		bool DraftTouched()
		{
			for (const auto& p : PicksNow()) if (p.kind == kPickPoint || (p.kind == kPickRoute && IsDraftRoute(p.id))) return true;
			return false;
		}
		// `file` and `draft` are copies, or the real ones during a drag. A route selected whole carries its points, so they do not move twice.
		void ApplyGroup(routefiles::File& file, trafficdata::Route* draft, const routeeditor::GroupMove& m)
		{
			const auto& picks = PicksNow();
			bool whole = false;
			for (const auto& p : picks) if (p.kind == kPickRoute && IsDraftRoute(p.id)) whole = true;
			for (const auto& p : picks) {
				switch (p.kind) {
				case kPickPoint:
					if (draft && !whole) { auto& q = draft->points[std::size_t(p.index)]; routeeditor::Apply(m, q.x, q.y, q.z); }
					break;
				case kPickLanding: case kPickSpawn: {
					auto& list = p.kind == kPickLanding ? file.landings : file.spawns;
					auto& s = list[std::size_t(p.kind == kPickLanding ? LandingIndex(p.id) : SpawnIndex(p.id))];
					routeeditor::Apply(m, s.at.x, s.at.y, s.at.z);
					s.turn = std::fmod(s.turn + g_ed.groupTurn + 360.0, 360.0);
					s.heading = std::fmod(s.heading + g_ed.groupTurn + 360.0, 360.0);
					break;
				}
				case kPickZone: {
					auto& z = file.zones[std::size_t(ZoneIndex(p.id))];
					double h = z.slow ? z.at.z : z.top;
					routeeditor::Apply(m, z.at.x, z.at.y, h);
					(z.slow ? z.at.z : z.top) = h;
					break;
				}
				case kPickRoute:
					if (IsDraftRoute(p.id)) { if (draft) MoveWholeRoute(*draft, m); }
					else MoveWholeRoute(file.routes[std::size_t(RouteIndex(p.id))], m);
					break;
				default: break;  // a junction sits on its routes
				}
			}
		}
		// The middle of what is selected, as seen from above; z for the Route tool's drag plane.
		routeeditor::Point PicksMiddle()
		{
			double x = 0, y = 0, z = 0; int n = 0;
			auto add = [&](double px, double py, double pz) { x += px; y += py; z += pz; ++n; };
			auto addRoute = [&](const auto& r) {
				if (r.ring) { add(r.shape.cx, r.shape.cy, r.shape.z); return; }
				for (const auto& q : r.points) add(q.x, q.y, q.z);
			};
			for (const auto& p : PicksNow()) {
				switch (p.kind) {
				case kPickPoint: { const auto& q = g_ed.draft->Value().points[std::size_t(p.index)]; add(q.x, q.y, q.z); break; }
				case kPickLanding: { const auto& s = g_ed.file.landings[std::size_t(LandingIndex(p.id))]; add(s.at.x, s.at.y, s.at.z); break; }
				case kPickSpawn: { const auto& s = g_ed.file.spawns[std::size_t(SpawnIndex(p.id))]; add(s.at.x, s.at.y, s.at.z); break; }
				case kPickZone: { const auto& s = g_ed.file.zones[std::size_t(ZoneIndex(p.id))]; add(s.at.x, s.at.y, s.slow ? s.at.z : s.top); break; }
				case kPickRoute:
					if (IsDraftRoute(p.id)) addRoute(g_ed.draft->Value());
					else addRoute(g_ed.file.routes[std::size_t(RouteIndex(p.id))]);
					break;
				default: break;
				}
			}
			return n ? routeeditor::Point{ x / n, y / n, z / n } : routeeditor::Point{ 0, 0, 0 };
		}
		routeeditor::GroupMove AboutMiddle() { const auto c = PicksMiddle(); routeeditor::GroupMove m; m.cx = c.x; m.cy = c.y; return m; }
		double AboutMiddleZ() { return PicksMiddle().z; }
		// Everything that moved is somewhere allowed: else says why.
		bool GroupFits(const routefiles::File& file, const trafficdata::Route* draft, const char* what)
		{
			auto fits = [&](double x, double y, double z) {
				const char* why = PointProblem(x, y, z);
				if (why) Refused(what, x, y, z, why);
				return !why;
			};
			auto routeFits = [&](const auto& r) {
				if (r.ring) return true;
				for (const auto& q : r.points) if (!fits(q.x, q.y, q.z)) return false;
				return true;
			};
			for (const auto& p : PicksNow()) {
				switch (p.kind) {
				case kPickPoint: if (draft) { const auto& q = draft->points[std::size_t(p.index)]; if (!fits(q.x, q.y, q.z)) return false; } break;
				case kPickLanding: { const auto& s = file.landings[std::size_t(LandingIndex(p.id))]; if (!fits(s.at.x, s.at.y, s.at.z)) return false; break; }
				case kPickSpawn: { const auto& s = file.spawns[std::size_t(SpawnIndex(p.id))]; if (!fits(s.at.x, s.at.y, s.at.z)) return false; break; }
				case kPickRoute:
					if (IsDraftRoute(p.id)) { if (draft && !routeFits(*draft)) return false; }
					else if (!routeFits(file.routes[std::size_t(RouteIndex(p.id))])) return false;
					break;
				default: break;
				}
			}
			return true;
		}
		// Undo: a change to many is one step. Places and other routes live in the file, which has no undo of its own, so the file is kept here; the open route keeps its own
		// steps, and the depth says which of the two came last. (A single place changed after this comes back to how it was too.)
		std::size_t DraftDepth() { return g_ed.draft ? g_ed.draft->Depth() : 0; }
		void PushSnap()
		{
			if (g_ed.massUndo.size() >= 32) g_ed.massUndo.erase(g_ed.massUndo.begin());
			g_ed.massUndo.push_back({ g_ed.file, false, DraftDepth() });
			g_ed.massRedo.clear();
		}
		bool UndoAny()
		{
			if (!g_ed.massUndo.empty() && g_ed.massUndo.back().depth == DraftDepth()) {
				MassSnap s = std::move(g_ed.massUndo.back());
				g_ed.massUndo.pop_back();
				MassSnap redo{ g_ed.file, s.draftChanged, 0 };
				g_ed.file = std::move(s.file);
				if (s.draftChanged && g_ed.draft) g_ed.draft->Undo();
				redo.depth = DraftDepth();
				g_ed.massRedo.push_back(std::move(redo));
				return true;
			}
			return g_ed.draft && g_ed.draft->Undo();
		}
		bool RedoAny()
		{
			if (!g_ed.massRedo.empty() && g_ed.massRedo.back().depth == DraftDepth()) {
				MassSnap s = std::move(g_ed.massRedo.back());
				g_ed.massRedo.pop_back();
				MassSnap undo{ g_ed.file, s.draftChanged, 0 };
				g_ed.file = std::move(s.file);
				if (s.draftChanged && g_ed.draft) g_ed.draft->Redo();
				undo.depth = DraftDepth();
				g_ed.massUndo.push_back(std::move(undo));
				return true;
			}
			g_ed.massRedo.clear();
			return g_ed.draft && g_ed.draft->Redo();
		}
		// One whole change, checked first. merge: steps close together (a held key, the wheel) are one undo step.
		bool GroupStep(const routeeditor::GroupMove& m, const char* what, bool merge = false)
		{
			auto file = g_ed.file;
			std::optional<trafficdata::Route> next;
			if (g_ed.draft) next = g_ed.draft->Value();
			ApplyGroup(file, next ? &*next : nullptr, m);
			if (!GroupFits(file, next ? &*next : nullptr, what)) return false;
			const bool touches = DraftTouched();
			static ULONGLONG lastAt = 0;
			const ULONGLONG now = GetTickCount64();
			const bool join = merge && now - lastAt < 400 && !g_ed.massUndo.empty() && g_ed.massUndo.back().depth == DraftDepth();
			lastAt = now;
			if (!join) PushSnap();
			g_ed.file = std::move(file);
			if (touches && next) {
				if (join && g_ed.massUndo.back().draftChanged) g_ed.draft->Amend(*next);
				else { g_ed.draft->Edit(*next); g_ed.massUndo.back().draftChanged = true; }
				g_ed.massUndo.back().depth = DraftDepth();
			}
			Status(PicksWords() + " (" + what + "; Save keeps it)");
			return true;
		}
		// A drag or a held key: moves as it goes, checked at the end.
		void GroupBegin()
		{
			PushSnap();
			if (DraftTouched()) { g_ed.draft->Edit(g_ed.draft->Value()); g_ed.massUndo.back().draftChanged = true; g_ed.massUndo.back().depth = DraftDepth(); }
		}
		void GroupLive(const routeeditor::GroupMove& m)
		{
			std::optional<trafficdata::Route> next;
			const bool touches = DraftTouched();
			if (g_ed.draft) next = g_ed.draft->Value();
			ApplyGroup(g_ed.file, next ? &*next : nullptr, m);
			if (touches && next) g_ed.draft->Amend(*next);
		}
		void GroupDragTo(const routemap::Vec3& at)
		{
			routeeditor::GroupMove m;
			m.dx = at.x - g_ed.groupLast.x; m.dy = at.y - g_ed.groupLast.y; m.dz = at.z - g_ed.groupLast.z;
			g_ed.groupLast = at;
			if (m.dx != 0 || m.dy != 0 || m.dz != 0) GroupLive(m);
		}
		bool GroupEnd(const char* what)
		{
			if (GroupFits(g_ed.file, g_ed.draft ? &g_ed.draft->Value() : nullptr, what)) return true;
			if (UndoAny()) g_ed.massRedo.clear();
			return false;
		}
		// Asked twice, as a route's delete is. Nothing is written until Save, so Undo still brings it all back.
		void GroupDelete()
		{
			const ULONGLONG now = GetTickCount64();
			if (now - g_ed.confirmDeleteAt > 3000) { g_ed.confirmDeleteAt = now; Status("Delete " + PicksWords() + "? Press again within 3 s"); return; }
			g_ed.confirmDeleteAt = 0;
			const auto picks = PicksNow();
			std::vector<int> points;
			for (const auto& p : picks) if (p.kind == kPickPoint) points.push_back(p.index);
			std::sort(points.begin(), points.end());
			bool draftGoes = false;
			for (const auto& p : picks) if (p.kind == kPickRoute && IsDraftRoute(p.id)) draftGoes = true;
			if (!points.empty() && !draftGoes) {
				const auto& r = g_ed.draft->Value();
				if (r.points.size() - points.size() < (r.closed ? 3u : 2u)) { Status(std::string("That would leave too few points: a route needs ") + (r.closed ? "three" : "two")); return; }
			}
			const std::string words = PicksWords();
			PushSnap();
			if (!points.empty() && !draftGoes) {
				const auto next = routeeditor::WithoutPoints(g_ed.draft->Value(), points);
				const std::string name = next.name;
				std::size_t count = g_ed.draft->Value().points.size();
				g_ed.draft->Edit(next);
				g_ed.massUndo.back().draftChanged = true;
				g_ed.massUndo.back().depth = DraftDepth();
				for (auto i = points.rbegin(); i != points.rend(); ++i) LinksPointRemoved(name, std::size_t(*i), count--);
			}
			for (const auto& p : picks) {
				switch (p.kind) {
				case kPickLanding: g_ed.file.landings.erase(g_ed.file.landings.begin() + LandingIndex(p.id)); break;
				case kPickSpawn: g_ed.file.spawns.erase(g_ed.file.spawns.begin() + SpawnIndex(p.id)); break;
				case kPickZone: g_ed.file.zones.erase(g_ed.file.zones.begin() + ZoneIndex(p.id)); break;
				case kPickLink: if (const int k = LinkIndex(p.id); k >= 0) g_ed.file.links.erase(g_ed.file.links.begin() + k); break;
				case kPickRoute: {
					const int k = RouteIndex(p.id);
					auto& links = g_ed.file.links;
					links.erase(std::remove_if(links.begin(), links.end(), [&](const routefiles::Link& l) { return l.to == p.id || l.from == p.id; }), links.end());
					if (k >= 0) {
						auto& r = g_ed.file.routes[std::size_t(k)];
						if (IsOriginal(r)) { r.removed = true; r.points.clear(); r.rounding.clear(); }  // an original stays switched off, as Del route does
						else g_ed.file.routes.erase(g_ed.file.routes.begin() + k);
					}
					break;
				}
				default: break;
				}
			}
			if (draftGoes) { g_ed.draft.reset(); g_ed.draftIndex = -1; g_ed.massUndo.back().depth = 0; }
			g_ed.picks.clear();
			g_ed.route = g_ed.draft ? g_ed.route : -1; g_ed.other = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1;
			Status("Deleted " + words + ". Undo brings it back; Save keeps it.");
		}
		void ChangeZ(double dz)
		{
			if (Many()) { routeeditor::GroupMove m; m.dz = dz; GroupStep(m, "Height"); return; }
			if (g_ed.zone >= 0 && g_ed.zone < int(g_ed.file.zones.size())) {
				auto& z = g_ed.file.zones[std::size_t(g_ed.zone)];
				if (z.slow) { z.at.z += dz; Status("Zone height " + std::to_string(int(z.at.z)) + " m (Save to keep it)"); }
				else { z.top += dz; Status("Traffic stays above " + std::to_string(int(z.top)) + " m here (Save to keep it)"); }
				return;
			}
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
			if (Many()) { routeeditor::GroupMove m; m.dx = dx; m.dy = dy; GroupStep(m, "Move"); return; }
			char b[128];
			if (g_ed.zone >= 0 && g_ed.zone < int(g_ed.file.zones.size())) {
				auto& z = g_ed.file.zones[std::size_t(g_ed.zone)];
				z.at.x += dx; z.at.y += dy;
				std::snprintf(b, sizeof b, "Zone at (%.0f, %.0f) (Save to keep it)", z.at.x, z.at.y);
				Status(b);
				return;
			}
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
		int LanesOf(const std::string& name)
		{
			if (g_ed.draft && g_ed.draft->Value().name == name) return g_ed.draft->Value().lanes;
			for (const auto& r : g_ed.file.routes) if (r.id == name) return r.lanes;
			for (const auto& o : g_ed.others) if (o.name == name) return o.route.lanes;
			return 1;
		}
		// What this junction's shape asks for (trafficlinks::Suggested); empty: both ways.
		std::string SuggestedType(const routefiles::Link& k)
		{
			trafficroute::Line a, b; bool ringA = false, ringB = false;
			if (!LineOf(k.from, a, ringA) || !LineOf(k.to, b, ringB)) return {};
			const double ua = ringA ? k.at / 360.0 * trafficroute::kRingSamples : a.ToLine(k.at);
			const double ub = k.toEnd ? b.Span() : ringB ? k.toAt / 360.0 * trafficroute::kRingSamples : b.ToLine(k.toAt);
			return trafficlinks::Suggested(a, LanesOf(k.from), ua, b, LanesOf(k.to), ub);
		}
		std::string RouteTitle(const std::string& id)
		{
			if (g_ed.draft && g_ed.draft->Value().name == id && !g_ed.draft->Value().label.empty()) return g_ed.draft->Value().label;
			for (const auto& r : g_ed.file.routes) if (r.id == id && !r.label.empty()) return r.label;
			for (const auto& o : g_ed.others) if (o.name == id && !o.route.label.empty()) return o.route.label;
			return id;
		}
		std::string TypeWords(const routefiles::Link& k, const std::string& type)
		{
			if (type == "exit") return "Traffic on " + RouteTitle(k.from) + " turns onto " + RouteTitle(k.to) + ", never back.";
			if (type == "merge") return "Traffic on " + RouteTitle(k.to) + " turns onto " + RouteTitle(k.from) + ", never back.";
			if (type == "crossing") return "Nobody turns. They pass here and one slows for the other.";
			return "Traffic turns either way, by the share.";
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
			k.type = SuggestedType(k);  // from the shape of the meeting; the Junction panel changes it
			links.push_back(k);
			g_ed.link = int(links.size()) - 1; g_ed.point = int(idx); g_ed.spot = -1;
			const int joins = int(std::count_if(links.begin(), links.end(), [&](const routefiles::Link& o) { return o.to == me; }));
			Status("Joined point " + std::to_string(idx + 1) + (idx == 0 ? " (start)" : end ? " (end)" : "") + " of this route to " + other + " (" +
				std::to_string(joins) + " junction(s) on it). " + TypeWords(k, k.type) + " The Junction panel changes it. Select another point and Link again for more.");
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
			if (Many()) { GroupDelete(); return; }
			if (g_ed.zone >= 0 && g_ed.zone < int(g_ed.file.zones.size())) {
				g_ed.file.zones.erase(g_ed.file.zones.begin() + g_ed.zone);
				g_ed.zone = -1; Status("Zone removed (Save to keep it)"); return;
			}
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
		struct TextItem { std::string text; double x, y; unsigned color; std::size_t chars; bool map; double unit = 0; };  // unit 0: kUnit
		// Stage 2 styles: 0 panel button, 1 toolbar tool, 2 folded group header, 3 list row, 4 menu item, 5 segment, 6 problems badge, 7 delete.
		struct Button { int id; double x, y, w, h; std::string text; bool active, enabled; std::string key; int style = 0; std::string value; unsigned swatch = 0; };
		struct Box { double x, y, w, h; unsigned fill; double alpha; unsigned line = 0; double lineAlpha = 0; };
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
			std::vector<Box> boxes;             // stage 2: row backgrounds and swatches, drawn before the buttons
			double barX = 494, barY = 50, barW = 0, barH = 44, menuX = 0, menuY = 0, menuW = 0, menuH = 0;
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
		void Text(uptr, const std::string& text, double x, double y, unsigned color, double width = 300, bool map = false, double unit = kUnit)
		{
			g_ui.items.push_back({ text, x, y + 5, color, strokefont::Fit(width, unit), map, unit });
		}
		double TextWidth(const std::string& t, double unit = kUnit) { return double(t.size()) * strokefont::kAdvance * unit; }  // 8.25 px a letter at kUnit
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
			const double unit = t.unit > 0 ? t.unit : kUnit;
			const auto lines = strokefont::Layout(t.text, t.x, t.y, unit, t.chars);
			for (int pass = outline ? 0 : 1; pass < 2; ++pass) {
				pen.Style(pass ? 1.4 * unit / kUnit : 3.6, pass ? t.color : 0x0B1620, pass ? 1.0 : 0.85);
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
				if (!t.map) { sig += t.text; sig += '\x1f'; sig += std::to_string(int(t.x)) + "," + std::to_string(int(t.y)) + "," + std::to_string(t.color) + "," + std::to_string(int(t.unit * 10)) + ";"; }
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
				Status("In combat: traffic can't be reset. Exit combat first.");
				notify::Show("In combat: traffic can't be reset. Exit combat first.");
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
			kEditOriginal, kRadiusDown, kRadiusUp, kBake, kMoveLeft, kMoveRight, kMoveForward, kMoveBack, kStartSwap, kRoundPoint, kRoundDown, kRoundUp, kRename, kColor,
			// Stage 2 (2026-10-30): the Route tool, the toolbar menus, the folded groups and the panel's two-way choices.
			kRouteTool, kMoreMenu, kViewMenu, kCopy, kMirror, kGroupFlow, kGroupPosition, kGroupRules, kProblems,
			kRouteUp, kRouteDown, kTurnLeft, kTurnRight, kOneWay, kTwoWay, kLoopOn, kLoopOff, kAllDay, kByHours, kSharp, kRounded,
			// Traffic Manager V1 (2026-10-30): vehicle rules.
			kVehicleRules, kAllowAll, kSheetBack, kTypesMore, kSplit, kJoin, kRestore,
			kJunctionBoth, kJunctionExit, kJunctionMerge, kJunctionCross, kJunctionSuggest,
			kShapeTool, kZoneTool, kTimeRules, kByWords, kWordsAdd, kBlendQuick, kBlendGentle, kBlendSlow, kRoleNext,
			kCity, kCityOn, kCityOff, kDensityDown, kDensityUp, kLimitDown, kLimitUp, kOriginalsOn, kOriginalsOff,
			kPlaceDone, kLayoutSingle, kLayoutRow, kLayoutBays, kLayoutRing, kSpacesDown, kSpacesUp, kAreaTurnDown, kAreaTurnUp,
			kPurposeTaxi, kPurposeDepot, kPurposeStop, kStayShort, kStayLong, kStayNeeded, kFullQueue, kFullCircle, kFullOn, kWhoParks,
			kZoneKeep, kZoneSlow, kZoneSizeDown, kZoneSizeUp, kZoneTopDown, kZoneTopUp, kZoneSpeedDown, kZoneSpeedUp,
			kGroupNext, kGroupSheet, kGroupNew, kGroupRename, kGroupDelete,
			kSizeDown, kSizeUp, kShapeADown, kShapeAUp, kShapeBDown, kShapeBUp, kShapeCDown, kShapeCUp,
			kReadme,
			kDayRow = 5000,    // + 4 x a zone of the day: earlier, later, next word, remove
			kLevelBase = 5500, // + 2 x a word: less, more
			kPlaceRow = 5600,  // + a line of the places list (landing areas, then zones)
			kShapeBase = 5900, // + a shape; the last is parallel lanes
			kGroupRow = 6000,  // + 0 all routes, + 1 + a group (route groups sheet)
			kProblemShow = 4000, kProblemFix = 4500,  // + a problem's place in the list
			kTypeBase = 3000,  // + a vehicle type in the vehicle rules sheet
			kRowBase = 1000,   // + index in the routes list (editor routes, then the other files' routes)
			kViewBase = 2000,  // + View item
		};
		// Keyboard shortcuts (2026-10-29, the user: "keyboard shortcuts for the traffic menu controls and they should show beside them").
		// Letters the map does not use while the editor holds it (inputgate: only Tab, Esc, WASD and the arrows reach the map); never
		// number keys or the in-game test keys , . / '. Shown beside each tool and button; Ctrl and the older keys (Insert, Delete,
		// PgUp / PgDn, [ ], Home / End) keep working. Stage 2 adds M (the Route tool) and Y (View); V stays Reverse as before.
		struct Shortcut { int vk; int id; const char* label; };
		const Shortcut kShortcuts[] = {
			{ 'Q', kSelect, "Q" }, { 'E', kDrop, "E" }, { 'R', kDrag, "R" }, { 'M', kRouteTool, "M" }, { 'C', kConnect, "C" }, { 'G', kLanding, "G" },
			{ 'F', kShapeTool, "F" },  // not S: S moves the map
			{ 'N', kNew, "N" }, { 'O', kPrevRoute, "O" }, { 'P', kNextRoute, "P" }, { 'X', kStartSwap, "X" },
			{ 'I', kMoveForward, "I" }, { 'J', kMoveLeft, "J" }, { 'K', kMoveBack, "K" }, { 'L', kMoveRight, "L" },
			{ 'V', kReverse, "V" }, { 'B', kLoop, "B" }, { 'H', kWays, "H" }, { 'T', kTest, "T" }, { 'Y', kViewMenu, "Y" },
			{ VK_OEM_MINUS, kVehDown, "-" }, { VK_OEM_PLUS, kVehUp, "=" }, { VK_F2, kRename, "F2" }, { 'U', kColor, "U" },
		};
		const char* KeyOf(int id)
		{
			for (const auto& k : kShortcuts) if (k.id == id) return k.label;
			switch (id) {
			case kSave: return "Ctrl+S"; case kUndo: return "Ctrl+Z"; case kRedo: return "Ctrl+Y";
			case kZUp: return "PgU"; case kZDown: return "PgD"; case kDeletePoint: return "Del"; case kZoneTool: return "Z";
			default: return "";
			}
		}

		// ---- Stage 2 layout (2026-10-30, the user approved notes/traffic-manager-mockup): a toolbar over the map, and a short
		// panel on the right that shows only what is selected. Its groups fold to one line (one open at a time); the key
		// hints show only the tool in use. Drawn with the overlay's own lines and stroke font (the SWF panel is untested).
		constexpr double kPanelW = 380;
		constexpr double kInner = kPanelW - 28;      // content width
		constexpr double kLabelW = 130;              // a row's label cell
		constexpr unsigned kTeal = 0x6FE0D0, kText = 0xDFE7EE, kDim = 0x8C9AAB, kAmber = 0xF2B04E, kCoral = 0xE8705C, kRowA = 0x27303F, kRowB = 0x1A2331;
		double PanelX() { return g_ui.width - kPanelW - 12; }
		bool OverPanel(double x, double y) { return x >= PanelX() && x <= PanelX() + kPanelW && y >= 12 && y <= g_ui.panelBottom; }
		bool OverUi(double x, double y)
		{
			if (OverPanel(x, y)) return true;
			if (x >= g_ui.barX && x <= g_ui.barX + g_ui.barW && y >= g_ui.barY && y <= g_ui.barY + g_ui.barH) return true;
			return g_ed.menu && x >= g_ui.menuX && x <= g_ui.menuX + g_ui.menuW && y >= g_ui.menuY && y <= g_ui.menuY + g_ui.menuH;
		}
		const Button* ButtonAt(double x, double y)
		{
			// Menus first: they lie over the panel and the map.
			for (const auto& b : g_ui.buttons) if (b.style == 4 && x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return &b;
			for (const auto& b : g_ui.buttons) if (b.style != 4 && x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) return &b;
			return nullptr;
		}
		// ---- Vehicle rules (2026-10-30, the user: an exclusion list per route) ----
		// The vehicle models of this load order that can carry a driver (the same list traffic picks from: the game's vehicle
		// keyword, a driver's seat), one line per name: variants of one model share it and are left out together.
		struct VehicleType { std::string name; std::vector<unsigned> ids; };
		std::vector<VehicleType> g_types;
		ULONGLONG g_typesAt = 0;
		const char* FormNameRaw(uptr form)
		{
			__try { return reinterpret_cast<const char* (*)(uptr)>((*reinterpret_cast<void***>(form))[0x268 / 8])(form); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}
		// A vehicle's own name, the plugin's FULL, as the game shows it when buying one (2026-10-30, the user: the list showed form ids). Vehicles are
		// TESFurniture; the display-name call above returns nothing on a base form. In 1.16.244 its RTTI puts TESFullName at +0xE8, and that holds a
		// BSFixedString at +8: an entry whose text starts at +0x18 (flag 2 at +0x14: the real entry is at +8). Null unless it reads as text.
		const char* FullNameRaw(uptr form)
		{
			__try {
				uptr entry = *reinterpret_cast<const uptr*>(form + 0xE8 + 8);
				if (entry < 0x10000) return nullptr;
				for (int hops = 0; hops < 8 && (*reinterpret_cast<const std::uint8_t*>(entry + 0x14) & 2); ++hops) entry = *reinterpret_cast<const uptr*>(entry + 8);
				const char* text = reinterpret_cast<const char*>(entry + 0x18);
				int length = 0;
				for (; length < 128 && text[length]; ++length) if (static_cast<unsigned char>(text[length]) < 0x20) return nullptr;
				return length > 0 && length < 128 ? text : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
		}
		unsigned FormIdRaw(uptr form)
		{
			__try { return *reinterpret_cast<const std::uint32_t*>(form + off::kFormId); }
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
		}
		const std::vector<VehicleType>& VehicleTypesHere()
		{
			if (!g_types.empty() && GetTickCount64() - g_typesAt < 30000) return g_types;
			g_types.clear(); g_typesAt = GetTickCount64();
			for (const uptr form : realvehicles::VehicleTypes()) {
				const unsigned id = FormIdRaw(form);
				if (!id || !npcvehicles::BaseHasDriverSeat(form)) continue;
				const char* raw = FullNameRaw(form);
				if (!raw) raw = FormNameRaw(form);
				std::string name = raw && raw[0] ? raw : std::string();
				if (name.empty()) { char b[16]; std::snprintf(b, sizeof b, "%08X", id); name = b; logger::Write("Route editor: vehicle type %08X has no name to show", id); }
				auto it = std::find_if(g_types.begin(), g_types.end(), [&](const VehicleType& t) { return t.name == name; });
				if (it == g_types.end()) g_types.push_back({ name, { id } }); else it->ids.push_back(id);
			}
			std::sort(g_types.begin(), g_types.end(), [](const VehicleType& a, const VehicleType& b) { return a.name < b.name; });
			return g_types;
		}
		// A type is left out when every one of its ids is in the list.
		bool TypeIn(const std::vector<unsigned>& list, const VehicleType& t)
		{
			for (const unsigned id : t.ids) if (!trafficdata::InList(list, id)) return false;
			return !t.ids.empty();
		}
		bool TypeLeftOut(const trafficdata::Route& r, const VehicleType& t) { return TypeIn(r.exclude, t); }
		// Left out from above: "group" or "all routes", or nothing.
		std::string TypeAbove(const trafficdata::Route& r, const VehicleType& t)
		{
			for (const auto& g : g_ed.routeGroups) if (g.name == trafficdata::GlobalGroup() && TypeIn(g.exclude, t)) return "left out for all routes";
			for (const auto& g : g_ed.routeGroups) if (!r.group.empty() && g.name == r.group && TypeIn(g.exclude, t)) return "left out by " + g.label;
			return {};
		}
		std::string VehicleRulesText(const trafficdata::Route& r)
		{
			const auto all = trafficdata::AllLeftOut(g_ed.routeGroups, r);
			if (all.empty()) return "All types";
			const auto& types = VehicleTypesHere();
			int out = 0;
			for (const auto& t : types) if (TypeIn(all, t)) ++out;
			return types.empty() ? std::to_string(all.size()) + " left out" : std::to_string(int(types.size()) - out) + " of " + std::to_string(types.size()) + " types";
		}
		// Route groups (2026-10-31, the user). "global" is the all-routes row, not a group you can join.
		trafficdata::RouteGroup* GroupByName(const std::string& name)
		{
			for (auto& g : g_ed.routeGroups) if (g.name == name) return &g;
			return nullptr;
		}
		std::vector<const trafficdata::RouteGroup*> ChoosableGroups()
		{
			std::vector<const trafficdata::RouteGroup*> out;
			for (const auto& g : g_ed.routeGroups) if (g.name != trafficdata::GlobalGroup()) out.push_back(&g);
			return out;
		}
		std::string GroupLabel(const std::string& name)
		{
			if (name.empty()) return "None";
			const auto* g = GroupByName(name);
			return g ? g->label : "None";
		}
		// The list the vehicle rules sheet is showing when it is not a route's; made when first needed.
		trafficdata::RouteGroup* RuleTarget()
		{
			const std::string name = g_ed.ruleScope == 2 ? std::string(trafficdata::GlobalGroup()) : g_ed.ruleGroup;
			if (auto* g = GroupByName(name)) return g;
			if (g_ed.ruleScope != 2) return nullptr;
			g_ed.routeGroups.push_back({ name, "All routes", {} });
			return &g_ed.routeGroups.back();
		}

		// The routes list: editor routes (removal markers too), then the routes of other files.
		int ListTotal() { return int(g_ed.file.routes.size() + g_ed.others.size()); }
		int ListSelected() { return g_ed.route >= 0 ? g_ed.route : g_ed.other >= 0 ? int(g_ed.file.routes.size()) + g_ed.other : g_ed.draft && g_ed.draftIndex < 0 ? -2 : -1; }
		// ---- Problems with one-click fixes (2026-10-30, Traffic Manager V1) ----
		// kind: 0 none, 1 set points a and b to z, 2 set point b to z, 3 remove point b, 4 round point a, 5 set point a to z, 6 move a and b up by z.
		struct Problem { std::string text, fix; int kind = 0, a = -1, b = -1; double z = 0; };
		// Where a segment crosses another route (on the map), the two heights there: true when found.
		bool CrossAt(const trafficdata::Vec& a, const trafficdata::Vec& b, const trafficdata::Vec& c, const trafficdata::Vec& d, double& ha, double& hc, double* xAt = nullptr, double* yAt = nullptr)
		{
			const double rx = b.x - a.x, ry = b.y - a.y, sx = d.x - c.x, sy = d.y - c.y, den = rx * sy - ry * sx;
			if (std::fabs(den) < 1e-9) return false;
			const double t = ((c.x - a.x) * sy - (c.y - a.y) * sx) / den, u = ((c.x - a.x) * ry - (c.y - a.y) * rx) / den;
			if (t < 0 || t > 1 || u < 0 || u > 1) return false;
			ha = a.z + (b.z - a.z) * t; hc = c.z + (d.z - c.z) * u;
			if (xAt) *xAt = a.x + rx * t;
			if (yAt) *yAt = a.y + ry * t;
			return true;
		}
		// Two routes that meet where a junction joins them: the crossing is on purpose (within 60 m of that junction).
		bool JoinedAt(const std::string& a, const std::string& b, double x, double y)
		{
			auto closeBy = [&](const routefiles::Link& k) {
				if (!((k.from == a && k.to == b) || (k.from == b && k.to == a))) return false;
				trafficroute::Vec at;
				return JunctionAt(k, at) && std::hypot(at.x - x, at.y - y) < 60.0;
			};
			for (const auto& k : g_ed.file.links) if (closeBy(k)) return true;
			for (const auto& k : g_ed.othersData.links) if (closeBy({ k.name, k.from, k.to, k.at, k.toEnd, k.share, k.toAt, k.radius, k.type })) return true;
			return false;
		}
		// deep: also what the rays know (roofs, bridges, the ground, sampled along each stretch) and other routes crossing too close.
		// Used for the selected route; the list's "!" uses the quick checks only.
		std::vector<Problem> Problems(const trafficdata::Route& r, bool deep = false)
		{
			std::vector<Problem> out;
			if (r.ring) return out;
			const auto& p = r.points;
			const int n = int(p.size());
			const int segs = r.closed ? n : n - 1;
			char b[160];
			for (int i = 0; i < segs && n >= 2; ++i) {
				const int j = (i + 1) % n;
				const double flat = std::hypot(p[j].x - p[i].x, p[j].y - p[i].y), dz = p[j].z - p[i].z;
				if (flat < 15) {
					std::snprintf(b, sizeof b, "Points %d and %d are under 15 m apart", i + 1, j + 1);
					if (n > (r.closed ? 3 : 2)) out.push_back({ b, "Remove " + std::to_string(j + 1), 3, i, j });
					else out.push_back({ b, "", 0, i, j });
				}
				else if (std::fabs(dz) > flat) {
					std::snprintf(b, sizeof b, "The climb from point %d to %d is steeper than 45 deg (%.0f m up over %.0f m)", i + 1, j + 1, std::fabs(dz), flat);
					out.push_back({ b, "Ease it", 2, i, j, p[i].z + (dz > 0 ? 1 : -1) * std::floor(flat * 0.8) });
				}
			}
			for (int i = 0; i < n && n > 2; ++i) {
				if (!r.closed && (i == 0 || i + 1 == n)) continue;
				const auto& a = p[(i + n - 1) % n]; const auto& c = p[(i + 1) % n];
				const double ux = p[i].x - a.x, uy = p[i].y - a.y, vx = c.x - p[i].x, vy = c.y - p[i].y, lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
				const double radius = std::size_t(i) < r.rounding.size() ? r.rounding[std::size_t(i)] : 0;
				if (lu > 1e-6 && lv > 1e-6 && (ux * vx + uy * vy) / (lu * lv) < -0.5 && radius <= 0) {
					std::snprintf(b, sizeof b, "A sharp turn (over 120 deg) at point %d", i + 1);
					out.push_back({ b, "Round it", 4, i, -1 });
				}
			}
			// Keep-out zones: a stretch below a zone's top inside its circle (sampled, the ends too).
			for (const auto& z : g_ed.file.zones) {
				if (z.slow) continue;
				for (int i = 0; i < segs && n >= 2; ++i) {
					const int j = (i + 1) % n;
					bool through = false;
					for (int k = 0; k <= 10 && !through; ++k) {
						const double t = k / 10.0, x = p[i].x + (p[j].x - p[i].x) * t, y = p[i].y + (p[j].y - p[i].y) * t, h = p[i].z + (p[j].z - p[i].z) * t;
						through = std::hypot(x - z.at.x, y - z.at.y) <= z.radius && h < z.top;
					}
					if (!through) continue;
					std::snprintf(b, sizeof b, "Points %d to %d fly through a keep-out zone", i + 1, j + 1);
					std::snprintf(b + 120, 40, "Raise to %.0f m", z.top + 10);
					out.push_back({ b, b + 120, 1, i, j, z.top + 10 });
				}
			}
			if (!deep) return out;
			// Roofs, bridges and the ground: at each point, and sampled along each stretch (about every 20 m, at most 10 a stretch).
			for (int i = 0; i < n; ++i) {
				const auto& s = SurfaceUnder(p[i].x, p[i].y, p[i].z);
				if (!s.covered && !s.buried) continue;
				std::snprintf(b, sizeof b, s.buried ? "Point %d is inside the ground" : "Point %d is under a roof or a bridge", i + 1);
				std::snprintf(b + 120, 40, "Raise to %.0f m", s.above + 10);
				out.push_back({ b, b + 120, 5, i, -1, s.above + 10 });
			}
			for (int i = 0; i < segs && n >= 2; ++i) {
				const int j = (i + 1) % n;
				const double flat = std::hypot(p[j].x - p[i].x, p[j].y - p[i].y);
				const int m = std::clamp(int(flat / 20.0), 1, 10);
				double top = -1e18;
				for (int k = 1; k <= m; ++k) {
					const double t = double(k) / (m + 1);
					const auto& s = SurfaceUnder(p[i].x + (p[j].x - p[i].x) * t, p[i].y + (p[j].y - p[i].y) * t, p[i].z + (p[j].z - p[i].z) * t);
					if (s.covered || s.buried) top = std::max(top, s.above);
				}
				if (top > -1e17) {
					std::snprintf(b, sizeof b, "Points %d to %d pass through a building, a bridge or the ground", i + 1, j + 1);
					std::snprintf(b + 120, 40, "Raise both to %.0f m", top + 10);
					out.push_back({ b, b + 120, 1, i, j, top + 10 });
				}
			}
			// Other routes crossing within 15 m of height (their lines as traffic flies them).
			std::vector<std::pair<std::string, trafficroute::Line>> others;
			std::vector<std::string> otherIds;
			for (std::size_t k = 0; k < g_ed.file.routes.size(); ++k) {
				const auto& o = g_ed.file.routes[k];
				if ((g_ed.draft && int(k) == g_ed.draftIndex) || (!g_ed.draft && int(k) == g_ed.route) || !Usable(o) || o.id == r.name) continue;
				others.push_back({ o.label.empty() ? o.id : o.label, trafficroute::Build(ToTd(o, EditorKey())) });
				otherIds.push_back(o.id);
			}
			for (std::size_t k = 0; k < g_ed.others.size(); ++k)
				if (!(g_ed.route < 0 && !g_ed.draft && int(k) == g_ed.other) && g_ed.others[k].name != r.name) {
					others.push_back({ g_ed.others[k].route.label.empty() ? g_ed.others[k].name : g_ed.others[k].route.label, g_ed.others[k].line });
					otherIds.push_back(g_ed.others[k].name);
				}
			for (int i = 0; i < segs && n >= 2; ++i) {
				const int j = (i + 1) % n;
				bool found = false;
				for (std::size_t w = 0; w < others.size(); ++w) {
					const auto& [name, l] = others[w];
					const int count = l.Count(), lsegs = l.closed ? count : count - 1;
					for (int q = 0; q < lsegs && !found; ++q) {
						const auto c0 = Middle(l, q), c1 = Middle(l, (q + 1) % count);
						double ha = 0, hc = 0, cx = 0, cy = 0;
						if (!CrossAt(p[i], p[j], { c0.x, c0.y, c0.z }, { c1.x, c1.y, c1.z }, ha, hc, &cx, &cy) || std::fabs(ha - hc) >= 15) continue;
						if (JoinedAt(r.name, otherIds[w], cx, cy)) continue;  // a junction joins them here: that is what it is for
						std::snprintf(b, sizeof b, "Points %d to %d cross %s only %.0f m apart in height", i + 1, j + 1, name.c_str(), std::fabs(ha - hc));
						out.push_back({ b, "Separate them", 6, i, j, hc + 20 - ha });
						found = true;
					}
					if (found) break;
				}
			}
			return out;
		}
		void FixProblem(const Problem& pb)
		{
			if (!pb.kind || !EnsureDraft()) return;
			auto r = g_ed.draft->Value();
			const int n = int(r.points.size());
			auto ok = [&](int i) { return i >= 0 && i < n; };
			switch (pb.kind) {
			case 3:
				if (ok(pb.b)) { g_ed.draft->Remove(std::size_t(pb.b)); LinksPointRemoved(r.name, std::size_t(pb.b), std::size_t(n)); g_ed.point = std::min(pb.a, n - 2); Status("Fixed: point " + std::to_string(pb.b + 1) + " removed (Save to keep it)"); }
				return;
			case 4:
				if (ok(pb.a) && g_ed.draft->Round(std::size_t(pb.a), 30)) { g_ed.point = pb.a; Status("Fixed: the corner at point " + std::to_string(pb.a + 1) + " is rounded, radius 30 m"); }
				return;
			case 1: if (ok(pb.a) && ok(pb.b)) { r.points[std::size_t(pb.a)].z = pb.z; r.points[std::size_t(pb.b)].z = pb.z; } break;
			case 2: if (ok(pb.b)) r.points[std::size_t(pb.b)].z = pb.z; break;
			case 5: if (ok(pb.a)) r.points[std::size_t(pb.a)].z = pb.z; break;
			case 6: if (ok(pb.a) && ok(pb.b)) { r.points[std::size_t(pb.a)].z += pb.z; r.points[std::size_t(pb.b)].z += pb.z; } break;
			default: return;
			}
			g_ed.draft->Edit(r);
			g_ed.point = ok(pb.b) ? pb.b : pb.a;
			Status("Fixed: " + pb.fix + ". Undo brings it back; Save keeps it.");
		}
		struct PanelText { std::string text; double x, y, w; unsigned color; double unit; };
		std::vector<PanelText> g_panelText;
		bool AreaOn() { return g_ed.spot >= 0 && g_ed.spotKind == 1 && g_ed.spot < int(g_ed.file.landings.size()); }
		bool ZoneOn() { return g_ed.zone >= 0 && g_ed.zone < int(g_ed.file.zones.size()); }
		trafficdata::Place PlaceOf(const routefiles::Spot& s)
		{
			trafficdata::Place p;
			p.at = s.at; p.layout = s.layout; p.spaces = s.spaces; p.turn = s.turn;
			return p;
		}
		std::string RouteLabel(const std::string& id)
		{
			for (const auto& r : g_ed.file.routes) if (r.id == id) return r.label.empty() ? r.id : r.label;
			for (const auto& o : g_ed.others) if (o.name == id) return o.route.label.empty() ? o.name : o.route.label;
			return id;
		}
		std::string AreaWords(const routefiles::Spot& s)
		{
			const std::string purpose = s.purpose == "taxi" ? "Taxi" : s.purpose == "depot" ? "Depot" : "Stop";
			return purpose + ", " + std::to_string(s.layout == "single" ? 1 : s.spaces) + (s.layout == "single" || s.spaces == 1 ? " space" : " spaces");
		}
		std::string ZoneWords(const routefiles::Zone& z)
		{
			char b[48];
			if (z.slow) std::snprintf(b, sizeof b, "Slow, %.0f m/s", z.speed); else std::snprintf(b, sizeof b, "Keep out to %.0f m", z.top);
			return b;
		}
		unsigned WordColor(int word)
		{
			static const unsigned colors[routeschedule::kWords] = { 0x2A3442, 0x2F6F69, 0x46A79C, 0xF2B04E, 0xE8705C };
			return colors[std::clamp(word, 0, routeschedule::kWords - 1)];
		}
		std::string RouteVehicles(const trafficdata::Route& r) { if (!r.schedule.zones.empty()) return std::to_string(r.schedule.peak); return std::to_string(r.schedule.allDay ? r.schedule.allDayVehicles : r.schedule.windows.empty() ? 0 : r.schedule.windows.front().vehicles); }
		// One layout of everything; returns the panel's bottom. maxRows: lines of the routes list; open: the groups drawn open.
		double LayoutPass(const trafficdata::Route* shown, bool timeKnown, int minute, int maxRows, int open)
		{
			auto& b = g_ui.buttons;
			b.clear(); g_ui.boxes.clear(); g_panelText.clear();
			auto text = [&](const std::string& t, double x, double y, double w, unsigned color, double unit = kUnit) { g_panelText.push_back({ t, x, y, w, color, unit }); };
			auto button = [&](int id, double x, double y, double w, double h, const std::string& t, bool active, bool enabled, int style, const std::string& value = {}, unsigned swatch = 0) {
				b.push_back({ id, x, y, w, h, t, active, enabled, KeyOf(id), style, value, swatch });
			};
			const bool draft = bool(g_ed.draft), route = shown != nullptr, ring = shown && shown->ring;
			const Mode m = g_ed.mode;

			// -- the toolbar, over the map at the top left of the free area --
			g_ui.barX = std::max(24.0, g_ui.width * 494.0 / 1920.0); g_ui.barY = 50;
			{
				const double w = 70, h = g_ui.barH, gap = 8;
				double x = g_ui.barX;
				auto tool = [&](int id, const char* label, bool active, bool enabled) { button(id, x, g_ui.barY, w, h, label, active, enabled, 1); x += w; };
				tool(kSelect, "Select", m == Mode::Select, true);
				tool(kDrop, "Add", m == Mode::Drop, true);
				tool(kDrag, "Move", m == Mode::Drag, true);
				tool(kRouteTool, "Route", m == Mode::Route, true);
				tool(kConnect, "Link", m == Mode::Connect, true);
				const double shapeX = x;
				tool(kShapeTool, "Shape", m == Mode::Shape || g_ed.menu == 3, true);
				tool(kLanding, "Land", m == Mode::Landing, true);
				tool(kZoneTool, "Zone", m == Mode::Zone, true);
				x += gap;
				tool(kUndo, "Undo", false, draft || !g_ed.massUndo.empty());
				tool(kRedo, "Redo", false, draft || !g_ed.massRedo.empty());
				tool(kMoreMenu, "More", g_ed.menu == 1, true);
				const double viewX = x;
				tool(kViewMenu, "View", g_ed.menu == 2, true);
				g_ui.barW = x - g_ui.barX;
				// -- the open menu, under its button --
				g_ui.menuW = 0; g_ui.menuH = 0;
				if (g_ed.menu) {
					const double mw = 270, mh = 28;
					g_ui.menuX = g_ed.menu == 3 ? shapeX : g_ed.menu == 1 ? viewX - w : viewX; g_ui.menuY = g_ui.barY + h + 4; g_ui.menuW = mw;
					double y = g_ui.menuY;
					auto item = [&](int id, const std::string& label, bool enabled, const std::string& value = {}) { button(id, g_ui.menuX, y, mw, mh, label, false, enabled, 4, value); y += mh; };
					if (g_ed.menu == 3) {
						item(kShapeBase + 0, "Circle", !draft, "two clicks");
						item(kShapeBase + 1, "Ellipse", !draft, "two clicks");
						item(kShapeBase + 2, "Rectangle", !draft, "two clicks");
						item(kShapeBase + 3, "Helix", !draft, "2 turns, climbs");
						item(kShapeBase + 4, "Ring a building", !draft, "clears its top");
						item(kShapeBase + 5, "Parallel lanes", route && !ring && !draft, "beside the route");
					}
					else if (g_ed.menu == 1) {
						item(kCopy, "Copy route", route && !draft, "beside it");
						item(kMirror, "Mirror", route && !ring, "left - right");
						item(kReverse, "Reverse direction", route && !ring);
						item(kEditOriginal, "Edit this original", !draft && g_ed.route < 0 && g_ed.other >= 0);
						item(kDeleteRoute, "Delete route", route || (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size())), "click twice");
						item(kSplit, "Disconnect", true, "at a point or junction");
						item(kJoin, "Merge routes", true, "selected, or nearest");
						item(kGroupSheet, "Route groups", true, "vehicle rules for many");
						item(kRestore, "Restore original", g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size()) && IsOriginal(g_ed.file.routes[std::size_t(g_ed.route)]) && !g_ed.file.routes[std::size_t(g_ed.route)].removed, "drops your edits");
						item(kStartSwap, "Reset traffic", !draft, "clears all, then new");
					}
					else {
						const View& v = g_ed.view;
						auto flag = [&](int i, const char* label, bool on) { item(kViewBase + i, std::string(on ? "[x] " : "[ ] ") + label, true); };
						flag(0, "Other routes", v.others); flag(1, "Vehicles", v.flow); flag(2, "Height lines", v.stems);
						flag(3, "Point numbers", v.numbers); flag(4, "Route names", v.names); flag(5, "Spots and junctions", v.spots);
						item(kViewBase + 6, "Reset map angle", true, "R-drag turns");
					}
					g_ui.menuH = y - g_ui.menuY;
				}
			}

			// -- the panel --
			const double px = PanelX(), x0 = px + 14, cx = x0 + kLabelW + 4, cw = kInner - kLabelW - 4;
			double y = 12;
			text("TRAFFIC MANAGER", x0, y + 12, 300, 0xFFFFFF, 2.3);
			text(g_ed.world, x0, y + 40, 210, kDim);
			text("F7 close", px + kPanelW - 14 - TextWidth("F7 close"), y + 12, 80, kDim);
			button(kReadme, px + kPanelW - 14 - 128, y + 34, 64, 22, "Readme", false, true, 0);
			button(kCity, px + kPanelW - 14 - 60, y + 34, 60, 22, "City", g_ed.sheet == 3, true, 0);
			y += 66;
			auto section = [&](const std::string& title, const std::string& right, unsigned color = kText) {
				g_ui.boxes.push_back({ px, y - 6, kPanelW, 1, 0x3A4658, 1 });
				text(title, x0, y + 2, kInner - TextWidth(right) - 8, color);
				if (!right.empty()) text(right, x0 + kInner - TextWidth(right), y + 2, TextWidth(right) + 4, kDim);
				y += 22;
			};
			auto cells = [&](const std::string& label, double h = 26) {
				g_ui.boxes.push_back({ x0, y, kLabelW, h, kRowA, 1 });
				g_ui.boxes.push_back({ cx - 2, y, cw + 2, h, kRowB, 1 });
				text(label, x0 + 8, y + (h - 16) / 2, kLabelW - 12, 0xCDD6DF);
			};
			auto stepper = [&](const std::string& label, int down, const std::string& value, int up, bool enabled) {
				cells(label);
				button(down, cx + cw - 26 - 78 - 26 - 4, y + 2, 26, 22, "-", false, enabled, 0);
				text(value, cx + cw - 30 - (78 + TextWidth(value)) / 2, y + 5, 80, enabled ? kText : kDim);
				button(up, cx + cw - 28, y + 2, 26, 22, "+", false, enabled, 0);
				y += 28;
			};
			auto choice = [&](const std::string& label, std::initializer_list<std::tuple<int, const char*, bool>> items, bool enabled) {
				cells(label);
				const double n = double(items.size()), w = (cw - 6) / n;
				double x = cx + 2;
				for (const auto& [id, t, on] : items) { button(id, x, y + 2, w - 2, 22, t, on, enabled, 5); x += w; }
				y += 28;
			};
			auto action = [&](const std::string& label, const std::string& value, int id, const char* t, bool enabled, unsigned swatch = 0) {
				cells(label);
				double vx = cx + 6;
				if (swatch) { g_ui.boxes.push_back({ vx, y + 7, 24, 12, swatch, 1, 0x0B1620, 1 }); vx += 32; }
				text(value, vx, y + 5, cw - 96 - (vx - cx), kText);
				button(id, cx + cw - 86, y + 2, 84, 22, t, false, enabled, 0);
				y += 28;
			};
			auto line = [&](const std::string& t, unsigned color) {
				for (const auto& l : strokefont::Wrap(t, strokefont::Fit(kInner, kUnit))) { text(l, x0, y, kInner, color); y += kLine; }
				y += 2;
			};

			const bool areaOn = AreaOn(), zoneOn = ZoneOn();
			if (g_ed.sheet == 3) {
				// City settings: everything on this world's routes.
				const auto& city = g_ed.file.city;
				section("City traffic", "every route here", kTeal);
				choice("Traffic here", { { kCityOn, "On", city.on }, { kCityOff, "Off", !city.on } }, true);
				stepper("Density", kDensityDown, std::to_string(city.density) + "%", kDensityUp, city.on);
				stepper("Vehicle limit (256 pilot slots)", kLimitDown, std::to_string(city.limit), kLimitUp, city.on);
				choice("Original routes", { { kOriginalsOn, "Use", city.originals }, { kOriginalsOff, "Leave out", !city.originals } }, city.on);
				line(!city.on ? "No new traffic flies here. Vehicles in the air finish their lap and land." :
					"Density scales every route's vehicles. The limit is the most at once. Originals are routes from other files.", kDim);
				button(kSheetBack, x0, y, kInner, 24, "Back", true, true, 0);
				y += 32;
			}
			else {
			// Routes, one line each (the selected one in its colour band); at most 8, scrolled to the selection.
			{
				char clock[32] = "";
				if (timeKnown) std::snprintf(clock, sizeof clock, "%s%s", g_ed.previewHour >= 0 ? "preview " : "", Clock(minute).c_str());
				section("ROUTES", clock);
				const int total = ListTotal(), sel = ListSelected(), mine = int(g_ed.file.routes.size());
				const int first = total > maxRows ? std::clamp(sel - maxRows / 2, 0, total - maxRows) : 0;
				for (int i = first; i < total && i < first + maxRows; ++i) {
					std::string name, right; unsigned color = 0;
					if (i < mine) {
						const auto& r = g_ed.file.routes[std::size_t(i)];
						name = r.label.empty() ? r.id : r.label;
						const auto t = ToTd(r, EditorKey());
						color = RouteColor(t, i);
						right = r.removed ? "removed" : g_ed.unplaced.count(r.id) ? "not here" : RouteVehicles(t) + " veh";
						if (Usable(r) && !Problems(t).empty()) name += "  !";
					}
					else {
						const auto& o = g_ed.others[std::size_t(i - mine)];
						name = o.route.label.empty() ? o.name : o.route.label;
						color = RouteColor(o.route, i);
						right = "original";
					}
					button(kRowBase + i, x0 - 4, y, kInner + 8, 22, name, i == sel, true, 3, right, color);
					y += 22;
				}
				if (sel == -2) { button(kRowBase - 1, x0 - 4, y, kInner + 8, 22, g_ed.draft->Value().label + "  (new)", true, true, 3, "not saved", RouteColor(g_ed.draft->Value(), mine)); y += 22; }
				if (total > maxRows) { text(std::to_string(total - maxRows) + " more: O / P go through them", x0, y + 2, kInner, kDim); y += 18; }
				button(kNew, x0 - 4, y, kInner + 8, 22, "+ New route", false, !draft, 3, "N");
				y += 26;
				button(kStartSwap, x0, y, kInner, 26, "Reset traffic", g_ed.loading, !draft, 0);
				y += 34;
			}
			// Places: one line each until clicked (the user: landing areas shrink to one line after Done).
			{
				const int lands = int(g_ed.file.landings.size()), zones = int(g_ed.file.zones.size()), total = lands + zones;
				if (total && maxRows > 0) {
					const int shownRows = std::min(maxRows, 6);
					const int sel = areaOn ? g_ed.spot : zoneOn ? lands + g_ed.zone : -1;
					const int first = total > shownRows ? std::clamp(sel - shownRows / 2, 0, total - shownRows) : 0;
					section("PLACES", std::to_string(total));
					for (int i = first; i < total && i < first + shownRows; ++i) {
						if (i < lands) button(kPlaceRow + i, x0 - 4, y, kInner + 8, 22, "Landing " + std::to_string(i + 1), i == sel, true, 3, AreaWords(g_ed.file.landings[std::size_t(i)]));
						else button(kPlaceRow + i, x0 - 4, y, kInner + 8, 22, "Zone " + std::to_string(i - lands + 1), i == sel, true, 3, ZoneWords(g_ed.file.zones[std::size_t(i - lands)]));
						y += 22;
					}
					y += 8;
				}
			}

			const bool groupSheet = g_ed.sheet == 4 || (g_ed.sheet == 1 && g_ed.ruleScope != 0);
			if (Many() && !groupSheet) {
				section(PicksWords(), "", 0xFFE15A);
				line("A change below goes to all of that kind. Moving, height, turn, size and delete go to all. Ctrl+click adds or removes one.", kDim);
				button(kJoin, x0, y, 150, 24, "Merge routes", false, true, 0);
				button(kSplit, x0 + kInner - 150, y, 150, 24, "Disconnect", false, true, 0);
				y += 32;
			}
			if (groupSheet && g_ed.sheet == 4) {
				section("Route groups", "", kTeal);
				line("A route in a group can't fly what its group leaves out. These hold on every planet.", kDim);
				auto outText = [](const std::vector<unsigned>& list) { return list.empty() ? std::string("all allowed") : std::to_string(list.size()) + " left out"; };
				const auto* all = GroupByName(trafficdata::GlobalGroup());
				button(kGroupRow, x0 - 4, y, kInner + 8, 24, "All routes", false, true, 3, all ? outText(all->exclude) : outText({}));
				y += 26;
				const auto choosable = ChoosableGroups();
				for (std::size_t k = 0; k < choosable.size(); ++k) {
					int here = 0;
					for (const auto& r : g_ed.file.routes) if (r.group == choosable[k]->name) ++here;
					button(kGroupRow + 1 + int(k), x0 - 4, y, kInner + 8, 24, choosable[k]->label, false, true, 3, outText(choosable[k]->exclude) + ", " + std::to_string(here) + (here == 1 ? " route here" : " routes here"));
					y += 26;
				}
				if (choosable.empty()) line("No groups yet. Make one, click it to choose what it leaves out, then set Group on a route under Rules.", kDim);
				y += 6;
				button(kGroupNew, x0, y, 150, 24, "New group", false, true, 0);
				button(kSheetBack, x0 + kInner - 150, y, 150, 24, "Back", true, true, 0);
				y += 32;
			}
			else if (groupSheet) {
				const auto* target = g_ed.ruleScope == 2 ? GroupByName(trafficdata::GlobalGroup()) : GroupByName(g_ed.ruleGroup);
				static const std::vector<unsigned> none;
				const auto& list = target ? target->exclude : none;
				section(g_ed.ruleScope == 2 ? std::string("All routes") : target ? target->label : std::string("Group"), "vehicle rules", kTeal);
				line(g_ed.ruleScope == 2 ? "Click a type to leave it out of every route, on every planet." : "Click a type to leave it out of every route in this group.", kDim);
				const auto& types = VehicleTypesHere();
				if (types.empty()) line("No vehicle types found yet: they are read once the game has loaded its vehicles.", kAmber);
				constexpr int kPerPage = 14;
				const int pages = std::max(1, (int(types.size()) + kPerPage - 1) / kPerPage);
				if (g_ed.typePage >= pages) g_ed.typePage = 0;
				for (int k = g_ed.typePage * kPerPage; k < int(types.size()) && k < (g_ed.typePage + 1) * kPerPage; ++k) {
					const bool out = TypeIn(list, types[std::size_t(k)]);
					button(kTypeBase + k, x0 - 4, y, kInner + 8, 22, std::string(out ? "[ ] " : "[x] ") + types[std::size_t(k)].name, !out, true, 3, out ? "left out" : "allowed");
					y += 22;
				}
				y += 6;
				const double bw = (kInner - 12) / 3;
				button(kAllowAll, x0, y, bw, 24, "Allow all", false, !list.empty(), 0);
				button(kTypesMore, x0 + bw + 6, y, bw, 24, pages > 1 ? "Page " + std::to_string(g_ed.typePage + 1) + "/" + std::to_string(pages) : "More", false, pages > 1, 0);
				button(kSheetBack, x0 + 2 * (bw + 6), y, bw, 24, "Back", true, true, 0);
				y += 30;
				if (g_ed.ruleScope == 1) {
					button(kGroupRename, x0, y, 150, 24, "Rename", false, true, 0);
					const bool armed = GetTickCount64() - g_ed.confirmDeleteAt < 3000;
					button(kGroupDelete, x0 + kInner - 150, y, 150, 24, armed ? "Click again" : "Delete group", armed, true, 7);
					y += 32;
				}
			}
			else if (areaOn) {
				// A landing area, open until Done (then one line in Places).
				const auto& a = g_ed.file.landings[std::size_t(g_ed.spot)];
				section("Landing " + std::to_string(g_ed.spot + 1), AreaWords(a), 0x7CFF8A);
				char deg[24]; std::snprintf(deg, sizeof deg, "%.0f deg", a.turn);
				stepper("Turn", kAreaTurnDown, deg, kAreaTurnUp, a.layout != "single");
				choice("Layout", { { kLayoutSingle, "Single", a.layout == "single" }, { kLayoutRow, "Row", a.layout == "row" }, { kLayoutBays, "Bays", a.layout == "bays" }, { kLayoutRing, "Ring", a.layout == "ring" } }, true);
				stepper("Spaces", kSpacesDown, std::to_string(a.layout == "single" ? 1 : a.spaces), kSpacesUp, a.layout != "single");
				char z[24]; std::snprintf(z, sizeof z, "%.0f m", a.at.z);
				stepper("Height", kZDown, z, kZUp, true);
				stepper("Left / right", kMoveLeft, "5 m", kMoveRight, true);
				stepper("Back / forward", kMoveBack, "5 m", kMoveForward, true);
				text("Occupancy rules", x0, y + 2, kInner, kTeal); y += 22;
				choice("Purpose", { { kPurposeTaxi, "Taxi", a.purpose == "taxi" }, { kPurposeDepot, "Depot", a.purpose == "depot" }, { kPurposeStop, "Stop", a.purpose == "stop" } }, true);
				choice("Parked for", { { kStayShort, "2 min", a.stay == 120 }, { kStayLong, "10 min", a.stay == 600 }, { kStayNeeded, "Until", a.stay != 120 && a.stay != 600 } }, true);
				choice("When full", { { kFullQueue, "Queue", a.full == "queue" }, { kFullCircle, "Circle", a.full == "circle" }, { kFullOn, "Go on", a.full == "on" } }, true);
				action("Who parks", a.routes.empty() ? std::string("All routes") : RouteLabel(a.routes.front()), kWhoParks, "Change", true);
				line(a.purpose == "taxi" ? "Taxi: only routes with the Taxi role land here." : a.purpose == "depot" ? "Depot: only routes with the Cargo role land here." : "Stop: routes land here, but not Cargo, Taxi or Patrol.", kDim);
				button(kDeletePoint, x0, y, 150, 24, "Delete", false, true, 7);
				button(kPlaceDone, x0 + kInner - 150, y, 150, 24, "Done", true, true, 0);
				y += 32;
			}
			else if (zoneOn) {
				const auto& z = g_ed.file.zones[std::size_t(g_ed.zone)];
				section("Zone " + std::to_string(g_ed.zone + 1), ZoneWords(z), z.slow ? kAmber : kCoral);
				choice("Kind", { { kZoneKeep, "Keep out", !z.slow }, { kZoneSlow, "Slow", z.slow } }, true);
				char v[24]; std::snprintf(v, sizeof v, "%.0f m", z.radius);
				stepper("Radius", kZoneSizeDown, v, kZoneSizeUp, true);
				if (z.slow) {
					std::snprintf(v, sizeof v, "%.0f m/s", z.speed); stepper("Speed inside", kZoneSpeedDown, v, kZoneSpeedUp, true);
					std::snprintf(v, sizeof v, "%.0f m", z.at.z); stepper("Height", kZDown, v, kZUp, true);
				}
				else { std::snprintf(v, sizeof v, "%.0f m", z.top); stepper("Stay above", kZoneTopDown, v, kZoneTopUp, true); }
				stepper("Left / right", kMoveLeft, "5 m", kMoveRight, true);
				stepper("Back / forward", kMoveBack, "5 m", kMoveForward, true);
				line(z.slow ? "Vehicles inside fly at this speed or slower." : "Vehicles on a route rise above this height inside the circle. Problems lists routes under it, with a fix.", kDim);
				button(kDeletePoint, x0, y, 150, 24, "Delete", false, true, 7);
				button(kPlaceDone, x0 + kInner - 150, y, 150, 24, "Done", true, true, 0);
				y += 32;
			}
			// The selected route: its name, any problems, then the three folded groups.
			else if (shown) {
				const auto lineOf = trafficroute::Build(*shown);
				char info[64];
				std::snprintf(info, sizeof info, ring ? "ring, %.1f km" : "%zu pts, %.1f km", shown->points.size(), trafficroute::Length(lineOf) / 1000.0);
				section(shown->label.empty() ? shown->name : shown->label, info, 0xFFFFFF);
				{
					// Delete (2026-10-30, the user: it was buried under More): a second click on the same button within 3 s confirms it.
					const bool armed = GetTickCount64() - g_ed.confirmDeleteAt < 3000;
					const bool isNew = draft && g_ed.draftIndex < 0, original = !draft && g_ed.route < 0 && g_ed.other >= 0;
					const bool edited = g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size()) && IsOriginal(g_ed.file.routes[std::size_t(g_ed.route)]);
					const char* label = isNew ? "Discard new route" : armed ? "Click again to confirm" : original ? "Switch off original" : edited ? "Undo my edits" : "Delete route";
					button(kDeleteRoute, x0, y, 210, 24, label, armed && !isNew, true, 7);
					y += 30;
				}
				if (draft) line("Editing, not saved yet", kAmber);
				if (!draft && g_ed.route < 0 && g_ed.other >= 0) {
					line("Original route from " + g_ed.others[std::size_t(g_ed.other)].file + ": changing it makes your own copy", kDim);
				}
				if (draft) if (const auto issue = g_ed.draft->Check(); issue != routeeditor::Issue::None) line(std::string("Can't save yet: the route ") + IssueText(issue), kCoral);
				const auto problems = Problems(*shown, true);
				if (!problems.empty()) { button(kProblems, x0, y, kInner, 22, "! " + std::to_string(problems.size()) + (problems.size() > 1 ? " problems" : " problem") + ((open & 8) ? ": hide" : ": show"), (open & 8) != 0, true, 6); y += 28; }
				else { text("No problems found. Buildings need Test route.", x0, y + 2, kInner, kDim); y += 22; }
				if (open & 8) {
					for (std::size_t k = 0; k < problems.size() && k < 6; ++k) {
						line(problems[k].text, kAmber);
						button(kProblemShow + int(k), x0, y, 70, 22, "Show", false, problems[k].a >= 0, 0);
						if (problems[k].kind) button(kProblemFix + int(k), x0 + 76, y, 190, 22, problems[k].fix, true, true, 0);
						y += 28;
					}
					if (problems.size() > 6) line(std::to_string(problems.size() - 6) + " more: fix these first", kDim);
					y += 4;
				}

				const auto d = routeschedule::Evaluate(shown->schedule, timeKnown, minute);
				const auto [lo, hi] = routeeditor::HeightRange(*shown);
				char hr[48];
				if (std::lround(lo) == std::lround(hi)) std::snprintf(hr, sizeof hr, "%.0f m", lo); else std::snprintf(hr, sizeof hr, "%.0f-%.0f m", lo, hi);
				char speed[32]; std::snprintf(speed, sizeof speed, "%.0f m/s", shown->speed);
				const std::string rules = (!shown->schedule.zones.empty() ? std::string("Busy words") : shown->schedule.allDay ? std::string("All day") : shown->schedule.windows.empty() ? std::string("By hours") :
					Clock(shown->schedule.windows.front().start) + "-" + Clock(shown->schedule.windows.front().end)) + (trafficdata::AllLeftOut(g_ed.routeGroups, *shown).empty() ? "" : ", some types out");
				auto group = [&](int id, int index, const char* title, const std::string& summary) {
					const bool isOpen = (open >> index & 1) != 0;
					button(id, x0, y, kInner, 24, title, isOpen, true, 2, isOpen ? std::string() : summary);
					y += 26;
					return isOpen;
				};
				if (g_ed.sheet == 1) {
					// Vehicle rules: every type is allowed; a click leaves one out of this route (and lets it back on).
					section("Vehicle rules", VehicleRulesText(*shown), kTeal);
					line("Click a type to leave it out of this route; click again to let it back on.", kDim);
					action("Group", GroupLabel(shown->group), kGroupNext, "Change", true);
					action("Route groups", "", kGroupSheet, "Open", true);
					const auto& types = VehicleTypesHere();
					if (types.empty()) line("No vehicle types found yet: they are read once the game has loaded its vehicles.", kAmber);
					constexpr int kPerPage = 14;
					const int pages = std::max(1, (int(types.size()) + kPerPage - 1) / kPerPage);
					if (g_ed.typePage >= pages) g_ed.typePage = 0;
					for (int k = g_ed.typePage * kPerPage; k < int(types.size()) && k < (g_ed.typePage + 1) * kPerPage; ++k) {
						const bool out = TypeLeftOut(*shown, types[std::size_t(k)]);
						const std::string above = TypeAbove(*shown, types[std::size_t(k)]);
						button(kTypeBase + k, x0 - 4, y, kInner + 8, 22, std::string(out || !above.empty() ? "[ ] " : "[x] ") + types[std::size_t(k)].name, !out && above.empty(), true, 3, !above.empty() ? above : out ? "left out" : "allowed");
						y += 22;
					}
					y += 6;
					const double bw = (kInner - 12) / 3;
					button(kAllowAll, x0, y, bw, 24, "Allow all", false, !shown->exclude.empty(), 0);
					button(kTypesMore, x0 + bw + 6, y, bw, 24, pages > 1 ? "Page " + std::to_string(g_ed.typePage + 1) + "/" + std::to_string(pages) : "More", false, pages > 1, 0);
					button(kSheetBack, x0 + 2 * (bw + 6), y, bw, 24, "Back", true, true, 0);
					y += 32;
				}
				else if (g_ed.sheet == 2) {
					// Time rules: all day, one stretch of hours, or the day in zones of busy words (the user's design).
					const auto& s = shown->schedule;
					const bool words = !s.zones.empty();
					section("Time rules", ScheduleText(s), kTeal);
					choice("Time", { { kAllDay, "All day", s.allDay && !words }, { kByHours, "Hours", !s.allDay && !words }, { kByWords, "Words", words } }, true);
					if (words) {
						// The day as a strip, midnight to midnight, with the preview time on it.
						const int n = int(s.zones.size());
						for (int i = 0; i < n; ++i) {
							const int from = s.zones[std::size_t(i)].start, to = i + 1 < n ? s.zones[std::size_t(i + 1)].start : routeschedule::kMinutesPerDay;
							g_ui.boxes.push_back({ x0 + kInner * from / routeschedule::kMinutesPerDay, y, kInner * (to - from) / routeschedule::kMinutesPerDay, 14, WordColor(s.zones[std::size_t(i)].word), 1 });
							if (i == 0 && from > 0) g_ui.boxes.push_back({ x0, y, kInner * from / routeschedule::kMinutesPerDay, 14, WordColor(s.zones[std::size_t(n - 1)].word), 1 });
						}
						if (timeKnown) g_ui.boxes.push_back({ x0 + kInner * minute / routeschedule::kMinutesPerDay - 1, y - 3, 2, 20, 0xFFFFFF, 1 });
						y += 22;
						for (int i = 0; i < n; ++i) {
							const auto& z = s.zones[std::size_t(i)];
							button(kDayRow + i * 4, x0, y, 24, 22, "-", false, true, 0);
							text(Clock(z.start), x0 + 31, y + 3, 50, kText);
							button(kDayRow + i * 4 + 1, x0 + 80, y, 24, 22, "+", false, true, 0);
							button(kDayRow + i * 4 + 2, x0 + 112, y, 130, 22, routeschedule::WordName(z.word), false, true, 0);
							button(kDayRow + i * 4 + 3, x0 + 250, y, 90, 22, "Remove", false, n > 1, 0);
							y += 26;
						}
						button(kWordsAdd, x0, y, 150, 22, "+ Add a change", false, n < 6, 0);
						y += 28;
						stepper("Most vehicles", kVehDown, std::to_string(s.peak), kVehUp, true);
						for (int w = 1; w < routeschedule::kWords; ++w) stepper(routeschedule::WordName(w), kLevelBase + w * 2, std::to_string(s.levels[w]) + "%", kLevelBase + w * 2 + 1, true);
						choice("Blend", { { kBlendQuick, "Quick", s.blend == 10 }, { kBlendGentle, "Gentle", s.blend == 30 }, { kBlendSlow, "Slow", s.blend == 60 } }, true);
					}
					else if (!s.allDay && !s.windows.empty()) {
						const auto& w = s.windows.front();
						stepper("Starts", kStartDown, Clock(w.start), kStartUp, true);
						stepper("Ends", kEndDown, Clock(w.end), kEndUp, true);
					}
					const std::string at2 = timeKnown ? Clock(minute) : std::string("--:--");
					stepper(g_ed.previewHour >= 0 ? "Preview time" : "Live time", kTimeDown, at2, kTimeUp, true);
					if (g_ed.previewHour >= 0) action("Clock", "previewing", kTimeLive, "Live", true);
					line(!d.known ? "Local time unknown: routes with hours wait" : d.active ? "Flying then: " + std::to_string(d.vehicles) + " vehicle(s)" : "Not flying then", d.active ? 0x7CFF8A : kAmber);
					if (words) line("When a zone asks for fewer, vehicles finish their lap, land and stay parked.", kDim);
					button(kSheetBack, x0, y, kInner, 24, "Back", true, true, 0);
					y += 32;
				}
				else {
				// A shape's own values (2026-10-30, the user: "their values for how they're built"), read back from its points.
				if (const auto sv = routeeditor::ReadShape(*shown); sv.known) {
					char va[32];
					text(std::string(routeeditor::ShapeName(sv.kind)) + " values", x0, y + 2, kInner, kTeal); y += 22;
					std::snprintf(va, sizeof va, "%.0f m", sv.kind == routeeditor::Shape::Rectangle ? sv.a * 2 : sv.a);
					stepper(sv.kind == routeeditor::Shape::Rectangle ? "Length" : sv.kind == routeeditor::Shape::Ellipse ? "Long radius" : "Radius", kShapeADown, va, kShapeAUp, true);
					if (sv.kind == routeeditor::Shape::Ellipse || sv.kind == routeeditor::Shape::Rectangle) {
						std::snprintf(va, sizeof va, "%.0f m", sv.kind == routeeditor::Shape::Rectangle ? sv.b * 2 : sv.b);
						stepper(sv.kind == routeeditor::Shape::Rectangle ? "Width" : "Short radius", kShapeBDown, va, kShapeBUp, true);
					}
					if (sv.kind == routeeditor::Shape::Helix) {
						std::snprintf(va, sizeof va, "%.1f", sv.turns);
						stepper("Turns", kShapeBDown, va, kShapeBUp, true);
						std::snprintf(va, sizeof va, "%.0f m", std::fabs(sv.climb));
						stepper("Climb a turn", kShapeCDown, va, kShapeCUp, true);
					}
					y += 4;
				}
				if (group(kGroupFlow, 0, "Flow", std::string(speed) + ", " + RouteVehicles(*shown) + " veh, " + (shown->lanes > 1 ? "two way" : "one way"))) {
					stepper("Speed", kSpeedDown, speed, kSpeedUp, true);
					stepper("Vehicles", kVehDown, RouteVehicles(*shown), kVehUp, true);
					choice("Direction", { { kOneWay, "One way", shown->lanes <= 1 }, { kTwoWay, "Two way", shown->lanes > 1 } }, true);
					choice("Path", { { kLoopOn, "Loop", shown->closed }, { kLoopOff, "Open", !shown->closed } }, !ring);
					if (ring) { char rr[32]; std::snprintf(rr, sizeof rr, "%.0f m", shown->shape.radius); stepper("Ring radius", kRadiusDown, rr, kRadiusUp, true); }
					action("Role", shown->role == "taxi" ? "Taxi" : shown->role == "cargo" ? "Cargo" : shown->role == "patrol" ? "Patrol" : "Civilian", kRoleNext, "Change", true);
					y += 4;
				}
				if (group(kGroupPosition, 1, "Position", hr)) {
					stepper("Route height", kRouteDown, hr, kRouteUp, true);
					stepper("Route turn", kTurnLeft, "15 deg", kTurnRight, !ring);
					{ char across[32]; std::snprintf(across, sizeof across, "%.0f m wide", routeeditor::Across(*shown)); stepper("Size", kSizeDown, across, kSizeUp, true); }
					action("Colour", "", kColor, "Next  U", true, RouteColor(*shown, ShownIndex()));
					action("Name", shown->label.empty() ? shown->name : shown->label, kRename, "Rename F2", true);
					button(kJoin, x0, y, 150, 24, "Merge routes", false, !ring, 0);
					button(kSplit, x0 + kInner - 150, y, 150, 24, "Disconnect", false, !ring, 0);
					y += 32;
				}
				if (group(kGroupRules, 2, "Rules", rules)) {
					action("Time rules", !shown->schedule.zones.empty() ? std::string("Busy words") : shown->schedule.allDay ? std::string("All day") : std::string("By hours"), kTimeRules, "Set", true);
					const std::string at = timeKnown ? Clock(minute) : std::string("--:--");
					stepper(g_ed.previewHour >= 0 ? "Preview time" : "Live time", kTimeDown, at, kTimeUp, true);
					if (g_ed.previewHour >= 0) action("Clock", "previewing", kTimeLive, "Live", true);
					const auto issue = routeschedule::Validate(shown->schedule);
					line(issue != routeschedule::Issue::None ? std::string("Time rules not valid: ") + routeschedule::IssueText(issue)
						: !d.known ? "Local time unknown: routes with hours wait" : d.active ? "Flying now: " + std::to_string(d.vehicles) + " vehicle(s)" : "Not flying at this time",
						d.active ? 0x7CFF8A : kAmber);
					action("Group", GroupLabel(shown->group), kGroupNext, "Change", true);
					action("Vehicle rules", VehicleRulesText(*shown), kVehicleRules, "Set", true);
					action("Route groups", "", kGroupSheet, "Open", true);
				}
				}  // the sheet or the groups
				y += 6;
			}
			else if (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size()) && g_ed.file.routes[std::size_t(g_ed.route)].removed) {
				section("Removed original", "");
				line("'" + g_ed.file.routes[std::size_t(g_ed.route)].id + "' is switched off.", kDim);
				{ const bool armed = GetTickCount64() - g_ed.confirmDeleteAt < 3000; button(kDeleteRoute, x0, y, 210, 24, armed ? "Click again to confirm" : "Bring back", armed, true, 0); y += 32; }
			}
			else { section("No route selected", ""); line(g_ed.file.routes.empty() ? "Press + New route, or click a route on the map." : "Click a route on the map or in the list.", kDim); y += 4; }

			// What else is selected: a point, a junction or a spot.
			if (areaOn || zoneOn || groupSheet) {}
			else if (shown && !ring && g_ed.point >= 0 && g_ed.point < int(shown->points.size())) {
				const auto& p = shown->points[std::size_t(g_ed.point)];
				char at[48]; std::snprintf(at, sizeof at, "%.0f, %.0f", p.x, p.y);
				section("Point " + std::to_string(g_ed.point + 1), at, 0xFFE15A);
				if (const auto& under = SurfaceUnder(p.x, p.y, p.z); under.known) {
					char up[96];
					if (under.buried) {
						std::snprintf(up, sizeof up, "%.0f m inside the ground: raise it", under.below - p.z);
						line(up, kCoral);
					}
					else {
						std::snprintf(up, sizeof up, under.rough ? "About %.0f m above the terrain (buildings not loaded)" : "%.0f m above the surface under it", p.z - under.below);
						line(up, kDim);
						if (under.covered) line("Under a roof or a bridge: vehicles may hit it", kAmber);
					}
				}
				else line("Ground unknown here", kAmber);
				char z[24]; std::snprintf(z, sizeof z, "%.0f m", p.z);
				stepper("Height", kZDown, z, kZUp, true);
				stepper("Left / right", kMoveLeft, "5 m", kMoveRight, true);
				stepper("Back / forward", kMoveBack, "5 m", kMoveForward, true);
				const double radius = std::size_t(g_ed.point) < shown->rounding.size() ? shown->rounding[std::size_t(g_ed.point)] : 0;
				const bool end = !shown->closed && (g_ed.point == 0 || g_ed.point + 1 == int(shown->points.size()));
				choice("Corner", { { kSharp, "Sharp", radius <= 0 }, { kRounded, "Round", radius > 0 } }, !end);
				if (radius > 0) { char rr[24]; std::snprintf(rr, sizeof rr, "%.0f m", radius); stepper("Corner radius", kRoundDown, rr, kRoundUp, true); }
				button(kDeletePoint, x0, y, 150, 24, "Delete point", false, true, 0);
				button(kSplit, x0 + kInner - 150, y, 150, 24, "Disconnect here", false, true, 0); y += 32;
			}
			else if (g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size())) {
				const auto& k = g_ed.file.links[std::size_t(g_ed.link)];
				section("Junction", RouteTitle(k.from) + " > " + RouteTitle(k.to), 0xFF8AD8);
				choice("Type", { { kJunctionBoth, "Both", k.type.empty() }, { kJunctionExit, "A > B", k.type == "exit" }, { kJunctionMerge, "B > A", k.type == "merge" }, { kJunctionCross, "Cross", k.type == "crossing" } }, true);
				line(TypeWords(k, k.type), kDim);
				if (const auto best = SuggestedType(k); best != k.type) {
					const std::string name = best.empty() ? "Both" : best == "exit" ? "A > B" : best == "merge" ? "B > A" : "Cross";
					action("Suggested", name + " for this shape", kJunctionSuggest, "Use", true);
				}
				char share[16]; std::snprintf(share, sizeof share, "%d%%", int(std::lround(k.share * 100)));
				stepper("Share turning", kShareDown, share, kShareUp, true);
				choice("Corner", { { kSharp, "Sharp", k.radius <= 0 }, { kRounded, "Round", k.radius > 0 } }, true);
				if (k.radius > 0) { char rr[24]; std::snprintf(rr, sizeof rr, "%.0f m", k.radius); stepper("Corner radius", kRoundDown, rr, kRoundUp, true); }
				button(kDeletePoint, x0, y, 150, 24, "Remove junction", false, true, 0); y += 32;
			}
			else if (g_ed.spot >= 0) {
				const auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				if (g_ed.spot < int(list.size())) {
					const auto& sp = list[std::size_t(g_ed.spot)];
					section(g_ed.spotKind == 1 ? "Landing spot" : "Spawn spot", sp.id, 0x7CFF8A);
					char z[24]; std::snprintf(z, sizeof z, "%.0f m", sp.at.z);
					stepper("Height", kZDown, z, kZUp, true);
				stepper("Left / right", kMoveLeft, "5 m", kMoveRight, true);
				stepper("Back / forward", kMoveBack, "5 m", kMoveForward, true);
					button(kDeletePoint, x0, y, 150, 24, "Remove spot", false, true, 0); y += 32;
				}
			}

			}  // the city sheet or the routes
			// Footer: Test, Cancel, Save.
			g_ui.boxes.push_back({ px, y - 4, kPanelW, 1, 0x3A4658, 1 });
			{
				const double w = (kInner - 12) / 3;
				button(kTest, x0, y + 6, w, 30, "Test", false, route, 0);
				button(kCancel, x0 + w + 6, y + 6, w, 30, "Cancel", false, true, 0);
				button(kSave, x0 + 2 * (w + 6), y + 6, w, 30, "Save", true, true, 0);
				y += 46;
			}
			g_ui.panelBottom = std::min(y, g_ui.height - 140);
			return y;
		}
		// Everything has to fit above the map's own buttons: first fewer lines in the routes list, then only the group opened last stays open.
		void Layout(const trafficdata::Route* shown, bool timeKnown, int minute)
		{
			const double room = g_ui.height - 140;
			if (LayoutPass(shown, timeKnown, minute, 8, g_ed.groups) <= room) return;
			if (LayoutPass(shown, timeKnown, minute, 4, g_ed.groups) <= room) return;
			// A sheet or an open place needs the room more than the lists do.
			if ((g_ed.sheet || AreaOn() || ZoneOn()) && LayoutPass(shown, timeKnown, minute, 0, g_ed.groups) <= room) return;
			g_ed.groups &= 1 << g_ed.lastGroup;
			LayoutPass(shown, timeKnown, minute, 4, g_ed.groups);
		}

		// Schedule edits on the open draft: one window (start, end, vehicles) or all day.
		routeschedule::Window& FirstWindow(trafficdata::Route& r)
		{
			if (r.schedule.windows.empty()) r.schedule.windows.push_back({ 6 * 60, 10 * 60, std::max(1, r.schedule.allDayVehicles) });
			return r.schedule.windows.front();
		}
		// A route's name (2026-10-30, the user: name and rename routes). Only its label changes: the id stays, so junctions and traffic keep to it.
		void CommitName()
		{
			if (!g_ed.naming) return;
			g_ed.naming = false;
			const std::string typed = trafficdata::Trim(g_ed.nameBuf);
			if (const int which = g_ed.namingGroup) {
				g_ed.namingGroup = 0;
				if (typed.empty()) return;
				const std::string label = routefiles::CleanLabel(typed);
				if (which == 2) { if (auto* g = GroupByName(g_ed.ruleGroup)) { g->label = label; Status("Group named '" + label + "' (Save to keep it)"); } return; }
				std::string name = routefiles::Slug(label);
				for (int n = 2; GroupByName(name) || name == trafficdata::GlobalGroup(); ++n) name = routefiles::Slug(label) + "_" + std::to_string(n);
				g_ed.routeGroups.push_back({ name, label, {} });
				g_ed.ruleGroup = name; g_ed.ruleScope = 1; g_ed.sheet = 1; g_ed.typePage = 0; g_types.clear();
				Status("Group '" + label + "': click what it leaves out, then set Group on a route under Rules. Save keeps it.");
				return;
			}
			if (typed.empty() || !EnsureDraft()) return;
			auto r = g_ed.draft->Value();
			r.label = routefiles::CleanLabel(typed);
			g_ed.draft->Edit(r);
			Status("Route named '" + r.label + "' (Save to keep)");
		}
		// ---- The whole route at once (stage 2: the Route tool, Route height / turn, Copy, Mirror) ----
		constexpr double kDeg = 3.14159265358979323846 / 180.0;
		// Every point (a ring: its centre) must pass the same check as a single point; the first refusal is reported.
		bool RouteFits(const trafficdata::Route& r, const char* what)
		{
			if (r.ring) {
				if (const char* why = PointProblem(r.shape.cx, r.shape.cy, r.shape.z)) { Refused(what, r.shape.cx, r.shape.cy, r.shape.z, why); return false; }
				return true;
			}
			for (const auto& p : r.points)
				if (const char* why = PointProblem(p.x, p.y, p.z)) { Refused(what, p.x, p.y, p.z, why); return false; }
			return true;
		}
		void RaiseRoute(double dz)
		{
			if (Many()) { routeeditor::GroupMove m; m.dz = dz; GroupStep(m, "Height"); return; }
			if (!EnsureDraft()) return;
			const auto next = routeeditor::Translated(g_ed.draft->Value(), 0, 0, dz);
			if (!RouteFits(next, "Route height")) return;
			g_ed.draft->Edit(next);
			const auto [lo, hi] = routeeditor::HeightRange(next);
			char b[128];
			std::snprintf(b, sizeof b, "Route %s %.0f m: now %.0f-%.0f m. Its points keep their heights to each other.", dz > 0 ? "raised" : "lowered", std::fabs(dz), lo, hi);
			Status(b);
		}
		// Clockwise as seen from above (Route turn +); the draft turns about its centre.
		void TurnRoute(double clockwise)
		{
			if (Many()) { routeeditor::GroupMove m = AboutMiddle(); m.radians = -clockwise * kDeg; g_ed.groupTurn = clockwise; GroupStep(m, "Turn"); g_ed.groupTurn = 0; return; }
			if (!EnsureDraft()) return;
			if (g_ed.draft->Value().ring) { Status("A ring looks the same turned: move it, or change its radius under Flow"); return; }
			const auto next = routeeditor::Turned(g_ed.draft->Value(), -clockwise * kDeg);
			if (!RouteFits(next, "Route turn")) return;
			g_ed.draft->Edit(next);
			char b[96]; std::snprintf(b, sizeof b, "Route turned %.0f deg %s about its centre", std::fabs(clockwise), clockwise > 0 ? "clockwise" : "anticlockwise");
			Status(b);
		}
		// Steps close together (a held key, the wheel) are one undo step.
		bool ApplySize(const trafficdata::Route& next, const char* what)
		{
			if (!RouteFits(next, what)) return false;
			static ULONGLONG lastAt = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - lastAt < 400) g_ed.draft->Amend(next); else g_ed.draft->Edit(next);
			lastAt = now;
			return true;
		}
		// Any route bigger or smaller about its centre, seen from above (2026-10-30, the user); heights and points kept.
		void ScaleRoute(double factor)
		{
			if (Many()) { routeeditor::GroupMove m = AboutMiddle(); m.factor = factor; GroupStep(m, "Size", true); return; }
			if (!EnsureDraft()) return;
			const auto next = routeeditor::Scaled(g_ed.draft->Value(), factor);
			const double across = routeeditor::Across(next);
			if (across < 40.0 || across > 6000.0) { Status(across < 40.0 ? "That is as small as a route goes (40 m wide)" : "That is as big as a route goes (6 km wide)"); return; }
			if (!ApplySize(next, "Route size")) return;
			char b[96]; std::snprintf(b, sizeof b, "Route now %.0f m wide; heights unchanged. Save to keep it.", across);
			Status(b);
		}
		// One step of a shape's own value: a, b or c as the panel lists them.
		void ShapeStep(char value, double sign, bool big)
		{
			if (!EnsureDraft()) return;
			const auto r = g_ed.draft->Value();
			const auto v = routeeditor::ReadShape(r);
			if (!v.known) { Status("This route no longer has its shape: a point was moved by hand. Size still works."); return; }
			auto next = r;
			char b[128];
			if (v.kind == routeeditor::Shape::Helix) {
				double radius = v.a, turns = v.turns, climb = std::fabs(v.climb);
				if (value == 'a') radius = std::clamp(radius + sign * (big ? 50.0 : 10.0), 20.0, 1500.0);
				else if (value == 'b') turns = std::clamp(turns + sign * 0.5, 0.5, 10.0);
				else climb = std::clamp(climb + sign * (big ? 25.0 : 5.0), 5.0, 200.0);
				next = routeeditor::WithHelix(r, v, radius, turns, v.climb < 0 ? -climb : climb);
				if (!ApplySize(next, "Helix")) return;
				const std::size_t was = r.points.size(), now = next.points.size();
				for (std::size_t i = was; i > now; --i) LinksPointRemoved(r.name, i - 1, i);  // a junction on a point that went goes with it
				if (now > was) LinksPointInserted(r.name, was, was);
				if (g_ed.point >= int(now)) g_ed.point = int(now) - 1;
				std::snprintf(b, sizeof b, "Helix: radius %.0f m, %.1f turns, %.0f m a turn. Save to keep it.", radius, turns, climb);
				Status(b);
				return;
			}
			const double step = sign * (big ? 25.0 : 5.0);
			double a = v.a, b2 = v.b;
			if (v.kind == routeeditor::Shape::Circle) { a = b2 = std::clamp(a + sign * (big ? 50.0 : 10.0), 20.0, 1500.0); }
			else if (v.kind == routeeditor::Shape::Ellipse) {  // the long radius stays the longer, and it never quite becomes a circle
				if (value == 'a') a = std::clamp(a + step * 2, b2 / 0.95, 1500.0);
				else b2 = std::clamp(b2 + step * 2, 20.0, a * 0.95);
			}
			else if (value == 'a') a = std::clamp(a + step, 10.0, 1500.0);
			else b2 = std::clamp(b2 + step, 10.0, 1500.0);
			next = routeeditor::WithSides(r, v, a, b2);
			if (!ApplySize(next, routeeditor::ShapeName(v.kind))) return;
			if (v.kind == routeeditor::Shape::Rectangle) std::snprintf(b, sizeof b, "Rectangle: %.0f m long, %.0f m wide. Save to keep it.", a * 2, b2 * 2);
			else if (v.kind == routeeditor::Shape::Ellipse) std::snprintf(b, sizeof b, "Ellipse: radius %.0f m by %.0f m. Save to keep it.", a, b2);
			else std::snprintf(b, sizeof b, "Circle: radius %.0f m. Save to keep it.", a);
			Status(b);
		}
		// The camera's forward direction on the ground (straight down: north).
		void Forward(const routemap::Camera& c, double& fx, double& fy)
		{
			fx = c.focus.x - c.eye.x; fy = c.focus.y - c.eye.y;
			const double l = std::hypot(fx, fy);
			if (l < 1e-3) { fx = 0; fy = 1; } else { fx /= l; fy /= l; }
		}
		void MirrorRoute(const routemap::Camera& c)
		{
			if (!EnsureDraft()) return;
			if (g_ed.draft->Value().ring) { Status("A ring is the same mirrored"); return; }
			double fx, fy; Forward(c, fx, fy);
			const auto next = routeeditor::Mirrored(g_ed.draft->Value(), fx, fy);  // across the screen's up-down line: left and right swap
			if (!RouteFits(next, "Mirror")) return;
			g_ed.draft->Edit(next);
			Status("Mirrored left to right as the map shows it. Junctions stay on the same points.");
		}
		// A copy of the selected route (an original too) as a new route of the editor, 40 m to the right on screen.
		void CopyRoute(const routemap::Camera& c)
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			trafficdata::Route scratch;
			const auto* shown = Shown(scratch);
			if (!shown) { Status("Select a route to copy first"); return; }
			trafficdata::Route t = *shown;
			const std::string base = t.label.empty() ? t.name : t.label;
			const unsigned was = RouteColor(*shown, ShownIndex());
			int at = 0;
			for (int i = 0; i < kRouteColorCount; ++i) if (kRouteColors[i] == was) at = i;
			t.label = routefiles::CleanLabel(base + " copy");
			t.name = routefiles::NewId(t.label, TakenIds());
			t.world = EditorKey(); t.removed = false; t.test = false;
			t.color = kRouteColors[(at + 1) % kRouteColorCount];
			double fx, fy; Forward(c, fx, fy);
			t = routeeditor::Translated(t, 40 * fy, -40 * fx, 0);
			g_ed.draft.emplace(t);
			g_ed.draftShift = {}; g_ed.draftIndex = -1;
			g_ed.route = -1; g_ed.other = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.link = -1;
			g_ed.mode = Mode::Route;
			Status("Copied as '" + t.label + "', 40 m to the side. Drag it into place with the Route tool, then Save.");
		}
		// ---- Split, Join, Restore (2026-10-30, Traffic Manager V1; More menu) ----
		// ---- Merge and Disconnect (2026-10-31, the user; they replace Split and Join, which stopped at any junction and only reached 300 m). Nothing is written until Save, and Undo
		// brings it all back. Junctions follow their points to where those points went; one whose point is gone, or that would join a route to itself, goes. ----
		// Where this link's end on the route went: which route, which point, and whether that is the new route's last point.
		struct MovedTo { std::string route; int index = -1; };
		int MoveLinks(const std::function<MovedTo(const std::string&, int)>& where, const std::function<int(const std::string&)>& lastOf)
		{
			int dropped = 0;
			auto& links = g_ed.file.links;
			for (auto k = links.begin(); k != links.end();) {
				bool drop = false;
				const int fromAt = std::max(0, int(std::floor(k->at)));
				if (const auto f = where(k->from, fromAt); !f.route.empty()) {
					if (f.index < 0) drop = true;
					else { k->at = f.index + (k->at - std::floor(k->at)); k->from = f.route; }
				}
				if (!drop) {
					const int old = k->toEnd ? -1 : int(std::lround(k->toAt));
					if (const auto t = where(k->to, old); !t.route.empty()) {
						if (t.index < 0) drop = true;
						else { k->to = t.route; k->toEnd = t.index == lastOf(t.route); k->toAt = k->toEnd ? 0 : double(t.index); }
					}
				}
				if (!drop && k->from == k->to) drop = true;
				if (drop) { k = links.erase(k); ++dropped; }
				else ++k;
			}
			if (g_ed.link >= int(links.size())) g_ed.link = -1;
			return dropped;
		}
		// Disconnect, by what is selected: at a point, the route breaks there (a loop opens there); two points next to each other, the stretch between them goes; a junction, it is
		// removed; routes selected together, the junctions between them are removed.
		void DisconnectRoute()
		{
			const auto picks = PicksNow();
			std::vector<int> cuts;
			std::vector<std::string> routes, joints;
			for (const auto& p : picks) {
				if (p.kind == kPickPoint) cuts.push_back(p.index);
				else if (p.kind == kPickRoute) routes.push_back(p.id);
				else if (p.kind == kPickLink) joints.push_back(p.id);
			}
			if (picks.empty() && g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size())) joints.push_back(g_ed.file.links[std::size_t(g_ed.link)].id);
			if (picks.empty() && g_ed.point >= 0) cuts.push_back(g_ed.point);
			if (cuts.empty() && !joints.empty()) {
				PushSnap();
				for (const auto& id : joints) if (const int k = LinkIndex(id); k >= 0) g_ed.file.links.erase(g_ed.file.links.begin() + k);
				g_ed.picks.clear(); g_ed.link = -1;
				Status(std::to_string(joints.size()) + (joints.size() == 1 ? " junction" : " junctions") + " removed: those routes no longer feed each other. Save keeps it.");
				return;
			}
			if (cuts.empty() && routes.size() >= 2) {
				auto between = [&](const routefiles::Link& k) { return std::find(routes.begin(), routes.end(), k.from) != routes.end() && std::find(routes.begin(), routes.end(), k.to) != routes.end(); };
				const auto count = std::count_if(g_ed.file.links.begin(), g_ed.file.links.end(), between);
				if (!count) { Status("Those routes have no junction between them. To cut a route, select a point on it."); return; }
				PushSnap();
				g_ed.file.links.erase(std::remove_if(g_ed.file.links.begin(), g_ed.file.links.end(), between), g_ed.file.links.end());
				g_ed.picks.clear(); g_ed.link = -1;
				Status(std::to_string(count) + (count == 1 ? " junction" : " junctions") + " between them removed. Save keeps it.");
				return;
			}
			if (cuts.empty()) { Status("Select where to disconnect: a point on a route, a junction, or two routes joined by one"); return; }
			if (!EnsureDraft()) return;
			const auto r = g_ed.draft->Value();
			if (r.ring) { Status("A ring has no points to cut"); return; }
			std::sort(cuts.begin(), cuts.end());
			auto cut = routeeditor::CutRoute(r, cuts);
			if (cut.pieces.empty()) { Status("That would leave nothing with two points"); return; }
			if (cut.pieces.size() == 1 && cut.pieces[0].points.size() == r.points.size() && !r.closed) { Status("A route's own end has nothing to disconnect: pick a point between its ends"); return; }
			PushSnap();
			const std::string base = r.label.empty() ? r.name : r.label;
			int colorAt = 0;
			for (int k = 0; k < kRouteColorCount; ++k) if (kRouteColors[k] == RouteColor(r, ShownIndex())) colorAt = k;
			std::vector<std::string> names{ r.name };
			for (std::size_t i = 1; i < cut.pieces.size(); ++i) {
				auto& b = cut.pieces[i];
				b.label = routefiles::CleanLabel(base + " " + std::to_string(i + 1));
				b.name = routefiles::NewId(b.label, TakenIds());
				colorAt = (colorAt + 1) % kRouteColorCount;
				b.color = kRouteColors[colorAt];
				g_ed.file.routes.push_back(FromTd(b));
				g_ed.shifts[b.name] = g_ed.draftShift;
				names.push_back(b.name);
			}
			g_ed.draft->Edit(cut.pieces[0]);
			g_ed.massUndo.back().draftChanged = true;
			g_ed.massUndo.back().depth = DraftDepth();
			const int last = int(r.points.size()) - 1;
			const int dropped = MoveLinks([&](const std::string& id, int at) -> MovedTo {
				if (id != r.name) return {};
				const auto& to = cut.map[std::size_t(at < 0 ? last : std::min(at, last))];
				if (to.first < 0) return { r.name, -1 };
				return { names[std::size_t(to.first)], to.second };
			}, [&](const std::string& id) {
				for (std::size_t i = 0; i < names.size(); ++i) if (names[i] == id) return int(cut.pieces[i].points.size()) - 1;
				return -1;
			});
			g_ed.picks.clear(); g_ed.point = -1;
			std::string text = cut.pieces.size() == 1 ? "'" + base + "' is open now, or shorter" : "'" + base + "' is now " + std::to_string(cut.pieces.size()) + " routes";
			if (dropped) text += "; " + std::to_string(dropped) + (dropped == 1 ? " junction" : " junctions") + " went with the removed part";
			Status(text + ". Undo brings it back; Save keeps it.");
		}
		// Merge the routes selected into one, nearest ends first, however far apart: a gap becomes a straight stretch. With no routes selected: the nearest open route within 300 m of
		// this one's end. It keeps the settings of the route open or shown, the others go (an original is switched off, as Del route does).
		void MergeRoutes()
		{
			const auto picks = PicksNow();
			std::vector<std::string> ids;
			if (g_ed.draft) ids.push_back(g_ed.draft->Value().name);
			else if (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size())) ids.push_back(g_ed.file.routes[std::size_t(g_ed.route)].id);
			for (const auto& p : picks) if (p.kind == kPickRoute && std::find(ids.begin(), ids.end(), p.id) == ids.end()) ids.push_back(p.id);
			if (ids.empty()) { Status("Select two routes first: Ctrl+click their lines"); return; }
			if (ids.size() == 1) {
				trafficdata::Route scratch;
				const auto* shown = Shown(scratch);
				if (!shown || shown->points.empty() || shown->ring) { Status("Select two routes first: Ctrl+click their lines"); return; }
				const auto end = shown->points.back();
				double bestD = 300;
				std::string best;
				for (const auto& o : g_ed.file.routes) {
					if (!Usable(o) || o.ring || o.closed || o.points.size() < 2 || o.id == ids[0]) continue;
					for (const auto& q : { o.points.front(), o.points.back() }) { const double d = std::hypot(q.x - end.x, q.y - end.y); if (d < bestD) { bestD = d; best = o.id; } }
				}
				if (best.empty()) { Status("No open route starts or ends within 300 m of this one's end. Ctrl+click two routes to merge them whatever the distance."); return; }
				ids.push_back(best);
			}
			for (const auto& id : ids) if (const int k = RouteIndex(id); k >= 0 && !IsDraftRoute(id) && g_ed.file.routes[std::size_t(k)].ring) { Status("A ring can't be merged: it has no points of its own"); return; }
			g_ed.picks.clear();
			if (!g_ed.draft) { g_ed.route = RouteIndex(ids[0]); g_ed.other = -1; }
			if (!EnsureDraft()) return;
			if (g_ed.draft->Value().ring) { Status("A ring can't be merged: it has no points of its own"); return; }
			std::vector<trafficdata::Route> in{ g_ed.draft->Value() };
			for (std::size_t i = 1; i < ids.size(); ++i) in.push_back(ToTd(g_ed.file.routes[std::size_t(RouteIndex(ids[i]))], EditorKey()));
			auto merged = routeeditor::MergeRoutes(in);
			PushSnap();
			g_ed.draft->Edit(merged.route);
			g_ed.massUndo.back().draftChanged = true;
			g_ed.massUndo.back().depth = DraftDepth();
			const std::string name = merged.route.name;
			const int newLast = int(merged.route.points.size()) - 1;
			const int dropped = MoveLinks([&](const std::string& id, int at) -> MovedTo {
				for (std::size_t i = 0; i < ids.size(); ++i) {
					if (ids[i] != id) continue;
					const auto& map = merged.map[i];
					return { name, map[std::size_t(at < 0 ? int(map.size()) - 1 : std::min(at, int(map.size()) - 1))] };
				}
				return {};
			}, [&](const std::string& id) { return id == name ? newLast : -1; });
			for (std::size_t i = 1; i < ids.size(); ++i) {
				const int k = RouteIndex(ids[i]);
				if (k < 0) continue;
				auto& o = g_ed.file.routes[std::size_t(k)];
				if (IsOriginal(o)) { o.removed = true; o.points.clear(); o.rounding.clear(); continue; }
				g_ed.shifts.erase(ids[i]);
				g_ed.file.routes.erase(g_ed.file.routes.begin() + k);
			}
			if (g_ed.draftIndex >= 0) g_ed.draftIndex = RouteIndex(name);
			g_ed.route = g_ed.draftIndex;
			g_ed.point = -1;
			std::string text = std::to_string(ids.size()) + " routes merged into '" + (merged.route.label.empty() ? name : merged.route.label) + "' with its settings";
			if (dropped) text += "; " + std::to_string(dropped) + (dropped == 1 ? " junction" : " junctions") + " between them removed";
			Status(text + ". Undo brings them back; Save keeps it.");
		}
		void RestoreOriginal()
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			if (g_ed.route < 0 && g_ed.other >= 0) { Status("This is already the original"); return; }
			if (g_ed.route < 0 || g_ed.route >= int(g_ed.file.routes.size()) || !IsOriginal(g_ed.file.routes[std::size_t(g_ed.route)])) {
				Status("Restore is for original routes you've edited"); return;
			}
			DeleteRoute();  // the original comes back as its file has it (asks first)
		}

		// A click on a line of the routes list (the same choice as < Route / Route >).
		// ---- Shapes and parallel lanes (2026-10-30, Traffic Manager V1) ----
		// A shape's height: 40 m over the ground under its centre; ring a building: 20 m over the highest thing round the circle.
		bool ShapeBase(const routemap::Camera& c, routemap::Vec3& at)
		{
			if (!routemap::MouseAt(c, c.focus.z, at)) { Status("The cursor's ray misses the ground plane; move the cursor"); return false; }
			double ground = 0;
			if (DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground)) at.z = ground + 40.0;
			else at.z = std::max(kPlaceFloor, at.z + 40.0);
			return true;
		}
		void PlaceShape(const routemap::Camera& c)
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			if (!g_ed.shapeAt) {
				routemap::Vec3 at;
				if (!ShapeBase(c, at)) return;
				g_ed.shapeCentre = at; g_ed.shapeAt = true;
				Status(std::string(routeeditor::ShapeName(g_ed.shape)) + ": centre placed. Click again to set how big it is.");
				return;
			}
			routemap::Vec3 edge;
			routeeditor::Point centre{ g_ed.shapeCentre.x, g_ed.shapeCentre.y, g_ed.shapeCentre.z };
			if (!routemap::MouseAt(c, centre.z, edge)) { Status("The cursor can't be placed at that height; move it"); return; }
			const double radius = std::clamp(std::hypot(edge.x - centre.x, edge.y - centre.y), 30.0, 1500.0);
			if (g_ed.shape == routeeditor::Shape::RingBuilding) {
				double top = centre.z - 40.0, hit = 0;
				for (int k = 0; k <= 8; ++k) {
					const double a = k * 0.7853981633974483, r = k == 8 ? 0.0 : radius * 0.8;
					if (DownRay(centre.x + r * std::cos(a), centre.y + r * std::sin(a), centre.z + 1500.0, centre.z - 1500.0, hit) == 1) top = std::max(top, hit);
				}
				centre.z = top + 20.0;
			}
			trafficdata::Route t;
			t.label = std::string(routeeditor::ShapeName(g_ed.shape)) + " " + std::to_string(g_ed.file.routes.size() + 1);
			t.name = routefiles::NewId(t.label, TakenIds());
			t.world = EditorKey(); t.ring = false; t.speed = 25; t.width = 20; t.lanes = 1;
			t.schedule = routeschedule::Schedule{ true, 2, {} };
			t.closed = g_ed.shape != routeeditor::Shape::Helix;
			t.points = routeeditor::ShapePoints(g_ed.shape, centre, radius, 0, &t.rounding);
			if (!RouteFits(t, routeeditor::ShapeName(g_ed.shape))) { g_ed.shapeAt = false; return; }
			g_ed.draft.emplace(t);
			g_ed.draftShift = {}; g_ed.draftIndex = -1;
			g_ed.route = -1; g_ed.other = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1;
			g_ed.shapeAt = false; g_ed.mode = Mode::Route;
			char b[128]; std::snprintf(b, sizeof b, "%s placed, %.0f m across, at %.0f m. Its values are in the panel; the Route tool moves, turns or raises it. Then Save.", t.label.c_str(), radius * 2, centre.z);
			Status(b);
		}
		void ParallelLanes()
		{
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			trafficdata::Route scratch;
			const auto* shown = Shown(scratch);
			if (!shown || shown->ring) { Status("Select a route with points first"); return; }
			trafficdata::Route t = *shown;
			t.label = routefiles::CleanLabel((t.label.empty() ? t.name : t.label) + " lane");
			t.name = routefiles::NewId(t.label, TakenIds());
			t.world = EditorKey(); t.removed = false; t.test = false; t.color = 0;
			t.points = routeeditor::Parallel(t.points, t.closed, 30);
			if (!RouteFits(t, "Parallel lanes")) return;
			g_ed.draft.emplace(t);
			g_ed.draftShift = {}; g_ed.draftIndex = -1;
			g_ed.route = -1; g_ed.other = -1; g_ed.point = -1; g_ed.spot = -1; g_ed.link = -1;
			g_ed.mode = Mode::Route;
			Status("'" + t.label + "' is a parallel route 30 m to the right. Save to keep it.");
		}
		// ---- Landing areas and zones on the map (click: select one, move the selected one, or place a new one) ----
		bool ClickedPlace(const routemap::Camera& c, double mx, double my)
		{
			for (std::size_t i = 0; i < g_ed.file.landings.size(); ++i) {
				const auto& s = g_ed.file.landings[i];
				double x, y;
				if (SpotHere(s) && Local(c, { s.at.x, s.at.y, s.at.z }, x, y) && std::hypot(x - mx, y - my) < 14) {
					g_ed.spotKind = 1; g_ed.spot = int(i); g_ed.zone = -1; g_ed.point = -1; g_ed.link = -1;
					Status("Landing " + std::to_string(i + 1) + ": " + AreaWords(s));
					return true;
				}
			}
			for (std::size_t i = 0; i < g_ed.file.zones.size(); ++i) {
				const auto& z = g_ed.file.zones[i];
				double x, y;
				if (!g_ed.unplacedSpots.count(z.name) && Local(c, { z.at.x, z.at.y, z.slow ? z.at.z : z.top }, x, y) && std::hypot(x - mx, y - my) < 14) {
					g_ed.zone = int(i); g_ed.spot = -1; g_ed.point = -1; g_ed.link = -1;
					Status("Zone " + std::to_string(i + 1) + ": " + ZoneWords(z));
					return true;
				}
			}
			return false;
		}
		void ClickLand(const routemap::Camera& c, double mx, double my)
		{
			if (ClickedPlace(c, mx, my)) return;
			if (!AreaOn()) {
				AddSpot(c, 1);
				if (AreaOn()) { auto& a = g_ed.file.landings[std::size_t(g_ed.spot)]; a.layout = "row"; a.spaces = 4; Status("Landing area placed: set it up, then press Done. Save keeps it."); }
				return;
			}
			auto& a = g_ed.file.landings[std::size_t(g_ed.spot)];
			routemap::Vec3 at;
			if (!routemap::MouseAt(c, c.focus.z, at)) { Status("The cursor's ray misses the ground plane; move the cursor"); return; }
			if (double ground = 0; DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground)) at.z = ground + kGroundLift;
			else at.z = a.at.z;
			if (const char* why = PointProblem(at.x, at.y, at.z)) { Refused("Landing area", at.x, at.y, at.z, why); return; }
			a.at = { at.x, at.y, at.z };
			Status("Landing area moved. Done shrinks it to one line; Save keeps it.");
		}
		void ClickZone(const routemap::Camera& c, double mx, double my)
		{
			if (ClickedPlace(c, mx, my)) return;
			routemap::Vec3 at;
			if (!routemap::MouseAt(c, c.focus.z, at)) { Status("The cursor's ray misses the ground plane; move the cursor"); return; }
			double ground = at.z;
			const bool known = DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground);
			if (!known) ground = at.z;
			if (ZoneOn()) {
				auto& z = g_ed.file.zones[std::size_t(g_ed.zone)];
				z.at = { at.x, at.y, ground };
				Status("Zone moved. Done shrinks it to one line; Save keeps it.");
				return;
			}
			std::set<std::string> taken;
			for (const auto& z : g_ed.file.zones) taken.insert(z.name);
			routefiles::Zone z;
			z.name = routefiles::NewId("zone " + std::to_string(g_ed.file.zones.size() + 1), taken);
			z.at = { at.x, at.y, ground }; z.top = ground + 80.0; z.radius = 100;
			trafficdata::Vec shift;
			z.world = CityAt(at.x, at.y, shift);
			g_ed.spotShifts[z.name] = shift;
			g_ed.file.zones.push_back(z);
			g_ed.zone = int(g_ed.file.zones.size()) - 1; g_ed.spot = -1; g_ed.point = -1; g_ed.link = -1;
			Status("Keep-out zone placed: set it up, then press Done. Save keeps it.");
		}
		void EditDay(void (*change)(routeschedule::Schedule&, int), int arg)
		{
			if (!EnsureDraft()) return;
			auto r = g_ed.draft->Value();
			change(r.schedule, arg);
			routeschedule::Expand(r.schedule);
			g_ed.draft->Edit(r);
			Status("Time rules: " + ScheduleText(r.schedule) + " (Save to keep them)");
		}
		void SelectRow(int at)
		{
			if (at < 0) { Status("This new route is open: Save or Cancel it"); return; }
			if (g_ed.draft && at == g_ed.draftIndex) return;
			if (g_ed.draft) { Status("Save or Cancel the open route first"); return; }
			g_ed.sheet = 0;
			const int mine = int(g_ed.file.routes.size());
			if (at >= ListTotal()) return;
			g_ed.point = -1; g_ed.spot = -1; g_ed.link = -1;
			if (at >= mine) {
				g_ed.route = -1; g_ed.other = at - mine;
				const auto& o = g_ed.others[std::size_t(g_ed.other)];
				Status("Original route '" + o.name + "' from " + o.file + ": any change makes your own copy; Delete route switches it off");
				return;
			}
			g_ed.route = at; g_ed.other = -1;
			const auto& r = g_ed.file.routes[std::size_t(at)];
			if (r.removed) Status("Removed original '" + r.id + "': Bring back switches it on again");
			else if (g_ed.unplaced.count(r.id)) Status("Route '" + r.label + "' belongs to a city not found in this world now; it is kept unchanged");
			else Status("Route '" + r.label + "'" + (IsOriginal(r) ? " (your version of an original; Delete route reverts it)" : ""));
		}

		// Every other button: which kind of selected thing it belongs to, or 0. Its own handler then runs once for each of that kind, so each kind's rules stay in one place and a
		// new button only has to be listed here. Move, height, turn, size and delete are not here: they go to everything selected (GroupStep).
		int ReplayKind(int id)
		{
			if (g_ed.ruleScope != 0 && (id == kAllowAll || (id >= kTypeBase && id < kTypeBase + 1000))) return 0;  // a group's list, not the routes'
			switch (id) {
			case kSpeedDown: case kSpeedUp: case kVehDown: case kVehUp: case kOneWay: case kTwoWay: case kLoopOn: case kLoopOff: case kRoleNext: case kColor: case kReverse:
			case kAllDay: case kByHours: case kSchedule: case kByWords: case kWordsAdd: case kBlendQuick: case kBlendGentle: case kBlendSlow: case kStartDown: case kStartUp: case kEndDown: case kEndUp:
			case kGroupNext: case kAllowAll: case kRadiusDown: case kRadiusUp:case kShapeADown: case kShapeAUp: case kShapeBDown: case kShapeBUp: case kShapeCDown: case kShapeCUp:
				return kPickRoute;
			case kLayoutSingle: case kLayoutRow: case kLayoutBays: case kLayoutRing: case kSpacesDown: case kSpacesUp: case kAreaTurnDown: case kAreaTurnUp:
			case kPurposeTaxi: case kPurposeDepot: case kPurposeStop: case kStayShort: case kStayLong: case kStayNeeded: case kFullQueue: case kFullCircle: case kFullOn: case kWhoParks:
				return kPickLanding;
			case kZoneKeep: case kZoneSlow: case kZoneSizeDown: case kZoneSizeUp: case kZoneTopDown: case kZoneTopUp: case kZoneSpeedDown: case kZoneSpeedUp:
				return kPickZone;
			case kShareDown: case kShareUp: case kJunctionBoth: case kJunctionExit: case kJunctionMerge: case kJunctionCross: case kJunctionSuggest:
				return kPickLink;
			case kRoundPoint: case kRoundDown: case kRoundUp: case kSharp: case kRounded:
				return g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size()) ? kPickLink : kPickPoint;
			default:
				if ((id >= kDayRow && id < kPlaceRow) || (id >= kTypeBase && id < kTypeBase + 1000)) return kPickRoute;
				return 0;
			}
		}
		void ShowReadme();
		void Press(int id, const routemap::Camera& c);
		bool g_replaying = false;
		bool Replay(int id, const routemap::Camera& c)
		{
			if (!Many()) return false;
			// The two keys that flip: everything goes the way the one in the panel goes, or they would swap.
			if (id == kWays || id == kLoop) {
				trafficdata::Route scratch;
				const auto* shown = Shown(scratch);
				if (!shown) return false;
				id = id == kWays ? (shown->lanes > 1 ? kOneWay : kTwoWay) : (shown->closed ? kLoopOff : kLoopOn);
			}
			const int kind = ReplayKind(id);
			if (!kind) return false;
			const auto picks = PicksNow();
			bool any = false;
			for (const auto& p : picks) if (p.kind == kind) any = true;
			if (!any) return false;

			const auto primary = PrimaryNow();
			const std::size_t depth = DraftDepth();
			PushSnap();
			g_replaying = true;
			int done = 0;
			auto run = [&]() { Press(id, c); ++done; };
			if (kind == kPickRoute) {
				// The route the panel shows is one of them, selected whole or not.
				trafficdata::Route scratch;
				const auto* shown = Shown(scratch);
				const std::string shownId = shown ? shown->name : std::string(), shownRole = shown ? shown->role : std::string(), shownGroup = shown ? shown->group : std::string();
				std::vector<std::string> ids;
				for (const auto& p : picks) if (p.kind == kPickRoute && !IsDraftRoute(p.id)) ids.push_back(p.id);
				if (!g_ed.draft && RouteIndex(shownId) >= 0 && std::find(ids.begin(), ids.end(), shownId) == ids.end()) ids.push_back(shownId);
				if (id == kRoleNext) for (const auto& name : ids) g_ed.file.routes[std::size_t(RouteIndex(name))].role = shownRole;  // the same next role for all
				if (id == kGroupNext) for (const auto& name : ids) g_ed.file.routes[std::size_t(RouteIndex(name))].group = shownGroup;  // the same next group for all
				auto open = std::move(g_ed.draft);
				const int openIndex = g_ed.draftIndex;
				const auto openShift = g_ed.draftShift;
				g_ed.draft.reset();
				for (const auto& name : ids) {
					const int k = RouteIndex(name);
					if (k < 0 || !Usable(g_ed.file.routes[std::size_t(k)])) continue;
					g_ed.route = k; g_ed.other = -1; g_ed.point = -1;
					g_ed.draft.emplace(ToTd(g_ed.file.routes[std::size_t(k)], EditorKey()));
					g_ed.draftIndex = k;
					run();
					if (g_ed.draft) g_ed.file.routes[std::size_t(k)] = FromTd(g_ed.draft->Value());
					g_ed.draft.reset();
				}
				g_ed.draft = std::move(open); g_ed.draftIndex = openIndex; g_ed.draftShift = openShift;
				g_ed.route = primary[0]; g_ed.other = primary[1]; g_ed.point = primary[2];
				if (g_ed.draft) {
					run();
					if (id == kReverse) {
						const int n = int(g_ed.draft->Value().points.size());
						for (auto& p : g_ed.picks) if (p.kind == kPickPoint) p.index = n - 1 - p.index;
					}
				}
			}
			else if (kind == kPickPoint) {
				// Corners end up alike: the radius of the one last clicked is where they all start from.
				auto start = g_ed.draft->Value();
				start.rounding.resize(start.points.size(), 0);
				const double was = g_ed.point >= 0 ? start.rounding[std::size_t(g_ed.point)] : 0;
				if (id == kRoundDown || id == kRoundUp) {
					for (const auto& p : picks) if (p.kind == kPickPoint && (start.closed || (p.index != 0 && p.index + 1 != int(start.points.size())))) start.rounding[std::size_t(p.index)] = was;
					g_ed.draft->Edit(start);
				}
				// A plain toggle goes the same way for all.
				const int step = id == kRoundPoint ? (was > 0 ? kSharp : kRounded) : id;
				id = step;
				for (const auto& p : picks) {
					if (p.kind != kPickPoint) continue;
					g_ed.point = p.index;
					Press(step, c);
				}
				g_ed.point = primary[2];
				const auto& r = g_ed.draft->Value();
				int rounded = 0;
				for (const auto& p : picks) if (p.kind == kPickPoint && std::size_t(p.index) < r.rounding.size() && r.rounding[std::size_t(p.index)] > 0) ++rounded;
				done = rounded;
			}
			else {
				if (kind == kPickLanding && id == kWhoParks && AreaOn())
					for (const auto& p : picks) if (p.kind == kPickLanding) g_ed.file.landings[std::size_t(LandingIndex(p.id))].routes = g_ed.file.landings[std::size_t(g_ed.spot)].routes;  // the same next for all
				for (const auto& p : picks) {
					if (p.kind != kind) continue;
					if (kind == kPickLanding) { g_ed.spotKind = 1; g_ed.spot = LandingIndex(p.id); g_ed.zone = -1; g_ed.link = -1; }
					else if (kind == kPickZone) { g_ed.zone = ZoneIndex(p.id); g_ed.spot = -1; g_ed.link = -1; }
					else { g_ed.link = LinkIndex(p.id); g_ed.spot = -1; g_ed.zone = -1; }
					run();
				}
				g_ed.spot = primary[3]; g_ed.spotKind = primary[4]; g_ed.zone = primary[5]; g_ed.link = primary[6];
			}
			g_replaying = false;
			if (g_ed.draft && DraftDepth() > depth) {
				g_ed.draft->CollapseTo(depth);
				g_ed.massUndo.back().draftChanged = true;
				g_ed.massUndo.back().depth = DraftDepth();
			}
			g_ed.pickPrimary = PrimaryNow();
			Status(kind == kPickPoint ? std::to_string(done) + " of the selected points now rounded" : "All " + std::to_string(done) + ": " + g_ed.status);
			return true;
		}
		void Press(int id, const routemap::Camera& c)
		{
			if (!g_replaying && Replay(id, c)) return;
			const bool shiftHeld = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
			if (id == kSelect || id == kDrop || id == kDrag || id == kRouteTool || id == kConnect) { g_ed.spot = -1; g_ed.zone = -1; }  // a route tool goes back to the route
			if (id == kLanding) g_ed.zone = -1;
			switch (id) {
			case kJunctionBoth: case kJunctionExit: case kJunctionMerge: case kJunctionCross: case kJunctionSuggest: {
				if (g_ed.link < 0 || g_ed.link >= int(g_ed.file.links.size())) break;
				auto& k = g_ed.file.links[std::size_t(g_ed.link)];
				k.type = id == kJunctionExit ? "exit" : id == kJunctionMerge ? "merge" : id == kJunctionCross ? "crossing" : id == kJunctionSuggest ? SuggestedType(k) : std::string();
				Status(TypeWords(k, k.type) + " (Save to keep it)");
				break;
			}
			case kShapeTool: g_ed.menu = g_ed.menu == 3 ? 0 : 3; break;
			case kZoneTool: g_ed.mode = Mode::Zone; g_ed.spot = -1; Status("Zone: click the map to place a keep-out or slow zone, or click one to change it"); break;
			case kTimeRules: g_ed.sheet = 2; break;
			case kCity: g_ed.sheet = g_ed.sheet == 3 ? 0 : 3; g_ed.file.cityWritten = true; break;
			case kPlaceDone: g_ed.spot = -1; g_ed.zone = -1; Status("Done: it is one line under Places now. Save keeps your changes."); break;
			case kRoleNext: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				r.role = r.role == "taxi" ? "cargo" : r.role == "cargo" ? "patrol" : r.role == "patrol" ? "" : "taxi";
				g_ed.draft->Edit(r);
				Status(r.role == "taxi" ? "Taxi: its vehicles land only at landing areas with the Taxi purpose" : r.role == "cargo" ? "Cargo: its vehicles fly at 80% speed and land only at Depots" :
					r.role == "patrol" ? "Patrol: its vehicles only fly laps and don't use landing areas" : "Civilian: its vehicles land at any area but a Depot");
				break;
			}
			case kByWords: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				if (!r.schedule.zones.empty()) break;
				int peak = r.schedule.allDay ? r.schedule.allDayVehicles : 0;
				for (const auto& w : r.schedule.windows) peak = std::max(peak, w.vehicles);
				r.schedule.peak = std::max(1, peak);
				r.schedule.zones = { { 0, 1 }, { 420, 3 }, { 600, 2 }, { 1020, 4 }, { 1200, 2 }, { 1320, 1 } };
				routeschedule::Expand(r.schedule);
				g_ed.draft->Edit(r);
				Status("Busy words: change each stretch of the day, or the share each word means (Save to keep them)");
				break;
			}
			case kWordsAdd:
				EditDay([](routeschedule::Schedule& s, int) {
					// In the middle of the longest stretch.
					int best = 0, at = 0;
					const int n = int(s.zones.size());
					for (int i = 0; i < n; ++i) {
						const int from = s.zones[std::size_t(i)].start, to = n > 1 ? s.zones[std::size_t((i + 1) % n)].start : from;
						int length = (to - from + routeschedule::kMinutesPerDay) % routeschedule::kMinutesPerDay;
						if (!length) length = routeschedule::kMinutesPerDay;
						if (length > best) { best = length; at = (from + length / 2) % routeschedule::kMinutesPerDay; }
					}
					at -= at % 30;
					for (const auto& z : s.zones) if (z.start == at) return;
					s.zones.push_back({ at, 2 });
				}, 0);
				break;
			case kBlendQuick: EditDay([](routeschedule::Schedule& s, int) { s.blend = 10; }, 0); break;
			case kBlendGentle: EditDay([](routeschedule::Schedule& s, int) { s.blend = 30; }, 0); break;
			case kBlendSlow: EditDay([](routeschedule::Schedule& s, int) { s.blend = 60; }, 0); break;
			case kCityOn: case kCityOff: g_ed.file.city.on = id == kCityOn; g_ed.file.cityWritten = true; Status(id == kCityOn ? "Traffic on here (Save to keep it)" : "Traffic off here: vehicles in the air finish their lap and land (Save to keep it)"); break;
			case kDensityDown: case kDensityUp: g_ed.file.city.density = std::clamp(g_ed.file.city.density + (id == kDensityUp ? 10 : -10), 0, 200); g_ed.file.cityWritten = true; Status("Density " + std::to_string(g_ed.file.city.density) + "% (Save to keep it)"); break;
			case kLimitDown: case kLimitUp: g_ed.file.city.limit = std::clamp(g_ed.file.city.limit + (id == kLimitUp ? 8 : -8), 0, fvolimits::kVehicles); g_ed.file.cityWritten = true; Status("At most " + std::to_string(g_ed.file.city.limit) + " vehicles at once (Save to keep it)"); break;
			case kOriginalsOn: case kOriginalsOff: g_ed.file.city.originals = id == kOriginalsOn; g_ed.file.cityWritten = true; Status(id == kOriginalsOn ? "Original routes fly (Save to keep it)" : "Original routes left out: only your own routes fly (Save to keep it)"); break;
			case kLayoutSingle: case kLayoutRow: case kLayoutBays: case kLayoutRing: case kSpacesDown: case kSpacesUp: case kAreaTurnDown: case kAreaTurnUp:
			case kPurposeTaxi: case kPurposeDepot: case kPurposeStop: case kStayShort: case kStayLong: case kStayNeeded: case kFullQueue: case kFullCircle: case kFullOn: case kWhoParks: {
				if (!AreaOn()) break;
				auto& a = g_ed.file.landings[std::size_t(g_ed.spot)];
				switch (id) {
				case kLayoutSingle: a.layout = "single"; break;
				case kLayoutRow: a.layout = "row"; a.spaces = std::max(a.spaces, 2); break;
				case kLayoutBays: a.layout = "bays"; a.spaces = std::max(a.spaces, 4); break;
				case kLayoutRing: a.layout = "ring"; a.spaces = std::max(a.spaces, 4); break;
				case kSpacesDown: a.spaces = std::max(1, a.spaces - 1); break;
				case kSpacesUp: a.spaces = std::min(24, a.spaces + 1); break;
				case kAreaTurnDown: a.turn = std::fmod(a.turn + 345.0, 360.0); break;
				case kAreaTurnUp: a.turn = std::fmod(a.turn + 15.0, 360.0); break;
				case kPurposeTaxi: a.purpose = "taxi"; break;
				case kPurposeDepot: a.purpose = "depot"; break;
				case kPurposeStop: a.purpose = "stop"; break;
				case kStayShort: a.stay = 120; break;
				case kStayLong: a.stay = 600; break;
				case kStayNeeded: a.stay = 0; break;
				case kFullQueue: a.full = "queue"; break;
				case kFullCircle: a.full = "circle"; break;
				case kFullOn: a.full = "on"; break;
				default: {  // Who parks: all routes, then each route in turn
					std::vector<std::string> names;
					for (const auto& r : g_ed.file.routes) if (!r.removed) names.push_back(r.id);
					for (const auto& o : g_ed.others) names.push_back(o.name);
					const std::string now = a.routes.empty() ? std::string() : a.routes.front();
					const auto at = std::find(names.begin(), names.end(), now);
					a.routes.clear();
					if (now.empty()) { if (!names.empty()) a.routes.push_back(names.front()); }
					else if (at != names.end() && at + 1 != names.end()) a.routes.push_back(*(at + 1));
					break;
				}
				}
				Status("Landing " + std::to_string(g_ed.spot + 1) + ": " + AreaWords(a) + " (Save to keep it)");
				break;
			}
			case kZoneKeep: case kZoneSlow: case kZoneSizeDown: case kZoneSizeUp: case kZoneTopDown: case kZoneTopUp: case kZoneSpeedDown: case kZoneSpeedUp: {
				if (!ZoneOn()) break;
				auto& z = g_ed.file.zones[std::size_t(g_ed.zone)];
				const double big = shiftHeld ? 5 : 1;
				switch (id) {
				case kZoneKeep: z.slow = false; break;
				case kZoneSlow: z.slow = true; break;
				case kZoneSizeDown: z.radius = std::max(10.0, z.radius - 10 * big); break;
				case kZoneSizeUp: z.radius = std::min(2000.0, z.radius + 10 * big); break;
				case kZoneTopDown: z.top -= 10 * big; break;
				case kZoneTopUp: z.top += 10 * big; break;
				case kZoneSpeedDown: z.speed = std::max(5.0, z.speed - 5); break;
				default: z.speed = std::min(80.0, z.speed + 5); break;
				}
				Status("Zone " + std::to_string(g_ed.zone + 1) + ": " + ZoneWords(z) + " (Save to keep it)");
				break;
			}
			case kVehicleRules: g_ed.sheet = 1; g_ed.ruleScope = 0; g_ed.typePage = 0; g_types.clear(); Status("Vehicle rules: click a type to leave it out of this route"); break;
			case kSheetBack: if (g_ed.sheet == 1 && g_ed.ruleScope != 0) { g_ed.sheet = 4; g_ed.ruleScope = 0; } else g_ed.sheet = 0; break;
			case kGroupSheet: g_ed.sheet = 4; g_ed.ruleScope = 0; g_types.clear(); break;
			case kGroupNew: g_ed.naming = true; g_ed.namingGroup = 1; g_ed.nameBuf.clear(); Status("New group: type its name and press Enter"); break;
			case kGroupRename: g_ed.naming = true; g_ed.namingGroup = 2; g_ed.nameBuf = GroupLabel(g_ed.ruleGroup); Status("Rename the group: type its name and press Enter"); break;
			case kGroupDelete: {
				const ULONGLONG nowMs = GetTickCount64();
				if (nowMs - g_ed.confirmDeleteAt > 3000) { g_ed.confirmDeleteAt = nowMs; Status("Delete group '" + GroupLabel(g_ed.ruleGroup) + "'? Its routes stay and just leave the group. Click again within 3 s"); break; }
				g_ed.confirmDeleteAt = 0;
				const std::string gone = g_ed.ruleGroup, label = GroupLabel(gone);
				g_ed.routeGroups.erase(std::remove_if(g_ed.routeGroups.begin(), g_ed.routeGroups.end(), [&](const trafficdata::RouteGroup& g) { return g.name == gone; }), g_ed.routeGroups.end());
				for (auto& r : g_ed.file.routes) if (r.group == gone) r.group.clear();
				if (g_ed.draft && g_ed.draft->Value().group == gone) { auto r = g_ed.draft->Value(); r.group.clear(); g_ed.draft->Edit(r); }
				g_ed.sheet = 4; g_ed.ruleScope = 0;
				Status("Group '" + label + "' deleted; its routes on this planet left it. Save keeps it.");
				break;
			}
			case kGroupNext: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				const auto choosable = ChoosableGroups();
				if (choosable.empty()) { Status("No groups yet: open Route groups and make one"); break; }
				std::size_t at = choosable.size();
				for (std::size_t k = 0; k < choosable.size(); ++k) if (choosable[k]->name == r.group) at = k;
				r.group = at == choosable.size() ? choosable[0]->name : at + 1 < choosable.size() ? choosable[at + 1]->name : std::string();
				g_ed.draft->Edit(r);
				Status("Group: " + GroupLabel(r.group) + " (Save to keep it)");
				break;
			}
			case kTypesMore: ++g_ed.typePage; break;
			case kSplit: DisconnectRoute(); break;
			case kJoin: MergeRoutes(); break;
			case kRestore: RestoreOriginal(); break;
			case kAllowAll:
				if (g_ed.ruleScope != 0) { if (auto* target = RuleTarget()) { target->exclude.clear(); Status("Every vehicle type is allowed again for " + (g_ed.ruleScope == 2 ? std::string("all routes") : target->label) + " (Save to keep it)"); } break; }
				if (EnsureDraft()) { auto r = g_ed.draft->Value(); r.exclude.clear(); g_ed.draft->Edit(r); Status("Every vehicle type may fly this route again (Save to keep it)"); }
				break;
			case kRouteTool: g_ed.mode = Mode::Route; Status("Route: drag the map to move the whole route, drag the round handle to turn it, wheel near its centre to raise or lower it, Ctrl+wheel there to size it"); break;
			case kMoreMenu: g_ed.menu = g_ed.menu == 1 ? 0 : 1; break;
			case kViewMenu: g_ed.menu = g_ed.menu == 2 ? 0 : 2; break;
			case kCopy: CopyRoute(c); break;
			case kMirror: MirrorRoute(c); break;
			case kGroupFlow: case kGroupPosition: case kGroupRules: case kProblems: {
				const int gi = id == kProblems ? 3 : id - kGroupFlow;  // each opens and closes on its own
				g_ed.groups ^= 1 << gi;
				if (g_ed.groups >> gi & 1) g_ed.lastGroup = gi;
				break;
			}
			case kRouteUp: RaiseRoute(shiftHeld ? 25 : 10); break;
			case kRouteDown: RaiseRoute(shiftHeld ? -25 : -10); break;
			case kSizeUp: ScaleRoute(shiftHeld ? 1.25 : 1.1); break;
			case kSizeDown: ScaleRoute(shiftHeld ? 1 / 1.25 : 1 / 1.1); break;
			case kShapeAUp: case kShapeADown: ShapeStep('a', id == kShapeAUp ? 1 : -1, shiftHeld); break;
			case kShapeBUp: case kShapeBDown: ShapeStep('b', id == kShapeBUp ? 1 : -1, shiftHeld); break;
			case kShapeCUp: case kShapeCDown: ShapeStep('c', id == kShapeCUp ? 1 : -1, shiftHeld); break;
			case kTurnLeft: TurnRoute(-15); break;
			case kTurnRight: TurnRoute(15); break;
			case kOneWay: case kTwoWay: {
				trafficdata::Route s2;
				const auto* r = Shown(s2);
				if (!r) { Status("Select a route first"); break; }
				if ((r->lanes > 1) == (id == kTwoWay)) break;
				if (id == kTwoWay) EditRoute([](trafficdata::Route& x) { x.lanes = 2; }, "Two way: a second lane runs the other way");
				else EditRoute([](trafficdata::Route& x) { x.lanes = 1; }, "One way: every vehicle flies the same way round");
				break;
			}
			case kLoopOn: case kLoopOff:
				if (EnsureDraft()) {
					const bool want = id == kLoopOn;
					if (g_ed.draft->Value().ring) { Status("A ring is always a loop"); break; }
					if (g_ed.draft->Value().closed == want) break;
					g_ed.draft->Close(want);
					Status(want ? "Loop: the last point joins the first" : "Open route: vehicles turn back at the ends");
				}
				break;
			case kAllDay: case kByHours: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				const bool want = id == kAllDay;
				const bool words = !r.schedule.zones.empty();
				if (r.schedule.allDay == want && !words) break;
				if (words) {  // from busy words: the most vehicles carries over, the zones' windows go
					r.schedule.allDayVehicles = r.schedule.peak;
					const int most = std::max(1, r.schedule.peak);
					r.schedule.zones.clear(); r.schedule.windows.clear();
					if (!want) r.schedule.windows.push_back({ 6 * 60, 10 * 60, most });
				}
				r.schedule.allDay = want;
				if (!want) FirstWindow(r);
				g_ed.draft->Edit(r);
				Status(want ? "All day" : "By hours (planet local time): " + ScheduleText(r.schedule));
				break;
			}
			case kSharp: case kRounded: {
				const bool junction = g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size());
				double current = 0;
				if (junction) current = g_ed.file.links[std::size_t(g_ed.link)].radius;
				else {
					trafficdata::Route s2;
					const auto* r = Shown(s2);
					if (r && g_ed.point >= 0 && std::size_t(g_ed.point) < r->rounding.size()) current = r->rounding[std::size_t(g_ed.point)];
				}
				if ((id == kRounded) != (current > 0)) Press(kRoundPoint, c);
				break;
			}
			case kRename: {
				trafficdata::Route renaming;
				if (!Shown(renaming)) { Status("Select a route first"); break; }
				g_ed.naming = true; g_ed.nameBuf.clear();
				Status("Rename '" + (renaming.label.empty() ? renaming.name : renaming.label) + "': type the new name and press Enter (Esc keeps it)");
				break;
			}
			case kColor: {
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				const unsigned now = RouteColor(r, ShownIndex());
				int at = 0;
				for (int i = 0; i < kRouteColorCount; ++i) if (kRouteColors[i] == now) at = i;
				r.color = kRouteColors[(at + 1) % kRouteColorCount];
				g_ed.draft->Edit(r);
				Status("Route colour changed (Save to keep)");
				break;
			}
			case kNew: NewRoute(); break;
			case kSave: Save(); break;
			case kReadme: ShowReadme(); break;
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
			case kDrop: g_ed.mode = Mode::Drop; Status("Add: click the map to add a point after the selected one (Insert does the same with any tool)"); break;
			case kDrag: g_ed.mode = Mode::Drag; Status("Move: hold the left button on a point and move it; it keeps its height"); break;
			case kLanding: g_ed.mode = Mode::Landing; Status("Land: click where vehicles may land between laps"); break;
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
			case kUndo: if (UndoAny()) Status("Undone"); break;
			case kRedo: if (RedoAny()) Status("Redone"); break;
			case kSpeedDown: EditRoute([](trafficdata::Route& r) { r.speed = std::max(5.0, r.speed - 5); }, "Speed lowered"); break;
			case kSpeedUp: EditRoute([](trafficdata::Route& r) { r.speed = std::min(80.0, r.speed + 5); }, "Speed raised"); break;
			case kVehDown: case kVehUp: {
				const int d = id == kVehUp ? 1 : -1;
				if (!EnsureDraft()) break;
				auto r = g_ed.draft->Value();
				if (!r.schedule.zones.empty()) { r.schedule.peak = std::clamp(r.schedule.peak + d, 0, fvolimits::kVehicles); routeschedule::Expand(r.schedule); }
				else if (r.schedule.allDay) r.schedule.allDayVehicles = std::clamp(r.schedule.allDayVehicles + d, 0, fvolimits::kVehicles);
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
			case kConnect: g_ed.mode = Mode::Connect; Status("Link: click another route's line to join the nearer end of this route to it (the selected end if one is selected)"); break;
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
			default:
				if (id >= kShapeBase && id < kShapeBase + 6) {
					if (id == kShapeBase + 5) { ParallelLanes(); break; }
					if (g_ed.draft) { Status("Save or Cancel the open route first"); break; }
					g_ed.shape = routeeditor::Shape(id - kShapeBase); g_ed.shapeAt = false; g_ed.mode = Mode::Shape;
					Status(std::string(routeeditor::ShapeName(g_ed.shape)) + ": click the map for its centre, then click again for its size");
				}
				else if (id >= kPlaceRow && id < kShapeBase) {
					const int k = id - kPlaceRow, lands = int(g_ed.file.landings.size());
					g_ed.point = -1; g_ed.link = -1;
					if (k < lands) { g_ed.spotKind = 1; g_ed.spot = k; g_ed.zone = -1; }
					else { g_ed.zone = k - lands; g_ed.spot = -1; }
				}
				else if (id >= kLevelBase && id < kPlaceRow) {
					const int w = (id - kLevelBase) / 2, up = (id - kLevelBase) % 2;
					EditDay([](routeschedule::Schedule& s, int a) { auto& l = s.levels[a / 2]; l = std::clamp(l + (a % 2 ? 5 : -5), 0, 100); }, w * 2 + up);
				}
				else if (id >= kDayRow && id < kLevelBase) {
					EditDay([](routeschedule::Schedule& s, int a) {
						const int i = a / 4;
						if (i >= int(s.zones.size())) return;
						auto& z = s.zones[std::size_t(i)];
						switch (a % 4) {
						case 0: case 1: {
							int t = (z.start + (a % 4 ? 30 : routeschedule::kMinutesPerDay - 30)) % routeschedule::kMinutesPerDay;
							for (const auto& o : s.zones) if (&o != &z && o.start == t) return;  // two changes never share a time
							z.start = t;
							break;
						}
						case 2: z.word = (z.word + 1) % routeschedule::kWords; break;
						default: if (s.zones.size() > 1) s.zones.erase(s.zones.begin() + i); break;
						}
					}, id - kDayRow);
				}
				else if (id >= kProblemShow && id < kProblemFix + 500) {  // a problem: show it, or fix it
					trafficdata::Route s2;
					const auto* r = Shown(s2);
					const auto list = r ? Problems(*r, true) : std::vector<Problem>{};
					const int k = id >= kProblemFix ? id - kProblemFix : id - kProblemShow;
					if (k < int(list.size())) {
						if (id >= kProblemFix) FixProblem(list[std::size_t(k)]);
						else { g_ed.point = list[std::size_t(k)].a; g_ed.spot = -1; Status(list[std::size_t(k)].text + ": its first point is selected"); }
					}
				}
				else if (id >= kGroupRow && id < kGroupRow + 100) {
					const int k = id - kGroupRow - 1;
					const auto choosable = ChoosableGroups();
					if (k < 0) { g_ed.ruleScope = 2; RuleTarget(); }
					else if (k < int(choosable.size())) { g_ed.ruleScope = 1; g_ed.ruleGroup = choosable[std::size_t(k)]->name; }
					else break;
					g_ed.sheet = 1; g_ed.typePage = 0; g_types.clear();
				}
				else if (id >= kTypeBase && id < kTypeBase + 1000 && g_ed.ruleScope != 0) {  // the same, for a group or all routes
					const auto& types = VehicleTypesHere();
					const int k = id - kTypeBase;
					auto* target = RuleTarget();
					if (target && k < int(types.size())) {
						const auto& t = types[std::size_t(k)];
						const bool out = TypeIn(target->exclude, t);
						for (const unsigned tid : t.ids) {
							const auto at = std::find(target->exclude.begin(), target->exclude.end(), tid);
							if (out && at != target->exclude.end()) target->exclude.erase(at);
							else if (!out && at == target->exclude.end()) target->exclude.push_back(tid);
						}
						std::sort(target->exclude.begin(), target->exclude.end());
						Status(t.name + (out ? " is allowed again for " : " is left out of ") + (g_ed.ruleScope == 2 ? std::string("all routes") : target->label) + " (Save, then X, to apply it)");
					}
				}
				else if (id >= kTypeBase && id < kTypeBase + 1000) {  // a vehicle type: leave it out, or let it back on
					const auto& types = VehicleTypesHere();
					const int k = id - kTypeBase;
					if (k < int(types.size()) && EnsureDraft()) {
						auto r = g_ed.draft->Value();
						const auto& t = types[std::size_t(k)];
						const bool out = TypeLeftOut(r, t);
						for (const unsigned tid : t.ids) {
							const auto at = std::find(r.exclude.begin(), r.exclude.end(), tid);
							if (out && at != r.exclude.end()) r.exclude.erase(at);
							else if (!out && at == r.exclude.end()) r.exclude.push_back(tid);
						}
						std::sort(r.exclude.begin(), r.exclude.end());
						g_ed.draft->Edit(r);
						Status(t.name + (out ? " may fly this route again" : " is left out of this route") + " (Save to keep it)");
					}
				}
				else if (id >= kRowBase - 1 && id < kViewBase) { g_ed.zone = -1; SelectRow(id - kRowBase); }
				else if (id == kViewBase + 6) {  // the game's own starting angle: yaw 0, pitch 45
					g_orbitYaw = 0; g_orbitPitch = 45; g_orbitOnce = true;
					Status("Map angle reset. Right-drag on the map turns and tilts it.");
				}
				else if (id >= kViewBase && id < kViewBase + 6) {
					auto& v = g_ed.view;
					bool* flags[] = { &v.others, &v.flow, &v.stems, &v.numbers, &v.names, &v.spots };
					static const char* const names[] = { "Other routes", "Vehicles", "Height lines", "Point numbers", "Route names", "Spots and junctions" };
					bool& f = *flags[id - kViewBase];
					f = !f;
					Status(std::string(names[id - kViewBase]) + (f ? " shown" : " hidden"));
				}
				break;
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
		// A grey route of another file joins as the editable copy it would become anyway (nothing is written until Save; Cancel puts it back).
		std::string MakeOriginalMine(int k)
		{
			auto t = g_ed.others[std::size_t(k)].route;
			trafficdata::Vec shift;
			bool anchored = false;
			for (const auto& [name, s] : ShiftsHere()) if (name == t.world) { shift = s; anchored = true; }
			if (!anchored) t.world = EditorKey();
			if (t.label.empty()) t.label = t.name;
			t.file.clear(); t.test = false;
			g_ed.file.routes.push_back(FromTd(t));
			g_ed.shifts[t.name] = shift;
			g_ed.others.erase(g_ed.others.begin() + k);
			if (g_ed.other == k) g_ed.other = -1; else if (g_ed.other > k) --g_ed.other;
			return t.name;
		}
		// The panel shows the one last touched.
		void ShowPick(const Pick& p)
		{
			g_ed.point = -1; g_ed.spot = -1; g_ed.zone = -1; g_ed.link = -1;
			switch (p.kind) {
			case kPickPoint: g_ed.point = p.index; break;
			case kPickLanding: g_ed.spotKind = 1; g_ed.spot = LandingIndex(p.id); break;
			case kPickSpawn: g_ed.spotKind = 2; g_ed.spot = SpawnIndex(p.id); break;
			case kPickZone: g_ed.zone = ZoneIndex(p.id); break;
			case kPickLink: g_ed.link = LinkIndex(p.id); break;
			default: if (!g_ed.draft) { g_ed.route = RouteIndex(p.id); g_ed.other = -1; } break;
			}
		}
		// Ctrl+click: what is under the cursor goes in or out of the selection. What was selected alone joins first. A miss does nothing, so it never adds a point or ends the selection.
		bool PickClick(const routemap::Camera& c, double mx, double my)
		{
			if (g_ed.mode == Mode::Connect || g_ed.mode == Mode::Shape) return false;
			PicksNow();
			Pick hit;
			for (std::size_t i = 0; i < g_ed.file.links.size() && !hit.kind; ++i) {
				trafficroute::Vec at; double x, y;
				if (JunctionAt(g_ed.file.links[i], at) && Local(c, { at.x, at.y, at.z }, x, y) && std::hypot(x - mx, y - kDiamondLift - my) < 10) { hit.kind = kPickLink; hit.id = g_ed.file.links[i].id; }
			}
			trafficdata::Route scratch;
			const auto* shown = Shown(scratch);
			if (!hit.kind && shown && !shown->ring) {
				if (const int p = PointAt(c, mx, my, 16); p >= 0) {
					const auto keep = PrimaryNow()[2];
					if (!EnsureDraft()) return true;
					g_ed.point = keep;
					hit.kind = kPickPoint; hit.id = g_ed.draft->Value().name; hit.index = p;
				}
			}
			for (int kind = 1; kind <= 2 && !hit.kind; ++kind) {
				const auto& list = kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				for (std::size_t i = 0; i < list.size() && !hit.kind; ++i) {
					double x, y;
					if (SpotHere(list[i]) && Local(c, { list[i].at.x, list[i].at.y, list[i].at.z }, x, y) && std::hypot(x - mx, y - my) < 14) { hit.kind = kind == 1 ? kPickLanding : kPickSpawn; hit.id = list[i].id; }
				}
			}
			for (std::size_t i = 0; i < g_ed.file.zones.size() && !hit.kind; ++i) {
				const auto& z = g_ed.file.zones[i];
				double x, y;
				if (!g_ed.unplacedSpots.count(z.name) && Local(c, { z.at.x, z.at.y, z.slow ? z.at.z : z.top }, x, y) && std::hypot(x - mx, y - my) < 14) { hit.kind = kPickZone; hit.id = z.name; }
			}
			auto onLine = [&](const trafficroute::Line& l) {
				const int count = l.Count(), segs = l.closed ? count : count - 1;
				for (int i = 0; i < segs; ++i) {
					const auto a = Middle(l, i), b = Middle(l, i + 1);
					double ax, ay, bx, by;
					if (Local(c, { a.x, a.y, a.z }, ax, ay) && Local(c, { b.x, b.y, b.z }, bx, by) && SegmentDistance(mx, my, ax, ay, bx, by) < 10) return true;
				}
				return false;
			};
			if (!hit.kind && g_ed.draft && onLine(trafficroute::Build(g_ed.draft->Value()))) { hit.kind = kPickRoute; hit.id = g_ed.draft->Value().name; }
			for (std::size_t k = 0; k < g_ed.file.routes.size() && !hit.kind; ++k) {
				const auto& r = g_ed.file.routes[k];
				if (Usable(r) && onLine(trafficroute::Build(ToTd(r, EditorKey())))) { hit.kind = kPickRoute; hit.id = r.id; }
			}
			for (std::size_t k = 0; k < g_ed.others.size() && !hit.kind; ++k) {
				if (onLine(g_ed.others[k].line)) { hit.kind = kPickRoute; hit.id = MakeOriginalMine(int(k)); }
			}
			if (!hit.kind) return true;

			auto& picks = g_ed.picks;
			if (picks.empty()) {
				if (hit.kind == kPickPoint) { if (g_ed.point >= 0) picks.push_back({ kPickPoint, hit.id, g_ed.point }); }
				else if (g_ed.point >= 0 && g_ed.draft) picks.push_back({ kPickPoint, g_ed.draft->Value().name, g_ed.point });
				if (AreaOn()) picks.push_back({ kPickLanding, g_ed.file.landings[std::size_t(g_ed.spot)].id });
				else if (g_ed.spot >= 0 && g_ed.spot < int(g_ed.file.spawns.size()) && g_ed.spotKind == 2) picks.push_back({ kPickSpawn, g_ed.file.spawns[std::size_t(g_ed.spot)].id });
				if (ZoneOn()) picks.push_back({ kPickZone, g_ed.file.zones[std::size_t(g_ed.zone)].name });
				if (g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size())) picks.push_back({ kPickLink, g_ed.file.links[std::size_t(g_ed.link)].id });
				if (hit.kind == kPickRoute && picks.empty()) {
					if (g_ed.draft) picks.push_back({ kPickRoute, g_ed.draft->Value().name });
					else if (g_ed.route >= 0 && g_ed.route < int(g_ed.file.routes.size())) picks.push_back({ kPickRoute, g_ed.file.routes[std::size_t(g_ed.route)].id });
					else if (g_ed.other >= 0 && g_ed.other < int(g_ed.others.size())) picks.push_back({ kPickRoute, MakeOriginalMine(g_ed.other) });
				}
			}
			const auto at = std::find_if(picks.begin(), picks.end(), [&](const Pick& p) { return p.kind == hit.kind && p.index == hit.index && p.id == hit.id; });
			if (at != picks.end()) picks.erase(at); else picks.push_back(hit);
			if (picks.empty()) ShowPick(Pick{});
			else ShowPick(picks.back());
			g_ed.pickPrimary = PrimaryNow();
			g_ed.picksCount = g_ed.draft ? int(g_ed.draft->Value().points.size()) : 0;
			Status(picks.size() >= 2 ? PicksWords() + ". Ctrl+click adds or removes one; a plain click ends it." : std::to_string(picks.size()) + " selected. Ctrl+click more to change them together.");
			return true;
		}
		void ClickMap(const routemap::Camera& c, double mx, double my)
		{
			g_ed.picks.clear();  // a plain click ends a selection
			switch (g_ed.mode) {
			case Mode::Drop:
				// A click on a point selects it, as in Select: Drop stays on after a route is started, so this added a second point on top. Insert still drops anywhere.
				if (const int p = PointAt(c, mx, my, 14); p >= 0) {
					g_ed.point = p; g_ed.spot = -1;
					trafficdata::Route scratch;
					const auto& q = Shown(scratch)->points[std::size_t(p)];
					char b[96]; std::snprintf(b, sizeof b, "Point %d: (%.0f, %.0f), height %.0f m", p + 1, q.x, q.y, q.z);
					Status(b); return;
				}
				DropPoint(c); return;
			case Mode::Landing: ClickLand(c, mx, my); return;
			case Mode::Zone: ClickZone(c, mx, my); return;
			case Mode::Shape: PlaceShape(c); return;
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
			g_ed.zone = -1;
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
						if (kind == 1) { ClickedPlace(c, mx, my); return; }
						g_ed.spotKind = kind; g_ed.spot = int(i); g_ed.point = -1;
						Status(std::string(kind == 1 ? "Landing" : "Spawn") + " spot " + list[i].id + ": Z up / Z down set its height, Del point removes it");
						return;
					}
				}
			}
			if (ClickedPlace(c, mx, my)) return;  // a zone's middle
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

		// The Route tool's turn handle: 70 px above the route's centre on screen (while turning: toward the cursor).
		bool TurnHandle(const routemap::Camera& c, const routeeditor::Point& centre, double& hx, double& hy, double towardX = 0, double towardY = 0, bool toward = false)
		{
			double sx = 0, sy = 0;
			if (!Local(c, { centre.x, centre.y, centre.z }, sx, sy)) return false;
			double dx = 0, dy = -1;
			if (toward) { const double l = std::hypot(towardX - sx, towardY - sy); if (l > 1) { dx = (towardX - sx) / l; dy = (towardY - sy) / l; } }
			hx = sx + 70 * dx; hy = sy + 70 * dy;
			return true;
		}
		bool NearRouteHandle(const routemap::Camera& c, const trafficdata::Route& r, double mx, double my)
		{
			const auto centre = routeeditor::Centre(r);
			double sx = 0, sy = 0, hx = 0, hy = 0;
			if (Local(c, { centre.x, centre.y, centre.z }, sx, sy) && std::hypot(sx - mx, sy - my) < 90) return true;
			return !r.ring && TurnHandle(c, centre, hx, hy) && std::hypot(hx - mx, hy - my) < 30;
		}
		void StartRouteDrag(const routemap::Camera& c, double mx, double my)
		{
			trafficdata::Route scratch;
			if (!Shown(scratch)) { Status("Select a route first: click it, or pick it in the list"); return; }
			if (!EnsureDraft()) return;
			auto& rd = g_ed.routeDrag;
			rd.base = g_ed.draft->Value(); rd.preview = rd.base; rd.centre = routeeditor::Centre(rd.base); rd.turned = 0;
			routemap::Vec3 at;
			if (!routemap::MouseAt(c, rd.centre.z, at)) { Status("The cursor can't be placed on the map there"); return; }
			double hx = 0, hy = 0;
			const bool handle = !rd.base.ring && TurnHandle(c, rd.centre, hx, hy) && std::hypot(hx - mx, hy - my) < 16;
			rd.kind = handle ? 2 : 1;
			rd.start = at;
			rd.a0 = std::atan2(at.y - rd.centre.y, at.x - rd.centre.x);
			Status(handle ? "Turning the route: let go to keep it (it snaps to 5 deg)" : "Moving the route: let go to keep it");
		}

		// ---- Everything placed moves in 3D: press its ball and drag; Alt while dragging moves it straight up and down ----
		bool StartPlaceDrag(const routemap::Camera& c, double mx, double my)
		{
			if (g_ed.mode != Mode::Drag && g_ed.mode != Mode::Landing && g_ed.mode != Mode::Spawn && g_ed.mode != Mode::Zone) return false;
			auto begin = [&](int kind, int index, const trafficdata::Vec& at) {
				g_ed.placeKind = kind; g_ed.placeIndex = index; g_ed.placeAt = { at.x, at.y, at.z }; g_ed.placeFrom = g_ed.placeAt;
				g_ed.placeAlt = false; g_ed.placeMoved = false;
				const std::string name = kind == 3 ? g_ed.file.zones[std::size_t(index)].name : (kind == 1 ? g_ed.file.landings : g_ed.file.spawns)[std::size_t(index)].id;
				const bool group = Many() && IsPicked(kind == 3 ? kPickZone : kind == 1 ? kPickLanding : kPickSpawn, name);  // asked before the fields change
				g_ed.point = -1; g_ed.link = -1;
				if (kind == 3) { g_ed.zone = index; g_ed.spot = -1; }
				else { g_ed.spotKind = kind; g_ed.spot = index; g_ed.zone = -1; }
				if (group) { g_ed.pickPrimary = PrimaryNow(); GroupBegin(); g_ed.groupDrag = 2; g_ed.groupLast = g_ed.placeAt; }
				return true;
			};
			for (int kind = 1; kind <= 2; ++kind) {
				const auto& list = kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				for (std::size_t i = 0; i < list.size(); ++i) {
					double x, y;
					if (SpotHere(list[i]) && Local(c, { list[i].at.x, list[i].at.y, list[i].at.z }, x, y) && std::hypot(x - mx, y - my) < 14) return begin(kind, int(i), list[i].at);
				}
			}
			for (std::size_t i = 0; i < g_ed.file.zones.size(); ++i) {
				const auto& z = g_ed.file.zones[i];
				const trafficdata::Vec at{ z.at.x, z.at.y, z.slow ? z.at.z : z.top };
				double x, y;
				if (!g_ed.unplacedSpots.count(z.name) && Local(c, { at.x, at.y, at.z }, x, y) && std::hypot(x - mx, y - my) < 14) return begin(3, int(i), at);
			}
			return false;
		}
		// Puts the dragged thing at (x, y, z).
		void SetPlace(const routemap::Vec3& p)
		{
			if (g_ed.placeKind == 3) {
				if (g_ed.placeIndex >= int(g_ed.file.zones.size())) return;
				auto& z = g_ed.file.zones[std::size_t(g_ed.placeIndex)];
				z.at.x = p.x; z.at.y = p.y;
				if (z.slow) z.at.z = p.z; else z.top = p.z;
				return;
			}
			auto& list = g_ed.placeKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
			if (g_ed.placeIndex < int(list.size())) list[std::size_t(g_ed.placeIndex)].at = { p.x, p.y, p.z };
		}
		// ---- The Alt key hub (2026-10-30, the user: WASD as a movement controller, without moving the map) ----
		// The left hand stays on WASD. Alt: W S A D across, Q E down and up, Z C turn, R F size a route. Alt+Shift: W S tilt the map, A D turn it, the mouse does both.
		// Moves what is selected: a zone, a spot, a route's point, else the whole route. The map's own keys are held while Alt is down.
		void HubMove(double dx, double dy, double dz)
		{
			if (Many()) {
				if (!g_ed.hubOn) { g_ed.hubOn = true; g_ed.hubKind = 4; GroupBegin(); }
				routeeditor::GroupMove m; m.dx = dx; m.dy = dy; m.dz = dz; GroupLive(m);
				return;
			}
			if (ZoneOn()) {
				auto& z = g_ed.file.zones[std::size_t(g_ed.zone)];
				if (!g_ed.hubOn) { g_ed.hubOn = true; g_ed.hubKind = 3; }
				z.at.x += dx; z.at.y += dy;
				if (z.slow) z.at.z += dz; else z.top += dz;
				return;
			}
			if (g_ed.spot >= 0) {
				auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				if (g_ed.spot >= int(list.size())) return;
				auto& at = list[std::size_t(g_ed.spot)].at;
				if (!g_ed.hubOn) { g_ed.hubOn = true; g_ed.hubKind = 2; g_ed.hubFrom = at; }
				at.x += dx; at.y += dy; at.z += dz;
				return;
			}
			if (!g_ed.hubOn) {
				trafficdata::Route scratch;
				if (!Shown(scratch)) { Status("Select something to move first"); return; }
				if (!EnsureDraft()) return;
				g_ed.draft->Edit(g_ed.draft->Value());  // the undo step for this whole burst
				g_ed.hubOn = true; g_ed.hubKind = 1;
			}
			if (!g_ed.draft) return;
			auto r = g_ed.draft->Value();
			if (g_ed.point >= 0 && g_ed.point < int(r.points.size())) { auto& q = r.points[std::size_t(g_ed.point)]; q.x += dx; q.y += dy; q.z += dz; }
			else r = routeeditor::Translated(r, dx, dy, dz);
			g_ed.draft->Amend(r);
		}
		// The keys are let go: the ground and roof checks run once, on where it ended up.
		void HubEnd()
		{
			if (!g_ed.hubOn) return;
			g_ed.hubOn = false;
			if (g_ed.hubKind == 4) { if (GroupEnd("Move")) Status(PicksWords() + ": moved (Save keeps it)"); return; }
			char b[96];
			if (g_ed.hubKind == 3) {
				if (ZoneOn()) { const auto& z = g_ed.file.zones[std::size_t(g_ed.zone)]; std::snprintf(b, sizeof b, "Zone at (%.0f, %.0f), %s (Save keeps it)", z.at.x, z.at.y, ZoneWords(z).c_str()); Status(b); }
				return;
			}
			if (g_ed.hubKind == 2) {
				auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				if (g_ed.spot < 0 || g_ed.spot >= int(list.size())) return;
				auto& at = list[std::size_t(g_ed.spot)].at;
				if (const char* why = PointProblem(at.x, at.y, at.z)) { Refused("Spot move", at.x, at.y, at.z, why); at = g_ed.hubFrom; return; }
				std::snprintf(b, sizeof b, "Spot at (%.0f, %.0f), height %.0f m (Save keeps it)", at.x, at.y, at.z);
				Status(b);
				return;
			}
			if (!g_ed.draft) return;
			const auto& r = g_ed.draft->Value();
			if (g_ed.point >= 0 && g_ed.point < int(r.points.size())) {
				const auto q = r.points[std::size_t(g_ed.point)];
				if (const char* why = PointProblem(q.x, q.y, q.z)) { Refused("Point move", q.x, q.y, q.z, why); g_ed.draft->Undo(); return; }
				std::snprintf(b, sizeof b, "Point %d at (%.0f, %.0f), height %.0f m", g_ed.point + 1, q.x, q.y, q.z);
				Status(b);
			}
			else if (!RouteFits(r, "Route move")) g_ed.draft->Undo();
			else Status("Route moved. Every point kept its place on the route. Save to keep it.");
		}
		void HubKeys(const routemap::Camera& c, bool fine, ULONGLONG now)
		{
			enum : std::uint32_t { kW = 1, kS = 2, kA = 4, kD = 8, kQ = 16, kE = 32 };
			auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
			const std::uint32_t keys = (down('W') ? kW : 0u) | (down('S') ? kS : 0u) | (down('A') ? kA : 0u) | (down('D') ? kD : 0u) | (down('Q') ? kQ : 0u) | (down('E') ? kE : 0u);
			const std::uint32_t fresh = keys & ~g_ed.hubWas;
			if (!g_ed.hubWas && keys) g_ed.hubSince = now;
			const double dt = g_ed.hubAt ? std::min<ULONGLONG>(now - g_ed.hubAt, 100) / 1000.0 : 0.0;
			g_ed.hubAt = now;
			g_ed.hubWas = keys;
			if (!keys) { HubEnd(); return; }
			auto axis = [&](std::uint32_t mask, std::uint32_t plus, std::uint32_t minus) { return ((mask & plus) ? 1.0 : 0.0) - ((mask & minus) ? 1.0 : 0.0); };
			const double travel = routeeditor::HubSpeed((now - g_ed.hubSince) / 1000.0, fine) * dt, tap = routeeditor::HubTap(fine);
			const double side = axis(fresh, kD, kA) * tap + axis(keys, kD, kA) * travel;
			const double ahead = axis(fresh, kW, kS) * tap + axis(keys, kW, kS) * travel;
			const double up = axis(fresh, kE, kQ) * tap + axis(keys, kE, kQ) * travel;
			if (side == 0 && ahead == 0 && up == 0) return;
			// Along the view, so W is away on screen whichever way the map is turned.
			double fx = c.focus.x - c.eye.x, fy = c.focus.y - c.eye.y;
			const double l = std::hypot(fx, fy);
			if (l < 1e-3) { fx = 0; fy = 1; } else { fx /= l; fy /= l; }
			HubMove(side * fy + ahead * fx, -side * fx + ahead * fy, up);
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
		// Each route's vehicles (2026-10-30, the user: "representative", speed included): its real count, shared between its lanes, at its real speed on the map's scale, the second
		// lane the other way. A preview worked out from the route: real traffic stands still while the map pauses the game.
		constexpr double kHeightLevel = 25.0;   // a controller's height step, m
		void DrawFlow(Pen& pen, const routemap::Camera& c, const trafficroute::Line& line, int lanes, double speed, unsigned color, int vehicles)
		{
			const double length = trafficroute::Length(line), span = line.Span();
			if (length < 1 || span <= 0 || vehicles <= 0 || lanes < 1) return;
			const double run = std::fmod(g_ed.phase * speed, length) / length * span, step = 3.0 / length * span;
			pen.Style(1, 0x07110F, 0.9);
			pen.Fill(color, 1);
			for (int k = 0; k < vehicles; ++k) {
				const int lane = k % lanes;
				const double dir = lane ? -1.0 : 1.0;
				const double u = std::fmod(std::fmod(span * (double(k) / vehicles) + dir * run, span) + span, span);
				const auto p = trafficroute::At(line, lane, u);
				const auto q = trafficroute::At(line, lane, std::fmod(u + dir * step + span, span));
				double x, y, ax, ay;
				if (!Local(c, { p.x, p.y, p.z }, x, y) || !Local(c, { q.x, q.y, q.z }, ax, ay)) continue;
				double dx = ax - x, dy = ay - y;
				const double l = std::hypot(dx, dy);
				if (l < 1e-6) { dx = 1; dy = 0; } else { dx /= l; dy /= l; }
				pen.Move(x + dx * 8, y + dy * 8);
				pen.Line(x - dx * 5 - dy * 5, y - dy * 5 + dx * 5);
				pen.Line(x - dx * 2.5, y - dy * 2.5);
				pen.Line(x - dx * 5 + dy * 5, y - dy * 5 - dx * 5);
				pen.Line(x + dx * 8, y + dy * 8);
			}
			pen.End();
			pen.NoLine();
		}
		// The route's name beside its start.
		void RouteName(uptr movie, const routemap::Camera& c, const trafficroute::Line& line, const std::string& name, unsigned color)
		{
			if (line.Count() < 1) return;
			const auto p = trafficroute::At(line, 0, 0);
			double x, y;
			if (Local(c, { p.x, p.y, p.z }, x, y)) Text(movie, name, x + 12, y - 30, color, 240, true);
		}
		// The ground under a point, where it is known.
		bool GroundUnder(double x, double y, double z, double& ground)
		{
			const auto m = navmap::At(x, y, z);
			if (m.haveGround && std::isfinite(m.ground)) { ground = m.ground; return true; }
			if (cellbake::Ground(x, y, ground) && std::isfinite(ground)) return true;
			return CachedTerrain(x, y, ground);
		}

		// ---- What is under a point (2026-10-30, the user: "when they drop a line down ... it should be to the bottom of the 3d object
		// not to the very bottom of the map") ----
		// A ray straight down from the point finds the first surface under it (a roof, a bridge, the ground); a ray from high above
		// down to the point tells whether something covers it. Rays run where the world is loaded; elsewhere the nav map, baked
		// terrain and the game's terrain answer. Results are kept per position (to 0.25 m), so a point costs rays only when it moves; at most
		// kSurfaceRaysPerFrame new lookups a frame, and a lookup without a ray hit is tried again after a second (a ray that misses may only mean nothing is loaded there yet).
		constexpr int kSurfaceRaysPerFrame = 6;
		std::map<std::tuple<long long, long long, long long>, Surface> g_surfaces;
		int g_surfaceBudget = kSurfaceRaysPerFrame;
		const Surface& SurfaceUnder(double x, double y, double z)
		{
			static const Surface none;
			if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return none;
			const auto key = std::make_tuple(std::llround(x * 4), std::llround(y * 4), std::llround(z * 4));
			const ULONGLONG now = GetTickCount64();
			auto it = g_surfaces.find(key);
			if (it != g_surfaces.end() && (it->second.rays || now - it->second.at < 1000 || g_surfaceBudget <= 0)) return it->second;
			if (g_surfaces.size() > 4000) { g_surfaces.clear(); it = g_surfaces.end(); }
			Surface s; s.at = now;
			if (g_surfaceBudget > 0) {
				--g_surfaceBudget;
				double hit = 0;
				const int below = DownRay(x, y, z, z - 3000.0, hit);
				if (below >= 0) {
					if (below == 1) { s.below = hit; s.known = s.rays = true; }
					double top = 0;
					if (DownRay(x, y, z + 1500.0, z + 0.5, top) == 1) {
						s.covered = below == 1; s.buried = below == 0; s.above = top;
						if (s.buried) { s.below = top; s.known = s.rays = true; }  // the line goes up to the surface it is under
					}
					// Is the game's terrain height the same thing a ray sees? Plain ground should agree; a roof will not.
					static int compared = 0;
					if (below == 1 && !s.covered && compared < 12) {
						if (double g = 0; CachedTerrain(x, y, g)) { ++compared; logger::Write("Route editor: terrain height check at (%.0f, %.0f): ray %.1f, game's terrain %.1f", x, y, hit, g); }
					}
				}
			}
			if (!s.known) {
				double g = 0;
				if (GroundUnder(x, y, z, g) && g <= z + 0.5) { s.below = g; s.known = s.rough = true; }
				else if (CachedTerrain(x, y, g)) {
					s.below = g; s.known = s.rough = true;
					if (g > z + 0.5) { s.buried = true; s.above = g; }
				}
				static int reported = 0;
				if (reported < 12) { ++reported; logger::Write("Route editor: no ray hit under (%.0f, %.0f, %.0f): %s %.1f", x, y, z, s.known ? (s.buried ? "under the game's terrain at" : "ground from map data at") : "ground unknown,", s.below); }
			}
			return g_surfaces[key] = s;
		}

		// A route point drawn as a small lit ball (2026-10-30, the user: "the points don't have 3D shapes"): sized by its distance from the
		// camera, on a stem down to the surface under it, with a footprint on that surface and a tick every 25 m of height. The stem is
		// amber when something covers the point (a roof or a bridge) and red when the point is inside the ground.
		struct Ball { unsigned light, dark; };
		double BallRadius(const routemap::Camera& c, const routemap::Vec3& p, bool big)
		{
			const double focus = std::hypot(c.focus.x - c.eye.x, c.focus.y - c.eye.y, c.focus.z - c.eye.z);
			const double d = std::hypot(p.x - c.eye.x, p.y - c.eye.y, p.z - c.eye.z);
			const double r = d > 1 && focus > 1 ? 6.5 * focus / d : 6.5;
			return std::clamp(r, 3.5, 11.0) + (big ? 2.5 : 0.0);
		}
		void DrawBall(Pen& pen, double x, double y, double r, const Ball& b, double alpha)
		{
			pen.Style(1.5, 0x0B1620, 0.9 * alpha); pen.Fill(b.dark, alpha); pen.Circle(x, y, r); pen.End();
			pen.NoLine();
			pen.Fill(b.light, alpha); pen.Circle(x - r * 0.18, y - r * 0.18, r * 0.72); pen.End();
			pen.Fill(0xFFFFFF, 0.85 * alpha); pen.Circle(x - r * 0.38, y - r * 0.38, std::max(1.2, r * 0.26)); pen.End();
		}
		// The stem, its 25 m ticks and the footprint; returns the surface it reached (or nullptr).
		const Surface* DrawStem(Pen& pen, const routemap::Camera& c, const routemap::Vec3& p, double x, double y, double r, bool selected, bool rays)
		{
			Surface fallback;
			const Surface* s = nullptr;
			if (rays) s = &SurfaceUnder(p.x, p.y, p.z);
			else if (double g = 0; GroundUnder(p.x, p.y, p.z, g) && g <= p.z + 0.5) { fallback.below = g; fallback.known = true; s = &fallback; }
			if (!s || !s->known) return rays ? s : nullptr;
			double fx = 0, fy = 0;
			if (!Local(c, { p.x, p.y, s->below }, fx, fy)) return rays ? s : nullptr;
			const unsigned color = s->buried ? kCoral : s->covered ? kAmber : 0xFFFFFF;
			pen.Style(selected ? 1.6 : 1, color, s->buried || s->covered ? 0.95 : 0.55); pen.Move(x, y); pen.Line(fx, fy);
			// a tick every 25 m of height between the surface and the point
			for (double h = std::ceil((s->below + 0.01) / kHeightLevel) * kHeightLevel; h < p.z - 0.01; h += kHeightLevel) {
				double tx = 0, ty = 0;
				if (!Local(c, { p.x, p.y, h }, tx, ty)) continue;
				pen.Style(1, color, 0.7); pen.Move(tx - 4, ty); pen.Line(tx + 4, ty);
			}
			// the footprint: a circle on that surface, as wide in metres as the ball is in pixels (so it looks the same size)
			double ex = 0, ey = 0;
			if (Local(c, { p.x + 1.0, p.y, s->below }, ex, ey)) {
				const double ppm = std::max(0.02, std::hypot(ex - fx, ey - fy));
				const double R = std::clamp(r * 1.1 / ppm, 0.5, 250.0);
				pen.Style(1, color, 0.75); pen.Fill(color, 0.18);
				bool first = true;
				for (int k = 0; k <= 16; ++k) {
					const double a = k * 2.0 * 3.14159265358979323846 / 16;
					double qx = 0, qy = 0;
					if (!Local(c, { p.x + R * std::cos(a), p.y + R * std::sin(a), s->below }, qx, qy)) continue;
					if (first) pen.Move(qx, qy); else pen.Line(qx, qy);
					first = false;
				}
				pen.End();
			}
			pen.NoLine();
			return rays ? s : nullptr;
		}

		// "  (12 m up)", with what it stands on when that is not plain ground: every placed thing says it the same way.
		std::string UpWords(const Surface* s, double z)
		{
			if (!s) return {};
			if (!s->known) return "  (ground unknown here)";
			char b[48];
			if (s->buried) std::snprintf(b, sizeof b, "  (%.0f m inside the ground)", s->below - z);
			else std::snprintf(b, sizeof b, s->covered ? "  (%.0f m up, under a roof)" : s->rough ? "  (about %.0f m up)" : "  (%.0f m up)", z - s->below);
			return b;
		}
		// ---- Each kind of placed thing has its own 3D look (2026-10-30, the user; the ball stays a route's point): a landing area is a pad, a spawn spot a cone,
		// a junction a diamond, a centre or a zone's handle a flat ring. Real solids in world coordinates, sized by the same pixels as a ball, so they turn with the map. ----
		double MetresPerPixel(const routemap::Camera& c, const routemap::Vec3& p)
		{
			double x = 0, y = 0, ax = 0, ay = 0, bx = 0, by = 0;
			if (!Local(c, p, x, y) || !Local(c, { p.x + 1, p.y, p.z }, ax, ay) || !Local(c, { p.x, p.y + 1, p.z }, bx, by)) return 1.0;
			return 1.0 / std::max(0.02, std::max(std::hypot(ax - x, ay - y), std::hypot(bx - x, by - y)));
		}
		unsigned Mix(unsigned dark, unsigned light, double t)
		{
			t = std::clamp(t, 0.0, 1.0);
			auto part = [&](int shift) { const double a = (dark >> shift) & 0xFF, b = (light >> shift) & 0xFF; return unsigned(a + (b - a) * t) << shift; };
			return part(16) | part(8) | part(0);
		}
		// lift: pixels up the screen (a junction's diamond floats above its stem).
		void DrawSolid(Pen& pen, const routemap::Camera& c, const routemap::Solid& solid, const routemap::Vec3& centre, const Ball& b, double alpha, double lift = 0)
		{
			for (const auto& lit : routemap::Visible(solid, centre, c.eye)) {
				pen.Style(1, 0x0B1620, 0.75 * alpha); pen.Fill(Mix(b.dark, b.light, lit.light), alpha);
				bool first = true;
				double fx = 0, fy = 0;
				for (const auto& v : *lit.face) {
					double x = 0, y = 0;
					if (!Local(c, v, x, y)) continue;
					y -= lift;
					if (first) { pen.Move(x, y); fx = x; fy = y; } else pen.Line(x, y);
					first = false;
				}
				if (!first) pen.Line(fx, fy);
				pen.End();
			}
			pen.NoLine();
		}
		void DrawPadMark(Pen& pen, const routemap::Camera& c, const routemap::Vec3& p, double pixels, double turnDegrees, const Ball& b, double alpha)
		{
			const double half = pixels * 1.3 * MetresPerPixel(c, p), t = turnDegrees * kDeg, ax = std::sin(t), ay = std::cos(t);
			DrawSolid(pen, c, routemap::Pad(p, half, half * 0.35, t), { p.x, p.y, p.z - half * 0.17 }, b, alpha);
			// The arrow on top: which way the area is turned.
			double x0, y0, x1, y1, x2, y2;
			const auto on = [&](double along, double across) { return routemap::Vec3{ p.x + along * ax + across * ay, p.y + along * ay - across * ax, p.z }; };
			if (Local(c, on(-half * 0.3, -half * 0.55), x0, y0) && Local(c, on(half * 0.6, 0), x1, y1) && Local(c, on(-half * 0.3, half * 0.55), x2, y2)) {
				pen.Style(2, 0x173404, alpha); pen.Move(x0, y0); pen.Line(x1, y1); pen.Line(x2, y2); pen.NoLine();
			}
		}
		void DrawConeMark(Pen& pen, const routemap::Camera& c, const routemap::Vec3& p, double pixels, const Ball& b, double alpha)
		{
			const double m = pixels * MetresPerPixel(c, p), height = m * 2.6, below = height * 0.35;
			DrawSolid(pen, c, routemap::Cone(p, m, height, below), { p.x, p.y, p.z - below + height * 0.3 }, b, alpha);
		}
		void DrawRingMark(Pen& pen, const routemap::Camera& c, const routemap::Vec3& p, double pixels, unsigned color, double alpha)
		{
			const double r = pixels * 1.5 * MetresPerPixel(c, p);
			pen.Style(3, color, alpha);
			bool first = true;
			for (int k = 0; k <= 24; ++k) {
				const double a = k * 3.14159265358979323846 / 12;
				double x = 0, y = 0;
				if (!Local(c, { p.x + r * std::cos(a), p.y + r * std::sin(a), p.z }, x, y)) continue;
				if (first) pen.Move(x, y); else pen.Line(x, y);
				first = false;
			}
			pen.NoLine();
			double x = 0, y = 0;
			if (Local(c, p, x, y)) { pen.Fill(color, alpha); pen.Circle(x, y, 3); pen.End(); }
		}
		// Can a panel of our own be a real SWF? (settings.ini [routeedit] swfTest=1, 2026-10-30, the user's option 1.) Two small red squares, one per path tried, loaded into the
		// map's movie by the same Flash calls a panel would use; 3 s later the log says what arrived. Off unless asked for.
		struct SwfTest { Value loader[2]; bool started = false, told = false; ULONGLONG at = 0; } g_swf;
		void SwfTestTick(uptr movie)
		{
			static int asked = -1;
			if (asked < 0) asked = GetPrivateProfileIntW(L"routeedit", L"swfTest", 0, fvopaths::Settings().c_str());
			if (!asked || g_swf.told) return;
			const char* const paths[2] = { "FVO_UITest.swf", "Interface/FVO_UITest.swf" };
			if (!g_swf.started) {
				g_swf.started = true; g_swf.at = GetTickCount64();
				for (int i = 0; i < 2; ++i) {
					Value request;
					const Value url[] = { Str(paths[i]) };
					if (!CreateObject(movie, &request, "flash.net.URLRequest", url, 1)) { logger::Write("Route editor: SWF test: no URLRequest"); return; }
					if (!CreateObject(movie, &g_swf.loader[i], "flash.display.Loader", nullptr, 0)) { logger::Write("Route editor: SWF test: no Loader"); Release(request); return; }
					SetMember(g_swf.loader[i], "x", Num(520.0 + i * 140.0)); SetMember(g_swf.loader[i], "y", Num(120.0));
					const Value child[] = { Arg(g_swf.loader[i]) };
					const bool added = Invoke(g_ui.layer, "addChild", child, 1);
					const Value request1[] = { Arg(request) };
					const bool loading = Invoke(g_swf.loader[i], "load", request1, 1);
					logger::Write("Route editor: SWF test: %s: added %d, load called %d", paths[i], added ? 1 : 0, loading ? 1 : 0);
					Release(request);
				}
				return;
			}
			if (GetTickCount64() - g_swf.at < 3000) return;
			g_swf.told = true;
			for (int i = 0; i < 2; ++i) {
				double got = -1, total = -1;
				Value info, content;
				if (IsObject(g_swf.loader[i]) && GetMember(g_swf.loader[i], "contentLoaderInfo", &info) && IsObject(info)) { GetNumber(info, "bytesLoaded", got); GetNumber(info, "bytesTotal", total); }
				const bool has = IsObject(g_swf.loader[i]) && GetMember(g_swf.loader[i], "content", &content) && IsObject(content);
				logger::Write("Route editor: SWF test: %s: %.0f of %.0f bytes, content %s", paths[i], got, total, has ? "LOADED" : "none");
				Release(info); Release(content);
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
		void ShowReadme()
		{
			g_ed.menu = 0;
			if (!ShowGameMessage("Traffic Manager - Readme",
				"Press F7 on the local map to open or close the Traffic Manager. Select a route on the map or in the route list.\n\n"
				"Use New route to make a route, then the point and shape tools to draw its path. Select an existing route to edit it. Set its height, speed, lanes and vehicle count; use Time rules for its schedule.\n\n"
				"Save (Ctrl+S) keeps your changes. Cancel discards unsaved edits. Use Test route to check clearance: buildings and terrain are not checked automatically.\n\n"
				"City controls local traffic and density. Reset traffic replaces the current traffic; finish or cancel your route edits first, and leave combat before resetting.\n\n"
				"Routes, groups and traffic settings belong to this playthrough. Other playthroughs start with the installed defaults.\n\n"
				"Autopilot near a traffic zone follows its path. Reopen this Readme whenever you need it."))
				Status("Could not open the Readme. Close and reopen the local map, then try again.");
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
			static std::uint32_t padWas = 0;
			const std::uint32_t pad = focused ? bindings::PadState() : 0, freshPad = pad & ~padWas;
			padWas = pad;
			if (!open) { Hide(); g_ed.dragging = false; g_mapOpenFrames = 0; g_ed.orbiting = false; g_orbitOn = false; return; }
			// The Traffic Manager comes with the Traffic Addon (2026-10-30, the user): without it no traffic would fly these routes, so no F7, no hint and no first-time
			// message. The message's flag stays unset, so someone who adds the addon later still gets it.
			if (!trafficpool::AddonAttached()) { Hide(); g_ed.dragging = false; return; }
			const auto profile = TrafficDir();
			if (profile.empty()) { Hide(); return; }
			if (g_ed.loaded && g_ed.profileDir != profile) { g_ed = Editor{}; g_mapOpenFrames = 0; }
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
			if (haveMouse && !g_ed.padCursor) Calibrate(c, lx, ly);
			if ((g_keys.Pressed(VK_F7, focused) || (freshPad & bindlogic::kRS)) && !g_ed.loading) {  // right stick click: open and close on a pad
				if (g_ed.on) { g_ed.on = false; g_ed.dragging = false; g_ed.naming = false; logger::Write("Route editor: closed%s", g_ed.draft ? " (the open route is kept until you save or cancel)" : ""); }
				else Open();
			}
			g_ui.items.clear();
			Pen lines(g_ui.lines), panel(g_ui.panel), cover(g_ui.cover);
			lines.Clear(); panel.Clear(); cover.Clear();
			if (!g_ed.on) {
				g_ed.orbiting = false; g_orbitOn = false;
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
					Status("Traffic is still filling toward your rules; remaining positions continue when ready and out of sight");
				}
				else if (!st.active && now - g_ed.loadingAt > 800 && !settling) {
					g_ed.loading = false;
					const bool ran = secretunpause::Active();
					secretunpause::End("traffic armed");
					if (st.total == 0) {
						Status("No traffic spots are known in this world yet: add landing spots, close the map once (traffic reads them), then press Reset traffic again");
						goto loaded;
					}
					char b[260];
					std::snprintf(b, sizeof b, "Traffic reset: every vehicle cleared, %d of %d route positions filled from new%s. %s",
						st.placed, st.total, st.skipped ? (", " + std::to_string(st.skipped) + " blocked: check route positions or creation failures").c_str() : "",
						ran ? "Pilots start as models become ready." : "Pilots start when the world runs and models are ready.");
					Status(b);
				loaded:;
				}
				else {
					const double w = 560, h = 150, x = (g_ui.width - w) / 2, y = (g_ui.height - h) / 2;
					panel.NoLine(); panel.Fill(0x0B1620, 0.92); panel.Rect(x, y, w, h); panel.End();
					panel.Style(2, 0x4FE3FF, 1); panel.Move(x, y); panel.Line(x + w, y); panel.Line(x + w, y + h); panel.Line(x, y + h); panel.Line(x, y); panel.NoLine();
					const int dots = int(g_ed.phase * 3) % 4;
					Text(movie, std::string("RESETTING TRAFFIC") + std::string(std::size_t(dots), '.'), x + 24, y + 22, 0xFFFFFF, w - 48);
					char b[160];
					// Start/Swap cold start: every scheduled route position gets its vehicle under this panel (2026-10-29).
					if (g_ed.filledAt) std::snprintf(b, sizeof b, "Traffic taking off: %d vehicle(s) waiting for their pilots", vehiclediag::StagedLoaded());
					else std::snprintf(b, sizeof b, "Clearing every vehicle and placing new ones: %d of %d positions", st.placed, st.total);
					Text(movie, b, x + 24, y + 54, 0xD8E4EC, w - 48);
					std::snprintf(b, sizeof b, "Placed now %d, deferred %d", st.made, st.skipped);
					Text(movie, b, x + 24, y + 76, 0x9AA4AE, w - 48);
					const double f = st.total > 0 ? std::clamp(double(st.placed) / st.total, 0.0, 1.0) : 0.0;
					panel.Fill(0x22384A, 1); panel.Rect(x + 24, y + 104, w - 48, 14); panel.End();
					panel.Fill(0x4FE3FF, 1); panel.Rect(x + 24, y + 104, (w - 48) * f, 14); panel.End();
					Text(movie, "Blocked positions remain part of your target; check the result count", x + 24, y + 124, 0x9AA4AE, w - 48);
					FlushText();
					return;
				}
			}

			// The pad cursor (2026-10-30, Traffic Manager V1): the left stick moves it and A is the left button, so every click and drag works
			// the same. It stands in for the mouse from here on; moving the mouse hands back.
			if (focused) {
				double sx = 0, sy = 0;
				bindings::PadLeftStick(sx, sy);
				const double dt = g_ed.padAt ? std::min<ULONGLONG>(now - g_ed.padAt, 100) / 1000.0 : 0.0;
				g_ed.padAt = now;
				if (std::hypot(sx, sy) > 0.2) {
					if (!g_ed.padCursor) { g_ed.padCursor = true; g_ed.padX = lx; g_ed.padY = ly; }
					const double speed = 150.0 + 750.0 * std::hypot(sx, sy);  // slow for a point, fast across the map
					g_ed.padX = std::clamp(g_ed.padX + sx * speed * dt, 0.0, g_ui.width);
					g_ed.padY = std::clamp(g_ed.padY - sy * speed * dt, 0.0, g_ui.height);
				}
				else if (g_ed.padCursor && haveMouse && g_ed.mouseWasX >= 0 && std::hypot(lx - g_ed.mouseWasX, ly - g_ed.mouseWasY) > 3) g_ed.padCursor = false;
				g_ed.mouseWasX = lx; g_ed.mouseWasY = ly;
				if (g_ed.padCursor && g_ui.ax > 1e-6 && g_ui.ay > 1e-6) {
					lx = g_ed.padX; ly = g_ed.padY;
					c.mouseX = (lx - g_ui.bx) / g_ui.ax * c.width; c.mouseY = (ly - g_ui.by) / g_ui.ay * c.height;
				}
			}
			const bool ctrl = focused && (GetAsyncKeyState(VK_CONTROL) & 0x8000);
			const bool shift = focused && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
			const bool mouseDown = focused && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (pad & bindlogic::kA));
			// The Alt key hub: WASD belongs to the editor while Alt is down, so the map stays still.
			const bool altHeld = focused && !g_ed.naming && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
			const bool hubTurn = altHeld && shift;
			if (altHeld) inputgate::HoldMapAll();
			static bool wasDown = false;
			const bool click = mouseDown && !wasDown, release = !mouseDown && wasDown;
			wasDown = mouseDown;
			if (click) CommitName();
			if (g_ed.naming) {
				inputgate::HoldMapAll();  // Esc cancels the name instead of leaving the map
				for (int vk = 'A'; vk <= 'Z'; ++vk) if (g_keys.Pressed(vk, focused) && g_ed.nameBuf.size() < 32) g_ed.nameBuf.push_back(char(shift ? vk : vk - 'A' + 'a'));
				for (int vk = '0'; vk <= '9'; ++vk) if (g_keys.Pressed(vk, focused) && g_ed.nameBuf.size() < 32) g_ed.nameBuf.push_back(char(vk));
				if (g_keys.Pressed(VK_SPACE, focused) && g_ed.nameBuf.size() < 32) g_ed.nameBuf.push_back(' ');
				if (g_keys.Pressed(VK_OEM_MINUS, focused) && g_ed.nameBuf.size() < 32) g_ed.nameBuf.push_back('-');
				if (g_keys.Pressed(VK_BACK, focused) && !g_ed.nameBuf.empty()) g_ed.nameBuf.pop_back();
				if (g_keys.Pressed(VK_RETURN, focused)) CommitName();
				else if (g_keys.Pressed(VK_ESCAPE, focused)) { g_ed.naming = false; g_ed.namingGroup = 0; Status("Name not changed"); }
			}
			// Height without leaving the mouse (2026-10-30, the user: a point is 3D, so the wheel sets its height): near the selected point, or while dragging it, the wheel
			// moves it up and down in small steps; anywhere else it still zooms the map. A controller steps through fixed levels instead, and picks the point with the D-pad.
			{
				bool wheelNear = false;
				trafficdata::Route wheelScratch;
				const auto* wheelRoute = Shown(wheelScratch);
				if (!g_ed.naming && wheelRoute && !wheelRoute->ring && g_ed.point >= 0 && g_ed.point < int(wheelRoute->points.size()) && haveMouse) {
					const auto& q = wheelRoute->points[std::size_t(g_ed.point)];
					double px = 0, py = 0;
					wheelNear = g_ed.dragging || (Local(c, { q.x, q.y, q.z }, px, py) && std::hypot(px - lx, py - ly) < 70);
				}
				// The Route tool (stage 2): near the route's centre or its handle the wheel raises or lowers the whole route; elsewhere it zooms the map.
				const bool routeWheel = !g_ed.naming && g_ed.mode == Mode::Route && wheelRoute && haveMouse && !OverUi(lx, ly) && !g_ed.routeDrag.kind && NearRouteHandle(c, *wheelRoute, lx, ly);
				// Near the selected landing area the wheel turns it; near the selected zone it sizes it.
				bool placeWheel = false, placeHeight = g_ed.placeKind != 0;
				if (!g_ed.naming && haveMouse && !OverUi(lx, ly)) {
					double sx = 0, sy = 0;
					if (g_ed.mode == Mode::Drag && g_ed.spot >= 0) {  // the Move tool: the wheel sets the height of what is selected, as for a point
						const auto& list = g_ed.spotKind == 1 ? g_ed.file.landings : g_ed.file.spawns;
						if (g_ed.spot < int(list.size())) { const auto& a = list[std::size_t(g_ed.spot)].at; placeHeight = placeHeight || (Local(c, { a.x, a.y, a.z }, sx, sy) && std::hypot(sx - lx, sy - ly) < 70); }
					}
					else if (g_ed.mode == Mode::Drag && ZoneOn()) { const auto& z = g_ed.file.zones[std::size_t(g_ed.zone)]; placeHeight = placeHeight || (Local(c, { z.at.x, z.at.y, z.slow ? z.at.z : z.top }, sx, sy) && std::hypot(sx - lx, sy - ly) < 70); }
					else if (AreaOn() && g_ed.mode == Mode::Landing) { const auto& a = g_ed.file.landings[std::size_t(g_ed.spot)].at; placeWheel = Local(c, { a.x, a.y, a.z }, sx, sy) && std::hypot(sx - lx, sy - ly) < 90; }
					else if (ZoneOn() && g_ed.mode == Mode::Zone) { const auto& z = g_ed.file.zones[std::size_t(g_ed.zone)]; placeWheel = Local(c, { z.at.x, z.at.y, z.slow ? z.at.z : z.top }, sx, sy) && std::hypot(sx - lx, sy - ly) < 90; }
				}
				inputgate::WheelForEditor(wheelNear || routeWheel || placeWheel || placeHeight);
				if (const int notches = inputgate::TakeWheel(); notches && (wheelNear || routeWheel || placeWheel || placeHeight)) {
					const double dz = notches * (shift ? 25.0 : 5.0);
					if (g_ed.placeKind) { g_ed.placeAt.z += dz; g_ed.placeMoved = true; SetPlace(g_ed.placeAt); }
					else if (placeHeight) ChangeZ(dz);
					else if (placeWheel && AreaOn()) Press(notches > 0 ? kAreaTurnUp : kAreaTurnDown, c);
					else if (placeWheel) Press(notches > 0 ? kZoneSizeUp : kZoneSizeDown, c);
					else if (routeWheel && !wheelNear && ctrl) ScaleRoute(std::pow(1.1, notches));
					else if (routeWheel && !wheelNear) RaiseRoute(dz);
					else if (g_ed.dragging) g_ed.dragAt.z += dz; else ChangeZ(dz);
				}
				// The rest of the pad: B steps back, X tests, Y is View, LB / RB go through the routes, Start saves, View undoes; LT / RT turn a landing area,
				// size a zone or change a junction's share.
				if (freshPad && !g_ed.naming) {
					if (freshPad & bindlogic::kB) {
						if (g_ed.menu) g_ed.menu = 0;
						else if (g_ed.sheet) g_ed.sheet = 0;
						else if (g_ed.shapeAt) g_ed.shapeAt = false;
						else if (AreaOn() || ZoneOn()) Press(kPlaceDone, c);
						else Press(kSelect, c);
					}
					if (freshPad & bindlogic::kX) Press(kTest, c);
					if (freshPad & bindlogic::kY) g_ed.menu = g_ed.menu == 2 ? 0 : 2;
					if (freshPad & bindlogic::kLB) Press(kPrevRoute, c);
					if (freshPad & bindlogic::kRB) Press(kNextRoute, c);
					if (freshPad & bindlogic::kStart) Save();
					if ((freshPad & bindlogic::kView) && g_ed.draft && g_ed.draft->Undo()) Status("Undone");
					if (freshPad & (bindlogic::kLT | bindlogic::kRT)) {
						const bool up = (freshPad & bindlogic::kRT) != 0;
						if (AreaOn()) Press(up ? kAreaTurnUp : kAreaTurnDown, c);
						else if (ZoneOn()) Press(up ? kZoneSizeUp : kZoneSizeDown, c);
						else if (g_ed.link >= 0) ChangeShare(up ? 0.05 : -0.05);
					}
				}
				if ((freshPad & (bindlogic::kDUp | bindlogic::kDDown)) && !g_ed.naming && (AreaOn() || ZoneOn())) ChangeZ((freshPad & bindlogic::kDUp) ? kHeightLevel : -kHeightLevel);
				else if (freshPad && !g_ed.naming && wheelRoute && g_ed.mode == Mode::Route) {
					// The Route tool on a controller: D-pad up / down a 25 m level, left / right a 15 degree turn.
					if (freshPad & bindlogic::kDUp) RaiseRoute(kHeightLevel);
					if (freshPad & bindlogic::kDDown) RaiseRoute(-kHeightLevel);
					if (freshPad & bindlogic::kDLeft) TurnRoute(-15);
					if (freshPad & bindlogic::kDRight) TurnRoute(15);
				}
				else if (freshPad && !g_ed.naming && wheelRoute) {
					const int n = int(wheelRoute->points.size());
					if ((freshPad & bindlogic::kDLeft) && n) { g_ed.point = g_ed.point <= 0 ? n - 1 : g_ed.point - 1; g_ed.spot = -1; }
					if ((freshPad & bindlogic::kDRight) && n) { g_ed.point = (g_ed.point + 1) % n; g_ed.spot = -1; }
					if ((freshPad & (bindlogic::kDUp | bindlogic::kDDown)) && g_ed.point >= 0 && g_ed.point < n) {
						const double z = wheelRoute->points[std::size_t(g_ed.point)].z;
						ChangeZ(routeeditor::NextLevel(z, (freshPad & bindlogic::kDUp) ? 1 : -1, kHeightLevel) - z);
					}
				}
			}
			// Turning the map (see ApplyOrbit): right-drag on the map, left-right turns it, up-down tilts it.
			{
				static bool rightWas = false;
				const bool right = focused && haveMouse && !g_ed.naming && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 || hubTurn);
				if (right && !rightWas && (hubTurn || !OverUi(lx, ly))) {
					static bool told = false;
					g_ed.orbiting = true; g_ed.orbitX = lx; g_ed.orbitY = ly; g_ed.orbitYaw0 = g_mapYaw.load(); g_ed.orbitPitch0 = g_mapPitch.load();
					if (!told) { told = true; Status("Turning the map: left and right turn it, up and down tilt it. View > Reset map angle puts it back."); }
				}
				rightWas = right;
				if (!right) g_ed.orbiting = false;
				if (g_ed.orbiting) {
					if (hubTurn) {  // Alt+Shift: A D turn, W S tilt, on top of what the mouse does
						static ULONGLONG turnedAt = 0;
						const double dt = turnedAt && now - turnedAt < 100 ? (now - turnedAt) / 1000.0 : 0.0;
						turnedAt = now;
						auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
						g_ed.orbitYaw0 += float(((held('D') ? 1.0 : 0.0) - (held('A') ? 1.0 : 0.0)) * 60.0 * dt);
						g_ed.orbitPitch0 = std::clamp(float(g_ed.orbitPitch0 + ((held('S') ? 1.0 : 0.0) - (held('W') ? 1.0 : 0.0)) * 40.0 * dt), g_pitchMin.load(), g_pitchMax.load());
					}
					double yaw = std::fmod(g_ed.orbitYaw0 + (lx - g_ed.orbitX) / std::max(1.0, g_ui.width) * 180.0, 360.0);
					if (yaw < 0) yaw += 360.0;
					g_orbitYaw = float(yaw);
					g_orbitPitch = std::clamp(float(g_ed.orbitPitch0 + (ly - g_ed.orbitY) / std::max(1.0, g_ui.height) * 120.0), g_pitchMin.load(), g_pitchMax.load());
				}
				g_orbitOn = g_ed.orbiting;
			}
			if (click && haveMouse) {
				const Button* hitButton = ButtonAt(lx, ly);
				if (!hitButton) g_ed.menu = 0;  // a click outside an open menu closes it and still does what it clicked
				if (const Button* b = hitButton) {
					const int id = b->id;
					const bool enabled = b->enabled;
					const int style = b->style;
					const std::string label = b->text;
					if (enabled) Press(id, c);
					else if (style != 3) Status("'" + label + "' is not available now");
					// Menus close after a choice; View's ticks stay open so several can be changed.
					if (id != kMoreMenu && id != kViewMenu && id != kShapeTool && !(id >= kViewBase && id < kViewBase + 6)) g_ed.menu = 0;  // Reset map angle closes it
				}
				else if (ctrl && !altHeld && !OverUi(lx, ly) && PickClick(c, lx, ly)) {}
				else if (!OverUi(lx, ly) && StartPlaceDrag(c, lx, ly)) {}
				else if (g_ed.mode == Mode::Route && !OverUi(lx, ly) && Many()) {
					routemap::Vec3 at;
					if (routemap::MouseAt(c, AboutMiddleZ(), at)) { GroupBegin(); g_ed.groupDrag = 3; g_ed.groupLast = at; }
				}
				else if (g_ed.mode == Mode::Route && !OverUi(lx, ly)) StartRouteDrag(c, lx, ly);
				else if (!OverUi(lx, ly)) {
					trafficdata::Route ringScratch;
					const auto* ringShown = Shown(ringScratch);
					double hx = 0, hy = 0;
					if (g_ed.mode == Mode::Drag && ringShown && ringShown->ring &&
						Local(c, { ringShown->shape.cx, ringShown->shape.cy, ringShown->shape.z }, hx, hy) && std::hypot(hx - lx, hy - ly) < 18) {
						if (EnsureDraft()) { g_ed.point = -2; g_ed.dragging = true; g_ed.dragAt = { g_ed.draft->Value().shape.cx, g_ed.draft->Value().shape.cy, g_ed.draft->Value().shape.z }; }
					}
					else if (g_ed.mode == Mode::Drag) {
						const int p = PointAt(c, lx, ly, 16);
						if (p < 0) g_ed.picks.clear();
						if (p >= 0 && EnsureDraft()) {
							const bool group = Many() && IsPicked(kPickPoint, g_ed.draft->Value().name, p);  // asked before the point changes
							g_ed.point = p; g_ed.dragging = true;
							const auto& q = g_ed.draft->Value().points[std::size_t(p)];
							g_ed.dragAt = { q.x, q.y, q.z };
							if (group) { g_ed.pickPrimary = PrimaryNow(); GroupBegin(); g_ed.groupDrag = 1; g_ed.groupLast = g_ed.dragAt; }
						}
					}
					else ClickMap(c, lx, ly);
				}
			}
			if (g_ed.dragging && g_ed.draft) {
				routemap::Vec3 at;
				// Alt held while moving a point: straight up and down with the mouse (2026-10-30, 3D placement); let go of Alt to move it across again.
				const bool alt = focused && g_ed.point >= 0 && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
				if (alt) {
					if (!g_ed.altDrag) {
						g_ed.altDrag = true; g_ed.altY = ly; g_ed.altZ = g_ed.dragAt.z;
						double ax = 0, ay = 0, bx = 0, by = 0;  // pixels per metre of height at the point, as the camera sees it now
						g_ed.altPpm = Local(c, g_ed.dragAt, ax, ay) && Local(c, { g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z + 1.0 }, bx, by) ? std::max(0.3, ay - by) : 1.0;
					}
					g_ed.dragAt.z = std::round((g_ed.altZ + (g_ed.altY - ly) / g_ed.altPpm) * 2.0) / 2.0;
				}
				else { g_ed.altDrag = false; if (routemap::MouseAt(c, g_ed.dragAt.z, at)) g_ed.dragAt = at; }
				if (g_ed.groupDrag == 1) GroupDragTo(g_ed.dragAt);
				if ((release || !mouseDown) && g_ed.point == -2) {  // a ring's centre
					g_ed.dragging = false; g_ed.point = -1;
					auto r = g_ed.draft->Value(); r.shape.cx = g_ed.dragAt.x; r.shape.cy = g_ed.dragAt.y; g_ed.draft->Edit(r);
					Status("Ring centre moved");
				}
				else if (release || !mouseDown) {
					g_ed.dragging = false;
					if (g_ed.groupDrag == 1) { g_ed.groupDrag = 0; if (GroupEnd("Point move")) Status(PicksWords() + ": moved (Save keeps it)"); }
					else if (const char* why = PointProblem(g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z)) Refused("Point move", g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z, why);
					else {
						g_ed.draft->Move(std::size_t(g_ed.point), { g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z });
						char b[96]; std::snprintf(b, sizeof b, "Point %d moved to (%.0f, %.0f), height %.0f m", g_ed.point + 1, g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z);
						Status(b);
					}
				}
			}
			else g_ed.dragging = false;
			if (!g_ed.dragging) g_ed.altDrag = false;
			if (!g_ed.dragging && g_ed.groupDrag == 1) { g_ed.groupDrag = 0; GroupEnd("Point move"); }
			if (g_ed.placeKind) {
				const bool alt = focused && (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
				if (alt) {
					if (!g_ed.placeAlt) {
						g_ed.placeAlt = true; g_ed.placeAltY = ly; g_ed.placeAltZ = g_ed.placeAt.z;
						double ax = 0, ay = 0, bx = 0, by = 0;
						g_ed.placePpm = Local(c, g_ed.placeAt, ax, ay) && Local(c, { g_ed.placeAt.x, g_ed.placeAt.y, g_ed.placeAt.z + 1.0 }, bx, by) ? std::max(0.3, ay - by) : 1.0;
					}
					const double z = std::round((g_ed.placeAltZ + (g_ed.placeAltY - ly) / g_ed.placePpm) * 2.0) / 2.0;
					if (z != g_ed.placeAt.z) { g_ed.placeAt.z = z; g_ed.placeMoved = true; }
				}
				else {
					g_ed.placeAlt = false;
					routemap::Vec3 at;
					if (routemap::MouseAt(c, g_ed.placeAt.z, at) && (g_ed.placeMoved || std::hypot(at.x - g_ed.placeFrom.x, at.y - g_ed.placeFrom.y) > 2.0)) { g_ed.placeAt = at; g_ed.placeMoved = true; }
				}
				if (g_ed.groupDrag == 2) GroupDragTo(g_ed.placeAt);
				else if (g_ed.placeMoved) SetPlace(g_ed.placeAt);
				if ((release || !mouseDown) && g_ed.groupDrag == 2) {
					g_ed.groupDrag = 0; g_ed.placeKind = 0;
					if (GroupEnd("Move") && g_ed.placeMoved) Status(PicksWords() + ": moved (Save keeps it)");
				}
				else if (release || !mouseDown) {
					const auto p = g_ed.placeAt;
					const char* why = g_ed.placeMoved && g_ed.placeKind != 3 ? PointProblem(p.x, p.y, p.z) : nullptr;
					if (why) { SetPlace(g_ed.placeFrom); Refused("Place move", p.x, p.y, p.z, why); }
					else if (g_ed.placeMoved) {
						// A zone's foot stays on the ground under it.
						if (double ground = 0; g_ed.placeKind == 3 && g_ed.placeIndex < int(g_ed.file.zones.size()) && !g_ed.file.zones[std::size_t(g_ed.placeIndex)].slow && DownRay(p.x, p.y, p.z + 1500.0, p.z - 1500.0, ground) == 1) g_ed.file.zones[std::size_t(g_ed.placeIndex)].at.z = ground;
						char b[96]; std::snprintf(b, sizeof b, "Moved to (%.0f, %.0f), height %.0f m. Save keeps it.", p.x, p.y, p.z); Status(b);
					}
					g_ed.placeKind = 0;
				}
			}
			if (altHeld && !hubTurn && !g_ed.dragging && !g_ed.placeKind && !g_ed.routeDrag.kind) {
				HubKeys(c, ctrl, now);
				// Z C turn what can turn (a landing area, a route) or size a zone: a step, and again every 0.2 s while held.
				const bool turnLeft = (GetAsyncKeyState('Z') & 0x8000) != 0, turnRight = (GetAsyncKeyState('C') & 0x8000) != 0;
				if ((turnLeft != turnRight) && now - g_ed.hubTurnAt >= 200) {
					g_ed.hubTurnAt = now;
					if (Many()) TurnRoute(turnRight ? 15 : -15);
					else if (AreaOn()) Press(turnRight ? kAreaTurnUp : kAreaTurnDown, c);
					else if (ZoneOn()) Press(turnRight ? kZoneSizeUp : kZoneSizeDown, c);
					else if (g_ed.draft || g_ed.route >= 0 || g_ed.other >= 0) TurnRoute(turnRight ? 15 : -15);
				}
				else if (!turnLeft && !turnRight) g_ed.hubTurnAt = 0;
				// R F make the route bigger or smaller (the fingers already on W and A reach them): a step, and again every 0.2 s while held; Ctrl: fine.
				static ULONGLONG sizedAt = 0;
				const bool grow = (GetAsyncKeyState('R') & 0x8000) != 0, shrink = (GetAsyncKeyState('F') & 0x8000) != 0;
				if ((grow != shrink) && now - sizedAt >= 200) {
					sizedAt = now;
					if (Many() || (g_ed.spot < 0 && !ZoneOn() && (g_ed.draft || g_ed.route >= 0 || g_ed.other >= 0))) ScaleRoute(grow ? (ctrl ? 1.02 : 1.1) : (ctrl ? 1 / 1.02 : 1 / 1.1));
				}
				else if (!grow && !shrink) sizedAt = 0;
			}
			else if (g_ed.hubOn) { g_ed.hubWas = 0; HubEnd(); }
			else g_ed.hubWas = 0;
			if (g_ed.groupDrag == 3) {
				routemap::Vec3 at;
				if (routemap::MouseAt(c, g_ed.groupLast.z, at)) GroupDragTo(at);
				if (release || !mouseDown) { g_ed.groupDrag = 0; if (GroupEnd("Move")) Status(PicksWords() + ": moved (Save keeps it)"); }
			}
			if (g_ed.routeDrag.kind) {
				auto& rd = g_ed.routeDrag;
				routemap::Vec3 at;
				if (!g_ed.draft || g_ed.mode != Mode::Route) rd.kind = 0;
				else if (routemap::MouseAt(c, rd.centre.z, at)) {
					if (rd.kind == 1) rd.preview = routeeditor::Translated(rd.base, at.x - rd.start.x, at.y - rd.start.y, 0);
					else {
						const double turned = std::remainder((std::atan2(at.y - rd.centre.y, at.x - rd.centre.x) - rd.a0) / kDeg, 360.0);
						rd.turned = std::round(turned / 5.0) * 5.0;  // anticlockwise, snapped to 5 degrees
						rd.preview = routeeditor::Turned(rd.base, rd.turned * kDeg);
					}
				}
				if (rd.kind && (release || !mouseDown)) {
					const int kind = rd.kind;
					rd.kind = 0;
					const auto a = routeeditor::Centre(rd.base), b = routeeditor::Centre(rd.preview);
					const double moved = std::hypot(b.x - a.x, b.y - a.y);
					if (kind == 1 && moved < 0.5) Status("Route not moved");
					else if (kind == 2 && rd.turned == 0) Status("Route not turned");
					else if (RouteFits(rd.preview, kind == 1 ? "Route move" : "Route turn")) {
						g_ed.draft->Edit(rd.preview);
						char m[128];
						if (kind == 1) std::snprintf(m, sizeof m, "Route moved %.0f m. Every point kept its height. Save to keep it.", moved);
						else std::snprintf(m, sizeof m, "Route turned %.0f deg %s. Save to keep it.", std::fabs(rd.turned), rd.turned < 0 ? "clockwise" : "anticlockwise");
						Status(m);
					}
				}
			}
			if (!g_ed.naming) {
				// Ctrl first, so Ctrl+Y (redo) is not also taken as Y (View).
				const bool keyZ = g_keys.Pressed('Z', focused), keyY = g_keys.Pressed('Y', focused), keyS = g_keys.Pressed('S', focused);
				if (ctrl && !altHeld && keyZ && UndoAny()) Status("Undone");
				if (ctrl && !altHeld && keyY && RedoAny()) Status("Redone");
				if (ctrl && !altHeld && keyS) Save();
				if (!ctrl && !altHeld && keyY) g_ed.menu = g_ed.menu == 2 ? 0 : 2;
				if (!ctrl && !altHeld && keyZ) Press(kZoneTool, c);
				(void)keyS;
			}
			if (!g_ed.naming) for (const auto& k : kShortcuts)
				if (g_keys.Pressed(k.vk, focused) && !ctrl && !altHeld) {
					const Button* b = nullptr;
					for (const auto& o : g_ui.buttons) if (o.id == k.id) b = &o;
					if (b && !b->enabled) Status(std::string("'") + b->text + "' is not available now");
					else Press(k.id, c);
				}
			if (!g_ed.naming) {
			if (g_keys.Pressed(VK_INSERT, focused)) DropPoint(c);
			if (g_keys.Pressed(VK_DELETE, focused)) {
				// Del removes the selected point, spot or junction; with only a route selected it deletes the route (asked first, as the button).
				const bool part = g_ed.point >= 0 || g_ed.spot >= 0 || ZoneOn() || (g_ed.link >= 0 && g_ed.link < int(g_ed.file.links.size()));
				if (!part && (g_ed.draft || g_ed.route >= 0 || g_ed.other >= 0)) DeleteRoute(); else DeleteSelected();
			}
			if (g_keys.Pressed(VK_PRIOR, focused) || g_keys.Pressed(VK_OEM_6, focused)) ChangeZ(shift ? 25 : 5);
			if (g_keys.Pressed(VK_NEXT, focused) || g_keys.Pressed(VK_OEM_4, focused)) ChangeZ(shift ? -25 : -5);
			{
				trafficdata::Route scratch;
				const auto* r = Shown(scratch);
				const int n = r ? int(r->points.size()) : 0;
				if (g_keys.Pressed(VK_HOME, focused) && n) { g_ed.point = g_ed.point <= 0 ? n - 1 : g_ed.point - 1; g_ed.spot = -1; }
				if (g_keys.Pressed(VK_END, focused) && n) { g_ed.point = (g_ed.point + 1) % n; g_ed.spot = -1; }
			}
			}

			// ---- draw ----
			g_surfaceBudget = kSurfaceRaysPerFrame;
			trafficdata::Route scratch;
			const trafficdata::Route* shown = Shown(scratch);
			int minute = 0;
			const bool timeKnown = LocalMinute(minute);
			const int mine = int(g_ed.file.routes.size());
			for (std::size_t k = 0; k < g_ed.others.size(); ++k) {
				const bool sel = !g_ed.draft && int(k) == g_ed.other;
				if (!sel && !g_ed.view.others) continue;
				const auto& o = g_ed.others[k];
				const unsigned color = RouteColor(o.route, mine + int(k));
				DrawRoute(lines, c, o.line, color, sel ? 3 : 2, sel ? 0.95 : 0.6);
				const auto d = routeschedule::Evaluate(o.route.schedule, timeKnown, minute);
				if (g_ed.view.flow) DrawFlow(lines, c, o.line, std::clamp(o.route.lanes, 1, 2), o.route.speed, color, d.active ? d.vehicles : 0);
				if (g_ed.view.names) RouteName(movie, c, o.line, o.route.label.empty() ? o.name : o.route.label, color);
			}
			for (std::size_t k = 0; k < g_ed.file.routes.size(); ++k) {
				if (g_ed.draft && int(k) == g_ed.draftIndex) continue;
				if (!Usable(g_ed.file.routes[k])) continue;
				const auto t = ToTd(g_ed.file.routes[k], "");
				const auto line = trafficroute::Build(t);
				const auto d = routeschedule::Evaluate(t.schedule, timeKnown, minute);
				const bool sel = (!g_ed.draft && int(k) == g_ed.route) || IsPicked(kPickRoute, t.name);
				if (!sel && !g_ed.view.others) continue;
				const unsigned color = RouteColor(t, int(k));
				DrawRoute(lines, c, line, color, sel ? 3 : 2, d.active ? 0.9 : 0.45);
				// A branch fed by junctions has no vehicles of its own: a couple of markers still show which way it runs.
				if (g_ed.view.flow) DrawFlow(lines, c, line, std::clamp(t.lanes, 1, 2), t.speed, color, d.active ? d.vehicles : FlowShare(t.name) < 1.0 ? 2 : 0);
				if (g_ed.view.names) RouteName(movie, c, line, t.label.empty() ? t.name : t.label, color);
			}
			if (shown) {
				auto r = *shown;
				if (g_ed.routeDrag.kind && g_ed.draft) r = g_ed.routeDrag.preview;  // the Route tool's drag, before it is kept
				if (g_ed.dragging && g_ed.point >= 0 && g_ed.point < int(r.points.size())) r.points[std::size_t(g_ed.point)] = { g_ed.dragAt.x, g_ed.dragAt.y, g_ed.dragAt.z };
				if (g_ed.dragging && g_ed.point == -2) { r.shape.cx = g_ed.dragAt.x; r.shape.cy = g_ed.dragAt.y; }
				const auto line = trafficroute::Build(r);
				if (r.ring) {  // the ring's centre handle (Drag moves it, Drop sets it)
					double hx, hy;
					if (Local(c, { r.shape.cx, r.shape.cy, r.shape.z }, hx, hy)) {
						const routemap::Vec3 middle{ r.shape.cx, r.shape.cy, r.shape.z };
						const double ball = BallRadius(c, middle, true);
						const Surface* under = g_ed.view.stems ? DrawStem(lines, c, middle, hx, hy, ball, true, true) : nullptr;
						DrawRingMark(lines, c, middle, ball, 0xFFE15A, 1.0);
						char b[96]; std::snprintf(b, sizeof b, "Centre  R %.0f  Z %.0f m%s", r.shape.radius, r.shape.z, UpWords(under, r.shape.z).c_str());
						Text(movie, b, hx + 12, hy - 10, 0xFFE15A, 320, true);
					}
				}
				const auto d = routeschedule::Evaluate(r.schedule, timeKnown, minute);
				const unsigned color = RouteColor(r, ShownIndex());
				DrawRoute(lines, c, line, color, 10, 0.22);
				if (g_ed.draft) {
					DrawRoute(lines, c, line, color, 3, 0.95);
					int planned = d.active ? d.vehicles : r.schedule.allDay ? r.schedule.allDayVehicles : 0;
					for (const auto& w : r.schedule.windows) planned = std::max(planned, w.vehicles);
					if (g_ed.view.flow) DrawFlow(lines, c, line, std::clamp(r.lanes, 1, 2), r.speed, color, planned);
					if (g_ed.view.names) RouteName(movie, c, line, r.label.empty() ? r.name : r.label, color);
				}
				for (std::size_t i = 0; i < r.points.size(); ++i) {
					double x, yy;
					if (!Local(c, { r.points[i].x, r.points[i].y, r.points[i].z }, x, yy)) continue;
					const bool sel = int(i) == g_ed.point || IsPicked(kPickPoint, r.name, int(i));
					// The point in 3D: a lit ball on a stem down to the surface under it (see DrawStem).
					const routemap::Vec3 p{ r.points[i].x, r.points[i].y, r.points[i].z };
					const double radius = BallRadius(c, p, sel);
					const Surface* under = g_ed.view.stems ? DrawStem(lines, c, p, x, yy, radius, sel, !r.ring) : nullptr;
					DrawBall(lines, x, yy, radius, sel ? Ball{ 0xFFE15A, 0xB08A1C } : i == 0 ? Ball{ 0x7CFF8A, 0x2E9E48 } : Ball{ 0xF4F7FA, 0x7D8896 }, 1.0);
					const bool over = haveMouse && std::hypot(lx - x, ly - yy) < radius + 6;  // a whole route of numbers would hide the map: the selected one and the one under the cursor
					if (!sel && !over && !g_ed.view.numbers) continue;
					char b[64];
					if (sel || over) std::snprintf(b, sizeof b, "%zu  Z %.0f m%s", i + 1, p.z, UpWords(under, p.z).c_str());
					else std::snprintf(b, sizeof b, "%zu", i + 1);
					Text(movie, b, x + radius + 4, yy - 10, sel ? 0xFFE15A : 0xFFFFFF, sel || over ? 300 : 40, true);
				}
			}
			// Add: a faint copy of the next point under the cursor, joined to where it would go, with its stem (2026-10-30, 3D placement).
			// The same choice as DropPoint: after the selected point (else at the end), or into a stretch of the route within 12 px.
			if (shown && !shown->ring && g_ed.mode == Mode::Drop && haveMouse && !OverUi(lx, ly) && !g_ed.menu && !g_ed.dragging && PointAt(c, lx, ly, 14) < 0) {
				const auto& r = *shown;
				const std::size_t n = r.points.size();
				double z = Baseline(c);
				std::size_t after = g_ed.point >= 0 && g_ed.point < int(n) ? std::size_t(g_ed.point) + 1 : n;
				bool split = false;
				if (n >= 2) {
					double best = 12;
					for (std::size_t i = 0; i < (r.closed ? n : n - 1); ++i) {
						const auto& a = r.points[i]; const auto& b2 = r.points[(i + 1) % n];
						double ax, ay, bx, by;
						if (!Local(c, { a.x, a.y, a.z }, ax, ay) || !Local(c, { b2.x, b2.y, b2.z }, bx, by)) continue;
						const double d = SegmentDistance(lx, ly, ax, ay, bx, by);
						if (d < best) {
							const double dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
							const double t = l2 > 1e-9 ? std::clamp(((lx - ax) * dx + (ly - ay) * dy) / l2, 0.0, 1.0) : 0.5;
							best = d; after = i + 1; z = a.z + (b2.z - a.z) * t; split = true;
						}
					}
				}
				routemap::Vec3 at; double gx = 0, gy = 0;
				if (routemap::MouseAt(c, z, at) && Local(c, at, gx, gy)) {
					const unsigned color = RouteColor(r, ShownIndex());
					auto join = [&](const trafficdata::Vec& q) { double qx, qy; if (Local(c, { q.x, q.y, q.z }, qx, qy)) { lines.Style(2, color, 0.5); lines.Move(qx, qy); lines.Line(gx, gy); } };
					if (after > 0) join(r.points[after - 1]); else if (r.closed && n) join(r.points[n - 1]);
					if (after < n && (split || g_ed.point >= 0)) join(r.points[after]); else if (r.closed && n >= 2) join(r.points[0]);
					const double rad = BallRadius(c, at, false);
					const Surface* under = g_ed.view.stems ? DrawStem(lines, c, at, gx, gy, rad, false, true) : nullptr;
					DrawBall(lines, gx, gy, rad, Ball{ 0xF4F7FA, 0x7D8896 }, 0.45);
					char b[80]; std::snprintf(b, sizeof b, split ? "insert  Z %.0f m%s" : "new  Z %.0f m%s", at.z, UpWords(under, at.z).c_str());
					Text(movie, b, gx + rad + 4, gy - 10, 0xBFD9E8, 300, true);
				}
			}
			// The Route tool: the route's centre and its turn handle (drag the handle to turn, anywhere else to move).
			if (shown && g_ed.mode == Mode::Route) {
				const auto& r = g_ed.routeDrag.kind && g_ed.draft ? g_ed.routeDrag.preview : *shown;
				const auto centre = routeeditor::Centre(r);
				double sx = 0, sy = 0, hx = 0, hy = 0;
				if (Local(c, { centre.x, centre.y, centre.z }, sx, sy)) {
					const Surface* under = g_ed.view.stems ? DrawStem(lines, c, { centre.x, centre.y, centre.z }, sx, sy, 7, true, true) : nullptr;
					DrawRingMark(lines, c, { centre.x, centre.y, centre.z }, 8, kTeal, 1.0);
					if (under) { char b[64]; std::snprintf(b, sizeof b, "Z %.0f m%s", centre.z, UpWords(under, centre.z).c_str()); Text(movie, b, sx + 14, sy + 6, kTeal, 260, true); }
					if (!r.ring && TurnHandle(c, centre, hx, hy, lx, ly, g_ed.routeDrag.kind == 2)) {
						lines.Style(1.5, kTeal, 0.9); lines.Move(sx, sy); lines.Line(hx, hy);
						lines.Style(2.5, kTeal, 1); lines.Fill(0x0B1620, 1); lines.Circle(hx, hy, 9); lines.End();
						if (g_ed.routeDrag.kind == 2) {
							char b[32]; std::snprintf(b, sizeof b, "%.0f deg", std::fabs(g_ed.routeDrag.turned));
							Text(movie, b, hx + 14, hy - 8, kTeal, 100, true);
						}
					}
					lines.NoLine();
				}
			}
			for (int kind = 1; kind <= 2 && g_ed.view.spots; ++kind) {
				const auto& list = kind == 1 ? g_ed.file.landings : g_ed.file.spawns;
				for (std::size_t i = 0; i < list.size(); ++i) {
					double x, yy;
					if (!SpotHere(list[i]) || !Local(c, { list[i].at.x, list[i].at.y, list[i].at.z }, x, yy)) continue;
					const bool sel = (g_ed.spotKind == kind && g_ed.spot == int(i)) || IsPicked(kind == 1 ? kPickLanding : kPickSpawn, (kind == 1 ? g_ed.file.landings : g_ed.file.spawns)[i].id);
					// In 3D, as a route's points: a lit ball on a stem to the surface under it. Its frame says it is a place and not a point.
					const routemap::Vec3 p{ list[i].at.x, list[i].at.y, list[i].at.z };
					const double radius = BallRadius(c, p, sel);
					const unsigned color = kind == 1 ? 0x7CFF8A : 0xC58BFF;
					const Surface* under = g_ed.view.stems ? DrawStem(lines, c, p, x, yy, radius, sel, true) : nullptr;
					const double f = radius + 3;
					if (kind == 1) DrawPadMark(lines, c, p, radius, list[i].turn, sel ? Ball{ 0xFFE15A, 0xB08A1C } : Ball{ 0x97E06A, 0x2E7A3A }, 1.0);
					else DrawConeMark(lines, c, p, radius, sel ? Ball{ 0xFFE15A, 0xB08A1C } : Ball{ 0xC58BFF, 0x5B349A }, 1.0);
					char label[64];
					const char* letter = kind == 2 ? "S" : list[i].parked ? "P" : "L";
					std::snprintf(label, sizeof label, "%s  Z %.0f m%s", letter, p.z, UpWords(under, p.z).c_str());  // always: there are few of them
					Text(movie, label, x + f + 3, yy - 10, sel ? 0xFFE15A : color, 300, true);
				}
			}

			// A landing area shows its spaces only while it is selected; zones are circles at their height.
			if (AreaOn()) {
				const auto& a = g_ed.file.landings[std::size_t(g_ed.spot)];
				// Each space is a pad of its real size at its own height, turned with the area, on its own stem.
				const double t = a.turn * kDeg, ax = std::sin(t), ay = std::cos(t);
				for (const auto& q : trafficdata::Spaces(PlaceOf(a))) {
					double x, yy;
					if (!Local(c, { q.x, q.y, q.z }, x, yy)) continue;
					if (g_ed.view.stems) DrawStem(lines, c, { q.x, q.y, q.z }, x, yy, 3, false, true);
					lines.Style(2, 0x7CFF8A, 0.95); lines.Fill(0x7CFF8A, 0.22);
					const double half = 5.0;
					const double sign[5][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
					bool first = true;
					for (const auto& s : sign) {
						const double along = s[0] * half, across = s[1] * half;
						double px0 = 0, py0 = 0;
						if (!Local(c, { q.x + along * ax + across * ay, q.y + along * ay - across * ax, q.z }, px0, py0)) continue;
						if (first) lines.Move(px0, py0); else lines.Line(px0, py0);
						first = false;
					}
					lines.End();
				}
				lines.NoLine();
			}
			for (std::size_t i = 0; i < g_ed.file.zones.size() && g_ed.view.spots; ++i) {
				const auto& z = g_ed.file.zones[i];
				if (g_ed.unplacedSpots.count(z.name)) continue;
				const bool sel = g_ed.zone == int(i) || IsPicked(kPickZone, g_ed.file.zones[i].name);
				const unsigned color = z.slow ? kAmber : kCoral;
				const double h = z.slow ? z.at.z : z.top;
				// A solid shape, not a mark on the map: a keep-out zone is a cylinder from the ground to its top, with its roof tinted; a slow zone is a disc at its height.
				auto circle = [&](double height, double alpha, double width, bool fill) {
					lines.Style(width, color, alpha);
					if (fill) lines.Fill(color, sel ? 0.2 : 0.1);
					bool first = true;
					for (int k = 0; k <= 48; ++k) {
						const double a = k * kDeg * 7.5;
						double px0 = 0, py0 = 0;
						if (!Local(c, { z.at.x + z.radius * std::cos(a), z.at.y + z.radius * std::sin(a), height }, px0, py0)) continue;
						if (first) lines.Move(px0, py0); else lines.Line(px0, py0);
						first = false;
					}
					if (fill) lines.End();
				};
				circle(h, 0.95, sel ? 3 : 2, true);
				if (!z.slow) {
					circle(z.at.z, 0.6, 1, false);
					for (int k = 0; k < 8; ++k) {  // the walls
						const double a = k * kDeg * 45;
						double ax, ay, bx, by;
						if (Local(c, { z.at.x + z.radius * std::cos(a), z.at.y + z.radius * std::sin(a), z.at.z }, ax, ay) && Local(c, { z.at.x + z.radius * std::cos(a), z.at.y + z.radius * std::sin(a), h }, bx, by)) {
							lines.Style(1, color, 0.55); lines.Move(ax, ay); lines.Line(bx, by);
						}
					}
				}
				lines.NoLine();
				double x, yy;
				const routemap::Vec3 top{ z.at.x, z.at.y, h };
				if (Local(c, top, x, yy)) {
					const double radius = BallRadius(c, top, sel);
					const Surface* under = g_ed.view.stems ? DrawStem(lines, c, top, x, yy, radius, sel, true) : nullptr;
					DrawRingMark(lines, c, top, radius, sel ? 0xFFE15A : color, 1.0);
					char label[96];
					std::snprintf(label, sizeof label, "%s%s", ZoneWords(z).c_str(), UpWords(under, h).c_str());
					Text(movie, label, x + radius + 4, yy - 10, sel ? 0xFFE15A : color, 320, true);
				}
			}
			// The Shape tool: the shape as it would be, centre to the cursor.
			if (g_ed.mode == Mode::Shape && g_ed.shapeAt && haveMouse) {
				routemap::Vec3 edge;
				if (routemap::MouseAt(c, g_ed.shapeCentre.z, edge)) {
					const double radius = std::clamp(std::hypot(edge.x - g_ed.shapeCentre.x, edge.y - g_ed.shapeCentre.y), 30.0, 1500.0);
					const auto pts = routeeditor::ShapePoints(g_ed.shape, { g_ed.shapeCentre.x, g_ed.shapeCentre.y, g_ed.shapeCentre.z }, radius, 0);
					bool down = false;
					double fx = 0, fy = 0, px0 = 0, py0 = 0;
					// The points it will have, as balls on stems, then the line through them.
					for (const auto& q : pts) {
						if (!Local(c, { q.x, q.y, q.z }, px0, py0)) continue;
						const double rad = BallRadius(c, { q.x, q.y, q.z }, false);
						if (g_ed.view.stems) DrawStem(lines, c, { q.x, q.y, q.z }, px0, py0, rad, false, true);
						DrawBall(lines, px0, py0, rad, Ball{ 0xF4F7FA, 0x7D8896 }, 0.5);
					}
					lines.Style(2, kTeal, 0.7);
					for (const auto& q : pts) { if (Local(c, { q.x, q.y, q.z }, px0, py0)) { if (down) lines.Line(px0, py0); else { lines.Move(px0, py0); fx = px0; fy = py0; } down = true; } else down = false; }
					if (g_ed.shape != routeeditor::Shape::Helix && down) lines.Line(fx, fy);
					lines.NoLine();
					char b[48]; std::snprintf(b, sizeof b, "%.0f m across", radius * 2);
					Text(movie, b, lx + 14, ly - 10, kTeal, 160, true);
				}
			}
			// A faint copy of what the next click places, at the height it would get: Land, Spawn, Zone and a shape's centre.
			{
				const bool newLand = g_ed.mode == Mode::Landing && !AreaOn(), newSpawn = g_ed.mode == Mode::Spawn;
				const bool newZone = g_ed.mode == Mode::Zone && !ZoneOn(), newShape = g_ed.mode == Mode::Shape && !g_ed.shapeAt && !g_ed.draft;
				routemap::Vec3 at; double gx = 0, gy = 0;
				if ((newLand || newSpawn || newZone || newShape) && haveMouse && !OverUi(lx, ly) && !g_ed.menu && !g_ed.placeKind && routemap::MouseAt(c, c.focus.z, at)) {
					double ground = 0;
					const bool known = DownRay(at.x, at.y, at.z + 1500.0, at.z - 1500.0, ground) == 1 || TerrainAt(at.x, at.y, at.z, ground);
					at.z = (known ? ground : at.z) + (newZone ? 80.0 : newShape ? 40.0 : kGroundLift);
					if (Local(c, at, gx, gy)) {
						const double rad = BallRadius(c, at, false);
						const Surface* under = g_ed.view.stems ? DrawStem(lines, c, at, gx, gy, rad, false, true) : nullptr;
						if (newLand) DrawPadMark(lines, c, at, rad, 0, Ball{ 0x97E06A, 0x2E7A3A }, 0.45);
						else if (newSpawn) DrawConeMark(lines, c, at, rad, Ball{ 0xC58BFF, 0x5B349A }, 0.45);
						else DrawRingMark(lines, c, at, rad, newZone ? kCoral : kTeal, 0.45);
						char b[80]; std::snprintf(b, sizeof b, "new  Z %.0f m%s", at.z, UpWords(under, at.z).c_str());
						Text(movie, b, gx + rad + 4, gy - 10, 0xBFD9E8, 300, true);
					}
				}
			}
			// The pad cursor.
			if (g_ed.padCursor) {
				lines.Style(2, 0x0B1620, 1); lines.Fill(0xFFFFFF, 1);
				lines.Move(lx, ly); lines.Line(lx + 13, ly + 5); lines.Line(lx + 5, ly + 7); lines.Line(lx + 3, ly + 14); lines.Line(lx, ly);
				lines.End(); lines.NoLine();
			}
			for (std::size_t i = 0; i < g_ed.file.links.size() && g_ed.view.spots; ++i) {
				trafficroute::Vec at; double x, yy;
				if (!JunctionAt(g_ed.file.links[i], at) || !Local(c, { at.x, at.y, at.z }, x, yy)) continue;
				const bool sel = g_ed.link == int(i) || IsPicked(kPickLink, g_ed.file.links[i].id);
const auto& junction=g_ed.file.links[i];
const Surface* jointUnder = g_ed.view.stems ? DrawStem(lines, c, { at.x, at.y, at.z }, x, yy, 4, sel, true) : nullptr;
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
				{
					const routemap::Vec3 gem{ at.x, at.y, at.z };
					DrawSolid(lines, c, routemap::Diamond(gem, 8 * MetresPerPixel(c, gem)), gem, sel ? Ball{ 0xFFE15A, 0xB08A1C } : Ball{ 0xFF9FE0, 0x993556 }, 1.0, kDiamondLift);
				}
				{
					char label[80];
					std::snprintf(label, sizeof label, "%d%%  Z %.0f m%s", int(std::lround(junction.share * 100)), at.z, UpWords(jointUnder, at.z).c_str());
					Text(movie, label, x + 10, yy + 2, sel ? 0xFFE15A : 0xFF8AD8, 300, true);
				}
			}
			// The scale the speeds are seen against: 100 m at the map's centre.
			{
				double ax = 0, ay = 0, bx = 0, by = 0;
				if (Local(c, c.focus, ax, ay) && Local(c, { c.focus.x + 100.0, c.focus.y, c.focus.z }, bx, by)) {
					const double w = std::hypot(bx - ax, by - ay), sx = 24, sy = g_ui.height - 84;
					lines.Style(2, 0xFFFFFF, 0.9); lines.Move(sx, sy - 7); lines.Line(sx, sy); lines.Line(sx + w, sy); lines.Line(sx + w, sy - 7);
					Text(movie, "100 m", sx, sy - 26, 0xFFFFFF, 80, true);
				}
			}
			// Cover: catches clicks on the map while dropping / dragging so the map does not also place its own marker.
			// (any mode: the editor owns map clicks while it is open)
			cover.NoLine(); cover.Fill(0x000000, 0.01); cover.Rect(0, 0, g_ui.width, g_ui.height); cover.End();

			// ---- toolbar, menus and panel (stage 2) ----
			SwfTestTick(movie);
			Layout(shown, timeKnown, minute);
			auto frame = [&](double x, double y, double w, double h, unsigned fill, double alpha, unsigned edge, double edgeAlpha) {
				if (edgeAlpha > 0) panel.Style(1, edge, edgeAlpha); else panel.NoLine();
				panel.Fill(fill, alpha); panel.Rect(x, y, w, h); panel.End();
				panel.NoLine();
			};
			const double px = PanelX();
			frame(g_ui.barX, g_ui.barY, g_ui.barW, g_ui.barH, 0x141C29, 0.95, 0x3A4658, 1);
			frame(px, 12, kPanelW, g_ui.panelBottom - 12, 0x141C29, 0.94, 0x3A4658, 1);
			frame(px, 12, kPanelW, 62, 0x0B1119, 0.8, 0, 0);
			for (const auto& bx : g_ui.boxes) frame(bx.x, bx.y, bx.w, bx.h, bx.fill, bx.alpha, bx.line, bx.lineAlpha);
			if (g_ed.menu) frame(g_ui.menuX, g_ui.menuY, g_ui.menuW, g_ui.menuH, 0x141C29, 0.97, 0x3A4658, 1);
			for (const auto& b : g_ui.buttons) {
				const unsigned textColor = !b.enabled ? 0x5C6876u : b.active && (b.style == 0 || b.style == 1 || b.style == 5) ? 0x07211Fu : kText;
				const double tw = TextWidth(b.text);
				switch (b.style) {
				case 1: {  // toolbar tool: its name, its key under it
					if (b.active) frame(b.x, b.y, b.w, b.h, kTeal, 1, 0, 0);
					panel.Style(1, 0x3A4658, 1); panel.Move(b.x + b.w, b.y); panel.Line(b.x + b.w, b.y + b.h); panel.NoLine();
					Text(movie, b.text, b.x + (b.w - tw) / 2, b.y + 7, textColor, b.w);
					if (!b.key.empty()) Text(movie, b.key, b.x + (b.w - TextWidth(b.key)) / 2, b.y + 24, b.active ? 0x1D4F4A : kDim, b.w);
					break;
				}
				case 2: {  // folded group: a triangle, its name, its one-line summary
					const double ty = b.y + b.h / 2;
					panel.NoLine(); panel.Fill(b.active ? kTeal : kText, 1);
					if (b.active) { panel.Move(b.x + 2, ty - 3); panel.Line(b.x + 12, ty - 3); panel.Line(b.x + 7, ty + 3); }
					else { panel.Move(b.x + 4, ty - 5); panel.Line(b.x + 10, ty); panel.Line(b.x + 4, ty + 5); }
					panel.End();
					Text(movie, b.text, b.x + 18, b.y + 4, b.active ? kTeal : kText, 140);
					if (!b.value.empty()) Text(movie, b.value, b.x + b.w - std::min(TextWidth(b.value), b.w - 150), b.y + 4, kDim, b.w - 150);
					break;
				}
				case 3: {  // a line of the routes list
					if (b.active) { frame(b.x, b.y, b.w, b.h, kTeal, 0.17, 0, 0); frame(b.x, b.y, 2, b.h, kTeal, 1, 0, 0); }
					double tx = b.x + 8;
					if (b.swatch) { frame(b.x + 8, b.y + 10, 18, 3, b.swatch, 1, 0, 0); tx = b.x + 34; }
					const double vw = TextWidth(b.value);
					Text(movie, b.text, tx, b.y + 3, b.swatch ? kText : kTeal, b.w - (tx - b.x) - vw - 12);
					if (!b.value.empty()) Text(movie, b.value, b.x + b.w - vw - 8, b.y + 3, kDim, vw + 4);
					break;
				}
				case 4: {  // a menu line
					panel.Style(1, 0x3A4658, 1); panel.Move(b.x, b.y + b.h); panel.Line(b.x + b.w, b.y + b.h); panel.NoLine();
					Text(movie, b.text, b.x + 12, b.y + 5, b.enabled ? kText : 0x5C6876, b.w - 24);
					const std::string right = !b.key.empty() ? b.key : b.value;
					if (!right.empty()) Text(movie, right, b.x + b.w - TextWidth(right) - 10, b.y + 5, kDim, TextWidth(right) + 4);
					break;
				}
				case 5:  // one of a row's choices
					frame(b.x, b.y, b.w, b.h, b.active ? kTeal : 0x141C29, b.active ? 1 : 0.6, 0x3A4658, b.active ? 0 : 1);
					Text(movie, b.text, b.x + (b.w - tw) / 2, b.y + 3, !b.enabled ? 0x5C6876u : b.active ? 0x07211Fu : kDim, b.w);
					break;
				case 7:  // delete: red outline; red when it is asking for the second click
					frame(b.x, b.y, b.w, b.h, b.active ? kCoral : 0x141C29, b.active ? 1 : 0.6, kCoral, b.active ? 0 : 1);
					Text(movie, b.text, b.x + (b.w - tw) / 2, b.y + (b.h - 16) / 2, b.active ? 0x260A05u : kCoral, b.w);
					break;
				case 6:  // the problems badge
					frame(b.x, b.y, b.w, b.h, kAmber, b.active ? 0.75 : 1, 0, 0);
					Text(movie, b.text, b.x + 8, b.y + 3, 0x2A1A02, b.w - 16);
					break;
				default: {  // a panel button; its key at the right when there is room
					frame(b.x, b.y, b.w, b.h, !b.enabled ? 0x1A2331 : b.active ? kTeal : kRowA, 1, 0x3A4658, b.active ? 0 : 1);
					const double kw = TextWidth(b.key);
					if (!b.key.empty() && tw + kw + 16 <= b.w) {
						Text(movie, b.text, b.x + 8, b.y + (b.h - 16) / 2, textColor, b.w - kw - 12);
						Text(movie, b.key, b.x + b.w - kw - 6, b.y + (b.h - 16) / 2, b.active ? 0x1D4F4A : kDim, kw + 4);
					}
					else Text(movie, b.text, b.x + (b.w - tw) / 2, b.y + (b.h - 16) / 2, textColor, b.w);
					break;
				}
				}
			}
			for (const auto& t : g_panelText) Text(movie, t.text, t.x, t.y, t.color, t.w, false, t.unit);

			// Messages: one box under the toolbar (naming first), gone after 12 s.
			{
				std::vector<std::pair<std::string, unsigned>> msg;
				if (g_ed.naming) { msg.push_back({ "Name: " + g_ed.nameBuf + "_", 0xFFE15A }); msg.push_back({ "Enter keeps it, Esc cancels", kDim }); }
				if (!g_ed.status.empty() && now - g_ed.statusAt < 12000)
					for (const auto& l : strokefont::Wrap(g_ed.status, strokefont::Fit(540, kUnit))) msg.push_back({ l, kText });
				if (!g_ui.calibrated) msg.push_back({ "Move the mouse across the map once to line the overlay up", kAmber });
				if (msg.size() > 4) msg.resize(4);
				if (!msg.empty()) {
					const double mx = g_ui.barX, my = g_ui.barY + g_ui.barH + 6, mw = std::min(560.0, std::max(200.0, (g_ed.menu ? g_ui.menuX - 8 : px - 16) - mx));
					frame(mx, my, mw, 10 + kLine * double(msg.size()), 0x141C29, 0.94, 0x3A4658, 1);
					double yy = my + 4;
					for (const auto& [t, col] : msg) { Text(movie, t, mx + 10, yy, col, mw - 20); yy += kLine; }
				}
			}
			// Key hints (as the mockup has them): one row of boxed keys for the tool in use, left of the map's own buttons
			// (Database, Missions, Return, Fast travel, Back), which start at about x 1050 of 1920.
			{
				std::vector<std::pair<const char*, const char*>> hints;
				switch (g_ed.mode) {
				case Mode::Drop: hints = { { "Click", "Add point" }, { "Wheel", "Height" }, { "R-drag", "Turn map" }, { "Ins", "Add anywhere" } }; break;
				case Mode::Drag: hints = { { "Drag", "Move" }, { "Ctrl+click", "Add to selection" }, { "Alt+drag", "Up/down" }, { "Wheel", "Height" } }; break;
				case Mode::Route: hints = { { "Drag", "Move route" }, { "Handle", "Turn" }, { "Wheel", "Raise" }, { "Ctrl+wheel", "Size" } }; break;
				case Mode::Connect: hints = { { "Click a route", "Join the selected end" }, { "- / +", "Share" } }; break;
				case Mode::Landing: hints = { { "Click", "Place" }, { "Drag", "Move" }, { "Alt+drag", "Up/down" }, { "Wheel", "Turn" } }; break;
				case Mode::Zone: hints = { { "Click", "Place" }, { "Drag", "Move" }, { "Alt+drag", "Up/down" }, { "Wheel", "Size" } }; break;
				case Mode::Spawn: hints = { { "Click", "Place" }, { "Drag", "Move" }, { "Alt+drag", "Up/down" } }; break;
				case Mode::Shape: hints = { { "Click", g_ed.shapeAt ? "Set its size" : "Place its centre" }, { "R-drag", "Turn map" } }; break;
				default: hints = { { "Click", "Select" }, { "Ctrl+click", "Add to selection" }, { "Del", "Delete" }, { "R-drag", "Turn map" } }; break;
				}
				if (altHeld) hints = shift ? std::vector<std::pair<const char*, const char*>>{ { "W S", "Tilt map" }, { "A D", "Turn map" }, { "Mouse", "Both" } }
					: std::vector<std::pair<const char*, const char*>>{ { "WASD", "Move" }, { "Q E", "Down, up" }, { "Z C", "Turn" }, { "Ctrl", "Fine" }, { "Shift", "Map" } };
				else hints.push_back({ "Alt", "Move keys" });
				double hx = g_ui.barX;
				const double hy = g_ui.height - 76, limit = g_ui.width * 1040.0 / 1920.0;
				for (const auto& [key, what] : hints) {
					const double kw = TextWidth(key) + 12, ww = TextWidth(what);
					if (hx + kw + 7 + ww > limit) break;
					frame(hx, hy, kw, 20, 0x0B1620, 0.55, 0x9AA6B5, 1);
					Text(movie, key, hx + 6, hy + 2, 0xE8EEF3, kw, true);
					Text(movie, what, hx + kw + 7, hy + 2, 0xE8EEF3, ww + 4, true);
					hx += kw + 7 + ww + 16;
				}
			}
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
			else if (CachedTerrain(a.px, a.py, baked)) { a.hit = true; a.z = baked; a.how = "game's terrain"; }
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
			g_swf = SwfTest{};
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
