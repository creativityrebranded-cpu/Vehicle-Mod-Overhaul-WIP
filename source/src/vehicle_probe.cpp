#include "vehicle_probe.h"

#include "addresses.h"
#include "game.h"
#include "hook.h"
#include "log.h"
#include "prefetch.h"
#include "shipmodel.h"

#include <Windows.h>
#include <array>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <mutex>

namespace vehicle_probe
{
	namespace
	{
		constexpr std::uint32_t kGroundLayers = 0x22006;
		struct Probe
		{
			bool pending = false;
			bool valid = false;
			float x = 0, y = 0, z = 0, above = 0, length = 0;
			float height = -3.4e38f;
			std::uint32_t mask = 0;
			unsigned age = 0;
		};
		std::array<Probe, prefetch::kSlotCount> g_probes{};
		std::mutex g_lock;
		// ---- Forward rays ----
		// The query FUN_140583CB0 builds on its stack (1.16.244), laid out as it is there:
		//  filter (hknpBSCustomCollisionFilter, 0x30): +0 vtable, +8 0xFFFFFFFF, +0x10 1, +0x18 (u8) 4, +0x20 layer mask,
		//    +0x28 0x6001E, +0x2C (u8) 0.
		//  query (0x70, right after it): +0 the world's +0x20 object's vfunc +0x18 result, +8 &filter, +0x10 (u16) 0xFFFF,
		//    +0x14 0xFFFFFFFF, +0x18 0, +0x20 (u16) 1, +0x22 (u8) 0xFB, +0x30 from (w 0), +0x40 to - from (w from the
		//    constant at 0x14587A64C), +0x50 1 / that per lane (0 -> the constant at 0x144F065B0), +0x60 0.
		//  collector (hknpAllHitsCollector): +0 vtable, +8 (u8) 0x40, +0xC hit count, +0x10 16 bytes from 0x14587A830,
		//    +0x20 (u8) 0, +0x30 hit array, +0x38 size, +0x3C capacity | 0x80000000 (not owned); hits are 0x70 bytes,
		//    position at +0, query type at +0x68 (1 = ray).
		// CastRay = FUN_1430CBEF0(bsWorld + 0x20, &query, &collector), called without the world lock like the game's own
		// generic FUN_142362760. The world: TES +0xE8 (interior cell) or the exterior cell at the point
		// (FUN_140BBD970(TES +0x188 holder, &cell, pos)); cell +0x48 bit 0 ? FUN_1405105C0(cell +0x50) : *0x1461DF6E8;
		// bsWorld = that +0x50.
		constexpr std::uint32_t kRvaCastRay = 0x030CBEF0, kRvaCellAt = 0x00BBD970, kRvaCellWorld = 0x005105C0, kRvaDefaultWorld = 0x061DF6E8,
			kRvaRelease = 0x00384600, kRvaHitsVtbl = 0x03BC1CF8, kRvaFilterVtbl = 0x04B07680, kRvaDirW = 0x0587A64C, kRvaInvZero = 0x04F065B0,
			kRvaCollectorInit = 0x0587A830;
		struct alignas(16) RayFrame
		{
			std::uint8_t filter[0x30];
			std::uint8_t query[0x70];
			std::uint8_t collector[0x50];
			std::uint8_t hits[0x490];
		};
		struct RayProbe
		{
			bool requested = false;
			float from[3] = {}, to[3] = {};
			std::uint64_t mask = 0;
			bool haveResult = false;
			prefetch::RayHit hit;
			unsigned age = 0;
		};
		RayProbe g_rays[prefetch::kRaySlots];
		bool g_rayFault = false;

		template <class T>
		T& F(std::uint8_t* p, std::size_t o) { return *reinterpret_cast<T*>(p + o); }

		std::uintptr_t RayWorld(std::uintptr_t tes, const float* at)
		{
			std::uintptr_t cell = *reinterpret_cast<std::uintptr_t*>(tes + 0xE8);
			std::uintptr_t found = 0;
			if (!cell) {
				const auto holder = *reinterpret_cast<std::uintptr_t*>(tes + 0x188);
				if (!holder) return 0;
				reinterpret_cast<void (*)(std::uintptr_t, std::uintptr_t*, const float*)>(game::Base() + kRvaCellAt)(holder, &found, at);
				cell = found;
			}
			std::uintptr_t world = 0;
			if (cell) {
				world = (*reinterpret_cast<std::uint8_t*>(cell + 0x48) & 1)
					? reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>(game::Base() + kRvaCellWorld)(*reinterpret_cast<std::uintptr_t*>(cell + 0x50))
					: **reinterpret_cast<std::uintptr_t**>(game::Base() + kRvaDefaultWorld);
			}
			if (found) reinterpret_cast<void (*)(std::uintptr_t)>(game::Base() + kRvaRelease)(found);
			return world ? *reinterpret_cast<std::uintptr_t*>(world + 0x50) : 0;
		}

		// The reference that owns hknp body `id`, the way FUN_14219AE90 does it (fork research, unconfirmed): w = the
		// world's +0x20 object; w vfunc +0x58(id) != 0; body = w vfunc +0x70(id); obj = *(body + 0x98);
		// node = obj vfunc +0x138(); ref = FUN_140B3CF40(*(node + 0x10), 1) (counted: released with FUN_140384600).
		constexpr std::uint32_t kRvaRefFromNode = 0x00B3CF40;
		std::uint32_t BodyOwner(std::uintptr_t w, std::uint32_t id)
		{
			if (!w || id == 0 || id == 0xFFFFFFFF) return 0;
			auto** vt = *reinterpret_cast<void***>(w);
			if (!reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(vt[0x58 / 8])(w, id)) return 0;
			const auto body = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, std::uint32_t)>(vt[0x70 / 8])(w, id);
			const auto obj = body ? *reinterpret_cast<std::uintptr_t*>(body + 0x98) : 0;
			if (!obj) return 0;
			const auto node = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t)>((*reinterpret_cast<void***>(obj))[0x138 / 8])(obj);
			const auto owner = node ? *reinterpret_cast<std::uintptr_t*>(node + 0x10) : 0;
			if (!owner) return 0;
			const auto ref = reinterpret_cast<std::uintptr_t (*)(std::uintptr_t, int)>(game::Base() + kRvaRefFromNode)(owner, 1);
			if (!ref) return 0;
			const std::uint32_t formId = *reinterpret_cast<std::uint32_t*>(ref + 0x28);
			reinterpret_cast<void (*)(std::uintptr_t)>(game::Base() + kRvaRelease)(ref);
			return formId;
		}

		bool CastRayNow(std::uintptr_t tes, const RayProbe& r, prefetch::RayHit& out)
		{
			out = prefetch::RayHit{};
			const std::uintptr_t bs = RayWorld(tes, r.from);
			if (!bs) return false;
			RayFrame fr{};
			const auto base = game::Base();
			F<std::uintptr_t>(fr.filter, 0) = base + kRvaFilterVtbl;
			F<std::uint64_t>(fr.filter, 8) = 0xFFFFFFFFull;
			F<std::uint64_t>(fr.filter, 0x10) = 1;
			F<std::uint8_t>(fr.filter, 0x18) = 4;
			F<std::uint64_t>(fr.filter, 0x20) = r.mask;
			F<std::uint32_t>(fr.filter, 0x28) = 0x6001E;
			const std::uintptr_t inner = *reinterpret_cast<std::uintptr_t*>(bs + 0x20);
			if (!inner) return false;
			F<std::uint64_t>(fr.query, 0) = reinterpret_cast<std::uint64_t (*)(std::uintptr_t)>((*reinterpret_cast<void***>(inner))[3])(inner);
			F<std::uintptr_t>(fr.query, 8) = reinterpret_cast<std::uintptr_t>(fr.filter);
			F<std::uint16_t>(fr.query, 0x10) = 0xFFFF;
			F<std::uint32_t>(fr.query, 0x14) = 0xFFFFFFFF;
			F<std::uint16_t>(fr.query, 0x20) = 1;
			F<std::uint8_t>(fr.query, 0x22) = 0xFB;
			const float dirW = *reinterpret_cast<const float*>(base + kRvaDirW);
			const float* invZero = reinterpret_cast<const float*>(base + kRvaInvZero);
			const float dir[4] = { r.to[0] - r.from[0], r.to[1] - r.from[1], r.to[2] - r.from[2], dirW };
			for (int i = 0; i < 3; ++i) F<float>(fr.query, 0x30 + 4 * i) = r.from[i];
			for (int i = 0; i < 4; ++i) {
				F<float>(fr.query, 0x40 + 4 * i) = dir[i];
				F<float>(fr.query, 0x50 + 4 * i) = dir[i] == 0.0f ? invZero[i] : 1.0f / dir[i];
			}
			F<std::uintptr_t>(fr.collector, 0) = base + kRvaHitsVtbl;
			F<std::uint8_t>(fr.collector, 8) = 0x40;
			std::memcpy(fr.collector + 0x10, reinterpret_cast<const void*>(base + kRvaCollectorInit), 16);
			F<std::uintptr_t>(fr.collector, 0x30) = reinterpret_cast<std::uintptr_t>(fr.hits);
			F<std::uint32_t>(fr.collector, 0x3C) = 0x8000000A;
			reinterpret_cast<void (*)(std::uintptr_t, void*, void*)>(base + kRvaCastRay)(bs + 0x20, fr.query, fr.collector);
			const std::uint32_t n = std::min<std::uint32_t>(std::max(F<std::uint32_t>(fr.collector, 0xC), F<std::uint32_t>(fr.collector, 0x38)), 10);
			const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
			float best = 1e30f;
			for (std::uint32_t i = 0; i < n; ++i) {
				std::uint8_t* h = fr.hits + i * 0x70;
				if (F<std::int32_t>(h, 0x68) != 1) continue;
				const float hx = F<float>(h, 0), hy = F<float>(h, 4), hz = F<float>(h, 8);
				const float d = std::sqrt((hx - r.from[0]) * (hx - r.from[0]) + (hy - r.from[1]) * (hy - r.from[1]) + (hz - r.from[2]) * (hz - r.from[2]));
				if (!(d < best) || d > len + 1.0f) continue;
				best = d;
				out.hit = true;
				out.distance = d;
				out.x = hx; out.y = hy; out.z = hz;
				std::memcpy(out.raw, h, sizeof(out.raw));
				out.bodyId = F<std::uint32_t>(h, 0x48);
				out.layer = F<std::uint32_t>(h, 0x5C) & 0x7F;
			}
			if (out.hit) out.formId = BodyOwner(inner, out.bodyId);
			return true;
		}

		// One ray, faults caught (no C++ objects here: __try needs a plain frame).
		int CastRayGuarded(std::uintptr_t tes, const RayProbe* r, prefetch::RayHit* out)
		{
			__try {
				return CastRayNow(tes, *r, *out) ? 1 : 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {
				return -static_cast<int>(GetExceptionCode() & 0x7FFFFFFF);
			}
		}

		void ServiceRays(std::uintptr_t tes)
		{
			if (g_rayFault) return;
			RayProbe work[prefetch::kRaySlots];
			{
				std::lock_guard lk(g_lock);
				for (int i = 0; i < prefetch::kRaySlots; ++i) {
					work[i] = g_rays[i];
					g_rays[i].requested = false;
					if (g_rays[i].haveResult) ++g_rays[i].age;
				}
			}
			for (int i = 0; i < prefetch::kRaySlots; ++i) {
				if (!work[i].requested) continue;
				prefetch::RayHit hit;
				const int r = CastRayGuarded(tes, &work[i], &hit);
				if (r < 0) {
					g_rayFault = true;
					logger::Write("Prefetch: forward ray faulted (code %d); forward rays off until restart", r);
					return;
				}
				if (r == 0) continue;
				std::lock_guard lk(g_lock);
				g_rays[i].hit = hit;
				g_rays[i].haveResult = true;
				g_rays[i].age = 0;
			}
		}

		using Update = void (*)(std::uintptr_t, std::uintptr_t);
		Update g_original = nullptr;

		void Detour(std::uintptr_t system, std::uintptr_t argument)
		{
			g_original(system, argument);
			const auto base = game::Base();
			const auto tes = *reinterpret_cast<std::uintptr_t*>(base + addr::kTESPtr);
			if (!tes) return;
			ServiceRays(tes);
			const auto ray = reinterpret_cast<float (*)(std::uintptr_t, const float*, std::uint32_t, float, float)>(base + addr::kGroundRayCast);
			for (int i = 0; i < prefetch::kSlotCount; ++i) {
				Probe work;
				{
					std::lock_guard lk(g_lock);
					work = g_probes[i];
					g_probes[i].pending = false;
					if (g_probes[i].valid && g_probes[i].age < 1000) ++g_probes[i].age;
				}
				if (!work.pending) continue;
				const float pos[3] = { work.x, work.y, work.z };
				const float height = ray(tes, pos, work.mask ? work.mask : kGroundLayers, work.above, work.length);
				{
					std::lock_guard lk(g_lock);
					g_probes[i].height = height;
					g_probes[i].valid = true;
					g_probes[i].age = 0;
				}
			}
			const auto player = *reinterpret_cast<std::uintptr_t*>(base + addr::kPlayerCharacterPtr);
			if (player) shipmodel::OnPlayerUpdate(player);
		}
	}

	bool Install()
	{
		static const std::uint8_t prologue[16] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x48, 0x89, 0x70, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70 };
		void* original = nullptr;
		if (!hook::Install(game::Base() + addr::kShipSystemUpdate, prologue, sizeof(prologue), reinterpret_cast<void*>(&Detour), &original)) {
			logger::Write("Vehicle: standalone ground-update hook unavailable");
			return false;
		}
		g_original = reinterpret_cast<Update>(original);
		logger::Write("Vehicle: standalone ground probes installed");
		return true;
	}

	void Request(int slot, float x, float y, float z, float above, float length, std::uint32_t mask)
	{
		if (slot < 0 || slot >= prefetch::kSlotCount) return;
		std::lock_guard lk(g_lock);
		auto& p = g_probes[slot];
		p.pending = true;
		p.x = x; p.y = y; p.z = z; p.above = above; p.length = length; p.mask = mask;
	}

	bool Latest(int slot, float& height, unsigned& age)
	{
		if (slot < 0 || slot >= prefetch::kSlotCount) return false;
		std::lock_guard lk(g_lock);
		const auto& p = g_probes[slot];
		height = p.height;
		age = p.age;
		return p.valid;
	}

	void Reset()
	{
		std::lock_guard lk(g_lock);
		g_probes = {};
		for (auto& r : g_rays) r = RayProbe{};
	}

	void RequestRay(int slot, const float from[3], const float to[3], std::uint64_t mask)
	{
		if (slot < 0 || slot >= prefetch::kRaySlots) return;
		std::lock_guard lk(g_lock);
		auto& r = g_rays[slot];
		r.requested = true;
		for (int i = 0; i < 3; ++i) { r.from[i] = from[i]; r.to[i] = to[i]; }
		r.mask = mask;
	}

	bool LatestRay(int slot, prefetch::RayHit& out, unsigned& age)
	{
		if (slot < 0 || slot >= prefetch::kRaySlots) return false;
		std::lock_guard lk(g_lock);
		const auto& r = g_rays[slot];
		if (!r.haveResult) return false;
		out = r.hit;
		age = r.age;
		return true;
	}
}
