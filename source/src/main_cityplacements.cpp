// FVO_CityPlacements.dll (2026-10-27, the user: "one mod with placements and traffic called the same name placements was").
// The traffic addon. Since 2026-10-28 it reads FVO traffic files (trafficdata.h, the user: "a type of mod that only needs a
// certain set of data to read so players can create their own traffic"): every *.ini in Data\SFSE\Plugins\FVO_Traffic\ plus
// the rings the in-game ring editor saves (Logs\FlyingVehicleOverhaul\ringroad.ini, route "ringroad" in its world). Core FVO
// offers each everyday-traffic pick; a pick near a route of the player's world becomes a trip along it (trafficroute.h): a lap
// and home, or to a destination place via the exit nearest it. Core boards the driver and parks the vehicle at the end. Needs
// core FlyingVehicleOverhaul.dll (fvo_addon_api.h); without it, this does nothing.
#include "fvo_addon_api.h"
#include "fvopaths.h"
#include "trafficdata.h"
#include "trafficroute.h"

// Steam Starfield 1.16.244 (sfse_common/sfse_version.h: RUNTIME_VERSION_1_16_244 = MAKE_EXE_VERSION(1, 16, 244)).
constexpr unsigned kRuntime_1_16_244 = 0x01100F40;
#include "sfse/PluginAPI.h"

#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	constexpr int kSlots = 16;
	constexpr ULONGLONG kLostMs = 3000;          // a trip core stopped stepping (fight, driver out) is over after this
	constexpr double kMinTripDistance = 150.0;   // m: a destination this close to the start is not a trip
	// The New Atlantis world (ringroad.ini's section for the user's ring); the starter file's places are there.
	constexpr char kNewAtlantis[] = "JemisonPlanetWorld157133511011091040";

	FvoLog_t g_log = nullptr;
	FvoWorldName_t g_worldName = nullptr;
	FvoBaseOf_t g_baseOf = nullptr;
	FvoSetRouteMarkers_t g_setMarkers = nullptr;
	FvoRefPos_t g_refPos = nullptr;
	FvoPoolSet_t g_poolSet = nullptr;
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
	}
	ULONGLONG g_readAt = 0;

	void Load()
	{
		const ULONGLONG now = GetTickCount64();
		if (g_readAt && now - g_readAt < 10000) return;
		g_readAt = now;
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
		g_read = std::move(d);
		Rebuild();
		Log("FVO traffic: %d file(s) in %s: %zu route(s), %zu exit(s), %zu place(s), %zu traffic rule(s)", g_data.files, Narrow(dir).c_str(),
			g_data.routes.size(), g_data.exits.size(), g_data.places.size(), g_data.traffic.size());
		for (const auto& p : g_data.problems) Log("FVO traffic: skipped %s", p.c_str());
	}

	struct Slot {
		bool used = false;
		trafficroute::Line line;
		trafficroute::Trip trip;
		double speed = 25;
		ULONGLONG seen = 0;
		trafficroute::Leg logged = trafficroute::Leg::Climb;
		std::string route, dest;
		double x = 0, y = 0, z = 0;      // where its vehicle was last step (the lane slot check)
		bool holding = false;
	};
	Slot g_slots[kSlots];
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
			if (o.trip.leg != trafficroute::Leg::Follow && (o.trip.leg != trafficroute::Leg::Join || o.holding)) continue;
			if (std::hypot(o.x - join.x, o.y - join.y) < kSlotGap && std::fabs(o.z - join.z) < kSlotGap) return true;
		}
		return false;
	}

	int Step(void* user, double x, double y, double z, FvoTripGoal* out)
	{
		auto* s = static_cast<Slot*>(user);
		s->seen = GetTickCount64();
		s->x = x; s->y = y; s->z = z;
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
		if (s->trip.leg != s->logged) {
			s->logged = s->trip.leg;
			Log("FVO traffic: %s lane %d trip%s%s: %s", s->route.c_str(), s->trip.lane, s->dest.empty() ? "" : " to ", s->dest.c_str(), trafficroute::Name(s->trip.leg));
		}
		if (s->trip.leg == trafficroute::Leg::Done) {
			s->used = false;
			if (!s->trip.lap) { out->x = s->trip.dest.x; out->y = s->trip.dest.y; out->z = s->trip.dest.z; return 2; }
			return 1;
		}
		out->x = g.x; out->y = g.y; out->z = g.z; out->stop = g.stop;
		out->passThrough = g.passThrough ? 1 : 0;
		out->fly = 1;  // routes are flight lanes
		out->speed = float(s->trip.leg == trafficroute::Leg::Follow ? s->speed : 15.0);
		return 0;
	}

	double ExitU(const trafficdata::Route& r, const trafficroute::Line&, double at)
	{
		return r.ring ? at / 360.0 * trafficroute::kRingSamples : at;
	}

	int Begin(const trafficdata::Traffic& rule, const char* world, Slot* free, double x, double y, double z, unsigned vehicleId,
		FvoTripClaim* out, bool pool);

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
			if (s.used && now - s.seen > kLostMs) s.used = false;  // core ended it (a fight, the driver got out)
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
		if (!free || active >= rule.max || (g_lastStart && now - g_lastStart < ULONGLONG(rule.gap * 1000.0))) return 0;
		return Begin(rule, world, free, x, y, z, vehicleId, out, false);
	}

	// The cold-start pool's trips: core asks once the spawned driver sits (pool vehicles do not count against `max`).
	int PoolTrip(int spot, unsigned vehicleId, double x, double y, double z, FvoTripClaim* out)
	{
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return 0;
		int active = 0;
		Slot* free = FreeSlot(active);
		if (!free) return 0;
		const int ok = Begin(RuleFor(world), world, free, x, y, z, vehicleId, out, true);
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
		s.used = true; s.seen = GetTickCount64(); s.line = bestLine; s.speed = best->speed; s.route = best->name;
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
		FvoTripClaim* out, bool pool)
	{
		const ULONGLONG now = GetTickCount64();
		if (!pool && !rule.anyVehicle) {
			const unsigned base = g_baseOf ? g_baseOf(vehicleId) : 0;
			const bool ok = (rule.rev8 && base == 0xFE001033) || std::find(rule.bases.begin(), rule.bases.end(), base) != rule.bases.end();
			if (!ok) return 0;
		}
		// The route of this world nearest the vehicle, within reach.
		const trafficdata::Route* route = nullptr;
		trafficroute::Line line;
		double best = rule.reach;
		for (const auto& r : g_data.routes) {
			if (!trafficdata::SameWorld(g_data, r.world, world) || (!rule.route.empty() && r.name != rule.route)) continue;
			const auto l = trafficroute::Build(r);
			const auto p = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			const double d = std::hypot(p.x - x, p.y - y);
			if (d <= best) { best = d; route = &r; line = l; }
		}
		if (!route) return 0;
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
		s.used = true; s.seen = now; s.line = line; s.speed = route->speed; s.route = route->name;
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

	// The cold-start pool for the player's world: its traffic rule's start_on_route = N takes the first N [spawn.*] spots (6 at
	// most); core spawns there once per save and asks PoolTrip for each trip.
	void SendPool(const char* world)
	{
		if (!g_poolSet) return;
		const auto rule = RuleFor(world);
		std::vector<FvoPoolSpot> spots;
		if (rule.startOnRoute > 0)
			for (const auto& s : g_data.spawns)
				if (trafficdata::SameWorld(g_data, s.world, world) && int(spots.size()) < std::min(rule.startOnRoute, 6))
					spots.push_back({ s.at.x, s.at.y, s.at.z, s.heading * 3.14159265358979 / 180.0 });
		static std::string sentWorld;
		static std::size_t sentCount = ~std::size_t(0);
		if (sentWorld != world || sentCount != spots.size()) {
			sentWorld = world; sentCount = spots.size();
			Log("FVO traffic: cold-start pool for %s: %zu spawn spot(s)", world, spots.size());
		}
		g_poolSet(world, spots.data(), int(spots.size()), &PoolTrip);
	}

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
		if (!g_setMarkers || (at && now - at < 5000)) return;
		at = now;
		Load();
		FindAnchors();
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return;
		SendPool(world);
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

	void Attach()
	{
		const HMODULE core = GetModuleHandleW(L"FlyingVehicleOverhaul.dll");
		if (!core) return;
		g_log = reinterpret_cast<FvoLog_t>(GetProcAddress(core, "FVO_Log"));
		g_worldName = reinterpret_cast<FvoWorldName_t>(GetProcAddress(core, "FVO_WorldName"));
		g_baseOf = reinterpret_cast<FvoBaseOf_t>(GetProcAddress(core, "FVO_BaseOf"));
		g_setMarkers = reinterpret_cast<FvoSetRouteMarkers_t>(GetProcAddress(core, "FVO_SetRouteMarkers"));
		g_refPos = reinterpret_cast<FvoRefPos_t>(GetProcAddress(core, "FVO_RefPos"));
		g_poolSet = reinterpret_cast<FvoPoolSet_t>(GetProcAddress(core, "FVO_PoolSet"));
		if (const auto plan = reinterpret_cast<FvoRegisterRoutePlanner_t>(GetProcAddress(core, "FVO_RegisterRoutePlanner"))) plan(&PlanRoute);
		if (const auto tick = reinterpret_cast<FvoRegisterTick_t>(GetProcAddress(core, "FVO_RegisterTick"))) tick(&Markers);
		const auto reg = reinterpret_cast<FvoRegisterTrafficProvider_t>(GetProcAddress(core, "FVO_RegisterTrafficProvider"));
		if (!g_worldName) { Log("FVO traffic: core has no FVO_WorldName (an older FlyingVehicleOverhaul.dll); traffic files off"); return; }
		if (reg && reg(&Offer)) Log("FVO traffic: route traffic attached (files in %s)", Narrow(TrafficDir()).c_str());
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
	SFSEPluginVersionData::kVersion, 2, "FVO_CityPlacements", "seemless",
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
