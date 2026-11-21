// FVO core API v5 for FVO_Traffic.dll: engine operations require presentation and physical birth proofs.
// The traffic pool's decisions moved into the addon (traffic/generator.cpp); this file keeps only the engine work it needs, as
// the pool did it (the create / seat / persist / move steps confirmed in game 2026-10-25..29), and forwards core's own calls
// (main_vehicle's tick, reset and paused frames; the map editor's Start / Swap) to the addon's hooks. Without the addon these
// do nothing.
#include "trafficpool.h"
#include "relocation.h"
#include "handoff.h"
#include "trafficpair.h"
#include "modeladmission.h"
#include "vehiclekeep.h"
#include "camera_visibility.h"
#include "traffic/traffic_api.h"
#include "traffic/birth_api.h"
#include "traffic/factorybirth_api.h"
#include "traffic/factoryfloat.h"
#include "traffic/factorydispatch_api.h"
#include "traffic/roadclearance.h"
#include "referencebirth.h"
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
#include "vehicle_probe.h"
#include "routeedit.h"
#include "secretunpause.h"
#include "playclock.h"
#include "inputgate.h"
#include "trafficarrival.h"
#include "spawnrule.h"
#include "vehiclediag.h"
#include "passenger.h"  // KO-1: a passenger ride is not taking the vehicle
#include "notify.h"
#include "lifecycle.h"
#include "traffic/admission.h"
#include "traffichealthapi.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include <set>
#include <mutex>
#include <algorithm>
#include <array>
#include <cstdio>
#include <optional>
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
    int ApiPresentation(){
        const bool loading=trafficarrival::Covered(),ready=g_worldActions.load(),panel=trafficarrival::panel.load();
        return spawnrule::Presentation(loading,ready,panel,!loading&&ready&&!panel&&routeedit::MapOpen(100)&&!inputgate::GameplayAllowed());
    }
    int ExposureAt(double,double,double);
    int ApiExposure(double x,double y,double z){return ExposureAt(x,y,z);}
    double ApiNoticeFar(unsigned base);
    int ApiInCameraView(double x,double y,double z);
    unsigned long long ProofLimitMs(int screen);
    // May a vehicle be made here in play? Hidden by terrain, or OUT OF THE CAMERA VIEW (and the camera not about to look there) and at least
    // spawnrule::kUnseenMinM from the player. Live creation (2026-11-20, the user: fill the road without frozen X, never a visible pop-in, never an empty
    // stretch). The old "beyond the notice distance AND out of view" never held inside the ~250 m loaded area, so open road was never filled.
    int BirthHidden(unsigned base,double x,double y,double z,double px,double py){
        const int exposure=ExposureAt(x,y,z);
        const bool distant=std::hypot(x-px,y-py)>=spawnrule::kUnseenMinM,inCamera=ApiInCameraView(x,y,z)!=0;
        (void)base;
        const int hidden=(exposure==2||(distant&&!inCamera))&&
            !vehiclekeep::HoldBirthForLook(x,y,z,unsigned(ProofLimitMs(1)));
        logger::Observe("traffic.fill",0,hidden?2u:distant?1u:0u,"event=hidden_check hidden=%d exposure=%d far=%d in_camera=%d",hidden,exposure,distant,inCamera);
        return hidden;
    }
    // How far the player may have moved between a birth request and its service. At a standstill 2 m; in play the player can be driving or flying, so the
    // allowance grows with the time the request waited at up to 60 m/s (a bound, capped at 40 m). The camera look-ahead and the concealment
    // are asked again at service, so a moved player is judged by them, not by a fixed 2 m.
    double PlayerMoveLimit(unsigned long long askedAt){
        const double seconds=double(GetTickCount64()-askedAt)/1000.0;
        return 2.0+std::min(40.0,60.0*seconds);
    }
    struct BirthProof {unsigned base=0,vehicle=0;double x=0,y=0,z=0,yaw=0;ULONGLONG at=0;std::string world;bool air=false;} g_birthProof;
    void NoteBirthRefusal(const FvoTrafficBirthV1& request,const char* stage,const char* reason);  // DW-2: defined with the record below

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
    constexpr runtimeforms::Range kFactoryStock{"FVO_FactoryPlacements.esm",0x800,kFvoFactoryStockCountV1};
    struct FactoryPlayer {
        uptr cell=0,world=0;
        double x=0,y=0,z=0;
    };
    struct FactoryCamera {
        FactoryPlayer player;
        float origin[3]{};
        ULONGLONG at=0;
    };
    struct FactorySite {
        FvoRoadSiteV1 draft{};
        FvoFactorySiteProofV1 proof{};
        FactoryCamera seenFrom{};
    };
#ifndef FVO_RELEASE
    struct FactoryIdentity {
        unsigned base=0;
        uptr ref=0,world=0;
        FvoRoadSiteV1 origin{};
        FactoryCamera concealedFrom{};
        FvoRoadSiteV1 concealedPose{};
        ULONGLONG concealedAt=0;
    };
    std::mutex g_factoryBirthLock;
    FactorySite g_factorySite;
    FactoryCamera g_factoryCamera;
    std::map<unsigned,FactoryIdentity> g_dynamicStock;
    std::uint64_t g_factoryNonce=0;
#endif
    uptr StockCell(uptr ref,uptr* world=nullptr){
        __try {
            const auto cell=ref?At<uptr>(ref,off::kRefParentCell):0;
            if(world)*world=cell?At<uptr>(cell,off::kCellWorldspace):0;
            return cell;
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    }
    bool FactoryStockId(unsigned id){
        if(!id)return false;
#ifndef FVO_RELEASE
        {
            std::lock_guard lock(g_factoryBirthLock);
            if(const auto it=g_dynamicStock.find(id);it!=g_dynamicStock.end()&&it->second.ref==VehicleRef(id))
                return true;
        }
#endif
        const auto file=runtimeforms::LoadedPlugin(kFactoryStock.plugin);
        if(!file)return false;
        for(int i=0;i<kFvoFactoryStockCountV1;++i)
            if(runtimeforms::RuntimeId(file,kFactoryStock.At(i).local)==id)return true;
        return false;
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

	std::uint32_t Create(const char* source, uptr base, uptr cell, uptr ws, double x, double y, double z, double heading, const void* vtbl)
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
		__try { referencebirth::Commit(source, data, handle, d); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
		const uptr ref = RefFromHandle(static_cast<std::uint32_t>(handle));
		if (!ref) return 0;
		std::uint32_t id = 0;
		__try { id = At<std::uint32_t>(ref, off::kFormId); EngineFn<void(*)(uptr,char,char)>(addr::kMarkerPost1)(ref,1,0);EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref); }
		__except (EXCEPTION_EXECUTE_HANDLER) {}
		return id;
	}
    std::uint64_t CreateSeatedActor(const char* source, unsigned base, uptr vehicle, uptr cell, uptr world,
                                    double x, double y, double z, double heading)
    {
        const Birth previous=g_birth;
        g_birth={vehicle,-99};
        const unsigned id=Create(source,runtimeforms::Lookup(base),cell,world,x,y,z,heading,kSeatedCreation);
        const auto seat=g_birth.result;
        g_birth=previous;
        return std::uint64_t(id)|(std::uint64_t(std::uint32_t(seat))<<32);
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
        modeladmission::storedBase=0;
        __try{
            const auto form=runtimeforms::Lookup(base);if(!form)return 0;
            double half[3]{},bottom=0,top=0;
            if(!realvehicles::ReadModelBounds(form,half,bottom,top))return 0;
            modeladmission::storedShape=modeladmission::FromBounds(half,bottom,top);
            modeladmission::storedBase=base;
            if(radius)*radius=std::hypot(half[0],half[1])+1;if(halfHeight)*halfHeight=half[2]+1;return 1;
        }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
    }
#ifndef FVO_RELEASE
    bool ReadFactoryPlayer(FactoryPlayer& out){
        FvoTrafficPlayer current{};
        if(!PlayerCell(out.cell,out.world)||!ApiPlayer(&current))return false;
        out.x=current.x;out.y=current.y;out.z=current.z;
        return std::isfinite(out.x)&&std::isfinite(out.y)&&std::isfinite(out.z);
    }
    bool SameFactoryPlayer(const FactoryPlayer& a,const FactoryPlayer& b){
        return a.cell==b.cell&&a.world==b.world&&
            std::hypot(a.x-b.x,a.y-b.y,a.z-b.z)<=factorybirth::rules::maxPlayerDrift;
    }
    bool SameFactorySite(const FvoRoadSiteV1& a,const FvoRoadSiteV1& b){
        return a.x==b.x&&a.y==b.y&&a.z==b.z&&a.heading==b.heading&&
            a.radius==b.radius&&a.halfHeight==b.halfHeight;
    }
    bool InFactoryCell(const FactoryPlayer& player,const FvoRoadSiteV1& site){
        __try {
            const auto exterior=At<uptr>(player.cell,off::kCellExterior);
            if(!exterior)return false;
            const double x=At<int>(exterior,0),y=At<int>(exterior,4);
            const auto in=[](double p,double radius,double cell){
                return std::floor((p-radius)/off::kCellSize)==cell&&
                       std::floor((p+radius)/off::kCellSize)==cell;
            };
            return in(site.x,site.radius,x)&&in(site.y,site.radius,y);
        }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool RememberFactoryCamera(FactoryCamera& camera){
        float forward[3]{};
        if(!ReadFactoryPlayer(camera.player)||!vehiclekeep::ReadCameraPose(camera.origin,forward)||
           !std::all_of(std::begin(camera.origin),std::end(camera.origin),[](float v){return std::isfinite(v);}))return false;
        camera.at=GetTickCount64();
        std::lock_guard lock(g_factoryBirthLock);
        g_factoryCamera=camera;
        return true;
    }
    bool FactoryCameraForSite(int screen,const FactoryPlayer& player,FactoryCamera& camera){
        if(!vehicle_probe::RaysLive())return false;
        if(screen==1)return RememberFactoryCamera(camera)&&SameFactoryPlayer(camera.player,player);
        if(screen!=2||!trafficarrival::panel.load()||!secretunpause::Active())return false;
        std::lock_guard lock(g_factoryBirthLock);
        if(!roadclearance::Fresh(g_factoryCamera.at,GetTickCount64(),factorybirth::rules::coveredProofMs)||
           !SameFactoryPlayer(g_factoryCamera.player,player))return false;
        camera=g_factoryCamera;
        return true;
    }
    bool SameFactoryView(const FactoryCamera& a,const FactoryCamera& b){
        return SameFactoryPlayer(a.player,b.player)&&
            std::hypot(double(a.origin[0])-b.origin[0],double(a.origin[1])-b.origin[1],double(a.origin[2])-b.origin[2])
                <=factorybirth::rules::maxPlayerDrift;
    }
    bool FactoryViewStillCurrent(const FactoryPlayer& player,const FactoryCamera& proof){
        FactoryCamera current{};
        current.player=player;
        float forward[3]{};
        return vehiclekeep::ReadCameraPose(current.origin,forward)&&SameFactoryView(current,proof);
    }
    FvoFactorySiteStatusV1 FactoryOcclusion(unsigned owner,const FactoryCamera& camera,const FvoRoadSiteV1& site){
        using Status=FvoFactorySiteStatusV1;
        bool pending=false;
        // Conservative whole-envelope samples use the existing camera ray scheduler.
        for(const double dx:{-site.radius,site.radius})for(const double dy:{-site.radius,site.radius})
            for(const double dz:{-site.halfHeight,site.halfHeight}){
                const float end[]{float(site.x+dx),float(site.y+dy),float(site.z+dz)};
                const double distance=std::hypot(double(end[0])-camera.origin[0],double(end[1])-camera.origin[1],double(end[2])-camera.origin[2]);
                bool hit=false;double offset=0;
                const bool answered=environment::CameraRay(owner,camera.origin,end,hit,offset);
                const auto evidence=cameravisibility::RayEvidence(answered,hit,offset,distance,0);
                if(evidence==cameravisibility::Evidence::Visible)return Status::Visible;
                if(evidence!=cameravisibility::Evidence::Occluded)pending=true;
            }
        const float centre[]{float(site.x),float(site.y),float(site.z)};
        bool hit=false;double offset=0;
        const bool answered=environment::CameraRay(owner,camera.origin,centre,hit,offset);
        const auto result=cameravisibility::RayEvidence(answered,hit,offset,
            std::hypot(double(centre[0])-camera.origin[0],double(centre[1])-camera.origin[1],double(centre[2])-camera.origin[2]),0);
        if(result==cameravisibility::Evidence::Visible)return Status::Visible;
        return pending||result!=cameravisibility::Evidence::Occluded?Status::Pending:Status::Ready;
    }
    // How long this site's support ray has been asked about (the environment query is dropped if not re-asked, so the wait
    // is kept here); entries not asked for 3 s are forgotten.
    ULONGLONG FactorySupportWaitMs(double x,double y){
        static std::vector<std::pair<std::int64_t,std::pair<ULONGLONG,ULONGLONG>>> asks;
        const auto now=GetTickCount64();
        const auto key=(std::int64_t(std::floor(x*2))<<32)^std::int64_t(std::uint32_t(std::int32_t(std::floor(y*2))));
        std::lock_guard lock(g_factoryBirthLock);
        std::erase_if(asks,[&](const auto& a){return now-a.second.second>3000;});
        for(auto& a:asks)if(a.first==key){a.second.second=now;return now-a.second.first;}
        if(asks.size()>=32)asks.erase(asks.begin());
        asks.push_back({key,{now,now}});
        return 0;
    }
    FvoFactorySiteStatusV1 QueryFactorySite(unsigned base,const FvoRoadSiteV1& draft,FvoFactorySiteProofV1& out){
        using Status=FvoFactorySiteStatusV1;
        if(!std::isfinite(draft.x)||!std::isfinite(draft.y)||!std::isfinite(draft.z)||!std::isfinite(draft.heading)||
           std::abs(draft.z)>1e6||!vehiclediag::BirthStageAvailable())return Status::Unsafe;
        FactoryPlayer player{};
        const int screen=ApiPresentation();
        if(!screen||!ReadFactoryPlayer(player))return Status::Pending;
        const auto form=runtimeforms::Lookup(base);
        double half[3]{},bottom=0,top=0;
        if(!realvehicles::ReadModelBounds(form,half,bottom,top)||!npcvehicles::BaseHasDriverSeat(form))return Status::Unsafe;
        FvoRoadSiteV1 site{draft.x,draft.y,draft.z,draft.heading,
            std::hypot(half[0],half[1])+factorybirth::rules::hullMargin,half[2]+factorybirth::rules::hullMargin};
        if(!InFactoryCell(player,site))return Status::WrongCell;
        FactoryCamera camera{};
        const bool haveCamera=FactoryCameraForSite(screen,player,camera);
        {
            std::lock_guard lock(g_factoryBirthLock);
            const auto& cached=g_factorySite;
            if(cached.proof.nonce&&cached.proof.base==base&&SameFactorySite(draft,cached.draft)&&
               SameFactoryPlayer(player,cached.seenFrom.player)&&(screen!=1||(haveCamera&&SameFactoryView(camera,cached.seenFrom)))&&
               roadclearance::Fresh(cached.proof.at,GetTickCount64(),
                   screen==1?factorybirth::rules::liveProofMs:factorybirth::rules::coveredProofMs)){
                out=cached.proof;return Status::Ready;
            }
        }
        realvehicles::RefreshRoadInventory(player.cell,player.world,ApiPresentation()>=2);
        if(!haveCamera)return Status::Pending;
        double baked=0;
        const double ceiling=cellbake::Ground(site.x,site.y,baked)&&std::isfinite(baked)?std::min(site.z,baked):site.z;
        // Lab stock floats (hover at a commanded absolute height): it needs no support under it. One dry-support ray below the
        // public surface is evidence only. Measured z where it answers in time; otherwise the float height under the layer gap.
        // (Was: 9 rays per candidate + NoSupport; 92 of 170 tries stayed Pending because those rays starved the query queue.)
        const double probeTop=ceiling-factorybirth::rules::layerGap;
        environment::Ground ground{};
        const bool answered=environment::GroundAt(0,site.x,site.y,probeTop,probeTop-factoryfloat::probeDepth,ground);
        const auto waited=FactorySupportWaitMs(site.x,site.y);
        const auto pick=factoryfloat::Decide({answered,answered&&ground.hit,ground.water,ground.normalOk,ground.z,ground.nz},
            ceiling,bottom,top,waited);
        logger::Observe("traffic.factory_site_gate",base,unsigned(pick.kind),
            "event=height kind=%d why=%s ceiling=%.2f probe_top=%.2f answered=%d hit=%d ground=%.2f waited_ms=%llu z=%.2f",
            int(pick.kind),pick.why,ceiling,probeTop,int(answered),int(answered&&ground.hit),ground.z,
            static_cast<unsigned long long>(waited),pick.z);
        if(pick.kind==factoryfloat::Kind::Wait)return Status::Pending;
        if(pick.kind==factoryfloat::Kind::Blocked)return Status::Unsafe;
        site.z=pick.z;
        const unsigned proofFlags=pick.kind==factoryfloat::Kind::Float?factoryfloat::flagFloatSite:0u;
        const auto sight=FactoryOcclusion(base,camera,site);
        if(sight!=Status::Ready)return sight;
        std::uint64_t epoch=0;
        const auto occupied=realvehicles::CheckRoadSite(player.cell,player.world,site,epoch);
        if(occupied!=FvoRoadClearance::Clear||!epoch)return occupied==FvoRoadClearance::Occupied?Status::Occupied:Status::Pending;
        if(screen!=1&&!roadclearance::Fresh(camera.at,GetTickCount64(),factorybirth::rules::coveredProofMs))
            return Status::Pending;
        std::lock_guard lock(g_factoryBirthLock);
        const auto at=GetTickCount64();
        // A proof produced under X is limited by the last real gameplay camera sample.
        const auto stamped=screen==1?at:camera.at;
        out.base=base;out.flags=proofFlags;out.site=site;out.nonce=++g_factoryNonce;out.at=stamped;
        g_factorySite={draft,out,camera};
        return Status::Ready;
    }
    bool FactoryRefPersistent(uptr ref){
        __try { return ref&&(At<unsigned>(ref,kFormFlags)&kPersistentFlag)!=0; }
        __except(EXCEPTION_EXECUTE_HANDLER){return false;}
    }
    bool FactoryDriverHidden(unsigned id,const FactoryPlayer& player,bool livePresentation){
        const auto ref=VehicleRef(id);
        FvoTrafficRef live{};
        if(!ApiRef(id,&live)||!live.usable)return false;
        std::lock_guard lock(g_factoryBirthLock);
        const auto it=g_dynamicStock.find(id);
        if(it==g_dynamicStock.end()||it->second.ref!=ref)return false;
        const auto& row=it->second;
        return SameFactoryPlayer(player,row.concealedFrom.player)&&
            std::hypot(live.x-row.concealedPose.x,live.y-row.concealedPose.y,live.z-row.concealedPose.z)
                <=factorybirth::rules::maxStockPoseDrift&&
            roadclearance::Fresh(row.concealedAt,GetTickCount64(),
                livePresentation?factorybirth::rules::liveProofMs:factorybirth::rules::coveredProofMs)&&
            (!livePresentation||FactoryViewStillCurrent(player,row.concealedFrom));
    }
    bool DynamicFactoryStock(unsigned id){
        std::lock_guard lock(g_factoryBirthLock);
        const auto found=g_dynamicStock.find(id);
        return found!=g_dynamicStock.end()&&found->second.ref==VehicleRef(id);
    }
    const char* FactorySiteRefusal(const FactorySite& site,const FactoryPlayer& current,bool live){
        if(!SameFactoryPlayer(site.seenFrom.player,current))return "factory_site_cell_or_player_changed";
        if(!roadclearance::Fresh(site.proof.at,GetTickCount64(),
            live?factorybirth::rules::liveProofMs:factorybirth::rules::coveredProofMs))
            return "factory_site_proof_expired";
        if(live&&!FactoryViewStillCurrent(current,site.seenFrom))return "factory_site_camera_changed";
        if(!InFactoryCell(current,site.proof.site))return "factory_site_outside_cell";
        std::uint64_t epoch=0;
        const auto clearance=realvehicles::CheckRoadSite(current.cell,current.world,site.proof.site,epoch);
        if(clearance==FvoRoadClearance::Occupied)return "factory_site_became_occupied";
        return clearance==FvoRoadClearance::Clear&&epoch?nullptr:"factory_site_clearance_unknown";
    }
    void RememberFactoryStock(unsigned id,const FactorySite& site){
        const auto ref=VehicleRef(id);
        if(!ref||(id>>24)!=0xFF)return;
        std::lock_guard lock(g_factoryBirthLock);
        const auto& proof=site.proof;
        g_dynamicStock[id]={proof.base,ref,site.seenFrom.player.world,proof.site,site.seenFrom,proof.site,proof.at};
    }
    FvoFactorySiteStatusV1 ObserveFactoryStock(unsigned id,unsigned base,const FvoRoadSiteV1& saved){
        using Status=FvoFactorySiteStatusV1;
        FactoryPlayer player{};FvoTrafficRef vehicle{};
        const auto ref=VehicleRef(id);
        if((id>>24)!=0xFF||!base||!ApiRef(id,&vehicle)||vehicle.isActor||!vehicle.usable||
           base!=vehicle.baseId||!FactoryRefPersistent(ref)||
           !spawnflow::FactoryPose(vehicle.x-saved.x,vehicle.y-saved.y,vehicle.z-saved.z)||!std::isfinite(saved.heading))return Status::Unsafe;
        if(!ReadFactoryPlayer(player))return Status::Pending;
        if(StockCell(ref)!=player.cell)return Status::WrongCell;
        double half[3]{},bottom=0,top=0;
        if(!realvehicles::ReadModelBounds(runtimeforms::Lookup(base),half,bottom,top))return Status::Unsafe;
        auto origin=saved;
        origin.radius=std::hypot(half[0],half[1])+factorybirth::rules::hullMargin;
        origin.halfHeight=half[2]+factorybirth::rules::hullMargin;
        if(!InFactoryCell(player,origin))return Status::WrongCell;
        FactoryCamera camera{};
        const int screen=ApiPresentation();
        const bool haveCamera=FactoryCameraForSite(screen,player,camera);
        {
            std::lock_guard lock(g_factoryBirthLock);
            const auto [it,inserted]=g_dynamicStock.try_emplace(id,FactoryIdentity{base,ref,player.world,origin});
            auto& row=it->second;
            if(!inserted&&(row.base!=base||row.ref!=ref||row.world!=player.world||!SameFactorySite(row.origin,origin)))return Status::Unsafe;
            const auto limit=screen==1?factorybirth::rules::liveProofMs:factorybirth::rules::coveredProofMs;
            if(SameFactoryPlayer(player,row.concealedFrom.player)&&
               (screen!=1||(haveCamera&&SameFactoryView(camera,row.concealedFrom)))&&
               std::hypot(vehicle.x-row.concealedPose.x,vehicle.y-row.concealedPose.y,vehicle.z-row.concealedPose.z)
                   <=factorybirth::rules::maxStockPoseDrift&&
               roadclearance::Fresh(row.concealedAt,GetTickCount64(),limit))return Status::Ready;
        }
        if(!haveCamera)return Status::Pending;
        auto live=origin;live.x=vehicle.x;live.y=vehicle.y;live.z=vehicle.z;
        const auto sight=FactoryOcclusion(id,camera,live);
        if(sight!=Status::Ready)return sight;
        std::lock_guard lock(g_factoryBirthLock);
        const auto it=g_dynamicStock.find(id);
        if(it==g_dynamicStock.end()||it->second.ref!=ref)return Status::Unsafe;
        it->second.concealedFrom=camera;
        it->second.concealedPose=live;
        it->second.concealedAt=screen==1?GetTickCount64():camera.at;
        return Status::Ready;
    }
#endif
	int ReadTrafficVehicles(FvoTrafficVehicle* out, int max, bool includeHeld)
	{
		static thread_local world::Snapshot snap;
		if (!(includeHeld?world::VehiclesAndHeld(snap):world::Vehicles(snap))) return -1;
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
	int ApiVehicles(FvoTrafficVehicle* out, int max) {return ReadTrafficVehicles(out,max,false);}
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
	std::string BirthKey(std::string key,const FvoTrafficBirthV1& request) {
		if(request.grant)key+=":"+std::to_string(request.grant);
		return key;
	}
	bool BirthWindowOpen(const FvoTrafficBirthV1& request, int screen)
	{
		if(request.source==FvoBirthSource::Legacy)return true;
		switch(request.window){
		case FvoBirthWindow::Loading:return screen==3;
		case FvoBirthWindow::Panel:return screen==2&&trafficarrival::panel.load();
		// The live road fill (2026-11-20): a fleet birth in play, in every build (the player build must fill the road too). The name is historic.
		case FvoBirthWindow::Diagnostic:return screen==1&&request.source==FvoBirthSource::Fleet;
        case FvoBirthWindow::FleetShortfall:
            return screen==1&&request.source==FvoBirthSource::Fleet&&request.slot>=0;
#ifndef FVO_RELEASE
        case FvoBirthWindow::FactoryConcealed:
            return screen==1&&playclock::Running()&&
                ((request.source==FvoBirthSource::FactoryStock&&!request.vehicle)||
                 (request.source==FvoBirthSource::DriverRepair&&DynamicFactoryStock(request.vehicle)));
#endif
		default:return false;
		}
	}
	std::optional<std::uint64_t> ListenBirth(const FvoTrafficBirthV1& request, const std::string& key,
		unsigned id, const std::function<lifecycle::Work()>& prepare, FvoBirthState* state)
	{
		const auto admit=[&]()->lifecycle::Work {
			const auto source=BirthSourceName(request.source);
			if(!BirthWindowOpen(request,ApiPresentation())){
				logger::Write("OBS system=traffic.birth event=refused stage=request reason=window_closed source=%s slot=%d key=%s",source,request.slot,key.c_str());
				NoteBirthRefusal(request,"request","window_closed");
				return {};
			}
			logger::Write("OBS system=traffic.birth event=accepted source=%s slot=%d base=%08X vehicle=%08X key=%s",
				source,request.slot,request.base,request.vehicle,key.c_str());
			return prepare();
		};
		if(!state)return lifecycle::Poll(key,id,admit);
		bool tracked=false;
		const auto result=request.start&&request.window!=FvoBirthWindow::Closed
			?lifecycle::PollTracked(key,id,admit,tracked):lifecycle::Collect(key,tracked);
		*state=result?FvoBirthState::Completed:tracked?FvoBirthState::Pending:FvoBirthState::Missing;
		return result;
	}
	int ApiSitedProof(unsigned base,double x,double y,double z,double heading,const char** why,BirthProof* out,
                      bool requireConcealment=true);
	const char* RoadRefusal(const FvoRoadProofV1& road,const FvoTrafficBirthV1& request,int screen,uptr cell,uptr ws)
	{
		if(!roadclearance::Matches(road,request))return "road_proof_other_site";
		if(!roadclearance::Fresh(road.at,GetTickCount64(),roadclearance::ProofMs(screen==3)))return "road_proof_expired";
		auto epoch=road.epoch;
		const auto result=realvehicles::CheckRoadSite(cell,ws,road.site,epoch);
		return result==FvoRoadClearance::Clear?nullptr:
			result==FvoRoadClearance::Occupied?"road_became_occupied":"road_clearance_unknown";
	}
	// The core's refusal reasons per birth ticket (DW-2): every refusal line below also lands here, so FVO_TrafficBirthRefusalV1 can hand the add-on the reason.
	struct BirthRefusalRecord { FvoBirthRefusalV1 row{}; bool used = false; };
	std::mutex g_refusalLock;
	std::array<BirthRefusalRecord,32> g_refusals{};
	std::size_t g_refusalNext=0;
	void NoteBirthRefusal(const FvoTrafficBirthV1& request,const char* stage,const char* reason)
	{
		std::lock_guard lock(g_refusalLock);
		for(auto& r:g_refusals)if(r.used&&r.row.grant==request.grant&&r.row.slot==request.slot){  // the same ticket polled again: keep the first reason and stage
			if(request.grant)return;
		}
		auto& r=g_refusals[g_refusalNext++%g_refusals.size()];
		r.used=true;r.row={};r.row.size=sizeof(FvoBirthRefusalV1);r.row.base=request.base;r.row.grant=request.grant;r.row.slot=request.slot;r.row.atMs=GetTickCount64();
		std::snprintf(r.row.stage,sizeof r.row.stage,"%s",stage?stage:"");std::snprintf(r.row.reason,sizeof r.row.reason,"%s",reason?reason:"");
	}
	std::mutex g_uncollectedChassisLock;
    std::set<unsigned> g_uncollectedChassis;
    int (*g_parkedRetirement)(unsigned,const char*,bool)=nullptr;
    unsigned RequestVehicle(const FvoTrafficBirthV1& request, FvoBirthState* state,
		std::optional<FvoRoadProofV1> road=std::nullopt,std::optional<FactorySite> factory=std::nullopt)
	{
		const auto [source,window,start,grant,slot,baseId,vehicleId,x,y,z,heading]=request;
			const auto key=BirthKey(MutationKey(source==FvoBirthSource::FactoryStock?"factory_create":"create",baseId,x,y,z,heading),request);
		const auto result=ListenBirth(request,key,baseId,[=]()->lifecycle::Work {
			FvoTrafficPlayer pl{};const int screen=ApiPresentation();uptr requestCell=0,requestWorld=0;
			logger::Write("OBS system=create.vehicle vehicle=00000000 event=request base=%08X pos=%.2f,%.2f,%.2f yaw=%.3f screen=%d key=%s",baseId,x,y,z,heading,screen,key.c_str());
			BirthProof proof;
			if(source==FvoBirthSource::Legacy)proof=g_birthProof;
            else if(source==FvoBirthSource::FactoryStock){
#ifndef FVO_RELEASE
                if(!factory||!ApiPlayer(&pl))return {};
                proof={baseId,0,x,y,z,heading,factory->proof.at,pl.world,true};
#else
                return {};
#endif
            }else{
                const char* sitedWhy=nullptr;
                if(!ApiSitedProof(baseId,x,y,z,heading,&sitedWhy,&proof)){  // 2026-11-21: a car made in play must be out of the player's view
                    NoteBirthRefusal(request,"sited_proof",sitedWhy);return {};
                }
            }
			const unsigned long long proofAge=GetTickCount64()-proof.at;
            const auto limit=factory&&screen!=1?factorybirth::rules::coveredProofMs:ProofLimitMs(screen);
			const char* why=!screen?"no_presentation":!ApiPlayer(&pl)?"no_player":proof.base!=baseId?"proof_other_base":proof.world!=pl.world?"proof_other_world":
				proofAge>limit?"proof_expired":
				(std::fabs(x-proof.x)>0.01||std::fabs(y-proof.y)>0.01||std::fabs(z-proof.z)>0.01||std::fabs(heading-proof.yaw)>0.01)?"proof_other_position":
				(road&&!PlayerCell(requestCell,requestWorld))?"no_player_cell":
				(screen<2&&!factory&&!BirthHidden(baseId,x,y,z,pl.x,pl.y))?"exposed":nullptr;
			if(why){logger::Write("OBS system=create.vehicle vehicle=00000000 event=refused stage=request reason=%s base=%08X proof_base=%08X proof_age_ms=%llu screen=%d",why,baseId,proof.base,proofAge,screen);NoteBirthRefusal(request,"request",why);return {};}
			if(source==FvoBirthSource::Legacy)g_birthProof.base=0;
			const unsigned long long askedAt=GetTickCount64();
			return [=]()->std::uint64_t {
				FvoTrafficPlayer current{};uptr cell=0,ws=0;
				const unsigned long long age=GetTickCount64()-proof.at;
                const auto liveLimit=factory&&ApiPresentation()!=1?factorybirth::rules::coveredProofMs:ProofLimitMs(ApiPresentation());
				const char* late=!lifecycle::InService()?"out_of_service":!BirthWindowOpen(request,ApiPresentation())?"window_closed":!ApiPresentation()?"no_presentation":!ApiPlayer(&current)?"no_player":proof.world!=current.world?"world_changed":
					(ApiPresentation()<2&&(screen>=2||std::hypot(current.x-pl.x,current.y-pl.y)>PlayerMoveLimit(askedAt)))?"player_moved_or_cover_gone":
					age>liveLimit?"proof_expired":!PlayerCell(cell,ws)?"no_player_cell":
					(!factory&&ApiPresentation()==1&&
                     vehiclekeep::HoldBirthForLook(x,y,z,unsigned(ProofLimitMs(1))))?"camera_lookahead":
					!road?nullptr:cell!=requestCell||ws!=requestWorld?"road_cell_changed":RoadRefusal(*road,request,ApiPresentation(),cell,ws);
                #ifndef FVO_RELEASE
                if(!late&&factory){
                    const FactoryPlayer player{cell,ws,current.x,current.y,current.z};
                    late=FactorySiteRefusal(*factory,player,ApiPresentation()==1);
                }
                #endif
				if(late){logger::Write("OBS system=create.vehicle vehicle=00000000 event=refused stage=service reason=%s base=%08X proof_age_ms=%llu screen=%d key=%s",late,baseId,age,ApiPresentation(),key.c_str());NoteBirthRefusal(request,"service",late);return 0;}
				const unsigned id=Create(factory?"traffic_factory_stock":"traffic_vehicle",runtimeforms::Lookup(baseId),cell,ws,x,y,z,heading,proof.air?kStagedCreation:nullptr);
                if(id){std::lock_guard lock(g_uncollectedChassisLock);g_uncollectedChassis.insert(id);}
                #ifndef FVO_RELEASE
                if(id&&factory)RememberFactoryStock(id,*factory);
                #endif
				if(!id){logger::Write("OBS system=create.vehicle vehicle=00000000 event=refused stage=engine reason=create_returned_nothing base=%08X",baseId);NoteBirthRefusal(request,"engine","create_returned_nothing");}
				return id;
			};
		},state);
		if(!result)return 0;
		const auto id=unsigned(*result);RememberIdentity(id);
		logger::Write("OBS system=create.vehicle vehicle=%08X event=completion base=%08X ok=%d pos=%.2f,%.2f,%.2f source=%s slot=%d key=%s",id,baseId,id!=0,x,y,z,BirthSourceName(source),slot,key.c_str());
		if(id){g_birthProof.vehicle=id;g_birthProof.at=GetTickCount64();}
		return id;
	}
	struct TrafficAuthority { unsigned chassis; std::uint64_t epoch; };
    TrafficAuthority CaptureTrafficAuthority(unsigned chassis);
    bool TrafficAuthorityCurrent(TrafficAuthority authority);
    void RememberFactoryDriver(unsigned driver,unsigned vehicle);
    unsigned RequestDriver(const FvoTrafficBirthV1& request, int* seatResult, FvoBirthState* state)
	{
		const auto [source,window,start,grant,slot,baseId,vehicleId,x,y,z,heading]=request;
		if (seatResult) *seatResult = -99;
		const auto key=BirthKey(MutationKey("driver",vehicleId),request);
        const auto authority=CaptureTrafficAuthority(vehicleId);
		const auto result=ListenBirth(request,key,vehicleId,[=]()->lifecycle::Work {
			logger::Write("OBS system=create.driver vehicle=%08X event=request base=%08X pos=%.2f,%.2f,%.2f",vehicleId,baseId,x,y,z);
			const auto owned=lifecycle::Keep(VehicleRef(vehicleId),vehicleId);FvoTrafficPlayer pl{};
			const char* why=!owned?"no_vehicle":!ApiPresentation()?"no_presentation":!ApiPlayer(&pl)?"no_player":nullptr;
			if(!why){
				const bool birth=g_birthProof.vehicle==vehicleId&&GetTickCount64()-g_birthProof.at<=ProofLimitMs(ApiPresentation());
				if(!birth&&window!=FvoBirthWindow::FactoryConcealed&&
                   ApiPresentation()<2&&ApiExposure(x,y,z)!=2)why="exposed";
			}
			if(why){logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=request reason=%s base=%08X",vehicleId,why,baseId);NoteBirthRefusal(request,"driver_request",why);return {};}
			const int screen=ApiPresentation();
			const unsigned long long askedAt=GetTickCount64();
			return [=]()->std::uint64_t {
				uptr cell=0,ws=0;FvoTrafficPlayer current{};
				const char* late=!lifecycle::InService()?"out_of_service":!BirthWindowOpen(request,ApiPresentation())?"window_closed":!ApiPresentation()?"no_presentation":!ApiPlayer(&current)?"no_player":std::strcmp(pl.world,current.world)?"world_changed":
					(ApiPresentation()<2&&(screen>=2||std::hypot(pl.x-current.x,pl.y-current.y)>PlayerMoveLimit(askedAt)))?"player_moved_or_cover_gone":!PlayerCell(cell,ws)?"no_player_cell":nullptr;
				if(late){logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=%s base=%08X key=%s",vehicleId,late,baseId,key.c_str());NoteBirthRefusal(request,"driver_service",late);return 0;}
				const uptr vehicle=reinterpret_cast<uptr>(owned.get());
                if(FactoryStockId(vehicleId)){
                    bool hidden=false;
#ifndef FVO_RELEASE
                    if(DynamicFactoryStock(vehicleId))
                        hidden=FactoryDriverHidden(vehicleId,{cell,ws,current.x,current.y,current.z},ApiPresentation()==1);
                    else
#endif
                        hidden=ApiExposure(x,y,z)==2;
                    if(StockCell(vehicle)!=cell||!hidden){
                        logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=factory_origin_cell_or_visibility_changed",vehicleId);
                        return 0;
                    }
                }
				const unsigned vehicleFlags=At<unsigned>(vehicle,kFormFlags);
				if((vehicleFlags&(kDisabledFlag|kDeletedFlag))||!trafficadmission::ValidOrigin(x,y)){
					logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=%s base=%08X flags=%08X",vehicleId,(vehicleFlags&(kDisabledFlag|kDeletedFlag))?"vehicle_hidden_or_deleted":"invalid_origin",baseId,vehicleFlags);
					return 0;
				}
				if(!TrafficAuthorityCurrent(authority)){
                    logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=traffic_authority_revoked",vehicleId);
                    return 0;
                }
                const auto live=At<game::Vec3>(vehicle,off::kRefPosition);
                if(!std::isfinite(live.x)||!std::isfinite(live.y)||!std::isfinite(live.z)||
                    std::hypot(live.x-x,live.y-y)>2.0||std::abs(live.z-z)>8.0){
                    logger::Write("OBS system=create.driver vehicle=%08X event=refused stage=service reason=chassis_left_requested_seat_position",vehicleId);
                    return 0;
                }
                return CreateSeatedActor("traffic_driver",baseId,vehicle,cell,ws,x,y,z,heading);
			};
		},state);
		if(!result)return 0;
		const unsigned id=unsigned(*result);if(seatResult)*seatResult=int(*result>>32);RememberIdentity(id);
        if(id&&FactoryStockId(vehicleId))RememberFactoryDriver(id,vehicleId);
		logger::Write("OBS system=create.driver vehicle=%08X event=completion driver=%08X seat_result=%d ok=%d source=%s slot=%d key=%s",vehicleId,id,int(*result>>32),id!=0,BirthSourceName(source),slot,key.c_str());return id;
	}
	unsigned RequestBirth(const FvoTrafficBirthV1& request,int* seatResult=nullptr,FvoBirthState* state=nullptr)
	{
		return request.vehicle?RequestDriver(request,seatResult,state):RequestVehicle(request,state);
	}
	unsigned ApiCreateVehicle(unsigned base,double x,double y,double z,double heading)
	{
		return RequestBirth({FvoBirthSource::Legacy,FvoBirthWindow::Closed,1,0,-1,base,0,x,y,z,heading});
	}
	unsigned ApiCreateSeatedDriver(unsigned base,unsigned vehicle,double x,double y,double z,double heading,int* seatResult)
	{
		if(!vehicle){if(seatResult)*seatResult=-99;return 0;}
		return RequestBirth({FvoBirthSource::Legacy,FvoBirthWindow::Closed,1,0,-1,base,vehicle,x,y,z,heading},seatResult);
	}
	std::map<unsigned, unsigned> g_guardOthers;  // seat-linked run-time drivers no registry knows (driver -> vehicle), refreshed by the guard
	std::map<unsigned, float> g_guardHidden;     // drivers the guard has shrunk, with the scale to give back
	std::map<unsigned, unsigned> g_madeDrivers;  // driver -> vehicle, for every pair the addon made or reused since the load
    void RememberFactoryDriver(unsigned driver,unsigned vehicle){g_madeDrivers[driver]=vehicle;}
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
        if(actor){
            const auto existing=g_madeDrivers.find(id);
            if(existing==g_madeDrivers.end()||!FactoryStockId(existing->second))RememberDriver(id);
        }else {g_lastMadeVehicle=id;g_madeVehicles.insert(id);std::lock_guard lock(g_uncollectedChassisLock);g_uncollectedChassis.erase(id);}
		g_unpersisted[id]=actor;QueuePersist(id,actor);
		return (At<unsigned>(r,kFormFlags)&kPersistentFlag)?1:0;
	}
	std::map<unsigned,int> g_visibility; // desired state survives a full service queue
	std::map<unsigned,TrafficAuthority> g_retireAuthority;
	std::map<unsigned,bool> g_retireCamera; // retire-only queued visibility checks; value identifies the driver half
	// Behind the loading screen kDisableReference only lists the reference (FUN_140C10660); the list is applied by
	// FUN_140C109C0, called only from the full step's job FUN_1417CA1E0, so the hide lands as the screen closes. Applying
	// it behind the screen (vt+0x7E8) hung the game in both save loads of 2026-10-31 19:28 and 19:37: main spun in
	// FUN_142404230 under FUN_1418C1AD0 while the background jobs were still loading. g_coverHides times each landing.
	std::mutex g_coverHideLock;
	std::map<unsigned,ULONGLONG> g_coverHides;
	void QueueVisibility(unsigned id,int on)
	{
		const auto retiring=g_retireAuthority.find(id);
        const bool retirePair=retiring!=g_retireAuthority.end();
        const auto authority=retirePair?retiring->second:CaptureTrafficAuthority(id);
        if(!on&&!TrafficAuthorityCurrent(authority))return;
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
					if(!TrafficAuthorityCurrent(authority)){
                        logger::Write("OBS system=traffic.visibility vehicle=%08X event=refused reason=traffic_authority_revoked",id);
                        return 0;
                    }
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
	void ApiShow(unsigned id,int on){if(id){if(!on){
            const auto retiring=g_retireAuthority.find(id);
            if(!TrafficAuthorityCurrent(retiring==g_retireAuthority.end()?CaptureTrafficAuthority(id):retiring->second))return;
            if(retiring==g_retireAuthority.end())vehiclediag::DropBirthHold(id);
        }else {g_retireCamera.erase(id);g_retireAuthority.erase(id);}g_visibility[id]=on?1:0;}}
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
			if(!ref||(At<unsigned>(ref,kFormFlags)&kDeletedFlag)){g_retireCamera.erase(id);it=g_visibility.erase(it);continue;}
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
            const auto retiring=g_retireAuthority.find(id);
            if(!it->second&&retiring!=g_retireAuthority.end()&&!TrafficAuthorityCurrent(retiring->second)){
                g_retireCamera.erase(id);it=g_visibility.erase(it);continue;
            }
			if(!pending&&enabled==bool(it->second)){
                if(!enabled&&retiring!=g_retireAuthority.end())vehiclediag::DropBirthHold(id);
                g_retireCamera.erase(id);it=g_visibility.erase(it);continue;
            }
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
					if(hasModel&&screen==1){
						const auto retire=g_retireCamera.find(id);
						if(retire!=g_retireCamera.end()){
							FvoTrafficRef pose{};
							const auto evidence=ApiRef(id,&pose)?vehiclekeep::TrafficCameraEvidence(id,pose.baseId,pose.x,pose.y,pose.z,retire->second):
								cameravisibility::Evidence::Unknown;
							if(const char* why=cameravisibility::HoldReason(evidence)){
								logger::Observe("traffic.lookahead",id,unsigned(evidence),"event=hide_queue_held reason=%s",why);
								++it;continue;
							}
						}
					}
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
    int QueueTrafficMove(unsigned id,double x,double y,double z,double heading,std::uint64_t birthHold)
    {
        // Poll BEFORE fresh preconditions: after a move the original position,
        // visibility and cell may have changed. Its acknowledgement still must
        // be consumed rather than mistaken for another new command.
        const auto key=MutationKey("move",id,x,y,z,heading)+(birthHold?":birth:"+std::to_string(birthHold):"");
        const auto result=lifecycle::Poll(key,id,[=]()->lifecycle::Work {
            const int screen=ApiPresentation();FvoTrafficPlayer pl{};
            if(!screen||!ApiPlayer(&pl)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))return {};
            const auto owned=lifecycle::Keep(TrafficRef(id),id);if(!owned)return {};
            const auto driver=g_madeDrivers.find(id);
            const auto authority=CaptureTrafficAuthority(driver==g_madeDrivers.end()?id:driver->second);
            const auto poseGeneration=npcvehicles::PoseGeneration(id);
            if(!poseGeneration)return {};
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
                if(!TrafficAuthorityCurrent(authority)){
                    logger::ObserveReference("traffic.move",id,4,"event=refused reason=traffic_authority_revoked");
                    return 0;
                }
                if(poseGeneration!=npcvehicles::PoseGeneration(id)){
                    logger::ObserveReference("traffic.move",id,3,"event=refused reason=relocation_revoked_pose");
                    return 0;
                }
                if(birthHold&&birthHold!=vehiclediag::BirthCorrectionGeneration(id)){
                    logger::ObserveReference("traffic.move",id,1,"event=refused reason=stale_birth_hold generation=%llu",
                        static_cast<unsigned long long>(birthHold));
                    return 0;
                }
                const auto flags=At<unsigned>(ref,kFormFlags);
                if(flags&kDeletedFlag)return 0;
                if(!(flags&kDisabledFlag)&&npcvehicles::LooksLikeActor(ref)&&npcvehicles::VehicleOf(ref)){
                    logger::ObserveReference("traffic.move",id,2,"event=refused reason=actor_linked_to_vehicle");
                    return 0;
                }
                const auto start=At<game::Vec3>(ref,off::kRefPosition);
                if(std::hypot(std::hypot(start.x-source.x,start.y-source.y),start.z-source.z)>1)return 0;
                float pos[]={float(x),float(y),float(z)},rot[]={0.0f,0.0f,float(heading)};
                std::uint64_t handle=*reinterpret_cast<unsigned*>(game::Base()+addr::kInvalidHandle);
                EngineFn<void(*)(uptr,std::uint64_t*,uptr,uptr,float*,float*)>(addr::kMoveReference)(ref,&handle,cell,world,pos,rot);
                return 1;
            };
        });
        return result?int(*result):0;
    }
    int ApiMoveTo(unsigned id,double x,double y,double z,double heading){
        return QueueTrafficMove(id,x,y,z,heading,0);
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
	int ApiPilotLive(unsigned vehicleId) { const int live = vehicleflight::PilotLive(vehicleId); return live == 0 && (playclock::Now() & (playclock::kMenu | playclock::kPhoto)) ? 1 : live; }  // W1: menus and photo mode are "not meant to step" (cover keeps its old answer: arrival judges it)
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
        environment::g_raysAnswered=screen<2;
        const auto site=environment::AirSite(base,x,y,z,heading);
        // Behind the loading screen and under the paused map (Start/Swap's panel) FVO's physics rays are not answered. There the
        // authored lane plus the nav map's solids are the check, and the chassis stays frozen until its autopilot holds it.
        // In play: wait for the rays.
        if(site.verdict==environment::Verdict::Pending&&screen<2)return 0;
        if(site.verdict==environment::Verdict::Unsafe){
            // The real reason per refused spot (2026-11-01: road-level refusals were only reported as one lumped message).
            static unsigned told=0;
            if(told<64){++told;logger::Write("OBS system=traffic.site event=refused base=%08X pos=%.1f,%.1f,%.1f ground=%.1f screen=%d reason=%s",base,x,y,z,site.ground,screen,site.reason?site.reason:"");}
            return -1;
        }
        ++(site.verdict==environment::Verdict::Safe?g_arrivalRayChecked:g_arrivalMapOnly);
        g_birthProof={base,0,x,y,z,heading,GetTickCount64(),pl.world,true};return 1;
    }
    // Spawn flow step 5 -> step 6 (change card C1, 2026-11-07): Siting's pose is the birth proof. The spawn flow sites a car from the
    // field box, model bounds, other vehicles and Health's spot verdict only (no site rays, user 2026-11-06); this records that pose so
    // ApiCreateVehicle reads Siting's output instead of judging the site again. Kept from the old proof: presentation, player, a valid
    // origin, staged creation available, and in play only where the player cannot see. Old callers of birthSite/airSite are unchanged.
    int ApiSitedProof(unsigned base,double x,double y,double z,double heading,const char** why,BirthProof* out,
                      bool requireConcealment){
        FvoTrafficPlayer pl{};const int screen=ApiPresentation();
        const char* no=!vehiclediag::BirthStageAvailable()?"staged_birth_unavailable":!screen?"no_presentation":!ApiPlayer(&pl)?"no_player":
            (!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))?"invalid_pose":!trafficadmission::ValidOrigin(x,y)?"invalid_origin":
            (requireConcealment&&screen<2&&!BirthHidden(base,x,y,z,pl.x,pl.y))?"exposed":nullptr;
        if(why)*why=no?no:"sited_by_spawn_flow";
        if(no){logger::Write("OBS system=create.vehicle vehicle=00000000 event=sited_proof ok=0 base=%08X pos=%.2f,%.2f,%.2f screen=%d reason=%s",base,x,y,z,screen,no);return 0;}
        *out={base,0,x,y,z,heading,GetTickCount64(),pl.world,true};
        logger::Write("OBS system=create.vehicle vehicle=00000000 event=sited_proof ok=1 base=%08X pos=%.2f,%.2f,%.2f screen=%d reason=sited_by_spawn_flow",base,x,y,z,screen);
        return 1;
    }
    void ApiArrivalProgress(int expected,int ready){trafficarrival::Progress(expected,ready);}
    // KO-2a: the birth-hold service carries out Handoff's request. For a vehicle in the player-took record the player's control counts as the
    // controller (a fact it reads); it never releases unless Handoff asks.
    int ApiPlayerTook(unsigned vehicle);
    int ApiReleaseBirth(unsigned id){return (ApiPlayerTook(id)?vehiclediag::ReleaseBirthToPlayer(id):vehiclediag::ReleaseBirth(id))?1:0;}

	// The driver watch (2026-10-30, the user: "I cant be providing NPC names for you each time"). Every check before this one looked at
	// the pairs FVO believed were in trouble, so each time the people on the cliffs were on a path nothing watched (the 10:31 run: 54 of
	// 93 pairs had no line after their birth). This one starts from the game's side: every actor in an attached cell with a seat link,
	// plus every driver traffic made. It reports the observation; the population Arbiter decides retirement.
	using RetirementReport=void (*)(unsigned,unsigned);
	RetirementReport g_retirementReport=nullptr;
	struct Watched { trafficpair::Seen seen = trafficpair::Seen::Seated; ULONGLONG since = 0; bool told = false, requested = false; };
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
	std::mutex g_tookLock;  // KO-1: written by the seat rules' thread and the traffic tick, read by the addon tick
	std::uint64_t g_trafficEpoch=1;
    TrafficAuthority CaptureTrafficAuthority(unsigned chassis){
        std::lock_guard lk(g_tookLock);return {chassis,g_trafficEpoch};
    }
    bool TrafficAuthorityCurrent(TrafficAuthority authority){
        std::lock_guard lk(g_tookLock);
        return authority.epoch==g_trafficEpoch&&!g_playerTook.count(authority.chassis);
    }
    int ApiPlayerTook(unsigned vehicle) { std::lock_guard lk(g_tookLock); return vehicle && g_playerTook.count(vehicle) ? 1 : 0; }
	// KO-1 (2026-11-08, kick-out port): the player took this vehicle (pulled out, handed over, hijacked, or in its driver's seat). Traffic lets go of it
	// (the addon's existing player-took branch) and the birth hold releases it to the player (KO-2).
	void NoteTaken(unsigned vehicle, const char* why)
	{
		if (!vehicle) return;
		bool added = false;
		{ std::lock_guard lk(g_tookLock); added = g_playerTook.insert(vehicle).second; if(added)++handoff::wheelGeneration; }
		if (added) logger::Write("OBS system=traffic.taken vehicle=%08X event=player_took reason=%s", vehicle, why ? why : "");
	}
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
        {std::lock_guard lock(g_uncollectedChassisLock);g_uncollectedChassis.erase(id);}
        g_retireAuthority.erase(id);
		g_madeDrivers.erase(id);g_madeVehicles.erase(id);g_pairHealth.erase(id);g_watched.erase(id);g_guardOthers.erase(id);g_guardHidden.erase(id);
		if(g_lastMadeVehicle==id)g_lastMadeVehicle=0;
	}
	int ApiDeleteRef(unsigned id)
    {
        const auto retiring=g_retireAuthority.find(id);
        const auto authority=retiring==g_retireAuthority.end()?CaptureTrafficAuthority(id):retiring->second;
        if(!TrafficAuthorityCurrent(authority))return 0;
		constexpr bool on = true;  // deleting retired pairs: always (2026-11-05 settings strip; was [traffic] deletePairs)
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
				if(!TrafficAuthorityCurrent(authority)){
                    logger::Write("OBS system=traffic.delete vehicle=%08X event=refused reason=traffic_authority_revoked",id);
                    return 0;
                }
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
		int counts[6]{}, unloaded = 0, reported = 0, retireRequests = 0;
		for (const unsigned id : ids) {
			FvoTrafficRef d{};
			if (!ApiRef(id, &d) || !d.isActor || d.dead || !d.usable || !d.has3D) { ++unloaded; g_watched.erase(id); continue; }
			const unsigned linked = ApiVehicleOf(id);
			const auto made = g_madeDrivers.find(id);
			const unsigned vehicle = linked ? linked : made != g_madeDrivers.end() ? made->second : 0;
            if(FactoryStockId(vehicle)||(made!=g_madeDrivers.end()&&FactoryStockId(made->second))){
                g_watched.erase(id);
                continue; // stock seat recovery is owned by its exact Census pair
            }
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
			const bool requestRetirement=trafficpair::HideShown(seen,made!=g_madeDrivers.end(),job,lasted);
			if (!requestRetirement) w.requested=false;
			if (reportAfter && lasted >= reportAfter && !w.told && g_watchLines < kWatchLineBudget) {
				w.told = true; ++g_watchLines;
				logger::Write("Driver watch: driver %08X %s for %.0f s%s: vehicle %08X model %08X at (%.0f, %.0f, %.0f), %.1f m away, %s; job %d, made by traffic %d, sight %d, %.0f m from the player, driver at (%.0f, %.0f, %.0f), %.1f m above the ground, driver cell state %d, sit %d",
					id, trafficpair::SeenName(seen), lasted / 1000.0, requestRetirement ? ", retirement requested" : "", vehicle, v.baseId, v.x, v.y, v.z, distance, vehicle3D ? "model loaded" : "no model",
					job ? 1 : 0, made != g_madeDrivers.end() ? 1 : 0, ExposureAt(d.x, d.y, d.z), std::hypot(d.x - pl.x, d.y - pl.y), d.x, d.y, d.z, above, CellStateOf(id), sit);
			}
			if (requestRetirement && !w.requested && g_retirementReport && (!linked||linked==made->second)) {
				w.requested=true;++retireRequests;g_retirementReport(made->second,id);
			}
		}
		if (g_healthLaunch) {  // the query at launch: every pair the first time play is running
			g_healthLaunch = false;
			int health[4]{};
			for (const auto& m : g_madeDrivers) ++health[ApiPairHealth(m.second)];
			logger::Write("Pair health at launch: %zu pairs: %d healthy, %d recovering, %d broken, %d unproven (no driver model yet)", g_madeDrivers.size(), health[1], health[2], health[3], health[0]);
		}
		if (reported || retireRequests || now - lastSummary >= 30000) {
			if (now - lastSummary >= 10000) {
				lastSummary = now;
				logger::Write("Driver watch: %zu drivers: %d seated, %d at the seat not sitting, %d ALONE, %d AWAY, %d out on the ground, %d out HIGH UP, %d no model; %d past their report time, %d retirement reports",
					ids.size(), counts[int(trafficpair::Seen::Seated)], counts[int(trafficpair::Seen::Seating)], counts[int(trafficpair::Seen::Alone)], counts[int(trafficpair::Seen::Apart)],
					counts[int(trafficpair::Seen::Out)], counts[int(trafficpair::Seen::Air)], unloaded, reported, retireRequests);
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
		if (const unsigned own = vehicleflight::OwnRoverId()) { std::lock_guard lk(g_tookLock); g_playerTook.insert(own); }
		// KO-1: the player in the driver's seat of a vehicle traffic made (not riding as a passenger) has taken it.
		if (const unsigned in = ApiVehicleOf(0x14); in && (g_madeVehicles.count(in)||FactoryStockId(in)) && passenger::RidingVehicle() != in) NoteTaken(in, "player_in_the_driver_seat");
		if (g_attached.load() && g_hooks.tick) {
#ifndef FVO_RELEASE
            if(ApiPresentation()==1){FactoryCamera camera;RememberFactoryCamera(camera);}
#endif
            vehiclekeep::UpdateTrafficLook();g_hooks.tick();WatchDrivers();
        }
		ServiceVisibility();
	}
	bool MadeVehicle(std::uint32_t id) { return g_madeVehicles.count(id) != 0; }
#ifndef FVO_RELEASE
    std::optional<std::uint64_t> FastSeatBirth(unsigned vehicle, std::uint64_t trial,
        const trafficpair::SeatPose& offset, std::uint64_t requestedAt)
    {
        return lifecycle::Poll("fast-seat:"+std::to_string(trial),vehicle,[=]()->lifecycle::Work {
            const auto held=lifecycle::Keep(VehicleRef(vehicle),vehicle);
            if(!held||!TrafficAuthorityCurrent(CaptureTrafficAuthority(vehicle))||MadeVehicle(vehicle)||FactoryStockId(vehicle))return {};
            const auto authority=CaptureTrafficAuthority(vehicle);
            return [=]()->std::uint64_t {
                uptr cell=0,worldspace=0;
                const auto ref=reinterpret_cast<uptr>(held.get());
                if(!lifecycle::InService()||ApiPresentation()!=1||!PlayerCell(cell,worldspace)||
                   StockCell(ref)!=cell||!TrafficAuthorityCurrent(authority)||ApiVehicleOf(0x14)==vehicle||
                   (At<unsigned>(ref,kFormFlags)&(kDeletedFlag|kDisabledFlag)))return 0;
                static thread_local world::Snapshot snap;
                if(!world::Vehicles(snap))return 0;
                const auto row=std::find_if(snap.v,snap.v+snap.count,[=](const auto& item){return item.formId==vehicle;});
                if(row==snap.v+snap.count||row->driverId||row->who==world::VehicleInfo::Who::Player||
                   row->who==world::VehicleInfo::Who::Npc)return 0;
                const auto position=At<game::Vec3>(ref,off::kRefPosition);
                const auto heading=At<game::Vec3>(ref,off::kRefRotation).z;
                float target[3]{};
                trafficpair::SeatPoint(position.x,position.y,position.z,heading,offset,target);
                if(!std::isfinite(target[0])||!std::isfinite(target[1])||!std::isfinite(target[2])||!std::isfinite(heading))return 0;
                const auto before=GetTickCount64();
                const auto result=CreateSeatedActor("fast_seat_test",kFvoTrafficDriverBase,ref,cell,worldspace,
                    target[0],target[1],target[2],heading+offset.yaw);
                logger::Write("OBS system=seat.fast event=created trial=%llu vehicle=%08X driver=%08X callback=%d queued_ms=%llu create_ms=%llu offset=%.2f,%.2f,%.2f",
                    trial,vehicle,unsigned(result),int(result>>32),before-requestedAt,GetTickCount64()-before,offset.lx,offset.ly,offset.lz);
                return result;
            };
        });
    }
#endif
    bool MayHideParked(unsigned id,const char* reason,bool protectedPlacement){
        if(FactoryStockId(id))return false;
        if(!id)return false;
        bool protectedIdentity=protectedPlacement||ApiPlayerTook(id)||MadeVehicle(id);
        {std::lock_guard lock(g_uncollectedChassisLock);protectedIdentity|=g_uncollectedChassis.count(id)!=0;}
        return g_parkedRetirement?g_parkedRetirement(id,reason,protectedIdentity)!=0:!protectedIdentity;
    }
	void Reset() {
        lifecycle::Reset();
#ifndef FVO_RELEASE
        {std::lock_guard lock(g_factoryBirthLock);g_factorySite={};g_factoryCamera={};g_dynamicStock.clear();}
#endif
        {std::lock_guard lock(g_uncollectedChassisLock);g_uncollectedChassis.clear();}g_coverScanned=false;g_coverActorCount=0;realvehicles::ResetRoadInventory();g_visibility.clear();g_retireCamera.clear();g_retireAuthority.clear();{std::lock_guard lock(g_coverHideLock);g_coverHides.clear();}g_unpersisted.clear();g_birthProof={};g_guardOthers.clear();g_guardHidden.clear();trafficarrival::Reset();g_madeVehicles.clear();g_madeDrivers.clear();g_watched.clear();g_pairHealth.clear();g_recoverTimes.clear();{std::lock_guard lk(g_tookLock);++g_trafficEpoch;++handoff::wheelGeneration;}g_healthLaunch=true;g_lastMadeVehicle=0;g_watchLines=0;if (g_attached.load() && g_hooks.reset) g_hooks.reset();
    }
	void RushTick() { if (g_attached.load() && g_hooks.housekeeping) g_hooks.housekeeping();ServiceVisibility(); }
	void Rush(const RushPlace* places, int count)
	{
		if (!g_attached.load() || !g_hooks.rush) { logger::Write("Traffic: Start / Swap asked for, but FVO_Traffic.dll is not installed"); return; }
		static_assert(sizeof(RushPlace) == sizeof(FvoRushPlace), "RushPlace and FvoRushPlace must match");
		g_hooks.rush(reinterpret_cast<const FvoRushPlace*>(places), count);
	}
	int PairHealth(unsigned vehicleId) { return ApiPairHealth(vehicleId); }
	bool WellHiddenFar(unsigned vehicleId, double distance) { FvoTrafficRef v{}; return ApiRef(vehicleId, &v) && distance >= ApiNoticeFar(v.baseId); }
	bool PutBack(unsigned id, double x, double y, double z, std::uint64_t generation)
	{
		const uptr ref = Ref(id);
		return generation&&ref&&QueueTrafficMove(id,x,y,z,At<game::Vec3>(ref,off::kRefRotation).z,generation)!=0;
	}
	int Everyday() { return g_attached.load() && g_hooks.everyday ? g_hooks.everyday() : 0; }
	bool AddonAttached() { return g_attached.load(); }
	bool PlayerOwnsHandoff(unsigned vehicle) { return ApiPlayerTook(vehicle)!=0; }
    std::uint64_t CaptureWheelHandoff(unsigned vehicle) {
        std::lock_guard lock(g_tookLock);
        return g_playerTook.count(vehicle)?0:handoff::wheelGeneration.load(std::memory_order_relaxed);
    }
    std::vector<unsigned> SavePlayerTakeovers() {
        std::lock_guard lock(g_tookLock);
        return {g_playerTook.begin(), g_playerTook.end()};
    }
    void RestorePlayerTakeovers(const std::vector<unsigned>& vehicles) {
        std::set<unsigned> owned(vehicles.begin(), vehicles.end());
        std::lock_guard lock(g_tookLock);
        if (g_playerTook != owned) {
            g_playerTook.swap(owned);
            ++handoff::wheelGeneration;
        }
    }
    void NotePlayerTook(unsigned vehicle, const char* why) { NoteTaken(vehicle, why); }  // KO-1
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


// Additive exports (no FvoTrafficApi table change).
extern "C" __declspec(dllexport) void FVO_TrafficParkedRetirementV1(int (*arbiter)(unsigned,const char*,bool)){
    g_parkedRetirement=arbiter;
}
extern "C" __declspec(dllexport) int FVO_TrafficPanelCoveredV1(){return trafficarrival::panel.load()?1:0;}
extern "C" __declspec(dllexport) void FVO_TrafficRetirementListenerV1(RetirementReport report){g_retirementReport=report;}
extern "C" __declspec(dllexport) int FVO_TrafficHandoffReleaseV1(unsigned vehicle,bool player){
    if(!vehicle||player!=(ApiPlayerTook(vehicle)!=0))return 0;
    const bool released=player?vehiclediag::ReleaseBirthToPlayer(vehicle):vehiclediag::ReleaseBirth(vehicle);
#ifndef FVO_RELEASE
    if(released&&!player)npcvehicles::FactoryLeaseReleased(vehicle);
#endif
    return released;
}
#ifndef FVO_RELEASE
extern "C" __declspec(dllexport) int FVO_TrafficFactoryLeaseV1(
    FvoTrafficFactoryLeaseActionV1 action, unsigned vehicle, unsigned driver, std::uint64_t* lease,
    const FvoTripClaim* claim)
{
    using Action = FvoTrafficFactoryLeaseActionV1;
    if (!lease || !vehicle || !driver) return 0;
    if (action == Action::Cancel) {
        if (!npcvehicles::CancelFactoryLease(vehicle, driver, *lease)) return 0;
        *lease = 0;
        return 1;
    }
    if (!trafficpool::AddonAttached() || ApiPlayerTook(vehicle)) return 0;
    if (action == Action::Status) return npcvehicles::CheckFactoryLease(vehicle, driver, *lease);
    if (action == Action::StartStock && !*lease && !claim) {
        if(!FactoryStockId(vehicle))return 0;
        *lease=npcvehicles::StartFactoryStockLease(ActorRef(driver),VehicleRef(vehicle));
        return *lease!=0;
    }
    if (action == Action::Start && !*lease && claim && claim->step && claim->user) {
        *lease = npcvehicles::StartFactoryLease(ActorRef(driver), VehicleRef(vehicle), *claim);
        return *lease != 0;
    }
    if (action == Action::Depart && npcvehicles::DepartFactoryLease(ActorRef(driver), VehicleRef(vehicle), *lease)) {
        *lease = 0;
        return 1;
    }
    return 0;
}
#endif
extern "C" __declspec(dllexport) void FVO_TrafficHandoffFactsV1(unsigned vehicle,unsigned driver,handoff::Facts* out){
    if(!out)return;
    *out={};
    if(!vehicle||!driver||ApiPlayerTook(vehicle))return;
    out->package=npcvehicles::TrafficPackageReady(vehicle,driver);
    if(!out->package)return;
    const auto physics=vehicleflight::HandoffPhysics(vehicle);
    out->bound=physics.bound;out->sampled=physics.bound;
    out->recentStep=physics.live;out->step=physics.serial;
}
extern "C" __declspec(dllexport) void FVO_TrafficRetirementShowV2(unsigned id,unsigned chassis,bool driver,bool camera){
    if(!id)return;
    const auto authority=CaptureTrafficAuthority(chassis?chassis:id);
    if(!TrafficAuthorityCurrent(authority))return;
    g_retireAuthority[id]=authority;
    if(camera)g_retireCamera[id]=driver;else g_retireCamera.erase(id);
    ApiShow(id,0);
}
#ifndef FVO_RELEASE
extern "C" __declspec(dllexport) int FVO_TrafficFactoryStockV1(FvoFactoryStockV1* out,int max){
    if(!out||max<=0)return 0;
    const auto file=runtimeforms::LoadedPlugin(kFactoryStock.plugin);
    if(!file)return 0;
    uptr playerCell=0,world=0;
    const bool havePlayerCell=PlayerCell(playerCell,world);
    const int count=std::min(max,kFvoFactoryStockCountV1);
    for(int i=0;i<count;++i){
        auto& row=out[i];
        row={};
        row.local=kFactoryStock.At(i).local;
        const auto id=runtimeforms::RuntimeId(file,row.local);
        FvoTrafficRef found{};
        if(id&&ApiRef(id,&found)&&!found.isActor){
            row.vehicle=found.id;row.base=found.baseId;
            row.usable=found.usable;row.modelLoaded=found.has3D;
            const auto ref=VehicleRef(id);
            uptr sourceWorld=0;
            const auto cell=StockCell(ref,&sourceWorld);
            row.inPlayerCell=havePlayerCell&&cell==playerCell;
            row.inPlayerWorld=havePlayerCell&&cell&&sourceWorld==world;
            row.x=found.x;row.y=found.y;row.z=found.z;
        }
    }
    return count;
}
#endif
// DW-2: would the core's own concealment test refuse a birth at this pose right now (the same BirthHidden the sited proof runs: blocked line of sight, or outside the camera's
// 3-D cone and not about to look there, and at least kUnseenMinM from the player)? 1 = hidden (a birth is allowed), 0 = exposed (it would be refused as 'exposed'), -1 = cannot
// say (no player, covered: the covered windows have their own rules). The add-on asks at the moment of a grant so it never grants what the core refuses. Read only; additive.
extern "C" __declspec(dllexport) int FVO_TrafficBirthHiddenV1(unsigned base,double x,double y,double z){
    FvoTrafficPlayer pl{};
    if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!ApiPlayer(&pl)||ApiPresentation()>=2)return -1;
    return BirthHidden(base,x,y,z,pl.x,pl.y)?1:0;
}
// DW-2: the core's reason for the last refusal of one birth ticket (grant + slot of the FvoTrafficBirthV1). 1 and *out filled, or 0 (none recorded: not refused, or an older ticket
// pushed out of the 32-row ring). Read only; additive (an older add-on never calls it, an older core lacks it and the add-on says so).
extern "C" __declspec(dllexport) int FVO_TrafficBirthRefusalV1(std::uint64_t grant,int slot,FvoBirthRefusalV1* out){
    if(!out||out->size<sizeof(FvoBirthRefusalV1)||!grant)return 0;
    std::lock_guard lock(g_refusalLock);
    const BirthRefusalRecord* best=nullptr;
    for(const auto& r:g_refusals)if(r.used&&r.row.grant==grant&&r.row.slot==slot&&(!best||r.row.atMs>best->row.atMs))best=&r;
    if(!best)return 0;
    *out=best->row;out->size=sizeof(FvoBirthRefusalV1);
    return 1;
}
// A model's REAL half extents (realvehicles::ReadModelBounds). 1 and the three halves filled, 0 = the base form is not readable. Read only; additive.
extern "C" __declspec(dllexport) int FVO_TrafficModelBoundsV1(unsigned base,double* halfX,double* halfY,double* halfZ){
    if(!base||!halfX||!halfY||!halfZ)return 0;
    __try{
        const auto form=runtimeforms::Lookup(base);if(!form)return 0;
        double half[3]{},bottom=0,top=0;
        if(!realvehicles::ReadModelBounds(form,half,bottom,top))return 0;
        if(!std::isfinite(half[0])||!std::isfinite(half[1])||!std::isfinite(half[2]))return 0;
        *halfX=half[0];*halfY=half[1];*halfZ=half[2];
        return 1;
    }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
}
extern "C" __declspec(dllexport) int FVO_TrafficRoadVehiclesV1(FvoTrafficVehicle* out,int max){
    uptr cell=0,ws=0;
    if(!out||max<=0||!PlayerCell(cell,ws)){
        logger::Observe("traffic.road_inventory",0,11,"event=road_list_unavailable reason=no_player_cell");
        return -1;
    }
    realvehicles::RefreshRoadInventory(cell,ws,ApiPresentation()>=2);
    const int count=ReadTrafficVehicles(out,max,true);
    if(count<0){
        logger::Observe("traffic.road_inventory",0,12,"event=road_list_unavailable reason=traffic_vehicle_read_failed");
        return count;
    }
    const int full=realvehicles::AppendRoadVehicles(out,count,max);
    if(full<0){
        // 2026-11-21 (live log 00:20-00:25: world_vehicle_list_incomplete 47 times, no live birth for 5 minutes): one stale or busy parked-vehicle source made the whole
        // list fail and stopped the fill for good. The traffic list is still true; parked vehicles are judged per pose by FVO_TrafficRoadCheckV1, which needs a
        // consistent inventory epoch of its own and answers Unknown (the birth waits) when it has none. So the fill goes on with the traffic list and says so.
        logger::Observe("traffic.road_inventory",0,13,"event=road_list_partial reason=parked_sources_not_current traffic_vehicles=%d",count);
        return count;
    }
    return full;
}
extern "C" __declspec(dllexport) FvoRoadClearance FVO_TrafficRoadCheckV1(const FvoRoadSiteV1* site,FvoRoadProofV1* proof){
    uptr cell=0,ws=0;
    if(!site||!proof||!PlayerCell(cell,ws))return FvoRoadClearance::Unknown;
    *proof={*site,0,GetTickCount64()};
    return realvehicles::CheckRoadSite(cell,ws,*site,proof->epoch);
}
#ifndef FVO_RELEASE
extern "C" __declspec(dllexport) FvoStockTransferResultV1 FVO_TrafficStockTransferV1(
    FvoStockTransferActionV1 action,const FvoStockTransferV1* request,std::uint64_t* serial,const FvoTripClaim* claim)
{
    using Action=FvoStockTransferActionV1;
    using Result=FvoStockTransferResultV1;
    using Sight=FvoFactorySiteStatusV1;
    if(serial)*serial=0;
    if(!request||request->size!=sizeof(*request)||request->version!=1||!request->vehicle||!request->driver||!request->lease)
        return Result::Refused;
    const auto& r=*request;
    const auto answer=[&](Result result,unsigned code,const char* why){
        logger::Observe("factory.transfer",r.vehicle,code,"action=%d driver=%08X lease=%llu serial=%llu status=%d reason=%s",
            int(action),r.driver,static_cast<unsigned long long>(r.lease),static_cast<unsigned long long>(r.serial),int(result),why);
        return result;
    };
    if(!trafficpool::AddonAttached()||ApiPlayerTook(r.vehicle))return answer(Result::Refused,1,"traffic_authority_revoked");
    FactoryPlayer player{};
    const int screen=ApiPresentation();
    if((screen==1&&!playclock::Running())||(screen!=1&&screen!=2)||!ReadFactoryPlayer(player))
        return answer(Result::Pending,2,"world_not_running");
    const auto vehicle=VehicleRef(r.vehicle),actor=ActorRef(r.driver);
    FactoryIdentity stock{};
    {
        std::lock_guard lock(g_factoryBirthLock);
        const auto found=g_dynamicStock.find(r.vehicle);
        if(found==g_dynamicStock.end()||found->second.ref!=vehicle||found->second.world!=player.world)
            return answer(Result::Refused,3,"stock_identity_mismatch");
        stock=found->second;
    }
    // The pair and the target may be in any loaded cell of the player's world (the lab follows the player across cells and its pairs are
    // delivered up to the core's reach, relocation::kMaxMoveM). The relocation itself refuses a destination whose cell is not loaded.
    uptr pairWorld=0,actorWorld=0;
    if(!actor||!vehicle||!StockCell(vehicle,&pairWorld)||!StockCell(actor,&actorWorld)||pairWorld!=player.world||actorWorld!=player.world)
        return answer(Result::Refused,4,"pair_outside_current_world");
    if(!std::isfinite(r.target.x)||!std::isfinite(r.target.y)||!std::isfinite(r.target.z)||
       !std::isfinite(r.target.heading)||!std::isfinite(r.target.radius)||!std::isfinite(r.target.halfHeight)||
       !(r.target.radius>0)||!(r.target.halfHeight>0))return answer(Result::Refused,5,"invalid_destination");
    auto target=r.target;
    target.radius=std::max(target.radius,stock.origin.radius);
    target.halfHeight=std::max(target.halfHeight,stock.origin.halfHeight);
    if(action==Action::Promote){
        if(!claim||!claim->step||!claim->user||!r.serial)return answer(Result::Refused,7,"road_claim_or_receipt_missing");
        if(!npcvehicles::PromoteFactoryStockTransfer(actor,vehicle,r.lease,r.serial,
            target.x,target.y,target.z,target.heading,*claim))return answer(Result::Pending,8,"await_exact_seated_arrival");
        {
            std::lock_guard lock(g_factoryBirthLock);
            g_dynamicStock.erase(r.vehicle);
        }
        ApiMakeTraffic(r.vehicle,0);
        ApiMakeTraffic(r.driver,1);
        RememberFactoryDriver(r.driver,r.vehicle);
        return answer(Result::Promoted,9,"public_route_claim_attached");
    }
    if(action!=Action::Check&&(action!=Action::Start||!serial))return Result::Refused;
    if(npcvehicles::FactoryStockTransferBusy())return answer(Result::Pending,10,"relocation_busy");
    if(!npcvehicles::CheckFactoryLease(r.vehicle,r.driver,r.lease))return answer(Result::Refused,11,"private_lease_lost");
    if(!npcvehicles::FactoryStockTransferReady(actor,vehicle,r.lease))
        return answer(Result::Pending,12,"await_live_seated_stock_proof");
    FvoTrafficRef at{};
    if(!ApiRef(r.vehicle,&at)||!at.usable||!at.has3D||at.baseId!=stock.base)
        return answer(Result::Refused,13,"stock_reference_changed");
    const double yaw=At<game::Vec3>(vehicle,off::kRefRotation).z;
    {   // The core's own reach: the whole 3-D move up to relocation::kMaxMoveM (the add-on's Reachable() is narrower). The doer cannot rotate the pair.
        const double distance=std::sqrt((target.x-at.x)*(target.x-at.x)+(target.y-at.y)*(target.y-at.y)+(target.z-at.z)*(target.z-at.z));
        const char* why="";
        if(!relocation::MoveAcceptable(distance,why)){
            logger::Write("OBS system=factory.transfer vehicle=%08X driver=%08X event=refused reason=%s distance=%.1f limit=%.0f",r.vehicle,r.driver,why,distance,relocation::kMaxMoveM);
            return answer(Result::Refused,14,"move_above_core_limit");
        }
        if(std::abs(std::remainder(target.heading-yaw,6.283185307179586))>factorydispatch::maxHeadingDelta)
            return answer(Result::Refused,14,"heading_outside_translation_only_envelope");
    }
    // Origin concealment: inside the player's cell the saved proof applies; outside it that proof is cell-bound (WrongCell), so the live pose is tested
    // against the same camera the destination uses.
    FvoFactorySiteStatusV1 origin;
    if(StockCell(vehicle)==player.cell)origin=ObserveFactoryStock(r.vehicle,stock.base,stock.origin);
    else{
        FactoryCamera originCamera{};
        if(!FactoryCameraForSite(screen,player,originCamera))origin=Sight::Pending;
        else{auto live=stock.origin;live.x=at.x;live.y=at.y;live.z=at.z;origin=FactoryOcclusion(r.vehicle,originCamera,live);}
    }
    if(origin==Sight::Pending||origin==Sight::Visible)return answer(Result::Pending,15,"await_origin_concealment");
    if(origin!=Sight::Ready)return answer(Result::Refused,16,"stock_origin_changed");
    FactoryCamera camera{};
    if(!FactoryCameraForSite(screen,player,camera))return answer(Result::Pending,17,"await_destination_camera");
    const auto sight=FactoryOcclusion(r.vehicle,camera,target);
    if(sight==Sight::Visible)return answer(Result::Refused,18,"destination_visible");
    if(sight!=Sight::Ready)return answer(Result::Pending,19,"await_destination_concealment");
    realvehicles::RefreshRoadInventory(player.cell,player.world,ApiPresentation()>=2);
    std::uint64_t roadEpoch=0;
    const auto clearance=realvehicles::CheckRoadSite(player.cell,player.world,target,roadEpoch);
    if(clearance==FvoRoadClearance::Occupied)return answer(Result::Refused,20,"destination_occupied");
    if(clearance!=FvoRoadClearance::Clear||!roadEpoch)return answer(Result::Pending,21,"await_destination_clearance");
    if(action==Action::Check)return answer(Result::Ready,22,"concealed_same_cell_destination_proved");
    FactoryPlayer current{};
    if(ApiPresentation()!=screen||!ReadFactoryPlayer(current)||!SameFactoryPlayer(player,current)||
       (screen==1&&(!playclock::Running()||!FactoryViewStillCurrent(current,camera)))||
       !roadclearance::Fresh(camera.at,GetTickCount64(),screen==1?factorybirth::rules::liveProofMs:factorybirth::rules::coveredProofMs))
        return answer(Result::Pending,25,"destination_camera_or_cell_changed_before_arm");
    *serial=npcvehicles::StartFactoryStockTransfer(actor,vehicle,r.lease,target.x,target.y,target.z,target.heading);
    return *serial?answer(Result::Armed,23,"physics_relocation_armed"):answer(Result::Refused,24,"physics_relocation_arm_refused");
}
#endif
extern "C" __declspec(dllexport) unsigned FVO_TrafficBirthRequestV1(const FvoTrafficBirthV1* request,FvoBirthState* state){
    if(!state)return 0;
    *state=FvoBirthState::Completed;
    if(!request||!request->base||!request->grant||(request->source!=FvoBirthSource::Fleet&&
       (request->source!=FvoBirthSource::DriverRepair||!request->vehicle)))return 0;
    // Chassis creation for a live physical shortfall must go through V2 with a road receipt.
    if(request->window==FvoBirthWindow::FleetShortfall&&request->start&&!request->vehicle)return 0;
    return RequestBirth(*request,nullptr,state);
}
extern "C" __declspec(dllexport) unsigned FVO_TrafficBirthRequestV2(const FvoTrafficBirthV1* request,const FvoRoadProofV1* proof,FvoBirthState* state){
    if(!state)return 0;
    *state=FvoBirthState::Completed;
    if(!request||!request->base||!request->grant||request->vehicle||request->source!=FvoBirthSource::Fleet||
       (request->start&&(!proof||!roadclearance::Matches(*proof,*request))))return 0;
    return RequestVehicle(*request,state,proof?std::optional{*proof}:std::nullopt);
}
#ifndef FVO_RELEASE
extern "C" __declspec(dllexport) FvoFactorySiteStatusV1 FVO_TrafficFactorySiteV1(
    unsigned base,const FvoRoadSiteV1* draft,FvoFactorySiteProofV1* out)
{
    if(!base||!draft||!out||out->size!=sizeof(*out)||out->version!=1)return FvoFactorySiteStatusV1::Unsafe;
    *out={};
    const auto result=QueryFactorySite(base,*draft,*out);
    logger::Observe("traffic.factory_origin",base,unsigned(result),
        "event=site status=%d draft=%.2f,%.2f,%.2f solved_z=%.2f nonce=%llu",
        int(result),draft->x,draft->y,draft->z,out->site.z,out->nonce);
    return result;
}
extern "C" __declspec(dllexport) unsigned FVO_TrafficFactoryBirthV1(
    const FvoTrafficBirthV1* request,const FvoFactorySiteProofV1* proof,FvoBirthState* state)
{
    if(!state)return 0;
    *state=FvoBirthState::Completed;
    if(!request||!request->base||!request->grant||request->vehicle||request->source!=FvoBirthSource::FactoryStock)return 0;
    if(!request->start)return RequestVehicle(*request,state);
    if(!proof||proof->size!=sizeof(*proof)||proof->version!=1||!proof->nonce||proof->base!=request->base||
       proof->site.x!=request->x||proof->site.y!=request->y||proof->site.z!=request->z||proof->site.heading!=request->heading||
       (request->window!=FvoBirthWindow::Panel&&request->window!=FvoBirthWindow::Loading&&
        request->window!=FvoBirthWindow::FactoryConcealed)||
       !BirthWindowOpen(*request,ApiPresentation()))return 0;
    FactorySite sealed{};
    {
        std::lock_guard lock(g_factoryBirthLock);
        const auto& actual=g_factorySite.proof;
        if(actual.nonce!=proof->nonce||actual.base!=proof->base||actual.at!=proof->at||!SameFactorySite(actual.site,proof->site))return 0;
        sealed=g_factorySite;
        g_factorySite.proof.nonce=0;
    }
    return RequestVehicle(*request,state,std::nullopt,sealed);
}
extern "C" __declspec(dllexport) int FVO_TrafficFactoryStockBindV1(unsigned vehicle,unsigned base,const FvoRoadSiteV1* origin){
    return origin&&ObserveFactoryStock(vehicle,base,*origin)==FvoFactorySiteStatusV1::Ready;
}
extern "C" __declspec(dllexport) FvoFactorySiteStatusV1 FVO_TrafficFactoryStockObserveV1(
    unsigned vehicle,unsigned base,const FvoRoadSiteV1* origin)
{
    return origin?ObserveFactoryStock(vehicle,base,*origin):FvoFactorySiteStatusV1::Unsafe;
}
#endif
extern "C" __declspec(dllexport) int FVO_StoredTrafficModelV1(unsigned base,FvoObservedModelV1* out){
    if(!out||out->size!=sizeof(*out)||out->version!=1||base!=modeladmission::storedBase||
       !modeladmission::Valid(modeladmission::storedShape))return 0;
    *out=modeladmission::storedShape;return 1;
}
// W1: the one play clock for the addon (playclock::Bits; 0 = play time runs). Optional export: an older addon never calls it.
extern "C" __declspec(dllexport) unsigned FVO_PlayClockV1(){return playclock::Now();}
