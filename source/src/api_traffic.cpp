// FVO core API v5 for FVO_Traffic.dll: engine operations require presentation and physical birth proofs.
// The traffic pool's decisions moved into the addon (traffic/generator.cpp); this file keeps only the engine work it needs, as
// the pool did it (the create / seat / persist / move steps confirmed in game 2026-10-25..29), and forwards core's own calls
// (main_vehicle's tick, reset and paused frames; the map editor's Start / Swap) to the addon's hooks. Without the addon these
// do nothing.
#include "trafficpool.h"
#include "trafficpair.h"
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
#include "vehicleworld.h"
#include "environment.h"
#include "vehicleflight.h"
#include "routeedit.h"
#include "inputgate.h"
#include "trafficarrival.h"
#include "vehiclediag.h"
#include "notify.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include <set>
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
    struct BirthProof {unsigned base=0,vehicle=0;double x=0,y=0,z=0,yaw=0;ULONGLONG at=0;std::string world;bool air=false;} g_birthProof;

	uptr Ref(std::uint32_t id)
	{
		if (!id) return 0;
		__try { const uptr r = runtimeforms::LookupChecked(id); return r && At<std::uint32_t>(r, off::kFormId) == id ? r : 0; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
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
		__try { id = At<std::uint32_t>(ref, off::kFormId); EngineFn<void (*)(uptr)>(addr::kRefRelease)(ref); }
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
        FvoTrafficPlayer pl{};if(!ApiPlayer(&pl)||std::hypot(x-pl.x,y-pl.y)>300)return 0; // physics must be loaded
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
		const uptr r = Ref(id);
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
	unsigned ApiCreateVehicle(unsigned baseId, double x, double y, double z, double heading)
	{
		// Revalidate at the mutation boundary: a queued map action may execute after the map closed.
        FvoTrafficPlayer pl{};
        if(!ApiPresentation()||!ApiPlayer(&pl)||g_birthProof.base!=baseId||g_birthProof.world!=pl.world||
           GetTickCount64()-g_birthProof.at>250||std::fabs(x-g_birthProof.x)>0.01||std::fabs(y-g_birthProof.y)>0.01||
           std::fabs(z-g_birthProof.z)>0.01||std::fabs(heading-g_birthProof.yaw)>0.01||
           (ApiPresentation()<2&&ApiExposure(x,y,z)!=2))return 0;
		uptr cell = 0, ws = 0;
		const uptr base = runtimeforms::Lookup(baseId);
		if (!base || !PlayerCell(cell, ws)) return 0;
        g_birthProof.vehicle=Create(base,cell,ws,x,y,z,heading,g_birthProof.air?kStagedCreation:nullptr);g_birthProof.base=0; // single-use proof
        return g_birthProof.vehicle;
	}
	unsigned ApiCreateSeatedDriver(unsigned baseId, unsigned vehicleId, double x, double y, double z, double heading, int* seatResult)
	{
		uptr cell = 0, ws = 0;
		const uptr base = runtimeforms::Lookup(baseId), v = Ref(vehicleId);
		if (seatResult) *seatResult = -99;
		if (!base || !v || !PlayerCell(cell, ws)) return 0;
        const bool birth=g_birthProof.vehicle==vehicleId&&GetTickCount64()-g_birthProof.at<=250;
        if(!ApiPresentation()||(!birth&&ApiPresentation()<2&&ApiExposure(x,y,z)!=2))return 0;
		g_birth = { v, -99 };
		const std::uint32_t id = Create(base, cell, ws, x, y, z, heading, kSeatedCreation);
		if (seatResult) *seatResult = g_birth.result;
		g_birth = {};
		return id;
	}
	std::map<unsigned, unsigned> g_madeDrivers;  // driver -> vehicle, for every pair the addon made or reused since the load
	unsigned g_lastMadeVehicle = 0;
	void RememberDriver(unsigned id) { g_madeDrivers[id] = g_lastMadeVehicle; }
	int ApiMakeTraffic(unsigned id, int actor)
	{
		const uptr r = Ref(id);
		if (!r) return 0;
        if(actor&&!npcvehicles::LooksLikeActor(r))return 0;
		__try {
			if (!(At<std::uint32_t>(r, kFormFlags) & kPersistentFlag))
				EngineFn<void (*)(uptr, char, char)>(addr::kMarkerPost1)(r, 1, 0);
			if (actor) At<std::uint32_t>(r, 0x378) &= ~kEssentialBit;
			// The pair is made vehicle first, then driver: the driver watch knows each driver's vehicle even with no seat link.
			if (actor) RememberDriver(id); else g_lastMadeVehicle = id;
			return (At<std::uint32_t>(r, kFormFlags) & kPersistentFlag) != 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	void ApiShow(unsigned id, int on)
	{
		if (!on) vehiclediag::DropBirthHold(id);  // a hidden traffic vehicle is never held again
		const uptr ref = Ref(id);
		if (!ref) return;
		__try {
			const bool disabled = (At<std::uint32_t>(ref, kFormFlags) & kDisabledFlag) != 0;
			if (on && disabled) { EnableEngine engine{ ref }; placementenable::Restore(engine); }
			if (!on && !disabled) EngineFn<void (*)(uptr, char)>(addr::kDisableReference)(ref, 0);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {}
	}
	int ApiMoveTo(unsigned id, double x, double y, double z, double heading)
	{
        const int screen=ApiPresentation();
        if(!screen||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))return 0;
		uptr cell = 0, ws = 0;
		const uptr ref = Ref(id);
		if (!ref || !PlayerCell(cell, ws)) return 0;
        // In play (no cover) only out of sight at both ends: never a visible jump (2026-10-29 late, the fall guard's set-down). With no model nothing can be seen moving.
        bool hasModel=true;
        __try{const uptr loaded=At<uptr>(ref,off::kRefLoadedData);hasModel=loaded&&At<uptr>(loaded,off::kLoadedData3D);}__except(EXCEPTION_EXECUTE_HANDLER){}
        if(screen<2&&hasModel){const auto at=At<game::Vec3>(ref,off::kRefPosition);if(ApiExposure(at.x,at.y,at.z)!=2||ApiExposure(x,y,z)!=2)return 0;}
		float pos[3] = { float(x), float(y), float(z) }, rot[3] = { 0, 0, float(heading) };
		std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
		__try { EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(ref, &handle, cell, ws, pos, rot); return 1; }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	unsigned ApiVehicleOf(unsigned actorId)
	{
		const uptr d = Ref(actorId);
		const uptr v = d ? npcvehicles::VehicleOf(d) : 0;
		if (!v) return 0;
		__try { return At<std::uint32_t>(v, off::kFormId); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
	}
	int ApiSitState(unsigned actorId) { const uptr d = Ref(actorId); return d ? npcvehicles::SitState(d) : -1; }
	int ApiSnapSeat(unsigned actorId, unsigned vehicleId)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
		if (d && v) npcvehicles::BringToVehicle(d, v);
		return d && v ? npcvehicles::SnapNow(d, v) : -1;
	}
	int ApiUsesVehicle(unsigned vehicleId) { return npcvehicles::UsesVehicle(vehicleId) ? 1 : 0; }
    int ApiTripActive(unsigned id){return npcvehicles::UsesVehicle(id)||vehicleflight::RouteTripActive(id);}
	void ApiEndJob(unsigned vehicleId, const char* why) { npcvehicles::EndJobOf(vehicleId, why ? why : "traffic"); }
	int ApiTripRefusal(unsigned actorId, unsigned vehicleId, char* why, int size)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
		const char* r = d && v ? npcvehicles::ScriptedRefusal(d, v) : "vehicle or driver gone";
		if (!r || !*r) return 0;
		if (why && size > 0) { strncpy_s(why, size_t(size), r, _TRUNCATE); }
		return 1;
	}
	int ApiStartTrip(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
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
	int ApiTrafficPilotFree() { return npcvehicles::TrafficPilotFree() ? 1 : 0; }
	int ApiStartArmed(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
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
		const uptr d = Ref(actorId), v = Ref(vehicleId);
		return d && v && npcvehicles::MayUse(d, v) ? 1 : 0;
	}
	int ApiStartScripted(unsigned actorId, unsigned vehicleId, const FvoTripClaim* claim)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
		return d && v && claim && npcvehicles::StartScripted(d, v, *claim) ? 1 : 0;
	}
	int ApiStartSupply(unsigned actorId, unsigned vehicleId, double x, double y)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
		return d && v && npcvehicles::StartSupply(d, v, x, y) ? 1 : 0;
	}
	int ApiStartPatrol(unsigned actorId, unsigned vehicleId)
	{
		const uptr d = Ref(actorId), v = Ref(vehicleId);
		return d && v && npcvehicles::StartPatrol(d, v) ? 1 : 0;
	}

    int ApiAirSite(unsigned base,double x,double y,double z,double heading){
        FvoTrafficPlayer pl{};
        const int screen=ApiPresentation();
        if(!vehiclediag::BirthStageAvailable()||!screen||!ApiPlayer(&pl)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(heading))return 0;
        if(screen<2&&ApiExposure(x,y,z)!=2)return 0; // in play only where the player cannot see; covered spots wait
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
		if (g_attached.load() && g_hooks.tick) { g_hooks.tick(); WatchDrivers(); }
	}
	void Reset() { trafficarrival::Reset();g_madeDrivers.clear();g_watched.clear();g_pairHealth.clear();g_recoverTimes.clear();g_playerTook.clear();g_healthLaunch=true;g_lastMadeVehicle=0;g_watchLines=0;if (g_attached.load() && g_hooks.reset) g_hooks.reset(); }
	void RushTick() { if (g_attached.load() && g_hooks.housekeeping) g_hooks.housekeeping(); }
	void Rush(const RushPlace* places, int count)
	{
		if (!g_attached.load() || !g_hooks.rush) { logger::Write("Traffic: Start / Swap asked for, but FVO_Traffic.dll is not installed"); return; }
		static_assert(sizeof(RushPlace) == sizeof(FvoRushPlace), "RushPlace and FvoRushPlace must match");
		g_hooks.rush(reinterpret_cast<const FvoRushPlace*>(places), count);
	}
	int PairHealth(unsigned vehicleId) { return ApiPairHealth(vehicleId); }
	bool WellHiddenFar(unsigned vehicleId, double distance) { FvoTrafficRef v{}; return ApiRef(vehicleId, &v) && distance >= ApiNoticeFar(v.baseId); }
	bool MarkDriverForFarPass(unsigned driverId)
	{
		constexpr std::uint32_t kMarkerBase = 0x0000003B;  // XMarker: no model, no physics
		constexpr std::uint32_t kHasExtra = 0x002C5C90, kAttachTo = 0x0206A7B0;  // FUN_14206A7B0(parent, child): the core of ObjectReference.AttachTo
		constexpr int kAttachChildren = 0xA9;
		const uptr driver = Ref(driverId);
		if (!driver) return false;
		const auto marked = [](uptr d) { return (At<std::uint8_t>(d, 0x2C) & 4) && At<uptr>(d, 0xC8) && EngineFn<char (*)(uptr, int)>(kHasExtra)(At<uptr>(d, 0xC8), kAttachChildren); };
		uptr cell = 0, ws = 0;
		const uptr base = runtimeforms::Lookup(kMarkerBase);
		if (!base || !PlayerCell(cell, ws)) return false;
		FvoTrafficRef d{};
		if (!ApiRef(driverId, &d)) return false;
		bool already = false;
		__try { already = marked(driver); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }  // a save keeps it
		if (already) return true;
		const std::uint32_t markerId = Create(base, cell, ws, d.x, d.y, d.z, 0);
		const uptr marker = Ref(markerId);
		if (!marker) { logger::Write("Driver pass: no marker made for driver %08X", driverId); return false; }
		bool ok = false;
		__try {
			EngineFn<void (*)(uptr, char, char)>(addr::kMarkerPost1)(marker, 1, 0);  // persistent, or the cell it was made in would take it away
			EngineFn<void (*)(uptr, uptr)>(kAttachTo)(driver, marker);
			ok = marked(driver);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
		logger::Write("Driver pass: marker %08X (base %08X, type %d) attached to driver %08X -> %s", markerId, kMarkerBase, int(At<std::uint8_t>(base, 0x2E)), driverId, ok ? "exempt" : "the game did not take it");
		return ok;
	}
	bool PutBack(unsigned id, double x, double y, double z)
	{
		uptr cell = 0, ws = 0;
		const uptr ref = Ref(id);
		if (!ref || !PlayerCell(cell, ws)) return false;
		__try {
			float pos[3] = { float(x), float(y), float(z) }, rot[3] = { 0, 0, At<game::Vec3>(ref, off::kRefRotation).z };
			std::uint64_t handle = *reinterpret_cast<std::uint32_t*>(game::Base() + addr::kInvalidHandle);
			EngineFn<void (*)(uptr, std::uint64_t*, uptr, uptr, float*, float*)>(addr::kMoveReference)(ref, &handle, cell, ws, pos, rot);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
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
