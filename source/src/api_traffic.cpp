// FVO core API v5 for FVO_Traffic.dll: engine operations require presentation and physical birth proofs.
// The traffic pool's decisions moved into the addon (traffic/generator.cpp); this file keeps only the engine work it needs, as
// the pool did it (the create / seat / persist / move steps confirmed in game 2026-10-25..29), and forwards core's own calls
// (main_vehicle's tick, reset and paused frames; the map editor's Start / Swap) to the addon's hooks. Without the addon these
// do nothing.
#include "trafficpool.h"
#include "trafficpair.h"
#include "trafficdiagnostics.h"
#include "vehiclekeep.h"
#include "traffic/traffic_api.h"
#include "cellbake.h"
#include "context.h"
#include "mappoints.h"
#include "navmap.h"
#include "addresses.h"
#include "features.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "placementenable.h"
#include "realvehicles.h"
#include "runtimecells.h"
#include "runtimeforms.h"
#include "fvolimits.h"
#include "fvopaths.h"
#include "vehicleworld.h"
#include "environment.h"
#include "vehicleflight.h"
#include "routeedit.h"
#include "inputgate.h"
#include "trafficarrival.h"
#include "vehiclediag.h"
#include "notify.h"
#include "lifecycle.h"
#include "traffic/admission.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include <set>
#include <mutex>
#include <algorithm>
#include <string>

namespace
{
	using uptr = std::uintptr_t;
	template <class T> T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
	uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
	template <class F> F EngineFn(std::uint32_t rva) { return reinterpret_cast<F>(game::Base() + rva); }
	constexpr std::uint32_t kFormFlags = 0x20, kDisabledFlag = 0x800, kDeletedFlag = 0x20, kBaseForm = 0x98;
	constexpr std::uint32_t kPersistentFlag = 0x400, kEssentialBit = 1u << 18;

	FvoTrafficHooks g_hooks{};
	std::atomic<bool> g_attached{ false };
    std::atomic<bool> g_worldActions{false};
    // 3 = the game's loading screen still hides the arrived world (core opens that window only after the save is read and
    // the world is ready); 2 = the local map covers it; 1 = gameplay; 0 = no world actions.
    std::atomic<int> g_arrivalRayChecked{0},g_arrivalMapOnly{0}; // arrival placements by kind of clearance check
    int ApiPresentation(){return trafficarrival::Covered()?3:!g_worldActions.load()?0:routeedit::MapOpen(100)&&!inputgate::GameplayAllowed()?2:1;}
    int ExposureAt(double,double,double);
    int ApiExposure(double x,double y,double z){return ExposureAt(x,y,z);}
    double ApiNoticeFar(unsigned base);
    int ApiInCameraView(double x,double y,double z);
    // May a vehicle be made here in play? Hidden by terrain, or far AND out of view, the standard the pair check's removals use (the user: "far and out of sight").
    // Terrain alone left open-air lane positions empty for good (2026-10-31: any one of 9 clear lines reads as seen; 32 of 94 stayed empty, site_wait over 1,000 per 5 s).
    int BirthHidden(unsigned base,double x,double y,double z,double px,double py){
        const int exposure=ExposureAt(x,y,z);
        const bool distant=std::hypot(x-px,y-py)>=ApiNoticeFar(base),inCamera=ApiInCameraView(x,y,z)!=0;
        const int hidden=exposure==2||(distant&&!inCamera);
        logger::Observe("traffic.fill",0,hidden?2u:distant?1u:0u,"event=hidden_check hidden=%d exposure=%d far=%d in_camera=%d",hidden,exposure,distant,inCamera);
        return hidden;
    }
    struct BirthProof {unsigned base=0,vehicle=0;double x=0,y=0,z=0,yaw=0;ULONGLONG at=0;std::string world;bool air=false;} g_birthProof;

	uptr Ref(std::uint32_t id)
	{
		if (!id) return 0;
		__try { const uptr r = runtimeforms::LookupChecked(id); return r && At<std::uint32_t>(r, off::kFormId) == id ? r : 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	// What each traffic id named when the traffic code made or adopted it (2026-10-30 18:43 crash). The game reuses an FF id once its reference
	// is deleted: a hidden spare driver's id, FF019072, had become a plain reference by the time the pilot recovery armed it, and the game's
	// package evaluation (actors only) read past the end of that object. Every traffic call on an id checks that it still names the same base
	// (and the right kind: a driver must be an actor, a vehicle must not); otherwise the id is treated as gone.
	std::mutex g_identityLock;
	std::map<unsigned, unsigned> g_identity;  // id -> base form id when made or adopted
	// A driver is only checked for still being an actor: traffic drivers have a runtime base (FF...) that the game makes again on a load (2026-10-30 20:27 run: 27 living
	// drivers read as "reused", 22 pairs given up on, and their drivers never made persistent). The id reuse that crashed became a plain reference: that still fails here.
	std::set<unsigned> g_identityActors;
	std::set<unsigned> g_identityTold;
	unsigned BaseIdOf(uptr r)
	{
		__try { const uptr b = At<uptr>(r, kBaseForm); return b ? At<std::uint32_t>(b, off::kFormId) : 0u; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
	}
	void RememberIdentity(unsigned id)
	{
		const uptr r = Ref(id);
		const unsigned b = r ? BaseIdOf(r) : 0;
		if (!id || !b) return;
		const bool actor = npcvehicles::LooksLikeActor(r);
		std::lock_guard lk(g_identityLock);
		g_identity[id] = b;
		if (actor) g_identityActors.insert(id); else g_identityActors.erase(id);
	}
	uptr TrafficRef(std::uint32_t id)
	{
		const uptr r = Ref(id);
		if (!r) return 0;
		std::lock_guard lk(g_identityLock);
		const auto it = g_identity.find(id);
		if (it == g_identity.end()) return r;
		const unsigned now = BaseIdOf(r);
		if (now == it->second) return r;
		if (g_identityActors.count(id) && npcvehicles::LooksLikeActor(r)) return r;
		if (g_identityTold.insert(id).second)
			logger::Write("FVO traffic: id %08X now names a different object (base %08X, was %08X): the game reused it after deleting the reference; treated as gone", id, now, it->second);
		return 0;
	}
	uptr ActorRef(std::uint32_t id) { const uptr r = TrafficRef(id); return r && npcvehicles::LooksLikeActor(r) ? r : 0; }
	uptr VehicleRef(std::uint32_t id) { const uptr r = TrafficRef(id); return r && !npcvehicles::LooksLikeActor(r) ? r : 0; }
	uptr RefFromHandle(std::uint32_t handle)
	{
		uptr ref = 0; uptr* out = &ref;
		struct { uptr vtbl; uptr** capture; } lookup{ game::Base() + addr::kHandleToRefFunctorVtbl, &out };
		__try { EngineFn<void (*)(uptr, std::uint32_t, void*)>(addr::kHandleLookup)(Ptr(addr::kRefHandleTablePtr), handle, &lookup); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		return ref;
	}
	bool PlayerCell(uptr& cell, uptr& ws)
	{
		__try {
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			cell = player ? At<uptr>(player, off::kRefParentCell) : 0;
			ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
			return ws != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}

	// One-step seated creation: the create-reference calls the creation data's vftable[0](data, newRef) after the new reference
	// has its base and place, before it joins a cell (stock: an empty ret). Ours seats the new driver.
	struct Birth { uptr vehicle = 0; int result = -99; } g_birth;
	void SeatInside(uptr ref)
	{
		__try { g_birth.result = npcvehicles::SeatNewborn(ref, g_birth.vehicle); }
		__except (EXCEPTION_EXECUTE_HANDLER) { g_birth.result = -1; }
	}
	void OnCreated(void*, void* ref) { if (g_birth.vehicle && ref) SeatInside(reinterpret_cast<uptr>(ref)); }
	void* const kSeatedCreation[1] = { reinterpret_cast<void*>(&OnCreated) };
    void OnStagedCreated(void*,void* ref){if(ref)vehiclediag::StageBirth(At<unsigned>(reinterpret_cast<uptr>(ref),off::kFormId));}
    void* const kStagedCreation[1]={reinterpret_cast<void*>(&OnStagedCreated)};

	std::uint32_t Create(uptr base, uptr cell, uptr ws, double x, double y, double z, double heading, const void* vtbl = nullptr)
	{
		const uptr data = Ptr(addr::kDataHandlerPtr);
		if (!base || !cell || !ws || !data) return 0;
		alignas(16) std::uint8_t d[0xC0] = {};
		const auto dp = reinterpret_cast<uptr>(d);
		At<uptr>(dp, 0x00) = vtbl ? reinterpret_cast<uptr>(vtbl) : game::Base() + addr::kNewRefrDataVtbl;
		At<float>(dp, 0x10) = float(x); At<float>(dp, 0x14) = float(y); At<float>(dp, 0x18) = float(z);
		At<float>(dp, 0x24) = float(heading); At<float>(dp, 0x28) = 1.0f;
		At<uptr>(dp, 0x30) = base; At<uptr>(dp, 0x38) = cell; At<uptr>(dp, 0x40) = ws;
		At<std::uint8_t>(dp, 0x8C) = 1; At<std::uint8_t>(dp, 0x8D) = 1; At<std::uint32_t>(dp, 0x98) = 1;
		std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
		__try { EngineFn<void* (*)(uptr, std::uint64_t*, void*)>(addr::kCreateReference)(data, &handle, d); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		const uptr ref = RefFromHandle(static_cast<std::uint32_t>(handle));
		if (!ref) return 0;
		std::uint32_t id = 0;
		__try { id = At<std::uint32_t>(ref, off::kFormId); EngineFn<void(*)(uptr,char,char)>(addr::kMarkerPost1)(ref,1,0);EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return id;
	}
	struct EnableEngine {
		uptr ref;
		void CancelRequests() { EngineFn<void (*)(uptr)>(addr::kEnableReference)(ref); }
		bool Disabled() const { return (At<std::uint32_t>(ref, kFormFlags) & kDisabledFlag) != 0; }
		void Enable() { EngineFn<void (*)(uptr, char)>(addr::kRefEnableTask)(ref, 1); }
	};

	// ---- API v2 ----
	int ApiPlayer(FvoTrafficPlayer* out)
	{
		if (!out) return 0;
		out->world[0] = 0;
		__try {
			const uptr player = Ptr(addr::kPlayerCharacterPtr);
			const uptr cell = player ? At<uptr>(player, off::kRefParentCell) : 0;
			const uptr ws = cell ? At<uptr>(cell, off::kCellWorldspace) : 0;
			if (!ws) return 0;
			const auto pos = At<game::Vec3>(player, off::kRefPosition);
			out->x = pos.x; out->y = pos.y; out->z = pos.z;
			auto** vt = *reinterpret_cast<void***>(ws);
			const char* n = reinterpret_cast<const char* (*)(uptr)>(vt[0x4D])(ws);
			int i = 0;
			for (; n && n[i] && i < 200; ++i) out->world[i] = n[i];
			out->world[i] = 0;
			return i > 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	int ApiTrafficOn() { return features::On(features::Id::Traffic) ? 1 : 0; }
    int ExposureAt(double x,double y,double z){
        FvoTrafficPlayer pl{};if(!ApiPresentation()||!ApiPlayer(&pl))return 0;
        if(std::hypot(x-pl.x,y-pl.y)<150)return 1;
        bool pending=false;
        // Conservatively sample the envelope allowed by SpawnSite's bounded models.
        // Facing away is never concealment. Unknown probes defer admission.
        for(const double side:{-40.0,0.0,40.0})for(const double depth:{-40.0,0.0,40.0}){
            const float from[]={float(pl.x),float(pl.y),float(pl.z+2)};
            const float to[]={float(x+side),float(y+depth),float(z+42)};
            double hx=0,hy=0,hz=0;bool hit=false;
            if(!environment::RayHit(from,to,hx,hy,hz,hit)){pending=true;continue;}
            const double full=std::hypot(std::hypot(to[0]-from[0],to[1]-from[1]),to[2]-from[2]);
            if(!hit||std::hypot(std::hypot(hx-from[0],hy-from[1]),hz-from[2])>full-10)return 1;
        }
        return pending?0:2;
    }
    int ApiBirthSite(unsigned base,double x,double y,double z,double heading,double* originZ){
        if(!ApiPresentation()||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))return 0;
        FvoTrafficPlayer pl{};if(!ApiPlayer(&pl)||!trafficadmission::ValidOrigin(x,y))return 0;
        const auto site=environment::SpawnSite(base,x,y,z,heading);
        if(site.verdict==environment::Verdict::Pending)return 0;
        if(site.verdict!=environment::Verdict::Safe)return -1;
        if(originZ)*originZ=site.z;
        g_birthProof={base,0,x,y,site.z,heading,GetTickCount64(),pl.world};return 1;
    }
	int ApiRef(unsigned id, FvoTrafficRef* out)
	{
		if (!out) return 0;
		*out = FvoTrafficRef{};
		const uptr r = TrafficRef(id);
		if (!r) return 0;
		out->id = id;
		__try {
			const uptr b = At<uptr>(r, kBaseForm);
			out->baseId = b ? At<std::uint32_t>(b, off::kFormId) : 0;
			const std::uint32_t flags = At<std::uint32_t>(r, kFormFlags);
			out->usable = (flags & (kDisabledFlag | kDeletedFlag)) == 0;
			out->disabled = (flags & kDisabledFlag) != 0;
			const uptr ld = At<uptr>(r, off::kRefLoadedData);
			out->has3D = ld && At<uptr>(ld, off::kLoadedData3D);
			const auto pos = At<game::Vec3>(r, off::kRefPosition);
			out->x = pos.x; out->y = pos.y; out->z = pos.z;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		out->isActor = npcvehicles::LooksLikeActor(r) ? 1 : 0;
		out->dead = out->isActor && npcvehicles::IsDead(r) ? 1 : 0;
		return 1;
	}
	int ApiFormExists(unsigned id) { return id && runtimeforms::Lookup(id) ? 1 : 0; }
    int ApiHullSize(unsigned base,double* radius,double* halfHeight){
        __try{
            const auto form=runtimeforms::Lookup(base);if(!form)return 0;
            double half[3]{};for(int i=0;i<3;++i){const double lo=At<float>(form,0x50+i*4),hi=At<float>(form,0x5C+i*4);
                if(!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo||hi-lo>40||std::max(std::fabs(lo),std::fabs(hi))>40)return 0;
                half[i]=std::max(std::fabs(lo),std::fabs(hi));}
            if(radius)*radius=std::hypot(half[0],half[1])+1;if(halfHeight)*halfHeight=half[2]+1;return 1;
        }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    }
	int ApiVehicles(FvoTrafficVehicle* out, int max)
	{
		static thread_local world::Snapshot snap;
		if (!world::Vehicles(snap)) return -1;
		int n = 0;
		for (std::uint32_t k = 0; k < snap.count && n < max; ++k, ++n) {
			const auto& v = snap.v[k];
			int who = kFvoWhoNone;
			switch (v.who) {
			case world::VehicleInfo::Who::Player: who = kFvoWhoPlayer; break;
			case world::VehicleInfo::Who::Npc: who = kFvoWhoNpc; break;
			case world::VehicleInfo::Who::Parked: who = kFvoWhoParked; break;
			default: break;
			}
            out[n] = { v.formId, v.baseId, who, v.pos[0], v.pos[1], v.pos[2], trafficpair::Body(v.bodyId) ? 1 : 0 };
            ApiHullSize(v.baseId,&out[n].radius,&out[n].halfHeight);
            out[n].driver = v.driverId;
		}
		return n;
	}
	int ApiGround(double x, double y, double zQuery, double* nav, double* baked)
	{
		int known = 0;
		const auto m = navmap::At(x, y, zQuery);
		if (m.haveGround && std::isfinite(m.ground)) { known |= 1; if (nav) *nav = m.ground; }
		double b = 0;
		if (cellbake::Ground(x, y, b)) { known |= 2; if (baked) *baked = b; }
		return known;
	}
	// How old a birth's site check may be when the game makes the reference. In play 250 ms. Behind the loading screen the
	// main loop's reduced steps take up to 600 ms each (15:32 run, log lines 261 and 532) and the commands run after them
	// (lifecycle_cover.h), so 2 s there; nothing can be seen move behind the screen.
	unsigned long long ProofLimitMs(int screen){return screen==3?2000:250;}
	std::string MutationKey(const char* operation,unsigned id,double x=0,double y=0,double z=0,double heading=0) {
		char key[180];sprintf_s(key,"%s:%08X:%a:%a:%a:%a",operation,id,x,y,z,heading);return key;
	}
	unsigned ApiCreateVehicle(unsigned baseId, double x, double y, double z, double heading)
	{
		const auto result=lifecycle::Poll(MutationKey("create",baseId,x,y,z,heading),baseId,[=]()->lifecycle::Work {
			FvoTrafficPlayer pl{};const int screen=ApiPresentation();
			logger::Write("OBS system=create.vehicle vehicle=00000000 event=request base=%08X pos=%.2f,%.2f,%.2f yaw=%.3f screen=%d key=%s",baseId,x,y,z,heading,screen,MutationKey("create",baseId,x,y,z,heading).c_str());
			const BirthProof proof=g_birthProof;
			const unsigned long long proofAge=GetTickCount64()-proof.at;
			const char* why=!screen?"no_presentation":!ApiPlayer(&pl)?"no_player":proof.base!=baseId?"proof_other_base":proof.world!=pl.world?"proof_other_world":
				proofAge>ProofLimitMs(screen)?"proof_expired":
				(std::fabs(x-proof.x)>0.01||std::fabs(y-proof.y)>0.01||std::fabs(z-proof.z)>0.01||std::fabs(heading-proof.yaw)>0.01)?"proof_other_position":
				(screen<2&&!BirthHidden(baseId,x,y,z,pl.x,pl.y))?"exposed":nullptr;
			if(why){logger::Write("OBS system=create.vehicle vehicle=00000000 event=refused stage=request reason=%s base=%08X proof_base=%08X proof_age_ms=%llu screen=%d",why,baseId,proof.base,proofAge,screen);return {};}
			g_birthProof.base=0;
			return [=]()->std::uint64_t {
				FvoTrafficPlayer current{};uptr cell=0,ws=0;
				const unsigned long long age=GetTickCount64()-proof.at;
				const char* late=!lifecycle::InService()?"out_of_service":!ApiPresentation()?"no_presentation":!ApiPlayer(&current)?"no_player":proof.world!=current.world?"world_changed":
					(ApiPresentation()<2&&(screen>=2||std::hypot(current.x-pl.x,current.y-pl.y)>2))?"player_moved_or_cover_gone":
					age>ProofLimitMs(ApiPresentation())?"proof_expired":!PlayerCell(cell,ws)?"no_player_cell":nullptr;
				if(late){logger::Write("OBS system=create.vehicle vehicle=00000000 event=refused stage=service reason=%s base=%08X proof_age_ms=%llu screen=%d",late,baseId,age,ApiPresentation());return 0;}
				const unsigned id=Create(runtimeforms::Lookup(baseId),cell,ws,x,y,z,heading,proof.air?kStagedCreation:nullptr);
				if(!id)logger::Write("OBS system=create.vehicle vehicle=00000000 event=refused stage=engine reason=create_returned_nothing base=%08X",baseId);
				return id;
			};
		});
		if(!result)return 0;
		const auto id=unsigned(*result);RememberIdentity(id);
		logger::Write("OBS system=create.vehicle vehicle=%08X event=completion base=%08X ok=%d pos=%.2f,%.2f,%.2f",id,baseId,id!=0,x,y,z);
		if(id){g_birthProof.vehicle=id;g_birthProof.at=GetTickCount64();}
		return id;
	}
	unsigned ApiCreateSeatedDriver(unsigned baseId, unsigned vehicleId, double x, double y, double z, double heading, int* seatResult)
	{
		if (seatResult) *seatResult = -99;
		const auto result=lifecycle::Poll(MutationKey("driver",vehicleId),vehicleId,[=]()->lifecycle::Work {
			logger::Write("OBS system=create.driver vehicle=%08X event=request base=%08X pos=%.2f,%.2f,%.2f",vehicleId,baseId,x,y,z);
			const auto owned=lifecycle::Keep(VehicleRef(vehicleId),vehicleId);FvoTrafficPlayer pl{};
			const char* why=!owned?"no_vehicle":!ApiPresentation()?"no_presentation":!ApiPlayer(&pl)?"no_player":nullptr;
			if(!why){
				const bool birth=g_birthProof.vehicle==vehicleId&&GetTickCount64()-g_birthProof.at<=ProofLimitMs(ApiPresentation());
				if(!birth&&ApiPresentation()<2&&ApiExposure(x,y,z)!=2)why="exposed";
			}
			if(why){logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=request reason=%s base=%08X",vehicleId,why,baseId);return {};}
			const int screen=ApiPresentation();
			return [=]()->std::uint64_t {
				uptr cell=0,ws=0;FvoTrafficPlayer current{};
				const char* late=!lifecycle::InService()?"out_of_service":!ApiPresentation()?"no_presentation":!ApiPlayer(&current)?"no_player":std::strcmp(pl.world,current.world)?"world_changed":
					(ApiPresentation()<2&&(screen>=2||std::hypot(pl.x-current.x,pl.y-current.y)>2))?"player_moved_or_cover_gone":!PlayerCell(cell,ws)?"no_player_cell":nullptr;
				if(late){logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=%s base=%08X",vehicleId,late,baseId);return 0;}
				const uptr vehicle=reinterpret_cast<uptr>(owned.get());
				const unsigned vehicleFlags=At<unsigned>(vehicle,kFormFlags);
				if((vehicleFlags&(kDisabledFlag|kDeletedFlag))||!trafficadmission::ValidOrigin(x,y)){
					logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=%s base=%08X flags=%08X",vehicleId,(vehicleFlags&(kDisabledFlag|kDeletedFlag))?"vehicle_hidden_or_deleted":"invalid_origin",baseId,vehicleFlags);
					return 0;
				}
				const Birth previous=g_birth;g_birth={vehicle,-99};
				const unsigned id=Create(runtimeforms::Lookup(baseId),cell,ws,x,y,z,heading,kSeatedCreation);
				const auto seat=g_birth.result;g_birth=previous;
				return std::uint64_t(id)|(std::uint64_t(std::uint32_t(seat))<<32);
			};
		});
		if(!result)return 0;
		const unsigned id=unsigned(*result);if(seatResult)*seatResult=int(*result>>32);RememberIdentity(id);
		logger::Write("OBS system=create.driver vehicle=%08X event=completion driver=%08X seat_result=%d ok=%d",vehicleId,id,int(*result>>32),id!=0);return id;
	}
	std::map<unsigned, unsigned> g_guardOthers;  // seat-linked run-time drivers no registry knows (driver -> vehicle), refreshed by the guard
	std::map<unsigned, float> g_guardHidden;     // drivers the guard has shrunk, with the scale to give back
	struct AirRescue { double x = 0, y = 0, z = 0; ULONGLONG at = 0; };
	std::map<unsigned, AirRescue> g_airRescue;   // a stable target per driver: the move request is keyed by its position
	std::map<unsigned, unsigned> g_madeDrivers;  // driver -> vehicle, for every pair the addon made or reused since the load
	unsigned g_lastMadeVehicle = 0;
	std::set<unsigned> g_madeVehicles;  // every vehicle the addon made or reused since the load
	void RememberDriver(unsigned id) { g_madeDrivers[id] = g_lastMadeVehicle; }
	std::map<unsigned,int> g_unpersisted; // id -> actor; as desired visibility, survives a full table or a job's cancel: a reference that never turns persistent can never be deleted
	void QueuePersist(unsigned id,int actor)
	{
		const uptr r = TrafficRef(id);
		if (!r) return;
		lifecycle::Post(MutationKey("persist",id),id,[=]()->lifecycle::Work {
			const auto owned=lifecycle::Keep(r,id);if(!owned)return {};
			return [=]()->std::uint64_t {
				if(!lifecycle::InService())return 0;const uptr ref=reinterpret_cast<uptr>(owned.get());
				if(!(At<unsigned>(ref,kFormFlags)&kPersistentFlag))EngineFn<void(*)(uptr,char,char)>(addr::kMarkerPost1)(ref,1,0);
				if(actor)At<unsigned>(ref,0x378)&=~kEssentialBit;
				return 1;
			};
		});
	}
	int ApiMakeTraffic(unsigned id, int actor)
	{
		const uptr r = TrafficRef(id);
		if (!r) return 0;
        if(actor&&!npcvehicles::LooksLikeActor(r))return 0;
		RememberIdentity(id);  // made, or adopted from the shelf: what this id names from now on
		// Registries belong to the controller, never the engine callback.
		if(actor)RememberDriver(id);else {g_lastMadeVehicle=id;g_madeVehicles.insert(id);}
		g_unpersisted[id]=actor;QueuePersist(id,actor);
		return (At<unsigned>(r,kFormFlags)&kPersistentFlag)?1:0;
	}
	std::map<unsigned,int> g_visibility; // desired state survives a full service queue
	// Behind the loading screen kDisableReference only lists the reference (FUN_140C10660); the list is applied by
	// FUN_140C109C0, called only from the full step's job FUN_1417CA1E0, so the hide lands as the screen closes. Applying
	// it behind the screen (vt+0x7E8) hung the game in both save loads of 2026-10-31 19:28 and 19:37: main spun in
	// FUN_142404230 under FUN_1418C1AD0 while the background jobs were still loading. g_coverHides times each landing.
	std::mutex g_coverHideLock;
	std::map<unsigned,ULONGLONG> g_coverHides;
	void QueueVisibility(unsigned id,int on)
	{
		if (!on) vehiclediag::DropBirthHold(id);  // a hidden traffic vehicle is never held again
		const uptr ref = TrafficRef(id);
		if (!ref) return;
		lifecycle::Post(MutationKey(on?"enable":"disable",id),id,[=]()->lifecycle::Work {
			const auto owned=lifecycle::Keep(ref,id);if(!owned)return {};
			return [=]()->std::uint64_t {
				if(!lifecycle::InService())return 0;const uptr r=reinterpret_cast<uptr>(owned.get());
				if(At<unsigned>(r,kFormFlags)&kDeletedFlag)return 0;
				const bool disabled=(At<unsigned>(r,kFormFlags)&kDisabledFlag)!=0;
				if(on&&disabled){EnableEngine engine{r};placementenable::Restore(engine);}
				if(!on&&!disabled){
					EngineFn<void(*)(uptr,char)>(addr::kDisableReference)(r,0);
					if(lifecycle::InCover()){
						std::lock_guard lock(g_coverHideLock);
						if(g_coverHides.emplace(id,GetTickCount64()).second)logger::Write("OBS system=traffic.visibility vehicle=%08X event=listed_under_cover",id);
					}
				}
				return 1;
			};
		});
	}
	// Records the wanted state only; ServiceVisibility sends it, paced (15:32 run: the old-save sweep sent 110 hides at once
	// and filled the 64-entry table, leaving no room for a single birth).
	void ApiShow(unsigned id,int on){if(id){if(!on)vehiclediag::DropBirthHold(id);g_visibility[id]=on?1:0;}}
	constexpr std::size_t kStateInFlight=24; // visibility + persistence requests outstanding at once; the rest of the table stays free for births
	// A pair marked persistent can lose the flag later (2026-10-31 19:38 run: FF014928 was persistent at 19:39:44, flags 0x80A at its delete; its driver's lookup had
	// failed by 19:39:52 and that id came back as another actor). Without it the game frees the reference. Checked once a second, put back by the same request.
	void WatchPersistence(){
		static ULONGLONG checkedAt=0;const ULONGLONG now=GetTickCount64();
		if(now-checkedAt<1000)return;
		checkedAt=now;
		auto watch=[](unsigned id,int actor){
			const uptr r=TrafficRef(id);
			if(!r)return;
			const auto flags=At<unsigned>(r,kFormFlags);
			if((flags&kPersistentFlag)||(flags&kDeletedFlag)||g_unpersisted.count(id))return;
			logger::ObserveReference("traffic.persist",id,1,"event=lost actor=%d flags=%08X",actor,flags);
			g_unpersisted[id]=actor;
		};
		for(const unsigned id:g_madeVehicles)watch(id,0);
		for(const auto& [id,vehicle]:g_madeDrivers)watch(id,1);
	}
	void ServiceVisibility(){
		WatchPersistence();
		static trafficpair::HidePace hidePace;
		int submitted=0,paced=0;
		const std::size_t busy=lifecycle::Outstanding("enable")+lifecycle::Outstanding("disable")+lifecycle::Outstanding("persist");
		const int room=busy>=kStateInFlight?0:int(std::min<std::size_t>(16,kStateInFlight-busy));
		for(auto it=g_visibility.begin();it!=g_visibility.end();){
			const unsigned id=it->first;const auto ref=TrafficRef(id);
			if(!ref||(At<unsigned>(ref,kFormFlags)&kDeletedFlag)){it=g_visibility.erase(it);continue;}
			const bool pending=lifecycle::Pending(id,"enable")||lifecycle::Pending(id,"disable");
			const bool enabled=(At<unsigned>(ref,kFormFlags)&kDisabledFlag)==0;
			const int screen=ApiPresentation();
			{
				std::lock_guard lock(g_coverHideLock);
				const auto listed=g_coverHides.find(id);
				if(listed!=g_coverHides.end()){
					if(!enabled){
						logger::Write("OBS system=traffic.visibility vehicle=%08X event=hide_landed after_ms=%llu screen=%d",id,static_cast<unsigned long long>(GetTickCount64()-listed->second),screen);
						g_coverHides.erase(listed);
					}
					else if(screen==3&&!it->second){++it;continue;}  // listed already; the engine applies it when the screen closes
				}
			}
			if(!pending&&enabled==bool(it->second)){it=g_visibility.erase(it);continue;}
			if(!pending&&submitted<room){
				bool send=true;
				// Under the paused map a hide cannot land until play resumes: it stays listed, and asking again each update only fills the queue and the log (2026-10-31: 684 in 10 s).
				static std::map<unsigned,ULONGLONG> hideAskedAt;
				const ULONGLONG askedNow=GetTickCount64();
				if(!it->second&&enabled){
					auto& asked=hideAskedAt[id];
					if(asked&&askedNow-asked<2000){++it;continue;}
				}
				if(hideAskedAt.size()>512)hideAskedAt.clear();
				if(!it->second&&enabled){
					const uptr loaded=At<uptr>(ref,off::kRefLoadedData);
					const bool hasModel=loaded&&At<uptr>(loaded,off::kLoadedData3D);
					FvoTrafficPlayer pl{};
					const auto at=At<game::Vec3>(ref,off::kRefPosition);
					const double distance=ApiPlayer(&pl)?std::hypot(at.x-pl.x,at.y-pl.y):0.0;  // player unknown: as near, never held back
					send=trafficpair::MayHide(hasModel,distance,screen,hidePace,GetTickCount64());
					if(!send)++paced;
					// Could the player see it go? 1 in sight, 2 out of sight, 0 not known yet. Only worth the rays for a model close enough to make out.
					else logger::Write("OBS system=traffic.visibility vehicle=%08X event=hide_sent model=%d distance=%.0f seen=%d screen=%d",id,hasModel,distance,hasModel&&distance<600?ApiExposure(at.x,at.y,at.z):2,screen);
				}
				if(send){++submitted;if(!it->second&&enabled)hideAskedAt[id]=askedNow;QueueVisibility(id,it->second);}
			}
			++it;
		}
		logger::Observe("traffic.visibility",0,paced?1u:0u,"event=hide_pace waiting=%d tokens=%.1f",paced,hidePace.tokens);
		for(auto it=g_unpersisted.begin();it!=g_unpersisted.end();){
			const unsigned id=it->first;const auto ref=TrafficRef(id);
			if(!ref){it=g_unpersisted.erase(it);continue;}
			const auto flags=At<unsigned>(ref,kFormFlags);const bool pending=lifecycle::Pending(id,"persist");
			const bool settled=(flags&kPersistentFlag)&&!(it->second&&(At<unsigned>(ref,0x378)&kEssentialBit));  // an essential driver could not be shot
			if(!pending&&((flags&kDeletedFlag)||settled)){it=g_unpersisted.erase(it);continue;}
			if(!pending&&submitted<room){++submitted;QueuePersist(id,it->second);}
			++it;
		}
	}
    int ApiMoveTo(unsigned id,double x,double y,double z,double heading)
    {
        // Poll BEFORE fresh preconditions: after a move the original position,
        // visibility and cell may have changed. Its acknowledgement still must
        // be consumed rather than mistaken for another new command.
        const auto result=lifecycle::Poll(MutationKey("move",id,x,y,z,heading),id,[=]()->lifecycle::Work {
            const int screen=ApiPresentation();FvoTrafficPlayer pl{};
            if(!screen||!ApiPlayer(&pl)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))return {};
            const auto owned=lifecycle::Keep(TrafficRef(id),id);if(!owned)return {};
            const auto sourceRef=reinterpret_cast<uptr>(owned.get());
            const auto source=At<game::Vec3>(sourceRef,off::kRefPosition);
            const uptr loaded=At<uptr>(sourceRef,off::kRefLoadedData);
            const bool hasModel=loaded&&At<uptr>(loaded,off::kLoadedData3D);
            if(screen<2&&hasModel&&(ApiExposure(source.x,source.y,source.z)!=2||ApiExposure(x,y,z)!=2))return {};
            return [=]()->std::uint64_t {
                const auto ref=reinterpret_cast<uptr>(owned.get());
                uptr cell=0,world=0;FvoTrafficPlayer current{};
                if(!lifecycle::InService()||!ApiPresentation()||!ApiPlayer(&current)||std::strcmp(pl.world,current.world)||
                   (ApiPresentation()<2&&(screen>=2||std::hypot(pl.x-current.x,pl.y-current.y)>2))||!PlayerCell(cell,world))return 0;
                const auto start=At<game::Vec3>(ref,off::kRefPosition);
                if(std::hypot(std::hypot(start.x-source.x,start.y-source.y),start.z-source.z)>1)return 0;
                if(At<unsigned>(ref,kFormFlags)&kDeletedFlag)return 0;
                float pos[]={float(x),float(y),float(z)},rot[]={0.0f,0.0f,float(heading)};
                std::uint64_t handle=*reinterpret_cast<unsigned*>(game::Base()+addr::kInvalidHandle);
                EngineFn<void(*)(uptr,std::uint64_t*,uptr,uptr,float*,float*)>(addr::kMoveReference)(ref,&handle,cell,world,pos,rot);
                return 1;
            };
        });
        return result?int(*result):0;
    }

	// Not ApiMoveTo: that one refuses within 150 m and whenever the player has moved 2 m, so a stranded driver was never set down (2026-10-31: 7 requests, 0 moves). The caller checks the camera.
	void RescueDriver(unsigned id, double x, double y, double z)
	{
		const uptr r = TrafficRef(id);
		if (!r) return;
		lifecycle::Post("rescue:" + std::to_string(id), id, [=]() -> lifecycle::Work {
			const auto owned = lifecycle::Keep(r, id);
			if (!owned) return {};
			return [=]() -> std::uint64_t {
				uptr cell = 0, world = 0;
				const uptr ref = reinterpret_cast<uptr>(owned.get());
				if (!lifecycle::InService() || !PlayerCell(cell, world) || (At<unsigned>(ref, kFormFlags) & (kDeletedFlag | kDisabledFlag))) {
					logger::Write("OBS system=driver.rescue reference=%08X event=refused flags=%08X", id, At<unsigned>(ref, kFormFlags));
					return 0;
				}
				float pos[] = { float(x), float(y), float(z) }, rot[] = { 0.0f, 0.0f, 0.0f };
				std::uint64_t handle = *reinterpret_cast<unsigned*>(game::Base() + addr::kInvalidHandle);
				EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(ref, &handle, cell, world, pos, rot);
				logger::Write("OBS system=driver.rescue reference=%08X event=set_down pos=%.1f,%.1f,%.1f", id, x, y, z);
				return 1;
			};
		});
	}
	unsigned ApiVehicleOf(unsigned actorId)
	{
		const uptr d = ActorRef(actorId);
		const uptr v = d ? npcvehicles::VehicleOf(d) : 0;
		if (!v) return 0;
		__try { return At<std::uint32_t>(v, off::kFormId); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	int ApiSitState(unsigned actorId) { const uptr d = ActorRef(actorId); return d ? npcvehicles::SitState(d) : -1; }
	int ApiSnapSeat(unsigned actorId, unsigned vehicleId)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		if (d && v) npcvehicles::BringToVehicle(d, v);
		return d && v ? npcvehicles::SnapNow(d, v) : -1;
	}
	int ApiUsesVehicle(unsigned vehicleId) { return npcvehicles::UsesVehicle(vehicleId) ? 1 : 0; }
    int ApiTripActive(unsigned id){return npcvehicles::UsesVehicle(id)||vehicleflight::RouteTripActive(id);}
	void ApiEndJob(unsigned vehicleId, const char* why) { npcvehicles::EndJobOf(vehicleId, why ? why : "traffic"); }
	int ApiTripRefusal(unsigned actorId, unsigned vehicleId, char* why, int size)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		const char* r = d && v ? npcvehicles::ScriptedRefusal(d, v) : "vehicle or driver gone";
		if (!r || !*r) return 0;
		if (why && size > 0) { strncpy_s(why, size_t(size), r, _TRUNCATE); }
		return 1;
	}
	int ApiStartTrip(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		return d && v && claim && npcvehicles::StartScriptedSeated(d, v, *claim) ? 1 : 0;
	}
	unsigned FormIdOf(uptr form)
	{
		__try { return form ? At<std::uint32_t>(form, off::kFormId) : 0u; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0u; }
	}
	int ApiVehicleTypes(unsigned* out, int max)
	{
		if (!out || max <= 0) return 0;
		struct Type { unsigned id; long long key; };
		std::vector<Type> types;
		for (const uptr form : realvehicles::VehicleTypes()) {
			const unsigned id = FormIdOf(form);
			double radius = 0, half = 0;
			if (!id) continue;
			// Seat data first (2026-10-30): a model whose base lists no driver's seat never joins traffic.
			if (!npcvehicles::BaseHasDriverSeat(form)) {
				static std::set<unsigned> told;
				if (told.insert(id).second) logger::Write("Traffic models: %08X lists no driver's seat; left out of traffic", id);
				continue;
			}
			// Variants of one model share its bounds: one entry per model, so four Deimog variants count as one Deimog. A model
			// without stored bounds (most mod vehicles: 2026-10-29 22:50 run, 20 of 25 dropped) stays in on its own; spacing and
			// the spawn check use a default envelope for it.
			const long long key = ApiHullSize(id, &radius, &half) ? (long long)std::llround(radius * 10.0) * 100000 + (long long)std::llround(half * 10.0)
				: -(long long)id;
			bool seen = false;
			for (auto& t : types) if (t.key == key) { seen = true; if (id < t.id) t.id = id; break; }
			if (!seen) types.push_back({ id, key });
		}
		std::sort(types.begin(), types.end(), [](const Type& a, const Type& b) { return a.id < b.id; });
		int n = 0;
		for (const auto& t : types) { if (n >= max) break; out[n++] = t.id; }
		return n;
	}
	int ApiMappedSpot(unsigned base, double x, double y, double z, double heading)
	{
		if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(heading)) return 0;
		const auto site = environment::MappedSite(base, x, y, z, heading);
		return site.verdict == environment::Verdict::Safe ? 1 : site.verdict == environment::Verdict::Unsafe ? -1 : 0;
	}
	int ApiPilotHolds(unsigned vehicleId) { return vehicleflight::PilotHolds(vehicleId) ? 1 : 0; }
	int ApiPilotLive(unsigned vehicleId) { const int live = vehicleflight::PilotLive(vehicleId); return live == 0 && !inputgate::GameplayAllowed() ? 1 : live; }
	int ApiTrafficPilotFree() { return npcvehicles::TrafficPilotFree() ? 1 : 0; }
	int ApiStartArmed(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		return d && v && claim && npcvehicles::StartTrafficArmed(d, v, *claim) ? 1 : 0;
	}
	int ApiCanManagePlacement(unsigned vehicleId) { return realvehicles::CanManagePlacement(vehicleId) ? 1 : 0; }

	// ---- v2: everyday traffic ----
	int ApiMapPlaces(double* xy, int maxPlaces)
	{
		FvoTrafficPlayer pl{};
		if (!xy || maxPlaces <= 0 || !ApiPlayer(&pl)) return 0;
		std::vector<mappoints::Point> pts;
		const float p3[3] = { float(pl.x), float(pl.y), float(pl.z) };
		if (!mappoints::WorldPoints(p3, pts)) return 0;
		int n = 0;
		for (const auto& q : pts) {
			if (n >= maxPlaces) break;
			xy[2 * n] = q.x; xy[2 * n + 1] = q.y; ++n;
		}
		return n;
	}
	int ApiCivilJobs() { return npcvehicles::CivilJobs(); }
	int ApiMayUse(unsigned actorId, unsigned vehicleId)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		return d && v && npcvehicles::MayUse(d, v) ? 1 : 0;
	}
	int ApiStartScripted(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		return d && v && claim && npcvehicles::StartScripted(d, v, *claim) ? 1 : 0;
	}
	int ApiStartSupply(unsigned actorId, unsigned vehicleId, double x, double y)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		return d && v && npcvehicles::StartSupply(d, v, x, y) ? 1 : 0;
	}
	int ApiStartPatrol(unsigned actorId, unsigned vehicleId)
	{
		const uptr d = ActorRef(actorId), v = VehicleRef(vehicleId);
		return d && v && npcvehicles::StartPatrol(d, v) ? 1 : 0;
	}

    int ApiAirSite(unsigned base,double x,double y,double z,double heading){
        FvoTrafficPlayer pl{};
        const int screen=ApiPresentation();
        if(!vehiclediag::BirthStageAvailable()||!screen||!ApiPlayer(&pl)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))return 0;
        if(!trafficadmission::ValidOrigin(x,y))return 0;
        if(screen<2&&!BirthHidden(base,x,y,z,pl.x,pl.y))return 0; // in play only where the player cannot see; covered spots wait
        if(screen==3&&!trafficarrival::seeded)return 0; // behind the loading screen: wait for the nav map's first pass
        const auto site=environment::AirSite(base,x,y,z,heading);
        // Behind the loading screen and under the paused map (Start/Swap's panel) FVO's physics rays are not answered. There the
        // authored lane plus the nav map's solids are the check, and the chassis stays frozen until its autopilot holds it.
        // In play: wait for the rays.
        if(site.verdict==environment::Verdict::Pending&&screen<2)return 0;
        if(site.verdict==environment::Verdict::Unsafe)return -1;
        ++(site.verdict==environment::Verdict::Safe?g_arrivalRayChecked:g_arrivalMapOnly);
        g_birthProof={base,0,x,y,z,heading,GetTickCount64(),pl.world,true};return 1;
    }
    void ApiArrivalProgress(int expected,int ready){trafficarrival::Progress(expected,ready);}
    int ApiReleaseBirth(unsigned id){return vehiclediag::ReleaseBirth(id)?1:0;}

	// The driver watch (2026-10-30, the user: "I cant be providing NPC names for you each time"). Every check before this one looked at
	// the pairs FVO believed were in trouble, so each time the people on the cliffs were on a path nothing watched (the 10:31 run: 54 of
	// 93 pairs had no line after their birth). This one starts from the game's side: every actor in an attached cell with a seat link,
	// plus every driver traffic made, wherever FVO thinks they are, and judges what the player would see. It logs each one with no
	// console click needed, and as a last resort hides a traffic pair whose driver has no job and no vehicle around them, whatever the reason.
	struct Watched { trafficpair::Seen seen = trafficpair::Seen::Seated; ULONGLONG since = 0; bool told = false; };
	std::map<unsigned, Watched> g_watched;
	struct PairRecord { trafficpair::PairHealth health = trafficpair::PairHealth::Unproven; ULONGLONG notSeatedSince = 0; };
	std::map<unsigned, PairRecord> g_pairHealth;  // by vehicle id
	bool g_healthLaunch = true;  // the first look after a load or a cover: the whole list is reported
	std::vector<ULONGLONG> g_recoverTimes;  // the last repairs that worked, this session
	unsigned long long RecoverLimitNow()
	{
		unsigned long long longest = 0;
		for (const auto t : g_recoverTimes) longest = std::max<unsigned long long>(longest, t);
		return trafficpair::RecoverLimit(longest);
	}
	void NotePairHealth(unsigned vehicle, unsigned driver, trafficpair::Seen seen, ULONGLONG now)
	{
		auto& r = g_pairHealth[vehicle];
		const bool seated = seen == trafficpair::Seen::Seated || seen == trafficpair::Seen::Out;
		if (seated && r.notSeatedSince) {
			if (g_recoverTimes.size() >= 32) g_recoverTimes.erase(g_recoverTimes.begin());
			g_recoverTimes.push_back(now - r.notSeatedSince);
		}
		if (seated) r.notSeatedSince = 0; else if (!r.notSeatedSince) r.notSeatedSince = now;
		const auto next = trafficpair::NextHealth(seen, seated ? 0 : now - r.notSeatedSince, RecoverLimitNow());
		if (next == trafficpair::PairHealth::Broken && r.health != next)
			logger::Write("Pair health: vehicle %08X / driver %08X broken: not seated for %.0f s (%s): no far pass, no driver pass; removed once far and out of sight or under a cover", vehicle, driver, (now - r.notSeatedSince) / 1000.0, trafficpair::SeenName(seen));
		else if (r.health == trafficpair::PairHealth::Broken && next == trafficpair::PairHealth::Healthy)
			logger::Write("Pair health: vehicle %08X / driver %08X recovered: passes back", vehicle, driver);
		r.health = next;
	}
	double LoadedEdge() { const unsigned cells = runtimecells::GridSize(); return cells ? std::max(120.0, std::sqrt(double(cells)) * 50.0 - 60.0) : 190.0; }
	double ApiNoticeFar(unsigned base)
	{
		double radius = 0, half = 0;
		const double size = !base ? trafficpair::kDriverSize : ApiHullSize(base, &radius, &half) ? std::min(radius * 2.0, 16.0) : trafficpair::kUnknownVehicleSize;
		return trafficpair::NoticeFar(size, LoadedEdge());
	}
	int ApiInCameraView(double x, double y, double z) { return vehiclekeep::InCameraView(x, y, z) ? 1 : 0; }
	std::set<unsigned> g_playerTook;  // stolen or given: the player's now. Traffic lets go of it (2026-10-30, the user: "shoot at, steal their vehicles, or board them").
	int ApiPlayerTook(unsigned vehicle) { return vehicle && g_playerTook.count(vehicle) ? 1 : 0; }
	int ApiPairHealth(unsigned vehicle) { const auto it = g_pairHealth.find(vehicle); return it == g_pairHealth.end() ? 0 : int(it->second.health); }
	int g_watchLines = 0;
	constexpr int kWatchLineBudget = 400;
	int LinkedActorsInCells(unsigned* out, int max)
	{
		int n = 0;
		__try {
			const uptr player = Ptr(addr::kPlayerCharacterPtr), tes = Ptr(addr::kTESPtr);
			if (!player || !tes) return 0;
			const auto entries = At<uptr*>(tes, off::kTESGridCells);
			const auto cells = std::min(At<std::uint32_t>(tes, off::kTESGridCount), 64u);
			for (std::uint32_t i = 0; entries && i < cells; ++i) {
				const uptr e = entries[i];
				if (!e) continue;
				const auto state = At<std::uint32_t>(e, off::kGridEntryState);
				if ((((state & 0x70000000u) + 0xD0000000u) & 0xEFFFFFFFu) != 0) continue;
				const uptr cell = At<uptr>(e, off::kGridEntryCell);
				if (!cell) continue;
				const auto loaded = At<std::uint8_t>(cell, off::kCellState);
				if (loaded != 9 && loaded != 10) continue;
				uptr* refs = nullptr; std::uint32_t count = 0;
				if (!runtimecells::References(cell, refs, count)) continue;
				for (std::uint32_t k = 0; k < count && k < 8192 && n < max; ++k) {
					const uptr r = refs[k];
					if (!r || r == player || !npcvehicles::LooksLikeActor(r)) continue;
					const auto id = At<std::uint32_t>(r, off::kFormId);
					if ((id >> 24) == 0xFF && npcvehicles::VehicleOf(r)) out[n++] = id;
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return n;
	}
	// Every seated run-time person in the game's form table (2026-10-31 09:42 run: an unloaded driver has no parent cell and is in no cell's list; the persistent cell scan found 0 of them). The seat
	// link is stored on the driver (FUN_14219BB40: extra data 0xA5), so it reads with no model. Once per loading screen; the ids stop well above what a save reaches.
	constexpr int kCoverActorsMax = 4096;
	unsigned g_coverActors[kCoverActorsMax]{};
	int g_coverActorCount = 0;
	bool g_coverScanned = false;
	bool LivePerson(uptr r, std::uint32_t id)
	{
		__try { return At<std::uint32_t>(r, off::kFormId) == id && !(At<std::uint32_t>(r, kFormFlags) & (kDisabledFlag | kDeletedFlag)); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	void ScanFormTable()
	{
		const ULONGLONG started = GetTickCount64();
		int people = 0;
		g_coverActorCount = 0;
		for (std::uint32_t id = 0xFF000800; id < 0xFF100000 && g_coverActorCount < kCoverActorsMax; ++id) {
			const uptr r = runtimeforms::Lookup(id);
			if (!r || !npcvehicles::LooksLikeActor(r) || !LivePerson(r, id)) continue;
			++people;
			if (npcvehicles::VehicleOf(r)) g_coverActors[g_coverActorCount++] = id;
		}
		logger::Write("Traffic orphans: form table has %d run-time people, %d of them seated (%llu ms)", people, g_coverActorCount, GetTickCount64() - started);
	}
	int AddCoverActors(unsigned* out, int n, int max)
	{
		if (ApiPresentation() != 3) { g_coverScanned = false; return n; }
		if (!g_coverScanned) { g_coverScanned = true; ScanFormTable(); }
		for (int i = 0; i < g_coverActorCount && n < max; ++i) {
			const unsigned id = g_coverActors[i];
			FvoTrafficRef d{};
			if (ApiRef(id, &d) && !d.disabled && std::find(out, out + n, id) == out + n) out[n++] = id;
		}
		return n;
	}
	int ApiProvingSpot(unsigned baseId, int slot, double* x, double* y, double* z, double* heading)
	{
		(void)baseId;(void)slot;(void)x;(void)y;(void)z;(void)heading;
		return 0; // retired ABI slot: admission uses its validated authored origin
	}
	int ApiSendPair(unsigned, unsigned, double, double, double, double)
	{
        // ABI compatibility for older traffic addons. The 12:44 hang captured
        // this synchronous MoveReference under SFSE's command-task callback:
        // actor-data reader -> actor-data writer -> physics lock -> worker jobs.
        // A started pilot already has its route claim and can fly there. Never
        // teleport a seated pair here, even under map/loading-screen cover.
        return 0;
	}
	// The user's rule, 2026-10-31: pairs that are not healthy are deleted. By the game's own ObjectReference.Delete, FUN_14206B650: a bare SetDelete, with no queued teardown, was followed by an allocator
	// fault and a stale Havok owner (mapguard.cpp, on dust), and again on 2026-10-31 at 11:03 (Starfield.exe+0x22D3390, not proven). It is called with no script VM, so everything that would make it report
	// an error, or queue a VM functor, is left out here: only a persistent, disabled, unprotected reference. Anything else stays hidden. settings.ini [traffic] deletePairs=0: all stay hidden.
	// Deleted ids can be reused by the game (18:43 crash); the identity record stays, so a reused id reads as gone.
	// Why a reference may not be deleted now; nullptr when it may.
	const char* DeleteRefusal(uptr r,std::uint32_t& flags)
	{
		__try {
			flags = At<std::uint32_t>(r, kFormFlags);
			if (!(flags & kDisabledFlag)) return "not_disabled";
			if (flags & kDeletedFlag) return "already_deleted";
			if (!(flags & kPersistentFlag)) return "not_persistent";
			const auto loaded=At<uptr>(r,off::kRefLoadedData);
			if(loaded&&At<uptr>(loaded,off::kLoadedData3D))return "model_loaded"; // revalidate after queue delay: model retirement must finish first
			if (r == Ptr(addr::kPlayerCharacterPtr)) return "player";
			const uptr base = At<uptr>(r, kBaseForm);
			if (base && At<std::uint8_t>(base, 0x2E) == 'A' && (At<std::uint32_t>(base, 0x20) & 0x40)) return "base_flag";
			return EngineFn<char (*)(uptr, uptr, int)>(0x005A0010)(*reinterpret_cast<uptr*>(game::Base() + 0x05FD9B28), r, 1) ? "engine_refused" : nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return "fault"; }
	}
	bool NativeDelete(uptr r)
	{
		// The persistent branch of ObjectReference.Delete (FUN_14206B650, disassembled 2026-10-31), step for step, without the script
		// return it ends on: nothing here waits on a stack, and FUN_14285CFD0 would take the VM's locks inside the service, find no
		// stack 0 and log a script error. MayDelete has refused what that function refuses before this point.
		__try {
			auto** vt=*reinterpret_cast<void***>(r);
			reinterpret_cast<void (*)(uptr,char)>(vt[0x198/8])(r,1);
			EngineFn<void (*)(uptr,uptr)>(0x018C1AD0)(Ptr(0x05E71728),r);
			if(EngineFn<char (*)(uptr,uptr,int)>(0x005A0010)(Ptr(0x05FD9B28),r,1))return false;
			auto& flags=At<std::uint16_t>(r,0x2C);
			if(flags&0x40){flags=std::uint16_t(flags&0xFFBF);reinterpret_cast<void (*)(uptr,char)>(vt[0xB8/8])(r,1);}
			std::uint64_t handle=0;uptr holder=0;  // extra 0x36: what holds this reference lets go of it
			EngineFn<void (*)(uptr*,void*)>(0x002B65E0)(&holder,EngineFn<void* (*)(uptr,void*)>(0x00514F60)(At<uptr>(r,0xC8),&handle));
			if(holder&&EngineFn<char (*)(uptr)>(0x012FE780)(holder)){
				EngineFn<void (*)(uptr,uptr)>(0x01300600)(holder,r);
				EngineFn<void (*)(uptr,unsigned)>(0x00516130)(At<uptr>(holder,0xC8),At<unsigned>(r,0x24));
			}
			EngineFn<void (*)(uptr,char)>(0x00B3F5D0)(r,1);
			if(holder)EngineFn<void (*)(uptr)>(addr::kRefRelease)(holder);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
	void ForgetTrafficId(unsigned id){
		g_madeDrivers.erase(id);g_madeVehicles.erase(id);g_pairHealth.erase(id);g_watched.erase(id);g_guardOthers.erase(id);g_guardHidden.erase(id);
		if(g_lastMadeVehicle==id)g_lastMadeVehicle=0;
	}
	int ApiDeleteRef(unsigned id)
	{
		static const bool on = GetPrivateProfileIntW(L"traffic", L"deletePairs", 1, fvopaths::Settings().c_str()) != 0;
		const uptr r = TrafficRef(id);
		if(!r){lifecycle::Forget(MutationKey("delete",id));ForgetTrafficId(id);return 1;}
		if(At<unsigned>(r,kFormFlags)&kDeletedFlag){
			const auto loaded=At<uptr>(r,off::kRefLoadedData);
			if(loaded&&At<uptr>(loaded,off::kLoadedData3D))return 0;
			lifecycle::Forget(MutationKey("delete",id));ForgetTrafficId(id);return 1;
		}
		if(!on||(id>>24)!=0xFF)return 0;
		// Only the persistent branch of the game's Delete is done here (NativeDelete). A hidden reference that lost the flag gets it back; the next try deletes it.
		if((At<unsigned>(r,kFormFlags)&kDisabledFlag)&&!(At<unsigned>(r,kFormFlags)&kPersistentFlag)&&!lifecycle::Pending(id,"delete")){
			logger::Write("OBS system=traffic.delete vehicle=%08X event=persist_first flags=%08X",id,At<unsigned>(r,kFormFlags));
			const int actor=npcvehicles::LooksLikeActor(r)?1:0;
			g_unpersisted[id]=actor;QueuePersist(id,actor);
			return 0;
		}
		const auto result=lifecycle::Poll(MutationKey("delete",id),id,[=]()->lifecycle::Work {
			const auto owned=lifecycle::Keep(r,id);if(!owned)return {};
			return [=]()->std::uint64_t {
				const auto ref=reinterpret_cast<uptr>(owned.get());
				std::uint32_t flags=0;
				// The delete goes through FUN_1418C1AD0, as the hide that hung behind the loading screen did.
				const char* why=!lifecycle::InService()?"out_of_service":lifecycle::InCover()?"behind_loading_screen":DeleteRefusal(ref,flags);
				if(why){logger::Write("OBS system=traffic.delete vehicle=%08X event=refused reason=%s flags=%08X",id,why,flags);return 0;}
				const bool deleted=NativeDelete(ref);
				logger::Write("OBS system=traffic.delete vehicle=%08X event=native ok=%d flags=%08X",id,deleted,flags);
				return deleted?1:0;
			};
		});
		if(result&&*result)ForgetTrafficId(id);
		return result?int(*result):0;
	}
	int ApiMutationPending(unsigned id,const char* operation){return lifecycle::Pending(id,operation?operation:"")?1:0;}
	void ApiHoldDriver(unsigned driverId, unsigned vehicleId)
	{
		const uptr d = ActorRef(driverId), v = VehicleRef(vehicleId);
		if (d && v) npcvehicles::HoldAtVehicle(d, v);
	}
	int ApiLinkedPairs(unsigned* drivers, unsigned* vehicles, int max)
	{
		if (!drivers || !vehicles || max <= 0) return 0;
		std::vector<unsigned> found(std::size_t(std::min(max, fvolimits::kVehicles)));
		int count = LinkedActorsInCells(found.data(), int(found.size()));
		count = AddCoverActors(found.data(), count, int(found.size()));
		int n = 0;
		for (int i = 0; i < count && n < max; ++i) {
			const unsigned vehicle = ApiVehicleOf(found[std::size_t(i)]);
			if ((vehicle >> 24) != 0xFF || ApiUsesVehicle(vehicle) || ApiPlayerTook(vehicle) || realvehicles::PlayersOrOwn(vehicle)) continue;
			drivers[n] = found[std::size_t(i)]; vehicles[n] = vehicle; ++n;
		}
		return n;
	}
	int CellStateOf(unsigned id)
	{
		const uptr r = Ref(id);
		__try { const uptr cell = r ? At<uptr>(r, off::kRefParentCell) : 0; return cell ? At<std::uint8_t>(cell, off::kCellState) : -1; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
	}
	// The model guard: every frame, every driver traffic knows and every seat-linked run-time driver it doesn't. See trafficpair::DriverMayShow.
	void GuardDriver(unsigned id, unsigned made, unsigned& hiddenNow, unsigned& told)
	{
		FvoTrafficRef d{};
		if (!ApiRef(id, &d) || !d.isActor || !d.usable || !d.has3D) return;
		const uptr actor = ActorRef(id);
		if (!actor) return;
		const unsigned linked = ApiVehicleOf(id), vehicle = linked ? linked : made;
		FvoTrafficRef v{};
		const bool vehicle3D = ApiRef(vehicle, &v) && v.usable && v.has3D;
		const uptr vehicleRef = vehicle3D ? VehicleRef(vehicle) : 0;
		const double distance = vehicleRef ? npcvehicles::SeatDistance(actor, vehicleRef) : std::nan("");
		const int sit = ApiSitState(id);
		bool high = false;
		if (!linked || sit == 4) {
			double nav = 0, baked = 0;
			const int known = ApiGround(d.x, d.y, d.z, &nav, &baked);
			high = !known || d.z - (known & 2 ? baked : nav) > 4;  // no ground known: not shown as standing on it
		}
		const auto seen = trafficpair::JudgeShown(linked != 0, vehicle3D, distance, sit, high);
		const bool show = trafficpair::DriverMayShow(d.dead != 0, npcvehicles::DriverLeaving(actor), ApiPlayerTook(vehicle) != 0, seen, sit, high, npcvehicles::IsInCombat(actor));
		const auto it = g_guardHidden.find(id);
		const bool wasKnown = it != g_guardHidden.end();
		if (show) {
			// A model that arrived shrunk (npcvehicles' attach hook) has no entry here yet: it gets the scale it came with.
			if (npcvehicles::ModelShrunk(actor)) { float back = wasKnown ? it->second : npcvehicles::AttachScale(id); npcvehicles::ShrinkModel(actor, false, back); }
			if (wasKnown) g_guardHidden.erase(it);
			return;
		}
		float& saved = wasKnown ? it->second : (g_guardHidden[id] = npcvehicles::AttachScale(id));
		npcvehicles::ShrinkModel(actor, true, saved);
		++hiddenNow;
		if (!wasKnown && told < 60) {
			++told;
			logger::Write("Model guard: driver %08X hidden: %s, sit %d, %.1f m from vehicle %08X (%s), made by traffic %d", id, trafficpair::SeenName(seen), sit, distance, vehicle, vehicle3D ? "model loaded" : "no model", made ? 1 : 0);
		}
	}
	void GuardDrivers()
	{
		static ULONGLONG listAt = 0, reportAt = 0;
		static unsigned told = 0;
		if (!trafficdiagnostics::ModelGuardEnabled() || !ApiPresentation()) return;
		const ULONGLONG now = GetTickCount64();
		if (now - listAt >= 500) {
			listAt = now;
			unsigned drivers[256]{}, vehicles[256]{};
			const int n = ApiLinkedPairs(drivers, vehicles, 256);
			// Kept once seen: someone who lost the link while shrunk must still be judged, or they would stay tiny for good.
			for (auto it = g_guardOthers.begin(); it != g_guardOthers.end();) { FvoTrafficRef d{}; if (!ApiRef(it->first, &d) || !d.usable) it = g_guardOthers.erase(it); else ++it; }
			for (int i = 0; i < n; ++i) if (!g_madeDrivers.count(drivers[i])) g_guardOthers[drivers[i]] = vehicles[i];
			std::vector<std::uint32_t> ids;
			for (const auto& m : g_madeDrivers) ids.push_back(m.first);
			for (const auto& m : g_guardOthers) ids.push_back(m.first);
			npcvehicles::SetGuardIds(ids.data(), int(ids.size()));
		}
		unsigned hiddenNow = 0;
		for (const auto& m : g_madeDrivers) GuardDriver(m.first, m.second, hiddenNow, told);
		for (const auto& m : g_guardOthers) GuardDriver(m.first, 0, hiddenNow, told);
		if (hiddenNow && now - reportAt >= 30000) {
			reportAt = now;
			logger::Write("Model guard: %u of %zu driver(s) hidden now", hiddenNow, g_madeDrivers.size() + g_guardOthers.size());
		}
	}
	void WatchDrivers()
	{
		static ULONGLONG last = 0, lastSummary = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 2000) return;
		last = now;
		if (ApiPresentation() != 1) {  // a cover is up: models and seats are still changing. The time to recover starts again after it.
			g_watched.clear(); g_healthLaunch = true;
			for (auto& entry : g_pairHealth) entry.second.notSeatedSince = 0;
			return;
		}
		FvoTrafficPlayer pl{};
		if (!ApiPlayer(&pl)) return;
		unsigned found[fvolimits::kVehicles]{};
		const int linkedInCells = LinkedActorsInCells(found, fvolimits::kVehicles);
		std::set<unsigned> ids(found, found + linkedInCells);
		for (const auto& m : g_madeDrivers) ids.insert(m.first);
		int counts[6]{}, unloaded = 0, reported = 0, hiddenNow = 0;
		for (const unsigned id : ids) {
			FvoTrafficRef d{};
			if (!ApiRef(id, &d) || !d.isActor || d.dead || !d.usable || !d.has3D) { ++unloaded; g_watched.erase(id); continue; }
			const unsigned linked = ApiVehicleOf(id);
			const auto made = g_madeDrivers.find(id);
			const unsigned vehicle = linked ? linked : made != g_madeDrivers.end() ? made->second : 0;
			FvoTrafficRef v{};
			const bool vehicle3D = ApiRef(vehicle, &v) && v.usable && v.has3D;
			const double distance = vehicle3D ? std::hypot(std::hypot(v.x - d.x, v.y - d.y), v.z - d.z) : std::nan("");
			double nav = 0, baked = 0;
			const int known = ApiGround(d.x, d.y, d.z, &nav, &baked);
			const double above = known ? d.z - (known & 2 ? baked : nav) : std::nan("");
			const int sit = ApiSitState(id);
			const auto seen = trafficpair::JudgeShown(linked != 0, vehicle3D, distance, sit, std::isfinite(above) && above > 4);
			++counts[int(seen)];
			if (made != g_madeDrivers.end() && vehicle) NotePairHealth(vehicle, id, seen, now);
			auto& w = g_watched[id];
			if (!w.since || w.seen != seen) { w = Watched{ seen, now, false }; }
			const ULONGLONG lasted = now - w.since, reportAfter = trafficpair::SeenReportMs(seen);
			if (reportAfter && lasted >= reportAfter) ++reported;
			const bool job = vehicle && ApiTripActive(vehicle) != 0;
			// Never where it can be noticed (the FVO Traffic mandate): the pair waits until both halves are far and out of the line of sight.
			const bool hide = !ApiPlayerTook(vehicle) && trafficpair::HideShown(seen, made != g_madeDrivers.end(), job, lasted) 
				&& std::hypot(d.x - pl.x, d.y - pl.y) >= ApiNoticeFar(0) && (!vehicle3D || std::hypot(v.x - pl.x, v.y - pl.y) >= ApiNoticeFar(v.baseId))
				&& (!ApiInCameraView(d.x, d.y, d.z) || ExposureAt(d.x, d.y, d.z) == 2) && (!vehicle3D || !ApiInCameraView(v.x, v.y, v.z) || ExposureAt(v.x, v.y, v.z) == 2);
			if (reportAfter && lasted >= reportAfter && (!w.told || hide) && g_watchLines < kWatchLineBudget) {
				w.told = true; ++g_watchLines;
				logger::Write("Driver watch: driver %08X %s for %.0f s%s: vehicle %08X model %08X at (%.0f, %.0f, %.0f), %.1f m away, %s; job %d, made by traffic %d, sight %d, %.0f m from the player, driver at (%.0f, %.0f, %.0f), %.1f m above the ground, driver cell state %d, sit %d",
					id, trafficpair::SeenName(seen), lasted / 1000.0, hide ? ", HIDDEN NOW" : "", vehicle, v.baseId, v.x, v.y, v.z, distance, vehicle3D ? "model loaded" : "no model",
					job ? 1 : 0, made != g_madeDrivers.end() ? 1 : 0, ExposureAt(d.x, d.y, d.z), std::hypot(d.x - pl.x, d.y - pl.y), d.x, d.y, d.z, above, CellStateOf(id), sit);
			}
			// A traffic driver with no job and no seat, high up, floats or falls to its death (2026-10-31: 5 in one run, no combat; FF10032E held 75 m up): set down on the ground below,
			// whenever neither end is in the camera's view.
			// Not one at its vehicle's side: that vehicle is frozen up there too, and the pair can still be seated (FF100370, 1.4 m from a vehicle 81 m up).
			if (seen == trafficpair::Seen::Air && made != g_madeDrivers.end() && !job && !d.dead && std::isfinite(above) && !(std::isfinite(distance) && distance <= trafficpair::kSnapReach)) {
				auto& rescue = g_airRescue[id];
				if (!rescue.at || now - rescue.at >= 2000) {
					rescue.at = now;
					const double groundZ = d.z - above + 0.5;
					const bool inCamera = ApiInCameraView(d.x, d.y, d.z) || ApiInCameraView(d.x, d.y, groundZ);
					logger::ObserveReference("driver.rescue", id, inCamera ? 1u : 2u, "event=request vehicle_id=%08X above=%.1f pos=%.1f,%.1f,%.1f in_camera=%d", vehicle, above, d.x, d.y, d.z, inCamera);
					if (!inCamera) RescueDriver(id, d.x, d.y, groundZ);
				}
			}
			if (hide) { ApiShow(id, 0); if (vehicle && ApiVehicleOf(0x14) != vehicle) ApiShow(vehicle, 0); ++hiddenNow; g_watched.erase(id); }  // the pair, never the player's own vehicle
		}
		if (g_healthLaunch) {  // the query at launch: every pair the first time play is running
			g_healthLaunch = false;
			int health[4]{};
			for (const auto& m : g_madeDrivers) ++health[ApiPairHealth(m.second)];
			logger::Write("Pair health at launch: %zu pairs: %d healthy, %d recovering, %d broken, %d unproven (no driver model yet)", g_madeDrivers.size(), health[1], health[2], health[3], health[0]);
		}
		if (reported || hiddenNow || now - lastSummary >= 30000) {
			if (now - lastSummary >= 10000) {
				lastSummary = now;
				logger::Write("Driver watch: %zu drivers: %d seated, %d at the seat not sitting, %d ALONE, %d AWAY, %d out on the ground, %d out HIGH UP, %d no model; %d past their report time, %d hidden now",
					ids.size(), counts[int(trafficpair::Seen::Seated)], counts[int(trafficpair::Seen::Seating)], counts[int(trafficpair::Seen::Alone)], counts[int(trafficpair::Seen::Apart)],
					counts[int(trafficpair::Seen::Out)], counts[int(trafficpair::Seen::Air)], unloaded, reported, hiddenNow);
			}
		}
	}
	const FvoTrafficApi kApi = {
		kFvoTrafficApiVersion,
		&ApiPlayer, &ApiTrafficOn, &ApiRef, &ApiFormExists, &ApiVehicles, &ApiGround,
		&ApiCreateVehicle, &ApiCreateSeatedDriver, &ApiMakeTraffic, &ApiShow, &ApiMoveTo,
		&ApiVehicleOf, &ApiSitState, &ApiSnapSeat, &ApiUsesVehicle, &ApiEndJob, &ApiTripRefusal, &ApiStartTrip,
		&ApiCanManagePlacement,
		&context::EverydayVehicles, &context::EverydayPeople, &ApiMapPlaces, &ApiCivilJobs, &ApiMayUse,
		&ApiStartScripted, &ApiStartSupply, &ApiStartPatrol,
        &ApiPresentation,&ApiBirthSite,&ApiExposure,&ApiTripActive,
        &ApiAirSite,&ApiArrivalProgress,&ApiReleaseBirth,
        &vehiclediag::StageBirth,
        &ApiHullSize,
        &ApiTrafficPilotFree,&ApiStartArmed,&ApiPilotHolds,
        &ApiVehicleTypes,&ApiMappedSpot,
        &ApiPairHealth,&ApiNoticeFar,&ApiInCameraView,&ApiPlayerTook,
        &ApiLinkedPairs,&ApiProvingSpot,&ApiSendPair,&ApiDeleteRef,&ApiHoldDriver,&ApiPilotLive,&ApiMutationPending,
	};
}

// Core's own calls, forwarded to the addon (trafficpool.h keeps its interface for main_vehicle and the map editor).
namespace trafficpool
{
    void WorldActionsAllowed(bool allowed){g_worldActions.store(allowed);}
    void ArrivalChecks(int& rayChecked,int& mapOnly,bool reset){
        rayChecked=g_arrivalRayChecked.load();mapOnly=g_arrivalMapOnly.load();
        if(reset){g_arrivalRayChecked=0;g_arrivalMapOnly=0;}
    }
	void Tick() {
		if (const unsigned own = vehicleflight::OwnRoverId()) g_playerTook.insert(own);
		if (g_attached.load() && g_hooks.tick) { g_hooks.tick(); WatchDrivers(); GuardDrivers(); }
		ServiceVisibility();
	}
	bool MadeVehicle(std::uint32_t id) { return g_madeVehicles.count(id) != 0; }
	void Reset() { lifecycle::Reset();g_visibility.clear();g_airRescue.clear();{std::lock_guard lock(g_coverHideLock);g_coverHides.clear();}g_unpersisted.clear();g_birthProof={};g_guardOthers.clear();g_guardHidden.clear();trafficarrival::Reset();g_madeVehicles.clear();g_madeDrivers.clear();g_watched.clear();g_pairHealth.clear();g_recoverTimes.clear();g_playerTook.clear();g_healthLaunch=true;g_lastMadeVehicle=0;g_watchLines=0;if (g_attached.load() && g_hooks.reset) g_hooks.reset(); }
	void RushTick() { if (g_attached.load() && g_hooks.housekeeping) g_hooks.housekeeping();ServiceVisibility(); }
	void Rush(const RushPlace* places, int count)
	{
		if (!g_attached.load() || !g_hooks.rush) { logger::Write("Traffic: Start / Swap asked for, but FVO_Traffic.dll is not installed"); return; }
		static_assert(sizeof(RushPlace) == sizeof(FvoRushPlace), "RushPlace and FvoRushPlace must match");
		g_hooks.rush(reinterpret_cast<const FvoRushPlace*>(places), count);
	}
	int PairHealth(unsigned vehicleId) { return ApiPairHealth(vehicleId); }
	bool WellHiddenFar(unsigned vehicleId, double distance) { FvoTrafficRef v{}; return ApiRef(vehicleId, &v) && distance >= ApiNoticeFar(v.baseId); }
	bool PutBack(unsigned id, double x, double y, double z)
	{
		const uptr ref = Ref(id);
		return ref&&ApiMoveTo(id,x,y,z,At<game::Vec3>(ref,off::kRefRotation).z)!=0;
	}
	int Everyday() { return g_attached.load() && g_hooks.everyday ? g_hooks.everyday() : 0; }
	bool AddonAttached() { return g_attached.load(); }
	void ReloadFiles() { if (g_attached.load() && g_hooks.reload) g_hooks.reload(); }
	RushState RushStatus()
	{
		RushState st;
		if (!g_attached.load() || !g_hooks.rushStatus) return st;
		FvoRushState f{};
		g_hooks.rushStatus(&f);
		st.active = f.active != 0; st.phase = f.phase; st.total = f.total; st.done = f.done; st.moved = f.moved; st.made = f.made;
		st.skipped = f.skipped; st.placed = f.placed; st.seated = f.seated; st.driving = f.driving;
		return st;
	}
}

extern "C" __declspec(dllexport) const FvoTrafficApi* FVO_TrafficApi(int version, const FvoTrafficHooks* hooks)
{
    // ES 0.3 uses the cached-ground v4 prefix, with no callbacks. Preserve that
    // read-only integration without permitting an old generator to bypass v5 admission.
    if(version==4&&!hooks){static const FvoTrafficApi legacy=[] {auto api=kApi;api.version=4;return api;}();return &legacy;}
	if (version != kFvoTrafficApiVersion) {
		logger::Write("Traffic: FVO_Traffic.dll asked for API version %d; this core offers %d (update both together)", version, kFvoTrafficApiVersion);
		return nullptr;
	}
	if (hooks) { g_hooks = *hooks; g_attached.store(true); }
	logger::Write("Traffic: FVO_Traffic.dll attached (API v%d)", version);
	return &kApi;
}
