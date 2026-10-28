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

	trafficdata::Data g_data;
	unsigned long long g_stamp = ~0ull;
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
		g_data = std::move(d);
		Log("FVO traffic: %d file(s) in %s: %zu route(s), %zu exit(s), %zu place(s), %zu traffic rule(s)", g_data.files, Narrow(dir).c_str(),
			g_data.routes.size(), g_data.exits.size(), g_data.places.size(), g_data.traffic.size());
		for (const auto& p : g_data.problems) Log("FVO traffic: skipped %s", p.c_str());
		for (const auto& t : g_data.traffic)
			if (t.startOnRoute > 0) Log("FVO traffic: [traffic.%s] start_on_route = %d is read but not built yet (vehicles start from their spots)", t.name.c_str(), t.startOnRoute);
	}

	struct Slot {
		bool used = false;
		trafficroute::Line line;
		trafficroute::Trip trip;
		double speed = 25;
		ULONGLONG seen = 0;
		trafficroute::Leg logged = trafficroute::Leg::Climb;
		std::string route, dest;
	};
	Slot g_slots[kSlots];
	ULONGLONG g_lastStart = 0;
	int g_nextLane = 0, g_nextPlace = 0;
	bool g_mixedPlace = false;

	int Step(void* user, double x, double y, double z, FvoTripGoal* out)
	{
		auto* s = static_cast<Slot*>(user);
		s->seen = GetTickCount64();
		const auto g = trafficroute::Step(s->line, s->trip, x, y, z);
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

	int Offer(double x, double y, double z, unsigned vehicleId, FvoTripClaim* out)
	{
		Load();
		const ULONGLONG now = GetTickCount64();
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return 0;
		// The rules for this world (none written: the old ring behaviour, 2 trips 30 s apart, laps).
		trafficdata::Traffic rule;
		for (const auto& t : g_data.traffic) if (trafficdata::SameWorld(g_data, t.world, world)) { rule = t; break; }
		int active = 0;
		Slot* free = nullptr;
		for (auto& s : g_slots) {
			if (s.used && now - s.seen > kLostMs) s.used = false;  // core ended it (a fight, the driver got out)
			if (s.used) ++active; else if (!free) free = &s;
		}
		if (!free || active >= rule.max || (g_lastStart && now - g_lastStart < ULONGLONG(rule.gap * 1000.0))) return 0;
		if (!rule.anyVehicle) {
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
		g_lastStart = now;
		Log("FVO traffic: vehicle %08X goes onto route %s, lane %d, %s%s", vehicleId, route->name.c_str(), s.trip.lane, place ? "to " : "a lap and home",
			place ? place->name.c_str() : "");
		out->step = &Step;
		out->user = &s;
		out->name = place ? "route trip to a place" : "route lap";
		return 1;
	}

	// Probes along every route of the player's world (the user: "it just makes it visible to see the route being taken
	// and looks visually appealing"): a probe about every 120 m along each lane, a larger one at each exit and place. Core
	// shows them while "Show traffic routes" is on (routeprobes.h).
	constexpr double kProbeSpacing = 120.0;
	void Markers()
	{
		static ULONGLONG at = 0;
		static std::string sentWorld;
		static unsigned long long sentStamp = ~0ull;
		const ULONGLONG now = GetTickCount64();
		if (!g_setMarkers || (at && now - at < 5000)) return;
		at = now;
		Load();
		char world[201]{};
		if (!g_worldName || !g_worldName(world, sizeof world)) return;
		if (world == sentWorld && g_stamp == sentStamp) return;
		sentWorld = world; sentStamp = g_stamp;
		std::vector<float> m;
		auto add = [&](double x, double y, double z, float scale) { m.push_back(float(x)); m.push_back(float(y)); m.push_back(float(z)); m.push_back(scale); };
		std::string only;  // the route this world's traffic is limited to (route = ...), if any
		for (const auto& t : g_data.traffic) if (trafficdata::SameWorld(g_data, t.world, world)) { only = t.route; break; }
		for (const auto& r : g_data.routes) {
			if (!trafficdata::SameWorld(g_data, r.world, world) || (!only.empty() && r.name != only)) continue;
			const auto l = trafficroute::Build(r);
			const double length = trafficroute::Length(l);
			const int n = std::clamp(int(length / kProbeSpacing), 6, 48);
			for (int lane = 0; lane < r.lanes; ++lane)
				for (int i = 0; i < n + (l.closed ? 0 : 1); ++i) {
					const auto p = trafficroute::At(l, lane, l.Span() * i / n);
					add(p.x, p.y, p.z, 1.0f);
				}
			for (const auto& e : g_data.exits) {
				if (e.route != r.name) continue;
				const auto p = trafficroute::At(l, 0, ExitU(r, l, e.at));
				add(p.x, p.y, p.z, 2.5f);
			}
		}
		for (const auto& p : g_data.places)
			if (trafficdata::SameWorld(g_data, p.world, world)) add(p.at.x, p.at.y, p.at.z + 3.0, 2.5f);
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
