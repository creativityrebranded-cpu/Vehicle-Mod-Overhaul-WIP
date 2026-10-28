#include "navseed.h"
#include "runtimeforms.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "navmap.h"
#include "vehicleworld.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace navseed
{
	namespace
	{
		using uptr = std::uintptr_t;
		template <class T>
		T& At(uptr p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		template <class Fn>
		Fn EngineFn(std::uint32_t rva) { return reinterpret_cast<Fn>(game::Base() + rva); }
		uptr Ptr(std::uint32_t rva) { return *reinterpret_cast<uptr*>(game::Base() + rva); }

		constexpr double kRadius = 250.0;          // m around the player
		constexpr ULONGLONG kPassMs = 2000;        // a new pass this often
		constexpr unsigned kPerFrame = 300;        // references looked at per frame
		// 8000 like the NPC systems (2026-10-27 stutter pass): asking for more than another caller already took this frame
		// forced a second full scene scan in the same frame (up to 12000 engine reference visits).
		constexpr unsigned kRefLimit = 8000;
		constexpr double kMaxHalf = 60.0;          // m: bigger boxes are left to the probes
		constexpr double kMinHalf = 0.2;
		// The same 3D and body reads as vehicleworld.cpp (proven there): ref +0xB8 loaded data, +0x08 root; NiAVObject collision object +0x110,
		// world bound sphere +0x100 (centre xyz, radius +0x0C), children +0x138 / count +0x142, vtable +0x20 "as node"; bhkNPCollisionObject
		// vfunc +0x138 = physics collision, FUN_142376C70(colObj, uint32* out) = body id.
		constexpr std::uint32_t kBoundMin = 0x50, kBoundMax = 0x5C /* 1.16.244 TESBoundObject bounds (were 0x48/0x54: 8 bytes early, 2026-10-25) */, kWorldBound = 0x100, kCollision = 0x110, kChildren = 0x138, kChildCount = 0x142,
			kVIsNode = 0x20, kVAsNP = 0x138, kBodyIdOf = 0x02376C70, kNoBody = 0xFFFFFF;

		// Capture IDs once per pass; resolve each small slice in its current frame.
		// No raw reference pointers survive a frame or an unload.
        std::vector<std::uint32_t> g_ids;
#ifdef SP_RUNTIME_PERF_TEST
        ULONGLONG g_testNow=10000;
#endif
		bool g_inPass = false;
		std::size_t g_pos = 0;
		ULONGLONG g_passAt = 0, g_logAt = 0;
		double g_cx = 0, g_cy = 0;
		unsigned g_objects = 0, g_looked = 0, g_tooBig = 0;
		std::unordered_set<std::uint32_t> g_vehicles;
		struct Solid { std::uint32_t id = 0; std::int8_t solid = -1; };
		std::unordered_map<uptr, Solid> g_solid;  // per reference: has a physics body (checked once)

		uptr VCall(uptr obj, std::uint32_t slot) { return reinterpret_cast<uptr (*)(uptr)>((*reinterpret_cast<void***>(obj))[slot / 8])(obj); }

		bool HasBody(uptr obj, int depth, int& budget)
		{
			if (!obj || depth > 8 || --budget < 0) return false;
			if (const uptr col = At<uptr>(obj, kCollision)) {
				if (VCall(col, kVAsNP)) {
					std::uint32_t id = kNoBody;
					EngineFn<std::uint32_t* (*)(uptr, std::uint32_t*)>(kBodyIdOf)(col, &id);
					if ((id & 0xFFFFFF) != kNoBody) return true;
				}
			}
			if (const uptr node = VCall(obj, kVIsNode)) {
				const auto n = At<std::uint16_t>(node, kChildCount);
				const auto kids = At<uptr*>(node, kChildren);
				for (unsigned i = 0; kids && i < n && i < 64; ++i) {
					if (HasBody(kids[i], depth + 1, budget)) return true;
				}
			}
			return false;
		}

		// Guarded: -1 fault / not loaded, 0 no body, 1 solid. Also the root's world bound sphere.
		int ReadSolid(uptr ref, float* sphere)
		{
			__try {
				const uptr loaded = At<uptr>(ref, off::kRefLoadedData);
				const uptr root = loaded ? At<uptr>(loaded, off::kLoadedData3D) : 0;
				if (!root) return -1;
				for (int i = 0; i < 4; ++i) sphere[i] = At<float>(root, kWorldBound + 4 * i);
				int budget = 400;
				return HasBody(root, 0, budget) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -1;
			}
		}

		// One reference: a solid object's box into the nav map; false when it is not one.
		bool Seed(uptr r)
		{
			if (!r) return false;
			if (At<std::uint8_t>(r, 0x2E) == 'K') return false;           // an actor
			if ((At<std::uint32_t>(r, 0x20) & 0x800) != 0) return false;   // disabled
			const std::uint32_t id = At<std::uint32_t>(r, off::kFormId);
			if (g_vehicles.count(id)) return false;                         // vehicles have their own predictions
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			if (!std::isfinite(p.x) || std::hypot(double(p.x) - g_cx, double(p.y) - g_cy) > kRadius) return false;
			const uptr base = At<uptr>(r, 0x98);
			if (!base) return false;
			float sphere[4] = {};
			auto& sc = g_solid[r];
			if (sc.id != id || sc.solid < 0) {
				const int v = ReadSolid(r, sphere);
				sc.id = id;
				sc.solid = std::int8_t(v);
				if (v < 0) return false;
			}
			else if (ReadSolid(r, sphere) < 0) {
				return false;
			}
			if (sc.solid != 1) return false;
			const float* mn = reinterpret_cast<const float*>(base + kBoundMin);
			const float* mx = reinterpret_cast<const float*>(base + kBoundMax);
			double half[3], mid[3];
			for (int i = 0; i < 3; ++i) {
				if (!std::isfinite(mn[i]) || !std::isfinite(mx[i]) || mx[i] < mn[i]) return false;
				half[i] = 0.5 * (double(mx[i]) - mn[i]);
				mid[i] = 0.5 * (double(mx[i]) + mn[i]);
			}
			// Scale from the live bound sphere against the base box (rocks and props are often scaled); never more than 10x either way.
			const double lr = std::sqrt(half[0] * half[0] + half[1] * half[1] + half[2] * half[2]);
			double scale = 1.0;
			if (lr > 0.1 && std::isfinite(sphere[3]) && sphere[3] > 0.05) scale = std::clamp(double(sphere[3]) / lr, 0.1, 10.0);
			for (int i = 0; i < 3; ++i) { half[i] *= scale; mid[i] *= scale; }
			if (half[0] < kMinHalf && half[1] < kMinHalf) return false;
			if (half[0] > kMaxHalf || half[1] > kMaxHalf) { ++g_tooBig; return false; }
			const auto rot = At<game::Vec3>(r, off::kRefRotation);
			const bool upright = std::fabs(rot.x) < 0.35f && std::fabs(rot.y) < 0.35f;
			double cx, cy, hx, hy, zlo, zhi, yaw;
			if (upright) {
				yaw = rot.z;
				const double c = std::cos(yaw), s = std::sin(yaw);
				cx = p.x + mid[0] * c + mid[1] * s;  // local right (cos, -sin), forward (sin, cos)
				cy = p.y - mid[0] * s + mid[1] * c;
				hx = half[0];
				hy = half[1];
				zlo = p.z + mid[2] - half[2];
				zhi = p.z + mid[2] + half[2];
			}
			else {
				// Tilted: the bound sphere's box (conservative).
				if (!std::isfinite(sphere[0]) || !(sphere[3] > 0)) return false;
				yaw = 0;
				cx = sphere[0];
				cy = sphere[1];
				hx = hy = std::min(double(sphere[3]), kMaxHalf);
				zlo = sphere[2] - sphere[3];
				zhi = sphere[2] + sphere[3];
			}
			navmap::NoteObject(cx, cy, yaw, hx, hy, zlo, zhi);
			return true;
		}
	}

    namespace {
        std::uint32_t FormId(uptr ref) {
            __try {return ref?At<std::uint32_t>(ref,off::kFormId):0;}
            __except(EXCEPTION_EXECUTE_HANDLER){return 0;}
        }
        bool SeedId(std::uint32_t id,uptr player) {
            __try {
                const auto ref=runtimeforms::Lookup(id);
                return ref&&ref!=player&&At<std::uint32_t>(ref,off::kFormId)==id&&Seed(ref);
            } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
        }
    }
	void Tick()
	{
		const uptr player = Ptr(addr::kPlayerCharacterPtr);
		if (!player) return;
#ifdef SP_RUNTIME_PERF_TEST
        const ULONGLONG now=g_testNow;
#else
		const ULONGLONG now = GetTickCount64();
#endif
		if (!g_inPass || g_pos >= g_ids.size()) {
			if (g_inPass) {
				navmap::NoteSeeded(g_cx, g_cy, kRadius);  // the pass is complete: the area is known
				if (now - g_logAt >= (logger::Verbose() ? 10000ull : 600000ull) || !g_logAt) {
					g_logAt = now;
					logger::Write("Nav seed: %u solid objects from %u references within %.0f m (%u too big for a box)", g_objects, g_looked, kRadius, g_tooBig);
				}
				g_inPass = false;
			}
			if (now - g_passAt < kPassMs) return;
            g_ids.clear();
            for(const auto ref:modscene::LoadedRefs(kRefLimit))if(const auto id=FormId(ref))g_ids.push_back(id);
			g_passAt = now;
			const auto pp = At<game::Vec3>(player, off::kRefPosition);
			g_cx = pp.x;
			g_cy = pp.y;
			g_inPass = true;
			g_pos = 0;
			g_objects = g_looked = g_tooBig = 0;
			g_vehicles.clear();
			static world::Snapshot snap;
			if (world::Vehicles(snap)) {
				for (std::uint32_t i = 0; i < snap.count && i < 64; ++i) g_vehicles.insert(snap.v[i].formId);
			}
			if (g_solid.size() > 30000) g_solid.clear();
		}
		const std::size_t end = std::min(g_ids.size(), g_pos + kPerFrame);
        LARGE_INTEGER began{},frequency{};QueryPerformanceCounter(&began);QueryPerformanceFrequency(&frequency);
		const auto first=g_pos;
		for (; g_pos < end; ++g_pos) {
            if(g_pos>first&&(g_pos-first)%16==0){LARGE_INTEGER at{};QueryPerformanceCounter(&at);if(double(at.QuadPart-began.QuadPart)>frequency.QuadPart*0.0005)break;}
			++g_looked;
			if (SeedId(g_ids[g_pos],player)) ++g_objects;
		}
	}

	void Reset()
	{
		g_inPass = false;
		g_pos = 0;
		g_passAt = 0;
		g_solid.clear();
		g_vehicles.clear();
        g_ids.clear();
	}
#ifdef SP_RUNTIME_PERF_TEST
    namespace testing {void Clock(ULONGLONG now){g_testNow=now;}}
#endif
}
