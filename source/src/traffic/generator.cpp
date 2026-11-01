// FVO Traffic: the Generator. Moved from core's trafficpool.cpp on 2026-10-29 (stage 1 of
// notes/TRAFFIC_ADDON_PROPOSAL_20261029.md) with the same rules, timings and "Traffic pool:" log lines; every engine step goes
// through core's API v2 (traffic_api.h).
//
// The spawn and seat steps are the ones confirmed in game on 2026-10-25 (notes/road-traffic-20261025/RESULT.md, run 02:05):
// the game's create-reference for the vehicle and the driver (the driver is seated inside its creation), then, once both have
// 3D, the driver must really sit (sit state > 0), else the instant seat every second, six tries at most.
#include "generator.h"
#include "admission.h"
#include "observations.h"
#include "fvolimits.h"
#include "fvopaths.h"
#include "trafficpair.h"
#include "trustzone.h"

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

		constexpr unsigned kDriverBase = 0x001591EA;  // Starfield.esm LvlCitizen_UC (what the road traffic spawned)
		constexpr unsigned kRev8 = 0xFE001033;        // SFBGS004.esm Rev8, as the addon matches it
		constexpr int kMaxSpots = fvolimits::kVehicles;  // cold-start spots + parking (fvolimits.h)
		constexpr ULONGLONG kCreateGapMs = 2500, kLoadWaitMs = 120000, kSeatEveryMs = 1000, kTripRetryMs = 2000, kWatchMs = 1000;
		constexpr int kRestarts = 5;          // a pair whose trip ended at its spot is started again this often
		constexpr ULONGLONG kIdleMs = 10000;  // ... after standing this long with no trip
		constexpr double kNearSpot = 20.0;    // m: "near its spot" (a driver out elsewhere ended a trip: ordinary traffic now)
		constexpr int kSeatTries = 6;
		constexpr double kSpawnNear = 350.0;  // m: a parking spot's driver only while the spot is this close (its cell is loaded)
		constexpr double kBakedNear = 3.5;    // m: a parked vehicle belongs to the spot it stands this close to
		constexpr int kSnapMax = fvolimits::kVehicles;  // core's vehicle list capacity

		std::mutex g_lock;
		std::string g_wantWorld;
		std::vector<FvoPoolSpot> g_wantSpots;
		std::vector<int> g_wantReuse;
		std::vector<std::string> g_wantNames;
		FvoPoolTrip g_trip = nullptr;
        unsigned long long g_revision=0,g_appliedRevision=~0ull;

		enum class St { Unknown, Empty, Created, Ready, Started, Failed };
		struct Pair {
			St st = St::Unknown;
			unsigned vehicle = 0, driver = 0;
			unsigned base = 0;         // the vehicle's base when the pair was made (0: an entry from before 2026-10-29)
			ULONGLONG at = 0, seatAt = 0, tripAt = 0, watchAt = 0, idleSince = 0, askAt = 0, busySince = 0;
			ULONGLONG refusedAt = 0;   // last "cannot start a trip yet" line (a new reason, or every 10 min for the same one)
			unsigned refusedHash = 0;  // the reason last logged (2026-10-29: far spots waiting parked repeated it every 30 s)
			int tries = 0, restarts = 0;
			ULONGLONG no3DSince = 0, reloadAt = 0;  // the driver's 3D reload (see Started)
			int reloads = 0;
            bool observed = false, retired = false, routeWaitTold = false;
            FvoPoolSpot birth{}; bool reserved = false;
            bool staged = false;
            bool armed = false; ULONGLONG bornAt = 0, fallCheckAt = 0;
            ULONGLONG frozenAt = 0, rearmAt = 0; int rearms = 0; bool setDown = false; // fall guard pilot recovery
            ULONGLONG noBodySince = 0; // staged with a model but no physics body (its cell left the physics world)
            ULONGLONG badSince = 0, badAt = 0; // a pair that will not recover (trafficpair::JudgePair): when it was first seen, last check
            trustzone::Record zone; ULONGLONG zoneAt = 0, noPhysicsSince = 0; // its place in the trust zone; since when its pilot gets no physics steps (ZoneTick)
            ULONGLONG floatingSince = 0, floatingAskAt = 0; bool broughtTold = false; // the driver seen with no vehicle around them (see Created)
            ULONGLONG precheckAt = 0; bool precheckTold = false; // see PrecheckPair
            bool fromSpare = false;      // this pair already came off the shelf: if it fails again it is not reused
            bool seatJudged = false; ULONGLONG both3DSince = 0;          // per-model seat check (did its driver ever sit?)
            bool spotChecked = false; ULONGLONG spotAt = 0;              // post-load spot check of a placed vehicle // pre-armed pilot at birth; when the vehicle was made (creation-to-autopilot log)
            double radius=8,halfHeight=6;
            bool vehiclePending=false,driverPending=false; int relocating=0;
            bool proving = false, sending = false; int proveSlot = -1;   // checked over the player before it joins traffic (trafficpair::ProveAt); then sent to birth
            ULONGLONG proveSince = 0, proveTick = 0, seatedSince = 0, sendSince = 0, remoteTryAt = 0;
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
        bool g_off = false;
        struct WorldPool {std::vector<FvoPoolSpot> spots;std::vector<int> reuse;std::vector<std::string> names;std::vector<Pair> pairs;};
        std::map<std::string,WorldPool> g_worldPools;
        std::map<std::string,std::set<std::pair<long long,long long>>> g_observedOrigins;
        std::size_t g_observeCursor=0;
        auto OriginKey(const FvoPoolSpot& s){return std::pair{std::llround(s.x/4),std::llround(s.y/4)};}
        int g_repair=-1;std::size_t g_repairOrigin=0;ULONGLONG g_repairAt=0;
        int g_admission = -1; std::size_t g_cursor = 0; ULONGLONG g_admissionAt = 0, g_lastTick = 0;
        std::vector<FvoPoolSpot> g_initialScene;std::set<int> g_initialRejected;
        std::map<int,std::vector<FvoPoolSpot>> g_initialAlternatives;std::map<int,int> g_initialAttempt,g_initialFails;int g_initialCursor=0;
        bool g_initialMap=false;
        bool g_initialClockPending=false;

		struct Pos { double x = 0, y = 0, z = 0; };
		using Snap = std::vector<FvoTrafficVehicle>;

		bool Info(unsigned id, FvoTrafficRef& r) { return id && g_api->ref(id, &r) != 0; }
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
		bool Aboard(unsigned driver, unsigned vehicle) { return driver && vehicle && g_api->vehicleOf(driver) == vehicle; }
		bool HasBody(unsigned vehicle,const Snap& snap){return std::any_of(snap.begin(),snap.end(),[&](const auto& v){return v.id==vehicle&&v.hasBody;});}
		// Seated (2026-10-29 22:36 run): the game reports sit state 0 until it next processes the driver; aboard with both models
		// and the driver on the vehicle (under 1 m) is in the seat.
		bool SeatedPair(bool v3D,bool d3D,bool aboard,int sit,double separation){
			return trafficpair::ReadyAt(v3D,d3D,aboard,sit,separation);}  // sitting (sit state 3), not merely linked
		bool Grounded(const Pos& vp){double nav=0,baked=0;const int known=g_api->ground(vp.x,vp.y,vp.z,&nav,&baked);return known&&std::isfinite(known&2?baked:nav)&&std::fabs(vp.z-(known&2?baked:nav))<2.5;}

		// trafficpool.ini [<world>] pair_<name> = vehicle driver: the pairs made for this world in earlier visits (any save; checked).
#ifdef FVO_GENERATOR_TEST
        std::wstring g_testIni;
        const std::wstring& Ini(){return g_testIni;}
#else
        const std::wstring& Ini() { static const std::wstring p = fvopaths::File(L"trafficpool.ini"); return p; }
#endif
		std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
		std::wstring PairKey(int i)
		{
			const bool named = i < int(g_names.size()) && !g_names[std::size_t(i)].empty();
			return named ? L"pair_" + Wide(g_names[std::size_t(i)]) : L"spot" + std::to_wstring(i);
		}
		void LoadIds(int i, Pair& p)
		{
			wchar_t text[64]{};
			GetPrivateProfileStringW(Wide(g_world).c_str(), PairKey(i).c_str(), L"", text, 64, Ini().c_str());
			unsigned v = 0, d = 0, b = 0, staged=0;
			const int n = std::swscanf(text, L"%x %x %x %x", &v, &d, &b,&staged);
			if (n >= 2) { p.vehicle = v; p.driver = d; p.base = n >= 3 ? b : 0;p.staged=n>=4&&staged!=0; }
		}
		void SaveIds(int i, const Pair& p)
		{
			wchar_t text[64];
			std::swprintf(text, 64, L"%08X %08X %08X %X", p.vehicle, p.driver, p.base,p.staged?1:0);
			WritePrivateProfileStringW(Wide(g_world).c_str(), PairKey(i).c_str(), text, Ini().c_str());
		}

        // A landing spot works like a placement: a vehicle waits there, one per spot, once the player is in range, and stays for later. Kept in trafficpool.ini per world; a remembered id is checked by its base.
        struct Landing{FvoPoolSpot at{};std::string key;unsigned vehicle=0,base=0;bool loaded=false,pending=false;FvoPoolSpot requestAt{};};
        std::vector<Landing> g_landings;
        bool g_coldFilling=false;
        ULONGLONG g_landingAt=0;
        void SaveLanding(const Landing& l){
            const std::wstring key=L"landing_"+std::wstring(l.key.begin(),l.key.end()),section(g_world.begin(),g_world.end());
            wchar_t text[64]{};if(l.vehicle)swprintf_s(text,L"%08X %08X",l.vehicle,l.base);
            WritePrivateProfileStringW(section.c_str(),key.c_str(),l.vehicle?text:nullptr,Ini().c_str());
        }
        bool LandingUnseen(double x,double y,double z){
            if(!g_api->presentation)return false;
            if(g_api->presentation()>=2)return true;
            return g_api->exposure&&g_api->exposure(x,y,z)==2;
        }
        bool Concealed(const FvoPoolSpot& s,Pair& p){
            if(!g_api->presentation||!g_api->exposure)return false;
            const int screen=g_api->presentation();if(!screen)return false;
            if(screen==2)return true;
            const int visibility=g_api->exposure(s.x,s.y,s.z);
            if(visibility==1){p.observed=true;g_observedOrigins[g_world].insert(OriginKey(s));}
            if(g_observedOrigins[g_world].count(OriginKey(s)))p.observed=true;
            return !p.observed&&visibility==2;
        }
        void Measure(Pair& p,unsigned base){if(!g_api->hullSize||!g_api->hullSize(base,&p.radius,&p.halfHeight)){p.radius=8;p.halfHeight=6;}}  // no stored bounds: generous default
        bool Occupied(int self,const FvoPoolSpot& at,const Snap& snap){
            const double radius=self>=0&&self<int(g_pairs.size())?g_pairs[self].radius:8;
            const double height=self>=0&&self<int(g_pairs.size())?g_pairs[self].halfHeight:6;
            const unsigned own=self>=0&&self<int(g_pairs.size())?g_pairs[self].vehicle:0;
            for(const auto& v:snap)if(v.id&&v.id!=own&&std::hypot(v.x-at.x,v.y-at.y)<radius+(v.radius>0?v.radius:8)&&std::fabs(v.z-at.z)<height+(v.halfHeight>0?v.halfHeight:6)){
                FvoTrafficRef seen{};if(Info(v.id,seen)&&seen.disabled)continue;  // a cleared pair is not in the way
                return true;
            }
            // Include pending native creations, not just this frame's stale snapshot.
            for(int j=0;j<int(g_pairs.size());++j){if(j==self)continue;const auto& q=g_pairs[std::size_t(j)];
                FvoTrafficRef ref{};const bool live=Info(q.vehicle,ref);
                const auto pos=live&&!q.proving&&!q.sending?FvoPoolSpot{ref.x,ref.y,ref.z,0}:q.birth;  // one being checked holds the place it is going to
                if((live||q.reserved)&&std::hypot(pos.x-at.x,pos.y-at.y)<radius+q.radius&&std::fabs(pos.z-at.z)<height+q.halfHeight)return true;
            }
            return false;
        }

        FvoRushState g_rush{};std::mutex g_rushLock;
        std::atomic<bool> g_rushRequest{false};
        bool g_rushFill=false;int g_rushMade=0; // Start/Swap fill in progress under the editor panel
        bool g_rushClear=false;                // Reset traffic: clear out every pair on the next covered update
        ULONGLONG g_clearUntil=0;              // ... and wait this long at most for the hiding to land
        constexpr ULONGLONG kClearWaitMs=3000;
        void (*g_onCleared)()=nullptr;         // the route side lets go of the cleared trips
        bool g_coverPassDone=false;            // leftover adoption + model re-roll ran under this cover
        // Spares (2026-10-30): traffic hidden under cover (leftovers, re-rolled models) is kept by model and brought back for a
        // position that needs that model, instead of creating another vehicle and driver. Deleting them is not an option: deleting
        // physics references crashed the game (mapguard notes). So the save's traffic stops growing.
        struct Spare{unsigned vehicle=0,driver=0,base=0;};std::vector<Spare> g_spares;std::string g_sparesWorld;
        // Retirement owns these ids until save reset, even after deletion is acknowledged. An orphan scan or stale
        // spare entry must never reclaim a reference while the engine still has deferred work for it.
        std::set<unsigned> g_retiring;
        bool Retiring(unsigned id){return id&&g_retiring.count(id)!=0;}
        void SaveSpares(){
            if(g_world.empty())return;std::wstring list;
            for(const auto& sp:g_spares){wchar_t b[40];swprintf(b,40,L"%s%08X %08X %08X",list.empty()?L"":L", ",sp.vehicle,sp.driver,sp.base);list+=b;}
            WritePrivateProfileStringW(Wide(g_world).c_str(),L"spares",list.c_str(),Ini().c_str());
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
                if(sp.vehicle&&sp.base&&!Retiring(sp.vehicle)&&!Retiring(sp.driver)&&g_api->ref&&g_api->ref(sp.vehicle,&v)&&v.baseId==sp.base&&g_api->ref(sp.driver,&d)&&d.isActor&&!d.dead)g_spares.push_back(sp);
                while(*c==L','||*c==L' ')++c;}
        }
        void AddSpare(unsigned vehicle,unsigned driver,unsigned base){
            if(!vehicle||!driver||!base||Retiring(vehicle)||Retiring(driver))return;LoadSpares();
            for(const auto& sp:g_spares)if(sp.vehicle==vehicle)return;
            g_spares.push_back({vehicle,driver,base});SaveSpares();
        }
        // Only a pair on a trip, and not one just born: a new pair is still waiting for its models, its seat and its pilot.
        // A pair given up on (St::Failed) is bad too: it has no job, so nothing else ever looks at it again.
        trafficpair::Bad PairVerdict(const Pair& p,ULONGLONG now){
            if(p.retired||!p.vehicle||!p.driver||!p.base)return trafficpair::Bad::None;
            FvoTrafficRef v{},d{};if(!Info(p.vehicle,v)||!Info(p.driver,d))return trafficpair::Bad::None;
            if(g_api->pairHealth&&g_api->pairHealth(p.vehicle)==int(trafficpair::PairHealth::Broken))return trafficpair::Bad::Unrecoverable;
            if(p.st==St::Failed)return trafficpair::Bad::GaveUp;
            if(p.st!=St::Started||(p.bornAt&&now-p.bornAt<=10000))return trafficpair::Bad::None;
            if(p.noPhysicsSince&&now-p.noPhysicsSince>=trafficpair::kNoPhysicsFaultMs)return trafficpair::Bad::NoPhysics;
            if(d.disabled&&!p.reloadAt)return trafficpair::Bad::DriverOut;  // the driver watch hid them: nobody is flying this vehicle
            const bool holds=g_api->pilotHolds&&g_api->pilotHolds(p.vehicle),aboard=g_api->vehicleOf(p.driver)==p.vehicle;
            // Got out and standing on the ground: pulled out, a fight, a wreck. That is the world working, not a broken pair (the record says Healthy for it). The vehicle stays.
            if(!aboard&&!d.dead&&g_api->pairHealth&&g_api->pairHealth(p.vehicle)==int(trafficpair::PairHealth::Healthy))return trafficpair::Bad::None;
            const double distance=std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z);
            // A flying driver's reference lags behind the vehicle while the game has them in the seat (2026-10-29 21:27 run): not bad on that alone.
            if(aboard&&distance<=(holds?kFlyingLagReach:8.0)&&trafficpair::Sitting(g_api->sitState(p.driver)))return trafficpair::Bad::None;
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
            if (apart <= trafficpair::kSnapReach || g_api->usesVehicle(p.vehicle)) return;  // a pilot's job keeps its own driver on the vehicle
            // Never the mover (2026-10-31 11:02 run: 35 of these through moveTo, 18 pairs then failed with the seat link gone; FUN_142062F50 releases the seat).
            if (g_api->holdDriver) {
                g_api->holdDriver(p.driver, p.vehicle);
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
        struct Doomed{unsigned id;ULONGLONG at;bool submitted=false;unsigned baseId=0;ULONGLONG retryAt=0,missingSince=0;int refused=0;};
        std::vector<Doomed> g_doomed;
        // A failed lookup is not proof the reference is gone (2026-10-31 18:59 run: FF016B4E read as gone at 19:02:53, built again at 19:04:23 and stayed parked with no driver).
        constexpr ULONGLONG kDoomedMissingMs=300000,kDeleteRetryMs=5000,kDeleteRetryMaxMs=60000;
        constexpr int kDeleteGiveUp=6;
        ULONGLONG g_clock=0;  // this update's time
        void DeletePair(unsigned vehicle,unsigned driver){
            Log("OBS system=traffic.retirement vehicle=%08X driver=%08X event=request already_retiring=%d",vehicle,driver,Retiring(vehicle));
            if(vehicle&&g_api->usesVehicle(vehicle))g_api->endJob(vehicle,"traffic: unhealthy pair deleted");
            const auto oldSize=g_spares.size();
            g_spares.erase(std::remove_if(g_spares.begin(),g_spares.end(),[&](const Spare& sp){return sp.vehicle==vehicle||sp.driver==driver||sp.driver==vehicle||sp.vehicle==driver;}),g_spares.end());
            for(const unsigned id:{vehicle,driver})if(id){
                if(g_api->show)g_api->show(id,0); // preserve the retired state if an old enable completes late
                if(g_retiring.insert(id).second){
                    FvoTrafficRef known{};Info(id,known);
                    g_doomed.push_back({id,g_clock});g_doomed.back().baseId=known.baseId;
                    Log("OBS system=traffic.retirement vehicle=%08X event=queued base=%08X lookup_ok=%d disabled=%d model=%d",id,known.baseId,known.id!=0,known.disabled,known.has3D);
                }
            }
            if(g_spares.size()!=oldSize)SaveSpares();
        }
        void DeleteTick(ULONGLONG now){
            int done=0;
            for(auto it=g_doomed.begin();it!=g_doomed.end();){
                FvoTrafficRef r{};
                if(now-it->at<2000){++it;continue;}
                if(!Info(it->id,r)){
                    if(it->submitted){Log("OBS system=traffic.retirement vehicle=%08X event=gone_after_delete",it->id);it=g_doomed.erase(it);continue;}
                    if(!it->missingSince){it->missingSince=now;Log("OBS system=traffic.retirement vehicle=%08X event=lookup_failed base=%08X",it->id,it->baseId);}
                    if(now-it->missingSince<kDoomedMissingMs){++it;continue;}
                    Log("OBS system=traffic.retirement vehicle=%08X event=gave_up lookup_failed_ms=%llu",it->id,now-it->missingSince);
                    it=g_doomed.erase(it);continue;
                }
                if(it->missingSince){
                    // The id came back. The game reuses ids: act only if it still names what was retired, and nothing here uses it now. A driver's base is
                    // not checked by the core, so one whose base was never read is left alone.
                    const unsigned id=it->id;
                    const bool same=it->baseId?r.baseId==it->baseId:!r.isActor;
                    const bool inUse=std::any_of(g_pairs.begin(),g_pairs.end(),[&](const Pair& q){return q.vehicle==id||q.driver==id;});
                    Log("OBS system=traffic.retirement vehicle=%08X event=lookup_returned after_ms=%llu base=%08X was=%08X actor=%d same=%d in_use=%d model=%d",id,now-it->missingSince,r.baseId,it->baseId,r.isActor,same,inUse,r.has3D);
                    if(!same||inUse){g_retiring.erase(id);it=g_doomed.erase(it);continue;}
                    it->missingSince=0;
                    if(!r.disabled&&g_api->show)g_api->show(id,0);
                }
                if(now-it->at<30000&&!r.disabled){++it;continue;}
                if(it->retryAt&&now<it->retryAt){++it;continue;}  // refused: waits without using this update's turns
                if(++done>2)break;  // a few at a time, so no burst of teardowns lands in one frame
                if(it->submitted&&r.disabled&&!r.has3D){Log("OBS system=traffic.retirement vehicle=%08X event=retired",it->id);it=g_doomed.erase(it);continue;}
                if(!r.disabled){if(g_api->show)g_api->show(it->id,0);++it;continue;}
                // Disabling is asynchronous. Do not delete a reference while its model may still be used by a
                // background update (18:10 crash: FF014591, Get3D returned null in FUN_141313D80).
                if(r.has3D){++it;continue;}
                if(!it->submitted&&g_api->deleteRef){
                    if(g_api->deleteRef(it->id)){
                        it->submitted=true;Log("Traffic pool: %08X native delete completed; waiting for model retirement",it->id);
                    }else if(!(g_api->mutationPending&&g_api->mutationPending(it->id,"delete"))){
                        // Finished and refused (the core logs why). Asking again every update held a request slot for good.
                        const ULONGLONG wait=std::min(kDeleteRetryMs<<std::min(it->refused,4),kDeleteRetryMaxMs);
                        ++it->refused;it->retryAt=now+wait;
                        Log("OBS system=traffic.retirement vehicle=%08X event=delete_refused refused=%d retry_ms=%llu",it->id,it->refused,wait);
                        // The game keeps refusing (engine_refused): it stays hidden and out of traffic, and stops counting against new traffic. A later load sweeps it.
                        if(it->refused>=kDeleteGiveUp){
                            Log("OBS system=traffic.retirement vehicle=%08X event=delete_given_up refused=%d",it->id,it->refused);
                            it=g_doomed.erase(it);continue;
                        }
                    }
                }
                ++it;
            }
        }
        void ShelveBadPair(int i,Pair& p,trafficpair::Bad bad,const char* how){
            FvoTrafficRef v{},d{};Info(p.vehicle,v);Info(p.driver,d);
            double nav=0,baked=0;const int known=g_api->ground?g_api->ground(v.x,v.y,v.z,&nav,&baked):0;
            const int sit=g_api->sitState?g_api->sitState(p.driver):-1;
            DeletePair(p.vehicle,p.driver);
            Log("Traffic pool: spot %d: pair %08X / %08X disbanded %s: %s; deleted, the position is filled from a landing spot; model %08X, state %d, born at (%.0f, %.0f, %.1f), vehicle at (%.0f, %.0f, %.1f) model %d, driver at (%.0f, %.0f, %.1f) model %d, %.1f m apart, ground %.1f (known %d), sit %d, aboard %d",
                i,p.vehicle,p.driver,how,trafficpair::BadName(bad),p.base,int(p.st),p.birth.x,p.birth.y,p.birth.z,v.x,v.y,v.z,v.has3D,d.x,d.y,d.z,d.has3D,
                std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z),known&2?baked:nav,known,sit,g_api->vehicleOf(p.driver)==p.vehicle?1:0);
            p=Pair{};p.st=St::Empty;SaveIds(i,p);g_initialRejected.erase(i);g_initialFails.erase(i);
            const auto alternatives=g_initialAlternatives.find(i);const int next=g_initialAttempt[i]+1;
            if(alternatives!=g_initialAlternatives.end()&&next<=int(alternatives->second.size()))g_initialAttempt[i]=next;else g_initialAttempt.erase(i);
        }
        // Never where it can be noticed (the FVO Traffic mandate: no unexplained disappearances; the user, 2026-10-30: "needs to be far and out of sight"): under a cover,
        // or each half with no model, or far AND out of sight. Far alone proves nothing: the Full LOD pass exists to keep far vehicles visible (53 held, out to 1378 m, in the
        // 11:57 run), and someone on the ground can have a clear view. A pair given up on goes at the first such moment; any other bad pair after 10 s of being bad.
        constexpr double kRidFar = 350.0;
        Pos g_playerAt{};
        trustzone::PlayerPath g_path;
        ULONGLONG g_coverEndedAt=0,g_zoneReportAt=0;unsigned g_zoneLostChecks=0;double g_zoneRadius=0;
        bool UnseenPair(const Pair& p){
            if(!g_api->presentation)return false;
            const int screen=g_api->presentation();
            if(screen>=2)return true;
            if(screen!=1)return false;
            FvoTrafficRef v{},d{};
            if(!Info(p.vehicle,v)||!Info(p.driver,d))return false;
            // A half with no model cannot be seen. So a broken pair denied the far pass unloads past the loaded cells and goes without anyone seeing it go.
            // Far is worked out per thing (trafficpair::NoticeFar). Out of sight: behind the camera's view, or the line of sight is blocked (a ray that hits nothing counts as seen,
            // and far terrain often has no physics to hit, so the rays alone hardly ever clear a far pair).
            const auto hidden=[&](const FvoTrafficRef& r,bool driver){
                if(!r.has3D)return true;
                const double far_=g_api->noticeFar?g_api->noticeFar(driver?0:r.baseId):kRidFar;
                if(std::hypot(r.x-g_playerAt.x,r.y-g_playerAt.y)<far_)return false;
                if(g_api->inCameraView&&!g_api->inCameraView(r.x,r.y,r.z))return true;
                return g_api->exposure&&g_api->exposure(r.x,r.y,r.z)==2;};
            return hidden(v,false)&&hidden(d,true);
        }
        bool ShelveBad(int i,Pair& p,ULONGLONG now){
            if(now-p.badAt<1000)return false;
            p.badAt=now;
            const auto bad=PairVerdict(p,now);
            if(bad==trafficpair::Bad::None){p.badSince=0;return false;}
            FvoTrafficRef where{};const bool ahead=Info(p.vehicle,where)&&g_path.Ahead(where.x,where.y,where.z,now);
            if(bad!=trafficpair::Bad::GaveUp&&bad!=trafficpair::Bad::Unrecoverable){
                if(!p.badSince){p.badSince=now;return false;}
                if(now-p.badSince<=trustzone::FaultWaitMs(ahead))return false;
            }
            if(!UnseenPair(p)){observations::Record(g_logFn,now,"traffic.zone.fault",p.vehicle,(std::uint64_t(bad)<<1)|std::uint64_t(ahead),"driver=%08X event=waiting_unseen slot=%d reason=%s ahead=%d",p.driver,i,trafficpair::BadName(bad),ahead);return false;}
            Log("OBS system=traffic.zone vehicle=%08X driver=%08X event=fault_swapped slot=%d reason=%s waited_ms=%llu ahead=%d",p.vehicle,p.driver,i,trafficpair::BadName(bad),p.badSince?now-p.badSince:0,ahead);
            ShelveBadPair(i,p,bad,g_api->presentation&&g_api->presentation()>=2?"under cover":"where it cannot be noticed");
            return true;
        }
        bool TakeSpare(unsigned base,Spare& out){
            LoadSpares();
            // Only a spare the game has really disabled. A disable is queued: one shelved under this same cover was still enabled, so its
            // show(1) did nothing and the queued disable hid it after the reuse (2026-10-30 10:31 run, spot 73: "vehicle or driver gone").
            for(auto it=g_spares.begin();it!=g_spares.end();++it){
                FvoTrafficRef v{},d{};
                if(it->base!=base||Retiring(it->vehicle)||Retiring(it->driver)||!Info(it->vehicle,v)||!Info(it->driver,d)||!v.disabled||!d.disabled||!d.isActor||d.dead)continue;
                out=*it;g_spares.erase(it);SaveSpares();return true;
            }
            return false;
        }
        // Every pair off the routes and out of sight, with its job and trip let go. Hidden, never deleted (deleting traffic has crashed the game), and kept to be used
        // again, so the save does not grow with each press. A vehicle the player took is theirs and stays.
        void ClearAllPairs(ULONGLONG){
            int cleared=0,kept=0;
            for(int i=0;i<int(g_pairs.size());++i){auto& p=g_pairs[std::size_t(i)];
                const bool retired=p.retired;
                if(p.vehicle&&g_api->playerTook&&g_api->playerTook(p.vehicle)){++kept;Log("Traffic reset: spot %d: vehicle %08X is the player's, left alone",i,p.vehicle);}
                else if(p.vehicle||p.driver){
                    FvoTrafficRef v{},d{};const bool hasV=Info(p.vehicle,v),hasD=Info(p.driver,d);
                    if(hasV&&g_api->usesVehicle(p.vehicle))g_api->endJob(p.vehicle,"traffic reset");
                    if(hasV)g_api->show(p.vehicle,0);
                    if(hasD)g_api->show(p.driver,0);
                    if(hasV&&hasD&&d.isActor&&!d.dead)AddSpare(p.vehicle,p.driver,p.base?p.base:v.baseId);  // entries from before 2026-10-29 kept no base
                    ++cleared;
                }
                p=Pair{};p.st=St::Empty;p.retired=retired;SaveIds(i,p);
            }
            g_initialRejected.clear();g_initialAttempt.clear();g_initialFails.clear();g_initialCursor=0;g_admission=-1;
            if(g_onCleared)g_onCleared();
            Log("Traffic reset: %d pair(s) cleared out, %d kept (the player's); the positions are filled from new",cleared,kept);
        }

		// Traffic NPCs: pool vehicles and their drivers are a fixed pool, made once and reused: both persistent (moved into the
		// world's persistent cell, as named NPCs are), so distance never unloads them. The drivers stay ordinary mortal citizens
		// (the user: "not be essential and unable to be killed").
		void MakeTrafficPair(int i, const Pair& p)
		{
			const bool v = g_api->makeTraffic(p.vehicle, 0) != 0, d = g_api->makeTraffic(p.driver, 1) != 0;
			Log("Traffic pool: spot %d: vehicle %08X %s, driver %08X %s", i, p.vehicle, v ? "persistent" : "NOT persistent",
				p.driver, d ? "persistent" : "NOT persistent");
		}

        void Fail(int i, Pair& p, const char* why){
            Log("Traffic pool: spot %d (%s): pair %08X/%08X parked/deferred: %s (no visible deletion or replacement)",i,g_world.c_str(),p.vehicle,p.driver,why);
            p.st=St::Failed;
        }
        bool InitialAdmissionsPending(){
            const int n=std::min(int(g_initialScene.size()),int(g_pairs.size()));
            for(int i=0;i<n;++i)if(g_pairs[i].st==St::Empty&&!g_pairs[i].vehicle&&!g_pairs[i].retired)return true;
            return false;
        }

		// Randomized traffic models (2026-10-29, the user: "randomize from vehicles that exist in game", "general not from a specific
		// vehicle list"): the pool is whatever the load order holds, every distinct model with the game's vehicle keyword (core's
		// vehicleTypes), never a list of ours. Each route position gets a seeded pick (world + slot), so the same position shows the
		// same model every visit and after every load. trafficpool.ini [types] skip = FORMID, FORMID leaves models out.
		std::vector<unsigned> g_typePool;std::string g_typeWorld;
		// Vehicle rules (2026-10-31, the user: models left out still appeared after X): the models each position may not hold.
		std::vector<std::vector<unsigned>> g_slotSkip;
		bool SlotSkips(int slot,unsigned id){
			if(slot<0||slot>=int(g_slotSkip.size()))return false;
			const auto& list=g_slotSkip[std::size_t(slot)];
			return std::find(list.begin(),list.end(),id)!=list.end();
		}
		std::set<unsigned> SkippedTypes(){
			std::set<unsigned> out;
			for(const wchar_t* key:{L"skip",L"auto_skip"}){wchar_t buf[2048]{};
				GetPrivateProfileStringW(L"types",key,L"",buf,2048,Ini().c_str());
				for(const wchar_t* c=buf;*c;){wchar_t* end=nullptr;const unsigned long v=std::wcstoul(c,&end,16);if(end==c){++c;continue;}if(v)out.insert(unsigned(v));c=end;}}
			return out;
		}
		// Models whose drivers do not sit (2026-10-30: some mod vehicles have no seat FVO's instant seat can use; their drivers
		// floated and the vehicles never got a pilot). Judged from what happens in game, never from a list: a model whose driver
		// failed to sit twice and never sat goes to trafficpool.ini [types] auto_skip; the next cover re-rolls its vehicles.
		struct SeatRecord{int sat=0,failed=0;};std::map<unsigned,SeatRecord> g_seatRecords;
		void JudgeSeat(Pair& p,bool sat){
			if(p.seatJudged||!p.base)return;
			p.seatJudged=true;auto& r=g_seatRecords[p.base];(sat?r.sat:r.failed)++;
			if(!sat&&r.failed>=2&&r.sat==0){
				std::set<unsigned> skip;wchar_t buf[2048]{};
				GetPrivateProfileStringW(L"types",L"auto_skip",L"",buf,2048,Ini().c_str());
				for(const wchar_t* c=buf;*c;){wchar_t* end=nullptr;const unsigned long v=std::wcstoul(c,&end,16);if(end==c){++c;continue;}if(v)skip.insert(unsigned(v));c=end;}
				if(skip.insert(p.base).second){
					std::wstring list;for(const unsigned id:skip){wchar_t b[16];swprintf(b,16,L"%s%08X",list.empty()?L"":L", ",id);list+=b;}
					WritePrivateProfileStringW(L"types",L"auto_skip",list.c_str(),Ini().c_str());
					g_typePool.clear();g_typeWorld.clear();
					Log("Traffic models: model %08X left out of traffic: its drivers failed to sit %d times and never sat (trafficpool.ini [types] auto_skip)",p.base,r.failed);
				}
			}
		}
		const std::vector<unsigned>& TypePool(){
			if(g_typeWorld==g_world&&!g_typePool.empty())return g_typePool;
			g_typePool.clear();
			if(g_api->vehicleTypes){
				unsigned ids[128]{};const int n=g_api->vehicleTypes(ids,128);const auto skip=SkippedTypes();
				for(int k=0;k<n;++k)if(!skip.count(ids[k])&&g_api->formExists(ids[k]))g_typePool.push_back(ids[k]);
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
		unsigned VehicleBase(int slot)
		{
			const auto& pool=TypePool();
			if(pool.empty())return g_api->formExists(kRev8)&&!SlotSkips(slot,kRev8)?kRev8:0;  // no pool yet (or none in the load order): the base game's REV-8
			const std::size_t n=pool.size(),round=std::size_t(std::max(slot,0))/n,at=std::size_t(std::max(slot,0))%n;
			std::uint32_t seed=2166136261u;for(const char c:g_world)seed=(seed^std::uint8_t(c))*16777619u;
			for(const unsigned id:pool)seed=(seed^id)*16777619u;
			seed^=std::uint32_t(round+1)*2654435761u;
			std::vector<unsigned> order(pool.begin(),pool.end());
			for(std::size_t k=n;k>1;--k){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;std::swap(order[k-1],order[seed%k]);}
			// Left out here: the next model in this round's order, so a position with no rules keeps the one it always had. None allowed: no vehicle here.
			for(std::size_t k=0;k<n;++k){const unsigned id=order[(at+k)%n];if(!SlotSkips(slot,id))return id;}
			return 0;
		}


        bool MutationPending(unsigned id,const char* operation){return g_api->mutationPending&&g_api->mutationPending(id,operation);}
        int FleetCount(){
            int n=0;for(const auto& p:g_pairs)if(p.vehicle||p.vehiclePending)++n;return n;
        }
        int ResidentCount(){
            int n=FleetCount();
            for(const auto& l:g_landings)if(l.vehicle||l.pending)++n;return n;
        }
        // A request returning zero is not a failed birth while its owned command
        // or completion is outstanding. Poll the exact same request before any
        // health check, alternate model selection, or retry counter.
        bool FinishBirth(int i,Pair& p,ULONGLONG now){
            if(p.vehiclePending){
                p.vehicle=g_api->createVehicle(p.base,p.birth.x,p.birth.y,p.birth.z,p.birth.heading);
                if(!p.vehicle&&MutationPending(p.base,"create"))return false;
                p.vehiclePending=false;
                if(!p.vehicle){p=Pair{};p.st=St::Empty;p.at=now+5000;SaveIds(i,p);g_initialRejected.insert(i);return false;}
                p.driverPending=true;SaveIds(i,p);
            }
            if(p.driverPending){
                p.driver=g_api->createSeatedDriver(kDriverBase,p.vehicle,p.birth.x,p.birth.y,p.birth.z,p.birth.heading,nullptr);
                if(!p.driver&&MutationPending(p.vehicle,"driver"))return false;
                p.driverPending=false;p.st=p.driver?St::Created:St::Failed;p.at=now;
                SaveIds(i,p);MakeTrafficPair(i,p);
            }
            if(p.relocating==1||p.relocating==2){
                const unsigned id=p.relocating==1?p.vehicle:p.driver;
                if(!g_api->moveTo(id,p.birth.x,p.birth.y,p.birth.z,p.birth.heading)){
                    if(MutationPending(id,"move"))return false;
                    DeletePair(p.vehicle,p.driver);p=Pair{};p.st=St::Empty;SaveIds(i,p);return false;
                }
                if(p.relocating++==1)return false;
                p.relocating=3;g_api->show(p.vehicle,1);g_api->show(p.driver,1);
            }
            if(p.relocating==3){
                FvoTrafficRef v{},d{};
                if(!Info(p.vehicle,v)||!Info(p.driver,d)||!v.usable||!d.usable||v.disabled||d.disabled)return false;
                p.relocating=0;if(g_api->holdBirth)g_api->holdBirth(p.vehicle);p.at=now;
            }
            return true;
        }
		void Step(int i, Pair& p, const FvoPoolSpot& s, const Pos& pp, const Snap& snap, ULONGLONG now)
		{
			g_clock=now;
			if(!FinishBirth(i,p,now)){
                observations::Record(g_logFn,now,"traffic.mutation-wait",p.vehicle,unsigned(p.relocating)|(std::uint64_t(p.vehiclePending)<<8)|(std::uint64_t(p.driverPending)<<9),
                    "slot=%d driver=%08X base=%08X relocation=%d vehicle_pending=%d driver_pending=%d",i,p.driver,p.base,p.relocating,p.vehiclePending,p.driverPending);return;
            }
            observations::Record(g_logFn,now,"traffic.slot",p.vehicle,unsigned(p.st)|(std::uint64_t(p.staged)<<8)|(std::uint64_t(p.proving)<<9)|(std::uint64_t(p.retired)<<10),
                "slot=%d driver=%08X base=%08X phase=%d staged=%d proving=%d reserved=%d retired=%d seat_tries=%d birth=%.2f,%.2f,%.2f target_slots=%zu",i,p.driver,p.base,int(p.st),p.staged,p.proving,p.reserved,p.retired,p.tries,p.birth.x,p.birth.y,p.birth.z,g_pairs.size());
			if (p.reloadAt) {  // the driver's 3D reload: enabled again after the gap, then seated by Created
				if (now - p.reloadAt < kReloadGapMs) return;
				p.reloadAt = 0;
				if (Exists(p.driver)) g_api->show(p.driver, 1);
				p.at = now; p.tries = 0; p.seatAt = 0;
				Log("Traffic pool: spot %d: driver %08X enabled again (3D reload %d)", i, p.driver, p.reloads);
				return;
			}
			g_playerAt=pp;
			// The player took it: it is theirs, and the position is filled again. Never hidden or moved, and its driver is a person in the world now.
			if(p.vehicle&&g_api->playerTook&&g_api->playerTook(p.vehicle)){
				Log("Traffic pool: spot %d: the player took vehicle %08X: traffic lets go of it (driver %08X), the position is filled again",i,p.vehicle,p.driver);
				p=Pair{};p.st=St::Empty;SaveIds(i,p);g_initialRejected.erase(i);g_initialAttempt.erase(i);g_initialFails.erase(i);
				return;
			}
			// Over the player nothing holds a new driver up until they sit: a made driver starts on the ground below, and one with a model falls. Kept at the vehicle until seated, every update.
			if(p.proving&&p.staged&&p.driver&&g_api->holdDriver)g_api->holdDriver(p.driver,p.vehicle);
			if(p.proving&&p.vehicle){
				// Time under the map does not count: the game is paused there and nothing can seat.
				const int cover=g_api->presentation?g_api->presentation():1;
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
						const bool armed=claimed&&g_api->startArmed(p.driver,p.vehicle,&claim);
						Log("OBS system=traffic.remote_start vehicle=%08X driver=%08X event=%s slot=%d distance=%.0f waited_ms=%llu",p.vehicle,p.driver,armed?"started":!claimed?"no_trip":"refused",i,std::hypot(pendingVehicle.x-pp.x,pendingVehicle.y-pp.y),now-p.bornAt);
						if(armed){p.st=St::Started;p.watchAt=now;p.proving=false;return;}
					}
					p.proveSince=now;p.seatedSince=0;return;
				}
				// Sitting is what the limit waits for (2026-10-31 18:59 run: FF1003A3 deleted 0.3 s after it sat, inside the hold; FF1003C9 seated 26 s, waiting for route room).
				// A seat that breaks later gets the whole limit to come back.
				if(p.st!=St::Failed&&pendingVehicle.usable&&pendingDriver.usable&&!pendingDriver.dead&&
				   SeatedPair(pendingVehicle.has3D!=0,pendingDriver.has3D!=0,Aboard(p.driver,p.vehicle),g_api->sitState?g_api->sitState(p.driver):-1,
				              std::hypot(std::hypot(pendingVehicle.x-pendingDriver.x,pendingVehicle.y-pendingDriver.y),pendingVehicle.z-pendingDriver.z)))p.proveSince=now;
				if((p.st==St::Failed||now-p.proveSince>trafficpair::kProveLimitMs)&&(p.staged||UnseenPair(p))){  // over the player nothing can be seen; a landing spot waits until it cannot be
					FvoTrafficRef v{},d{};const bool both=Info(p.vehicle,v)&&Info(p.driver,d)&&v.has3D&&d.has3D;
					Log("Traffic pool: spot %d: new pair %08X / %08X failed its check after %.0f s (vehicle model %d, driver model %d, aboard %d, sit %d): deleted, never joined traffic",i,p.vehicle,p.driver,(now-p.proveSince)/1000.0,v.has3D,d.has3D,
						p.driver&&g_api->vehicleOf(p.driver)==p.vehicle?1:0,p.driver&&g_api->sitState?g_api->sitState(p.driver):-1);
					if(both)JudgeSeat(p,false);
					DeletePair(p.vehicle,p.driver);
					p=Pair{};p.st=St::Empty;p.at=now+5000;SaveIds(i,p);g_initialRejected.insert(i);
					return;
				}
			}
			if(ShelveBad(i,p,now))return;
			PrecheckPair(i,p,pp,now);
			switch (p.st) {
			case St::Unknown: {
				auto alive = [&](const Pair& q) {
					bool v3 = false, d3 = false;
					// A remembered id can belong to something else in another save: the vehicle must still have the base it was made
					// with (entries from before 2026-10-29 kept no base: then a REV-8 / Creations base, as those pairs were made).
					const unsigned base = BaseId(q.vehicle);
					const bool sameKind = q.base ? base == q.base : (base == kRev8 || (base >> 24) == 0xFE);
					return Exists(q.vehicle) && Exists(q.driver) && sameKind && Usable(q.vehicle, v3) && Usable(q.driver, d3) &&
						LooksLikeActor(q.driver) && !IsDead(q.driver);
				};
				if(!p.vehicle&&!p.driver)LoadIds(i, p);
				if (alive(p)) {
                    if(p.staged&&g_api->holdBirth)g_api->holdBirth(p.vehicle);
                    // Saved while flying (2026-10-29 22:50 run: saved traffic dropped the moment physics started): frozen first, then
                    // the pilot recovery gives it a fresh pilot; one on the ground goes the ordinary way.
                    {FvoTrafficRef v{};
                     if(!p.staged&&g_api->holdBirth&&Info(p.vehicle,v)&&!Grounded({v.x,v.y,v.z})){
                         g_api->holdBirth(p.vehicle);p.staged=true;p.st=St::Started;p.at=p.watchAt=now;p.frozenAt=now-3000;p.rearms=0;p.rearmAt=0;
                         MakeTrafficPair(i,p);SaveIds(i,p);
                         Log("Traffic pool: spot %d (%s): reusing vehicle %08X / driver %08X, saved in the air: frozen until a fresh pilot holds it",i,g_world.c_str(),p.vehicle,p.driver);
                         return;}}
					// Made in an earlier visit and still there: reuse it, never make another.
					const bool seated = Aboard(p.driver, p.vehicle);
					p.st = seated ? St::Created : St::Started;
					p.at = now;
					MakeTrafficPair(i, p);
					Log("Traffic pool: spot %d (%s): reusing vehicle %08X / driver %08X (%s)", i, g_world.c_str(), p.vehicle, p.driver,
						seated ? "driver aboard: starting it" : "driver out: it is ordinary traffic now");
				}
				else if(p.vehicle&&Exists(p.vehicle)){p.st=St::Failed;}
                else { p.vehicle = p.driver = 0; p.st = St::Empty; }
				return;
			}
            case St::Empty: {
                if(p.retired||i!=g_admission)return;
                if(g_api->presentation&&g_api->presentation()==3)return; // arrival owns initial admission
                if(g_coldFilling)return;  // the cold start is still filling positions
                if(i<int(g_reuse.size())&&g_reuse[std::size_t(i)]<0)return; // no authored ground origin
                // Record observation before support is ready; a later cached
                // proof must not forget that the player already saw this origin.
                if(g_api->presentation&&g_api->presentation()==1&&g_api->exposure&&g_api->exposure(s.x,s.y,s.z)==1){p.observed=true;g_observedOrigins[g_world].insert(OriginKey(s));}
                if(now-g_createdAt<kCreateGapMs)return;
                if(std::count_if(g_pairs.begin(),g_pairs.end(),[](const Pair& q){return q.vehicle!=0;})>=kMaxSpots)return;
                const bool parkingSpot=i<int(g_reuse.size())&&g_reuse[std::size_t(i)];
                if(!g_api->birthSite||!g_api->presentation||!g_api->presentation())return;
                Snap live=snap;
                if(parkingSpot){
                    unsigned found=0;
                    for(const auto& v:live){
                        if(v.who!=kFvoWhoParked||!v.hasBody||std::hypot(v.x-s.x,v.y-s.y)>kBakedNear||std::fabs(v.z-s.z)>4||!g_api->canManagePlacement(v.id))continue;
                        bool taken=false;for(const auto& q:g_pairs)if(q.vehicle==v.id)taken=true;
                        if(!taken){found=v.id;break;}
                    }
                    if(!found||!trafficadmission::Capacity(ResidentCount(),0,int(g_doomed.size()),int(g_pairs.size()))||!g_trip||!g_trip(i,0,s.x,s.y,s.z,nullptr))return;
                    FvoTrafficRef vr{};if(!Info(found,vr))return;
                    const FvoPoolSpot origin{vr.x,vr.y,vr.z,s.heading};
                    if(!Grounded({vr.x,vr.y,vr.z})||!Concealed(origin,p))return;
                    p.vehicle=found;p.base=vr.baseId;p.birth=origin;
                }else{
                    if(p.vehicle||!g_api->formExists(kDriverBase))return; // a partial birth is never replaced by a second vehicle
                    // In play a position is filled only from a landing spot near the player: the vehicle waiting there, and a driver called to it while the spot cannot be seen.
                    Landing* best=nullptr;double bestDistance=std::numeric_limits<double>::infinity();
                    for(auto& l:g_landings){
                        FvoTrafficRef v{};if(!l.vehicle||!Info(l.vehicle,v)||!v.usable||!v.has3D||!HasBody(l.vehicle,snap))continue;
                        const double d=std::hypot(v.x-pp.x,v.y-pp.y);
                        if(d<bestDistance&&LandingUnseen(v.x,v.y,v.z)&&!g_api->usesVehicle(l.vehicle)){best=&l;bestDistance=d;}
                    }
                    if(!best)return;
                    FvoTrafficRef vr{};Info(best->vehicle,vr);
                    p.vehicle=best->vehicle;p.base=vr.baseId;p.birth={vr.x,vr.y,vr.z,best->at.heading};p.reserved=true;Measure(p,p.base);
                    best->vehicle=0;SaveLanding(*best);
                    p.proving=true;p.proveSince=p.proveTick=now;p.seatedSince=0;
                }
                g_createdAt=now;
                // Place the actor at the vehicle, not 2.5 m away at an unrelated fallback height.
                p.driver=g_api->createSeatedDriver(kDriverBase,p.vehicle,p.birth.x,p.birth.y,p.birth.z,p.birth.heading,nullptr);
                SaveIds(i,p);MakeTrafficPair(i,p);
                p.driverPending=!p.driver&&MutationPending(p.vehicle,"driver");
                p.st=(p.driver||p.driverPending)?St::Created:St::Failed;
                p.at=now;p.tries=0;p.seatAt=0;
                Log("Traffic pool: admitted grounded pair %08X/%08X at fixed origin %.1f %.1f %.1f; no lane-height birth",p.vehicle,p.driver,p.birth.x,p.birth.y,p.birth.z);
                return;
            }

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
				if(trafficpair::Floating(dr.has3D!=0,vr.has3D!=0,separation)&&!SeatedPair(vr.has3D!=0,dr.has3D!=0,Aboard(p.driver,p.vehicle),g_api->sitState(p.driver),separation)){
					if(!p.floatingSince)p.floatingSince=now;
					if(vr.has3D){
						if(!p.broughtTold){p.broughtTold=true;Log("Traffic pool: spot %d: driver %08X was %.1f m from vehicle %08X (born at z %.1f, vehicle z %.1f, driver z %.1f): brought to it",i,p.driver,separation,p.vehicle,p.birth.z,vr.z,dr.z);}
						if(now-p.seatAt>=250){p.seatAt=now;g_api->snapSeat(p.driver,p.vehicle);}  // every frame made log lines by the hundred and asked for a keyword each time
					}
					if(now-p.floatingSince>=(vr.has3D?kApartMs:kNoModelMs)&&now-p.floatingAskAt>=1000){
						p.floatingAskAt=now;
						if(UnseenPair(p))ShelveBadPair(i,p,vr.has3D?trafficpair::Bad::DriverFar:trafficpair::Bad::NoVehicleModel,g_api->presentation&&g_api->presentation()>=2?"under cover":"where it cannot be noticed");
					}
					return;
				}
				p.floatingSince=0;
				if (!vr.has3D || !dr.has3D) {
					if(vr.has3D&&!dr.has3D&&now-p.at>=kNo3DMs&&p.reloads<kMaxReloads&&std::hypot(vr.x-pp.x,vr.y-pp.y)<kDriver3DNear&&Grounded({vr.x,vr.y,vr.z})&&Concealed({vr.x,vr.y,vr.z,0},p)){
						++p.reloads;g_api->show(p.driver,0);p.reloadAt=now;p.at=now;
						Log("Traffic pool: spot %d: reloading driver model only after vehicle is grounded",i);return;
					}
					if (std::hypot(vr.x - pp.x, vr.y - pp.y) > kSpawnNear) { p.at = now; return; }  // far: its 3D comes when the player does
					if (now - p.at > kLoadWaitMs) Fail(i, p, "no 3D after 120 s");
					return;
				}
				if(!HasBody(p.vehicle,snap)){p.at=now;return;} // a model is not a live chassis
				const int sit = g_api->sitState(p.driver);
				if (SeatedPair(vr.has3D!=0,dr.has3D!=0,Aboard(p.driver,p.vehicle),sit,separation) && HasBody(p.vehicle,snap)) {
					Log("Traffic pool: spot %d: driver %08X sits in vehicle %08X (sit state %d)", i, p.driver, p.vehicle, sit);
					p.st = St::Ready; p.tripAt = 0; return;
				}
				if (p.seatAt && now - p.seatAt < kSeatEveryMs) return;
				if (p.tries >= kSeatTries) { Fail(i, p, "the driver would not sit after 6 instant seats"); return; }
				p.seatAt = now; ++p.tries;
				const int r = g_api->snapSeat(p.driver, p.vehicle);
				Log("Traffic pool: spot %d: instant seat -> %d, sit state %d (try %d)", i, r, g_api->sitState(p.driver), p.tries);
				return;
			}
			case St::Failed: {
                // A failed actor creation must not leak a second vehicle. Retry
                // just the missing driver, on the existing grounded chassis,
                // under the same concealment and readiness rules as admission.
                if(p.driver||!p.vehicle||p.retired||p.reloads>=kMaxReloads||now-g_createdAt<kCreateGapMs)return;
                FvoTrafficRef v{};if(!Info(p.vehicle,v)||!v.usable||!v.has3D||!HasBody(p.vehicle,snap)||
                    (p.staged?(g_api->presentation()<2&&!(g_api->exposure&&g_api->exposure(v.x,v.y,v.z)==2)):(!Grounded({v.x,v.y,v.z})||!Concealed({v.x,v.y,v.z,0},p))))return;
                g_createdAt=now;++p.reloads;
                p.driver=g_api->createSeatedDriver(kDriverBase,p.vehicle,v.x,v.y,v.z,s.heading,nullptr);
                if(p.driver){SaveIds(i,p);MakeTrafficPair(i,p);p.st=St::Created;p.at=now;p.tries=0;}
                else p.driverPending=MutationPending(p.vehicle,"driver");
                return;
            }
			case St::Ready: {
                if(p.retired)return;
                if(!p.proving&&p.staged&&g_api->presentation&&g_api->presentation()==3&&InitialAdmissionsPending())return;
				if (p.tripAt && (now < p.tripAt || now - p.tripAt < kTripRetryMs)) return;  // tripAt may lie ahead (a back-off)
				p.tripAt = now;
				if (!Exists(p.vehicle) || !Exists(p.driver) || !Aboard(p.driver, p.vehicle)) { p.st = St::Created; p.at = now; p.tries = 0; return; }  // fell out: seat again
				FvoTrafficRef vr{},dr{};
				if(!Info(p.vehicle,vr)||!Info(p.driver,dr)||!vr.usable||!dr.usable||dr.dead||
					!SeatedPair(vr.has3D!=0,dr.has3D!=0,true,g_api->sitState(p.driver),std::hypot(std::hypot(vr.x-dr.x,vr.y-dr.y),vr.z-dr.z))||!HasBody(p.vehicle,snap)){
					p.st=St::Created;p.at=now;p.tries=0;p.seatAt=0;p.seatedSince=0;return;
				}
				if (!g_trip) return;
				// Already driving under another job (e.g. reused aboard after a load): watch it, never claim a trip for it.
				if (g_api->usesVehicle(p.vehicle)) {
					Log("Traffic pool: spot %d: vehicle %08X already has an NPC job; watching it", i, p.vehicle);
					p.st = St::Started; p.watchAt = now; return;
				}
				// Ask first, claim after: a trip claimed and then refused held an addon slot (and a route's count) every 2 s.
				char why[160]{};
				if (g_api->tripRefusal(p.driver, p.vehicle, why, int(sizeof why))) {
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
				const bool ok = g_api->startTrip(p.driver, p.vehicle, &claim) != 0;
				Log("Traffic pool: spot %d: vehicle %08X with driver %08X starts a %s -> %s", i, p.vehicle, p.driver, claim.name ? claim.name : "trip", ok ? "driving" : "refused");
				if (ok) {
					if(p.proving)Log("Traffic pool: spot %d: pair %08X / %08X passed its check after %.1f s",i,p.vehicle,p.driver,(now-p.proveSince)/1000.0);
					p.st = St::Started; p.watchAt = now; p.proving=false; p.sending=sendOut; p.sendSince=now;
				}
				return;
			}
			case St::Started: {
                // Per-model seat check: once both models are loaded, a driver SITTING (sit state 3) within 10 s means the model seats
                // drivers (2026-10-30 07:38 run: on models 08000809 and 0800085F every driver stayed at sit state 1, linked but never
                // seated, in two runs; "aboard" alone is the creation link and passed every model).
                if(!p.seatJudged){FvoTrafficRef sv{},sd{};
                    if(Info(p.vehicle,sv)&&Info(p.driver,sd)&&sv.has3D&&sd.has3D){
                        if(Aboard(p.driver,p.vehicle)&&trafficpair::Sitting(g_api->sitState(p.driver)))JudgeSeat(p,true);
                        else if(!p.both3DSince)p.both3DSince=now;
                        else if(now-p.both3DSince>10000)JudgeSeat(p,false);
                    }else p.both3DSince=0;}
                // Post-load spot check: a vehicle placed blind behind the loading screen is checked against the nav map once it is
                // mapped there; one inside a building goes to a clear lane alternative while nobody can see it.
                if(p.staged&&!p.spotChecked&&g_api->mappedSpot&&now-p.spotAt>=1000&&g_api->presentation&&g_api->presentation()==1){
                    p.spotAt=now;FvoTrafficRef sv{};
                    if(Info(p.vehicle,sv)&&sv.has3D){
                        const int r=g_api->mappedSpot(p.base,sv.x,sv.y,sv.z,p.birth.heading);
                        if(r>0)p.spotChecked=true;
                        else if(r<0){
                            const auto alt=g_initialAlternatives.find(i);bool moved=false;
                            if(alt!=g_initialAlternatives.end())for(const auto& a:alt->second){
                                if(g_api->mappedSpot(p.base,a.x,a.y,a.z,a.heading)<=0||Occupied(i,a,snap))continue;
                                if(g_api->moveTo&&g_api->moveTo(p.vehicle,a.x,a.y,a.z,a.heading)){
                                    p.birth=a;moved=true;p.spotChecked=true;
                                    Log("Traffic spot check: vehicle %08X was inside a solid at (%.0f, %.0f, %.0f); moved out of sight to (%.0f, %.0f, %.0f)",p.vehicle,sv.x,sv.y,sv.z,a.x,a.y,a.z);}
                                break;  // one try per second: the move may wait for the player to look away
                            }
                            if(!moved&&(alt==g_initialAlternatives.end()||alt->second.empty())){p.spotChecked=true;
                                Log("Traffic spot check: vehicle %08X is inside a solid at (%.0f, %.0f, %.0f) and has no lane alternative; kept frozen",p.vehicle,sv.x,sv.y,sv.z);}
                        }
                    }
                }
                // A staged pair whose chassis left the physics world never gets released and hangs frozen at its lane spot (2026-10-30
                // 07:38 run: three pairs 340-460 m out, "chassis not in the physics world", frozen in the sky for good). After 30 s
                // without a body, while the player has never seen that spot and cannot see it now, it goes back on the spare shelf
                // and its route position is filled again later (when the player is near enough for physics).
                {FvoTrafficRef bv{};
                if(p.staged&&Info(p.vehicle,bv)&&bv.has3D&&!HasBody(p.vehicle,snap)){
                    if(!p.noBodySince)p.noBodySince=now;
                    else if(now-p.noBodySince>30000&&Concealed({bv.x,bv.y,bv.z,0},p)){
                        DeletePair(p.vehicle,p.driver);
                        Log("Traffic pool: spot %d: vehicle %08X had no physics body for %.0f s at (%.0f, %.0f, %.0f), frozen out of sight; deleted, the position is filled from a landing spot",
                            i,p.vehicle,(now-p.noBodySince)/1000.0,bv.x,bv.y,bv.z);
                        p=Pair{};p.st=St::Empty;SaveIds(i,p);g_initialRejected.erase(i);g_initialAttempt.erase(i);g_initialFails.erase(i);
                        return;
                    }
                }else p.noBodySince=0;}
                // Confirmed handoff: released only when its physics body exists, its driver sits in it and its pilot holds it.
                const bool handoffReady=[&]{FvoTrafficRef hv{};return Info(p.vehicle,hv)&&hv.has3D&&HasBody(p.vehicle,snap)&&Aboard(p.driver,p.vehicle)&&trafficpair::Sitting(g_api->sitState(p.driver));}();  // frozen until its driver really sits
                if(p.staged&&handoffReady&&g_api->releaseBirth&&g_api->releaseBirth(p.vehicle)){
                    p.staged=false;SaveIds(i,p);
                    if(p.bornAt)Log("Traffic arrival: vehicle %08X under its autopilot %.1f s after its birth (%s pilot)",p.vehicle,(now-p.bornAt)/1000.0,p.armed?"pre-armed":"ordinary");
                    p.bornAt=0;
                }
                // The route claim is already running. Let its pilot carry the
                // seated pair from the proving area; synchronous cross-cell
                // MoveReference here deadlocked actor-data/physics jobs in the
                // 2026-10-31 12:44 dump, including while the target was unseen.
                if(p.sending&&!p.staged&&(!g_api->pilotHolds||g_api->pilotHolds(p.vehicle))){
                    p.sending=false;
                    Log("Traffic pool: spot %d: checked pair %08X / %08X flies from its proving area to its route (no relocation)",i,p.vehicle,p.driver);
                }
                // Fall guard (2026-10-29 late; 21:27 run: pilots stopped in mid-air and 29 traffic vehicles fell through the world,
                // down to z -36 km). A traffic vehicle that no pilot holds never free-falls: frozen where it is (unknown ground counts
                // as airborne) until its pilot takes it again. One already far under its route goes back to its route position
                // while a cover hides it (loading screen or map), frozen.
                if(g_api->pilotHolds&&g_api->holdBirth&&now-p.fallCheckAt>=500){
                    p.fallCheckAt=now;
                    FvoTrafficRef v{};
                    if(Info(p.vehicle,v)&&!g_api->pilotHolds(p.vehicle)){
                        FvoPoolSpot home=p.birth;
                        if(!(home.x||home.y||home.z)&&i<int(g_initialScene.size()))home=g_initialScene[std::size_t(i)];
                        const bool homeKnown=home.x||home.y||home.z;
                        const bool fallen=v.z<-1000||(homeKnown&&v.z<home.z-150);
                        if(fallen&&g_api->presentation&&g_api->presentation()>=2&&homeKnown&&g_api->moveTo&&g_api->moveTo(p.vehicle,home.x,home.y,home.z,home.heading)){
                            g_api->holdBirth(p.vehicle);p.staged=true;SaveIds(i,p);
                            Log("Traffic fall guard: vehicle %08X was at z %.0f; put back on its route at (%.0f, %.0f, %.0f) under cover, frozen until its pilot holds it",p.vehicle,v.z,home.x,home.y,home.z);
                        }
                        else if(!p.staged&&v.has3D&&!fallen&&!Grounded({v.x,v.y,v.z})){
                            g_api->holdBirth(p.vehicle);p.staged=true;SaveIds(i,p);p.frozenAt=now;
                            Log("Traffic fall guard: vehicle %08X has no pilot holding it at (%.0f, %.0f, %.0f): frozen in place until its pilot takes it again",p.vehicle,v.x,v.y,v.z);
                        }
                    }
                }
                // Pilot recovery: a frozen vehicle must not hang in the air. 3 s without a pilot: its stalled job ends and a fresh
                // pre-armed pilot with a new trip takes it (driver re-seated first). After 3 failed tries it is set down on the known
                // ground below once nobody can see it, and waits there frozen like a parked vehicle for a later trip.
                if(p.staged&&g_api->pilotHolds&&(!p.bornAt||now-p.bornAt>10000)){
                    FvoTrafficRef v{};
                    FvoTrafficRef rd{};
                    if(g_api->pilotHolds(p.vehicle)){p.frozenAt=0;p.rearms=0;p.setDown=false;}
                    // The driver's model is out of the loaded cells: no pilot can seat them until it is back, so it is not a stalled pilot. Waits frozen;
                    // restarts here only ran out and set the vehicle down (2026-10-30 10:59 run: 49 restarts).
                    else if(Info(p.vehicle,v)&&v.has3D&&Info(p.driver,rd)&&!rd.has3D&&std::hypot(v.x-pp.x,v.y-pp.y)>=trafficpair::kDriverModelReach)p.frozenAt=now;
                    else if(Info(p.vehicle,v)&&v.has3D&&!p.setDown){
                        if(!p.frozenAt)p.frozenAt=now;
                        // Never arm a pilot on a vehicle whose driver is out of reach of its seat (16:55 run: FF10E519's driver was
                        // 450 m away, snapSeat refused, the pilot was armed anyway and the empty vehicle's body was thrown about
                        // 1,000 km). Such a pair is left to the pair check (ShelveBad: driver out or away, retired once unseen).
                        FvoTrafficRef dr{};
                        const bool driverInReach=Aboard(p.driver,p.vehicle)||(Info(p.driver,dr)&&
                            std::hypot(std::hypot(dr.x-v.x,dr.y-v.y),dr.z-v.z)<=trafficpair::kSnapReach);
                        if(!driverInReach&&now-p.frozenAt>=3000&&now-p.rearmAt>=3000&&p.rearms<3){
                            p.rearmAt=now;p.rearms=3;  // no restart can work: straight to the set-down below, the pair check retires it
                            Log("Traffic fall guard: vehicle %08X frozen %.1f s without a pilot, its driver %08X out of reach of the seat: no pilot restart; set down and left to the pair check",
                                p.vehicle,(now-p.frozenAt)/1000.0,p.driver);
                        }
                        if(driverInReach&&now-p.frozenAt>=3000&&now-p.rearmAt>=3000&&p.rearms<3&&g_api->startArmed&&g_trip){
                            p.rearmAt=now;++p.rearms;
                            if(g_api->usesVehicle(p.vehicle))g_api->endJob(p.vehicle,"traffic fall guard: pilot restart");
                            g_api->snapSeat(p.driver,p.vehicle);  // the core snaps a linked driver that is not sitting, leaves a sitting one
                            FvoTripClaim claim{};
                            const bool claimed=g_trip(i,p.vehicle,v.x,v.y,v.z,&claim)&&claim.step;
                            const bool ok=claimed&&g_api->startArmed(p.driver,p.vehicle,&claim);
                            Log("Traffic fall guard: vehicle %08X frozen %.1f s without a pilot: pilot restart %d of 3 -> %s",p.vehicle,(now-p.frozenAt)/1000.0,p.rearms,
                                ok?"armed":!claimed?"refused (no trip: the route has no free place)":"refused (no free pilot, or vehicle/driver busy)");
                        }
                        else if(p.rearms>=3&&now-p.rearmAt>=5000&&g_api->ground&&g_api->moveTo){
                            double nav=0,baked=0;const int known=g_api->ground(v.x,v.y,v.z,&nav,&baked);
                            const double ground=known&2?baked:nav;
                            if(known&&std::isfinite(ground)&&ground<v.z&&g_api->moveTo(p.vehicle,v.x,v.y,ground+0.5,p.birth.heading)){
                                p.setDown=true;
                                Log("Traffic fall guard: vehicle %08X had no pilot after 3 restarts: set down out of sight at (%.0f, %.0f, %.0f), parked",p.vehicle,v.x,v.y,ground+0.5);
                            }
                        }
                    }
                }
				// Watch: the trip ended at its spot. After kIdleMs with no trip: a driver still aboard gets a new trip, one out of the
				// vehicle is seated again first. kRestarts times.
				if (now - p.watchAt < kWatchMs) return;
				p.watchAt = now;
				if (!Exists(p.vehicle) || !Exists(p.driver) || IsDead(p.driver)) { p.idleSince = 0; return; }
				if (g_api->usesVehicle(p.vehicle)) {
					// Driving: a minute of it earns its restarts back.
					p.idleSince = 0;
					if (!p.busySince) p.busySince = now;
					else if (now - p.busySince > 60000) { p.restarts = 0; p.reloads = 0; }
					bool v3 = false, d3 = false;
					const Pos vp = PosOf(p.vehicle);
					const bool close = std::hypot(vp.x - pp.x, vp.y - pp.y) < kDriver3DNear;
					if (!(Usable(p.vehicle, v3) && Usable(p.driver, d3) && v3 && !d3 && close && Aboard(p.driver, p.vehicle))) { p.no3DSince = 0; return; }
					if (!p.no3DSince) { p.no3DSince = now; return; }
					if (now - p.no3DSince < kNo3DMs || p.reloads >= kMaxReloads || !Grounded(vp)||!Concealed({vp.x,vp.y,vp.z,0},p)) return;
					p.no3DSince = 0; ++p.reloads;
					Log("Traffic pool: spot %d: vehicle %08X is %.0f m away with 3D but driver %08X has none: trip ended, reloading the driver (%d of %d)",
						i, p.vehicle, std::hypot(vp.x - pp.x, vp.y - pp.y), p.driver, p.reloads, kMaxReloads);
					g_api->endJob(p.vehicle, "driver has no 3D");
					g_api->show(p.driver, 0);
					p.reloadAt = now; p.st = St::Created; p.at = now;
					return;
				}
				p.busySince = 0;
				const bool aboard = Aboard(p.driver, p.vehicle);
				const Pos vp = PosOf(p.vehicle);
				// A driver aboard gets a new trip wherever the vehicle stopped; one who got out is seated again only at its spot.
				if ((!aboard && std::hypot(vp.x - s.x, vp.y - s.y) > kNearSpot) || p.restarts >= kRestarts) return;
				if (!p.idleSince) { p.idleSince = now; return; }
				if (now - p.idleSince < kIdleMs) return;
				p.idleSince = 0;
				++p.restarts;
				Log("Traffic pool: spot %d: vehicle %08X has had no trip for %llu s (driver %s): %s (restart %d)", i, p.vehicle,
					static_cast<unsigned long long>(kIdleMs / 1000), aboard ? "aboard" : "out", aboard ? "new trip" : "seating again", p.restarts);
				if (aboard) { p.st = St::Ready; p.tripAt = 0; }
				else { p.st = St::Created; p.at = now; p.tries = 0; p.seatAt = 0; }
				return;
			}
			default: return;
			}
		}

		bool PlayerNow(Pos& pp, std::string& world)
		{
			FvoTrafficPlayer pl{};
			if (!g_api->player(&pl)) return false;
			pp = { pl.x, pl.y, pl.z };
			world = pl.world;
			return true;
		}

        void RushStep(){
            if(!g_rushRequest.exchange(false))return;
            g_initialMap=true;
            std::lock_guard lock(g_rushLock);
            g_rush={};g_rush.phase=0;g_rush.total=int(g_pairs.size());g_rush.done=g_rush.total;
            g_rush.placed=0;g_rush.made=0;g_rushMade=0;g_rushFill=true;g_rushClear=true;g_coverPassDone=false;g_rush.active=1; // cleared and filled under the panel next
            Log("Traffic reset: asked for; %d position(s) will be cleared and filled from new under the panel",int(g_pairs.size()));
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
                if(pair.vehicle)used.insert(pair.vehicle);pair.retired=false;g_pairs.push_back(pair);
            }
            // Removing/editing an origin doesn't orphan its population. Surplus
            // vehicles finish their committed trips, then remain parked.
            for(std::size_t j=0;j<old.size();++j)if(!taken[j]&&old[j].vehicle&&!used.count(old[j].vehicle)){
                old[j].retired=true;g_pairs.push_back(old[j]);g_spots.push_back(oldSpots[j]);g_reuse.push_back(oldReuse[j]);g_names.push_back(oldNames[j]);used.insert(old[j].vehicle);
            }
            g_admission=-1;g_appliedRevision=g_revision;
        }
        void RecoverRegistry(){
            // Read the entire historical registry: route-key churn used to orphan
            // old saved fleets. Adopt each valid physical pair at most once.
            std::vector<wchar_t> section(65536);
            GetPrivateProfileSectionW(Wide(g_world).c_str(),section.data(),DWORD(section.size()),Ini().c_str());
            std::set<unsigned> vehicles,drivers;
            for(int i=0;i<int(g_pairs.size());++i){auto& p=g_pairs[i];
                if(!p.vehicle)LoadIds(i,p);
                FvoTrafficRef v{},d{};
                if(p.vehicle&&(!Info(p.vehicle,v)||(p.base&&v.baseId!=p.base)||vehicles.count(p.vehicle)))p={};
                if(p.vehicle&&(!Info(p.driver,d)||!d.isActor||drivers.count(p.driver))){p.driver=0;p.st=St::Failed;}
                if(p.vehicle){Measure(p,v.baseId);vehicles.insert(p.vehicle);if(p.driver)drivers.insert(p.driver);if(p.staged&&g_api->holdBirth)g_api->holdBirth(p.vehicle);}
            }
            for(const wchar_t* row=section.data();*row;row+=wcslen(row)+1){
                if(wcsncmp(row,L"pair_",5)&&wcsncmp(row,L"spot",4))continue;
                const auto eq=wcschr(row,L'=');if(!eq)continue;Pair p;
                unsigned staged=0;
                if(std::swscanf(eq+1,L"%x %x %x %x",&p.vehicle,&p.driver,&p.base,&staged)<2||vehicles.count(p.vehicle)||drivers.count(p.driver))continue;
                p.staged=staged!=0;
                FvoTrafficRef v{},d{};
                if(!Info(p.vehicle,v)||!Info(p.driver,d)||!v.usable||!d.usable||!d.isActor||d.dead||
                   (p.base&&v.baseId!=p.base)||!Aboard(p.driver,p.vehicle))continue;
                auto target=std::find_if(g_pairs.begin(),g_pairs.end(),[](const Pair& q){return !q.vehicle;});
                if(target==g_pairs.end()){
                    if(g_pairs.size()>=1024)break; // bound legacy-cache recovery, not physical admission
                    g_pairs.push_back({});g_spots.push_back({v.x,v.y,v.z,0});g_reuse.push_back(0);g_names.push_back("retained."+std::to_string(p.vehicle));target=std::prev(g_pairs.end());p.retired=true;
                }
                p.base=v.baseId;Measure(p,p.base);p.st=St::Created;p.at=GetTickCount64();if(p.staged&&g_api->holdBirth)g_api->holdBirth(p.vehicle);*target=p;
                vehicles.insert(p.vehicle);drivers.insert(p.driver);SaveIds(int(target-g_pairs.begin()),p);
            }
        }
        void RepairPoisoned(const Snap& snap,ULONGLONG now){
            // Old builds can leave a valid scene body kilometres below terrain.
            // Recover the same pair only under a covering map, never replace it
            // or teleport healthy traffic to match an edit.
            if(g_api->presentation()!=2||!g_api->birthSite||now-g_repairAt<kCreateGapMs)return;
            if(g_repair<0||g_repair>=int(g_pairs.size())){
                g_repair=-1;
                for(int i=0;i<int(g_pairs.size());++i){const auto& p=g_pairs[i];FvoTrafficRef v{},d{};
                    if(!Info(p.vehicle,v)||!Info(p.driver,d)||d.dead||!v.usable)continue;
                    double nav=0,baked=0;const int k=g_api->ground(v.x,v.y,v.z,&nav,&baked);
                    const double floor=k&2?baked:nav;
                    if(k&&std::isfinite(floor)&&v.z<floor-20){g_repair=i;g_repairOrigin=0;g_repairAt=now;break;}
                }
                return;
            }
            auto& p=g_pairs[g_repair];
            if(g_wantSpots.empty()){g_repair=-1;return;}
            const auto& origin=g_wantSpots[g_repairOrigin%g_wantSpots.size()];
            double z=0;const int result=g_api->birthSite(p.base,origin.x,origin.y,origin.z,origin.heading,&z);
            if(result<0||now-g_repairAt>8000){++g_repairOrigin;g_repairAt=now;return;}
            if(result!=1)return;
            const FvoPoolSpot at{origin.x,origin.y,z,origin.heading};if(Occupied(g_repair,at,snap)){++g_repairOrigin;return;}
            g_api->endJob(p.vehicle,"world repair: invalid below-terrain traffic body"); // force release before relocation
            if(!g_api->moveTo(p.vehicle,at.x,at.y,at.z,at.heading)){if(!MutationPending(p.vehicle,"move"))g_repair=-1;return;}
            p.birth=at;p.relocating=2;
            p.st=St::Created;p.at=now;p.tries=0;
            p.seatAt=p.tripAt=p.no3DSince=0;p.birth=at;p.reserved=true;g_createdAt=now;
            Log("Traffic pool: recovered existing below-terrain pair %08X/%08X at validated ground under map cover",p.vehicle,p.driver);
            g_repair=-1;g_repairAt=now;
        }

	}

	void Attach(const FvoTrafficApi* api, FvoLog_t log) { g_api = api; g_logFn = log; }

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

    void SetModelRules(const std::vector<std::vector<unsigned>>& skip){g_slotSkip=skip;}
    void SetInitialScene(const FvoPoolSpot* spots,int count){
        g_initialScene.clear();if(spots&&count>0)g_initialScene.assign(spots,spots+std::min(count,kMaxSpots));g_initialRejected.clear();
        g_initialAlternatives.clear();g_initialAttempt.clear();g_initialFails.clear();g_initialCursor=0;
    }
    void SetInitialAlternatives(int slot,const FvoPoolSpot* spots,int count){if(slot>=0&&slot<int(g_initialScene.size())&&spots&&count>0)g_initialAlternatives[slot].assign(spots,spots+std::min(count,16));}
    void WaitForInitialClock(bool pending){g_initialClockPending=pending;}
    bool InitialVehicle(int spot){return spot>=0&&spot<int(g_pairs.size())&&g_pairs[spot].staged;}
    // Under a cover (loading screen or map), once (2026-10-29 22:50 run):
    //  - leftover traffic: runtime-made vehicles with a driver aboard, no FVO job and no slot here (traffic saved by an older
    //    session whose registry entries were since replaced) dropped the moment physics started. Each goes to the nearest empty
    //    slot's position, frozen, and the pilot recovery gives it a fresh pilot; with no empty slot it is hidden under the cover.
    //  - model re-roll: a traffic vehicle whose model is not its position's pick is replaced (hidden, slot refilled) so the
    //    randomized mix shows at once instead of only for brand-new positions.
    // The trust zone: every pair's stage, once a second each. Watches and reports; removal stays with ShelveBad.
    void ZoneTick(const Pos& pp,const Snap& snap,ULONGLONG now){
        const int screen=g_api->presentation();
        if(screen>=2)g_coverEndedAt=now;
        g_path.Sample(pp.x,pp.y,pp.z,now);
        std::vector<trustzone::Seen> seen;seen.reserve(g_pairs.size());
        unsigned count[4]{};
        for(int i=0;i<int(g_pairs.size());++i){auto& p=g_pairs[std::size_t(i)];
            if(!p.vehicle||!p.driver||p.retired)continue;
            FvoTrafficRef v{},d{};if(!Info(p.vehicle,v)||!Info(p.driver,d))continue;
            if(now-p.zoneAt>=1000){p.zoneAt=now;
                const double apart=std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z);
                const bool holds=g_api->pilotHolds&&g_api->pilotHolds(p.vehicle),aboard=g_api->vehicleOf(p.driver)==p.vehicle;
                // Stepping physics proves a chassis. The vehicle list cannot: it leaves out vehicles held far away, which is most flying traffic (2026-11-01 run: checked stayed at 0).
                const int live=g_api->pilotLive?g_api->pilotLive(p.vehicle):-1;
                const bool noPhysics=live==0&&screen==1&&v.has3D;
                if(noPhysics&&!p.noPhysicsSince){p.noPhysicsSince=now;
                    Log("OBS system=traffic.zone vehicle=%08X driver=%08X event=no_physics_begin slot=%d distance=%.0f",p.vehicle,p.driver,i,std::hypot(v.x-pp.x,v.y-pp.y));
                }else if(!noPhysics&&p.noPhysicsSince){
                    Log("OBS system=traffic.zone vehicle=%08X driver=%08X event=no_physics_end slot=%d after_ms=%llu live=%d screen=%d",p.vehicle,p.driver,i,now-p.noPhysicsSince,live,screen);
                    p.noPhysicsSince=0;
                }
                trustzone::Facts facts;facts.vehicleModel=v.has3D!=0;facts.driverModel=d.has3D!=0;facts.fault=PairVerdict(p,now)!=trafficpair::Bad::None;
                const bool body=live==1||(live<0&&HasBody(p.vehicle,snap)),sitting=trafficpair::Sitting(g_api->sitState(p.driver)),close=std::isfinite(apart)&&apart<=(holds?kFlyingLagReach:8.0),heldUp=live==1||Grounded({v.x,v.y,v.z});
                facts.passes=aboard&&body&&sitting&&close&&heldUp;
                const char* why=!facts.vehicleModel?"vehicle_model":!facts.driverModel?"driver_model":facts.fault?"fault":!aboard?"not_aboard":live==0?"no_physics":!body?"no_chassis":!sitting?"not_sitting":!close?"driver_apart":!heldUp?"nothing_holds_it_up":"none";
                const auto before=p.zone.stage;
                if(p.zone.Update(facts,now)){
                    const bool lost=trustzone::LostCheck(before,p.zone.stage);
                    if(lost)++g_zoneLostChecks;
                    Log("OBS system=traffic.zone vehicle=%08X driver=%08X event=stage slot=%d from=%s to=%s why=%s lost_check=%d since_cover_ms=%llu distance=%.0f apart=%.1f aboard=%d sit=%d holds=%d screen=%d exposure=%d ahead=%d",
                        p.vehicle,p.driver,i,trustzone::Name(before),trustzone::Name(p.zone.stage),why,lost,g_coverEndedAt?now-g_coverEndedAt:0,std::hypot(v.x-pp.x,v.y-pp.y),apart,aboard,g_api->sitState(p.driver),holds,screen,
                        g_api->exposure?g_api->exposure(v.x,v.y,v.z):-1,g_path.Ahead(v.x,v.y,v.z,now));
                }
            }
            ++count[int(p.zone.stage)];
            seen.push_back({std::hypot(v.x-pp.x,v.y-pp.y),p.zone.stage});
        }
        g_zoneRadius=trustzone::CheckedRadius(std::move(seen));
        if(now-g_zoneReportAt>=5000){g_zoneReportAt=now;
            Log("OBS system=traffic.zone event=state build=trust-first-20261101 unloaded=%u trusted=%u checked=%u faulty=%u checked_radius=%.0f lost_checks=%u speed=%.1f screen=%d",
                count[0],count[1],count[2],count[3],g_zoneRadius,g_zoneLostChecks,g_path.Speed(),screen);
        }
    }

    void CoverPass(const Snap& snap,ULONGLONG now){
        (void)now;g_coverPassDone=true;
        if(!g_api->holdBirth||!g_api->moveTo||!g_api->show)return;
        std::set<unsigned> mine;for(const auto& p:g_pairs){if(p.vehicle)mine.insert(p.vehicle);if(p.driver)mine.insert(p.driver);}
        int adopted=0,hiddenLeft=0,rerolled=0;
        const int total=std::min(int(g_initialScene.size()),int(g_pairs.size()));
        for(const auto& v:snap){
            if(!v.id||(v.id>>24)!=0xFF||v.who!=kFvoWhoNpc||!v.driver||(v.driver>>24)!=0xFF||mine.count(v.id)||mine.count(v.driver)||Retiring(v.id)||Retiring(v.driver))continue;
            if(g_api->usesVehicle&&g_api->usesVehicle(v.id))continue;  // an FVO job (follower, crew, combat) owns it
            // Old-save cleanup (2026-10-30): leftovers carry whatever state an older build left (inside buildings, below the
            // city, drivers detached); they are hidden under the cover and the routes are filled with a fresh fleet instead.
            g_api->show(v.id,0);g_api->show(v.driver,0);mine.insert(v.id);mine.insert(v.driver);++hiddenLeft;AddSpare(v.id,v.driver,v.baseId);
            Log("Traffic cover pass: leftover vehicle %08X / driver %08X from an older session hidden under cover (a fresh fleet fills the routes)",v.id,v.driver);
        }
        for(int i=0;i<total;++i){auto& p=g_pairs[std::size_t(i)];if(!p.vehicle||p.retired||!p.base)continue;
            const unsigned want=VehicleBase(i);if(!want||want==p.base)continue;
            if(g_api->usesVehicle&&g_api->usesVehicle(p.vehicle)&&g_api->endJob)g_api->endJob(p.vehicle,"traffic: model re-roll under cover");
            g_api->show(p.vehicle,0);if(p.driver)g_api->show(p.driver,0);AddSpare(p.vehicle,p.driver,p.base);
            Log("Traffic cover pass: slot %d vehicle %08X (model %08X) replaced under cover by a %08X (kept as a spare)",i,p.vehicle,p.base,want);
            p=Pair{};p.st=St::Empty;SaveIds(i,p);g_initialRejected.erase(i);g_initialAttempt.erase(i);g_initialFails.erase(i);++rerolled;
        }
        // Bad pairs (2026-10-30, the user: replaced behind the loading screen, never seen): hidden and kept as spares; the arrival
        // fills their positions under this same cover, and the handoff keeps a new vehicle frozen until its driver really sits.
        int replaced=0;
        for(int i=0;i<total;++i){auto& p=g_pairs[std::size_t(i)];
            const auto bad=PairVerdict(p,now);
            if(bad!=trafficpair::Bad::None){ShelveBadPair(i,p,bad,"under cover");++replaced;}
        }
        if(replaced)Log("Traffic cover pass: %d bad pair(s) replaced",replaced);
        if(adopted||hiddenLeft||rerolled)Log("Traffic cover pass: %d leftover vehicle(s) adopted, %d hidden, %d re-rolled to their position's model",adopted,hiddenLeft,rerolled);
    }
    // Traffic the save holds but no registry knows (2026-10-31: trafficpool.ini is not per save, so a save older than it brings back a whole fleet nobody owns; its vehicles drop when
    // built and their drivers stay at lane height. The cover pass only caught those with the driver still aboard). Any such pair is hidden as one and kept as a spare, under a
    // cover at once, in play when it can't be noticed, so the save stops growing and the positions reuse them.
    // Every id in trafficpool.ini: the form table holds other worlds' fleets too, unloaded, and those are not leftovers.
    std::set<unsigned> ListedIds(){
        std::set<unsigned> ids;std::string text;
        if(FILE* f=_wfopen(Ini().c_str(),L"rb")){char buf[4096];size_t got;while((got=fread(buf,1,sizeof(buf),f))>0)text.append(buf,got);fclose(f);}
        for(size_t i=0;i+8<=text.size();++i){
            if(text[i]!='F'||text[i+1]!='F'||(i&&isxdigit((unsigned char)text[i-1])))continue;
            size_t k=2;while(k<8&&isxdigit((unsigned char)text[i+k]))++k;
            if(k==8&&(i+8==text.size()||!isxdigit((unsigned char)text[i+8])))ids.insert(unsigned(strtoul(text.substr(i,8).c_str(),nullptr,16)));
        }
        return ids;
    }
    void SweepOrphans(ULONGLONG now){
        static ULONGLONG at=0;
        if(!g_api->linkedPairs||!g_api->show||g_pairs.empty()||now-at<500)return;  // often: a pair that just loaded has no model yet, the moment it can go unseen
        at=now;
        std::set<unsigned> mine;
        for(const auto& p:g_pairs){if(p.vehicle)mine.insert(p.vehicle);if(p.driver)mine.insert(p.driver);}
        for(const auto& sp:g_spares){mine.insert(sp.vehicle);mine.insert(sp.driver);}
        unsigned drivers[256]{},vehicles[256]{};
        const int n=g_api->linkedPairs(drivers,vehicles,256);
        const std::set<unsigned> listed=n?ListedIds():std::set<unsigned>{};
        for(int i=0;i<n;++i){
            if(mine.count(drivers[i])||mine.count(vehicles[i])||Retiring(drivers[i])||Retiring(vehicles[i]))continue;
            FvoTrafficRef v{},d{};
            if(!Info(vehicles[i],v)||!Info(drivers[i],d)||!d.isActor||d.disabled)continue;
            if(!v.has3D&&!d.has3D&&(listed.count(drivers[i])||listed.count(vehicles[i])))continue;
            Pair orphan;orphan.vehicle=vehicles[i];orphan.driver=drivers[i];
            if(!UnseenPair(orphan))continue;
            g_api->show(vehicles[i],0);g_api->show(drivers[i],0);AddSpare(vehicles[i],drivers[i],v.baseId);
            mine.insert(vehicles[i]);mine.insert(drivers[i]);
            Log("Traffic orphans: vehicle %08X / driver %08X (model %08X) from a save hidden and kept as a spare: %.1f m apart",vehicles[i],drivers[i],v.baseId,std::hypot(std::hypot(v.x-d.x,v.y-d.y),v.z-d.z));
        }
    }
    void SetLandingSpots(const FvoPoolSpot* spots,const char* const* names,int count){
        g_landings.clear();
        for(int i=0;spots&&i<count;++i){Landing l;l.at=spots[i];l.key=names&&names[i]?names[i]:std::to_string(i);g_landings.push_back(l);}
    }
    void LandingStock(const Snap& snap,const Pos&,ULONGLONG now){
        if(!g_api->birthSite||!g_api->createVehicle)return;
        for(auto& l:g_landings)if(l.pending){
            l.vehicle=g_api->createVehicle(l.base,l.requestAt.x,l.requestAt.y,l.requestAt.z,l.requestAt.heading);
            if(!l.vehicle&&MutationPending(l.base,"create"))return;
            l.pending=false;if(l.vehicle){if(g_api->makeTraffic)g_api->makeTraffic(l.vehicle,0);SaveLanding(l);}return;
        }
        for(auto& l:g_landings){
            if(!l.loaded){l.loaded=true;const std::wstring key=L"landing_"+std::wstring(l.key.begin(),l.key.end());wchar_t text[64]{};
                const std::wstring section(g_world.begin(),g_world.end());
                GetPrivateProfileStringW(section.c_str(),key.c_str(),L"",text,64,Ini().c_str());
                unsigned v=0,b=0;
                if(swscanf_s(text,L"%x %x",&v,&b)==2&&v&&b&&Exists(v)&&BaseId(v)==b&&!LooksLikeActor(v)){FvoTrafficRef r{};if(Info(v,r)&&r.usable){l.vehicle=v;l.base=b;}}}
            if(l.vehicle){bool taken=g_api->playerTook&&g_api->playerTook(l.vehicle);FvoTrafficRef r{};
                if(!Info(l.vehicle,r)||!r.usable||taken||g_api->usesVehicle(l.vehicle)){l.vehicle=0;SaveLanding(l);}}
            if(l.vehicle||now-g_landingAt<kCreateGapMs)continue;
            if(!trafficadmission::Capacity(ResidentCount(),0,int(g_doomed.size()),int(g_pairs.size())))return;
            g_landingAt=now;
            if(g_pairs.empty())continue;
            const unsigned base=VehicleBase(int(g_pairs.size())-1);if(!base)continue;
            double z=0;if(g_api->birthSite(base,l.at.x,l.at.y,l.at.z,l.at.heading,&z)!=1)continue;
            const FvoPoolSpot origin{l.at.x,l.at.y,z,l.at.heading};
            if(!LandingUnseen(origin.x,origin.y,origin.z)||Occupied(-1,origin,snap))continue;
            l.vehicle=g_api->createVehicle(base,origin.x,origin.y,origin.z,origin.heading);
            if(!l.vehicle){l.pending=MutationPending(base,"create");if(l.pending){l.base=base;l.requestAt=origin;return;}continue;}
            l.base=base;if(g_api->makeTraffic)g_api->makeTraffic(l.vehicle,0);SaveLanding(l);
            Log("Traffic landing: %s: vehicle %08X (model %08X) waits at (%.0f, %.0f, %.0f) for a driver",l.key.c_str(),l.vehicle,base,origin.x,origin.y,origin.z);
        }
    }
    void InitialScene(const Snap& snap,const Pos& pp,ULONGLONG now){
        if(!g_api->presentation||!g_api->airSite)return;
        // 3: behind the game's loading screen; 2: under the local map; 1: gameplay, where airSite only accepts spots the
        // player cannot see. The route population therefore exists on arrival and is completed out of sight, never in view.
        const int screen=g_api->presentation();
        if(!screen)return;
        if(screen<2)g_coverPassDone=false;  // the next cover runs the pass again
        if(g_rushClear&&screen>=2){
            for(int i=0;i<int(g_pairs.size());++i)if(!FinishBirth(i,g_pairs[i],now))return;
            g_rushClear=false;ClearAllPairs(now);g_clearUntil=now+kClearWaitMs;return;
        }
        if(screen>=2&&!g_coverPassDone)CoverPass(snap,now);
        SweepOrphans(now);
        DeleteTick(now);
        // Reset traffic (the editor's X): every pair is cleared out above, then the positions are filled from new. Hiding takes a moment to land, and only a hidden
        // pair can be reused, so the fill waits for it (at most kClearWaitMs): otherwise each press would make a whole new fleet.
        if(g_clearUntil){
            bool landed=now>=g_clearUntil;
            if(!landed){landed=true;for(const auto& sp:g_spares){FvoTrafficRef v{},d{};if((Info(sp.vehicle,v)&&!v.disabled)||(Info(sp.driver,d)&&!d.disabled)){landed=false;break;}}}
            if(!landed)return;
            g_clearUntil=0;
        }
        if(g_initialClockPending){if(g_api->arrivalProgress)g_api->arrivalProgress(std::max(1,int(g_initialScene.size())),0);return;}
        const int total=std::min(int(g_initialScene.size()),int(g_pairs.size()));
        // Traffic starts two ways only (2026-10-31, the user): a cold start (the loading screen; what it did not finish goes on after it) or a reset (X, under the panel). Anything later comes from a landing spot.
        if(screen==3)g_coldFilling=true;
        if(g_rushFill&&InitialAdmissionsPending())g_coldFilling=true; // finish queued reset positions after the map closes, concealed
        if(g_coldFilling&&!InitialAdmissionsPending())g_coldFilling=false;
        const bool filling=g_coldFilling||g_rushFill;
        if(!filling)LandingStock(snap,pp,now); // initial route fill has priority over parked supply
        // One admission per update at the authored lane position. Pending operations and active model checks are paced
        // separately from the rule-owned fleet target; unloaded distant pairs do not monopolize that work budget.
        int waiting=0,resident=FleetCount();std::set<int> slots;
        for(const auto& q:g_pairs)if(q.proving){
            FvoTrafficRef v{},d{};
            // Throttle work that can actually run now, not the total fleet awaiting distant model loads.
            // A pair past its seat hold only waits for route room; that must not stop other routes filling.
            if(trafficpair::ProveHeld(q.seatedSince,now)){slots.insert(q.proveSlot);continue;}
            if(q.vehiclePending||q.driverPending||q.relocating||
               (Info(q.vehicle,v)&&Info(q.driver,d)&&v.has3D&&d.has3D))++waiting;
            slots.insert(q.proveSlot);
        }
        const int maxMade=1,maxLook=screen>=2?32:4;int made=0;
        const int first=total?g_initialCursor%total:0;
        // At most this many over the player at once (2026-10-31 11:23 run: 79 fully simulated actors at once, allocator crash 20 s later, not proven). The fleet fills as they pass.
        const int maxWaiting=screen>=2?12:4;
        // Why positions stay empty, every 5 s while filling. site_wait: the site or the player's-sight check has no answer yet (its rays are still out).
        static ULONGLONG fillToldAt=0;static int fillSiteWait=0,fillRefused=0,fillOccupied=0;
        if(filling&&now-fillToldAt>=5000){
            int empty=0;for(int k=0;k<total;++k)if(g_pairs[k].st==St::Empty&&!g_pairs[k].vehicle)++empty;
            Log("OBS system=traffic.fill vehicle=00000000 event=state empty=%d total=%d waiting=%d max_waiting=%d site_wait=%d site_refused=%d occupied=%d doomed=%d screen=%d",empty,total,waiting,maxWaiting,fillSiteWait,fillRefused,fillOccupied,int(g_doomed.size()),screen);
            fillToldAt=now;fillSiteWait=fillRefused=fillOccupied=0;
        }
        for(int k=0,looked=0;filling&&k<total&&looked<maxLook&&waiting<maxWaiting;++k){const int i=(first+k)%total;auto& p=g_pairs[i];
            if(p.st!=St::Empty||p.vehicle||p.vehiclePending||p.retired||now<p.at)continue;
            g_initialRejected.erase(i);
            ++looked;g_initialCursor=(i+1)%total;
            const int attempt=g_initialAttempt[i];const auto alternatives=g_initialAlternatives.find(i);
            const auto& at=attempt&&alternatives!=g_initialAlternatives.end()?alternatives->second[std::size_t(attempt-1)]:g_initialScene[i];
            // Queue pressure is temporary. Never turn it into a permanently skipped route position.
            if(!trafficadmission::Capacity(resident,0,int(g_doomed.size()),int(g_pairs.size())))break;
            if(!trafficadmission::ValidOrigin(at.x,at.y)) { g_initialRejected.insert(i);p.at=now+5000;continue; }
            const unsigned base=VehicleBase(i);if(!base)continue;
            Measure(p,base);
            // Where it will go must be clear; the rays only answer in play for what can be checked unseen, so in play the nav map decides.
            const int site=g_api->airSite(base,at.x,at.y,at.z,at.heading);
            const bool occupied=site==1&&Occupied(i,at,snap);
            if(site==0)++fillSiteWait;
            if(site<0)++fillRefused;
            if(occupied)++fillOccupied;
            if(site<0||occupied){
                if(alternatives!=g_initialAlternatives.end()&&attempt<int(alternatives->second.size()))++g_initialAttempt[i];
                else if(occupied){g_initialAttempt[i]=0;} // traffic moves: try the authored positions again on a later update
                else {g_initialRejected.insert(i);p.at=now+5000;g_initialAttempt[i]=0;
                    Log("Traffic arrival: slot %d (model %08X) has no usable lane position at (%.0f, %.0f, %.0f) or its 16 alternatives; last refusal: %s; deferred",
                        i,base,at.x,at.y,at.z,site<0?"the spawn check (under the ground, inside a solid, or below 0 with the ground unknown)":"another vehicle is there");}
                continue;
            }if(site!=1)continue;
            int slot=0;while(slots.count(slot))++slot;
            const double ox=at.x,oy=at.y,oz=at.z,oh=at.heading;
            if(screen>=2&&g_api->show&&g_api->moveTo&&g_api->holdBirth){Spare sp{};
                if(TakeSpare(base,sp)){
                    // Keep both disabled while their owned relocation commands
                    // finish. Enabling before a queued move exposed falling stock.
                    p.vehicle=sp.vehicle;p.driver=sp.driver;p.base=base;p.birth=at;p.reserved=p.staged=true;p.at=p.bornAt=now;
                    p.armed=false;p.fromSpare=true;p.st=St::Created;p.relocating=1;
                    p.proving=true;p.proveSlot=slot;p.proveSince=p.proveTick=now;p.seatedSince=0;
                    slots.insert(slot);++waiting;++resident;SaveIds(i,p);MakeTrafficPair(i,p);++g_rushMade;
                    if(++made>=maxMade)break;continue;
                }
            }
            p.vehicle=g_api->createVehicle(base,ox,oy,oz,oh);
            if(!p.vehicle&&!MutationPending(base,"create")){
                g_initialRejected.insert(i);p.at=now+5000;
                Log("Traffic arrival: slot %d: creation not completed; still queued for the requested fleet, retry in 5 s",i);continue;
            }
            p.base=base;p.birth=at;p.reserved=p.staged=true;p.at=now;p.bornAt=now;p.armed=false;p.vehiclePending=!p.vehicle;
            p.proving=true;p.proveSlot=slot;p.proveSince=p.proveTick=now;p.seatedSince=0;slots.insert(slot);++waiting;
            g_createdAt=now;
            if(p.vehicle)p.driver=g_api->createSeatedDriver(kDriverBase,p.vehicle,ox,oy,oz,oh,nullptr);
            p.driverPending=p.vehiclePending||(!p.driver&&MutationPending(p.vehicle,"driver"));
            p.st=(p.driver||p.vehiclePending||p.driverPending)?St::Created:St::Failed;
            SaveIds(i,p);if(!p.vehiclePending)MakeTrafficPair(i,p);++resident;
            Log("Traffic arrival: new pair %08X/%08X made for a check before its place at %.1f %.1f %.1f %s",p.vehicle,p.driver,at.x,at.y,at.z,screen==3?"behind the loading screen":screen==2?"under the local map":"out of the player's sight");
            ++g_rushMade;if(++made>=maxMade)break;
        }
        // Progress for the loading-screen log line: route positions that hold a vehicle with a living driver (placed now,
        // or kept from an earlier visit). Staged ones stay frozen until their autopilot takes them.
        int placed=0;
        for(int i=0;i<total;++i){const auto& p=g_pairs[i];FvoTrafficRef v{},d{};
            if(Info(p.vehicle,v)&&Info(p.driver,d)&&!d.dead)++placed;
        }
        (void)pp;
        if(g_api->arrivalProgress)g_api->arrivalProgress(total,placed);
        // Start/Swap: the editor's loading panel stays up while positions are still being filled under the map.
        if(g_rushFill){
            const bool pending=screen>=2&&InitialAdmissionsPending();
            std::lock_guard lock(g_rushLock);
            g_rush.total=total;g_rush.placed=placed;g_rush.made=g_rushMade;g_rush.skipped=int(g_initialRejected.size());
            g_rush.driving=0;for(const auto& p:g_pairs)if(p.vehicle&&g_api->usesVehicle(p.vehicle))++g_rush.driving;
            g_rush.active=pending?1:0;
            if(!pending){g_rushFill=false;
                Log("Traffic reset: %d of %d route positions hold vehicles, %d placed under the panel, %d deferred with no clear lane spot",
                    placed,total,g_rushMade,g_rush.skipped);}
        }
    }
    void Reset(){
        ++observations::epoch;
        std::scoped_lock lock(g_lock,g_rushLock);
        g_world.clear();g_spots.clear();g_reuse.clear();g_names.clear();g_pairs.clear();g_worldPools.clear();g_observedOrigins.clear();g_observeCursor=0;
        g_rush={};g_rushRequest=false;g_rushFill=false;g_rushMade=0;g_rushClear=false;g_clearUntil=0;g_coverPassDone=false;g_spares.clear();g_sparesWorld.clear();
        g_wantWorld.clear();g_wantSpots.clear();g_wantReuse.clear();g_wantNames.clear();
        g_off=false;g_createdAt=g_lastTick=0;g_admission=-1;g_cursor=0;g_repair=-1;g_repairAt=0;
        g_path={};g_coverEndedAt=g_zoneReportAt=0;g_zoneLostChecks=0;g_zoneRadius=0;
        g_initialScene.clear();g_initialRejected.clear();g_initialAlternatives.clear();g_initialAttempt.clear();g_initialFails.clear();g_initialCursor=0;g_initialMap=false;
        g_initialClockPending=false;
        g_coldFilling=false;g_landings.clear();g_landingAt=0;g_doomed.clear();g_retiring.clear();
    }
    void Rush(const FvoRushPlace*,int){g_rushRequest.store(true);}
    void SetClearedHook(void (*hook)()){g_onCleared=hook;}
    void RushStatus(FvoRushState* out){if(!out)return;std::lock_guard lock(g_rushLock);*out=g_rush;if(g_rushRequest.load())out->active=1;}
    void Housekeeping(){if(g_api)Tick();}
    void Tick(){
        if(!g_api||g_off||!g_api->trafficOn()||!g_api->presentation||!g_api->presentation())return;
        Pos pp;std::string name;if(!PlayerNow(pp,name))return;
        {std::lock_guard lock(g_lock);if(g_wantWorld!=name)return;}
        if(g_world!=name){
            g_repair=-1;g_admission=-1;g_lastTick=0;
            if(!g_world.empty())g_worldPools[g_world]={g_spots,g_reuse,g_names,g_pairs};
            g_world=name;auto it=g_worldPools.find(name);
            if(it!=g_worldPools.end()){g_spots=it->second.spots;g_reuse=it->second.reuse;g_names=it->second.names;g_pairs=it->second.pairs;}
            else {g_spots.clear();g_reuse.clear();g_names.clear();g_pairs.clear();}
            Reconcile();RecoverRegistry();
        }else if(g_appliedRevision!=g_revision)Reconcile();

        RushStep();
        const auto now=GetTickCount64();
        if(g_lastTick&&now-g_lastTick>1000){const auto gap=now-g_lastTick;
            for(auto& p:g_pairs)for(auto* t:{&p.at,&p.seatAt,&p.tripAt,&p.watchAt,&p.idleSince,&p.askAt,&p.busySince,&p.no3DSince})if(*t)*t+=gap;
        }g_lastTick=now;
        Snap snap(kSnapMax);const int n=g_api->vehicles(snap.data(),kSnapMax);if(n<0)return;snap.resize(std::size_t(std::clamp(n,0,kSnapMax)));
        // Remember observable origins independently of which fleet member is
        // waiting there; a new slot/route must not erase the player's knowledge.
        if(g_api->presentation()==1&&g_api->exposure&&!g_spots.empty())for(int count=0;count<4;++count){
            const auto& origin=g_spots[g_observeCursor++%g_spots.size()];
            if(g_api->exposure(origin.x,origin.y,origin.z)==1)g_observedOrigins[g_world].insert(OriginKey(origin));
        }
        // One candidate's physical queries at a time: no fleet burst, queue flood,
        // or concurrent births hidden from the snapshot. Warm checks also run
        // during play; actual admission is separately concealed and observable.
        if(g_admission<0||g_admission>=int(g_pairs.size())||g_pairs[g_admission].st!=St::Empty||now-g_admissionAt>5000){
            g_admission=-1;
            for(std::size_t j=0;j<g_pairs.size();++j){const auto i=(g_cursor+j)%g_pairs.size();
                if(g_pairs[i].st==St::Empty&&!g_pairs[i].retired){g_admission=int(i);g_cursor=(i+1)%g_pairs.size();g_admissionAt=now;break;}}
        }
        RepairPoisoned(snap,now);
        for(int i=0;i<int(g_pairs.size());++i)Step(i,g_pairs[i],g_spots[i],pp,snap,now);
        ZoneTick(pp,snap,now);
        InitialScene(snap,pp,now);
    }
}
