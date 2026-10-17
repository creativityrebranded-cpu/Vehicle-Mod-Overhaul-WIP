#include "actors.h"

#include "addresses.h"
#include "game.h"
#include "log.h"
#include "modscene.h"
#include "telemetry.h"

#include <Windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace actors
{
	namespace
	{
		template <class T>
		T& At(std::uintptr_t p, std::uint32_t o) { return *reinterpret_cast<T*>(p + o); }
		std::uintptr_t Ptr(std::uint32_t rva) { return *reinterpret_cast<std::uintptr_t*>(game::Base() + rva); }

		struct A { std::uint32_t formId; game::Vec3 before; game::Vec3 after; };
		std::unordered_map<std::uintptr_t, A> g_actors;
		std::int32_t g_cx = 0, g_cy = 0;
		ULONGLONG g_checkAt = 0;
		ULONGLONG g_observeAt = 0;

		bool ControllerPos(std::uintptr_t actor, float out[4])
		{
			__try {
				const auto proc = At<std::uintptr_t>(actor, 0x228);
				const auto mid = proc ? At<std::uintptr_t>(proc, 8) : 0;
				const auto ctrl = mid ? At<std::uintptr_t>(mid, 0x458) : 0;
				if (!ctrl) return false;
				auto** vtbl = *reinterpret_cast<void***>(ctrl);
				reinterpret_cast<void (*)(std::uintptr_t, float*, char)>(vtbl[0x248 / 8])(ctrl, out, 1);
				return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		bool ScenePos(std::uintptr_t actor, game::Vec3& out)
		{
			__try {
				const auto loaded = At<std::uintptr_t>(actor, off::kRefLoadedData);
				const auto node = loaded ? At<std::uintptr_t>(loaded, off::kLoadedData3D) : 0;
				if (!node) return false;
				out = At<game::Vec3>(node, off::kNodeWorldT);
				return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
		}

		void ObserveNearby(ULONGLONG now)
		{
			if (now < g_observeAt) return;
			g_observeAt = now + 3000;
			const auto player = Ptr(addr::kPlayerCharacterPtr);
			if (!player) return;
			const auto here = At<game::Vec3>(player, off::kRefPosition);
			std::unordered_set<std::uintptr_t> seen;
			unsigned nearby = 0, controllers = 0, gaps = 0;
			for (const auto r : modscene::LoadedRefs()) {
				if (r == player || !seen.insert(r).second || !modscene::IsActor(r)) continue;
				const auto p = At<game::Vec3>(r, off::kRefPosition);
				if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::hypot(p.x - here.x, p.y - here.y) > 100.0f) continue;
				++nearby;
				alignas(16) float ctrl[4]{};
				const bool have = ControllerPos(r, ctrl);
				game::Vec3 scene{};
				const bool haveScene = ScenePos(r, scene);
				if (have) ++controllers;
				const float gap = have ? std::hypot(ctrl[0] - p.x, ctrl[1] - p.y, ctrl[2] - p.z) : -1.0f;
				const float sceneGap = haveScene ? std::hypot(scene.x - p.x, scene.y - p.y, scene.z - p.z) : -1.0f;
				if (gap > 3.0f) ++gaps;
				if (nearby <= 16) logger::Write("Actors nearby: %08X ref (%.1f, %.1f, %.1f), controller %s(%.1f, %.1f, %.1f) gap %.1f m; scene %s(%.1f, %.1f, %.1f) gap %.1f m",
					At<std::uint32_t>(r, off::kFormId), p.x, p.y, p.z, have ? "" : "missing ", ctrl[0], ctrl[1], ctrl[2], gap,
					haveScene ? "" : "missing ", scene.x, scene.y, scene.z, sceneGap);
			}
			if (nearby || gaps) logger::Write("Actors nearby: %u within 100 m, %u controllers, %u gaps > 3 m", nearby, controllers, gaps);
		}
	}

	void BeforeShift(std::int32_t, std::int32_t, std::uintptr_t)
	{
		g_actors.clear();
		const auto player = Ptr(addr::kPlayerCharacterPtr);
		for (const auto r : modscene::LoadedRefs()) {
			if (r != player && modscene::IsActor(r)) {
				g_actors[r] = A{ At<std::uint32_t>(r, off::kFormId), At<game::Vec3>(r, off::kRefPosition), {} };
			}
		}
	}

	void AfterShift(std::int32_t cx, std::int32_t cy, std::uintptr_t)
	{
		g_cx = cx;
		g_cy = cy;
		for (auto& [r, a] : g_actors) {
			a.after = At<game::Vec3>(r, off::kRefPosition);
		}
		g_checkAt = GetTickCount64() + 3000;
	}

	void Sync(std::uintptr_t)
	{
		const auto now = GetTickCount64();
		ObserveNearby(now);
		if (!g_checkAt || now < g_checkAt) {
			return;
		}
		g_checkAt = 0;
		const float ex = float(g_cx) * off::kCellSize, ey = float(g_cy) * off::kCellSize;
		unsigned ok = 0, backToOld = 0, jumped = 0, gone = 0;
		std::string list;
		std::unordered_map<std::uintptr_t, bool> present;
		for (const auto r : modscene::LoadedRefs()) present[r] = true;
		for (const auto& [r, a] : g_actors) {
			if (!present.count(r) || At<std::uint32_t>(r, off::kFormId) != a.formId) {
				++gone;
				continue;
			}
			const auto p = At<game::Vec3>(r, off::kRefPosition);
			const float sinceShift = std::hypot(p.x - a.after.x, p.y - a.after.y);
			// Heading back to the old frame: now nearer the pre-shift coordinates than where the shift put it.
			const float toOld = std::hypot(p.x - a.before.x, p.y - a.before.y);
			const float toNew = std::hypot(p.x - (a.before.x - ex), p.y - (a.before.y - ey));
			const char* what = nullptr;
			if (toOld < toNew) { ++backToOld; what = "back toward the old frame"; }
			else if (sinceShift > 40.0f) { ++jumped; what = "jumped"; }
			else ++ok;
			if (what && list.size() < 400) {
				char b[128];
				std::snprintf(b, sizeof(b), "%s%08X %s (%s, %.0f m since the shift)", list.empty() ? "" : "; ", a.formId, modscene::RttiName(r), what, sinceShift);
				list += b;
			}
		}
		logger::Write("Actors: shift (%d, %d), 3 s after: %u ok, %u back toward the old frame, %u jumped > 40 m, %u unloaded%s%s", g_cx, g_cy, ok, backToOld, jumped,
			gone, list.empty() ? "" : " - ", list.c_str());
		telemetry::Record("actors", "\"ok\":%u,\"back\":%u,\"jumped\":%u,\"gone\":%u", ok, backToOld, jumped, gone);
	}

	void LoadReset()
	{
		g_actors.clear();
		g_checkAt = 0;
		g_observeAt = 0;
	}
}
