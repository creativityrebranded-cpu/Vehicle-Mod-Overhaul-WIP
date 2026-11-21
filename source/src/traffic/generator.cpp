#include "../populationinspect.h"
#include "engineport.h"
// FVO Traffic: the Generator. Moved from core's trafficpool.cpp on 2026-10-29 (stage 1 of
// notes/TRAFFIC_ADDON_PROPOSAL_20261029.md) with the same rules, timings and "Traffic pool:" log lines; every engine step goes
// through core's API v2 (traffic_api.h).
//
// The spawn and seat steps are the ones confirmed in game on 2026-10-25 (notes/road-traffic-20261025/RESULT.md, run 02:05):
// the game's create-reference for the vehicle and the driver (the driver is seated inside its creation), then, once both have
// 3D, the driver must really sit (sit state > 0), else the instant seat every second, six tries at most.
#include "generator.h"
#include "../trafficsight.h"
#include "../spawnrule.h"
#include "trafficprofileclient.h"
#include "admission.h"
#include "observations.h"
#include "fvolimits.h"
#include "fvopaths.h"
#include "trafficpair.h"
#include "spawnflow.h"
#include "roadclearance.h"
#include "retiringrecord.h"  // SW-1 / SW-2
#include "healthscore.h"
#include "trustzone.h"
#include <cstring>
#include "../trafficcensusapi.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <set>

namespace generator
{
	namespace
	{
		const FvoTrafficApi* g_api = nullptr;
		FvoLog_t g_logFn = nullptr;
		void Log(const char* fmt, ...)
		{
			if (!g_logFn) return;
			char text[700];
			va_list args;
			va_start(args, fmt);
			std::vsnprintf(text, sizeof text, fmt, args);
			va_end(args);
			g_logFn(text);
		}

		constexpr unsigned kDriverBase = kFvoTrafficDriverBase;
        void BoundaryTrace(const engineport::Trace& event) {
#ifndef FVO_RELEASE
            static observations::Gate gate;
            const auto key=observations::Key(event.operation,event.vehicle)^observations::Key(event.caller,event.line);
            const auto state=observations::Key(event.reason,unsigned(event.event));
            if(gate.Sample(GetTickCount64(),key,state).emit)
                Log("traffic.boundary event=%s operation=%s caller=%s vehicle=%08X reason=\"%s\" request=%llu source=%s:%u",engineport::Name(event.event),event.operation,event.caller,event.vehicle,event.reason,static_cast<unsigned long long>(event.request),event.file,event.line);
#else
            (void)event;
#endif
        }
		constexpr unsigned kRev8 = 0xFE001033;        // SFBGS004.esm Rev8, as the addon matches it
		constexpr int kMaxSpots = fvolimits::kVehicles;  // cold-start spots + parking (fvolimits.h)
		constexpr ULONGLONG kCreateGapMs = 2500, kLoadWaitMs = 120000, kSeatEveryMs = 1000, kTripRetryMs = 2000, kWatchMs = 1000;
		constexpr int kRestarts = 5;          // a pair whose trip ended at its spot is started again this often
		constexpr ULONGLONG kIdleMs = 10000;  // ... after standing this long with no trip
		constexpr int kSeatTries = 6;
		constexpr double kSpawnNear = 350.0;  // m: a parking spot's driver only while the spot is this close (its cell is loaded)
		constexpr int kSnapMax = fvolimits::kVehicles;  // core's vehicle list capacity

		std::mutex g_lock;
		std::string g_wantWorld;
		std::vector<FvoPoolSpot> g_wantSpots;
		std::vector<int> g_wantReuse;
		std::vector<std::string> g_wantNames;
		FvoPoolTrip g_trip = nullptr;
        const char* (*g_tripRefusal)(unsigned,int*,int*) = nullptr;  // SR-1: the route side's last refusal reason (a fact)
        int (*g_routeRoom)(int,int*,int*,const char**) = nullptr;     // SR-3: trip room left on a cold-start slot's route
        bool (*g_savedFleetJourney)(unsigned,unsigned,const char*) = nullptr;
        unsigned long long g_revision=0,g_appliedRevision=~0ull;

		enum class St { Unknown, Empty, Created, Ready, Started, Failed };
		struct Pair {
            bool handoffForeign = false;
            handoff::Progress handoff;
            bool handoffPending=false;
            bool handoffEligible=false, takeoverQueued=false;
            spawnflow::ObservedWait handoffWait;
			St st = St::Unknown;
			unsigned vehicle = 0, driver = 0;
			unsigned base = 0;         // the vehicle's base when the pair was made (0: an entry from before 2026-10-29)
            unsigned long long handoffLogAt = 0;  // throttles road_handoff_deferred (was ~100 lines/s per pair)
            unsigned long long handoffDeferSince = 0;  // first deferral: after 10 s the pair is released or cleaned up
			ULONGLONG at = 0, seatAt = 0, tripAt = 0, watchAt = 0, idleSince = 0, askAt = 0, busySince = 0;
			ULONGLONG refusedAt = 0;   // last "cannot start a trip yet" line (a new reason, or every 10 min for the same one)
			unsigned refusedHash = 0;  // the reason last logged (2026-10-29: far spots waiting parked repeated it every 30 s)
			int tries = 0, restarts = 0;
			ULONGLONG no3DSince = 0, reloadAt = 0;  // the driver's 3D reload (see Started)
			int reloads = 0;
            bool observed = false, retired = false, routeWaitTold = false;
            std::wstring storedKey;     // archived row's exact original name, including legacy spot keys
            FvoPoolSpot birth{}; bool reserved = false;
            FvoRoadProofV1 roadProof{};
            bool staged = false;
            bool armed = false; ULONGLONG bornAt = 0, fallCheckAt = 0;
            ULONGLONG frozenAt = 0, rearmAt = 0; int rearms = 0; bool setDown = false; // fall guard pilot recovery
            trustzone::Record zone; ULONGLONG zoneAt = 0, noPhysicsSince = 0; // its place in the trust zone; since when its pilot gets no physics steps (ZoneTick)
            ULONGLONG noBodySince = 0; // staged with a model but no physics body (its cell left the physics world)
            spawnflow::ObservedWait provingBodyWait, provingSeatWait;
        healthscore::Account health;           // CH-3/CH-5: the continuous account (Health's only clock for a driving pair)
        bool brokenSticky = false; ULONGLONG brokenAt = 0;  // when Health first called it broken (Retirement reports the wait)              // CH-5: a Broken verdict not from the account's score never clears by itself
        double hx = 0, hy = 0, hz = 0; bool hHave = false;  // last judged position (moved since the last judgement)
        std::uint64_t relocSerial = 0; double relocToZ = 0; bool relocBase = false;  // the latest relocation receipt applied here: the fall baseline is its destination
        ULONGLONG hPosAt = 0;
        bool uncontrolledFall = false;
        bool fallRetirementPending = false;
        ULONGLONG spotAskAt = 0, accountLogAt = 0;          // CH-4 request pacing; the account's trace
            // Step 7, Health: the verdict, and the facts a step saw that only Health judges. faultConcealed: removal waits for Concealed, not just unseen.
            spawnflow::Verdict verdict = spawnflow::Verdict::Proving; const char* verdictWhy = ""; ULONGLONG judgedAt = 0, readyAt = 0;
            const char* fault = nullptr; bool faultConcealed = false;
            ULONGLONG floatingSince = 0, floatingAskAt = 0; bool broughtTold = false; // the driver seen with no vehicle around them (see Created)
            ULONGLONG precheckAt = 0; bool precheckTold = false; // see PrecheckPair
            bool seatJudged = false, seatTested = false; ULONGLONG both3DSince = 0;          // per-model seat check (did its driver ever sit?)
            ULONGLONG seatPlayMs = 0, seatCheckAt = 0;                   // gameplay time both models were loaded and aboard without sitting
            ULONGLONG emptyPlayMs = 0, emptyCheckAt = 0; const char* emptyCause = nullptr; bool deadTold = false;
            ULONGLONG seatedPlayMs = 0, seatedCheckAt = 0;  // SR-2: play time seated while still being proved (waiting for its trip)  // EV-1: play time its vehicle has had nobody of the pair aboard
            // Card HR1 evidence: the pair's spot key (Demand), which stages it has reported, and where it was released (drive stage).
            std::string spotKey; bool bothLoadedOnce = false, brokenEvidenceJudged = false, bodyJudged = false, releaseAttempted = false, releaseJudged = false, driveJudged = false; double relX = 0, relY = 0;
            double radius=8,halfHeight=6;
            bool vehiclePending=false,driverPending=false;
            bool vehicleIssued=false,driverIssued=false;
            std::uint64_t birthGrant=0;
            FvoBirthSource driverSource=FvoBirthSource::Fleet;
            FvoPoolSpot driverAt{};
            bool proving = false, sending = false; int proveSlot = -1;   // checked over the player before it joins traffic (trafficpair::ProveAt); then sent to birth
            ULONGLONG proveSince = 0, proveTick = 0, seatedSince = 0, sendSince = 0, remoteTryAt = 0;
            // Spawn flow step 2, Census: the pair's one state. Written only by SetLife (Census); every other step reads it.
            spawnflow::Life life = spawnflow::Life::None; const char* lifeWhy = "";
		};
		// 2026-10-29 12:29 run (the user: "vehicles flying without an NPC in it"): a driver whose vehicle flew ~430 m out lost its 3D
		// and never got it back when the vehicle returned to 130 m. A driving pair near the player whose vehicle has 3D and driver
		// none for kNo3DMs: the trip ends, the driver is disabled and enabled kReloadGapMs later (the game loads its 3D), then seated
		// again and given a new trip.
		constexpr double kDriver3DNear = 180.0;
		constexpr ULONGLONG kNo3DMs = 4000, kReloadGapMs = 600;
		constexpr int kMaxReloads = 3;
		constexpr double kFlyingLagReach = 40.0;
		constexpr ULONGLONG kNoModelMs = 3000, kApartMs = 6000;  // a driver seen with no vehicle model / still apart after being brought to it: the pair is disbanded
		std::string g_world;
		std::vector<FvoPoolSpot> g_spots;
		std::vector<int> g_reuse;  // 1: a parking spot: a parked vehicle there gets a driver when a route wants one
		std::vector<std::string> g_names;  // stable spot names (the addon's place / spawn / lane names); empty: by index
		std::vector<Pair> g_pairs;
		ULONGLONG g_createdAt = 0;
        // Population belongs to the world, never to the player's position or a
        // schedule evaluation. Creation is admitted at real, validated origins.
        struct WorldPool {std::vector<FvoPoolSpot> spots;std::vector<int> reuse;std::vector<std::string> names;std::vector<Pair> pairs;};
        std::map<std::string,WorldPool> g_worldPools;
        std::map<std::string,std::set<std::pair<long long,long long>>> g_observedOrigins;
        std::size_t g_observeCursor=0;
        auto OriginKey(const FvoPoolSpot& s){return std::pair{std::llround(s.x/4),std::llround(s.y/4)};}
        bool g_off = false;
        int g_admission = -1; std::size_t g_cursor = 0; ULONGLONG g_admissionAt = 0, g_lastTick = 0;
        std::vector<FvoPoolSpot> g_initialScene;std::set<int> g_initialRejected,g_modelUnavailable;
        std::vector<bool> g_initialRoads;
        std::map<int,std::vector<FvoPoolSpot>> g_initialAlternatives;std::map<int,int> g_initialAttempt,g_initialFails;int g_initialCursor=0;
        bool g_initialMap=false;
        bool g_initialClockPending=false;
        // 2026-11-06 (notes/SPAWN_DESIGN_20261106.md): spawning happens only behind the loading screen or the X panel.
        std::set<int> g_passRefused;              // refused in this window: tried again in the next one
        std::set<int> g_clearanceWaiting;
        bool g_wasCovered=false;std::atomic<ULONGLONG> g_rushAt{0};ULONGLONG g_rushSettlingAt=0;
        std::size_t g_handoffCursor=0;
        handoff::ReadFacts g_handoffFacts=nullptr;
        handoff::Release g_handoffRelease=nullptr;
        std::set<unsigned> g_takenRelease;  // KO-2a Handoff: taken frozen vehicles whose release is still asked for
        // Spawn flow v3 (spawnflow.h). Health's model and spot records (step 7; read by Demand and Siting), this window's Ledger tally
        // (step 10), the Siting source (step 5) and the optional Census copy for core readers (step 2).
        spawnflow::Records g_health;
        spawnflow::Tally g_tally;
        // Cards HR1/HR2: Demand's spot key per slot; Health's file (kept across loads; written by Health only); the test switch (card TS1).
        std::vector<std::string> g_spotKeys;
        // LV-3 live road fill in play (spawnflow.h 4b). Times are GetTickCount64; the recycle slot is chosen once a tick (Tick) and read by Health.
        ULONGLONG g_lastCoveredAt=0,g_liveToldAt=0;
        FvoFleetRailAssignV1Fn g_fleetAssign=nullptr;
        FvoFleetRailReleaseV1Fn g_fleetRelease=nullptr;
        FvoFleetRailStateV1Fn g_fleetState=nullptr;
        FleetPath g_fleetPath=nullptr;
        ULONGLONG g_fleetRefreshAt=0;
        std::atomic<double> g_fleetViewerSpeed{0};
        // A refused live birth delays only its own slot. The core's own reason comes from FVO_TrafficBirthRefusalV1 and is logged in the add-on's own line.
        FvoBirthRefusalFnV1 g_birthRefusal=nullptr;
        bool CoreRefusal(std::uint64_t grant,int slot,FvoBirthRefusalV1& out){out={};out.size=sizeof out;return g_birthRefusal&&grant&&g_birthRefusal(grant,slot,&out)==1;}
        void LogRefusal(const char* where,int slot,unsigned base,std::uint64_t grant,const FvoBirthRefusalV1* r){
            Log("OBS system=traffic.live vehicle=00000000 event=create_refused where=%s slot=%d base=%08X grant=%llu core_known=%d stage=%s reason=%s retry=this_slot_only",
                where,slot,base,static_cast<unsigned long long>(grant),int(r!=nullptr),r?r->stage:"-",r?r->reason:"core_recorded_no_reason_for_this_ticket");
        }
        FvoRelocationStatusFn g_relocation=nullptr;  // the core's relocation status (optional; null on an older core)
        int (*g_spotTest)(unsigned)=nullptr;  // ST-B: Pair Facts, the core's spot test result (0 none, 1 passed, 2 suspect, 3 failed, 4 running)
        bool g_healthLoaded=false;ULONGLONG g_healthSavedAt=0;
        constexpr const char* kHealthBuild=__DATE__ " " __TIME__;  // the build that made a record: a new build ages old evidence
        // F7 reader (card FR1): a copy of the model records, published after each tick; your allows queue here for Health to apply.
        std::mutex g_healthPubLock;std::map<unsigned,spawnhealth::Record> g_healthPub;std::vector<std::pair<unsigned,bool>> g_healthAllowQueue;
        std::map<std::string,spawnhealth::Record> g_healthSpotPub;std::map<unsigned,std::string> g_healthWhy;  // card RI-4: rejected spots, each model's last fail reason (readers: F7)
        std::wstring HealthFile(){return fvopaths::File(L"traffic_health.txt");}
        void HealthLoad(){
            if(g_healthLoaded)return;g_healthLoaded=true;g_health.build=kHealthBuild;
            const auto path=HealthFile();if(path.empty()){Log("OBS system=traffic.health event=file_load result=no_folder");return;}
            std::string text;if(FILE* f=_wfopen(path.c_str(),L"rb")){char buf[4096];std::size_t n;while((n=std::fread(buf,1,sizeof buf,f))>0)text.append(buf,n);std::fclose(f);}
            if(text.empty()){Log("OBS system=traffic.health event=file_load result=none_yet reason=new_record_file");return;}
            const bool ok=g_health.Load(text);int rejected=0;for(const auto& [k,r]:g_health.models)rejected+=r.rejected&&!r.allowed;
            g_health.dirty=false;
            Log("OBS system=traffic.health event=file_load result=%s models=%zu spots=%zu models_rejected=%d build=%s",ok?"read":"unknown_format_ignored",g_health.models.size(),g_health.spots.size(),rejected,kHealthBuild);
        }
        void HealthSave(ULONGLONG now,bool force){
            if(!g_healthLoaded||!g_health.dirty||(!force&&now-g_healthSavedAt<5000))return;
            g_healthSavedAt=now;const auto path=HealthFile();if(path.empty())return;
            const auto text=g_health.Save();const auto tmp=path+L".tmp";bool ok=false;
            if(FILE* f=_wfopen(tmp.c_str(),L"wb")){ok=std::fwrite(text.data(),1,text.size(),f)==text.size();ok=std::fclose(f)==0&&ok;}
            ok=ok&&MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
            if(ok)g_health.dirty=false;
            Log("OBS system=traffic.health event=file_save result=%s models=%zu spots=%zu error=%lu",ok?"written":"failed_kept_in_memory",g_health.models.size(),g_health.spots.size(),ok?0ul:GetLastError());
        }
        void HealthPublish(){
            std::lock_guard lock(g_healthPubLock);
            for(const auto& [id,allow]:g_healthAllowQueue){g_health.Allow(id,allow);Log("OBS system=traffic.health event=user_allow model=%08X allow=%d reason=f7_types",id,int(allow));}
            g_healthAllowQueue.clear();
            g_healthPub.clear();for(const auto& [k,r]:g_health.models)g_healthPub[unsigned(std::strtoul(k.c_str(),nullptr,16))]=r;
            g_healthSpotPub.clear();for(const auto& [k,r]:g_health.spots)if(r.rejected)g_healthSpotPub[k]=r;  // RI-4: only rejected spots are published
        }
        // One report into Health's store, traced with every change it caused (card HR1).
        void Evidence(int i,const Pair& p,spawnhealth::Stage stage,bool pass,const char* why){
            if(!p.base)return;
            if(!pass&&why&&*why){std::lock_guard lock(g_healthPubLock);g_healthWhy[p.base]=why;}  // RI-4: the model's last fail reason, for F7
            const auto c=g_health.Report(stage,p.base,p.spotKey,pass);const auto* r=g_health.Model(p.base);const int s=int(stage);
            Log("OBS system=traffic.health vehicle=%08X driver=%08X event=evidence slot=%d stage=%s result=%s model=%08X spot=%s reason=%s model_pass=%.1f model_fail=%.1f model_spots=%zu",
                p.vehicle,p.driver,i,spawnhealth::Name(stage),pass?"pass":"fail",p.base,p.spotKey.empty()?"none":p.spotKey.c_str(),why,r?r->stage[s].pass:0.0,r?r->stage[s].fail:0.0,r?r->stage[s].with.size():std::size_t(0));
            if(c.modelRejected)Log("OBS system=traffic.health event=model_rejected model=%08X stage=%s pass=%.1f fail=%.1f spots=%zu reason=fails_at_several_spots; Demand leaves it out (trial every %d asks)",p.base,spawnhealth::Name(stage),r->stage[s].pass,r->stage[s].fail,r->stage[s].with.size(),spawnhealth::rules::retrialEvery);
            if(c.modelCleared)Log("OBS system=traffic.health event=model_cleared model=%08X stage=%s reason=passed_since",p.base,spawnhealth::Name(stage));
            if(c.spotRejected)Log("OBS system=traffic.health event=spot_rejected slot=%d spot=%s stage=%s reason=fails_with_several_models",i,p.spotKey.c_str(),spawnhealth::Name(stage));
            if(c.spotCleared)Log("OBS system=traffic.health event=spot_cleared slot=%d spot=%s stage=%s",i,p.spotKey.c_str(),spawnhealth::Name(stage));
            if(c.unexplained)Log("OBS system=traffic.health event=unexplained slot=%d model=%08X spot=%s stage=%s reason=same_model_same_spot_keeps_failing; no verdict: likely an FVO fault, not the model or the spot",i,p.base,p.spotKey.c_str(),spawnhealth::Name(stage));
        }
        // A pair Health called broken: its fail goes to the stage it never got past (only when that stage could be tested).
        void BrokenEvidence(int i,const Pair& p,const char* why){
            if(why&&(!std::strcmp(why,"taken_by_the_player")||!std::strcmp(why,"driver_dead")||!std::strcmp(why,"empty_vehicle")||!std::strcmp(why,"seated_no_trip")))return;  // SR-2: no trip is the route side's, not the model's or the spot's  // EV-1: an emptied car is the world's doing, not the model's or the spot's
            if(!p.bothLoadedOnce){Log("OBS system=traffic.health vehicle=%08X event=not_tested slot=%d reason=%s why=models_never_loaded",p.vehicle,i,why?why:"");return;}
            if(p.releaseAttempted&&!p.releaseJudged)Evidence(i,p,spawnhealth::Stage::Release,false,why?why:"broken_before_release");
            else if(p.releaseJudged&&!p.driveJudged)Evidence(i,p,spawnhealth::Stage::Drive,false,why?why:"broken_while_driving");
        }
        FvoTrafficCensusPublishV1 g_censusPublish=nullptr;FvoTrafficCensusForgetV1 g_censusForget=nullptr;
        FvoTrafficBirthFnV1 g_birthListener=nullptr;
        FvoTrafficBirthFnV2 g_roadBirth=nullptr;
        FvoRoadCheckFnV1 g_roadCheck=nullptr;
        int (*g_roadVehicles)(FvoTrafficVehicle*,int)=nullptr;
        std::set<int> g_madeThisWindow;   // card C2: slots given a birth in the open window (the Ledger's made vs kept)

		struct Pos { double x = 0, y = 0, z = 0; };
		using Snap = std::vector<FvoTrafficVehicle>;
        // Step 2, Census: the only writer of a pair's state. A transition the flow never makes is refused and traced, never applied.
        // The core's copy (trafficcensusapi.h) is published when the core offers it; it is a read-only copy, never a second registry.
        void CensusPublish(int slot,const Pair& p);
        void SetLife(int slot,Pair& p,spawnflow::Life to,const char* why){
            if(p.life==to)return;
            if(!spawnflow::Allowed(p.life,to)){
                Log("OBS system=traffic.census vehicle=%08X driver=%08X event=refused slot=%d from=%s to=%s reason=%s",p.vehicle,p.driver,slot,spawnflow::Name(p.life),spawnflow::Name(to),why);return;}
            Log("OBS system=traffic.census vehicle=%08X driver=%08X event=state slot=%d from=%s to=%s control=%d reason=%s",p.vehicle,p.driver,slot,spawnflow::Name(p.life),spawnflow::Name(to),spawnflow::Control(to),why);
            p.life=to;p.lifeWhy=why;
            if(!p.vehicle)return;  // no vehicle yet (creation queued): CensusPublish sends this state once the id arrives (change card C2)
            if(to==spawnflow::Life::Gone||to==spawnflow::Life::None){if(g_censusForget)g_censusForget(p.vehicle);return;}
            CensusPublish(slot,p);
        }
        // S1 (spawn one-place cards): the pair's lifecycle is the Census (Life). Birth's own progress (St) is read only by Birth; other steps
        // read these named answers. Driving: handed to the driving pipeline (Driving or Recovering). BirthFailed: Birth's fact that it
        // could not make or seat the pair (St::Failed). SlotFree: the Arbiter's free slot (nothing made there, nothing queued).
        bool Driving(const Pair& p){return !p.proving&&(p.life==spawnflow::Life::Driving||p.life==spawnflow::Life::Recovering);}
        bool BirthFailed(const Pair& p){return p.st==St::Failed;}
        bool SlotFree(const Pair& p){return p.st==St::Empty;}
        // The core's read-only copy of the current state (trafficcensusapi.h). Also called when a queued creation gives the vehicle its id,
        // so the copy never misses a pair born before its id existed (2026-11-06 run: every Census line had vehicle 0).
        void CensusPublish(int slot,const Pair& p){
            if(!g_censusPublish||!p.vehicle)return;
            FvoTrafficCensusV1 c;c.vehicle=p.vehicle;c.driver=p.driver;c.model=p.base;c.slot=slot;c.life=int(spawnflow::ToOwner(p.life));c.control=spawnflow::Control(p.life);c.at=GetTickCount64();
            std::snprintf(c.reason,sizeof c.reason,"%s",p.lifeWhy?p.lifeWhy:"");
            if(g_censusPublish(&c)==2)Log("OBS system=traffic.census vehicle=%08X event=stronger_owner slot=%d reason=core_owner_record_holds_it",p.vehicle,slot);
        }

		bool Info(unsigned id, FvoTrafficRef& r) { return id && engineport::Forward<&FvoTrafficApi::ref>(g_api,"ref","generator",0u,id, &r) != 0; }
		bool Exists(unsigned id) { FvoTrafficRef r; return Info(id, r); }
		unsigned BaseId(unsigned id) { FvoTrafficRef r; return Info(id, r) ? r.baseId : 0; }
		bool Usable(unsigned id, bool& has3D)
		{
			FvoTrafficRef r;
			has3D = false;
			if (!Info(id, r) || !r.usable) return false;
			has3D = r.has3D != 0;
			return true;
		}
		bool LooksLikeActor(unsigned id) { FvoTrafficRef r; return Info(id, r) && r.isActor; }
		bool IsDead(unsigned id) { FvoTrafficRef r; return Info(id, r) && r.dead; }
		Pos PosOf(unsigned id) { FvoTrafficRef r; Pos p; if (Info(id, r)) { p.x = r.x; p.y = r.y; p.z = r.z; } return p; }
		bool Aboard(unsigned driver, unsigned vehicle) { return driver && vehicle && engineport::Forward<&FvoTrafficApi::vehicleOf>(g_api,"vehicleOf","generator",0u,driver) == vehicle; }
		bool HasBody(unsigned vehicle,const Snap& snap){return std::any_of(snap.begin(),snap.end(),[&](const auto& v){return v.id==vehicle&&v.hasBody;});}
		// Seated (2026-10-29 22:36 run): the game reports sit state 0 until it next processes the driver; aboard with both models
		// and the driver on the vehicle (under 1 m) is in the seat.
		bool SeatedPair(bool v3D,bool d3D,bool aboard,int sit,double separation){
			return trafficpair::ReadyAt(v3D,d3D,aboard,sit,separation);}  // sitting (sit state 3), not merely linked
		bool Grounded(const Pos& vp){double nav=0,baked=0;const int known=engineport::Forward<&FvoTrafficApi::ground>(g_api,"ground","generator",0u,vp.x,vp.y,vp.z,&nav,&baked);return known&&std::isfinite(known&2?baked:nav)&&std::fabs(vp.z-(known&2?baked:nav))<2.5;}

		// Bound at the start of a frame, so a concurrent load cannot redirect an old frame's writes.
        std::wstring g_profileIni;
#ifdef FVO_GENERATOR_TEST
        std::wstring g_testIni;
        const std::wstring& Ini(){return g_testIni;}
#else
        const std::wstring& Ini() { return g_profileIni; }
#endif
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		std::wstring PairKey(int i)
		{
			const bool named = i < int(g_names.size()) && !g_names[std::size_t(i)].empty();
			return named ? L"pair_" + Wide(g_names[std::size_t(i)]) : L"spot" + std::to_wstring(i);
		}
        bool WriteRegistry(const wchar_t* section, const wchar_t* key, const wchar_t* value) {
#ifndef FVO_GENERATOR_TEST
            // Lock only the file operation, never engine queries/mutations made by Tick.
            trafficprofileclient::Guard files;
            if (!files) return false;
#endif
            return WritePrivateProfileStringW(section, key, value, Ini().c_str())!=0;
        }
		void LoadIds(int i, Pair& p)
		{
			wchar_t text[64]{};
			GetPrivateProfileStringW(Wide(g_world).c_str(), PairKey(i).c_str(), L"", text, 64, Ini().c_str());
			const auto row=retiringrecord::ParsePair(text);
			p.vehicle=row.vehicle;p.driver=row.driver;p.base=row.base;p.staged=row.staged;
		}
		bool SaveIds(int i, const Pair& p)
		{
			const auto value=retiringrecord::FormatPair({p.vehicle,p.driver,p.base,p.staged,p.retired});
			const auto key=p.storedKey.empty()?PairKey(i):p.storedKey;
			return WriteRegistry(Wide(g_world).c_str(),key.c_str(),value.c_str());
		}

        // A landing spot works like a placement: a vehicle waits there, one per spot, once the player is in range, and stays for later. Kept in trafficpool.ini per world; a remembered id is checked by its base.
        struct Landing{FvoPoolSpot at{};std::string key;unsigned vehicle=0,base=0;bool loaded=false,pending=false;FvoPoolSpot requestAt{};};
        std::vector<Landing> g_landings;
        bool g_coldFilling=false;
        ULONGLONG g_landingAt=0;
        void SaveLanding(const Landing& l){
            const std::wstring key=L"landing_"+std::wstring(l.key.begin(),l.key.end()),section(g_world.begin(),g_world.end());
            wchar_t text[64]{};if(l.vehicle)swprintf_s(text,L"%08X %08X",l.vehicle,l.base);
            WriteRegistry(section.c_str(),key.c_str(),l.vehicle?text:nullptr);
        }
        constexpr double kRidFar = 350.0;  // no notice distance from the core: this far counts as far
        Pos g_playerAt{};
        // V1 (spawn one-place cards, strip build 3): the one answer to "can the player notice this point?". Every unseen rule below reads
        // it and none measures on its own. Each part is measured at most once per answer, only when a rule asks for it.
        struct Sight {
            double x = 0, y = 0, z = 0; unsigned base = 0; bool driver = false; int screen = 0;  // screen: presentation (2+ covered, 1 play)
            int exposure_ = -1; double notice_ = -1;
            bool Covered() const { return screen >= 2; }
            bool Play() const { return screen == 1; }
            double Dist() const { return std::hypot(x - g_playerAt.x, y - g_playerAt.y); }
            double Notice() { if (notice_ < 0) notice_ = g_api->noticeFar ? engineport::Forward<&FvoTrafficApi::noticeFar>(g_api, "noticeFar", "generator", 0u, driver ? 0u : base) : kRidFar; return notice_; }
            bool Far(bool fell = false) { return spawnflow::RetirementDistance(Dist(), z - g_playerAt.z, fell) >= Notice(); }
            int Exposure() { if (exposure_ < 0) exposure_ = g_api->exposure ? engineport::Forward<&FvoTrafficApi::exposure>(g_api, "exposure", "generator", 0u, x, y, z) : 0; return exposure_; }  // 1 seen, 2 hidden, 0 unknown
        };
        Sight SightAt(double x, double y, double z, unsigned base = 0, bool driver = false) {
            Sight s; s.x = x; s.y = y; s.z = z; s.base = base; s.driver = driver;
            s.screen = g_api->presentation ? engineport::Forward<&FvoTrafficApi::presentation>(g_api, "presentation", "generator", 0u) : 0;
            return s;
        }
        // Siting's rule for a landing spot: covered, or hidden from the player's line of sight.
        bool LandingUnseen(double x,double y,double z){
            if(!g_api->presentation)return false;
            auto s=SightAt(x,y,z);return s.Covered()||s.Exposure()==2;
        }
        // Birth's rule for a spot it acts at: the map covers it, or it is hidden and the player has never seen that spot.
        bool Concealed(const FvoPoolSpot& sp,Pair& p){
            if(!g_api->presentation||!g_api->exposure)return false;
            auto s=SightAt(sp.x,sp.y,sp.z);if(!s.screen)return false;
            if(s.screen==2)return true;
            const int visibility=s.Exposure();
            if(visibility==1){p.observed=true;g_observedOrigins[g_world].insert(OriginKey(sp));}
            if(g_observedOrigins[g_world].count(OriginKey(sp)))p.observed=true;
            return !p.observed&&visibility==2;
        }
        bool Measure(Pair& p,unsigned base){if(!g_api->hullSize||!engineport::Forward<&FvoTrafficApi::hullSize>(g_api,"hullSize","generator",0u,base,&p.radius,&p.halfHeight)){p.radius=8;p.halfHeight=6;return false;}return true;}  // no stored bounds: generous default
        bool Retiring(unsigned id);
        RoadSpacingGate g_roadSpacing=nullptr;
        const char* Occupied(int self,const FvoPoolSpot& at,const Snap& snap,const RoadSite& road,double& gap,const Pair* moving=nullptr){
            const auto* shape=moving?moving:self>=0&&self<int(g_pairs.size())?&g_pairs[self]:nullptr;
            const double radius=shape?shape->radius:8;
            const double height=shape?shape->halfHeight:6;
            const unsigned own=shape?shape->vehicle:0;
            const spawnflow::Pose site{at.x,at.y,at.z,at.heading};
            const auto spacing=[&](const FvoPoolSpot& other,double otherRadius,double otherHeight,bool onRoad){
                const auto local=spawnflow::VehicleSpacing(site,radius,height,{other.x,other.y,other.z},otherRadius,otherHeight);
                if(local)return local;
                return onRoad&&road.interval>0&&g_roadSpacing&&g_roadSpacing(self,road,other,gap)?"kilometre_gap_occupied":nullptr;
            };
            for(const auto& v:snap)if(v.id&&v.id!=own){
                const auto why=spacing({v.x,v.y,v.z,0},v.radius>0?v.radius:8,v.halfHeight>0?v.halfHeight:6,true);
                if(!why)continue;
                FvoTrafficRef seen{};if(!v.hasBody&&Info(v.id,seen)&&seen.disabled&&!seen.has3D)continue;
                return why;
            }
            // Include pending native creations, not just this frame's stale snapshot.
            for(int j=0;j<int(g_pairs.size());++j){if(j==self)continue;const auto& q=g_pairs[std::size_t(j)];
                if(own&&q.vehicle==own)continue;
                FvoTrafficRef ref{};const bool live=Info(q.vehicle,ref);
                const auto pos=live&&!q.proving&&!q.sending?FvoPoolSpot{ref.x,ref.y,ref.z,0}:q.birth;  // one being checked holds the place it is going to
                if(live||q.reserved){
                    const bool pending=q.vehiclePending||q.proving||q.sending;
                    if(const auto why=spacing(pos,q.radius,q.halfHeight,live||pending))return why;
                }
            }
            return nullptr;
        }

        FvoRushState g_rush{};std::mutex g_rushLock;
        std::atomic<bool> g_rushRequest{false};
        bool g_rushFill=false;int g_rushMade=0; // Start/Swap fill in progress under the editor panel
        bool g_coverPassDone=false;
        bool g_rosterRecoveryPending=true;
        ULONGLONG g_linkedRecoveryAt=0,g_registryRecoveryAt=0;
        struct DeferredPair { std::wstring key; retiringrecord::PairRow row; };
        std::vector<DeferredPair> g_deferredRegistry;
        std::size_t g_registryRecoveryCursor=0;
        constexpr ULONGLONG kSavedSlotGraceMs=8000,kLinkedRecoveryIntervalMs=3000,kRegistryRecoveryIntervalMs=250;
        constexpr std::size_t kRegistryRecoveryBatch=12;
        int (*g_panelCover)()=nullptr;
        bool XPanelCovered(){return !g_panelCover||g_panelCover()!=0;}
        // Spares: a list older versions saved (key `spares`: hidden vehicle/driver pairs kept for reuse). Nothing adds to it any more;
        // CoverPass retires what is still listed and Retirement drops entries as they go, so the record empties itself.
        struct Spare{unsigned vehicle=0,driver=0,base=0;};std::vector<Spare> g_spares;std::string g_sparesWorld;
        // Retirement owns these ids until save reset, even after deletion is acknowledged. An orphan scan or stale
        // spare entry must never reclaim a reference while the engine still has deferred work for it.
        std::set<unsigned> g_retiring;
        bool Retiring(unsigned id){return id&&g_retiring.count(id)!=0;}
        void SaveSpares(){
            if(g_world.empty())return;std::wstring list;
            for(const auto& sp:g_spares){wchar_t b[40];swprintf(b,40,L"%s%08X %08X %08X",list.empty()?L"":L", ",sp.vehicle,sp.driver,sp.base);list+=b;}
            WriteRegistry(Wide(g_world).c_str(),L"spares",list.c_str());
        }
        void LoadSpares(){
            if(g_sparesWorld==g_world)return;g_sparesWorld=g_world;g_spares.clear();
            std::vector<wchar_t> buf(65536);GetPrivateProfileStringW(Wide(g_world).c_str(),L"spares",L"",buf.data(),DWORD(buf.size()),Ini().c_str());
            for(const wchar_t* c=buf.data();*c;){Spare sp{};wchar_t* e=nullptr;
                sp.vehicle=unsigned(std::wcstoul(c,&e,16));if(e==c){++c;continue;}c=e;
                sp.driver=unsigned(std::wcstoul(c,&e,16));if(e==c)break;c=e;
                sp.base=unsigned(std::wcstoul(c,&e,16));if(e==c)break;c=e;
                // A remembered id can be something else in this save: only a vehicle that still has that base counts.
                FvoTrafficRef v{},d{};
                if(sp.vehicle&&sp.base&&!Retiring(sp.vehicle)&&!Retiring(sp.driver)&&g_api->ref&&engineport::Forward<&FvoTrafficApi::ref>(g_api,"ref","generator",0u,sp.vehicle,&v)&&v.baseId==sp.base&&engineport::Forward<&FvoTrafficApi::ref>(g_api,"ref","generator",0u,sp.driver,&d)&&d.isActor&&!d.dead)g_spares.push_back(sp);
                while(*c==L','||*c==L' ')++c;}
        }
        // Only a pair on a trip, and not one just born: a new pair is still waiting for its models, its seat and its pilot.
        // A pair given up on (St::Failed) is bad too: it has no job, so nothing else ever looks at it again.
        trafficpair::Bad PairVerdict(const Pair& p,ULONGLONG now){
            if(p.retired||!p.vehicle||!p.driver||!p.base)return trafficpair::Bad::None;
            FvoTrafficRef v{},d{};if(!Info(p.vehicle,v)||!Info(p.driver,d))return trafficpair::Bad::None;
            if(g_api->pairHealth&&engineport::Forward<&FvoTrafficApi::pairHealth>(g_api,"pairHealth","generator",0u,p.vehicle)==int(trafficpair::PairHealth::Broken))return trafficpair::Bad::Unrecoverable;
            if(BirthFailed(p))return trafficpair::Bad::GaveUp;
            if(!Driving(p)||(p.bornAt&&now-p.bornAt<=10000))return trafficpair::Bad::None;
            if(d.disabled&&!p.reloadAt)return trafficpair::Bad::DriverOut;  // the driver watch hid them: nobody is flying this vehicle
            const bool holds=g_api->pilotHolds&&engineport::Forward<&FvoTrafficApi::pilotHolds>(g_api,"pilotHolds","generator",0u,p.vehicle),aboard=engineport::Forward<&FvoTrafficApi::vehicleOf>(g_api,"vehicleOf","generator",0u,p.driver)==p.vehicle;
            // Got out and standing on the ground: pulled out, a fight, a wreck. That is the world working, not a broken pair (the record says Healthy for it). The vehicle stays.
            if(!aboard&&!d.dead&&g_api->pairHealth&&engineport::Forward<&FvoTrafficApi::pairHealth>(g_api,"pairHealth","generator",0u,p.vehicle)==int(trafficpair::PairHealth::Healthy))return trafficpair::Bad::None;
            const double distance=std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z);
            // A flying driver's reference lags behind the vehicle while the game has them in the seat (2026-10-29 21:27 run): not bad on that alone.
            if(aboard&&distance<=(holds?kFlyingLagReach:8.0)&&trafficpair::Sitting(engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver)))return trafficpair::Bad::None;
            return trafficpair::JudgePair(v.has3D!=0,d.has3D!=0,d.dead!=0,aboard,distance,holds,Grounded({v.x,v.y,v.z}));
        }
        // Before a pair's models load nothing of it can be seen (2026-10-30, the user's predictive cleaning). A pair with no pilot whose driver has drifted from its vehicle
        // is put right then: it does not take a look at the ground, or at why (unloaded actors move along their AI package). What cannot be checked yet is the pair health
        // record's job once the models are there: recover, and if not, the removal rule (UnseenPair). Nothing is placed on the ground: a vehicle there is noticed on foot.
        constexpr double kWindowFar = 1500.0;
        void PrecheckPair(int i, Pair& p, const Pos& pp, ULONGLONG now) {
            if (!p.vehicle || !p.driver || p.retired || now - p.precheckAt < 2000) return;
            p.precheckAt = now;
            FvoTrafficRef v{}, d{};
            if (!Info(p.vehicle, v) || !Info(p.driver, d) || v.has3D || d.has3D || !v.usable || !d.usable || d.dead) return;
            if (std::hypot(v.x - pp.x, v.y - pp.y) > kWindowFar) return;
            const double apart = std::hypot(std::hypot(v.x - d.x, v.y - d.y), v.z - d.z);
            if (apart <= trafficpair::kSnapReach || engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,p.vehicle)) return;  // a pilot's job keeps its own driver on the vehicle
            // Never the mover (2026-10-31 11:02 run: 35 of these through moveTo, 18 pairs then failed with the seat link gone; FUN_142062F50 releases the seat).
            if (g_api->holdDriver) {
                engineport::Forward<&FvoTrafficApi::holdDriver>(g_api,"holdDriver","generator",0u,p.driver, p.vehicle);
                if (!p.precheckTold) {
                    p.precheckTold = true;
                    Log("Traffic precheck: spot %d: driver %08X was %.1f m from vehicle %08X, both with no model: put back on it unseen", i, p.driver, apart, p.vehicle);
                }
            }
        }
        // A pair is one unit (2026-10-30, the user: "when you disband one you can easily disband its vehicle too"; hiding only the driver
        // left empty vehicles scattered along the routes). Both are hidden, never deleted (deleting traffic has crashed the game), and the
        // position is filled again, at its next lane alternative if it has one, so a spot that keeps failing varies. A pair that already
        // came off the shelf and failed again stays hidden for good: it is not offered a third time.
        // The line keeps the evidence the disband removes from view (the user: does this also find the cause?): the model, where the pair
        // was born and where each half ended up, so a run shows what the failures share.
        // Hidden now, deleted once the hide has landed (a disable is queued).
        struct Doomed{unsigned id;ULONGLONG at;bool submitted=false;unsigned baseId=0;ULONGLONG retryAt=0,missingSince=0;int refused=0;unsigned chassis=0;bool actor=false;};
        std::vector<Doomed> g_doomed;
        // SW-1 Census (2026-11-07, user: "yes write both but maintain mandate"): a pair being retired stays in the Census, saved per world
        // in trafficpool.ini under the additive key `retiring` (vehicle vbase driver dbase, comma separated), until Retirement reports it
        // gone. Retirement reports (CensusRetiringSave / CensusRetiringClear); only the Census writes the record. Older files: no key.
        using RetiringEntry=retiringrecord::Entry;  // the pure parts: retiringrecord.h (tests/traffic_rules_test.cpp)
        std::vector<RetiringEntry> g_retiringSaved;std::string g_retiringWorld;
        void SaveRetiringSaved(){
            if(g_world.empty())return;const std::wstring list=retiringrecord::Format(g_retiringSaved);
            WriteRegistry(Wide(g_world).c_str(),L"retiring",list.empty()?nullptr:list.c_str());
        }
        void LoadRetiringSaved(){
            if(g_retiringWorld==g_world)return;g_retiringWorld=g_world;
            std::vector<wchar_t> buf(65536);GetPrivateProfileStringW(Wide(g_world).c_str(),L"retiring",L"",buf.data(),DWORD(buf.size()),Ini().c_str());
            g_retiringSaved=retiringrecord::Parse(buf.data());
        }
        void CensusRetiringSave(unsigned vehicle,unsigned driver,const char* why){
            if(!vehicle&&!driver)return;LoadRetiringSaved();
            for(const auto& e:g_retiringSaved)if((vehicle&&e.vehicle==vehicle)||(driver&&e.driver==driver))return;
            FvoTrafficRef v{},d{};RetiringEntry e;e.vehicle=vehicle;e.driver=driver;
            if(vehicle&&Info(vehicle,v))e.vbase=v.baseId;if(driver&&Info(driver,d))e.dbase=d.baseId?d.baseId:kDriverBase;else if(driver)e.dbase=kDriverBase;
            g_retiringSaved.push_back(e);SaveRetiringSaved();
            Log("OBS system=traffic.census vehicle=%08X driver=%08X event=retiring_saved base=%08X driver_base=%08X reason=%s",vehicle,driver,e.vbase,e.dbase,why?why:"");
        }
        void CensusRetiringClear(unsigned id,const char* why,unsigned paired=0){
            if(!id&&!paired)return;LoadRetiringSaved();
            const bool first=retiringrecord::Clear(g_retiringSaved,id);
            const bool second=retiringrecord::Clear(g_retiringSaved,paired);
            if(!first&&!second)return;
            SaveRetiringSaved();Log("OBS system=traffic.census vehicle=%08X event=retiring_cleared reason=%s",id,why);
        }
        // A failed lookup is not proof the reference is gone (2026-10-31 18:59 run: FF016B4E read as gone at 19:02:53, built again at 19:04:23 and stayed parked with no driver).
        constexpr ULONGLONG kDoomedMissingMs=300000,kDeleteRetryMs=5000,kDeleteRetryMaxMs=60000;
        constexpr int kDeleteGiveUp=6;
        ULONGLONG g_clock=0;  // this update's time
        // Step 9, Retirement: the only remover. Every removal in the addon comes here with its reason; each caller has already waited for a cover
        // or for the pair to be out of sight (spawnflow::MayRemove). Never a vehicle the world owns: one the player took is refused here.
        struct RetirementOrder {
            unsigned vehicle,driver;
            const char* why;
            const char* rule;
            spawnflow::Removal decision;
        };
        handoff::RetireShow g_retireShowV2=nullptr;
        void ApplyRetirement(const RetirementOrder& order){
            if(!order.decision.now)return;
            const auto [vehicle,driver,why,rule,decision]=order;
            if (vehicle && g_fleetRelease) g_fleetRelease(vehicle);
            Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=grant speaker=arbiter reason=%s rule=%s",vehicle,driver,why,rule);
            CensusRetiringSave(vehicle,driver,why);  // SW-1: kept in the Census until gone, across loads
            ++g_tally.retired;if(vehicle&&g_censusForget)g_censusForget(vehicle);  // Census: gone from traffic; the core copy forgets only traffic metadata
            Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=request already_retiring=%d reason=%s",vehicle,driver,Retiring(vehicle),why);
            if(vehicle&&engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,vehicle))engineport::Forward<&FvoTrafficApi::endJob>(g_api,"endJob","generator",0u,vehicle,"traffic: unhealthy pair deleted");
            const auto oldSize=g_spares.size();
            g_spares.erase(std::remove_if(g_spares.begin(),g_spares.end(),[&](const Spare& sp){return sp.vehicle==vehicle||sp.driver==driver||sp.driver==vehicle||sp.vehicle==driver;}),g_spares.end());
            for(const unsigned id:{vehicle,driver})if(id){
                const bool camera=std::strncmp(rule,"deep_below_terrain",18)!=0;
                if(g_retireShowV2)g_retireShowV2(id,vehicle,id==driver,camera);
                if(g_retiring.insert(id).second){
                    FvoTrafficRef known{};Info(id,known);
                    g_doomed.push_back({id,g_clock});
                    g_doomed.back().baseId=known.baseId;
                    g_doomed.back().chassis=vehicle;
                    g_doomed.back().actor=id==driver;
                    Log("OBS system=traffic.retirement vehicle=%08X event=queued base=%08X lookup_ok=%d disabled=%d model=%d",id,known.baseId,known.id!=0,known.disabled,known.has3D);
                }
            }
            if(g_spares.size()!=oldSize)SaveSpares();
        }
        auto FinishRetirement(std::vector<Doomed>::iterator entry,const char* reason){
            if(reason)CensusRetiringClear(entry->id,reason);
            g_retiring.erase(entry->id);
            return g_doomed.erase(entry);
        }
        void DeleteTick(ULONGLONG now){
            int done=0;
            for(auto it=g_doomed.begin();it!=g_doomed.end();){
                if(it->chassis&&g_api->playerTook&&g_api->playerTook(it->chassis)){
                    g_takenRelease.insert(it->chassis);
                    it=FinishRetirement(it,"taken_by_the_player");continue;
                }
                FvoTrafficRef r{};
                if(now-it->at<2000){++it;continue;}
                if(!Info(it->id,r)){
                    if(it->submitted){Log("OBS system=traffic.retirement vehicle=%08X event=gone_after_delete",it->id);it=FinishRetirement(it,"gone_after_delete");continue;}
                    if(!it->missingSince){it->missingSince=now;Log("OBS system=traffic.retirement vehicle=%08X event=lookup_failed base=%08X",it->id,it->baseId);}
                    if(now-it->missingSince<kDoomedMissingMs){++it;continue;}
                    Log("OBS system=traffic.retirement vehicle=%08X event=gave_up lookup_failed_ms=%llu",it->id,now-it->missingSince);
                    it=FinishRetirement(it,"gave_up_lookup_failed");continue;
                }
                const bool same=(r.isActor!=0)==it->actor&&
                    (it->baseId?r.baseId==it->baseId:!it->actor);
                if(!same){
                    Log("OBS system=traffic.retirement vehicle=%08X event=identity_changed base=%08X was=%08X actor=%d",it->id,r.baseId,it->baseId,r.isActor);
                    it=FinishRetirement(it,"id_reused");continue;
                }
                if(it->missingSince){
                    // The id came back. The game reuses ids: act only if it still names what was retired, and nothing here uses it now. A driver's base is
                    // not checked by the core, so one whose base was never read is left alone.
                    const unsigned id=it->id;
                    const bool inUse=std::any_of(g_pairs.begin(),g_pairs.end(),[&](const Pair& q){return q.vehicle==id||q.driver==id;});
                    Log("OBS system=traffic.retirement vehicle=%08X event=lookup_returned after_ms=%llu base=%08X was=%08X actor=%d same=%d in_use=%d model=%d",id,now-it->missingSince,r.baseId,it->baseId,r.isActor,same,inUse,r.has3D);
                    if(inUse){it=FinishRetirement(it,"in_use_again");continue;}
                    it->missingSince=0;
                    if(!r.disabled&&g_api->show)engineport::Forward<&FvoTrafficApi::show>(g_api,"show","generator",0u,id,0);
                }
                if(now-it->at<30000&&!r.disabled){++it;continue;}
                if(it->retryAt&&now<it->retryAt){++it;continue;}  // refused: waits without using this update's turns
                if(++done>2)break;  // a few at a time, so no burst of teardowns lands in one frame
                if(it->submitted&&r.disabled&&!r.has3D){Log("OBS system=traffic.retirement vehicle=%08X event=retired",it->id);it=FinishRetirement(it,"retired");continue;}
                if(!r.disabled){if(g_api->show)engineport::Forward<&FvoTrafficApi::show>(g_api,"show","generator",0u,it->id,0);++it;continue;}
                // Disabling is asynchronous. Do not delete a reference while its model may still be used by a
                // background update (18:10 crash: FF014591, Get3D returned null in FUN_141313D80).
                if(r.has3D){++it;continue;}
                if(!it->submitted&&g_api->deleteRef){
                    if(engineport::Forward<&FvoTrafficApi::deleteRef>(g_api,"deleteRef","generator",0u,it->id)){
                        it->submitted=true;Log("Traffic pool: %08X native delete completed; waiting for model retirement",it->id);
                    }else if(!(g_api->mutationPending&&engineport::Forward<&FvoTrafficApi::mutationPending>(g_api,"mutationPending","generator",0u,it->id,"delete"))){
                        // Finished and refused (the core logs why). Asking again every update held a request slot for good.
                        const ULONGLONG wait=std::min(kDeleteRetryMs<<std::min(it->refused,4),kDeleteRetryMaxMs);
                        ++it->refused;it->retryAt=now+wait;
                        Log("OBS system=traffic.retirement vehicle=%08X event=delete_refused refused=%d retry_ms=%llu",it->id,it->refused,wait);
                        // The game keeps refusing (engine_refused): it stays hidden and out of traffic, and stops counting against new traffic. A later load sweeps it.
                        if(it->refused>=kDeleteGiveUp){
                            Log("OBS system=traffic.retirement vehicle=%08X event=delete_given_up refused=%d",it->id,it->refused);
                            it=FinishRetirement(it,nullptr);continue; // Keep the saved retirement for the next cover pass.
                        }
                    }
                }
                ++it;
            }
        }
        void ShelveBadPair(int i,Pair& p,const RetirementOrder& order,const char* how){
            const char* why=order.why;
            FvoTrafficRef v{},d{};Info(p.vehicle,v);Info(p.driver,d);
            double nav=0,baked=0;const int known=g_api->ground?engineport::Forward<&FvoTrafficApi::ground>(g_api,"ground","generator",0u,v.x,v.y,v.z,&nav,&baked):0;
            const int sit=g_api->sitState?engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver):-1;
            SetLife(i,p,spawnflow::Life::Broken,why);SetLife(i,p,spawnflow::Life::Retiring,how);
            ApplyRetirement(order);
            Log("Traffic pool: spot %d: pair %08X / %08X disbanded %s: %s; deleted, the position is filled from a landing spot; model %08X, state %d, born at (%.0f, %.0f, %.1f), vehicle at (%.0f, %.0f, %.1f) model %d, driver at (%.0f, %.0f, %.1f) model %d, %.1f m apart, ground %.1f (known %d), sit %d, aboard %d",
                i,p.vehicle,p.driver,how,why,p.base,int(p.st),p.birth.x,p.birth.y,p.birth.z,v.x,v.y,v.z,v.has3D,d.x,d.y,d.z,d.has3D,
                std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z),known&2?baked:nav,known,sit,engineport::Forward<&FvoTrafficApi::vehicleOf>(g_api,"vehicleOf","generator",0u,p.driver)==p.vehicle?1:0);
            p=Pair{};p.st=St::Empty;SaveIds(i,p);g_initialRejected.erase(i);g_initialFails.erase(i);
            const auto alternatives=g_initialAlternatives.find(i);const int next=g_initialAttempt[i]+1;
            if(alternatives!=g_initialAlternatives.end()&&next<=int(alternatives->second.size()))g_initialAttempt[i]=next;else g_initialAttempt.erase(i);
        }
        // Never where it can be noticed (the FVO Traffic mandate: no unexplained disappearances; the user, 2026-10-30: "needs to be far and out of sight"): under a cover,
        // or each half with no model, or far AND out of sight. Far alone proves nothing: the Full LOD pass exists to keep far vehicles visible (53 held, out to 1378 m, in the
        // 11:57 run), and someone on the ground can have a clear view. A pair given up on goes at the first such moment; any other bad pair after 10 s of being bad.
        trustzone::PlayerPath g_path;
        ULONGLONG g_coverEndedAt=0,g_zoneReportAt=0;unsigned g_zoneLostChecks=0;double g_zoneRadius=0;
        // Card CP-1: the core's camera prediction (FVO_CameraAheadV1) and the user's switch.
        int (*g_cameraAhead)(double,double,double,double,double*)=nullptr;
        int (*g_cameraEnvelopeAhead)(double,double,double,double,double)=nullptr;
        cameravisibility::Reader g_cameraEvidence=nullptr;
        cameravisibility::ForecastReader g_lookForecast=nullptr;
        struct HealthAhead { unsigned id=0; int eta=0; };
        std::array<HealthAhead,cameravisibility::rules::healthBudget> g_healthAhead{};
        int (*g_healthSample)(unsigned,FvoPairHealthSampleV1*)=nullptr;  // CH-1: core FVO_PairHealthSampleV1; null: an older core
        void (*g_spotRequest)(unsigned)=nullptr;                          // CH-4: core FVO_SpotTestRequestV1; null: an older core
        unsigned (*g_playClock)()=nullptr;  // W1: core FVO_PlayClockV1 (0 = play time runs)
        // W1 (spawn one-place cards): every play-time clock here reads this one answer.
        bool PlayTime(){return g_playClock&&g_playClock()==0u;}
        void PrioritizeHealth(const Snap& snap,ULONGLONG now,int screen){
            g_healthAhead={};
            cameravisibility::Forecast look;
            if(screen!=1||!g_lookForecast||!PlayTime()||!g_lookForecast(&look))return;
            const auto projection=cameravisibility::Project(look,cameravisibility::rules::healthMs);
            for(const auto& v:snap){
                if(!v.id)continue;
                const double radius=std::hypot(v.radius>0?v.radius:8,v.halfHeight>0?v.halfHeight:6);
                const int eta=cameravisibility::EntryMs(projection,v.x,v.y,v.z,radius);
                if(eta<=0)continue;
                auto candidate=std::max_element(g_healthAhead.begin(),g_healthAhead.end(),[](const auto& a,const auto& b){
                    return a.id!=0&&(b.id==0||a.eta<b.eta);
                });
                if(candidate->id&&candidate->eta<=eta)continue;
                const auto pair=std::find_if(g_pairs.begin(),g_pairs.end(),[&](const Pair& p){
                    return p.vehicle==v.id&&!p.retired&&!p.fallRetirementPending&&!p.vehiclePending&&!p.reloadAt;
                });
                if(pair!=g_pairs.end()&&cameravisibility::AdvanceHealth(eta,now-pair->judgedAt,spawnflow::rules::judgeEveryMs))
                    *candidate={v.id,eta};
            }
        }
        bool g_predictOn=false;
        bool FellThroughWorld(const Pair& p){return p.fault&&std::strcmp(p.fault,"fell_through_world")==0;}
        spawnflow::Unseen ConfirmCameraUnseen(spawnflow::Unseen unseen,unsigned id,const FvoTrafficRef& ref,bool driver){
            if(!unseen.yes||!ref.has3D||!g_cameraEvidence)return unseen;
            const auto evidence=g_cameraEvidence(id,ref.baseId,ref.x,ref.y,ref.z,driver);
            if(const char* why=cameravisibility::HoldReason(evidence))return {false,why};
            return unseen;
        }
        const char* g_unseenRule="";  // which rule cleared (or held) the last UnseenPair answer, for the Retirement trace
        bool UnseenPair(const Pair& p){
            g_unseenRule="no_presentation";
            if(!g_api->presentation)return false;
            const int screen=engineport::Forward<&FvoTrafficApi::presentation>(g_api,"presentation","generator",0u);
            if(screen>=2){g_unseenRule="covered";return true;}
            g_unseenRule="not_in_play";
            if(screen!=1)return false;
            // A half with no model cannot be seen. So a broken pair denied the far pass unloads past the loaded cells and goes without anyone seeing it go.
            // Far is worked out per thing (trafficpair::NoticeFar). Out of sight: behind the camera's view, or the line of sight is blocked (a ray that hits nothing counts as seen,
            // and far terrain often has no physics to hit, so the rays alone hardly ever clear a far pair).
            // Range gates retirement independently of confirmed occlusion. CP-1 is a separate, explicitly enabled camera rule.
            const bool fell=FellThroughWorld(p);
            const bool predict=g_predictOn&&g_cameraAhead;
            const auto half=[&](unsigned id,const FvoTrafficRef& r,bool driver){
                if(!r.has3D)return spawnflow::HalfUnseen(false,false,predict,3);
                auto s=SightAt(r.x,r.y,r.z,r.baseId,driver);
                if(p.uncontrolledFall&&spawnflow::DeepFallCandidate(fell,g_playerAt.z-r.z,s.Notice())){
                    if(p.fallRetirementPending&&g_api->ground){
                        double terrain=0;
                        const int known=engineport::Forward<&FvoTrafficApi::ground>(g_api,"ground","generator",p.vehicle,r.x,r.y,r.z,nullptr,&terrain);
                        if(spawnflow::BelowBakedTerrain((known&2)!=0,terrain,r.z))
                            return spawnflow::Unseen{true,"deep_below_terrain"};
                    }
                    if(g_cameraEnvelopeAhead)
                        return ConfirmCameraUnseen(spawnflow::DeepFallUnseen(g_cameraEnvelopeAhead(r.x,r.y,r.z,spawnflow::rules::predictSeconds,spawnflow::rules::fallVisibilityRadius)),id,r,driver);
                }
                const bool beyondNotice=s.Far(fell);
                const bool standing=spawnflow::RetirementConcealed(beyondNotice,beyondNotice?s.Exposure():0);
                const int ahead=predict&&!standing?g_cameraAhead(r.x,r.y,r.z,spawnflow::rules::predictSeconds,nullptr):3;
                return ConfirmCameraUnseen(spawnflow::HalfUnseen(true,standing,predict,ahead),id,r,driver);};
            FvoTrafficRef v{},d{};
            g_unseenRule="no_reference";
            if(!p.vehicle&&Info(p.driver,d)){
                const auto driver=half(p.driver,d,true);g_unseenRule=driver.rule;return driver.yes;
            }
            if(!Info(p.vehicle,v))return false;
            // A vehicle whose driver reference is gone is judged on the vehicle alone.
            const bool driverGone=!p.driver||!Info(p.driver,d);
            const auto hv=half(p.vehicle,v,false);
            if(!hv.yes){g_unseenRule=hv.rule;return false;}
            if(driverGone){
                g_unseenRule=std::strcmp(hv.rule,"deep_below_terrain")==0?"deep_below_terrain_driver_gone":"vehicle_only_driver_gone";
                return true;
            }
            const auto hd=half(p.driver,d,true);
            g_unseenRule=hd.yes?(std::strcmp(hv.rule,"camera_prediction")==0||std::strcmp(hd.rule,"camera_prediction")==0?"camera_prediction":hv.rule):hd.rule;
            if(hd.yes&&(std::strcmp(hv.rule,"deep_below_terrain")==0||std::strcmp(hd.rule,"deep_below_terrain")==0))
                g_unseenRule="deep_below_terrain_pair_unseen";
            else if(hd.yes&&fell&&std::strcmp(g_unseenRule,"camera_prediction")!=0)
                g_unseenRule=std::strcmp(hv.rule,"deep_fall_out_of_view")==0||std::strcmp(hd.rule,"deep_fall_out_of_view")==0
                    ?"deep_fall_pair_out_of_view":"fell_through_world_pair_unseen";
            return hd.yes;
        }
        // R1 (spawn one-place cards, strip build 3): Retirement is the only remover. Every other step asks here with its reason; Retirement
        // applies its one rule (MayRemove: covered, or the pair unseen by the sight answer) and removes now, or keeps the request and asks
        // again every tick.
        struct RemoveRequest { unsigned vehicle = 0, driver = 0; const char* why = ""; ULONGLONG at = 0; };
        std::vector<RemoveRequest> g_removeQueue;
        struct ParkedGrant { unsigned base; ULONGLONG expires; };
        std::map<unsigned,ParkedGrant> g_parkedGrants;
        constexpr ULONGLONG kParkedRetryMs=2000;
        RetirementOrder DecideRetirement(Pair& pair,unsigned driver,const char* why,bool protectedIdentity=false){
            const bool owned=protectedIdentity||pair.life==spawnflow::Life::World||
                (pair.vehicle&&g_api->playerTook&&engineport::Forward<&FvoTrafficApi::playerTook>(g_api,"playerTook","generator",pair.vehicle,pair.vehicle));
            const bool cover=SightAt(0,0,0).Covered();
            bool unseen=false;
            const char* rule=owned?"world_owns_it":cover?"covered":"waiting_unseen";
            if(!owned&&!cover){
                if(pair.faultConcealed){
                    FvoTrafficRef v{};unseen=Info(pair.vehicle,v)&&Concealed({v.x,v.y,v.z,0},pair);
                    rule="concealed";
                    if(unseen&&g_cameraEvidence){
                        auto view=ConfirmCameraUnseen({true,rule},pair.vehicle,v,false);
                        FvoTrafficRef d{};
                        if(view.yes&&Info(driver,d))view=ConfirmCameraUnseen(view,driver,d,true);
                        unseen=view.yes;rule=view.rule;
                    }
                }else{
                    unseen=UnseenPair(pair);rule=g_unseenRule;
                }
            }
            spawnflow::PopulationRequest request{spawnflow::PopulationAction::Retirement};
            request.window.cover=cover;request.worldOwned=owned;request.unseen=unseen;
            return {pair.vehicle,driver,why,rule,spawnflow::ArbitratePopulation(request)};
        }
        RetirementOrder DecideRetirement(unsigned vehicle,unsigned driver,const char* why){
            if(vehicle)for(auto& pair:g_pairs)if(pair.vehicle==vehicle)return DecideRetirement(pair,driver,why);
            Pair pair;pair.vehicle=vehicle;pair.driver=driver;
            return DecideRetirement(pair,driver,why);
        }
        bool RetireRequest(unsigned vehicle,unsigned driver,const char* why){
            if(!vehicle&&!driver)return true;
            const auto order=DecideRetirement(vehicle,driver,why);
            if(order.decision.now){ApplyRetirement(order);return true;}
            if(!std::strcmp(order.decision.why,"world_owns_it"))return false;
            for(const auto& r:g_removeQueue)if(r.vehicle==vehicle&&r.driver==driver)return false;
            g_removeQueue.push_back({vehicle,driver,why,GetTickCount64()});CensusRetiringSave(vehicle,driver,why);  // SW-1: a waiting request survives a load
            Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=request_waiting reason=%s rule=%s",vehicle,driver,why,order.rule);
            return false;
        }
        bool PopulationTracks(unsigned vehicle,unsigned driver){
            return std::any_of(g_pairs.begin(),g_pairs.end(),[&](const Pair& p){return (vehicle&&p.vehicle==vehicle)||(driver&&p.driver==driver);});
        }
        void RetireRequestsTick(ULONGLONG now){
            for(auto it=g_removeQueue.begin();it!=g_removeQueue.end();){
                if(!std::strcmp(it->why,"driver_watch_orphan")&&PopulationTracks(it->vehicle,it->driver)){
                    CensusRetiringClear(it->vehicle,"rejoined_census",it->driver);it=g_removeQueue.erase(it);continue;
                }
                const auto order=DecideRetirement(it->vehicle,it->driver,it->why);
                if(order.decision.now){
                    Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=queue_resolved reason=%s rule=%s waited_ms=%llu",it->vehicle,it->driver,it->why,order.rule,now-it->at);
                    ApplyRetirement(order);it=g_removeQueue.erase(it);}
                else if(!std::strcmp(order.decision.why,"world_owns_it")){
                    CensusRetiringClear(it->vehicle,"world_owns_it",it->driver);it=g_removeQueue.erase(it);}
                else ++it;
            }
        }
        void JudgeSeat(int i,Pair& p,bool sat);
        // Step 7, Health: the only judge. Gathers the pair's facts (PairVerdict stays as the reading of the core's record and the pair's
        // state), asks spawnflow::Judge, and records the verdict. Never removes. true: the slot was let go (the world owns the pair now).
        // cover: under the loading screen a bad pair has waited long enough (it is replaced there, never seen).
        bool HealthJudge(int i,Pair& p,const Snap& snap,ULONGLONG now,bool cover=false,bool closing=false){  // CC-2: closing = the cover owner says the cover is closing now
            // EV-1: a handed-off pair whose driver id is gone is still judged (its vehicle would otherwise stand empty on the route).
            if(p.retired||p.fallRetirementPending||!p.vehicle||(!p.driver&&(p.proving||(!Driving(p)&&p.life!=spawnflow::Life::World))))return false;
            if(!cover&&now-p.judgedAt<spawnflow::rules::judgeEveryMs){
                const auto ahead=std::find_if(g_healthAhead.begin(),g_healthAhead.end(),[&](const auto& entry){return entry.id==p.vehicle;});
                if(ahead==g_healthAhead.end())return false;
                observations::Record(g_logFn,now,"traffic.health-lookahead",p.vehicle,1,
                    "event=advanced eta_ms=%d regular_due_ms=%llu",ahead->eta,spawnflow::rules::judgeEveryMs-(now-p.judgedAt));
            }
            p.judgedAt=now;
            if(p.fault&&!p.faultConcealed&&!p.floatingSince&&std::strcmp(p.fault,"fell_through_world")!=0)p.fault=nullptr;  // floating ended by itself
            const auto bad=PairVerdict(p,now);
            FvoTrafficRef v{},d{};const bool haveV=Info(p.vehicle,v),haveD=Info(p.driver,d);
            const bool aboard=Aboard(p.driver,p.vehicle);
            const int sit=g_api->sitState?engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver):-1;
            const double apart=haveV&&haveD?std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z):-1;
            const bool play=PlayTime();
            const bool body=HasBody(p.vehicle,snap);
            const bool models=haveV&&haveD&&v.has3D&&d.has3D&&v.usable&&d.usable&&!d.dead;
            const bool sitting=models&&SeatedPair(true,true,aboard,sit,apart);
            p.handoffWait.Observe(p.handoffPending&&models&&body,play&&!cover,now);
            // Relocation: while the core says this pair is being relocated nothing about distance, descent or the handoff clock is judged;
            // a receipt that says applied re-bases the fall baseline to the destination (once per serial); anything else is only recorded.
            FvoRelocationStatusV1 reloc{};const bool haveReloc=g_relocation&&g_relocation(p.vehicle,&reloc)==1;
            const bool relocating=haveReloc&&reloc.relocating!=0;
            if(haveReloc&&!relocating&&reloc.serial&&reloc.serial!=p.relocSerial){
                p.relocSerial=reloc.serial;
                if(reloc.result==1){p.relocToZ=reloc.to[2];p.relocBase=true;p.hHave=false;p.hPosAt=0;p.uncontrolledFall=false;}
                Log("OBS system=traffic.health vehicle=%08X event=rebased slot=%d reason=relocation_receipt serial=%llu result=%d fall_baseline_z=%.2f was_birth_z=%.2f",
                    p.vehicle,i,(unsigned long long)reloc.serial,reloc.result,reloc.to[2],p.birth.z);}
            const bool handoffExpired=!relocating&&p.handoffWait.elapsed>=spawnflow::rules::seatedNoTripMs;
            const bool bodyExpected=p.proving&&p.staged&&models&&
                std::hypot(v.x-g_playerAt.x,v.y-g_playerAt.y)<kDriver3DNear;
            p.provingBodyWait.Observe(bodyExpected&&!body,play&&!cover,now);
            const bool provingBodyFailed=p.provingBodyWait.elapsed>=spawnflow::rules::noBodyMs;
            if(provingBodyFailed&&!p.bodyJudged&&p.bornAt)
                Log("OBS system=traffic.health vehicle=%08X event=body_proof_failed slot=%d play_ms=%llu",p.vehicle,i,p.provingBodyWait.elapsed);
            const bool restoringSeat=p.st==St::Created&&p.seatAt!=0;
            p.provingSeatWait.Observe(spawnflow::ObserveSeatProof(p.proving,restoringSeat,models,body,sitting),play&&!cover,now);
            const bool seatProofExpired=p.provingSeatWait.elapsed>=trafficpair::kProveLimitMs;
            // A loaded seat only counts as a failed trial after an actual retry and a live chassis.
            if(!p.seatJudged&&models&&body&&p.tries>0&&!sitting&&play&&!cover){
                if(p.seatCheckAt)p.seatPlayMs+=std::min<ULONGLONG>(now-p.seatCheckAt,spawnflow::rules::maxObservedWaitMs);
                if(p.seatPlayMs>=spawnflow::rules::seatProveMs)p.seatTested=true;
                p.seatCheckAt=now;
            }else p.seatCheckAt=0;
            // Card HR1, the other stages. Body: the vehicle's model is loaded, so a body is due (fail: the no_physics_body fact). Drive: released
            // and has driven spawnhealth::rules::driveProof m. Release fails are reported where Health's Broken verdict is acted on.
            if(haveV&&haveD&&v.has3D&&d.has3D)p.bothLoadedOnce=true;
            if(!p.bodyJudged&&p.bornAt&&haveV&&v.has3D){
                if(body){p.bodyJudged=true;Evidence(i,p,spawnhealth::Stage::Body,true,"physics_body");}
                else if(provingBodyFailed||(p.fault&&!std::strcmp(p.fault,"no_physics_body"))){p.bodyJudged=true;Evidence(i,p,spawnhealth::Stage::Body,false,"no_physics_body");}}
            if(p.releaseJudged&&!p.driveJudged&&haveV&&spawnhealth::DriveProved(v.x-p.relX,v.y-p.relY)){
                p.driveJudged=true;Evidence(i,p,spawnhealth::Stage::Drive,true,"drove_from_release");}
            const int record=g_api->pairHealth?engineport::Forward<&FvoTrafficApi::pairHealth>(g_api,"pairHealth","generator",0u,p.vehicle):0;
            spawnflow::Facts f;
            f.dead=haveD&&d.dead!=0;
            f.outOnGround=Driving(p)&&haveD&&!aboard&&!d.dead&&record==int(trafficpair::PairHealth::Healthy);
            // Through the world: past the loaded cells nothing holds a car up (2026-11-07: FF10002F at z -6054, falling at 104 m/s).
            double navFloor=0,bakedFloor=0;const int floorKnown=haveV&&g_api->ground?engineport::Forward<&FvoTrafficApi::ground>(g_api,"ground","generator",0u,v.x,v.y,v.z,&navFloor,&bakedFloor):0;
            const double floorZ=floorKnown&2?bakedFloor:navFloor;
            const bool underFloor=floorKnown&&std::isfinite(floorZ)&&v.z<floorZ-spawnflow::rules::fellBelowFloor;
            const bool haveBase=p.relocBase||(p.birth.x||p.birth.y||p.birth.z);
            const bool deepBelowBirth=spawnflow::DeepBelowBase(haveBase,p.relocBase?p.relocToZ:p.birth.z,v.z);
            const auto sampleMs=p.hPosAt&&now>=p.hPosAt?now-p.hPosAt:0ull;
            const double drop=sampleMs?p.hz-v.z:0;
            const bool descending=haveV&&spawnflow::UncontrolledDescent(std::hypot(v.x-p.hx,v.y-p.hy),drop,sampleMs);
            if(!p.fault&&haveV){
                if(relocating)observations::Record(g_logFn,now,"traffic.health",p.vehicle,1u,"slot=%d event=held reason=relocating_no_fall_verdict z=%.2f",i,v.z);
                else if(spawnflow::JudgeFall(relocating,underFloor,deepBelowBirth,descending)){
                    p.fault="fell_through_world";p.faultConcealed=false;
                    Log("OBS system=traffic.cleanup vehicle=%08X event=fault slot=%d reason=fell_through_world z=%.2f birth_z=%.2f map_floor=%.2f floor_source=%s drop=%.2f sample_ms=%llu",
                        p.vehicle,i,v.z,p.birth.z,floorZ,floorKnown&2?"baked":floorKnown?"nav":"none",drop,sampleMs);
                }else if(underFloor)observations::Record(g_logFn,now,"traffic.cleanup.floor",p.vehicle,unsigned(floorKnown),
                    "slot=%d event=deferred reason=floor_discrepancy_without_descent z=%.2f map_floor=%.2f floor_source=%s drop=%.2f sample_ms=%llu",
                    i,v.z,floorZ,floorKnown&2?"baked":"nav",drop,sampleMs);
            }
            const bool falling=FellThroughWorld(p)&&descending;
            if(falling!=p.uncontrolledFall){
                p.uncontrolledFall=falling;
                Log("OBS system=traffic.health vehicle=%08X event=uncontrolled_fall slot=%d active=%d",p.vehicle,i,int(falling));}
            f.unrecoverable=bad==trafficpair::Bad::Unrecoverable&&!p.proving;f.gaveUp=bad==trafficpair::Bad::GaveUp||BirthFailed(p);
            f.fault=p.fault?p.fault:handoffExpired?"handoff_unproved":provingBodyFailed?"no_physics_body":
                !p.proving&&restoringSeat&&seatProofExpired?"restored_seat_unproven":nullptr;
            bool spotFailure=false;
            {static bool told=false;if(!told){told=true;Log("OBS system=traffic.health vehicle=00000000 event=spot_test_source available=%d reason=%s",int(g_spotTest!=nullptr),g_spotTest?"core_exports_FVO_SpotTestV1":"older_core_no_spot_test");}
             const int spot=g_spotTest?g_spotTest(p.vehicle):0;spotFailure=spot==3&&!p.proving;f.misplaced=spot==5&&!p.proving;}
            f.seated=sitting;
            // SR-2: how long it has sat while still being proved (waiting for its trip / release), play time only.
            if(p.proving&&f.seated){
                if(!p.seatedCheckAt){int on=-1,want=-1;const char* why=g_tripRefusal?g_tripRefusal(p.vehicle,&on,&want):nullptr;
                    Log("OBS system=traffic.health vehicle=%08X driver=%08X event=seated_waiting state=start slot=%d trip_refusal=%s on=%d want=%d",p.vehicle,p.driver,i,why?why:"none",on,want);}
                if(PlayTime()&&p.seatedCheckAt)p.seatedPlayMs+=std::min<ULONGLONG>(now-p.seatedCheckAt,1500);
                p.seatedCheckAt=now;
            }else{p.seatedPlayMs=0;p.seatedCheckAt=0;}
            f.seatedWaitMs=p.seatedPlayMs;
            f.proving=p.proving;f.proveExpired=seatProofExpired;
            f.coverClosing=closing;  // CC-2
            f.seatTriable=haveV&&haveD&&v.has3D&&d.has3D;  // CC-2a: a seat could be tried (both models loaded)
            f.bad=bad!=trafficpair::Bad::None&&!f.unrecoverable&&!f.gaveUp&&!f.dead;
            // EV-1 (2026-11-07, user: "empty vehicles need clearing ... cleanup should make sure it works on both"): after handoff, a traffic
            // vehicle with nobody of its pair aboard (driver dead, out, or gone) for emptyConfirmMs of play is broken, and Retirement removes
            // vehicle and driver together, out of sight. Someone else in the seat (player, companion, a job) is not traffic's to judge.
            // EV-2: a pair still being proved counts too, but only for a dead or gone driver (not aboard yet is the seat check's).
            if(haveV&&(Driving(p)||p.proving||p.life==spawnflow::Life::World||p.life==spawnflow::Life::Broken)){
                unsigned occupant=0;for(const auto& s:snap)if(s.id==p.vehicle){occupant=s.driver;break;}
                const char* cause=spawnflow::EmptyCause(haveD,haveD&&d.dead!=0,aboard,occupant,p.driver);
                if(p.proving&&cause&&!std::strcmp(cause,"driver_out"))cause=nullptr;  // EV-2
                if(cause){
                    if(!p.emptyCause)Log("OBS system=traffic.health vehicle=%08X driver=%08X event=empty_vehicle state=start slot=%d cause=%s life=%s occupant=%08X",p.vehicle,p.driver,i,cause,spawnflow::Name(p.life),occupant);
                    else if(std::strcmp(cause,p.emptyCause)!=0)Log("OBS system=traffic.health vehicle=%08X driver=%08X event=empty_vehicle state=cause_changed slot=%d cause=%s was=%s",p.vehicle,p.driver,i,cause,p.emptyCause);
                    if(PlayTime()&&p.emptyCheckAt)p.emptyPlayMs+=std::min<ULONGLONG>(now-p.emptyCheckAt,1500);
                    p.emptyCheckAt=now;p.emptyCause=cause;
                    f.empty=cause;f.emptyConfirmed=cover||p.emptyPlayMs>=spawnflow::rules::emptyConfirmMs;  // under cover nobody sees the swap: no wait
                }else if(p.emptyCause){
                    Log("OBS system=traffic.health vehicle=%08X driver=%08X event=empty_vehicle state=cleared slot=%d was=%s empty_ms=%llu occupant=%08X",p.vehicle,p.driver,i,p.emptyCause,p.emptyPlayMs,occupant);
                    p.emptyCause=nullptr;p.emptyPlayMs=0;p.emptyCheckAt=0;}
            }
            // CH-3/CH-5 (strip build 2): one continuous account replaces the self-report holds (PF-1), PairVerdict's own wait and the 20 s
            // no-physics clock. Facts in (the core's raw sample, what this step sees), the account decides; Judge reads its answer.
            {   healthscore::Input hi;hi.play=PlayTime();hi.driving=Driving(p)&&haveV&&haveD&&!f.dead&&!f.unrecoverable&&!f.gaveUp;
                FvoPairHealthSampleV1 hs{};hi.sample=g_healthSample&&g_healthSample(p.vehicle,&hs)==1;
                if(hi.sample){hi.askedKnown=true;hi.asked=hs.asked;hi.actual=hs.actual;hi.steps=hs.steps!=0;hi.lateral=hs.lateral;hi.halfWidth=hs.halfWidth;
                    hi.fieldShare=hs.fieldShare;hi.floorGap=hs.floorGap;hi.floorKnown=hs.floorKnown!=0;hi.onRail=hs.onRail!=0;hi.upZ=hs.upZ;}
                else hi.steps=!g_api->pilotLive||engineport::Forward<&FvoTrafficApi::pilotLive>(g_api,"pilotLive","generator",0u,p.vehicle)!=0;
                hi.aboard=aboard;hi.sitting=trafficpair::Sitting(sit);
                hi.driverOut=bad==trafficpair::Bad::DriverOut||bad==trafficpair::Bad::NoPilotInAir;hi.driverFar=bad==trafficpair::Bad::DriverFar;
                hi.fell=f.fault&&!std::strcmp(f.fault,"fell_through_world");hi.noBody=f.fault&&!std::strcmp(f.fault,"no_physics_body");
                hi.spotDead=spotFailure;hi.misplaced=f.misplaced;
                // Strip build 4 (M1): what the removed in-play fixes patched is Health's fact now.
                hi.noJob=hi.driving&&aboard&&!engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,p.vehicle);
                hi.driverNoModel=hi.driving&&haveV&&haveD&&v.has3D&&!d.has3D&&aboard&&std::hypot(v.x-g_playerAt.x,v.y-g_playerAt.y)<kDriver3DNear;
                if(haveV){if(p.hHave)hi.moved=std::hypot(std::hypot(v.x-p.hx,v.y-p.hy),v.z-p.hz);p.hx=v.x;p.hy=v.y;p.hz=v.z;p.hHave=true;p.hPosAt=now;}
                const auto was=p.health.state;const auto st=healthscore::Step(p.health,hi,now);
                f.spotFailed=p.health.decisive==healthscore::Kind::SpotDead;
                if(st!=was||now-p.accountLogAt>=5000&&st!=healthscore::State::Healthy){p.accountLogAt=now;
                    Log("OBS system=traffic.health vehicle=%08X driver=%08X event=account slot=%d state=%s score=%.1f top=%s decisive=%s sample=%d play=%d asked=%.1f actual=%.1f steps=%d lateral=%.1f field=%.2f floor_gap=%.1f up_z=%.2f",
                        p.vehicle,p.driver,i,healthscore::Name(st),p.health.score,*p.health.top?p.health.top:"-",healthscore::Name(p.health.decisive),int(hi.sample),int(hi.play),hi.asked,hi.actual,int(hi.steps),hi.lateral,hi.fieldShare,hi.floorGap,hi.upZ);}
                if(st!=healthscore::State::Healthy&&hi.driving){f.bad=true;f.badWhy=p.health.why;f.badWaitOver=st==healthscore::State::Broken;}
                else if(bad!=trafficpair::Bad::None&&!f.unrecoverable&&!f.gaveUp&&!f.dead)f.bad=false;  // the record's driver facts went into the account
                // CH-4: a doubtful pair is spot-tested again, any number of times (the core paces and hides it).
                if(st==healthscore::State::Doubtful&&g_spotRequest&&now-p.spotAskAt>=3000){p.spotAskAt=now;g_spotRequest(p.vehicle);
                    Log("OBS system=traffic.health vehicle=%08X event=spot_test_requested slot=%d score=%.1f top=%s",p.vehicle,i,p.health.score,*p.health.top?p.health.top:"-");}
            }
            auto j=spawnflow::Judge(f);
            // CH-5: Broken sticks. Only a break by the account's score may clear (after real driving, inside the account).
            // HB-1: Broken also sticks against the world's verdicts (driver dead or out): FF10005D 22:15:07 lost its Broken to driver_dead and was never
            // retired. Taking by the player is handled before Health and lets go of the pair there.
            if(p.brokenSticky&&j.verdict!=spawnflow::Verdict::Broken)j={spawnflow::Verdict::Broken,spawnflow::Life::Broken,p.verdictWhy};
            if(j.verdict==spawnflow::Verdict::Broken&&!(p.health.brokenByScore&&p.health.decisive==healthscore::Kind::None&&f.badWhy&&j.why==f.badWhy))p.brokenSticky=true;
            const bool changed=j.verdict!=p.verdict||std::strcmp(j.why,p.verdictWhy)!=0;
            const bool newlyBroken=j.verdict==spawnflow::Verdict::Broken&&p.verdict!=spawnflow::Verdict::Broken;
            if(newlyBroken)p.brokenAt=now;
            if(!p.seatJudged){
                const bool seatFailed=j.verdict==spawnflow::Verdict::Broken&&p.proving&&
                    (std::strcmp(j.why,"never_seated_in_time")==0||(f.gaveUp&&models&&body&&p.tries>=kSeatTries));
                const auto evidence=spawnhealth::JudgeSeatEvidence(sitting,seatFailed,p.seatTested);
                if(evidence!=spawnhealth::SeatResult::Pending)JudgeSeat(i,p,evidence==spawnhealth::SeatResult::Pass);
            }
            if(p.brokenSticky&&j.verdict==spawnflow::Verdict::Broken&&!p.brokenEvidenceJudged){p.brokenEvidenceJudged=true;BrokenEvidence(i,p,j.why);}
            if(changed&&j.verdict==spawnflow::Verdict::Broken&&!std::strcmp(j.why,"seated_no_trip")){int on=-1,want=-1;const char* why=g_tripRefusal?g_tripRefusal(p.vehicle,&on,&want):nullptr;
                Log("OBS system=traffic.health vehicle=%08X driver=%08X event=seated_no_trip slot=%d seated_ms=%llu trip_refusal=%s on=%d want=%d",p.vehicle,p.driver,i,p.seatedPlayMs,why?why:"none",on,want);}
            p.verdict=j.verdict;p.verdictWhy=j.why;
            const char* waiting=spawnflow::Waiting(haveV&&v.has3D,haveD&&d.has3D,HasBody(p.vehicle,snap),aboard,sit==3);
            if(changed)Log("OBS system=traffic.health vehicle=%08X driver=%08X event=verdict slot=%d verdict=%s reason=%s life=%s bad=%s record=%d waiting=%s aboard=%d sit=%d apart=%.1f",
                p.vehicle,p.driver,i,spawnflow::Name(j.verdict),j.why,spawnflow::Name(p.life),trafficpair::BadName(bad),record,waiting,int(aboard),sit,apart);
            else if(p.proving&&now-p.readyAt>=spawnflow::rules::readinessEveryMs){p.readyAt=now;
                Log("OBS system=traffic.health vehicle=%08X driver=%08X event=readiness slot=%d phase=%d staged=%d waiting=%s aboard=%d sit=%d apart=%.1f proving_ms=%llu",
                    p.vehicle,p.driver,i,int(p.st),int(p.staged),waiting,int(aboard),sit,apart,now-p.proveSince);}
            if(j.verdict==spawnflow::Verdict::World){
                SetLife(i,p,spawnflow::Life::World,j.why);
                // EV-1: a dead driver no longer lets go of the pair (that left the vehicle standing empty on the route for good). The pair
                // stays in its slot; once its vehicle is confirmed empty, Retirement removes vehicle and body together out of sight.
                if(f.dead&&!p.deadTold){p.deadTold=true;
                    Log("Traffic pool: spot %d: driver %08X of vehicle %08X is dead: the pair is kept; it is retired as an empty vehicle out of sight, then the position is filled again",i,p.driver,p.vehicle);}
            }
            else if(j.verdict==spawnflow::Verdict::Broken&&f.empty&&f.emptyConfirmed&&p.life==spawnflow::Life::World){
                SetLife(i,p,spawnflow::Life::Broken,"empty_vehicle");  // EV-1: out of the world's hands, so Retirement may remove it
                Log("OBS system=traffic.health vehicle=%08X driver=%08X event=empty_vehicle state=confirmed slot=%d cause=%s empty_ms=%llu cover=%d",p.vehicle,p.driver,i,f.empty,p.emptyPlayMs,int(cover));}
            else if(j.verdict==spawnflow::Verdict::Broken&&f.empty&&f.emptyConfirmed&&changed)
                Log("OBS system=traffic.health vehicle=%08X driver=%08X event=empty_vehicle state=confirmed slot=%d cause=%s empty_ms=%llu cover=%d",p.vehicle,p.driver,i,f.empty,p.emptyPlayMs,int(cover));
            else if(j.verdict==spawnflow::Verdict::Recovering)SetLife(i,p,spawnflow::Life::Recovering,j.why);
            else if(j.verdict==spawnflow::Verdict::Healthy&&p.life==spawnflow::Life::Recovering&&!p.handoffPending)SetLife(i,p,spawnflow::Life::Driving,"recovered");
            return false;
        }
        // Step 9, Retirement: the only remover of a pair Health called broken. Only under cover or out of sight. true: the slot is empty.
        bool RetireTick(int i,Pair& p,ULONGLONG now){
            if(!p.vehicle||p.verdict!=spawnflow::Verdict::Broken)return false;
            if(p.uncontrolledFall&&!p.fallRetirementPending){
                const bool taken=g_api->playerTook&&engineport::Forward<&FvoTrafficApi::playerTook>(g_api,"playerTook","generator",p.vehicle,p.vehicle);
                if(spawnflow::QueueFallRetirement(p.verdict,p.life,p.uncontrolledFall,p.fallRetirementPending,taken)){
                    p.fallRetirementPending=true;
                    CensusRetiringSave(p.vehicle,p.driver,"fell_through_world");
                    const bool hasJob=g_api->endJob&&engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",p.vehicle,p.vehicle);
                    // PilotStop's existing world-loss reason bypasses the normal landing attempt.
                    if(hasJob)engineport::Forward<&FvoTrafficApi::endJob>(g_api,"endJob","generator",p.vehicle,p.vehicle,"traffic: fell through world");
                    Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=fall_queued slot=%d end_job_requested=%d",
                        p.vehicle,p.driver,i,int(hasJob));
                }
            }
            const char* why=p.fallRetirementPending?"fell_through_world":p.verdictWhy;
            const auto order=DecideRetirement(p,p.driver,why);
            const auto may=order.decision;const char* rule=order.rule;
            if(!may.now){observations::Record(g_logFn,now,"traffic.zone.fault",p.vehicle,std::uint64_t(int(p.verdict)),"driver=%08X event=%s slot=%d reason=%s rule=%s prediction=%d",p.driver,may.why,i,p.verdictWhy,rule,int(g_predictOn));return false;}
            Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=retired slot=%d reason=%s how=%s rule=%s prediction=%d waited_ms=%llu",p.vehicle,p.driver,i,why,may.why,rule,int(g_predictOn),p.brokenAt?now-p.brokenAt:0ull);
            if(p.proving){
                // (HR1: the seat verdict is Health's own tick now; Retirement only reads it.)
                SetLife(i,p,spawnflow::Life::Broken,why);SetLife(i,p,spawnflow::Life::Retiring,may.why);
                ApplyRetirement(order);
                p=Pair{};p.st=St::Empty;p.at=now+5000;SaveIds(i,p);g_initialRejected.insert(i);
                return true;
            }
            ShelveBadPair(i,p,order,!std::strcmp(rule,"covered")?"under cover":"where it cannot be noticed");
            return true;
        }
        bool RetireSurplus(int i,Pair& p,const Snap& snap,ULONGLONG now){
            if(!p.retired||p.fallRetirementPending)return false;
            if(!p.vehicle||p.life==spawnflow::Life::World)return true;
            if(std::any_of(snap.begin(),snap.end(),[&](const auto& v){return v.id==p.vehicle&&v.who==kFvoWhoPlayer;}))return true;
            if(engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",p.vehicle,p.vehicle)||
               (g_api->tripActive&&engineport::Forward<&FvoTrafficApi::tripActive>(g_api,"tripActive","generator",p.vehicle,p.vehicle)))return false;

            const unsigned driver=(p.proving||Aboard(p.driver,p.vehicle))?p.driver:0;
            const auto order=DecideRetirement(p.vehicle,driver,"fleet_target_reduced");
            if(!order.decision.now){
                observations::Record(g_logFn,now,"traffic.surplus",p.vehicle,0,
                    "driver=%08X event=waiting slot=%d reason=fleet_target_reduced rule=%s",p.driver,i,order.rule);
                return true;
            }
            const auto key=p.storedKey.empty()?PairKey(i):p.storedKey;
            SetLife(i,p,spawnflow::Life::Retiring,"fleet_target_reduced");
            ApplyRetirement(order);
            WriteRegistry(Wide(g_world).c_str(),key.c_str(),nullptr);
            p=Pair{};p.retired=true;p.st=St::Empty;
            return true;
        }

		// Traffic NPCs: pool vehicles and their drivers are a fixed pool, made once and reused: both persistent (moved into the
		// world's persistent cell, as named NPCs are), so distance never unloads them. The drivers stay ordinary mortal citizens
		// (the user: "not be essential and unable to be killed").
		void MakeTrafficPair(int i, const Pair& p)
		{
			const bool v = engineport::Forward<&FvoTrafficApi::makeTraffic>(g_api,"makeTraffic","generator",0u,p.vehicle, 0) != 0, d = engineport::Forward<&FvoTrafficApi::makeTraffic>(g_api,"makeTraffic","generator",0u,p.driver, 1) != 0;
			Log("Traffic pool: spot %d: vehicle %08X %s, driver %08X %s", i, p.vehicle, v ? "persistent" : "NOT persistent",
				p.driver, d ? "persistent" : "NOT persistent");
		}

        void Fail(int i, Pair& p, const char* why){
            Log("Traffic pool: spot %d (%s): pair %08X/%08X parked/deferred: %s (no visible deletion or replacement)",i,g_world.c_str(),p.vehicle,p.driver,why);
            p.st=St::Failed;
        }
        bool InitialAdmissionsPending(){
            const int n=std::min(int(g_initialScene.size()),int(g_pairs.size()));
            for(int i=0;i<n;++i)if(SlotFree(g_pairs[i])&&!g_pairs[i].vehicle&&!g_pairs[i].retired&&!g_modelUnavailable.count(i)&&!g_passRefused.count(i))return true;
            return false;
        }
        // CC-4 Window (2026-11-07, user: "the X panel can be extended to do it whilst it's loading"; replaces XP-1, which read the seat
        // itself): the X panel also stays up while Health has a pair still being proved without a seated driver, or a broken pair
        // Retirement has not removed yet. Reads Health's output only (verdict and its reason). Bounded by kResetLimitMs.
        int UnprovenUnderPanel(const char** first=nullptr){
            int c=0;const int n=std::min(int(g_initialScene.size()),int(g_pairs.size()));
            for(int i=0;i<n;++i){const auto& p=g_pairs[std::size_t(i)];
                if(p.retired||!p.vehicle)continue;
                const bool broken=p.verdict==spawnflow::Verdict::Broken&&p.life!=spawnflow::Life::World;
                const bool unseated=p.verdict==spawnflow::Verdict::Proving&&std::strcmp(p.verdictWhy,"seated_waiting")!=0;
                if(broken||unseated){if(first&&!c)*first=broken?"broken_not_removed":"proving_not_seated";++c;}}
            return c;
        }
        // CC-2 + CC-3: the cover is closing (kind 3: the cover owner's report on the loading screen's close frame; kind 2: the X panel
        // about to close). Health judges every pair with the closing fact (step 7, the one judge), then Retirement removes what it
        // called broken under its own rule; the cover still holds on this frame, so that rule is "covered". Returns pairs removed.
        int CoverClosing(const Snap& snap,ULONGLONG now,int kind){
            int judged=0,broken=0,removed=0;
            const int total=std::min(int(g_initialScene.size()),int(g_pairs.size()));
            for(int i=0;i<total;++i){auto& p=g_pairs[std::size_t(i)];
                if(p.retired||!p.vehicle)continue;
                if(p.proving||p.vehiclePending||p.driverPending||
                   (g_fleetState&&g_fleetState(p.vehicle)>=2))continue;
                ++judged;
                if(HealthJudge(i,p,snap,now,true,true))continue;
                if(p.verdict==spawnflow::Verdict::Broken)++broken;
                if(RetireTick(i,p,now))++removed;
            }
            Log("OBS system=traffic.health vehicle=00000000 event=cover_closing kind=%d judged=%d broken=%d removed=%d cover=%d",kind,judged,broken,removed,
                engineport::Forward<&FvoTrafficApi::presentation>(g_api,"presentation","generator",0u));
            return removed;
        }

		// Randomized traffic models (2026-10-29, the user: "randomize from vehicles that exist in game", "general not from a specific
		// vehicle list"): the pool is whatever the load order holds, every distinct model with the game's vehicle keyword (core's
		// vehicleTypes), never a list of ours. Each route position gets a seeded pick (world + slot), so the same position shows the
		// same model every visit and after every load. trafficpool.ini [types] skip = FORMID, FORMID leaves models out.
		std::vector<unsigned> g_typePool;std::string g_typeWorld;
		// Vehicle rules (2026-10-31, the user: models left out still appeared after X): the models each position may not hold.
		std::vector<std::vector<unsigned>> g_slotSkip;
		unsigned (*g_idAdapter)(unsigned)=nullptr;  // stored form ids -> this load order (trafficidmap.ini)
        std::map<int,std::set<unsigned>> g_fitRejected;ModelGate g_modelGate=nullptr;SiteGate g_siteGate=nullptr;
		bool SlotSkips(int slot,unsigned id){
			if(const auto fit=g_fitRejected.find(slot);fit!=g_fitRejected.end()&&fit->second.count(id))return true;
            if(slot<0||slot>=int(g_slotSkip.size()))return false;
			const auto& list=g_slotSkip[std::size_t(slot)];
			return std::find(list.begin(),list.end(),id)!=list.end();
		}
		std::set<unsigned> SkippedTypes(){
			std::set<unsigned> out;
			for(const wchar_t* key:{L"skip",L"auto_skip"}){wchar_t buf[2048]{};
				GetPrivateProfileStringW(L"types",key,L"",buf,2048,Ini().c_str());
				for(const wchar_t* c=buf;*c;){wchar_t* end=nullptr;const unsigned long v=std::wcstoul(c,&end,16);if(end==c){++c;continue;}if(v)out.insert(g_idAdapter?g_idAdapter(unsigned(v)):unsigned(v));c=end;}}
			return out;
		}
		// Models whose drivers do not sit (2026-10-30: some mod vehicles have no seat FVO's instant seat can use). Spawn flow step 7, Health,
		// owns the verdict: a model whose drivers failed to sit twice and never sat is rejected in Health's model record (logged, reversible:
		// a reset or a load starts the evidence again). Demand reads the record (VehicleBase). It no longer writes trafficpool.ini [types]
		// auto_skip silently (removed 2026-11-07); entries an older build wrote there are still read as earlier verdicts (SkippedTypes).
		// Card HR1: the seat stage goes into Health's one store (model and spot, kept across loads by HR2); the rule and the blame live there.
		void JudgeSeat(int i,Pair& p,bool sat){
			if(p.seatJudged||!p.base)return;
			p.seatJudged=true;
			Log("OBS system=traffic.health vehicle=%08X driver=%08X event=seat_judged model=%08X result=%s play_ms=%llu",p.vehicle,p.driver,p.base,sat?"sat":"never_sat",p.seatPlayMs);
			Evidence(i,p,spawnhealth::Stage::Seat,sat,sat?"sat":"never_sat");
		}
		const std::vector<unsigned>& TypePool(){
			if(g_typeWorld==g_world&&!g_typePool.empty())return g_typePool;
			g_typePool.clear();
			if(g_api->vehicleTypes){
				unsigned ids[128]{};const int n=engineport::Forward<&FvoTrafficApi::vehicleTypes>(g_api,"vehicleTypes","generator",0u,ids,128);const auto skip=SkippedTypes();
				for(int k=0;k<n;++k)if(!skip.count(ids[k])&&engineport::Forward<&FvoTrafficApi::formExists>(g_api,"formExists","generator",0u,ids[k]))g_typePool.push_back(ids[k]);
				if(!g_typePool.empty()){
					std::string list;for(const unsigned id:g_typePool){char b[16];std::snprintf(b,sizeof b," %08X",id);list+=b;}
					Log("Traffic models: %zu vehicle model(s) in this load order are used for traffic (%zu skipped):%s",g_typePool.size(),skip.size(),list.c_str());
					g_typeWorld=g_world;
				}
			}
			return g_typePool;
		}
		// As much variety as possible (2026-10-29, the user): not an independent roll per position but a seeded shuffle of the whole
		// pool for this world, dealt out across positions in turn and reshuffled for each round. Every model gets used about equally
		// and neighbouring positions (interleaved across routes) never repeat within a round. Still fixed per position.
		unsigned VehicleBase(int slot,int deal=-1)
		{
			const auto& pool=TypePool();
			if(pool.empty())return engineport::Forward<&FvoTrafficApi::formExists>(g_api,"formExists","generator",0u,kRev8)&&!SlotSkips(slot,kRev8)&&!g_health.ModelRejected(kRev8)?kRev8:0;  // no pool yet (or none in the load order): the base game's REV-8
            const auto position=std::size_t(std::max(deal<0?slot:deal,0));
			const std::size_t n=pool.size(),round=position/n,at=position%n;
			std::uint32_t seed=2166136261u;for(const char c:g_world)seed=(seed^std::uint8_t(c))*16777619u;
			for(const unsigned id:pool)seed=(seed^id)*16777619u;
			seed^=std::uint32_t(round+1)*2654435761u;
			std::vector<unsigned> order(pool.begin(),pool.end());
			for(std::size_t k=n;k>1;--k){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;std::swap(order[k-1],order[seed%k]);}
			// Left out here: the next model in this round's order, so a position with no rules keeps the one it always had. None allowed: no vehicle here.
			// Spawn flow step 3, Demand: your Types first (SlotSkips: route/lane rules, [types] skip), Health's model verdicts second.
			const char* why="";const unsigned id=spawnflow::Model(order,at,[&](unsigned m){return SlotSkips(slot,m);},g_health,&why);
			if(!id){static std::set<int> told;if(told.insert(slot).second)Log("OBS system=traffic.demand vehicle=00000000 event=no_model slot=%d reason=%s",slot,why);}
			return id;
		}


        // The model to make for a slot: `wanted` unless the profile's rules for this slot or Health's rejection forbid it; then the next model of the
        // pool that the same rules allow (VehicleBase's own rules: spawnflow::Model). 0: none allowed. why: set when the model had to change.
        unsigned FallbackModelFor(int slot,unsigned wanted,const char** why){
            *why="";
            std::vector<unsigned> order=TypePool();
            if(order.empty()&&engineport::Forward<&FvoTrafficApi::formExists>(g_api,"formExists","generator",0u,kRev8))order.push_back(kRev8);
            const auto at=wanted?std::find(order.begin(),order.end(),wanted):order.end();
            if(wanted){
                if(at==order.end())*why="model_not_in_the_profile_pool";
                else if(SlotSkips(slot,wanted))*why="profile_rule_or_earlier_fit_refusal_for_this_slot";
                else if(g_health.ModelRejected(wanted)&&!g_health.ModelTrial(wanted))*why="health_rejected_model";
                else return wanted;
            }
            if(at!=order.end())std::rotate(order.begin(),at+1,order.end());
            const char* w="";
            return spawnflow::Model(order,0,[&](unsigned m){return m==wanted||SlotSkips(slot,m);},g_health,&w);
        }

        bool MutationPending(unsigned id,const char* operation){return g_api->mutationPending&&engineport::Forward<&FvoTrafficApi::mutationPending>(g_api,"mutationPending","generator",id,id,operation);}
        FvoBirthWindow BirthWindow(int screen,const spawnflow::Window& window,FvoBirthSource source) {
            if(!window.open||(source==FvoBirthSource::DriverRepair&&!window.cover))return FvoBirthWindow::Closed;
            if(screen==1&&source==FvoBirthSource::Fleet&&g_roadCheck&&g_roadBirth)
                return FvoBirthWindow::FleetShortfall;
            return screen==3?FvoBirthWindow::Loading:screen==2?FvoBirthWindow::Panel:FvoBirthWindow::Diagnostic;
        }
        std::uint64_t SpeakBirth(spawnflow::BirthBudget& budget,const spawnflow::PopulationRequest& request,FvoBirthSource source,int slot,unsigned base,unsigned vehicle,const FvoPoolSpot& at) {
            const auto decision=spawnflow::ArbitratePopulation(request,&budget);
            if(!decision.now)return 0;
            static std::uint64_t serial=0;
            const auto grant=++serial;
            Log("OBS system=traffic.birth event=grant speaker=arbiter grant=%llu source=%s slot=%d base=%08X vehicle=%08X pos=%.2f,%.2f,%.2f window=%s",
                static_cast<unsigned long long>(grant),BirthSourceName(source),slot,base,vehicle,at.x,at.y,at.z,request.window.why);
            return grant;
        }
        struct BirthReply { unsigned id=0; bool pending=false, ambiguous=false; };
        BirthReply ListenBirth(std::uint64_t grant,FvoBirthSource source,FvoBirthWindow window,int slot,unsigned base,unsigned vehicle,const FvoPoolSpot& at,bool& issued,
            const FvoRoadProofV1* road=nullptr) {
            if(road&&!issued&&!roadclearance::Fresh(road->at,GetTickCount64(),roadclearance::ProofMs(window==FvoBirthWindow::Loading))){
                Log("OBS system=traffic.birth event=deferred reason=unissued_road_proof_expired slot=%d grant=%llu",slot,static_cast<unsigned long long>(grant));
                return {};
            }
            if(!issued&&window==FvoBirthWindow::Closed)return {0,true};
            const FvoTrafficBirthV1 request{source,window,!issued,grant,slot,base,vehicle,at.x,at.y,at.z,at.heading};
            FvoBirthState state{};
            const auto id=road?g_roadBirth(&request,road,&state):g_birthListener(&request,&state);
            if(state==FvoBirthState::Completed)return {id,false};
            if(state==FvoBirthState::Pending){issued=true;return {0,true};}
            if(issued)Log("OBS system=traffic.birth event=ticket_lost grant=%llu source=%s slot=%d base=%08X vehicle=%08X",static_cast<unsigned long long>(grant),BirthSourceName(source),slot,base,vehicle);
            return {0,!issued,issued};
        }
        int FleetCount(){
            int n=0;
            for(const auto& p:g_pairs)if(p.vehicle||p.vehiclePending)++n;
            return n;
        }
        // A request returning zero is not a failed birth while its owned command
        // or completion is outstanding. Poll the exact same request before any
        // health check, alternate model selection, or retry counter.
        bool FinishBirth(int i,Pair& p,ULONGLONG now,int screen,const spawnflow::Window& window){
            if(p.vehiclePending){
                if(!p.vehicleIssued&&p.retired){
                    p.vehiclePending=p.driverPending=false;
                    SetLife(i,p,spawnflow::Life::None,"profile_removed_unissued_birth");
                    p=Pair{};p.st=St::Empty;SaveIds(i,p);
                    return false;
                }
                const auto reply=ListenBirth(p.birthGrant,FvoBirthSource::Fleet,BirthWindow(screen,window,FvoBirthSource::Fleet),i,p.base,0,p.birth,p.vehicleIssued,
                    p.roadProof.epoch?&p.roadProof:nullptr);
                if(reply.pending)return false;
                p.vehicle=reply.id;
                p.vehiclePending=false;
                // Birth failed (the core or the engine refused the create): the Census records it and the Ledger counts it (card C2).
                if(!p.vehicle){SetLife(i,p,spawnflow::Life::None,"create_refused");g_tally.Refuse("create_refused");g_madeThisWindow.erase(i);
                    if(!window.cover){FvoBirthRefusalV1 r;const bool known=CoreRefusal(p.birthGrant,i,r);LogRefusal("completion",i,p.base,p.birthGrant,known?&r:nullptr);}
                    p=Pair{};p.st=St::Empty;p.at=now+5000;SaveIds(i,p);g_initialRejected.insert(i);return false;}
                Log("OBS system=traffic.birth event=completed slot=%d vehicle=%08X driver_pending=%d",i,p.vehicle,int(!p.retired));
                Log("OBS system=traffic.live vehicle=%08X event=chassis_created slot=%d create_ms=%llu screen=%d",p.vehicle,i,p.bornAt?now-p.bornAt:0ull,screen);
                p.driverPending=!p.retired;SaveIds(i,p);CensusPublish(i,p);
            }
            if(p.driverPending){
                if(!p.driverIssued&&(p.retired||
                    (g_api->playerTook&&engineport::Forward<&FvoTrafficApi::playerTook>(g_api,"playerTook","generator",p.vehicle,p.vehicle)))){
                    p.driverPending=false;
                    return true;
                }
                const auto driverWindow=BirthWindow(screen,window,p.driverSource);
                if(screen&&p.vehicle&&!p.driverIssued&&driverWindow==FvoBirthWindow::Closed){
                    p.driverPending=false;p.st=St::Failed;p.at=now;
                    Log("OBS system=traffic.birth vehicle=%08X event=partial_pair slot=%d reason=cover_closed_before_driver",p.vehicle,i);
                    SaveIds(i,p);MakeTrafficPair(i,p);
                    return true;
                }
                const auto reply=ListenBirth(p.birthGrant,p.driverSource,driverWindow,i,kDriverBase,p.vehicle,p.driverAt,p.driverIssued);
                if(reply.pending)return false;
                p.driver=reply.id;
                p.driverPending=false;p.st=p.driver?St::Created:St::Failed;p.at=now;
                Log("OBS system=traffic.live vehicle=%08X event=driver_created slot=%d driver=%08X create_ms=%llu screen=%d",p.vehicle,i,p.driver,p.bornAt?now-p.bornAt:0ull,screen);
                SaveIds(i,p);MakeTrafficPair(i,p);
            }
            return true;
        }

		void Step(int i, Pair& p, const FvoPoolSpot& s, const Pos& pp, const Snap& snap, ULONGLONG now,
            int screen,const spawnflow::Window& window,spawnflow::BirthBudget& budget)
		{
			g_clock=now;
            p.handoffEligible=false;
            if(p.staged&&p.vehicle&&!p.takeoverQueued&&g_api->playerTook&&g_api->playerTook(p.vehicle)){
                p.takeoverQueued=true;g_takenRelease.insert(p.vehicle);
                Log("OBS system=traffic.handoff vehicle=%08X event=release_requested slot=%d reason=taken_by_the_player",p.vehicle,i);
            }
			if(!FinishBirth(i,p,now,screen,window)){
                observations::Record(g_logFn,now,"traffic.mutation-wait",p.vehicle,(std::uint64_t(p.vehiclePending)<<8)|(std::uint64_t(p.driverPending)<<9),
                    "slot=%d driver=%08X base=%08X vehicle_pending=%d driver_pending=%d",i,p.driver,p.base,p.vehiclePending,p.driverPending);return;
            }
            observations::Record(g_logFn,now,"traffic.slot",p.vehicle,unsigned(p.st)|(std::uint64_t(p.staged)<<8)|(std::uint64_t(p.proving)<<9)|(std::uint64_t(p.retired)<<10),
                "slot=%d driver=%08X base=%08X phase=%d staged=%d proving=%d reserved=%d retired=%d seat_tries=%d birth=%.2f,%.2f,%.2f target_slots=%zu",i,p.driver,p.base,int(p.st),p.staged,p.proving,p.reserved,p.retired,p.tries,p.birth.x,p.birth.y,p.birth.z,g_pairs.size());
			if (p.reloadAt) {  // the driver's 3D reload: enabled again after the gap, then seated by Created
				if (now - p.reloadAt < kReloadGapMs) return;
				p.reloadAt = 0;
				if (Exists(p.driver)) engineport::Forward<&FvoTrafficApi::show>(g_api,"show","generator",0u,p.driver, 1);
				p.at = now; p.tries = 0; p.seatAt = 0;
				Log("Traffic pool: spot %d: driver %08X enabled again (3D reload %d)", i, p.driver, p.reloads);
				return;
			}
			g_playerAt=pp;
			// The player took it: it is theirs, and the position is filled again. Never hidden or moved, and its driver is a person in the world now.
			if(p.vehicle&&g_api->playerTook&&engineport::Forward<&FvoTrafficApi::playerTook>(g_api,"playerTook","generator",0u,p.vehicle)){
				Log("Traffic pool: spot %d: the player took vehicle %08X: traffic lets go of it (driver %08X), the position is filled again",i,p.vehicle,p.driver);
                if(p.fallRetirementPending)CensusRetiringClear(p.vehicle,"taken_by_the_player",p.driver);
                // A receipt can supply a chassis after the first ownership check.
                if(p.staged&&!p.takeoverQueued)g_takenRelease.insert(p.vehicle);
                g_fleetRelease(p.vehicle);
				SetLife(i,p,spawnflow::Life::World,"taken_by_the_player");  // Health: the world's now; never on the retire list
				p=Pair{};p.st=St::Empty;SaveIds(i,p);g_initialRejected.erase(i);g_initialAttempt.erase(i);g_initialFails.erase(i);
				return;
			}
            if(p.handoffForeign){
                if(engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,p.vehicle))return;
                p.handoffForeign=false;p.st=St::Created;p.at=now;
                SetLife(i,p,spawnflow::Life::Proving,"previous_job_released");
            }
			// A managed rail may legitimately lack ground physics or a fresh pilot step. The
            // core owns mode transitions, so those facts cannot trigger legacy road recovery.
            if(p.vehicle&&!p.retired){
                const int mode=g_fleetState(p.vehicle);
                if(mode==2||mode==3){
                    const bool activeTrip=engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",p.vehicle,p.vehicle)!=0;
                    FvoTrafficRef driver{},vehicle{};
                    const bool dead=Info(p.driver,driver)&&driver.dead;
                    const bool dismounted=driver.has3D&&Info(p.vehicle,vehicle)&&vehicle.has3D&&!Aboard(p.driver,p.vehicle);
                    if(activeTrip&&!dead&&!dismounted)return;
                    // A completed/abandoned journey releases the controller before this
                    // same physical pair is offered another existing route.
                    g_fleetRelease(p.vehicle);
                }
            }
			// Step 7 then step 9: every pair judged once, and only Retirement removes.
			if(RetireSurplus(i,p,snap,now)||HealthJudge(i,p,snap,now)||RetireTick(i,p,now))return;
            // Handoff does not offer another trip after Retirement commits a terminal fall.
            if(p.fallRetirementPending)return;
			// Over the player nothing holds a new driver up until they sit: a made driver starts on the ground below, and one with a model falls. Kept at the vehicle until seated, every update.
			if(p.proving&&p.staged&&p.driver&&g_api->holdDriver)engineport::Forward<&FvoTrafficApi::holdDriver>(g_api,"holdDriver","generator",0u,p.driver,p.vehicle);
			if(p.proving&&p.vehicle){
				// Time under the map does not count: the game is paused there and nothing can seat.
				const int cover=g_api->presentation?engineport::Forward<&FvoTrafficApi::presentation>(g_api,"presentation","generator",0u):1;
				if(p.proveTick&&(cover==2||now-p.proveTick>1000))p.proveSince+=now-p.proveTick;
				p.proveTick=now;
				// Remote pairs wait frozen for their models, rather than failing a seating test the engine cannot run yet.
				FvoTrafficRef pendingVehicle{},pendingDriver{};
				if(p.st!=St::Failed&&Info(p.vehicle,pendingVehicle)&&Info(p.driver,pendingDriver)&&
				   pendingVehicle.usable&&pendingDriver.usable&&!pendingDriver.dead&&(!pendingVehicle.has3D||!pendingDriver.has3D)){
					// The vehicle is here and its driver cannot be yet: a driver's model loads only near the player, so far pairs waited frozen for minutes (2026-10-31: median 80-150 s, 35-57 pairs never moved).
					// It flies now on a pre-armed pilot, as the fly-on rule already lets a pair do past that reach. Its seat is checked when the driver's model comes.
					if(pendingVehicle.has3D&&!pendingDriver.has3D&&p.staged&&g_api->startArmed&&g_trip&&Aboard(p.driver,p.vehicle)&&HasBody(p.vehicle,snap)&&
					   std::hypot(pendingVehicle.x-pp.x,pendingVehicle.y-pp.y)>=trafficpair::kDriverModelReach&&now-p.remoteTryAt>=3000){
						p.remoteTryAt=now;
						FvoTripClaim claim{};
						const bool claimed=g_trip(i,p.vehicle,pendingVehicle.x,pendingVehicle.y,pendingVehicle.z,&claim)&&claim.step;
						const bool armed=claimed&&engineport::Forward<&FvoTrafficApi::startArmed>(g_api,"startArmed","generator",0u,p.driver,p.vehicle,&claim);
						Log("OBS system=traffic.remote_start vehicle=%08X driver=%08X event=%s slot=%d distance=%.0f waited_ms=%llu",p.vehicle,p.driver,armed?"started":!claimed?"no_trip":"refused",i,std::hypot(pendingVehicle.x-pp.x,pendingVehicle.y-pp.y),now-p.bornAt);
						if(armed){p.st=St::Started;p.watchAt=now;p.proving=false;
                        p.handoff={};p.handoffWait={};p.handoffPending=true;
                        return;}
					}
					p.proveSince=now;p.seatedSince=0;return;
				}
				// Sitting is what the limit waits for (2026-10-31 18:59 run: FF1003A3 deleted 0.3 s after it sat, inside the hold; FF1003C9 seated 26 s, waiting for route room).
				// A seat that breaks later gets the whole limit to come back.
				if(p.st!=St::Failed&&pendingVehicle.usable&&pendingDriver.usable&&!pendingDriver.dead&&
				   SeatedPair(pendingVehicle.has3D!=0,pendingDriver.has3D!=0,Aboard(p.driver,p.vehicle),g_api->sitState?engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver):-1,
				              std::hypot(std::hypot(pendingVehicle.x-pendingDriver.x,pendingVehicle.y-pendingDriver.y),pendingVehicle.z-pendingDriver.z)))p.proveSince=now;
			}
			PrecheckPair(i,p,pp,now);
			switch (p.st) {
			case St::Unknown: {
				auto alive = [&](const Pair& q) {
					bool v3 = false, d3 = false;
					// A remembered id can belong to something else in another save: the vehicle must still have the base it was made
					// with (entries from before 2026-10-29 kept no base: then a REV-8 / Creations base, as those pairs were made).
					const unsigned base = BaseId(q.vehicle);
					const bool sameKind=retiringrecord::RecoveryBaseMatches(q.base,base,kRev8);
					return Exists(q.vehicle) && Exists(q.driver) && sameKind && Usable(q.vehicle, v3) && Usable(q.driver, d3) &&
						LooksLikeActor(q.driver) && !IsDead(q.driver);
				};
				if (alive(p)) {
                    if(p.staged&&g_api->holdBirth)engineport::Forward<&FvoTrafficApi::holdBirth>(g_api,"holdBirth","generator",0u,p.vehicle);
                    // Saved while flying (2026-10-29 22:50 run: saved traffic dropped the moment physics started): frozen first, then
                    // the pilot recovery gives it a fresh pilot; one on the ground goes the ordinary way.
                    {FvoTrafficRef v{};
                     if(!p.staged&&g_api->holdBirth&&Info(p.vehicle,v)&&!Grounded({v.x,v.y,v.z})){
                         engineport::Forward<&FvoTrafficApi::holdBirth>(g_api,"holdBirth","generator",0u,p.vehicle);p.staged=true;p.st=St::Started;p.at=p.watchAt=now;p.frozenAt=now-3000;p.rearms=0;p.rearmAt=0;
                         MakeTrafficPair(i,p);SaveIds(i,p);SetLife(i,p,spawnflow::Life::Proving,"restored_in_the_air_frozen");
                         Log("Traffic pool: spot %d (%s): reusing vehicle %08X / driver %08X, saved in the air: frozen until a fresh pilot holds it",i,g_world.c_str(),p.vehicle,p.driver);
                         return;}}
					// Made in an earlier visit and still there: reuse it, never make another.
					const bool seated = Aboard(p.driver, p.vehicle);
					p.st = seated ? St::Created : St::Started;
					p.at = now;
					MakeTrafficPair(i, p);
                    if(seated)SetLife(i,p,spawnflow::Life::Proving,"restored_pair_needs_current_handoff");
                    else SetLife(i,p,spawnflow::Life::Driving,"restored_from_save"); // Preserve legacy and unseated recovery.
					Log("Traffic pool: spot %d (%s): reusing vehicle %08X / driver %08X (%s)", i, g_world.c_str(), p.vehicle, p.driver,
						seated ? "driver aboard: starting it" : "driver out: it is ordinary traffic now");
				}
				else if(p.vehicle&&Exists(p.vehicle)){p.st=St::Failed;}
                else { p.vehicle = p.driver = 0; p.st = St::Empty; }
				return;
			}
            case St::Empty: return;  // 2026-11-06: positions are filled only by InitialScene, behind the loading screen or the X panel

			case St::Created: {
				FvoTrafficRef vr{}, dr{};  // failure logging also reads a short-circuited lookup
				if (!Info(p.vehicle, vr) || !Info(p.driver, dr) || !vr.usable || !dr.usable || dr.dead) {
					Log("Traffic pool: spot %d: pair %08X/%08X: vehicle known %d usable %d, driver known %d usable %d dead %d",i,p.vehicle,p.driver,vr.id!=0,vr.usable,dr.id!=0,dr.usable,dr.dead);
					Fail(i, p, "vehicle or driver gone"); return;
				}
				const double separation=std::hypot(std::hypot(vr.x-dr.x,vr.y-dr.y),vr.z-dr.z);
				// A driver with no job seen with no vehicle around them (2026-10-30, the people on the cliffs). The vehicle went to the ground, the driver stayed:
				// the driver is brought to it, where the snap can seat them, every frame until they are near (the snap writes it, see the core's ApiSnapSeat).
				// Nothing vanishes and the pair leaves from where it stands. Disbanding is only for when that does not take: the 11:21 run still had 9 pairs
				// disbanded, too obvious (the user). A driver sitting in it is fine: a long vehicle's seat is far from its centre (SebPod: 7.9 m).
				if(trafficpair::Floating(dr.has3D!=0,vr.has3D!=0,separation)&&!SeatedPair(vr.has3D!=0,dr.has3D!=0,Aboard(p.driver,p.vehicle),engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver),separation)){
					if(!p.floatingSince)p.floatingSince=now;
					if(vr.has3D){
						if(!p.broughtTold){p.broughtTold=true;Log("Traffic pool: spot %d: driver %08X was %.1f m from vehicle %08X (born at z %.1f, vehicle z %.1f, driver z %.1f): brought to it",i,p.driver,separation,p.vehicle,p.birth.z,vr.z,dr.z);}
						if(now-p.seatAt>=250){p.seatAt=now;engineport::Forward<&FvoTrafficApi::snapSeat>(g_api,"snapSeat","generator",0u,p.driver,p.vehicle);}
						// The snap cannot reach a driver tens of metres away: move them to the vehicle every 2 s (2026-11-04).
						if(separation>10&&now-p.floatingAskAt>=2000&&g_api->moveTo){p.floatingAskAt=now;engineport::Forward<&FvoTrafficApi::moveTo>(g_api,"moveTo","generator",0u,p.driver,vr.x,vr.y,vr.z,p.birth.heading);
							Log("OBS system=traffic.spare vehicle=%08X event=driver_moved slot=%d driver=%08X apart=%.1f",p.vehicle,i,p.driver,separation);}  // every frame made log lines by the hundred and asked for a keyword each time
					}
					if(now-p.floatingSince>=(vr.has3D?kApartMs:kNoModelMs)&&now-p.floatingAskAt>=1000){
						p.floatingAskAt=now;
						if(!p.fault){p.fault=vr.has3D?"driver_far_from_vehicle":"no_vehicle_model";p.faultConcealed=false;}  // a fact for Health
					}
					return;
				}
				p.floatingSince=0;
				if (!vr.has3D || !dr.has3D) {
					if(vr.has3D&&!dr.has3D&&now-p.at>=kNo3DMs&&p.reloads<kMaxReloads&&std::hypot(vr.x-pp.x,vr.y-pp.y)<kDriver3DNear&&Grounded({vr.x,vr.y,vr.z})&&Concealed({vr.x,vr.y,vr.z,0},p)){
						++p.reloads;engineport::Forward<&FvoTrafficApi::show>(g_api,"show","generator",0u,p.driver,0);p.reloadAt=now;p.at=now;
						Log("Traffic pool: spot %d: reloading driver model only after vehicle is grounded",i);return;
					}
					if (std::hypot(vr.x - pp.x, vr.y - pp.y) > kSpawnNear) { p.at = now; return; }  // far: its 3D comes when the player does
					if (now - p.at > kLoadWaitMs) Fail(i, p, "no 3D after 120 s");
					return;
				}
				if(!HasBody(p.vehicle,snap)){p.at=now;return;} // a model is not a live chassis
				const int sit = engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver);
				if (SeatedPair(vr.has3D!=0,dr.has3D!=0,Aboard(p.driver,p.vehicle),sit,separation) && HasBody(p.vehicle,snap)) {
					Log("Traffic pool: spot %d: driver %08X sits in vehicle %08X (sit state %d)", i, p.driver, p.vehicle, sit);
					p.st = St::Ready; p.tripAt = 0; return;
				}
				if (p.seatAt && now - p.seatAt < kSeatEveryMs) return;
				if (p.tries >= kSeatTries) { Fail(i, p, "the driver would not sit after 6 instant seats"); return; }
				p.seatAt = now;
				const int r = engineport::Forward<&FvoTrafficApi::snapSeat>(g_api,"snapSeat","generator",0u,p.driver, p.vehicle);
                const bool pending=r==0&&MutationPending(p.driver,"seat");
                const bool spent=spawnflow::SeatAttemptSpent(r,pending);
                if(spent)++p.tries;
				Log("OBS system=traffic.seat vehicle=%08X driver=%08X event=%s slot=%d reply=%d sit=%d completed_tries=%d pending=%d",
                    p.vehicle,p.driver,spent?"attempt":"deferred",i,r,engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver),p.tries,int(pending));
				return;
			}
			case St::Failed: {
                // A failed actor creation must not leak a second vehicle. Retry
                // just the missing driver, on the existing grounded chassis,
                // under the same concealment and readiness rules as admission.
                if(p.driver||!p.vehicle||p.retired||p.reloads>=kMaxReloads||now-g_createdAt<kCreateGapMs)return;
                if(!window.open||!window.cover||budget.granted>=spawnflow::rules::maxCreatePerTick)return;
                FvoTrafficRef v{};if(!Info(p.vehicle,v)||!v.usable||!v.has3D||!HasBody(p.vehicle,snap)||
                    (p.staged?[&]{auto sv=SightAt(v.x,v.y,v.z);return !sv.Covered()&&sv.Exposure()!=2;}():(!Grounded({v.x,v.y,v.z})||!Concealed({v.x,v.y,v.z,0},p))))return;
                FvoPoolSpot at{};at.x=v.x;at.y=v.y;at.z=v.z;at.heading=s.heading;
                const auto grant=SpeakBirth(budget,{spawnflow::PopulationAction::DriverRepair,window},FvoBirthSource::DriverRepair,i,kDriverBase,p.vehicle,at);
                if(!grant)return;
                p.birthGrant=grant;
                g_createdAt=now;++p.reloads;p.driverSource=FvoBirthSource::DriverRepair;p.driverAt=at;p.driverIssued=false;
                const auto reply=ListenBirth(p.birthGrant,p.driverSource,BirthWindow(screen,window,p.driverSource),i,kDriverBase,p.vehicle,p.driverAt,p.driverIssued);
                p.driver=reply.id;
                if(p.driver){SaveIds(i,p);MakeTrafficPair(i,p);p.st=St::Created;p.at=now;p.tries=0;}
                else p.driverPending=reply.pending;
                return;
            }
			case St::Ready: {
                if(p.retired)return;
				if (p.tripAt && (now < p.tripAt || now - p.tripAt < kTripRetryMs)) return;  // tripAt may lie ahead (a back-off)
				p.tripAt = now;
				if (!Exists(p.vehicle) || !Exists(p.driver) || !Aboard(p.driver, p.vehicle)) { p.st = St::Created; p.at = now; p.tries = 0; return; }  // fell out: seat again
				FvoTrafficRef vr{},dr{};
				if(!Info(p.vehicle,vr)||!Info(p.driver,dr)||!vr.usable||!dr.usable||dr.dead||
					!SeatedPair(vr.has3D!=0,dr.has3D!=0,true,engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver),std::hypot(std::hypot(vr.x-dr.x,vr.y-dr.y),vr.z-dr.z))||!HasBody(p.vehicle,snap)){
					p.st=St::Created;p.at=now;p.tries=0;p.seatAt=0;p.seatedSince=0;return;
				}
				if (!g_trip) return;
				// Already driving under another job (e.g. reused aboard after a load): watch it, never claim a trip for it.
				if (engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,p.vehicle)) {
                    p.st=St::Started;p.watchAt=now;
                    handoff::Facts facts{};
                    g_handoffFacts(p.vehicle,p.driver,&facts);
                    if(facts.package){
                        p.proving=false;p.handoff={};p.handoff.beforeRelease=facts.step;
                        p.handoffWait={};p.handoffPending=p.handoffEligible=true;
                        SetLife(i,p,spawnflow::Life::Proving,"existing_traffic_job_needs_fresh_step");
                    }else{
                        p.handoffForeign=true;
                        SetLife(i,p,spawnflow::Life::World,"existing_job_without_matching_traffic_package");
                    }
                    return;
				}
				// Ask first, claim after: a trip claimed and then refused held an addon slot (and a route's count) every 2 s.
				char why[160]{};
				if (engineport::Forward<&FvoTrafficApi::tripRefusal>(g_api,"tripRefusal","generator",0u,p.driver, p.vehicle, why, int(sizeof why))) {
                    engineport::Report(engineport::Event::Refused,"tripRefusal","generator",p.vehicle,why);
					unsigned h = 2166136261u;
					for (const char* c = why; *c; ++c) h = (h ^ unsigned(static_cast<unsigned char>(*c))) * 16777619u;
					if (!p.refusedAt || h != p.refusedHash || now - p.refusedAt > 600000) {
						p.refusedHash = h;
						Log("Traffic pool: spot %d: vehicle %08X cannot start a trip yet: %s (asked again in 10 s)", i, p.vehicle, why);
						p.refusedAt = now;
					}
					p.tripAt = now + 8000;  // with kTripRetryMs: 10 s
					return;
				}
				// The check: still sitting after the hold.
				if(p.proving){
					if(!p.seatedSince)p.seatedSince=now;
					if(!trafficpair::ProveHeld(p.seatedSince,now)){p.tripAt=0;return;}
				}
				// One checked over the player takes its trip for where it is going.
				const bool sendOut=p.proving&&p.staged;
				const Pos vp = sendOut ? Pos{p.birth.x,p.birth.y,p.birth.z} : PosOf(p.vehicle);
				FvoTripClaim claim{};
				if (!g_trip(i, p.vehicle, vp.x, vp.y, vp.z, &claim) || !claim.step) {  // the route side waits (a slot, a route)
					if(!p.routeWaitTold){p.routeWaitTold=true;Log("Traffic pool: spot %d: pair %08X / %08X is seated and waits for room on a route",i,p.vehicle,p.driver);}
					return;
				}
				const bool ok = engineport::Forward<&FvoTrafficApi::startTrip>(g_api,"startTrip","generator",p.vehicle,p.driver, p.vehicle, &claim) != 0;
                if(!ok)engineport::Report(engineport::Event::Refused,"startTrip","generator",p.vehicle,"adapter_returned_false");
				Log("Traffic pool: spot %d: vehicle %08X with driver %08X starts a %s -> %s", i, p.vehicle, p.driver, claim.name ? claim.name : "trip", ok ? "accepted; awaiting handoff proof" : "refused");
				if (ok) {
					if(p.proving)Log("Traffic pool: spot %d: pair %08X / %08X passed its check after %.1f s",i,p.vehicle,p.driver,(now-p.proveSince)/1000.0);
					p.st = St::Started; p.watchAt = now; p.proving=false; p.sending=sendOut; p.sendSince=now;
					p.handoff={};p.handoffWait={};p.handoffPending=true;
				}
				return;
			}
			case St::Started: {
                // Health has already confirmed this handoff failed. Leave the pair to Retirement.
                if(!p.staged&&!p.proving&&p.verdict==spawnflow::Verdict::Broken&&p.uncontrolledFall)return;
                // (HR1, 2026-11-07: the per-model seat check moved to Health's tick, HealthJudge; it judged only started pairs, and pairs
                // whose drivers never sit never start. Same test: both models loaded, aboard, sit state 3 within 10 s of play.)
                // (Removed 2026-11-07, spawn flow v3: the post-load spot check moved placed vehicles a second time through mappedSpot + moveTo.
                // Siting is the only writer of a birth position; solids in a field are F7 tile warnings while drawing, not spawn gates.)
                // A staged pair whose chassis left the physics world never gets released and hangs frozen at its lane spot (2026-10-30
                // 07:38 run: three pairs 340-460 m out, "chassis not in the physics world", frozen in the sky for good). After 30 s
                // without a body, while the player has never seen that spot and cannot see it now, it goes back on the spare shelf
                // and its route position is filled again later (when the player is near enough for physics).
                {FvoTrafficRef bv{};
                if(p.staged&&Info(p.vehicle,bv)&&bv.has3D&&!HasBody(p.vehicle,snap)){
                    if(!p.noBodySince)p.noBodySince=now;
                    else if(now-p.noBodySince>spawnflow::rules::noBodyMs&&!p.fault){p.fault="no_physics_body";p.faultConcealed=true;}  // a fact for Health
                }else{p.noBodySince=0;if(p.fault&&!std::strcmp(p.fault,"no_physics_body"))p.fault=nullptr;}}
                p.handoffEligible=p.staged||p.handoffPending||p.sending;
                if(p.handoffPending)return;
                // M1/F1/H1 (strip build 4, D-S2: retire and replace): after Handoff spawn never moves a car again. The fall guard's move home,
                // its pilot restarts and its set-down are gone: a fall is Health's fact (fell_through_world), a pilot that let go is Health's
                // (no pilot in the air -> driver_out), and Retirement replaces the pair once it is unseen. Kept: the in-place freeze of a car no
                // pilot holds and the ground does not carry, so it hangs still instead of dropping in view until Retirement takes it (it never moves).
                if(g_api->pilotHolds&&g_api->holdBirth&&now-p.fallCheckAt>=500&&!p.staged){
                    p.fallCheckAt=now;
                    FvoTrafficRef v{};
                    if(Info(p.vehicle,v)&&v.has3D&&!engineport::Forward<&FvoTrafficApi::pilotHolds>(g_api,"pilotHolds","generator",0u,p.vehicle)&&!Grounded({v.x,v.y,v.z})){
                        engineport::Forward<&FvoTrafficApi::holdBirth>(g_api,"holdBirth","generator",0u,p.vehicle);p.staged=true;SaveIds(i,p);p.frozenAt=now;
                        Log("OBS system=traffic.cleanup vehicle=%08X event=held_until_retired slot=%d pos=%.0f,%.0f,%.0f reason=no_pilot_holds_it_in_the_air",p.vehicle,i,v.x,v.y,v.z);
                    }
                }
				// Watch: the trip ended at its spot. After kIdleMs with no trip: a driver still aboard gets a new trip, one out of the
				// vehicle is seated again first. kRestarts times.
				if (now - p.watchAt < kWatchMs) return;
				p.watchAt = now;
				if (!Exists(p.vehicle) || !Exists(p.driver) || IsDead(p.driver)) { p.idleSince = 0; return; }
				if (engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,p.vehicle)) {
					// Driving: a minute of it earns its restarts back.
					p.idleSince = 0;
					if (!p.busySince) p.busySince = now;
					else if (now - p.busySince > 60000) { p.restarts = 0; p.reloads = 0; }
					// M1: the in-play driver-model reload is gone; a driver with no model near the player is Health's fact (driver_no_model).
					return;
				}
				p.busySince = 0;
				const bool aboard = Aboard(p.driver, p.vehicle);
				// A driver aboard gets a new trip wherever the vehicle stopped (the journey goes on). M1: one who got out is no longer seated
				// again here; that is Health's fact (driver_out) and Retirement replaces the pair once unseen.
				if (!aboard || p.restarts >= kRestarts) return;
				if (!p.idleSince) { p.idleSince = now; return; }
				if (now - p.idleSince < kIdleMs) return;
				p.idleSince = 0;
				++p.restarts;
				Log("Traffic pool: spot %d: vehicle %08X has had no trip for %llu s (driver aboard): new trip (restart %d)", i, p.vehicle,
					static_cast<unsigned long long>(kIdleMs / 1000), p.restarts);
				p.st = St::Ready; p.tripAt = 0;  // Birth's own progress: ask for the next trip (the Census stays Driving)
				return;
			}
			default: return;
			}
		}

		bool PlayerNow(Pos& pp, std::string& world)
		{
			FvoTrafficPlayer pl{};
			if (!engineport::Forward<&FvoTrafficApi::player>(g_api,"player","generator",0u,&pl)) return false;
			pp = { pl.x, pl.y, pl.z };
			world = pl.world;
			return true;
		}

        void RushStep(){
            if(!g_rushRequest.exchange(false))return;
            g_initialMap=true;g_passRefused.clear();g_rushSettlingAt=0;
            g_initialRejected.clear();g_initialAttempt.clear();g_initialFails.clear();
            g_rosterRecoveryPending=true;
            std::lock_guard lock(g_rushLock);
            g_rush={};g_rush.phase=int(spawnrule::Preparation::Filling);g_rush.total=int(g_pairs.size());g_rush.done=g_rush.total;
            g_rushMade=0;g_rushFill=true;g_coverPassDone=false;g_rush.active=1;
            Log("OBS system=traffic.spawn vehicle=00000000 event=x_prepare slots=%d reason=fill_gaps_keep_fleet",int(g_pairs.size()));
        }

        void Reconcile(){
            std::lock_guard lock(g_lock);
            if(g_wantWorld!=g_world)return;
            auto old=std::move(g_pairs);auto oldNames=std::move(g_names);auto oldSpots=std::move(g_spots);auto oldReuse=std::move(g_reuse);
            std::set<unsigned> used;std::vector<bool> taken(old.size(),false);
            g_pairs.clear();g_names=g_wantNames;g_spots=g_wantSpots;g_reuse=g_wantReuse;
            for(std::size_t i=0;i<g_names.size();++i){
                Pair pair;
                for(std::size_t j=0;j<old.size();++j)if(!taken[j]&&oldNames[j]==g_names[i]){pair=old[j];taken[j]=true;break;}
                if(pair.vehicle)used.insert(pair.vehicle);
                const bool reactivated=pair.retired;
                pair.retired=false;g_pairs.push_back(std::move(pair));
                if(reactivated&&g_pairs.back().vehicle)SaveIds(int(i),g_pairs.back());
            }
            // Census keeps surplus identities until Retirement can clear them.
            for(std::size_t j=0;j<old.size();++j)if(!taken[j]&&(old[j].vehicle||old[j].vehiclePending)&&!used.count(old[j].vehicle)){
                if(old[j].vehicle)used.insert(old[j].vehicle);
                const bool newlySurplus=!old[j].retired;
                old[j].retired=true;
                g_pairs.push_back(std::move(old[j]));g_spots.push_back(oldSpots[j]);g_reuse.push_back(oldReuse[j]);g_names.push_back(oldNames[j]);
                const auto& pair=g_pairs.back();
                if(newlySurplus){
                    if(pair.vehicle)SaveIds(int(g_pairs.size()-1),pair);
                    Log("OBS system=traffic.census vehicle=%08X driver=%08X event=surplus reason=fleet_target_reduced",pair.vehicle,pair.driver);
                }
            }
            g_admission=-1;g_appliedRevision=g_revision;
        }
        bool RecoverySlot(const Pair& p){
            return !p.vehicle&&!p.driver&&!p.vehiclePending&&!p.driverPending&&!p.reserved&&!p.retired;
        }
        void ClaimRecovered(int index,Pair pair,const FvoTrafficRef& v,ULONGLONG now,const char* source){
            pair.base=v.baseId;pair.at=now;Measure(pair,pair.base);
            g_pairs[index]=std::move(pair);
            auto& p=g_pairs[index];
            if(p.staged&&g_api->holdBirth)engineport::Forward<&FvoTrafficApi::holdBirth>(g_api,"holdBirth","census",p.vehicle,p.vehicle);
            SaveIds(index,p);
            Log("OBS system=traffic.census vehicle=%08X driver=%08X event=fleet_recovered slot=%d reason=%s",
                p.vehicle,p.driver,index,source);
        }
        // Rows older versions wrote for retired systems (factory bay slots, runtime stock, landing spots): retiringrecord.h
        // RetiredRowKind / ParseRetiredRow / DecideRetiredRow, checked in traffic_rules_test.
        struct RetiredSystemEntry { std::wstring key; retiringrecord::PairRow row; bool landing=false; };
        // One pass on load: a car the public fleet already holds keeps that owner; a car whose saved journey names it is left to
        // the journey recovery (RecoverJourneyPairs) to join the normal fleet; every other car that is still ours is retired
        // through the arbiter (the request survives a save). The row is then deleted; a car that is not loaded keeps its row.
        void RecoverRetiredSystems(const std::vector<RetiredSystemEntry>& rows,const std::set<unsigned>& vehicles,const std::set<unsigned>& drivers){
            using retiringrecord::RetiredOutcome;
            int counts[6]{};
            const auto section=Wide(g_world);
            for(const auto& old:rows){
                const auto& row=old.row;
                FvoTrafficRef v{},d{};
                const bool loaded=row.vehicle&&Info(row.vehicle,v);
                const bool vehicleOurs=loaded&&retiringrecord::VehicleStillOurs(true,v.isActor!=0,v.baseId,row.base);
                const bool driverOurs=row.driver&&retiringrecord::DriverStillOurs(Info(row.driver,d),d.isActor!=0,d.baseId,kDriverBase);
                const bool publicOwner=(row.vehicle&&vehicles.count(row.vehicle))||(row.driver&&drivers.count(row.driver))||
                    std::any_of(g_deferredRegistry.begin(),g_deferredRegistry.end(),[&](const DeferredPair& p){return p.row.vehicle==row.vehicle;});
                const bool journey=vehicleOurs&&g_savedFleetJourney&&g_savedFleetJourney(row.vehicle,v.baseId,g_world.c_str());
                const auto outcome=retiringrecord::DecideRetiredRow(row.vehicle,publicOwner,loaded,vehicleOurs,journey);
                ++counts[int(outcome)];
                if(outcome==RetiredOutcome::Retire)
                    RetireRequest(row.vehicle,driverOurs?row.driver:0,old.landing?"retired_landing_spot":"retired_factory_stock");
                Log("OBS system=traffic.census vehicle=%08X driver=%08X event=retired_system_row key=%ls outcome=%s",row.vehicle,row.driver,old.key.c_str(),retiringrecord::RetiredOutcomeName(outcome));
                if(outcome!=RetiredOutcome::NotLoadedKeep)WriteRegistry(section.c_str(),old.key.c_str(),nullptr);
            }
            if(!rows.empty())Log("OBS system=traffic.census vehicle=00000000 event=retired_system_rows total=%zu public_owner=%d journey=%d retired=%d gone=%d not_loaded=%d unreadable=%d",
                rows.size(),counts[int(RetiredOutcome::PublicOwner)],counts[int(RetiredOutcome::JourneyRecovery)],counts[int(RetiredOutcome::Retire)],
                counts[int(RetiredOutcome::Gone)],counts[int(RetiredOutcome::NotLoadedKeep)],counts[int(RetiredOutcome::Unreadable)]);
        }
        void RecoverRegistry(){
            g_deferredRegistry.clear();g_registryRecoveryCursor=0;g_registryRecoveryAt=0;g_linkedRecoveryAt=0;
            const auto now=GetTickCount64();
            // Read the entire historical registry: route-key churn used to orphan
            // old saved fleets. Adopt each valid physical pair at most once.
            std::vector<wchar_t> section(65536);
            GetPrivateProfileSectionW(Wide(g_world).c_str(),section.data(),DWORD(section.size()),Ini().c_str());
            std::vector<RetiredSystemEntry> retiredRows;
            for(const wchar_t* item=section.data();*item;item+=wcslen(item)+1){
                const int kind=retiringrecord::RetiredRowKind(item);
                const auto value=wcschr(item,L'=');
                if(!kind||!value)continue;
                RetiredSystemEntry old;old.key.assign(item,value);old.landing=kind==2;
                old.row=retiringrecord::ParseRetiredRow(kind,value+1);
                retiredRows.push_back(std::move(old));
            }
            LoadRetiringSaved();
            const auto retiring=[&](const FvoTrafficRef& v){
                return std::any_of(g_retiringSaved.begin(),g_retiringSaved.end(),[&](const auto& entry){
                    return entry.vehicle==v.id&&retiringrecord::VehicleStillOurs(v.id!=0,v.isActor!=0,v.baseId,entry.vbase);
                });
            };
            std::set<unsigned> vehicles,drivers;
            for(int i=0;i<int(g_pairs.size());++i){auto& p=g_pairs[i];
                if(!p.vehicle)LoadIds(i,p);
                FvoTrafficRef v{},d{};
                if(p.vehicle&&!Info(p.vehicle,v)){p={};p.st=St::Empty;p.at=now+kSavedSlotGraceMs;}
                if(p.vehicle&&(v.isActor||!retiringrecord::RecoveryBaseMatches(p.base,v.baseId,kRev8)||vehicles.count(p.vehicle)||retiring(v)))p={};
                if(p.vehicle&&p.driver&&!Info(p.driver,d)){p={};p.st=St::Empty;p.at=now+kSavedSlotGraceMs;}
                if(p.vehicle&&(!p.driver||!d.isActor||drivers.count(p.driver))){p.driver=0;p.st=St::Failed;}
                if(p.vehicle){Measure(p,v.baseId);vehicles.insert(p.vehicle);if(p.driver)drivers.insert(p.driver);if(p.staged&&g_api->holdBirth)engineport::Forward<&FvoTrafficApi::holdBirth>(g_api,"holdBirth","generator",0u,p.vehicle);}
            }
            for(const wchar_t* row=section.data();*row;row+=wcslen(row)+1){
                if(wcsncmp(row,L"pair_",5)&&wcsncmp(row,L"spot",4))continue;
                const auto eq=wcschr(row,L'=');if(!eq)continue;
                const auto saved=retiringrecord::ParsePair(eq+1);
                if(!saved.vehicle||vehicles.count(saved.vehicle)||(saved.driver&&drivers.count(saved.driver)))continue;
                Pair p;
                p.vehicle=saved.vehicle;p.driver=saved.driver;p.base=saved.base;p.staged=saved.staged;
                FvoTrafficRef v{},d{};
                const bool haveVehicle=Info(p.vehicle,v);
                const bool haveDriver=p.driver&&Info(p.driver,d);
                if(haveVehicle&&(!v.usable||v.isActor||!retiringrecord::RecoveryBaseMatches(p.base,v.baseId,kRev8)||retiring(v)))continue;
                if(!haveVehicle||(!haveDriver&&p.driver&&!saved.surplus)){
                    if(g_deferredRegistry.size()<1024)g_deferredRegistry.push_back({std::wstring(row,eq),saved});
                    continue;
                }
                if(!haveDriver||!d.usable||!d.isActor||d.dead||!Aboard(p.driver,p.vehicle)){
                    if(!saved.surplus||!p.base)continue;
                    p.driver=0;  // an unlinked actor may belong to the world now
                }
                auto target=saved.surplus?g_pairs.end():
                    std::find_if(g_pairs.begin(),g_pairs.end(),[&](const Pair& q){return RecoverySlot(q)&&now>=q.at;});
                if(target==g_pairs.end()){
                    if(g_pairs.size()>=1024)break; // bound legacy-cache recovery, not physical admission
                    p.storedKey.assign(row,eq);p.retired=true;
                    const auto name=p.storedKey.starts_with(L"pair_")?p.storedKey.substr(5):L"retained."+std::to_wstring(p.vehicle);
                    g_pairs.push_back({});g_spots.push_back({v.x,v.y,v.z,0});g_reuse.push_back(0);
                    g_names.emplace_back(name.begin(),name.end());target=std::prev(g_pairs.end());
                }
                p.st=St::Created;p.at=now;
                vehicles.insert(p.vehicle);if(p.driver)drivers.insert(p.driver);
                ClaimRecovered(int(target-g_pairs.begin()),std::move(p),v,now,"saved_registry");
            }
            RecoverRetiredSystems(retiredRows,vehicles,drivers);
        }

        void RecoverDeferredRegistry(ULONGLONG now){
            if(g_deferredRegistry.empty()||now<g_registryRecoveryAt)return;
            g_registryRecoveryAt=now+kRegistryRecoveryIntervalMs;
            const auto budget=std::min(kRegistryRecoveryBatch,g_deferredRegistry.size());
            for(std::size_t examined=0;examined<budget&&!g_deferredRegistry.empty();++examined){
                const auto index=g_registryRecoveryCursor%g_deferredRegistry.size();
                const auto candidate=g_deferredRegistry[index];
                const auto& row=candidate.row;
                const auto forget=[&]{g_deferredRegistry.erase(g_deferredRegistry.begin()+index);};
                if(PopulationTracks(row.vehicle,row.driver)||Retiring(row.vehicle)||Retiring(row.driver)||
                    (g_api->playerTook&&g_api->playerTook(row.vehicle))||
                    std::any_of(g_spares.begin(),g_spares.end(),[&](const Spare& s){return s.vehicle==row.vehicle||s.driver==row.driver;})||
                    std::any_of(g_retiringSaved.begin(),g_retiringSaved.end(),[&](const auto& r){return r.vehicle==row.vehicle;})){
                    forget();continue;
                }
                FvoTrafficRef v{},d{};
                if(!Info(row.vehicle,v)){++g_registryRecoveryCursor;continue;}
                if(v.isActor||!retiringrecord::RecoveryBaseMatches(row.base,v.baseId,kRev8)){forget();continue;}
                const bool linked=row.driver&&Info(row.driver,d)&&d.usable&&d.isActor&&!d.dead&&Aboard(row.driver,row.vehicle);
                if(!v.usable||(!linked&&!(row.surplus&&row.base))){++g_registryRecoveryCursor;continue;}
                if(g_api->usesVehicle&&g_api->usesVehicle(row.vehicle)){++g_registryRecoveryCursor;continue;}
                const auto end=g_pairs.begin()+std::min(g_pairs.size(),g_initialScene.size());
                auto slot=end;
                if(!row.surplus){
                    for(auto it=g_pairs.begin();it!=end;++it)if(RecoverySlot(*it)&&PairKey(int(it-g_pairs.begin()))==candidate.key){slot=it;break;}
                    if(slot==end)slot=std::find_if(g_pairs.begin(),end,[&](const Pair& p){return RecoverySlot(p)&&now>=p.at;});
                    if(slot==end){++g_registryRecoveryCursor;continue;}
                }
                Pair pair;pair.vehicle=row.vehicle;pair.driver=linked?row.driver:0;pair.staged=row.staged;
                int target=int(slot-g_pairs.begin());
                if(row.surplus){
                    if(g_pairs.size()>=1024){++g_registryRecoveryCursor;continue;}
                    target=int(g_pairs.size());pair.retired=true;pair.storedKey=candidate.key;
                    const auto name=candidate.key.starts_with(L"pair_")?candidate.key.substr(5):L"retained."+std::to_wstring(row.vehicle);
                    g_pairs.push_back({});g_spots.push_back({v.x,v.y,v.z,0});g_reuse.push_back(0);g_names.emplace_back(name.begin(),name.end());
                }
                ClaimRecovered(target,std::move(pair),v,now,"late_saved_registry");
                forget();
            }
        }

        // Census: a linked actor alone is not evidence that a pair belongs to FVO. The same
        // save's journey must identify the vehicle and world before a missing entry is adopted.
        void RecoverJourneyPairs(ULONGLONG now){
            if(!g_savedFleetJourney||!g_api->linkedPairs)return;
            const auto end=g_pairs.begin()+std::min(g_pairs.size(),g_initialScene.size());
            const auto free=[&](const Pair& p){return RecoverySlot(p)&&now>=p.at;};
            if(std::none_of(g_pairs.begin(),end,free))return;
            LoadSpares();LoadRetiringSaved();
            std::set<unsigned> claimed;
            for(const auto& p:g_pairs){if(p.vehicle)claimed.insert(p.vehicle);if(p.driver)claimed.insert(p.driver);}
            for(const auto& spare:g_spares){claimed.insert(spare.vehicle);claimed.insert(spare.driver);}
            for(const auto& retiring:g_retiringSaved){claimed.insert(retiring.vehicle);claimed.insert(retiring.driver);}
            unsigned drivers[kMaxSpots]{},vehicles[kMaxSpots]{};
            const int found=engineport::Forward<&FvoTrafficApi::linkedPairs>(g_api,"linkedPairs","generator",0u,drivers,vehicles,kMaxSpots);
            int recovered=0,unmatched=0,deferred=0;
            for(int i=0;i<std::min(found,kMaxSpots);++i){
                const unsigned vehicle=vehicles[i],driver=drivers[i];
                if(!vehicle||!driver||vehicle==driver||claimed.count(vehicle)||claimed.count(driver)||Retiring(vehicle)||Retiring(driver))continue;
                FvoTrafficRef v{},d{};
                if(!Info(vehicle,v)||!v.usable||v.isActor||!Info(driver,d)||!d.usable||!d.isActor||d.dead||!Aboard(driver,vehicle))continue;
                if(!g_savedFleetJourney(vehicle,v.baseId,g_world.c_str())){++unmatched;continue;}
                const auto slot=std::find_if(g_pairs.begin(),end,free);
                if(slot==end){++deferred;continue;}
                const int index=int(slot-g_pairs.begin());
                Pair pair;pair.vehicle=vehicle;pair.driver=driver;
                ClaimRecovered(index,std::move(pair),v,now,"same_save_journey_and_live_link");
                claimed.insert(vehicle);claimed.insert(driver);++recovered;
            }
            if(recovered||g_rosterRecoveryPending)Log("OBS system=traffic.census vehicle=00000000 event=fleet_recovery recovered=%d unverified_left_alone=%d deferred_no_slot=%d",recovered,unmatched,deferred);
        }
        void RecoverSavedFleet(ULONGLONG now,int presentation){
            if(!presentation||g_initialClockPending)return;
            LoadSpares();LoadRetiringSaved();
            RecoverDeferredRegistry(now);
            const bool covered=spawnrule::Open(presentation,g_rushFill&&(presentation!=2||XPanelCovered()));
            if(g_initialScene.empty()||(!covered&&presentation!=1)||(!g_rosterRecoveryPending&&now<g_linkedRecoveryAt))return;
            RecoverJourneyPairs(now);
            g_rosterRecoveryPending=false;g_linkedRecoveryAt=now+kLinkedRecoveryIntervalMs;
        }
	}

	void Attach(const FvoTrafficApi* api, FvoLog_t log) { g_api = api; g_logFn = log; engineport::observer.store(&BoundaryTrace); }

	void SetSpots(const char* world, const FvoPoolSpot* spots, int count, const int* parking, const char* const* names, FvoPoolTrip trip)
	{
		std::lock_guard lk(g_lock);
		g_wantWorld = world ? world : "";
        const int n=spots?std::clamp(count,0,kMaxSpots):0;
        std::vector<FvoPoolSpot> next;if(n)next.assign(spots,spots+n);
        std::vector<int> reused(std::size_t(n),0);if(parking&&n)std::copy(parking,parking+n,reused.begin());
        std::vector<std::string> labels;for(int k=0;k<n;++k)labels.push_back(names&&names[k]?names[k]:"");
        bool changed=g_wantSpots.size()!=next.size()||g_wantNames!=labels||g_wantReuse!=reused;
        for(std::size_t k=0;!changed&&k<next.size();++k)changed=g_wantSpots[k].x!=next[k].x||g_wantSpots[k].y!=next[k].y||g_wantSpots[k].z!=next[k].z||g_wantSpots[k].heading!=next[k].heading;
        if(changed)++g_revision;
        g_wantSpots=std::move(next);g_wantReuse=std::move(reused);g_wantNames=std::move(labels);
		g_trip = trip;
	}

    void SetStorage(const wchar_t* registry){std::lock_guard lock(g_lock);g_profileIni=registry?registry:L"";}
    void SetModelRules(const std::vector<std::vector<unsigned>>& skip){g_slotSkip=skip;g_fitRejected.clear();g_modelUnavailable.clear();}
    void SetModelGate(ModelGate gate){g_modelGate=gate;}
    void SetSiteGate(SiteGate gate,RoadSpacingGate spacing){g_siteGate=gate;g_roadSpacing=spacing;}
    void SetCensusPublisher(FvoTrafficCensusPublishV1 publish,FvoTrafficCensusForgetV1 forget){g_censusPublish=publish;g_censusForget=forget;}
    void SetBirthListener(FvoTrafficBirthFnV1 listener){g_birthListener=listener;}
    void SetHandoffListeners(handoff::ReadFacts facts,handoff::Release release,handoff::RetireShow retire){
        g_handoffFacts=release?facts:nullptr;g_handoffRelease=g_handoffFacts?release:nullptr;g_retireShowV2=retire;
    }
    // Required with the fleet exports (every core that has those has these): without them Tick creates no traffic.
    void SetRoadClearance(int (*read)(FvoTrafficVehicle*,int), FvoRoadCheckFnV1 check, FvoTrafficBirthFnV2 birth){
        const bool available=read&&check&&birth&&g_birthListener;
        g_roadVehicles=available?read:nullptr;g_roadCheck=available?check:nullptr;g_roadBirth=available?birth:nullptr;
    }
    int DecideParkedRetirement(unsigned vehicle,const char* reason,bool protectedIdentity){
        if(!g_api)return 0;
        Pos player;std::string world;FvoTrafficRef v{};
        if(!PlayerNow(player,world)||!Info(vehicle,v)||!v.usable||v.isActor)return 0;
        const bool activeWorld=world==g_world;
        const bool preparingCensus=!activeWorld&&g_api->trafficOn&&g_api->trafficOn();
        const bool pendingSaved=activeWorld&&std::any_of(g_deferredRegistry.begin(),g_deferredRegistry.end(),[&](const DeferredPair& p){
            return p.row.vehicle==vehicle&&(!p.row.base||p.row.base==v.baseId);
        });
        const bool savedJourney=g_savedFleetJourney&&g_savedFleetJourney(vehicle,v.baseId,world.c_str());
        const bool pendingBirth=activeWorld&&std::any_of(g_pairs.begin(),g_pairs.end(),[&](const Pair& p){
            return p.vehiclePending&&p.reserved&&p.base==v.baseId&&
                std::hypot(v.x-p.birth.x,v.y-p.birth.y)<=2.0&&std::abs(v.z-p.birth.z)<=8.0;
        });
        const bool claimed=protectedIdentity||preparingCensus||pendingSaved||savedJourney||pendingBirth||
            (activeWorld&&(PopulationTracks(vehicle,0)||Retiring(vehicle)||
                std::any_of(g_spares.begin(),g_spares.end(),[=](const Spare& s){return s.vehicle==vehicle;})));
        const auto now=GetTickCount64();
        const auto previous=g_parkedGrants.find(vehicle);
        if(previous!=g_parkedGrants.end()&&previous->second.base==v.baseId&&now<previous->second.expires)return 0;
        Pair candidate;candidate.vehicle=vehicle;
        g_playerAt=player;
        const auto order=DecideRetirement(candidate,0,reason,claimed);
        if(!order.decision.now)return 0;
        if(g_parkedGrants.size()>=256){
            std::erase_if(g_parkedGrants,[&](const auto& entry){return entry.second.expires<=now;});
            if(g_parkedGrants.size()>=256)return 0;
        }
        g_parkedGrants[vehicle]={v.baseId,now+kParkedRetryMs};
        Log("OBS system=traffic.retirement vehicle=%08X event=grant speaker=arbiter effect=hide_only reason=%s rule=%s",vehicle,reason,order.rule);
        return 1;
    }
    void ReportDriverRetirement(unsigned vehicle,unsigned driver){
        if(!driver||g_world.empty())return;
        if(PopulationTracks(vehicle,driver)){
            Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=watch_report listener=health reason=registered_pair",vehicle,driver);
            return; // Health already reads the watch's PairHealth observation for registered traffic.
        }
        FvoTrafficRef ref{};
        if(vehicle&&(!Info(vehicle,ref)||!ref.usable))return; // the unknown half could still be in sight
        RetireRequest(vehicle,driver,"driver_watch_orphan");
    }
    void SetIdAdapter(unsigned (*adapter)(unsigned)){g_idAdapter=adapter;}
    void SetInitialSpotKeys(const std::vector<std::string>& keys){g_spotKeys=keys;}
    int OutOfTraffic(unsigned* out,int max){int n=0;for(const auto& p:g_pairs)if(n<max&&p.vehicle&&!p.retired&&p.verdict==spawnflow::Verdict::Broken&&p.life!=spawnflow::Life::World)out[n++]=p.vehicle;return n;}
    void SetHealthSources(int (*sample)(unsigned,FvoPairHealthSampleV1*),void (*spot)(unsigned)){g_healthSample=sample;g_spotRequest=spot;}  // CH-1/CH-4, optional
    void SetPlayClock(unsigned (*fn)()){g_playClock=fn;Log("OBS system=traffic.window vehicle=00000000 event=play_clock_source available=%d reason=%s",int(fn!=nullptr),fn?"core_FVO_PlayClockV1":"older_core_presentation");}
    void SetBirthRefusal(FvoBirthRefusalFnV1 refusal){g_birthRefusal=refusal;}
    void SetPhysicalFleet(FvoFleetRailAssignV1Fn assign,FvoFleetRailReleaseV1Fn release,
                          FvoFleetRailStateV1Fn state,FleetPath path){
        const bool ready=assign&&release&&state&&path;
        g_fleetAssign=ready?assign:nullptr;
        g_fleetRelease=ready?release:nullptr;
        g_fleetState=ready?state:nullptr;
        g_fleetPath=ready?path:nullptr;
    }
    bool PhysicalFleetActive(){return g_fleetAssign!=nullptr;}
    void SetCameraAhead(int (*fn)(double,double,double,double,double*)){g_cameraAhead=fn;}  // CP-1, logged when the switch is read
    void SetCameraEnvelopeAhead(int (*fn)(double,double,double,double,double)){g_cameraEnvelopeAhead=fn;}
    void SetCameraEvidence(cameravisibility::Reader reader,cameravisibility::ForecastReader forecast){
        g_cameraEvidence=reader;g_lookForecast=forecast;
    }
    void SetRemovalPrediction(bool on){
        Log("OBS system=traffic.retirement vehicle=00000000 event=removal_rule prediction=%d core_export=%d reason=%s",int(on),int(g_cameraAhead!=nullptr),
            !on?"settings_traffic_removeByCameraPrediction_0":g_cameraAhead?"settings_traffic_removeByCameraPrediction_1":"older_core_no_FVO_CameraAheadV1_far_and_out_of_sight_only");
        g_predictOn=on;}
    void SetRelocation(FvoRelocationStatusFn fn){g_relocation=fn;}
    void SetSpotTest(int (*fn)(unsigned)){g_spotTest=fn;}  // logged on the first Health judgement (the log is not attached yet here)
    bool HealthModel(unsigned model,FvoTrafficHealthModelV1& out){
        std::lock_guard lock(g_healthPubLock);const auto it=g_healthPub.find(model);if(it==g_healthPub.end())return false;const auto& r=it->second;
        out.known=1;out.rejected=r.rejected?1:0;out.allowed=r.allowed?1:0;out.stage=r.why;
        const int s=r.why>=0?r.why:int(spawnhealth::Stage::Seat);out.pass=float(r.stage[s].pass);out.fail=float(r.stage[s].fail);out.spots=int(r.stage[s].with.size());
        std::snprintf(out.stageName,sizeof out.stageName,"%s",spawnhealth::Name(spawnhealth::Stage(s)));return true;}
    void HealthAllow(unsigned model,bool allow){std::lock_guard lock(g_healthPubLock);g_healthAllowQueue.push_back({model,allow});}
    bool HealthReason(unsigned model,char* out,int n){
        if(!out||n<=0)return false;out[0]=0;std::lock_guard lock(g_healthPubLock);const auto it=g_healthWhy.find(model);if(it==g_healthWhy.end())return false;
        std::snprintf(out,std::size_t(n),"%s",it->second.c_str());return true;}
    // Spot keys are spawnhealth::SpotKey: <route>/<lane>/<ix>_<iy> (route cleaned, cells 10 m).
    bool HealthSpot(const char* route,int index,FvoTrafficHealthSpotV1& out){
        if(!route||index<0)return false;const std::string prefix=spawnhealth::Clean(route)+"/";
        std::lock_guard lock(g_healthPubLock);int k=0;
        for(const auto& [key,r]:g_healthSpotPub){if(key.compare(0,prefix.size(),prefix)!=0)continue;if(k++!=index)continue;
            int lane=0;long ix=0,iy=0;if(std::sscanf(key.c_str()+prefix.size(),"%d/%ld_%ld",&lane,&ix,&iy)!=3)return false;
            out.lane=lane;out.x=(double(ix)+.5)*10;out.y=(double(iy)+.5)*10;out.stage=r.why;const int s=r.why>=0?r.why:0;
            out.pass=float(r.stage[s].pass);out.fail=float(r.stage[s].fail);out.models=int(r.stage[s].with.size());
            std::snprintf(out.stageName,sizeof out.stageName,"%s",spawnhealth::Name(spawnhealth::Stage(s)));return true;}
        return false;}
    void SetInitialScene(const FvoPoolSpot* spots,int count){
        g_initialRoads.clear();
        g_initialScene.clear();if(spots&&count>0)g_initialScene.assign(spots,spots+std::min(count,kMaxSpots));g_initialRejected.clear();
        g_initialAlternatives.clear();g_initialAttempt.clear();g_initialFails.clear();g_initialCursor=0;g_modelUnavailable.clear();g_fitRejected.clear();g_clearanceWaiting.clear();
    }
    void SetInitialRoads(const std::vector<bool>& roads){g_initialRoads=roads;}
    void SetInitialAlternatives(int slot,const FvoPoolSpot* spots,int count){if(slot>=0&&slot<int(g_initialScene.size())&&spots&&count>0)g_initialAlternatives[slot].assign(spots,spots+std::min(count,16));}
    void WaitForInitialClock(bool pending){g_initialClockPending=pending;}
    bool InitialVehicle(int spot){return spot>=0&&spot<int(g_pairs.size())&&g_pairs[spot].staged;}
    // Under cover, finish previously recorded retirements and give Health its usual covered check.
    // A pair missing from the roster is not a retirement decision.
    void CoverPass(const Snap& snap,ULONGLONG now){
        (void)now;g_coverPassDone=true;
        if(!g_api->holdBirth||!g_api->moveTo||!g_api->show)return;
        std::set<unsigned> mine;for(const auto& p:g_pairs){if(p.vehicle)mine.insert(p.vehicle);if(p.driver)mine.insert(p.driver);}
        // A shelved pair is already owned by cleanup. Do not submit a second hide for the same asynchronous retirement.
        for(const auto& sp:g_spares){mine.insert(sp.vehicle);mine.insert(sp.driver);}
        int hiddenLeft=0;
        const int total=std::min(int(g_initialScene.size()),int(g_pairs.size()));
        // Old spares are old traffic too: hidden already, now deleted (the user, 2026-11-06). A stored id passed LoadSpares' base check.
        {LoadSpares();const auto old=g_spares;for(const auto& sp:old){Log("OBS system=traffic.spawn vehicle=%08X event=old_spare_deleted driver=%08X base=%08X",sp.vehicle,sp.driver,sp.base);RetireRequest(sp.vehicle,sp.driver,"old_spare");++hiddenLeft;}}
        // SW-2 Retirement: pairs the last session was still retiring (SW-1 Census record) are finished here, under cover, before Arrival.
        // Ids are reused across saves: only an id that still names the stored base is touched (a driver: an actor with the stored base).
        {LoadRetiringSaved();const auto saved=g_retiringSaved;int asked=0,dropped=0;
         for(const auto& e:saved){FvoTrafficRef v{},d{};
            const bool vOk=e.vehicle&&retiringrecord::VehicleStillOurs(Info(e.vehicle,v),v.isActor!=0,v.baseId,e.vbase);
            const bool dOk=e.driver&&retiringrecord::DriverStillOurs(Info(e.driver,d),d.isActor!=0,d.baseId,e.dbase);
            if(e.vehicle&&!vOk){Log("OBS system=traffic.census vehicle=%08X event=retiring_dropped reason=id_reused_or_gone base_was=%08X base_now=%08X",e.vehicle,e.vbase,v.baseId);CensusRetiringClear(e.vehicle,"id_reused_or_gone");++dropped;}
            if(e.driver&&!dOk){Log("OBS system=traffic.census vehicle=%08X event=retiring_dropped reason=driver_id_reused_or_gone base_was=%08X base_now=%08X",e.driver,e.dbase,d.baseId);CensusRetiringClear(e.driver,"driver_id_reused_or_gone");++dropped;}
            if(mine.count(e.vehicle)||mine.count(e.driver))continue;  // a slot or a spare owns it now
            if(vOk||dOk){RetireRequest(vOk?e.vehicle:0,dOk?e.driver:0,"retiring_from_last_session");++asked;++hiddenLeft;}
         }
         if(asked||dropped)Log("OBS system=traffic.retirement vehicle=00000000 event=last_session_retiring asked=%d dropped=%d",asked,dropped);}
        // Bad pairs (2026-10-30, the user: replaced behind the loading screen, never seen): hidden and kept as spares; the arrival
        // fills their positions under this same cover, and the handoff keeps a new vehicle frozen until its driver really sits.
        int replaced=0;
        for(int i=0;i<total;++i){auto& p=g_pairs[std::size_t(i)];
            if(p.vehicle&&g_fleetState&&g_fleetState(p.vehicle)>=2)continue;
            if(HealthJudge(i,p,snap,now,true))continue;
            if(RetireTick(i,p,now))++replaced;
        }
        if(replaced)Log("Traffic cover pass: %d bad pair(s) replaced",replaced);
        if(hiddenLeft)Log("Traffic cover pass: %d recorded retirement(s) handled",hiddenLeft);
    }
    // Handoff is the sole release arbiter. Step supplies eligible Census rows after Health.
    void ServiceHandovers(const Snap& snap,ULONGLONG now){
        const auto release=[](unsigned vehicle,bool player){
            if(g_handoffRelease)return g_handoffRelease(vehicle,player)!=0;
            return g_api->releaseBirth&&engineport::Forward<&FvoTrafficApi::releaseBirth>(g_api,"releaseBirth","generator",0u,vehicle);
        };
        // KO-2a Handoff: ask again for each taken frozen vehicle until the birth-hold service grants it.
        for(auto it=g_takenRelease.begin();it!=g_takenRelease.end();){
            if(g_api->playerTook&&!g_api->playerTook(*it)){it=g_takenRelease.erase(it);continue;}
            if(release(*it,true)){
                Log("OBS system=traffic.handoff vehicle=%08X event=released reason=taken_by_the_player",*it);it=g_takenRelease.erase(it);}
            else ++it;}
        constexpr unsigned kReadyLimit=8;
        unsigned serviced=0;
        for(std::size_t scanned=0;scanned<g_pairs.size()&&serviced<kReadyLimit;++scanned){
            const int i=int(g_handoffCursor++%g_pairs.size());
            auto& p=g_pairs[i];
            if(!p.handoffEligible||p.life==spawnflow::Life::World||p.fallRetirementPending||p.verdict==spawnflow::Verdict::Broken||
                (g_api->playerTook&&g_api->playerTook(p.vehicle)))continue;
            if(g_handoffFacts&&!HasBody(p.vehicle,snap))continue;
            ++serviced;
            // Confirmed handoff: released only when its physics body exists, its driver sits in it and its pilot holds it.
            FvoTrafficRef hv{};
            const bool localRoad=p.bornAt&&i<int(g_initialRoads.size())&&g_initialRoads[i];
            const bool handoffReady=[&]{
                FvoTrafficRef driver{};
                return Info(p.vehicle,hv)&&hv.has3D&&HasBody(p.vehicle,snap)&&Aboard(p.driver,p.vehicle)&&
                    trafficpair::Sitting(engineport::Forward<&FvoTrafficApi::sitState>(g_api,"sitState","generator",0u,p.driver))&&
                    (!localRoad||(Info(p.driver,driver)&&driver.usable&&driver.has3D&&!driver.dead));
            }();
            const bool roadPose=!localRoad||
                (std::isfinite(hv.x)&&std::isfinite(hv.y)&&std::isfinite(hv.z)&&std::hypot(hv.x-p.birth.x,hv.y-p.birth.y)<1.5&&hv.z-p.birth.z<1.5&&p.birth.z-hv.z<4.0);  // settling onto the road under the birth height (hull-bottom offset, 2026-11-04: -1.6 m) is on the road; deeper is a fall
            // Load cleanup (2026-11-04): a deferral is decided after 10 s instead of holding the pair forever. Settled at its spot
            // up to 8 m under the route = on the real road under an authored height that is off (spot 38: route 4.2 m over it):
            // released. Anything else fell or was misplaced: deleted while out of sight, the position freed.
            bool settledUnderRoute=false;
            if(p.staged&&handoffReady&&!roadPose){
                if(!p.handoffDeferSince)p.handoffDeferSince=now;
                else if(now-p.handoffDeferSince>=10000){
                    const bool atSpot=std::isfinite(hv.x)&&std::hypot(hv.x-p.birth.x,hv.y-p.birth.y)<1.5;
                    if(atSpot&&p.birth.z-hv.z>=0&&p.birth.z-hv.z<8.0){settledUnderRoute=true;
                        if(now-p.handoffLogAt>=5000){p.handoffLogAt=now;  // once per 5 s, not every frame (5,837 lines in one run)
                            Log("OBS system=traffic.cleanup vehicle=%08X event=released slot=%d reason=settled_under_route drop=%.2f",p.vehicle,i,p.birth.z-hv.z);}}
                    else if(!p.fault){  // a fact for Health: it judges, Retirement removes under cover
                        Log("OBS system=traffic.cleanup vehicle=%08X event=fault slot=%d reason=%s requested=%.2f,%.2f,%.2f observed=%.2f,%.2f,%.2f",p.vehicle,i,atSpot?"fell_through_road":"moved_off_spot",p.birth.x,p.birth.y,p.birth.z,hv.x,hv.y,hv.z);
                        p.fault=atSpot?"fell_through_road":"moved_off_spot";p.faultConcealed=true;
                    }
                }
            }else p.handoffDeferSince=0;
            if(p.staged&&handoffReady&&!roadPose&&!settledUnderRoute&&now-p.handoffLogAt>=2000&&(p.handoffLogAt=now,true))Log("OBS system=traffic.placement vehicle=%08X event=road_handoff_deferred slot=%d requested=%.3f,%.3f,%.3f observed=%.3f,%.3f,%.3f",p.vehicle,i,p.birth.x,p.birth.y,p.birth.z,hv.x,hv.y,hv.z);
            bool authorized=handoffReady;
            if(g_handoffFacts&&(p.staged||p.handoffPending)){
                handoff::Facts facts{};
                if(handoffReady)g_handoffFacts(p.vehicle,p.driver,&facts);
                const auto previous=p.handoff.phase;
                const auto phase=p.handoff.Observe(handoffReady,facts.package,facts.bound,p.staged,facts.sampled,facts.recentStep,facts.step);
                if(previous!=phase)Log("OBS system=traffic.handoff vehicle=%08X driver=%08X event=phase slot=%d from=%d to=%d step=%llu",
                    p.vehicle,p.driver,i,int(previous),int(phase),static_cast<unsigned long long>(facts.step));
                if(phase==handoff::Phase::Driving){
                    p.handoffPending=false;
                    SetLife(i,p,spawnflow::Life::Driving,"fresh_physics_after_handoff");
                    Log("OBS system=traffic.handoff vehicle=%08X driver=%08X event=driving slot=%d physics_step=%llu",
                        p.vehicle,p.driver,i,static_cast<unsigned long long>(facts.step));
                }
                authorized=phase==handoff::Phase::Release;
                if(authorized)p.handoff.beforeRelease=facts.step;
            }
            const bool canRelease=p.staged&&authorized&&(roadPose||settledUnderRoute)&&(g_handoffRelease||g_api->releaseBirth);
            if(canRelease)p.releaseAttempted=true;
            if(canRelease&&release(p.vehicle,false)){
                p.staged=false;p.fault=nullptr;SaveIds(i,p);
                // Step 8, Handoff: released; the driving pipeline owns its motion from here (today's start path is the adapter until the
                // Goal Door exists). Spawn never moves it again. Health's spot record notes a good birth here.
                if(p.bornAt&&!p.releaseJudged){p.releaseJudged=true;p.relX=hv.x;p.relY=hv.y;Evidence(i,p,spawnhealth::Stage::Release,true,"released");}
                if(g_handoffFacts)p.handoffPending=true;
                else SetLife(i,p,spawnflow::Life::Driving,"released_to_driving_pipeline");
                if(p.bornAt){
                    Log("Traffic arrival: vehicle %08X released to its pilot %.1f s after its birth (%s pilot)",p.vehicle,(now-p.bornAt)/1000.0,p.armed?"pre-armed":"ordinary");
                    Log("OBS system=traffic.placement vehicle=%08X event=handoff slot=%d driver=%08X model=%08X requested=%.3f,%.3f,%.3f observed=%.3f,%.3f,%.3f",
                        p.vehicle,i,p.driver,p.base,p.birth.x,p.birth.y,p.birth.z,hv.x,hv.y,hv.z);
                }
                p.bornAt=0;
            }
            // The route claim is already running. Let its pilot carry the
            // seated pair from the proving area; synchronous cross-cell
            // MoveReference here deadlocked actor-data/physics jobs in the
            // 2026-10-31 12:44 dump, including while the target was unseen.
            if(p.sending&&!p.staged&&!p.handoffPending&&(!g_api->pilotHolds||engineport::Forward<&FvoTrafficApi::pilotHolds>(g_api,"pilotHolds","generator",0u,p.vehicle))){
                p.sending=false;
                Log("Traffic pool: spot %d: checked pair %08X / %08X flies from its proving area to its route (no relocation)",i,p.vehicle,p.driver);
            }
        }
    }

    // The same ring reach sorts covered placements and rolling path updates. Every
    // candidate receives a rank, including beyond the outermost ring.
    unsigned FleetRing(double distance,const trafficsight::Reach& reach){
        if(distance<=trafficsight::rules::rangeM)return 0;
        const double beyond=distance-trafficsight::rules::rangeM;
        if(!(reach.stepM>0)||!(beyond<=reach.totalM))return unsigned(reach.rings+1);
        return unsigned(std::ceil(beyond/reach.stepM));
    }
    struct FleetViewer {
        Pos prior{};
        ULONGLONG at=0;
        double speed=0;
        trafficsight::Reach Update(const Pos& position,ULONGLONG now,bool playing){
            const auto elapsed=now-at;
            if(!playing||!at||elapsed>1500)speed=0;
            else if(elapsed>=100){
                const double distance=std::hypot(position.x-prior.x,position.y-prior.y);
                speed=distance<=150?std::min(150.0,distance*1000.0/double(elapsed)):0;
            }
            if(!at||!playing||elapsed>=100){prior=position;at=now;}
            g_fleetViewerSpeed.store(speed,std::memory_order_relaxed);
            return trafficsight::ReachOf(speed);
        }
    } g_fleetViewer;

    unsigned FleetPriority(double distance){
        return FleetRing(distance,trafficsight::ReachOf(g_fleetViewerSpeed.load(std::memory_order_relaxed)));
    }

    void ClearFleetAssignments(){
        if(g_fleetRelease)g_fleetRelease(0);
        g_fleetViewer={};
        g_fleetViewerSpeed.store(0,std::memory_order_relaxed);
        g_fleetRefreshAt=0;
    }

    void RefreshFleetPaths(const Pos& player,const trafficsight::Reach& reach,ULONGLONG now){
        if(g_fleetRefreshAt&&now-g_fleetRefreshAt<750)return;
        g_fleetRefreshAt=now;
        struct Candidate { int slot; FvoTrafficRef vehicle; double distance; unsigned ring; };
        std::vector<Candidate> ordered;
        for(int i=0;i<int(g_pairs.size());++i){
            const auto& p=g_pairs[std::size_t(i)];
            if(p.retired||!p.vehicle||!p.driver||p.staged||p.handoffPending||!Driving(p))continue;
            const bool worldOwned=g_api->playerTook&&
                engineport::Forward<&FvoTrafficApi::playerTook>(g_api,"playerTook","generator",p.vehicle,p.vehicle);
            if(worldOwned){g_fleetRelease(p.vehicle);continue;}
            FvoTrafficRef vehicle{};
            if(!Info(p.vehicle,vehicle)||!vehicle.usable||!Aboard(p.driver,p.vehicle))continue;
            const double distance=std::hypot(vehicle.x-player.x,vehicle.y-player.y);
            if(!std::isfinite(distance))continue;
            ordered.push_back({i,vehicle,distance,FleetRing(distance,reach)});
        }
        std::stable_sort(ordered.begin(),ordered.end(),[](const Candidate& a,const Candidate& b){
            if(a.ring!=b.ring)return a.ring<b.ring;
            return a.distance<b.distance;
        });
        std::vector<FvoFleetRailPoint> points;
        points.reserve(64);
        for(const auto& item:ordered){
            const auto& p=g_pairs[std::size_t(item.slot)];
            double speed=0;
            points.clear();
            spawnflow::PopulationRequest request{spawnflow::PopulationAction::FleetPath};
            request.sited=g_fleetPath(p.vehicle,item.vehicle,points,speed)&&points.size()>=2;
            if(!spawnflow::ArbitratePopulation(request).now)continue;
            const FvoFleetRailRequest assignment{p.vehicle,p.driver,points.data(),static_cast<unsigned>(points.size()),speed};
            const int accepted=g_fleetAssign(&assignment);
            observations::Record(g_logFn,now,"traffic.fleet",p.vehicle,unsigned(accepted)|(std::uint64_t(item.ring)<<8),
                "event=path_result driver=%08X accepted=%d ring=%u points=%zu speed=%.1f",p.driver,accepted,item.ring,points.size(),speed);
        }
    }


    // (LandingStock removed 2026-11-07, spawn flow v3: it created vehicles in play outside the Window and was no longer called.)
    void InitialScene(const Snap& snap,const Snap* occupancy,const Pos& pp,ULONGLONG now,int screen,bool panelCover,
        const spawnflow::Window& window,spawnflow::BirthBudget& budget){
        if(!screen)return;
        const bool covered=window.open;  // the spawn window (in play: the live road window, LV-3, or the lab test switch)
        if(covered!=g_wasCovered){g_wasCovered=covered;if(covered){g_passRefused.clear();g_clearanceWaiting.clear();g_tally.Clear();g_madeThisWindow.clear();}
            Log("OBS system=traffic.spawn vehicle=00000000 event=window state=%s screen=%d reset=%d reason=%s",covered?"open":"closed",screen,g_rushFill,window.why);
            // Step 10, Ledger: one line per window.
            // kept = pairs with a vehicle born before this window opened; made = this window's pairs that got a vehicle (card C2: the
            // 2026-11-06 run showed kept=-13 when every create was refused and made still counted the requests).
            if(!covered){int empty=0,kept=0,made=0;for(int k=0;k<std::min(int(g_initialScene.size()),int(g_pairs.size()));++k){const auto& q=g_pairs[k];
                    if(q.vehicle){if(g_madeThisWindow.count(k))++made;else ++kept;}else if(SlotFree(q))++empty;}
                g_tally.empty=empty;g_tally.kept=kept;g_tally.made=made;char line[600];g_tally.Line(line,sizeof line);Log("OBS system=traffic.ledger vehicle=00000000 event=window %s",line);}}
        if(!(covered&&window.cover)){
            if(g_coverPassDone)g_passRefused.clear();
            g_coverPassDone=false;
        }  // the next covered window runs the pass again (never in play)
        if(covered&&window.cover&&!g_coverPassDone)CoverPass(snap,now);
        RetireRequestsTick(now);  // R1
        DeleteTick(now);
        if(g_initialClockPending){if(g_api->arrivalProgress)engineport::Forward<&FvoTrafficApi::arrivalProgress>(g_api,"arrivalProgress","generator",0u,std::max(1,int(g_initialScene.size())),0);return;}
        const int total=std::min(int(g_initialScene.size()),int(g_pairs.size()));
        // Arrival and X use the same gap-fill path while their cover is active. Tick runs this only with the fleet exports.
        g_coldFilling=screen==3;
        const bool filling=covered&&(window.cover||window.live);
        const auto reach=g_fleetViewer.Update(pp,now,screen==1&&PlayTime());
        if(window.cover||(screen==1&&PlayTime()))RefreshFleetPaths(pp,reach,now);
        // One admission per update at the authored lane position. Pending operations and active model checks are paced
        // separately from the fleet target. The current handoff path budgets only observed readiness.
        int waiting=0,resident=FleetCount();std::set<int> slots;
        for(const auto& q:g_pairs)if(q.proving){
            FvoTrafficRef v{},d{};
            if(!g_handoffFacts&&q.bornAt&&q.staged){++waiting;slots.insert(q.proveSlot);continue;}
            // Throttle work that can actually run now, not the total fleet awaiting distant model loads.
            // A pair past its seat hold only waits for route room; that must not stop other routes filling.
            if(trafficpair::ProveHeld(q.seatedSince,now)){slots.insert(q.proveSlot);continue;}
            // Paused-map disables may retire only after play resumes. These reservations submit no work yet and must
            // not occupy the active-check throttle, otherwise a large reset cannot finish its loading panel.
            if(q.vehiclePending||q.driverPending||
               (Info(q.vehicle,v)&&Info(q.driver,d)&&v.has3D&&d.has3D&&(!g_handoffFacts||HasBody(q.vehicle,snap))))++waiting;
            slots.insert(q.proveSlot);
        }
        // The Arbiter's limits live in spawnflow::rules (one create per tick; 12 being proved at once: 2026-10-31 11:23 run, 79 fully simulated actors).
        const int maxLook=window.cover?spawnflow::rules::maxLookCovered:spawnflow::rules::maxLookPlay,maxWaiting=window.cover?spawnflow::rules::maxWaitingCovered:spawnflow::rules::maxWaitingPlay;
        // Why positions stay empty, every 5 s while filling (site_wait and occupied stay 0 since Siting answers at once; refusals carry the reason).
        static ULONGLONG fillToldAt=0;static int fillSiteWait=0,fillRefused=0,fillOccupied=0;
        if(filling&&now-fillToldAt>=5000){
            int empty=0;for(int k=0;k<total;++k)if(SlotFree(g_pairs[k])&&!g_pairs[k].vehicle)++empty;
            Log("OBS system=traffic.fill vehicle=00000000 event=state empty=%d total=%d waiting=%d max_waiting=%d site_wait=%d site_refused=%d occupied=%d doomed=%d screen=%d",empty,total,waiting,maxWaiting,fillSiteWait,fillRefused,fillOccupied,int(g_doomed.size()),screen);
            fillToldAt=now;fillSiteWait=fillRefused=fillOccupied=0;
        }
        // Spawn flow step 4, Arbiter: spread the first grants along each route. Distance breaks ties between equally ranked slots.
        std::vector<spawnflow::Candidate> order;
        // In play, a direct birth waits out the pause after a refusal.
        const bool live=!window.cover;
        budget.limit=spawnflow::rules::maxCreatePerTick;
        // Covered preparation and live shortfalls use the same physical slot order (the dot-based live fill is removed).
        if(filling){
            for(int i=0;i<total;++i){
                const auto& q=g_pairs[i];
                if(!SlotFree(q)||q.vehicle||q.vehiclePending||q.retired||now<q.at||g_passRefused.count(i))continue;
                const auto distance=std::hypot(g_initialScene[i].x-pp.x,g_initialScene[i].y-pp.y);
                // In play only replace a real shortfall within the existing native
                // service envelope. Ring order does not reject any candidate.
                if(live&&distance>spawnflow::rules::liveRadius)continue;
                order.push_back({i,distance,FleetRing(distance,reach)});
            }
        }
        spawnflow::Order(order);
        for(int k=0,looked=0;k<int(order.size())&&looked<maxLook&&spawnflow::MayCreate(window,budget.granted,waiting,budget.limit);++k){const int i=order[std::size_t(k)].slot;auto& p=g_pairs[i];
            g_initialRejected.erase(i);
            ++looked;g_initialCursor=(i+1)%total;
            const int attempt=g_initialAttempt[i];const auto alternatives=g_initialAlternatives.find(i);
            auto at=attempt&&alternatives!=g_initialAlternatives.end()?alternatives->second[std::size_t(attempt-1)]:g_initialScene[i];
            // Hidden traffic waiting for its delete takes no fleet place (13:34 run: 186 old refs held the whole window).
            spawnflow::PopulationRequest request{spawnflow::PopulationAction::FleetBirth,window};
            request.profileHasRoom=trafficadmission::Capacity(resident,0,0,total);
            // The core proves the exact road footprint even if unrelated cells exceed the census buffer.
            request.worldComplete=occupancy&&(occupancy->size()<kSnapMax||g_roadCheck);
            if(!request.profileHasRoom||!request.worldComplete){
                const char* why=request.profileHasRoom?"world_vehicle_list_incomplete":"traffic_profile_full";
                observations::Record(g_logFn,now,"traffic.birth",0,request.profileHasRoom?1u:2u,
                    "event=deferred speaker=arbiter reason=%s resident=%d target=%d world_vehicles=%zu",why,resident,total,occupancy?occupancy->size():0);
                break;
            }
            if(!trafficadmission::ValidOrigin(at.x,at.y)) { g_initialRejected.insert(i);p.at=now+5000;g_tally.Refuse("invalid_origin");continue; }
            // SR-3 Demand: never make a pair its route has no trip room for (it would sit seated and never be released). Room's own
            // counts (route side), less the pairs of this route still being proved here.
            if(g_routeRoom){int on=-1,want=-1;const char* route=nullptr;const int left=g_routeRoom(i,&on,&want,&route);
                if(left>=0&&route){int waitingSame=0;
                    for(int k=0;k<total;++k){const auto& q=g_pairs[std::size_t(k)];if(k==i||(!q.vehicle&&!q.vehiclePending)||q.retired||!q.proving)continue;
                        const char* rk=nullptr;g_routeRoom(k,nullptr,nullptr,&rk);if(rk&&!std::strcmp(rk,route))++waitingSame;}
                    if(!spawnflow::RouteHasRoom(left,waitingSame)){g_initialRejected.insert(i);p.at=now+5000;g_tally.Refuse("route_has_no_trip_room");
                        static ULONGLONG toldAt=0;if(now-toldAt>=5000){toldAt=now;Log("OBS system=traffic.spawn vehicle=00000000 event=demand_skip slot=%d reason=route_has_no_trip_room route=%s on=%d want=%d proving_same_route=%d",i,route,on,want,waitingSame);}
                        continue;}}}
            // Step 3, Demand: the slot's model (your Types first, Health second; VehicleBase).
            if(g_modelUnavailable.erase(i))g_fitRejected.erase(i);
            unsigned base=VehicleBase(i);
            if(!base){g_modelUnavailable.insert(i);g_initialRejected.insert(i);p.at=now+5000;g_tally.Refuse("no_model");continue;}
            g_modelUnavailable.erase(i);
            bool hullKnown=Measure(p,base);  // Model Bounds (source)
            if(hullKnown&&g_health.Fingerprint(base,p.radius,p.halfHeight))Log("OBS system=traffic.health event=model_record_reset model=%08X reason=bounds_changed_id_reused radius=%.2f half_height=%.2f",base,p.radius,p.halfHeight);
            if(g_modelGate&&!g_modelGate(i,base,p.radius,p.halfHeight,hullKnown)){
                g_fitRejected[i].insert(base);
                Log("OBS system=traffic.admission vehicle=00000000 event=refused slot=%d model=%08X reason=lane_model_fit_or_rule",i,base);
                // A slot never stays empty because of one model alone: the next pool model that fits this lane is made instead (2026-11-21: this
                // fallback once served only the removed dot-world grants, so the covered fill gave up on the first model that did not fit).
                unsigned fitted=0;
                {
                    const unsigned refused=base;const char* w="";
                    for(int tries=0;tries<int(TypePool().size())+1&&!fitted;++tries){
                        const unsigned alt=FallbackModelFor(i,0u,&w);if(!alt||alt==refused||SlotSkips(i,alt))break;
                        const bool known=Measure(p,alt);
                        if(!g_modelGate||g_modelGate(i,alt,p.radius,p.halfHeight,known)){fitted=alt;hullKnown=known;}
                        else{g_fitRejected[i].insert(alt);Log("OBS system=traffic.admission vehicle=00000000 event=refused slot=%d model=%08X reason=lane_model_fit_or_rule",i,alt);}
                    }
                    if(fitted){Log("OBS system=traffic.admission vehicle=00000000 event=model_changed slot=%d from=%08X to=%08X reason=lane_model_fit_or_rule",i,refused,fitted);base=fitted;}
                }
                if(!fitted){
                    p.at=now+100;g_tally.Refuse("lane_model_fit_or_rule");
                    continue;
                }
            }
            // The core checks concealment at creation; its refusal reason is logged in the add-on's own line.
            // Step 5, Siting: one pose or one reason, from the field box, the model's bounds, other vehicles and Health's spot verdict only.
            // No navmesh, no World levels, no site rays (the user, 2026-11-06). A refusal tries the slot's next authored alternative; none
            // left: empty until the next window.
            const std::string spotKey=i<int(g_spotKeys.size())?g_spotKeys[std::size_t(i)]:std::string();
            bool rejectedSpot=!spotKey.empty()&&g_health.SpotRejected(spotKey);
            if(rejectedSpot&&g_health.SpotTrial(spotKey)){rejectedSpot=false;Log("OBS system=traffic.health event=spot_trial slot=%d spot=%s reason=retrial_due",i,spotKey.c_str());}
            const char* why="no_field_provider";
            int site=0;
            RoadSite road;double gap=-1;
            FvoRoadProofV1 clearance{};
            if(g_siteGate){
                FvoPoolSpot pose=at;site=g_siteGate(i,attempt,base,p.radius,p.halfHeight,pose,false,rejectedSpot,road,&why);
                if(site==1){
                    if(const auto spacing=Occupied(i,pose,*occupancy,road,gap)){site=0;why=spacing;}
                }
                if(site==1&&g_roadCheck){
                    const FvoRoadSiteV1 envelope{pose.x,pose.y,pose.z,pose.heading,p.radius,p.halfHeight};
                    const auto result=g_roadCheck(&envelope,&clearance);
                    if(result==FvoRoadClearance::Unknown||(result==FvoRoadClearance::Clear&&!clearance.epoch)){
                        ++fillSiteWait;p.at=now+250;
                        g_clearanceWaiting.insert(i);
                        if(alternatives!=g_initialAlternatives.end()&&!alternatives->second.empty())
                            g_initialAttempt[i]=(attempt+1)%(int(alternatives->second.size())+1);
                        observations::Record(g_logFn,now,"traffic.birth",0,3u,
                            "event=deferred speaker=arbiter reason=road_clearance_unknown slot=%d at=%.2f,%.2f,%.2f",i,pose.x,pose.y,pose.z);
                        continue;
                    }
                    if(result==FvoRoadClearance::Occupied){site=0;why="road_clearance_occupied";++fillOccupied;}
                }
                if(site==1)at=pose;
            }
            if(site!=1){
                
                ++fillRefused;g_tally.Refuse(why);
                Log("OBS system=traffic.spawn vehicle=00000000 event=refused slot=%d model=%08X at=%.2f,%.2f,%.2f attempt=%d reason=%s",i,base,at.x,at.y,at.z,attempt,why);
                if(gap>=0)Log("OBS system=traffic.spawn event=road_gap_refused slot=%d station_m=%.1f conflict_gap_m=%.1f nominal_interval_m=%.1f",i,road.station,gap,road.interval);
                if(alternatives!=g_initialAlternatives.end()&&attempt<int(alternatives->second.size())){++g_initialAttempt[i];p.at=now+100;}
                else if(g_clearanceWaiting.count(i)){g_initialAttempt[i]=0;p.at=now+250;}
                else {g_initialAttempt[i]=0;if(window.cover)g_passRefused.insert(i);g_initialRejected.insert(i);p.at=now+(window.cover?5000:10000);}  // LV-3: a covered window asks again at its next opening; play asks again in 10 s (a car that blocked the spot will have moved)
                continue;
            }
            g_clearanceWaiting.erase(i);
            Log("OBS system=traffic.spawn vehicle=00000000 event=sited slot=%d model=%08X at=%.2f,%.2f,%.2f heading=%.3f reason=%s",i,base,at.x,at.y,at.z,at.heading,why);
            if(road.interval>0)Log("OBS system=traffic.spawn event=road_station slot=%d station_m=%.1f nominal_interval_m=%.1f coverage_rank=%u attempt=%d",i,road.station,road.interval,order[std::size_t(k)].coverage,attempt);
            int slot=0;while(slots.count(slot))++slot;
            // Step 6, Birth reads Siting's pose (card C1) through the core's birth listener (required with the fleet exports).
            request.sited=site==1;
            const auto grant=SpeakBirth(budget,request,FvoBirthSource::Fleet,i,base,0,at);
            if(!grant)break;
            if(!window.cover){
                Log("OBS system=traffic.live vehicle=00000000 event=grant slot=%d model=%08X distance=%.0f resident=%d cap=%d at=%.2f,%.2f,%.2f reason=%s",
                    i,base,std::hypot(at.x-pp.x,at.y-pp.y),resident,total,at.x,at.y,at.z,"physical_fleet_shortfall");}
            p.birthGrant=grant;
            const auto birthWindow=BirthWindow(screen,window,FvoBirthSource::Fleet);
            p.vehicleIssued=false;
            const auto reserved=p.reserved;const auto previousBirth=p.birth;
            p.birth=at;p.reserved=true;
            const auto reply=ListenBirth(grant,FvoBirthSource::Fleet,birthWindow,i,base,0,at,p.vehicleIssued,clearance.epoch?&clearance:nullptr);
            p.vehicle=reply.id;
            if(!p.vehicle&&!reply.pending){
                p.reserved=reserved;p.birth=previousBirth;
                g_initialRejected.insert(i);p.at=now+5000;
                
                Log("Traffic arrival: slot %d: creation not completed; still queued for the requested fleet, retry in 5 s",i);
                if(!window.cover){FvoBirthRefusalV1 r;const bool known=CoreRefusal(grant,i,r);LogRefusal("request",i,base,grant,known?&r:nullptr);}
                continue;
            }
            p.base=base;p.birth=at;p.reserved=p.staged=true;p.at=now;p.bornAt=now;p.armed=false;p.vehiclePending=!p.vehicle;g_madeThisWindow.insert(i);
            p.roadProof=clearance;
            p.spotKey=spotKey;p.bothLoadedOnce=p.brokenEvidenceJudged=p.bodyJudged=p.releaseAttempted=p.releaseJudged=p.driveJudged=false;
            p.seatJudged=p.seatTested=false;p.seatPlayMs=p.seatCheckAt=0;
            p.hPosAt=0;p.uncontrolledFall=false;p.fallRetirementPending=false;
            p.proving=true;p.proveSlot=slot;p.proveSince=p.proveTick=now;p.seatedSince=0;slots.insert(slot);++waiting;
            g_createdAt=now;
            p.driverSource=FvoBirthSource::Fleet;p.driverAt=at;p.driverIssued=false;
            const auto driver=p.vehicle?ListenBirth(grant,p.driverSource,birthWindow,i,kDriverBase,p.vehicle,p.driverAt,p.driverIssued):BirthReply{};
            p.driver=driver.id;p.driverPending=p.vehiclePending||driver.pending;
            p.st=(p.driver||p.vehiclePending||p.driverPending)?St::Created:St::Failed;
            SaveIds(i,p);if(!p.vehiclePending)MakeTrafficPair(i,p);++resident;
            SetLife(i,p,spawnflow::Life::Born,"sited_and_made");SetLife(i,p,spawnflow::Life::Proving,"frozen_until_seated");++g_tally.made;
            Log("OBS system=traffic.birth event=%s slot=%d vehicle=%08X driver=%08X stage=proving at=%.2f,%.2f,%.2f",
                p.vehicle?"completed":"queued",i,p.vehicle,p.driver,at.x,at.y,at.z);
            ++g_rushMade;
        }
        // The live fill's state, every 5 s while the live window is open.
        if(live&&filling&&now-g_liveToldAt>=5000){
            g_liveToldAt=now;
            // The cap is the one the Arbiter applies above (FleetTarget(true, total) = every slot), not the old 32-car live cap.
            Log("OBS system=traffic.live vehicle=00000000 event=state resident=%d cap=%d waiting=%d waiting_cap=%d",resident,total,waiting,spawnflow::rules::maxWaitingPlay);
        }
        // Progress for the loading-screen log line: route positions that hold a vehicle with a living driver (placed now,
        // or kept from an earlier visit). Staged ones stay frozen until their autopilot takes them.
        int placed=0;
        for(int i=0;i<total;++i){const auto& p=g_pairs[i];FvoTrafficRef v{},d{};
            if(Info(p.vehicle,v)&&Info(p.driver,d)&&!d.dead)++placed;
        }
        (void)pp;
        if(g_api->arrivalProgress)engineport::Forward<&FvoTrafficApi::arrivalProgress>(g_api,"arrivalProgress","generator",0u,total,placed);
        // Start/Swap: the editor's loading panel stays up while positions are still being filled under the map.
        if(g_rushFill){
            const bool underCover=screen>=2&&panelCover;
            const char* holdWhy="";const int unproven=underCover?UnprovenUnderPanel(&holdWhy):0;  // CC-4: Health's output
            const auto elapsed=now-g_rushAt.load();
            const bool work=underCover&&(InitialAdmissionsPending()||unproven>0);
            const bool nearbyPilot=underCover&&!work&&std::any_of(g_pairs.begin(),g_pairs.end(),[&](const Pair& p){
                if(!p.vehicle||p.retired||(!p.staged&&!p.handoffPending))return false;
                return std::any_of(snap.begin(),snap.end(),[&](const FvoTrafficVehicle& v){
                    return p.vehicle==v.id&&v.hasBody&&std::hypot(v.x-pp.x,v.y-pp.y)<kDriver3DNear;
                });
            });
            if(work||!nearbyPilot)g_rushSettlingAt=0;
            else if(!g_rushSettlingAt)g_rushSettlingAt=now;
            const auto phase=spawnrule::DecidePreparation(underCover,work,
                g_rushSettlingAt&&now-g_rushSettlingAt<spawnrule::kSettleLimitMs,elapsed);
            const bool pending=spawnrule::Active(phase);
            {static ULONGLONG holdLog=0;if(pending&&unproven&&now-holdLog>=2000){holdLog=now;
                Log("OBS system=traffic.spawn vehicle=00000000 event=x_panel_held unproven=%d first=%s elapsed_ms=%llu limit_ms=%llu",unproven,holdWhy,elapsed,spawnrule::kResetLimitMs);}}
            if(!pending&&unproven>0)CoverClosing(snap,now,2);  // CC-4: settled by Health and Retirement while the panel still covers
            std::lock_guard lock(g_rushLock);
            g_rush.total=total;g_rush.placed=placed;g_rush.made=g_rushMade;g_rush.skipped=int(g_initialRejected.size());
            g_rush.driving=0;
            for(const auto& p:g_pairs)if(p.vehicle&&(!g_handoffFacts||(!p.staged&&!p.handoffPending&&Driving(p)))&&
                engineport::Forward<&FvoTrafficApi::usesVehicle>(g_api,"usesVehicle","generator",0u,p.vehicle))++g_rush.driving;
            g_rush.active=pending?1:0;g_rush.phase=int(phase);
            if(!pending){g_rushFill=false;
                Log("OBS system=traffic.window event=x_panel_closed phase=%d screen=%d panel=%d elapsed_ms=%llu",int(phase),screen,int(panelCover),elapsed);
                Log("Traffic preparation: %d of %d route positions hold vehicles, %d created under the panel, %d deferred",
                    placed,total,g_rushMade,g_rush.skipped);}
        }
    }
    void Reset(){
        ClearFleetAssignments();
        ++observations::epoch;
        std::scoped_lock lock(g_lock,g_rushLock);
        g_world.clear();g_spots.clear();g_reuse.clear();g_names.clear();g_pairs.clear();g_worldPools.clear();g_observedOrigins.clear();g_observeCursor=0;
        g_rush={};g_rushRequest=false;g_rushAt=0;g_rushSettlingAt=0;g_rushFill=false;g_rushMade=0;g_coverPassDone=false;g_rosterRecoveryPending=true;g_spares.clear();g_sparesWorld.clear();g_retiringWorld.clear();g_removeQueue.clear();g_parkedGrants.clear();  /* R1: ids are reused across saves */
        g_wantWorld.clear();g_wantSpots.clear();g_wantReuse.clear();g_wantNames.clear();
        g_off=false;g_createdAt=g_lastTick=0;g_admission=-1;g_cursor=0;
        g_path={};g_coverEndedAt=g_zoneReportAt=0;g_zoneLostChecks=0;g_zoneRadius=0;
        g_initialScene.clear();g_initialRoads.clear();g_initialRejected.clear();g_initialAlternatives.clear();g_initialAttempt.clear();g_initialFails.clear();g_initialCursor=0;g_initialMap=false;
        g_initialClockPending=false;g_deferredRegistry.clear();g_registryRecoveryAt=0;g_linkedRecoveryAt=0;g_registryRecoveryCursor=0;g_takenRelease.clear();g_handoffCursor=0;
        g_passRefused.clear();g_clearanceWaiting.clear();g_wasCovered=false;g_coldFilling=false;g_landings.clear();g_landingAt=0;g_doomed.clear();g_retiring.clear();
        HealthSave(GetTickCount64(),true);g_tally.Clear();  // card HR2: Health's records last across loads (no Clear)
        // These clears were inside the comment above and never ran (found 2026-11-21): the model rules come back with the
        // pool resend that ResetAll requests, and the inspection list must not show the previous save's pairs.
        g_slotSkip.clear();g_fitRejected.clear();g_modelUnavailable.clear();populationinspect::Publish({});
    }
    void Rush(const FvoRushPlace*,int){
        std::lock_guard lock(g_rushLock);
        g_rush={};g_rush.phase=int(spawnrule::Preparation::Filling);g_rush.active=1;
        g_rushAt=GetTickCount64();g_rushRequest.store(true);
    }
    void SetLandingSpots(const FvoPoolSpot* spots,const char* const* names,int count){
        g_landings.clear();
        for(int i=0;spots&&i<count;++i){Landing l;l.at=spots[i];l.key=names&&names[i]?names[i]:std::to_string(i);g_landings.push_back(l);}
    }
    void SetSavedFleetJourney(bool (*matches)(unsigned,unsigned,const char*)){g_savedFleetJourney=matches;}
    void SetPanelCover(int (*covered)()){g_panelCover=covered;}
    void RushStatus(FvoRushState* out){
        if(!out)return;
        std::lock_guard lock(g_rushLock);*out=g_rush;
        const auto start=g_rushAt.load();
        // A stalled world/profile tick cannot leave the editor trapped under cover.
        // Normal ticks publish their terminal result after the final covered Health pass.
        if(out->active&&start&&GetTickCount64()-start>=spawnrule::kResetLimitMs){
            out->active=0;out->phase=int(spawnrule::Preparation::TimedOut);
        }
    }
    void Housekeeping(){if(g_api)Tick();}
    // CC-1 -> CC-2/CC-3: the core's cover owner reports the loading screen's close frame (the cover still holds on it). Same checks as
    // Tick (traffic on, the world this pool belongs to); then Health judges with the closing fact and Retirement removes under cover.
    void SetTripReaders(const char* (*refusal)(unsigned,int*,int*),int (*routeRoom)(int,int*,int*,const char**)){g_tripRefusal=refusal;g_routeRoom=routeRoom;}
    bool ReadVehicles(Snap& snap,bool includeHeld=false){
        snap.resize(kSnapMax);
        const int n=includeHeld&&g_roadVehicles?g_roadVehicles(snap.data(),kSnapMax):
            engineport::Forward<&FvoTrafficApi::vehicles>(g_api,"vehicles","generator",0u,snap.data(),kSnapMax);
        if(n<0)return false;
        snap.resize(std::size_t(std::min(n,kSnapMax)));
        return true;
    }
    int CoverClosingNow(int kind){
        if(!g_api||g_off||!engineport::Forward<&FvoTrafficApi::trafficOn>(g_api,"trafficOn","generator",0u)||!g_api->presentation)return -1;
        Pos pp;std::string name;if(!PlayerNow(pp,name))return -1;
        {std::lock_guard lock(g_lock);if(g_wantWorld!=name||g_world!=name)return -1;}
        Snap snap;if(!ReadVehicles(snap))return -1;
        const auto now=GetTickCount64();g_clock=now;
        return CoverClosing(snap,now,kind);
    }
    void Tick(){
        // 2026-11-21: the dot-based live fill is removed. A core without the fleet rail exports gets no population at all,
        // so two population systems can never run against each other.
        if(!PhysicalFleetActive()){static bool told=false;if(!told){told=true;Log("OBS system=traffic.fleet event=refused reason=core_without_fleet_rail_exports effect=no_population");}return;}
        // Every core with the fleet exports also has the birth listener and road clearance; their older-core fallbacks are gone,
        // so a core missing one is refused the same way instead of running a second birth path.
        if(!g_birthListener||!g_roadCheck||!g_handoffFacts||!g_handoffRelease){static bool told=false;if(!told){told=true;
            Log("OBS system=traffic.fleet event=refused reason=core_without_required_export missing=%s effect=no_population",
                !g_birthListener?"FVO_TrafficBirthRequestV1":!g_roadCheck?"FVO_TrafficRoadVehiclesV1_FVO_TrafficRoadCheckV1_FVO_TrafficBirthRequestV2":"FVO_TrafficHandoffFactsV1_FVO_TrafficHandoffReleaseV1");}return;}
        if(!g_api||g_off||!engineport::Forward<&FvoTrafficApi::trafficOn>(g_api,"trafficOn","generator",0u)||!g_api->presentation||!engineport::Forward<&FvoTrafficApi::presentation>(g_api,"presentation","generator",0u))return;
        Pos pp;std::string name;if(!PlayerNow(pp,name))return;
        HealthLoad();
        {std::lock_guard lock(g_lock);if(g_wantWorld!=name)return;}
        if(g_world!=name){
            ClearFleetAssignments();
            g_parkedGrants.clear();
            g_admission=-1;g_lastTick=0;g_lastCoveredAt=GetTickCount64();
            if(!g_world.empty())g_worldPools[g_world]={g_spots,g_reuse,g_names,g_pairs};
            g_world=name;auto it=g_worldPools.find(name);
            if(it!=g_worldPools.end()){g_spots=it->second.spots;g_reuse=it->second.reuse;g_names=it->second.names;g_pairs=it->second.pairs;}
            else {g_spots.clear();g_reuse.clear();g_names.clear();g_pairs.clear();}
            Reconcile();RecoverRegistry();g_rosterRecoveryPending=true;
        }else if(g_appliedRevision!=g_revision)Reconcile();

        RushStep();
        const auto now=GetTickCount64();
        if(g_lastTick&&now-g_lastTick>1000){const auto gap=now-g_lastTick;
            for(auto& p:g_pairs)for(auto* t:{&p.at,&p.seatAt,&p.tripAt,&p.watchAt,&p.idleSince,&p.askAt,&p.busySince,&p.no3DSince,&p.noBodySince})if(*t)*t+=gap;
        }g_lastTick=now;
        Snap snap;if(!ReadVehicles(snap))return;
        const int presentation=engineport::Forward<&FvoTrafficApi::presentation>(g_api,"presentation","generator",0u);
        RecoverSavedFleet(now,presentation);
        // Remember observable origins independently of which fleet member is
        // waiting there; a new slot/route must not erase the player's knowledge.
        if(presentation==1&&g_api->exposure&&!g_spots.empty())for(int count=0;count<4;++count){
            const auto& origin=g_spots[g_observeCursor++%g_spots.size()];
            if(SightAt(origin.x,origin.y,origin.z).Exposure()==1)g_observedOrigins[g_world].insert(OriginKey(origin));  // V1
        }
        // One candidate's physical queries at a time: no fleet burst, queue flood,
        // or concurrent births hidden from the snapshot.
        if(g_admission<0||g_admission>=int(g_pairs.size())||!SlotFree(g_pairs[g_admission])||now-g_admissionAt>5000){
            g_admission=-1;
            for(std::size_t j=0;j<g_pairs.size();++j){const auto i=(g_cursor+j)%g_pairs.size();
                if(SlotFree(g_pairs[i])&&!g_pairs[i].retired){g_admission=int(i);g_cursor=(i+1)%g_pairs.size();g_admissionAt=now;break;}}
        }
        const bool panelCover=presentation!=2||!g_rushFill||XPanelCovered();
        const bool preparing=g_rushFill&&GetTickCount64()-g_rushAt.load()<spawnrule::kResetLimitMs;
        // LV-3: the play rule (spawnrule::LiveOpen) after any cover or map has been gone long enough; the lab test switch is its own, separate window flag.
        if(presentation!=1||!g_lastCoveredAt)g_lastCoveredAt=now;
        const bool liveRoad=spawnrule::LiveOpen(presentation,false,now-g_lastCoveredAt);
        const auto window=spawnflow::WindowNow(presentation,preparing,false,liveRoad,panelCover);
        {   static bool liveWasOpen=false;
            if(window.live!=liveWasOpen){liveWasOpen=window.live;Log("OBS system=traffic.live vehicle=00000000 event=window state=%s settle_ms=%llu fleet_cap=%d stretch_m=%.0f demand=%s",window.live?"open":"closed",spawnrule::kLiveSettleMs,int(g_initialScene.size()),spawnflow::rules::liveRadius,"physical_shortfall");}
        }
        spawnflow::BirthBudget budget;
        PrioritizeHealth(snap,now,presentation);
        for(int i=0;i<int(g_pairs.size());++i)Step(i,g_pairs[i],g_spots[i],pp,snap,now,presentation,window,budget);
        ServiceHandovers(snap,now);
        Snap held;
        const Snap* occupancy=&snap;
        if(g_roadVehicles&&spawnflow::MayCreate(window,budget.granted,0))
            occupancy=ReadVehicles(held,true)?&held:nullptr;
        LARGE_INTEGER sceneFrom{},sceneTo{},sceneHz{};QueryPerformanceFrequency(&sceneHz);QueryPerformanceCounter(&sceneFrom);
        InitialScene(snap,occupancy,pp,now,presentation,panelCover,window,budget);
        QueryPerformanceCounter(&sceneTo);
        if(window.live&&sceneHz.QuadPart){  // LV-3 cost: the fill's own time per tick, so the next run measures what a live fill costs the frame
            static double worst=0,total=0;static int ticks=0;static ULONGLONG toldAt=0;
            const double ms=1000.0*double(sceneTo.QuadPart-sceneFrom.QuadPart)/double(sceneHz.QuadPart);
            worst=std::max(worst,ms);total+=ms;++ticks;
            if(now-toldAt>=10000){toldAt=now;Log("OBS system=traffic.live vehicle=00000000 event=tick_cost ticks=%d average_ms=%.3f worst_ms=%.3f",ticks,total/std::max(1,ticks),worst);worst=total=0;ticks=0;}
        }
        std::vector<FvoPopulationInspectV1> inspected;inspected.reserve(g_pairs.size());
        for(std::size_t i=0;i<g_pairs.size();++i){const auto& pair=g_pairs[i];FvoPopulationInspectV1 row;row.vehicle=pair.vehicle;row.driver=pair.driver;row.state=int(pair.st);row.retired=pair.retired;row.staged=pair.staged;row.armed=pair.armed;row.vehiclePending=pair.vehiclePending;row.driverPending=pair.driverPending;row.at=now;
            std::snprintf(row.name,sizeof row.name,"%s",i<g_names.size()?g_names[i].c_str():"unnamed slot");
            const int fleetMode=pair.vehicle&&g_fleetState?g_fleetState(pair.vehicle):0;
            const char* reason=fleetMode==2?"active journey; rail owns motion":fleetMode==3?"active journey; awaiting engine evidence":pair.retired?"retiring existing identity":pair.vehiclePending?"vehicle creation pending":pair.driverPending?"driver creation pending":pair.noBodySince?"waiting for physics body":pair.no3DSince?"waiting for driver model":pair.proving?"existing readiness validation":pair.handoffPending?"handoff proof pending":pair.sending?"existing departure pending":pair.staged?"staged; awaiting pilot handover":pair.st==St::Started?"active journey":pair.st==St::Ready?"ready; awaiting route assignment":pair.st==St::Created?"pair created; awaiting readiness":pair.st==St::Failed?"existing recovery pending":pair.st==St::Empty?"empty; awaiting admission":"reconciling saved identity";
            std::snprintf(row.reason,sizeof row.reason,"%s",reason);inspected.push_back(row);
        }
        populationinspect::Publish(std::move(inspected));
        HealthPublish();HealthSave(now,false);
    }
}
