#include "../roadfield.h"
#include "../routecache.h"
#include "../populationinspect.h"
#include "engineport.h"
#include "../trafficinspect.h"
#include <mutex>
#include "slotcookie.h"
#include <set>
#include "routefiles.h"
#include <iomanip>
#include <filesystem>
#include "routeguidance.h"
#include "roadnetwork.h"
#include "planspeed.h"
#include "coordinator.h"
#include "planowner.h"
#include "lanecoord.h"
#include "trafficrank.h"
// FVO_Traffic.dll: the FVO Traffic addon (renamed from FVO_CityPlacements.dll on 2026-10-29, stage 1 of
// notes/TRAFFIC_ADDON_PROPOSAL_20261029.md; the user: "placements is just position of vehicles and traffic will be its own file,
// however if traffic is there it'll use all vehicles in placements or by whatever mod there is"). FVO Placements is now only
// FVO_CityPlacements.esm. This file is the entry and the route side (Atlas + Director to come); generator.cpp builds and keeps
// the traffic pairs (moved from core's trafficpool.cpp) through core's API v2 (traffic_api.h).
// History: Since 2026-10-28 it reads FVO traffic files (trafficdata.h, the user: "a type of mod that only needs a
// certain set of data to read so players can create their own traffic"): every *.ini in Data\SFSE\Plugins\FVO_Traffic\ plus
// the rings the in-game ring editor saves (Logs\FlyingVehicleOverhaul\ringroad.ini, route "ringroad" in its world). The
// generator's population takes trips along those routes (trafficroute.h), turning at junctions under the lane coordinator. Needs
// core FlyingVehicleOverhaul.dll (fvo_addon_api.h); without it, this does nothing.
#include "traffic_api.h"
#include "trafficprofileclient.h"
#include "observations.h"
#include "fvolimits.h"
#include "generator.h"
#include "factorysite.h"
#include "everyday.h"
#include "fvopaths.h"
#include "trafficdata.h"
#include "trafficroute.h"
#include "routeshare.h"
#include "trafficlinks.h"
#include "trafficinitial.h"
#include "modeladmission.h"
#include "liftpath.h"
#include "fieldboxapi.h"
#include "tilebox.h"
#include "spawnflow.h"
#include "../trafficcensusapi.h"
#include <limits>
#include <optional>

// Steam Starfield 1.16.244 (sfse_common/sfse_version.h: RUNTIME_VERSION_1_16_244 = MAKE_EXE_VERSION(1, 16, 244)).
constexpr unsigned kRuntime_1_16_244 = 0x01100F40;
#include "sfse/PluginAPI.h"

#include <Windows.h>
#include <algorithm>
#include <random>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include "roomrule.h"  // RR-1
#include <sstream>
#include <string>
#include <vector>

namespace
{
	std::recursive_mutex g_journeyMutex;
	constexpr int kSlots = fvolimits::kVehicles;  // trips at once: one per NPC pilot core can drive (fvolimits.h)
	constexpr ULONGLONG kLostMs = 3000;          // a trip core stopped stepping (fight, driver out) is over after this
	constexpr double kMinTripDistance = 150.0;   // m: a destination this close to the start is not a trip
	// The New Atlantis world (ringroad.ini's section for the user's ring); the starter file's places are there.
	constexpr char kNewAtlantis[] = "JemisonPlanetWorld157133511011091040";

	FvoLog_t g_log = nullptr;
	FvoWorldName_t g_worldName = nullptr;
	FvoBaseOf_t g_baseOf = nullptr;
	FvoRefHeading_t g_refHeading = nullptr;  // optional core export: Cruise's lane pick by direction
	FvoSetRouteMarkers_t g_setMarkers = nullptr;
	FvoRefPos_t g_refPos = nullptr;
	const FvoTrafficApi* g_trafficApi = nullptr;  // core API v2 (null: core too old; no traffic pairs)
    FvoObservedModelProviderV1 g_observedModel=nullptr;
    FvoObservedModelProviderV1 g_storedModel=nullptr;
    int (*g_outOfTraffic)(const unsigned*,int)=nullptr;double g_outOfTrafficAt=-1;  // C4: core export FVO_VehiclesOutOfTrafficV1 (null: not exported)
    planowner::SnapshotV1 g_ownerSnapshot=nullptr;  // C2: the core's one owner/rank record, read-only (null: junctions see traffic only)
    double g_coordLogAt=-1;
    struct InitialSiteOutput { FvoPoolSpot pose{}; unsigned base=0; };
    std::vector<InitialSiteOutput> g_initialSiteOutput;
    std::uint64_t g_initialPlanGeneration=0;
    using FvoNavWant_t=void(*)(double x,double y);
    FvoNavWant_t g_navWant=nullptr;
    bool g_corridorSupported=false;bool g_fieldSupported=false;
    bool g_coordinatorSupported=false;
    bool g_travelRulesSupported=false;
	FvoLocalTime_t g_localTime = nullptr;
	FvoSetWorldShift_t g_setShift = nullptr;
    std::vector<trafficinitial::Slot> g_initialSlots;
    // SR-1 (2026-11-07): the trip claim says why it refused (a fact; who gets a trip is unchanged). Per vehicle, the last refusal;
    // read by the generator (Health's input, SR-2) and traced once per change (traffic.trip).
    struct TripRefusalFact { const char* why = ""; int on = -1, want = -1; };
    std::map<unsigned, TripRefusalFact> g_tripRefusal;
    inline int RefuseTrip(unsigned vehicle, const char* why, int on = -1, int want = -1) { if (vehicle) g_tripRefusal[vehicle] = { why, on, want }; return 0; }
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
		return trafficprofileclient::Routes();
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
		const std::wstring ini = trafficprofileclient::File(L"ringroad.ini");
		if (ini.empty()) return out;
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
    factorysite::Config g_factoryConfig;
    trafficdata::Data g_factoryGeometry;
    FvoSpotMotionProofQueryV1 g_factoryProof = nullptr;
    FvoTrafficFactoryLeaseFnV1 g_factoryLease = nullptr;
	unsigned long long g_stamp = ~0ull;
	// Anchored worlds found in game: world name -> (the player's world then, the shift). Kept across file reloads; measured
	// again when the player is in another world (a new landing lays the city down again).
	struct Found { std::string world; trafficdata::Vec shift; };
	std::map<std::string, Found> g_found;

	void Rebuild()
	{
		g_data = g_read;
		for (const auto& [name, f] : g_found) trafficdata::PlaceWorld(g_data, name, f.world, f.shift);
        trafficdata::ApplyLaneDefaults(g_data);
	}

	// Looks for each anchored world's references in the player's world (a few times a minute; cheap lookups).
	void FindAnchors()
	{
		if (!g_refPos || g_read.anchors.empty()) return;
		char world[201]{};
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return;
		bool changed = false;
		for (const auto& [name, refs] : g_read.anchors) {
			const auto it = g_found.find(name);
			if (it != g_found.end() && it->second.world == world) continue;
			for (const auto& a : refs) {
				double p[3];
				if (!engineport::FunctionForward(g_refPos,"refPosition","routes",0u,a.id, p)) continue;
				const trafficdata::Vec shift{ p[0] - a.at.x, p[1] - a.at.y, p[2] - a.at.z };
				g_found[name] = { world, shift };
				changed = true;
				Log("FVO traffic: [world.%s] found in %s by anchor %08X: shift (%.2f, %.2f, %.2f)", name.c_str(), world, a.id, shift.x, shift.y, shift.z);
				break;
			}
		}
		if (changed) { Rebuild(); ++g_stamp; }
		// The map route editor (core) needs the shifts to draw and edit these routes; sent every pass (cheap, idempotent).
		if (g_setShift) for (const auto& [name, f] : g_found) engineport::FunctionForward(g_setShift,"setWorldShift","routes",0u,name.c_str(), f.world.c_str(), f.shift.x, f.shift.y, f.shift.z);
	}
	// Read on demand (2026-10-29 audit, the user: "we dont need traffic file to be checked so often when we can just have an update
	// button in the menu"): the folder was scanned every ~2 s (each everyday pick, each Auto route plan, the 5 s marker tick). Now
	// once at start, then only when asked: the settings menu's "Reload traffic files", or the map route editor after it saves.
	std::atomic<bool> g_reloadWanted{ true };
	std::wstring g_loadedProfile;
	void RequestReload() { g_reloadWanted.store(true); }

    // Route and skip lists hold form ids with the load-order index of the day they were written. trafficidmap.ini (optional) maps a hex prefix
    // to its index now, e.g. 08=07 or FE009=FE008, while plugins before them are off. Files are never rewritten; delete it when the plugins return.
    struct IdMap{unsigned from=0,to=0,shift=0;};
    std::vector<IdMap> g_idMap;
    unsigned MapStoredId(unsigned id){for(const auto& m:g_idMap)if((id>>m.shift)==m.from)return (m.to<<m.shift)|(id&((1u<<m.shift)-1));return id;}
    void LoadIdMap(){
        g_idMap.clear();wchar_t buf[2048]{};
        GetPrivateProfileSectionW(L"map",buf,2048,fvopaths::File(L"trafficidmap.ini").c_str());
        for(const wchar_t* row=buf;*row;row+=wcslen(row)+1){const wchar_t* eq=wcschr(row,L'=');if(!eq)continue;
            const std::size_t digits=std::size_t(eq-row);if(!digits||digits>7||wcslen(eq+1)!=digits)continue;
            IdMap m;m.from=unsigned(std::wcstoul(std::wstring(row,digits).c_str(),nullptr,16));m.to=unsigned(std::wcstoul(eq+1,nullptr,16));m.shift=unsigned(8-digits)*4;g_idMap.push_back(m);
            Log("OBS system=traffic.spawn vehicle=00000000 event=id_map from=%X to=%X digits=%zu",m.from,m.to,digits);}
        trafficdata::idAdapter=g_idMap.empty()?nullptr:&MapStoredId;generator::SetIdAdapter(trafficdata::idAdapter);
    }
	void Load()
	{
		trafficprofileclient::Guard trafficFilesLock;
		if (!trafficFilesLock) return;
        std::lock_guard journeyLock(g_journeyMutex);
		const std::wstring dir = TrafficDir();
		if (dir.empty()) { g_reloadWanted = true; return; }
		if (dir != g_loadedProfile) { g_loadedProfile = dir; g_stamp = ~0ull; g_found.clear(); g_reloadWanted = true; }
		if (!g_reloadWanted.exchange(false)) return;
		const ULONGLONG now = GetTickCount64();
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
		if (GetFileAttributesExW(trafficprofileclient::File(L"ringroad.ini").c_str(), GetFileExInfoStandard, &ra))
			stamp = stamp * 31 + (static_cast<unsigned long long>(ra.ftLastWriteTime.dwHighDateTime) << 32 | ra.ftLastWriteTime.dwLowDateTime);
		if (stamp == g_stamp) return;
		g_stamp = stamp;
		trafficdata::Data d;
		std::sort(files.begin(), files.end());
        g_factoryConfig = {};
        g_factoryGeometry = {};
        for (const auto& f : files) {
            const auto text = ReadFile(dir + L"\\" + f);
            if (_wcsicmp(f.c_str(), L"FactoryBay.ini") == 0) {
                const auto site = factorysite::Parse(text);
                if (site.recognized) {
                    if (site.enabled) {
                        g_factoryConfig = site.config;
                        trafficdata::Read(text, Narrow(f), g_factoryGeometry, true);
                    } else if (site.reason) Log("OBS system=traffic.factory_site event=inactive reason=%s", site.reason);
                    continue;
                }
            }
            trafficdata::Read(text, Narrow(f), d);
        }
		for (const auto& [world, ring] : Ringroad()) trafficdata::AddRingroad(world, ring, d);
		trafficdata::PreferEditor(d);trafficdata::ApplyLaneDefaults(d);  // routes edited in the map route editor replace the originals of the same name
		g_read = std::move(d);
		Rebuild();
		Log("FVO traffic: %d file(s) in %s: %zu route(s), %zu exit(s), %zu place(s), %zu traffic rule(s)", g_data.files, Narrow(dir).c_str(),
			g_data.routes.size(), g_data.exits.size(), g_data.places.size(), g_data.traffic.size());
		for (const auto& p : g_data.problems) Log("FVO traffic: skipped %s", p.c_str());
	}

    struct ScriptStop { trafficdata::Vec at;double dwell=0;std::vector<std::string> routes; };
    enum class FactoryRoutePhase { None, Test, Departing, Arrival, StockReserved, StockCommitted };
    struct FactoryRoute {
        generator::FactoryBay bay;
        trafficdata::Route lane;
        trafficroute::Line line;
        trafficlinks::Junction exit;
        std::string world;
        int privateLane = -1;
        double arrivalU = 0;
    };
    std::optional<FactoryRoute> g_factoryRoute;
    std::atomic<std::uint64_t> g_journeyGeneration{0};
	struct Slot {
        std::uint64_t generation=g_journeyGeneration.fetch_add(1,std::memory_order_relaxed)+1;
        trafficdata::Route spec;std::vector<trafficdata::Route> plannedRoutes;std::vector<trafficdata::Link> plannedLinks;bool networkSnapshot=false;
        roadnetwork::Itinerary itinerary;
        unsigned heldWhyLogged=0;  // last traffic.lane_hold reasons logged
        int travelLogged=-9;const char* travelWhyLogged="";  // last traffic.travel kind logged
        const char* liftWhyLogged="";  // last traffic.lift rules choice logged
        placeaccess::Plan accessPlan,manoeuvrePlan;bool placeApproach=false,placePath=false,pickupWait=false;unsigned scriptSignal=0;const char* scriptReason="following journey";journeyplan::Plan authoredJourney;std::vector<ScriptStop> script;std::size_t scriptAt=0;double scriptDwell=0;bool scriptHolding=false;
        std::size_t itineraryAt=0;
        FactoryRoutePhase factoryPhase=FactoryRoutePhase::None;
        std::string factoryDestination;
        FvoPoolSpot factoryArrival{};
        double factoryArrivalU=0;
        int stockRoadSlot=-1;
        std::uint64_t stockRouteStamp=0;
        double stepAt=0,measuredSpeed=0,controlStopped=0;
        trafficdata::Vec stepVelocity;
        int completedControl=-1;
        FvoLaneGuidanceV1 guidance;
		bool used = false;
		bool repeat = false;
        unsigned vehicleId = 0, baseId=0;
		trafficroute::Line line;
		trafficroute::Trip trip;
        std::vector<trafficroute::Vec> turn;std::size_t turnAt=0;
        std::string turnFrom;trafficdata::Route turnSpec;trafficroute::Line turnFromLine;int turnFromLane=0;routepolicy::Section turnRule;
		double speed = 25;
		ULONGLONG seen = 0;
		trafficroute::Leg logged = trafficroute::Leg::Climb;
		std::string route, dest, world;
		// Its landing area (2026-10-30, occupancy rules): the area and space it holds, waiting above a full area (queue), how long it stays parked.
		std::string place;
		int space = -1;
		bool queue = false;
		double stay = 0;
		ULONGLONG stayUntil = 0;
		double x = 0, y = 0, z = 0;      // where its vehicle was last step (the lane slot check)
		bool holding = false;bool connectionBlocked=false;
		// Junctions (2026-10-29): the route the trip belongs to, where it left that route for a branch, and the junctions of the
		// route it is on now. `route` is the one being flown (vehicle counts follow it).
		std::string home;
		int homeLane = 0;
		double leftU = 0, leftTravelled = 0, homeTarget = 0;
		std::vector<trafficlinks::Junction> junctions;
		std::uint64_t dice = 0;
		std::map<std::string,int> cruisePicks;  // Cruise only: picks per junction option this cruise (reset with the slot)
		double flown = 0;                // m flown since the trip began (a trip that ends without flying was no trip)
		bool flownStart = false;
	};
	Slot g_slots[kSlots];
    bool StockRoutePending(const Slot& s) {
        return s.factoryPhase==FactoryRoutePhase::StockReserved||s.factoryPhase==FactoryRoutePhase::StockCommitted;
    }
    roadnetwork::Network g_road;
    roadnetwork::Arbiter g_rightOfWay;
    unsigned long long g_roadStamp=~0ull;
    std::string g_roadWorld;
    std::map<unsigned,std::pair<double,bool>> g_roadDecision;double g_roadDecisionAt=-1;
    using ApproachKey=std::pair<int,unsigned>;
    std::map<ApproachKey,roadnetwork::ApproachTiming> g_approachTiming;
    std::map<ApproachKey,double> g_otherApproachDistance;
    const trafficdata::Route& Spec(Slot& s) {
        if(s.spec.name!=s.route)for(const auto& r:g_data.routes)if(r.name==s.route){s.spec=r;break;}
        return s.spec;
    }
    // Where the car is along its lane. Before it has joined, in 3D: in a vertical shaft every height shares one x, y, so a 2D answer
    // landed anywhere up the shaft and the car was told to climb toward it (2026-11-06 15:24: rose to z -117 while joining, circling).
    double CarProgress(const Slot& s){return s.trip.uKnown?s.trip.lastU:trafficroute::NearestProgress(s.line,s.trip.lane,s.x,s.y,s.z,false,0.0);}
    // Natural travel for a "both" tile. Steepness = rise over horizontal run of the tile's two markers.
    routepolicy::Travel NaturalTravel(const trafficdata::Route& r,const trafficroute::Line& line,double u,const char*& why){
        const int n=int(r.points.size());if(n<2){why="natural_both";return routepolicy::Travel::Both;}
        const int t=std::clamp(int(line.ToSource(u)),0,n-1);const auto& a=r.points[std::size_t(t)];const auto& b=r.points[std::size_t((t+1)%n)];
        if(r.road.enabled&&roadplan::PrimitiveAt(r.road,std::size_t(t)).kind==roadplan::Open){why="open_air_lift";return routepolicy::Travel::Hover;}
        if(liftpath::IsLift(r,t)){why="steep_climb_lift";return routepolicy::Travel::Hover;}
        if(r.road.enabled){why="road_tile";return routepolicy::Travel::Drive;}
        why="natural_both";return routepolicy::Travel::Both;
    }
    routepolicy::Section SectionAt(Slot& s,double u) {
        const auto& r=Spec(s);
        const auto section=routepolicy::Segment(r.points.size(),r.closed,s.line.ToSource(u),s.trip.lane);
        auto out=routepolicy::Resolve(r.travel,r.sections,section);
        if(r.lanePlan.enabled){const auto settings=laneplan::Resolve(r.lanePlan,r.ring?-1:int(section),s.trip.lane);if(settings.travel!=routepolicy::Travel::Inherit)out.travel=settings.travel;}
        return out;
    }

    std::map<unsigned,Slot> g_rememberedJourneys;
    std::wstring g_journeyProfile;bool g_journeyWritable=true;
    double g_signalSeconds=0,g_signalLastTick=0;
    void WriteVector(std::ostream& out,trafficdata::Vec v){out<<v.x<<' '<<v.y<<' '<<v.z<<' ';}
    bool ReadVector(std::istream& in,trafficdata::Vec& v){return bool(in>>v.x>>v.y>>v.z)&&std::isfinite(v.x)&&std::isfinite(v.y)&&std::isfinite(v.z);}
    std::string HexText(const std::string& text){const char* digits="0123456789ABCDEF";std::string out;out.reserve(text.size()*2);for(unsigned char c:text){out.push_back(digits[c>>4]);out.push_back(digits[c&15]);}return out;}
    bool UnhexText(const std::string& text,std::string& out){if(text.size()%2||text.size()>4000000)return false;auto digit=[](char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:-1;};for(std::size_t i=0;i<text.size();i+=2){const int a=digit(text[i]),b=digit(text[i+1]);if(a<0||b<0)return false;out.push_back(char(a*16+b));}return true;}
    routefiles::Route ArchiveRoute(const trafficdata::Route& r){routefiles::Route out;out.world=r.world;out.world=r.world;out.id=r.name;out.label=r.label;out.ring=r.ring;out.shape=r.shape;out.tilt=r.tilt;out.tiltToward=r.tiltToward;out.points=r.points;out.rounding=r.rounding;out.closed=r.closed;out.lanePlan=r.lanePlan;out.spacePlan=r.spacePlan;out.road=r.road;out.journey=r.journey;out.restrictions=r.restrictions;out.lanes=r.lanes;out.speed=r.speed;out.width=r.width;out.schedule=r.schedule;out.color=r.color;out.exclude=r.exclude;out.role=r.role;out.group=r.group;out.travel=r.travel;out.sections=r.sections;out.corridor=r.corridor;out.end=r.end;return out;}
    void CommitNetwork(Slot& slot,const roadnetwork::Network& network){slot.plannedRoutes.clear();slot.plannedLinks.clear();slot.networkSnapshot=true;for(const auto& route:network.routes)if(std::find(slot.itinerary.routes.begin(),slot.itinerary.routes.end(),route.name)!=slot.itinerary.routes.end())slot.plannedRoutes.push_back(route);for(const auto& link:g_data.links){const bool a=std::any_of(slot.plannedRoutes.begin(),slot.plannedRoutes.end(),[&](const auto& r){return r.name==link.from;}),b=std::any_of(slot.plannedRoutes.begin(),slot.plannedRoutes.end(),[&](const auto& r){return r.name==link.to;});if(a&&b)slot.plannedLinks.push_back(link);}}
    std::string ArchiveNetwork(const Slot& slot){if(!slot.networkSnapshot)return {};routefiles::File file;file.worldId=slot.world;file.traffic=false;for(const auto& route:slot.plannedRoutes)file.routes.push_back(ArchiveRoute(route));for(const auto& link:slot.plannedLinks)file.links.push_back({link.name,link.from,link.to,link.at,link.toEnd,link.share,link.toAt,link.radius,link.type,link.fromLane,link.toLane});return HexText(routefiles::Format(file));}
    std::string JourneyRoute(Slot& slot){
        const auto& r=Spec(slot);routefiles::File file;file.worldId=slot.world.empty()?"saved_world":slot.world;routefiles::Route out;
        out.id=r.name;out.label=r.label;out.ring=r.ring;out.shape=r.shape;out.tilt=r.tilt;out.tiltToward=r.tiltToward;out.points=r.points;out.rounding=r.rounding;out.closed=r.closed;
        out.restrictions=r.restrictions;out.journey=slot.authoredJourney;out.lanePlan=r.lanePlan;out.spacePlan=r.spacePlan;out.road=r.road;out.lanes=r.lanes;out.speed=r.speed;out.width=r.width;out.schedule=r.schedule;out.color=r.color;out.exclude=r.exclude;out.role=r.role;out.group=r.group;out.travel=r.travel;out.sections=r.sections;out.corridor=r.corridor;out.end=r.end;
        file.routes.push_back(out);return routefiles::Format(file)+"\n; journey_signal = "+std::to_string(slot.scriptSignal)+"\n; place_access = "+placeaccess::Write(slot.accessPlan)+"\n; manoeuvre_plan = "+placeaccess::Write(slot.manoeuvrePlan)+"\n; place_approach = "+std::to_string(slot.placeApproach)+" "+std::to_string(slot.placePath)+" "+std::to_string(slot.pickupWait)+"\n; committed_network = "+ArchiveNetwork(slot)+"\n";
    }
    void WriteJourney(std::ostream& out,Slot& s){
        out<<s.vehicleId<<' '<<s.baseId<<' '<<std::quoted(s.world)<<' '<<std::quoted(s.route)<<' '<<std::quoted(s.home)<<' '<<std::quoted(s.dest)<<' '<<std::quoted(s.place)<<' '<<std::quoted(JourneyRoute(s))<<' ';
        out<<s.repeat<<' '<<int(s.trip.leg)<<' '<<s.trip.lane<<' '<<s.trip.lap<<' '<<s.trip.targetU<<' '<<s.trip.lastU<<' '<<s.trip.travelled<<' '<<s.trip.uKnown<<' ';
        WriteVector(out,s.trip.home);WriteVector(out,s.trip.dest);out<<s.trip.laneZ<<' '<<s.x<<' '<<s.y<<' '<<s.z<<' '<<s.speed<<' '<<s.flown<<' '<<s.homeLane<<' '<<s.leftU<<' '<<s.leftTravelled<<' '<<s.homeTarget<<' '<<s.space<<' '<<s.queue<<' '<<s.stay<<' ';
        out<<s.itineraryAt<<' '<<s.itinerary.routes.size()<<' ';for(std::size_t i=0;i<s.itinerary.routes.size();++i)out<<std::quoted(s.itinerary.routes[i])<<' '<<s.itinerary.entries[i]<<' '<<s.itinerary.exits[i]<<' '<<s.itinerary.lanes[i]<<' ';
        out<<s.scriptAt<<' '<<s.script.size()<<' ';for(const auto& stop:s.script){WriteVector(out,stop.at);out<<stop.dwell<<' '<<stop.routes.size()<<' ';for(const auto& route:stop.routes)out<<std::quoted(route)<<' ';}
        out<<s.turnAt<<' '<<s.turn.size()<<' ';for(auto p:s.turn)WriteVector(out,p);out<<' '<<std::quoted(s.turnFrom)<<' '<<s.turnFromLane<<' ';if(!s.turnFrom.empty()){Slot from;from.route=s.turnFrom;from.world=s.world;from.spec=s.turnSpec;out<<std::quoted(JourneyRoute(from))<<' ';}else out<<std::quoted(std::string())<<' ';
        out<<(s.scriptDwell?std::max(0.0,GetTickCount64()*0.001-s.scriptDwell):-1.0)<<' '<<(s.stayUntil?std::max(0.0,double(s.stayUntil)-double(GetTickCount64())):0.0)<<' '<<s.completedControl<<'\n';
    }
    bool ReadJourney(std::istream& in,Slot& s){
        std::string routeText;int leg=0;
        if(!(in>>s.vehicleId>>s.baseId>>std::quoted(s.world)>>std::quoted(s.route)>>std::quoted(s.home)>>std::quoted(s.dest)>>std::quoted(s.place)>>std::quoted(routeText)>>s.repeat>>leg>>s.trip.lane>>s.trip.lap>>s.trip.targetU>>s.trip.lastU>>s.trip.travelled>>s.trip.uKnown))return false;
        if(!s.vehicleId||leg<0||leg>4||s.trip.lane<0||s.trip.lane>=laneplan::kMaxLanes||routeText.size()>2000000)return false;
        trafficdata::Data data;trafficdata::Read(routeText,"saved_journey.ini",data);if(!data.problems.empty()||data.routes.size()!=1)return false;{const auto marker=routeText.find("; journey_signal = ");if(marker!=std::string::npos){std::istringstream signal(routeText.substr(marker+19));unsigned value=0;if(signal>>value&&value<=65535)s.scriptSignal=value;}}
        {const auto marker=routeText.find("; place_access = ");if(marker!=std::string::npos){const auto begin=marker+17;const auto end=routeText.find('\n',begin);s.accessPlan=placeaccess::Read(routeText.substr(begin,end-begin));}const auto active=routeText.find("; manoeuvre_plan = ");if(active!=std::string::npos){const auto begin=active+19;const auto end=routeText.find('\n',begin);s.manoeuvrePlan=placeaccess::Read(routeText.substr(begin,end-begin));}const auto stage=routeText.find("; place_approach = ");if(stage!=std::string::npos){std::istringstream flags(routeText.substr(stage+19));flags>>s.placeApproach>>s.placePath;flags>>s.pickupWait;}}
        {const auto marker=routeText.find("; committed_network = ");if(marker!=std::string::npos){const auto begin=marker+22;const auto end=routeText.find('\n',begin);const auto encoded=routeText.substr(begin,end-begin);if(!encoded.empty()){std::string decoded;if(!UnhexText(encoded,decoded))return false;trafficdata::Data archive;trafficdata::Read(decoded,"committed_network.ini",archive);if(!archive.problems.empty()||archive.routes.size()>128||archive.links.size()>512)return false;s.plannedRoutes=std::move(archive.routes);s.plannedLinks=std::move(archive.links);s.networkSnapshot=true;}}}
        s.spec=data.routes.front();s.authoredJourney=s.spec.journey;s.spec.name=s.route;s.line=trafficroute::Build(s.spec);s.trip.leg=trafficroute::Leg(leg);
        if(!trafficroute::HasLane(s.line,s.spec.lanes,s.trip.lane))return false;
        if(!ReadVector(in,s.trip.home)||!ReadVector(in,s.trip.dest))return false;
        if(!(in>>s.trip.laneZ>>s.x>>s.y>>s.z>>s.speed>>s.flown>>s.homeLane>>s.leftU>>s.leftTravelled>>s.homeTarget>>s.space>>s.queue>>s.stay))return false;
        std::size_t count=0;if(!(in>>s.itineraryAt>>count)||count>128)return false;
        for(std::size_t i=0;i<count;++i){std::string route;double entry=0,exit=0;int lane=0;if(!(in>>std::quoted(route)>>entry>>exit>>lane)||lane<0||lane>=laneplan::kMaxLanes||!std::isfinite(entry)||!std::isfinite(exit))return false;s.itinerary.routes.push_back(route);s.itinerary.entries.push_back(entry);s.itinerary.exits.push_back(exit);s.itinerary.lanes.push_back(lane);}
        if(count&&s.itineraryAt>=count)return false;
        if(!(in>>s.scriptAt>>count)||count>128)return false;for(std::size_t i=0;i<count;++i){ScriptStop stop;std::size_t routes=0;if(!ReadVector(in,stop.at)||!(in>>stop.dwell>>routes)||routes>128||!std::isfinite(stop.dwell)||stop.dwell<0)return false;for(std::size_t j=0;j<routes;++j){std::string route;if(!(in>>std::quoted(route)))return false;stop.routes.push_back(route);}s.script.push_back(stop);}
        if(count&&s.scriptAt>=count)return false;
        if(!(in>>s.turnAt>>count)||count>4096||s.turnAt>count)return false;for(std::size_t i=0;i<count;++i){trafficdata::Vec p;if(!ReadVector(in,p))return false;s.turn.push_back(p);}
        for(double v:{s.trip.targetU,s.trip.lastU,s.trip.travelled,s.trip.laneZ,s.x,s.y,s.z,s.speed,s.flown,s.leftU,s.leftTravelled,s.homeTarget,s.stay})if(!std::isfinite(v))return false;
        std::string fromText;if(!(in>>std::quoted(s.turnFrom)>>s.turnFromLane>>std::quoted(fromText))||s.turnFromLane<0||s.turnFromLane>=laneplan::kMaxLanes)return false;
        if(!s.turnFrom.empty()){trafficdata::Data old;trafficdata::Read(fromText,"saved_turn.ini",old);if(old.routes.size()!=1||!old.problems.empty())return false;s.turnSpec=old.routes[0];s.turnFromLine=trafficroute::Build(old.routes[0]);const double u=trafficroute::Nearest(s.turnFromLine,s.turnFromLane,s.x,s.y);s.turnRule=routepolicy::Resolve(old.routes[0].travel,old.routes[0].sections,routepolicy::Segment(old.routes[0].points.size(),old.routes[0].closed,s.turnFromLine.ToSource(u),s.turnFromLane));}
        if(!s.turnFrom.empty()&&!trafficroute::HasLane(s.turnFromLine,s.turnSpec.lanes,s.turnFromLane))return false;
        double dwell=-1,stayRemaining=0;if(!(in>>dwell>>stayRemaining>>s.completedControl)||!std::isfinite(dwell)||!std::isfinite(stayRemaining)||dwell>86400||stayRemaining>86400000)return false;
        if(dwell>=0){s.scriptDwell=GetTickCount64()*0.001-dwell;s.scriptHolding=true;}if(stayRemaining>0)s.stayUntil=GetTickCount64()+ULONGLONG(stayRemaining);
        s.used=true;s.flownStart=true;return true;
    }
    int SaveJourneys(){
        trafficprofileclient::Guard lock;if(!lock)return 0;std::lock_guard journeyLock(g_journeyMutex);if(g_journeyProfile.empty())return 1;if(!g_journeyWritable)return 0;
        const auto target=std::filesystem::path(g_journeyProfile)/L"journeys.state";auto temp=target;temp+=L".pending";
        auto records=g_rememberedJourneys;for(auto& s:g_slots)if(s.used&&s.vehicleId&&s.factoryPhase==FactoryRoutePhase::None)records[s.vehicleId]=s;
        std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.precision(17);out<<"FVO_JOURNEYS 1 "<<records.size()<<' '<<std::quoted(g_roadWorld)<<' '<<g_signalSeconds<<'\n';for(auto& [id,s]:records){(void)id;WriteJourney(out,s);}out.flush();if(!out)return 0;out.close();
        return MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)?1:0;
    }
    void LoadJourneys(const std::wstring& profile){
        std::lock_guard journeyLock(g_journeyMutex);
        if(profile==g_journeyProfile)return;g_journeyProfile=profile;g_journeyWritable=true;g_rememberedJourneys.clear();
        const auto path=std::filesystem::path(profile)/L"journeys.state";std::error_code error;const auto bytes=std::filesystem::file_size(path,error);if(error){if(error!=std::errc::no_such_file_or_directory)g_journeyWritable=false;return;}
        if(bytes>64*1024*1024){g_journeyWritable=false;Log("FVO traffic: journey state too large; keeping legacy route admission");return;}
        std::ifstream in(path,std::ios::binary);std::string magic,world,line;std::getline(in,line);std::istringstream header(line);unsigned version=0,count=0;if(!(header>>magic>>version>>count>>std::quoted(world))||magic!="FVO_JOURNEYS"||version!=1||count>4096){g_journeyWritable=false;return;}double seconds=0;if(header>>seconds){if(!std::isfinite(seconds)||seconds<0){g_journeyWritable=false;return;}}g_signalSeconds=seconds;g_signalLastTick=0;
        std::map<unsigned,Slot> loaded;for(unsigned i=0;i<count;++i){Slot s;if(!ReadJourney(in,s)||loaded.count(s.vehicleId)){g_journeyWritable=false;Log("FVO traffic: invalid journey state retained on disk; not resuming saved journeys");return;}loaded[s.vehicleId]=std::move(s);}g_rememberedJourneys=std::move(loaded);
    }

    bool SavedFleetJourney(unsigned vehicle,unsigned base,const char* world){
        std::lock_guard lock(g_journeyMutex);
        const auto found=g_rememberedJourneys.find(vehicle);
        if(found==g_rememberedJourneys.end())return false;
        const auto& journey=found->second;
        return base&&journey.baseId==base&&journey.world==world&&
            journey.dest!="your marker"&&journey.dest!="cruise";
    }

	// Trips that end where they started (2026-10-30 18:42 run, Neon: vehicles at the end of the editor's 40-60 m feeder routes got a lap,
	// were 'done' in the same step and got the same route back every 3 s, never moving). That route is skipped for that vehicle for
	// kEmptyTripMs, so it takes another route in reach (or stays parked) instead of the same empty trip.
	constexpr double kEmptyTripFlown = 30.0;
	constexpr ULONGLONG kEmptyTripMs = 120000;
	std::map<unsigned, std::pair<std::string, ULONGLONG>> g_emptyTrips;
	bool EmptyHere(unsigned vehicle, const std::string& route)
	{
		if (!vehicle) return false;
		const auto it = g_emptyTrips.find(vehicle);
		return it != g_emptyTrips.end() && it->second.first == route && GetTickCount64() - it->second.second < kEmptyTripMs;
	}
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
                // A stopped car away from the join point is not about to take it; it may be queued on the joiner itself
                // (2026-11-06 15:47: Cruise rejoining in the shaft waited on FF100029 12 m below, which queued behind it).
                if(o.measuredSpeed<0.5&&std::hypot(o.x-join.x,o.y-join.y,o.z-join.z)>8.0){
                    static std::uint64_t loggedPair=0;static ULONGLONG loggedAt=0;const std::uint64_t pair=(std::uint64_t(me.vehicleId)<<32)|o.vehicleId;const ULONGLONG now=GetTickCount64();
                    if(pair!=loggedPair||now-loggedAt>5000){loggedPair=pair;loggedAt=now;
                        Log("OBS system=traffic.join vehicle=%08X event=stopped_not_blocking other=%08X distance=%.1f",me.vehicleId,o.vehicleId,std::hypot(o.x-join.x,o.y-join.y,o.z-join.z));}
                    continue;}
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
    int g_constraintMinute=-1;
	bool LocalMinute(int& minute)
	{
		double h = 0;
		if (!g_localTime || !engineport::FunctionForward(g_localTime,"localTime","routes",0u,&h) || !(h >= 0 && h <= 24)){g_constraintMinute=-1;return false;}
		minute = std::clamp(int(h * 60.0), 0, routeschedule::kMinutesPerDay - 1);g_constraintMinute=minute;
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
	// Vehicle rules (2026-10-30): a route's exclude list keeps those models off it. A vehicle of unknown base is let on.
	bool Excluded(const trafficdata::Route& r, unsigned vehicleId,int lane=-1)
	{
		if (!vehicleId || !g_baseOf) return false;
		const unsigned base = engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId);
		if(!base)return false;
        if(lane>=0)return trafficdata::LaneLeftOut(g_data.groups,r,lane,base);
        if(trafficdata::LeftOut(g_data.groups,r,base))return true;
        if(r.lanePlan.vehicles.lanes.empty()&&r.lanePlan.vehicles.unsupported.empty())return false;
        for(int id=0;id<laneplan::kMaxLanes;++id){bool active=r.lanePlan.enabled?laneplan::Active(r.lanePlan.base,id):id<std::min(2,r.lanes);
            if(r.lanePlan.enabled)for(const auto& entry:r.lanePlan.overrides)active=active||laneplan::Active(laneplan::Resolve(r.lanePlan,entry.section,id),id);
            if(active&&!trafficdata::LaneLeftOut(g_data.groups,r,id,base))return false;
        }return true;
	}
	// City settings (2026-10-30, Traffic Manager V1): this world's traffic on or off, density, vehicle limit, original routes.
	trafficdata::City CityNow()
	{
		if (g_data.cities.empty()) return {};
		char world[201]{};
		return g_worldName && engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world) ? trafficdata::CityOf(g_data, world) : trafficdata::City{};
	}
	// PF-2 (2026-11-07, user: "I change the route and save but it doesnt save ... it says finding traffic", "Yes, fix it"): a route the user put in
	// the shared Permanent_<hash>[_live].ini with Permanent Save is the user's own route, like the editor file's, so "Original routes: Leave out"
	// no longer leaves it out. Built-in route files are still left out. Traced once per route.
	bool OwnRouteFile(const std::string& file) { std::string f = file; for (auto& c : f) c = char(std::tolower(static_cast<unsigned char>(c))); return trafficdata::EditorFile(file) || f.rfind("permanent_", 0) == 0; }
	bool CityFlies(const trafficdata::Route& r, const trafficdata::City& city) {
		if (!city.on) return false; if (city.originals || trafficdata::EditorFile(r.file)) return true;
		if (!OwnRouteFile(r.file)) return false;
		static std::mutex m; static std::set<std::string> told; { std::lock_guard lk(m); if (told.insert(r.file + "|" + r.name).second)
			Log("OBS system=traffic.routes event=permanent_route_flies route=%s file=%s originals=0 reason=permanent_save_is_users_own_route", r.name.c_str(), r.file.c_str()); }
		return true;
	}
	// Cargo flies slower (route roles).
	double RoleSpeed(const trafficdata::Route& r) { return r.role == "cargo" ? r.speed * 0.8 : r.speed; }
	// `on` / `want`: the vehicles on the route now and its count now (the fair share's inputs, routeshare.h).
    bool RevisionReady(const trafficdata::Route& route);
	// RR-1 (2026-11-07): why names the refusal (a fact; who gets room is unchanged). Optional: callers passing nothing are unchanged.
	bool Room(const trafficdata::Route& r, const trafficdata::Traffic& rule, const Slot* except, bool pool, int* on = nullptr, int* want = nullptr, const char** why = nullptr)
	{
		// RR-1: the facts are gathered in the same order as before (later ones only when the earlier allow) and roomrule::Decide names the answer.
		roomrule::Facts f;
		auto answer = [&] { const char* w = roomrule::Decide(f); if (why) *why = w; return roomrule::Has(w); };
		int minute = 0;
		const bool known = LocalMinute(minute);
		const auto d = routeschedule::Evaluate(r.schedule, known, minute, r.trafficMetres);
		f.timeKnown = d.known; f.active = d.active;
		if (!f.timeKnown || !f.active) return answer();
		const auto city = CityNow();
		f.count = trafficdata::Scaled(d.vehicles, city);
		f.cityFlies = CityFlies(r, city);
		if (!f.cityFlies || f.count <= 0) return answer();
		f.revisionReady = RevisionReady(r);
		if (!f.revisionReady) return answer();
		for (const auto& o : g_slots) {
			if (!o.used || &o == except || o.dest == "your marker" || o.factoryPhase==FactoryRoutePhase::Test) continue;
			++f.all;
			if ((o.factoryPhase==FactoryRoutePhase::Departing ? o.factoryDestination : o.route) == r.name) ++f.onRoute;
			if (o.route.rfind("ed_", 0) != 0 && o.dest != "your marker") ++f.ruled;
		}
		if (on) *on = f.onRoute;
		if (want) *want = f.count;
		f.cityLimit = city.limit; f.pool = pool; f.editorRoute = EditorRoute(r); f.ruledMax = rule.max;
		return answer();
	}
	bool Allowed(const trafficdata::Route& r, const trafficdata::Traffic& rule);
	// The route a free vehicle at (x, y) takes: the least filled of those in reach with room (routeshare.h); null when none.
	const trafficdata::Route* FairRoute(const char* world, const trafficdata::Traffic& rule, const Slot* except, bool pool, double x, double y,
		const std::string& own, trafficroute::Line* lineOut = nullptr, unsigned skipFor = 0)
	{
		std::vector<const trafficdata::Route*> routes;
		std::vector<trafficroute::Line> lines;
		std::vector<routeshare::Candidate> c;
        FvoTrafficPlayer player{};
        const bool prioritize=pool&&g_trafficApi&&g_trafficApi->player&&
            engineport::Forward<&FvoTrafficApi::player>(g_trafficApi,"player","routes",0u,&player)&&
            trafficdata::SameWorld(g_data,player.world,world);
		for (const auto& r : g_data.routes) {
			int on = 0, want = 0;
			if (!trafficdata::SameWorld(g_data, r.world, world) || !(Allowed(r, rule) || r.name == own) || !Room(r, rule, except, pool, &on, &want)) continue;
			if (EmptyHere(skipFor, r.name)) continue;  // its last trip here ended where it began
			if (Excluded(r, skipFor)) continue;        // its model is left out of this route (vehicle rules)
			auto l = trafficroute::Build(r);
			if(l.Count()<2)continue;
			const auto p = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			const auto priority=prioritize?generator::FleetPriority(std::hypot(p.x-player.x,p.y-player.y,p.z-player.z)):0u;
            c.push_back({ on, want, r.name == own ? 0.0 : std::hypot(p.x - x, p.y - y), r.name == own, priority });
			routes.push_back(&r); lines.push_back(std::move(l));
		}
		const int k = routeshare::Pick(c, rule.reach);
		if (k < 0) return nullptr;
		if (lineOut) *lineOut = lines[std::size_t(k)];
		return routes[std::size_t(k)];
	}
    trafficconstraints::Effective Restrictions(const trafficdata::Route& route,bool latest=false){
        const auto* r=&route;if(latest)for(const auto& candidate:g_data.routes)if(candidate.name==route.name){r=&candidate;break;}
        trafficconstraints::Effective result;for(const auto& c:g_data.cities)if(c.world==r->world||(g_data.worlds.count(c.world)&&g_data.worlds.count(r->world)&&g_data.worlds.at(c.world)==g_data.worlds.at(r->world)))trafficconstraints::Apply(result,c.restrictions,g_constraintMinute);
        for(const auto& group:g_data.groups)if(group.name==trafficdata::GlobalGroup()||group.name==r->group)trafficconstraints::Apply(result,group.restrictions,g_constraintMinute);
        trafficconstraints::Apply(result,r->restrictions,g_constraintMinute);return result;
    }
    bool RevisionReady(const trafficdata::Route& route){const auto key=routecache::Key(route);for(auto& slot:g_slots)if(slot.used&&slot.trip.leg!=trafficroute::Leg::Done){if(slot.route==route.name&&Spec(slot).name==route.name&&routecache::Key(Spec(slot))!=key)return false;if(!slot.turn.empty()&&slot.turnFrom==route.name&&routecache::Key(slot.turnSpec)!=key)return false;}return true;}
    trafficconstraints::Effective SpatialRestrictions(const trafficdata::Route& route,double x,double y,double z){auto result=Restrictions(route,true);const auto* worldRoute=&route;for(const auto& current:g_data.routes)if(current.name==route.name){worldRoute=&current;break;}for(const auto& zone:g_data.zones)if(zone.world==worldRoute->world&&z>=zone.restrictionFloor&&z<=zone.restrictionTop&&std::hypot(x-zone.at.x,y-zone.at.y)<=zone.radius)trafficconstraints::Apply(result,zone.restrictions,g_constraintMinute);return result;}
	bool Allowed(const trafficdata::Route& r, const trafficdata::Traffic& rule) { return (!r.road.enabled||g_fieldSupported)&&!splinekit::Problem(r.spacePlan,r.points.size())&&(!r.spacePlan.enabled||g_corridorSupported)&&Restrictions(r).admit&&(!Restrictions(r).extended||g_travelRulesSupported)&&!laneplan::Problem(r.lanePlan)&&(!r.lanePlan.enabled||g_coordinatorSupported)&&(!laneplan::Extended(r.lanePlan)||g_travelRulesSupported)&&(rule.route.empty() || r.name == rule.route || EditorRoute(r)); }
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
        if(s.networkSnapshot){std::vector<const trafficdata::Route*> routes;for(const auto& route:s.plannedRoutes)routes.push_back(&route);s.junctions=trafficlinks::Of(s.plannedLinks,routes,s.route);}
		char world[201]{};
		if (g_data.links.empty() || !g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return;
		if(!s.networkSnapshot)s.junctions = trafficlinks::Of(g_data.links, WorldRoutes(world), s.route);
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
        if(j.fromLane>=0&&j.fromLane!=s.trip.lane)return false;
		char world[201]{};
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return false;
		const auto* q = trafficlinks::Find(WorldRoutes(world), j.other);if(s.networkSnapshot)for(const auto& committed:s.plannedRoutes)if(committed.name==j.other){q=&committed;break;}
		if (!q) return false;
        const auto restrictions=Restrictions(*q,true);const bool committed=s.networkSnapshot&&s.itineraryAt+1<s.itinerary.routes.size()&&s.itinerary.routes[s.itineraryAt+1]==q->name;if((!restrictions.admit&&!(committed&&restrictions.valid&&restrictions.closure==1))||(s.route==q->name&&s.trip.lane!=j.toLane&&restrictions.noLaneChange)){s.connectionBlocked=true;return false;}
		const bool home = q->name == s.home;
		if (!home && !OpenNow(*q)) return false;
		const auto line = trafficroute::Build(*q);
		const int lane = j.toLane>=0?j.toLane:!s.itinerary.routes.empty()&&s.itineraryAt+1<s.itinerary.lanes.size()?s.itinerary.lanes[s.itineraryAt+1]:home && line.closed ? s.homeLane : trafficlinks::EntryLane(line, q->lanes, j.otherU);
		if (lane < 0) return false;
        if(s.baseId&&trafficdata::LaneLeftOut(g_data.groups,*q,lane,s.baseId)){s.connectionBlocked=true;return false;}
        const auto lanes=trafficroute::Lanes(line,q->lanes);if(std::find(lanes.begin(),lanes.end(),lane)==lanes.end()||!trafficroute::Available(line,lane,j.otherU))return false;
        const auto candidate=trafficlinks::TurnPath(s.line,s.trip.lane,j.u,line,lane,j.otherU,j.radius);
        if(s.route==q->name&&s.trip.lane!=lane&&candidate.empty()){s.connectionBlocked=true;return false;}
        const auto destination=trafficroute::At(line,lane,j.otherU);
        for(const auto& other:g_slots){if(!other.used||&other==&s)continue;
            if(other.route==q->name&&other.trip.lane==lane&&std::fabs(other.z-destination.z)<5&&std::hypot(other.x-destination.x,other.y-destination.y)<std::max(10.0,other.measuredSpeed*2+8)){s.connectionBlocked=true;return false;}
            if(!candidate.empty()&&!other.turn.empty())for(std::size_t a=0;a<candidate.size();a+=2)for(std::size_t b=other.turnAt;b<other.turn.size();b+=2){const auto p=candidate[a],q=other.turn[b];if(std::hypot(p.x-q.x,p.y-q.y,p.z-q.z)<8){s.connectionBlocked=true;return false;}}
        }
        if(const auto wait=g_roadDecision.find(s.vehicleId);wait!=g_roadDecision.end()&&wait->second.first<60){s.connectionBlocked=true;return false;}
        s.connectionBlocked=false;
        s.turnFrom=s.route;s.turnSpec=Spec(s);s.turnFromLine=s.line;s.turnFromLane=s.trip.lane;s.turnRule=SectionAt(s,s.trip.lastU);
        s.turn=candidate;s.turnAt=0;
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
		s.trip = t; s.line = line; s.spec=*q;s.completedControl=-1;s.controlStopped=0;s.route = q->name; s.speed = RoleSpeed(*q); s.logged = t.leg;
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
		if (g_worldName && engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world))
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
		if (s.dest=="cruise" && before.leg == trafficroute::Leg::Follow && s.trip.leg == trafficroute::Leg::Follow && before.uKnown) {
			// Cruise: every link passed here plus staying on, equal at first, repeats less likely.
			std::vector<const trafficlinks::Junction*> passed;
			for (const auto& j : s.junctions) {
				const double start=j.radius>0?trafficroute::LeadU(s.line,s.trip.lane,j.u,2.5*j.radius,true):j.u;
				if (trafficlinks::Crossed(s.line,s.trip.lane,before.lastU,s.trip.lastU,start)) passed.push_back(&j);
			}
			if (passed.empty()) return;
			std::string key = s.route + "@";for (const auto* j : passed) key += j->link + "|";
			std::vector<int> picks{s.cruisePicks[key + "stay"]};for (const auto* j : passed) picks.push_back(s.cruisePicks[key + j->link]);
			const auto weights = trafficlinks::CruiseWeights(picks);
			const auto pick = trafficlinks::CruisePick(weights, trafficlinks::Roll(s.dice));
			std::string options;for (std::size_t k = 0; k < weights.size(); ++k){char item[96];std::snprintf(item,sizeof item,"%s%s=%.3f",k?",":"",k?passed[k-1]->link.c_str():"stay",weights[k]);options+=item;}
			Log("OBS system=traffic.cruise vehicle=%08X event=junction route=%s options=%s choice=%s reason=equal_split_repeat_damped",s.vehicleId,s.route.c_str(),options.c_str(),pick?passed[pick-1]->link.c_str():"stay");
			++s.cruisePicks[key + (pick ? passed[pick-1]->link : std::string("stay"))];
			if (pick && TakeJunction(s, *passed[pick-1], false)) return;
			return;
		}
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

	// Keep-out and slow zones on a flight goal. Only the lane itself is lifted: joining and landing keep their own heights.
	void ZoneLimits(const Slot& s, double x, double y, FvoTripGoal* out, bool keepOut)
	{
		if (g_data.zones.empty()) return;
		char world[201]{};
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return;
		double z = out->z, speed = out->speed;
		trafficdata::ApplyZones(g_data, world, x, y, out->x, out->y, z, speed, keepOut);
		out->z = z; out->speed = float(speed);
		(void)s;
	}
	// Crossings (junction type crossing): nobody turns; near the point the vehicle with the higher id slows for the other, so two can't wait on each other.
	struct CrossPoint { std::string a, b; trafficdata::Vec at; };
	std::vector<CrossPoint> g_crossings;
	unsigned long long g_crossStamp = ~0ull;
	std::string g_crossWorld;
	void CrossLimit(const Slot& s, double x, double y, FvoTripGoal* out)
	{
		bool any = false;
		for (const auto& k : g_data.links) if (k.type == "crossing") { any = true; break; }
		if (!any) return;
		char world[201]{};
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return;
		if (g_crossStamp != g_stamp || g_crossWorld != world) {
			g_crossStamp = g_stamp; g_crossWorld = world; g_crossings.clear();
			const auto routes = WorldRoutes(world);
			for (const auto& k : g_data.links) {
				if (k.type != "crossing") continue;
				const auto* a = trafficlinks::Find(routes, k.from);
				if (!a || !trafficlinks::Find(routes, k.to)) continue;
				g_crossings.push_back({ k.from, k.to, trafficroute::At(trafficroute::Build(*a), 0, trafficlinks::U(*a, k.at)) });
			}
		}
		for (const auto& c : g_crossings) {
			if (s.route != c.a && s.route != c.b) continue;
			if (std::hypot(x - c.at.x, y - c.at.y) >= 60.0) continue;
			const std::string& other = s.route == c.a ? c.b : c.a;
			for (const auto& o : g_slots)
				if (o.used && &o != &s && o.route == other && o.vehicleId && o.vehicleId < s.vehicleId && std::hypot(o.x - c.at.x, o.y - c.at.y) < 40.0) { out->speed = std::min(out->speed, 6.0f); break; }
		}
	}
    void BuildRoad() {
        char world[201]{};if(!g_worldName||!engineport::FunctionForward(g_worldName,"worldName","routes",0u,world,sizeof world))return;
        if(g_roadStamp==g_stamp&&g_roadWorld==world)return;
        std::vector<trafficdata::Route> routes;
        for(const auto& r:g_data.routes)if(!r.removed&&trafficdata::SameWorld(g_data,r.world,world))routes.push_back(r);
        g_road.Build(std::move(routes),g_data.links);g_rightOfWay.Reset();g_roadDecision.clear();g_approachTiming.clear();
        g_otherApproachDistance.clear();g_roadDecisionAt=-1;g_roadStamp=g_stamp;g_roadWorld=world;
    }
    roadnetwork::Itinerary PlanNetwork(trafficdata::Vec from,trafficdata::Vec to,double reach,const std::vector<std::string>& required={},const std::string& destinationRoute={},int destinationLane=-1){return g_road.Plan(from,to,reach,required,destinationRoute,destinationLane,[](const auto& route){return Restrictions(route).admit&&RevisionReady(route);});}
    bool AuthoredJourney(Slot& slot,const trafficdata::Route& owner,const char* world,trafficdata::Vec origin,double reach) {
        if(!owner.journey.unsupported.empty()||!journeyplan::Valid(owner.journey))return false;
        if(!owner.journey.enabled)return true;
        roadnetwork::Network network;std::vector<trafficdata::Route> routes;
        for(const auto& r:g_data.routes)if(!r.removed&&trafficdata::SameWorld(g_data,r.world,world)&&!laneplan::Problem(r.lanePlan)&&Restrictions(r).admit&&RevisionReady(r))routes.push_back(r);
        network.Build(std::move(routes),g_data.links);
        std::vector<ScriptStop> stops;roadnetwork::Itinerary first;auto from=origin;
        for(const auto& stop:owner.journey.stops){const trafficdata::Vec to{stop.x,stop.y,stop.z};auto leg=network.Plan(from,to,reach);if(leg.routes.empty())return false;
            const auto destination=std::find_if(network.routes.begin(),network.routes.end(),[&](const auto& r){return r.name==leg.routes.back();});if(destination==network.routes.end())return false;
            const auto endpoint=trafficroute::At(network.lines[std::size_t(destination-network.routes.begin())],leg.lanes.back(),leg.exits.back());
            if(std::hypot(endpoint.x-to.x,endpoint.y-to.y,endpoint.z-to.z)>reach)return false;
            if(stops.empty())first=leg;stops.push_back({to,stop.dwell,{}});from=to;}
        if(owner.journey.loop&&network.Plan(from,stops.front().at,reach).routes.empty())return false;
        for(std::size_t i=0;i<owner.journey.stops.size();++i){const int fallback=owner.journey.stops[i].fallback;if(fallback>=0&&network.Plan(stops[i].at,stops[std::size_t(fallback)].at,reach).routes.empty())return false;}
        const auto found=std::find_if(network.routes.begin(),network.routes.end(),[&](const auto& r){return r.name==first.routes.front();});if(found==network.routes.end())return false;const auto* route=&*found;
        slot.authoredJourney=owner.journey;slot.script=std::move(stops);slot.scriptAt=0;slot.scriptDwell=0;slot.scriptHolding=false;slot.itinerary=first;slot.itineraryAt=0;CommitNetwork(slot,network);
        slot.spec=*route;slot.route=route->name;slot.line=trafficroute::Build(*route);slot.speed=RoleSpeed(*route);slot.dest="authored journey";
        slot.trip=trafficroute::StartTo(slot.line,route->lanes,origin,slot.script[0].at,first.exits.front());slot.trip.lane=first.lanes.front();
        slot.place.clear();slot.space=-1;slot.queue=false;slot.stay=0;FindJunctions(slot);return true;
    }
    double LaneBumper(const Slot& slot) {
        FvoObservedModelV1 model;
        return slot.baseId&&g_observedModel&&g_observedModel(slot.baseId,&model)&&modeladmission::Valid(model)?
            model.halfLength:coordinator::rules::bumperHalfFallback;
    }
    // ---- Lane coordination (lanecoord.h; plan cards C1-C4, variant 2026-11-10) ----
    // The coordinator's one arbiter for lane changes. It reads the core's read-only records (the planned lane, World's corridor summary)
    // and speaks only through route data: lane speed and holds (as before) and lane permissions. Each export is optional:
    // a pointer below stays null when the core does not provide it, and that part of coordination is skipped.
    planowner::SnapshotV2 g_ownerSnapshot2=nullptr;      // C1: FVO_OwnerSnapshotV2 (the lane each committed plan heads for)
    lanecoord::SnapshotV1 g_corridorSnapshot=nullptr;    // C2: FVO_CorridorSnapshotV1
    struct LaneCar {
        unsigned vehicle=0;std::uint64_t road=0;int lane=-1,planned=-1,priority=0;
        double station=0,length=0,speed=0,halfLength=2.8,stoppedFor=0;unsigned heldBy=0;const Slot* slot=nullptr;
    };
    std::vector<LaneCar> g_laneCars;double g_laneCoordAt=-1;
    lanecoord::Reservations g_laneReserved;lanecoord::Blocks g_laneBlocks;lanecoord::Yields g_laneYields;
    std::map<std::pair<unsigned,int>,std::pair<double,int>> g_laneClosed;  // (vehicle, lane) -> when it was last closed, and why (hysteresis and the trace)
    bool OnRoadLane(const Slot& s){return s.used&&!StockRoutePending(s)&&s.vehicleId&&s.trip.leg==trafficroute::Leg::Follow&&!s.placePath&&s.turn.empty()&&!s.line.error&&!s.line.distance.empty();}
    std::uint64_t RoadId(const Slot& s){std::uint64_t h=1469598103934665603ull;for(char c:s.world)h=(h^static_cast<unsigned char>(c))*1099511628211ull;h=(h^0x2Full)*1099511628211ull;
        for(char c:s.route)h=(h^static_cast<unsigned char>(c))*1099511628211ull;return h?h:1;}
    const LaneCar* LaneCarOf(unsigned vehicle){for(const auto& c:g_laneCars)if(c.vehicle==vehicle)return &c;return nullptr;}
    // Metres from `self` to `other` along lane k's own travel direction (negative: upstream of self in that lane).
    double LaneAhead(const LaneCar& self,const LaneCar& other,int k){
        double d=(other.station-self.station)*trafficroute::Direction(self.slot->line,k);
        if(self.length>0){d=std::fmod(d,self.length);if(d>self.length*.5)d-=self.length;if(d<-self.length*.5)d+=self.length;}
        return d;
    }
    // One lane for one car: the arbiter's answer, with every fact gathered here. `useReservation`: a held reservation keeps the lane.
    lanecoord::Answer LaneAnswer(const LaneCar& self,int k,double now,bool useReservation){
        static lanecoord::Other others[256];int n=0;
        const auto& line=self.slot->line;
        for(const auto& o:g_laneCars){
            if(o.vehicle==self.vehicle||o.road!=self.road||n>=256)continue;
            auto& out=others[n++];out=lanecoord::Other{};out.vehicle=o.vehicle;out.lane=o.lane;out.planned=o.planned;out.halfLength=o.halfLength;
            out.ahead=LaneAhead(self,o,k);out.speed=o.speed*trafficroute::Direction(line,o.lane)*trafficroute::Direction(line,k);
        }
        lanecoord::Ask a;a.vehicle=self.vehicle;a.own=self.lane;a.lane=k;a.now=now;a.speed=self.speed;a.halfLength=self.halfLength;
        a.sameWay=trafficroute::Direction(line,k)*trafficroute::Direction(line,self.lane)>0;
        a.blockAhead=g_laneBlocks.Ahead(self.road,k,self.station,trafficroute::Direction(line,k),self.length,now);
        if(const auto* mine=g_laneReserved.Find(self.vehicle,self.road,k))a.holdsReservation=useReservation&&now-mine->seen<=lanecoord::rules::reserveLapse;
        a.yielding=g_laneYields.To(self.vehicle,now)!=0;
        a.reservedBy=g_laneReserved.Before(self.vehicle,self.road,k,self.station,self.halfLength,now);
        const auto was=g_laneClosed.find({self.vehicle,k});
        a.wasClosed=was!=g_laneClosed.end()&&now-was->second.first<=lanecoord::rules::permitHold*2;
        return lanecoord::Decide(a,others,n);
    }
    void UpdateLaneCoordination(double now){
        if(!g_ownerSnapshot2&&!g_corridorSnapshot)return;
        if(g_laneCoordAt>=0&&now>=g_laneCoordAt&&now-g_laneCoordAt<0.1)return;
        g_laneCoordAt=now;
        static planowner::RowV2 owners[256];static lanecoord::RowV1 corridor[256];int no=0,nc=0;
        if(g_ownerSnapshot2){owners[0]=planowner::RowV2{};no=std::clamp(g_ownerSnapshot2(owners,256),0,256);}
        if(g_corridorSnapshot){corridor[0]=lanecoord::RowV1{};nc=std::clamp(g_corridorSnapshot(corridor,256),0,256);}
        g_laneCars.clear();g_laneReserved.Sweep(now);g_laneBlocks.Sweep(now);g_laneYields.Sweep(now);
        for(auto it=g_laneClosed.begin();it!=g_laneClosed.end();)if(now-it->second.first>10)it=g_laneClosed.erase(it);else ++it;
        for(auto& s:g_slots){
            if(!OnRoadLane(s)||!s.stepAt||now-s.stepAt>coordinator::rules::poseFresh)continue;
            LaneCar c;c.vehicle=s.vehicleId;c.slot=&s;c.lane=s.trip.lane;c.road=RoadId(s);
            const double u=CarProgress(s);c.station=trafficroute::MetresAt(s.line,u);c.length=s.line.closed?trafficroute::Length(s.line):0;
            c.speed=s.measuredSpeed;c.halfLength=LaneBumper(s);c.priority=SectionAt(s,u).priority;
            for(int i=0;i<no;++i)if(owners[i].vehicle==s.vehicleId){   // C1: the lane its committed plan heads for, only while it is this lane's plan and fresh
                if(owners[i].laneIdentity==s.guidance.identity&&owners[i].laneAge10ms>=0&&owners[i].laneAge10ms<=150)c.planned=owners[i].lane;break;}
            for(int i=0;i<nc;++i){const auto& r=corridor[i];if(r.vehicle!=s.vehicleId)continue;
                if(r.laneIdentity==s.guidance.identity&&r.age<=1.5f){
                    c.stoppedFor=r.stoppedFor;c.heldBy=r.blockOwner;
                    // C3: a fixed blockage this car measured in a lane of its road. Kept by road, lane and station; forgotten when no car reports it.
                    const double ownDir=trafficroute::Direction(s.line,s.trip.lane);
                    for(int k=0;k<std::min(r.laneCount,lanecoord::kLanes);++k){
                        if(!(r.freeLength[k]<r.measured-0.5f)||r.freeLength[k]>=lanecoord::rules::approachReach)continue;
                        double at=c.station+ownDir*double(r.freeLength[k]);
                        if(c.length>0){at=std::fmod(at,c.length);if(at<0)at+=c.length;}
                        if(g_laneBlocks.Report(c.road,k,at,c.vehicle,now))observations::Record(g_log,GetTickCount64(),"traffic.lane_block",c.vehicle,std::uint64_t(k),
                            "event=fixed_blockage route=%s lane=%d station_m=%.1f ahead_m=%.1f measured_m=%.1f reason=corridor_summary_reports_a_fixed_solid_in_this_lane",
                            s.route.c_str(),k,at,double(r.freeLength[k]),double(r.measured));
                    }
                }
                break;}
            g_laneCars.push_back(c);
        }
        // Reservations: a plan heading for another lane holds that lane's gap from the moment the lane was open to it. A plan that heads
        // for a lane the arbiter has closed gets no reservation, so the permission stays closed and Avoid's branch there is not a lane.
        for(const auto& c:g_laneCars){
            if(c.planned<0||c.planned==c.lane){g_laneReserved.Drop(c.vehicle);continue;}
            const bool had=g_laneReserved.Find(c.vehicle,c.road,c.planned)!=nullptr;
            if(had||LaneAnswer(c,c.planned,now,false).reason==lanecoord::Open){
                const bool held=g_laneReserved.Hold(c.vehicle,c.road,c.planned,c.station,c.halfLength,now);
                if(!had)observations::Record(g_log,GetTickCount64(),"traffic.lane_reserve",c.vehicle,std::uint64_t(c.planned),
                    "event=%s route=%s from_lane=%d to_lane=%d station_m=%.1f reason=committed_plan_heads_for_an_open_lane",held?"reserved":"table_full",c.slot->route.c_str(),c.lane,c.planned,c.station);
            }
        }
        // C4: two cars that each stand held by the other. One yields: every lane but its own is closed to it for a while.
        for(std::size_t i=0;i<g_laneCars.size();++i)for(std::size_t j=i+1;j<g_laneCars.size();++j){
            const auto& a=g_laneCars[i];const auto& b=g_laneCars[j];
            const lanecoord::Waiter wa{a.vehicle,a.heldBy,a.stoppedFor,a.priority},wb{b.vehicle,b.heldBy,b.stoppedFor,b.priority};
            if(!lanecoord::Deadlocked(wa,wb))continue;
            const unsigned loser=lanecoord::Loser(wa,wb),winner=loser==a.vehicle?b.vehicle:a.vehicle;
            if(g_laneYields.Set(loser,winner,now))observations::Record(g_log,GetTickCount64(),"traffic.deadlock",loser,winner,
                "event=yield loser=%08X winner=%08X stood_s=%.1f,%.1f priority=%d,%d reason=each_car_stands_held_by_the_other",loser,winner,a.stoppedFor,b.stoppedFor,a.priority,b.priority);
        }
    }
    // C3 / C4: lane permissions for one car, asked by the core each planning update (lanecoord::PermitProviderV1). Route data only.
    int LanePermit(void* user,lanecoord::PermitV1* out){
        if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
        std::lock_guard journeyLock(g_journeyMutex);
        Slot* s=slotcookie::Find(g_slots,user);
        if(!s||!s->used||!OnRoadLane(*s)||out->identity!=s->guidance.identity||!Spec(*s).road.enabled||s->line.road.empty())return 0;
        const double now=GetTickCount64()*0.001;
        UpdateLaneCoordination(now);
        const LaneCar* self=LaneCarOf(s->vehicleId);if(!self)return 0;
        FvoRoadLanesV1 lanes;if(!roadfield::LanesAt(s->line,s->trip.lane,CarProgress(*s),lanes))return 0;
        out->count=lanes.count;
        for(int k=0;k<lanes.count&&k<lanecoord::kLanes;++k){
            out->open[k]=1;out->reason[k]=lanecoord::Open;
            if(k==lanes.own)continue;
            auto answer=LaneAnswer(*self,k,now,true);
            const auto key=std::make_pair(s->vehicleId,k);const auto was=g_laneClosed.find(key);
            // A closed answer stands at least permitHold: a lane does not flicker open and shut between two updates.
            if(answer.reason==lanecoord::Open&&was!=g_laneClosed.end()&&now-was->second.first<lanecoord::rules::permitHold&&now>=was->second.first)answer.reason=was->second.second;
            const int before=was!=g_laneClosed.end()&&now-was->second.first<=0.5?was->second.second:lanecoord::Open;
            if(answer.reason!=lanecoord::Open){
                if(was==g_laneClosed.end()||was->second.second!=answer.reason||now-was->second.first>0.5)
                    if(before!=answer.reason)observations::Record(g_log,GetTickCount64(),"traffic.lane_permit",s->vehicleId,std::uint64_t(k)|(std::uint64_t(answer.reason)<<8),
                        "event=closed route=%s own_lane=%d lane=%d why=%s by=%08X gap_m=%.1f need_m=%.1f reason=coordinator_lane_arbiter",s->route.c_str(),lanes.own,k,lanecoord::Name(answer.reason),answer.by,answer.gap>1e17?-1.0:answer.gap,answer.need);
                if(was==g_laneClosed.end()||answer.reason!=was->second.second||now-was->second.first>=lanecoord::rules::permitHold)g_laneClosed[key]={now,answer.reason};
                out->open[k]=0;out->reason[k]=answer.reason;
            }
        }
        return 1;
    }
    // C3: a car coming up to a known fixed blockage in its own lane slows early, so its own volume measures it with room to choose.
    void ApproachBlock(const Slot& self,double now,coordinator::Holds<FvoLaneGuidanceV1>& holds){
        const LaneCar* car=LaneCarOf(self.vehicleId);if(!car)return;
        const double ahead=g_laneBlocks.Ahead(car->road,car->lane,car->station,trafficroute::Direction(self.line,car->lane),car->length,now);
        if(!(ahead>0&&ahead<lanecoord::rules::approachReach)||holds.lane.speed<=lanecoord::rules::approachSpeed)return;
        holds.lane.speed=float(lanecoord::rules::approachSpeed);
        observations::Record(g_log,GetTickCount64(),"traffic.lane_block",self.vehicleId,0x41ull,"event=approach ahead_m=%.1f cap_mps=%.1f reason=known_fixed_blockage_in_own_lane",ahead,lanecoord::rules::approachSpeed);
    }
    void FollowLane(const Slot& self,double u,double now,coordinator::Holds<FvoLaneGuidanceV1>& holds) {
        if(self.trip.leg!=trafficroute::Leg::Follow||self.placePath||!self.turn.empty())return;
        UpdateLaneCoordination(now);
        // PD-1b (2026-11-12, one driver): a car whose committed plan the core reports follows the car ahead itself. World holds that car as
        // a Vehicle shape and Speed keeps the gap with the holder named, and only the plan can decide to go round it. This cap reached the
        // plan as a wanted speed of 0 with no reason (13:10:44 run: leader FF101109 rocking, gap 1.1 m, ledger "held by nothing").
        if(const LaneCar* mine=LaneCarOf(self.vehicleId);mine&&mine->planned>=0){
            observations::Record(g_log,GetTickCount64(),"traffic.spacing",self.vehicleId,0x504C414Eull,
                "event=left_to_plan lane=%d planned=%d reason=core_plan_follows_the_car_ahead_as_a_world_shape",self.trip.lane,mine->planned);
            return;
        }
        using namespace coordinator::rules;
        double ownBumper=0;
        unsigned leader=0;
        double gap=0;
        for(const auto& other:g_slots) {
            // C1 (variant): a car of another lane of this road also leads when it stands in this lane (the position check below) or its
            // committed plan holds a reservation into it (it is moving in ahead of us).
            const bool merging=other.trip.lane!=self.trip.lane&&other.used&&other.vehicleId&&[&]{const auto* c=LaneCarOf(other.vehicleId);
                return c&&c->planned==self.trip.lane&&g_laneReserved.Find(c->vehicle,c->road,self.trip.lane)!=nullptr;}();
            if(&other==&self||!other.used||!other.vehicleId||other.world!=self.world||other.route!=self.route||
               other.trip.leg!=trafficroute::Leg::Follow||
               other.placePath||!other.turn.empty()||!other.stepAt||now-other.stepAt>poseFresh||
               std::hypot(other.x-self.x,other.y-self.y,other.z-self.z)>followingReach)continue;
            const int lane=self.trip.lane;
            const double at=CarProgress(other);
            const auto point=trafficroute::At(self.line,lane,at);
            if(std::fabs(point.z-other.z)>laneHeightTolerance||
               (!merging&&std::hypot(point.x-other.x,point.y-other.y)>holds.lane.halfWidth+laneSideAllowance))continue;
            const double distance=trafficroute::DistanceAlong(self.line,lane,u,at);
            if(!(distance>0&&distance<=followingReach))continue;
            const auto next=trafficroute::At(self.line,lane,trafficroute::LeadU(self.line,lane,at,1,false,false));
            const double length=std::hypot(next.x-point.x,next.y-point.y,next.z-point.z);
            const double pace=length>1e-6?((next.x-point.x)*other.stepVelocity.x+
                (next.y-point.y)*other.stepVelocity.y+(next.z-point.z)*other.stepVelocity.z)/length:0;
            if(!ownBumper)ownBumper=LaneBumper(self);
            const double available=distance-ownBumper-LaneBumper(other);
            const double cap=planspeed::FollowingCap(available,pace);
            if(cap<holds.lane.speed){holds.lane.speed=cap;leader=other.vehicleId;gap=available;}
        }
        if(leader){
            if(holds.lane.speed<=roadnetwork::rules::stoppedSpeed)holds.Hold(coordinator::Following);
            observations::Record(g_log,GetTickCount64(),"traffic.spacing",self.vehicleId,leader,
                "leader=%08X gap_m=%.1f cap_mps=%.1f reason=following_headway",leader,gap,double(holds.lane.speed));
        }
    }
    void UpdateJunctions(double now) {
        using namespace roadnetwork::rules;
        if(g_roadDecisionAt>=0&&now-g_roadDecisionAt<approachRefresh)return;
        g_roadDecisionAt=now;
        g_roadDecision.clear();
        decltype(g_approachTiming) timings;
        decltype(g_otherApproachDistance) otherDistances;
        static planowner::RowV1 others[64];
        int otherCount=0;
        if(g_ownerSnapshot){others[0]=planowner::RowV1{};otherCount=std::clamp(g_ownerSnapshot(others,64),0,64);}

        for(int j=0;j<int(g_road.junctions.size());++j){
            const auto& node=g_road.junctions[j];
            std::vector<roadnetwork::Request> requests;
            std::map<std::pair<std::string,int>,std::size_t> fronts;
            for(auto& o:g_slots){
                if(!o.used||!o.vehicleId||o.world!=g_roadWorld||!o.stepAt||now-o.stepAt>approachFresh)continue;
                const auto key=ApproachKey{j,o.vehicleId};
                const auto previous=g_approachTiming.find(key);
                const bool turning=!o.turn.empty()&&!o.turnFrom.empty();
                const auto& approachLine=turning?o.turnFromLine:o.line;
                const auto& approachRoute=turning?o.turnFrom:o.route;
                const int approachLane=turning?o.turnFromLane:o.trip.lane;
                const double centre=std::hypot(o.x-node.at.x,o.y-node.at.y);
                const bool inside=centre<node.radius+stopMargin&&std::fabs(o.z-node.at.z)<=crossingHeight;
                bool submitted=false;
                for(const auto& a:node.approaches){
                    if(a.route!=approachRoute||std::fabs(o.z-a.at.z)>crossingHeight)continue;
                    const auto progress=trafficroute::NearestProgress(approachLine,approachLane,o.x,o.y,o.z,false,0);
                    const auto station=trafficroute::DistanceAlong(approachLine,approachLane,progress,a.u);
                    if(!inside&&(!std::isfinite(station)||station>approachReach))continue;
                    const auto policy=turning?o.turnRule:SectionAt(o,progress);
                    const auto outU=turning?trafficroute::NearestProgress(o.line,o.trip.lane,node.at.x,node.at.y,node.at.z,false,0):a.u;
                    const auto exit=trafficroute::At(o.line,o.trip.lane,trafficroute::LeadU(o.line,o.trip.lane,outU,node.radius+exitLead,false,false));
                    const auto blocksExit=[&](double x,double y,double z,double speed){
                        return speed<exitBlockedSpeed&&std::fabs(z-exit.z)<exitHeight&&std::hypot(x-exit.x,y-exit.y)<exitRadius;
                    };
                    bool exitClear=true;
                    for(const auto& p:g_slots)
                        if(p.used&&p.vehicleId!=o.vehicleId&&p.world==o.world&&p.stepAt&&now-p.stepAt<=approachFresh&&
                           blocksExit(p.x,p.y,p.z,p.measuredSpeed)){exitClear=false;break;}
                    for(int k=0;exitClear&&k<otherCount;++k)
                        if(others[k].vehicle!=o.vehicleId&&others[k].age<=approachFresh&&
                           blocksExit(others[k].x,others[k].y,others[k].z,others[k].speed))exitClear=false;
                    const double distance=inside?0:std::max(0.0,station-node.radius-stopMargin);
                    auto& timer=timings[key];
                    if(previous!=g_approachTiming.end())timer=previous->second;
                    timer.Observe(now,distance,o.measuredSpeed);
                    roadnetwork::Request q{o.vehicleId,j,policy.priority-(policy.control==routepolicy::Control::Yield?10:0),
                        distance,timer.stoppedSince<0?0:now-timer.stoppedSince,now-timer.since,
                        inside,exitClear,roadnetwork::Green(policy,g_signalSeconds),policy.control,policy.stopSeconds};
                    if(!inside){
                        auto [front,created]=fronts.emplace(std::make_pair(approachRoute,approachLane),requests.size());
                        if(!created){
                            auto& first=requests[front->second];
                            if(std::make_pair(q.distance,q.vehicle)<std::make_pair(first.distance,first.vehicle)){
                                first.frontOfQueue=false;front->second=requests.size();
                            }else q.frontOfQueue=false;
                        }
                    }
                    requests.push_back(q);submitted=true;break;
                }
                if(!submitted&&inside)
                    requests.push_back({o.vehicleId,j,0,0,0,0,true});
                else if(!submitted&&previous!=g_approachTiming.end())
                    requests.push_back({o.vehicleId,j,0,-1});
            }
            for(int k=0;k<otherCount;++k){
                const auto& o=others[k];
                if(!o.vehicle||o.owner==int(planowner::Owner::Traffic)||o.owner==int(planowner::Owner::None)||
                   o.age>approachFresh||std::fabs(o.z-node.at.z)>crossingHeight)continue;
                if(std::any_of(requests.begin(),requests.end(),[&](const auto& r){return r.vehicle==o.vehicle;}))continue;
                const auto key=ApproachKey{j,o.vehicle};
                const double centre=std::hypot(o.x-node.at.x,o.y-node.at.y);
                const bool inside=centre<node.radius+stopMargin;
                const auto was=g_otherApproachDistance.find(key);
                if(centre<=node.radius+approachReach)otherDistances.emplace(key,centre);
                if(!inside&&(centre>node.radius+approachReach||o.speed<exitBlockedSpeed||
                   (was!=g_otherApproachDistance.end()&&centre>=was->second+.05))){
                    if(was!=g_otherApproachDistance.end())requests.push_back({o.vehicle,j,0,-1});
                    continue;
                }
                const int rank=std::clamp(o.rank,0,int(trafficrank::Rank::Player));
                requests.push_back({o.vehicle,j,trafficrank::Priority(trafficrank::Rank(rank))-trafficrank::Priority(trafficrank::Rank::Traffic),
                    inside?0:std::max(0.0,centre-node.radius-stopMargin),0,0,inside});
            }
            const unsigned granted=g_rightOfWay.Grant(j,now,requests);
            for(const auto& q:requests)if(roadnetwork::MustYield(q,granted)){
                const bool held=q.distance<=holdDistance;
                auto [decision,created]=g_roadDecision.emplace(q.vehicle,std::make_pair(q.distance,held));
                if(!created){decision->second.first=std::min(decision->second.first,q.distance);decision->second.second|=held;}
                if(held)observations::Record(g_log,GetTickCount64(),"traffic.right_of_way",q.vehicle,granted,
                    "junction=%d granted=%08X exit_clear=%d green=%d front=%d stopped_s=%.1f reason=yield_before_entry",
                    j,granted,int(q.exitClear),int(q.green),int(q.frontOfQueue),q.stoppedFor);
            }
        }
        g_approachTiming.swap(timings);
        g_otherApproachDistance.swap(otherDistances);
    }
    int LaneProvider(void* user,FvoLaneGuidanceV1* out) {
        std::lock_guard journeyLock(g_journeyMutex);
        if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
        Slot* s=slotcookie::Find(g_slots,user);if(!s||!s->used)return 0;
        BuildRoad();const auto& r=Spec(*s);const double u=CarProgress(*s);
        const auto rule=SectionAt(*s,u);
        FvoLaneGuidanceV1 lane;lane.travel=int(rule.travel);lane.priority=rule.priority;
        // Travel kind (2026-11-04, the user): an explicit Drive or Hover on the route, section or lane is a lock. "Both"
        // is read from the route itself: a lift (open-air tile, or a climb steeper than 35 degrees) flies, a road tile drives,
        // anything else keeps the both-rules (drive near the ground, fly when the lane is high over it).
        {const char* travelWhy=rule.travel==routepolicy::Travel::Drive?"set_drive":rule.travel==routepolicy::Travel::Hover?"set_hover":"natural_both";
         if(rule.travel==routepolicy::Travel::Both&&!s->placePath&&s->turn.empty()&&(s->trip.leg==trafficroute::Leg::Follow||s->trip.leg==trafficroute::Leg::Join)){  // joining reads the road too (alignment 2026-11-04)
            const auto natural=NaturalTravel(r,s->line,u,travelWhy);if(natural!=routepolicy::Travel::Both)lane.travel=int(natural);}
         if(lane.travel!=s->travelLogged||std::strcmp(travelWhy,s->travelWhyLogged)!=0){s->travelLogged=lane.travel;s->travelWhyLogged=travelWhy;
            Log("OBS system=traffic.travel vehicle=%08X route=%s lane=%d kind=%s reason=%s",s->vehicleId,s->route.c_str(),s->trip.lane,lane.travel==1?"drive":lane.travel==2?"hover":"both",travelWhy);}}
        const auto settings=trafficroute::SettingsAt(s->line,u,s->trip.lane);
        lane.halfWidth=(s->placePath?s->manoeuvrePlan.width:(r.lanePlan.enabled||r.spacePlan.enabled)?settings.profile.width:r.corridor)*0.5;
        if(r.road.enabled&&!s->placePath&&!s->line.road.empty()){double narrowest=1e30;const auto first=std::size_t(std::max(0.0,std::floor(u)));
            for(std::size_t k=0;k<12;++k){const auto i=s->line.closed?(first+k)%s->line.road.size():std::min(first+k,s->line.road.size()-1);const auto& b=s->line.road[i].boundaries;
                if(std::size_t(s->trip.lane+1)<b.size())narrowest=std::min(narrowest,std::hypot(b[s->trip.lane+1].x-b[s->trip.lane].x,b[s->trip.lane+1].y-b[s->trip.lane].y)-s->line.median*.5*((s->trip.lane>0)+(std::size_t(s->trip.lane+2)<b.size()))-(s->trip.lane==0?s->line.edgeRight:0)-(std::size_t(s->trip.lane+2)==b.size()?s->line.edgeLeft:0));}
            if(narrowest<1e29)lane.halfWidth=std::clamp(narrowest*.5,.5,100.0);}lane.speed=rule.speed>0?std::min(s->speed,rule.speed):s->speed;
        if(s->scriptAt<s->authoredJourney.stops.size()){const auto& stage=s->authoredJourney.stops[s->scriptAt];if(stage.speed>0)lane.speed=std::min(lane.speed,stage.speed);}
        // C3: the coordinator is the one writer of holds and stop lines; each rule below reports to it by name (its condition unchanged).
        coordinator::Holds<FvoLaneGuidanceV1> H(lane);unsigned& heldWhy=H.bits;  // traced below: which rule holds the lane
        H.HoldIf(s->holding||s->factoryPhase==FactoryRoutePhase::Test||s->factoryPhase==FactoryRoutePhase::Arrival||StockRoutePending(*s),coordinator::Holding);H.HoldIf(s->scriptHolding,coordinator::Script);H.HoldIf(s->connectionBlocked,coordinator::Connection);
        if(s->factoryPhase==FactoryRoutePhase::Departing&&s->route==s->factoryDestination&&s->turn.empty()){
            const double remaining=trafficroute::Ahead(s->line,s->trip.lane,u,s->factoryArrivalU);
            if(std::isfinite(remaining))H.StopMin(remaining);
        }
        // Exclusion lists choose which models traffic spawns and sends; your own Cruise is your choice and is never held by them (2026-11-04: FE00900A excluded on Roadrunner froze Cruise).
        if(s->dest!="cruise"&&((s->baseId&&trafficdata::LaneLeftOut(g_data.groups,r,s->trip.lane,s->baseId))||(!s->baseId&&!r.lanePlan.vehicles.lanes.empty()))){H.Hold(coordinator::ExcludedModel);}const auto restrictions=SpatialRestrictions(r,s->x,s->y,s->z);lane.speed=std::min(lane.speed,restrictions.speed);if(!restrictions.valid||restrictions.closure==2){H.Hold(coordinator::Zone);}
        if(!(restrictions.modes&(lane.travel==1?1:lane.travel==2?2:3))){H.Hold(coordinator::ZoneMode);}lane.identity=1469598103934665603ull;
        if(!r.lanePlan.unsupported.empty()||(r.lanePlan.enabled&&!laneplan::Active(settings,s->trip.lane))){H.Hold(coordinator::LanePlan);}
        for(char c:s->route)lane.identity=(lane.identity^static_cast<unsigned char>(c))*1099511628211ull;
        lane.identity^=std::uint64_t(s->trip.lane+1)<<56;
        lane.identity^=s->generation*0x9e3779b97f4a7c15ull;lane.identity^=std::uint64_t(s->placePath)<<54;
        auto add=[&](trafficdata::Vec p){if(lane.count<64)lane.points[lane.count++]={p.x,p.y,p.z};};
        if(!s->turn.empty()) {
            add({s->x,s->y,s->z});for(std::size_t i=s->turnAt;i<s->turn.size()&&lane.count<64;++i)add(s->turn[i]);
        } else if(s->trip.leg==trafficroute::Leg::Follow) {
            for(int i=-3;i<61;++i){const double d=i*2.0;const double q=trafficroute::LeadU(s->line,s->trip.lane,u,std::fabs(d),d<0,false);
                if(d>=0){const auto point=trafficroute::At(s->line,s->trip.lane,q);const auto spatial=SpatialRestrictions(r,point.x,point.y,point.z);lane.speed=std::min(lane.speed,spatial.speed);if(!spatial.valid||spatial.closure==2){const double stop=std::max(0.0,d-4);H.StopMin(stop);}}
                if(d>=0&&!trafficroute::Available(s->line,s->trip.lane,q)){const double stop=std::max(0.0,d-4);H.StopMin(stop);}
                add(liftpath::Apply(liftpath::At(r,s->line,s->trip.lane,q),trafficroute::At(s->line,s->trip.lane,q)));}  // a lift has one line: the route's
        } else {
            const auto to=(s->trip.leg==trafficroute::Leg::Leave||s->trip.leg==trafficroute::Leg::Done)?(s->trip.lap?s->trip.home:s->trip.dest):trafficroute::At(s->line,s->trip.lane,u);
            add({s->x,s->y,s->z});add(to);
        }
        if(s->pickupWait){lane.count=2;lane.points[0]={s->x,s->y,s->z};lane.points[1]=lane.points[0];lane.speed=0;H.Hold(coordinator::Pickup);}
        if(lane.count<2)return 0;
        lane.targetHeight=lane.points[std::min(5,lane.count-1)].z;
        const double now=GetTickCount64()*0.001;
        FollowLane(*s,u,now,H);
        ApproachBlock(*s,now,H);
        const bool paused=g_trafficApi&&g_trafficApi->presentation&&engineport::Forward<&FvoTrafficApi::presentation>(g_trafficApi,"presentation","routes",0u)>=2;
        if(!paused&&g_signalLastTick>0)g_signalSeconds+=std::clamp(now-g_signalLastTick,0.0,0.1);
        g_signalLastTick=now;
        // Section end controls also work on a lone road with no crossing link.
        if(!r.ring && s->trip.leg==trafficroute::Leg::Follow) {
            const int section=int(routepolicy::Segment(r.points.size(),r.closed,s->line.ToSource(u),s->trip.lane));
            const double endSource=trafficroute::Direction(s->line,s->trip.lane)<0?double(section):double(section+1);
            const double endU=s->line.ToLine(endSource);
            const double distance=trafficroute::DistanceAlong(s->line,s->trip.lane,u,endU);
            const auto end=trafficroute::At(s->line,s->trip.lane,endU);
            const bool junctionControls=std::any_of(g_road.junctions.begin(),g_road.junctions.end(),[&](const auto& j){
                return std::fabs(end.z-j.at.z)<=roadnetwork::rules::crossingHeight&&
                    std::hypot(end.x-j.at.x,end.y-j.at.y)<=j.radius&&
                    std::any_of(j.approaches.begin(),j.approaches.end(),[&](const auto& a){return a.route==s->route;});
            });
            const double clock=g_signalSeconds;const bool haveClock=true;
            bool wait=!junctionControls&&rule.control==routepolicy::Control::Signal&&(!haveClock||!roadnetwork::Green(rule,clock));
            if(!junctionControls&&rule.control==routepolicy::Control::Stop&&section!=s->completedControl) {
                wait=true;
                if(distance<5 && s->measuredSpeed<0.3){if(!s->controlStopped)s->controlStopped=now;if(now-s->controlStopped>=rule.stopSeconds){s->completedControl=section;wait=false;}}
                else s->controlStopped=0;
            }
            if(wait&&std::isfinite(distance)){H.StopMin(std::max(0.0,distance-roadnetwork::rules::stopMargin));
                if(distance<=roadnetwork::rules::stopMargin+roadnetwork::rules::holdDistance)H.Hold(coordinator::StopControl);}
        }
        UpdateJunctions(now);
        if(const auto decision=g_roadDecision.find(s->vehicleId);decision!=g_roadDecision.end()){
            H.StopMin(decision->second.first);
            if(decision->second.second)H.Hold(coordinator::Junction);
        }
        if(heldWhy!=s->heldWhyLogged){s->heldWhyLogged=heldWhy;
            Log("OBS system=traffic.lane_hold vehicle=%08X route=%s lane=%d dest=%s held=%d why=%s%s%s%s%s%s%s%s%s%s%s",s->vehicleId,s->route.c_str(),s->trip.lane,s->dest.c_str(),int(lane.held),
                heldWhy&1?"holding,":"",heldWhy&2?"script,":"",heldWhy&4?"connection_blocked,":"",heldWhy&8?"excluded_model,":"",heldWhy&16?"zone_closed_or_invalid,":"",
                heldWhy&32?"zone_mode_forbidden,":"",heldWhy&64?"lane_plan_inactive,":"",heldWhy&128?"pickup_wait,":"",heldWhy&256?"stop_line,":"",heldWhy&512?"junction_right_of_way,":"",heldWhy&1024?"following_headway,":"");}
        s->guidance=lane;*out=lane;return 1;
    }
    int TravelRulesProvider(void* user,FvoTravelRulesV1* out) {
        std::lock_guard journeyLock(g_journeyMutex);
        if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used){
            auto& s=*match;
            const double u=CarProgress(s);
            FvoTravelRulesV1 rules;rules.identity=s.guidance.identity;
            if(Spec(s).lanePlan.enabled)rules.rules=trafficroute::SettingsAt(s.line,u,s.trip.lane).rules;
            if(s.scriptAt<s.authoredJourney.stops.size()){
                const int request=s.authoredJourney.stops[s.scriptAt].mode;const auto permission=SectionAt(s,u).travel;
                if(request&&rules.rules.mode!=travelrules::Drive&&rules.rules.mode!=travelrules::Hover){
                    if(permission==routepolicy::Travel::Drive)rules.rules.mode=travelrules::Drive;
                    else if(permission==routepolicy::Travel::Hover)rules.rules.mode=travelrules::Hover;
                    else rules.rules.mode=request;
                }
            }
            const auto restrictions=SpatialRestrictions(Spec(s),s.x,s.y,s.z);
            if((rules.rules.mode==travelrules::Drive&&!(restrictions.modes&1))||(rules.rules.mode==travelrules::Hover&&!(restrictions.modes&2)))return 0;
            if(restrictions.modes==1)rules.rules.mode=travelrules::Drive;else if(restrictions.modes==2)rules.rules.mode=travelrules::Hover;
            // A lift (open-air tile, or a steep climb): the route is the authority. The authored-hover check wanted every point of a
            // 134-171 m shaft measured and within 40 m of the ground, so the car held at the foot (2026-11-06 12:08). The lane driver
            // climbs along the lane instead; ray-measured solids still stop it (traffic.lift).
            // Joining counts too (2026-11-06 15:47): Cruise re-engaged inside the shaft put the car in the join leg, the strict check
            // refused it 17 m up, and the join could never finish. While joining, u is the 3D nearest (CarProgress), so it is the shaft tile.
            {const char* why="";const bool flying=rules.rules.mode==travelrules::Hover||rules.rules.mode==travelrules::SurfaceFirst||rules.rules.mode==travelrules::Legacy;
             const bool onLane=s.trip.leg==trafficroute::Leg::Follow||s.trip.leg==trafficroute::Leg::Join;
             const bool lift=NaturalTravel(Spec(s),s.line,u,why)==routepolicy::Travel::Hover;
             const bool driveSet=SectionAt(s,u).travel==routepolicy::Travel::Drive;
             const char* liftWhy=!lift?"":!flying?"not_flying":driveSet?"drive_set":!onLane?"not_on_lane":!s.turn.empty()?"turning":s.placePath?"place_path":"lift";
             if(std::strcmp(liftWhy,s.liftWhyLogged)!=0){s.liftWhyLogged=liftWhy;
                if(*liftWhy)Log("OBS system=traffic.lift vehicle=%08X event=%s route=%s lane=%d leg=%s u=%.2f tile_why=%s",s.vehicleId,std::strcmp(liftWhy,"lift")==0?"route_rules":"strict_rules",s.route.c_str(),s.trip.lane,s.trip.leg==trafficroute::Leg::Join?"join":s.trip.leg==trafficroute::Leg::Follow?"follow":s.trip.leg==trafficroute::Leg::Climb?"climb":"other",u,liftWhy);}
             if(std::strcmp(liftWhy,"lift")==0){rules.rules=travelrules::Rules{};*out=rules;return 1;}}
            rules.rules.minHeight=std::max(rules.rules.minHeight,restrictions.minHeight);rules.rules.maxHeight=std::min(rules.rules.maxHeight,restrictions.maxHeight);
            if(rules.rules.minHeight<=rules.rules.maxHeight)rules.rules.hoverHeight=std::clamp(rules.rules.hoverHeight,rules.rules.minHeight,rules.rules.maxHeight);
            *out=rules;return 1;
        }
        return 0;
    }
    int FieldProvider(void* user,const FvoFieldPoseV1* pose) {
        std::lock_guard journeyLock(g_journeyMutex);
        if(!pose||!fieldgate::Valid(*pose))return 0;
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used){
            auto& slot=*match;
            if(pose->identity!=slot.guidance.identity)return 0;
            if(!Spec(slot).road.enabled)return 1;
            // Junction connectors, joins and leaves run between fields: not governed by this lane's field.
            if(!slot.turn.empty()||slot.placePath||slot.trip.leg!=trafficroute::Leg::Follow)return 1;
            if(slot.line.error||slot.line.road.empty())return 3;
            // Open-air tiles (vertical shafts) have no field: the vehicle hovers through unconstrained by it.
            {const double u=CarProgress(slot);const int tiles=int(Spec(slot).points.size());
             if(tiles>0&&roadplan::PrimitiveAt(Spec(slot).road,std::size_t(std::clamp(int(slot.line.ToSource(u)),0,tiles-1))).kind==roadplan::Open)return 1;}
            // Easing onto a lift's line leaves the lane's own field on purpose.
            {const double at=CarProgress(slot);
             if(liftpath::At(Spec(slot),slot.line,slot.trip.lane,at).weight>0)return 1;}
            if(pose->probe)return 2;
            return roadfield::Fits(slot.line,slot.trip.lane,*pose)?2:3;
        }
        return 0;
    }
    // The whole road's lanes at a point (roadlanes.h, 2026-11-05): each lane's edges across the road from the car's own lane centre
    // (+ = right of its travel) and whether it runs the same way, so the core can pass through a neighbouring lane. 0 off road routes,
    // junctions, joins and open-air tiles.
    int RoadLanes(void* user,double x,double y,FvoRoadLanesV1* out){
        if(!out||!std::isfinite(x)||!std::isfinite(y))return 0;
        std::lock_guard journeyLock(g_journeyMutex);
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used){
            auto& slot=*match;
            const auto& r=Spec(slot);
            if(!r.road.enabled||slot.line.error||slot.line.road.empty()||!slot.turn.empty()||slot.placePath||slot.trip.leg!=trafficroute::Leg::Follow)return 0;
            // The car's own progress on its lane (as the travel rules and the field use), not the nearest point in 2D: on stacked or
            // parallel legs that was another stretch of the route (2026-11-05 22:24/22:39: lanes answered 11-34 m to the right).
            const int own=slot.trip.lane;const double u=CarProgress(slot);
            const int tiles=int(r.points.size());
            if(tiles>0&&roadplan::PrimitiveAt(r.road,std::size_t(std::clamp(int(slot.line.ToSource(u)),0,tiles-1))).kind==roadplan::Open)return 0;
            return roadfield::LanesAt(slot.line,own,u,*out)?1:0;
        }
        return 0;
    }
    // Field boxes (fieldboxapi.h, the driving side's step 3): the tile under the car and its neighbours, only where the route has explicit
    // road_boxes. D11 is not approved: nothing is derived from stored rounded, tube or trapezoid shapes, and a route without road_boxes
    // (or with text this build cannot read) answers 0, so the core reads the road exactly as before. Placement: tilebox::TileBox (route
    // line, width 0 = road width), the same box the F7 tile check uses.
    int FieldBox(void* user,double x,double y,double z,FvoFieldBoxV1* out,int max){
        (void)z;if(!out||max<=0||!std::isfinite(x)||!std::isfinite(y))return 0;
        std::lock_guard journeyLock(g_journeyMutex);
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used){
            auto& slot=*match;
            const auto& r=Spec(slot);
            if(!r.road.boxesUnsupported.empty())return 0;
            if(slot.line.error||!slot.turn.empty()||slot.placePath||slot.trip.leg!=trafficroute::Leg::Follow)return 0;
            const int tiles=int(r.points.size());if(tiles<2)return 0;
            const double u=CarProgress(slot);const int here=std::clamp(int(slot.line.ToSource(u)),0,tiles-1);
            constexpr double kDeg=3.14159265358979323846/180.0;
            int n=0;
            // FB-1 (2026-11-12, hairpin at 13:56: the car drove against the route's direction, so of here/here-1/here+1 one tile was behind
            // it and one short tile ahead; the plan's line ran past the last box and "leaving the field" refused the route): the car's own
            // tile, then tiles ahead in its direction of travel until they cover kAheadMetres or `max` is reached, then one behind. A caller
            // that asks for 4 gets here + 3 ahead.
            constexpr double kAheadMetres=60.0;
            const int dir=trafficroute::Direction(slot.line,slot.trip.lane)<0?-1:1;
            int order[16];int no=0;order[no++]=here;
            {double covered=0;for(int k=1;k<15&&no<15&&covered<kAheadMetres;++k){int t=here+dir*k;
                if(r.closed)t=((t%tiles)+tiles)%tiles;else if(t<0||t>=tiles)break;
                order[no++]=t;FvoFieldBoxV1 probe;if(tilebox::TileBox(r,slot.line,t,probe))covered+=probe.halfLength*2;}}
            {int t=here-dir;if(r.closed)t=((t%tiles)+tiles)%tiles;if(t>=0&&t<tiles&&no<16)order[no++]=t;}
            for(int oi=0;oi<no;++oi){const int t=order[oi];
                if(n>=max)break;FvoFieldBoxV1 f;
                if(!tilebox::TileBox(r,slot.line,t,f))continue;  // the one placement, shared with the F7 tile check
                f.lineEnd=int(r.end.end);
                if(r.end.end!=0){
                    if(r.end.hasPlace){f.endX=r.end.place.x;f.endY=r.end.place.y;f.endZ=r.end.place.z;f.arriveHeading=r.end.place.heading*kDeg;f.arrivePark=r.end.place.park;}
                    else{const auto e=trafficroute::At(slot.line,slot.trip.lane,slot.line.Span());f.endX=e.x;f.endY=e.y;f.endZ=e.z;f.arriveHeading=std::numeric_limits<double>::quiet_NaN();}
                }
                out[n++]=f;
            }
            return n;
        }
        return 0;
    }
    // The addon's own road: 1 authored surface (drive) with its floor, 2 lift (hover), 0 not this lane's road.
    int RoadSurface(void* user,double x,double y,double* floorZ){
        if(!floorZ||!std::isfinite(x)||!std::isfinite(y))return 0;
        std::lock_guard journeyLock(g_journeyMutex);
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used){
            auto& slot=*match;
            const auto& r=Spec(slot);
            // Following or joining its lane (2026-11-04 14:01: Cruise joining from 10 m was blocked by deck boxes the road answer did not cover).
            if(!r.road.enabled||slot.line.error||slot.line.road.empty()||!slot.turn.empty()||slot.placePath||(slot.trip.leg!=trafficroute::Leg::Follow&&slot.trip.leg!=trafficroute::Leg::Join))return 0;
            const int tiles=int(r.points.size());const double u=trafficroute::Nearest(slot.line,slot.trip.lane,x,y);
            if(tiles>0&&roadplan::PrimitiveAt(r.road,std::size_t(std::clamp(int(slot.line.ToSource(u)),0,tiles-1))).kind==roadplan::Open)return 2;
            double floor=0;if(!roadfield::Floor(slot.line,slot.trip.lane,x,y,floor))return 0;
            *floorZ=floor;return 1;
        }
        return 0;
    }
    int FieldRecovery(void* user,const FvoFieldPoseV1* pose,FvoFieldPoseV1* out){
        if(!pose||!out||!fieldgate::Valid(*pose)||out->size!=sizeof(*out)||out->version!=1)return 0;
        std::lock_guard journeyLock(g_journeyMutex);
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used&&match->guidance.identity==pose->identity){
            auto& slot=*match;
            if(!Spec(slot).road.enabled||slot.line.error||slot.line.road.empty()||
               !slot.turn.empty()||slot.placePath||slot.trip.leg!=trafficroute::Leg::Follow)return 0;
            const double u=trafficroute::Nearest(slot.line,slot.trip.lane,pose->x,pose->y);
            const auto at=trafficroute::At(slot.line,slot.trip.lane,u);
            auto candidate=*pose;double floor=0;
            // Prefer vertical correction at the present footprint before moving toward lane centre.
            for(int pass=0;pass<2;++pass){
                if(pass){candidate.x=at.x;candidate.y=at.y;}
                if(!roadfield::Floor(slot.line,slot.trip.lane,candidate.x,candidate.y,floor))continue;
                // Keep the present hover height when it fits there; otherwise the nearest permitted one, else on the floor.
                {double z=pose->z;candidate.z=fieldgate::FitHeight(candidate,pose->z,[&](const FvoFieldPoseV1& p){return roadfield::Fits(slot.line,slot.trip.lane,p);},z)?z:floor+candidate.originAbove;}
                if(std::hypot(std::hypot(candidate.x-pose->x,candidate.y-pose->y),candidate.z-pose->z)>12)continue;
                if(roadfield::Fits(slot.line,slot.trip.lane,candidate)){*out=candidate;return 1;}
            }
        }
        return 0;
    }
    int CorridorProvider(void* user,FvoCorridorProfileV1* out){std::lock_guard journeyLock(g_journeyMutex);if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;if(auto* match=slotcookie::Find(g_slots,user);match&&match->used){auto& s=*match;FvoCorridorProfileV1 p;p.identity=s.guidance.identity;if(trafficroute::Spatial(s.line)&&!s.placePath&&s.turn.empty()&&s.trip.leg==trafficroute::Leg::Follow){if(s.line.error||s.line.Count()<2)return 0;const int lane=s.trip.lane;const double u=s.trip.uKnown?s.trip.lastU:trafficroute::Nearest(s.line,lane,s.x,s.y);const auto profile=trafficroute::SettingsAt(s.line,u,lane).profile;const auto points=splinekit::Resample(profile,s.line.space.profileSamples);p.active=1;p.count=int(points.size());p.width=profile.width;p.height=profile.height;const auto& basis=s.line.basis[lane];const auto i=std::min(std::size_t(trafficroute::Wrap(s.line,u)),basis.right.size()-1);const auto r=basis.right[i],v=basis.up[i];p.right=lanevolume::Add(lanevolume::Mul(r,std::cos(profile.bank)),lanevolume::Mul(v,std::sin(profile.bank)));p.up=lanevolume::Sub(lanevolume::Mul(v,std::cos(profile.bank)),lanevolume::Mul(r,std::sin(profile.bank)));for(int j=0;j<p.count;++j)p.outline[j]=points[j];}*out=p;return corridorprofile::Valid(p)?1:0;}return 0;}
    int PolicyProvider(void* user,FvoTrafficPolicyV1* out) {
        std::lock_guard journeyLock(g_journeyMutex);
        if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
        if(auto* match=slotcookie::Find(g_slots,user);match&&match->used) {
            auto& s=*match;
            FvoTrafficPolicyV1 p;p.identity=s.guidance.identity;const auto& r=Spec(s);
            if(r.lanePlan.enabled) {
                const double u=CarProgress(s);
                const auto settings=trafficroute::SettingsAt(s.line,u,s.trip.lane);
                p.movement=int(settings.movement);p.obstacles=int(settings.obstacles);p.moving=int(settings.moving);p.people=int(settings.people);p.recovery=settings.recovery;
                p.clearance=settings.movement==laneplan::Movement::Legacy?0:settings.profile.height;p.shape=int(settings.profile.shape);p.cornerRadius=settings.profile.radius;p.topWidth=settings.profile.topWidth;
            }
            if(r.road.enabled){
                // The road field supplies clearance for lane following.
                p.clearance=0;
                p.movement=1;
                p.recovery=0;
            }
            if(r.spacePlan.enabled){p.shape=0;p.clearance=0;if(p.movement==0||p.movement==5)p.movement=1;}
            if(s.placePath){p.clearance=s.manoeuvrePlan.height;p.shape=0;p.cornerRadius=0;p.topWidth=s.manoeuvrePlan.width;if(p.movement==0||p.movement==5)p.movement=1;}
            if(s.pickupWait||s.scriptHolding||s.connectionBlocked||(s.baseId&&trafficdata::LaneLeftOut(g_data.groups,r,s.trip.lane,s.baseId))||(!s.baseId&&!r.lanePlan.vehicles.lanes.empty())){p.movement=1;p.recovery=0;}
            *out=p;return 1;
        }
        return 0;
    }

	// ---- Landing areas: a free space, or what its occupancy rules say to do when it is full ----
	const trafficdata::Place* PlaceNamed(const std::string& name)
	{
		for (const auto& p : g_data.places) if (p.name == name) return &p;
		return nullptr;
	}
	int FreeSpace(const trafficdata::Place& p, const Slot* me)
	{
		const int n = int(trafficdata::Spaces(p).size());
		std::vector<bool> taken(std::size_t(n), false);
		for (const auto& o : g_slots) if (o.used && &o != me && o.place == p.name && o.space >= 0 && o.space < n) taken[std::size_t(o.space)] = true;
		for (int i = 0; i < n; ++i) if (!taken[std::size_t(i)]) return i;
		return -1;
	}
	struct PlacePick { const trafficdata::Place* place = nullptr; int space = -1; bool queue = false; trafficdata::Vec at; };
	PlacePick PickPlace(const char* world, const trafficdata::Route& route, double x, double y, const Slot* me)
	{
		PlacePick pick;
		std::vector<const trafficdata::Place*> ok;
		for (const auto& p : g_data.places)
			if (trafficdata::SameWorld(g_data, p.world, world) && std::hypot(p.at.x - x, p.at.y - y) >= kMinTripDistance && trafficdata::RouteMayPark(p, route.name, route.role)&&placeaccess::Valid(p.access)&&(p.access.entryRoute.empty()||p.access.entryRoute==route.name)) ok.push_back(&p);
		if (ok.empty()) return pick;
		const std::size_t first = std::size_t(g_nextPlace++);
		for (std::size_t k = 0; k < ok.size(); ++k) {
			const auto* p = ok[(first + k) % ok.size()];
			const int space = FreeSpace(*p, me);
			if (space >= 0) { pick.place = p; pick.space = space; pick.at = trafficdata::Spaces(*p)[std::size_t(space)]; return pick; }
			if (p->full == "circle") return pick;  // another lap, then it looks again
			if (p->full == "queue" && route.travel!=routepolicy::Travel::Drive && (!route.lanePlan.enabled||(laneplan::Resolve(route.lanePlan).rules.mode!=travelrules::Drive&&laneplan::Resolve(route.lanePlan).rules.mode!=travelrules::SurfaceFirst))) { pick.place = p; pick.queue = true; pick.at = p->at; return pick; }
			// "on": the next area
		}
		return pick;
	}
	double ExitU(const trafficdata::Route& r, const trafficroute::Line& line, double at);
	// The exit toward a place: its own, else the route's exit nearest it, else the route point nearest it.
	double ExitToward(const trafficdata::Route& route, const trafficroute::Line& line, const trafficdata::Place& place)
	{
		double exitU = trafficroute::Nearest(line, 0, place.at.x, place.at.y), exitD = 1e18;
		for (const auto& e : g_data.exits) {
			if (e.route != route.name) continue;
			const double u = ExitU(route, line, e.at);
			const auto p = trafficroute::At(line, 0, u);
			const double d = e.name == place.exitName ? -1.0 : std::hypot(p.x - place.at.x, p.y - place.at.y);
			if (d < exitD) { exitD = d; exitU = u; }
		}
        // Leave the route where this route's In wire starts (the first In wire otherwise, as before).
        const placeaccess::Wire* in=nullptr;for(const auto& w:place.wires)if(w.socket==0&&w.route==route.name&&!w.path.empty()){in=&w;break;}
        if(in){const auto& point=in->path.front();const int lane=in->lane>=0?in->lane:trafficroute::PickLane(line,route.lanes,0);if(lane>=0)return trafficroute::Nearest(line,lane,point.x,point.y);}
        if(!place.access.entrance.empty()){const auto& point=place.access.entrance.front();const int lane=place.access.entryLane>=0?place.access.entryLane:trafficroute::PickLane(line,route.lanes,0);if(lane>=0)return trafficroute::Nearest(line,lane,point.x,point.y);}
		return exitU;
	}
	// Landing wires (F7 spec 7b): arrive on this route's In wire (else the first one), leave on an Out wire chosen with the
	// Cruise repeat damping so a landing with several departures spreads them.
	placeaccess::Plan WirePlan(Slot& s,const trafficdata::Place& place){
		const placeaccess::Wire* in=nullptr;const placeaccess::Wire* out=nullptr;
		for(const auto& w:place.wires)if(w.socket==0&&w.route==s.route){in=&w;break;}
		const bool own=in!=nullptr;if(!in)for(const auto& w:place.wires)if(w.socket==0){in=&w;break;}
		std::vector<const placeaccess::Wire*> outs;for(const auto& w:place.wires)if(w.socket==1)outs.push_back(&w);
		if(!outs.empty()){std::vector<int> picks;for(const auto* w:outs)picks.push_back(s.cruisePicks["out@"+place.name+"|"+w->route]);
			out=outs[trafficlinks::CruisePick(trafficlinks::CruiseWeights(picks),trafficlinks::Roll(s.dice))];++s.cruisePicks["out@"+place.name+"|"+out->route];}
		if(place.wires.size()>1||(in&&!own))Log("OBS system=traffic.place vehicle=%08X event=%s place=%s route=%s in=%s out=%s outs=%zu reason=%s",s.vehicleId,in&&!own?"wire_refused":"wire_used",place.name.c_str(),s.route.c_str(),
			in?in->route.c_str():"-",out?out->route.c_str():"-",outs.size(),in&&!own?"no_in_wire_from_this_route_first_used":"wire_choice");
		return placeaccess::Compose(place.access,in,out);
	}
	void TakePlace(Slot& s, const PlacePick& pick) { s.accessPlan=WirePlan(s,*pick.place);s.placeApproach=false;s.placePath=false;s.place = pick.place->name; s.space = pick.space; s.queue = pick.queue; s.stay = pick.place->stay; s.stayUntil = 0; s.dest = pick.place->name; }

    bool PlacePath(Slot& s,bool entering,trafficdata::Vec current,trafficdata::Vec destination){
        if(!placeaccess::Valid(s.accessPlan)){s.connectionBlocked=true;return false;}
        const auto authored=placeaccess::Path(s.accessPlan,entering,{current.x,current.y,current.z},{destination.x,destination.y,destination.z});
        if(authored.empty())return true;std::vector<trafficdata::Vec> points;for(const auto& p:authored)points.push_back({p.x,p.y,p.z});
        std::vector<double> rounding(points.size(),std::min(5.0,s.accessPlan.width));if(!rounding.empty())rounding.front()=rounding.back()=0;const auto path=routecurve::Build(points,rounding,false).points;if(path.empty()){s.connectionBlocked=true;return false;}
        const double margin=std::max(4.0,s.accessPlan.width*.5);
        for(const auto& other:g_slots)if(other.used&&&other!=&s){for(std::size_t i=0;i<path.size();i+=2){const auto p=path[i];if(std::hypot(p.x-other.x,p.y-other.y,p.z-other.z)<margin){s.connectionBlocked=true;return false;}for(std::size_t j=other.turnAt;j<other.turn.size();j+=2){const auto q=other.turn[j];if(std::hypot(p.x-q.x,p.y-q.y,p.z-q.z)<margin){s.connectionBlocked=true;return false;}}}}
        s.manoeuvrePlan=s.accessPlan;s.turn=path;s.turnAt=0;s.turnFrom.clear();s.placePath=true;s.connectionBlocked=false;return true;
    }
	bool TurnGoal(Slot& s, double x, double y, double z, FvoTripGoal* out)
	{
        if(s.turn.empty())return false;
        while(s.turnAt+1<s.turn.size()&&std::hypot(s.turn[s.turnAt+1].x-x,s.turn[s.turnAt+1].y-y,s.turn[s.turnAt+1].z-z)<2.5)++s.turnAt;
        while(s.turnAt<s.turn.size()&&std::hypot(std::hypot(s.turn[s.turnAt].x-x,s.turn[s.turnAt].y-y),s.turn[s.turnAt].z-z)<5)++s.turnAt;
        if(s.turnAt<s.turn.size()){
            const auto& goal=s.turn[std::min(s.turnAt+1,s.turn.size()-1)];
            out->x=goal.x;out->y=goal.y;out->z=goal.z;out->stop=1;out->passThrough=1;out->fly=1;out->speed=float(std::min(s.placePath?3.0:15.0,s.speed));ZoneLimits(s,x,y,out,true);return true;
        }
        s.turn.clear();s.turnFrom.clear();s.turnFromLine={};s.trip.uKnown=false;s.placePath=false;return false;
	}
    void StepFactoryRoute(Slot& s, double x, double y, double z, FvoTripGoal& out) {
        const auto hold = [&] { out = {x, y, z, 1.0, 0, 0, 0.0f}; };
        if(s.factoryPhase!=FactoryRoutePhase::Departing) { hold(); return; }
        if(TurnGoal(s,x,y,z,&out)) { out.fly=0; return; }
        const double u=trafficroute::NearestProgress(s.line,s.trip.lane,x,y,z,s.trip.uKnown,s.trip.lastU);
        if(s.route!=s.factoryDestination&&!s.junctions.empty()) {
            const auto exit=s.junctions.front();
            if(trafficroute::DistanceAlong(s.line,s.trip.lane,u,exit.u)<=std::max(3.0,2.5*exit.radius)) {
                if(TakeJunction(s,exit,true)) {
                    if(TurnGoal(s,x,y,z,&out)) { out.fly=0; return; }
                } else if(trafficroute::DistanceAlong(s.line,s.trip.lane,u,exit.u)<=1.0) {
                    s.connectionBlocked=true;
                    hold();
                    return;
                }
            }
        }
        const double progress=trafficroute::NearestProgress(s.line,s.trip.lane,x,y,z,s.trip.uKnown,s.trip.lastU);
        s.trip.lastU=progress;
        s.trip.uKnown=true;
        if(s.route==s.factoryDestination) {
            if(spawnflow::FactoryPose(x-s.factoryArrival.x,y-s.factoryArrival.y,z-s.factoryArrival.z)) {
                s.factoryPhase=FactoryRoutePhase::Arrival;
                Log("OBS system=traffic.factory_site vehicle=%08X event=arrival_hold route=%s lane=%d",s.vehicleId,s.route.c_str(),s.trip.lane);
                hold();
            } else out={s.factoryArrival.x,s.factoryArrival.y,s.factoryArrival.z,1.0,0,0,float(s.speed)};
            return;
        }
        const auto goal=trafficroute::At(s.line,s.trip.lane,trafficroute::LeadU(s.line,s.trip.lane,progress,trafficroute::kLead));
        out={goal.x,goal.y,goal.z,1.0,1,0,float(s.speed)};
    }
	int Step(void* user, double x, double y, double z, FvoTripGoal* out)
	{
        std::lock_guard journeyLock(g_journeyMutex);
		Slot* s=slotcookie::Find(g_slots,user);if(!s||!s->used||!out)return 1;
        if(StockRoutePending(*s)) {
            s->seen=GetTickCount64();
            const auto& at=s->factoryArrival;
            if(s->factoryPhase!=FactoryRoutePhase::StockCommitted||!spawnflow::FactoryPose(x-at.x,y-at.y,z-at.z)) {
                observations::Record(g_log,GetTickCount64(),"traffic.factory_route",s->vehicleId,0,
                    "event=arrival_hold reason=await_destination_sample");
                *out={x,y,z,1.0,0,0,0.0f};
                return 0;
            }
            // Only the retained pilot may call this claim after Core applies the transfer. Seed the
            // existing route follower with the destination sample, without inheriting a stock pose or speed.
            s->trip=trafficroute::StartLap(s->line,s->trip.lane,{x,y,z});
            s->trip.leg=trafficroute::Leg::Follow;
            s->trip.lastU=trafficroute::NearestProgress(s->line,s->trip.lane,x,y,z,true,s->factoryArrivalU);
            s->trip.uKnown=true;
            s->x=x;s->y=y;s->z=z;
            s->logged=s->trip.leg;
            s->factoryPhase=FactoryRoutePhase::None;
            s->stockRoadSlot=-1;
            FindJunctions(*s);
            Log("OBS system=traffic.factory_route vehicle=%08X event=active route=%s lane=%d reason=seated_arrival_goal_requested",
                s->vehicleId,s->route.c_str(),s->trip.lane);
        }
        static thread_local int depth=0;struct Depth{int& value;Depth(int& v):value(v){++value;}~Depth(){--value;}} depthGuard(depth);
        if(depth>4){out->x=x;out->y=y;out->z=z;out->stop=1;out->speed=0;out->fly=1;s->scriptReason="script transition budget; resume next update";return 0;}
		s->seen = GetTickCount64();s->connectionBlocked=false;
		if (s->flownStart) s->flown += std::hypot(std::hypot(x - s->x, y - s->y), z - s->z);
		s->flownStart = true;
        const double tick=GetTickCount64()*0.001;
        if(s->stepAt>0 && tick>s->stepAt){
            const double dt=tick-s->stepAt;
            s->stepVelocity={(x-s->x)/dt,(y-s->y)/dt,(z-s->z)/dt};
            s->measuredSpeed=std::hypot(s->stepVelocity.x,s->stepVelocity.y);
        }
        s->stepAt=tick;
		s->x = x; s->y = y; s->z = z;
        if(s->factoryPhase!=FactoryRoutePhase::None) { StepFactoryRoute(*s,x,y,z,*out); return 0; }
        if(s->pickupWait){const auto* place=PlaceNamed(s->place);const int space=place?FreeSpace(*place,s):-1;if(!place||space<0){s->holding=true;out->x=x;out->y=y;out->z=z;out->stop=1;out->speed=0;out->fly=1;return 0;}s->pickupWait=false;s->holding=false;s->space=space;s->trip.dest=trafficdata::Spaces(*place)[std::size_t(space)];}
        if(TurnGoal(*s,x,y,z,out))return 0;
        if(s->trip.leg==trafficroute::Leg::Follow&&!s->itinerary.routes.empty()&&s->itineraryAt+1<s->itinerary.routes.size()) {
            const auto at=s->itineraryAt;
            for(const auto& k:s->junctions)if(k.other==s->itinerary.routes[at+1]&&std::fabs(k.u-s->itinerary.exits[at])<0.01){
                const double u=trafficroute::NearestProgress(s->line,s->trip.lane,x,y,z,s->trip.uKnown,s->trip.lastU);
                const double distance=trafficroute::DistanceAlong(s->line,s->trip.lane,u,k.u);
                if(distance<=std::max(3.0,2.5*k.radius)&&TakeJunction(*s,k,true)){
                    ++s->itineraryAt;s->trip.lap=false;s->trip.targetU=s->itinerary.exits[s->itineraryAt];s->trip.lane=s->itinerary.lanes[s->itineraryAt];
                    if(TurnGoal(*s,x,y,z,out))return 0;
                }
                break;
            }
        }
        if(s->trip.leg==trafficroute::Leg::Leave&&!s->place.empty()&&!s->placeApproach&&!s->accessPlan.entrance.empty()){
            if(!PlacePath(*s,true,{x,y,z},s->trip.dest)){out->x=x;out->y=y;out->z=z;out->speed=0;out->stop=1;out->fly=1;return 0;}s->placeApproach=true;if(TurnGoal(*s,x,y,z,out))return 0;
        }
		// Check the slot before Step can promote Join to Follow inside kReach.
		const auto localRule=SectionAt(*s,trafficroute::Nearest(s->line,s->trip.lane,x,y));
        if(s->trip.leg==trafficroute::Leg::Climb && localRule.travel!=routepolicy::Travel::Hover)s->trip.leg=trafficroute::Leg::Join;
        const auto before = s->trip;
		auto g = trafficroute::Step(s->line, s->trip, x, y, z,3.0,std::clamp(s->speed*0.6,4.0,18.0));
        observations::Record(g_log,GetTickCount64(),"route.goal",s->vehicleId,unsigned(s->trip.leg)|(std::uint64_t(s->trip.lane)<<8)|(std::uint64_t(s->holding)<<16),
            "route=%s lane=%d leg=%s holding=%d repeat=%d pos=%.2f,%.2f,%.2f goal=%.2f,%.2f,%.2f speed=%.2f flown=%.2f",s->route.c_str(),s->trip.lane,trafficroute::Name(s->trip.leg),s->holding,s->repeat,x,y,z,g.x,g.y,g.z,s->speed,s->flown);
		// The join point in 3D: in a vertical shaft a 2D nearest lands anywhere up it (2026-11-06 15:47).
		if (before.leg == trafficroute::Leg::Join && s->trip.leg == trafficroute::Leg::Follow) {
			const auto join = trafficroute::At(s->line, s->trip.lane, trafficroute::NearestProgress(s->line, s->trip.lane, x, y, z, false, 0.0));
			if (SlotTaken(*s, join)) s->trip = before;
		}
		if (s->trip.leg == trafficroute::Leg::Join) {
			const auto join = trafficroute::At(s->line, s->trip.lane, trafficroute::NearestProgress(s->line, s->trip.lane, x, y, z, false, 0.0));
			const bool taken = SlotTaken(*s, join);
			if (taken != s->holding) {
				s->holding = taken;
				Log("FVO traffic: %s lane %d: %s", s->route.c_str(), s->trip.lane, taken ? "slot taken, waiting beside the lane" : "slot free, joining");
			}
			if (taken) { g.x = x; g.y = y; g.z = join.z; g.stop = 3.0; g.passThrough = false; }
		} else if (s->holding && !s->pickupWait) {
			// JH-1 (2026-11-13 07:34 run: your car held at 0 m/s for minutes, "route hold: holding"): the slot hold is only written while
			// joining, so when Step promoted Join to Follow on a frame the slot was free, the hold set the frame before was never cleared and
			// the lane stayed held with the car already following. Leaving the join leg ends its slot hold.
			s->holding = false;
			Log("FVO traffic: %s lane %d: slot hold cleared, the trip left the join leg (%s)", s->route.c_str(), s->trip.lane, trafficroute::Name(s->trip.leg));
			Log("OBS system=traffic.join vehicle=%08X event=slot_hold_cleared leg=%s reason=left_join_leg_with_a_stale_slot_hold", s->vehicleId, trafficroute::Name(s->trip.leg));
		}
        if(!s->itinerary.routes.empty() && s->itineraryAt+1<s->itinerary.routes.size() && s->trip.leg==trafficroute::Leg::Leave) {
            const auto at=s->itineraryAt;
            const trafficlinks::Junction* link=nullptr;
            for(const auto& k:s->junctions)if(k.other==s->itinerary.routes[at+1] && std::fabs(k.u-s->itinerary.exits[at])<0.01){link=&k;break;}
            if(link && TakeJunction(*s,*link,true)) {
                ++s->itineraryAt;s->trip.lap=false;s->trip.targetU=s->itinerary.exits[s->itineraryAt];s->trip.lane=s->itinerary.lanes[s->itineraryAt];
                g=trafficroute::Step(s->line,s->trip,x,y,z,3.0,std::clamp(s->speed*0.6,4.0,18.0));
            } else {out->x=x;out->y=y;out->z=z;out->stop=1;out->fly=1;out->speed=0;return 0;}
        }
		if (!s->junctions.empty() || s->route != s->home) {
			const std::string was = s->route;
			const bool wasLap = s->trip.lap;
			Junctions(*s, before, x, y);
            if(TurnGoal(*s,x,y,z,out))return 0;
			if (s->route != was || s->trip.lap != wasLap) g = trafficroute::Step(s->line, s->trip, x, y, z,3.0,std::clamp(s->speed*0.6,4.0,18.0));
		}
		if (s->trip.leg != s->logged) {
			s->logged = s->trip.leg;
			Log("FVO traffic: %s lane %d trip%s%s: %s", s->route.c_str(), s->trip.lane, s->dest.empty() ? "" : " to ", s->dest.c_str(), trafficroute::Name(s->trip.leg));
		}
		if (s->trip.leg == trafficroute::Leg::Done && !s->trip.lap && s->queue) {
			// Queue: it waits above the area until a space frees, then lands in it.
			const auto* p = PlaceNamed(s->place);
			const int space = p ? FreeSpace(*p, s) : -1;
			if (p && space < 0) {
				out->x = p->at.x; out->y = p->at.y; out->z = p->at.z + 25.0; out->stop = 3.0; out->passThrough = 0; out->fly = 1; out->speed = 8.0f;
				return 0;
			}
			s->queue = false;
			if (p) { s->space = space; s->trip.dest = trafficdata::Spaces(*p)[std::size_t(space)]; Log("FVO traffic: %s: a space freed at %s (space %d)", s->route.c_str(), p->name.c_str(), space + 1); }
		}
        if(s->trip.leg==trafficroute::Leg::Done&&!s->script.empty()) {
            const auto& stop=s->script[s->scriptAt];const double seconds=GetTickCount64()*0.001;
            const bool arrived=std::hypot(x-stop.at.x,y-stop.at.y,z-stop.at.z)<3&&s->measuredSpeed<0.3;
            s->scriptHolding=true;
            if(arrived&&!s->scriptDwell)s->scriptDwell=seconds;
            if(!arrived)s->scriptDwell=0;
            bool advance=s->scriptDwell&&seconds-s->scriptDwell>=stop.dwell;int nextIndex=s->scriptAt+1<s->script.size()?int(s->scriptAt+1):s->authoredJourney.loop?0:-1;
            if(s->scriptAt<s->authoredJourney.stops.size()){
                bool gap=true;for(const auto& other:g_slots)if(other.used&&&other!=s&&std::fabs(other.z-z)<5&&std::hypot(other.x-x,other.y-y)<12)gap=false;
                const auto& stage=s->authoredJourney.stops[s->scriptAt];const auto decision=journeyplan::Evaluate(s->authoredJourney,s->scriptAt,{arrived,gap,g_roadDecision.find(s->vehicleId)==g_roadDecision.end(),s->scriptSignal==stage.trigger,s->scriptDwell?seconds-s->scriptDwell:0});
                advance=decision.advance;nextIndex=decision.next;s->scriptReason=decision.reason;
            }
            if(!advance){out->x=stop.at.x;out->y=stop.at.y;out->z=stop.at.z;out->stop=1;out->fly=1;out->passThrough=0;out->speed=0;return 0;}
            if(nextIndex>=0&&nextIndex<int(s->script.size())) {
                const auto nextAt=std::size_t(nextIndex);const auto& next=s->script[nextAt];BuildRoad();const auto journey=PlanNetwork({x,y,z},next.at,RuleFor(g_roadWorld.c_str()).reach,next.routes);
                if(journey.routes.empty()){out->x=x;out->y=y;out->z=z;out->stop=1;out->fly=1;return 0;}
                const auto* route=trafficlinks::Find(WorldRoutes(g_roadWorld.c_str()),journey.routes.front());if(!route)return 0;
                s->scriptAt=nextAt;s->scriptSignal=0;s->scriptHolding=false;s->scriptDwell=0;s->itinerary=journey;s->itineraryAt=0;CommitNetwork(*s,g_road);s->spec=*route;s->route=route->name;s->line=trafficroute::Build(*route);s->speed=RoleSpeed(*route);
                s->trip=trafficroute::StartTo(s->line,route->lanes,{x,y,z},next.at,journey.exits.front());s->trip.lane=journey.lanes.front();FindJunctions(*s);
                return Step(user,x,y,z,out);
            }
        }
		if (s->trip.leg == trafficroute::Leg::Done) {
			if (s->flown < kEmptyTripFlown && s->vehicleId) {
				auto& e = g_emptyTrips[s->vehicleId];
				if (e.first != s->route || GetTickCount64() - e.second >= kEmptyTripMs)
					Log("FVO traffic: vehicle %08X: its trip on %s ended where it began (%.0f m flown; too short from here); that route is skipped for it for %d s",
						s->vehicleId, s->route.c_str(), s->flown, int(kEmptyTripMs / 1000));
				e = { s->route, GetTickCount64() };
			}
			s->used = s->repeat;
			if (!s->trip.lap) { out->x = s->trip.dest.x; out->y = s->trip.dest.y; out->z = s->trip.dest.z; return s->repeat ? 4 : 2; }
			return s->repeat ? 3 : 1;
		}
		out->x = g.x; out->y = g.y; out->z = g.z; out->stop = g.stop;
		out->passThrough = g.passThrough ? 1 : 0;
		out->fly = 1;  // routes are flight lanes
		out->speed = float(s->trip.leg == trafficroute::Leg::Follow ? s->speed : 15.0);
		ZoneLimits(*s, x, y, out, s->trip.leg == trafficroute::Leg::Follow);
		if (s->trip.leg == trafficroute::Leg::Follow) CrossLimit(*s, x, y, out);
		return 0;
	}

	int Cycle(void* user, int restart, double x, double y, double z)
	{
        std::lock_guard journeyLock(g_journeyMutex);
		Slot* s = slotcookie::Find(g_slots, user);
		if (!s || !s->used || !s->repeat || s->trip.leg != trafficroute::Leg::Done) return 0;
		if(populationinspect::Surplus(s->vehicleId)){
			s->repeat=false;s->used=false;
			Log("OBS system=traffic.surplus vehicle=%08X event=cycle_ended reason=fleet_target_reduced",s->vehicleId);
			return 0;
		}
		s->seen = GetTickCount64();
		s->x = x; s->y = y; s->z = z;
		if (restart && s->stay > 0 && !s->place.empty()) {
			// Occupancy rules: it stays parked in its space this long (counted from when it has landed).
			const ULONGLONG now = GetTickCount64();
			if (!s->stayUntil) s->stayUntil = now + ULONGLONG(s->stay * 1000.0);
			if (now < s->stayUntil) return 0;
		}
		if (restart) {
			// The landing is the safe moment to take up edits (2026-10-28, the map route editor): the route as the files say now,
			// if it is still active and has room; else the nearest route that is; else the vehicle stays parked (return 0). The files
			// themselves are re-read on demand (Load: a reload request from the menu or the map route editor).
			char world[201]{};
			if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return 0;
			const auto rule = RuleFor(world);
			const trafficdata::Route* route = nullptr;
			const std::string own = s->home.empty() ? s->route : s->home;  // a spur's vehicle flies home to its own route
            if(!s->accessPlan.exitRoute.empty())for(const auto& candidate:g_data.routes)if(candidate.name==s->accessPlan.exitRoute&&trafficdata::SameWorld(g_data,candidate.world,world)&&Room(candidate,rule,s,false)&&!Excluded(candidate,s->vehicleId)){route=&candidate;break;}
			if (!s->home.empty())
				for (const auto& r : g_data.routes)
					if (!route && r.name == own && trafficdata::SameWorld(g_data, r.world, world) && Room(r, rule, s, false) && !EmptyHere(s->vehicleId, r.name) && !Excluded(r, s->vehicleId)) { route = &r; break; }
			if (!route) route = FairRoute(world, rule, s, false, x, y, EmptyHere(s->vehicleId, own) ? std::string() : own, nullptr, s->vehicleId);  // fair share; its own route on a tie
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
            Slot staged=*s;staged.script.clear();staged.scriptAt=0;staged.scriptHolding=false;staged.scriptDwell=0;staged.authoredJourney={};
            if(!AuthoredJourney(staged,*route,world,{x,y,z},rule.reach))return 0;
            if(route->journey.enabled){*s=std::move(staged);s->flown=0;s->flownStart=true;s->logged=s->trip.leg;s->holding=false;return 1;}
            s->script.clear();s->scriptHolding=false;s->scriptDwell=0;s->scriptAt=0;s->authoredJourney={};
            const auto departure=s->accessPlan;const bool haveDeparture=!departure.departure.empty();
            if(haveDeparture){s->accessPlan=departure;const auto nextLine=trafficroute::Build(*route);const int nextLane=departure.exitLane>=0?departure.exitLane:trafficroute::KeepLane(nextLine,route->lanes,s->trip.lane);if(nextLane<0||!trafficroute::HasLane(nextLine,route->lanes,nextLane))return 0;
                if(!departure.exitRoute.empty()&&departure.exitRoute!=route->name)return 0;const auto& anchor=departure.departure.back();const auto u=trafficroute::Nearest(nextLine,nextLane,anchor.x,anchor.y);const auto join=trafficroute::At(nextLine,nextLane,u);if(!PlacePath(*s,false,{x,y,z},join))return 0;}
			s->line = trafficroute::Build(*route);s->spec=*route;s->completedControl=-1;s->controlStopped=0;
			s->speed = RoleSpeed(*route); s->route = route->name;
			const int lane = trafficroute::KeepLane(s->line,route->lanes,s->trip.lane);
			s->place.clear(); s->space = -1; s->queue = false; s->stay = 0; s->stayUntil = 0;
			// Landing areas are used lap after lap, not only by a vehicle's first trip: places, or every other trip with mixed.
			const bool toPlace = rule.trips == trafficdata::Trips::Places || (rule.trips == trafficdata::Trips::Mixed && (g_mixedPlace = !g_mixedPlace));
			const auto pick = toPlace ? PickPlace(world, *route, x, y, s) : PlacePick{};
			s->dest.clear();
			if (pick.place) {
				s->trip = trafficroute::StartTo(s->line, route->lanes, { x, y, z }, pick.at, ExitToward(*route, s->line, *pick.place));
				TakePlace(*s, pick);if(s->accessPlan.entryLane>=0){if(!trafficroute::HasLane(s->line,route->lanes,s->accessPlan.entryLane))return 0;s->trip.lane=s->accessPlan.entryLane;}
			}
			else s->trip = trafficroute::StartLap(s->line, lane, {x, y, z});
			s->flown = 0; s->flownStart = true;
			s->logged = s->trip.leg; s->holding = false;if(haveDeparture)s->placePath=true;
			s->home = s->route; FindJunctions(*s);
			Log("FVO traffic: %s lane %d: landed dwell complete; %s%s (route as saved now)", s->route.c_str(), s->trip.lane, pick.place ? "flying to " : "starting another full lap", pick.place ? pick.place->name.c_str() : "");
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
        std::lock_guard journeyLock(g_journeyMutex);
		const ULONGLONG now = GetTickCount64();
		active = 0;
		Slot* free = nullptr;
		for (auto& s : g_slots) {
            if(s.used&&now-s.seen>kLostMs&&!(s.vehicleId&&g_trafficApi&&
                (engineport::Forward<&FvoTrafficApi::usesVehicle>(g_trafficApi,"usesVehicle","routes",0u,s.vehicleId)||(g_trafficApi->tripActive&&engineport::Forward<&FvoTrafficApi::tripActive>(g_trafficApi,"tripActive","routes",0u,s.vehicleId))))){if(s.vehicleId&&s.factoryPhase==FactoryRoutePhase::None)g_rememberedJourneys[s.vehicleId]=s;s.used=false;}
			if (s.used) ++active; else if (!free) free = &s;
		}
		return free;
	}

    Slot* VehicleSlot(unsigned vehicle,int& active){Slot* free=FreeSlot(active);if(vehicle)for(auto& s:g_slots)if(s.used&&s.vehicleId==vehicle)return StockRoutePending(s)?nullptr:&s;return free;}

	int Offer(double x, double y, double z, unsigned vehicleId, FvoTripClaim* out)
	{
		Load();
        {
            std::lock_guard lock(g_journeyMutex);
            if(g_factoryRoute&&vehicleId==g_factoryRoute->bay.vehicle)return 0;  // reserved authored inventory
        }
		const ULONGLONG now = GetTickCount64();
		char world[201]{};
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return 0;
		const auto rule = RuleFor(world);
		int active = 0;
		Slot* free = VehicleSlot(vehicleId,active);
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
	// SR-1: one trace line per change of a vehicle's trip answer (refused with its reason, or granted).
	void TraceTrip(int spot, unsigned vehicle, int ok, int active) {
		if (!vehicle) return;
		static std::map<unsigned, std::string> told;
		const auto it = g_tripRefusal.find(vehicle);
		const std::string now = ok ? std::string("granted") : it != g_tripRefusal.end() ? std::string(it->second.why) : std::string("unknown");
		if (told[vehicle] == now) return;
		told[vehicle] = now;
		if (ok) Log("OBS system=traffic.trip vehicle=%08X event=granted slot=%d active=%d cap=%d", vehicle, spot, active, kSlots);
		else Log("OBS system=traffic.trip vehicle=%08X event=refused slot=%d reason=%s on=%d want=%d active=%d cap=%d", vehicle, spot, now.c_str(),
			it != g_tripRefusal.end() ? it->second.on : -1, it != g_tripRefusal.end() ? it->second.want : -1, active, kSlots);
	}
	// SR-1 / SR-3 readers for the generator (Health and Demand read; they never decide here).
	const char* TripRefusalOf(unsigned vehicle, int* on, int* want) {
		const auto it = g_tripRefusal.find(vehicle); if (it == g_tripRefusal.end()) return nullptr;
		if (on) *on = it->second.on; if (want) *want = it->second.want; return it->second.why;
	}
	// SR-3 reader: trip room left on a cold-start slot's route now (Room's own counts: the route's wanted count less the trips on it;
	// 0 when the route is full, inactive or the city is at its limit). -1: unknown (no such slot or route here).
	int InitialRouteRoom(int slot, int* on, int* want, const char** route) {
		if (slot < 0 || slot >= int(g_initialSlots.size())) return -1;
		const auto& is = g_initialSlots[std::size_t(slot)]; if (route) *route = is.route.c_str();
		if (!on && !want) return 0;  // only the route name was asked for
		char world[201]{};
		if (is.route.empty() || !g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return -1;
		std::lock_guard journeyLock(g_journeyMutex);
		for (const auto& r : g_data.routes) if (r.name == is.route && trafficdata::SameWorld(g_data, r.world, world)) {
			int o = 0, w = 0; const char* rw = ""; const bool room = Room(r, RuleFor(world), nullptr, true, &o, &w, &rw);  // RR-1
			{ static std::map<int, std::string> told; if (!room && told[slot] != rw) { told[slot] = rw; Log("OBS system=traffic.room slot=%d route=%s event=no_room reason=%s on=%d want=%d", slot, is.route.c_str(), rw, o, w); } else if (room) told.erase(slot); }
			if (on) *on = o; if (want) *want = w; return room ? std::max(0, w - o) : 0;
		}
		return -1;
	}
	// The generator's population trips: it asks for its vehicle's trip once the spawned driver sits, always with a claim to fill
	// (population vehicles do not count against `max`).
	int PoolTrip(int spot, unsigned vehicleId, double x, double y, double z, FvoTripClaim* out)
	{
		char world[201]{};
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return RefuseTrip(vehicleId, "no_world_name");
		if (!vehicleId && !out) return Wanted(world, x, y) ? 1 : 0;
		int active = 0;
		Slot* free = VehicleSlot(vehicleId,active);
		if (!free) { RefuseTrip(vehicleId, "no_free_trip_slot", active, kSlots); TraceTrip(spot, vehicleId, 0, active); return 0; }
        const bool initial=generator::InitialVehicle(spot)&&spot>=0&&spot<int(g_initialSlots.size());
        const int ok = Begin(RuleFor(world), world, free, x, y, z, vehicleId, out, true,
            initial?g_initialSlots[spot].route:std::string(),initial?g_initialSlots[spot].lane:-1);
		if (ok) Log("FVO traffic: cold-start spot %d: vehicle %08X takes a trip", spot, vehicleId);
		TraceTrip(spot, vehicleId, ok, active);  // SR-1
		return ok;
	}

    int FactoryTrip(generator::FactoryTripAction action,unsigned vehicle,double x,double y,double z,FvoTripClaim* claim) {
        using Action=generator::FactoryTripAction;
        std::lock_guard lock(g_journeyMutex);
        Slot* current=nullptr;
        for(auto& slot:g_slots)if(slot.used&&slot.vehicleId==vehicle){current=&slot;break;}
        if(current&&StockRoutePending(*current))return 0;
        if(action==Action::Complete) {
            if(!current||current->factoryPhase!=FactoryRoutePhase::Arrival)return 0;
            auto& s=*current;
            s.factoryPhase=FactoryRoutePhase::None;
            s.factoryDestination.clear();
            s.plannedRoutes.clear();
            s.plannedLinks.clear();
            s.networkSnapshot=false;
            s.home=s.route;
            s.trip=trafficroute::StartLap(s.line,s.trip.lane,{x,y,z});
            s.trip.leg=trafficroute::Leg::Follow;
            s.trip.lastU=trafficroute::Nearest(s.line,s.trip.lane,x,y);
            s.trip.uKnown=true;
            s.repeat=g_cycleSupported;
            FindJunctions(s);
            return 1;
        }
        if(action==Action::Rollback) {
            if(!current||current->factoryPhase==FactoryRoutePhase::None||
                current->factoryPhase==FactoryRoutePhase::Arrival||current->route==current->factoryDestination)return 0;
            current->factoryPhase=FactoryRoutePhase::Test;
            current->factoryDestination.clear();
            current->connectionBlocked=false;
            return 1;
        }
        if(!g_factoryRoute||vehicle!=g_factoryRoute->bay.vehicle)return 0;
        const auto& site=*g_factoryRoute;
        if(action==Action::Dispatch) {
            if(!current||current->factoryPhase!=FactoryRoutePhase::Test||current->world!=site.world||
                routecache::Key(current->spec)!=routecache::Key(site.lane))return 0;
            const auto* road=trafficlinks::Find(WorldRoutes(current->world.c_str()),site.exit.other);
            const auto rule=RuleFor(current->world.c_str());
            if(!road||!Allowed(*road,rule)||!Room(*road,rule,current,true)||Excluded(*road,vehicle,site.exit.toLane))return 0;
            current->plannedRoutes={site.lane,*road};
            current->junctions={site.exit};
            current->networkSnapshot=true;
            current->factoryDestination=road->name;
            current->factoryArrival=site.bay.arrival;
            current->factoryArrivalU=site.arrivalU;
            current->factoryPhase=FactoryRoutePhase::Departing;
            return 1;
        }
        if(action!=Action::Test||!claim||g_rememberedJourneys.contains(vehicle)||
            (current&&current->factoryPhase!=FactoryRoutePhase::Test))return 0;
        if(!current) { int active=0;current=FreeSlot(active); }
        if(!current)return 0;
        const unsigned base=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicle):0;
        if(!base||Excluded(site.lane,vehicle,site.privateLane))return 0;
        if(!current->used) {
            auto& s=*current;
            s=Slot{};
            s.used=true;s.vehicleId=vehicle;s.baseId=base;s.world=site.world;
            s.route=site.lane.name;s.home=s.route;s.spec=site.lane;s.line=site.line;
            s.x=x;s.y=y;s.z=z;s.speed=RoleSpeed(site.lane);
            s.trip=trafficroute::StartLap(s.line,site.privateLane,{x,y,z});
            s.trip.leg=trafficroute::Leg::Follow;
            s.trip.lastU=trafficroute::Nearest(s.line,site.privateLane,x,y);
            s.trip.uKnown=true;
            s.factoryPhase=FactoryRoutePhase::Test;
        }
        current->seen=GetTickCount64();
        *claim={&Step,current,"factory bay spot test"};
        return 1;
    }

    // Route owns the reserved public lane and its journey cookie. The Generator owns the
    // destination proof and passes it to Core while this cookie still publishes a hold.
    int StockRoute(generator::StockRouteAction action,int roadSlot,unsigned vehicle,const FvoPoolSpot& at,FvoTripClaim* claim) {
        using Action=generator::StockRouteAction;
        if(claim)*claim={};
        if(!vehicle)return 0;
        std::lock_guard lock(g_journeyMutex);
        const auto it=std::find_if(std::begin(g_slots),std::end(g_slots),[=](const Slot& s){return s.used&&s.vehicleId==vehicle;});
        Slot* current=it==std::end(g_slots)?nullptr:&*it;
        if(action==Action::Cancel) {
            if(!current||!StockRoutePending(*current)||current->stockRoadSlot!=roadSlot)return 0;
            Log("OBS system=traffic.factory_route vehicle=%08X event=cancelled route=%s lane=%d slot=%d",
                vehicle,current->route.c_str(),current->trip.lane,roadSlot);
            *current=Slot{};
            return 1;
        }

        if(roadSlot<0||roadSlot>=int(g_initialSlots.size())||g_initialClockPending)return RefuseTrip(vehicle,"stock_road_slot_unavailable");
        if(!std::isfinite(at.x)||!std::isfinite(at.y)||!std::isfinite(at.z)||!std::isfinite(at.heading))return RefuseTrip(vehicle,"stock_bad_destination");
        const auto& seed=g_initialSlots[std::size_t(roadSlot)];
        const auto samePose=[&](const FvoPoolSpot& other){return at.x==other.x&&at.y==other.y&&at.z==other.z&&at.heading==other.heading;};
        char world[201]{};
        if(!g_worldName||!engineport::FunctionForward(g_worldName,"worldName","routes",vehicle,world,sizeof world))return RefuseTrip(vehicle,"stock_world_unavailable");
        const auto same=current&&StockRoutePending(*current)&&current->stockRoadSlot==roadSlot&&current->stockRouteStamp==g_initialPlanGeneration&&
            current->world==world&&current->route==seed.route&&current->trip.lane==seed.lane&&
            samePose(current->factoryArrival);
        const auto* route=trafficlinks::Find(WorldRoutes(world),seed.route);
        const auto rule=RuleFor(world);
        int on=-1,want=-1;
        const char* reason="stock_route_has_no_room";
        if(!route||route->removed||!seed.eligible||!Allowed(*route,rule)||route->journey.enabled)return RefuseTrip(vehicle,"stock_route_unavailable");
        if(!Room(*route,rule,current,true,&on,&want,&reason))return RefuseTrip(vehicle,reason,on,want);
        if(action==Action::Commit) {
            if(!same||!claim||routecache::Key(*route)!=routecache::Key(current->spec))return RefuseTrip(vehicle,"stock_reservation_changed");
            current->factoryPhase=FactoryRoutePhase::StockCommitted;
            current->seen=GetTickCount64();
            *claim={&Step,current,"factory delivery"};
            return 1;
        }
        if(action!=Action::Reserve||g_rememberedJourneys.contains(vehicle)||(current&&!same))return RefuseTrip(vehicle,"stock_journey_already_exists");
        if(same)return 1;
        if(std::any_of(std::begin(g_slots),std::end(g_slots),[=](const Slot& s){return s.used&&StockRoutePending(s)&&s.stockRoadSlot==roadSlot&&s.world==world;}))
            return RefuseTrip(vehicle,"stock_road_slot_reserved");
        const unsigned base=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",vehicle,vehicle):0;
        if(!base||Excluded(*route,vehicle,seed.lane))return RefuseTrip(vehicle,"stock_model_excluded");
        if(roadSlot>=int(g_initialSiteOutput.size()))return RefuseTrip(vehicle,"stock_destination_not_sited");
        const auto& site=g_initialSiteOutput[std::size_t(roadSlot)];
        if(site.base!=base||!samePose(site.pose))return RefuseTrip(vehicle,"stock_destination_not_sited");
        auto line=trafficroute::Build(*route);
        if(line.error||line.Count()<2||!trafficroute::HasLane(line,route->lanes,seed.lane))return RefuseTrip(vehicle,"stock_lane_unavailable");
        const double station=trafficroute::NearestProgress(line,seed.lane,at.x,at.y,at.z,true,seed.u);
        const auto zone=SpatialRestrictions(*route,at.x,at.y,at.z);
        if(!zone.valid||!zone.admit||zone.closure!=0||!(zone.modes&1))return RefuseTrip(vehicle,"stock_road_destination_restricted");
        int active=0;
        auto* s=FreeSlot(active);
        if(!s)return RefuseTrip(vehicle,"stock_no_free_trip_slot");
        *s=Slot{};
        s->used=true;s->vehicleId=vehicle;s->baseId=base;s->world=world;s->route=route->name;s->home=s->route;
        s->spec=*route;s->line=std::move(line);s->speed=RoleSpeed(*route);s->repeat=g_cycleSupported;
        s->x=at.x;s->y=at.y;s->z=at.z;s->seen=GetTickCount64();s->dice=(std::uint64_t(vehicle)<<20)^s->seen;
        s->trip=trafficroute::StartLap(s->line,seed.lane,{at.x,at.y,at.z});
        if(SectionAt(*s,station).travel==routepolicy::Travel::Hover){*s=Slot{};return RefuseTrip(vehicle,"stock_road_destination_hover_only");}
        s->factoryArrival=at;s->factoryArrivalU=station;
        s->factoryPhase=FactoryRoutePhase::StockReserved;s->stockRoadSlot=roadSlot;s->stockRouteStamp=g_initialPlanGeneration;
        Log("OBS system=traffic.factory_route vehicle=%08X event=reserved route=%s lane=%d slot=%d",vehicle,s->route.c_str(),seed.lane,roadSlot);
        return 1;
    }

	// Auto on routes (2026-10-28, the user: a destination "with a traffic route nearby adds as one of the vehicles and uses it to
	// get to the nearest exit to the destination and then continue from there"). The player's vehicle takes a slot like traffic
	// (the lane slot check keeps the gaps), joins at the route point nearest it and leaves at the exit nearest the marker; core
	// flies on to the marker itself. Used whenever the vehicle can join a route (within that world's traffic `reach`, the same
	// distance traffic joins from) and the route's leaving point is nearer the marker than the joining point (the user: "being in
	// route is very important ... an arb dist is not helpful": no minimum trip length). Of those, the least flying off the route.
	constexpr double kAutoGain = 30.0;  // m the route must bring the vehicle nearer the marker (a leaving point next to the joining point is no trip)
	int PlanRoute(unsigned vehicleId,double x,double y,double z,double tx,double ty,double tz,FvoTripClaim* out) {
        Load();std::lock_guard journeyLock(g_journeyMutex);if(!out)return 0;BuildRoad();
        const trafficdata::Place* pickup=nullptr;trafficdata::Vec destination{tx,ty,tz},roadTarget=destination;int space=-1;
        for(const auto& place:g_data.places)if(trafficdata::SameWorld(g_data,place.world,g_roadWorld.c_str())&&std::hypot(place.at.x-tx,place.at.y-ty,place.at.z-tz)<3&&placeaccess::Valid(place.access)){pickup=&place;const Slot* existing=nullptr;for(const auto& current:g_slots)if(current.used&&current.vehicleId==vehicleId){existing=&current;break;}space=FreeSpace(place,existing);if(space>=0)destination=trafficdata::Spaces(place)[std::size_t(space)];if(!place.access.entrance.empty()){const auto& point=place.access.entrance.front();roadTarget={point.x,point.y,point.z};}break;}
        const auto trip=PlanNetwork({x,y,z},roadTarget,RuleFor(g_roadWorld.c_str()).reach,{},pickup?pickup->access.entryRoute:std::string(),pickup?pickup->access.entryLane:-1);
        if(trip.routes.empty())return 0;
        const auto* r=trafficlinks::Find(WorldRoutes(g_roadWorld.c_str()),trip.routes.front());if(!r)return 0;
        int active=0;Slot* free=VehicleSlot(vehicleId,active);if(!free)return 0;
        Slot& s=*free;s=Slot{};s.used=true;s.world=g_roadWorld;s.vehicleId=vehicleId;s.baseId=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId):0;s.seen=GetTickCount64();s.line=trafficroute::Build(*r);s.spec=*r;s.speed=RoleSpeed(*r);s.route=r->name;
        s.itinerary=trip;CommitNetwork(s,g_road);s.trip=trafficroute::StartTo(s.line,r->lanes,{x,y,z},{tx,ty,tz},trip.exits.front());s.trip.lane=trip.lanes.front();s.dest="your marker";if(pickup){PlacePick pick;pick.place=pickup;pick.space=space;pick.at=destination;TakePlace(s,pick);s.dest="your marker";s.trip.dest=destination;s.pickupWait=space<0;s.holding=s.pickupWait;}
        s.x=x;s.y=y;s.z=z;s.logged=s.trip.leg;FindJunctions(s);
        out->step=&Step;out->user=&s;out->name="connected lane itinerary to marker";
        Log("FVO traffic: vehicle %08X plans %zu connected route(s) to its marker",vehicleId,trip.routes.size());return 1;
    }

    int PlanJourney(const FvoJourneyRequestV1* request,FvoTripClaim* out) {
        if(!request||!out||request->size!=sizeof(*request)||request->version!=1||!request->vehicle||!request->stops||request->stopCount<1||request->stopCount>128||
            !std::isfinite(request->x)||!std::isfinite(request->y)||!std::isfinite(request->z)||!std::isfinite(request->joinReach)||request->joinReach<0||request->joinReach>10000)return 0;
        std::vector<ScriptStop> stops;
        for(int i=0;i<request->stopCount;++i){const auto& q=request->stops[i];
            if(!std::isfinite(q.x)||!std::isfinite(q.y)||!std::isfinite(q.z)||!std::isfinite(q.dwellSeconds)||q.dwellSeconds<0||q.dwellSeconds>86400||q.routeCount<0||q.routeCount>128||(q.routeCount&&!q.routes))return 0;
            ScriptStop stop{{q.x,q.y,q.z},q.dwellSeconds,{}};
            for(int j=0;j<q.routeCount;++j){if(!q.routes[j]||!*q.routes[j]||strlen(q.routes[j])>200)return 0;stop.routes.push_back(trafficdata::Lower(q.routes[j]));}stops.push_back(std::move(stop));
        }
        Load();std::lock_guard journeyLock(g_journeyMutex);BuildRoad();const trafficdata::Vec origin{request->x,request->y,request->z};
        const auto itinerary=PlanNetwork(origin,stops[0].at,request->joinReach,stops[0].routes);if(itinerary.routes.empty())return 0;
        // Validate every later leg before reserving a live trip slot.
        for(std::size_t i=1;i<stops.size();++i)if(PlanNetwork(stops[i-1].at,stops[i].at,request->joinReach,stops[i].routes).routes.empty())return 0;
        const auto* r=trafficlinks::Find(WorldRoutes(g_roadWorld.c_str()),itinerary.routes.front());if(!r)return 0;
        int active=0;Slot* slot=VehicleSlot(request->vehicle,active);if(!slot)return 0;
        *slot=Slot{};slot->used=true;slot->world=g_roadWorld;slot->vehicleId=request->vehicle;slot->baseId=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,request->vehicle):0;slot->seen=GetTickCount64();slot->x=origin.x;slot->y=origin.y;slot->z=origin.z;
        slot->script=std::move(stops);slot->itinerary=itinerary;CommitNetwork(*slot,g_road);slot->spec=*r;slot->route=r->name;slot->line=trafficroute::Build(*r);slot->speed=RoleSpeed(*r);slot->dest="scripted journey";
        slot->trip=trafficroute::StartTo(slot->line,r->lanes,origin,slot->script[0].at,itinerary.exits.front());slot->trip.lane=itinerary.lanes.front();FindJunctions(*slot);
        out->step=&Step;out->user=slot;out->name="scripted road/flight itinerary";return 1;
    }

	// Cruise: the player's vehicle takes a slot like traffic and flies a lap of a route picked at random, turning off at junctions as traffic does. No landing: at the end of the
	// lap core asks for another.
	// One road-tile station: does a vehicle of `shape` fit lane `lane` here? Width between its dividers less half the
	// median on each divider side, height under the roof; open-air stations always fit. Traffic admission and Cruise
	// share it so both apply one rule (2026-11-04: Cruise took Roadrunner's 1 m lanes that admission refused).
	bool RoadStationFits(const trafficroute::Line& l,int i,int lane,const FvoObservedModelV1& shape,double& width,double& height){
		const auto& road=l.road[std::size_t(i)];
		if(lane<0||std::size_t(lane+1)>=road.boundaries.size()){width=height=0;return false;}
		const auto a=road.boundaries[lane],b=road.boundaries[lane+1];
		const int inner=(lane>0)+(std::size_t(lane+2)<road.boundaries.size());  // sides that border a red divider
		width=std::hypot(b.x-a.x,b.y-a.y)-l.median*.5*inner-(lane==0?l.edgeRight:0)-(std::size_t(lane+2)==road.boundaries.size()?l.edgeLeft:0);
		height=road.top.z-road.divider.z;
		if(road.primitive.kind==roadplan::Open)return true;  // an open-air shaft has no lane to fit
		return width>=2*shape.halfWidth+.3&&(height<=0||height>=shape.height+.3);
	}
	bool RoadLaneFits(const trafficroute::Line& l,int lane,const FvoObservedModelV1& shape,int& at,double& width,double& height){
		for(int i=0;i<int(l.road.size());++i)if(!RoadStationFits(l,i,lane,shape,width,height)){at=i;return false;}
		at=-1;return true;
	}
	std::string g_cruiseLast;
	int g_cruiseLastLane=-1;  // re-planning Cruise on the same route keeps its lane (2026-11-04: nearest-lane flips turned the car across the road)
	int PlanCruise(unsigned vehicleId, double x, double y, double z, FvoTripClaim* out)
	{
		Load();
		char world[201]{};
		if (!out || !g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return 0;
		const auto rule = RuleFor(world);
		const auto city = CityNow();
		int minute = 0;
		const bool clock = LocalMinute(minute);
		std::vector<const trafficdata::Route*> routes;
		std::vector<routeshare::CruiseOption> options;
		std::vector<std::vector<int>> fitting;  // per option: road-tile lanes this vehicle fits (empty = not a road route)
		// This vehicle's measured body, as traffic admission reads it; unknown -> road lanes are not filtered (traced).
		const unsigned cruiseBase=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId):0;
		FvoObservedModelV1 shape;
		bool shapeKnown=cruiseBase&&((g_observedModel&&g_observedModel(cruiseBase,&shape)&&modeladmission::Valid(shape))||(g_storedModel&&g_storedModel(cruiseBase,&shape)&&modeladmission::Valid(shape)));
		if(shapeKnown)shape=modeladmission::Rested(shape,fieldgate::kOriginAbove);
		for (const auto& r : g_data.routes) {
			if (!trafficdata::SameWorld(g_data, r.world, world) || r.removed || !CityFlies(r, city) || EmptyHere(vehicleId, r.name)) continue;
			const auto l = trafficroute::Build(r);
			if (l.Count() < 2) continue;
			std::vector<int> fit;
			if(!l.road.empty()){
				for(int lane:trafficroute::Lanes(l,r.lanes)){
					int at=-1;double width=0,height=0;
					if(!shapeKnown||RoadLaneFits(l,lane,shape,at,width,height)){fit.push_back(lane);continue;}
					Log("OBS system=cruise vehicle=%08X event=lane_skipped route=%s lane=%d model=%08X reason=vehicle_does_not_fit station=%d lane_width=%.2f need_width=%.2f lane_height=%.2f need_height=%.2f median=%.2f",
						vehicleId,r.name.c_str(),lane,cruiseBase,at,width,2*shape.halfWidth+.3,height,shape.height+.3,l.median);
				}
				if(!shapeKnown)Log("OBS system=cruise vehicle=%08X event=fit_unknown route=%s model=%08X reason=model_bounds_unavailable; lanes not filtered",vehicleId,r.name.c_str(),cruiseBase);
				if(fit.empty()){Log("OBS system=cruise vehicle=%08X event=route_skipped route=%s reason=no_lane_fits_vehicle",vehicleId,r.name.c_str());continue;}
			}
			const auto in = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			// Reachable only (2026-11-05 09:51: Cruise jumped to "ring", 399 m away and ~200 m up, and the autopilot climbed into the deck
			// above): a route whose nearest point is more than 30 m above or below the car is not offered.
			// In 3D: on a vertical shaft the nearest point seen from above is high up the shaft (19:08: "170 m up" 5 m from the route).
			double near3=1e30;if(l.lane.size()>0)for(const auto& q:l.lane[0])near3=std::min(near3,std::hypot(q.x-x,q.y-y,q.z-z));
			if (near3 > 40.0 && std::fabs(in.z - z) > 30.0) { Log("OBS system=cruise vehicle=%08X event=route_skipped route=%s reason=out_of_reach height_gap=%.0f distance=%.0f",vehicleId,r.name.c_str(),in.z-z,std::hypot(in.x-x,in.y-y)); continue; }
			fitting.push_back(fit);
			const auto d = routeschedule::Evaluate(r.schedule, clock, minute, r.trafficMetres);
			routes.push_back(&r);
			options.push_back({ std::hypot(in.x - x, in.y - y), d.known && d.active, r.name == g_cruiseLast });
		}
		static std::mt19937 random{ std::random_device{}() };
		// Stay on the route you are on (2026-11-05: the user tests Cruise on their own road; the picker preferred any other route).
		int k = -1;
		for (std::size_t i = 0; i < routes.size(); ++i) if (routes[i]->name == g_cruiseLast && options[i].dist <= std::max(rule.reach, 200.0)) k = int(i);
		if (k < 0) k = routeshare::PickCruise(options, rule.reach, unsigned(random()));
		else Log("OBS system=cruise vehicle=%08X event=route route=%s reason=stay_on_current_route distance=%.0f",vehicleId,g_cruiseLast.c_str(),options[std::size_t(k)].dist);
		if (k < 0) return 0;
		int active = 0;
		Slot* free = VehicleSlot(vehicleId,active);
		if (!free) return 0;
		const auto& r = *routes[std::size_t(k)];
		const auto line = trafficroute::Build(r);
		Slot& s = *free;
		s = Slot{};
		s.used = true;s.world=world; s.vehicleId = vehicleId;s.baseId=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId):0; s.seen = GetTickCount64(); s.line = line; s.speed = RoleSpeed(r); s.route = r.name; s.home = r.name;
		s.dice = (std::uint64_t(vehicleId) << 20) ^ s.seen; FindJunctions(s);
		// Road-tile routes: the lane this vehicle was already cruising on (same route), else the nearest lane it fits;
		// other routes keep the old random pick.
		int lane=trafficroute::PickLane(line,r.lanes,random());
		// Lane choice (2026-11-05, the user: "why is it driving in a lane not supposed to"): the nearest lane that runs the way the car
		// faces; a lane running the other way only when none does. Each lane's direction comes from the road data (1 with the
		// markers, -1 against). Was: the nearest lane by distance alone (road routes) or a random lane (other routes).
		double carHeading=0;const bool headingKnown=g_refHeading&&g_refHeading(vehicleId,&carHeading)==1;
		auto laneYaw=[&](int candidate){const double u=trafficroute::Nearest(line,candidate,x,y);const auto a=trafficroute::At(line,candidate,u),b=trafficroute::At(line,candidate,u+trafficroute::Direction(line,candidate)*0.5);return std::atan2(b.x-a.x,b.y-a.y);};
		auto sameWay=[&](int candidate){return !headingKnown||std::cos(laneYaw(candidate)-carHeading)>0;};
		{
			const auto& fit=fitting[std::size_t(k)];
			std::vector<int> candidates=fit.empty()?trafficroute::Lanes(line,r.lanes):fit;
			const bool keep=r.name==g_cruiseLast&&std::find(candidates.begin(),candidates.end(),g_cruiseLastLane)!=candidates.end()&&sameWay(g_cruiseLastLane);
			const char* why="kept_previous_lane";
			if(keep)lane=g_cruiseLastLane;
			else if(!candidates.empty()){double best=1e30;bool bestSame=false;
				for(int candidate:candidates){const auto p=trafficroute::At(line,candidate,trafficroute::Nearest(line,candidate,x,y));const double d=std::hypot(p.x-x,p.y-y);const bool same=sameWay(candidate);
					if((same&&!bestSame)||(same==bestSame&&d<best)){best=d;bestSame=same;lane=candidate;}}
				why=!headingKnown?"nearest_heading_unknown":bestSame?"nearest_same_direction":"no_lane_runs_this_way_nearest";}
			Log("OBS system=cruise vehicle=%08X event=lane route=%s lane=%d reason=%s car_heading=%.0f lane_heading=%.0f",vehicleId,r.name.c_str(),lane,why,
				headingKnown?carHeading*57.29578:-1.0,laneYaw(lane)*57.29578);
		}
		g_cruiseLastLane=lane;
		s.trip = trafficroute::StartLap(line, lane, { x, y, z });
		s.dest = "cruise";
		s.logged = s.trip.leg;
		s.x = x; s.y = y; s.z = z; s.flownStart = true;
		g_cruiseLast = r.name;
		Log("FVO traffic: your vehicle %08X cruises on route %s, lane %d (%.0f m to join; %zu route(s) to choose from)", vehicleId, r.name.c_str(), s.trip.lane, options[std::size_t(k)].dist, options.size());
		out->step = &Step;
		out->user = &s;
		out->name = "cruise";
		return 1;
	}

	int Begin(const trafficdata::Traffic& rule, const char* world, Slot* free, double x, double y, double z, unsigned vehicleId,
		FvoTripClaim* out, bool pool,const std::string& initial,int initialLane)
	{
        std::lock_guard journeyLock(g_journeyMutex);
        if(out&&free&&vehicleId){const auto remembered=g_rememberedJourneys.find(vehicleId);if(remembered!=g_rememberedJourneys.end()&&(remembered->second.world.empty()||remembered->second.world==world)&&(!remembered->second.baseId||!g_baseOf||engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId)==remembered->second.baseId)){
            *free=remembered->second;g_rememberedJourneys.erase(remembered);free->used=true;free->seen=GetTickCount64();free->stepAt=0;free->x=x;free->y=y;free->z=z;FindJunctions(*free);
            out->step=&Step;out->user=free;out->name="restored saved journey";return 1;
        }}

		const ULONGLONG now = GetTickCount64();
		if (!pool && !rule.anyVehicle) {
			const unsigned base = g_baseOf ? engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId) : 0;
			const bool ok = (rule.rev8 && base == 0xFE001033) || std::find(rule.bases.begin(), rule.bases.end(), base) != rule.bases.end();
			if (!ok) return RefuseTrip(vehicleId, "model_not_in_world_rule");
		}
		// A test route asked for in the editor first (once per save, twice the reach), else the route of this world nearest the
		// vehicle, within reach, active now and with room.
		const trafficdata::Route* route = nullptr;
		trafficroute::Line line;
		bool test = false;
        bool useInitial=!initial.empty()&&!EmptyHere(vehicleId,initial);
        if(useInitial){
            bool excluded=false;  // its model is left out of its first route (vehicle rules): it takes any other route instead
            for(const auto& r:g_data.routes)if(r.name==initial&&trafficdata::SameWorld(g_data,r.world,world)){
                if(Excluded(r,vehicleId,initialLane)){return RefuseTrip(vehicleId,"model_excluded_from_route");}
                {int on=-1,want=-1;const char* rw="route_refused";if(Room(r,rule,nullptr,true,&on,&want,&rw)){route=&r;line=trafficroute::Build(r);}else RefuseTrip(vehicleId,rw,on,want);}  // RR-1: the exact reason  // SR-1
                break;}
            if(excluded)useInitial=false;
            else if(!route){if(!g_tripRefusal.count(vehicleId))RefuseTrip(vehicleId,"initial_route_missing");return 0;}
        }
		for (const auto& r : g_data.routes) {
            if(route)break;
			if (!r.test || !trafficdata::SameWorld(g_data, r.world, world) || Excluded(r, vehicleId)||!RevisionReady(r)) continue;
			const std::string key = r.name + "@" + std::to_string(g_stamp);
			if (g_testDone == key) continue;
			const auto l = trafficroute::Build(r);
			const auto p = trafficroute::At(l, 0, trafficroute::Nearest(l, 0, x, y));
			if (std::hypot(p.x - x, p.y - y) > 2 * rule.reach) continue;
			route = &r; line = l; test = true; g_testDone = key;
			break;
		}
		if (!test&&!useInitial) route = FairRoute(world, rule, nullptr, pool, x, y, std::string(), &line, vehicleId);  // fair share (routeshare.h)
		if (!route) return RefuseTrip(vehicleId, "no_route_with_room_in_reach");
		if (test) Log("FVO traffic: test route %s: vehicle %08X flies it once (asked for in the map route editor)", route->name.c_str(), vehicleId);
		const trafficdata::Vec home{ x, y, z };
		// Lap or a destination.
		bool toPlace = rule.trips == trafficdata::Trips::Places || (rule.trips == trafficdata::Trips::Mixed && (g_mixedPlace = !g_mixedPlace));
		const auto pick = toPlace ? PickPlace(world, *route, x, y, nullptr) : PlacePick{};
		const trafficdata::Place* place = pick.place;
		Slot& s = *free;
		s = Slot{};
		s.used = true;s.world=world; s.vehicleId=vehicleId;s.baseId=g_baseOf?engineport::FunctionForward(g_baseOf,"baseOf","routes",0u,vehicleId):0; s.seen = now; s.line = line; s.speed = RoleSpeed(*route); s.route = route->name;
		s.home = route->name; s.dice = (std::uint64_t(vehicleId) << 20) ^ now; FindJunctions(s);
		s.repeat = g_cycleSupported;
		if (place) {
			s.trip = trafficroute::StartTo(line, route->lanes, home, pick.at, ExitToward(*route, line, *place));
			TakePlace(s, pick);if(s.accessPlan.entryLane>=0){if(!trafficroute::HasLane(s.line,route->lanes,s.accessPlan.entryLane)){s.used=false;return RefuseTrip(vehicleId,"place_entry_lane_missing");}s.trip.lane=s.accessPlan.entryLane;}
		}
		else {
			s.trip = trafficroute::StartLap(line, trafficroute::PickLane(line,route->lanes,unsigned(g_nextLane++)), home);
		}
        if(useInitial){
            s.trip=trafficroute::StartLap(line,initialLane,home);s.trip.leg=trafficroute::Leg::Follow;
            s.trip.lastU=trafficroute::Nearest(line,initialLane,x,y);s.trip.uKnown=true;
            s.dest.clear();s.place.clear();s.space=-1;s.queue=false;s.stay=0;
        }
        if(s.baseId&&trafficdata::LaneLeftOut(g_data.groups,*route,s.trip.lane,s.baseId)){
            const auto lanes=trafficroute::Lanes(line,route->lanes);auto allowed=std::find_if(lanes.begin(),lanes.end(),[&](int lane){return !trafficdata::LaneLeftOut(g_data.groups,*route,lane,s.baseId);});
            if(useInitial||allowed==lanes.end()||s.accessPlan.entryLane>=0){s.used=false;return RefuseTrip(vehicleId,"lane_left_out_for_model");}
            s.trip.lane=*allowed;
        }
        s.spec=*route;if(!AuthoredJourney(s,*route,world,home,rule.reach)){s.used=false;return RefuseTrip(vehicleId,"journey_not_authored");}
		s.logged = s.trip.leg;
		s.x = x; s.y = y; s.z = z; s.flown = 0; s.flownStart = true;
		if (!pool) g_lastStart = now;
		Log("FVO traffic: vehicle %08X goes onto route %s, lane %d, %s%s", vehicleId, route->name.c_str(), s.trip.lane, place ? "to " : "a lap and home",
			place ? place->name.c_str() : "");
		out->step = &Step;
		out->user = &s;
		out->name = place ? "route trip to a place" : "route lap";
		g_tripRefusal.erase(vehicleId);  // SR-1
		return 1;
	}

    // Capacity is stable across clock/schedule changes. Birth locations are
    // authored ground origins, never arbitrary projections of airborne lanes.
    struct AdmissionLane {trafficroute::Line line;int lane=0;bool explicitGeometry=false,rulesAvailable=true;std::map<unsigned,bool> verdicts;std::shared_ptr<const trafficdata::Route> route;std::vector<double> stations;};  // route: the Field Provider's tile boxes (spawn Siting)
    std::vector<std::shared_ptr<AdmissionLane>> g_admissionLanes;
    // Existing hull query encloses the model around its origin; no new engine reads.
    void HullShape(double radius,double halfHeight,FvoObservedModelV1& shape){
        const double horizontal=std::max(.6,radius-1),vertical=std::max(.5,halfHeight-1);
        shape.halfWidth=shape.halfLength=horizontal;shape.height=2*vertical;shape.originAbove=vertical;
    }
    // Spawn flow step 5, Siting (spawnflow.h; 2026-11-07). The Field Provider's box for the slot's tile (tilebox::TileBox: authored road_boxes,
    // or the default box for a route without them; none for D11 shapes or unreadable boxes), the lane point at the slot's station, and the
    // model's bounds give one pose (floor + rest height + drop margin, so the car drops onto the floor) or one reason. Trusts the field as
    // built: no navmesh, no World levels, no site rays (the user, 2026-11-06). Replaces InitialField (roadfield) and InitialDeck (navmesh).
    int InitialSite(int slot,int attempt,unsigned base,double radius,double halfHeight,FvoPoolSpot& at,bool occupied,bool spotRejected,generator::RoadSite& road,const char** why){
        *why="no_admission_lane";
        if(slot<0||slot>=int(g_admissionLanes.size())||!g_admissionLanes[slot]||!g_admissionLanes[slot]->route)return 0;
        const auto& plan=*g_admissionLanes[slot];const auto& route=*plan.route;
        if(plan.line.error){*why="route_line_error";return 0;}
        const auto& seed=g_initialSlots[slot];
        double u=0;
        if(seed.interval>0){
            if(!seed.eligible||attempt<0||std::size_t(attempt)>seed.backups.size()){*why="kilometre_slot_no_field_room";return 0;}
            u=attempt?seed.backups[std::size_t(attempt-1)].u:seed.u;
            road={trafficinitial::StationAt(plan.stations,u),seed.interval};
        }else u=trafficroute::Nearest(plan.line,plan.lane,at.x,at.y);
        const auto a=trafficroute::At(plan.line,plan.lane,u),b=trafficroute::At(plan.line,plan.lane,u+trafficroute::Direction(plan.line,plan.lane)*.001);
        const int tiles=tilebox::TileCount(route);
        const int tile=tiles>0?std::clamp(int(plan.line.ToSource(u)),0,tiles-1):-1;
        FvoFieldBoxV1 box;const bool haveBox=tile>=0&&tilebox::TileBox(route,plan.line,tile,box);
        FvoObservedModelV1 model;
        if(!(g_observedModel&&g_observedModel(base,&model)&&modeladmission::Valid(model))&&
           !(g_storedModel&&g_storedModel(base,&model)&&modeladmission::Valid(model)))HullShape(radius,halfHeight,model);
        model=modeladmission::Rested(model,fieldgate::kOriginAbove);
        spawnflow::Shape shape;shape.halfWidth=model.halfWidth;shape.halfLength=model.halfLength;shape.height=model.height;shape.originAbove=model.originAbove;
        const spawnflow::Pose lanePoint{a.x,a.y,a.z,std::atan2(b.x-a.x,b.y-a.y)};
        auto site=spawnflow::Place(haveBox,box,lanePoint,shape,occupied,spotRejected);
        // LV-1: the tile is picked by line index, which a rounded corner stretches (the index is not metres). A lane point the picked tile's box does not
        // hold may lie in the box of the tile before or after it: that tile's field is the one under the car.
        if(!site.ok&&!std::strcmp(site.why,"lane_point_outside_its_tile")&&tile>=0)
            for(const int other:{tile-1,tile+1}){
                if(other<0||other>=tiles)continue;
                FvoFieldBoxV1 next;if(!tilebox::TileBox(route,plan.line,other,next))continue;
                const auto retry=spawnflow::Place(true,next,lanePoint,shape,occupied,spotRejected);
                if(retry.ok){site=retry;Log("OBS system=traffic.spawn event=site_tile_corrected slot=%d tile=%d used_tile=%d reason=lane_point_in_neighbour_tile",slot,tile,other);break;}
            }
        *why=site.why;
        if(!site.ok)return 0;
        at={site.pose.x,site.pose.y,site.pose.z,site.pose.heading};
        if(slot<int(g_initialSiteOutput.size()))g_initialSiteOutput[std::size_t(slot)]={at,base};
        return 1;
    }
    bool InitialRoadGap(int slot,const generator::RoadSite& road,const FvoPoolSpot& other,double& gap){
        const auto& plan=*g_admissionLanes[slot];
        double station=0;
        if(!trafficinitial::ProjectRoadStation(*plan.route,plan.line,plan.stations,{other.x,other.y,other.z},station))return false;
        const double length=plan.stations.back();
        if(trafficinitial::GapClear(road.station,station,length,plan.line.closed,road.interval))return false;
        gap=trafficinitial::StationGap(road.station,station,length,plan.line.closed);
        return true;
    }
    bool InitialModelFits(int slot,unsigned base,double radius,double halfHeight,bool hullKnown=true){
        if(slot<0||slot>=int(g_admissionLanes.size())||!g_admissionLanes[slot])return true;
        auto& plan=*g_admissionLanes[slot];if(!plan.rulesAvailable)return false;if(!plan.explicitGeometry)return true;
        if(!trafficroute::Spatial(plan.line)&&plan.line.road.empty()){
            bool needsBounds=false;
            for(int i=0;i<plan.line.Count();++i){
                const auto movement=trafficroute::SettingsAt(plan.line,double(i),plan.lane).movement;
                if(movement!=laneplan::Movement::Legacy&&movement!=laneplan::Movement::Free){needsBounds=true;break;}
            }
            if(!needsBounds)return true;
        }
        if(const auto at=plan.verdicts.find(base);at!=plan.verdicts.end())return at->second;
        FvoObservedModelV1 shape;
        const bool observed=g_observedModel&&g_observedModel(base,&shape)&&modeladmission::Valid(shape);
        if(!observed&&!hullKnown){static std::set<unsigned> told;if(told.insert(base).second)Log("OBS system=traffic.admission vehicle=00000000 event=model_deferred route=%s lane=%d model=%08X reason=model_bounds_unavailable note=logged_once_per_model; the spot picks another model",plan.line.routeName.c_str(),plan.lane,base);return false;}
        const bool stored=!observed&&g_storedModel&&g_storedModel(base,&shape)&&modeladmission::Valid(shape);
        if(!observed&&!stored)HullShape(radius,halfHeight,shape);
        shape=modeladmission::Rested(shape,fieldgate::kOriginAbove);
        bool fits=plan.line.Count()>=2;
        int failAt=-1;double failWidth=0,failHeight=0;  // where and why a road-tile lane refused the model (traced below)
        for(int i=0;fits&&i<plan.line.Count();++i){
            if(!plan.line.road.empty()){
                double width=0,height=0;
                fits=RoadStationFits(plan.line,i,plan.lane,shape,width,height);
                if(!fits){failAt=i;failWidth=width;failHeight=height;break;}
                continue;  // road tiles: the dividers are the lanes; an old lane plan kept on the route does not gate models
            }
            const auto settings=trafficroute::SettingsAt(plan.line,double(i),plan.lane);
            if(!trafficroute::Spatial(plan.line)&&(settings.movement==laneplan::Movement::Legacy||settings.movement==laneplan::Movement::Free))continue;
            fits=modeladmission::Fits(settings.profile,shape);
            if(trafficroute::Spatial(plan.line)){
                const auto& basis=plan.line.basis[plan.lane];if(std::size_t(i)>=basis.right.size()||std::size_t(i)>=basis.up.size()){fits=false;break;}
                FvoCorridorProfileV1 corridor;const auto& profile=settings.profile;const auto outline=splinekit::Resample(profile,plan.line.space.profileSamples);
                corridor.active=1;corridor.width=profile.width;corridor.height=profile.height;corridor.count=int(outline.size());if(corridor.count>64){fits=false;break;}
                const auto r=basis.right[i],v=basis.up[i];corridor.right=lanevolume::Add(lanevolume::Mul(r,std::cos(profile.bank)),lanevolume::Mul(v,std::sin(profile.bank)));corridor.up=lanevolume::Sub(lanevolume::Mul(v,std::cos(profile.bank)),lanevolume::Mul(r,std::sin(profile.bank)));
                for(int n=0;n<corridor.count;++n)corridor.outline[n]=outline[n];
                const auto a=trafficroute::At(plan.line,plan.lane,double(i)),b=trafficroute::At(plan.line,plan.lane,double(i)+trafficroute::Direction(plan.line,plan.lane)*.001);
                struct Input {FvoObservedModelV1 shape;double heading;};fits=corridorprofile::Fits(corridor,Input{shape,std::atan2(b.x-a.x,b.y-a.y)});
            }
        }
        if(!fits)Log("OBS system=traffic.admission vehicle=00000000 event=model_rejected route=%s lane=%d model=%08X reason=lane_envelope bounds=%s station=%d lane_width=%.2f need_width=%.2f lane_height=%.2f need_height=%.2f median=%.2f",plan.line.routeName.c_str(),plan.lane,base,observed?"published_model":stored?"stored_bounds":"conservative_hull",
            failAt,failWidth,2*shape.halfWidth+.3,failHeight,shape.height+.3,plan.line.median);
        if(observed)plan.verdicts[base]=fits;return fits;
    }
    void ReadRemovalRule(){
        // Card CP-1: the removal rule switch (a gameplay rule, so read in release builds too; missing key = 0 = far and out of sight only).
        static fvopaths::Watch removalWatch;
        if(removalWatch.Changed(fvopaths::Settings()))
            generator::SetRemovalPrediction(GetPrivateProfileIntW(L"traffic",L"removeByCameraPrediction",0,fvopaths::Settings().c_str())!=0);
    }
    void ConfigureFactoryRoute(const char* world) {
        std::lock_guard lock(g_journeyMutex);
        g_factoryRoute.reset();
        const auto publish=[&] {
            generator::SetFactoryBay(g_factoryRoute?&g_factoryRoute->bay:nullptr,&FactoryTrip,g_factoryProof,g_factoryLease);
        };
        const auto reject=[&](const char* reason) {
            Log("OBS system=traffic.factory_site event=inactive bay=%s reason=%s",g_factoryConfig.key.c_str(),reason);
            publish();
        };
        if(!g_factoryConfig.vehicle||!trafficdata::SameWorld(g_data,g_factoryConfig.world,world)) { publish(); return; }
        if(!g_factoryProof||!g_factoryLease) { reject("factory_core_exports_unavailable"); return; }
        if(!g_factoryGeometry.problems.empty()||g_factoryGeometry.routes.size()!=1||g_factoryGeometry.links.size()!=1) {
            reject("expected_one_private_route_and_exit");
            return;
        }
        auto validation=g_data;
        validation.routes.push_back(g_factoryGeometry.routes.front());
        validation.links.push_back(g_factoryGeometry.links.front());
        if(const auto* why=factorysite::Validate(g_factoryConfig,validation,world)) { reject(why); return; }
        const auto* publicRoad=trafficlinks::Find(WorldRoutes(world),g_factoryConfig.publicRoute);
        const auto& privateRoad=g_factoryGeometry.routes.front();
        if(!publicRoad||!Allowed(*publicRoad,RuleFor(world))||
            (privateRoad.road.enabled&&!g_fieldSupported)) { reject("route_policy_unavailable"); return; }
        const auto links=trafficlinks::Of(g_factoryGeometry.links,{&privateRoad,publicRoad},privateRoad.name);
        if(links.size()!=1) { reject("factory_exit_unavailable"); return; }
        FactoryRoute route;
        route.lane=privateRoad;
        route.line=trafficroute::Build(privateRoad);
        route.exit=links.front();
        route.world=world;
        route.privateLane=g_factoryConfig.privateLane;
        route.bay.key=g_factoryConfig.key;
        route.bay.vehicle=g_factoryConfig.vehicle;
        route.bay.at=g_factoryConfig.pose;
        const auto publicLine=trafficroute::Build(*publicRoad);
        const auto turn=trafficlinks::TurnPath(route.line,route.privateLane,route.exit.u,
            publicLine,route.exit.toLane,route.exit.otherU,route.exit.radius);
        const auto point=turn.empty()?trafficroute::At(publicLine,route.exit.toLane,route.exit.otherU):turn.back();
        route.arrivalU=trafficroute::NearestProgress(publicLine,route.exit.toLane,point.x,point.y,point.z,true,route.exit.otherU);
        const auto before=trafficroute::At(publicLine,route.exit.toLane,trafficroute::LeadU(publicLine,route.exit.toLane,route.arrivalU,1.0,true));
        const auto ahead=trafficroute::At(publicLine,route.exit.toLane,trafficroute::LeadU(publicLine,route.exit.toLane,route.arrivalU,1.0));
        route.bay.arrival={point.x,point.y,point.z,std::atan2(ahead.x-before.x,ahead.y-before.y)};
        g_factoryRoute=std::move(route);
        Log("OBS system=traffic.factory_site vehicle=%08X event=ready bay=%s private=%s public=%s exit=%s",
            g_factoryRoute->bay.vehicle,g_factoryRoute->bay.key.c_str(),privateRoad.name.c_str(),publicRoad->name.c_str(),g_factoryConfig.link.c_str());
        publish();
    }
    void SendPool(const char* world){
        if(!g_trafficApi)return;
        ConfigureFactoryRoute(world);
        const auto rule=RuleFor(world);
        const auto city=trafficdata::CityOf(g_data,world);
        std::vector<FvoPoolSpot> origins;std::vector<std::string> originNames;
        for(const auto& p:g_data.places)if(trafficdata::SameWorld(g_data,p.world,world)){
            origins.push_back({p.at.x,p.at.y,p.at.z,0});originNames.push_back("place."+p.name);}
        int target=0;
        for(const auto& r:g_data.routes)if(trafficdata::SameWorld(g_data,r.world,world)&&Allowed(r,rule)&&CityFlies(r,city)){
            const int peak=routeschedule::PeakVehicles(r.schedule,r.trafficMetres);
            target=std::min(fvolimits::kVehicles,target+std::max(0,trafficdata::Scaled(peak,city)));
        }
        target=std::clamp(std::max(target,city.on?rule.startOnRoute:0),0,std::min(fvolimits::kVehicles,city.limit));
        // An origin may dispatch several vehicles over time. They are not born
        // together there: the generator reserves the footprint until it clears.
        std::vector<FvoPoolSpot> spots;std::vector<std::string> names;std::vector<int> reuse;
        std::vector<std::vector<trafficinitial::Slot>> perRoute;int minute=0;const bool clock=LocalMinute(minute);g_initialClockPending=false;
        for(const auto& r:g_data.routes)if(trafficdata::SameWorld(g_data,r.world,world)&&Allowed(r,rule)&&CityFlies(r,city)){
            if(!clock&&!r.schedule.allDay)g_initialClockPending=true;
            const auto demand=routeschedule::Evaluate(r.schedule,clock,minute,r.trafficMetres);
            if(routeschedule::HasLevel(r.schedule)){
                const auto band=r.schedule.trafficBands[r.schedule.trafficLevel];
                Log("OBS system=traffic.demand event=prepared route=%s level=%s metres=%.0f min_per_km=%d max_per_km=%d known=%d target=%d scaled=%d",
                    r.name.c_str(),routeschedule::LevelName(r.schedule.trafficLevel),r.trafficMetres,band.low,band.high,
                    int(demand.known),demand.vehicles,trafficdata::Scaled(demand.vehicles,city));
            }
            // Cars on drawn lanes only for now: hover-only routes get no positions (the user, 2026-11-06).
            if(demand.known&&demand.active&&(r.road.enabled||r.travel==routepolicy::Travel::Drive)){
                std::vector<std::pair<double,double>> junctions;
                for(const auto& link:g_data.links){if(link.from==r.name)junctions.push_back({link.at,link.radius});if(link.to==r.name)junctions.push_back({link.toEnd?double(r.points.size()-1):link.toAt,link.radius});}
                // Crossings constrain placement even when they do not authorize a route connection.
                if(r.road.enabled||r.travel==routepolicy::Travel::Drive)for(const auto& other:g_data.routes)if(other.name!=r.name&&!other.ring&&trafficdata::SameWorld(g_data,other.world,world))
                    for(std::size_t i=0;i+1<r.points.size();++i)for(std::size_t j=0;j+1<other.points.size();++j){
                        const auto a=r.points[i],b=r.points[i+1],c=other.points[j],d=other.points[j+1];
                        const double x=b.x-a.x,y=b.y-a.y,vx=d.x-c.x,vy=d.y-c.y,den=x*vy-y*vx;
                        if(std::fabs(den)<1e-9)continue;const double t=((c.x-a.x)*vy-(c.y-a.y)*vx)/den,q=((c.x-a.x)*y-(c.y-a.y)*x)/den;
                        if(t>=0&&t<=1&&q>=0&&q<=1&&std::fabs(a.z+(b.z-a.z)*t-c.z-(d.z-c.z)*q)<3)junctions.push_back({double(i)+t,std::max(r.corridor,other.corridor)*2});
                    }
                perRoute.push_back(trafficinitial::RoadByKilometre(r,trafficdata::Scaled(demand.vehicles,city),junctions));
                const auto& plan=perRoute.back();
                if(!plan.empty()&&plan.front().interval>0)
                    Log("OBS system=traffic.spawn event=road_distribution route=%s target=%zu length_m=%.1f interval_m=%.1f minimum_gap_m=%.1f blocked_slots=%zu",
                        r.name.c_str(),plan.size(),plan.front().interval*plan.size(),plan.front().interval,trafficinitial::MinimumGap(plan.front().interval),
                        std::size_t(std::count_if(plan.begin(),plan.end(),[](const auto& s){return !s.eligible;})));
            }
        }
        g_initialSlots=trafficinitial::Interleave(perRoute,city.limit);
        ++g_initialPlanGeneration;
        if(int(g_initialSlots.size())>city.limit)g_initialSlots.resize(std::size_t(std::max(0,city.limit)));
        g_initialSiteOutput.assign(g_initialSlots.size(),{});
        target=std::max(target,int(g_initialSlots.size()));
        for(int i=0;i<target;++i){
            spots.push_back(origins.empty()?FvoPoolSpot{}:origins[std::size_t(i)%origins.size()]);
            names.push_back("fleet."+std::to_string(i));reuse.push_back(origins.empty()?-1:0);}
        // Existing parked vehicles can take real drivers and leave normally.
        for(const auto& p:g_data.places)if(p.parked&&trafficdata::SameWorld(g_data,p.world,world)&&spots.size()<std::size_t(fvolimits::kVehicles)){
            spots.push_back({p.at.x,p.at.y,p.at.z,0});names.push_back("place."+p.name);reuse.push_back(1);}
        std::vector<const char*> ptrs;for(const auto& n:names)ptrs.push_back(n.c_str());
        generator::SetSpots(world,spots.data(),int(spots.size()),reuse.data(),ptrs.data(),&PoolTrip);
        generator::SetTripReaders(&TripRefusalOf,&InitialRouteRoom);  // SR-1 / SR-3: facts the generator reads
        {std::vector<const char*> landingNames;for(const auto& n:originNames)landingNames.push_back(n.c_str());generator::SetLandingSpots(origins.data(),landingNames.data(),int(origins.size()));}
        // Vehicle rules reach the models dealt out: a position on a route holds what that route allows; any other leaves from the ground and may take any route of
        // this world, so only a model every one of them leaves out is barred. (2026-10-31: the deal ignored them, and a left-out model on a lane got no trip and stayed there.)
        {
            std::vector<unsigned> barred;bool first=true;
            for(const auto& r:g_data.routes)if(trafficdata::SameWorld(g_data,r.world,world)&&Allowed(r,rule)&&CityFlies(r,city)){
                if(first){barred=trafficdata::AllLeftOut(g_data.groups,r);first=false;continue;}
                barred.erase(std::remove_if(barred.begin(),barred.end(),[&](unsigned id){return !trafficdata::LeftOut(g_data.groups,r,id);}),barred.end());
            }
            std::vector<std::vector<unsigned>> skip(spots.size(),barred);
            int ruled=0;
            for(std::size_t i=0;i<g_initialSlots.size()&&i<skip.size();++i)for(const auto& r:g_data.routes)if(r.name==g_initialSlots[i].route){skip[i]=trafficdata::LaneLeftOut(g_data.groups,r,g_initialSlots[i].lane);if(!skip[i].empty())++ruled;break;}
            generator::SetModelRules(skip);
            if(ruled||!barred.empty())Log("FVO traffic: %s: vehicle rules: %d lane position(s) limited to their route/lane models; %zu model(s) left out of every route",world,ruled,barred.size());
        }
        std::vector<FvoPoolSpot> initial;for(const auto& s:g_initialSlots)initial.push_back({s.at.x,s.at.y,s.at.z,s.heading});
        generator::SetInitialScene(initial.data(),int(initial.size()));
        // Route geometry also seeds the stock search when its schedule requests no public cars.
        std::vector<FvoPoolSpot> factoryAnchors;
        for(const auto& r:g_data.routes){
            if(!trafficdata::SameWorld(g_data,r.world,world)||!Allowed(r,rule)||!CityFlies(r,city))continue;
            for(std::size_t j=1;j<r.points.size();++j){
                const auto& a=r.points[j-1];const auto& b=r.points[j];
                const double dx=b.x-a.x,dy=b.y-a.y,dz=b.z-a.z;
                const double length=std::hypot(dx,dy);
                if(!std::isfinite(length)||!std::isfinite(dz)||length<.1)continue;
                const int segments=int(std::clamp(std::ceil(length/40),1.0,128.0));
                const double heading=std::atan2(dx,dy);
                for(int k=0;k<segments;++k){
                    const double t=(k+.5)/segments;
                    factoryAnchors.push_back({a.x+dx*t,a.y+dy*t,a.z+dz*t,heading});
                }
            }
        }
        generator::SetFactoryAnchors(std::move(factoryAnchors));
        {std::vector<std::string> keys;for(const auto& q:g_initialSlots)keys.push_back(spawnhealth::SpotKey(q.route,q.lane,q.at.x,q.at.y));generator::SetInitialSpotKeys(keys);}  // card HR1
        std::vector<bool> roads;for(const auto& slot:g_initialSlots){bool road=false;for(const auto& route:g_data.routes)if(route.name==slot.route){road=route.road.enabled||routepolicy::Resolve(route.travel,route.sections,std::size_t(slot.u)).travel==routepolicy::Travel::Drive;break;}roads.push_back(road);}generator::SetInitialRoads(roads);
        // A schedule awaiting the game clock does not stop the arbiter from
        // using already-known routes. PreparePool rebuilds the roster once it resolves.
        generator::WaitForInitialClock(g_initialClockPending&&initial.empty());
        std::map<std::string,trafficroute::Line> alternativeLines;
        g_admissionLanes.assign(g_initialSlots.size(),{});std::map<std::pair<std::string,int>,std::shared_ptr<AdmissionLane>> admission;
        for(int i=0;i<int(g_initialSlots.size());++i){const auto& slot=g_initialSlots[i];
            for(const auto& route:g_data.routes)if(route.name==slot.route){auto compiled=alternativeLines.find(route.name);if(compiled==alternativeLines.end())compiled=alternativeLines.emplace(route.name,trafficroute::Build(route)).first;const auto& line=compiled->second;
                const auto key=std::make_pair(route.name,slot.lane);auto& gate=admission[key];if(!gate){gate=std::make_shared<AdmissionLane>();gate->line=line;gate->route=std::make_shared<const trafficdata::Route>(route);gate->lane=slot.lane;gate->rulesAvailable=!trafficdata::LaneLeftOut(g_data.groups,route,slot.lane,0);gate->explicitGeometry=route.road.enabled||route.lanePlan.enabled||route.spacePlan.enabled;if(slot.interval>0)gate->stations=trafficinitial::RoadStations(line);}g_admissionLanes[i]=gate;
                std::vector<FvoPoolSpot> alternatives;
                const auto movement=trafficroute::SettingsAt(line,slot.u,slot.lane).movement;
                const bool exact=slot.interval>0||!slot.backups.empty()||route.road.enabled||route.travel==routepolicy::Travel::Drive||route.spacePlan.enabled||(route.lanePlan.enabled&&movement!=laneplan::Movement::Legacy&&movement!=laneplan::Movement::Free);
                for(const auto& backup:slot.backups)alternatives.push_back({backup.at.x,backup.at.y,backup.at.z,backup.heading});
                for(int j=1;!exact&&j<=16;++j){const double u=trafficroute::LeadU(line,slot.lane,slot.u,6.0*((j+1)/2),j%2==0);
                    const auto a=trafficroute::At(line,slot.lane,u),b=trafficroute::At(line,slot.lane,u+trafficroute::Direction(line,slot.lane)*0.001);
                    alternatives.push_back({a.x,a.y,a.z,std::atan2(b.x-a.x,b.y-a.y)});}
                generator::SetInitialAlternatives(i,alternatives.data(),int(alternatives.size()));break;
            }
        }
        generator::SetModelGate(&InitialModelFits);
        generator::SetSiteGate(&InitialSite,&InitialRoadGap);
        Log("Traffic arrival: %zu active route positions prepared; ground origins are only for later departures",initial.size());
        Log("FVO traffic: %s: stable fleet capacity %d, %zu authored ground origins, %zu total registry slots; additions require validated concealed departure",world,target,origins.size(),spots.size());
    }
    std::string g_poolWorld;unsigned long long g_poolStamp=~0ull;
    std::atomic<bool> g_poolResend{false};
    // X reads the current routes and schedule, then fills only the gaps under the map panel.
    bool SyncProfile();
    void StartSwap(const FvoRushPlace* places,int count){if(!SyncProfile())return;RequestReload();g_poolResend=true;generator::Rush(places,count);}
    void PreparePool(){
        Load();char world[201]{};if(!g_worldName||!engineport::FunctionForward(g_worldName,"worldName","routes",0u,world,sizeof world))return;
        int minute=0;
        ReadRemovalRule();
        if(g_poolResend.exchange(false)||g_poolWorld!=world||g_poolStamp!=g_stamp||(g_initialClockPending&&LocalMinute(minute))){FindAnchors();SendPool(world);g_poolWorld=world;g_poolStamp=g_stamp;}
    }
    void ResetAll();

    // The committed journey already names the vehicle's route and lane. Build a rolling
    // chassis path from its current position so reloading never starts it at the old slot.
    bool FleetPath(unsigned vehicle,const FvoTrafficRef& pose,std::vector<FvoFleetRailPoint>& points,double& speed){
        std::lock_guard lock(g_journeyMutex);
        const auto it=std::find_if(std::begin(g_slots),std::end(g_slots),[vehicle](const Slot& slot){
            return slot.used&&slot.vehicleId==vehicle&&slot.factoryPhase==FactoryRoutePhase::None;
        });
        if(it==std::end(g_slots)||!std::isfinite(pose.x)||!std::isfinite(pose.y)||!std::isfinite(pose.z))return false;
        const auto& slot=*it;
        const auto& line=slot.line;
        const int lane=slot.trip.lane;
        if(line.error||line.Count()<2||lane<0||std::size_t(lane)>=line.lane.size()||line.lane[lane].size()<2||
           slot.trip.leg==trafficroute::Leg::Done)return false;
        constexpr int maxPoints=64;
        constexpr double sampledM=4.0;
        constexpr double maxJoinGapM=12.0;
        points.push_back({pose.x,pose.y,pose.z});
        speed=(slot.guidance.speed>0&&std::isfinite(slot.guidance.speed))?slot.guidance.speed:slot.speed;
        if(!std::isfinite(speed))return false;
        speed=std::clamp(speed,0.0,150.0);
        if(slot.guidance.held||slot.queue||slot.scriptHolding)speed=0;

        auto append=[&](double x,double y,double z){
            if(points.size()>=maxPoints||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return;
            const auto& back=points.back();
            if(std::hypot(x-back.x,y-back.y,z-back.z)>=0.05)points.push_back({x,y,z});
        };
        if(slot.trip.leg!=trafficroute::Leg::Follow||!slot.turn.empty()||slot.placePath){
            // A connector or place manoeuvre is already an authored part of the trip.
            // Use its explicit guidance, never project across to a different lane or deck.
            const auto& guidance=slot.guidance;
            if(guidance.count<2||guidance.count>maxPoints)return false;
            int nearest=0;
            double distance=maxJoinGapM;
            for(int i=0;i<guidance.count;++i){
                const auto& q=guidance.points[i];
                const double gap=std::hypot(q.x-pose.x,q.y-pose.y,q.z-pose.z);
                if(gap<distance){distance=gap;nearest=i;}
            }
            if(distance>=maxJoinGapM)return false;
            for(int i=nearest;i<guidance.count;++i){
                const auto& q=guidance.points[i];
                append(q.x,q.y,q.z);
            }
            return points.size()>=2;
        }

        double u=trafficroute::NearestProgress(line,lane,pose.x,pose.y,pose.z,slot.trip.uKnown,slot.trip.lastU);
        const auto from=trafficroute::At(line,lane,u);
        if(std::hypot(from.x-pose.x,from.y-pose.y,from.z-pose.z)>maxJoinGapM)return false;
        // Preserve the vehicle's actual rest offset above the lane data. This is the
        // chassis path consumed by the rail, not a terrain-height query.
        // Maintain the same lane order on the rail when native traffic physics
        // cannot see a leader, using the trip's existing position samples.
        constexpr double minimumGapM=8.0;
        constexpr double targetHeadwayS=1.5;
        const auto now=GetTickCount64();
        for(const auto& other:g_slots){
            if(&other==&slot||!other.used||other.route!=slot.route||other.trip.lane!=lane||
               other.trip.leg!=trafficroute::Leg::Follow||!other.trip.uKnown||now-other.seen>2000||
               std::hypot(other.x-pose.x,other.y-pose.y)>100.0)continue;
            const double gap=trafficroute::DistanceAlong(line,lane,u,other.trip.lastU);
            if(std::isfinite(gap))speed=std::min(speed,std::max(0.0,(gap-minimumGapM)/targetHeadwayS));
        }
        const double offset=pose.z-from.z;
        for(int i=1;i<maxPoints;++i){
            const double next=trafficroute::LeadU(line,lane,u,sampledM,false,false);
            if(std::fabs(next-u)<1e-6||!trafficroute::Available(line,lane,next))break;
            const auto q=trafficroute::At(line,lane,next);
            append(q.x,q.y,q.z+offset);
            u=next;
        }
        return points.size()>=2;
    }

    std::wstring g_runningProfile;
    bool SyncProfile() {
        const auto profile = TrafficDir();
        if (profile.empty()) return false;
        if (profile != g_runningProfile) {
            ResetAll(); g_runningProfile = profile;LoadJourneys(profile);
            const auto registry = profile.substr(0, profile.find_last_of(L"\\/")) + L"\\trafficpool.ini";
            generator::SetStorage(registry.c_str());
        }
        return true;
    }
    // 2026-11-05 strip: no population. The tick keeps the route files current for Cruise; the spawn pool is no longer built.
    // 2026-11-06: population back on the approved rules (notes/SPAWN_DESIGN_20261106.md): created only behind the loading screen or the X panel.
    // Ask the core to save the navmesh of every cell a spot stands in; it bakes them as the player comes near (a spot with no tile is skipped).
    void WantSpotCells(){
        static ULONGLONG at=0;const ULONGLONG now=GetTickCount64();if(!g_navWant||now-at<5000)return;at=now;
        for(const auto& slot:g_initialSlots)g_navWant(slot.at.x,slot.at.y);
    }
    // C4 (coordinator -> Vehicle List): once a second, the pairs Health called broken are out of traffic for World (D1 steer round).
    void CoordinateOutOfTraffic(){const double now=GetTickCount64()*0.001;if(!g_outOfTraffic||(g_outOfTrafficAt>=0&&now-g_outOfTrafficAt<1.0))return;g_outOfTrafficAt=now;
        static unsigned ids[256];const int n=generator::OutOfTraffic(ids,256);g_outOfTraffic(ids,n);}
    void TrafficTick(){if(SyncProfile()){PreparePool();generator::Tick();CoordinateOutOfTraffic();WantSpotCells();}}
    void TrafficHousekeeping(){if(SyncProfile()){PreparePool();generator::Housekeeping();}}

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
		if (!g_worldName || !engineport::FunctionForward(g_worldName,"worldName","routes",0u,world, sizeof world)) return;
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
		engineport::FunctionForward(g_setMarkers,"setRouteMarkers","routes",0u,world, m.data(), int(m.size() / 4));
		Log("FVO traffic: %zu route probe point(s) sent for %s", m.size() / 4, world);
	}

	void ResetAll() {
        std::unique_lock journeyLock(g_journeyMutex);
		for(auto& s:g_slots)s={};g_poolWorld.clear();g_poolStamp=~0ull;g_testDone.clear();g_lastStart=0;
		g_found.clear();g_read={};g_data={};g_stamp=~0ull;g_reloadWanted=true;g_poolResend=true;
        g_factoryRoute.reset();g_factoryConfig={};g_factoryGeometry={};
		g_initialSlots.clear();g_initialSiteOutput.clear();g_initialClockPending=false;g_emptyTrips.clear();
		g_admissionLanes.clear();g_cruiseLast.clear();g_constraintMinute=-1;g_signalSeconds=g_signalLastTick=0;g_rememberedJourneys.clear();g_journeyProfile.clear();g_rightOfWay.Reset();g_roadDecision.clear();g_approachTiming.clear();g_otherApproachDistance.clear();g_roadStamp=~0ull;
		journeyLock.unlock();generator::Reset();everyday::Reset();
	}  // a save load or a different playthrough

	void Attach()
	{
		const HMODULE core = GetModuleHandleW(L"FlyingVehicleOverhaul.dll");
        g_observedModel=reinterpret_cast<FvoObservedModelProviderV1>(GetProcAddress(core,"FVO_ObservedTrafficModelV1"));
        g_navWant=reinterpret_cast<FvoNavWant_t>(GetProcAddress(core,"FVO_NavWantV1"));
        g_storedModel=reinterpret_cast<FvoObservedModelProviderV1>(GetProcAddress(core,"FVO_StoredTrafficModelV1"));
        g_ownerSnapshot=core?reinterpret_cast<planowner::SnapshotV1>(GetProcAddress(core,"FVO_OwnerSnapshotV1")):nullptr;  // C2, optional
        // Lane coordination (lanecoord.h): all optional; a missing export leaves its pointer null and that part of coordination off.
        g_ownerSnapshot2=core?reinterpret_cast<planowner::SnapshotV2>(GetProcAddress(core,"FVO_OwnerSnapshotV2")):nullptr;
        g_corridorSnapshot=core?reinterpret_cast<lanecoord::SnapshotV1>(GetProcAddress(core,"FVO_CorridorSnapshotV1")):nullptr;
        g_outOfTraffic=core?reinterpret_cast<int(*)(const unsigned*,int)>(GetProcAddress(core,"FVO_VehiclesOutOfTrafficV1")):nullptr;  // C4, optional
        generator::SetSpotTest(core?reinterpret_cast<int(*)(unsigned)>(GetProcAddress(core,"FVO_SpotTestV1")):nullptr);  // ST-B, optional
        generator::SetRelocation(core?reinterpret_cast<FvoRelocationStatusFn>(GetProcAddress(core,"FVO_TrafficRelocationV1")):nullptr);  // relocation receipts, optional (a diagnostic core)
        generator::SetStockDispatch(core?reinterpret_cast<FvoStockTransferFnV1>(GetProcAddress(core,"FVO_TrafficStockTransferV1")):nullptr,&StockRoute);
        g_factoryProof=core?reinterpret_cast<FvoSpotMotionProofQueryV1>(GetProcAddress(core,"FVO_SpotMotionProofV1")):nullptr;
        generator::SetFactoryStockQuery(core?reinterpret_cast<FvoFactoryStockQueryV1>(
            GetProcAddress(core,"FVO_TrafficFactoryStockV1")):nullptr);
        generator::SetFactoryBirth(
            core?reinterpret_cast<FvoFactorySiteFnV1>(GetProcAddress(core,"FVO_TrafficFactorySiteV1")):nullptr,
            core?reinterpret_cast<FvoFactoryBirthFnV1>(GetProcAddress(core,"FVO_TrafficFactoryBirthV1")):nullptr,
            core?reinterpret_cast<FvoFactoryStockBindFnV1>(GetProcAddress(core,"FVO_TrafficFactoryStockBindV1")):nullptr,
            core?reinterpret_cast<FvoFactoryStockObserveFnV1>(GetProcAddress(core,"FVO_TrafficFactoryStockObserveV1")):nullptr);
        g_factoryLease=core?reinterpret_cast<FvoTrafficFactoryLeaseFnV1>(GetProcAddress(core,"FVO_TrafficFactoryLeaseV1")):nullptr;
        generator::SetHealthSources(core?reinterpret_cast<int(*)(unsigned,FvoPairHealthSampleV1*)>(GetProcAddress(core,"FVO_PairHealthSampleV1")):nullptr,
            core?reinterpret_cast<void(*)(unsigned)>(GetProcAddress(core,"FVO_SpotTestRequestV1")):nullptr);  // CH-1/CH-4, optional
        generator::SetPlayClock(core?reinterpret_cast<unsigned(*)()>(GetProcAddress(core,"FVO_PlayClockV1")):nullptr);  // W1, optional
        generator::SetCameraAhead(core?reinterpret_cast<int(*)(double,double,double,double,double*)>(GetProcAddress(core,"FVO_CameraAheadV1")):nullptr);  // CP-1, optional
        generator::SetCameraEnvelopeAhead(core?reinterpret_cast<int(*)(double,double,double,double,double)>(GetProcAddress(core,"FVO_CameraEnvelopeAheadV1")):nullptr);
        generator::SetPhysicalFleet(
            core?reinterpret_cast<FvoFleetRailAssignV1Fn>(GetProcAddress(core,"FVO_FleetRailAssignV1")):nullptr,
            core?reinterpret_cast<FvoFleetRailReleaseV1Fn>(GetProcAddress(core,"FVO_FleetRailReleaseV1")):nullptr,
            core?reinterpret_cast<FvoFleetRailStateV1Fn>(GetProcAddress(core,"FVO_FleetRailStateV1")):nullptr,
            &FleetPath);
        generator::SetBirthRefusal(core?reinterpret_cast<FvoBirthRefusalFnV1>(GetProcAddress(core,"FVO_TrafficBirthRefusalV1")):nullptr);
        generator::SetCameraEvidence(reinterpret_cast<cameravisibility::Reader>(GetProcAddress(core,"FVO_TrafficCameraEvidenceV1")),
            reinterpret_cast<cameravisibility::ForecastReader>(GetProcAddress(core,"FVO_TrafficLookForecastV1")));
		if (!core) return;
#ifndef FVO_RELEASE  // release DLLs: g_log stays null, so Log and observations::Record stop at their null check
		g_log = reinterpret_cast<FvoLog_t>(GetProcAddress(core, "FVO_Log"));
        Log("OBS system=traffic.fleet event=mode matched_core=%d policy=%s",
            int(generator::PhysicalFleetActive()),generator::PhysicalFleetActive()?"physical_pool":"no_population");
		LoadIdMap();
#endif
		g_worldName = reinterpret_cast<FvoWorldName_t>(GetProcAddress(core, "FVO_WorldName"));
		g_baseOf = reinterpret_cast<FvoBaseOf_t>(GetProcAddress(core, "FVO_BaseOf"));
		g_refHeading = reinterpret_cast<FvoRefHeading_t>(GetProcAddress(core, "FVO_RefHeading"));
		g_setMarkers = reinterpret_cast<FvoSetRouteMarkers_t>(GetProcAddress(core, "FVO_SetRouteMarkers"));
		g_refPos = reinterpret_cast<FvoRefPos_t>(GetProcAddress(core, "FVO_RefPos"));
		g_localTime = reinterpret_cast<FvoLocalTime_t>(GetProcAddress(core, "FVO_LocalTime"));
		g_setShift = reinterpret_cast<FvoSetWorldShift_t>(GetProcAddress(core, "FVO_SetWorldShift"));
		// Core API v2: the traffic pairs (generator.cpp) run on core's frames through these hooks. The everyday Scan slot is kept in the
		// frozen hook ABI (traffic_api.h) and left null: no everyday NPC trips.
		if (const auto api = reinterpret_cast<FvoTrafficApi_t>(GetProcAddress(core, "FVO_TrafficApi"))) {
			static const FvoTrafficHooks hooks = { &TrafficTick, &TrafficHousekeeping, &ResetAll, &StartSwap, &generator::RushStatus, &everyday::Scan, &RequestReload };
			g_trafficApi = api(kFvoTrafficApiVersion, &hooks);
			if (g_trafficApi) { generator::Attach(g_trafficApi, g_log); generator::SetSavedFleetJourney(&SavedFleetJourney); everyday::Attach(g_trafficApi, g_log, &Offer); }
		}
		if (!g_trafficApi) Log("FVO traffic: core offers no traffic API v%d (FlyingVehicleOverhaul.dll older or newer than FVO_Traffic.dll); no traffic", int(kFvoTrafficApiVersion));
		if (!g_localTime) Log("FVO traffic: core has no FVO_LocalTime; scheduled routes wait (all-day routes run)");
		if (const auto cycle = reinterpret_cast<FvoRegisterTrafficCycle_t>(GetProcAddress(core, "FVO_RegisterTrafficCycle"))) g_cycleSupported = cycle(&Cycle) != 0;
		if(const auto save=reinterpret_cast<int (*)(int (*)())>(GetProcAddress(core,"FVO_RegisterJourneySaveV1")))save(&SaveJourneys);
		if(const auto lane=reinterpret_cast<FvoRegisterLaneProviderV1>(GetProcAddress(core,"FVO_RegisterLaneProviderV1")))lane(&LaneProvider);
        if(const auto policy=reinterpret_cast<FvoRegisterTrafficPolicyV1>(GetProcAddress(core,"FVO_RegisterTrafficPolicyV1")))g_coordinatorSupported=policy(&PolicyProvider)!=0;
        if(const auto field=reinterpret_cast<FvoRegisterFieldV1>(GetProcAddress(core,"FVO_RegisterFieldGateV1")))g_fieldSupported=field(&FieldProvider)!=0;
        if(const auto recovery=reinterpret_cast<FvoRegisterFieldRecoveryV1>(GetProcAddress(core,"FVO_RegisterFieldRecoveryV1")))recovery(&FieldRecovery);
        if(const auto surface=reinterpret_cast<FvoRegisterRoadSurfaceV1>(GetProcAddress(core,"FVO_RegisterRoadSurfaceV1")))surface(&RoadSurface);
        if(const auto permits=reinterpret_cast<lanecoord::RegisterPermitV1>(GetProcAddress(core,"FVO_RegisterLanePermitV1"))){const int ok=permits(&LanePermit);
            Log("OBS system=traffic.lane_permit event=provider registered=%d planned_lane=%d corridor=%d reason=lane_coordination_exports_found_in_core",ok,int(g_ownerSnapshot2!=nullptr),int(g_corridorSnapshot!=nullptr));}
        if(const auto lanes=reinterpret_cast<FvoRegisterRoadLanesV1>(GetProcAddress(core,"FVO_RegisterRoadLanesV1")))lanes(&RoadLanes);  // optional: the addon provides its road lanes
        // Optional: the core's read-only Census copy (trafficcensusapi.h); spawning works the same without it.
        generator::SetCensusPublisher(reinterpret_cast<FvoTrafficCensusPublishV1>(GetProcAddress(core,"FVO_TrafficCensusPublishV1")),reinterpret_cast<FvoTrafficCensusForgetV1>(GetProcAddress(core,"FVO_TrafficCensusForgetV1")));
        generator::SetHandoffListeners(
            reinterpret_cast<handoff::ReadFacts>(GetProcAddress(core,"FVO_TrafficHandoffFactsV1")),
            reinterpret_cast<handoff::Release>(GetProcAddress(core,"FVO_TrafficHandoffReleaseV1")),
            reinterpret_cast<handoff::RetireShow>(GetProcAddress(core,"FVO_TrafficRetirementShowV2")));
        generator::SetBirthListener(reinterpret_cast<FvoTrafficBirthFnV1>(GetProcAddress(core,"FVO_TrafficBirthRequestV1")));
        generator::SetRoadClearance(reinterpret_cast<int (*)(FvoTrafficVehicle*,int)>(GetProcAddress(core,"FVO_TrafficRoadVehiclesV1")),
            reinterpret_cast<FvoRoadCheckFnV1>(GetProcAddress(core,"FVO_TrafficRoadCheckV1")),
            reinterpret_cast<FvoTrafficBirthFnV2>(GetProcAddress(core,"FVO_TrafficBirthRequestV2")));
        if(const auto set=reinterpret_cast<void (*)(void (*)(unsigned,unsigned))>(GetProcAddress(core,"FVO_TrafficRetirementListenerV1")))
            set(&generator::ReportDriverRetirement);
        if(const auto set=reinterpret_cast<void (*)(int (*)(unsigned,const char*,bool))>(GetProcAddress(core,"FVO_TrafficParkedRetirementV1")))
            set(&generator::DecideParkedRetirement);
        generator::SetPanelCover(reinterpret_cast<int (*)()>(GetProcAddress(core,"FVO_TrafficPanelCoveredV1")));
        if(const auto boxes=reinterpret_cast<FvoRegisterFieldBoxV2>(GetProcAddress(core,"FVO_RegisterFieldBoxV2")))boxes(&FieldBox);
        if(const auto corridor=reinterpret_cast<FvoRegisterCorridorV1>(GetProcAddress(core,"FVO_RegisterCorridorProfileV1")))g_corridorSupported=corridor(&CorridorProvider)!=0;
        if(const auto rules=reinterpret_cast<FvoRegisterTravelRulesV1>(GetProcAddress(core,"FVO_RegisterTravelRulesV1")))g_travelRulesSupported=rules(&TravelRulesProvider)!=0;
		if (const auto plan = reinterpret_cast<FvoRegisterRoutePlanner_t>(GetProcAddress(core, "FVO_RegisterRoutePlanner"))) plan(&PlanRoute);
		if (const auto plan = reinterpret_cast<FvoRegisterCruisePlanner_t>(GetProcAddress(core, "FVO_RegisterCruisePlanner"))) plan(&PlanCruise);
		if (const auto tick = reinterpret_cast<FvoRegisterTick_t>(GetProcAddress(core, "FVO_RegisterTick"))) tick(&Markers);
		if (!g_worldName) { Log("FVO traffic: core has no FVO_WorldName (an older FlyingVehicleOverhaul.dll); traffic files off"); return; }
		if (g_trafficApi) Log("FVO Traffic addon 0.4.0: route traffic attached (playthrough files loaded after the save)");
	}

	void SfseMessage(SFSEMessagingInterface::Message* msg)
	{
		if (msg->type == SFSEMessagingInterface::kMessage_PostDataLoad) Attach();  // every plugin is loaded by now
		if (msg->type == SFSEMessagingInterface::kMessage_PreLoadGame) for (auto& s : g_slots) s.used = false;
	}
}

extern "C" {
__declspec(dllexport) SFSEPluginVersionData SFSEPlugin_Version = {
	SFSEPluginVersionData::kVersion, 6, "FVO_Traffic", "seemless",
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

// Optional authoring capability: the core reads it with GetProcAddress (lane vehicle rules supported).
extern "C" __declspec(dllexport) int FVO_Traffic_LaneVehicleRulesV1(){return 1;}
// CC-1 (2026-11-07): the core's cover owner calls this once on the frame the loading screen closes (kind 3); optional, read with GetProcAddress.
extern "C" __declspec(dllexport) int FVO_Traffic_CoverClosingV1(int kind){return generator::CoverClosingNow(kind);}
// Card FR1: Health's model record for F7 (reader), and your allow (Health applies it on its next tick).
extern "C" __declspec(dllexport) int FVO_Traffic_HealthModelV1(unsigned model,FvoTrafficHealthModelV1* out){if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;return generator::HealthModel(model,*out)?1:0;}
extern "C" __declspec(dllexport) void FVO_Traffic_HealthAllowV1(unsigned model,int allow){if(model)generator::HealthAllow(model,allow!=0);}
// Card RI-4: read-only, additive (traffichealthapi.h).
extern "C" __declspec(dllexport) int FVO_Traffic_HealthReasonV1(unsigned model,char* out,int n){return generator::HealthReason(model,out,n)?1:0;}
extern "C" __declspec(dllexport) int FVO_Traffic_HealthSpotV1(const char* route,int index,FvoTrafficHealthSpotV1* out){if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;return generator::HealthSpot(route,index,*out)?1:0;}
extern "C" __declspec(dllexport) int FVO_Traffic_PlanJourneyV1(const FvoJourneyRequestV1* request,FvoTripClaim* out){return PlanJourney(request,out);}
extern "C" __declspec(dllexport) int FVO_Traffic_InspectV1(const char* route,int index,FvoTrafficInspectV1* out){
    if(!out||out->size!=sizeof(*out)||out->version!=1||index<0)return 0;
    std::lock_guard guard(g_journeyMutex);
    for(const auto& s:g_slots)if(s.used&&!StockRoutePending(s)&&(!route||!*route||s.route==route)){
        if(index--!=0)continue;FvoTrafficInspectV1 value;value.vehicle=s.vehicleId;value.lane=s.trip.lane;value.leg=int(s.trip.leg);
        value.holding=s.holding||s.scriptHolding||s.guidance.held;value.scriptStop=int(s.scriptAt);value.scriptCount=int(s.script.size());
        value.x=s.x;value.y=s.y;value.z=s.z;value.speed=s.measuredSpeed;value.at=s.seen;
        std::snprintf(value.route,sizeof value.route,"%s",s.route.c_str());std::snprintf(value.destination,sizeof value.destination,"%s",s.dest.c_str());*out=value;return 1;
    }
    return 0;
}
extern "C" __declspec(dllexport) int FVO_Traffic_JourneyStatusV1(void* user,FvoJourneyStatusV1* out){
    std::lock_guard guard(g_journeyMutex);
    if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;
    if(const auto* match=slotcookie::Find(g_slots,user);match&&match->used){const auto& s=*match;out->stopIndex=int(s.scriptAt);out->stopCount=int(s.script.size());out->dwelling=s.scriptHolding?1:0;out->blocked=s.guidance.held||s.guidance.stopDistance>=0;
        const auto p=trafficroute::At(s.line,s.trip.lane,trafficroute::Nearest(s.line,s.trip.lane,s.x,s.y));out->laneError=std::hypot(p.x-s.x,p.y-s.y);return 1;}return 0;
}

extern "C" __declspec(dllexport) int FVO_Traffic_SignalJourneyV1(const char* route,unsigned trigger){
    if(!route||!trigger||trigger>65535)return 0;std::lock_guard guard(g_journeyMutex);int count=0;
    for(auto& s:g_slots)if(s.used&&(s.route==route||s.home==route)&&s.scriptAt<s.authoredJourney.stops.size()&&s.authoredJourney.stops[s.scriptAt].wait==journeyplan::Wait::Trigger&&s.authoredJourney.stops[s.scriptAt].trigger==trigger){s.scriptSignal=trigger;++count;}return count;
}

extern "C" __declspec(dllexport) int FVO_Traffic_PopulationV1(int index,FvoPopulationInspectV1* out){if(!out||out->size!=sizeof(*out)||out->version!=1)return 0;return populationinspect::Read(index,*out)?1:0;}

extern "C" __declspec(dllexport) int FVO_Traffic_InspectV2(const char* route,int index,FvoTrafficInspectV2* out){
    if(!out||out->size!=sizeof(*out)||out->version!=2||index<0)return 0;std::lock_guard guard(g_journeyMutex);const int requested=index;
    for(auto& s:g_slots)if(s.used&&!StockRoutePending(s)&&(!route||!*route||s.route==route)){if(index--!=0)continue;FvoTrafficInspectV2 value;if(!FVO_Traffic_InspectV1(route,requested,&value.journey))return 0;const auto restrictions=SpatialRestrictions(Spec(s),s.x,s.y,s.z);value.draining=restrictions.closure==1;value.turning=!s.turn.empty();for(const auto& current:g_data.routes)if(current.name==s.route)value.olderPlan=routecache::Key(current)!=routecache::Key(Spec(s))||journeyplan::Write(current.journey)!=journeyplan::Write(Spec(s).journey);
        const char* why=s.pickupWait?"pickup full; retaining the existing vehicle":s.scriptHolding?s.scriptReason:s.connectionBlocked?"connection or manoeuvre reservation busy":s.holding?"waiting for a lane gap":restrictions.closure==2||!restrictions.valid?restrictions.reason:s.guidance.held?"waiting for route or junction permission":"following committed plan";
        std::snprintf(value.waitReason,sizeof value.waitReason,"%s",why);std::snprintf(value.restriction,sizeof value.restriction,"%s",restrictions.reason);*out=value;return 1;}return 0;
}
extern "C" __declspec(dllexport) int FVO_Traffic_PathInspectV1(int index,FvoTrafficPathInspectV1* out){
    if(!out||out->size!=sizeof(*out)||out->version!=1||index<0)return 0;std::lock_guard guard(g_journeyMutex);for(const auto& s:g_slots)if(s.used&&s.guidance.count>=2){if(index--!=0)continue;FvoTrafficPathInspectV1 value;value.vehicle=s.vehicleId;value.kind=s.guidance.held?2:!s.turn.empty()?1:0;value.at=s.seen;value.width=s.guidance.halfWidth*2;value.count=s.guidance.count;for(int i=0;i<value.count;++i)value.points[i]={s.guidance.points[i].x,s.guidance.points[i].y,s.guidance.points[i].z};*out=value;return 1;}return 0;
}

extern "C" __declspec(dllexport) unsigned FVO_Traffic_AuthoringCapabilitiesV1(){return g_corridorSupported?63u:55u;}

extern "C" __declspec(dllexport) int FVO_Traffic_BoundaryTraceV1(int index,engineport::Record* out,unsigned size){if(!out||size!=sizeof(*out))return 0;return engineport::ReadRecord(index,*out)?1:0;}
