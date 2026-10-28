#include "vehicleworld.h"

#include "addresses.h"
#include "bodymass.h"
#include "game.h"
#include "log.h"
#include "runtimeforms.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <mutex>

namespace world
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }

		constexpr std::uint32_t kBoundMin = 0x50, kBoundMax = 0x5C;  // TESBoundObject 1.16.244: six floats from +0x50 (the constructor FUN_140a6db30 zeroes +0x50..+0x67; 0x48/0x54 from CommonLibSF 1.14 read 8 bytes early: every base fell back to the default box, 2026-10-25)
		// The ref's 3D (used in game by actors.cpp / shipmodel.cpp): ref +0xB8 loaded data, +0x08 root. NiAVObject: world
		// rotation +0x80 (3 rows of 4 floats), world bound sphere +0x100 (centre xyz, radius +0x0C), collision object
		// +0x110, NiNode children +0x138 / count (u16) +0x142, vtable +0x20 = "as node".
		constexpr std::uint32_t kWorldRotate = 0x80, kWorldBound = 0x100, kCollision = 0x110, kChildren = 0x138, kChildCount = 0x142, kVIsNode = 0x20;
		// bhkNPCollisionObject: vfunc +0x138 non-null = physics collision; FUN_142376C70(colObj, uint32* out) = body id
		// (0xFFFFFF none) -- the game's own scene visitors (FUN_140F44DD0 ...); notes/VEHICLE_SHAPE_CAST_RESEARCH_20261017.md.
		constexpr std::uint32_t kVAsNP = 0x138, kBodyIdOf = 0x02376C70;
		constexpr std::uint32_t kNoBody = 0xFFFFFF;

		// Immutable copies published under a short mutex.
		std::mutex g_pubLock; // a seqlock cannot make concurrent non-atomic memcpy legal C++
		Snapshot g_pub;

		// Main-thread state.
		struct Track
		{
			float last[3] = {};
			float vel[3] = {};
			LARGE_INTEGER at{};
			bool have = false;
		};
		std::unordered_map<std::uint32_t, Track> g_track;
		// Per-vehicle cache (2026-10-27 FPS pass). The 16:03 run's 59-61 ms "Vehicle world spikes" were all "occupants":
		// the driver lookup makes engine calls (is-vehicle, the actor's vehicle, the driver seat) for every vehicle every
		// frame, and those wait while the engine loads cells. Placements add vehicles, so they multiplied the waits.
		//  - occupants: your driven vehicle and NPC job vehicles are known without the engine; the engine lookup is
		//    refreshed every 250 ms for moving / used vehicles, 1 s for parked ones, at most kDriverCallsPerFrame a frame;
		//  - the 3D and its bodies stay live every frame for every vehicle (a parked vehicle's 3D loading, or a new physics
		//    generation under the same root, must show at once: stale body ids broke the chassis binding before); the
		//    16:03 breakdown put the scene walk at 0.00 ms anyway.
		struct Cached
		{
			VehicleInfo info{};
			ULONGLONG driverAt = 0;
			uptr root = 0;
			std::uint32_t driverId = 0;
			bool have = false;
		};
		std::unordered_map<std::uint32_t, Cached> g_cache;
		constexpr int kDriverCallsPerFrame = 3;
		constexpr ULONGLONG kDriverActiveMs = 250, kDriverParkedMs = 1000;
		std::uint32_t g_ids[64] = {};
		std::uint32_t g_idCount = 0;
		std::uint64_t g_frame = 0;
		bool g_toldBounds = false;
        vehiclelinks::Services g_services;

		// Bounded reference slices, including within a dense city cell, published
		// to g_ids when it has passed every cell, instead of every cell of a city in one frame each second.
		std::uint32_t g_sweepIds[64] = {};
		std::uint32_t g_sweepCount = 0;
		framework::ScanCursor g_sweepCursor;
		void CollectSweep(uptr ref, void* ctx)
		{
			(void)ctx;
			const auto id=At<std::uint32_t>(ref,off::kFormId);
            if(g_sweepCount<64&&std::find(g_sweepIds,g_sweepIds+g_sweepCount,id)==g_sweepIds+g_sweepCount)
                g_sweepIds[g_sweepCount++]=id;
		}

		uptr Lookup(std::uint32_t id)
		{
			__try {
				return runtimeforms::Lookup(id);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		uptr Driven(uptr player)
		{
			__try {
				return player ? EngineFn<uptr (*)(uptr)>(addr::kPlayerVehicle)(player) : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return 0;
			}
		}

		// Half-extents from the base's bounds; a fallback (a REV-8-sized box) when they look wrong.
		bool Bounds(uptr base, float half[3], float centre[3])
		{
			const float* mn = reinterpret_cast<const float*>(base + kBoundMin);
			const float* mx = reinterpret_cast<const float*>(base + kBoundMax);
			bool ok = true;
			for (int i = 0; i < 3; ++i) {
				const float h = 0.5f * (mx[i] - mn[i]);
				if (!std::isfinite(h) || h < 0.25f || h > 100000.0f) ok = false;
				half[i] = std::max(h, 0.25f); centre[i]=(mx[i]+mn[i])*0.5f;
                if(!std::isfinite(centre[i]))ok=false;
			}
			if (!ok) {
				half[0] = 1.25f; half[1] = 2.3f; half[2] = 1.0f; centre[0]=centre[1]=centre[2]=0;
			}
			if (!g_toldBounds) {
				g_toldBounds = true;
				logger::Write("Vehicle world: base %08X bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) -> half %.2f %.2f %.2f%s", At<std::uint32_t>(base, off::kFormId), mn[0], mn[1],
					mn[2], mx[0], mx[1], mx[2], half[0], half[1], half[2], ok ? "" : " (implausible: default box)");
			}
			return ok;
		}

		uptr Root(uptr ref)
		{
			const uptr loaded = At<uptr>(ref, off::kRefLoadedData);
			return loaded ? At<uptr>(loaded, off::kLoadedData3D) : 0;
		}

		uptr VCall(uptr obj, std::uint32_t slot) { return reinterpret_cast<uptr (*)(uptr)>((*reinterpret_cast<void***>(obj))[slot / 8])(obj); }

		// Enumerate owned collision bodies; the native vehicle callback selects its chassis from this set.
		void BodiesIn(uptr obj,int depth,std::uint32_t ids[16],std::uint32_t&count,unsigned&visited)
		{
			if(!obj||depth>16||count>=16||++visited>512)return;
			if(const auto col=At<uptr>(obj,kCollision);col&&VCall(col,kVAsNP)) {
				std::uint32_t id=kNoBody;EngineFn<std::uint32_t*(*)(uptr,std::uint32_t*)>(kBodyIdOf)(col,&id);
				if(id && (id&0xFFFFFF)!=kNoBody) {
					bool duplicate=false;for(unsigned i=0;i<count;++i)duplicate=duplicate||ids[i]==id;
					if(!duplicate)ids[count++]=id;
				}
			}
			if(const auto node=VCall(obj,kVIsNode)) {
				const auto n=At<std::uint16_t>(node,kChildCount);const auto kids=At<uptr*>(node,kChildren);
				for(unsigned i=0;kids&&i<n&&i<128&&count<16&&visited<512;++i)BodiesIn(kids[i],depth+1,ids,count,visited);
			}
		}

		// Rotation, bound radius and scale from the ref's 3D (no scene-graph walk); false when it is not loaded or faults.
		bool SceneLive(uptr ref, uptr& rootOut, float rot[9], float& radius, float& scale)
		{
			__try {
				const uptr root = Root(ref);
				rootOut = root;
				if (!root) return false;
				const float* m = reinterpret_cast<const float*>(root + kWorldRotate);
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 3; ++c) rot[r * 3 + c] = m[r * 4 + c];
				radius = At<float>(root, kWorldBound + 0x0C);
				scale = std::fabs(At<float>(root, 0xBC));
				if (!std::isfinite(scale) || scale < 0.001f || scale > 100000.0f) scale = 1;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
		bool SceneBodiesOf(uptr root, std::uint32_t ids[16], std::uint32_t& count)
		{
			__try {
				count = 0;
				unsigned visited = 0;
				BodiesIn(root, 0, ids, count, visited);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				count = 0;
				return false;
			}
		}

		// Rotation, bound radius and body id from the ref's 3D; false when it is not loaded or reading faults.
		bool FromScene(uptr ref, float rot[9], float& radius, std::uint32_t& body, std::uint32_t ids[16],std::uint32_t&count,float&scale)
		{
			__try {
				const uptr root = Root(ref);
				if (!root) return false;
				const float* m = reinterpret_cast<const float*>(root + kWorldRotate);
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 3; ++c) rot[r * 3 + c] = m[r * 4 + c];
				radius = At<float>(root, kWorldBound + 0x0C);
				scale=std::fabs(At<float>(root,0xBC)); // NiTransform world scale
				if(!std::isfinite(scale)||scale<0.001f||scale>100000.0f)scale=1;
				unsigned visited=0;BodiesIn(root,0,ids,count,visited);body=count?ids[0]:0;
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

        void Write(const Snapshot& s)
		{
            std::lock_guard lk(g_pubLock);
            g_pub=s;
		}
	}

    unsigned SceneBodies(std::uintptr_t ref,std::uint32_t (&ids)[16]) {
        float rot[9]{},radius=0,scale=1;std::uint32_t body=0,count=0;
        if(!FromScene(ref,rot,radius,body,ids,count,scale))return 0;
        return count;
    }
	void Publish()
	{
        if(!g_services){static const Snapshot empty{};Write(empty);return;}
		const ULONGLONG now = GetTickCount64();
		LARGE_INTEGER scanStart{},scanEnd{},scanFrequency{};
        QueryPerformanceCounter(&scanStart);QueryPerformanceFrequency(&scanFrequency);
		if (g_services.scan(&CollectSweep, nullptr, g_sweepCursor)) {
			std::copy(g_sweepIds, g_sweepIds + g_sweepCount, g_ids);
			g_idCount = g_sweepCount;
			g_sweepCount = 0;
		}
		QueryPerformanceCounter(&scanEnd);
		LARGE_INTEGER qpc, freq;
		QueryPerformanceCounter(&qpc);
		QueryPerformanceFrequency(&freq);
		static Snapshot s;  // big: built in place, not on the stack
		s.frame = ++g_frame;
		s.count = 0;
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		const uptr driven = Driven(player);
        // Player control must not wait for the first incremental city sweep.
        const auto remember=[&](std::uint32_t id){
            if(!id||std::find(g_ids,g_ids+g_idCount,id)!=g_ids+g_idCount)return;
            if(g_idCount<64)g_ids[g_idCount++]=id;else g_ids[63]=id;
        };
        remember(g_services.owned());
        if(driven)remember(At<std::uint32_t>(driven,off::kFormId));
        double sceneMs=0,massMs=0,driverMs=0,slowMs=0;std::uint32_t slowId=0;
        const auto milliseconds=[&](LARGE_INTEGER a,LARGE_INTEGER b){return 1000.0*double(b.QuadPart-a.QuadPart)/freq.QuadPart;};
		for (std::uint32_t i = 0; i < g_idCount; ++i) {
			LARGE_INTEGER vehicleStart{},partStart{},partEnd{};QueryPerformanceCounter(&vehicleStart);
			const std::uint32_t id = g_ids[i];
			const uptr r = Lookup(id);
			if (!r || (At<std::uint32_t>(r, 0x20) & 0x800) || !At<uptr>(r, off::kRefParentCell)) continue;
			const uptr base = At<uptr>(r, 0x98);
			if (!base) continue;
			VehicleInfo& v = s.v[s.count];
			Cached& cache = g_cache[id];
			const bool usedNow = r == driven || id == g_services.owned() || g_services.inUse(id);
			v = VehicleInfo{};
			v.formId = id;
			v.baseId = At<std::uint32_t>(base, off::kFormId);
			v.ref = r;
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
			v.heading = At<game::Vec3>(r, off::kRefRotation).z;
			const float sn = std::sin(v.heading), cs = std::cos(v.heading);
			const float rot[9] = { cs, -sn, 0, sn, cs, 0, 0, 0, 1 };  // right, forward, up (heading only)
			std::memcpy(v.rot, rot, sizeof(rot));
			float localCentre[3]{}; v.baseBoundsValid=Bounds(base, v.half,localCentre);
			float sceneRot[9], radius = 0,sceneScale=1;
			std::uint32_t body = 0;
			QueryPerformanceCounter(&partStart);
			uptr root = 0;
			bool sceneOk = SceneLive(r, root, sceneRot, radius, sceneScale);
			if (sceneOk) {
				// The body walk runs on every full sample: body ids can change under the same 3D root (a new physics
				// generation) and stale ids break the chassis binding. Parked vehicles only take a full sample once a second.
				SceneBodiesOf(root, v.bodyIds, v.bodyCount);
				cache.root = root;
				body = v.bodyCount ? v.bodyIds[0] : 0;
			}
			if (sceneOk) {
				std::memcpy(v.rot, sceneRot, sizeof(sceneRot));  // the 3D's world rotation (pitch and roll too)
				v.bodyId = body;
				for(auto&half:v.half)half*=sceneScale;
				// the live bound sphere caps the box (a mod's bogus bounds cannot make it larger than the model)
				if (std::isfinite(radius) && radius > 0.5f && radius < 100000.0f) {
					for (int k = 0; k < 3; ++k) v.half[k] = std::min(v.half[k], radius);
				}
			}
			QueryPerformanceCounter(&partEnd);sceneMs+=milliseconds(partStart,partEnd);
			for(int axis=0;axis<3;++axis){v.center[axis]=v.pos[axis];for(int local=0;local<3;++local)v.center[axis]+=v.rot[local*3+axis]*localCentre[local]*sceneScale;}
            // Velocity: smoothed position difference (a jump over 50 m restarts it).
			Track& t = g_track[id];
            // The callback has current chassis mass while it already owns native
            // physics access. Never run the synchronous reference mass visitor here.
            QueryPerformanceCounter(&partStart);
            const float mass=bodymass::observations.Read({v.bodyIds,v.bodyCount},now);
            v.nativeMass=mass>0;v.mass=v.nativeMass?mass:float(std::clamp(1500.0*double(v.half[0])*v.half[1]*v.half[2]/(1.25*2.3),100.0,100000.0));
            QueryPerformanceCounter(&partEnd);massMs+=milliseconds(partStart,partEnd);
			if (t.have) {
				const double dt = double(qpc.QuadPart - t.at.QuadPart) / double(freq.QuadPart);
				const float jump = std::hypot(p.x - t.last[0], p.y - t.last[1]);
				{
					// A parked vehicle that moves by itself (the hopping small vehicles, 2026-10-18): name it.
					const float d3 = std::sqrt((p.x - t.last[0]) * (p.x - t.last[0]) + (p.y - t.last[1]) * (p.y - t.last[1]) + (p.z - t.last[2]) * (p.z - t.last[2]));
					static unsigned hopLogged = 0;
					if (!r || r == driven || g_services.inUse(id)) {
					}
					else if (d3 > 0.6f && d3 < 50.0f && hopLogged < 40) {
						++hopLogged;
						if (logger::Verbose()) logger::Write("Vehicle hop: %08X (base %08X) moved %.1f m while parked, to (%.1f, %.1f, %.1f)", id, v.baseId, d3, p.x, p.y, p.z);
					}
				}
				if (dt > 1e-4 && dt < 0.5 && jump < 50.0f) {
					const float a = float(std::min(1.0, dt * 8.0));
					const float cur[3] = { p.x - t.last[0], p.y - t.last[1], p.z - t.last[2] };
					for (int k = 0; k < 3; ++k) t.vel[k] += (float(cur[k] / dt) - t.vel[k]) * a;
				}
				else if (jump >= 50.0f) {
					t.vel[0] = t.vel[1] = t.vel[2] = 0;
				}
			}
			t.last[0] = p.x; t.last[1] = p.y; t.last[2] = p.z;
			t.at = qpc;
			t.have = true;
			std::memcpy(v.vel, t.vel, sizeof(v.vel));
			QueryPerformanceCounter(&partStart);
			// Occupants: your driven vehicle needs no engine call; the engine lookup is cached and rationed.
			static ULONGLONG callFrame = 0;
			static int callsThisFrame = 0;
			if (callFrame != g_frame) { callFrame = g_frame; callsThisFrame = 0; }
			const bool moving = std::hypot(t.vel[0], t.vel[1]) > 0.3f;
			const ULONGLONG driverEvery = (usedNow || moving) ? kDriverActiveMs : kDriverParkedMs;
			if (r == driven && player) { cache.driverId = At<std::uint32_t>(player, off::kFormId); cache.driverAt = now; }
			else if (!cache.have || now - cache.driverAt >= driverEvery) {
				// New vehicles wait their turn too (a cell load brings many at once); until then they read as parked, which is
				// safe: your driven vehicle and NPC job vehicles are known without the lookup.
				if (callsThisFrame < kDriverCallsPerFrame) { ++callsThisFrame; cache.driverId = g_services.driver(id); cache.driverAt = now; }
			}
			v.driverId = cache.driverId;
            if (player && v.driverId==At<std::uint32_t>(player,off::kFormId)) {
                v.who=VehicleInfo::Who::Player;
            }
			else if (v.driverId || g_services.inUse(id)) {
				v.who = VehicleInfo::Who::Npc;
			}
			else {
				v.who = VehicleInfo::Who::Parked;
			}
            QueryPerformanceCounter(&partEnd);driverMs+=milliseconds(partStart,partEnd);
            const auto vehicleMs=milliseconds(vehicleStart,partEnd);
            if(vehicleMs>slowMs){slowMs=vehicleMs;slowId=id;}
			cache.info = v;
			cache.have = true;
			if (++s.count >= 64) break;
		}
		if (g_track.size() > 256) g_track.clear();
		if (g_cache.size() > 256) g_cache.clear();
		Write(s);
        LARGE_INTEGER finished{};QueryPerformanceCounter(&finished);
        const double scanMs=1000.0*double(scanEnd.QuadPart-scanStart.QuadPart)/scanFrequency.QuadPart;
        const double sampleMs=1000.0*double(finished.QuadPart-scanEnd.QuadPart)/scanFrequency.QuadPart;
        static ULONGLONG reported=0;
        if(scanMs+sampleMs>6&&now-reported>=2000){reported=now;logger::Write("Vehicle world spike: discovery %.2f ms, live %.2f ms (%u vehicles): scene %.2f, observed mass %.2f, occupants %.2f, other %.2f; slowest %08X %.2f ms",scanMs,sampleMs,s.count,sceneMs,massMs,driverMs,std::max(0.0,sampleMs-sceneMs-massMs-driverMs),slowId,slowMs);}
	}

	void Reset()
	{
		g_cache.clear();
		bodymass::observations.Reset();
		g_track.clear();
		g_idCount = 0;
		g_sweepCount = 0;
		g_sweepCursor = {};
		static Snapshot empty;
		empty = Snapshot{};
		Write(empty);
	}
    void SetServices(vehiclelinks::Services services){g_services=services;Reset();}

	bool Vehicles(Snapshot& out)
	{
        std::lock_guard lk(g_pubLock);
        out=g_pub;
        return out.frame!=0;
	}

	bool FirstInPath(const float p[3], const float d[3], const float half[3], std::uint32_t ignoreFormId, std::uint32_t ignoreBodyId, VehicleInfo* hit,
		float* distanceOut)
	{
		static thread_local Snapshot s;
		if (!Vehicles(s)) return false;
		const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (len < 1e-3f) return false;
		const float rSelf = std::hypot(half[0], half[1]);
		float best = 2.0f;
		int bestI = -1;
		for (std::uint32_t i = 0; i < s.count; ++i) {
			const VehicleInfo& v = s.v[i];
			if ((ignoreFormId && v.formId == ignoreFormId) || (ignoreBodyId && v.bodyId && v.bodyId == ignoreBodyId)) continue;
			// heights must overlap somewhere along the move (both boxes centred on their positions)
			const float zLo = std::min(p[2], p[2] + d[2]) - half[2], zHi = std::max(p[2], p[2] + d[2]) + half[2];
			if (v.pos[2] + v.half[2] < zLo || v.pos[2] - v.half[2] > zHi) continue;
			// first t in [0, 1] where the horizontal distance to v drops under rSelf + rOther
			const float R = rSelf + std::hypot(v.half[0], v.half[1]);
			const float ox = p[0] - v.pos[0], oy = p[1] - v.pos[1];
			const float a = d[0] * d[0] + d[1] * d[1], b = 2.0f * (ox * d[0] + oy * d[1]), c = ox * ox + oy * oy - R * R;
			float t;
			if (c <= 0.0f) t = 0.0f;  // already overlapping
			else if (a < 1e-6f) continue;
			else {
				const float disc = b * b - 4.0f * a * c;
				if (disc < 0.0f) continue;
				t = (-b - std::sqrt(disc)) / (2.0f * a);
				if (t < 0.0f || t > 1.0f) continue;
			}
			if (t < best) { best = t; bestI = int(i); }
		}
		if (bestI < 0) return false;
		if (hit) *hit = s.v[bestI];
		if (distanceOut) *distanceOut = best * len;
		return true;
	}
	bool BaseSize(std::uintptr_t base, float half[3], float centre[3])
	{
		half[0] = 1.25f; half[1] = 2.3f; half[2] = 1.0f; centre[0] = centre[1] = centre[2] = 0;
		if (!base) return false;
		__try { return Bounds(base, half, centre); }
		__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
	}
}
