#include "citytraffic.h"
#include "airtraffic.h"
#include "addresses.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "realvehicles.h"
#include "ringschedule.h"
#include "citybuild.h"
#include "citystartup.h"
#include "citypool.h"
#include "fvopaths.h"
#include "own_spots.h"
#include "trafficspots.h"
#include "framework.h"
#include "runtimeforms.h"
#include "placementenable.h"

#include <Windows.h>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace citytraffic
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T> T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		template <class F> F EngineFn(std::uint32_t rva) { return reinterpret_cast<F>(game::Base() + rva); }

        constexpr const auto& kProfile=citybuild::kProfile;
		constexpr const char* kPlugin = kProfile.plugin;
		constexpr ULONGLONG kTickMs = 2000, kReportMs = 30000;
		constexpr double kSeatReach = 12.0;               // m: a pilot this close to its vehicle is seated at once

		constexpr int kN = int(kProfile.vehicles.count);
		std::array<std::uint32_t, kN> g_vehicles{}, g_pilots{};
		bool g_resolved = false, g_present = false;
		ULONGLONG g_at = 0, g_reportAt = 0;
		unsigned g_seatedNow = 0, g_loadedNow = 0, g_seatTries = 0, g_seatOk = 0;
        std::array<framework::Retry,kN> g_seatRetry{};
        unsigned g_nextSeat=0;

		uptr Lookup(std::uint32_t id)
		{
			if (!id) return 0;
			__try {
				const uptr f = runtimeforms::LookupChecked(id);
				return f && At<std::uint32_t>(f, off::kFormId) == id ? f : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		}
		// Loaded = in a cell with its 3D (a persistent reference far away has neither 3D nor a loaded cell).
		bool Loaded(uptr ref, game::Vec3& pos)
		{
			__try {
				if (!ref || (At<std::uint32_t>(ref, 0x20) & 0x800) || !At<uptr>(ref, off::kRefParentCell)) return false;
				const uptr data = At<uptr>(ref, off::kRefLoadedData);
				if (!data || !At<uptr>(data, off::kLoadedData3D)) return false;
				pos = At<game::Vec3>(ref, off::kRefPosition);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		// ---- The spare pool (stage B, 2026-10-24, the user: new spots work at once in game) ----
		// Spots recorded since the game started are not in FVO_CityTraffic.esm yet (it is rebuilt at the next start): each gets a
		// pool vehicle (0xF00 + i, in key order), moved there and enabled; removed spots that are in the plugin are disabled. Done
		// again on every load and every few seconds from the files, so a save never holds the pool's places for good.
		std::map<std::uint64_t, std::uint32_t> g_bakedIds;   // spots in the plugin: key -> local id (citybuild-ids.tsv, as of this start)
		std::set<std::uint64_t> g_homeKeys;                   // the traffic's home spots (never parked or pooled)
		std::uint64_t g_marksStamp = 0, g_removedStamp = 0;
		std::vector<citybuild::Spot> g_pending;               // in the pool's order
		citybuild::Removed g_removed;
		unsigned g_poolShown = 0;
        citypool::Assignments g_assignments;
        std::set<std::uint64_t> g_wanted;
        std::map<unsigned,std::uint64_t> g_synced;
        std::array<std::uint32_t,citybuild::kPool> g_poolIds{};
        bool g_idsResolved=false;
        bool g_poolReady=false,g_filesReady=false;
        void ResolveIds(){
            if(g_idsResolved)return;g_idsResolved=true;g_present=false;
            for(int k=0;k<kN;++k){g_vehicles[k]=runtimeforms::Resolve(kProfile.vehicles.At(std::size_t(k)));g_pilots[k]=runtimeforms::Resolve(kProfile.pilots.At(std::size_t(k)));g_present=g_present||g_vehicles[k];}
            for(unsigned slot=0;slot<citybuild::kPool;++slot)g_poolIds[slot]=runtimeforms::Resolve(kProfile.pool.At(slot));
        }
        bool SavePool(const citypool::Assignments& assignments){
            const auto path=fvopaths::File(L"citypool-ids.tsv"),tmp=path+L".fvo-new";
            {std::ofstream f(tmp,std::ios::binary|std::ios::trunc);const auto text=citypool::Text(assignments);f.write(text.data(),std::streamsize(text.size()));f.flush();if(!f)return false;f.close();if(f.fail())return false;}
            if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){DeleteFileW(tmp.c_str());return false;}return true;
        }


		std::uint64_t Stamp(const std::wstring& path)
		{
			WIN32_FILE_ATTRIBUTE_DATA a{};
			if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
			return (std::uint64_t(a.ftLastWriteTime.dwHighDateTime) << 32 | a.ftLastWriteTime.dwLowDateTime) ^ a.nFileSizeLow;
		}
		std::vector<std::string> ReadLines(const std::wstring& path)
		{
			std::vector<std::string> out;
			std::ifstream f(path);
			for (std::string l; std::getline(f, l);) { if (!l.empty() && l.back() == '\r') l.pop_back(); out.push_back(l); }
			return out;
		}
        bool ReadPoolFiles(bool force)
        {
            const auto marks=fvopaths::File(L"placement-markers.tsv"),removed=fvopaths::File(L"placement-removed.tsv");
            const auto ms=Stamp(marks),rs=Stamp(removed);
            if(!force&&g_filesReady&&ms==g_marksStamp&&rs==g_removedStamp)return true;
            const auto readable=[](const std::wstring& path){return GetFileAttributesW(path.c_str())==INVALID_FILE_ATTRIBUTES||bool(std::ifstream(path));};
            if(!readable(marks)||!readable(removed))return false;
            bool goodMarks=false,goodRemoved=false;
            const auto recorded=citybuild::Recorded(ReadLines(marks),&goodMarks);
            const auto deleted=citybuild::RemovedFrom(ReadLines(removed),&goodRemoved);
            if(!goodMarks||!goodRemoved)return false;
            std::vector<citybuild::Spot> baked;for(const auto& spot:ownspots::kSpots)baked.push_back({spot.key,spot.x,spot.y,spot.z,spot.heading});
            const auto all=citybuild::Merge(baked,recorded,deleted);
            g_pending.clear();g_wanted.clear();
            for(const auto& spot:all){g_wanted.insert(spot.key);if(!g_bakedIds.count(spot.key)&&!g_homeKeys.count(spot.key))g_pending.push_back(spot);}
            g_removed=deleted;g_marksStamp=ms;g_removedStamp=rs;g_filesReady=true;
            logger::Write("City placements: %zu pending spot(s), %zu active keys, %zu removed",g_pending.size(),g_wanted.size(),g_removed.keys.size());return true;
        }

		bool PlayerWorld(uptr& cell, uptr& ws, char* name, std::size_t n)
		{
			name[0] = 0;
			__try {
				const uptr player = *reinterpret_cast<uptr*>(game::Base() + addr::kPlayerCharacterPtr);
				cell = player ? At<uptr>(player, off::kRefParentCell) : 0;
				ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
				if (!ws) return false;
				auto** vt = *reinterpret_cast<void***>(ws);
				const char* id = reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(ws);
				std::size_t i = 0;
				for (; id && id[i] && i + 1 < n; ++i) name[i] = id[i];
				name[i] = 0;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool Move(uptr ref, uptr cell, uptr ws, float pos[3], float rot[3])
		{
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			__try { EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(ref, &handle, cell, ws, pos, rot); return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		struct EnableEngine {
			uptr ref;
			void CancelRequests() { EngineFn<void (*)(uptr)>(addr::kEnableReference)(ref); }
			bool Disabled() const { return (At<std::uint32_t>(ref,0x20)&0x800)!=0; }
			void Enable() { EngineFn<void (*)(uptr,char)>(addr::kRefEnableTask)(ref,1); }
		};
		bool Show(uptr ref, bool on)
		{
			__try {
				const bool disabled = (At<std::uint32_t>(ref, 0x20) & 0x800) != 0;
				if (on) { EnableEngine engine{ref}; return placementenable::Restore(engine); }
				if (!on && !disabled) EngineFn<void (*)(uptr, char)>(addr::kDisableReference)(ref, 0);
                return ((At<std::uint32_t>(ref,0x20)&0x800)==0)==on;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
		bool PositionOf(uptr ref, float out[3])
		{
			__try { const auto& p = At<game::Vec3>(ref, off::kRefPosition); out[0] = p.x; out[1] = p.y; out[2] = p.z; return true; }
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}
        void SyncPool()
        {
            if(!g_poolReady)return;
            uptr cell=0,ws=0;char world[200];
            if(!PlayerWorld(cell,ws,world,sizeof(world))||!citybuild::CityWorld(world)||!ReadPoolFiles(false))return;
            std::array<bool,citybuild::kPool> busy{};
            for(unsigned slot=0;slot<citybuild::kPool;++slot)busy[slot]=!realvehicles::CanManagePlacement(g_poolIds[slot]);
            const auto planned=citypool::Plan(g_assignments,g_pending,busy);
            if(planned!=g_assignments){if(!SavePool(planned)){logger::Write("City placements: pool mapping write failed; changes deferred");return;}g_assignments=planned;}
            std::map<unsigned,const citybuild::Spot*> wanted;
            for(const auto& spot:g_pending)if(const auto it=g_assignments.find(spot.key);it!=g_assignments.end())wanted[it->second]=&spot;
            unsigned shown=0;
            for(unsigned slot=0;slot<citybuild::kPool;++slot){
                const auto ref=Lookup(g_poolIds[slot]);if(!ref||busy[slot])continue;
                const auto it=wanted.find(slot);
                if(it==wanted.end()){Show(ref,false);g_synced.erase(slot);continue;}
                const auto& spot=*it->second;
                if(g_synced.count(slot)&&g_synced[slot]==spot.key){++shown;continue;}
                float pos[3]={float(spot.x),float(spot.y),float(spot.z+.3)},rot[3]={0,0,float(spot.yaw)},now[3]{};
                const bool there=PositionOf(ref,now)&&std::hypot(now[0]-pos[0],now[1]-pos[1])<.5&&std::fabs(now[2]-pos[2])<1;
                if(!there){
                    if(!Move(ref,cell,ws,pos,rot)||!PositionOf(ref,now)||std::hypot(now[0]-pos[0],now[1]-pos[1])>=.5||std::fabs(now[2]-pos[2])>=1)continue;
                }
                if(Show(ref,true)){g_synced[slot]=spot.key;++shown;}
            }
            // Erased recordings as well as explicit removals take effect; never disable a vehicle in use.
            for(const auto& [key,local]:g_bakedIds){
                const auto id=runtimeforms::Resolve({kPlugin,local});if(!realvehicles::CanManagePlacement(id))continue;
                bool poolInUse=false;
                if(const auto it=g_assignments.find(key);it!=g_assignments.end())poolInUse=busy[it->second];
                if(const auto ref=Lookup(id))Show(ref,g_wanted.count(key)&&!poolInUse);
            }
            if(shown!=g_poolShown){logger::Write("City placements: %u pool vehicle(s), %zu pending (%zu deferred)",shown,g_pending.size(),g_pending.size()-wanted.size());g_poolShown=shown;}
        }

		void Resolve()
		{
			g_resolved = true;
            ResolveIds();
			logger::Write("City traffic: %s %s; %d configured vehicle/pilot pairs", kPlugin, g_present ? "loaded" : "not loaded; no city traffic", kN);
			g_bakedIds=citystartup::Snapshot().active;
            const auto poolPath=fvopaths::File(L"citypool-ids.tsv");
            bool validPool=false;g_assignments=citypool::Read(ReadLines(poolPath),validPool);
            if(GetFileAttributesW(poolPath.c_str())!=INVALID_FILE_ATTRIBUTES&&!std::ifstream(poolPath))validPool=false;
            g_poolReady=citystartup::Snapshot().ok&&validPool;
            if(!g_poolReady)logger::Write("City placements: metadata unavailable/invalid; pool management deferred");
			std::vector<trafficspots::In> in;
			for (const auto& s : ownspots::kSpots) in.push_back({ s.x, s.y, s.z, s.heading, s.key });
			ringschedule::Spot homes[ringschedule::kVehicles];
			trafficspots::Assign(in.data(), int(in.size()), homes);
			g_homeKeys.clear();
			for (const auto& h : homes)
				for (const auto& s : ownspots::kSpots)
					if (h.valid && h.x == double(s.x) && h.y == double(s.y) && h.z == double(s.z)) g_homeKeys.insert(s.key);
			ReadPoolFiles(true);
			// The traffic parks on the user's recorded spots, where FVO_CityPlacements.esm parks its REV-8s too.
			if (g_present && runtimeforms::Resolve({"FVO_CityPlacements.esm", 0x800}))
				logger::Write("City traffic: FVO_CityPlacements.esm is loaded too: its parked REV-8s stand on the traffic's home spots; disable one of the two");
		}
	}

	void Reset() { g_resolved = false;g_idsResolved=false; g_present = false; g_vehicles = {}; g_pilots = {};g_poolIds={}; g_poolShown = 0;g_at=0;g_synced.clear();g_filesReady=false;g_poolReady=false;g_seatRetry={};g_nextSeat=0; }

	int VehicleIndex(std::uint32_t formId)
	{
        ResolveIds();
		if (!g_present || !formId) return -1;
		for (int k = 0; k < kN; ++k) if (g_vehicles[k] == formId) return k;
		return -1;
	}
	bool IsTrafficVehicle(std::uint32_t formId) { return airtraffic::IsVehicle(formId)||VehicleIndex(formId) >= 0; }
    bool IsPoolVehicle(std::uint32_t id){ResolveIds();return id&&std::find(g_poolIds.begin(),g_poolIds.end(),id)!=g_poolIds.end();}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_at < kTickMs) return;
		g_at = now;
		if (!g_resolved) Resolve();
		if (!g_present) return;
		unsigned loaded = 0, seated = 0;
        unsigned attempts=0;const unsigned first=g_nextSeat;
		for (unsigned offset = 0; offset < kN; ++offset) {
            const auto k=(first+offset)%kN;
			const uptr v = Lookup(g_vehicles[k]), a = Lookup(g_pilots[k]);
			game::Vec3 vp{}, ap{};
			if (!Loaded(v, vp) || !Loaded(a, ap)) {g_seatRetry[k].Clear();continue;}
			++loaded;
			if (npcvehicles::IsDead(a)) continue;
			if (npcvehicles::VehicleOf(a) == v) { ++seated;g_seatRetry[k].Clear(); continue; }
			if(npcvehicles::VehicleOf(a)||npcvehicles::IsInCombat(a)||!realvehicles::CanManagePlacement(g_vehicles[k]))continue;
			// The pilot is out (just loaded, or its AI stood it up): back in, when it is at its vehicle.
			if (std::hypot(double(ap.x) - vp.x, double(ap.y) - vp.y) > kSeatReach) continue;
            if(attempts>=2||!g_seatRetry[k].Ready(now))continue;
			++attempts;g_nextSeat=(k+1)%kN;
			++g_seatTries;
			if (npcvehicles::SeatNow(a, v)) { ++seated; ++g_seatOk;g_seatRetry[k].Clear(); }
            else g_seatRetry[k].Failed(now);
		}
		SyncPool();
		if (loaded != g_loadedNow || seated != g_seatedNow || now - g_reportAt >= kReportMs) {
			g_reportAt = now;
			logger::Write("City traffic: %u of %d vehicles loaded with their pilots, %u pilots seated (seat attempts %u, %u worked)", loaded, kN, seated, g_seatTries, g_seatOk);
			g_loadedNow = loaded; g_seatedNow = seated;
		}
	}
}
