#include "vehicleworld.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "npcvehicles.h"
#include "realvehicles.h"

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

		constexpr std::uint32_t kBoundMin = 0x48, kBoundMax = 0x54;  // TESBoundObject (CommonLibSF 1.14; checked for sanity)
		// The ref's 3D (used in game by actors.cpp / shipmodel.cpp): ref +0xB8 loaded data, +0x08 root. NiAVObject: world
		// rotation +0x80 (3 rows of 4 floats), world bound sphere +0x100 (centre xyz, radius +0x0C), collision object
		// +0x110, NiNode children +0x138 / count (u16) +0x142, vtable +0x20 = "as node".
		constexpr std::uint32_t kWorldRotate = 0x80, kWorldBound = 0x100, kCollision = 0x110, kChildren = 0x138, kChildCount = 0x142, kVIsNode = 0x20;
		// bhkNPCollisionObject: vfunc +0x138 non-null = physics collision; FUN_142376C70(colObj, uint32* out) = body id
		// (0xFFFFFF none) -- the game's own scene visitors (FUN_140F44DD0 ...); notes/VEHICLE_SHAPE_CAST_RESEARCH_20261017.md.
		constexpr std::uint32_t kVAsNP = 0x138, kBodyIdOf = 0x02376C70;
		constexpr std::uint32_t kNoBody = 0xFFFFFF;
		constexpr ULONGLONG kScanMs = 1000;                         // the cell scan; positions update every frame

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
            float mass=0; ULONGLONG massAt=0; std::uint32_t massBody=0;
		};
		std::unordered_map<std::uint32_t, Track> g_track;
		std::uint32_t g_ids[64] = {};
		std::uint32_t g_idCount = 0;
		ULONGLONG g_lastScan = 0;
		std::uint64_t g_frame = 0;
		bool g_toldBounds = false;

		void Collect(uptr ref, void* ctx)
		{
			(void)ctx;
			if (g_idCount < 64) g_ids[g_idCount++] = At<std::uint32_t>(ref, off::kFormId);
		}

		uptr Lookup(std::uint32_t id)
		{
			__try {
				return EngineFn<uptr (*)(std::uint32_t)>(0x005DE3B0)(id);
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
		void Bounds(uptr base, float half[3], float centre[3])
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

		float NativeMass(uptr ref) {
            __try {
                constexpr unsigned char sig[]{0x48,0x83,0xEC,0x48,0x49,0x8B,0x00,0x48,0x8D,0x54,0x24,0x68,0x49,0x8B,0xC8,0xFF};
                if(std::memcmp(reinterpret_cast<void*>(game::Base()+0x02072450),sig,sizeof(sig)))return 0;
                const float mass=EngineFn<float(*)(uptr,uptr,uptr)>(0x02072450)(0,0,ref);
                return std::isfinite(mass)&&mass>0.01f&&mass<100000000.0f?mass:0;
            }__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
        }
        void Write(const Snapshot& s)
		{
            std::lock_guard lk(g_pubLock);
            g_pub=s;
		}
	}

	void Publish()
	{
		const ULONGLONG now = GetTickCount64();
		if (now - g_lastScan >= kScanMs) {
			g_lastScan = now;
			g_idCount = 0;
			realvehicles::ForEachLoaded(&Collect, nullptr);
		}
		LARGE_INTEGER qpc, freq;
		QueryPerformanceCounter(&qpc);
		QueryPerformanceFrequency(&freq);
		static Snapshot s;  // big: built in place, not on the stack
		s.frame = ++g_frame;
		s.count = 0;
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		const uptr driven = Driven(player);
        unsigned massReads=0;
		for (std::uint32_t i = 0; i < g_idCount; ++i) {
			const std::uint32_t id = g_ids[i];
			const uptr r = Lookup(id);
			if (!r || (At<std::uint32_t>(r, 0x20) & 0x800) || !At<uptr>(r, off::kRefParentCell)) continue;
			const uptr base = At<uptr>(r, 0x98);
			if (!base) continue;
			VehicleInfo& v = s.v[s.count];
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
			float localCentre[3]{}; Bounds(base, v.half,localCentre);
			float sceneRot[9], radius = 0,sceneScale=1;
			std::uint32_t body = 0;
			if (FromScene(r, sceneRot, radius, body,v.bodyIds,v.bodyCount,sceneScale)) {
				std::memcpy(v.rot, sceneRot, sizeof(sceneRot));  // the 3D's world rotation (pitch and roll too)
				v.bodyId = body;
				for(auto&half:v.half)half*=sceneScale;
				// the live bound sphere caps the box (a mod's bogus bounds cannot make it larger than the model)
				if (std::isfinite(radius) && radius > 0.5f && radius < 100000.0f) {
					for (int k = 0; k < 3; ++k) v.half[k] = std::min(v.half[k], radius);
				}
			}
			for(int axis=0;axis<3;++axis){v.center[axis]=v.pos[axis];for(int local=0;local<3;++local)v.center[axis]+=v.rot[local*3+axis]*localCentre[local]*sceneScale;}
            // Velocity: smoothed position difference (a jump over 50 m restarts it).
			Track& t = g_track[id];
            if(t.massBody!=v.bodyId){t.mass=0;t.massAt=0;t.massBody=v.bodyId;}
            if(massReads<2&&(!t.massAt||now-t.massAt>5000)){++massReads;t.mass=NativeMass(r);t.massAt=now;}
            v.nativeMass=t.mass>0; v.mass=v.nativeMass?t.mass:float(std::clamp(1500.0*double(v.half[0])*v.half[1]*v.half[2]/(1.25*2.3),100.0,100000.0));
			if (t.have) {
				const double dt = double(qpc.QuadPart - t.at.QuadPart) / double(freq.QuadPart);
				const float jump = std::hypot(p.x - t.last[0], p.y - t.last[1]);
				{
					// A parked vehicle that moves by itself (the hopping small vehicles, 2026-10-18): name it.
					const float d3 = std::sqrt((p.x - t.last[0]) * (p.x - t.last[0]) + (p.y - t.last[1]) * (p.y - t.last[1]) + (p.z - t.last[2]) * (p.z - t.last[2]));
					static unsigned hopLogged = 0;
					if (!r || r == driven || npcvehicles::UsesVehicle(id)) {
					}
					else if (d3 > 0.6f && d3 < 50.0f && hopLogged < 40) {
						++hopLogged;
						logger::Write("Vehicle hop: %08X (base %08X) moved %.1f m while parked, to (%.1f, %.1f, %.1f)", id, v.baseId, d3, p.x, p.y, p.z);
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
			v.driverId=npcvehicles::VehicleDriverId(id);
            if (player && v.driverId==At<std::uint32_t>(player,off::kFormId)) {
                v.who=VehicleInfo::Who::Player;
            }
			else if (v.driverId || npcvehicles::UsesVehicle(id)) {
				v.who = VehicleInfo::Who::Npc;
			}
			else {
				v.who = VehicleInfo::Who::Parked;
			}
			if (++s.count >= 64) break;
		}
		if (g_track.size() > 256) g_track.clear();
		Write(s);
	}

	void Reset()
	{
		g_track.clear();
		g_idCount = 0;
		g_lastScan = 0;
		static Snapshot empty;
		empty = Snapshot{};
		Write(empty);
	}

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
}
