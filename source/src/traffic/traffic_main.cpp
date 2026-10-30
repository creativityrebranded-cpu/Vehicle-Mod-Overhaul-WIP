// FVO_Traffic.dll: the FVO Traffic addon (renamed from FVO_CityPlacements.dll on 2026-10-29, stage 1 of
// notes/TRAFFIC_ADDON_PROPOSAL_20261029.md; the user: "placements is just position of vehicles and traffic will be its own file,
// however if traffic is there it'll use all vehicles in placements or by whatever mod there is"). FVO Placements is now only
// FVO_CityPlacements.esm. This file is the entry and the route side (Atlas + Director to come); generator.cpp builds and keeps
// the traffic pairs (moved from core's trafficpool.cpp) through core's API v2 (traffic_api.h).
// History: Since 2026-10-28 it reads FVO traffic files (trafficdata.h, the user: "a type of mod that only needs a
// certain set of data to read so players can create their own traffic"): every *.ini in Data\SFSE\Plugins\FVO_Traffic\ plus
// the rings the in-game ring editor saves (Logs\FlyingVehicleOverhaul\ringroad.ini, route "ringroad" in its world). Core FVO
// offers each everyday-traffic pick; a pick near a route of the player's world becomes a trip along it (trafficroute.h): a lap
// and home, or to a destination place via the exit nearest it. Core boards the driver and parks the vehicle at the end. Needs
// core FlyingVehicleOverhaul.dll (fvo_addon_api.h); without it, this does nothing.
#include "traffic_api.h"
#include "fvolimits.h"
#include "generator.h"
#include "everyday.h"
#include "fvopaths.h"
#include "trafficdata.h"
#include "trafficroute.h"
#include "routeshare.h"
#include "trafficlinks.h"
#include "trafficinitial.h"

// Steam Starfield 1.16.244 (sfse_common/sfse_version.h: RUNTIME_VERSION_1_16_244 = MAKE_EXE_VERSION(1, 16, 244)).
constexpr unsigned kRuntime_1_16_244 = 0x01100F40;
#include "sfse/PluginAPI.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr int kSlots = fvolimits::kVehicles;  // trips at once: one per NPC pilot core can drive (fvolimits.h)
	constexpr ULONGLONG kLostMs = 3000;          // a trip core stopped stepping (fight, driver out) is over after this
	constexpr double kMinTripDistance = 150.0;   // m: a destination this close to the start is not a trip
	// The New Atlantis world (ringroad.ini's section for the user's ring); the starter file's places are there.
	constexpr char kNewAtlantis[] = "JemisonPlanetWorld157133511011091040";

	FvoLog_t g_log = nullptr;
	FvoWorldName_t g_worldName = nullptr;
	FvoBaseOf_t g_baseOf = nullptr;
	FvoSetRouteMarkers_t g_setMarkers = nullptr;
	FvoRefPos_t g_refPos = nullptr;
	const FvoTrafficApi* g_trafficApi = nullptr;  // core API v2 (null: core too old; no traffic pairs)
	FvoLocalTime_t g_localTime = nullptr;
	FvoSetWorldShift_t g_setShift = nullptr;
    std::vector<trafficinitial::Slot> g_initialSlots;
    bool g_initialClockPending=false;
	PluginHandle g_handle = 0;
	SFSEMessagingInterface* g_messaging = nullptr;

	void Log(const char* fmt, ...)
	{
		if (!g_log) return;
		char text[600];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(text, sizeof text, fmt, args);
		va_end(args);
		g_log(text);
	}

	std::string Narrow(const std::wstring& w)
	{
		std::string s;
		for (const wchar_t c : w) s.push_back(c < 128 ? char(c) : '?');
		return s;
	}
	std::wstring TrafficDir()
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&TrafficDir), &self);
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(self, path, MAX_PATH);
		std::wstring dir(path);
		dir = dir.substr(0, dir.find_last_of(L"\\/"));
		return dir + L"\\FVO_Traffic";
	}
	std::string ReadFile(const std::wstring& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream s;
		s << in.rdbuf();
		return s.str();
	}

	// ringroad.ini: every world section's ring= (section name = world id).
	std::vector<std::pair<std::string, std::string>> Ringroad()
	{
		std::vector<std::pair<std::string, std::string>> out;
		const std::wstring ini = fvopaths::File(L"ringroad.ini");
		std::vector<wchar_t> names(32768);
		const DWORD n = GetPrivateProfileSectionNamesW(names.data(), DWORD(names.size()), ini.c_str());
		for (const wchar_t* sec = names.data(); n && *sec; sec += wcslen(sec) + 1) {
			wchar_t w[256]{};
			GetPrivateProfileStringW(sec, L"ring", L"", w, 256, ini.c_str());
			if (*w) out.push_back({ Narrow(sec), Narrow(w) });
		}
		return out;
	}

	// First run: the folder gets a starter file for the editor's ring (exits at the four quarters; in New Atlantis also
	// landing places at the recorded parking spots, trips mixed).
	void WriteStarter(const std::wstring& dir)
	{
		CreateDirectoryW(dir.c_str(), nullptr);
		const auto rings = Ringroad();
		if (rings.empty()) return;
		for (const auto& [world, ring] : rings) {
			const bool na = world == kNewAtlantis;
			std::string text =
				"; FVO traffic file, written once from the in-game ring editor's ring (ringroad.ini).\n"
				"; Edit freely: FVO re-reads this folder every 10 s. Lines starting with ; are comments.\n"
				"; Sections: [world.NAME] id=, [route.NAME], [exit.NAME], [place.NAME], [traffic.NAME]. The ring itself stays in\n"
				"; ringroad.ini (route name \"ringroad\"), so the ring editor keeps working.\n\n"
				"[file]\nname = " + std::string(na ? "New Atlantis ring" : "Ring") + "\nenabled = 1\n\n"
				"[world." + std::string(na ? "newatlantis" : "ringworld") + "]\nid = " + world + "\n\n";
			const std::string w = na ? "newatlantis" : "ringworld";
			const char* exits[4] = { "north", "east", "south", "west" };
			for (int k = 0; k < 4; ++k) text += "[exit." + std::string(exits[k]) + "]\nroute = ringroad\nat = " + std::to_string(k * 90) + "\n\n";
			if (na) {
				text +=
					"; Landing places (recorded parking spots). near = the exit to leave by; leave it out to use the nearest.\n"
					"[place.mast_pads]\nworld = newatlantis\npoint = -877 1604 -161\nradius = 30\n\n"
					"[place.east_row]\nworld = newatlantis\npoint = -713 1672 -160\nradius = 30\n\n"
					"[place.south_lot]\nworld = newatlantis\npoint = -797 1549 -161\nradius = 30\n\n"
					"[place.upper_level]\nworld = newatlantis\npoint = -762 1858 3\nradius = 30\n\n";
			}
			text += "[traffic." + w + "]\nworld = " + w + "\nmax = 3\ngap = 25\nvehicles = any\ntrips = " + std::string(na ? "mixed" : "laps") +
				"\nreach = 400\nstart_on_route = 0\n";
			const std::wstring file = dir + L"\\" + (na ? L"NewAtlantis.ini" : std::wstring(world.begin(), world.end()) + L".ini");
			std::ofstream(file, std::ios::binary) << text;
			Log("FVO traffic: wrote the starter file %s", Narrow(file).c_str());
		}
	}

	trafficdata::Data g_data;       // as used: anchored worlds found in game are placed (moved into game coordinates)
	trafficdata::Data g_read;       // as the files say
	unsigned long long g_stamp = ~0ull;
	// Anchored worlds found in game: world name -> (the player's world then, the shift). Kept across file reloads; measured
	// again when the player is in another world (a new landing lays the city down again).
	struct Found { std::string world; trafficdata::Vec shift; };
	std::map<std::string, Found> g_found;

	void Rebuild()
	{
		g_data = g_read;
		for (const auto& [name, f] : g_found) trafficdata::PlaceWorld(g_data, name, f.world, f.shift);
	}

	// Looks for each anchored world's references in the player's world (a few times a minute; cheap lookups).
	void FindAnchors()
	{
		if (!g_refPos || g_read.anchors.empty()) return;
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return;
		bool changed = false;
		for (const auto& [name, refs] : g_read.anchors) {
			const auto it = g_found.find(name);
			if (it != g_found.end() && it->second.world == world) continue;
			for (const auto& a : refs) {
				double p[3];
				if (!g_refPos(a.id, p)) continue;
				const trafficdata::Vec shift{ p[0] - a.at.x, p[1] - a.at.y, p[2] - a.at.z };
				g_found[name] = { world, shift };
				changed = true;
				Log("FVO traffic: [world.%s] found in %s by anchor %08X: shift (%.2f, %.2f, %.2f)", name.c_str(), world, a.id, shift.x, shift.y, shift.z);
				break;
			}
		}
		if (changed) { Rebuild(); ++g_stamp; }
		// The map route editor (core) needs the shifts to draw and edit these routes; sent every pass (cheap, idempotent).
		if (g_setShift) for (const auto& [name, f] : g_found) g_setShift(name.c_str(), f.world.c_str(), f.shift.x, f.shift.y, f.shift.z);
	}
	// Read on demand (2026-10-29 audit, the user: "we dont need traffic file to be checked so often when we can just have an update
	// button in the menu"): the folder was scanned every ~2 s (each everyday pick, each Auto route plan, the 5 s marker tick). Now
	// once at start, then only when asked: the settings menu's "Reload traffic files", or the map route editor after it saves.
	std::atomic<bool> g_reloadWanted{ true };
	void RequestReload() { g_reloadWanted.store(true); }

	void Load()
	{
		if (!g_reloadWanted.exchange(false)) return;
		const ULONGLONG now = GetTickCount64();
		const std::wstring dir = TrafficDir();
		std::vector<std::wstring> files;
		unsigned long long stamp = 0;
		auto scan = [&] {
			files.clear(); stamp = 0;
			WIN32_FIND_DATAW fd{};
			const HANDLE h = FindFirstFileW((dir + L"\\*.ini").c_str(), &fd);
			if (h == INVALID_HANDLE_VALUE) return;
			do {
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
				files.push_back(fd.cFileName);
				stamp = stamp * 31 + (static_cast<unsigned long long>(fd.ftLastWriteTime.dwHighDateTime) << 32 | fd.ftLastWriteTime.dwLowDateTime) + fd.nFileSizeLow;
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		};
		scan();
		if (files.empty()) { WriteStarter(dir); scan(); }
		WIN32_FILE_ATTRIBUTE_DATA ra{};
		if (GetFileAttributesExW(fvopaths::File(L"ringroad.ini").c_str(), GetFileExInfoStandard, &ra))
			stamp = stamp * 31 + (static_cast<unsigned long long>(ra.ftLastWriteTime.dwHighDateTime) << 32 | ra.ftLastWriteTime.dwLowDateTime);
		if (stamp == g_stamp) return;
		g_stamp = stamp;
		trafficdata::Data d;
		std::sort(files.begin(), files.end());
		for (const auto& f : files) trafficdata::Read(ReadFile(dir + L"\\" + f), Narrow(f), d);
		for (const auto& [world, ring] : Ringroad()) trafficdata::AddRingroad(world, ring, d);
		trafficdata::PreferEditor(d);  // routes edited in the map route editor replace the originals of the same name
		g_read = std::move(d);
		Rebuild();
		Log("FVO traffic: %d file(s) in %s: %zu route(s), %zu exit(s), %zu place(s), %zu traffic rule(s)", g_data.files, Narrow(dir).c_str(),
			g_data.routes.size(), g_data.exits.size(), g_data.places.size(), g_data.traffic.size());
		for (const auto& p : g_data.problems) Log("FVO traffic: skipped %s", p.c_str());
	}

	struct Slot {
		bool used = false;
		bool repeat = false;
        unsigned vehicleId = 0;
		trafficroute::Line line;
		trafficroute::Trip trip;
        std::vector<trafficroute::Vec> turn;std::size_t turnAt=0;
		double speed = 25;
		ULONGLONG seen = 0;
		trafficroute::Leg logged = trafficroute::Leg::Climb;
		std::string route, dest;
		double x = 0, y = 0, z = 0;      // where its vehicle was last step (the lane slot check)
		bool holding = false;
		// Junctions (2026-10-29): the route the trip belongs to, where it left that route for a branch, and the junctions of the
		// route it is on now. `route` is the one being flown (vehicle counts follow it).
		std::string home;
		int homeLane = 0;
		double leftU = 0, leftTravelled = 0, homeTarget = 0;
		std::vector<trafficlinks::Junction> junctions;
		std::uint64_t dice = 0;
	};
	Slot g_slots[kSlots];
	bool g_cycleSupported = false;
	ULONGLONG g_lastStart = 0;
	int g_nextLane = 0, g_nextPlace = 0;
	bool g_mixedPlace = false;

	// Lane slots (2026-10-28, cold start "without risk"): a vehicle joins its lane only where no vehicle already on that lane is
	// within kSlotGap; until then it waits at lane height beside the lane. Vehicles on a lane all fly its speed, so the gaps hold.
	constexpr double kSlotGap = 60.0;
	bool SlotTaken(const Slot& me, const trafficroute::Vec& join)
	{
		for (const auto& o : g_slots) {
			if (&o == &me || !o.used || o.route != me.route || o.trip.lane != me.trip.lane) continue;
			// on the lane, or joining it (one that waits does not block: two joining at once cannot wait on each other)
			if (o.trip.leg != trafficroute::Leg::Follow && o.trip.leg != trafficroute::Leg::Join) continue;
			if (std::hypot(o.x-join.x,o.y-join.y)<kSlotGap&&std::fabs(o.z-join.z)<15.0) {
                // Already-established lane traffic wins; simultaneous joiners
                // use a stable identity order instead of mutually waiting.
                if(o.trip.leg==trafficroute::Leg::Follow || (o.vehicleId&&o.vehicleId<me.vehicleId))return true;
            }
		}
		return false;
	}

	// Route schedules and vehicle counts (2026-10-28, the map route editor). A route takes one more vehicle while its schedule
	// is active at the planet's local time and fewer than its density fly it. Editor routes (ed_*) have their own counts; the
	// world rule's max and route = still govern the hand-written routes. Unknown local time: scheduled routes wait.
	bool LocalMinute(int& minute)
	{
		double h = 0;
		if (!g_localTime || !g_localTime(&h) || !(h >= 0 && h <= 24)) return false;
		minute = std::clamp(int(h * 60.0), 0, routeschedule::kMinutesPerDay - 1);
		return true;
	}
	std::string ClockText(bool known, int minute)
	{
		if (!known) return "unknown";
		char b[16];
		std::snprintf(b, sizeof b, "%02d:%02d", minute / 60, minute % 60);
		return b;
	}
	bool EditorRoute(const trafficdata::Route& r) { return r.name.rfind("ed_", 0) == 0; }
	// `on` / `want`: the vehicles on the route now and its count now (the fair share's inputs, routeshare.h).
	bool Room(const trafficdata::Route& r, const trafficdata::Traffic& rule, const Slot* except, bool pool, int* on = nullptr, int* want = nullptr)
	{
		int minute = 0;
		const bool known = LocalMinute(minute);
		const auto d = routeschedule::Evaluate(r.schedule, known, minute);
		if (!d.known || !d.active) return false;
		int onRoute = 0, ruled = 0;
		for (const auto& o : g_slots) {
			if (!o.used || &o == except || o.dest == "your marker") continue;  // the player's own Auto on a route is not traffic
			if (o.route == r.name) ++onRoute;
			if (o.route.rfind("ed_", 0) != 0 && o.dest != "your marker") ++ruled;
		}
		if (on) *on = onRoute;
		if (want) *want = d.vehicles;
		if (onRoute >= d.vehicles) return false;
		return pool || EditorRoute(r) || ruled < rule.max;
	}
	bool Allowed(const trafficdata::Route& r, const trafficdata::Traffic& rule);
	// The route a free vehicle at (x, y) takes: the least filled of those in reach with room (routeshare.h); null when none.
	const trafficdata::Route* FairRoute(const char* world, const trafficdata::Traffic& rule, const Slot* except, bool pool, double x, double y,
		const std::string& own, trafficroute::Line* lineOut = nullptr)
	{
		std::vector<const trafficdata::Route*> routes;
		std::vector<trafficroute::Line> lines;
		std::vector<routeshare::Candidate> c;
		for (const auto& r : g_data.routes) {
			int on = 0, want = 0;
			if (!trafficdata::SameWorld(g_data, r.world, world) || !(Allowed(r, rule) || r.name == own) || !Room(r, rule, except, pool, &on, &want)) continue;
			auto l = trafficroute::Build(r);
			const auto p = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			c.push_back({ on, want, r.name == own ? 0.0 : std::hypot(p.x - x, p.y - y), r.name == own });
			routes.push_back(&r); lines.push_back(std::move(l));
		}
		const int k = routeshare::Pick(c, rule.reach);
		if (k < 0) return nullptr;
		if (lineOut) *lineOut = lines[std::size_t(k)];
		return routes[std::size_t(k)];
	}
	bool Allowed(const trafficdata::Route& r, const trafficdata::Traffic& rule) { return rule.route.empty() || r.name == rule.route || EditorRoute(r); }
	// Test route (the editor's Test route button writes test = 1): the next offered vehicle near it flies it once per save.
	std::string g_testDone;
	trafficdata::Traffic RuleFor(const char* world);

	std::vector<const trafficdata::Route*> WorldRoutes(const char* world)
	{
		std::vector<const trafficdata::Route*> out;
		for (const auto& r : g_data.routes) if (trafficdata::SameWorld(g_data, r.world, world)) out.push_back(&r);
		return out;
	}
	void FindJunctions(Slot& s)
	{
		s.junctions.clear();
		char world[201]{};
		if (g_data.links.empty() || !g_worldName || !g_worldName(world, sizeof world)) return;
		s.junctions = trafficlinks::Of(g_data.links, WorldRoutes(world), s.route);
	}
	// A branch is open while its schedule allows traffic now (a branch fed only by junctions has 0 vehicles of its own).
	bool OpenNow(const trafficdata::Route& r)
	{
		if (r.schedule.allDay) return true;
		int minute = 0;
		if (!LocalMinute(minute)) return false;
		for (const auto& w : r.schedule.windows) if (routeschedule::Contains(w, minute)) return true;
		return false;
	}
	// Onto the route the junction leads to. Back onto the trip's own route: the lap goes on (the skipped stretch counts).
	bool TakeJunction(Slot& s, const trafficlinks::Junction& j, bool forced)
	{
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return false;
		const auto* q = trafficlinks::Find(WorldRoutes(world), j.other);
		if (!q) return false;
		const bool home = q->name == s.home;
		if (!home && !OpenNow(*q)) return false;
		const auto line = trafficroute::Build(*q);
		const int lane = home && line.closed ? s.homeLane : trafficlinks::EntryLane(line, q->lanes, j.otherU);
		if (lane < 0) return false;
        s.turn=trafficlinks::TurnPath(s.line,s.trip.lane,j.u,line,lane,j.otherU,j.radius);s.turnAt=0;
		if (s.route == s.home) { s.leftU = s.trip.lastU; s.leftTravelled = s.trip.travelled; s.homeTarget = s.trip.targetU; s.homeLane = s.trip.lane; }
		auto t = s.trip;
		t.leg = trafficroute::Leg::Follow; t.lane = lane; t.lastU = j.otherU; t.uKnown = true;
		if (home) {
			const double skipped = trafficroute::Ahead(line, lane, s.leftU, j.otherU);
			t.travelled = s.leftTravelled + (std::isfinite(skipped) ? skipped : 0.0);
			t.targetU = s.homeTarget;
		}
		else { t.travelled = 0; t.targetU = line.closed ? j.otherU : trafficlinks::LaneEnd(line, lane); }
		Log("FVO traffic: %s lane %d: %s junction %s onto %s lane %d", s.route.c_str(), s.trip.lane, forced ? "end of branch, takes" : "turns at",
			j.link.c_str(), q->name.c_str(), lane);
		s.trip = t; s.line = line; s.route = q->name; s.speed = q->speed; s.logged = t.leg;
		FindJunctions(s);
		return true;
	}
	// A spur's far end (no junction there): land at the nearest landing place within 150 m of it, else at the end itself.
	void LandAtSpurEnd(Slot& s, double x, double y)
	{
		char world[201]{};
		trafficdata::Vec end = trafficroute::At(s.line, s.trip.lane, trafficlinks::LaneEnd(s.line, s.trip.lane));
		std::string name = "the end of " + s.route;
		double best = 150.0;
		if (g_worldName && g_worldName(world, sizeof world))
			for (const auto& p : g_data.places)
				if (trafficdata::SameWorld(g_data, p.world, world) && std::hypot(p.at.x - end.x, p.at.y - end.y) < best) {
					best = std::hypot(p.at.x - end.x, p.at.y - end.y); end = p.at; name = p.name;
				}
		s.trip.lap = false; s.trip.dest = end; s.dest = name;
		Log("FVO traffic: %s: spur end, landing at %s (%.0f m away)", s.route.c_str(), name.c_str(), std::hypot(end.x - x, end.y - y));
	}
	// After the route step: junctions passed this step (laps only) and the end of a branch.
	void Junctions(Slot& s, const trafficroute::Trip& before, double x, double y)
	{
		if (!s.trip.lap || s.home.empty()) return;
		if (before.leg == trafficroute::Leg::Follow && s.trip.leg == trafficroute::Leg::Follow && before.uKnown) {
			for (const auto& j : s.junctions) {
				const double start=j.radius>0?trafficroute::LeadU(s.line,s.trip.lane,j.u,2.5*j.radius,true):j.u;
                if (!trafficlinks::Crossed(s.line,s.trip.lane,before.lastU,s.trip.lastU,start))continue;
				// Any junction passed rolls its share: into a branch from the route, and (a junction in the middle of a branch,
				// 2026-10-29) back out to the trip's own route before the branch's end. The far end of a branch is always taken (below).
				if (trafficlinks::Roll(s.dice) < j.share && TakeJunction(s, j, false)) return;
			}
		}
		if (before.leg == trafficroute::Leg::Follow && s.trip.leg == trafficroute::Leg::Leave && !s.line.closed && s.route != s.home) {
			const trafficlinks::Junction* j = trafficlinks::At(s.junctions, trafficlinks::LaneEnd(s.line, s.trip.lane));
			if (j && TakeJunction(s, *j, true)) return;
			LandAtSpurEnd(s, x, y);
		}
	}

	bool TurnGoal(Slot& s, double x, double y, double z, FvoTripGoal* out)
	{
        if(s.turn.empty())return false;
        while(s.turnAt<s.turn.size()&&std::hypot(std::hypot(s.turn[s.turnAt].x-x,s.turn[s.turnAt].y-y),s.turn[s.turnAt].z-z)<5)++s.turnAt;
        if(s.turnAt<s.turn.size()){
            const auto& goal=s.turn[std::min(s.turnAt+1,s.turn.size()-1)];
            out->x=goal.x;out->y=goal.y;out->z=goal.z;out->stop=1;out->passThrough=1;out->fly=1;out->speed=float(std::min(15.0,s.speed));return true;
        }
        s.turn.clear();s.trip.uKnown=false;return false;
	}
	int Step(void* user, double x, double y, double z, FvoTripGoal* out)
	{
		auto* s = static_cast<Slot*>(user);
		s->seen = GetTickCount64();
		s->x = x; s->y = y; s->z = z;
        if(TurnGoal(*s,x,y,z,out))return 0;
		// Check the slot before Step can promote Join to Follow inside kReach.
		const auto before = s->trip;
		auto g = trafficroute::Step(s->line, s->trip, x, y, z);
		if (before.leg == trafficroute::Leg::Join && s->trip.leg == trafficroute::Leg::Follow) {
			const auto join = trafficroute::At(s->line, s->trip.lane, trafficroute::Nearest(s->line, s->trip.lane, x, y));
			if (SlotTaken(*s, join)) s->trip = before;
		}
		if (s->trip.leg == trafficroute::Leg::Join) {
			const auto join = trafficroute::At(s->line, s->trip.lane, trafficroute::Nearest(s->line, s->trip.lane, x, y));
			const bool taken = SlotTaken(*s, join);
			if (taken != s->holding) {
				s->holding = taken;
				Log("FVO traffic: %s lane %d: %s", s->route.c_str(), s->trip.lane, taken ? "slot taken, waiting beside the lane" : "slot free, joining");
			}
			if (taken) { g.x = x; g.y = y; g.z = join.z; g.stop = 3.0; g.passThrough = false; }
		}
		if (!s->junctions.empty() || s->route != s->home) {
			const std::string was = s->route;
			const bool wasLap = s->trip.lap;
			Junctions(*s, before, x, y);
            if(TurnGoal(*s,x,y,z,out))return 0;
			if (s->route != was || s->trip.lap != wasLap) g = trafficroute::Step(s->line, s->trip, x, y, z);
		}
		if (s->trip.leg != s->logged) {
			s->logged = s->trip.leg;
			Log("FVO traffic: %s lane %d trip%s%s: %s", s->route.c_str(), s->trip.lane, s->dest.empty() ? "" : " to ", s->dest.c_str(), trafficroute::Name(s->trip.leg));
		}
		if (s->trip.leg == trafficroute::Leg::Done) {
			s->used = s->repeat;
			if (!s->trip.lap) { out->x = s->trip.dest.x; out->y = s->trip.dest.y; out->z = s->trip.dest.z; return s->repeat ? 4 : 2; }
			return s->repeat ? 3 : 1;
		}
		out->x = g.x; out->y = g.y; out->z = g.z; out->stop = g.stop;
		out->passThrough = g.passThrough ? 1 : 0;
		out->fly = 1;  // routes are flight lanes
		out->speed = float(s->trip.leg == trafficroute::Leg::Follow ? s->speed : 15.0);
		return 0;
	}

	int Cycle(void* user, int restart, double x, double y, double z)
	{
		Slot* s = nullptr;
		for (auto& candidate : g_slots) if (&candidate == user) { s = &candidate; break; }
		if (!s || !s->used || !s->repeat || s->trip.leg != trafficroute::Leg::Done) return 0;
		s->seen = GetTickCount64();
		s->x = x; s->y = y; s->z = z;
		if (restart) {
			// The landing is the safe moment to take up edits (2026-10-28, the map route editor): the route as the files say now,
			// if it is still active and has room; else the nearest route that is; else the vehicle stays parked (return 0). The files
			// themselves are re-read by Offer and the route lights tick (every 2-5 s).
			char world[201]{};
			if (!g_worldName || !g_worldName(world, sizeof world)) return 0;
			const auto rule = RuleFor(world);
			const trafficdata::Route* route = nullptr;
			const std::string own = s->home.empty() ? s->route : s->home;  // a spur's vehicle flies home to its own route
			if (!s->home.empty())
				for (const auto& r : g_data.routes)
					if (r.name == own && trafficdata::SameWorld(g_data, r.world, world) && Room(r, rule, s, false)) { route = &r; break; }
			if (!route) route = FairRoute(world, rule, s, false, x, y, own);  // fair share; its own route on a tie
			{
				if (!route) {
					int minute = 0;
					const bool known = LocalMinute(minute);
					Log("FVO traffic: %s: landed; no route is active with room at local time %s; the vehicle stays parked", s->route.c_str(),
						ClockText(known, minute).c_str());
					s->used = false;
					return 0;
				}
				if (route->name != s->route)
					Log("FVO traffic: %s: landed; switching to route %s (fair share: it is the least filled in reach, or the old one is gone, inactive or full)",
						s->route.c_str(), route->name.c_str());
			}
			s->line = trafficroute::Build(*route);
			s->speed = route->speed; s->route = route->name;
			const int lane = route->lanes > 1 ? std::clamp(s->trip.lane, 0, 1) : 0;
			s->trip = trafficroute::StartLap(s->line, lane, {x, y, z});
			s->logged = s->trip.leg; s->holding = false; s->dest.clear();
			s->home = s->route; FindJunctions(*s);
			Log("FVO traffic: %s lane %d: landed dwell complete; starting another full lap (route as saved now)", s->route.c_str(), s->trip.lane);
		}
		return 1;
	}

	double ExitU(const trafficdata::Route& r, const trafficroute::Line& line, double at)
	{
		return r.ring ? at / 360.0 * trafficroute::kRingSamples : line.ToLine(at);
	}

	int Begin(const trafficdata::Traffic& rule, const char* world, Slot* free, double x, double y, double z, unsigned vehicleId,
		FvoTripClaim* out, bool pool,const std::string& initial={},int initialLane=-1);

	trafficdata::Traffic RuleFor(const char* world)
	{
		// The rules for this world (none written: the old ring behaviour, 2 trips 30 s apart, laps).
		trafficdata::Traffic rule;
		for (const auto& t : g_data.traffic) if (trafficdata::SameWorld(g_data, t.world, world)) { rule = t; break; }
		return rule;
	}
	Slot* FreeSlot(int& active)
	{
		const ULONGLONG now = GetTickCount64();
		active = 0;
		Slot* free = nullptr;
		for (auto& s : g_slots) {
            if(s.used&&now-s.seen>kLostMs&&!(s.vehicleId&&g_trafficApi&&
                (g_trafficApi->usesVehicle(s.vehicleId)||(g_trafficApi->tripActive&&g_trafficApi->tripActive(s.vehicleId)))))s.used=false;
			if (s.used) ++active; else if (!free) free = &s;
		}
		return free;
	}

	int Offer(double x, double y, double z, unsigned vehicleId, FvoTripClaim* out)
	{
		Load();
		const ULONGLONG now = GetTickCount64();
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return 0;
		const auto rule = RuleFor(world);
		int active = 0;
		Slot* free = FreeSlot(active);
		if (!free || (g_lastStart && now - g_lastStart < ULONGLONG(rule.gap * 1000.0))) return 0;  // counts: Room()
		return Begin(rule, world, free, x, y, z, vehicleId, out, false);
	}

	// The cold-start pool's trips: core asks once the spawned driver sits (pool vehicles do not count against `max`).
	// Would a route of this world take one more vehicle from (x, y) now (within reach, active, with room)? Asked by core before
	// it gives a baked parked vehicle a driver (PoolTrip with vehicleId 0 and out = null).
	bool Wanted(const char* world, double x, double y)
	{
		const auto rule = RuleFor(world);
		for (const auto& r : g_data.routes) {
			if (!trafficdata::SameWorld(g_data, r.world, world) || !Allowed(r, rule) || !Room(r, rule, nullptr, true)) continue;
			const auto l = trafficroute::Build(r);
			const auto q = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			if (std::hypot(q.x - x, q.y - y) <= rule.reach) return true;
		}
		return false;
	}
	int PoolTrip(int spot, unsigned vehicleId, double x, double y, double z, FvoTripClaim* out)
	{
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return 0;
		if (!vehicleId && !out) return Wanted(world, x, y) ? 1 : 0;
		int active = 0;
		Slot* free = FreeSlot(active);
		if (!free) return 0;
        const bool initial=generator::InitialVehicle(spot)&&spot>=0&&spot<int(g_initialSlots.size());
        const int ok = Begin(RuleFor(world), world, free, x, y, z, vehicleId, out, true,
            initial?g_initialSlots[spot].route:std::string(),initial?g_initialSlots[spot].lane:-1);
		if (ok) Log("FVO traffic: cold-start spot %d: vehicle %08X takes a trip", spot, vehicleId);
		return ok;
	}

	// Auto on routes (2026-10-28, the user: a destination "with a traffic route nearby adds as one of the vehicles and uses it to
	// get to the nearest exit to the destination and then continue from there"). The player's vehicle takes a slot like traffic
	// (the lane slot check keeps the gaps), joins at the route point nearest it and leaves at the exit nearest the marker; core
	// flies on to the marker itself. Used whenever the vehicle can join a route (within that world's traffic `reach`, the same
	// distance traffic joins from) and the route's leaving point is nearer the marker than the joining point (the user: "being in
	// route is very important ... an arb dist is not helpful": no minimum trip length). Of those, the least flying off the route.
	constexpr double kAutoGain = 30.0;  // m the route must bring the vehicle nearer the marker (a leaving point next to the joining point is no trip)
	int PlanRoute(unsigned vehicleId, double x, double y, double z, double tx, double ty, double tz, FvoTripClaim* out)
	{
		Load();
		char world[201]{};
		if (!out || !g_worldName || !g_worldName(world, sizeof world)) return 0;
		const double direct = std::hypot(tx - x, ty - y);
		const double reachMax = RuleFor(world).reach;
		const trafficdata::Route* best = nullptr;
		trafficroute::Line bestLine;
		double bestCost = 1e18, bestExitU = 0;
		for (const auto& r : g_data.routes) {
			if (!trafficdata::SameWorld(g_data, r.world, world)) continue;
			const auto l = trafficroute::Build(r);
			const auto in = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			const double reach = std::hypot(in.x - x, in.y - y);
			if (reach > reachMax) continue;
			// leave at the route's exit nearest the marker, else the route point nearest it
			double exitU = trafficroute::Nearest(l, 0, tx, ty), exitD = 1e18;
			for (const auto& e : g_data.exits) {
				if (e.route != r.name) continue;
				const double u = ExitU(r, l, e.at);
				const auto p = trafficroute::At(l, 0, u);
				const double d = std::hypot(p.x - tx, p.y - ty);
				if (d < exitD) { exitD = d; exitU = u; }
			}
			const auto out_ = trafficroute::At(l, 0, exitU);
			const double left = std::hypot(out_.x - tx, out_.y - ty);
			if (left > std::hypot(in.x - tx, in.y - ty) - kAutoGain) continue;  // the route does not bring it nearer
			const double cost = reach + left;
			if (cost < bestCost) { bestCost = cost; best = &r; bestLine = l; bestExitU = exitU; }
		}
		if (!best) return 0;
		int active = 0;
		Slot* free = FreeSlot(active);
		if (!free) return 0;
		Slot& s = *free;
		s = Slot{};
		s.used = true; s.vehicleId=vehicleId; s.seen = GetTickCount64(); s.line = bestLine; s.speed = best->speed; s.route = best->name;
		s.trip = trafficroute::StartTo(bestLine, best->lanes, { x, y, z }, { tx, ty, tz }, bestExitU);
		s.dest = "your marker";
		s.logged = s.trip.leg;
		s.x = x; s.y = y; s.z = z;
		Log("FVO traffic: your vehicle %08X takes route %s, lane %d, to your marker (%.0f m straight; joins %.0f m away, leaves %.0f m from it)",
			vehicleId, best->name.c_str(), s.trip.lane, direct, bestCost - std::hypot(trafficroute::At(bestLine, 0, bestExitU).x - tx,
			trafficroute::At(bestLine, 0, bestExitU).y - ty), std::hypot(trafficroute::At(bestLine, 0, bestExitU).x - tx, trafficroute::At(bestLine, 0, bestExitU).y - ty));
		out->step = &Step;
		out->user = &s;
		out->name = "route trip to your marker";
		return 1;
	}

	int Begin(const trafficdata::Traffic& rule, const char* world, Slot* free, double x, double y, double z, unsigned vehicleId,
		FvoTripClaim* out, bool pool,const std::string& initial,int initialLane)
	{
		const ULONGLONG now = GetTickCount64();
		if (!pool && !rule.anyVehicle) {
			const unsigned base = g_baseOf ? g_baseOf(vehicleId) : 0;
			const bool ok = (rule.rev8 && base == 0xFE001033) || std::find(rule.bases.begin(), rule.bases.end(), base) != rule.bases.end();
			if (!ok) return 0;
		}
		// A test route asked for in the editor first (once per save, twice the reach), else the route of this world nearest the
		// vehicle, within reach, active now and with room.
		const trafficdata::Route* route = nullptr;
		trafficroute::Line line;
		bool test = false;
        if(!initial.empty()){
            for(const auto& r:g_data.routes)if(r.name==initial&&trafficdata::SameWorld(g_data,r.world,world)&&Room(r,rule,nullptr,true)){
                route=&r;line=trafficroute::Build(r);break;}
            if(!route)return 0;
        }
		for (const auto& r : g_data.routes) {
            if(route)break;
			if (!r.test || !trafficdata::SameWorld(g_data, r.world, world)) continue;
			const std::string key = r.name + "@" + std::to_string(g_stamp);
			if (g_testDone == key) continue;
			const auto l = trafficroute::Build(r);
			const auto p = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			if (std::hypot(p.x - x, p.y - y) > 2 * rule.reach) continue;
			route = &r; line = l; test = true; g_testDone = key;
			break;
		}
		if (!test&&initial.empty()) route = FairRoute(world, rule, nullptr, pool, x, y, std::string(), &line);  // fair share (routeshare.h)
		if (!route) return 0;
		if (test) Log("FVO traffic: test route %s: vehicle %08X flies it once (asked for in the map route editor)", route->name.c_str(), vehicleId);
		const trafficdata::Vec home{ x, y, z };
		// Lap or a destination.
		bool toPlace = rule.trips == trafficdata::Trips::Places || (rule.trips == trafficdata::Trips::Mixed && (g_mixedPlace = !g_mixedPlace));
		const trafficdata::Place* place = nullptr;
		if (toPlace) {
			std::vector<const trafficdata::Place*> ok;
			for (const auto& p : g_data.places)
				if (trafficdata::SameWorld(g_data, p.world, world) && std::hypot(p.at.x - x, p.at.y - y) >= kMinTripDistance) ok.push_back(&p);
			if (!ok.empty()) place = ok[std::size_t(g_nextPlace++) % ok.size()];
		}
		Slot& s = *free;
		s = Slot{};
		s.used = true; s.vehicleId=vehicleId; s.seen = now; s.line = line; s.speed = route->speed; s.route = route->name;
		s.home = route->name; s.dice = (std::uint64_t(vehicleId) << 20) ^ now; FindJunctions(s);
		s.repeat = g_cycleSupported;
		if (place) {
			// The exit: the place's own, else this route's exit nearest the place, else the route point nearest it.
			double exitU = trafficroute::Nearest(line, 0, place->at.x, place->at.y), exitD = 1e18;
			for (const auto& e : g_data.exits) {
				if (e.route != route->name) continue;
				const double u = ExitU(*route, line, e.at);
				const auto p = trafficroute::At(line, 0, u);
				const double d = e.name == place->exitName ? -1.0 : std::hypot(p.x - place->at.x, p.y - place->at.y);
				if (d < exitD) { exitD = d; exitU = u; }
			}
			s.trip = trafficroute::StartTo(line, route->lanes, home, place->at, exitU);
			s.dest = place->name;
		}
		else {
			s.trip = trafficroute::StartLap(line, route->lanes > 1 ? g_nextLane : 0, home);
			g_nextLane ^= 1;
		}
        if(!initial.empty()){
            s.trip=trafficroute::StartLap(line,initialLane,home);s.trip.leg=trafficroute::Leg::Follow;
            s.trip.lastU=trafficroute::Nearest(line,initialLane,x,y);s.trip.uKnown=true;
            s.dest.clear();
        }
		s.logged = s.trip.leg;
		s.x = x; s.y = y; s.z = z;
		if (!pool) g_lastStart = now;
		Log("FVO traffic: vehicle %08X goes onto route %s, lane %d, %s%s", vehicleId, route->name.c_str(), s.trip.lane, place ? "to " : "a lap and home",
			place ? place->name.c_str() : "");
		out->step = &Step;
		out->user = &s;
		out->name = place ? "route trip to a place" : "route lap";
		return 1;
	}

    // Capacity is stable across clock/schedule changes. Birth locations are
    // authored ground origins, never arbitrary projections of airborne lanes.
    void SendPool(const char* world){
        if(!g_trafficApi)return;
        const auto rule=RuleFor(world);
        std::vector<FvoPoolSpot> origins;std::vector<std::string> originNames;
        for(const auto& s:g_data.spawns)if(trafficdata::SameWorld(g_data,s.world,world)){
            origins.push_back({s.at.x,s.at.y,s.at.z,s.heading*3.14159265358979/180});originNames.push_back("spawn."+s.name);}
        for(const auto& p:g_data.places)if(trafficdata::SameWorld(g_data,p.world,world)){
            origins.push_back({p.at.x,p.at.y,p.at.z,0});originNames.push_back("place."+p.name);}
        int target=0;
        for(const auto& r:g_data.routes)if(trafficdata::SameWorld(g_data,r.world,world)&&Allowed(r,rule)){
            int peak=r.schedule.allDay?r.schedule.allDayVehicles:0;
            for(const auto& w:r.schedule.windows)peak=std::max(peak,w.vehicles);
            target=std::min(fvolimits::kVehicles,target+std::max(0,peak));
        }
        target=std::clamp(std::max(target,rule.startOnRoute),0,fvolimits::kVehicles);
        // An origin may dispatch several vehicles over time. They are not born
        // together there: the generator reserves the footprint until it clears.
        std::vector<FvoPoolSpot> spots;std::vector<std::string> names;std::vector<int> reuse;
        std::vector<std::vector<trafficinitial::Slot>> perRoute;int minute=0;const bool clock=LocalMinute(minute);g_initialClockPending=false;
        for(const auto& r:g_data.routes)if(trafficdata::SameWorld(g_data,r.world,world)&&Allowed(r,rule)){
            if(!clock&&!r.schedule.allDay)g_initialClockPending=true;
            const auto demand=routeschedule::Evaluate(r.schedule,clock,minute);
            if(demand.known&&demand.active)perRoute.push_back(trafficinitial::Route(r,demand.vehicles));
        }
        g_initialSlots=trafficinitial::Interleave(perRoute);
        target=std::max(target,int(g_initialSlots.size()));
        for(int i=0;i<target;++i){
            spots.push_back(origins.empty()?FvoPoolSpot{}:origins[std::size_t(i)%origins.size()]);
            names.push_back("fleet."+std::to_string(i));reuse.push_back(origins.empty()?-1:0);}
        // Existing parked vehicles can take real drivers and leave normally.
        for(const auto& p:g_data.places)if(p.parked&&trafficdata::SameWorld(g_data,p.world,world)&&spots.size()<std::size_t(fvolimits::kVehicles)){
            spots.push_back({p.at.x,p.at.y,p.at.z,0});names.push_back("place."+p.name);reuse.push_back(1);}
        std::vector<const char*> ptrs;for(const auto& n:names)ptrs.push_back(n.c_str());
        generator::SetSpots(world,spots.data(),int(spots.size()),reuse.data(),ptrs.data(),&PoolTrip);
        std::vector<FvoPoolSpot> initial;for(const auto& s:g_initialSlots)initial.push_back({s.at.x,s.at.y,s.at.z,s.heading});
        generator::SetInitialScene(initial.data(),int(initial.size()));
        generator::WaitForInitialClock(g_initialClockPending);
        for(int i=0;i<int(g_initialSlots.size());++i){const auto& slot=g_initialSlots[i];
            for(const auto& route:g_data.routes)if(route.name==slot.route){const auto line=trafficroute::Build(route);std::vector<FvoPoolSpot> alternatives;
                for(int j=1;j<=16;++j){const double u=trafficroute::LeadU(line,slot.lane,slot.u,6.0*((j+1)/2),j%2==0);
                    const auto a=trafficroute::At(line,slot.lane,u),b=trafficroute::At(line,slot.lane,u+trafficroute::Direction(slot.lane)*0.001);
                    alternatives.push_back({a.x,a.y,a.z,std::atan2(b.x-a.x,b.y-a.y)});}
                generator::SetInitialAlternatives(i,alternatives.data(),int(alternatives.size()));break;
            }
        }
        Log("Traffic arrival: %zu active route positions prepared; ground origins are only for later departures",initial.size());
        Log("FVO traffic: %s: stable fleet capacity %d, %zu authored ground origins, %zu total registry slots; additions require validated concealed departure",world,target,origins.size(),spots.size());
    }
    std::string g_poolWorld;unsigned long long g_poolStamp=~0ull;
    std::atomic<bool> g_poolResend{false};
    // Start/Swap (the editor's loading panel covers the map): re-read the files and re-plan every route position from the
    // schedule as it is now, then the generator fills them under the panel, the same cold start as arriving in the city.
    void StartSwap(const FvoRushPlace* places,int count){RequestReload();g_poolResend=true;generator::Rush(places,count);}
    void PreparePool(){
        Load();char world[201]{};if(!g_worldName||!g_worldName(world,sizeof world))return;
        int minute=0;
        if(g_poolResend.exchange(false)||g_poolWorld!=world||g_poolStamp!=g_stamp||(g_initialClockPending&&LocalMinute(minute))){FindAnchors();SendPool(world);g_poolWorld=world;g_poolStamp=g_stamp;}
    }
    void TrafficTick(){PreparePool();generator::Tick();}
    void TrafficHousekeeping(){PreparePool();generator::Housekeeping();}

	// Route lights (2026-10-28, the user: the beacons were "too crowded and too large ... maybe small lights"): small lights (core's
	// probe model, routeprobes.ini [look] model) every `spacing` m along the middle of each route, a row of three at each exit and
	// a ring of six over each landing place. routeprobes.ini [look] spacing (m, default 45) and scale (default 4: a light about
	// 40 cm across). Core shows them while "Show traffic routes" is on (routeprobes.h).
	void Look(double& spacing, float& scale)
	{
		const std::wstring ini = fvopaths::File(L"routeprobes.ini");
		wchar_t text[32]{};
		GetPrivateProfileStringW(L"look", L"spacing", L"", text, 32, ini.c_str());
		spacing = std::wcstod(text, nullptr);
		if (!(spacing >= 10.0 && spacing <= 500.0)) { spacing = 45.0; WritePrivateProfileStringW(L"look", L"spacing", L"45", ini.c_str()); }
		GetPrivateProfileStringW(L"look", L"scale", L"", text, 32, ini.c_str());
		scale = float(std::wcstod(text, nullptr));
		if (!(scale >= 0.2f && scale <= 40.0f)) { scale = 4.0f; WritePrivateProfileStringW(L"look", L"scale", L"4", ini.c_str()); }
	}
	trafficroute::Vec Middle(const trafficdata::Route& r, const trafficroute::Line& l, double u)
	{
		const auto a = trafficroute::At(l, 0, u);
		if (r.lanes < 2) return a;
		const auto b = trafficroute::At(l, 1, u);
		return { (a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2 };
	}
	void Markers()
	{
		static ULONGLONG at = 0;
		static std::string sentWorld;
		static unsigned long long sentStamp = ~0ull;
		const ULONGLONG now = GetTickCount64();
		if (!g_setMarkers || ((at && now - at < 5000) && !g_reloadWanted.load())) return;  // a reload is taken at once
		at = now;
		Load();
		FindAnchors();
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return;
		PreparePool();
		if (world == sentWorld && g_stamp == sentStamp) return;
		sentWorld = world; sentStamp = g_stamp;
		std::vector<float> m;
		double spacing = 45.0; float scale = 4.0f;
		Look(spacing, scale);
		auto add = [&](double x, double y, double z) { m.push_back(float(x)); m.push_back(float(y)); m.push_back(float(z)); m.push_back(scale); };
		std::string only;  // the route this world's traffic is limited to (route = ...), if any
		for (const auto& t : g_data.traffic) if (trafficdata::SameWorld(g_data, t.world, world)) { only = t.route; break; }
		for (const auto& r : g_data.routes) {
			if (!trafficdata::SameWorld(g_data, r.world, world) || (!only.empty() && r.name != only)) continue;
			const auto l = trafficroute::Build(r);
			const double length = trafficroute::Length(l);
			const int n = std::clamp(int(length / spacing), 8, 110);
			for (int i = 0; i < n + (l.closed ? 0 : 1); ++i) {
				const auto p = Middle(r, l, l.Span() * i / n);
				add(p.x, p.y, p.z);
			}
			for (const auto& e : g_data.exits) {
				if (e.route != r.name) continue;
				const double u = ExitU(r, l, e.at), du = 5.0 / std::max(1.0, length / l.Span());  // three lights 5 m apart
				for (int k = -1; k <= 1; ++k) { const auto p = Middle(r, l, u + k * du); add(p.x, p.y, p.z - 1.5); }
			}
		}
		for (const auto& p : g_data.places)
			if (trafficdata::SameWorld(g_data, p.world, world))
				for (int k = 0; k < 6; ++k) add(p.at.x + 4.0 * std::sin(k * 1.0471975512), p.at.y + 4.0 * std::cos(k * 1.0471975512), p.at.z + 2.0);
		g_setMarkers(world, m.data(), int(m.size() / 4));
		Log("FVO traffic: %zu route probe point(s) sent for %s", m.size() / 4, world);
	}

	void ResetAll() { for(auto& s:g_slots)s={};g_poolWorld.clear();g_poolStamp=~0ull;g_testDone.clear();g_lastStart=0;generator::Reset();everyday::Reset(); }  // a save load

	void Attach()
	{
		const HMODULE core = GetModuleHandleW(L"FlyingVehicleOverhaul.dll");
		if (!core) return;
		g_log = reinterpret_cast<FvoLog_t>(GetProcAddress(core, "FVO_Log"));
		g_worldName = reinterpret_cast<FvoWorldName_t>(GetProcAddress(core, "FVO_WorldName"));
		g_baseOf = reinterpret_cast<FvoBaseOf_t>(GetProcAddress(core, "FVO_BaseOf"));
		g_setMarkers = reinterpret_cast<FvoSetRouteMarkers_t>(GetProcAddress(core, "FVO_SetRouteMarkers"));
		g_refPos = reinterpret_cast<FvoRefPos_t>(GetProcAddress(core, "FVO_RefPos"));
		g_localTime = reinterpret_cast<FvoLocalTime_t>(GetProcAddress(core, "FVO_LocalTime"));
		g_setShift = reinterpret_cast<FvoSetWorldShift_t>(GetProcAddress(core, "FVO_SetWorldShift"));
		// Core API v2: the traffic pairs (generator.cpp) and everyday traffic (everyday.cpp) run on core's frames through these hooks.
		if (const auto api = reinterpret_cast<FvoTrafficApi_t>(GetProcAddress(core, "FVO_TrafficApi"))) {
			static const FvoTrafficHooks hooks = { &TrafficTick, &TrafficHousekeeping, &ResetAll, &StartSwap, &generator::RushStatus, &everyday::Scan, &RequestReload };
			g_trafficApi = api(kFvoTrafficApiVersion, &hooks);
			if (g_trafficApi) { generator::Attach(g_trafficApi, g_log); everyday::Attach(g_trafficApi, g_log, &Offer); }
		}
		if (!g_trafficApi) Log("FVO traffic: core offers no traffic API v%d (FlyingVehicleOverhaul.dll older or newer than FVO_Traffic.dll); no traffic", int(kFvoTrafficApiVersion));
		if (!g_localTime) Log("FVO traffic: core has no FVO_LocalTime; scheduled routes wait (all-day routes run)");
		if (const auto cycle = reinterpret_cast<FvoRegisterTrafficCycle_t>(GetProcAddress(core, "FVO_RegisterTrafficCycle"))) g_cycleSupported = cycle(&Cycle) != 0;
		if (const auto plan = reinterpret_cast<FvoRegisterRoutePlanner_t>(GetProcAddress(core, "FVO_RegisterRoutePlanner"))) plan(&PlanRoute);
		if (const auto tick = reinterpret_cast<FvoRegisterTick_t>(GetProcAddress(core, "FVO_RegisterTick"))) tick(&Markers);
		if (!g_worldName) { Log("FVO traffic: core has no FVO_WorldName (an older FlyingVehicleOverhaul.dll); traffic files off"); return; }
		if (g_trafficApi) Log("FVO Traffic addon 0.1: route traffic attached (files in %s)", Narrow(TrafficDir()).c_str());
		Load();
	}

	void SfseMessage(SFSEMessagingInterface::Message* msg)
	{
		if (msg->type == SFSEMessagingInterface::kMessage_PostDataLoad) Attach();  // every plugin is loaded by now
		if (msg->type == SFSEMessagingInterface::kMessage_PreLoadGame) for (auto& s : g_slots) s.used = false;
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion, 3, "FVO_Traffic", "seemless",
	SFSEPluginVersionData::kAddressIndependence_Signatures,
	SFSEPluginVersionData::kStructureIndependence_1_14_70_Layout,
	{ kRuntime_1_16_244, 0 }, 0, 0, 0,
};
__declspec(dllexport) bool SFSEPlugin_Load(const SFSEInterface* sfse)
{
	g_handle = sfse->GetPluginHandle();
	g_messaging = static_cast<SFSEMessagingInterface*>(sfse->QueryInterface(kInterface_Messaging));
	return g_messaging && g_messaging->RegisterListener(g_handle, "SFSE", SfseMessage);
}
}
